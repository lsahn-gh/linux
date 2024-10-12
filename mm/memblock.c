// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Procedures for maintaining information about logical memory blocks.
 *
 * Peter Bergner, IBM Corp.	June 2001.
 * Copyright (C) 2001 Peter Bergner.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/poison.h>
#include <linux/pfn.h>
#include <linux/debugfs.h>
#include <linux/kmemleak.h>
#include <linux/seq_file.h>
#include <linux/memblock.h>

#include <asm/sections.h>
#include <linux/io.h>

#include "internal.h"

#define INIT_MEMBLOCK_REGIONS			128
#define INIT_PHYSMEM_REGIONS			4

#ifndef INIT_MEMBLOCK_RESERVED_REGIONS
# define INIT_MEMBLOCK_RESERVED_REGIONS		INIT_MEMBLOCK_REGIONS
#endif

/**
 * DOC: memblock overview
 *
 * Memblock is a method of managing memory regions during the early
 * boot period when the usual kernel memory allocators are not up and
 * running.
 *
 * Memblock views the system memory as collections of contiguous
 * regions. There are several types of these collections:
 *
 * * ``memory`` - describes the physical memory available to the
 *   kernel; this may differ from the actual physical memory installed
 *   in the system, for instance when the memory is restricted with
 *   ``mem=`` command line parameter
 * * ``reserved`` - describes the regions that were allocated
 * * ``physmem`` - describes the actual physical memory available during
 *   boot regardless of the possible restrictions and memory hot(un)plug;
 *   the ``physmem`` type is only available on some architectures.
 *
 * Each region is represented by struct memblock_region that
 * defines the region extents, its attributes and NUMA node id on NUMA
 * systems. Every memory type is described by the struct memblock_type
 * which contains an array of memory regions along with
 * the allocator metadata. The "memory" and "reserved" types are nicely
 * wrapped with struct memblock. This structure is statically
 * initialized at build time. The region arrays are initially sized to
 * %INIT_MEMBLOCK_REGIONS for "memory" and %INIT_MEMBLOCK_RESERVED_REGIONS
 * for "reserved". The region array for "physmem" is initially sized to
 * %INIT_PHYSMEM_REGIONS.
 * The memblock_allow_resize() enables automatic resizing of the region
 * arrays during addition of new regions. This feature should be used
 * with care so that memory allocated for the region array will not
 * overlap with areas that should be reserved, for example initrd.
 *
 * The early architecture setup should tell memblock what the physical
 * memory layout is by using memblock_add() or memblock_add_node()
 * functions. The first function does not assign the region to a NUMA
 * node and it is appropriate for UMA systems. Yet, it is possible to
 * use it on NUMA systems as well and assign the region to a NUMA node
 * later in the setup process using memblock_set_node(). The
 * memblock_add_node() performs such an assignment directly.
 *
 * Once memblock is setup the memory can be allocated using one of the
 * API variants:
 *
 * * memblock_phys_alloc*() - these functions return the **physical**
 *   address of the allocated memory
 * * memblock_alloc*() - these functions return the **virtual** address
 *   of the allocated memory.
 *
 * Note, that both API variants use implicit assumptions about allowed
 * memory ranges and the fallback methods. Consult the documentation
 * of memblock_alloc_internal() and memblock_alloc_range_nid()
 * functions for more elaborate description.
 *
 * As the system boot progresses, the architecture specific mem_init()
 * function frees all the memory to the buddy page allocator.
 *
 * Unless an architecture enables %CONFIG_ARCH_KEEP_MEMBLOCK, the
 * memblock data structures (except "physmem") will be discarded after the
 * system initialization completes.
 */
/* IAMROOT, 2024.01.05:
 * - kernel boot-time에 먼저 활성화되는 memory allocator이며 다른 allocator가
 *   준비되기 전에 memory allocation이 필요한 경우에 사용한다.
 *   물리 메모리 범위를 미리 등록하여 가용 범위내에서 사용하게 되며
 *   정규 allocator가 사용되기 전이니 early memory allocator라 부르기도 한다.
 */

#ifndef CONFIG_NUMA
struct pglist_data __refdata contig_page_data;
EXPORT_SYMBOL(contig_page_data);
#endif

/*
 * IAMROOT, 2021.11.06:
 * - bootmem_init 에서 설정된다.
 * - max_pfn을 경우 제일 마지막 frame을 가리키는게 아니라, 마지막의 다음을 가리키고 있다.
 */
unsigned long max_low_pfn;
unsigned long min_low_pfn;
unsigned long max_pfn;
unsigned long long max_possible_pfn;

/* IAMROOT, 2024.01.05:
 * - memory-region
 *   사용할 물리 메모리 영역이 등록될 공간이며 compile-time에 128개 bucket으로
 *   초기화된다.
 *
 * - reserved-region
 *   사용중이거나 사용할 물리 메모리 영역이 등록될 공간이며 compile-time에
 *   128개 bucket으로 초기화된다.
 *
 * - physmem-region
 *   물리적으로 감지된 메모리 영역이 등록될 공간이며 등록된 후 수정되지 않는
 *   특성이 존재한다. 실제 물리 메모리 크기를 등록하여 사용되며 2014년에
 *   추가되었다.
 */
static struct memblock_region memblock_memory_init_regions[INIT_MEMBLOCK_REGIONS] __initdata_memblock;
static struct memblock_region memblock_reserved_init_regions[INIT_MEMBLOCK_RESERVED_REGIONS] __initdata_memblock;
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
static struct memblock_region memblock_physmem_init_regions[INIT_PHYSMEM_REGIONS];
#endif

/* IAMROOT, 2021.10.16:
 * - .meminit.data section에 위치한다.
 *
 * - cnt가 1인데, empty여도 1이고 1개여도 1이다. 0을 사용하면 안됨.
 * - .bottom_up: arm64에서는 top-down 방식 사용하므로 false로 설정.
 */
struct memblock memblock __initdata_memblock = {
	.memory.regions		= memblock_memory_init_regions,
	.memory.cnt		= 1,	/* empty dummy entry */
	.memory.max		= INIT_MEMBLOCK_REGIONS,
	.memory.name		= "memory",

	.reserved.regions	= memblock_reserved_init_regions,
	.reserved.cnt		= 1,	/* empty dummy entry */
	.reserved.max		= INIT_MEMBLOCK_RESERVED_REGIONS,
	.reserved.name		= "reserved",

	.bottom_up		= false,
	.current_limit		= MEMBLOCK_ALLOC_ANYWHERE,
};

#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
struct memblock_type physmem = {
	.regions		= memblock_physmem_init_regions,
	.cnt			= 1,	/* empty dummy entry */
	.max			= INIT_PHYSMEM_REGIONS,
	.name			= "physmem",
};
#endif

/*
 * keep a pointer to &memblock.memory in the text section to use it in
 * __next_mem_range() and its helpers.
 *  For architectures that do not keep memblock data after init, this
 * pointer will be reset to NULL at memblock_discard()
 */
static __refdata struct memblock_type *memblock_memory = &memblock.memory;

/* IAMROOT, 2021.10.16:
 * - @memblock_type에 저장된 region을 [0..nr]까지 모두 iteration 한다.
 *   @i는 loop에 사용되는 임시 변수이며 @rgn은 각 region의 pointer 이다.
 *
 * - memblock_type->cnt는 항상 1 이상이므로 loop를 최소 한번은 수행한다.
 */
#define for_each_memblock_type(i, memblock_type, rgn)			\
	for (i = 0, rgn = &memblock_type->regions[0];			\
	     i < memblock_type->cnt;					\
	     i++, rgn = &memblock_type->regions[i])

#define memblock_dbg(fmt, ...)						\
	do {								\
		if (memblock_debug)					\
			pr_info(fmt, ##__VA_ARGS__);			\
	} while (0)

static int memblock_debug __initdata_memblock;

/* IAMROOT, 2021.10.23:
 * - disk의 raid랑 비슷한 개념으로 똑같은 값을 mirror로 사용하는지의 여부
 *   mirror system을 사용할 경우 기존 데이터 * 2배가 필요하다.
 *
 *   mirror 운영 도중 error가 발생하면 error message를 출력한다.
 *   kernel만 mirror를 쓰고 application은 mirror를 쓰지 않을수도 있다.
 *   이 경우 kernel에서도 일부 메모리만 mirror를 쓸수있는데 어떤 memory를
 *   mirroring 할지 marking 해야한다.
 *
 * - Arm64에서는 아직 사용하지 않는다.
 */
static bool system_has_some_mirror __initdata_memblock = false;

/* IAMROOT, 2021.10.23:
 * - @memblock_can_resie:
 *   membloc size 확장 가능 여부. paging init때 memblock_alloc을
 *   사용할 수 있게 되면 값이 1이 된다.
 *
 * - @memblock_memory_in_slab:
 *   @memblock_reserved_in_slab:
 *   region 확장시 slab allocator 사용 여부.
 */
static int memblock_can_resize __initdata_memblock;
static int memblock_memory_in_slab __initdata_memblock = 0;
static int memblock_reserved_in_slab __initdata_memblock = 0;

static enum memblock_flags __init_memblock choose_memblock_flags(void)
{
	return system_has_some_mirror ? MEMBLOCK_MIRROR : MEMBLOCK_NONE;
}

/* IAMROOT, 2024.01.06:
 * - pa(@base + @size)가 PHYS_ADDR_MAX 보다 커지지 않도록 @size를 조정한다.
 *   (overflow 방지)
 */
/* adjust *@size so that (@base + *@size) doesn't overflow, return new size */
static inline phys_addr_t memblock_cap_size(phys_addr_t base, phys_addr_t *size)
{
	return *size = min(*size, PHYS_ADDR_MAX - base);
}

/*
 * Address comparison utilities
 */
static unsigned long __init_memblock memblock_addrs_overlap(phys_addr_t base1, phys_addr_t size1,
				       phys_addr_t base2, phys_addr_t size2)
{
	return ((base1 < (base2 + size2)) && (base2 < (base1 + size1)));
}

/* IAMROOT, 2024.10.01:
 * - @base, @size가 @type의 region들과 겹치는 부분이 있는지 확인한다.
 */
bool __init_memblock memblock_overlaps_region(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size)
{
	unsigned long i;

	memblock_cap_size(base, &size);

	for (i = 0; i < type->cnt; i++)
		if (memblock_addrs_overlap(base, size, type->regions[i].base,
					   type->regions[i].size))
			break;
	return i < type->cnt;
}

/**
 * __memblock_find_range_bottom_up - find free area utility in bottom-up
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Utility called from memblock_find_in_range_node(), find free area bottom-up.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
/* IAMROOT, 2021.10.23:
 * - arm64에서는 미사용. top down 방식만 사용한다.
 */
static phys_addr_t __init_memblock
__memblock_find_range_bottom_up(phys_addr_t start, phys_addr_t end,
				phys_addr_t size, phys_addr_t align, int nid,
				enum memblock_flags flags)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	for_each_free_mem_range(i, nid, flags, &this_start, &this_end, NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		cand = round_up(this_start, align);
		if (cand < this_end && this_end - cand >= size)
			return cand;
	}

	return 0;
}

/**
 * __memblock_find_range_top_down - find free area utility, in top-down
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Utility called from memblock_find_in_range_node(), find free area top-down.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
/* IAMROOT, 2021.10.23:
 * - @start 에서 @end 까지 regions을 탐색해서 @size 크기의 free region이
 *   존재하면 해당 pa(addr)을 반환한다.
 */
