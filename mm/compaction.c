// SPDX-License-Identifier: GPL-2.0
/*
 * linux/mm/compaction.c
 *
 * Memory compaction for the reduction of external fragmentation. Note that
 * this heavily depends upon page migration to do all the real heavy
 * lifting
 *
 * Copyright IBM Corp. 2007-2010 Mel Gorman <mel@csn.ul.ie>
 */
#include <linux/cpu.h>
#include <linux/swap.h>
#include <linux/migrate.h>
#include <linux/compaction.h>
#include <linux/mm_inline.h>
#include <linux/sched/signal.h>
#include <linux/backing-dev.h>
#include <linux/sysctl.h>
#include <linux/sysfs.h>
#include <linux/page-isolation.h>
#include <linux/kasan.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/page_owner.h>
#include <linux/psi.h>
#include "internal.h"

#ifdef CONFIG_COMPACTION
static inline void count_compact_event(enum vm_event_item item)
{
	count_vm_event(item);
}

static inline void count_compact_events(enum vm_event_item item, long delta)
{
	count_vm_events(item, delta);
}
#else
#define count_compact_event(item) do { } while (0)
#define count_compact_events(item, delta) do { } while (0)
#endif

#if defined CONFIG_COMPACTION || defined CONFIG_CMA

#define CREATE_TRACE_POINTS
#include <trace/events/compaction.h>

#define block_start_pfn(pfn, order)	round_down(pfn, 1UL << (order))
#define block_end_pfn(pfn, order)	ALIGN((pfn) + 1, 1UL << (order))
#define pageblock_start_pfn(pfn)	block_start_pfn(pfn, pageblock_order)
#define pageblock_end_pfn(pfn)		block_end_pfn(pfn, pageblock_order)

/*
 * Fragmentation score check interval for proactive compaction purposes.
 */
static const unsigned int HPAGE_FRAG_CHECK_INTERVAL_MSEC = 500;

/*
 * Page order with-respect-to which proactive compaction
 * calculates external fragmentation, which is used as
 * the "fragmentation score" of a node/zone.
 */
#if defined CONFIG_TRANSPARENT_HUGEPAGE
#define COMPACTION_HPAGE_ORDER	HPAGE_PMD_ORDER
#elif defined CONFIG_HUGETLBFS
#define COMPACTION_HPAGE_ORDER	HUGETLB_PAGE_ORDER
#else
#define COMPACTION_HPAGE_ORDER	(PMD_SHIFT - PAGE_SHIFT)
#endif

/*
 * IAMROOT, 2022.03.26: 
 * freelist의 각 페이지들을 buddy 시스템으로 되돌린다.
 * 되돌린 페이지 중 high pfn을 반환한다.
 */
static unsigned long release_freepages(struct list_head *freelist)
{
	struct page *page, *next;
	unsigned long high_pfn = 0;

	list_for_each_entry_safe(page, next, freelist, lru) {
		unsigned long pfn = page_to_pfn(page);
		list_del(&page->lru);
		__free_page(page);
		if (pfn > high_pfn)
			high_pfn = pfn;
	}

	return high_pfn;
}

static void split_map_pages(struct list_head *list)
{
	unsigned int i, order, nr_pages;
	struct page *page, *next;
	LIST_HEAD(tmp_list);

	list_for_each_entry_safe(page, next, list, lru) {
		list_del(&page->lru);

		order = page_private(page);
		nr_pages = 1 << order;

		post_alloc_hook(page, order, __GFP_MOVABLE);
		if (order)
			split_page(page, order);

		for (i = 0; i < nr_pages; i++) {
			list_add(&page->lru, &tmp_list);
			page++;
		}
	}

	list_splice(&tmp_list, list);
}

#ifdef CONFIG_COMPACTION

/*
 * IAMROOT, 2022.03.30:
 * @return 1 movable 가능.
 * - movable page인지를 완전히 확인한다.
 *   lock이 된채로 이 함수에 접근을 해야된다.
 * - __PageMovable에서는 movable page인지 peek 검사,
 *   mapping은 실제 driver에서 address_space가 존재하는지 검사한다.
 */
int PageMovable(struct page *page)
{
	struct address_space *mapping;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	if (!__PageMovable(page))
		return 0;

	mapping = page_mapping(page);
	if (mapping && mapping->a_ops && mapping->a_ops->isolate_page)
		return 1;

	return 0;
}
EXPORT_SYMBOL(PageMovable);

void __SetPageMovable(struct page *page, struct address_space *mapping)
{
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE((unsigned long)mapping & PAGE_MAPPING_MOVABLE, page);
	page->mapping = (void *)((unsigned long)mapping | PAGE_MAPPING_MOVABLE);
}
EXPORT_SYMBOL(__SetPageMovable);

void __ClearPageMovable(struct page *page)
{
	VM_BUG_ON_PAGE(!PageMovable(page), page);
	/*
	 * Clear registered address_space val with keeping PAGE_MAPPING_MOVABLE
	 * flag so that VM can catch up released page by driver after isolation.
	 * With it, VM migration doesn't try to put it back.
	 */
	page->mapping = (void *)((unsigned long)page->mapping &
				PAGE_MAPPING_MOVABLE);
}
EXPORT_SYMBOL(__ClearPageMovable);

/* Do not skip compaction more than 64 times */
#define COMPACT_MAX_DEFER_SHIFT 6

/*
 * Compaction is deferred when compaction fails to result in a page
 * allocation success. 1 << compact_defer_shift, compactions are skipped up
 * to a limit of 1 << COMPACT_MAX_DEFER_SHIFT
 */
/*
 * IAMROOT, 2022.03.19:
 * - compaction이 실패한 상황에서 관련값들을 설정한다. 실패할수록 derfer 조건을
 *   높인다. 매번 바로 시도해봤자 어짜피 실패할것이기때문이다.
 */
static void defer_compaction(struct zone *zone, int order)
{
	zone->compact_considered = 0;
	zone->compact_defer_shift++;

	if (order < zone->compact_order_failed)
		zone->compact_order_failed = order;

	if (zone->compact_defer_shift > COMPACT_MAX_DEFER_SHIFT)
		zone->compact_defer_shift = COMPACT_MAX_DEFER_SHIFT;

	trace_mm_compaction_defer_compaction(zone, order);
}

/* Returns true if compaction should be skipped this time */
/*
 * IAMROOT, 2022.03.19:
 * - 이전 compaction 결과를 참고해서 굳이 할 필요없는 compaction은
 *   지연시킨다.
 */
static bool compaction_deferred(struct zone *zone, int order)
{
	unsigned long defer_limit = 1UL << zone->compact_defer_shift;

/*
 * IAMROOT, 2022.03.19:
 * - 실패했던 order 보다 작으면 지연을 안한다.
 */
	if (order < zone->compact_order_failed)
		return false;

/*
 * IAMROOT, 2022.03.19:
 * - 여러번 지연이된 상태가 됬다면 지연을 안시킨다.
 */
	/* Avoid possible overflow */
	if (++zone->compact_considered >= defer_limit) {
		zone->compact_considered = defer_limit;
		return false;
	}

	trace_mm_compaction_deferred(zone, order);

	return true;
}

/*
 * Update defer tracking counters after successful compaction of given order,
 * which means an allocation either succeeded (alloc_success == true) or is
 * expected to succeed.
 */
/*
 * IAMROOT, 2022.03.19:
 * @alloc_success compaction 완료후에 실제 완전히 할당이 가능한 상태라면 true
 *                그게 아니라면 false.
 * compact defer 값들을 업데이트한다. @order + 1값으로 compact defer조건을
 * 완화한다.
 */
void compaction_defer_reset(struct zone *zone, int order,
		bool alloc_success)
{
	if (alloc_success) {
		zone->compact_considered = 0;
		zone->compact_defer_shift = 0;
	}
/*
 * IAMROOT, 2022.03.19:
 * - 이전에 compact fail했던 order 값보다 같거나 크다면 fail값을 + 1해준다.
 */
	if (order >= zone->compact_order_failed)
		zone->compact_order_failed = order + 1;

	trace_mm_compaction_defer_reset(zone, order);
}

/* Returns true if restarting compaction after many failures */
/*
 * IAMROOT, 2022.03.19:
 * - fail이 됬던 order보다 작은 @order라면 false, 크거나 같은 order라면
 *   defer가 아직 많이 안됬으면 false, defer가 많이 됬고 요청이 많았다면
 *   true return.
 */
static bool compaction_restarting(struct zone *zone, int order)
{
	if (order < zone->compact_order_failed)
		return false;

	return zone->compact_defer_shift == COMPACT_MAX_DEFER_SHIFT &&
		zone->compact_considered >= 1UL << zone->compact_defer_shift;
}

/* Returns true if the pageblock should be scanned for pages to isolate. */
/*
 * IAMROOT, 2022.03.26: 
 * ignore_skip_hint를 사용하기 전에는 해당 페이지가 소속된 페이지블럭의
 * skip 비트를 확인하여 isolation 적합여부를 판단한다.
 * @return @cc->ignore_skip_hint가 true면 skip을 확인 안하겠다는 것이므로
 *		   skip 확인없이 true, skip이 true면 false, skip이 false면 true
 */
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{
/*
 * IAMROOT, 2022.03.28:
 * - full scan이면 true로 set됬었을 것이다.
 */
	if (cc->ignore_skip_hint)
		return true;

	return !get_pageblock_skip(page);
}

static void reset_cached_positions(struct zone *zone)
{
	zone->compact_cached_migrate_pfn[0] = zone->zone_start_pfn;
	zone->compact_cached_migrate_pfn[1] = zone->zone_start_pfn;
	zone->compact_cached_free_pfn =
				pageblock_start_pfn(zone_end_pfn(zone) - 1);
}

/*
 * Compound pages of >= pageblock_order should consistently be skipped until
 * released. It is always pointless to compact pages of such order (if they are
 * migratable), and the pageblocks they occupy cannot contain any free pages.
 */
/*
 * IAMROOT, 2022.03.19:
 * - @page가 compound page고 pageblock_order단위 이상이면 true로 return한다.
 */
static bool pageblock_skip_persistent(struct page *page)
{
	if (!PageCompound(page))
		return false;

	page = compound_head(page);

	if (compound_order(page) >= pageblock_order)
		return true;

	return false;
}

/*
 * IAMROOT, 2022.03.19:
 * - @check_source : free쪽을 갱신할때 free set이 됬는지에 대한 여부.
 * - @check_target : migrate쪽을 갱신할때 migrate set이 됬는지에 대한 여부.
 * - @return true면 reset을 수행.
 *
 * -- git blame 참고
 *  tree b336a96e0c0073222608d70f4e029d259bfd8691
 *  parent 4fca9730c51d51f643f2a3f8f10ebd718349c80f
 *
 *  mm, compaction: be selective about what pageblocks to clear skip hints
 *
 *  Pageblock hints are cleared when compaction restarts or kswapd makes
 *  enough progress that it can sleep but it's over-eager in that the bit is
 *  cleared for migration sources with no LRU pages and migration targets
 *  with no free pages.  As pageblock skip hint flushes are relatively rare
 *  and out-of-band with respect to kswapd, this patch makes a few more
 *  expensive checks to see if it's appropriate to even clear the bit.
 *  Every pageblock that is not cleared will avoid 512 pages being scanned
 *  unnecessarily on x86-64.
 *
 * The impact is variable with different workloads showing small
 * differences in latency, success rates and scan rates.  This is expected
 * as clearing the hints is not that common but doing a small amount of
 * work out-of-band to avoid a large amount of work in-band later is
 * generally a good thing.
 *
 * -papago
 *  mm, compression: 생략 힌트를 지울 페이지 블록을 선택합니다.
 *
 *  페이지 블록 힌트는 압축이 재시작되거나 kswapd가 sleep 상태가 될 정도로
 *  진행되지만 LRU 페이지가 없는 마이그레이션 소스 및 빈 페이지가 없는
 *  마이그레이션 대상에 대해 비트가 지워진다는 점에서 over-eager이다.
 *  페이지 블록 skip hint flushes는 kswapd에 관해 비교적 드물고
 *  대역 외이기 때문에 이 패치는 비트 클리어까지 적절한지 확인하기 위해
 *  몇 가지 더 비용이 많이 듭니다. 삭제되지 않은 모든 페이지 블록은
 *  x86-64에서 512 페이지가 불필요하게 스캔되는 것을 방지합니다.
 *
 *  이 영향은 워크로드에 따라 달라지며 지연 시간, 성공률 및 검색 속도에서
 *  작은 차이를 보입니다. 이는 힌트를 클리어하는 것이 일반적이지 않기
 *  때문에 예상되지만 나중에 많은 양의 인밴드 작업을 피하기 위해 적은 양의
 *  아웃오브밴드 작업을 하는 것이 일반적으로 좋습니다.
 */
static bool
__reset_isolation_pfn(struct zone *zone, unsigned long pfn, bool check_source,
							bool check_target)
{
	struct page *page = pfn_to_online_page(pfn);
	struct page *block_page;
	struct page *end_page;
	unsigned long block_pfn;

	if (!page)
		return false;
	if (zone != page_zone(page))
		return false;
/*
 * IAMROOT, 2022.03.19:
 * - huge page일 경우 isolate를 안한다.
 */
	if (pageblock_skip_persistent(page))
		return false;

	/*
	 * If skip is already cleared do no further checking once the
	 * restart points have been set.
	 */
/*
 * IAMROOT, 2022.03.19:
 * - @check_source, @check_target이 전부 true라면 한번은 true 된것이므로
 *   해당 page block의 skip이 이미 clear라면 굳이 뒤에 루틴을 할필요없으므로
 *   (skip bit를 clear하러갈 필요가 없으므로) return 한다.
 */
	if (check_source && check_target && !get_pageblock_skip(page))
		return true;

	/*
	 * If clearing skip for the target scanner, do not select a
	 * non-movable pageblock as the starting point.
	 */
/*
 * IAMROOT, 2022.03.19:
 * - @!check_source 의 의미는 free_set을 확인하고 있는 의미 이므로
 *   movable이 아니면 false
 */
	if (!check_source && check_target &&
	    get_pageblock_migratetype(page) != MIGRATE_MOVABLE)
		return false;

/*
 * IAMROOT, 2022.03.19:
 * - pfn이 소속되있는 block의 start page를 가져온다. 성공하면
 *   isoloate를 block단위로 수행하기 @page, start ,end를 계산한다.
 */
	/* Ensure the start of the pageblock or zone is online and valid */
	block_pfn = pageblock_start_pfn(pfn);
	block_pfn = max(block_pfn, zone->zone_start_pfn);
	block_page = pfn_to_online_page(block_pfn);
	if (block_page) {
		page = block_page;
		pfn = block_pfn;
	}

/*
 * IAMROOT, 2022.03.19:
 * - pfn이 소속되있는 block의 end page를 가져온다.
 */
	/* Ensure the end of the pageblock or zone is online and valid */
	block_pfn = pageblock_end_pfn(pfn) - 1;
	block_pfn = min(block_pfn, zone_end_pfn(zone) - 1);
	end_page = pfn_to_online_page(block_pfn);
	if (!end_page)
		return false;

	/*
	 * Only clear the hint if a sample indicates there is either a
	 * free page or an LRU page in the block. One or other condition
	 * is necessary for the block to be a migration source/target.
	 */
/*
 * IAMROOT, 2022.03.19:
 * - 해당 block내에서 costly_order단위로 pageblock skip clear를 해야되는지
 *   확인한다.
 * - skip clear의 의미는 pageblock내를 전부 조사하겠다는 의미가 된다.
 */
	do {
		if (check_source && PageLRU(page)) {
			clear_pageblock_skip(page);
			return true;
		}

		if (check_target && PageBuddy(page)) {
			clear_pageblock_skip(page);
			return true;
		}

		page += (1 << PAGE_ALLOC_COSTLY_ORDER);
		pfn += (1 << PAGE_ALLOC_COSTLY_ORDER);
	} while (page <= end_page);

	return false;
}

/*
 * This function is called to clear all cached information on pageblocks that
 * should be skipped for page isolation when the migrate and free page scanner
 * meet.
 */
