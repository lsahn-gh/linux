// SPDX-License-Identifier: GPL-2.0
/*
 * sparse memory mappings.
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mmzone.h>
#include <linux/memblock.h>
#include <linux/compiler.h>
#include <linux/highmem.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/bootmem_info.h>

#include "internal.h"
#include <asm/dma.h>

/*
 * Permanent SPARSEMEM data:
 *
 * 1) mem_section	- memory sections, mem_map's for valid memory
 */
#ifdef CONFIG_SPARSEMEM_EXTREME
struct mem_section **mem_section;
#else
struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT]
	____cacheline_internodealigned_in_smp;
#endif
EXPORT_SYMBOL(mem_section);

#ifdef NODE_NOT_IN_PAGE_FLAGS
/*
 * If we did not store the node number in the page then we have to
 * do a lookup in the section_to_node_table in order to find which
 * node the page belongs to.
 */
#if MAX_NUMNODES <= 256
/*
 * IAMROOT, 2021.11.30:
 *  해당 mem_secion이 무슨 nid인지 바로 알아내기 위한 용도.
 *  이 변수를 통해 section nr만알고있으면 바로 nid를 구할수있다.
 */
static u8 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#else
static u16 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#endif

int page_to_nid(const struct page *page)
{
	return section_to_node_table[page_to_section(page)];
}
EXPORT_SYMBOL(page_to_nid);

/* IAMROOT, 2021.11.13:
 * - 64bits system에서는 NODE_NOT_IN_PAGE_FLAGS == disable인 경우가
 *   대부분이지만 만약 enable 된다면 mem_section[..]가 pair인 table을
 *   하나 더 생성하여 @section_nr 값을 기반으로 빠르게 탐색하도록 추적한다.
 */
static void set_section_nid(unsigned long section_nr, int nid)
{
	section_to_node_table[section_nr] = nid;
}
#else /* !NODE_NOT_IN_PAGE_FLAGS */
static inline void set_section_nid(unsigned long section_nr, int nid)
{
}
#endif

#ifdef CONFIG_SPARSEMEM_EXTREME
/* IAMROOT, 2024.06.04:
 * - root 마다 소유할 수 있는 최대 nr sections 만큼 alloc 하고 반환.
 */
static noinline struct mem_section __ref *sparse_index_alloc(int nid)
{
	struct mem_section *section = NULL;
	/* IAMROOT, 2024.06.04:
	 * - extreme에서 @array_size == PAGE_SIZE(4k) 이다.
	 */
	unsigned long array_size = SECTIONS_PER_ROOT *
				   sizeof(struct mem_section);

	/* IAMROOT, 2024.06.04:
	 * - slab 할당자가 아직 비활성화되어 있다면 memblock을 사용한다.
	 */
	if (slab_is_available()) {
		section = kzalloc_node(array_size, GFP_KERNEL, nid);
	} else {
		section = memblock_alloc_node(array_size, SMP_CACHE_BYTES,
					      nid);
		if (!section)
			panic("%s: Failed to allocate %lu bytes nid=%d\n",
			      __func__, array_size, nid);
	}

	return section;
}

/* IAMROOT, 2021.11.13:
 * - new section을 alloc하고 @section_nr을 root(index)로 변환하여
 *   mem_section[root]에 매핑(초기화)한다.
 */
static int __meminit sparse_index_init(unsigned long section_nr, int nid)
{
	unsigned long root = SECTION_NR_TO_ROOT(section_nr);
	struct mem_section *section;

	/*
	 * An existing section is possible in the sub-section hotplug
	 * case. First hot-add instantiates, follow-on hot-add reuses
	 * the existing section.
	 *
	 * The mem_hotplug_lock resolves the apparent race below.
	 */
	/* IAMROOT, 2024.06.04:
	 * - 값이 이미 존재하면 skip.
	 */
	if (mem_section[root])
		return 0;

	section = sparse_index_alloc(nid);
	if (!section)
		return -ENOMEM;

	/* IAMROOT, 2024.06.04:
	 * - @root가 가리키는 mem_section[..] bucket에 va(section) 저장.
	 *   여기서 section은 SECTIONS_PER_ROOT 갯수만큼 담을 수 있는 배열.
	 */
	mem_section[root] = section;

	return 0;
}
#else /* !SPARSEMEM_EXTREME */
static inline int sparse_index_init(unsigned long section_nr, int nid)
{
	return 0;
}
#endif

/*
 * During early boot, before section_mem_map is used for an actual
 * mem_map, we use section_mem_map to store the section's NUMA
 * node.  This keeps us from having to use another data structure.  The
 * node information is cleared just before we store the real mem_map.
 */
/*
 * IAMROOT, 2021.11.13:
 * - nid는 bit6부터 저장한다. sparse를 초기화할때만 사용한다.
 */
static inline unsigned long sparse_encode_early_nid(int nid)
{
	return ((unsigned long)nid << SECTION_NID_SHIFT);
}

/* IAMROOT, 2024.06.06:
 * - @section에 저장된 nid를 반환한다.
 */
static inline int sparse_early_nid(struct mem_section *section)
{
	return (section->section_mem_map >> SECTION_NID_SHIFT);
}

/* Validate the physical addressing limitations of the model */
/* IAMROOT, 2021.11.13:
 * - @start_pfn .. @end_pfn 범위에 대한 validation 체크.
 */
void __meminit mminit_validate_memmodel_limits(unsigned long *start_pfn,
						unsigned long *end_pfn)
{
	/* IAMROOT, 2021.11.13:
	 * - max pfn 값 정의.
	 *   1 << (48 - 12) = 1 << (36) = 2^36
	 */
	unsigned long max_sparsemem_pfn = 1UL << (MAX_PHYSMEM_BITS-PAGE_SHIFT);

	/*
	 * Sanity checks - do not allow an architecture to pass
	 * in larger pfns than the maximum scope of sparsemem:
	 */
	/* IAMROOT, 2024.06.02:
	 * - @start_pfn, @end_pfn 의 값이 max_sparsemem_pfn을 넘는지 확인.
	 *   넘는다면 arch setup 잘못이므로 로그 출력 후 해당 변수들의 값을
	 *   max_sparsemem_pfn으로 제한.
	 */
	if (*start_pfn > max_sparsemem_pfn) {
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"Start of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*start_pfn = max_sparsemem_pfn;
		*end_pfn = max_sparsemem_pfn;
	} else if (*end_pfn > max_sparsemem_pfn) {
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"End of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*end_pfn = max_sparsemem_pfn;
	}
}

