/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

/*
 * Define 'struct task_struct' and provide the main scheduler
 * APIs (schedule(), wakeup variants, etc.)
 */

#include <uapi/linux/sched.h>

#include <asm/current.h>

#include <linux/pid.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/mutex.h>
#include <linux/plist.h>
#include <linux/hrtimer.h>
#include <linux/irqflags.h>
#include <linux/seccomp.h>
#include <linux/nodemask.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/resource.h>
#include <linux/latencytop.h>
#include <linux/sched/prio.h>
#include <linux/sched/types.h>
#include <linux/signal_types.h>
#include <linux/syscall_user_dispatch.h>
#include <linux/mm_types_task.h>
#include <linux/task_io_accounting.h>
#include <linux/posix-timers.h>
#include <linux/rseq.h>
#include <linux/seqlock.h>
#include <linux/kcsan.h>
#include <asm/kmap_size.h>

/* task_struct member predeclarations (sorted alphabetically): */
struct audit_context;
struct backing_dev_info;
struct bio_list;
struct blk_plug;
struct bpf_local_storage;
struct bpf_run_ctx;
struct capture_control;
struct cfs_rq;
struct fs_struct;
struct futex_pi_state;
struct io_context;
struct io_uring_task;
struct mempolicy;
struct nameidata;
struct nsproxy;
struct perf_event_context;
struct pid_namespace;
struct pipe_inode_info;
struct rcu_node;
struct reclaim_state;
struct robust_list_head;
struct root_domain;
struct rq;
struct sched_attr;
struct sched_param;
struct seq_file;
struct sighand_struct;
struct signal_struct;
struct task_delay_info;
struct task_group;

/*
 * Task state bitmask. NOTE! These bits are also
 * encoded in fs/proc/array.c: get_task_state().
 *
 * We have two separate sets of flags: task->state
 * is about runnability, while task->exit_state are
 * about the task exiting. Confusing, but this way
 * modifying one set can't modify the other one by
 * mistake.
 */

/* Used in tsk->state: */
#define TASK_RUNNING			0x0000
#define TASK_INTERRUPTIBLE		0x0001
#define TASK_UNINTERRUPTIBLE		0x0002
#define __TASK_STOPPED			0x0004
#define __TASK_TRACED			0x0008
/* Used in tsk->exit_state: */
#define EXIT_DEAD			0x0010
#define EXIT_ZOMBIE			0x0020
#define EXIT_TRACE			(EXIT_ZOMBIE | EXIT_DEAD)
/* Used in tsk->state again: */

/*
 * IAMROOT, 2022.11.19:
 * - TASK_PARKED
 *   kthread에만 있는 개념.
 *   wakeup업시 unpark할때까지 잠깐 멈추는 기능.
 *
 * - TASK_NOLOAD
 *   load 계산에서 task를 제외한다.
 */
#define TASK_PARKED			0x0040
#define TASK_DEAD			0x0080
#define TASK_WAKEKILL			0x0100
#define TASK_WAKING			0x0200
#define TASK_NOLOAD			0x0400
#define TASK_NEW			0x0800
/* RT specific auxilliary flag to mark RT lock waiters */
#define TASK_RTLOCK_WAIT		0x1000
#define TASK_STATE_MAX			0x2000

/* Convenience macros for the sake of set_current_state: */
#define TASK_KILLABLE			(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_STOPPED			(TASK_WAKEKILL | __TASK_STOPPED)
#define TASK_TRACED			(TASK_WAKEKILL | __TASK_TRACED)

#define TASK_IDLE			(TASK_UNINTERRUPTIBLE | TASK_NOLOAD)

/* Convenience macros for the sake of wake_up(): */
#define TASK_NORMAL			(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

/* get_task_state(): */
#define TASK_REPORT			(TASK_RUNNING | TASK_INTERRUPTIBLE | \
					 TASK_UNINTERRUPTIBLE | __TASK_STOPPED | \
					 __TASK_TRACED | EXIT_DEAD | EXIT_ZOMBIE | \
					 TASK_PARKED)

#define task_is_running(task)		(READ_ONCE((task)->__state) == TASK_RUNNING)

#define task_is_traced(task)		((READ_ONCE(task->__state) & __TASK_TRACED) != 0)

#define task_is_stopped(task)		((READ_ONCE(task->__state) & __TASK_STOPPED) != 0)

#define task_is_stopped_or_traced(task)	((READ_ONCE(task->__state) & (__TASK_STOPPED | __TASK_TRACED)) != 0)

/*
 * Special states are those that do not use the normal wait-loop pattern. See
 * the comment with set_special_state().
 */
#define is_special_task_state(state)				\
	((state) & (__TASK_STOPPED | __TASK_TRACED | TASK_PARKED | TASK_DEAD))

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
# define debug_normal_state_change(state_value)				\
	do {								\
		WARN_ON_ONCE(is_special_task_state(state_value));	\
		current->task_state_change = _THIS_IP_;			\
	} while (0)

# define debug_special_state_change(state_value)			\
	do {								\
		WARN_ON_ONCE(!is_special_task_state(state_value));	\
		current->task_state_change = _THIS_IP_;			\
	} while (0)

# define debug_rtlock_wait_set_state()					\
	do {								 \
		current->saved_state_change = current->task_state_change;\
		current->task_state_change = _THIS_IP_;			 \
	} while (0)

# define debug_rtlock_wait_restore_state()				\
	do {								 \
		current->task_state_change = current->saved_state_change;\
	} while (0)

#else
# define debug_normal_state_change(cond)	do { } while (0)
# define debug_special_state_change(cond)	do { } while (0)
# define debug_rtlock_wait_set_state()		do { } while (0)
# define debug_rtlock_wait_restore_state()	do { } while (0)
#endif

/*
 * set_current_state() includes a barrier so that the write of current->state
 * is correctly serialised wrt the caller's subsequent test of whether to
 * actually sleep:
 *
 *   for (;;) {
 *	set_current_state(TASK_UNINTERRUPTIBLE);
 *	if (CONDITION)
 *	   break;
 *
 *	schedule();
 *   }
 *   __set_current_state(TASK_RUNNING);
 *
 * If the caller does not need such serialisation (because, for instance, the
 * CONDITION test and condition change and wakeup are under the same lock) then
 * use __set_current_state().
 *
 * The above is typically ordered against the wakeup, which does:
 *
 *   CONDITION = 1;
 *   wake_up_state(p, TASK_UNINTERRUPTIBLE);
 *
 * where wake_up_state()/try_to_wake_up() executes a full memory barrier before
 * accessing p->state.
 *
 * Wakeup will do: if (@state & p->state) p->state = TASK_RUNNING, that is,
 * once it observes the TASK_UNINTERRUPTIBLE store the waking CPU can issue a
 * TASK_RUNNING store which can collide with __set_current_state(TASK_RUNNING).
 *
 * However, with slightly different timing the wakeup TASK_RUNNING store can
 * also collide with the TASK_UNINTERRUPTIBLE store. Losing that store is not
 * a problem either because that will result in one extra go around the loop
 * and our @cond test will save the day.
 *
 * Also see the comments of try_to_wake_up().
 */
#define __set_current_state(state_value)				\
	do {								\
		debug_normal_state_change((state_value));		\
		WRITE_ONCE(current->__state, (state_value));		\
	} while (0)

#define set_current_state(state_value)					\
	do {								\
		debug_normal_state_change((state_value));		\
		smp_store_mb(current->__state, (state_value));		\
	} while (0)

/*
 * set_special_state() should be used for those states when the blocking task
 * can not use the regular condition based wait-loop. In that case we must
 * serialize against wakeups such that any possible in-flight TASK_RUNNING
 * stores will not collide with our state change.
 */
#define set_special_state(state_value)					\
	do {								\
		unsigned long flags; /* may shadow */			\
									\
		raw_spin_lock_irqsave(&current->pi_lock, flags);	\
		debug_special_state_change((state_value));		\
		WRITE_ONCE(current->__state, (state_value));		\
		raw_spin_unlock_irqrestore(&current->pi_lock, flags);	\
	} while (0)

/*
 * PREEMPT_RT specific variants for "sleeping" spin/rwlocks
 *
 * RT's spin/rwlock substitutions are state preserving. The state of the
 * task when blocking on the lock is saved in task_struct::saved_state and
 * restored after the lock has been acquired.  These operations are
 * serialized by task_struct::pi_lock against try_to_wake_up(). Any non RT
 * lock related wakeups while the task is blocked on the lock are
 * redirected to operate on task_struct::saved_state to ensure that these
 * are not dropped. On restore task_struct::saved_state is set to
 * TASK_RUNNING so any wakeup attempt redirected to saved_state will fail.
 *
 * The lock operation looks like this:
 *
 *	current_save_and_set_rtlock_wait_state();
 *	for (;;) {
 *		if (try_lock())
 *			break;
 *		raw_spin_unlock_irq(&lock->wait_lock);
 *		schedule_rtlock();
 *		raw_spin_lock_irq(&lock->wait_lock);
 *		set_current_state(TASK_RTLOCK_WAIT);
 *	}
 *	current_restore_rtlock_saved_state();
 */
#define current_save_and_set_rtlock_wait_state()			\
	do {								\
		lockdep_assert_irqs_disabled();				\
		raw_spin_lock(&current->pi_lock);			\
		current->saved_state = current->__state;		\
		debug_rtlock_wait_set_state();				\
		WRITE_ONCE(current->__state, TASK_RTLOCK_WAIT);		\
		raw_spin_unlock(&current->pi_lock);			\
	} while (0);

#define current_restore_rtlock_saved_state()				\
	do {								\
		lockdep_assert_irqs_disabled();				\
		raw_spin_lock(&current->pi_lock);			\
		debug_rtlock_wait_restore_state();			\
		WRITE_ONCE(current->__state, current->saved_state);	\
		current->saved_state = TASK_RUNNING;			\
		raw_spin_unlock(&current->pi_lock);			\
	} while (0);

#define get_current_state()	READ_ONCE(current->__state)

/* Task command name length: */
#define TASK_COMM_LEN			16

extern void scheduler_tick(void);

#define	MAX_SCHEDULE_TIMEOUT		LONG_MAX

extern long schedule_timeout(long timeout);
extern long schedule_timeout_interruptible(long timeout);
extern long schedule_timeout_killable(long timeout);
extern long schedule_timeout_uninterruptible(long timeout);
extern long schedule_timeout_idle(long timeout);
asmlinkage void schedule(void);
extern void schedule_preempt_disabled(void);
asmlinkage void preempt_schedule_irq(void);
#ifdef CONFIG_PREEMPT_RT
 extern void schedule_rtlock(void);
#endif

extern int __must_check io_schedule_prepare(void);
extern void io_schedule_finish(int token);
extern long io_schedule_timeout(long timeout);
extern void io_schedule(void);

/**
 * struct prev_cputime - snapshot of system and user cputime
 * @utime: time spent in user mode
 * @stime: time spent in system mode
 * @lock: protects the above two fields
 *
 * Stores previous user/system time values such that we can guarantee
 * monotonicity.
 */
struct prev_cputime {
#ifndef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE
	u64				utime;
	u64				stime;
	raw_spinlock_t			lock;
#endif
};

enum vtime_state {
	/* Task is sleeping or running in a CPU with VTIME inactive: */
	VTIME_INACTIVE = 0,
	/* Task is idle */
	VTIME_IDLE,
	/* Task runs in kernelspace in a CPU with VTIME active: */
	VTIME_SYS,
	/* Task runs in userspace in a CPU with VTIME active: */
	VTIME_USER,
	/* Task runs as guests in a CPU with VTIME active: */
	VTIME_GUEST,
};

struct vtime {
	seqcount_t		seqcount;
	unsigned long long	starttime;
	enum vtime_state	state;
	unsigned int		cpu;
	u64			utime;
	u64			stime;
	u64			gtime;
};

/*
 * Utilization clamp constraints.
 * @UCLAMP_MIN:	Minimum utilization
 * @UCLAMP_MAX:	Maximum utilization
 * @UCLAMP_CNT:	Utilization clamp constraints count
 */
enum uclamp_id {
	UCLAMP_MIN = 0,
	UCLAMP_MAX,
	UCLAMP_CNT
};

#ifdef CONFIG_SMP
extern struct root_domain def_root_domain;
extern struct mutex sched_domains_mutex;
#endif

/*
 * IAMROOT, 2023.01.28:
 * - sched_info_arrive(), sched_info_depart() 참고
 */
struct sched_info {
#ifdef CONFIG_SCHED_INFO
	/* Cumulative counters: */

/*
 * IAMROOT, 2023.01.28:
 * - pcount    : cpu에 올라간 횟수.
 *   run_delay : 대기queue에서 기다렸던 시간총합.
 *   last_arrival : cpu에 마지막에 올라간 시각
 *   last_queued  : cpu에서 마지막에 제거된 시각.
 */
	/* # of times we have run on this CPU: */
	unsigned long			pcount;

	/* Time spent waiting on a runqueue: */
	unsigned long long		run_delay;