static void __reset_isolation_suitable(struct zone *zone)
{
	unsigned long migrate_pfn = zone->zone_start_pfn;
	unsigned long free_pfn = zone_end_pfn(zone) - 1;
	unsigned long reset_migrate = free_pfn;
	unsigned long reset_free = migrate_pfn;
	bool source_set = false;
	bool free_set = false;

/*
 * IAMROOT, 2022.03.19:
 * - 중복동작 방지.
 */
	if (!zone->compact_blockskip_flush)
		return;

	zone->compact_blockskip_flush = false;

	/*
	 * Walk the zone and update pageblock skip information. Source looks
	 * for PageLRU while target looks for PageBuddy. When the scanner
	 * is found, both PageBuddy and PageLRU are checked as the pageblock
	 * is suitable as both source and target.
	 */
/*
 * IAMROOT, 2022.03.19:
 * - migrate_pfn ~ free_pfn까지 pageblock 단위로 iterate한다.
 * 
 * high
 * ----------- 
 *            |
 *            | free(target) scanner
 *            |
 *            v 
 *  MEMORY         
 *            ^
 *            |
 *            | migrate(source) scanner
 *            |
 * -----------
 * low
 */
	for (; migrate_pfn < free_pfn; migrate_pfn += pageblock_nr_pages,
					free_pfn -= pageblock_nr_pages) {
		cond_resched();

		/* Update the migrate PFN */
		if (__reset_isolation_pfn(zone, migrate_pfn, true, source_set) &&
		    migrate_pfn < reset_migrate) {
/*
 * IAMROOT, 2022.03.19:
 * - __reset_isolation_pfn이 최초에 한번 true로 됬을대 해당 if문이 실행된다.
 */
			source_set = true;
			reset_migrate = migrate_pfn;
			zone->compact_init_migrate_pfn = reset_migrate;
			zone->compact_cached_migrate_pfn[0] = reset_migrate;
			zone->compact_cached_migrate_pfn[1] = reset_migrate;
		}

		/* Update the free PFN */
		if (__reset_isolation_pfn(zone, free_pfn, free_set, true) &&
		    free_pfn > reset_free) {
/*
 * IAMROOT, 2022.03.19:
 * - __reset_isolation_pfn이 최초에 한번 true로 됬을대 해당 if문이 실행된다.
 */
			free_set = true;
			reset_free = free_pfn;
			zone->compact_init_free_pfn = reset_free;
			zone->compact_cached_free_pfn = reset_free;
		}
	}

/*
 * IAMROOT, 2022.03.19:
 * - 위 for문에서 source, free가 찾아진 경우
 * high
 * ----------- 
 *            |
 *            + (최초에 free_set true가 찾아진 지점)reset_free
 *  ...
 *            |
 *            + (중앙지점) migrate_pfn, free_pfn
 *            |
 *  ...
 *            + (최초에 source_set true가 찾아진 지점)reset_migrate
 *            |
 * -----------
 * low
 *
 * - 위 for문에서 source, free가 못찾아진 경우
 * high
 * -----------+ reset_migrate
 *            |
 *  ...
 *            |
 *            + (중앙지점) migrate_pfn, free_pfn
 *            |
 *  ...
 *            |
 * -----------+ reset_free
 * low
 *
 * - 위 for문에서 migrate, free 둘다 reset이 안된경우 if문이 실행된다.
 *   scanning을 못한다는 의미가 된다.
 */
	/* Leave no distance if no suitable block was reset */
	if (reset_migrate >= reset_free) {
		zone->compact_cached_migrate_pfn[0] = migrate_pfn;
		zone->compact_cached_migrate_pfn[1] = migrate_pfn;
		zone->compact_cached_free_pfn = free_pfn;
	}
}

void reset_isolation_suitable(pg_data_t *pgdat)
{
	int zoneid;

	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
		struct zone *zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		/* Only flush if a full compaction finished recently */
		if (zone->compact_blockskip_flush)
			__reset_isolation_suitable(zone);
	}
}

/*
 * Sets the pageblock skip bit if it was clear. Note that this is a hint as
 * locks are not required for read/writers. Returns true if it was already set.
 */
/*
 * IAMROOT, 2022.03.26: 
 * 해당 페이지가 있는 페이지 블럭의 skip bit 여부를 알아오고, skip bit를 설정한다.
 */
static bool test_and_set_skip(struct compact_control *cc, struct page *page,
							unsigned long pfn)
{
	bool skip;

	/* Do no update if skip hint is being ignored */
	if (cc->ignore_skip_hint)
		return false;

	if (!IS_ALIGNED(pfn, pageblock_nr_pages))
		return false;

	skip = get_pageblock_skip(page);
	if (!skip && !cc->no_set_skip_hint)
		set_pageblock_skip(page);

	return skip;
}

/*
 * IAMROOT, 2022.03.26: 
 * 해당 @pfn이 속한 블럭의 다음 블럭 위치로 migrate 캐시를 기억한다. 
 */
static void update_cached_migrate(struct compact_control *cc, unsigned long pfn)
{
	struct zone *zone = cc->zone;

	pfn = pageblock_end_pfn(pfn);
/*
 * IAMROOT, 2022.03.29:
 * - alloc_contig_range요청일때만 true가 되는듯하다.
 */
	/* Set for isolation rather than compaction */
	if (cc->no_set_skip_hint)
		return;

	if (pfn > zone->compact_cached_migrate_pfn[0])
		zone->compact_cached_migrate_pfn[0] = pfn;
	if (cc->mode != MIGRATE_ASYNC &&
	    pfn > zone->compact_cached_migrate_pfn[1])
		zone->compact_cached_migrate_pfn[1] = pfn;
}

/*
 * If no pages were isolated then mark this pageblock to be skipped in the
 * future. The information is later cleared by __reset_isolation_suitable().
 */
static void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long pfn)
{
	struct zone *zone = cc->zone;

	if (cc->no_set_skip_hint)
		return;

	if (!page)
		return;

	set_pageblock_skip(page);

	/* Update where async and sync compaction should restart */
	if (pfn < zone->compact_cached_free_pfn)
		zone->compact_cached_free_pfn = pfn;
}
#else
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{
	return true;
}

static inline bool pageblock_skip_persistent(struct page *page)
{
	return false;
}

static inline void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long pfn)
{
}

static void update_cached_migrate(struct compact_control *cc, unsigned long pfn)
{
}

static bool test_and_set_skip(struct compact_control *cc, struct page *page,
							unsigned long pfn)
{
	return false;
}
#endif /* CONFIG_COMPACTION */

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. For async compaction, trylock and record if the
 * lock is contended. The lock will still be acquired but compaction will
 * abort when the current block is finished regardless of success rate.
 * Sync compaction acquires the lock.
 *
 * Always returns true which makes it easier to track lock state in callers.
 */
static bool compact_lock_irqsave(spinlock_t *lock, unsigned long *flags,
						struct compact_control *cc)
	__acquires(lock)
{
	/* Track if the lock is contended in async mode */
	if (cc->mode == MIGRATE_ASYNC && !cc->contended) {
		if (spin_trylock_irqsave(lock, *flags))
			return true;

		cc->contended = true;
	}

	spin_lock_irqsave(lock, *flags);
	return true;
}

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. The lock should be periodically unlocked to avoid
 * having disabled IRQs for a long time, even when there is nobody waiting on
 * the lock. It might also be that allowing the IRQs will result in
 * need_resched() becoming true. If scheduling is needed, async compaction
 * aborts. Sync compaction schedules.
 * Either compaction type will also abort if a fatal signal is pending.
 * In either case if the lock was locked, it is dropped and not regained.
 *
 * Returns true if compaction should abort due to fatal signal pending, or
 *		async compaction due to need_resched()
 * Returns false when compaction can continue (sync compaction might have
 *		scheduled)
 */
static bool compact_unlock_should_abort(spinlock_t *lock,
		unsigned long flags, bool *locked, struct compact_control *cc)
{
	if (*locked) {
		spin_unlock_irqrestore(lock, flags);
		*locked = false;
	}

	if (fatal_signal_pending(current)) {
		cc->contended = true;
		return true;
	}

	cond_resched();

	return false;
}

/*
 * Isolate free pages onto a private freelist. If @strict is true, will abort
 * returning 0 on any invalid PFNs or non-free pages inside of the pageblock
 * (even though it may still end up isolating some pages).
 */
