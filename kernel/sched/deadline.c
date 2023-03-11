// SPDX-License-Identifier: GPL-2.0
/*
 * Deadline Scheduling Class (SCHED_DEADLINE)
 *
 * Earliest Deadline First (EDF) + Constant Bandwidth Server (CBS).
 *
 * Tasks that periodically executes their instances for less than their
 * runtime won't miss any of their deadlines.
 * Tasks that are not periodic or sporadic or that tries to execute more
 * than their reserved bandwidth will be slowed down (and may potentially
 * miss some of their deadlines), and won't affect any other task.
 *
 * Copyright (C) 2012 Dario Faggioli <raistlin@linux.it>,
 *                    Juri Lelli <juri.lelli@gmail.com>,
 *                    Michael Trimarchi <michael@amarulasolutions.com>,
 *                    Fabio Checconi <fchecconi@gmail.com>
 */
#include "sched.h"
#include "pelt.h"

struct dl_bandwidth def_dl_bandwidth;

static inline struct task_struct *dl_task_of(struct sched_dl_entity *dl_se)
{
	return container_of(dl_se, struct task_struct, dl);
}

static inline struct rq *rq_of_dl_rq(struct dl_rq *dl_rq)
{
	return container_of(dl_rq, struct rq, dl);
}

static inline struct dl_rq *dl_rq_of_se(struct sched_dl_entity *dl_se)
{
	struct task_struct *p = dl_task_of(dl_se);
	struct rq *rq = task_rq(p);

	return &rq->dl;
}

static inline int on_dl_rq(struct sched_dl_entity *dl_se)
{
	return !RB_EMPTY_NODE(&dl_se->rb_node);
}

#ifdef CONFIG_RT_MUTEXES
static inline struct sched_dl_entity *pi_of(struct sched_dl_entity *dl_se)
{
	return dl_se->pi_se;
}

/*
 * IAMROOT, 2023.03.04:
 * - 자기자신이면 boost(priority inversion)이 안된상태(pi_se 주석참고).
 *   이므로 자기자신과 비교해서 boost여부를 판단한다.
 */
static inline bool is_dl_boosted(struct sched_dl_entity *dl_se)
{
	return pi_of(dl_se) != dl_se;
}
#else
static inline struct sched_dl_entity *pi_of(struct sched_dl_entity *dl_se)
{
	return dl_se;
}

static inline bool is_dl_boosted(struct sched_dl_entity *dl_se)
{
	return false;
}
#endif

#ifdef CONFIG_SMP
static inline struct dl_bw *dl_bw_of(int i)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
			 "sched RCU must be held");
	return &cpu_rq(i)->rd->dl_bw;
}

static inline int dl_bw_cpus(int i)
{
	struct root_domain *rd = cpu_rq(i)->rd;
	int cpus;

	RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
			 "sched RCU must be held");

	if (cpumask_subset(rd->span, cpu_active_mask))
		return cpumask_weight(rd->span);

	cpus = 0;

	for_each_cpu_and(i, rd->span, cpu_active_mask)
		cpus++;

	return cpus;
}

static inline unsigned long __dl_bw_capacity(int i)
{
	struct root_domain *rd = cpu_rq(i)->rd;
	unsigned long cap = 0;

	RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
			 "sched RCU must be held");

	for_each_cpu_and(i, rd->span, cpu_active_mask)
		cap += capacity_orig_of(i);

	return cap;
}

/*
 * XXX Fix: If 'rq->rd == def_root_domain' perform AC against capacity
 * of the CPU the task is running on rather rd's \Sum CPU capacity.
 */
static inline unsigned long dl_bw_capacity(int i)
{
	if (!static_branch_unlikely(&sched_asym_cpucapacity) &&
	    capacity_orig_of(i) == SCHED_CAPACITY_SCALE) {
		return dl_bw_cpus(i) << SCHED_CAPACITY_SHIFT;
	} else {
		return __dl_bw_capacity(i);
	}
}

static inline bool dl_bw_visited(int cpu, u64 gen)
{
	struct root_domain *rd = cpu_rq(cpu)->rd;

	if (rd->visit_gen == gen)
		return true;

	rd->visit_gen = gen;
	return false;
}
#else
static inline struct dl_bw *dl_bw_of(int i)
{
	return &cpu_rq(i)->dl.dl_bw;
}

static inline int dl_bw_cpus(int i)
{
	return 1;
}

static inline unsigned long dl_bw_capacity(int i)
{
	return SCHED_CAPACITY_SCALE;
}

static inline bool dl_bw_visited(int cpu, u64 gen)
{
	return false;
}
#endif

/*
 * IAMROOT, 2023.03.04:
 * - @dl_rq에 dl_bw를 running_bw로 추가한다.
 */
static inline
void __add_running_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->running_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->running_bw += dl_bw;
	SCHED_WARN_ON(dl_rq->running_bw < old); /* overflow */
	SCHED_WARN_ON(dl_rq->running_bw > dl_rq->this_bw);
	/* kick cpufreq (see the comment in kernel/sched/sched.h). */
	cpufreq_update_util(rq_of_dl_rq(dl_rq), 0);
}

static inline
void __sub_running_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->running_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->running_bw -= dl_bw;
	SCHED_WARN_ON(dl_rq->running_bw > old); /* underflow */
	if (dl_rq->running_bw > old)
		dl_rq->running_bw = 0;
	/* kick cpufreq (see the comment in kernel/sched/sched.h). */
	cpufreq_update_util(rq_of_dl_rq(dl_rq), 0);
}

/*
 * IAMROOT, 2023.03.04:
 * - @dl_rq의 this_bw에 dl_bw를 추가한다.
 */
static inline
void __add_rq_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->this_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->this_bw += dl_bw;
	SCHED_WARN_ON(dl_rq->this_bw < old); /* overflow */
}

static inline
void __sub_rq_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->this_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->this_bw -= dl_bw;
	SCHED_WARN_ON(dl_rq->this_bw > old); /* underflow */
	if (dl_rq->this_bw > old)
		dl_rq->this_bw = 0;
	SCHED_WARN_ON(dl_rq->running_bw > dl_rq->this_bw);
}

/*
 * IAMROOT, 2023.03.04:
 * - @dl_rq의 this bw에 @dl_se의 bw를 추가한다.
 */
static inline
void add_rq_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	if (!dl_entity_is_special(dl_se))
		__add_rq_bw(dl_se->dl_bw, dl_rq);
}

static inline
void sub_rq_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	if (!dl_entity_is_special(dl_se))
		__sub_rq_bw(dl_se->dl_bw, dl_rq);
}

/*
 * IAMROOT, 2023.03.04:
 * - @dl_rq의 running_bw에 @dl_bw을 추가한다.
 */
static inline
void add_running_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	if (!dl_entity_is_special(dl_se))
		__add_running_bw(dl_se->dl_bw, dl_rq);
}

static inline
void sub_running_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	if (!dl_entity_is_special(dl_se))
		__sub_running_bw(dl_se->dl_bw, dl_rq);
}

static void dl_change_utilization(struct task_struct *p, u64 new_bw)
{
	struct rq *rq;

	BUG_ON(p->dl.flags & SCHED_FLAG_SUGOV);

	if (task_on_rq_queued(p))
		return;

	rq = task_rq(p);
	if (p->dl.dl_non_contending) {
		sub_running_bw(&p->dl, &rq->dl);
		p->dl.dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be canceled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		if (hrtimer_try_to_cancel(&p->dl.inactive_timer) == 1)
			put_task_struct(p);
	}
	__sub_rq_bw(p->dl.dl_bw, &rq->dl);
	__add_rq_bw(new_bw, &rq->dl);
}

/*
 * The utilization of a task cannot be immediately removed from
 * the rq active utilization (running_bw) when the task blocks.
 * Instead, we have to wait for the so called "0-lag time".
 *
 * If a task blocks before the "0-lag time", a timer (the inactive
 * timer) is armed, and running_bw is decreased when the timer
 * fires.
 *
 * If the task wakes up again before the inactive timer fires,
 * the timer is canceled, whereas if the task wakes up after the
 * inactive timer fired (and running_bw has been decreased) the
 * task's utilization has to be added to running_bw again.
 * A flag in the deadline scheduling entity (dl_non_contending)
 * is used to avoid race conditions between the inactive timer handler
 * and task wakeups.
 *
 * The following diagram shows how running_bw is updated. A task is
 * "ACTIVE" when its utilization contributes to running_bw; an
 * "ACTIVE contending" task is in the TASK_RUNNING state, while an
 * "ACTIVE non contending" task is a blocked task for which the "0-lag time"
 * has not passed yet. An "INACTIVE" task is a task for which the "0-lag"
 * time already passed, which does not contribute to running_bw anymore.
 *                              +------------------+
 *             wakeup           |    ACTIVE        |
 *          +------------------>+   contending     |
 *          | add_running_bw    |                  |
 *          |                   +----+------+------+
 *          |                        |      ^
 *          |                dequeue |      |
 * +--------+-------+                |      |
 * |                |   t >= 0-lag   |      | wakeup
 * |    INACTIVE    |<---------------+      |
 * |                | sub_running_bw |      |
 * +--------+-------+                |      |
 *          ^                        |      |
 *          |              t < 0-lag |      |
 *          |                        |      |
 *          |                        V      |
 *          |                   +----+------+------+
 *          | sub_running_bw    |    ACTIVE        |
 *          +-------------------+                  |
 *            inactive timer    |  non contending  |
 *            fired             +------------------+
 *
 * The task_non_contending() function is invoked when a task
 * blocks, and checks if the 0-lag time already passed or
 * not (in the first case, it directly updates running_bw;
 * in the second case, it arms the inactive timer).
 *
 * The task_contending() function is invoked when a task wakes
 * up, and checks if the task is still in the "ACTIVE non contending"
 * state or not (in the second case, it updates running_bw).
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   작업이 차단되면 작업의 사용률을 rq 활성 사용률(running_bw)에서 즉시 제거할 수
 *   없습니다. 대신 소위 "0 지연 시간"을 기다려야 합니다. "0-지연 시간" 이전에 작업이
 *   차단되면 타이머(비활성 타이머)가 활성화되고 타이머가 실행될 때 running_bw가
 *   감소합니다. 비활성 타이머가 실행되기 전에 작업이 다시 깨어나면 타이머가 취소되는
 *   반면, 비활성 타이머가 실행된 후 작업이 깨어나면(및 running_bw가 감소됨) 작업의
 *   활용도를 running_bw에 다시 추가해야 합니다. 데드라인 스케줄링
 *   엔터티(dl_non_contending)의 플래그는 비활성 타이머 핸들러와 태스크 웨이크업
 *   사이의 경쟁 조건을 방지하는 데 사용됩니다. 다음 다이어그램은 running_bw가
 *   업데이트되는 방식을 보여줍니다. 작업의 활용도가 running_bw에 기여할 때 작업은
 *   "활성"입니다. "ACTIVE 경합" 작업은 TASK_RUNNING 상태인 반면, "ACTIVE 비경합"
 *   작업은 "0 지연 시간"이 아직 지나지 않은 차단된 작업입니다. "INACTIVE" 작업은
 *   "0-lag" 시간이 이미 경과한 작업으로 더 이상 running_bw에 기여하지 않습니다.
 *                              +------------------+
 *             wakeup           |    ACTIVE        |
 *          +------------------>+   contending     |
 *          | add_running_bw    |                  |
 *          |                   +----+------+------+
 *          |                        |      ^
 *          |                dequeue |      |
 * +--------+-------+                |      |
 * |                |   t >= 0-lag   |      | wakeup
 * |    INACTIVE    |<---------------+      |
 * |                | sub_running_bw |      |
 * +--------+-------+                |      |
 *          ^                        |      |
 *          |              t < 0-lag |      |
 *          |                        |      |
 *          |                        V      |
 *          |                   +----+------+------+
 *          | sub_running_bw    |    ACTIVE        |
 *          +-------------------+                  |
 *            inactive timer    |  non contending  |
 *            fired             +------------------+
 *
 *   task_non_contending() 함수는 작업이 차단될 때 호출되며 0 지연 시간이 이미
 *   경과했는지 확인합니다(첫 번째 경우 running_bw를 직접 업데이트하고 두 번째 경우
 *   비활성 타이머를 준비함). task_contending() 함수는 작업이 깨어날 때 호출되며
 *   작업이 여전히 "ACTIVE 비경합" 상태인지 확인합니다(두 번째 경우 running_bw를
 *   업데이트함).
 *
 * - 1) active contending +- t >= 0-lag -> 3) inactive
 *                        |
 *                        +- t < 0-lag  -> 2) active non contending 
 *                                             |
 *                                             +-- timer -> 3) inactive
 *
 * - contending task인 경우 진입하여 0-lag을 계산한다.
 *   1. 0-lag < 0
 *      inactive로 전환한다.
 *   2. 0-lag >= 0
 *      non contending으로 전환하고 inactive timer를 0-lag후에 동작하도록 
 *      예약한다.
 */
static void task_non_contending(struct task_struct *p)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct hrtimer *timer = &dl_se->inactive_timer;
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);
	s64 zerolag_time;

	/*
	 * If this is a non-deadline task that has been boosted,
	 * do nothing
	 */
/*
 * IAMROOT, 2023.03.04:
 * - dl task가 아니게됬다. return.
 */
	if (dl_se->dl_runtime == 0)
		return;

	if (dl_entity_is_special(dl_se))
		return;

/*
 * IAMROOT, 2023.03.04:
 * - 이미 non contending인데 이 함수를 호출하는건 잘못된것이다.
 */
	WARN_ON(dl_se->dl_non_contending);

/*
 * IAMROOT, 2023.03.04:
 * - 0-lag 계산.
 *   runtime == 0에 가까울수록 deadline값에 가까워진다.
 *   runtime이 클수록 dealine에서 멀어지거나 0, 음수가 된다.
 */
	zerolag_time = dl_se->deadline -
		 div64_long((dl_se->runtime * dl_se->dl_period),
			dl_se->dl_runtime);

	/*
	 * Using relative times instead of the absolute "0-lag time"
	 * allows to simplify the code
	 */
	zerolag_time -= rq_clock(rq);

	/*
	 * If the "0-lag time" already passed, decrease the active
	 * utilization now, instead of starting a timer
	 */
/*
 * IAMROOT, 2023.03.04:
 * - 1. zerolag_time < 0  or inactive timer가 가동중 
 *   active contending -> inactive
 */
	if ((zerolag_time < 0) || hrtimer_active(&dl_se->inactive_timer)) {
		if (dl_task(p))
			sub_running_bw(dl_se, dl_rq);

/*
 * IAMROOT, 2023.03.04:
 * - 종료중인 task에 대한 처리.
 */
		if (!dl_task(p) || READ_ONCE(p->__state) == TASK_DEAD) {
			struct dl_bw *dl_b = dl_bw_of(task_cpu(p));

			if (READ_ONCE(p->__state) == TASK_DEAD)
				sub_rq_bw(&p->dl, &rq->dl);
			raw_spin_lock(&dl_b->lock);
			__dl_sub(dl_b, p->dl.dl_bw, dl_bw_cpus(task_cpu(p)));
			__dl_clear_params(p);
			raw_spin_unlock(&dl_b->lock);
		}

		return;
	}

/*
 * IAMROOT, 2023.03.04:
 * - zerolag_time >= 0 : active contending -> active non contending
 *                                            + inactive_timer 예약.
 */
	dl_se->dl_non_contending = 1;
	get_task_struct(p);
	hrtimer_start(timer, ns_to_ktime(zerolag_time), HRTIMER_MODE_REL_HARD);
}

/*
 * IAMROOT, 2023.03.04:
 * - task가 contending상태로 전환한다.
 */
static void task_contending(struct sched_dl_entity *dl_se, int flags)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	/*
	 * If this is a non-deadline task that has been boosted,
	 * do nothing
	 */
/*
 * IAMROOT, 2023.03.04:
 * - 더이상 deadline이 아니다. return.
 */
	if (dl_se->dl_runtime == 0)
		return;

	if (flags & ENQUEUE_MIGRATED)
		add_rq_bw(dl_se, dl_rq);

/*
 * IAMROOT, 2023.03.04:
 *             +-- else문 진입시점.
 *             |              
 *             v                +------------------+
 *             wakeup           |    ACTIVE        |
 *          +------------------>+   contending     |
 *          | add_running_bw    |                  |
 *          |                   +----+------+------+
 *          |                        |      ^
 *          |                dequeue |      |
 * +--------+-------+                |      |
 * |                |   t >= 0-lag   |      | wakeup <--- if문 진입시점.
 * |    INACTIVE    |<---------------+      |
 * |                | sub_running_bw |      |
 * +--------+-------+                |      |
 *          ^                        |      |
 *          |              t < 0-lag |      |
 *          |                        |      |
 *          |                        V      |
 *          |                   +----+------+------+
 *          | sub_running_bw    |    ACTIVE        |
 *          +-------------------+                  |
 *            inactive timer    |  non contending  |
 *            fired             +------------------+
 *
 * - 1) active contending +- t >= 0-lag -> 3) inactive
 *                        |
 *                        +- t < 0-lag  -> 2) active non contending 
 *                                             |
 *                                             +-- timer -> 3) inactive
 *
 * - task가 sleep이고, 2) active non contending 상태로 
 *   이때 task가 깨어나서 진입하면 다음과 같이 동작한다.
 *
 *   1. contending 상태가 되므로 non_contending을 0으로 한다.
 *   2. inactive timer가 동작중일수있다.
 *      (active non conteding에서 inactive로 될수있는 상황)
 *      active contending으로 전환해야되니 timer를 취소한다.
 */
	if (dl_se->dl_non_contending) {
		dl_se->dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be canceled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   타이머 핸들러가 현재 실행 중이고 타이머를 취소할 수 없는 경우 
 *   inactive_task_timer()는 dl_not_contending이 설정되지 않았음을 
 *   확인하고 rq의 활성 사용률을 건드리지 않으므로 여전히 안전합니다.
 *
 * - 0-lag timer 취소
 */
		if (hrtimer_try_to_cancel(&dl_se->inactive_timer) == 1)
			put_task_struct(dl_task_of(dl_se));
	} else {
		/*
		 * Since "dl_non_contending" is not set, the
		 * task's utilization has already been removed from
		 * active utilization (either when the task blocked,
		 * when the "inactive timer" fired).
		 * So, add it back.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   dl_non_contending이 설정되지 않았기 때문에 작업의 사용률은 이미 
 *   active 사용률에서 제거되었습니다(작업이 차단되었을 때, 비활성 
 *   타이머가 실행되었을 때).
 *   다시 추가하십시오.
 * - task block이거나 invactive timer가 동작했을때.(3번 구간) running_bw를 증가 
 *   시켜준다.
 */
		add_running_bw(dl_se, dl_rq);
	}
}

