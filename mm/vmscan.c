// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/migrate.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/pagevec.h>
#include <linux/prefetch.h>
#include <linux/printk.h>
#include <linux/dax.h>
#include <linux/psi.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>
#include <linux/balloon_compaction.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/vmscan.h>

struct scan_control {
	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	/*
	 * Scan pressure balancing between anon and file LRUs
	 */
	unsigned long	anon_cost;
	unsigned long	file_cost;

	/* Can active pages be deactivated as part of reclaim? */
#define DEACTIVATE_ANON 1
#define DEACTIVATE_FILE 2
/*
 * IAMROOT, 2022.04.23:
 * - active를 끌수 있다는 뜻. 0번 bit는 anon, 1번 비트는 file
 */
	unsigned int may_deactivate:2;
	unsigned int force_deactivate:1;
/*
 * IAMROOT, 2022.04.23:
 * - may_deactivate에 설정이 안되있어 deactive를 못했을때 set된다.
 *   (shrink_list 참고)
 */
	unsigned int skipped_deactivate:1;

	/* Writepage batching in laptop mode; RECLAIM_WRITE */
	unsigned int may_writepage:1;

	/* Can mapped pages be reclaimed? */
	unsigned int may_unmap:1;

	/* Can pages be swapped as part of reclaim? */
	unsigned int may_swap:1;

	/*
	 * Cgroup memory below memory.low is protected as long as we
	 * don't threaten to OOM. If any cgroup is reclaimed at
	 * reduced force or passed over entirely due to its memory.low
	 * setting (memcg_low_skipped), and nothing is reclaimed as a
	 * result, then go back for one more cycle that reclaims the protected
	 * memory (memcg_low_reclaim) to avert OOM.
	 */
	unsigned int memcg_low_reclaim:1;
	unsigned int memcg_low_skipped:1;

	unsigned int hibernation_mode:1;

	/* One of the zones is ready for compaction */
	unsigned int compaction_ready:1;

	/* There is easily reclaimable cold cache in the current node */
	unsigned int cache_trim_mode:1;

	/* The file pages on the current node are dangerously low */
	unsigned int file_is_tiny:1;

	/* Always discard instead of demoting to lower tier memory */
	unsigned int no_demotion:1;

	/* Allocation order */
	s8 order;

	/* Scan (total_size >> priority) pages at once */
	s8 priority;

	/* The highest zone to isolate pages for reclaim from */
	s8 reclaim_idx;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	struct {
		unsigned int dirty;
		unsigned int unqueued_dirty;
		unsigned int congested;
		unsigned int writeback;
		unsigned int immediate;
		unsigned int file_taken;
		unsigned int taken;
	} nr;

	/* for recording the reclaimed slab by now */
	struct reclaim_state reclaim_state;
};

#ifdef ARCH_HAS_PREFETCHW
/*
 * IAMROOT, 2022.04.23:
 * - page 구조체들은 flag등을 atomic으로 관리하는데 여러 core가 이런 atomic
 *   접근으로 경합상태에 빠질수 있으므로 미리 prefetch를 하여 경합상태를
 *   줄이려는 의도이다.
 */
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

/*
 * From 0 .. 200.  Higher means more swappy.
 */
int vm_swappiness = 60;

static void set_task_reclaim_state(struct task_struct *task,
				   struct reclaim_state *rs)
{
	/* Check for an overwrite */
	WARN_ON_ONCE(rs && task->reclaim_state);

	/* Check for the nulling of an already-nulled member */
	WARN_ON_ONCE(!rs && !task->reclaim_state);

	task->reclaim_state = rs;
}

static LIST_HEAD(shrinker_list);
static DECLARE_RWSEM(shrinker_rwsem);

#ifdef CONFIG_MEMCG
static int shrinker_nr_max;

/* The shrinker_info is expanded in a batch of BITS_PER_LONG */
static inline int shrinker_map_size(int nr_items)
{
	return (DIV_ROUND_UP(nr_items, BITS_PER_LONG) * sizeof(unsigned long));
}

static inline int shrinker_defer_size(int nr_items)
{
	return (round_up(nr_items, BITS_PER_LONG) * sizeof(atomic_long_t));
}

static struct shrinker_info *shrinker_info_protected(struct mem_cgroup *memcg,
						     int nid)
{
	return rcu_dereference_protected(memcg->nodeinfo[nid]->shrinker_info,
					 lockdep_is_held(&shrinker_rwsem));
}

static int expand_one_shrinker_info(struct mem_cgroup *memcg,
				    int map_size, int defer_size,
				    int old_map_size, int old_defer_size)
{
	struct shrinker_info *new, *old;
	struct mem_cgroup_per_node *pn;
	int nid;
	int size = map_size + defer_size;

	for_each_node(nid) {
		pn = memcg->nodeinfo[nid];
		old = shrinker_info_protected(memcg, nid);
		/* Not yet online memcg */
		if (!old)
			return 0;

		new = kvmalloc_node(sizeof(*new) + size, GFP_KERNEL, nid);
		if (!new)
			return -ENOMEM;

		new->nr_deferred = (atomic_long_t *)(new + 1);
		new->map = (void *)new->nr_deferred + defer_size;

		/* map: set all old bits, clear all new bits */
		memset(new->map, (int)0xff, old_map_size);
		memset((void *)new->map + old_map_size, 0, map_size - old_map_size);
		/* nr_deferred: copy old values, clear all new values */
		memcpy(new->nr_deferred, old->nr_deferred, old_defer_size);
		memset((void *)new->nr_deferred + old_defer_size, 0,
		       defer_size - old_defer_size);

		rcu_assign_pointer(pn->shrinker_info, new);
		kvfree_rcu(old, rcu);
	}

	return 0;
}

void free_shrinker_info(struct mem_cgroup *memcg)
{
	struct mem_cgroup_per_node *pn;
	struct shrinker_info *info;
	int nid;

	for_each_node(nid) {
		pn = memcg->nodeinfo[nid];
		info = rcu_dereference_protected(pn->shrinker_info, true);
		kvfree(info);
		rcu_assign_pointer(pn->shrinker_info, NULL);
	}
}

int alloc_shrinker_info(struct mem_cgroup *memcg)
{
	struct shrinker_info *info;
	int nid, size, ret = 0;
	int map_size, defer_size = 0;

	down_write(&shrinker_rwsem);
	map_size = shrinker_map_size(shrinker_nr_max);
	defer_size = shrinker_defer_size(shrinker_nr_max);
	size = map_size + defer_size;
	for_each_node(nid) {
		info = kvzalloc_node(sizeof(*info) + size, GFP_KERNEL, nid);
		if (!info) {
			free_shrinker_info(memcg);
			ret = -ENOMEM;
			break;
		}
		info->nr_deferred = (atomic_long_t *)(info + 1);
		info->map = (void *)info->nr_deferred + defer_size;
		rcu_assign_pointer(memcg->nodeinfo[nid]->shrinker_info, info);
	}
	up_write(&shrinker_rwsem);

	return ret;
}

static inline bool need_expand(int nr_max)
{
	return round_up(nr_max, BITS_PER_LONG) >
	       round_up(shrinker_nr_max, BITS_PER_LONG);
}

static int expand_shrinker_info(int new_id)
{
	int ret = 0;
	int new_nr_max = new_id + 1;
	int map_size, defer_size = 0;
	int old_map_size, old_defer_size = 0;
	struct mem_cgroup *memcg;

	if (!need_expand(new_nr_max))
		goto out;

	if (!root_mem_cgroup)
		goto out;

	lockdep_assert_held(&shrinker_rwsem);

	map_size = shrinker_map_size(new_nr_max);
	defer_size = shrinker_defer_size(new_nr_max);
	old_map_size = shrinker_map_size(shrinker_nr_max);
	old_defer_size = shrinker_defer_size(shrinker_nr_max);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		ret = expand_one_shrinker_info(memcg, map_size, defer_size,
					       old_map_size, old_defer_size);
		if (ret) {
			mem_cgroup_iter_break(NULL, memcg);
			goto out;
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);
out:
	if (!ret)
		shrinker_nr_max = new_nr_max;

	return ret;
}

void set_shrinker_bit(struct mem_cgroup *memcg, int nid, int shrinker_id)
{
	if (shrinker_id >= 0 && memcg && !mem_cgroup_is_root(memcg)) {
		struct shrinker_info *info;

		rcu_read_lock();
		info = rcu_dereference(memcg->nodeinfo[nid]->shrinker_info);
		/* Pairs with smp mb in shrink_slab() */
		smp_mb__before_atomic();
		set_bit(shrinker_id, info->map);
		rcu_read_unlock();
	}
}

static DEFINE_IDR(shrinker_idr);

static int prealloc_memcg_shrinker(struct shrinker *shrinker)
{
	int id, ret = -ENOMEM;

	if (mem_cgroup_disabled())
		return -ENOSYS;

	down_write(&shrinker_rwsem);
	/* This may call shrinker, so it must use down_read_trylock() */
	id = idr_alloc(&shrinker_idr, shrinker, 0, 0, GFP_KERNEL);
	if (id < 0)
		goto unlock;

	if (id >= shrinker_nr_max) {
		if (expand_shrinker_info(id)) {
			idr_remove(&shrinker_idr, id);
			goto unlock;
		}
	}
	shrinker->id = id;
	ret = 0;
unlock:
	up_write(&shrinker_rwsem);
	return ret;
}

static void unregister_memcg_shrinker(struct shrinker *shrinker)
{
	int id = shrinker->id;

	BUG_ON(id < 0);

	lockdep_assert_held(&shrinker_rwsem);

	idr_remove(&shrinker_idr, id);
}

static long xchg_nr_deferred_memcg(int nid, struct shrinker *shrinker,
				   struct mem_cgroup *memcg)
{
	struct shrinker_info *info;

	info = shrinker_info_protected(memcg, nid);
	return atomic_long_xchg(&info->nr_deferred[shrinker->id], 0);
}

static long add_nr_deferred_memcg(long nr, int nid, struct shrinker *shrinker,
				  struct mem_cgroup *memcg)
{
	struct shrinker_info *info;

	info = shrinker_info_protected(memcg, nid);
	return atomic_long_add_return(nr, &info->nr_deferred[shrinker->id]);
}

void reparent_shrinker_deferred(struct mem_cgroup *memcg)
{
	int i, nid;
	long nr;
	struct mem_cgroup *parent;
	struct shrinker_info *child_info, *parent_info;

	parent = parent_mem_cgroup(memcg);
	if (!parent)
		parent = root_mem_cgroup;

	/* Prevent from concurrent shrinker_info expand */
	down_read(&shrinker_rwsem);
	for_each_node(nid) {
		child_info = shrinker_info_protected(memcg, nid);
		parent_info = shrinker_info_protected(parent, nid);
		for (i = 0; i < shrinker_nr_max; i++) {
			nr = atomic_long_read(&child_info->nr_deferred[i]);
			atomic_long_add(nr, &parent_info->nr_deferred[i]);
		}
	}
	up_read(&shrinker_rwsem);
}

static bool cgroup_reclaim(struct scan_control *sc)
{
	return sc->target_mem_cgroup;
}

/**
 * writeback_throttling_sane - is the usual dirty throttling mechanism available?
 * @sc: scan_control in question
 *
 * The normal page dirty throttling mechanism in balance_dirty_pages() is
 * completely broken with the legacy memcg and direct stalling in
 * shrink_page_list() is used for throttling instead, which lacks all the
 * niceties such as fairness, adaptive pausing, bandwidth proportional
 * allocation and configurability.
 *
 * This function tests whether the vmscan currently in progress can assume
 * that the normal dirty throttling mechanism is operational.
 */
/*
 * IAMROOT, 2022.04.23:
 * - normal dirty throttling mechanism이 적용되는 상태인지 확인한다.
 */
static bool writeback_throttling_sane(struct scan_control *sc)
{
	if (!cgroup_reclaim(sc))
		return true;
#ifdef CONFIG_CGROUP_WRITEBACK
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return true;
#endif
	return false;
}
#else
static int prealloc_memcg_shrinker(struct shrinker *shrinker)
{
	return -ENOSYS;
}

static void unregister_memcg_shrinker(struct shrinker *shrinker)
{
}

static long xchg_nr_deferred_memcg(int nid, struct shrinker *shrinker,
				   struct mem_cgroup *memcg)
{
	return 0;
}

static long add_nr_deferred_memcg(long nr, int nid, struct shrinker *shrinker,
				  struct mem_cgroup *memcg)
{
	return 0;
}

static bool cgroup_reclaim(struct scan_control *sc)
{
	return false;
}

static bool writeback_throttling_sane(struct scan_control *sc)
{
	return true;
}
#endif

static long xchg_nr_deferred(struct shrinker *shrinker,
			     struct shrink_control *sc)
{
	int nid = sc->nid;

	if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
		nid = 0;

	if (sc->memcg &&
	    (shrinker->flags & SHRINKER_MEMCG_AWARE))
		return xchg_nr_deferred_memcg(nid, shrinker,
					      sc->memcg);

	return atomic_long_xchg(&shrinker->nr_deferred[nid], 0);
}


static long add_nr_deferred(long nr, struct shrinker *shrinker,
			    struct shrink_control *sc)
{
	int nid = sc->nid;

	if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
		nid = 0;

	if (sc->memcg &&
	    (shrinker->flags & SHRINKER_MEMCG_AWARE))
		return add_nr_deferred_memcg(nr, nid, shrinker,
					     sc->memcg);

	return atomic_long_add_return(nr, &shrinker->nr_deferred[nid]);
}

/*
 * IAMROOT, 2022.04.16:
 * - 현재 node에서 reclaim이 불가능할경우 system이나 상위 요청에서 demotion을
 *   허락했는지 확인하고, 다음 demotion node가 있는지 확인한다.
 */
static bool can_demote(int nid, struct scan_control *sc)
{
	if (!numa_demotion_enabled)
		return false;
	if (sc) {
		if (sc->no_demotion)
			return false;
		/* It is pointless to do demotion in memcg reclaim */
		if (cgroup_reclaim(sc))
			return false;
	}
	if (next_demotion_node(nid) == NUMA_NO_NODE)
		return false;

	return true;
}

/*
 * IAMROOT, 2022.04.16:
 * - swap memory가 존재하거나 다른 demotion node가 존재하는지 확인한다.
 */
static inline bool can_reclaim_anon_pages(struct mem_cgroup *memcg,
					  int nid,
					  struct scan_control *sc)
{
/*
 * IAMROOT, 2022.04.16:
 * - 연결되있는 cgroup이 없으면 system에서 swap 설정이 되있고,
 *   swap pages가 존재하는지 확인한다.
 *   그게 아니면 연결되잇는 cgroup을 통해서 현재 task가 가져올수있는
 *   남은 swap page가 존재하는지 확인한다.
 */
	if (memcg == NULL) {
		/*
		 * For non-memcg reclaim, is there
		 * space in any swap device?
		 */
		if (get_nr_swap_pages() > 0)
			return true;
	} else {
		/* Is the memcg below its swap limit? */
		if (mem_cgroup_get_nr_swap_pages(memcg) > 0)
			return true;
	}

/*
 * IAMROOT, 2022.04.16:
 * - 남은 swap page가 없는 경우 
 */
	/*
	 * The page can not be swapped.
	 *
	 * Can it be reclaimed from this node via demotion?
	 */
	return can_demote(nid, sc);
}

/*
 * This misses isolated pages which are not accounted for to save counters.
 * As the data only determines if reclaim or compaction continues, it is
 * not expected that isolated pages will be a dominating factor.
 */
/*
 * IAMROOT, 2022.04.16:
 * - @zone에서 page 회수가 가능한 개수를 추정한다.
 * 1) 잔여 swap 공간이 없는 경우 
 *	NR_ZONE_INACTIVE_FILE + NR_ZONE_ACTIVE_FILE
 * 2) 잔여 swap 공간이 있는 경우
 *	NR_ZONE_INACTIVE_FILE + NR_ZONE_ACTIVE_FILE +
 *	NR_ZONE_INACTIVE_ANON + NR_ZONE_ACTIVE_ANON
 */
unsigned long zone_reclaimable_pages(struct zone *zone)
{
	unsigned long nr;

	nr = zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_FILE) +
		zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_FILE);
	if (can_reclaim_anon_pages(NULL, zone_to_nid(zone), NULL))
		nr += zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_ANON) +
			zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_ANON);

	return nr;
}

/**
 * lruvec_lru_size -  Returns the number of pages on the given LRU list.
 * @lruvec: lru vector
 * @lru: lru to use
 * @zone_idx: zones to consider (use MAX_NR_ZONES for the whole LRU list)
 */
/*
 * IAMROOT, 2022.04.23:
 * - @lru에 등록되있는 page 수를 zone_idx까지 합산하여 return한다.
 */
static unsigned long lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru,
				     int zone_idx)
{
	unsigned long size = 0;
	int zid;

	for (zid = 0; zid <= zone_idx && zid < MAX_NR_ZONES; zid++) {
		struct zone *zone = &lruvec_pgdat(lruvec)->node_zones[zid];

		if (!managed_zone(zone))
			continue;

		if (!mem_cgroup_disabled())
			size += mem_cgroup_get_zone_lru_size(lruvec, lru, zid);
		else
			size += zone_page_state(zone, NR_ZONE_LRU_BASE + lru);
	}
	return size;
}

/*
 * Add a shrinker callback to be called from the vm.
 */
int prealloc_shrinker(struct shrinker *shrinker)
{
	unsigned int size;
	int err;

	if (shrinker->flags & SHRINKER_MEMCG_AWARE) {
		err = prealloc_memcg_shrinker(shrinker);
		if (err != -ENOSYS)
			return err;

		shrinker->flags &= ~SHRINKER_MEMCG_AWARE;
	}

	size = sizeof(*shrinker->nr_deferred);
	if (shrinker->flags & SHRINKER_NUMA_AWARE)
		size *= nr_node_ids;

	shrinker->nr_deferred = kzalloc(size, GFP_KERNEL);
	if (!shrinker->nr_deferred)
		return -ENOMEM;

	return 0;
}

void free_prealloced_shrinker(struct shrinker *shrinker)
{
	if (shrinker->flags & SHRINKER_MEMCG_AWARE) {
		down_write(&shrinker_rwsem);
		unregister_memcg_shrinker(shrinker);
		up_write(&shrinker_rwsem);
		return;
	}

	kfree(shrinker->nr_deferred);
	shrinker->nr_deferred = NULL;
}

void register_shrinker_prepared(struct shrinker *shrinker)
{
	down_write(&shrinker_rwsem);
	list_add_tail(&shrinker->list, &shrinker_list);
	shrinker->flags |= SHRINKER_REGISTERED;
	up_write(&shrinker_rwsem);
}

int register_shrinker(struct shrinker *shrinker)
{
	int err = prealloc_shrinker(shrinker);

	if (err)
		return err;
	register_shrinker_prepared(shrinker);
	return 0;
}
EXPORT_SYMBOL(register_shrinker);

/*
 * Remove one
 */
void unregister_shrinker(struct shrinker *shrinker)
{
	if (!(shrinker->flags & SHRINKER_REGISTERED))
		return;

	down_write(&shrinker_rwsem);
	list_del(&shrinker->list);
	shrinker->flags &= ~SHRINKER_REGISTERED;
	if (shrinker->flags & SHRINKER_MEMCG_AWARE)
		unregister_memcg_shrinker(shrinker);
	up_write(&shrinker_rwsem);

	kfree(shrinker->nr_deferred);
	shrinker->nr_deferred = NULL;
}
EXPORT_SYMBOL(unregister_shrinker);

#define SHRINK_BATCH 128

static unsigned long do_shrink_slab(struct shrink_control *shrinkctl,
				    struct shrinker *shrinker, int priority)
{
	unsigned long freed = 0;
	unsigned long long delta;
	long total_scan;
	long freeable;
	long nr;
	long new_nr;
	long batch_size = shrinker->batch ? shrinker->batch
					  : SHRINK_BATCH;
	long scanned = 0, next_deferred;

	freeable = shrinker->count_objects(shrinker, shrinkctl);
	if (freeable == 0 || freeable == SHRINK_EMPTY)
		return freeable;

	/*
	 * copy the current shrinker scan count into a local variable
	 * and zero it so that other concurrent shrinker invocations
	 * don't also do this scanning work.
	 */
	nr = xchg_nr_deferred(shrinker, shrinkctl);

	if (shrinker->seeks) {
		delta = freeable >> priority;
		delta *= 4;
		do_div(delta, shrinker->seeks);
	} else {
		/*
		 * These objects don't require any IO to create. Trim
		 * them aggressively under memory pressure to keep
		 * them from causing refetches in the IO caches.
		 */
		delta = freeable / 2;
	}

	total_scan = nr >> priority;
	total_scan += delta;
	total_scan = min(total_scan, (2 * freeable));

	trace_mm_shrink_slab_start(shrinker, shrinkctl, nr,
				   freeable, delta, total_scan, priority);

	/*
	 * Normally, we should not scan less than batch_size objects in one
	 * pass to avoid too frequent shrinker calls, but if the slab has less
	 * than batch_size objects in total and we are really tight on memory,
	 * we will try to reclaim all available objects, otherwise we can end
	 * up failing allocations although there are plenty of reclaimable
	 * objects spread over several slabs with usage less than the
	 * batch_size.
	 *
	 * We detect the "tight on memory" situations by looking at the total
	 * number of objects we want to scan (total_scan). If it is greater
	 * than the total number of objects on slab (freeable), we must be
	 * scanning at high prio and therefore should try to reclaim as much as
	 * possible.
	 */
	while (total_scan >= batch_size ||
	       total_scan >= freeable) {
		unsigned long ret;
		unsigned long nr_to_scan = min(batch_size, total_scan);

		shrinkctl->nr_to_scan = nr_to_scan;
		shrinkctl->nr_scanned = nr_to_scan;
		ret = shrinker->scan_objects(shrinker, shrinkctl);
		if (ret == SHRINK_STOP)
			break;
		freed += ret;

		count_vm_events(SLABS_SCANNED, shrinkctl->nr_scanned);
		total_scan -= shrinkctl->nr_scanned;
		scanned += shrinkctl->nr_scanned;

		cond_resched();
	}

	/*
	 * The deferred work is increased by any new work (delta) that wasn't
	 * done, decreased by old deferred work that was done now.
	 *
	 * And it is capped to two times of the freeable items.
	 */
	next_deferred = max_t(long, (nr + delta - scanned), 0);
	next_deferred = min(next_deferred, (2 * freeable));

	/*
	 * move the unused scan count back into the shrinker in a
	 * manner that handles concurrent updates.
	 */
	new_nr = add_nr_deferred(next_deferred, shrinker, shrinkctl);

	trace_mm_shrink_slab_end(shrinker, shrinkctl->nid, freed, nr, new_nr, total_scan);
	return freed;
}