static unsigned long isolate_freepages_block(struct compact_control *cc,
				unsigned long *start_pfn,
				unsigned long end_pfn,
				struct list_head *freelist,
				unsigned int stride,
				bool strict)
{
	int nr_scanned = 0, total_isolated = 0;
	struct page *cursor;
	unsigned long flags = 0;
	bool locked = false;
	unsigned long blockpfn = *start_pfn;
	unsigned int order;

	/* Strict mode is for isolation, speed is secondary */
	if (strict)
		stride = 1;

	cursor = pfn_to_page(blockpfn);

	/* Isolate free pages. */
	for (; blockpfn < end_pfn; blockpfn += stride, cursor += stride) {
		int isolated;
		struct page *page = cursor;

		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort if fatal signal
		 * pending or async compaction detects need_resched()
		 */
		if (!(blockpfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(&cc->zone->lock, flags,
								&locked, cc))
			break;

		nr_scanned++;

		/*
		 * For compound pages such as THP and hugetlbfs, we can save
		 * potentially a lot of iterations if we skip them at once.
		 * The check is racy, but we can consider only valid values
		 * and the only danger is skipping too much.
		 */
		if (PageCompound(page)) {
			const unsigned int order = compound_order(page);

			if (likely(order < MAX_ORDER)) {
				blockpfn += (1UL << order) - 1;
				cursor += (1UL << order) - 1;
			}
			goto isolate_fail;
		}

		if (!PageBuddy(page))
			goto isolate_fail;

		/*
		 * If we already hold the lock, we can skip some rechecking.
		 * Note that if we hold the lock now, checked_pageblock was
		 * already set in some previous iteration (or strict is true),
		 * so it is correct to skip the suitable migration target
		 * recheck as well.
		 */
		if (!locked) {
			locked = compact_lock_irqsave(&cc->zone->lock,
								&flags, cc);

			/* Recheck this is a buddy page under lock */
			if (!PageBuddy(page))
				goto isolate_fail;
		}

		/* Found a free page, will break it into order-0 pages */
		order = buddy_order(page);
		isolated = __isolate_free_page(page, order);
		if (!isolated)
			break;
		set_page_private(page, order);

		total_isolated += isolated;
		cc->nr_freepages += isolated;
		list_add_tail(&page->lru, freelist);

		if (!strict && cc->nr_migratepages <= cc->nr_freepages) {
			blockpfn += isolated;
			break;
		}
		/* Advance to the end of split page */
		blockpfn += isolated - 1;
		cursor += isolated - 1;
		continue;

isolate_fail:
		if (strict)
			break;
		else
			continue;

	}

	if (locked)
		spin_unlock_irqrestore(&cc->zone->lock, flags);

	/*
	 * There is a tiny chance that we have read bogus compound_order(),
	 * so be careful to not go outside of the pageblock.
	 */
	if (unlikely(blockpfn > end_pfn))
		blockpfn = end_pfn;

	trace_mm_compaction_isolate_freepages(*start_pfn, blockpfn,
					nr_scanned, total_isolated);

	/* Record how far we have got within the block */
	*start_pfn = blockpfn;

	/*
	 * If strict isolation is requested by CMA then check that all the
	 * pages requested were isolated. If there were any failures, 0 is
	 * returned and CMA will fail.
	 */
	if (strict && blockpfn < end_pfn)
		total_isolated = 0;

	cc->total_free_scanned += nr_scanned;
	if (total_isolated)
		count_compact_events(COMPACTISOLATED, total_isolated);
	return total_isolated;
}

/**
 * isolate_freepages_range() - isolate free pages.
 * @cc:        Compaction control structure.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Non-free pages, invalid PFNs, or zone boundaries within the
 * [start_pfn, end_pfn) range are considered errors, cause function to
 * undo its actions and return zero.
 *
 * Otherwise, function returns one-past-the-last PFN of isolated page
 * (which may be greater then end_pfn if end fell in a middle of
 * a free page).
 */
unsigned long
isolate_freepages_range(struct compact_control *cc,
			unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long isolated, pfn, block_start_pfn, block_end_pfn;
	LIST_HEAD(freelist);

	pfn = start_pfn;
	block_start_pfn = pageblock_start_pfn(pfn);
	if (block_start_pfn < cc->zone->zone_start_pfn)
		block_start_pfn = cc->zone->zone_start_pfn;
	block_end_pfn = pageblock_end_pfn(pfn);

	for (; pfn < end_pfn; pfn += isolated,
				block_start_pfn = block_end_pfn,
				block_end_pfn += pageblock_nr_pages) {
		/* Protect pfn from changing by isolate_freepages_block */
		unsigned long isolate_start_pfn = pfn;

		block_end_pfn = min(block_end_pfn, end_pfn);

		/*
		 * pfn could pass the block_end_pfn if isolated freepage
		 * is more than pageblock order. In this case, we adjust
		 * scanning range to right one.
		 */
		if (pfn >= block_end_pfn) {
			block_start_pfn = pageblock_start_pfn(pfn);
			block_end_pfn = pageblock_end_pfn(pfn);
			block_end_pfn = min(block_end_pfn, end_pfn);
		}

		if (!pageblock_pfn_to_page(block_start_pfn,
					block_end_pfn, cc->zone))
			break;

		isolated = isolate_freepages_block(cc, &isolate_start_pfn,
					block_end_pfn, &freelist, 0, true);

		/*
		 * In strict mode, isolate_freepages_block() returns 0 if
		 * there are any holes in the block (ie. invalid PFNs or
		 * non-free pages).
		 */
		if (!isolated)
			break;

		/*
		 * If we managed to isolate pages, it is always (1 << n) *
		 * pageblock_nr_pages for some non-negative n.  (Max order
		 * page may span two pageblocks).
		 */
	}

	/* __isolate_free_page() does not map the pages */
	split_map_pages(&freelist);

	if (pfn < end_pfn) {
		/* Loop terminated early, cleanup. */
		release_freepages(&freelist);
		return 0;
	}

	/* We don't use freelists for anything. */
	return pfn;
}

/* Similar to reclaim, but different enough that they don't share logic */
/*
 * IAMROOT, 2022.03.26: 
 * isolation된 페이지가 4개 lru 총합 페이지의 절반을 초과하는 경우 true를 반환
 */
static bool too_many_isolated(pg_data_t *pgdat)
{
	unsigned long active, inactive, isolated;

	inactive = node_page_state(pgdat, NR_INACTIVE_FILE) +
			node_page_state(pgdat, NR_INACTIVE_ANON);
	active = node_page_state(pgdat, NR_ACTIVE_FILE) +
			node_page_state(pgdat, NR_ACTIVE_ANON);
	isolated = node_page_state(pgdat, NR_ISOLATED_FILE) +
			node_page_state(pgdat, NR_ISOLATED_ANON);

	return isolated > (inactive + active) / 2;
}

/**
 * isolate_migratepages_block() - isolate all migrate-able pages within
 *				  a single pageblock
 * @cc:		Compaction control structure.
 * @low_pfn:	The first PFN to isolate
 * @end_pfn:	The one-past-the-last PFN to isolate, within same pageblock
 * @isolate_mode: Isolation mode to be used.
 *
 * Isolate all pages that can be migrated from the range specified by
 * [low_pfn, end_pfn). The range is expected to be within same pageblock.
 * Returns errno, like -EAGAIN or -EINTR in case e.g signal pending or congestion,
 * -ENOMEM in case we could not allocate a page, or 0.
 * cc->migrate_pfn will contain the next pfn to scan.
 *
 * The pages are isolated on cc->migratepages list (not required to be empty),
 * and cc->nr_migratepages is updated accordingly.
 */
/*
 * IAMROOT, 2022.03.26: 
 * 1 페이지블럭내 페이지들을 순회하며 migrate 가능한 lru 페이지들을 분리하여 
 * cc->migratepages 리스트로 이동한다. 이동시 lru 플래그를 제거한다.
 * 
 * isolate를 중단하는 case:
 *-------------------------
 * 1) 너무 많은 페이지를 isolate한 경우 (lru의 절반 이상)
 * 2) 페이지블럭에 skip 비트가 있는 경우, 물론 ignore_skip_hint=0 일때

 * 이동이 불가능한 페이지들:
 *--------------------------
 * 1) buddy 페이지
 * 2) alloc_contig_range() 호출이 아닌 상태에서 huge 페이지
 * 3) non-lru movable 페이지를 제외한 non-lru 페이지
 * 4) pinned anon 페이지
 * 5) FS 허용하지 않을 때의 파일 매핑된 페이지
 * 6) lru 페이지중 async 모드에서 다음 조건에 해당되는 페이지
 *    - write-back 페이지
 *    - migratepage()가 구현되지 않은 fs에 있는 파일 매핑된 페이지 
 * 7) unmapped 페이지들만 요청한 시 파일 매핑된 페이지
 * 8) alloc_contig_range() 호출인 상태에서 compound 페이지
 */

static int
isolate_migratepages_block(struct compact_control *cc, unsigned long low_pfn,
			unsigned long end_pfn, isolate_mode_t isolate_mode)
{
	pg_data_t *pgdat = cc->zone->zone_pgdat;
	unsigned long nr_scanned = 0, nr_isolated = 0;
	struct lruvec *lruvec;
	unsigned long flags = 0;
	struct lruvec *locked = NULL;
	struct page *page = NULL, *valid_page = NULL;
	unsigned long start_pfn = low_pfn;
	bool skip_on_failure = false;
	unsigned long next_skip_pfn = 0;
	bool skip_updated = false;
	int ret = 0;

	cc->migrate_pfn = low_pfn;

	/*
	 * Ensure that there are not too many pages isolated from the LRU
	 * list by either parallel reclaimers or compaction. If there are,
	 * delay for some time until fewer pages are isolated
	 */
/*
 * IAMROOT, 2022.03.26: 
 * 너무 많은 페이지들이 isolation 되었는지 확인한다.
 * 이러한 경우 이미 isolation한 페이지가 있거나, async 모드로 진입한 경우,
 * 그리고 fatal signal인 경우 여기까지만 isolation할 목적으로 중단한다.
 * 그 외의 경우 0.1초 간격으로 다시 루프를 돈다.
 */
	while (unlikely(too_many_isolated(pgdat))) {
		/* stop isolation if there are still pages not migrated */
		if (cc->nr_migratepages)
			return -EAGAIN;

		/* async migration should just abort */
		if (cc->mode == MIGRATE_ASYNC)
			return -EAGAIN;

		congestion_wait(BLK_RW_ASYNC, HZ/10);

		if (fatal_signal_pending(current))
			return -EINTR;
	}

	cond_resched();

/*
 * IAMROOT, 2022.03.26: 
 * async 모드의 direct-compaction 호출인 경우 skip_on_failure를 true로 변경한다.
 * 또한 해당 order 단위로 low_pfn값에 해당하는 pfn을 next_skip_pfn에 대입해둔다.
 */

	if (cc->direct_compaction && (cc->mode == MIGRATE_ASYNC)) {
		skip_on_failure = true;
		next_skip_pfn = block_end_pfn(low_pfn, cc->order);
	}

	/* Time to isolate some pages for migration */
/*
 * IAMROOT, 2022.03.31:
 * -
 */
	for (; low_pfn < end_pfn; low_pfn++) {

/*
 * IAMROOT, 2022.03.26: 
 *
 * 1개 이상 isolation된 페이지가 있고, 이미 next_skip_pfn위치를 지났으면
 * 루프를 break 한다.
 * 이 과정에서 아직 isolate한 페이지가 없으면 next_skip_pfn만 갱신한다.
 *
 * - async direct compaction 일때만 low_pfn >= next_skip_pfn 조건을 확인할것이다.
 *   다시 말해 적당한 범위에서 isolate가 찾아지면 종료가 가능하다.
 *   sync 일때는 해당 if문을 탈일이 없다. 즉 범위문제로 중간에 멈추지 않고
 *   쭉 진행할것이다.
 *
 * --- low_pfn 관점에서 if문 진입
 *
 * - 평범하게 for문을 진입하는경우
 *   next_skip_pfn은 최초에 cc->order만큼의 distance를 가질것이고, isolate가
 *   없거나 isolate 실패를 한상황이면 cc->order만큼 더 범위를 검사하는 식으로
 *   진행이 된다.
 *   
 * - conintue를 통해서 for문을 재진입한경우
 *   -- buddy : buddy page 크기만큼 low_pfn이 증가되서 진입한다.
 *   만약 skip후 low_pfn이 next_skip_pfn보다 커지게되면 진입할것이다.
 *   즉 이 경우엔 해당 if문을 진입할 일이없다.
 *
 *   -- isolate에 성공 : migrate page가 아직 부족하거나, rescan요청이 있거나,
 *   경합중이면 low_pfn++로 진입한다.(compound page나 huge page였을 경우 해당
 *   page 크기만큼 low_pfn이 증가된채로 진입) next_skip_pfn이 갱신이 잘 안된다면
 *   cc->order만큼만 검사를 하고 그 사이에 isolate가 된게 하나라도 있으면
 *   해당 if문에서 빠져나가게 될것이다.
 *
 *   -- isolate fail : 
 *   --
 */
		if (skip_on_failure && low_pfn >= next_skip_pfn) {
			/*
			 * We have isolated all migration candidates in the
			 * previous order-aligned block, and did not skip it due
			 * to failure. We should migrate the pages now and
			 * hopefully succeed compaction.
			 */
			if (nr_isolated)
				break;

			/*
			 * We failed to isolate in the previous order-aligned
			 * block. Set the new boundary to the end of the
			 * current block. Note we can't simply increase
			 * next_skip_pfn by 1 << order, as low_pfn might have
			 * been incremented by a higher number due to skipping
			 * a compound or a high-order buddy page in the
			 * previous loop iteration.
			 */
			next_skip_pfn = block_end_pfn(low_pfn, cc->order);
		}

		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort completely if
		 * a fatal signal is pending.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * 장시간 isolation 하는 경우이므로 여기에도 preemption point를 호출한다.
 * 또한 중간 중간 interrupt가 진입할 수 있도록 lock을 한번씩 풀어준다.
 */
		if (!(low_pfn % SWAP_CLUSTER_MAX)) {
			if (locked) {
				unlock_page_lruvec_irqrestore(locked, flags);
				locked = NULL;
			}

			if (fatal_signal_pending(current)) {
				cc->contended = true;
				ret = -EINTR;

				goto fatal_pending;
			}

			cond_resched();
		}

		nr_scanned++;

		page = pfn_to_page(low_pfn);

		/*
		 * Check if the pageblock has already been marked skipped.
		 * Only the aligned PFN is checked as the caller isolates
		 * COMPACT_CLUSTER_MAX at a time so the second call must
		 * not falsely conclude that the block should be skipped.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * 1 페이지 블럭단위로 처음 만난 페이지를 valid_page로 갱신한다.
 * skip 블럭인 경우 isolate를 중단한다.
 */
		if (!valid_page && IS_ALIGNED(low_pfn, pageblock_nr_pages)) {
/*
 * IAMROOT, 2022.03.31:
 * skip hint 무시 요청이 없을때만 동작한다.
 * - isolate가 끝난 상황에서 isolate된게 한개도 없거나 rescan요청이 있는 경우,
 *   즉 isolate가 할게 없거나 이미 꽤 많은 범위를 검색 했는데 lock을 해본적이
 *   없다면 valid_page skip으로 설정하는 데, 만약 skip이 지워지기 전이나
 *   빠른시간안에 누군가에 요청해서 같은 시작주소로 isolate가 수행되게 되면
 *   해봤자 isolate할게 적거나 이미 할만큼 한 상태라 시간낭비이므로
 *   abort 시킨다.
 */
			if (!cc->ignore_skip_hint && get_pageblock_skip(page)) {
				low_pfn = end_pfn;
				page = NULL;
				goto isolate_abort;
			}
			valid_page = page;
		}

/*
 * IAMROOT, 2022.03.26: 
 * alloc_contig_range() 함수를 사용하여 hugepage를 isolate를 시도한다.
 */
		if (PageHuge(page) && cc->alloc_contig) {
			ret = isolate_or_dissolve_huge_page(page, &cc->migratepages);

			/*
			 * Fail isolation in case isolate_or_dissolve_huge_page()
			 * reports an error. In case of -ENOMEM, abort right away.
			 */
			if (ret < 0) {
				 /* Do not report -EBUSY down the chain */
				if (ret == -EBUSY)
					ret = 0;
				low_pfn += (1UL << compound_order(page)) - 1;
				goto isolate_fail;
			}

			if (PageHuge(page)) {
				/*
				 * Hugepage was successfully isolated and placed
				 * on the cc->migratepages list.
				 */
				low_pfn += compound_nr(page) - 1;
				goto isolate_success_no_list;
			}

			/*
			 * Ok, the hugepage was dissolved. Now these pages are
			 * Buddy and cannot be re-allocated because they are
			 * isolated. Fall-through as the check below handles
			 * Buddy pages.
			 */
		}

		/*
		 * Skip if free. We read page order here without zone lock
		 * which is generally unsafe, but the race window is small and
		 * the worst thing that can happen is that we skip some
		 * potential isolation targets.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * 할당되어 사용되는 페이지들을 대상으로 isolate해야 하므로 버디 페이지들은 
 * skip 한다.
 */
		if (PageBuddy(page)) {
			unsigned long freepage_order = buddy_order_unsafe(page);

			/*
			 * Without lock, we cannot be sure that what we got is
			 * a valid page order. Consider only values in the
			 * valid order range to prevent low_pfn overflow.
			 */
/*
 * IAMROOT, 2022.03.30:
 * - unsafe로 가져왔기 때문에 유효한 order 인지만 체크하고 처리한다.
 */
			if (freepage_order > 0 && freepage_order < MAX_ORDER)
				low_pfn += (1UL << freepage_order) - 1;
			continue;
		}

		/*
		 * Regardless of being on LRU, compound pages such as THP and
		 * hugetlbfs are not to be compacted unless we are attempting
		 * an allocation much larger than the huge page size (eg CMA).
		 * We can potentially save a lot of iterations if we skip them
		 * at once. The check is racy, but we can consider only valid
		 * values and the only danger is skipping too much.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * alloc_contig_range()로 할당 요청한 경우가 아니면 THP 또는 hugetlbfs 같은
 *  compound 페이지들은 isolate_fail로 이동한다.
 *  low_pfn을 compund page의 next page로 설정한다.
 */
		if (PageCompound(page) && !cc->alloc_contig) {
			const unsigned int order = compound_order(page);

			if (likely(order < MAX_ORDER))
				low_pfn += (1UL << order) - 1;
			goto isolate_fail;
		}

/*
 * IAMROOT, 2022.03.31:
 * ----------- 이시점에 compound page는 모두 cc->alloc_contig = true  -----------
 */
		/*
		 * Check may be lockless but that's ok as we recheck later.
		 * It's possible to migrate LRU and non-lru movable pages.
		 * Skip any other type of page
		 */
/*
 * IAMROOT, 2022.03.26: 
 * non-lru 페이지들은 isolate_fail로 이동한다. 단 non-lru 페이지들 중
 * non-lru movable 페이지는 isolate 시도를 한다. 
 */
		if (!PageLRU(page)) {
			/*
			 * __PageMovable can return false positive so we need
			 * to verify it under page_lock.
			 */
/*
 * IAMROOT, 2022.03.30:
 * - enum pageflags 위 주석 참고
 * - __PageMovable을 통하여 non-lru movable page인지 peek한다.
 * - PageIsolated를 통해여 이미 분리된 page인지 확인한다.
 */
			if (unlikely(__PageMovable(page)) &&
					!PageIsolated(page)) {
				if (locked) {
					unlock_page_lruvec_irqrestore(locked, flags);
					locked = NULL;
				}

				if (!isolate_movable_page(page, isolate_mode))
					goto isolate_success;
			}

			goto isolate_fail;
		}

/*
 * IAMROOT, 2022.03.31:
 * ----------- 이시점에 LRU page들만 남게 된다. -----------
 */
		/*
		 * Migration will fail if an anonymous page is pinned in memory,
		 * so avoid taking lru_lock and isolating it unnecessarily in an
		 * admittedly racy check.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * pinned anon 페이지인 경우 isolate_fail로 이동한다.
 * 참조카운터(_count)가 매핑카운터(_mapcount)보다 큰 경우 pinned 페이지
 */
		if (!page_mapping(page) &&
		    page_count(page) > page_mapcount(page))
			goto isolate_fail;

		/*
		 * Only allow to migrate anonymous pages in GFP_NOFS context
		 * because those do not depend on fs locks.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * FS(file system)를 이용한 reclaim이 허용되지 않았기 때문에 파일 매핑된 페이지는
 * isolate_fail로 이동한다.
 */
		if (!(cc->gfp_mask & __GFP_FS) && page_mapping(page))
			goto isolate_fail;

		/*
		 * Be careful not to clear PageLRU until after we're
		 * sure the page is not being freed elsewhere -- the
		 * page release code relies on it.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * 페이지에 대한 참조 카운터를 획득한다. 만일 실패하는 경우 isolate_fail로 
 * 이동한다.
 */
		if (unlikely(!get_page_unless_zero(page)))
			goto isolate_fail;

/*
 * IAMROOT, 2022.03.26: 
 * lru 페이지가 migrate가 가능한 상태가 아니면 isolate_fail_put으로 이동한다.
 */
		if (!__isolate_lru_page_prepare(page, isolate_mode))
			goto isolate_fail_put;

/*
 * IAMROOT, 2022.03.26: 
 * 페이지를 lru 리스트에서 다른 리스트로 이동하기 위해 lru 플래그를 클리어한다.
 * lru 리스트 관리를 받지 않는다는 의미이다.
 * LRU 플래그를 제거하는 시점에서 이미 다른 cpu에서 LRU 플래그를 제거한 경우
 * isolate_fail_put으로 이동한다.
 */
		/* Try isolate the page */
		if (!TestClearPageLRU(page))
			goto isolate_fail_put;

/*
 * IAMROOT, 2022.03.31:
 * ----------- 이시점에 page의 LRU flag는 제거됬다. -----------
 */
		lruvec = mem_cgroup_page_lruvec(page);

		/* If we already hold the lock, we can skip some rechecking */
/*
 * IAMROOT, 2022.03.26: 
 * lock건 lruvec의 위치가 동일하지 않은 경우 다시 lock을 획득하고 획득한 memcg
 * 위치를 locked에 갱신한다.
 */
		if (lruvec != locked) {
			if (locked)
				unlock_page_lruvec_irqrestore(locked, flags);

			compact_lock_irqsave(&lruvec->lru_lock, &flags, cc);
			locked = lruvec;

			lruvec_memcg_debug(lruvec, page);

			/* Try get exclusive access under lock */
/*
 * IAMROOT, 2022.03.26: 
 * lock을 걸고 확인해본다. 누군가 먼저 해당 진행하는 블럭에 skip 비트를 
 * 먼저 설정해두었는지.. 만일 누군가 먼저 설정한 경우 현재 루틴은 
 * 진행을 포기하기 위해 isolate_abort로 이동한다.
 *
 * - 위쪽에 if문 valid_page갱신때와 같은 상황으로, 해당 page를 시작지점으로
 *   이미 isolate시도를 근시일내에 해봤는데 isolate가 한개도 없었거나
 *   충분히 해봤으면 abort한다.
 */
			if (!skip_updated) {
				skip_updated = true;
				if (test_and_set_skip(cc, page, low_pfn))
					goto isolate_abort;
			}

			/*
			 * Page become compound since the non-locked check,
			 * and it's on LRU. It can only be a THP so the order
			 * is safe to read and it's 0 for tail pages.
			 */
/*
 * IAMROOT, 2022.03.26: 
 * alloc_contig_range()로 호출하지 않은 일반적인 할당 시 compound 페이지는
 * lru로 되돌리기 위해 lru 플래그를 설정하고 isolate_fail_put으로 이동한다.
 *
 * -  현재 if문을 진입하기 전에 이미 위에서 non-lock 상태로 같은조건으
 *    검사를 했지만 lock을 새로 얻은후에 다시 확인 한다.
 */
			if (unlikely(PageCompound(page) && !cc->alloc_contig)) {
				low_pfn += compound_nr(page) - 1;
				SetPageLRU(page);
				goto isolate_fail_put;
			}
		}

		/* The whole page is taken off the LRU; skip the tail pages. */
		if (PageCompound(page))
			low_pfn += compound_nr(page) - 1;

		/* Successfully isolated */
/*
 * IAMROOT, 2022.03.26: 
 * lruvec 리스트에서 현재 페이지를 분리한다.
 */
		del_page_from_lru_list(page, lruvec);
		mod_node_page_state(page_pgdat(page),
				NR_ISOLATED_ANON + page_is_file_lru(page),
				thp_nr_pages(page));

isolate_success:
/*
 * IAMROOT, 2022.03.26: 
 * cc->migratepages 리스트에 분리한 페이지를 추가한다.
 */
		list_add(&page->lru, &cc->migratepages);
isolate_success_no_list:
		cc->nr_migratepages += compound_nr(page);
		nr_isolated += compound_nr(page);

		/*
		 * Avoid isolating too much unless this block is being
		 * rescanned (e.g. dirty/writeback pages, parallel allocation)
		 * or a lock is contended. For contention, isolate quickly to
		 * potentially remove one source of contention.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * 1 페이지 블럭 전체를 isloation하지 않았어도 32개 이상의 페이지를 isolation
 * 한 경우 rescan 요구와 contended가 없는 상황이면 중단한다.
 */
		if (cc->nr_migratepages >= COMPACT_CLUSTER_MAX &&
		    !cc->rescan && !cc->contended) {
			++low_pfn;
			break;
		}

		continue;

isolate_fail_put:
		/* Avoid potential deadlock in freeing page under lru_lock */
		if (locked) {
			unlock_page_lruvec_irqrestore(locked, flags);
			locked = NULL;
		}
		put_page(page);

/*
 * IAMROOT, 2022.03.26: 
 * isolate_fail: 레이블에서는 isolate가 실패했을 때 이동되어지는 곳이다.
 * 에러가 없으면 당연히 다음 page를 계속하고, 만일 에러 발생시 skip 할지
 * 여부에 따라 종료될 수 있다.
 */

isolate_fail:
/*
 * IAMROOT, 2022.03.31:
 * - sync or indirect compaction일때만 ret을 검사해서 continue 시킨다.
 *   즉 실패를 해도 error 상태가 아니면 요청된 end_pfn까지 전부 돌아보거나
 *   위에 isolate sucess에서 COMPACT_CLUSTER_MAX조건이 있는 if문에 빠져가기전까지
 *   isolate 시킬것이다.
 *
 * - ret이 -ENOMEM(memory 관련 erorr)이 되는경우는
 *   isolate_or_dissolve_huge_page 때만 존재한다.
 *   즉 sync or indirect compaction일때 huge만 안건드리면
 *   (건드려도 memory 실패만 안나면) 범위 전체를 탐색할것이다.
 */
		if (!skip_on_failure && ret != -ENOMEM)
			continue;

		/*
		 * We have isolated some pages, but then failed. Release them
		 * instead of migrating, as we cannot form the cc->order buddy
		 * page anyway.
		 */
/*
 * IAMROOT, 2022.03.31:
 * - ENOMEM이 발생했거나 async direct compaction일 경우에 여기에 도달한다.
 * - isolate하고 있는 중간에 실패했다면 isolate 햇던것을 원복 시킨다.
 */
		if (nr_isolated) {
			if (locked) {
				unlock_page_lruvec_irqrestore(locked, flags);
				locked = NULL;
			}
			putback_movable_pages(&cc->migratepages);
			cc->nr_migratepages = 0;
			nr_isolated = 0;
		}

/*
 * IAMROOT, 2022.03.31:
 * - async direct compaction 일때만 next_skip_pfn을 사용하는데 이미 여기 온시점이
 *   이런 상황이(-ENOMEM일 수도 있지만)므로 next_skip_pfn을 갱신해준다.
 *   -ENOMEM인 경우 아래에서 바로 처리해준다.
 */
		if (low_pfn < next_skip_pfn) {
			low_pfn = next_skip_pfn - 1;
			/*
			 * The check near the loop beginning would have updated
			 * next_skip_pfn too, but this is a bit simpler.
			 */
			next_skip_pfn += 1UL << cc->order;
		}

		if (ret == -ENOMEM)
			break;
/*
 * IAMROOT, 2022.03.31:
 * - for 문이 끝나는 시점으로 갱신된 low_pfn으로 다음 iterate를 수행한다.
 *   compound page인 경우 이미 해당 범위의 보정이 끝난 low_pfn일것이다.
 */
	}

/*
 * IAMROOT, 2022.03.26: 
 * 1페이지 블럭안에서 루프를 다 돌고 종료하였다.
 */
	/*
	 * The PageBuddy() check could have potentially brought us outside
	 * the range to be scanned.
	 */
	if (unlikely(low_pfn > end_pfn))
		low_pfn = end_pfn;

	page = NULL;

isolate_abort:
	if (locked)
		unlock_page_lruvec_irqrestore(locked, flags);
/*
 * IAMROOT, 2022.03.31:
 * - abort가 발생한 @page는 LRU flag가 제거되었을 수 있으므로 다시 설정해준다.
 */
	if (page) {
		SetPageLRU(page);
		put_page(page);
	}

	/*
	 * Updated the cached scanner pfn once the pageblock has been scanned
	 * Pages will either be migrated in which case there is no point
	 * scanning in the near future or migration failed in which case the
	 * failure reason may persist. The block is marked for skipping if
	 * there were no pages isolated in the block or if the block is
	 * rescanned twice in a row.
	 */
/*
 * IAMROOT, 2022.03.26: 
 * 마지막 종료하였고, isolate 페이지가 없거나 rescan 요구가 있었으면
 * 다음 블럭위치부터 시작하도록 캐시에 기록한다.
 * 또한 valid 페이지에 대한 skip비트가 설정되지 않은 경우 valid 페이지가 
 * 속한 블럭의 skip 비트를 설정한다.
 */
	if (low_pfn == end_pfn && (!nr_isolated || cc->rescan)) {
/*
 * IAMROOT, 2022.03.31:
 * - valid_page는 pageblock단위로 생길것이다.
 *   누군가 이미 isolate를 한 상황이라 abort로 빠져나온 상황 (!vaild_page)
 *   이 없고 한번이라도 lock을 걸어본 (skip_updated) 적이 없다면 skip을 표시한다.
 */
		if (valid_page && !skip_updated)
			set_pageblock_skip(valid_page);
		update_cached_migrate(cc, low_pfn);
	}

	trace_mm_compaction_isolate_migratepages(start_pfn, low_pfn,
						nr_scanned, nr_isolated);

fatal_pending:
	cc->total_migrate_scanned += nr_scanned;
	if (nr_isolated)
		count_compact_events(COMPACTISOLATED, nr_isolated);

	cc->migrate_pfn = low_pfn;

	return ret;
}

/**
 * isolate_migratepages_range() - isolate migrate-able pages in a PFN range
 * @cc:        Compaction control structure.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Returns -EAGAIN when contented, -EINTR in case of a signal pending, -ENOMEM
 * in case we could not allocate a page, or 0.
 */
int
isolate_migratepages_range(struct compact_control *cc, unsigned long start_pfn,
							unsigned long end_pfn)
{
	unsigned long pfn, block_start_pfn, block_end_pfn;
	int ret = 0;

	/* Scan block by block. First and last block may be incomplete */
	pfn = start_pfn;
	block_start_pfn = pageblock_start_pfn(pfn);
	if (block_start_pfn < cc->zone->zone_start_pfn)
		block_start_pfn = cc->zone->zone_start_pfn;
	block_end_pfn = pageblock_end_pfn(pfn);

	for (; pfn < end_pfn; pfn = block_end_pfn,
				block_start_pfn = block_end_pfn,
				block_end_pfn += pageblock_nr_pages) {

		block_end_pfn = min(block_end_pfn, end_pfn);

		if (!pageblock_pfn_to_page(block_start_pfn,
					block_end_pfn, cc->zone))
			continue;

		ret = isolate_migratepages_block(cc, pfn, block_end_pfn,
						 ISOLATE_UNEVICTABLE);

		if (ret)
			break;

		if (cc->nr_migratepages >= COMPACT_CLUSTER_MAX)
			break;
	}

	return ret;
}

#endif /* CONFIG_COMPACTION || CONFIG_CMA */
#ifdef CONFIG_COMPACTION

/*
 * IAMROOT, 2022.03.29:
 * - migrate에 적절하다(true)
 *   persistent block이 아니다.
 *   -> 연속되지 않았으니 migrate할만하다.
 *   mode가 sync이다.
 *   -> 즉 background 동작이 아니다.
 *   direct_compaction이 아니다.
 *   -> 즉 user 요청이거나 backgroup다. 무조건 해야된다.
 *   요청된 mt와 @page의 pageblock mt가 같다(movable일경우 cma포함)
 *
 * - migarte에 부적절하다.(false)
 *   persistent block이다.
 *   -> 연속되있는걸 굳이 옮기진 않는다는것.
 *   async direct_compaction이 true이며, 요청 mt와 pageblock의 mt가 다르다
 *   -> asnyc direct_compaction을 asnyc direct_compaction이라고 묶어 부르는듯
 *   싶다. asnc에 대해서는 movable 페이지만으로 제한한다. movable page가
 *   unmovable page를 포함할 가능성이 낮다는 가정하에 지연 시간을 줄이기
 *   위한 heuristic이라고 한다.(Git blame참고)
 */
static bool suitable_migration_source(struct compact_control *cc,
							struct page *page)
{
	int block_mt;

/*
 * IAMROOT, 2022.03.26: 
 * pageblock_order 이상의 compound 페이지(huge page)등은 항상 false를 반환한다.
 */
	if (pageblock_skip_persistent(page))
		return false;

/*
 * IAMROOT, 2022.03.26: 
 * async가 아닌 경우와 direct-compaction이 아닌 경우(manual-compaction, i
 * kcompactd를 사용한 백그라운드 compaction)인 경우 무조건 true를 반환
 */
	if ((cc->mode != MIGRATE_ASYNC) || !cc->direct_compaction)
		return true;

/*
 * IAMROOT, 2022.03.26: 
 * sync이면서 dirct-compaction의 경우에는 해당 페이지 블럭이 
 * 요청한 migratetype인 경우에만 true를 반환한다. 
 * (단 movable 할당 요청인 경우 cma 블럭도 포함)
 */
	block_mt = get_pageblock_migratetype(page);

	if (cc->migratetype == MIGRATE_MOVABLE)
		return is_migrate_movable(block_mt);
	else
		return block_mt == cc->migratetype;
}

/* Returns true if the page is within a block suitable for migration to */
static bool suitable_migration_target(struct compact_control *cc,
							struct page *page)
{
	/* If the page is a large free page, then disallow migration */
	if (PageBuddy(page)) {
		/*
		 * We are checking page_order without zone->lock taken. But
		 * the only small danger is that we skip a potentially suitable
		 * pageblock, so it's not worth to check order for valid range.
		 */
		if (buddy_order_unsafe(page) >= pageblock_order)
			return false;
	}

	if (cc->ignore_block_suitable)
		return true;

	/* If the block is MIGRATE_MOVABLE or MIGRATE_CMA, allow migration */
	if (is_migrate_movable(get_pageblock_migratetype(page)))
		return true;

	/* Otherwise skip the block */
	return false;
}

/*
 * IAMROOT, 2022.03.28:
 *
 * fast_search_fail return
 * 0                33
 * 1                17
 * 2                9
 * 3                5
 * 4                3
 * 5                2          
 * 6                1
 *
 * - 이전 fast search 실패 횟수가 많을 수록 freelist iterate 횟수를
 *   약 절반씩 줄인다.
 */
static inline unsigned int
freelist_scan_limit(struct compact_control *cc)
{
	unsigned short shift = BITS_PER_LONG - 1;

	return (COMPACT_CLUSTER_MAX >> min(shift, cc->fast_search_fail)) + 1;
}

/*
 * Test whether the free scanner has reached the same or lower pageblock than
 * the migration scanner, and compaction should thus terminate.
 */
/*
 * IAMROOT, 2022.03.26: 
 * migrate 스캐너와 free 스캐너가 만나 교차하는 순간인지,
 * 즉 스캐닝 종료인지 여부를 반환한다.
 */
static inline bool compact_scanners_met(struct compact_control *cc)
{
	return (cc->free_pfn >> pageblock_order)
		<= (cc->migrate_pfn >> pageblock_order);
}

/*
 * Used when scanning for a suitable migration target which scans freelists
 * in reverse. Reorders the list such as the unscanned pages are scanned
 * first on the next iteration of the free scanner
 */
/*
 * IAMROOT, 2022.04.02:
 * - @freepage가 last page가 아니면 free_page 의 뒷 entry들을 앞으로 옮긴다.
 *
 * - ex) freepage == n4
 *  freelist : n1 n2 n3 n4 n5 n6
 *                      ^freepage
 *  < list_cut_before 수행 >
 *
 *  head : n4 n5 n6
 *  list : root n1 n2 n3 
 *
 * < list_splice_tail 수행 >
 * head : n4 n5 n6 n1 n2 n3
 */
static void
move_freelist_head(struct list_head *freelist, struct page *freepage)
{
	LIST_HEAD(sublist);

	if (!list_is_last(freelist, &freepage->lru)) {
		list_cut_before(&sublist, freelist, &freepage->lru);
		list_splice_tail(&sublist, freelist);
	}
}

/*
 * Similar to move_freelist_head except used by the migration scanner
 * when scanning forward. It's possible for these list operations to
 * move against each other if they search the free list exactly in
 * lockstep.
 */
static void
move_freelist_tail(struct list_head *freelist, struct page *freepage)
{
	LIST_HEAD(sublist);

	if (!list_is_first(freelist, &freepage->lru)) {
		list_cut_position(&sublist, freelist, &freepage->lru);
		list_splice_tail(&sublist, freelist);
	}
}

static void
fast_isolate_around(struct compact_control *cc, unsigned long pfn, unsigned long nr_isolated)
{
	unsigned long start_pfn, end_pfn;
	struct page *page;

	/* Do not search around if there are enough pages already */
	if (cc->nr_freepages >= cc->nr_migratepages)
		return;

	/* Minimise scanning during async compaction */
	if (cc->direct_compaction && cc->mode == MIGRATE_ASYNC)
		return;

	/* Pageblock boundaries */
	start_pfn = max(pageblock_start_pfn(pfn), cc->zone->zone_start_pfn);
	end_pfn = min(pageblock_end_pfn(pfn), zone_end_pfn(cc->zone));

	page = pageblock_pfn_to_page(start_pfn, end_pfn, cc->zone);
	if (!page)
		return;

	/* Scan before */
	if (start_pfn != pfn) {
		isolate_freepages_block(cc, &start_pfn, pfn, &cc->freepages, 1, false);
		if (cc->nr_freepages >= cc->nr_migratepages)
			return;
	}

	/* Scan after */
	start_pfn = pfn + nr_isolated;
	if (start_pfn < end_pfn)
		isolate_freepages_block(cc, &start_pfn, end_pfn, &cc->freepages, 1, false);

	/* Skip this pageblock in the future as it's full or nearly full */
	if (cc->nr_freepages < cc->nr_migratepages)
		set_pageblock_skip(page);
}

/* Search orders in round-robin fashion */
/*
 * IAMROOT, 2022.04.02:
 * - @order를 1씩 내린다. 만약 0보다 작아지면 cc->order - 1로 갱신한다.
 *   @order가 cc->search_order와 같아질경우(한바퀴 동안 전부 실패했다는 듯)
 *   search_order를 -1로 갱신하고 return -1로 수행한다.
 */
static int next_search_order(struct compact_control *cc, int order)
{
	order--;
	if (order < 0)
		order = cc->order - 1;

	/* Search wrapped around? */
	if (order == cc->search_order) {
		cc->search_order--;
		if (cc->search_order < 0)
			cc->search_order = cc->order - 1;
		return -1;
	}

	return order;
}

/*
 * IAMROOT, 2022.04.02:
 * @return free scan start pfn
 */
static unsigned long
fast_isolate_freepages(struct compact_control *cc)
{
	unsigned int limit = max(1U, freelist_scan_limit(cc) >> 1);
	unsigned int nr_scanned = 0;
	unsigned long low_pfn, min_pfn, highest = 0;
	unsigned long nr_isolated = 0;
	unsigned long distance;
	struct page *page = NULL;
	bool scan_start = false;
	int order;

	/* Full compaction passes in a negative order */
/*
 * IAMROOT, 2022.04.02:
 * - order == -1(지정되지 않음(user 요청등)) or 0(single page 요청)일때 조정
 *   없이 그냥 return.
 */
	if (cc->order <= 0)
		return cc->free_pfn;

	/*
	 * If starting the scan, use a deeper search and use the highest
	 * PFN found if a suitable one is not found.
	 */
/*
 * IAMROOT, 2022.04.02:
 * - free_pfn은 high => low address로 진행한다. if문 뜻은 free_pfn이 설정된
 *   범위까지 끝낫으므로 다시 limit을 정해 scan을 수행해야된다는것.
 * - 최초의 시작이라면 cc->free_pfn은 0xffff.. 일것이다.
 */
	if (cc->free_pfn >= cc->zone->compact_init_free_pfn) {
		limit = pageblock_nr_pages >> 1;
		scan_start = true;
	}

	/*
	 * Preferred point is in the top quarter of the scan space but take
	 * a pfn from the top half if the search is problematic.
	 */
	distance = (cc->free_pfn - cc->migrate_pfn);
/*
 * IAMROOT, 2022.04.02:
 * - low_pfn : 25%, min_pfn : 50%
 * -
 *  ----- cc->free_pfn
 *
 *  ----- low_pfn
 *
 *  ----- min_pfn 
 *
 *
 *
 *  ---- cc->migrate_pfn
 */
	low_pfn = pageblock_start_pfn(cc->free_pfn - (distance >> 2));
	min_pfn = pageblock_start_pfn(cc->free_pfn - (distance >> 1));

	if (WARN_ON_ONCE(min_pfn > low_pfn))
		low_pfn = min_pfn;

	/*
	 * Search starts from the last successful isolation order or the next
	 * order to search after a previous failure
	 */
/*
 * IAMROOT, 2022.04.02:
 * - 마지막 성공 order부터 시작을 하거나 실패했던 order -1(search_order) 부터 시작한다.
 *   (next_search_order 참고)
 */
	cc->search_order = min_t(unsigned int, cc->order - 1, cc->search_order);

	for (order = cc->search_order;
	     !page && order >= 0;
	     order = next_search_order(cc, order)) {
		struct free_area *area = &cc->zone->free_area[order];
		struct list_head *freelist;
		struct page *freepage;
		unsigned long flags;
		unsigned int order_scanned = 0;
		unsigned long high_pfn = 0;

		if (!area->nr_free)
			continue;

		spin_lock_irqsave(&cc->zone->lock, flags);
/*
 * IAMROOT, 2022.04.02:
 * - movable 만 조회한다. migrate는 movable page로만 할수있다.
 */
		freelist = &area->free_list[MIGRATE_MOVABLE];
/*
 * IAMROOT, 2022.04.02:
 * - buddy에 오래있던 page순으로 (cold순)으로 가져온다.
 */
		list_for_each_entry_reverse(freepage, freelist, lru) {
			unsigned long pfn;

			order_scanned++;
			nr_scanned++;
			pfn = page_to_pfn(freepage);

/*
 * IAMROOT, 2022.04.02:
 * - highest를 갱신한다. 극단적인 경우 zone범위 내료 clamp를 한다.
 */
			if (pfn >= highest)
				highest = max(pageblock_start_pfn(pfn),
					      cc->zone->zone_start_pfn);

/*
 * IAMROOT, 2022.04.02:
 * - 높은 주소의 page를 찾을수록 좋은 page로 생각한다.
 * - low_pfn보다 높게 찾아진것은 성공으로 판단하여 fail count를 0으로 초기화,
 *   search_order를 성공한 order로 갱신한다.
 *   차선으로 min_pfn ~ low_pfn 사이면서 이전에 차선으로 찾은 high_pfn 보다 높은
 *   page일 경우 high_pfn에 차선 page로 갱신하고 scan 최대 횟수를 반절씩 줄인다.
 */
			if (pfn >= low_pfn) {
				cc->fast_search_fail = 0;
				cc->search_order = order;
				page = freepage;
				break;
			}

			if (pfn >= min_pfn && pfn > high_pfn) {
				high_pfn = pfn;

				/* Shorten the scan if a candidate is found */
				limit >>= 1;
			}

			if (order_scanned >= limit)
				break;
		}

		/* Use a minimum pfn if a preferred one was not found */
/*
 * IAMROOT, 2022.04.02:
 * - low_pfn보다 높은 주소(좋은 case)의 page를 못찾은 경우, 차선으로 택한 page를
 *   사용한다.
 */
		if (!page && high_pfn) {
			page = pfn_to_page(high_pfn);

			/* Update freepage for the list reorder below */
			freepage = page;
		}

		/* Reorder to so a future search skips recent pages */
/*
 * IAMROOT, 2022.04.02:
 * - scan이 끝난 지점(freepage) page들을 head로 옮긴다.
 * - ex) node4가 freepage
 *  before : node1 node2 node3 node4 node5 node6
 *                                ^------------ scan
 *  after  : [node4 node5 node6] node1 node2 node3
 *            옮겨진 page들                    ^--- 다음 scan은 여기부터 시작
 */
		move_freelist_head(freelist, freepage);

		/* Isolate the page if available */
		if (page) {
			if (__isolate_free_page(page, order)) {
				set_page_private(page, order);
				nr_isolated = 1 << order;
				cc->nr_freepages += nr_isolated;
				list_add_tail(&page->lru, &cc->freepages);
				count_compact_events(COMPACTISOLATED, nr_isolated);
			} else {
				/* If isolation fails, abort the search */
				order = cc->search_order + 1;
				page = NULL;
			}
		}

		spin_unlock_irqrestore(&cc->zone->lock, flags);

		/*
		 * Smaller scan on next order so the total scan is related
		 * to freelist_scan_limit.
		 */
		if (order_scanned >= limit)
			limit = max(1U, limit >> 1);
	}

	if (!page) {
		cc->fast_search_fail++;
		if (scan_start) {
			/*
			 * Use the highest PFN found above min. If one was
			 * not found, be pessimistic for direct compaction
			 * and use the min mark.
			 */
			if (highest) {
				page = pfn_to_page(highest);
				cc->free_pfn = highest;
			} else {
				if (cc->direct_compaction && pfn_valid(min_pfn)) {
					page = pageblock_pfn_to_page(min_pfn,
						min(pageblock_end_pfn(min_pfn),
						    zone_end_pfn(cc->zone)),
						cc->zone);
					cc->free_pfn = min_pfn;
				}
			}
		}
	}

	if (highest && highest >= cc->zone->compact_cached_free_pfn) {
		highest -= pageblock_nr_pages;
		cc->zone->compact_cached_free_pfn = highest;
	}

	cc->total_free_scanned += nr_scanned;
	if (!page)
		return cc->free_pfn;

	low_pfn = page_to_pfn(page);
	fast_isolate_around(cc, low_pfn, nr_isolated);
	return low_pfn;
}

/*
 * Based on information in the current compact_control, find blocks
 * suitable for isolating free pages from and then isolate them.
 */
static void isolate_freepages(struct compact_control *cc)
{
	struct zone *zone = cc->zone;
	struct page *page;
	unsigned long block_start_pfn;	/* start of current pageblock */
	unsigned long isolate_start_pfn; /* exact pfn we start at */
	unsigned long block_end_pfn;	/* end of current pageblock */
	unsigned long low_pfn;	     /* lowest pfn scanner is able to scan */
	struct list_head *freelist = &cc->freepages;
	unsigned int stride;

	/* Try a small search of the free lists for a candidate */
	isolate_start_pfn = fast_isolate_freepages(cc);
	if (cc->nr_freepages)
		goto splitmap;

	/*
	 * Initialise the free scanner. The starting point is where we last
	 * successfully isolated from, zone-cached value, or the end of the
	 * zone when isolating for the first time. For looping we also need
	 * this pfn aligned down to the pageblock boundary, because we do
	 * block_start_pfn -= pageblock_nr_pages in the for loop.
	 * For ending point, take care when isolating in last pageblock of a
	 * zone which ends in the middle of a pageblock.
	 * The low boundary is the end of the pageblock the migration scanner
	 * is using.
	 */
	isolate_start_pfn = cc->free_pfn;
	block_start_pfn = pageblock_start_pfn(isolate_start_pfn);
	block_end_pfn = min(block_start_pfn + pageblock_nr_pages,
						zone_end_pfn(zone));
	low_pfn = pageblock_end_pfn(cc->migrate_pfn);
	stride = cc->mode == MIGRATE_ASYNC ? COMPACT_CLUSTER_MAX : 1;

	/*
	 * Isolate free pages until enough are available to migrate the
	 * pages on cc->migratepages. We stop searching if the migrate
	 * and free page scanners meet or enough free pages are isolated.
	 */
	for (; block_start_pfn >= low_pfn;
				block_end_pfn = block_start_pfn,
				block_start_pfn -= pageblock_nr_pages,
				isolate_start_pfn = block_start_pfn) {
		unsigned long nr_isolated;

		/*
		 * This can iterate a massively long zone without finding any
		 * suitable migration targets, so periodically check resched.
		 */
		if (!(block_start_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages)))
			cond_resched();

		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone);
		if (!page)
			continue;

		/* Check the block is suitable for migration */
		if (!suitable_migration_target(cc, page))
			continue;

		/* If isolation recently failed, do not retry */
		if (!isolation_suitable(cc, page))
			continue;

		/* Found a block suitable for isolating free pages from. */
		nr_isolated = isolate_freepages_block(cc, &isolate_start_pfn,
					block_end_pfn, freelist, stride, false);

		/* Update the skip hint if the full pageblock was scanned */
		if (isolate_start_pfn == block_end_pfn)
			update_pageblock_skip(cc, page, block_start_pfn);

		/* Are enough freepages isolated? */
		if (cc->nr_freepages >= cc->nr_migratepages) {
			if (isolate_start_pfn >= block_end_pfn) {
				/*
				 * Restart at previous pageblock if more
				 * freepages can be isolated next time.
				 */
				isolate_start_pfn =
					block_start_pfn - pageblock_nr_pages;
			}
			break;
		} else if (isolate_start_pfn < block_end_pfn) {
			/*
			 * If isolation failed early, do not continue
			 * needlessly.
			 */
			break;
		}

		/* Adjust stride depending on isolation */
		if (nr_isolated) {
			stride = 1;
			continue;
		}
		stride = min_t(unsigned int, COMPACT_CLUSTER_MAX, stride << 1);
	}

	/*
	 * Record where the free scanner will restart next time. Either we
	 * broke from the loop and set isolate_start_pfn based on the last
	 * call to isolate_freepages_block(), or we met the migration scanner
	 * and the loop terminated due to isolate_start_pfn < low_pfn
	 */
	cc->free_pfn = isolate_start_pfn;

