// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux VM pressure
 *
 * Copyright 2012 Linaro Ltd.
 *		  Anton Vorontsov <anton.vorontsov@linaro.org>
 *
 * Based on ideas from Andrew Morton, David Rientjes, KOSAKI Motohiro,
 * Leonid Moiseichuk, Mel Gorman, Minchan Kim and Pekka Enberg.
 */

#include <linux/cgroup.h>
#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/eventfd.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/printk.h>
#include <linux/vmpressure.h>

/*
 * The window size (vmpressure_win) is the number of scanned pages before
 * we try to analyze scanned/reclaimed ratio. So the window is used as a
 * rate-limit tunable for the "low" level notification, and also for
 * averaging the ratio for medium/critical levels. Using small window
 * sizes can cause lot of false positives, but too big window size will
 * delay the notifications.
 *
 * As the vmscan reclaimer logic works with chunks which are multiple of
 * SWAP_CLUSTER_MAX, it makes sense to use it for the window size as well.
 *
 * TODO: Make the window size depend on machine size, as we do for vmstat
 * thresholds. Currently we set it to 512 pages (2MB for 4KB pages).
 */
/*
 * IAMROOT, 2022.04.16:
 * - scan을 얼마나 할것인가에 대한 기준값. vmscan reclaimer에서 SWAP_CLUSTER_MAX
 *   단위로 동작하기 때문에 이 단위를 썻다고한다.
 *   32 * 16 = 512 page
 */
static const unsigned long vmpressure_win = SWAP_CLUSTER_MAX * 16;

/*
 * These thresholds are used when we account memory pressure through
 * scanned/reclaimed ratio. The current values were chosen empirically. In
 * essence, they are percents: the higher the value, the more number
 * unsuccessful reclaims there were.
 */
/*
 * IAMROOT, 2022.04.16:
 * - ex) scanned = 100, reclaim = 30
 *   1 - (reclaim / scanned) = 70 / 100 => 70%.
 * - 직 reclaim이 scanned에 비해 적을수록 %가 높아진다.
 */
static const unsigned int vmpressure_level_med = 60;
static const unsigned int vmpressure_level_critical = 95;

/*
 * When there are too little pages left to scan, vmpressure() may miss the
 * critical pressure as number of pages will be less than "window size".
 * However, in that case the vmscan priority will raise fast as the
 * reclaimer will try to scan LRUs more deeply.
 *
 * The vmscan logic considers these special priorities:
 *
 * prio == DEF_PRIORITY (12): reclaimer starts with that value
 * prio <= DEF_PRIORITY - 2 : kswapd becomes somewhat overwhelmed
 * prio == 0                : close to OOM, kernel scans every page in an lru
 *
 * Any value in this range is acceptable for this tunable (i.e. from 12 to
 * 0). Current value for the vmpressure_level_critical_prio is chosen
 * empirically, but the number, in essence, means that we consider
 * critical level when scanning depth is ~10% of the lru size (vmscan
 * scans 'lru_size >> prio' pages, so it is actually 12.5%, or one
 * eights).
 */
/*
 * IAMROOT, 2022.04.16:
 * - papago
 *   검색할 페이지가 너무 적으면 페이지 수가 window size보다 작기 때문에
 *   vmpressure()가 위험 압력을 놓칠 수 있습니다.
 *   그러나 이 경우 회수자가 LRU를 더 자세히 검색하려고 할 때 vmscan 우선 순위가
 *   빠르게 상승합니다.
 *
 *   vmscan 로직은 다음과 같은 특수 우선 순위를 고려합니다.
 *
 *   prio == DEF_PRIORITY(12): 회수기는 해당 값으로 시작합니다.
 *   prio <= DEF_PRIORITY - 2: kswapd는 다소 압도적이 된다.
 *   prio == 0: OOM에 가깝게, 커널은 lru의 모든 페이지를 스캔한다
 *
 *   이 범위의 모든 값은 이 튜닝 테이블에 대해 허용됩니다(즉, 12 ~ 0).
 *   vmpressure_level_critical_prio의 현재 값은 경험적으로 선택되지만, 기본적으로
 *   이 숫자는 스캔 깊이가 lru 크기의 최대 10%일 때 임계 수준을 고려한다는 것을
 *   의미합니다(vmscan 'lru_size >> prio' 페이지를 스캔하므로 실제로는 12.5% 또는
 *   8분의 1입니다).
 *
 * - log2(10) = 3, DEF_PRIORITY는 12
 */