	/* Timestamps: */

	/* When did we last run on a CPU? */
	unsigned long long		last_arrival;

	/* When were we last queued to run? */
	unsigned long long		last_queued;

#endif /* CONFIG_SCHED_INFO */
};

/*
 * Integer metrics need fixed point arithmetic, e.g., sched/fair
 * has a few: load, load_avg, util_avg, freq, and capacity.
 *
 * We define a basic fixed point arithmetic range, and then formalize
 * all these metrics based on that basic range.
 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *   정수 메트릭에는 고정 소수점 산술이 필요합니다. 예를 들어
 *   sched/fair에는 몇 가지가 있습니다.
 *   load, load_avg, util_avg, freq 및 용량.
 *
 *   기본 고정 소수점 산술 범위를 정의한 다음 해당 기본 범위를
 *   기반으로 이러한 모든 메트릭을 형식화합니다.
 */
# define SCHED_FIXEDPOINT_SHIFT		10
# define SCHED_FIXEDPOINT_SCALE		(1L << SCHED_FIXEDPOINT_SHIFT)

/* Increase resolution of cpu_capacity calculations */
# define SCHED_CAPACITY_SHIFT		SCHED_FIXEDPOINT_SHIFT
# define SCHED_CAPACITY_SCALE		(1L << SCHED_CAPACITY_SHIFT)

/*
 * IAMROOT, 2022.12.21:
 * - nice값에 따른 weight, inv_weight 참고
 *   sched_prio_to_weight, sched_prio_to_wmult
 * - inv_weight : WMULT_SHIFT로 1 / weight를 이진화정수한 값.
 * - x / weight를 실제 계산시 (x * inv_weight) >> WMULT_SHIFT로 수행한다.
 */
struct load_weight {
	unsigned long			weight;
	u32				inv_weight;
};

/**
 * struct util_est - Estimation utilization of FAIR tasks
 * @enqueued: instantaneous estimated utilization of a task/cpu
 * @ewma:     the Exponential Weighted Moving Average (EWMA)
 *            utilization of a task
 *
 * Support data structure to track an Exponential Weighted Moving Average
 * (EWMA) of a FAIR task's utilization. New samples are added to the moving
 * average each time a task completes an activation. Sample's weight is chosen
 * so that the EWMA will be relatively insensitive to transient changes to the
 * task's workload.
 *
 * The enqueued attribute has a slightly different meaning for tasks and cpus:
 * - task:   the task's util_avg at last task dequeue time
 * - cfs_rq: the sum of util_est.enqueued for each RUNNABLE task on that CPU
 * Thus, the util_est.enqueued of a task represents the contribution on the
 * estimated utilization of the CPU where that task is currently enqueued.
 *
 * Only for tasks we track a moving average of the past instantaneous
 * estimated utilization. This allows to absorb sporadic drops in utilization
 * of an otherwise almost periodic task.
 *
 * The UTIL_AVG_UNCHANGED flag is used to synchronize util_est with util_avg
 * updates. When a task is dequeued, its util_est should not be updated if its
 * util_avg has not been updated in the meantime.
 * This information is mapped into the MSB bit of util_est.enqueued at dequeue
 * time. Since max value of util_est.enqueued for a task is 1024 (PELT util_avg
 * for a task) it is safe to use MSB.
 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *  struct util_est - FAIR 작업의 추정 활용
 *  @enqueued: 작업/CPU의 즉각적인 예상 사용률
 *  @ewma: 작업의 지수 가중 이동 평균(EWMA) 사용률입니다.
 *
 *  FAIR 작업 활용도의 지수 가중 이동 평균(EWMA)을 추적하는 데이터 구조를 지원합니다.
 *  작업이 활성화를 완료할 때마다 새 샘플이 이동 평균에 추가됩니다. 샘플의 가중치는 
 *  EWMA가 작업 부하의 일시적인 변화에 상대적으로 덜 민감하도록 선택됩니다. 
 *
 *  대기열에 추가된 속성은 작업 및 cpus에 대해 약간 다른 의미를 갖습니다.
 *  - task: 마지막 작업 대기열에서 빼기 시간에 작업의 util_avg.
 *  - cfs_rq: 해당 CPU의 각 RUNNABLE 작업에 대한 util_est.enqueued의 합계 따라서 작업의 
 *  util_est.enqueued는 해당 작업이 현재 대기열에 있는 CPU의 예상 사용률에 대한 기여도를 
 *  나타냅니다. 
 *
 *  작업에 대해서만 과거 순간 예상 사용률의 이동 평균을 추적합니다. 이를 통해 거의 
 *  주기적인 작업을 사용할 때 산발적인 감소를 흡수할 수 있습니다. 
 *
 *  UTIL_AVG_UNCHANGED 플래그는 util_est를 util_avg 업데이트와 동기화하는 데 사용됩니다. 
 *  작업이 대기열에서 제거되면 그 동안 util_avg가 업데이트되지 않은 경우 해당 util_est를 
 *  업데이트하면 안 됩니다.
 *
 *  이 정보는 큐에서 제거할 때 util_est.enqueued의 MSB 비트에 매핑됩니다. 작업에 대한 
 *  util_est.enqueued의 최대 값은 1024(작업에 대한 PELT util_avg)이므로 MSB를 사용하는 
 *  것이 안전합니다.
 *
 * - enqueued의 MSB는 UTIL_AVG_UNCHANGED로 사용될수있다.
 */
struct util_est {
	unsigned int			enqueued;
	unsigned int			ewma;
#define UTIL_EST_WEIGHT_SHIFT		2
#define UTIL_AVG_UNCHANGED		0x80000000
} __attribute__((__aligned__(sizeof(u64))));

/*
 * The load/runnable/util_avg accumulates an infinite geometric series
 * (see __update_load_avg_cfs_rq() in kernel/sched/pelt.c).
 *
 * [load_avg definition]
 *
 *   load_avg = runnable% * scale_load_down(load)
 *
 * [runnable_avg definition]
 *
 *   runnable_avg = runnable% * SCHED_CAPACITY_SCALE
 *
 * [util_avg definition]
 *
 *   util_avg = running% * SCHED_CAPACITY_SCALE
 *
 * where runnable% is the time ratio that a sched_entity is runnable and
 * running% the time ratio that a sched_entity is running.
 *
 * For cfs_rq, they are the aggregated values of all runnable and blocked
 * sched_entities.
 *
 * The load/runnable/util_avg doesn't directly factor frequency scaling and CPU
 * capacity scaling. The scaling is done through the rq_clock_pelt that is used
 * for computing those signals (see update_rq_clock_pelt())
 *
 * N.B., the above ratios (runnable% and running%) themselves are in the
 * range of [0, 1]. To do fixed point arithmetics, we therefore scale them
 * to as large a range as necessary. This is for example reflected by
 * util_avg's SCHED_CAPACITY_SCALE.
 *
 * [Overflow issue]
 *
 * The 64-bit load_sum can have 4353082796 (=2^64/47742/88761) entities
 * with the highest load (=88761), always runnable on a single cfs_rq,
 * and should not overflow as the number already hits PID_MAX_LIMIT.
 *
 * For all other cases (including 32-bit kernels), struct load_weight's
 * weight will overflow first before we do, because:
 *
 *    Max(load_avg) <= Max(load.weight)
 *
 * Then it is the load_weight's responsibility to consider overflow
 * issues.
 */
/*
 * IAMROOT. 2022.12.17:
 * - google-translate
 *   load/runnable/util_avg는 무한 기하학적 계열을 누적합니다
 *   (kernel/sched/pelt.c의 __update_load_avg_cfs_rq() 참조).
 *
 *   [load_avg 정의]
 *
 *     load_avg = runnable% * scale_load_down(load)
 *
 *   [runnable_avg 정의]
 *
 *     runnable_avg = runnable% * SCHED_CAPACITY_SCALE
 *
 *   [util_avg 정의]
 *
 *     util_avg = running% * SCHED_CAPACITY_SCALE
 *
 *   여기서 runnable%는 sched_entity가 실행 가능하고 실행 중인 시간의
 *   비율입니다. sched_entity가 실행 중인 비율입니다.
 *
 *   cfs_rq의 경우 실행 가능하고
 *   차단된 모든 sched_entities의 집계된 값입니다.
 *
 *   load/runnable/util_avg는 주파수
 *   조정 및 CPU 용량 조정을 직접 고려하지 않습니다. 스케일링은 이러한 신호를
 *   계산하는 데 사용되는 rq_clock_pelt를 통해 수행됩니다
 *   (update_rq_clock_pelt() 참조).
 *
 *   고정 소수점 산술을 수행하기 위해 필요에 따라 큰 범위로 크기를
 *   조정합니다. 예를 들어 이것은 util_avg의 SCHED_CAPACITY_SCALE에
 *   반영됩니다.
 *
 *   [오버플로 문제]
 *
 *   64비트 load_sum은 가장 높은 로드(=88761)를 가진
 *   4353082796(=2^64/47742/88761) 엔티티를 가질 수 있으며 항상 단일 cfs_rq에서 실행
 *   가능하며 숫자가 이미 PID_MAX_LIMIT에 도달하므로 오버플로되어서는 안 됩니다.
 *
 *   다른 모든 경우(32비트 커널 포함)의 경우 다음과 같은 이유로 struct load_weight의
 *   가중치가 우리보다 먼저 오버플로됩니다.
 *
 *   Max(load_avg) <= Max(load.weight)
 *
 *   그런 다음 오버플로 문제를 고려하는 것은 load_weight의 책임입니다.
 */
struct sched_avg {
/*
 * IAMROOT, 2022.12.31:
 * - ns단위 갱신될때 1024 미만 단위는 버려진다.(___update_load_sum()) 참고
 */
	u64				last_update_time;
	u64				load_sum;
	u64				runnable_sum;
	u32				util_sum;

/*
 * IAMROOT, 2022.12.31:
 * - accumulate_sum()에서, 직전 주기에서 1ms가 못되서 남겨둔 나머지 시간.
 */
	u32				period_contrib;
	unsigned long			load_avg;
	unsigned long			runnable_avg;
	unsigned long			util_avg;
/*
 * IAMROOT, 2023.01.01: 
 * - util_est가 새롭게 추가되었다.
 *   예를 들어 cpu 점유율(util_avg)이 큰 task가 있다고 가정한다. 
 *   이 task가 long 슬립한 후 깨어나면, 그 long 슬립한 기간만큼 util_avg가
 *   decay하여 아주 작은 값이 된다. 그러면 스케줄러가 이의 cpu 점유율이 
 *   작다고 판단하여 cpu freq가 낮게 시작할 수 있다. 이를 방지하기 위해
 *   슬립을 위해 dequeue 되는 순간의 util_avg 값과, 새롭게 산출된 util_avg
 *   값을 비교하여 max 값을 사용하기 위해 적용되었다.
 */
	struct util_est			util_est;
} ____cacheline_aligned;

struct sched_statistics {
#ifdef CONFIG_SCHEDSTATS
	u64				wait_start;
	u64				wait_max;
	u64				wait_count;
	u64				wait_sum;
	u64				iowait_count;
	u64				iowait_sum;

	u64				sleep_start;
	u64				sleep_max;
	s64				sum_sleep_runtime;

	u64				block_start;
	u64				block_max;
	u64				exec_max;
	u64				slice_max;

	u64				nr_migrations_cold;
	u64				nr_failed_migrations_affine;
	u64				nr_failed_migrations_running;
	u64				nr_failed_migrations_hot;
	u64				nr_forced_migrations;

	u64				nr_wakeups;
	u64				nr_wakeups_sync;
	u64				nr_wakeups_migrate;
	u64				nr_wakeups_local;
	u64				nr_wakeups_remote;
	u64				nr_wakeups_affine;
	u64				nr_wakeups_affine_attempts;
	u64				nr_wakeups_passive;
	u64				nr_wakeups_idle;
#endif
};

/*
 * IAMROOT, 2022.12.19:
 * --- chat open ai 
 *  - scheduling entity
 *  A scheduling entity (struct sched_entity) is a data structure that 
 *  represents a task or a group of tasks in the Linux kernel's scheduler. 
 *  A scheduling entity can represent a single task, or it can represent a 
 *  group of tasks that are organized into a hierarchy.
 *
 * 스케줄링 엔터티(struct sched_entity)는 Linux 커널의 스케줄러에서 task 
 * 또는 task group 을 나타내는 데이터 구조입니다. 일정 엔터티는 단일 task을 
 * 나타내거나 계층 구조로 구성된 task group을 나타낼 수 있습니다. 
 *
 * ----------------------------------------------------------------------
 *
 * - se 구성에 대해선 for_each_sched_entity 주석 참고
 */
