// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/percpu.c - percpu memory allocator
 *
 * Copyright (C) 2009		SUSE Linux Products GmbH
 * Copyright (C) 2009		Tejun Heo <tj@kernel.org>
 *
 * Copyright (C) 2017		Facebook Inc.
 * Copyright (C) 2017		Dennis Zhou <dennis@kernel.org>
 *
 * The percpu allocator handles both static and dynamic areas.  Percpu
 * areas are allocated in chunks which are divided into units.  There is
 * a 1-to-1 mapping for units to possible cpus.  These units are grouped
 * based on NUMA properties of the machine.
 *
 *  c0                           c1                         c2
 *  -------------------          -------------------        ------------
 * | u0 | u1 | u2 | u3 |        | u0 | u1 | u2 | u3 |      | u0 | u1 | u
 *  -------------------  ......  -------------------  ....  ------------
 *
 * Allocation is done by offsets into a unit's address space.  Ie., an
 * area of 512 bytes at 6k in c1 occupies 512 bytes at 6k in c1:u0,
 * c1:u1, c1:u2, etc.  On NUMA machines, the mapping may be non-linear
 * and even sparse.  Access is handled by configuring percpu base
 * registers according to the cpu to unit mappings and offsetting the
 * base address using pcpu_unit_size.
 *
 * There is special consideration for the first chunk which must handle
 * the static percpu variables in the kernel image as allocation services
 * are not online yet.  In short, the first chunk is structured like so:
 *
 *                  <Static | [Reserved] | Dynamic>
 *
 * The static data is copied from the original section managed by the
 * linker.  The reserved section, if non-zero, primarily manages static
 * percpu variables from kernel modules.  Finally, the dynamic section
 * takes care of normal allocations.
 *
 * The allocator organizes chunks into lists according to free size and
 * memcg-awareness.  To make a percpu allocation memcg-aware the __GFP_ACCOUNT
 * flag should be passed.  All memcg-aware allocations are sharing one set
 * of chunks and all unaccounted allocations and allocations performed
 * by processes belonging to the root memory cgroup are using the second set.
 *
 * The allocator tries to allocate from the fullest chunk first. Each chunk
 * is managed by a bitmap with metadata blocks.  The allocation map is updated
 * on every allocation and free to reflect the current state while the boundary
 * map is only updated on allocation.  Each metadata block contains
 * information to help mitigate the need to iterate over large portions
 * of the bitmap.  The reverse mapping from page to chunk is stored in
 * the page's index.  Lastly, units are lazily backed and grow in unison.
 *
 * There is a unique conversion that goes on here between bytes and bits.
 * Each bit represents a fragment of size PCPU_MIN_ALLOC_SIZE.  The chunk
 * tracks the number of pages it is responsible for in nr_pages.  Helper
 * functions are used to convert from between the bytes, bits, and blocks.
 * All hints are managed in bits unless explicitly stated.
 *
 * To use this allocator, arch code should do the following:
 *
 * - define __addr_to_pcpu_ptr() and __pcpu_ptr_to_addr() to translate
 *   regular address to percpu pointer and back if they need to be
 *   different from the default
 *
 * - use pcpu_setup_first_chunk() during percpu area initialization to
 *   setup the first chunk containing the kernel static percpu area
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitmap.h>
#include <linux/cpumask.h>
#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/lcm.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/kmemleak.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/memcontrol.h>

#include <asm/cacheflush.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>
#include <asm/io.h>

#define CREATE_TRACE_POINTS
#include <trace/events/percpu.h>

#include "percpu-internal.h"

/*
 * The slots are sorted by the size of the biggest continuous free area.
 * 1-31 bytes share the same slot.
 */
/*
 * IAMROOT, 2022.02.05:
 * - 32byte 미만의 size는 1개의 slot에서 관리하겠다는 의미이다.
 */
#define PCPU_SLOT_BASE_SHIFT		5
/* chunks in slots below this are subject to being sidelined on failed alloc */

/*
 * IAMROOT, 2022.02.05:
 * - 할당실패를 한 chunk가 PCPU_SLOT_FAIL_THRESHOLD미만이면 0번으로
 *   밀어 넣겠다는것이다.
 *   메모리 파편화가 너무 되있다고 예상이 되어 일반 slot에서 빼버리는 의미.
 */
#define PCPU_SLOT_FAIL_THRESHOLD	3

/*
 * IAMROOT, 2022.02.05:
 * - 전체 empty populate page가 아래범위보다 적거나 크면 balacne work를 수행하겠다는것.
 */
#define PCPU_EMPTY_POP_PAGES_LOW	2
#define PCPU_EMPTY_POP_PAGES_HIGH	4

#ifdef CONFIG_SMP
/* default addr <-> pcpu_ptr mapping, override in asm/percpu.h if necessary */
#ifndef __addr_to_pcpu_ptr

/*
 * IAMROOT, 2022.02.05:
 * __addr_to_pcpu_ptr : addr - (pcpu_base_addr - __per_cpu_start)
 *
 *                      +--------+
 *                      |dynamic |-- <- addr
 *                      |        | ^
 *                      |reserved| |
 *                      |static  | | delta
 * pcpu_base_addr  -> --+--------+ | 
 *                    ^            |
 *                    | +--------+ v 
 *                    | |dynamic |-- <- __addr_to_pcpu_ptr(addr)
 *                    | |        |
 *              delta | |reserved|
 *                    v |static  |
 * __per_cpu_start -> --+--------+
 *
 * cpu마다 똑같은 macro로 addr을 접근하기 위해서 아래 변형식을 사용한다.
 * addr의 __per_cpu_start의 static을 기준으로 변환을 하다가,
 * read/write할때 macro를 사용해 실제 memory 영역을 사용하는 방식이다.
*/
#define __addr_to_pcpu_ptr(addr)					\
	(void __percpu *)((unsigned long)(addr) -			\
			  (unsigned long)pcpu_base_addr	+		\
			  (unsigned long)__per_cpu_start)
#endif
#ifndef __pcpu_ptr_to_addr
#define __pcpu_ptr_to_addr(ptr)						\
	(void __force *)((unsigned long)(ptr) +				\
			 (unsigned long)pcpu_base_addr -		\
			 (unsigned long)__per_cpu_start)
#endif
#else	/* CONFIG_SMP */
/* on UP, it's always identity mapped */
#define __addr_to_pcpu_ptr(addr)	(void __percpu *)(addr)
#define __pcpu_ptr_to_addr(ptr)		(void __force *)(ptr)
#endif	/* CONFIG_SMP */

static int pcpu_unit_pages __ro_after_init;
static int pcpu_unit_size __ro_after_init;
/*
 * IAMROOT, 2022.01.15:
 * - 전체 unit개수. (사용하지 않은 unit도 포함된 수)
 */
static int pcpu_nr_units __ro_after_init;
static int pcpu_atom_size __ro_after_init;
/*
 * IAMROOT, 2022.01.15:
 * - pcpu_to_depopulate_slot + 1로 구해진다.
 *   총 slot 개수
 * - size에 따른 slot번호 변환은 __pcpu_size_to_slot 참고
 *
 * - pcpu_unit_size를 기준으로 slot번호 할당
 * +-------------+-------------------------------------
 * | slot 번호   | slot에 넣어지는 것
 * +-------------+-------------------------------------
 * | slot nr + 3 | pcpu_to_depopulate_slot
 * | slot nr + 2 | pcpu_free_slot
 * | slot nr + 1 | pcpu_sidelined_slot
 * | slot nr     | __pcpu_size_to_slot(pcpu_unit_size)
 * | ...         |
 * | 2           | 16 ~ 31 byte chunk
 * | 1           | 0 ~ 15 byte chunk
 * | 0           | free size가 부족한 chunk
 * +-------------+-------------------------------------
 */
int pcpu_nr_slots __ro_after_init;
/*
 * IAMROOT, 2022.01.15:
 * - pcpu_sidelined_slot + 1의 자리에 위치한다.
 *   pcpu는 빨리 동작해야되므로 미리 free chunk를
 *   준비해놓는 slot
 */
static int pcpu_free_slot __ro_after_init;
/*
 * IAMROOT, 2022.01.15:
 * - 가장 바깥의 slot. unit_size로 구해진다.
 */
int pcpu_sidelined_slot __ro_after_init;
/*
 * IAMROOT, 2022.01.15:
 * - pcpu_free_slot + 1에 위치한다.
 * - isolate한 chunk가 일반 slot에서 분리되어 위치하는 slot.
 */
int pcpu_to_depopulate_slot __ro_after_init;
static size_t pcpu_chunk_struct_size __ro_after_init;

/* cpus with the lowest and highest unit addresses */
/*
 * IAMROOT, 2022.01.15:
 * 아래 변수들 대부분은 pcpu_setup_first_chunk 에서 설정한다.
 *
 * - pcpu_low_unit_cpu, pcpu_high_unit_cpu
 *   제일 낮은 unit주소로부터 unit의 시작위치가 가장 높은것과 낮은것의 cpu번호가 저장된다.
 */
static unsigned int pcpu_low_unit_cpu __ro_after_init;
static unsigned int pcpu_high_unit_cpu __ro_after_init;

/* the address of the first chunk which starts with the kernel static area */
void *pcpu_base_addr __ro_after_init;

/*
 * IAMROOT, 2022.02.05:
 * - cpu에 해당하는 unit 번호를 저장.
 * ex) 0번 cpu가 unit 10개를 가지고 있다면
 * pcpu_unit_map[0] = 0
 * pcpu_unit_map[1] = 10
 */
static const int *pcpu_unit_map __ro_after_init;		/* cpu -> unit */
/*
 * IAMROOT, 2022.01.21:
 * - pcpu_setup_first_chunk 에서 possible cpu개수만큼 초기화된다.
 * - cpu 번호에 해당하는 unit의 offset주소를 저장한다.
 */
const unsigned long *pcpu_unit_offsets __ro_after_init;	/* cpu -> unit offset */

/* group information, used for vm allocation */
static int pcpu_nr_groups __ro_after_init;
/*
 * IAMROOT, 2022.01.21:
 * - pcpu_base_addr부터 각 그룹별 unit start 까지의 offset
 */
static const unsigned long *pcpu_group_offsets __ro_after_init;
static const size_t *pcpu_group_sizes __ro_after_init;

/*
 * The first chunk which always exists.  Note that unlike other
 * chunks, this one can be allocated and mapped in several different
 * ways and thus often doesn't live in the vmalloc area.
 */
/*
 * IAMROOT, 2022.01.15:
 * - dynamic chunk를 관리한다.
 */
struct pcpu_chunk *pcpu_first_chunk __ro_after_init;

/*
 * Optional reserved chunk.  This chunk reserves part of the first
 * chunk and serves it for reserved allocations.  When the reserved
 * region doesn't exist, the following variable is NULL.
 */
/*
 * IAMROOT, 2022.01.15:
 * - reserved chunk를 전용으로 관리한다.
 */
struct pcpu_chunk *pcpu_reserved_chunk __ro_after_init;

DEFINE_SPINLOCK(pcpu_lock);	/* all internal data structures */
static DEFINE_MUTEX(pcpu_alloc_mutex);	/* chunk create/destroy, [de]pop, map ext */

/*
 * IAMROOT, 2022.01.15:
 * - pcpu_nr_slots 개수만큼 생긴다. list header 관리.
 */
struct list_head *pcpu_chunk_lists __ro_after_init; /* chunk list slots */

/* chunks which need their map areas extended, protected by pcpu_lock */
static LIST_HEAD(pcpu_map_extend_chunks);

/*
 * The number of empty populated pages, protected by pcpu_lock.
 * The reserved chunk doesn't contribute to the count.
 */
int pcpu_nr_empty_pop_pages;

/*
 * The number of populated pages in use by the allocator, protected by
 * pcpu_lock.  This number is kept per a unit per chunk (i.e. when a page gets
 * allocated/deallocated, it is allocated/deallocated in all units of a chunk
 * and increments/decrements this count by 1).
 */
static unsigned long pcpu_nr_populated;

/*
 * Balance work is used to populate or destroy chunks asynchronously.  We
 * try to keep the number of populated free pages between
 * PCPU_EMPTY_POP_PAGES_LOW and HIGH for atomic allocations and at most one
 * empty chunk.
 */
static void pcpu_balance_workfn(struct work_struct *work);
static DECLARE_WORK(pcpu_balance_work, pcpu_balance_workfn);
static bool pcpu_async_enabled __read_mostly;

/*
 * IAMROOT, 2022.02.05:
 * - atomic alloc에서 실패를 하게되면 true가 되고, Balance work때
 *   해당 flag를 바라본다.
 */
static bool pcpu_atomic_alloc_failed;


/*
 * IAMROOT, 2022.02.05:
 * - pcpu_balance_work를 동작시킨다.
 *   nr_empty_pop_pages가 적은경우 동작을 할 수 있으며
 *   empty chunk를 미리 준비해준다.
 */
static void pcpu_schedule_balance_work(void)
{
	if (pcpu_async_enabled)
		schedule_work(&pcpu_balance_work);
}

/**
 * pcpu_addr_in_chunk - check if the address is served from this chunk
 * @chunk: chunk of interest
 * @addr: percpu address
 *
 * RETURNS:
 * True if the address is served from this chunk.
 */

/*
 * IAMROOT, 2022.02.05:
 * - chunk가 관리하는 영역이 addr을 포함하는지 확인한다.
 */
static bool pcpu_addr_in_chunk(struct pcpu_chunk *chunk, void *addr)
{
	void *start_addr, *end_addr;

	if (!chunk)
		return false;

	start_addr = chunk->base_addr + chunk->start_offset;
	end_addr = chunk->base_addr + chunk->nr_pages * PAGE_SIZE -
		   chunk->end_offset;

	return addr >= start_addr && addr < end_addr;
}

/*
 * IAMROOT, 2022.01.15:
 * - unit_size to slot
 * - ex) size = 96k 라고 가정.
 *   highbit = 17(bit 16)
 *   max(17 - 5 + 2, 1) = 14
 *
 * --
 * 1) byte단위로 올때 slot 번호
 *
 *  byte |   0 | 16 |  32 |  64 | 128 | 256 | 512 |
 *  slot |   1 |  2 |   3 |   4 |   5 |   6 |   7 |
 *
 *  ex) 17byte면 2번슬롯
 *
 * 2) kb 단위 이상일때 slot 번호(큰 시스템도 200k를 넘진 않는다고한다.)
 *   kb  |   1 |   2 |   4 |   8 |  16 |  32 |  64 | 128 | 256 | 512 |
 *  slot |   8 |   9 |  10 |  11 |  12 |  13 |  14 |  15 |  16 |  17 |
 *
 * ex) 96k => 14번, 32k => 13번
 */
static int __pcpu_size_to_slot(int size)
{
      
/*
 * IAMROOT, 2022.01.22: 
 * 예) 28K = 0b01100000 00000000의 경우 
 *           highbit = fls(28K) = 15
 *           return = 15 - 10 + 2 = 12번 slot
 *
 *     -> 16K <= size < 32K에 속하므로 12번 슬롯
 */
	int highbit = fls(size);	/* size is in bytes */
	return max(highbit - PCPU_SLOT_BASE_SHIFT + 2, 1);
}

static int pcpu_size_to_slot(int size)
{
	if (size == pcpu_unit_size)
		return pcpu_free_slot;
	return __pcpu_size_to_slot(size);
}

/*
 * IAMROOT, 2022.02.05:
 * - 0번 slot은 이 수식으로 계산되지 않고 할당 실패시 해당 chunk가
 *   PCPU_SLOT_FAIL_THRESHOLD 미만의 slot번호거나
 *   size가 없거나 free_bytes가 PCPU_MIN_ALLOC_SIZE미만인것이 위치한다.
 */
static int pcpu_chunk_slot(const struct pcpu_chunk *chunk)
{
	const struct pcpu_block_md *chunk_md = &chunk->chunk_md;

	if (chunk->free_bytes < PCPU_MIN_ALLOC_SIZE ||
	    chunk_md->contig_hint == 0)
		return 0;

	return pcpu_size_to_slot(chunk_md->contig_hint * PCPU_MIN_ALLOC_SIZE);
}


/*
 * IAMROOT, 2022.02.05:
 * - struct page의 index member에 pcpu chunk구조체를 넣어놓는다.
 */
/* set the pointer to a chunk in a page struct */
static void pcpu_set_page_chunk(struct page *page, struct pcpu_chunk *pcpu)
{
	page->index = (unsigned long)pcpu;
}


/*
 * IAMROOT, 2022.02.05:
 * - populate할때 struct page의 index에 pcpu_chunk를 저장했었다.
 *   해당 page에서 pcpu_chunk를 가져온다.
 */
/* obtain pointer to a chunk from a page struct */
static struct pcpu_chunk *pcpu_get_page_chunk(struct page *page)
{
	return (struct pcpu_chunk *)page->index;
}


/*
 * IAMROOT, 2022.02.05:
 * - cpu에 해당하는 unit공간에서 page idx에 해당하는 offset page
 *   위치를 구해온다.
 * - pcpu_unit_pages가 80개라고 가정, cpu = 1, page_idx = 1
 * +---------+
 * |         |
 * |         |
 * | unit 1  |
 * | (cpu 1) |
 * |         | pcpu_unit_map[1] * pcpu_unit_pages + 1 = 81
 * +---------+
 * |         |
 * |         |
 * | unit 0  |
 * | (cpu 0) |
 * |         |
 * +---------+
 */
static int __maybe_unused pcpu_page_idx(unsigned int cpu, int page_idx)
{
	return pcpu_unit_map[cpu] * pcpu_unit_pages + page_idx;
}


/*
 * IAMROOT, 2022.02.05:
 * - cpu의 unit의 page_idx에 해당하는 offset주소를 return한다.
 */
static unsigned long pcpu_unit_page_offset(unsigned int cpu, int page_idx)
{
	return pcpu_unit_offsets[cpu] + (page_idx << PAGE_SHIFT);
}


/*
 * IAMROOT, 2022.02.05:
 * - base_addr로부터의 cpu의 unit page_idx에 해당하는 offset주소를
 *   return 한다.
 */
static unsigned long pcpu_chunk_addr(struct pcpu_chunk *chunk,
				     unsigned int cpu, int page_idx)
{
	return (unsigned long)chunk->base_addr +
	       pcpu_unit_page_offset(cpu, page_idx);
}

/*
 * The following are helper functions to help access bitmaps and convert
 * between bitmap offsets to address offsets.
 */
/*
 * IAMROOT, 2022.01.15:
 * - 해당 index block의 alloc map을 가져온다.
 * - ex) index=2
 * blk   | 0     | 1      | 2      | 3      | ...
 * alloc | 0..15 | 16..31 | 32..47 | 48..63 | ...
 *                          ^
 *  &alloc_map[(1024 * 2) / BITS_PER_LONG(64)] == &alloc_map[32]
 */
static unsigned long *pcpu_index_alloc_map(struct pcpu_chunk *chunk, int index)
{
	return chunk->alloc_map +
	       (index * PCPU_BITMAP_BLOCK_BITS / BITS_PER_LONG);
}

static unsigned long pcpu_off_to_block_index(int off)
{
	return off / PCPU_BITMAP_BLOCK_BITS;
}

static unsigned long pcpu_off_to_block_off(int off)
{
	return off & (PCPU_BITMAP_BLOCK_BITS - 1);
}

static unsigned long pcpu_block_off_to_off(int index, int off)
{
	return index * PCPU_BITMAP_BLOCK_BITS + off;
}

/**
 * pcpu_check_block_hint - check against the contig hint
 * @block: block of interest
 * @bits: size of allocation
 * @align: alignment of area (max PAGE_SIZE)
 *
 * Check to see if the allocation can fit in the block's contig hint.
 * Note, a chunk uses the same hints as a block so this can also check against
 * the chunk's contig hint.
 */
/*
 * IAMROOT, 2022.01.18: 
 * 할당이 블록의 contig_hint에 적합한지(들어갈수있는지) 여부를 보여준다(return).
 * 청크는 블록과 동일한 힌트를 사용하므로 청크의 contig_hint로 확인할 수 있다.
 *
 * align까지 고려해서 contig_hint에 할당이 가능한지 체크한다.
 * 
 * @block: 블럭 단위가 아니라 chunk가 관리하는 hint를 담은 chunk->chunk_md 이다.
 */
static bool pcpu_check_block_hint(struct pcpu_block_md *block, int bits,
				  size_t align)
{
	int bit_off = ALIGN(block->contig_hint_start, align) -
		block->contig_hint_start;

	return bit_off + bits <= block->contig_hint;
}

/*
 * pcpu_next_hint - determine which hint to use
 * @block: block of interest
 * @alloc_bits: size of allocation
 *
 * This determines if we should scan based on the scan_hint or first_free.
 * In general, we want to scan from first_free to fulfill allocations by
 * first fit.  However, if we know a scan_hint at position scan_hint_start
 * cannot fulfill an allocation, we can begin scanning from there knowing
 * the contig_hint will be our fallback.
 */

/*
 * IAMROOT, 2022.01.18: 
 * scan_hint를 기준으로 스캔할지, first_free를 기준으로 스캔할지 결정한다.
 * 일반적으로 우리는 first_free에서 첫 번째 적합(first fit)으로 할당을 
 * 수행하기를 원한다. 그러나 할당할 사이즈가 scan_hint 보다 커서 할당을 
 * 수행할 수 없으면, contig_hint를 대안(fallback)으로 스캔할 것이다.
 *
 * @block: 청크 또는 블럭 단위 모두 사용
 */
static int pcpu_next_hint(struct pcpu_block_md *block, int alloc_bits)
{
	/*
	 * The three conditions below determine if we can skip past the
	 * scan_hint.  First, does the scan hint exist.  Second, is the
	 * contig_hint after the scan_hint (possibly not true iff
	 * contig_hint == scan_hint).  Third, is the allocation request
	 * larger than the scan_hint.
	 */
/*
 * IAMROOT, 2022.01.18: 
 * 아래의 세 가지 조건에 따라 scan_hint를 건너뛰어도(skip) 되는지 결정한다. 
 * - 1st) scan_hint가 존재
 * - 2nd) scan_hint_start < contig_hint_start (IFF contig_hint == scan_hint).
 * - 3rd) 할당 요청이 scan_hint보다 커서 못들어가는 경우.
 *
 * -> 위의 세 가지 조건이 모두 만족하면 scan_hint가 위치하는 다음 위치를 반환.
 *    그게 아니면 일반적인 처음 free 위치를 반환.
 *
 * case a) alloc_bits가 scan_hint 초과 & scan_hint < contig_hint
 *                    |**alloc_bits**| 
 *      +-----------------------------------------------------+
 *      |  |free|     |scan_hint|       |   contig_hint  |    |
 *      +-----------------------------------------------------+
 *                              ^
 *                              +--- return
 *
 * case b) 그 외 (scan_hint가 없거나, 등등..) 
 *      +-----------------------------------------------------+
 *      |  |free|     |scan_hint|       |   contig_hint  |    |
 *      +-----------------------------------------------------+
 *         ^
 *         +--- return
 */
	if (block->scan_hint &&
	    block->contig_hint_start > block->scan_hint_start &&
	    alloc_bits > block->scan_hint)
		return block->scan_hint_start + block->scan_hint;

	return block->first_free;
}

