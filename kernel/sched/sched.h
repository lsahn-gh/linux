/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scheduler internal types and methods:
 */
#include <linux/sched.h>

#include <linux/sched/autogroup.h>
#include <linux/sched/clock.h>
#include <linux/sched/coredump.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/cputime.h>
#include <linux/sched/deadline.h>
#include <linux/sched/debug.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/idle.h>
#include <linux/sched/init.h>
#include <linux/sched/isolation.h>
#include <linux/sched/jobctl.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/mm.h>
#include <linux/sched/nohz.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/prio.h>
#include <linux/sched/rt.h>
#include <linux/sched/signal.h>
#include <linux/sched/smt.h>
#include <linux/sched/stat.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/topology.h>
#include <linux/sched/user.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/xacct.h>

#include <uapi/linux/sched/types.h>

#include <linux/binfmts.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/compat.h>
#include <linux/context_tracking.h>
#include <linux/cpufreq.h>
#include <linux/cpuidle.h>
#include <linux/cpuset.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/delayacct.h>
#include <linux/energy_model.h>
#include <linux/init_task.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/membarrier.h>
#include <linux/migrate.h>
#include <linux/mmu_context.h>
#include <linux/nmi.h>
#include <linux/proc_fs.h>
#include <linux/prefetch.h>
#include <linux/profile.h>
#include <linux/psi.h>
#include <linux/ratelimit.h>
#include <linux/rcupdate_wait.h>
#include <linux/security.h>
#include <linux/stop_machine.h>
#include <linux/suspend.h>
#include <linux/swait.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/tsacct_kern.h>

#include <asm/tlb.h>

#ifdef CONFIG_PARAVIRT
# include <asm/paravirt.h>
#endif

#include "cpupri.h"
#include "cpudeadline.h"

#include <trace/events/sched.h>

#ifdef CONFIG_SCHED_DEBUG
# define SCHED_WARN_ON(x)	WARN_ONCE(x, #x)
#else
# define SCHED_WARN_ON(x)	({ (void)(x), 0; })
#endif

struct rq;
struct cpuidle_state;

/* task_struct::on_rq states: */
/*
 * IAMROOT, 2022.12.29:
 * - TASK_ON_RQ_QUEUED
 *   queue에 들어간 상태
 * - TASK_ON_RQ_MIGRATING
 *   다른 cpu로 move중인 상태(migrate)
 */
#define TASK_ON_RQ_QUEUED	1
#define TASK_ON_RQ_MIGRATING	2

extern __read_mostly int scheduler_running;

extern unsigned long calc_load_update;
extern atomic_long_t calc_load_tasks;

extern void calc_global_load_tick(struct rq *this_rq);
extern long calc_load_fold_active(struct rq *this_rq, long adjust);

extern void call_trace_sched_update_nr_running(struct rq *rq, int count);
/*
 * Helpers for converting nanosecond timing to jiffy resolution
 */
#define NS_TO_JIFFIES(TIME)	((unsigned long)(TIME) / (NSEC_PER_SEC / HZ))

/*
 * Increase resolution of nice-level calculations for 64-bit architectures.
 * The extra resolution improves shares distribution and load balancing of
 * low-weight task groups (eg. nice +19 on an autogroup), deeper taskgroup
 * hierarchies, especially on larger systems. This is not a user-visible change
 * and does not change the user-interface for setting shares/weights.
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. 64-bit). The costs for increasing resolution when 32-bit
 * are pretty high and the returns do not justify the increased costs.
 *
 * Really only required when CONFIG_FAIR_GROUP_SCHED=y is also set, but to
 * increase coverage and consistency always enable it on 64-bit platforms.
 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *   64비트 아키텍처에 대한 좋은 수준의 계산 해상도를 높입니다.
 *   추가 해상도는 특히 더 큰 시스템에서 낮은 가중치 작업 그룹(예:
 *   자동 그룹의 nice +19), 더 깊은 작업 그룹 계층의 공유 배포 및
 *   로드 밸런싱을 개선합니다. 이는 사용자가 볼 수 있는 변경 사항이
 *   아니며 공유/가중치 설정을 위한 사용자 인터페이스를 변경하지 않습니다.
 *
 *   이 증가된 해상도(예: 64비트)를 허용하기에 충분한 비트가 있는
 *   경우에만 해상도를 높입니다. 32비트에서 해상도를 높이는 데 드는
 *   비용은 상당히 높고 수익은 증가된 비용을 정당화하지 못합니다.
 *
 *   CONFIG_FAIR_GROUP_SCHED=y도 설정된 경우에만 실제로 필요하지만 적용
 *   범위와 일관성을 높이기 위해 항상 64비트 플랫폼에서 활성화합니다.
 */
#ifdef CONFIG_64BIT
/*
 * IAMROOT, 2022.11.26:
 * - 20
 */
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT + SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		((w) << SCHED_FIXEDPOINT_SHIFT)

/*
 * IAMROOT, 2022.12.31:
 * - 20bit로 scale up됫던걸 10bit로 변경한다.
 */
# define scale_load_down(w) \
({ \
	unsigned long __w = (w); \
	if (__w) \
		__w = max(2UL, __w >> SCHED_FIXEDPOINT_SHIFT); \
	__w; \
})
#else
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		(w)
# define scale_load_down(w)	(w)
#endif

/*
 * Task weight (visible to users) and its load (invisible to users) have
 * independent resolution, but they should be well calibrated. We use
 * scale_load() and scale_load_down(w) to convert between them. The
 * following must be true:
 *
 *  scale_load(sched_prio_to_weight[NICE_TO_PRIO(0)-MAX_RT_PRIO]) == NICE_0_LOAD
 *
 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *   Task weight(사용자에게 표시됨)와 그 load(사용자에게 표시되지 않음)는
 *   독립적인 해상도를 갖지만 잘 조정되어야 합니다. scale_load() 및
 *   scale_load_down(w)를 사용하여 변환합니다. 다음 사항이 참이어야 합니다.
 *
 *   scale_load(sched_prio_to_weight[NICE_TO_PRIO(0)-MAX_RT_PRIO]) == NICE_0_LOAD
 * - 32bit인경우 1024, 64bit인경우 1024 * 1024
 */
#define NICE_0_LOAD		(1L << NICE_0_LOAD_SHIFT)

/*
 * Single value that decides SCHED_DEADLINE internal math precision.
 * 10 -> just above 1us
 * 9  -> just above 0.5us
 */
#define DL_SCALE		10

/*
 * Single value that denotes runtime == period, ie unlimited time.
 */
#define RUNTIME_INF		((u64)~0ULL)

static inline int idle_policy(int policy)
{
	return policy == SCHED_IDLE;
}
static inline int fair_policy(int policy)
{
	return policy == SCHED_NORMAL || policy == SCHED_BATCH;
}

static inline int rt_policy(int policy)
{
	return policy == SCHED_FIFO || policy == SCHED_RR;
}

static inline int dl_policy(int policy)
{
	return policy == SCHED_DEADLINE;
}
static inline bool valid_policy(int policy)
{
	return idle_policy(policy) || fair_policy(policy) ||
		rt_policy(policy) || dl_policy(policy);
}

/*
 * IAMROOT, 2022.11.26:
 * policy == SCHED_IDLE ?
 */
static inline int task_has_idle_policy(struct task_struct *p)
{
	return idle_policy(p->policy);
}

static inline int task_has_rt_policy(struct task_struct *p)
{
	return rt_policy(p->policy);
}

static inline int task_has_dl_policy(struct task_struct *p)
{
	return dl_policy(p->policy);
}

#define cap_scale(v, s) ((v)*(s) >> SCHED_CAPACITY_SHIFT)

/*
 * IAMROOT, 2023.05.24:
 * - 기존값(*avg)에 new 값(@sampe)에 대한 diff를 1/8 배 하여 기존값에 적산한다.
 */
static inline void update_avg(u64 *avg, u64 sample)
{
	s64 diff = sample - *avg;
	*avg += diff / 8;
}

/*
 * Shifting a value by an exponent greater *or equal* to the size of said value
 * is UB; cap at size-1.
 */
#define shr_bound(val, shift)							\
	(val >> min_t(typeof(shift), shift, BITS_PER_TYPE(typeof(val)) - 1))

/*
 * !! For sched_setattr_nocheck() (kernel) only !!
 *
 * This is actually gross. :(
 *
 * It is used to make schedutil kworker(s) higher priority than SCHED_DEADLINE
 * tasks, but still be able to sleep. We need this on platforms that cannot
 * atomically change clock frequency. Remove once fast switching will be
 * available on such platforms.
 *
 * SUGOV stands for SchedUtil GOVernor.
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   !! sched_setattr_nocheck()(커널) 전용!!
 *
 *   이것은 실제로 역겹다. :(
 *
 *   SCHED_DEADLINE 작업보다 schedutil kworker(s)를 더 높은 우선 순위로 만드는 데
 *   사용되지만 여전히 휴면할 수 있습니다. 클록 주파수를 원자적으로 변경할 수 없는 플랫폼에서
 *   필요합니다. 이러한 플랫폼에서 빠른 전환이 가능해지면 제거하십시오.
 *
 *   SUGOV 는 SchedUtil GOVernor 약자
 *
 *   - 마감시간에 관계없이 dl 중에서 무조건 먼저 우선 순위를 주고 싶은 경우
 */
#define SCHED_FLAG_SUGOV	0x10000000

#define SCHED_DL_FLAGS (SCHED_FLAG_RECLAIM | SCHED_FLAG_DL_OVERRUN | SCHED_FLAG_SUGOV)

/*
 * IAMROOT, 2023.03.04:
 * - specail entity인지 확인.
 */
static inline bool dl_entity_is_special(struct sched_dl_entity *dl_se)
{
#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL
	return unlikely(dl_se->flags & SCHED_FLAG_SUGOV);
#else
	return false;
#endif
}

/*
 * Tells if entity @a should preempt entity @b.
 */
/*
 * IAMROOT, 2023.03.04:
 * - special 이거나 a < b 라면(a의 만료시간이 더 짧다면) return true.
 */
static inline bool
dl_entity_preempt(struct sched_dl_entity *a, struct sched_dl_entity *b)
{
	return dl_entity_is_special(a) ||
	       dl_time_before(a->deadline, b->deadline);
}

/*
 * This is the priority-queue data structure of the RT scheduling class:
 */
/*
 * IAMROOT, 2022.12.29:
 * - bitmap
 *   init_rt_rq()에서 delimiter 용법으로 초기화된다.
 */
struct rt_prio_array {
	DECLARE_BITMAP(bitmap, MAX_RT_PRIO+1); /* include 1 bit for delimiter */
	struct list_head queue[MAX_RT_PRIO];
};

struct rt_bandwidth {
	/* nests inside the rq lock: */
	raw_spinlock_t		rt_runtime_lock;
	ktime_t			rt_period;
	u64			rt_runtime;
	struct hrtimer		rt_period_timer;
	unsigned int		rt_period_active;
};

void __dl_clear_params(struct task_struct *p);

struct dl_bandwidth {
	raw_spinlock_t		dl_runtime_lock;
	u64			dl_runtime;
	u64			dl_period;
};

static inline int dl_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

/*
 * To keep the bandwidth of -deadline tasks under control
 * we need some place where:
 *  - store the maximum -deadline bandwidth of each cpu;
 *  - cache the fraction of bandwidth that is currently allocated in
 *    each root domain;
 *
 * This is all done in the data structure below. It is similar to the
 * one used for RT-throttling (rt_bandwidth), with the main difference
 * that, since here we are only interested in admission control, we
 * do not decrease any runtime while the group "executes", neither we
 * need a timer to replenish it.
 *
 * With respect to SMP, bandwidth is given on a per root domain basis,
 * meaning that:
 *  - bw (< 100%) is the deadline bandwidth of each CPU;
 *  - total_bw is the currently allocated bandwidth in each root domain;
 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *  -deadline 작업의 대역폭을 제어하려면 다음과 같은 장소가 필요합니다. 
 *  - 각 CPU의 최대 기한 대역폭을 저장합니다.
 *  - 각 루트 도메인에 현재 할당된 대역폭의 일부를 캐시합니다.
 *
 *   이것은 모두 아래 데이터 구조에서 수행됩니다. 이것은 RT 조절(rt_bandwidth)
 *   에 사용되는 것과 유사하지만 여기서는 승인 제어에만 관심이 있기 때문에 
 *   그룹이 실행되는 동안 런타임을 줄이지 않으며 이를 보충하기 위한 타이머도 
 *   필요하지 않습니다. 
 *
 *   SMP와 관련하여 대역폭은 루트 도메인별로 제공되며 이는 다음을 의미합니다.
 *   - bw(< 100%)는 각 CPU의 데드라인 대역폭입니다. 
 *   - total_bw는 각 루트 도메인에서 현재 할당된 대역폭입니다. 
 *
 * - BW_SHIFT로 shift해서 사용한다. (1 ==> 1 << BW_SHIFT)
 */
struct dl_bw {
	raw_spinlock_t		lock;
	u64			bw;
	u64			total_bw;
};

static inline void __dl_update(struct dl_bw *dl_b, s64 bw);

static inline
void __dl_sub(struct dl_bw *dl_b, u64 tsk_bw, int cpus)
{
	dl_b->total_bw -= tsk_bw;
	__dl_update(dl_b, (s32)tsk_bw / cpus);
}

static inline
void __dl_add(struct dl_bw *dl_b, u64 tsk_bw, int cpus)
{
	dl_b->total_bw += tsk_bw;
	__dl_update(dl_b, -((s32)tsk_bw / cpus));
}

static inline bool __dl_overflow(struct dl_bw *dl_b, unsigned long cap,
				 u64 old_bw, u64 new_bw)
{
	return dl_b->bw != -1 &&
	       cap_scale(dl_b->bw, cap) < dl_b->total_bw - old_bw + new_bw;
}

/*
 * Verify the fitness of task @p to run on @cpu taking into account the
 * CPU original capacity and the runtime/deadline ratio of the task.
 *
 * The function will return true if the CPU original capacity of the
 * @cpu scaled by SCHED_CAPACITY_SCALE >= runtime/deadline ratio of the
 * task and false otherwise.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   CPU 원래 용량과 작업의 런타임/데드라인 비율을 고려하여 @cpu에서 실행하기 
 *   위한 작업 @p의 적합성을 확인합니다.
 *
 *   함수는 SCHED_CAPACITY_SCALE로 조정된 @cpu의 원래 CPU 용량 >= 작업의 
 *   런타임/데드라인 비율이면 true를 반환하고 그렇지 않으면 false를 
 *   반환합니다.
 *
 * - big cpu인 경우 cap은 높고, little은 작을 것이다.
 *   deadline에 cpu cap을 적용해서, dl_runtime을 해소할수있는 @cpu라면
 *   return true.
 */
static inline bool dl_task_fits_capacity(struct task_struct *p, int cpu)
{
	unsigned long cap = arch_scale_cpu_capacity(cpu);

	return cap_scale(p->dl.dl_deadline, cap) >= p->dl.dl_runtime;
}

extern void init_dl_bw(struct dl_bw *dl_b);
extern int  sched_dl_global_validate(void);
extern void sched_dl_do_global(void);
extern int  sched_dl_overflow(struct task_struct *p, int policy, const struct sched_attr *attr);
extern void __setparam_dl(struct task_struct *p, const struct sched_attr *attr);
extern void __getparam_dl(struct task_struct *p, struct sched_attr *attr);
extern bool __checkparam_dl(const struct sched_attr *attr);
extern bool dl_param_changed(struct task_struct *p, const struct sched_attr *attr);
extern int  dl_task_can_attach(struct task_struct *p, const struct cpumask *cs_cpus_allowed);
extern int  dl_cpuset_cpumask_can_shrink(const struct cpumask *cur, const struct cpumask *trial);
extern bool dl_cpu_busy(unsigned int cpu);

#ifdef CONFIG_CGROUP_SCHED

#include <linux/cgroup.h>
#include <linux/psi.h>