struct sched_entity {
	/* For load-balancing: */
/*
 * IAMROOT, 2023.01.07:
 * - tg shares * load 기여율의 비율에따른 shares값이 load에 set된다.
 *   (update_cfs_group(), calc_group_shares(), reweight_entity()참고)
 *   단, cfs_rq에 들어가는 load weight은 해당 entity들의 weight 총합이 된다.
 */
	struct load_weight		load;
	struct rb_node			run_node;
	struct list_head		group_node;
	/*
	 * IAMROOT, 2022.12.17:
	 * - sched_entity 가 rq에 들어가 있는 경우 1. 시간할당을 받는 상태
   *   그게 아니면 sleep인 상태.
	 */
	unsigned int			on_rq;

	/*
	 * IAMROOT, 2022.12.17:
	 * - update_curr 에서 시간을 갱신(ns)
	 */
	u64				exec_start;
/*
 * IAMROOT, 2023.06.17:
 * - 실행누적시간
 */
	u64				sum_exec_runtime;
/*
 * IAMROOT, 2022.12.21:
 * - cfs의 경우 update_curr에서 갱신된다.
 *   nice-0 대비 load.weight의 비율이 적용된 delta값이 누적된다.
 *   weight클수록 적게 누적된다.
 */
	u64				vruntime;
	u64				prev_sum_exec_runtime;

	u64				nr_migrations;

	struct sched_statistics		statistics;

#ifdef CONFIG_FAIR_GROUP_SCHED
	int				depth;
	struct sched_entity		*parent;
/*
 * IAMROOT, 2022.12.21:
 * - cfs_rq
 *   소속되잇는 cfs_rq
 * - my_q
 *   자신의 cfs_rq
 *
 *
 * -ex) A에 소속되있는 A1에 대한e의 구조. A1의 A의 cfs_rq에 들어가있느 상태
 *
 *    +--------+
 *  A | cfs_rq |
 * se | A1:se  |
 *    +---|----+
 *        |
 *      (my_q)
 *        |
 *    +--------+
 *  A1| A1의   |
 *  se| task들 |
 *    +--------+
 *
 *    A1의 cfs_rq는 A의 cfs_rq를 가리킨다. A1은 A의 cfs_rq에 들어가있다.
 *    A1의 my_q에는 자신의 cfs_rq를 가리킨다.
 */
	/* rq on which this entity is (to be) queued: */
	struct cfs_rq			*cfs_rq;
	/* rq "owned" by this entity/group: */
	struct cfs_rq			*my_q;
	/* cached value of my_q->h_nr_running */
	unsigned long			runnable_weight;
#endif

#ifdef CONFIG_SMP
	/*
	 * Per entity load average tracking.
	 *
	 * Put into separate cache line so it does not
	 * collide with read-mostly values above.
	 */
	struct sched_avg		avg;
#endif
};

struct sched_rt_entity {
	struct list_head		run_list;
	unsigned long			timeout;
	unsigned long			watchdog_stamp;
	unsigned int			time_slice;
	unsigned short			on_rq;
	unsigned short			on_list;

	struct sched_rt_entity		*back;
#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity		*parent;
	/* rq on which this entity is (to be) queued: */
	struct rt_rq			*rt_rq;
	/* rq "owned" by this entity/group: */
	struct rt_rq			*my_q;
#endif
} __randomize_layout;

struct sched_dl_entity {
	struct rb_node			rb_node;

	/*
	 * Original scheduling parameters. Copied here from sched_attr
	 * during sched_setattr(), they will remain the same until
	 * the next sched_setattr().
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   원래 스케줄링 매개변수. sched_setattr() 동안 sched_attr에서 여기로 
 *   복사되며 다음 sched_setattr()까지 동일하게 유지됩니다.
 * - dl_runtime
 *   최악의 경우 매 dl_period마다 최대 실행될 수 있는 시간.
 * - dl_deadline
 *   일반적으로 dl_period와 같지만, 따로 설정할 경우 dl_period 이내에
 *   또 마감시간을 두는 개념.
 * - 설정 관련 매개변수.
 * - dl_density = dl_runtime / dl_deadline을 미리 계산해놓은것.
 */
	u64				dl_runtime;	/* Maximum runtime for each instance	*/
	u64				dl_deadline;	/* Relative deadline of each instance	*/
	u64				dl_period;	/* Separation of two instances (period) */
	u64				dl_bw;		/* dl_runtime / dl_period		*/
	u64				dl_density;	/* dl_runtime / dl_deadline		*/

	/*
	 * Actual scheduling parameters. Initialized with the values above,
	 * they are continuously updated during task execution. Note that
	 * the remaining runtime could be < 0 in case we are in overrun.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   실제 스케줄링 매개변수. 위의 값으로 초기화되며 태스크 실행 중에 
 *   지속적으로 업데이트됩니다. 오버런이 발생하는 경우 남은 
 *   런타임은 < 0일 수 있습니다.
 *
 * - runtime 관련 매개변수.
 */
	s64				runtime;	/* Remaining runtime for this instance	*/
	u64				deadline;	/* Absolute deadline for this instance	*/
	unsigned int			flags;		/* Specifying the scheduler behaviour	*/

	/*
	 * Some bool flags:
	 *
	 * @dl_throttled tells if we exhausted the runtime. If so, the
	 * task has to wait for a replenishment to be performed at the
	 * next firing of dl_timer.
	 *
	 * @dl_boosted tells if we are boosted due to DI. If so we are
	 * outside bandwidth enforcement mechanism (but only until we
	 * exit the critical section);
	 *
	 * @dl_yielded tells if task gave up the CPU before consuming
	 * all its available runtime during the last job.
	 *
	 * @dl_non_contending tells if the task is inactive while still
	 * contributing to the active utilization. In other words, it
	 * indicates if the inactive timer has been armed and its handler
	 * has not been executed yet. This flag is useful to avoid race
	 * conditions between the inactive timer handler and the wakeup
	 * code.
	 *
	 * @dl_overrun tells if the task asked to be informed about runtime
	 * overruns.
	 */
	/*
	 * IAMROOT. 2023.02.25:
	 * - google-translate
	 *   일부 부울 플래그:
	 *
	 *   @dl_throttled는 런타임이 소진되었는지 알려줍니다. 그렇다면
	 *   작업은 다음 번 dl_timer 실행 시 보충이 수행될 때까지 기다려야
	 *   합니다.
	 *   - 해당 periods에서 runtime이 소진되어 throttle 된 경우
	 *
	 *   @dl_boosted는 DI로 인해 부스트되었는지 여부를 알려줍니다. 그렇다면
	 *   우리는 대역폭 시행 메커니즘 밖에 있는 것입니다(단, 중요한 섹션을 종료할
	 *   때까지만).
	 *   - 해당 periods에서 DI로 인해 부스트되었는지 여부
	 *
	 *   @dl_yielded는 작업이 마지막 작업 중에 사용 가능한 모든 런타임을
	 *   소비하기 전에 CPU를 포기했는지 알려줍니다.
	 *   - 해당 periods에서 runtime 소진전에 포기하고 양보한 경우
;	 *
	 *   @dl_non_contending은 작업이 활성
	 *   사용률에 계속 기여하면서 비활성 상태인지 알려줍니다. 즉, 비활성 타이머가
	 *   준비되었고 핸들러가 아직 실행되지 않았음을 나타냅니다. 이 플래그는 비활성 타이머
	 *   핸들러와 웨이크업 코드 간의 경쟁 조건을 피하는 데 유용합니다.
	 *   - TODO.
	 *
	 *   @dl_overrun은
	 *   태스크가 런타임 오버런에 대한 정보를 요청했는지 여부를 알려줍니다.
	 *   - 해당 periods에서 runtime 이상 동작하는 경우
	 */
	unsigned int			dl_throttled      : 1;
	unsigned int			dl_yielded        : 1;
	unsigned int			dl_non_contending : 1;
	unsigned int			dl_overrun	  : 1;

	/*
	 * Bandwidth enforcement timer. Each -deadline task has its
	 * own bandwidth to be enforced, thus we need one timer per task.
	 */
	struct hrtimer			dl_timer;

	/*
	 * Inactive timer, responsible for decreasing the active utilization
	 * at the "0-lag time". When a -deadline task blocks, it contributes
	 * to GRUB's active utilization until the "0-lag time", hence a
	 * timer is needed to decrease the active utilization at the correct
	 * time.
	 */
	struct hrtimer inactive_timer;

#ifdef CONFIG_RT_MUTEXES
	/*
	 * Priority Inheritance. When a DEADLINE scheduling entity is boosted
	 * pi_se points to the donor, otherwise points to the dl_se it belongs
	 * to (the original one/itself).
	 */
	/*
	 * IAMROOT. 2023.02.27:
	 * - google-translate
	 *   우선 상속. DEADLINE 스케줄링 엔터티가 부스트되면 pi_se는 기증자를 가리키고,
	 *   그렇지 않으면 그것이 속한 dl_se(원래 것/자체)를 가리킵니다.
   * - priority inversion
   *   경우에 따라 priority가 다른 entity로 증가할수있다.
   *   현재 entity가 inversion된 entity를 가리킨다.
   *   아닐 경우 자기자신을 가리킨다.
	 */
	struct sched_dl_entity *pi_se;
#endif
};

#ifdef CONFIG_UCLAMP_TASK
/* Number of utilization clamp buckets (shorter alias) */
#define UCLAMP_BUCKETS CONFIG_UCLAMP_BUCKETS_COUNT

/*
 * Utilization clamp for a scheduling entity
 * @value:		clamp value "assigned" to a se
 * @bucket_id:		bucket index corresponding to the "assigned" value
 * @active:		the se is currently refcounted in a rq's bucket
 * @user_defined:	the requested clamp value comes from user-space
 *
 * The bucket_id is the index of the clamp bucket matching the clamp value
 * which is pre-computed and stored to avoid expensive integer divisions from
 * the fast path.
 *
 * The active bit is set whenever a task has got an "effective" value assigned,
 * which can be different from the clamp value "requested" from user-space.
 * This allows to know a task is refcounted in the rq's bucket corresponding
 * to the "effective" bucket_id.
 *
 * The user_defined bit is set whenever a task has got a task-specific clamp
 * value requested from userspace, i.e. the system defaults apply to this task
 * just as a restriction. This allows to relax default clamps when a less
 * restrictive task-specific value has been requested, thus allowing to
 * implement a "nice" semantic. For example, a task running with a 20%
 * default boost can still drop its own boosting to 0%.
 */
struct uclamp_se {
	unsigned int value		: bits_per(SCHED_CAPACITY_SCALE);
	unsigned int bucket_id		: bits_per(UCLAMP_BUCKETS);
	unsigned int active		: 1;
	unsigned int user_defined	: 1;
};
#endif /* CONFIG_UCLAMP_TASK */

union rcu_special {
	struct {
		u8			blocked;
		u8			need_qs;
		u8			exp_hint; /* Hint for performance. */
		u8			need_mb; /* Readers need smp_mb(). */
	} b; /* Bits. */
	u32 s; /* Set of bits. */
};

enum perf_event_task_context {
	perf_invalid_context = -1,
	perf_hw_context = 0,
	perf_sw_context,
	perf_nr_task_contexts,
};

struct wake_q_node {
	struct wake_q_node *next;
};

struct kmap_ctrl {
#ifdef CONFIG_KMAP_LOCAL
	int				idx;
	pte_t				pteval[KM_MAX_IDX];
#endif
};

struct task_struct {
/*
 * IAMROOT, 2022.11.05: 
 * 아래 커널 옵션이 사용되는 경우 보안을 위해 task 구조체 내부의 가장 위에 
 * thread_info 구조체를 가지고 있게 한다.
 *
 * 기존엔 스택의 가장 마지막에 thread_info 구조체를 가지고 있어서,
 * 이를 통해 task_struct의 위치를 금방 찾는 단점이 있었다.
 */

#ifdef CONFIG_THREAD_INFO_IN_TASK
	/*
	 * For reasons of header soup (see current_thread_info()), this
	 * must be the first element of task_struct.
	 */
	struct thread_info		thread_info;
#endif
/*
 * IAMROOT, 2023.01.26:
 * - TASK_RUNNING, TASK_INTERRUPTIBLE등의 값이 온다.
 */
	unsigned int			__state;

#ifdef CONFIG_PREEMPT_RT
	/* saved state for "spinlock sleepers" */
	unsigned int			saved_state;
#endif

	/*
	 * This begins the randomizable portion of task_struct. Only
	 * scheduling-critical items should be added above here.
	 */
	randomized_struct_fields_start