/*
 * There are a number of times that we loop over NR_MEM_SECTIONS,
 * looking for section_present() on each.  But, when we have very
 * large physical address spaces, NR_MEM_SECTIONS can also be
 * very large which makes the loops quite long.
 *
 * Keeping track of this gives us an easy way to break out of
 * those loops early.
 */
unsigned long __highest_present_section_nr;

/* IAMROOT, 2021.11.13:
 * - @ms (struct mem_section)에 SECTION_MARKED_PRESENT flag 설정.
 */
static void __section_mark_present(struct mem_section *ms,
		unsigned long section_nr)
{
	/* IAMROOT, 2024.06.04:
	 * - highest section 정보를 빠르게 찾기 위해 mark할 때 계속 업데이트.
	 */
	if (section_nr > __highest_present_section_nr)
		__highest_present_section_nr = section_nr;

	/* IAMROOT, 2024.06.05:
	 * - SECTION_MARKED_PRESENT flag는 memory가 존재하는 section이란 의미.
	 */
	ms->section_mem_map |= SECTION_MARKED_PRESENT;
}

#define for_each_present_section_nr(start, section_nr)		\
	for (section_nr = next_present_section_nr(start-1);	\
	     ((section_nr != -1) &&				\
	      (section_nr <= __highest_present_section_nr));	\
	     section_nr = next_present_section_nr(section_nr))

/* IAMROOT, 2021.11.13:
 * - phys memory가 존재하는 첫번째 section을 탐색하여 section 번호를 반환한다.
 */
static inline unsigned long first_present_section_nr(void)
{
	return next_present_section_nr(-1);
}

#ifdef CONFIG_SPARSEMEM_VMEMMAP
/*
 * IAMROOT, 2021.12.04:
 * - 해당 section의 start subsection idx, end subsection idx를 구해서 bitmap set한다.
 * - pfn은 section단위 pfn.
 * ex) pfn = 0x1000, nr_pages = 0x400
 * idx = 0, end = 31
 * bitmap_set(map, 0, 32)
 */
static void subsection_mask_set(unsigned long *map, unsigned long pfn,
		unsigned long nr_pages)
{
	int idx = subsection_map_index(pfn);
	int end = subsection_map_index(pfn + nr_pages - 1);

	bitmap_set(map, idx, end - idx + 1);
}

/*
 * IAMROOT, 2021.12.04:
 * - memblock단위로 해당 함수에 진입한다.
 *   해당 pfn의 start section ~ end section까지의 section을 순회하며
 *   해당 section내에서 subsection을 초기화한다.
 */
void __init subsection_map_init(unsigned long pfn, unsigned long nr_pages)
{
	int end_sec = pfn_to_section_nr(pfn + nr_pages - 1);
	unsigned long nr, start_sec = pfn_to_section_nr(pfn);

	if (!nr_pages)
		return;

	for (nr = start_sec; nr <= end_sec; nr++) {
		struct mem_section *ms;
		unsigned long pfns;

		pfns = min(nr_pages, PAGES_PER_SECTION
				- (pfn & ~PAGE_SECTION_MASK));
		ms = __nr_to_section(nr);
/*
 * IAMROOT, 2021.12.04:
 * - 현재 memblock의 start pfn(pfn)과 현재 section의 pfn개수(pfns)를 가지고
 *   현재 section의 mem_section의 subsection_map에 초기화한다.
 */
		subsection_mask_set(ms->usage->subsection_map, pfn, pfns);

		pr_debug("%s: sec: %lu pfns: %lu set(%d, %d)\n", __func__, nr,
				pfns, subsection_map_index(pfn),
				subsection_map_index(pfn + pfns - 1));

		pfn += pfns;
		nr_pages -= pfns;
	}
}
#else
void __init subsection_map_init(unsigned long pfn, unsigned long nr_pages)
{
}
#endif

/* Record a memory area against a node. */
/*
 * IAMROOT, 2021.11.13:
 * - @nid, @start, @end에 해당하는 mem_section[]을 초기화한다.
 *   mem_section[]의 section_mem_map에는 nid, online, present가 set 된다.
 *
 *   @start: PFN type 이다.
 *   @end  : PFN type 이다.
 */
static void __init memory_present(int nid, unsigned long start, unsigned long end)
{
	unsigned long pfn;

#ifdef CONFIG_SPARSEMEM_EXTREME
	/* IAMROOT, 2021.11.13:
	 * - sparse memory에는 extreme, static방식이 있으며 arm64는 default로
	 *   extreme을 사용하며 해당 방식은 dynamic allocation 방식이므로
	 *   memblock_alloc(..)을 이용하여 관련 작업을 수행한다.
	 *
	 * - 우선 root(1차원 배열)를 생성하고 node(phys memory)가 해당 section의
	 *   범위에 할당되면 child(2차원 배열)을 할당하는 구조이기 때문에 section에
	 *   사용되는 메모리를 절약하게 된다.
	 *
	 * - size = 8 * 8192 = 64kb
	 */

	/* IAMROOT, 2024.06.02:
	 * - SPARSEMEM_EXTREME 환경에서는 boottime에 mem_section[]이 초기화되어
	 *   있지 않으므로 이곳에서 크기를 계산하고 memblock에서 alloc 한다.
	 *
	 *   단, 32bits는 static 구조를 사용하므로 compile-time에 크기가 정해진다.
	 */
	if (unlikely(!mem_section)) {
		unsigned long size, align;

		size = sizeof(struct mem_section *) * NR_SECTION_ROOTS;
		align = 1 << (INTERNODE_CACHE_SHIFT);
		mem_section = memblock_alloc(size, align);
		if (!mem_section)
			panic("%s: Failed to allocate %lu bytes align=0x%lx\n",
			      __func__, size, align);
	}
#endif

	start &= PAGE_SECTION_MASK;
	mminit_validate_memmodel_limits(&start, &end);

	/* IAMROOT, 2024.06.02:
	 * - validator에서 sanity check 이후 아래 loop를 다음 3가지 경우 중 하나로
	 *   결정하여 수행함.
	 *
	 *   1). @start < @end : range가 올바르므로 validator가 adjust 하지 않고
	 *                       loop 수행.
	 *   2). @start < @end : @end > max_sparsemem_pfn 조건으로 인해 validator가
	 *                       @end 값을 max_sparsemem_pfn으로 보정하였으나
	 *                       range는 여전히 유효하므로 loop 수행.
	 *   3). @start == @end: @start > max_sparsemem_pfn 조건으로 인해 validator가
	 *                       @start, @end 값을 max_sparsemem_pfn으로 보정하고
	 *                       loop cond로 인해 loop 수행하지 않고 함수 return.
	 */
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		unsigned long section = pfn_to_section_nr(pfn);
		struct mem_section *ms;

		/* IAMROOT, 2024.06.04:
		 * - 신규 section array (SECTIONS_PER_ROOT)을 alloc하고 @section 값을
		 *   기반으로 index를 구해 mem_section[index]에 매핑.
		 */
		sparse_index_init(section, nid);
		set_section_nid(section, nid);

		/* IAMROOT, 2024.06.04:
		 * - mem_section[root]에서 struct mem_section 오브젝트를 구해 ms에
		 *   저장하고 필요시 section_mem_map 멤버변수를 초기화한다.
		 */
		ms = __nr_to_section(section);
		if (!ms->section_mem_map) {
			/* IAMROOT, 2021.11.13:
			 * - nid 번호, ONLINE/PRESENT flag를 section_mem_map에 기록한다.
			 */
			ms->section_mem_map = sparse_encode_early_nid(nid) |
							SECTION_IS_ONLINE;
			__section_mark_present(ms, section);
		}
	}
}