static const unsigned int vmpressure_level_critical_prio = ilog2(100 / 10);

static struct vmpressure *work_to_vmpressure(struct work_struct *work)
{
	return container_of(work, struct vmpressure, work);
}

static struct vmpressure *vmpressure_parent(struct vmpressure *vmpr)
{
	struct mem_cgroup *memcg = vmpressure_to_memcg(vmpr);

	memcg = parent_mem_cgroup(memcg);
	if (!memcg)
		return NULL;
	return memcg_to_vmpressure(memcg);
}

enum vmpressure_levels {
	VMPRESSURE_LOW = 0,
	VMPRESSURE_MEDIUM,
	VMPRESSURE_CRITICAL,
	VMPRESSURE_NUM_LEVELS,
};

enum vmpressure_modes {
	VMPRESSURE_NO_PASSTHROUGH = 0,
	VMPRESSURE_HIERARCHY,
	VMPRESSURE_LOCAL,
	VMPRESSURE_NUM_MODES,
};

static const char * const vmpressure_str_levels[] = {
	[VMPRESSURE_LOW] = "low",
	[VMPRESSURE_MEDIUM] = "medium",
	[VMPRESSURE_CRITICAL] = "critical",
};

static const char * const vmpressure_str_modes[] = {
	[VMPRESSURE_NO_PASSTHROUGH] = "default",
	[VMPRESSURE_HIERARCHY] = "hierarchy",
	[VMPRESSURE_LOCAL] = "local",
};

/*
 * IAMROOT, 2022.04.16:
 * - @pressure 값(0 ~ 100)을 enum으로 환산한다.
 */
static enum vmpressure_levels vmpressure_level(unsigned long pressure)
{
	if (pressure >= vmpressure_level_critical)
		return VMPRESSURE_CRITICAL;
	else if (pressure >= vmpressure_level_med)
		return VMPRESSURE_MEDIUM;
	return VMPRESSURE_LOW;
}

/*
 * IAMROOT, 2022.04.16:
 * - @scanned, @reclaimed 값을 enum으로 환산한다.
 *   @reclaimed가 0으로 옳경우 pressure가 100으로 계산되 가장 높은
 *   VMPRESSURE_CRITICAL로 return된다.
 */
static enum vmpressure_levels vmpressure_calc_level(unsigned long scanned,
						    unsigned long reclaimed)
{
	unsigned long scale = scanned + reclaimed;
	unsigned long pressure = 0;

	/*
	 * reclaimed can be greater than scanned for things such as reclaimed
	 * slab pages. shrink_node() just adds reclaimed pages without a
	 * related increment to scanned pages.
	 */
	if (reclaimed >= scanned)
		goto out;
	/*
	 * We calculate the ratio (in percents) of how many pages were
	 * scanned vs. reclaimed in a given time frame (window). Note that
	 * time is in VM reclaimer's "ticks", i.e. number of pages
	 * scanned. This makes it possible to set desired reaction time
	 * and serves as a ratelimit.
	 */
/*
 * IAMROOT, 2022.04.26:
 * - 일반 수학식이라고 가정했을때
 *   pressure = pressure * 100 / scale
 *            = (scale - (reclaimed * scale / scanned)) * 100 / scale
 *            = 100 * (1 - (reclaimed / scanned))
 * - scanned가 높을수록 100에 가깝다.
 *   reclaimed가 높을수록 0에 가깝다.
 */
	pressure = scale - (reclaimed * scale / scanned);
	pressure = pressure * 100 / scale;

out:
	pr_debug("%s: %3lu  (s: %lu  r: %lu)\n", __func__, pressure,
		 scanned, reclaimed);

	return vmpressure_level(pressure);
}