	void				*stack;
	refcount_t			usage;
	/* Per task flags (PF_*), defined further below: */
	unsigned int			flags;
	unsigned int			ptrace;

/*
 * IAMROOT, 2023.05.13:
 * - openai : on_cpu ?
 *   리눅스 커널의 스케줄러에서 curr은 CPU에서 현재 실행 중인 프로세스나 스레드를 나타내는
 *   task_struct 데이터 구조에 대한 포인터입니다. curr 포인터는 현재 실행 중인 프로세스나
 *   스레드에 대한 정보를 얻는 데 사용될 수 있지만 항상 신뢰성이 있거나 최신 상태인 것은
 *   아닙니다. 그 이유는 curr 포인터가 컨텍스트 스위치를 수행할 때만 업데이트되기 때문입니다.
 *   컨텍스트 스위치는 스케줄러가 CPU에서 현재 실행 중인 프로세스나 스레드를 다른 프로세스나
 *   스레드로 전환하는 경우에 발생합니다. 컨텍스트 스위치 간에는 CPU가 유휴 상태이거나 특정
 *   프로세스나 스레드와 관련이 없는 커널 코드를 실행하는 짧은 시간이 있을 수 있으므로 curr
 *   포인터는 현재 실행 중인 프로세스나 스레드를 정확하게 나타내지 않을 수 있습니다.
 *   그러므로 CPU에서 현재 실행 중인 프로세스나 스레드에 대한 보다 신뢰성이 있고 최신 정보를
 *   얻기 위해서는 task_struct 데이터 구조의 on_cpu 플래그가 대신 사용됩니다. 전에 설명한
 *   대로, on_cpu 플래그는 스케줄러가 프로세스나 스레드를 CPU에서 실행할 때 설정되고
 *   지워지므로 CPU에서 현재 실행 중인 프로세스나 스레드가 어떤 것인지에 대한 더 정확하고
 *   최신 정보를 제공합니다.
 *
 * - on_cpu: 현재 동작중인 task. curr 보다 신뢰성이 있다.
 */
#ifdef CONFIG_SMP
	int				on_cpu;
	struct __call_single_node	wake_entry;
#ifdef CONFIG_THREAD_INFO_IN_TASK
	/* Current CPU: */
	unsigned int			cpu;
#endif
/*
 * IAMROOT, 2023.05.24:
 * - wakee_flips(record_wakee() 참고)
 *   마지막에 깨운 task와 현재(최신) task가 다른 경우에 대한 count.
 *   1초단위로 절반으로 줄어든다.
 */
	unsigned int			wakee_flips;
	unsigned long			wakee_flip_decay_ts;
	struct task_struct		*last_wakee;

	/*
	 * recent_used_cpu is initially set as the last CPU used by a task
	 * that wakes affine another task. Waker/wakee relationships can
	 * push tasks around a CPU where each wakeup moves to the next one.
	 * Tracking a recently used CPU allows a quick search for a recently
	 * used CPU that may be idle.
	 */
/*
 * IAMROOT, 2023.04.13:
 * - papago
 *   recent_used_cpu는 초기에 다른 task과 관련하여 깨우는 task에서 사용한 
 *   마지막 CPU로 설정됩니다. 웨이커/웨이키 관계는 각 웨이크업이 다음 
 *   웨이크업으로 이동하는 CPU 주변에서 작업을 푸시할 수 있습니다.
 *   최근에 사용된 CPU를 추적하면 유휴 상태일 수 있는 최근에 사용된 CPU를 
 *   빠르게 검색할 수 있습니다.
 * - recent_used_cpu
 *   ex) 1 -> 2 -> 3 -> 4 로 cpu가 바뀐다고 했을때, task의 현재 cpu가 3인경우
 *   recent_used_cpu는 2가 된다.
 * - wake_cpu : 깨어날때 사용한 cpu 번호.
 */
	int				recent_used_cpu;
	int				wake_cpu;
#endif
/*
 * IAMROOT, 2022.12.29:
 * - TASK_ON_RQ_QUEUED등의 값이 들어간다.
 */
	int				on_rq;

/*
 * IAMROOT, 2023.02.18:
 * - prio
 *   현재 운영. 상황에 따라 변경될 수 있는 우선순위.
 * - static_prio
 *   cfs 설정
 * - rt_priority
 *   rt 설정
 * - normal_prio
 *   현재 설정. rt로 운영이면 rt_priority, cfs로 운영되면 static_prio
 */
	int				prio;
	int				static_prio;
	int				normal_prio;
	unsigned int			rt_priority;

/*
 * IAMROOT, 2022.12.29:
 * -  DEFINE_SCHED_CLASS(...) 으로 정의된 callback함수들.
 */
	const struct sched_class	*sched_class;
	struct sched_entity		se;
	struct sched_rt_entity		rt;
	struct sched_dl_entity		dl;

#ifdef CONFIG_SCHED_CORE
	struct rb_node			core_node;
	unsigned long			core_cookie;
	unsigned int			core_occupation;
#endif

#ifdef CONFIG_CGROUP_SCHED
	struct task_group		*sched_task_group;
#endif

#ifdef CONFIG_UCLAMP_TASK
	/*
	 * Clamp values requested for a scheduling entity.
	 * Must be updated with task_rq_lock() held.
	 */
	struct uclamp_se		uclamp_req[UCLAMP_CNT];
	/*
	 * Effective clamp values used for a scheduling entity.
	 * Must be updated with task_rq_lock() held.
	 */
	struct uclamp_se		uclamp[UCLAMP_CNT];
#endif

#ifdef CONFIG_PREEMPT_NOTIFIERS
	/* List of struct preempt_notifier: */
	struct hlist_head		preempt_notifiers;
#endif

#ifdef CONFIG_BLK_DEV_IO_TRACE
	unsigned int			btrace_seq;
#endif

	unsigned int			policy;
	int				nr_cpus_allowed;
/*
 * IAMROOT, 2023.04.13:
 * - 기본적으로 자기자신의 cpus_mask를 가리킨다.
 */
	const cpumask_t			*cpus_ptr;
	cpumask_t			*user_cpus_ptr;
	cpumask_t			cpus_mask;
	void				*migration_pending;
#ifdef CONFIG_SMP
	unsigned short			migration_disabled;
#endif
	unsigned short			migration_flags;

#ifdef CONFIG_PREEMPT_RCU
	int				rcu_read_lock_nesting;
	union rcu_special		rcu_read_unlock_special;
	struct list_head		rcu_node_entry;
	struct rcu_node			*rcu_blocked_node;
#endif /* #ifdef CONFIG_PREEMPT_RCU */

#ifdef CONFIG_TASKS_RCU
	unsigned long			rcu_tasks_nvcsw;
	u8				rcu_tasks_holdout;
	u8				rcu_tasks_idx;
	int				rcu_tasks_idle_cpu;
	struct list_head		rcu_tasks_holdout_list;
#endif /* #ifdef CONFIG_TASKS_RCU */

#ifdef CONFIG_TASKS_TRACE_RCU
	int				trc_reader_nesting;
	int				trc_ipi_to_cpu;
	union rcu_special		trc_reader_special;
	bool				trc_reader_checked;
	struct list_head		trc_holdout_list;
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */

	struct sched_info		sched_info;

	struct list_head		tasks;
#ifdef CONFIG_SMP
	struct plist_node		pushable_tasks;
	struct rb_node			pushable_dl_tasks;
#endif

	struct mm_struct		*mm;
	struct mm_struct		*active_mm;

	/* Per-thread vma caching: */
	struct vmacache			vmacache;

#ifdef SPLIT_RSS_COUNTING
	struct task_rss_stat		rss_stat;
#endif
	int				exit_state;
	int				exit_code;
	int				exit_signal;
	/* The signal sent when the parent dies: */
	int				pdeath_signal;
	/* JOBCTL_*, siglock protected: */
	unsigned long			jobctl;

	/* Used for emulating ABI behavior of previous Linux versions: */
	unsigned int			personality;

	/* Scheduler bits, serialized by scheduler locks: */

/*
 * IAMROOT, 2023.04.01:
 * - fork시 schedule reset에 대한 여부.
 */
	unsigned			sched_reset_on_fork:1;
/*
 * IAMROOT, 2023.01.26:
 * - io thread 가 load에 참여 할지를 설정하는 변수
 */
	unsigned			sched_contributes_to_load:1;
	unsigned			sched_migrated:1;
#ifdef CONFIG_PSI
	unsigned			sched_psi_wake_requeue:1;
#endif

	/* Force alignment to the next boundary: */
	unsigned			:0;

	/* Unserialized, strictly 'current' */

	/*
	 * This field must not be in the scheduler word above due to wakelist
	 * queueing no longer being serialized by p->on_cpu. However:
	 *
	 * p->XXX = X;			ttwu()
	 * schedule()			  if (p->on_rq && ..) // false
	 *   smp_mb__after_spinlock();	  if (smp_load_acquire(&p->on_cpu) && //true
	 *   deactivate_task()		      ttwu_queue_wakelist())
	 *     p->on_rq = 0;			p->sched_remote_wakeup = Y;
	 *
	 * guarantees all stores of 'current' are visible before
	 * ->sched_remote_wakeup gets used, so it can be in this word.
	 */
	unsigned			sched_remote_wakeup:1;

	/* Bit to tell LSMs we're in execve(): */
	unsigned			in_execve:1;
	unsigned			in_iowait:1;
#ifndef TIF_RESTORE_SIGMASK
	unsigned			restore_sigmask:1;
#endif
#ifdef CONFIG_MEMCG
	unsigned			in_user_fault:1;
#endif
#ifdef CONFIG_COMPAT_BRK
	unsigned			brk_randomized:1;
#endif
#ifdef CONFIG_CGROUPS
	/* disallow userland-initiated cgroup migration */
	unsigned			no_cgroup_migration:1;
	/* task is frozen/stopped (used by the cgroup freezer) */
	unsigned			frozen:1;
#endif
#ifdef CONFIG_BLK_CGROUP
	unsigned			use_memdelay:1;
#endif
#ifdef CONFIG_PSI
	/* Stalled due to lack of memory */
	unsigned			in_memstall:1;
#endif
#ifdef CONFIG_PAGE_OWNER
	/* Used by page_owner=on to detect recursion in page tracking. */
	unsigned			in_page_owner:1;
#endif
#ifdef CONFIG_EVENTFD
	/* Recursion prevention for eventfd_signal() */
	unsigned			in_eventfd_signal:1;
#endif

	unsigned long			atomic_flags; /* Flags requiring atomic access. */

	struct restart_block		restart_block;

	pid_t				pid;
	pid_t				tgid;

#ifdef CONFIG_STACKPROTECTOR
	/* Canary value for the -fstack-protector GCC feature: */
	unsigned long			stack_canary;
#endif
	/*
	 * Pointers to the (original) parent process, youngest child, younger sibling,
	 * older sibling, respectively.  (p->father can be replaced with
	 * p->real_parent->pid)
	 */

	/* Real parent process: */
	struct task_struct __rcu	*real_parent;

	/* Recipient of SIGCHLD, wait4() reports: */
	struct task_struct __rcu	*parent;

	/*
	 * Children/sibling form the list of natural children:
	 */
	struct list_head		children;
	struct list_head		sibling;
	struct task_struct		*group_leader;

	/*
	 * 'ptraced' is the list of tasks this task is using ptrace() on.
	 *
	 * This includes both natural children and PTRACE_ATTACH targets.
	 * 'ptrace_entry' is this task's link on the p->parent->ptraced list.
	 */
	struct list_head		ptraced;
	struct list_head		ptrace_entry;

	/* PID/PID hash table linkage. */
	/*
	 * IAMROOT, 2023.04.08:
	 * - 2c4704756cab7cfa031ada4dab361562f0e357c0 커밋에 의해 기존 pids
	 * 멤버가 변경됨
	 * -	struct pid_link			pids[PIDTYPE_MAX];
	 * +	struct pid			*thread_pid;
	 * +	struct hlist_node		pid_links[PIDTYPE_MAX];
	 *
	 * - thread_pid
	 *   일반 pid. task_pid_ptr에서 PIDTYPE_PID type인 경우 선택 된다.
	 */
	struct pid			*thread_pid;
	struct hlist_node		pid_links[PIDTYPE_MAX];
	struct list_head		thread_group;
	struct list_head		thread_node;

	struct completion		*vfork_done;

	/* CLONE_CHILD_SETTID: */
/*
 * IAMROOT, 2022.12.29:
 * - chat openai
 *   Linux 커널에서 struct task_struct의 set_child_tid 필드는 작업이 실행될 
 *   때 커널이 작업의 자식 프로세스 ID를 저장할 수 있는 메모리 위치의 주소를 
 *   저장하는 데 사용됩니다.
 *
 *   set_child_tid 필드는 int __user * 유형이며 이는 사용자 공간 메모리의 
 *   정수에 대한 포인터임을 의미합니다. 작업이 실행되면 커널은 
 *   set_child_tid가 가리키는 메모리 위치에 작업의 자식 프로세스 ID를 
 *   저장합니다. 이를 통해 부모 프로세스는 자식 프로세스 ID를 인수로 사용하는 
 *   waitpid 시스템 호출을 사용하여 자식 프로세스가 완료될 때까지 기다릴 수 
 *   있습니다.
 *
 *   일반적인(kthread가 아닌) 작업의 경우 set_child_tid 필드는 위에서 설명한 
 *   대로 사용됩니다. 작업이 실행되면 커널은 set_child_tid가 가리키는 메모리 
 *   위치에 자식 프로세스 ID를 저장하고 부모 프로세스는 waitpid 시스템 호출을 
 *   사용하여 자식 프로세스가 완료될 때까지 기다릴 수 있습니다. 
 *
 *   커널 스레드의 경우 set_child_tid 필드는 작업의 자식 프로세스 ID를 
 *   저장하기 위한 메모리 위치로 의도된 용도가 아니라 커널 스레드와 연결된 
 *   struct kthread에 대한 포인터를 저장하기 위해 종종 "남용"됩니다. 이는 
 *   커널 스레드를 실행할 수 없고 PF_KTHREAD 플래그를 지울 수 없기 때문입니다. 
 *   set_child_tid 필드는 copy_process 함수로 잘못 복사할 수 없기 때문에 
 *   이렇게 사용됩니다.
 */
	int __user			*set_child_tid;