struct cfs_rq;
struct rt_rq;

extern struct list_head task_groups;

/*
 * IAMROOT, 2022.12.22:
 * --- chat open ai ---
 * - lock: A spin lock that is used to protect the data structure from 
 *   concurrent access.
 *
 * - period: The time interval over which the CPU usage rate is measured.
 *   cpu 사용률이 측정되는 시간 간격.
 *
 * - quota: The maximum amount of CPU time (in nanoseconds) that is allowed 
 *   to be used by the tasks on the runqueue over a single period.
 *   -- RUNTIME_INF - runqueue가 스로틀링되지 않음을 나타내기 위해 CFS 대역폭
 *     컨트롤러에서 특수 값으로 사용됩니다. 이는 관리자가 실행 대기열의 
 *     작업이 제한 없이 필요한 만큼의 CPU 시간을 사용하도록 허용하려는 
 *     시나리오에서 유용할 수 있습니다. 
 *     __assign_cfs_rq_runtime() 에서 해당 시점에 period_timer를 시작을
 *     안한다.(5ms time 뒤로 미룬다.)
 *    do_sched_cfs_period_timer() 에서 period_timer는 deactivate 한다.
 *    (timer 동작을 했어도 기능동작을 안한다.)
 *
 * - runtime: The amount of CPU time (in nanoseconds) that is currently 
 *   available for use by the tasks on the runqueue.
 *
 * - burst: The maximum amount of CPU time (in nanoseconds) that can be used 
 *   by a single task on the runqueue in a single burst.
 *
 * - hierarchical_quota: The maximum amount of CPU time (in nanoseconds) 
 *   that is allowed to be used by the tasks in the active hierarchy 
 *   (i.e., the set of tasks that are eligible to run on the CPU) 
 *   over a single period.
 *
 * - idle: A flag that indicates whether the runqueue is currently idle.
 *   유휴 필드는 CFS 대역폭 컨트롤러의 상태를 추적하는 데 사용됩니다. 
 *   컨트롤러가 유휴 상태이면 사용 가능한 런타임이 없으며 더 많은 런타임이 
 *   할당될 때까지 작업을 예약할 수 없음을 의미합니다. 컨트롤러가 유휴 
 *   상태가 아닌 경우 사용 가능한 런타임이 있고 필요에 따라 작업을 예약할 
 *   수 있음을 의미합니다.
 *
 * - period_active: A flag that indicates whether the current period is 
 *   active.
 *   cpu usage를 측정하고 있는 상황인지 확인한다. 측정하고 있으면 set.
 *   아니면 unset. (start_cfs_bandwidth() 참고)
 *
 * - slack_started: A flag that indicates whether the runqueue's slack 
 *   timer has been started.
 *
 * - period_timer: A timer that is used to track the progress of the 
 *   current period.
 *
 * - slack_timer: A timer that is used to track the runqueue's slack time.
 *
 * - throttled_cfs_rq: A list of runqueues that are currently being 
 *   throttled.
 *
 * - nr_periods: The number of periods that have elapsed since the CFS 
 *   bandwidth controller was enabled.
 *
 * - nr_throttled: The number of times the runqueue has been throttled 
 *   since the CFS bandwidth controller was enabled.
 *
 * - throttled_time: The total amount of time (in nanoseconds) that the 
 *   runqueue has been throttled since the CFS bandwidth controller was 
 *   enabled.
 */
struct cfs_bandwidth {
#ifdef CONFIG_CFS_BANDWIDTH
	raw_spinlock_t		lock;
	ktime_t			period;
/*
 * IAMROOT, 2022.12.23:
 * -  runtime, quota, burst의 관계는 
 *    __refill_cfs_bandwidth_runtime() 참고
 */
	u64			quota;
	u64			runtime;
	u64			burst;
	s64			hierarchical_quota;

	u8			idle;
	u8			period_active;
	u8			slack_started;
	struct hrtimer		period_timer;
	struct hrtimer		slack_timer;
	struct list_head	throttled_cfs_rq;

	/* Statistics: */
	int			nr_periods;
	int			nr_throttled;
	u64			throttled_time;
#endif
};

/* Task group related information */
struct task_group {
	struct cgroup_subsys_state css;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* schedulable entities of this group on each CPU */
	struct sched_entity	**se;
	/* runqueue "owned" by this group on each CPU */
	struct cfs_rq		**cfs_rq;
	/*
	 * IAMROOT, 2022.11.26:
	 * group의 shares 값. 초기값은 nice 0 weight 값과 동일
	 */
	unsigned long		shares;

	/* A positive value indicates that this is a SCHED_IDLE group. */
	int			idle;

#ifdef	CONFIG_SMP
	/*
	 * load_avg can be heavily contended at clock tick time, so put
	 * it in its own cacheline separated from the fields above which
	 * will also be accessed at each tick.
	 */
	atomic_long_t		load_avg ____cacheline_aligned;
#endif
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity	**rt_se;
	struct rt_rq		**rt_rq;

	struct rt_bandwidth	rt_bandwidth;
#endif

	struct rcu_head		rcu;
	struct list_head	list;

	struct task_group	*parent;
	struct list_head	siblings;
	struct list_head	children;

#ifdef CONFIG_SCHED_AUTOGROUP
	struct autogroup	*autogroup;
#endif

	struct cfs_bandwidth	cfs_bandwidth;

#ifdef CONFIG_UCLAMP_TASK_GROUP
	/* The two decimal precision [%] value requested from user-space */
	unsigned int		uclamp_pct[UCLAMP_CNT];
	/* Clamp values requested for a task group */
	struct uclamp_se	uclamp_req[UCLAMP_CNT];
	/* Effective clamp values used for a task group */
	struct uclamp_se	uclamp[UCLAMP_CNT];
#endif

};

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * IAMROOT, 2022.11.26:
 * - 32bit인경우 1024, 64bit인경우 1024 * 1024
 */
#define ROOT_TASK_GROUP_LOAD	NICE_0_LOAD

/*
 * A weight of 0 or 1 can cause arithmetics problems.
 * A weight of a cfs_rq is the sum of weights of which entities
 * are queued on this cfs_rq, so a weight of a entity should not be
 * too large, so as the shares value of a task group.
 * (The default weight is 1024 - so there's no practical
 *  limitation from this.)
 */
#define MIN_SHARES		(1UL <<  1)
#define MAX_SHARES		(1UL << 18)
#endif

typedef int (*tg_visitor)(struct task_group *, void *);

extern int walk_tg_tree_from(struct task_group *from,
			     tg_visitor down, tg_visitor up, void *data);

/*
 * Iterate the full tree, calling @down when first entering a node and @up when
 * leaving it for the final time.
 *
 * Caller must hold rcu_lock or sufficient equivalent.
 */
static inline int walk_tg_tree(tg_visitor down, tg_visitor up, void *data)
{
	return walk_tg_tree_from(&root_task_group, down, up, data);
}

extern int tg_nop(struct task_group *tg, void *data);

extern void free_fair_sched_group(struct task_group *tg);
extern int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent);
extern void online_fair_sched_group(struct task_group *tg);
extern void unregister_fair_sched_group(struct task_group *tg);
extern void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent);
extern void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b);

extern void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b);
extern void start_cfs_bandwidth(struct cfs_bandwidth *cfs_b);
extern void unthrottle_cfs_rq(struct cfs_rq *cfs_rq);

extern void free_rt_sched_group(struct task_group *tg);
extern int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent);
extern void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent);
extern int sched_group_set_rt_runtime(struct task_group *tg, long rt_runtime_us);
extern int sched_group_set_rt_period(struct task_group *tg, u64 rt_period_us);
extern long sched_group_rt_runtime(struct task_group *tg);
extern long sched_group_rt_period(struct task_group *tg);
extern int sched_rt_can_attach(struct task_group *tg, struct task_struct *tsk);

extern struct task_group *sched_create_group(struct task_group *parent);
extern void sched_online_group(struct task_group *tg,
			       struct task_group *parent);
extern void sched_destroy_group(struct task_group *tg);
extern void sched_offline_group(struct task_group *tg);

extern void sched_move_task(struct task_struct *tsk);

#ifdef CONFIG_FAIR_GROUP_SCHED
extern int sched_group_set_shares(struct task_group *tg, unsigned long shares);

extern int sched_group_set_idle(struct task_group *tg, long idle);

#ifdef CONFIG_SMP
extern void set_task_rq_fair(struct sched_entity *se,
			     struct cfs_rq *prev, struct cfs_rq *next);
#else /* !CONFIG_SMP */
static inline void set_task_rq_fair(struct sched_entity *se,
			     struct cfs_rq *prev, struct cfs_rq *next) { }
#endif /* CONFIG_SMP */
#endif /* CONFIG_FAIR_GROUP_SCHED */

#else /* CONFIG_CGROUP_SCHED */

struct cfs_bandwidth { };

#endif	/* CONFIG_CGROUP_SCHED */

/* CFS-related fields in a runqueue */
struct cfs_rq {
	struct load_weight	load;
/*
 * IAMROOT, 2022.12.27:
 * - chat open ai
 *  nr_running is an unsigned integer that stores the number of tasks that 
 *  are currently running on the runqueue.
 *  h_nr_running is an unsigned integer that stores the number of tasks that 
 *  are currently running on the runqueue, but only includes tasks with a 
 *  normal, batch, or idle scheduling policy.
 *  idle_h_nr_running is an unsigned integer that stores the number of tasks 
 *  that are currently running on the runqueue, but only includes tasks with 
 *  an idle scheduling policy. 
 *
 *  nr_running은 현재 runqueue에서 실행 중인 작업 수를 저장하는 부호 없는 
 *  정수입니다. 
 *  h_nr_running은 실행 대기열에서 현재 실행 중인 작업 수를 저장하는 부호 
 *  없는 정수이지만 일반, 배치 또는 유휴 스케줄링 정책이 있는 작업만 
 *  포함합니다. 
 *  idle_h_nr_running은 현재 실행 대기열에서 실행 중인 작업 수를 저장하는 
 *  부호 없는 정수이지만 유휴 스케줄링 정책이 있는 작업만 포함합니다. 
 *
 *  - nr_running
 *    cfs rq에서 현재 실행가능한 entity 수
 *  - h_nr_running
 *    하위 그룹까지 포함한 실행가능한 cfs_rq의 task 수.
 *    (throttled된 하위 cfs rq는 제외).
 *  - idle_h_nr_running
 *    하위 그룹까지 포함한 실행가능한 SCHED_IDLE policy 에 task 수
 */
	unsigned int		nr_running;
	unsigned int		h_nr_running;      /* SCHED_{NORMAL,BATCH,IDLE} */
	unsigned int		idle_h_nr_running; /* SCHED_IDLE */

	u64			exec_clock;
	u64			min_vruntime;
#ifdef CONFIG_SCHED_CORE
	unsigned int		forceidle_seq;
	u64			min_vruntime_fi;
#endif

#ifndef CONFIG_64BIT
	u64			min_vruntime_copy;
#endif

/*
 * IAMROOT, 2022.12.21:
 * - sched_entity의 vruntime을 기준으로 leftmost cache된다.
 *   (__entity_less 참고)
 */
	struct rb_root_cached	tasks_timeline;

	/*
	 * 'curr' points to currently running entity on this cfs_rq.
	 * It is set to NULL otherwise (i.e when none are currently running).
	 */
/*
 * IAMROOT, 2023.01.28:
 * - set 함수 정리.
 *   next : set_next_buddy(check_preempt_wakeup(), yield_to_task_fair(), dequeue_task_fair())
 *   last : set_last_buddy(check_preempt_wakeup())
 *   skip : set_skip_buddy(yield_to_task_fair())
 */
	struct sched_entity	*curr;
	struct sched_entity	*next;
	struct sched_entity	*last;
	struct sched_entity	*skip;

#ifdef	CONFIG_SCHED_DEBUG
	unsigned int		nr_spread_over;
#endif

#ifdef CONFIG_SMP
	/*
	 * CFS load tracking
	 */
	struct sched_avg	avg;
#ifndef CONFIG_64BIT
	u64			load_last_update_time_copy;
#endif

/*
 * IAMROOT, 2022.12.31:
 * - cfs rq에서 se를 deattach(dequeue)할때 즉시 감소시키지 않고 
 *   removed에 넣어놨다가 나중에 계산한다.update_cfs_rq_load_avg())
 */
	struct {
		raw_spinlock_t	lock ____cacheline_aligned;
		int		nr;
		unsigned long	load_avg;
		unsigned long	util_avg;
		unsigned long	runnable_avg;
	} removed;

#ifdef CONFIG_FAIR_GROUP_SCHED

/*
 * IAMROOT, 2023.01.07:
 * - 직전에 tg->load_avg에 적용된 cfs_rq의 load_avg.
 *   (update_tg_load_avg() 참고)
 */
	unsigned long		tg_load_avg_contrib;

/*
 * IAMROOT, 2022.12.31:
 * - propagate 
 *   propagate를 해야되는지에 대한 flag.
 * - prop_runnable_sum 
 *   propagate을 위해 미리 누적해놓는값.
 */
	long			propagate;
	long			prop_runnable_sum;

	/*
	 *   h_load = weight * f(tg)
	 *
	 * Where f(tg) is the recursive weight fraction assigned to
	 * this group.
	 */
/*
 * IAMROOT, 2023.05.03:
 * update_cfs_rq_h_load() 참고
 * - h_load_next : update_cfs_rq_h_load에서 top-down 방식으로 내려가기 위해
 *   임시로 사용하는 용도.
 * - h_load : hierarchy load
 * - last_h_load_update : 마지막 h_load update 시각
 */
	unsigned long		h_load;
	u64			last_h_load_update;
	struct sched_entity	*h_load_next;
#endif /* CONFIG_FAIR_GROUP_SCHED */
#endif /* CONFIG_SMP */

#ifdef CONFIG_FAIR_GROUP_SCHED
	struct rq		*rq;	/* CPU runqueue to which this cfs_rq is attached */

	/*
	 * leaf cfs_rqs are those that hold tasks (lowest schedulable entity in
	 * a hierarchy). Non-leaf lrqs hold other higher schedulable entities
	 * (like users, containers etc.)
	 *
	 * leaf_cfs_rq_list ties together list of leaf cfs_rq's in a CPU.
	 * This list is used during load balance.
	 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *  leaf cfs_rqs는 task(계층 구조에서 가장 낮은 스케줄 가능한 엔터티)을
 *  보유하는 것입니다. non-leaf lrqs는 사용자, 컨테이너 등과 같은 다른
 *  더 높은 스케줄 가능한 엔터티를 보유합니다. leaf_cfs_rq_list는 CPU에서
 *  리프 cfs_rq의 목록을 함께 묶습니다. 
 *  이 목록은 load balance 중에 사용됩니다.
 * - leaf cfs.
 *   가장 마지막에 있는 cfs group.
 */
	int			on_list;
	struct list_head	leaf_cfs_rq_list;
	struct task_group	*tg;	/* group that "owns" this runqueue */

	/* Locally cached copy of our task_group's idle value */
	int			idle;

#ifdef CONFIG_CFS_BANDWIDTH
	int			runtime_enabled;
/*
 * IAMROOT, 2022.12.22:
 * - runtime_remaining
 *   task가 runqueue에 있는 cpu시간을 제한하는데 사용.
 *   1. 실행시간 만큼 감소한다 (__account_cfs_rq_runtime())
 *   2. assign전 bandwidth에 대한 제한이 없으면 5ms로 다시 갱신된다.
 *      (__assign_cfs_rq_runtime())
 *   3. assign전 cfs_bandwidth runtime 이 감소될경우 그만큼 
 *   runtime_remain에 추가된다.
 *
 * - throttled_clock_task
 *   throttle이 시작됬을때의 시간 clock_task
 *
 * - throttled_clock_task_time
 *   unthrottle가 됬을때 update. throttle됬던 시간.
 *
 * - throttled
 *   쓰로틀중이면 set.
 */
	s64			runtime_remaining;