/*
 * Mark all memblocks as present using memory_present().
 * This is a convenience function that is useful to mark all of the systems
 * memory as present during initialization.
 */
/* IAMROOT, 2021.11.13:
 * - memblock.memory 타입을 탐색하여 사용중인/가능한 memory에 대해
 *   mem_section[..] 및 root/section 모두를 초기화한다.
 */
static void __init memblocks_present(void)
{
	unsigned long start, end;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid)
		memory_present(nid, start, end);
}

/*
 * Subtle, we encode the real pfn into the mem_map such that
 * the identity pfn - section_mem_map will return the actual
 * physical page frame number.
 */
/*
 * IAMROOT, 2021.11.30:
 * - mem_map은 section_map_size() 으로(2MB) align되되어 생성되었고, 
 *   mem_section에 나눠줄때도 section_map_size()으로 align해서 잘라서 주기
 *   때문에 ~SECTION_MAP_MASK에 걸릴일은 없을것이다.
 */
static unsigned long sparse_encode_mem_map(struct page *mem_map, unsigned long pnum)
{
	unsigned long coded_mem_map =
		(unsigned long)(mem_map - (section_nr_to_pfn(pnum)));
	BUILD_BUG_ON(SECTION_MAP_LAST_BIT > (1UL<<PFN_SECTION_SHIFT));
	BUG_ON(coded_mem_map & ~SECTION_MAP_MASK);
	return coded_mem_map;
}

#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * Decode mem_map from the coded memmap
 */
struct page *sparse_decode_mem_map(unsigned long coded_mem_map, unsigned long pnum)
{
	/* mask off the extra low bits of information */
	coded_mem_map &= SECTION_MAP_MASK;
	return ((struct page *)coded_mem_map) + section_nr_to_pfn(pnum);
}
#endif /* CONFIG_MEMORY_HOTPLUG */

/*
 * IAMROOT, 2021.11.27:
 * - mem_section에 mem_map address를 저장하고 mem_map을 가지고 있다는 flag와
 *   인자의 flags를 저장하고 mem_section_usage 주소를 세팅한다.
 */
static void __meminit sparse_init_one_section(struct mem_section *ms,
		unsigned long pnum, struct page *mem_map,
		struct mem_section_usage *usage, unsigned long flags)
{
/*
 * IAMROOT, 2021.11.30:
 * - SECTION_MAP_MASK가 아닌 영역. 즉 flag영역을 제외한 나머지는 지운다.
 *   section_mem_map에 임시적으로 SECTION_NID_SHIFT 로 nid를 저장했었고,
 *   flag는 SECTION_IS_ONLINE, SECTION_MARKED_PRESENT 가 set되 있었을 것이다.
 *   즉 nid값만 지우게 된다.
 */
	ms->section_mem_map &= ~SECTION_MAP_MASK;
	ms->section_mem_map |= sparse_encode_mem_map(mem_map, pnum)
		| SECTION_HAS_MEM_MAP | flags;
	ms->usage = usage;
}

/*
 * IAMROOT, 2021.11.20: TODO
 * - struct mem_section_usage의 pageblock_flags의 크기를 결정하기 위한 함수.
 *   Section 크기, 수가 결정되어야지만 이를 통해 계산이 가능하다.
 *
 *   SECTION_BLOCKFLAGS_BITS: 256
 *   BITS_PER_TYPE(long)    : 32 bits (4 bytes)
 *   ----------------------------------------
 *   BITS_TO_LONGS(x)       : 8
 *
 *   따라서 256 bits를 수용하는데 long type 저장소 8개가 필요함.
 *
 *   128MB 영역관리당 32byte
 */
static unsigned long usemap_size(void)
{
	return BITS_TO_LONGS(SECTION_BLOCKFLAGS_BITS) * sizeof(unsigned long);
}

/* IAMROOT, 2021.11.20: TODO
 * - struct mem_section_usage 구조체의 크기를 구한다.
 *
 *   sizeof(struct mem_section_usage):  8 bytes
 *                      usemap_size(): 32 bytes
 *   ------------------------------------------
 *                                     40 bytes
 *
 *   따라서 struct mem_section_usage는 40 bytes 크기를 가진다.
 */
size_t mem_section_usage_size(void)
{
	return sizeof(struct mem_section_usage) + usemap_size();
}

/* IAMROOT, 2024.07.05:
 * - va(@pgdat)를 pa(@pgdat)로 변환하여 반환한다.
 */
