/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Only give sleepers 50% of their service deficit. This allows
 * them to run sooner, but does not allow tons of sleepers to
 * rip the spread apart.
 */
SCHED_FEAT(GENTLE_FAIR_SLEEPERS, true)

/*
 * Place new tasks ahead so that they do not starve already running
 * tasks
 */
SCHED_FEAT(START_DEBIT, true)

/*
 * Prefer to schedule the task we woke last (assuming it failed
 * wakeup-preemption), since its likely going to consume data we
 * touched, increases cache locality.
 */
SCHED_FEAT(NEXT_BUDDY, false)

/*
 * Prefer to schedule the task that ran last (when we did
 * wake-preempt) as that likely will touch the same data, increases
 * cache locality.
 */
SCHED_FEAT(LAST_BUDDY, true)

/*
 * Consider buddies to be cache hot, decreases the likeliness of a
 * cache buddy being migrated away, increases cache locality.
 */
/*
 * IAMROOT. 2023.05.13:
 * - google-translate
 * 버디를 캐시 핫으로 간주하고, 캐시 버디가 멀리 마이그레이션될 가능성을 줄이고,
 * 캐시 지역성을 높입니다.
 */
SCHED_FEAT(CACHE_HOT_BUDDY, true)

/*
 * Allow wakeup-time preemption of the current task:
 */
SCHED_FEAT(WAKEUP_PREEMPTION, true)

SCHED_FEAT(HRTICK, false)
SCHED_FEAT(HRTICK_DL, false)
SCHED_FEAT(DOUBLE_TICK, false)

/*
 * Decrement CPU capacity based on time not spent running tasks
 */
SCHED_FEAT(NONTASK_CAPACITY, true)

/*
 * Queue remote wakeups on the target CPU and process them
 * using the scheduler IPI. Reduces rq->lock contention/bounces.
 */
/*
 * IAMROOT. 2023.05.20:
 * - google-translate
 * 대상 CPU에서 원격 깨우기를 대기하고 스케줄러 IPI를 사용하여 처리합니다. rq->잠금
 * 경합/바운스를 줄입니다.
 */
SCHED_FEAT(TTWU_QUEUE, true)

/*
 * When doing wakeups, attempt to limit superfluous scans of the LLC domain.
 */
SCHED_FEAT(SIS_PROP, true)

/*
 * Issue a WARN when we do multiple update_rq_clock() calls
 * in a single rq->lock section. Default disabled because the
 * annotations are not complete.
 */
SCHED_FEAT(WARN_DOUBLE_CLOCK, false)

#ifdef HAVE_RT_PUSH_IPI
/*
 * In order to avoid a thundering herd attack of CPUs that are
 * lowering their priorities at the same time, and there being
 * a single CPU that has an RT task that can migrate and is waiting
 * to run, where the other CPUs will try to take that CPUs
 * rq lock and possibly create a large contention, sending an
 * IPI to that CPU and let that CPU push the RT task to where
 * it should go may be a better scenario.
 */

/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   동시에 우선 순위를 낮추고 마이그레이션할 수 있고 실행 대기 중인 
 *   RT 작업이 있는 단일 CPU가 있는 CPU의 thundering herd attack을 피하기 
 *   위해 다른 CPU가 해당 CPU를 차지하려고 합니다. rq를 잠그고 가능하면 
 *   대규모 경합을 생성하여 해당 CPU에 IPI를 보내고 해당 CPU가 RT 
 *   작업을 가야 할 곳으로 푸시하도록 하는 것이 더 나은 시나리오일 
 *   수 있습니다.
 */
SCHED_FEAT(RT_PUSH_IPI, true)
#endif

SCHED_FEAT(RT_RUNTIME_SHARE, false)
SCHED_FEAT(LB_MIN, false)
SCHED_FEAT(ATTACH_AGE_LOAD, true)

SCHED_FEAT(WA_IDLE, true)
SCHED_FEAT(WA_WEIGHT, true)
SCHED_FEAT(WA_BIAS, true)

/*
 * UtilEstimation. Use estimated CPU utilization.
 */
SCHED_FEAT(UTIL_EST, true)
SCHED_FEAT(UTIL_EST_FASTUP, true)

SCHED_FEAT(LATENCY_WARN, false)

SCHED_FEAT(ALT_PERIOD, true)
SCHED_FEAT(BASE_SLICE, true)