	/* CLONE_CHILD_CLEARTID: */
	int __user			*clear_child_tid;

	/* PF_IO_WORKER */
	void				*pf_io_worker;

	u64				utime;
	u64				stime;
#ifdef CONFIG_ARCH_HAS_SCALED_CPUTIME
	u64				utimescaled;
	u64				stimescaled;
#endif
	u64				gtime;
	struct prev_cputime		prev_cputime;
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	struct vtime			vtime;
#endif

#ifdef CONFIG_NO_HZ_FULL
	atomic_t			tick_dep_mask;
#endif
	/* Context switch counts: */
	/*
	 * IAMROOT, 2023.01.14:
	 * - 다른 task 로의 switching 횟수
	 *   nvcsw(number_voluntary_count_switch?)
	 *   nivcsw(number_involuntary_count_switch?)
	 */
	unsigned long			nvcsw;
	unsigned long			nivcsw;

	/* Monotonic time in nsecs: */
	u64				start_time;

	/* Boot based time in nsecs: */
	u64				start_boottime;

	/* MM fault and swap info: this can arguably be seen as either mm-specific or thread-specific: */
	unsigned long			min_flt;
	unsigned long			maj_flt;

	/* Empty if CONFIG_POSIX_CPUTIMERS=n */
	struct posix_cputimers		posix_cputimers;

#ifdef CONFIG_POSIX_CPU_TIMERS_TASK_WORK
	struct posix_cputimers_work	posix_cputimers_work;
#endif

	/* Process credentials: */

	/* Tracer's credentials at attach: */
	const struct cred __rcu		*ptracer_cred;

	/* Objective and real subjective task credentials (COW): */
	const struct cred __rcu		*real_cred;

	/* Effective (overridable) subjective task credentials (COW): */
	const struct cred __rcu		*cred;

#ifdef CONFIG_KEYS
	/* Cached requested key. */
	struct key			*cached_requested_key;
#endif

	/*
	 * executable name, excluding path.
	 *
	 * - normally initialized setup_new_exec()
	 * - access it with [gs]et_task_comm()
	 * - lock it with task_lock()
	 */
	char				comm[TASK_COMM_LEN];

	struct nameidata		*nameidata;

#ifdef CONFIG_SYSVIPC
	struct sysv_sem			sysvsem;
	struct sysv_shm			sysvshm;
#endif
#ifdef CONFIG_DETECT_HUNG_TASK
	unsigned long			last_switch_count;
	unsigned long			last_switch_time;
#endif
	/* Filesystem information: */
	struct fs_struct		*fs;

	/* Open file information: */
	struct files_struct		*files;

#ifdef CONFIG_IO_URING
	struct io_uring_task		*io_uring;
#endif

	/* Namespaces: */
/*
 * IAMROOT, 2023.04.01:
 * - 소속된 namespace proxy.
 *   init task의 경우 init_nsproxy
 */
	struct nsproxy			*nsproxy;

	/* Signal handlers: */
	struct signal_struct		*signal;
	struct sighand_struct __rcu		*sighand;
	sigset_t			blocked;
	sigset_t			real_blocked;
	/* Restored if set_restore_sigmask() was used: */
	sigset_t			saved_sigmask;
	struct sigpending		pending;
	unsigned long			sas_ss_sp;
	size_t				sas_ss_size;
	unsigned int			sas_ss_flags;

	struct callback_head		*task_works;

#ifdef CONFIG_AUDIT
#ifdef CONFIG_AUDITSYSCALL
	struct audit_context		*audit_context;
#endif
	kuid_t				loginuid;
	unsigned int			sessionid;
#endif
	struct seccomp			seccomp;
	struct syscall_user_dispatch	syscall_dispatch;

	/* Thread group tracking: */
	u64				parent_exec_id;
	u64				self_exec_id;

	/* Protection against (de-)allocation: mm, files, fs, tty, keyrings, mems_allowed, mempolicy: */
	spinlock_t			alloc_lock;

	/* Protection of the PI data structures: */
	raw_spinlock_t			pi_lock;

	struct wake_q_node		wake_q;

#ifdef CONFIG_RT_MUTEXES
	/* PI waiters blocked on a rt_mutex held by this task: */
	struct rb_root_cached		pi_waiters;
	/* Updated under owner's pi_lock and rq lock */
	struct task_struct		*pi_top_task;
	/* Deadlock detection and priority inheritance handling: */
	struct rt_mutex_waiter		*pi_blocked_on;
#endif

#ifdef CONFIG_DEBUG_MUTEXES
	/* Mutex deadlock detection: */
	struct mutex_waiter		*blocked_on;
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	int				non_block_count;
#endif

#ifdef CONFIG_TRACE_IRQFLAGS
	struct irqtrace_events		irqtrace;
	unsigned int			hardirq_threaded;
	u64				hardirq_chain_key;
	int				softirqs_enabled;
	int				softirq_context;
	int				irq_config;
#endif
#ifdef CONFIG_PREEMPT_RT
	int				softirq_disable_cnt;
#endif

#ifdef CONFIG_LOCKDEP
# define MAX_LOCK_DEPTH			48UL
	u64				curr_chain_key;
	int				lockdep_depth;
	unsigned int			lockdep_recursion;
	struct held_lock		held_locks[MAX_LOCK_DEPTH];
#endif

#if defined(CONFIG_UBSAN) && !defined(CONFIG_UBSAN_TRAP)
	unsigned int			in_ubsan;
#endif

	/* Journalling filesystem info: */
	void				*journal_info;

	/* Stacked block device info: */
	struct bio_list			*bio_list;

#ifdef CONFIG_BLOCK
	/* Stack plugging: */
	struct blk_plug			*plug;
#endif

	/* VM state: */
	struct reclaim_state		*reclaim_state;

	struct backing_dev_info		*backing_dev_info;

	struct io_context		*io_context;

#ifdef CONFIG_COMPACTION
	struct capture_control		*capture_control;
#endif
	/* Ptrace state: */
	unsigned long			ptrace_message;
	kernel_siginfo_t		*last_siginfo;

	struct task_io_accounting	ioac;
#ifdef CONFIG_PSI
	/* Pressure stall state */
	unsigned int			psi_flags;
#endif
#ifdef CONFIG_TASK_XACCT
	/* Accumulated RSS usage: */
	u64				acct_rss_mem1;
	/* Accumulated virtual memory usage: */
	u64				acct_vm_mem1;
	/* stime + utime since last update: */
	u64				acct_timexpd;
#endif
#ifdef CONFIG_CPUSETS
	/* Protected by ->alloc_lock: */
/*
 * IAMROOT, 2022.02.12:
 * - 해당 task가 할당할수있는 node들을 표시.
 */
	nodemask_t			mems_allowed;
	/* Sequence number to catch updates: */
	seqcount_spinlock_t		mems_allowed_seq;
	int				cpuset_mem_spread_rotor;
	int				cpuset_slab_spread_rotor;
#endif
#ifdef CONFIG_CGROUPS
	/* Control Group info protected by css_set_lock: */
	struct css_set __rcu		*cgroups;
	/* cg_list protected by css_set_lock and tsk->alloc_lock: */
	struct list_head		cg_list;
#endif
#ifdef CONFIG_X86_CPU_RESCTRL
	u32				closid;
	u32				rmid;
#endif
#ifdef CONFIG_FUTEX
	struct robust_list_head __user	*robust_list;
#ifdef CONFIG_COMPAT
	struct compat_robust_list_head __user *compat_robust_list;
#endif
	struct list_head		pi_state_list;
	struct futex_pi_state		*pi_state_cache;
	struct mutex			futex_exit_mutex;
	unsigned int			futex_state;
#endif
#ifdef CONFIG_PERF_EVENTS
	struct perf_event_context	*perf_event_ctxp[perf_nr_task_contexts];
	struct mutex			perf_event_mutex;
	struct list_head		perf_event_list;
#endif
#ifdef CONFIG_DEBUG_PREEMPT
	unsigned long			preempt_disable_ip;
#endif
#ifdef CONFIG_NUMA
	/* Protected by alloc_lock: */
	struct mempolicy		*mempolicy;
/*
 * IAMROOT, 2022.05.14:
 * - il_prev(interleave prev)
 *   numa policy에서 사용한다.
 *   interlev_nodes()에서 node를 순회할때 이전에 순회했던 node가 뭐였는지
 *   nid형태로 기록한다.
 */
	short				il_prev;
	/*
	 * IAMROOT, 2023.03.25:
	 * - pref_node_fork : fork 시 어느 node로 갈 것인가
	 */
	short				pref_node_fork;
#endif
#ifdef CONFIG_NUMA_BALANCING
/*
 * IAMROOT, 2023.06.17:
 * - numa_scan_seq : user라면 mm->numa_scan_seq로 초기화된다.
 *   numa_scan_period : 기본 1000 msec
 */
	int				numa_scan_seq;
	unsigned int			numa_scan_period;
	unsigned int			numa_scan_period_max;
	int				numa_preferred_nid;
	unsigned long			numa_migrate_retry;
	/* Migration stamp: */
/*
 * IAMROOT, 2023.06.17:
 * - 계산된 task(thread)기준의 node scan 간격(msec)
 *   실제 scan은 mm->numa_next_scan
 *   task_numa_work()에서 scan후, task 실행시간의 최대 3%로 제한한다.
 */
	u64				node_stamp;
	u64				last_task_numa_placement;
	u64				last_sum_exec_runtime;
/*
 * IAMROOT, 2023.06.17:
 * - task_numa_work()가 등록된다.
 */
	struct callback_head		numa_work;

	/*
	 * This pointer is only modified for current in syscall and
	 * pagefault context (and for tasks being destroyed), so it can be read
	 * from any of the following contexts:
	 *  - RCU read-side critical section
	 *  - current->numa_group from everywhere
	 *  - task's runqueue locked, task not running
	 */
	struct numa_group __rcu		*numa_group;

	/*
	 * numa_faults is an array split into four regions:
	 * faults_memory, faults_cpu, faults_memory_buffer, faults_cpu_buffer
	 * in this precise order.
	 *
	 * faults_memory: Exponential decaying average of faults on a per-node
	 * basis. Scheduling placement decisions are made based on these
	 * counts. The values remain static for the duration of a PTE scan.
	 * faults_cpu: Track the nodes the process was running on when a NUMA
	 * hinting fault was incurred.
	 * faults_memory_buffer and faults_cpu_buffer: Record faults per node
	 * during the current scan window. When the scan completes, the counts
	 * in faults_memory and faults_cpu decay and these values are copied.
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   numa_faults는 4개의 영역으로 분할된 배열입니다.
 *   faults_memory, faults_cpu, faults_memory_buffer, faults_cpu_buffer 이 정확한 순서대로.
 *
 *   faults_memory: 노드당 기준으로 결함의 기하급수적 감쇠 평균. 일정 배치
 *   결정은 이러한 수를 기반으로 이루어집니다. 값은 PTE 스캔 기간 동안 정적
 *   상태로 유지됩니다.
 *   faults_cpu: NUMA 힌트 오류가 발생했을 때 프로세스가 실행 중인 노드를 추적합니다.
 *   faults_memory_buffer 및 faults_cpu_buffer: 현재 스캔 기간 동안 노드당 오류를 기록합니다.
 *   스캔이 완료되면 faults_memory 및 faults_cpu의 카운트가 감소하고 이러한 값이 복사됩니다. 
 */
	unsigned long			*numa_faults;
	unsigned long			total_numa_faults;

	/*
	 * numa_faults_locality tracks if faults recorded during the last
	 * scan window were remote/local or failed to migrate. The task scan
	 * period is adapted based on the locality of the faults with different
	 * weights depending on whether they were shared or private faults
	 */
/*
 * IAMROOT, 2023.06.24:
 * - papago
 *   numa_faults_locality는 마지막 스캔 기간 동안 기록된 결함이
 *   원격/로컬인지 또는 마이그레이션에 실패했는지 추적합니다. 태스크
 *   스캔 기간은 공유 결함인지 개인 결함인지에 따라 가중치가 다른 결함의
 *   위치에 따라 조정됩니다. 
 * - remote / local / fail
 */
	unsigned long			numa_faults_locality[3];

	unsigned long			numa_pages_migrated;
#endif /* CONFIG_NUMA_BALANCING */

#ifdef CONFIG_RSEQ
	struct rseq __user *rseq;
	u32 rseq_sig;
	/*
	 * RmW on rseq_event_mask must be performed atomically
	 * with respect to preemption.
	 */
	unsigned long rseq_event_mask;
#endif