static phys_addr_t __init_memblock
__memblock_find_range_top_down(phys_addr_t start, phys_addr_t end,
			       phys_addr_t size, phys_addr_t align, int nid,
			       enum memblock_flags flags)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	/* IAMROOT, 2024.01.09:
	 * - 역(reverse)으로 탐색한다.
	 */
	for_each_free_mem_range_reverse(i, nid, flags, &this_start, &this_end,
					NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		/* IAMROOT, 2021.10.23:
		 * - 'this_start <= @size <= this_end' 의 조건을 만족하는 region을
		 *   찾아야 하므로 아래의 conditions을 기반으로 region을 탐색한다.
		 *
		 *   1) pa(this_end) < @size 라면 this rgn이 커버하는 영역을 초과하니
		 *      skip 한다.
		 *   2) pa(cand) >= pa(this_start) 라면 위 조건을 만족하는 영역이므로
		 *      @align 만큼 paddr을 정렬하고 반환한다.
		 */
		if (this_end < size)
			continue;

		cand = round_down(this_end - size, align);
		if (cand >= this_start)
			return cand;
	}

	return 0;
}

/**
 * memblock_find_in_range_node - find free area in given range and node
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Find @size free area aligned to @align in the specified range and node.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock memblock_find_in_range_node(phys_addr_t size,
					phys_addr_t align, phys_addr_t start,
					phys_addr_t end, int nid,
					enum memblock_flags flags)
{
	/* pump up @end */
	if (end == MEMBLOCK_ALLOC_ACCESSIBLE ||
	    end == MEMBLOCK_ALLOC_KASAN)
		end = memblock.current_limit;

	/* avoid allocating the first page */
	start = max_t(phys_addr_t, start, PAGE_SIZE);
	end = max(start, end);

	/* IAMROOT, 2021.10.23:
	 * - Arm64는 top_down 방식을 사용함.
	 */
	if (memblock_bottom_up())
		return __memblock_find_range_bottom_up(start, end, size, align,
						       nid, flags);
	else
		return __memblock_find_range_top_down(start, end, size, align,
						      nid, flags);
}

/**
 * memblock_find_in_range - find free area in given range
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 *
 * Find @size free area aligned to @align in the specified range.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
/* IAMROOT, 2021.10.23:
 * - @start 에서 @end 까지 regions을 탐색해서 @size 크기의 free region이
 *   존재하면 해당 pa(addr)을 반환한다.
 */
static phys_addr_t __init_memblock memblock_find_in_range(phys_addr_t start,
					phys_addr_t end, phys_addr_t size,
					phys_addr_t align)
{
	phys_addr_t ret;
	enum memblock_flags flags = choose_memblock_flags();

	/* IAMROOT, 2024.01.10:
	 * - 탐색 시작.
	 */
again:
	ret = memblock_find_in_range_node(size, align, start, end,
					    NUMA_NO_NODE, flags);

	/* IAMROOT, 2021.10.23:
	 * - @size만큼의 free rgn을 찾지 못하였으므로 MEMBLOCK_MIRROR flag를
	 *   off 하여 다시 탐색한다.
	 */
	if (!ret && (flags & MEMBLOCK_MIRROR)) {
		pr_warn("Could not allocate %pap bytes of mirrored memory\n",
			&size);
		flags &= ~MEMBLOCK_MIRROR;
		goto again;
	}

	return ret;
}

static void __init_memblock memblock_remove_region(struct memblock_type *type, unsigned long r)
{
	/* IAMROOT, 2021.10.23:
	 * - rgn[@r + 1]을 rgn[@r]로 옮기는 방식을 이용하여 rgn[@r]을 삭제한다.
	 */
	type->total_size -= type->regions[r].size;
	memmove(&type->regions[r], &type->regions[r + 1],
		(type->cnt - (r + 1)) * sizeof(type->regions[r]));
	type->cnt--;

	/* Special case for empty arrays */
	if (type->cnt == 0) {
		WARN_ON(type->total_size != 0);
		type->cnt = 1;
		type->regions[0].base = 0;
		type->regions[0].size = 0;
		type->regions[0].flags = 0;
		memblock_set_region_node(&type->regions[0], MAX_NUMNODES);
	}
}

#ifndef CONFIG_ARCH_KEEP_MEMBLOCK
/**
 * memblock_discard - discard memory and reserved arrays if they were allocated
 */
/*
 * IAMROOT, 2021.10.23: TODO
 * - memory를 buddy system으로 전환할때 관련된 함수.
 */
void __init memblock_discard(void)
{
	phys_addr_t addr, size;

	if (memblock.reserved.regions != memblock_reserved_init_regions) {
		addr = __pa(memblock.reserved.regions);
		size = PAGE_ALIGN(sizeof(struct memblock_region) *
				  memblock.reserved.max);
		__memblock_free_late(addr, size);
	}

	if (memblock.memory.regions != memblock_memory_init_regions) {
		addr = __pa(memblock.memory.regions);
		size = PAGE_ALIGN(sizeof(struct memblock_region) *
				  memblock.memory.max);
		__memblock_free_late(addr, size);
	}

	memblock_memory = NULL;
}
#endif

/**
 * memblock_double_array - double the size of the memblock regions array
 * @type: memblock type of the regions array being doubled
 * @new_area_start: starting address of memory range to avoid overlap with
 * @new_area_size: size of memory range to avoid overlap with
 *
 * Double the size of the @type regions array. If memblock is being used to
 * allocate memory for a new reserved regions array and there is a previously
 * allocated memory range [@new_area_start, @new_area_start + @new_area_size]
 * waiting to be reserved, ensure the memory used by the new array does
 * not overlap.
 *
 * Return:
 * 0 on success, -1 on failure.
 */
/* IAMROOT, 2024.01.06:
 * - memblock regions array pool이 찼으므로 확장을 시도한다.
 */
static int __init_memblock memblock_double_array(struct memblock_type *type,
						phys_addr_t new_area_start,
						phys_addr_t new_area_size)
{
	struct memblock_region *new_array, *old_array;
	phys_addr_t old_alloc_size, new_alloc_size;
	phys_addr_t old_size, new_size, addr, new_end;
	int use_slab = slab_is_available();
	int *in_slab;

	/* We don't allow resizing until we know about the reserved regions
	 * of memory that aren't suitable for allocation
	 */
	/* IAMROOT, 2021.10.23:
	 * - paging_init이 완료되기 전에는 동작하지 않는다.
	 */
	if (!memblock_can_resize)
		return -1;

	/* Calculate new doubled size */
	old_size = type->max * sizeof(struct memblock_region);
	new_size = old_size << 1;
	/*
	 * We need to allocated new one align to PAGE_SIZE,
	 *   so we can free them completely later.
	 */
	old_alloc_size = PAGE_ALIGN(old_size);
	new_alloc_size = PAGE_ALIGN(new_size);

	/* Retrieve the slab flag */
	if (type == &memblock.memory)
		in_slab = &memblock_memory_in_slab;
	else
		in_slab = &memblock_reserved_in_slab;

	/* Try to find some space for it */
	if (use_slab) {
		/* IAMROOT, 2024.01.09:
		 * - slab에서 allocation 가능하면 va(addr) -> pa(addr)로 변환한다.
		 */
		new_array = kmalloc(new_size, GFP_KERNEL);
		addr = new_array ? __pa(new_array) : 0;
	} else {
		/* only exclude range when trying to double reserved.regions */
		if (type != &memblock.reserved)
			new_area_start = new_area_size = 0;

		/* IAMROOT, 2024.01.09:
		 * - memblock에 이미 등록된 memory region에서 선택하여 2 배수로
		 *   확장한다.
		 */
		addr = memblock_find_in_range(new_area_start + new_area_size,
						memblock.current_limit,
						new_alloc_size, PAGE_SIZE);

		/* IAMROOT, 2021.10.23:
		 * - free rgn 탐색에 실패한 경우 마지막부터 바닥까지 재탐색한다.
		 */
		if (!addr && new_area_size)
			addr = memblock_find_in_range(0,
				min(new_area_start, memblock.current_limit),
				new_alloc_size, PAGE_SIZE);

		/* IAMROOT, 2024.01.11:
		 * - 탐색에 성공하면 pa(addr) -> va(addr)로 변환한다.
		 */
		new_array = addr ? __va(addr) : NULL;
	}
	/* IAMROOT, 2021.10.30:
	 * - 위 if-statement 가 완료된 후에 변수 new_array, addr는 아래와 같은
	 *   값을 가진다.
	 *
	 *   addr = pa(new_array)
	 *   new_array = va(addr)
	 *
	 *   slab을 통한 alloc은 va만 알고 있으므로 addr = pa(new_array)를 해주고,
	 *   memblock을 통한 alloc은 pa만 알고 있으므로 new_array = va(addr)를
	 *   한다.
	 */

	/* IAMROOT, 2024.01.11:
	 * - addr == null 라면 free rgn 탐색에 실패했으므로 -1을 반환한다.
	 */
	if (!addr) {
		pr_err("memblock: Failed to double %s array from %ld to %ld entries !\n",
		       type->name, type->max, type->max * 2);
		return -1;
	}

	new_end = addr + new_size - 1;
	memblock_dbg("memblock: %s is doubled to %ld at [%pa-%pa]",
			type->name, type->max * 2, &addr, &new_end);

	/*
	 * Found space, we now need to move the array over before we add the
	 * reserved region since it may be our reserved array itself that is
	 * full.
	 */
	/* IAMROOT, 2021.10.23:
	 * - old_size만큼 new_array에 복사하고 확장된 나머지 영역은 0으로 채운다.
	 */
	memcpy(new_array, type->regions, old_size);
	memset(new_array + type->max, 0, old_size);

	/* IAMROOT, 2024.01.11:
	 * - type->regions에 저장된 old_array와 new_array의 주소를 swap하고
	 *   최대 크기를 보정한다.
	 */
	old_array = type->regions;
	type->regions = new_array;
	type->max <<= 1;

	/* IAMROOT, 2021.10.23:
	 * - slab allocator가 동작중이면 kfree로 해제하고 그게 아니면
	 *   memblock_free로 해제한다.
	 *
	 *   다만 최초에는 static memblock regions을 사용하므로 해제하지 않지만
	 *   추후에 allocator에 의해 size double이 발생하면 그때서야 해제한다.
	 */
	/* Free old array. We needn't free it if the array is the static one */
	if (*in_slab)
		kfree(old_array);
	else if (old_array != memblock_memory_init_regions &&
		 old_array != memblock_reserved_init_regions)
		memblock_free_ptr(old_array, old_alloc_size);

	/*
	 * Reserve the new array if that comes from the memblock.  Otherwise, we
	 * needn't do it
	 */
	/* IAMROOT, 2021.10.23:
	 * - slab allocator를 사용하지 않으면 reserved region에 등록한다.
	 */
	if (!use_slab)
		BUG_ON(memblock_reserve(addr, new_alloc_size));

	/* Update slab flag */
	*in_slab = use_slab;

	return 0;
}

/**
 * memblock_merge_regions - merge neighboring compatible regions
 * @type: memblock type to scan
 *
 * Scan @type and merge neighboring compatible regions.
 */
static void __init_memblock memblock_merge_regions(struct memblock_type *type)
{
	int i = 0;

	/* IAMROOT, 2021.10.23:
	 *                +------+ end
	 *                |      |
	 *                | next |
	 *                |      |
	 *  end +------+  +------+ base
	 *      |      |
	 *      | this |
	 * base +------+
	 *
	 * this end와 next base가 동일, nid가 같으며 flag가 같으면 두개의 블럭을
     * 병합한다.
	 *
	 *      +--------+ end
	 *      |        |
	 *      | merged |
	 *      | this   |
	 *      |        |
	 *      |        |
	 *      |        |
	 * base +--------+
	 */
	/* cnt never goes below 1 */
	while (i < type->cnt - 1) {
		struct memblock_region *this = &type->regions[i];
		struct memblock_region *next = &type->regions[i + 1];

		/* IAMROOT, 2024.01.06:
		 * - 병합할 수 없는 조건인 경우 skip 한다.
		 */
		if (this->base + this->size != next->base ||
		    memblock_get_region_node(this) !=
		    memblock_get_region_node(next) ||
		    this->flags != next->flags) {
			BUG_ON(this->base + this->size > next->base);
			i++;
			continue;
		}

		/* IAMROOT, 2024.01.06:
		 * - 병합은 this region에 next의 size만 더하면 되므로 간단하다.
		 */
		this->size += next->size;

		/* IAMROOT, 2021.10.23:
		 * - 병합되었으므로 (next + 1) region을 next로 옮겨서 중간에 빈공간이
		 *   생기지 않도록 보정한다.
		 */
		/* move forward from next + 1, index of which is i + 2 */
		memmove(next, next + 1, (type->cnt - (i + 2)) * sizeof(*next));
		type->cnt--;
	}
}

