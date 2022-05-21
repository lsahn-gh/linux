/* SPDX-License-Identifier: GPL-2.0-or-later */
/* internal.h: mm/ internal definitions
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#ifndef __MM_INTERNAL_H
#define __MM_INTERNAL_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/tracepoint-defs.h>

/*
 * The set of flags that only affect watermark checking and reclaim
 * behaviour. This is used by the MM to obey the caller constraints
 * about IO, FS and watermark checking while ignoring placement
 * hints such as HIGHMEM usage.
 */
#define GFP_RECLAIM_MASK (__GFP_RECLAIM|__GFP_HIGH|__GFP_IO|__GFP_FS|\
			__GFP_NOWARN|__GFP_RETRY_MAYFAIL|__GFP_NOFAIL|\
			__GFP_NORETRY|__GFP_MEMALLOC|__GFP_NOMEMALLOC|\
			__GFP_ATOMIC)

/*
 * IAMROOT, 2022.02.26: 
 * 부트 타임에 사용가능한 gfp 플래그들이다. 
 * reclaim 루틴이 동작할 수 없으므로,
 * 전체 gfp 플래그들중 다음 3가지 플래그는 제외시킨다.
 * __GFP_RECLAIM, __GFP_IO, __GFP_FS
 */
/* The GFP flags allowed during early boot */
#define GFP_BOOT_MASK (__GFP_BITS_MASK & ~(__GFP_RECLAIM|__GFP_IO|__GFP_FS))

/* Control allocation cpuset and node placement constraints */
#define GFP_CONSTRAINT_MASK (__GFP_HARDWALL|__GFP_THISNODE)

/* Do not use these with a slab allocator */
#define GFP_SLAB_BUG_MASK (__GFP_DMA32|__GFP_HIGHMEM|~__GFP_BITS_MASK)

void page_writeback_init(void);

vm_fault_t do_swap_page(struct vm_fault *vmf);

void free_pgtables(struct mmu_gather *tlb, struct vm_area_struct *start_vma,
		unsigned long floor, unsigned long ceiling);

static inline bool can_madv_lru_vma(struct vm_area_struct *vma)
{
	return !(vma->vm_flags & (VM_LOCKED|VM_HUGETLB|VM_PFNMAP));
}

void unmap_page_range(struct mmu_gather *tlb,
			     struct vm_area_struct *vma,
			     unsigned long addr, unsigned long end,
			     struct zap_details *details);

void do_page_cache_ra(struct readahead_control *, unsigned long nr_to_read,
		unsigned long lookahead_size);
void force_page_cache_ra(struct readahead_control *, unsigned long nr);
static inline void force_page_cache_readahead(struct address_space *mapping,
		struct file *file, pgoff_t index, unsigned long nr_to_read)
{
	DEFINE_READAHEAD(ractl, file, &file->f_ra, mapping, index);
	force_page_cache_ra(&ractl, nr_to_read);
}

unsigned find_lock_entries(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct pagevec *pvec, pgoff_t *indices);

/**
 * page_evictable - test whether a page is evictable
 * @page: the page to test
 *
 * Test whether page is evictable--i.e., should be placed on active/inactive
 * lists vs unevictable list.
 *
 * Reasons page might not be evictable:
 * (1) page's mapping marked unevictable
 * (2) page is part of an mlocked VMA
 *
 */
/*
 * IAMROOT, 2022.04.02:
 * - @page가 evictable인지(회수가 가능한지) 확인한다.
 * - unevictable flag가 없거나 mlock이 안걸려있어야된다.
 */
static inline bool page_evictable(struct page *page)
{
	bool ret;

	/* Prevent address_space of inode and swap cache from being freed */
	rcu_read_lock();
	ret = !mapping_unevictable(page_mapping(page)) && !PageMlocked(page);
	rcu_read_unlock();
	return ret;
}

/*
 * Turn a non-refcounted page (->_refcount == 0) into refcounted with
 * a count of one.
 */
/*
 * IAMROOT, 2022.03.05:
 * - 할당이 완료된 @page의 refcount를 1로 한다.
 */
static inline void set_page_refcounted(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	VM_BUG_ON_PAGE(page_ref_count(page), page);
	set_page_count(page, 1);
}