	u64			throttled_clock;
	u64			throttled_clock_task;
	u64			throttled_clock_task_time;
	int			throttled;
	int			throttle_count;
	struct list_head	throttled_list;
#endif /* CONFIG_CFS_BANDWIDTH */
#endif /* CONFIG_FAIR_GROUP_SCHED */
};

static inline int rt_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

/* RT IPI pull logic requires IRQ_WORK */
#if defined(CONFIG_IRQ_WORK) && defined(CONFIG_SMP)
# define HAVE_RT_PUSH_IPI
#endif

/* Real-Time classes' related field in a runqueue: */
struct rt_rq {
	struct rt_prio_array	active;
/*
 * IAMROOT, 2023.02.18:
 * - rt_nr_running
 *   현재 그룹 이하의 rt task수.
 * - rr_nr_running
 *   현재 그룹 이하의 rt task수 중에서도 rr policy를 사용하는 task 수
 * - rt_nr_total
 *   overload 검사를 위해 task가 enqueue/dequeue될때 증감을 하는 
 *   rt task수
 * - rt_nr_migratory
 *   migrate 가능한 rt task수. cpu가 2개이상인 경우 증감한다.
 */
	unsigned int		rt_nr_running;
	unsigned int		rr_nr_running;
#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
	struct {
		int		curr; /* highest queued rt task prio */
#ifdef CONFIG_SMP
		int		next; /* next highest */
#endif
	} highest_prio;
#endif
#ifdef CONFIG_SMP
	unsigned int		rt_nr_migratory;
	unsigned int		rt_nr_total;

/*
 * IAMROOT, 2023.02.11:
 * - 두개 이상의 rt_task가 있는 경우 1.
 */
	int			overloaded;
	struct plist_head	pushable_tasks;

#endif /* CONFIG_SMP */
	/*
	 * IAMROOT, 2023.02.04:
	 * - rt_queued : rt_rq 가동상태(rt task 가 1개 이상 enqueue 상태일때)
	 */
	int			rt_queued;

	/*
	 * IAMROOT, 2023.02.04:
	 * - rt_throttled : 각 rt periosds 마다 throttled 된 적이 있는지
	 * - rt_time : 각 rt periosds 마다 rt 실행 시간 누적 시간
	 * - rt_runtime : runtime 으로 설정된 시간. 매 periods 마다 갱신.
	 */
	int			rt_throttled;
	u64			rt_time;
	u64			rt_runtime;
	/* Nests inside the rq lock: */
	raw_spinlock_t		rt_runtime_lock;

#ifdef CONFIG_RT_GROUP_SCHED
	unsigned int		rt_nr_boosted;

	struct rq		*rq;
	struct task_group	*tg;
#endif
};

static inline bool rt_rq_is_runnable(struct rt_rq *rt_rq)
{
	return rt_rq->rt_queued && rt_rq->rt_nr_running;
}

/* Deadline class' related fields in a runqueue */
struct dl_rq {
	/* runqueue is an rbtree, ordered by deadline */
	struct rb_root_cached	root;

	unsigned int		dl_nr_running;

#ifdef CONFIG_SMP
	/*
	 * Deadline values of the currently executing and the
	 * earliest ready task on this rq. Caching these facilitates
	 * the decision whether or not a ready but not running task
	 * should migrate somewhere else.
	 */
	struct {
		u64		curr;
		u64		next;
	} earliest_dl;

	unsigned int		dl_nr_migratory;
	int			overloaded;

	/*
	 * Tasks on this rq that can be pushed away. They are kept in
	 * an rb-tree, ordered by tasks' deadlines, with caching
	 * of the leftmost (earliest deadline) element.
	 */
	struct rb_root_cached	pushable_dl_tasks_root;
#else
	struct dl_bw		dl_bw;
#endif
	/*
	 * "Active utilization" for this runqueue: increased when a
	 * task wakes up (becomes TASK_RUNNING) and decreased when a
	 * task blocks
	 */
	u64			running_bw;

	/*
	 * Utilization of the tasks "assigned" to this runqueue (including
	 * the tasks that are in runqueue and the tasks that executed on this
	 * CPU and blocked). Increased when a task moves to this runqueue, and
	 * decreased when the task moves away (migrates, changes scheduling
	 * policy, or terminates).
	 * This is needed to compute the "inactive utilization" for the
	 * runqueue (inactive utilization = this_bw - running_bw).
	 */
	u64			this_bw;
	u64			extra_bw;

	/*
	 * Inverse of the fraction of CPU utilization that can be reclaimed
	 * by the GRUB algorithm.
	 */
	u64			bw_ratio;
};

#ifdef CONFIG_FAIR_GROUP_SCHED
/* An entity is a task if it doesn't "own" a runqueue */
/*
 * IAMROOT, 2022.12.17:
 * - schedule entity의 my_q가 null 이면 task에 포함된 entity 이다.
 */
#define entity_is_task(se)	(!se->my_q)

static inline void se_update_runnable(struct sched_entity *se)
{
	if (!entity_is_task(se))
		se->runnable_weight = se->my_q->h_nr_running;
}

/*
 * IAMROOT, 2022.12.31:
 * @return @se가 runable 상태인지.
 *
 * - @se가 task이면 rq에 들어가있을것이므로 on_req를 검사한다.
 *         task group인 경우엔 runnable_weight를 검사한다.
 */
static inline long se_runnable(struct sched_entity *se)
{
	if (entity_is_task(se))
		return !!se->on_rq;
	else
		return se->runnable_weight;
}

#else
#define entity_is_task(se)	1

static inline void se_update_runnable(struct sched_entity *se) {}

static inline long se_runnable(struct sched_entity *se)
{
	return !!se->on_rq;
}
#endif

#ifdef CONFIG_SMP
/*
 * XXX we want to get rid of these helpers and use the full load resolution.
 */
static inline long se_weight(struct sched_entity *se)
{
	return scale_load_down(se->load.weight);
}

/*
 * IAMROOT, 2023.04.22:
 * - ex) a = 10, b = 11, return true
 * - powerPC에선 번호가 빠른게 빠르다.
 */
static inline bool sched_asym_prefer(int a, int b)
{
	return arch_asym_cpu_priority(a) > arch_asym_cpu_priority(b);
}

struct perf_domain {
	struct em_perf_domain *em_pd;
	struct perf_domain *next;
	struct rcu_head rcu;
};

/* Scheduling group status flags */
#define SG_OVERLOAD		0x1 /* More than one runnable task on a CPU. */
#define SG_OVERUTILIZED		0x2 /* One or more CPUs are over-utilized. */

/*
 * We add the notion of a root-domain which will be used to define per-domain
 * variables. Each exclusive cpuset essentially defines an island domain by
 * fully partitioning the member CPUs from any other cpuset. Whenever a new
 * exclusive cpuset is created, we also create and attach a new root-domain
 * object.
 *
 */
struct root_domain {
	atomic_t		refcount;
/*
 * IAMROOT, 2023.02.11:
 * - rt overloaded task 숫자
 */
	atomic_t		rto_count;
	struct rcu_head		rcu;
	/*
	 * IAMROOT, 2023.02.04:
	 * - span: root_domain에 참여한 cpumask
	 * - online: root_domain 참여한 cpu중 schedule이 가능한 cpumask
	 */
	cpumask_var_t		span;
	cpumask_var_t		online;

	/*
	 * Indicate pullable load on at least one CPU, e.g:
	 * - More than one runnable task
	 * - Running task is misfit
	 */
	int			overload;

	/* Indicate one or more cpus over-utilized (tipping point) */
	int			overutilized;

	/*
	 * The bit corresponding to a CPU gets set here if such CPU has more
	 * than one runnable -deadline task (as it is below for RT tasks).
	 */
	cpumask_var_t		dlo_mask;
	atomic_t		dlo_count;
	struct dl_bw		dl_bw;
	struct cpudl		cpudl;

	/*
	 * Indicate whether a root_domain's dl_bw has been checked or
	 * updated. It's monotonously increasing value.
	 *
	 * Also, some corner cases, like 'wrap around' is dangerous, but given
	 * that u64 is 'big enough'. So that shouldn't be a concern.
	 */
	u64 visit_gen;

#ifdef HAVE_RT_PUSH_IPI
	/*
	 * For IPI pull requests, loop across the rto_mask.
	 */
	struct irq_work		rto_push_work;
	raw_spinlock_t		rto_lock;
	/* These are only updated and read within rto_lock */
	int			rto_loop;
/*
 * IAMROOT, 2023.02.11:
 * - IPI RT 푸시를 시작할 때 rto_cpu는 -1로 설정된다.
 */
	int			rto_cpu;
	/* These atomics are updated outside of a lock */
	atomic_t		rto_loop_next;
	atomic_t		rto_loop_start;
#endif
	/*
	 * The "RT overload" flag: it gets set if a CPU has more than
	 * one runnable RT task.
	 */
	cpumask_var_t		rto_mask;
	struct cpupri		cpupri;

	unsigned long		max_cpu_capacity;

	/*
	 * NULL-terminated list of performance domains intersecting with the
	 * CPUs of the rd. Protected by RCU.
	 */
	struct perf_domain __rcu *pd;
};

extern void init_defrootdomain(void);
extern int sched_init_domains(const struct cpumask *cpu_map);
extern void rq_attach_root(struct rq *rq, struct root_domain *rd);
extern void sched_get_rd(struct root_domain *rd);
extern void sched_put_rd(struct root_domain *rd);

#ifdef HAVE_RT_PUSH_IPI
extern void rto_push_irq_work_func(struct irq_work *work);
#endif
#endif /* CONFIG_SMP */

#ifdef CONFIG_UCLAMP_TASK
/*
 * struct uclamp_bucket - Utilization clamp bucket
 * @value: utilization clamp value for tasks on this clamp bucket
 * @tasks: number of RUNNABLE tasks on this clamp bucket
 *
 * Keep track of how many tasks are RUNNABLE for a given utilization
 * clamp value.
 */
struct uclamp_bucket {
	unsigned long value : bits_per(SCHED_CAPACITY_SCALE);
	unsigned long tasks : BITS_PER_LONG - bits_per(SCHED_CAPACITY_SCALE);
};

/*
 * struct uclamp_rq - rq's utilization clamp
 * @value: currently active clamp values for a rq
 * @bucket: utilization clamp buckets affecting a rq
 *
 * Keep track of RUNNABLE tasks on a rq to aggregate their clamp values.
 * A clamp value is affecting a rq when there is at least one task RUNNABLE
 * (or actually running) with that value.
 *
 * There are up to UCLAMP_CNT possible different clamp values, currently there
 * are only two: minimum utilization and maximum utilization.
 *
 * All utilization clamping values are MAX aggregated, since:
 * - for util_min: we want to run the CPU at least at the max of the minimum
 *   utilization required by its currently RUNNABLE tasks.
 * - for util_max: we want to allow the CPU to run up to the max of the
 *   maximum utilization allowed by its currently RUNNABLE tasks.
 *
 * Since on each system we expect only a limited number of different
 * utilization clamp values (UCLAMP_BUCKETS), use a simple array to track
 * the metrics required to compute all the per-rq utilization clamp values.
 */
struct uclamp_rq {
	unsigned int value;
	struct uclamp_bucket bucket[UCLAMP_BUCKETS];
};

DECLARE_STATIC_KEY_FALSE(sched_uclamp_used);
#endif /* CONFIG_UCLAMP_TASK */

/*
 * This is the main, per-CPU runqueue data structure.
 *
 * Locking rule: those places that want to lock multiple runqueues
 * (such as the load balancing or the thread migration code), lock
 * acquire operations must be ordered by ascending &runqueue.
 */
struct rq {
	/* runqueue lock: */
	raw_spinlock_t		__lock;

	/*
	 * nr_running and cpu_load should be in the same cacheline because
	 * remote CPUs use both these fields when doing load calculation.
	 */
	 /*
	  * IAMROOT, 2023.01.14:
	  * - nr_running : 해당 cpu의 rq에서 동작하는 모든 task 의 수
	  *   stop, dl, rt, cfs 에서 동작하는 task의 총합
	  */
	unsigned int		nr_running;
#ifdef CONFIG_NUMA_BALANCING
	unsigned int		nr_numa_running;
	unsigned int		nr_preferred_running;
	unsigned int		numa_migrate_on;
#endif
#ifdef CONFIG_NO_HZ_COMMON
#ifdef CONFIG_SMP

/*
 * IAMROOT, 2022.11.26:
 * - 잠들기전 tick을 저장.
 */
	unsigned long		last_blocked_load_update_tick;
	unsigned int		has_blocked_load;

/*
 * IAMROOT, 2022.11.26:
 * - call single data
 *   func : nohz_csd_func
 */
	call_single_data_t	nohz_csd;
#endif /* CONFIG_SMP */
	unsigned int		nohz_tick_stopped;
	atomic_t		nohz_flags;
#endif /* CONFIG_NO_HZ_COMMON */

#ifdef CONFIG_SMP
	unsigned int		ttwu_pending;
#endif
	u64			nr_switches;

#ifdef CONFIG_UCLAMP_TASK
	/* Utilization clamp values based on CPU's RUNNABLE tasks */
	struct uclamp_rq	uclamp[UCLAMP_CNT] ____cacheline_aligned;
	unsigned int		uclamp_flags;
#define UCLAMP_FLAG_IDLE 0x01
#endif

	struct cfs_rq		cfs;
	struct rt_rq		rt;
	struct dl_rq		dl;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* list of leaf cfs_rq on this CPU: */
	struct list_head	leaf_cfs_rq_list;
	struct list_head	*tmp_alone_branch;
#endif /* CONFIG_FAIR_GROUP_SCHED */

	/*
	 * This is part of a global counter where only the total sum
	 * over all CPUs matters. A task can increase this counter on
	 * one CPU and if it got migrated afterwards it may decrease
	 * it on another CPU. Always updated under the runqueue lock:
	 */
	/*
	 * IAMROOT. 2023.01.14:
	 * - google-translate
	 *   이것은 모든 CPU에 대한 총 합계만 중요한 글로벌 카운터의 일부입니다. 작업은 한
	 *   CPU에서 이 카운터를 증가시킬 수 있으며 나중에 마이그레이션된 경우 다른 CPU에서
	 *   감소시킬 수 있습니다. runqueue 잠금 상태에서 항상 업데이트됨:
	 * - load에 참여할 io thread가 io 대기중인 경우의 task 수
	 */
	unsigned int		nr_uninterruptible;

	struct task_struct __rcu	*curr;
	struct task_struct	*idle;
	struct task_struct	*stop;
	unsigned long		next_balance;
	struct mm_struct	*prev_mm;

	/*
	 * IAMROOT, 2023.01.14:
	 * - #define RQCF_REQ_SKIP		0x01
	 *   #define RQCF_ACT_SKIP		0x02
	 *   #define RQCF_UPDATED		0x04
	 */
	unsigned int		clock_update_flags;
/*
 * IAMROOT, 2022.12.22:
 * - clock
 *   rq에 대한 system clock의 현재 값.
 *
 * - clock_task
 *   interrupt등의 시간이 빠진 실제 task clock값.
 *
 * - clock_pelt
 *   rq에 대한 pelt clock의 현재값. cpu성능이 고려된 clock값.
 *   (update_rq_clock_pelt()참고)
 */
	u64			clock;
	/* Ensure that all clocks are in the same cache line */
	u64			clock_task ____cacheline_aligned;
	u64			clock_pelt;
/*
 * IAMROOT, 2022.12.22:
 * - cpu의 lost된 idle 시간의 적산. 
 *   task를 실행하지 않은 시간.
 */
	unsigned long		lost_idle_time;