static inline phys_addr_t pgdat_to_phys(struct pglist_data *pgdat)
{
#ifndef CONFIG_NUMA
	VM_BUG_ON(pgdat != &contig_page_data);
	return __pa_symbol(&contig_page_data);
#else
	return __pa(pgdat);
#endif
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * IAMROOT, 2021.11.20:
 * - nid에 해당하는 memory에서 mem_secion_usage로 사용할 memory를 얻어온다.
 *   node_data(pgdat)는 해당 nid에 존재하리라 예측하고, node_data 시작주소로해서
 *   secsion size내에서 구해오도록 노력한다.
 *   즉 한 section에서 node_data와 mem_secion_usage가 같이 존재하게 할려는 목적.
 *
 * - 일단 mem_secion 개수만큼 memory를 아래 함수에서 할당하고, 
 *   그 이후에 mem_secion을 하나하나돌며 mem_section_usage_size() 만큼 잘라서
 *   해당 mem_secion에서 사용할 memory를 mapping 해준다.
 *
 */
static struct mem_section_usage * __init
sparse_early_usemaps_alloc_pgdat_section(struct pglist_data *pgdat,
					 unsigned long size)
{
	struct mem_section_usage *usage;
	unsigned long goal, limit;
	int nid;
/*
 * IAMROOT, 2021.11.20:
 * - remove가 가능한 상황에서 memory들은 최대한 한곳에 모은다.
 */
	/*
	 * A page may contain usemaps for other sections preventing the
	 * page being freed and making a section unremovable while
	 * other sections referencing the usemap remain active. Similarly,
	 * a pgdat can prevent a section being removed. If section A
	 * contains a pgdat and section B contains the usemap, both
	 * sections become inter-dependent. This allocates usemaps
	 * from the same section as the pgdat where possible to avoid
	 * this problem.
	 */
	goal = pgdat_to_phys(pgdat) & (PAGE_SECTION_MASK << PAGE_SHIFT);
	limit = goal + (1UL << PA_SECTION_SHIFT);
	nid = early_pfn_to_nid(goal >> PAGE_SHIFT);
again:
	usage = memblock_alloc_try_nid(size, SMP_CACHE_BYTES, goal, limit, nid);
	if (!usage && limit) {
		limit = 0;
		goto again;
	}
	return usage;
}

static void __init check_usemap_section_nr(int nid,
		struct mem_section_usage *usage)
{
	unsigned long usemap_snr, pgdat_snr;
	static unsigned long old_usemap_snr;
	static unsigned long old_pgdat_snr;
	struct pglist_data *pgdat = NODE_DATA(nid);
	int usemap_nid;
/*
 * IAMROOT, 2021.11.20:
 * - 가장 높은 번호의 section 번호를 default로 설정
 */
	/* First call */
	if (!old_usemap_snr) {
		old_usemap_snr = NR_MEM_SECTIONS;
		old_pgdat_snr = NR_MEM_SECTIONS;
	}

	usemap_snr = pfn_to_section_nr(__pa(usage) >> PAGE_SHIFT);
	pgdat_snr = pfn_to_section_nr(pgdat_to_phys(pgdat) >> PAGE_SHIFT);
/*
 * IAMROOT, 2021.11.20:
 * - usemap과 pgdat이 있는곳이 같은 section에 있는지 검사한다.
 *   pgdat(node_data)와 usage는 이전에 같은 section에 위치하도록 노력했었다.
 *   같은 section이면 return
 */
	if (usemap_snr == pgdat_snr)
		return;

/*
 * IAMROOT, 2021.11.20:
 * - pgdat와 usage가 같지 않더라도 그전 section과 같다면 return.
 */
	if (old_usemap_snr == usemap_snr && old_pgdat_snr == pgdat_snr)
		/* skip redundant message */
		return;

/*
 * IAMROOT, 2021.11.20:
 * - usemap, pgdat가 같은 section이 아닌 경우에 들어온다.
 * - old section number를 갱신한다.
 * - usage가 존재하는 memory와 usage가 mapping되있는 mem_section의
 *   nid가 같은지 검사한다. 같지 않다면 return.
 */
	old_usemap_snr = usemap_snr;
	old_pgdat_snr = pgdat_snr;

	usemap_nid = sparse_early_nid(__nr_to_section(usemap_snr));
	if (usemap_nid != nid) {
		pr_info("node %d must be removed before remove section %ld\n",
			nid, usemap_snr);
		return;
	}
	/*
	 * There is a circular dependency.
	 * Some platforms allow un-removable section because they will just
	 * gather other removable sections for dynamic partitioning.
	 * Just notify un-removable section's number here.
	 */
	pr_info("Section %ld and %ld (node %d) have a circular dependency on usemap and pgdat allocations\n",
		usemap_snr, pgdat_snr, nid);
}
#else
/* IAMROOT, 2021.11.20:
 * - @size 크기만큼 memblock에서 memory를 alloc 한다.
 */
static struct mem_section_usage * __init
sparse_early_usemaps_alloc_pgdat_section(struct pglist_data *pgdat,
					 unsigned long size)
{
	return memblock_alloc_node(size, SMP_CACHE_BYTES, pgdat->node_id);
}

static void __init check_usemap_section_nr(int nid,
		struct mem_section_usage *usage)
{
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

#ifdef CONFIG_SPARSEMEM_VMEMMAP
/*
 * IAMROOT, 2021.11.20:
 * - 2MB단위(section 단위)로 align한다.
 *   struct page는 64byte고, PAGE_PER_SECTION은 32k이므로 2MB에 딱 떨어진다.
 */
static unsigned long __init section_map_size(void)
{
	return ALIGN(sizeof(struct page) * PAGES_PER_SECTION, PMD_SIZE);
}

#else
static unsigned long __init section_map_size(void)
{
	return PAGE_ALIGN(sizeof(struct page) * PAGES_PER_SECTION);
}

/*
 * IAMROOT, 2021.11.20:
 * - 인자의 nid에 해당하는 memory에서 (그 전에 할당해놓은 sparse_buffer)
 *   size만큼 map을 가져오고, 실패하면 다른 node에서 map을 가져온다.
 */
struct page __init *__populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap)
{
	unsigned long size = section_map_size();
	struct page *map = sparse_buffer_alloc(size);
	phys_addr_t addr = __pa(MAX_DMA_ADDRESS);

	if (map)
		return map;

	map = memmap_alloc(size, size, addr, nid, false);
	if (!map)
		panic("%s: Failed to allocate %lu bytes align=0x%lx nid=%d from=%pa\n",
		      __func__, size, PAGE_SIZE, nid, &addr);

	return map;
}
#endif /* !CONFIG_SPARSEMEM_VMEMMAP */

static void *sparsemap_buf __meminitdata;
static void *sparsemap_buf_end __meminitdata;

static inline void __meminit sparse_buffer_free(unsigned long size)
{
	WARN_ON(!sparsemap_buf || size == 0);
	memblock_free_early(__pa(sparsemap_buf), size);
}

/*
 * IAMROOT, 2021.11.20:
 * - 인자의 nid의 node에서 메모리를 할당받아온다.
 *   section_map_size를 align으로 하고 MAX_DMA_ADDRESS를 min주소로 하여
 *   size만큼 memory를 할당받아온다.
 */
static void __init sparse_buffer_init(unsigned long size, int nid)
{
	phys_addr_t addr = __pa(MAX_DMA_ADDRESS);
	WARN_ON(sparsemap_buf);	/* forgot to call sparse_buffer_fini()? */
	/*
	 * Pre-allocated buffer is mainly used by __populate_section_memmap
	 * and we want it to be properly aligned to the section size - this is
	 * especially the case for VMEMMAP which maps memmap to PMDs
	 */
	sparsemap_buf = memmap_alloc(size, section_map_size(), addr, nid, true);
	sparsemap_buf_end = sparsemap_buf + size;
}

static void __init sparse_buffer_fini(void)
{
	unsigned long size = sparsemap_buf_end - sparsemap_buf;

	if (sparsemap_buf && size > 0)
		sparse_buffer_free(size);
	sparsemap_buf = NULL;
}

/*
 * IAMROOT, 2021.11.27:
 * - sparse buffer에서 size align에 맞게 공간을 구해온다.
 *   만약에 구해오는 ptr 주소와 그 전 ptr 사이에 공간이 있다면 그 공간은 free를한다.
 */
void * __meminit sparse_buffer_alloc(unsigned long size)
{
	void *ptr = NULL;

	if (sparsemap_buf) {
		ptr = (void *) roundup((unsigned long)sparsemap_buf, size);
		if (ptr + size > sparsemap_buf_end)
			ptr = NULL;
		else {
/*
 * IAMROOT, 2021.11.20:
 *
 * sparse_buffer_init
 * Reserve ffffffc07d000000 - ffffffc07d600000 (6M)
 *
 * Sparse_buffer_alloc
 * Alloc   ffffffc07d000000 - ffffffc07d001000 (4k)
 * Sparse_buffer_alloc
 * Alloc   ffffffc07d200000 - ffffffc07d400000 (2M) <- 2MB - 4k 할당해제
 * Sparse_buffer_fini
 * Free    ffffffc07d400000 - ffffffc07d600000 (2M) <- 남은 부분 할당해제
 */
			/* Free redundant aligned space */
			if ((unsigned long)(ptr - sparsemap_buf) > 0)
				sparse_buffer_free((unsigned long)(ptr - sparsemap_buf));
			sparsemap_buf = ptr + size;
		}
	}
	return ptr;
}

void __weak __meminit vmemmap_populate_print_last(void)
{
}

/*
 * Initialize sparse on a specific node. The node spans [pnum_begin, pnum_end)
 * And number of present sections in this node is map_count.
 */
/* IAMROOT, 2021.11.27:
 * - @nid에 대해 range[@pnum_begin .. @pnum_end]까지 @map_count 개의
 *   section을 sparse 방식으로 memmap에 초기화한다.
 */
static void __init sparse_init_nid(int nid, unsigned long pnum_begin,
				   unsigned long pnum_end,
				   unsigned long map_count)
{
	struct mem_section_usage *usage;
	unsigned long pnum;
	struct page *map;

	/* IAMROOT, 2021.11.20:
	 * - @nid에 대응되는 struct mem_section_usage 자료구조를 @map_count
	 *   갯수만큼 생성한다.
	 */
	usage = sparse_early_usemaps_alloc_pgdat_section(NODE_DATA(nid),
			mem_section_usage_size() * map_count);
	if (!usage) {
		pr_err("%s: node[%d] usemap allocation failed", __func__, nid);
		goto failed;
	}

/*
 * IAMROOT, 2021.11.20:
 * - nid에 해당하는 node memory에서 sparse buffer(mem_map)를 만든다.
 *   일단 sparse buffer자체는 nid에 맞는 memory로만 가져오고 추후에
 *   각 mem_secion에 나눠줄때 모자르게되면 nid무관으로해서 memory를 가져온다.
 */
	sparse_buffer_init(map_count * section_map_size(), nid);
	for_each_present_section_nr(pnum_begin, pnum) {
		unsigned long pfn = section_nr_to_pfn(pnum);

		if (pnum >= pnum_end)
			break;

		map = __populate_section_memmap(pfn, PAGES_PER_SECTION,
				nid, NULL);
		if (!map) {
			pr_err("%s: node[%d] memory map backing failed. Some memory will not be available.",
			       __func__, nid);
			pnum_begin = pnum;
			sparse_buffer_fini();
			goto failed;
		}
		check_usemap_section_nr(nid, usage);
		sparse_init_one_section(__nr_to_section(pnum), pnum, map, usage,
				SECTION_IS_EARLY);
		usage = (void *) usage + mem_section_usage_size();
	}
	sparse_buffer_fini();
	return;
failed:
	/* We failed to allocate, mark all the following pnums as not present */
	for_each_present_section_nr(pnum_begin, pnum) {
		struct mem_section *ms;

		if (pnum >= pnum_end)
			break;
		ms = __nr_to_section(pnum);
		ms->section_mem_map = 0;
	}
}

/*
 * Allocate the accumulated non-linear sections, allocate a mem_map
 * for each and record the physical to section mapping.
 */
/* IAMROOT, 2021.11.13:
 * - Kenrel에 구현된 Physical Memory Model에는 크게 다음 3가지를 지원한다.
 *   1) FLATMEM
 *      - phys memory의 node가 1개일때 사용하는 모델이다. 모든 page frame이
 *        선형적으로 연결되어 있다.
 *   2) DISCONTGMEM
 *      - phys memory의 node가 2개 이상일 때 사용가능하며 각 node에 대한
 *        memblock region은 서로 떨어져 있을 수 있어 phys addr 상에 hole이
 *        존재할 수 있다.
 *   3) SPARSEMEM
 *      - DISCONTGMEM과 비슷하지만 phys memory addr를 section 이란 논리적
 *        영역으로 나눠서 section 단위로 관리한다.
 *        - 32bits와 64bits arch 간에 설계 차이가 존재하기도 한다.
 *
 *
 * - mem_secion을 초기화한다.
 *   > mem_section root 및 present mem_secion 생성
 *   > mem_section_usage를 초기화한다.
 *   > mem_map을 초기화한다.
 *   > mem_section.usage에 mem_section_usage를 매핑하고
 *   mem_section.section_mem_map에 sparse_encode_mem_map을 통해 mem_map을
 *   매핑한다.
 *  - section_to_node_table을 초기화한다.
 *
 * ---
 * 4K page 기준 address 변환
 *
 *   63.............12
 *   PFN : page frame number :  __phys_to_pfn, __pfn_to_phys,
 *   63........27
 *   section           : PA_SECTION_SHIFT, PFN_SECTION_SHIFT
 *
 * --- mem block에 생성된 memory들 중간정리
 * - mem_secion의 root
 *   > 위치
 *   nid 무관 mem_block
 *   > 역할
 *   root 한개당 secion(128MB) 256개 관리. (총 32GB)
 *   root는 총 8192개 있다.(총 256TB)
 *   > memory 사용량
 *   (struct mem_secion *) * 8192 = 64kb
 *   NR_SECTION_ROOTS 개수 만큼 nid무관으로 생성된다.
 *   memory_present 에서 초기화된다.  8192개(NR_SECTION_ROOTS) 만큼 필요하므로
 *   64kb 크기만큼의 memblock으로 할당된다.
 *
 * - nid별 mem_secion
 *   > 위치
 *   해당 nid의 mem_block에 위치하도록 노력한다.
 *   > 역할
 *   128MB 영역관리. mem_section_usage, mem_map이 mapping된다.
 *   > memory 사용량
 *   mem_block에 존재하는 nid 종류와 해당 nid memory가 차지하는 section 개수에
 *   따라서 생성된다. nid mem_block의 pfn으로 pfn이 속하는 section을 구하며,
 *   해당 section에 대한 mem_section이 존재하지 않으면 mem_secion에 256개
 *   (SECTIONS_PER_ROOT) 만큼을 한번에 할당한다.
 *   > section_mem_map에 mem_secion의 nid등의 flag를 encode한다.
 *
 * - nid별 mem_secion_usage
 *   > 위치
 *   해당 nid의 mem_block에 위치하도록 노력한다.
 *   node_data와 같은 section에 위치하도록 노력한다.
 *   > 역할
 *   1개의 mem_secion(128MB)에 대해서 subsection_map과 pageblock_flags를 관리한다.
 *   pageblock_flags는 pageblock_order(2MB) 단위로 나눠서 관리한다.
 *   subsection_map은 SUBSECTIONS_PER_SECTION(2MB) 단위로 나눠서 64개로 관리한다.
 *   > memory 사용량
 *   subsecion_map하나당 1bit. mem_secion엔 subsecion_map이 64개 존재하므로
 *   총 64bit인 8byte를 사용한다.
 *   pageblock_flags는 pageblock_order하나당 4bit. mem_secion엔 pageblock_bits
 *   64개 존재하므로 4 * 64 = 256 bit. 총 32byte.
 *   두개를 더하면 40byte에 해당한다. 즉 mem_secion 단위인 128MB당 40byte의
 *   관리 비용이 소모된다.
 *
 * - node_data
 *   > 위치
 *   해당 nid의 mem_blcok에 위치하도록 노력한다.
 *   > 역할
 *   해당 nid의 start_pfn과 size가 저장되있다.
 *
 * - section_map(mem_map)
 *   > 위치
 *   해당 nid의 mem_block에 위치하도록 노력한다.
 *   > 역할
 *   struct page가 PAGE_PER_SECTION개만큼 모여 있다.
 *   > memory 사용량
 *   struct page는 64byte고(option에 따라서 늘어난다면 align에 의해서 128byte
 *   가 될수있다.) section당 page수는 32k(PAGE_PER_SECION)이므로 2MB(PMD_SIZE)
 *   가 된다. 즉 한개의 section 128MB에 대한 struct page를 관리하는데 2MB가
 *   필요하다.
 *
 * ---
 * 변환식 정리
 *
 * from       | to         | func example
 * -----------+------------+--------------------------
 * section_nr | mem_secion | __nr_to_section
 *            | nid        | section_to_node_table
 *            | pfn        | section_nr_to_pfn
 * -----------+------------+--------------------------
 * pfn        | section_nr | pfn_to_section_nr
 */
void __init sparse_init(void)
{
	/* IAMROOT, 2021.11.25:
	 * pnum : present section number
	 */
	unsigned long pnum_end, pnum_begin, map_count = 1;
	int nid_begin;

	/* IAMROOT, 2024.06.05:
	 * - mem_section[..]을 초기화하고 memblock.memory에서 사용중인/가능한
	 *   memory에 대해 root/section을 alloc하고 mem_section[..]에 초기화한다.
	 */
	memblocks_present();

	/* IAMROOT, 2021.11.13:
	 * - phys memory가 존재하는 첫번째 section의 번호를 @pnum_begin에
	 *   저장하고 @nid 정보를 가져와 nid_begin에 저장한다.
	 */
	pnum_begin = first_present_section_nr();
	nid_begin = sparse_early_nid(__nr_to_section(pnum_begin));

	/* Setup pageblock_order for HUGETLB_PAGE_SIZE_VARIABLE */
	set_pageblock_order();

	/* IAMROOT, 2024.06.06: TODO
	 * - range[@pnum_begin .. @pnum_end)까지 (!= -1 && <= __highest) 조건으로
	 *   loop를 수행하며 sparse_init_nid(..)을 호출한다.
	 */
	for_each_present_section_nr(pnum_begin + 1, pnum_end) {
		int nid = sparse_early_nid(__nr_to_section(pnum_end));

		/* IAMROOT, 2024.07.09:
		 * - pnum_begin과 pnum_end의 nid가 동일하면 우선 map_count만
		 *   증가시키다가 nid가 다르면 sparse_init_nid(..) 호출하여 한번에
		 *   처리하도록 한다.
		 *
		 *   'map_count > 1' 이라면 동일한 nid를 가진 section이 여러개
		 *   존재함을 의미하지만 중간에 PRESENT 상태가 아닌 section도
		 *   존재할 수 있다.
		 */
		if (nid == nid_begin) {
			map_count++;
			continue;
		}

		/* IAMROOT, 2024.07.09:
		 * - pnum_begin과 pnum_end의 nid가 다르므로 pnum_begin에서 map_count
		 *   갯수만큼 sparse_init_nid(..)을 호출하여 초기화한다.
		 */
		/* Init node with sections in range [pnum_begin, pnum_end) */
		sparse_init_nid(nid_begin, pnum_begin, pnum_end, map_count);

		/* IAMROOT, 2024.07.09:
		 * - @nid_begin의 section 초기화가 완료되었으므로 다음 nid/section
		 *   초기화를 위해 관련 변수들을 다시 세팅한다.
		 */
		nid_begin = nid;
		pnum_begin = pnum_end;
		map_count = 1;
	}
	/* cover the last node */
	sparse_init_nid(nid_begin, pnum_begin, pnum_end, map_count);
	vmemmap_populate_print_last();
}

#ifdef CONFIG_MEMORY_HOTPLUG

/* Mark all memory sections within the pfn range as online */
void online_mem_sections(unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		unsigned long section_nr = pfn_to_section_nr(pfn);
		struct mem_section *ms;

		/* onlining code should never touch invalid ranges */
		if (WARN_ON(!valid_section_nr(section_nr)))
			continue;

		ms = __nr_to_section(section_nr);
		ms->section_mem_map |= SECTION_IS_ONLINE;
	}
}