/**
 * pcpu_next_md_free_region - finds the next hint free area
 * @chunk: chunk of interest
 * @bit_off: chunk offset
 * @bits: size of free area
 *
 * Helper function for pcpu_for_each_md_free_region.  It checks
 * block->contig_hint and performs aggregation across blocks to find the
 * next hint.  It modifies bit_off and bits in-place to be consumed in the
 * loop.
 */
static void pcpu_next_md_free_region(struct pcpu_chunk *chunk, int *bit_off,
				     int *bits)
{
	int i = pcpu_off_to_block_index(*bit_off);
	int block_off = pcpu_off_to_block_off(*bit_off);
	struct pcpu_block_md *block;

	*bits = 0;
	for (block = chunk->md_blocks + i; i < pcpu_chunk_nr_blocks(chunk);
	     block++, i++) {
		/* handles contig area across blocks */
		if (*bits) {
			*bits += block->left_free;
			if (block->left_free == PCPU_BITMAP_BLOCK_BITS)
				continue;
			return;
		}

		/*
		 * This checks three things.  First is there a contig_hint to
		 * check.  Second, have we checked this hint before by
		 * comparing the block_off.  Third, is this the same as the
		 * right contig hint.  In the last case, it spills over into
		 * the next block and should be handled by the contig area
		 * across blocks code.
		 */
		*bits = block->contig_hint;
		if (*bits && block->contig_hint_start >= block_off &&
		    *bits + block->contig_hint_start < PCPU_BITMAP_BLOCK_BITS) {
			*bit_off = pcpu_block_off_to_off(i,
					block->contig_hint_start);
			return;
		}
		/* reset to satisfy the second predicate above */
		block_off = 0;

		*bits = block->right_free;
		*bit_off = (i + 1) * PCPU_BITMAP_BLOCK_BITS - block->right_free;
	}
}

/**
 * pcpu_next_fit_region - finds fit areas for a given allocation request
 * @chunk: chunk of interest
 * @alloc_bits: size of allocation
 * @align: alignment of area (max PAGE_SIZE)
 * @bit_off: chunk offset
 * @bits: size of free area
 *
 * Finds the next free region that is viable for use with a given size and
 * alignment.  This only returns if there is a valid area to be used for this
 * allocation.  block->first_free is returned if the allocation request fits
 * within the block to see if the request can be fulfilled prior to the contig
 * hint.
 */
/*
 * IAMROOT, 2022.01.25:
 * chunk의 각 block을 순회하며 free area를 찾는다.
 */
static void pcpu_next_fit_region(struct pcpu_chunk *chunk, int alloc_bits,
				 int align, int *bit_off, int *bits)
{
/*
 * IAMROOT, 2022.01.22: 
 * i:		@bit_off를 사용하여 블럭 인덱스를 가져온다.
 * block_off:	@bit_off를 사용하여 해당 블럭에서의 offset를 가져온다.
 */
	int i = pcpu_off_to_block_index(*bit_off);
	int block_off = pcpu_off_to_block_off(*bit_off);
	struct pcpu_block_md *block;

	*bits = 0;
	for (block = chunk->md_blocks + i; i < pcpu_chunk_nr_blocks(chunk);
	     block++, i++) {
		/* handles contig area across blocks */
/*
 * IAMROOT, 2022.01.22: 
 * 3) 지난 블럭의 가장 우측 free 공간에서 남은 사이즈(bits)를 가져와서
 *    현재 블럭의 가장 좌측 공간에서 할당을 시도한다.
 *
 *    만일 해당 블럭이 통째로 사용되지 않은 블럭인 경우 bits는 누적되고
 *    다음 블럭에서 이어진다.
 */
		if (*bits) {
			*bits += block->left_free;
			if (*bits >= alloc_bits)
				return;
			if (block->left_free == PCPU_BITMAP_BLOCK_BITS)
				continue;
		}

		/* check block->contig_hint */
/*
 * IAMROOT, 2022.01.22: 
 * 1) 해당 블럭의 contig_hint 범위안에서 할당을 시도한다.
 *    반환되는 @bit_off와 @bits는 할당 가능한 위치와 사이즈를 정확히
 *    반환하지 않고, 대략적인 범위를 포함하여 반환한다. 
 *         
 *                                      |alloc_bits|
 *      +-----------------------------------------------------+
 *      |  |free|     |scan_hint|       |   contig_hint  |    |
 *      +-----------------------------------------------------+
 *         ^                    ^                  ^
 *         +-------- or --------+		   |
 *                bit_off 		           |
 *                   ^                             |
 *                   |<----------- bits ---------->|
 */
		*bits = ALIGN(block->contig_hint_start, align) -
			block->contig_hint_start;
		/*
		 * This uses the block offset to determine if this has been
		 * checked in the prior iteration.
		 */
		if (block->contig_hint &&
		    block->contig_hint_start >= block_off &&
		    block->contig_hint >= *bits + alloc_bits) {
			int start = pcpu_next_hint(block, alloc_bits);

			*bits += alloc_bits + block->contig_hint_start -
				 start;
			*bit_off = pcpu_block_off_to_off(i, start);
			return;
		}
		/* reset to satisfy the second predicate above */
/*
 * IAMROOT, 2022.01.22: 
 * 2) 블럭의 가장 우측 free 공간에서 할당을 시도한다.
 */
		block_off = 0;

		*bit_off = ALIGN(PCPU_BITMAP_BLOCK_BITS - block->right_free,
				 align);
		*bits = PCPU_BITMAP_BLOCK_BITS - *bit_off;
		*bit_off = pcpu_block_off_to_off(i, *bit_off);
		if (*bits >= alloc_bits)
			return;
	}

	/* no valid offsets were found - fail condition */
	*bit_off = pcpu_chunk_map_bits(chunk);
}

/*
 * Metadata free area iterators.  These perform aggregation of free areas
 * based on the metadata blocks and return the offset @bit_off and size in
 * bits of the free area @bits.  pcpu_for_each_fit_region only returns when
 * a fit is found for the allocation request.
 */
#define pcpu_for_each_md_free_region(chunk, bit_off, bits)		\
	for (pcpu_next_md_free_region((chunk), &(bit_off), &(bits));	\
	     (bit_off) < pcpu_chunk_map_bits((chunk));			\
	     (bit_off) += (bits) + 1,					\
	     pcpu_next_md_free_region((chunk), &(bit_off), &(bits)))

#define pcpu_for_each_fit_region(chunk, alloc_bits, align, bit_off, bits)     \
	for (pcpu_next_fit_region((chunk), (alloc_bits), (align), &(bit_off), \
				  &(bits));				      \
	     (bit_off) < pcpu_chunk_map_bits((chunk));			      \
	     (bit_off) += (bits),					      \
	     pcpu_next_fit_region((chunk), (alloc_bits), (align), &(bit_off), \
				  &(bits)))

/**
 * pcpu_mem_zalloc - allocate memory
 * @size: bytes to allocate
 * @gfp: allocation flags
 *
 * Allocate @size bytes.  If @size is smaller than PAGE_SIZE,
 * kzalloc() is used; otherwise, the equivalent of vzalloc() is used.
 * This is to facilitate passing through whitelisted flags.  The
 * returned memory is always zeroed.
 *
 * RETURNS:
 * Pointer to the allocated area on success, NULL on failure.
 */
static void *pcpu_mem_zalloc(size_t size, gfp_t gfp)
{
	if (WARN_ON_ONCE(!slab_is_available()))
		return NULL;

	if (size <= PAGE_SIZE)
		return kzalloc(size, gfp);
	else
		return __vmalloc(size, gfp | __GFP_ZERO);
}

/**
 * pcpu_mem_free - free memory
 * @ptr: memory to free
 *
 * Free @ptr.  @ptr should have been allocated using pcpu_mem_zalloc().
 */
static void pcpu_mem_free(void *ptr)
{
	kvfree(ptr);
}

/*
 * IAMROOT, 2022.01.22: 
 * reserved용 chunk를 제외한 모든 chunk들은 슬롯 리스트에 연결되어 있다.
 * 위로 올라간 chunk인 경우(contig size 증가) 리스트의
 * front 쪽에 추가한다.(우선 할당될 예정)
 */
static void __pcpu_chunk_move(struct pcpu_chunk *chunk, int slot,
			      bool move_front)
{
	if (chunk != pcpu_reserved_chunk) {
		if (move_front)
			list_move(&chunk->list, &pcpu_chunk_lists[slot]);
		else
			list_move_tail(&chunk->list, &pcpu_chunk_lists[slot]);
	}
}

static void pcpu_chunk_move(struct pcpu_chunk *chunk, int slot)
{
	__pcpu_chunk_move(chunk, slot, true);
}

/**
 * pcpu_chunk_relocate - put chunk in the appropriate chunk slot
 * @chunk: chunk of interest
 * @oslot: the previous slot it was on
 *
 * This function is called after an allocation or free changed @chunk.
 * New slot according to the changed state is determined and @chunk is
 * moved to the slot.  Note that the reserved chunk is never put on
 * chunk slots.
 *
 * CONTEXT:
 * pcpu_lock.
 */
static void pcpu_chunk_relocate(struct pcpu_chunk *chunk, int oslot)
{
	int nslot = pcpu_chunk_slot(chunk);

	/* leave isolated chunks in-place */
	if (chunk->isolated)
		return;

	if (oslot != nslot)
		__pcpu_chunk_move(chunk, nslot, oslot < nslot);
}


/*
 * IAMROOT, 2022.02.05:
 * - 일반 slot에서 depopulate slot으로 isolate하여 옮긴다.
 */
static void pcpu_isolate_chunk(struct pcpu_chunk *chunk)
{
	lockdep_assert_held(&pcpu_lock);

	if (!chunk->isolated) {
		chunk->isolated = true;
		pcpu_nr_empty_pop_pages -= chunk->nr_empty_pop_pages;
	}
	list_move(&chunk->list, &pcpu_chunk_lists[pcpu_to_depopulate_slot]);
}


/*
 * IAMROOT, 2022.02.05:
 * - chunk가 isolated되있다면 isolated를 풀어버리고
 *   slot에 재할당한다.
 * - system empty popluate를 증가시키게된다.
 */
static void pcpu_reintegrate_chunk(struct pcpu_chunk *chunk)
{
	lockdep_assert_held(&pcpu_lock);

	if (chunk->isolated) {
		chunk->isolated = false;
		pcpu_nr_empty_pop_pages += chunk->nr_empty_pop_pages;
		pcpu_chunk_relocate(chunk, -1);
	}
}

/*
 * pcpu_update_empty_pages - update empty page counters
 * @chunk: chunk of interest
 * @nr: nr of empty pages
 *
 * This is used to keep track of the empty pages now based on the premise
 * a md_block covers a page.  The hint update functions recognize if a block
 * is made full or broken to calculate deltas for keeping track of free pages.
 */

/*
 * IAMROOT, 2022.02.05:
 * - nr_empty_pop_pages를 갱신한다.
 */
static inline void pcpu_update_empty_pages(struct pcpu_chunk *chunk, int nr)
{
	chunk->nr_empty_pop_pages += nr;
	if (chunk != pcpu_reserved_chunk && !chunk->isolated)
		pcpu_nr_empty_pop_pages += nr;
}

/*
 * pcpu_region_overlap - determines if two regions overlap
 * @a: start of first region, inclusive
 * @b: end of first region, exclusive
 * @x: start of second region, inclusive
 * @y: end of second region, exclusive
 *
 * This is used to determine if the hint region [a, b) overlaps with the
 * allocated region [x, y).
 */
/*
 * IAMROOT, 2022.01.15:
 * - (a, b) 와 (x, y)가 겹치는지 확인한다.
 */
static inline bool pcpu_region_overlap(int a, int b, int x, int y)
{
	return (a < y) && (x < b);
}

/**
 * pcpu_block_update - updates a block given a free area
 * @block: block of interest
 * @start: start offset in block
 * @end: end offset in block
 *
 * Updates a block given a known free area.  The region [start, end) is
 * expected to be the entirety of the free area within a block.  Chooses
 * the best starting offset if the contig hints are equal.
 */
/*
 * IAMROOT, 2022.01.17:
 *
 * @block contig_hint, scan_hint, first_free가 저장되는 metadata block
 * @start, end 검색이 된 free area의 start ~ end
 *
 * metablock정보를 업데이트한다. @start ~ @end까지의 범위가 비교되는
 * free area가 되며 해당 정보로 metablock의 contig_hint, scan_hint가
 * udpate된다.
 *
 * scan_hint는 존재하지 않거나 조건에따라
 * contig_hint 와 size가 같을경우 오른쪽에 위치하며(high address) 아니면
 * 왼쪽(low address)에만 위치해야 한다.
 *
 * 결과1) scan_hint가 없는 경우
 * +-----------------------------------------------------------------------+
 * |  |xx|                                |xxxxxxxxxxxxx|                  |
 * +-----------------------------------------------------------------------+
 *    ^                                   ^
 *    first_free                          contig_hint_start
 *
 * 결과2) scan_hint가 존재
 * +-----------------------------------------------------------------------+
 * |  |xx|            |xxxxx|             |xxxxxxxxxxxxx|                  |
 * +-----------------------------------------------------------------------+
 *    ^               ^                   ^
 *    first_free      scan_hint_start     contig_hint_start
 *
 * 결과3) scan_hint가 존재 (contig_hint와 동일한 사이즈)
 * +-----------------------------------------------------------------------+
 * |  |xx|            |xxxxx|             |xxxxx|                          |
 * +-----------------------------------------------------------------------+
 *    ^               ^                   ^
 *    first_free      contig_hint_start   scan_hint_start
 * 이 경우엔 무조건 contig_hint == scan_hint
 * --
 *
 * first_free, scan_hint_start, conitg_hint_start 의 큰 개념은 다음과 같다.
 * (x : free인 memory라고 가정)
 *
 * +-----------------------------------------------------------------------+
 * |  |xx|            |xxxxx|             |xxxxxxxxxxxxx|                  |
 * +-----------------------------------------------------------------------+
 *    ^               ^                   ^
 *    first_free      scan_hint_start     contig_hint_start
 * (ps 이름이 길어져 아래 그림부턴 xxxx_start에서 _start는 생략) 
 *
 * 1) contig_hint가 scan_hint보다 큰 경우 반드시 오른쪽(high address)에 존재한다.
 * 2) scan_hint는 contig_hint 왼쪽에 위치한다.
 * 3) scan_hint의 size는 contig_hint size보다 반드시 작진 않다.
 * 4) 단 scan_hint와 contig_hint size가 같은경우 순서가 바뀔수도 있어보인다.
 *
 * 불변: scan_hint가 contig_hint와 사이즈가 동일시 scan_hint_start는 우측이다.
 * +-----------------------------------------------------------------------+
 * |  |xx|            |xxxxx|             |xxxxx|                          |
 * +-----------------------------------------------------------------------+
 *    ^               ^                   ^
 *    first_free      contig_hint_start   scan_hint_start
 * ---
 * 더 좋은 조건
 * pcpu_block_update에서 갱신을할때나 비교를 할때 더 좋다고 생각하는건
 * 다음과 같다.
 * 1. size가 더 큰게 좋다
 * 2. size가 같다면, 오른쪽에 있는것일수록 더 좋다.
 * 3. 정렬된 주소가 좋다.
 *
 * ---
 *  scan_hint
 *  1) scan_hint는 이전에 contig_hint 였던 영역이다. 그렇지만 
 *  무조건 contg_hint가 갱신된다고 해서 scan_hint가 직전 contig_hint로
 *  갱신되는것은 아니다.
 *  2) scan_hint는 contig_hint의 왼쪽(낮은 주소)에 위치를 주로 하는데
 *  만약 contig_hint보다 오른쪽에 있다면 반드시 contig_hint와 사이즈가 같다.
 *  왼쪽에 위치한다고 해서 contig_hint의 바로 다음 큰사이즈가 아닐수있다.
 *  scan_hint와 contig_hint사이에 scan_hint보다 크고 contig_hint보다 작은
 *  free area가 존재할수있다.
 *  3) scan할때 scan의 시작기준점으로 사용되고 초기화되며,
 *  scan하면서 scan_hint는 다시 갱신된다.
 *
 * - promote(승격)
 * scan_hint -> contig_hint로 되고 scan_hint는 0;
 *
 * ---
 *  contig_hint
 *  1) scan을 할때 기준이 되는는 start와 범위
 *  2) scan시작전 scan_hint가 contig_hint로 승격이 되는 경우가 있다.
 *  3) 만약 contig_hint가 0으로 시작되면 full scan을 의미한다.
 *
 * ---
 */
static void pcpu_block_update(struct pcpu_block_md *block, int start, int end)
{
/*
 * IAMROOT, 2022.01.15:
 * - start ~ end전까지 0인 bit가 되며, contig는 즉 연속적인 free size를 의미한다.
 */
	int contig = end - start;

	block->first_free = min(block->first_free, start);
/*
 * IAMROOT, 2022.01.15:
 * - start가 0이면 처음부터 free임으로 left_free가 contig가 된다.
 */
	if (start == 0)
		block->left_free = contig;
/*
 * IAMROOT, 2022.01.15:
 * - 마지막까지 contig면 right부터 contig이므로 right_free를 갱신한다.
 */
	if (end == block->nr_bits)
		block->right_free = contig;

	if (contig > block->contig_hint) {
		/* promote the old contig_hint to be the new scan_hint */
		if (start > block->contig_hint_start) {
/*
 * IAMROOT, 2022.01.17:
 * before) 
 * +-----------------------------------------------------------------------+
 * |  |xx|            |xxxxx|       |xxxxxxxxx|       |NNNNNNNNNNNNNNNN    |
 * +-----------------------------------------------------------------------+
 *    ^               ^             ^                 ^
 *    first_free      scan_hint     contig_hint       start -> contig_hint
 *                    (없어짐)      ->old contig_hint
 *                                  ->scan_hint
 * after)
 * +-----------------------------------------------------------------------+
 * |  |xx|            |xxxxx|       |xxxxxxxxx|       |NNNNNNNNNNNNNNNN    |
 * +-----------------------------------------------------------------------+
 *    ^                             ^                 ^
 *    first_free                    scan_hint         contig_hint
 *
 * scan_hint < contig_hint인 경우 무조건 위의 경우만이 존재할것이다.
 * (scan_hint < contig_hint이면서 scan_hint가 contig_hint 오른쪽에 있는 경우가
 * 없다.)
 */
			if (block->contig_hint > block->scan_hint) {
				block->scan_hint_start =
					block->contig_hint_start;
				block->scan_hint = block->contig_hint;
/*
 * IAMROOT, 2022.01.17:
 * before)
 * +-----------------------------------------------------------------------+
 * |  |xx|          |xxxxx|           |NNNNNNNNNNNNNNNN     |xxxxx|        |
 * +-----------------------------------------------------------------------+
 *    ^             ^                 ^                     ^           
 *    first_free    contig_hint       start                 scan_hint ->
 *                  ->(없어짐)        -> contig_hint       -> new contig_hint
 *                                                           보다 작은데
 *                                                           오른쪽에 있음
 *                                                           -> 삭제
 * after)                                                               
 * +-----------------------------------------------------------------------+
 * |  |xx|          |xxxxx|           |NNNNNNNNNNNNNNNN     |xxxxx|        |
 * +-----------------------------------------------------------------------+
 *    ^                               ^                                 
 *    first_free                      contig_hint                       
 *
 * else인 경우)
 * before)
 * +-----------------------------------------------------------------------+
 * |  |xx|          |xxxxx|      |xxxxx|     |NNNNNNNNNNNNNNNN             |
 * +-----------------------------------------------------------------------+
 *    ^             ^            ^           ^                  
 *    first_free    contig_hint scan_hint    start              
 *                  ->(없어짐)               -> contig_hint     
 *                                                       
 *                                                       
 * after)                                                               
 * +-----------------------------------------------------------------------+
 * |  |xx|          |xxxxx|      |xxxxx|     |NNNNNNNNNNNNNNNN             |
 * +-----------------------------------------------------------------------+
 *    ^                          ^           ^                  
 *    first_free                 scan_hint   contig_hint              
 *
 */
			} else if (start < block->scan_hint_start) {
				/*
				 * The old contig_hint == scan_hint.  But, the
				 * new contig is larger so hold the invariant
				 * scan_hint_start < contig_hint_start.
				 */
				block->scan_hint = 0;
			}
		} else {
/*
 * IAMROOT, 2022.01.17:
 * ex) contig_hint < contig && start <= contig_hint_start
 * before) 
 *                                start -> new_contig_hint
 *                                v(contig)
 * +-----------------------------------------------------------------------+
 * |                              |NNNNNNNNNNNNNNN|                        |
 * |  |xx|   |x| |x|     |xxxxx|                    |xxxxxxxxx|            |
 * +-----------------------------------------------------------------------+
 *    ^                  ^                         ^              
 *    first_free         scan_hint                  contig_hint   
 *                       (없어짐)                   ->old contig_hint
 *                                                  ->scan_hint
 *                                                  ->new contig_hint의
 *                                                    오른편에 있는데
 *                                                    작으므로(아마도?)
 *                                                    유효하지 않음
 *
 * afeter)
 * +-----------------------------------------------------------------------+
 * |  |xx|   |x| |x|     |xxxxx|  |NNNNNNNNNNNNNNN| |xxxxxxxxx|            |
 * +-----------------------------------------------------------------------+
 *    ^                           ^
 *    first_free                  contig_hint(new) 
 */
			block->scan_hint = 0;
		}
		block->contig_hint_start = start;
		block->contig_hint = contig;
	} else if (contig == block->contig_hint) {
/*
 * IAMROOT, 2022.01.17:
 * size가 같다면 주소가 정렬이 더 잘되있을거 같은걸로 대체한다는것이다.
 * (0개수가 많을수록 더 잘 정렬됬다고 생각한다.).
 */
		if (block->contig_hint_start &&
		    (!start ||
		     __ffs(start) > __ffs(block->contig_hint_start))) {
			/* start has a better alignment so use it */
			block->contig_hint_start = start;
/*
 * IAMROOT, 2022.01.17:
 * - contig가 갱신되었는데 갱신된 contig가 scan_hint보다 왼쪽에 있으면서
 *   size가 더 크다면, 오른쪽에 있는 hint가 더 크거나 같다는 규칙에 위배되므로
 *   삭제한다.
 */
			if (start < block->scan_hint_start &&
			    block->contig_hint > block->scan_hint)
				block->scan_hint = 0;
/*
 * IAMROOT, 2022.01.17:
 * 위 상황이 아니면 (contig가 contig_hint로 될수없는상황) scan_hint와 비교한다.
 * 기존 scan_hint보다 오른쪽에 위치하거나 size가 크면(혹은 scan_hint가 없었다면)
 * scan_hint를 갱신한다.
 * 이 경우 scan_hint == contig_hint가 되면서 scan_hint가 contig_hint보다
 * 오른쪽에 위치할것이다.
 */
		} else if (start > block->scan_hint_start ||
			   block->contig_hint > block->scan_hint) {
			/*
			 * Knowing contig == contig_hint, update the scan_hint
			 * if it is farther than or larger than the current
			 * scan_hint.
			 */
			block->scan_hint_start = start;
			block->scan_hint = contig;
		}
/*
 * IAMROOT, 2022.01.15:
 * - contig_hint보다 현재 contig가 작은 경우.
 *   현재 contig의 왼쪽 위치라면 scan_hint와 비교해서 대체 여부를 판별한다.
 *   기존 scan_hint보다 size가 크거나, size가 같은 경우 더 오른쪽에
 *   있을때에만 대체한다.
 */
	} else {
		/*
		 * The region is smaller than the contig_hint.  So only update
		 * the scan_hint if it is larger than or equal and farther than
		 * the current scan_hint.
		 */
		if ((start < block->contig_hint_start &&
		     (contig > block->scan_hint ||
		      (contig == block->scan_hint &&
		       start > block->scan_hint_start)))) {
			block->scan_hint_start = start;
			block->scan_hint = contig;
		}
	}
}