	atomic_t		nr_iowait;

#ifdef CONFIG_SCHED_DEBUG
	u64 last_seen_need_resched_ns;
	int ticks_without_resched;
#endif

#ifdef CONFIG_MEMBARRIER
	int membarrier_state;
#endif

#ifdef CONFIG_SMP
	struct root_domain		*rd;
	struct sched_domain __rcu	*sd;

/*
 * IAMROOT, 2023.04.22:
 * - rt부분이 제외된 cpu capacity. 항상 변한다.
 *   update_cpu_capacity() 참고 
 */
	unsigned long		cpu_capacity;
/*
 * IAMROOT, 2023.02.11:
 * - 현재 cpu성능. cpu 원래 성능이 기록된다.
 *   update_cpu_capacity() 참고 
 */
	unsigned long		cpu_capacity_orig;

	struct callback_head	*balance_callback;

/*
 * IAMROOT, 2023.05.18:
 * - nohz_csd_func()참고. NOHZ_KICK_MASK등의 flag가 위치할수있다.
 */
	unsigned char		nohz_idle_balance;
	unsigned char		idle_balance;

/*
 * IAMROOT, 2023.05.03:
 * - task에 대한 load의 1.2배가 cpu capacity를 넘었을경우,
 *   h_load가 계산되어 기록된다. 그게 아니면 보통 0
 *   (update_misfit_status()참고)
 */
	unsigned long		misfit_task_load;

	/* For active balancing */
	int			active_balance;
	int			push_cpu;
	struct cpu_stop_work	active_balance_work;

	/* CPU of this runqueue: */
	int			cpu;
	int			online;

/*
 * IAMROOT, 2023.01.28:
 * - 현재 rq에서 runable하고있는 task들.
 */
	struct list_head cfs_tasks;

	struct sched_avg	avg_rt;
	struct sched_avg	avg_dl;
#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	struct sched_avg	avg_irq;
#endif
#ifdef CONFIG_SCHED_THERMAL_PRESSURE
	struct sched_avg	avg_thermal;
#endif
	u64			idle_stamp;
	u64			avg_idle;

/*
 * IAMROOT, 2023.06.15:
 * - wake_stamp: 그전에 깨울때(ttwu_do_wakeup)의 시간.
 * - wake_avg_idle : 일어날때까지 평균 idle시간
 */
	unsigned long		wake_stamp;
	u64			wake_avg_idle;

	/* This is used to determine avg_idle's max value */
	u64			max_idle_balance_cost;

#ifdef CONFIG_HOTPLUG_CPU
	struct rcuwait		hotplug_wait;
#endif
#endif /* CONFIG_SMP */

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	u64			prev_irq_time;
#endif
#ifdef CONFIG_PARAVIRT
	u64			prev_steal_time;
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64			prev_steal_time_rq;
#endif

	/* calc_load related fields */
	unsigned long		calc_load_update;

/*
 * IAMROOT, 2022.12.03:
 * - runing + nr_uninterruptible. calc_load_fold_active() 참고
 */
	long			calc_load_active;

#ifdef CONFIG_SCHED_HRTICK
#ifdef CONFIG_SMP
	call_single_data_t	hrtick_csd;
#endif
	struct hrtimer		hrtick_timer;
	ktime_t 		hrtick_time;
#endif

#ifdef CONFIG_SCHEDSTATS
	/* latency stats */
	struct sched_info	rq_sched_info;
	unsigned long long	rq_cpu_time;
	/* could above be rq->cfs_rq.exec_clock + rq->rt_rq.rt_runtime ? */

	/* sys_sched_yield() stats */
	unsigned int		yld_count;

	/* schedule() stats */
	unsigned int		sched_count;
	unsigned int		sched_goidle;

	/* try_to_wake_up() stats */
	unsigned int		ttwu_count;
	unsigned int		ttwu_local;
#endif

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a rcu lock section */
	struct cpuidle_state	*idle_state;
#endif

#ifdef CONFIG_SMP
	unsigned int		nr_pinned;
#endif
	unsigned int		push_busy;
	struct cpu_stop_work	push_work;

#ifdef CONFIG_SCHED_CORE
	/* per rq */
	struct rq		*core;
	struct task_struct	*core_pick;
	unsigned int		core_enabled;
	unsigned int		core_sched_seq;
	struct rb_root		core_tree;

	/* shared state -- careful with sched_core_cpu_deactivate() */
	unsigned int		core_task_seq;
	unsigned int		core_pick_seq;
	unsigned long		core_cookie;
	unsigned char		core_forceidle;
	unsigned int		core_forceidle_seq;
#endif
};

#ifdef CONFIG_FAIR_GROUP_SCHED

/* CPU runqueue to which this cfs_rq is attached */
static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return cfs_rq->rq;
}

#else

static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return container_of(cfs_rq, struct rq, cfs);
}
#endif

static inline int cpu_of(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq->cpu;
#else
	return 0;
#endif
}

#define MDF_PUSH	0x01

/*
 * IAMROOT, 2023.02.11:
 * - migration이 안되는 task면 return true.
 */
static inline bool is_migration_disabled(struct task_struct *p)
{
#ifdef CONFIG_SMP
	return p->migration_disabled;
#else
	return false;
#endif
}

struct sched_group;
#ifdef CONFIG_SCHED_CORE
static inline struct cpumask *sched_group_span(struct sched_group *sg);

DECLARE_STATIC_KEY_FALSE(__sched_core_enabled);

static inline bool sched_core_enabled(struct rq *rq)
{
	return static_branch_unlikely(&__sched_core_enabled) && rq->core_enabled;
}

static inline bool sched_core_disabled(void)
{
	return !static_branch_unlikely(&__sched_core_enabled);
}

/*
 * Be careful with this function; not for general use. The return value isn't
 * stable unless you actually hold a relevant rq->__lock.
 */
static inline raw_spinlock_t *rq_lockp(struct rq *rq)
{
	if (sched_core_enabled(rq))
		return &rq->core->__lock;

	return &rq->__lock;
}

static inline raw_spinlock_t *__rq_lockp(struct rq *rq)
{
	if (rq->core_enabled)
		return &rq->core->__lock;

	return &rq->__lock;
}

bool cfs_prio_less(struct task_struct *a, struct task_struct *b, bool fi);

/*
 * Helpers to check if the CPU's core cookie matches with the task's cookie
 * when core scheduling is enabled.
 * A special case is that the task's cookie always matches with CPU's core
 * cookie if the CPU is in an idle core.
 */
/*
 * IAMROOT. 2023.06.10:
 * - google-translate
 * 코어 스케줄링이 활성화되었을 때 CPU의 코어 쿠키가 작업의 쿠키와 일치하는지
 * 확인하는 도우미. 특별한 경우는 CPU가 유휴 코어에 있는 경우 작업의 쿠키가 항상
 * CPU의 코어 쿠키와 일치한다는 것입니다.
 */
static inline bool sched_cpu_cookie_match(struct rq *rq, struct task_struct *p)
{
	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	if (!sched_core_enabled(rq))
		return true;

	return rq->core->core_cookie == p->core_cookie;
}

static inline bool sched_core_cookie_match(struct rq *rq, struct task_struct *p)
{
	bool idle_core = true;
	int cpu;

	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	if (!sched_core_enabled(rq))
		return true;

	for_each_cpu(cpu, cpu_smt_mask(cpu_of(rq))) {
		if (!available_idle_cpu(cpu)) {
			idle_core = false;
			break;
		}
	}

	/*
	 * A CPU in an idle core is always the best choice for tasks with
	 * cookies.
	 */
	return idle_core || rq->core->core_cookie == p->core_cookie;
}

static inline bool sched_group_cookie_match(struct rq *rq,
					    struct task_struct *p,
					    struct sched_group *group)
{
	int cpu;

	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	if (!sched_core_enabled(rq))
		return true;

	for_each_cpu_and(cpu, sched_group_span(group), p->cpus_ptr) {
		if (sched_core_cookie_match(rq, p))
			return true;
	}
	return false;
}

extern void queue_core_balance(struct rq *rq);

static inline bool sched_core_enqueued(struct task_struct *p)
{
	return !RB_EMPTY_NODE(&p->core_node);
}

extern void sched_core_enqueue(struct rq *rq, struct task_struct *p);
extern void sched_core_dequeue(struct rq *rq, struct task_struct *p);

extern void sched_core_get(void);
extern void sched_core_put(void);

extern unsigned long sched_core_alloc_cookie(void);
extern void sched_core_put_cookie(unsigned long cookie);
extern unsigned long sched_core_get_cookie(unsigned long cookie);
extern unsigned long sched_core_update_cookie(struct task_struct *p, unsigned long cookie);

#else /* !CONFIG_SCHED_CORE */

static inline bool sched_core_enabled(struct rq *rq)
{
	return false;
}

static inline bool sched_core_disabled(void)
{
	return true;
}

static inline raw_spinlock_t *rq_lockp(struct rq *rq)
{
	return &rq->__lock;
}

static inline raw_spinlock_t *__rq_lockp(struct rq *rq)
{
	return &rq->__lock;
}

static inline void queue_core_balance(struct rq *rq)
{
}

/*
 * IAMROOT, 2023.06.10:
 * - skip
 */
static inline bool sched_cpu_cookie_match(struct rq *rq, struct task_struct *p)
{
	return true;
}

static inline bool sched_core_cookie_match(struct rq *rq, struct task_struct *p)
{
	return true;
}

static inline bool sched_group_cookie_match(struct rq *rq,
					    struct task_struct *p,
					    struct sched_group *group)
{
	return true;
}
#endif /* CONFIG_SCHED_CORE */

static inline void lockdep_assert_rq_held(struct rq *rq)
{
	lockdep_assert_held(__rq_lockp(rq));
}

extern void raw_spin_rq_lock_nested(struct rq *rq, int subclass);
extern bool raw_spin_rq_trylock(struct rq *rq);
extern void raw_spin_rq_unlock(struct rq *rq);

static inline void raw_spin_rq_lock(struct rq *rq)
{
	raw_spin_rq_lock_nested(rq, 0);
}

/*
 * IAMROOT, 2023.01.30:
 * - irq disable + spin lock
 */
static inline void raw_spin_rq_lock_irq(struct rq *rq)
{
	local_irq_disable();
	raw_spin_rq_lock(rq);
}

/*
 * IAMROOT, 2023.01.30:
 * - spin unlock + irq disable
 */
static inline void raw_spin_rq_unlock_irq(struct rq *rq)
{
	raw_spin_rq_unlock(rq);
	local_irq_enable();
}

static inline unsigned long _raw_spin_rq_lock_irqsave(struct rq *rq)
{
	unsigned long flags;
	local_irq_save(flags);
	raw_spin_rq_lock(rq);
	return flags;
}

static inline void raw_spin_rq_unlock_irqrestore(struct rq *rq, unsigned long flags)
{
	raw_spin_rq_unlock(rq);
	local_irq_restore(flags);
}

#define raw_spin_rq_lock_irqsave(rq, flags)	\
do {						\
	flags = _raw_spin_rq_lock_irqsave(rq);	\
} while (0)

#ifdef CONFIG_SCHED_SMT
extern void __update_idle_core(struct rq *rq);

static inline void update_idle_core(struct rq *rq)
{
	if (static_branch_unlikely(&sched_smt_present))
		__update_idle_core(rq);
}

#else
static inline void update_idle_core(struct rq *rq) { }
#endif

DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

/*
 * IAMROOT, 2022.11.26:
 * cpu 에 해당하는 per_cpu rq를 가져온다.
 */
#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
#define this_rq()		this_cpu_ptr(&runqueues)
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)
#define raw_rq()		raw_cpu_ptr(&runqueues)

#ifdef CONFIG_FAIR_GROUP_SCHED
static inline struct task_struct *task_of(struct sched_entity *se)
{
	SCHED_WARN_ON(!entity_is_task(se));
	return container_of(se, struct task_struct, se);
}

static inline struct cfs_rq *task_cfs_rq(struct task_struct *p)
{
	return p->se.cfs_rq;
}

/* runqueue on which this entity is (to be) queued */
/*
 * IAMROOT, 2022.12.21:
 * - @se가 소속된 cfs_rq를 가져온다.
 */
static inline struct cfs_rq *cfs_rq_of(struct sched_entity *se)
{
	return se->cfs_rq;
}

/* runqueue "owned" by this group */
/*
 * IAMROOT, 2022.12.21:
 * - 자신의 cfs_rq를 가져온다.
 */
static inline struct cfs_rq *group_cfs_rq(struct sched_entity *grp)
{
	return grp->my_q;
}

#else

static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}

static inline struct cfs_rq *task_cfs_rq(struct task_struct *p)
{
	return &task_rq(p)->cfs;
}

static inline struct cfs_rq *cfs_rq_of(struct sched_entity *se)
{
	struct task_struct *p = task_of(se);
	struct rq *rq = task_rq(p);

	return &rq->cfs;
}

/* runqueue "owned" by this group */
static inline struct cfs_rq *group_cfs_rq(struct sched_entity *grp)
{
	return NULL;
}
#endif

extern void update_rq_clock(struct rq *rq);

static inline u64 __rq_clock_broken(struct rq *rq)
{
	return READ_ONCE(rq->clock);
}

/*
 * rq::clock_update_flags bits
 *
 * %RQCF_REQ_SKIP - will request skipping of clock update on the next
 *  call to __schedule(). This is an optimisation to avoid
 *  neighbouring rq clock updates.
 *
 * %RQCF_ACT_SKIP - is set from inside of __schedule() when skipping is
 *  in effect and calls to update_rq_clock() are being ignored.
 *
 * %RQCF_UPDATED - is a debug flag that indicates whether a call has been
 *  made to update_rq_clock() since the last time rq::lock was pinned.
 *
 * If inside of __schedule(), clock_update_flags will have been
 * shifted left (a left shift is a cheap operation for the fast path
 * to promote %RQCF_REQ_SKIP to %RQCF_ACT_SKIP), so you must use,
 *
 *	if (rq-clock_update_flags >= RQCF_UPDATED)
 *
 * to check if %RQCF_UPDATED is set. It'll never be shifted more than
 * one position though, because the next rq_unpin_lock() will shift it
 * back.
 */
/*
 * IAMROOT. 2022.12.10:
 * - google-translate
 *   rq::clock_update_flags 비트
 *
 *   %RQCF_REQ_SKIP - __schedule()에 대한 다음 호출에서
 *   클록 업데이트 건너뛰기를 요청합니다. 이는 인접한 rq 클럭 업데이트를 피하기 위한
 *   최적화입니다.
 *
 *   %RQCF_ACT_SKIP - 건너뛰기가 적용되고 update_rq_clock()에 대한
 *   호출이 무시될 때 __schedule() 내부에서 설정됩니다.
 *
 *   %RQCF_UPDATED - rq::lock이
 *   마지막으로 고정된 이후 update_rq_clock()에 대한 호출이 이루어졌는지 여부를
 *   나타내는 디버그 플래그입니다.
 *
 *   __schedule() 내부에서 clock_update_flags가
 *   왼쪽으로 이동된 경우(왼쪽 이동은 빠른 경로에서 %RQCF_REQ_SKIP를 %RQCF_ACT_SKIP로
 *   승격하는 저렴한 작업임)
 *
 *   if (rq-clock_update_flags >= RQCF_UPDATED)
 *
 *   를 사용하여
 *   확인해야 합니다. %RQCF_UPDATED가 설정된 경우. 다음 rq_unpin_lock()이 그것을 다시
 *   이동시킬 것이기 때문에 그것은 결코 한 위치 이상 이동하지 않을 것입니다.
 */
#define RQCF_REQ_SKIP		0x01
#define RQCF_ACT_SKIP		0x02
#define RQCF_UPDATED		0x04

static inline void assert_clock_updated(struct rq *rq)
{
	/*
	 * The only reason for not seeing a clock update since the
	 * last rq_pin_lock() is if we're currently skipping updates.
	 */
	SCHED_WARN_ON(rq->clock_update_flags < RQCF_ACT_SKIP);
}

/*
 * IAMROOT, 2023.01.28:
 * - return clock
 */
static inline u64 rq_clock(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	assert_clock_updated(rq);

	return rq->clock;
}

/*
 * IAMROOT, 2023.02.11:
 * - 현재시각
 */
static inline u64 rq_clock_task(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	assert_clock_updated(rq);

	return rq->clock_task;
}