splitmap:
	/* __isolate_free_page() does not map the pages */
	split_map_pages(freelist);
}

/*
 * This is a migrate-callback that "allocates" freepages by taking pages
 * from the isolated freelists in the block we are migrating to.
 */
/*
 * IAMROOT, 2022.04.02:
 * - migrate시 이동이 될 대상인 free page를 가져온다(unmap_and_move 참고)
 */
static struct page *compaction_alloc(struct page *migratepage,
					unsigned long data)
{
	struct compact_control *cc = (struct compact_control *)data;
	struct page *freepage;

/*
 * IAMROOT, 2022.04.02:
 * - 처음진입했거나 isolate한 page가 하나도 없으면 isolate하기 위해서
 *   free scanning을 한다.
 */
	if (list_empty(&cc->freepages)) {
		isolate_freepages(cc);

		if (list_empty(&cc->freepages))
			return NULL;
	}

/*
 * IAMROOT, 2022.04.02:
 * - freepages에서 한개를 빼온다.
 */
	freepage = list_entry(cc->freepages.next, struct page, lru);
	list_del(&freepage->lru);
	cc->nr_freepages--;

	return freepage;
}

/*
 * This is a migrate-callback that "frees" freepages back to the isolated
 * freelist.  All pages on the freelist are from the same zone, so there is no
 * special handling needed for NUMA.
 */