struct vmpressure_event {
	struct eventfd_ctx *efd;
	enum vmpressure_levels level;
	enum vmpressure_modes mode;
	struct list_head node;
};

/*
 * IAMROOT, 2022.04.16:
 * - vmpr에 등록된 ev들(tasks)에 대해서 조건에 만족하는 tasks에 한해
 *   event signal을 보낸다.
 * @ancestor 한번이라도 시도했는지에 대한 flag. local끼리는 단 한번만 시도한다.
 * @signalled  한번이라도 성공했는지에 대한 flag. VMPRESSURE_NO_PASSTHROUGH mode
 *             한테는 안보낸다.
 *
 * - listener 의 mode에 따라 event를 받는 여부
 *   VMPRESSURE_LOCAL	       : 최초에 등록되 있어야 시도를 한다.
 *   VMPRESSURE_NO_PASSTHROUGH : 모든 listener 중에서 이전에 signal을 받은적이
 *                               없을때만 시도를 한다.
 *   VMPRESSURE_HIERARCHY      : level만 확인한다.
 */
static bool vmpressure_event(struct vmpressure *vmpr,
			     const enum vmpressure_levels level,
			     bool ancestor, bool signalled)
{
	struct vmpressure_event *ev;
	bool ret = false;

	mutex_lock(&vmpr->events_lock);
/*
 * IAMROOT, 2022.04.16:
 * - 두번째 이후라면 ancestor가 true. 즉 local끼리는 성공 유무 상관없이 한번만
 *   시도 한다는것이다.
 *   signalled가 true라면 이전에 최소 한번 signal을 보냈다는것이고, 이 상황에서
 *   mode가 VMPRESSURE_NO_PASSTHROUGH면 보내지 않는다.
 *   
 */
	list_for_each_entry(ev, &vmpr->events, node) {
		if (ancestor && ev->mode == VMPRESSURE_LOCAL)
			continue;
		if (signalled && ev->mode == VMPRESSURE_NO_PASSTHROUGH)
			continue;
		if (level < ev->level)
			continue;
		eventfd_signal(ev->efd, 1);
		ret = true;
	}
	mutex_unlock(&vmpr->events_lock);

	return ret;
}

/*
 * IAMROOT, 2022.04.16:
 * - tree_scanned,  tree_reclaimed값으로 vmpressure level을 계산하고,
 *   signal을 보낸다.
 */
static void vmpressure_work_fn(struct work_struct *work)
{
	struct vmpressure *vmpr = work_to_vmpressure(work);
	unsigned long scanned;
	unsigned long reclaimed;
	enum vmpressure_levels level;
	bool ancestor = false;
	bool signalled = false;

	spin_lock(&vmpr->sr_lock);
	/*
	 * Several contexts might be calling vmpressure(), so it is
	 * possible that the work was rescheduled again before the old
	 * work context cleared the counters. In that case we will run
	 * just after the old work returns, but then scanned might be zero
	 * here. No need for any locks here since we don't care if
	 * vmpr->reclaimed is in sync.
	 */
	scanned = vmpr->tree_scanned;
	if (!scanned) {
		spin_unlock(&vmpr->sr_lock);
		return;
	}

	reclaimed = vmpr->tree_reclaimed;
	vmpr->tree_scanned = 0;
	vmpr->tree_reclaimed = 0;
	spin_unlock(&vmpr->sr_lock);

	level = vmpressure_calc_level(scanned, reclaimed);

/*
 * IAMROOT, 2022.04.16:
 * - 최초에 등록된 listener에 보내고, 그 이후에 parent들한테 event signal을 보낸다.
 *   한번이라도 signal보내는데 성공했으면 signalled는 true, 두번째 시도부터는
 *   ancestor가 true가 된다.
 */
	do {
		if (vmpressure_event(vmpr, level, ancestor, signalled))
			signalled = true;
		ancestor = true;
	} while ((vmpr = vmpressure_parent(vmpr)));
}