/*
 * pcpu_block_update_scan - update a block given a free area from a scan
 * @chunk: chunk of interest
 * @bit_off: chunk offset
 * @bits: size of free area
 *
 * Finding the final allocation spot first goes through pcpu_find_block_fit()
 * to find a block that can hold the allocation and then pcpu_alloc_area()
 * where a scan is used.  When allocations require specific alignments,
 * we can inadvertently create holes which will not be seen in the alloc
 * or free paths.
 *
 * This takes a given free area hole and updates a block as it may change the
 * scan_hint.  We need to scan backwards to ensure we don't miss free bits
 * from alignment.
 */

/*
 * IAMROOT, 2022.02.05:
 * - bit_off부터 bits만큼의 clear bits 영역으로 hint update를 시도한다.
 */
static void pcpu_block_update_scan(struct pcpu_chunk *chunk, int bit_off,
				   int bits)
{
	int s_off = pcpu_off_to_block_off(bit_off);
	int e_off = s_off + bits;
	int s_index, l_bit;
	struct pcpu_block_md *block;

	if (e_off > PCPU_BITMAP_BLOCK_BITS)
		return;

	s_index = pcpu_off_to_block_index(bit_off);
	block = chunk->md_blocks + s_index;

	/* scan backwards in case of alignment skipping free bits */
	l_bit = find_last_bit(pcpu_index_alloc_map(chunk, s_index), s_off);
	s_off = (s_off == l_bit) ? 0 : l_bit + 1;

	pcpu_block_update(block, s_off, e_off);
}

/**
 * pcpu_chunk_refresh_hint - updates metadata about a chunk
 * @chunk: chunk of interest
 * @full_scan: if we should scan from the beginning
 *
 * Iterates over the metadata blocks to find the largest contig area.
 * A full scan can be avoided on the allocation path as this is triggered
 * if we broke the contig_hint.  In doing so, the scan_hint will be before
 * the contig_hint or after if the scan_hint == contig_hint.  This cannot
 * be prevented on freeing as we want to find the largest area possibly
 * spanning blocks.
 */
static void pcpu_chunk_refresh_hint(struct pcpu_chunk *chunk, bool full_scan)
{
	struct pcpu_block_md *chunk_md = &chunk->chunk_md;
	int bit_off, bits;

	/* promote scan_hint to contig_hint */
	if (!full_scan && chunk_md->scan_hint) {
		bit_off = chunk_md->scan_hint_start + chunk_md->scan_hint;
		chunk_md->contig_hint_start = chunk_md->scan_hint_start;
		chunk_md->contig_hint = chunk_md->scan_hint;
		chunk_md->scan_hint = 0;
	} else {
		bit_off = chunk_md->first_free;
		chunk_md->contig_hint = 0;
	}

	bits = 0;
	pcpu_for_each_md_free_region(chunk, bit_off, bits)
		pcpu_block_update(chunk_md, bit_off, bit_off + bits);
}

/**
 * pcpu_block_refresh_hint
 * @chunk: chunk of interest
 * @index: index of the metadata block
 *
 * Scans over the block beginning at first_free and updates the block
 * metadata accordingly.
 */
/*
 * IAMROOT, 2022.01.17:
 * - index block의 metadata를 update한다. scan_hint가 있다면 해당영역부터
 * 시작하며, 없다면 full scan을 한다.
 * - 직접 bitmap(alloc_map)을 참조하여 free_area(bit clear)을 순회한다.
 * - 즉 가장큰 연속된 공간을 새로 찾아 갱신한다(refresh)
 *
 * scan_hint & config_hint의 표현
 *       |free|      |free 영역    |      |free 영역      |
 * +------------------------------------------------------------+
 * |     |xxxx|      |<-scan_hint->|      |<-contig_hint->|     |
 * +------------------------------------------------------------+
 *       ^           ^                    ^
 *       |           |            contig_hint_start
 *       |     scan_hint_start
 *   first_free
 */
static void pcpu_block_refresh_hint(struct pcpu_chunk *chunk, int index)
{
	struct pcpu_block_md *block = chunk->md_blocks + index;
	unsigned long *alloc_map = pcpu_index_alloc_map(chunk, index);
	unsigned int rs, re, start;	/* region start, region end */

	/* promote scan_hint to contig_hint */
/*
 * IAMROOT, 2022.01.15:
 * 1. scan_hint가 있는 경우
 *    -> scan_hint를 contig_hint로 promote하고, 
 *       scan_hint 다음 위치부터 시작하는 scan.
 * 2. scan_hint가 없는 경우
 *    -> 기존의 contig_hint도 없애고, 
 *       first_free부터 시작하는 full scan.
 *
 * ex) scan_hint > 0
 * before)
 * (scan_hint와 contig_hint가 사이즈가 같은 경우 scan_hint가 우측)
 * +--------------------------------------------------------------+
 * |  |0000|         | scan_hint |           |contig_hint|        |
 * +--------------------------------------------------------------+
 *
 * after)
 * +--------------------------------------------------------------+
 * |  |0000|         |contig_hint|           |00000000000|        |
 * +--------------------------------------------------------------+
 *                               ^
 *                             start
 *
 * ex) scan_hint == 0
 * +--------------------------------------------------------------+
 * |  |0000|         |00000000|              |contig_hint|        |
 * +--------------------------------------------------------------+
 *
 * after)
 * +--------------------------------------------------------------+
 * |  |0000|         |00000000|              |00000000000|        |
 * +--------------------------------------------------------------+
 *    ^
 *  start
 */
	if (block->scan_hint) {
		start = block->scan_hint_start + block->scan_hint;
		block->contig_hint_start = block->scan_hint_start;
		block->contig_hint = block->scan_hint;
		block->scan_hint = 0;
	} else {
		start = block->first_free;
		block->contig_hint = 0;
	}

/*
 * IAMROOT, 2022.01.17:
 * - left_free를 초기화를 안하는 이유
 *   start가 0 이라면 left_free는 pcpu_block_update에서 갱신될것이다.
 *   start가 0이 아니라면 left_free영역은 범위에 포함이 안된다.
 *
 * - right_free만 초기화를 하는 이유
 *   for문에서 free영역만 범위를 잡아 순회하기때문에
 *   마지막 영역이 할당되었으면 right_free를 안건들고 종료되기 때문이다.
 *
 *   그래서 이 경우 때문에 0로 초기화를 시켜버린다.
 * ex) 마지막 영역이 alloc인 상태
 *                     v PCPU_BITMAP_BLOCK_BITS
 * alloc_map |..10000011 |
 *               ^---^ 여기까지만 순회하고 종료. right_free를 안건듬
 *
 * ex) 마지막 영역이 free인 상태
 *                     v PCPU_BITMAP_BLOCK_BITS
 * alloc_map |..10000000 |
 *               ^-----^ 마지막 bit를 쳤으므로 right_free가 설정될것이다.
 *
 * 즉 일단 right_free = 0으로 만들어야된다.
 */
	block->right_free = 0;

/*
 * IAMROOT, 2022.01.15:
 * - block내에 alloc_map의 bit가 0인부분을(free area) 순회하며
 *   metadata를 update한다.
 */
	/* iterate over free areas and update the contig hints */
	bitmap_for_each_clear_region(alloc_map, rs, re, start,
				     PCPU_BITMAP_BLOCK_BITS)
		pcpu_block_update(block, rs, re);
}

/**
 * pcpu_block_update_hint_alloc - update hint on allocation path
 * @chunk: chunk of interest
 * @bit_off: chunk offset
 * @bits: size of request
 *
 * Updates metadata for the allocation path.  The metadata only has to be
 * refreshed by a full scan iff the chunk's contig hint is broken.  Block level
 * scans are required if the block's contig hint is broken.
 */
/*
 * IAMROOT, 2022.01.15:
 * @bit_off 할당 시작 위치
 * @bits    이미 할당한 size. 1bit당 4byte
 * - hint
 *   free 영역이 가장 큰곳을 의미한다.
 * - start_offset의 경우
 *   bits_off = 0, bits = start_offset의 map 단위
 * - end_offset의 경우
 *   bits_off = last offset - end_offset, end_offset의 map단위
 */
static void pcpu_block_update_hint_alloc(struct pcpu_chunk *chunk, int bit_off,
					 int bits)
{
	struct pcpu_block_md *chunk_md = &chunk->chunk_md;
	int nr_empty_pages = 0;
	struct pcpu_block_md *s_block, *e_block, *block;
	int s_index, e_index;	/* block indexes of the freed allocation */
	int s_off, e_off;	/* block offsets of the freed allocation */

	/*
	 * Calculate per block offsets.
	 * The calculation uses an inclusive range, but the resulting offsets
	 * are [start, end).  e_index always points to the last block in the
	 * range.
	 */
/*
 * IAMROOT, 2022.01.15:
 * - ex) bit_off = 0x450, bits = 0x500
 *   s_index = 0x450 / 0x400 = 0x1 
 *   e_index = (0x450 + 0x500 - 1) / 0x400 = 0x2
 *   s_off = (0x450 & 0x3ff) = 0x50
 *   e_off = (0x450 + 0x500 - 1) & 0x3ff + 1 = 0x150
 *
 *                         v`s_index           v`e_index
 *   blk       | 0       | 1                 | 2                      | 3       |
 *   alloc_map | 0       | x400 x450 x500 .. | x800 x850 x900 x950 .. | xC00 .. | ..
 *                              ^s_off                        ^e_off
 */
	s_index = pcpu_off_to_block_index(bit_off);
	e_index = pcpu_off_to_block_index(bit_off + bits - 1);
	s_off = pcpu_off_to_block_off(bit_off);
	e_off = pcpu_off_to_block_off(bit_off + bits - 1) + 1;

	s_block = chunk->md_blocks + s_index;
	e_block = chunk->md_blocks + e_index;

	/*
	 * Update s_block.
	 * block->first_free must be updated if the allocation takes its place.
	 * If the allocation breaks the contig_hint, a scan is required to
	 * restore this hint.
	 */
/*
 * IAMROOT, 2022.01.15:
 * - s_block이 빈페이지인 경우.
 */
	if (s_block->contig_hint == PCPU_BITMAP_BLOCK_BITS)
		nr_empty_pages++;

/*
 * IAMROOT, 2022.01.15:
 * - 할당시작지점이 s_block의 first_free(처음 빈자리)인 경우와 일치한경우이다.
 *   이 경우 현재 first free 지점에서 bits만큼 할당이 되었으므로
 *   새로운 first_free지점을 찾아서 갱신을 해야된다.
 *
 * ex) bits = 4
 *
 * old)
 *  1111000000
 *      ^first_free == s_off 
 * 
 * new)
 *      .s_off 
 *      v 
 *  1111111100
 *          ^first_free(new)
 */
	if (s_off == s_block->first_free)
		s_block->first_free = find_next_zero_bit(
					pcpu_index_alloc_map(chunk, s_index),
					PCPU_BITMAP_BLOCK_BITS,
					s_off + bits);

/*
 * IAMROOT, 2022.01.15:
 * - free area인 scan_hint과 alloc area가 겹치면 더이상 scan_hint는
 *   유효하지 않으므로 버린다.
 */
	if (pcpu_region_overlap(s_block->scan_hint_start,
				s_block->scan_hint_start + s_block->scan_hint,
				s_off,
				s_off + bits))
		s_block->scan_hint = 0;

/*
 * IAMROOT, 2022.01.15:
 * - free area영역중에 가장 큰영역(contig_hint)과 요청 영역이 겹치면 contig를 갱신해야될것이다.
 */
	if (pcpu_region_overlap(s_block->contig_hint_start,
				s_block->contig_hint_start +
				s_block->contig_hint,
				s_off,
				s_off + bits)) {
		/* block contig hint is broken - scan to fix it */
		if (!s_off)
			s_block->left_free = 0;
		pcpu_block_refresh_hint(chunk, s_index);
	} else {
/*
 * IAMROOT, 2022.01.15:
 * - left_free, right_free를 갱신한다.
 *   s_index == e_index이면 같은 block에서 마지막 right_free가 생길것이므로 계산을하고
 *   한 block을 초과하는경우는 s_block의 right가 전부 채워질것이므로 0이된다.
 */
		/* update left and right contig manually */
		s_block->left_free = min(s_block->left_free, s_off);
		if (s_index == e_index)
			s_block->right_free = min_t(int, s_block->right_free,
					PCPU_BITMAP_BLOCK_BITS - e_off);
		else
			s_block->right_free = 0;
	}

	/*
	 * Update e_block.
	 */
/*
 * IAMROOT, 2022.01.15:
 * - s_block을 처리할때와 비슷한방식으로 e_block을 처리한다.
 */
	if (s_index != e_index) {

/*
 * IAMROOT, 2022.01.15:
 * - end_offset이 없는경우 마지막 block은 전부 미사용일 것이다.
 */
		if (e_block->contig_hint == PCPU_BITMAP_BLOCK_BITS)
			nr_empty_pages++;

		/*
		 * When the allocation is across blocks, the end is along
		 * the left part of the e_block.
		 */
		e_block->first_free = find_next_zero_bit(
				pcpu_index_alloc_map(chunk, e_index),
				PCPU_BITMAP_BLOCK_BITS, e_off);

		if (e_off == PCPU_BITMAP_BLOCK_BITS) {
			/* reset the block */
			e_block++;
		} else {
			if (e_off > e_block->scan_hint_start)
				e_block->scan_hint = 0;

			e_block->left_free = 0;
			if (e_off > e_block->contig_hint_start) {
				/* contig hint is broken - scan to fix it */
				pcpu_block_refresh_hint(chunk, e_index);
			} else {
				e_block->right_free =
					min_t(int, e_block->right_free,
					      PCPU_BITMAP_BLOCK_BITS - e_off);
			}
		}

		/* update in-between md_blocks */
		nr_empty_pages += (e_index - s_index - 1);
		for (block = s_block + 1; block < e_block; block++) {
			block->scan_hint = 0;
			block->contig_hint = 0;
			block->left_free = 0;
			block->right_free = 0;
		}
	}

	if (nr_empty_pages)
		pcpu_update_empty_pages(chunk, -nr_empty_pages);

	if (pcpu_region_overlap(chunk_md->scan_hint_start,
				chunk_md->scan_hint_start +
				chunk_md->scan_hint,
				bit_off,
				bit_off + bits))
		chunk_md->scan_hint = 0;

	/*
	 * The only time a full chunk scan is required is if the chunk
	 * contig hint is broken.  Otherwise, it means a smaller space
	 * was used and therefore the chunk contig hint is still correct.
	 */
	if (pcpu_region_overlap(chunk_md->contig_hint_start,
				chunk_md->contig_hint_start +
				chunk_md->contig_hint,
				bit_off,
				bit_off + bits))
		pcpu_chunk_refresh_hint(chunk, false);
}

/**
 * pcpu_block_update_hint_free - updates the block hints on the free path
 * @chunk: chunk of interest
 * @bit_off: chunk offset
 * @bits: size of request
 *
 * Updates metadata for the allocation path.  This avoids a blind block
 * refresh by making use of the block contig hints.  If this fails, it scans
 * forward and backward to determine the extent of the free area.  This is
 * capped at the boundary of blocks.
 *
 * A chunk update is triggered if a page becomes free, a block becomes free,
 * or the free spans across blocks.  This tradeoff is to minimize iterating
 * over the block metadata to update chunk_md->contig_hint.
 * chunk_md->contig_hint may be off by up to a page, but it will never be more
 * than the available space.  If the contig hint is contained in one block, it
 * will be accurate.
 */
static void pcpu_block_update_hint_free(struct pcpu_chunk *chunk, int bit_off,
					int bits)
{
	int nr_empty_pages = 0;
	struct pcpu_block_md *s_block, *e_block, *block;
	int s_index, e_index;	/* block indexes of the freed allocation */
	int s_off, e_off;	/* block offsets of the freed allocation */
	int start, end;		/* start and end of the whole free area */

	/*
	 * Calculate per block offsets.
	 * The calculation uses an inclusive range, but the resulting offsets
	 * are [start, end).  e_index always points to the last block in the
	 * range.
	 */
	s_index = pcpu_off_to_block_index(bit_off);
	e_index = pcpu_off_to_block_index(bit_off + bits - 1);
	s_off = pcpu_off_to_block_off(bit_off);
	e_off = pcpu_off_to_block_off(bit_off + bits - 1) + 1;

	s_block = chunk->md_blocks + s_index;
	e_block = chunk->md_blocks + e_index;

	/*
	 * Check if the freed area aligns with the block->contig_hint.
	 * If it does, then the scan to find the beginning/end of the
	 * larger free area can be avoided.
	 *
	 * start and end refer to beginning and end of the free area
	 * within each their respective blocks.  This is not necessarily
	 * the entire free area as it may span blocks past the beginning
	 * or end of the block.
	 */
	start = s_off;
	if (s_off == s_block->contig_hint + s_block->contig_hint_start) {
		start = s_block->contig_hint_start;
	} else {
		/*
		 * Scan backwards to find the extent of the free area.
		 * find_last_bit returns the starting bit, so if the start bit
		 * is returned, that means there was no last bit and the
		 * remainder of the chunk is free.
		 */
		int l_bit = find_last_bit(pcpu_index_alloc_map(chunk, s_index),
					  start);
		start = (start == l_bit) ? 0 : l_bit + 1;
	}

	end = e_off;
	if (e_off == e_block->contig_hint_start)
		end = e_block->contig_hint_start + e_block->contig_hint;
	else
		end = find_next_bit(pcpu_index_alloc_map(chunk, e_index),
				    PCPU_BITMAP_BLOCK_BITS, end);

	/* update s_block */
	e_off = (s_index == e_index) ? end : PCPU_BITMAP_BLOCK_BITS;
	if (!start && e_off == PCPU_BITMAP_BLOCK_BITS)
		nr_empty_pages++;
	pcpu_block_update(s_block, start, e_off);

	/* freeing in the same block */
	if (s_index != e_index) {
		/* update e_block */
		if (end == PCPU_BITMAP_BLOCK_BITS)
			nr_empty_pages++;
		pcpu_block_update(e_block, 0, end);

		/* reset md_blocks in the middle */
		nr_empty_pages += (e_index - s_index - 1);
		for (block = s_block + 1; block < e_block; block++) {
			block->first_free = 0;
			block->scan_hint = 0;
			block->contig_hint_start = 0;
			block->contig_hint = PCPU_BITMAP_BLOCK_BITS;
			block->left_free = PCPU_BITMAP_BLOCK_BITS;
			block->right_free = PCPU_BITMAP_BLOCK_BITS;
		}
	}

	if (nr_empty_pages)
		pcpu_update_empty_pages(chunk, nr_empty_pages);

	/*
	 * Refresh chunk metadata when the free makes a block free or spans
	 * across blocks.  The contig_hint may be off by up to a page, but if
	 * the contig_hint is contained in a block, it will be accurate with
	 * the else condition below.
	 */
	if (((end - start) >= PCPU_BITMAP_BLOCK_BITS) || s_index != e_index)
		pcpu_chunk_refresh_hint(chunk, true);
	else
		pcpu_block_update(&chunk->chunk_md,
				  pcpu_block_off_to_off(s_index, start),
				  end);
}

/**
 * pcpu_is_populated - determines if the region is populated
 * @chunk: chunk of interest
 * @bit_off: chunk offset
 * @bits: size of area
 * @next_off: return value for the next offset to start searching
 *
 * For atomic allocations, check if the backing pages are populated.
 *
 * RETURNS:
 * Bool if the backing pages are populated.
 * next_index is to skip over unpopulated blocks in pcpu_find_block_fit.
 */
static bool pcpu_is_populated(struct pcpu_chunk *chunk, int bit_off, int bits,
			      int *next_off)
{
	unsigned int page_start, page_end, rs, re;

	page_start = PFN_DOWN(bit_off * PCPU_MIN_ALLOC_SIZE);
	page_end = PFN_UP((bit_off + bits) * PCPU_MIN_ALLOC_SIZE);

/*
 * IAMROOT, 2022.01.22: 
 * page_end가 rs보다 작은 경우 매핑된 경우라는 것을 알 수 있다.
 *
 *           page_start    page_end
 *                V            v
 *   +--------------------------------------------
 *   |           1111111111111111111|0000000|111
 *   +--------------------------------------------
 *                                   ^       ^
 *                                   rs      re
 *                                        (실패시 이 값으로 next_off 산출)
 */
	rs = page_start;
	bitmap_next_clear_region(chunk->populated, &rs, &re, page_end);
	if (rs >= page_end)
		return true;

	*next_off = re * PAGE_SIZE / PCPU_MIN_ALLOC_SIZE;
	return false;
}

/**
 * pcpu_find_block_fit - finds the block index to start searching
 * @chunk: chunk of interest
 * @alloc_bits: size of request in allocation units
 * @align: alignment of area (max PAGE_SIZE bytes)
 * @pop_only: use populated regions only
 *
 * Given a chunk and an allocation spec, find the offset to begin searching
 * for a free region.  This iterates over the bitmap metadata blocks to
 * find an offset that will be guaranteed to fit the requirements.  It is
 * not quite first fit as if the allocation does not fit in the contig hint
 * of a block or chunk, it is skipped.  This errs on the side of caution
 * to prevent excess iteration.  Poor alignment can cause the allocator to
 * skip over blocks and chunks that have valid free areas.
 *
 * RETURNS:
 * The offset in the bitmap to begin searching.
 * -1 if no offset is found.
 */
/*
 * IAMROOT, 2022.01.25:
 * @chunk에서 @alloc_bits 만큼(@align 고려) 할당할수있는 위치를 구한다.
 * scan hint에 따라가 scan 시작위치가 좁혀질수있어 탐색시간이 짧아질수있다.
 *
 * 1) 해당 chunk에서 가장 큰 free 영역인 contig hint에 alloc이 되는지 검사.
 * 2) scan hint 다음 or fisrt 부터 scan 시작.
 * 3) pop_only에 따라 populate됬는지 bitmap을 검사하면서 실제 시작
 * offset을 찾음.
 * scan 된 영역에따라 scan hint, contig hint가 break될수도있다.
 */