static inline int is_leftmost(struct task_struct *p, struct dl_rq *dl_rq)
{
	struct sched_dl_entity *dl_se = &p->dl;

	return dl_rq->root.rb_leftmost == &dl_se->rb_node;
}

static void init_dl_rq_bw_ratio(struct dl_rq *dl_rq);

/*
 * IAMROOT, 2022.11.26:
 * - 초기화.
 */
void init_dl_bandwidth(struct dl_bandwidth *dl_b, u64 period, u64 runtime)
{
	raw_spin_lock_init(&dl_b->dl_runtime_lock);
	dl_b->dl_period = period;
	dl_b->dl_runtime = runtime;
}

/*
 * IAMROOT, 2022.11.26:
 * - load_balance에 사용할 자료구조. 초기화.
 */
void init_dl_bw(struct dl_bw *dl_b)
{
	raw_spin_lock_init(&dl_b->lock);
	raw_spin_lock(&def_dl_bandwidth.dl_runtime_lock);
	if (global_rt_runtime() == RUNTIME_INF)
		dl_b->bw = -1;
	else
		dl_b->bw = to_ratio(global_rt_period(), global_rt_runtime());
	raw_spin_unlock(&def_dl_bandwidth.dl_runtime_lock);
	dl_b->total_bw = 0;
}

/*
 * IAMROOT, 2022.11.26:
 * - dl rq초기화.
 */
void init_dl_rq(struct dl_rq *dl_rq)
{
	dl_rq->root = RB_ROOT_CACHED;

/*
 * IAMROOT, 2022.11.26:
 * - SMP일경우 이미 init_dl_bw가 이전에 진행됫을것이다.
 */
#ifdef CONFIG_SMP
	/* zero means no -deadline tasks */
	dl_rq->earliest_dl.curr = dl_rq->earliest_dl.next = 0;

	dl_rq->dl_nr_migratory = 0;
	dl_rq->overloaded = 0;
	dl_rq->pushable_dl_tasks_root = RB_ROOT_CACHED;
#else
	init_dl_bw(&dl_rq->dl_bw);
#endif

	dl_rq->running_bw = 0;
	dl_rq->this_bw = 0;
	init_dl_rq_bw_ratio(dl_rq);
}

#ifdef CONFIG_SMP

static inline int dl_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->dlo_count);
}

static inline void dl_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->dlo_mask);
	/*
	 * Must be visible before the overload count is
	 * set (as in sched_rt.c).
	 *
	 * Matched by the barrier in pull_dl_task().
	 */
	smp_wmb();
	atomic_inc(&rq->rd->dlo_count);
}

static inline void dl_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	atomic_dec(&rq->rd->dlo_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->dlo_mask);
}

/*
 * IAMROOT, 2023.02.25:
 * - migration 조건인데 overload 설정이 아니면 overload 설정
 *   migration 조건이 아닌데 overload 설정이면 설정 clear
 * - atomic operation을 줄이기 위한 코드 작성
 */
static void update_dl_migration(struct dl_rq *dl_rq)
{
	if (dl_rq->dl_nr_migratory && dl_rq->dl_nr_running > 1) {
		if (!dl_rq->overloaded) {
			dl_set_overload(rq_of_dl_rq(dl_rq));
			dl_rq->overloaded = 1;
		}
	} else if (dl_rq->overloaded) {
		dl_clear_overload(rq_of_dl_rq(dl_rq));
		dl_rq->overloaded = 0;
	}
}

/*
 * IAMROOT, 2023.03.04:
 * - 1. nr migrate 증가.
 *   2. overloaded 갱신.
 */
static void inc_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	struct task_struct *p = dl_task_of(dl_se);

	if (p->nr_cpus_allowed > 1)
		dl_rq->dl_nr_migratory++;

	update_dl_migration(dl_rq);
}

/*
 * IAMROOT, 2023.02.25:
 * - overload 설정
 */
static void dec_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	struct task_struct *p = dl_task_of(dl_se);

	if (p->nr_cpus_allowed > 1)
		dl_rq->dl_nr_migratory--;

	update_dl_migration(dl_rq);
}

/*
 * IAMROOT, 2023.02.27:
 * - node to pushable dl task. node->pushable_dl_tasks의 container task
 */
#define __node_2_pdl(node) \
	rb_entry((node), struct task_struct, pushable_dl_tasks)

static inline bool __pushable_less(struct rb_node *a, const struct rb_node *b)
{
	return dl_entity_preempt(&__node_2_pdl(a)->dl, &__node_2_pdl(b)->dl);
}

/*
 * The list of pushable -deadline task is not a plist, like in
 * sched_rt.c, it is an rb-tree with tasks ordered by deadline.
 */
/*
 * IAMROOT, 2023.03.04:
 * - dl pushable에 @p를 추가한다.(deadline 정렬)
 *   leftmost가 갱신됫으면 2번째 빠른 deadline을 earlist_dl next로 갱신한다.
 */
static void enqueue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct rb_node *leftmost;

	BUG_ON(!RB_EMPTY_NODE(&p->pushable_dl_tasks));

	leftmost = rb_add_cached(&p->pushable_dl_tasks,
				 &rq->dl.pushable_dl_tasks_root,
				 __pushable_less);
	if (leftmost)
		rq->dl.earliest_dl.next = p->dl.deadline;
}

/*
 * IAMROOT, 2023.02.25:
 * - pushable_dl_tasks_root에서 해당 task node를 삭제하고 leftmost인 경우는
 *   dl_rq의 earliest_dl.next 교체
 */
static void dequeue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct dl_rq *dl_rq = &rq->dl;
	struct rb_root_cached *root = &dl_rq->pushable_dl_tasks_root;
	struct rb_node *leftmost;

	if (RB_EMPTY_NODE(&p->pushable_dl_tasks))
		return;

	leftmost = rb_erase_cached(&p->pushable_dl_tasks, root);
	/*
	 * IAMROOT, 2023.02.25:
	 * - pushable_dl_tasks_root의 leftmost가 교체되었으므로
	 *   earliest_dl.next를 교체한다
	 * - NOTE. earliest_dl.curr에는 dl_rq->root.rb_leftmost의
	 *   deadline이 저장된다.
	 */
	if (leftmost)
		dl_rq->earliest_dl.next = __node_2_pdl(leftmost)->dl.deadline;

	RB_CLEAR_NODE(&p->pushable_dl_tasks);
}

/*
 * IAMROOT, 2023.03.04:
 * - pushable을 할 task가 있으면 return 1
 *   아니면 return 0.
 */
static inline int has_pushable_dl_tasks(struct rq *rq)
{
	return !RB_EMPTY_ROOT(&rq->dl.pushable_dl_tasks_root.rb_root);
}

static int push_dl_task(struct rq *rq);

/*
 * IAMROOT, 2023.03.04:
 * - @rq가 online이고, @prev가 deadline이면 return true.
 */
static inline bool need_pull_dl_task(struct rq *rq, struct task_struct *prev)
{
	return rq->online && dl_task(prev);
}

static DEFINE_PER_CPU(struct callback_head, dl_push_head);
static DEFINE_PER_CPU(struct callback_head, dl_pull_head);

static void push_dl_tasks(struct rq *);
static void pull_dl_task(struct rq *);

static inline void deadline_queue_push_tasks(struct rq *rq)
{
	if (!has_pushable_dl_tasks(rq))
		return;

	queue_balance_callback(rq, &per_cpu(dl_push_head, rq->cpu), push_dl_tasks);
}

static inline void deadline_queue_pull_task(struct rq *rq)
{
	queue_balance_callback(rq, &per_cpu(dl_pull_head, rq->cpu), pull_dl_task);
}

static struct rq *find_lock_later_rq(struct task_struct *task, struct rq *rq);

/*
 * IAMROOT, 2023.03.04:
 * - pushable 요청.
 * - PASS
 */
static struct rq *dl_task_offline_migration(struct rq *rq, struct task_struct *p)
{
	struct rq *later_rq = NULL;
	struct dl_bw *dl_b;

	later_rq = find_lock_later_rq(p, rq);
	if (!later_rq) {
		int cpu;

		/*
		 * If we cannot preempt any rq, fall back to pick any
		 * online CPU:
		 */
		cpu = cpumask_any_and(cpu_active_mask, p->cpus_ptr);
		if (cpu >= nr_cpu_ids) {
			/*
			 * Failed to find any suitable CPU.
			 * The task will never come back!
			 */
			BUG_ON(dl_bandwidth_enabled());

			/*
			 * If admission control is disabled we
			 * try a little harder to let the task
			 * run.
			 */
			cpu = cpumask_any(cpu_active_mask);
		}
		later_rq = cpu_rq(cpu);
		double_lock_balance(rq, later_rq);
	}

	if (p->dl.dl_non_contending || p->dl.dl_throttled) {
		/*
		 * Inactive timer is armed (or callback is running, but
		 * waiting for us to release rq locks). In any case, when it
		 * will fire (or continue), it will see running_bw of this
		 * task migrated to later_rq (and correctly handle it).
		 */
		sub_running_bw(&p->dl, &rq->dl);
		sub_rq_bw(&p->dl, &rq->dl);

		add_rq_bw(&p->dl, &later_rq->dl);
		add_running_bw(&p->dl, &later_rq->dl);
	} else {
		sub_rq_bw(&p->dl, &rq->dl);
		add_rq_bw(&p->dl, &later_rq->dl);
	}

	/*
	 * And we finally need to fixup root_domain(s) bandwidth accounting,
	 * since p is still hanging out in the old (now moved to default) root
	 * domain.
	 */
	dl_b = &rq->rd->dl_bw;
	raw_spin_lock(&dl_b->lock);
	__dl_sub(dl_b, p->dl.dl_bw, cpumask_weight(rq->rd->span));
	raw_spin_unlock(&dl_b->lock);

	dl_b = &later_rq->rd->dl_bw;
	raw_spin_lock(&dl_b->lock);
	__dl_add(dl_b, p->dl.dl_bw, cpumask_weight(later_rq->rd->span));
	raw_spin_unlock(&dl_b->lock);

	set_task_cpu(p, later_rq->cpu);
	double_unlock_balance(later_rq, rq);

	return later_rq;
}

#else

static inline
void enqueue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void dequeue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
}

static inline
void dec_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
}

static inline bool need_pull_dl_task(struct rq *rq, struct task_struct *prev)
{
	return false;
}

static inline void pull_dl_task(struct rq *rq)
{
}

static inline void deadline_queue_push_tasks(struct rq *rq)
{
}

static inline void deadline_queue_pull_task(struct rq *rq)
{
}
#endif /* CONFIG_SMP */

static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags);
static void __dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags);
static void check_preempt_curr_dl(struct rq *rq, struct task_struct *p, int flags);

/*
 * We are being explicitly informed that a new instance is starting,
 * and this means that:
 *  - the absolute deadline of the entity has to be placed at
 *    current time + relative deadline;
 *  - the runtime of the entity has to be set to the maximum value.
 *
 * The capability of specifying such event is useful whenever a -deadline
 * entity wants to (try to!) synchronize its behaviour with the scheduler's
 * one, and to (try to!) reconcile itself with its own scheduling
 * parameters.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   새 인스턴스가 시작되고 있음을 명시적으로 알리고 있으며 이는 다음을 
 *   의미합니다.
 * - 엔터티의 절대 기한은 현재 시간 + 상대 기한에 있어야 합니다.
 * - 엔티티의 런타임은 최대값으로 설정되어야 합니다.
 *
 *   이러한 이벤트를 지정하는 기능은 -deadline 엔터티가 자신의 동작을 스케줄러의 
 *   동작과 동기화하고(시도!) 자신의 스케줄링 매개변수로 조정하려고 할 
 *   때마다(시도!) 유용합니다.
 *
 * - 새로 시작하는 dl에 대한 초기화이다.
 * - dl_runtime, dl_deadline을 runtime, deadline으로 설정한다.
 */
static inline void setup_new_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

/*
 * IAMROOT, 2023.03.04:
 * - 새로 setup하는 상황인데 boost되있거나 만료가 안된 deadline이면 말이 
 *   안되는 상황이다. 이를 한번검사한다.
 */
	WARN_ON(is_dl_boosted(dl_se));
	WARN_ON(dl_time_before(rq_clock(rq), dl_se->deadline));

	/*
	 * We are racing with the deadline timer. So, do nothing because
	 * the deadline timer handler will take care of properly recharging
	 * the runtime and postponing the deadline
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   우리는 마감 타이머와 경쟁하고 있습니다. 따라서 기한 타이머 핸들러가 
 *   런타임을 적절하게 재충전하고 기한을 연기하기 때문에 아무것도 하지 마십시오. 
 */
	if (dl_se->dl_throttled)
		return;

	/*
	 * We use the regular wall clock time to set deadlines in the
	 * future; in fact, we must consider execution overheads (time
	 * spent on hardirq context, etc.).
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   우리는 일반 벽시계 시간을 사용하여 미래의 마감일을 설정합니다. 실제로 
 *   우리는 실행 오버헤드(hardirq 컨텍스트에 소요된 시간 등)를 고려해야 합니다.
 */
	dl_se->deadline = rq_clock(rq) + dl_se->dl_deadline;
	dl_se->runtime = dl_se->dl_runtime;
}

/*
 * Pure Earliest Deadline First (EDF) scheduling does not deal with the
 * possibility of a entity lasting more than what it declared, and thus
 * exhausting its runtime.
 *
 * Here we are interested in making runtime overrun possible, but we do
 * not want a entity which is misbehaving to affect the scheduling of all
 * other entities.
 * Therefore, a budgeting strategy called Constant Bandwidth Server (CBS)
 * is used, in order to confine each entity within its own bandwidth.
 *
 * This function deals exactly with that, and ensures that when the runtime
 * of a entity is replenished, its deadline is also postponed. That ensures
 * the overrunning entity can't interfere with other entity in the system and
 * can't make them miss their deadlines. Reasons why this kind of overruns
 * could happen are, typically, a entity voluntarily trying to overcome its
 * runtime, or it just underestimated it during sched_setattr().
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   순수한 EDF(Earliest Deadline First) 스케줄링은 엔티티가 선언한 
 *   것보다 더 오래 지속되어 런타임이 소진될 가능성을 처리하지 않습니다.
 *
 *   여기서 우리는 런타임 오버런을 가능하게 만드는 데 관심이 있지만 다른 
 *   모든 엔터티의 일정에 영향을 미치기 위해 오작동하는 엔터티를 원하지 
 *   않습니다.
 *   따라서 각 엔터티를 자체 대역폭 내로 제한하기 위해 
 *   CBS(Constant Bandwidth Server)라는 예산 책정 전략이 사용됩니다.
 *
 *   이 기능은 이를 정확히 처리하고 엔터티의 런타임이 보충될 때 기한도 
 *   연기되도록 합니다. 이렇게 하면 초과 실행 엔터티가 시스템의 다른 
 *   엔터티를 방해할 수 없고 마감일을 놓치게 할 수 없습니다. 이러한 
 *   종류의 오버런이 발생할 수 있는 이유는 일반적으로 엔터티가 자발적으로 
 *   런타임을 극복하려고 시도하거나 sched_setattr() 중에 런타임을 과소 
 *   평가했기 때문입니다.
 *
 * - period마다 동작하는 timer에 의한 replenish로 수행된다.
 * - 여러개의 task가 있을경우, 일반적으로 max runtime에 의해 나뉘어져 
 *   실행되서 서로 경쟁을 안할것이다.
 *   하지만 경우에 따라 서로 경쟁을 하게 되는 상황이 발생하게 되는게 
 *   그거에 대한 처리를 수행한다.
 *
 * - runtime보충 및 deadline 갱신. yield, throttle flag 초기화.
 */
static void replenish_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	BUG_ON(pi_of(dl_se)->dl_runtime <= 0);

	/*
	 * This could be the case for a !-dl task that is boosted.
	 * Just go with full inherited parameters.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   이것은 부스트된 !-dl task의 경우일 수 있습니다.
 *   전체 상속 매개 변수를 사용하십시오.
 * - 누군가 갑자기 바꿔서 dl이 아니게 된상황이다.
 */
	if (dl_se->dl_deadline == 0) {
/*
 * IAMROOT, 2023.03.04:
 * - pi부스트 중인경우 pi의 deadline을 가져온다. 그렇지 않으면
 *   자기꺼(boost중이 아니면 pi는 자기자신을 가리키므로)를 가져온다.
 * - rt mutex등 이유로 pi가 생긴 경우에 대한 처리.
 */
		dl_se->deadline = rq_clock(rq) + pi_of(dl_se)->dl_deadline;
		dl_se->runtime = pi_of(dl_se)->dl_runtime;
	}