/**
 * memblock_insert_region - insert new memblock region
 * @type:	memblock type to insert into
 * @idx:	index for the insertion point
 * @base:	base address of the new region
 * @size:	size of the new region
 * @nid:	node id of the new region
 * @flags:	flags of the new region
 *
 * Insert new memblock region [@base, @base + @size) into @type at @idx.
 * @type must already have extra room to accommodate the new region.
 */
/* IAMROOT, 2021.10.23:
 * - 기존 region[@idx .. cnt]까지의 blocks을 한칸씩 뒤로 이동시키고
 *   region[@idx]에 insert 한다.
 *
 *   +-----------+
 *   | max rgn   |
 *   | ...       |
 *   | rgn + cnt |
 *   |           |
 *   | rgn + idx |
 *   | ...       |
 *   | rgn + 3   |
 *   | rgn + 2   |
 *   | rgn + 1   |
 *   +-----------+
 */
static void __init_memblock memblock_insert_region(struct memblock_type *type,
						   int idx, phys_addr_t base,
						   phys_addr_t size,
						   int nid,
						   enum memblock_flags flags)
{
	struct memblock_region *rgn = &type->regions[idx];

	BUG_ON(type->cnt >= type->max);
	memmove(rgn + 1, rgn, (type->cnt - idx) * sizeof(*rgn));
	rgn->base = base;
	rgn->size = size;
	rgn->flags = flags;
	memblock_set_region_node(rgn, nid);
	type->cnt++;
	type->total_size += size;
}

/**
 * memblock_add_range - add new memblock region
 * @type: memblock type to add new region into
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 * @flags: flags of the new region
 *
 * Add new memblock region [@base, @base + @size) into @type.  The new region
 * is allowed to overlap with existing ones - overlaps don't affect already
 * existing regions.  @type is guaranteed to be minimal (all neighbouring
 * compatible regions are merged) after the addition.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
/* IAMROOT, 2021.10.16:
 * - @nid: 0, 1, 2, ... 같은 node id가 들어오는데 MAX_NUMNODES를
 *         사용하겟다는것은 어떤 노드든 (any node)상관없다는 의미와 같다.
 *         Non-NUMA 시스템에서 node id는 0으로 처리된다.
 */
static int __init_memblock memblock_add_range(struct memblock_type *type,
				phys_addr_t base, phys_addr_t size,
				int nid, enum memblock_flags flags)
{
	bool insert = false;
	phys_addr_t obase = base;
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int idx, nr_new;
	struct memblock_region *rgn;

	if (!size)
		return 0;

	/* IAMROOT, 2021.10.16:
	 * - memblock region(memory, reserve)이 비어있는 상태일때.
	 *   이 경우 특수케이스로 처리하는데 해당 region에 아무것도
	 *   없으므로 단순하게 추가하고 끝낸다.
	 *
	 * - 오버헤드(cnt 보정, memory 공간 중복시 merge 등) 없이 추가 후
	 *   바로 리턴 (fastpath).
	 */
	/* special case for empty array */
	if (type->regions[0].size == 0) {
		WARN_ON(type->cnt != 1 || type->total_size);
		type->regions[0].base = base;
		type->regions[0].size = size;
		type->regions[0].flags = flags;
		memblock_set_region_node(&type->regions[0], nid);
		type->total_size = size;
		return 0;
	}
repeat:
	/*
	 * The following is executed twice.  Once with %false @insert and
	 * then with %true.  The first counts the number of regions needed
	 * to accommodate the new area.  The second actually inserts them.
	 */
	base = obase;
	nr_new = 0;

	for_each_memblock_type(idx, type, rgn) {
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		/* IAMROOT, 2021.10.16:
		 * - region base가 @end 보다 크다면 종료.
		 *
		 *   pa(@base + @end) 범위를 @rgn - 1과 @rgn 사이에 insert 해야하고
		 *   다음 그림처럼 구성되어야 한다.
		 *   == (@rgn - 1) < rgn(@base + @end) < (@rgn)
		 *
		 *  rend +--------+
		 *       |        |
		 *       | region |
		 * rbase +--------+
		 *                  +-----+ end
		 *                  |     |
		 *                  | new |
		 *                  |     |
		 *                  +-----+ base
		 */
		if (rbase >= end)
			break;

		/*
		 * IAMROOT, 2021.10.23:
		 * - region end가 @base 보다 작다면 다음 region 탐색.
		 *
		 *   lower paddr가 region의 가장 아래에 위치하도록 설계되었으므로
		 *   rend <= @base 인 경우 계속 skip 하면서 @rbase >= @end인 region을
		 *   탐색한다.
		 *
		 *                  +-----+ end
		 *                  |     |
		 *                  | new |
		 *                  |     |
		 *                  +-----+ base
		 *  rend +--------+
		 *       |        |
		 *       | region |
		 * rbase +--------+
		 */
		if (rend <= base)
			continue;

		/*
		 * @rgn overlaps.  If it separates the lower part of new
		 * area, insert that portion.
		 */
		/* IAMROOT, 2021.10.16:
		 * - 여기서부터는 @rbase < rgn(@base or @end) < @rend 처럼 region이
		 *   겹치는 경우이므로 그에 맞게 추가 작업을 수행한다.
		 *
		 * - rbase, @base와 관련된 region 추가 작업.
		 *   region base > @base 라면 rgn(rbase - @base)만 새로 insert.
		 *
		 *     rend +--------+
		 *          |        | +-----+ end
		 *          | region | |     |
		 *    rbase +--------+ | new |    <--+--------
		 *                     |     |       | 추가하는 부분
		 *                     +-----+ base  +--------
		 *
		 * - rend, @end와 관련된 region 추가 작업.
		 *   region end < @end 라면 @base를 조정하여 rgn(@end - rend)를
		 *   새 region으로 만들고 insert.
		 *
		 *                     +-----+ end   ||                   +-----+ end
		 *                     |     |       ||                   | new |
		 *     rend +--------+ |     |       ||   rend +--------+ +-----+ (adj) base
		 *          |        | |     |       ||        |        | |     |
		 *          | region | |     |       ||        | region | |     |
		 *    rbase +--------+ | new |       ||  rbase +--------+ |     |
		 *                     |     |       ||                   |     |
		 *                     +-----+ base  ||                   +-----+ (prev) base
		 *
		 *   다만 @base가 보정되었으니 rgn + 1 과 비교를 하여 어느 @idx에
		 *   저장할지 다시 계산한다.
		 */
		if (rbase > base) {
#ifdef CONFIG_NUMA
			WARN_ON(nid != memblock_get_region_node(rgn));
#endif
			WARN_ON(flags != rgn->flags);
			nr_new++;
			if (insert)
				memblock_insert_region(type, idx++, base,
						       rbase - base, nid,
						       flags);
		}
		/* area below @rend is dealt with, forget about it */
		base = min(rend, end);
	}

	/* IAMROOT, 2021.10.23:
	 * - for문 종료후 'base < end' 조건이 참이라면 추가할 block이 있는 것이므로
	 *   해당 block 추가.
	 */
	/* insert the remaining portion */
	if (base < end) {
		nr_new++;
		if (insert)
			memblock_insert_region(type, idx, base, end - base,
					       nid, flags);
	}

	/* IAMROOT, 2021.10.23:
	 * - insert할게 없으면 그냥 종료
	 */
	if (!nr_new)
		return 0;

	/*
	 * If this was the first round, resize array and repeat for actual
	 * insertions; otherwise, merge and return.
	 */
	if (!insert) {
		/* IAMROOT, 2021.10.23:
		 * - 1st loop에서는 memblock region에 추가할 영역의 '갯수(nr_new)'를
		 *   계산하고 만약 '갯수 > max(128)' 라면 memblock region을 현재
		 *   크기에서 2배수로 확장시킨다.
		 *
		 * - 계산이 완료되면 'insert = true' flag를 설정하고 'goto repeat' 통해
		 *   2th loop를 수행하며 이때 요청한 region을 추가한다.
		 */
		while (type->cnt + nr_new > type->max)
			if (memblock_double_array(type, obase, size) < 0)
				return -ENOMEM;
		insert = true;
		goto repeat;
	} else {
		/* IAMROOT, 2021.10.25:
		 * - memblock region 전체를 iter하여 blocks 중에 인접 blocks을 merging
		 *   할 수 있다면 merging하여 1개의 block unit으로 만든다.
		 */
		memblock_merge_regions(type);
		return 0;
	}
}

/**
 * memblock_add_node - add new memblock region within a NUMA node
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 *
 * Add new memblock region [@base, @base + @size) to the "memory"
 * type. See memblock_add_range() description for mode details
 *
 * Return:
 * 0 on success, -errno on failure.
 */
/* IAMROOT, 2021.10.16:
 * - pa(@base)에서 시작하여 @size 크기의 물리 메모리를 memory region에
 *   추가하되 merge시 @nid를 참조하여 같을 때만 한다.
 */
int __init_memblock memblock_add_node(phys_addr_t base, phys_addr_t size,
				       int nid)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] nid=%d %pS\n", __func__,
		     &base, &end, nid, (void *)_RET_IP_);

	return memblock_add_range(&memblock.memory, base, size, nid, 0);
}

/**
 * memblock_add - add new memblock region
 * @base: base address of the new region
 * @size: size of the new region
 *
 * Add new memblock region [@base, @base + @size) to the "memory"
 * type. See memblock_add_range() description for mode details
 *
 * Return:
 * 0 on success, -errno on failure.
 */
/* IAMROOT, 2021.10.16:
 * - pa(@base)에서 시작하여 @size 크기의 물리 메모리를 memory region에 추가.
 */
int __init_memblock memblock_add(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	return memblock_add_range(&memblock.memory, base, size, MAX_NUMNODES, 0);
}

/**
 * memblock_isolate_range - isolate given range into disjoint memblocks
 * @type: memblock type to isolate range for
 * @base: base of range to isolate
 * @size: size of range to isolate
 * @start_rgn: out parameter for the start of isolated region
 * @end_rgn: out parameter for the end of isolated region
 *
 * Walk @type and ensure that regions don't cross the boundaries defined by
 * [@base, @base + @size).  Crossing regions are split at the boundaries,
 * which may create at most two more regions.  The index of the first
 * region inside the range is returned in *@start_rgn and end in *@end_rgn.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
/* IAMROOT, 2022.03.22:
 * - region[@base .. @size]을 후처리하기 위해 기존 @type region에서 조각낸다.
 *   기존 nr개 보다 더 많은 nr block들이 @type region에 생성된다.
 *
 *   out vars: @start_rgn, @end_rgn
 */