/*
 * IAMROOT, 2022.04.09:
 * - migrate 실패했을시 @page를 freepages에 되돌린다.
 */
static void compaction_free(struct page *page, unsigned long data)
{
	struct compact_control *cc = (struct compact_control *)data;

	list_add(&page->lru, &cc->freepages);
	cc->nr_freepages++;
}

/* possible outcome of isolate_migratepages */
typedef enum {
	ISOLATE_ABORT,		/* Abort compaction now */
	ISOLATE_NONE,		/* No pages isolated, continue scanning */
	ISOLATE_SUCCESS,	/* Pages isolated, migrate */
} isolate_migrate_t;

/*
 * Allow userspace to control policy on scanning the unevictable LRU for
 * compactable pages.
 */
#ifdef CONFIG_PREEMPT_RT
int sysctl_compact_unevictable_allowed __read_mostly = 0;
#else
int sysctl_compact_unevictable_allowed __read_mostly = 1;
#endif

/*
 * IAMROOT, 2022.03.29:
 * - @cc->fast_start_pfn이 ULONG_MAX값이 아닐경우 @pfn과 min 비교하여
 *   update한다.
 * - compact_zone함수 등에서 @cc->fast_start_pfn이 0으로 초기화되어
 *   update가능한 상태가 된다.
 * - reinit_migrate_pfn함수 등에서 @cc->fast_start_pfn이 ULONG_MAX로
 *   설정되어 update 불가능한 상태가 된다.
 */
static inline void
update_fast_start_pfn(struct compact_control *cc, unsigned long pfn)
{
	if (cc->fast_start_pfn == ULONG_MAX)
		return;

	if (!cc->fast_start_pfn)
		cc->fast_start_pfn = pfn;

	cc->fast_start_pfn = min(cc->fast_start_pfn, pfn);
}

/*
 * IAMROOT, 2022.03.29:
 * - @cc가 fast_start_pfn이 설정된 상태에서만 @cc->migrate_pfn을
 *   fast_start_pfn으로 update하고 fast_start_pfn은 ULONG_MAX로 만들어
 *   update_fast_start_pfn에서 update되는걸 막는다.
 */
static inline unsigned long
reinit_migrate_pfn(struct compact_control *cc)
{
	if (!cc->fast_start_pfn || cc->fast_start_pfn == ULONG_MAX)
		return cc->migrate_pfn;

	cc->migrate_pfn = cc->fast_start_pfn;
	cc->fast_start_pfn = ULONG_MAX;

	return cc->migrate_pfn;
}

/*
 * Briefly search the free lists for a migration source that already has
 * some free pages to reduce the number of pages that need migration
 * before a pageblock is free.
 */
/*
 * IAMROOT, 2022.03.28:
 * - movable freelist에서 compact범위의 12.5 ~ 50% 범위에 대한 freepages를 찾아
 *   skip bit를 set하고 movable freelist의 tail로 옮겨 놓는다.
 *   @cc->fast_start_pfn이 찾아진 freepages와 min값으로 비교되 업데이트되며
 *   만약 실패할시에는 cc->migrate_pfn이 cc->fast_start_pfn으로 update되고,
 *   cc->fast_start_pfn은 ULONG_MAX으로 되어 향후 update가 안된다.
 *
 * --- ex) cc->migrate_pfn = 100, cc->migrate_pfn = 200, 50%의 범위
 *  cc->fast_search_fail = 0 일때, freepage가 찾아진 경우
 *
 * order의 movable freelist에 들어있는 freepage
 *
 * 50%의 범위이므로 100 ~ 150까지만 유효하다.
 * before
 * +-----+-----+
 * | idx | pfn |
 * +-----+-----+
 * | 0   | 210 |
 * | 1   | 180 | 
 * | 2   | 90  |
 * | 3   | 130 | (100 ~ 150의 범위. tail로 이동)
 * | 4   | 110 |
 * | 5   | 120 |
 * +-----+-----+
 *
 * after
 * +-----+-----+
 * | idx | pfn |
 * +-----+-----+
 * | 0   | 210 |
 * | 1   | 180 |
 * | 2   | 90  |
 * | 3   | 110 |
 * | 4   | 120 |
 * | 5   | 130 | (tail로 이동되며 skip이 set되고
 * +-----+-----+ cc->fast_start_pfn = 130으로 설정된다.)
 *
 * ---
 * - 관련변수
 *   -- @cc->fast_start_pfn
 *   0 : 이 함수를 최초에 진입하기전에 0으로 초기화가 되어 진입하게 된다.
 *   fast_start_pfn이 update가 가능한 상태.
 *   ULONG_MAX : fast search가 실패했을때 설정되며 마지막 fast_start_pfn이
 *   cc->migrate_pfn으로 reinit된다. 이렇게 한번이라도 실패를 하고 나서는
 *   만약 page를찾는데 성공해도 fast_start_pfn이 변경되진 않고
 *   tail로 이동만 할것이다.
 *   -- @cc->fast_search_fail
 *   fast search가 된다면 증가되고 향후에 freelist iter횟수 제한에 사용된다.
 *   만약 성공한다면 0으로 초기화된다. 
 *   -- @cc->migrate_pfn
 *   fase search가 처음으로 실패했을시 가장 최근의 fast_start_pfn으로
 *   대체 된다.
 */
static unsigned long fast_find_migrateblock(struct compact_control *cc)
{
	unsigned int limit = freelist_scan_limit(cc);
	unsigned int nr_scanned = 0;
	unsigned long distance;
	unsigned long pfn = cc->migrate_pfn;
	unsigned long high_pfn;
	int order;
	bool found_block = false;

	/* Skip hints are relied on to avoid repeats on the fast search */
/*
 * IAMROOT, 2022.03.28:
 * - skip을 안하겠다는 의미이므로 첫번째 pfn으로 바로 넘긴다.
 */
	if (cc->ignore_skip_hint)
		return pfn;

	/*
	 * If the migrate_pfn is not at the start of a zone or the start
	 * of a pageblock then assume this is a continuation of a previous
	 * scan restarted due to COMPACT_CLUSTER_MAX.
	 */
/*
 * IAMROOT, 2022.03.28:
 * - 현재 zone의 start와 일치하지 않고, pageblock의 start pfn도 아니라면
 *   새로 재계된 scan이라고 생각하여 skip을 안한다.
 *   (reinit_migrate_pfn을 통해 migrate_pfn이 reinit 된경우등) 
 */
	if (pfn != cc->zone->zone_start_pfn && pfn != pageblock_start_pfn(pfn))
		return pfn;

	/*
	 * For smaller orders, just linearly scan as the number of pages
	 * to migrate should be relatively small and does not necessarily
	 * justify freeing up a large block for a small allocation.
	 */
	if (cc->order <= PAGE_ALLOC_COSTLY_ORDER)
		return pfn;

	/*
	 * Only allow kcompactd and direct requests for movable pages to
	 * quickly clear out a MOVABLE pageblock for allocation. This
	 * reduces the risk that a large movable pageblock is freed for
	 * an unmovable/reclaimable small allocation.
	 */
/*
 * IAMROOT, 2022.03.28:
 * - direct compaction가 true(kcompactd or /proc/... 으로 아닌 경우등)
 *   이면서 UNMOVABLE이나 RECLAIMABLE일 경우 skip을 안한다.
 *
 * ---
 * - direct compaction == false -> komcpacd or /proc/... 의 요청.
 *   무조건 skip 여부를 확인
 * - direct compaction == true -> komcpacd or /proc/... 의 요청이 아닐때,
 *   MOVABLE 에 대해서만 skip 여부를 확인한다.
 *   skip을 하게되면 큰 block을 사용할수 있는데,
 *   UNMOVABLE, RECLAIMABLE에 대해서는 꼼꼼하게 page를 다쓰는게 좋기때문이다.
 */
	if (cc->direct_compaction && cc->migratetype != MIGRATE_MOVABLE)
		return pfn;

	/*
	 * When starting the migration scanner, pick any pageblock within the
	 * first half of the search space. Otherwise try and pick a pageblock
	 * within the first eighth to reduce the chances that a migration
	 * target later becomes a source.
	 */
/*
 * IAMROOT, 2022.03.28:
 * - 시작 위치가 zone start와 일치할 경우 시작부 50%만 확인.
 *   그렇지 않을 경우 12.5%( 1 / 2 / 4 => 1 / 8)만 확인.
 *   (reinit_migrate_pfn을 통해 migrate_pfn이 reinit 된경우등) 
 */
	distance = (cc->free_pfn - cc->migrate_pfn) >> 1;
	if (cc->migrate_pfn != cc->zone->zone_start_pfn)
		distance >>= 2;
/*
 * IAMROOT, 2022.03.28:
 * - 좁혀진 범위에서는 end pfn을 high_pfn으로 설정.
 */
	high_pfn = pageblock_start_pfn(cc->migrate_pfn + distance);
/*
 * IAMROOT, 2022.03.29:
 * - movable freelist에서 high_pfn미만 주소의 page이면서 skip bit가 clear인
 *   page(이미 한번 tail로 보낸 page가 아닌)를 찾는다.
 *   찾는데 성공한다면 fast search관련 변수를 update한다.
 *
 * - 종료조건
 *   1. order가 PAGE_ALLOC_COSTLY_ORDER 보다 작아진경우
 *   2. found_block이 true가 된경우
 *   3. 스캔횟수(nr_scanned)가 limit보다 크거나 같아진경우.
 *
 * - limit
 * fast_search_fail limit
 * 0                33
 * 1                17
 * 2                9
 * 3                5
 * 4                3
 * 5                2          
 * 6                1
 *
 */
	for (order = cc->order - 1;
	     order >= PAGE_ALLOC_COSTLY_ORDER && !found_block && nr_scanned < limit;
	     order--) {
		struct free_area *area = &cc->zone->free_area[order];
		struct list_head *freelist;
		unsigned long flags;
		struct page *freepage;

		if (!area->nr_free)
			continue;

		spin_lock_irqsave(&cc->zone->lock, flags);
		freelist = &area->free_list[MIGRATE_MOVABLE];
/*
 * IAMROOT, 2022.03.29:
 * - movable freelist에서 최대 limit만큼 시도하여 high_pfn미만의 주소를 가진,
 *   skip bit가 clear인 page를 찾는다.
 *   찾았다면 해당 page를 tail로 보내고 fast search관련 변수를 업데이트
 *   한후 종료한다.
 */
		list_for_each_entry(freepage, freelist, lru) {
			unsigned long free_pfn;

			if (nr_scanned++ >= limit) {
/*
 * IAMROOT, 2022.03.29:
 * - 제일 마지막 freepage는 tail로 보낸다.
 */
				move_freelist_tail(freelist, freepage);
				break;
			}

			free_pfn = page_to_pfn(freepage);
			if (free_pfn < high_pfn) {
				/*
				 * Avoid if skipped recently. Ideally it would
				 * move to the tail but even safe iteration of
				 * the list assumes an entry is deleted, not
				 * reordered.
				 */
/*
 * IAMROOT, 2022.03.29:
 * - skip bit가 있으면 이미 tail로 옮긴 page이므로 pass한다.
 */
				if (get_pageblock_skip(freepage))
					continue;

				/* Reorder to so a future search skips recent pages */
				move_freelist_tail(freelist, freepage);
/*
 * IAMROOT, 2022.03.29:
 * - compact_zone 함수를 통해 현재 함수에 진입한 경우 최초에
 *   cc->fast_start_pfn은 초기화 됬을 것이다.
 *   min(free_pfn, cc->fast_start_pfn)으로 업데이트 된다.
 * - skip bit를 set하고 tail로 옮긴다.
 */
				update_fast_start_pfn(cc, free_pfn);
				pfn = pageblock_start_pfn(free_pfn);
				cc->fast_search_fail = 0;
				found_block = true;
				set_pageblock_skip(freepage);
				break;
			}
		}
		spin_unlock_irqrestore(&cc->zone->lock, flags);
	}

	cc->total_migrate_scanned += nr_scanned;

	/*
	 * If fast scanning failed then use a cached entry for a page block
	 * that had free pages as the basis for starting a linear scan.
	 */
	if (!found_block) {
		cc->fast_search_fail++;
		pfn = reinit_migrate_pfn(cc);
	}
	return pfn;
}