#ifdef CONFIG_MEMCG
static unsigned long shrink_slab_memcg(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	struct shrinker_info *info;
	unsigned long ret, freed = 0;
	int i;

	if (!mem_cgroup_online(memcg))
		return 0;

	if (!down_read_trylock(&shrinker_rwsem))
		return 0;

	info = shrinker_info_protected(memcg, nid);
	if (unlikely(!info))
		goto unlock;

	for_each_set_bit(i, info->map, shrinker_nr_max) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
			.nid = nid,
			.memcg = memcg,
		};
		struct shrinker *shrinker;

		shrinker = idr_find(&shrinker_idr, i);
		if (unlikely(!shrinker || !(shrinker->flags & SHRINKER_REGISTERED))) {
			if (!shrinker)
				clear_bit(i, info->map);
			continue;
		}

		/* Call non-slab shrinkers even though kmem is disabled */
		if (!memcg_kmem_enabled() &&
		    !(shrinker->flags & SHRINKER_NONSLAB))
			continue;

		ret = do_shrink_slab(&sc, shrinker, priority);
		if (ret == SHRINK_EMPTY) {
			clear_bit(i, info->map);
			/*
			 * After the shrinker reported that it had no objects to
			 * free, but before we cleared the corresponding bit in
			 * the memcg shrinker map, a new object might have been
			 * added. To make sure, we have the bit set in this
			 * case, we invoke the shrinker one more time and reset
			 * the bit if it reports that it is not empty anymore.
			 * The memory barrier here pairs with the barrier in
			 * set_shrinker_bit():
			 *
			 * list_lru_add()     shrink_slab_memcg()
			 *   list_add_tail()    clear_bit()
			 *   <MB>               <MB>
			 *   set_bit()          do_shrink_slab()
			 */
			smp_mb__after_atomic();
			ret = do_shrink_slab(&sc, shrinker, priority);
			if (ret == SHRINK_EMPTY)
				ret = 0;
			else
				set_shrinker_bit(memcg, nid, i);
		}
		freed += ret;

		if (rwsem_is_contended(&shrinker_rwsem)) {
			freed = freed ? : 1;
			break;
		}
	}
unlock:
	up_read(&shrinker_rwsem);
	return freed;
}
#else /* CONFIG_MEMCG */
static unsigned long shrink_slab_memcg(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	return 0;
}
#endif /* CONFIG_MEMCG */

/**
 * shrink_slab - shrink slab caches
 * @gfp_mask: allocation context
 * @nid: node whose slab caches to target
 * @memcg: memory cgroup whose slab caches to target
 * @priority: the reclaim priority
 *
 * Call the shrink functions to age shrinkable caches.
 *
 * @nid is passed along to shrinkers with SHRINKER_NUMA_AWARE set,
 * unaware shrinkers will receive a node id of 0 instead.
 *
 * @memcg specifies the memory cgroup to target. Unaware shrinkers
 * are called only if it is the root cgroup.
 *
 * @priority is sc->priority, we take the number of objects and >> by priority
 * in order to get the scan target.
 *
 * Returns the number of reclaimed slab objects.
 */
/*
 * IAMROOT, 2022.05.07:
 * - TODO
 */
static unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 int priority)
{
	unsigned long ret, freed = 0;
	struct shrinker *shrinker;

	/*
	 * The root memcg might be allocated even though memcg is disabled
	 * via "cgroup_disable=memory" boot parameter.  This could make
	 * mem_cgroup_is_root() return false, then just run memcg slab
	 * shrink, but skip global shrink.  This may result in premature
	 * oom.
	 */
	if (!mem_cgroup_disabled() && !mem_cgroup_is_root(memcg))
		return shrink_slab_memcg(gfp_mask, nid, memcg, priority);

	if (!down_read_trylock(&shrinker_rwsem))
		goto out;

	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
			.nid = nid,
			.memcg = memcg,
		};

		ret = do_shrink_slab(&sc, shrinker, priority);
		if (ret == SHRINK_EMPTY)
			ret = 0;
		freed += ret;
		/*
		 * Bail out if someone want to register a new shrinker to
		 * prevent the registration from being stalled for long periods
		 * by parallel ongoing shrinking.
		 */
		if (rwsem_is_contended(&shrinker_rwsem)) {
			freed = freed ? : 1;
			break;
		}
	}

	up_read(&shrinker_rwsem);
out:
	cond_resched();
	return freed;
}

void drop_slab_node(int nid)
{
	unsigned long freed;
	int shift = 0;

	do {
		struct mem_cgroup *memcg = NULL;

		if (fatal_signal_pending(current))
			return;

		freed = 0;
		memcg = mem_cgroup_iter(NULL, NULL, NULL);
		do {
			freed += shrink_slab(GFP_KERNEL, nid, memcg, 0);
		} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);
	} while ((freed >> shift++) > 1);
}

void drop_slab(void)
{
	int nid;

	for_each_online_node(nid)
		drop_slab_node(nid);
}

/*
 * IAMROOT, 2022.04.30:
 * - @page가 freeable page cache page인지 확인힌다. @page는 isolate되어있을
 *   것이고 page cache와 optional buffer를 page->private에 가지고있을것이다.
 * - buffer head를 가지고 있지 않은 free 가능한 page cache인지 확인한다.
 * - page_cache_pins = 1 or 512
 *   page_count(page) - (0 or 1) = 1 + (1 or 512)
 * - ex) page_count == 2, buffer head를 안가지고 있음.
 *   2 - (0) = 1 + (1)
 *
 * --- page cache가 있을 경우
 *  page ref | page_has_private | page ref 해석
 *  2        | false            | 1(page isolate) + 1(page cache)
 *  3        | true             | 1(page isolate) + 1(page cache) + 1(private)
 *  513      | false            | 512(huse page isolate) + 1(page cache)
 *  514      | true             | 512(huse page isolate) + 1(page cache) +
 *                              | 1(private)
 *
 *  - private를 얻을때 get ref도 하기때문에 private를 가지고있으면 ref가 없을때보다
 *  1높아야된다.(attach_page_private(), detach_page_private() 참고)
 */
static inline int is_page_cache_freeable(struct page *page)
{
	/*
	 * A freeable page cache page is referenced only by the caller
	 * that isolated the page, the page cache and optional buffer
	 * heads at page->private.
	 */
/*
 * IAMROOT, 2022.04.30:
 *  isolate page에 대한 refcnt는 다음과 같이 예상한다.
 *
 *  page_count(page) = (page isolate ref) + (page cache ref) +
 *                     (private ref)
 *                   = thr_nr_pages(page) + 1 + page_has_private(page)
 */
	int page_cache_pins = thp_nr_pages(page);
	return page_count(page) - page_has_private(page) == 1 + page_cache_pins;
}

static int may_write_to_inode(struct inode *inode)
{
	if (current->flags & PF_SWAPWRITE)
		return 1;
	if (!inode_write_congested(inode))
		return 1;
	if (inode_to_bdi(inode) == current->backing_dev_info)
		return 1;
	return 0;
}

/*
 * We detected a synchronous write error writing a page out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the page and once
 * that page is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping lock_page() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct page *page, int error)
{
	lock_page(page);
	if (page_mapping(page) == mapping)
		mapping_set_error(mapping, error);
	unlock_page(page);
}

/* possible outcome of pageout() */
typedef enum {
	/* failed to write page out, page is locked */
	PAGE_KEEP,
	/* move page to the active list, page is locked */
	PAGE_ACTIVATE,
	/* page has been sent to the disk successfully, page is unlocked */
	PAGE_SUCCESS,
	/* page is clean and locked */
	PAGE_CLEAN,
} pageout_t;

/*
 * pageout is called by shrink_page_list() for each dirty page.
 * Calls ->writepage().
 */

/*
 * IAMROOT, 2022.04.30:
 * - disk에 write를 수행한다.
 *   dirty page면 write수행후 success, dirty가 아니면 clean return.
 */
static pageout_t pageout(struct page *page, struct address_space *mapping)
{
	/*
	 * If the page is dirty, only perform writeback if that write
	 * will be non-blocking.  To prevent this allocation from being
	 * stalled by pagecache activity.  But note that there may be
	 * stalls if we need to run get_block().  We could test
	 * PagePrivate for that.
	 *
	 * If this process is currently in __generic_file_write_iter() against
	 * this page's queue, we can perform writeback even if that
	 * will block.
	 *
	 * If the page is swapcache, write it back even if that would
	 * block, for some throttling. This happens by accident, because
	 * swap_backing_dev_info is bust: it doesn't reflect the
	 * congestion state of the swapdevs.  Easy to fix, if needed.
	 */
	if (!is_page_cache_freeable(page))
		return PAGE_KEEP;
	if (!mapping) {
		/*
		 * Some data journaling orphaned pages can have
		 * page->mapping == NULL while being dirty with clean buffers.
		 */
		if (page_has_private(page)) {
			if (try_to_free_buffers(page)) {
				ClearPageDirty(page);
				pr_info("%s: orphaned page\n", __func__);
				return PAGE_CLEAN;
			}
		}
		return PAGE_KEEP;
	}
	if (mapping->a_ops->writepage == NULL)
		return PAGE_ACTIVATE;
	if (!may_write_to_inode(mapping->host))
		return PAGE_KEEP;

/*
 * IAMROOT, 2022.04.30:
 * - dirty set이면 reclaim설정후 write를 수행한다.
 */
	if (clear_page_dirty_for_io(page)) {
		int res;
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.for_reclaim = 1,
		};

		SetPageReclaim(page);
		res = mapping->a_ops->writepage(page, &wbc);
		if (res < 0)
			handle_write_error(mapping, page, res);
		if (res == AOP_WRITEPAGE_ACTIVATE) {
			ClearPageReclaim(page);
			return PAGE_ACTIVATE;
		}

/*
 * IAMROOT, 2022.04.30:
 * - write가 끝나고 나올수도있고(sync), 그냥 요청 or 진행상태로 나올수도(async)있다.
 */
		if (!PageWriteback(page)) {
			/* synchronous write or broken a_ops? */
			ClearPageReclaim(page);
		}
		trace_mm_vmscan_writepage(page);
		inc_node_page_state(page, NR_VMSCAN_WRITE);
		return PAGE_SUCCESS;
	}

	return PAGE_CLEAN;
}

/*
 * Same as remove_mapping, but if the page is removed from the mapping, it
 * gets returned with a refcount of 0.
 */
/*
 * IAMROOT, 2022.04.30:
 * - reclaim등의 이유로 inactive page들이 disk에 쓰여지면서 page cache(shadow)에
 *   관련정보를 기록한다.
 */
static int __remove_mapping(struct address_space *mapping, struct page *page,
			    bool reclaimed, struct mem_cgroup *target_memcg)
{
	int refcount;
	void *shadow = NULL;

	BUG_ON(!PageLocked(page));
	BUG_ON(mapping != page_mapping(page));

	xa_lock_irq(&mapping->i_pages);
	/*
	 * The non racy check for a busy page.
	 *
	 * Must be careful with the order of the tests. When someone has
	 * a ref to the page, it may be possible that they dirty it then
	 * drop the reference. So if PageDirty is tested before page_count
	 * here, then the following race may occur:
	 *
	 * get_user_pages(&page);
	 * [user mapping goes away]
	 * write_to(page);
	 *				!PageDirty(page)    [good]
	 * SetPageDirty(page);
	 * put_page(page);
	 *				!page_count(page)   [good, discard it]
	 *
	 * [oops, our write_to data is lost]
	 *
	 * Reversing the order of the tests ensures such a situation cannot
	 * escape unnoticed. The smp_rmb is needed to ensure the page->flags
	 * load is not satisfied before that of page->_refcount.
	 *
	 * Note that if SetPageDirty is always performed via set_page_dirty,
	 * and thus under the i_pages lock, then this ordering is not required.
	 */
	refcount = 1 + compound_nr(page);
	if (!page_ref_freeze(page, refcount))
		goto cannot_free;
	/* note: atomic_cmpxchg in page_ref_freeze provides the smp_rmb */
	if (unlikely(PageDirty(page))) {
		page_ref_unfreeze(page, refcount);
		goto cannot_free;
	}

/*
 * IAMROOT, 2022.04.30:
 * - anon page
 */
	if (PageSwapCache(page)) {
		swp_entry_t swap = { .val = page_private(page) };
		mem_cgroup_swapout(page, swap);
/*
 * IAMROOT, 2022.04.30:
 * - reclaim 중에 들어왔고, mapping이 안되있다면 shadow를 구해서 기록한다.
 */
		if (reclaimed && !mapping_exiting(mapping))
			shadow = workingset_eviction(page, target_memcg);
/*
 * IAMROOT, 2022.04.30:
 * - disk에 다옮겼으니 @page의 xarray에 swap_cache를 삭제하고 그 공간에 shadow를
 *   넣는다.
 */
		__delete_from_swap_cache(page, swap, shadow);
		xa_unlock_irq(&mapping->i_pages);
		put_swap_page(page, swap);
	} else {

/*
 * IAMROOT, 2022.04.30:
 * - file page
 */
		void (*freepage)(struct page *);

		freepage = mapping->a_ops->freepage;
		/*
		 * Remember a shadow entry for reclaimed file cache in
		 * order to detect refaults, thus thrashing, later on.
		 *
		 * But don't store shadows in an address space that is
		 * already exiting.  This is not just an optimization,
		 * inode reclaim needs to empty out the radix tree or
		 * the nodes are lost.  Don't plant shadows behind its
		 * back.
		 *
		 * We also don't store shadows for DAX mappings because the
		 * only page cache pages found in these are zero pages
		 * covering holes, and because we don't want to mix DAX
		 * exceptional entries and shadow exceptional entries in the
		 * same address_space.
		 */
/*
 * IAMROOT, 2022.04.30:
 * - anon_page와 비슷한 처리를 한다.
 * - DAX는 disk에 비해 빠르므로 workingset 작업을 안한다. 즉 refault란
 *   개념은 없고 fault만 있게 된다.
 */
		if (reclaimed && page_is_file_lru(page) &&
		    !mapping_exiting(mapping) && !dax_mapping(mapping))
			shadow = workingset_eviction(page, target_memcg);
		__delete_from_page_cache(page, shadow);
		xa_unlock_irq(&mapping->i_pages);

		if (freepage != NULL)
			freepage(page);
	}

	return 1;

cannot_free:
	xa_unlock_irq(&mapping->i_pages);
	return 0;
}

/*
 * Attempt to detach a locked page from its ->mapping.  If it is dirty or if
 * someone else has a ref on the page, abort and return 0.  If it was
 * successfully detached, return 1.  Assumes the caller has a single ref on
 * this page.
 */
int remove_mapping(struct address_space *mapping, struct page *page)
{
	if (__remove_mapping(mapping, page, false, NULL)) {
		/*
		 * Unfreezing the refcount with 1 rather than 2 effectively
		 * drops the pagecache ref for us without requiring another
		 * atomic operation.
		 */
		page_ref_unfreeze(page, 1);
		return 1;
	}
	return 0;
}

/**
 * putback_lru_page - put previously isolated page onto appropriate LRU list
 * @page: page to be put back to appropriate lru list
 *
 * Add previously isolated @page to appropriate LRU list.
 * Page may still be unevictable for other reasons.
 *
 * lru_lock must not be held, interrupts must be enabled.
 */
/*
 * IAMROOT, 2022.04.02:
 * - @page를 lru list로 돌려보낸다. 그리고 isolate ref를 drop시켜준다.
 */
void putback_lru_page(struct page *page)
{
	lru_cache_add(page);

/*
 * IAMROOT, 2022.04.02:
 * - isolate에 대한 ref drop.
 */
	put_page(page);		/* drop ref from isolate */
}

enum page_references {
	PAGEREF_RECLAIM,
	PAGEREF_RECLAIM_CLEAN,
	PAGEREF_KEEP,
	PAGEREF_ACTIVATE,
};

/*
 * IAMROOT, 2022.04.23:
 * - PAGEREF_ACTIVATE(실제 참조가 일어난 상황.)
 *   set PG_referenced를 수행한다.
 *   1. 참조 pte가 2개 이상인 상황.
 *    => 여러군데서 참조하고있으니 reclaim, inactive 안하겠다는것.
 *   2. 참조 pte가 1개이면서 이전에 PG_referenced가 존재했던상황. 
 *   3. 참조 pte가 1개이면서 실행가능 code
 *
 * - PAGEREF_KEEP(실제 참조가 일어난 상황.)
 *   set PG_referenced를 수행한다.
 *   참조 pte가 1 and unreference and not exec code.
 *    => inactive면 inactive로, active면 active로 keep 한다는것.
 *
 * - PAGEREF_RECLAIM_CLEAN
 *   clear PG_referenced를 수행한다.
 *   참조 pte가 0, reference, exec file or clean anon page 이면.
 *   => 참조가 없는 실행 code나 dirty된적이 없는 anon page.
 *
 * - PAGEREF_RECLAIM
 *   1. VM_LOCKED
 *   2. unreferenced, pte 0, pte == 1 and not exec file
 *
 * ---
 *
 */
static enum page_references page_check_references(struct page *page,
						  struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

/*
 * IAMROOT, 2022.04.30:
 * - referenced_ptes
 *   af가 set된 pte들(실제 참조가 일어난) 개수.
 * - referenced_pages
 *   PG_referenced set여부
 */
	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	referenced_page = TestClearPageReferenced(page);

	/*
	 * Mlock lost the isolation race with us.  Let try_to_unmap()
	 * move the page to the unevictable list.
	 */
	if (vm_flags & VM_LOCKED)
		return PAGEREF_RECLAIM;

	if (referenced_ptes) {
		/*
		 * All mapped pages start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file page is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated pages as well
		 * so that recently deactivated but used pages are
		 * quickly recovered.
		 */
		SetPageReferenced(page);

		if (referenced_page || referenced_ptes > 1)
			return PAGEREF_ACTIVATE;

		/*
		 * Activate file-backed executable pages after first usage.
		 */
		if ((vm_flags & VM_EXEC) && !PageSwapBacked(page))
			return PAGEREF_ACTIVATE;

		return PAGEREF_KEEP;
	}

	/* Reclaim if clean, defer dirty pages to writeback */
	if (referenced_page && !PageSwapBacked(page))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
}

/* Check if a page is dirty or under writeback */
/*
 * IAMROOT, 2022.04.23:
 * - @page가 anon page인 경우 dirty, writeback은 false.
 *   아닌 경우 @page의 dirty, writeback flag를 먼저 읽는다.
 *   그 후에 fs에 mapping되 있다면 is_dirty_writeback ops로 결정한다.
 */
static void page_check_dirty_writeback(struct page *page,
				       bool *dirty, bool *writeback)
{
	struct address_space *mapping;

	/*
	 * Anonymous pages are not handled by flushers and must be written
	 * from reclaim context. Do not stall reclaim based on them
	 */
/*
 * IAMROOT, 2022.04.23:
 * - anon page인지 확인한다. anon page이면 dirty, writeback을 하고 있지
 *   않을것이므로 false로 처리한다.
 * - !page_is_file_lru에서 대부분의 anon page가 찾아지고, anon page 임에도
 *   page_is_file_lru로 true가 나오는 조건에선(clean anon page)
 *   (PageAnon(page) && !PageSwapBacked(page)) 로 clean anon page임을 확인한다.
 */
	if (!page_is_file_lru(page) ||
	    (PageAnon(page) && !PageSwapBacked(page))) {
		*dirty = false;
		*writeback = false;
		return;
	}
/*
 * IAMROOT, 2022.05.03:
 * ------------------------ file page --------------------------------------
 */
	/* By default assume that the page flags are accurate */
	*dirty = PageDirty(page);
	*writeback = PageWriteback(page);

/*
 * IAMROOT, 2022.04.23:
 * - buffer head가 없는 경우 return.
 */
	/* Verify dirty/writeback state if the filesystem supports it */
	if (!page_has_private(page))
		return;

/*
 * IAMROOT, 2022.04.23:
 * - 참고함수
 *   nfs_check_dirty_writeback
 */
	mapping = page_mapping(page);
	if (mapping && mapping->a_ops->is_dirty_writeback)
		mapping->a_ops->is_dirty_writeback(page, dirty, writeback);
}

static struct page *alloc_demote_page(struct page *page, unsigned long node)
{
	struct migration_target_control mtc = {
		/*
		 * Allocate from 'node', or fail quickly and quietly.
		 * When this happens, 'page' will likely just be discarded
		 * instead of migrated.
		 */
		.gfp_mask = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) |
			    __GFP_THISNODE  | __GFP_NOWARN |
			    __GFP_NOMEMALLOC | GFP_NOWAIT,
		.nid = node
	};

	return alloc_migration_target(page, (unsigned long)&mtc);
}

/*
 * Take pages on @demote_list and attempt to demote them to
 * another node.  Pages which are not demoted are left on
 * @demote_pages.
 */
static unsigned int demote_page_list(struct list_head *demote_pages,
				     struct pglist_data *pgdat)
{
	int target_nid = next_demotion_node(pgdat->node_id);
	unsigned int nr_succeeded;
	int err;

	if (list_empty(demote_pages))
		return 0;

	if (target_nid == NUMA_NO_NODE)
		return 0;

	/* Demotion ignores all cpuset and mempolicy settings */
	err = migrate_pages(demote_pages, alloc_demote_page, NULL,
			    target_nid, MIGRATE_ASYNC, MR_DEMOTION,
			    &nr_succeeded);

	if (current_is_kswapd())
		__count_vm_events(PGDEMOTE_KSWAPD, nr_succeeded);
	else
		__count_vm_events(PGDEMOTE_DIRECT, nr_succeeded);

	return nr_succeeded;
}

/*
 * shrink_page_list() returns the number of reclaimed pages
 */
/*
 * IAMROOT, 2022.04.30:
 * - reclaim을 한다.
 * @page_list[in/out] in : recalim 대상 isolate된 pages.
 *                    out : keep or activate된 pages
 */
static unsigned int shrink_page_list(struct list_head *page_list,
				     struct pglist_data *pgdat,
				     struct scan_control *sc,
				     struct reclaim_stat *stat,
				     bool ignore_references)
{
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);
	LIST_HEAD(demote_pages);
	unsigned int nr_reclaimed = 0;
	unsigned int pgactivate = 0;
	bool do_demote_pass;

	memset(stat, 0, sizeof(*stat));
	cond_resched();
/*
 * IAMROOT, 2022.05.04:
 * - demote를 skip하겠다는게아니라 demote로 pass하겠다는뜻.
 * -
 */
	do_demote_pass = can_demote(pgdat->node_id, sc);