/**
 * vmpressure() - Account memory pressure through scanned/reclaimed ratio
 * @gfp:	reclaimer's gfp mask
 * @memcg:	cgroup memory controller handle
 * @tree:	legacy subtree mode
 * @scanned:	number of pages scanned
 * @reclaimed:	number of pages reclaimed
 *
 * This function should be called from the vmscan reclaim path to account
 * "instantaneous" memory pressure (scanned/reclaimed ratio). The raw
 * pressure index is then further refined and averaged over time.
 *
 * If @tree is set, vmpressure is in traditional userspace reporting
 * mode: @memcg is considered the pressure root and userspace is
 * notified of the entire subtree's reclaim efficiency.
 *
 * If @tree is not set, reclaim efficiency is recorded for @memcg, and
 * only in-kernel users are notified.
 *
 * This function does not return any value.
 */
/*
 * IAMROOT, 2022.04.16:
 * - @tree값에 따라 work queue를 동작할지, socket_pressure를 갱신할지 선택한다.
 * - 누적 scanned, reclaimed 에 따라 vmpressure를 monitoring하고있는 task들한테
 *   signal을 보낸다.
 * - 누적 scanned가 vmpressure_win보다 작다면 누적만 시킨다.
 */
void vmpressure(gfp_t gfp, struct mem_cgroup *memcg, bool tree,
		unsigned long scanned, unsigned long reclaimed)
{
	struct vmpressure *vmpr;

	if (mem_cgroup_disabled())
		return;

	vmpr = memcg_to_vmpressure(memcg);

	/*
	 * Here we only want to account pressure that userland is able to
	 * help us with. For example, suppose that DMA zone is under
	 * pressure; if we notify userland about that kind of pressure,
	 * then it will be mostly a waste as it will trigger unnecessary
	 * freeing of memory by userland (since userland is more likely to
	 * have HIGHMEM/MOVABLE pages instead of the DMA fallback). That
	 * is why we include only movable, highmem and FS/IO pages.
	 * Indirect reclaim (kswapd) sets sc->gfp_mask to GFP_KERNEL, so
	 * we account it too.
	 */
/*
 * IAMROOT, 2022.04.16:
 * - papago
 *   여기서는 userland가 우리를 도울 수 있는 압력만을 설명하고자 한다. 예를 들어,
 *   DMA 영역이 압력을 받고 있다고 가정하자. 어떤 종류의 pressure을 userland에
 *   통지하면 userland에 의한 불필요한 memory freeing(userland에는 DMA fallback 대신
 *   HIGHMEM/MOVABLE 페이지가 있을 가능성이 더 높기 때문에)를 유발하기 때문에
 *   대부분 낭비가 될 것이다. 그렇기 때문에 이동 가능한 고메모리 및 FS/IO 페이지만
 *   포함합니다.  간접 회수(kswapd)는 sc->gfp_mask를 GFP_KERNEL로 설정하므로 우리도
 *   이를 고려합니다.
 */
	if (!(gfp & (__GFP_HIGHMEM | __GFP_MOVABLE | __GFP_IO | __GFP_FS)))
		return;

	/*
	 * If we got here with no pages scanned, then that is an indicator
	 * that reclaimer was unable to find any shrinkable LRUs at the
	 * current scanning depth. But it does not mean that we should
	 * report the critical pressure, yet. If the scanning priority
	 * (scanning depth) goes too high (deep), we will be notified
	 * through vmpressure_prio(). But so far, keep calm.
	 */
/*
 * IAMROOT, 2022.04.16:
 * - papago
 *   스캔한 페이지가 없는 상태에서 여기에 도달하면 회수자가 현재 스캔 깊이에서
 *   축소 가능한 LRU를 찾을 수 없다는 것을 나타냅니다. 그러나 그것은 우리가
 *   아직 임계 압력을 보고해야 한다는 것을 의미하지는 않는다.
 *   검색 우선 순위(스캔 깊이)가 너무 높은 경우(깊이) vmpressure_prio()를 통해
 *   알립니다. 하지만 아직까지는, 침착하세요.
 * - scanned 값은 compact를 실패한 상황에서 reclaim을 하러 오는 등의 상황에서
 *   (do_try_to_free_pages) 일때는 window sie 개념으로 vmpressure_win가 오게 된다. 
 *   즉 무조건 수행을 한다는 의미.
 */
	if (!scanned)
		return;

/*
 * IAMROOT, 2022.04.16:
 * - tree가 true면 legacy mode로 동작한다.
 */
	if (tree) {
		spin_lock(&vmpr->sr_lock);
/*
 * IAMROOT, 2022.04.16:
 * - vmpr->tree_scanned += scanned;
 *   scanned = vmpr->tree_scanned;
 */
		scanned = vmpr->tree_scanned += scanned;
		vmpr->tree_reclaimed += reclaimed;
		spin_unlock(&vmpr->sr_lock);

		if (scanned < vmpressure_win)
			return;
/*
 * IAMROOT, 2022.04.16:
 * - vmpressure_init에서 등록됬던 vmpressure_work_fn 함수가 work thread로 동작한다.
 *   listener들한테 signal을 보낸다.
 */
		schedule_work(&vmpr->work);
	} else {
		enum vmpressure_levels level;

		/* For now, no users for root-level efficiency */
		if (!memcg || mem_cgroup_is_root(memcg))
			return;

		spin_lock(&vmpr->sr_lock);
		scanned = vmpr->scanned += scanned;
		reclaimed = vmpr->reclaimed += reclaimed;
		if (scanned < vmpressure_win) {
			spin_unlock(&vmpr->sr_lock);
			return;
		}
		vmpr->scanned = vmpr->reclaimed = 0;
		spin_unlock(&vmpr->sr_lock);

		level = vmpressure_calc_level(scanned, reclaimed);

		if (level > VMPRESSURE_LOW) {
			/*
			 * Let the socket buffer allocator know that
			 * we are having trouble reclaiming LRU pages.
			 *
			 * For hysteresis keep the pressure state
			 * asserted for a second in which subsequent
			 * pressure events can occur.
			 */
/*
 * IAMROOT, 2022.04.16:
 * - 현재시간 + 1초. 즉 1초후
 */
			memcg->socket_pressure = jiffies + HZ;
		}
	}
}