static int pcpu_find_block_fit(struct pcpu_chunk *chunk, int alloc_bits,
			       size_t align, bool pop_only)
{
	struct pcpu_block_md *chunk_md = &chunk->chunk_md;
	int bit_off, bits, next_off;

	/*
	 * This is an optimization to prevent scanning by assuming if the
	 * allocation cannot fit in the global hint, there is memory pressure
	 * and creating a new chunk would happen soon.
	 */
	if (!pcpu_check_block_hint(chunk_md, alloc_bits, align))
		return -1;

/*
 * IAMROOT, 2022.01.22: 
 * bit_off: full scan할 경우 first_free에서 시작, 
 *          그렇지 않은 경우 scan_hint 다음부터 시작
 *
 * ---
 * - pcpu_next_hint를 통해 탐색 영역을 좁힌다.
 * popluate 고려안했을때 다음과 같다.
 *
 * 1) alloc_bits가 chunk_md의 scan hint보다 클경우
 * scan hint end부터 시작해 ~ contig hint start 까지 검색이 될것이다.
 * 그 사이에 alloc_bits가 할당될수있는 free area가 존재하면 거기에 할당되고
 * 아니면 contig hint에 할당되고 contig hint는 break될것이다.
 *
 * 2) alloc_bits가 chunk_md의 scan hint보다 작은 경은 경우
 * first_free ~ scan hint start까지 scan을 할 것이다.
 * 1과 비슷하게 scan hint전까지 alloc이 되는 영역이있으면 거기에 할당되고
 * 아니면 scan hint에 할당되며 scan hint는 break될것이다.
 *
 * populate를 고려한다면, alloc_bits 기준이 아닌 for문에서 구해낸 free area
 * 전체가 populate를 검색하는것이 확인된다.
 */
	bit_off = pcpu_next_hint(chunk_md, alloc_bits);
	bits = 0;
	pcpu_for_each_fit_region(chunk, alloc_bits, align, bit_off, bits) {
		if (!pop_only || pcpu_is_populated(chunk, bit_off, bits,
						   &next_off))
			break;

		bit_off = next_off;
		bits = 0;
	}

	if (bit_off == pcpu_chunk_map_bits(chunk))
		return -1;

	return bit_off;
}

/*
 * pcpu_find_zero_area - modified from bitmap_find_next_zero_area_off()
 * @map: the address to base the search on
 * @size: the bitmap size in bits
 * @start: the bitnumber to start searching at
 * @nr: the number of zeroed bits we're looking for
 * @align_mask: alignment mask for zero area
 * @largest_off: offset of the largest area skipped
 * @largest_bits: size of the largest area skipped
 *
 * The @align_mask should be one less than a power of 2.
 *
 * This is a modified version of bitmap_find_next_zero_area_off() to remember
 * the largest area that was skipped.  This is imperfect, but in general is
 * good enough.  The largest remembered region is the largest failed region
 * seen.  This does not include anything we possibly skipped due to alignment.
 * pcpu_block_update_scan() does scan backwards to try and recover what was
 * lost to alignment.  While this can cause scanning to miss earlier possible
 * free areas, smaller allocations will eventually fill those holes.
 */

/*
 * IAMROOT, 2022.02.05:
 * - start부터 nr만큼의 clear bits영역을 찾는다.
 *   찾는 도중에 nr보다 작은 영역을 skip하는 경우 largest에
 *   저장한다.
 */
static unsigned long pcpu_find_zero_area(unsigned long *map,
					 unsigned long size,
					 unsigned long start,
					 unsigned long nr,
					 unsigned long align_mask,
					 unsigned long *largest_off,
					 unsigned long *largest_bits)
{
	unsigned long index, end, i, area_off, area_bits;
again:

/*
 * IAMROOT, 2022.02.05:
 * - start bit 이후에 clear bit를 찾는다.
 * ex) 11110000000001111
 *         ^index
 */
	index = find_next_zero_bit(map, size, start);

	/* Align allocation */
	index = __ALIGN_MASK(index, align_mask);
	area_off = index;

	end = index + nr;

/*
 * IAMROOT, 2022.02.05:
 * - map의 크기보다 end가 커지면 결국 검색 실패.
 */
	if (end > size)
		return end;

/*
 * IAMROOT, 2022.02.05:
 * - index의후의 set bit를 찾는다.
 * ex) nr보다 작은 범위로 검색된경우
 *        (area_bits)
 *         |-------|   vend
 *     11110000000001111111100000000000000000000111
 *         ^index   ^i      ^index`
 * i < end가 되어 if문으로 진입한다. 검색된 범위(area_bits)는
 * 요구한 nr보다 작은 크기이므로 skip하는데, 이전에 skip했던 영역들중에
 * 제일 큰것을 저장한다. 만약 largest랑 크기 같다면 area_off가 0으로
 * 시작하거나 정렬이 더 잘된것을 우선으로 취급한다.
 *
 * ex) nr보다 큰범위로 검색된경우
 *         | nr  |vend
 *     11110000000001111
 *         ^index   ^i
 *         |-------|
 *          area_bits
 * 검색된 범위 (area_bits)가 nr보다 크므로 검색이 성공되고 시작 index로
 * return 한다.
 */
	i = find_next_bit(map, end, index);
	if (i < end) {
		area_bits = i - area_off;
		/* remember largest unused area with best alignment */
		if (area_bits > *largest_bits ||
		    (area_bits == *largest_bits && *largest_off &&
		     (!area_off || __ffs(area_off) > __ffs(*largest_off)))) {
			*largest_off = area_off;
			*largest_bits = area_bits;
		}

		start = i + 1;
		goto again;
	}
	return index;
}

/**
 * pcpu_alloc_area - allocates an area from a pcpu_chunk
 * @chunk: chunk of interest
 * @alloc_bits: size of request in allocation units
 * @align: alignment of area (max PAGE_SIZE)
 * @start: bit_off to start searching
 *
 * This function takes in a @start offset to begin searching to fit an
 * allocation of @alloc_bits with alignment @align.  It needs to scan
 * the allocation map because if it fits within the block's contig hint,
 * @start will be block->first_free. This is an attempt to fill the
 * allocation prior to breaking the contig hint.  The allocation and
 * boundary maps are updated accordingly if it confirms a valid
 * free area.
 *
 * RETURNS:
 * Allocated addr offset in @chunk on success.
 * -1 if no matching area is found.
 */

/*
 * IAMROOT, 2022.02.05:
 * - chunk에서 alloc_bits만큼의 free area를 찾는다.
 *   찾는데 성공하면 실제 alloc_map, bound_map이 할당되고,
 *   기타 관련 값 및 hint가 update 된다.
 */
static int pcpu_alloc_area(struct pcpu_chunk *chunk, int alloc_bits,
			   size_t align, int start)
{
	struct pcpu_block_md *chunk_md = &chunk->chunk_md;
	size_t align_mask = (align) ? (align - 1) : 0;
	unsigned long area_off = 0, area_bits = 0;
	int bit_off, end, oslot;

	lockdep_assert_held(&pcpu_lock);

	oslot = pcpu_chunk_slot(chunk);

	/*
	 * Search to find a fit.
	 */
	end = min_t(int, start + alloc_bits + PCPU_BITMAP_BLOCK_BITS,
		    pcpu_chunk_map_bits(chunk));
	bit_off = pcpu_find_zero_area(chunk->alloc_map, end, start, alloc_bits,
				      align_mask, &area_off, &area_bits);
	if (bit_off >= end)
		return -1;


/*
 * IAMROOT, 2022.02.05:
 * - skip했던 bits중에 가장 큰영역으로 scan hint update를 시도한다.
 */
	if (area_bits)
		pcpu_block_update_scan(chunk, area_off, area_bits);


/*
 * IAMROOT, 2022.02.05:
 * - alloc_map과 bound_map, free_bytes, first_free을 찾아낸 범위로 update한다.
 */
	/* update alloc map */
	bitmap_set(chunk->alloc_map, bit_off, alloc_bits);

	/* update boundary map */
	set_bit(bit_off, chunk->bound_map);
	bitmap_clear(chunk->bound_map, bit_off + 1, alloc_bits - 1);
	set_bit(bit_off + alloc_bits, chunk->bound_map);

	chunk->free_bytes -= alloc_bits * PCPU_MIN_ALLOC_SIZE;

	/* update first free bit */
	if (bit_off == chunk_md->first_free)
		chunk_md->first_free = find_next_zero_bit(
					chunk->alloc_map,
					pcpu_chunk_map_bits(chunk),
					bit_off + alloc_bits);

	pcpu_block_update_hint_alloc(chunk, bit_off, alloc_bits);

	pcpu_chunk_relocate(chunk, oslot);

	return bit_off * PCPU_MIN_ALLOC_SIZE;
}

/**
 * pcpu_free_area - frees the corresponding offset
 * @chunk: chunk of interest
 * @off: addr offset into chunk
 *
 * This function determines the size of an allocation to free using
 * the boundary bitmap and clears the allocation map.
 *
 * RETURNS:
 * Number of freed bytes.
 */

/*
 * IAMROOT, 2022.02.05:
 * - bound_map을 참고해 free한다.
 */
static int pcpu_free_area(struct pcpu_chunk *chunk, int off)
{
	struct pcpu_block_md *chunk_md = &chunk->chunk_md;
	int bit_off, bits, end, oslot, freed;

	lockdep_assert_held(&pcpu_lock);
	pcpu_stats_area_dealloc(chunk);

	oslot = pcpu_chunk_slot(chunk);

	bit_off = off / PCPU_MIN_ALLOC_SIZE;

	/* find end index */
	end = find_next_bit(chunk->bound_map, pcpu_chunk_map_bits(chunk),
			    bit_off + 1);
	bits = end - bit_off;
	bitmap_clear(chunk->alloc_map, bit_off, bits);

	freed = bits * PCPU_MIN_ALLOC_SIZE;

	/* update metadata */
	chunk->free_bytes += freed;

	/* update first free bit */
	chunk_md->first_free = min(chunk_md->first_free, bit_off);

	pcpu_block_update_hint_free(chunk, bit_off, bits);

	pcpu_chunk_relocate(chunk, oslot);

	return freed;
}

static void pcpu_init_md_block(struct pcpu_block_md *block, int nr_bits)
{
	block->scan_hint = 0;
	block->contig_hint = nr_bits;
	block->left_free = nr_bits;
	block->right_free = nr_bits;
	block->first_free = 0;
	block->nr_bits = nr_bits;
}

/*
 * IAMROOT, 2022.01.15:
 * - chunk_md와 각각의 md_blocks를 초기화한다.
 */
static void pcpu_init_md_blocks(struct pcpu_chunk *chunk)
{
	struct pcpu_block_md *md_block;

	/* init the chunk's block */
	pcpu_init_md_block(&chunk->chunk_md, pcpu_chunk_map_bits(chunk));

	for (md_block = chunk->md_blocks;
	     md_block != chunk->md_blocks + pcpu_chunk_nr_blocks(chunk);
	     md_block++)
		pcpu_init_md_block(md_block, PCPU_BITMAP_BLOCK_BITS);
}

/**
 * pcpu_alloc_first_chunk - creates chunks that serve the first chunk
 * @tmp_addr: the start of the region served
 * @map_size: size of the region served
 *
 * This is responsible for creating the chunks that serve the first chunk.  The
 * base_addr is page aligned down of @tmp_addr while the region end is page
 * aligned up.  Offsets are kept track of to determine the region served. All
 * this is done to appease the bitmap allocator in avoiding partial blocks.
 *
 * RETURNS:
 * Chunk serving the region at @tmp_addr of @map_size.
 */
/*
 * IAMROOT, 2022.01.15:
 * 1st reserved or dynamic chunk를 관리하는 구조체를 할당하고 초기화한다.
 * @tmp_addr 해당 영역의 시작 주소
 * @map_size 해당 영역의 size
 *
 * ---
 *
 * region : start_offset, end_offset이 포함된 전체 영역
 * populate : region을 page_size 단위로 관리하는 bitmap
 * alloc_map : region을 PCPU_MIN_ALLOC_SIZE 단위로 관리하는 bitmap
 * bound_map : alloc_map의 경계를 표시하기위해 사용하는 bitmap
 * md_blocks : alloc_map을 PCPU_BITMAP_BLOCK_BITS단위로 관리하는 배열.
 * ---
 *
 *   |<-----------------region_size------------------------>|
 *   |                                                      |
 *   +------------------------------------------------------+
 *   |  start_offset |          map_size       | end_offset |
 *   |111111111111111|0000000000000000000000000|111111111111|  <- alloc_map
 *   |100000000000000|1000000000000000000000000|100000000000|1 <- bound_map 
 *   +------------------------------------------------------+
 *   ^               ^
 * aligned_addr      +---tmp_addr
 * chunk->base_addr
 *
 */
static struct pcpu_chunk * __init pcpu_alloc_first_chunk(unsigned long tmp_addr,
							 int map_size)
{
	struct pcpu_chunk *chunk;
	unsigned long aligned_addr, lcm_align;
	int start_offset, offset_bits, region_size, region_bits;
	size_t alloc_size;

	/* region calculations */
	aligned_addr = tmp_addr & PAGE_MASK;

	start_offset = tmp_addr - aligned_addr;

	/*
	 * Align the end of the region with the LCM of PAGE_SIZE and
	 * PCPU_BITMAP_BLOCK_SIZE.  One of these constants is a multiple of
	 * the other.
	 */
	lcm_align = lcm(PAGE_SIZE, PCPU_BITMAP_BLOCK_SIZE);
	region_size = ALIGN(start_offset + map_size, lcm_align);

	/* allocate chunk */
	alloc_size = struct_size(chunk, populated,
				 BITS_TO_LONGS(region_size >> PAGE_SHIFT));
	chunk = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!chunk)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

	INIT_LIST_HEAD(&chunk->list);

	chunk->base_addr = (void *)aligned_addr;
	chunk->start_offset = start_offset;
	chunk->end_offset = region_size - chunk->start_offset - map_size;

/*
 * IAMROOT, 2022.01.22: 
 * alloc_map: region_size를 4바이트 단위로 관리하는 할당관리하는 비트맵
 */

	chunk->nr_pages = region_size >> PAGE_SHIFT;
	region_bits = pcpu_chunk_map_bits(chunk);

	alloc_size = BITS_TO_LONGS(region_bits) * sizeof(chunk->alloc_map[0]);
	chunk->alloc_map = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!chunk->alloc_map)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

	alloc_size =
		BITS_TO_LONGS(region_bits + 1) * sizeof(chunk->bound_map[0]);
	chunk->bound_map = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!chunk->bound_map)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

/*
 * IAMROOT, 2022.01.22: 
 * arm64에서 pcpu 블럭사이즈는 페이지 단위와 동일하다.
 * 따라서 pcpu_block_md 구조체를 블럭(페이지) 수만큼 할당한다.
 */
	alloc_size = pcpu_chunk_nr_blocks(chunk) * sizeof(chunk->md_blocks[0]);
	chunk->md_blocks = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!chunk->md_blocks)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

#ifdef CONFIG_MEMCG_KMEM
	/* first chunk is free to use */
	chunk->obj_cgroups = NULL;
#endif
	pcpu_init_md_blocks(chunk);

/*
 * IAMROOT, 2022.01.22: 
 * embed 방식의 1st chunk 영역은 이미 lowmem 할당이 되었고, 매핑되어 
 * 실제로 곧바로 사용할 수 있는 영역이다. 
 * 따라서 imuutable(불변성)을 true로 설정하고, populated 비트들은 1로 fill한다.
 * 2dn chunk 부터는 매핑하여 사용중인 페이지들만 populate 비트를 설정한다.
 */
	/* manage populated page bitmap */
	chunk->immutable = true;
	bitmap_fill(chunk->populated, chunk->nr_pages);
	chunk->nr_populated = chunk->nr_pages;
	chunk->nr_empty_pop_pages = chunk->nr_pages;

	chunk->free_bytes = map_size;

/*
 * IAMROOT, 2022.01.15:
 * - start_offset과 end_offset의 범위는 사용을 안할것이므로 아에 bits을 set한다.
 * - ex) region memory map
 *
 *           |                   region_size                     |
 *           | start_offset |   map_size            | end_offset |
 * alloc_map | 111.....1111 | 0000000....0000000000 | 1111...111 |
 * bound_map | 100.....0000 | 1000000....0000000000 | 100....000 | 1
 *                            ^first_free
 */
	if (chunk->start_offset) {
		/* hide the beginning of the bitmap */
		offset_bits = chunk->start_offset / PCPU_MIN_ALLOC_SIZE;
		bitmap_set(chunk->alloc_map, 0, offset_bits);
		set_bit(0, chunk->bound_map);
		set_bit(offset_bits, chunk->bound_map);

		chunk->chunk_md.first_free = offset_bits;

		pcpu_block_update_hint_alloc(chunk, 0, offset_bits);
	}

	if (chunk->end_offset) {
		/* hide the end of the bitmap */
		offset_bits = chunk->end_offset / PCPU_MIN_ALLOC_SIZE;
		bitmap_set(chunk->alloc_map,
			   pcpu_chunk_map_bits(chunk) - offset_bits,
			   offset_bits);
		set_bit((start_offset + map_size) / PCPU_MIN_ALLOC_SIZE,
			chunk->bound_map);
		set_bit(region_bits, chunk->bound_map);

		pcpu_block_update_hint_alloc(chunk, pcpu_chunk_map_bits(chunk)
					     - offset_bits, offset_bits);
	}

	return chunk;
}


/*
 * IAMROOT, 2022.02.05:
 * - chunk 생성.
 */
static struct pcpu_chunk *pcpu_alloc_chunk(gfp_t gfp)
{
	struct pcpu_chunk *chunk;
	int region_bits;

	chunk = pcpu_mem_zalloc(pcpu_chunk_struct_size, gfp);
	if (!chunk)
		return NULL;

	INIT_LIST_HEAD(&chunk->list);
	chunk->nr_pages = pcpu_unit_pages;
	region_bits = pcpu_chunk_map_bits(chunk);

	chunk->alloc_map = pcpu_mem_zalloc(BITS_TO_LONGS(region_bits) *
					   sizeof(chunk->alloc_map[0]), gfp);
	if (!chunk->alloc_map)
		goto alloc_map_fail;

	chunk->bound_map = pcpu_mem_zalloc(BITS_TO_LONGS(region_bits + 1) *
					   sizeof(chunk->bound_map[0]), gfp);
	if (!chunk->bound_map)
		goto bound_map_fail;

	chunk->md_blocks = pcpu_mem_zalloc(pcpu_chunk_nr_blocks(chunk) *
					   sizeof(chunk->md_blocks[0]), gfp);
	if (!chunk->md_blocks)
		goto md_blocks_fail;

#ifdef CONFIG_MEMCG_KMEM
	if (!mem_cgroup_kmem_disabled()) {
		chunk->obj_cgroups =
			pcpu_mem_zalloc(pcpu_chunk_map_bits(chunk) *
					sizeof(struct obj_cgroup *), gfp);
		if (!chunk->obj_cgroups)
			goto objcg_fail;
	}
#endif

	pcpu_init_md_blocks(chunk);

	/* init metadata */
	chunk->free_bytes = chunk->nr_pages * PAGE_SIZE;

	return chunk;

#ifdef CONFIG_MEMCG_KMEM
objcg_fail:
	pcpu_mem_free(chunk->md_blocks);
#endif
md_blocks_fail:
	pcpu_mem_free(chunk->bound_map);
bound_map_fail:
	pcpu_mem_free(chunk->alloc_map);
alloc_map_fail:
	pcpu_mem_free(chunk);

	return NULL;
}

/*
 * IAMROOT, 2022.02.05:
 * - chunk를 관리하는 memory를 전부 free한다.
 */
static void pcpu_free_chunk(struct pcpu_chunk *chunk)
{
	if (!chunk)
		return;
#ifdef CONFIG_MEMCG_KMEM
	pcpu_mem_free(chunk->obj_cgroups);
#endif
	pcpu_mem_free(chunk->md_blocks);
	pcpu_mem_free(chunk->bound_map);
	pcpu_mem_free(chunk->alloc_map);
	pcpu_mem_free(chunk);
}

/**
 * pcpu_chunk_populated - post-population bookkeeping
 * @chunk: pcpu_chunk which got populated
 * @page_start: the start page
 * @page_end: the end page
 *
 * Pages in [@page_start,@page_end) have been populated to @chunk.  Update
 * the bookkeeping information accordingly.  Must be called after each
 * successful population.
 */

/*
 * IAMROOT, 2022.02.05:
 * - populate가 된 page 영역을 chunk에 기록한다.
 */
static void pcpu_chunk_populated(struct pcpu_chunk *chunk, int page_start,
				 int page_end)
{
	int nr = page_end - page_start;

	lockdep_assert_held(&pcpu_lock);

	bitmap_set(chunk->populated, page_start, nr);
	chunk->nr_populated += nr;
	pcpu_nr_populated += nr;

	pcpu_update_empty_pages(chunk, nr);
}

/**
 * pcpu_chunk_depopulated - post-depopulation bookkeeping
 * @chunk: pcpu_chunk which got depopulated
 * @page_start: the start page
 * @page_end: the end page
 *
 * Pages in [@page_start,@page_end) have been depopulated from @chunk.
 * Update the bookkeeping information accordingly.  Must be called after
 * each successful depopulation.
 */

/*
 * IAMROOT, 2022.02.05:
 * - depopulate 시킨다. populate page가 아에 없어지는것이므로
 *   pcpu_nr_populated, pcpu_nr_empty_pop_pages 둘다 감소시키는게 보인다.
 */
static void pcpu_chunk_depopulated(struct pcpu_chunk *chunk,
				   int page_start, int page_end)
{
	int nr = page_end - page_start;

	lockdep_assert_held(&pcpu_lock);

	bitmap_clear(chunk->populated, page_start, nr);
	chunk->nr_populated -= nr;
	pcpu_nr_populated -= nr;

	pcpu_update_empty_pages(chunk, -nr);
}

/*
 * Chunk management implementation.
 *
 * To allow different implementations, chunk alloc/free and
 * [de]population are implemented in a separate file which is pulled
 * into this file and compiled together.  The following functions
 * should be implemented.
 *
 * pcpu_populate_chunk		- populate the specified range of a chunk
 * pcpu_depopulate_chunk	- depopulate the specified range of a chunk
 * pcpu_post_unmap_tlb_flush	- flush tlb for the specified range of a chunk
 * pcpu_create_chunk		- create a new chunk
 * pcpu_destroy_chunk		- destroy a chunk, always preceded by full depop
 * pcpu_addr_to_page		- translate address to physical address
 * pcpu_verify_alloc_info	- check alloc_info is acceptable during init
 */
static int pcpu_populate_chunk(struct pcpu_chunk *chunk,
			       int page_start, int page_end, gfp_t gfp);
static void pcpu_depopulate_chunk(struct pcpu_chunk *chunk,
				  int page_start, int page_end);
static void pcpu_post_unmap_tlb_flush(struct pcpu_chunk *chunk,
				      int page_start, int page_end);
