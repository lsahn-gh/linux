// SPDX-License-Identifier: GPL-2.0
/*
 * linux/mm/page_isolation.c
 */

#include <linux/mm.h>
#include <linux/page-isolation.h>
#include <linux/pageblock-flags.h>
#include <linux/memory.h>
#include <linux/hugetlb.h>
#include <linux/page_owner.h>
#include <linux/migrate.h>
#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/page_isolation.h>

/*
 * IAMROOT, 2022.07.09:
 * - @page가 소속된 page block단위로 @migratetype으로 이동이 가능한지 확인한다.
 *   이동이 가능하다면
 *   1. pageblock mt를 isolate로 전환
 *   2. pageblock에서 buddy에 있는것들은 page order isolate freelist로 옮긴다.
 */
static int set_migratetype_isolate(struct page *page, int migratetype, int isol_flags)
{
	struct zone *zone = page_zone(page);
	struct page *unmovable;
	unsigned long flags;

	spin_lock_irqsave(&zone->lock, flags);

	/*
	 * We assume the caller intended to SET migrate type to isolate.
	 * If it is already set, then someone else must have raced and
	 * set it before us.
	 */
	if (is_migrate_isolate_page(page)) {
		spin_unlock_irqrestore(&zone->lock, flags);
		return -EBUSY;
	}

	/*
	 * FIXME: Now, memory hotplug doesn't call shrink_slab() by itself.
	 * We just check MOVABLE pages.
	 */
/*
 * IAMROOT, 2022.07.09:
 * - page block내에 움직일수없는 page가 있는지 확인한다.
 *   이동이 가능한 page들이라면
 *   1. pageblock mt를 isolate로 전환
 *   2. pageblock에서 buddy에 있는것들은 page order isolate freelist로 옮긴다.
 */
	unmovable = has_unmovable_pages(zone, page, migratetype, isol_flags);
	if (!unmovable) {
		unsigned long nr_pages;
		int mt = get_pageblock_migratetype(page);

		set_pageblock_migratetype(page, MIGRATE_ISOLATE);
		zone->nr_isolate_pageblock++;
		nr_pages = move_freepages_block(zone, page, MIGRATE_ISOLATE,
									NULL);

		__mod_zone_freepage_state(zone, -nr_pages, mt);
		spin_unlock_irqrestore(&zone->lock, flags);
		return 0;
	}

	spin_unlock_irqrestore(&zone->lock, flags);
	if (isol_flags & REPORT_FAILURE) {
		/*
		 * printk() with zone->lock held will likely trigger a
		 * lockdep splat, so defer it here.
		 */
		dump_page(unmovable, "unmovable page");
	}

	return -EBUSY;
}

static void unset_migratetype_isolate(struct page *page, unsigned migratetype)
{
	struct zone *zone;
	unsigned long flags, nr_pages;
	bool isolated_page = false;
	unsigned int order;
	unsigned long pfn, buddy_pfn;
	struct page *buddy;

	zone = page_zone(page);
	spin_lock_irqsave(&zone->lock, flags);
	if (!is_migrate_isolate_page(page))
		goto out;

	/*
	 * Because freepage with more than pageblock_order on isolated
	 * pageblock is restricted to merge due to freepage counting problem,
	 * it is possible that there is free buddy page.
	 * move_freepages_block() doesn't care of merge so we need other
	 * approach in order to merge them. Isolation and free will make
	 * these pages to be merged.
	 */
/*
 * IAMROOT, 2022.07.09:
 * - papago
 *   isolated pageblock에 pageblock_order 이상의 freepage가 있는 freepage는
 *   freepage counting 문제로 merge가 제한되기 때문에 free buddy page가 있을
 *   가능성이 있다.
 *   move_freepages_block()은 병합을 신경 쓰지 않으므로 병합하려면 다른 접근
 *   방식이 필요합니다. Isolation 및 free는 이러한 페이지를 병합하도록 합니다.
 */
	if (PageBuddy(page)) {
		order = buddy_order(page);
		if (order >= pageblock_order && order < MAX_ORDER - 1) {
			pfn = page_to_pfn(page);
			buddy_pfn = __find_buddy_pfn(pfn, order);
			buddy = page + (buddy_pfn - pfn);

			if (!is_migrate_isolate_page(buddy)) {
				__isolate_free_page(page, order);
				isolated_page = true;
			}
		}
	}

	/*
	 * If we isolate freepage with more than pageblock_order, there
	 * should be no freepage in the range, so we could avoid costly
	 * pageblock scanning for freepage moving.
	 *
	 * We didn't actually touch any of the isolated pages, so place them
	 * to the tail of the freelist. This is an optimization for memory
	 * onlining - just onlined memory won't immediately be considered for
	 * allocation.
	 */
	if (!isolated_page) {
		nr_pages = move_freepages_block(zone, page, migratetype, NULL);
		__mod_zone_freepage_state(zone, nr_pages, migratetype);
	}
	set_pageblock_migratetype(page, migratetype);
	if (isolated_page)
		__putback_isolated_page(page, order, migratetype);
	zone->nr_isolate_pageblock--;
out:
	spin_unlock_irqrestore(&zone->lock, flags);
}