retry:
	while (!list_empty(page_list)) {
		struct address_space *mapping;
		struct page *page;
		enum page_references references = PAGEREF_RECLAIM;
		bool dirty, writeback, may_enter_fs;
		unsigned int nr_pages;

		cond_resched();

		page = lru_to_page(page_list);
		list_del(&page->lru);

/*
 * IAMROOT, 2022.04.23:
 * - lock일 실패했으면 ret_pages에 넣어놓고 유보시키고 다 끝난후에 @page_list
 *   에 되돌린다.
 */
		if (!trylock_page(page))
			goto keep;
/*
 * IAMROOT, 2022.05.03:
 * -------- 남겨진 page 상태 -------------
 * locked
 */
		VM_BUG_ON_PAGE(PageActive(page), page);

		nr_pages = compound_nr(page);

		/* Account the number of base pages even though THP */
		sc->nr_scanned += nr_pages;

/*
 * IAMROOT, 2022.04.23:
 * - active로 되돌린다.
 */
		if (unlikely(!page_evictable(page)))
			goto activate_locked;

/*
 * IAMROOT, 2022.05.03:
 * -------- 남겨진 page 상태 -------------
 * locked, evicatable
 */
/*
 * IAMROOT, 2022.04.23:
 * - 요청(@sc)에서 unmap disable 했는데 page가 mapped상태일 경우.
 *   pass한다.
 */
		if (!sc->may_unmap && page_mapped(page))
			goto keep_locked;

		may_enter_fs = (sc->gfp_mask & __GFP_FS) ||
			(PageSwapCache(page) && (sc->gfp_mask & __GFP_IO));

		/*
		 * The number of dirty pages determines if a node is marked
		 * reclaim_congested which affects wait_iff_congested. kswapd
		 * will stall and start writing pages if the tail of the LRU
		 * is all dirty unqueued pages.
		 */
		page_check_dirty_writeback(page, &dirty, &writeback);
		if (dirty || writeback)
			stat->nr_dirty++;

		if (dirty && !writeback)
			stat->nr_unqueued_dirty++;

		/*
		 * Treat this page as congested if the underlying BDI is or if
		 * pages are cycling through the LRU so quickly that the
		 * pages marked for immediate reclaim are making it to the
		 * end of the LRU a second time.
		 */
/*
 * IAMROOT, 2022.04.23:
 * - 이미 이 page를 다른 쪽에서 reclaim하고 있는 상태면 nr_congested로 누적.
 *   => 너무 빨리 reclaim중이라는것.
 * - dirty or writeback인데 BDI(backing devcie info)가 congested인 경우.
 */
		mapping = page_mapping(page);
		if (((dirty || writeback) && mapping &&
		     inode_write_congested(mapping->host)) ||
		    (writeback && PageReclaim(page)))
			stat->nr_congested++;

		/*
		 * If a page at the tail of the LRU is under writeback, there
		 * are three cases to consider.
		 *
		 * 1) If reclaim is encountering an excessive number of pages
		 *    under writeback and this page is both under writeback and
		 *    PageReclaim then it indicates that pages are being queued
		 *    for IO but are being recycled through the LRU before the
		 *    IO can complete. Waiting on the page itself risks an
		 *    indefinite stall if it is impossible to writeback the
		 *    page due to IO error or disconnected storage so instead
		 *    note that the LRU is being scanned too quickly and the
		 *    caller can stall after page list has been processed.
		 *
		 * 2) Global or new memcg reclaim encounters a page that is
		 *    not marked for immediate reclaim, or the caller does not
		 *    have __GFP_FS (or __GFP_IO if it's simply going to swap,
		 *    not to fs). In this case mark the page for immediate
		 *    reclaim and continue scanning.
		 *
		 *    Require may_enter_fs because we would wait on fs, which
		 *    may not have submitted IO yet. And the loop driver might
		 *    enter reclaim, and deadlock if it waits on a page for
		 *    which it is needed to do the write (loop masks off
		 *    __GFP_IO|__GFP_FS for this reason); but more thought
		 *    would probably show more reasons.
		 *
		 * 3) Legacy memcg encounters a page that is already marked
		 *    PageReclaim. memcg does not have any dirty pages
		 *    throttling so we could easily OOM just because too many
		 *    pages are in writeback and there is nothing else to
		 *    reclaim. Wait for the writeback to complete.
		 *
		 * In cases 1) and 2) we activate the pages to get them out of
		 * the way while we continue scanning for clean pages on the
		 * inactive list and refilling from the active list. The
		 * observation here is that waiting for disk writes is more
		 * expensive than potentially causing reloads down the line.
		 * Since they're marked for immediate reclaim, they won't put
		 * memory pressure on the cache working set any longer than it
		 * takes to write them to disk.
		 */
/*
 * IAMROOT, 2022.04.23:
 * - writeback이라면 case 3가지의 처리 방법이 있다.
 */
		if (PageWriteback(page)) {
			/* Case 1 above */
/*
 * IAMROOT, 2022.04.23:
 * - papgo
 *   1) 회수할 때 writeback 아래에 있는 페이지 수가 너무 많고 이 페이지가
 *   wrtieback 및 페이지 회수 아래에 있으면 페이지가 IO를 위해 대기 중이지만
 *   IO가 완료되기 전에 LRU를 통해 재활용되고 있음을 나타냅니다.
 *   IO 오류나 스토리지 연결 끊김으로 인해 페이지를 다시 쓸 수 없는 경우
 *   페이지에서 대기하는 것 자체가 무기한 정지될 위험이 있으므로 대신 LRU가
 *   너무 빨리 스캔되고 페이지 목록이 처리된 후 호출자가 정지할 수 있습니다.
 *
 * - papgo
 *   사례 1)과 2)에서는 inactive list의 clean pages계속 검색하고 active list에서
 *   다시 refill 하는동안 페이지를 활성화하여 방해가 되지 않도록 합니다.
 *   여기서 관찰할 수 있는 것은 디스크 쓰기를 기다리는 것이 잠재적으로 재로드의
 *   원인이 되는 것보다 더 비싸다는 것입니다.
 *   즉시 회수하도록 표시되므로 디스크에 쓰는 데 걸리는 시간보다 더 이상
 *   캐시 작업 세트에 메모리 부담을 주지 않습니다.
 *
 * - kswapd에서 실행되었고 해당 page가 이미 reclaim중이며 해당 node가 writeback
 *   상태라면(many pages writeback) nr_immediate를 증가시키고 activate_locked로
 *   이동한다. 즉 node가 바쁜데 reclaim예약된 page가 한번더 온경우 그냥 activate
 *   해놓는다는것.
 */
			if (current_is_kswapd() &&
			    PageReclaim(page) &&
			    test_bit(PGDAT_WRITEBACK, &pgdat->flags)) {
				stat->nr_immediate++;
				goto activate_locked;

			/* Case 2 above */
			} else if (writeback_throttling_sane(sc) ||
			    !PageReclaim(page) || !may_enter_fs) {
/*
 * IAMROOT, 2022.04.23:
 * - reclaim 대상이라는것만 표시해놓고 activate_locked로 돌아간다.
 */
				/*
				 * This is slightly racy - end_page_writeback()
				 * might have just cleared PageReclaim, then
				 * setting PageReclaim here end up interpreted
				 * as PageReadahead - but that does not matter
				 * enough to care.  What we do want is for this
				 * page to have PageReclaim set next time memcg
				 * reclaim reaches the tests above, so it will
				 * then wait_on_page_writeback() to avoid OOM;
				 * and it's also appropriate in global reclaim.
				 */
				SetPageReclaim(page);
				stat->nr_writeback++;
				goto activate_locked;

			/* Case 3 above */
			} else {
/*
 * IAMROOT, 2022.05.03:
 * - writeback이 완료될때까지 기다린다. 
 *   dirty page throttle 조절기능이 없고, 현재 reclaim중일 것으로 예상되는 page
 *   들인 경우 이 else문으로 진입하게 된다. throttle기능이 없으므로 너무많은
 *   page가 writeback이 된경우 OOM이 동작할수있어 기다린다는것 같다.
 */
				unlock_page(page);
				wait_on_page_writeback(page);
				/* then go back and try same page again */
				list_add_tail(&page->lru, page_list);
				continue;
			}
		}

/*
 * IAMROOT, 2022.05.03:
 * -------- 남겨진 page 상태 -------------
 * locked, evicatable, !writeback
 */
/*
 * IAMROOT, 2022.04.30:
 * - 참조 ptes개수와 PG_referenced를 조사해서 page를 어떻게 할지 결정한다.
 */
		if (!ignore_references)
			references = page_check_references(page, sc);

		switch (references) {
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			stat->nr_ref_keep += nr_pages;
			goto keep_locked;
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim the page below */
		}

		/*
		 * Before reclaiming the page, try to relocate
		 * its contents to another node.
		 */
/*
 * IAMROOT, 2022.04.30:
 * - PASS
 */
		if (do_demote_pass &&
		    (thp_migration_supported() || !PageTransHuge(page))) {
			list_add(&page->lru, &demote_pages);
			unlock_page(page);
			continue;
		}

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 * Lazyfree page could be freed directly
		 */
/*
 * IAMROOT, 2022.04.30:
 * - backing store를 가진 anon page.
 */
		if (PageAnon(page) && PageSwapBacked(page)) {
/*
 * IAMROOT, 2022.05.04:
 * - swapcache가 없는것은 swap cache에 기록을 해야한다.
 */
			if (!PageSwapCache(page)) {
/*
 * IAMROOT, 2022.05.04:
 * - swap요청이 없다. kepp.
 */
				if (!(sc->gfp_mask & __GFP_IO))
					goto keep_locked;
/*
 * IAMROOT, 2022.05.04:
 * - dma pinned는 안한다.
 */
				if (page_maybe_dma_pinned(page))
					goto keep_locked;
				if (PageTransHuge(page)) {
					/* cannot split THP, skip it */
					if (!can_split_huge_page(page, NULL))
						goto activate_locked;
					/*
					 * Split pages without a PMD map right
					 * away. Chances are some or all of the
					 * tail pages can be freed without IO.
					 */
					if (!compound_mapcount(page) &&
					    split_huge_page_to_list(page,
								    page_list))
						goto activate_locked;
				}
/*
 * IAMROOT, 2022.04.30:
 * - swap cache에 page 정보 추가시도.
 */
				if (!add_to_swap(page)) {
/*
 * IAMROOT, 2022.04.30:
 * - swap이 실패한경우. thp가 아니면 scan개수를 경감시키고 activate시킨다.
 */
					if (!PageTransHuge(page))
						goto activate_locked_split;
					/* Fallback to swap normal pages */
					if (split_huge_page_to_list(page,
								    page_list))
						goto activate_locked;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
					count_vm_event(THP_SWPOUT_FALLBACK);
#endif
					if (!add_to_swap(page))
						goto activate_locked_split;
				}

				may_enter_fs = true;

				/* Adding to swap updated mapping */
				mapping = page_mapping(page);
			}
		} else if (unlikely(PageTransHuge(page))) {
			/* Split file THP */
			if (split_huge_page_to_list(page, page_list))
				goto keep_locked;
		}

/*
 * IAMROOT, 2022.05.03:
 * -------- 남겨진 page 상태 -------------
 * 공통 : locked, evicatable, !writeback
 * anon && swapback : swap cache 기록 성공.
 * (!swapback || !anon) && thp : split 실패
 */
		/*
		 * THP may get split above, need minus tail pages and update
		 * nr_pages to avoid accounting tail pages twice.
		 *
		 * The tail pages that are added into swap cache successfully
		 * reach here.
		 */
		if ((nr_pages > 1) && !PageTransHuge(page)) {
			sc->nr_scanned -= (nr_pages - 1);
			nr_pages = 1;
		}

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (page_mapped(page)) {
			enum ttu_flags flags = TTU_BATCH_FLUSH;
			bool was_swapbacked = PageSwapBacked(page);

			if (unlikely(PageTransHuge(page)))
				flags |= TTU_SPLIT_HUGE_PMD;

			try_to_unmap(page, flags);
			if (page_mapped(page)) {
				stat->nr_unmap_fail += nr_pages;
				if (!was_swapbacked && PageSwapBacked(page))
					stat->nr_lazyfree_fail += nr_pages;
				goto activate_locked;
			}
		}
/*
 * IAMROOT, 2022.05.04:
 * -------- 남겨진 page 상태 -------------
 * 공통 : locked, evicatable, !writeback, unmap
 * anon && swapback : swap cache 기록 성공.
 * (!swapback || !anon) && thp : split 실패
 */
/*
 * IAMROOT, 2022.04.30:
 * - 여기부턴 unmap file, swap요청 성공한 anon page(clean anon은 당연히 swap안함)만
 *   존재(swap요청 성공한 anon page는 dirty set 상태.).
 */
		if (PageDirty(page)) {
			/*
			 * Only kswapd can writeback filesystem pages
			 * to avoid risk of stack overflow. But avoid
			 * injecting inefficient single-page IO into
			 * flusher writeback as much as possible: only
			 * write pages when we've encountered many
			 * dirty pages, and when we've already scanned
			 * the rest of the LRU for clean pages and see
			 * the same dirty pages again (PageReclaim).
			 */
/*
 * IAMROOT, 2022.04.30:
 * dirty page여도 최대한 write를 다음에 미루기 위한 조건 검사를 한다.
 * file에 대해서는 kswapd가 reclaim중 writeback을 할 수 있는 상태일때
 * 2번이상 reclaim인 page에 대해서만 writebakc을 수행한다.
 *
 * - page_is_file_lru
 *   clean anon page or file page인지를 먼저 확인한다. 
 *
 * - !current_is_kswapd()
 *   kswapd만 writeback시에 stackoverflow risk를 피할수있다. 즉 kswapd만
 *   writebac을 수행하며 reclaimer인경우 PageReclaim만 set하고
 *   activate시켜놓는다.
 *
 * - !PageReclaim(page)
 *   lru를 두번 순회할때까진 기다린다는것. PageReclaim을 set하고 다음 루틴에서도
 *   dirty이면 page write를 수행하겠다는것이다.
 *
 * - !test_bit(PGDAT_DIRTY, &pgdat->flags)
 *   kswapd가 reclaim중에 writeback을 할수있는 상태에 대한 확인. write할게
 *   많거나 하는 등일때 한번에 수행한다.
 *
 * ---
 *  reclaim중 dirty page write를 미루는 여러 이유
 *
 * 1. write를 한번 미뤄본다.
 * write 할 page가 메모리 너무 여기저기(inode, bdi등의 차이점에 따른)에
 * 분포하고 있을 경우 write가 엄청나게 오래걸리고 이는 reclaim완료가 늦는다는
 * 것이기 때문이다.
 *
 * 2. back store가 다른 page를 write하고 있을 수 있다.
 * reclaimer에서의 write요청이 다른 page write를(flushers) 중단하고 reclaim한
 * page를 먼저 write 시킬수있다. 그런데 만약 reclaim page가 single order page만
 * 여러개있으면 엄청 오래걸릴수잇을것이다. (reclaim때문에 다른 write가 취소되는
 * 비용 + single order를 여기저기 wrtie하는 비용) 이 경우 reclaim을 한번
 * 지연 시키고 다른 clean page를 찾는게 낫다.
 *
 * 3. kswapd가 writeback의 stackoverflow 회피 기능이 있다.
 */
			if (page_is_file_lru(page) &&
			    (!current_is_kswapd() || !PageReclaim(page) ||
			     !test_bit(PGDAT_DIRTY, &pgdat->flags))) {
				/*
				 * Immediately reclaim when written back.
				 * Similar in principal to deactivate_page()
				 * except we already have the page isolated
				 * and know it's dirty
				 */
				inc_node_page_state(page, NR_VMSCAN_IMMEDIATE);
				SetPageReclaim(page);

				goto activate_locked;
			}

			if (references == PAGEREF_RECLAIM_CLEAN)
				goto keep_locked;
/*
 * IAMROOT, 2022.05.04:
 * - fs가 아니거나 위에서 write 요청을 안했다면 keep
 */
			if (!may_enter_fs)
				goto keep_locked;
			if (!sc->may_writepage)
				goto keep_locked;

			/*
			 * Page is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after IO
			 * starts and then write it out here.
			 */
			try_to_unmap_flush_dirty();
/*
 * IAMROOT, 2022.04.30:
 * - dirty page를 back store(disk)에 기록한다.
 */
			switch (pageout(page, mapping)) {
			case PAGE_KEEP:
				goto keep_locked;
			case PAGE_ACTIVATE:
				goto activate_locked;
			case PAGE_SUCCESS:
				stat->nr_pageout += thp_nr_pages(page);

				if (PageWriteback(page))
					goto keep;
				if (PageDirty(page))
					goto keep;

/*
 * IAMROOT, 2022.04.30:
 * - async였으면 위 writeback이나 dirty가 아직 set됫을수도있다. 위에 두 bit가
 *   없다는건 완전히 완료했다는것을 예상하고 이는 sync write일것이다.
 */
				/*
				 * A synchronous write - probably a ramdisk.  Go
				 * ahead and try to reclaim the page.
				 */
				if (!trylock_page(page))
					goto keep;
				if (PageDirty(page) || PageWriteback(page))
					goto keep_locked;
				mapping = page_mapping(page);
				fallthrough;
			case PAGE_CLEAN:
				; /* try to free the page below */
			}
		}
		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 *
		 * We do this even if the page is PageDirty().
		 * try_to_release_page() does not perform I/O, but it is
		 * possible for a page to have PageDirty set, but it is actually
		 * clean (all its buffers are clean).  This happens if the
		 * buffers were written out directly, with submit_bh(). ext3
		 * will do this, as well as the blockdev mapping.
		 * try_to_release_page() will discover that cleanness and will
		 * drop the buffers and mark the page clean - it can be freed.
		 *
		 * Rarely, pages can have buffers and no ->mapping.  These are
		 * the pages which were not successfully invalidated in
		 * truncate_cleanup_page().  We try to drop those buffers here
		 * and if that worked, and the page is no longer mapped into
		 * process address space (page_count == 1) it can be freed.
		 * Otherwise, leave the page on the LRU so it is swappable.
		 */

		if (page_has_private(page)) {
			if (!try_to_release_page(page, sc->gfp_mask))
				goto activate_locked;
/*
 * IAMROOT, 2022.04.30:
 * - page를 사용하고 있는 user가 있는지 한번검사. 없으면 free.
 */
			if (!mapping && page_count(page) == 1) {
				unlock_page(page);
				if (put_page_testzero(page))
					goto free_it;
				else {
					/*
					 * rare race with speculative reference.
					 * the speculative reference will free
					 * this page shortly, so we may
					 * increment nr_reclaimed here (and
					 * leave it off the LRU).
					 */
					nr_reclaimed++;
					continue;
				}
			}
		}
/*
 * IAMROOT, 2022.04.30:
 * - 여기까지 왔으면 clean page들만이 존재.
 * - clean anon page. 할게 딱히 없으니 바로 풀어버린다.
 */
		if (PageAnon(page) && !PageSwapBacked(page)) {
			/* follow __remove_mapping for reference */
			if (!page_ref_freeze(page, 1))
				goto keep_locked;
			/*
			 * The page has only one reference left, which is
			 * from the isolation. After the caller puts the
			 * page back on lru and drops the reference, the
			 * page will be freed anyway. It doesn't matter
			 * which lru it goes. So we don't bother checking
			 * PageDirty here.
			 */
			count_vm_event(PGLAZYFREED);
			count_memcg_page_event(page, PGLAZYFREED);
/*
 * IAMROOT, 2022.04.30:
 * - clean page들은 mapping만 remove시켜주면된다.
 */
		} else if (!mapping || !__remove_mapping(mapping, page, true,
							 sc->target_mem_cgroup))
			goto keep_locked;

		unlock_page(page);
free_it:
		/*
		 * THP may get swapped out in a whole, need account
		 * all base pages.
		 */
		nr_reclaimed += nr_pages;

		/*
		 * Is there need to periodically free_page_list? It would
		 * appear not as the counts should be low
		 */
		if (unlikely(PageTransHuge(page)))
			destroy_compound_page(page);
		else
			list_add(&page->lru, &free_pages);
		continue;

activate_locked_split:
		/*
		 * The tail pages that are failed to add into swap cache
		 * reach here.  Fixup nr_scanned and nr_pages.
		 */
		if (nr_pages > 1) {
			sc->nr_scanned -= (nr_pages - 1);
			nr_pages = 1;
		}
activate_locked:
/*
 * IAMROOT, 2022.04.23:
 * - recalim을 하지 않고, activation으로 돌려야되는 경우.
 */
		/* Not a candidate for swapping, so reclaim swap space. */
		if (PageSwapCache(page) && (mem_cgroup_swap_full(page) ||
						PageMlocked(page)))
			try_to_free_swap(page);
		VM_BUG_ON_PAGE(PageActive(page), page);
		if (!PageMlocked(page)) {
			int type = page_is_file_lru(page);
			SetPageActive(page);
			stat->nr_activate[type] += nr_pages;
			count_memcg_page_event(page, PGACTIVATE);
		}
keep_locked:
		unlock_page(page);
keep:
		list_add(&page->lru, &ret_pages);
		VM_BUG_ON_PAGE(PageLRU(page) || PageUnevictable(page), page);
	}
	/* 'page_list' is always empty here */

	/* Migrate pages selected for demotion */
/*
 * IAMROOT, 2022.05.04:
 * - demote page들은 demote next node에서 migrate 한다.
 */
	nr_reclaimed += demote_page_list(&demote_pages, pgdat);
	/* Pages that could not be demoted are still in @demote_pages */
	if (!list_empty(&demote_pages)) {
		/* Pages which failed to demoted go back on @page_list for retry: */
		list_splice_init(&demote_pages, page_list);
		do_demote_pass = false;
		goto retry;
	}

/*
 * IAMROOT, 2022.04.30:
 * - anon activate + file activate
 */
	pgactivate = stat->nr_activate[0] + stat->nr_activate[1];

	mem_cgroup_uncharge_list(&free_pages);
	try_to_unmap_flush();
	free_unref_page_list(&free_pages);

/*
 * IAMROOT, 2022.04.30:
 * - reclaim못한것들은 되돌려 놓는다.
 */
	list_splice(&ret_pages, page_list);
	count_vm_events(PGACTIVATE, pgactivate);

	return nr_reclaimed;
}

unsigned int reclaim_clean_pages_from_list(struct zone *zone,
					    struct list_head *page_list)
{
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_unmap = 1,
	};
	struct reclaim_stat stat;
	unsigned int nr_reclaimed;
	struct page *page, *next;
	LIST_HEAD(clean_pages);
	unsigned int noreclaim_flag;

	list_for_each_entry_safe(page, next, page_list, lru) {
		if (!PageHuge(page) && page_is_file_lru(page) &&
		    !PageDirty(page) && !__PageMovable(page) &&
		    !PageUnevictable(page)) {
			ClearPageActive(page);
			list_move(&page->lru, &clean_pages);
		}
	}

	/*
	 * We should be safe here since we are only dealing with file pages and
	 * we are not kswapd and therefore cannot write dirty file pages. But
	 * call memalloc_noreclaim_save() anyway, just in case these conditions
	 * change in the future.
	 */
	noreclaim_flag = memalloc_noreclaim_save();
	nr_reclaimed = shrink_page_list(&clean_pages, zone->zone_pgdat, &sc,
					&stat, true);
	memalloc_noreclaim_restore(noreclaim_flag);

	list_splice(&clean_pages, page_list);
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE,
			    -(long)nr_reclaimed);
	/*
	 * Since lazyfree pages are isolated from file LRU from the beginning,
	 * they will rotate back to anonymous LRU in the end if it failed to
	 * discard so isolated count will be mismatched.
	 * Compensate the isolated count for both LRU lists.
	 */
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_ANON,
			    stat.nr_lazyfree_fail);
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE,
			    -(long)stat.nr_lazyfree_fail);
	return nr_reclaimed;
}

/*
 * Attempt to remove the specified page from its LRU.  Only take this page
 * if it is of the appropriate PageActive status.  Pages which are being
 * freed elsewhere are also ignored.
 *
 * page:	page to consider
 * mode:	one of the LRU isolation modes defined above
 *
 * returns true on success, false on failure.
 */