/*
 * Isolate all pages that can be migrated from the first suitable block,
 * starting at the block pointed to by the migrate scanner pfn within
 * compact_control.
 */
/*
 * IAMROOT, 2022.03.26: 
 * migrate 스캐너가 진행중인 페이지 블럭 중에 isolation을 시도할 블럭
 * 하나를 선택해서 isolation을 진행하고 다음 3가지 결과중 하나를 반환한다.
 * ISOLATON_ABORT : 중단.
 * ISOLATION_SUCCESS : isolate된 page가 존재.
 * ISOLATION_NONE : isolate된 page가 하나도 없음.
 */
static isolate_migrate_t isolate_migratepages(struct compact_control *cc)
{
	unsigned long block_start_pfn;
	unsigned long block_end_pfn;
	unsigned long low_pfn;
	struct page *page;
	const isolate_mode_t isolate_mode =
		(sysctl_compact_unevictable_allowed ? ISOLATE_UNEVICTABLE : 0) |
		(cc->mode != MIGRATE_SYNC ? ISOLATE_ASYNC_MIGRATE : 0);
	bool fast_find_block;

	/*
	 * Start at where we last stopped, or beginning of the zone as
	 * initialized by compact_zone(). The first failure will use
	 * the lowest PFN as the starting point for linear scanning.
	 */
/*
 * IAMROOT, 2022.03.29:
 * - compact범위의 12.5~50%내의 범위에서 [PAGE_ALLOC_COSTLY_ORDER, cc->order) 
 *   의 freelist에서 skip bit가 없던 freepage의 주소를 가져오는걸 시도한다.
 *   실패하거나 find조건에 안맞다면 cc->migrate_pfn == low_pfn
 */
	low_pfn = fast_find_migrateblock(cc);
	block_start_pfn = pageblock_start_pfn(low_pfn);
	if (block_start_pfn < cc->zone->zone_start_pfn)
		block_start_pfn = cc->zone->zone_start_pfn;

	/*
	 * fast_find_migrateblock marks a pageblock skipped so to avoid
	 * the isolation_suitable check below, check whether the fast
	 * search was successful.
	 */
/*
 * IAMROOT, 2022.03.29:
 * - fast_find_migrateblock에서 block이 찾아졌다면 cc->fast_search_fail == 0
 *   이고 low_pfn은 cc->migrate_pfn과 다를것이다. fast find 결과를
 *   저장해놓는것이다.
 */
	fast_find_block = low_pfn != cc->migrate_pfn && !cc->fast_search_fail;

	/* Only scan within a pageblock boundary */
	block_end_pfn = pageblock_end_pfn(low_pfn);

	/*
	 * Iterate over whole pageblocks until we find the first suitable.
	 * Do not cross the free scanner.
	 */
	for (; block_end_pfn <= cc->free_pfn;
			fast_find_block = false,
			cc->migrate_pfn = low_pfn = block_end_pfn,
			block_start_pfn = block_end_pfn,
			block_end_pfn += pageblock_nr_pages) {

		/*
		 * This can potentially iterate a massively long zone with
		 * many pageblocks unsuitable, so periodically check if we
		 * need to schedule.
		 */
/*
 * IAMROOT, 2022.03.26: 
 * 32블럭 단위로 preempt point를 수행하도록 코드가 구성되어 있다.
 */
		if (!(low_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages)))
			cond_resched();

/*
 * IAMROOT, 2022.03.26: 
 * [block_start_pfn, block_end_pfn)이 모두 zone에 포함된 online 메모리가 아니면
 * skip 한다.
 */
		page = pageblock_pfn_to_page(block_start_pfn,
						block_end_pfn, cc->zone);
		if (!page)
			continue;

		/*
		 * If isolation recently failed, do not retry. Only check the
		 * pageblock once. COMPACT_CLUSTER_MAX causes a pageblock
		 * to be visited multiple times. Assume skip was checked
		 * before making it "skip" so other compaction instances do
		 * not scan the same block.
		 */
/*
 * IAMROOT, 2022.03.28:
 * - 제일 처음 iterate에서 fast_find_block이 true일수있다. 이 if문은
 *   동작을 안시킬것이다. 두번째 iterate부터는 fast_find_block은 무조건 false
 *   가 되므로 isolation_suitable을 확인할것이다.
 */
		if (IS_ALIGNED(low_pfn, pageblock_nr_pages) &&
		    !fast_find_block && !isolation_suitable(cc, page))
			continue;

		/*
		 * For async compaction, also only scan in MOVABLE blocks
		 * without huge pages. Async compaction is optimistic to see
		 * if the minimum amount of work satisfies the allocation.
		 * The cached PFN is updated as it's possible that all
		 * remaining blocks between source and target are unsuitable
		 * and the compaction scanners fail to meet.
		 */
		if (!suitable_migration_source(cc, page)) {
			update_cached_migrate(cc, block_end_pfn);
			continue;
		}

/*
 * IAMROOT, 2022.03.26: 
 * 한 블럭만 isolation을 시도한다.
 */

		/* Perform the isolation */
		if (isolate_migratepages_block(cc, low_pfn, block_end_pfn,
						isolate_mode))
			return ISOLATE_ABORT;

		/*
		 * Either we isolated something and proceed with migration. Or
		 * we failed and compact_zone should decide if we should
		 * continue or not.
		 */
		break;
	}

	return cc->nr_migratepages ? ISOLATE_SUCCESS : ISOLATE_NONE;
}

/*
 * order == -1 is expected when compacting via
 * /proc/sys/vm/compact_memory
 */
/*
 * IAMROOT, 2022.03.19:
 * - 다음과 같은 compaction 호출에서 order 입력없이 들어온다.
 *   1) manual compaction:  
 *   2) pro-active compaction:
 *   3) alloc_contig_range():
 *  그외 direct compaction과 kcompactd의 경우 order가 지정된다.
 */
static inline bool is_via_compact_memory(int order)
{
	return order == -1;
}

static bool kswapd_is_running(pg_data_t *pgdat)
{
	return pgdat->kswapd && task_is_running(pgdat->kswapd);
}

/*
 * A zone's fragmentation score is the external fragmentation wrt to the
 * COMPACTION_HPAGE_ORDER. It returns a value in the range [0, 100].
 */
static unsigned int fragmentation_score_zone(struct zone *zone)
{
	return extfrag_for_order(zone, COMPACTION_HPAGE_ORDER);
}

/*
 * A weighted zone's fragmentation score is the external fragmentation
 * wrt to the COMPACTION_HPAGE_ORDER scaled by the zone's size. It
 * returns a value in the range [0, 100].
 *
 * The scaling factor ensures that proactive compaction focuses on larger
 * zones like ZONE_NORMAL, rather than smaller, specialized zones like
 * ZONE_DMA32. For smaller zones, the score value remains close to zero,
 * and thus never exceeds the high threshold for proactive compaction.
 */
static unsigned int fragmentation_score_zone_weighted(struct zone *zone)
{
	unsigned long score;

	score = zone->present_pages * fragmentation_score_zone(zone);
	return div64_ul(score, zone->zone_pgdat->node_present_pages + 1);
}

/*
 * The per-node proactive (background) compaction process is started by its
 * corresponding kcompactd thread when the node's fragmentation score
 * exceeds the high threshold. The compaction process remains active till
 * the node's score falls below the low threshold, or one of the back-off
 * conditions is met.
 */
static unsigned int fragmentation_score_node(pg_data_t *pgdat)
{
	unsigned int score = 0;
	int zoneid;

	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
		struct zone *zone;

		zone = &pgdat->node_zones[zoneid];
		score += fragmentation_score_zone_weighted(zone);
	}

	return score;
}

static unsigned int fragmentation_score_wmark(pg_data_t *pgdat, bool low)
{
	unsigned int wmark_low;

	/*
	 * Cap the low watermark to avoid excessive compaction
	 * activity in case a user sets the proactiveness tunable
	 * close to 100 (maximum).
	 */
	wmark_low = max(100U - sysctl_compaction_proactiveness, 5U);
	return low ? wmark_low : min(wmark_low + 10, 100U);
}

static bool should_proactive_compact_node(pg_data_t *pgdat)
{
	int wmark_high;

	if (!sysctl_compaction_proactiveness || kswapd_is_running(pgdat))
		return false;

	wmark_high = fragmentation_score_wmark(pgdat, false);
	return fragmentation_score_node(pgdat) > wmark_high;
}

/*
 * IAMROOT, 2022.04.02:
 * - @cc에 따라 compact를 어떻게 할지(complete, skip, continue)를 결정한다.
 */
static enum compact_result __compact_finished(struct compact_control *cc)
{
	unsigned int order;
	const int migratetype = cc->migratetype;
	int ret;

	/* Compaction run completes if the migrate and free scanner meet */
/*
 * IAMROOT, 2022.03.26: 
 * 해당 zone 전체 블럭에 대해 스캐닝이 완료했으면
 * 캐시들을 존의 가장 최하측및 상위 블럭으로 지정한다.
 */
	if (compact_scanners_met(cc)) {
		/* Let the next compaction start anew. */
		reset_cached_positions(cc->zone);

		/*
		 * Mark that the PG_migrate_skip information should be cleared
		 * by kswapd when it goes to sleep. kcompactd does not set the
		 * flag itself as the decision to be clear should be directly
		 * based on an allocation request.
		 */
		if (cc->direct_compaction)
			cc->zone->compact_blockskip_flush = true;

/*
 * IAMROOT, 2022.03.26: 
 * zone의 전체에 대해 진행을 하도록 시작하여 끝까지 완료한 경우 complete로 
 * 끝나고, 그렇지 않고 중간에 시작한 경우는 partial skipped로 끝난다.
 */
		if (cc->whole_zone)
			return COMPACT_COMPLETE;
		else
			return COMPACT_PARTIAL_SKIPPED;
	}

/*
 * IAMROOT, 2022.04.02:
 * - proactive 요청이 왔을때의 처리.
 */
	if (cc->proactive_compaction) {
		int score, wmark_low;
		pg_data_t *pgdat;

		pgdat = cc->zone->zone_pgdat;
		if (kswapd_is_running(pgdat))
			return COMPACT_PARTIAL_SKIPPED;

		score = fragmentation_score_zone(cc->zone);
		wmark_low = fragmentation_score_wmark(pgdat, true);

		if (score > wmark_low)
			ret = COMPACT_CONTINUE;
		else
			ret = COMPACT_SUCCESS;

		goto out;
	}

/*
 * IAMROOT, 2022.03.26: 
 * order가 지정되지 않는 compaction 요청들의 경우이다.
 */
	if (is_via_compact_memory(cc->order))
		return COMPACT_CONTINUE;

	/*
	 * Always finish scanning a pageblock to reduce the possibility of
	 * fallbacks in the future. This is particularly important when
	 * migration source is unmovable/reclaimable but it's not worth
	 * special casing.
	 */
/*
 * IAMROOT, 2022.03.26: 
 * 지금 진행중인 migrate 스캐너 위치가 페이지 블럭단위의 시작이 아닌 경우이다.
 */
	if (!IS_ALIGNED(cc->migrate_pfn, pageblock_nr_pages))
		return COMPACT_CONTINUE;

	/* Direct compactor: Is a suitable page free? */
	ret = COMPACT_NO_SUITABLE_PAGE;

/*
 * IAMROOT, 2022.04.02:
 * - cc->order의 순서로 cc->migratetype 의 freearea를 iterator한다.
 */
	for (order = cc->order; order < MAX_ORDER; order++) {
		struct free_area *area = &cc->zone->free_area[order];
		bool can_steal;

		/* Job done if page is free of the right migratetype */
		if (!free_area_empty(area, migratetype))
			return COMPACT_SUCCESS;

#ifdef CONFIG_CMA
		/* MIGRATE_MOVABLE can fallback on MIGRATE_CMA */
/*
 * IAMROOT, 2022.04.02:
 * - movable 요청인 경우에는 cma도 검사를 해본다. 즉 movable요청이면
 *   movable, cma free_area 둘중하나라도 free 용량이 있으면 success
 */
		if (migratetype == MIGRATE_MOVABLE &&
			!free_area_empty(area, MIGRATE_CMA))
			return COMPACT_SUCCESS;
#endif
		/*
		 * Job done if allocation would steal freepages from
		 * other migratetype buddy lists.
		 */
/*
 * IAMROOT, 2022.04.02:
 * - fallback list도 뒤져본다. fall list에도 없으면 다음 order로
 *   iterate
 */
		if (find_suitable_fallback(area, order, migratetype,
						true, &can_steal) != -1) {

			/* movable pages are OK in any pageblock */
			if (migratetype == MIGRATE_MOVABLE)
				return COMPACT_SUCCESS;

			/*
			 * We are stealing for a non-movable allocation. Make
			 * sure we finish compacting the current pageblock
			 * first so it is as free as possible and we won't
			 * have to steal another one soon. This only applies
			 * to sync compaction, as async compaction operates
			 * on pageblocks of the same migratetype.
			 */
/*
 * IAMROOT, 2022.04.02:
 * - unmovable 이나 reclaim type으로 steal을 해온 경우 좀 더 compact를
 *   해야될수있으므로 Continue를 처리한다.
 *   하지만 async 요청이면 적당히 짧게 끝내는 개념으로, 아니면 pageblock 단위
 *   로 적당한 범위로 success 처리한다.
 */
			if (cc->mode == MIGRATE_ASYNC ||
					IS_ALIGNED(cc->migrate_pfn,
							pageblock_nr_pages)) {
				return COMPACT_SUCCESS;
			}

			ret = COMPACT_CONTINUE;
			break;
		}
	}

out:
	if (cc->contended || fatal_signal_pending(current))
		ret = COMPACT_CONTENDED;

	return ret;
}

/*
 * IAMROOT, 2022.04.02:
 * - @cc에 따라 compact 상태(complete, skip, continue)를 결정한다.
 */
static enum compact_result compact_finished(struct compact_control *cc)
{
	int ret;

	ret = __compact_finished(cc);
	trace_mm_compaction_finished(cc->zone, cc->order, ret);
	if (ret == COMPACT_NO_SUITABLE_PAGE)
		ret = COMPACT_CONTINUE;

	return ret;
}

/*
 * IAMROOT, 2022.03.19:
 * - compact가 가능한 상황인지에 대해 확인한다.
 * - COMPACT_CONTINUE
 *   1. proc에서 요청한 경우.
 *   2. comapct를 위한 order 0 page공간이 확보된경우.
 *
 * - COMPACT_SUCCESS
 *   1. 다른 cpu나 기타의 이유로 free memory가 확보되어 있는 경우.
 *
 * - COMPACT_SKIPPED
 *   1. compact를 하기 위한 order 0 page도 확보가 불가능한경우.
 */
static enum compact_result __compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int highest_zoneidx,
					unsigned long wmark_target)
{
	unsigned long watermark;

/*
 * IAMROOT, 2022.03.19:
 * - proc을 통해 compaction을 호출한거면 continue return
 */
	if (is_via_compact_memory(order))
		return COMPACT_CONTINUE;

	watermark = wmark_pages(zone, alloc_flags & ALLOC_WMARK_MASK);
	/*
	 * If watermarks for high-order allocation are already met, there
	 * should be no need for compaction at all.
	 */
/*
 * IAMROOT, 2022.03.19:
 * - watermark의 free memory가 충분하면 success로 return. 다른 cpu측에서
 *   memory를 한번에 많이 풀어줬거나 demon등에서 회수를 한것으로 예상된다.
 */
	if (zone_watermark_ok(zone, order, watermark, highest_zoneidx,
								alloc_flags))
		return COMPACT_SUCCESS;

	/*
	 * Watermarks for order-0 must be met for compaction to be able to
	 * isolate free pages for migration targets. This means that the
	 * watermark and alloc_flags have to match, or be more pessimistic than
	 * the check in __isolate_free_page(). We don't use the direct
	 * compactor's alloc_flags, as they are not relevant for freepage
	 * isolation. We however do use the direct compactor's highest_zoneidx
	 * to skip over zones where lowmem reserves would prevent allocation
	 * even if compaction succeeds.
	 * For costly orders, we require low watermark instead of min for
	 * compaction to proceed to increase its chances.
	 * ALLOC_CMA is used, as pages in CMA pageblocks are considered
	 * suitable migration targets
	 */
/*
 * IAMROOT, 2022.03.19:
 * --- papago
 *   order-0의 워터마크가 충족되면 migration target의 빈 페이지를 분리(isolate)
 *   할수있다.
 *   즉, 워터마크와 allocate_flags가 일치하거나 __isolate_free_page()의
 *   체크보다 비관적이어야 합니다. direct compressor의 allocate_flags는
 *   프리 페이지 분리와 관련이 없기 때문에 사용하지 않습니다. 단, 압축이
 *   성공하더라도 lowmem 예약이 할당되지 않는 영역을 건너뛰기 위해
 *   direct compact의 highest_zoneidx를 사용합니다.
 *   costly order의 경우, 가능성을 높이기 위해 compaction을 진행하려면
 *   min이 아닌 low watermark가 필요합니다.
 *   CMA 페이지 블록 내의 페이지가 적절한 이행 타깃으로 간주되기 때문에
 *   ALLOCK_CMA가 사용됩니다. 
 */
	watermark = (order > PAGE_ALLOC_COSTLY_ORDER) ?
				low_wmark_pages(zone) : min_wmark_pages(zone);
	watermark += compact_gap(order);

/*
 * IAMROOT, 2022.03.19:
 * - order 0의 page가 확보가 되는지 확인한다.
 */
	if (!__zone_watermark_ok(zone, 0, watermark, highest_zoneidx,
						ALLOC_CMA, wmark_target))
		return COMPACT_SKIPPED;

	return COMPACT_CONTINUE;
}