/*
 * IAMROOT, 2022.07.09:
 * - 첫번째 online page를 return.
 */
static inline struct page *
__first_valid_page(unsigned long pfn, unsigned long nr_pages)
{
	int i;

	for (i = 0; i < nr_pages; i++) {
		struct page *page;

		page = pfn_to_online_page(pfn + i);
		if (!page)
			continue;
		return page;
	}
	return NULL;
}

/**
 * start_isolate_page_range() - make page-allocation-type of range of pages to
 * be MIGRATE_ISOLATE.
 * @start_pfn:		The lower PFN of the range to be isolated.
 * @end_pfn:		The upper PFN of the range to be isolated.
 *			start_pfn/end_pfn must be aligned to pageblock_order.
 * @migratetype:	Migrate type to set in error recovery.
 * @flags:		The following flags are allowed (they can be combined in
 *			a bit mask)
 *			MEMORY_OFFLINE - isolate to offline (!allocate) memory
 *					 e.g., skip over PageHWPoison() pages
 *					 and PageOffline() pages.
 *			REPORT_FAILURE - report details about the failure to
 *			isolate the range
 *
 * Making page-allocation-type to be MIGRATE_ISOLATE means free pages in
 * the range will never be allocated. Any free pages and pages freed in the
 * future will not be allocated again. If specified range includes migrate types
 * other than MOVABLE or CMA, this will fail with -EBUSY. For isolating all
 * pages in the range finally, the caller have to free all pages in the range.
 * test_page_isolated() can be used for test it.
 *
 * There is no high level synchronization mechanism that prevents two threads
 * from trying to isolate overlapping ranges. If this happens, one thread
 * will notice pageblocks in the overlapping range already set to isolate.
 * This happens in set_migratetype_isolate, and set_migratetype_isolate
 * returns an error. We then clean up by restoring the migration type on
 * pageblocks we may have modified and return -EBUSY to caller. This
 * prevents two threads from simultaneously working on overlapping ranges.
 *
 * Please note that there is no strong synchronization with the page allocator
 * either. Pages might be freed while their page blocks are marked ISOLATED.
 * A call to drain_all_pages() after isolation can flush most of them. However
 * in some cases pages might still end up on pcp lists and that would allow
 * for their allocation even when they are in fact isolated already. Depending
 * on how strong of a guarantee the caller needs, zone_pcp_disable/enable()
 * might be used to flush and disable pcplist before isolation and enable after
 * unisolation.
 *
 * Return: 0 on success and -EBUSY if any part of range cannot be isolated.
 */