	struct tlbflush_unmap_batch	tlb_ubc;

	union {
		refcount_t		rcu_users;
		struct rcu_head		rcu;
	};

	/* Cache last used pipe for splice(): */
	struct pipe_inode_info		*splice_pipe;

	struct page_frag		task_frag;

#ifdef CONFIG_TASK_DELAY_ACCT
	struct task_delay_info		*delays;
#endif

#ifdef CONFIG_FAULT_INJECTION
	int				make_it_fail;
	unsigned int			fail_nth;
#endif
	/*
	 * When (nr_dirtied >= nr_dirtied_pause), it's time to call
	 * balance_dirty_pages() for a dirty throttling pause:
	 */
	int				nr_dirtied;
	int				nr_dirtied_pause;
	/* Start of a write-and-pause period: */
	unsigned long			dirty_paused_when;

#ifdef CONFIG_LATENCYTOP
	int				latency_record_count;
	struct latency_record		latency_record[LT_SAVECOUNT];
#endif
	/*
	 * Time slack values; these are used to round up poll() and
	 * select() etc timeout values. These are in nanoseconds.
	 */
	u64				timer_slack_ns;
	u64				default_timer_slack_ns;

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	unsigned int			kasan_depth;
#endif

#ifdef CONFIG_KCSAN
	struct kcsan_ctx		kcsan_ctx;
#ifdef CONFIG_TRACE_IRQFLAGS
	struct irqtrace_events		kcsan_save_irqtrace;
#endif
#endif

#if IS_ENABLED(CONFIG_KUNIT)
	struct kunit			*kunit_test;
#endif

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	/* Index of current stored address in ret_stack: */
	int				curr_ret_stack;
	int				curr_ret_depth;

	/* Stack of return addresses for return function tracing: */
	struct ftrace_ret_stack		*ret_stack;

	/* Timestamp for last schedule: */
	unsigned long long		ftrace_timestamp;

	/*
	 * Number of functions that haven't been traced
	 * because of depth overrun:
	 */
	atomic_t			trace_overrun;

	/* Pause tracing: */
	atomic_t			tracing_graph_pause;
#endif

#ifdef CONFIG_TRACING
	/* State flags for use by tracers: */
	unsigned long			trace;

	/* Bitmask and counter of trace recursion: */
	unsigned long			trace_recursion;
#endif /* CONFIG_TRACING */

#ifdef CONFIG_KCOV
	/* See kernel/kcov.c for more details. */

	/* Coverage collection mode enabled for this task (0 if disabled): */
	unsigned int			kcov_mode;

	/* Size of the kcov_area: */
	unsigned int			kcov_size;

	/* Buffer for coverage collection: */
	void				*kcov_area;

	/* KCOV descriptor wired with this task or NULL: */
	struct kcov			*kcov;

	/* KCOV common handle for remote coverage collection: */
	u64				kcov_handle;

	/* KCOV sequence number: */
	int				kcov_sequence;

	/* Collect coverage from softirq context: */
	unsigned int			kcov_softirq;
#endif

#ifdef CONFIG_MEMCG
	struct mem_cgroup		*memcg_in_oom;
	gfp_t				memcg_oom_gfp_mask;
	int				memcg_oom_order;

	/* Number of pages to reclaim on returning to userland: */
	unsigned int			memcg_nr_pages_over_high;

	/* Used by memcontrol for targeted memcg charge: */
	struct mem_cgroup		*active_memcg;
#endif

#ifdef CONFIG_BLK_CGROUP
	struct request_queue		*throttle_queue;
#endif

#ifdef CONFIG_UPROBES
	struct uprobe_task		*utask;
#endif
#if defined(CONFIG_BCACHE) || defined(CONFIG_BCACHE_MODULE)
	unsigned int			sequential_io;
	unsigned int			sequential_io_avg;
#endif
	struct kmap_ctrl		kmap_ctrl;
#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	unsigned long			task_state_change;
# ifdef CONFIG_PREEMPT_RT
	unsigned long			saved_state_change;
# endif
#endif
	int				pagefault_disabled;
#ifdef CONFIG_MMU
	struct task_struct		*oom_reaper_list;
#endif
#ifdef CONFIG_VMAP_STACK
	struct vm_struct		*stack_vm_area;
#endif
#ifdef CONFIG_THREAD_INFO_IN_TASK
	/* A live task holds one reference: */
	refcount_t			stack_refcount;
#endif
#ifdef CONFIG_LIVEPATCH
	int patch_state;
#endif
#ifdef CONFIG_SECURITY
	/* Used by LSM modules for access restriction: */
	void				*security;
#endif
#ifdef CONFIG_BPF_SYSCALL
	/* Used by BPF task local storage */
	struct bpf_local_storage __rcu	*bpf_storage;
	/* Used for BPF run context */
	struct bpf_run_ctx		*bpf_ctx;
#endif

#ifdef CONFIG_GCC_PLUGIN_STACKLEAK
	unsigned long			lowest_stack;
	unsigned long			prev_lowest_stack;
#endif

#ifdef CONFIG_X86_MCE
	void __user			*mce_vaddr;
	__u64				mce_kflags;
	u64				mce_addr;
	__u64				mce_ripv : 1,
					mce_whole_page : 1,
					__mce_reserved : 62;
	struct callback_head		mce_kill_me;
	int				mce_count;
#endif

#ifdef CONFIG_KRETPROBES
	struct llist_head               kretprobe_instances;
#endif

#ifdef CONFIG_ARCH_HAS_PARANOID_L1D_FLUSH
	/*
	 * If L1D flush is supported on mm context switch
	 * then we use this callback head to queue kill work
	 * to kill tasks that are not running on SMT disabled
	 * cores
	 */
	struct callback_head		l1d_flush_kill;
#endif

	/*
	 * New fields for task_struct should be added above here, so that
	 * they are included in the randomized portion of task_struct.
	 */
	randomized_struct_fields_end

	/* CPU-specific state of this task: */
	struct thread_struct		thread;

	/*
	 * WARNING: on x86, 'thread_struct' contains a variable-sized
	 * structure.  It *MUST* be at the end of 'task_struct'.
	 *
	 * Do not put anything below here!
	 */
};

/*
 * IAMROOT, 2023.04.13:
 * - thread_pid를 return.
 */
static inline struct pid *task_pid(struct task_struct *task)
{
	return task->thread_pid;
}

/*
 * the helpers to get the task's different pids as they are seen
 * from various namespaces
 *
 * task_xid_nr()     : global id, i.e. the id seen from the init namespace;
 * task_xid_vnr()    : virtual id, i.e. the id seen from the pid namespace of
 *                     current.
 * task_xid_nr_ns()  : id seen from the ns specified;
 *
 * see also pid_nr() etc in include/linux/pid.h
 */
pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type, struct pid_namespace *ns);

static inline pid_t task_pid_nr(struct task_struct *tsk)
{
	return tsk->pid;
}

static inline pid_t task_pid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PID, ns);
}

static inline pid_t task_pid_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PID, NULL);
}


static inline pid_t task_tgid_nr(struct task_struct *tsk)
{
	return tsk->tgid;
}

/**
 * pid_alive - check that a task structure is not stale
 * @p: Task structure to be checked.
 *
 * Test if a process is not yet dead (at most zombie state)
 * If pid_alive fails, then pointers within the task structure
 * can be stale and must not be dereferenced.
 *
 * Return: 1 if the process is alive. 0 otherwise.
 */
static inline int pid_alive(const struct task_struct *p)
{
	return p->thread_pid != NULL;
}

static inline pid_t task_pgrp_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PGID, ns);
}

static inline pid_t task_pgrp_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PGID, NULL);
}


static inline pid_t task_session_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_SID, ns);
}

static inline pid_t task_session_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_SID, NULL);
}

static inline pid_t task_tgid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_TGID, ns);
}

static inline pid_t task_tgid_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_TGID, NULL);
}

static inline pid_t task_ppid_nr_ns(const struct task_struct *tsk, struct pid_namespace *ns)
{
	pid_t pid = 0;

	rcu_read_lock();
	if (pid_alive(tsk))
		pid = task_tgid_nr_ns(rcu_dereference(tsk->real_parent), ns);
	rcu_read_unlock();

	return pid;
}

static inline pid_t task_ppid_nr(const struct task_struct *tsk)
{
	return task_ppid_nr_ns(tsk, &init_pid_ns);
}

/* Obsolete, do not use: */
static inline pid_t task_pgrp_nr(struct task_struct *tsk)
{
	return task_pgrp_nr_ns(tsk, &init_pid_ns);
}

#define TASK_REPORT_IDLE	(TASK_REPORT + 1)
#define TASK_REPORT_MAX		(TASK_REPORT_IDLE << 1)

static inline unsigned int task_state_index(struct task_struct *tsk)
{
	unsigned int tsk_state = READ_ONCE(tsk->__state);
	unsigned int state = (tsk_state | tsk->exit_state) & TASK_REPORT;

	BUILD_BUG_ON_NOT_POWER_OF_2(TASK_REPORT_MAX);

	if (tsk_state == TASK_IDLE)
		state = TASK_REPORT_IDLE;

	return fls(state);
}

static inline char task_index_to_char(unsigned int state)
{
	static const char state_char[] = "RSDTtXZPI";

	BUILD_BUG_ON(1 + ilog2(TASK_REPORT_MAX) != sizeof(state_char) - 1);

	return state_char[state];
}

static inline char task_state_to_char(struct task_struct *tsk)
{
	return task_index_to_char(task_state_index(tsk));
}

/**
 * is_global_init - check if a task structure is init. Since init
 * is free to have sub-threads we need to check tgid.
 * @tsk: Task structure to be checked.
 *
 * Check if a task structure is the first user space task the kernel created.
 *
 * Return: 1 if the task structure is init. 0 otherwise.
 */
static inline int is_global_init(struct task_struct *tsk)
{
	return task_tgid_nr(tsk) == 1;
}

extern struct pid *cad_pid;

/*
 * Per process flags
 */
#define PF_VCPU			0x00000001	/* I'm a virtual CPU */
#define PF_IDLE			0x00000002	/* I am an IDLE thread */
#define PF_EXITING		0x00000004	/* Getting shut down */
#define PF_IO_WORKER		0x00000010	/* Task is an IO worker */
#define PF_WQ_WORKER		0x00000020	/* I'm a workqueue worker */
#define PF_FORKNOEXEC		0x00000040	/* Forked but didn't exec */
#define PF_MCE_PROCESS		0x00000080      /* Process policy on mce errors */
#define PF_SUPERPRIV		0x00000100	/* Used super-user privileges */
#define PF_DUMPCORE		0x00000200	/* Dumped core */
#define PF_SIGNALED		0x00000400	/* Killed by a signal */
/*
 * IAMROOT, 2022.04.16:
 * - PF_MEMALLOC
 *   task가 memory 회수 mode이면 set된다. memory 제한없이 사용할수있는 권한을 가질 수 있다.
 *   memory 회수 재귀등을 확인하는 flag로도 사용한다.
 */
#define PF_MEMALLOC		0x00000800	/* Allocating memory */
#define PF_NPROC_EXCEEDED	0x00001000	/* set_user() noticed that RLIMIT_NPROC was exceeded */
#define PF_USED_MATH		0x00002000	/* If unset the fpu must be initialized before use */
#define PF_USED_ASYNC		0x00004000	/* Used async_schedule*(), used by module init */
#define PF_NOFREEZE		0x00008000	/* This thread should not be frozen */
#define PF_FROZEN		0x00010000	/* Frozen for system suspend */
#define PF_KSWAPD		0x00020000	/* I am kswapd */
#define PF_MEMALLOC_NOFS	0x00040000	/* All allocation requests will inherit GFP_NOFS */
#define PF_MEMALLOC_NOIO	0x00080000	/* All allocation requests will inherit GFP_NOIO */
#define PF_LOCAL_THROTTLE	0x00100000	/* Throttle writes only against the bdi I write to,
						 * I am cleaning dirty pages from some other bdi. */
#define PF_KTHREAD		0x00200000	/* I am a kernel thread */
#define PF_RANDOMIZE		0x00400000	/* Randomize virtual address space */
#define PF_SWAPWRITE		0x00800000	/* Allowed to write to swap */
#define PF_NO_SETAFFINITY	0x04000000	/* Userland is not allowed to meddle with cpus_mask */
#define PF_MCE_EARLY		0x08000000      /* Early kill for mce process policy */
#define PF_MEMALLOC_PIN		0x10000000	/* Allocation context constrained to zones which allow long term pinning. */
#define PF_FREEZER_SKIP		0x40000000	/* Freezer should not count it as freezable */
#define PF_SUSPEND_TASK		0x80000000      /* This thread called freeze_processes() and should not be frozen */