/* Mark all memory sections within the pfn range as offline */
void offline_mem_sections(unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		unsigned long section_nr = pfn_to_section_nr(pfn);
		struct mem_section *ms;

		/*
		 * TODO this needs some double checking. Offlining code makes
		 * sure to check pfn_valid but those checks might be just bogus
		 */
		if (WARN_ON(!valid_section_nr(section_nr)))
			continue;

		ms = __nr_to_section(section_nr);
		ms->section_mem_map &= ~SECTION_IS_ONLINE;
	}
}

#ifdef CONFIG_SPARSEMEM_VMEMMAP
static struct page * __meminit populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap)
{
	return __populate_section_memmap(pfn, nr_pages, nid, altmap);
}

static void depopulate_section_memmap(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap)
{
	unsigned long start = (unsigned long) pfn_to_page(pfn);
	unsigned long end = start + nr_pages * sizeof(struct page);

	vmemmap_free(start, end, altmap);
}
static void free_map_bootmem(struct page *memmap)
{
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);

	vmemmap_free(start, end, NULL);
}

static int clear_subsection_map(unsigned long pfn, unsigned long nr_pages)
{
	DECLARE_BITMAP(map, SUBSECTIONS_PER_SECTION) = { 0 };
	DECLARE_BITMAP(tmp, SUBSECTIONS_PER_SECTION) = { 0 };
	struct mem_section *ms = __pfn_to_section(pfn);
	unsigned long *subsection_map = ms->usage
		? &ms->usage->subsection_map[0] : NULL;

	subsection_mask_set(map, pfn, nr_pages);
	if (subsection_map)
		bitmap_and(tmp, map, subsection_map, SUBSECTIONS_PER_SECTION);

	if (WARN(!subsection_map || !bitmap_equal(tmp, map, SUBSECTIONS_PER_SECTION),
				"section already deactivated (%#lx + %ld)\n",
				pfn, nr_pages))
		return -EINVAL;

	bitmap_xor(subsection_map, map, subsection_map, SUBSECTIONS_PER_SECTION);
	return 0;
}