/*
 * IAMROOT, 2023.03.04:
 * - yield중이면 runtime을 없애서 강제로 보충하게 한다.
 */
	if (dl_se->dl_yielded && dl_se->runtime > 0)
		dl_se->runtime = 0;

	/*
	 * We keep moving the deadline away until we get some
	 * available runtime for the entity. This ensures correct
	 * handling of situations where the runtime overrun is
	 * arbitrary large.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   엔터티에 사용할 수 있는 런타임을 얻을 때까지 기한을 계속 미루고 
 *   있습니다. 이렇게 하면 런타임 오버런이 임의로 큰 상황을 올바르게 
 *   처리할 수 있습니다.
 * - runtime이 양수가 될때까지 계속 보충해준다.
 *   여러 period만큼 overrun이 될수있었기 때문에 while로 보충한다.
 */
	while (dl_se->runtime <= 0) {
		dl_se->deadline += pi_of(dl_se)->dl_period;
		dl_se->runtime += pi_of(dl_se)->dl_runtime;
	}

	/*
	 * At this point, the deadline really should be "in
	 * the future" with respect to rq->clock. If it's
	 * not, we are, for some reason, lagging too much!
	 * Anyway, after having warn userspace abut that,
	 * we still try to keep the things running by
	 * resetting the deadline and the budget of the
	 * entity.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   이 시점에서 데드라인은 rq->clock과 관련하여 실제로 미래여야 합니다. 
 *   그렇지 않다면 어떤 이유에서인지 너무 뒤처져 있는 것입니다! 어쨌든 
 *   사용자 공간에 경고를 한 후에도 엔터티의 기한과 예산을 재설정하여 
 *   작업을 계속 실행하려고 합니다.
 *
 * - 보충을 해서 runtime은 양수가 됬지만 deadline이 과거인 경우에 대한
 *   예외 처리. 현재 시각을 기준으로 다시 세팅한다.
 */
	if (dl_time_before(dl_se->deadline, rq_clock(rq))) {
		printk_deferred_once("sched: DL replenish lagged too much\n");
		dl_se->deadline = rq_clock(rq) + pi_of(dl_se)->dl_deadline;
		dl_se->runtime = pi_of(dl_se)->dl_runtime;
	}

	if (dl_se->dl_yielded)
		dl_se->dl_yielded = 0;
	if (dl_se->dl_throttled)
		dl_se->dl_throttled = 0;
}

/*
 * Here we check if --at time t-- an entity (which is probably being
 * [re]activated or, in general, enqueued) can use its remaining runtime
 * and its current deadline _without_ exceeding the bandwidth it is
 * assigned (function returns true if it can't). We are in fact applying
 * one of the CBS rules: when a task wakes up, if the residual runtime
 * over residual deadline fits within the allocated bandwidth, then we
 * can keep the current (absolute) deadline and residual budget without
 * disrupting the schedulability of the system. Otherwise, we should
 * refill the runtime and set the deadline a period in the future,
 * because keeping the current (absolute) deadline of the task would
 * result in breaking guarantees promised to other tasks (refer to
 * Documentation/scheduler/sched-deadline.rst for more information).
 *
 * This function returns true if:
 *
 *   runtime / (deadline - t) > dl_runtime / dl_deadline ,
 *
 * IOW we can't recycle current parameters.
 *
 * Notice that the bandwidth check is done against the deadline. For
 * task with deadline equal to period this is the same of using
 * dl_period instead of dl_deadline in the equation above.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   여기에서 --at time t-- 엔터티(아마 [재]활성화되거나 일반적으로 큐에 추가됨)가 
 *   할당된 대역폭을 _초과하지 않고_ 남은 런타임과 현재 기한을 사용할 수 있는지 
 *   확인합니다(함수는 다음과 같은 경우 true를 반환합니다. 할 수 없습니다). 실제로
 *   CBS 규칙 중 하나를 적용하고 있습니다.
 *   작업이 깨어날 때 잔여 기한을 초과한 잔여 런타임이 할당된 대역폭에 맞으면 
 *   시스템의 일정 가능성을 방해하지 않고 현재(절대) 기한과 잔여 예산을 유지할 수 
 *   있습니다. 그렇지 않으면 런타임을 다시 채우고 기한을 미래의 기간으로 설정해야 
 *   합니다. 작업의 현재(절대적인) 기한을 유지하면 다른 작업에 대한 약속이 깨질 
 *   수 있기 때문입니다(Documentation/scheduler/sched-deadline.rst 참조).
 *   추가 정보).
 *
 *   이 함수는 다음과 같은 경우 true를 반환합니다.
 *
 *   runtime / (deadline - t) > dl_runtime / dl_deadline ,
 *
 *   다른말로 말하자면, 현재 매개변수를 재활용할 수 없습니다.
 *
 *   대역폭 검사는 기한에 맞춰 수행됩니다. 기한이 기간과 같은 작업의 경우 이는 
 *   위의 방정식에서 dl_deadline 대신 dl_period를 사용하는 것과 동일합니다.
 *
 * - 설정된 런타임 비율에 비해 남은 런타임 비율이 크면 overflow라고 판단한다.
 *
 *   남은 런타임     설정 런타임 
 *   -----------  >  --------------
 *   남은 시간       설정 deadline
 *
 * - 곱셈을 하기 때문에 us로 down scale하여 계산한다.
 * - runtime에 비해 시간이 많이 소모되면 overflow가 된다.
 * - overflow가 된다는건 결국 만료시간 이내에 runtime이 소모되기 힘든 상황을
 *   의미한다.(runtime이 소모되지 않고 시간이 계속 흐르는 상태)
 */
static bool dl_entity_overflow(struct sched_dl_entity *dl_se, u64 t)
{
	u64 left, right;

	/*
	 * left and right are the two sides of the equation above,
	 * after a bit of shuffling to use multiplications instead
	 * of divisions.
	 *
	 * Note that none of the time values involved in the two
	 * multiplications are absolute: dl_deadline and dl_runtime
	 * are the relative deadline and the maximum runtime of each
	 * instance, runtime is the runtime left for the last instance
	 * and (deadline - t), since t is rq->clock, is the time left
	 * to the (absolute) deadline. Even if overflowing the u64 type
	 * is very unlikely to occur in both cases, here we scale down
	 * as we want to avoid that risk at all. Scaling down by 10
	 * means that we reduce granularity to 1us. We are fine with it,
	 * since this is only a true/false check and, anyway, thinking
	 * of anything below microseconds resolution is actually fiction
	 * (but still we want to give the user that illusion >;).
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   왼쪽과 오른쪽은 나눗셈 대신 곱셈을 사용하기 위해 약간 섞은 후 위 방정식의 
 *   양면입니다.
 *
 *   두 가지 곱셈에 포함된 시간 값은 절대 값이 아닙니다. dl_deadline 및 
 *   dl_runtime은 각 인스턴스의 상대적 데드라인 및 최대 런타임이고, runtime은 
 *   마지막 인스턴스에 남은 런타임이며 (deadline - t)는 t가 rq->clock이므로 
 *   (절대) 데드라인까지 남은 시간입니다. 두 경우 모두 u64 유형의 오버플로가 
 *   발생할 가능성이 매우 낮더라도 이러한 위험을 전혀 피하고 싶기 때문에 여기서는 
 *   규모를 줄입니다. 10으로 축소한다는 것은 세분성을 1us로 줄이는 것을 
 *   의미합니다. 이것은 단지 참/거짓 검사일 뿐이고 어쨌든 마이크로초 이하의 
 *   해상도를 생각하는 것은 실제로 허구이기 때문에 괜찮습니다(하지만 여전히 
 *   사용자에게 환상을 주고 싶습니다 >;).
 *
 * - t 
 */
	left = (pi_of(dl_se)->dl_deadline >> DL_SCALE) * (dl_se->runtime >> DL_SCALE);
	right = ((dl_se->deadline - t) >> DL_SCALE) *
		(pi_of(dl_se)->dl_runtime >> DL_SCALE);

	return dl_time_before(right, left);
}

/*
 * Revised wakeup rule [1]: For self-suspending tasks, rather then
 * re-initializing task's runtime and deadline, the revised wakeup
 * rule adjusts the task's runtime to avoid the task to overrun its
 * density.
 *
 * Reasoning: a task may overrun the density if:
 *    runtime / (deadline - t) > dl_runtime / dl_deadline
 *
 * Therefore, runtime can be adjusted to:
 *     runtime = (dl_runtime / dl_deadline) * (deadline - t)
 *
 * In such way that runtime will be equal to the maximum density
 * the task can use without breaking any rule.
 *
 * [1] Luca Abeni, Giuseppe Lipari, and Juri Lelli. 2015. Constant
 * bandwidth server revisited. SIGBED Rev. 11, 4 (January 2015), 19-24.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   개정된 웨이크업 규칙[1]: 자체 일시 중단 작업의 경우 작업의 실행 시간과 
 *   기한을 다시 초기화하는 대신 수정된 깨우기 규칙은 작업의 실행 시간을 조정하여 
 *   작업이 밀도를 초과하지 않도록 합니다.
 *
 *   추리: 다음과 같은 경우 작업이 밀도를 초과할 수 있습니다.
 *    runtime / (deadline - t) > dl_runtime / dl_deadline
 *
 *   런타임은 다음과 같이 조정할 수 있습니다.
 *     runtime = (dl_runtime / dl_deadline) * (deadline - t)
 *
 *  이러한 방식으로 런타임은 작업이 규칙을 위반하지 않고 사용할 수 있는 최대 
 *  밀도와 동일합니다.
 *
 *  [1] Luca Abeni, Giuseppe Lipari, and Juri Lelli. 2015. Constant
 *  bandwidth server revisited. SIGBED Rev. 11, 4 (January 2015), 19-24.
 *
 * - original cbs가 아닌 task가 overflow등의 상황에서 runtime을 
 *   재충전해야될때에는 특정 비율에 맞춰 재충전한다.
 */
static void
update_dl_revised_wakeup(struct sched_dl_entity *dl_se, struct rq *rq)
{
	u64 laxity = dl_se->deadline - rq_clock(rq);

	/*
	 * If the task has deadline < period, and the deadline is in the past,
	 * it should already be throttled before this check.
	 *
	 * See update_dl_entity() comments for further details.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   작업에 deadline < period이 있고 기한이 과거인 경우 이 확인 전에 이미 
 *   제한되어야 합니다.
 *
 *   자세한 내용은 update_dl_entity() 주석을 참조하십시오.
 *
 * - runtime = dl_runtime / dl_deadline * (deadline - t)
 */
	WARN_ON(dl_time_before(dl_se->deadline, rq_clock(rq)));

	dl_se->runtime = (dl_se->dl_density * laxity) >> BW_SHIFT;
}

/*
 * Regarding the deadline, a task with implicit deadline has a relative
 * deadline == relative period. A task with constrained deadline has a
 * relative deadline <= relative period.
 *
 * We support constrained deadline tasks. However, there are some restrictions
 * applied only for tasks which do not have an implicit deadline. See
 * update_dl_entity() to know more about such restrictions.
 *
 * The dl_is_implicit() returns true if the task has an implicit deadline.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   기한과 관련하여 암시적 기한이 있는 작업에는 상대 기한 == 상대 기간이 
 *   있습니다. 기한이 제한된 작업은 상대 기한 <= 상대 기간을 가집니다.
 *
 *   제한된 기한 작업을 지원합니다. 그러나 암시적 기한이 없는 작업에만 
 *   적용되는 몇 가지 제한 사항이 있습니다. 이러한 제한 사항에 대한 
 *   자세한 내용은 update_dl_entity()를 참조하십시오.
 *
 *   작업에 암시적 기한이 있는 경우 dl_is_implicit()는 true를 반환합니다.
 *
 * - 일반적인 deadline인지 확인한다. ( dl_deadline == dl_period)
 * - 특정 task에 임의적인 우선순위를 안준경우이다.
 */
static inline bool dl_is_implicit(struct sched_dl_entity *dl_se)
{
	return dl_se->dl_deadline == dl_se->dl_period;
}

/*
 * When a deadline entity is placed in the runqueue, its runtime and deadline
 * might need to be updated. This is done by a CBS wake up rule. There are two
 * different rules: 1) the original CBS; and 2) the Revisited CBS.
 *
 * When the task is starting a new period, the Original CBS is used. In this
 * case, the runtime is replenished and a new absolute deadline is set.
 *
 * When a task is queued before the begin of the next period, using the
 * remaining runtime and deadline could make the entity to overflow, see
 * dl_entity_overflow() to find more about runtime overflow. When such case
 * is detected, the runtime and deadline need to be updated.
 *
 * If the task has an implicit deadline, i.e., deadline == period, the Original
 * CBS is applied. the runtime is replenished and a new absolute deadline is
 * set, as in the previous cases.
 *
 * However, the Original CBS does not work properly for tasks with
 * deadline < period, which are said to have a constrained deadline. By
 * applying the Original CBS, a constrained deadline task would be able to run
 * runtime/deadline in a period. With deadline < period, the task would
 * overrun the runtime/period allowed bandwidth, breaking the admission test.
 *
 * In order to prevent this misbehave, the Revisited CBS is used for
 * constrained deadline tasks when a runtime overflow is detected. In the
 * Revisited CBS, rather than replenishing & setting a new absolute deadline,
 * the remaining runtime of the task is reduced to avoid runtime overflow.
 * Please refer to the comments update_dl_revised_wakeup() function to find
 * more about the Revised CBS rule.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   deadline 엔터티가 실행 대기열에 배치되면 해당 런타임 및 deadline을 업데이트해야 할 
 *   수 있습니다. 이것은 CBS 깨우기 규칙에 의해 수행됩니다. 두 가지 다른 규칙이 
 *   있습니다. 1) original CBS 2) Revisited CBS.
 *
 *   task이 새 period을 시작하는 경우 original CBS가 사용됩니다. 이 경우 런타임이 
 *   보충되고 새로운 절대 deadline이 설정됩니다.
 *
 *   task이 다음 period이 시작되기 전에 대기열에 있는 경우 남은 런타임과 deadline을 
 *   사용하면 엔터티가 오버플로될 수 있습니다. 런타임 오버플로에 대한 자세한 
 *   내용은 dl_entity_overflow()를 참조하십시오. 이러한 경우가 감지되면 런타임 
 *   및 데드라인을 업데이트해야 합니다. 
 *
 *   task에 implicit deadline(즉, deadline == period)이 있는 경우 Original CBS가 
 *   적용됩니다. 이전 사례에서와 같이 런타임이 보충되고 새로운 절대 deadline이 
 *   설정됩니다.
 *
 *   그러나 deadline이 정해져 있다고 하는 deadline < period이 있는 task에 대해서는 
 *   Original CBS가 제대로 작동하지 않습니다. Original CBS를 적용하면 deadline이
 *   제한된 task이 일정 period 동안 런타임/deadline을 실행할 수 있습니다.
 *   deadline < period을 사용하면 task이 런타임/period 허용 대역폭을 초과하여 승인 
 *   테스트를 중단합니다.
 *
 *   이러한 오작동을 방지하기 위해 Revisited CBS는 런타임 오버플로가 감지될 때 
 *   제한된 deadline task에 사용됩니다. Revisited CBS에서는 새로운 절대 deadline을 
 *   보충하고 설정하는 대신 task의 남은 런타임을 줄여 런타임 오버플로를 
 *   방지합니다. 개정된 CBS 규칙에 대한 자세한 내용은 주석 
 *   update_dl_revised_wakeup() 함수를 참조하십시오.
 *
 * - 만료 or overflow인 dl에 대해서 implicit dl task인 경우 일반적인 
 *   충전(original cbs)를 사용한다
 * - implicit dl이 아니고, overflow인 경우 revised wakeup방식으로 충전한다.
 */
static void update_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

/*
 * IAMROOT, 2023.03.04:
 * - deadline이 만료되엇거나 overflow라면 
 */
	if (dl_time_before(dl_se->deadline, rq_clock(rq)) ||
	    dl_entity_overflow(dl_se, rq_clock(rq))) {

/*
 * IAMROOT, 2023.03.04:
 * - constrained task이고 (즉 별도의 dealine설정이 있는),
 *   마감이 아닌 overflow만 된 상황이며, 부스트중이 아니라면
 *   revisited cbs를 사용해서 충전한다.
 */
		if (unlikely(!dl_is_implicit(dl_se) &&
			     !dl_time_before(dl_se->deadline, rq_clock(rq)) &&
			     !is_dl_boosted(dl_se))) {
			update_dl_revised_wakeup(dl_se, rq);
			return;
		}

/*
 * IAMROOT, 2023.03.04:
 * - original CBS로 사용한다.
 */
		dl_se->deadline = rq_clock(rq) + pi_of(dl_se)->dl_deadline;
		dl_se->runtime = pi_of(dl_se)->dl_runtime;
	}
}

/*
 * IAMROOT, 2023.03.04:
 * - period의 절대값을 구한다
 *   deadline = time_stamp + dl_deadline 
 *   return = time_stamp + dl_deadline - dl_deadline + dl_period
 *          = time_stamp + dl_period
 */
static inline u64 dl_next_period(struct sched_dl_entity *dl_se)
{
	return dl_se->deadline - dl_se->dl_deadline + dl_se->dl_period;
}