/*
 * Only the _current_ task can read/write to tsk->flags, but other
 * tasks can access tsk->flags in readonly mode for example
 * with tsk_used_math (like during threaded core dumping).
 * There is however an exception to this rule during ptrace
 * or during fork: the ptracer task is allowed to write to the
 * child->flags of its traced child (same goes for fork, the parent
 * can write to the child->flags), because we're guaranteed the
 * child is not running and in turn not changing child->flags
 * at the same time the parent does it.
 */
#define clear_stopped_child_used_math(child)	do { (child)->flags &= ~PF_USED_MATH; } while (0)
#define set_stopped_child_used_math(child)	do { (child)->flags |= PF_USED_MATH; } while (0)
#define clear_used_math()			clear_stopped_child_used_math(current)
#define set_used_math()				set_stopped_child_used_math(current)

#define conditional_stopped_child_used_math(condition, child) \
	do { (child)->flags &= ~PF_USED_MATH, (child)->flags |= (condition) ? PF_USED_MATH : 0; } while (0)

#define conditional_used_math(condition)	conditional_stopped_child_used_math(condition, current)

#define copy_to_stopped_child_used_math(child) \
	do { (child)->flags &= ~PF_USED_MATH, (child)->flags |= current->flags & PF_USED_MATH; } while (0)

/* NOTE: this will return 0 or PF_USED_MATH, it will never return 1 */
#define tsk_used_math(p)			((p)->flags & PF_USED_MATH)
#define used_math()				tsk_used_math(current)

static __always_inline bool is_percpu_thread(void)
{
#ifdef CONFIG_SMP
	return (current->flags & PF_NO_SETAFFINITY) &&
		(current->nr_cpus_allowed  == 1);
#else
	return true;
#endif
}

/* Per-process atomic flags. */
#define PFA_NO_NEW_PRIVS		0	/* May not gain new privileges. */
#define PFA_SPREAD_PAGE			1	/* Spread page cache over cpuset */
#define PFA_SPREAD_SLAB			2	/* Spread some slab caches over cpuset */
#define PFA_SPEC_SSB_DISABLE		3	/* Speculative Store Bypass disabled */
#define PFA_SPEC_SSB_FORCE_DISABLE	4	/* Speculative Store Bypass force disabled*/
#define PFA_SPEC_IB_DISABLE		5	/* Indirect branch speculation restricted */
#define PFA_SPEC_IB_FORCE_DISABLE	6	/* Indirect branch speculation permanently restricted */
#define PFA_SPEC_SSB_NOEXEC		7	/* Speculative Store Bypass clear on execve() */

#define TASK_PFA_TEST(name, func)					\
	static inline bool task_##func(struct task_struct *p)		\
	{ return test_bit(PFA_##name, &p->atomic_flags); }

#define TASK_PFA_SET(name, func)					\
	static inline void task_set_##func(struct task_struct *p)	\
	{ set_bit(PFA_##name, &p->atomic_flags); }

#define TASK_PFA_CLEAR(name, func)					\
	static inline void task_clear_##func(struct task_struct *p)	\
	{ clear_bit(PFA_##name, &p->atomic_flags); }

TASK_PFA_TEST(NO_NEW_PRIVS, no_new_privs)
TASK_PFA_SET(NO_NEW_PRIVS, no_new_privs)

TASK_PFA_TEST(SPREAD_PAGE, spread_page)
TASK_PFA_SET(SPREAD_PAGE, spread_page)
TASK_PFA_CLEAR(SPREAD_PAGE, spread_page)

TASK_PFA_TEST(SPREAD_SLAB, spread_slab)
TASK_PFA_SET(SPREAD_SLAB, spread_slab)
TASK_PFA_CLEAR(SPREAD_SLAB, spread_slab)

TASK_PFA_TEST(SPEC_SSB_DISABLE, spec_ssb_disable)
TASK_PFA_SET(SPEC_SSB_DISABLE, spec_ssb_disable)
TASK_PFA_CLEAR(SPEC_SSB_DISABLE, spec_ssb_disable)

TASK_PFA_TEST(SPEC_SSB_NOEXEC, spec_ssb_noexec)
TASK_PFA_SET(SPEC_SSB_NOEXEC, spec_ssb_noexec)
TASK_PFA_CLEAR(SPEC_SSB_NOEXEC, spec_ssb_noexec)

TASK_PFA_TEST(SPEC_SSB_FORCE_DISABLE, spec_ssb_force_disable)
TASK_PFA_SET(SPEC_SSB_FORCE_DISABLE, spec_ssb_force_disable)

TASK_PFA_TEST(SPEC_IB_DISABLE, spec_ib_disable)
TASK_PFA_SET(SPEC_IB_DISABLE, spec_ib_disable)
TASK_PFA_CLEAR(SPEC_IB_DISABLE, spec_ib_disable)

TASK_PFA_TEST(SPEC_IB_FORCE_DISABLE, spec_ib_force_disable)
TASK_PFA_SET(SPEC_IB_FORCE_DISABLE, spec_ib_force_disable)

static inline void
current_restore_flags(unsigned long orig_flags, unsigned long flags)
{
	current->flags &= ~flags;
	current->flags |= orig_flags & flags;
}

extern int cpuset_cpumask_can_shrink(const struct cpumask *cur, const struct cpumask *trial);
extern int task_can_attach(struct task_struct *p, const struct cpumask *cs_cpus_allowed);
#ifdef CONFIG_SMP
extern void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask);
extern int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask);
extern int dup_user_cpus_ptr(struct task_struct *dst, struct task_struct *src, int node);
extern void release_user_cpus_ptr(struct task_struct *p);
extern int dl_task_check_affinity(struct task_struct *p, const struct cpumask *mask);
extern void force_compatible_cpus_allowed_ptr(struct task_struct *p);
extern void relax_compatible_cpus_allowed_ptr(struct task_struct *p);
#else
static inline void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
}
static inline int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	if (!cpumask_test_cpu(0, new_mask))
		return -EINVAL;
	return 0;
}
static inline int dup_user_cpus_ptr(struct task_struct *dst, struct task_struct *src, int node)
{
	if (src->user_cpus_ptr)
		return -EINVAL;
	return 0;
}
static inline void release_user_cpus_ptr(struct task_struct *p)
{
	WARN_ON(p->user_cpus_ptr);
}

static inline int dl_task_check_affinity(struct task_struct *p, const struct cpumask *mask)
{
	return 0;
}
#endif

extern int yield_to(struct task_struct *p, bool preempt);
extern void set_user_nice(struct task_struct *p, long nice);
extern int task_prio(const struct task_struct *p);

/**
 * task_nice - return the nice value of a given task.
 * @p: the task in question.
 *
 * Return: The nice value [ -20 ... 0 ... 19 ].
 */
static inline int task_nice(const struct task_struct *p)
{
	return PRIO_TO_NICE((p)->static_prio);
}

extern int can_nice(const struct task_struct *p, const int nice);
extern int task_curr(const struct task_struct *p);
extern int idle_cpu(int cpu);
extern int available_idle_cpu(int cpu);
extern int sched_setscheduler(struct task_struct *, int, const struct sched_param *);
extern int sched_setscheduler_nocheck(struct task_struct *, int, const struct sched_param *);
extern void sched_set_fifo(struct task_struct *p);
extern void sched_set_fifo_low(struct task_struct *p);
extern void sched_set_normal(struct task_struct *p, int nice);
extern int sched_setattr(struct task_struct *, const struct sched_attr *);
extern int sched_setattr_nocheck(struct task_struct *, const struct sched_attr *);
extern struct task_struct *idle_task(int cpu);

/**
 * is_idle_task - is the specified task an idle task?
 * @p: the task in question.
 *
 * Return: 1 if @p is an idle task. 0 otherwise.
 */
/*
 * IAMROOT. 2023.07.15:
 * - google-translate
 * is_idle_task - 지정된 작업이 유휴 작업입니까?
 * @p: 해당 작업.
 *
 * 반환: @p가 유휴 작업인 경우 1입니다. 그렇지 않으면 0입니다.
 */
static __always_inline bool is_idle_task(const struct task_struct *p)
{
	return !!(p->flags & PF_IDLE);
}

extern struct task_struct *curr_task(int cpu);
extern void ia64_set_curr_task(int cpu, struct task_struct *p);

void yield(void);

/*
 * IAMROOT, 2021.09.04:
 *
 * INIT_TASK_DATA, init_thread_info, init_task를 종합적으로 살펴보면
 * 다음과 같다.
 *
 * ====================================================================
 *
 * CONFIG별 thread_info, task위치 (stack start, end는 GROW DOWN 기준)
 *
 * on,off / on,off : CONFIG_ARCH_TASK_STRUCT_ON_STACK/CONFIG_THREAD_INFO_IN_TASK
 *
 * - on / on : stack에 task가 존재하며 task에 thread_info가 존재한다.
 *       +--------- stack ---------------------------------------> TASK_SIZE
 *       +-- task_struct(thread_info 포함) --->| stack end       | stack start
 *  
 * - on / off : stack에는 task, thread_info가 순서대로 존재한다.(사용안함)
 *       +--------- stack ---------------------------------------> TASK_SIZE
 *       +- task_struct ----->| -thread_info ->| stack end       | stack start
 *
 * - off / on : stack은 stack 용도로만 사용한다.
 *       +--------- stack ---------------------------------------> TASK_SIZE
 *       + stack end                                             | stack start
 *
 * - off / off : stack에 thread_info가 존재한다.
 *       +--------- stack ---------------------------------------> TASK_SIZE
 *       +--thread_info ->| stack end                            | stack start
 *
 * current_thread_info()의 주석을 살펴보면 CONFIG_THREAD_INFO_IN_TASK == off
 * 에 대한것은 플랫폼 개발자가 정의해야되는거 같다.
 * task_thread_info에서도 기본적으로 task->stack으로 접근을 하여
 * task안에 thread_info가 없는 경우는 일단 무조건 stack의 첫주소를 보게는
 * 해놨지만 새로 정의 가능하게 해놓은걸 볼수있다.
 * ====================================================================
 *
 * thread_info가 task안에 들어갈수 있으며 task는 stack안에 들어갈수 있는
 * 구조이다. 그렇기 때문에 union구조가 된다
 * 
 * - CONFIG_ARCH_TASK_STRUCT_ON_STACK(default off)
 *   task 구조체가 stack안에 있는지 없는지에 대한 설정.
 *
 * - CONFIG_THREAD_INFO_IN_TASK(default on)
 *   struct task_struct 안에 thread_info가 존재하게하는지에 대한 설정.
 *   존재하지 않으면 kernel stack에 넣어야된다.
 *
 *   결국에 default는 task와 stack만 union으로 공유하고 있는 상황이다.
 *
 * 히스토리:
 * =========
 * 다음과 같은 순서대로 발전하였다.
 *
 * 1) 스택에 task 및 thread_info 구조체 존재
 *    - 현재 ia64 아키텍처만 사용
 * 2) 스택에서 task 구조체 분리하고, thread_info만 존재
 *    - 현재 arm, mips, ... 아키텍처에서 사용
 * 3) 스택에서 모든 구조체 분리
 *    - 현재 arm64, x86, powerpc, s390, riscv, ndis32 아키텍처에서 사용
 * ====================================================================
 */
union thread_union {
#ifndef CONFIG_ARCH_TASK_STRUCT_ON_STACK
	struct task_struct task;
#endif
#ifndef CONFIG_THREAD_INFO_IN_TASK
	struct thread_info thread_info;
#endif
	unsigned long stack[THREAD_SIZE/sizeof(long)];
};

#ifndef CONFIG_THREAD_INFO_IN_TASK
extern struct thread_info init_thread_info;
#endif

extern unsigned long init_stack[THREAD_SIZE / sizeof(unsigned long)];

#ifdef CONFIG_THREAD_INFO_IN_TASK
/*
 * IAMROOT, 2023.04.13:
 * - @task의 thread_info return.
 */
static inline struct thread_info *task_thread_info(struct task_struct *task)
{
	return &task->thread_info;
}
#elif !defined(__HAVE_THREAD_FUNCTIONS)
# define task_thread_info(task)	((struct thread_info *)(task)->stack)
#endif

/*
 * find a task by one of its numerical ids
 *
 * find_task_by_pid_ns():
 *      finds a task by its pid in the specified namespace
 * find_task_by_vpid():
 *      finds a task by its virtual pid
 *
 * see also find_vpid() etc in include/linux/pid.h
 */

extern struct task_struct *find_task_by_vpid(pid_t nr);
extern struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns);

/*
 * find a task by its virtual pid and get the task struct
 */
extern struct task_struct *find_get_task_by_vpid(pid_t nr);

extern int wake_up_state(struct task_struct *tsk, unsigned int state);
extern int wake_up_process(struct task_struct *tsk);
extern void wake_up_new_task(struct task_struct *tsk);

#ifdef CONFIG_SMP
extern void kick_process(struct task_struct *tsk);
#else
static inline void kick_process(struct task_struct *tsk) { }
#endif

extern void __set_task_comm(struct task_struct *tsk, const char *from, bool exec);

static inline void set_task_comm(struct task_struct *tsk, const char *from)
{
	__set_task_comm(tsk, from, false);
}

extern char *__get_task_comm(char *to, size_t len, struct task_struct *tsk);
#define get_task_comm(buf, tsk) ({			\
	BUILD_BUG_ON(sizeof(buf) != TASK_COMM_LEN);	\
	__get_task_comm(buf, sizeof(buf), tsk);		\
})