/*
 * IAMROOT, 2022.03.26: 
 * 5개의 lru list중 4개를 사용하고, 마지막 unevictable lru list는 조건에 따라
 * 사용을 하는데 다음과 같은 페이지들은 제외한다.
 * 1) async mode로 수행시 기록 중인 페이지(write-back)
 * 2)       "             dirty 상태의 페이지가 lock을 획득하지 못한 경우 
 * 3)       "             dirty 상태의 파일 매핑된 페이지가 
 *                        ops->migratepage 콜백구현이 없을때
 *                        (참고: ext2등의 일반적인 파일시스템에는 구현되어 있음)
 * 
 * 4) unmapped 페이지들만 요청한 경우(ISOLATE_UNMAPPED) 해당 페이지가 파일 매핑된 
 *    페이지인 경우
 *
 * --- memory 측면에서 disk로부터 file을 읽게되는 큰 그림
 *
 * - open/read/write syscall 호출시
 *
 *            page cache 사용         anon memory 사용
 *          +--------------+   +-------------------------+
 *          |              |   |                 syscall |
 *          |              |   |                 +-----+ |
 * +----+   |+------------+|   |+-----------+    |open | |
 * |disk| =>|| file cache || =>||buffer     | => |read | |
 * +----+   |+------------+|   |+-----------+    |write| |
 *          |              |   |                 +-----+ |
 *          +--------------+   +-------------------------+
 *
 * - mmap 사용시
 *
 *           pinned page     vm
 *          +-----------+    +-----------------------+
 *          |syscall    |    | mmap 요청 인자에 따른 |
 * +----+   |+----+     |    | vm mapping 주소 연결  |
 * |disk| =>||mmap|     | => |                       |
 * +----+   |+----+     |    |                       |
 *          +-----------+    +-----------------------+
 *                           이렇게 연결(mmapping)될경우 fs에서 migrate call
 *                           구현이 없으면 migrate 할수 없다.
 * --- lru page
 * - lru page라는 의미 자체가 이미 할당중인 page이다. migrate 가능한 것들을
 *   찾는 과정이된다.
 */
bool __isolate_lru_page_prepare(struct page *page, isolate_mode_t mode)
{
	/* Only take pages on the LRU. */
	if (!PageLRU(page))
		return false;

	/* Compaction should not handle unevictable pages but CMA can do so */
/*
 * IAMROOT, 2022.03.31:
 * - unevictable page인경우 cma에 한해서 migrate가 가능하다.
 */
	if (PageUnevictable(page) && !(mode & ISOLATE_UNEVICTABLE))
		return false;

	/*
	 * To minimise LRU disruption, the caller can indicate that it only
	 * wants to isolate pages it will be able to operate on without
	 * blocking - clean pages for the most part.
	 *
	 * ISOLATE_ASYNC_MIGRATE is used to indicate that it only wants to pages
	 * that it is possible to migrate without blocking
	 */
	if (mode & ISOLATE_ASYNC_MIGRATE) {
		/* All the caller can do on PageWriteback is block */
		if (PageWriteback(page))
			return false;

		if (PageDirty(page)) {
			struct address_space *mapping;
			bool migrate_dirty;

			/*
			 * Only pages without mappings or that have a
			 * ->migratepage callback are possible to migrate
			 * without blocking. However, we can be racing with
			 * truncation so it's necessary to lock the page
			 * to stabilise the mapping as truncation holds
			 * the page lock until after the page is removed
			 * from the page cache.
			 */
/*
 * IAMROOT, 2022.03.31:
 * - truncate와 race가 될수있기 때문에(Git blame) lock을 한다는 것과
 *   mapping이 없거나, migrate callback이 있는 page만 block없이 migrate
 *   수행이 가능하다고 한다.
 */
			if (!trylock_page(page))
				return false;

			mapping = page_mapping(page);
			migrate_dirty = !mapping || mapping->a_ops->migratepage;
			unlock_page(page);
			if (!migrate_dirty)
				return false;
		}
	}

	if ((mode & ISOLATE_UNMAPPED) && page_mapped(page))
		return false;

	return true;
}

/*
 * Update LRU sizes after isolating pages. The LRU size updates must
 * be complete before mem_cgroup_update_lru_size due to a sanity check.
 */
/*
 * IAMROOT, 2022.04.23:
 * - taken한(isolate 성공) page가 존재하는 zone만 lru size를 update한다.
 */
static __always_inline void update_lru_sizes(struct lruvec *lruvec,
			enum lru_list lru, unsigned long *nr_zone_taken)
{
	int zid;

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		if (!nr_zone_taken[zid])
			continue;

		update_lru_size(lruvec, lru, zid, -nr_zone_taken[zid]);
	}

}

/*
 * Isolating page from the lruvec to fill in @dst list by nr_to_scan times.
 *
 * lruvec->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Lru_lock must be held before calling this function.
 *
 * @nr_to_scan:	The number of eligible pages to look through on the list.
 * @lruvec:	The LRU vector to pull pages from.
 * @dst:	The temp list to put pages on to.
 * @nr_scanned:	The number of pages that were scanned.
 * @sc:		The scan_control struct for this reclaim session
 * @lru:	LRU list id for isolating
 *
 * returns how many pages were moved onto *@dst.
 */
/*
 * IAMROOT, 2022.04.23:
 * @nr_scanned scan시도를 한번이라도 했던 page개수
 * @return isolate가 성공한 page개수(nr_taken)
 *
 * @nr_to_scan만큼 scan을 하며 isolate를 시도한다. 성공한 page들은 @dst로 옮기고
 * 개수를 return시킨다.
 * isolate 실패한 page들은 rotate한다.
 */
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
/*
 * IAMROOT, 2022.04.23:
 * - scan : @sc에서 요청한 범위안에 있는 page 개수
 * - nr_taken : scan 페이지 개수 내에서 isolate까지 성공한 page개수
 */
	unsigned long nr_taken = 0;
	unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
	unsigned long nr_skipped[MAX_NR_ZONES] = { 0, };
	unsigned long skipped = 0;
	unsigned long scan, total_scan, nr_pages;
	LIST_HEAD(pages_skipped);
	isolate_mode_t mode = (sc->may_unmap ? 0 : ISOLATE_UNMAPPED);

	total_scan = 0;
	scan = 0;
	while (scan < nr_to_scan && !list_empty(src)) {
		struct page *page;

		page = lru_to_page(src);
		prefetchw_prev_lru_page(page, src, flags);

		nr_pages = compound_nr(page);
		total_scan += nr_pages;

/*
 * IAMROOT, 2022.04.23:
 * - 요청한 zone에 속한 page가 아닐 경우 pages_skipped로 옮기고 nr_skipped를
 *   누적한다.
 */
		if (page_zonenum(page) > sc->reclaim_idx) {
			list_move(&page->lru, &pages_skipped);
			nr_skipped[page_zonenum(page)] += nr_pages;
			continue;
		}

		/*
		 * Do not count skipped pages because that makes the function
		 * return with no isolated pages if the LRU mostly contains
		 * ineligible pages.  This causes the VM to not reclaim any
		 * pages, triggering a premature OOM.
		 *
		 * Account all tail pages of THP.  This would not cause
		 * premature OOM since __isolate_lru_page() returns -EBUSY
		 * only when the page is being freed somewhere else.
		 */
		scan += nr_pages;
/*
 * IAMROOT, 2022.04.23:
 * - isolate를 실패시 @src로 돌려보낸다. 이때 @src의 맨앞에 위치하게 될것이다.
 *   (빼올땐 맨뒤, 다시 넣을땐 맨 앞.(rotate))
 */
		if (!__isolate_lru_page_prepare(page, mode)) {
			/* It is being freed elsewhere */
			list_move(&page->lru, src);
			continue;
		}
		/*
		 * Be careful not to clear PageLRU until after we're
		 * sure the page is not being freed elsewhere -- the
		 * page release code relies on it.
		 */
/*
 * IAMROOT, 2022.04.23:
 * - get refcount
 */
		if (unlikely(!get_page_unless_zero(page))) {
			list_move(&page->lru, src);
			continue;
		}
/*
 * IAMROOT, 2022.04.23:
 * - 다른 thread가 isolate를 해버렸다. 성공하게 되면 lru flag를 clear한다.
 */
		if (!TestClearPageLRU(page)) {
			/* Another thread is already isolating this page */
			put_page(page);
			list_move(&page->lru, src);
			continue;
		}

/*
 * IAMROOT, 2022.04.23:
 * - isolate prepare, get refcount, lru clear 성공까지 되면 isolate성공으로
 *   판단한다.
 */
		nr_taken += nr_pages;
		nr_zone_taken[page_zonenum(page)] += nr_pages;
		list_move(&page->lru, dst);
	}

	/*
	 * Splice any skipped pages to the start of the LRU list. Note that
	 * this disrupts the LRU order when reclaiming for lower zones but
	 * we cannot splice to the tail. If we did then the SWAP_CLUSTER_MAX
	 * scanning would soon rescan the same pages to skip and put the
	 * system at risk of premature OOM.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - skipped page가 존재하면 src로 한번에 복귀시키고(rotate) PGSCAN_SKIP count를 누적시킨다.
 */
	if (!list_empty(&pages_skipped)) {
		int zid;

		list_splice(&pages_skipped, src);
		for (zid = 0; zid < MAX_NR_ZONES; zid++) {
			if (!nr_skipped[zid])
				continue;

			__count_zid_vm_events(PGSCAN_SKIP, zid, nr_skipped[zid]);
			skipped += nr_skipped[zid];
		}
	}
	*nr_scanned = total_scan;
	trace_mm_vmscan_lru_isolate(sc->reclaim_idx, sc->order, nr_to_scan,
				    total_scan, skipped, nr_taken, mode, lru);
	update_lru_sizes(lruvec, lru, nr_zone_taken);
	return nr_taken;
}

/**
 * isolate_lru_page - tries to isolate a page from its LRU list
 * @page: page to isolate from its LRU list
 *
 * Isolates a @page from an LRU list, clears PageLRU and adjusts the
 * vmstat statistic corresponding to whatever LRU list the page was on.
 *
 * Returns 0 if the page was removed from an LRU list.
 * Returns -EBUSY if the page was not on an LRU list.
 *
 * The returned page will have PageLRU() cleared.  If it was found on
 * the active list, it will have PageActive set.  If it was found on
 * the unevictable list, it will have the PageUnevictable bit set. That flag
 * may need to be cleared by the caller before letting the page go.
 *
 * The vmstat statistic corresponding to the list on which the page was
 * found will be decremented.
 *
 * Restrictions:
 *
 * (1) Must be called with an elevated refcount on the page. This is a
 *     fundamental difference from isolate_lru_pages (which is called
 *     without a stable reference).
 * (2) the lru_lock must not be held.
 * (3) interrupts must be enabled.
 */
int isolate_lru_page(struct page *page)
{
	int ret = -EBUSY;

	VM_BUG_ON_PAGE(!page_count(page), page);
	WARN_RATELIMIT(PageTail(page), "trying to isolate tail page");

	if (TestClearPageLRU(page)) {
		struct lruvec *lruvec;

		get_page(page);
		lruvec = lock_page_lruvec_irq(page);
		del_page_from_lru_list(page, lruvec);
		unlock_page_lruvec_irq(lruvec);
		ret = 0;
	}

	return ret;
}

/*
 * A direct reclaimer may isolate SWAP_CLUSTER_MAX pages from the LRU list and
 * then get rescheduled. When there are massive number of tasks doing page
 * allocation, such sleeping direct reclaimers may keep piling up on each CPU,
 * the LRU list will go small and be scanned faster than necessary, leading to
 * unnecessary swapping, thrashing and OOM.
 */
/*
 * IAMROOT, 2022.04.23:
 * - papago
 *   직접 회수자는 SWAP_CLUSTER_MAX 페이지를 LRU 목록에서 분리한 다음 다시
 *   예약할 수 있습니다. 페이지 할당을 수행하는 엄청난 수의 태스크가 있을 때,
 *   이러한 슬립 다이렉트 회수기는 각 CPU에 계속 쌓일 수 있으며, LRU 목록은
 *   작아지고 필요 이상으로 빠르게 스캔되어 불필요한 스왑, 스레싱 및 OOM으로
 *   이어진다.
 * - isolate > inactive 이면 isolate가 많다고 판단한다.
 * - 많은 page alloc등의 상황이 일어나고 있을시 reclaim이 쓸모없는 일을 할수있다.
 *   그것을 isolate page개수와 비교해 감지하는 역할을 수행한다.
 */
static int too_many_isolated(struct pglist_data *pgdat, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;

	if (current_is_kswapd())
		return 0;

	if (!writeback_throttling_sane(sc))
		return 0;

	if (file) {
		inactive = node_page_state(pgdat, NR_INACTIVE_FILE);
		isolated = node_page_state(pgdat, NR_ISOLATED_FILE);
	} else {
		inactive = node_page_state(pgdat, NR_INACTIVE_ANON);
		isolated = node_page_state(pgdat, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - papago
 *   GFP_NIO/GFP_NOFS 호출자는 더 많은 페이지를 분리할 수 있으므로 일반적인
 *   직접 회수자에 의해 차단되지 않고 순환 교착 상태를 형성합니다.
 * - __GFP_IO, __GFP_FS둘다 있으면 inactive를 1/8로 줄인다.
 */
	if ((sc->gfp_mask & (__GFP_IO | __GFP_FS)) == (__GFP_IO | __GFP_FS))
		inactive >>= 3;

	return isolated > inactive;
}

/*
 * move_pages_to_lru() moves pages from private @list to appropriate LRU list.
 * On return, @list is reused as a list of pages to be freed by the caller.
 *
 * Returns the number of pages moved to the given lruvec.
 */
/*
 * IAMROOT, 2022.04.23:
 * @list[in/out] in - lru로 옮길 pages
 *               out - lru로 옮기면서 free할 pages이 생기면 넣는다.
 * @return lru move에 성공한 page개수
 */
static unsigned int move_pages_to_lru(struct lruvec *lruvec,
				      struct list_head *list)
{
	int nr_pages, nr_moved = 0;
	LIST_HEAD(pages_to_free);
	struct page *page;

	while (!list_empty(list)) {
		page = lru_to_page(list);
		VM_BUG_ON_PAGE(PageLRU(page), page);
		list_del(&page->lru);
		if (unlikely(!page_evictable(page))) {
			spin_unlock_irq(&lruvec->lru_lock);
			putback_lru_page(page);
			spin_lock_irq(&lruvec->lru_lock);
			continue;
		}

		/*
		 * The SetPageLRU needs to be kept here for list integrity.
		 * Otherwise:
		 *   #0 move_pages_to_lru             #1 release_pages
		 *   if !put_page_testzero
		 *				      if (put_page_testzero())
		 *				        !PageLRU //skip lru_lock
		 *     SetPageLRU()
		 *     list_add(&page->lru,)
		 *                                        list_add(&page->lru,)
		 */
		SetPageLRU(page);
/*
 * IAMROOT, 2022.04.23:
 * - ref가 0으로 됬으면 그 사이에 해제된것이므로 buddy로 되돌릴 준비를 한다.
 */
		if (unlikely(put_page_testzero(page))) {
			__clear_page_lru_flags(page);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&lruvec->lru_lock);
				destroy_compound_page(page);
				spin_lock_irq(&lruvec->lru_lock);
			} else
				list_add(&page->lru, &pages_to_free);

			continue;
		}

		/*
		 * All pages were isolated from the same lruvec (and isolation
		 * inhibits memcg migration).
		 */
		VM_BUG_ON_PAGE(!page_matches_lruvec(page, lruvec), page);
		add_page_to_lru_list(page, lruvec);
		nr_pages = thp_nr_pages(page);
		nr_moved += nr_pages;
/*
 * IAMROOT, 2022.04.23:
 * - page가 active이면 @lruvec의 nonresident_age를 parent를 올라가며 증가시킨다.
 */
		if (PageActive(page))
			workingset_age_nonresident(lruvec, nr_pages);
	}

	/*
	 * To save our caller's stack, now use input list for pages to free.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - free할 page들은 이미 비어있을 list에 옮겨 놓는다.
 */
	list_splice(&pages_to_free, list);

	return nr_moved;
}

/*
 * If a kernel thread (such as nfsd for loop-back mounts) services
 * a backing device by writing to the page cache it sets PF_LOCAL_THROTTLE.
 * In that case we should only throttle if the backing device it is
 * writing to is congested.  In other cases it is safe to throttle.
 */
static int current_may_throttle(void)
{
	return !(current->flags & PF_LOCAL_THROTTLE) ||
		current->backing_dev_info == NULL ||
		bdi_write_congested(current->backing_dev_info);
}

/*
 * shrink_inactive_list() is a helper for shrink_node().  It returns the number
 * of reclaimed pages
 */

/*
 * IAMROOT, 2022.05.07:
 * - reclaim을 수행한다.
 */
static unsigned long
shrink_inactive_list(unsigned long nr_to_scan, struct lruvec *lruvec,
		     struct scan_control *sc, enum lru_list lru)
{
	LIST_HEAD(page_list);
	unsigned long nr_scanned;
	unsigned int nr_reclaimed = 0;
	unsigned long nr_taken;
	struct reclaim_stat stat;
	bool file = is_file_lru(lru);
	enum vm_event_item item;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	bool stalled = false;

/*
 * IAMROOT, 2022.04.23:
 * - isolate 된 page가 inactvie page에 비해 많으면(page alloc등의 일이 많은상태)
 *   0.1초 지연시킨다. 2번이상 지연되면 return.
 */
	while (unlikely(too_many_isolated(pgdat, file, sc))) {
		if (stalled)
			return 0;

		/* wait a bit for the reclaimer. */
		msleep(100);
		stalled = true;

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &page_list,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	item = current_is_kswapd() ? PGSCAN_KSWAPD : PGSCAN_DIRECT;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_scanned);
	__count_memcg_events(lruvec_memcg(lruvec), item, nr_scanned);
	__count_vm_events(PGSCAN_ANON + file, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

	if (nr_taken == 0)
		return 0;

	nr_reclaimed = shrink_page_list(&page_list, pgdat, sc, &stat, false);

	spin_lock_irq(&lruvec->lru_lock);
/*
 * IAMROOT, 2022.04.30:
 * - reclaim을 못한 나머지들을 되돌린다.
 */
	move_pages_to_lru(lruvec, &page_list);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	item = current_is_kswapd() ? PGSTEAL_KSWAPD : PGSTEAL_DIRECT;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_reclaimed);
	__count_memcg_events(lruvec_memcg(lruvec), item, nr_reclaimed);
	__count_vm_events(PGSTEAL_ANON + file, nr_reclaimed);
	spin_unlock_irq(&lruvec->lru_lock);

/*
 * IAMROOT, 2022.04.30:
 * - reclaim을 하면서 pageout된 page개수를 cost로 넣는다.
 */
	lru_note_cost(lruvec, file, stat.nr_pageout);
	mem_cgroup_uncharge_list(&page_list);
	free_unref_page_list(&page_list);

	/*
	 * If dirty pages are scanned that are not queued for IO, it
	 * implies that flushers are not doing their job. This can
	 * happen when memory pressure pushes dirty pages to the end of
	 * the LRU before the dirty limits are breached and the dirty
	 * data has expired. It can also happen when the proportion of
	 * dirty pages grows not through writes but through memory
	 * pressure reclaiming all the clean cache. And in some cases,
	 * the flushers simply cannot keep up with the allocation
	 * rate. Nudge the flusher threads in case they are asleep.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - papago
 *   IO를 위해 대기열에 있지 않은 더러운 페이지가 검색되면 플러셔가 작업을 수행하지
 *   않고 있음을 의미합니다. 메모리 압력이 더티 한계를 위반하고 더티 데이터가
 *   만료되기 전에 더티 페이지를 LRU의 끝으로 밀어넣을 때 발생할 수 있습니다.
 *   또한 쓰기가 아닌 모든 클린 캐시를 회수하는 메모리 압력으로 인해 dirty page의
 *   비율이 증가할 때 발생할 수 있습니다. 그리고 어떤 경우에는, 플러셔가 단순히
 *   할당 비율을 따라가지 못한다. 잠들어 있을 때를 대비해서 밀어라
 * - unqueue
 *   dirty && !writeback.
 */
	if (stat.nr_unqueued_dirty == nr_taken)
		wakeup_flusher_threads(WB_REASON_VMSCAN);

	sc->nr.dirty += stat.nr_dirty;
	sc->nr.congested += stat.nr_congested;
	sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	sc->nr.writeback += stat.nr_writeback;
	sc->nr.immediate += stat.nr_immediate;
	sc->nr.taken += nr_taken;
	if (file)
		sc->nr.file_taken += nr_taken;

	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			nr_scanned, nr_reclaimed, &stat, sc->priority, file);
	return nr_reclaimed;
}

/*
 * shrink_active_list() moves pages from the active LRU to the inactive LRU.
 *
 * We move them the other way if the page is referenced by one or more
 * processes.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()), so
 * we should drop lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->_refcount against each page.
 * But we had to alter page->flags anyway.
 */
/*
 * IAMROOT, 2022.04.23:
 * - 참조하는 실행 code를 제외한 page들을 inactive list로 옮긴다.
 *   nr_to_scan은 위에서 최대 32개로 제한됫을것이다.
 */
static void shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	struct page *page;
	unsigned nr_deactivate, nr_activate;
	unsigned nr_rotated = 0;
	int file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

/*
 * IAMROOT, 2022.04.23:
 * - @lruvec에서 @nr_to_scan만큼 scan을 하여 isolate를 시도한다.
 *   isolate에 성공한 page들은 l_hold에 넣어지고 nr_taken로 개수가
 *   return 된다. (nr_scanned는 scan을 시도한 page수)
 */
	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

	if (!cgroup_reclaim(sc))
		__count_vm_events(PGREFILL, nr_scanned);
	__count_memcg_events(lruvec_memcg(lruvec), PGREFILL, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

/*
 * IAMROOT, 2022.04.23:
 * - isolate된 page들을 조건에 따라 l_active, l_inactive로 넣어놓는다.
 */
	while (!list_empty(&l_hold)) {
		cond_resched();
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

/*
 * IAMROOT, 2022.04.23:
 * - app등에서 Mlock이 됫거나 unevictable로 변경 될 경우를 검사한다.
 */
		if (unlikely(!page_evictable(page))) {
			putback_lru_page(page);
			continue;
		}

/*
 * IAMROOT, 2022.04.23:
 * - buffer heads over인 상황이고 page가 private를 가지고 있으면(즉 buffer를
 *   가지고 있는 page이면) lock시도후 release한다.
 */
		if (unlikely(buffer_heads_over_limit)) {
			if (page_has_private(page) && trylock_page(page)) {
				if (page_has_private(page))
					try_to_release_page(page, 0);
				unlock_page(page);
			}
		}

		if (page_referenced(page, 0, sc->target_mem_cgroup,
				    &vm_flags)) {
			/*
			 * Identify referenced, file-backed active pages and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon pages
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC pages,
			 * so we ignore them here.
			 */
/*
 * IAMROOT, 2022.04.23:
 * - 참조된 page를 찾았고, 그중에 실행가능한 파일 page가 있다면 active list로
 *   옮겨놓는다.
 *   회수를 하지 말고 active에 놔두라는 의미.
 */
			if ((vm_flags & VM_EXEC) && page_is_file_lru(page)) {
				nr_rotated += thp_nr_pages(page);
				list_add(&page->lru, &l_active);
				continue;
			}
		}

/*
 * IAMROOT, 2022.04.23:
 * - deactive를 한다. active flag를 지우고 inactive list에 옮긴다.
 *   active -> inactive가 될때 workingset 가 set된다.
 */
		ClearPageActive(page);	/* we are de-activating */
		SetPageWorkingset(page);
		list_add(&page->lru, &l_inactive);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&lruvec->lru_lock);

	nr_activate = move_pages_to_lru(lruvec, &l_active);
	nr_deactivate = move_pages_to_lru(lruvec, &l_inactive);
	/* Keep all free pages in l_active list */
/*
 * IAMROOT, 2022.04.23:
 * - free page를 l_active에 모아놓는다
 */
	list_splice(&l_inactive, &l_active);

	__count_vm_events(PGDEACTIVATE, nr_deactivate);
	__count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE, nr_deactivate);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&lruvec->lru_lock);

	mem_cgroup_uncharge_list(&l_active);
	free_unref_page_list(&l_active);
	trace_mm_vmscan_lru_shrink_active(pgdat->node_id, nr_taken, nr_activate,
			nr_deactivate, nr_rotated, sc->priority, file);
}