extern unsigned long highest_memmap_pfn;

/*
 * Maximum number of reclaim retries without progress before the OOM
 * killer is consider the only way forward.
 */
#define MAX_RECLAIM_RETRIES 16

/*
 * in mm/vmscan.c:
 */
extern int isolate_lru_page(struct page *page);
extern void putback_lru_page(struct page *page);

/*
 * in mm/rmap.c:
 */
extern pmd_t *mm_find_pmd(struct mm_struct *mm, unsigned long address);

/*
 * in mm/memcontrol.c:
 */
extern bool cgroup_memory_nokmem;

/*
 * in mm/page_alloc.c
 */

/*
 * Structure for holding the mostly immutable allocation parameters passed
 * between functions involved in allocations, including the alloc_pages*
 * family of functions.
 *
 * nodemask, migratetype and highest_zoneidx are initialized only once in
 * __alloc_pages() and then never change.
 *
 * zonelist, preferred_zone and highest_zoneidx are set first in
 * __alloc_pages() for the fast path, and might be later changed
 * in __alloc_pages_slowpath(). All other functions pass the whole structure
 * by a const pointer.
 */
/*
 * IAMROOT, 2022.02.26: 
 * page 할당자에서 연속된 함수들 호출간에 인자 전달을 용이하기 위해
 * 내부 목적으로 사용된다.
 */
struct alloc_context {
/*
 * IAMROOT, 2022.03.25:
 * - node_zonelist 함수에서 설정된다. 요청된 gfp flag에 __GFP_THISNODE가
 *   있다면 ZONELIST_NOFALLBACK, 그게 아니라면 ZONELIST_FALLBACK으로
 *   선택된다.
 */
	struct zonelist *zonelist;
	nodemask_t *nodemask;
	struct zoneref *preferred_zoneref;
	int migratetype;

	/*
	 * highest_zoneidx represents highest usable zone index of
	 * the allocation request. Due to the nature of the zone,
	 * memory on lower zone than the highest_zoneidx will be
	 * protected by lowmem_reserve[highest_zoneidx].
	 *
	 * highest_zoneidx is also used by reclaim/compaction to limit
	 * the target zone since higher zone than this index cannot be
	 * usable for this allocation request.
	 */
/*
 * IAMROOT, 2022.03.23:
 * - prepare_alloc_pages, gfp_zone에서 gfp_mask를 통해 정해졌다.
 */
	enum zone_type highest_zoneidx;
	bool spread_dirty_pages;
};

/*
 * Locate the struct page for both the matching buddy in our
 * pair (buddy1) and the combined O(n+1) page they form (page).
 *
 * 1) Any buddy B1 will have an order O twin B2 which satisfies
 * the following equation:
 *     B2 = B1 ^ (1 << O)
 * For example, if the starting buddy (buddy2) is #8 its order
 * 1 buddy is #10:
 *     B2 = 8 ^ (1 << 1) = 8 ^ 2 = 10
 *
 * 2) Any buddy B will have an order O+1 parent P which
 * satisfies the following equation:
 *     P = B & ~(1 << O)
 *
 * Assumption: *_mem_map is contiguous at least up to MAX_ORDER
 */
/*
 * IAMROOT, 2022.05.07:
 * - buddy page를 찾는다.
 * ex) order 0에 대한 pair page
 *   3 -> 2
 *   2 -> 3
 *   1 -> 0
 *   0 -> 1
 * ex) order 1에 대한 pair page
 *   7 -> X
 *   6 -> 4
 *   5 -> X
 *   4 -> 6
 *   3 -> X
 *   2 -> 0
 *   1 -> X
 *   0 -> 2
 */
static inline unsigned long
__find_buddy_pfn(unsigned long page_pfn, unsigned int order)
{
	return page_pfn ^ (1 << order);
}

extern struct page *__pageblock_pfn_to_page(unsigned long start_pfn,
				unsigned long end_pfn, struct zone *zone);

/*
 * IAMROOT, 2022.03.26: 
 * [start_pfn, end_pfn)의 페이지가 zone에 속한 online page인지 여부를 검사하여
 * 그러한 경우 start_page를 반환하고, 아니면 NULL을 반환한다.
 */