static int __init_memblock memblock_isolate_range(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size,
					int *start_rgn, int *end_rgn)
{
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int idx;
	struct memblock_region *rgn;

	*start_rgn = *end_rgn = 0;

	if (!size)
		return 0;

	/* IAMROOT, 2021.10.23:
	 * - region을 해제할 때도 파편화될 가능성이 있고 그로 인해 늘어날 수
	 *   있으므로 갯수를 계산하여 부족하면 region을 2 배수 늘린다.
	 */
	/* we'll create at most two more regions */
	while (type->cnt + 2 > type->max)
		if (memblock_double_array(type, base, size) < 0)
			return -ENOMEM;

	for_each_memblock_type(idx, type, rgn) {
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		/* IAMROOT, 2021.10.23:
		 * 1). rbase >= @end 조건.
		 *     이 경우 rgn과 iso(lated) region이 겹치지 않으므로 loop에서
		 *     나와 return 한다.
		 *
		 *    rend +-------+
		 *         |       |
		 *         | rgn   |
		 *         |       |
		 *   rbase +-------+
		 *                   +------+ end
		 *                   |      |
		 *                   | iso  |
		 *                   |      |
		 *                   +------+ base
		 *
		 */
		if (rbase >= end)
			break;

		/* IAMROOT, 2024.10.11:
		 * 2). rend <= @base 조건.
		 *     이 경우도 rgn과 iso region이 겹치지 않으나 rgn 보다 lower addr
		 *     범위에 존재하므로 그보다 큰 rgn + 1과 비교하기 위해 다음 loop로
		 *     넘어간다.
		 *
		 *                   +------+ end
		 *                   |      |
		 *                   | iso  |
		 *                   |      |
		 *                   +------+ base
		 *    rend +-------+
		 *         |       |
		 *         | rgn   |
		 *         |       |
		 *   rbase +-------+
		 */
		if (rend <= base)
			continue;

		/* IAMROOT, 2024.01.13:
		 * - 아래 예제로 다음 조건을 이해하자.
		 *   1). rbase < @base
		 *   2). rend > @end
		 *
		 *   higher addr
		 *   ...
		 *    rend +-------+
		 *         |       |
		 *         |       | +------+ end
		 *         |       | |      |
		 *         | rgn   | | iso  |
		 *         |       | |      |
		 *         |       | +------+ base
		 *         |       |
		 *   rbase +-------+
		 *   ...
		 *   lower addr
		 */
		if (rbase < base) {
			/* IAMROOT, 2021.10.16:
			 * - 'rbase < @base' 조건은 아래의 상황이다.
			 *
			 *   기존에 하나였던 @rgn block이 아래처럼 계산되어
			 *   파편화되어 2개로 나뉘고 @rgn은 보정을, iso는 추가된다.
			 *   단, rgn[@end .. @rend]은 여기서 처리하지 않는다.
			 *
			 *    rend +-------+
			 *         |       |
			 *         |       |
			 *         |       |
			 *         | rgn   |
			 *         |       |
			 *    base +-------+ +--- size - (base - rbase)
			 *         | iso   | | insert rgn
			 *   rbase +-------+ +---
			 */
			/*
			 * @rgn intersects from below.  Split and continue
			 * to process the next region - the new top half.
			 */
			rgn->base = base;
			rgn->size -= base - rbase;
			type->total_size -= base - rbase;
			memblock_insert_region(type, idx, rbase, base - rbase,
					       memblock_get_region_node(rgn),
					       rgn->flags);
		} else if (rend > end) {
			/* IAMROOT, 2021.10.23:
			 * - 'rend > end' 조건은 아래의 상황이다.
			 *
			 *   기존에 하나였던 @rgn block에 아래처럼 계산되어
			 *   @rgn은 보정을, iso는 새로 추가된다.
			 *   단, rgn[@rbase.. @base]은 여기서 처리하지 않는다.
			 *
			 *   Y = end - rbase
			 *
			 *    @Y +-------+
			 *       | rgn   |
			 *   end +-------+ +--- size - (end - rbase)
			 *       |       | |
			 *       | iso   | | insert rgn
			 *       |       | |
			 *       |       | |
			 *       |       | |
			 *       +-------+ +--- rbase
			 */
			/*
			 * @rgn intersects from above.  Split and redo the
			 * current region - the new bottom half.
			 */
			rgn->base = end;
			rgn->size -= end - rbase;
			type->total_size -= end - rbase;
			memblock_insert_region(type, idx--, rbase, end - rbase,
					       memblock_get_region_node(rgn),
					       rgn->flags);
		} else {
			/* IAMROOT, 2021.10.23:
			 * - @rgn에 완전히 iso region에 포함되는 경우이며 1개 이상의
			 *   regions이 포함될 수 있으므로 계속 recording 한다.
			 *   @end_rgn + 1은 iteration 도중 종료 조건을 효율적으로 찾기
			 *   위함이다.
			 *
			 *                 +-------+ end
			 * rend  +-------+ |       |
			 *       |       | |       |
			 *       | rgn   | | iso   |
			 *       |       | |       |
			 * rbase +-------+ |       |
			 *                 +-------+ base
			 */
			/* @rgn is fully contained, record it */
			if (!*end_rgn)
				*start_rgn = idx;
			*end_rgn = idx + 1;
		}
	}

	return 0;
}

/* IAMROOT, 2021.10.23:
 * - reserved region[@base .. @(base + size)]을 해제한다.
 */
static int __init_memblock memblock_remove_range(struct memblock_type *type,
					  phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	/* IAMROOT, 2024.01.16:
	 * - @start_rgn, @end_rgn이 record 되었다면 삭제한다.
	 */
	for (i = end_rgn - 1; i >= start_rgn; i--)
		memblock_remove_region(type, i);
	return 0;
}

/* IAMROOT, 2021.10.23:
 * - memory region[@base .. @end] 까지의 영역을 삭제한다.
 *
 *   실제 자주 호출되지 않는다. 산업용 장비에서 memory를
 *   물리적으로 제거(hotplug) 하거나 guest-OS가 할당받은 메모리를
 *   반환해야 하는 상황 등에서 호출된다.
 */
int __init_memblock memblock_remove(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	return memblock_remove_range(&memblock.memory, base, size);
}

/**
 * memblock_free_ptr - free boot memory allocation
 * @ptr: starting address of the  boot memory allocation
 * @size: size of the boot memory block in bytes
 *
 * Free boot memory block previously allocated by memblock_alloc_xx() API.
 * The freeing memory will not be released to the buddy allocator.
 */
/* IAMROOT, 2021.11.15:
 * - va(@ptr) -> pa(@ptr)로 변경하고 해당 region을 free 한다.
 */
void __init_memblock memblock_free_ptr(void *ptr, size_t size)
{
	if (ptr)
		memblock_free(__pa(ptr), size);
}

/**
 * memblock_free - free boot memory block
 * @base: phys starting address of the  boot memory block
 * @size: size of the boot memory block in bytes
 *
 * Free boot memory block previously allocated by memblock_alloc_xx() API.
 * The freeing memory will not be released to the buddy allocator.
 */
/* IAMROOT, 2021.10.23:
 * - @base region을 free 하여 reserved region에서 해제한다.
 */
int __init_memblock memblock_free(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	kmemleak_free_part_phys(base, size);
	return memblock_remove_range(&memblock.reserved, base, size);
}

/* IAMROOT, 2021.10.23:
 * - pa(@base)에서 시작하여 @size 크기의 물리 메모리를 reserved 영역에 추가.
 */
int __init_memblock memblock_reserve(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	return memblock_add_range(&memblock.reserved, base, size, MAX_NUMNODES, 0);
}

#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
int __init_memblock memblock_physmem_add(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	return memblock_add_range(&physmem, base, size, MAX_NUMNODES, 0);
}
#endif

/**
 * memblock_setclr_flag - set or clear flag for a memory region
 * @base: base address of the region
 * @size: size of the region
 * @set: set or clear the flag
 * @flag: the flag to update
 *
 * This function isolates region [@base, @base + @size), and sets/clears flag
 *
 * Return: 0 on success, -errno on failure.
 */
/*
 * IAMROOT, 2021.10.23: TODO
 * - isolate로 해당 범위에 대한 memblock을 선별한뒤 flag를 고치고
 *   merge가 가능한 memblock은 merge한다.
 */
static int __init_memblock memblock_setclr_flag(phys_addr_t base,
				phys_addr_t size, int set, int flag)
{
	struct memblock_type *type = &memblock.memory;
	int i, ret, start_rgn, end_rgn;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++) {
		struct memblock_region *r = &type->regions[i];

		if (set)
			r->flags |= flag;
		else
			r->flags &= ~flag;
	}

	memblock_merge_regions(type);
	return 0;
}

/**
 * memblock_mark_hotplug - Mark hotpluggable memory with flag MEMBLOCK_HOTPLUG.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
/* IAMROOT, 2022.02.19:
 * - region[@base .. (@base + @size)]까지 MEMBLOCK_HOTPLUG flag를 on 한다.
 */
int __init_memblock memblock_mark_hotplug(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 1, MEMBLOCK_HOTPLUG);
}

/**
 * memblock_clear_hotplug - Clear flag MEMBLOCK_HOTPLUG for a specified region.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
/* IAMROOT, 2022.02.19:
 * - region[@base .. (@base + @size)]까지 MEMBLOCK_HOTPLUG flag를 off 한다.
 */
int __init_memblock memblock_clear_hotplug(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 0, MEMBLOCK_HOTPLUG);
}

/**
 * memblock_mark_mirror - Mark mirrored memory with flag MEMBLOCK_MIRROR.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
/* IAMROOT, 2022.02.19:
 * - region[@base .. (@base + @size)]까지 MEMBLOCK_MIRROR flag를 on 한다.
 */
int __init_memblock memblock_mark_mirror(phys_addr_t base, phys_addr_t size)
{
	system_has_some_mirror = true;

	return memblock_setclr_flag(base, size, 1, MEMBLOCK_MIRROR);
}

/**
 * memblock_mark_nomap - Mark a memory region with flag MEMBLOCK_NOMAP.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * The memory regions marked with %MEMBLOCK_NOMAP will not be added to the
 * direct mapping of the physical memory. These regions will still be
 * covered by the memory map. The struct page representing NOMAP memory
 * frames in the memory map will be PageReserved()
 *
 * Note: if the memory being marked %MEMBLOCK_NOMAP was allocated from
 * memblock, the caller must inform kmemleak to ignore that memory
 *
 * Return: 0 on success, -errno on failure.
 */
/* IAMROOT, 2021.10.30:
 * - region[@base .. (@base + @size)]까지 MEMBLOCK_NOMAP flag를 on 한다.
 *
 *   NOMAP: mapping을 원하지 않을때 사용.
 */
int __init_memblock memblock_mark_nomap(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 1, MEMBLOCK_NOMAP);
}

/**
 * memblock_clear_nomap - Clear flag MEMBLOCK_NOMAP for a specified region.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
/* IAMROOT, 2021.10.30:
 * - region[@base .. (@base + @size)]까지 MEMBLOCK_NOMAP flag를 off 한다.
 */
int __init_memblock memblock_clear_nomap(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 0, MEMBLOCK_NOMAP);
}

static bool should_skip_region(struct memblock_type *type,
			       struct memblock_region *m,
			       int nid, int flags)
{
	int m_nid = memblock_get_region_node(m);

	/* we never skip regions when iterating memblock.reserved or physmem */
	if (type != memblock_memory)
		return false;

	/* IAMROOT, 2024.01.10:
	 * - memory region만 아래 conditions 확인함.
	 */

	/* only memory regions are associated with nodes, check it */
	if (nid != NUMA_NO_NODE && nid != m_nid)
		return true;

	/* skip hotpluggable memory regions if needed */
	if (movable_node_is_enabled() && memblock_is_hotpluggable(m) &&
	    !(flags & MEMBLOCK_HOTPLUG))
		return true;

	/* if we want mirror memory skip non-mirror memory regions */
	if ((flags & MEMBLOCK_MIRROR) && !memblock_is_mirror(m))
		return true;

	/* skip nomap memory unless we were asked for it explicitly */
	if (!(flags & MEMBLOCK_NOMAP) && memblock_is_nomap(m))
		return true;

	/* IAMROOT, 2024.01.10:
	 * - 위 조건이 아닌 memory region은 모두 skip 하지 않음.
	 */
	return false;
}

/**
 * __next_mem_range - next function for for_each_free_mem_range() etc.
 * @idx: pointer to u64 loop variable
 * @nid: node selector, %NUMA_NO_NODE for all nodes
 * @flags: pick from blocks based on memory attributes
 * @type_a: pointer to memblock_type from where the range is taken
 * @type_b: pointer to memblock_type which excludes memory from being taken
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Find the first area from *@idx which matches @nid, fill the out
 * parameters, and update *@idx for the next iteration.  The lower 32bit of
 * *@idx contains index into type_a and the upper 32bit indexes the
 * areas before each region in type_b.	For example, if type_b regions
 * look like the following,
 *
 *	0:[0-16), 1:[32-48), 2:[128-130)
 *
 * The upper 32bit indexes the following regions.
 *
 *	0:[0-0), 1:[16-32), 2:[48-128), 3:[130-MAX)
 *
 * As both region arrays are sorted, the function advances the two indices
 * in lockstep and returns each intersection.
 */