unsigned long reclaim_pages(struct list_head *page_list)
{
	int nid = NUMA_NO_NODE;
	unsigned int nr_reclaimed = 0;
	LIST_HEAD(node_page_list);
	struct reclaim_stat dummy_stat;
	struct page *page;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.no_demotion = 1,
	};

	noreclaim_flag = memalloc_noreclaim_save();

	while (!list_empty(page_list)) {
		page = lru_to_page(page_list);
		if (nid == NUMA_NO_NODE) {
			nid = page_to_nid(page);
			INIT_LIST_HEAD(&node_page_list);
		}

		if (nid == page_to_nid(page)) {
			ClearPageActive(page);
			list_move(&page->lru, &node_page_list);
			continue;
		}

		nr_reclaimed += shrink_page_list(&node_page_list,
						NODE_DATA(nid),
						&sc, &dummy_stat, false);
		while (!list_empty(&node_page_list)) {
			page = lru_to_page(&node_page_list);
			list_del(&page->lru);
			putback_lru_page(page);
		}

		nid = NUMA_NO_NODE;
	}

	if (!list_empty(&node_page_list)) {
		nr_reclaimed += shrink_page_list(&node_page_list,
						NODE_DATA(nid),
						&sc, &dummy_stat, false);
		while (!list_empty(&node_page_list)) {
			page = lru_to_page(&node_page_list);
			list_del(&page->lru);
			putback_lru_page(page);
		}
	}

	memalloc_noreclaim_restore(noreclaim_flag);

	return nr_reclaimed;
}

static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
				 struct lruvec *lruvec, struct scan_control *sc)
{
	if (is_active_lru(lru)) {
/*
 * IAMROOT, 2022.04.23:
 * - deactive할수있다면 shrink_active_list를 호출한다.
 */
		if (sc->may_deactivate & (1 << is_file_lru(lru)))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		else
			sc->skipped_deactivate = 1;
		return 0;
	}

	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}

/*
 * The inactive anon list should be small enough that the VM never has
 * to do too much work.
 *
 * The inactive file list should be small enough to leave most memory
 * to the established workingset on the scan-resistant active list,
 * but large enough to avoid thrashing the aggregate readahead window.
 *
 * Both inactive lists should also be large enough that each inactive
 * page has a chance to be referenced again before it is reclaimed.
 *
 * If that fails and refaulting is observed, the inactive list grows.
 *
 * The inactive_ratio is the target ratio of ACTIVE to INACTIVE pages
 * on this LRU, maintained by the pageout code. An inactive_ratio
 * of 3 means 3:1 or 25% of the pages are kept on the inactive list.
 *
 * total     target    max
 * memory    ratio     inactive
 * -------------------------------------
 *   10MB       1         5MB
 *  100MB       1        50MB
 *    1GB       3       250MB
 *   10GB      10       0.9GB
 *  100GB      31         3GB
 *    1TB     101        10GB
 *   10TB     320        32GB
 */

/*
 * IAMROOT, 2022.05.07:
 * - papago
 * inactive anon list는 VM이 너무 많은 작업을 수행할 필요가 없을 정도로 작아야
 * 합니다.
 *
 * 비활성 파일 목록은 대부분의 메모리를 검사 방지 활성 목록에 설정된 workingsets에
 * 남겨둘 수 있을 만큼 작아야 하지만, 전체 읽기 미리 실행된 창이 스레싱되지 않도록
 * 충분히 커야 합니다.
 *
 * 또한 두 비활성 목록은 각 비활성 페이지가 회수되기 전에 다시 참조될 수 있을
 * 정도로 충분히 커야 합니다.
 * 
 * 이 작업이 실패하여 재장애가 관찰되면 비활성 목록이 증가합니다.
 *
 * inactive_ratio는 이 LRU의 ACTIVE 페이지와 ICTIVE 페이지의 목표 비율이며,
 * 페이지아웃 코드에 의해 유지된다. 3의 inactive_ratio는 3을 의미한다.
 * 페이지의 1 또는 25%가 비활성 목록에 보관됩니다.
 */
static bool inactive_is_low(struct lruvec *lruvec, enum lru_list inactive_lru)
{
	enum lru_list active_lru = inactive_lru + LRU_ACTIVE;
	unsigned long inactive, active;
	unsigned long inactive_ratio;
	unsigned long gb;

	inactive = lruvec_page_state(lruvec, NR_LRU_BASE + inactive_lru);
	active = lruvec_page_state(lruvec, NR_LRU_BASE + active_lru);

	gb = (inactive + active) >> (30 - PAGE_SHIFT);

/*
 * IAMROOT, 2022.05.07:
 * - int_sqrt(10 * gb) = root(10 * gb)
 */
	if (gb)
		inactive_ratio = int_sqrt(10 * gb);
	else
		inactive_ratio = 1;

/*
 * IAMROOT, 2022.05.07:
 * - 1GB 미만으로 차이나면 inactive < active
 * - 1GB 이상 차이나면 inactive * root(10 * gb) < active
 *   ex) gb 
 *       1    inactive : active = 1 : 3
 *       2    inactive : active = 1 : 4
 *       3    inactive : active = 1 : 5
 *       10   inactive : active = 1 : 10
 *       16   inactive : active = 1 : 12
 */
	return inactive * inactive_ratio < active;
}

enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.  The relative value of each set of LRU lists is determined
 * by looking at the fraction of the pages scanned we did rotate back
 * onto the active list instead of evict.
 *
 * nr[0] = anon inactive pages to scan; nr[1] = anon active pages to scan
 * nr[2] = file inactive pages to scan; nr[3] = file active pages to scan
 */
/*
 * IAMROOT, 2022.04.23:
 * - memcg에서 swappiness값을 가져와서 그 값을 토대로 scan 비율을 얻어오고
 *   evictable lru을 @lurvec에서 순회하며 sc->reclaim_idx zone까지의
 *   page수를 가져와서 lru마다 scan할 page수를 @nr에 얻어온다.
 */
static void get_scan_count(struct lruvec *lruvec, struct scan_control *sc,
			   unsigned long *nr)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	unsigned long anon_cost, file_cost, total_cost;
	int swappiness = mem_cgroup_swappiness(memcg);
	u64 fraction[ANON_AND_FILE];
	u64 denominator = 0;	/* gcc */
	enum scan_balance scan_balance;
	unsigned long ap, fp;
	enum lru_list lru;

/*
 * IAMROOT, 2022.04.23:
 * - 요청에서 swap을 하지 말라고 했거나 anon_page가 reclaim이 불가능한 상태면
 *   (swap할 공간이 없으므로) file만을 scan한다.
 */
	/* If we have no swap space, do not bother scanning anon pages. */
	if (!sc->may_swap || !can_reclaim_anon_pages(memcg, pgdat->node_id, sc)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Global reclaim will swap to prevent OOM even with no
	 * swappiness, but memcg users want to use this knob to
	 * disable swapping for individual groups completely when
	 * using the memory controller's swap limit feature would be
	 * too expensive.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - papago
 *   스왑 기능이 없는 경우에도 OOM을 방지하기 위해 글로벌 회수 기능이 스왑되지만
 *   memcg 사용자는 메모리 컨트롤러의 스왑 제한 기능을 사용하는 경우 이 knob를
 *   사용하여 개별 그룹에 대한 스왑을 완전히 비활성화하려고 합니다.
 *
 * - 상위에서 target memcg가 설정되서 내려왔지만 실제 전역이나 memcg의 
 *   swappiness설정값이 disable인 경우 swap을 하지 말라는것이다.
 */
	if (cgroup_reclaim(sc) && !swappiness) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Do not apply any pressure balancing cleverness when the
	 * system is close to OOM, scan both anon and file equally
	 * (unless the swappiness setting disagrees with swapping).
	 */
/*
 * IAMROOT, 2022.04.23:
 * - 시스템이 OOM에 근접해 있을 때는 압력 밸런싱의 영리함을 적용하지 말고,
 *   anon과 파일을 모두 동일하게 스캔하십시오.
 *   (스왑성 설정이 스왑과 일치하지 않는 경우).
 * - 우선순위가 가장 높게 설정되 있을 경우 swappiness를 고려안하고 균등하게 한다.
 */
	if (!sc->priority && swappiness) {
		scan_balance = SCAN_EQUAL;
		goto out;
	}

	/*
	 * If the system is almost out of file pages, force-scan anon.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - file 사용이 거의 없는 시스템에서는 anon page만 스캔한다.
 */
	if (sc->file_is_tiny) {
		scan_balance = SCAN_ANON;
		goto out;
	}

	/*
	 * If there is enough inactive page cache, we do not reclaim
	 * anything from the anonymous working right now.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - cold cache(inactive page cache)가 많다고 판단했던 경우 file만 scan.
 */
	if (sc->cache_trim_mode) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	scan_balance = SCAN_FRACT;
	/*
	 * Calculate the pressure balance between anon and file pages.
	 *
	 * The amount of pressure we put on each LRU is inversely
	 * proportional to the cost of reclaiming each list, as
	 * determined by the share of pages that are refaulting, times
	 * the relative IO cost of bringing back a swapped out
	 * anonymous page vs reloading a filesystem page (swappiness).
	 *
	 * Although we limit that influence to ensure no list gets
	 * left behind completely: at least a third of the pressure is
	 * applied, before swappiness.
	 *
	 * With swappiness at 100, anon and file have equal IO cost.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - papago
 *   anon 페이지와 파일 페이지 사이의 압력 밸런스를 계산합니다.
 *
 * 각 LRU에 가해지는 압력은 각 목록을 재확보하는 데 드는 비용에 반비례합니다.
 * 이는 다시 장애가 발생한 페이지 공유에 따라 결정됩니다. 이는 스왑된
 * anon 페이지를 다시 불러올 때와 파일 시스템 페이지(스왑)를 다시 불러올
 * 때의 상대적 IO 비용에 곱한 것입니다.
 *
 * 목록이 완전히 뒤처지지 않도록 이러한 영향을 제한하고 있지만:
 * 적어도 3분의 1의 압력이 가해질 때, 휘청거린다.
 *
 * swappiness가 100인 경우, annon 및 file의 IO 비용은 동일합니다.*
 *
 * - 이전에 refault activate 및 pageout을 했던 file or anon 의 cost 비율을
 *   반영한다. (lru_note_cost()). cost가 높은쪽이 비율이 적게 잡힌다.
 *   만약 file cost가 높다면 anon쪽으로 비율이 많이 잡힐것이다.
 * ex) anon_cost = 10, file_cost = 20, swappiness = 100
 * ap = (100 * 31) / 11 = 약 300
 * fp = (100 * 31)) / 21 = 약 200
 * 즉 역비율이 된다.
 */
	total_cost = sc->anon_cost + sc->file_cost;
	anon_cost = total_cost + sc->anon_cost;
	file_cost = total_cost + sc->file_cost;
	total_cost = anon_cost + file_cost;

/*
 * IAMROOT, 2022.04.23:
 * - cost가 없다는 가정하에 계산
 *   ap = swappiness
 *   fp = 200 - swappiness
 */
	ap = swappiness * (total_cost + 1);
	ap /= anon_cost + 1;

	fp = (200 - swappiness) * (total_cost + 1);
	fp /= file_cost + 1;

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp;
out:
	for_each_evictable_lru(lru) {
		int file = is_file_lru(lru);
		unsigned long lruvec_size;
		unsigned long low, min;
		unsigned long scan;

/*
 * IAMROOT, 2022.04.23:
 * - @lruvec의 @lru에 해당하는 page수를 @sc->recalim_idx까지 합산하여 가져온다.
 */
		lruvec_size = lruvec_lru_size(lruvec, lru, sc->reclaim_idx);
		mem_cgroup_protection(sc->target_mem_cgroup, memcg,
				      &min, &low);

		if (min || low) {
			/*
			 * Scale a cgroup's reclaim pressure by proportioning
			 * its current usage to its memory.low or memory.min
			 * setting.
			 *
			 * This is important, as otherwise scanning aggression
			 * becomes extremely binary -- from nothing as we
			 * approach the memory protection threshold, to totally
			 * nominal as we exceed it.  This results in requiring
			 * setting extremely liberal protection thresholds. It
			 * also means we simply get no protection at all if we
			 * set it too low, which is not ideal.
			 *
			 * If there is any protection in place, we reduce scan
			 * pressure by how much of the total memory used is
			 * within protection thresholds.
			 *
			 * There is one special case: in the first reclaim pass,
			 * we skip over all groups that are within their low
			 * protection. If that fails to reclaim enough pages to
			 * satisfy the reclaim goal, we come back and override
			 * the best-effort low protection. However, we still
			 * ideally want to honor how well-behaved groups are in
			 * that case instead of simply punishing them all
			 * equally. As such, we reclaim them based on how much
			 * memory they are using, reducing the scan pressure
			 * again by how much of the total memory used is under
			 * hard protection.
			 */
			unsigned long cgroup_size = mem_cgroup_size(memcg);
			unsigned long protection;

			/* memory.low scaling, make sure we retry before OOM */
			if (!sc->memcg_low_reclaim && low > min) {
				protection = low;
				sc->memcg_low_skipped = 1;
			} else {
				protection = min;
			}

			/* Avoid TOCTOU with earlier protection check */
			cgroup_size = max(cgroup_size, protection);
/*
 * IAMROOT, 2022.04.23:
 * - scan = lruvec_size * (1 - (protection / (cgroup_size + 1)))
 * - memcg usage와 protection에 대한 역비율을 lruvec_size에도 적용한다.
 *
 * - ex) protection = 30, cgroup_size = 99, lruvec_size = 10
 *   scan = 10 - 10 * 30 / 100 = 10 - 300 /100 = 7
 *
 * - ex) protection = 30, cgroup_size = 200, lruvec_size = 10
 *   scan = 10 - 10 * 30 / 100 = 10 - 300 /201 = 9
 */
			scan = lruvec_size - lruvec_size * protection /
				(cgroup_size + 1);

			/*
			 * Minimally target SWAP_CLUSTER_MAX pages to keep
			 * reclaim moving forwards, avoiding decrementing
			 * sc->priority further than desirable.
			 */
			scan = max(scan, SWAP_CLUSTER_MAX);
		} else {
			scan = lruvec_size;
		}

		scan >>= sc->priority;

		/*
		 * If the cgroup's already been deleted, make sure to
		 * scrape out the remaining cache.
		 */
/*
 * IAMROOT, 2022.04.23:
 * - 위의 계산으로 scan이 0이고, memcg가 offline이 됬다면 해당 cache가 필요없어
 *   지므로 SWAP_CLUSTER_MAX를 min 기준으로 해서 scan을 정한다.
 */
		if (!scan && !mem_cgroup_online(memcg))
			scan = min(lruvec_size, SWAP_CLUSTER_MAX);

		switch (scan_balance) {
		case SCAN_EQUAL:
			/* Scan lists relative to size */
			break;
		case SCAN_FRACT:
			/*
			 * Scan types proportional to swappiness and
			 * their relative recent reclaim efficiency.
			 * Make sure we don't miss the last page on
			 * the offlined memory cgroups because of a
			 * round-off error.
			 */
/*
 * IAMROOT, 2022.04.23:
 * - 위에서 계산했던 ap, fp 비율을 토대로 scan을 계산한다.
 */
			scan = mem_cgroup_online(memcg) ?
			       div64_u64(scan * fraction[file], denominator) :
			       DIV64_U64_ROUND_UP(scan * fraction[file],
						  denominator);
			break;
		case SCAN_FILE:
		case SCAN_ANON:
			/* Scan one type exclusively */
			if ((scan_balance == SCAN_FILE) != file)
				scan = 0;
			break;
		default:
			/* Look ma, no brain */
			BUG();
		}

		nr[lru] = scan;
	}
}

/*
 * Anonymous LRU management is a waste if there is
 * ultimately no way to reclaim the memory.
 */
/*
 * IAMROOT, 2022.05.07:
 * - @pgdat에 대해서 anon page에 대해 page가 swap할 공간이 있거나
 *   demote가 가능한지 알아온다.
 */
static bool can_age_anon_pages(struct pglist_data *pgdat,
			       struct scan_control *sc)
{
	/* Aging the anon LRU is valuable if swap is present: */
	if (total_swap_pages > 0)
		return true;

	/* Also valuable if anon pages can be demoted: */
	return can_demote(pgdat->node_id, sc);
}

/*
 * IAMROOT, 2022.05.07:
 * - @lruvec에 대해서 reclaim을 수행한다.
 */
static void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	unsigned long nr[NR_LRU_LISTS];
	unsigned long targets[NR_LRU_LISTS];
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;
	struct blk_plug plug;
	bool scan_adjusted;

	get_scan_count(lruvec, sc, nr);

	/* Record the original scan target for proportional adjustments later */
	memcpy(targets, nr, sizeof(nr));

	/*
	 * Global reclaiming within direct reclaim at DEF_PRIORITY is a normal
	 * event that can occur when there is little memory pressure e.g.
	 * multiple streaming readers/writers. Hence, we do not abort scanning
	 * when the requested number of pages are reclaimed when scanning at
	 * DEF_PRIORITY on the assumption that the fact we are direct
	 * reclaiming implies that kswapd is not keeping up and it is best to
	 * do a batch of work at once. For memcg reclaim one check is made to
	 * abort proportional reclaim if either the file or anon lru has already
	 * dropped to zero at the first pass.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - recalim target memcg가 없고(global 이라는 뜻), kswapd에서 호출한게 아니고,
 *   (direct reclaim이라는 뜻)
 *   priority가 DEF_PRIORITY이면(처음 시작) scan_adjusted가 true가 된다.
 */
	scan_adjusted = (!cgroup_reclaim(sc) && !current_is_kswapd() &&
			 sc->priority == DEF_PRIORITY);

	blk_start_plug(&plug);
/*
 * IAMROOT, 2022.04.23:
 * - ACTIVE_ANON을 제외한 lru page들을 바라본다.
 */
	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		unsigned long nr_anon, nr_file, percentage;
		unsigned long nr_scanned;

		for_each_evictable_lru(lru) {
			if (nr[lru]) {
/*
 * IAMROOT, 2022.04.16:
 * - SWAP_CLUSTER_MAX을 min으로 기준하여 scan 숫자를 누적한다.
 */
				nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;

				nr_reclaimed += shrink_list(lru, nr_to_scan,
							    lruvec, sc);
			}
		}

		cond_resched();

		if (nr_reclaimed < nr_to_reclaim || scan_adjusted)
			continue;

		/*
		 * For kswapd and memcg, reclaim at least the number of pages
		 * requested. Ensure that the anon and file LRUs are scanned
		 * proportionally what was requested by get_scan_count(). We
		 * stop reclaiming one LRU and reduce the amount scanning
		 * proportional to the original scan target.
		 */
/*
 * IAMROOT, 2022.04.16:
 * - 위 for문에서 scan할것을 제외한 나머지 값들에 대해서 file, anon으로 나눠
 *   합산한다.
 */
		nr_file = nr[LRU_INACTIVE_FILE] + nr[LRU_ACTIVE_FILE];
		nr_anon = nr[LRU_INACTIVE_ANON] + nr[LRU_ACTIVE_ANON];

		/*
		 * It's just vindictive to attack the larger once the smaller
		 * has gone to zero.  And given the way we stop scanning the
		 * smaller below, this makes sure that we only make one nudge
		 * towards proportionality once we've got nr_to_reclaim.
		 */
		if (!nr_file || !nr_anon)
			break;

/*
 * IAMROOT, 2022.04.16:
 * - 남아있는 file, anon중에 작은것을 기준으로 scan_target, lru, percentage를 정한다.
 *   percentage = (scan으로 빠진 page) * 100/ (전체 page)
 */
		if (nr_file > nr_anon) {
			unsigned long scan_target = targets[LRU_INACTIVE_ANON] +
						targets[LRU_ACTIVE_ANON] + 1;
			lru = LRU_BASE;
			percentage = nr_anon * 100 / scan_target;
		} else {
			unsigned long scan_target = targets[LRU_INACTIVE_FILE] +
						targets[LRU_ACTIVE_FILE] + 1;
			lru = LRU_FILE;
			percentage = nr_file * 100 / scan_target;
		}

		/* Stop scanning the smaller of the LRU */
		nr[lru] = 0;
		nr[lru + LRU_ACTIVE] = 0;

		/*
		 * Recalculate the other LRU scan count based on its original
		 * scan target and the percentage scanning already complete
		 */

/*
 * IAMROOT, 2022.05.07:
 * - 위에서 정해진 lru의 반대(file <=> anon)의 nr을 산정한다. 최소 lru를 기준으로
 *   반대측의 lru와 비율을 조정한다.
 */
		lru = (lru == LRU_FILE) ? LRU_BASE : LRU_FILE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		lru += LRU_ACTIVE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		scan_adjusted = true;
	}
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;

	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - anon page에 대해서 inactive가 active에 비해 비율이 적으면 deactive 시도를한다.
 *   SWAP_CLUSTER_MAX개씩만 reclaim을 하기때문에 anon page에 대해서만 조금더
 *   신경 쓰는 개념.
 */
	if (can_age_anon_pages(lruvec_pgdat(lruvec), sc) &&
	    inactive_is_low(lruvec, LRU_INACTIVE_ANON))
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);
}

/* Use reclaim/compaction for costly allocs or under memory pressure */

/*
 * IAMROOT, 2022.05.07:
 * - @sc의 order와 priority에 대해서 reclaim중 compaction을 할지에 대해 정한다.
 * - compaction은 order가 1이상이며 order가 costly_order보다 크거나 priority가 낮을때
 *   (4번째 시도 이상) true로 return한다.
 */
static bool in_reclaim_compaction(struct scan_control *sc)
{
	if (IS_ENABLED(CONFIG_COMPACTION) && sc->order &&
			(sc->order > PAGE_ALLOC_COSTLY_ORDER ||
			 sc->priority < DEF_PRIORITY - 2))
		return true;

	return false;
}