static struct pcpu_chunk *pcpu_create_chunk(gfp_t gfp);
static void pcpu_destroy_chunk(struct pcpu_chunk *chunk);
static struct page *pcpu_addr_to_page(void *addr);
static int __init pcpu_verify_alloc_info(const struct pcpu_alloc_info *ai);

#ifdef CONFIG_NEED_PER_CPU_KM
#include "percpu-km.c"
#else
#include "percpu-vm.c"
#endif

/**
 * pcpu_chunk_addr_search - determine chunk containing specified address
 * @addr: address for which the chunk needs to be determined.
 *
 * This is an internal function that handles all but static allocations.
 * Static percpu address values should never be passed into the allocator.
 *
 * RETURNS:
 * The address of the found chunk.
 */

/*
 * IAMROOT, 2022.02.05:
 * - addr을 관리하는 pcpu_chunk를 구해온다.
 */
static struct pcpu_chunk *pcpu_chunk_addr_search(void *addr)
{
	/* is it in the dynamic region (first chunk)? */
	if (pcpu_addr_in_chunk(pcpu_first_chunk, addr))
		return pcpu_first_chunk;

	/* is it in the reserved region? */
	if (pcpu_addr_in_chunk(pcpu_reserved_chunk, addr))
		return pcpu_reserved_chunk;

	/*
	 * The address is relative to unit0 which might be unused and
	 * thus unmapped.  Offset the address to the unit space of the
	 * current processor before looking it up in the vmalloc
	 * space.  Note that any possible cpu id can be used here, so
	 * there's no need to worry about preemption or cpu hotplug.
	 */
	addr += pcpu_unit_offsets[raw_smp_processor_id()];
	return pcpu_get_page_chunk(pcpu_addr_to_page(addr));
}

#ifdef CONFIG_MEMCG_KMEM

/*
 * IAMROOT, 2022.01.23:
 * - TODO
 * - cgroup의 memory controller의 제한을 받는지 확인하고 요청된 size를
 *   cgroup에 더하고 objcgp를 얻어온다.
 * - pcpu_memcg_post_alloc_hook과 한쌍이된다.
 */
static bool pcpu_memcg_pre_alloc_hook(size_t size, gfp_t gfp,
				      struct obj_cgroup **objcgp)
{
	struct obj_cgroup *objcg;

	if (!memcg_kmem_enabled() || !(gfp & __GFP_ACCOUNT))
		return true;

	objcg = get_obj_cgroup_from_current();
	if (!objcg)
		return true;

	if (obj_cgroup_charge(objcg, gfp, size * num_possible_cpus())) {
		obj_cgroup_put(objcg);
		return false;
	}

	*objcgp = objcg;
	return true;
}


/*
 * IAMROOT, 2022.01.23:
 * - TODO
 * - @chunk가 null일 경우 실패했다는 의미로 cgroup에 넣었던 size를
 *   다시 뺀다.
 * - @chunk가 null이 아닐경우 할당에 성공했다는거고 @chunk의 obj_cgroups에
 *   @objcg를 넣고 mod_memcg_state를 수행한다.
 */
static void pcpu_memcg_post_alloc_hook(struct obj_cgroup *objcg,
				       struct pcpu_chunk *chunk, int off,
				       size_t size)
{
	if (!objcg)
		return;

	if (likely(chunk && chunk->obj_cgroups)) {
		chunk->obj_cgroups[off >> PCPU_MIN_ALLOC_SHIFT] = objcg;

		rcu_read_lock();
		mod_memcg_state(obj_cgroup_memcg(objcg), MEMCG_PERCPU_B,
				size * num_possible_cpus());
		rcu_read_unlock();
	} else {
		obj_cgroup_uncharge(objcg, size * num_possible_cpus());
		obj_cgroup_put(objcg);
	}
}

static void pcpu_memcg_free_hook(struct pcpu_chunk *chunk, int off, size_t size)
{
	struct obj_cgroup *objcg;

	if (unlikely(!chunk->obj_cgroups))
		return;

	objcg = chunk->obj_cgroups[off >> PCPU_MIN_ALLOC_SHIFT];
	if (!objcg)
		return;
	chunk->obj_cgroups[off >> PCPU_MIN_ALLOC_SHIFT] = NULL;

	obj_cgroup_uncharge(objcg, size * num_possible_cpus());

	rcu_read_lock();
	mod_memcg_state(obj_cgroup_memcg(objcg), MEMCG_PERCPU_B,
			-(size * num_possible_cpus()));
	rcu_read_unlock();

	obj_cgroup_put(objcg);
}

#else /* CONFIG_MEMCG_KMEM */
static bool
pcpu_memcg_pre_alloc_hook(size_t size, gfp_t gfp, struct obj_cgroup **objcgp)
{
	return true;
}

static void pcpu_memcg_post_alloc_hook(struct obj_cgroup *objcg,
				       struct pcpu_chunk *chunk, int off,
				       size_t size)
{
}

static void pcpu_memcg_free_hook(struct pcpu_chunk *chunk, int off, size_t size)
{
}
#endif /* CONFIG_MEMCG_KMEM */

/**
 * pcpu_alloc - the percpu allocator
 * @size: size of area to allocate in bytes
 * @align: alignment of area (max PAGE_SIZE)
 * @reserved: allocate from the reserved chunk if available
 * @gfp: allocation flags
 *
 * Allocate percpu area of @size bytes aligned at @align.  If @gfp doesn't
 * contain %GFP_KERNEL, the allocation is atomic. If @gfp has __GFP_NOWARN
 * then no warning will be triggered on invalid or failed allocation
 * requests.
 *
 * RETURNS:
 * Percpu pointer to the allocated area on success, NULL on failure.
 */
static void __percpu *pcpu_alloc(size_t size, size_t align, bool reserved,
				 gfp_t gfp)
{
	gfp_t pcpu_gfp;
	bool is_atomic;
	bool do_warn;
	struct obj_cgroup *objcg = NULL;
	static int warn_limit = 10;
	struct pcpu_chunk *chunk, *next;
	const char *err;
	int slot, off, cpu, ret;
	unsigned long flags;
	void __percpu *ptr;
	size_t bits, bit_align;

/*
 * IAMROOT, 2022.01.23:
 * - 현재 context에 따라 요청 gfp를 한번 수정한후 pcpu_gfp를 추출한다.
 *   retry 불가(__GFP_NORETRY), 경고메세지 출력해제(__GFP_NOWARN),
 *   일반적인 flags(GFP_KERNEL)만을 사용하는게 보인다.
 *
 * - GFP_KERNEL은 interrupt context에서 해당 api를 호출했는지 안했는지
 *   에 대한 의미도 포함한다.
 */
	gfp = current_gfp_context(gfp);
	/* whitelisted flags that can be passed to the backing allocators */
	pcpu_gfp = gfp & (GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);
	is_atomic = (gfp & GFP_KERNEL) != GFP_KERNEL;
	do_warn = !(gfp & __GFP_NOWARN);

	/*
	 * There is now a minimum allocation size of PCPU_MIN_ALLOC_SIZE,
	 * therefore alignment must be a minimum of that many bytes.
	 * An allocation may have internal fragmentation from rounding up
	 * of up to PCPU_MIN_ALLOC_SIZE - 1 bytes.
	 */
/*
 * IAMROOT, 2022.01.22: 
 * @align 요청이 4바이트 보다 작은 경우에는 4바이트로 결정한다.
 * @size 요청은 항상 4바이트 단위로 자동 정렬하여 사용한다.
 */
	if (unlikely(align < PCPU_MIN_ALLOC_SIZE))
		align = PCPU_MIN_ALLOC_SIZE;

	size = ALIGN(size, PCPU_MIN_ALLOC_SIZE);
	bits = size >> PCPU_MIN_ALLOC_SHIFT;
	bit_align = align >> PCPU_MIN_ALLOC_SHIFT;

/*
 * IAMROOT, 2022.01.22: 
 * 다음과 같은 요청을 경고 메시지를 출력하고, null을 반환한다.
 *   - size가 0으로 요청
 *   - size가 유닛 크기보다 큰 경우
 *   - align이 페이지 사이즈를 초과하는 경우(예: 4K)
 *   - align이 2의 승수 요청이 아닌 경우
 */
	if (unlikely(!size || size > PCPU_MIN_UNIT_SIZE || align > PAGE_SIZE ||
		     !is_power_of_2(align))) {
		WARN(do_warn, "illegal size (%zu) or align (%zu) for percpu allocation\n",
		     size, align);
		return NULL;
	}

/*
 * IAMROOT, 2022.01.22: 
 * cgroup의 memory controller의 제한을 받아 초과한 경우 null을 반환한다.
 */
	if (unlikely(!pcpu_memcg_pre_alloc_hook(size, gfp, &objcg)))
		return NULL;

/*
 * IAMROOT, 2022.01.22: 
 * 일반적인 커널 메모리 할당(is_atomic==0)을 사용하는 경우
 * pcpu_alloc_mutex를 잡고 진입한다. 단 __GFP_NOFAIL 옵션을 사용하지 않는 
 * 경우 killable(at fatal signal)이 가능한 lock 함수를 사용한다.
 */
	if (!is_atomic) {
		/*
		 * pcpu_balance_workfn() allocates memory under this mutex,
		 * and it may wait for memory reclaim. Allow current task
		 * to become OOM victim, in case of memory pressure.
		 */
		if (gfp & __GFP_NOFAIL) {
			mutex_lock(&pcpu_alloc_mutex);
		} else if (mutex_lock_killable(&pcpu_alloc_mutex)) {
			pcpu_memcg_post_alloc_hook(objcg, NULL, 0, size);
			return NULL;
		}
	}

	spin_lock_irqsave(&pcpu_lock, flags);

/*
 * IAMROOT, 2022.01.22: 
 * module(insmod)이 로딩될 때 DEFINE_PER_CPU() 함수를 사용한 영역들이 
 * reserved 청크에서 할당되고 관리된다.
 */
	/* serve reserved allocations from the reserved chunk if available */
	if (reserved && pcpu_reserved_chunk) {
		chunk = pcpu_reserved_chunk;

		off = pcpu_find_block_fit(chunk, bits, bit_align, is_atomic);
		if (off < 0) {
			err = "alloc from reserved chunk failed";
			goto fail_unlock;
		}

		off = pcpu_alloc_area(chunk, bits, bit_align, off);
		if (off >= 0)
			goto area_found;

		err = "alloc from reserved chunk failed";
		goto fail_unlock;
	}

restart:
	/* search through normal chunks */
	for (slot = pcpu_size_to_slot(size); slot <= pcpu_free_slot; slot++) {
		list_for_each_entry_safe(chunk, next, &pcpu_chunk_lists[slot],
					 list) {
			off = pcpu_find_block_fit(chunk, bits, bit_align,
						  is_atomic);
			if (off < 0) {
				if (slot < PCPU_SLOT_FAIL_THRESHOLD)
					pcpu_chunk_move(chunk, 0);
				continue;
			}

			off = pcpu_alloc_area(chunk, bits, bit_align, off);
			if (off >= 0) {
				pcpu_reintegrate_chunk(chunk);
				goto area_found;
			}
		}
	}

	spin_unlock_irqrestore(&pcpu_lock, flags);

/*
 * IAMROOT, 2022.01.22: 
 * is_atomic이 설정되고, 슬롯을 검색하여 한 번에 찾지 못한 경우 반복하지 않고
 * fail 레이블로 이동한다.
 * - is_atomic이면 위에서 mutex를 안걸었다. 주석을 보면 chunk를 동시에
 *   생성하는건 바라지 않는다.
 */
	/*
	 * No space left.  Create a new chunk.  We don't want multiple
	 * tasks to create chunks simultaneously.  Serialize and create iff
	 * there's still no empty chunk after grabbing the mutex.
	 */
	if (is_atomic) {
		err = "atomic alloc failed, no space left";
		goto fail;
	}

	if (list_empty(&pcpu_chunk_lists[pcpu_free_slot])) {
		chunk = pcpu_create_chunk(pcpu_gfp);
		if (!chunk) {
			err = "failed to allocate new chunk";
			goto fail;
		}

		spin_lock_irqsave(&pcpu_lock, flags);
		pcpu_chunk_relocate(chunk, -1);
	} else {
		spin_lock_irqsave(&pcpu_lock, flags);
	}

	goto restart;

area_found:
	pcpu_stats_area_alloc(chunk, size);
	spin_unlock_irqrestore(&pcpu_lock, flags);


/*
 * IAMROOT, 2022.02.05:
 * - atomic 인 경우 pcpu_find_block_fit 에서 populate된 공간만을
 *   찾았기때문에 populate를 할필요가 없다. 하지만 none atomic인
 *   경우 pcpu_find_block_fit에서 first fit으로 바로 찾았기 때문에
 *   해당 공간이 populate안되있다면 populate를 해줘야된다.
 */
	/* populate if not all pages are already there */
	if (!is_atomic) {
		unsigned int page_start, page_end, rs, re;

		page_start = PFN_DOWN(off);
		page_end = PFN_UP(off + size);

		bitmap_for_each_clear_region(chunk->populated, rs, re,
					     page_start, page_end) {
			WARN_ON(chunk->immutable);

			ret = pcpu_populate_chunk(chunk, rs, re, pcpu_gfp);

			spin_lock_irqsave(&pcpu_lock, flags);
			if (ret) {
				pcpu_free_area(chunk, off);
				err = "failed to populate";
				goto fail_unlock;
			}
			pcpu_chunk_populated(chunk, rs, re);
			spin_unlock_irqrestore(&pcpu_lock, flags);
		}

		mutex_unlock(&pcpu_alloc_mutex);
	}

	if (pcpu_nr_empty_pop_pages < PCPU_EMPTY_POP_PAGES_LOW)
		pcpu_schedule_balance_work();


/*
 * IAMROOT, 2022.02.05:
 * - 할당이 끝난 영역에 대해서 모든 cpu마다 memory를 초기화한다.
 */
	/* clear the areas and return address relative to base address */
	for_each_possible_cpu(cpu)
		memset((void *)pcpu_chunk_addr(chunk, cpu, 0) + off, 0, size);


/*
 * IAMROOT, 2022.02.05:
 * - pcpu pointer로 변환한다. unit 0을 기준으로 변환을 한다.
 */
	ptr = __addr_to_pcpu_ptr(chunk->base_addr + off);
	kmemleak_alloc_percpu(ptr, size, gfp);

	trace_percpu_alloc_percpu(reserved, is_atomic, size, align,
			chunk->base_addr, off, ptr);

	pcpu_memcg_post_alloc_hook(objcg, chunk, off, size);

	return ptr;

fail_unlock:
	spin_unlock_irqrestore(&pcpu_lock, flags);
fail:
	trace_percpu_alloc_percpu_fail(reserved, is_atomic, size, align);

	if (!is_atomic && do_warn && warn_limit) {
		pr_warn("allocation failed, size=%zu align=%zu atomic=%d, %s\n",
			size, align, is_atomic, err);
		dump_stack();
		if (!--warn_limit)
			pr_info("limit reached, disable warning\n");
	}
	if (is_atomic) {
		/* see the flag handling in pcpu_balance_workfn() */
		pcpu_atomic_alloc_failed = true;
		pcpu_schedule_balance_work();
	} else {
		mutex_unlock(&pcpu_alloc_mutex);
	}

	pcpu_memcg_post_alloc_hook(objcg, NULL, 0, size);

	return NULL;
}

/**
 * __alloc_percpu_gfp - allocate dynamic percpu area
 * @size: size of area to allocate in bytes
 * @align: alignment of area (max PAGE_SIZE)
 * @gfp: allocation flags
 *
 * Allocate zero-filled percpu area of @size bytes aligned at @align.  If
 * @gfp doesn't contain %GFP_KERNEL, the allocation doesn't block and can
 * be called from any context but is a lot more likely to fail. If @gfp
 * has __GFP_NOWARN then no warning will be triggered on invalid or failed
 * allocation requests.
 *
 * RETURNS:
 * Percpu pointer to the allocated area on success, NULL on failure.
 */
void __percpu *__alloc_percpu_gfp(size_t size, size_t align, gfp_t gfp)
{
	return pcpu_alloc(size, align, false, gfp);
}
EXPORT_SYMBOL_GPL(__alloc_percpu_gfp);

/**
 * __alloc_percpu - allocate dynamic percpu area
 * @size: size of area to allocate in bytes
 * @align: alignment of area (max PAGE_SIZE)
 *
 * Equivalent to __alloc_percpu_gfp(size, align, %GFP_KERNEL).
 */