#ifdef CONFIG_SMP

/*
 * IAMROOT, 2023.01.07:
 * - 별거안한다. reschedule은 interrupt를 빠져나가면서 수행하기 때문에
 *   reschedule ipi를 받았다는 명목상의 이유로 호출할뿐이다.
 */
static __always_inline void scheduler_ipi(void)
{
	/*
	 * Fold TIF_NEED_RESCHED into the preempt_count; anybody setting
	 * TIF_NEED_RESCHED remotely (for the first time) will also send
	 * this IPI.
	 */
	preempt_fold_need_resched();
}
extern unsigned long wait_task_inactive(struct task_struct *, unsigned int match_state);
#else
static inline void scheduler_ipi(void) { }
static inline unsigned long wait_task_inactive(struct task_struct *p, unsigned int match_state)
{
	return 1;
}
#endif

/*
 * Set thread flags in other task's structures.
 * See asm/thread_info.h for TIF_xxxx flags available:
 */
static inline void set_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	set_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline void clear_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	clear_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline void update_tsk_thread_flag(struct task_struct *tsk, int flag,
					  bool value)
{
	update_ti_thread_flag(task_thread_info(tsk), flag, value);
}

static inline int test_and_set_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_and_set_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline int test_and_clear_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_and_clear_ti_thread_flag(task_thread_info(tsk), flag);
}

/*
 * IAMROOT, 2023.01.07:
 * - @flag가 있으면 return 1. 없으면 return 0
 */
static inline int test_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_ti_thread_flag(task_thread_info(tsk), flag);
}

/*
 * IAMROOT, 2023.01.07:
 * - @tsk에 set TIF_NEED_RESCHED. reschedule 요청.
 */
static inline void set_tsk_need_resched(struct task_struct *tsk)
{
	set_tsk_thread_flag(tsk,TIF_NEED_RESCHED);
}


/*
 * IAMROOT, 2023.01.28:
 * - TIF_NEED_RESCHED clear.
 */
static inline void clear_tsk_need_resched(struct task_struct *tsk)
{
	clear_tsk_thread_flag(tsk,TIF_NEED_RESCHED);
}

/*
 * IAMROOT, 2023.01.07:
 * - TIF_NEED_RESCHED이 있으면 return 1. 없으면 return 0.
 * - resched 요청이 있는지 확인한다.
 */
static inline int test_tsk_need_resched(struct task_struct *tsk)
{
	return unlikely(test_tsk_thread_flag(tsk,TIF_NEED_RESCHED));
}

/*
 * cond_resched() and cond_resched_lock(): latency reduction via
 * explicit rescheduling in places that are safe. The return
 * value indicates whether a reschedule was done in fact.
 * cond_resched_lock() will drop the spinlock before scheduling,
 */
#if !defined(CONFIG_PREEMPTION) || defined(CONFIG_PREEMPT_DYNAMIC)
extern int __cond_resched(void);

#ifdef CONFIG_PREEMPT_DYNAMIC

DECLARE_STATIC_CALL(cond_resched, __cond_resched);

static __always_inline int _cond_resched(void)
{
	return static_call_mod(cond_resched)();
}

#else

static inline int _cond_resched(void)
{
	return __cond_resched();
}

#endif /* CONFIG_PREEMPT_DYNAMIC */

#else

static inline int _cond_resched(void) { return 0; }

#endif /* !defined(CONFIG_PREEMPTION) || defined(CONFIG_PREEMPT_DYNAMIC) */

/*
 * IAMROOT, 2021.12.18:
 * - CONFIG_PREEMPT_VOLUNTARY일 경우만 동작한다.
 * - PREEMPT가 안되는 os의 경우 잠시 sleep하여 다른 thread한테 스케쥴자원을
 *   양보하기 위함이다.
 */
#define cond_resched() ({			\
	___might_sleep(__FILE__, __LINE__, 0);	\
	_cond_resched();			\
})

extern int __cond_resched_lock(spinlock_t *lock);
extern int __cond_resched_rwlock_read(rwlock_t *lock);
extern int __cond_resched_rwlock_write(rwlock_t *lock);

#define cond_resched_lock(lock) ({				\
	___might_sleep(__FILE__, __LINE__, PREEMPT_LOCK_OFFSET);\
	__cond_resched_lock(lock);				\
})

#define cond_resched_rwlock_read(lock) ({			\
	__might_sleep(__FILE__, __LINE__, PREEMPT_LOCK_OFFSET);	\
	__cond_resched_rwlock_read(lock);			\
})

#define cond_resched_rwlock_write(lock) ({			\
	__might_sleep(__FILE__, __LINE__, PREEMPT_LOCK_OFFSET);	\
	__cond_resched_rwlock_write(lock);			\
})

static inline void cond_resched_rcu(void)
{
#if defined(CONFIG_DEBUG_ATOMIC_SLEEP) || !defined(CONFIG_PREEMPT_RCU)
	rcu_read_unlock();
	cond_resched();
	rcu_read_lock();
#endif
}

/*
 * Does a critical section need to be broken due to another
 * task waiting?: (technically does not depend on CONFIG_PREEMPTION,
 * but a general need for low latency)
 */
static inline int spin_needbreak(spinlock_t *lock)
{
#ifdef CONFIG_PREEMPTION
	return spin_is_contended(lock);
#else
	return 0;
#endif
}

/*
 * Check if a rwlock is contended.
 * Returns non-zero if there is another task waiting on the rwlock.
 * Returns zero if the lock is not contended or the system / underlying
 * rwlock implementation does not support contention detection.
 * Technically does not depend on CONFIG_PREEMPTION, but a general need
 * for low latency.
 */
static inline int rwlock_needbreak(rwlock_t *lock)
{
#ifdef CONFIG_PREEMPTION
	return rwlock_is_contended(lock);
#else
	return 0;
#endif
}

/*
 * IAMROOT, 2023.03.15:
 * @return 1 : TIF_NEED_RESCHED이 curr에 있는 경우.
 *         0 : 없는경우.
 */
static __always_inline bool need_resched(void)
{
	return unlikely(tif_need_resched());
}

/*
 * Wrappers for p->thread_info->cpu access. No-op on UP.
 */
#ifdef CONFIG_SMP

static inline unsigned int task_cpu(const struct task_struct *p)
{
#ifdef CONFIG_THREAD_INFO_IN_TASK
	return READ_ONCE(p->cpu);
#else
	return READ_ONCE(task_thread_info(p)->cpu);
#endif
}

extern void set_task_cpu(struct task_struct *p, unsigned int cpu);

#else

static inline unsigned int task_cpu(const struct task_struct *p)
{
	return 0;
}

static inline void set_task_cpu(struct task_struct *p, unsigned int cpu)
{
}

#endif /* CONFIG_SMP */

extern bool sched_task_on_rq(struct task_struct *p);

/*
 * In order to reduce various lock holder preemption latencies provide an
 * interface to see if a vCPU is currently running or not.
 *
 * This allows us to terminate optimistic spin loops and block, analogous to
 * the native optimistic spin heuristic of testing if the lock owner task is
 * running or not.
 */
#ifndef vcpu_is_preempted
static inline bool vcpu_is_preempted(int cpu)
{
	return false;
}
#endif

extern long sched_setaffinity(pid_t pid, const struct cpumask *new_mask);
extern long sched_getaffinity(pid_t pid, struct cpumask *mask);

#ifndef TASK_SIZE_OF
#define TASK_SIZE_OF(tsk)	TASK_SIZE
#endif

#ifdef CONFIG_SMP
/* Returns effective CPU energy utilization, as seen by the scheduler */
unsigned long sched_cpu_util(int cpu, unsigned long max);
#endif /* CONFIG_SMP */

#ifdef CONFIG_RSEQ

/*
 * Map the event mask on the user-space ABI enum rseq_cs_flags
 * for direct mask checks.
 */
enum rseq_event_mask_bits {
	RSEQ_EVENT_PREEMPT_BIT	= RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT,
	RSEQ_EVENT_SIGNAL_BIT	= RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT,
	RSEQ_EVENT_MIGRATE_BIT	= RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT,
};

enum rseq_event_mask {
	RSEQ_EVENT_PREEMPT	= (1U << RSEQ_EVENT_PREEMPT_BIT),
	RSEQ_EVENT_SIGNAL	= (1U << RSEQ_EVENT_SIGNAL_BIT),
	RSEQ_EVENT_MIGRATE	= (1U << RSEQ_EVENT_MIGRATE_BIT),
};

static inline void rseq_set_notify_resume(struct task_struct *t)
{
	if (t->rseq)
		set_tsk_thread_flag(t, TIF_NOTIFY_RESUME);
}

void __rseq_handle_notify_resume(struct ksignal *sig, struct pt_regs *regs);

static inline void rseq_handle_notify_resume(struct ksignal *ksig,
					     struct pt_regs *regs)
{
	if (current->rseq)
		__rseq_handle_notify_resume(ksig, regs);
}

static inline void rseq_signal_deliver(struct ksignal *ksig,
				       struct pt_regs *regs)
{
	preempt_disable();
	__set_bit(RSEQ_EVENT_SIGNAL_BIT, &current->rseq_event_mask);
	preempt_enable();
	rseq_handle_notify_resume(ksig, regs);
}

/* rseq_preempt() requires preemption to be disabled. */
/*
 * IAMROOT, 2023.01.28:
 * - PASS
 * - Gitblame 참고 (restartable sequences system call)
 *   rseq systemcall을 위한것
 */
static inline void rseq_preempt(struct task_struct *t)
{
	__set_bit(RSEQ_EVENT_PREEMPT_BIT, &t->rseq_event_mask);
	rseq_set_notify_resume(t);
}

/* rseq_migrate() requires preemption to be disabled. */
/*
 * IAMROOT, 2023.04.01:
 * - PASS
 */
static inline void rseq_migrate(struct task_struct *t)
{
	__set_bit(RSEQ_EVENT_MIGRATE_BIT, &t->rseq_event_mask);
	rseq_set_notify_resume(t);
}

/*
 * If parent process has a registered restartable sequences area, the
 * child inherits. Unregister rseq for a clone with CLONE_VM set.
 */
static inline void rseq_fork(struct task_struct *t, unsigned long clone_flags)
{
	if (clone_flags & CLONE_VM) {
		t->rseq = NULL;
		t->rseq_sig = 0;
		t->rseq_event_mask = 0;
	} else {
		t->rseq = current->rseq;
		t->rseq_sig = current->rseq_sig;
		t->rseq_event_mask = current->rseq_event_mask;
	}
}

static inline void rseq_execve(struct task_struct *t)
{
	t->rseq = NULL;
	t->rseq_sig = 0;
	t->rseq_event_mask = 0;
}

#else

static inline void rseq_set_notify_resume(struct task_struct *t)
{
}
static inline void rseq_handle_notify_resume(struct ksignal *ksig,
					     struct pt_regs *regs)
{
}
static inline void rseq_signal_deliver(struct ksignal *ksig,
				       struct pt_regs *regs)
{
}
static inline void rseq_preempt(struct task_struct *t)
{
}
static inline void rseq_migrate(struct task_struct *t)
{
}
static inline void rseq_fork(struct task_struct *t, unsigned long clone_flags)
{
}
static inline void rseq_execve(struct task_struct *t)
{
}

#endif

#ifdef CONFIG_DEBUG_RSEQ

void rseq_syscall(struct pt_regs *regs);

#else

static inline void rseq_syscall(struct pt_regs *regs)
{
}

#endif

const struct sched_avg *sched_trace_cfs_rq_avg(struct cfs_rq *cfs_rq);
char *sched_trace_cfs_rq_path(struct cfs_rq *cfs_rq, char *str, int len);
int sched_trace_cfs_rq_cpu(struct cfs_rq *cfs_rq);

const struct sched_avg *sched_trace_rq_avg_rt(struct rq *rq);
const struct sched_avg *sched_trace_rq_avg_dl(struct rq *rq);
const struct sched_avg *sched_trace_rq_avg_irq(struct rq *rq);

int sched_trace_rq_cpu(struct rq *rq);
int sched_trace_rq_cpu_capacity(struct rq *rq);
int sched_trace_rq_nr_running(struct rq *rq);

const struct cpumask *sched_trace_rd_span(struct root_domain *rd);

#ifdef CONFIG_SCHED_CORE
extern void sched_core_free(struct task_struct *tsk);
extern void sched_core_fork(struct task_struct *p);
extern int sched_core_share_pid(unsigned int cmd, pid_t pid, enum pid_type type,
				unsigned long uaddr);
#else
static inline void sched_core_free(struct task_struct *tsk) { }
static inline void sched_core_fork(struct task_struct *p) { }
#endif

#endif