static inline struct page *pageblock_pfn_to_page(unsigned long start_pfn,
				unsigned long end_pfn, struct zone *zone)
{
/*
 * IAMROOT, 2022.03.28:
 * - Git blame 참고 / papago
 *   tree 8a57a26127dc9c96059ceedebc2cf13e5d124e3c
 *   parent e1409c325fdc1fef7b3d8025c51892355f065d15
 *
 *   mm/compaction: speed up pageblock_pfn_to_page() when zone is contiguous.
 *   There is a performance drop report due to hugepage allocation and in
 *   there half of cpu time are spent on pageblock_pfn_to_page() in
 *   compaction [1].
 *
 *   In that workload, compaction is triggered to make hugepage but most of
 *   pageblocks are un-available for compaction due to pageblock type and
 *   skip bit so compaction usually fails. Most costly operations in this
 *   case is to find valid pageblock while scanning whole zone range.
 *   To check if pageblock is valid to compact, valid pfn within pageblock is
 *   required and we can obtain it by calling pageblock_pfn_to_page().
 *   This function checks whether pageblock is in a single zone and return
 *   valid pfn if possible. Problem is that we need to check it every time
 *   before scanning pageblock even if we re-visit it and this turns out to
 *   be very expensive in this workload.
 *
 *   Although we have no way to skip this pageblock check in the system where
 *   hole exists at arbitrary position, we can use cached value for zone
 *   continuity and just do pfn_to_page() in the system where hole doesn't
 *   exist.  This optimization considerably speeds up in above workload.
 *
 * ---
 *
 *  mm/compaction: 존이 연속된 경우 pageblock_pfn_to_page() 속도를 높입니다.
 *
 * 페이지 할당이 크기 때문에 퍼포먼스가 저하된 보고서가 있으며, 이 보고서에서는
 * CPU 시간의 절반이 압축으로 pageblock_pfn_to_page()에 소비됩니다 [1].
 *
 * 이 워크로드에서는 큰 페이지를 만들기 위해 압축이 트리거되지만 대부분의
 * 페이지 블록은 페이지 블록 유형과 건너뛰기 비트로 인해 압축에 사용할 수
 * 없기 때문에 압축이 실패합니다. 이 경우 가장 비용이 많이 드는 작업은
 * 존 범위 전체를 스캔하면서 유효한 페이지 블록을 찾는 것입니다. 페이지 블록이
 * 압축에 유효한지 여부를 확인하려면 페이지 블록 내의 유효한 pfn이 필요하며
 * pageblock_pfn_to_page()를 호출하여 얻을 수 있습니다. 이 함수는
 * 페이지 블록이 단일 존 내에 있는지 확인하고 가능하면 유효한 pfn을 반환합니다.
 * 문제는 다시 방문하더라도 페이지 블록을 스캔하기 전에 매번 확인해야 하고,
 * 이 작업 부하에서 비용이 많이 든다는 것입니다.
 *
 * 홀이 임의의 위치에 있는 시스템에서는 이 페이지 블록체크를 생략할 수 없지만
 * 존의 연속성에 캐시된 값을 사용하여 홀이 존재하지 않는 시스템에서는
 * pfn_to_page()를 실행할 수 있습니다. 이러한 최적화를 통해 위의 워크로드에서
 * 속도가 크게 향상됩니다.
 *
 * - 대략 hole이 없는 system에서는 zone->contiguous가 true가 될수 있으며
 *   이 경우 즉시 pfn_to_page로 확인이 가능하다는 뜻인듯 싶다.
 */
	if (zone->contiguous)
		return pfn_to_page(start_pfn);

	return __pageblock_pfn_to_page(start_pfn, end_pfn, zone);
}

extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __putback_isolated_page(struct page *page, unsigned int order,
				    int mt);
extern void memblock_free_pages(struct page *page, unsigned long pfn,
					unsigned int order);
extern void __free_pages_core(struct page *page, unsigned int order);
extern void prep_compound_page(struct page *page, unsigned int order);
extern void post_alloc_hook(struct page *page, unsigned int order,
					gfp_t gfp_flags);
extern int user_min_free_kbytes;

extern void free_unref_page(struct page *page, unsigned int order);
extern void free_unref_page_list(struct list_head *list);