/*
 * compaction_suitable: Is this suitable to run compaction on this zone now?
 * Returns
 *   COMPACT_SKIPPED  - If there are too few free pages for compaction
 *   COMPACT_SUCCESS  - If the allocation would succeed without compaction
 *   COMPACT_CONTINUE - If compaction should run now
 */
/*
 * IAMROOT, 2022.03.19:
 * - compaction 가능한 상황인지 확인한다.
 * - COMPACT_CONTINUE
 *   1. proc에서 요청한 경우.
 *   2. order에 대한 즉시 할당이 안되는 상황이라 compact를 해야되며,
 *   comapct를 위한 order 0 page공간이 확보된경우.
 *
 * - COMPACT_SUCCESS
 *   1. 다른 cpu나 기타의 이유로 free memory가 확보되어 compact를 할 필요가 없는
 *   경우.
 *
 * - COMPACT_SKIPPED
 *   1. compact를 하기 위한 order 0 page도 확보가 불가능한경우.
 *   메모리 단편화의 이유로 compact가 불가능한경우
 *
 * - COMPACT_CONTINUE요건에서 order가 costly_order보다 높은 경우
 *   fragmentation_index가 compaction에 비적합인 경우 COMPACT_SKIPPED로
 *   변경한다.
 */
enum compact_result compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int highest_zoneidx)
{
	enum compact_result ret;
	int fragindex;

	ret = __compaction_suitable(zone, order, alloc_flags, highest_zoneidx,
				    zone_page_state(zone, NR_FREE_PAGES));
	/*
	 * fragmentation index determines if allocation failures are due to
	 * low memory or external fragmentation
	 *
	 * index of -1000 would imply allocations might succeed depending on
	 * watermarks, but we already failed the high-order watermark check
	 * index towards 0 implies failure is due to lack of memory
	 * index towards 1000 implies failure is due to fragmentation
	 *
	 * Only compact if a failure would be due to fragmentation. Also
	 * ignore fragindex for non-costly orders where the alternative to
	 * a successful reclaim/compaction is OOM. Fragindex and the
	 * vm.extfrag_threshold sysctl is meant as a heuristic to prevent
	 * excessive compaction for costly orders, but it should not be at the
	 * expense of system stability.
	 */
/*
 * IAMROOT, 2022.03.19:
 * - costly_order 보다 높은 order에서 할당요청을 해서 memory가 확보된 경우에
 *   fragment index를 구해온다. 0 <= idx <= sysctl_extfrag_threshold 사이면
 *   compaction으로 해결을 못하는 상황으로 간주하여 return값을
 *   COMPACT_NOT_SUITABLE_ZONE로 해서 후에 COMPACT_SKIPPED가 된다.
 *   fragindex가 -값이면 할당가능 상태이므로 COMPACT_CONTINUE를 그대로 유지한다.
 */
	if (ret == COMPACT_CONTINUE && (order > PAGE_ALLOC_COSTLY_ORDER)) {
		fragindex = fragmentation_index(zone, order);
		if (fragindex >= 0 && fragindex <= sysctl_extfrag_threshold)
			ret = COMPACT_NOT_SUITABLE_ZONE;
	}

	trace_mm_compaction_suitable(zone, order, ret);
	if (ret == COMPACT_NOT_SUITABLE_ZONE)
		ret = COMPACT_SKIPPED;

	return ret;
}

/*
 * IAMROOT, 2022.04.09:
 * - @ac에 등록된 zone들이 compaction가능한지 reclimable page + free page snapshot
 *   을 확인한다. 모든 zone이 skipped(0 order도 없는 상태)라면 false.
 */
bool compaction_zonelist_suitable(struct alloc_context *ac, int order,
		int alloc_flags)
{
	struct zone *zone;
	struct zoneref *z;

	/*
	 * Make sure at least one zone would pass __compaction_suitable if we continue
	 * retrying the reclaim.
	 */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist,
				ac->highest_zoneidx, ac->nodemask) {
		unsigned long available;
		enum compact_result compact_result;

		/*
		 * Do not consider all the reclaimable memory because we do not
		 * want to trash just for a single high order allocation which
		 * is even not guaranteed to appear even if __compaction_suitable
		 * is happy about the watermark check.
		 */
		available = zone_reclaimable_pages(zone) / order;
		available += zone_page_state_snapshot(zone, NR_FREE_PAGES);
		compact_result = __compaction_suitable(zone, order, alloc_flags,
				ac->highest_zoneidx, available);
		if (compact_result != COMPACT_SKIPPED)
			return true;
	}

	return false;
}

/*
 * IAMROOT, 2022.04.09:
 * - compact 수행
 *   1. migrate scanner, free scanner를 사용하여 migratable, free page를 찾는다.
 *   2. 다음 comacpt를 위해 범위를 cache해놓는다.
 *   3. migrate할 page들을 isolate를 시키고, 성공을 한다면 free page에
 *	migrate를 진행한다.
 */
static enum compact_result
compact_zone(struct compact_control *cc, struct capture_control *capc)
{
	enum compact_result ret;
	unsigned long start_pfn = cc->zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(cc->zone);
	unsigned long last_migrated_pfn;
	const bool sync = cc->mode != MIGRATE_ASYNC;
	bool update_cached;

/*
 * IAMROOT, 2022.03.19:
 * - @cc를 초기화한다.
 */
	/*
	 * These counters track activities during zone compaction.  Initialize
	 * them before compacting a new zone.
	 */
	cc->total_migrate_scanned = 0;
	cc->total_free_scanned = 0;
	cc->nr_migratepages = 0;
	cc->nr_freepages = 0;
	INIT_LIST_HEAD(&cc->freepages);
	INIT_LIST_HEAD(&cc->migratepages);

	cc->migratetype = gfp_migratetype(cc->gfp_mask);
/*
 * IAMROOT, 2022.03.19:
 * - compaction suitable test를 한다.
 */
	ret = compaction_suitable(cc->zone, cc->order, cc->alloc_flags,
							cc->highest_zoneidx);
	/* Compaction is likely to fail */
/*
 * IAMROOT, 2022.03.19:
 * - COMPACT_SUCCESS인 경우 이미 memory가 존재하고, COMPACT_SKIPPED은 compact를
 *   못하는 상황이다.
 */
	if (ret == COMPACT_SUCCESS || ret == COMPACT_SKIPPED)
		return ret;

	/* huh, compaction_suitable is returning something unexpected */
	VM_BUG_ON(ret != COMPACT_CONTINUE);

	/*
	 * Clear pageblock skip if there were failures recently and compaction
	 * is about to be retried after being deferred.
	 */
/*
 * IAMROOT, 2022.03.25:
 * - 이 전에 compact가 실패한 상태에서 현재 요청 @cc->order가 이전 compact
 *   실패 order보다 크며 마지막 defer까지 이미 진행하였었으면(shift=6단계, 
 *   count=63까지) __reset_isolation_suitable을 실행한다.
 *
 *   defer  0~2, 0~4, 0~8, 0~16, 0~32, 0~64
 *                                       ^--- 끝
 */
	if (compaction_restarting(cc->zone, cc->order))
		__reset_isolation_suitable(cc->zone);

	/*
	 * Setup to move all movable pages to the end of the zone. Used cached
	 * information on where the scanners should start (unless we explicitly
	 * want to compact the whole zone), but check that it is initialised
	 * by ensuring the values are within zone boundaries.
	 */
/*
 * IAMROOT, 2022.03.29:
 * - isolate_migratepages함수에서 fast_find_migrateblock를 할때
 *   update_fast_start_pfn를 통하여
 *   가장 주소가 낮은 fast_start_pfn을 기록하기 위해 0으로 초기화해놓는다.
 */
	cc->fast_start_pfn = 0;
/*
 * IAMROOT, 2022.03.26: 
 * while_zone: 
 *    1=zone의 처음 부터 시작한다.
 *    0=기존에 캐시되어 있던 위치부터 시작한다.
 */
	if (cc->whole_zone) {
		cc->migrate_pfn = start_pfn;
		cc->free_pfn = pageblock_start_pfn(end_pfn - 1);
	} else {
		cc->migrate_pfn = cc->zone->compact_cached_migrate_pfn[sync];
		cc->free_pfn = cc->zone->compact_cached_free_pfn;
		if (cc->free_pfn < start_pfn || cc->free_pfn >= end_pfn) {
			cc->free_pfn = pageblock_start_pfn(end_pfn - 1);
			cc->zone->compact_cached_free_pfn = cc->free_pfn;
		}
		if (cc->migrate_pfn < start_pfn || cc->migrate_pfn >= end_pfn) {
			cc->migrate_pfn = start_pfn;
			cc->zone->compact_cached_migrate_pfn[0] = cc->migrate_pfn;
			cc->zone->compact_cached_migrate_pfn[1] = cc->migrate_pfn;
		}

		if (cc->migrate_pfn <= cc->zone->compact_init_migrate_pfn)
			cc->whole_zone = true;
	}

	last_migrated_pfn = 0;

	/*
	 * Migrate has separate cached PFNs for ASYNC and SYNC* migration on
	 * the basis that some migrations will fail in ASYNC mode. However,
	 * if the cached PFNs match and pageblocks are skipped due to having
	 * no isolation candidates, then the sync state does not matter.
	 * Until a pageblock with isolation candidates is found, keep the
	 * cached PFNs in sync to avoid revisiting the same blocks.
	 */
/*
 * IAMROOT, 2022.04.02:
 * - aync일 경우 isolate 실패시 sync cache를 aync cache로 갱신해야되는 여부를
 *   판별하기위한 flag.
 * - aync요청일 경우 scan을 하면서 compact_cached_migrate_pfn이 update될텐데,
 *   만약 실패할경우 sync compact_cached_migrate_pfn을 async 완료지점으로 동기화시켜
 *   줌으로써 나중에 sync mode에서 동작할 때 aync에서 isolate실패했던 범위를
 *   건너뛰게 하기 위함이다.
 *
 */
	update_cached = !sync &&
		cc->zone->compact_cached_migrate_pfn[0] == cc->zone->compact_cached_migrate_pfn[1];

	trace_mm_compaction_begin(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync);

	/* lru_add_drain_all could be expensive with involving other CPUs */
	lru_add_drain();

	while ((ret = compact_finished(cc)) == COMPACT_CONTINUE) {
		int err;
		unsigned long iteration_start_pfn = cc->migrate_pfn;

		/*
		 * Avoid multiple rescans which can happen if a page cannot be
		 * isolated (dirty/writeback in async mode) or if the migrated
		 * pages are being allocated before the pageblock is cleared.
		 * The first rescan will capture the entire pageblock for
		 * migration. If it fails, it'll be marked skip and scanning
		 * will proceed as normal.
		 */
/*
 * IAMROOT, 2022.04.02:
 * - rescan을 false로 초기화한다.
 */
		cc->rescan = false;
		if (pageblock_start_pfn(last_migrated_pfn) ==
		    pageblock_start_pfn(iteration_start_pfn)) {
			cc->rescan = true;
		}

		switch (isolate_migratepages(cc)) {
		case ISOLATE_ABORT:
/*
 * IAMROOT, 2022.04.02:
 * - 중단됫으므로 isolate된 page들을 원래위치로 되돌린다.
 */
			ret = COMPACT_CONTENDED;
			putback_movable_pages(&cc->migratepages);
			cc->nr_migratepages = 0;
			goto out;
		case ISOLATE_NONE:
/*
 * IAMROOT, 2022.04.02:
 * - isolate된 page가 하나도 없다. update_cached가 true라면 scan 시작시
 *   sync과 aync의 cache가 같은 상황이면서 현재 async인 상황인데, async에서
 *   isolatge 실패를 했으므로 sync에서 같은 구간을 다시 scan안하도록 sync cache를
 *   현재 완료된 async cache로 update해준다.
 */
			if (update_cached) {
				cc->zone->compact_cached_migrate_pfn[1] =
					cc->zone->compact_cached_migrate_pfn[0];
			}

			/*
			 * We haven't isolated and migrated anything, but
			 * there might still be unflushed migrations from
			 * previous cc->order aligned block.
			 */
			goto check_drain;
		case ISOLATE_SUCCESS:
/*
 * IAMROOT, 2022.04.02:
 * - isolate가 성공했으므로 update cache를 할필요가 없다.
 */
			update_cached = false;
			last_migrated_pfn = iteration_start_pfn;
		}

/*
 * IAMROOT, 2022.04.09:
 * - migrate(unmap + copy + map or unmap)
 */
		err = migrate_pages(&cc->migratepages, compaction_alloc,
				compaction_free, (unsigned long)cc, cc->mode,
				MR_COMPACTION, NULL);

		trace_mm_compaction_migratepages(cc->nr_migratepages, err,
							&cc->migratepages);

		/* All pages were either migrated or will be released */
		cc->nr_migratepages = 0;
/*
 * IAMROOT, 2022.04.09:
 * - error가 발생하면 migrate_pages를 전부 되돌리고 시작점을 다시 정한다.
 */
		if (err) {
			putback_movable_pages(&cc->migratepages);
			/*
			 * migrate_pages() may return -ENOMEM when scanners meet
			 * and we want compact_finished() to detect it
			 */
			if (err == -ENOMEM && !compact_scanners_met(cc)) {
				ret = COMPACT_CONTENDED;
				goto out;
			}
			/*
			 * We failed to migrate at least one page in the current
			 * order-aligned block, so skip the rest of it.
			 */
			if (cc->direct_compaction &&
						(cc->mode == MIGRATE_ASYNC)) {
				cc->migrate_pfn = block_end_pfn(
						cc->migrate_pfn - 1, cc->order);
				/* Draining pcplists is useless in this case */
				last_migrated_pfn = 0;
			}
		}

check_drain:
		/*
		 * Has the migration scanner moved away from the previous
		 * cc->order aligned block where we migrated from? If yes,
		 * flush the pages that were freed, so that they can merge and
		 * compact_finished() can detect immediately if allocation
		 * would succeed.
		 */
		if (cc->order > 0 && last_migrated_pfn) {
			unsigned long current_block_start =
				block_start_pfn(cc->migrate_pfn, cc->order);

			if (last_migrated_pfn < current_block_start) {
				lru_add_drain_cpu_zone(cc->zone);
				/* No more flushing until we migrate again */
				last_migrated_pfn = 0;
			}
		}

		/* Stop if a page has been captured */
/*
 * IAMROOT, 2022.04.02:
 * - capture page가 있는 경우 success로 하고 종료한다.
 */
		if (capc && capc->page) {
			ret = COMPACT_SUCCESS;
			break;
		}
	}

out:
	/*
	 * Release free pages and update where the free scanner should restart,
	 * so we don't leave any returned pages behind in the next attempt.
	 */
/*
 * IAMROOT, 2022.03.26: 
 * compaction이 종료된 이후에 아직 처리되지 않은 free 스캐너가 모아놓은
 * cc->freepages의 페이지들을 다시 버디 시스템으로 되돌린다.
 * 되돌리는 과정에서 high pfn 페이지의 경우 기존에 기억해둔 free 스캐너 캐시
 * 시작위치보다 더 높은 경우 다시 free 스캐너 캐시 위치에 갱신한다.
 */
	if (cc->nr_freepages > 0) {
		unsigned long free_pfn = release_freepages(&cc->freepages);

		cc->nr_freepages = 0;
		VM_BUG_ON(free_pfn == 0);
		/* The cached pfn is always the first in a pageblock */
		free_pfn = pageblock_start_pfn(free_pfn);
		/*
		 * Only go back, not forward. The cached pfn might have been
		 * already reset to zone end in compact_finished()
		 */
		if (free_pfn > cc->zone->compact_cached_free_pfn)
			cc->zone->compact_cached_free_pfn = free_pfn;
	}

	count_compact_events(COMPACTMIGRATE_SCANNED, cc->total_migrate_scanned);
	count_compact_events(COMPACTFREE_SCANNED, cc->total_free_scanned);

	trace_mm_compaction_end(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync, ret);

	return ret;
}

/*
 * IAMROOT, 2022.04.09:
 * - compact_zone을 실행하기 위한 cc설정을 하고, compact_zone을 수행한다.
 */
static enum compact_result compact_zone_order(struct zone *zone, int order,
		gfp_t gfp_mask, enum compact_priority prio,
		unsigned int alloc_flags, int highest_zoneidx,
		struct page **capture)
{
	enum compact_result ret;
	struct compact_control cc = {
		.order = order,
		.search_order = order,
		.gfp_mask = gfp_mask,
		.zone = zone,
		.mode = (prio == COMPACT_PRIO_ASYNC) ?
					MIGRATE_ASYNC :	MIGRATE_SYNC_LIGHT,
		.alloc_flags = alloc_flags,
		.highest_zoneidx = highest_zoneidx,
/*
 * IAMROOT, 2022.03.19:
 * - direct compaction.
 */
		.direct_compaction = true,
/*
 * IAMROOT, 2022.03.19:
 * - sync full로 들어오면 모든 zone, no skip, 모든 block을 시도
 *   (적절성 검사안함) 한다는것.
 */
		.whole_zone = (prio == MIN_COMPACT_PRIORITY),
		.ignore_skip_hint = (prio == MIN_COMPACT_PRIORITY),
		.ignore_block_suitable = (prio == MIN_COMPACT_PRIORITY)
	};
	struct capture_control capc = {
		.cc = &cc,
		.page = NULL,
	};