/*
 * Reclaim/compaction is used for high-order allocation requests. It reclaims
 * order-0 pages before compacting the zone. should_continue_reclaim() returns
 * true if more pages should be reclaimed such that when the page allocator
 * calls try_to_compact_pages() that it will have enough free pages to succeed.
 * It will give up earlier than that if there is difficulty reclaiming pages.
 */
/*
 * IAMROOT, 2022.05.07:
 * - @sc에 대해서 reclaim을 더 해야되는건지를 결정한다.
 *   (모든 zone에 대해서 compaction을 못하는 상황에서만 reclaim 수행.)
 */
static inline bool should_continue_reclaim(struct pglist_data *pgdat,
					unsigned long nr_reclaimed,
					struct scan_control *sc)
{
	unsigned long pages_for_compaction;
	unsigned long inactive_lru_pages;
	int z;

	/* If not in reclaim/compaction mode, stop */
	if (!in_reclaim_compaction(sc))
		return false;

	/*
	 * Stop if we failed to reclaim any pages from the last SWAP_CLUSTER_MAX
	 * number of pages that were scanned. This will return to the caller
	 * with the risk reclaim/compaction and the resulting allocation attempt
	 * fails. In the past we have tried harder for __GFP_RETRY_MAYFAIL
	 * allocations through requiring that the full LRU list has been scanned
	 * first, by assuming that zero delta of sc->nr_scanned means full LRU
	 * scan, but that approximation was wrong, and there were corner cases
	 * where always a non-zero amount of pages were scanned.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - 직전 reclaim에서 성공한게 하나도 없으면 false
 */
	if (!nr_reclaimed)
		return false;

	/* If compaction would go ahead or the allocation would succeed, stop */
	for (z = 0; z <= sc->reclaim_idx; z++) {
		struct zone *zone = &pgdat->node_zones[z];
		if (!managed_zone(zone))
			continue;

		switch (compaction_suitable(zone, sc->order, 0, sc->reclaim_idx)) {
		case COMPACT_SUCCESS:
		case COMPACT_CONTINUE:
			return false;
		default:
			/* check next zone */
			;
		}
	}

/*
 * IAMROOT, 2022.05.07:
 * - 모든 zone이 COMPACT_SKIPPED이면 진입.
 */
	/*
	 * If we have not reclaimed enough pages for compaction and the
	 * inactive lists are large enough, continue reclaiming
	 */
	pages_for_compaction = compact_gap(sc->order);
	inactive_lru_pages = node_page_state(pgdat, NR_INACTIVE_FILE);
	if (can_reclaim_anon_pages(NULL, pgdat->node_id, sc))
		inactive_lru_pages += node_page_state(pgdat, NR_INACTIVE_ANON);

/*
 * IAMROOT, 2022.05.07:
 * inactive file + swap가능한 anon > pages_for_compaction
 */
	return inactive_lru_pages > pages_for_compaction;
}

/*
 * IAMROOT, 2022.05.07:
 * - @sc->target_mem_cgroup의 @pgdat의 memcg들에 대해 reclaim을 수행한다.
 */
static void shrink_node_memcgs(pg_data_t *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *target_memcg = sc->target_mem_cgroup;
	struct mem_cgroup *memcg;

	memcg = mem_cgroup_iter(target_memcg, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
		unsigned long reclaimed;
		unsigned long scanned;

		/*
		 * This loop can become CPU-bound when target memcgs
		 * aren't eligible for reclaim - either because they
		 * don't have any reclaimable pages, or because their
		 * memory is explicitly protected. Avoid soft lockups.
		 */
		cond_resched();

		mem_cgroup_calculate_protection(target_memcg, memcg);

		if (mem_cgroup_below_min(memcg)) {
			/*
			 * Hard protection.
			 * If there is no reclaimable memory, OOM.
			 */
			continue;
		} else if (mem_cgroup_below_low(memcg)) {
			/*
			 * Soft protection.
			 * Respect the protection only as long as
			 * there is an unprotected supply
			 * of reclaimable memory from other cgroups.
			 */
			if (!sc->memcg_low_reclaim) {
				sc->memcg_low_skipped = 1;
				continue;
			}
			memcg_memory_event(memcg, MEMCG_LOW);
		}

		reclaimed = sc->nr_reclaimed;
		scanned = sc->nr_scanned;

		shrink_lruvec(lruvec, sc);

		shrink_slab(sc->gfp_mask, pgdat->node_id, memcg,
			    sc->priority);

		/* Record the group's reclaim efficiency */
		vmpressure(sc->gfp_mask, memcg, false,
			   sc->nr_scanned - scanned,
			   sc->nr_reclaimed - reclaimed);

	} while ((memcg = mem_cgroup_iter(target_memcg, memcg, NULL)));
}

/*
 * IAMROOT, 2022.05.07:
 * - @pgdat에 대해 reclaim을 수행한다.
 */
static void shrink_node(pg_data_t *pgdat, struct scan_control *sc)
{
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long nr_reclaimed, nr_scanned;
	struct lruvec *target_lruvec;
	bool reclaimable = false;
	unsigned long file;

	target_lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup, pgdat);

again:
	/*
	 * Flush the memory cgroup stats, so that we read accurate per-memcg
	 * lruvec stats for heuristics.
	 */
	mem_cgroup_flush_stats();

	memset(&sc->nr, 0, sizeof(sc->nr));

	nr_reclaimed = sc->nr_reclaimed;
	nr_scanned = sc->nr_scanned;

	/*
	 * Determine the scan balance between anon and file LRUs.
	 */
	spin_lock_irq(&target_lruvec->lru_lock);
	sc->anon_cost = target_lruvec->anon_cost;
	sc->file_cost = target_lruvec->file_cost;
	spin_unlock_irq(&target_lruvec->lru_lock);

	/*
	 * Target desirable inactive:active list ratios for the anon
	 * and file LRU lists.
	 */
	if (!sc->force_deactivate) {
		unsigned long refaults;

		refaults = lruvec_page_state(target_lruvec,
				WORKINGSET_ACTIVATE_ANON);

/*
 * IAMROOT, 2022.05.07:
 * - refault != target refaults(snpashot_refaults() 참고)
 *   새로운 workingset activate가 있는경우 deactive(밀어내기)를 하겠다는것.
 * - incative_is_low
 *   inactive가 너무 적은경우 deactive를 하겠다는것.
 */
		if (refaults != target_lruvec->refaults[0] ||
			inactive_is_low(target_lruvec, LRU_INACTIVE_ANON))
			sc->may_deactivate |= DEACTIVATE_ANON;
		else
			sc->may_deactivate &= ~DEACTIVATE_ANON;

		/*
		 * When refaults are being observed, it means a new
		 * workingset is being established. Deactivate to get
		 * rid of any stale active pages quickly.
		 */
		refaults = lruvec_page_state(target_lruvec,
				WORKINGSET_ACTIVATE_FILE);
		if (refaults != target_lruvec->refaults[1] ||
		    inactive_is_low(target_lruvec, LRU_INACTIVE_FILE))
			sc->may_deactivate |= DEACTIVATE_FILE;
		else
			sc->may_deactivate &= ~DEACTIVATE_FILE;
	} else
		sc->may_deactivate = DEACTIVATE_ANON | DEACTIVATE_FILE;

	/*
	 * If we have plenty of inactive file pages that aren't
	 * thrashing, try to reclaim those first before touching
	 * anonymous pages.
	 */
	file = lruvec_page_state(target_lruvec, NR_INACTIVE_FILE);
	if (file >> sc->priority && !(sc->may_deactivate & DEACTIVATE_FILE))
		sc->cache_trim_mode = 1;
	else
		sc->cache_trim_mode = 0;

	/*
	 * Prevent the reclaimer from falling into the cache trap: as
	 * cache pages start out inactive, every cache fault will tip
	 * the scan balance towards the file LRU.  And as the file LRU
	 * shrinks, so does the window for rotation from references.
	 * This means we have a runaway feedback loop where a tiny
	 * thrashing file LRU becomes infinitely more attractive than
	 * anon pages.  Try to detect this based on file LRU size.
	 */
	if (!cgroup_reclaim(sc)) {
		unsigned long total_high_wmark = 0;
		unsigned long free, anon;
		int z;

		free = sum_zone_node_page_state(pgdat->node_id, NR_FREE_PAGES);
		file = node_page_state(pgdat, NR_ACTIVE_FILE) +
			   node_page_state(pgdat, NR_INACTIVE_FILE);

		for (z = 0; z < MAX_NR_ZONES; z++) {
			struct zone *zone = &pgdat->node_zones[z];
			if (!managed_zone(zone))
				continue;

			total_high_wmark += high_wmark_pages(zone);
		}

		/*
		 * Consider anon: if that's low too, this isn't a
		 * runaway file reclaim problem, but rather just
		 * extreme pressure. Reclaim as per usual then.
		 */
		anon = node_page_state(pgdat, NR_INACTIVE_ANON);

		sc->file_is_tiny =
			file + free <= total_high_wmark &&
			!(sc->may_deactivate & DEACTIVATE_ANON) &&
			anon >> sc->priority;
	}

	shrink_node_memcgs(pgdat, sc);

	if (reclaim_state) {
		sc->nr_reclaimed += reclaim_state->reclaimed_slab;
		reclaim_state->reclaimed_slab = 0;
	}

	/* Record the subtree's reclaim efficiency */
	vmpressure(sc->gfp_mask, sc->target_mem_cgroup, true,
		   sc->nr_scanned - nr_scanned,
		   sc->nr_reclaimed - nr_reclaimed);

	if (sc->nr_reclaimed - nr_reclaimed)
		reclaimable = true;

	if (current_is_kswapd()) {
		/*
		 * If reclaim is isolating dirty pages under writeback,
		 * it implies that the long-lived page allocation rate
		 * is exceeding the page laundering rate. Either the
		 * global limits are not being effective at throttling
		 * processes due to the page distribution throughout
		 * zones or there is heavy usage of a slow backing
		 * device. The only option is to throttle from reclaim
		 * context which is not ideal as there is no guarantee
		 * the dirtying process is throttled in the same way
		 * balance_dirty_pages() manages.
		 *
		 * Once a node is flagged PGDAT_WRITEBACK, kswapd will
		 * count the number of pages under pages flagged for
		 * immediate reclaim and stall if any are encountered
		 * in the nr_immediate check below.
		 */

/*
 * IAMROOT, 2022.05.07:
 * - papgo
 *   reclaim가 writeback에서 dirty page를 isolate하는 경우, 오래 지속되는 페이지
 *   할당률이 페이지 laundering 속도를 초과하고 있음을 의미합니다. 영역 전체에 페이지
 *   배포로 인해 조정 프로세스에서 전역 제한이 효과적이지 않거나 느린 backing device
 *   가 많이 사용되고 있습니다. 유일한 옵션은 회수 컨텍스트에서 조정되는 것인데,
 *   더티 프로세스가 balance_dirty_pages()가 관리하는 것과 동일한 방식으로
 *   조정된다는 보장이 없기 때문에 이상적이지 않습니다.
 *
 *  노드에 PGDAT_WRITHBACK 플래그가 지정되면 kswapd는 즉시 회수 플래그가 지정된
 *  페이지 아래의 페이지 수를 세고 아래의 nr_immediate 검사에서 해당 페이지 수가
 *  발견되면 중지합니다.
 *
 * - backing device속도가 느리거나 요청등이 많은 이유로 node가 busy인것을 표시한다.
 */
		if (sc->nr.writeback && sc->nr.writeback == sc->nr.taken)
			set_bit(PGDAT_WRITEBACK, &pgdat->flags);

		/* Allow kswapd to start writing pages during reclaim.*/
		if (sc->nr.unqueued_dirty == sc->nr.file_taken)
			set_bit(PGDAT_DIRTY, &pgdat->flags);

		/*
		 * If kswapd scans pages marked for immediate
		 * reclaim and under writeback (nr_immediate), it
		 * implies that pages are cycling through the LRU
		 * faster than they are written so also forcibly stall.
		 */
/*
 * IAMROOT, 2022.05.07:
 * - rotate이후에도 node가 너무 바쁘다. 현재 task(kswpad)를 0.1 sec sleep
 */
		if (sc->nr.immediate)
			congestion_wait(BLK_RW_ASYNC, HZ/10);
	}

	/*
	 * Tag a node/memcg as congested if all the dirty pages
	 * scanned were backed by a congested BDI and
	 * wait_iff_congested will stall.
	 *
	 * Legacy memcg will stall in page writeback so avoid forcibly
	 * stalling in wait_iff_congested().
	 */

/*
 * IAMROOT, 2022.05.07:
 * - reclaimer가 kswpad이거나 writeback_throttling_sane을 지원하는 reclaimer일때
 *   congested상태(rotate중인 상황)이면 lruvec에다가 LRUVEC_CONGESTED를 set한다.
 */
	if ((current_is_kswapd() ||
	     (cgroup_reclaim(sc) && writeback_throttling_sane(sc))) &&
	    sc->nr.dirty && sc->nr.dirty == sc->nr.congested)
		set_bit(LRUVEC_CONGESTED, &target_lruvec->flags);

	/*
	 * Stall direct reclaim for IO completions if underlying BDIs
	 * and node is congested. Allow kswapd to continue until it
	 * starts encountering unqueued dirty pages or cycling through
	 * the LRU too quickly.
	 */

/*
 * IAMROOT, 2022.05.07:
 * - direct reclaim이고, current_may_throttle상태, 절전모드가 아니고, lruvec이
 *   congested상태라면 0.1초를 쉰다.
 */
	if (!current_is_kswapd() && current_may_throttle() &&
	    !sc->hibernation_mode &&
	    test_bit(LRUVEC_CONGESTED, &target_lruvec->flags))
		wait_iff_congested(BLK_RW_ASYNC, HZ/10);

	if (should_continue_reclaim(pgdat, sc->nr_reclaimed - nr_reclaimed,
				    sc))
		goto again;

	/*
	 * Kswapd gives up on balancing particular nodes after too
	 * many failures to reclaim anything from them and goes to
	 * sleep. On reclaim progress, reset the failure counter. A
	 * successful direct reclaim run will revive a dormant kswapd.
	 */
	if (reclaimable)
		pgdat->kswapd_failures = 0;
}

/*
 * Returns true if compaction should go ahead for a costly-order request, or
 * the allocation would already succeed without compaction. Return false if we
 * should reclaim first.
 */
/*
 * IAMROOT, 2022.04.16:
 * @return true면 compaction 가능한 상황. false면 reclaim을 해야되는 상황.
 * - constly order에 대해서 reclaim을 하기전에 compact를 할 수 있는지 검사한다.
 */
static inline bool compaction_ready(struct zone *zone, struct scan_control *sc)
{
	unsigned long watermark;
	enum compact_result suitable;

/*
 * IAMROOT, 2022.04.26:
 * - alloc_flags 인자를 0으로 넘긴다.(일반 상황, 일반적이지 않은 예: GFP_ATOMIC)
 *   watermark는 min으로 사용하겠다는것.
 *   ALLOC_HIGH, ALLOC_OOM, ALLOC_CMA요청등을 안함으로 watermark min을 줄이거나
 *   CMA, HIGHATOMIC buddy를 탐색하지 않는다.
 */
	suitable = compaction_suitable(zone, sc->order, 0, sc->reclaim_idx);
	if (suitable == COMPACT_SUCCESS)
		/* Allocation should succeed already. Don't reclaim. */
		return true;
	if (suitable == COMPACT_SKIPPED)
		/* Compaction cannot yet proceed. Do reclaim. */
		return false;

	/*
	 * Compaction is already possible, but it takes time to run and there
	 * are potentially other callers using the pages just freed. so proceed
	 * with reclaim to make a buffer of free pages available to give
	 * compaction a reasonable chance of completing and allocating the page.
	 * Note that we won't actually reclaim the whole buffer in one attempt
	 * as the target watermark in should_continue_reclaim() is lower. but if
	 * we are already above the high+gap watermark, don't reclaim at all.
	 */
/*
 * IAMROOT, 2022.04.16:
 * - papago
 *   compaction은 이미 가능하지만 실행하는데 시간이 걸리고 방금 해방된 페이지를
 *   사용하는 다른 발신자가 있을 수 있습니다. 따라서 reclaim 작업을 진행하여
 *   페이지를 완료하고 할당할 수 있는 적절한 기회를 compaction 할 수 있는
 *   사용 가능한 페이지의 버퍼를 만드십시오.
 *   should_continue_reclaim의 target watermark가 더 낮기 때문에 실제로
 *   전체 버퍼를 한 번에 회수하지는 않습니다. 그러나 이미 high+gap watermark
 *   위에 있는 경우에는 전혀 회수하지 마십시오.
 *
 * - suitable == COMPACT_CONTINUE 
 *   compaction을 해야되는 상황이긴 한데, reclaim을 안하기 위해 다시 한번
 *   high wmarkmark를 사용해 조건을 완화해보고 검사를 해본다.
 *   reclaim이 오래걸리는 작업이다 보니 현재 상황이 low ~ high 사이 근처라면
 *   그 사이에 memory가 풀릴수도 있으니 다음 검사 주기때는 compact로 할 수도
 *   있다고 생각하기 때문에 최대한 reclaim을 안해볼려고 이렇게 조건을 높여
 *   skip 시도를 해본다.
 */
	watermark = high_wmark_pages(zone) + compact_gap(sc->order);

	return zone_watermark_ok_safe(zone, 0, watermark, sc->reclaim_idx);
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 */
/*
 * IAMROOT, 2022.05.07:
 * - @zonelist에 대해서 reclaim을 수행한다.
 */
static void shrink_zones(struct zonelist *zonelist, struct scan_control *sc)
{
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	gfp_t orig_mask;
	pg_data_t *last_pgdat = NULL;

	/*
	 * If the number of buffer_heads in the machine exceeds the maximum
	 * allowed level, force direct reclaim to scan the highmem zone as
	 * highmem pages could be pinning lowmem pages storing buffer_heads
	 */
	orig_mask = sc->gfp_mask;
/*
 * IAMROOT, 2022.04.16:
 * - buffer heads over인 상황이면 highmem까지 포함시키고 reclaim_idx를 갱신한다.
 */
	if (buffer_heads_over_limit) {
		sc->gfp_mask |= __GFP_HIGHMEM;
		sc->reclaim_idx = gfp_zone(sc->gfp_mask);
	}

	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					sc->reclaim_idx, sc->nodemask) {
		/*
		 * Take care memory controller reclaiming has small influence
		 * to global LRU.
		 */
		if (!cgroup_reclaim(sc)) {
/*
 * IAMROOT, 2022.04.16:
 * - __GFP_HARDWALL을 요청하고(현재 task에 대해서만 확인한다는것.)
 *   cpuset_zone_allowed를 확인한다. 다음의 상황에서
 *   for문을 계속진행하고 아니면 continue가 된다. (__cpuset_node_allowed 참고)
 *   1. intrrupt context인 경우
 *   2. zone이 속한 node가 현재 task에서 허락한 node인 경우
 *   3. oom중인 경우
 */
			if (!cpuset_zone_allowed(zone,
						 GFP_KERNEL | __GFP_HARDWALL))
				continue;
			/*
			 * If we already have plenty of memory free for
			 * compaction in this zone, don't free any more.
			 * Even though compaction is invoked for any
			 * non-zero order, only frequent costly order
			 * reclamation is disruptive enough to become a
			 * noticeable problem, like transparent huge
			 * page allocations.
			 */
/*
 * IAMROOT, 2022.04.16:
 * - 현재 zone이 compaction을 할 수 있는 상황인지 여기서 한번 검사한다.
 *   가능하면 compaction_ready를 true로 하고 continue한다.
 */
			if (IS_ENABLED(CONFIG_COMPACTION) &&
			    sc->order > PAGE_ALLOC_COSTLY_ORDER &&
			    compaction_ready(zone, sc)) {
				sc->compaction_ready = true;
				continue;
			}

			/*
			 * Shrink each node in the zonelist once. If the
			 * zonelist is ordered by zone (not the default) then a
			 * node may be shrunk multiple times but in that case
			 * the user prefers lower zones being preserved.
			 */
/*
 * IAMROOT, 2022.04.16:
 * - 한번 했던 node는 skip
 */
			if (zone->zone_pgdat == last_pgdat)
				continue;

			/*
			 * This steals pages from memory cgroups over softlimit
			 * and returns the number of reclaimed pages and
			 * scanned pages. This works for global memory pressure
			 * and balancing, not for a memcg's limit.
			 */
			nr_soft_scanned = 0;
			nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone->zone_pgdat,
						sc->order, sc->gfp_mask,
						&nr_soft_scanned);
			sc->nr_reclaimed += nr_soft_reclaimed;
			sc->nr_scanned += nr_soft_scanned;
			/* need some check for avoid more shrink_zone() */
		}

		/* See comment about same check for global reclaim above */
		if (zone->zone_pgdat == last_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;
		shrink_node(zone->zone_pgdat, sc);
	}

	/*
	 * Restore to original mask to avoid the impact on the caller if we
	 * promoted it to __GFP_HIGHMEM.
	 */
	sc->gfp_mask = orig_mask;
}

/*
 * IAMROOT, 2022.05.07:
 * - @target_memcg, @pgdat에 대한 lruvec에 refault 통계값을 적용한다.
 *   refault가 변경됬는지에 대한 여부를 검사하는데 사용한다.
 */
static void snapshot_refaults(struct mem_cgroup *target_memcg, pg_data_t *pgdat)
{
	struct lruvec *target_lruvec;
	unsigned long refaults;

	target_lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_ANON);
	target_lruvec->refaults[0] = refaults;
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_FILE);
	target_lruvec->refaults[1] = refaults;
}

/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  We kick the writeback threads and take explicit
 * naps in the hope that some of these pages can be written.  But if the
 * allocating task holds filesystem locks which prevent writeout this might not
 * work, and the allocation attempt will fail.
 *
 * returns:	0, if no pages reclaimed
 * 		else, the number of pages reclaimed
 */
/*
 * IAMROOT, 2022.05.07:
 * - @zonelist에 대한 reclaim을 수행한다.
 */
static unsigned long do_try_to_free_pages(struct zonelist *zonelist,
					  struct scan_control *sc)
{
	int initial_priority = sc->priority;
	pg_data_t *last_pgdat;
	struct zoneref *z;
	struct zone *zone;
retry:
	delayacct_freepages_start();

	if (!cgroup_reclaim(sc))
		__count_zid_vm_events(ALLOCSTALL, sc->reclaim_idx, 1);

	do {
/*
 * IAMROOT, 2022.04.16:
 * - priority에 따라서 vmpressure event를 동작시킨다.
 */
		vmpressure_prio(sc->gfp_mask, sc->target_mem_cgroup,
				sc->priority);
		sc->nr_scanned = 0;
		shrink_zones(zonelist, sc);

		if (sc->nr_reclaimed >= sc->nr_to_reclaim)
			break;

		if (sc->compaction_ready)
			break;

		/*
		 * If we're getting trouble reclaiming, start doing
		 * writepage even in laptop mode.
		 */
/*
 * IAMROOT, 2022.04.26:
 * - vmpressure_level_critical_prio 주석 참고
 */
		if (sc->priority < DEF_PRIORITY - 2)
			sc->may_writepage = 1;
	} while (--sc->priority >= 0);