/*
 * If the entity depleted all its runtime, and if we want it to sleep
 * while waiting for some new execution time to become available, we
 * set the bandwidth replenishment timer to the replenishment instant
 * and try to activate it.
 *
 * Notice that it is important for the caller to know if the timer
 * actually started or not (i.e., the replenishment instant is in
 * the future or in the past).
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   엔터티가 모든 실행 시간을 고갈하고 새로운 실행 시간을 사용할 수 있을 때까지
 *   기다리는 동안 휴면 상태를 유지하려는 경우 대역폭 보충 타이머를 보충 순간으로
 *   설정하고 활성화를 시도합니다.
 *
 *   호출자가 타이머가 실제로 시작되었는지 여부를 아는 것이 중요합니다
 *   (즉, 보충 순간이 미래 또는 과거임).
 *
 * - +---------+-----+
 *   | runtime |     |
 *   +---------+-----+
 *                   ^next period
 *   runtime이 다 소모됬으면 next period때 hrtimer가 깨어나도록 예약한다.
 *   (dl_task_timer)
 */
static int start_dl_timer(struct task_struct *p)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct hrtimer *timer = &dl_se->dl_timer;
	struct rq *rq = task_rq(p);
	ktime_t now, act;
	s64 delta;

	lockdep_assert_rq_held(rq);

	/*
	 * We want the timer to fire at the deadline, but considering
	 * that it is actually coming from rq->clock and not from
	 * hrtimer's time base reading.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - act   : dl의 next period의 시각.
 * - now   : timer에 따른 현재 시각.
 * - delta : rq에 기록한 time stamp와 current time의 차이.
 * - act갱신 : next period + (rq에 기록한 시각으로부터 지난 시각)
 *
 * - delta를 별도로 구해 더해주는 이유:
 *   스케줄러에서 사용하는 시간인 런큐 클럭(rq->clock)은 현재 시각을 의미하긴
 *   하지만 클럭의 현재 시간을 가져와서 갱신하여 사용한다. 이 값은 로드 평균을
 *   산출한 기간등의 계산을 정확하게 하기 위해 해당 시점에서 갱신되어 사용한다.
 *   따라서 아래 코드에서 사용하는 rq_clock()은 갱신된 시점의 약각 과거 
 *   시각이므로 지금 타이머 H/W를 사용하려는 현재 시각과 약간의 차이가 발생한다.
 *   따라서 이를 보정하기 위해 delta를 산출하여 보정해야 한다.
 */
	act = ns_to_ktime(dl_next_period(dl_se));
	now = hrtimer_cb_get_time(timer);
	delta = ktime_to_ns(now) - rq_clock(rq);
	act = ktime_add_ns(act, delta);

	/*
	 * If the expiry time already passed, e.g., because the value
	 * chosen as the deadline is too small, don't even try to
	 * start the timer in the past!
	 */
	/*
	 * IAMROOT. 2023.02.25:
	 * - google-translate
	 *   만료 시간이 이미 지난 경우, 예를 들어 마감 시간으로 선택한 값이 너무 작기 때문에
	 *   과거에 타이머를 시작하려고 시도조차 하지 마십시오!
	 */
	if (ktime_us_delta(act, now) < 0)
		return 0;

	/*
	 * !enqueued will guarantee another callback; even if one is already in
	 * progress. This ensures a balanced {get,put}_task_struct().
	 *
	 * The race against __run_timer() clearing the enqueued state is
	 * harmless because we're holding task_rq()->lock, therefore the timer
	 * expiring after we've done the check will wait on its task_rq_lock()
	 * and observe our state.
	 */
	if (!hrtimer_is_queued(timer)) {
		get_task_struct(p);
		hrtimer_start(timer, act, HRTIMER_MODE_ABS_HARD);
	}

	return 1;
}

/*
 * This is the bandwidth enforcement timer callback. If here, we know
 * a task is not on its dl_rq, since the fact that the timer was running
 * means the task is throttled and needs a runtime replenishment.
 *
 * However, what we actually do depends on the fact the task is active,
 * (it is on its rq) or has been removed from there by a call to
 * dequeue_task_dl(). In the former case we must issue the runtime
 * replenishment and add the task back to the dl_rq; in the latter, we just
 * do nothing but clearing dl_throttled, so that runtime and deadline
 * updating (and the queueing back to dl_rq) will be done by the
 * next call to enqueue_task_dl().
 */

/*
 * IAMROOT, 2023.03.04:
 * - papago 
 *   이것은 대역폭 적용 타이머 콜백입니다. 여기서 타이머가 실행 중이라는 
 *   사실은 작업이 제한되고 런타임 보충이 필요하기 때문에 작업이 dl_rq에 
 *   없다는 것을 알 수 있습니다.
 *
 *   그러나 실제로 수행하는 작업은 작업이 활성 상태이거나(rq에 있음) 
 *   dequeue_task_dl() 호출에 의해 제거되었다는 사실에 따라 달라집니다. 
 *   전자의 경우 런타임 보충을 실행하고 작업을 dl_rq에 다시 추가해야 
 *   합니다. 후자의 경우, 우리는 dl_throttled를 지우는 것 외에는 아무 
 *   것도 하지 않기 때문에 런타임 및 데드라인 업데이트(및 dl_rq로 다시 
 *   대기하는 것)는 다음에 enqueue_task_dl()을 호출할 때 완료됩니다.
 *
 * - update_curr_dl을 통해서 매 period마다 호출된다.
 * - 소모된 런타임을 보충한다.
 *   다시 enqueue시킨다.
 */
static enum hrtimer_restart dl_task_timer(struct hrtimer *timer)
{
	struct sched_dl_entity *dl_se = container_of(timer,
						     struct sched_dl_entity,
						     dl_timer);
	struct task_struct *p = dl_task_of(dl_se);
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);

	/*
	 * The task might have changed its scheduling policy to something
	 * different than SCHED_DEADLINE (through switched_from_dl()).
	 */
	if (!dl_task(p))
		goto unlock;

	/*
	 * The task might have been boosted by someone else and might be in the
	 * boosting/deboosting path, its not throttled.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - 이미 부스트되있다면 enqueue되있기 때문에 다시 enqueue를 할 필요 없다.
 */
	if (is_dl_boosted(dl_se))
		goto unlock;

	/*
	 * Spurious timer due to start_dl_timer() race; or we already received
	 * a replenishment from rt_mutex_setprio().
	 */
/*
 * IAMROOT, 2023.03.04:
 * - spurious timer에 대한 처리.
 */
	if (!dl_se->dl_throttled)
		goto unlock;

	sched_clock_tick();
	update_rq_clock(rq);

	/*
	 * If the throttle happened during sched-out; like:
	 *
	 *   schedule()
	 *     deactivate_task()
	 *       dequeue_task_dl()
	 *         update_curr_dl()
	 *           start_dl_timer()
	 *         __dequeue_task_dl()
	 *     prev->on_rq = 0;
	 *
	 * We can be both throttled and !queued. Replenish the counter
	 * but do not enqueue -- wait for our wakeup to do that.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - throttled이 발생됬을때 위의 주석과 같은 형태에 의해 dequeue된 
 *   상태이다.
 * - throttle이 되서 들어온 시점에서, dequeue가 되있다면(sleep 한 task)
 *   throttle이 아닌 정말 sleep이 된 task이다.
 *   이 경우 enqueue할 필요도 없으므로 runtime보충만 하고 wakeup할때까지
 *   끝낸다.
 */
	if (!task_on_rq_queued(p)) {
		replenish_dl_entity(dl_se);
		goto unlock;
	}

#ifdef CONFIG_SMP
/*
 * IAMROOT, 2023.03.04:
 * - cpu scheduler가 off인 상태. 다른 cpu rq로 migration 한다.
 */
	if (unlikely(!rq->online)) {
		/*
		 * If the runqueue is no longer available, migrate the
		 * task elsewhere. This necessarily changes rq.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   실행 대기열을 더 이상 사용할 수 없는 경우 작업을 다른 곳으로 
 *   마이그레이션하십시오. 이것은 필연적으로 rq를 변경합니다.
 */
		lockdep_unpin_lock(__rq_lockp(rq), rf.cookie);
		rq = dl_task_offline_migration(rq, p);
		rf.cookie = lockdep_pin_lock(__rq_lockp(rq));
		update_rq_clock(rq);

		/*
		 * Now that the task has been migrated to the new RQ and we
		 * have that locked, proceed as normal and enqueue the task
		 * there.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   이제 작업이 새 RQ로 마이그레이션되고 잠겼으므로 정상적으로 진행하고 
 *   거기에 작업을 대기열에 넣습니다.
 */
	}
#endif

	enqueue_task_dl(rq, p, ENQUEUE_REPLENISH);

/*
 * IAMROOT, 2023.03.04:
 * - rq->curr가 dl이면 @p와 비교해 급한것으로 resched한다.
 *   그게 아니면 deadline인 p가 무조건 높을것이므로 reschedule하면 @p로
 *   바뀔것이다.
 */
	if (dl_task(rq->curr))
		check_preempt_curr_dl(rq, p, 0);
	else
		resched_curr(rq);

#ifdef CONFIG_SMP
	/*
	 * Queueing this task back might have overloaded rq, check if we need
	 * to kick someone away.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   이 작업을 다시 대기열에 넣으면 rq가 과부하되었을 수 있습니다. 
 *   누군가를 쫓아내야 하는지 확인하십시오.
 * - pushable 할 task가 있는지 확ㅇ니한다.
 */
	if (has_pushable_dl_tasks(rq)) {
		/*
		 * Nothing relies on rq->lock after this, so its safe to drop
		 * rq->lock.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   이 이후에는 rq->lock에 의존하는 것이 없으므로 rq->lock을 삭제하는 
 *   것이 안전합니다.
 */
		rq_unpin_lock(rq, &rf);
		push_dl_task(rq);
		rq_repin_lock(rq, &rf);
	}
#endif

unlock:
	task_rq_unlock(rq, p, &rf);

	/*
	 * This can free the task_struct, including this hrtimer, do not touch
	 * anything related to that after this.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   이것은 이 hrtimer를 포함하여 task_struct를 해제할 수 있습니다. 이 
 *   이후에는 그와 관련된 어떤 것도 건드리지 마십시오.
 */
	put_task_struct(p);

	return HRTIMER_NORESTART;
}

/*
 * IAMROOT, 2023.03.04:
 * - dl_task_timer로 설정한다.
 */
void init_dl_task_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->dl_timer;

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	timer->function = dl_task_timer;
}

/*
 * During the activation, CBS checks if it can reuse the current task's
 * runtime and period. If the deadline of the task is in the past, CBS
 * cannot use the runtime, and so it replenishes the task. This rule
 * works fine for implicit deadline tasks (deadline == period), and the
 * CBS was designed for implicit deadline tasks. However, a task with
 * constrained deadline (deadline < period) might be awakened after the
 * deadline, but before the next period. In this case, replenishing the
 * task would allow it to run for runtime / deadline. As in this case
 * deadline < period, CBS enables a task to run for more than the
 * runtime / period. In a very loaded system, this can cause a domino
 * effect, making other tasks miss their deadlines.
 *
 * To avoid this problem, in the activation of a constrained deadline
 * task after the deadline but before the next period, throttle the
 * task and set the replenishing timer to the begin of the next period,
 * unless it is boosted.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   활성화하는 동안 CBS는 현재 task의 runtime 및 기간을 재사용할 수 
 *   있는지 확인합니다. task의 기한이 지난 경우 CBS는 runtime을 사용할 수 
 *   없으므로 task을 보충합니다.
 *   이 규칙은 암시적 기한 task(기한 == 기간)에 적합하며 CBS는 
 *   암시적 기한 task을 위해 설계되었습니다. 그러나 기한이 throttle된 
 *   task(기한 < 기간)은 기한 이후이지만 다음 기간 전에 깨어날 수 
 *   있습니다. 이 경우 task을 보충하면 runtime/마감 시간 동안 실행할 수 
 *   있습니다. 이 경우 마감일 < 기간과 같이 CBS는 task이 runtime/기간 
 *   이상 실행되도록 합니다. 매우 로드된 시스템에서 이것은 도미노 효과를 
 *   일으켜 다른 task이 마감일을 놓치게 만들 수 있습니다.
 *
 *   이 문제를 방지하려면 기한 후 다음 기간 이전에 throttle된 기한 task을 
 *   활성화할 때 task을 조절하고 보충 타이머가 부스트되지 않는 한 다음 
 *   기간의 시작으로 설정합니다.
 *
 * - implicit task가 아닌 경우에, system이 부하인 상황에서 task들의 실해이
 *   점점 뒤로 밀리다가 마감을 놓치게 될수있다.
 *   이를 방지하기 위해 runtime을 아에 새로 시작한다.
 */
static inline void dl_check_constrained_dl(struct sched_dl_entity *dl_se)
{
	struct task_struct *p = dl_task_of(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq_of_se(dl_se));

/*
 * IAMROOT, 2023.03.04:
 * - 마감시간 < 현재시간 < 다음 마감시간의 범위인지 확인한다.
 */
	if (dl_time_before(dl_se->deadline, rq_clock(rq)) &&
	    dl_time_before(rq_clock(rq), dl_next_period(dl_se))) {

/*
 * IAMROOT, 2023.03.04:
 * - boost가 동작하면 return.
 *   boost가 동작안하고 있다면, timer를 동작시킨다.
 *   timer동작을 실패했으면 빠져나간다.
 */
		if (unlikely(is_dl_boosted(dl_se) || !start_dl_timer(p)))
			return;

/*
 * IAMROOT, 2023.03.04:
 * - timer가동. throttle 설정 및 runtime을 보충을 위해 초기화.
 */
		dl_se->dl_throttled = 1;
		if (dl_se->runtime > 0)
			dl_se->runtime = 0;
	}
}

static
int dl_runtime_exceeded(struct sched_dl_entity *dl_se)
{
	return (dl_se->runtime <= 0);
}

extern bool sched_rt_bandwidth_account(struct rt_rq *rt_rq);

/*
 * This function implements the GRUB accounting rule:
 * according to the GRUB reclaiming algorithm, the runtime is
 * not decreased as "dq = -dt", but as
 * "dq = -max{u / Umax, (1 - Uinact - Uextra)} dt",
 * where u is the utilization of the task, Umax is the maximum reclaimable
 * utilization, Uinact is the (per-runqueue) inactive utilization, computed
 * as the difference between the "total runqueue utilization" and the
 * runqueue active utilization, and Uextra is the (per runqueue) extra
 * reclaimable utilization.
 * Since rq->dl.running_bw and rq->dl.this_bw contain utilizations
 * multiplied by 2^BW_SHIFT, the result has to be shifted right by
 * BW_SHIFT.
 * Since rq->dl.bw_ratio contains 1 / Umax multiplied by 2^RATIO_SHIFT,
 * dl_bw is multiped by rq->dl.bw_ratio and shifted right by RATIO_SHIFT.
 * Since delta is a 64 bit variable, to have an overflow its value
 * should be larger than 2^(64 - 20 - 8), which is more than 64 seconds.
 * So, overflow is not an issue here.
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   이 함수는 GRUB 계정 규칙을 구현합니다. GRUB 재확보 알고리즘에 따르면 런타임은
 *   "dq = -dt"가 아니라
 *   "dq = -max{u / Umax, (1 - Uinact - Uextra)} dt"로
 *   감소합니다. 여기서 u는 작업의 활용도, Umax는 회수 가능한 최대 활용도, Uinact는
 *   "총 실행 대기열 활용도"와 실행 대기열 활성 활용도 간의 차이로 계산된 (실행
 *   대기열당) 비활성 활용도이며, Uextra는 (실행 대기열당) 추가 회수 가능 활용도이다.
 *   rq->dl.running_bw 및 rq->dl.this_bw에는 2^BW_SHIFT를 곱한 활용도가
 *   포함되어 있으므로 결과를 BW_SHIFT만큼 오른쪽으로 이동해야 합니다.
 *   rq->dl.bw_ratio는 2^RATIO_SHIFT를 곱한 1/Umax를 포함하므로 dl_bw는
 *   rq->dl.bw_ratio를 곱하고 RATIO_SHIFT만큼 오른쪽으로 이동합니다. delta는 64비트
 *   변수이므로 오버플로가 발생하려면 해당 값이 2^(64 - 20 - 8)보다 커야 하며, 이는
 *   64초 이상입니다. 따라서 오버플로는 여기서 문제가 되지 않습니다.
 * --- GRUB ---
 *  - Documentation/scheduler/sched-deadline.rst 참고
 *  - Greedy Reclamation of Unused Bandwidth
 * ------------
 *   @return delta * min : 사용하지 않은 bw가 너무 많은 경우.
 *   @return delta * (system에서 사용한 bw) 
 * - runtime을 차감할때, delta를 배율로 차감하는데, 이때 차감되는 정도를 결정하여
 *   return한다.
 *   이때 차감되는 정도는 system에서 사용한 bw를 의미하여,
 * - @rq가 사용중인 bw을 가 적으면 적은값, 많으면 많은값을 return할 것이다.
 *
 *   system 기준 bw(BW_UNIT) - (사용하지 않은 bw)의 방법으로 구한다.
 *
 */