static bool is_subsection_map_empty(struct mem_section *ms)
{
	return bitmap_empty(&ms->usage->subsection_map[0],
			    SUBSECTIONS_PER_SECTION);
}

static int fill_subsection_map(unsigned long pfn, unsigned long nr_pages)
{
	struct mem_section *ms = __pfn_to_section(pfn);
	DECLARE_BITMAP(map, SUBSECTIONS_PER_SECTION) = { 0 };
	unsigned long *subsection_map;
	int rc = 0;

	subsection_mask_set(map, pfn, nr_pages);

	subsection_map = &ms->usage->subsection_map[0];

	if (bitmap_empty(map, SUBSECTIONS_PER_SECTION))
		rc = -EINVAL;
	else if (bitmap_intersects(map, subsection_map, SUBSECTIONS_PER_SECTION))
		rc = -EEXIST;
	else
		bitmap_or(subsection_map, map, subsection_map,
				SUBSECTIONS_PER_SECTION);

	return rc;
}
#else
struct page * __meminit populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap)
{
	return kvmalloc_node(array_size(sizeof(struct page),
					PAGES_PER_SECTION), GFP_KERNEL, nid);
}

static void depopulate_section_memmap(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap)
{
	kvfree(pfn_to_page(pfn));
}

static void free_map_bootmem(struct page *memmap)
{
	unsigned long maps_section_nr, removing_section_nr, i;
	unsigned long magic, nr_pages;
	struct page *page = virt_to_page(memmap);

	nr_pages = PAGE_ALIGN(PAGES_PER_SECTION * sizeof(struct page))
		>> PAGE_SHIFT;

	for (i = 0; i < nr_pages; i++, page++) {
		magic = (unsigned long) page->freelist;

		BUG_ON(magic == NODE_INFO);

		maps_section_nr = pfn_to_section_nr(page_to_pfn(page));
		removing_section_nr = page_private(page);

		/*
		 * When this function is called, the removing section is
		 * logical offlined state. This means all pages are isolated
		 * from page allocator. If removing section's memmap is placed
		 * on the same section, it must not be freed.
		 * If it is freed, page allocator may allocate it which will
		 * be removed physically soon.
		 */
		if (maps_section_nr != removing_section_nr)
			put_page_bootmem(page);
	}
}