	last_pgdat = NULL;
	for_each_zone_zonelist_nodemask(zone, z, zonelist, sc->reclaim_idx,
					sc->nodemask) {
		if (zone->zone_pgdat == last_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;

		snapshot_refaults(sc->target_mem_cgroup, zone->zone_pgdat);

		if (cgroup_reclaim(sc)) {
			struct lruvec *lruvec;

			lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup,
						   zone->zone_pgdat);
			clear_bit(LRUVEC_CONGESTED, &lruvec->flags);
		}
	}

	delayacct_freepages_end();

	if (sc->nr_reclaimed)
		return sc->nr_reclaimed;

	/* Aborted reclaim to try compaction? don't OOM, then */
	if (sc->compaction_ready)
		return 1;

	/*
	 * We make inactive:active ratio decisions based on the node's
	 * composition of memory, but a restrictive reclaim_idx or a
	 * memory.low cgroup setting can exempt large amounts of
	 * memory from reclaim. Neither of which are very common, so
	 * instead of doing costly eligibility calculations of the
	 * entire cgroup subtree up front, we assume the estimates are
	 * good, and retry with forcible deactivation if that fails.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - papgo
 *   노드의 메모리 구성을 기반으로 inactive:active 비율을 결정하지만 제한적인
 *   reclaim_idx 또는 memory.low cgroup 설정은 많은 양의 메모리를 회수할 수 없게
 *   할 수 있습니다. 둘 다 매우 흔하지 않기 때문에 전체 cgroup 하위 트리에 대해
 *   비용이 많이 드는 적격성 계산을 수행하는 대신 추정치가 양호하다고 가정하고
 *   실패하면 강제 비활성화를 통해 재시도한다.
 */
	if (sc->skipped_deactivate) {
		sc->priority = initial_priority;
		sc->force_deactivate = 1;
		sc->skipped_deactivate = 0;
		goto retry;
	}

	/* Untapped cgroup reserves?  Don't OOM, retry. */
	if (sc->memcg_low_skipped) {
		sc->priority = initial_priority;
		sc->force_deactivate = 0;
		sc->memcg_low_reclaim = 1;
		sc->memcg_low_skipped = 0;
		goto retry;
	}

	return 0;
}

/*
 * IAMROOT, 2022.04.16:
 * - direct reclaim 허락 여부를 판단한다.
 *   direct reclaim 을 못한는 상황(memory가 꽤 부족)이면 kswapd를 깨운다.
 */
static bool allow_direct_reclaim(pg_data_t *pgdat)
{
	struct zone *zone;
	unsigned long pfmemalloc_reserve = 0;
	unsigned long free_pages = 0;
	int i;
	bool wmark_ok;

	if (pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES)
		return true;

/*
 * IAMROOT, 2022.04.16:
 * - reclaim page가 존재하는 zone을 찾아 min_wmark와 free_page를 각각 합산한다.
 */
	for (i = 0; i <= ZONE_NORMAL; i++) {
		zone = &pgdat->node_zones[i];
		if (!managed_zone(zone))
			continue;

		if (!zone_reclaimable_pages(zone))
			continue;

		pfmemalloc_reserve += min_wmark_pages(zone);
		free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}

/*
 * IAMROOT, 2022.04.16:
 * - reclaim page가 없다면 direct reclaim을 허락한다.
 */
	/* If there are no reserves (unexpected config) then do not throttle */
	if (!pfmemalloc_reserve)
		return true;

/*
 * IAMROOT, 2022.04.16:
 * - 남은 reserve의 절반이상의 free_pages가 존재하면 direct reclaim을 허락한다.
 *   그게 아니면 kswapd를 깨운다.
 *   즉 memory가 많이 부족해지면 kswpad를 깨운다.
 */
	wmark_ok = free_pages > pfmemalloc_reserve / 2;

	/* kswapd must be awake if processes are being throttled */
	if (!wmark_ok && waitqueue_active(&pgdat->kswapd_wait)) {
/*
 * IAMROOT, 2022.04.16:
 * - kswapd를 깨우기전 kswapd_highest_zoneidx가 ZONE_NORMAL보다 크다면
 *   ZONE_NORMAL로 제한한다.
 */
		if (READ_ONCE(pgdat->kswapd_highest_zoneidx) > ZONE_NORMAL)
			WRITE_ONCE(pgdat->kswapd_highest_zoneidx, ZONE_NORMAL);

		wake_up_interruptible(&pgdat->kswapd_wait);
	}

	return wmark_ok;
}

/*
 * Throttle direct reclaimers if backing storage is backed by the network
 * and the PFMEMALLOC reserve for the preferred node is getting dangerously
 * depleted. kswapd will continue to make progress and wake the processes
 * when the low watermark is reached.
 *
 * Returns true if a fatal signal was delivered during throttling. If this
 * happens, the page allocator should not consider triggering the OOM killer.
 */
/*
 * IAMROOT, 2022.04.16:
 * - papgo
 *   backing storage가 네트워크에 의해 backing되고 기본 노드에 대한 PFMEMALLOC 
 *   약이 위험할 정도로 고갈되는 경우 직접 회수기를 조절합니다.
 *   kswapd는 계속 진행되며 낮은 워터마크에 도달하면 프로세스를 웨이크업합니다.
 *
 *   스로틀 중에 치명적인 신호가 전달되면 true를 반환합니다. 
 *   이 경우 페이지 할당자는 OOM 킬러를 트리거하는 것을 고려해서는 안 됩니다.
 *
 * - network swap이 일어날경우 network packet을 만들어야 되는데 이때도 할당이
 *   필요하다. 할당을 하기위해 memory를 너무많이 사용하면 또 부하가 걸리기 때문에
 *   이런 상황에서 reclaim을 막는다.
 *
 * - reclaim을 너무 자주하지 않게 하기위해 throttling을 수행한다.
 *   direct reclaim이 가능한등의 조건이면 throttling을 하지 않고, 그게 아니라
 *   실제 reclaim memory가 없으면 throttling을 수행한다.
 *
 * - @return true이면 direct reclaim을 할 필요가 없는 상황.
 */
static bool throttle_direct_reclaim(gfp_t gfp_mask, struct zonelist *zonelist,
					nodemask_t *nodemask)
{
	struct zoneref *z;
	struct zone *zone;
	pg_data_t *pgdat = NULL;

	/*
	 * Kernel threads should not be throttled as they may be indirectly
	 * responsible for cleaning pages necessary for reclaim to make forward
	 * progress. kjournald for example may enter direct reclaim while
	 * committing a transaction where throttling it could forcing other
	 * processes to block on log_wait_commit().
	 */
/*
 * IAMROOT, 2022.04.16:
 * - kernel thread는 throttling도 안하고 direct reclaim도 안한다.
 */
	if (current->flags & PF_KTHREAD)
		goto out;

	/*
	 * If a fatal signal is pending, this process should not throttle.
	 * It should return quickly so it can exit and free its memory
	 */
/*
 * IAMROOT, 2022.04.16:
 * - fatal signal도 throttling도 안하고 direct reclaim도 안한다.
 */
	if (fatal_signal_pending(current))
		goto out;

	/*
	 * Check if the pfmemalloc reserves are ok by finding the first node
	 * with a usable ZONE_NORMAL or lower zone. The expectation is that
	 * GFP_KERNEL will be required for allocating network buffers when
	 * swapping over the network so ZONE_HIGHMEM is unusable.
	 *
	 * Throttling is based on the first usable node and throttled processes
	 * wait on a queue until kswapd makes progress and wakes them. There
	 * is an affinity then between processes waking up and where reclaim
	 * progress has been made assuming the process wakes on the same node.
	 * More importantly, processes running on remote nodes will not compete
	 * for remote pfmemalloc reserves and processes on different nodes
	 * should make reasonable progress.
	 */
/*
 * IAMROOT, 2022.04.16:
 * - papago
 *   사용 가능한 ZONE_NORMAL 또는 그보다 낮은 존을 가진 첫 번째 노드를 찾아
 *   pfmemalloc reserve가 정상인지 확인한다. 네트워크를 통해 스왑할 때
 *   네트워크 버퍼를 할당하기 위해 GFP_KERNEL이 필요하므로 ZONE_HIGHMEM을
 *   사용할 수 없습니다.
 *
 *   throttling은 사용 가능한 첫 번째 노드를 기반으로 하며 조절된 프로세스는
 *   kswapd가 진행하여 절전 모드를 해제할 때까지 대기열에 대기합니다.
 *   프로세스 웨이크업과 프로세스가 동일한 노드에서 웨이크업한다고 가정했을 때
 *   회수 진행 상황 사이에는 유사성이 있습니다.
 *   더 중요한 것은 remote 노드에서 실행되는 프로세스가 remote pfmemalloc
 *   reserve에 대해 경쟁하지 않을 것이며 다른 노드에서 프로세스가 합리적으로
 *   진행되어야 한다는 것이다. 
 *
 * - ZONE_NORMAL이하를 가진 첫번째 node를 찾아가 direct reclaim을
 *   허용했으면 제한을 안둔다.
 */
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(gfp_mask), nodemask) {
		if (zone_idx(zone) > ZONE_NORMAL)
			continue;

		/* Throttle based on the first usable node */
		pgdat = zone->zone_pgdat;
		if (allow_direct_reclaim(pgdat))
			goto out;
		break;
	}

	/* If no zone was usable by the allocation flags then do not throttle */
/*
 * IAMROOT, 2022.04.16:
 * - ZONE_NORMAL 이하의 zone이 없는 경우. throttling, direct reclaim을 안한다.
 */
	if (!pgdat)
		goto out;

/*
 * IAMROOT, 2022.04.16:
 * - 여기부터 throttling을 시작하겠다는것.
 */
	/* Account for the throttling */
	count_vm_event(PGSCAN_DIRECT_THROTTLE);

	/*
	 * If the caller cannot enter the filesystem, it's possible that it
	 * is due to the caller holding an FS lock or performing a journal
	 * transaction in the case of a filesystem like ext[3|4]. In this case,
	 * it is not safe to block on pfmemalloc_wait as kswapd could be
	 * blocked waiting on the same lock. Instead, throttle for up to a
	 * second before continuing.
	 */
/*
 * IAMROOT, 2022.04.16:
 * - 실제 sleep되는 구간. fs의 경우가 아니면(ex. memory) 최대 1초,
 *   그게 아니면 direct reclaim이 가능할때까지 기다린다.
 */
	if (!(gfp_mask & __GFP_FS))
		wait_event_interruptible_timeout(pgdat->pfmemalloc_wait,
			allow_direct_reclaim(pgdat), HZ);
	else
		/* Throttle until kswapd wakes the process */
		wait_event_killable(zone->zone_pgdat->pfmemalloc_wait,
			allow_direct_reclaim(pgdat));

/*
 * IAMROOT, 2022.04.16:
 *
 * - throttle 중에 signal을 받아 중지 됬다면 direct reclaim을 하지않는다.
 *   true 리턴 될경우 상위에서 progress가 1로 시작한다.
 *
 * - Git blame에서 process가 throttle상태에서 kill을 당한다면 OOM killer를
 *   촉발하지 않아야 된다고 한다. system에 OOM인 상황에서 프로세스가
 *   종료되는 경우 __alloc_pages_slowpath에서 loop하며 loop도중 direct reclaim을
 *   호출 할수 있다고 한다.
 *   return값을 true로 함으로써 progress를 1로 전진시킴으로써 이러한 deadlock을
 *   빠져나갈수 있다고 한다.
 */
	if (fatal_signal_pending(current))
		return true;

out:
	return false;
}

/*
 * IAMROOT, 2022.04.16:
 * @return reclaim 한 page 개수
 * - direct reclaim을 수행하는데, 만약 memory가 너무 적을 경우 throttle을 수행한다.
 */
unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
				gfp_t gfp_mask, nodemask_t *nodemask)
{
	unsigned long nr_reclaimed;
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.gfp_mask = current_gfp_context(gfp_mask),
		.reclaim_idx = gfp_zone(gfp_mask),
		.order = order,
		.nodemask = nodemask,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = 1,
	};

	/*
	 * scan_control uses s8 fields for order, priority, and reclaim_idx.
	 * Confirm they are large enough for max values.
	 */
	BUILD_BUG_ON(MAX_ORDER > S8_MAX);
	BUILD_BUG_ON(DEF_PRIORITY > S8_MAX);
	BUILD_BUG_ON(MAX_NR_ZONES > S8_MAX);

	/*
	 * Do not enter reclaim if fatal signal was delivered while throttled.
	 * 1 is returned so that the page allocator does not OOM kill at this
	 * point.
	 */
/*
 * IAMROOT, 2022.04.16:
 * - 실제 throttle 수행 및 direct reclaim여부를 판단한다.
 */
	if (throttle_direct_reclaim(sc.gfp_mask, zonelist, nodemask))
		return 1;

/*
 * IAMROOT, 2022.04.16:
 * - direct reclaim을 수행한다.
 */
	set_task_reclaim_state(current, &sc.reclaim_state);
	trace_mm_vmscan_direct_reclaim_begin(order, sc.gfp_mask);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	trace_mm_vmscan_direct_reclaim_end(nr_reclaimed);
	set_task_reclaim_state(current, NULL);

	return nr_reclaimed;
}

#ifdef CONFIG_MEMCG

/* Only used by soft limit reclaim. Do not reuse for anything else. */
unsigned long mem_cgroup_shrink_node(struct mem_cgroup *memcg,
						gfp_t gfp_mask, bool noswap,
						pg_data_t *pgdat,
						unsigned long *nr_scanned)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.target_mem_cgroup = memcg,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.may_swap = !noswap,
	};

	WARN_ON_ONCE(!current->reclaim_state);

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);

	trace_mm_vmscan_memcg_softlimit_reclaim_begin(sc.order,
						      sc.gfp_mask);

	/*
	 * NOTE: Although we can get the priority field, using it
	 * here is not a good idea, since it limits the pages we can scan.
	 * if we don't reclaim here, the shrink_node from balance_pgdat
	 * will pick up pages from other mem cgroup's as well. We hack
	 * the priority and make it zero.
	 */
	shrink_lruvec(lruvec, &sc);

	trace_mm_vmscan_memcg_softlimit_reclaim_end(sc.nr_reclaimed);

	*nr_scanned = sc.nr_scanned;

	return sc.nr_reclaimed;
}

unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
					   unsigned long nr_pages,
					   gfp_t gfp_mask,
					   bool may_swap)
{
	unsigned long nr_reclaimed;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = (current_gfp_context(gfp_mask) & GFP_RECLAIM_MASK) |
				(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK),
		.reclaim_idx = MAX_NR_ZONES - 1,
		.target_mem_cgroup = memcg,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = may_swap,
	};
	/*
	 * Traverse the ZONELIST_FALLBACK zonelist of the current node to put
	 * equal pressure on all the nodes. This is based on the assumption that
	 * the reclaim does not bail out early.
	 */
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);

	set_task_reclaim_state(current, &sc.reclaim_state);
	trace_mm_vmscan_memcg_reclaim_begin(0, sc.gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	memalloc_noreclaim_restore(noreclaim_flag);
	trace_mm_vmscan_memcg_reclaim_end(nr_reclaimed);
	set_task_reclaim_state(current, NULL);

	return nr_reclaimed;
}
#endif

static void age_active_anon(struct pglist_data *pgdat,
				struct scan_control *sc)
{
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;

	if (!can_age_anon_pages(pgdat, sc))
		return;

	lruvec = mem_cgroup_lruvec(NULL, pgdat);
	if (!inactive_is_low(lruvec, LRU_INACTIVE_ANON))
		return;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		lruvec = mem_cgroup_lruvec(memcg, pgdat);
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);
		memcg = mem_cgroup_iter(NULL, memcg, NULL);
	} while (memcg);
}

static bool pgdat_watermark_boosted(pg_data_t *pgdat, int highest_zoneidx)
{
	int i;
	struct zone *zone;

	/*
	 * Check for watermark boosts top-down as the higher zones
	 * are more likely to be boosted. Both watermarks and boosts
	 * should not be checked at the same time as reclaim would
	 * start prematurely when there is no boosting and a lower
	 * zone is balanced.
	 */
	for (i = highest_zoneidx; i >= 0; i--) {
		zone = pgdat->node_zones + i;
		if (!managed_zone(zone))
			continue;

		if (zone->watermark_boost)
			return true;
	}

	return false;
}

/*
 * Returns true if there is an eligible zone balanced for the request order
 * and highest_zoneidx
 */
static bool pgdat_balanced(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	int i;
	unsigned long mark = -1;
	struct zone *zone;

	/*
	 * Check watermarks bottom-up as lower zones are more likely to
	 * meet watermarks.
	 */
	for (i = 0; i <= highest_zoneidx; i++) {
		zone = pgdat->node_zones + i;

		if (!managed_zone(zone))
			continue;

		mark = high_wmark_pages(zone);
		if (zone_watermark_ok_safe(zone, order, mark, highest_zoneidx))
			return true;
	}

	/*
	 * If a node has no populated zone within highest_zoneidx, it does not
	 * need balancing by definition. This can happen if a zone-restricted
	 * allocation tries to wake a remote kswapd.
	 */
	if (mark == -1)
		return true;

	return false;
}

/* Clear pgdat state for congested, dirty or under writeback. */
static void clear_pgdat_congested(pg_data_t *pgdat)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(NULL, pgdat);

	clear_bit(LRUVEC_CONGESTED, &lruvec->flags);
	clear_bit(PGDAT_DIRTY, &pgdat->flags);
	clear_bit(PGDAT_WRITEBACK, &pgdat->flags);
}

/*
 * Prepare kswapd for sleeping. This verifies that there are no processes
 * waiting in throttle_direct_reclaim() and that watermarks have been met.
 *
 * Returns true if kswapd is ready to sleep
 */
static bool prepare_kswapd_sleep(pg_data_t *pgdat, int order,
				int highest_zoneidx)
{
	/*
	 * The throttled processes are normally woken up in balance_pgdat() as
	 * soon as allow_direct_reclaim() is true. But there is a potential
	 * race between when kswapd checks the watermarks and a process gets
	 * throttled. There is also a potential race if processes get
	 * throttled, kswapd wakes, a large process exits thereby balancing the
	 * zones, which causes kswapd to exit balance_pgdat() before reaching
	 * the wake up checks. If kswapd is going to sleep, no process should
	 * be sleeping on pfmemalloc_wait, so wake them now if necessary. If
	 * the wake up is premature, processes will wake kswapd and get
	 * throttled again. The difference from wake ups in balance_pgdat() is
	 * that here we are under prepare_to_wait().
	 */
	if (waitqueue_active(&pgdat->pfmemalloc_wait))
		wake_up_all(&pgdat->pfmemalloc_wait);

	/* Hopeless node, leave it to direct reclaim */
	if (pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES)
		return true;

	if (pgdat_balanced(pgdat, order, highest_zoneidx)) {
		clear_pgdat_congested(pgdat);
		return true;
	}

	return false;
}

/*
 * kswapd shrinks a node of pages that are at or below the highest usable
 * zone that is currently unbalanced.
 *
 * Returns true if kswapd scanned at least the requested number of pages to
 * reclaim or if the lack of progress was due to pages under writeback.
 * This is used to determine if the scanning priority needs to be raised.
 */
static bool kswapd_shrink_node(pg_data_t *pgdat,
			       struct scan_control *sc)
{
	struct zone *zone;
	int z;

	/* Reclaim a number of pages proportional to the number of zones */
	sc->nr_to_reclaim = 0;
	for (z = 0; z <= sc->reclaim_idx; z++) {
		zone = pgdat->node_zones + z;
		if (!managed_zone(zone))
			continue;

		sc->nr_to_reclaim += max(high_wmark_pages(zone), SWAP_CLUSTER_MAX);
	}

	/*
	 * Historically care was taken to put equal pressure on all zones but
	 * now pressure is applied based on node LRU order.
	 */
	shrink_node(pgdat, sc);

	/*
	 * Fragmentation may mean that the system cannot be rebalanced for
	 * high-order allocations. If twice the allocation size has been
	 * reclaimed then recheck watermarks only at order-0 to prevent
	 * excessive reclaim. Assume that a process requested a high-order
	 * can direct reclaim/compact.
	 */
	if (sc->order && sc->nr_reclaimed >= compact_gap(sc->order))
		sc->order = 0;

	return sc->nr_scanned >= sc->nr_to_reclaim;
}

/* Page allocator PCP high watermark is lowered if reclaim is active. */
static inline void
update_reclaim_active(pg_data_t *pgdat, int highest_zoneidx, bool active)
{
	int i;
	struct zone *zone;

	for (i = 0; i <= highest_zoneidx; i++) {
		zone = pgdat->node_zones + i;

		if (!managed_zone(zone))
			continue;

		if (active)
			set_bit(ZONE_RECLAIM_ACTIVE, &zone->flags);
		else
			clear_bit(ZONE_RECLAIM_ACTIVE, &zone->flags);
	}
}

static inline void
set_reclaim_active(pg_data_t *pgdat, int highest_zoneidx)
{
	update_reclaim_active(pgdat, highest_zoneidx, true);
}

static inline void
clear_reclaim_active(pg_data_t *pgdat, int highest_zoneidx)
{
	update_reclaim_active(pgdat, highest_zoneidx, false);
}

/*
 * For kswapd, balance_pgdat() will reclaim pages across a node from zones
 * that are eligible for use by the caller until at least one zone is
 * balanced.
 *
 * Returns the order kswapd finished reclaiming at.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > high_wmark_pages(zone), but once a zone is
 * found to have free_pages <= high_wmark_pages(zone), any page in that zone
 * or lower is eligible for reclaim until at least one usable zone is
 * balanced.
 */
static int balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	int i;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	unsigned long pflags;
	unsigned long nr_boost_reclaim;
	unsigned long zone_boosts[MAX_NR_ZONES] = { 0, };
	bool boosted;
	struct zone *zone;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.order = order,
		.may_unmap = 1,
	};

	set_task_reclaim_state(current, &sc.reclaim_state);
	psi_memstall_enter(&pflags);
	__fs_reclaim_acquire(_THIS_IP_);

	count_vm_event(PAGEOUTRUN);

	/*
	 * Account for the reclaim boost. Note that the zone boost is left in
	 * place so that parallel allocations that are near the watermark will
	 * stall or direct reclaim until kswapd is finished.
	 */
	nr_boost_reclaim = 0;
	for (i = 0; i <= highest_zoneidx; i++) {
		zone = pgdat->node_zones + i;
		if (!managed_zone(zone))
			continue;

		nr_boost_reclaim += zone->watermark_boost;
		zone_boosts[i] = zone->watermark_boost;
	}
	boosted = nr_boost_reclaim;