/**
 * vmpressure_prio() - Account memory pressure through reclaimer priority level
 * @gfp:	reclaimer's gfp mask
 * @memcg:	cgroup memory controller handle
 * @prio:	reclaimer's priority
 *
 * This function should be called from the reclaim path every time when
 * the vmscan's reclaiming priority (scanning depth) changes.
 *
 * This function does not return any value.
 */
/*
 * IAMROOT, 2022.04.16:
 * - @prio가 vmpressure_level_critical_prio 이하일때 vmpressure에서 work를 동작시킨다.
 * - 누적 scanned, reclaimed 에 따라 vmpressure를 monitoring하고있는 task들한테
 *   signal을 보낸다.
 */
void vmpressure_prio(gfp_t gfp, struct mem_cgroup *memcg, int prio)
{
	/*
	 * We only use prio for accounting critical level. For more info
	 * see comment for vmpressure_level_critical_prio variable above.
	 */
/*
 * IAMROOT, 2022.04.16:
 * - DEF_PRIORITY는 12.
 */
	if (prio > vmpressure_level_critical_prio)
		return;

	/*
	 * OK, the prio is below the threshold, updating vmpressure
	 * information before shrinker dives into long shrinking of long
	 * range vmscan. Passing scanned = vmpressure_win, reclaimed = 0
	 * to the vmpressure() basically means that we signal 'critical'
	 * level.
	 */
	vmpressure(gfp, memcg, true, vmpressure_win, 0);
}

#define MAX_VMPRESSURE_ARGS_LEN	(strlen("critical") + strlen("hierarchy") + 2)