static u64 grub_reclaim(u64 delta, struct rq *rq, struct sched_dl_entity *dl_se)
{

/*
 * IAMROOT, 2023.03.11:
 * - extra_bw
 *   해당 dl이 더 bw를 더 사용할수 있게하는 보너스.
 * - u_inact
 *   사용하지 않은 bw
 * - u_act_min
 *   할당되어야 할 최소 bw
 */
	u64 u_inact = rq->dl.this_bw - rq->dl.running_bw; /* Utot - Uact */
	u64 u_act;
	u64 u_act_min = (dl_se->dl_bw * rq->dl.bw_ratio) >> RATIO_SHIFT;

	/*
	 * Instead of computing max{u * bw_ratio, (1 - u_inact - u_extra)},
	 * we compare u_inact + rq->dl.extra_bw with
	 * 1 - (u * rq->dl.bw_ratio >> RATIO_SHIFT), because
	 * u_inact + rq->dl.extra_bw can be larger than
	 * 1 * (so, 1 - u_inact - rq->dl.extra_bw would be negative
	 * leading to wrong results)
	 */
	/*
	 * IAMROOT. 2023.02.25:
	 * - google-translate
	 *   max{u * bw_ratio, (1 - u_inact - u_extra)}를 계산하는 대신
	 *   u_inact + rq->dl.extra_bw를
	 *   1 - (u * rq->dl.bw_ratio >> RATIO_SHIFT)와 비교합니다.
	 *   u_inact + rq->dl.extra_bw는 1 *보다 클 수 있습니다.
	 *   (따라서 1 - u_inact - rq->dl.extra_bw는 음수가 되어 잘못된 결과를
	 *   초래합니다.)
	 *
	 * - 사용되지 않은 bw(u_inact) + 추가 bw(extra_bw) 이 너무 큰값을 가졌다면,
	 *   즉 bw를 너무 적게 썻으면 min을 배율로 한다.
	 *   그게 아니면 system에서 사용중인 bw를 배율로하여 그만큼 차감하게한다.
	 */
	if (u_inact + rq->dl.extra_bw > BW_UNIT - u_act_min)
		u_act = u_act_min;
	else
		u_act = BW_UNIT - u_inact - rq->dl.extra_bw;

	return (delta * u_act) >> BW_SHIFT;
}

/*
 * Update the current task's runtime statistics (provided it is still
 * a -deadline task and has not been removed from the dl_rq).
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   현재 작업의 런타임 통계를 업데이트합니다(여전히 -deadline 작업이고 dl_rq에서
 *   제거되지 않은 경우).
 *
 * - 1. delta_exec(이전 실행시간과 현재 시간차이) 계산
 *   2. delta_exec에 scale 적용(grub 또는 cpu freq,capacity)
 *   3. runtime 계산(runtime -= delta_exec)
 *   4. runtime < 0 이면 dequeue task
 *   5. curr 가 leftmost가 아니면 reschedule
 *   6. bandwidth enable 된경우 rt_time 누적
 */
static void update_curr_dl(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_dl_entity *dl_se = &curr->dl;
	u64 delta_exec, scaled_delta_exec;
	int cpu = cpu_of(rq);
	u64 now;

	/*
	 * IAMROOT, 2023.02.25:
	 * - lock을 잡지 않았기 때문에 dl_rq에서 동작하는 task인지 확인해야 한다
	 */
	if (!dl_task(curr) || !on_dl_rq(dl_se))
		return;

	/*
	 * Consumed budget is computed considering the time as
	 * observed by schedulable tasks (excluding time spent
	 * in hardirq context, etc.). Deadlines are instead
	 * computed using hard walltime. This seems to be the more
	 * natural solution, but the full ramifications of this
	 * approach need further study.
	 */
	/*
	 * IAMROOT. 2023.02.25:
	 * - google-translate
	 *   소비된 예산은 예약 가능한 작업에서 관찰된 시간을 고려하여 계산됩니다(hardirq
	 *   컨텍스트에서 소요된 시간 등 제외). 마감일은 대신 하드 월타임을 사용하여
	 *   계산됩니다. 이것은 보다 자연스러운 해결책인 것처럼 보이지만 이 접근 방식의 전체
	 *   결과는 추가 연구가 필요합니다.
	 */
	now = rq_clock_task(rq);
	delta_exec = now - curr->se.exec_start;
	/*
	 * IAMROOT, 2023.02.25:
	 * - clock이 불안정한 시스템에서는 delta_exec 가 마이너스 값이 나올수 있음
	 */
	if (unlikely((s64)delta_exec <= 0)) {
		if (unlikely(dl_se->dl_yielded))
			goto throttle;
		return;
	}

	/*
	 * IAMROOT, 2023.02.25:
	 * - delta_exec의 최대값을 추적하여 schedule tick이 늦어짐을 추적.(ex.NOHZ)
	 */
	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	/*
	 * IAMROOT, 2023.02.26:
	 * - se.sum_exec_runtime 에 delta_exec 누적
	 * - curr의 thread_group_cputimer의 sum_exec_runtime 에 delta_exec 누적
	 */
	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	/*
	 * IAMROOT, 2023.02.26:
	 * - 다음 delta_exec 계산을 위해 now(현재시간)를 exec_start에 저장
	 */
	curr->se.exec_start = now;
	/*
	 * IAMROOT, 2023.02.26:
	 * - 해당 cgroup 에 delta_exec 누적
	 */
	cgroup_account_cputime(curr, delta_exec);

	/*
	 * IAMROOT, 2023.02.26:
	 * - SCHED_FLAG_SUGOV flag 설정된 경우
	 */
	if (dl_entity_is_special(dl_se))
		return;

	/*
	 * For tasks that participate in GRUB, we implement GRUB-PA: the
	 * spare reclaimed bandwidth is used to clock down frequency.
	 *
	 * For the others, we still need to scale reservation parameters
	 * according to current frequency and CPU maximum capacity.
	 */
	/*
	 * IAMROOT. 2023.02.25:
	 * - google-translate
	 *   GRUB에 참여하는 작업의 경우 GRUB-PA를 구현합니다. 여분의 회수된 대역폭은
	 *   주파수를 낮추는 데 사용됩니다. 나머지는 여전히 현재 주파수와 CPU 최대 용량에
	 *   따라 예약 매개변수를 확장해야 합니다.
	 *
	 * - TODO. 유저 app에서 SCHED_FLAG_RECLAIM flag를 사용한 경우
	 *   grub_reclaim scale 이 적용된 scaled_delta_exec 값을 가져온다
	 */
	if (unlikely(dl_se->flags & SCHED_FLAG_RECLAIM)) {
		scaled_delta_exec = grub_reclaim(delta_exec,
						 rq,
						 &curr->dl);
	} else {
		unsigned long scale_freq = arch_scale_freq_capacity(cpu);
		unsigned long scale_cpu = arch_scale_cpu_capacity(cpu);

		/*
		 * IAMROOT, 2023.02.25:
		 * - (((delta_exec * scale_freq) >> 10) * scale_cpu) >> 10
		 * - cpu freq, capacity scale이 적용된 1보다 작은 값
		 */
		scaled_delta_exec = cap_scale(delta_exec, scale_freq);
		scaled_delta_exec = cap_scale(scaled_delta_exec, scale_cpu);
	}

	/*
	 * IAMROOT, 2023.02.26:
	 * - runtime 에서 scale 이 적용된 delta_exec를 뺀다
	 */
	dl_se->runtime -= scaled_delta_exec;

throttle:
	if (dl_runtime_exceeded(dl_se) || dl_se->dl_yielded) {
	/*
	 * IAMROOT, 2023.02.27:
	 * - runtime 을 모두 사용하였거나 양보한 경우
	 */
		dl_se->dl_throttled = 1;

		/* If requested, inform the user about runtime overruns. */
		if (dl_runtime_exceeded(dl_se) &&
		    (dl_se->flags & SCHED_FLAG_DL_OVERRUN))
			dl_se->dl_overrun = 1;

		__dequeue_task_dl(rq, curr, 0);
		/*
		 * IAMROOT, 2023.02.25:
		 * - boosted 되었으면 enqueue
		 *   boosted 되지 않았으면 dl_timer를 가동 시도. 만익 dl_timer
		 *   start를 실패했으면(start하기 너무 짧은 간격) 즉시 enqueue한다.
		 */
		if (unlikely(is_dl_boosted(dl_se) || !start_dl_timer(curr)))
			enqueue_task_dl(rq, curr, ENQUEUE_REPLENISH);

		/*
		 * IAMROOT, 2023.02.25:
		 * - boost 되지 않았고 dl_timer 시작한 case.
		 *   curr가 leftmost가 아닐 경우 reschedule 요청.
		 *
		 */
		if (!is_leftmost(curr, &rq->dl))
			resched_curr(rq);
	}

	/*
	 * Because -- for now -- we share the rt bandwidth, we need to
	 * account our runtime there too, otherwise actual rt tasks
	 * would be able to exceed the shared quota.
	 *
	 * Account to the root rt group for now.
	 *
	 * The solution we're working towards is having the RT groups scheduled
	 * using deadline servers -- however there's a few nasties to figure
	 * out before that can happen.
	 */
	/*
	 * IAMROOT. 2023.02.25:
	 * - google-translate
	 *   -- 현재 -- 우리는 rt 대역폭을 공유하기 때문에 거기에서 런타임도 고려해야
	 *   합니다. 그렇지 않으면 실제 rt 작업이 공유 할당량을 초과할 수 있습니다.
	 *
	 *   지금은 루트 rt 그룹에 계정을 지정하십시오.
	 *
	 *   우리가 작업하고 있는 솔루션은 데드라인 서버를 사용하여 RT 그룹을
	 *   예약하는 것입니다. -- 그러나 그런 일이 일어나기 전에 알아내야 할 몇 가지
	 *   불쾌한 일이 있습니다.
	 *
	 * - rt_time 누적 - bandwidth_enabled 되어 있고 rt_period_timer가 active
	 *   되어 있거나 rt_time이 runtime보다 작을때
	 */
	if (rt_bandwidth_enabled()) {
		struct rt_rq *rt_rq = &rq->rt;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We'll let actual RT tasks worry about the overflow here, we
		 * have our own CBS to keep us inline; only account when RT
		 * bandwidth is relevant.
		 */
		/*
		 * IAMROOT. 2023.02.25:
		 * - google-translate
		 *   우리는 실제 RT 작업이 여기에서 오버플로우에 대해 걱정하도록 할
		 *   것입니다. 우리는 우리를 인라인으로 유지하기 위한 자체 CBS가 있습니다.
		 *   RT 대역폭이 관련된 경우에만 설명합니다.
		 */
		if (sched_rt_bandwidth_account(rt_rq))
			rt_rq->rt_time += delta_exec;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
}

static enum hrtimer_restart inactive_task_timer(struct hrtimer *timer)
{
	struct sched_dl_entity *dl_se = container_of(timer,
						     struct sched_dl_entity,
						     inactive_timer);
	struct task_struct *p = dl_task_of(dl_se);
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);

	sched_clock_tick();
	update_rq_clock(rq);

	if (!dl_task(p) || READ_ONCE(p->__state) == TASK_DEAD) {
		struct dl_bw *dl_b = dl_bw_of(task_cpu(p));

		if (READ_ONCE(p->__state) == TASK_DEAD && dl_se->dl_non_contending) {
			sub_running_bw(&p->dl, dl_rq_of_se(&p->dl));
			sub_rq_bw(&p->dl, dl_rq_of_se(&p->dl));
			dl_se->dl_non_contending = 0;
		}

		raw_spin_lock(&dl_b->lock);
		__dl_sub(dl_b, p->dl.dl_bw, dl_bw_cpus(task_cpu(p)));
		raw_spin_unlock(&dl_b->lock);
		__dl_clear_params(p);

		goto unlock;
	}
	if (dl_se->dl_non_contending == 0)
		goto unlock;

	sub_running_bw(dl_se, &rq->dl);
	dl_se->dl_non_contending = 0;
unlock:
	task_rq_unlock(rq, p, &rf);
	put_task_struct(p);

	return HRTIMER_NORESTART;
}

void init_dl_inactive_task_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->inactive_timer;

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	timer->function = inactive_task_timer;
}

#ifdef CONFIG_SMP

/*
 * IAMROOT, 2023.03.04:
 * - 1. earliest_dl 갱신.
 *   2. cpupri를 deadline의 갱신.
 *   3. cpudl 설정.
 */
static void inc_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	if (dl_rq->earliest_dl.curr == 0 ||
	    dl_time_before(deadline, dl_rq->earliest_dl.curr)) {
		if (dl_rq->earliest_dl.curr == 0)
			cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_HIGHER);
		dl_rq->earliest_dl.curr = deadline;
		cpudl_set(&rq->rd->cpudl, rq->cpu, deadline);
	}
}

/*
 * IAMROOT, 2023.02.25:
 * - 1. dl_nr_running == 0 이면 cpupri 갱신
 *   2. dl_nr_running > 0 이면 dl_rq rbtree의 leftmost deadline으로
 *      earliest_dl.curr 와 rd의 cpudl 갱신
 */
static void dec_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	/*
	 * Since we may have removed our earliest (and/or next earliest)
	 * task we must recompute them.
	 */
	if (!dl_rq->dl_nr_running) {
		dl_rq->earliest_dl.curr = 0;
		dl_rq->earliest_dl.next = 0;
		cpudl_clear(&rq->rd->cpudl, rq->cpu);
		/*
		 * IAMROOT, 2023.02.25:
		 * - dl이 존재하지 않게 되므로 rt.highest_prio.curr를 cpupri에 설정
		 */
		cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
	} else {
		struct rb_node *leftmost = dl_rq->root.rb_leftmost;
		struct sched_dl_entity *entry;

		entry = rb_entry(leftmost, struct sched_dl_entity, rb_node);
		/*
		 * IAMROOT, 2023.02.25:
		 * - runtime이 소진되어 rbtree에서 삭제하였으므로 dl_rq의 earliest
		 *   deadline 설정을 다시 하여야 한다.
		 * - cpudl 자료구조에서도 해당 cpu의 dl 값을 방금 교체한 leftmost
		 *   entry의 dl로 설정
		 */
		dl_rq->earliest_dl.curr = entry->deadline;
		cpudl_set(&rq->rd->cpudl, rq->cpu, entry->deadline);
	}
}

#else

static inline void inc_dl_deadline(struct dl_rq *dl_rq, u64 deadline) {}
static inline void dec_dl_deadline(struct dl_rq *dl_rq, u64 deadline) {}

#endif /* CONFIG_SMP */

/*
 * IAMROOT, 2023.03.04:
 * - 1. dl nr 증가
 *   2. earliest_dl 갱신.
 *   3. cpupri를 deadline의 갱신.
 *   4. cpudl 설정.
 *   5. nr migrate 증가.
 *   6. overloaded 갱신.
 */
static inline
void inc_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	int prio = dl_task_of(dl_se)->prio;
	u64 deadline = dl_se->deadline;

	WARN_ON(!dl_prio(prio));
	dl_rq->dl_nr_running++;
	add_nr_running(rq_of_dl_rq(dl_rq), 1);

	inc_dl_deadline(dl_rq, deadline);
	inc_dl_migration(dl_se, dl_rq);
}

/*
 * IAMROOT, 2023.02.25:
 * - 1. dl_nr_running--
 *   2. rq->nr_running--
 *   3. dl_rq의 earliest_dl.curr 와 rd의 cpudl deadline 갱신
 *   4. overload 설정
 */
static inline
void dec_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	int prio = dl_task_of(dl_se)->prio;

	WARN_ON(!dl_prio(prio));
	WARN_ON(!dl_rq->dl_nr_running);
	dl_rq->dl_nr_running--;
	sub_nr_running(rq_of_dl_rq(dl_rq), 1);

	dec_dl_deadline(dl_rq, dl_se->deadline);
	dec_dl_migration(dl_se, dl_rq);
}

#define __node_2_dle(node) \
	rb_entry((node), struct sched_dl_entity, rb_node)

/*
 * IAMROOT, 2023.03.04:
 * - deadline으로 비교한다.
 */
static inline bool __dl_less(struct rb_node *a, const struct rb_node *b)
{
	return dl_time_before(__node_2_dle(a)->deadline, __node_2_dle(b)->deadline);
}

/*
 * IAMROOT, 2023.03.04:
 * - @dl_se를 dl rq에 추가한다. deadline으로 정렬된다.
 *   dl task 추가대한 매개변수값을 갱신한다.
 */
static void __enqueue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	BUG_ON(!RB_EMPTY_NODE(&dl_se->rb_node));

	rb_add_cached(&dl_se->rb_node, &dl_rq->root, __dl_less);

	inc_dl_tasks(dl_se, dl_rq);
}

/*
 * IAMROOT, 2023.02.25:
 * - 1. @dl_se를 dl_rq의 rbtree에서 삭제
 *      (rq->curr->dl_se->rb_node 를 rq->dl_rq->root->rb_root에서 삭제)
 *   2. dec_dl_tasks 수행
 */
static void __dequeue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	if (RB_EMPTY_NODE(&dl_se->rb_node))
		return;

	rb_erase_cached(&dl_se->rb_node, &dl_rq->root);

	RB_CLEAR_NODE(&dl_se->rb_node);

	dec_dl_tasks(dl_se, dl_rq);
}

/*
 * IAMROOT, 2023.03.04:
 * - flag에 따라 다음을 수행하고 enqueue 한다.
 * - ENQUEUE_WAKEUP
 *   task가 wakeup인 경우의 보충상황.
 *   active contending상태가 된다. (task_contending() 내부 주석참고).
 *   runtime, deadline을 리필한다.(revised cbs 보충이 추가되었다.)
 * - ENQUEUE_REPLENISH
 *   timer에 의한 runtime 보충상황. runtime, deadline을 리필한다.
 * - ENQUEUE_RESTORE 이고 deadline이 만료라면.
 *   new dl이라는 의미로 판단하여 새로 설정한다.
 */
static void
enqueue_dl_entity(struct sched_dl_entity *dl_se, int flags)
{
	BUG_ON(on_dl_rq(dl_se));

	/*
	 * If this is a wakeup or a new instance, the scheduling
	 * parameters of the task might need updating. Otherwise,
	 * we want a replenishment of its runtime.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   깨우기 또는 새 인스턴스인 경우 작업의 예약 매개변수를 업데이트해야 할 수 
 *   있습니다. 그렇지 않으면 런타임을 보충해야 합니다.
 */
	if (flags & ENQUEUE_WAKEUP) {
		task_contending(dl_se, flags);
		update_dl_entity(dl_se);
	} else if (flags & ENQUEUE_REPLENISH) {
		replenish_dl_entity(dl_se);
	} else if ((flags & ENQUEUE_RESTORE) &&
		  dl_time_before(dl_se->deadline,
				 rq_clock(rq_of_dl_rq(dl_rq_of_se(dl_se))))) {
		setup_new_dl_entity(dl_se);
	}

	__enqueue_dl_entity(dl_se);
}

