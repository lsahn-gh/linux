// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/percpu-vm.c - vmalloc area based chunk allocation
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * Chunks are mapped into vmalloc areas and populated page by page.
 * This is the default chunk allocator.
 */
#include "internal.h"

static struct page *pcpu_chunk_page(struct pcpu_chunk *chunk,
				    unsigned int cpu, int page_idx)
{
	/* must not be used on pre-mapped chunk */
	WARN_ON(chunk->immutable);

	return vmalloc_to_page((void *)pcpu_chunk_addr(chunk, cpu, page_idx));
}

/**
 * pcpu_get_pages - get temp pages array
 *
 * Returns pointer to array of pointers to struct page which can be indexed
 * with pcpu_page_idx().  Note that there is only one array and accesses
 * should be serialized by pcpu_alloc_mutex.
 *
 * RETURNS:
 * Pointer to temp pages array on success.
 */

/*
 * IAMROOT, 2022.02.05:
 * - pcpu chunk에 필요한 전체 unit page수 만큼 page pointer를 할당하고
 *   return한다.
 *   (unit 개수 * 1개의 unit에서 사용하는 page 개수 * sizeof(struct page *))
 */
static struct page **pcpu_get_pages(void)
{
	static struct page **pages;
	size_t pages_size = pcpu_nr_units * pcpu_unit_pages * sizeof(pages[0]);

	lockdep_assert_held(&pcpu_alloc_mutex);

	if (!pages)
		pages = pcpu_mem_zalloc(pages_size, GFP_KERNEL);
	return pages;
}

/**
 * pcpu_free_pages - free pages which were allocated for @chunk
 * @chunk: chunk pages were allocated for
 * @pages: array of pages to be freed, indexed by pcpu_page_idx()
 * @page_start: page index of the first page to be freed
 * @page_end: page index of the last page to be freed + 1
 *
 * Free pages [@page_start and @page_end) in @pages for all units.
 * The pages were allocated for @chunk.
 */
static void pcpu_free_pages(struct pcpu_chunk *chunk,
			    struct page **pages, int page_start, int page_end)
{
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page *page = pages[pcpu_page_idx(cpu, i)];

			if (page)
				__free_page(page);
		}
	}
}

/**
 * pcpu_alloc_pages - allocates pages for @chunk
 * @chunk: target chunk
 * @pages: array to put the allocated pages into, indexed by pcpu_page_idx()
 * @page_start: page index of the first page to be allocated
 * @page_end: page index of the last page to be allocated + 1
 * @gfp: allocation flags passed to the underlying allocator
 *
 * Allocate pages [@page_start,@page_end) into @pages for all units.
 * The allocation is for @chunk.  Percpu core doesn't care about the
 * content of @pages and will pass it verbatim to pcpu_map_pages().
 */

/*
 * IAMROOT, 2022.02.05:
 * - 모든 cpu에 대해서 요청한 범위만큼의 page를 할당하고 pages에 저장한다.
 */
static int pcpu_alloc_pages(struct pcpu_chunk *chunk,
			    struct page **pages, int page_start, int page_end,
			    gfp_t gfp)
{
	unsigned int cpu, tcpu;
	int i;

	gfp |= __GFP_HIGHMEM;


/*
 * IAMROOT, 2022.02.05:
 * - 모든 possible cpu에 대해서 요청한 범위만큼 page를 할당한다.
 *   이때 각 page는 연속된 영역이 아니라 page allocator에 의한 영역에서 할당되온다.
 *   나중에 파편화를 방지하기 위해서 일부러 single order page를 받아온것이다.
 */
	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page **pagep = &pages[pcpu_page_idx(cpu, i)];

			*pagep = alloc_pages_node(cpu_to_node(cpu), gfp, 0);
			if (!*pagep)
				goto err;
		}
	}
	return 0;

err:
/*
 * IAMROOT, 2022.02.05:
 * - 실패했다면 실패한지점부터 역으로 돌면서 정리를 하고 빠져나간다.
 */
	while (--i >= page_start)
		__free_page(pages[pcpu_page_idx(cpu, i)]);

	for_each_possible_cpu(tcpu) {
		if (tcpu == cpu)
			break;
		for (i = page_start; i < page_end; i++)
			__free_page(pages[pcpu_page_idx(tcpu, i)]);
	}
	return -ENOMEM;
}

/**
 * pcpu_pre_unmap_flush - flush cache prior to unmapping
 * @chunk: chunk the regions to be flushed belongs to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages in [@page_start,@page_end) of @chunk are about to be
 * unmapped.  Flush cache.  As each flushing trial can be very
 * expensive, issue flush on the whole region at once rather than
 * doing it for each cpu.  This could be an overkill but is more
 * scalable.
 */