/**
 * By default the decay is the default pelt decay period.
 * The decay shift can change the decay period in
 * multiples of 32.
 *  Decay shift		Decay period(ms)
 *	0			32
 *	1			64
 *	2			128
 *	3			256
 *	4			512
 */
extern int sched_thermal_decay_shift;

static inline u64 rq_clock_thermal(struct rq *rq)
{
	return rq_clock_task(rq) >> sched_thermal_decay_shift;
}

static inline void rq_clock_skip_update(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	rq->clock_update_flags |= RQCF_REQ_SKIP;
}

/*
 * See rt task throttling, which is the only time a skip
 * request is canceled.
 */
/*
 * IAMROOT, 2023.02.18:
 * - clear RQCF_REQ_SKIP
 */
static inline void rq_clock_cancel_skipupdate(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	rq->clock_update_flags &= ~RQCF_REQ_SKIP;
}

struct rq_flags {
	unsigned long flags;
	struct pin_cookie cookie;
#ifdef CONFIG_SCHED_DEBUG
	/*
	 * A copy of (rq::clock_update_flags & RQCF_UPDATED) for the
	 * current pin context is stashed here in case it needs to be
	 * restored in rq_repin_lock().
	 */
	unsigned int clock_update_flags;
#endif
};

extern struct callback_head balance_push_callback;

/*
 * Lockdep annotation that avoids accidental unlocks; it's like a
 * sticky/continuous lockdep_assert_held().
 *
 * This avoids code that has access to 'struct rq *rq' (basically everything in
 * the scheduler) from accidentally unlocking the rq if they do not also have a
 * copy of the (on-stack) 'struct rq_flags rf'.
 *
 * Also see Documentation/locking/lockdep-design.rst.
 */
static inline void rq_pin_lock(struct rq *rq, struct rq_flags *rf)
{
	rf->cookie = lockdep_pin_lock(__rq_lockp(rq));

#ifdef CONFIG_SCHED_DEBUG
	rq->clock_update_flags &= (RQCF_REQ_SKIP|RQCF_ACT_SKIP);
	rf->clock_update_flags = 0;
#ifdef CONFIG_SMP
	SCHED_WARN_ON(rq->balance_callback && rq->balance_callback != &balance_push_callback);
#endif
#endif
}

static inline void rq_unpin_lock(struct rq *rq, struct rq_flags *rf)
{
#ifdef CONFIG_SCHED_DEBUG
	if (rq->clock_update_flags > RQCF_ACT_SKIP)
		rf->clock_update_flags = RQCF_UPDATED;
#endif

	lockdep_unpin_lock(__rq_lockp(rq), rf->cookie);
}

static inline void rq_repin_lock(struct rq *rq, struct rq_flags *rf)
{
	lockdep_repin_lock(__rq_lockp(rq), rf->cookie);

#ifdef CONFIG_SCHED_DEBUG
	/*
	 * Restore the value we stashed in @rf for this pin context.
	 */
	rq->clock_update_flags |= rf->clock_update_flags;
#endif
}

struct rq *__task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(rq->lock);

struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(p->pi_lock)
	__acquires(rq->lock);

static inline void __task_rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock(rq);
}

/*
 * IAMROOT, 2023.04.14:
 * - unlock 진행.
 */
static inline void
task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock(rq);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
}

static inline void
rq_lock_irqsave(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_rq_lock_irqsave(rq, rf->flags);
	rq_pin_lock(rq, rf);
}

static inline void
rq_lock_irq(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_rq_lock_irq(rq);
	rq_pin_lock(rq, rf);
}

static inline void
rq_lock(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_rq_lock(rq);
	rq_pin_lock(rq, rf);
}

static inline void
rq_relock(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_rq_lock(rq);
	rq_repin_lock(rq, rf);
}

static inline void
rq_unlock_irqrestore(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock_irqrestore(rq, rf->flags);
}

static inline void
rq_unlock_irq(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock_irq(rq);
}

static inline void
rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock(rq);
}

static inline struct rq *
this_rq_lock_irq(struct rq_flags *rf)
	__acquires(rq->lock)
{
	struct rq *rq;

	local_irq_disable();
	rq = this_rq();
	rq_lock(rq, rf);
	return rq;
}

#ifdef CONFIG_NUMA
enum numa_topology_type {
	NUMA_DIRECT,
	NUMA_GLUELESS_MESH,
	NUMA_BACKPLANE,
};
extern enum numa_topology_type sched_numa_topology_type;
extern int sched_max_numa_distance;
extern bool find_numa_distance(int distance);
extern void sched_init_numa(void);
extern void sched_domains_numa_masks_set(unsigned int cpu);
extern void sched_domains_numa_masks_clear(unsigned int cpu);
extern int sched_numa_find_closest(const struct cpumask *cpus, int cpu);
#else
static inline void sched_init_numa(void) { }
static inline void sched_domains_numa_masks_set(unsigned int cpu) { }
static inline void sched_domains_numa_masks_clear(unsigned int cpu) { }
static inline int sched_numa_find_closest(const struct cpumask *cpus, int cpu)
{
	return nr_cpu_ids;
}
#endif

#ifdef CONFIG_NUMA_BALANCING
/* The regions in numa_faults array from task_struct */
/*
 * IAMROOT, 2023.06.17:
 * - MEM : memory node
 *   CPU : cpu node
 */
enum numa_faults_stats {
/*
 * IAMROOT, 2023.06.17:
 * - NUMA_MEM, NUMA_CPU
 *   avg계산하여 저장을 해놓는 개념.
 */
	NUMA_MEM = 0,
	NUMA_CPU,
/*
 * IAMROOT, 2023.06.17:
 * - NUMA_MEMBUF, NUMA_CPUBUF
 *   temp buf. 즉시 sum이되는 buf
 */
	NUMA_MEMBUF,
	NUMA_CPUBUF
};
extern void sched_setnuma(struct task_struct *p, int node);
extern int migrate_task_to(struct task_struct *p, int cpu);
extern int migrate_swap(struct task_struct *p, struct task_struct *t,
			int cpu, int scpu);
extern void init_numa_balancing(unsigned long clone_flags, struct task_struct *p);
#else
static inline void
init_numa_balancing(unsigned long clone_flags, struct task_struct *p)
{
}
#endif /* CONFIG_NUMA_BALANCING */

#ifdef CONFIG_SMP

/*
 * IAMROOT, 2023.02.11:
 * - @func를 등록시켜준다.
 * - @func : push_rt_tasks
 *           pull_rt_task
 * - splice 및 실행 부분
 *   splice_balance_callbacks(), do_balance_callbacks()
 */
static inline void
queue_balance_callback(struct rq *rq,
		       struct callback_head *head,
		       void (*func)(struct rq *rq))
{
	lockdep_assert_rq_held(rq);

	if (unlikely(head->next || rq->balance_callback == &balance_push_callback))
		return;

	head->func = (void (*)(struct callback_head *))func;
	head->next = rq->balance_callback;
	rq->balance_callback = head;
}

#define rcu_dereference_check_sched_domain(p) \
	rcu_dereference_check((p), \
			      lockdep_is_held(&sched_domains_mutex))

/*
 * The domain tree (rq->sd) is protected by RCU's quiescent state transition.
 * See destroy_sched_domains: call_rcu for details.
 *
 * The domain tree of any CPU may only be accessed from within
 * preempt-disabled sections.
 */
/*
 * IAMROOT, 2022.09.03:
 */
/*
 * IAMROOT, 2023.02.11:
 * ----
 * - schedule domain
 *   -- SMT  : l1 cache 공유 
 *   -- MC   : l2 cache 공유
 *   -- DIE  : l3 cache 공유
 *   -- NUMA : memory 공유 numa level 1, 2, 3, 4로 세분화되기도한다.
 *   ex) SMT :  0, 1, MC : 0, 1, 2, 3, DIE : 0, 1, 2, 3, 4, 5, 6, 7
 *   이라고 가정.
 *   0번 cpu기준으로 SMT의 1번이 가장 가깝고 그다음 MC의 2, 3이 가깝고
 *   그다음 DIE의 4, 5, 6, 7이 가까울것이다.
 *   이런 묶음으로 domain을 구성하여 가까운 cpu를 찾아가는 개념.
 *
 *   -- 자기가 속한 domain은 span을 보고 금방 알아낸다.
 *
 * - schedule group
 *   -- cgroup의 group schedule과는 다른 개념.
 *   -- schedule domain의 아래 단계 개념
 * ---
 * - tree 구조로 되있다. sched domain 위로(SMT -> MC -> DIE -> NUMA)
 *   올라간다.
 */
#define for_each_domain(cpu, __sd) \
	for (__sd = rcu_dereference_check_sched_domain(cpu_rq(cpu)->sd); \
			__sd; __sd = __sd->parent)

/**
 * highest_flag_domain - Return highest sched_domain containing flag.
 * @cpu:	The CPU whose highest level of sched domain is to
 *		be returned.
 * @flag:	The flag to check for the highest sched_domain
 *		for the given CPU.
 *
 * Returns the highest sched_domain of a CPU which contains the given flag.
 */
/*
 * IAMROOT. 2023.04.29:
 * - google-translate
 * 가장 높은 플래그_도메인 - 플래그를 포함하는 가장 높은 sched_domain을 반환합니다.
 * @cpu: 가장 높은 수준의 sched 도메인을 반환할 CPU.
 * @flag: 주어진 CPU에 대해 가장 높은 sched_domain을 확인하는 플래그입니다.
 *
 * 주어진 플래그를 포함하는 CPU의 가장 높은 sched_domain을 반환합니다.
 */
static inline struct sched_domain *highest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd, *hsd = NULL;

	for_each_domain(cpu, sd) {
		if (!(sd->flags & flag))
			break;
		hsd = sd;
	}

	return hsd;
}

/*
 * IAMROOT, 2023.04.29:
 * - 주어진 플래그를 포함하는 CPU의 가장 낮은 sched_domain을 반환합니다.
 */
static inline struct sched_domain *lowest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd;

	for_each_domain(cpu, sd) {
		if (sd->flags & flag)
			break;
	}

	return sd;
}

DECLARE_PER_CPU(struct sched_domain __rcu *, sd_llc);
DECLARE_PER_CPU(int, sd_llc_size);
DECLARE_PER_CPU(int, sd_llc_id);
DECLARE_PER_CPU(struct sched_domain_shared __rcu *, sd_llc_shared);
DECLARE_PER_CPU(struct sched_domain __rcu *, sd_numa);
DECLARE_PER_CPU(struct sched_domain __rcu *, sd_asym_packing);
DECLARE_PER_CPU(struct sched_domain __rcu *, sd_asym_cpucapacity);
extern struct static_key_false sched_asym_cpucapacity;

struct sched_group_capacity {
	atomic_t		ref;
	/*
	 * CPU capacity of this group, SCHED_CAPACITY_SCALE being max capacity
	 * for a single CPU.
	 */
/*
 * IAMROOT, 2023.04.22:
 * - cpu개수로 초기값이 결정된다. build_balance_mask() 참고.
 */
	unsigned long		capacity;
	unsigned long		min_capacity;		/* Min per-CPU capacity in group */
	unsigned long		max_capacity;		/* Max per-CPU capacity in group */
	unsigned long		next_update;
	int			imbalance;		/* XXX unrelated to capacity but shared group state */

#ifdef CONFIG_SCHED_DEBUG
	int			id;
#endif

	unsigned long		cpumask[];		/* Balance mask */
};

struct sched_group {
	struct sched_group	*next;			/* Must be a circular list */
	atomic_t		ref;

	unsigned int		group_weight;
	struct sched_group_capacity *sgc;
	int			asym_prefer_cpu;	/* CPU of highest priority in group */

	/*
	 * The CPUs this group covers.
	 *
	 * NOTE: this field is variable length. (Allocated dynamically
	 * by attaching extra space to the end of the structure,
	 * depending on how many CPUs the kernel has booted up with)
	 */
/*
 * IAMROOT, 2023.04.22:
 * - balance mask.
 *   numa : build_balance_mask()설명 참고
 *   numa 이하 : get_group()참고.
 */
	unsigned long		cpumask[];
};

static inline struct cpumask *sched_group_span(struct sched_group *sg)
{
	return to_cpumask(sg->cpumask);
}

/*
 * See build_balance_mask().
 */
/*
 * IAMROOT, 2023.04.22:
 * - balance mask은 build_balance_mask()설명 참고
 */
static inline struct cpumask *group_balance_mask(struct sched_group *sg)
{
	return to_cpumask(sg->sgc->cpumask);
}

/**
 * group_first_cpu - Returns the first CPU in the cpumask of a sched_group.
 * @group: The group whose first CPU is to be returned.
 */
static inline unsigned int group_first_cpu(struct sched_group *group)
{
	return cpumask_first(sched_group_span(group));
}

extern int group_balance_cpu(struct sched_group *sg);

#ifdef CONFIG_SCHED_DEBUG
void update_sched_domain_debugfs(void);
void dirty_sched_domain_sysctl(int cpu);
#else
static inline void update_sched_domain_debugfs(void)
{
}
static inline void dirty_sched_domain_sysctl(int cpu)
{
}
#endif

extern int sched_update_scaling(void);

extern void flush_smp_call_function_from_idle(void);

#else /* !CONFIG_SMP: */
static inline void flush_smp_call_function_from_idle(void) { }
#endif

#include "stats.h"
#include "autogroup.h"

#ifdef CONFIG_CGROUP_SCHED

/*
 * Return the group to which this tasks belongs.
 *
 * We cannot use task_css() and friends because the cgroup subsystem
 * changes that value before the cgroup_subsys::attach() method is called,
 * therefore we cannot pin it and might observe the wrong value.
 *
 * The same is true for autogroup's p->signal->autogroup->tg, the autogroup
 * core changes this before calling sched_move_task().
 *
 * Instead we use a 'copy' which is updated from sched_move_task() while
 * holding both task_struct::pi_lock and rq::lock.
 */
static inline struct task_group *task_group(struct task_struct *p)
{
	return p->sched_task_group;
}

/* Change a task's cfs_rq and parent entity if it moves across CPUs/groups */
/*
 * IAMROOT, 2022.11.26:
 * - @p를 wakeup하는 @cpu에 해당하는 cfs_rq, tg를 설정한다.
 * - @p의 cfs, rt의 rq를 tg의 @cpu에 대한 cfs, rt의 rq로 설정한다.
 */
static inline void set_task_rq(struct task_struct *p, unsigned int cpu)
{
#if defined(CONFIG_FAIR_GROUP_SCHED) || defined(CONFIG_RT_GROUP_SCHED)
	struct task_group *tg = task_group(p);
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	set_task_rq_fair(&p->se, p->se.cfs_rq, tg->cfs_rq[cpu]);
	p->se.cfs_rq = tg->cfs_rq[cpu];
	p->se.parent = tg->se[cpu];
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	p->rt.rt_rq  = tg->rt_rq[cpu];
	p->rt.parent = tg->rt_se[cpu];
#endif
}

#else /* CONFIG_CGROUP_SCHED */

static inline void set_task_rq(struct task_struct *p, unsigned int cpu) { }
static inline struct task_group *task_group(struct task_struct *p)
{
	return NULL;
}

#endif /* CONFIG_CGROUP_SCHED */

/*
 * IAMROOT, 2022.11.26:
 * - @p를 wakeup하는 @cpu에 해당하는 cfs_rq, tg를 설정하고 cpu에 대한
 *   정보를 기록한다.
 * - @p의 cfs, rt rq를 @p의 tg의 @cpu에 대해서 설정한다.
 */