static void dequeue_dl_entity(struct sched_dl_entity *dl_se)
{
	__dequeue_dl_entity(dl_se);
}

/*
 * IAMROOT, 2023.03.04:
 * - enqueue를 수행한다
 *   1. boost, throttle에 대한 처리
 *   2. dl task가 아니게 된것에 대한 처리.
 *   3. dl task 개수등의 매갭변수 갱신
 *   4. this_bw, running_bw의 적산.
 *   5. task contending 처리
 *   6. throttle 설정 및 @flags에 따른 runtime / deadline갱신
 */
static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	if (is_dl_boosted(&p->dl)) {
		/*
		 * Because of delays in the detection of the overrun of a
		 * thread's runtime, it might be the case that a thread
		 * goes to sleep in a rt mutex with negative runtime. As
		 * a consequence, the thread will be throttled.
		 *
		 * While waiting for the mutex, this thread can also be
		 * boosted via PI, resulting in a thread that is throttled
		 * and boosted at the same time.
		 *
		 * In this case, the boost overrides the throttle.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   스레드 런타임의 오버런 감지 지연으로 인해 스레드가 런타임이 음수인 
 *   rt 뮤텍스에서 휴면 상태가 되는 경우가 있습니다. 결과적으로 스레드가 
 *   제한됩니다.
 *
 *   뮤텍스를 기다리는 동안 이 스레드는 PI를 통해 부스트될 수 있으므로 
 *   스레드가 동시에 스로틀링되고 부스트됩니다.
 *
 *   이 경우 부스트가 스로틀을 무시합니다.
 *
 * - boost + throttle
 *   주석의 상황 정리 
 *   1. overrun 상태이지만 확인이 안된상태(overrun 감지 지연)
 *   2. runtime이 -가 되면서 sleep이 됨.
 *   3. sleep을 기다리는 동안 pi를 통해 부스트 될수있음.
 *   4. 위의 상황에 의해 thread가 동시에 throttle되면서 boost도 된다.
 *   이 경우 boost가 throttle을 무시한다.
 *
 * - 위의 경우에 replenish 취소 및 throttle 상태를 0으로 갱신.
 */
		if (p->dl.dl_throttled) {
			/*
			 * The replenish timer needs to be canceled. No
			 * problem if it fires concurrently: boosted threads
			 * are ignored in dl_task_timer().
			 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   보충 타이머를 취소해야 합니다. 동시에 발생해도 문제 없습니다.
 *   향상된 스레드는 dl_task_timer()에서 무시됩니다.
 */
			hrtimer_try_to_cancel(&p->dl.dl_timer);
			p->dl.dl_throttled = 0;
		}
	} else if (!dl_prio(p->normal_prio)) {
		/*
		 * Special case in which we have a !SCHED_DEADLINE task that is going
		 * to be deboosted, but exceeds its runtime while doing so. No point in
		 * replenishing it, as it's going to return back to its original
		 * scheduling class after this. If it has been throttled, we need to
		 * clear the flag, otherwise the task may wake up as throttled after
		 * being boosted again with no means to replenish the runtime and clear
		 * the throttle.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   디부스트될 !SCHED_DEADLINE 작업이 있지만 그렇게 하는 동안 런타임을 
 *   초과하는 특별한 경우입니다. 이후에 원래 스케줄링 클래스로 돌아가므로 
 *   보충할 필요가 없습니다. 스로틀링된 경우 플래그를 지워야 합니다. 
 *   그렇지 않으면 작업이 다시 부스트된 후 런타임을 보충하고 스로틀을 
 *   지울 수단 없이 스로틀링된 상태로 깨어날 수 있습니다.
 *
 * - dl task가 갑자기 아니게 된경우. throttle을 0으로 하고 return.
 */
		p->dl.dl_throttled = 0;
		BUG_ON(!is_dl_boosted(&p->dl) || flags != ENQUEUE_REPLENISH);
		return;
	}

	/*
	 * Check if a constrained deadline task was activated
	 * after the deadline but before the next period.
	 * If that is the case, the task will be throttled and
	 * the replenishment timer will be set to the next period.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   제한된 기한 작업이 기한 이후이지만 다음 기간 이전에 활성화되었는지 
 *   확인하십시오.  이 경우 작업이 제한되고 보충 타이머가 다음 기간으로 
 *   설정됩니다.
 *
 * - throttle이 아니고, implicit dl이 아니라면 runtime초기화 및 throttle 
 *   설정 여부를 판단한다.
 */
	if (!p->dl.dl_throttled && !dl_is_implicit(&p->dl))
		dl_check_constrained_dl(&p->dl);

/*
 * IAMROOT, 2023.03.04:
 * - group간의 migrate or 설정중인 경우 rq의 this_bw와 running_bw에
 *   task의 bw를 추가한다.
 * - migrate중이므로 bw를 옮겨가는 rq에 추가해주는 개념.
 *   그 전에 dequeue할때는 해당 rq에서 빠졋을것이다.
 */
	if (p->on_rq == TASK_ON_RQ_MIGRATING || flags & ENQUEUE_RESTORE) {

		add_rq_bw(&p->dl, &rq->dl);
		add_running_bw(&p->dl, &rq->dl);
	}

	/*
	 * If p is throttled, we do not enqueue it. In fact, if it exhausted
	 * its budget it needs a replenishment and, since it now is on
	 * its rq, the bandwidth timer callback (which clearly has not
	 * run yet) will take care of this.
	 * However, the active utilization does not depend on the fact
	 * that the task is on the runqueue or not (but depends on the
	 * task's state - in GRUB parlance, "inactive" vs "active contending").
	 * In other words, even if a task is throttled its utilization must
	 * be counted in the active utilization; hence, we need to call
	 * add_running_bw().
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   p가 제한되면 대기열에 넣지 않습니다. 사실, 예산이 소진되면 보충이 
 *   필요하고, 이제 rq에 있기 때문에 bw timere cb(분명히 아직 
 *   실행되지 않음)이 이를 처리합니다.
 *   그러나 활성 사용률은 작업이 실행 대기열에 있는지 여부에 따라 
 *   달라지지 않습니다(그러나 작업 상태에 따라 달라집니다. GRUB 용어로는 
 *   비활성 대 활성 경합).
 *   즉, 작업이 제한되더라도 해당 사용률은 활성 사용률로 계산되어야 
 *   합니다. 따라서 add_running_bw()를 호출해야 합니다.
 *
 * - throttle중인데 enqueue를 시키면안된다.(replenish인 경우는 보충하러
 *   call한것이므로 제외.)
 */
	if (p->dl.dl_throttled && !(flags & ENQUEUE_REPLENISH)) {
/*
 * IAMROOT, 2023.03.04:
 * - task가 wakeup인 경우 active contending상태가 된다.
 *   (task_contending() 내부 주석참고).
 */
		if (flags & ENQUEUE_WAKEUP)
			task_contending(&p->dl, flags);

		return;
	}

	enqueue_dl_entity(&p->dl, flags);

/*
 * IAMROOT, 2023.03.04:
 * - curr가 아니고, migrate할 cpu가 있으면 pushable을 시도한다.
 */
	if (!task_current(rq, p) && p->nr_cpus_allowed > 1)
		enqueue_pushable_dl_task(rq, p);
}

static void __dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	dequeue_dl_entity(&p->dl);
	dequeue_pushable_dl_task(rq, p);
}

static void dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	update_curr_dl(rq);
	__dequeue_task_dl(rq, p, flags);

	if (p->on_rq == TASK_ON_RQ_MIGRATING || flags & DEQUEUE_SAVE) {
		sub_running_bw(&p->dl, &rq->dl);
		sub_rq_bw(&p->dl, &rq->dl);
	}

	/*
	 * This check allows to start the inactive timer (or to immediately
	 * decrease the active utilization, if needed) in two cases:
	 * when the task blocks and when it is terminating
	 * (p->state == TASK_DEAD). We can handle the two cases in the same
	 * way, because from GRUB's point of view the same thing is happening
	 * (the task moves from "active contending" to "active non contending"
	 * or "inactive")
	 */
	if (flags & DEQUEUE_SLEEP)
		task_non_contending(p);
}

/*
 * Yield task semantic for -deadline tasks is:
 *
 *   get off from the CPU until our next instance, with
 *   a new runtime. This is of little use now, since we
 *   don't have a bandwidth reclaiming mechanism. Anyway,
 *   bandwidth reclaiming is planned for the future, and
 *   yield_task_dl will indicate that some spare budget
 *   is available for other task instances to use it.
 */
static void yield_task_dl(struct rq *rq)
{
	/*
	 * We make the task go to sleep until its current deadline by
	 * forcing its runtime to zero. This way, update_curr_dl() stops
	 * it and the bandwidth timer will wake it up and will give it
	 * new scheduling parameters (thanks to dl_yielded=1).
	 */
	rq->curr->dl.dl_yielded = 1;

	update_rq_clock(rq);
	update_curr_dl(rq);
	/*
	 * Tell update_rq_clock() that we've just updated,
	 * so we don't do microscopic update in schedule()
	 * and double the fastpath cost.
	 */
	rq_clock_skip_update(rq);
}

#ifdef CONFIG_SMP

static int find_later_rq(struct task_struct *task);

/*
 * IAMROOT, 2023.03.04:
 * - @p를 어느 cpu에서 깨울건지 정한다.
 *   @p를 @cpu에서 동작시킬수없는 상황이면 다른 적합한 cpu를 찾아 반환한다.
 */
static int
select_task_rq_dl(struct task_struct *p, int cpu, int flags)
{
	struct task_struct *curr;
	bool select_rq;
	struct rq *rq;

/*
 * IAMROOT, 2023.03.04:
 * - task가 있지만 sleep에서 깨어나는 경우.
 *   wakeup상황에서만 현재 함수를 호출한다.
 */
	if (!(flags & WF_TTWU))
		goto out;

/*
 * IAMROOT, 2023.03.04:
 * - 일단 @cpu로 rq를 정한다.
 */
	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */

	/*
	 * If we are dealing with a -deadline task, we must
	 * decide where to wake it up.
	 * If it has a later deadline and the current task
	 * on this rq can't move (provided the waking task
	 * can!) we prefer to send it somewhere else. On the
	 * other hand, if it has a shorter deadline, we
	 * try to make it stay here, it might be important.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   -deadline 작업을 처리하는 경우 해당 작업을 깨울 위치를 결정해야 합니다.
 *   더 늦은 기한이 있고 이 rq의 현재 작업이 이동할 수 없는 경우(깨어 있는 
 *   작업이 이동할 수 있는 경우!) 다른 곳으로 보내는 것이 좋습니다. 반면 
 *   마감일이 더 짧다면 여기에 머물게 하려고 노력하는 것이 중요할 수 있습니다.
 *
 * - curr가 dl이고, @p가 다른 cpu에서 동작가능하고
 *   1. 현재 cpu에서만 curr가 동작 가능
 *   2. @curr가 더 @p보다 더 급한 마감.
 *
 *   이면 select_rq는 true가 된다. 즉 @p를 다른 cpu로 옮겨야되는 상황이다.
 */
	select_rq = unlikely(dl_task(curr)) &&
		    (curr->nr_cpus_allowed < 2 ||
		     !dl_entity_preempt(&p->dl, &curr->dl)) &&
		    p->nr_cpus_allowed > 1;

	/*
	 * Take the capacity of the CPU into account to
	 * ensure it fits the requirement of the task.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - HMP까지 고려해야되면 고려한다. @cpu에 적합하지않으면 떠나야된다.
 */
	if (static_branch_unlikely(&sched_asym_cpucapacity))
		select_rq |= !dl_task_fits_capacity(p, cpu);

	if (select_rq) {
/*
 * IAMROOT, 2023.03.04:
 * - 찾는다. 가장 만료시간이 느리고 적합한 cpu를 찾았고, @p의 만료시간이
 *   target보다 급하거나, target에 dl이 없다면 target으로 전환한다.
 */
		int target = find_later_rq(p);

		if (target != -1 &&
				(dl_time_before(p->dl.deadline,
					cpu_rq(target)->dl.earliest_dl.curr) ||
				(cpu_rq(target)->dl.dl_nr_running == 0)))
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}

static void migrate_task_rq_dl(struct task_struct *p, int new_cpu __maybe_unused)
{
	struct rq *rq;

	if (READ_ONCE(p->__state) != TASK_WAKING)
		return;

	rq = task_rq(p);
	/*
	 * Since p->state == TASK_WAKING, set_task_cpu() has been called
	 * from try_to_wake_up(). Hence, p->pi_lock is locked, but
	 * rq->lock is not... So, lock it
	 */
	raw_spin_rq_lock(rq);
	if (p->dl.dl_non_contending) {
		update_rq_clock(rq);
		sub_running_bw(&p->dl, &rq->dl);
		p->dl.dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be canceled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		if (hrtimer_try_to_cancel(&p->dl.inactive_timer) == 1)
			put_task_struct(p);
	}
	sub_rq_bw(&p->dl, &rq->dl);
	raw_spin_rq_unlock(rq);
}

/*
 * IAMROOT, 2023.03.04:
 * - 다음과 같은경우엔 reschedule 안한다.
 *   1. @curr가 현재 cpu에서만 돌수있으면 
 *   2. @curr에 적합한 cpudl을 못찾으면.
 *   3. @p가 여러군데 cpu에서 돌수있고, 다른 cpudl에서 돌수있는 상황
 *
 * - curr가 현재 cpu가 아닌 다른 cpu에서 돌수있고, @p가 현재 cpu에서만 돌수
 *   있는 상황이면 curr를 reschedule한다.
 */
static void check_preempt_equal_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * Current can't be migrated, useless to reschedule,
	 * let's hope p can move out.
	 */
	if (rq->curr->nr_cpus_allowed == 1 ||
	    !cpudl_find(&rq->rd->cpudl, rq->curr, NULL))
		return;

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	if (p->nr_cpus_allowed != 1 &&
	    cpudl_find(&rq->rd->cpudl, p, NULL))
		return;

	resched_curr(rq);
}

/*
 * IAMROOT, 2023.03.04:
 * - @p에서 dl가 동작을 안하고 있고, @p가 @rq의 우선순위가 높은상황,
 *   즉 dl task
 * --- push / pull을 하는 상황 정리.
 *  1. cpu에서 idle인 상황에서 pull
 *  2. core에서 balance을 call한 경우 push / pull
 *  3. sleep에서 깨어낫을때(woken) sleep에서 깨어난 task를 그냥 동작할지
 *     push를 할지 선택.
 *  4. balance_callback의 선택에 따른 처리.
 *     __schedule() 함수에서 post처리에서 상황에 따라 push / pull 선택
 * ---
 */