static int clear_subsection_map(unsigned long pfn, unsigned long nr_pages)
{
	return 0;
}

static bool is_subsection_map_empty(struct mem_section *ms)
{
	return true;
}

static int fill_subsection_map(unsigned long pfn, unsigned long nr_pages)
{
	return 0;
}
#endif /* CONFIG_SPARSEMEM_VMEMMAP */

/*
 * To deactivate a memory region, there are 3 cases to handle across
 * two configurations (SPARSEMEM_VMEMMAP={y,n}):
 *
 * 1. deactivation of a partial hot-added section (only possible in
 *    the SPARSEMEM_VMEMMAP=y case).
 *      a) section was present at memory init.
 *      b) section was hot-added post memory init.
 * 2. deactivation of a complete hot-added section.
 * 3. deactivation of a complete section from memory init.
 *
 * For 1, when subsection_map does not empty we will not be freeing the
 * usage map, but still need to free the vmemmap range.
 *
 * For 2 and 3, the SPARSEMEM_VMEMMAP={y,n} cases are unified
 */
static void section_deactivate(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap)
{
	struct mem_section *ms = __pfn_to_section(pfn);
	bool section_is_early = early_section(ms);
	struct page *memmap = NULL;
	bool empty;

	if (clear_subsection_map(pfn, nr_pages))
		return;

	empty = is_subsection_map_empty(ms);
	if (empty) {
		unsigned long section_nr = pfn_to_section_nr(pfn);

		/*
		 * When removing an early section, the usage map is kept (as the
		 * usage maps of other sections fall into the same page). It
		 * will be re-used when re-adding the section - which is then no
		 * longer an early section. If the usage map is PageReserved, it
		 * was allocated during boot.
		 */
		if (!PageReserved(virt_to_page(ms->usage))) {
			kfree(ms->usage);
			ms->usage = NULL;
		}
		memmap = sparse_decode_mem_map(ms->section_mem_map, section_nr);
		/*
		 * Mark the section invalid so that valid_section()
		 * return false. This prevents code from dereferencing
		 * ms->usage array.
		 */
		ms->section_mem_map &= ~SECTION_HAS_MEM_MAP;
	}

	/*
	 * The memmap of early sections is always fully populated. See
	 * section_activate() and pfn_valid() .
	 */
	if (!section_is_early)
		depopulate_section_memmap(pfn, nr_pages, altmap);
	else if (memmap)
		free_map_bootmem(memmap);

	if (empty)
		ms->section_mem_map = (unsigned long)NULL;
}