	/*
	 * Make sure the structs are really initialized before we expose the
	 * capture control, in case we are interrupted and the interrupt handler
	 * frees a page.
	 */
	barrier();

/*
 * IAMROOT, 2022.03.19:
 * - current task의 capture_control에 capc를 한번 달아놨다가 compact_zone이
 *   끝나면 초기화한다.
 */
	WRITE_ONCE(current->capture_control, &capc);

	ret = compact_zone(&cc, &capc);

	VM_BUG_ON(!list_empty(&cc.freepages));
	VM_BUG_ON(!list_empty(&cc.migratepages));

	/*
	 * Make sure we hide capture control first before we read the captured
	 * page pointer, otherwise an interrupt could free and capture a page
	 * and we would leak it.
	 */
	WRITE_ONCE(current->capture_control, NULL);
	*capture = READ_ONCE(capc.page);
	/*
	 * Technically, it is also possible that compaction is skipped but
	 * the page is still captured out of luck(IRQ came and freed the page).
	 * Returning COMPACT_SUCCESS in such cases helps in properly accounting
	 * the COMPACT[STALL|FAIL] when compaction is skipped.
	 */
/*
 * IAMROOT, 2022.03.19:
 * - 할당이 성공됬으면 COMPACT_SUCCESS
 */
	if (*capture)
		ret = COMPACT_SUCCESS;

	return ret;
}

/*
 * IAMROOT, 2022.04.26:
 * -요청 order에 대한 buddy가 없는 경우, __fragmentation_index을 통해서
 *  현재 system이 fragment때문에 실패하는지, memory 부족으로 실패했는지
 *  예상한다. 이 예측값을 토대로 compaction_suitable등의 함수를 통해서
 *  compaction을 할지 reclaim을 할지, 그냥 실패처리로 할지등을 정하는데
 *  정하는데, 그 기준값으로 사용하는게 이 변수이다. 
 *  order 할당 실패 상황에서 이 값이 0에 가까울수록 compact를 안하며,
 *  1000에 가까울수록 compact를 잘 수행한다.
 * - compaction_ready, __fragmentation_index함수 참고.
 */
int sysctl_extfrag_threshold = 500;

/**
 * try_to_compact_pages - Direct compact to satisfy a high-order allocation
 * @gfp_mask: The GFP mask of the current allocation
 * @order: The order of the current allocation
 * @alloc_flags: The allocation flags of the current allocation
 * @ac: The context of current allocation
 * @prio: Determines how hard direct compaction should try to succeed
 * @capture: Pointer to free page created by compaction will be stored here
 *
 * This is the main entry point for direct page compaction.
 */
/*
 * IAMROOT, 2022.03.19:
 * - zone을 iterate하면서 compact를 시도한다.
 *   @prio가 sync full일 경우 defer되거나 중지되는 경우 없이 무조건
 *   수행하고 아닌 경우에는 defer되거나 중지될수있다.
 */
enum compact_result try_to_compact_pages(gfp_t gfp_mask, unsigned int order,
		unsigned int alloc_flags, const struct alloc_context *ac,
		enum compact_priority prio, struct page **capture)
{
	int may_perform_io = gfp_mask & __GFP_IO;
	struct zoneref *z;
	struct zone *zone;
	enum compact_result rc = COMPACT_SKIPPED;

	/*
	 * Check if the GFP flags allow compaction - GFP_NOIO is really
	 * tricky context because the migration might require IO
	 */
/*
 * IAMROOT, 2022.03.19:
 * - __GFP_IO가 GFP_KERNEL에 붙고, GFP_ATOMIC에는 안붙는다. 즉 메모리 회수가
 *   가능한 상황에서 붙게 된다.
 */
	if (!may_perform_io)
		return COMPACT_SKIPPED;

	trace_mm_compaction_try_to_compact_pages(order, gfp_mask, prio);

	/* Compact each zone in the list */
/*
 * IAMROOT, 2022.03.25:
 * - @ac->zonelist는 preferred node소속 으로 선택되고, __GFP_THISNODE가
 *   있었다면 ZONELIST_NOFALLBACK, 아니면 ZONELIST_FALLBACK일것이다.
 * - full scan이 아니라면 compact를 defer 할지 확인하여 defer를 시킬수도
 *   있다.
 */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist,
					ac->highest_zoneidx, ac->nodemask) {
		enum compact_result status;

/*
 * IAMROOT, 2022.03.19:
 * - sync full일 경우 defer를 안할것이다.
 */
		if (prio > MIN_COMPACT_PRIORITY
					&& compaction_deferred(zone, order)) {
/*
 * IAMROOT, 2022.03.19:
 * - sync light나 async인 상황에서 compaction이 지연이 된다면 이전 rc 값을
 *   COMPACT_DEFERRED로 업데이트할지 결정하고 해당 zone을 skip한다.
 */
			rc = max_t(enum compact_result, COMPACT_DEFERRED, rc);
			continue;
		}

		status = compact_zone_order(zone, order, gfp_mask, prio,
				alloc_flags, ac->highest_zoneidx, capture);
		rc = max(status, rc);

		/* The allocation should succeed, stop compacting */
		if (status == COMPACT_SUCCESS) {
			/*
			 * We think the allocation will succeed in this zone,
			 * but it is not certain, hence the false. The caller
			 * will repeat this with true if allocation indeed
			 * succeeds in this zone.
			 */
			compaction_defer_reset(zone, order, false);

			break;
		}

/*
 * IAMROOT, 2022.03.19:
 * - sync full or sync light인 경우에 대해서 status를 확인해 defer_compaction
 *   을 수행한다.
 */
		if (prio != COMPACT_PRIO_ASYNC && (status == COMPACT_COMPLETE ||
					status == COMPACT_PARTIAL_SKIPPED))
			/*
			 * We think that allocation won't succeed in this zone
			 * so we defer compaction there. If it ends up
			 * succeeding after all, it will be reset.
			 */
			defer_compaction(zone, order);

		/*
		 * We might have stopped compacting due to need_resched() in
		 * async compaction, or due to a fatal signal detected. In that
		 * case do not try further zones
		 */
/*
 * IAMROOT, 2022.03.19:
 * - async이면서 shcedule이 필요한상황이거나 current task가 fatal signal을
 *   받았다면 중지한다.
 */
		if ((prio == COMPACT_PRIO_ASYNC && need_resched())
					|| fatal_signal_pending(current))
			break;
	}

	return rc;
}

/*
 * Compact all zones within a node till each zone's fragmentation score
 * reaches within proactive compaction thresholds (as determined by the
 * proactiveness tunable).
 *
 * It is possible that the function returns before reaching score targets
 * due to various back-off conditions, such as, contention on per-node or
 * per-zone locks.
 */
static void proactive_compact_node(pg_data_t *pgdat)
{
	int zoneid;
	struct zone *zone;
	struct compact_control cc = {
		.order = -1,
		.mode = MIGRATE_SYNC_LIGHT,
		.ignore_skip_hint = true,
		.whole_zone = true,
		.gfp_mask = GFP_KERNEL,
		.proactive_compaction = true,
	};

	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		cc.zone = zone;

		compact_zone(&cc, NULL);

		VM_BUG_ON(!list_empty(&cc.freepages));
		VM_BUG_ON(!list_empty(&cc.migratepages));
	}
}

/* Compact all zones within a node */
static void compact_node(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int zoneid;
	struct zone *zone;
	struct compact_control cc = {
		.order = -1,
		.mode = MIGRATE_SYNC,
		.ignore_skip_hint = true,
		.whole_zone = true,
		.gfp_mask = GFP_KERNEL,
	};


	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {

		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		cc.zone = zone;

		compact_zone(&cc, NULL);

		VM_BUG_ON(!list_empty(&cc.freepages));
		VM_BUG_ON(!list_empty(&cc.migratepages));
	}
}

/* Compact all nodes in the system */
static void compact_nodes(void)
{
	int nid;

	/* Flush pending updates to the LRU lists */
	lru_add_drain_all();

	for_each_online_node(nid)
		compact_node(nid);
}

/*
 * Tunable for proactive compaction. It determines how
 * aggressively the kernel should compact memory in the
 * background. It takes values in the range [0, 100].
 */
unsigned int __read_mostly sysctl_compaction_proactiveness = 20;

int compaction_proactiveness_sysctl_handler(struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int rc, nid;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	if (write && sysctl_compaction_proactiveness) {
		for_each_online_node(nid) {
			pg_data_t *pgdat = NODE_DATA(nid);

			if (pgdat->proactive_compact_trigger)
				continue;

			pgdat->proactive_compact_trigger = true;
			wake_up_interruptible(&pgdat->kcompactd_wait);
		}
	}

	return 0;
}

/*
 * This is the entry point for compacting all nodes via
 * /proc/sys/vm/compact_memory
 */
int sysctl_compaction_handler(struct ctl_table *table, int write,
			void *buffer, size_t *length, loff_t *ppos)
{
	if (write)
		compact_nodes();

	return 0;
}

#if defined(CONFIG_SYSFS) && defined(CONFIG_NUMA)
static ssize_t compact_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int nid = dev->id;

	if (nid >= 0 && nid < nr_node_ids && node_online(nid)) {
		/* Flush pending updates to the LRU lists */
		lru_add_drain_all();

		compact_node(nid);
	}

	return count;
}
static DEVICE_ATTR_WO(compact);

int compaction_register_node(struct node *node)
{
	return device_create_file(&node->dev, &dev_attr_compact);
}

void compaction_unregister_node(struct node *node)
{
	return device_remove_file(&node->dev, &dev_attr_compact);
}
#endif /* CONFIG_SYSFS && CONFIG_NUMA */

static inline bool kcompactd_work_requested(pg_data_t *pgdat)
{
	return pgdat->kcompactd_max_order > 0 || kthread_should_stop() ||
		pgdat->proactive_compact_trigger;
}

static bool kcompactd_node_suitable(pg_data_t *pgdat)
{
	int zoneid;
	struct zone *zone;
	enum zone_type highest_zoneidx = pgdat->kcompactd_highest_zoneidx;

	for (zoneid = 0; zoneid <= highest_zoneidx; zoneid++) {
		zone = &pgdat->node_zones[zoneid];

		if (!populated_zone(zone))
			continue;

		if (compaction_suitable(zone, pgdat->kcompactd_max_order, 0,
					highest_zoneidx) == COMPACT_CONTINUE)
			return true;
	}

	return false;
}

static void kcompactd_do_work(pg_data_t *pgdat)
{
	/*
	 * With no special task, compact all zones so that a page of requested
	 * order is allocatable.
	 */
	int zoneid;
	struct zone *zone;
	struct compact_control cc = {
		.order = pgdat->kcompactd_max_order,
		.search_order = pgdat->kcompactd_max_order,
		.highest_zoneidx = pgdat->kcompactd_highest_zoneidx,
		.mode = MIGRATE_SYNC_LIGHT,
		.ignore_skip_hint = false,
		.gfp_mask = GFP_KERNEL,
	};
	trace_mm_compaction_kcompactd_wake(pgdat->node_id, cc.order,
							cc.highest_zoneidx);
	count_compact_event(KCOMPACTD_WAKE);

	for (zoneid = 0; zoneid <= cc.highest_zoneidx; zoneid++) {
		int status;

		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		if (compaction_deferred(zone, cc.order))
			continue;

		if (compaction_suitable(zone, cc.order, 0, zoneid) !=
							COMPACT_CONTINUE)
			continue;

		if (kthread_should_stop())
			return;

		cc.zone = zone;
		status = compact_zone(&cc, NULL);

		if (status == COMPACT_SUCCESS) {
			compaction_defer_reset(zone, cc.order, false);
		} else if (status == COMPACT_PARTIAL_SKIPPED || status == COMPACT_COMPLETE) {
			/*
			 * Buddy pages may become stranded on pcps that could
			 * otherwise coalesce on the zone's free area for
			 * order >= cc.order.  This is ratelimited by the
			 * upcoming deferral.
			 */
			drain_all_pages(zone);

			/*
			 * We use sync migration mode here, so we defer like
			 * sync direct compaction does.
			 */
			defer_compaction(zone, cc.order);
		}

		count_compact_events(KCOMPACTD_MIGRATE_SCANNED,
				     cc.total_migrate_scanned);
		count_compact_events(KCOMPACTD_FREE_SCANNED,
				     cc.total_free_scanned);

		VM_BUG_ON(!list_empty(&cc.freepages));
		VM_BUG_ON(!list_empty(&cc.migratepages));
	}

	/*
	 * Regardless of success, we are done until woken up next. But remember
	 * the requested order/highest_zoneidx in case it was higher/tighter
	 * than our current ones
	 */
	if (pgdat->kcompactd_max_order <= cc.order)
		pgdat->kcompactd_max_order = 0;
	if (pgdat->kcompactd_highest_zoneidx >= cc.highest_zoneidx)
		pgdat->kcompactd_highest_zoneidx = pgdat->nr_zones - 1;
}

void wakeup_kcompactd(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	if (!order)
		return;

	if (pgdat->kcompactd_max_order < order)
		pgdat->kcompactd_max_order = order;

	if (pgdat->kcompactd_highest_zoneidx > highest_zoneidx)
		pgdat->kcompactd_highest_zoneidx = highest_zoneidx;

	/*
	 * Pairs with implicit barrier in wait_event_freezable()
	 * such that wakeups are not missed.
	 */
	if (!wq_has_sleeper(&pgdat->kcompactd_wait))
		return;

	if (!kcompactd_node_suitable(pgdat))
		return;

	trace_mm_compaction_wakeup_kcompactd(pgdat->node_id, order,
							highest_zoneidx);
	wake_up_interruptible(&pgdat->kcompactd_wait);
}

/*
 * The background compaction daemon, started as a kernel thread
 * from the init process.
 */
static int kcompactd(void *p)
{
	pg_data_t *pgdat = (pg_data_t *)p;
	struct task_struct *tsk = current;
	long default_timeout = msecs_to_jiffies(HPAGE_FRAG_CHECK_INTERVAL_MSEC);
	long timeout = default_timeout;

	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	set_freezable();

	pgdat->kcompactd_max_order = 0;
	pgdat->kcompactd_highest_zoneidx = pgdat->nr_zones - 1;

	while (!kthread_should_stop()) {
		unsigned long pflags;

		/*
		 * Avoid the unnecessary wakeup for proactive compaction
		 * when it is disabled.
		 */
		if (!sysctl_compaction_proactiveness)
			timeout = MAX_SCHEDULE_TIMEOUT;
		trace_mm_compaction_kcompactd_sleep(pgdat->node_id);
		if (wait_event_freezable_timeout(pgdat->kcompactd_wait,
			kcompactd_work_requested(pgdat), timeout) &&
			!pgdat->proactive_compact_trigger) {

			psi_memstall_enter(&pflags);
			kcompactd_do_work(pgdat);
			psi_memstall_leave(&pflags);
			/*
			 * Reset the timeout value. The defer timeout from
			 * proactive compaction is lost here but that is fine
			 * as the condition of the zone changing substantionally
			 * then carrying on with the previous defer interval is
			 * not useful.
			 */
			timeout = default_timeout;
			continue;
		}

		/*
		 * Start the proactive work with default timeout. Based
		 * on the fragmentation score, this timeout is updated.
		 */
		timeout = default_timeout;
		if (should_proactive_compact_node(pgdat)) {
			unsigned int prev_score, score;

			prev_score = fragmentation_score_node(pgdat);
			proactive_compact_node(pgdat);
			score = fragmentation_score_node(pgdat);
			/*
			 * Defer proactive compaction if the fragmentation
			 * score did not go down i.e. no progress made.
			 */
			if (unlikely(score >= prev_score))
				timeout =
				   default_timeout << COMPACT_MAX_DEFER_SHIFT;
		}
		if (unlikely(pgdat->proactive_compact_trigger))
			pgdat->proactive_compact_trigger = false;
	}

	return 0;
}

/*
 * This kcompactd start function will be called by init and node-hot-add.
 * On node-hot-add, kcompactd will moved to proper cpus if cpus are hot-added.
 */
int kcompactd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (pgdat->kcompactd)
		return 0;

	pgdat->kcompactd = kthread_run(kcompactd, pgdat, "kcompactd%d", nid);
	if (IS_ERR(pgdat->kcompactd)) {
		pr_err("Failed to start kcompactd on node %d\n", nid);
		ret = PTR_ERR(pgdat->kcompactd);
		pgdat->kcompactd = NULL;
	}
	return ret;
}

/*
 * Called by memory hotplug when all memory in a node is offlined. Caller must
 * hold mem_hotplug_begin/end().
 */
void kcompactd_stop(int nid)
{
	struct task_struct *kcompactd = NODE_DATA(nid)->kcompactd;

	if (kcompactd) {
		kthread_stop(kcompactd);
		NODE_DATA(nid)->kcompactd = NULL;
	}
}

/*
 * It's optimal to keep kcompactd on the same CPUs as their memory, but
 * not required for correctness. So if the last cpu in a node goes
 * away, we get changed to run anywhere: as the first one comes back,
 * restore their cpu bindings.
 */
static int kcompactd_cpu_online(unsigned int cpu)
{
	int nid;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);
		const struct cpumask *mask;

		mask = cpumask_of_node(pgdat->node_id);

		if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
			/* One of our CPUs online: restore mask */
			set_cpus_allowed_ptr(pgdat->kcompactd, mask);
	}
	return 0;
}

static int __init kcompactd_init(void)
{
	int nid;
	int ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"mm/compaction:online",
					kcompactd_cpu_online, NULL);
	if (ret < 0) {
		pr_err("kcompactd: failed to register hotplug callbacks.\n");
		return ret;
	}

	for_each_node_state(nid, N_MEMORY)
		kcompactd_run(nid);
	return 0;
}
subsys_initcall(kcompactd_init)

#endif /* CONFIG_COMPACTION */
