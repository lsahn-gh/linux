/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_COMPACTION_H
#define _LINUX_COMPACTION_H

/*
 * Determines how hard direct compaction should try to succeed.
 * Lower value means higher priority, analogically to reclaim priority.
 */
/*
 * IAMROOT, 2022.03.19:
 * - sync full  == min priority
 *   sync light == min costly priority, default priority
 *   async      == init
 * - direct compaction을 얼마나 hard하게 동작시킬지에 대한것.
 *   번호가 낮을수록 hard하다(우선순위가 높다).
 */
enum compact_priority {
	COMPACT_PRIO_SYNC_FULL,
	MIN_COMPACT_PRIORITY = COMPACT_PRIO_SYNC_FULL,
	COMPACT_PRIO_SYNC_LIGHT,
	MIN_COMPACT_COSTLY_PRIORITY = COMPACT_PRIO_SYNC_LIGHT,
	DEF_COMPACT_PRIORITY = COMPACT_PRIO_SYNC_LIGHT,
	COMPACT_PRIO_ASYNC,
	INIT_COMPACT_PRIORITY = COMPACT_PRIO_ASYNC
};

/* Return values for compact_zone() and try_to_compact_pages() */
/* When adding new states, please adjust include/trace/events/compaction.h */
enum compact_result {
	/* For more detailed tracepoint output - internal to compaction */
	COMPACT_NOT_SUITABLE_ZONE,
	/*
	 * compaction didn't start as it was not possible or direct reclaim
	 * was more suitable
	 */
	COMPACT_SKIPPED,
	/* compaction didn't start as it was deferred due to past failures */
	COMPACT_DEFERRED,

	/* For more detailed tracepoint output - internal to compaction */
	COMPACT_NO_SUITABLE_PAGE,
	/* compaction should continue to another pageblock */
	COMPACT_CONTINUE,

	/*
	 * The full zone was compacted scanned but wasn't successful to compact
	 * suitable pages.
	 */
	COMPACT_COMPLETE,
	/*
	 * direct compaction has scanned part of the zone but wasn't successful
	 * to compact suitable pages.
	 */
	COMPACT_PARTIAL_SKIPPED,

	/* compaction terminated prematurely due to lock contentions */
	COMPACT_CONTENDED,

	/*
	 * direct compaction terminated after concluding that the allocation
	 * should now succeed
	 */
	COMPACT_SUCCESS,
};

struct alloc_context; /* in mm/internal.h */

/*
 * Number of free order-0 pages that should be available above given watermark
 * to make sure compaction has reasonable chance of not running out of free
 * pages that it needs to isolate as migration target during its work.
 */
/*
 * IAMROOT, 2022.03.19:
 * 압축이 작업 중에 마이그레이션 대상으로 분리해야 하는 빈 페이지가 부족하지
 * 않도록 하기 위해 지정된 워터마크 위에 사용할 수 있는 free order 0 페이지 수다.
 *
 * - compaction 하는 동안에 추가로 필요로 하는 memory. order page 2개가 필요하다.
 */
static inline unsigned long compact_gap(unsigned int order)
{
	/*
	 * Although all the isolations for migration are temporary, compaction
	 * free scanner may have up to 1 << order pages on its list and then
	 * try to split an (order - 1) free page. At that point, a gap of
	 * 1 << order might not be enough, so it's safer to require twice that
	 * amount. Note that the number of pages on the list is also
	 * effectively limited by COMPACT_CLUSTER_MAX, as that's the maximum
	 * that the migrate scanner can have isolated on migrate list, and free
	 * scanner is only invoked when the number of isolated free pages is
	 * lower than that. But it's not worth to complicate the formula here
	 * as a bigger gap for higher orders than strictly necessary can also
	 * improve chances of compaction success.
	 */
	return 2UL << order;
}

#ifdef CONFIG_COMPACTION
extern unsigned int sysctl_compaction_proactiveness;
extern int sysctl_compaction_handler(struct ctl_table *table, int write,
			void *buffer, size_t *length, loff_t *ppos);
extern int compaction_proactiveness_sysctl_handler(struct ctl_table *table,
		int write, void *buffer, size_t *length, loff_t *ppos);
extern int sysctl_extfrag_threshold;
extern int sysctl_compact_unevictable_allowed;

extern unsigned int extfrag_for_order(struct zone *zone, unsigned int order);
extern int fragmentation_index(struct zone *zone, unsigned int order);
extern enum compact_result try_to_compact_pages(gfp_t gfp_mask,
		unsigned int order, unsigned int alloc_flags,
		const struct alloc_context *ac, enum compact_priority prio,
		struct page **page);
extern void reset_isolation_suitable(pg_data_t *pgdat);
extern enum compact_result compaction_suitable(struct zone *zone, int order,
		unsigned int alloc_flags, int highest_zoneidx);

extern void compaction_defer_reset(struct zone *zone, int order,
				bool alloc_success);

/* Compaction has made some progress and retrying makes sense */
static inline bool compaction_made_progress(enum compact_result result)
{
	/*
	 * Even though this might sound confusing this in fact tells us
	 * that the compaction successfully isolated and migrated some
	 * pageblocks.
	 */
	if (result == COMPACT_SUCCESS)
		return true;

	return false;
}