/*
 * IAMROOT, 2022.07.09:
 * - papgo
 *   페이지 할당 유형을 MIGRATE_ISOLATE로 설정하면 해당 범위의 사용 가능한
 *   페이지가 할당되지 않습니다. 이후에 사용 가능한 페이지와 페이지는 다시
 *   할당되지 않습니다. 지정된 범위에 MOVAL 또는 CMA가 아닌 마이그레이션 유형이
 *   포함된 경우 -EBUSY에서 실패합니다. 범위의 모든 페이지를 마지막으로 분리하려면
 *   호출자가 범위의 모든 페이지를 비워 두어야 합니다.
 *   test_page_filename은 테스트에 사용할 수 있습니다.
 *
 *   두 스레드가 겹치는 범위를 분리하는 것을 방지하는 높은 수준의 동기화
 *   메커니즘은 없습니다. 이 경우, 한 스레드는 이미 분리되도록 설정된 겹치는
 *   범위의 페이지 블록을 인식합니다.
 *   이 문제는 set_migratetype_isolate에서 발생하며 set_migratetype_isolate는
 *   오류를 반환합니다. 그런 다음 수정한 페이지 블록의 마이그레이션 유형을
 *   복원하여 정리하고 -EBUSY를 호출자에게 반환합니다. 이렇게 하면 두 개의
 *   스레드가 겹치는 범위에서 동시에 작동하지 않습니다. 
 *
 *   페이지 할당기와의 강력한 동기화도 없습니다. 페이지 블록이 분리되어 있는
 *   동안 페이지가 해제될 수 있습니다.
 *   분리 후 drain_all_pages()를 호출하면 대부분의 페이지를 플러시할 수 있습니다.
 *   그러나 경우에 따라서는 페이지가 pcp 목록에 남아 있을 수 있으며, 이는 페이지가
 *   이미 격리되어 있는 경우에도 페이지 할당을 허용한다. 호출자에게 필요한 보증
 *   강도에 따라 zone_pcp_disable/enable()을 사용하여 분리 전에 pcplist를 플러시
 *   및 해제하고 분리 해제 후 활성화할 수 있습니다.
 *
 *   Return: 성공 시 0이고 범위의 어느 부분도 분리할 수 없는 경우 -EBUSY입니다.
 *
 * - pageblock단위로 요청범위만큼 @migratetype으로 이동이 가능한지 확인하고
 *   1. pageblock단위로 mt를 isolate로 전환
 *   2. page중 buddy에 있는것들은 page order isolate freelist로 옮긴다.
 */
int start_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn,
			     unsigned migratetype, int flags)
{
	unsigned long pfn;
	unsigned long undo_pfn;
	struct page *page;

	BUG_ON(!IS_ALIGNED(start_pfn, pageblock_nr_pages));
	BUG_ON(!IS_ALIGNED(end_pfn, pageblock_nr_pages));

/*
 * IAMROOT, 2022.07.09:
 * - @start_pfn ~ @end_pfn 을 순회하며 online page가 있는지 확인한다.
 *   online page들은 isolate를 시킨다.
 */
	for (pfn = start_pfn;
	     pfn < end_pfn;
	     pfn += pageblock_nr_pages) {
		page = __first_valid_page(pfn, pageblock_nr_pages);
		if (page) {
			if (set_migratetype_isolate(page, migratetype, flags)) {
				undo_pfn = pfn;
				goto undo;
			}
		}
	}
	return 0;
undo:
/*
 * IAMROOT, 2022.07.09:
 * - 도중에 실패했다면 원복시킨다.
 */
	for (pfn = start_pfn;
	     pfn < undo_pfn;
	     pfn += pageblock_nr_pages) {
		struct page *page = pfn_to_online_page(pfn);
		if (!page)
			continue;
		unset_migratetype_isolate(page, migratetype);
	}

	return -EBUSY;
}

/*
 * Make isolated pages available again.
 */
void undo_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn,
			    unsigned migratetype)
{
	unsigned long pfn;
	struct page *page;

	BUG_ON(!IS_ALIGNED(start_pfn, pageblock_nr_pages));
	BUG_ON(!IS_ALIGNED(end_pfn, pageblock_nr_pages));

	for (pfn = start_pfn;
	     pfn < end_pfn;
	     pfn += pageblock_nr_pages) {
		page = __first_valid_page(pfn, pageblock_nr_pages);
		if (!page || !is_migrate_isolate_page(page))
			continue;
		unset_migratetype_isolate(page, migratetype);
	}
}
/*
 * Test all pages in the range is free(means isolated) or not.
 * all pages in [start_pfn...end_pfn) must be in the same zone.
 * zone->lock must be held before call this.
 *
 * Returns the last tested pfn.
 */