/* IAMROOT, 2021.10.23:
 * - region[@type_a && !@type_b]의 영역을 구한다.
 *   다만, arm64에서는 기본적으로 reverse로 탐색한다.
 */
void __next_mem_range(u64 *idx, int nid, enum memblock_flags flags,
		      struct memblock_type *type_a,
		      struct memblock_type *type_b, phys_addr_t *out_start,
		      phys_addr_t *out_end, int *out_nid)
{
	/* IAMROOT, 2021.10.23:
	 * - 이 for문에서 idx 0 ~ 31bit는 @type_a에 대한 idx,
	 *   32 ~ 63bit는 @type_b에 대한 idx로 사용한다.
	 *
	 * - @type_a는 memory region, @type_b는 reserved region이다.
	 */
	int idx_a = *idx & 0xffffffff;
	int idx_b = *idx >> 32;

	if (WARN_ONCE(nid == MAX_NUMNODES,
	"Usage of MAX_NUMNODES is deprecated. Use NUMA_NO_NODE instead\n"))
		nid = NUMA_NO_NODE;

	/* IAMROOT, 2021.10.23:
	 * - @type_a(memory region)에 대해 탐색을 수행한다.
	 */
	for (; idx_a < type_a->cnt; idx_a++) {
		struct memblock_region *m = &type_a->regions[idx_a];

		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;
		int	    m_nid = memblock_get_region_node(m);

		if (should_skip_region(type_a, m, nid, flags))
			continue;
		/* IAMROOT, 2024.01.10:
		 * - 아래 조건이면 skip 하지 않음.
		 *   1) reserved, physmem regions 인 경우.
		 *   2) memory region이면서 @nid와 region nid가 동일.
		 *   3) memory region이면서 mirror, nomap flag == off.
		 */

		/* IAMROOT, 2021.10.23:
		 * - @type_b가 null이면 @type_a의 비교군이 없으므로 out vars를
		 *   설정하고 return한다.
		 */
		if (!type_b) {
			if (out_start)
				*out_start = m_start;
			if (out_end)
				*out_end = m_end;
			if (out_nid)
				*out_nid = m_nid;
			idx_a++;
			*idx = (u32)idx_a | (u64)idx_b << 32;
			return;
		}

		/* IAMROOT, 2021.10.23:
		 * - @type_b(reserved region)에 대해 탐색을 수행한다.
		 */
		/* scan areas before each reservation */
		for (; idx_b < type_b->cnt + 1; idx_b++) {
			struct memblock_region *r;
			phys_addr_t r_start;
			phys_addr_t r_end;

			/* IAMROOT, 2021.10.23:
			 * - reserved rgn[b]와 rgb[b-1] 사이의 free region을 탐색한다.
			 *
			 *   higher addr
			 *      ...
			 *   +----------+
			 *   |          |
			 *   | r(b)     |
			 *   |          |
			 *   +----------+  +----- >> r_end OR PHYS_ADDR_MAX
			 *                 |
			 *                 | free rgn
			 *                 |
			 *   +----------+  +----- >> r_start
			 *   |          |
			 *   | r(b-1)   |
			 *   |          |
			 *   +----------+
			 *      ...
			 *   lower addr
			 *
			 *   어떠한 영역도 사용되지 않은 최초의 경우에는 reserved region에
			 *   아무것도 없는데 그럴때는 PHYS_ADDR_MAX 값을 사용한다.
			 */
			r = &type_b->regions[idx_b];
			r_start = idx_b ? r[-1].base + r[-1].size : 0;
			r_end = idx_b < type_b->cnt ? r->base : PHYS_ADDR_MAX;

			/*
			 * if idx_b advanced past idx_a,
			 * break out to advance idx_a
			 */
			if (r_start >= m_end)
				break;
			/* if the two regions intersect, we're done */
			if (m_start < r_end) {
				if (out_start)
					*out_start =
						max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = m_nid;
				/*
				 * The region which ends first is
				 * advanced for the next iteration.
				 */
				if (m_end <= r_end)
					idx_a++;
				else
					idx_b++;
				*idx = (u32)idx_a | (u64)idx_b << 32;
				return;
			}
		}
	}

	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

/**
 * __next_mem_range_rev - generic next function for for_each_*_range_rev()
 *
 * @idx: pointer to u64 loop variable
 * @nid: node selector, %NUMA_NO_NODE for all nodes
 * @flags: pick from blocks based on memory attributes
 * @type_a: pointer to memblock_type from where the range is taken
 * @type_b: pointer to memblock_type which excludes memory from being taken
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Finds the next range from type_a which is not marked as unsuitable
 * in type_b.
 *
 * Reverse of __next_mem_range().
 */
/* IAMROOT, 2021.10.23:
 * - @type_a(memory)에서 free region을 찾되 @type_b(reserved)와 overlap 되면
 *   무시한다. 결국 가용 region을 탐색하여 반환하는게 목적이다.
 *
 *   out vars: free region의 @start, @end, @nid
 */
void __init_memblock __next_mem_range_rev(u64 *idx, int nid,
					  enum memblock_flags flags,
					  struct memblock_type *type_a,
					  struct memblock_type *type_b,
					  phys_addr_t *out_start,
					  phys_addr_t *out_end, int *out_nid)
{
	/* IAMROOT, 2021.10.23:
	 * - 이 for문에서 idx 0 ~ 31bit는 @type_a에 대한 idx,
	 *   32 ~ 63bit는 @type_b에 대한 idx로 사용한다.
	 *
	 * - @type_a는 memory region, @type_b는 reserved region이다.
	 */
	int idx_a = *idx & 0xffffffff;
	int idx_b = *idx >> 32;

	if (WARN_ONCE(nid == MAX_NUMNODES, "Usage of MAX_NUMNODES is deprecated. Use NUMA_NO_NODE instead\n"))
		nid = NUMA_NO_NODE;

	/* IAMROOT, 2021.10.23:
	 * - 첫 진입시 @idx는 ULLONG_MAX이므로 @idx_a, @idx_b를 초기화한다.
	 *
	 *   @type_b가 null이 아니면 reserved region도 loop 수행해야 하므로
	 *   @idx_b를 @type_b->cnt로 설정하고 그게 아니라면 사용하지 않는 의미로
	 *   0을 대입한다.
	 *
	 * - @type_a는 memory region으로 실제 존재하는 영역으로 사용되므로
	 *   @type_a->cnt - 1(실제 마지막 영역)을 @idx_a에 대입하고
	 *   @type_b(reserved)의 경우 reserved의 반대편(free)부터 검색하므로
	 *   마지막 블럭의 다음 공간이 필요하기 때문에 -1을 사용할 필요가 없다.
	 */
	if (*idx == (u64)ULLONG_MAX) {
		idx_a = type_a->cnt - 1;
		if (type_b != NULL)
			idx_b = type_b->cnt;
		else
			idx_b = 0;
	}

	/* IAMROOT, 2021.10.23:
	 * - @type_a(memory region)에 대해 reverse 탐색을 수행한다.
	 */
	for (; idx_a >= 0; idx_a--) {
		struct memblock_region *m = &type_a->regions[idx_a];

		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;
		int m_nid = memblock_get_region_node(m);

		if (should_skip_region(type_a, m, nid, flags))
			continue;
		/* IAMROOT, 2024.01.10:
		 * - 아래 조건이면 skip 하지 않음.
		 *   1) reserved, physmem regions 인 경우.
		 *   2) memory region이면서 @nid와 region nid가 동일.
		 *   3) memory region이면서 mirror, nomap flag == off.
		 */

		/* IAMROOT, 2021.10.23:
		 * - @type_b가 null이면 @type_a의 비교군이 없으므로 out vars를
		 *   설정하고 return한다.
		 */
		if (!type_b) {
			if (out_start)
				*out_start = m_start;
			if (out_end)
				*out_end = m_end;
			if (out_nid)
				*out_nid = m_nid;
			idx_a--;
			*idx = (u32)idx_a | (u64)idx_b << 32;
			return;
		}

		/* IAMROOT, 2021.10.23:
		 * - @type_b(reserved region)에 대해 reverse 탐색을 수행한다.
		 */
		/* scan areas before each reservation */
		for (; idx_b >= 0; idx_b--) {
			struct memblock_region *r;
			phys_addr_t r_start;
			phys_addr_t r_end;

			/* IAMROOT, 2021.10.23:
			 * - reserved rgn[b]와 rgb[b-1] 사이의 free region을 탐색한다.
			 *
			 *   higher addr
			 *      ...
			 *   +----------+
			 *   |          |
			 *   | r(b)     |
			 *   |          |
			 *   +----------+  +----- >> r_end OR PHYS_ADDR_MAX
			 *                 |
			 *                 | free rgn
			 *                 |
			 *   +----------+  +----- >> r_start
			 *   |          |
			 *   | r(b-1)   |
			 *   |          |
			 *   +----------+
			 *      ...
			 *   lower addr
			 *
			 *   어떠한 영역도 사용되지 않은 최초의 경우에는 reserved region에
			 *   아무것도 없는데 그럴때는 PHYS_ADDR_MAX 값을 사용한다.
			 */
			r = &type_b->regions[idx_b];
			r_start = idx_b ? r[-1].base + r[-1].size : 0;
			r_end = idx_b < type_b->cnt ? r->base : PHYS_ADDR_MAX;
			/*
			 * if idx_b advanced past idx_a,
			 * break out to advance idx_a
			 */
			/* IAMROOT, 2024.01.11:
			 * - 결국 r(b) .. r(b-1) 사이의 free rgn(@r_start .. @r_end)을
			 *   찾았다는 의미이고 이제 memory region과 비교하여 사용 가능한지
			 *   확인한다.
			 */

			/* IAMROOT, 2021.10.23:
			 * - memory region(@m_start .. @m_end) 범위를 벗어났다는 것은
			 *   사용할 수 없음을 의미하므로 다음 memory region을 탐색한다.
			 *
			 *   m_end    +---------+
			 *            |         |
			 *            | memory  |
			 *            |         |
			 *   m_start  +---------+  +----- >> r_end
			 *                         |
			 *                         | free rgn
			 *                         |
			 *                         +----- >> r_start
			 */
			if (r_end <= m_start)
				break;

			/* IAMROOT, 2021.10.23:
			 * - @r_end > @m_start 조건은 다음을 의미한다.
			 *
			 *   m_end    +---------+
			 *            |         |
			 *            | memory  |  +----- >> r_end
			 *            |         |  |
			 *   m_start  +---------+  | free rgn
			 *                         |
			 *                         +----- >> r_start
			 *
			 * - max(m_start, r_start)를 통해 @out_start를 보정하고,
			 *   min(m_end, r_end)를 통해 @out_end를 보정하면 다음과 같다.
			 *
			 *   m_end    +---------+
			 *            |         |
			 *            | memory  |  +----- >> r_end   << -- out_end
			 *            |         |  |
			 *   m_start  +---------+  | free rgn        << -- out_start
			 *                         |
			 *                         +----- >> r_start
			 *
			 *   결국 memory region과 free region이 교차하는 사용 가능한
			 *   region을 찾아낸 것이고 이 범위를 @out_start, @out_end에
			 *   저장한다.
			 */
			/* if the two regions intersect, we're done */
			if (m_end > r_start) {
				if (out_start)
					*out_start = max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = m_nid;

				/* IAMROOT, 2024.01.11:
				 * - loop를 수행하면서 전체 region을 모두 탐색해야 하므로
				 *   다음에 어느 region을 비교할 건지 계산한다.
				 *
				 *   m_start >= r_start 라면 memory region(idx_a)를 보정하고,
				 *   m_start < r_start 라면 reserved region(idx_b)를 보정한다.
				 *
				 *   m_start >= r_start 인 경우.
				 *                          ...
				 *   m_end    +---------+  | rsv   |
				 *            | memory  |  +-------+ r_end <-- out_end
				 *            | 탐색됨  |
				 *            | 탐색됨  |
				 *   m_start  +---------+  <----------------- out_start
				 *
				 *                         +-------+ r_start
				 *                         | rsv   |
				 *                          ...
				 *
				 *
				 *   m_start < r_start 인 경우.
				 *   해당 free rgn에서 memory 탐색할게 없으므로 다음
				 *   free rgn을 찾기 위해 reserved rgn[idx_b - 1]로 이동한다.
				 *                          ...
				 *   m_end    +---------+  | rsv   |
				 *            |         |  +-------+ r_end   <-- out_end
				 *            | memory  |
				 *            | 탐색됨  |    free
				 *            |         |  +-------+ r_start <-- out_start
				 *            |         |  | rsv   |
				 *   m_start  +---------+   ...
				 */
				if (m_start >= r_start)
					idx_a--;
				else
					idx_b--;

				/* IAMROOT, 2024.01.11:
				 * - 보정한 a, b index 정보를 @idx에 저장하고 return 하여
				 *   free rgn[@out_start .. @out_end] 범위를 사용할 수 있는지
				 *   확인하고 다음 loop를 수행할지 여부를 결정한다.
				 */
				*idx = (u32)idx_a | (u64)idx_b << 32;
				return;
			}
		}
	}

	/* IAMROOT, 2024.01.11:
	 * - 여기까지 오면 free rgn을 못 찾은 것이다.
	 */
	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

/*
 * Common iterator interface used to define for_each_mem_pfn_range().
 */
void __init_memblock __next_mem_pfn_range(int *idx, int nid,
				unsigned long *out_start_pfn,
				unsigned long *out_end_pfn, int *out_nid)
{
	struct memblock_type *type = &memblock.memory;
	struct memblock_region *r;
	int r_nid;

	while (++*idx < type->cnt) {
		r = &type->regions[*idx];
		r_nid = memblock_get_region_node(r);

		if (PFN_UP(r->base) >= PFN_DOWN(r->base + r->size))
			continue;

		/* IAMROOT, 2024.06.02:
		 * - PFN_UP(r->base) < PFN_DOWN(r->base + r->size)이고 @nid가 아래
		 *   케이스 중 하나라면 loop에서 빠져나옴.
		 *
		 *   1). @nid가 MAX_NUMNODES라면 node에 상관없이 탐색 의미.
		 *   2). @nid가 r_nid와 같은 경우. (특정 node 탐색)
		 */
		if (nid == MAX_NUMNODES || nid == r_nid)
			break;
	}

	/* IAMROOT, 2024.06.02:
	 * - 원하는 range의 region을 찾지 못하거나 phys memory 사용량이 full인
	 *   경우 그냥 return.
	 */
	if (*idx >= type->cnt) {
		*idx = -1;
		return;
	}

	if (out_start_pfn)
		*out_start_pfn = PFN_UP(r->base);
	if (out_end_pfn)
		*out_end_pfn = PFN_DOWN(r->base + r->size);
	if (out_nid)
		*out_nid = r_nid;
}

/**
 * memblock_set_node - set node ID on memblock regions
 * @base: base of area to set node ID for
 * @size: size of area to set node ID for
 * @type: memblock type to set node ID for
 * @nid: node ID to set
 *
 * Set the nid of memblock @type regions in [@base, @base + @size) to @nid.
 * Regions which cross the area boundaries are split as necessary.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
/* IAMROOT, 2021.10.23:
 * - region[@base .. (@base + size)] 영역의 nid를 @nid로 변경 후 merge.
 */
int __init_memblock memblock_set_node(phys_addr_t base, phys_addr_t size,
				      struct memblock_type *type, int nid)
{
#ifdef CONFIG_NUMA
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++)
		memblock_set_region_node(&type->regions[i], nid);

	memblock_merge_regions(type);
#endif
	return 0;
}

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
/**
 * __next_mem_pfn_range_in_zone - iterator for for_each_*_range_in_zone()
 *
 * @idx: pointer to u64 loop variable
 * @zone: zone in which all of the memory blocks reside
 * @out_spfn: ptr to ulong for start pfn of the range, can be %NULL
 * @out_epfn: ptr to ulong for end pfn of the range, can be %NULL
 *
 * This function is meant to be a zone/pfn specific wrapper for the
 * for_each_mem_range type iterators. Specifically they are used in the
 * deferred memory init routines and as such we were duplicating much of
 * this logic throughout the code. So instead of having it in multiple
 * locations it seemed like it would make more sense to centralize this to
 * one new iterator that does everything they need.
 */
void __init_memblock
__next_mem_pfn_range_in_zone(u64 *idx, struct zone *zone,
			     unsigned long *out_spfn, unsigned long *out_epfn)
{
	int zone_nid = zone_to_nid(zone);
	phys_addr_t spa, epa;
	int nid;

	__next_mem_range(idx, zone_nid, MEMBLOCK_NONE,
			 &memblock.memory, &memblock.reserved,
			 &spa, &epa, &nid);

	while (*idx != U64_MAX) {
		unsigned long epfn = PFN_DOWN(epa);
		unsigned long spfn = PFN_UP(spa);

		/*
		 * Verify the end is at least past the start of the zone and
		 * that we have at least one PFN to initialize.
		 */
		if (zone->zone_start_pfn < epfn && spfn < epfn) {
			/* if we went too far just stop searching */
			if (zone_end_pfn(zone) <= spfn) {
				*idx = U64_MAX;
				break;
			}

			if (out_spfn)
				*out_spfn = max(zone->zone_start_pfn, spfn);
			if (out_epfn)
				*out_epfn = min(zone_end_pfn(zone), epfn);

			return;
		}

		__next_mem_range(idx, zone_nid, MEMBLOCK_NONE,
				 &memblock.memory, &memblock.reserved,
				 &spa, &epa, &nid);
	}

	/* signal end of iteration */
	if (out_spfn)
		*out_spfn = ULONG_MAX;
	if (out_epfn)
		*out_epfn = 0;
}

#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

/**
 * memblock_alloc_range_nid - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @start: the lower bound of the memory region to allocate (phys address)
 * @end: the upper bound of the memory region to allocate (phys address)
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @exact_nid: control the allocation fall back to other nodes
 *
 * The allocation is performed from memory region limited by
 * memblock.current_limit if @end == %MEMBLOCK_ALLOC_ACCESSIBLE.
 *
 * If the specified node can not hold the requested memory and @exact_nid
 * is false, the allocation falls back to any node in the system.
 *
 * For systems with memory mirroring, the allocation is attempted first
 * from the regions with mirroring enabled and then retried from any
 * memory region.
 *
 * In addition, function sets the min_count to 0 using kmemleak_alloc_phys for
 * allocated boot memory block, so that it is never reported as leaks.
 *
 * Return:
 * Physical address of allocated memory block on success, %0 on failure.
 */
/* IAMROOT, 2021.10.23:
 * - 다음 args에 올 수 있는 값들.
 *   - @start:
 *     MEMBLOCK_LOW_LIMIT: 0
 *
 *   - @end:
 *     MEMBLOCK_ALLOC_ACCESSIBLE: 0
 *                                (current_limit 사용 의미)
 *     MEMBLOCK_ALLOC_KASAN     : 1
 *     MEMBLOCK_ALLOC_ANYWHERE  : max
 */
phys_addr_t __init memblock_alloc_range_nid(phys_addr_t size,
					phys_addr_t align, phys_addr_t start,
					phys_addr_t end, int nid,
					bool exact_nid)
{
	/* IAMROOT, 2021.10.23:
	 * - 시스템이 half OR full mirror로 운영중인지 확인한다.
	 */
	enum memblock_flags flags = choose_memblock_flags();
	phys_addr_t found;

	if (WARN_ONCE(nid == MAX_NUMNODES, "Usage of MAX_NUMNODES is deprecated. Use NUMA_NO_NODE instead\n"))
		nid = NUMA_NO_NODE;

	if (!align) {
		/* Can't use WARNs this early in boot on powerpc */
		dump_stack();
		align = SMP_CACHE_BYTES;
	}

again:
	found = memblock_find_in_range_node(size, align, start, end, nid,
					    flags);

	/* IAMROOT, 2021.10.23:
	 * - 할당할 수 있는 region을 찾고, reserved region에 추가하고 난 뒤
	 *   done label로 점프한다.
	 */
	if (found && !memblock_reserve(found, size))
		goto done;

	/* IAMROOT, 2021.10.23:
	 * - @nid가 지정되어 있고 @exact_nid == false라면 nid를 ANY node로
	 *   변경해서 다시 탐색한다.
	 */
	if (nid != NUMA_NO_NODE && !exact_nid) {
		found = memblock_find_in_range_node(size, align, start,
						    end, NUMA_NO_NODE,
						    flags);
		if (found && !memblock_reserve(found, size))
			goto done;
	}

	/* IAMROOT, 2021.10.23:
	 * - flags에 MEMBLOCK_MIRROR가 설정되어 있다면 off해서 다시 시도한다.
	 */
	if (flags & MEMBLOCK_MIRROR) {
		flags &= ~MEMBLOCK_MIRROR;
		pr_warn("Could not allocate %pap bytes of mirrored memory\n",
			&size);
		goto again;
	}

	/* IAMROOT, 2021.10.23:
	 * - 실패
	 */
	return 0;

done:
	/* IAMROOT, 2021.10.23:
	 * - kasan_init()에서 호출된 경우라면 kmemleak_.. 호출을 skip 한다.
	 */
	/* Skip kmemleak for kasan_init() due to high volume. */
	if (end != MEMBLOCK_ALLOC_KASAN)
		/*
		 * The min_count is set to 0 so that memblock allocated
		 * blocks are never reported as leaks. This is because many
		 * of these blocks are only referred via the physical
		 * address which is not looked up by kmemleak.
		 */
		kmemleak_alloc_phys(found, size, 0, 0);

	/* IAMROOT, 2024.01.20:
	 * - 성공
	 */
	return found;
}

/**
 * memblock_phys_alloc_range - allocate a memory block inside specified range
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @start: the lower bound of the memory region to allocate (physical address)
 * @end: the upper bound of the memory region to allocate (physical address)
 *
 * Allocate @size bytes in the between @start and @end.
 *
 * Return: physical address of the allocated memory block on success,
 * %0 on failure.
 */
/* IAMROOT, 2021.10.23:
 * - ANY node로 하여 memory region에서 alloc을 시도한다.
 */
phys_addr_t __init memblock_phys_alloc_range(phys_addr_t size,
					     phys_addr_t align,
					     phys_addr_t start,
					     phys_addr_t end)
{
	memblock_dbg("%s: %llu bytes align=0x%llx from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, &start, &end,
		     (void *)_RET_IP_);
	return memblock_alloc_range_nid(size, align, start, end, NUMA_NO_NODE,
					false);
}

/**
 * memblock_phys_alloc_try_nid - allocate a memory block from specified NUMA node
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Allocates memory block from the specified NUMA node. If the node
 * has no available memory, attempts to allocated from any node in the
 * system.
 *
 * Return: physical address of the allocated memory block on success,
 * %0 on failure.
 */
/* IAMROOT, 2021.10.23:
 * - memory region에서 @nid과 같은 region에서 alloc을 시도하고 실패시
 *   ANY node 하여 다시 시도한다.
 */
phys_addr_t __init memblock_phys_alloc_try_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	return memblock_alloc_range_nid(size, align, 0,
					MEMBLOCK_ALLOC_ACCESSIBLE, nid, false);
}