static int balance_dl(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
{
/*
 * IAMROOT, 2023.03.04:
 * - @p가 아직 rq에 없고, @rq(현재 task)보다 @p가(요청한 task) 
 *   deadline이면 pull작업을 수행한다.
 */
	if (!on_dl_rq(&p->dl) && need_pull_dl_task(rq, p)) {
		/*
		 * This is OK, because current is on_cpu, which avoids it being
		 * picked for load-balance and preemption/IRQs are still
		 * disabled avoiding further scheduler activity on it and we've
		 * not yet started the picking loop.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   current는 on_cpu이므로 부하 분산 및 선점/IRQ가 추가 스케줄러 활동을 
 *   방지하기 위해 여전히 비활성화되어 있고 아직 선택 루프를 시작하지 
 *   않았기 때문에 괜찮습니다.
 */
		rq_unpin_lock(rq, rf);
		pull_dl_task(rq);
		rq_repin_lock(rq, rf);
	}

	return sched_stop_runnable(rq) || sched_dl_runnable(rq);
}
#endif /* CONFIG_SMP */

/*
 * Only called when both the current and waking task are -deadline
 * tasks.
 */
/*
 * IAMROOT, 2023.03.04:
 * - @p가 curr보다 더 만료시간이 급한 deadline이라면 reschedule 요청한다.
 *   만약 deadline같다면 cpudl을 통해서 curr가 resched 가능여부를 확인해
 *   수행한다.
 */
static void check_preempt_curr_dl(struct rq *rq, struct task_struct *p,
				  int flags)
{
	if (dl_entity_preempt(&p->dl, &rq->curr->dl)) {
		resched_curr(rq);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * In the unlikely case current and p have the same deadline
	 * let us try to decide what's the best thing to do...
	 */
/*
 * IAMROOT, 2023.03.04:
 * - deadline이 동일한 상황에서 curr가 reschedule이 없다면
 *   (reschedule 요청이 있다면 이미 누군가 햇으므로 할필요가없다는것.)
 *   curr를 다른 cpu로 reschedule할수있는지 찾아서 가능하면 수행한다.
 */
	if ((p->dl.deadline == rq->curr->dl.deadline) &&
	    !test_tsk_need_resched(rq->curr))
		check_preempt_equal_dl(rq, p);
#endif /* CONFIG_SMP */
}

#ifdef CONFIG_SCHED_HRTICK

/*
 * IAMROOT, 2023.03.04:
 * - dl hrtick을 runtime이후에 동작한다.
 */
static void start_hrtick_dl(struct rq *rq, struct task_struct *p)
{
	hrtick_start(rq, p->dl.runtime);
}
#else /* !CONFIG_SCHED_HRTICK */
static void start_hrtick_dl(struct rq *rq, struct task_struct *p)
{
}
#endif

static void set_next_task_dl(struct rq *rq, struct task_struct *p, bool first)
{
	p->se.exec_start = rq_clock_task(rq);

	/* You can't push away the running task */
	dequeue_pushable_dl_task(rq, p);

	if (!first)
		return;

	if (hrtick_enabled_dl(rq))
		start_hrtick_dl(rq, p);

	if (rq->curr->sched_class != &dl_sched_class)
		update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	deadline_queue_push_tasks(rq);
}

static struct sched_dl_entity *pick_next_dl_entity(struct rq *rq,
						   struct dl_rq *dl_rq)
{
	struct rb_node *left = rb_first_cached(&dl_rq->root);

	if (!left)
		return NULL;

	return rb_entry(left, struct sched_dl_entity, rb_node);
}

static struct task_struct *pick_task_dl(struct rq *rq)
{
	struct sched_dl_entity *dl_se;
	struct dl_rq *dl_rq = &rq->dl;
	struct task_struct *p;

	if (!sched_dl_runnable(rq))
		return NULL;

	dl_se = pick_next_dl_entity(rq, dl_rq);
	BUG_ON(!dl_se);
	p = dl_task_of(dl_se);

	return p;
}

static struct task_struct *pick_next_task_dl(struct rq *rq)
{
	struct task_struct *p;

	p = pick_task_dl(rq);
	if (p)
		set_next_task_dl(rq, p, true);

	return p;
}

static void put_prev_task_dl(struct rq *rq, struct task_struct *p)
{
	update_curr_dl(rq);

	update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 1);
	if (on_dl_rq(&p->dl) && p->nr_cpus_allowed > 1)
		enqueue_pushable_dl_task(rq, p);
}

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   스케줄링 클래스의 작업을 치는 스케줄러 틱.
 *
 *   참고: 이 함수는 완전한 dynticks를 따라가는 틱 오프로드에 의해 원격으로 호출될 수
 *   있습니다. 따라서 로컬 가정을 할 수 없으며 매개 변수에 전달된 @rq 및 @curr를 통해
 *   모든 항목에 액세스해야 합니다.
 *
 * - runtime관련 수행
 *   (runtime 갱싱, throttle, reschedule, dequeue, enqueue등)
 * - rt bw rt_time 갱신.
 * - loadavg 갱신
 * - dl hrtick수행 여부확인.
 */
static void task_tick_dl(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_dl(rq);

	update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 1);
	/*
	 * Even when we have runtime, update_curr_dl() might have resulted in us
	 * not being the leftmost task anymore. In that case NEED_RESCHED will
	 * be set and schedule() will start a new hrtick for the next task.
	 */
	/*
	 * IAMROOT. 2023.02.25:
	 * - google-translate
	 *   런타임이 있는 경우에도 update_curr_dl()로 인해 더 이상 leftmost 작업이
	 *   아닐 수 있습니다. 이 경우 NEED_RESCHED가 설정되고 schedule()은
	 *   다음 작업을 위해 새로운 hrtick을 시작합니다.
	 *
	 * - hrtick을 지원하는 상황에서 queue에 들어가있고, 제일빠른 task인데
	 *   runtime이 남아있다면(계속 동작을해야되는 상태)
	 *   hrtick을 예약한다.
	 */
	if (hrtick_enabled_dl(rq) && queued && p->dl.runtime > 0 &&
	    is_leftmost(p, &rq->dl))
		start_hrtick_dl(rq, p);
}

static void task_fork_dl(struct task_struct *p)
{
	/*
	 * SCHED_DEADLINE tasks cannot fork and this is achieved through
	 * sched_fork()
	 */
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define DL_MAX_TRIES 3

static int pick_dl_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    cpumask_test_cpu(cpu, &p->cpus_mask))
		return 1;
	return 0;
}

/*
 * Return the earliest pushable rq's task, which is suitable to be executed
 * on the CPU, NULL otherwise:
 */
/*
 * IAMROOT, 2023.03.04:
 * - pick_highest_pushable_task() 참고.
 */
static struct task_struct *pick_earliest_pushable_dl_task(struct rq *rq, int cpu)
{
	struct rb_node *next_node = rq->dl.pushable_dl_tasks_root.rb_leftmost;
	struct task_struct *p = NULL;

	if (!has_pushable_dl_tasks(rq))
		return NULL;

next_node:
	if (next_node) {
		p = rb_entry(next_node, struct task_struct, pushable_dl_tasks);

		if (pick_dl_task(rq, p, cpu))
			return p;

		next_node = rb_next(next_node);
		goto next_node;
	}

	return NULL;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask_dl);

static int find_later_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *later_mask = this_cpu_cpumask_var_ptr(local_cpu_mask_dl);
	int this_cpu = smp_processor_id();
	int cpu = task_cpu(task);

	/* Make sure the mask is initialized first */
	if (unlikely(!later_mask))
		return -1;

	if (task->nr_cpus_allowed == 1)
		return -1;

	/*
	 * We have to consider system topology and task affinity
	 * first, then we can look for a suitable CPU.
	 */
	if (!cpudl_find(&task_rq(task)->rd->cpudl, task, later_mask))
		return -1;

	/*
	 * If we are here, some targets have been found, including
	 * the most suitable which is, among the runqueues where the
	 * current tasks have later deadlines than the task's one, the
	 * rq with the latest possible one.
	 *
	 * Now we check how well this matches with task's
	 * affinity and system topology.
	 *
	 * The last CPU where the task run is our first
	 * guess, since it is most likely cache-hot there.
	 */
	if (cpumask_test_cpu(cpu, later_mask))
		return cpu;
	/*
	 * Check if this_cpu is to be skipped (i.e., it is
	 * not in the mask) or not.
	 */
	if (!cpumask_test_cpu(this_cpu, later_mask))
		this_cpu = -1;

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * If possible, preempting this_cpu is
			 * cheaper than migrating.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			best_cpu = cpumask_any_and_distribute(later_mask,
							      sched_domain_span(sd));
			/*
			 * Last chance: if a CPU being in both later_mask
			 * and current sd span is valid, that becomes our
			 * choice. Of course, the latest possible CPU is
			 * already under consideration through later_mask.
			 */
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * At this point, all our guesses failed, we just return
	 * 'something', and let the caller sort the things out.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any_distribute(later_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

	return -1;
}

/* Locks the rq it finds */
/*
 * IAMROOT, 2023.03.04:
 * - later rq를 찾아 doublelock을 한후 return한다.
 *   rt측의 find_lock_lowest_rq()와 비슷한 방법으루 수행한다.
 */
static struct rq *find_lock_later_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *later_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < DL_MAX_TRIES; tries++) {
		cpu = find_later_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		later_rq = cpu_rq(cpu);

		if (later_rq->dl.dl_nr_running &&
		    !dl_time_before(task->dl.deadline,
					later_rq->dl.earliest_dl.curr)) {
			/*
			 * Target rq has tasks of equal or earlier deadline,
			 * retrying does not release any lock and is unlikely
			 * to yield a different result.
			 */
			later_rq = NULL;
			break;
		}

		/* Retry if something changed. */
		if (double_lock_balance(rq, later_rq)) {
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(later_rq->cpu, &task->cpus_mask) ||
				     task_running(rq, task) ||
				     !dl_task(task) ||
				     !task_on_rq_queued(task))) {
				double_unlock_balance(rq, later_rq);
				later_rq = NULL;
				break;
			}
		}

		/*
		 * If the rq we found has no -deadline task, or
		 * its earliest one has a later deadline than our
		 * task, the rq is a good one.
		 */
		if (!later_rq->dl.dl_nr_running ||
		    dl_time_before(task->dl.deadline,
				   later_rq->dl.earliest_dl.curr))
			break;

		/* Otherwise we try again. */
		double_unlock_balance(rq, later_rq);
		later_rq = NULL;
	}

	return later_rq;
}

static struct task_struct *pick_next_pushable_dl_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_dl_tasks(rq))
		return NULL;

	p = rb_entry(rq->dl.pushable_dl_tasks_root.rb_leftmost,
		     struct task_struct, pushable_dl_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(p->nr_cpus_allowed <= 1);

	BUG_ON(!task_on_rq_queued(p));
	BUG_ON(!dl_task(p));

	return p;
}

/*
 * See if the non running -deadline tasks on this rq
 * can be sent to some other CPU where they can preempt
 * and start executing.
 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   이 rq에서 실행되지 않는 -deadline 작업을 선점하고 실행을 시작할 수 있는 
 *   다른 CPU로 보낼 수 있는지 확인하십시오.
 *
 * - push_rt_task()와 비슷한 방법으로 동작한다.
 */
static int push_dl_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *later_rq;
	int ret = 0;

	if (!rq->dl.overloaded)
		return 0;

	next_task = pick_next_pushable_dl_task(rq);
	if (!next_task)
		return 0;

retry:
	if (is_migration_disabled(next_task))
		return 0;

	if (WARN_ON(next_task == rq->curr))
		return 0;

	/*
	 * If next_task preempts rq->curr, and rq->curr
	 * can move away, it makes sense to just reschedule
	 * without going further in pushing next_task.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   next_task가 rq->curr를 선점하고 rq->curr이 멀리 이동할 수 있는 경우 
 *   next_task를 더 이상 푸시하지 않고 일정을 다시 잡는 것이 좋습니다.
 * - curr가 dl이고, next가 curr보다 더 급한경우, curr를 reschedule 요청하여
 *   next로 전환되도록한다.
 */
	if (dl_task(rq->curr) &&
	    dl_time_before(next_task->dl.deadline, rq->curr->dl.deadline) &&
	    rq->curr->nr_cpus_allowed > 1) {
		resched_curr(rq);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* Will lock the rq it'll find */
	later_rq = find_lock_later_rq(next_task, rq);
	if (!later_rq) {
		struct task_struct *task;

		/*
		 * We must check all this again, since
		 * find_lock_later_rq releases rq->lock and it is
		 * then possible that next_task has migrated.
		 */
		task = pick_next_pushable_dl_task(rq);
		if (task == next_task) {
			/*
			 * The task is still there. We don't try
			 * again, some other CPU will pull it when ready.
			 */
			goto out;
		}

		if (!task)
			/* No more tasks */
			goto out;

		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	set_task_cpu(next_task, later_rq->cpu);

	/*
	 * Update the later_rq clock here, because the clock is used
	 * by the cpufreq_update_util() inside __add_running_bw().
	 */
	update_rq_clock(later_rq);
	activate_task(later_rq, next_task, ENQUEUE_NOCLOCK);
	ret = 1;

	resched_curr(later_rq);

	double_unlock_balance(rq, later_rq);

out:
	put_task_struct(next_task);

	return ret;
}

static void push_dl_tasks(struct rq *rq)
{
	/* push_dl_task() will return true if it moved a -deadline task */
	while (push_dl_task(rq))
		;
}

/*
 * IAMROOT, 2023.03.04:
 * - pull_rt_task() 참조
 */
static void pull_dl_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	struct task_struct *p, *push_task;
	bool resched = false;
	struct rq *src_rq;
	u64 dmin = LONG_MAX;

	if (likely(!dl_overloaded(this_rq)))
		return;

	/*
	 * Match the barrier from dl_set_overloaded; this guarantees that if we
	 * see overloaded we must also see the dlo_mask bit.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   dl_set_overloaded의 장벽을 일치시킵니다. 이는 오버로드된 것을 볼 경우
 *   dlo_mask 비트도 확인해야 함을 보장합니다.
 */
	smp_rmb();

/*
 * IAMROOT, 2023.03.04:
 * - overload된 cpu들을 순회하면서 가장 급한 task를 찾아와서
 */
	for_each_cpu(cpu, this_rq->rd->dlo_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * It looks racy, abd it is! However, as in sched_rt.c,
		 * we are fine with this.
		 */
/*
 * IAMROOT, 2023.03.04:
 * - 순회중인 src_rq보다 this가 더 급하면 skip한다.
 */
		if (this_rq->dl.dl_nr_running &&
		    dl_time_before(this_rq->dl.earliest_dl.curr,
				   src_rq->dl.earliest_dl.next))
			continue;

		/* Might drop this_rq->lock */
		push_task = NULL;
		double_lock_balance(this_rq, src_rq);

		/*
		 * If there are no more pullable tasks on the
		 * rq, we're done with it.
		 */
		if (src_rq->dl.dl_nr_running <= 1)
			goto skip;

/*
 * IAMROOT, 2023.03.04:
 * - 가장 급한 pushable dl task를 얻어온다.
 */
		p = pick_earliest_pushable_dl_task(src_rq, this_cpu);

		/*
		 * We found a task to be pulled if:
		 *  - it preempts our current (if there's one),
		 *  - it will preempt the last one we pulled (if any).
		 */
/*
 * IAMROOT, 2023.03.04:
 * - 제일 급한 dl task를 찾아서 기록한다. 찾으면 resched true.
 */
		if (p && dl_time_before(p->dl.deadline, dmin) &&
		    (!this_rq->dl.dl_nr_running ||
		     dl_time_before(p->dl.deadline,
				    this_rq->dl.earliest_dl.curr))) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!task_on_rq_queued(p));

			/*
			 * Then we pull iff p has actually an earlier
			 * deadline than the current task of its runqueue.
			 */
			if (dl_time_before(p->dl.deadline,
					   src_rq->curr->dl.deadline))
				goto skip;

			if (is_migration_disabled(p)) {
				push_task = get_push_task(src_rq);
			} else {
				deactivate_task(src_rq, p, 0);
				set_task_cpu(p, this_cpu);
				activate_task(this_rq, p, 0);
				dmin = p->dl.deadline;
				resched = true;
			}

			/* Is there any other task even earlier? */
		}
skip:
		double_unlock_balance(this_rq, src_rq);

/*
 * IAMROOT, 2023.03.04:
 * - curr를 옮겨야하는경우 stop후 옮긴다.
 */
		if (push_task) {
			raw_spin_rq_unlock(this_rq);
			stop_one_cpu_nowait(src_rq->cpu, push_cpu_stop,
					    push_task, &src_rq->push_work);
			raw_spin_rq_lock(this_rq);
		}
	}

	if (resched)
		resched_curr(this_rq);
}

/*
 * Since the task is not running and a reschedule is not going to happen
 * anytime soon on its runqueue, we try pushing it away now.
 */
static void task_woken_dl(struct rq *rq, struct task_struct *p)
{
	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    p->nr_cpus_allowed > 1 &&
	    dl_task(rq->curr) &&
	    (rq->curr->nr_cpus_allowed < 2 ||
	     !dl_entity_preempt(&p->dl, &rq->curr->dl))) {
		push_dl_tasks(rq);
	}
}

static void set_cpus_allowed_dl(struct task_struct *p,
				const struct cpumask *new_mask,
				u32 flags)
{
	struct root_domain *src_rd;
	struct rq *rq;

	BUG_ON(!dl_task(p));

	rq = task_rq(p);
	src_rd = rq->rd;
	/*
	 * Migrating a SCHED_DEADLINE task between exclusive
	 * cpusets (different root_domains) entails a bandwidth
	 * update. We already made space for us in the destination
	 * domain (see cpuset_can_attach()).
	 */
	if (!cpumask_intersects(src_rd->span, new_mask)) {
		struct dl_bw *src_dl_b;

		src_dl_b = dl_bw_of(cpu_of(rq));
		/*
		 * We now free resources of the root_domain we are migrating
		 * off. In the worst case, sched_setattr() may temporary fail
		 * until we complete the update.
		 */
		raw_spin_lock(&src_dl_b->lock);
		__dl_sub(src_dl_b, p->dl.dl_bw, dl_bw_cpus(task_cpu(p)));
		raw_spin_unlock(&src_dl_b->lock);
	}

	set_cpus_allowed_common(p, new_mask, flags);
}

/* Assumes rq->lock is held */
static void rq_online_dl(struct rq *rq)
{
	if (rq->dl.overloaded)
		dl_set_overload(rq);

	cpudl_set_freecpu(&rq->rd->cpudl, rq->cpu);
	if (rq->dl.dl_nr_running > 0)
		cpudl_set(&rq->rd->cpudl, rq->cpu, rq->dl.earliest_dl.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_dl(struct rq *rq)
{
	if (rq->dl.overloaded)
		dl_clear_overload(rq);

	cpudl_clear(&rq->rd->cpudl, rq->cpu);
	cpudl_clear_freecpu(&rq->rd->cpudl, rq->cpu);
}

void __init init_sched_dl_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i)
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask_dl, i),
					GFP_KERNEL, cpu_to_node(i));
}

void dl_add_task_root_domain(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;
	struct dl_bw *dl_b;

	raw_spin_lock_irqsave(&p->pi_lock, rf.flags);
	if (!dl_task(p)) {
		raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
		return;
	}

	rq = __task_rq_lock(p, &rf);

	dl_b = &rq->rd->dl_bw;
	raw_spin_lock(&dl_b->lock);

	__dl_add(dl_b, p->dl.dl_bw, cpumask_weight(rq->rd->span));

	raw_spin_unlock(&dl_b->lock);

	task_rq_unlock(rq, p, &rf);
}

void dl_clear_root_domain(struct root_domain *rd)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rd->dl_bw.lock, flags);
	rd->dl_bw.total_bw = 0;
	raw_spin_unlock_irqrestore(&rd->dl_bw.lock, flags);
}

#endif /* CONFIG_SMP */