restart:
	set_reclaim_active(pgdat, highest_zoneidx);
	sc.priority = DEF_PRIORITY;
	do {
		unsigned long nr_reclaimed = sc.nr_reclaimed;
		bool raise_priority = true;
		bool balanced;
		bool ret;

		sc.reclaim_idx = highest_zoneidx;

		/*
		 * If the number of buffer_heads exceeds the maximum allowed
		 * then consider reclaiming from all zones. This has a dual
		 * purpose -- on 64-bit systems it is expected that
		 * buffer_heads are stripped during active rotation. On 32-bit
		 * systems, highmem pages can pin lowmem memory and shrinking
		 * buffers can relieve lowmem pressure. Reclaim may still not
		 * go ahead if all eligible zones for the original allocation
		 * request are balanced to avoid excessive reclaim from kswapd.
		 */
		if (buffer_heads_over_limit) {
			for (i = MAX_NR_ZONES - 1; i >= 0; i--) {
				zone = pgdat->node_zones + i;
				if (!managed_zone(zone))
					continue;

				sc.reclaim_idx = i;
				break;
			}
		}

		/*
		 * If the pgdat is imbalanced then ignore boosting and preserve
		 * the watermarks for a later time and restart. Note that the
		 * zone watermarks will be still reset at the end of balancing
		 * on the grounds that the normal reclaim should be enough to
		 * re-evaluate if boosting is required when kswapd next wakes.
		 */
		balanced = pgdat_balanced(pgdat, sc.order, highest_zoneidx);
		if (!balanced && nr_boost_reclaim) {
			nr_boost_reclaim = 0;
			goto restart;
		}

		/*
		 * If boosting is not active then only reclaim if there are no
		 * eligible zones. Note that sc.reclaim_idx is not used as
		 * buffer_heads_over_limit may have adjusted it.
		 */
		if (!nr_boost_reclaim && balanced)
			goto out;

		/* Limit the priority of boosting to avoid reclaim writeback */
		if (nr_boost_reclaim && sc.priority == DEF_PRIORITY - 2)
			raise_priority = false;

		/*
		 * Do not writeback or swap pages for boosted reclaim. The
		 * intent is to relieve pressure not issue sub-optimal IO
		 * from reclaim context. If no pages are reclaimed, the
		 * reclaim will be aborted.
		 */
		sc.may_writepage = !laptop_mode && !nr_boost_reclaim;
		sc.may_swap = !nr_boost_reclaim;

		/*
		 * Do some background aging of the anon list, to give
		 * pages a chance to be referenced before reclaiming. All
		 * pages are rotated regardless of classzone as this is
		 * about consistent aging.
		 */
		age_active_anon(pgdat, &sc);

		/*
		 * If we're getting trouble reclaiming, start doing writepage
		 * even in laptop mode.
		 */
		if (sc.priority < DEF_PRIORITY - 2)
			sc.may_writepage = 1;

		/* Call soft limit reclaim before calling shrink_node. */
		sc.nr_scanned = 0;
		nr_soft_scanned = 0;
		nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(pgdat, sc.order,
						sc.gfp_mask, &nr_soft_scanned);
		sc.nr_reclaimed += nr_soft_reclaimed;

		/*
		 * There should be no need to raise the scanning priority if
		 * enough pages are already being scanned that that high
		 * watermark would be met at 100% efficiency.
		 */
		if (kswapd_shrink_node(pgdat, &sc))
			raise_priority = false;

		/*
		 * If the low watermark is met there is no need for processes
		 * to be throttled on pfmemalloc_wait as they should not be
		 * able to safely make forward progress. Wake them
		 */
		if (waitqueue_active(&pgdat->pfmemalloc_wait) &&
				allow_direct_reclaim(pgdat))
			wake_up_all(&pgdat->pfmemalloc_wait);

		/* Check if kswapd should be suspending */
		__fs_reclaim_release(_THIS_IP_);
		ret = try_to_freeze();
		__fs_reclaim_acquire(_THIS_IP_);
		if (ret || kthread_should_stop())
			break;

		/*
		 * Raise priority if scanning rate is too low or there was no
		 * progress in reclaiming pages
		 */
		nr_reclaimed = sc.nr_reclaimed - nr_reclaimed;
		nr_boost_reclaim -= min(nr_boost_reclaim, nr_reclaimed);

		/*
		 * If reclaim made no progress for a boost, stop reclaim as
		 * IO cannot be queued and it could be an infinite loop in
		 * extreme circumstances.
		 */
		if (nr_boost_reclaim && !nr_reclaimed)
			break;

		if (raise_priority || !nr_reclaimed)
			sc.priority--;
	} while (sc.priority >= 1);

	if (!sc.nr_reclaimed)
		pgdat->kswapd_failures++;

out:
	clear_reclaim_active(pgdat, highest_zoneidx);

	/* If reclaim was boosted, account for the reclaim done in this pass */
	if (boosted) {
		unsigned long flags;

		for (i = 0; i <= highest_zoneidx; i++) {
			if (!zone_boosts[i])
				continue;

			/* Increments are under the zone lock */
			zone = pgdat->node_zones + i;
			spin_lock_irqsave(&zone->lock, flags);
			zone->watermark_boost -= min(zone->watermark_boost, zone_boosts[i]);
			spin_unlock_irqrestore(&zone->lock, flags);
		}

		/*
		 * As there is now likely space, wakeup kcompact to defragment
		 * pageblocks.
		 */
		wakeup_kcompactd(pgdat, pageblock_order, highest_zoneidx);
	}

	snapshot_refaults(NULL, pgdat);
	__fs_reclaim_release(_THIS_IP_);
	psi_memstall_leave(&pflags);
	set_task_reclaim_state(current, NULL);

	/*
	 * Return the order kswapd stopped reclaiming at as
	 * prepare_kswapd_sleep() takes it into account. If another caller
	 * entered the allocator slow path while kswapd was awake, order will
	 * remain at the higher level.
	 */
	return sc.order;
}

/*
 * The pgdat->kswapd_highest_zoneidx is used to pass the highest zone index to
 * be reclaimed by kswapd from the waker. If the value is MAX_NR_ZONES which is
 * not a valid index then either kswapd runs for first time or kswapd couldn't
 * sleep after previous reclaim attempt (node is still unbalanced). In that
 * case return the zone index of the previous kswapd reclaim cycle.
 */
static enum zone_type kswapd_highest_zoneidx(pg_data_t *pgdat,
					   enum zone_type prev_highest_zoneidx)
{
	enum zone_type curr_idx = READ_ONCE(pgdat->kswapd_highest_zoneidx);

	return curr_idx == MAX_NR_ZONES ? prev_highest_zoneidx : curr_idx;
}

static void kswapd_try_to_sleep(pg_data_t *pgdat, int alloc_order, int reclaim_order,
				unsigned int highest_zoneidx)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;

	prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);

	/*
	 * Try to sleep for a short interval. Note that kcompactd will only be
	 * woken if it is possible to sleep for a short interval. This is
	 * deliberate on the assumption that if reclaim cannot keep an
	 * eligible zone balanced that it's also unlikely that compaction will
	 * succeed.
	 */
	if (prepare_kswapd_sleep(pgdat, reclaim_order, highest_zoneidx)) {
		/*
		 * Compaction records what page blocks it recently failed to
		 * isolate pages from and skips them in the future scanning.
		 * When kswapd is going to sleep, it is reasonable to assume
		 * that pages and compaction may succeed so reset the cache.
		 */
		reset_isolation_suitable(pgdat);

		/*
		 * We have freed the memory, now we should compact it to make
		 * allocation of the requested order possible.
		 */
		wakeup_kcompactd(pgdat, alloc_order, highest_zoneidx);

		remaining = schedule_timeout(HZ/10);

		/*
		 * If woken prematurely then reset kswapd_highest_zoneidx and
		 * order. The values will either be from a wakeup request or
		 * the previous request that slept prematurely.
		 */
		if (remaining) {
			WRITE_ONCE(pgdat->kswapd_highest_zoneidx,
					kswapd_highest_zoneidx(pgdat,
							highest_zoneidx));

			if (READ_ONCE(pgdat->kswapd_order) < reclaim_order)
				WRITE_ONCE(pgdat->kswapd_order, reclaim_order);
		}

		finish_wait(&pgdat->kswapd_wait, &wait);
		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
	}

	/*
	 * After a short sleep, check if it was a premature sleep. If not, then
	 * go fully to sleep until explicitly woken up.
	 */
	if (!remaining &&
	    prepare_kswapd_sleep(pgdat, reclaim_order, highest_zoneidx)) {
		trace_mm_vmscan_kswapd_sleep(pgdat->node_id);

		/*
		 * vmstat counters are not perfectly accurate and the estimated
		 * value for counters such as NR_FREE_PAGES can deviate from the
		 * true value by nr_online_cpus * threshold. To avoid the zone
		 * watermarks being breached while under pressure, we reduce the
		 * per-cpu vmstat threshold while kswapd is awake and restore
		 * them before going back to sleep.
		 */
		set_pgdat_percpu_threshold(pgdat, calculate_normal_threshold);

		if (!kthread_should_stop())
			schedule();

		set_pgdat_percpu_threshold(pgdat, calculate_pressure_threshold);
	} else {
		if (remaining)
			count_vm_event(KSWAPD_LOW_WMARK_HIT_QUICKLY);
		else
			count_vm_event(KSWAPD_HIGH_WMARK_HIT_QUICKLY);
	}
	finish_wait(&pgdat->kswapd_wait, &wait);
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
static int kswapd(void *p)
{
	unsigned int alloc_order, reclaim_order;
	unsigned int highest_zoneidx = MAX_NR_ZONES - 1;
	pg_data_t *pgdat = (pg_data_t *)p;
	struct task_struct *tsk = current;
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	set_freezable();

	WRITE_ONCE(pgdat->kswapd_order, 0);
	WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);
	for ( ; ; ) {
		bool ret;

		alloc_order = reclaim_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);

kswapd_try_sleep:
		kswapd_try_to_sleep(pgdat, alloc_order, reclaim_order,
					highest_zoneidx);

		/* Read the new order and highest_zoneidx */
		alloc_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);
		WRITE_ONCE(pgdat->kswapd_order, 0);
		WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);

		ret = try_to_freeze();
		if (kthread_should_stop())
			break;

		/*
		 * We can speed up thawing tasks if we don't call balance_pgdat
		 * after returning from the refrigerator
		 */
		if (ret)
			continue;

		/*
		 * Reclaim begins at the requested order but if a high-order
		 * reclaim fails then kswapd falls back to reclaiming for
		 * order-0. If that happens, kswapd will consider sleeping
		 * for the order it finished reclaiming at (reclaim_order)
		 * but kcompactd is woken to compact for the original
		 * request (alloc_order).
		 */
		trace_mm_vmscan_kswapd_wake(pgdat->node_id, highest_zoneidx,
						alloc_order);
		reclaim_order = balance_pgdat(pgdat, alloc_order,
						highest_zoneidx);
		if (reclaim_order < alloc_order)
			goto kswapd_try_sleep;
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD);

	return 0;
}

/*
 * A zone is low on free memory or too fragmented for high-order memory.  If
 * kswapd should reclaim (direct reclaim is deferred), wake it up for the zone's
 * pgdat.  It will wake up kcompactd after reclaiming memory.  If kswapd reclaim
 * has failed or is not needed, still wake up kcompactd if only compaction is
 * needed.
 */
/*
 * IAMROOT, 2022.03.05:
 * - TODO
 * - kswapd를 wakeup 한다.
 */
void wakeup_kswapd(struct zone *zone, gfp_t gfp_flags, int order,
		   enum zone_type highest_zoneidx)
{
	pg_data_t *pgdat;
	enum zone_type curr_idx;

	if (!managed_zone(zone))
		return;

	if (!cpuset_zone_allowed(zone, gfp_flags))
		return;

	pgdat = zone->zone_pgdat;
	curr_idx = READ_ONCE(pgdat->kswapd_highest_zoneidx);

	if (curr_idx == MAX_NR_ZONES || curr_idx < highest_zoneidx)
		WRITE_ONCE(pgdat->kswapd_highest_zoneidx, highest_zoneidx);

	if (READ_ONCE(pgdat->kswapd_order) < order)
		WRITE_ONCE(pgdat->kswapd_order, order);

	if (!waitqueue_active(&pgdat->kswapd_wait))
		return;

	/* Hopeless node, leave it to direct reclaim if possible */
	if (pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES ||
	    (pgdat_balanced(pgdat, order, highest_zoneidx) &&
	     !pgdat_watermark_boosted(pgdat, highest_zoneidx))) {
		/*
		 * There may be plenty of free memory available, but it's too
		 * fragmented for high-order allocations.  Wake up kcompactd
		 * and rely on compaction_suitable() to determine if it's
		 * needed.  If it fails, it will defer subsequent attempts to
		 * ratelimit its work.
		 */
		if (!(gfp_flags & __GFP_DIRECT_RECLAIM))
			wakeup_kcompactd(pgdat, order, highest_zoneidx);
		return;
	}

	trace_mm_vmscan_wakeup_kswapd(pgdat->node_id, highest_zoneidx, order,
				      gfp_flags);
	wake_up_interruptible(&pgdat->kswapd_wait);
}

#ifdef CONFIG_HIBERNATION
/*
 * Try to free `nr_to_reclaim' of memory, system-wide, and return the number of
 * freed pages.
 *
 * Rather than trying to age LRUs the aim is to preserve the overall
 * LRU order by reclaiming preferentially
 * inactive > active > active referenced > active mapped
 */
unsigned long shrink_all_memory(unsigned long nr_to_reclaim)
{
	struct scan_control sc = {
		.nr_to_reclaim = nr_to_reclaim,
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.hibernation_mode = 1,
	};
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);
	unsigned long nr_reclaimed;
	unsigned int noreclaim_flag;

	fs_reclaim_acquire(sc.gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();
	set_task_reclaim_state(current, &sc.reclaim_state);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	set_task_reclaim_state(current, NULL);
	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(sc.gfp_mask);

	return nr_reclaimed;
}
#endif /* CONFIG_HIBERNATION */

/*
 * This kswapd start function will be called by init and node-hot-add.
 * On node-hot-add, kswapd will moved to proper cpus if cpus are hot-added.
 */
void kswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);

	if (pgdat->kswapd)
		return;

	pgdat->kswapd = kthread_run(kswapd, pgdat, "kswapd%d", nid);
	if (IS_ERR(pgdat->kswapd)) {
		/* failure at boot is fatal */
		BUG_ON(system_state < SYSTEM_RUNNING);
		pr_err("Failed to start kswapd on node %d\n", nid);
		pgdat->kswapd = NULL;
	}
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold mem_hotplug_begin/end().
 */
void kswapd_stop(int nid)
{
	struct task_struct *kswapd = NODE_DATA(nid)->kswapd;

	if (kswapd) {
		kthread_stop(kswapd);
		NODE_DATA(nid)->kswapd = NULL;
	}
}

static int __init kswapd_init(void)
{
	int nid;

	swap_setup();
	for_each_node_state(nid, N_MEMORY)
 		kswapd_run(nid);
	return 0;
}

module_init(kswapd_init)

#ifdef CONFIG_NUMA
/*
 * Node reclaim mode
 *
 * If non-zero call node_reclaim when the number of free pages falls below
 * the watermarks.
 */
int node_reclaim_mode __read_mostly;

/*
 * Priority for NODE_RECLAIM. This determines the fraction of pages
 * of a node considered for each zone_reclaim. 4 scans 1/16th of
 * a zone.
 */
#define NODE_RECLAIM_PRIORITY 4

/*
 * Percentage of pages in a zone that must be unmapped for node_reclaim to
 * occur.
 */
int sysctl_min_unmapped_ratio = 1;

/*
 * If the number of slab pages in a zone grows beyond this percentage then
 * slab reclaim needs to occur.
 */
int sysctl_min_slab_ratio = 5;

static inline unsigned long node_unmapped_file_pages(struct pglist_data *pgdat)
{
	unsigned long file_mapped = node_page_state(pgdat, NR_FILE_MAPPED);
	unsigned long file_lru = node_page_state(pgdat, NR_INACTIVE_FILE) +
		node_page_state(pgdat, NR_ACTIVE_FILE);

	/*
	 * It's possible for there to be more file mapped pages than
	 * accounted for by the pages on the file LRU lists because
	 * tmpfs pages accounted for as ANON can also be FILE_MAPPED
	 */
	return (file_lru > file_mapped) ? (file_lru - file_mapped) : 0;
}

/* Work out how many page cache pages we can reclaim in this reclaim_mode */
static unsigned long node_pagecache_reclaimable(struct pglist_data *pgdat)
{
	unsigned long nr_pagecache_reclaimable;
	unsigned long delta = 0;

	/*
	 * If RECLAIM_UNMAP is set, then all file pages are considered
	 * potentially reclaimable. Otherwise, we have to worry about
	 * pages like swapcache and node_unmapped_file_pages() provides
	 * a better estimate
	 */
	if (node_reclaim_mode & RECLAIM_UNMAP)
		nr_pagecache_reclaimable = node_page_state(pgdat, NR_FILE_PAGES);
	else
		nr_pagecache_reclaimable = node_unmapped_file_pages(pgdat);

	/* If we can't clean pages, remove dirty pages from consideration */
	if (!(node_reclaim_mode & RECLAIM_WRITE))
		delta += node_page_state(pgdat, NR_FILE_DIRTY);

	/* Watch for any possible underflows due to delta */
	if (unlikely(delta > nr_pagecache_reclaimable))
		delta = nr_pagecache_reclaimable;

	return nr_pagecache_reclaimable - delta;
}

/*
 * Try to free up some pages from this node through reclaim.
 */
static int __node_reclaim(struct pglist_data *pgdat, gfp_t gfp_mask, unsigned int order)
{
	/* Minimum pages needed in order to stay on node */
	const unsigned long nr_pages = 1 << order;
	struct task_struct *p = current;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = current_gfp_context(gfp_mask),
		.order = order,
		.priority = NODE_RECLAIM_PRIORITY,
		.may_writepage = !!(node_reclaim_mode & RECLAIM_WRITE),
		.may_unmap = !!(node_reclaim_mode & RECLAIM_UNMAP),
		.may_swap = 1,
		.reclaim_idx = gfp_zone(gfp_mask),
	};
	unsigned long pflags;

	trace_mm_vmscan_node_reclaim_begin(pgdat->node_id, order,
					   sc.gfp_mask);

	cond_resched();
	psi_memstall_enter(&pflags);
	fs_reclaim_acquire(sc.gfp_mask);
	/*
	 * We need to be able to allocate from the reserves for RECLAIM_UNMAP
	 * and we also need to be able to write out pages for RECLAIM_WRITE
	 * and RECLAIM_UNMAP.
	 */
	noreclaim_flag = memalloc_noreclaim_save();
	p->flags |= PF_SWAPWRITE;
	set_task_reclaim_state(p, &sc.reclaim_state);

	if (node_pagecache_reclaimable(pgdat) > pgdat->min_unmapped_pages) {
		/*
		 * Free memory by calling shrink node with increasing
		 * priorities until we have enough memory freed.
		 */
		do {
			shrink_node(pgdat, &sc);
		} while (sc.nr_reclaimed < nr_pages && --sc.priority >= 0);
	}

	set_task_reclaim_state(p, NULL);
	current->flags &= ~PF_SWAPWRITE;
	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(sc.gfp_mask);
	psi_memstall_leave(&pflags);

	trace_mm_vmscan_node_reclaim_end(sc.nr_reclaimed);

	return sc.nr_reclaimed >= nr_pages;
}

/*
 * IAMROOT, 2022.05.13:
 * - TODO
 *   papago만 돌려놓음.
 */
int node_reclaim(struct pglist_data *pgdat, gfp_t gfp_mask, unsigned int order)
{
	int ret;

	/*
	 * Node reclaim reclaims unmapped file backed pages and
	 * slab pages if we are over the defined limits.
	 *
	 * A small portion of unmapped file backed pages is needed for
	 * file I/O otherwise pages read by file I/O will be immediately
	 * thrown out if the node is overallocated. So we do not reclaim
	 * if less than a specified percentage of the node is used by
	 * unmapped file backed pages.
	 */
/*
 * IAMROOT, 2022.05.13:
 * - papago
 *   정의된 제한을 초과하면 노드 회수는 매핑되지 않은 파일 백업 페이지와
 *   슬래브 페이지를 회수합니다.
 *
 *   매핑되지 않은 파일 백업 페이지의 일부가 파일 I/O에 필요합니다. 그렇지 않으면
 *   노드가 과도하게 할당되면 파일 I/O에서 읽은 페이지가 즉시 삭제됩니다. 따라서
 *   매핑되지 않은 파일 백업 페이지에서 지정된 비율 미만의 노드가 사용되는 경우
 *   회수하지 않습니다.
 */
	if (node_pagecache_reclaimable(pgdat) <= pgdat->min_unmapped_pages &&
	    node_page_state_pages(pgdat, NR_SLAB_RECLAIMABLE_B) <=
	    pgdat->min_slab_pages)
		return NODE_RECLAIM_FULL;

	/*
	 * Do not scan if the allocation should not be delayed.
	 */
	if (!gfpflags_allow_blocking(gfp_mask) || (current->flags & PF_MEMALLOC))
		return NODE_RECLAIM_NOSCAN;

	/*
	 * Only run node reclaim on the local node or on nodes that do not
	 * have associated processors. This will favor the local processor
	 * over remote processors and spread off node memory allocations
	 * as wide as possible.
	 */
/*
 * IAMROOT, 2022.05.13:
 * - papago
 *   로컬 노드 또는 연결된 프로세서가 없는 노드에서만 노드 회수 실행 이것은
 *   원격 프로세서보다 로컬 프로세서를 선호하고 가능한 한 넓게 노드 메모리
 *   할당을 분산시킬 것이다.
 */
	if (node_state(pgdat->node_id, N_CPU) && pgdat->node_id != numa_node_id())
		return NODE_RECLAIM_NOSCAN;

	if (test_and_set_bit(PGDAT_RECLAIM_LOCKED, &pgdat->flags))
		return NODE_RECLAIM_NOSCAN;

	ret = __node_reclaim(pgdat, gfp_mask, order);
	clear_bit(PGDAT_RECLAIM_LOCKED, &pgdat->flags);

	if (!ret)
		count_vm_event(PGSCAN_ZONE_RECLAIM_FAILED);

	return ret;
}
#endif

/**
 * check_move_unevictable_pages - check pages for evictability and move to
 * appropriate zone lru list
 * @pvec: pagevec with lru pages to check
 *
 * Checks pages for evictability, if an evictable page is in the unevictable
 * lru list, moves it to the appropriate evictable lru list. This function
 * should be only used for lru pages.
 */
void check_move_unevictable_pages(struct pagevec *pvec)
{
	struct lruvec *lruvec = NULL;
	int pgscanned = 0;
	int pgrescued = 0;
	int i;

	for (i = 0; i < pvec->nr; i++) {
		struct page *page = pvec->pages[i];
		int nr_pages;

		if (PageTransTail(page))
			continue;

		nr_pages = thp_nr_pages(page);
		pgscanned += nr_pages;

		/* block memcg migration during page moving between lru */
		if (!TestClearPageLRU(page))
			continue;

		lruvec = relock_page_lruvec_irq(page, lruvec);
		if (page_evictable(page) && PageUnevictable(page)) {
			del_page_from_lru_list(page, lruvec);
			ClearPageUnevictable(page);
			add_page_to_lru_list(page, lruvec);
			pgrescued += nr_pages;
		}
		SetPageLRU(page);
	}

	if (lruvec) {
		__count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
		__count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
		unlock_page_lruvec_irq(lruvec);
	} else if (pgscanned) {
		count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
	}
}
EXPORT_SYMBOL_GPL(check_move_unevictable_pages);