static inline void __set_task_cpu(struct task_struct *p, unsigned int cpu)
{
	set_task_rq(p, cpu);
#ifdef CONFIG_SMP
	/*
	 * After ->cpu is set up to a new value, task_rq_lock(p, ...) can be
	 * successfully executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *   After ->cpu 이 새로운 값으로 설정되면 task_rq_lock(p, ...)이 다른
 *   CPU에서 성공적으로 실행될 수 있습니다. 이 시점까지 작업별 데이터
 *   업데이트가 완료되었는지 확인해야 합니다.
 */
	smp_wmb();
#ifdef CONFIG_THREAD_INFO_IN_TASK
	WRITE_ONCE(p->cpu, cpu);
#else
	WRITE_ONCE(task_thread_info(p)->cpu, cpu);
#endif
	p->wake_cpu = cpu;
#endif
}

/*
 * Tunables that become constants when CONFIG_SCHED_DEBUG is off:
 */
#ifdef CONFIG_SCHED_DEBUG
# include <linux/static_key.h>
# define const_debug __read_mostly
#else
# define const_debug const
#endif

#define SCHED_FEAT(name, enabled)	\
	__SCHED_FEAT_##name ,

enum {
#include "features.h"
	__SCHED_FEAT_NR,
};

#undef SCHED_FEAT

#ifdef CONFIG_SCHED_DEBUG

/*
 * To support run-time toggling of sched features, all the translation units
 * (but core.c) reference the sysctl_sched_features defined in core.c.
 */
extern const_debug unsigned int sysctl_sched_features;

#ifdef CONFIG_JUMP_LABEL
#define SCHED_FEAT(name, enabled)					\
static __always_inline bool static_branch_##name(struct static_key *key) \
{									\
	return static_key_##enabled(key);				\
}

#include "features.h"
#undef SCHED_FEAT

extern struct static_key sched_feat_keys[__SCHED_FEAT_NR];
#define sched_feat(x) (static_branch_##x(&sched_feat_keys[__SCHED_FEAT_##x]))

#else /* !CONFIG_JUMP_LABEL */

#define sched_feat(x) (sysctl_sched_features & (1UL << __SCHED_FEAT_##x))

#endif /* CONFIG_JUMP_LABEL */

#else /* !SCHED_DEBUG */

/*
 * Each translation unit has its own copy of sysctl_sched_features to allow
 * constants propagation at compile time and compiler optimization based on
 * features default.
 */
#define SCHED_FEAT(name, enabled)	\
	(1UL << __SCHED_FEAT_##name) * enabled |
static const_debug __maybe_unused unsigned int sysctl_sched_features =
#include "features.h"
	0;
#undef SCHED_FEAT

#define sched_feat(x) !!(sysctl_sched_features & (1UL << __SCHED_FEAT_##x))

#endif /* SCHED_DEBUG */

extern struct static_key_false sched_numa_balancing;
extern struct static_key_false sched_schedstats;

/*
 * IAMROOT, 2022.11.26:
 * - ns단위 return. 1초 ( 1000 * 1000 * 1000) return.
 */
static inline u64 global_rt_period(void)
{

/*
 * IAMROOT, 2022.11.26:
 * - us단위를 ns로 변경하기위해 1000을 곱한다.
 */
	return (u64)sysctl_sched_rt_period * NSEC_PER_USEC;
}

/*
 * IAMROOT, 2022.11.26:
 * - ns단위 return. 0.95초(950 * 1000 * 1000)
 */
static inline u64 global_rt_runtime(void)
{
	if (sysctl_sched_rt_runtime < 0)
		return RUNTIME_INF;

	return (u64)sysctl_sched_rt_runtime * NSEC_PER_USEC;
}

static inline int task_current(struct rq *rq, struct task_struct *p)
{
	return rq->curr == p;
}

/*
 * IAMROOT, 2023.05.13:
 * - on_cpu: 현재 동작중인 task. curr 보다 신뢰성이 있다.
 */
static inline int task_running(struct rq *rq, struct task_struct *p)
{
#ifdef CONFIG_SMP
	return p->on_cpu;
#else
	return task_current(rq, p);
#endif
}

static inline int task_on_rq_queued(struct task_struct *p)
{
	return p->on_rq == TASK_ON_RQ_QUEUED;
}

static inline int task_on_rq_migrating(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_MIGRATING;
}

/* Wake flags. The first three directly map to some SD flag value */
#define WF_EXEC     0x02 /* Wakeup after exec; maps to SD_BALANCE_EXEC */
#define WF_FORK     0x04 /* Wakeup after fork; maps to SD_BALANCE_FORK */
/*
 * IAMROOT, 2023.03.04:
 * - try to wakeup
 */
#define WF_TTWU     0x08 /* Wakeup;            maps to SD_BALANCE_WAKE */

/*
 * IAMROOT, 2023.06.03:
 * - waker는 wakeup을 한 후 sleep을 하러 가야된다는 뜻.
 */
#define WF_SYNC     0x10 /* Waker goes to sleep after wakeup */
#define WF_MIGRATED 0x20 /* Internal use, task got migrated */
#define WF_ON_CPU   0x40 /* Wakee is on_cpu */

#ifdef CONFIG_SMP
static_assert(WF_EXEC == SD_BALANCE_EXEC);
static_assert(WF_FORK == SD_BALANCE_FORK);
static_assert(WF_TTWU == SD_BALANCE_WAKE);
#endif

/*
 * To aid in avoiding the subversion of "niceness" due to uneven distribution
 * of tasks with abnormal "nice" values across CPUs the contribution that
 * each task makes to its run queue's load is weighted according to its
 * scheduling class and "nice" value. For SCHED_NORMAL tasks this is just a
 * scaled version of the new time slice allocation that they receive on time
 * slice expiry etc.
 */

/*
 * IAMROOT, 2022.12.29:
 * - papago
 *   CPU 전체에서 비정상적인 nice 값을 가진 작업의 고르지 않은 배포로 인한 
 *   niceness의 전복을 방지하기 위해 실행 대기열의 로드에 대한 각 작업의 
 *   기여도는 스케줄링 클래스 및 nice 값에 따라 가중치가 부여됩니다. 
 *   SCHED_NORMAL 작업의 경우 이는 시간 조각 만료 등에서 받는 새 시간 조각 
 *   할당의 확장된 버전일 뿐입니다.
.*
 * - sched_prio_to_weight를 참고하면 제일 낮은게 15이다. idle은 이것보다도 낮은
 *   3을 사용한다.
 */
#define WEIGHT_IDLEPRIO		3
#define WMULT_IDLEPRIO		1431655765

extern const int		sched_prio_to_weight[40];
extern const u32		sched_prio_to_wmult[40];

/*
 * {de,en}queue flags:
 *
 * DEQUEUE_SLEEP  - task is no longer runnable
 * ENQUEUE_WAKEUP - task just became runnable
 *
 * SAVE/RESTORE - an otherwise spurious dequeue/enqueue, done to ensure tasks
 *                are in a known state which allows modification. Such pairs
 *                should preserve as much state as possible.
 *
 * MOVE - paired with SAVE/RESTORE, explicitly does not preserve the location
 *        in the runqueue.
 *
 * ENQUEUE_HEAD      - place at front of runqueue (tail if not specified)
 * ENQUEUE_REPLENISH - CBS (replenish runtime and postpone deadline)
 * ENQUEUE_MIGRATED  - the task was migrated during wakeup
 *
 */
/*
 * IAMROOT. 2023.01.14:
 * - google-translate
 *   {de,en}대기열 플래그:
 *
 *   DEQUEUE_SLEEP - 작업이 더 이상 실행 가능하지 않음
 *   ENQUEUE_WAKEUP - 작업이 실행 가능함
 *
 *   SAVE/RESTORE - 작업이 수정을 허용하는 알려진 상태에 있는지 확인하기 위해
 *   수행되는 가짜 대기열에서 빼기/넣기입니다. 이러한 쌍은 가능한 한 많은 
 *   상태를 보존해야 합니다.
 *
 *   MOVE -SAVE/RESTORE와 쌍을 이루어 실행 대기열의 위치를 명시적으로
 *   보존하지 않습니다.
 *
 *   ENQUEUE_HEAD - runqueue 앞에 배치(지정되지 않은 경우 꼬리)
 *   ENQUEUE_REPLENISH - CBS(런타임 보충 및 기한 연기)
 *   ENQUEUE_MIGRATED - 깨우는 동안 작업이 마이그레이션됨
 *
 * - ENQUEUE_WAKEUP / DEQUEUE_SLEEP
 *   sleep했었다가 깨어나는 상황(ENQUEUE_WAKEUP)
 *   task sleep으로 인한 dequeue(DEQUEUE_SLEEP)
 *
 * - DEQUEUE_MOVE / ENQUEUE_MOVE
 *   cgroup간의 이동.
 *
 * - ENQUEUE_RESTORE/ DEQUEUE_SAVE
 *   설정 변경에 따라 잠깐 dequee/enqueue 할때 사용한다.
 */

#define DEQUEUE_SLEEP		0x01
#define DEQUEUE_SAVE		0x02 /* Matches ENQUEUE_RESTORE */
#define DEQUEUE_MOVE		0x04 /* Matches ENQUEUE_MOVE */
#define DEQUEUE_NOCLOCK		0x08 /* Matches ENQUEUE_NOCLOCK */

#define ENQUEUE_WAKEUP		0x01
#define ENQUEUE_RESTORE		0x02
#define ENQUEUE_MOVE		0x04
#define ENQUEUE_NOCLOCK		0x08

#define ENQUEUE_HEAD		0x10
#define ENQUEUE_REPLENISH	0x20
#ifdef CONFIG_SMP
#define ENQUEUE_MIGRATED	0x40
#else
#define ENQUEUE_MIGRATED	0x00
#endif

#define RETRY_TASK		((void *)-1UL)

/*
 * IAMROOT, 2022.12.19:
 * - chat openai
 *   "enqueue_task" is a pointer to a function that is used to add a task to 
 *   the runqueue of a CPU.
 *
 *   "dequeue_task" is a pointer to a function that is used to remove a task 
 *   from the runqueue of a CPU.
 *
 *   "yield_task" is a pointer to a function that is called when a task 
 *   yields the CPU to allow other tasks to run.
 *
 *   "yield_to_task" is a pointer to a function that is called when a task 
 *   attempts to yield the CPU to a specific task. The function returns a 
 *   boolean value indicating whether the yield was successful. 
 *
 *   "check_preempt_curr" is a pointer to a function that is called to check 
 *   whether the current task should be preempted by a higher-priority task.
 *   - 자신보다 우선순위가 높은게 있으면 바꾼다.
 *
 *   "pick_next_task" is a pointer to a function that is used to select 
 *   the next task to run on a CPU. 
 *
 *   "put_prev_task" is a pointer to a function that is called when a task 
 *   is no longer the current task on a CPU.  
 *
 *   "set_next_task" is a pointer to a function that is called to set the 
 *   next task to run on a CPU. 
 *   - ex) A -> B task로 변경되는과정.
 *   B를 선택(pick_next_task) -> curr(A)를 정리(put_prev_task) ->
 *   B를 curr로 변경(set_next_task)
 *
 *   "balance" is a pointer to a function that is used to balance the load 
 *   across different CPUs in a system. 
 *   - load balance 수행.
 *
 *   "select_task_rq" is a pointer to a function that is used to select the 
 *   runqueue on which a task should be queued. 
 *
 *   "pick_task" is a pointer to a function that is used to select a task 
 *   for migration to another CPU.
 *
 *   "migrate_task_rq" is a pointer to a function that is called when a task 
 *   is migrated to another CPU.
 *
 *   "task_woken" is a pointer to a function that is called when a task is 
 *   woken up from sleep.
 *
 *   "set_cpus_allowed" is a pointer to a function that is called to set the 
 *   CPUs on which a task is allowed to run.
 *
 *   "rq_online" is a pointer to a function that is called when a runqueue 
 *   becomes available for scheduling.
 *   - cpu on시 동작
 *
 *   "rq_offline" is a pointer to a function that is called when a runqueue 
 *   becomes unavailable for scheduling.
 *   - cpu off시 동작
 *
 *   "find_lock_rq" is a pointer to a function that is used to find and lock 
 *   the runqueue on which a task is queued.
 *
 *   "task_tick" is a pointer to a function that is called when a task is 
 *   selected to run on a CPU.
 *   - schedule tick.
 *
 *   "task_fork" is a pointer to a function that is called when a new task 
 *   is forked. 
 *   - fork 될때.
 *
 *   "task_dead" is a pointer to a function that is called when a task exits.
 *
 *   "switched_from" is a pointer to a function that is called when a task 
 *   is switched from one CPU to another.
 *   - 스케쥴러가 바뀔때.
 *
 *   "switched_to" is a pointer to a function that is called when a task is 
 *   switched to a new CPU. 
 *   - 스케쥴러가 바뀔때.
 *
 *   "prio_changed" is a pointer to a function that is called when the 
 *   priority of a task changes.
*
*   "get_rr_interval" is a pointer to a function that is used to calculate 
*   the time slice (in nanoseconds) for a round-robin task. 
*
*   "update_curr" is a pointer to a function that is called to update the 
*   current task on a CPU.
*
*   "task_change_group" is a pointer to a function that is called when a 
*   task is moved to a new cgroup or when the cgroup of a task is changed.
*
*   "uclamp_enabled" is a field that indicates whether uclamp 
*   (user-level clamping) is enabled for this scheduling class.
*/
struct sched_class {

#ifdef CONFIG_UCLAMP_TASK
	int uclamp_enabled;
#endif

	void (*enqueue_task) (struct rq *rq, struct task_struct *p, int flags);
	void (*dequeue_task) (struct rq *rq, struct task_struct *p, int flags);
	void (*yield_task)   (struct rq *rq);
	bool (*yield_to_task)(struct rq *rq, struct task_struct *p);

	void (*check_preempt_curr)(struct rq *rq, struct task_struct *p, int flags);

	struct task_struct *(*pick_next_task)(struct rq *rq);

	void (*put_prev_task)(struct rq *rq, struct task_struct *p);
	void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first);

#ifdef CONFIG_SMP
	int (*balance)(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);
	int  (*select_task_rq)(struct task_struct *p, int task_cpu, int flags);

	struct task_struct * (*pick_task)(struct rq *rq);

	void (*migrate_task_rq)(struct task_struct *p, int new_cpu);

	void (*task_woken)(struct rq *this_rq, struct task_struct *task);

	void (*set_cpus_allowed)(struct task_struct *p,
				 const struct cpumask *newmask,
				 u32 flags);

	void (*rq_online)(struct rq *rq);
	void (*rq_offline)(struct rq *rq);

	struct rq *(*find_lock_rq)(struct task_struct *p, struct rq *rq);
#endif

	void (*task_tick)(struct rq *rq, struct task_struct *p, int queued);
	void (*task_fork)(struct task_struct *p);
	void (*task_dead)(struct task_struct *p);

	/*
	 * The switched_from() call is allowed to drop rq->lock, therefore we
	 * cannot assume the switched_from/switched_to pair is serialized by
	 * rq->lock. They are however serialized by p->pi_lock.
	 */
	void (*switched_from)(struct rq *this_rq, struct task_struct *task);
	void (*switched_to)  (struct rq *this_rq, struct task_struct *task);
	void (*prio_changed) (struct rq *this_rq, struct task_struct *task,
			      int oldprio);

	unsigned int (*get_rr_interval)(struct rq *rq,
					struct task_struct *task);

	void (*update_curr)(struct rq *rq);

#define TASK_SET_GROUP		0
#define TASK_MOVE_GROUP		1

#ifdef CONFIG_FAIR_GROUP_SCHED
	void (*task_change_group)(struct task_struct *p, int type);
#endif
};

static inline void put_prev_task(struct rq *rq, struct task_struct *prev)
{
	WARN_ON_ONCE(rq->curr != prev);
	prev->sched_class->put_prev_task(rq, prev);
}

/*
 * IAMROOT, 2023.02.11:
 * - exec_start를 갱신하고 선택된 @p를 pushable에서 dequeue한다.
 *   ex) rt : set_next_task_rt()
 */
static inline void set_next_task(struct rq *rq, struct task_struct *next)
{
	next->sched_class->set_next_task(rq, next, false);
}