extern void zone_pcp_update(struct zone *zone, int cpu_online);
extern void zone_pcp_reset(struct zone *zone);
extern void zone_pcp_disable(struct zone *zone);
extern void zone_pcp_enable(struct zone *zone);

extern void *memmap_alloc(phys_addr_t size, phys_addr_t align,
			  phys_addr_t min_addr,
			  int nid, bool exact_nid);

#if defined CONFIG_COMPACTION || defined CONFIG_CMA

/*
 * in mm/compaction.c
 */
/*
 * compact_control is used to track pages being migrated and the free pages
 * they are being migrated to during memory compaction. The free_pfn starts
 * at the end of a zone and migrate_pfn begins at the start. Movable pages
 * are moved to the end of a zone during a compaction run and the run
 * completes when free_pfn <= migrate_pfn
 */
struct compact_control {
	struct list_head freepages;	/* List of free pages to migrate to */
	struct list_head migratepages;	/* List of pages being migrated */
	unsigned int nr_freepages;	/* Number of isolated free pages */
	unsigned int nr_migratepages;	/* Number of pages to migrate */
	unsigned long free_pfn;		/* isolate_freepages search base */
	/*
	 * Acts as an in/out parameter to page isolation for migration.
	 * isolate_migratepages uses it as a search base.
	 * isolate_migratepages_block will update the value to the next pfn
	 * after the last isolated one.
	 */
/*
 * IAMROOT, 2022.03.29:
 * - migrate_pfn
 *   compact의 start pfn. compact범위가 정해지면 migrate_pfn이 정해지는데,
 *   여기서 한번더 fast_find_migrateblock을 통해 fast search가 실패한다면
 *   가장 최근 fast_start_pfn으로 migrate_pfn이 update되며 줄어들어 범위가
 *   줄어들수있다.
 */
	unsigned long migrate_pfn;
/*
 * IAMROOT, 2022.03.29:
 * - fast_find_migrateblock에서 migrate_pfn을 갱신하기 위한 임시변수.
 *   fast search를 한다면 0, 이후 fast search가 실패한다면 ULONG_MAX로
 *   되서 향후 udpate를 막는다.
 */
	unsigned long fast_start_pfn;	/* a pfn to start linear scan from */
	struct zone *zone;
/*
 * IAMROOT, 2022.03.29:
 * - 총 freelist scan 횟수가 저장된다.
 */
	unsigned long total_migrate_scanned;
	unsigned long total_free_scanned;
/*
 * IAMROOT, 2022.03.29:
 * - fast_find_migrateblock에서 freelist scan횟수를 결정한다.
 */
	unsigned short fast_search_fail;/* failures to use free list searches */
	short search_order;		/* order to start a fast search at */
	const gfp_t gfp_mask;		/* gfp mask of a direct compactor */
	int order;			/* order a direct compactor needs */
	int migratetype;		/* migratetype of direct compactor */
	const unsigned int alloc_flags;	/* alloc flags of a direct compactor */
	const int highest_zoneidx;	/* zone index of a direct compactor */
	enum migrate_mode mode;		/* Async or sync migration mode */
	bool ignore_skip_hint;		/* Scan blocks even if marked skip */
	bool no_set_skip_hint;		/* Don't mark blocks for skipping */
	bool ignore_block_suitable;	/* Scan blocks considered unsuitable */
	bool direct_compaction;		/* False from kcompactd or /proc/... */
	bool proactive_compaction;	/* kcompactd proactive compaction */
	bool whole_zone;		/* Whole zone should/has been scanned */
	bool contended;			/* Signal lock or sched contention */
	bool rescan;			/* Rescanning the same pageblock */
	bool alloc_contig;		/* alloc_contig_range allocation */
};

/*
 * Used in direct compaction when a page should be taken from the freelists
 * immediately when one is created during the free path.
 */
struct capture_control {
	struct compact_control *cc;
	struct page *page;
};

unsigned long
isolate_freepages_range(struct compact_control *cc,
			unsigned long start_pfn, unsigned long end_pfn);
int
isolate_migratepages_range(struct compact_control *cc,
			   unsigned long low_pfn, unsigned long end_pfn);
#endif
int find_suitable_fallback(struct free_area *area, unsigned int order,
			int migratetype, bool only_stealable, bool *can_steal);

