// SPDX-License-Identifier: GPL-2.0
/*
 * Completely Fair Scheduling (CFS) Class (SCHED_NORMAL/SCHED_BATCH)
 *
 *  Copyright (C) 2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 *  Interactivity improvements by Mike Galbraith
 *  (C) 2007 Mike Galbraith <efault@gmx.de>
 *
 *  Various enhancements by Dmitry Adamushko.
 *  (C) 2007 Dmitry Adamushko <dmitry.adamushko@gmail.com>
 *
 *  Group scheduling enhancements by Srivatsa Vaddagiri
 *  Copyright IBM Corporation, 2007
 *  Author: Srivatsa Vaddagiri <vatsa@linux.vnet.ibm.com>
 *
 *  Scaled math optimizations by Thomas Gleixner
 *  Copyright (C) 2007, Thomas Gleixner <tglx@linutronix.de>
 *
 *  Adaptive scheduling granularity, math enhancements by Peter Zijlstra
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 */
#include "sched.h"

/*
 * Targeted preemption latency for CPU-bound tasks:
 *
 * NOTE: this latency value is not the same as the concept of
 * 'timeslice length' - timeslices in CFS are of variable length
 * and have no persistent notion like in traditional, time-slice
 * based scheduling concepts.
 *
 * (to see the precise effective timeslice length of your workload,
 *  run vmstat and monitor the context-switches (cs) field)
 *
 */
/*
 * IAMROOT, 2023.01.07:
 * - (default: 6ms * (1 + ilog2(ncpus)), units: nanoseconds)
 * - cpu 갯수 2 = 6 * (1 + ilog2(2)) = 6 * 2 = 12 ms
 *   cpu 갯수 3 = 6 * (1 + ilog2(3)) = 6 * 2 = 12 ms
 *   cpu 갯수 4 = 6 * (1 + ilog2(4)) = 6 * 3 = 18 ms
 *   cpu 갯수 5 = 6 * (1 + ilog2(5)) = 6 * 3 = 18 ms
 *   cpu 갯수 6 = 6 * (1 + ilog2(6)) = 6 * 3 = 18 ms
 *   cpu 갯수 7 = 6 * (1 + ilog2(7)) = 6 * 3 = 18 ms
 *   cpu 갯수 8 = 6 * (1 + ilog2(8)) = 6 * 4 = 24 ms
 */
unsigned int sysctl_sched_latency			= 6000000ULL;
static unsigned int normalized_sysctl_sched_latency	= 6000000ULL;

/*
 * The initial- and re-scaling of tunables is configurable
 *
 * Options are:
 *
 *   SCHED_TUNABLESCALING_NONE - unscaled, always *1
 *   SCHED_TUNABLESCALING_LOG - scaled logarithmical, *1+ilog(ncpus)
 *   SCHED_TUNABLESCALING_LINEAR - scaled linear, *ncpus
 *
 * (default SCHED_TUNABLESCALING_LOG = *(1+ilog(ncpus))
 */
unsigned int sysctl_sched_tunable_scaling = SCHED_TUNABLESCALING_LOG;

/*
 * Minimal preemption granularity for CPU-bound tasks:
 *
 * (default: 0.75 msec * (1 + ilog(ncpus)), units: nanoseconds)
 */
unsigned int sysctl_sched_min_granularity			= 750000ULL;
static unsigned int normalized_sysctl_sched_min_granularity	= 750000ULL;

/*
 * This value is kept at sysctl_sched_latency/sysctl_sched_min_granularity
 */
static unsigned int sched_nr_latency = 8;

/*
 * After fork, child runs first. If set to 0 (default) then
 * parent will (try to) run first.
 */
unsigned int sysctl_sched_child_runs_first __read_mostly;

/*
 * SCHED_OTHER wake-up granularity.
 *
 * This option delays the preemption effects of decoupled workloads
 * and reduces their over-scheduling. Synchronous workloads will still
 * have immediate wakeup/sleep latencies.
 *
 * (default: 1 msec * (1 + ilog(ncpus)), units: nanoseconds)
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *  SCHED_OTHER 깨우기 세분성입니다.
 *
 *  이 옵션은 분리된 워크로드의 선점 효과를 지연시키고 과도한 스케줄링을 줄입니다.
 *  동기식 워크로드에는 여전히 즉각적인 깨우기/절전 대기 시간이 있습니다.
 *
 * (default: 1 msec * (1 + ilog(ncpus)), units: nanoseconds)
 */
unsigned int sysctl_sched_wakeup_granularity			= 1000000UL;
static unsigned int normalized_sysctl_sched_wakeup_granularity	= 1000000UL;

const_debug unsigned int sysctl_sched_migration_cost	= 500000UL;

int sched_thermal_decay_shift;
static int __init setup_sched_thermal_decay_shift(char *str)
{
	int _shift = 0;

	if (kstrtoint(str, 0, &_shift))
		pr_warn("Unable to set scheduler thermal pressure decay shift parameter\n");

	sched_thermal_decay_shift = clamp(_shift, 0, 10);
	return 1;
}
__setup("sched_thermal_decay_shift=", setup_sched_thermal_decay_shift);

#ifdef CONFIG_SMP
/*
 * For asym packing, by default the lower numbered CPU has higher priority.
 */
/*
 * IAMROOT, 2023.04.22:
 * - cpu번호가 작을수록 우선순위가 높다.(빠르다.)
 */
int __weak arch_asym_cpu_priority(int cpu)
{
	return -cpu;
}

/*
 * The margin used when comparing utilization with CPU capacity.
 *
 * (default: ~20%)
 */
/*
 * IAMROOT, 2023.05.06:
 * @return true  @cap의 1.2배가 max를 초과 안한다.
 *         false @cap의 1.2배가 max를 넘는다.
 */
#define fits_capacity(cap, max)	((cap) * 1280 < (max) * 1024)

/*
 * The margin used when comparing CPU capacities.
 * is 'cap1' noticeably greater than 'cap2'
 *
 * (default: ~5%)
 */
/*
 * IAMROOT, 2023.05.06:
 * @return true  cap1 > cap2 * 1.05
 *         false cap1 <= cap2 * 1.05
 */
#define capacity_greater(cap1, cap2) ((cap1) * 1024 > (cap2) * 1078)
#endif

#ifdef CONFIG_CFS_BANDWIDTH
/*
 * Amount of runtime to allocate from global (tg) to local (per-cfs_rq) pool
 * each time a cfs_rq requests quota.
 *
 * Note: in the case that the slice exceeds the runtime remaining (either due
 * to consumption or the quota being specified to be smaller than the slice)
 * we will always only issue the remaining available time.
 *
 * (default: 5 msec, units: microseconds)
 */
/*
 * IAMROOT, 2022.12.22:
 * - usec단위. 5ms
 */
unsigned int sysctl_sched_cfs_bandwidth_slice		= 5000UL;
#endif

static inline void update_load_add(struct load_weight *lw, unsigned long inc)
{
	lw->weight += inc;
	lw->inv_weight = 0;
}

static inline void update_load_sub(struct load_weight *lw, unsigned long dec)
{
	lw->weight -= dec;
	lw->inv_weight = 0;
}


/*
 * IAMROOT, 2023.01.07:
 * - @w를 weight로 사용.
 */
static inline void update_load_set(struct load_weight *lw, unsigned long w)
{
	lw->weight = w;
	lw->inv_weight = 0;
}

/*
 * Increase the granularity value when there are more CPUs,
 * because with more CPUs the 'effective latency' as visible
 * to users decreases. But the relationship is not linear,
 * so pick a second-best guess by going with the log2 of the
 * number of CPUs.
 *
 * This idea comes from the SD scheduler of Con Kolivas:
 */
/*
 * IAMROOT. 2023.04.30:
 * - google-translate
 * CPU가 많을수록 사용자에게 표시되는 '유효 대기 시간'이 줄어들기 때문에 CPU가 더
 * 많은 경우 세분성 값을 늘립니다. 그러나 관계는 선형적이지 않으므로 CPU 수의
 * log2를 사용하여 두 번째로 좋은 추측을 선택합니다. 이 아이디어는 Con Kolivas의 SD
 * 스케줄러에서 가져온 것입니다.
 *
 * - ex. default 의 경우 cpus가 4라면 ilog2(4) + 1 로 해서 3을 반환
 */
static unsigned int get_update_sysctl_factor(void)
{
	unsigned int cpus = min_t(unsigned int, num_online_cpus(), 8);
	unsigned int factor;

	switch (sysctl_sched_tunable_scaling) {
	case SCHED_TUNABLESCALING_NONE:
		factor = 1;
		break;
	case SCHED_TUNABLESCALING_LINEAR:
		factor = cpus;
		break;
	case SCHED_TUNABLESCALING_LOG:
	default:
		factor = 1 + ilog2(cpus);
		break;
	}

	return factor;
}

/*
 * IAMROOT, 2023.04.30:
 * - ex. num_online_cpus 가 4이고 default case라 factor에 3이 설정되었다고 가정
 * - sysctl_sched_min_granularity = 3 * normalized_sysctl_sched_min_granularity
 *                                = 3 * 0.75
 *                                = 2.25(ms)
 */
static void update_sysctl(void)
{
	unsigned int factor = get_update_sysctl_factor();

#define SET_SYSCTL(name) \
	(sysctl_##name = (factor) * normalized_sysctl_##name)
	SET_SYSCTL(sched_min_granularity);
	SET_SYSCTL(sched_latency);
	SET_SYSCTL(sched_wakeup_granularity);
#undef SET_SYSCTL
}

void __init sched_init_granularity(void)
{
	update_sysctl();
}

#define WMULT_CONST	(~0U)
#define WMULT_SHIFT	32

/*
 * IAMROOT, 2022.12.17:
 * - lw의 weight 에 대한 inv_weight 값을 갱신.
 */
static void __update_inv_weight(struct load_weight *lw)
{
	unsigned long w;

	if (likely(lw->inv_weight))
		return;

	w = scale_load_down(lw->weight);

	/*
	 * IAMROOT, 2022.12.17:
	 * - inv_weight = (1/weight) * 2^32 = 2^32/weight
	 *   w 가 최대값을 초과하면 inv_weight는 최소값 1
	 *   w 가 최소값 0이면 inv_weight는 최대값 사용
	 */
	if (BITS_PER_LONG > 32 && unlikely(w >= WMULT_CONST))
		lw->inv_weight = 1;
	else if (unlikely(!w))
		lw->inv_weight = WMULT_CONST;
	else
		lw->inv_weight = WMULT_CONST / w;
}

/*
 * delta_exec * weight / lw.weight
 *   OR
 * (delta_exec * (weight * lw->inv_weight)) >> WMULT_SHIFT
 *
 * Either weight := NICE_0_LOAD and lw \e sched_prio_to_wmult[], in which case
 * we're guaranteed shift stays positive because inv_weight is guaranteed to
 * fit 32 bits, and NICE_0_LOAD gives another 10 bits; therefore shift >= 22.
 *
 * Or, weight =< lw.weight (because lw.weight is the runqueue weight), thus
 * weight/lw.weight <= 1, and therefore our shift will also be positive.
 */
/*
 * IAMROOT, 2022.12.17:
 * - @weight와 @lw->weight의 비율에 따른 @delta_exec의 계산값을 return한다.
 *   (@delta_exec : @return = @weight : @lw->weight)
 *
 * - 실제론 이미 계산된 1 / lw.weight값을 사용해서 이진화 정수계산으로
 *   구한다 (위 주석 참고)
 * - 계산을 수행하면서 발생할수있는 32bit overflow에 대한 처리와,
 *   그에 따른 정밀도 감소처리가 있다.
 *
 * - calc_delta_fair 에서 호출 하였을 때
 * - ex. delta_exec=1000000, weight=1024, lw.weight=1277
 *   1. fact=1024, lw.inv_weight=3363326
 *   2. fact_hi = (fact >> 32) = 0 -> pass
 *   3. fact = fact*inv_weight = 1024*3363323 = 3,444,042,752 = 0xCD47,EC00 =
 *      0b1100,1101,0100,0111,1110,1100,0000,0000 (총32bit)
 *   4. fact_hi = (fact >> 32) = 0 -> pass
 *   5. (delta_exec * fact) >> 32
 *      (1000000 * 3,444,042,752) >> 32 = 801,878
 *
 * - ex. delta_exec=1000000, weight=1024, lw.weight=1024
 *   1. fact=1024, lw.inv_weight=4194304
 *   2. fact_hi = (fact >> 32) = 0 -> pass
 *   3. fact = fact*inv_weight = 1024*4194304 = 0x1,0000,0000 = 4,294,967,296 =
 *      0b1,0000,0000,0000,0000,0000,0000,0000,0000(총33bit)
 *   4. fact_hi = (fact >> 32) = 1
 *      fs = 1, shift = 31
 *      fact = (fact >> 1) = 0b1000,0000,0000,0000,0000,0000,0000,0000 =
 *      2,147,483,648
 *   5. (delta_exec * fact) >> 31
 *      (1000000 * 2,147,483,648) >> 31 = 1,000,000
 *
 * - sched_slice 에서 호출하였을 때
 * - ex1.
 *   delta_exec = 6000000(6ms)
 *   task 1 : nice 0, 1024(weight), 4194304(inv_weight)
 *   task 2 : nice -10, 9548(weight), 449829(inv_weight)
 *   cfs_rq : weight = task1 + task2 = 1024 + 9548 = 10572
 *            lw->inv_weight = (2^32-1) / 10572 = 406258
 *
 * - 최종식
 *   (delta_exec * (weight * lw->inv_weight)) >> 32 = (delta_exec * fact) >> 32
 *   delta_exec = 6000000(6ms)
 *   weight = 1024(task1), 9548(task2)
 *   lw->inv_weight = 406258
 *
 *   task 1 : fact = mul_u32_u32(fact, lw->inv_weight)
 *            1024 * 406258 = 416008192 = 0x18CB_C800 => fact_hi 조건 pass
 *            (6000000 * 416008192) >> 32 = 581,156ns = 0.58ms
 *
 *   task 2 : fact = mul_u32_u32(fact, lw->inv_weight)
 *            9548 * 406258 = 3878951384 = 0xE734_19D8 => fact_hi 조건 pass
 *            (6000000 * 3878951384) >> 32 = 5,418,832 ns = 5.42ms
 *
 * -- ex1-1. se의 부모가 있을 경우 루프를 계속한다.
 *    부모 cfs_rq에 se가 3개 있고 각각 1024 weight 라고 가정
 *    cfs_rq : weight = 1024 * 3 = 3072
 *             lw->inv_weight = (2^32 - 1) / 3072 = 1398101
 *
 * - 최종식
 *   (delta_exec * (weight * lw->inv_weight)) >> 32 = (delta_exec * fact) >> 32
 *   delta_exec = 581156(task1), 5418832(task2)
 *   weight = 1024
 *   lw->inv_weight = 406258

 *   task 1 : fact = mul_u32_u32(fact, lw->inv_weight)
 *            1024 * 1398101 = 1431655424  = 0x5555_5400 => fact_hi 조건 pass
 *            (581156 * 1431655424) >> 32 = 193,718ns = 0.19ms
 *
 *   task 2 : fact = mul_u32_u32(fact, lw->inv_weight)
 *            1024 * 1398101 = 1431655424  = 0x5555_5400 => fact_hi 조건 pass
 *            (5418832 * 1431655424) >> 32 = 1,806,276ns = 1.81ms
 *
 * --- se의 parent == NULL -> 종료
 */
static u64 __calc_delta(u64 delta_exec, unsigned long weight, struct load_weight *lw)
{
/*
 * IAMROOT, 2022.12.21:
 * - delta_exec * weight * lw->inv_weight를 구하기전에 먼제
 *   weight * lw->inv_weight 값을 구하기 위한 준비를 한다.
 */
	u64 fact = scale_load_down(weight);
	u32 fact_hi = (u32)(fact >> 32);
	int shift = WMULT_SHIFT;
	int fs;

/*
 * IAMROOT, 2022.12.21:
 * - lw->inv_weigh가계산이 안되어있으면 계산.
 */
	__update_inv_weight(lw);

	/*
	 * IAMROOT, 2022.12.17:
	 * - shift를 32bit 정밀도로 사용하여야 하는데 weight가 32bit를 초과 하였으므로
	 *   그만큼 shift값에서 빼준다.
	 */
	if (unlikely(fact_hi)) {
		fs = fls(fact_hi);
		shift -= fs;
		fact >>= fs;
	}

/*
 * IAMROOT, 2022.12.21:
 * - weight * lw->inv_weight 계산을 수행한다. 계산값이 u64가 될수있으므로
 *   그에 대한 처리가 있는 api사용.
 *   (실제 계산. weight * ( 1 / lw->weight))
 */
	fact = mul_u32_u32(fact, lw->inv_weight);

	/*
	 * IAMROOT, 2022.12.17:
	 * - inv_weight 값을 곱하고 32bit를 초과하면 그만틈 shift값을 또 빼줌
	 */
	fact_hi = (u32)(fact >> 32);
	if (fact_hi) {
		fs = fls(fact_hi);
		shift -= fs;
		fact >>= fs;
	}

/*
 * IAMROOT, 2022.12.21:
 *   ret = (delta_exec * (weight * lw->inv_weight)) >> WMULT_CONST
 */
	return mul_u64_u32_shr(delta_exec, fact, shift);
}


const struct sched_class fair_sched_class;

/**************************************************************
 * CFS operations on generic schedulable entities:
 */

#ifdef CONFIG_FAIR_GROUP_SCHED

/* Walk up scheduling entities hierarchy */
/*
 * IAMROOT, 2022.12.17:
 * - A1 그룹에 task x용 se 가 있고 B1 그룹에 task y용 se가 있다.
 *   A 그룹에 A1그룹용 se가 있고 B 그룹에는 B1 그룹용 se가 있다.
 *   root 그룹에 A그룹용 se와 B그룹용 se가 있다.
 *                  null
 *             +--/------\----+
 *         Root|(se)A    (se)B|
 *             +--/-------\---+
 *           A:(se)A1    B:(se)B1
 *              /            \
 *         A1:[se]x       B1:[se]y
 *
 *   se가 task x 일 경우 loop 순서
 *   [se]x -> (se)A1 -> (se)A -> null
 */
#define for_each_sched_entity(se) \
		for (; se; se = se->parent)

static inline void cfs_rq_tg_path(struct cfs_rq *cfs_rq, char *path, int len)
{
	if (!path)
		return;

	if (cfs_rq && task_group_is_autogroup(cfs_rq->tg))
		autogroup_path(cfs_rq->tg, path, len);
	else if (cfs_rq && cfs_rq->tg->css.cgroup)
		cgroup_path(cfs_rq->tg->css.cgroup, path, len);
	else
		strlcpy(path, "(null)", len);
}

/*
 * IAMROOT, 2022.12.27:
 * @return true @cfs_rq가 tree에 연결되있는 경우
 *
 * - @cfs_rq가 on list라면 tree 연결 여부를 판별, 그게 아니면
 *   on list후 parent에 따라 list 연결, tree 연결 여부 판별을한다.
 * - rq->tmp_alone_branch == rq->leaf_cfs_rq_list
 *   rq tmp_alone_branch가 자신의 rq leaf_cfs_rq_list면 tree에
 *   연결된, 성공적으로 add된것을 나타낸다.
 * - tree 연결의 의미.
 *   parent가 아에 없거나, parent가 있고, parent가 on list가 되있는 상태
 *   자기자신의 tree 최상위거나, parent가 tree에 이미 있으므로, tree에
 *   연결되있다고 판단한다.
 * - parent가 있지만 parent가 on list가 아닌 상태.
 *   parent가 아직 tree에 들어가지 않았으므로, child도 자연스럽게 아직
 *   tree에 안들어갓다.
 *
 * - tmp_alone_branch
 *   tree(branch)의 시작점을 cache한다.
 *   tree가 root에서부터 이어질수도, 아니면 parent가 아직 tree에 연결이
 *   안되 root까지 안이어진 상태일수도 있다. 
 */
static inline bool list_add_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	int cpu = cpu_of(rq);

/*
 * IAMROOT, 2023.05.04:
 * - @cfs_rq가 on list인 경우 이미 추가되있다. 따로 추가할 필요없이
 *   list가 tree에 연결되어있는지만을 확인한다.
 *   (tmp_alone_branch가 leaf_cfs_rq_list가 같은지를 비교)
 */
	if (cfs_rq->on_list)
		return rq->tmp_alone_branch == &rq->leaf_cfs_rq_list;
/*
 * IAMROOT, 2023.05.04:
 * - @cfs_rq를 on list한다.
 */
	cfs_rq->on_list = 1;

	/*
	 * Ensure we either appear before our parent (if already
	 * enqueued) or force our parent to appear after us when it is
	 * enqueued. The fact that we always enqueue bottom-up
	 * reduces this to two cases and a special case for the root
	 * cfs_rq. Furthermore, it also means that we will always reset
	 * tmp_alone_branch either when the branch is connected
	 * to a tree or when we reach the top of the tree
	 */
/*
 * IAMROOT, 2023.05.04:
 * - papago
 *   우리가 부모보다 먼저 나타나거나(이미 대기열에 있는 경우) 부모가 
 *   대기열에 있을 때 우리 뒤에 나타나도록 강제합니다. 우리가 항상 
 *   상향식으로 큐에 넣는다는 사실은 이것을 두 가지 경우와 루트 
 *   cfs_rq에 대한 특별한 경우로 줄입니다. 또한 분기가 트리에 
 *   연결되거나 트리의 맨 위에 도달할 때 항상 tmp_alone_branch를 
 *   재설정한다는 의미이기도 합니다. 
 * - parent가 이미 on list라면 parent 뒤에 child를 넣어야한다.
 */
	if (cfs_rq->tg->parent &&
	    cfs_rq->tg->parent->cfs_rq[cpu]->on_list) {
		/*
		 * If parent is already on the list, we add the child
		 * just before. Thanks to circular linked property of
		 * the list, this means to put the child at the tail
		 * of the list that starts by parent.
		 */
/*
 * IAMROOT, 2023.05.04:
 * - papago
 *   부모가 이미 목록에 있는 경우 직전에 자식을 추가합니다. 
 *   목록의 순환 연결 속성 덕분에 이는 부모로 시작하는 목록의 
 *   끝에 자식을 두는 것을 의미합니다.
 */
		list_add_tail_rcu(&cfs_rq->leaf_cfs_rq_list,
			&(cfs_rq->tg->parent->cfs_rq[cpu]->leaf_cfs_rq_list));
		/*
		 * The branch is now connected to its tree so we can
		 * reset tmp_alone_branch to the beginning of the
		 * list.
		 */
/*
 * IAMROOT, 2023.05.04:
 * - papago
 *   이제 분기가 트리에 연결되었으므로 tmp_alone_branch를 목록의 
 *   시작 부분으로 재설정할 수 있습니다.
 */
		rq->tmp_alone_branch = &rq->leaf_cfs_rq_list;
		return true;
	}

/*
 * IAMROOT, 2023.05.04:
 * - parent가 없으면 list의 last에 넣는다.
 */
	if (!cfs_rq->tg->parent) {
		/*
		 * cfs rq without parent should be put
		 * at the tail of the list.
		 */
/*
 * IAMROOT, 2023.05.04:
 * - papago
 *   부모가 없는 cfs rq는 목록의 끝에 넣어야 합니다.
 */
		list_add_tail_rcu(&cfs_rq->leaf_cfs_rq_list,
			&rq->leaf_cfs_rq_list);
		/*
		 * We have reach the top of a tree so we can reset
		 * tmp_alone_branch to the beginning of the list.
		 */
/*
 * IAMROOT, 2023.05.04:
 * - papago
 *   트리의 맨 위에 도달하여 tmp_alone_branch를 목록의 
 *   시작 부분으로 재설정할 수 있습니다.
 */
		rq->tmp_alone_branch = &rq->leaf_cfs_rq_list;
		return true;
	}

	/*
	 * The parent has not already been added so we want to
	 * make sure that it will be put after us.
	 * tmp_alone_branch points to the begin of the branch
	 * where we will add parent.
	 */
/*
 * IAMROOT, 2023.05.04:
 * - papago
 *   부모는 아직 추가되지 않았으므로 우리 뒤에 추가되도록 하고 싶습니다.
 *   tmp_alone_branch는 부모를 추가할 분기의 시작을 가리킵니다.
 *
 * - parent가 있지만, parent가 on_list가 아닌 경우이다.
 *   child -> parent순으로 놓는다.
 */
	list_add_rcu(&cfs_rq->leaf_cfs_rq_list, rq->tmp_alone_branch);
	/*
	 * update tmp_alone_branch to points to the new begin
	 * of the branch
	 */
/*
 * IAMROOT, 2023.05.04:
 * - papago
 *   분기의 새로운 시작을 가리키도록 tmp_alone_branch를 업데이트합니다.
 */
	rq->tmp_alone_branch = &cfs_rq->leaf_cfs_rq_list;
	return false;
}

/*
 * IAMROOT, 2023.05.03:
 * - @cfs_rq를 leaf_cfs_rq_list에서 제거한다.
 */
static inline void list_del_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->on_list) {
		struct rq *rq = rq_of(cfs_rq);

		/*
		 * With cfs_rq being unthrottled/throttled during an enqueue,
		 * it can happen the tmp_alone_branch points the a leaf that
		 * we finally want to del. In this case, tmp_alone_branch moves
		 * to the prev element but it will point to rq->leaf_cfs_rq_list
		 * at the end of the enqueue.
		 */
/*
 * IAMROOT, 2023.05.03:
 * - papago
 *   대기열에 넣는 동안 cfs_rq가 제한 해제/제한되면 tmp_alone_branch가 
 *   우리가 최종적으로 삭제하려는 잎을 가리킬 수 있습니다. 이 경우 
 *   tmp_alone_branch는 prev 요소로 이동하지만 enqueue의 끝에서 
 *   rq->leaf_cfs_rq_list를 가리킵니다.
 * - 제거할 @cfs_rq의 leaf_cfs_rq_list가 tmp_alone_branch이라면 prev로
 *   갱신한다.
 */
		if (rq->tmp_alone_branch == &cfs_rq->leaf_cfs_rq_list)
			rq->tmp_alone_branch = cfs_rq->leaf_cfs_rq_list.prev;

		list_del_rcu(&cfs_rq->leaf_cfs_rq_list);
		cfs_rq->on_list = 0;
	}
}

static inline void assert_list_leaf_cfs_rq(struct rq *rq)
{
	SCHED_WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list);
}

/* Iterate thr' all leaf cfs_rq's on a runqueue */
#define for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)			\
	list_for_each_entry_safe(cfs_rq, pos, &rq->leaf_cfs_rq_list,	\
				 leaf_cfs_rq_list)

/* Do the two (enqueued) entities belong to the same group ? */
/*
 * IAMROOT, 2023.01.28:
 * - 같은 cfs_rq 소속인지 확인한다.
 */
static inline struct cfs_rq *
is_same_group(struct sched_entity *se, struct sched_entity *pse)
{
	if (se->cfs_rq == pse->cfs_rq)
		return se->cfs_rq;

	return NULL;
}

static inline struct sched_entity *parent_entity(struct sched_entity *se)
{
	return se->parent;
}

static void
find_matching_se(struct sched_entity **se, struct sched_entity **pse)
{
	int se_depth, pse_depth;

	/*
	 * preemption test can be made between sibling entities who are in the
	 * same cfs_rq i.e who have a common parent. Walk up the hierarchy of
	 * both tasks until we find their ancestors who are siblings of common
	 * parent.
	 */

	/* First walk up until both entities are at same depth */
	se_depth = (*se)->depth;
	pse_depth = (*pse)->depth;

	while (se_depth > pse_depth) {
		se_depth--;
		*se = parent_entity(*se);
	}

	while (pse_depth > se_depth) {
		pse_depth--;
		*pse = parent_entity(*pse);
	}

	while (!is_same_group(*se, *pse)) {
		*se = parent_entity(*se);
		*pse = parent_entity(*pse);
	}
}

static int tg_is_idle(struct task_group *tg)
{
	return tg->idle > 0;
}

static int cfs_rq_is_idle(struct cfs_rq *cfs_rq)
{
	return cfs_rq->idle > 0;
}

static int se_is_idle(struct sched_entity *se)
{
	if (entity_is_task(se))
		return task_has_idle_policy(task_of(se));
	return cfs_rq_is_idle(group_cfs_rq(se));
}

#else	/* !CONFIG_FAIR_GROUP_SCHED */

#define for_each_sched_entity(se) \
		for (; se; se = NULL)

static inline void cfs_rq_tg_path(struct cfs_rq *cfs_rq, char *path, int len)
{
	if (path)
		strlcpy(path, "(null)", len);
}

static inline bool list_add_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	return true;
}

static inline void list_del_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
}

static inline void assert_list_leaf_cfs_rq(struct rq *rq)
{
}

#define for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)	\
		for (cfs_rq = &rq->cfs, pos = NULL; cfs_rq; cfs_rq = pos)

static inline struct sched_entity *parent_entity(struct sched_entity *se)
{
	return NULL;
}

static inline void
find_matching_se(struct sched_entity **se, struct sched_entity **pse)
{
}

static inline int tg_is_idle(struct task_group *tg)
{
	return 0;
}

static int cfs_rq_is_idle(struct cfs_rq *cfs_rq)
{
	return 0;
}

static int se_is_idle(struct sched_entity *se)
{
	return 0;
}

#endif	/* CONFIG_FAIR_GROUP_SCHED */

static __always_inline
void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec);

/**************************************************************
 * Scheduling class tree data structure manipulation methods:
 */

static inline u64 max_vruntime(u64 max_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - max_vruntime);
	if (delta > 0)
		max_vruntime = vruntime;

	return max_vruntime;
}

static inline u64 min_vruntime(u64 min_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - min_vruntime);
	if (delta < 0)
		min_vruntime = vruntime;

	return min_vruntime;
}

/*
 * IAMROOT, 2023.01.28:
 * - a가 b보다 vruntime이 작다면 return true;
 */
static inline bool entity_before(struct sched_entity *a,
				struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) < 0;
}

#define __node_2_se(node) \
	rb_entry((node), struct sched_entity, run_node)

/*
 * IAMROOT, 2022.12.17:
 * - min_vruntime 값을 현재 cfs_rq 의 entity들 중 가장 작은 vruntime 값으로 업데이트한다.
 * - 1. 보통은 curr->vruntime으로 갱신.
 *   2. curr가 없거나 on_rq가 아닌 상태면 leftmost_se->vruntime으로 처리
 *   3. 그게 아니면 min_vruntime유지
 *
 * - 보통은 curr->vruntime이 가장 빠른 시간으로 설정되고, 그에따라
 *   아래 조건들로 해서도 왠간해선 curr->vruntime이 선택 될것이다.
 *   그러나 어찌됫든 혹시나 모를 상황에 대비해 min, max비교등을 수행해서
 *   min_vruntime이 갱신된다.
 *
 * --- vruntime이 min_vruntime보다 클경우(chat open ai. 검증필요. 참고만)
 *  1. task이 이전에 선점되었고 나중에 재개된 경우입니다. task가 선점되면
 *  해당 vruntime이 일시적으로 중지될 수 있으며 다시 시작되면 해당
 *  vruntime이 현재 시간으로 업데이트될 수 있습니다. task가 min_vruntime
 *  값보다 나중에 재개되면 해당 vruntime이 min_vruntime보다 클 수 있습니다.
 *
 *  2. 현재 실행 중인 task이 cfs_rq의 다른 task보다 높은 우선 순위를
 *  가지고 있어 더 많은 CPU 시간을 받은 경우입니다. 이 경우 현재 실행 중인
 *  task의 vruntime이 아직 실행되지 않은 task의 vruntime보다 클 수
 *  있으므로 min_vruntime 값보다 클 수 있습니다.
 */
static void update_min_vruntime(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	struct rb_node *leftmost = rb_first_cached(&cfs_rq->tasks_timeline);

	u64 vruntime = cfs_rq->min_vruntime;

/*
 * IAMROOT, 2022.12.21:
 * - min_vruntime과 비교될 vruntime을 결정한다.
 *
 * 1. curr가 rq에 들어있다. leftmost
 * 1. curr가 rq에 들어있고, leftmost가 존재.
 *   vruntime = min_vruntime(curr->vruntime, leftmost_se->vruntime)
 * 2. leftmost가 있고 curr가 없는 상태.
 *   vruntime = leftmost_se->vruntime
 *
 * curr on_rq leftmost (compare max_vruntime cfs_rq->min_vruntime)
 * X    -     X        -
 * X    -     O        leftmost_se->vruntime
 * O    X     X        -
 * O    X     O        leftmost_se->vruntime
 * O    O     X        curr->vruntime
 * O    O     O        min_vruntime(curr->vruntime, leftmost_se->vruntime)
 *
 * - 현재 시점에서 curr가 가장 빠른 vruntime을 가진다.
 *   그래서 curr = O, on_rq = O, leftmost = O인 상황에서의 min_vruntime
 *   비교는 사실 의미는 없지만 혹시나 모르는 상황에 대비해서 min비교를
 *   한번 해준것이다.
 *
 * - leftmost가 없는 상황이면 curr->vruntime이 min_vruntime과 비교되서
 *   들어가는데 보통은 같다.
 *
 * -
 */
	if (curr) {
/*
 * IAMROOT, 2022.12.21:
 * - curr가 on_rq가 없으면 없는걸로 취급.
 */
		if (curr->on_rq)
			vruntime = curr->vruntime;
		else
			curr = NULL;
	}

	if (leftmost) { /* non-empty tree */
		struct sched_entity *se = __node_2_se(leftmost);

		if (!curr)
			vruntime = se->vruntime;
		else
			vruntime = min_vruntime(vruntime, se->vruntime);
	}

	/* ensure we never gain time by being placed backwards. */
/*
 * IAMROOT, 2022.12.21:
 * - 보통의 경우에 선택된 vruntime이 무조건 클것이지만 혹시나 모를 상황에
 *   대비해 max처리 해준것.
 */
	cfs_rq->min_vruntime = max_vruntime(cfs_rq->min_vruntime, vruntime);
#ifndef CONFIG_64BIT
	smp_wmb();
	cfs_rq->min_vruntime_copy = cfs_rq->min_vruntime;
#endif
}

static inline bool __entity_less(struct rb_node *a, const struct rb_node *b)
{
	return entity_before(__node_2_se(a), __node_2_se(b));
}

/*
 * Enqueue an entity into the rb-tree:
 */
/*
 * IAMROOT, 2023.01.28:
 * - cfs_rq에 넣는다.
 */
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	rb_add_cached(&se->run_node, &cfs_rq->tasks_timeline, __entity_less);
}

/*
 * IAMROOT, 2023.01.28:
 * - node(@se->run_node)를 rbtree(@cfs_rq->tasks_timeline)에서 제거
 */
static void __dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	rb_erase_cached(&se->run_node, &cfs_rq->tasks_timeline);
}

/*
 * IAMROOT, 2023.01.14:
 * - rbtree에서 가장 작은 vruntime entity를 반환한다.
 */
struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);

	if (!left)
		return NULL;

	return __node_2_se(left);
}

/*
 * IAMROOT, 2023.01.14:
 * - rbtree에서 @se 의 vruntime 보다 많은 다음 se를 반환한다.
 */
static struct sched_entity *__pick_next_entity(struct sched_entity *se)
{
	struct rb_node *next = rb_next(&se->run_node);

	if (!next)
		return NULL;

	return __node_2_se(next);
}

#ifdef CONFIG_SCHED_DEBUG
struct sched_entity *__pick_last_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *last = rb_last(&cfs_rq->tasks_timeline.rb_root);

	if (!last)
		return NULL;

	return __node_2_se(last);
}

/**************************************************************
 * Scheduling class statistics methods:
 */

int sched_update_scaling(void)
{
	unsigned int factor = get_update_sysctl_factor();

	sched_nr_latency = DIV_ROUND_UP(sysctl_sched_latency,
					sysctl_sched_min_granularity);

#define WRT_SYSCTL(name) \
	(normalized_sysctl_##name = sysctl_##name / (factor))
	WRT_SYSCTL(sched_min_granularity);
	WRT_SYSCTL(sched_latency);
	WRT_SYSCTL(sched_wakeup_granularity);
#undef WRT_SYSCTL

	return 0;
}
#endif

/*
 * delta /= w
 */
/*
 * IAMROOT, 2022.12.21:
 * - @se->load.weight에 따른 delta /= w값을 return 한다.
 *   ex)@se->load.weight가 nice 0의 1.1배이면 dalta /= 1.1
 *
 * - nice 0일 경우 __calc_delta계산 결과가 입력값 @delta와 변함이 없고
 *   어짜피 대부분의 task가 nice0이므로 계산을 안한다.
 */
static inline u64 calc_delta_fair(u64 delta, struct sched_entity *se)
{
	/*
	 * IAMROOT, 2022.12.17:
	 * - nice 값이 0 이 아닌 경우에만 vruntime 계산
	 */
	if (unlikely(se->load.weight != NICE_0_LOAD))
		delta = __calc_delta(delta, NICE_0_LOAD, &se->load);

	return delta;
}

/*
 * The idea is to set a period in which each task runs once.
 *
 * When there are too many tasks (sched_nr_latency) we have to stretch
 * this period because otherwise the slices get too small.
 *
 * p = (nr <= nl) ? l : l*nr/nl
 */
/*
 * IAMROOT, 2023.01.07:
 * - se 갯수가 8 이하이면 sysctl_sched_latency(6ms) 반환.
 *   단 cpu 갯수가 2이상이면 (1 + ilog2(cpus)) 을 곱한다.
 * - se 갯수가 9 이상이면 nr_running * sysctl_sched_min_granularity
 *   ex. se = 9, cpu = 1
 *   9 * 0.75(sysctl_sched_min_granularity) = 6.75 ms
 *   단 cpu 갯수가 2이상이면 (1 + ilog2(cpus)) 을 곱한다.
 */
static u64 __sched_period(unsigned long nr_running)
{
/*
 * IAMROOT, 2023.01.07:
 * - sched_nr_latency(9개) 이상일 때에는
 */
	if (unlikely(nr_running > sched_nr_latency))
		return nr_running * sysctl_sched_min_granularity;
	else
		return sysctl_sched_latency;
}

/*
 * We calculate the wall-time slice from the period by taking a part
 * proportional to the weight.
 *
 * s = p*P[w/rw]
 */
/*
 * IAMROOT, 2023.01.07:
 * - @se에 대한 slice 값을 반환한다
 *   @se->load.weight / @cfs_rq->load.weight 비율로 slice 값을 산출하되
 *   @se 는 parent 를 모두 찾을 때까지 loop 를 돌며 계속해서 slice 값을 업데이트 한다.
 */
static u64 sched_slice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
/*
 * IAMROOT, 2023.01.07:
 * - entity 수를 nr_running으로 사용한다.
 */
	unsigned int nr_running = cfs_rq->nr_running;
	u64 slice;

/*
 * IAMROOT, 2023.01.07:
 * - 계층구조의 task 수로 갱신한다.
 */
	if (sched_feat(ALT_PERIOD))
		nr_running = rq_of(cfs_rq)->cfs.h_nr_running;

	/*
	 * IAMROOT, 2023.01.14:
	 * - one cpu se 8개 이하 기준 6ms 반환
	 */
	slice = __sched_period(nr_running + !se->on_rq);

	for_each_sched_entity(se) {
		struct load_weight *load;
		struct load_weight lw;

		cfs_rq = cfs_rq_of(se);
		load = &cfs_rq->load;

		if (unlikely(!se->on_rq)) {
			lw = cfs_rq->load;

			update_load_add(&lw, se->load.weight);
			load = &lw;
		}
		/*
		 * IAMROOT, 2023.01.14:
		 * - @se에 사용할 slice 를 알아온다.
		 */
		slice = __calc_delta(slice, se->load.weight, load);
	}

	/*
	 * IAMROOT, 2023.01.14:
	 * - slice값이 0.75ms 보다 작으면 0.75ms(sysctl_sched_min_granularity)를
	 *   그냥 사용한다.
	 */
	if (sched_feat(BASE_SLICE))
		slice = max(slice, (u64)sysctl_sched_min_granularity);

	return slice;
}

/*
 * We calculate the vruntime slice of a to-be-inserted task.
 *
 * vs = s/w
 */
static u64 sched_vslice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	return calc_delta_fair(sched_slice(cfs_rq, se), se);
}

#include "pelt.h"
#ifdef CONFIG_SMP

static int select_idle_sibling(struct task_struct *p, int prev_cpu, int cpu);
static unsigned long task_h_load(struct task_struct *p);
static unsigned long capacity_of(int cpu);

/* Give new sched_entity start runnable values to heavy its load in infant time */
/*
 * IAMROOT, 2023.04.01:
 * - task인 경우에만 full load로 수행한다. 그렇지 않은 경우 0으로 시작한다.
 */
void init_entity_runnable_average(struct sched_entity *se)
{
	struct sched_avg *sa = &se->avg;

	memset(sa, 0, sizeof(*sa));

	/*
	 * Tasks are initialized with full load to be seen as heavy tasks until
	 * they get a chance to stabilize to their real load level.
	 * Group entities are initialized with zero load to reflect the fact that
	 * nothing has been attached to the task group yet.
	 */
/*
 * IAMROOT, 2023.04.01:
 * - papago
 *   작업은 full load 로 초기화되어 실제 로드 수준으로 안정화될 때까지 무거운 
 *   작업으로 표시됩니다.
 *   그룹 엔터티는 작업 그룹에 아직 아무 것도 연결되지 않았다는 사실을 반영하기 
 *   위해 로드가 없는 상태로 초기화됩니다.
 */
	if (entity_is_task(se))
		sa->load_avg = scale_load_down(se->load.weight);

	/* when this task enqueue'ed, it will contribute to its cfs_rq's load_avg */
}

static void attach_entity_cfs_rq(struct sched_entity *se);

/*
 * With new tasks being created, their initial util_avgs are extrapolated
 * based on the cfs_rq's current util_avg:
 *
 *   util_avg = cfs_rq->util_avg / (cfs_rq->load_avg + 1) * se.load.weight
 *
 * However, in many cases, the above util_avg does not give a desired
 * value. Moreover, the sum of the util_avgs may be divergent, such
 * as when the series is a harmonic series.
 *
 * To solve this problem, we also cap the util_avg of successive tasks to
 * only 1/2 of the left utilization budget:
 *
 *   util_avg_cap = (cpu_scale - cfs_rq->avg.util_avg) / 2^n
 *
 * where n denotes the nth task and cpu_scale the CPU capacity.
 *
 * For example, for a CPU with 1024 of capacity, a simplest series from
 * the beginning would be like:
 *
 *  task  util_avg: 512, 256, 128,  64,  32,   16,    8, ...
 * cfs_rq util_avg: 512, 768, 896, 960, 992, 1008, 1016, ...
 *
 * Finally, that extrapolated util_avg is clamped to the cap (util_avg_cap)
 * if util_avg > util_avg_cap.
 */
/*
 * IAMROOT, 2023.04.13:
 * - TODO
 */
void post_init_entity_util_avg(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	struct sched_avg *sa = &se->avg;
	long cpu_scale = arch_scale_cpu_capacity(cpu_of(rq_of(cfs_rq)));
	long cap = (long)(cpu_scale - cfs_rq->avg.util_avg) / 2;

	if (cap > 0) {
		if (cfs_rq->avg.util_avg != 0) {
			sa->util_avg  = cfs_rq->avg.util_avg * se->load.weight;
			sa->util_avg /= (cfs_rq->avg.load_avg + 1);

			if (sa->util_avg > cap)
				sa->util_avg = cap;
		} else {
			sa->util_avg = cap;
		}
	}

	sa->runnable_avg = sa->util_avg;

	if (p->sched_class != &fair_sched_class) {
		/*
		 * For !fair tasks do:
		 *
		update_cfs_rq_load_avg(now, cfs_rq);
		attach_entity_load_avg(cfs_rq, se);
		switched_from_fair(rq, p);
		 *
		 * such that the next switched_to_fair() has the
		 * expected state.
		 */
		se->avg.last_update_time = cfs_rq_clock_pelt(cfs_rq);
		return;
	}

	attach_entity_cfs_rq(se);
}

#else /* !CONFIG_SMP */
void init_entity_runnable_average(struct sched_entity *se)
{
}
void post_init_entity_util_avg(struct task_struct *p)
{
}
static void update_tg_load_avg(struct cfs_rq *cfs_rq)
{
}
#endif /* CONFIG_SMP */

/*
 * Update the current task's runtime statistics.
 */
 /*
  * IAMROOT, 2022.12.17:
  * - 1. 실행시간 누적.
  *   2. vruntime 누적
  *   3. min_vruntime 갱신
  *   4. cfs bandwidth 처리(cfs bandwidth enable 시에만)
  */
static void update_curr(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	u64 now = rq_clock_task(rq_of(cfs_rq));
	u64 delta_exec;

	if (unlikely(!curr))
		return;

	delta_exec = now - curr->exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	curr->exec_start = now;

	schedstat_set(curr->statistics.exec_max,
		      max(delta_exec, curr->statistics.exec_max));

	/*
	 * IAMROOT, 2022.12.17:
	 * - entity 실행한 시간 누적
	 */
	curr->sum_exec_runtime += delta_exec;
	schedstat_add(cfs_rq->exec_clock, delta_exec);

	/*
	 * IAMROOT, 2022.12.17:
	 * - delta_exec를 가지고 curr entity의 load_weight 를 적용해서
	 *   vruntime을 구해와서 누적시킨다.
	 */
	curr->vruntime += calc_delta_fair(delta_exec, curr);
	update_min_vruntime(cfs_rq);

	/*
	 * IAMROOT, 2022.12.17:
	 * - loop 의 처음(task가 entity인 경우)만 호출
	 */
	if (entity_is_task(curr)) {
		struct task_struct *curtask = task_of(curr);

		trace_sched_stat_runtime(curtask, delta_exec, curr->vruntime);
		cgroup_account_cputime(curtask, delta_exec);
		account_group_exec_runtime(curtask, delta_exec);
	}

/*
 * IAMROOT, 2022.12.22:
 * - cfs bandwidth 처리
 */
	account_cfs_rq_runtime(cfs_rq, delta_exec);
}

static void update_curr_fair(struct rq *rq)
{
	update_curr(cfs_rq_of(&rq->curr->se));
}

/*
 * IAMROOT, 2023.01.28:
 * - schedule stat 처리
 */
static inline void
update_stats_wait_start(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	u64 wait_start, prev_wait_start;

	if (!schedstat_enabled())
		return;

	wait_start = rq_clock(rq_of(cfs_rq));
	prev_wait_start = schedstat_val(se->statistics.wait_start);

	if (entity_is_task(se) && task_on_rq_migrating(task_of(se)) &&
	    likely(wait_start > prev_wait_start))
		wait_start -= prev_wait_start;

	__schedstat_set(se->statistics.wait_start, wait_start);
}


/*
 * IAMROOT, 2023.01.28:
 * - stats 처리.
 */
static inline void
update_stats_wait_end(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct task_struct *p;
	u64 delta;

	if (!schedstat_enabled())
		return;

	/*
	 * When the sched_schedstat changes from 0 to 1, some sched se
	 * maybe already in the runqueue, the se->statistics.wait_start
	 * will be 0.So it will let the delta wrong. We need to avoid this
	 * scenario.
	 */
	if (unlikely(!schedstat_val(se->statistics.wait_start)))
		return;

	delta = rq_clock(rq_of(cfs_rq)) - schedstat_val(se->statistics.wait_start);

	if (entity_is_task(se)) {
		p = task_of(se);
		if (task_on_rq_migrating(p)) {
			/*
			 * Preserve migrating task's wait time so wait_start
			 * time stamp can be adjusted to accumulate wait time
			 * prior to migration.
			 */
			__schedstat_set(se->statistics.wait_start, delta);
			return;
		}
		trace_sched_stat_wait(p, delta);
	}

	__schedstat_set(se->statistics.wait_max,
		      max(schedstat_val(se->statistics.wait_max), delta));
	__schedstat_inc(se->statistics.wait_count);
	__schedstat_add(se->statistics.wait_sum, delta);
	__schedstat_set(se->statistics.wait_start, 0);
}

static inline void
update_stats_enqueue_sleeper(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct task_struct *tsk = NULL;
	u64 sleep_start, block_start;

	if (!schedstat_enabled())
		return;

	sleep_start = schedstat_val(se->statistics.sleep_start);
	block_start = schedstat_val(se->statistics.block_start);

	if (entity_is_task(se))
		tsk = task_of(se);

	if (sleep_start) {
		u64 delta = rq_clock(rq_of(cfs_rq)) - sleep_start;

		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > schedstat_val(se->statistics.sleep_max)))
			__schedstat_set(se->statistics.sleep_max, delta);

		__schedstat_set(se->statistics.sleep_start, 0);
		__schedstat_add(se->statistics.sum_sleep_runtime, delta);

		if (tsk) {
			account_scheduler_latency(tsk, delta >> 10, 1);
			trace_sched_stat_sleep(tsk, delta);
		}
	}
	if (block_start) {
		u64 delta = rq_clock(rq_of(cfs_rq)) - block_start;

		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > schedstat_val(se->statistics.block_max)))
			__schedstat_set(se->statistics.block_max, delta);

		__schedstat_set(se->statistics.block_start, 0);
		__schedstat_add(se->statistics.sum_sleep_runtime, delta);

		if (tsk) {
			if (tsk->in_iowait) {
				__schedstat_add(se->statistics.iowait_sum, delta);
				__schedstat_inc(se->statistics.iowait_count);
				trace_sched_stat_iowait(tsk, delta);
			}

			trace_sched_stat_blocked(tsk, delta);

			/*
			 * Blocking time is in units of nanosecs, so shift by
			 * 20 to get a milliseconds-range estimation of the
			 * amount of time that the task spent sleeping:
			 */
			if (unlikely(prof_on == SLEEP_PROFILING)) {
				profile_hits(SLEEP_PROFILING,
						(void *)get_wchan(tsk),
						delta >> 20);
			}
			account_scheduler_latency(tsk, delta >> 10, 0);
		}
	}
}

/*
 * Task is being enqueued - update stats:
 */
static inline void
update_stats_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	if (!schedstat_enabled())
		return;

	/*
	 * Are we enqueueing a waiting task? (for current tasks
	 * a dequeue/enqueue event is a NOP)
	 */
	if (se != cfs_rq->curr)
		update_stats_wait_start(cfs_rq, se);

	if (flags & ENQUEUE_WAKEUP)
		update_stats_enqueue_sleeper(cfs_rq, se);
}

static inline void
update_stats_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{

	if (!schedstat_enabled())
		return;

	/*
	 * Mark the end of the wait period if dequeueing a
	 * waiting task:
	 */
	if (se != cfs_rq->curr)
		update_stats_wait_end(cfs_rq, se);

	if ((flags & DEQUEUE_SLEEP) && entity_is_task(se)) {
		struct task_struct *tsk = task_of(se);
		unsigned int state;

		/* XXX racy against TTWU */
		state = READ_ONCE(tsk->__state);
		if (state & TASK_INTERRUPTIBLE)
			__schedstat_set(se->statistics.sleep_start,
				      rq_clock(rq_of(cfs_rq)));
		if (state & TASK_UNINTERRUPTIBLE)
			__schedstat_set(se->statistics.block_start,
				      rq_clock(rq_of(cfs_rq)));
	}
}

/*
 * We are picking a new current task - update its stats:
 */
static inline void
update_stats_curr_start(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/*
	 * We are starting a new run period:
	 */
	se->exec_start = rq_clock_task(rq_of(cfs_rq));
}

/**************************************************
 * Scheduling class queueing methods:
 */

#ifdef CONFIG_NUMA_BALANCING
/*
 * Approximate time to scan a full NUMA task in ms. The task scan period is
 * calculated based on the tasks virtual memory size and
 * numa_balancing_scan_size.
 */
/*
 * IAMROOT, 2023.06.17:
 * - 1 sec ~ 60 sec
 */
unsigned int sysctl_numa_balancing_scan_period_min = 1000;
unsigned int sysctl_numa_balancing_scan_period_max = 60000;

/* Portion of address space to scan in MB */
/*
 * IAMROOT, 2023.06.17:
 * - 256MB
 *   0이면 disable과 마찬가지다.
 */
unsigned int sysctl_numa_balancing_scan_size = 256;

/* Scan @scan_size MB every @scan_period after an initial @scan_delay in ms */
/*
 * IAMROOT, 2023.06.17:
 * - default 1 sec
 */
unsigned int sysctl_numa_balancing_scan_delay = 1000;

struct numa_group {
	refcount_t refcount;

	spinlock_t lock; /* nr_tasks, tasks */
	int nr_tasks;
	pid_t gid;
	int active_nodes;

	struct rcu_head rcu;
	unsigned long total_faults;
/*
 * IAMROOT, 2023.06.24:
 * - 전체 노드에 대해서 fault cpu를 더해놓은 통계값.
 */
	unsigned long max_faults_cpu;
	/*
	 * Faults_cpu is used to decide whether memory should move
	 * towards the CPU. As a consequence, these stats are weighted
	 * more by CPU use than by memory faults.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   Faults_cpu는 메모리가 CPU 쪽으로 이동해야 하는지 여부를 결정하는
 *   데 사용됩니다. 결과적으로 이러한 통계는 메모리 fault보다 CPU 사용에
 *   더 많은 가중치를 부여합니다.
 * - numa_faults 참고
 */
	unsigned long *faults_cpu;
	unsigned long faults[];
};

/*
 * For functions that can be called in multiple contexts that permit reading
 * ->numa_group (see struct task_struct for locking rules).
 */
static struct numa_group *deref_task_numa_group(struct task_struct *p)
{
	return rcu_dereference_check(p->numa_group, p == current ||
		(lockdep_is_held(__rq_lockp(task_rq(p))) && !READ_ONCE(p->on_cpu)));
}

/*
 * IAMROOT, 2023.06.17:
 * - @p가 curr인경우에만 @p의 numa_group을 가져온다.
 */
static struct numa_group *deref_curr_numa_group(struct task_struct *p)
{
	return rcu_dereference_protected(p->numa_group, p == current);
}

static inline unsigned long group_faults_priv(struct numa_group *ng);
static inline unsigned long group_faults_shared(struct numa_group *ng);

/*
 * IAMROOT, 2023.06.17:
 * - @p의 rss에 대한 numa scan 갯수를 return 한다.
 * - 한번에 scan할 scan pages를 구하고, 이걸로 몇번 돌려야 rss를 스캔할지를
 *   계산해낸다.
 */
static unsigned int task_nr_scan_windows(struct task_struct *p)
{
	unsigned long rss = 0;
	unsigned long nr_scan_pages;

	/*
	 * Calculations based on RSS as non-present and empty pages are skipped
	 * by the PTE scanner and NUMA hinting faults should be trapped based
	 * on resident pages
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   존재하지 않는 빈 페이지로 RSS를 기반으로 하는 계산은 PTE 스캐너에서 
 *   건너뛰고 NUMA 힌트 fault는 상주 페이지를 기반으로 트랩되어야 합니다. 
 * - ex) PAGE_SIZE = 12
 *   nr_scan_pages * 2^8 = 256 * 256 = 65536 = 64kb = 2^16
 */
	nr_scan_pages = sysctl_numa_balancing_scan_size << (20 - PAGE_SHIFT);
	rss = get_mm_rss(p->mm);
	if (!rss)
		rss = nr_scan_pages;

/*
 * IAMROOT, 2023.06.17:
 * - ex) rss = 2^16 + 1, nr_scan_pages = 2^16 
 *   rss = 2^17
 *   return  = 2
 *
 *   ex) rss = 2^10, nr_scan_pages = 2^16
 *   rss = 2^16
 *   return = 1
 *
 *   ex) rss = 2^20 + 1, nr_scan_pages = 2^16
 *   rss = 2^21
 *   return = 2^5
 */
	rss = round_up(rss, nr_scan_pages);
	return rss / nr_scan_pages;
}

/* For sanity's sake, never scan more PTEs than MAX_SCAN_WINDOW MB/sec. */
/*
 * IAMROOT, 2023.06.17:
 * - 2560MB
 */
#define MAX_SCAN_WINDOW 2560

/*
 * IAMROOT, 2023.06.17:
 * - kernel의 목표는 1초에 최대 2560MB처리를 하기를 원한다. 이에 대한 scan 간격을 정한다.
 *   scan이 너무 자주 일어나지 않게 floor라는 제한을 둔다. 기본 100ms
 *
 * - task rss가 충분히 작을경우. 
 *   task = 384MB - > scan = 1000 / 2 = 500ms. max(500, 100)
 *   제한인 100ms보다 크므로 500ms마다 천천히 scan하면된다.
 *   
 * - task rss가 충분히 클경우
 *   task = 5120MB -> scan = 1000 / 20 = 50ms. -> max(50, 100)
 *   제한인 100ms보다 작으므로 너무 자주 scan하는 상황이 된다. 제한인 100ms로 제한한다.
 *
 * IAMROOT, 2023.06.23:
 * - @p의 rss 크기 만큼을 256MB씩 scan 하고 총 소요시간이 1초(...period_min) 라고
 *   했을때 한번 scan 에 사용할 수 있는 최소 주기.
 *   단 rss 가 2560MB를 초과 하면 2560MB 에 해당하는 주기(100ms) 사용.
 *                                                               floor
 *               |----|----|----|----|----|----|----|----|----|----|----|----|
 * - rss크기(MB): 0   256  512  768 1024 1280 1536 1792 2048 2304 2560 2816 3072
 *   scan횟수:      1    2    3    4    5    6    7    8    9    10   11   12
 *   scan(ms):    1000  500  333  250  200  166  143  125  111  100  100  100
 */
static unsigned int task_scan_min(struct task_struct *p)
{
	unsigned int scan_size = READ_ONCE(sysctl_numa_balancing_scan_size);
	unsigned int scan, floor;
	unsigned int windows = 1;

/*
 * IAMROOT, 2023.06.17:
 * - scan_size 단위로 window 개수 증가.
 *   windows : scan_size만큼 할수있는 횟수
 */
	if (scan_size < MAX_SCAN_WINDOW)
		windows = MAX_SCAN_WINDOW / scan_size;
/*
 * IAMROOT, 2023.06.17:
 * - ex) scan_size = 256MB
 *   windows = 10, floor = 100
 *   floor : 1초당 2560MB를 기준. windows 256MB -> 100ms라는 개념으로 floor = 100
 *   ex) windows 2560MB -> floor = 1000(ms)
 *       windows 256MB -> floor = 100(ms)
 */
	floor = 1000 / windows;

/*
 * IAMROOT, 2023.06.17:
 * - scan = (최소시간) / (rss fullscan시 필요한 횟수)
 *        = scan 1번당 쓸수있는 최소시간
 */
	scan = sysctl_numa_balancing_scan_period_min / task_nr_scan_windows(p);
/*
 * IAMROOT, 2023.06.17:
 * - ex) task의 rss가 256MB보다 크면 계산된 scan이되고, 그게 아니면 100ms가 된다.
 *   task = 384MB - > scan = 1000 / 2 = 500ms. return 500ms
 *   task = 5120MB -> scan = 1000 / 20 = 50ms. return 100ms
 */
	return max_t(unsigned int, floor, scan);
}

/*
 * IAMROOT, 2023.06.24:
 * - task_scan_min 값 기준으로 설정하고 refcount 가 크고 shared 비율이 높을수록
 *   상향 조정된다.
 */
static unsigned int task_scan_start(struct task_struct *p)
{
	unsigned long smin = task_scan_min(p);
	unsigned long period = smin;
	struct numa_group *ng;

	/* Scale the maximum scan period with the amount of shared memory. */
	rcu_read_lock();
	ng = rcu_dereference(p->numa_group);
	if (ng) {
		unsigned long shared = group_faults_shared(ng);
		unsigned long private = group_faults_priv(ng);

		period *= refcount_read(&ng->refcount);
		period *= shared + 1;
		period /= private + shared + 1;
	}
	rcu_read_unlock();

	return max(smin, period);
}

/*
 * IAMROOT, 2023.06.17:
 * @return max msec
 * - numa scan max값을 구해온다.
 */
static unsigned int task_scan_max(struct task_struct *p)
{
	unsigned long smin = task_scan_min(p);
	unsigned long smax;
	struct numa_group *ng;

	/* Watch for min being lower than max due to floor calculations */
/*
 * IAMROOT, 2023.06.17:
 * - task = 384MB - > scan = 1000 / 2 = 500ms. max(500, 100)
 *   min = 500ms, max = 30000ms
 *   
 * - task = 5120MB -> scan = 1000 / 20 = 50ms. -> max(50, 100)
 *   min = 100ms, max = 3000ms
 */
	/*
	 * IAMROOT, 2023.06.23:
	 * - rss 전체를 스캔하는 시간을 60초(...period_max)라 보고 256MB 크기 단위로
	 *   스캔했을때 한번 scan 간격.
	 * - ex. rss 크기가 2560MB 라면 task_nr_scan_windows = 10
	 *   60000 / 10 = 6000(ms) = 6(sec)
	 */
	smax = sysctl_numa_balancing_scan_period_max / task_nr_scan_windows(p);

	/* Scale the maximum scan period with the amount of shared memory. */
	ng = deref_curr_numa_group(p);
	if (ng) {
/*
 * IAMROOT, 2023.06.17:
 * - ng에 대한 shared, priv faults avg sum을 구해온다.
 */
		unsigned long shared = group_faults_shared(ng);
		unsigned long private = group_faults_priv(ng);
		unsigned long period = smax;
/*
 * IAMROOT, 2023.06.17:
 * - period * refcount * (shared + 1) / (private + shared + 1) 
 *   shared + private에서 shared의 비율을 구하는데, 이때 refcount로 곱하기보정한다.
 *   shared 하는 thread가 많으면 smax가 늘어날수있다.
 * - ex) shared = 10%, ref = 10개라면 10 * 10 = 100% 해서 priv와 동등해지는 개념(변함없음)
 *       shared = 20%, ref = 10개라면 10 * 20 = 200% 해서 priv 보다 2배가 될수있는 개념
 */
		/*
		 * IAMROOT, 2023.06.23:
		 * - refcount 를 shared에 곱한 값이 shared에 private를 더한 값보다
		 *   크게 되면 smax 값이 상향 조정된다. 즉 shared thread가 많으면
		 *   refcount가 커져서 원래 smax 값보다 크게 조정한다. 하지만 원래값
		 *   보다 작은 경우에는 원래값을 유지한다.
		 */
		period *= refcount_read(&ng->refcount);
		period *= shared + 1;
		period /= private + shared + 1;

		smax = max(smax, period);
	}

	/*
	 * IAMROOT, 2023.06.23:
	 * - rss 값이 매우 큰 경우(256MB * 60 보다 클 때)는 smin 보다 작아 질 수 있다
	 */
	return max(smin, smax);
}

static void account_numa_enqueue(struct rq *rq, struct task_struct *p)
{
	rq->nr_numa_running += (p->numa_preferred_nid != NUMA_NO_NODE);
	rq->nr_preferred_running += (p->numa_preferred_nid == task_node(p));
}

static void account_numa_dequeue(struct rq *rq, struct task_struct *p)
{
	rq->nr_numa_running -= (p->numa_preferred_nid != NUMA_NO_NODE);
	rq->nr_preferred_running -= (p->numa_preferred_nid == task_node(p));
}

/* Shared or private faults. */
/*
 * IAMROOT, 2023.06.17:
 * - shared, private해서 2개라는 의미.
 */
#define NR_NUMA_HINT_FAULT_TYPES 2

/* Memory and CPU locality */
/*
 * IAMROOT, 2023.06.17:
 * - memory shared
 *   memory private
 *   cpu shared
 *   cpu private
 *   해서 총 4개라는 의미.
 */
#define NR_NUMA_HINT_FAULT_STATS (NR_NUMA_HINT_FAULT_TYPES * 2)

/* Averaged statistics, and temporary buffers. */
/*
 * IAMROOT, 2023.06.17:
 * - 임시버퍼를 한쌍을 더 둬서 2배를 하는 개념.
 */
#define NR_NUMA_HINT_FAULT_BUCKETS (NR_NUMA_HINT_FAULT_STATS * 2)

pid_t task_numa_group_id(struct task_struct *p)
{
	struct numa_group *ng;
	pid_t gid = 0;

	rcu_read_lock();
	ng = rcu_dereference(p->numa_group);
	if (ng)
		gid = ng->gid;
	rcu_read_unlock();

	return gid;
}

/*
 * The averaged statistics, shared & private, memory & CPU,
 * occupy the first half of the array. The second half of the
 * array is for current counters, which are averaged into the
 * first set by task_numa_placement.
 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   평균 통계, 공유 및 private, 메모리 및 CPU가 어레이의 전반부를 차지합니다. 
 *   배열의 두 번째 절반은 task_numa_placement에 의해 첫 번째 세트로 
 *   평균화되는 현재 카운터용입니다.
 *   
 * @priv 1이면 priv, 0이면 shared
 * - @s, @nid, @priv에 대한 idx를 계산한다.
 *
 * IAMROOT, 2023.06.22:
 * - 각각의 numa 노드에 대해 faults_stats(mem,cpu,membuf,cpubuf)별
 *   fault_type(priv,shared)별 index를 반환.
 *   node가 4개이면 총 32(4*4*2)개 index 가 있다.
 */
static inline int task_faults_idx(enum numa_faults_stats s, int nid, int priv)
{
	return NR_NUMA_HINT_FAULT_TYPES * (s * nr_node_ids + nid) + priv;
}

/*
 * IAMROOT, 2023.06.22:
 * - Return: @nid에 대한 NUMA_MEM stats의 shared와 private type faults 합
 */
static inline unsigned long task_faults(struct task_struct *p, int nid)
{
	if (!p->numa_faults)
		return 0;

	return p->numa_faults[task_faults_idx(NUMA_MEM, nid, 0)] +
		p->numa_faults[task_faults_idx(NUMA_MEM, nid, 1)];
}

/*
 * IAMROOT, 2023.06.24:
 * - @group에 대한 @nid의 전체 memory node fault값(private + shared fault)
 *
 * IAMROOT, 2023.07.02:
 * - Return: task(@p) 의 numa_group 에서 @nid 에 해당하는 NUMA_MEM stats 의
 *           faults 값 반환
 */
static inline unsigned long group_faults(struct task_struct *p, int nid)
{
	struct numa_group *ng = deref_task_numa_group(p);

	if (!ng)
		return 0;

	return ng->faults[task_faults_idx(NUMA_MEM, nid, 0)] +
		ng->faults[task_faults_idx(NUMA_MEM, nid, 1)];
}

/*
 * IAMROOT, 2023.06.24:
 * - @group에 대한 @nid의 전체 memory node fault cpu값(private + shared fault)
 *
 * IAMROOT, 2023.07.02:
 * - Return: @group 에서 @nid 에 해당하는 NUMA_CPU stats 의 faults 값 반환
 *
 * - XXX faults_cpu 가 NUMA_CPU stats의 시작을 가리키므로 idx 계산에 NUMA_MEM
 *   를 사용했지만 실지로는 NUMA_CPU 의 faults 에 대한 idx가 된다.
 */
static inline unsigned long group_faults_cpu(struct numa_group *group, int nid)
{
	return group->faults_cpu[task_faults_idx(NUMA_MEM, nid, 0)] +
		group->faults_cpu[task_faults_idx(NUMA_MEM, nid, 1)];
}

/*
 * IAMROOT, 2023.06.17:
 * - NUMA_MEM의 @ng에 속한 모든 node에 대한 priv fault avg sum을 구한다.
 */
static inline unsigned long group_faults_priv(struct numa_group *ng)
{
	unsigned long faults = 0;
	int node;

	for_each_online_node(node) {
		faults += ng->faults[task_faults_idx(NUMA_MEM, node, 1)];
	}

	return faults;
}

/*
 * IAMROOT, 2023.06.17:
 * - NUMA_MEM의 @ng에 속한 모든 node에 대한 shared fault avg sum를 구한다.
 */
static inline unsigned long group_faults_shared(struct numa_group *ng)
{
	unsigned long faults = 0;
	int node;

	for_each_online_node(node) {
		faults += ng->faults[task_faults_idx(NUMA_MEM, node, 0)];
	}

	return faults;
}

/*
 * A node triggering more than 1/3 as many NUMA faults as the maximum is
 * considered part of a numa group's pseudo-interleaving set. Migrations
 * between these nodes are slowed down, to allow things to settle down.
 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   최대값의 1/3보다 많은 NUMA 결함을 트리거하는 노드는 numa 그룹의
 *   의사 인터리빙 세트의 일부로 간주됩니다. 이러한 노드 간의
 *   마이그레이션은 속도가 느려지므로 문제가 해결됩니다.
 */
#define ACTIVE_NODE_FRACTION 3

/*
 * IAMROOT, 2023.06.24:
 * - max_faults_cpu의 1/3배 초과로 cpu fault가 많이 일어난경우.
 *   즉 @ng fault개수에서 @nid의 fault cpu 비중이 많은경우이다.
 * - 전체 노드대비 비율로 되기때문에 acitve인 node가 있고, inactive인 node가 있을것이다.
 * - 비율상 active node는 최대 2개.
 */
static bool numa_is_active_node(int nid, struct numa_group *ng)
{
	return group_faults_cpu(ng, nid) * ACTIVE_NODE_FRACTION > ng->max_faults_cpu;
}

/* Handle placement on systems where not all nodes are directly connected. */
/*
 * IAMROOT. 2023.06.22:
 * - google-translate
 * 모든 노드가 직접 연결되지 않은 시스템에서 배치를 처리합니다.
 *
 * - Return: 각 online node에 대하여 task @p(또는 @p가 속한 그룹)에 대한 faults 값에
 *           dist 비율(@nid와 가까운 노드 일수록 높은 비율)을 적용하여 모두 더한 score.
 * - sched_numa_topology_type 에 따른 반환값
 *   @nid 와 가장 먼 노드는 제외
 *   1. NUMA_DIRECT - 0
 *   2. NUMA_BACKPLANE - @maxdist보다 가까운 online 노드들의 faults 합
 *   3. NUMA_GLUELESS_MESH - 가까울 수록 높은 비율을 적용한 online 노드들의 faults 합
 */
static unsigned long score_nearby_nodes(struct task_struct *p, int nid,
					int maxdist, bool task)
{
	unsigned long score = 0;
	int node;

	/*
	 * All nodes are directly connected, and the same distance
	 * from each other. No need for fancy placement algorithms.
	 */
	/*
	 * IAMROOT. 2023.06.22:
	 * - google-translate
	 * 모든 노드는 직접 연결되어 있으며 서로 같은 거리에 있습니다. 멋진 배치 알고리즘이
	 * 필요하지 않습니다.
	 */
	if (sched_numa_topology_type == NUMA_DIRECT)
		return 0;

	/*
	 * This code is called for each node, introducing N^2 complexity,
	 * which should be ok given the number of nodes rarely exceeds 8.
	 */
	/*
	 * IAMROOT. 2023.06.22:
	 * - google-translate
	 * 이 코드는 각 노드에 대해 호출되며 N^2 복잡성을 도입하며 노드 수가 8을 거의
	 * 초과하지 않는 경우 괜찮을 것입니다.
	 *
	 * - 각 online node에 대하여 task @p(또는 @p가 속한 그룹)에 대한 faults 값에
	 *   dist 비율(@nid와 가까운 노드 일수록 높은 비율)을 적용하여 모두 더한다.
	 */
	for_each_online_node(node) {
		unsigned long faults;
		int dist = node_distance(nid, node);

		/*
		 * The furthest away nodes in the system are not interesting
		 * for placement; nid was already counted.
		 */
		/*
		 * IAMROOT. 2023.06.22:
		 * - google-translate
		 * 시스템에서 가장 멀리 떨어진 노드는 배치에 관심이 없습니다. nid는 이미
		 * 계산되었습니다.
		 *
		 * - local node와 가장 먼 node는 제외
		 */
		if (dist == sched_max_numa_distance || node == nid)
			continue;

		/*
		 * On systems with a backplane NUMA topology, compare groups
		 * of nodes, and move tasks towards the group with the most
		 * memory accesses. When comparing two nodes at distance
		 * "hoplimit", only nodes closer by than "hoplimit" are part
		 * of each group. Skip other nodes.
		 */
		/*
		 * IAMROOT. 2023.06.22:
		 * - google-translate
		 * 백플레인 NUMA 토폴로지가 있는 시스템에서 노드 그룹을 비교하고 메모리
		 * 액세스가 가장 많은 그룹으로 작업을 이동합니다. "hoplimit" 거리에 있는
		 * 두 노드를 비교할 때 "hoplimit"보다 가까운 노드만 각 그룹의
		 * 일부입니다. 다른 노드를 건너뜁니다.
		 */
		if (sched_numa_topology_type == NUMA_BACKPLANE &&
					dist >= maxdist)
			continue;

		/* Add up the faults from nearby nodes. */
		if (task)
			faults = task_faults(p, node);
		else
			faults = group_faults(p, node);

		/*
		 * On systems with a glueless mesh NUMA topology, there are
		 * no fixed "groups of nodes". Instead, nodes that are not
		 * directly connected bounce traffic through intermediate
		 * nodes; a numa_group can occupy any set of nodes.
		 * The further away a node is, the less the faults count.
		 * This seems to result in good task placement.
		 */
		/*
		 * IAMROOT. 2023.06.22:
		 * - google-translate
		 * glueless mesh NUMA 토폴로지가 있는 시스템에는 고정된 "노드 그룹"이
		 * 없습니다. 대신 직접 연결되지 않은 노드는 중간 노드를 통해 트래픽을
		 * 바운스합니다. numa_group은 모든 노드 집합을 차지할 수 있습니다.
		 * 노드가 멀리 떨어져 있을수록 오류 수가 적습니다. 이것은 좋은 작업
		 * 배치를 초래하는 것 같습니다.
		 *
		 * - @nid에 가까운 노드 일수록 faults 반영 비율이 높다.
		 */
		if (sched_numa_topology_type == NUMA_GLUELESS_MESH) {
			faults *= (sched_max_numa_distance - dist);
			faults /= (sched_max_numa_distance - LOCAL_DISTANCE);
		}

		score += faults;
	}

	return score;
}

/*
 * These return the fraction of accesses done by a particular task, or
 * task group, on a particular numa node.  The group weight is given a
 * larger multiplier, in order to group tasks together that are almost
 * evenly spread out between numa nodes.
 */
/*
 * IAMROOT. 2023.07.02:
 * - google-translate
 * 특정 numa 노드에서 특정 작업 또는 작업 그룹이 수행한 액세스의 일부를
 * 반환합니다. 그룹 가중치에는 numa 노드 간에 거의 균등하게 분산된 작업을 함께
 * 그룹화하기 위해 더 큰 승수가 지정됩니다.
 *
 * - sched_numa_topology_type 에 따른 faults 값
 *   가장 먼 노드는 제외
 *   1. NUMA_DIRECT - @p의 @nid에 해당하는 faults 값
 *   2. NUMA_BACKPLANE - @dist보다 가까운 online 노드들의 faults 합
 *   3. NUMA_GLUELESS_MESH - 가까울 수록 높은 비율을 적용한 online 노드들의 faults 합
 *
 * - Return: total_faults 에 대한 faults 의 비율
 *
 */
static inline unsigned long task_weight(struct task_struct *p, int nid,
					int dist)
{
	unsigned long faults, total_faults;

	if (!p->numa_faults)
		return 0;

	total_faults = p->total_numa_faults;

	if (!total_faults)
		return 0;

	faults = task_faults(p, nid);
	faults += score_nearby_nodes(p, nid, dist, true);

	return 1000 * faults / total_faults;
}

/*
 * IAMROOT, 2023.07.04:
 * - faults 값 계산에서 group faults 값을 사용하는 것 외에는 task_weight 과 동일하다
 */
static inline unsigned long group_weight(struct task_struct *p, int nid,
					 int dist)
{
	struct numa_group *ng = deref_task_numa_group(p);
	unsigned long faults, total_faults;

	if (!ng)
		return 0;

	total_faults = ng->total_faults;

	if (!total_faults)
		return 0;

	faults = group_faults(p, nid);
	faults += score_nearby_nodes(p, nid, dist, false);

	return 1000 * faults / total_faults;
}

/*
 * IAMROOT, 2023.06.24:
 * @p       @page를 numa fault 한 task. (사실상 current)
 * @src_nid @page의 원래 nodeid
 * @dst_cpu @page가 migrate할 node의 cpu(사실상 thiscpu)
 * - @page의 cpupid를 this로 고치고,
 *   @dst_cpu로 numa migrate가 가능한지 확인한다.
 *   1. scan 횟수가 적은경우 true
 *   2. 
 *   3. private fault인경우 true.
 *   4. dst_nid의 fault cpu이 src_nid fault cpu보다 3배초과인경우 true
 *   5. fault cpu / falut mem의 비율이  dst가 1.33배이상 많은경우 true.
 *      아니면 false
 *
 * IAMROOT, 2023.07.01:
 * - @p: current
 *   @page: fault 가 발생한 page
 *   @src_nid: @page에 설정된 node id
 *   @dst_cpu: raw_smp_processor_id
 *
 * IAMROOT, 2023.07.11:
 * - @page를 @src_nid 에서 @dst_cpu의 node로 migration 해야 하나?
 * - 다음 경우 migration 하라는 의미의 true 를 반환한다.
 *   1. 첫번째 fault
 *   2. private fault 인 경우(이전 fault 발생 task 와 현재 task가 같으면 private)
 *      1. numa_scan_seq 4 이하
 *      2. 연속으로 같은 node의 cpu에서 @page에 접근하였다.
 *   3. shared fault
 *      1. task가 속한 node group의 task 들이 memory 접근시 사용한 cpu가 dst_nid 의
 *         cpu 인 경우가 scr_nid 의 cpu인 경우보다 3배 초과로 많다.
 *      2. cpu 사용만 봤을때 3배를 넘지 않는다면 cpu/mem 비율이 src 보다 큰지 확인한다
 *         cpu/mem node group fault 비율이 dst가 src 보다 1.33배 크다
 */
bool should_numa_migrate_memory(struct task_struct *p, struct page * page,
				int src_nid, int dst_cpu)
{
	struct numa_group *ng = deref_curr_numa_group(p);
	int dst_nid = cpu_to_node(dst_cpu);
	int last_cpupid, this_cpupid;

	this_cpupid = cpu_pid_to_cpupid(dst_cpu, current->pid);
	last_cpupid = page_cpupid_xchg_last(page, this_cpupid);

	/*
	 * Allow first faults or private faults to migrate immediately early in
	 * the lifetime of a task. The magic number 4 is based on waiting for
	 * two full passes of the "multi-stage node selection" test that is
	 * executed below.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   첫 번째 fault 또는 private fault가 task lifetime 초기에 즉시 마이그레이션되도록 
 *   허용합니다. 매직 넘버 4는 아래에서 실행되는 다단계 노드 선택 테스트의
 *   두 번의 전체 패스를 기다리는 것을 기반으로 합니다.
 * - 초기에는(numa_scan 횟수 2번이하), 설정이 안됬거나, @p가 last_cpuid로 설정되있다면
 *   return true.
 *   
 */
	if ((p->numa_preferred_nid == NUMA_NO_NODE || p->numa_scan_seq <= 4) &&
	    (cpupid_pid_unset(last_cpupid) || cpupid_match_pid(p, last_cpupid)))
		return true;

	/*
	 * Multi-stage node selection is used in conjunction with a periodic
	 * migration fault to build a temporal task<->page relation. By using
	 * a two-stage filter we remove short/unlikely relations.
	 *
	 * Using P(p) ~ n_p / n_t as per frequentist probability, we can equate
	 * a task's usage of a particular page (n_p) per total usage of this
	 * page (n_t) (in a given time-span) to a probability.
	 *
	 * Our periodic faults will sample this probability and getting the
	 * same result twice in a row, given these samples are fully
	 * independent, is then given by P(n)^2, provided our sample period
	 * is sufficiently short compared to the usage pattern.
	 *
	 * This quadric squishes small probabilities, making it less likely we
	 * act on an unlikely task<->page relation.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   다단계 노드 선택은 임시 작업<->페이지 관계를 구축하기 위해 주기적
 *   마이그레이션 fault와 함께 사용됩니다. 2단계 필터를 사용하여 짧거나
 *   가능성이 없는 관계를 제거합니다.  빈도주의자 확률에 따라
 *   P(p) ~ n_p / n_t를 사용하여 이 페이지의 총 사용량(n_t)당 특정
 *   페이지의 작업 사용량(n_p)을 확률과 동일시할 수 있습니다.
 *
 *   주기적인 fault은 이 확률을 샘플링하고 이러한 샘플이 완전히 독립적인
 *   경우 연속으로 두 번 동일한 결과를 얻고 샘플 기간이 사용 패턴에 비해
 *   충분히 짧은 경우 P(n)^2로 제공됩니다.
 *
 *   이 2차 함수는 작은 확률을 찌그러뜨려 우리가 있을 법하지 않은
 *   작업<->페이지 관계에서 행동할 가능성을 줄입니다.
 *
 * - last_cpupid가 설정되있었고, 그게 dst_nid가 아니라면 return false
 *
 * IAMROOT, 2023.07.11:
 * - dst_nid 로 @page를 migration 하려고 하는데 연속으로 dst_nid 의 cpu들 중에서
 *   @page에 접근 하지 않았다면 migration 하지 않는다.
 */
	if (!cpupid_pid_unset(last_cpupid) &&
				cpupid_to_nid(last_cpupid) != dst_nid)
		return false;

	/* Always allow migrate on private faults */
/*
 * IAMROOT, 2023.06.24:
 * - @p의 last pid와 @page의 lastpid가 같으면 private fault라고 판단하여
 *   return true.
 */
	if (cpupid_match_pid(p, last_cpupid))
		return true;

	/* A shared fault, but p->numa_group has not been set up yet. */
/*
 * IAMROOT, 2023.06.24:
 * - numa group이 아직 설정이 안됬으면 그냥 migrate 수행.
 */
	if (!ng)
		return true;

	/*
	 * Destination node is much more heavily used than the source
	 * node? Allow migration.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - @ng dst_nid가 src_nid보다 3배초과로 fault cpu 가 많이 발생했으면 옮긴다.
 *
 * IAMROOT, 2023.07.11:
 * - task가 속한 node group의 task 들이 memory 접근시 사용한 cpu가
 *   dst_nid 의 cpu 인 경우가 scr_nid 의 cpu인 경우보다 3배 초과로 많다.
 */
	if (group_faults_cpu(ng, dst_nid) > group_faults_cpu(ng, src_nid) *
					ACTIVE_NODE_FRACTION)
		return true;

	/*
	 * Distribute memory according to CPU & memory use on each node,
	 * with 3/4 hysteresis to avoid unnecessary memory migrations:
	 *
	 * faults_cpu(dst)   3   faults_cpu(src)
	 * --------------- * - > ---------------
	 * faults_mem(dst)   4   faults_mem(src)
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   불필요한 메모리 마이그레이션을 방지하기 위해 3/4 히스테리시스를
 *   사용하여 각 노드의 CPU 및 메모리 사용에 따라 메모리를 분배합니다.
 *
 *   faults_cpu(dst)   3   faults_cpu(src)
 *   --------------- * - > ---------------
 *   faults_mem(dst)   4   faults_mem(src)
 *
 * - dst의 fault가 src보다 1.33배 이상 많다면 dst로 옮긴다.
 *
 * IAMROOT, 2023.07.08:
 * - memory less node를 가지고 있는 numa system(ex.threadripper)에서는 메모리를
 *   가지고 있는 노드가 가지고 있지 않는 노드보다 1.33배 이상 access가 많은 경우
 *   해당 페이지를 옮긴다.
 */
	return group_faults_cpu(ng, dst_nid) * group_faults(p, src_nid) * 3 >
	       group_faults_cpu(ng, src_nid) * group_faults(p, dst_nid) * 4;
}

/*
 * 'numa_type' describes the node at the moment of load balancing.
 */
/*
 * IAMROOT, 2023.07.13:
 * - numa_classify() 참고
 */
enum numa_type {
	/* The node has spare capacity that can be used to run more tasks.  */
	node_has_spare = 0,
	/*
	 * The node is fully used and the tasks don't compete for more CPU
	 * cycles. Nevertheless, some tasks might wait before running.
	 */
	node_fully_busy,
	/*
	 * The node is overloaded and can't provide expected CPU cycles to all
	 * tasks.
	 */
	node_overloaded
};

/* Cached statistics for all CPUs within a node */
struct numa_stats {
	unsigned long load;
	unsigned long runnable;
	unsigned long util;
	/* Total compute capacity of CPUs on a node */
	unsigned long compute_capacity;
	unsigned int nr_running;
	unsigned int weight;
	enum numa_type node_type;
	int idle_cpu;
};

static inline bool is_core_idle(int cpu)
{
#ifdef CONFIG_SCHED_SMT
	int sibling;

	for_each_cpu(sibling, cpu_smt_mask(cpu)) {
		if (cpu == sibling)
			continue;

		if (!idle_cpu(sibling))
			return false;
	}
#endif

	return true;
}

struct task_numa_env {
	struct task_struct *p;

	int src_cpu, src_nid;
	int dst_cpu, dst_nid;

	struct numa_stats src_stats, dst_stats;

	int imbalance_pct;
	int dist;

	struct task_struct *best_task;
	long best_imp;
	int best_cpu;
};

static unsigned long cpu_load(struct rq *rq);
static unsigned long cpu_runnable(struct rq *rq);
static unsigned long cpu_util(int cpu);
static inline long adjust_numa_imbalance(int imbalance,
					int dst_running, int dst_weight);

/*
 * IAMROOT, 2023.07.04:
 * - Return:
 *   1. node_overloaded - 다음 경우 노드가 과부하 되었다고 볾
 *      1. running task 수가 전체 online node cpu 수보다 많고
 *         전체 capa 가 전체 util avg 합에 가중치를 준 값보다 작다.
 *      2. running task 수가 전체 online node cpu 수보다 많고
 *         전체 capa 에 가중치를 준 값이 전체 runnable avg 합보다 작다.
 *   2. node_has_spare - 다음 경우 노드가 여유가 있다고 볾
 *      1. running task 수가 전체 online node cpu 수보다 작다.
 *      2. 전체 capa 가 전체 util avg 합에 가중치를 준 값보다 크고
 *         전체 capa 에 가중치를 준 값이 전체 runnable avg 합보다 작다.
 *   3. node_fully_busy - 다음 경우 fully_busy
 *      1. running task 수가 전체 online node cpu 수 이상이고
 *         전체 capa 가 전체 util avg 합에 가중치를 준 값보다 크고
 *         전체 capa 에 가중치를 준 값이 전체 runnable avg 합보다 작다.
 */
static inline enum
numa_type numa_classify(unsigned int imbalance_pct,
			 struct numa_stats *ns)
{
	if ((ns->nr_running > ns->weight) &&
	    (((ns->compute_capacity * 100) < (ns->util * imbalance_pct)) ||
	     ((ns->compute_capacity * imbalance_pct) < (ns->runnable * 100))))
		return node_overloaded;

	if ((ns->nr_running < ns->weight) ||
	    (((ns->compute_capacity * 100) > (ns->util * imbalance_pct)) &&
	     ((ns->compute_capacity * imbalance_pct) > (ns->runnable * 100))))
		return node_has_spare;

	return node_fully_busy;
}

#ifdef CONFIG_SCHED_SMT
/* Forward declarations of select_idle_sibling helpers */
static inline bool test_idle_cores(int cpu, bool def);
/*
 * IAMROOT, 2023.07.07:
 * - @idle_core = -1로 처음에 진입해서 @cpu가 idle_core인지 검사하고, 이후로
 *   idle_core가 한번 찾아지고 @idle_core >= 0 인채로 입력되면 그냥 검색을
 *   안하고 return @idle_core를 하는 개념이다.
 *
 * - Return:
 *   1. idle_core 설정 조건이고 cpu 의 smt가 전부 idle 이면 cpu
 *   2. 그렇지 않으면 전달된 @idle_core
 */
static inline int numa_idle_core(int idle_core, int cpu)
{
	/*
	 * IAMROOT, 2023.07.04:
	 * - smt 가 아니거나 이미 idle_core cpu가 설정되었거나 llc_shared_domain
	 *   에 idle_core가 없으면 @idle_core 그냥 반환
	 */
	if (!static_branch_likely(&sched_smt_present) ||
	    idle_core >= 0 || !test_idle_cores(cpu, false))
		return idle_core;

	/*
	 * Prefer cores instead of packing HT siblings
	 * and triggering future load balancing.
	 */
	/*
	 * IAMROOT. 2023.07.04:
	 * - google-translate
	 * HT 형제를 묶고 향후 로드 밸런싱을 트리거하는 대신 코어를 선호합니다.
	 */
	if (is_core_idle(cpu))
		idle_core = cpu;

	return idle_core;
}
#else
static inline int numa_idle_core(int idle_core, int cpu)
{
	return idle_core;
}
#endif

/*
 * Gather all necessary information to make NUMA balancing placement
 * decisions that are compatible with standard load balancer. This
 * borrows code and logic from update_sg_lb_stats but sharing a
 * common implementation is impractical.
 */
/*
 * IAMROOT. 2023.07.04:
 * - google-translate
 * 표준 로드 밸런서와 호환되는 NUMA 균형 배치 결정을 내리는 데 필요한 모든 정보를
 * 수집합니다. 이것은 update_sg_lb_stats에서 코드와 논리를 차용하지만 공통 구현을
 * 공유하는 것은 비실용적입니다.
 */
/*
 * IAMROOT, 2023.07.07:
 * - @nid에속한 cpus의 load등을 @ns에 합산하며, weight, node_type 를 설정한다.
 *   @find_idle이 true인경우 idle_cpu까지 찾는다.
 *
 */
static void update_numa_stats(struct task_numa_env *env,
			      struct numa_stats *ns, int nid,
			      bool find_idle)
{
	int cpu, idle_core = -1;

	memset(ns, 0, sizeof(*ns));
	ns->idle_cpu = -1;

	rcu_read_lock();
	for_each_cpu(cpu, cpumask_of_node(nid)) {
		struct rq *rq = cpu_rq(cpu);

		ns->load += cpu_load(rq);
		ns->runnable += cpu_runnable(rq);
		ns->util += cpu_util(cpu);
		ns->nr_running += rq->cfs.h_nr_running;
		ns->compute_capacity += capacity_of(cpu);

		if (find_idle && !rq->nr_running && idle_cpu(cpu)) {
			if (READ_ONCE(rq->numa_migrate_on) ||
			    !cpumask_test_cpu(cpu, env->p->cpus_ptr))
				continue;

			/*
			 * IAMROOT, 2023.07.04:
			 * - 첫번째 idle cpu 를 idle_cpu로설정
			 */
			if (ns->idle_cpu == -1)
				ns->idle_cpu = cpu;

			idle_core = numa_idle_core(idle_core, cpu);
		}
	}
	rcu_read_unlock();

	ns->weight = cpumask_weight(cpumask_of_node(nid));

	ns->node_type = numa_classify(env->imbalance_pct, ns);

	/*
	 * IAMROOT, 2023.07.07:
	 * - idle_core가 있으면 idle_core의 첫번째 cpu, 없으면 첫번째 idle cpu
	 */
	if (idle_core >= 0)
		ns->idle_cpu = idle_core;
}

/*
 * IAMROOT, 2023.07.04:
 * - @env->dst_cpu를 @env->best_cpu로 설정
 *   1. env->dst_cpu 가 이미 migrate 중이다.
 *      1. 노드의 다른 idle cpu를 찾았다
 *         2.1 수행
 *      2. 노드의 다른 idle cpu를 못찾았다
 *         return
 *   2. env->dst_cpu 가 이미 migrate 중이 아니다.
 *      1. env->best_task = @p, env->best_imp = @imp,
 *         env->best_cpu = env->dst_cpu 설정
 */
static void task_numa_assign(struct task_numa_env *env,
			     struct task_struct *p, long imp)
{
	struct rq *rq = cpu_rq(env->dst_cpu);

	/* Check if run-queue part of active NUMA balance. */
	/*
	 * IAMROOT. 2023.07.04:
	 * - google-translate
	 * active NUMA balance의 run-queue 부분인지 확인하십시오.
	 *
	 * - 이미 migrate 중이라면 노드에서 다른 idle cpu를 찾는다.
	 */
	if (env->best_cpu != env->dst_cpu && xchg(&rq->numa_migrate_on, 1)) {
		int cpu;
		int start = env->dst_cpu;

		/* Find alternative idle CPU. */
		for_each_cpu_wrap(cpu, cpumask_of_node(env->dst_nid), start) {
			if (cpu == env->best_cpu || !idle_cpu(cpu) ||
			    !cpumask_test_cpu(cpu, env->p->cpus_ptr)) {
				continue;
			}

			env->dst_cpu = cpu;
			rq = cpu_rq(env->dst_cpu);
			if (!xchg(&rq->numa_migrate_on, 1))
				goto assign;
		}

		/* Failed to find an alternative idle CPU */
		return;
	}

assign:
	/*
	 * Clear previous best_cpu/rq numa-migrate flag, since task now
	 * found a better CPU to move/swap.
	 */
	/*
	 * IAMROOT. 2023.07.04:
	 * - google-translate
	 * 이전의 best_cpu/rq numa-migrate 플래그를 지웁니다. 작업이 이제 이동/스왑하기에
	 * 더 나은 CPU를 찾았기 때문입니다.
	 *
	 * - 이전에 best_cpu를 찾아놨지만 @env의 dst_cpu와 다른상태. best_cpu를 교체해야되는
	 *   상황이므로 기존의 best_cpu를 지우고 numa migrte flag도 지운다.
	 */
	if (env->best_cpu != -1 && env->best_cpu != env->dst_cpu) {
		rq = cpu_rq(env->best_cpu);
		WRITE_ONCE(rq->numa_migrate_on, 0);
	}

/*
 * IAMROOT, 2023.07.13:
 * - best_task도 바꾼다. 기존 best_task는 refdown, 새로운 best_task는 ref up
 */
	if (env->best_task)
		put_task_struct(env->best_task);
	if (p)
		get_task_struct(p);

	env->best_task = p;
	env->best_imp = imp;
	env->best_cpu = env->dst_cpu;
}

/*
 * IAMROOT, 2023.07.04:
 * - task가 dst 로 옮겨가고 dst 와 src의 load 불균형이 더 심해졌나?
 * - Return: load 불균형이 더 심혀졌으면 true
 */
static bool load_too_imbalanced(long src_load, long dst_load,
				struct task_numa_env *env)
{
	long imb, old_imb;
	long orig_src_load, orig_dst_load;
	long src_capacity, dst_capacity;

	/*
	 * The load is corrected for the CPU capacity available on each node.
	 *
	 * src_load        dst_load
	 * ------------ vs ---------
	 * src_capacity    dst_capacity
	 */
	/*
	 * IAMROOT, 2023.07.15:
	 * - 이동후의 imbalance 값 계산
	 */
	src_capacity = env->src_stats.compute_capacity;
	dst_capacity = env->dst_stats.compute_capacity;

	imb = abs(dst_load * src_capacity - src_load * dst_capacity);

	/*
	 * IAMROOT, 2023.07.15:
	 * - 이동전의 imbalance 값 계산
	 */
	orig_src_load = env->src_stats.load;
	orig_dst_load = env->dst_stats.load;

	old_imb = abs(orig_dst_load * src_capacity - orig_src_load * dst_capacity);

	/* Would this change make things worse? */
	return (imb > old_imb);
}

/*
 * Maximum NUMA importance can be 1998 (2*999);
 * SMALLIMP @ 30 would be close to 1998/64.
 * Used to deter task migration.
 */
/*
 * IAMROOT. 2023.07.15:
 * - google-translate
 * 최대 NUMA 중요도는 1998(2*999)일 수 있습니다. SMALLIMP @ 30은 1998/64에
 * 가깝습니다. 작업 마이그레이션을 방지하는 데 사용됩니다.
 */
#define SMALLIMP	30

/*
 * This checks if the overall compute and NUMA accesses of the system would
 * be improved if the source tasks was migrated to the target dst_cpu taking
 * into account that it might be best if task running on the dst_cpu should
 * be exchanged with the source task
 */
/*
 * IAMROOT. 2023.07.04:
 * - google-translate
 * 이는 dst_cpu에서 실행 중인 작업을 소스 작업과 교환해야 하는 경우가 가장 좋을 수
 * 있다는 점을 고려하여 소스 작업이 대상 dst_cpu로 마이그레이션된 경우 시스템의
 * 전체 컴퓨팅 및 NUMA 액세스가 개선되는지 확인합니다.
 *
 * - @env->p: current
 *   @env->dst_cpu: @env->dst_nid 의 cpu중 현재 loop의 cpu
 *   @taskimp: dst node 와 src node의 fault 차이
 *   @groupimp: dst node 와 src node의 group fault 차이
 *   @maymove: src->dst(단방향 이동후 balance 예측)로 이동해도 됨
 *   moveimp: 전달된 단방향 imp(fault 차이)를 백업한 변수
 *   imp: 양방향 imp 계산후 가중치까지 적용되는 변수
 * - env->dst_nid 의 cpu(env->dst_cpu)들을 비교하여 그중 best_cpu를
 *   설정하기위해 task_numa_assign 호출
 * - 단방향 이동(src->dst)이 swap 보다 이득이면 best_cpu 만 설정
 *   swap이 이득이면 best_cpu와 best_task를 설정
 * - Return: stopsearch(더이상 검색이 필요 없으면 true)
 */
static bool task_numa_compare(struct task_numa_env *env,
			      long taskimp, long groupimp, bool maymove)
{
	struct numa_group *cur_ng, *p_ng = deref_curr_numa_group(env->p);
	struct rq *dst_rq = cpu_rq(env->dst_cpu);
	long imp = p_ng ? groupimp : taskimp;
	struct task_struct *cur;
	long src_load, dst_load;
	int dist = env->dist;
	long moveimp = imp;
	long load;
	bool stopsearch = false;

	if (READ_ONCE(dst_rq->numa_migrate_on))
		return false;

	rcu_read_lock();
	cur = rcu_dereference(dst_rq->curr);
	/*
	 * IAMROOT, 2023.07.05:
	 * - dst_cpu의 curr가 종료중이거나 idle인 경우
	 */
	if (cur && ((cur->flags & PF_EXITING) || is_idle_task(cur)))
		cur = NULL;

	/*
	 * Because we have preemption enabled we can get migrated around and
	 * end try selecting ourselves (current == env->p) as a swap candidate.
	 */
	/*
	 * IAMROOT. 2023.07.04:
	 * - google-translate
	 * 우리는 선점을 활성화했기 때문에 마이그레이션을 수행하고 스왑 후보로 자신(current
	 * == env->p)을 선택하려고 시도할 수 있습니다.
	 */
	if (cur == env->p) {
		stopsearch = true;
		goto unlock;
	}

	if (!cur) {
		/*
		 * IAMROOT, 2023.07.05:
		 * - dst_cpu의 curr가 종료중이거나 idle task이면
		 *   현재 loop의 fault 비교치가 best보다 좋다면 단방향 예측이 참인
		 *   조건만으로 idle cpu 할당을 시도해 본다.
		 */
		if (maymove && moveimp >= env->best_imp)
			goto assign;
		else
			goto unlock;
	}

	/* Skip this swap candidate if cannot move to the source cpu. */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 소스 CPU로 이동할 수 없는 경우 이 스왑 후보를 건너뜁니다.
	 *
	 * - swap 후보이므로 dst->src 가 가능한지도 체크
	 */
	if (!cpumask_test_cpu(env->src_cpu, cur->cpus_ptr))
		goto unlock;

	/*
	 * Skip this swap candidate if it is not moving to its preferred
	 * node and the best task is.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 선호하는 노드로 이동하지 않고 최선의 작업인 경우 이 스왑 후보를 건너뜁니다.
	 *
	 * - best_task는 preffered_nid로 이동하지만 현재 loop의 swap 후보는
	 *   그렇지 않을 때 건너뛴다.
	 */
	if (env->best_task &&
	    env->best_task->numa_preferred_nid == env->src_nid &&
	    cur->numa_preferred_nid != env->src_nid) {
		goto unlock;
	}

	/*
	 * "imp" is the fault differential for the source task between the
	 * source and destination node. Calculate the total differential for
	 * the source task and potential destination task. The more negative
	 * the value is, the more remote accesses that would be expected to
	 * be incurred if the tasks were swapped.
	 *
	 * If dst and source tasks are in the same NUMA group, or not
	 * in any group then look only at task weights.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * "imp"는 소스 노드와 대상 노드 사이의 소스 작업에 대한 결함 차이입니다. 원본
	 * 작업과 잠재적인 대상 작업에 대한 총 차이를 계산합니다. 값이 음수일수록 태스크가
	 * 교환된 경우 발생할 것으로 예상되는 원격 액세스가 더 많습니다.
	 *
	 * dst 및 소스 작업이 동일한 NUMA 그룹에 있거나 어떤 그룹에도 없는 경우
	 * 작업 가중치만 확인합니다.
	 *
	 * - current task 가 preferred node로 이동시 faults 차이와
	 *   preferred node의 cpu 중 현재 loop의 cpu에서 current task 의 노드로
	 *   이동시 faults 차이의 합을 구한다. 값이 음수일수록 원격 억세스가 많다.
	 *   즉 src task 를 dst 로 이동하려는데 swap 으로 이동한다면 dst -> src
	 *   의 이동에 대한 faults도 염두에 두어야 한다.
	 */
	cur_ng = rcu_dereference(cur->numa_group);
	if (cur_ng == p_ng) {
		/*
		 * IAMROOT, 2023.07.05:
		 * - 같은 그룹에 있을 경우 task만 계산
		 * - src task 가 dst node 로 옮겨 갈 경우 faults 차이와
		 *   dst task 가 src node 로 옮겨 갈 경우 faults 차이를 더한다.
		 */
		imp = taskimp + task_weight(cur, env->src_nid, dist) -
		      task_weight(cur, env->dst_nid, dist);
		/*
		 * Add some hysteresis to prevent swapping the
		 * tasks within a group over tiny differences.
		 */
		/*
		 * IAMROOT. 2023.07.05:
		 * - google-translate
		 * 약간의 히스테리시스를 추가하여 그룹 내에서 작은 차이로 작업이 바뀌는 것을
		 * 방지합니다.
		 */
		if (cur_ng)
			imp -= imp / 16;
	} else {
		/*
		 * Compare the group weights. If a task is all by itself
		 * (not part of a group), use the task weight instead.
		 */
		/*
		 * IAMROOT. 2023.07.05:
		 * - google-translate
		 * 그룹 가중치를 비교합니다. 작업이 그룹의 일부가 아닌 그 자체인 경우,
		 * 대신 작업 가중치를 사용합니다.
		 */
		if (cur_ng && p_ng)
			imp += group_weight(cur, env->src_nid, dist) -
			       group_weight(cur, env->dst_nid, dist);
		else
			imp += task_weight(cur, env->src_nid, dist) -
			       task_weight(cur, env->dst_nid, dist);
	}

	/* Discourage picking a task already on its preferred node */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 선호하는 노드에 이미 있는 작업을 선택하지 않도록 합니다.
	 *
	 * - swap에 의한 dst->src 이동을 고려하면 이미 dst가 preferred node에 있는
	 *   경우는 imp를 감소시켜 선택되지 않도록 한다.
	 */
	if (cur->numa_preferred_nid == env->dst_nid)
		imp -= imp / 16;

	/*
	 * Encourage picking a task that moves to its preferred node.
	 * This potentially makes imp larger than it's maximum of
	 * 1998 (see SMALLIMP and task_weight for why) but in this
	 * case, it does not matter.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 선호하는 노드로 이동하는 작업을 선택하도록 권장합니다. 이로 인해 잠재적으로
	 * imp가 최대값인 1998보다 커질 수 있지만(이유는 SMALLIMP 및 task_weight 참조) 이
	 * 경우에는 중요하지 않습니다.
	 *
	 * - swap에 의한 dst->src 이동을 고려하면 preferred node로 이동하는 경우이므로
	 *   imp를 증가시켜 선택하도록 권장
	 */
	if (cur->numa_preferred_nid == env->src_nid)
		imp += imp / 8;

	/*
	 * IAMROOT, 2023.07.07:
	 * - 단방향(src->dst) 이동은 괜찮지만 swap 할 경우는 fault가 감소하는 경향이고
	 *   현재 루프의 imp 가 best 보다 크다면 swap 하지 않고 단방향 migration 하도록
	 *   cur을 NULL로 설정하고 현재 loop cpu로 할당을 시도해본다.
	 */
	if (maymove && moveimp > imp && moveimp > env->best_imp) {
		imp = moveimp;
		cur = NULL;
		goto assign;
	}

	/*
	 * Prefer swapping with a task moving to its preferred node over a
	 * task that is not.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 그렇지 않은 작업보다 선호하는 노드로 이동하는 작업으로 교체하는 것을 선호합니다.
	 *
	 * - 현재 루프의 task는 swap시 dst->src 이동에서 preferred_nid 로 이동하지만
	 *   best는 그렇지 않을 경우 현재 loop cpu와 task 로 할당하기 위해 jump
	 */
	if (env->best_task && cur->numa_preferred_nid == env->src_nid &&
	    env->best_task->numa_preferred_nid != env->src_nid) {
		goto assign;
	}

	/*
	 * If the NUMA importance is less than SMALLIMP,
	 * task migration might only result in ping pong
	 * of tasks and also hurt performance due to cache
	 * misses.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * NUMA 중요도가 SMALLIMP보다 낮으면 작업 마이그레이션으로 인해 작업의 핑퐁만
	 * 발생하고 캐시 누락으로 인해 성능이 저하될 수 있습니다.
	 */
	/*
	 * IAMROOT, 2023.07.06:
	 * - 중요도(imp=fault차이)가 30 미만이거나 best_imp보다 16이상 크지 않다면
	 *   무시하고 다음 루프 진행
	 */
	if (imp < SMALLIMP || imp <= env->best_imp + SMALLIMP / 2)
		goto unlock;

	/*
	 * In the overloaded case, try and keep the load balanced.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 과부하된 경우 부하 균형을 유지하도록 노력하십시오.
	 *
	 * - XXX src 와 dst load의 차이를 구하는 이유는 swap 할 것이므로
	 *   차이를 더하거나 빼야 한다.
	 */
	load = task_h_load(env->p) - task_h_load(cur);
	if (!load)
		goto assign;

	dst_load = env->dst_stats.load + load;
	src_load = env->src_stats.load - load;

	/*
	 * IAMROOT, 2023.07.07:
	 * - swap후 load 불균형이 더 심해진다면 다음 루프를 진행한다
	 */
	if (load_too_imbalanced(src_load, dst_load, env))
		goto unlock;

assign:
	/* Evaluate an idle CPU for a task numa move. */
	/*
	 * IAMROOT, 2023.07.06:
	 * - 현재 레이블(assign)에서 cur 가 NULL 인 조건
	 *   1. maymove(src->dst 단방향 예측)가 참이어야 한다.
	 *   2. 현재 루프의 중요도(imp) 가 best 보다 크다
	 *   - 위 두조건이 모두 충족되고 아래 중 하나에 해당
	 *     1. dst_cpu의 curr가 종료중이거나 idle task이고
	 *     2. swap 할 경우는 fault가 감소하는 경향이고
	 * - env->dst_cpu 설정(cur 가 NULL 인 조건에서). 순서대로
	 *   1. cache된 idle cpu(dst_stats.idle_cpu)가 현재도 idle이면 캐쉬된
	 *      idle_cpu
	 *   2. dst_cpu가 현재 idle이면 dst_cpu 유지
	 *   3. best_cpu가 현재 idle 이면 best_cpu
	 *   4. idle cpu를 못찾았다면 cache 되었던 idle_cpu나 현재 루프의 dst_cpu
	 */
	if (!cur) {
		int cpu = env->dst_stats.idle_cpu;

		/* Nothing cached so current CPU went idle since the search. */
		/*
		 * IAMROOT. 2023.07.06:
		 * - google-translate
		 * 캐시된 것이 없으므로 검색 이후 현재 CPU가 유휴 상태가 되었습니다.
		 *
		 * - XXX 주석의 의미는?
		 *   종료중이거나 idle task가 idle cpu로 될 것으로 예상함?
		 */
		if (cpu < 0)
			cpu = env->dst_cpu;

		/*
		 * If the CPU is no longer truly idle and the previous best CPU
		 * is, keep using it.
		 */
		/*
		 * IAMROOT. 2023.07.06:
		 * - google-translate
		 * CPU가 더 이상 실제로 유휴 상태가 아니고 이전에 best CPU가
		 * 있는 경우 계속 사용하십시오.
		 */
		if (!idle_cpu(cpu) && env->best_cpu >= 0 &&
		    idle_cpu(env->best_cpu)) {
			cpu = env->best_cpu;
		}

		env->dst_cpu = cpu;
	}

	task_numa_assign(env, cur, imp);

	/*
	 * If a move to idle is allowed because there is capacity or load
	 * balance improves then stop the search. While a better swap
	 * candidate may exist, a search is not free.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 용량이 있거나 부하 균형이 개선되어 유휴 상태로 전환이 허용되면 검색을
	 * 중지합니다. 더 나은 스왑 후보가 존재할 수 있지만 검색은 무료가 아닙니다.
	 *
	 * - @maymove가 true 로 전달되었을 경우 검색 중단 조건(idle cpu의 경우
	 *   단방향 조건만 충족하면 된다.)
	 *   best_cpu가 idle 인 경우
	 */
	if (maymove && !cur && env->best_cpu >= 0 && idle_cpu(env->best_cpu))
		stopsearch = true;

	/*
	 * If a swap candidate must be identified and the current best task
	 * moves its preferred node then stop the search.
	 */
	/*
	 * IAMROOT. 2023.07.05:
	 * - google-translate
	 * 스왑 후보를 식별해야 하고 현재 최상의 작업이 선호하는 노드를 이동하면 검색을
	 * 중지합니다.
	 *
	 * - @maymove가 false 로 전달되었을 경우 검색 중단 조건
	 *   src->dst 이동은 load 불균형이 나빠지지만 best_task가 설정되었고
	 *   src_nid가 preffered 이다
	 * - best_task 가 설정되는 조건
	 *   1. cur 가 NULL 이 아니다
	 *   2. 현재 루프의 task는 swap시 dst->src 이동에서 preferred_nid 로
	 *      이동하지만 best는 그렇지 않을 경우
	 *   3. swap 후에는 불균형이 유지나 개선된다.
	 */
	if (!maymove && env->best_task &&
	    env->best_task->numa_preferred_nid == env->src_nid) {
		stopsearch = true;
	}
unlock:
	/*
	 * IAMROOT, 2023.07.08:
	 * - task_numa_assign을 호출 하지 않고 다음 루프로 검색을 진행하는 조건
	 *   1. cur == env->p (dst_rq->curr == current).(예외로 검색도 중단)
	 *   2. cur 가 종료중이거나 idle task
	 *      1. @maymove 가 false
	 *      2. moveimp >= env->best_imp
	 *   3. cur가 src_cpu로 이동할 수 없는 경우
	 *   4. cur는 preffered_nid 로 이동하지 않지만 best_task는 그럴경우
	 *   5. faults 차이가 많지 않을때
	 *   6. swap후 로드 불균형이 더 심해질 경우
	 */
	rcu_read_unlock();

	return stopsearch;
}

/*
 * IAMROOT, 2023.07.08:
 * - 현재 task(@env->p)의 dst_nid cpu 중 numa balance에 최적인 cpu를 찾아서
 *   @env->best_cpu에 설정한다.
 *
 * - best_cpu 설정(task migration) 조건
 *   1. 이동후 balance가 유지되는 idle cpu
 *   2. swap 이나 단방향 이동후 접근도(imp) 와 load balance 가 좋아지는 cpu
 *      (preferred_nid 로 이동하는 cpu 우선)
 */
static void task_numa_find_cpu(struct task_numa_env *env,
				long taskimp, long groupimp)
{
	bool maymove = false;
	int cpu;

	/*
	 * If dst node has spare capacity, then check if there is an
	 * imbalance that would be overruled by the load balancer.
	 */
	/*
	 * IAMROOT. 2023.07.04:
	 * - google-translate
	 * dst 노드에 여유 용량이 있는 경우 로드 밸런서에 의해 무시되는 불균형이 있는지
	 * 확인하십시오.
	 */
	if (env->dst_stats.node_type == node_has_spare) {
		unsigned int imbalance;
		int src_running, dst_running;

		/*
		 * Would movement cause an imbalance? Note that if src has
		 * more running tasks that the imbalance is ignored as the
		 * move improves the imbalance from the perspective of the
		 * CPU load balancer.
		 * */
		/*
		 * IAMROOT. 2023.07.04:
		 * - google-translate
		 * 움직임으로 인해 불균형이 발생합니까? src에 실행 중인 작업이 더 많은 경우
		 * 이동으로 인해 CPU 로드 밸런서의 관점에서 불균형이 개선되므로 불균형이
		 * 무시됩니다.
		 */
		src_running = env->src_stats.nr_running - 1;
		dst_running = env->dst_stats.nr_running + 1;
		imbalance = max(0, dst_running - src_running);
		imbalance = adjust_numa_imbalance(imbalance, dst_running,
							env->dst_stats.weight);

		/* Use idle CPU if there is no imbalance */
		/*
		 * IAMROOT. 2023.07.06:
		 * - google-translate
		 * 불균형이 없으면 유휴 CPU 사용
		 *
		 * - 균형상태일때
		 *   1) 이전에 idle_cpu를 찾았다면
		 *     해당 idle_cpu를 dst_cpu로 사용한다.
		 *     idle_core로의 이동이므로 @env의 best_task는 refdown만 시키고 새로
		 *     들어가는 task는 없는 의미에서 NULL이된다. 또한 균형 상태이므로 impl
		 *     값또한 0으로 들어간다.
		 *   2) idle_cpu를 못찾았으면
		 *     maymove만 true로 설정한다.
		 *
		 * - 이동후 balance로 판단되면 idle_cpu를 best_cpu로 지정
		 */
		if (!imbalance) {
			maymove = true;
			if (env->dst_stats.idle_cpu >= 0) {
				env->dst_cpu = env->dst_stats.idle_cpu;
				task_numa_assign(env, NULL, 0);
				return;
			}
		}
	} else {
		/*
		 * IAMROOT, 2023.07.04:
		 * - node_type이 overloaded 이거나 fully_busy
		 */
		long src_load, dst_load, load;
		/*
		 * If the improvement from just moving env->p direction is better
		 * than swapping tasks around, check if a move is possible.
		 */
		/*
		 * IAMROOT. 2023.07.04:
		 * - google-translate
		 * env->p 방향으로 이동하는 것의 개선이 작업을 교환하는 것보다 낫다면 이동이
		 * 가능한지 확인하십시오.
		 */
		load = task_h_load(env->p);
		dst_load = env->dst_stats.load + load;
		src_load = env->src_stats.load - load;
		/*
		 * IAMROOT, 2023.07.04:
		 * - dst로 이동(src->dst 단방향)후 이전보다 load 불균형이 개선된다면
		 *   maymove = true
		 */
		maymove = !load_too_imbalanced(src_load, dst_load, env);
	}

	/*
	 * IAMROOT, 2023.07.12:
	 * - task_numa_compare 에 maymove가 true로 호출되는 경우
	 *   1. has_spare node type
	 *      이동후 balance 이지만 dst에 캐쉬된 idle_cpu가 없는 경우
	 *      balance로 판단되는 경우는 아래와 같다.
	 *      1. 이동후 dst node에 running task가 node의 cpu 숫자의 25%이상이고
	 *         src 가 dst보다 running task가 같거나 많다
	 *      2. 이동후 dst node에 running task가 node의 cpu 숫자의 25%미만이고
	 *         dst가 src 보다 running task 가 3개이상 많지 않다.
	 *      위 두경우지만 idle cpu는 없는 경우이므로 2번은 제외된다.
	 *   2. overloaded, fully_busy node type
	 *      현재 task 가 dst node로 이동후(src->dst 단방향 이동) load 불균형이
	 *      개선됨
	 */
	for_each_cpu(cpu, cpumask_of_node(env->dst_nid)) {
		/* Skip this CPU if the source task cannot migrate */
		if (!cpumask_test_cpu(cpu, env->p->cpus_ptr))
			continue;

		env->dst_cpu = cpu;
		if (task_numa_compare(env, taskimp, groupimp, maymove))
			break;
	}
}

/*
 * IAMROOT, 2023.07.14:
 * - @p를 접근이 가장많은(max_faults) memory 노드(preffered_nid)의
 *   적절한(balance 유지) cpu(best_cpu)로 옮기거나 swap 한다.
 */
static int task_numa_migrate(struct task_struct *p)
{
	struct task_numa_env env = {
		.p = p,

		.src_cpu = task_cpu(p),
		.src_nid = task_node(p),

		.imbalance_pct = 112,

		.best_task = NULL,
		.best_imp = 0,
		.best_cpu = -1,
	};
	unsigned long taskweight, groupweight;
	struct sched_domain *sd;
	long taskimp, groupimp;
	struct numa_group *ng;
	struct rq *best_rq;
	int nid, ret, dist;

	/*
	 * Pick the lowest SD_NUMA domain, as that would have the smallest
	 * imbalance and would be the first to start moving tasks about.
	 *
	 * And we want to avoid any moving of tasks about, as that would create
	 * random movement of tasks -- counter the numa conditions we're trying
	 * to satisfy here.
	 */
	/*
	 * IAMROOT. 2023.07.02:
	 * - google-translate
	 * 가장 낮은 SD_NUMA 도메인을 선택하면 불균형이 가장 적고 작업 이동을 가장 먼저
	 * 시작하게 됩니다. 그리고 우리는 작업의 임의 이동을 생성하기 때문에 작업 이동을
	 * 피하고 싶습니다. 여기서 만족시키려는 누마 조건에 반대합니다.
	 */
	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_numa, env.src_cpu));
	/*
	 * IAMROOT, 2023.07.08:
	 * - numa migrate 시는 load balance 보다 이동이 쉽게 초과 숫자의 절반만 적용
	 */
	if (sd)
		env.imbalance_pct = 100 + (sd->imbalance_pct - 100) / 2;
	rcu_read_unlock();

	/*
	 * Cpusets can break the scheduler domain tree into smaller
	 * balance domains, some of which do not cross NUMA boundaries.
	 * Tasks that are "trapped" in such domains cannot be migrated
	 * elsewhere, so there is no point in (re)trying.
	 */
	/*
	 * IAMROOT. 2023.07.02:
	 * - google-translate
	 * Cpusets는 스케줄러 도메인 트리를 더 작은 균형 도메인으로 나눌 수 있으며 그 중
	 * 일부는 NUMA 경계를 넘지 않습니다. 이러한 도메인에 "갇힌" 작업은 다른 곳으로
	 * 마이그레이션할 수 없으므로 (재)시도할 필요가 없습니다.
	 *
	 * - numa domain내에서만 migrate를 한다. sd가 없으면 numa_preferred_nid를
	 *   @p의 numa로 다시 변경후 실패처리
	 */
	if (unlikely(!sd)) {
		sched_setnuma(p, task_node(p));
		return -EINVAL;
	}

/*
 * IAMROOT, 2023.07.13:
 * - src_nid, dst_nid에 대해 통계를 작성한다.
 *   dst의경우 idle_core까지 찾는다.
 */
	env.dst_nid = p->numa_preferred_nid;
	dist = env.dist = node_distance(env.src_nid, env.dst_nid);
	taskweight = task_weight(p, env.src_nid, dist);
	groupweight = group_weight(p, env.src_nid, dist);
	update_numa_stats(&env, &env.src_stats, env.src_nid, false);
	taskimp = task_weight(p, env.dst_nid, dist) - taskweight;
	groupimp = group_weight(p, env.dst_nid, dist) - groupweight;
	update_numa_stats(&env, &env.dst_stats, env.dst_nid, true);

	/* Try to find a spot on the preferred nid. */
	/*
	 * IAMROOT, 2023.07.13:
	 * - 현재 task(@p)의 preferred_nid(memory fault가 가장많은 node) 에서
	 *   먼저 찾아 본다.
	 */
	task_numa_find_cpu(&env, taskimp, groupimp);

	/*
	 * Look at other nodes in these cases:
	 * - there is no space available on the preferred_nid
	 * - the task is part of a numa_group that is interleaved across
	 *   multiple NUMA nodes; in order to better consolidate the group,
	 *   we need to check other locations.
	 */
	/*
	 * IAMROOT. 2023.07.07:
	 * - google-translate
	 * 다음과 같은 경우 다른 노드를 살펴보십시오.
	 * - preferred_nid에 사용 가능한 공간이 없습니다.
	 * - 작업이 여러 NUMA 노드에 걸쳐 인터리브되는 numa_group의
	 *   일부입니다. 그룹을 더 잘 통합하려면 다른 위치를 확인해야 합니다.
	 *
	 * - preferred_nid 에서 best_cpu를 못찾다면 online node의 다른 노드중
	 *   faults 가 많은 노드에서 찾아본다.
	 * - best_cpu를 찾았더라도 여러 노드에 겹치는 그룹일 경우는 모든 노드에서
	 *   best_cpu보다 더 나은 cpu를 찾는다.
	 */
	ng = deref_curr_numa_group(p);
	if (env.best_cpu == -1 || (ng && ng->active_nodes > 1)) {
		for_each_online_node(nid) {
			if (nid == env.src_nid || nid == p->numa_preferred_nid)
				continue;

			dist = node_distance(env.src_nid, env.dst_nid);
			/*
			 * IAMROOT, 2023.07.07:
			 * - BACKPLANE 이고 dist가 변경되었다면 faults 값 재계산
			 *   (BACKPLANE 은 weight 계산에서 dist 이상은 건너뛴다.)
			 */
			if (sched_numa_topology_type == NUMA_BACKPLANE &&
						dist != env.dist) {
				taskweight = task_weight(p, env.src_nid, dist);
				groupweight = group_weight(p, env.src_nid, dist);
			}

			/* Only consider nodes where both task and groups benefit */
			/*
			 * IAMROOT. 2023.07.07:
			 * - google-translate
			 * 작업과 그룹 모두 이익이 되는 노드만 고려하십시오.
			 *
			 * - src 보다 faults(access) 가 많은 노드만 고려
			 */
			taskimp = task_weight(p, nid, dist) - taskweight;
			groupimp = group_weight(p, nid, dist) - groupweight;
			if (taskimp < 0 && groupimp < 0)
				continue;

			env.dist = dist;
			env.dst_nid = nid;
			update_numa_stats(&env, &env.dst_stats, env.dst_nid, true);
			task_numa_find_cpu(&env, taskimp, groupimp);
		}
	}

	/*
	 * If the task is part of a workload that spans multiple NUMA nodes,
	 * and is migrating into one of the workload's active nodes, remember
	 * this node as the task's preferred numa node, so the workload can
	 * settle down.
	 * A task that migrated to a second choice node will be better off
	 * trying for a better one later. Do not set the preferred node here.
	 */
	/*
	 * IAMROOT. 2023.07.07:
	 * - google-translate
	 * 작업이 여러 NUMA 노드에 걸쳐 있고 작업 부하의 활성 노드 중 하나로
	 * 마이그레이션되는 작업 부하의 일부인 경우 작업 부하가 안정될 수 있도록 이 노드를
	 * 작업의 기본 누마 노드로 기억하십시오. 두 번째 선택 노드로 마이그레이션된 작업은
	 * 나중에 더 나은 노드로 시도하는 것이 좋습니다. 여기에서 기본 노드를 설정하지
	 * 마십시오.
	 *
	 * - XXX 주석의 의미는?
	 *
	 * - 1. best_cpu가 설정되었다 -> best_cpu의 노드로 numa_preferred_nid 설정
	 *   2. best_cpu가 설정되지 않았다 -> src 노드로 numa_preferred_nid 설정
	 * - src 보다 나은 node를 못 찾았다면 src를 preferred로 설정하고 찾았고
	 *   원래 preferred node가 아니면 갱신한다.
	 */
	if (ng) {
		if (env.best_cpu == -1)
			nid = env.src_nid;
		else
			nid = cpu_to_node(env.best_cpu);

		if (nid != p->numa_preferred_nid)
			sched_setnuma(p, nid);
	}

	/* No better CPU than the current one was found. */
	if (env.best_cpu == -1) {
		trace_sched_stick_numa(p, env.src_cpu, NULL, -1);
		return -EAGAIN;
	}

	best_rq = cpu_rq(env.best_cpu);
	/*
	 * IAMROOT, 2023.07.14:
	 * - best_task가 NULL이면 현재 task만 best_cpu로 migration 하고
	 *   NULL이 아니면 best와 src cpu의 curr를 swap migration 한다.
	 */
	if (env.best_task == NULL) {
		ret = migrate_task_to(p, env.best_cpu);
		WRITE_ONCE(best_rq->numa_migrate_on, 0);
		if (ret != 0)
			trace_sched_stick_numa(p, env.src_cpu, NULL, env.best_cpu);
		return ret;
	}

	ret = migrate_swap(p, env.best_task, env.best_cpu, env.src_cpu);
	WRITE_ONCE(best_rq->numa_migrate_on, 0);

	if (ret != 0)
		trace_sched_stick_numa(p, env.src_cpu, env.best_task, env.best_cpu);
	put_task_struct(env.best_task);
	return ret;
}

/* Attempt to migrate a task to a CPU on the preferred node. */
/*
 * IAMROOT. 2023.07.04:
 * - google-translate
 * preferred 노드의 CPU로 작업을 마이그레이션하려고 시도합니다.
 *
 * - numa_migrate_retry 주기를 설정하고 task_numa_migrate 호출
 */
static void numa_migrate_preferred(struct task_struct *p)
{
	unsigned long interval = HZ;

	/* This task has no NUMA fault statistics yet */
/*
 * IAMROOT, 2023.07.10:
 * - 선호 node가 없거나 fault가 없으면 할게 없다. return.
 */
	if (unlikely(p->numa_preferred_nid == NUMA_NO_NODE || !p->numa_faults))
		return;

	/* Periodically retry migrating the task to the preferred node */
/*
 * IAMROOT, 2023.07.10:
 * - numa_scan_period로 next numa_migrate_retry시각을 갱신한다.
 */
	interval = min(interval, msecs_to_jiffies(p->numa_scan_period) / 16);
	p->numa_migrate_retry = jiffies + interval;

	/* Success if task is already running on preferred CPU */
/*
 * IAMROOT, 2023.07.10:
 * - 이미 선호 node라면 바꿀필요없다.
 */
	if (task_node(p) == p->numa_preferred_nid)
		return;

	/* Otherwise, try migrate to a CPU on the preferred node */
	task_numa_migrate(p);
}

/*
 * Find out how many nodes on the workload is actively running on. Do this by
 * tracking the nodes from which NUMA hinting faults are triggered. This can
 * be different from the set of nodes where the workload's memory is currently
 * located.
 */
/*
 * IAMROOT. 2023.06.24:
 * - google-translate
 * 워크로드에서 활발하게 실행 중인 노드 수를 확인합니다. NUMA 힌트 결함이
 * 트리거되는 노드를 추적하여 이를 수행하십시오. 이는 워크로드의 메모리가 현재
 * 위치한 노드 집합과 다를 수 있습니다.
 *
 * IAMROOT, 2023.07.01:
 * - active_nodes개수와 max_faults를 갱신한다.
 * - active_nodes
 *   max_faults의 1 / 3 이상인것들을 
 * - online node중 @numa_group의 faults_cpu 수가 가장 큰 node 의 1/3 보다
 *   큰 노드들의 갯수와 max_faults수를 numa_group 멤버 변수에 설정한다.
 * - NOTE active_nodes 계산에는 faults_cpu를 사용한다.
 */
static void numa_group_count_active_nodes(struct numa_group *numa_group)
{
	unsigned long faults, max_faults = 0;
	int nid, active_nodes = 0;

/*
 * IAMROOT, 2023.07.11:
 * - max_faults를 알아온다.
 */
	for_each_online_node(nid) {
		faults = group_faults_cpu(numa_group, nid);
		if (faults > max_faults)
			max_faults = faults;
	}

	for_each_online_node(nid) {
		faults = group_faults_cpu(numa_group, nid);
		if (faults * ACTIVE_NODE_FRACTION > max_faults)
			active_nodes++;
	}

	numa_group->max_faults_cpu = max_faults;
	numa_group->active_nodes = active_nodes;
}

/*
 * When adapting the scan rate, the period is divided into NUMA_PERIOD_SLOTS
 * increments. The more local the fault statistics are, the higher the scan
 * period will be for the next scan window. If local/(local+remote) ratio is
 * below NUMA_PERIOD_THRESHOLD (where range of ratio is 1..NUMA_PERIOD_SLOTS)
 * the scan period will decrease. Aim for 70% local accesses.
 */
/*
 * IAMROOT. 2023.07.02:
 * - google-translate
 * 스캔 속도를 조정할 때 기간은 NUMA_PERIOD_SLOTS 단위로 나뉩니다. 결함 통계가
 * 로컬일수록 다음 스캔 창에 대한 스캔 기간이 높아집니다. 로컬/(로컬+원격) 비율이
 * NUMA_PERIOD_THRESHOLD(비율 범위는 1..NUMA_PERIOD_SLOTS) 미만이면 스캔 기간이
 * 줄어듭니다. 70% 로컬 액세스를 목표로 합니다.
 */
#define NUMA_PERIOD_SLOTS 10
#define NUMA_PERIOD_THRESHOLD 7

/*
 * Increase the scan period (slow down scanning) if the majority of
 * our memory is already on our local node, or if the majority of
 * the page accesses are shared with other processes.
 * Otherwise, decrease the scan period.
 */
/*
 * IAMROOT. 2023.06.24:
 * - google-translate
 * 대부분의 메모리가 이미 로컬 노드에 있거나 대부분의 페이지 액세스가 다른
 * 프로세스와 공유되는 경우 스캔 기간을 늘립니다(스캔 속도 저하). 그렇지 않으면
 * 스캔 기간을 줄이십시오.
 *
 * IAMROOT, 2023.07.08:
 * - skip
 */
static void update_task_scan_period(struct task_struct *p,
			unsigned long shared, unsigned long private)
{
	unsigned int period_slot;
	int lr_ratio, ps_ratio;
	int diff;

	unsigned long remote = p->numa_faults_locality[0];
	unsigned long local = p->numa_faults_locality[1];

	/*
	 * If there were no record hinting faults then either the task is
	 * completely idle or all activity is areas that are not of interest
	 * to automatic numa balancing. Related to that, if there were failed
	 * migration then it implies we are migrating too quickly or the local
	 * node is overloaded. In either case, scan slower
	 */
	/*
	 * IAMROOT. 2023.06.24:
	 * - google-translate
	 * 레코드 암시 오류가 없는 경우 작업이 완전히 유휴 상태이거나 모든 활동이 자동 누마
	 * 밸런싱에 관심이 없는 영역입니다. 이와 관련하여 마이그레이션에 실패한 경우
	 * 마이그레이션이 너무 빠르거나 로컬 노드에 과부하가 걸린 것입니다. 두 경우 모두 더
	 * 느리게 스캔
	 *
	 * IAMROOT, 2023.07.02:
	 * - 다음 3가지 상태에 대해서 다음 scan 주기를 2배로 한다
	 *   1. 유휴상태
	 *   2. numa balancing 이 아닌 영역
	 *   3. migration에 실패한 적이 있다
	 */
	if (local + shared == 0 || p->numa_faults_locality[2]) {
		p->numa_scan_period = min(p->numa_scan_period_max,
			p->numa_scan_period << 1);

		p->mm->numa_next_scan = jiffies +
			msecs_to_jiffies(p->numa_scan_period);

		return;
	}

	/*
	 * Prepare to scale scan period relative to the current period.
	 *	 == NUMA_PERIOD_THRESHOLD scan period stays the same
	 *       <  NUMA_PERIOD_THRESHOLD scan period decreases (scan faster)
	 *	 >= NUMA_PERIOD_THRESHOLD scan period increases (scan slower)
	 */
	period_slot = DIV_ROUND_UP(p->numa_scan_period, NUMA_PERIOD_SLOTS);
	lr_ratio = (local * NUMA_PERIOD_SLOTS) / (local + remote);
	ps_ratio = (private * NUMA_PERIOD_SLOTS) / (private + shared);

	if (ps_ratio >= NUMA_PERIOD_THRESHOLD) {
		/*
		 * Most memory accesses are local. There is no need to
		 * do fast NUMA scanning, since memory is already local.
		 */
		/*
		 * IAMROOT. 2023.07.02:
		 * - google-translate
		 * 대부분의 메모리 액세스는 로컬입니다. 메모리가 이미 로컬이기 때문에
		 * 빠른 NUMA 스캔을 수행할 필요가 없습니다.
		 *
		 * - 주석의 내용은 private access인 경우 local 로 보는 것 같다.
		 */
		int slot = ps_ratio - NUMA_PERIOD_THRESHOLD; /*  */
		if (!slot)
			slot = 1;
		diff = slot * period_slot;
	} else if (lr_ratio >= NUMA_PERIOD_THRESHOLD) {
		/*
		 * Most memory accesses are shared with other tasks.
		 * There is no point in continuing fast NUMA scanning,
		 * since other tasks may just move the memory elsewhere.
		 */
		/*
		 * IAMROOT. 2023.07.02:
		 * - google-translate
		 * 대부분의 메모리 액세스는 다른 작업과 공유됩니다. 다른 작업이 메모리를 다른
		 * 곳으로 이동할 수 있기 때문에 빠른 NUMA 검색을 계속할 필요가 없습니다.
		 */
		int slot = lr_ratio - NUMA_PERIOD_THRESHOLD;
		if (!slot)
			slot = 1;
		diff = slot * period_slot;
	} else {
		/*
		 * Private memory faults exceed (SLOTS-THRESHOLD)/SLOTS,
		 * yet they are not on the local NUMA node. Speed up
		 * NUMA scanning to get the memory moved over.
		 */
		/*
		 * IAMROOT. 2023.07.02:
		 * - google-translate
		 * 개인 메모리 오류는 (SLOTS-THRESHOLD)/SLOTS를 초과하지만 로컬
		 * NUMA 노드에 없습니다. 메모리 이동을 위해 NUMA 스캔 속도를 높입니다.
		 */
		int ratio = max(lr_ratio, ps_ratio);
		diff = -(NUMA_PERIOD_THRESHOLD - ratio) * period_slot;
	}

	p->numa_scan_period = clamp(p->numa_scan_period + diff,
			task_scan_min(p), task_scan_max(p));
	memset(p->numa_faults_locality, 0, sizeof(p->numa_faults_locality));
}

/*
 * Get the fraction of time the task has been running since the last
 * NUMA placement cycle. The scheduler keeps similar statistics, but
 * decays those on a 32ms period, which is orders of magnitude off
 * from the dozens-of-seconds NUMA balancing period. Use the scheduler
 * stats only if the task is so new there are no NUMA statistics yet.
 */
/*
 * IAMROOT. 2023.06.21:
 * - google-translate
 * 마지막 NUMA 배치 주기 이후 작업이 실행된 시간의 일부를 가져옵니다. 스케줄러는
 * 유사한 통계를 유지하지만 수십 초의 NUMA 밸런싱 기간에서 훨씬 벗어난 32ms 기간의
 * 통계를 감소시킵니다. 작업이 너무 새로운 경우에만 스케줄러 통계를 사용하여 아직
 * NUMA 통계가 없습니다.
 *
 * IAMROOT, 2023.07.01:
 * @period now ~ last_task_numa_placement동안의 지난 시간
 * @return now ~ last_sum_exec_runtime까지의 지나간 runtime시간
 *
 * - 이전 실행되었을 때와 @p->se.sum_exec_runtime 의 delta 값을 반환한다.
 *   @p->se.exec_start의 delta는 @period 인자에 전달한다.
 * - 이전 runtime과 시간의 간격을 @return, @period에 전달하고 갱신한다.
 * - 최초로 여길 진입한경우 @return은 se load_sum, @period는 LOAD_AVG_MAX로
 *   return 한다.
 */
static u64 numa_get_avg_runtime(struct task_struct *p, u64 *period)
{
	u64 runtime, delta, now;
	/* Use the start of this time slice to avoid calculations. */
	now = p->se.exec_start;
	runtime = p->se.sum_exec_runtime;

	if (p->last_task_numa_placement) {
		delta = runtime - p->last_sum_exec_runtime;
		*period = now - p->last_task_numa_placement;

		/* Avoid time going backwards, prevent potential divide error: */
		if (unlikely((s64)*period < 0))
			*period = 0;
	} else {
		delta = p->se.avg.load_sum;
		*period = LOAD_AVG_MAX;
	}

	p->last_sum_exec_runtime = runtime;
	p->last_task_numa_placement = now;

	return delta;
}

/*
 * Determine the preferred nid for a task in a numa_group. This needs to
 * be done in a way that produces consistent results with group_weight,
 * otherwise workloads might not converge.
 */
/*
 * IAMROOT. 2023.06.24:
 * - google-translate
 * numa_group의 작업에 대해 선호하는 nid를 결정합니다. 이는 group_weight와 일관된
 * 결과를 생성하는 방식으로 수행되어야 합니다. 그렇지 않으면 워크로드가 수렴되지
 * 않을 수 있습니다.
 * - sched_numa_topology_type에 따른 numa_group preffered node를 알아온다.
 */
static int preferred_group_nid(struct task_struct *p, int nid)
{
	nodemask_t nodes;
	int dist;

	/* Direct connections between all NUMA nodes. */
	if (sched_numa_topology_type == NUMA_DIRECT)
		return nid;

	/*
	 * On a system with glueless mesh NUMA topology, group_weight
	 * scores nodes according to the number of NUMA hinting faults on
	 * both the node itself, and on nearby nodes.
	 */
	/*
	 * IAMROOT. 2023.07.01:
	 * - google-translate
	 * 글루리스 메시 NUMA 토폴로지가 있는 시스템에서 group_weight는 노드 자체와 인근
	 * 노드 모두에서 NUMA 힌트 오류 수에 따라 노드에 점수를 매깁니다.
	 */
	if (sched_numa_topology_type == NUMA_GLUELESS_MESH) {
		unsigned long score, max_score = 0;
		int node, max_node = nid;

		dist = sched_max_numa_distance;

		for_each_online_node(node) {
			score = group_weight(p, node, dist);
			if (score > max_score) {
				max_score = score;
				max_node = node;
			}
		}
		return max_node;
	}

	/*
	 * Finding the preferred nid in a system with NUMA backplane
	 * interconnect topology is more involved. The goal is to locate
	 * tasks from numa_groups near each other in the system, and
	 * untangle workloads from different sides of the system. This requires
	 * searching down the hierarchy of node groups, recursively searching
	 * inside the highest scoring group of nodes. The nodemask tricks
	 * keep the complexity of the search down.
	 */
	/*
	 * IAMROOT. 2023.07.01:
	 * - google-translate
	 * NUMA 백플레인 상호 연결 토폴로지가 있는 시스템에서 선호하는 nid를 찾는 것이 더
	 * 복잡합니다. 목표는 시스템에서 서로 가까운 numa_groups의 작업을 찾고 시스템의
	 * 서로 다른 측면에서 얽힌 워크로드를 해결하는 것입니다. 이를 위해서는 노드 그룹의
	 * 계층 구조를 검색하고 가장 높은 점수를 받은 노드 그룹 내부를 재귀적으로 검색해야
	 * 합니다. 노드 마스크 트릭은 검색의 복잡성을 낮춥니다.
	 */
	nodes = node_online_map;
	for (dist = sched_max_numa_distance; dist > LOCAL_DISTANCE; dist--) {
		unsigned long max_faults = 0;
		nodemask_t max_group = NODE_MASK_NONE;
		int a, b;

		/* Are there nodes at this distance from each other? */
		if (!find_numa_distance(dist))
			continue;

		for_each_node_mask(a, nodes) {
			unsigned long faults = 0;
			nodemask_t this_group;
			nodes_clear(this_group);

			/* Sum group's NUMA faults; includes a==b case. */
			for_each_node_mask(b, nodes) {
				if (node_distance(a, b) < dist) {
					faults += group_faults(p, b);
					node_set(b, this_group);
					node_clear(b, nodes);
				}
			}

			/* Remember the top group. */
			if (faults > max_faults) {
				max_faults = faults;
				max_group = this_group;
				/*
				 * subtle: at the smallest distance there is
				 * just one node left in each "group", the
				 * winner is the preferred nid.
				 */
				nid = a;
			}
		}
		/* Next round, evaluate the nodes within max_group. */
		if (!max_faults)
			break;
		nodes = max_group;
	}
	return nid;
}

/*
 * IAMROOT, 2023.07.04:
 * - task 와 ng의 faults 통계 업데이트(buf 임시 통계를 가져와서 업데이트후 비운다)
 *   mem faults 가 가장 많은 노드를 @p->numa_preferred_nid로 설정한다.
 *
 * - 1. node, cpu, mem별 fault 통계한다.
 *      통계할때, 기존값은 50% decay해서 사용한다.
 *      mem 통계는 fault값 그대로 사용하지만,
 *      cpu 통계는 runtime에서 cpu fault비율을 계산해 그 비율로 사용한다.
 *      (runtime이 적는 cpu는 fault가 별로 중요하지 않기때문이다.)
 *   2. ng에도 계산한 통계를 가산한다.
 *   3. tmp buf를 사용후 0 초기화.
 *   4. ng의 active_nodes, max_fault 갱신
 *   5. max mem fault가 발생한 node를 기준으로 preferred node를 선정하여
 *      @p의 preferred_nid로 설정
 *  6. fault상태에 따른 numa_scan_period update
 */
static void task_numa_placement(struct task_struct *p)
{
	int seq, nid, max_nid = NUMA_NO_NODE;
	unsigned long max_faults = 0;
	unsigned long fault_types[2] = { 0, 0 };
	unsigned long total_faults;
	u64 runtime, period;
	spinlock_t *group_lock = NULL;
	struct numa_group *ng;

	/*
	 * The p->mm->numa_scan_seq field gets updated without
	 * exclusive access. Use READ_ONCE() here to ensure
	 * that the field is read in a single access:
	 */
	/*
	 * IAMROOT. 2023.07.02:
	 * - google-translate
	 * p->mm->numa_scan_seq 필드는 독점 액세스 없이 업데이트됩니다. 여기서
	 * READ_ONCE()를 사용하여 단일 액세스에서 필드를 읽을 수 있도록 합니다.
	 */
	seq = READ_ONCE(p->mm->numa_scan_seq);
	if (p->numa_scan_seq == seq)
		return;
	p->numa_scan_seq = seq;
	p->numa_scan_period_max = task_scan_max(p);

	total_faults = p->numa_faults_locality[0] +
		       p->numa_faults_locality[1];
	runtime = numa_get_avg_runtime(p, &period);

	/* If the task is part of a group prevent parallel updates to group stats */
	/*
	 * IAMROOT. 2023.07.02:
	 * - google-translate
	 * 작업이 그룹의 일부인 경우 그룹 통계에 대한 병렬 업데이트 방지
	 */
	ng = deref_curr_numa_group(p);
	if (ng) {
		group_lock = &ng->lock;
		spin_lock_irq(group_lock);
	}

	/* Find the node with the highest number of faults */
	/*
	 * IAMROOT. 2023.07.02:
	 * - google-translate
	 * faults가 가장 많은 노드 찾기
	 */
	for_each_online_node(nid) {
		/* Keep track of the offsets in numa_faults array */
		int mem_idx, membuf_idx, cpu_idx, cpubuf_idx;
		unsigned long faults = 0, group_faults = 0;
		int priv;

		for (priv = 0; priv < NR_NUMA_HINT_FAULT_TYPES; priv++) {
			long diff, f_diff, f_weight;

			mem_idx = task_faults_idx(NUMA_MEM, nid, priv);
			membuf_idx = task_faults_idx(NUMA_MEMBUF, nid, priv);
			cpu_idx = task_faults_idx(NUMA_CPU, nid, priv);
			cpubuf_idx = task_faults_idx(NUMA_CPUBUF, nid, priv);

			/* Decay existing window, copy faults since last scan */
			/*
			 * IAMROOT. 2023.07.02:
			 * - google-translate
			 * 기존 창 decay, 마지막 스캔 이후 오류 복사
			 *
			 * - mem에대해 계산. 임시버퍼 값으로 fault_types에 통계내고,
			 * 임시버퍼측은 초기화한다.
			 */
			diff = p->numa_faults[membuf_idx] - p->numa_faults[mem_idx] / 2;
			fault_types[priv] += p->numa_faults[membuf_idx];
			p->numa_faults[membuf_idx] = 0;

			/*
			 * Normalize the faults_from, so all tasks in a group
			 * count according to CPU use, instead of by the raw
			 * number of faults. Tasks with little runtime have
			 * little over-all impact on throughput, and thus their
			 * faults are less important.
			 */
			/*
			 * IAMROOT. 2023.07.01:
			 * - google-translate
			 * faults_from을 정규화하여 그룹의 모든 작업이 원시 결함 수
			 * 대신 CPU 사용에 따라 계산되도록 합니다. 런타임이 적은 작업은
			 * 처리량에 전반적으로 거의 영향을 미치지 않으므로 오류가 덜
			 * 중요합니다.
			 *
			 * - period당 runtime의 cpu fault 가중치를 계산한다. 
			 *
			 *  runtime    cpu fault
			 *  ------- *  ------------ = cpu weight
			 *  perioid    total_faults
			 *
			 *  단위시간당 runtime값에셔 cpu fault의 비율만큼을 runtime시간을
			 *  f_weight에 계산해낸다.
			 *
			 *  ex) 직관적인 예를 위해 shift나 + 1값등은 생략.
			 *      runtime = 1000, period = 10,
			 *      cpu fault = 1, total_faults = 3
			 *
			 *      period당 runtime은 100.
			 *      이 runtime에서 cpu fault는 100 * 1 / 4 = 25
			 *      
			 *      즉 1 period당 cpu fault에 대한 runtime시간을 25라고 하고,
			 *      이걸 f_weight로 정의한다.
			 */
			f_weight = div64_u64(runtime << 16, period + 1);
			f_weight = (f_weight * p->numa_faults[cpubuf_idx]) /
				   (total_faults + 1);
/*
 * IAMROOT, 2023.07.10:
 * - 이전에 저장한 f_diff sum값(p->numa_faults[cpu_idx])은 50%만 적용해서
 *   new f_diff를 계산해낸다.
 */
			f_diff = f_weight - p->numa_faults[cpu_idx] / 2;
/*
 * IAMROOT, 2023.07.10:
 * - tmp 값은 다 썻으니 초기화를 한다.
 */
			p->numa_faults[cpubuf_idx] = 0;
/*
 * IAMROOT, 2023.07.10:
 * - 새로 계산해낸 diff값들을 누적한다.
 */
			p->numa_faults[mem_idx] += diff;
			p->numa_faults[cpu_idx] += f_diff;
			faults += p->numa_faults[mem_idx];
			p->total_numa_faults += diff;
			if (ng) {
				/*
				 * safe because we can only change our own group
				 *
				 * mem_idx represents the offset for a given
				 * nid and priv in a specific region because it
				 * is at the beginning of the numa_faults array.
				 */
/*
 * IAMROOT, 2023.07.11:
 * - papago
 *   우리 자신의 그룹만 변경할 수 있기 때문에 안전합니다.
 *
 *   mem_idx는 numa_faults 배열의 시작 부분에 있기 때문에 특정 영역에서
 *   주어진 nid 및 priv에 대한 오프셋을 나타냅니다.
 *
 * - ng가 있다면 ng에도 계산한 통계값을 누적해준다.
 */
				/*
				 * IAMROOT. 2023.07.10:
				 * - ng->faults_cpu[mem_idx]는 mem_idx 이지만
				 *   cpu faults라는 얘기
				 */
				ng->faults[mem_idx] += diff;
				ng->faults_cpu[mem_idx] += f_diff;
				ng->total_faults += diff;
				group_faults += ng->faults[mem_idx];
			}
		}

		/*
		 * IAMROOT, 2023.07.08:
		 * - ng가 없으면 task faults중 가장 큰 값의 nid를 max_nid로 설정
		 *   ng가 있으면 group faults중 가장 큰 값의 nid를 max_nid로 설정
		 */
		if (!ng) {
			if (faults > max_faults) {
				max_faults = faults;
				max_nid = nid;
			}
		} else if (group_faults > max_faults) {
			max_faults = group_faults;
			max_nid = nid;
		}
	}

/*
 * IAMROOT, 2023.07.11:
 * - 위에서 계산한 ng의 fault값들을 토대로 active_nodes, max_faults값 갱신.
 *   max_nid를 기준으로 preferred nid를 구해온다.
 */
	if (ng) {
		numa_group_count_active_nodes(ng);
		spin_unlock_irq(group_lock);
		max_nid = preferred_group_nid(p, max_nid);
	}

/*
 * IAMROOT, 2023.07.12:
 * - 위에서 구해온 preferred nid로 설정한다.
 */
	if (max_faults) {
		/* Set the new preferred node */
		if (max_nid != p->numa_preferred_nid)
			sched_setnuma(p, max_nid);
	}

	update_task_scan_period(p, fault_types[0], fault_types[1]);
}

static inline int get_numa_group(struct numa_group *grp)
{
	return refcount_inc_not_zero(&grp->refcount);
}

static inline void put_numa_group(struct numa_group *grp)
{
	if (refcount_dec_and_test(&grp->refcount))
		kfree_rcu(grp, rcu);
}

/*
 * IAMROOT, 2023.06.24:
 * - @p : this (current)
 *   tsk : @cpupid(page)의 cpu에서 동작중인 curr
 *   cpupid의 pid : @cpupid)를 마지막에 썻던 task
 *
 * IAMROOT, 2023.07.01:
 * - @p: current
 *   @cpupid: fault가 발생한 페이지에 설정되었던 last_cpupid
 *
 * IAMROOT, 2023.07.04:
 * - 같은 공유페이지에 접근하는 task들은 faults를 group 으로 관리한다.
 *
 * IAMROOT, 2023.07.08:
 * - vm을 같이 사용하는 thread들 또는 같은 공유페이지를 사용하는 프로세스들은 하나의
 *   numa group에 조인하여 사용한다.
 */
static void task_numa_group(struct task_struct *p, int cpupid, int flags,
			int *priv)
{
	struct numa_group *grp, *my_grp;
	struct task_struct *tsk;
	bool join = false;
	int cpu = cpupid_to_cpu(cpupid);
	int i;

/*
 * IAMROOT, 2023.06.24:
 * - first access시 grp를 만든다. @p의 값을 초기값으로 해서 생성한다.
 *   ING
 */
	if (unlikely(!deref_curr_numa_group(p))) {
/*
 * IAMROOT, 2023.06.24:
 * - mem shared, mem priv, cpu shard, cpu priv 이렇게해서 4쌍
 */
		unsigned int size = sizeof(struct numa_group) +
				    4*nr_node_ids*sizeof(unsigned long);

		grp = kzalloc(size, GFP_KERNEL | __GFP_NOWARN);
		if (!grp)
			return;

		refcount_set(&grp->refcount, 1);
		grp->active_nodes = 1;
		grp->max_faults_cpu = 0;
		spin_lock_init(&grp->lock);
		grp->gid = p->pid;
		/* Second half of the array tracks nids where faults happen */
/*
 * IAMROOT, 2023.06.24:
 * - faults 뒤에 address를 계산하여 faults_cpu에 넣는것.
 */
		grp->faults_cpu = grp->faults + NR_NUMA_HINT_FAULT_TYPES *
						nr_node_ids;

/*
 * IAMROOT, 2023.06.24:
 * - task것을 초기값으로 가져온다.
 */
		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			grp->faults[i] = p->numa_faults[i];

		grp->total_faults = p->total_numa_faults;

		grp->nr_tasks++;
		rcu_assign_pointer(p->numa_group, grp);
	}

	rcu_read_lock();
/*
 * IAMROOT, 2023.06.24:
 * - cpupid의 cpu에서 동작중인 task를 가져온다.
 */
	tsk = READ_ONCE(cpu_rq(cpu)->curr);

/*
 * IAMROOT, 2023.06.24:
 * - 해당 page를 마지막에 접근했던 task와 page를 사용했던 cpu의 curr와
 *   동일한지 비교한다.
 *
 * ---- page curr와 last access를 비교하는 이유? -----
 * - page curr가 page last access와 같은 경우
 *   1. numa_migrate_prep()에서 should_numa_migrate_memory()를 통해 변경된
 *      경우. last access는 this이며 동시에 page curr다.
 *      cpupid_match_pid()에선 같다고 넘어가겠지만 아래 grp == my_grp에서
 *      no_join으로 넘어갈것이다.
 *
 *   2. 그외에 그냥 같은 경우
 *      last가 아직 동작하고 있어서 다시 접근이 예상된다.
 *      last가 자주동작한다고 판단되어 다시 접근이 예상된다. 
 *
 * - page curr가 page last access와 다른 경우
 *   1. curr가 없는 경우 (cpu sleep 등)
 *      동작중이 아니므로 group 할 필요없음
 *
 *   2. 그외에 그냥 다른 경우
 *      2-1 tsk가 이미 죽은 경우 -> 할필요 없음
 *      2-2 충분한 시간이 흘러 tsk가 해당 page에 잘 접근하지 않은 거라
 *      판단한 경우 -> 굳이 group화 안함.
 * --------------------------------------------------
 */
	/*
	 * IAMROOT, 2023.07.08:
	 * - page에 마지막에 접근했던 task가 원래 cpu에서 curr로 동작하고 있어야 한다.
	 */
	if (!cpupid_match_pid(tsk, cpupid))
		goto no_join;

/*
 * IAMROOT, 2023.06.24:
 * - tsk(cpupid의 curr)의 ng가 없으면 return.
 * - grp: 이전 fault 발생 task 의 numa group
 *   mygrp: 현재 fault 발생 task 의 numa group
 */
	grp = rcu_dereference(tsk->numa_group);
	if (!grp)
		goto no_join;

	my_grp = deref_curr_numa_group(p);
	if (grp == my_grp)
		goto no_join;

	/*
	 * Only join the other group if its bigger; if we're the bigger group,
	 * the other task will join us.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - my_grp -> grp로 옮길려고한다. my_grp이 큰데 괜히 옮길 필욘 없을것이다.
 */
	if (my_grp->nr_tasks > grp->nr_tasks)
		goto no_join;

	/*
	 * Tie-break on the grp address.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - 같다면, grp가 address가 작은 경우에만 조인한다.
 */
	if (my_grp->nr_tasks == grp->nr_tasks && my_grp > grp)
		goto no_join;

	/* Always join threads in the same process. */
/*
 * IAMROOT, 2023.06.24:
 * - 같은 process면 무조건 join
 */
	if (tsk->mm == current->mm)
		join = true;

	/* Simple filter to avoid false positives due to PID collisions */
	/*
	 * IAMROOT. 2023.06.25:
	 * - google-translate
	 * PID 충돌로 인한 오탐을 방지하는 간단한 필터
	 */
	if (flags & TNF_SHARED)
		join = true;

	/* Update priv based on whether false sharing was detected */
	*priv = !join;

/*
 * IAMROOT, 2023.06.24:
 * - grp로 get ref한다.
 */
	if (join && !get_numa_group(grp))
		goto no_join;

	rcu_read_unlock();

	if (!join)
		return;

	BUG_ON(irqs_disabled());
	double_lock_irq(&my_grp->lock, &grp->lock);

/*
 * IAMROOT, 2023.06.24:
 * - @p(current)에 대한 값들을 my_group -> grp으로 옮긴다.
 */
	for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++) {
		my_grp->faults[i] -= p->numa_faults[i];
		grp->faults[i] += p->numa_faults[i];
	}
	my_grp->total_faults -= p->total_numa_faults;
	grp->total_faults += p->total_numa_faults;

	my_grp->nr_tasks--;
	grp->nr_tasks++;

	spin_unlock(&my_grp->lock);
	spin_unlock_irq(&grp->lock);

/*
 * IAMROOT, 2023.06.24:
 * - 최종적으로 curr를 grp로 옮긴다.
 */
	rcu_assign_pointer(p->numa_group, grp);

/*
 * IAMROOT, 2023.06.24:
 * - grp로 다 옮겨졌고, my_group은 ref down한다.
 */
	put_numa_group(my_grp);
	return;

no_join:
	rcu_read_unlock();
	return;
}

/*
 * Get rid of NUMA statistics associated with a task (either current or dead).
 * If @final is set, the task is dead and has reached refcount zero, so we can
 * safely free all relevant data structures. Otherwise, there might be
 * concurrent reads from places like load balancing and procfs, and we should
 * reset the data back to default state without freeing ->numa_faults.
 */
void task_numa_free(struct task_struct *p, bool final)
{
	/* safe: p either is current or is being freed by current */
	struct numa_group *grp = rcu_dereference_raw(p->numa_group);
	unsigned long *numa_faults = p->numa_faults;
	unsigned long flags;
	int i;

	if (!numa_faults)
		return;

	if (grp) {
		spin_lock_irqsave(&grp->lock, flags);
		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			grp->faults[i] -= p->numa_faults[i];
		grp->total_faults -= p->total_numa_faults;

		grp->nr_tasks--;
		spin_unlock_irqrestore(&grp->lock, flags);
		RCU_INIT_POINTER(p->numa_group, NULL);
		put_numa_group(grp);
	}

	if (final) {
		p->numa_faults = NULL;
		kfree(numa_faults);
	} else {
		p->total_numa_faults = 0;
		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			numa_faults[i] = 0;
	}
}

/*
 * Got a PROT_NONE fault for a page on @node.
 */
/*
 * IAMROOT, 2023.06.24:
 * - @mem_node - page->flags 에 설정된 node id(물리 page가 존재하는 node)
 *   cpu_node - faults 가 발생한 task의 node
 * - 1. shared 페이지 일 경우 numa group 설정
 *   2. max fault node 를 preferred node로 설정
 *   3. preferred node에서 best_cpu와 best_task를 찾아 swap
 *   4. faults 통계 업데이트
 */
void task_numa_fault(int last_cpupid, int mem_node, int pages, int flags)
{
	struct task_struct *p = current;
	bool migrated = flags & TNF_MIGRATED;
	int cpu_node = task_node(current);
	int local = !!(flags & TNF_FAULT_LOCAL);
	struct numa_group *ng;
	int priv;

	if (!static_branch_likely(&sched_numa_balancing))
		return;

	/* for example, ksmd faulting in a user's mm */
	if (!p->mm)
		return;

	/* Allocate buffer to track faults on a per-node basis */
/*
 * IAMROOT, 2023.06.24:
 * - 최초 memory 할당.
 */
	if (unlikely(!p->numa_faults)) {
		int size = sizeof(*p->numa_faults) *
			   NR_NUMA_HINT_FAULT_BUCKETS * nr_node_ids;

		p->numa_faults = kzalloc(size, GFP_KERNEL|__GFP_NOWARN);
		if (!p->numa_faults)
			return;

		p->total_numa_faults = 0;
		memset(p->numa_faults_locality, 0, sizeof(p->numa_faults_locality));
	}

	/*
	 * First accesses are treated as private, otherwise consider accesses
	 * to be private if the accessing pid has not changed
	 */
/*
 * IAMROOT, 2023.06.24:
 * - first access는 priv 으로 처리.
 * - 한개의 page에 접근하는 순서에 따라 priv와 share가 구분된다.
 *   1. [process1]   -> priv
 *   2. [process1]   -> priv
 *   3. [process2]   -> share
 *   4.    [thread1] -> share
 *   5.    [thread2] -> share
 *   6.    [thread2] -> priv
 * - priv, share 구분 numa_faults 통계는 scan 주기 설정에 사용
 */
	if (unlikely(last_cpupid == (-1 & LAST_CPUPID_MASK))) {
		priv = 1;
	} else {
		priv = cpupid_match_pid(p, last_cpupid);
/*
 * IAMROOT, 2023.06.24:
 * - shared고, no group요청이 없엇으면 group화 한다.
 */
		if (!priv && !(flags & TNF_NO_GROUP))
			task_numa_group(p, last_cpupid, flags, &priv);
	}

	/*
	 * If a workload spans multiple NUMA nodes, a shared fault that
	 * occurs wholly within the set of nodes that the workload is
	 * actively using should be counted as local. This allows the
	 * scan rate to slow down when a workload has settled down.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   워크로드가 여러 NUMA 노드에 걸쳐 있는 경우 워크로드가 적극적으로
 *   사용하는 노드 세트 내에서 전적으로 발생하는 공유 fault은 로컬로
 *   계산되어야 합니다. 이렇게 하면 워크로드가 안정되었을 때 스캔
 *   속도가 느려질 수 있습니다.
 *
 * - shared fault가 local로 처리가 되야되는 경우에 대한 처리.
 *   1. workload가 여러 numa node에 걸쳐있다.
 *   2. active node가 많다.
 *   3. 그중에서도 cpu node, mem node 둘다 활성화되있다.
 */
	ng = deref_curr_numa_group(p);
	if (!priv && !local && ng && ng->active_nodes > 1 &&
				numa_is_active_node(cpu_node, ng) &&
				numa_is_active_node(mem_node, ng))
		local = 1;

	/*
	 * Retry to migrate task to preferred node periodically, in case it
	 * previously failed, or the scheduler moved us.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   이전에 실패했거나 스케줄러가 이동한 경우 주기적으로 작업을 기본
 *   노드로 마이그레이션을 다시 시도하십시오.
 * - numa_migrate_retry시간이 지낫으면 task_numa_placement()를 수행하고,
 *   다음 numa_migrate_retry을 갱신한다.
 */
	if (time_after(jiffies, p->numa_migrate_retry)) {
		task_numa_placement(p);
		numa_migrate_preferred(p);
	}

	if (migrated)
		p->numa_pages_migrated += pages;
	if (flags & TNF_MIGRATE_FAIL)
		p->numa_faults_locality[2] += pages;

/*
 * IAMROOT, 2023.07.10:
 * - fault가 발생할때 data를 cpu로 load하는쪽은 cpu fault, data가 있던
 *   memory는 memory fault로 기록될것이다.
 */
	/*
	 * IAMROOT, 2023.07.10:
	 * - faults 발생 page의 노드(mem_node)에 대한 faults 값 누적
	 */
	p->numa_faults[task_faults_idx(NUMA_MEMBUF, mem_node, priv)] += pages;
	/*
	 * IAMROOT, 2023.07.10:
	 * - faults 발생 page에 접근한 task의 cpu의 노드(cpu_node)에 대한 faults 값 누적
	 */
	p->numa_faults[task_faults_idx(NUMA_CPUBUF, cpu_node, priv)] += pages;
	p->numa_faults_locality[local] += pages;
}

/*
 * IAMROOT, 2023.06.17:
 * - numa_scan_seq를 up하고 시작위치를 초기화한다..
 */
static void reset_ptenuma_scan(struct task_struct *p)
{
	/*
	 * We only did a read acquisition of the mmap sem, so
	 * p->mm->numa_scan_seq is written to without exclusive access
	 * and the update is not guaranteed to be atomic. That's not
	 * much of an issue though, since this is just used for
	 * statistical sampling. Use READ_ONCE/WRITE_ONCE, which are not
	 * expensive, to avoid any form of compiler optimizations:
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   우리는 mmap sem의 읽기 획득만 수행했으므로 p->mm->numa_scan_seq는 
 *   독점 액세스 없이 기록되며 업데이트가 원자적임을 보장하지 않습니다. 
 *   통계적 샘플링에만 사용되기 때문에 큰 문제는 아닙니다. 비싸지 않은 
 *   READ_ONCE/WRITE_ONCE를 사용하여 모든 형태의 컴파일러 최적화를 
 *   피하십시오.
 */
	WRITE_ONCE(p->mm->numa_scan_seq, READ_ONCE(p->mm->numa_scan_seq) + 1);
	p->mm->numa_scan_offset = 0;
}

/*
 * The expensive part of numa migration is done from task_work context.
 * Triggered from task_tick_numa().
 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   numa 마이그레이션의 비용이 많이 드는 부분은 task_work 컨텍스트에서 수행됩니다.
 *   task_tick_numa()에서 트리거됩니다.
 *
 * - 1. next scan 시간 갱신
 *   2. 이미 scan 중이라고 판단되면 return
 *   3. scan 갯수만큼의 pages를 vma를 순회하며 numa fault 시킨다.
 *   4. scan 완료 next scan offset 갱신
 * - 이 함수에서 numa fault의해서 mapping해제된 page는 fault 발생시
 *   handle_pte_fault()에서 do_numa_page()를 통해 수행할것이다.
 */
static void task_numa_work(struct callback_head *work)
{
	unsigned long migrate, next_scan, now = jiffies;
	struct task_struct *p = current;
	struct mm_struct *mm = p->mm;
	u64 runtime = p->se.sum_exec_runtime;
	struct vm_area_struct *vma;
	unsigned long start, end;
	unsigned long nr_pte_updates = 0;
	long pages, virtpages;

	SCHED_WARN_ON(p != container_of(work, struct task_struct, numa_work));

	work->next = work;
	/*
	 * Who cares about NUMA placement when they're dying.
	 *
	 * NOTE: make sure not to dereference p->mm before this check,
	 * exit_task_work() happens _after_ exit_mm() so we could be called
	 * without p->mm even though we still had it when we enqueued this
	 * work.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   죽을 때 누가 NUMA 배치에 신경을 쓰겠어요.
 *
 *   참고: 이 검사 전에 exit_task_work()가 _exit_mm() 이후에 발생하므로 
 *   이 작업을 대기열에 넣을 때에도 p->mm가 없어도 호출할 수 있습니다.
 */
	if (p->flags & PF_EXITING)
		return;

/*
 * IAMROOT, 2023.06.17:
 * - 최초수행이면 지금시각기준으로 갱신
 */
	if (!mm->numa_next_scan) {
		mm->numa_next_scan = now +
			msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
	}

	/*
	 * Enforce maximal scan/migration frequency..
	 */
	migrate = mm->numa_next_scan;
	if (time_before(now, migrate))
		return;

	if (p->numa_scan_period == 0) {
		p->numa_scan_period_max = task_scan_max(p);
		p->numa_scan_period = task_scan_start(p);
	}

	next_scan = now + msecs_to_jiffies(p->numa_scan_period);
/*
 * IAMROOT, 2023.06.17:
 * - atomic 검사. 경합발생시. return.
 */
	if (cmpxchg(&mm->numa_next_scan, migrate, next_scan) != migrate)
		return;

	/*
	 * Delay this task enough that another task of this mm will likely win
	 * the next time around.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   이 mm의 다른 작업이 다음에 이길 가능성이 높도록 이 작업을 충분히 지연시킵니다.
. 
 * - 다른 task가 혹시 검사하러 들어올줄 모르니 갭을 더 준다.
 */
	p->node_stamp += 2 * TICK_NSEC;

/*
 * IAMROOT, 2023.06.17:
 * - 그전에 마지막으로 했던 offset.
 */
	start = mm->numa_scan_offset;
	/*
	 * IAMROOT, 2023.06.24:
	 * - 2^8(256) * 2^8 * 2^12(page size) = 2^28 = 256MB
	 */
	pages = sysctl_numa_balancing_scan_size;
	pages <<= 20 - PAGE_SHIFT; /* MB in pages */
/*
 * IAMROOT, 2023.06.17:
 * - virt page는 scan용량의 8배
 */
	/*
	 * IAMROOT, 2023.06.24:
	 * - 2^28 * 2^3 = 2^31 = 2G
	 */
	virtpages = pages * 8;	   /* Scan up to this much virtual space */
	if (!pages)
		return;


	if (!mmap_read_trylock(mm))
		return;
/*
 * IAMROOT, 2023.06.17:
 * - 이전에 기록했던 start를 못찾던가 최초인상황. 처음부터 다시한다.
 */
	vma = find_vma(mm, start);
	if (!vma) {
		reset_ptenuma_scan(p);
		start = 0;
		vma = mm->mmap;
	}
/*
 * IAMROOT, 2023.06.24:
 * - vma를 순회하며 pages개수만큼 numa fault 시킨다.
 */
	for (; vma; vma = vma->vm_next) {
/*
 * IAMROOT, 2023.06.17:
 * - 다음과 같은 vma는 scan을 지원하지 않는다.
 *   1. migrate 불가능
 *   2. MOF(migrate on fault) 미지원
 *   3. hugetlb 불가능
 *   4. VM_MIXEDMAP은 지원 안한다.
 */
		if (!vma_migratable(vma) || !vma_policy_mof(vma) ||
			is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
			continue;
		}

		/*
		 * Shared library pages mapped by multiple processes are not
		 * migrated as it is expected they are cache replicated. Avoid
		 * hinting faults in read-only file-backed mappings or the vdso
		 * as migrating the pages will be of marginal benefit.
		 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   여러 프로세스에 의해 매핑된 공유 라이브러리 페이지는 캐시 복제될 것으로 
 *   예상되므로 마이그레이션되지 않습니다. 읽기 전용 파일 지원 매핑 또는 
 *   vdso에서 힌트 fault을 피하십시오. 페이지를 마이그레이션하는 것이 약간의 
 *   이점이 있을 것입니다.
 *
 * - library(readonly file)은 안한다. 주석내용확인.
 */
		if (!vma->vm_mm ||
		    (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)))
			continue;

		/*
		 * Skip inaccessible VMAs to avoid any confusion between
		 * PROT_NONE and NUMA hinting ptes
		 */
		if (!vma_is_accessible(vma))
			continue;

/*
 * IAMROOT, 2023.06.24:
 * - HPAGE_SIZE단위로 ALIGN을 한 범위를 순회하며 numa fault시킨다.
 */
		do {
			start = max(start, vma->vm_start);
			end = ALIGN(start + (pages << PAGE_SHIFT), HPAGE_SIZE);
			end = min(end, vma->vm_end);
/*
 * IAMROOT, 2023.06.24:
 * - start ~ end까지 PAGE_NONE으로 numa fault식으로 update한다.
 */
			nr_pte_updates = change_prot_numa(vma, start, end);

			/*
			 * Try to scan sysctl_numa_balancing_size worth of
			 * hpages that have at least one present PTE that
			 * is not already pte-numa. If the VMA contains
			 * areas that are unused or already full of prot_numa
			 * PTEs, scan up to virtpages, to skip through those
			 * areas faster.
			 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   아직 pte-numa가 아닌 현재 PTE가 하나 이상 있는 hpage의 
 *   sysctl_numa_balancing_size에 해당하는 스캔을 시도합니다. 
 *   VMA에 사용되지 않았거나 이미 prot_numa PTE로 가득 찬 영역이 
 *   포함된 경우 해당 영역을 더 빨리 건너뛰려면 virtpages까지 
 *   스캔하십시오.
 * - 남은것을 pages, virtpages에 기록한다.
 */
			if (nr_pte_updates)
				pages -= (end - start) >> PAGE_SHIFT;
			virtpages -= (end - start) >> PAGE_SHIFT;
/*
 * IAMROOT, 2023.06.24:
 * - 범위갱신
 */
			start = end;
			if (pages <= 0 || virtpages <= 0)
				goto out;

			cond_resched();
		} while (end != vma->vm_end);
	}

out:
	/*
	 * It is possible to reach the end of the VMA list but the last few
	 * VMAs are not guaranteed to the vma_migratable. If they are not, we
	 * would find the !migratable VMA on the next scan but not reset the
	 * scanner to the start so check it now.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   VMA 목록의 끝에 도달하는 것이 가능하지만 마지막 몇 개의 VMA는 
 *   vma_migratable에 대해 보장되지 않습니다. 그렇지 않은 경우 다음 스캔에서 
 *   !migrateable VMA를 찾을 수 있지만 스캐너를 시작으로 재설정하지 않으므로 
 *   지금 확인하십시오.
 */
	if (vma)
		mm->numa_scan_offset = start;
	else
		reset_ptenuma_scan(p);
	mmap_read_unlock(mm);

	/*
	 * Make sure tasks use at least 32x as much time to run other code
	 * than they used here, to limit NUMA PTE scanning overhead to 3% max.
	 * Usually update_task_scan_period slows down scanning enough; on an
	 * overloaded system we need to limit overhead on a per task basis.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   NUMA PTE 스캐닝 오버헤드를 최대 3%로 제한하기 위해 태스크가 여기에서 
 *   사용된 것보다 다른 코드를 실행하는 데 최소 32배의 시간을 사용하는지 
 *   확인하십시오.
 *   일반적으로 update_task_scan_period는 스캔 속도를 충분히 늦춥니다. 
 *   오버로드된 시스템에서는 작업별로 오버헤드를 제한해야 합니다.
 * - ex) numa fault하는 데 1초 걸렸으면, 최소 32초후에 하겠다는 의미.
 */
	if (unlikely(p->se.sum_exec_runtime != runtime)) {
		u64 diff = p->se.sum_exec_runtime - runtime;
		p->node_stamp += 32 * diff;
	}
}

/*
 * IAMROOT, 2023.06.17:
 * - tick
 *   task_tick_fair()의 task_tick_numa()
 *
 * - callstack
 *   copy_process() -> sched_fork() ->  __sched_fork() -> init_numa_balancing()
 *
 * - numa scan관련 파라미터를 초기화한다.
 */
void init_numa_balancing(unsigned long clone_flags, struct task_struct *p)
{
	int mm_users = 0;
	struct mm_struct *mm = p->mm;

/*
 * IAMROOT, 2023.06.17:
 * - mm == NULL이면 kernel
 */
	if (mm) {
		mm_users = atomic_read(&mm->mm_users);
/*
 * IAMROOT, 2023.06.17:
 * - thread가 없다는 의미. 현재시각 + 1sec후로 
 */
		if (mm_users == 1) {
			mm->numa_next_scan = jiffies + msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
			mm->numa_scan_seq = 0;
		}
	}
	p->node_stamp			= 0;
	p->numa_scan_seq		= mm ? mm->numa_scan_seq : 0;
	p->numa_scan_period		= sysctl_numa_balancing_scan_delay;
	/* Protect against double add, see task_tick_numa and task_numa_work */
	p->numa_work.next		= &p->numa_work;
	p->numa_faults			= NULL;
	RCU_INIT_POINTER(p->numa_group, NULL);
	p->last_task_numa_placement	= 0;
	p->last_sum_exec_runtime	= 0;

	init_task_work(&p->numa_work, task_numa_work);

	/* New address space, reset the preferred nid */
/*
 * IAMROOT, 2023.06.17:
 * - thread가 아닌 경우. 즉 일반 process
 */
	if (!(clone_flags & CLONE_VM)) {
		p->numa_preferred_nid = NUMA_NO_NODE;
		return;
	}

	/*
	 * New thread, keep existing numa_preferred_nid which should be copied
	 * already by arch_dup_task_struct but stagger when scans start.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - new thread가 있는 경우.
 *
 * - task = 384MB - > scan = 1000 / 2 = 500ms. max(500, 100)
 *   min = 500ms, max = 30000ms 
 *   mm_user가 2이라면 2초. mm_user가 31이라면 30초. max가 30이므로.
 *   
 * - task = 5120MB -> scan = 1000 / 20 = 50ms. -> max(50, 100)
 *   min = 100ms, max = 3000ms
 *   mm_user가 2이라면 2초. mm_user가 4라면 3초. max가 3초이므로.
 *
 * - thread이면 node scan시각을 max대비 계산된 시간 + 2틱여유를 둬서 정한다.
 */
	/*
	 * IAMROOT, 2023.06.23:
	 * - XXX task_scan_max의 반환값은 ms 단위인데 ns 단위와 비교하고 있다.
	 *   실제로 delay는 nsec 단위로 보인다
	 */
	if (mm) {
		unsigned int delay;

		delay = min_t(unsigned int, task_scan_max(current),
			current->numa_scan_period * mm_users * NSEC_PER_MSEC);
		delay += 2 * TICK_NSEC;
		p->node_stamp = delay;
	}
}

/*
 * Drive the periodic memory faults..
 */
/*
 * IAMROOT, 2023.06.17:
 * - callstack
 *   task_tick_fair() -> task_tick_numa()
 *
 * - numa scan시간이 됬으면 task work(task_numa_work)를 add한다.
 *   즉 work수행요청.
 * - numa balance time은 최대 task의 실행시간의 3%로 제한된다.
 */
static void task_tick_numa(struct rq *rq, struct task_struct *curr)
{
	struct callback_head *work = &curr->numa_work;
	u64 period, now;

	/*
	 * We don't care about NUMA placement if we don't have memory.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - 나가고있거나, kernel thread거나, 이미 work로 정해져있으면 안한다.
 */
	if ((curr->flags & (PF_EXITING | PF_KTHREAD)) || work->next != work)
		return;

	/*
	 * Using runtime rather than walltime has the dual advantage that
	 * we (mostly) drive the selection from busy threads and that the
	 * task needs to have done some actual work before we bother with
	 * NUMA placement.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   walltime(realtime) 대신 런타임을 사용하면 (대부분) 바쁜 스레드에서 선택을 
 *   유도하고 NUMA 배치를 방해하기 전에 작업이 실제 작업을 완료해야 
 *   한다는 이중 이점이 있습니다.
 */
	now = curr->se.sum_exec_runtime;
	period = (u64)curr->numa_scan_period * NSEC_PER_MSEC;

/*
 * IAMROOT, 2023.06.17:
 * - thread기준의 node scan 확인 -> mm 기준의 node scan확인. 다른 task에 
 *   의해 같은 mm이 중복으로 scan을 안하기 위함이다.
 */
	if (now > curr->node_stamp + period) {
		if (!curr->node_stamp)
			curr->numa_scan_period = task_scan_start(curr);
		curr->node_stamp += period;

/*
 * IAMROOT, 2023.06.17:
 * - task_numa_work()에서 갱신된다. sysctl_numa_balancing_scan_delay간격(기본 1초)
 */
		if (!time_before(jiffies, curr->mm->numa_next_scan))
			task_work_add(curr, work, TWA_RESUME);
	}
}

static void update_scan_period(struct task_struct *p, int new_cpu)
{
	int src_nid = cpu_to_node(task_cpu(p));
	int dst_nid = cpu_to_node(new_cpu);

	if (!static_branch_likely(&sched_numa_balancing))
		return;

	if (!p->mm || !p->numa_faults || (p->flags & PF_EXITING))
		return;

	if (src_nid == dst_nid)
		return;

	/*
	 * Allow resets if faults have been trapped before one scan
	 * has completed. This is most likely due to a new task that
	 * is pulled cross-node due to wakeups or load balancing.
	 */
	if (p->numa_scan_seq) {
		/*
		 * Avoid scan adjustments if moving to the preferred
		 * node or if the task was not previously running on
		 * the preferred node.
		 */
		if (dst_nid == p->numa_preferred_nid ||
		    (p->numa_preferred_nid != NUMA_NO_NODE &&
			src_nid != p->numa_preferred_nid))
			return;
	}

	p->numa_scan_period = task_scan_start(p);
}

#else
static void task_tick_numa(struct rq *rq, struct task_struct *curr)
{
}

static inline void account_numa_enqueue(struct rq *rq, struct task_struct *p)
{
}

static inline void account_numa_dequeue(struct rq *rq, struct task_struct *p)
{
}

static inline void update_scan_period(struct task_struct *p, int new_cpu)
{
}

#endif /* CONFIG_NUMA_BALANCING */

static void
account_entity_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	update_load_add(&cfs_rq->load, se->load.weight);
#ifdef CONFIG_SMP
	if (entity_is_task(se)) {
		struct rq *rq = rq_of(cfs_rq);

		account_numa_enqueue(rq, task_of(se));
		list_add(&se->group_node, &rq->cfs_tasks);
	}
#endif
	cfs_rq->nr_running++;
}

static void
account_entity_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	update_load_sub(&cfs_rq->load, se->load.weight);
#ifdef CONFIG_SMP
	if (entity_is_task(se)) {
		account_numa_dequeue(rq_of(cfs_rq), task_of(se));
		list_del_init(&se->group_node);
	}
#endif
	cfs_rq->nr_running--;
}

/*
 * Signed add and clamp on underflow.
 *
 * Explicitly do a load-store to ensure the intermediate value never hits
 * memory. This allows lockless observations without ever seeing the negative
 * values.
 */
#define add_positive(_ptr, _val) do {                           \
	typeof(_ptr) ptr = (_ptr);                              \
	typeof(_val) val = (_val);                              \
	typeof(*ptr) res, var = READ_ONCE(*ptr);                \
								\
	res = var + val;                                        \
								\
	if (val < 0 && res > var)                               \
		res = 0;                                        \
								\
	WRITE_ONCE(*ptr, res);                                  \
} while (0)

/*
 * Unsigned subtract and clamp on underflow.
 *
 * Explicitly do a load-store to ensure the intermediate value never hits
 * memory. This allows lockless observations without ever seeing the negative
 * values.
 */
/*
 * IAMROOT, 2022.12.31:
 * ---
 * *ptr -= _val;
 * if (*ptr < 0)
 *   *ptr = 0;
 * ---
 * - 뺄섬 결과값이 음수면 0으로 한다.
 */
#define sub_positive(_ptr, _val) do {				\
	typeof(_ptr) ptr = (_ptr);				\
	typeof(*ptr) val = (_val);				\
	typeof(*ptr) res, var = READ_ONCE(*ptr);		\
	res = var - val;					\
	if (res > var)						\
		res = 0;					\
	WRITE_ONCE(*ptr, res);					\
} while (0)

/*
 * Remove and clamp on negative, from a local variable.
 *
 * A variant of sub_positive(), which does not use explicit load-store
 * and is thus optimized for local variable updates.
 */
/*
 * IAMROOT. 2023.05.11:
 * - google-translate
 * 지역 변수에서 음수를 제거하고 고정합니다. 명시적 로드 저장소를 사용하지 않으므로
 * 로컬 변수 업데이트에 최적화된 sub_positive()의 변형입니다.
 */
/*
 * IAMROOT, 2023.05.06:
 * - *_ptr = *_ptr - _val
 *   0보다 작아지면 *_ptr = 0
 */
#define lsub_positive(_ptr, _val) do {				\
	typeof(_ptr) ptr = (_ptr);				\
	*ptr -= min_t(typeof(*ptr), *ptr, _val);		\
} while (0)

#ifdef CONFIG_SMP

/*
 * IAMROOT, 2023.01.07:
 * - @se는 @cfs_rq에 속해있다. @cfs_rq에 @se의 값들을 적산한다.
 *   cfs_rq의 load_sum엔, 속해있는 entity의 weight가 적용되서 들어간다.
 */
static inline void
enqueue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	cfs_rq->avg.load_avg += se->avg.load_avg;
	cfs_rq->avg.load_sum += se_weight(se) * se->avg.load_sum;
}

/*
 * IAMROOT, 2023.01.07:
 * - @cfs_rq에서 @se에 대한 값을 뺀다.
 * - @cfs_rq에서 @se에 대한 load_avg를 빼고, cfs_rq의 sum을 다시 계산한다.
 */
static inline void
dequeue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	u32 divider = get_pelt_divider(&se->avg);
	sub_positive(&cfs_rq->avg.load_avg, se->avg.load_avg);
	cfs_rq->avg.load_sum = cfs_rq->avg.load_avg * divider;
}
#else
static inline void
enqueue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) { }
static inline void
dequeue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) { }
#endif

/*
 * IAMROOT, 2023.01.07:
 * - @se의 load weight에 tg에 기여한 load비율에 따른 shares값을 set한다.
 * - ex) cpu가 2개있는 상황. cpu0 = 50% 이였다가 cpu1 = 30%로 떨어지는 상황.
 *       weight는 330으로 가정.
 *
 *   reweight_entity 진입전 값들정리(cpu0 = 50%)
 *   cfs_rq load weight = 1024
 *   cfs_rq load avg    = 1024
 *   cfs_rq load sum    = 47742
 *
 *   se load weight = 512
 *   se load avg    = 512
 *   se load sum    = 47742 / 2
 *
 *   cpu0 se weight = 512
 *
 *   reweight_entity 수행
 *   1) update_load_sub()
 *      cfs_rq load weight = 1024 - 512 = 512
 *
 *   2) dequeue_load_avg()
 *      cfs_rq load avg    = 1024 - 512 = 512
 *      cfs_rq load sum    = 47742 / 2
 *
 *   3) update_load_set
 *      se weight = 330
 *
 *   4) se load_avg 재계산
 *      se load avg = (330 * (47742 / 2)) / 47742
 *                  = 165
 *
 *   5) enqueue_load_avg
 *      cfs_rq load avg = 512 + 165 = 677
 *      cfs_rq load sum = (47742 * 677) / 1024
 *
 *   6) update_load_add
 *      cfs_rq load weight = 512 + 330 = 842
 *
 *   7) 결과
 *   cfs_rq load weight 1024 -> 842
 *   cfs_rq load avg    1024 -> 677
 *   cfs_rq load sum    100% -> 약 70%
 *   se load weight     512  -> 330
 *   se load avg        512  -> 165
 *   se load sum        50%  -> 50%(계산안함)
 *
 * - 1) @cfs_rq의 기존 @se값을 뺀다.
 *   2) @se의 weight를 @weight(tg shares비율)로 갱신한다.
 *   3) 변경된 @se를 @cfs_rq에 적용한다.
 */
static void reweight_entity(struct cfs_rq *cfs_rq, struct sched_entity *se,
			    unsigned long weight)
{
	if (se->on_rq) {
/*
 * prifri, 2023.01.07:
 * - @cfs_rq에서 @se에 대한 weight와 load값들을 뺀다.
 */
		/* commit outstanding execution time */
		if (cfs_rq->curr == se)
			update_curr(cfs_rq);
		update_load_sub(&cfs_rq->load, se->load.weight);
	}
	dequeue_load_avg(cfs_rq, se);

/*
 * IAMROOT, 2023.01.07:
 * - @weight(shares)을 @se->load에 set한다.
 */
	update_load_set(&se->load, weight);

#ifdef CONFIG_SMP
	do {
		u32 divider = get_pelt_divider(&se->avg);

		se->avg.load_avg = div_u64(se_weight(se) * se->avg.load_sum, divider);
	} while (0);
#endif


/*
 * IAMROOT, 2023.01.07:
 * - @cfs_rq에 다시계산된 @se를 적산한다.
 */
	enqueue_load_avg(cfs_rq, se);
	if (se->on_rq)
		update_load_add(&cfs_rq->load, se->load.weight);

}

/*
 * IAMROOT, 2022.12.29:
 * - TODO
 */
void reweight_task(struct task_struct *p, int prio)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	struct load_weight *load = &se->load;
	unsigned long weight = scale_load(sched_prio_to_weight[prio]);

	reweight_entity(cfs_rq, se, weight);
	load->inv_weight = sched_prio_to_wmult[prio];
}

#ifdef CONFIG_FAIR_GROUP_SCHED
#ifdef CONFIG_SMP
/*
 * All this does is approximate the hierarchical proportion which includes that
 * global sum we all love to hate.
 *
 * That is, the weight of a group entity, is the proportional share of the
 * group weight based on the group runqueue weights. That is:
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------               (1)
 *                       \Sum grq->load.weight
 *
 * Now, because computing that sum is prohibitively expensive to compute (been
 * there, done that) we approximate it with this average stuff. The average
 * moves slower and therefore the approximation is cheaper and more stable.
 *
 * So instead of the above, we substitute:
 *
 *   grq->load.weight -> grq->avg.load_avg                         (2)
 *
 * which yields the following:
 *
 *                     tg->weight * grq->avg.load_avg
 *   ge->load.weight = ------------------------------              (3)
 *                             tg->load_avg
 *
 * Where: tg->load_avg ~= \Sum grq->avg.load_avg
 *
 * That is shares_avg, and it is right (given the approximation (2)).
 *
 * The problem with it is that because the average is slow -- it was designed
 * to be exactly that of course -- this leads to transients in boundary
 * conditions. In specific, the case where the group was idle and we start the
 * one task. It takes time for our CPU's grq->avg.load_avg to build up,
 * yielding bad latency etc..
 *
 * Now, in that special case (1) reduces to:
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = ----------------------------- = tg->weight   (4)
 *                         grp->load.weight
 *
 * That is, the sum collapses because all other CPUs are idle; the UP scenario.
 *
 * So what we do is modify our approximation (3) to approach (4) in the (near)
 * UP case, like:
 *
 *   ge->load.weight =
 *
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------         (5)
 *     tg->load_avg - grq->avg.load_avg + grq->load.weight
 *
 * But because grq->load.weight can drop to 0, resulting in a divide by zero,
 * we need to use grq->avg.load_avg as its lower bound, which then gives:
 *
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------		   (6)
 *                             tg_load_avg'
 *
 * Where:
 *
 *   tg_load_avg' = tg->load_avg - grq->avg.load_avg +
 *                  max(grq->load.weight, grq->avg.load_avg)
 *
 * And that is shares_weight and is icky. In the (near) UP case it approaches
 * (4) while in the normal case it approaches (3). It consistently
 * overestimates the ge->load.weight and therefore:
 *
 *   \Sum ge->load.weight >= tg->weight
 *
 * hence icky!
 */
/*
 * IAMROOT, 2023.01.07:
 * - papago
 *   이 모든 것은 우리 모두가 싫어하는 전체 합계를 포함하는 계층적 비율에 가깝습니다.
 *
 *   즉, 그룹 엔터티의 가중치는 그룹 실행 대기열 가중치를 기반으로 한 그룹 가중치의 비례적
 *   공유입니다. 그건:
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------               (1)
 *                       \Sum grq->load.weight
 *
 *  이제 그 합계를 계산하는 것은 엄청나게 비용이 많이 들기 때문에(거기에 있었고,
 *  완료했습니다) 우리는 이 평균값으로 근사합니다. 평균은 느리게 이동하므로 근사값이 더
 *  저렴하고 안정적입니다.
 *
 *  따라서 위의 대신 다음을 대체합니다.
 *
 *  grq->load.weight -> grq->avg.load_avg (2)
 *
 *  결과는 다음과 같습니다.
 *
 *
 *                     tg->weight * grq->avg.load_avg
 *   ge->load.weight = ------------------------------              (3)
 *                             tg->load_avg
 *
 *  (\Sum grep->load.weight가 의미론적으로 tg->load_avg와 일치한다. )
 *
 * Where: tg->load_avg ~= \Sum grq->avg.load_avg
 *
 * 그것은 share_avg이며, 맞습니다(주어진 근사치(2)).
 *
 * 문제는 평균이 느리기 때문에(당연히 정확히 그렇게 설계되었으므로) 경계 조건에서 과도
 * 현상이 발생한다는 것입니다. 특히 그룹이 유휴 상태이고 하나의 작업을 시작하는 경우입니다.
 * CPU의 grq->avg.load_avg가 쌓이는 데 시간이 걸리고 대기 시간이 좋지 않습니다.
 *
 * 이제 특별한 경우 (1)은 다음과 같이 줄어듭니다.
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = ----------------------------- = tg->weight   (4)
 *                         grp->load.weight
 *
 * 즉, 다른 모든 CPU가 유휴 상태이므로 합계가 축소됩니다. UP 시나리오.
 *
 * 따라서 우리가 하는 일은 다음과 같이 (가까운) UP 사례에서 (4)에 접근하도록 근사치 (3)을
 * 수정하는 것입니다.
 *
 *   ge->load.weight =
 *
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------         (5)
 *     tg->load_avg - grq->avg.load_avg + grq->load.weight
 *
 *  (
 *     ex) rq 2개.
 *         rq 1개가 idle. 1개가 tg의 load를 100%했을때. 즉 UP와 다름 없는 상황.
 *         tg->load_avg == grp->avg.load_avg
 *
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------
 *     tg->load_avg - tg->load_avg + grq->load.weight
 *
 *     =
 *              tg->weight * grq->load.weight
 *     --------------------------------------------------- = tg->weight
 *                     grq->load.weight
 *
 *     ex) rq 2개.
 *         rq 1개가 50%. 1개가 50%했을때.
 *
 *         tg->load_avg / 2 == grp->avg.load_avg
 *
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------
 *     tg->load_avg - tg->load_avg / 2 + grq->load.weight
 *
 *     =
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------
 *           tg->load_avg / 2 + grq->load.weigh
 *
 *     ex) rq 2개.
 *         rq 1개가 70%. 1개가 30%했을때. 70%의 기준의 계산
 *
 *         tg->load_avg * 7 / 10 == grp->avg.load_avg
 *
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------
 *     tg->load_avg - tg->load_avg * 7 / 10 + grq->load.weight
 *
 *     =
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------
 *           tg->load_avg * 3 / 10 + grq->load.weigh
 *
 *
 *    30%%의 기준의 결과값.
 *
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------
 *           tg->load_avg * 7 / 10 + grq->load.weigh
 *  )
 *
 * 그러나 grq->load.weight가 0으로 떨어질 수 있으므로 결과적으로 0으로 나누기 때문에
 * grq->avg.load_avg를 하한으로 사용해야 합니다. 그런 다음 다음을 제공합니다.
 *
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------		   (6)
 *                             tg_load_avg'
 *
 * Where:
 *
 *   tg_load_avg' = tg->load_avg - grq->avg.load_avg +
 *                  max(grq->load.weight, grq->avg.load_avg)
 *
 * 그리고 그것은 share_weight이고 엉뚱합니다. (가까운) UP 경우에는 (4)에 접근하고 일반적인
 * 경우에는 (3)에 접근합니다. ge->load.weight를 일관되게 과대평가하므로 다음과 같습니다.
 *
 *   \Sum ge->load.weight >= tg->weight
 *
 * hence icky!
 *
 * - @cfs_rq의 tg에 대한 기여도만큼 shares을 n등분을 하여 reweight를 한다.
 * - ex) tg shares = 1024,
 *       tg load_avg = 512
 *       cpu0 cfs_rq->tg_load_avg_contrib = 100, load = 95
 *       cpu1 cfs_rq->tg_load_avg_contrib = 412, load = 400
 *
 *       1) cpu0
 *       tg_weight = 512 - 100 + 95 = 507
 *       shares / tg_weight = 1024 * 95 / 507 = 191
 *       2) cpu1
 *       tg_weight = 512 - 412 + 400 = 500
 *       shares / tg_weight = 1024 * 400 / 500 = 819
 *
 *
 * - tg->shares에 대해서 cfs_rq의 load 기여도에따른 비율의 근사값으로
 *   shares 값을 구한다.
 */
static long calc_group_shares(struct cfs_rq *cfs_rq)
{
	long tg_weight, tg_shares, load, shares;
	struct task_group *tg = cfs_rq->tg;


/*
 * IAMROOT, 2023.01.07:
 * - 6번식을 수행한다.
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------		   (6)
 *                             tg_load_avg'
 *
 * Where:
 *
 *   tg_load_avg' = tg->load_avg - grq->avg.load_avg +
 *                  max(grq->load.weight, grq->avg.load_avg)
 *
 * - tg->weight => tg->shares로 치환해서 생각한다.
 *   grq->avg.load_avg => cfs_rq->tg_load_avg_contrib로 치환해서 생각한다.
 *
 *
 * - 풀어쓰기
 *   load = max(scale_load_down(cfs_rq->load.weight), cfs_rq->avg.load_avg);
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------
 *                             tg_load_avg'
 *
 *   =>
 *                          tg->shares * load
 *   return = --------------------------------------------------
 *             tg->load_avg - cfs_rq->tg_load_avg_contrib + load
 *
 */
	tg_shares = READ_ONCE(tg->shares);

/*
 * IAMROOT, 2023.01.07:
 * - @weight보다는 높은 load avg를 사용한다. weight가 0일수도있어서 거기에 대한
 *   예외처리.
 */
	load = max(scale_load_down(cfs_rq->load.weight), cfs_rq->avg.load_avg);

	tg_weight = atomic_long_read(&tg->load_avg);

	/* Ensure tg_weight >= load */
	tg_weight -= cfs_rq->tg_load_avg_contrib;
	tg_weight += load;

	shares = (tg_shares * load);

	if (tg_weight)
		shares /= tg_weight;

	/*
	 * MIN_SHARES has to be unscaled here to support per-CPU partitioning
	 * of a group with small tg->shares value. It is a floor value which is
	 * assigned as a minimum load.weight to the sched_entity representing
	 * the group on a CPU.
	 *
	 * E.g. on 64-bit for a group with tg->shares of scale_load(15)=15*1024
	 * on an 8-core system with 8 tasks each runnable on one CPU shares has
	 * to be 15*1024*1/8=1920 instead of scale_load(MIN_SHARES)=2*1024. In
	 * case no task is runnable on a CPU MIN_SHARES=2 should be returned
	 * instead of 0.
	 */
/*
 * IAMROOT, 2023.01.07:
 * - papago
 *   MIN_SHARES는 tg->shares 값이 작은 그룹의 CPU당 파티셔닝을 지원하기 위해
 *   여기에서 크기를 조정하지 않아야 합니다. CPU에서 그룹을 나타내는
 *   sched_entity에 최소 load.weight로 할당되는 floor 값입니다.
 *
 *   예를 들어 64비트에서 tg->shares of scale_load(15)=15*1024 그룹의 경우
 *   하나의 CPU 공유에서 각각 실행 가능한 8개의 작업이 있는 8코어 시스템에서
 *   15*1024*1/8=1920이 되어야 합니다. scale_load(MIN_SHARES)=2*1024.
 *   CPU에서 실행할 수 있는 작업이 없는 경우 MIN_SHARES=2가 0 대신 반환되어야 합니다.
 */
	return clamp_t(long, shares, MIN_SHARES, tg_shares);
}
#endif /* CONFIG_SMP */

static inline int throttled_hierarchy(struct cfs_rq *cfs_rq);

/*
 * Recomputes the group entity based on the current state of its group
 * runqueue.
 */
/*
 * IAMROOT, 2023.01.07:
 * - @se에 group인경우 tg의 기여도에따른 shares를 weight로
 *   가져와 update한다.
 */
static void update_cfs_group(struct sched_entity *se)
{
	struct cfs_rq *gcfs_rq = group_cfs_rq(se);
	long shares;

/*
 * IAMROOT, 2023.01.07:
 * - @se가 task인경우는 gcfs_rq가 없다. group에 대한 처리를
 *   수행하므로 return.
 */
	if (!gcfs_rq)
		return;

/*
 * IAMROOT, 2023.01.07:
 * - throttle중이면 return.
 */
	if (throttled_hierarchy(gcfs_rq))
		return;

#ifndef CONFIG_SMP

/*
 * IAMROOT, 2023.01.07:
 * - UMP일 경우.
 * - @se weight와 group의 shares가 같으면 return.
 *   reweight을 할 필요가 없다.
 *   초기값은 shares기준으로 만들어지기때문이다.
 *   shares나 weight이 변경되는 경우만 아래로 진입할것이다.
 */
	shares = READ_ONCE(gcfs_rq->tg->shares);

	if (likely(se->load.weight == shares))
		return;
#else

/*
 * IAMROOT, 2023.01.07:
 * - @gcfs_rq의 load 기여도에 따른 shares을 구한다.
 */
	shares   = calc_group_shares(gcfs_rq);
#endif


/*
 * IAMROOT, 2023.01.07:
 * - shares값을 @se의 weight에 적용하고 se의 rq에 변경된 값들을
 *   재적용한다.
 */
	reweight_entity(cfs_rq_of(se), se, shares);
}

#else /* CONFIG_FAIR_GROUP_SCHED */
static inline void update_cfs_group(struct sched_entity *se)
{
}
#endif /* CONFIG_FAIR_GROUP_SCHED */

/*
 * IAMROOT, 2023.01.07:
 * - @cfs_rq가 root이면 cpufreq_update_util을 호출해준다.
 *   @cfs_rq의 상태에 따라 cpu freq를 변경할수도있다.
 */
static inline void cfs_rq_util_change(struct cfs_rq *cfs_rq, int flags)
{
	struct rq *rq = rq_of(cfs_rq);

	if (&rq->cfs == cfs_rq) {
		/*
		 * There are a few boundary cases this might miss but it should
		 * get called often enough that that should (hopefully) not be
		 * a real problem.
		 *
		 * It will not get called when we go idle, because the idle
		 * thread is a different class (!fair), nor will the utilization
		 * number include things like RT tasks.
		 *
		 * As is, the util number is not freq-invariant (we'd have to
		 * implement arch_scale_freq_capacity() for that).
		 *
		 * See cpu_util().
		 */
/*
 * IAMROOT, 2023.01.07:
 * - papago
 *   이것이 놓칠 수 있는 몇 가지 경계 사례가 있지만 실제 문제가 되지 않도록
 *   충분히 자주 호출되어야 합니다.
 *
 *   유휴 스레드가 다른 클래스(!fair)이기 때문에 유휴 상태일 때 호출되지 않으며
 *   사용률에 RT 작업과 같은 항목이 포함되지 않습니다.
 *
 *   있는 그대로, util 번호는 freq-invariant가 아닙니다(이를 위해
 *   arch_scale_freq_capacity()를 구현해야 합니다).
 *
 *   cpu_util()을 참조하십시오.
 */
		cpufreq_update_util(rq, flags);
	}
}

#ifdef CONFIG_SMP
#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * Because list_add_leaf_cfs_rq always places a child cfs_rq on the list
 * immediately before a parent cfs_rq, and cfs_rqs are removed from the list
 * bottom-up, we only have to test whether the cfs_rq before us on the list
 * is our child.
 * If cfs_rq is not on the list, test whether a child needs its to be added to
 * connect a branch to the tree  * (see list_add_leaf_cfs_rq() for details).
 */
/*
 * IAMROOT, 2022.12.27:
 * - papago
 *   list_add_leaf_cfs_rq는 항상 상위 cfs_rq 바로 앞에 하위 cfs_rq를
 *   목록에 배치하고 cfs_rqs는 목록 상향식에서 제거하기 때문에 목록에서
 *   우리 앞에 있는 cfs_rq가 하위인지 여부만 테스트하면 됩니다.
 *   cfs_rq가 목록에 없으면 분기를 트리에 연결하기 위해 자식을 추가해야
 *   하는지 여부를 테스트합니다 * (자세한 내용은 list_add_leaf_cfs_rq() 참조)
 *
 * - 
 */
static inline bool child_cfs_rq_on_list(struct cfs_rq *cfs_rq)
{
	struct cfs_rq *prev_cfs_rq;
	struct list_head *prev;

	if (cfs_rq->on_list) {
		prev = cfs_rq->leaf_cfs_rq_list.prev;
	} else {
		struct rq *rq = rq_of(cfs_rq);

		prev = rq->tmp_alone_branch;
	}

	prev_cfs_rq = container_of(prev, struct cfs_rq, leaf_cfs_rq_list);

	return (prev_cfs_rq->tg->parent == cfs_rq->tg);
}

/*
 * IAMROOT, 2022.12.27:
 * - @cfs_rq가 decay됬는지 확인한다.
 *   weight, load_sum, util_sum, running_sum 전부 @cfs_rq에
 *   load가 있다는것을 의미하고, child_cfs_rq_on_list()가 true면
 *   하위 cfs_rq가 있다는 거고 이또한 load가 있다는 의미다.
 */
static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->load.weight)
		return false;

	if (cfs_rq->avg.load_sum)
		return false;

	if (cfs_rq->avg.util_sum)
		return false;

	if (cfs_rq->avg.runnable_sum)
		return false;

	if (child_cfs_rq_on_list(cfs_rq))
		return false;

	/*
	 * _avg must be null when _sum are null because _avg = _sum / divider
	 * Make sure that rounding and/or propagation of PELT values never
	 * break this.
	 */
	SCHED_WARN_ON(cfs_rq->avg.load_avg ||
		      cfs_rq->avg.util_avg ||
		      cfs_rq->avg.runnable_avg);

	return true;
}

/**
 * update_tg_load_avg - update the tg's load avg
 * @cfs_rq: the cfs_rq whose avg changed
 *
 * This function 'ensures': tg->load_avg := \Sum tg->cfs_rq[]->avg.load.
 * However, because tg->load_avg is a global value there are performance
 * considerations.
 *
 * In order to avoid having to look at the other cfs_rq's, we use a
 * differential update where we store the last value we propagated. This in
 * turn allows skipping updates if the differential is 'small'.
 *
 * Updating tg's load_avg is necessary before update_cfs_share().
 */
/*
 * IAMROOT, 2023.01.07:
 * - papago
 *   update_tg_load_avg - update the tg's load avg
 *   @cfs_rq: the cfs_rq whose avg changed
 *
 *   이 기능은 다음을 '보장'합니다. tg->load_avg := \Sum tg->cfs_rq[]->avg.load.
 *   그러나 tg->load_avg는 전역 값이므로 성능을 고려해야 합니다.
 *
 *   다른 cfs_rq를 볼 필요가 없도록 전파한 마지막 값을 저장하는 차등 업데이트를 사용합니다.
 *   차등이 '작은' 경우 업데이트를 건너뛸 수 있습니다.
 *
 *   update_cfs_share() 전에 tg의 load_avg 업데이트가 필요합니다.
 *
 * - task group에 update한다. 적당한 값이상으로 변화된 경우에만 (직전값의 1 / 64보다 큰값)
 *   update한다.
 */
static inline void update_tg_load_avg(struct cfs_rq *cfs_rq)
{
	long delta = cfs_rq->avg.load_avg - cfs_rq->tg_load_avg_contrib;

	/*
	 * No need to update load_avg for root_task_group as it is not used.
	 */
	if (cfs_rq->tg == &root_task_group)
		return;


/*
 * IAMROOT, 2023.01.07:
 * - @cfs_rq의 taskgroup에 delta를 추가한다.
 *   방금 추가한 @cfs_rq에 대한 load_avg를 tg_load_avg_contrib에 기록해놓는다.
 * - delta가 어느정도 이상인 경우만 update한다.
 */
	if (abs(delta) > cfs_rq->tg_load_avg_contrib / 64) {
		atomic_long_add(delta, &cfs_rq->tg->load_avg);
		cfs_rq->tg_load_avg_contrib = cfs_rq->avg.load_avg;
	}
}

/*
 * Called within set_task_rq() right before setting a task's CPU. The
 * caller only guarantees p->pi_lock is held; no other assumptions,
 * including the state of rq->lock, should be made.
 */
void set_task_rq_fair(struct sched_entity *se,
		      struct cfs_rq *prev, struct cfs_rq *next)
{
	u64 p_last_update_time;
	u64 n_last_update_time;

	if (!sched_feat(ATTACH_AGE_LOAD))
		return;

	/*
	 * We are supposed to update the task to "current" time, then its up to
	 * date and ready to go to new CPU/cfs_rq. But we have difficulty in
	 * getting what current time is, so simply throw away the out-of-date
	 * time. This will result in the wakee task is less decayed, but giving
	 * the wakee more load sounds not bad.
	 */
	if (!(se->avg.last_update_time && prev))
		return;

#ifndef CONFIG_64BIT
	{
		u64 p_last_update_time_copy;
		u64 n_last_update_time_copy;

		do {
			p_last_update_time_copy = prev->load_last_update_time_copy;
			n_last_update_time_copy = next->load_last_update_time_copy;

			smp_rmb();

			p_last_update_time = prev->avg.last_update_time;
			n_last_update_time = next->avg.last_update_time;

		} while (p_last_update_time != p_last_update_time_copy ||
			 n_last_update_time != n_last_update_time_copy);
	}
#else
	p_last_update_time = prev->avg.last_update_time;
	n_last_update_time = next->avg.last_update_time;
#endif
	__update_load_avg_blocked_se(p_last_update_time, se);
	se->avg.last_update_time = n_last_update_time;
}


/*
 * When on migration a sched_entity joins/leaves the PELT hierarchy, we need to
 * propagate its contribution. The key to this propagation is the invariant
 * that for each group:
 *
 *   ge->avg == grq->avg						(1)
 *
 * _IFF_ we look at the pure running and runnable sums. Because they
 * represent the very same entity, just at different points in the hierarchy.
 *
 * Per the above update_tg_cfs_util() and update_tg_cfs_runnable() are trivial
 * and simply copies the running/runnable sum over (but still wrong, because
 * the group entity and group rq do not have their PELT windows aligned).
 *
 * However, update_tg_cfs_load() is more complex. So we have:
 *
 *   ge->avg.load_avg = ge->load.weight * ge->avg.runnable_avg		(2)
 *
 * And since, like util, the runnable part should be directly transferable,
 * the following would _appear_ to be the straight forward approach:
 *
 *   grq->avg.load_avg = grq->load.weight * grq->avg.runnable_avg	(3)
 *
 * And per (1) we have:
 *
 *   ge->avg.runnable_avg == grq->avg.runnable_avg
 *
 * Which gives:
 *
 *                      ge->load.weight * grq->avg.load_avg
 *   ge->avg.load_avg = -----------------------------------		(4)
 *                               grq->load.weight
 *
 * Except that is wrong!
 *
 * Because while for entities historical weight is not important and we
 * really only care about our future and therefore can consider a pure
 * runnable sum, runqueues can NOT do this.
 *
 * We specifically want runqueues to have a load_avg that includes
 * historical weights. Those represent the blocked load, the load we expect
 * to (shortly) return to us. This only works by keeping the weights as
 * integral part of the sum. We therefore cannot decompose as per (3).
 *
 * Another reason this doesn't work is that runnable isn't a 0-sum entity.
 * Imagine a rq with 2 tasks that each are runnable 2/3 of the time. Then the
 * rq itself is runnable anywhere between 2/3 and 1 depending on how the
 * runnable section of these tasks overlap (or not). If they were to perfectly
 * align the rq as a whole would be runnable 2/3 of the time. If however we
 * always have at least 1 runnable task, the rq as a whole is always runnable.
 *
 * So we'll have to approximate.. :/
 *
 * Given the constraint:
 *
 *   ge->avg.running_sum <= ge->avg.runnable_sum <= LOAD_AVG_MAX
 *
 * We can construct a rule that adds runnable to a rq by assuming minimal
 * overlap.
 *
 * On removal, we'll assume each task is equally runnable; which yields:
 *
 *   grq->avg.runnable_sum = grq->avg.load_sum / grq->load.weight
 *
 * XXX: only do this for the part of runnable > running ?
 *
 */

/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   마이그레이션 중에 sched_entity가 PELT 계층 구조에 합류/탈퇴할 때 기여도를
 *   전파해야 합니다. 이 전파의 핵심은 각 그룹에 대한 불변성입니다.
 *
 *   ge->avg == grq->avg						(1)
 *
 *   IFF_ 순수한 running 및 runnable sum을 봅니다. 계층 구조의 다른 지점에서
 *   매우 동일한 entity를 나타내기 때문입니다.
 *
 *   위의 update_tg_cfs_util() 및 update_tg_cfs_runnable()은 사소하고 단순히
 *   running/runnable sum을 복사합니다(그러나 그룹 엔티티 및 그룹 rq에 PELT
 *   window가 정렬되어 있지 않기 때문에 여전히 잘못됨).
 *
 *   그러나 update_tg_cfs_load()는 더 복잡합니다. 그래서 우리는:
 *
 *   ge->avg.load_avg = ge->load.weight * ge->avg.runnable_avg		(2)
 *
 *   그리고 util과 마찬가지로 runnable을 직접 양도할 수 있어야 하므로
 *   다음과 같이 간단하게 접근할 수 있습니다.
 *
 *   grq->avg.load_avg = grq->load.weight * grq->avg.runnable_avg	(3)
 *
 *   그리고 (1)에 따라 다음이 있습니다.
 *
 *   ge->avg.runnable_avg == grq->avg.runnable_avg
 *
 *   다음을 제공합니다.
 *
 *                      ge->load.weight * grq->avg.load_avg
 *   ge->avg.load_avg = -----------------------------------		(4)
 *                               grq->load.weight
 *
 *   틀린 것만 빼면!
 *
 *   entity의 경우 historical weight는 중요하지 않고 실제로는 미래에만 관심이
 *   있으므로 순수 runnable sum을 고려할 수 있지만 rq는 이를 수행할 수
 *   없습니다.
 *   (se는 weight 필요없이 runnable값으로만 계산해도 상관없지만, cfs_rq는
 *   필요해서 결국 weight를 고려해야된다는 말이다.)
 *
 *   우리는 특히 rq가 historical weight를 포함하는 load_avg를 갖기를 원합니다.
 *   그것들은 blocked load, 즉 (곧) 우리에게 돌아올 것으로 예상되는 load를
 *   나타냅니다.
 *   이것은 weight를 sum의 필수적인 부분으로 유지해야만 작동합니다. 따라서
 *   (3)에 따라 분해할 수 없습니다.
 *   (historical weight load_avg가 rq에 필요하기 때문에 결국 load sum을
 *   계산할때 weight를 사용해야된다는 의미.)
 *
 *   이것이 작동하지 않는 또 다른 이유는 runnable이 0-sum entity가 아니기
 *   때문입니다.
 *   각각 2/3 시간 동안 runnable 2개의 작업이 있는 rq를 상상해 보십시오.
 *   그런 다음 rq 자체는 이러한 작업의 runnable 섹션이 겹치는 방식
 *   (또는 겹치지 않음)에 따라 2/3에서 1 사이 어디에서나 runnable 가능합니다.
 *   rq를 전체적으로 완벽하게 정렬하려면 시간의 2/3를 runnable 수 있습니다.
 *   그러나 항상 최소한 하나의 runnable 작업이 있는 경우 전체 rq는 항상
 *   runnable 가능합니다.
 *   (각 task runnable시간의 sum이 group의 sum보다 같거나 작을수박에
 *   없다는의미.)
 *
 *   그래서 우리는 근사해야 할 것입니다.. :/
 *
 *   주어진 제약조건:
 *
 *   ge->avg.running_sum <= ge->avg.runnable_sum <= LOAD_AVG_MAX
 *
 *   최소한의 overlap을 가정하여 rq에 runnable을 추가하는 규칙을 구성할 수 있습니다.
 *   (runnable이 양수인경우엔 overlap의 개념을 도입해서 적당히 그냥 add만
 *   한다는 의미.)
 *
 *   제거할 때 각 작업이 동등하게 runnable이라고가정합니다. 결과는 다음과
 *   같습니다.
 *
 *   grq->avg.runnable_sum = grq->avg.load_sum / grq->load.weight
 *
 *   (runnable이 음수일때는 제거되는 과정이 되므로 overlap된값을 결국 배제하고
 *   좀 정확히 계산해야되므로 식을 사용해 계산해야된다는 의미.)
 *
 *   XXX:
 *   runnable > running ? 부분에 대해서만 이 작업을 수행합니다.
 *
 * ------------------ chat open ai --------------------------------------
 * --- 주석의 줄임말
 * - "IFF"는 "if and only if"의 약자입니다. 명제가 특정 조건에서만 참임을
 *   나타내는 데 사용됩니다.
*
*  -"XXX"는 수행해야 하거나 추가 주의가 필요한 작업에 대한 자리 표시자입니다.
*   검토하거나 수정해야 하는 영역을 나타내기 위해 코드 주석에서 자주
*   사용됩니다.
*
*  - 0-sum entity
*  스케줄러에서 "runnable" entity의 개념입니다.
*  "0-sum"이라는 용어는 작업 그룹에 대한 "runnable" 시간의 sum가 그룹이
*  실행 가능한 총 시간과 반드시 같지는 않다는 사실을 나타냅니다. 이는 그룹의
*  작업이 항상 동시에 실행되지 않을 수 있기 때문에 그룹 전체가 개별 작업의
*  실행 가능한 시간의 합보다 더 긴 시간 동안 실행될 수 있기 때문입니다.
* --------------------
* --- PELT 관련 용어 ---
*  - PELT window
*  task 또는 task gorup의 load 및 util을 추적하는 기간입니다. PELT window의
*  크기는 task 또는 task group의 load 및 util avg이 업데이트되는 빈도에 따라
*  결정됩니다.
*
*  PELT window의 크기는 task 또는 task group의 load 및 util avg이
*  업데이트되는 빈도에 따라 결정됩니다. load 및 uitl avg은 일반적으로
*  1ms의 정기적인 간격으로 업데이트됩니다. 즉, PELT window의 크기도
*  1ms이며 업데이트 간격과 동시에 시작하고 끝납니다.
*
*  - PELT window align
*  서로 다른 task 및 task group의 PELT window을 서로 정렬하면 크기와
*  시작 및 종료 시간이 동일함을 의미합니다. PELT 창을 정렬하면 task 및
*  task group에 대해 계산된 load 및 util avg의 정확도를 개선하는 데 도움이
*  될 수 있습니다.
*
*  예를 들어 A와 B라는 두 개의 task과 CPU에서 실행되는 G라는 task group이
*  있다고 가정합니다. A, B, G의 load 및 util avg은 PELT 알고리즘을 사용하여
*  계산되며 각 task 및 task group에는 자체 PELT window가 있습니다.
*
*  A, B, G의 PELT window가 서로 정렬되어 있지 않으면 크기나 시작 시간과
*  종료 시간이 같지 않다는 의미입니다. PELT 알고리즘은 서로 다른 기간
*  동안 task 및 task group의 load 및 util을 추적하므로 각 task 및
*  task group에 대해 계산된 load 및 util avg이 부정확해질 수 있습니다.
*
*  반면에 A, B, G의 PELT 창들이 서로 정렬되어 있다면 크기와 시작 시간과
*  종료 시간이 같다는 뜻입니다. 이를 통해 PELT 알고리즘은 동일한 기간 동안
*  task 및 task group의 load 및 util을 추적하므로 각 task 및 task group에
*  대해 계산된 load 및 util avg의 정확도를 개선하는 데 도움이 될 수 있습니다.
* --------------------
* --- load 관련 용어 ---
*  - historical weigth
*  과거에 작업 또는 작업 그룹에 주어진 CPU 시간의 양을 나타냅니다.
*  스케줄러에서 작업 또는 작업 그룹의 "load_avg"는 향후 소비할 것으로
*  예상되는 CPU 시간을 측정한 것입니다. 이 load_avg 값은 작업 또는 그룹의
*  현재 및 과거 CPU 사용량을 기반으로 하는 경우가 많습니다.
*
*  예를 들어 오랫동안 CPU에서 실행되어 온 작업 그룹을 생각해 보십시오.
*  이 그룹은 과거에 많은 CPU 시간을 소비했기 때문에 높은 "역사적 가중치"를
*  갖게 됩니다. 그룹이 앞으로도 비슷한 양의 CPU 시간을 계속 사용할 것으로
*  예상되면 해당 load_avg 값이 높아집니다. 반면에 그룹이 앞으로 더 적은
*  CPU 시간을 소비할 것으로 예상되는 경우에는 load_avg 값이 더 낮아집니다.
*
* - overlap
*  A와 B라는 두 개의 작업이 있는 실행 대기열이 있다고 상상해 보십시오.
*  작업 A는 실행 가능한 시간의 50%이고 작업 B는 실행 가능한 시간의
*  40%입니다. 이 경우 두 작업의 실행 가능한 기간이 완전히 겹치기 때문에
*  runqueue는 항상 실행 가능합니다. 작업 A가 실행 가능할 때 작업 B도 실행
*  가능한 시간의 40%이고 작업 B가 실행 가능한 경우 작업 A도 실행 가능한
*  시간의 50%이기 때문입니다.
*
*  이제 C와 D라는 두 개의 작업이 있는 실행 대기열이 있다고 상상해 보십시오.
*  작업 C는 실행 가능한 시간의 60%이고 작업 D는 실행 가능한 시간의
*  70%입니다. 이 경우 runqueue는 두 작업의 실행 가능한 기간이 겹치는 방식에
*  따라 시간의 60%에서 100% 사이 어디에서나 실행할 수 있습니다. 두 작업을
*  동시에 실행할 수 없는 경우 실행 대기열은 시간의 60%만 실행할 수
*  있습니다(각 작업이 실행 가능한 최소 시간). 두 작업이 항상 동시에 실행
*  가능한 경우 실행 대기열은 시간의 100% 실행 가능합니다(각 작업이 실행
*   가능한 최대 시간). 두 작업의 실행 가능 기간이 겹치면 실행 대기열
*   실행 가능 시간이 60%에서 100% 사이가 됩니다.
*
* - assuming minimal overlap
*   runqueue(rq)에 대한 실행 가능 시간을 계산할 때 runqueue에 있는 작업의
*   실행 가능 기간이 많이 겹치지 않는다고 가정하는 것을 의미합니다. 이로
*   인해 runqueue의 실행 가능한 시간에 대한 추정치가 낮아집니다.
*
*   예를 들어 E와 F라는 두 개의 작업이 있는 실행 대기열이 있고 실행 대기열의
*   실행 가능 시간을 계산하려는 경우 두 작업의 실행 가능 기간이 전혀 겹치지
*   않는다고 가정할 수 있습니다. 이 경우 실행 대기열에 대한 총 실행 가능
*   시간을 얻기 위해 각 작업에 대한 실행 가능 시간을 추가하기만 하면 됩니다.
*   이렇게 하면 두 작업 간의 중복이 최소화된다고 가정하기 때문에 실행
*   대기열의 실행 가능한 시간에 대한 추정치가 낮아집니다.
*
*   반면에 두 작업의 실행 가능한 기간이 상당히 겹친다고 가정하면 실행
*   대기열의 실행 가능한 시간에 대해 더 높은 추정치를 얻을 수 있습니다.
*   작업이 동시에 더 자주 실행될 수 있다고 가정하기 때문에 실행 대기열의
*   전체 실행 가능 시간이 늘어납니다.
* --------------------
 * ----------------------------------------------------------------------
* - @gcfs_rq나 @se에 변화가 생긴경우 @se, @cfs를 update한다.
*/
static inline void
update_tg_cfs_util(struct cfs_rq *cfs_rq, struct sched_entity *se, struct cfs_rq *gcfs_rq)
{

/*
 * IAMROOT, 2022.12.31:
 * - @gcfs_rq나 @se에 변화가 생긴경우에만 고려한다.
 */
	long delta = gcfs_rq->avg.util_avg - se->avg.util_avg;
	u32 divider;

	/* Nothing to update */
	if (!delta)
		return;

	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	divider = get_pelt_divider(&cfs_rq->avg);

	/* Set new sched_entity's utilization */
/*
 * IAMROOT, 2022.12.31:
 * - gcfs_rq값을 new로 생각하고 group se로 update한다.
 */
	se->avg.util_avg = gcfs_rq->avg.util_avg;
	se->avg.util_sum = se->avg.util_avg * divider;

	/* Update parent cfs_rq utilization */
/*
 * IAMROOT, 2022.12.31:
 * - 소속 @cfs_rq엔 변화된값을 적용한다.
 */
	add_positive(&cfs_rq->avg.util_avg, delta);
	cfs_rq->avg.util_sum = cfs_rq->avg.util_avg * divider;
}

/*
 * IAMROOT, 2022.12.31:
 * - util과 동일하게 진행한다.
 */
static inline void
update_tg_cfs_runnable(struct cfs_rq *cfs_rq, struct sched_entity *se, struct cfs_rq *gcfs_rq)
{
	long delta = gcfs_rq->avg.runnable_avg - se->avg.runnable_avg;
	u32 divider;

	/* Nothing to update */
	if (!delta)
		return;

	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	divider = get_pelt_divider(&cfs_rq->avg);

	/* Set new sched_entity's runnable */
	se->avg.runnable_avg = gcfs_rq->avg.runnable_avg;
	se->avg.runnable_sum = se->avg.runnable_avg * divider;

	/* Update parent cfs_rq runnable */
	add_positive(&cfs_rq->avg.runnable_avg, delta);
	cfs_rq->avg.runnable_sum = cfs_rq->avg.runnable_avg * divider;
}

/*
 * IAMROOT, 2022.12.31:
 * - propagate runnable sum값의 증감 유무에 따라 @se의 load를 update하고
 *   변경된 값만큼 @cfs_rq의 load를 재계산한다.
 *
 * 1. 증가하는경우 assuming minimal overlap을 적용하는 개념으로
 *    add 와 divider에 대한 min처리만 수행한다.
 *
 * 2. 감소하는 경우는 weight를 고려한다. 위쪽 주석의 식에 따라
 *                      ge->load.weight * grq->avg.load_avg
 *   ge->avg.load_avg = -----------------------------------
 *                               grq->load.weight
 *
 *   를 사용하여 재계산한다. 단 재계산값이 증가했을 경우엔 min처리를 한다.
 */
static inline void
update_tg_cfs_load(struct cfs_rq *cfs_rq, struct sched_entity *se, struct cfs_rq *gcfs_rq)
{
	long delta, running_sum, runnable_sum = gcfs_rq->prop_runnable_sum;
	unsigned long load_avg;
	u64 load_sum = 0;
	u32 divider;

	if (!runnable_sum)
		return;

	gcfs_rq->prop_runnable_sum = 0;

	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	divider = get_pelt_divider(&cfs_rq->avg);

/*
 * IAMROOT, 2022.12.31:
 * - 증가됬다면 asumming minimal overlap을 적용하여, 그냥 더하기만 한다.
 *   overlap이 있기때문에 max값보다 커질수있으므로 이를 고려한 min처리를
 *   한다.
 */
	if (runnable_sum >= 0) {
		/*
		 * Add runnable; clip at LOAD_AVG_MAX. Reflects that until
		 * the CPU is saturated running == runnable.
		 */
		runnable_sum += se->avg.load_sum;
		runnable_sum = min_t(long, runnable_sum, divider);
	} else {

/*
 * IAMROOT, 2022.12.31:
 * - 감소됬다면 task runnable이 균등하게 계산한다는 관점으로 avg와 weight로
 *   재계산한다.
 *
 *   runnable_sum = grq->avg.load_sum / grq->load.weight
 */
		/*
		 * Estimate the new unweighted runnable_sum of the gcfs_rq by
		 * assuming all tasks are equally runnable.
		 */
		if (scale_load_down(gcfs_rq->load.weight)) {
			load_sum = div_s64(gcfs_rq->avg.load_sum,
				scale_load_down(gcfs_rq->load.weight));
		}

		/* But make sure to not inflate se's runnable */
/*
 * IAMROOT, 2022.12.31:
 * - 균등하게 계산을 했는데, 오히려 더 커진 경우가 있을수있다.
 *   (반영을 할려는 cfs_rq의 weight와 gcfs_rq의 weight가 다를수도잇는 상태등)
 *   min 비교를 해서 커지지 않게 조절한다.
 */
		runnable_sum = min(se->avg.load_sum, load_sum);
	}

	/*
	 * runnable_sum can't be lower than running_sum
	 * Rescale running sum to be in the same range as runnable sum
	 * running_sum is in [0 : LOAD_AVG_MAX <<  SCHED_CAPACITY_SHIFT]
	 * runnable_sum is in [0 : LOAD_AVG_MAX]
	 */

/*
 * IAMROOT, 2022.12.31:
 * - runnable_sum이 running_sum보다 더 크거나 같은 개념.
 *   (ge->avg.running_sum <= ge->avg.runnable_sum <= LOAD_AVG_MAX )
 */
	running_sum = se->avg.util_sum >> SCHED_CAPACITY_SHIFT;
	runnable_sum = max(runnable_sum, running_sum);

/*
 * IAMROOT, 2022.12.31:
 * - se에 대한 load_avg를 weights가중치를 포함해 계산한다.
 *
 *                      ge->load.weight * grq->avg.load_avg
 *   ge->avg.load_avg = -----------------------------------		(4)
 *                               grq->load.weight
 */
	load_sum = (s64)se_weight(se) * runnable_sum;
	load_avg = div_s64(load_sum, divider);

	se->avg.load_sum = runnable_sum;

/*
 * IAMROOT, 2022.12.31:
 * - avg에 대한 변화가 있으면 소속 cfs_rq에 delta를 가중한다.
 */
	delta = load_avg - se->avg.load_avg;
	if (!delta)
		return;

	se->avg.load_avg = load_avg;

	add_positive(&cfs_rq->avg.load_avg, delta);
	cfs_rq->avg.load_sum = cfs_rq->avg.load_avg * divider;
}

/*
 * IAMROOT, 2022.12.31:
 * - propagate를 해야된다는걸 알리고, 값을 누적시켜놓는다.
 *   나중에 할것이다.
 */
static inline void add_tg_cfs_propagate(struct cfs_rq *cfs_rq, long runnable_sum)
{
	cfs_rq->propagate = 1;
	cfs_rq->prop_runnable_sum += runnable_sum;
}

/* Update task and its cfs_rq load average */
/*
 * IAMROOT, 2022.12.31:
 * @return 1 propagate 요청있었음. 0. 없엇음.
 * 1. propagate 확인 및 상위에 전달.
 * 2. @se와 @cfs_rq에 대해 util, runnable, load update.
 */
static inline int propagate_entity_load_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq, *gcfs_rq;

/*
 * IAMROOT, 2022.12.31:
 * - entity가 task group인 경우에만 propagate 하면된다.
 */
	if (entity_is_task(se))
		return 0;


/*
 * IAMROOT, 2022.12.31:
 * - propagate 요청이 있는지 확인한다.
 */
	gcfs_rq = group_cfs_rq(se);
	if (!gcfs_rq->propagate)
		return 0;

	gcfs_rq->propagate = 0;

	cfs_rq = cfs_rq_of(se);

/*
 * IAMROOT, 2022.12.31:
 * - @se가 소속된 cfs_rq에 propagate를 알린다.
 */
	add_tg_cfs_propagate(cfs_rq, gcfs_rq->prop_runnable_sum);

	update_tg_cfs_util(cfs_rq, se, gcfs_rq);
	update_tg_cfs_runnable(cfs_rq, se, gcfs_rq);
	update_tg_cfs_load(cfs_rq, se, gcfs_rq);

	trace_pelt_cfs_tp(cfs_rq);
	trace_pelt_se_tp(se);

	return 1;
}

/*
 * Check if we need to update the load and the utilization of a blocked
 * group_entity:
 */
/*
 * IAMROOT, 2023.05.03:
 * - papago
 *   차단된 group_entity의 부하 및 사용률을 업데이트해야 하는지 확인합니다.
 *
 * @return true update할 필요없다.(skip)
 *
 * - load나 propagat중이라면 update를 해야된다(return false).
 *   그에 대하 확인을 진행한다.
 */
static inline bool skip_blocked_update(struct sched_entity *se)
{
	struct cfs_rq *gcfs_rq = group_cfs_rq(se);

	/*
	 * If sched_entity still have not zero load or utilization, we have to
	 * decay it:
	 */
/*
 * IAMROOT, 2023.05.03:
 * - papago
 *   sched_entity가 여전히 로드 또는 사용률이 0이 아닌 경우 이를 소멸시켜야 합니다.
 */
	if (se->avg.load_avg || se->avg.util_avg)
		return false;

	/*
	 * If there is a pending propagation, we have to update the load and
	 * the utilization of the sched_entity:
	 */
/*
 * IAMROOT, 2023.05.03:
 * - papago
 *   보류 중인 전파가 있는 경우 sched_entity의 로드 및 사용률을 업데이트해야 합니다.
 */
	if (gcfs_rq->propagate)
		return false;

	/*
	 * Otherwise, the load and the utilization of the sched_entity is
	 * already zero and there is no pending propagation, so it will be a
	 * waste of time to try to decay it:
	 */
/*
 * IAMROOT, 2023.05.03:
 * - papago
 *   그렇지 않으면 sched_entity의 부하와 사용률이 이미 0이고 보류 중인 전파가 
 *   없으므로 이를 소멸시키려고 시도하는 것은 시간 낭비입니다.
 */
	return true;
}

#else /* CONFIG_FAIR_GROUP_SCHED */

static inline void update_tg_load_avg(struct cfs_rq *cfs_rq) {}

static inline int propagate_entity_load_avg(struct sched_entity *se)
{
	return 0;
}

static inline void add_tg_cfs_propagate(struct cfs_rq *cfs_rq, long runnable_sum) {}

#endif /* CONFIG_FAIR_GROUP_SCHED */

/**
 * update_cfs_rq_load_avg - update the cfs_rq's load/util averages
 * @now: current time, as per cfs_rq_clock_pelt()
 * @cfs_rq: cfs_rq to update
 *
 * The cfs_rq avg is the direct sum of all its entities (blocked and runnable)
 * avg. The immediate corollary is that all (fair) tasks must be attached, see
 * post_init_entity_util_avg().
 *
 * cfs_rq->avg is used for task_h_load() and update_cfs_share() for example.
 *
 * Returns true if the load decayed or we removed load.
 *
 * Since both these conditions indicate a changed cfs_rq->avg.load we should
 * call update_tg_load_avg() when this function returns true.
 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   update_cfs_rq_load_avg - cfs_rq의 load/유틸 avg 업데이트
 *   @now: cfs_rq_clock_pelt()에 따른 현재 시간.
 *   @cfs_rq: 업데이트할 cfs_rq.
 *
 *   cfs_rq avg는 모든 entity(차단 및 실행 가능) avg의 직접 sum입니다.
 *   즉각적인 결과는 모든 (공정한) 작업이 연결되어야 한다는 것입니다.
 *   post_init_entity_util_avg()를 참조하십시오.
 *
 *   cfs_rq->avg는 예를 들어 task_h_load() 및 update_cfs_share()에 사용됩니다.
 *
 *   load가 감소했거나 load를 제거한 경우 true를 반환합니다.
 *
 *   이 두 조건 모두 변경된 cfs_rq->avg.load를 나타내므로 이 함수가 true를
 *   반환할 때 update_tg_load_avg()를 호출해야 합니다.
 *
 * @return decay가 됬으면 return 1.
 *
 * - 1. removed된게 있으면 removed field를 초기화하면서,
 *      removed됬던 값들을 cfs_rq에 적용한다.
 *   2. cfs_rq load update.
 *   3. Propagate 요청 여부 set.
 *   4. removed가 된게 있거나 cfs_rq load가 됬다면 decay가 된것이므로 return 1.
 */
static inline int
update_cfs_rq_load_avg(u64 now, struct cfs_rq *cfs_rq)
{
	unsigned long removed_load = 0, removed_util = 0, removed_runnable = 0;
	struct sched_avg *sa = &cfs_rq->avg;
	int decayed = 0;

/*
 * IAMROOT, 2022.12.31:
 * - removed된게 있었다면.
 */
	if (cfs_rq->removed.nr) {
		unsigned long r;
		u32 divider = get_pelt_divider(&cfs_rq->avg);


/*
 * IAMROOT, 2022.12.31:
 * - 0값들과 swap을 하면서 old는 지역변수에 가져온다.
 */
		raw_spin_lock(&cfs_rq->removed.lock);
		swap(cfs_rq->removed.util_avg, removed_util);
		swap(cfs_rq->removed.load_avg, removed_load);
		swap(cfs_rq->removed.runnable_avg, removed_runnable);
		cfs_rq->removed.nr = 0;
		raw_spin_unlock(&cfs_rq->removed.lock);

/*
 * IAMROOT, 2022.12.31:
 * - removed된 avg값들을 cfs_rq에 avg, sum에 빼준다.
 *   sum / div = avg
 *   sum = avg * div
 */
		r = removed_load;
		sub_positive(&sa->load_avg, r);
		sa->load_sum = sa->load_avg * divider;

		r = removed_util;
		sub_positive(&sa->util_avg, r);
		sa->util_sum = sa->util_avg * divider;

		r = removed_runnable;
		sub_positive(&sa->runnable_avg, r);
		sa->runnable_sum = sa->runnable_avg * divider;

		/*
		 * removed_runnable is the unweighted version of removed_load so we
		 * can use it to estimate removed_load_sum.
		 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   removed_runnable은 removed_load의 비가중 버전이므로 removed_load_sum을
 *   추정하는 데 사용할 수 있습니다.
 *
 * - 감소된값을 propagate해야된다. 감소된값을 기록한다.
 */
		add_tg_cfs_propagate(cfs_rq,
			-(long)(removed_runnable * divider) >> SCHED_CAPACITY_SHIFT);

		decayed = 1;
	}

/*
 * IAMROOT, 2022.12.31:
 * - cfs_rq의 load update.
 */
	decayed |= __update_load_avg_cfs_rq(now, cfs_rq);

#ifndef CONFIG_64BIT
	smp_wmb();
	cfs_rq->load_last_update_time_copy = sa->last_update_time;
#endif

	return decayed;
}

/**
 * attach_entity_load_avg - attach this entity to its cfs_rq load avg
 * @cfs_rq: cfs_rq to attach to
 * @se: sched_entity to attach
 *
 * Must call update_cfs_rq_load_avg() before this, since we rely on
 * cfs_rq->avg.last_update_time being current.
 */
/*
 * IAMROOT, 2023.01.07:
 * - @se가 @cfs_rq에 attach되는 상황. @se의 값들을 @cfs_rq에 update하고 상위에 전파한다.
 * - task일 경우 방금 생겻을경우 load가 없을수 있지만, group일 경우 기존에 load가 있을수있다는
 *   걸 고려한다.
 * - sum 값들의 경우
 *   sum = avg * time 의 개념으로 sum을 만들어 넣는다.
 */
static void attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	u32 divider = get_pelt_divider(&cfs_rq->avg);

	/*
	 * When we attach the @se to the @cfs_rq, we must align the decay
	 * window because without that, really weird and wonderful things can
	 * happen.
	 *
	 * XXX illustrate
	 */
/*
 * IAMROOT, 2023.01.07:
 * - papago
 *   @se를 @cfs_rq에 첨부할 때 decay window 을 정렬해야 합니다. 그렇지 않으면 정말 이상하고
 *   멋진 일이 발생할 수 있기 때문입니다.
 *   (entity 입장에서의 decay window에 대한 정렬을 의미)
 */
	se->avg.last_update_time = cfs_rq->avg.last_update_time;
	se->avg.period_contrib = cfs_rq->avg.period_contrib;

	/*
	 * Hell(o) Nasty stuff.. we need to recompute _sum based on the new
	 * period_contrib. This isn't strictly correct, but since we're
	 * entirely outside of the PELT hierarchy, nobody cares if we truncate
	 * _sum a little.
	 */
/*
 * IAMROOT, 2023.01.07:
 * - papago
 *   Hell(o) 불쾌한 물건.. 새 period_contrib를 기반으로 _sum을 다시 계산해야 합니다.
 *   이것은 정확하지는 않지만 우리는 완전히 PELT 계층 구조 밖에 있기 때문에 _sum을
 *   약간 잘라도 아무도 신경 쓰지 않습니다.
 */
	se->avg.util_sum = se->avg.util_avg * divider;

	se->avg.runnable_sum = se->avg.runnable_avg * divider;

	se->avg.load_sum = divider;

/*
 * IAMROOT, 2023.01.07:
 * - se_weight(se) == 0 때는 se->avg.load_sum = divider로 사용한다.
 */
	if (se_weight(se)) {
		se->avg.load_sum =
			div_u64(se->avg.load_avg * se->avg.load_sum, se_weight(se));
	}

/*
 * IAMROOT, 2023.01.07:
 * - @cfs_rq load_avg, load_sum update.
 */
	enqueue_load_avg(cfs_rq, se);

/*
 * IAMROOT, 2023.01.07:
 * - util은 사용률의 개념이라 그냥 적산.
 * - runnable은 이미 weight가 적용되있으므로 그냥 적산.
 */
	cfs_rq->avg.util_avg += se->avg.util_avg;
	cfs_rq->avg.util_sum += se->avg.util_sum;
	cfs_rq->avg.runnable_avg += se->avg.runnable_avg;
	cfs_rq->avg.runnable_sum += se->avg.runnable_sum;


/*
 * IAMROOT, 2023.01.07:
 * - @cfs_rq에 들어왔으니까, 상위한테도 이를 전파한다.
 */
	add_tg_cfs_propagate(cfs_rq, se->avg.load_sum);

	cfs_rq_util_change(cfs_rq, 0);

	trace_pelt_cfs_tp(cfs_rq);
}

/**
 * detach_entity_load_avg - detach this entity from its cfs_rq load avg
 * @cfs_rq: cfs_rq to detach from
 * @se: sched_entity to detach
 *
 * Must call update_cfs_rq_load_avg() before this, since we rely on
 * cfs_rq->avg.last_update_time being current.
 */
static void detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	u32 divider = get_pelt_divider(&cfs_rq->avg);

	dequeue_load_avg(cfs_rq, se);
	sub_positive(&cfs_rq->avg.util_avg, se->avg.util_avg);
	cfs_rq->avg.util_sum = cfs_rq->avg.util_avg * divider;
	sub_positive(&cfs_rq->avg.runnable_avg, se->avg.runnable_avg);
	cfs_rq->avg.runnable_sum = cfs_rq->avg.runnable_avg * divider;

	add_tg_cfs_propagate(cfs_rq, -se->avg.load_sum);

	cfs_rq_util_change(cfs_rq, 0);

	trace_pelt_cfs_tp(cfs_rq);
}

/*
 * Optional action to be done while updating the load average
 */
/*
 * IAMROOT, 2022.12.22:
 * - UPDATE_TG: taskgroup을 update하라는 의미.
 * - DO_ATTACH : cfs rq에 enqueue 된경우.
 */
#define UPDATE_TG	0x1
#define SKIP_AGE_LOAD	0x2
#define DO_ATTACH	0x4

/* Update task and its cfs_rq load average */
/*
 * IAMROOT, 2022.12.22:
 * @flags entity_tick에서 불러졌을경우 : UPDATE_TG
 *        enqueue_entity의 경우 : UPDATE_TG | DO_ATTACH
 *        attach_entity_cfs_rq  : ATTACH_AGE_LOAD sched_feat가 없는경우 SKIP_AGE_LOAD
 *
 *  - 1. update를 했던 @se인 경우, @se에 대한 load sum, load avg 재계산.
 *    2. @cfs_rq에 대한 load avg 계산.
 *    3. @se에 대한 전파 처리.
 *    4. @se가 attach인 경우 @cfs_rq에 attach.
 *    5. decay됫거나 attach인경우
 *       cpu freq 변경시도, taskgroup에 변경을 알려야되는경우 update.
 */
static inline void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	u64 now = cfs_rq_clock_pelt(cfs_rq);
	int decayed;

	/*
	 * Track task load average for carrying it to new CPU after migrated, and
	 * track group sched_entity load average for task_h_load calc in migration
	 */
	/*
	 * IAMROOT. 2022.12.17:
	 * - google-translate
	 *   마이그레이션 후 새 CPU로 옮기기 위한 작업 load avg 추적 및 마이그레이션에서
	 *   task_h_load calc에 대한 그룹 sched_entity load avg 추적
	 *
	 * - 이전에 update를 했다면, @se에 대한 load sum, avg를 구한다.
	 */
	if (se->avg.last_update_time && !(flags & SKIP_AGE_LOAD))
		__update_load_avg_se(now, cfs_rq, se);

/*
 * IAMROOT, 2022.12.31:
 * - cfs_rq에 대한 removed 처리 및, load update.
 *   se에대한 propagate처리.
 * - decay가 됬거나 propagate를 했으면 decayed != 0
 */
	decayed  = update_cfs_rq_load_avg(now, cfs_rq);
	decayed |= propagate_entity_load_avg(se);

/*
 * IAMROOT, 2022.12.31:
 * - 최초이자(entity가 한번도 계산된적없음), ATTACH flag가 있는경우 진입.)
 */
	if (!se->avg.last_update_time && (flags & DO_ATTACH)) {

		/*
		 * DO_ATTACH means we're here from enqueue_entity().
		 * !last_update_time means we've passed through
		 * migrate_task_rq_fair() indicating we migrated.
		 *
		 * IOW we're enqueueing a task on a new CPU.
		 */

/*
 * IAMROOT, 2023.01.07:
 * - 최초이므로, @se를 @cfs_rq에 attach시킨다.
 *   task group에도 update.
 */
		attach_entity_load_avg(cfs_rq, se);
		update_tg_load_avg(cfs_rq);

	} else if (decayed) {

/*
 * IAMROOT, 2023.01.07:
 * - cpu freq 변경시도.
 */
		cfs_rq_util_change(cfs_rq, 0);

/*
 * IAMROOT, 2023.01.07:
 * - taskgrup upate를 해야되면 한다.
 */
		if (flags & UPDATE_TG)
			update_tg_load_avg(cfs_rq);
	}
}

#ifndef CONFIG_64BIT
/*
 * IAMROOT, 2023.06.03:
 * - load_last_update_time_copy와 last_update_time이 sync할때까지
 *   맞춘다.
 */
static inline u64 cfs_rq_last_update_time(struct cfs_rq *cfs_rq)
{
	u64 last_update_time_copy;
	u64 last_update_time;

	do {
		last_update_time_copy = cfs_rq->load_last_update_time_copy;
		smp_rmb();
		last_update_time = cfs_rq->avg.last_update_time;
	} while (last_update_time != last_update_time_copy);

	return last_update_time;
}
#else
static inline u64 cfs_rq_last_update_time(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.last_update_time;
}
#endif

/*
 * Synchronize entity load avg of dequeued entity without locking
 * the previous rq.
 */
/*
 * IAMROOT, 2023.06.03:
 * - @se cfs_rq의 last_update_time으로 decay한다.
 *
 * IAMROOT. 2023.06.02:
 * - google-translate
 * 이전 rq를 잠그지 않고 큐에서 제거된 엔터티의 엔터티 로드 평균을 동기화합니다.
 */
static void sync_entity_load_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	u64 last_update_time;

	last_update_time = cfs_rq_last_update_time(cfs_rq);
	__update_load_avg_blocked_se(last_update_time, se);
}

/*
 * Task first catches up with cfs_rq, and then subtract
 * itself from the cfs_rq (task must be off the queue now).
 */
static void remove_entity_load_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	unsigned long flags;

	/*
	 * tasks cannot exit without having gone through wake_up_new_task() ->
	 * post_init_entity_util_avg() which will have added things to the
	 * cfs_rq, so we can remove unconditionally.
	 */

	sync_entity_load_avg(se);

	raw_spin_lock_irqsave(&cfs_rq->removed.lock, flags);
	++cfs_rq->removed.nr;
	cfs_rq->removed.util_avg	+= se->avg.util_avg;
	cfs_rq->removed.load_avg	+= se->avg.load_avg;
	cfs_rq->removed.runnable_avg	+= se->avg.runnable_avg;
	raw_spin_unlock_irqrestore(&cfs_rq->removed.lock, flags);
}

static inline unsigned long cfs_rq_runnable_avg(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.runnable_avg;
}

static inline unsigned long cfs_rq_load_avg(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.load_avg;
}

static int newidle_balance(struct rq *this_rq, struct rq_flags *rf);

static inline unsigned long task_util(struct task_struct *p)
{
	return READ_ONCE(p->se.avg.util_avg);
}

/*
 * IAMROOT, 2023.05.03:
 * - ewma, enqueued중에 큰거를 가져온다.(util_est 주석참고)
 */
static inline unsigned long _task_util_est(struct task_struct *p)
{
	struct util_est ue = READ_ONCE(p->se.avg.util_est);

	return max(ue.ewma, (ue.enqueued & ~UTIL_AVG_UNCHANGED));
}

/*
 * IAMROOT, 2023.05.03:
 * - rq의 load_avg, se의 ewma, enqueued중에 큰거를 가져온다.
 */
static inline unsigned long task_util_est(struct task_struct *p)
{
	return max(task_util(p), _task_util_est(p));
}

#ifdef CONFIG_UCLAMP_TASK
/*
 * IAMROOT, 2023.05.03:
 * - UCLAMP_MIN <= max(rq load_avg, se util_est  ewma,
 *                     se util_est enqueue) <= UCLAMP_MAX
 */
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
	return clamp(task_util_est(p),
		     uclamp_eff_value(p, UCLAMP_MIN),
		     uclamp_eff_value(p, UCLAMP_MAX));
}
#else
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
	return task_util_est(p);
}
#endif

static inline void util_est_enqueue(struct cfs_rq *cfs_rq,
				    struct task_struct *p)
{
	unsigned int enqueued;

	if (!sched_feat(UTIL_EST))
		return;

	/* Update root cfs_rq's estimated utilization */
	enqueued  = cfs_rq->avg.util_est.enqueued;
	enqueued += _task_util_est(p);
	WRITE_ONCE(cfs_rq->avg.util_est.enqueued, enqueued);

	trace_sched_util_est_cfs_tp(cfs_rq);
}

static inline void util_est_dequeue(struct cfs_rq *cfs_rq,
				    struct task_struct *p)
{
	unsigned int enqueued;

	if (!sched_feat(UTIL_EST))
		return;

	/* Update root cfs_rq's estimated utilization */
	enqueued  = cfs_rq->avg.util_est.enqueued;
	enqueued -= min_t(unsigned int, enqueued, _task_util_est(p));
	WRITE_ONCE(cfs_rq->avg.util_est.enqueued, enqueued);

	trace_sched_util_est_cfs_tp(cfs_rq);
}

#define UTIL_EST_MARGIN (SCHED_CAPACITY_SCALE / 100)

/*
 * Check if a (signed) value is within a specified (unsigned) margin,
 * based on the observation that:
 *
 *     abs(x) < y := (unsigned)(x + y - 1) < (2 * y - 1)
 *
 * NOTE: this only works when value + margin < INT_MAX.
 */
static inline bool within_margin(int value, int margin)
{
	return ((unsigned int)(value + margin - 1) < (2 * margin - 1));
}

static inline void util_est_update(struct cfs_rq *cfs_rq,
				   struct task_struct *p,
				   bool task_sleep)
{
	long last_ewma_diff, last_enqueued_diff;
	struct util_est ue;

	if (!sched_feat(UTIL_EST))
		return;

	/*
	 * Skip update of task's estimated utilization when the task has not
	 * yet completed an activation, e.g. being migrated.
	 */
	if (!task_sleep)
		return;

	/*
	 * If the PELT values haven't changed since enqueue time,
	 * skip the util_est update.
	 */
	ue = p->se.avg.util_est;
	if (ue.enqueued & UTIL_AVG_UNCHANGED)
		return;

	last_enqueued_diff = ue.enqueued;

	/*
	 * Reset EWMA on utilization increases, the moving average is used only
	 * to smooth utilization decreases.
	 */
	ue.enqueued = task_util(p);
	if (sched_feat(UTIL_EST_FASTUP)) {
		if (ue.ewma < ue.enqueued) {
			ue.ewma = ue.enqueued;
			goto done;
		}
	}

	/*
	 * Skip update of task's estimated utilization when its members are
	 * already ~1% close to its last activation value.
	 */
	last_ewma_diff = ue.enqueued - ue.ewma;
	last_enqueued_diff -= ue.enqueued;
	if (within_margin(last_ewma_diff, UTIL_EST_MARGIN)) {
		if (!within_margin(last_enqueued_diff, UTIL_EST_MARGIN))
			goto done;

		return;
	}

	/*
	 * To avoid overestimation of actual task utilization, skip updates if
	 * we cannot grant there is idle time in this CPU.
	 */
	if (task_util(p) > capacity_orig_of(cpu_of(rq_of(cfs_rq))))
		return;

	/*
	 * Update Task's estimated utilization
	 *
	 * When *p completes an activation we can consolidate another sample
	 * of the task size. This is done by storing the current PELT value
	 * as ue.enqueued and by using this value to update the Exponential
	 * Weighted Moving Average (EWMA):
	 *
	 *  ewma(t) = w *  task_util(p) + (1-w) * ewma(t-1)
	 *          = w *  task_util(p) +         ewma(t-1)  - w * ewma(t-1)
	 *          = w * (task_util(p) -         ewma(t-1)) +     ewma(t-1)
	 *          = w * (      last_ewma_diff            ) +     ewma(t-1)
	 *          = w * (last_ewma_diff  +  ewma(t-1) / w)
	 *
	 * Where 'w' is the weight of new samples, which is configured to be
	 * 0.25, thus making w=1/4 ( >>= UTIL_EST_WEIGHT_SHIFT)
	 */
	ue.ewma <<= UTIL_EST_WEIGHT_SHIFT;
	ue.ewma  += last_ewma_diff;
	ue.ewma >>= UTIL_EST_WEIGHT_SHIFT;
done:
	ue.enqueued |= UTIL_AVG_UNCHANGED;
	WRITE_ONCE(p->se.avg.util_est, ue);

	trace_sched_util_est_se_tp(&p->se);
}

/*
 * IAMROOT, 2023.05.03:
 * - return uclamp_task_util(p) * 1.2 <= capacity
 * - uclamp_task_util에 20%의 가중치를 둔값이, capacity보다 큰지를 확인한다.
 *
 * IAMROOT, 2023.05.19:
 * - @p가 cpu에서 curr로 선택된게 80% 이하인가?
 */
static inline int task_fits_capacity(struct task_struct *p, long capacity)
{
	return fits_capacity(uclamp_task_util(p), capacity);
}

/*
 * IAMROOT, 2023.01.28:
 * - misfit_task_load를 갱신한다.
 * - misfit_task_load 미갱신
 *   1. sched_asym_cpucapacity disable
 * - misfit_task_load = 0
 *   1. p == NULL이거나 allow cpu가 1개
 *   2. @p의 load 1.2배값이 @rq의 cpu보다 작거나 같은 경우
 * - misfit_task_load update
 *   1. @p의 load 1.2배값이 @rq의 cpu보다 큰경우.
 */
static inline void update_misfit_status(struct task_struct *p, struct rq *rq)
{
	if (!static_branch_unlikely(&sched_asym_cpucapacity))
		return;

	if (!p || p->nr_cpus_allowed == 1) {
		rq->misfit_task_load = 0;
		return;
	}

	if (task_fits_capacity(p, capacity_of(cpu_of(rq)))) {
		rq->misfit_task_load = 0;
		return;
	}

	/*
	 * Make sure that misfit_task_load will not be null even if
	 * task_h_load() returns 0.
	 */
	rq->misfit_task_load = max_t(unsigned long, task_h_load(p), 1);
}

#else /* CONFIG_SMP */

static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
	return true;
}

#define UPDATE_TG	0x0
#define SKIP_AGE_LOAD	0x0
#define DO_ATTACH	0x0

static inline void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int not_used1)
{
	cfs_rq_util_change(cfs_rq, 0);
}

static inline void remove_entity_load_avg(struct sched_entity *se) {}

static inline void
attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}
static inline void
detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}

static inline int newidle_balance(struct rq *rq, struct rq_flags *rf)
{
	return 0;
}

static inline void
util_est_enqueue(struct cfs_rq *cfs_rq, struct task_struct *p) {}

static inline void
util_est_dequeue(struct cfs_rq *cfs_rq, struct task_struct *p) {}

static inline void
util_est_update(struct cfs_rq *cfs_rq, struct task_struct *p,
		bool task_sleep) {}
static inline void update_misfit_status(struct task_struct *p, struct rq *rq) {}

#endif /* CONFIG_SMP */

/*
 * IAMROOT, 2023.01.28:
 * - debug.
 */
static void check_spread(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
#ifdef CONFIG_SCHED_DEBUG
	s64 d = se->vruntime - cfs_rq->min_vruntime;

	if (d < 0)
		d = -d;

	if (d > 3*sysctl_sched_latency)
		schedstat_inc(cfs_rq->nr_spread_over);
#endif
}

static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int initial)
{
	u64 vruntime = cfs_rq->min_vruntime;

	/*
	 * The 'current' period is already promised to the current tasks,
	 * however the extra weight of the new task will slow them down a
	 * little, place the new task so that it fits in the slot that
	 * stays open at the end.
	 */
	if (initial && sched_feat(START_DEBIT))
		vruntime += sched_vslice(cfs_rq, se);

	/* sleeps up to a single latency don't count. */
	if (!initial) {
		unsigned long thresh = sysctl_sched_latency;

		/*
		 * Halve their sleep time's effect, to allow
		 * for a gentler effect of sleepers:
		 */
		if (sched_feat(GENTLE_FAIR_SLEEPERS))
			thresh >>= 1;

		vruntime -= thresh;
	}

	/* ensure we never gain time by being placed backwards. */
	se->vruntime = max_vruntime(se->vruntime, vruntime);
}

static void check_enqueue_throttle(struct cfs_rq *cfs_rq);

static inline void check_schedstat_required(void)
{
#ifdef CONFIG_SCHEDSTATS
	if (schedstat_enabled())
		return;

	/* Force schedstat enabled if a dependent tracepoint is active */
	if (trace_sched_stat_wait_enabled()    ||
			trace_sched_stat_sleep_enabled()   ||
			trace_sched_stat_iowait_enabled()  ||
			trace_sched_stat_blocked_enabled() ||
			trace_sched_stat_runtime_enabled())  {
		printk_deferred_once("Scheduler tracepoints stat_sleep, stat_iowait, "
			     "stat_blocked and stat_runtime require the "
			     "kernel parameter schedstats=enable or "
			     "kernel.sched_schedstats=1\n");
	}
#endif
}

static inline bool cfs_bandwidth_used(void);

/*
 * MIGRATION
 *
 *	dequeue
 *	  update_curr()
 *	    update_min_vruntime()
 *	  vruntime -= min_vruntime
 *
 *	enqueue
 *	  update_curr()
 *	    update_min_vruntime()
 *	  vruntime += min_vruntime
 *
 * this way the vruntime transition between RQs is done when both
 * min_vruntime are up-to-date.
 *
 * WAKEUP (remote)
 *
 *	->migrate_task_rq_fair() (p->state == TASK_WAKING)
 *	  vruntime -= min_vruntime
 *
 *	enqueue
 *	  update_curr()
 *	    update_min_vruntime()
 *	  vruntime += min_vruntime
 *
 * this way we don't have the most up-to-date min_vruntime on the originating
 * CPU and an up-to-date min_vruntime on the destination CPU.
 */

/*
 * IAMROOT, 2022.12.27:
 * - TODO
 */
static void
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	bool renorm = !(flags & ENQUEUE_WAKEUP) || (flags & ENQUEUE_MIGRATED);
	bool curr = cfs_rq->curr == se;

	/*
	 * If we're the current task, we must renormalise before calling
	 * update_curr().
	 */
	if (renorm && curr)
		se->vruntime += cfs_rq->min_vruntime;

	update_curr(cfs_rq);

	/*
	 * Otherwise, renormalise after, such that we're placed at the current
	 * moment in time, instead of some random moment in the past. Being
	 * placed in the past could significantly boost this task to the
	 * fairness detriment of existing tasks.
	 */
	if (renorm && !curr)
		se->vruntime += cfs_rq->min_vruntime;

	/*
	 * When enqueuing a sched_entity, we must:
	 *   - Update loads to have both entity and cfs_rq synced with now.
	 *   - Add its load to cfs_rq->runnable_avg
	 *   - For group_entity, update its weight to reflect the new share of
	 *     its group cfs_rq
	 *   - Add its new weight to cfs_rq->load.weight
	 */
	update_load_avg(cfs_rq, se, UPDATE_TG | DO_ATTACH);
	se_update_runnable(se);
	update_cfs_group(se);
	account_entity_enqueue(cfs_rq, se);

	if (flags & ENQUEUE_WAKEUP)
		place_entity(cfs_rq, se, 0);

	check_schedstat_required();
	update_stats_enqueue(cfs_rq, se, flags);
	check_spread(cfs_rq, se);
	if (!curr)
		__enqueue_entity(cfs_rq, se);
	se->on_rq = 1;

	/*
	 * When bandwidth control is enabled, cfs might have been removed
	 * because of a parent been throttled but cfs->nr_running > 1. Try to
	 * add it unconditionally.
	 */
	if (cfs_rq->nr_running == 1 || cfs_bandwidth_used())
		list_add_leaf_cfs_rq(cfs_rq);

	if (cfs_rq->nr_running == 1)
		check_enqueue_throttle(cfs_rq);
}

static void __clear_buddies_last(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->last != se)
			break;

		cfs_rq->last = NULL;
	}
}

static void __clear_buddies_next(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->next != se)
			break;

		cfs_rq->next = NULL;
	}
}

static void __clear_buddies_skip(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->skip != se)
			break;

		cfs_rq->skip = NULL;
	}
}

/*
 * IAMROOT, 2023.01.14:
 * - @se에 대한 buddy 설정이 있으면 삭제한다.
 */
static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (cfs_rq->last == se)
		__clear_buddies_last(se);

	if (cfs_rq->next == se)
		__clear_buddies_next(se);

	if (cfs_rq->skip == se)
		__clear_buddies_skip(se);
}

static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq);

static void
dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	/*
	 * Update run-time statistics of the 'current'.
	 */
	update_curr(cfs_rq);

	/*
	 * When dequeuing a sched_entity, we must:
	 *   - Update loads to have both entity and cfs_rq synced with now.
	 *   - Subtract its load from the cfs_rq->runnable_avg.
	 *   - Subtract its previous weight from cfs_rq->load.weight.
	 *   - For group entity, update its weight to reflect the new share
	 *     of its group cfs_rq.
	 */
	update_load_avg(cfs_rq, se, UPDATE_TG);
	se_update_runnable(se);

	update_stats_dequeue(cfs_rq, se, flags);

	clear_buddies(cfs_rq, se);

	if (se != cfs_rq->curr)
		__dequeue_entity(cfs_rq, se);
	se->on_rq = 0;
	account_entity_dequeue(cfs_rq, se);

	/*
	 * Normalize after update_curr(); which will also have moved
	 * min_vruntime if @se is the one holding it back. But before doing
	 * update_min_vruntime() again, which will discount @se's position and
	 * can move min_vruntime forward still more.
	 */
	if (!(flags & DEQUEUE_SLEEP))
		se->vruntime -= cfs_rq->min_vruntime;

	/* return excess runtime on last dequeue */
	return_cfs_rq_runtime(cfs_rq);

	update_cfs_group(se);

	/*
	 * Now advance min_vruntime if @se was the entity holding it back,
	 * except when: DEQUEUE_SAVE && !DEQUEUE_MOVE, in this case we'll be
	 * put back on, and if we advance min_vruntime, we'll be placed back
	 * further than we started -- ie. we'll be penalized.
	 */
	if ((flags & (DEQUEUE_SAVE | DEQUEUE_MOVE)) != DEQUEUE_SAVE)
		update_min_vruntime(cfs_rq);
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
/*
 * IAMROOT, 2023.01.07:
 * - @resched_curr 요청을 할지 결정한다.
 *   1. 실행시간을 다 소모했으면 rechedule 요청을 하고 @curr 가 buddy 설정된경우
 *   buddy 설정 clear
 */
static void
check_preempt_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	unsigned long ideal_runtime, delta_exec;
	struct sched_entity *se;
	s64 delta;

/*
 * IAMROOT, 2023.01.07:
 * - @curr runtime을 종료된것을 검사한다. 다 사용됬으면
 *   @cfs_rq의 cpu한테 reschedule 요청을 한다.
 */

	ideal_runtime = sched_slice(cfs_rq, curr);
	/*
	 * IAMROOT, 2023.01.14:
	 * - prev_sum_exec_runtime: set_next_entity 에서 entity 전환시 저장
	 */
	delta_exec = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
	if (delta_exec > ideal_runtime) {
		resched_curr(rq_of(cfs_rq));
		/*
		 * The current task ran long enough, ensure it doesn't get
		 * re-elected due to buddy favours.
		 */
		/*
		 * IAMROOT, 2023.01.14:
		 * - 현재 작업이 충분히 실행되었으므로 재선택 되지 않도록 buddies
		 *   관련 설정을 지운다.
		 */
		clear_buddies(cfs_rq, curr);
		return;
	}

	/*
	 * Ensure that a task that missed wakeup preemption by a
	 * narrow margin doesn't have to wait for a full slice.
	 * This also mitigates buddy induced latencies under load.
	 */
	/*
	 * IAMROOT. 2023.01.14:
	 * - google-translate
	 *   근소한 차이로 웨이크업 선점을 놓친 작업이 전체 슬라이스를 기다릴 필요가 없도록
	 *   합니다. 이것은 또한 부하 상태에서 버디 유도 대기 시간을 완화합니다.
	 */
/*
 * IAMROOT, 2023.01.07:
 * - config적으로 제한값을 검사한다.
 */
	if (delta_exec < sysctl_sched_min_granularity)
		return;

	se = __pick_first_entity(cfs_rq);
	delta = curr->vruntime - se->vruntime;

	if (delta < 0)
		return;

	/*
	 * IAMROOT, 2023.01.14:
	 * - 특수한 케이스로 rbtree에 새로 들어온 entity가 현재 실행중인 entity에
	 *   배정된 실행시간보다도 더 이전인 경우 rechedule 요청을 한다.
	 */
	if (delta > ideal_runtime)
		resched_curr(rq_of(cfs_rq));
}

/*
 * IAMROOT, 2023.01.28:
 * - 1. @se에 대한 buddy정보를 다 지운다.
 *   2. on_rq일시 rbtree에서 @se node 제거
 *   3. load avg재계산.
 *   4. @se를 curr로 선택한다.
 *   5. stats 및 debug처리.
 *   6. prev_sum_exec_runtime update.
 */
static void
set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
/*
 * IAMROOT, 2023.01.28:
 * - @se에 대한 정보를 다 지운다.
 */
	clear_buddies(cfs_rq, se);

	/* 'current' is not kept within the tree. */
	if (se->on_rq) {
		/*
		 * Any task has to be enqueued before it get to execute on
		 * a CPU. So account for the time it spent waiting on the
		 * runqueue.
		 */
		update_stats_wait_end(cfs_rq, se);
		__dequeue_entity(cfs_rq, se);
		update_load_avg(cfs_rq, se, UPDATE_TG);
	}

	update_stats_curr_start(cfs_rq, se);
	cfs_rq->curr = se;

	/*
	 * Track our maximum slice length, if the CPU's load is at
	 * least twice that of our own weight (i.e. dont track it
	 * when there are only lesser-weight tasks around):
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   CPU 부하가 자체 무게의 두 배 이상인 경우 최대 슬라이스 길이를 추적합니다(즉, 주변에 더
 *   가벼운 작업만 있는 경우 추적하지 않음).
 */
	if (schedstat_enabled() &&
	    rq_of(cfs_rq)->cfs.load.weight >= 2*se->load.weight) {
		schedstat_set(se->statistics.slice_max,
			max((u64)schedstat_val(se->statistics.slice_max),
			    se->sum_exec_runtime - se->prev_sum_exec_runtime));
	}

	se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se);

/*
 * Pick the next process, keeping these things in mind, in this order:
 * 1) keep things fair between processes/task groups
 * 2) pick the "next" process, since someone really wants that to run
 * 3) pick the "last" process, for cache locality
 * 4) do not run the "skip" process, if something else is available
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   이러한 사항을 염두에 두고 다음 순서로 다음 프로세스를 선택합니다.
 *
 *   1) 프로세스/작업 그룹 간에 일을 공정하게 유지
 *   2) 누군가가 실제로 실행하기를 원하기 때문에 다음 프로세스를 선택하십시오.
 *   3) 캐시 지역성을 위해 마지막 프로세스를 선택합니다.
 *   4) 다른 것을 사용할 수 있는 경우 건너뛰기 프로세스를 실행하지 마십시오.
 *
 * - @curr의 next를 고른다.
 *   1. curr의 left인것을 찾는다.
 *   2. next -> last -> skip고려 순으로 se를 선택한다.
 *   3. 3개가 전부 없으면 left가 선택될것이다.
 */
static struct sched_entity *
pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	struct sched_entity *left = __pick_first_entity(cfs_rq);
	struct sched_entity *se;

	/*
	 * If curr is set we have to see if its left of the leftmost entity
	 * still in the tree, provided there was anything in the tree at all.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   curr이 설정되어 있으면 트리에 아무 것도 없는 경우 가장 왼쪽 엔터티의 왼쪽이 여전히
 *   트리에 있는지 확인해야 합니다.
 *
 * - curr가 left보다 왼쪽인지 확인한다. 더 왼쪽이면 left = curr.
 */
	if (!left || (curr && entity_before(curr, left)))
		left = curr;

	se = left; /* ideally we run the leftmost entity */

	/*
	 * Avoid running the skip buddy, if running something else can
	 * be done without getting too unfair.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   너무 불공평하지 않고 다른 것을 실행할 수 있다면 스킵 버디를 실행하지 마십시오.
 * - skip 지정이 되있고, se가 skip에 해당된다면 차선책을 찾는다.
 *   curr보다 이전인것을 고르려고 노력한다.
 */
	if (cfs_rq->skip && cfs_rq->skip == se) {
		struct sched_entity *second;

/*
 * IAMROOT, 2023.01.28:
 * - se(skip)이 curr면은 가장작은것을 찾아 second로 고른다.
 *   se이 curr가 아니면, se의 next를 고른다.
 */
		if (se == curr) {
			second = __pick_first_entity(cfs_rq);
		} else {
			second = __pick_next_entity(se);

/*
 * IAMROOT, 2023.01.28:
 * - next를 골라봤는데도 불구하고, next가 없거나, curr가 next의 이전이라면
 *   curr를 second를 사용한다.
 */
			if (!second || (curr && entity_before(curr, second)))
				second = curr;
		}

/*
 * IAMROOT, 2023.01.28:
 * - second가 wakeup_gran보다 너무 앞서있는 경우만 아니면 se를 second로 선택한다.
 *   최대한 이전을 골라봤는데도 앞서있을수있는데, wakeup_gran만큼은 허용한다는뜻이다.
 */
		if (second && wakeup_preempt_entity(second, left) < 1)
			se = second;
	}

/*
 * IAMROOT, 2023.01.28:
 * - next를 가장 높은 우선순위로 고려한다.
 * - next가 left 보다 너무 앞서있는경우가 아니면 se를 next로 선택한다.
 */
	if (cfs_rq->next && wakeup_preempt_entity(cfs_rq->next, left) < 1) {
		/*
		 * Someone really wants this to run. If it's not unfair, run it.
		 */
		se = cfs_rq->next;
/*
 * IAMROOT, 2023.01.28:
 * - last도 next처럼 고려한다.
 */
	} else if (cfs_rq->last && wakeup_preempt_entity(cfs_rq->last, left) < 1) {
		/*
		 * Prefer last buddy, try to return the CPU to a preempted task.
		 */
		se = cfs_rq->last;
	}

	return se;
}

static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq);

/*
 * IAMROOT, 2023.01.28:
 * - 1. @prev가 cfs_rq에 있으면 update_curr()
 *   2. cfs_bandwidth처리
 *   3. debug및 통계처리
 *   4. cfs_rq에 enqueue.
 *   5. load avg 재계산.
 *   6. curr를 NULL로 update.
 */
static void put_prev_entity(struct cfs_rq *cfs_rq, struct sched_entity *prev)
{
	/*
	 * If still on the runqueue then deactivate_task()
	 * was not called and update_curr() has to be done:
	 */
/*
 * IAMROOT, 2023.01.28:
 * - rq에 들어가있엇으면 시간갱신을 해준다.
 */
	if (prev->on_rq)
		update_curr(cfs_rq);

	/* throttle cfs_rqs exceeding runtime */
	check_cfs_rq_runtime(cfs_rq);

	check_spread(cfs_rq, prev);

	if (prev->on_rq) {
		update_stats_wait_start(cfs_rq, prev);
		/* Put 'current' back into the tree. */
		__enqueue_entity(cfs_rq, prev);
		/* in !on_rq case, update occurred at dequeue */
		update_load_avg(cfs_rq, prev, 0);
	}
	cfs_rq->curr = NULL;
}

/*
 * IAMROOT, 2022.12.21:
 * - @curr
 *   ex) A:33%, B:33%, C:34%
 *   A가 33%에 대한 분배 시간을 다 마치고 이함수에 진입했을때 curr는 B가 된다.
 * - queued
 *   scheduler_tick에서 task_tick() callback을 통해 task_tick_fair()로 호출됫을때
 *   queued == 0
 */
static void
entity_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr, int queued)
{
	/*
	 * Update run-time statistics of the 'current'.
	 */

/*
 * IAMROOT, 2023.01.07:
 * - runtime관련 처리.(vruntime, runtime, cfs bandwidth등)
 */
	update_curr(cfs_rq);

	/*
	 * Ensure that runnable average is periodically updated.
	 */
/*
 * IAMROOT, 2023.01.07:
 * - @cfs_rq, @curr에 대한 load관련 처리(avg, sum, 전파, attach등)
 */
	update_load_avg(cfs_rq, curr, UPDATE_TG);

/*
 * IAMROOT, 2023.01.07:
 * - @curr의 weight를 재갱신한다.
 */
	update_cfs_group(curr);

#ifdef CONFIG_SCHED_HRTICK
	/*
	 * queued ticks are scheduled to match the slice, so don't bother
	 * validating it and just reschedule.
	 */

/*
 * IAMROOT, 2023.01.07:
 * - 일반 schedule tick에서는 queued가 0이므로 reschedule을 안한다.
 */
	if (queued) {
/*
 * IAMROOT, 2023.01.07:
 * - cfs_rq 의 cpu에 reschedule 요청을 한다.
 */
		resched_curr(rq_of(cfs_rq));
		return;
	}
	/*
	 * don't let the period tick interfere with the hrtick preemption
	 */

/*
 * IAMROOT, 2023.01.07:
 * - DOUBLE_TICK
 *   1000hz + hrtick 둘다 사용한다는것.
 *
 * - double tick을 사용안하고 있는 상황에서
 *   일반 schedule tick으로 들어온 경우엔 hrtick_timer가 active인 상황이므로
 *   여기서 return.
 */
	if (!sched_feat(DOUBLE_TICK) &&
			hrtimer_active(&rq_of(cfs_rq)->hrtick_timer))
		return;
#endif

/*
 * IAMROOT, 2023.01.07:
 * - double tick을 사용하는 경우 일반 schedule tick에서도 진입한다.
 * - double tick을 사용하지 않은경우 여기에 진입하는 경우는 hrtimer가
 *   이 함수를 호출한 상황이다.(이 경우 hrtimer_active()는 false가 되므로.)
 * - task가 2개이상 돌고있으면 진입한다.
 * - @cfs_rq에 reschedule을 요청할지 결정한다.
 */
	if (cfs_rq->nr_running > 1)
		check_preempt_tick(cfs_rq, curr);
}


/**************************************************
 * CFS bandwidth control machinery
 */

#ifdef CONFIG_CFS_BANDWIDTH

#ifdef CONFIG_JUMP_LABEL
static struct static_key __cfs_bandwidth_used;

static inline bool cfs_bandwidth_used(void)
{
	return static_key_false(&__cfs_bandwidth_used);
}

void cfs_bandwidth_usage_inc(void)
{
	static_key_slow_inc_cpuslocked(&__cfs_bandwidth_used);
}

void cfs_bandwidth_usage_dec(void)
{
	static_key_slow_dec_cpuslocked(&__cfs_bandwidth_used);
}
#else /* CONFIG_JUMP_LABEL */
static bool cfs_bandwidth_used(void)
{
	return true;
}

void cfs_bandwidth_usage_inc(void) {}
void cfs_bandwidth_usage_dec(void) {}
#endif /* CONFIG_JUMP_LABEL */

/*
 * default period for cfs group bandwidth.
 * default: 0.1s, units: nanoseconds
 */
/*
 * IAMROOT, 2022.11.26:
 * - ns단위 return. 0.1
 */
static inline u64 default_cfs_period(void)
{
	return 100000000ULL;
}

/*
 * IAMROOT, 2022.12.22:
 * - nsec 단위로 return. 기본값 5ms
 */
static inline u64 sched_cfs_bandwidth_slice(void)
{
	return (u64)sysctl_sched_cfs_bandwidth_slice * NSEC_PER_USEC;
}

/*
 * Replenish runtime according to assigned quota. We use sched_clock_cpu
 * directly instead of rq->clock to avoid adding additional synchronization
 * around rq->lock.
 *
 * requires cfs_b->lock
 */
/*
 * IAMROOT, 2022.12.23:
 * - papago
 *   할당량에 따라 런타임을 보충합니다. rq->lock 주변에 추가 동기화를 추가하지
 *   않도록 rq->clock 대신 sched_clock_cpu를 직접 사용합니다.
 *
 *   cfs_b->잠금이 필요합니다.
 *
 * - 한번에 qouta만큼 추가한다.
 * - cfs_b->quota + cfs_b->burst이상을 초과시키진 않는다.
 */
void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b)
{
	if (unlikely(cfs_b->quota == RUNTIME_INF))
		return;

	cfs_b->runtime += cfs_b->quota;
	cfs_b->runtime = min(cfs_b->runtime, cfs_b->quota + cfs_b->burst);
}

static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return &tg->cfs_bandwidth;
}

/* returns 0 on failure to allocate runtime */
/*
 * IAMROOT, 2022.12.22:
 * - @target_runtime 기본값 5ms.
 * - bandwidth runtime에서 최대 runtime_remain이 @target_runtime이 될때까지
 *   가져온다.
 *
 *  ex) target_runtime == 5ms
 *
 *  1) quota == RUNTIME_INF. bandwidth 사용안하는 경우
 *  cfs_b | runtime_  | cfs_b   || timer | runtime_  |
 *  qouta | remaining | runtime || 예약  | remaining | runtime
 *  +-----+-----------+---------++-------+-----------+--------
 *  | ~0  | -         | -       || X     | 5ms       | 유지
 *
 *  2) runtime_remain 이 target_runtime이 될때까지 불충분한 경우
 *     남아있는 runtime만큼 runtime_remain이 충전된다.
 *
 *  | runtime_  | cfs_b   || timer | runtime_  |
 *  | remaining | runtime || 예약  | remaining | runtime
 *  +-----------+---------++-------+-----------+--------
 *  | 0ms       | 0ms     || O     | 0ms       | 0ms
 *  | 0ms       | 1ms     || O     | 1ms       | 0ms
 *  | 1ms       | 0ms     || O     | 1ms       | 0ms
 *  | 2ms       | 1ms     || O     | 3ms       | 0ms
 *  | 1ms       | 2ms     || O     | 3ms       | 0ms
 *  | -1ms      | 0ms     || O     | -1ms      | 0ms
 *  | -1ms      | 1ms     || O     | 0ms       | 0ms
 *  | -1ms      | 2ms     || O     | 1ms       | 0ms
 *
 *  3) runtime_remain이 target_runtime이 될때까지 충분한 경우
 *
 *  | runtime_  | cfs_b   || timer | runtime_  |
 *  | remaining | runtime || 예약  | remaining | runtime
 *  +-----------+---------++-------+-----------+--------
 *  | 0ms       | 5ms     || O     | 5ms       | 0ms
 *  | 5ms       | 0ms     || O     | 5ms       | 0ms
 *  | 2ms       | 3ms     || O     | 5ms       | 0ms
 *  | 2ms       | 3ms     || O     | 5ms       | 0ms
 *  | -1ms      | 6ms     || O     | 5ms       | 0ms
 *  | 5ms       | 1ms     || O     | 5ms       | 1ms
 *  | 10ms      | 1ms     || O     | 5ms       | 6ms
 */
static int __assign_cfs_rq_runtime(struct cfs_bandwidth *cfs_b,
				   struct cfs_rq *cfs_rq, u64 target_runtime)
{
	u64 min_amount, amount = 0;

	lockdep_assert_held(&cfs_b->lock);

	/* note: this is a positive sum as runtime_remaining <= 0 */
/*
 * IAMROOT, 2022.12.22:
 * - runtime_remaining만큼을 5ms에서 빼준다.
 *   runtime이 아무리 많아도 최고 target_runtime만큼 증가하게 하는 장치가 되는 동시에
 *   -를 runtime에서 보정하는 양의 역할을 수행한다.
 *   ex) runtime_remain = -10ms, target_runtime = 5ms, runtime = 100ms
 *     min_amount = 15 => amount = 15 => new runtime_remain = -10 + 15 = 5ms
 */
	min_amount = target_runtime - cfs_rq->runtime_remaining;

/*
 * IAMROOT, 2022.12.22:
 * - quota가 RUNTIME_INF면 이 함수진입상황에서는 period_timer 추적을 안한다
 *   는것. cfs_rq->runtime_remaining은 그대로 유지.
 */
	if (cfs_b->quota == RUNTIME_INF)
		amount = min_amount;
	else {
/*
 * IAMROOT, 2022.12.22:
 * - 5ms후 period_timer 시작 예약.
 */
		start_cfs_bandwidth(cfs_b);

/*
 * IAMROOT, 2022.12.22:
 * - runtime 이미 남아있다면 amount만큼 빼준다. 그리고 뺀만큼을
 *   runtime_remaining에 추가한다.
 */
		if (cfs_b->runtime > 0) {
			amount = min(cfs_b->runtime, min_amount);
			cfs_b->runtime -= amount;
			cfs_b->idle = 0;
		}
	}

/*
 * IAMROOT, 2022.12.22:
 * - quota == RUNTIME_INF인 경우 유지
 * - runtime에서 차출이 됬다면 차출된 양이 amount가 된다.
 */
	cfs_rq->runtime_remaining += amount;

	return cfs_rq->runtime_remaining > 0;
}

/* returns 0 on failure to allocate runtime */
/*
 * IAMROOT, 2022.12.22:
 * - runtime_remaining을 얻어온경우(0이 아닌 경우) return 1.
 *   period_timer가 실행됫을수도 있다.
 */
static int assign_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	int ret;

	raw_spin_lock(&cfs_b->lock);
	ret = __assign_cfs_rq_runtime(cfs_b, cfs_rq, sched_cfs_bandwidth_slice());
	raw_spin_unlock(&cfs_b->lock);

	return ret;
}

/*
 * IAMROOT, 2022.12.22:
 * - runtime_remain을 delta_exec로 감산한다.
 *   runtime_remain이 안남앗다면 얻어오는길 시도한다.
 *   얻어오는걸 실패했는데, curr가 있다면(실행중이라는 뜻) resched_curr를
 *   요청한다.
 * - runtime_remain을 얻어오면서 period_timer가 실행될수있다.
 */
static void __account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	/* dock delta_exec before expiring quota (as it could span periods) */
/*
 * IAMROOT, 2022.12.22:
 * - delta_exec만큼 실행이 됬으니 runtime_remaining에서 그만큼 빼준다.
 */
	cfs_rq->runtime_remaining -= delta_exec;

	if (likely(cfs_rq->runtime_remaining > 0))
		return;

	if (cfs_rq->throttled)
		return;
/*
 * IAMROOT, 2022.12.22:
 * - likely(cfs_rq->curr)
 *   현재 실행중
 * - assign_cfs_rq_runtime(cfs_rq)
 *   runtime_remain을 얻어오고 period_timer 예약
 * - runtime_remain을 얻어오는데 실패했는데 실행중이면 resched_curr 요청.
 */
	/*
	 * if we're unable to extend our runtime we resched so that the active
	 * hierarchy can be throttled
	 */
	if (!assign_cfs_rq_runtime(cfs_rq) && likely(cfs_rq->curr))
		resched_curr(rq_of(cfs_rq));
}

/*
 * IAMROOT, 2022.12.22:
 * - cfs bandwidth 처리를 수행한다.
 * - runtime_remain을 delta_exec로 감산한다.
 *   runtime_remain이 안남앗다면 얻어오는길 시도한다.
 *   얻어오는걸 실패했는데, curr가 있다면(실행중이라는 뜻) resched_curr를
 *   요청한다.
 * - runtime_remain을 얻어오면서 period_timer가 실행될수있다.
 */
static __always_inline
void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	if (!cfs_bandwidth_used() || !cfs_rq->runtime_enabled)
		return;

	__account_cfs_rq_runtime(cfs_rq, delta_exec);
}

static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttled;
}

/* check whether cfs_rq, or any parent, is throttled */
/*
 * IAMROOT, 2023.01.07:
 * - cfs_bandwidth를 사용하는 상태. 즉 throttled 관련 기능
 *   @cfs_rq가 throttle중 인지 확인한다.
 */
static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttle_count;
}

/*
 * Ensure that neither of the group entities corresponding to src_cpu or
 * dest_cpu are members of a throttled hierarchy when performing group
 * load-balance operations.
 */
/*
 * IAMROOT. 2023.05.13:
 * - google-translate
 * 그룹 부하 분산 작업을 수행할 때 src_cpu 또는 dest_cpu에 해당하는 그룹 엔터티 중
 * 어느 것도 제한 계층의 구성원이 아닌지 확인합니다.
 */
static inline int throttled_lb_pair(struct task_group *tg,
				    int src_cpu, int dest_cpu)
{
	struct cfs_rq *src_cfs_rq, *dest_cfs_rq;

	src_cfs_rq = tg->cfs_rq[src_cpu];
	dest_cfs_rq = tg->cfs_rq[dest_cpu];

	return throttled_hierarchy(src_cfs_rq) ||
	       throttled_hierarchy(dest_cfs_rq);
}

/*
 * IAMROOT, 2022.12.27:
 * - throttle count 값을 감소시킨다. 0이 됬다면 throttled 된 시간을 누적한다.
 *   또한 이때 cfs rq가 decayed가 아니거나 nr_running개수가 존재한다면
 *   list_add_leaf_cfs_rq를 수행한다.
 */
static int tg_unthrottle_up(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

	cfs_rq->throttle_count--;
	if (!cfs_rq->throttle_count) {
		cfs_rq->throttled_clock_task_time += rq_clock_task(rq) -
					     cfs_rq->throttled_clock_task;

		/* Add cfs_rq with load or one or more already running entities to the list */
		if (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running)
			list_add_leaf_cfs_rq(cfs_rq);
	}

	return 0;
}

static int tg_throttle_down(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

	/* group is entering throttled state, stop time */
	if (!cfs_rq->throttle_count) {
		cfs_rq->throttled_clock_task = rq_clock_task(rq);
		list_del_leaf_cfs_rq(cfs_rq);
	}
	cfs_rq->throttle_count++;

	return 0;
}

static bool throttle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *se;
	long task_delta, idle_task_delta, dequeue = 1;

	raw_spin_lock(&cfs_b->lock);
	/* This will start the period timer if necessary */
	if (__assign_cfs_rq_runtime(cfs_b, cfs_rq, 1)) {
		/*
		 * We have raced with bandwidth becoming available, and if we
		 * actually throttled the timer might not unthrottle us for an
		 * entire period. We additionally needed to make sure that any
		 * subsequent check_cfs_rq_runtime calls agree not to throttle
		 * us, as we may commit to do cfs put_prev+pick_next, so we ask
		 * for 1ns of runtime rather than just check cfs_b.
		 */
		dequeue = 0;
	} else {
		list_add_tail_rcu(&cfs_rq->throttled_list,
				  &cfs_b->throttled_cfs_rq);
	}
	raw_spin_unlock(&cfs_b->lock);

	if (!dequeue)
		return false;  /* Throttle no longer required. */

	se = cfs_rq->tg->se[cpu_of(rq_of(cfs_rq))];

	/* freeze hierarchy runnable averages while throttled */
	rcu_read_lock();
	walk_tg_tree_from(cfs_rq->tg, tg_throttle_down, tg_nop, (void *)rq);
	rcu_read_unlock();

	task_delta = cfs_rq->h_nr_running;
	idle_task_delta = cfs_rq->idle_h_nr_running;
	for_each_sched_entity(se) {
		struct cfs_rq *qcfs_rq = cfs_rq_of(se);
		/* throttled entity or throttle-on-deactivate */
		if (!se->on_rq)
			goto done;

		dequeue_entity(qcfs_rq, se, DEQUEUE_SLEEP);

		if (cfs_rq_is_idle(group_cfs_rq(se)))
			idle_task_delta = cfs_rq->h_nr_running;

		qcfs_rq->h_nr_running -= task_delta;
		qcfs_rq->idle_h_nr_running -= idle_task_delta;

		if (qcfs_rq->load.weight) {
			/* Avoid re-evaluating load for this entity: */
			se = parent_entity(se);
			break;
		}
	}

	for_each_sched_entity(se) {
		struct cfs_rq *qcfs_rq = cfs_rq_of(se);
		/* throttled entity or throttle-on-deactivate */
		if (!se->on_rq)
			goto done;

		update_load_avg(qcfs_rq, se, 0);
		se_update_runnable(se);

		if (cfs_rq_is_idle(group_cfs_rq(se)))
			idle_task_delta = cfs_rq->h_nr_running;

		qcfs_rq->h_nr_running -= task_delta;
		qcfs_rq->idle_h_nr_running -= idle_task_delta;
	}

	/* At this point se is NULL and we are at root level*/
	sub_nr_running(rq, task_delta);

done:
	/*
	 * Note: distribution will already see us throttled via the
	 * throttled-list.  rq->lock protects completion.
	 */
	cfs_rq->throttled = 1;
	cfs_rq->throttled_clock = rq_clock(rq);
	return true;
}

/*
 * IAMROOT, 2022.12.23:
 * - TODO
 */
void unthrottle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *se;
	long task_delta, idle_task_delta;

	se = cfs_rq->tg->se[cpu_of(rq)];

	cfs_rq->throttled = 0;

	update_rq_clock(rq);

	raw_spin_lock(&cfs_b->lock);
/*
 * IAMROOT, 2022.12.23:
 * - throttled 됫던 시간을 기록해놓는다.
 */
	cfs_b->throttled_time += rq_clock(rq) - cfs_rq->throttled_clock;
	list_del_rcu(&cfs_rq->throttled_list);
	raw_spin_unlock(&cfs_b->lock);

/*
 * IAMROOT, 2022.12.27:
 * - cfs->tg를 포함한 하위그룹에 대해 tg_unthrottle_up을 수행한다.
 */
	/* update hierarchical throttle state */
	walk_tg_tree_from(cfs_rq->tg, tg_nop, tg_unthrottle_up, (void *)rq);

	/* Nothing to run but something to decay (on_list)? Complete the branch */
	if (!cfs_rq->load.weight) {
		if (cfs_rq->on_list)
			goto unthrottle_throttle;
		return;
	}

	task_delta = cfs_rq->h_nr_running;
	idle_task_delta = cfs_rq->idle_h_nr_running;
	for_each_sched_entity(se) {
		struct cfs_rq *qcfs_rq = cfs_rq_of(se);

		if (se->on_rq)
			break;
		enqueue_entity(qcfs_rq, se, ENQUEUE_WAKEUP);

		if (cfs_rq_is_idle(group_cfs_rq(se)))
			idle_task_delta = cfs_rq->h_nr_running;

		qcfs_rq->h_nr_running += task_delta;
		qcfs_rq->idle_h_nr_running += idle_task_delta;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(qcfs_rq))
			goto unthrottle_throttle;
	}

	for_each_sched_entity(se) {
		struct cfs_rq *qcfs_rq = cfs_rq_of(se);

		update_load_avg(qcfs_rq, se, UPDATE_TG);
		se_update_runnable(se);

		if (cfs_rq_is_idle(group_cfs_rq(se)))
			idle_task_delta = cfs_rq->h_nr_running;

		qcfs_rq->h_nr_running += task_delta;
		qcfs_rq->idle_h_nr_running += idle_task_delta;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(qcfs_rq))
			goto unthrottle_throttle;

		/*
		 * One parent has been throttled and cfs_rq removed from the
		 * list. Add it back to not break the leaf list.
		 */
		if (throttled_hierarchy(qcfs_rq))
			list_add_leaf_cfs_rq(qcfs_rq);
	}

	/* At this point se is NULL and we are at root level*/
	add_nr_running(rq, task_delta);

unthrottle_throttle:
	/*
	 * The cfs_rq_throttled() breaks in the above iteration can result in
	 * incomplete leaf list maintenance, resulting in triggering the
	 * assertion below.
	 */
	for_each_sched_entity(se) {
		struct cfs_rq *qcfs_rq = cfs_rq_of(se);

		if (list_add_leaf_cfs_rq(qcfs_rq))
			break;
	}

	assert_list_leaf_cfs_rq(rq);

	/* Determine whether we need to wake up potentially idle CPU: */
	if (rq->curr == rq->idle && rq->cfs.nr_running)
		resched_curr(rq);
}

/*
 * IAMROOT, 2022.12.23:
 * - @cfs_b에 소속된 rq들에 runtime을 분배한다. 분배에 성공하면
 *   해당 rq를 unthrottle한다.
 */
static void distribute_cfs_runtime(struct cfs_bandwidth *cfs_b)
{
	struct cfs_rq *cfs_rq;
	u64 runtime, remaining = 1;

	rcu_read_lock();
	list_for_each_entry_rcu(cfs_rq, &cfs_b->throttled_cfs_rq,
				throttled_list) {
		struct rq *rq = rq_of(cfs_rq);
		struct rq_flags rf;

		rq_lock_irqsave(rq, &rf);
/*
 * IAMROOT, 2022.12.23:
 * - bandwidth 사용중인지, throttle이 남아있는지 확인
 */
		if (!cfs_rq_throttled(cfs_rq))
			goto next;

		/* By the above check, this should never be true */
/*
 * IAMROOT, 2022.12.23:
 * - 소속된 throttled rq의 runtime_remain이 <=0 이 아니고선
 *   이 함수에 진입할수 없는 구조이다.
 */
		SCHED_WARN_ON(cfs_rq->runtime_remaining > 0);

		raw_spin_lock(&cfs_b->lock);
/*
 * IAMROOT, 2022.12.23:
 * - rq에서 + 값이 될때까지 필요한 runtime을 bandwidth에서 차감 후
 *   rq에 가져온다.
 */
		runtime = -cfs_rq->runtime_remaining + 1;
		if (runtime > cfs_b->runtime)
			runtime = cfs_b->runtime;
		cfs_b->runtime -= runtime;
		remaining = cfs_b->runtime;
		raw_spin_unlock(&cfs_b->lock);

		cfs_rq->runtime_remaining += runtime;

		/* we check whether we're throttled above */
/*
 * IAMROOT, 2022.12.23:
 * - runtime을 제대로 얻엇더면 unthrottle을 시킨다.
 */
		if (cfs_rq->runtime_remaining > 0)
			unthrottle_cfs_rq(cfs_rq);

next:
		rq_unlock_irqrestore(rq, &rf);
/*
 * IAMROOT, 2022.12.23:
 * - bandwidth runtime이 더이상 없다. break. 후 종료
 */
		if (!remaining)
			break;
	}
	rcu_read_unlock();
}

/*
 * Responsible for refilling a task_group's bandwidth and unthrottling its
 * cfs_rqs as appropriate. If there has been no activity within the last
 * period the timer is deactivated until scheduling resumes; cfs_b->idle is
 * used to track this state.
 */
/*
 * IAMROOT, 2022.12.23:
 * - papago
 *   task_group의 대역폭을 다시 채우고 cfs_rqs를 적절하게 조절 해제하는 일을 담당합니다. 마지막 기간
 *   내에 활동이 없으면 예약이 재개될 때까지 타이머가 비활성화됩니다. cfs_b->idle은 이 상태를 추적하는
 *   데 사용됩니다.
 *
 * @return 0 : timer restart.
 *         1 : timer 종료.
 *
 * 1. @cfs_b runtime refill
 * 2. @cfs_b 소속 rq에 runttime 분배
 * 3. bandwidth idle 제어
 * 4. timter restart 제어
 */
static int do_sched_cfs_period_timer(struct cfs_bandwidth *cfs_b, int overrun, unsigned long flags)
{
	int throttled;

	/* no need to continue the timer with no bandwidth constraint */
	if (cfs_b->quota == RUNTIME_INF)
		goto out_deactivate;

	throttled = !list_empty(&cfs_b->throttled_cfs_rq);
	cfs_b->nr_periods += overrun;

	/* Refill extra burst quota even if cfs_b->idle */
	__refill_cfs_bandwidth_runtime(cfs_b);

	/*
	 * idle depends on !throttled (for the case of a large deficit), and if
	 * we're going inactive then everything else can be deferred
	 */
/*
 * IAMROOT, 2022.12.23:
 * - papago
 *  유휴는 !throttled(적자가 큰 경우)에 따라 달라지며 비활성 상태가 되면 다른
 *  모든 작업을 연기할 수 있습니다.
 *
 * - 이미 idle이 되어있는데 throttled rq도 없는 상황. timer 종료
 */
	if (cfs_b->idle && !throttled)
		goto out_deactivate;

/*
 * IAMROOT, 2022.12.23:
 * - throttled_cfs_rq에 아무것도 없다면 bandwidth를 idle시키고 return 0.
    일반 이번에 idle만들고 restart한다. 만약 그 다음 timer에도 들어왓을때
	idle이 안풀려있고 throttled도니가 없으면 바로 위 code에서 out_deactivate
	로 계속 빠지며 timer restart는 안할것이다.
 */
	if (!throttled) {
		/* mark as potentially idle for the upcoming period */
		cfs_b->idle = 1;
		return 0;
	}

	/* account preceding periods in which throttling occurred */
	cfs_b->nr_throttled += overrun;

	/*
	 * This check is repeated as we release cfs_b->lock while we unthrottle.
	 */
/*
 * IAMROOT, 2022.12.23:
 * - runtime 이 남아있고, throttled가 있다면 계속 distribute_cfs_runtime 수행
 */
	while (throttled && cfs_b->runtime > 0) {
		raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
		/* we can't nest cfs_b->lock while distributing bandwidth */
		distribute_cfs_runtime(cfs_b);
		raw_spin_lock_irqsave(&cfs_b->lock, flags);

		throttled = !list_empty(&cfs_b->throttled_cfs_rq);
	}

	/*
	 * While we are ensured activity in the period following an
	 * unthrottle, this also covers the case in which the new bandwidth is
	 * insufficient to cover the existing bandwidth deficit.  (Forcing the
	 * timer to remain active while there are any throttled entities.)
	 */
/*
 * IAMROOT, 2022.12.27:
 * - papago
 *   스로틀 해제 후 기간 동안 활동이 보장되지만 새 대역폭이 기존 대역폭
 *   부족을 충당하기에 불충분한 경우도 포함됩니다. (조절된 entity가 있는
 *   동안 타이머가 활성 상태를 유지하도록 합니다.).
 */
	cfs_b->idle = 0;

	return 0;

out_deactivate:
	return 1;
}

/* a cfs_rq won't donate quota below this amount */
static const u64 min_cfs_rq_runtime = 1 * NSEC_PER_MSEC;
/* minimum remaining period time to redistribute slack quota */
static const u64 min_bandwidth_expiration = 2 * NSEC_PER_MSEC;
/* how long we wait to gather additional slack before distributing */
static const u64 cfs_bandwidth_slack_period = 5 * NSEC_PER_MSEC;

/*
 * Are we near the end of the current quota period?
 *
 * Requires cfs_b->lock for hrtimer_expires_remaining to be safe against the
 * hrtimer base being cleared by hrtimer_start. In the case of
 * migrate_hrtimers, base is never cleared, so we are fine.
 */
static int runtime_refresh_within(struct cfs_bandwidth *cfs_b, u64 min_expire)
{
	struct hrtimer *refresh_timer = &cfs_b->period_timer;
	s64 remaining;

	/* if the call-back is running a quota refresh is already occurring */
	if (hrtimer_callback_running(refresh_timer))
		return 1;

	/* is a quota refresh about to occur? */
	remaining = ktime_to_ns(hrtimer_expires_remaining(refresh_timer));
	if (remaining < (s64)min_expire)
		return 1;

	return 0;
}

static void start_cfs_slack_bandwidth(struct cfs_bandwidth *cfs_b)
{
	u64 min_left = cfs_bandwidth_slack_period + min_bandwidth_expiration;

	/* if there's a quota refresh soon don't bother with slack */
	if (runtime_refresh_within(cfs_b, min_left))
		return;

	/* don't push forwards an existing deferred unthrottle */
	if (cfs_b->slack_started)
		return;
	cfs_b->slack_started = true;

	hrtimer_start(&cfs_b->slack_timer,
			ns_to_ktime(cfs_bandwidth_slack_period),
			HRTIMER_MODE_REL);
}

/* we know any runtime found here is valid as update_curr() precedes return */
static void __return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	s64 slack_runtime = cfs_rq->runtime_remaining - min_cfs_rq_runtime;

	if (slack_runtime <= 0)
		return;

	raw_spin_lock(&cfs_b->lock);
	if (cfs_b->quota != RUNTIME_INF) {
		cfs_b->runtime += slack_runtime;

		/* we are under rq->lock, defer unthrottling using a timer */
		if (cfs_b->runtime > sched_cfs_bandwidth_slice() &&
		    !list_empty(&cfs_b->throttled_cfs_rq))
			start_cfs_slack_bandwidth(cfs_b);
	}
	raw_spin_unlock(&cfs_b->lock);

	/* even if it's not valid for return we don't want to try again */
	cfs_rq->runtime_remaining -= slack_runtime;
}

static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	if (!cfs_rq->runtime_enabled || cfs_rq->nr_running)
		return;

	__return_cfs_rq_runtime(cfs_rq);
}

/*
 * This is done with a timer (instead of inline with bandwidth return) since
 * it's necessary to juggle rq->locks to unthrottle their respective cfs_rqs.
 */
static void do_sched_cfs_slack_timer(struct cfs_bandwidth *cfs_b)
{
	u64 runtime = 0, slice = sched_cfs_bandwidth_slice();
	unsigned long flags;

	/* confirm we're still not at a refresh boundary */
	raw_spin_lock_irqsave(&cfs_b->lock, flags);
	cfs_b->slack_started = false;

	if (runtime_refresh_within(cfs_b, min_bandwidth_expiration)) {
		raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
		return;
	}

	if (cfs_b->quota != RUNTIME_INF && cfs_b->runtime > slice)
		runtime = cfs_b->runtime;

	raw_spin_unlock_irqrestore(&cfs_b->lock, flags);

	if (!runtime)
		return;

	distribute_cfs_runtime(cfs_b);
}

/*
 * When a group wakes up we want to make sure that its quota is not already
 * expired/exceeded, otherwise it may be allowed to steal additional ticks of
 * runtime as update_curr() throttling can not trigger until it's on-rq.
 */
static void check_enqueue_throttle(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	/* an active group must be handled by the update_curr()->put() path */
	if (!cfs_rq->runtime_enabled || cfs_rq->curr)
		return;

	/* ensure the group is not already throttled */
	if (cfs_rq_throttled(cfs_rq))
		return;

	/* update runtime allocation */
	account_cfs_rq_runtime(cfs_rq, 0);
	if (cfs_rq->runtime_remaining <= 0)
		throttle_cfs_rq(cfs_rq);
}

static void sync_throttle(struct task_group *tg, int cpu)
{
	struct cfs_rq *pcfs_rq, *cfs_rq;

	if (!cfs_bandwidth_used())
		return;

	if (!tg->parent)
		return;

	cfs_rq = tg->cfs_rq[cpu];
	pcfs_rq = tg->parent->cfs_rq[cpu];

	cfs_rq->throttle_count = pcfs_rq->throttle_count;
	cfs_rq->throttled_clock_task = rq_clock_task(cpu_rq(cpu));
}

/* conditionally throttle active cfs_rq's from put_prev_entity() */
/*
 * IAMROOT, 2023.01.28:
 * - cfs_bandwidth 미지원이면 return false.
 */
static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return false;

	if (likely(!cfs_rq->runtime_enabled || cfs_rq->runtime_remaining > 0))
		return false;

	/*
	 * it's possible for a throttled entity to be forced into a running
	 * state (e.g. set_curr_task), in this case we're finished.
	 */
	if (cfs_rq_throttled(cfs_rq))
		return true;

	return throttle_cfs_rq(cfs_rq);
}

static enum hrtimer_restart sched_cfs_slack_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, slack_timer);

	do_sched_cfs_slack_timer(cfs_b);

	return HRTIMER_NORESTART;
}

extern const u64 max_cfs_quota_period;

/*
 * IAMROOT, 2022.12.22:
 * - 1. bandwidth rutime refill
 *   2. bandwidth에 소속된 rq runtime에 재분배
 *   3. timer 재가동 여부 판단.
 *   4. timer overrun이 자주 발생하면 timer period 및 runtime refill양등
 *      증가
 */
static enum hrtimer_restart sched_cfs_period_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, period_timer);
	unsigned long flags;
	int overrun;
	int idle = 0;
	int count = 0;

	raw_spin_lock_irqsave(&cfs_b->lock, flags);
/*
 * IAMROOT, 2022.12.22:
 * ex) timer 진입시 orverrun = hrtimer_forward_now(timer, cfs_b->period) 동작 원리
 * 1. 최초 loop 진입
 *   orverrun = hrtimer_forward_now(timer, cfs_b->period) 수행
 *   expire로 인한 timer동작이므로 overrun은 1이상으로 return될것이다.
 *   next expire시간은 cfs_b->period 이후로 설정될것이다.
 * 2. loop 동작후 두번째 loop 진입.
 *   orverrun = hrtimer_forward_now(timer, cfs_b->period) 수행
 *   직전 loop에서 cfs_b->period이후로 expire가 재설정됬고, 이 시간은 충분히 길것이므로
 *   아직 expire가 안된상태. overrun == 0 return. break.
 * 3. loop 종료후
 *   idle == 0 이라면 설정했던 cfs_b->period 이후 시간에 timer가 재수행을 할 것이다. 그게 아니면
 *   timer는 종료된 상태로 유지
 */
	for (;;) {
		overrun = hrtimer_forward_now(timer, cfs_b->period);
		if (!overrun)
			break;

		idle = do_sched_cfs_period_timer(cfs_b, overrun, flags);

/*
 * IAMROOT, 2022.12.23:
 * --- loop가 3번이상 (hrtimer_forward_now()에서 2연속 overrun이 양수 return) 발생하는 경우. ---
 *     (chat open ai)
 * 1. period가 너무 짧게 설정.
 * 2. expire전에 너무 빠른 timer 갱신.
 * 3. cpu 부하, interrupt등의 이유
 */
		if (++count > 3) {
			u64 new, old = ktime_to_ns(cfs_b->period);

			/*
			 * Grow period by a factor of 2 to avoid losing precision.
			 * Precision loss in the quota/period ratio can cause __cfs_schedulable
			 * to fail.
			 */
/*
 * IAMROOT, 2022.12.23:
 * - 너무 자주 발생한다.
 *   1. period를 2배로 늘리는걸 시도 한다.
 *   2. period동안 사용할 수있는 cpu 작업양 2배로 늘린다.
 *   3. period동안 single burst를 수행하는 cpu 수행 양을 늘린다.
 */
			new = old * 2;
			if (new < max_cfs_quota_period) {
				cfs_b->period = ns_to_ktime(new);
				cfs_b->quota *= 2;
				cfs_b->burst *= 2;

				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, scaling up (new cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(new, NSEC_PER_USEC),
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			} else {
				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, but cannot scale up without losing precision (cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(old, NSEC_PER_USEC),
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			}

			/* reset count so we don't come right back in here */
			count = 0;
		}
	}
	if (idle)
		cfs_b->period_active = 0;
	raw_spin_unlock_irqrestore(&cfs_b->lock, flags);

/*
 * IAMROOT, 2022.12.23:
 * - idle == 0 라면 hrtimer_forward에서 설정한 cfs_b->period이후에 다시한번 timer가 동작할것이다.
 */
	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

/*
 * IAMROOT, 2022.11.26:
 * - CONFIG_CFS_BANDWIDTH
 *   Documentation/scheduler/sched-bwc.rst
 * - Kconfig papago
 *   이 옵션을 사용하면 fair 그룹 스케줄러 내에서 실행되는 작업에 대한
 *   CPU 대역폭 속도(제한)를 정의할 수 있습니다. 제한이 설정되지 않은
 *   그룹은 제한이 없는 것으로 간주되며 제한 없이 실행됩니다.
 * - quota
 *   period를 쓰는 정도.
 *   ex) period = 1, quota = -1인 경우 (편의상 ns를 sec로 하여 1로 표현)
 *   cfs process A, B가 있는 경우 각각 50%씩 사용. (합쳐서 100%)
 *
 *   ex) perioid = 1, quota = 0.3인 경우,
 *   cfs process A, B가 있는 경우 각각 16.66%씩 사용. (33%)
 *
 * - shares
 *   group이 A,B,C 3개있고, 각각 shares가 6*1024, 3*1024, 1*1024라고 할때
 *   shares 비율대로 60%, 30%, 10%의 점유율을 가져간다.
 *
 * ex) shares가 60%, period = 1, quota = 0.1인 경우
 *   6%의 점유율을 가져간다.
 * - group 에서 cfs bw 를 초기화 한다.
 */
void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	raw_spin_lock_init(&cfs_b->lock);
	cfs_b->runtime = 0;
	cfs_b->quota = RUNTIME_INF;
	cfs_b->period = ns_to_ktime(default_cfs_period()); //0.1초
	cfs_b->burst = 0;

	INIT_LIST_HEAD(&cfs_b->throttled_cfs_rq);
	hrtimer_init(&cfs_b->period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED);
	cfs_b->period_timer.function = sched_cfs_period_timer;
	hrtimer_init(&cfs_b->slack_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cfs_b->slack_timer.function = sched_cfs_slack_timer;
	cfs_b->slack_started = false;
}

static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	cfs_rq->runtime_enabled = 0;
	INIT_LIST_HEAD(&cfs_rq->throttled_list);
}

/*
 * IAMROOT, 2022.12.22:
 * - perioid_timer(sched_cfs_period_timer)를 cfs_b->period후에
 *   시작하도록한다.
 */
void start_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	lockdep_assert_held(&cfs_b->lock);

/*
 * IAMROOT, 2022.12.22:
 * - 이미 누군가 start했다면 return.
 */
	if (cfs_b->period_active)
		return;

	cfs_b->period_active = 1;
/*
 * IAMROOT, 2022.12.22:
 * - cfs_b->period후에 period_timer(sched_cfs_period_timer)시작.
 */
	hrtimer_forward_now(&cfs_b->period_timer, cfs_b->period);
	hrtimer_start_expires(&cfs_b->period_timer, HRTIMER_MODE_ABS_PINNED);
}

static void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	/* init_cfs_bandwidth() was not called */
	if (!cfs_b->throttled_cfs_rq.next)
		return;

	hrtimer_cancel(&cfs_b->period_timer);
	hrtimer_cancel(&cfs_b->slack_timer);
}

/*
 * Both these CPU hotplug callbacks race against unregister_fair_sched_group()
 *
 * The race is harmless, since modifying bandwidth settings of unhooked group
 * bits doesn't do much.
 */

/* cpu online callback */
static void __maybe_unused update_runtime_enabled(struct rq *rq)
{
	struct task_group *tg;

	lockdep_assert_rq_held(rq);

	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
		struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

		raw_spin_lock(&cfs_b->lock);
		cfs_rq->runtime_enabled = cfs_b->quota != RUNTIME_INF;
		raw_spin_unlock(&cfs_b->lock);
	}
	rcu_read_unlock();
}

/* cpu offline callback */
static void __maybe_unused unthrottle_offline_cfs_rqs(struct rq *rq)
{
	struct task_group *tg;

	lockdep_assert_rq_held(rq);

	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

		if (!cfs_rq->runtime_enabled)
			continue;

		/*
		 * clock_task is not advancing so we just need to make sure
		 * there's some valid quota amount
		 */
		cfs_rq->runtime_remaining = 1;
		/*
		 * Offline rq is schedulable till CPU is completely disabled
		 * in take_cpu_down(), so we prevent new cfs throttling here.
		 */
		cfs_rq->runtime_enabled = 0;

		if (cfs_rq_throttled(cfs_rq))
			unthrottle_cfs_rq(cfs_rq);
	}
	rcu_read_unlock();
}

#else /* CONFIG_CFS_BANDWIDTH */

static inline bool cfs_bandwidth_used(void)
{
	return false;
}

static void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec) {}
static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq) { return false; }
static void check_enqueue_throttle(struct cfs_rq *cfs_rq) {}
static inline void sync_throttle(struct task_group *tg, int cpu) {}
static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}

static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return 0;
}

static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return 0;
}

static inline int throttled_lb_pair(struct task_group *tg,
				    int src_cpu, int dest_cpu)
{
	return 0;
}

void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b) {}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}
#endif

static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return NULL;
}
static inline void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b) {}
static inline void update_runtime_enabled(struct rq *rq) {}
static inline void unthrottle_offline_cfs_rqs(struct rq *rq) {}

#endif /* CONFIG_CFS_BANDWIDTH */

/**************************************************
 * CFS operations on tasks:
 */

#ifdef CONFIG_SCHED_HRTICK
/*
 * IAMROOT, 2023.01.28:
 * - prev에서 slice만큼 동작했는지 확인한다. slice만큼 동작했고 p가 current라면 resched요청을
 *   하고 그게 아니면 delta후에 hrtick이 동작하도록 조정한다.
 */
static void hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	SCHED_WARN_ON(task_rq(p) != rq);

	if (rq->cfs.h_nr_running > 1) {
		u64 slice = sched_slice(cfs_rq, se);
		u64 ran = se->sum_exec_runtime - se->prev_sum_exec_runtime;
		s64 delta = slice - ran;

		if (delta < 0) {
			if (task_current(rq, p))
				resched_curr(rq);
			return;
		}
		hrtick_start(rq, delta);
	}
}

/*
 * called from enqueue/dequeue and updates the hrtick when the
 * current task is from our class and nr_running is low enough
 * to matter.
 */
static void hrtick_update(struct rq *rq)
{
	struct task_struct *curr = rq->curr;

	if (!hrtick_enabled_fair(rq) || curr->sched_class != &fair_sched_class)
		return;

	if (cfs_rq_of(&curr->se)->nr_running < sched_nr_latency)
		hrtick_start_fair(rq, curr);
}
#else /* !CONFIG_SCHED_HRTICK */
static inline void
hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
}

static inline void hrtick_update(struct rq *rq)
{
}
#endif

#ifdef CONFIG_SMP
static inline unsigned long cpu_util(int cpu);

/*
 * prifri, 2023.05.06:
 * @return true  estimate cpu util의 1.2배가 cap을 넘은 경우.
 * @return false 안넘은 경우
 */
static inline bool cpu_overutilized(int cpu)
{
	return !fits_capacity(cpu_util(cpu), capacity_of(cpu));
}

static inline void update_overutilized_status(struct rq *rq)
{
	if (!READ_ONCE(rq->rd->overutilized) && cpu_overutilized(rq->cpu)) {
		WRITE_ONCE(rq->rd->overutilized, SG_OVERUTILIZED);
		trace_sched_overutilized_tp(rq->rd, SG_OVERUTILIZED);
	}
}
#else
static inline void update_overutilized_status(struct rq *rq) { }
#endif

/* Runqueue only has SCHED_IDLE tasks enqueued */
/*
 * IAMROOT, 2023.04.29:
 * - 가장 느린 SCHED_IDLE policy 를 가진 cfs task 만 동작 할때
 */
static int sched_idle_rq(struct rq *rq)
{
	return unlikely(rq->nr_running == rq->cfs.idle_h_nr_running &&
			rq->nr_running);
}

#ifdef CONFIG_SMP
static int sched_idle_cpu(int cpu)
{
	return sched_idle_rq(cpu_rq(cpu));
}
#endif

/*
 * The enqueue_task method is called before nr_running is
 * increased. Here we update the fair scheduling stats and
 * then put the task into the rbtree:
 */
static void
enqueue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;
	int idle_h_nr_running = task_has_idle_policy(p);
	int task_new = !(flags & ENQUEUE_WAKEUP);

	/*
	 * The code below (indirectly) updates schedutil which looks at
	 * the cfs_rq utilization to select a frequency.
	 * Let's add the task's estimated utilization to the cfs_rq's
	 * estimated utilization, before we update schedutil.
	 */
	util_est_enqueue(&rq->cfs, p);

	/*
	 * If in_iowait is set, the code below may not trigger any cpufreq
	 * utilization updates, so do it here explicitly with the IOWAIT flag
	 * passed.
	 */
	if (p->in_iowait)
		cpufreq_update_util(rq, SCHED_CPUFREQ_IOWAIT);

	for_each_sched_entity(se) {
		if (se->on_rq)
			break;
		cfs_rq = cfs_rq_of(se);
		enqueue_entity(cfs_rq, se, flags);

		cfs_rq->h_nr_running++;
		cfs_rq->idle_h_nr_running += idle_h_nr_running;

		if (cfs_rq_is_idle(cfs_rq))
			idle_h_nr_running = 1;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto enqueue_throttle;

		flags = ENQUEUE_WAKEUP;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		update_load_avg(cfs_rq, se, UPDATE_TG);
		se_update_runnable(se);
		update_cfs_group(se);

		cfs_rq->h_nr_running++;
		cfs_rq->idle_h_nr_running += idle_h_nr_running;

		if (cfs_rq_is_idle(cfs_rq))
			idle_h_nr_running = 1;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto enqueue_throttle;

               /*
                * One parent has been throttled and cfs_rq removed from the
                * list. Add it back to not break the leaf list.
                */
               if (throttled_hierarchy(cfs_rq))
                       list_add_leaf_cfs_rq(cfs_rq);
	}

	/* At this point se is NULL and we are at root level*/
	add_nr_running(rq, 1);

	/*
	 * Since new tasks are assigned an initial util_avg equal to
	 * half of the spare capacity of their CPU, tiny tasks have the
	 * ability to cross the overutilized threshold, which will
	 * result in the load balancer ruining all the task placement
	 * done by EAS. As a way to mitigate that effect, do not account
	 * for the first enqueue operation of new tasks during the
	 * overutilized flag detection.
	 *
	 * A better way of solving this problem would be to wait for
	 * the PELT signals of tasks to converge before taking them
	 * into account, but that is not straightforward to implement,
	 * and the following generally works well enough in practice.
	 */
	if (!task_new)
		update_overutilized_status(rq);

enqueue_throttle:
	if (cfs_bandwidth_used()) {
		/*
		 * When bandwidth control is enabled; the cfs_rq_throttled()
		 * breaks in the above iteration can result in incomplete
		 * leaf list maintenance, resulting in triggering the assertion
		 * below.
		 */
		for_each_sched_entity(se) {
			cfs_rq = cfs_rq_of(se);

			if (list_add_leaf_cfs_rq(cfs_rq))
				break;
		}
	}

	assert_list_leaf_cfs_rq(rq);

	hrtick_update(rq);
}

static void set_next_buddy(struct sched_entity *se);

/*
 * The dequeue_task method is called before nr_running is
 * decreased. We remove the task from the rbtree and
 * update the fair scheduling stats:
 */
static void dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;
	int task_sleep = flags & DEQUEUE_SLEEP;
	int idle_h_nr_running = task_has_idle_policy(p);
	bool was_sched_idle = sched_idle_rq(rq);

	util_est_dequeue(&rq->cfs, p);

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		dequeue_entity(cfs_rq, se, flags);

		cfs_rq->h_nr_running--;
		cfs_rq->idle_h_nr_running -= idle_h_nr_running;

		if (cfs_rq_is_idle(cfs_rq))
			idle_h_nr_running = 1;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto dequeue_throttle;

		/* Don't dequeue parent if it has other entities besides us */
		if (cfs_rq->load.weight) {
			/* Avoid re-evaluating load for this entity: */
			se = parent_entity(se);
			/*
			 * Bias pick_next to pick a task from this cfs_rq, as
			 * p is sleeping when it is within its sched_slice.
			 */
			if (task_sleep && se && !throttled_hierarchy(cfs_rq))
				set_next_buddy(se);
			break;
		}
		flags |= DEQUEUE_SLEEP;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		update_load_avg(cfs_rq, se, UPDATE_TG);
		se_update_runnable(se);
		update_cfs_group(se);

		cfs_rq->h_nr_running--;
		cfs_rq->idle_h_nr_running -= idle_h_nr_running;

		if (cfs_rq_is_idle(cfs_rq))
			idle_h_nr_running = 1;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto dequeue_throttle;

	}

	/* At this point se is NULL and we are at root level*/
	sub_nr_running(rq, 1);

	/* balance early to pull high priority tasks */
	if (unlikely(!was_sched_idle && sched_idle_rq(rq)))
		rq->next_balance = jiffies;

dequeue_throttle:
	util_est_update(&rq->cfs, p, task_sleep);
	hrtick_update(rq);
}

#ifdef CONFIG_SMP

/* Working cpumask for: load_balance, load_balance_newidle. */
DEFINE_PER_CPU(cpumask_var_t, load_balance_mask);
DEFINE_PER_CPU(cpumask_var_t, select_idle_mask);

#ifdef CONFIG_NO_HZ_COMMON

static struct {
	cpumask_var_t idle_cpus_mask;
	atomic_t nr_cpus;
	int has_blocked;		/* Idle CPUS has blocked load */
	unsigned long next_balance;     /* in jiffy units */
	unsigned long next_blocked;	/* Next update of blocked load in jiffies */
} nohz ____cacheline_aligned;

#endif /* CONFIG_NO_HZ_COMMON */

/*
 * IAMROOT, 2023.06.03:
 * - return cfs load
 */
static unsigned long cpu_load(struct rq *rq)
{
	return cfs_rq_load_avg(&rq->cfs);
}

/*
 * cpu_load_without - compute CPU load without any contributions from *p
 * @cpu: the CPU which load is requested
 * @p: the task which load should be discounted
 *
 * The load of a CPU is defined by the load of tasks currently enqueued on that
 * CPU as well as tasks which are currently sleeping after an execution on that
 * CPU.
 *
 * This method returns the load of the specified CPU by discounting the load of
 * the specified task, whenever the task is currently contributing to the CPU
 * load.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * cpu_load_without - *p의 기여 없이 CPU 부하를 계산합니다.
 * @cpu: 부하가 요청된 CPU
 * @p: 부하를 할인해야 하는 작업 CPU
 *
 * 부하는 해당 CPU에 현재 대기열에 있는 작업의
 * 부하로 정의됩니다. 해당 CPU에서 실행된 후 현재 잠자고 있는 작업.
 *
 * 이 메서드는 작업이 현재 CPU 부하에 기여할 때마다 지정된 작업의 부하를 할인하여 지정된
 * CPU의 부하를 반환합니다.
 *
 * - @p가 @rq에서 돌고 있으면 @p의 load를 뺀 load_avg 반환
 */
static unsigned long cpu_load_without(struct rq *rq, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned int load;

	/* Task has no contribution or is new */
	if (cpu_of(rq) != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_load(rq);

	cfs_rq = &rq->cfs;
	load = READ_ONCE(cfs_rq->avg.load_avg);

	/* Discount task's util from CPU's util */
	lsub_positive(&load, task_h_load(p));

	return load;
}

static unsigned long cpu_runnable(struct rq *rq)
{
	return cfs_rq_runnable_avg(&rq->cfs);
}

/*
 * IAMROOT, 2023.06.17:
 * - @p에 대한 runnable을 뺀 runnable 값을 return한다.
 */
static unsigned long cpu_runnable_without(struct rq *rq, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned int runnable;

	/* Task has no contribution or is new */
	if (cpu_of(rq) != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_runnable(rq);

	cfs_rq = &rq->cfs;
	runnable = READ_ONCE(cfs_rq->avg.runnable_avg);

	/* Discount task's runnable from CPU's runnable */
	lsub_positive(&runnable, p->se.avg.runnable_avg);

	return runnable;
}

/*
 * IAMROOT, 2023.06.13:
 * - return cpu_capacity
 */
static unsigned long capacity_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity;
}

/*
 * IAMROOT, 2023.05.23:
 * - wakee flip 통계를 낸다.
 *   1. 1초에 한번씩 wakee_flips을 절반으로 줄인다.
 *   2. 마지막에 깨운 task가 이전과 다르다면 기록하고 wakee_flips를 늘린다.
 * - wake_wide()와 연계해서 생각한다.
 * - ex) P0 thread가 P1, P2.. thread를 RR방식으로 깨우는 환경이라고 가정한다.
 *
 *   P0(Distributer) -> P1, P2, P3..
 *
 *   이경우 P0에서만 wakee_flips가 증가하고 나머지 PX에서는 wakee_flips가
 *   증가하지 않을것이다.
 *
 */
static void record_wakee(struct task_struct *p)
{
	/*
	 * Only decay a single time; tasks that have less then 1 wakeup per
	 * jiffy will not have built up many flips.
	 */
	/*
	 * IAMROOT. 2023.05.20:
	 * - google-translate
	 * 1초에 한 번 decay합니다. jiffy당 깨우기가 1회 미만인 작업은 뒤집기를 많이 만들지
	 * 않습니다.
	 * - 1초에 한번 wakee_flips를 반으로
	 */
	if (time_after(jiffies, current->wakee_flip_decay_ts + HZ)) {
		current->wakee_flips >>= 1;
		current->wakee_flip_decay_ts = jiffies;
	}

	/*
	 * IAMROOT, 2023.05.20:
	 * - 마지막 깨운 task와 다른 task를 깨우는 상황이면 wakee_flips 증가
	 */
	if (current->last_wakee != p) {
		current->last_wakee = p;
		current->wakee_flips++;
	}
}

/*
 * Detect M:N waker/wakee relationships via a switching-frequency heuristic.
 *
 * A waker of many should wake a different task than the one last awakened
 * at a frequency roughly N times higher than one of its wakees.
 *
 * In order to determine whether we should let the load spread vs consolidating
 * to shared cache, we look for a minimum 'flip' frequency of llc_size in one
 * partner, and a factor of lls_size higher frequency in the other.
 *
 * With both conditions met, we can be relatively sure that the relationship is
 * non-monogamous, with partner count exceeding socket size.
 *
 * Waker/wakee being client/server, worker/dispatcher, interrupt source or
 * whatever is irrelevant, spread criteria is apparent partner count exceeds
 * socket size.
 */
/*
 * IAMROOT, 2023.05.23:
 * - papago
 *   스위칭 주파수 휴리스틱을 통해 M:N 웨이커/웨이키 관계를 감지합니다.
 *
 *   많은 웨이커는 마지막으로 깨어난 것과 다른 작업을 하나의 웨이크보다 대략 
 *   N배 더 높은 빈도로 깨워야 합니다.
 *
 *   로드 분산과 공유 캐시로의 통합을 허용할지 여부를 결정하기 위해 한 파트너에서 
 *   llc_size의 최소 '플립' 빈도를 찾고 다른 파트너에서는 lls_size의 더 높은 
 *   빈도를 찾습니다.
 *
 *   두 조건이 모두 충족되면 파트너 수가 소켓 크기를 초과하여 일부일처제가 아닌 
 *   관계임을 비교적 확신할 수 있습니다.
 *
 *   Waker/wakee는 클라이언트/서버, 작업자/디스패처, 인터럽트 소스 또는 관련이
 *   없는 모든 것이므로 확산 기준은 명백한 파트너 수가 소켓 크기를 초과하는
 *   것입니다.
 * ----- chatopenai -----
 * - llc를 wide의 기준으로 쓰는 이유
 *   sd_llc_size는 웨이크업 관계가 넓은지를 판단하는 요인으로 사용된다. 
 *   sd_llc_size로 표시되는 캐시 크기는 캐시 공유 및 잠재적인 캐시 충돌 측면에서 
 *   작업 간의 관계를 추론하기 위한 휴리스틱으로 사용됩니다.
 *
 *   멀티 코어 시스템에서 서로 다른 CPU에서 실행되는 작업은 공유된 마지막 수준 
 *   캐시(LLC)에 액세스할 수 있습니다. 작업이 자주 깨어나면 캐시 리소스를 놓고 
 *   경합할 가능성이 있으며, 이로 인해 캐시 스래싱 및 성능 저하가 발생할 수 
 *   있습니다.
 *
 *   깨우기 폭을 결정하는 요소로 캐시 크기를 사용하는 아이디어는 다음과 같습니다.
 *
 *   주어진 작업의 깨우기 뒤집기 횟수(슬레이브)가 캐시 크기(인자)보다 현저히 
 *   적으면 해당 작업이 다른 작업과 캐시 경합을 일으킬 만큼 충분히 자주 깨우지 
 *   않고 있음을 나타냅니다. 이 경우 웨이크업 관계는 좁은 것으로 간주되며 CPU에
 *   부하를 분산시키는 것이 사용 가능한 캐시 리소스를 활용하는 데 도움이 될 수
 *   있습니다.
 *
 *   반면 현재 태스크의 웨이크업 플립 횟수(마스터)가 슬레이브 * 인자보다 현저히
 *   높으면 현재 태스크가 주어진 태스크보다 훨씬 더 자주 웨이크업하고 있음을
 *   나타냅니다. 이는 광범위한 웨이크업 관계를 시사하며, 이는 현재 작업이
 *   잠재적으로 캐시 리소스에 대한 여러 작업과 경합하고 있음을 의미합니다.
 *   이러한 경우 캐시 스래싱을 줄이기 위해 동일한 CPU에서 관련 작업 실행을
 *   통합하는 것이 좋습니다.
 *
 *   두 작업의 웨이크업 플립 수를 비교하고 캐시 크기를 고려하여 wake_wide 함수는
 *   웨이크업 관계가 부하 통합을 보장할 만큼 충분히 넓은지 또는 부하를 서로 다른 
 *   CPU에 분산시킬 만큼 충분히 좁은지 확인하려고 시도합니다. 
 * ----------------------
 *
 * - master를 wakee_flips이 큰것으로, 작은것을 slave로 설정한다.
 *   slave가 sd_llc_size보다 작거나 master가 sd_llc_size * slave보다 작다면
 *   return 0(not wide), 아니면 return 1(wide)이다.
 * - wakee_flips가 크다는것이 간접적으로 wakeup 범위가 넓다(wide)는 것을 의미하는데,
 *   이것을 sd_llc_size를 socket size개념으로 하여 wide의 기준을 잡는다.
 * - 휴리스틱으로 sd_llc_size를 가지고 판단하며, return 1이면 캐시가 친화가 깨진것,
 *   return 0이면 캐시 친화가 유지중이라고 생각한다.
 * - record_wakee()와 연계해서 생각한다.
 * - wake_wide는 캐쉬 비친화라는 뜻.
 */
static int wake_wide(struct task_struct *p)
{
	unsigned int master = current->wakee_flips;
	unsigned int slave = p->wakee_flips;
	/*
	 * IAMROOT, 2023.06.09:
	 * - SD_SHARE_PKG_RESOURCE가 설정된 가장 높은 sched_domain의 cpumask_weight
	 */
	int factor = __this_cpu_read(sd_llc_size);

	if (master < slave)
		swap(master, slave);
	if (slave < factor || master < slave * factor)
		return 0;
	return 1;
}

/*
 * The purpose of wake_affine() is to quickly determine on which CPU we can run
 * soonest. For the purpose of speed we only consider the waking and previous
 * CPU.
 *
 * wake_affine_idle() - only considers 'now', it check if the waking CPU is
 *			cache-affine and is (or	will be) idle.
 *
 * wake_affine_weight() - considers the weight to reflect the average
 *			  scheduling latency of the CPUs. This seems to work
 *			  for the overloaded case.
 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   wake_affine()의 목적은 우리가 가장 빨리 실행할 수 있는 CPU를 빠르게 
 *   결정하는 것입니다. 속도를 위해 깨어 있는 CPU와 이전 CPU만 고려합니다.
 *
 *   wake_affine_idle() - '지금'만 고려하여 깨우는 CPU가 캐시 affine이고 
 *   유휴 상태인지 확인합니다.
 *
 *   wake_affine_weight() - CPU의 평균 스케줄링 대기 시간을 반영하기 
 *   위해 가중치를 고려합니다. 이것은 오버로드된 경우에 작동하는 것 같습니다.
 *
 * @sync WF_SYNC set여부. select_task_rq_fair() 참고
 * - this_cpu, prev_cpu중에 cache affine cpu를 idle위주로 고려해서 선택한다.
 *   둘다 대상이 없으면 return nr_cpumask_bits
 */
static int
wake_affine_idle(int this_cpu, int prev_cpu, int sync)
{
	/*
	 * If this_cpu is idle, it implies the wakeup is from interrupt
	 * context. Only allow the move if cache is shared. Otherwise an
	 * interrupt intensive workload could force all tasks onto one
	 * node depending on the IO topology or IRQ affinity settings.
	 *
	 * If the prev_cpu is idle and cache affine then avoid a migration.
	 * There is no guarantee that the cache hot data from an interrupt
	 * is more important than cache hot data on the prev_cpu and from
	 * a cpufreq perspective, it's better to have higher utilisation
	 * on one CPU.
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   this_cpu가 유휴 상태이면 깨우기가 인터럽트 컨텍스트에서 발생했음을 
 *   의미합니다. 캐시가 공유된 경우에만 이동을 허용하십시오. 그렇지 
 *   않으면 인터럽트 집중 워크로드가 IO 토폴로지 또는 IRQ 선호도 설정에 
 *   따라 모든 작업을 하나의 노드로 강제 실행할 수 있습니다.
 *
 *   prev_cpu가 유휴 상태이고 캐시 관련성이 있는 경우 마이그레이션을 
 *   피하십시오. 인터럽트의 캐시 핫 데이터가 prev_cpu의 캐시 핫 데이터보다 
 *   더 중요하다는 보장은 없으며 cpufreq 관점에서 볼 때 하나의 CPU에서 더 
 *   높은 활용도를 갖는 것이 좋습니다.
 *
 * - this_cpu가 idle, prev_cpu와 cache share.
 *   prev_cpu가 idle이면 prev_cpu를 우선으로 고르고, 아니면 this_cpu
 * - prev_cpu가 idle일 경우 migrate를 안하고 기존 cpu에서 동작하는 것이
 *   l1 cache의 이득을 조금더 볼수있다고 생각한다.
 */
	if (available_idle_cpu(this_cpu) && cpus_share_cache(this_cpu, prev_cpu))
		return available_idle_cpu(prev_cpu) ? prev_cpu : this_cpu;

/*
 * IAMROOT, 2023.06.03:
 * - WF_SYNC참고. prev_cpu는 가능하면 sleep하러 가야된다. 그러므로 this_cpu에서
 *   worker를 동작시켜도 되는 상황이다.
 */
	if (sync && cpu_rq(this_cpu)->nr_running == 1)
		return this_cpu;

	if (available_idle_cpu(prev_cpu))
		return prev_cpu;

	return nr_cpumask_bits;
}

/*
 * IAMROOT, 2023.06.03:
 * @sync WF_SYNC set여부. select_task_rq_fair() 참고
 * - load 비율 계산을 하여 @this_cpu가 prev_cpu비해 더 여유로우면 this_cpu
 *   결정. this_cpu를 사용못하면 return nr_cpumask_bits
 * IAMROOT, 2023.06.09:
 * - @p가 @this_cpu로 이동했을 때 @prev_cpu 보다 load가 낮다면 this_cpu를 반환
 */
static int
wake_affine_weight(struct sched_domain *sd, struct task_struct *p,
		   int this_cpu, int prev_cpu, int sync)
{
	s64 this_eff_load, prev_eff_load;
	unsigned long task_load;

	this_eff_load = cpu_load(cpu_rq(this_cpu));

/*
 * IAMROOT, 2023.06.03:
 * - sync가 있으면 곧 sleep을 할 예정이므로 load가 감소할 것이다.
 *   그 보정을 취한다.
 */
	if (sync) {
		unsigned long current_load = task_h_load(current);
/*
 * IAMROOT, 2023.06.03:
 * - this cpu에 current task가 워낙 큰 load를 차지하고 있어
 *   없어지면 thi_cpu 부하가 없는거랑 마찬가지가 되니 그냥
 *   this_cpu로 선택한다.
 */
		if (current_load > this_eff_load)
			return this_cpu;

		this_eff_load -= current_load;
	}

/*
 * IAMROOT, 2023.06.03:
 * - 새로 추가되는 @p에 대한 load를 더해본다.
 */
	task_load = task_h_load(p);

	this_eff_load += task_load;
/*
 * IAMROOT, 2023.06.03:
 * - WA_BIAS : default true. imbalance_pct - 100 의 절반을 보정하여 
 *   계산한다.
 *   ex) imbalance_pct = 117 -> 8을 더 보정한다.
 */
	if (sched_feat(WA_BIAS))
		this_eff_load *= 100;
	this_eff_load *= capacity_of(prev_cpu);

/*
 * IAMROOT, 2023.06.03:
 * - 다른데로 옮겨갈 @p를 prev에서 빼준다.
 */
	prev_eff_load = cpu_load(cpu_rq(prev_cpu));
	prev_eff_load -= task_load;
	if (sched_feat(WA_BIAS))
		prev_eff_load *= 100 + (sd->imbalance_pct - 100) / 2;
	prev_eff_load *= capacity_of(this_cpu);

	/*
	 * If sync, adjust the weight of prev_eff_load such that if
	 * prev_eff == this_eff that select_idle_sibling() will consider
	 * stacking the wakee on top of the waker if no other CPU is
	 * idle.
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   동기화된 경우 prev_eff == this_eff인 경우 select_idle_sibling()이 
 *   다른 CPU가 유휴 상태가 아닌 경우 웨이커 위에 웨이크를 쌓는 것을 
 *   고려하도록 prev_eff_load의 가중치를 조정합니다.
 * - 동일한 점수일때 this_cpu가 더 유리하도록 한다.
 */
	if (sync)
		prev_eff_load += 1;

/*
 * IAMROOT, 2023.06.03:
 * - this_eff_load * prev_cpu_capa < prev_eff_load * this_cpu_capa
 *   즉 this가 prev보다 비율적으로 덜 바쁘면 this를 선택한다.
 *   그게 아니면 fail 처리.
 */
	return this_eff_load < prev_eff_load ? this_cpu : nr_cpumask_bits;
}

/*
 * IAMROOT, 2023.06.03:
 * - 1. idle로 우선 고려. prev / this / fail
 *   2. load 비율 고려. this / fail
 *   3. 그외 prev
 * - cache 친화 상태에서는 prev가 최우선이고 그다음이 현재 cpu가 우선이다
 */
static int wake_affine(struct sched_domain *sd, struct task_struct *p,
		       int this_cpu, int prev_cpu, int sync)
{
	int target = nr_cpumask_bits;
/*
 * IAMROOT, 2023.06.03:
 * - WA_IDLE default true
 *   idle인 cpu를 우선으로해서 1차적으로 결정을 한다.
 */
	if (sched_feat(WA_IDLE))
		target = wake_affine_idle(this_cpu, prev_cpu, sync);
/*
 * IAMROOT, 2023.06.03:
 * - WA_IDLE이 없거나 있었어도 못찾았다. WA_WEIGHT default true.
 *   위에서 못찾앗으면 load 비율을 따져 this_cpu가 prev보다 유리하면
 *   this_cpu로 선택한다.
 */
	if (sched_feat(WA_WEIGHT) && target == nr_cpumask_bits)
		target = wake_affine_weight(sd, p, this_cpu, prev_cpu, sync);

	schedstat_inc(p->se.statistics.nr_wakeups_affine_attempts);
/*
 * IAMROOT, 2023.06.03:
 * - 못찾앗으면 prev_cpu
 */
	if (target == nr_cpumask_bits)
		return prev_cpu;

	schedstat_inc(sd->ttwu_move_affine);
	schedstat_inc(p->se.statistics.nr_wakeups_affine);
	return target;
}

static struct sched_group *
find_idlest_group(struct sched_domain *sd, struct task_struct *p, int this_cpu);

/*
 * find_idlest_group_cpu - find the idlest CPU among the CPUs in the group.
 */
/*
 * IAMROOT, 2023.06.17:
 * - 기본적으로 idle cpu를 고른다.  sched idle이 있으면 우선 선택된다.
 * - 선택우선순위
 *   1. sched idle(cpu run상태)
 *   2. 기동이 가장짧은 cpu
 *   3. 가장 최근에 잠든 cpu
 *   4. load가 적은 cpu
 *
 * IAMROOT. 2023.06.12:
 * - google-translate
 * find_idlest_group_cpu - 그룹의 CPU 중에서 가장 유휴 상태인 CPU를 찾습니다.
 *
 * - Return: 가장 idle한 cpu
 *   1. group 내 idle policy 만 동작하는 rq가 있으면 바로 cpu 반환
 *   2. group 내 idle cpu 가 있다. 가장 얕게 잠든 cpu(shallowest_idle_cpu)를 선택
 *      하는데 shallowest_idle_cpu 판단은 cpuidle_state 여부에 따라 다르다.
 *      1. cpuidle_state를 가져올 수 있다. exit_latency 로 판단
 *         -> exit_latency가 가장 작은 cpu 반환
 *      2. cpuidle_state를 가져올 수 없다. idle_stamp로 판단
 *         -> idle_stamp가 가장 큰(가장 최근에 idle 된) cpu 반환
 *   3. group 내 idle cpu 가 없다. cpu_load로 판단
 *      -> cpu_load 가장 작은 cpu 반환
 *   4. group cpumask 와 @p cpumask 가 겹치지 않는다.
 *      -> @this_cpu 반환
 *   XXX 4번이 아니고 @this_cpu를 반환하는 조건은 찾지 못했다.
 */
static int
find_idlest_group_cpu(struct sched_group *group, struct task_struct *p, int this_cpu)
{
	unsigned long load, min_load = ULONG_MAX;
	unsigned int min_exit_latency = UINT_MAX;
	u64 latest_idle_timestamp = 0;
	int least_loaded_cpu = this_cpu;
	int shallowest_idle_cpu = -1;
	int i;

	/* Check if we have any choice: */
	if (group->group_weight == 1)
		return cpumask_first(sched_group_span(group));

	/* Traverse only the allowed CPUs */
/*
 * IAMROOT, 2023.06.17:
 * - @p와 @group의 and 범위의 cpu들을 순회한다.
 *   1. sched idle(cpu가 깨어있는 상태)
 *   2. cpu가 idle
 */
	for_each_cpu_and(i, sched_group_span(group), p->cpus_ptr) {
		struct rq *rq = cpu_rq(i);

		if (!sched_core_cookie_match(rq, p))
			continue;

		if (sched_idle_cpu(i))
			return i;

/*
 * IAMROOT, 2023.06.17:
 * - idle인 cpu깨워야된다. 시간이 덜걸리는것을 골라본다.
 */
		if (available_idle_cpu(i)) {
			struct cpuidle_state *idle = idle_get_state(rq);
			if (idle && idle->exit_latency < min_exit_latency) {
				/*
				 * We give priority to a CPU whose idle state
				 * has the smallest exit latency irrespective
				 * of any idle timestamp.
				 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   우리는 유휴 타임스탬프와 관계없이 유휴 상태에서 종료 대기 시간이 
 *   가장 짧은 CPU에 우선 순위를 부여합니다.
 */
				min_exit_latency = idle->exit_latency;
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
/*
 * IAMROOT, 2023.06.17:
 * - 최소 동작시간이 같은경우, 최근에 잠든것을 먼저 선택한다.
 */
			} else if ((!idle || idle->exit_latency == min_exit_latency) &&
				   rq->idle_stamp > latest_idle_timestamp) {
				/*
				 * If equal or no active idle state, then
				 * the most recently idled CPU might have
				 * a warmer cache.
				 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   활성 유휴 상태가 같거나 없는 경우 가장 최근에 유휴 상태가 
 *   된 CPU에 더 따뜻한 warmer가 있을 수 있습니다.
 */
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
			}
/*
 * IAMROOT, 2023.06.17:
 * - idle이 없는상태인 경우 . load기준으로 골라놓는다.
 */
		} else if (shallowest_idle_cpu == -1) {
			load = cpu_load(cpu_rq(i));
			if (load < min_load) {
				min_load = load;
				least_loaded_cpu = i;
			}
		}
	}

/*
 * IAMROOT, 2023.06.17:
 * - idle이 있으면 idle기준이였것으로 고르고 아니면 load로 고른것을 고른다.
 */
	return shallowest_idle_cpu != -1 ? shallowest_idle_cpu : least_loaded_cpu;
}

/*
 * IAMROOT, 2023.06.15:
 * - 1. sd에서 idlest group을 찾는다.
 *   2. idlest group에서 idlest cpu를 찾는다.
 *   3. sd의 범위를 좁힌다.
 *   4. 좁혀진 sd에서 1부터 반복한다.
 *   이렇게 함으로써 group 단위의 idle 분산이 일어날것이다.
 *   (idlest group을 cpu보다 먼저 고름으로써 idlest group쪽으로 선택이 되므로)
 */
static inline int find_idlest_cpu(struct sched_domain *sd, struct task_struct *p,
				  int cpu, int prev_cpu, int sd_flag)
{
	int new_cpu = cpu;

	if (!cpumask_intersects(sched_domain_span(sd), p->cpus_ptr))
		return prev_cpu;

	/*
	 * We need task's util for cpu_util_without, sync it up to
	 * prev_cpu's last_update_time.
	 */
	/*
	 * IAMROOT. 2023.06.09:
	 * - google-translate
	 * 우리는 cpu_util_without에 대한 작업의 util이 필요하며, prev_cpu의
	 * last_update_time까지 동기화합니다.
	 */
	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

/*
 * IAMROOT, 2023.06.15:
 * - sd는 select_idle_cpu()에서 sd_flag가 있는 최상위의 sd일것이다.
 *   아래로 내려가면서 찾는다.
 *   1. @p를 제외한 상태의 group중에서 가장 idle group을 고른다.
 *     - this group이면 child로 이동
 *   2. idle group중에 idle cpu를 고른다.
 *     - 이전과 같은 cpu를 찾았으면 child로 이동
 *   3. cpu의 최하위 sd부터 현재 찾은 sd보다 낮은 sd를 한번 좁힌다.
 */
	while (sd) {
		struct sched_group *group;
		struct sched_domain *tmp;
		int weight;

/*
 * IAMROOT, 2023.06.15:
 * - select_idle_cpu()에서 상위로 올라갈때, 만약 sd_flag가 없었으면 그냥 parent로
 *   간적이 있었을수도 있을것이다. 그런 경우 skip한다.
 */
		if (!(sd->flags & sd_flag)) {
			sd = sd->child;
			continue;
		}

		group = find_idlest_group(sd, p, cpu);
/*
 * IAMROOT, 2023.06.17:
 * - NULL이라는 것은 local group(this보다 idle이 없다는것)이
 *   선택됬다는것.
 *
 * - ex)
 *   +-------------top---------------+
 *   +------A-------+ +-----B--------+
 *   +----+ +-------+ +----+ +-------+
 *    idle   this             idle
 *  
 *    위의 경우에서, top인 상일때 A와 B가 비슷한 상황이라면
 *    이번 loop에서는 local group측인 A를 선택할것이다.
 *    그리고 다음 loop에서 idle, this중에 고르게 될것이다.
 */
		if (!group) {
			sd = sd->child;
			continue;
		}

		new_cpu = find_idlest_group_cpu(group, p, cpu);
/*
 * IAMROOT, 2023.06.15:
 * - 같은 cpu가 나왔으면 child로 내려간다.
 */
		/*
		 * IAMROOT, 2023.06.12:
		 * - group cpumask와 @p cpumask 가 겹치지 않는 경우. 즉 group cpu
		 *   모두가 task @p에서 허용되지 않음
		 */
		if (new_cpu == cpu) {
			/* Now try balancing at a lower domain level of 'cpu': */
			sd = sd->child;
			continue;
		}

		/* Now try balancing at a lower domain level of 'new_cpu': */
		/*
		 * IAMROOT, 2023.06.15:
		 * - @현재 커서의 @sd 에서 가장 idle한 그룹을 찾아 그 그룹에서 가장
		 *   idle한 cpu를 찾았다. cpu를 찾은 newcpu로 변경하고 sd_flag가
		 *   있는 한단계 아래 도메인에서 검색할 준비를 한다.
		 */
		cpu = new_cpu;
		weight = sd->span_weight;
		sd = NULL;
/*
 * IAMROOT, 2023.06.15:
 * - 새로찾은 cpu의 sd에서 weight가 작은 sd_flag가 겹치는 sd가 잇는지를
 *   parent로 올라가면서 찾아본다.
 *
 * - 즉 new cpu sd->weight 이내의 범위에서 최상위의 sd_flag가 겹치는
 *   sd를 찾는다.
 *   cpu를 일단 찾고, 해당 cpu의 sd에서 sd_flag가 겹치는 더 하위의 sd가
 *   있는지 한번더 스캔해본다.
 *  - ex) sd flag가 겹치는것을 O, 아니면 라고 표시, span_weight를 기준으로
 *  순회 표시. 16에서 시작했다고 가정
 *
 *  1) 2(X) -> 4(O) -> 8(O) -> 16(O)
 *                     ^8가 다음 대상 sd가 된다.
 *
 *  2) 2(X) -> 4(X) -> 8(X) -> 16(O)
 *    대상을 못찾고 sd == NULL로 끝나고 while을 빠져나간다.
 */
		/*
		 * IAMROOT, 2023.06.15:
		 * - 현재 루프의 sd 보다 sd_flag가 있는 한단계 아래 레벨의
		 *   도메인에서 break
		 */
		for_each_domain(cpu, tmp) {
			if (weight <= tmp->span_weight)
				break;
			if (tmp->flags & sd_flag)
				sd = tmp;
		}
	}

	return new_cpu;
}

/*
 * IAMROOT, 2023.06.10:
 * - @cpu가 idle 이면 @cpu 번호 반환
 */
static inline int __select_idle_cpu(int cpu, struct task_struct *p)
{
	if ((available_idle_cpu(cpu) || sched_idle_cpu(cpu)) &&
	    sched_cpu_cookie_match(cpu_rq(cpu), p))
		return cpu;

	return -1;
}

#ifdef CONFIG_SCHED_SMT
DEFINE_STATIC_KEY_FALSE(sched_smt_present);
EXPORT_SYMBOL_GPL(sched_smt_present);

/*
 * IAMROOT, 2023.06.15:
 * - @val(true or false)로 update.
 */
static inline void set_idle_cores(int cpu, int val)
{
	struct sched_domain_shared *sds;

	sds = rcu_dereference(per_cpu(sd_llc_shared, cpu));
	if (sds)
		WRITE_ONCE(sds->has_idle_cores, val);
}

/*
 * IAMROOT, 2023.06.13:
 * - @cpu의 sd_llc_shared에서 idle core가 있다면 true. 없으면 return @def
 */
static inline bool test_idle_cores(int cpu, bool def)
{
	struct sched_domain_shared *sds;

	sds = rcu_dereference(per_cpu(sd_llc_shared, cpu));
	if (sds)
		return READ_ONCE(sds->has_idle_cores);

	return def;
}

/*
 * Scans the local SMT mask to see if the entire core is idle, and records this
 * information in sd_llc_shared->has_idle_cores.
 *
 * Since SMT siblings share all cache levels, inspecting this limited remote
 * state should be fairly cheap.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * 로컬 SMT 마스크를 스캔하여 전체 코어가 유휴 상태인지 확인하고 이 정보를
 * sd_llc_shared->has_idle_cores에 기록합니다. SMT 형제는 모든 캐시 수준을
 * 공유하므로 이 제한된 원격 상태를 검사하는 비용은 상당히 저렴합니다.
 */
void __update_idle_core(struct rq *rq)
{
	int core = cpu_of(rq);
	int cpu;

	rcu_read_lock();
	if (test_idle_cores(core, true))
		goto unlock;

	for_each_cpu(cpu, cpu_smt_mask(core)) {
		if (cpu == core)
			continue;

		if (!available_idle_cpu(cpu))
			goto unlock;
	}

	set_idle_cores(core, 1);
unlock:
	rcu_read_unlock();
}

/*
 * Scan the entire LLC domain for idle cores; this dynamically switches off if
 * there are no idle cores left in the system; tracked through
 * sd_llc->shared->has_idle_cores and enabled through update_idle_core() above.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * 전체 LLC 도메인에서 유휴 코어를 검색합니다. 시스템에 유휴 코어가 남아 있지
 * 않으면 동적으로 꺼집니다. sd_llc->shared->has_idle_cores를 통해 추적되고 위의
 * update_idle_core()를 통해 활성화됩니다.
 *
 * - Return: 1. @core에 속한 모든 하드웨어 thread가 idle인 경우 @core retrun
 *           2. @core에 속한 일부 thread가 idle이 아닌 경우 -1. 그리고 @cpus에서
 *              core smt제거
 *   @idle_cpu: 하드웨어 thread중 idle 인 첫번째 cpu
 *
 * - arm64는 smt가 없으므로 무조건 __select_idle_cpu()로 진입한다.
 *   @core가 idle이면 return true. idle_cpu는 초기값(-1)
 *
 * - smt인시스템인 경우 목적은 다음과같다.
 *   1. core내의 thread가 전부idle인 core를 찾는게 최선의 목표. (return >= 0)
 *   2. run thread가 중간에 있을경우 그전에 감지한 idle thread를 @idle_cpu에 기록해놓는다.
 *      모든 core가 run중이면(return -1) @idle_cpu를 사용할것이다.
 */
static int select_idle_core(struct task_struct *p, int core, struct cpumask *cpus, int *idle_cpu)
{
	bool idle = true;
	int cpu;

	if (!static_branch_likely(&sched_smt_present))
		return __select_idle_cpu(core, p);

/*
 * IAMROOT, 2023.06.15:
 * - ex) smt cpu 0, 1, ,2가 있다고 가정한다. 0,1,2는 cpus_ptr에 모두 포함되있다고 가정,
 *       run cpu는 sched_idle이 아니라고 가정.
 *
 *   1. 0(idle) 1(idle) 2(idle)
 *   idle_cpu = 0, return core
 *
 *   2. 0(idle) 1(idle) 2(run) 인경우
 *   idle_cpu = 0, 2을 만남 시점에서 idle = false가 된다.
 *   return -1, idle_cpu = 0, cpus에서 core smt제거
 *
 *   3. 0(run) 1(idle) 2(idle) 인경우
 *   return -1, idle_cpu = -1(초기값), cpus에서 core smt 제거
 *
 *  4. 0(idle) 1(run) 2(idle) 인경우
 *  return -1, idle_cpu = 0, cpus에서 core smt제거
 */
	for_each_cpu(cpu, cpu_smt_mask(core)) {
		if (!available_idle_cpu(cpu)) {
			idle = false;
			if (*idle_cpu == -1) {
				if (sched_idle_cpu(cpu) && cpumask_test_cpu(cpu, p->cpus_ptr)) {
					*idle_cpu = cpu;
					break;
				}
				continue;
			}
			break;
		}
		if (*idle_cpu == -1 && cpumask_test_cpu(cpu, p->cpus_ptr))
			*idle_cpu = cpu;
	}

	if (idle)
		return core;

	cpumask_andnot(cpus, cpus, cpu_smt_mask(core));
	return -1;
}

/*
 * Scan the local SMT mask for idle CPUs.
 */
/*
 * IAMROOT, 2023.06.10:
 * - core내 하드웨어 스레드(smt)를 대상으로 idle cpu를 찾아서 반환
 */
static int select_idle_smt(struct task_struct *p, struct sched_domain *sd, int target)
{
	int cpu;

	for_each_cpu(cpu, cpu_smt_mask(target)) {
		if (!cpumask_test_cpu(cpu, p->cpus_ptr) ||
		    !cpumask_test_cpu(cpu, sched_domain_span(sd)))
			continue;
		if (available_idle_cpu(cpu) || sched_idle_cpu(cpu))
			return cpu;
	}

	return -1;
}

#else /* CONFIG_SCHED_SMT */

static inline void set_idle_cores(int cpu, int val)
{
}

static inline bool test_idle_cores(int cpu, bool def)
{
	return def;
}

static inline int select_idle_core(struct task_struct *p, int core, struct cpumask *cpus, int *idle_cpu)
{
	return __select_idle_cpu(core, p);
}

static inline int select_idle_smt(struct task_struct *p, struct sched_domain *sd, int target)
{
	return -1;
}

#endif /* CONFIG_SCHED_SMT */

/*
 * Scan the LLC domain for idle CPUs; this is dynamically regulated by
 * comparing the average scan cost (tracked in sd->avg_scan_cost) against the
 * average idle time for this rq (as found in rq->avg_idle).
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * 유휴 CPU에 대한 LLC 도메인을 스캔합니다. 이는 평균 스캔
 * 비용(sd->avg_scan_cost에서 추적됨)과 이 rq의 평균 유휴 시간(rq->avg_idle에서
 * 확인됨)을 비교하여 동적으로 조정됩니다.
 *
 * - 범위는 p->cpus_ptr, @sd의 span의 and 영역을 @target + 1부터 순회.
 * - has_idle_core 가 true: 하드웨어 thread가 모두 idle인 코어를 찾아서 찾은 cpu
 *   번호 반환. 없다면 thread 라도 반환.
 *   idle이 없엇따면 pcpu llc share측의 has_idle_core를 false로 update.
 *
 * - has_idle_core 가 false : 평균 스캔 비용과 이 rq의 평균 유휴 시간을 비교하여 동적
 *   으로 조정된  nr 수 이내의 idle 하드웨어 thread를 찾아 cpu 번호를 반환 및
 *   wake_avg_idle, avg_scan_cost update.
 *
 */
static int select_idle_cpu(struct task_struct *p, struct sched_domain *sd, bool has_idle_core, int target)
{
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(select_idle_mask);
	int i, cpu, idle_cpu = -1, nr = INT_MAX;
	struct rq *this_rq = this_rq();
	int this = smp_processor_id();
	struct sched_domain *this_sd;
	u64 time = 0;

	this_sd = rcu_dereference(*this_cpu_ptr(&sd_llc));
	if (!this_sd)
		return -1;

	cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr);

	/*
	 * IAMROOT, 2023.06.10:
	 * - SIS_PROP은 idle core를 못찾았을 때 찾기위해 시도하는 하드웨어 thread
	 *   갯수 제한
	 */
	if (sched_feat(SIS_PROP) && !has_idle_core) {
		u64 avg_cost, avg_idle, span_avg;
		unsigned long now = jiffies;

		/*
		 * If we're busy, the assumption that the last idle period
		 * predicts the future is flawed; age away the remaining
		 * predicted idle time.
		 */
		/*
		 * IAMROOT. 2023.06.10:
		 * - google-translate
		 * 우리가 바쁘다면 마지막 유휴 기간이 미래를 예측한다는 가정에 fault이
		 * 있습니다. 남은 예상 유휴 시간을 에이징합니다.
		 *
		 * - 틱으로 wake_avg_idle을 반감시킨다.
		 */
		if (unlikely(this_rq->wake_stamp < now)) {
			while (this_rq->wake_stamp < now && this_rq->wake_avg_idle) {
				this_rq->wake_stamp++;
				this_rq->wake_avg_idle >>= 1;
			}
		}

		avg_idle = this_rq->wake_avg_idle;
		avg_cost = this_sd->avg_scan_cost + 1;

		span_avg = sd->span_weight * avg_idle;
		/*
		 * IAMROOT, 2023.06.10:
		 * - nr: has_idle_core가 아닐 경우 idle_cpu 찾기 시도 횟수.
		 *       span_avg가 충분히 크다면 avg_cost를 나눈 값
		 *       아니면 최대 4회까지는 시도해 본다.
		 * - idle시간이 길었을수록 or span weight가 클수록 nr이 커진다.
		 */
		if (span_avg > 4*avg_cost)
			nr = div_u64(span_avg, avg_cost);
		else
			nr = 4;

		time = cpu_clock(this);
	}

/*
 * IAMROOT, 2023.06.15:
 * - 이전에 idle core가 있다고 예상됬으면 fullscan. 없었으면 nr 제한만큼만
 *   scan한다.
 * - smt가 아닐경우(arm64)
 *   기본적으로 has_idle_core의 차이는 nr번의 loop를 하느냐 안하느냐의 
 *   차이고, 이것도 SIS_PROP에 의존한다 SIS_PROP는 false인경우 
 *   has_idle_core의 true/ false여부의 차이는 없다. 즉 같은동작수행.
 *
 * - smt일 경우
 *   1. has_idle_core == true
 *   idle core가 있다는걸 기대하고 찾으며, idle thread가 있다는걸
 *   가정하고 상세하게 찾는 시도를 한다. 
 *   has_idle_core가 true임에도 idle core를 못찾았다면 그나마 찾은 
 *   idle thread를 최종적으로 return return한다.
 *
 *   2. has_idle_core == false
 *   nr번만 적당히 찾아보고 넘어간다. SIS_PROP == false였으면 다 돌긴할것이다.
 *
 */
	for_each_cpu_wrap(cpu, cpus, target + 1) {
		if (has_idle_core) {
			i = select_idle_core(p, cpu, cpus, &idle_cpu);
			/*
			 * IAMROOT, 2023.06.10:
			 * - 현재 루프의 cpu 내 하드웨어 thread가 모두 idle인
			 *   코어를 찾는다.
			 */
			if ((unsigned int)i < nr_cpumask_bits)
				return i;

		} else {
			if (!--nr)
				return -1;
			/*
			 * IAMROOT, 2023.06.10:
			 * - nr 횟수 내에서 idle인 cpu(하드웨어 thread)를 찾는다
			 */
			idle_cpu = __select_idle_cpu(cpu, p);
			if ((unsigned int)idle_cpu < nr_cpumask_bits)
				break;
		}
	}

	/*
	 * IAMROOT, 2023.06.10:
	 * - XXX has_idle_core 가 있다고 설정되었지만 못찾았음.
	 *   idle core가 없는것으로 pcpu에 false로 update.
	 */
	if (has_idle_core)
		set_idle_cores(target, false);

/*
 * IAMROOT, 2023.06.15:
 * - 이 함수 진입전에서 wake_avg_idle을 재게산하였고, 시작시간또한 time에
 *   기록하였다. scan시간만큼 wake_avg_idle을 보정하고 avg_scan_cost를
 *   update한다.
 */
	if (sched_feat(SIS_PROP) && !has_idle_core) {
		time = cpu_clock(this) - time;

		/*
		 * Account for the scan cost of wakeups against the average
		 * idle time.
		 */
		/*
		 * IAMROOT. 2023.06.10:
		 * - google-translate
		 * 평균 유휴 시간에 대한 웨이크업의 스캔 비용을 고려하십시오.
		 */
		this_rq->wake_avg_idle -= min(this_rq->wake_avg_idle, time);

		update_avg(&this_sd->avg_scan_cost, time);
	}

	return idle_cpu;
}

/*
 * Scan the asym_capacity domain for idle CPUs; pick the first idle one on which
 * the task fits. If no CPU is big enough, but there are idle ones, try to
 * maximize capacity.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * 유휴 CPU에 대한 asym_capacity 도메인을 스캔합니다. 작업에 맞는 첫 번째 유휴
 * 항목을 선택하십시오. 충분히 큰 CPU가 없지만 유휴 CPU가 있는 경우 용량을
 * 최대화하십시오.
 *
 * - @sd 내에서 idle cpu를 대상으로 task_util의 capa가 적합하면 해당 cpu를 선택하고
 *   적합한 cpu가 없으면 가장 capa 가 높은 idle cpu를 선택한다.
 */
static int
select_idle_capacity(struct task_struct *p, struct sched_domain *sd, int target)
{
	unsigned long task_util, best_cap = 0;
	int cpu, best_cpu = -1;
	struct cpumask *cpus;

	cpus = this_cpu_cpumask_var_ptr(select_idle_mask);
	cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr);

	task_util = uclamp_task_util(p);

	for_each_cpu_wrap(cpu, cpus, target) {
		unsigned long cpu_cap = capacity_of(cpu);

		if (!available_idle_cpu(cpu) && !sched_idle_cpu(cpu))
			continue;
		if (fits_capacity(task_util, cpu_cap))
			return cpu;

		if (cpu_cap > best_cap) {
			best_cap = cpu_cap;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

/*
 * IAMROOT, 2023.06.13:
 * @return true  @task_util의 1.2배가 @cpu capa를 초과 안한다.
 *         false @task_util의 1.2배가 @cpu capa를 넘는다.
 */
static inline bool asym_fits_capacity(int task_util, int cpu)
{
	if (static_branch_unlikely(&sched_asym_cpucapacity))
		return fits_capacity(task_util, capacity_of(cpu));

	return true;
}

/*
 * Try and locate an idle core/thread in the LLC cache domain.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * LLC 캐시 도메인에서 유휴 코어/스레드를 찾으십시오.
 *
 * - idle cpu를 찾는다.
 *   target, prev, p->recent_used_cpu, asym domain, smt, asym domain(cache 무시)의 순서로 무엇을 사용할지 결정한다.
 *
 * - 선택 우선순위
 *   1. target 이 idle(idle cpu or idle sched)이고 asym fits인경우
 *      -> target
 *   2. @target과 cache 친화이고 idle(idle cpu or idle sched)이고 asym fits인 경우
 *      -> prev
 *   3. this_rq가 kthread 한개만 돌고있고, this cpu가 @prev인경우.
 *      -> prev
 *   4. p->recent_used_cpu가 cache친화, asym fits한경우
 *      -> p->recent_used_cpu
 *   5. asym domain일 경우, 캐시친화포기. idle중에서 capa가 가장 높은 cpu을 선택
 *      -> asym domain중 capa가 가장 높은 cpu.
 *      -> idle인 cpu가 없었으면 target.
 *   6. sd_llc가 없을경우
 *      -> target
 *   7. smt일경우, idle core가 없다면 smt 내에서 idle인것을 찾는다.
 *      -> smt내의 idle cpu
 *   8. asym sd span와 p->cpus의 and영역에서 target + 1부터 순회를 하며
 *      idle core를 찾는다.
 *      -> asym sd span, p->cpus의 and영역에서 찾아진 idle core or idle thread
 *   9. 여지껏 못찾은경우. idle이 하나도 없었을 경우
 *      -> cpu
 *
 * - fast path(캐쉬친화)이므로 @target, @prev 가 idle이면 먼저 선택하고 그다음
 *   recent_used_cpu, hmp, llc 순서로 idle cpu를 선택해서 반환한다.
 */
static int select_idle_sibling(struct task_struct *p, int prev, int target)
{
	bool has_idle_core = false;
	struct sched_domain *sd;
	unsigned long task_util;
	int i, recent_used_cpu;

	/*
	 * On asymmetric system, update task utilization because we will check
	 * that the task fits with cpu's capacity.
	 */
	/*
	 * IAMROOT. 2023.06.10:
	 * - google-translate
	 * 비대칭 시스템에서는 작업이 CPU 용량에 맞는지 확인하므로 작업 활용도를
	 * 업데이트합니다.
	 */
	if (static_branch_unlikely(&sched_asym_cpucapacity)) {
		sync_entity_load_avg(&p->se);
		task_util = uclamp_task_util(p);
	}

	/*
	 * per-cpu select_idle_mask usage
	 */
	lockdep_assert_irqs_disabled();

	/*
	 * IAMROOT, 2023.06.10:
	 * - @target이 idle이고 capa 를 만족
	 */
	if ((available_idle_cpu(target) || sched_idle_cpu(target)) &&
	    asym_fits_capacity(task_util, target))
		return target;

	/*
	 * If the previous CPU is cache affine and idle, don't be stupid:
	 */
	/*
	 * IAMROOT. 2023.06.10:
	 * - google-translate
	 * 이전 CPU가 캐시 아핀 및 유휴 상태인 경우 어리석지 마십시오.
	 *
	 * - @target 과 @prev가 cache 공유상태이고 @prev가 idle 이면서 capa를 만족하면
	 *   @prev return
	 */
	if (prev != target && cpus_share_cache(prev, target) &&
	    (available_idle_cpu(prev) || sched_idle_cpu(prev)) &&
	    asym_fits_capacity(task_util, prev))
		return prev;

	/*
	 * Allow a per-cpu kthread to stack with the wakee if the
	 * kworker thread and the tasks previous CPUs are the same.
	 * The assumption is that the wakee queued work for the
	 * per-cpu kthread that is now complete and the wakeup is
	 * essentially a sync wakeup. An obvious example of this
	 * pattern is IO completions.
	 */
	/*
	 * IAMROOT. 2023.06.10:
	 * - google-translate
	 * kworker 스레드와 이전 CPU의 작업이 동일한 경우 cpu당 kthread가 wakee와 함께
	 * 스택되도록 허용합니다. 이제 완료된 cpu당 kthread에 대해 대기 중인 wakee 작업과
	 * 깨우기가 기본적으로 동기화 깨우기라고 가정합니다. 이 패턴의 명백한 예는 IO
	 * 완료입니다.
	 * - current 가 pcpu kthread 인 경우는 sync flag 가 있다고 가정하고
	 *   nr_running이 하나 이하이면 kthread가 잠들어서 0으로 될 것이라 예상하고
	 *   @prev를 반환
	 */
	if (is_per_cpu_kthread(current) &&
	    prev == smp_processor_id() &&
	    this_rq()->nr_running <= 1) {
		return prev;
	}

	/* Check a recently used CPU as a potential idle candidate: */
	/*
	 * IAMROOT. 2023.06.10:
	 * - google-translate
	 * 잠재적 유휴 후보로 최근에 사용된 CPU를 확인합니다.
	 *
	 * - 위 조건이 아니고 @target 과 recent_used_cpu가 cache 공유상태이고
	 *   recent_used_cpu가 idle 이면서 capa를 만족하면 recent_used_cpu(전전 cpu)를 반환.
	 *
	 *   ex) 1 -> 2, target = 3, if문 진입. (recent_used_cpu = 1, prev = 2)
	 *       1 -> 1, target = 3, if문 진입X. (recent_used_cpu = 1, prev = 1)
	 *       1 -> 2, target = 1, if문 진입X. (recent_used_cpu = 1, prev = 2)
	 *
	 * - 위에 if문까지를 통해서 여기까지 왔으면 target, prev사용하지 못하는 상태이다.
	 *   p->recent_used_cpu를 사용할수있는지 결정한다.
	 *   target이 최근에 사용한 cpu가 아닌상황에서, 이전에 사용한 cpu가
	 *   cache친화라면 target을 이전cpu로 사용한다는것이다.
	 *
	 * - 여기서부턴 p->recent_used_cpu가 갱신이되는 것이되므로 prev로 갱신한다.
	 */
	recent_used_cpu = p->recent_used_cpu;
	p->recent_used_cpu = prev;
	if (recent_used_cpu != prev &&
	    recent_used_cpu != target &&
	    cpus_share_cache(recent_used_cpu, target) &&
	    (available_idle_cpu(recent_used_cpu) || sched_idle_cpu(recent_used_cpu)) &&
	    cpumask_test_cpu(p->recent_used_cpu, p->cpus_ptr) &&
	    asym_fits_capacity(task_util, recent_used_cpu)) {
		/*
		 * Replace recent_used_cpu with prev as it is a potential
		 * candidate for the next wake:
		 */
		/*
		 * IAMROOT. 2023.06.10:
		 * - google-translate
		 * next_used_cpu는 다음 깨우기에 대한 잠재적인 후보이므로 prev로
		 * 교체합니다.
		 *
		 * - 중복코드라 최신 커널에서는 삭제됨
		 */
		p->recent_used_cpu = prev;
		return recent_used_cpu;
	}

/*
 * IAMROOT, 2023.06.13:
 * - 여기까지 왔으면 target, prev, p->recent_used_cpu가 전부 idle이 아니거나 
 *   asym fit이 아니던가, cache친화가 아니던가, @p의 범위에 설정이 안되있던가
 *   하는 상황일 것이다. 캐시친화는 포기한다.
 *   sd에서 idle인것들중에서 capa가 가장큰것을 선택한다.
 */
	/*
	 * For asymmetric CPU capacity systems, our domain of interest is
	 * sd_asym_cpucapacity rather than sd_llc.
	 */
	/*
	 * IAMROOT. 2023.06.10:
	 * - google-translate
	 * 비대칭 CPU 용량 시스템의 경우 관심 영역은 sd_llc가 아닌
	 * sd_asym_cpucapacity입니다.
	 */
	if (static_branch_unlikely(&sched_asym_cpucapacity)) {
		sd = rcu_dereference(per_cpu(sd_asym_cpucapacity, target));
		/*
		 * On an asymmetric CPU capacity system where an exclusive
		 * cpuset defines a symmetric island (i.e. one unique
		 * capacity_orig value through the cpuset), the key will be set
		 * but the CPUs within that cpuset will not have a domain with
		 * SD_ASYM_CPUCAPACITY. These should follow the usual symmetric
		 * capacity path.
		 */
		/*
		 * IAMROOT. 2023.06.10:
		 * - google-translate
		 * 독점 cpuset이 대칭 아일랜드(즉, cpuset를 통한 하나의 고유한
		 * capacity_orig 값)를 정의하는 비대칭 CPU 용량 시스템에서 키는
		 * 설정되지만 해당 cpuset 내의 CPU에는 SD_ASYM_CPUCAPACITY가 있는
		 * 도메인이 없습니다. 이들은 일반적인 대칭 용량 경로를 따라야 합니다.
		 *
		 * - 비대칭 구조인 cluster @sd 내에서 idle cpu를 대상으로
		 *   task_util의 capa가 적합하면 해당 cpu를 선택하고 적합한
		 *   cpu가 없으면 가장 capa 가 높은 idle cpu를 선택한다.
		 */
		if (sd) {
			i = select_idle_capacity(p, sd, target);
			return ((unsigned)i < nr_cpumask_bits) ? i : target;
		}
	}

/*
 * IAMROOT, 2023.06.13:
 * - asym가 아니였을경우, target의 sd_llc을 가져온다.
 */
	sd = rcu_dereference(per_cpu(sd_llc, target));
	if (!sd)
		return target;

/*
 * IAMROOT, 2023.06.13:
 * - arm64는 진입안한다.
 */
	if (sched_smt_active()) {
		has_idle_core = test_idle_cores(target, false);

		/*
		 * IAMROOT, 2023.06.10:
		 * - idle core가 없는 경우, prev와 target이 같은 cache라면
		 *   smt 내에서 찾는다
		 */
		if (!has_idle_core && cpus_share_cache(prev, target)) {
			/*
			 * IAMROOT, 2023.06.10:
			 * - core내 하드웨어 스레드(smt)를 대상으로 idle cpu를
			 *   찾아서 반환
			 */
			i = select_idle_smt(p, sd, prev);
			if ((unsigned int)i < nr_cpumask_bits)
				return i;
		}
/*
 * IAMROOT, 2023.06.13:
 * - smt였을경우, idle core가 있거나, smt내부에서 idle이 없었거나,
 *   cache공유를 안한다면 아랫쪽으로 진입한다.
 */
	}

/*
 * IAMROOT, 2023.06.15:
 * - 1순위로 idle core를 찾는다. 2순위로 has_idle_core가 true라면, idle core
 *   가 없는 경우 idle thread라도 찾는다.
 *   has_idle_core가 false라면 적당히 idle core를 찾는다.
 */
	i = select_idle_cpu(p, sd, has_idle_core, target);
	if ((unsigned)i < nr_cpumask_bits)
		return i;

	return target;
}

/**
 * cpu_util - Estimates the amount of capacity of a CPU used by CFS tasks.
 * @cpu: the CPU to get the utilization of
 *
 * The unit of the return value must be the one of capacity so we can compare
 * the utilization with the capacity of the CPU that is available for CFS task
 * (ie cpu_capacity).
 *
 * cfs_rq.avg.util_avg is the sum of running time of runnable tasks plus the
 * recent utilization of currently non-runnable tasks on a CPU. It represents
 * the amount of utilization of a CPU in the range [0..capacity_orig] where
 * capacity_orig is the cpu_capacity available at the highest frequency
 * (arch_scale_freq_capacity()).
 * The utilization of a CPU converges towards a sum equal to or less than the
 * current capacity (capacity_curr <= capacity_orig) of the CPU because it is
 * the running time on this CPU scaled by capacity_curr.
 *
 * The estimated utilization of a CPU is defined to be the maximum between its
 * cfs_rq.avg.util_avg and the sum of the estimated utilization of the tasks
 * currently RUNNABLE on that CPU.
 * This allows to properly represent the expected utilization of a CPU which
 * has just got a big task running since a long sleep period. At the same time
 * however it preserves the benefits of the "blocked utilization" in
 * describing the potential for other tasks waking up on the same CPU.
 *
 * Nevertheless, cfs_rq.avg.util_avg can be higher than capacity_curr or even
 * higher than capacity_orig because of unfortunate rounding in
 * cfs.avg.util_avg or just after migrating tasks and new task wakeups until
 * the average stabilizes with the new running time. We need to check that the
 * utilization stays within the range of [0..capacity_orig] and cap it if
 * necessary. Without utilization capping, a group could be seen as overloaded
 * (CPU0 utilization at 121% + CPU1 utilization at 80%) whereas CPU1 has 20% of
 * available capacity. We allow utilization to overshoot capacity_curr (but not
 * capacity_orig) as it useful for predicting the capacity required after task
 * migrations (scheduler-driven DVFS).
 *
 * Return: the (estimated) utilization for the specified CPU
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   cpu_util - CFS 작업이 사용하는 CPU 용량을 추정합니다.
 *   @cpu: 활용도를 얻을 CPU.
 *
 *   반환 값의 단위는 용량 중 하나여야 CFS 작업에 사용 가능한 
 *   CPU 용량(예: cpu_capacity)과 사용량을 비교할 수 있습니다.
 *
 *   cfs_rq.avg.util_avg는 실행 가능한 작업의 실행 시간과 CPU에서 현재 실행할 
 *   수 없는 작업의 최근 사용률을 더한 합계입니다. 이는 
 *   [0..capacity_orig] 범위에서 CPU 사용률을 나타냅니다. 여기서 capacity_orig는 
 *   가장 높은 빈도(arch_scale_freq_capacity())에서 사용 가능한 cpu_capacity입니다.
 *   CPU 사용률은 이 CPU의 실행 시간이 capacity_curr로 조정되기 때문에 CPU의 
 *   현재 용량(capacity_curr <= capacity_orig) 이하의 합계로 수렴됩니다.
 *
 *   CPU의 예상 사용률은 cfs_rq.avg.util_avg와 해당 CPU에서 현재 실행 가능한 
 *   작업의 예상 사용률 합계 사이의 최대값으로 정의됩니다.
 *   이를 통해 긴 휴면 기간 이후 실행 중인 큰 작업이 있는 CPU의 예상 사용률을 
 *   적절하게 나타낼 수 있습니다. 그러나 동시에 동일한 CPU에서 깨어나는 다른 
 *   작업의 가능성을 설명할 때 차단된 사용률의 이점을 유지합니다.
 *
 *   그럼에도 불구하고 cfs_rq.avg.util_avg는 cfs.avg.util_avg의 잘못된 
 *   반올림으로 인해 또는 새 실행 시간으로 평균이 안정화될 때까지 작업 
 *   마이그레이션 및 새 작업 웨이크업 직후에 capacity_curr 또는 
 *   capacity_orig보다 높을 수 있습니다. 사용량이 [0..capacity_orig] 
 *   범위 내에서 유지되는지 확인하고 필요한 경우 제한해야 합니다. 사용률 
 *   제한이 없으면 그룹은 과부하 상태(CPU0 사용률 121% + CPU1 사용률 80%)로 
 *   표시될 수 있지만 CPU1은 사용 가능한 용량의 20%를 가집니다. 작업 
 *   마이그레이션(스케줄러 기반 DVFS) 후에 필요한 용량을 예측하는 데 
 *   유용하므로 사용률이 capacity_curr(capacity_orig는 아님)를 초과하도록 허용합니다.
 *
 *   Return: 지정된 CPU의 (예상) 사용률. 
 *  - estimate cpu 사용율을 return한다.
 *  - ps) big task였던 작업이 long sleep을 이후 깨어 낫을때 load가 높아질것을
 *  예측하기 위함.
 */
static inline unsigned long cpu_util(int cpu)
{
	struct cfs_rq *cfs_rq;
	unsigned int util;

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	if (sched_feat(UTIL_EST))
		util = max(util, READ_ONCE(cfs_rq->avg.util_est.enqueued));

	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

/*
 * cpu_util_without: compute cpu utilization without any contributions from *p
 * @cpu: the CPU which utilization is requested
 * @p: the task which utilization should be discounted
 *
 * The utilization of a CPU is defined by the utilization of tasks currently
 * enqueued on that CPU as well as tasks which are currently sleeping after an
 * execution on that CPU.
 *
 * This method returns the utilization of the specified CPU by discounting the
 * utilization of the specified task, whenever the task is currently
 * contributing to the CPU utilization.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * cpu_util_without: *p의 기여 없이 cpu 사용률을 계산합니다.
 * @cpu: 사용률이 요청된 CPU
 * @p: 사용률을 할인해야 하는 작업
 *
 * CPU 사용률은 해당 CPU에 현재 대기 중인 작업의 사용률과 해당 CPU에서 실행된 후 현재
 * 잠자고 있는 작업.
 *
 * 이 메서드는 작업이 현재 CPU 사용률에 기여할 때마다 지정된 작업의 사용률을 할인하여 지정된
 * CPU의 사용률을 반환합니다.
 *
 * - util or est util중에 max를 선택한다.
 *   return (rq util sum - task util_est)
 *   return (rq util est sum - rq util_est)
 */
static unsigned long cpu_util_without(int cpu, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned int util;

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_util(cpu);

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	/* Discount task's util from CPU's util */
	lsub_positive(&util, task_util(p));

	/*
	 * Covered cases:
	 *
	 * a) if *p is the only task sleeping on this CPU, then:
	 *      cpu_util (== task_util) > util_est (== 0)
	 *    and thus we return:
	 *      cpu_util_without = (cpu_util - task_util) = 0
	 *
	 * b) if other tasks are SLEEPING on this CPU, which is now exiting
	 *    IDLE, then:
	 *      cpu_util >= task_util
	 *      cpu_util > util_est (== 0)
	 *    and thus we discount *p's blocked utilization to return:
	 *      cpu_util_without = (cpu_util - task_util) >= 0
	 *
	 * c) if other tasks are RUNNABLE on that CPU and
	 *      util_est > cpu_util
	 *    then we use util_est since it returns a more restrictive
	 *    estimation of the spare capacity on that CPU, by just
	 *    considering the expected utilization of tasks already
	 *    runnable on that CPU.
	 *
	 * Cases a) and b) are covered by the above code, while case c) is
	 * covered by the following code when estimated utilization is
	 * enabled.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   a) *p가 이 CPU에서 잠자고 있는 유일한 작업이면: 
 *      cpu_util (== task_util) > util_est (== 0)
 *    and thus we return:
 *      cpu_util_without = (cpu_util - task_util) = 0
 *
 *   b) 이 CPU에서 다른 작업이 SLEEPING중이고, 다른 task가 깨어나면서
 *   CPU가 IDLE을 벗어나는 경우
 *    then:
 *      cpu_util >= task_util
 *      cpu_util > util_est (== 0)
 *    and thus we discount *p's blocked utilization to return:
 *      cpu_util_without = (cpu_util - task_util) >= 0
 *
 *  c) 다른 작업이 해당 CPU에서 실행 가능하고 
 *  util_est > cpu_util인 경우 해당 CPU에서 이미 실행 가능한 작업의
 *  예상 사용률을 고려하여 해당 CPU의 예비 용량에 대한 보다 제한적인
 *  추정치를 반환하므로 util_est를 사용합니다.    
 *  
 *  a)와 b)의 경우는 위의 코드로 처리되며, c)의 경우는 사용량 추정이 
 *  활성화된 경우 다음 코드로 처리됩니다. 
 *
 * - 보통 rq의 est와, task의 est중 max값을 보통 사용하면 되는데, race time에 대한
 *   처리가 조금 추가되있다.
 */  
	 if (sched_feat(UTIL_EST)) {
		unsigned int estimated =
			READ_ONCE(cfs_rq->avg.util_est.enqueued);

		/*
		 * Despite the following checks we still have a small window
		 * for a possible race, when an execl's select_task_rq_fair()
		 * races with LB's detach_task():
		 *
		 *   detach_task()
		 *     p->on_rq = TASK_ON_RQ_MIGRATING;
		 *     ---------------------------------- A
		 *     deactivate_task()                   \
		 *       dequeue_task()                     + RaceTime
		 *         util_est_dequeue()              /
		 *     ---------------------------------- B
		 *
		 * The additional check on "current == p" it's required to
		 * properly fix the execl regression and it helps in further
		 * reducing the chances for the above race.
		 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   다음 검사에도 불구하고 execl의 select_task_rq_fair()가 LB의 
 *   detach_task()와 경쟁할 때 가능한 경쟁에 대한 작은 window이 여전히 
 *   있습니다. 
 *
 *   detach_task()
 *     p->on_rq = TASK_ON_RQ_MIGRATING;
 *     ---------------------------------- A
 *    deactivate_task()                   \
 *      dequeue_task()                     + RaceTime
 *        util_est_dequeue()              /
 *     ---------------------------------- B
 *
 *   current == p에 대한 추가 검사는 execl 회귀를 올바르게 수정하는 
 *   데 필요하며 위 경쟁의 기회를 더 줄이는 데 도움이 됩니다. 
 *
 * - flag가 TASK_ON_RQ_MIGRATING로 바뀐후에 util_est_dequeue()를 나중에 하는게
 *   보인다. 원래 대로라면 lock을 사용하면되겠지만, lock을 안하기 위해 그냥 
 *   racetime인것이 확인되면 @p에 대한 util을 제거해서 계산한다.
 */
		if (unlikely(task_on_rq_queued(p) || current == p))
			lsub_positive(&estimated, _task_util_est(p));

		util = max(util, estimated);
	}

	/*
	 * Utilization (estimated) can exceed the CPU capacity, thus let's
	 * clamp to the maximum CPU capacity to ensure consistency with
	 * the cpu_util call.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   사용률(예상)은 CPU 용량을 초과할 수 있으므로 cpu_util 호출과의 
 *   일관성을 보장하기 위해 최대 CPU 용량으로 고정합니다.
 */
	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

/*
 * Predicts what cpu_util(@cpu) would return if @p was migrated (and enqueued)
 * to @dst_cpu.
 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   @p가 @dst_cpu로 마이그레이션(및 대기열에 포함)된 경우 cpu_util(@cpu)이 반환할
 *   항목을 예측합니다.
 *
 *  @cpu 평가 cpu
 *  @dst_cpu 목적 cpu 
 *
 *  - migrate 했을때의(next 상황에서의) util 예상치를 계산한다.
 *    util 과 util_est끼리 따로 계산해서 max값을 계산한다.
 */
static unsigned long cpu_util_next(int cpu, struct task_struct *p, int dst_cpu)
{
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util_est, util = READ_ONCE(cfs_rq->avg.util_avg);

	/*
	 * If @p migrates from @cpu to another, remove its contribution. Or,
	 * if @p migrates from another CPU to @cpu, add its contribution. In
	 * the other cases, @cpu is not impacted by the migration, so the
	 * util_avg should already be correct.
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   @p가 @cpu에서 다른 것으로 마이그레이션하는 경우 해당 기여를 제거합니다. 
 *   또는 @p가 다른 CPU에서 @cpu로 마이그레이션하는 경우 기여도를 추가합니다.
 *   다른 경우에는 @cpu가 마이그레이션의 영향을 받지 않으므로 util_avg가 이미
 *   정확해야 합니다.
 *  - cpu == dst_cpu 인경우는 자신(cpu)한테 이동해오는 개념이라 추가되는 개념.
 *    cpu != dst_cpu 인경우는 cpu -> dst_cpu로 가는 개념이라 빼야된다.
 */
	if (task_cpu(p) == cpu && dst_cpu != cpu)
		lsub_positive(&util, task_util(p));
	else if (task_cpu(p) != cpu && dst_cpu == cpu)
		util += task_util(p);

/*
 * IAMROOT, 2023.06.03:
 * - 증간된 util과 est를 비교해서 max로 보정한다.
 *   est가 task가 적어도 이만큼은 동작을해야되는 추정치이므로 이에 대한 고려
 *   (즉 저평가 방지)를 한다.
 */
	/*
	 * IAMROOT, 2023.06.04:
	 * - XXX if(task_cpu(p) = cpu && dst_cpu == cpu) 의 경우 아래 조건
	 *   if (dst_cpu == cpu) 에서 참이 되어 util_est 에 값을 더하게 되는데
	 *   위의 util 조건에서는 이런 경우 같은 cpu가 dst이므로 아무 계산을
	 *   하지 않는다. util_est에서는 task의 이동이 없는 데도 더해주는 이유는?
	 */
	if (sched_feat(UTIL_EST)) {
		util_est = READ_ONCE(cfs_rq->avg.util_est.enqueued);

		/*
		 * During wake-up, the task isn't enqueued yet and doesn't
		 * appear in the cfs_rq->avg.util_est.enqueued of any rq,
		 * so just add it (if needed) to "simulate" what will be
		 * cpu_util() after the task has been enqueued.
		 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   깨우는 동안 작업은 아직 대기열에 추가되지 않고 rq의 
 *   cfs_rq->avg.util_est.enqueued에 표시되지 않으므로 필요한 경우 추가하여 
 *   작업 후 cpu_util()이 될 것을 시뮬레이트합니다. 인큐되었습니다.
 * - 저평가를 방지하는 개념이므로, 빼야되는 상황(dst_cpu != cpu)에 대한 처리는 안하고,
 *   더해야되는 상황에서만 수행한다.
 */
		if (dst_cpu == cpu)
			util_est += _task_util_est(p);

		util = max(util, util_est);
	}

	return min(util, capacity_orig_of(cpu));
}

/*
 * compute_energy(): Estimates the energy that @pd would consume if @p was
 * migrated to @dst_cpu. compute_energy() predicts what will be the utilization
 * landscape of @pd's CPUs after the task migration, and uses the Energy Model
 * to compute what would be the energy if we decided to actually migrate that
 * task.
 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   compute_energy(): @p가 @dst_cpu로 마이그레이션된 경우 @pd가 소비할 
 *   에너지를 추정합니다. compute_energy()는 작업 마이그레이션 후 @pd CPU의 
 *   사용 환경을 예측하고 에너지 모델을 사용하여 해당 작업을 실제로 
 *   마이그레이션하기로 결정한 경우 에너지가 무엇인지 계산합니다.
 *
 * - @p가 @dst_cpu로 migrate migrate후의 @pd에 대한 energy를 예측한다.
 *   dst_cpu == -1이면 migrate없는 상황에서의 base값.
 *
 * IAMROOT, 2023.06.07:
 * - @pd 내에서의 최대 util(max_util) 값과 util의 총합(sum_util)을 구하여 이를 인수로
 *   em_cpu_energy 를 호출한다.
 */
static long
compute_energy(struct task_struct *p, int dst_cpu, struct perf_domain *pd)
{
	struct cpumask *pd_mask = perf_domain_span(pd);
	unsigned long cpu_cap = arch_scale_cpu_capacity(cpumask_first(pd_mask));
	unsigned long max_util = 0, sum_util = 0;
	unsigned long _cpu_cap = cpu_cap;
	int cpu;

/*
 * IAMROOT, 2023.06.03:
 * - 온도 capa보정
 */
	_cpu_cap -= arch_scale_thermal_pressure(cpumask_first(pd_mask));

	/*
	 * The capacity state of CPUs of the current rd can be driven by CPUs
	 * of another rd if they belong to the same pd. So, account for the
	 * utilization of these CPUs too by masking pd with cpu_online_mask
	 * instead of the rd span.
	 *
	 * If an entire pd is outside of the current rd, it will not appear in
	 * its pd list and will not be accounted by compute_energy().
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   현재 rd의 CPU 용량 상태는 동일한 pd에 속한 경우 다른 rd의 CPU에 
 *   의해 구동될 수 있습니다. 따라서 rd span 대신 cpu_online_mask로 pd를 
 *   마스킹하여 이러한 CPU의 사용률도 고려하십시오. 
 *
 *   전체 pd가 현재 rd 밖에 있는 경우 pd 목록에 나타나지 않으며 
 *   compute_energy()에서 계산되지 않습니다.
 *
 * - sum_util
 *   energy model 산출 총합
 * - max_util
 *   max freq util model
 */
	for_each_cpu_and(cpu, pd_mask, cpu_online_mask) {
		unsigned long util_freq = cpu_util_next(cpu, p, dst_cpu);
		unsigned long cpu_util, util_running = util_freq;
		struct task_struct *tsk = NULL;

		/*
		 * When @p is placed on @cpu:
		 *
		 * util_running = max(cpu_util, cpu_util_est) +
		 *		  max(task_util, _task_util_est)
		 *
		 * while cpu_util_next is: max(cpu_util + task_util,
		 *			       cpu_util_est + _task_util_est)
		 */
/*
 * IAMROOT, 2023.06.03:
 * - @p가 @cpu로 migrate되는(즉 증가되는 상황) cpu에 대해서의 처리.
 * - cpu_util_next(cpu, p, -1) 
 *   dst_cpu가 없은 예측값을 가져온다.(max(cpu_util, cpu_util_est))
 * - task_util_est(p) 
 *   max(task_util, _task_util_est) 
 *
 * - cpu_util_next에서 dst_cpu를 넣어서 계산하는 것은 task_util만
 *   고려가 되지만, 이렇게 task_util_est를 해 좀더 load를 추가 해준다.
 */
		if (cpu == dst_cpu) {
			tsk = p;
			util_running =
				cpu_util_next(cpu, p, -1) + task_util_est(p);
		}

		/*
		 * Busy time computation: utilization clamping is not
		 * required since the ratio (sum_util / cpu_capacity)
		 * is already enough to scale the EM reported power
		 * consumption at the (eventually clamped) cpu_capacity.
		 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   바쁜 시간 계산: 비율(sum_util / cpu_capacity)이 (결국 고정된) 
 *   cpu_capacity에서 EM 보고된 전력 소비를 확장하기에 이미 충분하기 
 *   때문에 사용률 고정이 필요하지 않습니다.
 */
		cpu_util = effective_cpu_util(cpu, util_running, cpu_cap,
					      ENERGY_UTIL, NULL);
/*
 * IAMROOT, 2023.06.03:
 * - 계산된 util을 온도가 계산된 cpu_capa와 min 비교후 sum_util에 추가한다.
 */
		sum_util += min(cpu_util, _cpu_cap);

		/*
		 * Performance domain frequency: utilization clamping
		 * must be considered since it affects the selection
		 * of the performance domain frequency.
		 * NOTE: in case RT tasks are running, by default the
		 * FREQUENCY_UTIL's utilization can be max OPP.
		 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   Performance domain frequency: utilization 클램핑은 
 *   Performance domain frequency 선택에 영향을 미치므로 반드시 
 *   고려해야 합니다.
 *   메모: RT 작업이 실행 중인 경우 기본적으로 FREQUENCY_UTIL의 
 *   utilization는 최대 OPP가 될 수 있습니다.
 *
 * - freq 산출은 max_util로하여 max값을 갱신한다.
 */
		cpu_util = effective_cpu_util(cpu, util_freq, cpu_cap,
					      FREQUENCY_UTIL, tsk);
		max_util = max(max_util, min(cpu_util, _cpu_cap));
	}

	return em_cpu_energy(pd->em_pd, max_util, sum_util, _cpu_cap);
}

/*
 * find_energy_efficient_cpu(): Find most energy-efficient target CPU for the
 * waking task. find_energy_efficient_cpu() looks for the CPU with maximum
 * spare capacity in each performance domain and uses it as a potential
 * candidate to execute the task. Then, it uses the Energy Model to figure
 * out which of the CPU candidates is the most energy-efficient.
 *
 * The rationale for this heuristic is as follows. In a performance domain,
 * all the most energy efficient CPU candidates (according to the Energy
 * Model) are those for which we'll request a low frequency. When there are
 * several CPUs for which the frequency request will be the same, we don't
 * have enough data to break the tie between them, because the Energy Model
 * only includes active power costs. With this model, if we assume that
 * frequency requests follow utilization (e.g. using schedutil), the CPU with
 * the maximum spare capacity in a performance domain is guaranteed to be among
 * the best candidates of the performance domain.
 *
 * In practice, it could be preferable from an energy standpoint to pack
 * small tasks on a CPU in order to let other CPUs go in deeper idle states,
 * but that could also hurt our chances to go cluster idle, and we have no
 * ways to tell with the current Energy Model if this is actually a good
 * idea or not. So, find_energy_efficient_cpu() basically favors
 * cluster-packing, and spreading inside a cluster. That should at least be
 * a good thing for latency, and this is consistent with the idea that most
 * of the energy savings of EAS come from the asymmetry of the system, and
 * not so much from breaking the tie between identical CPUs. That's also the
 * reason why EAS is enabled in the topology code only for systems where
 * SD_ASYM_CPUCAPACITY is set.
 *
 * NOTE: Forkees are not accepted in the energy-aware wake-up path because
 * they don't have any useful utilization data yet and it's not possible to
 * forecast their impact on energy consumption. Consequently, they will be
 * placed by find_idlest_cpu() on the least loaded CPU, which might turn out
 * to be energy-inefficient in some use-cases. The alternative would be to
 * bias new tasks towards specific types of CPUs first, or to try to infer
 * their util_avg from the parent task, but those heuristics could hurt
 * other use-cases too. So, until someone finds a better way to solve this,
 * let's keep things simple by re-using the existing slow path.
 */
/*
 * IAMROOT. 2023.05.20:
 * - google-translate
 * find_energy_efficient_cpu(): 깨우기 작업을 위한 가장 에너지 효율적인 대상 CPU를
 * 찾습니다. find_energy_efficient_cpu()는 각 성능 도메인에서 최대 예비 용량을 가진
 * CPU를 찾고 이를 잠재적인 후보로 사용하여 작업을 실행합니다. 그런 다음 에너지
 * 모델을 사용하여 가장 에너지 효율적인 CPU 후보를 파악합니다.
 *
 * 이 휴리스틱의 근거는
 * 다음과 같습니다. 성능 영역에서 가장 에너지 효율적인 모든 CPU 후보(에너지 모델에
 * 따라)는 저주파를 요청하는 후보입니다. 주파수 요청이 동일한 여러 CPU가 있는 경우
 * 에너지 모델에는 활성 전력 비용만 포함되기 때문에 이들 사이의 관계를 끊을 만큼
 * 충분한 데이터가 없습니다. 이 모델에서 주파수 요청이 사용률(예: schedutil 사용)을
 * 따른다고 가정하면 성능 도메인에서 최대 예비 용량을 가진 CPU가 성능 도메인의
 * 최상의 후보 중 하나가 되도록 보장됩니다.
 *
 * 실제로 다른 CPU가 더 깊은 유휴 상태가
 * 되도록 하기 위해 CPU에 작은 작업을 압축하는 것이 에너지 관점에서 바람직할 수
 * 있지만, 이는 클러스터 유휴 상태가 될 기회를 손상시킬 수 있으며 우리는 이를 알 수
 * 있는 방법이 없습니다. 이것이 실제로 좋은 생각인지 아닌지 현재 에너지
 * 모델. 따라서 find_energy_efficient_cpu()는 기본적으로 클러스터 패킹과 클러스터
 * 내부 확산을 선호합니다. 이는 대기 시간에 적어도 좋은 일이 되어야 하며, 이는
 * EAS의 에너지 절약의 대부분이 시스템의 비대칭성에서 비롯되며 동일한 CPU 간의
 * 연결을 끊는 데서 오는 것이 아니라는 생각과 일치합니다. 이는
 * SD_ASYM_CPUCAPACITY가 설정된 시스템에 대해서만 토폴로지 코드에서 EAS가
 * 활성화되는 이유이기도 합니다.
 *
 * 참고: 포크는 아직 유용한 활용 데이터가 없고 에너지
 * 소비에 미치는 영향을 예측할 수 없기 때문에 에너지 인식 웨이크업 경로에서
 * 허용되지 않습니다. 결과적으로 그들은 가장 로드가 적은 CPU에서
 * find_idlest_cpu()에 의해 배치될 것이며 일부 사용 사례에서 에너지 비효율적인
 * 것으로 판명될 수 있습니다. 대안은 새 작업을 특정 유형의 CPU로 먼저 편향시키거나
 * 상위 작업에서 util_avg를 추론하는 것이지만 이러한 휴리스틱은 다른 사용 사례에도
 * 해를 끼칠 수 있습니다. 따라서 누군가 이 문제를 해결할 수 있는 더 나은 방법을
 * 찾을 때까지 기존 느린 경로를 재사용하여 작업을 단순하게 유지하겠습니다.
 */
/*
 * IAMROOT, 2023.05.20:
 * - pd(perf_domain) build 과정 예. partition_sched_domains_locked 주석 참조.
 *   build_perf_domains
 *     ▶ partition_sched_domains_locked (topology.c:3622)
 *       ▼ partition_and_rebuild_sched_domains (cpuset.c:1006)
 *       ▼ partition_sched_domains (topology.c:3770)
 *
 * ---------------
 *  - 절전을 위해선 빠른 응답, 에너지효율, 높은성능이 필요하다.
 *    그걸 위해서 느린 반응속도인 PELT를 안하고 상승시 4배, 하향시 8배 빠른
 *    WALT로 바꾸고, uclamp을 통해 적절한 core를 찾게 하는 방식을 사용한다.
 *
 * - 에너지이득 : cpufreq(DVFS)
 *   빠른응답 : WLAT
 *   높은 성능 : EAS
 *
 * - EAS는 다음과 같은 컴포넌트를 가진다.
 *   > EAS Core
 *    스케쥴러 내에서 에너지 모델로 동작하는 Task Placement 로직
 *    WALT 또는 PELT
 *   > schedutil
 *    스케쥴러 결합된 새로운 CPUFreq(DVFS) based governor
 *   > CPUidle
 *    스케쥴러 결합된 새로운 CPUIdle based governor
 *   > UtilClamp(uclamp)
 *    Task Placement와 schedutil에 영향을 준다. 기존 SchedTune을 대체하여 사용한다.
 *
 * - Wake 밸런스에 깨우는 방법들
 *   1. shallowest idle - state : C0(얕은 잠. ex, wfi)를 먼저 선택한다.
 *   2. driven DVFS : freq를 변경해서 해결거나 깨우거나 한다.
 * ---------------
 *  
 * @prev_cpu sleep을 햇던 cpu
 * - ex)
 *    pd0          pd1(max 못찾음)  pd2         pd3(max 못찾음)
 *    cpu0<-max1   cpu2<-prev       cpu4<-max2  cpu6
 *    cpu1         cpu3             cpu5        cpu7
 *    > max를 찾은 pd0, pd2과 prev가 있는 pd1만을 대상으로 base_energy_pd값을 계산
 * - 전력이득이 있는 cpu를 고른다.
 *    > prev가 있는 pd는 prev_delta를 산출하고, best_delta min비교해서 갱신한다.
 *    > max값이 있는 pd는 best_delta와 현재 pd를 min비교해서 best_delta, 
 *      best_energy_cpu를 min 비교 갱신한다.
 *    > 최종적으로, 6%이상의 이득이 있으면 best_energy_cpu를 선택하고, 그게 아니면
 *      prev_cpu로 유지한다.
 */
static int find_energy_efficient_cpu(struct task_struct *p, int prev_cpu)
{
	unsigned long prev_delta = ULONG_MAX, best_delta = ULONG_MAX;
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;
	int cpu, best_energy_cpu = prev_cpu, target = -1;
	unsigned long cpu_cap, util, base_energy = 0;
	struct sched_domain *sd;
	struct perf_domain *pd;

	rcu_read_lock();
	pd = rcu_dereference(rd->pd);
	if (!pd || READ_ONCE(rd->overutilized))
		goto unlock;

	/*
	 * Energy-aware wake-up happens on the lowest sched_domain starting
	 * from sd_asym_cpucapacity spanning over this_cpu and prev_cpu.
	 */
	/*
	 * IAMROOT. 2023.06.02:
	 * - google-translate
	 * 에너지 인식 웨이크업은 this_cpu 및 prev_cpu에 걸쳐 있는
	 * sd_asym_cpucapacity에서 시작하여 가장 낮은 sched_domain에서 발생합니다.
	 * - 현재 cpu에 대하여 가장 낮은 레벨의 비대칭 구조 sd를 찾는다.
	 */
	sd = rcu_dereference(*this_cpu_ptr(&sd_asym_cpucapacity));
/*
 * IAMROOT, 2023.06.03:
 * - @prev_cpu가 위에서 구한 비대칭 구조 sd에 없다면 sd를 parent로 올라가면서 찾는다.
 */
	while (sd && !cpumask_test_cpu(prev_cpu, sched_domain_span(sd)))
		sd = sd->parent;
	if (!sd)
		goto unlock;

	/*
	 * IAMROOT, 2023.06.04:
	 * - prev_cpu가 포함된 비대칭 구조 sd(ex.B,M,L구조)를 찾았다면
	 *   prev_cpu를 이 함수의 반환값인 target으로 지정해 둔다.
	 */
	target = prev_cpu;

/*
 * IAMROOT, 2023.06.03:
 * - 경과시간만큼 decay
 */
	sync_entity_load_avg(&p->se);
/*
 * IAMROOT, 2023.06.03:
 * - sleep전에 load가 없엇으면 out. EAS포기.
 */
	if (!task_util_est(p))
		goto unlock;

/*
 * IAMROOT, 2023.06.03:
 * - rd에 속한 pd 순회 -> pd에 속한 cpu순회
 */
	for (; pd; pd = pd->next) {
		unsigned long cur_delta, spare_cap, max_spare_cap = 0;
		bool compute_prev_delta = false;
		unsigned long base_energy_pd;
		int max_spare_cap_cpu = -1;

/*
 * IAMROOT, 2023.06.03:
 * - pd, sd에 둘다 포함되는 cpu and @p에 허용된 cpu를 iterate 한다.
 */
		for_each_cpu_and(cpu, perf_domain_span(pd), sched_domain_span(sd)) {
			if (!cpumask_test_cpu(cpu, p->cpus_ptr))
				continue;

			util = cpu_util_next(cpu, p, cpu);
			cpu_cap = capacity_of(cpu);
			spare_cap = cpu_cap;
/*
 * IAMROOT, 2023.06.03:
 * - 예상 사용량(util)을 빼서, 남아있는 capa를 계산(spare_cap)한다.
 */
			lsub_positive(&spare_cap, util);

			/*
			 * Skip CPUs that cannot satisfy the capacity request.
			 * IOW, placing the task there would make the CPU
			 * overutilized. Take uclamp into account to see how
			 * much capacity we can get out of the CPU; this is
			 * aligned with sched_cpu_util().
			 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   capacity 요청을 충족할 수 없는 CPU는 건너뜁니다.
 *   IOW, 거기에 작업을 배치하면 CPU가 과도하게 사용됩니다. uclamp를 
 *   고려하여 CPU에서 얼마나 많은 용량을 얻을 수 있는지 확인하십시오. 
 *   이것은 sched_cpu_util()과 일치합니다.
 *
 * - 가장 여유로은 pd를 찾고, 그 pd안에서 가장 여유로운 cpu를 찾는다.
 * - @p가 uclamp를 사용할 경우 clamp 한다.
 */
			util = uclamp_rq_util_with(cpu_rq(cpu), util, p);
/*
 * IAMROOT, 2023.06.03:
 * - util을 1.2배 해 적합한지 판별한다.
 */
			if (!fits_capacity(util, cpu_cap))
				continue;
/*
 * IAMROOT, 2023.06.03:
 * - 이전 cpu가 대상 cpu에도 포함되어 대상이 된경우, compute_prev_delta를 
 *   true로 한다.
 */
			if (cpu == prev_cpu) {
				/* Always use prev_cpu as a candidate. */
				compute_prev_delta = true;
			} else if (spare_cap > max_spare_cap) {
				/*
				 * Find the CPU with the maximum spare capacity
				 * in the performance domain.
				 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   현재 순회중인 pd에서 최대 여유 용량이 있는 CPU를 찾습니다.
 * - max값 갱신.
 */
				max_spare_cap = spare_cap;
				max_spare_cap_cpu = cpu;
			}
/*
 * IAMROOT, 2023.06.03:
 * - for문 종료
 */
		}

/*
 * IAMROOT, 2023.06.03:
 * - pd, sd, @p->cpus와 겹치는 span의 cpus의 iterate가 끝낫는데 
 *   max_spare_cap_cpu을 못찾은 pd는 skip한다. 단 prev_cpu가 있는 pd는
 *   skip하지 않는다.
 */
		if (max_spare_cap_cpu < 0 && !compute_prev_delta)
			continue;

		/* Compute the 'base' energy of the pd, without @p */
/*
 * IAMROOT, 2023.06.03:
 * - base를 일단계산해 base끼리 합산을 한다.
 */
		base_energy_pd = compute_energy(p, -1, pd);
		base_energy += base_energy_pd;

		/* Evaluate the energy impact of using prev_cpu. */
/*
 * IAMROOT, 2023.06.03:
 * - prev cpu가 있는 pd. prev_cpu로 energy를 계산하는데,
 *   migrate를 안하는게 이득이라고 판단하면, 완전 best를 찾으러 가는 것보다
 *   prev_cpu로 그냥 한다.
 */
		if (compute_prev_delta) {
			/*
			 * IAMROOT, 2023.06.07:
			 * - @p가 @prev_cpu로 이동했을때 @pd의 예상 에너지 총합 계산
			 */
			prev_delta = compute_energy(p, prev_cpu, pd);
/*
 * IAMROOT, 2023.06.03:
 * - migrate를 하지 않는게 더 energy 이득이라면 종료한다.
 */
			if (prev_delta < base_energy_pd)
				goto unlock;
/*
 * IAMROOT, 2023.06.03:
 * - prev_cpu가 현재 pd의 energy의 차이값을 best_delta에 min비교하여 저장한다.
 */
			prev_delta -= base_energy_pd;
			best_delta = min(best_delta, prev_delta);
		}

		/* Evaluate the energy impact of using max_spare_cap_cpu. */
		if (max_spare_cap_cpu >= 0) {
			/*
			 * IAMROOT, 2023.06.07:
			 * - @p가 @max_spare_cap_cpu로 이동했을때 @pd의
			 *   예상 에너지 총합 계산
			 */
			cur_delta = compute_energy(p, max_spare_cap_cpu, pd);
/*
 * IAMROOT, 2023.06.03:
 * - 여기서도 위에처럼 이득만 본다면 바로 out(예외처리 개념)
 */
			if (cur_delta < base_energy_pd)
				goto unlock;
/*
 * IAMROOT, 2023.06.03:
 * - 전력량이 상승하는 상태. 최소한의 상승인것을 우선순위에한다.
 */
			cur_delta -= base_energy_pd;
			if (cur_delta < best_delta) {
				best_delta = cur_delta;
				best_energy_cpu = max_spare_cap_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * Pick the best CPU if prev_cpu cannot be used, or if it saves at
	 * least 6% of the energy used by prev_cpu.
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   prev_cpu를 사용할 수 없거나 prev_cpu에서 사용하는 에너지의 6% 이상을 절약하는 
 *   경우 최상의 CPU를 선택합니다.
. 
 * - 여기 if문에서 target이 안바뀌면 prev_cpu가 target으로 된다.
 * - prev_delta == ULONG_MAX. 즉 prev_cpu가 한번도 대상이 된적없음.
 *   1. best_energy_cpu가 찾아졌었으면 그것이 cpu가 된다.
 *   2. best_energy_cpu도 찾은적이 한번도 없으면 prev_cpu가 그냥 target이된다.
 *
 * - (prev_delta - best_delta) > ((prev_delta + base_energy) >> 4)
 *   즉 prev_cpu가 찾아졌었다.
 *   prev_cpu에서보다 에너지절약이 6%이상이 되는 경우 해당 target을 return한다.
 * - prev_delta - best_delta
 *   옮긴후의 이득되는 delta
 * - (prev_delta + base_energy) >> 4
 *   옮기기전의 energy 총합의 6%
 */
	if ((prev_delta == ULONG_MAX) ||
	    (prev_delta - best_delta) > ((prev_delta + base_energy) >> 4))
		target = best_energy_cpu;

	return target;

unlock:
	rcu_read_unlock();

/*
 * IAMROOT, 2023.06.03:
 * - return prev_cpu
 */
	return target;
}

/*
 * select_task_rq_fair: Select target runqueue for the waking task in domains
 * that have the relevant SD flag set. In practice, this is SD_BALANCE_WAKE,
 * SD_BALANCE_FORK, or SD_BALANCE_EXEC.
 *
 * Balances load by selecting the idlest CPU in the idlest group, or under
 * certain conditions an idle sibling CPU if the domain has SD_WAKE_AFFINE set.
 *
 * Returns the target CPU number.
 */
/*
 * IAMROOT. 2023.05.20:
 * - google-translate
 * select_task_rq_fair: 관련 SD 플래그가 설정된 도메인에서 깨우기 작업에 대한 대상
 * 실행 대기열을 선택합니다. 실제로 이것은 SD_BALANCE_WAKE, SD_BALANCE_FORK 또는
 * SD_BALANCE_EXEC입니다.
 *
 * 가장 유휴 그룹에서 가장 유휴 CPU를 선택하거나 도메인에 SD_WAKE_AFFINE이 설정된 경우
 * 특정 조건에서 유휴 형제 CPU를 선택하여 로드 균형을 조정합니다.
 *
 * 대상 CPU 번호를 반환합니다.
 * - TTWU, cache친화
 *   1. wakee 기록
 *   2. EAS. 전력효율기준으로 선택.
 *   --- EAS 미지원 ---
 *   3. cache 친화여부 기록(want affine)
 *   4. SD_WAKE_AFFINE를 지원하는 sd에서 cache친화 new_cpu(prev or this)를 얻어옴
 *   5. new_cpu를 얻었다면 new_cpu, prev_cpu를 기준으로 cache 친화중에서 중에서 고른다.
 *     (select_idle_sibling())
 *
 * - TTWU가 아니거나, EAS 미지원, cache친화가 아님.
 *   1. 최상위 sd를 찾는다.
 *   2. 최상위 sd부터 slowpath로 idle cpu를 찾는다.
 */
static int
select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags)
{
	/*
	 * IAMROOT, 2023.05.20:
	 * - sync : 종료중이 아니고 WF_SYNC flags 가 설정되었을때 1
	 */
	int sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	struct sched_domain *tmp, *sd = NULL;
	int cpu = smp_processor_id();
	int new_cpu = prev_cpu;
	int want_affine = 0;
	/* SD_flags and WF_flags share the first nibble */
	int sd_flag = wake_flags & 0xF;

	/*
	 * required for stable ->cpus_allowed
	 */
	lockdep_assert_held(&p->pi_lock);
/*
 * IAMROOT, 2023.05.23:
 * - try_to_wake_up()에서 진입.
 */
	if (wake_flags & WF_TTWU) {
		record_wakee(p);

/*
 * IAMROOT, 2023.05.26:
 * - EAS -> cache affinity의 순서로 cpu를 찾아본다.
 */
		if (sched_energy_enabled()) {
			new_cpu = find_energy_efficient_cpu(p, prev_cpu);
			if (new_cpu >= 0)
				return new_cpu;
			/*
			 * IAMROOT, 2023.06.07:
			 * - prev_cpu가 포함된 asym_cap 을 못찾은 경우
			 */
			new_cpu = prev_cpu;
		}
/*
 * IAMROOT, 2023.06.03:
 * - cache 친화여부를 알아온다.
 */
		want_affine = !wake_wide(p) && cpumask_test_cpu(cpu, p->cpus_ptr);
	}

	rcu_read_lock();
	for_each_domain(cpu, tmp) {
		/*
		 * If both 'cpu' and 'prev_cpu' are part of this domain,
		 * cpu is a valid SD_WAKE_AFFINE target.
		 */
/*
 * IAMROOT, 2023.06.03:
 * - cache친화를 찾아볼려고 노력을 하는상황. sd도 affine을 지원하고,
 *   prev_cpu도 포함되어있다. 즉 affine가능한 상태다.
 *   이때 대상 cpu가 prev_cpu 아니라면, 대상 cpu로 결정할지 정한다.
 *   new_cpu는 prev_cpu or cpu로 무조건 골라진다.
 */
		if (want_affine && (tmp->flags & SD_WAKE_AFFINE) &&
		    cpumask_test_cpu(prev_cpu, sched_domain_span(tmp))) {
			if (cpu != prev_cpu)
				new_cpu = wake_affine(tmp, p, cpu, prev_cpu, sync);

			sd = NULL; /* Prefer wake_affine over balance flags */
			break;
		}

/*
 * IAMROOT, 2023.06.03:
 * - sd_flag와 겹치는 tmp sd를 sd에서 저장해놓는다. 만약 
 *   못찾았는데, want_affine조차 없엇으면 그냥 종료한다.
 */
		if (tmp->flags & sd_flag)
			sd = tmp;
		else if (!want_affine)
			break;
	}

/*
 * IAMROOT, 2023.06.03:
 * - want_affine이 없었거나, affine flag가 있는 sd를 못찾았고 sd_flag와 겹치는
 *   sd가 있다.
 */
	if (unlikely(sd)) {
		/* Slow path */
/*
 * IAMROOT, 2023.06.15:
 * - sd는 sd_flag가 있는 최상위일 것이다.
 */
		new_cpu = find_idlest_cpu(sd, p, cpu, prev_cpu, sd_flag);
	} else if (wake_flags & WF_TTWU) { /* XXX always ? */
		/* Fast path */
		/*
		 * IAMROOT, 2023.06.10:
		 * - 캐쉬 친화 cpu들 중 idle cpu를 찾는다. 찾는 순서는
		 *   1. new_cpu
		 *   2. prev_cpu
		 *      (2.5 pcpu kthread)
		 *   3. recent_used_cpu
		 *   4. HMP
		 *   5. LLC
		 */
		new_cpu = select_idle_sibling(p, prev_cpu, new_cpu);
	}
	rcu_read_unlock();

	return new_cpu;
}

static void detach_entity_cfs_rq(struct sched_entity *se);

/*
 * Called immediately before a task is migrated to a new CPU; task_cpu(p) and
 * cfs_rq_of(p) references at time of call are still valid and identify the
 * previous CPU. The caller guarantees p->pi_lock or task_rq(p)->lock is held.
 */
static void migrate_task_rq_fair(struct task_struct *p, int new_cpu)
{
	/*
	 * As blocked tasks retain absolute vruntime the migration needs to
	 * deal with this by subtracting the old and adding the new
	 * min_vruntime -- the latter is done by enqueue_entity() when placing
	 * the task on the new runqueue.
	 */
	if (READ_ONCE(p->__state) == TASK_WAKING) {
		struct sched_entity *se = &p->se;
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		u64 min_vruntime;

#ifndef CONFIG_64BIT
		u64 min_vruntime_copy;

		do {
			min_vruntime_copy = cfs_rq->min_vruntime_copy;
			smp_rmb();
			min_vruntime = cfs_rq->min_vruntime;
		} while (min_vruntime != min_vruntime_copy);
#else
		min_vruntime = cfs_rq->min_vruntime;
#endif

		se->vruntime -= min_vruntime;
	}

	if (p->on_rq == TASK_ON_RQ_MIGRATING) {
		/*
		 * In case of TASK_ON_RQ_MIGRATING we in fact hold the 'old'
		 * rq->lock and can modify state directly.
		 */
		lockdep_assert_rq_held(task_rq(p));
		detach_entity_cfs_rq(&p->se);

	} else {
		/*
		 * We are supposed to update the task to "current" time, then
		 * its up to date and ready to go to new CPU/cfs_rq. But we
		 * have difficulty in getting what current time is, so simply
		 * throw away the out-of-date time. This will result in the
		 * wakee task is less decayed, but giving the wakee more load
		 * sounds not bad.
		 */
		remove_entity_load_avg(&p->se);
	}

	/* Tell new CPU we are migrated */
	p->se.avg.last_update_time = 0;

	/* We have migrated, no longer consider this task hot */
	p->se.exec_start = 0;

	update_scan_period(p, new_cpu);
}

static void task_dead_fair(struct task_struct *p)
{
	remove_entity_load_avg(&p->se);
}

/*
 * IAMROOT, 2023.05.03:
 * - ING
 */
static int
balance_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	if (rq->nr_running)
		return 1;

	return newidle_balance(rq, rf) != 0;
}
#endif /* CONFIG_SMP */

/*
 * IAMROOT, 2023.01.28:
 * - @se의 weight를 적용한 gran값을 구한다.
 */
static unsigned long wakeup_gran(struct sched_entity *se)
{
	unsigned long gran = sysctl_sched_wakeup_granularity;

	/*
	 * Since its curr running now, convert the gran from real-time
	 * to virtual-time in his units.
	 *
	 * By using 'se' instead of 'curr' we penalize light tasks, so
	 * they get preempted easier. That is, if 'se' < 'curr' then
	 * the resulting gran will be larger, therefore penalizing the
	 * lighter, if otoh 'se' > 'curr' then the resulting gran will
	 * be smaller, again penalizing the lighter task.
	 *
	 * This is especially important for buddies when the leftmost
	 * task is higher priority than the buddy.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   이제 curr가 실행되기 때문에 gran을 실시간에서 가상 시간으로 단위로 변환하십시오.
 *
 *   'curr' 대신 'se'를 사용하여 가벼운 작업에 페널티를 주어 더 쉽게 선점할 수 있습니다.
 *   즉, 'se' < 'curr'이면 결과 gran이 더 커지므로 더 가벼운 작업에 페널티를 주고,
 *   otoh 'se' > 'curr'이면 결과 gran이 더 작아지므로 더 가벼운 작업에 페널티를 줍니다.
 *
 *   이것은 가장 왼쪽 작업이 버디보다 우선순위가 높을 때 버디에게 특히 중요합니다.
 */
	return calc_delta_fair(gran, se);
}

/*
 * Should 'se' preempt 'curr'.
 *
 *             |s1
 *        |s2
 *   |s3
 *         g
 *      |<--->|c
 *
 *  w(c, s1) = -1
 *  w(c, s2) =  0
 *  w(c, s3) =  1
 *
 */
/*
 * IAMROOT, 2023.01.28:
 * - @return -1 : curr의 vruntime이 se vruntime 보다 작다
 *            1 : @se gran값보다 diff가 크면, 즉 충분한 시간차가 있으면
 *            0 : @se gran값보다 diff가 작으면, 즉 gran 이내의 시간이면(너무 짧은시간)
 *
 * ------------
 *
 * - curr |  se   : -1
 *
 * -      gran
 *   se |  curr | : 0
 *
 * -      gran
 *   se |      |  curr : 1
 */
static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se)
{
	s64 gran, vdiff = curr->vruntime - se->vruntime;

	if (vdiff <= 0)
		return -1;

	gran = wakeup_gran(se);
	if (vdiff > gran)
		return 1;

	return 0;
}

static void set_last_buddy(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		if (SCHED_WARN_ON(!se->on_rq))
			return;
		if (se_is_idle(se))
			return;
		cfs_rq_of(se)->last = se;
	}
}

static void set_next_buddy(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		if (SCHED_WARN_ON(!se->on_rq))
			return;
		if (se_is_idle(se))
			return;
		cfs_rq_of(se)->next = se;
	}
}

static void set_skip_buddy(struct sched_entity *se)
{
	for_each_sched_entity(se)
		cfs_rq_of(se)->skip = se;
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
/*
 * IAMROOT. 2023.05.20:
 * - google-translate
 * 필요한 경우 새로 깨어난 작업으로 현재 작업을 선점합니다.
 * - TODO
 */
static void check_preempt_wakeup(struct rq *rq, struct task_struct *p, int wake_flags)
{
	struct task_struct *curr = rq->curr;
	struct sched_entity *se = &curr->se, *pse = &p->se;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);
	int scale = cfs_rq->nr_running >= sched_nr_latency;
	int next_buddy_marked = 0;
	int cse_is_idle, pse_is_idle;

/*
 * IAMROOT, 2023.05.23:
 * - 이미 curr. 할의미가 없다.
 */
	if (unlikely(se == pse))
		return;

	/*
	 * This is possible from callers such as attach_tasks(), in which we
	 * unconditionally check_preempt_curr() after an enqueue (which may have
	 * lead to a throttle).  This both saves work and prevents false
	 * next-buddy nomination below.
	 */
/*
 * IAMROOT, 2023.05.23:
 * - papago
 *   이것은 attach_tasks()와 같은 호출자에서 가능합니다. 여기서 우리는 
 *   enqueue(스로틀로 이어질 수 있음) 후에 무조건적으로 check_preempt_curr()를 
 *   수행합니다. 이것은 작업을 저장하고 아래에서 잘못된 다음 친구 지명을 
 *   방지합니다.
 * - group이 이미 throttle중이라면 깨어나봤자 의미가 없기때문에 return.
 */
	if (unlikely(throttled_hierarchy(cfs_rq_of(pse))))
		return;

	/*
	 * IAMROOT, 2023.05.20:
	 * - WF_FORK flags 가 없으면 curr의 다음 task 를 pse로 한다.
	 * - curr다음에 많은 task가 대기중인경우(scale = 1), fork가 아니라면,
	 *   buddy를 지원하는 경우 curr의 next로 설정해 다음에 즉시 wakeup
	 *   되게 한다.
	 */
	if (sched_feat(NEXT_BUDDY) && scale && !(wake_flags & WF_FORK)) {
		set_next_buddy(pse);
		next_buddy_marked = 1;
	}

	/*
	 * We can come here with TIF_NEED_RESCHED already set from new task
	 * wake up path.
	 *
	 * Note: this also catches the edge-case of curr being in a throttled
	 * group (e.g. via set_curr_task), since update_curr() (in the
	 * enqueue of curr) will have resulted in resched being set.  This
	 * prevents us from potentially nominating it as a false LAST_BUDDY
	 * below.
	 */
/*
 * IAMROOT, 2023.05.23:
 * - papago
 *   새 작업 깨우기 경로에서 이미 설정된 TIF_NEED_RESCHED를 사용하여 여기로 
 *   올 수 있습니다.
 *
 *   Note: 이것은 또한 조절된 그룹(예: set_curr_task를 통해)에 있는 curr의 
 *   엣지 케이스를 포착합니다. 이렇게 하면 잠재적으로 아래에서 잘못된 
 *   LAST_BUDDY로 지명하는 것을 방지할 수 있습니다.
 *
 * - 이미 reschedule 요청이 있다.
 */
	if (test_tsk_need_resched(curr))
		return;

	/* Idle tasks are by definition preempted by non-idle tasks. */
/*
 * IAMROOT, 2023.05.24:
 * - papago
 *   유휴 작업은 정의에 따라 유휴 작업이 아닌 작업에 의해 선점됩니다. 
 */
	if (unlikely(task_has_idle_policy(curr)) &&
	    likely(!task_has_idle_policy(p)))
		goto preempt;

	/*
	 * Batch and idle tasks do not preempt non-idle tasks (their preemption
	 * is driven by the tick):
	 */
/*
 * IAMROOT, 2023.05.24:
 * - papago
 *   배치 및 유휴 작업은 유휴가 아닌 작업을 선점하지 않습니다(선점은 틱에 
 *   의해 구동됨).
 */
	if (unlikely(p->policy != SCHED_NORMAL) || !sched_feat(WAKEUP_PREEMPTION))
		return;

	find_matching_se(&se, &pse);
	BUG_ON(!pse);

	cse_is_idle = se_is_idle(se);
	pse_is_idle = se_is_idle(pse);

	/*
	 * Preempt an idle group in favor of a non-idle group (and don't preempt
	 * in the inverse case).
	 */
/*
 * IAMROOT, 2023.05.24:
 * - papago
 *   유휴가 아닌 그룹을 위해 유휴 그룹을 선점합니다(반대의 경우 선점하지 않음).
 */
	if (cse_is_idle && !pse_is_idle)
		goto preempt;
	if (cse_is_idle != pse_is_idle)
		return;

	update_curr(cfs_rq_of(se));
	if (wakeup_preempt_entity(se, pse) == 1) {
		/*
		 * Bias pick_next to pick the sched entity that is
		 * triggering this preemption.
		 */
/*
 * IAMROOT, 2023.05.24:
 * - papago
 *   이 선점을 트리거하는 sched 엔터티를 선택하려면 pick_next를 바이어스합니다.
 */
		if (!next_buddy_marked)
			set_next_buddy(pse);
		goto preempt;
	}

	return;

preempt:
	resched_curr(rq);
	/*
	 * Only set the backward buddy when the current task is still
	 * on the rq. This can happen when a wakeup gets interleaved
	 * with schedule on the ->pre_schedule() or idle_balance()
	 * point, either of which can * drop the rq lock.
	 *
	 * Also, during early boot the idle thread is in the fair class,
	 * for obvious reasons its a bad idea to schedule back to it.
	 */
/*
 * IAMROOT, 2023.05.24:
 * - papago
 *   현재 작업이 여전히 rq에 있는 경우에만 역방향 버디를 설정하십시오. 
 *   이것은 웨이크업이 ->pre_schedule() 또는 idle_balance() 지점에서 
 *   일정과 인터리브될 때 발생할 수 있으며, 둘 중 하나가 
 *   rq 잠금을 * 해제할 수 있습니다.
 *
 *   또한 초기 부팅 중에 유휴 스레드는 공정한 클래스에 속하며, 
 *   분명한 이유 때문에 다시 예약하는 것은 좋지 않습니다.
 */
	if (unlikely(!se->on_rq || curr == rq->idle))
		return;

	if (sched_feat(LAST_BUDDY) && scale && entity_is_task(se))
		set_last_buddy(se);
}

#ifdef CONFIG_SMP
static struct task_struct *pick_task_fair(struct rq *rq)
{
	struct sched_entity *se;
	struct cfs_rq *cfs_rq;

again:
	cfs_rq = &rq->cfs;
	if (!cfs_rq->nr_running)
		return NULL;

	do {
		struct sched_entity *curr = cfs_rq->curr;

		/* When we pick for a remote RQ, we'll not have done put_prev_entity() */
		if (curr) {
			if (curr->on_rq)
				update_curr(cfs_rq);
			else
				curr = NULL;

			if (unlikely(check_cfs_rq_runtime(cfs_rq)))
				goto again;
		}

		se = pick_next_entity(cfs_rq, curr);
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	return task_of(se);
}
#endif

/*
 * IAMROOT, 2023.01.28:
 * - cfs_rq의 curr의 next를 선택한다. 선택이 되면 @prev를 put, 선택된 next를 curr로 계층구조로
 *   순환하며 설정한다. 선택된 next의 task를 return한다.
 */
struct task_struct *
pick_next_task_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct cfs_rq *cfs_rq = &rq->cfs;
	struct sched_entity *se;
	struct task_struct *p;
	int new_tasks;

again:
	if (!sched_fair_runnable(rq))
		goto idle;

#ifdef CONFIG_FAIR_GROUP_SCHED
	if (!prev || prev->sched_class != &fair_sched_class)
		goto simple;

	/*
	 * Because of the set_next_buddy() in dequeue_task_fair() it is rather
	 * likely that a next task is from the same cgroup as the current.
	 *
	 * Therefore attempt to avoid putting and setting the entire cgroup
	 * hierarchy, only change the part that actually changes.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   dequeue_task_fair()의 set_next_buddy() 때문에 다음 작업이 현재 작업과 동일한 cgroup에서
 *   나올 가능성이 높습니다.
 *
 *   따라서 전체 cgroup 계층 구조를 넣거나 설정하는 것을 피하고 실제로 변경되는 부분만
 *   변경하십시오.
 *
 * - 계층구조로 내려가면서 curr의 next를 순회한다. 최종적으로 마지막 next로 선택된것을
 *   se로 선택될것이고, 이것은 task가 된다.
 */
	do {
		struct sched_entity *curr = cfs_rq->curr;

		/*
		 * Since we got here without doing put_prev_entity() we also
		 * have to consider cfs_rq->curr. If it is still a runnable
		 * entity, update_curr() will update its vruntime, otherwise
		 * forget we've ever seen it.
		 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   put_prev_entity()를 수행하지 않고 여기에 왔기 때문에 cfs_rq->curr도 고려해야 합니다.
 *   여전히 실행 가능한 엔터티인 경우 update_curr()는 vruntime을 업데이트하고, 그렇지 않으면
 *   우리가 본 적이 있다는 사실을 잊어버립니다.
 */
		if (curr) {

/*
 * IAMROOT, 2023.01.28:
 * - curr entity가 rq에 안들어가있으면 null로 설정.
 */
			if (curr->on_rq)
				update_curr(cfs_rq);
			else
				curr = NULL;

			/*
			 * This call to check_cfs_rq_runtime() will do the
			 * throttle and dequeue its entity in the parent(s).
			 * Therefore the nr_running test will indeed
			 * be correct.
			 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   check_cfs_rq_runtime()에 대한 이 호출은 스로틀링을 수행하고 상위 항목의 대기열에서
 *   제외합니다.
 *   따라서 nr_running 테스트는 실제로 정확합니다.
 * - throttle인지 확인한다.
 */
			if (unlikely(check_cfs_rq_runtime(cfs_rq))) {
				cfs_rq = &rq->cfs;

				if (!cfs_rq->nr_running)
					goto idle;

				goto simple;
			}
		}

		se = pick_next_entity(cfs_rq, curr);
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	p = task_of(se);

	/*
	 * Since we haven't yet done put_prev_entity and if the selected task
	 * is a different task than we started out with, try and touch the
	 * least amount of cfs_rqs.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   아직 put_prev_entity를 수행하지 않았고 선택한 작업이 시작과 다른 작업인 경우 최소량의
 *   cfs_rq를 터치해 봅니다.
 *
 * - 선택한 p(se)가 @prev가 다르다면 prev se를 put하고, 새로운 p(se)를 set한다.
 */
	if (prev != p) {
		struct sched_entity *pse = &prev->se;

/*
 * IAMROOT, 2023.01.28:
 * - se, pse가 같은 cfs_rq소속일때까지 반복하며
 *   pse를 cfs_rq에 enqueue, se를 cfs_rq에서 dequeue를 하고, se를 curr로 update한다.
 *
 *         O
 *        / \
 *       O <----same_group
 *      / \
 *     O  se
 *    / \
 *  pse ..
 */
		while (!(cfs_rq = is_same_group(se, pse))) {
			int se_depth = se->depth;
			int pse_depth = pse->depth;

/*
 * IAMROOT, 2023.01.28:
 * - pse가 se보다 더 깊거나 같은 위치. pse를 put해준다.
 * - ex)
 *         O
 *        / \
 *       O <----same_group
 *      / \
 *     O  se
 *    / \
 *  pse ..
 */
			if (se_depth <= pse_depth) {
/*
 * IAMROOT, 2023.01.28:
 * - prev se를 cfs_rq로 enqueue.
 */
				put_prev_entity(cfs_rq_of(pse), pse);
				pse = parent_entity(pse);
			}

/*
 * IAMROOT, 2023.01.28:
 * - se가 pse보다 더 깊거나 같은 위치. curr를 update해준다.
 * - ex)
 *         O
 *        / \
 *       O <----same_group
 *      / \
 *     O  pse
 *    / \
 *   se ..
 */
			if (se_depth >= pse_depth) {
/*
 * prifri, 2023.01.28:
 * - cfs_rq에 se를 dequeue하고 se를 curr로 설정한다.
 */
				set_next_entity(cfs_rq_of(se), se);
				se = parent_entity(se);
			}
		}


/*
 * IAMROOT, 2023.01.28:
 * - 최종적으로 same_group에서 만낫을때. pse, se의 처리.
 */
		put_prev_entity(cfs_rq, pse);
		set_next_entity(cfs_rq, se);
	}

	goto done;
simple:
#endif

/*
 * IAMROOT, 2023.01.28:
 * - simple인 경우는 계층구조 처리가 필요없어 간단히 처리하고 넘어간다.
 */
	if (prev)
		put_prev_task(rq, prev);

	do {
/*
 * IAMROOT, 2023.01.28:
 * - next를 pick하고 set한다.
 */
		se = pick_next_entity(cfs_rq, NULL);
		set_next_entity(cfs_rq, se);
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	p = task_of(se);

done: __maybe_unused;
#ifdef CONFIG_SMP
	/*
	 * Move the next running task to the front of
	 * the list, so our cfs_tasks list becomes MRU
	 * one.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   다음 실행 작업을 목록의 맨 앞으로 이동하여 cfs_tasks 목록이 MRU 목록이 되도록 합니다.
 */
	list_move(&p->se.group_node, &rq->cfs_tasks);
#endif

	if (hrtick_enabled_fair(rq))
		hrtick_start_fair(rq, p);

	update_misfit_status(p, rq);

/*
 * IAMROOT, 2023.01.28:
 * - next의 task가 return된다.
 */
	return p;

idle:
	if (!rf)
		return NULL;

/*
 * IAMROOT, 2023.02.11:
 * - 아래 코드들이 balancing.
 *   next task를 못고른 상태가 되어 idle이 되기전에 가져올수 있는 task를 찾는다.
 */
	new_tasks = newidle_balance(rq, rf);

	/*
	 * Because newidle_balance() releases (and re-acquires) rq->lock, it is
	 * possible for any higher priority task to appear. In that case we
	 * must re-start the pick_next_entity() loop.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * newidle_balance()는 rq->lock을 해제(및 다시 획득)하기 때문에 우선 순위가 더 높은
	 * 작업이 나타날 수 있습니다. 이 경우 pick_next_entity() 루프를 다시 시작해야
	 * 합니다.
	 * - new_tasks 가 rt 나 dl 이다.
	 */
	if (new_tasks < 0)
		return RETRY_TASK;

	if (new_tasks > 0)
		goto again;

	/*
	 * rq is about to be idle, check if we need to update the
	 * lost_idle_time of clock_pelt
	 */
	update_idle_rq_clock_pelt(rq);

	return NULL;
}

static struct task_struct *__pick_next_task_fair(struct rq *rq)
{
	return pick_next_task_fair(rq, NULL, NULL);
}

/*
 * Account for a descheduled task:
 */
static void put_prev_task_fair(struct rq *rq, struct task_struct *prev)
{
	struct sched_entity *se = &prev->se;
	struct cfs_rq *cfs_rq;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		put_prev_entity(cfs_rq, se);
	}
}

/*
 * sched_yield() is very simple
 *
 * The magic of dealing with the ->skip buddy is in pick_next_entity.
 */
static void yield_task_fair(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);
	struct sched_entity *se = &curr->se;

	/*
	 * Are we the only task in the tree?
	 */
	if (unlikely(rq->nr_running == 1))
		return;

	clear_buddies(cfs_rq, se);

	if (curr->policy != SCHED_BATCH) {
		update_rq_clock(rq);
		/*
		 * Update run-time statistics of the 'current'.
		 */
		update_curr(cfs_rq);
		/*
		 * Tell update_rq_clock() that we've just updated,
		 * so we don't do microscopic update in schedule()
		 * and double the fastpath cost.
		 */
		rq_clock_skip_update(rq);
	}

	set_skip_buddy(se);
}

static bool yield_to_task_fair(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	/* throttled hierarchies are not runnable */
	if (!se->on_rq || throttled_hierarchy(cfs_rq_of(se)))
		return false;

	/* Tell the scheduler that we'd really like pse to run next. */
	set_next_buddy(se);

	yield_task_fair(rq);

	return true;
}

#ifdef CONFIG_SMP
/**************************************************
 * Fair scheduling class load-balancing methods.
 *
 * BASICS
 *
 * The purpose of load-balancing is to achieve the same basic fairness the
 * per-CPU scheduler provides, namely provide a proportional amount of compute
 * time to each task. This is expressed in the following equation:
 *
 *   W_i,n/P_i == W_j,n/P_j for all i,j                               (1)
 *
 * Where W_i,n is the n-th weight average for CPU i. The instantaneous weight
 * W_i,0 is defined as:
 *
 *   W_i,0 = \Sum_j w_i,j                                             (2)
 *
 * Where w_i,j is the weight of the j-th runnable task on CPU i. This weight
 * is derived from the nice value as per sched_prio_to_weight[].
 *
 * The weight average is an exponential decay average of the instantaneous
 * weight:
 *
 *   W'_i,n = (2^n - 1) / 2^n * W_i,n + 1 / 2^n * W_i,0               (3)
 *
 * C_i is the compute capacity of CPU i, typically it is the
 * fraction of 'recent' time available for SCHED_OTHER task execution. But it
 * can also include other factors [XXX].
 *
 * To achieve this balance we define a measure of imbalance which follows
 * directly from (1):
 *
 *   imb_i,j = max{ avg(W/C), W_i/C_i } - min{ avg(W/C), W_j/C_j }    (4)
 *
 * We them move tasks around to minimize the imbalance. In the continuous
 * function space it is obvious this converges, in the discrete case we get
 * a few fun cases generally called infeasible weight scenarios.
 *
 * [XXX expand on:
 *     - infeasible weights;
 *     - local vs global optima in the discrete case. ]
 *
 *
 * SCHED DOMAINS
 *
 * In order to solve the imbalance equation (4), and avoid the obvious O(n^2)
 * for all i,j solution, we create a tree of CPUs that follows the hardware
 * topology where each level pairs two lower groups (or better). This results
 * in O(log n) layers. Furthermore we reduce the number of CPUs going up the
 * tree to only the first of the previous level and we decrease the frequency
 * of load-balance at each level inv. proportional to the number of CPUs in
 * the groups.
 *
 * This yields:
 *
 *     log_2 n     1     n
 *   \Sum       { --- * --- * 2^i } = O(n)                            (5)
 *     i = 0      2^i   2^i
 *                               `- size of each group
 *         |         |     `- number of CPUs doing load-balance
 *         |         `- freq
 *         `- sum over all levels
 *
 * Coupled with a limit on how many tasks we can migrate every balance pass,
 * this makes (5) the runtime complexity of the balancer.
 *
 * An important property here is that each CPU is still (indirectly) connected
 * to every other CPU in at most O(log n) steps:
 *
 * The adjacency matrix of the resulting graph is given by:
 *
 *             log_2 n
 *   A_i,j = \Union     (i % 2^k == 0) && i / 2^(k+1) == j / 2^(k+1)  (6)
 *             k = 0
 *
 * And you'll find that:
 *
 *   A^(log_2 n)_i,j != 0  for all i,j                                (7)
 *
 * Showing there's indeed a path between every CPU in at most O(log n) steps.
 * The task movement gives a factor of O(m), giving a convergence complexity
 * of:
 *
 *   O(nm log n),  n := nr_cpus, m := nr_tasks                        (8)
 *
 *
 * WORK CONSERVING
 *
 * In order to avoid CPUs going idle while there's still work to do, new idle
 * balancing is more aggressive and has the newly idle CPU iterate up the domain
 * tree itself instead of relying on other CPUs to bring it work.
 *
 * This adds some complexity to both (5) and (8) but it reduces the total idle
 * time.
 *
 * [XXX more?]
 *
 *
 * CGROUPS
 *
 * Cgroups make a horror show out of (2), instead of a simple sum we get:
 *
 *                                s_k,i
 *   W_i,0 = \Sum_j \Prod_k w_k * -----                               (9)
 *                                 S_k
 *
 * Where
 *
 *   s_k,i = \Sum_j w_i,j,k  and  S_k = \Sum_i s_k,i                 (10)
 *
 * w_i,j,k is the weight of the j-th runnable task in the k-th cgroup on CPU i.
 *
 * The big problem is S_k, its a global sum needed to compute a local (W_i)
 * property.
 *
 * [XXX write more on how we solve this.. _after_ merging pjt's patches that
 *      rewrite all of this once again.]
 */

/*
 * IAMROOT, 2023.04.29:
 * - load balance  주기는 1틱에서 최대 0.1초로 제한
 */
static unsigned long __read_mostly max_load_balance_interval = HZ/10;

/*
 * IAMROOT, 2023.05.06:
 * - fbq(first busiest queue)
 *   fbq_classify_group(),fbq_classify_rq() 참고
 *
 * - regular
 *   numa설정이 안된 task가 하나라도 있는 경우
 * - remote
 *   preferred node가 아닌 곳에서 동작하는 task가 있다.
 * - all
 *   모든 task가 preferred node에서 동작하고있다.
 */
enum fbq_type { regular, remote, all };

/*
 * 'group_type' describes the group of CPUs at the moment of load balancing.
 *
 * The enum is ordered by pulling priority, with the group with lowest priority
 * first so the group_type can simply be compared when selecting the busiest
 * group. See update_sd_pick_busiest().
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   'group_type'은 로드 밸런싱 시점의 CPU 그룹을 설명합니다.
 *
 *   enum은 가장 바쁜 그룹을 선택할 때 group_type을 간단히 비교할 수 있도록 
 *   우선 순위가 가장 낮은 그룹부터 우선 순위를 끌어서 정렬합니다. 
 *   update_sd_pick_busiest()를 참조하십시오.
 *
 *   group_has_spare :
 *   그룹에는 더 많은 작업을 실행하는 데 사용할 수 있는 여유 용량이 있습니다. 
 *
 *   group_fully_busy :
 *   그룹이 완전히 사용되고 작업이 더 많은 CPU 주기를 놓고 경쟁하지 않습니다. 
 *   그럼에도 불구하고 일부 작업은 실행되기 전에 대기할 수 있습니다.
 *
 *   group_misfit_task :
 *   SD_ASYM_CPUCAPACITY만. 하나의 작업이 CPU 용량에 맞지 않아 더 강력한 
 *   CPU로 마이그레이션해야 합니다.
 *
 *   group_asym_packing :
 *   SD_ASYM_PACKING 전용. 용량이 더 큰 하나의 로컬 CPU를 사용할 수 있으며 
 *   작업을 현재 CPU에서 실행하는 대신 여기로 마이그레이션해야 합니다.
 *
 *   group_imbalanced :
 *   태스크의 친화성 제약으로 인해 이전에는 스케줄러가 시스템 전체에 걸쳐 
 *   부하를 분산할 수 없었습니다.
 *
 *   group_overloaded : 
 *   CPU가 과부하되어 모든 작업에 예상 CPU 주기를 제공할 수 없습니다.
 */
enum group_type {
	/* The group has spare capacity that can be used to run more tasks.  */
	group_has_spare = 0,
	/*
	 * The group is fully used and the tasks don't compete for more CPU
	 * cycles. Nevertheless, some tasks might wait before running.
	 */
	group_fully_busy,
	/*
	 * SD_ASYM_CPUCAPACITY only: One task doesn't fit with CPU's capacity
	 * and must be migrated to a more powerful CPU.
	 */
	group_misfit_task,
	/*
	 * SD_ASYM_PACKING only: One local CPU with higher capacity is available,
	 * and the task should be migrated to it instead of running on the
	 * current CPU.
	 */
	group_asym_packing,
	/*
	 * The tasks' affinity constraints previously prevented the scheduler
	 * from balancing the load across the system.
	 */
	group_imbalanced,
	/*
	 * The CPU is overloaded and can't provide expected CPU cycles to all
	 * tasks.
	 */
	group_overloaded
};

/*
 * IAMROOT, 2023.05.06:
 * - find_busiest_group()(migration_type을 찾음),
 *   find_busiest_queue()(migration_type을 사용) 참고
 *
 * - migrate_load
 *   load를 비교한다.
 * - migrate_util
 *   util를 비교한다.
 * - migrate_task
 *   task수를 비교한다.
 * - migrate_misfit
 *   misfit load를 비교한다.
 */
enum migration_type {
	migrate_load = 0,
	migrate_util,
	migrate_task,
	migrate_misfit
};

/*
 * IAMROOT, 2023.05.13:
 * - LBF_ALL_PINNED: 모든 task들을 migration 할 수 없을 때
 *   LBF_DST_PINNED: dst cpu로 migration 할 수 없을 때
 *   LBF_SOME_PINNED: 일부 task가 migration 할 수 없을 때
 */
#define LBF_ALL_PINNED	0x01
#define LBF_NEED_BREAK	0x02
#define LBF_DST_PINNED  0x04
#define LBF_SOME_PINNED	0x08
#define LBF_ACTIVE_LB	0x10

struct lb_env {
	struct sched_domain	*sd;

	struct rq		*src_rq;
	int			src_cpu;

	int			dst_cpu;
	struct rq		*dst_rq;

	struct cpumask		*dst_grpmask;
	int			new_dst_cpu;
	enum cpu_idle_type	idle;
	long			imbalance;
	/* The set of CPUs under consideration for load-balancing */
	struct cpumask		*cpus;

	unsigned int		flags;

	unsigned int		loop;
	unsigned int		loop_break;
	unsigned int		loop_max;

	enum fbq_type		fbq_type;
	enum migration_type	migration_type;
	struct list_head	tasks;
};

/*
 * Is this task likely cache-hot:
 */
/*
 * IAMROOT. 2023.05.13:
 * - google-translate
 * 이 작업이 캐시 핫일 가능성이 있습니까?
 * - cache hot 이 아닌 조건
 *   1. !fair_sched_class 2. idle_policy 3. SMT
 * - cache hot 인 조건
 *   1. CACHE_HOT_BUDDY
 *   2. task 가 스케쥴링되고 실행한 시간 이
 *      sysctl_sched_migration_cost(default 0.5ms) 보다 작을때
 */
static int task_hot(struct task_struct *p, struct lb_env *env)
{
	s64 delta;

	lockdep_assert_rq_held(env->src_rq);

	if (p->sched_class != &fair_sched_class)
		return 0;

	if (unlikely(task_has_idle_policy(p)))
		return 0;

	/* SMT siblings share cache */
	if (env->sd->flags & SD_SHARE_CPUCAPACITY)
		return 0;

	/*
	 * Buddy candidates are cache hot:
	 */
	/*
	 * IAMROOT, 2023.05.13:
	 * - 다음 순서(next,last)는 cache hot task 로 판단한다.
	 */
	if (sched_feat(CACHE_HOT_BUDDY) && env->dst_rq->nr_running &&
			(&p->se == cfs_rq_of(&p->se)->next ||
			 &p->se == cfs_rq_of(&p->se)->last))
		return 1;

	/*
	 * IAMROOT, 2023.05.13:
	 * - taskhot 설정으로 debug시 sysfs를 통해 설정
	 */
	if (sysctl_sched_migration_cost == -1)
		return 1;

	/*
	 * Don't migrate task if the task's cookie does not match
	 * with the destination CPU's core cookie.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 작업의 쿠키가 대상 CPU의 코어 쿠키와 일치하지 않는 경우 작업을 마이그레이션하지
	 * 마십시오.
	 */
	if (!sched_core_cookie_match(cpu_rq(env->dst_cpu), p))
		return 1;

	/*
	 * IAMROOT, 2023.05.13:
	 * - taskhot 이 아닌 설정으로 debug시 sysfs를 통해 설정
	 */
	if (sysctl_sched_migration_cost == 0)
		return 0;

	/*
	 * IAMROOT, 2023.05.13:
	 * - task 가 스케쥴링되고 실행한 시간.
	 *   sysctl_sched_migration_cost(default 0.5ms) 보다 작을때 task hot 임.
	 */
	delta = rq_clock_task(env->src_rq) - p->se.exec_start;

	return delta < (s64)sysctl_sched_migration_cost;
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * Returns 1, if task migration degrades locality
 * Returns 0, if task migration improves locality i.e migration preferred.
 * Returns -1, if task migration is not affected by locality.
 */
/*
 * IAMROOT. 2023.05.13:
 * - google-translate
 * 작업 마이그레이션이 지역성을 저하시키는 경우 1을 반환합니다.
 * 작업 마이그레이션이 지역성을 향상시키는 경우(즉, 마이그레이션이 선호되는 경우) 0을
 * 반환합니다.
 * 작업 마이그레이션이 지역의 영향을 받지 않는 경우 -1을 반환합니다.
 * TODO
 */
static int migrate_degrades_locality(struct task_struct *p, struct lb_env *env)
{
	struct numa_group *numa_group = rcu_dereference(p->numa_group);
	unsigned long src_weight, dst_weight;
	int src_nid, dst_nid, dist;

	if (!static_branch_likely(&sched_numa_balancing))
		return -1;

	if (!p->numa_faults || !(env->sd->flags & SD_NUMA))
		return -1;

	src_nid = cpu_to_node(env->src_cpu);
	dst_nid = cpu_to_node(env->dst_cpu);

	if (src_nid == dst_nid)
		return -1;

	/* Migrating away from the preferred node is always bad. */
	if (src_nid == p->numa_preferred_nid) {
		if (env->src_rq->nr_running > env->src_rq->nr_preferred_running)
			return 1;
		else
			return -1;
	}

	/* Encourage migration to the preferred node. */
	if (dst_nid == p->numa_preferred_nid)
		return 0;

	/* Leaving a core idle is often worse than degrading locality. */
	if (env->idle == CPU_IDLE)
		return -1;

	dist = node_distance(src_nid, dst_nid);
	if (numa_group) {
		src_weight = group_weight(p, src_nid, dist);
		dst_weight = group_weight(p, dst_nid, dist);
	} else {
		src_weight = task_weight(p, src_nid, dist);
		dst_weight = task_weight(p, dst_nid, dist);
	}

	return dst_weight < src_weight;
}

#else
static inline int migrate_degrades_locality(struct task_struct *p,
					     struct lb_env *env)
{
	return -1;
}
#endif

/*
 * can_migrate_task - may task p from runqueue rq be migrated to this_cpu?
 */
/*
 * IAMROOT. 2023.05.13:
 * - google-translate
 * can_migrate_task - runqueue rq의 작업 p를 this_cpu로 마이그레이션할 수 있습니까?
 * - migrate(x)
 *   1. throttled_lb_pair
 *   2. pcpu kthread
 *   3. available cpus_ptr에 포함되지 않은 경우
 *   4. task_running
 *   5.
 * - migrate
 *   1. LBF_ACTIVE_LB 설정인 경우
 *   2. cache cold인 경우
 *   3. 이번에 hot으로 판명 되었지만 migration 시도 실패(그전 cold 타임)가
 *	 cache_nice_tries를 넘은 경우 return 1
 */
static
int can_migrate_task(struct task_struct *p, struct lb_env *env)
{
	int tsk_cache_hot;

	lockdep_assert_rq_held(env->src_rq);

	/*
	 * We do not migrate tasks that are:
	 * 1) throttled_lb_pair, or
	 * 2) cannot be migrated to this CPU due to cpus_ptr, or
	 * 3) running (obviously), or
	 * 4) are cache-hot on their current CPU.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 다음과 같은 작업은 마이그레이션하지 않습니다.
	 * 1) throttled_lb_pair, 또는
	 * 2) cpus_ptr로 인해 이 CPU로 마이그레이션할 수 없음, 또는
	 * 3) (분명히) 실행 중, 또는
	 * 4) 현재 CPU에서 캐시 핫입니다.
	 * - 2.5) pcpu kthread
	 */
	if (throttled_lb_pair(task_group(p), env->src_cpu, env->dst_cpu))
		return 0;

	/* Disregard pcpu kthreads; they are where they need to be. */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * pcpu kthread를 무시하십시오. 그들은 그들이 있어야 할 곳에 있습니다.
	 */
	if (kthread_is_per_cpu(p))
		return 0;

	/*
	 * IAMROOT, 2023.05.13:
	 * - available cpu에 dst_cpu 가 제외된 경우
	 */
	if (!cpumask_test_cpu(env->dst_cpu, p->cpus_ptr)) {
		int cpu;

		schedstat_inc(p->se.statistics.nr_failed_migrations_affine);

		env->flags |= LBF_SOME_PINNED;

		/*
		 * Remember if this task can be migrated to any other CPU in
		 * our sched_group. We may want to revisit it if we couldn't
		 * meet load balance goals by pulling other tasks on src_cpu.
		 *
		 * Avoid computing new_dst_cpu
		 * - for NEWLY_IDLE
		 * - if we have already computed one in current iteration
		 * - if it's an active balance
		 */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * 이 작업을 sched_group의 다른 CPU로 마이그레이션할 수 있는지
		 * 기억하십시오. src_cpu에서 다른 작업을 끌어와서 로드 균형 목표를
		 * 충족할 수 없는 경우 다시 방문하고 싶을 수 있습니다.
		 *
		 * new_dst_cpu 계산을 피하십시오
		 * - NEWLY_IDLE의 경우
		 * - 현재 반복에서 이미 계산한 경우
		 * - active balance 경우
		 */
		if (env->idle == CPU_NEWLY_IDLE ||
		    env->flags & (LBF_DST_PINNED | LBF_ACTIVE_LB))
			return 0;

		/* Prevent to re-select dst_cpu via env's CPUs: */
		/*
		 * IAMROOT, 2023.05.13:
		 * - dst_grpmask 중 avilable cpu가 있으면 LBF_DST_PINNED 설정
		 *   하고 new_dst_cpu 교체
		 */
		for_each_cpu_and(cpu, env->dst_grpmask, env->cpus) {
			if (cpumask_test_cpu(cpu, p->cpus_ptr)) {
				env->flags |= LBF_DST_PINNED;
				env->new_dst_cpu = cpu;
				break;
			}
		}

		return 0;
	}

	/* Record that we found at least one task that could run on dst_cpu */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * dst_cpu에서 실행할 수 있는 작업을 하나 이상 찾았음을 기록합니다.
	 */
	env->flags &= ~LBF_ALL_PINNED;

	/*
	 * IAMROOT, 2023.05.15:
	 * - 1. detach_tasks 에서 호출
	 *      @p 중 하나가 on_cpu(curr) 가 될 수 있고 @가 on_cpu(curr)이면
	 *      migration 할 수 없다.
	 *   2. active balance 에서 호출
	 *      on_cpu(curr)는 stop_thread 일 것이다. stop_thread 라면 위
	 *      kthread_is_per_cpu 에서 걸러졌을 것이라 생각할 수 있지만 @p 가
	 *      cfs_tasks 리스트 중 하나가 전달되었다
	 */
	if (task_running(env->src_rq, p)) {
		schedstat_inc(p->se.statistics.nr_failed_migrations_running);
		return 0;
	}

	/*
	 * Aggressive migration if:
	 * 1) active balance
	 * 2) destination numa is preferred
	 * 3) task is cache cold, or
	 * 4) too many balance attempts have failed.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 다음과 같은 경우 적극적인 마이그레이션:
	 * 1) active balance
	 * 2) 대상 numa가 선호됨
	 * 3) 작업이 캐시 콜드이거나
	 * 4) 너무 많은 균형 시도가 실패했습니다.
	 */
	if (env->flags & LBF_ACTIVE_LB)
		return 1;

	tsk_cache_hot = migrate_degrades_locality(p, env);
	if (tsk_cache_hot == -1)
		tsk_cache_hot = task_hot(p, env);

	/*
	 * IAMROOT, 2023.05.13:
	 * - 1. cache cold인 경우 return 1
	 *   2. 이번에 hot으로 판명 되었지만 migration 시도 실패(그전 cold 타임)가
	 *      cache_nice_tries를 넘은 경우 return 1
	 */
	if (tsk_cache_hot <= 0 ||
	    env->sd->nr_balance_failed > env->sd->cache_nice_tries) {
		if (tsk_cache_hot == 1) {
			schedstat_inc(env->sd->lb_hot_gained[env->idle]);
			schedstat_inc(p->se.statistics.nr_forced_migrations);
		}
		return 1;
	}

	schedstat_inc(p->se.statistics.nr_failed_migrations_hot);
	return 0;
}

/*
 * detach_task() -- detach the task for the migration specified in env
 */
static void detach_task(struct task_struct *p, struct lb_env *env)
{
	lockdep_assert_rq_held(env->src_rq);

	deactivate_task(env->src_rq, p, DEQUEUE_NOCLOCK);
	set_task_cpu(p, env->dst_cpu);
}

/*
 * detach_one_task() -- tries to dequeue exactly one task from env->src_rq, as
 * part of active balancing operations within "domain".
 *
 * Returns a task if successful and NULL otherwise.
 */
/*
 * IAMROOT. 2023.05.13:
 * - google-translate
 * detach_one_task() -- "도메인" 내 활성 균형 작업의 일부로 env->src_rq에서 정확히
 * 하나의 작업을 대기열에서 빼려고 시도합니다. 성공하면 작업을 반환하고 그렇지
 * 않으면 NULL을 반환합니다.
 */
/*
 * IAMROOT, 2023.05.13:
 * - 리스트의 뒤에서 task 하나를 detach 한다.
 */
static struct task_struct *detach_one_task(struct lb_env *env)
{
	struct task_struct *p;

	lockdep_assert_rq_held(env->src_rq);

	/*
	 * IAMROOT, 2023.05.13:
	 * - list의 뒷부분(cold)부터 순회한다.
	 */
	list_for_each_entry_reverse(p,
			&env->src_rq->cfs_tasks, se.group_node) {
		/*
		 * IAMROOT, 2023.05.18:
		 * - task 가 하나인 경우는 can_migrate_task 를 하지 않았으므로
		 *   여기서 확인한다. 또 중간에 cpu affinity 제한이 변경된 경우가
		 *   있을지는 모르겠다.
		 */
		if (!can_migrate_task(p, env))
			continue;

		detach_task(p, env);

		/*
		 * Right now, this is only the second place where
		 * lb_gained[env->idle] is updated (other is detach_tasks)
		 * so we can safely collect stats here rather than
		 * inside detach_tasks().
		 */
		schedstat_inc(env->sd->lb_gained[env->idle]);
		return p;
	}
	return NULL;
}

static const unsigned int sched_nr_migrate_break = 32;

/*
 * detach_tasks() -- tries to detach up to imbalance load/util/tasks from
 * busiest_rq, as part of a balancing operation within domain "sd".
 *
 * Returns number of detached tasks if successful and 0 otherwise.
 */
/*
 * IAMROOT, 2023.05.19:
 * - papago
 *   detach_tasks() -- 도메인 sd 내에서 균형 조정 작업의 일부로 
 *   busiest_rq에서 최대 불균형 로드/유틸/작업을 분리하려고 시도합니다.
 *
 *   성공하면 분리된 작업의 수를 반환하고 그렇지 않으면 0을 반환합니다.
 *
 * - 
 */
static int detach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->src_rq->cfs_tasks;
	unsigned long util, load;
	struct task_struct *p;
	int detached = 0;

	lockdep_assert_rq_held(env->src_rq);

	/*
	 * Source run queue has been emptied by another CPU, clear
	 * LBF_ALL_PINNED flag as we will not test any task.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 소스 실행 대기열이 다른 CPU에 의해 비워졌습니다. 어떤 작업도 테스트하지 않을
	 * 것이므로 LBF_ALL_PINNED 플래그를 지웁니다.
	 *
	 * - 직전에 nr_running > 1으로 들어왔는데, 그사이 감소가 되어버렸다.
	 *   누군가에게 migrate된것이므로 LBF_ALL_PINNED를 지워주면서 종료한다.
	 */
	if (env->src_rq->nr_running <= 1) {
		env->flags &= ~LBF_ALL_PINNED;
		return 0;
	}

	if (env->imbalance <= 0)
		return 0;

	while (!list_empty(tasks)) {
		/*
		 * We don't want to steal all, otherwise we may be treated likewise,
		 * which could at worst lead to a livelock crash.
		 */
		if (env->idle != CPU_NOT_IDLE && env->src_rq->nr_running <= 1)
			break;

		p = list_last_entry(tasks, struct task_struct, se.group_node);

		env->loop++;
		/* We've more or less seen every task there is, call it quits */
		if (env->loop > env->loop_max)
			break;

		/* take a breather every nr_migrate tasks */
		if (env->loop > env->loop_break) {
			env->loop_break += sched_nr_migrate_break;
			env->flags |= LBF_NEED_BREAK;
			break;
		}

		if (!can_migrate_task(p, env))
			goto next;

		switch (env->migration_type) {
		case migrate_load:
			/*
			 * Depending of the number of CPUs and tasks and the
			 * cgroup hierarchy, task_h_load() can return a null
			 * value. Make sure that env->imbalance decreases
			 * otherwise detach_tasks() will stop only after
			 * detaching up to loop_max tasks.
			 */
			load = max_t(unsigned long, task_h_load(p), 1);

			if (sched_feat(LB_MIN) &&
			    load < 16 && !env->sd->nr_balance_failed)
				goto next;

			/*
			 * Make sure that we don't migrate too much load.
			 * Nevertheless, let relax the constraint if
			 * scheduler fails to find a good waiting task to
			 * migrate.
			 */
			/*
			 * IAMROOT. 2023.05.12:
			 * - google-translate
			 * 너무 많은 부하를 마이그레이션하지 않도록 합니다. 그럼에도
			 * 불구하고 스케줄러가 마이그레이션할 좋은 대기 작업을 찾는 데
			 * 실패하면 제약 조건을 완화하십시오.
			 */
			if (shr_bound(load, env->sd->nr_balance_failed) > env->imbalance)
				goto next;

			env->imbalance -= load;
			break;
		case migrate_util:
			util = task_util_est(p);

			if (util > env->imbalance)
				goto next;

			env->imbalance -= util;
			break;

		case migrate_task:
			env->imbalance--;
			break;

		case migrate_misfit:
			/* This is not a misfit task */
			if (task_fits_capacity(p, capacity_of(env->src_cpu)))
				goto next;

			env->imbalance = 0;
			break;
		}

		detach_task(p, env);
		list_add(&p->se.group_node, &env->tasks);

		detached++;

#ifdef CONFIG_PREEMPTION
		/*
		 * NEWIDLE balancing is a source of latency, so preemptible
		 * kernels will stop after the first task is detached to minimize
		 * the critical section.
		 */
		if (env->idle == CPU_NEWLY_IDLE)
			break;
#endif

		/*
		 * We only want to steal up to the prescribed amount of
		 * load/util/tasks.
		 */
		/*
		 * IAMROOT. 2023.05.12:
		 * - google-translate
		 * 우리는 load/util/tasks의 규정된 양까지만 훔치기를 원합니다.
		 */
		if (env->imbalance <= 0)
			break;

		continue;
next:
		list_move(&p->se.group_node, tasks);
	}

	/*
	 * Right now, this is one of only two places we collect this stat
	 * so we can safely collect detach_one_task() stats here rather
	 * than inside detach_one_task().
	 */
	schedstat_add(env->sd->lb_gained[env->idle], detached);

	return detached;
}

/*
 * attach_task() -- attach the task detached by detach_task() to its new rq.
 */
static void attach_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);

	BUG_ON(task_rq(p) != rq);
	activate_task(rq, p, ENQUEUE_NOCLOCK);
	check_preempt_curr(rq, p, 0);
}

/*
 * attach_one_task() -- attaches the task returned from detach_one_task() to
 * its new rq.
 */
static void attach_one_task(struct rq *rq, struct task_struct *p)
{
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	attach_task(rq, p);
	rq_unlock(rq, &rf);
}

/*
 * attach_tasks() -- attaches all tasks detached by detach_tasks() to their
 * new rq.
 */
static void attach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->tasks;
	struct task_struct *p;
	struct rq_flags rf;

	rq_lock(env->dst_rq, &rf);
	update_rq_clock(env->dst_rq);

	while (!list_empty(tasks)) {
		p = list_first_entry(tasks, struct task_struct, se.group_node);
		list_del_init(&p->se.group_node);

		attach_task(env->dst_rq, p);
	}

	rq_unlock(env->dst_rq, &rf);
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * IAMROOT, 2023.05.03:
 * - @cfs_rq에 load avg, util_avg가 있는경우, 작업이 block되있다고
 *   예상할수있다.
 */
static inline bool cfs_rq_has_blocked(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->avg.load_avg)
		return true;

	if (cfs_rq->avg.util_avg)
		return true;

	return false;
}

/*
 * IAMROOT, 2023.04.29:
 * - cfs를 제외한(rt, dl, thermal, irq)util_avg중 하나라도 0 이 아니면 true
 */
static inline bool others_have_blocked(struct rq *rq)
{
	if (READ_ONCE(rq->avg_rt.util_avg))
		return true;

	if (READ_ONCE(rq->avg_dl.util_avg))
		return true;

	if (thermal_load_avg(rq))
		return true;

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	if (READ_ONCE(rq->avg_irq.util_avg))
		return true;
#endif

	return false;
}

/*
 * IAMROOT, 2023.05.03:
 * - update_blocked_averages()를 했던 마지막 시각 timestamp
 */
static inline void update_blocked_load_tick(struct rq *rq)
{
	WRITE_ONCE(rq->last_blocked_load_update_tick, jiffies);
}

/*
 * IAMROOT, 2023.04.29:
 * - rt, dl, thermal,irq cfs load avg 가 모두 0이면 has_blocked_load를 0으로 갱신
 *   @has_blocked는 rt dl thermal irq util_avg, cfs load_avg중 하나라도 있으면 true
 */
static inline void update_blocked_load_status(struct rq *rq, bool has_blocked)
{
	if (!has_blocked)
		rq->has_blocked_load = 0;
}
#else
static inline bool cfs_rq_has_blocked(struct cfs_rq *cfs_rq) { return false; }
static inline bool others_have_blocked(struct rq *rq) { return false; }
static inline void update_blocked_load_tick(struct rq *rq) {}
static inline void update_blocked_load_status(struct rq *rq, bool has_blocked) {}
#endif

/*
 * IAMROOT, 2023.05.03:
 * - @rq의 curr sched_class기준으로, 즉 현재 동작하고있는 sched_class기준으로
 *   rt,dl과 온도및 irq에 대한 load avg를 계산한다. decay가 어느 한군데서라도
 *   수행됬으면 return true.
 * - 아직 rt, dl, thermal, irq에 load 있는, 즉 blocked되고있다면 @done을
 *   false로 update한다.
 */
static bool __update_blocked_others(struct rq *rq, bool *done)
{
	const struct sched_class *curr_class;
	u64 now = rq_clock_pelt(rq);
	unsigned long thermal_pressure;
	bool decayed;

	/*
	 * update_load_avg() can call cpufreq_update_util(). Make sure that RT,
	 * DL and IRQ signals have been updated before updating CFS.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * update_load_avg()는 cpufreq_update_util()을 호출할 수 있습니다. CFS를
	 * 업데이트하기 전에 RT, DL 및 IRQ 신호가 업데이트되었는지 확인하십시오.
	 */
	curr_class = rq->curr->sched_class;

	thermal_pressure = arch_scale_thermal_pressure(cpu_of(rq));

	decayed = update_rt_rq_load_avg(now, rq, curr_class == &rt_sched_class) |
		  update_dl_rq_load_avg(now, rq, curr_class == &dl_sched_class) |
		  update_thermal_load_avg(rq_clock_thermal(rq), rq, thermal_pressure) |
		  update_irq_load_avg(rq, 0);

	/*
	 * IAMROOT, 2023.04.29:
	 * - cfs를 제외한(rt, dl, thermal, irq) util_avg중 하나라도 있으면 *done=false
	 */
	if (others_have_blocked(rq))
		*done = false;

	return decayed;
}

#ifdef CONFIG_FAIR_GROUP_SCHED

/*
 * IAMROOT, 2023.05.03:
 * - bottom-up으로 순회하며 다음을 수행한다.
 *   1. cfs_rq load를 update한다. decay됬으면 task group도 update한다.
 *      이때 decay된 cfs_rq가 요청한 @rq였다면 return시 decayed 표시를한다.
 *   2. cfs_rq의 se의 update여부를 확인해 update한다.
 *   3. @cfs_rq가 decay됬으면 leaf_cfs_rq에서 제거한다.
 *   4. @cfs_rq가 block이면, 즉 어떤 작업이 있으면 @done을 false로
 *   update한다
 */
static bool __update_blocked_fair(struct rq *rq, bool *done)
{
	struct cfs_rq *cfs_rq, *pos;
	bool decayed = false;
	int cpu = cpu_of(rq);

	/*
	 * Iterates the task_group tree in a bottom up fashion, see
	 * list_add_leaf_cfs_rq() for details.
	 */
	for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos) {
		struct sched_entity *se;

		if (update_cfs_rq_load_avg(cfs_rq_clock_pelt(cfs_rq), cfs_rq)) {
			update_tg_load_avg(cfs_rq);

			if (cfs_rq == &rq->cfs)
				decayed = true;
		}

		/* Propagate pending load changes to the parent, if any: */
		se = cfs_rq->tg->se[cpu];
		if (se && !skip_blocked_update(se))
			update_load_avg(cfs_rq_of(se), se, UPDATE_TG);

		/*
		 * There can be a lot of idle CPU cgroups.  Don't let fully
		 * decayed cfs_rqs linger on the list.
		 */
		if (cfs_rq_is_decayed(cfs_rq))
			list_del_leaf_cfs_rq(cfs_rq);

		/* Don't need periodic decay once load/util_avg are null */
		if (cfs_rq_has_blocked(cfs_rq))
			*done = false;
	}

	return decayed;
}

/*
 * Compute the hierarchical load factor for cfs_rq and all its ascendants.
 * This needs to be done in a top-down fashion because the load of a child
 * group is a fraction of its parents load.
 */
/*
 * IAMROOT, 2023.05.03:
 * - papago
 *   cfs_rq 및 모든 해당 상위 항목에 대한 계층적 부하 계수를 계산합니다.
 *   하위 그룹의 로드는 상위 로드의 일부이기 때문에 하향식 방식으로 수행해야 합니다.
 *
 * - top-down 방식으로 내려가며 h_load를 계산한다.
 */
static void update_cfs_rq_h_load(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct sched_entity *se = cfs_rq->tg->se[cpu_of(rq)];
	unsigned long now = jiffies;
	unsigned long load;

/*
 * IAMROOT, 2023.05.03:
 * - 이전에 이미 햇다면 return.
 */
	if (cfs_rq->last_h_load_update == now)
		return;

	WRITE_ONCE(cfs_rq->h_load_next, NULL);
/*
 * IAMROOT, 2023.05.03:
 * - se를 오르며 h_load_next에 등록해놓는다.
 *   누군가 이미 update를 햇다면 이후엔 다해놧을것이므로 break.
 */
	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		WRITE_ONCE(cfs_rq->h_load_next, se);
		if (cfs_rq->last_h_load_update == now)
			break;
	}

/*
 * IAMROOT, 2023.05.03:
 * - root까지 도달했다면 root h_load를 load avg로 기록해둔다.
 */
	if (!se) {
		cfs_rq->h_load = cfs_rq_load_avg(cfs_rq);
		cfs_rq->last_h_load_update = now;
	}

/*
 * IAMROOT, 2023.05.03:
 * - root(혹은 기록해놨던 se부터)시작하여 아래로 내려가면서
 *   hierarchy로 통계를 내가며 기록하면서 내려간다.
 *   마지막으로 기록한 시간을 last_h_load_update에 기록한다.
 */
	while ((se = READ_ONCE(cfs_rq->h_load_next)) != NULL) {
		load = cfs_rq->h_load;
		load = div64_ul(load * se->avg.load_avg,
			cfs_rq_load_avg(cfs_rq) + 1);
		cfs_rq = group_cfs_rq(se);
		cfs_rq->h_load = load;
		cfs_rq->last_h_load_update = now;
	}
}

/*
 * IAMROOT, 2023.05.03:
 * - @p를 기준으로 h load를 update후, @p에 대한 h_load를 가져온다.
 */
static unsigned long task_h_load(struct task_struct *p)
{
	struct cfs_rq *cfs_rq = task_cfs_rq(p);

	update_cfs_rq_h_load(cfs_rq);
	return div64_ul(p->se.avg.load_avg * cfs_rq->h_load,
			cfs_rq_load_avg(cfs_rq) + 1);
}
#else
static bool __update_blocked_fair(struct rq *rq, bool *done)
{
	struct cfs_rq *cfs_rq = &rq->cfs;
	bool decayed;

	decayed = update_cfs_rq_load_avg(cfs_rq_clock_pelt(cfs_rq), cfs_rq);
	if (cfs_rq_has_blocked(cfs_rq))
		*done = false;

	return decayed;
}

static unsigned long task_h_load(struct task_struct *p)
{
	return p->se.avg.load_avg;
}
#endif

/*
 * IAMROOT, 2023.04.29:
 * - 1. 현재함수 진입시각 update 및 rq clock 
 * - 2. blocked cfs, rt, dl, thermal, irq를 update한다.
 *   3. rq dl thermal irq util avg, cfs load avg 가 모두 0 이면
 *      has_blocked_load = 0 으로 설정
 *   4. decayed 면 cpufreq_update_util 호출
 */
static void update_blocked_averages(int cpu)
{
	bool decayed = false, done = true;
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	update_blocked_load_tick(rq);
	update_rq_clock(rq);

	/*
	 * IAMROOT, 2023.04.29:
	 * - cfs를 제외한(rt, dl, thermal, irq) util_avg중 하나라도 있으면 *done=false
	 */
	decayed |= __update_blocked_others(rq, &done);
	/*
	 * IAMROOT, 2023.04.29:
	 * - cfs load_avg가 있으면 *done=false
	 */
	decayed |= __update_blocked_fair(rq, &done);

	update_blocked_load_status(rq, !done);
	if (decayed)
		cpufreq_update_util(rq, 0);
	rq_unlock_irqrestore(rq, &rf);
}

/********** Helpers for find_busiest_group ************************/

/*
 * sg_lb_stats - stats of a sched_group required for load_balancing
 */
struct sg_lb_stats {
	unsigned long avg_load; /*Avg load across the CPUs of the group */
	unsigned long group_load; /* Total load over the CPUs of the group */
	unsigned long group_capacity;
	unsigned long group_util; /* Total utilization over the CPUs of the group */
	unsigned long group_runnable; /* Total runnable time over the CPUs of the group */
	unsigned int sum_nr_running; /* Nr of tasks running in the group */
	unsigned int sum_h_nr_running; /* Nr of CFS tasks running in the group */
	unsigned int idle_cpus;
	unsigned int group_weight;
	enum group_type group_type;
/*
 * IAMROOT, 2023.05.06:
 * - task를 move해야되는지의 여부. update_sg_lb_stats() 참고
 */
	unsigned int group_asym_packing; /* Tasks should be moved to preferred CPU */
	unsigned long group_misfit_task_load; /* A CPU has a task too big for its capacity */
#ifdef CONFIG_NUMA_BALANCING
/*
 * IAMROOT, 2023.05.06:
 * - nr_numa_running : node 지정을 했을때, 선호 node 상관없이 동작하는 task수
 *                     node 지정을 안했으면 증가를 안한다.
 * - numa_migrate_preferred : 권장 node에 동작하는 task수
 */
	unsigned int nr_numa_running;
	unsigned int nr_preferred_running;
#endif
};

/*
 * sd_lb_stats - Structure to store the statistics of a sched_domain
 *		 during load balancing.
 */
struct sd_lb_stats {
	struct sched_group *busiest;	/* Busiest group in this sd */
	struct sched_group *local;	/* Local group in this sd */
	unsigned long total_load;	/* Total load of all groups in sd */
	unsigned long total_capacity;	/* Total capacity of all groups in sd */
	unsigned long avg_load;	/* Average load across all groups in sd */
	unsigned int prefer_sibling; /* tasks should go to sibling first */

/*
 * IAMROOT, 2023.05.06:
 * - 가장 높은 group
 */
	struct sg_lb_stats busiest_stat;/* Statistics of the busiest group */
	struct sg_lb_stats local_stat;	/* Statistics of the local group */
};

/*
 * IAMROOT, 2023.05.06:
 * - @sds초기화.
 */
static inline void init_sd_lb_stats(struct sd_lb_stats *sds)
{
	/*
	 * Skimp on the clearing to avoid duplicate work. We can avoid clearing
	 * local_stat because update_sg_lb_stats() does a full clear/assignment.
	 * We must however set busiest_stat::group_type and
	 * busiest_stat::idle_cpus to the worst busiest group because
	 * update_sd_pick_busiest() reads these before assignment.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   중복 작업을 피하기 위해 정리 작업에 인색합니다. update_sg_lb_stats()가 
 *   전체 지우기/할당을 수행하기 때문에 local_stat 지우기를 피할 수 있습니다.
 *   하지만 busiest_stat::group_type 및 busiest_stat::idle_cpus를 가장 바쁜 
 *   그룹으로 설정해야 합니다. 왜냐하면 update_sd_pick_busiest()가 할당 전에 
 *   이들을 읽기 때문입니다.
 */
	*sds = (struct sd_lb_stats){
		.busiest = NULL,
		.local = NULL,
		.total_load = 0UL,
		.total_capacity = 0UL,
		.busiest_stat = {
			.idle_cpus = UINT_MAX,
			.group_type = group_has_spare,
		},
	};
}

/*
 * IAMROOT, 2023.04.22:
 * - rt + dl + 온도를 제외한 capacity값에 irq 소모비율까지 고려한 여유 capacity를 return한다.
 */
static unsigned long scale_rt_capacity(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long max = arch_scale_cpu_capacity(cpu);
	unsigned long used, free;
	unsigned long irq;

/*
 * IAMROOT, 2023.04.22:
 * - irq에서 사용한 시간을 가져온다.
 */
	irq = cpu_util_irq(rq);

/*
 * IAMROOT, 2023.04.22:
 * - irq에서 사용한 cpu만으로 cpu 성능을 다쓴경우는 매우 드물지만 
 *   예외처리를 해준다.
 */
	if (unlikely(irq >= max))
		return 1;

	/*
	 * avg_rt.util_avg and avg_dl.util_avg track binary signals
	 * (running and not running) with weights 0 and 1024 respectively.
	 * avg_thermal.load_avg tracks thermal pressure and the weighted
	 * average uses the actual delta max capacity(load).
	 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   avg_rt.util_avg 및 avg_dl.util_avg는 각각 가중치 0 및 1024로 이진 
 *   신호(실행 중 및 실행 중이 아님)를 추적합니다.
 *   avg_thermal.load_avg는 열 압력을 추적하고 가중 평균은 실제 델타 
 *   최대 용량(부하)을 사용합니다.
 */
	used = READ_ONCE(rq->avg_rt.util_avg);
	used += READ_ONCE(rq->avg_dl.util_avg);
	used += thermal_load_avg(rq);

	if (unlikely(used >= max))
		return 1;

	free = max - used;

	return scale_irq_capacity(free, irq, max);
}

/*
 * IAMROOT, 2023.04.22:
 * - 1. @cpu에 대한 원래 성능을 cpu_capacity_orig에 기록한다.
 *   2. rq에 rt에 대한 여유 capacity를 기록
 */
static void update_cpu_capacity(struct sched_domain *sd, int cpu)
{
	unsigned long capacity = scale_rt_capacity(cpu);
	struct sched_group *sdg = sd->groups;

	cpu_rq(cpu)->cpu_capacity_orig = arch_scale_cpu_capacity(cpu);

	if (!capacity)
		capacity = 1;

	cpu_rq(cpu)->cpu_capacity = capacity;
	trace_sched_cpu_capacity_tp(cpu_rq(cpu));

	sdg->sgc->capacity = capacity;
	sdg->sgc->min_capacity = capacity;
	sdg->sgc->max_capacity = capacity;
}

/*
 * IAMROOT, 2023.04.22:
 * - 최하위 domain
 *   cpu capacity값을 설정한다.
 * - SD_OVERLAP
 *   group에 속해있는 cpu capacity값 통계하여 @sd의 sgc를 설정한다.
 * - !SD_OVERLAP
 *   하위 sgc의 capacity를 누적 통계하여 @sd의 sgc에 설정한다.
 */
void update_group_capacity(struct sched_domain *sd, int cpu)
{
	struct sched_domain *child = sd->child;
	struct sched_group *group, *sdg = sd->groups;
	unsigned long capacity, min_capacity, max_capacity;
	unsigned long interval;

	interval = msecs_to_jiffies(sd->balance_interval);
	interval = clamp(interval, 1UL, max_load_balance_interval);
	sdg->sgc->next_update = jiffies + interval;

/*
 * IAMROOT, 2023.04.22:
 * - child가 없는 최하위 domain이 먼저 초기화되고, 이후  multi cpu
 *   가 있는 domain들은 미리 계산된 capacity를 합산 및 비교를 하여 계산된다.
 */
	if (!child) {
		update_cpu_capacity(sd, cpu);
		return;
	}

	capacity = 0;
	min_capacity = ULONG_MAX;
	max_capacity = 0;

/*
 * IAMROOT, 2023.04.22:
 * - numa. SD_OVERLAP 의미 자체가 중복되있는 cpu가 있을수 있다는 개념이 되므로,
 *   직접 cpu capacity를 사용해 계산한다.
 */
	if (child->flags & SD_OVERLAP) {
		/*
		 * SD_OVERLAP domains cannot assume that child groups
		 * span the current group.
		 */
/*
 * IAMROOT, 2023.04.22:
 * - child에서 미리 update_cpu_capacity()를 통해 계산된 값들을 통계한다.
 */
		for_each_cpu(cpu, sched_group_span(sdg)) {
			unsigned long cpu_cap = capacity_of(cpu);

			capacity += cpu_cap;
			min_capacity = min(cpu_cap, min_capacity);
			max_capacity = max(cpu_cap, max_capacity);
		}
	} else  {
/*
 * IAMROOT, 2023.04.22:
 * - SD_OVERLAP이 없으면 중복된 CPU가 없다는 개념이 되어 sgc 사용이 가능하다.
 *   하위 sgc를 합산하여 현재 sgc로 통계한다.
 */
		/*
		 * !SD_OVERLAP domains can assume that child groups
		 * span the current group.
		 */
/*
 * IAMROOT, 2023.04.22:
 * - child group들을 합산하여 sgc에 넣는다.
 */
		/*
		 * IAMROOT. 2023.04.22:
		 * - google-translate
		 * !SD_OVERLAP 도메인은 하위 그룹이 현재 그룹에 걸쳐 있다고 가정할 수
		 * 있습니다.
		 * - numa가 아닌 경우 도메인 하위 그룹을 순회하며 capacity, min, max
		 *   를 설정한다.
		 *
		 * IAMROOT, 2023.05.10:
		 * - group 은 child->groups 이므로 첫번째 손자에 해당한다. 손자의
		 *   capacity sum, min, max 등을 구해 아들(sdg) 통계에 업데이트하는
		 *   개념.
		 */
		group = child->groups;
		do {
			struct sched_group_capacity *sgc = group->sgc;

			capacity += sgc->capacity;
			min_capacity = min(sgc->min_capacity, min_capacity);
			max_capacity = max(sgc->max_capacity, max_capacity);
			group = group->next;
		} while (group != child->groups);
	}

	sdg->sgc->capacity = capacity;
	sdg->sgc->min_capacity = min_capacity;
	sdg->sgc->max_capacity = max_capacity;
}

/*
 * Check whether the capacity of the rq has been noticeably reduced by side
 * activity. The imbalance_pct is used for the threshold.
 * Return true is the capacity is reduced
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   부수적인 활동으로 rq의 용량이 눈에 띄게 줄었는지 확인합니다. 
 *   imbalance_pct는 임계값에 사용됩니다. true를 반환하면 용량이 줄었다는것.
 *
 * - rq capa가 눈에뜨게 줄었다면 return true.
 */
static inline int
check_cpu_capacity(struct rq *rq, struct sched_domain *sd)
{
	return ((rq->cpu_capacity * sd->imbalance_pct) <
				(rq->cpu_capacity_orig * 100));
}

/*
 * Check whether a rq has a misfit task and if it looks like we can actually
 * help that task: we can migrate the task to a CPU of higher capacity, or
 * the task's current CPU is heavily pressured.
 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   rq에 부적합한 작업이 있는지 확인하고 우리가 실제로 해당 작업을 도울 
 *   수 있는 것처럼 보이는지 확인합니다. 작업을 더 높은 용량의 CPU로 
 *   마이그레이션하거나 작업의 현재 CPU에 큰 부담이 있습니다.
 *
 * - 1. @rq에 이미 misfit load(cpu capa를 넘어선 load)가 있다.
 *   2. @rd에 @rq보다 높은 capa를 가진 cpu가 존재하거나
 *      @rq capa가 이미 줄어든 상태라면
 *   위 조건이 맞는지 확인한다. 즉 misfit load가 있지만, 더 좋은
 *   cpu로 옮길수 있는 상황인지 확인한다.
 */
static inline int check_misfit_status(struct rq *rq, struct sched_domain *sd)
{
	return rq->misfit_task_load &&
		(rq->cpu_capacity_orig < rq->rd->max_cpu_capacity ||
		 check_cpu_capacity(rq, sd));
}

/*
 * Group imbalance indicates (and tries to solve) the problem where balancing
 * groups is inadequate due to ->cpus_ptr constraints.
 *
 * Imagine a situation of two groups of 4 CPUs each and 4 tasks each with a
 * cpumask covering 1 CPU of the first group and 3 CPUs of the second group.
 * Something like:
 *
 *	{ 0 1 2 3 } { 4 5 6 7 }
 *	        *     * * *
 *
 * If we were to balance group-wise we'd place two tasks in the first group and
 * two tasks in the second group. Clearly this is undesired as it will overload
 * cpu 3 and leave one of the CPUs in the second group unused.
 *
 * The current solution to this issue is detecting the skew in the first group
 * by noticing the lower domain failed to reach balance and had difficulty
 * moving tasks due to affinity constraints.
 *
 * When this is so detected; this group becomes a candidate for busiest; see
 * update_sd_pick_busiest(). And calculate_imbalance() and
 * find_busiest_group() avoid some of the usual balance conditions to allow it
 * to create an effective group imbalance.
 *
 * This is a somewhat tricky proposition since the next run might not find the
 * group imbalance and decide the groups need to be balanced again. A most
 * subtle and fragile situation.
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   그룹 불균형은 ->cpus_ptr 제약 조건으로 인해 균형 그룹이 부적합한 문제를 
 *   나타냅니다(그리고 해결을 시도합니다).
 *
 *   첫 번째 그룹의 CPU 1개와 두 번째 그룹의 CPU 3개를 덮는 cpumask가 있는 
 *   각각 4개의 CPU와 4개의 작업으로 구성된 두 그룹의 상황을 상상해 보십시오.
 *   다음과 같은 것:
 *
 *	{ 0 1 2 3 } { 4 5 6 7 }
 *	        *     * * *
 *   그룹별로 균형을 잡으려면 첫 번째 그룹에 두 개의 작업을 배치하고 두 번째 
 *   그룹에 두 개의 작업을 배치합니다. 분명히 이것은 cpu 3에 과부하를 일으키고 
 *   두 번째 그룹의 CPU 중 하나를 사용하지 않은 상태로 남겨두기 때문에 
 *   바람직하지 않습니다.
 *
 *   이 문제에 대한 현재 솔루션은 하위 도메인이 균형에 도달하지 못하고 선호도 
 *   제약으로 인해 작업을 이동하는 데 어려움이 있음을 확인하여 첫 번째 그룹의 
 *   편향을 감지하는 것입니다.
 *
 *   이렇게 적발된 때 이 그룹은 가장 바쁜 후보가 됩니다. 
 *   update_sd_pick_busiest()를 참조하십시오. 그리고 calculate_imbalance() 및 
 *   find_busiest_group()은 효과적인 그룹 불균형을 만들 수 있도록 일반적인 
 *   균형 조건 중 일부를 피합니다.
 *
 *   다음 실행에서 그룹 불균형을 찾지 못하고 그룹의 균형을 다시 맞춰야 한다고 
 *   결정할 수 있기 때문에 이것은 다소 까다로운 제안입니다. 가장 미묘하고 
 *   깨지기 쉬운 상황.
 *
 * - imbalance가 있는지 확인한다.
 */
static inline int sg_imbalanced(struct sched_group *group)
{
	return group->sgc->imbalance;
}

/*
 * group_has_capacity returns true if the group has spare capacity that could
 * be used by some tasks.
 * We consider that a group has spare capacity if the  * number of task is
 * smaller than the number of CPUs or if the utilization is lower than the
 * available capacity for CFS tasks.
 * For the latter, we use a threshold to stabilize the state, to take into
 * account the variance of the tasks' load and to return true if the available
 * capacity in meaningful for the load balancer.
 * As an example, an available capacity of 1% can appear but it doesn't make
 * any benefit for the load balance.
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   group_has_capacity는 그룹에 일부 작업에서 사용할 수 있는 여유 용량이 
 *   있는 경우 true를 반환합니다.
 *   작업 수가 CPU 수보다 적거나 사용률이 CFS 작업에 사용할 수 있는 용량보다 
 *   낮은 경우 그룹에 여유 용량이 있는 것으로 간주합니다.
 *   후자의 경우 임계값을 사용하여 상태를 안정화하고 작업 로드의 분산을 
 *   고려하며 사용 가능한 용량이 로드 밸런서에 의미가 있는 경우 true를 반환합니다.
 *   예를 들어 사용 가능한 용량이 1%로 나타날 수 있지만 부하 분산에는 아무런 
 *   이점이 없습니다.
 *
 * - group이 충분한 cap을 가지고 있는지 확인한다. group_is_overloaded()와 비슷한
 *   방법으로 비교한다.
 */
static inline bool
group_has_capacity(unsigned int imbalance_pct, struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running < sgs->group_weight)
		return true;

	if ((sgs->group_capacity * imbalance_pct) <
			(sgs->group_runnable * 100))
		return false;

	if ((sgs->group_capacity * 100) >
			(sgs->group_util * imbalance_pct))
		return true;

	return false;
}

/*
 *  group_is_overloaded returns true if the group has more tasks than it can
 *  handle.
 *  group_is_overloaded is not equals to !group_has_capacity because a group
 *  with the exact right number of tasks, has no more spare capacity but is not
 *  overloaded so both group_has_capacity and group_is_overloaded return
 *  false.
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   group_is_overloaded는 그룹에 처리할 수 있는 것보다 많은 작업이 있는 경우 
 *   true를 반환합니다.
 *   group_is_overloaded는 !group_has_capacity와 같지 않습니다. 정확한 
 *   작업 수를 가진 그룹은 더 이상 여유 용량이 없지만 과부하되지 않았기 때문에 
 *   group_has_capacity와 group_is_overloaded 모두 false를 반환합니다.
 *
 * - group이 overload가 됬는지에 대한 여부를 판별한다. 판별시 imbalance 보정을 한다.
 *   group_has_capacity()에서도 비슷한 방법으로 판별한다.
 */
static inline bool
group_is_overloaded(unsigned int imbalance_pct, struct sg_lb_stats *sgs)
{
/*
 * IAMROOT, 2023.05.06:
 * - task가 cpu개수보다 적은 경우. return false.
 */
	if (sgs->sum_nr_running <= sgs->group_weight)
		return false;

/*
 * IAMROOT, 2023.05.06:
 * - group util이 group cap을 넘은 경우 return true.
 */
	if ((sgs->group_capacity * 100) <
			(sgs->group_util * imbalance_pct))
		return true;

/*
 * IAMROOT, 2023.05.06:
 * - group runnable이 group cap을 넘은 경우 return true.
 */
	/*
	 * IAMROOT, 2023.05.10:
	 * - XXX group_util 과 group_runnable 에서 imbalance_pct 를 cross
	 *   로 곱하는 이유는?
	 */
	if ((sgs->group_capacity * imbalance_pct) <
			(sgs->group_runnable * 100))
		return true;

	return false;
}

/*
 * IAMROOT, 2023.05.06:
 * - 인자를 사용하 group type을 판별한다.
 */
static inline enum
group_type group_classify(unsigned int imbalance_pct,
			  struct sched_group *group,
			  struct sg_lb_stats *sgs)
{
	if (group_is_overloaded(imbalance_pct, sgs))
		return group_overloaded;

	if (sg_imbalanced(group))
		return group_imbalanced;

	if (sgs->group_asym_packing)
		return group_asym_packing;

	if (sgs->group_misfit_task_load)
		return group_misfit_task;

	if (!group_has_capacity(imbalance_pct, sgs))
		return group_fully_busy;

	return group_has_spare;
}

/**
 * update_sg_lb_stats - Update sched_group's statistics for load balancing.
 * @env: The load balancing environment.
 * @group: sched_group whose statistics are to be updated.
 * @sgs: variable to hold the statistics for this group.
 * @sg_status: Holds flag indicating the status of the sched_group
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   update_sg_lb_stats - 부하 분산을 위해 sched_group의 통계를 업데이트합니다.
 *   @env: 로드 밸런싱 환경.
 *   @group: 통계를 업데이트할 sched_group.
 *   @sgs: 이 그룹에 대한 통계를 보유하는 변수입니다.
 *   @sg_status: sched_group의 상태를 나타내는 플래그를 보유합니다. 
 *
 * - group 통계를 갱신한다.
 * - @sg_status
 *   SG_OVERLOAD : nr_running이 2개이상인 cpu가 있는 경우
 *                 remote asym이면서, group_misfit_task_load이 갱신된경우
 *   SG_OVERUTILIZED : overutil인 cpu가 있는 경우
 */
static inline void update_sg_lb_stats(struct lb_env *env,
				      struct sched_group *group,
				      struct sg_lb_stats *sgs,
				      int *sg_status)
{
	int i, nr_running, local_group;

	memset(sgs, 0, sizeof(*sgs));

	local_group = cpumask_test_cpu(env->dst_cpu, sched_group_span(group));

	for_each_cpu_and(i, sched_group_span(group), env->cpus) {
		struct rq *rq = cpu_rq(i);

		sgs->group_load += cpu_load(rq);
		sgs->group_util += cpu_util(i);
		sgs->group_runnable += cpu_runnable(rq);
		sgs->sum_h_nr_running += rq->cfs.h_nr_running;

		nr_running = rq->nr_running;
		sgs->sum_nr_running += nr_running;

		if (nr_running > 1)
			*sg_status |= SG_OVERLOAD;

/*
 * IAMROOT, 2023.05.06:
 * - @i가 overutil이라면 SG_OVERUTILIZED를 set한다.
 */
		if (cpu_overutilized(i))
			*sg_status |= SG_OVERUTILIZED;

#ifdef CONFIG_NUMA_BALANCING
		sgs->nr_numa_running += rq->nr_numa_running;
		sgs->nr_preferred_running += rq->nr_preferred_running;
#endif
		/*
		 * No need to call idle_cpu() if nr_running is not 0
		 */
		if (!nr_running && idle_cpu(i)) {
			sgs->idle_cpus++;
			/* Idle cpu can't have misfit task */
			continue;
		}

		if (local_group)
			continue;

		/* Check for a misfit task on the cpu */
/*
 * IAMROOT, 2023.05.06:
 * - remote인 경우, asym이라면 misfit이 제일큰것을 기록하고, SG_OVERLOAD표시를 한다.
 */
		if (env->sd->flags & SD_ASYM_CPUCAPACITY &&
		    sgs->group_misfit_task_load < rq->misfit_task_load) {
			sgs->group_misfit_task_load = rq->misfit_task_load;
			*sg_status |= SG_OVERLOAD;
		}
	}

	/* Check if dst CPU is idle and preferred to this group */
/*
 * IAMROOT, 2023.05.06:
 * - smt이고, 1개이상의 task가 동작중이고, dst_cpu가 
 *   asym_prefer_cpu보다 우선순위가 높다면 group_asym_packing을 set한다.
 */
	if (env->sd->flags & SD_ASYM_PACKING &&
	    env->idle != CPU_NOT_IDLE &&
	    sgs->sum_h_nr_running &&
	    sched_asym_prefer(env->dst_cpu, group->asym_prefer_cpu)) {
		sgs->group_asym_packing = 1;
	}

	sgs->group_capacity = group->sgc->capacity;

	sgs->group_weight = group->group_weight;

/*
 * IAMROOT, 2023.05.06:
 * - group_type을 update한다.
 */
	sgs->group_type = group_classify(env->sd->imbalance_pct, group, sgs);

	/* Computing avg_load makes sense only when group is overloaded */
/*
 * IAMROOT, 2023.05.06:
 * - overload인 경우에만 avg_load를 update한다.
 */
	if (sgs->group_type == group_overloaded)
		sgs->avg_load = (sgs->group_load * SCHED_CAPACITY_SCALE) /
				sgs->group_capacity;
}

/**
 * update_sd_pick_busiest - return 1 on busiest group
 * @env: The load balancing environment.
 * @sds: sched_domain statistics
 * @sg: sched_group candidate to be checked for being the busiest
 * @sgs: sched_group statistics
 *
 * Determine if @sg is a busier group than the previously selected
 * busiest group.
 *
 * Return: %true if @sg is a busier group than the previously selected
 * busiest group. %false otherwise.
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *  update_sd_pick_busiest - 가장 바쁜 그룹에서 1을 반환합니다. 
 *  @env: 로드 밸런싱 환경.
 *  @sds: sched_domain 통계.
 *  @sg: 가장 바쁜지 확인할 sched_group 후보.
 *  @sgs: sched_group 통계.
 *
 *  @sg가 이전에 선택한 가장 바쁜 그룹보다 더 바쁜 그룹인지 확인합니다.
 *
 *  Return: @sg가 이전에 선택한 가장 바쁜 그룹보다 더 바쁜 그룹인 경우 
 *  %true입니다. 그렇지 않으면 %false입니다.
 *
 *  @return true sg를 busiest로 갱신한다.
 *
 *  1. @sgs이 misfit인 경우, 교체대상이 misfit보다 커야 해결된다. 작을 경우 return false.,
 *  2. local이 group_has_spare가 아닌 경우 이미 바쁘다. return false.
 *  3. group_type이 높은게 우선 순위가 높다. 
 *  4. group_type이 같은 경우 group_type에 따른 비교를 한다.
 *  5. asym일 경우 cap이 높은 경우가 우선시된다.
 *
 * IAMROOT, 2023.05.11:
 * - @sg 의 stat(@sgs)를 @sds와 비교해서 @sg가 더 빠르다면 update 하라는 의미로
 *   true를 반환한다.
 * - @sds에 현재 설정된 busiest stats와 @sg에 설정된 stats(@sgs)를 비교하여
 *   현재 루프의 @sg가 더 바쁘다고(busiest) 판단되면 true를 리턴한다.
 */
static bool update_sd_pick_busiest(struct lb_env *env,
				   struct sd_lb_stats *sds,
				   struct sched_group *sg,
				   struct sg_lb_stats *sgs)
{
	struct sg_lb_stats *busiest = &sds->busiest_stat;

	/* Make sure that there is at least one task to pull */
	if (!sgs->sum_h_nr_running)
		return false;

	/*
	 * Don't try to pull misfit tasks we can't help.
	 * We can use max_capacity here as reduction in capacity on some
	 * CPUs in the group should either be possible to resolve
	 * internally or be covered by avg_load imbalance (eventually).
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   우리가 도울 수 없는 부적합한 작업을 끌어내려고 하지 마십시오.
 *   여기서 max_capacity를 사용할 수 있습니다. 그룹의 일부 CPU에 대한 
 *   용량 감소는 내부적으로 해결할 수 있거나 avg_load 불균형에 의해 
 *   처리되어야 하기 때문입니다(결국).
 *
 * - group에 big task가 돌고있는 상황에서, group보다 더 낮은 cpu가 dst인 경우,
 *   즉 group이 misfit으로 바쁜데, dest가 이미 group 보다 성능이 떨어지면 busiest로 
 *   선택안하겠다는것이다.
 * - local_stat이 group_has_spare가 아닌 경우, 즉 local이 이미 바쁜상태.
 */
	if (sgs->group_type == group_misfit_task &&
	    (!capacity_greater(capacity_of(env->dst_cpu), sg->sgc->max_capacity) ||
	     sds->local_stat.group_type != group_has_spare))
		return false;

/*
 * IAMROOT, 2023.05.06:
 * - group_type으로 우선순위를 먼저 판별한다.
 * - 맨처음 진입할 땐 init_sd_lb_stats 에서 설정된 group_has_spare 가
 *   busiest->group_type 이다. 후에 group_has_spare 보다 더 바쁜 그룹이 있으면
 *   계속 루프에서 업데이트 된다.
 */
	if (sgs->group_type > busiest->group_type)
		return true;

	if (sgs->group_type < busiest->group_type)
		return false;

/*
 * IAMROOT, 2023.05.06:
 * - group_type이 같은 경우. 각 type별로 비교하는 방법을 따른다.
 */
	/*
	 * The candidate and the current busiest group are the same type of
	 * group. Let check which one is the busiest according to the type.
	 */

	switch (sgs->group_type) {
	case group_overloaded:
		/* Select the overloaded group with highest avg_load. */
		if (sgs->avg_load <= busiest->avg_load)
			return false;
		break;

	case group_imbalanced:
		/*
		 * Select the 1st imbalanced group as we don't have any way to
		 * choose one more than another.
		 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   다른 것보다 하나를 더 선택할 방법이 없으므로 첫 번째 불균형 그룹을 선택합니다.
 */
		return false;

	case group_asym_packing:
		/* Prefer to move from lowest priority CPU's work */
		if (sched_asym_prefer(sg->asym_prefer_cpu, sds->busiest->asym_prefer_cpu))
			return false;
		break;

	case group_misfit_task:
		/*
		 * If we have more than one misfit sg go with the biggest
		 * misfit.
		 */
		if (sgs->group_misfit_task_load < busiest->group_misfit_task_load)
			return false;
		break;

	case group_fully_busy:
		/*
		 * Select the fully busy group with highest avg_load. In
		 * theory, there is no need to pull task from such kind of
		 * group because tasks have all compute capacity that they need
		 * but we can still improve the overall throughput by reducing
		 * contention when accessing shared HW resources.
		 *
		 * XXX for now avg_load is not computed and always 0 so we
		 * select the 1st one.
		 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   avg_load가 가장 높은 완전히 바쁜 그룹을 선택합니다. 이론상으로는 작업에 
 *   필요한 모든 컴퓨팅 용량이 있기 때문에 이러한 종류의 그룹에서 작업을 가져올 
 *   필요가 없지만 공유 HW 리소스에 액세스할 때 경합을 줄임으로써 전체 처리량을 
 *   여전히 개선할 수 있습니다.
 *
 *   XXX: 현재 avg_load는 계산되지 않고 항상 0이므로 첫 번째 것을 선택합니다.
 */
		if (sgs->avg_load <= busiest->avg_load)
			return false;
		break;

	case group_has_spare:
		/*
		 * Select not overloaded group with lowest number of idle cpus
		 * and highest number of running tasks. We could also compare
		 * the spare capacity which is more stable but it can end up
		 * that the group has less spare capacity but finally more idle
		 * CPUs which means less opportunity to pull tasks.
		 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   유휴 CPU 수가 가장 적고 실행 중인 작업 수가 가장 많은 오버로드되지 않은 
 *   그룹을 선택합니다. 더 안정적인 여유 용량을 비교할 수도 있지만 그룹의 여유 
 *   용량은 적지만 결국 유휴 CPU가 많아져 작업을 가져올 기회가 줄어듭니다.
 *
 * - idle_cpus가 많은것을 우선한다. 같다면 running이 많은것을 우선한다.
 */
		if (sgs->idle_cpus > busiest->idle_cpus)
			return false;
		else if ((sgs->idle_cpus == busiest->idle_cpus) &&
			 (sgs->sum_nr_running <= busiest->sum_nr_running))
			return false;

		break;
	}

	/*
	 * Candidate sg has no more than one task per CPU and has higher
	 * per-CPU capacity. Migrating tasks to less capable CPUs may harm
	 * throughput. Maximize throughput, power/energy consequences are not
	 * considered.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   후보 sg는 CPU당 작업이 하나 이상 없으며 CPU당 용량이 더 큽니다. 
 *   성능이 낮은 CPU로 작업을 마이그레이션하면 처리량이 저하될 수 있습니다. 
 *   처리량을 최대화하고 전력/에너지 결과는 고려하지 않습니다.
 *
 * - asym인 경우 cap이 높은것을 우선한다는 의미가된다.
 */
	if ((env->sd->flags & SD_ASYM_CPUCAPACITY) &&
	    (sgs->group_type <= group_fully_busy) &&
	    (capacity_greater(sg->sgc->min_capacity, capacity_of(env->dst_cpu))))
		return false;

	return true;
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * IAMROOT, 2023.05.06:
 * - fbq_type을 판별해 return 한다. 
 *   group 기준으로 판별한다.
 *
 * IAMROOT, 2023.05.11:
 * - XXX regular, remote가 무엇을 뜻하는지 모르겠다.
 */
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	if (sgs->sum_h_nr_running > sgs->nr_numa_running)
		return regular;
	if (sgs->sum_h_nr_running > sgs->nr_preferred_running)
		return remote;
	return all;
}

/*
 * IAMROOT, 2023.05.06:
 * - fbq_type을 판별해 return한다.
 *   rq기준으로 판별한다.
 *
 * - regular
 *   numa설정이 안된 task가 하나라도 있는 경우
 * - remote
 *   preferred running초과의 task가 있다면 1개 이상이 preferred node가
 *   아닌 곳에서 동작하고 있다는것이다.
 * - all
 *   모든 task가 preferred node에서 동작하고있다.
 */
static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
/*
 * IAMROOT, 2023.05.06:
 * - nr_running이 nr_numa_running이 될순없다. == 인 상태로 내려오게된다.
 */
	if (rq->nr_running > rq->nr_numa_running)
		return regular;
	if (rq->nr_running > rq->nr_preferred_running)
		return remote;
	return all;
}
#else
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	return all;
}

static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
	return regular;
}
#endif /* CONFIG_NUMA_BALANCING */


struct sg_lb_stats;

/*
 * task_running_on_cpu - return 1 if @p is running on @cpu.
 */

/*
 * IAMROOT, 2023.06.17:
 * - @p가 cpu에 on이라면 return 1.
 */
static unsigned int task_running_on_cpu(int cpu, struct task_struct *p)
{
	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return 0;

	if (task_on_rq_queued(p))
		return 1;

	return 0;
}

/**
 * idle_cpu_without - would a given CPU be idle without p ?
 * @cpu: the processor on which idleness is tested.
 * @p: task which should be ignored.
 *
 * Return: 1 if the CPU would be idle. 0 otherwise.
 */
static int idle_cpu_without(int cpu, struct task_struct *p)
{
	struct rq *rq = cpu_rq(cpu);

	if (rq->curr != rq->idle && rq->curr != p)
		return 0;

	/*
	 * rq->nr_running can't be used but an updated version without the
	 * impact of p on cpu must be used instead. The updated nr_running
	 * be computed and tested before calling idle_cpu_without().
	 */

#ifdef CONFIG_SMP
	if (rq->ttwu_pending)
		return 0;
#endif

	return 1;
}

/*
 * update_sg_wakeup_stats - Update sched_group's statistics for wakeup.
 * @sd: The sched_domain level to look for idlest group.
 * @group: sched_group whose statistics are to be updated.
 * @sgs: variable to hold the statistics for this group.
 * @p: The task for which we look for the idlest group/CPU.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * update_sg_wakeup_stats - 웨이크업에 대한 sched_group의 통계를
 * 업데이트합니다.
 * @sd: 가장 유휴 그룹을 찾기 위한 sched_domain 수준입니다.
 * @group: 통계를 업데이트할 sched_group.
 * @sgs: 이 그룹에 대한 통계를 보유하는 변수입니다.
 * @p: 가장 유휴 그룹/CPU를 찾는 작업입니다.
 *
 * - @sgs에 @group에서 @p를 제외한 통계값을 산출한다.
 */
static inline void update_sg_wakeup_stats(struct sched_domain *sd,
					  struct sched_group *group,
					  struct sg_lb_stats *sgs,
					  struct task_struct *p)
{
	int i, nr_running;

	memset(sgs, 0, sizeof(*sgs));

/*
 * IAMROOT, 2023.06.17:
 * - group에서 속한 cpu를 순회한다.
 *   @p에 대한 load를 제외한 값을 통계한다.
 */
	for_each_cpu(i, sched_group_span(group)) {
		struct rq *rq = cpu_rq(i);
		unsigned int local;

		sgs->group_load += cpu_load_without(rq, p);
		sgs->group_util += cpu_util_without(i, p);
		sgs->group_runnable += cpu_runnable_without(rq, p);
		local = task_running_on_cpu(i, p);
		sgs->sum_h_nr_running += rq->cfs.h_nr_running - local;

		nr_running = rq->nr_running - local;
		sgs->sum_nr_running += nr_running;

		/*
		 * No need to call idle_cpu_without() if nr_running is not 0
		 */
		if (!nr_running && idle_cpu_without(i, p))
			sgs->idle_cpus++;

	}

	/* Check if task fits in the group */
/*
 * IAMROOT, 2023.06.17:
 * - asym인데 @p가 @group에 fit이 안되면 misfit이라는것을 표시한다.
 */
	if (sd->flags & SD_ASYM_CPUCAPACITY &&
	    !task_fits_capacity(p, group->sgc->max_capacity)) {
		sgs->group_misfit_task_load = 1;
	}

/*
 * IAMROOT, 2023.06.17:
 * - @group 값들로 채운다.
 */
	sgs->group_capacity = group->sgc->capacity;

	sgs->group_weight = group->group_weight;

	sgs->group_type = group_classify(sd->imbalance_pct, group, sgs);

	/*
	 * Computing avg_load makes sense only when group is fully busy or
	 * overloaded
	 */
/*
 * IAMROOT, 2023.06.17:
 * - fully_busy나 overloaded라면 avg_load를 계산한다..
 */
	if (sgs->group_type == group_fully_busy ||
		sgs->group_type == group_overloaded)
		sgs->avg_load = (sgs->group_load * SCHED_CAPACITY_SCALE) /
				sgs->group_capacity;
}

/*
 * IAMROOT, 2023.06.17:
 * @return @sgs가 @idlest_sgs보다 idle하면 return true. 아니면 false.
 */
static bool update_pick_idlest(struct sched_group *idlest,
			       struct sg_lb_stats *idlest_sgs,
			       struct sched_group *group,
			       struct sg_lb_stats *sgs)
{
	if (sgs->group_type < idlest_sgs->group_type)
		return true;

	if (sgs->group_type > idlest_sgs->group_type)
		return false;

	/*
	 * The candidate and the current idlest group are the same type of
	 * group. Let check which one is the idlest according to the type.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - group_type 같은 경우에 따른 처리.
 */
	switch (sgs->group_type) {
	case group_overloaded:
	case group_fully_busy:
		/* Select the group with lowest avg_load. */
		if (idlest_sgs->avg_load <= sgs->avg_load)
			return false;
		break;

	case group_imbalanced:
	case group_asym_packing:
		/* Those types are not used in the slow wakeup path */
		return false;

	case group_misfit_task:
		/* Select group with the highest max capacity */
		if (idlest->sgc->max_capacity >= group->sgc->max_capacity)
			return false;
		break;

	case group_has_spare:
		/* Select group with most idle CPUs */
		if (idlest_sgs->idle_cpus > sgs->idle_cpus)
			return false;

		/* Select group with lowest group_util */
		if (idlest_sgs->idle_cpus == sgs->idle_cpus &&
			idlest_sgs->group_util <= sgs->group_util)
			return false;

		break;
	}

	return true;
}

/*
 * Allow a NUMA imbalance if busy CPUs is less than 25% of the domain.
 * This is an approximation as the number of running tasks may not be
 * related to the number of busy CPUs due to sched_setaffinity.
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   사용량이 많은 CPU가 도메인의 25% 미만인 경우 NUMA 불균형을 허용합니다.
 *   실행 중인 작업의 수는 sched_setaffinity로 인해 사용 중인 CPU 수와 관련이 
 *   없을 수 있으므로 근사치입니다.
 *
 * - return true : running의 25%보다 cpu가 많은 경우.
 *
 * IAMROOT, 2023.07.04:
 * - Return: dst node cpu 갯수의 25% 보다 running task 가 작은 경우 true 반환
 */
static inline bool allow_numa_imbalance(int dst_running, int dst_weight)
{
	return (dst_running < (dst_weight >> 2));
}

/*
 * find_idlest_group() finds and returns the least busy CPU group within the
 * domain.
 *
 * Assumes p is allowed on at least one CPU in sd.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * find_idles_group()은 도메인 내에서 가장 사용량이 적은 CPU 그룹을 찾아서
 * 반환합니다. p는 sd에 있는 하나 이상의 CPU에서 허용된다고 가정합니다.
 *
 * @return NULL local 선택. != NULL이면 local이 아닌 idlest group
 * - @group을 순회하며 @p를 제외한 통계값을 산출하여 idlest group을 찾는다.
 *
 * - @sd 내에 group들을 순회하며 @this_cpu가 속한 local group 과 그외의 그룹들중
 *   가장 idle한 그룹을 비교하여 더 idle한 그룹을 반환한다. local이 더 idle 한 경우는
 *   NULL 반환.
 */
static struct sched_group *
find_idlest_group(struct sched_domain *sd, struct task_struct *p, int this_cpu)
{
	struct sched_group *idlest = NULL, *local = NULL, *group = sd->groups;
	struct sg_lb_stats local_sgs, tmp_sgs;
	struct sg_lb_stats *sgs;
	unsigned long imbalance;
	struct sg_lb_stats idlest_sgs = {
			.avg_load = UINT_MAX,
			.group_type = group_overloaded,
	};

/*
 * IAMROOT, 2023.06.17:
 * - @group을 순회하하며 통계값을 산출하고 local과 idlest group을 찾는다.
 */
	do {
		int local_group;

		/* Skip over this group if it has no CPUs allowed */
		if (!cpumask_intersects(sched_group_span(group),
					p->cpus_ptr))
			continue;

		/* Skip over this group if no cookie matched */
		if (!sched_group_cookie_match(cpu_rq(this_cpu), p, group))
			continue;

		local_group = cpumask_test_cpu(this_cpu,
					       sched_group_span(group));

/*
 * IAMROOT, 2023.06.15:
 * - local이면 sgs를 local_sds, local = group, 아니라면 local은 남겨놓고
 *   sgs를 tmp_sgs로 설정한다.
 */
		if (local_group) {
			sgs = &local_sgs;
			local = group;
		} else {
			sgs = &tmp_sgs;
		}

		update_sg_wakeup_stats(sd, group, sgs, p);

/*
 * IAMROOT, 2023.06.17:
 * - local상황이 아닌 경우, idlest를 갱신한다.
 */
		/*
		 * IAMROOT, 2023.06.15:
		 * - local_group이 아닌 group 들중 가장 idle한 group을 찾는다.
		 */
		if (!local_group && update_pick_idlest(idlest, &idlest_sgs, group, sgs)) {
			idlest = group;
			idlest_sgs = *sgs;
		}

	} while (group = group->next, group != sd->groups);


	/* There is no idlest group to push tasks to */
	if (!idlest)
		return NULL;

	/* The local group has been skipped because of CPU affinity */
/*
 * IAMROOT, 2023.06.17:
 * - local이 범위에 없었다면 즉시 idlest로 선택한다.
 */
	if (!local)
		return idlest;
/*
 * IAMROOT, 2023.06.17:
 * - 지금부터 local과 idlest 비교를 시작한다.
 *   local과 idlest중에 더 idle한것을 고른다.
 */
	/*
	 * If the local group is idler than the selected idlest group
	 * don't try and push the task.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   로컬 그룹이 선택한 가장 유휴 그룹보다 유휴 상태이면 작업을 푸시하지 
 *   마십시오.
 */
	if (local_sgs.group_type < idlest_sgs.group_type)
		return NULL;

	/*
	 * If the local group is busier than the selected idlest group
	 * try and push the task.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   로컬 그룹이 선택한 가장 유휴 그룹보다 더 바쁘면 작업을 푸시하십시오.
 */
	if (local_sgs.group_type > idlest_sgs.group_type)
		return idlest;

	switch (local_sgs.group_type) {
	case group_overloaded:
	case group_fully_busy:

		/* Calculate allowed imbalance based on load */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   부하에 따라 허용되는 불균형을 계산합니다. 
 */
		imbalance = scale_load_down(NICE_0_LOAD) *
				(sd->imbalance_pct-100) / 100;

		/*
		 * When comparing groups across NUMA domains, it's possible for
		 * the local domain to be very lightly loaded relative to the
		 * remote domains but "imbalance" skews the comparison making
		 * remote CPUs look much more favourable. When considering
		 * cross-domain, add imbalance to the load on the remote node
		 * and consider staying local.
		 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   NUMA 도메인 전체에서 그룹을 비교할 때 로컬 도메인이 원격 도메인에 
 *   비해 매우 가볍게 로드될 수 있지만 불균형으로 인해 비교가 왜곡되어 
 *   원격 CPU가 훨씬 더 유리해 보입니다. 교차 도메인을 고려할 때 원격 
 *   노드의 부하에 불균형을 추가하고 로컬 유지를 고려하십시오.
 */

		if ((sd->flags & SD_NUMA) &&
		    ((idlest_sgs.avg_load + imbalance) >= local_sgs.avg_load))
			return NULL;

		/*
		 * If the local group is less loaded than the selected
		 * idlest group don't try and push any tasks.
		 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   로컬 그룹이 선택한 가장 유휴 그룹보다 로드가 적으면 어떤 작업도 
 *   시도하지 마십시오.
 */
		if (idlest_sgs.avg_load >= (local_sgs.avg_load + imbalance))
			return NULL;

		if (100 * local_sgs.avg_load <= sd->imbalance_pct * idlest_sgs.avg_load)
			return NULL;
		break;

	case group_imbalanced:
	case group_asym_packing:
		/* Those type are not used in the slow wakeup path */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   이러한 유형은 느린 웨이크업 경로에서 사용되지 않습니다. 
 * - group_imbalance, group_asym_packing은 cache 비친화 유형에서는 무시한다.  
 */
		return NULL;

	case group_misfit_task:
		/* Select group with the highest max capacity */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   최대 용량이 가장 큰 그룹 선택.
 */
		if (local->sgc->max_capacity >= idlest->sgc->max_capacity)
			return NULL;
		break;

	case group_has_spare:
/*
 * IAMROOT, 2023.06.17:
 * - NUMA인경우
 *   1. preferred를 먼저 확인한다.
 *   2. local에서 적게 돌고있으면 그냥 local을 선택한다.
 *   
 * - 위의 2번까지도 대상이 아니였거나 그냥 numa가 아닌 경우엔
 *   idle cpu가 많은 group을 선택한다.
 */
		if (sd->flags & SD_NUMA) {
#ifdef CONFIG_NUMA_BALANCING
			int idlest_cpu;
			/*
			 * If there is spare capacity at NUMA, try to select
			 * the preferred node
			 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   NUMA에 여유 용량이 있으면 선호하는 노드를 선택해 보십시오. 
 */
			if (cpu_to_node(this_cpu) == p->numa_preferred_nid)
				return NULL;

			idlest_cpu = cpumask_first(sched_group_span(idlest));
			if (cpu_to_node(idlest_cpu) == p->numa_preferred_nid)
				return idlest;
#endif
			/*
			 * Otherwise, keep the task on this node to stay close
			 * its wakeup source and improve locality. If there is
			 * a real need of migration, periodic load balance will
			 * take care of it.
			 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   그렇지 않으면 이 노드에 작업을 유지하여 웨이크업 소스 가까이에 유지하고 
 *   지역성을 개선합니다. 실제로 마이그레이션이 필요한 경우 정기적인 로드 
 *   밸런싱이 이를 처리합니다.
 */
			if (allow_numa_imbalance(local_sgs.sum_nr_running, sd->span_weight))
				return NULL;
		}

		/*
		 * Select group with highest number of idle CPUs. We could also
		 * compare the utilization which is more stable but it can end
		 * up that the group has less spare capacity but finally more
		 * idle CPUs which means more opportunity to run task.
		 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   유휴 CPU 수가 가장 많은 그룹을 선택합니다. 보다 안정적인 사용률을 
 *   비교할 수도 있지만 그룹의 여유 용량은 적지만 마지막으로 유휴 CPU가 
 *   많아 작업을 실행할 기회가 더 많아질 수 있습니다.
 */
		if (local_sgs.idle_cpus >= idlest_sgs.idle_cpus)
			return NULL;
		break;
	}

	return idlest;
}

/**
 * update_sd_lb_stats - Update sched_domain's statistics for load balancing.
 * @env: The load balancing environment.
 * @sds: variable to hold the statistics for this sched_domain.
 */

/*
 * IAMROOT, 2023.05.06:
 * - group을 순회하며 local과 busiest group을 찾고, 통계를 내어 @sds를 완성한다.
 *   통계후, rd에 SG_OVERLOAD, SG_OVERUTILIZED가 있었다면 기록한다.
 */
static inline void update_sd_lb_stats(struct lb_env *env, struct sd_lb_stats *sds)
{
	struct sched_domain *child = env->sd->child;
	struct sched_group *sg = env->sd->groups;
	struct sg_lb_stats *local = &sds->local_stat;
	struct sg_lb_stats tmp_sgs;
	int sg_status = 0;

/*
 * IAMROOT, 2023.05.06:
 * - group을 순회하며 sds를 통계한다. local인것은 한번반 선택되고, other인 경우
 *   busiest 판별까지 수행한다.
 */
	do {
		struct sg_lb_stats *sgs = &tmp_sgs;
		int local_group;

/*
 * IAMROOT, 2023.05.06:
 * - dst_cpu가 local group에 있는지 확인한다.
 *   group의 첫번째는 local, 두번째부터는 remote가 된다.
 */
		local_group = cpumask_test_cpu(env->dst_cpu, sched_group_span(sg));
		if (local_group) {
			sds->local = sg;
			sgs = local;

/*
 * IAMROOT, 2023.05.06:
 * - @env->idle이 새로추가되는 경우가 아닌 경우(CPU_NEWLY_IDLE), 즉
 *   periodic balance이거나 next_update이후에 진입했다면 capacity를 update한다.
 */
			if (env->idle != CPU_NEWLY_IDLE ||
			    time_after_eq(jiffies, sg->sgc->next_update))
				update_group_capacity(env->sd, env->dst_cpu);
		}

		update_sg_lb_stats(env, sg, sgs, &sg_status);

		if (local_group)
			goto next_group;

/*
 * IAMROOT, 2023.05.06:
 * - 여기서부터 other group
 */

/*
 * IAMROOT, 2023.05.06:
 * - busiest 여부를 판별하여 갱신한다.
 */
		if (update_sd_pick_busiest(env, sds, sg, sgs)) {
			sds->busiest = sg;
			sds->busiest_stat = *sgs;
		}

next_group:
		/* Now, start updating sd_lb_stats */
		sds->total_load += sgs->group_load;
		sds->total_capacity += sgs->group_capacity;

		sg = sg->next;
	} while (sg != env->sd->groups);

	/* Tag domain that child domain prefers tasks go to siblings first */
	sds->prefer_sibling = child && child->flags & SD_PREFER_SIBLING;

/*
 * IAMROOT, 2023.05.06:
 * - numa인 경우, fbq_type을 판별한다.
 */
	if (env->sd->flags & SD_NUMA)
		env->fbq_type = fbq_classify_group(&sds->busiest_stat);

/*
 * IAMROOT, 2023.05.06:
 * - root인 경우, SG_OVERLOAD, SG_OVERUTILIZED가 있었다면 기록한다.
 *   SG_OVERUTILIZED은 무조건 기록한다.
 */
	if (!env->sd->parent) {
		struct root_domain *rd = env->dst_rq->rd;

		/* update overload indicator if we are at root domain */
		WRITE_ONCE(rd->overload, sg_status & SG_OVERLOAD);

		/* Update over-utilization (tipping point, U >= 0) indicator */
		WRITE_ONCE(rd->overutilized, sg_status & SG_OVERUTILIZED);
		trace_sched_overutilized_tp(rd, sg_status & SG_OVERUTILIZED);
	} else if (sg_status & SG_OVERUTILIZED) {
		struct root_domain *rd = env->dst_rq->rd;

		WRITE_ONCE(rd->overutilized, SG_OVERUTILIZED);
		trace_sched_overutilized_tp(rd, SG_OVERUTILIZED);
	}
}

#define NUMA_IMBALANCE_MIN 2

/*
 * IAMROOT, 2023.05.06:
 * - imbalance를 그대로 사용할지 여부를 정한다. dest에 cpu대비 running이 이미 
 *   많을때 imbalance값이 적다면 그냥 0 return.
 *   아닌 경우 그대로 imbalance 사용
 *
 * - dst의 running task 가 cpu 갯수의 25% 미만이고 @imbalnce 값이 1 이나 2
 *   이면 0 으로 조정한다.
 *
 * - src -> dst로 task가 이동한후 dst의 running task 가 dst node 의 cpu 갯수의
 *   25% 미만이라면 src보다 running task 갯수가 2개까지 많더라도 balance로
 *   판단하도록 imbalnce 값을 0 으로 조정하여 반환한다.
 *   위 조건이 아니면 원래값 반환
 */
static inline long adjust_numa_imbalance(int imbalance,
				int dst_running, int dst_weight)
{
/*
 * IAMROOT, 2023.05.06:
 * - running이 cpu개수보다 25%이상이면 imbalance값을 그대로 사용한다.
 */
	if (!allow_numa_imbalance(dst_running, dst_weight))
		return imbalance;

	/*
	 * Allow a small imbalance based on a simple pair of communicating
	 * tasks that remain local when the destination is lightly loaded.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - 목적지가 가볍게 로드될 때 로컬로 남아 있는 간단한 통신 작업 쌍을 
 *   기반으로 약간의 불균형을 허용합니다.
 *
 * - running task가 25% 미만이라면 imbalance 2 이하는 0으로 조정
 */
	if (imbalance <= NUMA_IMBALANCE_MIN)
		return 0;

	return imbalance;
}

/**
 * calculate_imbalance - Calculate the amount of imbalance present within the
 *			 groups of a given sched_domain during load balance.
 * @env: load balance environment
 * @sds: statistics of the sched_domain whose imbalance is to be calculated.
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   calculate_imbalance - 부하 분산 동안 주어진 sched_domain의 그룹 내에 
 *   존재하는 불균형의 양을 계산합니다.
 *   @env: 부하 분산 환경
 *   @sds:불균형을 계산할 sched_domain의 통계입니다.
 *
 * - imbalance값을 산출한다. group에 따라 migration_type과 imbalance를
 *   정한다.
 */
static inline void calculate_imbalance(struct lb_env *env, struct sd_lb_stats *sds)
{
	struct sg_lb_stats *local, *busiest;

	local = &sds->local_stat;
	busiest = &sds->busiest_stat;

	if (busiest->group_type == group_misfit_task) {
		/* Set imbalance to allow misfit tasks to be balanced. */
		env->migration_type = migrate_misfit;
		env->imbalance = 1;
		return;
	}

/*
 * IAMROOT, 2023.05.06:
 * - smt인 경우이다.
 * - smt인 경우 load를 이동시키는게 그렇게 중요하진 않다.
 */
	if (busiest->group_type == group_asym_packing) {
		/*
		 * In case of asym capacity, we will try to migrate all load to
		 * the preferred CPU.
		 */
		env->migration_type = migrate_task;
		env->imbalance = busiest->sum_h_nr_running;
		return;
	}

	if (busiest->group_type == group_imbalanced) {
		/*
		 * In the group_imb case we cannot rely on group-wide averages
		 * to ensure CPU-load equilibrium, try to move any task to fix
		 * the imbalance. The next load balance will take care of
		 * balancing back the system.
		 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   group_imb의 경우 CPU 로드 평형을 보장하기 위해 그룹 전체 평균에 
 *   의존할 수 없으며 불균형을 수정하기 위해 작업을 이동하려고 합니다. 
 *   다음 부하 균형은 시스템 균형을 다시 조정합니다.
 */
		env->migration_type = migrate_task;
		env->imbalance = 1;
		return;
	}

	/*
	 * Try to use spare capacity of local group without overloading it or
	 * emptying busiest.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   로컬 그룹의 여유 용량을 과부하하거나 가장 많이 비우지 않고 사용하십시오.
 * - local이 놀고있는 경우 busiest->group_type > group_fully_busy(실질적으로 overload)
 *   이면서 SD_SHARE_PKG_RESOURCES를 가지지 않은 경우(numa or die)
 */
	/*
	 * IAMROOT, 2023.05.11:
	 * - 남은 busiest->group_type
	 *   group_has_spare
	 *   group_fully_busy,
	 *   [x]group_misfit_task,
	 *   [x]group_asym_packing,
	 *   [x]group_imbalanced,
	 *   group_overloaded
	 */
	if (local->group_type == group_has_spare) {
		if ((busiest->group_type > group_fully_busy) &&
		    !(env->sd->flags & SD_SHARE_PKG_RESOURCES)) {
			/*
			 * If busiest is overloaded, try to fill spare
			 * capacity. This might end up creating spare capacity
			 * in busiest or busiest still being overloaded but
			 * there is no simple way to directly compute the
			 * amount of load to migrate in order to balance the
			 * system.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   가장 사용량이 많은 경우 여유 용량을 채우십시오. 이로 인해 가장 
 *   바쁘거나 여전히 과부하 상태인 예비 용량이 생성될 수 있지만 시스템 균형을 
 *   유지하기 위해 마이그레이션할 로드 양을 직접 계산하는 간단한 방법은 없습니다.
 * - local이 놀고있지만, busiest가 overload이고, numa or die와 같은 많은 cpu가 보유될수
 *   있는 경우 util로 선택하게 한다.
 * - imbalance는 남은 capa개념으로 설정한다.
 */
			env->migration_type = migrate_util;
			env->imbalance = max(local->group_capacity, local->group_util) -
					 local->group_util;

			/*
			 * In some cases, the group's utilization is max or even
			 * higher than capacity because of migrations but the
			 * local CPU is (newly) idle. There is at least one
			 * waiting task in this overloaded busiest group. Let's
			 * try to pull it.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   경우에 따라 마이그레이션으로 인해 그룹의 사용률이 최대이거나 
 *   용량보다 훨씬 높지만 로컬 CPU는 (새로) 유휴 상태입니다. 가장 바쁜 이 
 *   그룹에는 최소한 하나의 대기 작업이 있습니다. 당겨보자.
 * -migrate등의 이유로 예외사항이 발생할수있다. 그에 대한 처리를 수행한다.
 */
			if (env->idle != CPU_NOT_IDLE && env->imbalance == 0) {
				env->migration_type = migrate_task;
				env->imbalance = 1;
			}

			return;
		}

/*
 * IAMROOT, 2023.05.06:
 * - group이 cpu1개뿐이거나, prefer sibling이 존재하는 경우
 *   local보다 2개이상 많은 running이 있으면 imbalance set된다.
 */
		if (busiest->group_weight == 1 || sds->prefer_sibling) {
			unsigned int nr_diff = busiest->sum_nr_running;
			/*
			 * When prefer sibling, evenly spread running tasks on
			 * groups.
			 */
			env->migration_type = migrate_task;
/*
 * IAMROOT, 2023.05.06:
 * - nr_diff -= local->sum_nr_running. -가 되면 nr_iff = 0.
 */
			lsub_positive(&nr_diff, local->sum_nr_running);
			env->imbalance = nr_diff >> 1;
		} else {

			/*
			 * If there is no overload, we just want to even the number of
			 * idle cpus.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - local이 busiest보다 2개이상 idle이 많은 경우 imbalance set된다.
 */
			env->migration_type = migrate_task;
			env->imbalance = max_t(long, 0, (local->idle_cpus -
						 busiest->idle_cpus) >> 1);
		}

		/* Consider allowing a small imbalance between NUMA groups */

/*
 * IAMROOT, 2023.05.06:
 * - numa인경우, cpu가 running의 25%보다 적으면 imbalance가 0으로 바뀐다.
 */
		if (env->sd->flags & SD_NUMA) {
			env->imbalance = adjust_numa_imbalance(env->imbalance,
				busiest->sum_nr_running, busiest->group_weight);
		}

		return;
	}

	/*
	 * Local is fully busy but has to take more load to relieve the
	 * busiest group
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   로컬은 완전히 바쁘지만 가장 바쁜 그룹을 완화하기 위해 더 많은 
 *   부하를 감당해야 합니다. 
 * - local이 overload이하인경우, local avg_load, sds avg_load를 계산을 한다.
 *   그 후 local이 busiest보다 바쁘다면 imbalance는 0으로 하고 return.
 */
	/*
	 * IAMROOT, 2023.05.11:
	 * - 아래 조건으로 중간 단계 type의 local group 들만 적용됨
	 *   [x]group_has_spare
	 *   group_fully_busy,
	 *   group_misfit_task,
	 *   group_asym_packing,
	 *   group_imbalanced,
	 *   [x]group_overloaded
	 */
	if (local->group_type < group_overloaded) {
		/*
		 * Local will become overloaded so the avg_load metrics are
		 * finally needed.
		 */

		local->avg_load = (local->group_load * SCHED_CAPACITY_SCALE) /
				  local->group_capacity;

		sds->avg_load = (sds->total_load * SCHED_CAPACITY_SCALE) /
				sds->total_capacity;
		/*
		 * If the local group is more loaded than the selected
		 * busiest group don't try to pull any tasks.
		 */
/*
 * IAMROOT, 2023.05.06:
 * - local 평균이 더 바쁘면 balance 하지 않는다.
 */
		/*
		 * IAMROOT, 2023.05.11:
		 * - balancing 하지 않을 것이므로 migration_type을 지정하지 않는다.
		 */
		if (local->avg_load >= busiest->avg_load) {
			env->imbalance = 0;
			return;
		}

/*
 * IAMROOT, 2023.05.06:
 * - 이 아래는 local이 busiest보다 안바쁜 상태.
 */
	}

	/*
	 * Both group are or will become overloaded and we're trying to get all
	 * the CPUs to the average_load, so we don't want to push ourselves
	 * above the average load, nor do we wish to reduce the max loaded CPU
	 * below the average load. At the same time, we also don't want to
	 * reduce the group load below the group capacity. Thus we look for
	 * the minimum possible imbalance.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   두 그룹 모두 과부하 상태이거나 과부하 상태가 될 것이며 우리는 모든 CPU를 
 *   average_load로 가져오려고 합니다. 따라서 우리는 평균 부하 이상으로 자신을 
 *   밀고 싶지 않으며 최대 부하 CPU를 평균 부하 아래로 낮추고 싶지도 않습니다. 
 *   동시에 우리는 그룹 부하를 그룹 용량 이하로 줄이고 싶지 않습니다. 
 *   따라서 우리는 가능한 최소한의 불균형을 찾습니다.
 *
 * - min((busiest - domain), (domain - local))
 *   busiest는 평균이상, local은 평균이하일 것이다. 평균에서 차이가 덜 나는 것을
 *   기준으로 imbalance를 정한다. minimum possible imbalance로 하고 싶기때문이다.
 */
	env->migration_type = migrate_load;
	env->imbalance = min(
		(busiest->avg_load - sds->avg_load) * busiest->group_capacity,
		(sds->avg_load - local->avg_load) * local->group_capacity
	) / SCHED_CAPACITY_SCALE;
}

/******* find_busiest_group() helpers end here *********************/

/*
 * Decision matrix according to the local and busiest group type:
 *
 * busiest \ local has_spare fully_busy misfit asym imbalanced overloaded
 * has_spare        nr_idle   balanced   N/A    N/A  balanced   balanced
 * fully_busy       nr_idle   nr_idle    N/A    N/A  balanced   balanced
 * misfit_task      force     N/A        N/A    N/A  force      force
 * asym_packing     force     force      N/A    N/A  force      force
 * imbalanced       force     force      N/A    N/A  force      force
 * overloaded       force     force      N/A    N/A  force      avg_load
 *
 * N/A :      Not Applicable because already filtered while updating
 *            statistics.
 * balanced : The system is balanced for these 2 groups.
 * force :    Calculate the imbalance as load migration is probably needed.
 * avg_load : Only if imbalance is significant enough.
 * nr_idle :  dst_cpu is not busy and the number of idle CPUs is quite
 *            different in groups.
 */

/**
 * find_busiest_group - Returns the busiest group within the sched_domain
 * if there is an imbalance.
 *
 * Also calculates the amount of runnable load which should be moved
 * to restore balance.
 *
 * @env: The load balancing environment.
 *
 * Return:	- The busiest group if imbalance exists.
 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   find_busiest_group - 불균형이 있는 경우 sched_domain 내에서 가장 
 *   바쁜 그룹을 반환합니다.
 *
 *   또한 균형을 회복하기 위해 이동해야 하는 실행 가능한 로드의 양을 계산합니다.
 *
 *   @env: 로드 밸런싱 환경.
 *
 *   return: - 불균형이 존재하는 경우 가장 바쁜 그룹. 
 *
 * -가장바쁜 group을 찾는다(단, imbalance는 0보다 커야한다.) 
 *   또한 group_type에 따른 imbalance와 migration_type을 정한다.
 */
static struct sched_group *find_busiest_group(struct lb_env *env)
{
	struct sg_lb_stats *local, *busiest;
	struct sd_lb_stats sds;

	init_sd_lb_stats(&sds);

	/*
	 * Compute the various statistics relevant for load balancing at
	 * this level.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - group을 순회해 통계하여 @sds를 완성한다.
 */
	update_sd_lb_stats(env, &sds);

	if (sched_energy_enabled()) {
		struct root_domain *rd = env->dst_rq->rd;

/*
 * IAMROOT, 2023.05.06:
 * - overutilized은 update_sd_lb_stats()에서 SG_OVERUTILIZED이였을 경우
 *   update되었을것이다.
 * - parition domain이 있고, overutil이 안됬으면 out이다.
 */
		if (rcu_dereference(rd->pd) && !READ_ONCE(rd->overutilized))
			goto out_balanced;
	}

	local = &sds.local_stat;
	busiest = &sds.busiest_stat;

	/* There is no busy sibling group to pull tasks from */
/*
 * IAMROOT, 2023.05.06:
 * - busiest가 없엇으면 out.
 */
	if (!sds.busiest)
		goto out_balanced;

	/* Misfit tasks should be dealt with regardless of the avg load */
/*
 * IAMROOT, 2023.05.06:
 * - group_misfit_task, group_asym_packing, group_imbalance면 force
 *   수행한다.
 * - 남은 group_type
 *   group_has_spare
 *   group_fully_busy,
 *   group_misfit_task,
 *   group_asym_packing,
 *   group_imbalanced,
 *   group_overloaded
 */
	if (busiest->group_type == group_misfit_task)
		goto force_balance;

/*
 * IAMROOT, 2023.05.06:
 * - 남은 group_type
 *   group_has_spare
 *   group_fully_busy,
 *   //group_misfit_task,
 *   group_asym_packing,
 *   group_imbalanced,
 *   group_overloaded
 */
	/* ASYM feature bypasses nice load balance check */
	if (busiest->group_type == group_asym_packing)
		goto force_balance;

/*
 * IAMROOT, 2023.05.06:
 * - 남은 group_type
 *   group_has_spare
 *   group_fully_busy,
 *   //group_misfit_task,
 *   //group_asym_packing,
 *   group_imbalanced,
 *   group_overloaded
 */
	/*
	 * If the busiest group is imbalanced the below checks don't
	 * work because they assume all things are equal, which typically
	 * isn't true due to cpus_ptr constraints and the like.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   가장 바쁜 그룹이 불균형한 경우 아래 검사는 모든 것이 동일하다고 
 *   가정하기 때문에 작동하지 않습니다. 이는 일반적으로 cpus_ptr 제약 
 *   조건 등으로 인해 사실이 아닙니다.
 *
 * - 주석에 따르면 group balance인 경우에만 아래가 동작한다고 한다.
 *   imbalance인경우는 force 진행.
 */
	if (busiest->group_type == group_imbalanced)
		goto force_balance;

/*
 * IAMROOT, 2023.05.06:
 * - 남은 group_type
 *   group_has_spare
 *   group_fully_busy,
 *   //group_misfit_task,
 *   //group_asym_packing,
 *   //group_imbalanced,
 *   group_overloaded
 */
	/*
	 * If the local group is busier than the selected busiest group
	 * don't try and pull any tasks.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - local이 busiest 보다 더 바쁘면 out.
 */
	if (local->group_type > busiest->group_type)
		goto out_balanced;

/*
 * IAMROOT, 2023.05.06:
 * - 남은 group_type. (local group보다 높거나 같은)
 *   group_has_spare
 *   group_fully_busy,
 *   //group_misfit_task,
 *   //group_asym_packing,
 *   //group_imbalanced,
 *   group_overloaded
 */
	/*
	 * When groups are overloaded, use the avg_load to ensure fairness
	 * between tasks.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - local이 overload인 경우 out인 상황을 판단한다.
 *   1. local이 이미 busiest보다 load가 더 걸린경우
 *   2. local이 domain전체보다 load가 더 높은 경우. 
 *   즉 평균보다 자기자신이 더 높은 경우.
 *   3. imbalance을 적용했을때, busiest보다 낮으면 안한다.
 */
	if (local->group_type == group_overloaded) {
		/*
		 * If the local group is more loaded than the selected
		 * busiest group don't try to pull any tasks.
		 */
		if (local->avg_load >= busiest->avg_load)
			goto out_balanced;

		/* XXX broken for overlapping NUMA groups */
		sds.avg_load = (sds.total_load * SCHED_CAPACITY_SCALE) /
				sds.total_capacity;

		/*
		 * Don't pull any tasks if this group is already above the
		 * domain average load.
		 */
		if (local->avg_load >= sds.avg_load)
			goto out_balanced;

		/*
		 * If the busiest group is more loaded, use imbalance_pct to be
		 * conservative.
		 */
		if (100 * busiest->avg_load <=
				env->sd->imbalance_pct * local->avg_load)
			goto out_balanced;
	}

	/* Try to move all excess tasks to child's sibling domain */
/*
 * IAMROOT, 2023.05.06:
 * - fource 수행.
 * 1. 선호 sibling이 존재
 * 2. local 이 idle
 * 3. busiest가 local보다 2개이상 많음.
 */
	if (sds.prefer_sibling && local->group_type == group_has_spare &&
	    busiest->sum_nr_running > local->sum_nr_running + 1)
		goto force_balance;

/*
 * IAMROOT, 2023.05.06:
 * - 남은 group_type. (local group보다 높거나 같은)
 *   group_has_spare <---
 *   group_fully_busy <--
 *   //group_misfit_task,
 *   //group_asym_packing,
 *   //group_imbalanced,
 *   group_overloaded
 */
	if (busiest->group_type != group_overloaded) {
		if (env->idle == CPU_NOT_IDLE)
			/*
			 * If the busiest group is not overloaded (and as a
			 * result the local one too) but this CPU is already
			 * busy, let another idle CPU try to pull task.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   가장 바쁜 그룹이 불균형한 경우 아래 검사는 모든 것이 동일하다고 
 *   가정하기 때문에 작동하지 않습니다. 이는 일반적으로 cpus_ptr 제약 
 *   조건 등으로 인해 사실이 아닙니다.
 */
			goto out_balanced;

/*
 * IAMROOT, 2023.05.06:
 * - busiest에 2개이상의 cpu가 있는데, busiest가 local보다 idle이 더 많은경우
 */
		if (busiest->group_weight > 1 &&
		    local->idle_cpus <= (busiest->idle_cpus + 1))
			/*
			 * If the busiest group is not overloaded
			 * and there is no imbalance between this and busiest
			 * group wrt idle CPUs, it is balanced. The imbalance
			 * becomes significant if the diff is greater than 1
			 * otherwise we might end up to just move the imbalance
			 * on another group. Of course this applies only if
			 * there is more than 1 CPU per group.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   가장 바쁜 그룹이 오버로드되지 않고 이 그룹과 가장 바쁜 그룹 wrt 
 *   유휴 CPU 간에 불균형이 없으면 균형이 잡힌 것입니다. diff가 1보다 
 *   크면 불균형이 중요해집니다. 그렇지 않으면 불균형을 다른 그룹으로 
 *   이동하게 될 수 있습니다. 물론 이는 그룹당 CPU가 1개 이상인 
 *   경우에만 적용됩니다.
 */
			goto out_balanced;

/*
 * IAMROOT, 2023.05.06:
 * - busiest가 한개인 경우 끌고 오면 안된다.
 *   ex) fully busy인데 task를 하나 끌고 와봤자 load가 또 찰뿐일것이다.
 */
		if (busiest->sum_h_nr_running == 1)
			/*
			 * busiest doesn't have any tasks waiting to run
			 */
			goto out_balanced;
	}

force_balance:
	/* Looks like there is an imbalance. Compute it */
	/*
	 * IAMROOT, 2023.05.13:
	 * - migration_type 과 imbalance 값이 결정된다
	 */
	calculate_imbalance(env, &sds);
	return env->imbalance ? sds.busiest : NULL;

out_balanced:
	env->imbalance = 0;
	return NULL;
}

/*
 * find_busiest_queue - find the busiest runqueue among the CPUs in the group.
 */
/*
 * IAMROOT, 2023.05.06:
 * - @group에서 가장 바쁜 cpu를 찾는다
 */
static struct rq *find_busiest_queue(struct lb_env *env,
				     struct sched_group *group)
{
	struct rq *busiest = NULL, *rq;
	unsigned long busiest_util = 0, busiest_load = 0, busiest_capacity = 1;
	unsigned int busiest_nr = 0;
	int i;

	for_each_cpu_and(i, sched_group_span(group), env->cpus) {
		unsigned long capacity, load, util;
		unsigned int nr_running;
		enum fbq_type rt;

		rq = cpu_rq(i);
		rt = fbq_classify_rq(rq);

		/*
		 * We classify groups/runqueues into three groups:
		 *  - regular: there are !numa tasks
		 *  - remote:  there are numa tasks that run on the 'wrong' node
		 *  - all:     there is no distinction
		 *
		 * In order to avoid migrating ideally placed numa tasks,
		 * ignore those when there's better options.
		 *
		 * If we ignore the actual busiest queue to migrate another
		 * task, the next balance pass can still reduce the busiest
		 * queue by moving tasks around inside the node.
		 *
		 * If we cannot move enough load due to this classification
		 * the next pass will adjust the group classification and
		 * allow migration of more tasks.
		 *
		 * Both cases only affect the total convergence complexity.
		 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   우리는 그룹/실행 대기열을 세 그룹으로 분류합니다.
 *   - regular: !numa 작업이 있습니다. 
 *   - remote: '잘못된' 노드에서 실행되는 numa 작업이 있습니다.
 *   - all: 구분이 없습니다.
 *
 *   이상적으로 배치된 누마 작업을 마이그레이션하지 않으려면 더 
 *   나은 옵션이 있을 때 무시하십시오.
 *
 *   다른 작업을 마이그레이션하기 위해 실제 가장 바쁜 대기열을 무시하더라도 
 *   다음 밸런스 패스는 노드 내부에서 작업을 이동하여 가장 바쁜 대기열을 
 *   여전히 줄일 수 있습니다.
 *
 *   이 분류로 인해 충분한 부하를 이동할 수 없는 경우 다음 단계에서 
 *   그룹 분류를 조정하고 더 많은 작업을 마이그레이션할 수 있습니다.
 *
 *   두 경우 모두 전체 수렴 복잡성에만 영향을 미칩니다.
 *
 * - 해당 cpu의 fbq_type이 group fbq_type보다 좋은경우 continue
 * - numa의 경우 busiset 그룹에 preferred node 나 아닌 곳에서 동작하는 cpu가 있다면
 *   그 cpu를 먼저 pull 해오기 위해 preferred node 에서 동작하는 cpu는 건너 뛴다.
 *   또 numa node가 아닌 곳에서 동작하는 cpu가 있을 때도 위와 같이 적용한다.
 */
		if (rt > env->fbq_type)
			continue;

/*
 * IAMROOT, 2023.05.06:
 * - busiest를 찾는데 idle인것은 필요없다. continue
 */
		nr_running = rq->cfs.h_nr_running;
		if (!nr_running)
			continue;

		capacity = capacity_of(i);

		/*
		 * For ASYM_CPUCAPACITY domains, don't pick a CPU that could
		 * eventually lead to active_balancing high->low capacity.
		 * Higher per-CPU capacity is considered better than balancing
		 * average load.
		 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   ASYM_CPUCAPACITY 도메인의 경우 결국 active_balancing 
 *   고용량->저용량으로 이어질 수 있는 CPU를 선택하지 마십시오.
 *   CPU당 용량이 높을수록 평균 로드 균형을 맞추는 것보다 더 나은 
 *   것으로 간주됩니다.
 * - asym인데, task가 1개면서 dest의 capa이 작은쪽이면 continue.
 */
		if (env->sd->flags & SD_ASYM_CPUCAPACITY &&
		    !capacity_greater(capacity_of(env->dst_cpu), capacity) &&
		    nr_running == 1)
			continue;

		switch (env->migration_type) {
		case migrate_load:
			/*
			 * When comparing with load imbalance, use cpu_load()
			 * which is not scaled with the CPU capacity.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   로드 불균형과 비교할 때 CPU 용량으로 스케일링되지 않는 cpu_load()를 사용하십시오.
 */
			load = cpu_load(rq);

/*
 * IAMROOT, 2023.05.06:
 * - task가 1개돌때, cpu load가 imbalance보다 크다면, rq capa가 눈에띄게 줄어든게 아니라면
 *   break
 */
			if (nr_running == 1 && load > env->imbalance &&
			    !check_cpu_capacity(rq, env->sd))
				break;

			/*
			 * For the load comparisons with the other CPUs,
			 * consider the cpu_load() scaled with the CPU
			 * capacity, so that the load can be moved away
			 * from the CPU that is potentially running at a
			 * lower capacity.
			 *
			 * Thus we're looking for max(load_i / capacity_i),
			 * crosswise multiplication to rid ourselves of the
			 * division works out to:
			 * load_i * capacity_j > load_j * capacity_i;
			 * where j is our previous maximum.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   다른 CPU와의 부하 비교를 위해 CPU 용량으로 확장된 cpu_load()를 고려하여 더 
 *   낮은 용량에서 잠재적으로 실행 중인 CPU에서 부하를 이동할 수 있습니다.
 *
 *   따라서 우리는 max(load_i / capacity_i)를 찾고 있습니다. 나눗셈을 없애기 위한 
 *   십자형 곱셈은 다음과 같이 작동합니다.
 *   load_i * capacity_j > load_j * capacity_i; 
 *   여기서 j는 이전 최대값입니다.
 *
 * - 현재의 load, busiest load와 현재의 capa, busiest capa를 cross하여 비교한다.
 *   iterate중인 load가 크다면 busiest로 갱신한다.
 */
			if (load * busiest_capacity > busiest_load * capacity) {
				busiest_load = load;
				busiest_capacity = capacity;
				busiest = rq;
			}
			break;

		case migrate_util:
			util = cpu_util(cpu_of(rq));

			/*
			 * Don't try to pull utilization from a CPU with one
			 * running task. Whatever its utilization, we will fail
			 * detach the task.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   하나의 실행 중인 작업으로 CPU에서 사용률을 끌어오려고 하지 마십시오. 
 *   활용도가 무엇이든 작업 분리에 실패합니다.
 *   
 * - task한개면 굳이 안한다.
 */
			if (nr_running <= 1)
				continue;

/*
 * IAMROOT, 2023.05.06:
 * - 현재 util이 크다면 busiest로 갱신한다.
 */
			if (busiest_util < util) {
				busiest_util = util;
				busiest = rq;
			}
			break;

		case migrate_task:
/*
 * IAMROOT, 2023.05.06:
 * - task숫자로 비교해 갱신한다.
 */
			if (busiest_nr < nr_running) {
				busiest_nr = nr_running;
				busiest = rq;
			}
			break;

		case migrate_misfit:
			/*
			 * For ASYM_CPUCAPACITY domains with misfit tasks we
			 * simply seek the "biggest" misfit task.
			 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   부적합 작업이 있는 ASYM_CPUCAPACITY 도메인의 경우 가장 큰 부적합 
 *   작업을 찾습니다.
 * - misfit_task_load로 비교해 갱신한다.
 */
			if (rq->misfit_task_load > busiest_load) {
				busiest_load = rq->misfit_task_load;
				busiest = rq;
			}

			break;

		}
	}

	return busiest;
}

/*
 * Max backoff if we encounter pinned tasks. Pretty arbitrary value, but
 * so long as it is large enough.
 */
/*
 * IAMROOT. 2023.05.15:
 * - google-translate
 * 고정된 작업이 발생하는 경우 최대 백오프. 꽤 임의의 값이지만 충분히 크면 됩니다.
 */
#define MAX_PINNED_INTERVAL	512

/*
 * IAMROOT, 2023.05.13:
 * - smt 에서 dst_cpu가 우선순위가 높으면 true
 */
static inline bool
asym_active_balance(struct lb_env *env)
{
	/*
	 * ASYM_PACKING needs to force migrate tasks from busy but
	 * lower priority CPUs in order to pack all tasks in the
	 * highest priority CPUs.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * ASYM_PACKING은 우선 순위가 가장 높은 CPU의 모든 작업을 압축하기 위해 바쁘지만
	 * 우선 순위가 낮은 CPU에서 마이그레이션 작업을 강제 실행해야 합니다.
	 */
	return env->idle != CPU_NOT_IDLE && (env->sd->flags & SD_ASYM_PACKING) &&
	       sched_asym_prefer(env->dst_cpu, env->src_cpu);
}

/*
 * IAMROOT, 2023.05.13:
 * - 여유 용량이 있는데 balance 실패횟수가 cache_nice_tries+2보다 클 경우 true 반환
 */
static inline bool
imbalanced_active_balance(struct lb_env *env)
{
	struct sched_domain *sd = env->sd;

	/*
	 * The imbalanced case includes the case of pinned tasks preventing a fair
	 * distribution of the load on the system but also the even distribution of the
	 * threads on a system with spare capacity
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 균형이 맞지 않는 경우에는 시스템에 대한 로드의 공정한 분배를 방해하는 고정된
	 * 작업의 경우뿐만 아니라 여유 용량이 있는 시스템의 스레드의 균등한 분배도
	 * 포함됩니다.
	 * - 여유 용량이 있는데 balance 실패횟수가 cache_nice_tries+2보다 클 경우 true
	 */
	if ((env->migration_type == migrate_task) &&
	    (sd->nr_balance_failed > sd->cache_nice_tries+2))
		return 1;

	return 0;
}

/*
 * IAMROOT, 2023.05.13:
 * - active_balance 조건:
 *   1. smt 에서 dst_cpu가 우선순위가 높다
 *   2. 여유 용량이 있는데 balance 실패횟수가 cache_nice_tries+2보다 클 경우
 *   3. src_cpu의 용량이 감소하여 dst_cpu에서 더 많은 용량을 사용할 수 있는 경우
 *   4. misfit type 인 경우
 */
static int need_active_balance(struct lb_env *env)
{
	struct sched_domain *sd = env->sd;

	if (asym_active_balance(env))
		return 1;

	if (imbalanced_active_balance(env))
		return 1;

	/*
	 * The dst_cpu is idle and the src_cpu CPU has only 1 CFS task.
	 * It's worth migrating the task if the src_cpu's capacity is reduced
	 * because of other sched_class or IRQs if more capacity stays
	 * available on dst_cpu.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * dst_cpu는 유휴 상태이고 src_cpu CPU에는 1개의 CFS 작업만 있습니다. 다른
	 * sched_class 또는 IRQ로 인해 src_cpu의 용량이 감소한 경우 dst_cpu에서 더 많은
	 * 용량을 사용할 수 있는 경우 작업을 마이그레이션할 가치가 있습니다.
	 */
	if ((env->idle != CPU_NOT_IDLE) &&
	    (env->src_rq->cfs.h_nr_running == 1)) {
		if ((check_cpu_capacity(env->src_rq, sd)) &&
		    (capacity_of(env->src_cpu)*sd->imbalance_pct < capacity_of(env->dst_cpu)*100))
			return 1;
	}

	if (env->migration_type == migrate_misfit)
		return 1;

	return 0;
}

static int active_load_balance_cpu_stop(void *data);

/*
 * IAMROOT, 2023.05.06:
 * - return 0
 *   1. dst_cpu와 cpus가 겹치는 범위가 없는 경우
 *   2. balance mask의 idle cpu의 첫번째와 dst_cpu가 불일치 하는 경우
 *   3. idle이 없으면 balance mask의 첫번째가 dest cpu가 아닌 경우
 *
 * - return 1. shulde balance를 하는 경우
 *   1. env->idle의 CPU_NEWLY_IDLE. 무조건 balance 하라는 의미이다.
 *   2. balance mask중에서 idle인 첫번째 cpu가 dest cpu 인 겨우
 *   3. idle이 없으면 balance mask의 첫번째가 dest cpu인 경우
 */
static int should_we_balance(struct lb_env *env)
{
	struct sched_group *sg = env->sd->groups;
	int cpu;

	/*
	 * Ensure the balancing environment is consistent; can happen
	 * when the softirq triggers 'during' hotplug.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   밸런싱 환경이 일관성이 있는지 확인합니다.
 *   softirq가 핫플러그 'during'을 트리거할
 *   때 발생할 수 있습니다.
 */
	if (!cpumask_test_cpu(env->dst_cpu, env->cpus))
		return 0;

	/*
	 * In the newly idle case, we will allow all the CPUs
	 * to do the newly idle load balance.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   새로 유휴 상태인 경우 모든 CPU가 새로 유휴 로드 밸런싱을
 *   수행하도록 허용합니다.
 */
	if (env->idle == CPU_NEWLY_IDLE)
		return 1;

	/* Try to find first idle CPU */
/*
 * IAMROOT, 2023.05.06:
 * - balance mask에서 idle인 cpu중 첫번째가 dst_cpu랑 일치하는지 판별한다
 */
	for_each_cpu_and(cpu, group_balance_mask(sg), env->cpus) {
		if (!idle_cpu(cpu))
			continue;

		/* Are we the first idle CPU? */
		return cpu == env->dst_cpu;
	}

/*
 * IAMROOT, 2023.05.06:
 * - idle cpu가 없는경우 balance mask의 첫번째 cpu가 dst_cpu와 일치하는
 *   지 비교한다.
 */
	/* Are we the first CPU of this group ? */
	return group_balance_cpu(sg) == env->dst_cpu;
}

/*
 * Check this_cpu to ensure it is balanced within domain. Attempt to move
 * tasks if there is an imbalance.
 */
/*
 * IAMROOT. 2023.05.11:
 * - google-translate
 * this_cpu를 확인하여 도메인 내에서 균형이 맞는지 확인하십시오. 불균형이 있는 경우
 * 작업 이동을 시도합니다.
 *
 * - rebalance_domains 에서 호출되었을 때 인자
 *   @this_cpu: softirq를 처리하는 cpu
 *   @this_rq: @this_cpu의 rq
 *   @sd: @this_cpu의 최하위 domain 부터 최상위 까지 loop
 *   @idle: CPU_IDLE 또는 CPU_NOT_IDLE
 */
static int load_balance(int this_cpu, struct rq *this_rq,
			struct sched_domain *sd, enum cpu_idle_type idle,
			int *continue_balancing)
{
	int ld_moved, cur_ld_moved, active_balance = 0;
	struct sched_domain *sd_parent = sd->parent;
	struct sched_group *group;
	struct rq *busiest;
	struct rq_flags rf;
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(load_balance_mask);

	/*
	 * IAMROOT, 2023.05.11:
	 * - .sd: dst_cpu의 최하위 domain 부터 최상위 까지 loop
	 *   .dst_cpu: softirq를 처리하는 cpu
	 *   .dst_rq: dst_cpu의 rq
	 *   .dst_grpmask: sd 의 첫번째 그룹
	 *   .cpus: 아래에서 domain_span & active_mask
	 */
	struct lb_env env = {
		.sd		= sd,
		.dst_cpu	= this_cpu,
		.dst_rq		= this_rq,
		.dst_grpmask    = sched_group_span(sd->groups),
		.idle		= idle,
		.loop_break	= sched_nr_migrate_break,
		.cpus		= cpus,
		.fbq_type	= all,
		.tasks		= LIST_HEAD_INIT(env.tasks),
	};

/*
 * IAMROOT, 2023.05.06:
 * - acive에서 span과 겹치는 범위를 cpus에 저장한다.
 */
	cpumask_and(cpus, sched_domain_span(sd), cpu_active_mask);

	schedstat_inc(sd->lb_count[idle]);

redo:
/*
 * IAMROOT, 2023.05.06:
 * - @idle이 CPU_NEWLY_IDLE가 아니였다면, balance cpu중에
 *   dest cpu가 있는지 확인한다. 없다면 out_balanced.
 */
	if (!should_we_balance(&env)) {
		*continue_balancing = 0;
		goto out_balanced;
	}

/*
 * IAMROOT, 2023.05.06:
 * - busiest group을 찾고, 해당 group에서 가장 바쁜 cpu를 찾는다.
 */
	group = find_busiest_group(&env);
	if (!group) {
		schedstat_inc(sd->lb_nobusyg[idle]);
		goto out_balanced;
	}

	busiest = find_busiest_queue(&env, group);
	if (!busiest) {
		schedstat_inc(sd->lb_nobusyq[idle]);
		goto out_balanced;
	}

	BUG_ON(busiest == env.dst_rq);

	schedstat_add(sd->lb_imbalance[idle], env.imbalance);

	env.src_cpu = busiest->cpu;
	env.src_rq = busiest;

	ld_moved = 0;
	/* Clear this flag as soon as we find a pullable task */
/*
 * IAMROOT, 2023.05.15: 
 * LBF_ALL_PINNED: 
 * 이 플래그는 최초 설정으로 migration할 수 있는 task가 하나도 없는 상태입니다.
 * 만일 하나라도 pull이 가능한 태스크가 발견되면 이 플래그를 클리어합니다.
 * - 일단 all pinned로 설정해놓고, 하나라도 migrate가능한 task가 찾아진다면
 *   거기서 clear한다.
 */
	env.flags |= LBF_ALL_PINNED;
	if (busiest->nr_running > 1) {
		/*
		 * Attempt to move tasks. If find_busiest_group has found
		 * an imbalance but busiest->nr_running <= 1, the group is
		 * still unbalanced. ld_moved simply stays zero, so it is
		 * correctly treated as an imbalance.
		 */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * 작업 이동을 시도합니다. find_busiest_group이 불균형을 발견했지만
		 * busiest->nr_running <= 1인 경우 그룹은 여전히 불균형입니다.
		 * ld_moved는 단순히 0으로 유지되므로 불균형으로 올바르게 처리됩니다.
		 */
		env.loop_max  = min(sysctl_sched_nr_migrate, busiest->nr_running);

more_balance:
		rq_lock_irqsave(busiest, &rf);
		update_rq_clock(busiest);

		/*
		 * cur_ld_moved - load moved in current iteration
		 * ld_moved     - cumulative load moved across iterations
		 */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * cur_ld_moved - 현재 반복에서 이동된 로드
		 * ld_moved - 반복 간에 이동된 누적 로드
		 */
		cur_ld_moved = detach_tasks(&env);

		/*
		 * We've detached some tasks from busiest_rq. Every
		 * task is masked "TASK_ON_RQ_MIGRATING", so we can safely
		 * unlock busiest->lock, and we are able to be sure
		 * that nobody can manipulate the tasks in parallel.
		 * See task_rq_lock() family for the details.
		 */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * busiest_rq에서 일부 작업을 분리했습니다. 모든 작업은
		 * "TASK_ON_RQ_MIGRATING"으로 마스킹되어 가장 바쁜->잠금을 안전하게
		 * 해제할 수 있으며 아무도 병렬로 작업을 조작할 수 없도록 할 수 있습니다.
		 * 자세한 내용은 task_rq_lock() 제품군을 참조하세요.
		 */

		rq_unlock(busiest, &rf);

		if (cur_ld_moved) {
			attach_tasks(&env);
			ld_moved += cur_ld_moved;
		}

		local_irq_restore(rf.flags);

/*
 * IAMROOT, 2023.05.19:
 * - loop_break동안 balance가 안됬을때, 너무 오래 lock을 잡고있었으므로
 *   lock을 한번 해제해서 한번 선점을 풀었다가 다시 시도한다.
 */
		if (env.flags & LBF_NEED_BREAK) {
			env.flags &= ~LBF_NEED_BREAK;
			goto more_balance;
		}

		/*
		 * Revisit (affine) tasks on src_cpu that couldn't be moved to
		 * us and move them to an alternate dst_cpu in our sched_group
		 * where they can run. The upper limit on how many times we
		 * iterate on same src_cpu is dependent on number of CPUs in our
		 * sched_group.
		 *
		 * This changes load balance semantics a bit on who can move
		 * load to a given_cpu. In addition to the given_cpu itself
		 * (or a ilb_cpu acting on its behalf where given_cpu is
		 * nohz-idle), we now have balance_cpu in a position to move
		 * load to given_cpu. In rare situations, this may cause
		 * conflicts (balance_cpu and given_cpu/ilb_cpu deciding
		 * _independently_ and at _same_ time to move some load to
		 * given_cpu) causing excess load to be moved to given_cpu.
		 * This however should not happen so much in practice and
		 * moreover subsequent load balance cycles should correct the
		 * excess load moved.
		 */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * 우리에게 이동할 수 없는 src_cpu의 작업(아핀)을 다시 방문하여 실행할
		 * 수 있는 sched_group의 대체 dst_cpu로 이동합니다. 동일한 src_cpu에서
		 * 반복하는 횟수의 상한은 sched_group의 CPU 수에 따라 다릅니다.
		 *
		 * 이것은
		 * 로드 밸런스 시맨틱을 누가 given_cpu로 이동할 수 있는지에 대해 약간
		 * 변경합니다. given_cpu 자체(또는 given_cpu가 nohz-idle인 경우 대신
		 * 작동하는 ilb_cpu) 외에도 이제 balance_cpu가 given_cpu로 부하를
		 * 이동할 위치에 있습니다. 드물게 충돌이 발생할 수 있습니다(balance_cpu
		 * 및 given_cpu/ilb_cpu가 _독립적으로_ 그리고 _동일_ 시간에 일부 로드를
		 * given_cpu로 이동하도록 결정) 과도한 로드가 given_cpu로 이동되도록
		 * 합니다. 그러나 이것은 실제로 그렇게 많이 발생하지 않아야 하며, 또한
		 * 후속 로드 균형 주기는 이동된 초과 로드를 수정해야 합니다.
		 *
		 * - dst_cpu 가 available 하지 않아 new_dst_cpu로 교체해서
		 *   재시도
		 * - dst를 범위에서 지운후 new_dst_cpu로 교체한다.
		 *   새로운 dst가 설정되었으므로 dst pinned를 풀고 loop 횟수도 초기화
		 *   한다.
		 */
		if ((env.flags & LBF_DST_PINNED) && env.imbalance > 0) {

			/* Prevent to re-select dst_cpu via env's CPUs */
			__cpumask_clear_cpu(env.dst_cpu, env.cpus);

			env.dst_rq	 = cpu_rq(env.new_dst_cpu);
			env.dst_cpu	 = env.new_dst_cpu;
			env.flags	&= ~LBF_DST_PINNED;
			env.loop	 = 0;
			env.loop_break	 = sched_nr_migrate_break;

			/*
			 * Go back to "more_balance" rather than "redo" since we
			 * need to continue with same src_cpu.
			 */
			/*
			 * IAMROOT. 2023.05.12:
			 * - google-translate
			 * 동일한 src_cpu로 계속해야 하므로 "redo"가 아닌
			 * "more_balance"로 돌아갑니다.
			 */
			goto more_balance;
		}

		/*
		 * We failed to reach balance because of affinity.
		 */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * 친화력 때문에 균형에 도달하지 못했습니다.
		 * - cpu 제한으로 balance를 할 수 없어 부모 도메인에서 시도하도록
		 *   sd_parent->groups->sgc->imbalance를 1로 설정한다.
		 *   (sg_imbalanced() 사용처 확인)
		 */
		if (sd_parent) {
			int *group_imbalance = &sd_parent->groups->sgc->imbalance;

			if ((env.flags & LBF_SOME_PINNED) && env.imbalance > 0)
				*group_imbalance = 1;
		}

		/* All tasks on this runqueue were pinned by CPU affinity */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * 이 실행 대기열의 모든 작업은 CPU 선호도에 의해 고정되었습니다.
		 */
		if (unlikely(env.flags & LBF_ALL_PINNED)) {
			__cpumask_clear_cpu(cpu_of(busiest), cpus);
			/*
			 * Attempting to continue load balancing at the current
			 * sched_domain level only makes sense if there are
			 * active CPUs remaining as possible busiest CPUs to
			 * pull load from which are not contained within the
			 * destination group that is receiving any migrated
			 * load.
			 */
			/*
			 * IAMROOT. 2023.05.13:
			 * - google-translate
			 * 현재 sched_domain 수준에서 로드 밸런싱을 계속하려는 시도는
			 * 마이그레이션된 로드를 수신하는 대상 그룹에 포함되지 않은
			 * 로드를 끌어올 수 있는 가장 바쁜 CPU로 남아 있는 활성
			 * CPU가 있는 경우에만 의미가 있습니다.
			 * - dst_grpmask에 없는 cpus 가 아직 있으니 재시도 한다.
			 *
			 * IAMROOT. 2023.05.09:
			 * - cpus 가 domain span 이므로 당연히 sd->groups 의 subset
			 *   이 아니지만 위에서 계속 clear 된다면 subset 이 될 수도
			 *   있다.
			 */
			if (!cpumask_subset(cpus, env.dst_grpmask)) {
				env.loop = 0;
				env.loop_break = sched_nr_migrate_break;
				goto redo;
			}
			goto out_all_pinned;
		}
	}

	/*
	 * IAMROOT, 2023.05.18:
	 * - 1. busiest rq에 nr_running 이 1 이하 이거나
	 *   2. 2이상이지만 하나도 옮기지 못한 경우
	 *   아래로 진입한다.
	 *   2의 경우는 detach_tasks 에서 사용한 list와 같을 것이다
	 */
	if (!ld_moved) {
		schedstat_inc(sd->lb_failed[idle]);
		/*
		 * Increment the failure counter only on periodic balance.
		 * We do not want newidle balance, which can be very
		 * frequent, pollute the failure counter causing
		 * excessive cache_hot migrations and active balances.
		 */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * periodic balance에서만 실패 카운터를 증가시킵니다. 우리는 매우
		 * 빈번할 수 있는 newidle 균형이 실패 카운터를 오염시켜 과도한 cache_hot
		 * 마이그레이션 및 활성 균형을 유발하는 것을 원하지 않습니다.
		 */
		if (idle != CPU_NEWLY_IDLE)
			sd->nr_balance_failed++;

		/*
		 * IAMROOT, 2023.05.13:
		 * - active balance 의 src는 busiest의 curr이다.
		 */
		if (need_active_balance(&env)) {
			unsigned long flags;

			raw_spin_rq_lock_irqsave(busiest, flags);

			/*
			 * Don't kick the active_load_balance_cpu_stop,
			 * if the curr task on busiest CPU can't be
			 * moved to this_cpu:
			 */
			/*
			 * IAMROOT. 2023.05.13:
			 * - google-translate
			 * 가장 바쁜 CPU의 현재 작업을 this_cpu로 이동할 수 없는 경우
			 * active_load_balance_cpu_stop을 시작하지 마십시오.
			 */
			if (!cpumask_test_cpu(this_cpu, busiest->curr->cpus_ptr)) {
				raw_spin_rq_unlock_irqrestore(busiest, flags);
				goto out_one_pinned;
			}

			/* Record that we found at least one task that could run on this_cpu */
			/*
			 * IAMROOT. 2023.05.13:
			 * - google-translate
			 * this_cpu에서 실행할 수 있는 작업을 하나 이상 찾았음을
			 * 기록합니다.
			 */
			env.flags &= ~LBF_ALL_PINNED;

			/*
			 * ->active_balance synchronizes accesses to
			 * ->active_balance_work.  Once set, it's cleared
			 * only after active load balance is finished.
			 */
			/*
			 * IAMROOT. 2023.05.13:
			 * - google-translate
			 * ->active_balance는 ->active_balance_work에 대한
			 * 액세스를 동기화합니다. 일단  설정되면 활성 부하 분산이
			 * 완료된 후에만 지워집니다.
			 */
			if (!busiest->active_balance) {
				busiest->active_balance = 1;
				busiest->push_cpu = this_cpu;
				active_balance = 1;
			}
			raw_spin_rq_unlock_irqrestore(busiest, flags);

			if (active_balance) {
				stop_one_cpu_nowait(cpu_of(busiest),
					active_load_balance_cpu_stop, busiest,
					&busiest->active_balance_work);
			}
		}
	} else {
		/*
		 * IAMROOT, 2023.05.13:
		 * - ld_moved 가 0이 아닌 경우
		 */
		sd->nr_balance_failed = 0;
	}

/*
 * IAMROOT, 2023.05.19:
 * - lb_moved의 유무에 상관없이 active_balance를 안했거나, active_balance가
 *   필요한 상황이면 unbalanced라고 판단해 min interval로 고친다.
 */
	/*
	 * IAMROOT, 2023.05.16:
	 * - 아래 경우는 min_interval로 지정하여 빠르게 재시도 하도록 한다.
	 *   1. active_balance 가 0 인 경우
	 *      1. ld_moved 가 0이 아닌 경우, 즉 pull 에서 하나이상 옮긴 경우
	 *      2. need_active_balance 가 0 인 경우. 즉 active_balance
	 *         조건에 해당하지 않는 경우
	 *   2. active_balance 가 1 인 경우
	 *      1. need_active_balance 가 1인 경우. 즉 active_balance를
	 *         실행했지만 아직 active_balance 조건인 경우. push migration에
	 *         실패 한 경우(검증 필요)
	 */
	if (likely(!active_balance) || need_active_balance(&env)) {
		/* We were unbalanced, so reset the balancing interval */
		/*
		 * IAMROOT. 2023.05.13:
		 * - google-translate
		 * 균형이 맞지 않았으므로 균형 간격을 재설정하십시오.
		 */
		sd->balance_interval = sd->min_interval;
	}

	goto out;

out_balanced:
	/*
	 * We reach balance although we may have faced some affinity
	 * constraints. Clear the imbalance flag only if other tasks got
	 * a chance to move and fix the imbalance.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   약간의 선호도 제약에 직면했을 수 있지만 균형에 도달합니다. 
 *   다른 작업이 이동하고 불균형을 수정할 기회가 있는 경우에만 
 *   불균형 플래그를 지우십시오.
 * - 
 */
	if (sd_parent && !(env.flags & LBF_ALL_PINNED)) {
		int *group_imbalance = &sd_parent->groups->sgc->imbalance;

		if (*group_imbalance)
			*group_imbalance = 0;
	}

out_all_pinned:
	/*
	 * We reach balance because all tasks are pinned at this level so
	 * we can't migrate them. Let the imbalance flag set so parent level
	 * can try to migrate them.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - papago
 *   모든 작업이 이 수준에 고정되어 마이그레이션할 수 없기 때문에 
 *   균형에 도달합니다. 부모 수준에서 마이그레이션을 시도할 수 있도록 
 *   불균형 플래그를 설정하십시오.
 */
	schedstat_inc(sd->lb_balanced[idle]);

	/*
	 * IAMROOT, 2023.05.13:
	 * - 해당 도메인은 고정 되었으므로 failed를 0으로 초기화
	 */
	sd->nr_balance_failed = 0;

out_one_pinned:
	ld_moved = 0;

	/*
	 * newidle_balance() disregards balance intervals, so we could
	 * repeatedly reach this code, which would lead to balance_interval
	 * skyrocketing in a short amount of time. Skip the balance_interval
	 * increase logic to avoid that.
	 */
/*
 * IAMROOT, 2023.05.06:
 * - newidle_balance()는 균형 간격을 무시하므로 이 코드에 반복적으로 
 *   도달할 수 있으며 짧은 시간 내에 balance_interval이 급등할 수 
 *   있습니다. 이를 방지하려면 balance_interval 증가 로직을 건너뛰십시오.
 */
	if (env.idle == CPU_NEWLY_IDLE)
		goto out;

	/* tune up the balancing interval */
	/*
	 * IAMROOT, 2023.05.13:
	 * - balance 가 어느 정도 맞았다면 interval을 2배로 설정
	 */
	if ((env.flags & LBF_ALL_PINNED &&
	     sd->balance_interval < MAX_PINNED_INTERVAL) ||
	    sd->balance_interval < sd->max_interval)
		sd->balance_interval *= 2;
out:
	return ld_moved;
}

/*
 * IAMROOT, 2023.04.29:
 * - @sd 에 해당하는 interval을 알아온다.
 *   busy 일 경우는 interval(ms)*busy_factor -1(tick) 한다
 * - cpu_busy일경우 busy_factor배수 interval시간을 늘려준다는 개념.
 */
static inline unsigned long
get_sd_balance_interval(struct sched_domain *sd, int cpu_busy)
{
	unsigned long interval = sd->balance_interval;

	if (cpu_busy)
		interval *= sd->busy_factor;

	/* scale ms to jiffies */
	interval = msecs_to_jiffies(interval);

	/*
	 * Reduce likelihood of busy balancing at higher domains racing with
	 * balancing at lower domains by preventing their balancing periods
	 * from being multiples of each other.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * 밸런싱 기간이 서로 배수가 되는 것을 방지하여 하위 도메인에서 밸런싱과 경쟁하는
	 * 상위 도메인에서 바쁜 밸런싱 가능성을 줄입니다.
	 */
	if (cpu_busy)
		interval -= 1;

	interval = clamp(interval, 1UL, max_load_balance_interval);

	return interval;
}

/*
 * IAMROOT, 2023.04.29:
 * - 다음 balancing 주기를 설정한다
 * - next_balance가 cpu not busy를 기준으로 balance_interval만을 고려하여,
 *   너무 빠를 경우 last_balance + balance_interval로 수정한다.
 * - cpu not busy를 기준으로 balance_interval만을 고려하여,
 *   *next_balance < last_balance + balance_interval 인 경우에만 한해
 *   next_balance를 더 나중으로 고친다.
 */
static inline void
update_next_balance(struct sched_domain *sd, unsigned long *next_balance)
{
	unsigned long interval, next;

	/* used by idle balance, so cpu_busy = 0 */
	interval = get_sd_balance_interval(sd, 0);
	next = sd->last_balance + interval;

	if (time_after(*next_balance, next))
		*next_balance = next;
}

/*
 * active_load_balance_cpu_stop is run by the CPU stopper. It pushes
 * running tasks off the busiest CPU onto idle CPUs. It requires at
 * least 1 task to be running on each physical CPU where possible, and
 * avoids physical / logical imbalances.
 */
/*
 * IAMROOT. 2023.05.13:
 * - google-translate
 * active_load_balance_cpu_stop은 CPU 스토퍼에 의해 실행됩니다. 가장 바쁜 CPU에서
 * 실행 중인 작업을 유휴 CPU로 푸시합니다. 가능한 경우 각 물리적 CPU에서 최소 1개의
 * 작업을 실행해야 하며 물리적/논리적 불균형을 방지합니다.
 */
/*
 * IAMROOT, 2023.05.13:
 * - busiest rq 에서 target rq로 옮긴다.
 */
static int active_load_balance_cpu_stop(void *data)
{
	struct rq *busiest_rq = data;
	int busiest_cpu = cpu_of(busiest_rq);
	int target_cpu = busiest_rq->push_cpu;
	struct rq *target_rq = cpu_rq(target_cpu);
	struct sched_domain *sd;
	struct task_struct *p = NULL;
	struct rq_flags rf;

	rq_lock_irq(busiest_rq, &rf);
	/*
	 * Between queueing the stop-work and running it is a hole in which
	 * CPUs can become inactive. We should not move tasks from or to
	 * inactive CPUs.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 중지 작업을 대기하고 실행하는 사이에는 CPU가 비활성화될 수 있는 구멍이
	 * 있습니다. 비활성 CPU에서 또는 비활성 CPU로 작업을 이동해서는 안 됩니다.
	 */
	if (!cpu_active(busiest_cpu) || !cpu_active(target_cpu))
		goto out_unlock;

	/* Make sure the requested CPU hasn't gone down in the meantime: */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 그 동안 요청된 CPU가 다운되지 않았는지 확인합니다.
	 */
	if (unlikely(busiest_cpu != smp_processor_id() ||
		     !busiest_rq->active_balance))
		goto out_unlock;

	/* Is there any task to move? */
	/*
	 * IAMROOT, 2023.05.18:
	 * - active balance 이므로 stopper 외에 다른 task 가 있어야 한다.
	 */
	if (busiest_rq->nr_running <= 1)
		goto out_unlock;

	/*
	 * This condition is "impossible", if it occurs
	 * we need to fix it. Originally reported by
	 * Bjorn Helgaas on a 128-CPU setup.
	 */
	/*
	 * IAMROOT. 2023.05.13:
	 * - google-translate
	 * 이 조건은 "불가능"하며 발생하면 수정해야 합니다. 원래 Bjorn Helgaas가 128-CPU
	 * 설정에서 보고했습니다.
	 */
	BUG_ON(busiest_rq == target_rq);

	/* Search for an sd spanning us and the target CPU. */
	rcu_read_lock();
	for_each_domain(target_cpu, sd) {
		if (cpumask_test_cpu(busiest_cpu, sched_domain_span(sd)))
			break;
	}

	if (likely(sd)) {
		struct lb_env env = {
			.sd		= sd,
			.dst_cpu	= target_cpu,
			.dst_rq		= target_rq,
			.src_cpu	= busiest_rq->cpu,
			.src_rq		= busiest_rq,
			.idle		= CPU_IDLE,
			.flags		= LBF_ACTIVE_LB,
		};

		schedstat_inc(sd->alb_count);
		update_rq_clock(busiest_rq);

		p = detach_one_task(&env);
		if (p) {
			schedstat_inc(sd->alb_pushed);
			/* Active balancing done, reset the failure counter. */
			sd->nr_balance_failed = 0;
		} else {
			schedstat_inc(sd->alb_failed);
		}
	}
	rcu_read_unlock();
out_unlock:
	busiest_rq->active_balance = 0;
	rq_unlock(busiest_rq, &rf);

	if (p)
		attach_one_task(target_rq, p);

	local_irq_enable();

	return 0;
}

static DEFINE_SPINLOCK(balancing);

/*
 * Scale the max load_balance interval with the number of CPUs in the system.
 * This trades load-balance latency on larger machines for less cross talk.
 */
void update_max_interval(void)
{
	max_load_balance_interval = HZ*num_online_cpus()/10;
}

/*
 * It checks each scheduling domain to see if it is due to be balanced,
 * and initiates a balancing operation if so.
 *
 * Balancing parameters are set up in init_sched_domains.
 */
/*
 * IAMROOT. 2023.04.29:
 * - google-translate
 * 각 스케줄링 도메인이 균형을 이루어야 하는지 확인하고 균형이 맞으면 균형 작업을
 * 시작합니다. 밸런싱 매개변수는 init_sched_domains에 설정됩니다.
 * - ING
 */
static void rebalance_domains(struct rq *rq, enum cpu_idle_type idle)
{
	int continue_balancing = 1;
	int cpu = rq->cpu;
	/*
	 * IAMROOT, 2023.04.29:
	 * - busy 는 NOT_IDLE 이면서 SCHED_IDLE task 는 제외
	 */
	int busy = idle != CPU_IDLE && !sched_idle_cpu(cpu);
	unsigned long interval;
	struct sched_domain *sd;
	/* Earliest time when we have to do rebalance again */
	unsigned long next_balance = jiffies + 60*HZ;
	int update_next_balance = 0;
	int need_serialize, need_decay = 0;
	u64 max_cost = 0;

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		/*
		 * Decay the newidle max times here because this is a regular
		 * visit to all the domains. Decay ~1% per second.
		 */
		/*
		 * IAMROOT. 2023.04.29:
		 * - google-translate
		 * 이것은 모든 도메인을 정기적으로 방문하기 때문에 여기에서 newidle
		 * 최대 시간을 감소시킵니다. 초당 ~1% 감소.
		 */
		if (time_after(jiffies, sd->next_decay_max_lb_cost)) {
			sd->max_newidle_lb_cost =
				(sd->max_newidle_lb_cost * 253) / 256;
			sd->next_decay_max_lb_cost = jiffies + HZ;
			need_decay = 1;
		}
		max_cost += sd->max_newidle_lb_cost;

		/*
		 * Stop the load balance at this level. There is another
		 * CPU in our sched group which is doing load balancing more
		 * actively.
		 */
		if (!continue_balancing) {
			if (need_decay)
				continue;
			break;
		}

		interval = get_sd_balance_interval(sd, busy);

		/*
		 * IAMROOT, 2023.04.29:
		 * - numa의 경우는 순차적으로 진행
		 */
		need_serialize = sd->flags & SD_SERIALIZE;
		if (need_serialize) {
			if (!spin_trylock(&balancing))
				goto out;
		}

		if (time_after_eq(jiffies, sd->last_balance + interval)) {
			if (load_balance(cpu, rq, sd, idle, &continue_balancing)) {
				/*
				 * The LBF_DST_PINNED logic could have changed
				 * env->dst_cpu, so we can't know our idle
				 * state even if we migrated tasks. Update it.
				 */
				idle = idle_cpu(cpu) ? CPU_IDLE : CPU_NOT_IDLE;
				busy = idle != CPU_IDLE && !sched_idle_cpu(cpu);
			}
			sd->last_balance = jiffies;
			interval = get_sd_balance_interval(sd, busy);
		}
		if (need_serialize)
			spin_unlock(&balancing);
out:
		if (time_after(next_balance, sd->last_balance + interval)) {
			next_balance = sd->last_balance + interval;
			update_next_balance = 1;
		}
	}
	if (need_decay) {
		/*
		 * Ensure the rq-wide value also decays but keep it at a
		 * reasonable floor to avoid funnies with rq->avg_idle.
		 */
		/*
		 * IAMROOT. 2023.04.29:
		 * - google-translate
		 * rq-wide 값도 감소하는지 확인하되 rq->avg_idle로 재미를 피하기 위해
		 * 합리적인 바닥에 유지하십시오.
		 */
		rq->max_idle_balance_cost =
			max((u64)sysctl_sched_migration_cost, max_cost);
	}
	rcu_read_unlock();

	/*
	 * next_balance will be updated only when there is a need.
	 * When the cpu is attached to null domain for ex, it will not be
	 * updated.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * next_balance는 필요할 때만 업데이트됩니다. 예를 들어 CPU가 null 도메인에
	 * 연결되면 업데이트되지 않습니다.
	 */
	if (likely(update_next_balance))
		rq->next_balance = next_balance;

}

/*
 * IAMROOT, 2023.04.29:
 * - schedule domain 이 지정되지 않았을 때
 */
static inline int on_null_domain(struct rq *rq)
{
	return unlikely(!rcu_dereference_sched(rq->sd));
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * idle load balancing details
 * - When one of the busy CPUs notice that there may be an idle rebalancing
 *   needed, they will kick the idle load balancer, which then does idle
 *   load balancing for all the idle CPUs.
 * - HK_FLAG_MISC CPUs are used for this task, because HK_FLAG_SCHED not set
 *   anywhere yet.
 */
/*
 * IAMROOT, 2023.05.18:
 * - papago
 *   유휴 로드 밸런싱 세부 정보.
 *   - 사용 중인 CPU 중 하나가 유휴 재조정이 필요할 수 있음을 알게 되면 
 *   유휴 로드 밸런서를 중지한 다음 모든 유휴 CPU에 대해 유휴 로드 밸런싱을 
 *   수행합니다.
 *   - HK_FLAG_SCHED가 아직 어디에도 설정되지 않았기 때문에 HK_FLAG_MISC CPU가 
 *   이 작업에 사용됩니다.
 *
 * - HK_FLAG_MISC가 set 되있는 nohz idle_cpu 중에서 idle인 가장 빠른
 *   cpu 한개를 가져온다.
 */
static inline int find_new_ilb(void)
{
	int ilb;
	const struct cpumask *hk_mask;

	hk_mask = housekeeping_cpumask(HK_FLAG_MISC);

	for_each_cpu_and(ilb, nohz.idle_cpus_mask, hk_mask) {

		if (ilb == smp_processor_id())
			continue;

		if (idle_cpu(ilb))
			return ilb;
	}

	return nr_cpu_ids;
}

/*
 * Kick a CPU to do the nohz balancing, if it is time for it. We pick any
 * idle CPU in the HK_FLAG_MISC housekeeping set (if there is one).
 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   Nohz 밸런싱을 수행할 시간이 되면 CPU를 걷어차십시오. HK_FLAG_MISC 
 *   하우스키핑 세트(있는 경우)에서 유휴 CPU를 선택합니다.
 *
 * - NOHZ_BALANCE_KICK이 있는경우 next_balance를 현재시간으로 update한다.
 *   ilb cpu가 없거나, 이미 실행이 안된다해도 next_balance는 update 될것이다.
 *
 * - NOHZ_STATS_KICK
 *   해당 flag만 있을경우 next_balance를 update안하고 수행을 시도할것이다.
 *
 * - HK_FLAG_MISC가 있는 nohz idle cpu를 찾고, 해당 cpu에 nohz_csd_func를
 *   수행하도록 하거나 예약(list 추가, 이미 있으면 ipi요청) 한다.
 *   궁극적으로 해당 cpu에 softirq를 통해 run_rebalance_domains를
 *   수행하게 한다.
 */
static void kick_ilb(unsigned int flags)
{
	int ilb_cpu;

	/*
	 * Increase nohz.next_balance only when if full ilb is triggered but
	 * not if we only update stats.
	 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   전체 ilb가 트리거되는 경우에만 nohz.next_balance를 늘리고 통계만 
 *   업데이트하는 경우에는 늘리지 않습니다.
 */
	if (flags & NOHZ_BALANCE_KICK)
		nohz.next_balance = jiffies+1;

	ilb_cpu = find_new_ilb();

/*
 * IAMROOT, 2023.05.18:
 * - HK_FLAG_MISC가 set되있는 nohz idle cpu가 없다면 return.
 */
	if (ilb_cpu >= nr_cpu_ids)
		return;

	/*
	 * Access to rq::nohz_csd is serialized by NOHZ_KICK_MASK; he who sets
	 * the first flag owns it; cleared by nohz_csd_func().
	 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   rq::nohz_csd에 대한 액세스는 NOHZ_KICK_MASK에 의해 직렬화됩니다. 
 *   첫 번째 깃발을 세우는 사람이 그것을 소유합니다. nohz_csd_func()에 
 *   의해 지워집니다.
 *
 * - atomic write로 권한 획득. 이미 flags가 있다면 누군가 요청했다는 것이므로
 *   return.
 */
	flags = atomic_fetch_or(flags, nohz_flags(ilb_cpu));
	if (flags & NOHZ_KICK_MASK)
		return;

	/*
	 * This way we generate an IPI on the target CPU which
	 * is idle. And the softirq performing nohz idle load balance
	 * will be run before returning from the IPI.
	 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   이렇게 하면 유휴 상태인 대상 CPU에서 IPI를 생성합니다. 그리고 
 *   nohz 유휴 부하 균형을 수행하는 softirq는 IPI에서 반환되기 
 *   전에 실행됩니다.
 *
 * - @cpu가 this cpu면 nohz_csd_func를 호출하고, 그게 아니면 @cpu의 csd list에 
 *   예약해놓는다.
 */
	smp_call_function_single_async(ilb_cpu, &cpu_rq(ilb_cpu)->nohz_csd);
}

/*
 * Current decision point for kicking the idle load balancer in the presence
 * of idle CPUs in the system.
 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   시스템에 유휴 CPU가 있는 경우 유휴 로드 밸런서를 제거하기 위한 현재 결정
 *   지점입니다.
 *
 * - flags를 정하고 거기에 따라 kick_ilb를 한다.
 *   -- 아무것도 안함.
 *      1. 이미 idle_balance 중
 *      2. nohz cpu가 하나도 없음.
 *      3. now <= nohz.next_balance
 *         (has_blocked == false거나, true여도 now <= next_blocked 인경우)
 *      4. @rq cpu가 sd_asym_cpucapacity에 있는 상황에서,
 *         @rq에 misfit load가 없거나, 있어도 더 좋은 cpu로 옮길수 없는 경우
 *
 *   -- NOHZ_STATS_KICK
 *      1. now <= nohz.next_balance
 *         (has_blocked == true, now > next_blocked 인경우)
 *
 *   -- NOHZ_KICK_MASK
 *      1. @rq에 running이 2개이상인 경우
 *      2. @rq에 cfs task가 있고, cpu 용량이 줄어든 경우
 *      3. @cpu의 asym pack에서, idle cpu중에 @cpu보다 높은 우선순위가 
 *      있는 경우
 *      4. @rq cpu가 sd_asym_cpucapacity에 있는 상황에서,
 *         @rq에 misfit load가 있고, 더 좋은 cpu로 옮길수 있는 경우
 *
 * IAMROOT. 2023.05.20:
 * - google-translate
 * 시스템에 유휴 CPU가 있는 경우 유휴 로드 밸런서를 제거하기 위한 현재 결정
 * 지점입니다.
 */
static void nohz_balancer_kick(struct rq *rq)
{
	unsigned long now = jiffies;
	struct sched_domain_shared *sds;
	struct sched_domain *sd;
	int nr_busy, i, cpu = rq->cpu;
	unsigned int flags = 0;

	if (unlikely(rq->idle_balance))
		return;

	/*
	 * We may be recently in ticked or tickless idle mode. At the first
	 * busy tick after returning from idle, we will update the busy stats.
	 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   최근에 틱 또는 틱리스 유휴 모드에 있을 수 있습니다. 유휴 상태에서 
 *   돌아온 후 첫 번째 바쁜 틱에서 바쁜 통계를 업데이트합니다.
 */
	nohz_balance_exit_idle(rq);

	/*
	 * None are in tickless mode and hence no need for NOHZ idle load
	 * balancing.
	 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   아무도 틱리스 모드에 있지 않으므로 NOHZ 유휴 로드 밸런싱이 필요하지
 *   않습니다.
 */
	if (likely(!atomic_read(&nohz.nr_cpus)))
		return;

/*
 * IAMROOT, 2023.05.12:
 * - has_blocked가 존재하고, now가 next_blocked 이후라면 flags를
 *   NOHZ_STATS_KICK으로 변경한다.
 */
	if (READ_ONCE(nohz.has_blocked) &&
	    time_after(now, READ_ONCE(nohz.next_blocked)))
		flags = NOHZ_STATS_KICK;

/*
 * IAMROOT, 2023.05.12:
 * - now가 next_balance 이전 시간이라면 out.
 */
	if (time_before(now, nohz.next_balance))
		goto out;

	if (rq->nr_running >= 2) {
		flags = NOHZ_KICK_MASK;
		goto out;
	}

	rcu_read_lock();

	sd = rcu_dereference(rq->sd);
	if (sd) {
		/*
		 * If there's a CFS task and the current CPU has reduced
		 * capacity; kick the ILB to see if there's a better CPU to run
		 * on.
		 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   CFS 작업이 있고 현재 CPU의 용량이 줄어든 경우; 실행하기에 더 나은 
 *   CPU가 있는지 확인하려면 ILB를 실행하십시오.
 */
		if (rq->cfs.h_nr_running >= 1 && check_cpu_capacity(rq, sd)) {
			flags = NOHZ_KICK_MASK;
			goto unlock;
		}
	}

	sd = rcu_dereference(per_cpu(sd_asym_packing, cpu));
/*
 * IAMROOT, 2023.05.12:
 * - @cpu의 asym pack에서, idle cpu중에 @cpu보다 높은 우선순위가 있는지
 *   확인한다.
 */
	if (sd) {
		/*
		 * When ASYM_PACKING; see if there's a more preferred CPU
		 * currently idle; in which case, kick the ILB to move tasks
		 * around.
		 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   ASYM_PACKING일 때; 현재 유휴 상태인 더 선호하는 CPU가 있는지 
 *   확인합니다. 이 경우 ILB를 걷어차서 작업을 이동합니다.
 */
		for_each_cpu_and(i, sched_domain_span(sd), nohz.idle_cpus_mask) {
			if (sched_asym_prefer(i, cpu)) {
				flags = NOHZ_KICK_MASK;
				goto unlock;
			}
		}
	}

	sd = rcu_dereference(per_cpu(sd_asym_cpucapacity, cpu));
/*
 * IAMROOT, 2023.05.12:
 * - @sd에 소속된 cpu라면, misfit load가 있으며, 더 좋은 cpu로 
 *   옮길수있다면 NOHZ_KICK_MASK
 * - @rq에 misfit load가 있지만, 더 좋은 cpu로 옮길수있는 경우 
 *   NOHZ_KICK_MASK을 선택한다.
 *   그게 아니라면 그냥 종료한다.
 */
	if (sd) {
		/*
		 * When ASYM_CPUCAPACITY; see if there's a higher capacity CPU
		 * to run the misfit task on.
		 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   ASYM_CPUCAPACITY일 때; 적합하지 않은 작업을 실행할 수 있는 더 높은 
 *   용량의 CPU가 있는지 확인하십시오.
 */ 
		if (check_misfit_status(rq, sd)) {
			flags = NOHZ_KICK_MASK;
			goto unlock;
		}

		/*
		 * For asymmetric systems, we do not want to nicely balance
		 * cache use, instead we want to embrace asymmetry and only
		 * ensure tasks have enough CPU capacity.
		 *
		 * Skip the LLC logic because it's not relevant in that case.
		 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   비대칭 시스템의 경우 캐시 사용의 균형을 잘 맞추는 것이 아니라 
 *   비대칭을 수용하고 작업에 충분한 CPU 용량만 있는지 확인하려고 합니다.
 *
 *   이 경우 관련이 없으므로 LLC 논리를 건너뜁니다.
 */
		goto unlock;
	}

	sds = rcu_dereference(per_cpu(sd_llc_shared, cpu));
	if (sds) {
		/*
		 * If there is an imbalance between LLC domains (IOW we could
		 * increase the overall cache use), we need some less-loaded LLC
		 * domain to pull some load. Likewise, we may need to spread
		 * load within the current LLC domain (e.g. packed SMT cores but
		 * other CPUs are idle). We can't really know from here how busy
		 * the others are - so just get a nohz balance going if it looks
		 * like this LLC domain has tasks we could move.
		 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *  LLC 도메인 간에 불균형이 있는 경우(IOW는 전체 캐시 사용을 증가시킬 수 
 *  있음) 일부 로드를 풀기 위해 로드가 적은 일부 LLC 도메인이 필요합니다. 
 *  마찬가지로 현재 LLC 도메인 내에서 부하를 분산해야 할 수도 있습니다
 *  (예: 압축된 SMT 코어이지만 다른 CPU는 유휴 상태임). 여기에서는 다른 
 *  사람들이 얼마나 바쁜지 알 수 없습니다. 따라서 이 LLC 도메인에 우리가 
 *  이동할 수 있는 작업이 있는 것처럼 보이면 노헤즈 잔액을 유지하세요.
 */
		nr_busy = atomic_read(&sds->nr_busy_cpus);
		if (nr_busy > 1) {
			flags = NOHZ_KICK_MASK;
			goto unlock;
		}
	}
unlock:
	rcu_read_unlock();
out:
	if (flags)
		kick_ilb(flags);
}

/*
 * IAMROOT, 2023.05.12:
 * - @cpu의 sd last level cache pcpu에서 nohz_idle표시를 풀고
 *   shared에서 busy cpu를 up한다.
 */
static void set_cpu_sd_state_busy(int cpu)
{
	struct sched_domain *sd;

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_llc, cpu));

	if (!sd || !sd->nohz_idle)
		goto unlock;
	sd->nohz_idle = 0;

	atomic_inc(&sd->shared->nr_busy_cpus);
unlock:
	rcu_read_unlock();
}

/*
 * IAMROOT, 2023.05.12:
 * - tick stop을 종료한다. 중지되었다는 표시(nohz_tick_stopped)를 
 *   unset하고, idle(tick stop이였으므로 idle 이였을것)도 풀어준다.
 *   nohz가 해제되므로 nohz개수에도 뺀다.
 * - @rq->cpu의 sd llc에서도 nohz idle을 unset한다.
 */
void nohz_balance_exit_idle(struct rq *rq)
{
	SCHED_WARN_ON(rq != this_rq());

	if (likely(!rq->nohz_tick_stopped))
		return;

	rq->nohz_tick_stopped = 0;
	cpumask_clear_cpu(rq->cpu, nohz.idle_cpus_mask);
	atomic_dec(&nohz.nr_cpus);

	set_cpu_sd_state_busy(rq->cpu);
}

static void set_cpu_sd_state_idle(int cpu)
{
	struct sched_domain *sd;

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_llc, cpu));

	if (!sd || sd->nohz_idle)
		goto unlock;
	sd->nohz_idle = 1;

	atomic_dec(&sd->shared->nr_busy_cpus);
unlock:
	rcu_read_unlock();
}

/*
 * This routine will record that the CPU is going idle with tick stopped.
 * This info will be used in performing idle load balancing in the future.
 */
void nohz_balance_enter_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	SCHED_WARN_ON(cpu != smp_processor_id());

	/* If this CPU is going down, then nothing needs to be done: */
	if (!cpu_active(cpu))
		return;

	/* Spare idle load balancing on CPUs that don't want to be disturbed: */
	if (!housekeeping_cpu(cpu, HK_FLAG_SCHED))
		return;

	/*
	 * Can be set safely without rq->lock held
	 * If a clear happens, it will have evaluated last additions because
	 * rq->lock is held during the check and the clear
	 */
	rq->has_blocked_load = 1;

	/*
	 * The tick is still stopped but load could have been added in the
	 * meantime. We set the nohz.has_blocked flag to trig a check of the
	 * *_avg. The CPU is already part of nohz.idle_cpus_mask so the clear
	 * of nohz.has_blocked can only happen after checking the new load
	 */
	if (rq->nohz_tick_stopped)
		goto out;

	/* If we're a completely isolated CPU, we don't play: */
	if (on_null_domain(rq))
		return;

	rq->nohz_tick_stopped = 1;

	cpumask_set_cpu(cpu, nohz.idle_cpus_mask);
	atomic_inc(&nohz.nr_cpus);

	/*
	 * Ensures that if nohz_idle_balance() fails to observe our
	 * @idle_cpus_mask store, it must observe the @has_blocked
	 * store.
	 */
	smp_mb__after_atomic();

	set_cpu_sd_state_idle(cpu);

out:
	/*
	 * Each time a cpu enter idle, we assume that it has blocked load and
	 * enable the periodic update of the load of idle cpus
	 */
	WRITE_ONCE(nohz.has_blocked, 1);
}

static bool update_nohz_stats(struct rq *rq)
{
	unsigned int cpu = rq->cpu;

	if (!rq->has_blocked_load)
		return false;

	if (!cpumask_test_cpu(cpu, nohz.idle_cpus_mask))
		return false;

	if (!time_after(jiffies, READ_ONCE(rq->last_blocked_load_update_tick)))
		return true;

	update_blocked_averages(cpu);

	return rq->has_blocked_load;
}

/*
 * Internal function that runs load balance for all idle cpus. The load balance
 * can be a simple update of blocked load or a complete load balance with
 * tasks movement depending of flags.
 */
/*
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   모든 유휴 CPU에 대한 로드 밸런스를 실행하는 내부 기능. 로드 균형은 차단된 로드의
 *   간단한 업데이트이거나 플래그에 따라 작업 이동이 있는 전체 로드 균형일 수
 *   있습니다.
 * - TODO.
 */
static void _nohz_idle_balance(struct rq *this_rq, unsigned int flags,
			       enum cpu_idle_type idle)
{
	/* Earliest time when we have to do rebalance again */
	unsigned long now = jiffies;
	unsigned long next_balance = now + 60*HZ;
	bool has_blocked_load = false;
	int update_next_balance = 0;
	int this_cpu = this_rq->cpu;
	int balance_cpu;
	struct rq *rq;

	SCHED_WARN_ON((flags & NOHZ_KICK_MASK) == NOHZ_BALANCE_KICK);

	/*
	 * We assume there will be no idle load after this update and clear
	 * the has_blocked flag. If a cpu enters idle in the mean time, it will
	 * set the has_blocked flag and trig another update of idle load.
	 * Because a cpu that becomes idle, is added to idle_cpus_mask before
	 * setting the flag, we are sure to not clear the state and not
	 * check the load of an idle cpu.
	 */
/*
 * IAMROOT, 2023.03.15:
 * - papago
 *   이 업데이트 후 유휴 로드가 없다고 가정하고 has_blocked 플래그를
 *   지웁니다. CPU가 중간에 유휴 상태가 되면 has_blocked 플래그를 설정하고
 *   유휴 로드의 또 다른 업데이트를 트리거합니다.
 *   유휴 상태가 된 CPU는 플래그를 설정하기 전에 idle_cpus_mask에
 *   추가되므로 상태를 지우지 않고 유휴 CPU의 부하를 확인하지 않습니다.
 */
	WRITE_ONCE(nohz.has_blocked, 0);

	/*
	 * Ensures that if we miss the CPU, we must see the has_blocked
	 * store from nohz_balance_enter_idle().
	 */
/*
 * IAMROOT, 2023.03.15:
 * - papago
 *   CPU를 놓치면 nohz_balance_enter_idle()에서 has_blocked 저장소를
 *   확인해야 합니다.
 */
	smp_mb();

	/*
	 * Start with the next CPU after this_cpu so we will end with this_cpu and let a
	 * chance for other idle cpu to pull load.
	 */
/*
 * IAMROOT, 2023.03.15:
 * - papago
 *   this_cpu 이후의 다음 CPU로 시작하여 this_cpu로 끝나고 다른 유휴 CPU가
 *   로드를 풀 수 있도록 합니다.
 */
	for_each_cpu_wrap(balance_cpu,  nohz.idle_cpus_mask, this_cpu+1) {
		if (!idle_cpu(balance_cpu))
			continue;

		/*
		 * If this CPU gets work to do, stop the load balancing
		 * work being done for other CPUs. Next load
		 * balancing owner will pick it up.
		 */
/*
 * IAMROOT, 2023.03.15:
 * - papago
 *   이 CPU에 작업이 있으면 다른 CPU에 대해 수행 중인 로드 밸런싱 작업을
 *   중지합니다. 다음 로드 밸런싱 소유자가 선택합니다.
 */
		if (need_resched()) {
			has_blocked_load = true;
			goto abort;
		}

		rq = cpu_rq(balance_cpu);

		has_blocked_load |= update_nohz_stats(rq);

		/*
		 * If time for next balance is due,
		 * do the balance.
		 */
		if (time_after_eq(jiffies, rq->next_balance)) {
			struct rq_flags rf;

			rq_lock_irqsave(rq, &rf);
			update_rq_clock(rq);
			rq_unlock_irqrestore(rq, &rf);

			if (flags & NOHZ_BALANCE_KICK)
				rebalance_domains(rq, CPU_IDLE);
		}

		if (time_after(next_balance, rq->next_balance)) {
			next_balance = rq->next_balance;
			update_next_balance = 1;
		}
	}

	/*
	 * next_balance will be updated only when there is a need.
	 * When the CPU is attached to null domain for ex, it will not be
	 * updated.
	 */
/*
 * IAMROOT, 2023.03.15:
 * - papago
 *   next_balance는 필요할 때만 업데이트됩니다. 
 *   예를 들어 CPU가 null 도메인에 연결되면 업데이트되지 않습니다.
 */
	if (likely(update_next_balance))
		nohz.next_balance = next_balance;

	WRITE_ONCE(nohz.next_blocked,
		now + msecs_to_jiffies(LOAD_AVG_PERIOD));

abort:
	/* There is still blocked load, enable periodic update */
	if (has_blocked_load)
		WRITE_ONCE(nohz.has_blocked, 1);
}

/*
 * In CONFIG_NO_HZ_COMMON case, the idle balance kickee will do the
 * rebalancing for all the cpus for whom scheduler ticks are stopped.
 */
/*
 * IAMROOT. 2023.04.29:
 * - google-translate
 * CONFIG_NO_HZ_COMMON의 경우 유휴 균형 키키는 스케줄러 틱이 중지된 모든 CPU에 대해
 * 재조정을 수행합니다.
 */
static bool nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle)
{
	unsigned int flags = this_rq->nohz_idle_balance;

	if (!flags)
		return false;

	this_rq->nohz_idle_balance = 0;

	if (idle != CPU_IDLE)
		return false;

	_nohz_idle_balance(this_rq, flags, idle);

	return true;
}

/*
 * Check if we need to run the ILB for updating blocked load before entering
 * idle state.
 */
/*
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   유휴 상태에 들어가기 전에 차단된 로드를 업데이트하기 위해 ILB를 실행해야 하는지
 *   확인합니다.
 * - NOHZ_NEWILB_KICK 설정은 newidle_balance -> nohz_newidle_balance 함수에서 한다.
 */
void nohz_run_idle_balance(int cpu)
{
	unsigned int flags;

/*
 * IAMROOT, 2023.03.15:
 * - NOHZ_STATS_KICK이 있는경우 flags에 NOHZ_STATS_KICK이 담기면서
 *   nohz_flags에서 지워진다.
 */
	flags = atomic_fetch_andnot(NOHZ_NEWILB_KICK, nohz_flags(cpu));

	/*
	 * Update the blocked load only if no SCHED_SOFTIRQ is about to happen
	 * (ie NOHZ_STATS_KICK set) and will do the same.
	 */
	/*
	 * IAMROOT. 2023.03.11:
	 * - google-translate
	 *   SCHED_SOFTIRQ가 발생하지 않을 경우에만(예: NOHZ_STATS_KICK 설정)
	 *   차단된 로드를 업데이트하고 동일한 작업을 수행합니다.
	 */
	if ((flags == NOHZ_NEWILB_KICK) && !need_resched())
		_nohz_idle_balance(cpu_rq(cpu), NOHZ_STATS_KICK, CPU_IDLE);
}

static void nohz_newidle_balance(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu;

	/*
	 * This CPU doesn't want to be disturbed by scheduler
	 * housekeeping
	 */
	if (!housekeeping_cpu(this_cpu, HK_FLAG_SCHED))
		return;

	/* Will wake up very soon. No time for doing anything else*/
	if (this_rq->avg_idle < sysctl_sched_migration_cost)
		return;

	/* Don't need to update blocked load of idle CPUs*/
	if (!READ_ONCE(nohz.has_blocked) ||
	    time_before(jiffies, READ_ONCE(nohz.next_blocked)))
		return;

	/*
	 * Set the need to trigger ILB in order to update blocked load
	 * before entering idle state.
	 */
	atomic_or(NOHZ_NEWILB_KICK, nohz_flags(this_cpu));
}

#else /* !CONFIG_NO_HZ_COMMON */
static inline void nohz_balancer_kick(struct rq *rq) { }

static inline bool nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle)
{
	return false;
}

static inline void nohz_newidle_balance(struct rq *this_rq) { }
#endif /* CONFIG_NO_HZ_COMMON */

/*
 * newidle_balance is called by schedule() if this_cpu is about to become
 * idle. Attempts to pull tasks from other CPUs.
 *
 * Returns:
 *   < 0 - we released the lock and there are !fair tasks present
 *     0 - failed, no new tasks
 *   > 0 - success, new (fair) tasks present
 */
/*
 * IAMROOT, 2023.01.28:
 * - TODO
 *   다른 cpu에서 task를 가져온다.
 * 1. update misfit_task_load
 *   1.1 avg_idle이 작거나 overload가 없는경우 next_balance만 계산
 * 2.
 */
static int newidle_balance(struct rq *this_rq, struct rq_flags *rf)
{
	unsigned long next_balance = jiffies + HZ;
	int this_cpu = this_rq->cpu;
	struct sched_domain *sd;
	int pulled_task = 0;
	u64 curr_cost = 0;

	update_misfit_status(NULL, this_rq);

	/*
	 * There is a task waiting to run. No need to search for one.
	 * Return 0; the task will be enqueued when switching to idle.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * 실행 대기 중인 작업이 있습니다. 하나를 검색할 필요가 없습니다. 0을
	 * 반환합니다. 유휴 상태로 전환하면 작업이 대기열에 추가됩니다.
	 */
	if (this_rq->ttwu_pending)
		return 0;

	/*
	 * We must set idle_stamp _before_ calling idle_balance(), such that we
	 * measure the duration of idle_balance() as idle time.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * 우리는 idle_balance()의 지속 시간을 유휴 시간으로 측정하도록 idle_balance()를
	 * 호출하기 _전에_ idle_stamp를 설정해야 합니다.
	 * - idle balance 현재 시각 update
	 */
	this_rq->idle_stamp = rq_clock(this_rq);

	/*
	 * Do not pull tasks towards !active CPUs...
	 */
	if (!cpu_active(this_cpu))
		return 0;

	/*
	 * This is OK, because current is on_cpu, which avoids it being picked
	 * for load-balance and preemption/IRQs are still disabled avoiding
	 * further scheduler activity on it and we're being very careful to
	 * re-start the picking loop.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * current는 on_cpu이므로 부하 분산을 위해 선택되는 것을 방지하고 선점/IRQ는 여전히
	 * 비활성화되어 추가 스케줄러 활동을 방지하며 선택 루프를 다시 시작하는 데 매우
	 * 주의를 기울이고 있습니다.
	 */
	rq_unpin_lock(this_rq, rf);

	/*
	 * IAMROOT, 2023.04.29:
	 * - avg_idle 이 500ms 보다 작거나 rt나 dl에 중복 task가 없으면
	 *   balancing 을 포기한다.(다음 balancing 주기만 설정)
	 */
	if (this_rq->avg_idle < sysctl_sched_migration_cost ||
	    !READ_ONCE(this_rq->rd->overload)) {

		rcu_read_lock();
		sd = rcu_dereference_check_sched_domain(this_rq->sd);
		if (sd)
			update_next_balance(sd, &next_balance);
		rcu_read_unlock();

		goto out;
	}

	raw_spin_rq_unlock(this_rq);

	update_blocked_averages(this_cpu);
	rcu_read_lock();
	for_each_domain(this_cpu, sd) {
		int continue_balancing = 1;
		u64 t0, domain_cost;

		/*
		 * IAMROOT, 2023.04.29:
		 * - sd 단계별 cost 가 증가하면서 avg_idle 보다 커지면 중단하고
		 *   빠져나온다.
		 */
		if (this_rq->avg_idle < curr_cost + sd->max_newidle_lb_cost) {
			update_next_balance(sd, &next_balance);
			break;
		}

		/*
		 * IAMROOT, 2023.04.29:
		 * - 각 도메인들은 default로 SD_BALANCE_NEWIDLE 설정임.
		 *   단 relax_domain_level 설정이 있는 경우는 그보다 상위 도메인의
		 *   SD_BALANCE_NEWIDLE 값은 제거된다.
		 */
		if (sd->flags & SD_BALANCE_NEWIDLE) {
			t0 = sched_clock_cpu(this_cpu);

			pulled_task = load_balance(this_cpu, this_rq,
						   sd, CPU_NEWLY_IDLE,
						   &continue_balancing);

			domain_cost = sched_clock_cpu(this_cpu) - t0;
			if (domain_cost > sd->max_newidle_lb_cost)
				sd->max_newidle_lb_cost = domain_cost;

			curr_cost += domain_cost;
		}

		update_next_balance(sd, &next_balance);

		/*
		 * Stop searching for tasks to pull if there are
		 * now runnable tasks on this rq.
		 */
		/*
		 * IAMROOT. 2023.04.29:
		 * - google-translate
		 * 현재 이 rq에 실행 가능한 작업이 있는 경우 가져올 작업 검색을 중지합니다.
		 */
		if (pulled_task || this_rq->nr_running > 0 ||
		    this_rq->ttwu_pending)
			break;
	}
	rcu_read_unlock();

	raw_spin_rq_lock(this_rq);

	if (curr_cost > this_rq->max_idle_balance_cost)
		this_rq->max_idle_balance_cost = curr_cost;

	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * 도메인을 탐색하는 동안 우리는 rq 잠금을 해제했으며 그 동안 작업이 대기열에
	 * 추가되었을 수 있습니다. 유휴 상태가 아니므로 작업을 수행했다고 가정합니다.
	 */
	if (this_rq->cfs.h_nr_running && !pulled_task)
		pulled_task = 1;

	/* Is there a task of a high priority class? */
	/*
	 * IAMROOT, 2023.04.29:
	 * - dl이나 rt task 가 추가된 경우.
	 *   -1(RETRY_TASK)을 return 하여 다른 class(rt,dl) pick_nexk_task 실행
	 */
	if (this_rq->nr_running != this_rq->cfs.h_nr_running)
		pulled_task = -1;

out:
	/* Move the next balance forward */
	if (time_after(this_rq->next_balance, next_balance))
		this_rq->next_balance = next_balance;

	if (pulled_task)
		this_rq->idle_stamp = 0;
	else
		nohz_newidle_balance(this_rq);

	rq_repin_lock(this_rq, rf);

	return pulled_task;
}

/*
 * run_rebalance_domains is triggered when needed from the scheduler tick.
 * Also triggered for nohz idle balancing (with nohz_balancing_kick set).
 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *   run_rebalance_domains는 스케줄러 틱에서 필요할 때 트리거됩니다.
 *   nohz 유휴 밸런싱(nohz_balancing_kick 설정)에 대해서도 트리거됩니다.
 * - ING
 */
static __latent_entropy void run_rebalance_domains(struct softirq_action *h)
{
	struct rq *this_rq = this_rq();
	enum cpu_idle_type idle = this_rq->idle_balance ?
						CPU_IDLE : CPU_NOT_IDLE;

	/*
	 * If this CPU has a pending nohz_balance_kick, then do the
	 * balancing on behalf of the other idle CPUs whose ticks are
	 * stopped. Do nohz_idle_balance *before* rebalance_domains to
	 * give the idle CPUs a chance to load balance. Else we may
	 * load balance only within the local sched_domain hierarchy
	 * and abort nohz_idle_balance altogether if we pull some load.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * 이 CPU에 보류 중인 nohz_balance_kick이 있으면 틱이 중지된 다른 유휴 CPU를
	 * 대신하여 밸런싱을 수행합니다. nohz_idle_balance *before* rebalance_domains를
	 * 수행하여 유휴 CPU에 로드 밸런싱 기회를 제공합니다. 그렇지 않으면 로컬
	 * sched_domain 계층 내에서만 부하를 분산하고 일부 부하를 풀면 nohz_idle_balance를
	 * 완전히 중단할 수 있습니다.
	 */
	if (nohz_idle_balance(this_rq, idle))
		return;

	/* normal load balance */
	update_blocked_averages(this_rq->cpu);
	rebalance_domains(this_rq, idle);
}

/*
 * Trigger the SCHED_SOFTIRQ if it is time to do periodic load balancing.
 */
/*
 * IAMROOT. 2023.04.30:
 * - google-translate
 * 주기적 로드 밸런싱을 수행할 시간인 경우 SCHED_SOFTIRQ를 트리거합니다.
 *
 * - next balance 시간이 지나면 SCHED_SOFTIRQ에 의해
 *   run_rebalance_domains 함수를 호출한다
 */
void trigger_load_balance(struct rq *rq)
{
	/*
	 * Don't need to rebalance while attached to NULL domain or
	 * runqueue CPU is not active
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * NULL 도메인에 연결되어 있거나 실행 대기열 CPU가 활성화되어 있지 않은 동안
	 * 재조정할 필요가 없습니다.
	 */
	if (unlikely(on_null_domain(rq) || !cpu_active(cpu_of(rq))))
		return;

	/*
	 * IAMROOT, 2023.04.29:
	 * - init_sched_fair_class 에서 설정한
	 *   run_rebalance_domains 함수를 호출하게 된다.
	 */
	if (time_after_eq(jiffies, rq->next_balance))
		raise_softirq(SCHED_SOFTIRQ);

	nohz_balancer_kick(rq);
}

static void rq_online_fair(struct rq *rq)
{
	update_sysctl();

	update_runtime_enabled(rq);
}

static void rq_offline_fair(struct rq *rq)
{
	update_sysctl();

	/* Ensure any throttled groups are reachable by pick_next_task */
	unthrottle_offline_cfs_rqs(rq);
}

#endif /* CONFIG_SMP */

#ifdef CONFIG_SCHED_CORE
static inline bool
__entity_slice_used(struct sched_entity *se, int min_nr_tasks)
{
	u64 slice = sched_slice(cfs_rq_of(se), se);
	u64 rtime = se->sum_exec_runtime - se->prev_sum_exec_runtime;

	return (rtime * min_nr_tasks > slice);
}

#define MIN_NR_TASKS_DURING_FORCEIDLE	2
static inline void task_tick_core(struct rq *rq, struct task_struct *curr)
{
	if (!sched_core_enabled(rq))
		return;

	/*
	 * If runqueue has only one task which used up its slice and
	 * if the sibling is forced idle, then trigger schedule to
	 * give forced idle task a chance.
	 *
	 * sched_slice() considers only this active rq and it gets the
	 * whole slice. But during force idle, we have siblings acting
	 * like a single runqueue and hence we need to consider runnable
	 * tasks on this CPU and the forced idle CPU. Ideally, we should
	 * go through the forced idle rq, but that would be a perf hit.
	 * We can assume that the forced idle CPU has at least
	 * MIN_NR_TASKS_DURING_FORCEIDLE - 1 tasks and use that to check
	 * if we need to give up the CPU.
	 */
	if (rq->core->core_forceidle && rq->cfs.nr_running == 1 &&
	    __entity_slice_used(&curr->se, MIN_NR_TASKS_DURING_FORCEIDLE))
		resched_curr(rq);
}

/*
 * se_fi_update - Update the cfs_rq->min_vruntime_fi in a CFS hierarchy if needed.
 */
static void se_fi_update(struct sched_entity *se, unsigned int fi_seq, bool forceidle)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		if (forceidle) {
			if (cfs_rq->forceidle_seq == fi_seq)
				break;
			cfs_rq->forceidle_seq = fi_seq;
		}

		cfs_rq->min_vruntime_fi = cfs_rq->min_vruntime;
	}
}

void task_vruntime_update(struct rq *rq, struct task_struct *p, bool in_fi)
{
	struct sched_entity *se = &p->se;

	if (p->sched_class != &fair_sched_class)
		return;

	se_fi_update(se, rq->core->core_forceidle_seq, in_fi);
}

bool cfs_prio_less(struct task_struct *a, struct task_struct *b, bool in_fi)
{
	struct rq *rq = task_rq(a);
	struct sched_entity *sea = &a->se;
	struct sched_entity *seb = &b->se;
	struct cfs_rq *cfs_rqa;
	struct cfs_rq *cfs_rqb;
	s64 delta;

	SCHED_WARN_ON(task_rq(b)->core != rq->core);

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * Find an se in the hierarchy for tasks a and b, such that the se's
	 * are immediate siblings.
	 */
	while (sea->cfs_rq->tg != seb->cfs_rq->tg) {
		int sea_depth = sea->depth;
		int seb_depth = seb->depth;

		if (sea_depth >= seb_depth)
			sea = parent_entity(sea);
		if (sea_depth <= seb_depth)
			seb = parent_entity(seb);
	}

	se_fi_update(sea, rq->core->core_forceidle_seq, in_fi);
	se_fi_update(seb, rq->core->core_forceidle_seq, in_fi);

	cfs_rqa = sea->cfs_rq;
	cfs_rqb = seb->cfs_rq;
#else
	cfs_rqa = &task_rq(a)->cfs;
	cfs_rqb = &task_rq(b)->cfs;
#endif

	/*
	 * Find delta after normalizing se's vruntime with its cfs_rq's
	 * min_vruntime_fi, which would have been updated in prior calls
	 * to se_fi_update().
	 */
	delta = (s64)(sea->vruntime - seb->vruntime) +
		(s64)(cfs_rqb->min_vruntime_fi - cfs_rqa->min_vruntime_fi);

	return delta > 0;
}
#else
static inline void task_tick_core(struct rq *rq, struct task_struct *curr) {}
#endif

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
/*
 * IAMROOT. 2022.12.17:
 * - google-translate
 *   스케줄링 클래스의 작업을 치는 스케줄러 틱.
 *
 *   참고: 이 함수는 완전한 dynticks를 따라가는 틱 오프load에 의해 원격으로 호출될 수
 *   있습니다. 따라서 로컬 가정을 할 수 없으며 매개 변수에 전달된 @rq 및 @curr를 통해
 *   모든 항목에 액세스해야 합니다.
 *
 * - scheduler_tick 에서 호출할 때 매개변수 queued = 0
 *
 * @rq: running task의 runqueue
 * @queued : @curr가 rq에 있는지의 여부. rq에 있으면 true, 없으면(즉 현재 cpu에서 실행중)
 *           false.
 */
static void task_tick_fair(struct rq *rq, struct task_struct *curr, int queued)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &curr->se;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		entity_tick(cfs_rq, se, queued);
	}

/*
 * IAMROOT, 2023.06.17:
 * - numa balance수행한다. 내부에서 numa balance시간이 도래했으면 수행하고 아니면
 *   아무것도 안할것이다.
 */
	if (static_branch_unlikely(&sched_numa_balancing))
		task_tick_numa(rq, curr);

	update_misfit_status(curr, rq);
	update_overutilized_status(task_rq(curr));

	task_tick_core(rq, curr);
}

/*
 * called on fork with the child task as argument from the parent's context
 *  - child not yet on the tasklist
 *  - preemption disabled
 */
static void task_fork_fair(struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se, *curr;
	struct rq *rq = this_rq();
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);

	cfs_rq = task_cfs_rq(current);
	curr = cfs_rq->curr;
	if (curr) {
		update_curr(cfs_rq);
		se->vruntime = curr->vruntime;
	}
	place_entity(cfs_rq, se, 1);

	if (sysctl_sched_child_runs_first && curr && entity_before(curr, se)) {
		/*
		 * Upon rescheduling, sched_class::put_prev_task() will place
		 * 'current' within the tree based on its new key value.
		 */
		swap(curr->vruntime, se->vruntime);
		resched_curr(rq);
	}

	se->vruntime -= cfs_rq->min_vruntime;
	rq_unlock(rq, &rf);
}

/*
 * Priority of the task has changed. Check to see if we preempt
 * the current task.
 */
static void
prio_changed_fair(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!task_on_rq_queued(p))
		return;

	if (rq->cfs.nr_running == 1)
		return;

	/*
	 * Reschedule if we are currently running on this runqueue and
	 * our priority decreased, or if we are not currently running on
	 * this runqueue and our priority is higher than the current's
	 */
	if (task_current(rq, p)) {
		if (p->prio > oldprio)
			resched_curr(rq);
	} else
		check_preempt_curr(rq, p, 0);
}

static inline bool vruntime_normalized(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	/*
	 * In both the TASK_ON_RQ_QUEUED and TASK_ON_RQ_MIGRATING cases,
	 * the dequeue_entity(.flags=0) will already have normalized the
	 * vruntime.
	 */
	if (p->on_rq)
		return true;

	/*
	 * When !on_rq, vruntime of the task has usually NOT been normalized.
	 * But there are some cases where it has already been normalized:
	 *
	 * - A forked child which is waiting for being woken up by
	 *   wake_up_new_task().
	 * - A task which has been woken up by try_to_wake_up() and
	 *   waiting for actually being woken up by sched_ttwu_pending().
	 */
	if (!se->sum_exec_runtime ||
	    (READ_ONCE(p->__state) == TASK_WAKING && p->sched_remote_wakeup))
		return true;

	return false;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * Propagate the changes of the sched_entity across the tg tree to make it
 * visible to the root
 */
static void propagate_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq;

	list_add_leaf_cfs_rq(cfs_rq_of(se));

	/* Start to propagate at parent */
	se = se->parent;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		if (!cfs_rq_throttled(cfs_rq)){
			update_load_avg(cfs_rq, se, UPDATE_TG);
			list_add_leaf_cfs_rq(cfs_rq);
			continue;
		}

		if (list_add_leaf_cfs_rq(cfs_rq))
			break;
	}
}
#else
static void propagate_entity_cfs_rq(struct sched_entity *se) { }
#endif

static void detach_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	/* Catch up with the cfs_rq and remove our load when we leave */
	update_load_avg(cfs_rq, se, 0);
	detach_entity_load_avg(cfs_rq, se);
	update_tg_load_avg(cfs_rq);
	propagate_entity_cfs_rq(se);
}

static void attach_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * Since the real-depth could have been changed (only FAIR
	 * class maintain depth value), reset depth properly.
	 */
	se->depth = se->parent ? se->parent->depth + 1 : 0;
#endif

	/* Synchronize entity with its cfs_rq */
	update_load_avg(cfs_rq, se, sched_feat(ATTACH_AGE_LOAD) ? 0 : SKIP_AGE_LOAD);
	attach_entity_load_avg(cfs_rq, se);
	update_tg_load_avg(cfs_rq);
	propagate_entity_cfs_rq(se);
}

static void detach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	if (!vruntime_normalized(p)) {
		/*
		 * Fix up our vruntime so that the current sleep doesn't
		 * cause 'unlimited' sleep bonus.
		 */
		place_entity(cfs_rq, se, 0);
		se->vruntime -= cfs_rq->min_vruntime;
	}

	detach_entity_cfs_rq(se);
}

static void attach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	attach_entity_cfs_rq(se);

	if (!vruntime_normalized(p))
		se->vruntime += cfs_rq->min_vruntime;
}

static void switched_from_fair(struct rq *rq, struct task_struct *p)
{
	detach_task_cfs_rq(p);
}

static void switched_to_fair(struct rq *rq, struct task_struct *p)
{
	attach_task_cfs_rq(p);

	if (task_on_rq_queued(p)) {
		/*
		 * We were most likely switched from sched_rt, so
		 * kick off the schedule if running, otherwise just see
		 * if we can still preempt the current task.
		 */
		if (task_current(rq, p))
			resched_curr(rq);
		else
			check_preempt_curr(rq, p, 0);
	}
}

/* Account for a task changing its policy or group.
 *
 * This routine is mostly called to set cfs_rq->curr field when a task
 * migrates between groups/classes.
 */
static void set_next_task_fair(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_entity *se = &p->se;

#ifdef CONFIG_SMP
	if (task_on_rq_queued(p)) {
		/*
		 * Move the next running task to the front of the list, so our
		 * cfs_tasks list becomes MRU one.
		 */
		list_move(&se->group_node, &rq->cfs_tasks);
	}
#endif

	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		set_next_entity(cfs_rq, se);
		/* ensure bandwidth has been allocated on our new cfs_rq */
		account_cfs_rq_runtime(cfs_rq, 0);
	}
}


/*
 * IAMROOT, 2022.11.26:
 * - min_vruntime(scheduler/sched-design-CFS.rst)
 *  that value is used to place newly activated entities on the left
 *  side of the tree as much as possible.
 */
void init_cfs_rq(struct cfs_rq *cfs_rq)
{
	cfs_rq->tasks_timeline = RB_ROOT_CACHED;
	cfs_rq->min_vruntime = (u64)(-(1LL << 20));
#ifndef CONFIG_64BIT
	cfs_rq->min_vruntime_copy = cfs_rq->min_vruntime;
#endif
#ifdef CONFIG_SMP
	raw_spin_lock_init(&cfs_rq->removed.lock);
#endif
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void task_set_group_fair(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	set_task_rq(p, task_cpu(p));
	se->depth = se->parent ? se->parent->depth + 1 : 0;
}

static void task_move_group_fair(struct task_struct *p)
{
	detach_task_cfs_rq(p);
	set_task_rq(p, task_cpu(p));

#ifdef CONFIG_SMP
	/* Tell se's cfs_rq has been changed -- migrated */
	p->se.avg.last_update_time = 0;
#endif
	attach_task_cfs_rq(p);
}

static void task_change_group_fair(struct task_struct *p, int type)
{
	switch (type) {
	case TASK_SET_GROUP:
		task_set_group_fair(p);
		break;

	case TASK_MOVE_GROUP:
		task_move_group_fair(p);
		break;
	}
}

void free_fair_sched_group(struct task_group *tg)
{
	int i;

	destroy_cfs_bandwidth(tg_cfs_bandwidth(tg));

	for_each_possible_cpu(i) {
		if (tg->cfs_rq)
			kfree(tg->cfs_rq[i]);
		if (tg->se)
			kfree(tg->se[i]);
	}

	kfree(tg->cfs_rq);
	kfree(tg->se);
}

int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct sched_entity *se;
	struct cfs_rq *cfs_rq;
	int i;

	tg->cfs_rq = kcalloc(nr_cpu_ids, sizeof(cfs_rq), GFP_KERNEL);
	if (!tg->cfs_rq)
		goto err;
	tg->se = kcalloc(nr_cpu_ids, sizeof(se), GFP_KERNEL);
	if (!tg->se)
		goto err;

	tg->shares = NICE_0_LOAD;

	init_cfs_bandwidth(tg_cfs_bandwidth(tg));

	for_each_possible_cpu(i) {
		cfs_rq = kzalloc_node(sizeof(struct cfs_rq),
				      GFP_KERNEL, cpu_to_node(i));
		if (!cfs_rq)
			goto err;

		se = kzalloc_node(sizeof(struct sched_entity),
				  GFP_KERNEL, cpu_to_node(i));
		if (!se)
			goto err_free_rq;

		init_cfs_rq(cfs_rq);
		init_tg_cfs_entry(tg, cfs_rq, se, i, parent->se[i]);
		init_entity_runnable_average(se);
	}

	return 1;

err_free_rq:
	kfree(cfs_rq);
err:
	return 0;
}

void online_fair_sched_group(struct task_group *tg)
{
	struct sched_entity *se;
	struct rq_flags rf;
	struct rq *rq;
	int i;

	for_each_possible_cpu(i) {
		rq = cpu_rq(i);
		se = tg->se[i];
		rq_lock_irq(rq, &rf);
		update_rq_clock(rq);
		attach_entity_cfs_rq(se);
		sync_throttle(tg, i);
		rq_unlock_irq(rq, &rf);
	}
}

void unregister_fair_sched_group(struct task_group *tg)
{
	unsigned long flags;
	struct rq *rq;
	int cpu;

	for_each_possible_cpu(cpu) {
		if (tg->se[cpu])
			remove_entity_load_avg(tg->se[cpu]);

		/*
		 * Only empty task groups can be destroyed; so we can speculatively
		 * check on_list without danger of it being re-added.
		 */
		if (!tg->cfs_rq[cpu]->on_list)
			continue;

		rq = cpu_rq(cpu);

		raw_spin_rq_lock_irqsave(rq, flags);
		list_del_leaf_cfs_rq(tg->cfs_rq[cpu]);
		raw_spin_rq_unlock_irqrestore(rq, flags);
	}
}

/*
 * IAMROOT, 2022.11.26:
 * - @cpu에 해당하는 rq를 @cfs_rq에 초기화한다.
 */
void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	cfs_rq->tg = tg;
	cfs_rq->rq = rq;
	init_cfs_rq_runtime(cfs_rq);

	tg->cfs_rq[cpu] = cfs_rq;
	tg->se[cpu] = se;

	/* se could be NULL for root_task_group */
	if (!se)
		return;

	if (!parent) {
		se->cfs_rq = &rq->cfs;
		se->depth = 0;
	} else {
		se->cfs_rq = parent->my_q;
		se->depth = parent->depth + 1;
	}

	se->my_q = cfs_rq;
	/* guarantee group entities always have weight */
	update_load_set(&se->load, NICE_0_LOAD);
	se->parent = parent;
}

static DEFINE_MUTEX(shares_mutex);

static int __sched_group_set_shares(struct task_group *tg, unsigned long shares)
{
	int i;

	lockdep_assert_held(&shares_mutex);

	/*
	 * We can't change the weight of the root cgroup.
	 */
	if (!tg->se[0])
		return -EINVAL;

	shares = clamp(shares, scale_load(MIN_SHARES), scale_load(MAX_SHARES));

	if (tg->shares == shares)
		return 0;

	tg->shares = shares;
	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct sched_entity *se = tg->se[i];
		struct rq_flags rf;

		/* Propagate contribution to hierarchy */
		rq_lock_irqsave(rq, &rf);
		update_rq_clock(rq);
		for_each_sched_entity(se) {
			update_load_avg(cfs_rq_of(se), se, UPDATE_TG);
			update_cfs_group(se);
		}
		rq_unlock_irqrestore(rq, &rf);
	}

	return 0;
}

int sched_group_set_shares(struct task_group *tg, unsigned long shares)
{
	int ret;

	mutex_lock(&shares_mutex);
	if (tg_is_idle(tg))
		ret = -EINVAL;
	else
		ret = __sched_group_set_shares(tg, shares);
	mutex_unlock(&shares_mutex);

	return ret;
}

int sched_group_set_idle(struct task_group *tg, long idle)
{
	int i;

	if (tg == &root_task_group)
		return -EINVAL;

	if (idle < 0 || idle > 1)
		return -EINVAL;

	mutex_lock(&shares_mutex);

	if (tg->idle == idle) {
		mutex_unlock(&shares_mutex);
		return 0;
	}

	tg->idle = idle;

	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct sched_entity *se = tg->se[i];
		struct cfs_rq *grp_cfs_rq = tg->cfs_rq[i];
		bool was_idle = cfs_rq_is_idle(grp_cfs_rq);
		long idle_task_delta;
		struct rq_flags rf;

		rq_lock_irqsave(rq, &rf);

		grp_cfs_rq->idle = idle;
		if (WARN_ON_ONCE(was_idle == cfs_rq_is_idle(grp_cfs_rq)))
			goto next_cpu;

		idle_task_delta = grp_cfs_rq->h_nr_running -
				  grp_cfs_rq->idle_h_nr_running;
		if (!cfs_rq_is_idle(grp_cfs_rq))
			idle_task_delta *= -1;

		for_each_sched_entity(se) {
			struct cfs_rq *cfs_rq = cfs_rq_of(se);

			if (!se->on_rq)
				break;

			cfs_rq->idle_h_nr_running += idle_task_delta;

			/* Already accounted at parent level and above. */
			if (cfs_rq_is_idle(cfs_rq))
				break;
		}

next_cpu:
		rq_unlock_irqrestore(rq, &rf);
	}

	/* Idle groups have minimum weight. */
	if (tg_is_idle(tg))
		__sched_group_set_shares(tg, scale_load(WEIGHT_IDLEPRIO));
	else
		__sched_group_set_shares(tg, NICE_0_LOAD);

	mutex_unlock(&shares_mutex);
	return 0;
}

#else /* CONFIG_FAIR_GROUP_SCHED */

void free_fair_sched_group(struct task_group *tg) { }

int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}

void online_fair_sched_group(struct task_group *tg) { }

void unregister_fair_sched_group(struct task_group *tg) { }

#endif /* CONFIG_FAIR_GROUP_SCHED */


static unsigned int get_rr_interval_fair(struct rq *rq, struct task_struct *task)
{
	struct sched_entity *se = &task->se;
	unsigned int rr_interval = 0;

	/*
	 * Time slice is 0 for SCHED_OTHER tasks that are on an otherwise
	 * idle runqueue:
	 */
	if (rq->cfs.load.weight)
		rr_interval = NS_TO_JIFFIES(sched_slice(cfs_rq_of(se), se));

	return rr_interval;
}

/*
 * All the scheduling class methods:
 */
DEFINE_SCHED_CLASS(fair) = {

	.enqueue_task		= enqueue_task_fair,
	.dequeue_task		= dequeue_task_fair,
	.yield_task		= yield_task_fair,
	.yield_to_task		= yield_to_task_fair,

	.check_preempt_curr	= check_preempt_wakeup,

	.pick_next_task		= __pick_next_task_fair,
	.put_prev_task		= put_prev_task_fair,
	.set_next_task          = set_next_task_fair,

#ifdef CONFIG_SMP
	.balance		= balance_fair,
	.pick_task		= pick_task_fair,
	.select_task_rq		= select_task_rq_fair,
	.migrate_task_rq	= migrate_task_rq_fair,

	.rq_online		= rq_online_fair,
	.rq_offline		= rq_offline_fair,

	.task_dead		= task_dead_fair,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.task_tick		= task_tick_fair,
	.task_fork		= task_fork_fair,

	.prio_changed		= prio_changed_fair,
	.switched_from		= switched_from_fair,
	.switched_to		= switched_to_fair,

	.get_rr_interval	= get_rr_interval_fair,

	.update_curr		= update_curr_fair,

#ifdef CONFIG_FAIR_GROUP_SCHED
	.task_change_group	= task_change_group_fair,
#endif

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};

#ifdef CONFIG_SCHED_DEBUG
void print_cfs_stats(struct seq_file *m, int cpu)
{
	struct cfs_rq *cfs_rq, *pos;

	rcu_read_lock();
	for_each_leaf_cfs_rq_safe(cpu_rq(cpu), cfs_rq, pos)
		print_cfs_rq(m, cpu, cfs_rq);
	rcu_read_unlock();
}

#ifdef CONFIG_NUMA_BALANCING
void show_numa_stats(struct task_struct *p, struct seq_file *m)
{
	int node;
	unsigned long tsf = 0, tpf = 0, gsf = 0, gpf = 0;
	struct numa_group *ng;

	rcu_read_lock();
	ng = rcu_dereference(p->numa_group);
	for_each_online_node(node) {
		if (p->numa_faults) {
			tsf = p->numa_faults[task_faults_idx(NUMA_MEM, node, 0)];
			tpf = p->numa_faults[task_faults_idx(NUMA_MEM, node, 1)];
		}
		if (ng) {
			gsf = ng->faults[task_faults_idx(NUMA_MEM, node, 0)],
			gpf = ng->faults[task_faults_idx(NUMA_MEM, node, 1)];
		}
		print_numa_stats(m, node, tsf, tpf, gsf, gpf);
	}
	rcu_read_unlock();
}
#endif /* CONFIG_NUMA_BALANCING */
#endif /* CONFIG_SCHED_DEBUG */

/*
 * IAMROOT, 2022.12.29:
 * - sched에 관련한 softirq를 하나 열어둔다.
 * - scheduler_tick - > trigger_load_balance 에서 SCHED_SOFTIRQ에 설정한
 *   run_rebalance_domains 함수를 호출하게 된다
 */
__init void init_sched_fair_class(void)
{
#ifdef CONFIG_SMP
	open_softirq(SCHED_SOFTIRQ, run_rebalance_domains);

#ifdef CONFIG_NO_HZ_COMMON
	nohz.next_balance = jiffies;
	nohz.next_blocked = jiffies;
	zalloc_cpumask_var(&nohz.idle_cpus_mask, GFP_NOWAIT);
#endif
#endif /* SMP */

}

/*
 * Helper functions to facilitate extracting info from tracepoints.
 */

const struct sched_avg *sched_trace_cfs_rq_avg(struct cfs_rq *cfs_rq)
{
#ifdef CONFIG_SMP
	return cfs_rq ? &cfs_rq->avg : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_cfs_rq_avg);

char *sched_trace_cfs_rq_path(struct cfs_rq *cfs_rq, char *str, int len)
{
	if (!cfs_rq) {
		if (str)
			strlcpy(str, "(null)", len);
		else
			return NULL;
	}

	cfs_rq_tg_path(cfs_rq, str, len);
	return str;
}
EXPORT_SYMBOL_GPL(sched_trace_cfs_rq_path);

int sched_trace_cfs_rq_cpu(struct cfs_rq *cfs_rq)
{
	return cfs_rq ? cpu_of(rq_of(cfs_rq)) : -1;
}
EXPORT_SYMBOL_GPL(sched_trace_cfs_rq_cpu);

const struct sched_avg *sched_trace_rq_avg_rt(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq ? &rq->avg_rt : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rq_avg_rt);

const struct sched_avg *sched_trace_rq_avg_dl(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq ? &rq->avg_dl : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rq_avg_dl);

const struct sched_avg *sched_trace_rq_avg_irq(struct rq *rq)
{
#if defined(CONFIG_SMP) && defined(CONFIG_HAVE_SCHED_AVG_IRQ)
	return rq ? &rq->avg_irq : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rq_avg_irq);

int sched_trace_rq_cpu(struct rq *rq)
{
	return rq ? cpu_of(rq) : -1;
}
EXPORT_SYMBOL_GPL(sched_trace_rq_cpu);

int sched_trace_rq_cpu_capacity(struct rq *rq)
{
	return rq ?
#ifdef CONFIG_SMP
		rq->cpu_capacity
#else
		SCHED_CAPACITY_SCALE
#endif
		: -1;
}
EXPORT_SYMBOL_GPL(sched_trace_rq_cpu_capacity);

const struct cpumask *sched_trace_rd_span(struct root_domain *rd)
{
#ifdef CONFIG_SMP
	return rd ? rd->span : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rd_span);

int sched_trace_rq_nr_running(struct rq *rq)
{
        return rq ? rq->nr_running : -1;
}
EXPORT_SYMBOL_GPL(sched_trace_rq_nr_running);