/*
 * Helper to define a sched_class instance; each one is placed in a separate
 * section which is ordered by the linker script:
 *
 *   include/asm-generic/vmlinux.lds.h
 *
 * Also enforce alignment on the instance, not the type, to guarantee layout.
 */
#define DEFINE_SCHED_CLASS(name) \
const struct sched_class name##_sched_class \
	__aligned(__alignof__(struct sched_class)) \
	__section("__" #name "_sched_class")

/* Defined in include/asm-generic/vmlinux.lds.h */
extern struct sched_class __begin_sched_classes[];
extern struct sched_class __end_sched_classes[];

#define sched_class_highest (__end_sched_classes - 1)
#define sched_class_lowest  (__begin_sched_classes - 1)

#define for_class_range(class, _from, _to) \
	for (class = (_from); class != (_to); class--)

#define for_each_class(class) \
	for_class_range(class, sched_class_highest, sched_class_lowest)

extern const struct sched_class stop_sched_class;
extern const struct sched_class dl_sched_class;
extern const struct sched_class rt_sched_class;
extern const struct sched_class fair_sched_class;
extern const struct sched_class idle_sched_class;

static inline bool sched_stop_runnable(struct rq *rq)
{
	return rq->stop && task_on_rq_queued(rq->stop);
}

static inline bool sched_dl_runnable(struct rq *rq)
{
	return rq->dl.dl_nr_running > 0;
}

static inline bool sched_rt_runnable(struct rq *rq)
{
	return rq->rt.rt_queued > 0;
}

/*
 * IAMROOT, 2023.01.14:
 * - cfs_rq에 동작하는 entity가 하나이상 있으면 true 반환
 */
static inline bool sched_fair_runnable(struct rq *rq)
{
	return rq->cfs.nr_running > 0;
}

extern struct task_struct *pick_next_task_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);
extern struct task_struct *pick_next_task_idle(struct rq *rq);

#define SCA_CHECK		0x01
#define SCA_MIGRATE_DISABLE	0x02
#define SCA_MIGRATE_ENABLE	0x04
#define SCA_USER		0x08

#ifdef CONFIG_SMP

extern void update_group_capacity(struct sched_domain *sd, int cpu);

extern void trigger_load_balance(struct rq *rq);

extern void set_cpus_allowed_common(struct task_struct *p, const struct cpumask *new_mask, u32 flags);

/*
 * IAMROOT, 2023.02.11:
 * - @rq->curr가 움직일수있는 상태인지 확인한다. 가능하면 ref를 얻고 
 *   return task.
 */
static inline struct task_struct *get_push_task(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	lockdep_assert_rq_held(rq);

/*
 * IAMROOT, 2023.02.11:
 * - 이미 누군가 push를 할려고한다.
 */
	if (rq->push_busy)
		return NULL;

/*
 * IAMROOT, 2023.02.11:
 * - 이 task은 허락된 cpu가 한개밖에 없어 이동을 못한다.
 */
	if (p->nr_cpus_allowed == 1)
		return NULL;

/*
 * IAMROOT, 2023.02.11:
 * - migrate disable
 */
	if (p->migration_disabled)
		return NULL;

	rq->push_busy = true;
	return get_task_struct(p);
}

extern int push_cpu_stop(void *arg);

#endif

#ifdef CONFIG_CPU_IDLE				\
/*
 * IAMROOT, 2023.03.11:
 * - rq의 idle_state 구조체 변수를 @idle_state로 설정
 */
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
	rq->idle_state = idle_state;
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	SCHED_WARN_ON(!rcu_read_lock_held());

	return rq->idle_state;
}
#else
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	return NULL;
}
#endif

extern void schedule_idle(void);

extern void sysrq_sched_debug_show(void);
extern void sched_init_granularity(void);
extern void update_max_interval(void);

extern void init_sched_dl_class(void);
extern void init_sched_rt_class(void);
extern void init_sched_fair_class(void);

extern void reweight_task(struct task_struct *p, int prio);

extern void resched_curr(struct rq *rq);
extern void resched_cpu(int cpu);

extern struct rt_bandwidth def_rt_bandwidth;
extern void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime);

extern struct dl_bandwidth def_dl_bandwidth;
extern void init_dl_bandwidth(struct dl_bandwidth *dl_b, u64 period, u64 runtime);
extern void init_dl_task_timer(struct sched_dl_entity *dl_se);
extern void init_dl_inactive_task_timer(struct sched_dl_entity *dl_se);

#define BW_SHIFT		20
#define BW_UNIT			(1 << BW_SHIFT)
#define RATIO_SHIFT		8
#define MAX_BW_BITS		(64 - BW_SHIFT)
#define MAX_BW			((1ULL << MAX_BW_BITS) - 1)
unsigned long to_ratio(u64 period, u64 runtime);

extern void init_entity_runnable_average(struct sched_entity *se);
extern void post_init_entity_util_avg(struct task_struct *p);

#ifdef CONFIG_NO_HZ_FULL
extern bool sched_can_stop_tick(struct rq *rq);
extern int __init sched_tick_offload_init(void);

/*
 * Tick may be needed by tasks in the runqueue depending on their policy and
 * requirements. If tick is needed, lets send the target an IPI to kick it out of
 * nohz mode if necessary.
 */
static inline void sched_update_tick_dependency(struct rq *rq)
{
	int cpu = cpu_of(rq);

	if (!tick_nohz_full_cpu(cpu))
		return;

	if (sched_can_stop_tick(rq))
		tick_nohz_dep_clear_cpu(cpu, TICK_DEP_BIT_SCHED);
	else
		tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_SCHED);
}
#else
static inline int sched_tick_offload_init(void) { return 0; }
static inline void sched_update_tick_dependency(struct rq *rq) { }
#endif

static inline void add_nr_running(struct rq *rq, unsigned count)
{
	unsigned prev_nr = rq->nr_running;

	rq->nr_running = prev_nr + count;
	if (trace_sched_update_nr_running_tp_enabled()) {
		call_trace_sched_update_nr_running(rq, count);
	}

#ifdef CONFIG_SMP
	if (prev_nr < 2 && rq->nr_running >= 2) {
		if (!READ_ONCE(rq->rd->overload))
			WRITE_ONCE(rq->rd->overload, 1);
	}
#endif

	sched_update_tick_dependency(rq);
}

static inline void sub_nr_running(struct rq *rq, unsigned count)
{
	rq->nr_running -= count;
	if (trace_sched_update_nr_running_tp_enabled()) {
		call_trace_sched_update_nr_running(rq, -count);
	}

	/* Check if we still need preemption */
	sched_update_tick_dependency(rq);
}

extern void activate_task(struct rq *rq, struct task_struct *p, int flags);
extern void deactivate_task(struct rq *rq, struct task_struct *p, int flags);

extern void check_preempt_curr(struct rq *rq, struct task_struct *p, int flags);

extern const_debug unsigned int sysctl_sched_nr_migrate;
extern const_debug unsigned int sysctl_sched_migration_cost;

#ifdef CONFIG_SCHED_DEBUG
extern unsigned int sysctl_sched_latency;
extern unsigned int sysctl_sched_min_granularity;
extern unsigned int sysctl_sched_wakeup_granularity;
extern int sysctl_resched_latency_warn_ms;
extern int sysctl_resched_latency_warn_once;

extern unsigned int sysctl_sched_tunable_scaling;

extern unsigned int sysctl_numa_balancing_scan_delay;
extern unsigned int sysctl_numa_balancing_scan_period_min;
extern unsigned int sysctl_numa_balancing_scan_period_max;
extern unsigned int sysctl_numa_balancing_scan_size;
#endif

#ifdef CONFIG_SCHED_HRTICK

/*
 * Use hrtick when:
 *  - enabled by features
 *  - hrtimer is actually high res
 */
/*
 * IAMROOT, 2023.02.25:
 * - hres timer는 cfs, dl 에서 task의 runtime에 맞춰 hrtick이 딱맞게 발생하도록 한다.
 */
static inline int hrtick_enabled(struct rq *rq)
{
	if (!cpu_active(cpu_of(rq)))
		return 0;
	return hrtimer_is_hres_active(&rq->hrtick_timer);
}

/*
 * IAMROOT, 2023.01.28:
 * - sched HRTICK 지원 확인.
 */
static inline int hrtick_enabled_fair(struct rq *rq)
{
	if (!sched_feat(HRTICK))
		return 0;
	return hrtick_enabled(rq);
}

/*
 * IAMROOT, 2023.03.04:
 * - HRTICK_DL을 지원한다면 hrtick을 enable한다.
 */
static inline int hrtick_enabled_dl(struct rq *rq)
{
	if (!sched_feat(HRTICK_DL))
		return 0;
	return hrtick_enabled(rq);
}

void hrtick_start(struct rq *rq, u64 delay);

#else

static inline int hrtick_enabled_fair(struct rq *rq)
{
	return 0;
}

static inline int hrtick_enabled_dl(struct rq *rq)
{
	return 0;
}

static inline int hrtick_enabled(struct rq *rq)
{
	return 0;
}

#endif /* CONFIG_SCHED_HRTICK */

#ifndef arch_scale_freq_tick
static __always_inline
void arch_scale_freq_tick(void)
{
}
#endif

#ifndef arch_scale_freq_capacity
/**
 * arch_scale_freq_capacity - get the frequency scale factor of a given CPU.
 * @cpu: the CPU in question.
 *
 * Return: the frequency scale factor normalized against SCHED_CAPACITY_SCALE, i.e.
 *
 *     f_curr
 *     ------ * SCHED_CAPACITY_SCALE
 *     f_max
 */
static __always_inline
unsigned long arch_scale_freq_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif


#ifdef CONFIG_SMP

static inline bool rq_order_less(struct rq *rq1, struct rq *rq2)
{
#ifdef CONFIG_SCHED_CORE
	/*
	 * In order to not have {0,2},{1,3} turn into into an AB-BA,
	 * order by core-id first and cpu-id second.
	 *
	 * Notably:
	 *
	 *	double_rq_lock(0,3); will take core-0, core-1 lock
	 *	double_rq_lock(1,2); will take core-1, core-0 lock
	 *
	 * when only cpu-id is considered.
	 */
	if (rq1->core->cpu < rq2->core->cpu)
		return true;
	if (rq1->core->cpu > rq2->core->cpu)
		return false;

	/*
	 * __sched_core_flip() relies on SMT having cpu-id lock order.
	 */
#endif
	return rq1->cpu < rq2->cpu;
}

extern void double_rq_lock(struct rq *rq1, struct rq *rq2);

#ifdef CONFIG_PREEMPTION

/*
 * fair double_lock_balance: Safely acquires both rq->locks in a fair
 * way at the expense of forcing extra atomic operations in all
 * invocations.  This assures that the double_lock is acquired using the
 * same underlying policy as the spinlock_t on this architecture, which
 * reduces latency compared to the unfair variant below.  However, it
 * also adds more overhead and therefore may reduce throughput.
 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   fair double_lock_balance: 모든 호출에서 추가 원자적 작업을 강제하는 
 *   비용으로 공정한 방식으로 두 rq-> 잠금을 안전하게 획득합니다. 이렇게 
 *   하면 이 아키텍처에서 spinlock_t와 동일한 기본 정책을 사용하여 
 *   double_lock을 획득할 수 있으므로 아래의 불공평한 변형에 비해 대기 
 *   시간이 줄어듭니다. 그러나 더 많은 오버헤드가 추가되므로 처리량이 
 *   줄어들 수 있습니다.
 *
 * - double lock을 얻는다. preempt은 무조건 경합한다.
 *  return 1.
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(this_rq->lock)
	__acquires(busiest->lock)
	__acquires(this_rq->lock)
{
	raw_spin_rq_unlock(this_rq);
	double_rq_lock(this_rq, busiest);

	return 1;
}

#else
/*
 * Unfair double_lock_balance: Optimizes throughput at the expense of
 * latency by eliminating extra atomic operations when the locks are
 * already in proper order on entry.  This favors lower CPU-ids and will
 * grant the double lock to lower CPUs over higher ids under contention,
 * regardless of entry order into the function.
 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   Unfair double_lock_balance: 진입 시 잠금이 이미 적절한 순서로 되어 
 *   있는 경우 추가적인 원자적 작업을 제거하여 대기 시간을 희생하여 
 *   처리량을 최적화합니다. 이것은 더 낮은 CPU ID를 선호하며 함수에 대한 
 *   항목 순서에 관계없이 경합 중인 더 높은 ID보다 더 낮은 CPU에 이중 
 *   잠금을 부여합니다.
 *
 * - double lock을 얻는다. 경합없이 얻으면 return 0. 있으면 return 1.
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(this_rq->lock)
	__acquires(busiest->lock)
	__acquires(this_rq->lock)
{
	if (__rq_lockp(this_rq) == __rq_lockp(busiest))
		return 0;

	if (likely(raw_spin_rq_trylock(busiest)))
		return 0;

	if (rq_order_less(this_rq, busiest)) {
		raw_spin_rq_lock_nested(busiest, SINGLE_DEPTH_NESTING);
		return 0;
	}

/*
 * IAMROOT, 2023.02.18:
 * - push_rt_task() 중간의 주석을 봤을때 unlock과 doublelock 사이에
 *   migrate가 될 가능할수도 있다는 언급이 있다.
 */
	raw_spin_rq_unlock(this_rq);
	double_rq_lock(this_rq, busiest);

	return 1;
}

#endif /* CONFIG_PREEMPTION */

/*
 * double_lock_balance - lock the busiest runqueue, this_rq is locked already.
 */
/*
 * IAMROOT, 2023.02.11:
 * - double lock 획득.
 * - double lock을 얻는다. 경합없이 얻으면 return 0. 있으면 return 1.
 */
static inline int double_lock_balance(struct rq *this_rq, struct rq *busiest)
{
	lockdep_assert_irqs_disabled();

	return _double_lock_balance(this_rq, busiest);
}

static inline void double_unlock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(busiest->lock)
{
	if (__rq_lockp(this_rq) != __rq_lockp(busiest))
		raw_spin_rq_unlock(busiest);
	lock_set_subclass(&__rq_lockp(this_rq)->dep_map, 0, _RET_IP_);
}

static inline void double_lock(spinlock_t *l1, spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

static inline void double_lock_irq(spinlock_t *l1, spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock_irq(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

static inline void double_raw_lock(raw_spinlock_t *l1, raw_spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	raw_spin_lock(l1);
	raw_spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	if (__rq_lockp(rq1) != __rq_lockp(rq2))
		raw_spin_rq_unlock(rq2);
	else
		__release(rq2->lock);
	raw_spin_rq_unlock(rq1);
}

extern void set_rq_online (struct rq *rq);
extern void set_rq_offline(struct rq *rq);
extern bool sched_smp_initialized;

#else /* CONFIG_SMP */

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */
static inline void double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	BUG_ON(!irqs_disabled());
	BUG_ON(rq1 != rq2);
	raw_spin_rq_lock(rq1);
	__acquire(rq2->lock);	/* Fake it out ;) */
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	BUG_ON(rq1 != rq2);
	raw_spin_rq_unlock(rq1);
	__release(rq2->lock);
}

#endif

extern struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq);
extern struct sched_entity *__pick_last_entity(struct cfs_rq *cfs_rq);

#ifdef	CONFIG_SCHED_DEBUG
extern bool sched_debug_verbose;

extern void print_cfs_stats(struct seq_file *m, int cpu);
extern void print_rt_stats(struct seq_file *m, int cpu);
extern void print_dl_stats(struct seq_file *m, int cpu);
extern void print_cfs_rq(struct seq_file *m, int cpu, struct cfs_rq *cfs_rq);
extern void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq);
extern void print_dl_rq(struct seq_file *m, int cpu, struct dl_rq *dl_rq);

extern void resched_latency_warn(int cpu, u64 latency);
#ifdef CONFIG_NUMA_BALANCING
extern void
show_numa_stats(struct task_struct *p, struct seq_file *m);
extern void
print_numa_stats(struct seq_file *m, int node, unsigned long tsf,
	unsigned long tpf, unsigned long gsf, unsigned long gpf);