static void pcpu_pre_unmap_flush(struct pcpu_chunk *chunk,
				 int page_start, int page_end)
{
	flush_cache_vunmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

static void __pcpu_unmap_pages(unsigned long addr, int nr_pages)
{
	vunmap_range_noflush(addr, addr + (nr_pages << PAGE_SHIFT));
}

/**
 * pcpu_unmap_pages - unmap pages out of a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array which can be used to pass information to free
 * @page_start: page index of the first page to unmap
 * @page_end: page index of the last page to unmap + 1
 *
 * For each cpu, unmap pages [@page_start,@page_end) out of @chunk.
 * Corresponding elements in @pages were cleared by the caller and can
 * be used to carry information to pcpu_free_pages() which will be
 * called after all unmaps are finished.  The caller should call
 * proper pre/post flush functions.
 */
static void pcpu_unmap_pages(struct pcpu_chunk *chunk,
			     struct page **pages, int page_start, int page_end)
{
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page *page;

			page = pcpu_chunk_page(chunk, cpu, i);
			WARN_ON(!page);
			pages[pcpu_page_idx(cpu, i)] = page;
		}
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, cpu, page_start),
				   page_end - page_start);
	}
}

/**
 * pcpu_post_unmap_tlb_flush - flush TLB after unmapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been unmapped.  Flush
 * TLB for the regions.  This can be skipped if the area is to be
 * returned to vmalloc as vmalloc will handle TLB flushing lazily.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */
static void pcpu_post_unmap_tlb_flush(struct pcpu_chunk *chunk,
				      int page_start, int page_end)
{
	flush_tlb_kernel_range(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}


/*
 * IAMROOT, 2022.02.05:
 * - page에 addr을 mapping한다.
 */
static int __pcpu_map_pages(unsigned long addr, struct page **pages,
			    int nr_pages)
{
	return vmap_pages_range_noflush(addr, addr + (nr_pages << PAGE_SHIFT),
					PAGE_KERNEL, pages, PAGE_SHIFT);
}

/**
 * pcpu_map_pages - map pages into a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array containing pages to be mapped
 * @page_start: page index of the first page to map
 * @page_end: page index of the last page to map + 1
 *
 * For each cpu, map pages [@page_start,@page_end) into @chunk.  The
 * caller is responsible for calling pcpu_post_map_flush() after all
 * mappings are complete.
 *
 * This function is responsible for setting up whatever is necessary for
 * reverse lookup (addr -> chunk).
 */

/*
 * IAMROOT, 2022.02.05:
 * - struct page에 memory를 mapping한다. struct page의 index에
 *   pcpu_chunk를 set한다.
 */
static int pcpu_map_pages(struct pcpu_chunk *chunk,
			  struct page **pages, int page_start, int page_end)
{
	unsigned int cpu, tcpu;
	int i, err;

	for_each_possible_cpu(cpu) {
		err = __pcpu_map_pages(pcpu_chunk_addr(chunk, cpu, page_start),
				       &pages[pcpu_page_idx(cpu, page_start)],
				       page_end - page_start);
		if (err < 0)
			goto err;

		for (i = page_start; i < page_end; i++)
			pcpu_set_page_chunk(pages[pcpu_page_idx(cpu, i)],
					    chunk);
	}
	return 0;
err:
	for_each_possible_cpu(tcpu) {
		if (tcpu == cpu)
			break;
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, tcpu, page_start),
				   page_end - page_start);
	}
	pcpu_post_unmap_tlb_flush(chunk, page_start, page_end);
	return err;
}

/**
 * pcpu_post_map_flush - flush cache after mapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been mapped.  Flush
 * cache.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */

/*
 * IAMROOT, 2022.02.05:
 * - TODO
 */