/**
 * memblock_alloc_internal - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region to allocate (phys address)
 * @max_addr: the upper bound of the memory region to allocate (phys address)
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @exact_nid: control the allocation fall back to other nodes
 *
 * Allocates memory block using memblock_alloc_range_nid() and
 * converts the returned physical address to virtual.
 *
 * The @min_addr limit is dropped if it can not be satisfied and the allocation
 * will fall back to memory below @min_addr. Other constraints, such
 * as node and mirrored memory will be handled again in
 * memblock_alloc_range_nid().
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
/* IAMROOT, 2021.10.23:
 * - @exact_nid: 해당 nid와 일치하는 node에서 region을 검색한다.
 */
static void * __init memblock_alloc_internal(
				phys_addr_t size, phys_addr_t align,
				phys_addr_t min_addr, phys_addr_t max_addr,
				int nid, bool exact_nid)
{
	phys_addr_t alloc;

	/*
	 * Detect any accidental use of these APIs after slab is ready, as at
	 * this moment memblock may be deinitialized already and its
	 * internal data may be destroyed (after execution of memblock_free_all)
	 */
	/* IAMROOT, 2021.10.23:
	 * - slab allocator 사용 가능해지면 정규 매핑에서 할당한다.
	 */
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, nid);

	/* IAMROOT, 2024.01.20:
	 * - @max_addr이 current_limit 보다 높다면 @max_addr을 보정한다.
	 */
	if (max_addr > memblock.current_limit)
		max_addr = memblock.current_limit;

	alloc = memblock_alloc_range_nid(size, align, min_addr, max_addr, nid,
					exact_nid);

	/* IAMROOT, 2024.01.20:
	 * - @min_addr 범위에서 alloc을 실패하면 0으로 설정하여 다시 시도한다.
	 */
	/* retry allocation without lower limit */
	if (!alloc && min_addr)
		alloc = memblock_alloc_range_nid(size, align, 0, max_addr, nid,
						exact_nid);

	if (!alloc)
		return NULL;

	/* IAMROOT, 2021.10.23:
	 * - memblock_alloc_* API가 호출되는 경우는 paging_init이 끝난 경우이므로
	 *   pa(alloc)을 vaddr로 변경하여 return 한다.
	 */
	return phys_to_virt(alloc);
}