/*
 * This function returns the order of a free page in the buddy system. In
 * general, page_zone(page)->lock must be held by the caller to prevent the
 * page from being allocated in parallel and returning garbage as the order.
 * If a caller does not hold page_zone(page)->lock, it must guarantee that the
 * page cannot be allocated or merged in parallel. Alternatively, it must
 * handle invalid values gracefully, and use buddy_order_unsafe() below.
 */
/*
 * IAMROOT, 2022.03.12:
 * - page가 buddy인 경우 private는 order를 가리킨다.
 */
static inline unsigned int buddy_order(struct page *page)
{
	/* PageBuddy() must be checked by the caller */
	return page_private(page);
}

/*
 * Like buddy_order(), but for callers who cannot afford to hold the zone lock.
 * PageBuddy() should be checked first by the caller to minimize race window,
 * and invalid values must be handled gracefully.
 *
 * READ_ONCE is used so that if the caller assigns the result into a local
 * variable and e.g. tests it for valid range before using, the compiler cannot
 * decide to remove the variable and inline the page_private(page) multiple
 * times, potentially observing different values in the tests and the actual
 * use of the result.
 */
#define buddy_order_unsafe(page)	READ_ONCE(page_private(page))

/*
 * These three helpers classifies VMAs for virtual memory accounting.
 */

/*
 * Executable code area - executable, not writable, not stack
 */
static inline bool is_exec_mapping(vm_flags_t flags)
{
	return (flags & (VM_EXEC | VM_WRITE | VM_STACK)) == VM_EXEC;
}

/*
 * Stack area - automatically grows in one direction
 *
 * VM_GROWSUP / VM_GROWSDOWN VMAs are always private anonymous:
 * do_mmap() forbids all other combinations.
 */
static inline bool is_stack_mapping(vm_flags_t flags)
{
	return (flags & VM_STACK) == VM_STACK;
}

/*
 * Data area - private, writable, not stack
 */
/*
 * IAMROOT, 2022.05.21:
 * stack을 제외한 data 공간.
 * Data area - private, writable, not stack
 */
static inline bool is_data_mapping(vm_flags_t flags)
{
	return (flags & (VM_WRITE | VM_SHARED | VM_STACK)) == VM_WRITE;
}

/* mm/util.c */
void __vma_link_list(struct mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev);
void __vma_unlink_list(struct mm_struct *mm, struct vm_area_struct *vma);

#ifdef CONFIG_MMU
extern long populate_vma_page_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long end, int *locked);
extern long faultin_vma_page_range(struct vm_area_struct *vma,
				   unsigned long start, unsigned long end,
				   bool write, int *locked);
extern void munlock_vma_pages_range(struct vm_area_struct *vma,
			unsigned long start, unsigned long end);
static inline void munlock_vma_pages_all(struct vm_area_struct *vma)
{
	munlock_vma_pages_range(vma, vma->vm_start, vma->vm_end);
}

/*
 * must be called with vma's mmap_lock held for read or write, and page locked.
 */
extern void mlock_vma_page(struct page *page);
extern unsigned int munlock_vma_page(struct page *page);

extern int mlock_future_check(struct mm_struct *mm, unsigned long flags,
			      unsigned long len);

/*
 * Clear the page's PageMlocked().  This can be useful in a situation where
 * we want to unconditionally remove a page from the pagecache -- e.g.,
 * on truncation or freeing.
 *
 * It is legal to call this function for any page, mlocked or not.
 * If called for a page that is still mapped by mlocked vmas, all we do
 * is revert to lazy LRU behaviour -- semantics are not broken.
 */
extern void clear_page_mlock(struct page *page);

extern pmd_t maybe_pmd_mkwrite(pmd_t pmd, struct vm_area_struct *vma);

/*
 * At what user virtual address is page expected in vma?
 * Returns -EFAULT if all of the page is outside the range of vma.
 * If page is a compound head, the entire compound page is considered.
 */
/*
 * IAMROOT, 2022.04.09:
 * - @page가 소속된 @vma로부터 실제 가상주소를 알아온다.
 */