static void pcpu_post_map_flush(struct pcpu_chunk *chunk,
				int page_start, int page_end)
{
	flush_cache_vmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

/**
 * pcpu_populate_chunk - populate and map an area of a pcpu_chunk
 * @chunk: chunk of interest
 * @page_start: the start page
 * @page_end: the end page
 * @gfp: allocation flags passed to the underlying memory allocator
 *
 * For each cpu, populate and map pages [@page_start,@page_end) into
 * @chunk.
 *
 * CONTEXT:
 * pcpu_alloc_mutex, does GFP_KERNEL allocation.
 */

/*
 * IAMROOT, 2022.02.05:
 * - 요청된 page 범위만큼 struct page를 할당해서 해당 memory에
 *   mapping하고 각 struct page의 index에 pcpu_chunk를 set한다.
 */
static int pcpu_populate_chunk(struct pcpu_chunk *chunk,
			       int page_start, int page_end, gfp_t gfp)
{
	struct page **pages;

	pages = pcpu_get_pages();
	if (!pages)
		return -ENOMEM;

	if (pcpu_alloc_pages(chunk, pages, page_start, page_end, gfp))
		return -ENOMEM;

	if (pcpu_map_pages(chunk, pages, page_start, page_end)) {
		pcpu_free_pages(chunk, pages, page_start, page_end);
		return -ENOMEM;
	}
	pcpu_post_map_flush(chunk, page_start, page_end);

	return 0;
}

/**
 * pcpu_depopulate_chunk - depopulate and unmap an area of a pcpu_chunk
 * @chunk: chunk to depopulate
 * @page_start: the start page
 * @page_end: the end page
 *
 * For each cpu, depopulate and unmap pages [@page_start,@page_end)
 * from @chunk.
 *
 * Caller is required to call pcpu_post_unmap_tlb_flush() if not returning the
 * region back to vmalloc() which will lazily flush the tlb.
 *
 * CONTEXT:
 * pcpu_alloc_mutex.
 */

/*
 * IAMROOT, 2022.02.05:
 * - struct page을 unmapping하고 free시킨다.
 */
static void pcpu_depopulate_chunk(struct pcpu_chunk *chunk,
				  int page_start, int page_end)
{
	struct page **pages;

	/*
	 * If control reaches here, there must have been at least one
	 * successful population attempt so the temp pages array must
	 * be available now.
	 */
	pages = pcpu_get_pages();
	BUG_ON(!pages);

	/* unmap and free */
	pcpu_pre_unmap_flush(chunk, page_start, page_end);

	pcpu_unmap_pages(chunk, pages, page_start, page_end);

	pcpu_free_pages(chunk, pages, page_start, page_end);
}


/*
 * IAMROOT, 2022.02.05:
 * - chunk를 만들고 mapping한다.
 */
static struct pcpu_chunk *pcpu_create_chunk(gfp_t gfp)
{
	struct pcpu_chunk *chunk;
	struct vm_struct **vms;

	chunk = pcpu_alloc_chunk(gfp);
	if (!chunk)
		return NULL;

	vms = pcpu_get_vm_areas(pcpu_group_offsets, pcpu_group_sizes,
				pcpu_nr_groups, pcpu_atom_size);
	if (!vms) {
		pcpu_free_chunk(chunk);
		return NULL;
	}

	chunk->data = vms;
	chunk->base_addr = vms[0]->addr - pcpu_group_offsets[0];

	pcpu_stats_chunk_alloc();
	trace_percpu_create_chunk(chunk->base_addr);

	return chunk;
}


/*
 * IAMROOT, 2022.02.05:
 * - chunk에 대한 메모리 해제
 */
static void pcpu_destroy_chunk(struct pcpu_chunk *chunk)
{
	if (!chunk)
		return;

	pcpu_stats_chunk_dealloc();
	trace_percpu_destroy_chunk(chunk->base_addr);

	if (chunk->data)
		pcpu_free_vm_areas(chunk->data, pcpu_nr_groups);
	pcpu_free_chunk(chunk);
}


/*
 * IAMROOT, 2022.02.05:
 * - 해당 addr의 struct page를 가져온다.
 */
static struct page *pcpu_addr_to_page(void *addr)
{
	return vmalloc_to_page(addr);
}

static int __init pcpu_verify_alloc_info(const struct pcpu_alloc_info *ai)
{
	/* no extra restriction */
	return 0;
}

/**
 * pcpu_should_reclaim_chunk - determine if a chunk should go into reclaim
 * @chunk: chunk of interest
 *
 * This is the entry point for percpu reclaim.  If a chunk qualifies, it is then
 * isolated and managed in separate lists at the back of pcpu_slot: sidelined
 * and to_depopulate respectively.  The to_depopulate list holds chunks slated
 * for depopulation.  They no longer contribute to pcpu_nr_empty_pop_pages once
 * they are on this list.  Once depopulated, they are moved onto the sidelined
 * list which enables them to be pulled back in for allocation if no other chunk
 * can suffice the allocation.
 */

/*
 * IAMROOT, 2022.02.05:
 * 1) isolate이면서 empty populate page가 있는경우
 * 2) 전체 시스템에서 empty poplate page가 일정범위 이상이면서
 * chunk의 empty populate page도 일정범위 이상인경우
 */
static bool pcpu_should_reclaim_chunk(struct pcpu_chunk *chunk)
{
	/* do not reclaim either the first chunk or reserved chunk */
	if (chunk == pcpu_first_chunk || chunk == pcpu_reserved_chunk)
		return false;

	/*
	 * If it is isolated, it may be on the sidelined list so move it back to
	 * the to_depopulate list.  If we hit at least 1/4 pages empty pages AND
	 * there is no system-wide shortage of empty pages aside from this
	 * chunk, move it to the to_depopulate list.
	 */
	return ((chunk->isolated && chunk->nr_empty_pop_pages) ||
		(pcpu_nr_empty_pop_pages >
		 (PCPU_EMPTY_POP_PAGES_HIGH + chunk->nr_empty_pop_pages) &&
		 chunk->nr_empty_pop_pages >= chunk->nr_pages / 4));
}