/**
 * memblock_alloc_exact_nid_raw - allocate boot memory block on the exact node
 * without zeroing memory
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %MEMBLOCK_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. Does not zero allocated memory.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
/* IAMROOT, 2024.01.20:
 * - memory region에서 alloc 수행할 때 @nid과 동일한 region에서만 시도한다.
 */
void * __init memblock_alloc_exact_nid_raw(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);

	return memblock_alloc_internal(size, align, min_addr, max_addr, nid,
				       true);
}

/**
 * memblock_alloc_try_nid_raw - allocate boot memory block without zeroing
 * memory and without panicking
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %MEMBLOCK_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. Does not zero allocated memory, does not panic if request
 * cannot be satisfied.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
/* IAMROOT, 2024.01.20:
 * - memory region에서 alloc 수행할 때 @nid과 동일한 region에서 우선 시도하고
 *   실패시 ANY node에서 다시 시도한다.
 */
void * __init memblock_alloc_try_nid_raw(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);

	return memblock_alloc_internal(size, align, min_addr, max_addr, nid,
				       false);
}

/**
 * memblock_alloc_try_nid - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %MEMBLOCK_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. This function zeroes the allocated memory.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
/* IAMROOT, 2024.01.20:
 * - memory region에서 alloc 수행할 때 @nid과 동일한 region에서 우선 시도하고
 *   실패시 ANY node에서 다시 시도한다.
 *   alloc 성공시 해당 메모리 영역의 값을 0으로 초기화한다.
 */
void * __init memblock_alloc_try_nid(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	void *ptr;

	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);

	/* IAMROOT, 2021.10.23:
	 * - kernel에서 pointer는 대부분 vaddr 이다.
	 *   결국 지금 시점에서 allocated memory는 linear mapping 이다.
	 */
	ptr = memblock_alloc_internal(size, align,
					   min_addr, max_addr, nid, false);
	if (ptr)
		memset(ptr, 0, size);

	return ptr;
}

/**
 * __memblock_free_late - free pages directly to buddy allocator
 * @base: phys starting address of the  boot memory block
 * @size: size of the boot memory block in bytes
 *
 * This is only useful when the memblock allocator has already been torn
 * down, but we are still initializing the system.  Pages are released directly
 * to the buddy allocator.
 */
void __init __memblock_free_late(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t cursor, end;

	end = base + size - 1;
	memblock_dbg("%s: [%pa-%pa] %pS\n",
		     __func__, &base, &end, (void *)_RET_IP_);
	kmemleak_free_part_phys(base, size);
	cursor = PFN_UP(base);
	end = PFN_DOWN(base + size);

	for (; cursor < end; cursor++) {
		memblock_free_pages(pfn_to_page(cursor), cursor, 0);
		totalram_pages_inc();
	}
}

/*
 * Remaining API functions
 */

phys_addr_t __init_memblock memblock_phys_mem_size(void)
{
	return memblock.memory.total_size;
}

phys_addr_t __init_memblock memblock_reserved_size(void)
{
	return memblock.reserved.total_size;
}

/* lowest address */
/* IAMROOT, 2021.10.25:
 * - memory region에 등록된 regions 중에서 가장 낮은 paddr을 반환한다.
 *   memory region은 사용 가능한 물리 메모리가 등록되어 있으므로
 *   DRAM의 start addr 이기도 하다.
 */
phys_addr_t __init_memblock memblock_start_of_DRAM(void)
{
	return memblock.memory.regions[0].base;
}

/* IAMROOT, 2024.01.17:
 * - memory region에 등록된 regions 중에서 가장 높은 paddr을 반환한다.
 *   memroy region은 사용 가능한 물리 메모리가 등록되어 있으므로
 *   DRAM의 end addr 이기도 하다.
 */
phys_addr_t __init_memblock memblock_end_of_DRAM(void)
{
	int idx = memblock.memory.cnt - 1;

	return (memblock.memory.regions[idx].base + memblock.memory.regions[idx].size);
}

/* IAMROOT, 2024.02.15:
 * - memory region에 등록된 regions에 대해 loop를 수행하며 'max_addr'을 구한다.
 *
 *   region[0]을 시작으로 for each region (@limit - r->size)을 계산하여
 *   누적하다가 (@limit <= r->size)인 condition을 만나면 (r->base + @limit)을
 *   계산하여 최종 값을 반환한다.
 *
 *   그림을 그려서 계산하면 이해하기 쉽다.
 */
static phys_addr_t __init_memblock __find_max_addr(phys_addr_t limit)
{
	phys_addr_t max_addr = PHYS_ADDR_MAX;
	struct memblock_region *r;

	/*
	 * translate the memory @limit size into the max address within one of
	 * the memory memblock regions, if the @limit exceeds the total size
	 * of those regions, max_addr will keep original value PHYS_ADDR_MAX
	 */
	for_each_mem_region(r) {
		if (limit <= r->size) {
			max_addr = r->base + limit;
			break;
		}
		limit -= r->size;
	}

	return max_addr;
}

void __init memblock_enforce_memory_limit(phys_addr_t limit)
{
	phys_addr_t max_addr;

	if (!limit)
		return;

	max_addr = __find_max_addr(limit);

	/* @limit exceeds the total size of the memory, do nothing */
	if (max_addr == PHYS_ADDR_MAX)
		return;

	/* truncate both memory and reserved regions */
	memblock_remove_range(&memblock.memory, max_addr,
			      PHYS_ADDR_MAX);
	memblock_remove_range(&memblock.reserved, max_addr,
			      PHYS_ADDR_MAX);
}

/* IAMROOT, 2021.10.23:
 * - pa(@base .. (@base + @size)) 범위외의 모든 memory/reserved regions을
 *   삭제한다.
 */
void __init memblock_cap_memory_range(phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	if (!size)
		return;

	if (!memblock_memory->total_size) {
		pr_warn("%s: No memory registered yet\n", __func__);
		return;
	}

	ret = memblock_isolate_range(&memblock.memory, base, size,
						&start_rgn, &end_rgn);
	if (ret)
		return;

	/* remove all the MAP regions */
	for (i = memblock.memory.cnt - 1; i >= end_rgn; i--)
		if (!memblock_is_nomap(&memblock.memory.regions[i]))
			memblock_remove_region(&memblock.memory, i);

	for (i = start_rgn - 1; i >= 0; i--)
		if (!memblock_is_nomap(&memblock.memory.regions[i]))
			memblock_remove_region(&memblock.memory, i);

	/* truncate the reserved regions */
	memblock_remove_range(&memblock.reserved, 0, base);
	memblock_remove_range(&memblock.reserved,
			base + size, PHYS_ADDR_MAX);
}

/* IAMROOT, 2024.02.16:
 * - @limit을 기반으로 max_addr을 구하여 memory/reserved region[0 .. max_addr]을
 *   제외한 모든 memblock region을 삭제한다.
 */
void __init memblock_mem_limit_remove_map(phys_addr_t limit)
{
	phys_addr_t max_addr;

	if (!limit)
		return;

	max_addr = __find_max_addr(limit);

	/* @limit exceeds the total size of the memory, do nothing */
	if (max_addr == PHYS_ADDR_MAX)
		return;

	memblock_cap_memory_range(0, max_addr);
}

/* IAMROOT, 2021.10.23:
 * - @type region에 @addr block이 존재하는지 binary search 한다.
 */
static int __init_memblock memblock_search(struct memblock_type *type, phys_addr_t addr)
{
	unsigned int left = 0, right = type->cnt;

	do {
		unsigned int mid = (right + left) / 2;

		if (addr < type->regions[mid].base)
			right = mid;
		else if (addr >= (type->regions[mid].base +
				  type->regions[mid].size))
			left = mid + 1;
		else
			return mid;
	} while (left < right);
	return -1;
}

bool __init_memblock memblock_is_reserved(phys_addr_t addr)
{
	return memblock_search(&memblock.reserved, addr) != -1;
}

bool __init_memblock memblock_is_memory(phys_addr_t addr)
{
	return memblock_search(&memblock.memory, addr) != -1;
}

/*
 * IAMROOT, 2022.07.16:
 * - @addr가 memory regions에 이미 mapping 되어 있는지 확인한다.
 *   만약 region 탐색에 성공하면 다시한번 NOMAP flag == on인지 확인한다.
 */
bool __init_memblock memblock_is_map_memory(phys_addr_t addr)
{
	int i = memblock_search(&memblock.memory, addr);

	if (i == -1)
		return false;
	return !memblock_is_nomap(&memblock.memory.regions[i]);
}

int __init_memblock memblock_search_pfn_nid(unsigned long pfn,
			 unsigned long *start_pfn, unsigned long *end_pfn)
{
	struct memblock_type *type = &memblock.memory;
	int mid = memblock_search(type, PFN_PHYS(pfn));

	if (mid == -1)
		return -1;

	*start_pfn = PFN_DOWN(type->regions[mid].base);
	*end_pfn = PFN_DOWN(type->regions[mid].base + type->regions[mid].size);

	return memblock_get_region_node(&type->regions[mid]);
}

/**
 * memblock_is_region_memory - check if a region is a subset of memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base + @size) is a subset of a memory block.
 *
 * Return:
 * 0 if false, non-zero if true
 */
bool __init_memblock memblock_is_region_memory(phys_addr_t base, phys_addr_t size)
{
	int idx = memblock_search(&memblock.memory, base);
	phys_addr_t end = base + memblock_cap_size(base, &size);

	if (idx == -1)
		return false;
	return (memblock.memory.regions[idx].base +
		 memblock.memory.regions[idx].size) >= end;
}

/**
 * memblock_is_region_reserved - check if a region intersects reserved memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base + @size) intersects a reserved
 * memory block.
 *
 * Return:
 * True if they intersect, false if not.
 */
/* IAMROOT, 2021.10.23:
 * - region[@base .. (@base + @size)] 요청범위가 reserved region과
 *   겹치는지/사용중인지 확인한다.
 */