static inline unsigned long
vma_address(struct page *page, struct vm_area_struct *vma)
{
	pgoff_t pgoff;
	unsigned long address;

	VM_BUG_ON_PAGE(PageKsm(page), page);	/* KSM page->index unusable */
	pgoff = page_to_pgoff(page);
	if (pgoff >= vma->vm_pgoff) {
/*
 * IAMROOT, 2022.04.09:
 * - 일반적인경우 vm_start를 base로 address를 구한다.
 */
		address = vma->vm_start +
			((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
		/* Check for address beyond vma (or wrapped through 0?) */
		if (address < vma->vm_start || address >= vma->vm_end)
			address = -EFAULT;
	} else if (PageHead(page) &&
		   pgoff + compound_nr(page) - 1 >= vma->vm_pgoff) {
		/* Test above avoids possibility of wrap to 0 on 32-bit */
		address = vma->vm_start;
	} else {
		address = -EFAULT;
	}
	return address;
}

/*
 * Then at what user virtual address will none of the page be found in vma?
 * Assumes that vma_address() already returned a good starting address.
 * If page is a compound head, the entire compound page is considered.
 */
static inline unsigned long
vma_address_end(struct page *page, struct vm_area_struct *vma)
{
	pgoff_t pgoff;
	unsigned long address;

	VM_BUG_ON_PAGE(PageKsm(page), page);	/* KSM page->index unusable */
	pgoff = page_to_pgoff(page) + compound_nr(page);
	address = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	/* Check for address beyond vma (or wrapped through 0?) */
	if (address < vma->vm_start || address > vma->vm_end)
		address = vma->vm_end;
	return address;
}

static inline struct file *maybe_unlock_mmap_for_io(struct vm_fault *vmf,
						    struct file *fpin)
{
	int flags = vmf->flags;

	if (fpin)
		return fpin;

	/*
	 * FAULT_FLAG_RETRY_NOWAIT means we don't want to wait on page locks or
	 * anything, so we only pin the file and drop the mmap_lock if only
	 * FAULT_FLAG_ALLOW_RETRY is set, while this is the first attempt.
	 */
	if (fault_flag_allow_retry_first(flags) &&
	    !(flags & FAULT_FLAG_RETRY_NOWAIT)) {
		fpin = get_file(vmf->vma->vm_file);
		mmap_read_unlock(vmf->vma->vm_mm);
	}
	return fpin;
}

#else /* !CONFIG_MMU */
static inline void clear_page_mlock(struct page *page) { }
static inline void mlock_vma_page(struct page *page) { }
static inline void vunmap_range_noflush(unsigned long start, unsigned long end)
{
}
#endif /* !CONFIG_MMU */

/*
 * Return the mem_map entry representing the 'offset' subpage within
 * the maximally aligned gigantic page 'base'.  Handle any discontiguity
 * in the mem_map at MAX_ORDER_NR_PAGES boundaries.
 */
static inline struct page *mem_map_offset(struct page *base, int offset)
{
	if (unlikely(offset >= MAX_ORDER_NR_PAGES))
		return nth_page(base, offset);
	return base + offset;
}

/*
 * Iterator over all subpages within the maximally aligned gigantic
 * page 'base'.  Handle any discontiguity in the mem_map.
 */
static inline struct page *mem_map_next(struct page *iter,
						struct page *base, int offset)
{
	if (unlikely((offset & (MAX_ORDER_NR_PAGES - 1)) == 0)) {
		unsigned long pfn = page_to_pfn(base) + offset;
		if (!pfn_valid(pfn))
			return NULL;
		return pfn_to_page(pfn);
	}
	return iter + 1;
}

/* Memory initialisation debug and verification */
enum mminit_level {
	MMINIT_WARNING,
	MMINIT_VERIFY,
	MMINIT_TRACE
};

#ifdef CONFIG_DEBUG_MEMORY_INIT

extern int mminit_loglevel;

#define mminit_dprintk(level, prefix, fmt, arg...) \
do { \
	if (level < mminit_loglevel) { \
		if (level <= MMINIT_WARNING) \
			pr_warn("mminit::" prefix " " fmt, ##arg);	\
		else \
			printk(KERN_DEBUG "mminit::" prefix " " fmt, ##arg); \
	} \
} while (0)

extern void mminit_verify_pageflags_layout(void);
extern void mminit_verify_zonelist(void);
#else

static inline void mminit_dprintk(enum mminit_level level,
				const char *prefix, const char *fmt, ...)
{
}

static inline void mminit_verify_pageflags_layout(void)
{
}

static inline void mminit_verify_zonelist(void)
{
}
#endif /* CONFIG_DEBUG_MEMORY_INIT */

/* mminit_validate_memmodel_limits is independent of CONFIG_DEBUG_MEMORY_INIT */
#if defined(CONFIG_SPARSEMEM)
extern void mminit_validate_memmodel_limits(unsigned long *start_pfn,
				unsigned long *end_pfn);
#else
static inline void mminit_validate_memmodel_limits(unsigned long *start_pfn,
				unsigned long *end_pfn)
{
}
#endif /* CONFIG_SPARSEMEM */

#define NODE_RECLAIM_NOSCAN	-2
#define NODE_RECLAIM_FULL	-1
#define NODE_RECLAIM_SOME	0
#define NODE_RECLAIM_SUCCESS	1

#ifdef CONFIG_NUMA
extern int node_reclaim(struct pglist_data *, gfp_t, unsigned int);
extern int find_next_best_node(int node, nodemask_t *used_node_mask);
#else
static inline int node_reclaim(struct pglist_data *pgdat, gfp_t mask,
				unsigned int order)
{
	return NODE_RECLAIM_NOSCAN;
}
static inline int find_next_best_node(int node, nodemask_t *used_node_mask)
{
	return NUMA_NO_NODE;
}
#endif

extern int hwpoison_filter(struct page *p);

extern u32 hwpoison_filter_dev_major;
extern u32 hwpoison_filter_dev_minor;
extern u64 hwpoison_filter_flags_mask;
extern u64 hwpoison_filter_flags_value;
extern u64 hwpoison_filter_memcg;
extern u32 hwpoison_filter_enable;

extern unsigned long  __must_check vm_mmap_pgoff(struct file *, unsigned long,
        unsigned long, unsigned long,
        unsigned long, unsigned long);

extern void set_pageblock_order(void);
unsigned int reclaim_clean_pages_from_list(struct zone *zone,
					    struct list_head *page_list);
/* The ALLOC_WMARK bits are used as an index to zone->watermark */
#define ALLOC_WMARK_MIN		WMARK_MIN
#define ALLOC_WMARK_LOW		WMARK_LOW
#define ALLOC_WMARK_HIGH	WMARK_HIGH
#define ALLOC_NO_WATERMARKS	0x04 /* don't check watermarks at all */

/* Mask to get the watermark bits */
#define ALLOC_WMARK_MASK	(ALLOC_NO_WATERMARKS-1)

/*
 * Only MMU archs have async oom victim reclaim - aka oom_reaper so we
 * cannot assume a reduced access to memory reserves is sufficient for
 * !MMU
 */
#ifdef CONFIG_MMU
#define ALLOC_OOM		0x08
#else
#define ALLOC_OOM		ALLOC_NO_WATERMARKS
#endif

/*
 * IAMROOT, 2022.03.16:
 * ALLOC_* 플래그들은 page 할당자에서 사용하는 내부 플래그들이다.
 *
 * - ALLOC_HARDER:
 *   인터럽트 context 등에서 할당을 하기 위해 할당 요청 시 GFP_ATOMIC 플래그를
 *   사용하였거나, 위의 플래그를 사용하지 않았어도 rt 스레드에서 할당 요청을 한 
 *   경우 ALLOC_HARDER 플래그를 내부에서 사용하는데, 
 *   1) 메모리 부족 시 남은 min 워터마크 기준보다 25% 더 할당을 받게 하고,
 *      (GFP_ATOMIC 사용시 아래 ALLOC_HIGH까지 부터 50% 한 후 추가 25% 적용)
 *   2) GFP_ATOMIC으로 높은 order 할당이 실패하지 않도록 예비로 관리하는 
 *      MIGRATE_HIGHATOMIC freelist를 사용하게 한다.
 * - ALLOC_HIGH:
 *   GFP_ATOMIC 플래그를 사용할 때 ALLOC_HARDER와 함께 이 플래그가 사용되며,
 *   메모리 부족 시 남은 min 워터마크 기준보다 50% 더 할당을 받게 한다.
 * - ALLOC_CMA:
 *   movable 페이지에 대한 할당 요청 시 가능하면 cma 영역을 사용하지 않고 
 *   할당을 시도하지만, 이 플래그를 사용하면 메모리가 부족 시 cma 영역도 
 *   사용하여 할당을 시도한다.
 * - ALLOC_NOFRAGMENT:
 *   페이지 할당 시 요청한 migratetype 만으로 구성된 페이지 블럭내에서 
 *   할당을 시도한다. 단 메모리가 부족한 경우에는 어쩔 수 없이,
 *   이 플래그 요청을 무시하고 fragment 할당을 한다.
 *   GFP_KERNEL or GFP_ATOMIC 등)과 같이 normal 존을 이용하는 커널 메모리 등을
 *   할당해야 할 때 노드 내에 해당 normal zone 밑에 dma(or dma32)가 구성되어 
 *   있는 경우 이러한 플래그를 사용되어 최대한 1 페이지 블럭내에서 여러 
 *   migratetype의 페이지가 할당되어 구성되지 않도록 노력한다.
 * - ALLOC_KSWAPD:
 *   GFP_ATOMIC을 제외한 GFP_KERNEL, GFP_USER, GFP_HIGHUSER 등의 메모리 할당을
 *   요청하는 경우 __GFP_RECLAIM 플래그(direct + kswapd)가 추가되는데 
 *   그 중 __GFP_KSWAPD_RECLAIM를 체크하여 이 플래그가 사용된다. 메모리 부족시
 *   즉각 kcompactd 및 kswapd 스레드를 꺄워 동작시키는 기능을 의미한다.
 */

#define ALLOC_HARDER		 0x10 /* try to alloc harder */
#define ALLOC_HIGH		 0x20 /* __GFP_HIGH set */
#define ALLOC_CPUSET		 0x40 /* check for correct cpuset */
#define ALLOC_CMA		 0x80 /* allow allocations from CMA areas */
#ifdef CONFIG_ZONE_DMA32
#define ALLOC_NOFRAGMENT	0x100 /* avoid mixing pageblock types */
#else
#define ALLOC_NOFRAGMENT	  0x0
#endif
#define ALLOC_KSWAPD		0x800 /* allow waking of kswapd, __GFP_KSWAPD_RECLAIM set */

enum ttu_flags;
struct tlbflush_unmap_batch;


/*
 * only for MM internal work items which do not depend on
 * any allocations or locks which might depend on allocations
 */
extern struct workqueue_struct *mm_percpu_wq;

#ifdef CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH
void try_to_unmap_flush(void);
void try_to_unmap_flush_dirty(void);
void flush_tlb_batched_pending(struct mm_struct *mm);
#else
static inline void try_to_unmap_flush(void)
{
}
static inline void try_to_unmap_flush_dirty(void)
{
}
static inline void flush_tlb_batched_pending(struct mm_struct *mm)
{
}
#endif /* CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH */

extern const struct trace_print_flags pageflag_names[];
extern const struct trace_print_flags vmaflag_names[];
extern const struct trace_print_flags gfpflag_names[];

static inline bool is_migrate_highatomic(enum migratetype migratetype)
{
	return migratetype == MIGRATE_HIGHATOMIC;
}

static inline bool is_migrate_highatomic_page(struct page *page)
{
	return get_pageblock_migratetype(page) == MIGRATE_HIGHATOMIC;
}

void setup_zone_pageset(struct zone *zone);

struct migration_target_control {
	int nid;		/* preferred node id */
	nodemask_t *nmask;
	gfp_t gfp_mask;
};

/*
 * mm/vmalloc.c
 */
#ifdef CONFIG_MMU
int vmap_pages_range_noflush(unsigned long addr, unsigned long end,
                pgprot_t prot, struct page **pages, unsigned int page_shift);
#else
static inline
int vmap_pages_range_noflush(unsigned long addr, unsigned long end,
                pgprot_t prot, struct page **pages, unsigned int page_shift)
{
	return -EINVAL;
}
#endif

void vunmap_range_noflush(unsigned long start, unsigned long end);

int numa_migrate_prep(struct page *page, struct vm_area_struct *vma,
		      unsigned long addr, int page_nid, int *flags);

#endif	/* __MM_INTERNAL_H */