/* Compaction has failed and it doesn't make much sense to keep retrying. */
/*
 * IAMROOT, 2022.04.09:
 * - compaction이 실패했다면 return true, 아니면 false
 * - compaction이 complete다 == compaction 범위를 다해본상태
 *                           == compaction 이제 할게 없다.(실패다)
 */
static inline bool compaction_failed(enum compact_result result)
{
	/* All zones were scanned completely and still not result. */
	if (result == COMPACT_COMPLETE)
		return true;

	return false;
}

/* Compaction needs reclaim to be performed first, so it can continue. */
/*
 * IAMROOT, 2022.05.13:
 * - compact가 skipped이다 == compact를 못하는 상황이니 reclaim이 필요하다.
 */
static inline bool compaction_needs_reclaim(enum compact_result result)
{
	/*
	 * Compaction backed off due to watermark checks for order-0
	 * so the regular reclaim has to try harder and reclaim something.
	 */
	if (result == COMPACT_SKIPPED)
		return true;

	return false;
}

/*
 * Compaction has backed off for some reason after doing some work or none
 * at all. It might be throttling or lock contention. Retrying might be still
 * worthwhile, but with a higher priority if allowed.
 */
/*
 * IAMROOT, 2022.05.13:
 * - papago
 *   어떤 작업을 하거나 전혀 하지 않은 후 어떤 이유로 압축이 철회되었습니다.
 *   제한 또는 잠금 경합일 수 있습니다. 재시도할 수 있지만 허용될 경우
 *   우선 순위가 더 높습니다. 
 */
static inline bool compaction_withdrawn(enum compact_result result)
{
	/*
	 * If compaction is deferred for high-order allocations, it is
	 * because sync compaction recently failed. If this is the case
	 * and the caller requested a THP allocation, we do not want
	 * to heavily disrupt the system, so we fail the allocation
	 * instead of entering direct reclaim.
	 */
/*
 * IAMROOT, 2022.05.13:
 * - papago
 *   고차 할당에 대해 압축이 지연되는 경우 동기화 압축이 최근에 실패했기
 *   때문입니다. 이 경우 호출자가 THP 할당을 요청한 경우 시스템을 크게
 *   중단시키지 않기 때문에 직접 회수를 입력하는 대신 할당에 실패합니다.
 * - compaction_deferred(), try_to_compact_pages() 참고
 */
	if (result == COMPACT_DEFERRED)
		return true;

	/*
	 * If compaction in async mode encounters contention or blocks higher
	 * priority task we back off early rather than cause stalls.
	 */
/*
 * IAMROOT, 2022.05.13:
 * - papago
 *   비동기 모드의 압축이 경합에 부딪히거나 더 높은 우선 순위 작업을 차단하는
 *   경우 정지하지 않고 조기에 뒤로 물러난다.
 * - signal을 받았거나 lock try가 지연된 경우.(compact_lock_irqsave() 참고)
 */
	if (result == COMPACT_CONTENDED)
		return true;

	/*
	 * Page scanners have met but we haven't scanned full zones so this
	 * is a back off in fact.
	 */
/*
 * IAMROOT, 2022.05.13:
 * - papago
 *   페이지 스캐너는 충족되었지만 전체 영역을 스캔하지 않았기 때문에 이는 사실
 *   백오프입니다.
 * - __compact_finished() 참고
 */
	if (result == COMPACT_PARTIAL_SKIPPED)
		return true;

	return false;
}


bool compaction_zonelist_suitable(struct alloc_context *ac, int order,
					int alloc_flags);

extern int kcompactd_run(int nid);
extern void kcompactd_stop(int nid);
extern void wakeup_kcompactd(pg_data_t *pgdat, int order, int highest_zoneidx);

#else
static inline void reset_isolation_suitable(pg_data_t *pgdat)
{
}

static inline enum compact_result compaction_suitable(struct zone *zone, int order,
					int alloc_flags, int highest_zoneidx)
{
	return COMPACT_SKIPPED;
}

static inline bool compaction_made_progress(enum compact_result result)
{
	return false;
}

static inline bool compaction_failed(enum compact_result result)
{
	return false;
}

static inline bool compaction_needs_reclaim(enum compact_result result)
{
	return false;
}

static inline bool compaction_withdrawn(enum compact_result result)
{
	return true;
}

static inline int kcompactd_run(int nid)
{
	return 0;
}
static inline void kcompactd_stop(int nid)
{
}

static inline void wakeup_kcompactd(pg_data_t *pgdat,
				int order, int highest_zoneidx)
{
}

#endif /* CONFIG_COMPACTION */

struct node;
#if defined(CONFIG_COMPACTION) && defined(CONFIG_SYSFS) && defined(CONFIG_NUMA)
extern int compaction_register_node(struct node *node);
extern void compaction_unregister_node(struct node *node);

#else

static inline int compaction_register_node(struct node *node)
{
	return 0;
}

static inline void compaction_unregister_node(struct node *node)
{
}
#endif /* CONFIG_COMPACTION && CONFIG_SYSFS && CONFIG_NUMA */

#endif /* _LINUX_COMPACTION_H */