void __percpu *__alloc_percpu(size_t size, size_t align)
{
	return pcpu_alloc(size, align, false, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(__alloc_percpu);

/**
 * __alloc_reserved_percpu - allocate reserved percpu area
 * @size: size of area to allocate in bytes
 * @align: alignment of area (max PAGE_SIZE)
 *
 * Allocate zero-filled percpu area of @size bytes aligned at @align
 * from reserved percpu area if arch has set it up; otherwise,
 * allocation is served from the same dynamic area.  Might sleep.
 * Might trigger writeouts.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.
 *
 * RETURNS:
 * Percpu pointer to the allocated area on success, NULL on failure.
 */
void __percpu *__alloc_reserved_percpu(size_t size, size_t align)
{
	return pcpu_alloc(size, align, true, GFP_KERNEL);
}

/**
 * pcpu_balance_free - manage the amount of free chunks
 * @empty_only: free chunks only if there are no populated pages
 *
 * If empty_only is %false, reclaim all fully free chunks regardless of the
 * number of populated pages.  Otherwise, only reclaim chunks that have no
 * populated pages.
 *
 * CONTEXT:
 * pcpu_lock (can be dropped temporarily)
 */

/*
 * IAMROOT, 2022.02.05:
 * - free_slot에서 한개 chunk를 제외하곤 empty_only에 따라 free를 한다.
 * - empty_only가 false인 경우엔 무조건 free.
 *   true인 경우엔 epmpty popuate page가 안남은 경우만 free시킨다.
 */
static void pcpu_balance_free(bool empty_only)
{
	LIST_HEAD(to_free);
	struct list_head *free_head = &pcpu_chunk_lists[pcpu_free_slot];
	struct pcpu_chunk *chunk, *next;

	lockdep_assert_held(&pcpu_lock);

	/*
	 * There's no reason to keep around multiple unused chunks and VM
	 * areas can be scarce.  Destroy all free chunks except for one.
	 */

/*
 * IAMROOT, 2022.02.05:
 * - 한개는 무조건 남겨놓고 empty_only에 따라 free시킨다.
 * - empty_only가 false인 경우엔 무조건 free.
 *   true인 경우엔 epmpty popuate page가 안남은 경우만 free시킨다.
 */
	list_for_each_entry_safe(chunk, next, free_head, list) {
		WARN_ON(chunk->immutable);

		/* spare the first one */
		if (chunk == list_first_entry(free_head, struct pcpu_chunk, list))
			continue;

		if (!empty_only || chunk->nr_empty_pop_pages == 0)
			list_move(&chunk->list, &to_free);
	}

	if (list_empty(&to_free))
		return;

	spin_unlock_irq(&pcpu_lock);
	list_for_each_entry_safe(chunk, next, &to_free, list) {
		unsigned int rs, re;

		bitmap_for_each_set_region(chunk->populated, rs, re, 0,
					   chunk->nr_pages) {
			pcpu_depopulate_chunk(chunk, rs, re);
			spin_lock_irq(&pcpu_lock);
			pcpu_chunk_depopulated(chunk, rs, re);
			spin_unlock_irq(&pcpu_lock);
		}
		pcpu_destroy_chunk(chunk);
		cond_resched();
	}
	spin_lock_irq(&pcpu_lock);
}

/**
 * pcpu_balance_populated - manage the amount of populated pages
 *
 * Maintain a certain amount of populated pages to satisfy atomic allocations.
 * It is possible that this is called when physical memory is scarce causing
 * OOM killer to be triggered.  We should avoid doing so until an actual
 * allocation causes the failure as it is possible that requests can be
 * serviced from already backed regions.
 *
 * CONTEXT:
 * pcpu_lock (can be dropped temporarily)
 */

/*
 * IAMROOT, 2022.02.05:
 * - atomic alloc을 실패했거나 system에 empty populate page가
 *   부족하면 page를 populate한다. populate도 못했으면 chunk를
 *   새로 만들고 다시 populate를 시도한다.
 * - 적정개수는 4개(PCPU_EMPTY_POP_PAGES_HIGH)
 */
static void pcpu_balance_populated(void)
{
	/* gfp flags passed to underlying allocators */
	const gfp_t gfp = GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN;
	struct pcpu_chunk *chunk;
	int slot, nr_to_pop, ret;

	lockdep_assert_held(&pcpu_lock);

	/*
	 * Ensure there are certain number of free populated pages for
	 * atomic allocs.  Fill up from the most packed so that atomic
	 * allocs don't increase fragmentation.  If atomic allocation
	 * failed previously, always populate the maximum amount.  This
	 * should prevent atomic allocs larger than PAGE_SIZE from keeping
	 * failing indefinitely; however, large atomic allocs are not
	 * something we support properly and can be highly unreliable and
	 * inefficient.
	 */
retry_pop:

/*
 * IAMROOT, 2022.02.05:
 * - aotmic alloc에서 실패를 한경우 무조건 4개를 추가로 populate하고,
 *   그게 아니라면 system empty populate를 4개로 유지한다.
 */
	if (pcpu_atomic_alloc_failed) {
		nr_to_pop = PCPU_EMPTY_POP_PAGES_HIGH;
		/* best effort anyway, don't worry about synchronization */
		pcpu_atomic_alloc_failed = false;
	} else {
		nr_to_pop = clamp(PCPU_EMPTY_POP_PAGES_HIGH -
				  pcpu_nr_empty_pop_pages,
				  0, PCPU_EMPTY_POP_PAGES_HIGH);
	}

	for (slot = pcpu_size_to_slot(PAGE_SIZE); slot <= pcpu_free_slot; slot++) {
		unsigned int nr_unpop = 0, rs, re;

		if (!nr_to_pop)
			break;

/*
 * IAMROOT, 2022.02.05:
 * - popluate를 안한 page가 있는 chunk를 찾는다.
 */
		list_for_each_entry(chunk, &pcpu_chunk_lists[slot], list) {
			nr_unpop = chunk->nr_pages - chunk->nr_populated;
			if (nr_unpop)
				break;
		}

		if (!nr_unpop)
			continue;

/*
 * IAMROOT, 2022.02.05:
 * - popluate되지 않은 영역을 찾아 최대 nr_to_pop만큼 popluate시킨다.
 */
		/* @chunk can't go away while pcpu_alloc_mutex is held */
		bitmap_for_each_clear_region(chunk->populated, rs, re, 0,
					     chunk->nr_pages) {
			int nr = min_t(int, re - rs, nr_to_pop);

			spin_unlock_irq(&pcpu_lock);
			ret = pcpu_populate_chunk(chunk, rs, rs + nr, gfp);
			cond_resched();
			spin_lock_irq(&pcpu_lock);
			if (!ret) {
				nr_to_pop -= nr;
				pcpu_chunk_populated(chunk, rs, rs + nr);
			} else {
				nr_to_pop = 0;
			}

			if (!nr_to_pop)
				break;
		}
	}

/*
 * IAMROOT, 2022.02.05:
 * - 결국 popluate를 못했으면 chunk를 하나 새로 만든다.
 */
	if (nr_to_pop) {
		/* ran out of chunks to populate, create a new one and retry */
		spin_unlock_irq(&pcpu_lock);
		chunk = pcpu_create_chunk(gfp);
		cond_resched();
		spin_lock_irq(&pcpu_lock);
		if (chunk) {
			pcpu_chunk_relocate(chunk, -1);
			goto retry_pop;
		}
	}
}

/**
 * pcpu_reclaim_populated - scan over to_depopulate chunks and free empty pages
 *
 * Scan over chunks in the depopulate list and try to release unused populated
 * pages back to the system.  Depopulated chunks are sidelined to prevent
 * repopulating these pages unless required.  Fully free chunks are reintegrated
 * and freed accordingly (1 is kept around).  If we drop below the empty
 * populated pages threshold, reintegrate the chunk if it has empty free pages.
 * Each chunk is scanned in the reverse order to keep populated pages close to
 * the beginning of the chunk.
 *
 * CONTEXT:
 * pcpu_lock (can be dropped temporarily)
 *
 */

/*
 * IAMROOT, 2022.02.05:
 * - depopulate slot에 존재하는 chunk에서 사용하지 않는 영역을 depopulate한다.
 */
static void pcpu_reclaim_populated(void)
{
	struct pcpu_chunk *chunk;
	struct pcpu_block_md *block;
	int freed_page_start, freed_page_end;
	int i, end;
	bool reintegrate;

	lockdep_assert_held(&pcpu_lock);

	/*
	 * Once a chunk is isolated to the to_depopulate list, the chunk is no
	 * longer discoverable to allocations whom may populate pages.  The only
	 * other accessor is the free path which only returns area back to the
	 * allocator not touching the populated bitmap.
	 */
	while (!list_empty(&pcpu_chunk_lists[pcpu_to_depopulate_slot])) {
		chunk = list_first_entry(&pcpu_chunk_lists[pcpu_to_depopulate_slot],
					 struct pcpu_chunk, list);
		WARN_ON(chunk->immutable);

		/*
		 * Scan chunk's pages in the reverse order to keep populated
		 * pages close to the beginning of the chunk.
		 */
		freed_page_start = chunk->nr_pages;
		freed_page_end = 0;
		reintegrate = false;

/*
 * IAMROOT, 2022.02.05:
 * - page를 끝에서부터 iterator를 한다.
 */
		for (i = chunk->nr_pages - 1, end = -1; i >= 0; i--) {
			/* no more work to do */
			if (chunk->nr_empty_pop_pages == 0)
				break;


/*
 * IAMROOT, 2022.02.05:
 * - system에서 비어있는 page가 별로 없는거 같으니 depopulate를 안하고
 *   reintegrate 한다.
 */
			/* reintegrate chunk to prevent atomic alloc failures */
			if (pcpu_nr_empty_pop_pages < PCPU_EMPTY_POP_PAGES_HIGH) {
				reintegrate = true;
				goto end_chunk;
			}

			/*
			 * If the page is empty and populated, start or
			 * extend the (i, end) range.  If i == 0, decrease
			 * i and perform the depopulation to cover the last
			 * (first) page in the chunk.
			 */
			block = chunk->md_blocks + i;
/*
 * IAMROOT, 2022.02.05:
 * - 빈 block이면서 popluate 되있으면 처음에 한해 end를 결정한다.
 *   i가 0보다 크면 연속되있으므로 이전 page로 continue하고
 *   i가 0이면 마무리를 위해서 i--를 한후 진행한다.
 */
			if (block->contig_hint == PCPU_BITMAP_BLOCK_BITS &&
			    test_bit(i, chunk->populated)) {
				if (end == -1)
					end = i;
				if (i > 0)
					continue;
				i--;
			}

/*
 * IAMROOT, 2022.02.05:
 * - 이 전 block에서 end를 못찾고, 이미 사용중인 block은 skip을 한다.
 */
			/* depopulate if there is an active range */
			if (end == -1)
				continue;

			spin_unlock_irq(&pcpu_lock);

/*
 * IAMROOT, 2022.02.05:
 * - i + 1 ~ end 까지를 depopulate 하고, end를 초기화한다.
 * - i는 사용중일수 있다는 전제(contig_hint != PCPU_BITMAP_BLOCK_BITS)가 있고,
 *   이 code에 온것이 최소 i + 1이상이 end가 되어 depopulate를 해야되는 영역이므로
 *   i + 1이후부터 end을 영역으로 잡는다.
 * - end + 1을 한 이유가 i + 1 <= X < end + 1로 pcpu_depopulate_chunk에서 처리되기
 *   때문이다.
 */
			pcpu_depopulate_chunk(chunk, i + 1, end + 1);
			cond_resched();
			spin_lock_irq(&pcpu_lock);

			pcpu_chunk_depopulated(chunk, i + 1, end + 1);
			freed_page_start = min(freed_page_start, i + 1);
			freed_page_end = max(freed_page_end, end + 1);

			/* reset the range and continue */
			end = -1;
		}

end_chunk:

/*
 * IAMROOT, 2022.02.05:
 * - depopulate한 영역이 존재하면 unmmap까지 한번에 한다.
 */
		/* batch tlb flush per chunk to amortize cost */
		if (freed_page_start < freed_page_end) {
			spin_unlock_irq(&pcpu_lock);
			pcpu_post_unmap_tlb_flush(chunk,
						  freed_page_start,
						  freed_page_end);
			cond_resched();
			spin_lock_irq(&pcpu_lock);
		}


/*
 * IAMROOT, 2022.02.05:
 * - system에 empty popluate page가 부족하거나 chunk전체가 free size인 경우
 *   reintegrate한다.
 *   chunk전체가 free size인 경우 free slot으로 옮겨지는데 후에 pcpu_balance_free에서
 *   free slot을 검사할때 해당 chunk가 nr_empty_pop_pages가 없으면 해제될것이다.
 * - 그게 아니면 sidelined에 무조건 옮긴다. pcpu_alloc같은 곳에서 for문으로
 *   slot을 조회할때 free slot 바로 직전에 sidelined slot이 조회될것이다.
 */
		if (reintegrate || chunk->free_bytes == pcpu_unit_size)
			pcpu_reintegrate_chunk(chunk);
		else
			list_move_tail(&chunk->list,
				       &pcpu_chunk_lists[pcpu_sidelined_slot]);
	}
}

/**
 * pcpu_balance_workfn - manage the amount of free chunks and populated pages
 * @work: unused
 *
 * For each chunk type, manage the number of fully free chunks and the number of
 * populated pages.  An important thing to consider is when pages are freed and
 * how they contribute to the global counts.
 */
/*
 * IAMROOT, 2022.02.05:
 * - pcpu balance work를 수행한다.
 */
static void pcpu_balance_workfn(struct work_struct *work)
{
	/*
	 * pcpu_balance_free() is called twice because the first time we may
	 * trim pages in the active pcpu_nr_empty_pop_pages which may cause us
	 * to grow other chunks.  This then gives pcpu_reclaim_populated() time
	 * to move fully free chunks to the active list to be freed if
	 * appropriate.
	 */
	mutex_lock(&pcpu_alloc_mutex);
	spin_lock_irq(&pcpu_lock);

/*
 * IAMROOT, 2022.02.05:
 * - 일단 free_slot에 있는건 한개빼구 전부 free한다.
 */
	pcpu_balance_free(false);
	pcpu_reclaim_populated();
	pcpu_balance_populated();
/*
 * IAMROOT, 2022.02.05:
 * - reclaim, balance를 하면서 free_slot에 chunk가 생겼을수도
 *   있는데 해당 chunk가 empty populated가 없다면 다시 해제해버린다.
 */
	pcpu_balance_free(true);

	spin_unlock_irq(&pcpu_lock);
	mutex_unlock(&pcpu_alloc_mutex);
}

/**
 * free_percpu - free percpu area
 * @ptr: pointer to area to free
 *
 * Free percpu area @ptr.
 *
 * CONTEXT:
 * Can be called from atomic context.
 */
void free_percpu(void __percpu *ptr)
{
	void *addr;
	struct pcpu_chunk *chunk;
	unsigned long flags;
	int size, off;
	bool need_balance = false;

	if (!ptr)
		return;

	kmemleak_free_percpu(ptr);


/*
 * IAMROOT, 2022.02.05:
 * - 받아온 pcpu pointer를 addr로 변환한다.
 */
	addr = __pcpu_ptr_to_addr(ptr);

	spin_lock_irqsave(&pcpu_lock, flags);

	chunk = pcpu_chunk_addr_search(addr);
	off = addr - chunk->base_addr;

	size = pcpu_free_area(chunk, off);

	pcpu_memcg_free_hook(chunk, off, size);

	/*
	 * If there are more than one fully free chunks, wake up grim reaper.
	 * If the chunk is isolated, it may be in the process of being
	 * reclaimed.  Let reclaim manage cleaning up of that chunk.
	 */
	if (!chunk->isolated && chunk->free_bytes == pcpu_unit_size) {
		struct pcpu_chunk *pos;

/*
 * IAMROOT, 2022.02.05:
 * - free chunk가 isolated가 아니고 완전히 빈 chunk일때,
 *   free_slot의 첫번째 chunk가 아니면 need_balance를 요청한다.
 */
		list_for_each_entry(pos, &pcpu_chunk_lists[pcpu_free_slot], list)
			if (pos != chunk) {
				need_balance = true;
				break;
			}
	} else if (pcpu_should_reclaim_chunk(chunk)) {
		pcpu_isolate_chunk(chunk);
		need_balance = true;
	}

	trace_percpu_free_percpu(chunk->base_addr, off, ptr);

	spin_unlock_irqrestore(&pcpu_lock, flags);

	if (need_balance)
		pcpu_schedule_balance_work();
}
EXPORT_SYMBOL_GPL(free_percpu);

bool __is_kernel_percpu_address(unsigned long addr, unsigned long *can_addr)
{
#ifdef CONFIG_SMP
	const size_t static_size = __per_cpu_end - __per_cpu_start;
	void __percpu *base = __addr_to_pcpu_ptr(pcpu_base_addr);
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		void *start = per_cpu_ptr(base, cpu);
		void *va = (void *)addr;

		if (va >= start && va < start + static_size) {
			if (can_addr) {
				*can_addr = (unsigned long) (va - start);
				*can_addr += (unsigned long)
					per_cpu_ptr(base, get_boot_cpu_id());
			}
			return true;
		}
	}
#endif
	/* on UP, can't distinguish from other static vars, always false */
	return false;
}

/**
 * is_kernel_percpu_address - test whether address is from static percpu area
 * @addr: address to test
 *
 * Test whether @addr belongs to in-kernel static percpu area.  Module
 * static percpu areas are not considered.  For those, use
 * is_module_percpu_address().
 *
 * RETURNS:
 * %true if @addr is from in-kernel static percpu area, %false otherwise.
 */
bool is_kernel_percpu_address(unsigned long addr)
{
	return __is_kernel_percpu_address(addr, NULL);
}

/**
 * per_cpu_ptr_to_phys - convert translated percpu address to physical address
 * @addr: the address to be converted to physical address
 *
 * Given @addr which is dereferenceable address obtained via one of
 * percpu access macros, this function translates it into its physical
 * address.  The caller is responsible for ensuring @addr stays valid
 * until this function finishes.
 *
 * percpu allocator has special setup for the first chunk, which currently
 * supports either embedding in linear address space or vmalloc mapping,
 * and, from the second one, the backing allocator (currently either vm or
 * km) provides translation.
 *
 * The addr can be translated simply without checking if it falls into the
 * first chunk. But the current code reflects better how percpu allocator
 * actually works, and the verification can discover both bugs in percpu
 * allocator itself and per_cpu_ptr_to_phys() callers. So we keep current
 * code.
 *
 * RETURNS:
 * The physical address for @addr.
 */
phys_addr_t per_cpu_ptr_to_phys(void *addr)
{
	void __percpu *base = __addr_to_pcpu_ptr(pcpu_base_addr);
	bool in_first_chunk = false;
	unsigned long first_low, first_high;
	unsigned int cpu;

	/*
	 * The following test on unit_low/high isn't strictly
	 * necessary but will speed up lookups of addresses which
	 * aren't in the first chunk.
	 *
	 * The address check is against full chunk sizes.  pcpu_base_addr
	 * points to the beginning of the first chunk including the
	 * static region.  Assumes good intent as the first chunk may
	 * not be full (ie. < pcpu_unit_pages in size).
	 */
	first_low = (unsigned long)pcpu_base_addr +
		    pcpu_unit_page_offset(pcpu_low_unit_cpu, 0);
	first_high = (unsigned long)pcpu_base_addr +
		     pcpu_unit_page_offset(pcpu_high_unit_cpu, pcpu_unit_pages);
	if ((unsigned long)addr >= first_low &&
	    (unsigned long)addr < first_high) {
		for_each_possible_cpu(cpu) {
			void *start = per_cpu_ptr(base, cpu);

			if (addr >= start && addr < start + pcpu_unit_size) {
				in_first_chunk = true;
				break;
			}
		}
	}

	if (in_first_chunk) {
		if (!is_vmalloc_addr(addr))
			return __pa(addr);
		else
			return page_to_phys(vmalloc_to_page(addr)) +
			       offset_in_page(addr);
	} else
		return page_to_phys(pcpu_addr_to_page(addr)) +
		       offset_in_page(addr);
}

/**
 * pcpu_alloc_alloc_info - allocate percpu allocation info
 * @nr_groups: the number of groups
 * @nr_units: the number of units
 *
 * Allocate ai which is large enough for @nr_groups groups containing
 * @nr_units units.  The returned ai's groups[0].cpu_map points to the
 * cpu_map array which is long enough for @nr_units and filled with
 * NR_CPUS.  It's the caller's responsibility to initialize cpu_map
 * pointer of other groups.
 *
 * RETURNS:
 * Pointer to the allocated pcpu_alloc_info on success, NULL on
 * failure.
 */
/*
 * IAMROOT, 2022.01.11:
 * 
 * - struct_size(ai, groups, nr_groups) = sizeof(struct pcpu_alloc_info) +
 *	sizeof(struct pcpu_group_info) * nr_groups
 * - base_size = 위 struct_size에서 구한 값을 struct pcpu_group_info
 *   member인 cpu_map type size로 align
 *
 * +------------------------- ai_size -----------------------+
 * +------ base_size --------------------+                   |
 * | ai              | groups[nr_groups] | cpu_map[nr_units] |
 *                   | groups[0]         | cpu_map[0]
 *                        '------------------' 모든 값 NR_CPUS
 */
struct pcpu_alloc_info * __init pcpu_alloc_alloc_info(int nr_groups,
						      int nr_units)
{
	struct pcpu_alloc_info *ai;
	size_t base_size, ai_size;
	void *ptr;
	int unit;

	base_size = ALIGN(struct_size(ai, groups, nr_groups),
			  __alignof__(ai->groups[0].cpu_map[0]));
	ai_size = base_size + nr_units * sizeof(ai->groups[0].cpu_map[0]);

	ptr = memblock_alloc(PFN_ALIGN(ai_size), PAGE_SIZE);
	if (!ptr)
		return NULL;
	ai = ptr;
/*
 * IAMROOT, 2022.01.12:
 * - ptr이 cpu_map[nr_units] 의 시작 주소가 된다.
 */
	ptr += base_size;
/*
 * IAMROOT, 2022.01.12:
 * - 일단 cpu_maps 전체를 초기화하기 위해 ai->groups[0]에 연결하고
 *   초기화한다.
 */
	ai->groups[0].cpu_map = ptr;

	for (unit = 0; unit < nr_units; unit++)
		ai->groups[0].cpu_map[unit] = NR_CPUS;

	ai->nr_groups = nr_groups;
	ai->__ai_size = PFN_ALIGN(ai_size);

	return ai;
}

/**
 * pcpu_free_alloc_info - free percpu allocation info
 * @ai: pcpu_alloc_info to free
 *
 * Free @ai which was allocated by pcpu_alloc_alloc_info().
 */
void __init pcpu_free_alloc_info(struct pcpu_alloc_info *ai)
{
	memblock_free_early(__pa(ai), ai->__ai_size);
}

/**
 * pcpu_dump_alloc_info - print out information about pcpu_alloc_info
 * @lvl: loglevel
 * @ai: allocation info to dump
 *
 * Print out information about @ai using loglevel @lvl.
 */
/*
 * IAMROOT, 2022.01.15:
 *
 * rpi4 예)
 * percpu: Embedded 24 pages/cpu s58904 r8192 d31208 u98304
 * pcpu-alloc: s58904 r8192 d31208 u98304 alloc=24*4096
 * pcpu-alloc: [0] 0 [0] 1 [0] 2 [0] 3
 *
 * x86 예)
 * percpu: Embedded 56 pages/cpu s192512 r8192 d28672 u262144
 * pcpu-alloc: s192512 r8192 d28672 u262144 alloc=1*2097152
 * pcpu-alloc: [0] 0 1 2 3 4 5 6 7 
 *
 * - alloc 1회마다 unit이 어떻게 저장되있는지 출력한다.
 *   []안의 값은 group이고, 그 밖에 잇는 값은 해당 group의 unit에 대응하는 cpu번호가
 *   표시되고, 할당이 안된 unit은 ---로 표시된다.
 */
static void pcpu_dump_alloc_info(const char *lvl,
				 const struct pcpu_alloc_info *ai)
{
	int group_width = 1, cpu_width = 1, width;
	char empty_str[] = "--------";
	int alloc = 0, alloc_end = 0;
	int group, v;
	int upa, apl;	/* units per alloc, allocs per line */

	v = ai->nr_groups;
	while (v /= 10)
		group_width++;

	v = num_possible_cpus();
	while (v /= 10)
		cpu_width++;
	empty_str[min_t(int, cpu_width, sizeof(empty_str) - 1)] = '\0';

	upa = ai->alloc_size / ai->unit_size;
	width = upa * (cpu_width + 1) + group_width + 3;
	apl = rounddown_pow_of_two(max(60 / width, 1));

	printk("%spcpu-alloc: s%zu r%zu d%zu u%zu alloc=%zu*%zu",
	       lvl, ai->static_size, ai->reserved_size, ai->dyn_size,
	       ai->unit_size, ai->alloc_size / ai->atom_size, ai->atom_size);

	for (group = 0; group < ai->nr_groups; group++) {
		const struct pcpu_group_info *gi = &ai->groups[group];
		int unit = 0, unit_end = 0;

		BUG_ON(gi->nr_units % upa);
/*
 * IAMROOT, 2022.01.15:
 * - 한번의 alloc이 for문의 1주기가 된다. 즉 한번의 alloc에서의 cpu 정보를 출력한다.
 * - ex) pcpu-alloc: [0] 0 [0] 1 [0] 2 [0] 3
 *   alloc이 4번 발생했다. 0번 group에 0, 1, 2, 3이라는 cpu 번호가 있따.
 */
		for (alloc_end += gi->nr_units / upa;
		     alloc < alloc_end; alloc++) {
/*
 * IAMROOT, 2022.01.15:
 * - [group_idx] cpu_idx
 */
			if (!(alloc % apl)) {
				pr_cont("\n");
				printk("%spcpu-alloc: ", lvl);
			}
			pr_cont("[%0*d] ", group_width, group);

/*
 * IAMROOT, 2022.01.15:
 * - 해당 unit에 cpu가 있으면 cpu번호, 없으면 ---
 */
			for (unit_end += upa; unit < unit_end; unit++)
				if (gi->cpu_map[unit] != NR_CPUS)
					pr_cont("%0*d ",
						cpu_width, gi->cpu_map[unit]);
				else
					pr_cont("%s ", empty_str);
		}
	}
	pr_cont("\n");
}

/**
 * pcpu_setup_first_chunk - initialize the first percpu chunk
 * @ai: pcpu_alloc_info describing how to percpu area is shaped
 * @base_addr: mapped address
 *
 * Initialize the first percpu chunk which contains the kernel static
 * percpu area.  This function is to be called from arch percpu area
 * setup path.
 *
 * @ai contains all information necessary to initialize the first
 * chunk and prime the dynamic percpu allocator.
 *
 * @ai->static_size is the size of static percpu area.
 *
 * @ai->reserved_size, if non-zero, specifies the amount of bytes to
 * reserve after the static area in the first chunk.  This reserves
 * the first chunk such that it's available only through reserved
 * percpu allocation.  This is primarily used to serve module percpu
 * static areas on architectures where the addressing model has
 * limited offset range for symbol relocations to guarantee module
 * percpu symbols fall inside the relocatable range.
 *
 * @ai->dyn_size determines the number of bytes available for dynamic
 * allocation in the first chunk.  The area between @ai->static_size +
 * @ai->reserved_size + @ai->dyn_size and @ai->unit_size is unused.
 *
 * @ai->unit_size specifies unit size and must be aligned to PAGE_SIZE
 * and equal to or larger than @ai->static_size + @ai->reserved_size +
 * @ai->dyn_size.
 *
 * @ai->atom_size is the allocation atom size and used as alignment
 * for vm areas.
 *
 * @ai->alloc_size is the allocation size and always multiple of
 * @ai->atom_size.  This is larger than @ai->atom_size if
 * @ai->unit_size is larger than @ai->atom_size.
 *
 * @ai->nr_groups and @ai->groups describe virtual memory layout of
 * percpu areas.  Units which should be colocated are put into the
 * same group.  Dynamic VM areas will be allocated according to these
 * groupings.  If @ai->nr_groups is zero, a single group containing
 * all units is assumed.
 *
 * The caller should have mapped the first chunk at @base_addr and
 * copied static data to each unit.
 *
 * The first chunk will always contain a static and a dynamic region.
 * However, the static region is not managed by any chunk.  If the first
 * chunk also contains a reserved region, it is served by two chunks -
 * one for the reserved region and one for the dynamic region.  They
 * share the same vm, but use offset regions in the area allocation map.
 * The chunk serving the dynamic region is circulated in the chunk slots
 * and available for dynamic allocation like any other chunk.
 */
/*
 * IAMROOT, 2022.01.23:
 * - group별 cpu 정보를 초기화한다.
 * - pcpu전역변수들을 초기화한다.
 * - slot을 생성한다.(pcpu_chunk_lists)
 * - dynamic, reserved chunk를 생성하고
 *   pcpu_first_chunk(dynamic)를 slot에 넣는다.
*/
void __init pcpu_setup_first_chunk(const struct pcpu_alloc_info *ai,
				   void *base_addr)
{
	size_t size_sum = ai->static_size + ai->reserved_size + ai->dyn_size;
	size_t static_size, dyn_size;
	struct pcpu_chunk *chunk;
/*
 * IAMROOT, 2022.01.15:
 * - group_offsets : 임시 배열. group 개수만큼 생성. 해당 group의 base_offset 저장.
 * - group_size : group개수 만큼 생성. 해당 group의 unit들이 사용하는 memory size 저장
 * - unit_off : possible cpu개수만큼 생성.
 *   제일 낮은 unit 주소로부터 해당 unit들의 시작위치 offset
 * - unit_map
 *   possible cpu개수만큼 생성, 초기값으로 UNIT_MAX로 설정
 *   cpu to unit 번호.
 */
	unsigned long *group_offsets;
	size_t *group_sizes;
	unsigned long *unit_off;
	unsigned int cpu;
	int *unit_map;
	int group, unit, i;
	int map_size;
	unsigned long tmp_addr;
	size_t alloc_size;

#define PCPU_SETUP_BUG_ON(cond)	do {					\
	if (unlikely(cond)) {						\
		pr_emerg("failed to initialize, %s\n", #cond);		\
		pr_emerg("cpu_possible_mask=%*pb\n",			\
			 cpumask_pr_args(cpu_possible_mask));		\
		pcpu_dump_alloc_info(KERN_EMERG, ai);			\
		BUG();							\
	}								\
} while (0)

	/* sanity checks */
	PCPU_SETUP_BUG_ON(ai->nr_groups <= 0);
#ifdef CONFIG_SMP
	PCPU_SETUP_BUG_ON(!ai->static_size);
	PCPU_SETUP_BUG_ON(offset_in_page(__per_cpu_start));
#endif
	PCPU_SETUP_BUG_ON(!base_addr);
	PCPU_SETUP_BUG_ON(offset_in_page(base_addr));
	PCPU_SETUP_BUG_ON(ai->unit_size < size_sum);
	PCPU_SETUP_BUG_ON(offset_in_page(ai->unit_size));
	PCPU_SETUP_BUG_ON(ai->unit_size < PCPU_MIN_UNIT_SIZE);
	PCPU_SETUP_BUG_ON(!IS_ALIGNED(ai->unit_size, PCPU_BITMAP_BLOCK_SIZE));
	PCPU_SETUP_BUG_ON(ai->dyn_size < PERCPU_DYNAMIC_EARLY_SIZE);
	PCPU_SETUP_BUG_ON(!ai->dyn_size);
	PCPU_SETUP_BUG_ON(!IS_ALIGNED(ai->reserved_size, PCPU_MIN_ALLOC_SIZE));
	PCPU_SETUP_BUG_ON(!(IS_ALIGNED(PCPU_BITMAP_BLOCK_SIZE, PAGE_SIZE) ||
			    IS_ALIGNED(PAGE_SIZE, PCPU_BITMAP_BLOCK_SIZE)));
	PCPU_SETUP_BUG_ON(pcpu_verify_alloc_info(ai) < 0);

	/* process group information and build config tables accordingly */
	alloc_size = ai->nr_groups * sizeof(group_offsets[0]);
	group_offsets = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!group_offsets)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

	alloc_size = ai->nr_groups * sizeof(group_sizes[0]);
	group_sizes = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!group_sizes)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

	alloc_size = nr_cpu_ids * sizeof(unit_map[0]);
	unit_map = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!unit_map)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

	alloc_size = nr_cpu_ids * sizeof(unit_off[0]);
	unit_off = memblock_alloc(alloc_size, SMP_CACHE_BYTES);
	if (!unit_off)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      alloc_size);

	for (cpu = 0; cpu < nr_cpu_ids; cpu++)
		unit_map[cpu] = UINT_MAX;

	pcpu_low_unit_cpu = NR_CPUS;
	pcpu_high_unit_cpu = NR_CPUS;

	for (group = 0, unit = 0; group < ai->nr_groups; group++, unit += i) {
		const struct pcpu_group_info *gi = &ai->groups[group];

		group_offsets[group] = gi->base_offset;
		group_sizes[group] = gi->nr_units * ai->unit_size;

		for (i = 0; i < gi->nr_units; i++) {
			cpu = gi->cpu_map[i];
			if (cpu == NR_CPUS)
				continue;

			PCPU_SETUP_BUG_ON(cpu >= nr_cpu_ids);
			PCPU_SETUP_BUG_ON(!cpu_possible(cpu));
			PCPU_SETUP_BUG_ON(unit_map[cpu] != UINT_MAX);

			unit_map[cpu] = unit + i;
			unit_off[cpu] = gi->base_offset + i * ai->unit_size;

			/* determine low/high unit_cpu */
			if (pcpu_low_unit_cpu == NR_CPUS ||
			    unit_off[cpu] < unit_off[pcpu_low_unit_cpu])
				pcpu_low_unit_cpu = cpu;
			if (pcpu_high_unit_cpu == NR_CPUS ||
			    unit_off[cpu] > unit_off[pcpu_high_unit_cpu])
				pcpu_high_unit_cpu = cpu;
		}
	}
	pcpu_nr_units = unit;

	for_each_possible_cpu(cpu)
		PCPU_SETUP_BUG_ON(unit_map[cpu] == UINT_MAX);

	/* we're done parsing the input, undefine BUG macro and dump config */