#endif /* CONFIG_NUMA_BALANCING */
#else
static inline void resched_latency_warn(int cpu, u64 latency) {}
#endif /* CONFIG_SCHED_DEBUG */

extern void init_cfs_rq(struct cfs_rq *cfs_rq);
extern void init_rt_rq(struct rt_rq *rt_rq);
extern void init_dl_rq(struct dl_rq *dl_rq);

extern void cfs_bandwidth_usage_inc(void);
extern void cfs_bandwidth_usage_dec(void);

#ifdef CONFIG_NO_HZ_COMMON
#define NOHZ_BALANCE_KICK_BIT	0
#define NOHZ_STATS_KICK_BIT	1
#define NOHZ_NEWILB_KICK_BIT	2

/*
 * IAMROOT, 2023.05.18:
 * - NOHZ_BALANCE_KICK
 *   kick_lib()할때 next_balance를 현재시각으로 업데이트한다.
 * - NOHZ_STATS_KICK
 *   단독으로 사용할 경우 kick_lib()를 next_balance없이 수행한다.
 */
#define NOHZ_BALANCE_KICK	BIT(NOHZ_BALANCE_KICK_BIT)
#define NOHZ_STATS_KICK		BIT(NOHZ_STATS_KICK_BIT)
/*
 * IAMROOT, 2023.03.15:
 * - new idle balance.
 *   set : nohz_newidle_balance()
 *   unset : nohz_run_idle_balance()
 */
#define NOHZ_NEWILB_KICK	BIT(NOHZ_NEWILB_KICK_BIT)

#define NOHZ_KICK_MASK	(NOHZ_BALANCE_KICK | NOHZ_STATS_KICK)

#define nohz_flags(cpu)	(&cpu_rq(cpu)->nohz_flags)

extern void nohz_balance_exit_idle(struct rq *rq);
#else
static inline void nohz_balance_exit_idle(struct rq *rq) { }
#endif

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
extern void nohz_run_idle_balance(int cpu);
#else
static inline void nohz_run_idle_balance(int cpu) { }
#endif

#ifdef CONFIG_SMP
static inline
void __dl_update(struct dl_bw *dl_b, s64 bw)
{
	struct root_domain *rd = container_of(dl_b, struct root_domain, dl_bw);
	int i;

	RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
			 "sched RCU must be held");
	for_each_cpu_and(i, rd->span, cpu_active_mask) {
		struct rq *rq = cpu_rq(i);

		rq->dl.extra_bw += bw;
	}
}
#else
static inline
void __dl_update(struct dl_bw *dl_b, s64 bw)
{
	struct dl_rq *dl = container_of(dl_b, struct dl_rq, dl_bw);

	dl->extra_bw += bw;
}
#endif


#ifdef CONFIG_IRQ_TIME_ACCOUNTING
struct irqtime {
	u64			total;
	u64			tick_delta;
	u64			irq_start_time;
	struct u64_stats_sync	sync;
};

DECLARE_PER_CPU(struct irqtime, cpu_irqtime);

/*
 * Returns the irqtime minus the softirq time computed by ksoftirqd.
 * Otherwise ksoftirqd's sum_exec_runtime is subtracted its own runtime
 * and never move forward.
 */
static inline u64 irq_time_read(int cpu)
{
	struct irqtime *irqtime = &per_cpu(cpu_irqtime, cpu);
	unsigned int seq;
	u64 total;

	do {
		seq = __u64_stats_fetch_begin(&irqtime->sync);
		total = irqtime->total;
	} while (__u64_stats_fetch_retry(&irqtime->sync, seq));

	return total;
}
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

#ifdef CONFIG_CPU_FREQ
DECLARE_PER_CPU(struct update_util_data __rcu *, cpufreq_update_util_data);

/**
 * cpufreq_update_util - Take a note about CPU utilization changes.
 * @rq: Runqueue to carry out the update for.
 * @flags: Update reason flags.
 *
 * This function is called by the scheduler on the CPU whose utilization is
 * being updated.
 *
 * It can only be called from RCU-sched read-side critical sections.
 *
 * The way cpufreq is currently arranged requires it to evaluate the CPU
 * performance state (frequency/voltage) on a regular basis to prevent it from
 * being stuck in a completely inadequate performance level for too long.
 * That is not guaranteed to happen if the updates are only triggered from CFS
 * and DL, though, because they may not be coming in if only RT tasks are
 * active all the time (or there are RT tasks only).
 *
 * As a workaround for that issue, this function is called periodically by the
 * RT sched class to trigger extra cpufreq updates to prevent it from stalling,
 * but that really is a band-aid.  Going forward it should be replaced with
 * solutions targeted more specifically at RT tasks.
 */
/*
 * IAMROOT, 2023.01.07:
 * - @rq의 변화에 따라 cpu freq를 바꿀수있는 기능이 있으면 이를 수행한다.
 */
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
	struct update_util_data *data;

	data = rcu_dereference_sched(*per_cpu_ptr(&cpufreq_update_util_data,
						  cpu_of(rq)));
	if (data)
		data->func(data, rq_clock(rq), flags);
}
#else
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags) {}
#endif /* CONFIG_CPU_FREQ */

#ifdef CONFIG_UCLAMP_TASK
unsigned long uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id);

/**
 * uclamp_rq_util_with - clamp @util with @rq and @p effective uclamp values.
 * @rq:		The rq to clamp against. Must not be NULL.
 * @util:	The util value to clamp.
 * @p:		The task to clamp against. Can be NULL if you want to clamp
 *		against @rq only.
 *
 * Clamps the passed @util to the max(@rq, @p) effective uclamp values.
 *
 * If sched_uclamp_used static key is disabled, then just return the util
 * without any clamping since uclamp aggregation at the rq level in the fast
 * path is disabled, rendering this operation a NOP.
 *
 * Use uclamp_eff_value() if you don't care about uclamp values at rq level. It
 * will return the correct effective uclamp value of the task even if the
 * static key is disabled.
 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   uclamp_rq_util_with - @rq 및 @p 유효 uclamp 값을 사용하여 @util을 클램프합니다.
 *   @rq: 클램프할 rq입니다. NULL이 아니어야 합니다.
 *   @util: 클램프할 util 값입니다.
 *   @p: 클램프할 작업입니다. @rq만 고정하려는 경우 NULL이 될 수 있습니다.
 *
 *   전달된 @util을 최대(@rq, @p) 유효 uclamp 값으로 고정합니다.
 *
 *   sched_uclamp_used 정적 키가 비활성화된 경우 빠른 경로의 rq 수준에서 uclamp 
 *   집계가 비활성화되어 이 작업을 NOP로 렌더링하므로 클램핑 없이 util을 반환합니다.
 *
 *   rq 수준에서 uclamp 값을 신경 쓰지 않는다면 uclamp_eff_value()를 사용하십시오. 
 *   정적 키가 비활성화된 경우에도 작업의 올바른 유효 uclamp 값을 반환합니다.
 *
 * - 1. 1차적으로 UCLAMP_FLAG_IDLE이 있는 경우 @p의 min max를 사용한다.
 *   2. 그게 아니면 @p와 @rq에 대한 min max를 비교해 최대값을 사용한다.
 */
static __always_inline
unsigned long uclamp_rq_util_with(struct rq *rq, unsigned long util,
				  struct task_struct *p)
{
	unsigned long min_util = 0;
	unsigned long max_util = 0;

	if (!static_branch_likely(&sched_uclamp_used))
		return util;

	if (p) {
		min_util = uclamp_eff_value(p, UCLAMP_MIN);
		max_util = uclamp_eff_value(p, UCLAMP_MAX);

		/*
		 * Ignore last runnable task's max clamp, as this task will
		 * reset it. Similarly, no need to read the rq's min clamp.
		 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   마지막으로 실행 가능한 작업의 최대 클램프를 무시합니다. 이 작업은 
 *   이를 재설정합니다. 마찬가지로 rq의 최소 클램프를 읽을 필요가 없습니다.
 */
		if (rq->uclamp_flags & UCLAMP_FLAG_IDLE)
			goto out;
	}

	min_util = max_t(unsigned long, min_util, READ_ONCE(rq->uclamp[UCLAMP_MIN].value));
	max_util = max_t(unsigned long, max_util, READ_ONCE(rq->uclamp[UCLAMP_MAX].value));
out:
	/*
	 * Since CPU's {min,max}_util clamps are MAX aggregated considering
	 * RUNNABLE tasks with _different_ clamps, we can end up with an
	 * inversion. Fix it now when the clamps are applied.
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   CPU의 {min,max}_util 클램프는 _다른_ 클램프가 있는 RUNNABLE 작업을 고려하여 MAX 
 *   집계되므로 반전으로 끝날 수 있습니다. 클램프가 적용되면 지금 수정하십시오.
 */
	if (unlikely(min_util >= max_util))
		return min_util;

	return clamp(util, min_util, max_util);
}

/*
 * When uclamp is compiled in, the aggregation at rq level is 'turned off'
 * by default in the fast path and only gets turned on once userspace performs
 * an operation that requires it.
 *
 * Returns true if userspace opted-in to use uclamp and aggregation at rq level
 * hence is active.
 */
static inline bool uclamp_is_used(void)
{
	return static_branch_likely(&sched_uclamp_used);
}
#else /* CONFIG_UCLAMP_TASK */
static inline
unsigned long uclamp_rq_util_with(struct rq *rq, unsigned long util,
				  struct task_struct *p)
{
	return util;
}

static inline bool uclamp_is_used(void)
{
	return false;
}
#endif /* CONFIG_UCLAMP_TASK */

#ifdef arch_scale_freq_capacity
# ifndef arch_scale_freq_invariant
#  define arch_scale_freq_invariant()	true
# endif
#else
# define arch_scale_freq_invariant()	false
#endif

#ifdef CONFIG_SMP
static inline unsigned long capacity_orig_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity_orig;
}

/**
 * enum cpu_util_type - CPU utilization type
 * @FREQUENCY_UTIL:	Utilization used to select frequency
 * @ENERGY_UTIL:	Utilization used during energy calculation
 *
 * The utilization signals of all scheduling classes (CFS/RT/DL) and IRQ time
 * need to be aggregated differently depending on the usage made of them. This
 * enum is used within effective_cpu_util() to differentiate the types of
 * utilization expected by the callers, and adjust the aggregation accordingly.
 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   enum cpu_util_type - CPU 사용 유형
 *   @FREQUENCY_UTIL: 주파수 선택에 사용되는 utilization
 *   @ENERGY_UTIL: 에너지 계산 시 사용되는 utilization
 *
 *   모든 스케줄링 클래스(CFS/RT/DL)의 활용 신호와 IRQ 시간은 utilization에 따라 다르게 
 *   집계해야 합니다. 이 열거형은 effective_cpu_util() 내에서 호출자가 예상하는 
 *   사용 유형을 구별하고 그에 따라 집계를 조정하는 데 사용됩니다.
 *
 * - FREQUENCY_UTIL : 주파수 설정목적
 *   ENERGY_UTIL : 에너지 산출목적
 */
enum cpu_util_type {
	FREQUENCY_UTIL,
	ENERGY_UTIL,
};

unsigned long effective_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long max, enum cpu_util_type type,
				 struct task_struct *p);
/*
 * IAMROOT, 2023.06.03:
 * - return dl bw 
 */
static inline unsigned long cpu_bw_dl(struct rq *rq)
{
	return (rq->dl.running_bw * SCHED_CAPACITY_SCALE) >> BW_SHIFT;
}

/*
 * IAMROOT, 2023.06.03:
 * - return dl util
 */
static inline unsigned long cpu_util_dl(struct rq *rq)
{
	return READ_ONCE(rq->avg_dl.util_avg);
}

/*
 * IAMROOT, 2023.06.03:
 * - return max(cfs util_avg, cfs util_est)
 */
static inline unsigned long cpu_util_cfs(struct rq *rq)
{
	unsigned long util = READ_ONCE(rq->cfs.avg.util_avg);

	if (sched_feat(UTIL_EST)) {
		util = max_t(unsigned long, util,
			     READ_ONCE(rq->cfs.avg.util_est.enqueued));
	}

	return util;
}

/*
 * IAMROOT, 2023.06.03:
 * - return rt util_avg
 */
static inline unsigned long cpu_util_rt(struct rq *rq)
{
	return READ_ONCE(rq->avg_rt.util_avg);
}
#endif

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	return rq->avg_irq.util_avg;
}

/*
 * IAMROOT, 2023.04.22:
 * - @util : 남은 capacity(max - dl - rt)
 *   @irq  : irq에서 사용한 성능
 *   @max  : 최대 성능
 *
 * - max값에서 irq position을 제외한 비율을 util에 적용한다.
 *   ex) util이 500이라고 할때, irq에 의해 cpu가 10% 소모됫으면 util도 10%낮춰서
 *       450으로 계산한다.
 *
 *  util * (max - irq)
 *   -------------
 *   max 
 *
 * ex) rt = 10, dl = 20, termal = 30, irq = 40, max = 1024
 *     util = 1024 - 10 - 20 - 30 = 964
 *
 *     (964 * (1024 - 40)) / 1024 = 926
 *
 * - @max 에서 @irq 외 의 비율을 util 에 곱한값을 max로 나누어 반환
 */
static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	util *= (max - irq);
	util /= max;

	return util;

}
#else
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	return 0;
}

static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	return util;
}
#endif

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)

#define perf_domain_span(pd) (to_cpumask(((pd)->em_pd->cpus)))

DECLARE_STATIC_KEY_FALSE(sched_energy_present);

/*
 * IAMROOT, 2023.05.20:
 * - mobile cpu 에서 주로 이용. EAS(energy aware scheduling) 활성화 여부
 */
static inline bool sched_energy_enabled(void)
{
	return static_branch_unlikely(&sched_energy_present);
}

#else /* ! (CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL) */

#define perf_domain_span(pd) NULL
static inline bool sched_energy_enabled(void) { return false; }

#endif /* CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL */

#ifdef CONFIG_MEMBARRIER
/*
 * The scheduler provides memory barriers required by membarrier between:
 * - prior user-space memory accesses and store to rq->membarrier_state,
 * - store to rq->membarrier_state and following user-space memory accesses.
 * In the same way it provides those guarantees around store to rq->curr.
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   - 스케줄러는 다음 사이에 membarrier에 필요한 메모리 배리어를 제공합니다.
 *   - 이전 사용자 공간 메모리 액세스 및 rq->membarrier_state 저장,
 *   - rq->membarrier_state에 저장하고 사용자 공간 메모리 액세스를 따릅니다.
 *   같은 방식으로 저장소 주변에서 rq->curr에 대한 보증을 제공합니다.
 *
 * - user공간에서는 mm간의 간섭을 막기위해 membarrier 처리를한다.
 */
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
	int membarrier_state;

/*
 * IAMROOT, 2023.01.28:
 * - mm이 안바꼇으면 switch할필요없다.
 */
	if (prev_mm == next_mm)
		return;

/*
 * IAMROOT, 2023.01.28:
 * - rq의 membarrier_state를 next로 갱신한다.
 */
	membarrier_state = atomic_read(&next_mm->membarrier_state);
	if (READ_ONCE(rq->membarrier_state) == membarrier_state)
		return;

	WRITE_ONCE(rq->membarrier_state, membarrier_state);
}
#else
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
}
#endif

#ifdef CONFIG_SMP
static inline bool is_per_cpu_kthread(struct task_struct *p)
{
	if (!(p->flags & PF_KTHREAD))
		return false;

	if (p->nr_cpus_allowed != 1)
		return false;

	return true;
}
#endif

extern void swake_up_all_locked(struct swait_queue_head *q);
extern void __prepare_to_swait(struct swait_queue_head *q, struct swait_queue *wait);

#ifdef CONFIG_PREEMPT_DYNAMIC
extern int preempt_dynamic_mode;
extern int sched_dynamic_mode(const char *str);
extern void sched_dynamic_update(int mode);
#endif