/**
 * vmpressure_register_event() - Bind vmpressure notifications to an eventfd
 * @memcg:	memcg that is interested in vmpressure notifications
 * @eventfd:	eventfd context to link notifications with
 * @args:	event arguments (pressure level threshold, optional mode)
 *
 * This function associates eventfd context with the vmpressure
 * infrastructure, so that the notifications will be delivered to the
 * @eventfd. The @args parameter is a comma-delimited string that denotes a
 * pressure level threshold (one of vmpressure_str_levels, i.e. "low", "medium",
 * or "critical") and an optional mode (one of vmpressure_str_modes, i.e.
 * "hierarchy" or "local").
 *
 * To be used as memcg event method.
 *
 * Return: 0 on success, -ENOMEM on memory failure or -EINVAL if @args could
 * not be parsed.
 */
int vmpressure_register_event(struct mem_cgroup *memcg,
			      struct eventfd_ctx *eventfd, const char *args)
{
	struct vmpressure *vmpr = memcg_to_vmpressure(memcg);
	struct vmpressure_event *ev;
	enum vmpressure_modes mode = VMPRESSURE_NO_PASSTHROUGH;
	enum vmpressure_levels level;
	char *spec, *spec_orig;
	char *token;
	int ret = 0;

	spec_orig = spec = kstrndup(args, MAX_VMPRESSURE_ARGS_LEN, GFP_KERNEL);
	if (!spec)
		return -ENOMEM;

	/* Find required level */
	token = strsep(&spec, ",");
	ret = match_string(vmpressure_str_levels, VMPRESSURE_NUM_LEVELS, token);
	if (ret < 0)
		goto out;
	level = ret;

	/* Find optional mode */
	token = strsep(&spec, ",");
	if (token) {
		ret = match_string(vmpressure_str_modes, VMPRESSURE_NUM_MODES, token);
		if (ret < 0)
			goto out;
		mode = ret;
	}

	ev = kzalloc(sizeof(*ev), GFP_KERNEL);
	if (!ev) {
		ret = -ENOMEM;
		goto out;
	}

	ev->efd = eventfd;
	ev->level = level;
	ev->mode = mode;

	mutex_lock(&vmpr->events_lock);
	list_add(&ev->node, &vmpr->events);
	mutex_unlock(&vmpr->events_lock);
	ret = 0;
out:
	kfree(spec_orig);
	return ret;
}

/**
 * vmpressure_unregister_event() - Unbind eventfd from vmpressure
 * @memcg:	memcg handle
 * @eventfd:	eventfd context that was used to link vmpressure with the @cg
 *
 * This function does internal manipulations to detach the @eventfd from
 * the vmpressure notifications, and then frees internal resources
 * associated with the @eventfd (but the @eventfd itself is not freed).
 *
 * To be used as memcg event method.
 */
void vmpressure_unregister_event(struct mem_cgroup *memcg,
				 struct eventfd_ctx *eventfd)
{
	struct vmpressure *vmpr = memcg_to_vmpressure(memcg);
	struct vmpressure_event *ev;

	mutex_lock(&vmpr->events_lock);
	list_for_each_entry(ev, &vmpr->events, node) {
		if (ev->efd != eventfd)
			continue;
		list_del(&ev->node);
		kfree(ev);
		break;
	}
	mutex_unlock(&vmpr->events_lock);
}

/**
 * vmpressure_init() - Initialize vmpressure control structure
 * @vmpr:	Structure to be initialized
 *
 * This function should be called on every allocated vmpressure structure
 * before any usage.
 */
void vmpressure_init(struct vmpressure *vmpr)
{
	spin_lock_init(&vmpr->sr_lock);
	mutex_init(&vmpr->events_lock);
	INIT_LIST_HEAD(&vmpr->events);
	INIT_WORK(&vmpr->work, vmpressure_work_fn);
}

/**
 * vmpressure_cleanup() - shuts down vmpressure control structure
 * @vmpr:	Structure to be cleaned up
 *
 * This function should be called before the structure in which it is
 * embedded is cleaned up.
 */
void vmpressure_cleanup(struct vmpressure *vmpr)
{
	/*
	 * Make sure there is no pending work before eventfd infrastructure
	 * goes away.
	 */
	flush_work(&vmpr->work);
}