#undef PCPU_SETUP_BUG_ON
	pcpu_dump_alloc_info(KERN_DEBUG, ai);

	pcpu_nr_groups = ai->nr_groups;
	pcpu_group_offsets = group_offsets;
	pcpu_group_sizes = group_sizes;
	pcpu_unit_map = unit_map;
	pcpu_unit_offsets = unit_off;

	/* determine basic parameters */
	pcpu_unit_pages = ai->unit_size >> PAGE_SHIFT;
	pcpu_unit_size = pcpu_unit_pages << PAGE_SHIFT;
	pcpu_atom_size = ai->atom_size;
	pcpu_chunk_struct_size = struct_size(chunk, populated,
					     BITS_TO_LONGS(pcpu_unit_pages));

	pcpu_stats_save_ai(ai);

	/*
	 * Allocate chunk slots.  The slots after the active slots are:
	 *   sidelined_slot - isolated, depopulated chunks
	 *   free_slot - fully free chunks
	 *   to_depopulate_slot - isolated, chunks to depopulate
	 */
	pcpu_sidelined_slot = __pcpu_size_to_slot(pcpu_unit_size) + 1;
	pcpu_free_slot = pcpu_sidelined_slot + 1;
	pcpu_to_depopulate_slot = pcpu_free_slot + 1;
	pcpu_nr_slots = pcpu_to_depopulate_slot + 1;
	pcpu_chunk_lists = memblock_alloc(pcpu_nr_slots *
					  sizeof(pcpu_chunk_lists[0]),
					  SMP_CACHE_BYTES);
	if (!pcpu_chunk_lists)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      pcpu_nr_slots * sizeof(pcpu_chunk_lists[0]));

	for (i = 0; i < pcpu_nr_slots; i++)
		INIT_LIST_HEAD(&pcpu_chunk_lists[i]);

	/*
	 * The end of the static region needs to be aligned with the
	 * minimum allocation size as this offsets the reserved and
	 * dynamic region.  The first chunk ends page aligned by
	 * expanding the dynamic region, therefore the dynamic region
	 * can be shrunk to compensate while still staying above the
	 * configured sizes.
	 */
	static_size = ALIGN(ai->static_size, PCPU_MIN_ALLOC_SIZE);
/*
 * IAMROOT, 2022.01.15:
 * - static_size를 align하고 align으로 추가된 size를 dyn_size에서 뺀다.
 */
	dyn_size = ai->dyn_size - (static_size - ai->static_size);

	/*
	 * Initialize first chunk.
	 * If the reserved_size is non-zero, this initializes the reserved
	 * chunk.  If the reserved_size is zero, the reserved chunk is NULL
	 * and the dynamic region is initialized here.  The first chunk,
	 * pcpu_first_chunk, will always point to the chunk that serves
	 * the dynamic region.
	 */
/*
 * IAMROOT, 2022.01.15:
 * - reserved가 있으면 tmp_addr은 reserved 영역, 아니면 dynamic 영역의 시작주소가
 *   될것이다.
 * - 그에 따라 map_size도 reserved_size를 보고 해당 영역 size로 정해진다.
 */
	tmp_addr = (unsigned long)base_addr + static_size;
	map_size = ai->reserved_size ?: dyn_size;
	chunk = pcpu_alloc_first_chunk(tmp_addr, map_size);

/*
 * IAMROOT, 2022.01.15:
 * - reserved_size가 있다면 방금 초기화한게 reserved 영역이므로 해당 영역을
 *   관리하도록한다.
 * - reserved 다음영역은 dynamic 영역이므로 한번더 pcpu_alloc_chunk으로 
 *   dynamic 영역을 초기화한다.
 * - 초기화된 dynamic 영역은 pcpu_first_chunk로 관리된다.
 */
	/* init dynamic chunk if necessary */
	if (ai->reserved_size) {
		pcpu_reserved_chunk = chunk;

		tmp_addr = (unsigned long)base_addr + static_size +
			   ai->reserved_size;
		map_size = dyn_size;
		chunk = pcpu_alloc_first_chunk(tmp_addr, map_size);
	}

	/* link the first chunk in */
	pcpu_first_chunk = chunk;
	pcpu_nr_empty_pop_pages = pcpu_first_chunk->nr_empty_pop_pages;
	pcpu_chunk_relocate(pcpu_first_chunk, -1);

	/* include all regions of the first chunk */
	pcpu_nr_populated += PFN_DOWN(size_sum);

	pcpu_stats_chunk_alloc();
	trace_percpu_create_chunk(base_addr);

	/* we're done */
	pcpu_base_addr = base_addr;
}

#ifdef CONFIG_SMP

const char * const pcpu_fc_names[PCPU_FC_NR] __initconst = {
	[PCPU_FC_AUTO]	= "auto",
	[PCPU_FC_EMBED]	= "embed",
	[PCPU_FC_PAGE]	= "page",
};

enum pcpu_fc pcpu_chosen_fc __initdata = PCPU_FC_AUTO;


/*
 * IAMROOT, 2022.02.05:
 * - early param으로 embed, page를 고를수있다.
 */
static int __init percpu_alloc_setup(char *str)
{
	if (!str)
		return -EINVAL;

	if (0)
		/* nada */;
#ifdef CONFIG_NEED_PER_CPU_EMBED_FIRST_CHUNK
	else if (!strcmp(str, "embed"))
		pcpu_chosen_fc = PCPU_FC_EMBED;
#endif
#ifdef CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK
	else if (!strcmp(str, "page"))
		pcpu_chosen_fc = PCPU_FC_PAGE;
#endif
	else
		pr_warn("unknown allocator %s specified\n", str);

	return 0;
}
early_param("percpu_alloc", percpu_alloc_setup);

/*
 * pcpu_embed_first_chunk() is used by the generic percpu setup.
 * Build it if needed by the arch config or the generic setup is going
 * to be used.
 */
#if defined(CONFIG_NEED_PER_CPU_EMBED_FIRST_CHUNK) || \
	!defined(CONFIG_HAVE_SETUP_PER_CPU_AREA)
#define BUILD_EMBED_FIRST_CHUNK
#endif

/* build pcpu_page_first_chunk() iff needed by the arch config */
#if defined(CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK)
#define BUILD_PAGE_FIRST_CHUNK
#endif

/* pcpu_build_alloc_info() is used by both embed and page first chunk */
#if defined(BUILD_EMBED_FIRST_CHUNK) || defined(BUILD_PAGE_FIRST_CHUNK)
/**
 * pcpu_build_alloc_info - build alloc_info considering distances between CPUs
 * @reserved_size: the size of reserved percpu area in bytes
 * @dyn_size: minimum free size for dynamic allocation in bytes
 * @atom_size: allocation atom size
 * @cpu_distance_fn: callback to determine distance between cpus, optional
 *
 * This function determines grouping of units, their mappings to cpus
 * and other parameters considering needed percpu size, allocation
 * atom size and distances between CPUs.
 *
 * Groups are always multiples of atom size and CPUs which are of
 * LOCAL_DISTANCE both ways are grouped together and share space for
 * units in the same group.  The returned configuration is guaranteed
 * to have CPUs on different nodes on different groups and >=75% usage
 * of allocated virtual address space.
 *
 * RETURNS:
 * On success, pointer to the new allocation_info is returned.  On
 * failure, ERR_PTR value is returned.
 */
/*
 * IAMROOT, 2022.01.11:
 * - struct pcpu_alloc_info 구조체를 초기화한다.
 *   1) alloc_size를 구한다.(pcpu_alloc_info.alloc_size)
 *   2) 해당 alloc_size에 몇개의 unit을 넣을수 있는지 산출한다.
 *   4) unit_size를 구한다.(pcpu_alloc_info.unit_size)
 *   5) 같은 numa node에 있는 cpu끼리 group화 하여 정보를 초기화하고 해당
 *   group에 속한 cpu정보를 넣는다.(pcpu_alloc_info.group, group.cpu_map)
 *
 * ---
 *
 * - unit
 *   static + reserved + dyn 을 합쳐 부르는말. cpu 한개당 unit 1개가 할당
 *   되며, alloc에 따라 cpu가 할당되지 않은 unit도 존재한다.
 *
 * - group
 *   cpu끼리의 numa가 서로 같은 numa인 경우(LOCAL_DISTANCE) group이라 칭함.
 *
 * - upa(unit per allocation)
 *   1번의 alloc으로 관리할수있는 unit 수
 *
 * ---
 */
static struct pcpu_alloc_info * __init __flatten pcpu_build_alloc_info(
				size_t reserved_size, size_t dyn_size,
				size_t atom_size,
				pcpu_fc_cpu_distance_fn_t cpu_distance_fn)
{
/*
 * IAMROOT, 2022.01.11:
 * - group_map[cpu 번호] = 해당 cpu가 속한 group
 * - group_cnt[group 번호] = 해당 group에 속한 cpu cnt 
 */
	static int group_map[NR_CPUS] __initdata;
	static int group_cnt[NR_CPUS] __initdata;
	static struct cpumask mask __initdata;
/*
 * IAMROOT, 2022.01.11:
 * PERCPU_INPUT 참고
 */
	const size_t static_size = __per_cpu_end - __per_cpu_start;
	int nr_groups = 1, nr_units = 0;
	size_t size_sum, min_unit_size, alloc_size;
	int upa, max_upa, best_upa;	/* units_per_alloc */
	int last_allocs, group, unit;
	unsigned int cpu, tcpu;
	struct pcpu_alloc_info *ai;
	unsigned int *cpu_map;

	/* this function may be called multiple times */
	memset(group_map, 0, sizeof(group_map));
	memset(group_cnt, 0, sizeof(group_cnt));
	cpumask_clear(&mask);

	/* calculate size_sum and ensure dyn_size is enough for early alloc */
/*
 * IAMROOT, 2022.01.08:
 * ARM64:
 *   size_sum = static(compile 결정) + reserved_size(8K) + dyn_size(28K)
 */

	size_sum = PFN_ALIGN(static_size + reserved_size +
			    max_t(size_t, dyn_size, PERCPU_DYNAMIC_EARLY_SIZE));
	dyn_size = size_sum - static_size - reserved_size;

	/*
	 * Determine min_unit_size, alloc_size and max_upa such that
	 * alloc_size is multiple of atom_size and is the smallest
	 * which can accommodate 4k aligned segments which are equal to
	 * or larger than min_unit_size.
	 */
/*
 * IAMROOT, 2022.01.12:
 * 한개의 unit에 필요한 size를 확정한다.
 */
	min_unit_size = max_t(size_t, size_sum, PCPU_MIN_UNIT_SIZE);

	/* determine the maximum # of units that can fit in an allocation */
/*
 * IAMROOT, 2022.01.08:
 * alloc size를 확정한다. atom_size단위로 alloc을 요청하는데, 만약 atom_size가
 * 클 경우 min_unit_size보다 커질것이므로 여러개의 unit을 관리할수있을것이다.
 * alloc_size를 unit_size로 나눠서 나머지가 안남고, page_align되게 unit_size를
 * 조정한다.(alloc_size당 unit개수를 낮추는게 결국 unit_size를 높이게 된다.)
 *
 * --- 
 *
 * upa: unit per allocation
 *      SMP 시스템에서 atom_size가 M단위의 큰 할당을 사용하고, 더 빠른
 *      성능을 내게한다.
 *      arm64) atom_size가 4K가 사용되므로 항상 1로 산출된다.
 *
 * 예) atom_size=2M, min_unit_size=44K
 *     max_upa?
 *     -> 1) alloc_size=2M, upa=46(2M/44K)
 *        2) while 후 -> 2M % 46 || offset_in_page(2M / 46)
 *           2M를 나머지 없이 떨어지는 값으로 만들면 46, 45, ... 32
 *           2M % 32=0, offset_in_page(2M/32)=0
 *           결국 max_upa=32
 *
 * - alloc 당 unit 의 max개수를 정한다. atom_size가 클경우,
 *   1개의 alloc memory에 여러개의 unit이 들어갈수있다.
 *   --- alloc_size % upa가 있는 경우
 *		alloc당 unit 개수를 줄임으로 unit 당 size를 늘린다.
 *
 *		ex) alloc_size=2M, upa=46(2M/44K)
 *		그냥 바로 unit당 alloc size를 구해보면
 *		alloc_size / upa = 45590.26..
 *		즉 소수점 size가 남게 되고 upa당 size는 45590 byte가 될것이다.
 *
 *		upa를 소수점이 안남을때까지 줄이면 upa는 32가 된다
 *		2M / 32 = 65536
 *		alloc당 unit개수는 32개가 되고, unit의 size는 65536으로 커지고
 *		소수점 size는 없으니 남는 사이즈도 없다.
 *   --- offset_in_page(alloc_size / upa)
 *		page align확인.
 */
	alloc_size = roundup(min_unit_size, atom_size);
	upa = alloc_size / min_unit_size;
	while (alloc_size % upa || (offset_in_page(alloc_size / upa)))
		upa--;
	max_upa = upa;

/*
 * IAMROOT, 2022.01.08:
 * 노드별 해당 cpu를 그룹화한다.
 * 예) NUMA: node#0:cpu#0-3, node#1:cpu#4-7
 *     group_map[], group_cnt[], nr_groups ?
 *     -> group_map[0..3]=0    <- 0~3번 cpu는 0번 노드로 배치
 *        group_map[4..7]=1    <- 4~7번 cpu는 1번 노드로 배치
 *        group_cnt[0..1]=4    <- 각 노드는 각가 4개의 cpu를 가진다.
 *        nr_groups=2          <- 2개의 노드
 *
 *  예2) NUMA node#0:cpu#0-3 node#1:cpu4-5 node#2:cpu6-7 node#3:cpu8-11
 * 		 group_map[], group_cnt[], nr_groups ?
 *     -> group_map[0..3]=0
 *        group_map[4..5]=1
 * 		  group_map[6..7]=2
 * 		  group_map[8..11]=3
 *        group_cnt[0]=4
 *     	  group_cnt[1]=2
 * 		  group_cnt[2]=2
 * 		  group_cnt[3]=4
 *        nr_groups=4
 */
	cpumask_copy(&mask, cpu_possible_mask);

	/* group cpus according to their proximity */
	for (group = 0; !cpumask_empty(&mask); group++) {
		/* pop the group's first cpu */
		cpu = cpumask_first(&mask);
		group_map[cpu] = group;
		group_cnt[group]++;
		cpumask_clear_cpu(cpu, &mask);

		for_each_cpu(tcpu, &mask) {
			if (!cpu_distance_fn ||
			    (cpu_distance_fn(cpu, tcpu) == LOCAL_DISTANCE &&
			     cpu_distance_fn(tcpu, cpu) == LOCAL_DISTANCE)) {
				group_map[tcpu] = group;
				group_cnt[group]++;
				cpumask_clear_cpu(tcpu, &mask);
			}
		}
	}
	nr_groups = group;

	/*
	 * Wasted space is caused by a ratio imbalance of upa to group_cnt.
	 * Expand the unit_size until we use >= 75% of the units allocated.
	 * Related to atom_size, which could be much larger than the unit_size.
	 */
	last_allocs = INT_MAX;
	best_upa = 0;
	for (upa = max_upa; upa; upa--) {
		int allocs = 0, wasted = 0;

/*
 * IAMROOT, 2022.01.08:
 * - alloc_size가 atom_size의 단위로 정렬되어 있고, 이 값은 1, 2, 4M 이런 단위를
 * 사용하며 2의 승수 단위 나눌때에만 나머지가 없다. 따라서 upa가 32부터 시작한
 * 경우 32, 16, 8, 4, 2, 1과 같은 값 이외에는 continue를 통해 skip 한다.
 * 예) 2M%32 != 0 || ((2M/32)%4k != 0)
 *
 * - 위에서 max_upa를 정할때 같은 방식으로 upa를 넘기는것을 확인했었다.
 *   page_align이 안맞거나alloc_size를 upa로 나눴을때 나머지가 있는 upa는
 *   skip한다.
 */
		if (alloc_size % upa || (offset_in_page(alloc_size / upa)))
			continue;

/*
 * IAMROOT, 2022.01.08:
 * 모든 노드에 대해 반복하며 allocs, wasted를 누적하여 구한다.
 * allocs: 할당할 수
 * wasted: 낭비되는 유닛 수
 *
 * ex) group_cnt[4] = {4, 2, 2, 4};
 * wasted[x] = (this_allocs[x] * upa - group_cnt[x])
 * 1) upa = 32, this_allocs[4] = {1, 1, 1, 1};
 *	allocs = 4 : wasted = (32-4) + (32-2) + (32-2) + (32-4) = 116 (낭비)
 * 2) upa = 16, this_allocs[4] = {1, 1, 1, 1};
 *	allocs = 4 : wasted = (16-4) + (16-2) + (16-2) + (16-4) = 52 (낭비)
 * 3) upa = 8, this_allocs[4] = {1, 1, 1, 1};
 *	allocs = 4 : wasted = (8-4) + (8-2) + (8-2) + (8-4) = 20 (낭비)
 * 4) upa = 4, this_allocs[4] = {1, 1, 1, 1};
 *	allocs = 4 : wasted = (4-4) + (4-2) + (4-2) + (4-4) = 4 (정상))
 *	-> 확정. 5)에서 allocs가 6으로 커지므로 break됨.
 * 5) upa = 2, this_allocs[4] = {2, 1, 1, 2};
 *	allocs = 6 : wasted = (4-4) + (2-2) + (2-2) + (4-4) = 0 (정상))
 *
 * 즉 wasted가 없는 경우가 있을지라도 allocs가 적은게 우선이 된다.
 */
		for (group = 0; group < nr_groups; group++) {
/*
 * IAMROOT, 2022.01.11:
 * - group_cnt[group] = group에 속한 cpu개수
 * - this_allocs = group에 속한 cpu개수를 전부 넣을수있는 할당횟수.
 * - wasted = (upa 단위로 할당한 size) - (group에 속한 cpu개수)
 *			= upa * this_allocs - (group에 속한 cpu개수)
 * group_cnt[group]이 upa의 배수에 맞지 않는다면 round up된만큼 wasted가 생긴다.
 */
			int this_allocs = DIV_ROUND_UP(group_cnt[group], upa);
			allocs += this_allocs;
			wasted += this_allocs * upa - group_cnt[group];
		}

		/*
		 * Don't accept if wastage is over 1/3.  The
		 * greater-than comparison ensures upa==1 always
		 * passes the following check.
		 */
/*
 * IAMROOT, 2022.01.08:
 * 필요한 유닛(possible cpus)들의 1/3을 초과하는, 즉 사용하지 않는 유닛들이 
 * 발생하면 너무 많이 낭비가 된다 판단하여 skip 한다.
 */
		if (wasted > num_possible_cpus() / 3)
			continue;

		/* and then don't consume more memory */
/*
 * IAMROOT, 2022.01.08:
 * 다시 반복할 때 allocs 수가 기존에 산출한 alloc 수보다 커지는 경우
 * 더 이상 진행할 필요가 없으므로 break 한다.
 * 즉 allocs가 가장 작은 값으로 사용한다.
 */
		if (allocs > last_allocs)
			break;
		last_allocs = allocs;
		best_upa = upa;
	}
	BUG_ON(!best_upa);
	upa = best_upa;

	/* allocate and fill alloc_info */
/*
 * IAMROOT, 2022.01.08: 
 * nr_units: 사용하지 않는 유닛도 모두 더한다.
 */
	for (group = 0; group < nr_groups; group++)
		nr_units += roundup(group_cnt[group], upa);

/*
 * IAMROOT, 2022.01.08: 
 * pcpu_alloc_info 구조체를 할당해온다.
 *   내부에 그룹(노드)별 구조체인 pcpu_group_info들이 채워지며
 *   마지막에 유닛 수만큼 4바이트 정보가 추가된다.
 */
	ai = pcpu_alloc_alloc_info(nr_groups, nr_units);
	if (!ai)
		return ERR_PTR(-ENOMEM);
/*
 * IAMROOT, 2022.01.08: 
 * 0번 그룹만 cpu_map을 연결해두었었다.
 * 그 정보를 사용하여 전체 그룹 수 만큼 cpu_map을 기록한다.
 */
	cpu_map = ai->groups[0].cpu_map;

/*
 * IAMROOT, 2022.01.11:
 * ex) group_cnt[4] = {4, 2, 2, 4}, upa = 4
 * nr_units = 4 + 4 + 4 + 4 = 16
 *
 * | ai | groups[0..nr_groups] | cpu_map[0..nr_units] |
 * | ai | groups[0..3]         | cpu_map[0..15]       |
 *        gorup[0] ------------->cpu_map[0]
 *        group[1] ---------------->cpu_map[4]
 *        group[2] ------------------->cpu_map[8]
 *        group[3] ----------------------->cpu_map[12]
 */
	for (group = 0; group < nr_groups; group++) {
		ai->groups[group].cpu_map = cpu_map;
		cpu_map += roundup(group_cnt[group], upa);
	}

/*
 * IAMROOT, 2022.01.08: 
 * 나머지 ai 정보를 채운다.
 */
	ai->static_size = static_size;
	ai->reserved_size = reserved_size;
	ai->dyn_size = dyn_size;
	ai->unit_size = alloc_size / upa;
	ai->atom_size = atom_size;
	ai->alloc_size = alloc_size;

/*
 * IAMROOT, 2022.01.08: 
 * pcpu_group_info[]->base_offset에 임시로 해당 group의 unit start 주소를 넣고
 * (후에 고쳐진다) pcpu_group_info[]->cpu_map[]이 가리키는 곳에
 * cpu 번호를 기록한다. 즉 unit->cpu map을 완성시칸다.
 */
	for (group = 0, unit = 0; group < nr_groups; group++) {
		struct pcpu_group_info *gi = &ai->groups[group];

		/*
		 * Initialize base_offset as if all groups are located
		 * back-to-back.  The caller should update this to
		 * reflect actual allocation.
		 */
		gi->base_offset = unit * ai->unit_size;

/*
 * IAMROOT, 2022.01.12:
 * - cpu가 group에 속해 있다면 해당 group info의 cpu_map에 cpu 번호를
 *   순서대로 넣는다.
 *
 * ex) group_cnt[4] = {4, 2, 2, 4}, upa = 4
 *     NUMA node#0:cpu#0-3 node#1:cpu4-5 node#2:cpu6-7 node#3:cpu8-11
 *
 * | ai | groups[0..3]  | cpu_map[0..15]                               |
 *        gorup[0] ------>&cpu_map[0] = {0, 1, 2, 3}
 *        group[1] --------->&cpu_map[4] = {4, 5, NR_CPUS, NR_CPUS}
 *        group[2] ------------>&cpu_map[8] = {6, 7, NR_CPUS, NR_CPUS}
 *        group[3] ---------------->&cpu_map[12] = {8, 9, 10, 11}
 */
		for_each_possible_cpu(cpu)
			if (group_map[cpu] == group)
				gi->cpu_map[gi->nr_units++] = cpu;
		gi->nr_units = roundup(gi->nr_units, upa);
		unit += gi->nr_units;
	}
	BUG_ON(unit != nr_units);

	return ai;
}
#endif /* BUILD_EMBED_FIRST_CHUNK || BUILD_PAGE_FIRST_CHUNK */