static void switched_from_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * task_non_contending() can start the "inactive timer" (if the 0-lag
	 * time is in the future). If the task switches back to dl before
	 * the "inactive timer" fires, it can continue to consume its current
	 * runtime using its current deadline. If it stays outside of
	 * SCHED_DEADLINE until the 0-lag time passes, inactive_task_timer()
	 * will reset the task parameters.
	 */
	if (task_on_rq_queued(p) && p->dl.dl_runtime)
		task_non_contending(p);

	if (!task_on_rq_queued(p)) {
		/*
		 * Inactive timer is armed. However, p is leaving DEADLINE and
		 * might migrate away from this rq while continuing to run on
		 * some other class. We need to remove its contribution from
		 * this rq running_bw now, or sub_rq_bw (below) will complain.
		 */
		if (p->dl.dl_non_contending)
			sub_running_bw(&p->dl, &rq->dl);
		sub_rq_bw(&p->dl, &rq->dl);
	}

	/*
	 * We cannot use inactive_task_timer() to invoke sub_running_bw()
	 * at the 0-lag time, because the task could have been migrated
	 * while SCHED_OTHER in the meanwhile.
	 */
	if (p->dl.dl_non_contending)
		p->dl.dl_non_contending = 0;

	/*
	 * Since this might be the only -deadline task on the rq,
	 * this is the right place to try to pull some other one
	 * from an overloaded CPU, if any.
	 */
	if (!task_on_rq_queued(p) || rq->dl.dl_nr_running)
		return;

	deadline_queue_pull_task(rq);
}

/*
 * When switching to -deadline, we may overload the rq, then
 * we try to push someone off, if possible.
 */
static void switched_to_dl(struct rq *rq, struct task_struct *p)
{
	if (hrtimer_try_to_cancel(&p->dl.inactive_timer) == 1)
		put_task_struct(p);

	/* If p is not queued we will update its parameters at next wakeup. */
	if (!task_on_rq_queued(p)) {
		add_rq_bw(&p->dl, &rq->dl);

		return;
	}

	if (rq->curr != p) {
#ifdef CONFIG_SMP
		if (p->nr_cpus_allowed > 1 && rq->dl.overloaded)
			deadline_queue_push_tasks(rq);
#endif
		if (dl_task(rq->curr))
			check_preempt_curr_dl(rq, p, 0);
		else
			resched_curr(rq);
	} else {
		update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 0);
	}
}

/*
 * If the scheduling parameters of a -deadline task changed,
 * a push or pull operation might be needed.
 */
static void prio_changed_dl(struct rq *rq, struct task_struct *p,
			    int oldprio)
{
	if (task_on_rq_queued(p) || task_current(rq, p)) {
#ifdef CONFIG_SMP
		/*
		 * This might be too much, but unfortunately
		 * we don't have the old deadline value, and
		 * we can't argue if the task is increasing
		 * or lowering its prio, so...
		 */
		if (!rq->dl.overloaded)
			deadline_queue_pull_task(rq);

		/*
		 * If we now have a earlier deadline task than p,
		 * then reschedule, provided p is still on this
		 * runqueue.
		 */
		if (dl_time_before(rq->dl.earliest_dl.curr, p->dl.deadline))
			resched_curr(rq);
#else
		/*
		 * Again, we don't know if p has a earlier
		 * or later deadline, so let's blindly set a
		 * (maybe not needed) rescheduling point.
		 */
		resched_curr(rq);
#endif /* CONFIG_SMP */
	}
}

DEFINE_SCHED_CLASS(dl) = {

	.enqueue_task		= enqueue_task_dl,
	.dequeue_task		= dequeue_task_dl,
	.yield_task		= yield_task_dl,

	.check_preempt_curr	= check_preempt_curr_dl,

	.pick_next_task		= pick_next_task_dl,
	.put_prev_task		= put_prev_task_dl,
	.set_next_task		= set_next_task_dl,

#ifdef CONFIG_SMP
	.balance		= balance_dl,
	.pick_task		= pick_task_dl,
	.select_task_rq		= select_task_rq_dl,
	.migrate_task_rq	= migrate_task_rq_dl,
	.set_cpus_allowed       = set_cpus_allowed_dl,
	.rq_online              = rq_online_dl,
	.rq_offline             = rq_offline_dl,
	.task_woken		= task_woken_dl,
	.find_lock_rq		= find_lock_later_rq,
#endif

	.task_tick		= task_tick_dl,
	.task_fork              = task_fork_dl,

	.prio_changed           = prio_changed_dl,
	.switched_from		= switched_from_dl,
	.switched_to		= switched_to_dl,

	.update_curr		= update_curr_dl,
};

/* Used for dl_bw check and update, used under sched_rt_handler()::mutex */
static u64 dl_generation;

int sched_dl_global_validate(void)
{
	u64 runtime = global_rt_runtime();
	u64 period = global_rt_period();
	u64 new_bw = to_ratio(period, runtime);
	u64 gen = ++dl_generation;
	struct dl_bw *dl_b;
	int cpu, cpus, ret = 0;
	unsigned long flags;

	/*
	 * Here we want to check the bandwidth not being set to some
	 * value smaller than the currently allocated bandwidth in
	 * any of the root_domains.
	 */
	for_each_possible_cpu(cpu) {
		rcu_read_lock_sched();

		if (dl_bw_visited(cpu, gen))
			goto next;

		dl_b = dl_bw_of(cpu);
		cpus = dl_bw_cpus(cpu);

		raw_spin_lock_irqsave(&dl_b->lock, flags);
		if (new_bw * cpus < dl_b->total_bw)
			ret = -EBUSY;
		raw_spin_unlock_irqrestore(&dl_b->lock, flags);

next:
		rcu_read_unlock_sched();

		if (ret)
			break;
	}

	return ret;
}

/*
 * IAMROOT, 2022.11.26:
 * - throttle 여부에 따라서 초기화.
 */
static void init_dl_rq_bw_ratio(struct dl_rq *dl_rq)
{
	if (global_rt_runtime() == RUNTIME_INF) {
		/*
		 * IAMROOT, 2023.02.26:
		 * - throttle을 안하는경우
		 * - bw_ratio : 1 << 8 = 256
		 * - extra_bw : 1 << 20 = 1048576
		 */
		dl_rq->bw_ratio = 1 << RATIO_SHIFT;
		dl_rq->extra_bw = 1 << BW_SHIFT;
	} else {
		/*
		 * IAMROOT, 2023.02.26:
		 * - throttle을 하는 경우.
		 * - bw_ratio :
		 *   (global_rt_period << 20) / global_rt_runtime >> (BW_SHIFT -
		 *   RATIO_SHIFT)
		 *   ((1,000,000,000 << 20) / 950,000,000) >> (20 - 8) = 269
		 *   NOTE: 원래 to_ratio 함수의 의도와 달리 runtime 과 period 인수를
		 *   반대로하여 호출하고 있다.
		 * - extra_bw :
		 *   (global_rt_runtime << 20) / global_rt_period
		 *   (950,000,000 << 20) / 1,000,000,000 = 996147
		 */
		dl_rq->bw_ratio = to_ratio(global_rt_runtime(),
			  global_rt_period()) >> (BW_SHIFT - RATIO_SHIFT);
		dl_rq->extra_bw = to_ratio(global_rt_period(),
						    global_rt_runtime());
	}
}

void sched_dl_do_global(void)
{
	u64 new_bw = -1;
	u64 gen = ++dl_generation;
	struct dl_bw *dl_b;
	int cpu;
	unsigned long flags;

	def_dl_bandwidth.dl_period = global_rt_period();
	def_dl_bandwidth.dl_runtime = global_rt_runtime();

	if (global_rt_runtime() != RUNTIME_INF)
		new_bw = to_ratio(global_rt_period(), global_rt_runtime());

	for_each_possible_cpu(cpu) {
		rcu_read_lock_sched();

		if (dl_bw_visited(cpu, gen)) {
			rcu_read_unlock_sched();
			continue;
		}

		dl_b = dl_bw_of(cpu);

		raw_spin_lock_irqsave(&dl_b->lock, flags);
		dl_b->bw = new_bw;
		raw_spin_unlock_irqrestore(&dl_b->lock, flags);

		rcu_read_unlock_sched();
		init_dl_rq_bw_ratio(&cpu_rq(cpu)->dl);
	}
}

/*
 * We must be sure that accepting a new task (or allowing changing the
 * parameters of an existing one) is consistent with the bandwidth
 * constraints. If yes, this function also accordingly updates the currently
 * allocated bandwidth to reflect the new situation.
 *
 * This function is called while holding p's rq->lock.
 */
int sched_dl_overflow(struct task_struct *p, int policy,
		      const struct sched_attr *attr)
{
	u64 period = attr->sched_period ?: attr->sched_deadline;
	u64 runtime = attr->sched_runtime;
	u64 new_bw = dl_policy(policy) ? to_ratio(period, runtime) : 0;
	int cpus, err = -1, cpu = task_cpu(p);
	struct dl_bw *dl_b = dl_bw_of(cpu);
	unsigned long cap;

	if (attr->sched_flags & SCHED_FLAG_SUGOV)
		return 0;

	/* !deadline task may carry old deadline bandwidth */
	if (new_bw == p->dl.dl_bw && task_has_dl_policy(p))
		return 0;

	/*
	 * Either if a task, enters, leave, or stays -deadline but changes
	 * its parameters, we may need to update accordingly the total
	 * allocated bandwidth of the container.
	 */
	raw_spin_lock(&dl_b->lock);
	cpus = dl_bw_cpus(cpu);
	cap = dl_bw_capacity(cpu);

	if (dl_policy(policy) && !task_has_dl_policy(p) &&
	    !__dl_overflow(dl_b, cap, 0, new_bw)) {
		if (hrtimer_active(&p->dl.inactive_timer))
			__dl_sub(dl_b, p->dl.dl_bw, cpus);
		__dl_add(dl_b, new_bw, cpus);
		err = 0;
	} else if (dl_policy(policy) && task_has_dl_policy(p) &&
		   !__dl_overflow(dl_b, cap, p->dl.dl_bw, new_bw)) {
		/*
		 * XXX this is slightly incorrect: when the task
		 * utilization decreases, we should delay the total
		 * utilization change until the task's 0-lag point.
		 * But this would require to set the task's "inactive
		 * timer" when the task is not inactive.
		 */
		__dl_sub(dl_b, p->dl.dl_bw, cpus);
		__dl_add(dl_b, new_bw, cpus);
		dl_change_utilization(p, new_bw);
		err = 0;
	} else if (!dl_policy(policy) && task_has_dl_policy(p)) {
		/*
		 * Do not decrease the total deadline utilization here,
		 * switched_from_dl() will take care to do it at the correct
		 * (0-lag) time.
		 */
		err = 0;
	}
	raw_spin_unlock(&dl_b->lock);

	return err;
}

/*
 * This function initializes the sched_dl_entity of a newly becoming
 * SCHED_DEADLINE task.
 *
 * Only the static values are considered here, the actual runtime and the
 * absolute deadline will be properly calculated when the task is enqueued
 * for the first time with its new policy.
 */
void __setparam_dl(struct task_struct *p, const struct sched_attr *attr)
{
	struct sched_dl_entity *dl_se = &p->dl;

	dl_se->dl_runtime = attr->sched_runtime;
	dl_se->dl_deadline = attr->sched_deadline;
	dl_se->dl_period = attr->sched_period ?: dl_se->dl_deadline;
	dl_se->flags = attr->sched_flags & SCHED_DL_FLAGS;
	dl_se->dl_bw = to_ratio(dl_se->dl_period, dl_se->dl_runtime);
	dl_se->dl_density = to_ratio(dl_se->dl_deadline, dl_se->dl_runtime);
}

void __getparam_dl(struct task_struct *p, struct sched_attr *attr)
{
	struct sched_dl_entity *dl_se = &p->dl;

	attr->sched_priority = p->rt_priority;
	attr->sched_runtime = dl_se->dl_runtime;
	attr->sched_deadline = dl_se->dl_deadline;
	attr->sched_period = dl_se->dl_period;
	attr->sched_flags &= ~SCHED_DL_FLAGS;
	attr->sched_flags |= dl_se->flags;
}

/*
 * Default limits for DL period; on the top end we guard against small util
 * tasks still getting ridiculously long effective runtimes, on the bottom end we
 * guard against timer DoS.
 */
unsigned int sysctl_sched_dl_period_max = 1 << 22; /* ~4 seconds */
unsigned int sysctl_sched_dl_period_min = 100;     /* 100 us */

/*
 * This function validates the new parameters of a -deadline task.
 * We ask for the deadline not being zero, and greater or equal
 * than the runtime, as well as the period of being zero or
 * greater than deadline. Furthermore, we have to be sure that
 * user parameters are above the internal resolution of 1us (we
 * check sched_runtime only since it is always the smaller one) and
 * below 2^63 ns (we have to check both sched_deadline and
 * sched_period, as the latter can be zero).
 */
bool __checkparam_dl(const struct sched_attr *attr)
{
	u64 period, max, min;

	/* special dl tasks don't actually use any parameter */
	if (attr->sched_flags & SCHED_FLAG_SUGOV)
		return true;

	/* deadline != 0 */
	if (attr->sched_deadline == 0)
		return false;

	/*
	 * Since we truncate DL_SCALE bits, make sure we're at least
	 * that big.
	 */
	if (attr->sched_runtime < (1ULL << DL_SCALE))
		return false;

	/*
	 * Since we use the MSB for wrap-around and sign issues, make
	 * sure it's not set (mind that period can be equal to zero).
	 */
	if (attr->sched_deadline & (1ULL << 63) ||
	    attr->sched_period & (1ULL << 63))
		return false;

	period = attr->sched_period;
	if (!period)
		period = attr->sched_deadline;

	/* runtime <= deadline <= period (if period != 0) */
	if (period < attr->sched_deadline ||
	    attr->sched_deadline < attr->sched_runtime)
		return false;

	max = (u64)READ_ONCE(sysctl_sched_dl_period_max) * NSEC_PER_USEC;
	min = (u64)READ_ONCE(sysctl_sched_dl_period_min) * NSEC_PER_USEC;

	if (period < min || period > max)
		return false;

	return true;
}

/*
 * This function clears the sched_dl_entity static params.
 */
void __dl_clear_params(struct task_struct *p)
{
	struct sched_dl_entity *dl_se = &p->dl;

	dl_se->dl_runtime		= 0;
	dl_se->dl_deadline		= 0;
	dl_se->dl_period		= 0;
	dl_se->flags			= 0;
	dl_se->dl_bw			= 0;
	dl_se->dl_density		= 0;

	dl_se->dl_throttled		= 0;
	dl_se->dl_yielded		= 0;
	dl_se->dl_non_contending	= 0;
	dl_se->dl_overrun		= 0;

#ifdef CONFIG_RT_MUTEXES
	dl_se->pi_se			= dl_se;
#endif
}

bool dl_param_changed(struct task_struct *p, const struct sched_attr *attr)
{
	struct sched_dl_entity *dl_se = &p->dl;

	if (dl_se->dl_runtime != attr->sched_runtime ||
	    dl_se->dl_deadline != attr->sched_deadline ||
	    dl_se->dl_period != attr->sched_period ||
	    dl_se->flags != (attr->sched_flags & SCHED_DL_FLAGS))
		return true;

	return false;
}

#ifdef CONFIG_SMP
int dl_task_can_attach(struct task_struct *p, const struct cpumask *cs_cpus_allowed)
{
	unsigned long flags, cap;
	unsigned int dest_cpu;
	struct dl_bw *dl_b;
	bool overflow;
	int ret;

	dest_cpu = cpumask_any_and(cpu_active_mask, cs_cpus_allowed);

	rcu_read_lock_sched();
	dl_b = dl_bw_of(dest_cpu);
	raw_spin_lock_irqsave(&dl_b->lock, flags);
	cap = dl_bw_capacity(dest_cpu);
	overflow = __dl_overflow(dl_b, cap, 0, p->dl.dl_bw);
	if (overflow) {
		ret = -EBUSY;
	} else {
		/*
		 * We reserve space for this task in the destination
		 * root_domain, as we can't fail after this point.
		 * We will free resources in the source root_domain
		 * later on (see set_cpus_allowed_dl()).
		 */
		int cpus = dl_bw_cpus(dest_cpu);

		__dl_add(dl_b, p->dl.dl_bw, cpus);
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&dl_b->lock, flags);
	rcu_read_unlock_sched();

	return ret;
}

int dl_cpuset_cpumask_can_shrink(const struct cpumask *cur,
				 const struct cpumask *trial)
{
	int ret = 1, trial_cpus;
	struct dl_bw *cur_dl_b;
	unsigned long flags;

	rcu_read_lock_sched();
	cur_dl_b = dl_bw_of(cpumask_any(cur));
	trial_cpus = cpumask_weight(trial);

	raw_spin_lock_irqsave(&cur_dl_b->lock, flags);
	if (cur_dl_b->bw != -1 &&
	    cur_dl_b->bw * trial_cpus < cur_dl_b->total_bw)
		ret = 0;
	raw_spin_unlock_irqrestore(&cur_dl_b->lock, flags);
	rcu_read_unlock_sched();

	return ret;
}

bool dl_cpu_busy(unsigned int cpu)
{
	unsigned long flags, cap;
	struct dl_bw *dl_b;
	bool overflow;

	rcu_read_lock_sched();
	dl_b = dl_bw_of(cpu);
	raw_spin_lock_irqsave(&dl_b->lock, flags);
	cap = dl_bw_capacity(cpu);
	overflow = __dl_overflow(dl_b, cap, 0, 0);
	raw_spin_unlock_irqrestore(&dl_b->lock, flags);
	rcu_read_unlock_sched();

	return overflow;
}
#endif

#ifdef CONFIG_SCHED_DEBUG
void print_dl_stats(struct seq_file *m, int cpu)
{
	print_dl_rq(m, cpu, &cpu_rq(cpu)->dl);
}
#endif /* CONFIG_SCHED_DEBUG */