bool __init_memblock memblock_is_region_reserved(phys_addr_t base, phys_addr_t size)
{
	return memblock_overlaps_region(&memblock.reserved, base, size);
}

/* IAMROOT, 2021.10.23:
 * - memory region에 등록된 모든 block을 @align 단위로 정렬한다.
 */
void __init_memblock memblock_trim_memory(phys_addr_t align)
{
	phys_addr_t start, end, orig_start, orig_end;
	struct memblock_region *r;

	for_each_mem_region(r) {
		orig_start = r->base;
		orig_end = r->base + r->size;
		start = round_up(orig_start, align);
		end = round_down(orig_end, align);

		if (start == orig_start && end == orig_end)
			continue;

		if (start < end) {
			r->base = start;
			r->size = end - start;
		} else {
			memblock_remove_region(&memblock.memory,
					       r - memblock.memory.regions);
			r--;
		}
	}
}

void __init_memblock memblock_set_current_limit(phys_addr_t limit)
{
	memblock.current_limit = limit;
}

phys_addr_t __init_memblock memblock_get_current_limit(void)
{
	return memblock.current_limit;
}

static void __init_memblock memblock_dump(struct memblock_type *type)
{
	phys_addr_t base, end, size;
	enum memblock_flags flags;
	int idx;
	struct memblock_region *rgn;

	pr_info(" %s.cnt  = 0x%lx\n", type->name, type->cnt);

	for_each_memblock_type(idx, type, rgn) {
		char nid_buf[32] = "";

		base = rgn->base;
		size = rgn->size;
		end = base + size - 1;
		flags = rgn->flags;
#ifdef CONFIG_NUMA
		if (memblock_get_region_node(rgn) != MAX_NUMNODES)
			snprintf(nid_buf, sizeof(nid_buf), " on node %d",
				 memblock_get_region_node(rgn));
#endif
		pr_info(" %s[%#x]\t[%pa-%pa], %pa bytes%s flags: %#x\n",
			type->name, idx, &base, &end, &size, nid_buf, flags);
	}
}

static void __init_memblock __memblock_dump_all(void)
{
	pr_info("MEMBLOCK configuration:\n");
	pr_info(" memory size = %pa reserved size = %pa\n",
		&memblock.memory.total_size,
		&memblock.reserved.total_size);

	memblock_dump(&memblock.memory);
	memblock_dump(&memblock.reserved);
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
	memblock_dump(&physmem);
#endif
}

/* IAMROOT, 2021.10.23:
 * - bootarg에 memblock=debug param이 지정된 상태에서 동작하며 bootmem_init
 *   마지막 stage에서 호출된다.
 */
void __init_memblock memblock_dump_all(void)
{
	if (memblock_debug)
		__memblock_dump_all();
}

/* IAMROOT, 2021.10.23:
 * - paging init이 완료되고나서 memblock_alloc_xx API 사용이 가능해지면
 *   @memblock_can_resize 변수를 1로 설정한다.
 */
void __init memblock_allow_resize(void)
{
	memblock_can_resize = 1;
}

/* IAMROOT, 2021.10.23:
 * - bootarg에 memblock=debug와 같이 입력되면 debug를 on한다.
 */
static int __init early_memblock(char *p)
{
	if (p && strstr(p, "debug"))
		memblock_debug = 1;
	return 0;
}
early_param("memblock", early_memblock);

static void __init free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	phys_addr_t pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and round start upwards and end
	 * downwards.
	 */
	pg = PAGE_ALIGN(__pa(start_pg));
	pgend = __pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these, free the section of the
	 * memmap array.
	 */
	if (pg < pgend)
		memblock_free(pg, pgend - pg);
}

/*
 * The mem_map array can get very big.  Free the unused area of the memory map.
 */
static void __init free_unused_memmap(void)
{
	unsigned long start, end, prev_end = 0;
	int i;
/*
 * IAMROOT, 2022.02.19:
 * - 이미 memmap에서 사용하지 않은것은 free시켰었다.
 */
	if (!IS_ENABLED(CONFIG_HAVE_ARCH_PFN_VALID) ||
	    IS_ENABLED(CONFIG_SPARSEMEM_VMEMMAP))
		return;

	/*
	 * This relies on each bank being in address order.
	 * The banks are sorted previously in bootmem_init().
	 */
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, NULL) {
#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist
		 * due to SPARSEMEM sections which aren't present.
		 */
		start = min(start, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
		/*
		 * Align down here since many operations in VM subsystem
		 * presume that there are no holes in the memory map inside
		 * a pageblock
		 */
		start = round_down(start, pageblock_nr_pages);

		/*
		 * If we had a previous bank, and there is a space
		 * between the current bank and the previous, free it.
		 */
		if (prev_end && prev_end < start)
			free_memmap(prev_end, start);

		/*
		 * Align up here since many operations in VM subsystem
		 * presume that there are no holes in the memory map inside
		 * a pageblock
		 */
		prev_end = ALIGN(end, pageblock_nr_pages);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_end, PAGES_PER_SECTION)) {
		prev_end = ALIGN(end, pageblock_nr_pages);
		free_memmap(prev_end, ALIGN(prev_end, PAGES_PER_SECTION));
	}
#endif
}

/*
 * IAMROOT, 2022.02.19:
 * @start start pfn
 * @end end pfn
 *
 * memblock의 free 영역(@start ~ @end)을 order 단위의 align에 맞춰 
 * 각 order의 freelist[]로 등록한다.
 */
static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
	int order;

	while (start < end) {
/*
 * IAMROOT, 2022.02.19:
 * - start에 맞는 order를 구한다.
 * ex) start = 0x123 , end = 0x223, size = 1mb
 *    __ffs  order--횟수   start 변화
 * 1: 0     | 0          | 0x124      | 4k
 * 2: 2     | 0          | 0x128      | 16k
 * 3: 3     | 0          | 0x130      | 32k
 * 4: 4     | 0          | 0x140      | 64k
 * 5: 6     | 0          | 0x180      | 256k
 * 6: 7     | 0          | 0x200      | 512k
 * 7: 9     | 4(9->5)    | 0x220      | 128k
 * 8: 5     | 4(5->1)    | 0x222      | 8k
 * 9: 1     | 1(1->0)    | 0x223      | 4k
 */
		order = min(MAX_ORDER - 1UL, __ffs(start));

		while (start + (1UL << order) > end)
			order--;

		memblock_free_pages(pfn_to_page(start), start, order);

		start += (1UL << order);
	}
}

/*
 * IAMROOT, 2022.02.26: 
 * @start ~ @end 까지의 주소 범위를 모두 buddy 시스템으로 등록한다.
 */
static unsigned long __init __free_memory_core(phys_addr_t start,
				 phys_addr_t end)
{
	unsigned long start_pfn = PFN_UP(start);
	unsigned long end_pfn = min_t(unsigned long,
				      PFN_DOWN(end), max_low_pfn);

	if (start_pfn >= end_pfn)
		return 0;

	__free_pages_memory(start_pfn, end_pfn);

	return end_pfn - start_pfn;
}

/*
 * IAMROOT, 2022.02.19:
 * - defer된 reserved 영역의 page를 초기화한다.
 */
static void __init memmap_init_reserved_pages(void)
{
	struct memblock_region *region;
	phys_addr_t start, end;
	u64 i;

/*
 * IAMROOT, 2022.02.19:
 * - mapping된 reserved 영역에 대해서 defer된 page를 찾아 초기화 해준다.
 */
	/* initialize struct pages for the reserved regions */
	for_each_reserved_mem_range(i, &start, &end)
		reserve_bootmem_region(start, end);

	/* and also treat struct pages for the NOMAP regions as PageReserved */
/*
 * IAMROOT, 2022.02.19:
 * - mapping이 안된 reserved 영역에 대해서 defer된 page를 찾아 초기화를 해준다.
 */
	for_each_mem_region(region) {
		if (memblock_is_nomap(region)) {
			start = region->base;
			end = start + region->size;
			reserve_bootmem_region(start, end);
		}
	}
}

/*
 * IAMROOT, 2022.03.22:
 * - memblock에서 reserve인 영역의 page를 reserved로 설정한다.
 * - reserved되있지 않은 free memblock memory를 buddy system에 등록한다.
 */
static unsigned long __init free_low_memory_core_early(void)
{
	unsigned long count = 0;
	phys_addr_t start, end;
	u64 i;

/*
 * IAMROOT, 2022.02.19:
 * - 모든 memblock의 MEMBLOCK_HOTPLUG를 clear해준다.
 * ---
 * -전범위에 대해서 memblock_isolate_range를 수행하게 된다. 이 경우 딱히
 *  region이 나눠지는 일 없이 start_rgn, end_rgn만 구해지게 되고 순회하면서
 *  hotplug flag가 clear 되다가 같은 type끼리 merge가 될것이다.
 */
	memblock_clear_hotplug(0, -1);

	memmap_init_reserved_pages();

	/*
	 * We need to use NUMA_NO_NODE instead of NODE_DATA(0)->node_id
	 *  because in some case like Node0 doesn't have RAM installed
	 *  low ram will be on Node1
	 */
/*
 * IAMROOT, 2022.02.19:
 * - 모든 reserved되 있지 않은 free memblock memory에 대해서
 *   __free_memory_core를 실행한다.
 */
	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end,
				NULL)
		count += __free_memory_core(start, end);

	return count;
}

static int reset_managed_pages_done __initdata;

/*
 * IAMROOT, 2022.02.26: 
 * 요청한 노드에 소속한 모든 존의 managed_paged 값을 0으로 리셋한다.
 */
void reset_node_managed_pages(pg_data_t *pgdat)
{
	struct zone *z;

	for (z = pgdat->node_zones; z < pgdat->node_zones + MAX_NR_ZONES; z++)
		atomic_long_set(&z->managed_pages, 0);
}

/*
 * IAMROOT, 2022.02.19:
 * - 모든 online node의 각 zone에 대해서 managed_pages를 0으로 초기화해준다.
 */
void __init reset_all_zones_managed_pages(void)
{
	struct pglist_data *pgdat;

	if (reset_managed_pages_done)
		return;

	for_each_online_pgdat(pgdat)
		reset_node_managed_pages(pgdat);

	reset_managed_pages_done = 1;
}

/**
 * memblock_free_all - release free pages to the buddy allocator
 */
/*
 * IAMROOT, 2022.02.26: 
 * memblock의 reserved 영역을 제외한 free 페이지들을 모두 버디시스템에 등록한다.
 */
void __init memblock_free_all(void)
{
	unsigned long pages;

	free_unused_memmap();
	reset_all_zones_managed_pages();

	pages = free_low_memory_core_early();
	totalram_pages_add(pages);
}

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_ARCH_KEEP_MEMBLOCK)

/* IAMROOT, 2021.10.23:
 *
 * - kernel의 debugfs config를 on하여 /sys/kernel/debug을 생성하여
 *   user 영역에서 memblock 정보를 확인 할 수있다.
 */
static int memblock_debug_show(struct seq_file *m, void *private)
{
	struct memblock_type *type = m->private;
	struct memblock_region *reg;
	int i;
	phys_addr_t end;

	for (i = 0; i < type->cnt; i++) {
		reg = &type->regions[i];
		end = reg->base + reg->size - 1;

		seq_printf(m, "%4d: ", i);
		seq_printf(m, "%pa..%pa\n", &reg->base, &end);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(memblock_debug);

static int __init memblock_init_debugfs(void)
{
	struct dentry *root = debugfs_create_dir("memblock", NULL);

	debugfs_create_file("memory", 0444, root,
			    &memblock.memory, &memblock_debug_fops);
	debugfs_create_file("reserved", 0444, root,
			    &memblock.reserved, &memblock_debug_fops);
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
	debugfs_create_file("physmem", 0444, root, &physmem,
			    &memblock_debug_fops);
#endif

	return 0;
}
__initcall(memblock_init_debugfs);

#endif /* CONFIG_DEBUG_FS */