#if defined(BUILD_EMBED_FIRST_CHUNK)
/**
 * pcpu_embed_first_chunk - embed the first percpu chunk into bootmem
 * @reserved_size: the size of reserved percpu area in bytes
 * @dyn_size: minimum free size for dynamic allocation in bytes
 * @atom_size: allocation atom size
 * @cpu_distance_fn: callback to determine distance between cpus, optional
 * @alloc_fn: function to allocate percpu page
 * @free_fn: function to free percpu page
 *
 * This is a helper to ease setting up embedded first percpu chunk and
 * can be called where pcpu_setup_first_chunk() is expected.
 *
 * If this function is used to setup the first chunk, it is allocated
 * by calling @alloc_fn and used as-is without being mapped into
 * vmalloc area.  Allocations are always whole multiples of @atom_size
 * aligned to @atom_size.
 *
 * This enables the first chunk to piggy back on the linear physical
 * mapping which often uses larger page size.  Please note that this
 * can result in very sparse cpu->unit mapping on NUMA machines thus
 * requiring large vmalloc address space.  Don't use this allocator if
 * vmalloc space is not orders of magnitude larger than distances
 * between node memory addresses (ie. 32bit NUMA machines).
 *
 * @dyn_size specifies the minimum dynamic area size.
 *
 * If the needed size is smaller than the minimum or specified unit
 * size, the leftover is returned using @free_fn.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * IAMROOT, 2022.01.15:
 * - alloc_info를 구해오고 group마다 unit_map과 할당 정보를 초기화한다.
 * - 사용하지 않은 cpu_map을 참고해 unitmemory를 할당해제 한다.
 * - node(group)마다 unit들을 할당하고. static 영역을 복사한다. 또한
 *   이때 alloc_info를 참고해 사용하지 않은 memory는 해제한다.
 * - base(가장 낮은 주소의 unit_memory 주소)와 max_distance(unit memory span size)를
 *   구한다. 이때 max_distance가 vmalloc size의 3/4을 넘는지 확인한다.
 * - 각 node마다 base_offset을 구한다.
 * - pcpu_setup_first_chunk를 수행한다.
 */
int __init pcpu_embed_first_chunk(size_t reserved_size, size_t dyn_size,
				  size_t atom_size,
				  pcpu_fc_cpu_distance_fn_t cpu_distance_fn,
				  pcpu_fc_alloc_fn_t alloc_fn,
				  pcpu_fc_free_fn_t free_fn)
{
	void *base = (void *)ULONG_MAX;
	void **areas = NULL;
	struct pcpu_alloc_info *ai;
	size_t size_sum, areas_size;
	unsigned long max_distance;
	int group, i, highest_group, rc = 0;

/*
 * IAMROOT, 2022.01.08: 
 * pcpu_alloc_info 구조체를 할당해온다.
 * - setup_per_cpu_areas 에서 불러진 경우
 *   @reserved_size PERCPU_MODULE_RESERVE. 8k
 *   @dyn_size PERCPU_DYNAMIC_RESERVE 28k
 *   @atom_size PAGE_SIZE
 *   @cpu_distance_fn pcpu_cpu_distance,
 *
 * - alloc info memory map
 * | ai | groups[0..nr_groups] | cpu_map[0..nr_units] |
 *               `--`--`--.....----^--^--^........^
 */
	ai = pcpu_build_alloc_info(reserved_size, dyn_size, atom_size,
				   cpu_distance_fn);
	if (IS_ERR(ai))
		return PTR_ERR(ai);

/*
 * IAMROOT, 2022.01.15:
 * - size_sum : unit size
 * - areas : group 주소를 저장할 임시배열
 *
 * group 주소를 저장해놓을 임시 배열을 할당해놓는다.
 */
	size_sum = ai->static_size + ai->reserved_size + ai->dyn_size;
	areas_size = PFN_ALIGN(ai->nr_groups * sizeof(void *));

	areas = memblock_alloc(areas_size, SMP_CACHE_BYTES);
	if (!areas) {
		rc = -ENOMEM;
		goto out_free;
	}

	/* allocate, copy and determine base address & max_distance */
	highest_group = 0;
	for (group = 0; group < ai->nr_groups; group++) {
		struct pcpu_group_info *gi = &ai->groups[group];
		unsigned int cpu = NR_CPUS;
		void *ptr;

/*
 * IAMROOT, 2022.01.15:
 * - 최초에 cpu가 세팅된 cpu_map을 찾는다.
 * - unit memory 준비
 * areas[0]          = | group[0].nr_units * unit_size |
 * areas[1]          = | group[1].nr_units * unit_size |
 * ..
 * areas[nr_groups-1] = | group[nr_groups-1].nr_units * unit_size |
 */
		for (i = 0; i < gi->nr_units && cpu == NR_CPUS; i++)
			cpu = gi->cpu_map[i];
		BUG_ON(cpu == NR_CPUS);

/*
 * IAMROOT, 2022.01.15:
 * - pcpu_embed_first_chunk에서 불러왔을때 alloc_fn == pcpu_fc_alloc
 * - cpu의 nid가 속한 memblock에서 해당 group의 unit memory을 할당해온다.
 */
		/* allocate space for the whole group */
		ptr = alloc_fn(cpu, gi->nr_units * ai->unit_size, atom_size);
		if (!ptr) {
			rc = -ENOMEM;
			goto out_free_areas;
		}
		/* kmemleak tracks the percpu allocations separately */
		kmemleak_free(ptr);
		areas[group] = ptr;

/*
 * IAMROOT, 2022.01.15:
 * - base : 할당받은 unit memory주소에서 가장 낮은 주소를 가지고잇는걸 저장한다.
 * - highest_group : 가장 높은 주소의 unit memory를 가지고 있는 group 번호를 저장한다.
 */
		base = min(ptr, base);

		if (ptr > areas[highest_group])
			highest_group = group;
	}

/*
 * IAMROOT, 2022.01.15:
 * - max_distance
 *   제일 낮은 group unit memory start addr ~ 제일 높은 group unit memory 주소의 end addr
 */
	max_distance = areas[highest_group] - base;
	max_distance += ai->unit_size * ai->groups[highest_group].nr_units;

/*
 * IAMROOT, 2022.01.15:
 * - 후에 2nd, 3rd chuk를 생성할때 vmalloc공간에 mapping을 하는데 2nd는 1st를 만들었을때
 *   사용했던 offset size를 가지고 만들고, 3rd부터는 2nd에서 만들었던 공간에서 자라는
 *   방식으로 할당되기 때문에 여유공간이 필요하다. 그래서 어느정도 여유공간을 두는것이다.
 */
	/* warn if maximum distance is further than 75% of vmalloc space */
	if (max_distance > VMALLOC_TOTAL * 3 / 4) {
		pr_warn("max_distance=0x%lx too large for vmalloc space 0x%lx\n",
				max_distance, VMALLOC_TOTAL);
#ifdef CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK
		/* and fail if we have fallback */
		rc = -EINVAL;
		goto out_free_areas;
#endif
	}

	/*
	 * Copy data and free unused parts.  This should happen after all
	 * allocations are complete; otherwise, we may end up with
	 * overlapping groups.
	 */
/*
 * IAMROOT, 2022.01.15:
 * 1.사용하지 않은 cpu_map을 찾아 해당 unit memory를 해제한다.
 * 2. percpu section(static data)를 각 group의 unit마다 복사한다.
 * 3. 실제 사용하는 unit_size보다 많은 영역이 unit memory로 할당된경우 해당 영역을
 * free한다.
 */
	for (group = 0; group < ai->nr_groups; group++) {
		struct pcpu_group_info *gi = &ai->groups[group];
		void *ptr = areas[group];

		for (i = 0; i < gi->nr_units; i++, ptr += ai->unit_size) {
			if (gi->cpu_map[i] == NR_CPUS) {
				/* unused unit, free whole */
				free_fn(ptr, ai->unit_size);
				continue;
			}
			/* copy and return the unused part */
			memcpy(ptr, __per_cpu_load, ai->static_size);
			free_fn(ptr + size_sum, ai->unit_size - size_sum);
		}
	}

/*
 * IAMROOT, 2022.01.15:
 * - base_offset : 제일 낮은 unit memory의 주소부터 현재 group의 unit memory 주소의 offset
 */
	/* base address is now known, determine group base offsets */
	for (group = 0; group < ai->nr_groups; group++) {
		ai->groups[group].base_offset = areas[group] - base;
	}

	pr_info("Embedded %zu pages/cpu s%zu r%zu d%zu u%zu\n",
		PFN_DOWN(size_sum), ai->static_size, ai->reserved_size,
		ai->dyn_size, ai->unit_size);
/*
 * IAMROOT, 2022.01.18:
 * - 여기까지 memory map 정리
 * -- alloc_info는 cpu_map까지 연속된 memory를 가지며 groups에 각 cpu_map이
 * 연결되있다.
 * -- unit memory는 각 group마다 해당 node의 memory에 할당되있있다.
 *  각 unit 마다 static영역이 복사되었으며 사용하지 않은 영역은 free된상태.
 * -- group별 memory는base_offset을 통해서 groups에서 접근할수있는 구조이다.
 *
 * +--------------------------------------------------+
 * | ai | groups[nr_groups]    | cpu_map[nr_units]    |
 * |    | group | cpu ==========> cpu[.]              |
 * |    |       +-------------------------------------+
 * |    |       | base_offset  |                      |
 * +-----------------||--------+----------------------+           
 *                   ||
 * base + base_offset||     (nodeX units | unit [0] unit [1]..)  ^   
 *                   \\     ...                                  |   
 *                    ``==> (nodeX units | unit [0] unit [1]..)  | max_distance
 *                          ...                                  |
 *                          (node0 units | unit [0] unit [1]..)  v
 *                                                              base
 */
	pcpu_setup_first_chunk(ai, base);
	goto out_free;

out_free_areas:
	for (group = 0; group < ai->nr_groups; group++)
		if (areas[group])
			free_fn(areas[group],
				ai->groups[group].nr_units * ai->unit_size);
out_free:
	pcpu_free_alloc_info(ai);
	if (areas)
		memblock_free_early(__pa(areas), areas_size);
	return rc;
}
#endif /* BUILD_EMBED_FIRST_CHUNK */

#ifdef BUILD_PAGE_FIRST_CHUNK
/**
 * pcpu_page_first_chunk - map the first chunk using PAGE_SIZE pages
 * @reserved_size: the size of reserved percpu area in bytes
 * @alloc_fn: function to allocate percpu page, always called with PAGE_SIZE
 * @free_fn: function to free percpu page, always called with PAGE_SIZE
 * @populate_pte_fn: function to populate pte
 *
 * This is a helper to ease setting up page-remapped first percpu
 * chunk and can be called where pcpu_setup_first_chunk() is expected.
 *
 * This is the basic allocator.  Static percpu area is allocated
 * page-by-page into vmalloc area.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
int __init pcpu_page_first_chunk(size_t reserved_size,
				 pcpu_fc_alloc_fn_t alloc_fn,
				 pcpu_fc_free_fn_t free_fn,
				 pcpu_fc_populate_pte_fn_t populate_pte_fn)
{
	static struct vm_struct vm;
	struct pcpu_alloc_info *ai;
	char psize_str[16];
	int unit_pages;
	size_t pages_size;
	struct page **pages;
	int unit, i, j, rc = 0;
	int upa;
	int nr_g0_units;

	snprintf(psize_str, sizeof(psize_str), "%luK", PAGE_SIZE >> 10);

	ai = pcpu_build_alloc_info(reserved_size, 0, PAGE_SIZE, NULL);
	if (IS_ERR(ai))
		return PTR_ERR(ai);
	BUG_ON(ai->nr_groups != 1);
	upa = ai->alloc_size/ai->unit_size;
	nr_g0_units = roundup(num_possible_cpus(), upa);
	if (WARN_ON(ai->groups[0].nr_units != nr_g0_units)) {
		pcpu_free_alloc_info(ai);
		return -EINVAL;
	}

	unit_pages = ai->unit_size >> PAGE_SHIFT;

	/* unaligned allocations can't be freed, round up to page size */
	pages_size = PFN_ALIGN(unit_pages * num_possible_cpus() *
			       sizeof(pages[0]));
	pages = memblock_alloc(pages_size, SMP_CACHE_BYTES);
	if (!pages)
		panic("%s: Failed to allocate %zu bytes\n", __func__,
		      pages_size);

	/* allocate pages */
	j = 0;
	for (unit = 0; unit < num_possible_cpus(); unit++) {
		unsigned int cpu = ai->groups[0].cpu_map[unit];
		for (i = 0; i < unit_pages; i++) {
			void *ptr;

			ptr = alloc_fn(cpu, PAGE_SIZE, PAGE_SIZE);
			if (!ptr) {
				pr_warn("failed to allocate %s page for cpu%u\n",
						psize_str, cpu);
				goto enomem;
			}
			/* kmemleak tracks the percpu allocations separately */
			kmemleak_free(ptr);
			pages[j++] = virt_to_page(ptr);
		}
	}

	/* allocate vm area, map the pages and copy static data */
	vm.flags = VM_ALLOC;
	vm.size = num_possible_cpus() * ai->unit_size;
	vm_area_register_early(&vm, PAGE_SIZE);

	for (unit = 0; unit < num_possible_cpus(); unit++) {
		unsigned long unit_addr =
			(unsigned long)vm.addr + unit * ai->unit_size;

		for (i = 0; i < unit_pages; i++)
			populate_pte_fn(unit_addr + (i << PAGE_SHIFT));

		/* pte already populated, the following shouldn't fail */
		rc = __pcpu_map_pages(unit_addr, &pages[unit * unit_pages],
				      unit_pages);
		if (rc < 0)
			panic("failed to map percpu area, err=%d\n", rc);

		/*
		 * FIXME: Archs with virtual cache should flush local
		 * cache for the linear mapping here - something
		 * equivalent to flush_cache_vmap() on the local cpu.
		 * flush_cache_vmap() can't be used as most supporting
		 * data structures are not set up yet.
		 */

		/* copy static data */
		memcpy((void *)unit_addr, __per_cpu_load, ai->static_size);
	}

	/* we're ready, commit */
	pr_info("%d %s pages/cpu s%zu r%zu d%zu\n",
		unit_pages, psize_str, ai->static_size,
		ai->reserved_size, ai->dyn_size);

	pcpu_setup_first_chunk(ai, vm.addr);
	goto out_free_ar;

enomem:
	while (--j >= 0)
		free_fn(page_address(pages[j]), PAGE_SIZE);
	rc = -ENOMEM;
out_free_ar:
	memblock_free_early(__pa(pages), pages_size);
	pcpu_free_alloc_info(ai);
	return rc;
}
#endif /* BUILD_PAGE_FIRST_CHUNK */

#ifndef	CONFIG_HAVE_SETUP_PER_CPU_AREA
/*
 * Generic SMP percpu area setup.
 *
 * The embedding helper is used because its behavior closely resembles
 * the original non-dynamic generic percpu area setup.  This is
 * important because many archs have addressing restrictions and might
 * fail if the percpu area is located far away from the previous
 * location.  As an added bonus, in non-NUMA cases, embedding is
 * generally a good idea TLB-wise because percpu area can piggy back
 * on the physical linear memory mapping which uses large page
 * mappings on applicable archs.
 */
unsigned long __per_cpu_offset[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(__per_cpu_offset);

static void * __init pcpu_dfl_fc_alloc(unsigned int cpu, size_t size,
				       size_t align)
{
	return  memblock_alloc_from(size, align, __pa(MAX_DMA_ADDRESS));
}

static void __init pcpu_dfl_fc_free(void *ptr, size_t size)
{
	memblock_free_early(__pa(ptr), size);
}

void __init setup_per_cpu_areas(void)
{
	unsigned long delta;
	unsigned int cpu;
	int rc;

	/*
	 * Always reserve area for module percpu variables.  That's
	 * what the legacy allocator did.
	 */
	rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
				    PERCPU_DYNAMIC_RESERVE, PAGE_SIZE, NULL,
				    pcpu_dfl_fc_alloc, pcpu_dfl_fc_free);
	if (rc < 0)
		panic("Failed to initialize percpu areas.");

	delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
	for_each_possible_cpu(cpu)
		__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
#endif	/* CONFIG_HAVE_SETUP_PER_CPU_AREA */

#else	/* CONFIG_SMP */

/*
 * UP percpu area setup.
 *
 * UP always uses km-based percpu allocator with identity mapping.
 * Static percpu variables are indistinguishable from the usual static
 * variables and don't require any special preparation.
 */
void __init setup_per_cpu_areas(void)
{
	const size_t unit_size =
		roundup_pow_of_two(max_t(size_t, PCPU_MIN_UNIT_SIZE,
					 PERCPU_DYNAMIC_RESERVE));
	struct pcpu_alloc_info *ai;
	void *fc;

	ai = pcpu_alloc_alloc_info(1, 1);
	fc = memblock_alloc_from(unit_size, PAGE_SIZE, __pa(MAX_DMA_ADDRESS));
	if (!ai || !fc)
		panic("Failed to allocate memory for percpu areas.");
	/* kmemleak tracks the percpu allocations separately */
	kmemleak_free(fc);

	ai->dyn_size = unit_size;
	ai->unit_size = unit_size;
	ai->atom_size = unit_size;
	ai->alloc_size = unit_size;
	ai->groups[0].nr_units = 1;
	ai->groups[0].cpu_map[0] = 0;

	pcpu_setup_first_chunk(ai, fc);
	pcpu_free_alloc_info(ai);
}

#endif	/* CONFIG_SMP */

/*
 * pcpu_nr_pages - calculate total number of populated backing pages
 *
 * This reflects the number of pages populated to back chunks.  Metadata is
 * excluded in the number exposed in meminfo as the number of backing pages
 * scales with the number of cpus and can quickly outweigh the memory used for
 * metadata.  It also keeps this calculation nice and simple.
 *
 * RETURNS:
 * Total number of populated backing pages in use by the allocator.
 */
unsigned long pcpu_nr_pages(void)
{
	return pcpu_nr_populated * pcpu_nr_units;
}

/*
 * Percpu allocator is initialized early during boot when neither slab or
 * workqueue is available.  Plug async management until everything is up
 * and running.
 */
static int __init percpu_enable_async(void)
{
	pcpu_async_enabled = true;
	return 0;
}
subsys_initcall(percpu_enable_async);