static struct page * __meminit section_activate(int nid, unsigned long pfn,
		unsigned long nr_pages, struct vmem_altmap *altmap)
{
	struct mem_section *ms = __pfn_to_section(pfn);
	struct mem_section_usage *usage = NULL;
	struct page *memmap;
	int rc = 0;

	if (!ms->usage) {
		usage = kzalloc(mem_section_usage_size(), GFP_KERNEL);
		if (!usage)
			return ERR_PTR(-ENOMEM);
		ms->usage = usage;
	}

	rc = fill_subsection_map(pfn, nr_pages);
	if (rc) {
		if (usage)
			ms->usage = NULL;
		kfree(usage);
		return ERR_PTR(rc);
	}

	/*
	 * The early init code does not consider partially populated
	 * initial sections, it simply assumes that memory will never be
	 * referenced.  If we hot-add memory into such a section then we
	 * do not need to populate the memmap and can simply reuse what
	 * is already there.
	 */
	if (nr_pages < PAGES_PER_SECTION && early_section(ms))
		return pfn_to_page(pfn);

	memmap = populate_section_memmap(pfn, nr_pages, nid, altmap);
	if (!memmap) {
		section_deactivate(pfn, nr_pages, altmap);
		return ERR_PTR(-ENOMEM);
	}

	return memmap;
}

/**
 * sparse_add_section - add a memory section, or populate an existing one
 * @nid: The node to add section on
 * @start_pfn: start pfn of the memory range
 * @nr_pages: number of pfns to add in the section
 * @altmap: device page map
 *
 * This is only intended for hotplug.
 *
 * Note that only VMEMMAP supports sub-section aligned hotplug,
 * the proper alignment and size are gated by check_pfn_span().
 *
 *
 * Return:
 * * 0		- On success.
 * * -EEXIST	- Section has been present.
 * * -ENOMEM	- Out of memory.
 */
int __meminit sparse_add_section(int nid, unsigned long start_pfn,
		unsigned long nr_pages, struct vmem_altmap *altmap)
{
	unsigned long section_nr = pfn_to_section_nr(start_pfn);
	struct mem_section *ms;
	struct page *memmap;
	int ret;

	ret = sparse_index_init(section_nr, nid);
	if (ret < 0)
		return ret;

	memmap = section_activate(nid, start_pfn, nr_pages, altmap);
	if (IS_ERR(memmap))
		return PTR_ERR(memmap);

	/*
	 * Poison uninitialized struct pages in order to catch invalid flags
	 * combinations.
	 */
	page_init_poison(memmap, sizeof(struct page) * nr_pages);

	ms = __nr_to_section(section_nr);
	set_section_nid(section_nr, nid);
	__section_mark_present(ms, section_nr);

	/* Align memmap to section boundary in the subsection case */
	if (section_nr_to_pfn(section_nr) != start_pfn)
		memmap = pfn_to_page(section_nr_to_pfn(section_nr));
	sparse_init_one_section(ms, section_nr, memmap, ms->usage, 0);

	return 0;
}

#ifdef CONFIG_MEMORY_FAILURE
static void clear_hwpoisoned_pages(struct page *memmap, int nr_pages)
{
	int i;

	/*
	 * A further optimization is to have per section refcounted
	 * num_poisoned_pages.  But that would need more space per memmap, so
	 * for now just do a quick global check to speed up this routine in the
	 * absence of bad pages.
	 */
	if (atomic_long_read(&num_poisoned_pages) == 0)
		return;

	for (i = 0; i < nr_pages; i++) {
		if (PageHWPoison(&memmap[i])) {
			num_poisoned_pages_dec();
			ClearPageHWPoison(&memmap[i]);
		}
	}
}
#else
static inline void clear_hwpoisoned_pages(struct page *memmap, int nr_pages)
{
}
#endif

void sparse_remove_section(struct mem_section *ms, unsigned long pfn,
		unsigned long nr_pages, unsigned long map_offset,
		struct vmem_altmap *altmap)
{
	clear_hwpoisoned_pages(pfn_to_page(pfn) + map_offset,
			nr_pages - map_offset);
	section_deactivate(pfn, nr_pages, altmap);
}
#endif /* CONFIG_MEMORY_HOTPLUG */