/*
 * IAMROOT, 2022.07.09:
 * -papago
 *  범위의 모든 페이지가 비어 있는지(격리됨을 의미) 테스트하십시오.
 *  [start_pfn...end_pfn)의 모든 페이지는 동일한 영역에 있어야 합니다.
 *  이것을 호출하기 전에 zone->lock을 유지해야 합니다.
 *
 *  마지막으로 테스트한 pfn을 반환합니다.
 *
 * - [pfn, end_pfn) 까지 buddy에 있는지 확인한다.
 */
static unsigned long
__test_page_isolated_in_pageblock(unsigned long pfn, unsigned long end_pfn,
				  int flags)
{
	struct page *page;

	while (pfn < end_pfn) {
		page = pfn_to_page(pfn);
		if (PageBuddy(page))
			/*
			 * If the page is on a free list, it has to be on
			 * the correct MIGRATE_ISOLATE freelist. There is no
			 * simple way to verify that as VM_BUG_ON(), though.
			 */
/*
 * IAMROOT, 2022.07.09:
 * - papago
 *   페이지가 사용 가능 목록에 있으면 올바른 MIGRATE_ISOLATE 사용 가능 목록에
 *   있어야 합니다. 그러나 VM_BUG_ON()으로 이를 확인할 수 있는 간단한 방법은
 *   없습니다. 
 */
			pfn += 1 << buddy_order(page);
		else if ((flags & MEMORY_OFFLINE) && PageHWPoison(page))
			/* A HWPoisoned page cannot be also PageBuddy */
			pfn++;
		else if ((flags & MEMORY_OFFLINE) && PageOffline(page) &&
			 !page_count(page))
			/*
			 * The responsible driver agreed to skip PageOffline()
			 * pages when offlining memory by dropping its
			 * reference in MEM_GOING_OFFLINE.
			 */
			pfn++;
		else
			break;
	}

	return pfn;
}

/* Caller should ensure that requested range is in a single zone */
/*
 * IAMROOT, 2022.07.09:
 * - 요청범위 page가 전부 buddy에 있는지 확인한다.
 */
int test_pages_isolated(unsigned long start_pfn, unsigned long end_pfn,
			int isol_flags)
{
	unsigned long pfn, flags;
	struct page *page;
	struct zone *zone;
	int ret;

	/*
	 * Note: pageblock_nr_pages != MAX_ORDER. Then, chunks of free pages
	 * are not aligned to pageblock_nr_pages.
	 * Then we just check migratetype first.
	 */
/*
 * IAMROOT, 2022.07.09:
 * - papago
 *   pageblock_nr_pages != MAX_ORDER. 그런 다음 사용 가능한 페이지 청크가
 *   pageblock_nr_pages에 정렬되지 않습니다. 그런 다음 먼저 migratetype을 확인합니다.
 */
	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		page = __first_valid_page(pfn, pageblock_nr_pages);
		if (page && !is_migrate_isolate_page(page))
			break;
	}
	page = __first_valid_page(start_pfn, end_pfn - start_pfn);
	if ((pfn < end_pfn) || !page) {
		ret = -EBUSY;
		goto out;
	}

	/* Check all pages are free or marked as ISOLATED */
	zone = page_zone(page);
	spin_lock_irqsave(&zone->lock, flags);
	pfn = __test_page_isolated_in_pageblock(start_pfn, end_pfn, isol_flags);
	spin_unlock_irqrestore(&zone->lock, flags);

	ret = pfn < end_pfn ? -EBUSY : 0;

out:
	trace_test_pages_isolated(start_pfn, end_pfn, pfn);

	return ret;
}
