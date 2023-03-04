// SPDX-License-Identifier: GPL-2.0
/*
 * Real-Time Scheduling Class (mapped to the SCHED_FIFO and SCHED_RR
 * policies)
 */
#include "sched.h"

#include "pelt.h"
/*
 * IAMROOT, 2023.02.10:
 * - 100ms
 */
int sched_rr_timeslice = RR_TIMESLICE;
int sysctl_sched_rr_timeslice = (MSEC_PER_SEC / HZ) * RR_TIMESLICE;
/* More than 4 hours if BW_SHIFT equals 20. */
static const u64 max_rt_runtime = MAX_BW;

static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun);

struct rt_bandwidth def_rt_bandwidth;

/*
 * IAMROOT, 2023.02.18:
 * - 1. timer expire시 rt_period시간에 따라 overrun을 얻어온다. 
 *      일반적으로 1.
 *   2. overrun에 따라 rt bw에 속한 rt들에 대해 다음을 수행한다.
 *     -- 설정시간 및 실행시간을 재계산한다.
 *     -- throttle이 풀릴수 있는 상황이면 푼다
 *     -- enqueue할 task가 있으면 enqueue한다.
 *   3. bw을 실행할 이유가 없으면 timer를 종료하고 그게아니면 재예약을 한다.
 */
static enum hrtimer_restart sched_rt_period_timer(struct hrtimer *timer)
{
	struct rt_bandwidth *rt_b =
		container_of(timer, struct rt_bandwidth, rt_period_timer);
	int idle = 0;
	int overrun;

	raw_spin_lock(&rt_b->rt_runtime_lock);

/*
 * IAMROOT, 2023.02.18:
 * - for문의 동작원리 
 *   1. 최초 timer expire. hrtimer_forward_now에서 일반적으로 overrun = 1
 *   2. for문동작
 *   3. 다시 hrtimer_forward_now 진입. expire안됬으므로 overrun = 0.
 *   4. for문 break되며 idle에 따라 return.
 */
	for (;;) {
/*
 * IAMROOT, 2023.02.18:
 * - expire 안됨 : overrun = 0
 *   rt_period 보다 짧은 시간에 expire : overrun = 1
 *   rt_period 보다 긴 시간에 expire : overrun = rt_period 배수
 */
		overrun = hrtimer_forward_now(timer, rt_b->rt_period);
		if (!overrun)
			break;

		raw_spin_unlock(&rt_b->rt_runtime_lock);

/*
 * IAMROOT, 2023.02.18:
 * - 만약 특수한 상황에따라 여기서 많은 시간이 소요되면 next iterate에서
 *   overrun이 발생할수도 있다.
 */
		idle = do_sched_rt_period_timer(rt_b, overrun);
		raw_spin_lock(&rt_b->rt_runtime_lock);
	}

/*
 * IAMROOT, 2023.02.18:
 * - idle이 있다면 timer가 쉬게되는 개념이 되므로 active를 clear
 */
	if (idle)
		rt_b->rt_period_active = 0;
	raw_spin_unlock(&rt_b->rt_runtime_lock);

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

/*
 * IAMROOT, 2022.11.26:
 * - rt bw 초기화.
 * - global_rt_period인 경우 rt_period는 1초
 */
void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime)
{
	rt_b->rt_period = ns_to_ktime(period);
	rt_b->rt_runtime = runtime;

	raw_spin_lock_init(&rt_b->rt_runtime_lock);

	hrtimer_init(&rt_b->rt_period_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL_HARD);
	rt_b->rt_period_timer.function = sched_rt_period_timer;
}

/*
 * IAMROOT, 2023.02.18:
 * - bw설정이 안되있으면 수행을 안한다.
 * - bw 동작 예약이 안되있으면 예약을 한다.
 *   (sched_rt_period_timer())
 */
static void start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
		return;

	raw_spin_lock(&rt_b->rt_runtime_lock);
	if (!rt_b->rt_period_active) {
		rt_b->rt_period_active = 1;
		/*
		 * SCHED_DEADLINE updates the bandwidth, as a run away
		 * RT task with a DL task could hog a CPU. But DL does
		 * not reset the period. If a deadline task was running
		 * without an RT task running, it can cause RT tasks to
		 * throttle when they start up. Kick the timer right away
		 * to update the period.
		 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   SCHED_DEADLINE은 대역폭을 업데이트합니다. DL 작업이 포함된 런어웨이 
 *   RT 작업이 CPU를 독차지할 수 있기 때문입니다. 그러나 DL은 기간을 
 *   재설정하지 않습니다. RT 작업이 실행되지 않고 마감일 작업이 실행 
 *   중인 경우 RT 작업이 시작될 때 조절될 수 있습니다. 기간을 
 *   업데이트하려면 타이머를 바로 킥하십시오.
 *
 * - 최대한 빨리 hrtimer가 동작하게 한다.
 */
		hrtimer_forward_now(&rt_b->rt_period_timer, ns_to_ktime(0));
		hrtimer_start_expires(&rt_b->rt_period_timer,
				      HRTIMER_MODE_ABS_PINNED_HARD);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

/*
 * IAMROOT, 2022.11.26:
 * - rt rq 초기화.
 */
void init_rt_rq(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array;
	int i;

	array = &rt_rq->active;

/*
 * IAMROOT, 2022.11.26:
 * - delimiter 방식을 채용. MAX_RT_PRIO로 101번째 end bit를 설정한다.
 */
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	__set_bit(MAX_RT_PRIO, array->bitmap);

#if defined CONFIG_SMP
	rt_rq->highest_prio.curr = MAX_RT_PRIO-1;
	rt_rq->highest_prio.next = MAX_RT_PRIO-1;
	rt_rq->rt_nr_migratory = 0;
	rt_rq->overloaded = 0;
	plist_head_init(&rt_rq->pushable_tasks);
#endif /* CONFIG_SMP */
	/* We start is dequeued state, because no RT tasks are queued */
	rt_rq->rt_queued = 0;

	rt_rq->rt_time = 0;
	rt_rq->rt_throttled = 0;
	rt_rq->rt_runtime = 0;
	raw_spin_lock_init(&rt_rq->rt_runtime_lock);
}

#ifdef CONFIG_RT_GROUP_SCHED
static void destroy_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	hrtimer_cancel(&rt_b->rt_period_timer);
}

#define rt_entity_is_task(rt_se) (!(rt_se)->my_q)

static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
#ifdef CONFIG_SCHED_DEBUG
	WARN_ON_ONCE(!rt_entity_is_task(rt_se));
#endif
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return rt_rq->rq;
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	return rt_se->rt_rq;
}

static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_se->rt_rq;

	return rt_rq->rq;
}

void free_rt_sched_group(struct task_group *tg)
{
	int i;

	if (tg->rt_se)
		destroy_rt_bandwidth(&tg->rt_bandwidth);

	for_each_possible_cpu(i) {
		if (tg->rt_rq)
			kfree(tg->rt_rq[i]);
		if (tg->rt_se)
			kfree(tg->rt_se[i]);
	}

	kfree(tg->rt_rq);
	kfree(tg->rt_se);
}

/*
 * IAMROOT, 2022.11.26:
 * - @cpu에 해당하는 rq를 @rt_rq에 등록한다.
 */
void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	rt_rq->highest_prio.curr = MAX_RT_PRIO-1;
	rt_rq->rt_nr_boosted = 0;
	rt_rq->rq = rq;
	rt_rq->tg = tg;

	tg->rt_rq[cpu] = rt_rq;
	tg->rt_se[cpu] = rt_se;

	if (!rt_se)
		return;

	if (!parent)
		rt_se->rt_rq = &rq->rt;
	else
		rt_se->rt_rq = parent->my_q;

	rt_se->my_q = rt_rq;
	rt_se->parent = parent;
	INIT_LIST_HEAD(&rt_se->run_list);
}

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct rt_rq *rt_rq;
	struct sched_rt_entity *rt_se;
	int i;

	tg->rt_rq = kcalloc(nr_cpu_ids, sizeof(rt_rq), GFP_KERNEL);
	if (!tg->rt_rq)
		goto err;
	tg->rt_se = kcalloc(nr_cpu_ids, sizeof(rt_se), GFP_KERNEL);
	if (!tg->rt_se)
		goto err;

	init_rt_bandwidth(&tg->rt_bandwidth,
			ktime_to_ns(def_rt_bandwidth.rt_period), 0);

	for_each_possible_cpu(i) {
		rt_rq = kzalloc_node(sizeof(struct rt_rq),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_rq)
			goto err;

		rt_se = kzalloc_node(sizeof(struct sched_rt_entity),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_se)
			goto err_free_rq;

		init_rt_rq(rt_rq);
		rt_rq->rt_runtime = tg->rt_bandwidth.rt_runtime;
		init_tg_rt_entry(tg, rt_rq, rt_se, i, parent->rt_se[i]);
	}

	return 1;

err_free_rq:
	kfree(rt_rq);
err:
	return 0;
}

#else /* CONFIG_RT_GROUP_SCHED */

#define rt_entity_is_task(rt_se) (1)

/*
 * IAMROOT, 2023.02.10:
 * - +-- task ------------------------+
 *   | ..                             |
 *   | struct sched_rt_entity rt_se   |
 *   | ..                             |
 *   +--------------------------------+
 * - @rt_se는 task에 embed된 struct sched_rt_entity 일것이다.
 */
static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

/*
 * IAMROOT, 2023.02.10:
 * - @rt_rq는 struct rq에 소속된 struct rt_rq일 것이다.
 */
static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt);
}

/*
 * IAMROOT, 2023.02.04:
 * - return: rt_se task의 per cpu rq
 * - @rt_se는 task에 embed되있다. rt_se가 소속된 task의 rq를 가져온다.
 *   @rt_se -> task -(task cpu)-> rq 의 접근으로 가져온다.
 * 1. task를 알아낸다.
 *   +-- task ------------------------+
 *   | ..                             |
 *   | struct sched_rt_entity rt_se   |
 *   | ..                             |
 *   +-------------------------------+
 * 2. 알아낸 task를 통해서 cpu nr을 알아온다.
 * 3. cpu nr을 통해 cpu rq를 가져온다.
 */
static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct task_struct *p = rt_task_of(rt_se);

	return task_rq(p);
}

/*
 * IAMROOT, 2023.02.10:
 * - @rt_se의 rq를 가져온다. @rt_se는 task에 embed된 rt_se이다.
 *   @rt_se -> task -(task cpu)-> cpu rq -> cpu rq의 rt_rq 의 접근으로 가져온다.
 */
static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	return &rq->rt;
}

void free_rt_sched_group(struct task_group *tg) { }

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SMP

static void pull_rt_task(struct rq *this_rq);

/*
 * IAMROOT, 2023.02.11:
 * - @rq가 online이고 @prev 우선순위보다 낮다면(숫자가 낮을수록 높음) return true
 */
static inline bool need_pull_rt_task(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull RT tasks here if we lower this rq's prio */
	return rq->online && rq->rt.highest_prio.curr > prev->prio;
}

/*
 * IAMROOT, 2023.02.11:
 * - rt overloaded task 수를 return한다.
 */
static inline int rt_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->rto_count);
}


/*
 * IAMROOT, 2023.02.18:
 * - @rq의 cpu를 rtomask에 set하고 rto_count를 증가시킨다.
 *   overload된 cpu를 기록하는 기능이다.
 */
static inline void rt_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->rto_mask);
	/*
	 * Make sure the mask is visible before we set
	 * the overload count. That is checked to determine
	 * if we should look at the mask. It would be a shame
	 * if we looked at the mask, but the mask was not
	 * updated yet.
	 *
	 * Matched by the barrier in pull_rt_task().
	 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *  과부하 수를 설정하기 전에 마스크가 보이는지 확인하십시오. 마스크를 
 *  확인해야 하는지 확인하기 위해 확인합니다. 마스크를 본다면 아쉬울 
 *  텐데 마스크가 아직 업데이트되지 않았습니다.
 *
 *  pull_rt_task()의 장벽과 일치합니다.
 *  
 *  - cpu mask를 set 및 증가하는 경우 mask set -> count up 순으로 진행한다.
 */
	smp_wmb();
	atomic_inc(&rq->rd->rto_count);
}

/*
 * IAMROOT, 2023.02.18:
 * - @rq의 cpu에 대한 정보를 rto mask와 count에서 제거한다.
 */
static inline void rt_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	/* the order here really doesn't matter */
/*
 * IAMROOT, 2023.02.18:
 *  - cpu mask를 clear 및 감소하는 경우 count down -> mask clear 
 *  순으로 진행한다.
 */
	atomic_dec(&rq->rd->rto_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->rto_mask);
}

/*
 * IAMROOT, 2023.02.18:
 * - overload를 해야되는 상황에 호출되면 overload시키고,
 *   overload를 풀어야되는 상황에서 호출되면 overload를 푼다.
 */
static void update_rt_migration(struct rt_rq *rt_rq)
{

/*
 * IAMROOT, 2023.02.18:
 * - rt_rq->rt_nr_migratory != 0
 *   migrate가능한 task가 있으면서 cpu가 두개 이상인 환경
 * - 1. if문을 해석하면 cpu가 두개 이상 이다.
 *   2. migrate가능 한 task가 있다.
 *   3. rt task가 2개이상 있다.
 */
	if (rt_rq->rt_nr_migratory && rt_rq->rt_nr_total > 1) {
/*
 * IAMROOT, 2023.02.18:
 * - @rt_rq가 overload되는 상황이다. @rt_rq의 cpu에 대한 정보를
 *   rt_rq의 rd의 rto에 기록한다.
 */
		if (!rt_rq->overloaded) {
			rt_set_overload(rq_of_rt_rq(rt_rq));
			rt_rq->overloaded = 1;
		}
	} else if (rt_rq->overloaded) {
/*
 * IAMROOT, 2023.02.18:
 * - overload가 풀리는 상황에서 update_rt_migration이 호출되는 상황.
 *   @rt_rq에 대한 overload를 푼다.
 */
		rt_clear_overload(rq_of_rt_rq(rt_rq));
		rt_rq->overloaded = 0;
	}
}

/*
 * IAMROOT, 2023.02.18:
 * - @rt_se가 task인 경우에만 count를 증가하고 overload에 대해서 갱신한다.
 *   (overload가 안됬으면 overload로 전환될수도있다.)
 */
static void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;

	if (!rt_entity_is_task(rt_se))
		return;

	p = rt_task_of(rt_se);
	rt_rq = &rq_of_rt_rq(rt_rq)->rt;

	rt_rq->rt_nr_total++;
	if (p->nr_cpus_allowed > 1)
		rt_rq->rt_nr_migratory++;

	update_rt_migration(rt_rq);
}

/*
 * IAMROOT, 2023.02.18:
 * - @rt_se가 task인 경우에만 count를 빼고 overload에 대해서 갱신한다.
 *   (overload가 된상태였으면 overload가 풀릴수 있다.)
 */
static void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;

	if (!rt_entity_is_task(rt_se))
		return;

	p = rt_task_of(rt_se);
	rt_rq = &rq_of_rt_rq(rt_rq)->rt;

	rt_rq->rt_nr_total--;
	if (p->nr_cpus_allowed > 1)
		rt_rq->rt_nr_migratory--;

	update_rt_migration(rt_rq);
}

/*
 * IAMROOT, 2023.02.11:
 * - pushable_tasks가 있으면 return 1
 */

static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->rt.pushable_tasks);
}

static DEFINE_PER_CPU(struct callback_head, rt_push_head);
static DEFINE_PER_CPU(struct callback_head, rt_pull_head);

static void push_rt_tasks(struct rq *);
static void pull_rt_task(struct rq *);

/*
 * IAMROOT, 2023.02.11:
 * - pushable_tasks가 있으면 push를 지정(push_rt_tasks)한다.
 *   (pull의 경우 pull_rt_task가 지정된다.)
 *   
 */
static inline void rt_queue_push_tasks(struct rq *rq)
{
	if (!has_pushable_tasks(rq))
		return;

	queue_balance_callback(rq, &per_cpu(rt_push_head, rq->cpu), push_rt_tasks);
}

static inline void rt_queue_pull_task(struct rq *rq)
{
	queue_balance_callback(rq, &per_cpu(rt_pull_head, rq->cpu), pull_rt_task);
}

/*
 * IAMROOT, 2023.02.11:
 * - @p를 pushable task로 enqueue한다.
 */
static void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
/*
 * IAMROOT, 2023.02.11:
 * - 이미 등록된 상황일 수도 있다. 일단 삭제하고 다시 추가한다.
 */
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the highest prio pushable task */
/*
 * IAMROOT, 2023.02.11:
 * - highest_prio.next 갱신.
 */
	if (p->prio < rq->rt.highest_prio.next)
		rq->rt.highest_prio.next = p->prio;
}

/*
 * IAMROOT, 2023.02.11:
 * - pushable_tasks
 *   같은 cpu에서 두개 이상의 rt task가 있는 경우 다른 cpu로 밀어내기 
 *   위한것
 * - pushable_tasks에 등록되있는 경우 일단 dequeue한다.
 */
static void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the new highest prio pushable task */
/*
 * IAMROOT, 2023.02.11:
 * - 여전시 pushable tasks가 있으면 highest prio를 기록하고 그게 아니면
 *   max로 설정한다.
 */
	if (has_pushable_tasks(rq)) {
		p = plist_first_entry(&rq->rt.pushable_tasks,
				      struct task_struct, pushable_tasks);
		rq->rt.highest_prio.next = p->prio;
	} else {
		rq->rt.highest_prio.next = MAX_RT_PRIO-1;
	}
}

#else

static inline void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline
void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline bool need_pull_rt_task(struct rq *rq, struct task_struct *prev)
{
	return false;
}

static inline void pull_rt_task(struct rq *this_rq)
{
}

static inline void rt_queue_push_tasks(struct rq *rq)
{
}
#endif /* CONFIG_SMP */

static void enqueue_top_rt_rq(struct rt_rq *rt_rq);
static void dequeue_top_rt_rq(struct rt_rq *rt_rq);

/*
 * IAMROOT, 2023.02.11:
 * - rq에 들어있는지 확인.
 */
static inline int on_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->on_rq;
}

#ifdef CONFIG_UCLAMP_TASK
/*
 * Verify the fitness of task @p to run on @cpu taking into account the uclamp
 * settings.
 *
 * This check is only important for heterogeneous systems where uclamp_min value
 * is higher than the capacity of a @cpu. For non-heterogeneous system this
 * function will always return true.
 *
 * The function will return true if the capacity of the @cpu is >= the
 * uclamp_min and false otherwise.
 *
 * Note that uclamp_min will be clamped to uclamp_max if uclamp_min
 * > uclamp_max.
 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   uclamp 설정을 고려하여 @cpu에서 실행할 작업 @p의 적합성을 확인합니다. 
 *
 *   이 검사는 uclamp_min 값이 @cpu의 용량보다 큰 이기종 시스템에서만 
 *   중요합니다. 이기종 시스템이 아닌 경우 이 함수는 항상 true를 반환합니다.
 *
 *   이 함수는 @cpu의 용량이 >= uclamp_min이면 true를 반환하고 그렇지 
 *   않으면 false를 반환합니다. 
 *
 *   uclamp_min > uclamp_max인 경우 uclamp_min은 uclamp_max로 고정됩니다.
 *
 * --- UCLAMP ---
 * - application의 경우 background, forground등에서 동작한다.
 *   어떤것들은 절전모드에서 조그맣게 동작하는걸 원하고,
 *   어떤것들은 강력하게 동작하는것을 원한다.
 *   예를들어 A라는 app이 background에서 가끔돌아도 cpu를 많이 사용해야된다 
 *   라던가
 *   벤치마킹 프로그램처럼 무조건 cpu를 많이 사용해야되는 것이 있다.
 *   이러한 상황들을 위해 제한을 시킨다.
 *   max는 Big cpu의 max freq 기준으로 정한다.
 * --------------
 *
 * - @p에서 설정한 uclamp의 min/max중 @cpu의 rq가 최소값을 넘으면 
 *   return true.
 */
static inline bool rt_task_fits_capacity(struct task_struct *p, int cpu)
{
	unsigned int min_cap;
	unsigned int max_cap;
	unsigned int cpu_cap;

	/* Only heterogeneous systems can benefit from this check */
	if (!static_branch_unlikely(&sched_asym_cpucapacity))
		return true;

	min_cap = uclamp_eff_value(p, UCLAMP_MIN);
	max_cap = uclamp_eff_value(p, UCLAMP_MAX);

	cpu_cap = capacity_orig_of(cpu);

	return cpu_cap >= min(min_cap, max_cap);
}
#else
static inline bool rt_task_fits_capacity(struct task_struct *p, int cpu)
{
	return true;
}
#endif

#ifdef CONFIG_RT_GROUP_SCHED

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	if (!rt_rq->tg)
		return RUNTIME_INF;

	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(rt_rq->tg->rt_bandwidth.rt_period);
}

typedef struct task_group *rt_rq_iter_t;

static inline struct task_group *next_task_group(struct task_group *tg)
{
	do {
		tg = list_entry_rcu(tg->list.next,
			typeof(struct task_group), list);
	} while (&tg->list != &task_groups && task_group_is_autogroup(tg));

	if (&tg->list == &task_groups)
		tg = NULL;

	return tg;
}

#define for_each_rt_rq(rt_rq, iter, rq)					\
	for (iter = container_of(&task_groups, typeof(*iter), list);	\
		(iter = next_task_group(iter)) &&			\
		(rt_rq = iter->rt_rq[cpu_of(rq)]);)

/*
 * IAMROOT, 2023.02.10:
 * - parent로 타고 올라간다.
 */
#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = rt_se->parent)

/*
 * IAMROOT, 2023.02.11:
 * - @rt_se가 entity인지 task인지 확인한다. my_q가 있으면 entity(group)
 *   return != NULL : group entity이며 my_q를 가져옴.
 *          NULL    : task
 */
static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->my_q;
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags);
static void dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags);

/*
 * IAMROOT, 2023.02.18:
 * - @rt_rq가 root group인경우 top enqueue(+cpufreq_update_util 수행).
 *   그렇지 않은 경우 rt_se로 enqueue
 *   enqueue후 highest가 변경됫으면 reschedule을 수행한다.
 */
static void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct task_struct *curr = rq_of_rt_rq(rt_rq)->curr;
	struct rq *rq = rq_of_rt_rq(rt_rq);
	struct sched_rt_entity *rt_se;

	int cpu = cpu_of(rq);

	rt_se = rt_rq->tg->rt_se[cpu];

	if (rt_rq->rt_nr_running) {
		if (!rt_se)
			enqueue_top_rt_rq(rt_rq);
		else if (!on_rt_rq(rt_se))
			enqueue_rt_entity(rt_se, 0);

		if (rt_rq->highest_prio.curr < curr->prio)
			resched_curr(rq);
	}
}

/*
 * IAMROOT, 2023.02.18:
 * - root group인 경우 top에서 dequeue 및 cpufreq_update_util 수행
 *   그렇지 않은경우 rt_se를 dequeue한다.
 */
static void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	struct sched_rt_entity *rt_se;
	int cpu = cpu_of(rq_of_rt_rq(rt_rq));

	rt_se = rt_rq->tg->rt_se[cpu];

	if (!rt_se) {
		dequeue_top_rt_rq(rt_rq);
		/* Kick cpufreq (see the comment in kernel/sched/sched.h). */
		cpufreq_update_util(rq_of_rt_rq(rt_rq), 0);
	}
	else if (on_rt_rq(rt_se))
		dequeue_rt_entity(rt_se, 0);
}

/*
 * IAMROOT, 2023.02.18:
 * - throttled인지 확인하는데, boost상황이면 throttle이 아니라고 판단한다.
 */
static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled && !rt_rq->rt_nr_boosted;
}

/*
 * IAMROOT, 2023.02.18:
 * @return 1 : boost가 되있는 상황.
 *             boost된 task가 있거나, task인데 boost인 상황.
 */
static int rt_se_boosted(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = group_rt_rq(rt_se);
	struct task_struct *p;


/*
 * IAMROOT, 2023.02.18:
 * - group entity이면 boost된 task가 있는지로 판단한다. 
 */
	if (rt_rq)
		return !!rt_rq->rt_nr_boosted;

	p = rt_task_of(rt_se);

/*
 * IAMROOT, 2023.02.18:
 * - 보통은 normal_prio와 prio랑은 같은데, 다를 경우 boost된 상황으로
 *   판단한다.
 */
	return p->prio != p->normal_prio;
}

#ifdef CONFIG_SMP
/*
 * IAMROOT, 2023.02.18:
 * - root domain에 참가한 cpu 목록.
 */
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return this_rq()->rd->span;
}
#else
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}
#endif

/*
 * IAMROOT, 2023.02.10:
 * - @rt_b가 속한 task_group에서 @cpu에 해당하는 rt_rq를 가져온다.
 */
static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return container_of(rt_b, struct task_group, rt_bandwidth)->rt_rq[cpu];
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &rt_rq->tg->rt_bandwidth;
}

#else /* !CONFIG_RT_GROUP_SCHED */

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(def_rt_bandwidth.rt_period);
}

typedef struct rt_rq *rt_rq_iter_t;

#define for_each_rt_rq(rt_rq, iter, rq) \
	for ((void) iter, rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = NULL)

static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return NULL;
}

static inline void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (!rt_rq->rt_nr_running)
		return;

	enqueue_top_rt_rq(rt_rq);
	resched_curr(rq);
}

/*
 * IAMROOT, 2023.02.10:
 * - rt_rq를 rt_rq가 속한 rq에서 dequeue한다..
 */
static inline void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	dequeue_top_rt_rq(rt_rq);
}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled;
}

static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}

/*
 * IAMROOT, 2023.02.10:
 * - rt_bandwidth가 없는 버전. pcp rq에서 rt_rq를 가져온다.
 */
static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return &cpu_rq(cpu)->rt;
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &def_rt_bandwidth;
}

#endif /* CONFIG_RT_GROUP_SCHED */

bool sched_rt_bandwidth_account(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

	return (hrtimer_active(&rt_b->rt_period_timer) ||
		rt_rq->rt_time < rt_b->rt_runtime);
}

#ifdef CONFIG_SMP
/*
 * We ran out of runtime, see if we can borrow some from our neighbours.
 */
/*
 * IAMROOT, 2023.02.04:
 * - 이미 실행된 시간을 보충하기 위하여 다른 cpu 의 남은 시간에서 모자란 시간 만큼을
 *   span cpu 갯수만큼 나누어 보충해 온다.
 */
static void do_balance_runtime(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
	struct root_domain *rd = rq_of_rt_rq(rt_rq)->rd;
	int i, weight;
	u64 rt_period;

	weight = cpumask_weight(rd->span);

	raw_spin_lock(&rt_b->rt_runtime_lock);
	rt_period = ktime_to_ns(rt_b->rt_period);
/*
 * IAMROOT, 2023.02.10:
 * - root_domain에 속한 cpu를 iterate한다.
 */
	for_each_cpu(i, rd->span) {
		struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
		s64 diff;

		/*
		 * IAMROOT, 2023.02.04:
		 * - 자기 자신(인자로 들어온 rt_rq)은 건너뛴다.
		 */
		if (iter == rt_rq)
			continue;

		raw_spin_lock(&iter->rt_runtime_lock);
		/*
		 * Either all rqs have inf runtime and there's nothing to steal
		 * or __disable_runtime() below sets a specific rq to inf to
		 * indicate its been disabled and disallow stealing.
		 */
		/*
		 * IAMROOT. 2023.02.04:
		 * - google-translate
		 *   모든 rq에는 inf 런타임이 있고 steal할 것이 없거나 아래의 __disable_runtime()이
		 *   특정 rq를 inf로 설정하여 비활성화되었음을 나타내고 steal을 허용하지 않습니다.
		 */
		if (iter->rt_runtime == RUNTIME_INF)
			goto next;

		/*
		 * From runqueues with spare time, take 1/n part of their
		 * spare time, but no more than our period.
		 */
		/*
		 * IAMROOT. 2023.02.04:
		 * - google-translate
		 *   여유 시간이 있는 실행 대기열에서 여가 시간의 1/n을 사용하되 우리 기간보다 더
		 *   많이 사용하지 마십시오.
		 * - diff > 0 이면 빌려줄수 있다.
		 */
		diff = iter->rt_runtime - iter->rt_time;
		if (diff > 0) {
			diff = div_u64((u64)diff, weight);
			if (rt_rq->rt_runtime + diff > rt_period)
				diff = rt_period - rt_rq->rt_runtime;
			iter->rt_runtime -= diff;
			rt_rq->rt_runtime += diff;
			/*
			 * IAMROOT, 2023.02.04:
			 * - runtime을 최대치로 충전했으면 더이상 진행할 필요 없다
			 */
			if (rt_rq->rt_runtime == rt_period) {
				raw_spin_unlock(&iter->rt_runtime_lock);
				break;
			}
		}
next:
		raw_spin_unlock(&iter->rt_runtime_lock);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

/*
 * Ensure this RQ takes back all the runtime it lend to its neighbours.
 */
static void __disable_runtime(struct rq *rq)
{
	struct root_domain *rd = rq->rd;
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
		s64 want;
		int i;

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * Either we're all inf and nobody needs to borrow, or we're
		 * already disabled and thus have nothing to do, or we have
		 * exactly the right amount of runtime to take out.
		 */
		if (rt_rq->rt_runtime == RUNTIME_INF ||
				rt_rq->rt_runtime == rt_b->rt_runtime)
			goto balanced;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);

		/*
		 * Calculate the difference between what we started out with
		 * and what we current have, that's the amount of runtime
		 * we lend and now have to reclaim.
		 */
		want = rt_b->rt_runtime - rt_rq->rt_runtime;

		/*
		 * Greedy reclaim, take back as much as we can.
		 */
		for_each_cpu(i, rd->span) {
			struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
			s64 diff;

			/*
			 * Can't reclaim from ourselves or disabled runqueues.
			 */
			if (iter == rt_rq || iter->rt_runtime == RUNTIME_INF)
				continue;

			raw_spin_lock(&iter->rt_runtime_lock);
			if (want > 0) {
				diff = min_t(s64, iter->rt_runtime, want);
				iter->rt_runtime -= diff;
				want -= diff;
			} else {
				iter->rt_runtime -= want;
				want -= want;
			}
			raw_spin_unlock(&iter->rt_runtime_lock);

			if (!want)
				break;
		}

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We cannot be left wanting - that would mean some runtime
		 * leaked out of the system.
		 */
		BUG_ON(want);
balanced:
		/*
		 * Disable all the borrow logic by pretending we have inf
		 * runtime - in which case borrowing doesn't make sense.
		 */
		rt_rq->rt_runtime = RUNTIME_INF;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);

		/* Make rt_rq available for pick_next_task() */
		sched_rt_rq_enqueue(rt_rq);
	}
}

static void __enable_runtime(struct rq *rq)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	/*
	 * Reset each runqueue's bandwidth settings
	 */
	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = rt_b->rt_runtime;
		rt_rq->rt_time = 0;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);
	}
}

/*
 * IAMROOT, 2023.02.10:
 * - RT_RUNTIME_SHARE가 켜져있고, 실행시간(rt_time)이 설정시간(rt_runtime)
 *   보다 크면 do_balance_runtime을 수행한다.
 *   over한 시간만큼 다른 cpu에서 빼온다.
 */
static void balance_runtime(struct rt_rq *rt_rq)
{
	/*
	 * IAMROOT, 2023.02.04:
	 * - default false. cpu 시간을 배분 할수 있는 기능
	 */
	if (!sched_feat(RT_RUNTIME_SHARE))
		return;

	if (rt_rq->rt_time > rt_rq->rt_runtime) {
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		do_balance_runtime(rt_rq);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
	}
}
#else /* !CONFIG_SMP */
static inline void balance_runtime(struct rt_rq *rt_rq) {}
#endif /* CONFIG_SMP */

/*
 * IAMROOT, 2023.02.18:
 * - 1. bw의 설정값(runtime)을 rt_rq에 설정한다
 *   2. 동작중인 rt_rq들에 대해서 실행시간을 설정시간에 따라 감소한다.
 *   3. 재계산된 실행시간에 따라 throttle을 푼다.
 *   4. throttle을 풀거나 새로 들어온 rt_rq라면 enqueue한다.
 *   5. rt 실행시간이 있거나, rt task가 있는경우, 새로 들어온 rt가 있는 
 *      경우 timer를 재예약(return 0.) 한다.
 *   6. rt 가 없거나 bw가 비활성화 된경우등은 timer를 끈다(return 1)
 */
static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun)
{
	int i, idle = 1, throttled = 0;
	const struct cpumask *span;

/*
 * IAMROOT, 2023.02.18:
 * - this_rq의 root domain의 cpumask를 가져온다.
 */
	span = sched_rt_period_mask();
#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * FIXME: isolated CPUs should really leave the root task group,
	 * whether they are isolcpus or were isolated via cpusets, lest
	 * the timer run on a CPU which does not service all runqueues,
	 * potentially leaving other CPUs indefinitely throttled.  If
	 * isolation is really required, the user will turn the throttle
	 * off to kill the perturbations it causes anyway.  Meanwhile,
	 * this maintains functionality for boot and/or troubleshooting.
	 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   FIXME: 격리된 CPU는 isolcpus이든 cpusets를 통해 격리되었는지 
 *   여부에 관계없이 루트 작업 그룹을 떠나야 합니다. 타이머가 모든 
 *   실행 대기열을 서비스하지 않는 CPU에서 실행되어 잠재적으로 다른 
 *   CPU가 무기한 스로틀링되지 않도록 합니다. 격리가 실제로 필요한 경우 
 *   사용자는 스로틀을 해제하여 발생하는 섭동을 제거합니다. 한편, 이것은 
 *   부팅 및/또는 문제 해결을 위한 기능을 유지합니다.
 *
 * - @rt_b가 root_task_group의 rt bw이면 그냥 전체 cpu로 span을 정한다.
 *   특정 상황에서 무기한 스로틀링이 발생할수 있어 이를 방지하기 위함
 *   이라고 한다.
 */
	if (rt_b == &root_task_group.rt_bandwidth)
		span = cpu_online_mask;
#endif
	for_each_cpu(i, span) {
		int enqueue = 0;
		struct rt_rq *rt_rq = sched_rt_period_rt_rq(rt_b, i);
		struct rq *rq = rq_of_rt_rq(rt_rq);
		int skip;

		/*
		 * When span == cpu_online_mask, taking each rq->lock
		 * can be time-consuming. Try to avoid it when possible.
		 */
		raw_spin_lock(&rt_rq->rt_runtime_lock);
/*
 * IAMROOT, 2023.02.18:
 * - RT_RUNTIME_SHARE기능이 없고, rt_rq가 운영중인  상황에서
 *   bw를 rt_rq에 충전한다.(무조건 bw와 설정시간을 동일하게 하는 개념.)
 */
		if (!sched_feat(RT_RUNTIME_SHARE) && rt_rq->rt_runtime != RUNTIME_INF)
			rt_rq->rt_runtime = rt_b->rt_runtime;

/*
 * IAMROOT, 2023.02.18:
 * - 실행시간이 없고, task도 없는 경우엔 skip한다.
 */
		skip = !rt_rq->rt_time && !rt_rq->rt_nr_running;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		if (skip)
			continue;

		raw_spin_rq_lock(rq);
		update_rq_clock(rq);

		if (rt_rq->rt_time) {
			u64 runtime;

			raw_spin_lock(&rt_rq->rt_runtime_lock);

/*
 * IAMROOT, 2023.02.18:
 * - throttle 된경우에만 balance_runtime을 수행한다.
 */
			if (rt_rq->rt_throttled)
				balance_runtime(rt_rq);
			runtime = rt_rq->rt_runtime;
/*
 * IAMROOT, 2023.02.18:
 * - overrun * runtime 만큼 rt_time에서 뺀다.
 * - 일반적인 상황(overrun = 1)
 *   1) rt_time <= runtime 
 *      설정된 시간을 다 못쓰거나 딱 맞춰서 다 쓴 상황.
 *      처음부터 시작하는 개념으로 0으로 초기화.
 *   2) rt_time > runtime
 *      설정된 시간 이상 쓴상황. 다음 peroid에서 넘게 쓴시간을 남겨놓
 *      는 개념이된다.
 *
 * - overrun이 1이 아닌상황(바쁜상황)
 *   timer가 여러번 동작했어야됬는데 못하는 상황. runtime이 overrun횟수만
 *   큼 계산이 됬어야됫므로 overrun을 runtime에 배수해서 계산한다.
 */
			rt_rq->rt_time -= min(rt_rq->rt_time, overrun*runtime);

/*
 * IAMROOT, 2023.02.18:
 * - rt_time < runtime : 설정시간보다 적게 썻으므로 현재 throttle이 
 *   아닌 상황이다.
 * - 이전에 throttle이였고 남은 잔량이 runtime보다 작다면 
 *   throttle을 풀어준다.
 * - rt_throttled은 실행시간이 설정시간보다 많이 동작햇을 경우 set
 *   됫을것이다.
 */
			if (rt_rq->rt_throttled && rt_rq->rt_time < runtime) {
				rt_rq->rt_throttled = 0;
				enqueue = 1;

				/*
				 * When we're idle and a woken (rt) task is
				 * throttled check_preempt_curr() will set
				 * skip_update and the time between the wakeup
				 * and this unthrottle will get accounted as
				 * 'runtime'.
				 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   우리가 유휴 상태이고 깨어난(rt) 작업이 제한되면 
 *   check_preempt_curr()는 skip_update를 설정하고 깨우기와 이 제한 해제 
 *   사이의 시간은 '런타임'으로 간주됩니다.
 *
 * - curr가 idle인데 rt task가 있는 상황.
 *   update_rq_clock을 idle task도 skip없이 동작시켜 schedule 시키겠다는 
 *   의미이다.
 */
				if (rt_rq->rt_nr_running && rq->curr == rq->idle)
					rq_clock_cancel_skipupdate(rq);
			}

/*
 * IAMROOT, 2023.02.18:
 * - rt 실행시간이 있거나 rt task가 있으면 idle을 안시킨다.
 *   (hrtimer 계속 동작)
 */
			if (rt_rq->rt_time || rt_rq->rt_nr_running)
				idle = 0;
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		} else if (rt_rq->rt_nr_running) {
/*
 * IAMROOT, 2023.02.18:
 * - 처음 시작하는 경우
 * - rt실행시간이 없는데 rt가 있으면 idle을 안시킨다.
 *   throttled이 안되있다면 enqueue
 */
			idle = 0;
			if (!rt_rq_throttled(rt_rq))
				enqueue = 1;
		}
		if (rt_rq->rt_throttled)
			throttled = 1;

/*
 * IAMROOT, 2023.02.18:
 * - throttled이 풀리거나 아에 새로운 rt가 들어온경우 enqueue를 할 것이다.
 */
		if (enqueue)
			sched_rt_rq_enqueue(rt_rq);
		raw_spin_rq_unlock(rq);
	}


/*
 * IAMROOT, 2023.02.18:
 * - bw가 비활성 상태면 timer를 종료시킨다.
 */
	if (!throttled && (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF))
		return 1;

/*
 * IAMROOT, 2023.02.18:
 * - rt task가 남아있는 경우 계속 bw를 수행해야된다.
 */
	return idle;
}

/*
 * IAMROOT, 2023.02.11:
 * - group entity이면 가장 높은 prio, task면 task prio를 return.
 */
static inline int rt_se_prio(struct sched_rt_entity *rt_se)
{
#ifdef CONFIG_RT_GROUP_SCHED
	struct rt_rq *rt_rq = group_rt_rq(rt_se);

/*
 * IAMROOT, 2023.02.11:
 * - 자기꺼중에 가장 높은 prio를 가진것을 return.
 */
	if (rt_rq)
		return rt_rq->highest_prio.curr;
#endif

/*
 * IAMROOT, 2023.02.11:
 * - task prio가져온다.
 */
	return rt_task_of(rt_se)->prio;
}

/*
 * IAMROOT, 2023.02.04:
 * - return: exceeded 된 경우 1. resched을 해도된다는 의미도 포함된다.
 *           그렇지 않으면 0
 * - return 1
 *   1. throttle 중이면서 boost 아님.
 *   2. 설정시간보다 많이 쓴 상태에서 bandwidth가 동작중인상태. 즉
 *      throttle된 상태.
 *
 * - return 0.
 *   1. throttle 중이면서 boost 중.
 *   2. period이상 runtime 사용.
 *   3. rt_rq 비활성화.
 *   4. 설정시간보다 runtime을 덜씀.
 */
static int sched_rt_runtime_exceeded(struct rt_rq *rt_rq)
{
	u64 runtime = sched_rt_runtime(rt_rq);

	/*
	 * IAMROOT, 2023.02.04:
	 * - CONFIG_RT_GROUP_SCHED 설정이고 boost 중이면 0을 반환해서 reschedule
	 *   되지 않고 계속 진행하게 한다. 즉 bandwidth 기능을 이번 periods에서 멈춘다
	 * - CONFIG_RT_GROUP_SCHED 설정이 아닌 경우는 throttled 인 경우 reschedule
	 *   한다.
	 */
	if (rt_rq->rt_throttled)
		return rt_rq_throttled(rt_rq);

/*
 * IAMROOT, 2023.02.10:
 * - runtime을 다썻다.
 */
	if (runtime >= sched_rt_period(rt_rq))
		return 0;

/*
 * IAMROOT, 2023.02.10:
 * - runtime 초과분에 대한 balance을 수행한다.
 */
	balance_runtime(rt_rq);
/*
 * IAMROOT, 2023.02.10:
 * - balance_runtime을 통해서 runtime이 수정됫을 수 있으므로 한번더 호출한다.
 *   비활성화면 return 0.
 */
	runtime = sched_rt_runtime(rt_rq);
	if (runtime == RUNTIME_INF)
		return 0;

/*
 * IAMROOT, 2023.02.10:
 * - 실행시간이 설정시간보다 높다면 throttled인지 확인한다.
 */
	if (rt_rq->rt_time > runtime) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		/*
		 * Don't actually throttle groups that have no runtime assigned
		 * but accrue some time due to boosting.
		 */
		/*
		 * IAMROOT. 2023.02.04:
		 * - google-translate
		 *   런타임이 할당되지 않았지만 부스팅으로 인해 시간이 누적되는 그룹을 실제로
		 *   제한하지 마십시오.
		 *
		 * - bandwidth의 runtime이 잇다면, 즉 배분할 runtime이 남아있다면
		 *   throttled로 설정한다.
		 *   그게 아니면 rt_time을 0으로 설정하여 아에 배분을 못받게 해버린다.
		 */
		if (likely(rt_b->rt_runtime)) {
			rt_rq->rt_throttled = 1;
			printk_deferred_once("sched: RT throttling activated\n");
		} else {
			/*
			 * In case we did anyway, make it go away,
			 * replenishment is a joke, since it will replenish us
			 * with exactly 0 ns.
			 */
			/*
			 * IAMROOT. 2023.02.04:
			 * - google-translate
			 *   어쨌든 우리가 그것을 사라지게 만들면
			 *   보충은 정확히 0ns로 우리를 보충할 것이기 때문에 농담입니다.
			 */
			rt_rq->rt_time = 0;
		}
/*
 * IAMROOT, 2023.02.10:
 * - throttled 됬다면 dequeue시키고 return 1.
 */
		if (rt_rq_throttled(rt_rq)) {
			sched_rt_rq_dequeue(rt_rq);
			return 1;
		}
	}

	return 0;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
/*
 * IAMROOT, 2023.02.04:
 * - 1. curr->se.sum_exec_runtime 누적
 *   2. curr task 의 cgroup cpu 실행시간 누적
 *   3. 실행시간이 rt_runtime을 초과 했다면 resched_curr 호출
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_rt_entity *rt_se = &curr->rt;
	u64 delta_exec;
	u64 now;

	if (curr->sched_class != &rt_sched_class)
		return;

	now = rq_clock_task(rq);
	delta_exec = now - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	/*
	 * IAMROOT, 2023.02.04:
	 * - CONFIG_SCHEDSTATS 설정시 rt task max 실행시간 기록. 추적용
	 */
	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = now;
	cgroup_account_cputime(curr, delta_exec);

	if (!rt_bandwidth_enabled())
		return;

	for_each_sched_rt_entity(rt_se) {
/*
 * IAMROOT, 2023.02.10:
 * - rt_se -> task -> pcpu rq -> rt_rq의 접근으로 rt_rq를 가져온다.
 */
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
/*
 * IAMROOT, 2023.02.10:
 * - RUNTIME_INF not equal이라는 의미가 활성화 상태라는것.
 *   활성화 상태인 rt_rq에 대하여 rt_time을 적산한다.
 * - rt_rq가 runtime이 초과됬는지 확인한다. 초과됬으면 reschedule을 요청한다.
 */
		if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_time += delta_exec;
			if (sched_rt_runtime_exceeded(rt_rq))
				resched_curr(rq);
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		}
	}
}

/*
 * IAMROOT, 2023.02.04:
 * - 1. rq->nr_running -= rt_rq->rt_nr_running
 *   2. rt_queued = 0 으로 해서 동작 안함 표시
 * - rq에서 rt_rq에 대한 running개수를 빼버리고 동작안함 표시.
 */
static void
dequeue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (!rt_rq->rt_queued)
		return;

	BUG_ON(!rq->nr_running);

	sub_nr_running(rq, rt_rq->rt_nr_running);
	rt_rq->rt_queued = 0;

}

/*
 * IAMROOT, 2023.02.18:
 * - @rt_rq의 count를 rq에 넣고, cpufreq uill을 수행한다.
 */
static void
enqueue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (rt_rq->rt_queued)
		return;

	if (rt_rq_throttled(rt_rq))
		return;

	if (rt_rq->rt_nr_running) {
		add_nr_running(rq, rt_rq->rt_nr_running);
		rt_rq->rt_queued = 1;
	}

	/* Kick cpufreq (see the comment in kernel/sched/sched.h). */
	cpufreq_update_util(rq, 0);
}

#if defined CONFIG_SMP

/*
 * IAMROOT, 2023.02.18:
 * - top rq이고, prio가 더 높은 경우에만 cpupri를 갱신한다.
 */
static void
inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * Change rq's cpupri only if rt_rq is the top queue.
	 */
	if (&rq->rt != rt_rq)
		return;
#endif
	if (rq->online && prio < prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, prio);
}

/*
 * IAMROOT, 2023.02.18:
 * - top rq이고, highest_prio가 변경된경우 cpupri를 갱신한다.
 *   (prev_prio가 이전에 highest였을 경우 지금의 highest로 바꿔야
 *   될수있으므로)
 */
static void
dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * Change rq's cpupri only if rt_rq is the top queue.
	 */
	if (&rq->rt != rt_rq)
		return;
#endif
	if (rq->online && rt_rq->highest_prio.curr != prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, rt_rq->highest_prio.curr);
}

#else /* CONFIG_SMP */

static inline
void inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}
static inline
void dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}

#endif /* CONFIG_SMP */

#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED

/*
 * IAMROOT, 2023.02.18:
 * - highest_prio 갱신 및 @rt_rq->rd의 cpupri 갱신
 */
static void
inc_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (prio < prev_prio)
		rt_rq->highest_prio.curr = prio;

	inc_rt_prio_smp(rt_rq, prio, prev_prio);
}

/*
 * IAMROOT, 2023.02.18:
 * - rt가 있으면 highest_prio 비교 및 갱신
 *   rt가 없으면 MAX_RT_PRIO - 1로 초기화.
 * - @rt_rq->rd의 cpupri 갱신
 */
static void
dec_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (rt_rq->rt_nr_running) {

		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some recomputation to do
		 */
		if (prio == prev_prio) {
			struct rt_prio_array *array = &rt_rq->active;

			rt_rq->highest_prio.curr =
				sched_find_first_bit(array->bitmap);
		}

	} else {
		rt_rq->highest_prio.curr = MAX_RT_PRIO-1;
	}

	dec_rt_prio_smp(rt_rq, prio, prev_prio);
}

#else

static inline void inc_rt_prio(struct rt_rq *rt_rq, int prio) {}
static inline void dec_rt_prio(struct rt_rq *rt_rq, int prio) {}

#endif /* CONFIG_SMP || CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED

/*
 * IAMROOT, 2023.02.18:
 * - boost된 @rt_se인 경우라면 rt_se가 들어가는 상황이므로 boost entity를
 *   증가시킨다.
 * - tg(cgroup)일 경우 rt bw를 예약한다.
 */
static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted++;

/*
 * IAMROOT, 2023.02.18:
 * - group에 대한 bw를 예약한다.
 */
	if (rt_rq->tg)
		start_rt_bandwidth(&rt_rq->tg->rt_bandwidth);
}

/*
 * IAMROOT, 2023.02.18:
 * - boost된 @rt_se인 경우라면 rt_se가 빠지는 상황이므로 boost entity를
 *   감소시킨다.
 */
static void
dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted--;

	WARN_ON(!rt_rq->rt_nr_running && rt_rq->rt_nr_boosted);
}

#else /* CONFIG_RT_GROUP_SCHED */

static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	start_rt_bandwidth(&def_rt_bandwidth);
}

static inline
void dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq) {}

#endif /* CONFIG_RT_GROUP_SCHED */

/*
 * IAMROOT, 2023.02.18:
 * - group이면 return rt_nr_running, task면 return 1.
 */
static inline
unsigned int rt_se_nr_running(struct sched_rt_entity *rt_se)
{
	struct rt_rq *group_rq = group_rt_rq(rt_se);

	if (group_rq)
		return group_rq->rt_nr_running;
	else
		return 1;
}

/*
 * IAMROOT, 2023.02.18:
 * - group이면 return rt_nr_running, task면 return 1.
 */
static inline
unsigned int rt_se_rr_nr_running(struct sched_rt_entity *rt_se)
{
	struct rt_rq *group_rq = group_rt_rq(rt_se);
	struct task_struct *tsk;

	if (group_rq)
		return group_rq->rr_nr_running;

	tsk = rt_task_of(rt_se);

	return (tsk->policy == SCHED_RR) ? 1 : 0;
}

/*
 * IAMROOT, 2023.02.18:
 * 1. 자신이 속한 @rt_rq에서 자신의 정보를 더한다.
 * 2. cpupri 갱신
 * 3. overload 갱신 (overload 설정 될수있다.)
 * 4. boost 갱신(rt bw 예약 될수있다.)
 */
static inline
void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running += rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running += rt_se_rr_nr_running(rt_se);

	inc_rt_prio(rt_rq, prio);
	inc_rt_migration(rt_se, rt_rq);
	inc_rt_group(rt_se, rt_rq);
}

/*
 * IAMROOT, 2023.02.18:
 * 1. 자신이 속한 @rt_rq에서 자신의 정보를 뺀다.
 * 2. cpupri 갱신
 * 3. overload 갱신 (해제 될수있다.)
 * 4. boost 갱신
 */
static inline
void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	WARN_ON(!rt_prio(rt_se_prio(rt_se)));
	WARN_ON(!rt_rq->rt_nr_running);

	rt_rq->rt_nr_running -= rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running -= rt_se_rr_nr_running(rt_se);

	dec_rt_prio(rt_rq, rt_se_prio(rt_se));
	dec_rt_migration(rt_se, rt_rq);
	dec_rt_group(rt_se, rt_rq);
}

/*
 * Change rt_se->run_list location unless SAVE && !MOVE
 *
 * assumes ENQUEUE/DEQUEUE flags match
 */
/*
 * IAMROOT, 2023.02.18:
 * @return true 
 *             - DEQUEUE_SAVE와 DEQUEUE_MOVE가 동시에 존재
 *             - DEQUEUE_SAVE가 없음.
 *         false 
 *             - DEQUEUE_MOVE를 제외한 DEQUEUE_SAVE가 있거나
 *               DEQUEUE_SAVE + 알파가 있는 상태.
 *
 * - 설정으로 인해 dequeue되는 경우 return false.
 */
static inline bool move_entity(unsigned int flags)
{
	if ((flags & (DEQUEUE_SAVE | DEQUEUE_MOVE)) == DEQUEUE_SAVE)
		return false;

	return true;
}

/*
 * IAMROOT, 2023.02.18:
 * - list에서 빠지고, priority array에서 clear한다.
 */
static void __delist_rt_entity(struct sched_rt_entity *rt_se, struct rt_prio_array *array)
{
	list_del_init(&rt_se->run_list);

	if (list_empty(array->queue + rt_se_prio(rt_se)))
		__clear_bit(rt_se_prio(rt_se), array->bitmap);

	rt_se->on_list = 0;
}

/*
 * IAMROOT, 2023.02.18:
 * - enqueue를 수행한다.
 *   (move, on_list set, on_rq set, count, cpupri, overload, boost)
 */
static void __enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct rt_rq *group_rq = group_rt_rq(rt_se);
	struct list_head *queue = array->queue + rt_se_prio(rt_se);

	/*
	 * Don't enqueue the group if its throttled, or when empty.
	 * The latter is a consequence of the former when a child group
	 * get throttled and the current group doesn't have any other
	 * active members.
	 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   제한된 경우 또는 비어 있는 경우 그룹을 대기열에 넣지 마십시오. 
 *   후자는 하위 그룹이 제한되고 현재 그룹에 다른 활성 구성원이 없는 
 *   경우 전자의 결과입니다.
 *
 * - group인경우, throttle상태거나 rt task가 없는상태면 @rt_se를 
 *   enqueue를 안한다. 바쁘거나 동작시킬게 없기때문이다.
 */
	if (group_rq && (rt_rq_throttled(group_rq) || !group_rq->rt_nr_running)) {
		if (rt_se->on_list)
			__delist_rt_entity(rt_se, array);
		return;
	}

/*
 * IAMROOT, 2023.02.18:
 * - move로 인한 enqueue 상확인경우, head에 넣어야되면 head로, tail에
 *   넣어야되면 tail로 넣는다.
 */
	if (move_entity(flags)) {
		WARN_ON_ONCE(rt_se->on_list);
		if (flags & ENQUEUE_HEAD)
			list_add(&rt_se->run_list, queue);
		else
			list_add_tail(&rt_se->run_list, queue);

		__set_bit(rt_se_prio(rt_se), array->bitmap);
		rt_se->on_list = 1;
	}
	rt_se->on_rq = 1;

	inc_rt_tasks(rt_se, rt_rq);
}


/*
 * IAMROOT, 2023.02.18:
 * - move 상황이라면 list에서 제거한다.
 *   @rt_rq에서 @rt_se을 dequeue한다.(count, cpupri, overload, boost등)
 */
static void __dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;

/*
 * IAMROOT, 2023.02.18:
 * - move로 인한 dequeue라면 자료구조에서 뺀다.
 */
	if (move_entity(flags)) {
		WARN_ON_ONCE(!rt_se->on_list);
		__delist_rt_entity(rt_se, array);
	}
	rt_se->on_rq = 0;

/*
 * IAMROOT, 2023.02.18:
 * - @rt_se가 속한 @rt_rq에서 entity가 빠지는 상황에 따른 정보를 처리한다.
 */
	dec_rt_tasks(rt_se, rt_rq);
}

/*
 * Because the prio of an upper entry depends on the lower
 * entries, we must remove entries top - down.
 */
/*
 * IAMROOT, 2023.02.18:
 * - stack을 이용해 top-down식으로 dequeue한다.
 *   priority에 따라서 상위에 영향을 주기 때문에 아에 관련 parent들에서 
 *   빼버리는것이다. 위에서부터 아래로 순차적으로 dequeue한다.
 * - @rt_se가 속한 최상위 rq에서 @rt_se관련 count를 뺀다.
 */
static void dequeue_rt_stack(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct sched_rt_entity *back = NULL;

/*
 * IAMROOT, 2023.02.18:
 * - 잠깐 보관을 위해 parent에 보관을 한다.(stack) 임시로 dequeue를 해야되는
 *   상황에서 원래의 연결경로를 기억해야되기 때문이다.
 */
	for_each_sched_rt_entity(rt_se) {
		rt_se->back = back;
		back = rt_se;
	}

	dequeue_top_rt_rq(rt_rq_of_se(back));

/*
 * IAMROOT, 2023.02.18:
 * - 위에서 bakup했던것을 최상위에서 내려오면서 dequeue한다.
 */
	for (rt_se = back; rt_se; rt_se = rt_se->back) {
		if (on_rt_rq(rt_se))
			__dequeue_rt_entity(rt_se, flags);
	}
}

/*
 * IAMROOT, 2023.02.18:
 * - stack을 이용해 먼저 @rt_se를 dequeue한 후, 다시 enqueue한다.
 *   (priority가 상위에 영향을 줄수 있기때문이다.)
 */
static void enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rq *rq = rq_of_rt_se(rt_se);

/*
 * IAMROOT, 2023.02.18:
 * - enqueue전에 dequeue를 한다.
 *   enqueue되는 task의 priority에 따라 parent의 rt_se의 대표 priority의
 *   가 변경 될 수 때문이다.
 * - @rt_se가 속한 최상위 rq에서 @rt_se관련 count를 뺀다.
 */
	dequeue_rt_stack(rt_se, flags);
/*
 * IAMROOT, 2023.02.18:
 * - down-top 으로 다시 enqueue한다.
 */
	for_each_sched_rt_entity(rt_se)
		__enqueue_rt_entity(rt_se, flags);

/*
 * IAMROOT, 2023.02.18:
 * - rq에서 @rt_se관련 count를 뺀다. dequeue할때 count를 뺏엇으니 다시
 *   enqueue를 끝내고 다시 update해주는 개념이다.
 */
	enqueue_top_rt_rq(&rq->rt);
}

/*
 * IAMROOT, 2023.02.18:
 * - @rt_se를 dequeue한다. 일단 stack을 사용해 @rt_se관련을 전부 dequeue
 *   하고 enqueue를 할때는 group entity만 수행한다.
 */
static void dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rq *rq = rq_of_rt_se(rt_se);

/*
 * IAMROOT, 2023.02.18:
 * - stack을 사용해 dequeue.
 */
	dequeue_rt_stack(rt_se, flags);

/*
 * IAMROOT, 2023.02.18:
 * - rt task가 있는 group은 다시 enqueue한다.
 */
	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = group_rt_rq(rt_se);

		if (rt_rq && rt_rq->rt_nr_running)
			__enqueue_rt_entity(rt_se, flags);
	}

/*
 * IAMROOT, 2023.02.18:
 * - dequeue_rt_stack에서 뺏던 top rq를 다시 여기서 갱신.
 *   dequeue된 @rt_se에 대한 정보는 빠졋을것이다.
 */
	enqueue_top_rt_rq(&rq->rt);
}

/*
 * Adding/removing a task to/from a priority array:
 */
/*
 * IAMROOT, 2023.02.18:
 * - task를 enqueue 하고, task가 current가 아닌경우 pushable task로 
 *   enqueue한다.
 */
static void
enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	enqueue_rt_entity(rt_se, flags);

/*
 * IAMROOT, 2023.02.18:
 * - @p가 curr로 동작중이 아니고, 동작할수있는 cpu가 여러개라면 pushable
 *   task로 넣어진다.
 */
	if (!task_current(rq, p) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

/*
 * IAMROOT, 2023.02.18:
 * - 1. update_curr_rt(실행시간 관련 처리 및 resched_curr 수행 여부 확인)
 *   2. @p의 dequeue 수행
 *   3. @p를 pushable task에서 dequeue.
 */
static void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);
	dequeue_rt_entity(rt_se, flags);

	dequeue_pushable_task(rq, p);
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
/*
 * IAMROOT, 2023.02.11:
 * - @rt_se priority list에 @head에 따라 requeue한다.
 * - group entity의 경우엔 group entity에서 가장 높은 우선순위로 옮긴다.
 */
static void
requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	if (on_rt_rq(rt_se)) {
		struct rt_prio_array *array = &rt_rq->active;
		struct list_head *queue = array->queue + rt_se_prio(rt_se);

		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

/*
 * IAMROOT, 2023.02.10:
 * - @p의 rt_se부터 시작하여 parent까지 올라가면서 @head따라 head / tail로
 *   requeue를 한다.
 */
static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq;

	for_each_sched_rt_entity(rt_se) {
		rt_rq = rt_rq_of_se(rt_se);
		requeue_rt_entity(rt_rq, rt_se, head);
	}
}

/*
 * IAMROOT, 2023.02.11:
 * - rt의 경우 가장높은 우선순위 태스크가 한개밖에 없으면 yield가
 *   안될 것이다.
 */
static void yield_task_rt(struct rq *rq)
{
	requeue_task_rt(rq, rq->curr, 0);
}

#ifdef CONFIG_SMP
static int find_lowest_rq(struct task_struct *task);

/*
 * IAMROOT, 2023.03.04:
 * - @p를 어느 cpu에서 깨울건지 정한다.
 *   @p를 @cpu에서 동작시킬수없는 상황이면 다른 적합한 cpu를 찾아 반환한다.
 */
static int
select_task_rq_rt(struct task_struct *p, int cpu, int flags)
{
	struct task_struct *curr;
	struct rq *rq;
	bool test;

	/* For anything but wake ups, just return the task_cpu */
/*
 * IAMROOT, 2023.03.04:
 * - task가 있지만 sleep에서 깨어나는 경우거나 fork한 경우.
 *   wakeup상황 or fork에서만 현재 함수를 호출한다.
 */
	if (!(flags & (WF_TTWU | WF_FORK)))
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */

	/*
	 * If the current task on @p's runqueue is an RT task, then
	 * try to see if we can wake this RT task up on another
	 * runqueue. Otherwise simply start this RT task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affined RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 *
	 * We take into account the capacity of the CPU to ensure it fits the
	 * requirement of the task - which is only important on heterogeneous
	 * systems like big.LITTLE.
	 */
/*
 * IAMROOT, 2023.03.04:
 * - papago
 *   @p의 실행 대기열에 있는 현재 작업이 RT 작업이면 다른 실행 대기열에서 이 
 *   RT 작업을 깨울 수 있는지 확인하십시오. 그렇지 않으면 단순히 현재 실행 
 *   대기열에서 이 RT 작업을 시작하십시오.
 *
 *   우리는 runqueue의 과부하를 피하고 싶습니다. 깨운 작업의 우선 순위가 더 
 *   높으면 이 CPU에 남게 되고 우선 순위가 낮은 작업은 다른 CPU로 옮겨야 
 *   합니다.
 *   이로 인해 하위 prio 작업이 캐시를 잃게 되더라도 CPU를 포기했기 때문에 더 
 *   높은 작업을 반송하고 싶지는 않습니다. 아마도 잠금을 위해? 
 *
 *   동등한 우선 순위 작업의 경우 스케줄러가 정렬하도록 합니다. 
 *
 *   그렇지 않으면 연결된 RQ를 타고 가도록 놔두면 사후 스케줄 라우터가 선점된 
 *   작업을 밀어낼 것입니다. 이 테스트는 낙관적입니다. 잘못되면 로드 밸런서가 
 *   이를 분류해야 합니다.
 *
 *   작업 요구 사항에 맞는지 확인하기 위해 CPU 용량을 고려합니다. 이는 
 *   big.LITTLE과 같은 이기종 시스템에서만 중요합니다.
 */
	test = curr &&
	       unlikely(rt_task(curr)) &&
	       (curr->nr_cpus_allowed < 2 || curr->prio <= p->prio);

	if (test || !rt_task_fits_capacity(p, cpu)) {
		int target = find_lowest_rq(p);

		/*
		 * Bail out if we were forcing a migration to find a better
		 * fitting CPU but our search failed.
		 */
		if (!test && target != -1 && !rt_task_fits_capacity(p, target))
			goto out_unlock;

		/*
		 * Don't bother moving it if the destination CPU is
		 * not running a lower priority task.
		 */
		if (target != -1 &&
		    p->prio < cpu_rq(target)->rt.highest_prio.curr)
			cpu = target;
	}

out_unlock:
	rcu_read_unlock();

out:
	return cpu;
}

static void check_preempt_equal_prio(struct rq *rq, struct task_struct *p)
{
	/*
	 * Current can't be migrated, useless to reschedule,
	 * let's hope p can move out.
	 */
	if (rq->curr->nr_cpus_allowed == 1 ||
	    !cpupri_find(&rq->rd->cpupri, rq->curr, NULL))
		return;

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	if (p->nr_cpus_allowed != 1 &&
	    cpupri_find(&rq->rd->cpupri, p, NULL))
		return;

	/*
	 * There appear to be other CPUs that can accept
	 * the current task but none can run 'p', so lets reschedule
	 * to try and push the current task away:
	 */
	requeue_task_rt(rq, p, 1);
	resched_curr(rq);
}

/*
 * IAMROOT, 2023.02.18:
 * - @p에서 rt가 동작을 안하고 있고, @p가 @rq의 우선순위가 높은상황,
 *   즉 rt task
 * --- push / pull을 하는 상황 정리.
 *  1. cpu에서 idle인 상황에서 pull
 *  2. core에서 balance을 call한 경우 push / pull
 *  3. sleep에서 깨어낫을때(woken) sleep에서 깨어난 task를 그냥 동작할지
 *     push를 할지 선택.
 *  4. balance_callback의 선택에 따른 처리.
 *     __schedule() 함수에서 post처리에서 상황에 따라 push / pull 선택
 * ---
 */
static int balance_rt(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
{

/*
 * IAMROOT, 2023.02.11:
 * - @p가 아직 rq에 없고, @rq(현재 task)보다 @p가(요청한 task) 
 *   우선순위가 높다면 pull작업을 수행한다.
 */
	if (!on_rt_rq(&p->rt) && need_pull_rt_task(rq, p)) {
		/*
		 * This is OK, because current is on_cpu, which avoids it being
		 * picked for load-balance and preemption/IRQs are still
		 * disabled avoiding further scheduler activity on it and we've
		 * not yet started the picking loop.
		 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   current는 on_cpu이므로 부하 분산 및 선점/IRQ가 추가 스케줄러 활동을 
 *   방지하기 위해 여전히 비활성화되어 있고 아직 선택 루프를 시작하지 
 *   않았기 때문에 괜찮습니다.
 */
		rq_unpin_lock(rq, rf);
		pull_rt_task(rq);
		rq_repin_lock(rq, rf);
	}

	return sched_stop_runnable(rq) || sched_dl_runnable(rq) || sched_rt_runnable(rq);
}
#endif /* CONFIG_SMP */

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->prio < rq->curr->prio) {
		resched_curr(rq);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * If:
	 *
	 * - the newly woken task is of equal priority to the current task
	 * - the newly woken task is non-migratable while current is migratable
	 * - current will be preempted on the next reschedule
	 *
	 * we should check to see if current can readily move to a different
	 * cpu.  If so, we will reschedule to allow the push logic to try
	 * to move current somewhere else, making room for our non-migratable
	 * task.
	 */
	if (p->prio == rq->curr->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
#endif
}

/*
 * IAMROOT, 2023.02.11:
 * - exec_start를 갱신하고 선택된 @p를 pushable에서 dequeue한다.
 * - pick_next_task에서 불러지는 경우 @first = true
 *   set_next_task에서는 @first = false
 */
static inline void set_next_task_rt(struct rq *rq, struct task_struct *p, bool first)
{
/*
 * IAMROOT, 2023.02.11:
 * - 현재 시각으로 (@rq의 clock_task)로 @p의 exec_start를 갱신한다.
 */
	p->se.exec_start = rq_clock_task(rq);

	/* The running task is never eligible for pushing */
/*
 * IAMROOT, 2023.02.11:
 * - @p가 선택된 상황이니, @p를 pushable task에서 dequeue한다.
 */
	dequeue_pushable_task(rq, p);

	if (!first)
		return;

	/*
	 * If prev task was rt, put_prev_task() has already updated the
	 * utilization. We only care of the case where we start to schedule a
	 * rt task
	 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   이전 작업이 rt인 경우 put_prev_task()는 이미 사용률을 
 *   업데이트했습니다. 우리는 rt 작업을 예약하기 시작하는 경우에만 
 *   관심이 있습니다. 
 * - 이전 task가 rt였다면 put_prev_task_rt()에서 load avg를 갱신했었다.
 *   이전 task가 rt가 아닌 경우에만 수행한다.
 */
	if (rq->curr->sched_class != &rt_sched_class)
		update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	rt_queue_push_tasks(rq);
}

/*
 * IAMROOT, 2023.02.11:
 * - 가장 높은 우선순위주에서도 제일 앞에 있는 rt_se를 가져온다.
 */
static struct sched_rt_entity *pick_next_rt_entity(struct rq *rq,
						   struct rt_rq *rt_rq)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct sched_rt_entity *next = NULL;
	struct list_head *queue;
	int idx;


/*
 * IAMROOT, 2023.02.11:
 * - 제일 높은 priority를 가져온다.
 */
	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_RT_PRIO);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_rt_entity, run_list);

	return next;
}

/*
 * IAMROOT, 2023.02.11:
 * - 제일 높은 priority를 가진 task를 가져온다.
 */
static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
	struct sched_rt_entity *rt_se;
	struct rt_rq *rt_rq  = &rq->rt;

	do {
		rt_se = pick_next_rt_entity(rq, rt_rq);
		BUG_ON(!rt_se);

/*
 * IAMROOT, 2023.02.11:
 * - rt_rq가 NULL이면 @rt_se가 task라는 의미가 되여 while이 종료되며
 *   task가 찾아지는 원리이다.
 */
		rt_rq = group_rt_rq(rt_se);
	} while (rt_rq);

	return rt_task_of(rt_se);
}

/*
 * IAMROOT, 2023.02.11:
 * - 제일 높은 priority를 가진 task를 가져온다.
 */
static struct task_struct *pick_task_rt(struct rq *rq)
{
	struct task_struct *p;

	if (!sched_rt_runnable(rq))
		return NULL;

	p = _pick_next_task_rt(rq);

	return p;
}

/*
 * IAMROOT, 2023.02.11:
 * - 제일높은 task를 선택한다. 선택됬으면 exec_start를 갱신하고
 *   pushable 상황일때는 push task를 지정(queue_balance_callback)한다.
 */
static struct task_struct *pick_next_task_rt(struct rq *rq)
{
	struct task_struct *p = pick_task_rt(rq);

	if (p)
		set_next_task_rt(rq, p, true);

	return p;
}

/*
 * IAMROOT, 2023.02.11:
 * - 기존 태스크(@p)에 대해 다음과 같은걸 처리한다.
 *   1. sum_exec_runtime 갱신 및 runtime 초과 처리 
 *   2. load avg 갱신
 *   3. 동작할 수 있는 cpu가 2개 이상인경우 pushable task로 enqueue
 */
static void put_prev_task_rt(struct rq *rq, struct task_struct *p)
{
	update_curr_rt(rq);

	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	/*
	 * The previous task needs to be made eligible for pushing
	 * if it is still active
	 */
/*
 * IAMROOT, 2023.02.11:
 * - rq에 on상태이고 허락된 cpu가 2개 이상인 경우 pushable_tasks로
 *   enqueue한다.
 * - 이 상황이되면 rt task가 이미 2개 이상인 상황에서, 다른 rt task에
 *   우선순위나 RR 정책에 밀려난 상황이된다.
 */
	if (on_rt_rq(&p->rt) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define RT_MAX_TRIES 3

/*
 * IAMROOT, 2023.02.18:
 * - @p가 동작중이 아니고, @cpu에서 동작이 가능하다면 return 1.
 */
static int pick_rt_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    cpumask_test_cpu(cpu, &p->cpus_mask))
		return 1;

	return 0;
}

/*
 * Return the highest pushable rq's task, which is suitable to be executed
 * on the CPU, NULL otherwise
 */
/*
 * IAMROOT, 2023.02.18:
 * - @rq에서 @cpu에서 동작할 수 있는 가장 highest pushable task를 가져온다.
 */
static struct task_struct *pick_highest_pushable_task(struct rq *rq, int cpu)
{
	struct plist_head *head = &rq->rt.pushable_tasks;
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	plist_for_each_entry(p, head, pushable_tasks) {
		if (pick_rt_task(rq, p, cpu))
			return p;
	}

	return NULL;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask);

/*
 * IAMROOT, 2023.02.11:
 * - 가장 낮은 우선순위를 가진 cpu rq를 선택한다. 
 *   idle -> cfs만 있는 rq -> rt task 순으로 낮을 것이다.
 *
 * - task의 우선순위범위내(최저 우선순위 부터 tsak 우선순위까지)에서
 *   cpupri와 겹치며 uclamp조건을 만족하는 cpu를 lowest_mask에 기록한다.
 *   최종적으로 선택하는 조건은 다음 우선순위로 따른다.
 *   1. task cpu (l1 cache공유) :
 *      task가 동작했던 cpu가 lowest_mask에 있는경우
 *   2. this cpu (l2 cache 이상 공유) :
 *      this cpu가 lowest_mask에 있고 해당 domain span에 포함되있는경우
 *   3. best cpu(l2 cache 이상 공유) :
 *      lowest_mask에 있고 해당 domain span에 포함되있는경우.
 *   4. domain에서 못찾은 경우 :
 *      lowest_mask에 this cpu가 있는 경우
 *   5. this cpu도 lowest_mask에 없는 경우 :
 *      lowest_mask에서 아무거나 한개.
 *
 *   5번까지 조건에도 없으면 return -1.
 */
static int find_lowest_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = this_cpu_cpumask_var_ptr(local_cpu_mask);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);
	int ret;

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return -1;

	if (task->nr_cpus_allowed == 1)
		return -1; /* No other targets possible */

	/*
	 * If we're on asym system ensure we consider the different capacities
	 * of the CPUs when searching for the lowest_mask.
	 */
	if (static_branch_unlikely(&sched_asym_cpucapacity)) {
/*
 * IAMROOT, 2023.02.11:
 * - HMP의 경우이다.
 * - @task의 우선순위범위에서 cpupri 범위와 겹치는 cpu를 찾아서
 *   uclamp 범위에 적합한지 검사(rt_task_fits_capacity)하여 
 *   lowest_mask에 기록한다.
 */
		ret = cpupri_find_fitness(&task_rq(task)->rd->cpupri,
					  task, lowest_mask,
					  rt_task_fits_capacity);
	} else {

/*
 * IAMROOT, 2023.02.11:
 * - @task의 우선순위범위에서 cpupri 범위와 겹치는 cpu를 찾아서 겹치는걸
 *   lowest_mask에 기록한다.
 */
		ret = cpupri_find(&task_rq(task)->rd->cpupri,
				  task, lowest_mask);
	}

/*
 * IAMROOT, 2023.02.11:
 * - 위에서 못찾았다. return -1
 */
	if (!ret)
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of CPUs representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last CPU that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   이 시점에서 우리는 시스템에서 가장 낮은 우선 순위 작업을 나타내는 
 *   CPU 마스크를 만들었습니다. 이제 우리는 선호도와 토폴로지를 기반으로 
 *   최고의 것을 선택하려고 합니다. 
 *
 *   작업이 실행된 마지막 CPU는 해당 위치에서 캐시 핫일 가능성이 높기 
 *   때문에 우선 순위를 지정합니다.
 *
 *  - @task의 cpu가 lowest_mask에 존재한다면 cache hot이라고 
 *  생각하여 즉시 task cpu로 return한다.
 *  - 마지막에 동작한 task가 task의 cpu에 기록되있으므로 가능하면 
 *    @task의 cpu와 같은게 좋다.
 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which CPU is logically closest to our hot cache data.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - current cpu가 lowest_mask에 속해있지 않은지를 검사한다.
 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	rcu_read_lock();

/*
 * IAMROOT, 2023.02.11:
 * - 밑에서부터 위로 올라간다.
 */
	for_each_domain(cpu, sd) {
/*
 * IAMROOT, 2023.02.11:
 * - SD_WAKE_AFFINE이 있는 domain을 찾는다.
 */
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
/*
 * IAMROOT, 2023.02.11:
 * - current cpu를 선택할수 있는 상황에서 sd에 current cpu가 포함되있다면
 *   바로 선택한다.
 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

/*
 * IAMROOT, 2023.02.11:
 * - 위의 상황이 아니라면 best_cpu를 찾는다.(그냥 적당히 숝서대로찾는다)
 */
			best_cpu = cpumask_any_and_distribute(lowest_mask,
							      sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
/*
 * IAMROOT, 2023.02.11:
 * - 여기까지 왔으면 sd에 소속되있지 않다. 상위 domain으로 간다.
 */
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - domain에서 못찾았으면, lowest_mask에 current cpu가 있엇을 경우 그냥
 *   current cpu로 선택한다.
 */
	if (this_cpu != -1)
		return this_cpu;

/*
 * IAMROOT, 2023.02.11:
 * - lowest_mask에서 아무거나 고른다.
 */
	cpu = cpumask_any_distribute(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

/*
 * IAMROOT, 2023.02.11:
 * - lowest도 못찾앗다..
 */
	return -1;
}

/* Will lock the rq it finds */
/*
 * IAMROOT, 2023.02.11:
 * - @task보다 우선순위가 낮은 lowest_rq를 찾는다.
 *   찾아지면 double lock 건채로 lowest_rq return.
 */
static struct rq *find_lock_lowest_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *lowest_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < RT_MAX_TRIES; tries++) {
		cpu = find_lowest_rq(task);

/*
 * IAMROOT, 2023.02.11:
 * - 찾는데 실패했거나 rq cpu와 같으면 break.
 */
		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		lowest_rq = cpu_rq(cpu);

		if (lowest_rq->rt.highest_prio.curr <= task->prio) {
			/*
			 * Target rq has tasks of equal or higher priority,
			 * retrying does not release any lock and is unlikely
			 * to yield a different result.
			 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   대상 rq에는 우선 순위가 같거나 더 높은 작업이 있으며 재시도해도 
 *   잠금이 해제되지 않으며 다른 결과가 나올 가능성이 없습니다.
. 
 * - lowest를 찾았는데 lowest가 @task보다 우선순위가 높은 상태.
 *   그냥 빠져나간다.
 */
			lowest_rq = NULL;
			break;
		}

		/* if the prio of this runqueue changed, try again */
		if (double_lock_balance(rq, lowest_rq)) {
			/*
			 * We had to unlock the run queue. In
			 * the mean time, task could have
			 * migrated already or had its affinity changed.
			 * Also make sure that it wasn't scheduled on its rq.
			 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   실행 대기열을 잠금 해제해야 했습니다. 그 동안 작업이 이미 
 *   마이그레이션되었거나 선호도가 변경되었을 수 있습니다.
 *   또한 rq에서 예약되지 않았는지 확인하십시오.
 *
 * - double lock을 얻는 중에 경합이 발생됬다. 변동이 발생됬을수도
 *   있는 상황이라 확인해야된다.
 */
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(lowest_rq->cpu, &task->cpus_mask) ||
				     task_running(rq, task) ||
				     !rt_task(task) ||
				     !task_on_rq_queued(task))) {
/*
 * IAMROOT, 2023.02.11:
 * - 바뀐 상황. break.
 */
				double_unlock_balance(rq, lowest_rq);
				lowest_rq = NULL;
				break;
			}
		}

/*
 * IAMROOT, 2023.02.11:
 * - 경합이 발생안했거나 경합이 발햇어도 조건에 맞는 상황.
 *   lock을 얻은 상태에서 다시한번 우선순위 비교를 해본다.
 *   task보다 낮은 우선순위가 찾아졌으면 break.
 */
		/* If this rq is still suitable use it. */
		if (lowest_rq->rt.highest_prio.curr > task->prio)
			break;

		/* try again */
		double_unlock_balance(rq, lowest_rq);
		lowest_rq = NULL;
	}

	return lowest_rq;
}

/*
 * IAMROOT, 2023.02.11:
 * - pushable task를 한개 빼온다.
 */
static struct task_struct *pick_next_pushable_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	p = plist_first_entry(&rq->rt.pushable_tasks,
			      struct task_struct, pushable_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(p->nr_cpus_allowed <= 1);

	BUG_ON(!task_on_rq_queued(p));
	BUG_ON(!rt_task(p));

	return p;
}

/*
 * If the current CPU has more than one RT task, see if the non
 * running task can migrate over to a CPU that is running a task
 * of lesser priority.
 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   현재 CPU에 RT 작업이 두 개 이상 있는 경우 실행되지 않는 작업이 
 *   우선 순위가 낮은 작업을 실행 중인 CPU로 마이그레이션할 수 있는지 
 *   확인합니다.
 * @pull push_rt_tasks에서는 fasle
 * @return 1 : migrate를 한경우.
 *         0 : migrate 못한 경우.
 *
 * - @rq에서 pushable task(next_task)를 고르고, push할 rq를 선택해서 
 *   migrate 및 reschedule한다.
 * - 만약 next_task가 migrate disable일 경우 현재 cpu의 curr를 migrate
 *   시도한다.
 * - migrate할 적합한 cpu가 없으면 안한다.
 */
static int push_rt_task(struct rq *rq, bool pull)
{
	struct task_struct *next_task;
	struct rq *lowest_rq;
	int ret = 0;


/*
 * IAMROOT, 2023.02.11:
 * - overload가 안된경우(2개 미만의 task가 있는 경우) push
 *   할 필요가 없다.
 */
	if (!rq->rt.overloaded)
		return 0;

/*
 * IAMROOT, 2023.02.11:
 * - push할 task를 한개 고른다.
 */
	next_task = pick_next_pushable_task(rq);
	if (!next_task)
		return 0;

retry:

/*
 * IAMROOT, 2023.02.11:
 * - migrate가 안되는 task들은 현재 cpu에서 동작하게 해야된다.
 *   즉 next_task는 현재 cpu에서 동작을 해야되는 상황이다.
 *   이 상황에서 next_task대신에 curr를 migration할수있는지 검사 및 수행
 *   한다.
 *   curr를 migration을 할수 있으면 stop scheduler를 사용한다.
 *
 *   stopper - curr         - next_task
 *            (migrate O)     (migrate X)
 */
	if (is_migration_disabled(next_task)) {
		struct task_struct *push_task = NULL;
		int cpu;

		if (!pull || rq->push_busy)
			return 0;

/*
 * IAMROOT, 2023.02.11:
 * - 아에 못찾았거나 rq의 cpu와 같은경우 return 0.
 */
		cpu = find_lowest_rq(rq->curr);
		if (cpu == -1 || cpu == rq->cpu)
			return 0;

		/*
		 * Given we found a CPU with lower priority than @next_task,
		 * therefore it should be running. However we cannot migrate it
		 * to this other CPU, instead attempt to push the current
		 * running task on this CPU away.
		 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   @next_task보다 우선 순위가 낮은 CPU를 찾았으므로 실행 중이어야 합니다.
 *   그러나 이를 다른 CPU로 마이그레이션할 수는 없으며 대신 이 CPU에서
 *   현재 실행 중인 작업을 밀어내려고 시도합니다.
 *   
 * - @rq->curr가 push가 가능하면 stop을 시킨후 꺼낸다. (push_cpu_stop 실행)
 */
		push_task = get_push_task(rq);
		if (push_task) {
			raw_spin_rq_unlock(rq);
			stop_one_cpu_nowait(rq->cpu, push_cpu_stop,
					    push_task, &rq->push_work);
			raw_spin_rq_lock(rq);
		}

		return 0;
	}

/*
 * IAMROOT, 2023.02.18:
 * - next_task가 migrate가능한 상황.
 */

	if (WARN_ON(next_task == rq->curr))
		return 0;

	/*
	 * It's possible that the next_task slipped in of
	 * higher priority than current. If that's the case
	 * just reschedule current.
	 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   next_task가 현재보다 더 높은 우선 순위로 미끄러졌을 가능성이 
 *   있습니다. 그렇다면 현재 일정을 다시 잡으십시오.
 *
 * - 혹시라도 우선순위가 바뀌어서 next_task가 더 높아졌다면 그냥
 *   reschedule한다.
 */
	if (unlikely(next_task->prio < rq->curr->prio)) {
		resched_curr(rq);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* find_lock_lowest_rq locks the rq if found */
	lowest_rq = find_lock_lowest_rq(next_task, rq);
/*
 * IAMROOT, 2023.02.18:
 * - 한가한 cpu가 없다.
 */
	if (!lowest_rq) {
		struct task_struct *task;
		/*
		 * find_lock_lowest_rq releases rq->lock
		 * so it is possible that next_task has migrated.
		 *
		 * We need to make sure that the task is still on the same
		 * run-queue and is also still the next task eligible for
		 * pushing.
		 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   find_lock_lowest_rq는 rq->lock을 해제하므로 next_task가 
 *   마이그레이션되었을 가능성이 있습니다.
 *
 *   작업이 여전히 동일한 실행 대기열에 있고 여전히 푸시할 수 있는 다음 
 *   작업인지 확인해야 합니다.
 */
		task = pick_next_pushable_task(rq);

/*
 * IAMROOT, 2023.02.18:
 * - 한가한 cpu가 없는 상황에서 pushable task list에 push할게 안바뀐
 *   상황. push를 할수 없는 상황이므로 out한다.
 */
		if (task == next_task) {
			/*
			 * The task hasn't migrated, and is still the next
			 * eligible task, but we failed to find a run-queue
			 * to push it to.  Do not retry in this case, since
			 * other CPUs will pull from us when ready.
			 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   작업이 마이그레이션되지 않았고 여전히 다음으로 적합한 작업이지만 
 *   푸시할 실행 대기열을 찾지 못했습니다. 준비가 되면 다른 CPU가 
 *   우리에게서 끌어오므로 이 경우 재시도하지 마십시오.
 */
			goto out;
		}

		if (!task)
			/* No more tasks, just exit */
			goto out;

		/*
		 * Something has shifted, try again.
		 */
		put_task_struct(next_task);

/*
 * IAMROOT, 2023.02.18:
 * - next_task로는 push할 rq를 못찾은 상황에서 다른 task가 pushable로
 *   선택될수있는 상황. 새로 찾은 task로 next_task를 정하고 다시 시도한다.
 */
		next_task = task;
		goto retry;
	}

/*
 * IAMROOT, 2023.02.18:
 * - next_task를 migrate할 lowest_rq를 찾았다. lowest_rq로 migrate 및
 *   reschedule한다.
 */
	deactivate_task(rq, next_task, 0);
	set_task_cpu(next_task, lowest_rq->cpu);
	activate_task(lowest_rq, next_task, 0);
	resched_curr(lowest_rq);
	ret = 1;

	double_unlock_balance(rq, lowest_rq);
out:
	put_task_struct(next_task);

	return ret;
}

/*
 * IAMROOT, 2023.02.11:
 * - @rq에서 pushable task(next_task)를 고르고, push할 rq를 선택해서 
 *   migrate 및 reschedule한다.
 * - 만약 next_task가 migrate disable일 경우 현재 cpu의 curr를 migrate
 *   시도한다.
 *
 * - migrate할 적합한 cpu를 못찾을때까지 migratge를 수행한다.
 *   false의미 : curr를 pull해야되는 상황에선 pull을 안한다.
 */
static void push_rt_tasks(struct rq *rq)
{
	/* push_rt_task will return true if it moved an RT */
	while (push_rt_task(rq, false))
		;
}

#ifdef HAVE_RT_PUSH_IPI

/*
 * When a high priority task schedules out from a CPU and a lower priority
 * task is scheduled in, a check is made to see if there's any RT tasks
 * on other CPUs that are waiting to run because a higher priority RT task
 * is currently running on its CPU. In this case, the CPU with multiple RT
 * tasks queued on it (overloaded) needs to be notified that a CPU has opened
 * up that may be able to run one of its non-running queued RT tasks.
 *
 * All CPUs with overloaded RT tasks need to be notified as there is currently
 * no way to know which of these CPUs have the highest priority task waiting
 * to run. Instead of trying to take a spinlock on each of these CPUs,
 * which has shown to cause large latency when done on machines with many
 * CPUs, sending an IPI to the CPUs to have them push off the overloaded
 * RT tasks waiting to run.
 *
 * Just sending an IPI to each of the CPUs is also an issue, as on large
 * count CPU machines, this can cause an IPI storm on a CPU, especially
 * if its the only CPU with multiple RT tasks queued, and a large number
 * of CPUs scheduling a lower priority task at the same time.
 *
 * Each root domain has its own irq work function that can iterate over
 * all CPUs with RT overloaded tasks. Since all CPUs with overloaded RT
 * task must be checked if there's one or many CPUs that are lowering
 * their priority, there's a single irq work iterator that will try to
 * push off RT tasks that are waiting to run.
 *
 * When a CPU schedules a lower priority task, it will kick off the
 * irq work iterator that will jump to each CPU with overloaded RT tasks.
 * As it only takes the first CPU that schedules a lower priority task
 * to start the process, the rto_start variable is incremented and if
 * the atomic result is one, then that CPU will try to take the rto_lock.
 * This prevents high contention on the lock as the process handles all
 * CPUs scheduling lower priority tasks.
 *
 * All CPUs that are scheduling a lower priority task will increment the
 * rt_loop_next variable. This will make sure that the irq work iterator
 * checks all RT overloaded CPUs whenever a CPU schedules a new lower
 * priority task, even if the iterator is in the middle of a scan. Incrementing
 * the rt_loop_next will cause the iterator to perform another scan.
 *
 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   우선 순위가 높은 작업이 CPU에서 예약되고 우선 순위가 낮은 작업이 
 *   예약되면 우선 순위가 더 높은 RT 작업이 현재 CPU에서 실행 중이기 
 *   때문에 다른 CPU에서 실행 대기 중인 RT 작업이 있는지 확인합니다.
 *   이 경우 여러 RT 작업이 대기열에 있는(오버로드된) CPU는 실행되지 
 *   않는 대기열에 있는 RT 작업 중 하나를 실행할 수 있는 CPU가 열렸음을 
 *   알려야 합니다. 
 *
 *   과부하된 RT 작업이 있는 모든 CPU는 현재 실행 대기 중인 작업의 우선 
 *   순위가 가장 높은 CPU를 알 수 있는 방법이 없기 때문에 알림을 받아야 
 *   합니다. CPU가 많은 컴퓨터에서 수행할 때 대기 시간이 길어지는 것으로 
 *   나타난 이러한 각 CPU에서 스핀록을 시도하는 대신 CPU에 IPI를 보내 
 *   실행 대기 중인 과부하된 RT 작업을 푸시하도록 합니다.
 *
 *   각 CPU에 IPI를 보내는 것만으로도 문제가 됩니다. 많은 수의 CPU 
 *   머신에서와 같이 이것은 CPU에서 IPI 스톰을 유발할 수 있습니다. 
 *   우선 순위가 낮은 작업을 동시에 수행합니다.
 *
 *   각 루트 도메인에는 RT 과부하 작업으로 모든 CPU를 반복할 수 있는 
 *   자체 irq 작업 기능이 있습니다. 과부하된 RT 작업이 있는 모든 CPU는 
 *   우선 순위를 낮추는 CPU가 하나 이상 있는 경우 확인해야 하므로 실행 
 *   대기 중인 RT 작업을 푸시 오프하려고 시도하는 단일 irq 작업 반복기가 
 *   있습니다.
 *
 *   CPU가 우선 순위가 낮은 작업을 예약하면 과부하된 RT 작업이 있는 
 *   각 CPU로 이동하는 irq 작업 반복자를 시작합니다.
 *   우선 순위가 낮은 작업을 예약하는 첫 번째 CPU만 프로세스를 시작하기 
 *   때문에 rto_start 변수가 증가하고 원자 결과가 1인 경우 해당 CPU는 
 *   rto_lock을 사용하려고 시도합니다.
 *   이렇게 하면 프로세스가 우선 순위가 낮은 작업을 예약하는 모든 
 *   CPU를 처리하므로 잠금에 대한 높은 경합이 방지됩니다.
 *
 *   우선 순위가 낮은 작업을 예약하는 모든 CPU는 rt_loop_next 변수를 
 *   증가시킵니다. 이렇게 하면 irq 작업 반복자가 CPU가 새로운 낮은 우선 
 *   순위 작업을 예약할 때마다 반복자가 스캔 중에 있더라도 모든 RT 
 *   오버로드 CPU를 확인합니다. rt_loop_next를 증가시키면 반복자가 다른 
 *   스캔을 수행하게 됩니다.
 *
 * - rto_mask중에서 그전(rto_cpu)의 next cpu를 고른다.
 */
static int rto_next_cpu(struct root_domain *rd)
{
	int next;
	int cpu;

	/*
	 * When starting the IPI RT pushing, the rto_cpu is set to -1,
	 * rt_next_cpu() will simply return the first CPU found in
	 * the rto_mask.
	 *
	 * If rto_next_cpu() is called with rto_cpu is a valid CPU, it
	 * will return the next CPU found in the rto_mask.
	 *
	 * If there are no more CPUs left in the rto_mask, then a check is made
	 * against rto_loop and rto_loop_next. rto_loop is only updated with
	 * the rto_lock held, but any CPU may increment the rto_loop_next
	 * without any locking.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   IPI RT 푸시를 시작할 때 rto_cpu는 -1로 설정되고 rt_next_cpu()는 
 *   단순히 rto_mask에서 발견된 첫 번째 CPU를 반환합니다.
 *
 *   rto_next_cpu()가 rto_cpu가 유효한 CPU로 호출되면 rto_mask에서 
 *   발견된 다음 CPU를 반환합니다.
 *
 *   rto_mask에 남아 있는 CPU가 더 이상 없으면 rto_loop 및 
 *   rto_loop_next에 대해 확인합니다. rto_loop는 rto_lock이 유지된 
 *   상태에서만 업데이트되지만 모든 CPU는 잠금 없이 rto_loop_next를 
 *   증가시킬 수 있습니다.
 */
	for (;;) {

		/* When rto_cpu is -1 this acts like cpumask_first() */
/*
 * IAMROOT, 2023.02.11:
 * - -1부터 시작한다. rto_mask중에서 다음에 선택할 cpu를 정한다.
 *
 * - rto_cpu가 -1인 경우
 *   IPI RT push를 시작하였다. rto_mask에서 발견도니 첫번째 cpu로
 *   return될 것이다.
 *
 * - rto_cpu가 유효한 cpu번호 인경우
 *   rto_cpu의 다음 cpu로 정해진다.
 */
		cpu = cpumask_next(rd->rto_cpu, rd->rto_mask);

		rd->rto_cpu = cpu;

		if (cpu < nr_cpu_ids)
			return cpu;


/*
 * IAMROOT, 2023.02.11:
 * - rto_cpu가 유효한 cpu번호 인경우에 대해서 만약 rto_mask에 더 이상 
 *   남아있는 cpu가 없으면 아래로 진입한다.
 *   rto_cpu를 다시 -1로 설정한다.
 */
		rd->rto_cpu = -1;

		/*
		 * ACQUIRE ensures we see the @rto_mask changes
		 * made prior to the @next value observed.
		 *
		 * Matches WMB in rt_set_overload().
		 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   ACQUIRE는 @next 값이 관찰되기 전에 이루어진 @rto_mask 변경 사항을 
 *   확인하도록 합니다.
 *
 *   rt_set_overload()에서 WMB와 일치합니다.
 *
 * - loop가 일어나면 rto_loop와 비교한다. 변경이 발생됬으면 갱신하고
 *   다시 loop로 동작한다. 그게 아닌 경우 return -1
 */
		next = atomic_read_acquire(&rd->rto_loop_next);

		if (rd->rto_loop == next)
			break;

		rd->rto_loop = next;
	}

	return -1;
}

static inline bool rto_start_trylock(atomic_t *v)
{
	return !atomic_cmpxchg_acquire(v, 0, 1);
}

static inline void rto_start_unlock(atomic_t *v)
{
	atomic_set_release(v, 0);
}

/*
 * IAMROOT, 2023.02.11:
 * - IPI RT 푸시 상태인경우 cpu를 골라와서 해당 cpu에 ipi work on을 해준다.
 * - pull 이 필요한 cpu가 overload된 cpu에 대해서 자신한테 push를 하라는 
 *   ipi를 보내는 상황이다.
 */
static void tell_cpu_to_push(struct rq *rq)
{
	int cpu = -1;

	/* Keep the loop going if the IPI is currently active */
	atomic_inc(&rq->rd->rto_loop_next);

	/* Only one CPU can initiate a loop at a time */
	if (!rto_start_trylock(&rq->rd->rto_loop_start))
		return;

	raw_spin_lock(&rq->rd->rto_lock);

	/*
	 * The rto_cpu is updated under the lock, if it has a valid CPU
	 * then the IPI is still running and will continue due to the
	 * update to loop_next, and nothing needs to be done here.
	 * Otherwise it is finishing up and an ipi needs to be sent.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   rto_cpu는 잠금 상태에서 업데이트됩니다. 유효한 CPU가 있는 경우 
 *   IPI는 여전히 실행 중이고 loop_next에 대한 업데이트로 인해 계속 
 *   실행되며 여기서 수행할 작업은 없습니다.
 *   그렇지 않으면 완료되고 ipi를 보내야 합니다.
 *
 * - rto_cpu < 0 : IPI RT 푸시를 시작한 상태. 이 경우
 *   cpu를 골라 온다.
 */
	if (rq->rd->rto_cpu < 0)
		cpu = rto_next_cpu(rq->rd);

	raw_spin_unlock(&rq->rd->rto_lock);

	rto_start_unlock(&rq->rd->rto_loop_start);


/*
 * IAMROOT, 2023.02.11:
 * - 위에서 cpu가 골라졌으면 해당 cpu에 ipi work on을 해준다.
 *   ex) rto_push_irq_work_func
 */
	if (cpu >= 0) {
		/* Make sure the rd does not get freed while pushing */
		sched_get_rd(rq->rd);
		irq_work_queue_on(&rq->rd->rto_push_work, cpu);
	}
}

/* Called from hardirq context */

/*
 * IAMROOT, 2023.02.11:
 * - @rq의 모든 pushable task를 push하고, 그 다음 cpu한테도
 *   (있을 경우) irq work on 요청을 한다.
 */
void rto_push_irq_work_func(struct irq_work *work)
{
	struct root_domain *rd =
		container_of(work, struct root_domain, rto_push_work);
	struct rq *rq;
	int cpu;

	rq = this_rq();

	/*
	 * We do not need to grab the lock to check for has_pushable_tasks.
	 * When it gets updated, a check is made if a push is possible.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - push task를 전부 push 한다.
 */
	if (has_pushable_tasks(rq)) {
		raw_spin_rq_lock(rq);
		while (push_rt_task(rq, true))
			;
		raw_spin_rq_unlock(rq);
	}

	raw_spin_lock(&rd->rto_lock);

	/* Pass the IPI to the next rt overloaded queue */
/*
 * IAMROOT, 2023.02.11:
 * - 다음 cpu한테도 push 작업을 하게 한다.
 */
	cpu = rto_next_cpu(rd);

	raw_spin_unlock(&rd->rto_lock);

	if (cpu < 0) {
		sched_put_rd(rd);
		return;
	}

	/* Try the next RT overloaded CPU */
	irq_work_queue_on(&rd->rto_push_work, cpu);
}
#endif /* HAVE_RT_PUSH_IPI */

/*
 * IAMROOT, 2023.02.11:
 * - ipi 지원
 *   @this_rq에 push를 요청한다.
 *
 * - ipi 미지원
 *   overload된 cpu들을 순회해가면서 highest pushable task를 선택해서
 *   this_rq로 migrate한다.
 *   만약에 migrate를 못하는 경우 해당 cpu의 curr를 stopper를 이용해서
 *   push한다.
 */
static void pull_rt_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	bool resched = false;
	struct task_struct *p, *push_task;
	struct rq *src_rq;
	int rt_overload_count = rt_overloaded(this_rq);

	if (likely(!rt_overload_count))
		return;

	/*
	 * Match the barrier from rt_set_overloaded; this guarantees that if we
	 * see overloaded we must also see the rto_mask bit.
	 */
	smp_rmb();

	/* If we are the only overloaded CPU do nothing */
/*
 * IAMROOT, 2023.02.11:
 * - 현재 cpu에 overloaded 되있다면 return.
 *   즉 자기가 overload를 시켜놓은 상태.
 */
	if (rt_overload_count == 1 &&
	    cpumask_test_cpu(this_rq->cpu, this_rq->rd->rto_mask))
		return;

#ifdef HAVE_RT_PUSH_IPI
	if (sched_feat(RT_PUSH_IPI)) {
		tell_cpu_to_push(this_rq);
		return;
	}
#endif

/*
 * IAMROOT, 2023.02.18:
 * - IPI feat를 안쓰는 상황.
 * - overload된 cpu들을 순회한다.
 */
	for_each_cpu(cpu, this_rq->rd->rto_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * Don't bother taking the src_rq->lock if the next highest
		 * task is known to be lower-priority than our current task.
		 * This may look racy, but if this value is about to go
		 * logically higher, the src_rq will push this task away.
		 * And if its going logically lower, we do not care
		 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   다음으로 높은 작업이 현재 작업보다 우선 순위가 낮은 것으로 알려진 
 *   경우 src_rq->lock을 사용하지 마십시오.
 *   정확해 보일 수 있지만 이 값이 논리적으로 더 높아지면 src_rq는 이 
 *   작업을 밀어냅니다.
 *   그리고 그것이 논리적으로 낮아진다면 우리는 상관하지 않습니다. 
 *
 * - 순회중인 cpu의 next가 현재 cpu의 curr보다 낮거나 같은경우 skip한다.
 *   현재 cpu 에서 순회중읜 cpu의 next를 동작시킬때에는 당연히 curr보다
 *   높은 우선순위를 가져와야 하기 때문이다.
 */
		if (src_rq->rt.highest_prio.next >=
		    this_rq->rt.highest_prio.curr)
			continue;

		/*
		 * We can potentially drop this_rq's lock in
		 * double_lock_balance, and another CPU could
		 * alter this_rq
		 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   잠재적으로 double_lock_balance에서 this_rq의 잠금을 해제할 수 
 *   있으며 다른 CPU가 this_rq를 변경할 수 있습니다. 
 *
 * - curr보다 높은 순위의 task를 고를수있는 상황. double lock을 건다.
 */
		push_task = NULL;
		double_lock_balance(this_rq, src_rq);

		/*
		 * We can pull only a task, which is pushable
		 * on its rq, and no others.
		 */
/*
 * IAMROOT, 2023.02.18:
 * - src_rq에서 가장 우선순위가 높은 pushable task를 얻어온다.
 */
		p = pick_highest_pushable_task(src_rq, this_cpu);

		/*
		 * Do we have an RT task that preempts
		 * the to-be-scheduled task?
		 */

/*
 * IAMROOT, 2023.02.18:
 * - lock걸고 다시한번 확인하는 상황이다.
 */
		if (p && (p->prio < this_rq->rt.highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!task_on_rq_queued(p));

			/*
			 * There's a chance that p is higher in priority
			 * than what's currently running on its CPU.
			 * This is just that p is waking up and hasn't
			 * had a chance to schedule. We only pull
			 * p if it is lower in priority than the
			 * current task on the run queue
			 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   현재 CPU에서 실행 중인 것보다 p의 우선순위가 더 높을 가능성이 
 *   있습니다.
 *   이것은 단지 p가 깨어났고 일정을 잡을 기회가 없었다는 것입니다. 
 *   실행 대기열의 현재 작업보다 우선 순위가 낮은 경우에만 p를 가져옵니다.
 */
			if (p->prio < src_rq->curr->prio)
				goto skip;

/*
 * IAMROOT, 2023.02.18:
 * - p가 migrate disable인 상황이면 src_rq의 curr를 stop시키고 src_rq로
 *   push 요청을 한다.
 */
			if (is_migration_disabled(p)) {
				push_task = get_push_task(src_rq);
			} else {
/*
 * IAMROOT, 2023.02.18:
 * - 다른 cpu의 task를 this cpu로 꺼내온다.(pull)
 */
				deactivate_task(src_rq, p, 0);
				set_task_cpu(p, this_cpu);
				activate_task(this_rq, p, 0);
				resched = true;
			}
			/*
			 * We continue with the search, just in
			 * case there's an even higher prio task
			 * in another runqueue. (low likelihood
			 * but possible)
			 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   다른 runqueue에 더 높은 우선 순위 작업이 있는 경우를 대비하여 
 *   검색을 계속합니다. (가능성은 낮지만 가능).
 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);

/*
 * IAMROOT, 2023.02.18:
 * - 위에서 선택된 push_task는 다른 lowest rq를 가진 cpu로 push될것이다.
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
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
/*
 * IAMROOT, 2023.02.18:
 * - sleep에서 깨어낫을때 그냥 동작시킬지 push를 할지 선택한다.
 */
static void task_woken_rt(struct rq *rq, struct task_struct *p)
{
	bool need_to_push = !task_running(rq, p) &&
			    !test_tsk_need_resched(rq->curr) &&
			    p->nr_cpus_allowed > 1 &&
			    (dl_task(rq->curr) || rt_task(rq->curr)) &&
			    (rq->curr->nr_cpus_allowed < 2 ||
			     rq->curr->prio <= p->prio);

	if (need_to_push)
		push_rt_tasks(rq);
}

/* Assumes rq->lock is held */
static void rq_online_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_set_overload(rq);

	__enable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_clear_overload(rq);

	__disable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_INVALID);
}

/*
 * When switch from the rt queue, we bring ourselves to a position
 * that we might want to pull RT tasks from other runqueues.
 */
static void switched_from_rt(struct rq *rq, struct task_struct *p)
{
	/*
	 * If there are other RT tasks then we will reschedule
	 * and the scheduling of the other RT tasks will handle
	 * the balancing. But if we are the last RT task
	 * we may need to handle the pulling of RT tasks
	 * now.
	 */
	if (!task_on_rq_queued(p) || rq->rt.rt_nr_running)
		return;

	rt_queue_pull_task(rq);
}

void __init init_sched_rt_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i) {
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask, i),
					GFP_KERNEL, cpu_to_node(i));
	}
}
#endif /* CONFIG_SMP */

/*
 * When switching a task to RT, we may overload the runqueue
 * with RT tasks. In this case we try to push them off to
 * other runqueues.
 */
/*
 * IAMROOT, 2023.02.18:
 * - ex) cfs -> rt
 */
static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
	/*
	 * If we are running, update the avg_rt tracking, as the running time
	 * will now on be accounted into the latter.
	 */
	if (task_current(rq, p)) {
		update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);
		return;
	}

	/*
	 * If we are not running we may need to preempt the current
	 * running task. If that current running task is also an RT task
	 * then see if we can move to another run queue.
	 */
	if (task_on_rq_queued(p)) {
#ifdef CONFIG_SMP

/*
 * IAMROOT, 2023.02.18:
 * - ex) [rt] [rt] [cfs] 인 상황에서 cfs -> rt로 바뀌는 상황.
 */
		if (p->nr_cpus_allowed > 1 && rq->rt.overloaded)
			rt_queue_push_tasks(rq);
#endif /* CONFIG_SMP */
		if (p->prio < rq->curr->prio && cpu_online(cpu_of(rq)))
			resched_curr(rq);
	}
}

/*
 * Priority of the task has changed. This may cause
 * us to initiate a push or pull.
 */
static void
prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!task_on_rq_queued(p))
		return;

	if (task_current(rq, p)) {
#ifdef CONFIG_SMP
		/*
		 * If our priority decreases while running, we
		 * may need to pull tasks to this runqueue.
		 */
		if (oldprio < p->prio)
			rt_queue_pull_task(rq);

		/*
		 * If there's a higher priority task waiting to run
		 * then reschedule.
		 */
		if (p->prio > rq->rt.highest_prio.curr)
			resched_curr(rq);
#else
		/* For UP simply resched on drop of prio */
		if (oldprio < p->prio)
			resched_curr(rq);
#endif /* CONFIG_SMP */
	} else {
		/*
		 * This task is not running, but if it is
		 * greater than the current running task
		 * then reschedule.
		 */
		if (p->prio < rq->curr->prio)
			resched_curr(rq);
	}
}

#ifdef CONFIG_POSIX_TIMERS
/*
 * IAMROOT, 2023.02.10:
 * - task rlimit이 활성화됭ㅆ으면 timeout을 기록하고 제한값 초과이면
 *   timer의 CPUCLOCK_SCHED에 sum_exec_runtime을 nextevt로 기록한다.
 */
static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);
/*
 * IAMROOT, 2023.02.10:
 * - rlimit이 활성화되있으면
 */
	if (soft != RLIM_INFINITY) {
		unsigned long next;
/*
 * IAMROOT, 2023.02.10:
 * - 중복 기록 방지. 이전 기록 시관가 다를때만 timeout 증가
 */
		if (p->rt.watchdog_stamp != jiffies) {
			p->rt.timeout++;
			p->rt.watchdog_stamp = jiffies;
		}

/*
 * IAMROOT, 2023.02.10:
 * - soft / hard 시간중에 작은것을 기준으로 제한값을 계산한다.
 *   제한값보다 timeout시간이 크면 timer의 CPUCLOCK_SCHED에 
 *   sum_exec_runtime을 nextevt로 기록한다.
 */
		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next) {
			posix_cputimers_rt_watchdog(&p->posix_cputimers,
						    p->se.sum_exec_runtime);
		}
	}
}
#else
static inline void watchdog(struct rq *rq, struct task_struct *p) { }
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
 * IAMROOT, 2023.02.10:
 * - 1. exec_runtime 및 rt_runtime 누적
 *   2. rt_runtime 초과에 따른 reschedule 처리
 *   3. load sun / avg update
 *   4. runtime balance
 *   5. SCHED_RR 처리.
 */
static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);
	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	watchdog(rq, p);

	/*
	 * RR tasks need a special form of timeslice management.
	 * FIFO tasks have no timeslices.
	 */
	/*
	 * IAMROOT. 2023.02.04:
	 * - google-translate
	 *   RR 작업에는 특별한 형태의 타임슬라이스 관리가 필요합니다. FIFO 작업에는 타임
	 *   슬라이스가 없습니다.
	 *
	 * - 즉 FIFO는 여기서 return된다. FIFO는 task가 queued에서 없어질때까지 수행할
	 *   뿐이기 때문이다.
	 */
	if (p->policy != SCHED_RR)
		return;

/*
 * IAMROOT, 2023.02.10:
 * - 여기부터는 RR 스케쥴링이다. RR에 대한 스케쥴링은 아래 코드들로 끝난다.
 * - time_slice가 아직 소모안됬으면 여기서 return된다.
 */
	if (--p->rt.time_slice)
		return;

/*
 * IAMROOT, 2023.02.10:
 * - time_slice를 여기서 다시 충전한다.
 */
	p->rt.time_slice = sched_rr_timeslice;

	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are not
	 * the only element on the queue
	 */
	for_each_sched_rt_entity(rt_se) {
/*
 * IAMROOT, 2023.02.10:
 * - prev != next 라면 유일한 rt entity가 아니라는 의미. 
 *   즉 같은 우선순위의 entity가 두개이상일때 reschedule을 한다.
 */
		if (rt_se->run_list.prev != rt_se->run_list.next) {
/*
 * IAMROOT, 2023.02.10:
 * - @p를 rt_rq의 tail에 넣고 rescheduled 요청을 한다. 제일 뒤의 순서로
 *   옮기는 개념.
 */
			requeue_task_rt(rq, p, 0);
			resched_curr(rq);
			return;
		}
	}
}

static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 0 for SCHED_FIFO tasks
	 */
	if (task->policy == SCHED_RR)
		return sched_rr_timeslice;
	else
		return 0;
}

DEFINE_SCHED_CLASS(rt) = {

	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.yield_task		= yield_task_rt,

	.check_preempt_curr	= check_preempt_curr_rt,

	.pick_next_task		= pick_next_task_rt,
	.put_prev_task		= put_prev_task_rt,
	.set_next_task          = set_next_task_rt,

#ifdef CONFIG_SMP
	.balance		= balance_rt,
	.pick_task		= pick_task_rt,
	.select_task_rq		= select_task_rq_rt,
	.set_cpus_allowed       = set_cpus_allowed_common,
	.rq_online              = rq_online_rt,
	.rq_offline             = rq_offline_rt,
	.task_woken		= task_woken_rt,
	.switched_from		= switched_from_rt,
	.find_lock_rq		= find_lock_lowest_rq,
#endif

	.task_tick		= task_tick_rt,

	.get_rr_interval	= get_rr_interval_rt,

	.prio_changed		= prio_changed_rt,
	.switched_to		= switched_to_rt,

	.update_curr		= update_curr_rt,

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};

#ifdef CONFIG_RT_GROUP_SCHED
/*
 * Ensure that the real time constraints are schedulable.
 */
static DEFINE_MUTEX(rt_constraints_mutex);

static inline int tg_has_rt_tasks(struct task_group *tg)
{
	struct task_struct *task;
	struct css_task_iter it;
	int ret = 0;

	/*
	 * Autogroups do not have RT tasks; see autogroup_create().
	 */
	if (task_group_is_autogroup(tg))
		return 0;

	css_task_iter_start(&tg->css, 0, &it);
	while (!ret && (task = css_task_iter_next(&it)))
		ret |= rt_task(task);
	css_task_iter_end(&it);

	return ret;
}

struct rt_schedulable_data {
	struct task_group *tg;
	u64 rt_period;
	u64 rt_runtime;
};

static int tg_rt_schedulable(struct task_group *tg, void *data)
{
	struct rt_schedulable_data *d = data;
	struct task_group *child;
	unsigned long total, sum = 0;
	u64 period, runtime;

	period = ktime_to_ns(tg->rt_bandwidth.rt_period);
	runtime = tg->rt_bandwidth.rt_runtime;

	if (tg == d->tg) {
		period = d->rt_period;
		runtime = d->rt_runtime;
	}

	/*
	 * Cannot have more runtime than the period.
	 */
	if (runtime > period && runtime != RUNTIME_INF)
		return -EINVAL;

	/*
	 * Ensure we don't starve existing RT tasks if runtime turns zero.
	 */
	if (rt_bandwidth_enabled() && !runtime &&
	    tg->rt_bandwidth.rt_runtime && tg_has_rt_tasks(tg))
		return -EBUSY;

	total = to_ratio(period, runtime);

	/*
	 * Nobody can have more than the global setting allows.
	 */
	if (total > to_ratio(global_rt_period(), global_rt_runtime()))
		return -EINVAL;

	/*
	 * The sum of our children's runtime should not exceed our own.
	 */
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		period = ktime_to_ns(child->rt_bandwidth.rt_period);
		runtime = child->rt_bandwidth.rt_runtime;

		if (child == d->tg) {
			period = d->rt_period;
			runtime = d->rt_runtime;
		}

		sum += to_ratio(period, runtime);
	}

	if (sum > total)
		return -EINVAL;

	return 0;
}

static int __rt_schedulable(struct task_group *tg, u64 period, u64 runtime)
{
	int ret;

	struct rt_schedulable_data data = {
		.tg = tg,
		.rt_period = period,
		.rt_runtime = runtime,
	};

	rcu_read_lock();
	ret = walk_tg_tree(tg_rt_schedulable, tg_nop, &data);
	rcu_read_unlock();

	return ret;
}

static int tg_set_rt_bandwidth(struct task_group *tg,
		u64 rt_period, u64 rt_runtime)
{
	int i, err = 0;

	/*
	 * Disallowing the root group RT runtime is BAD, it would disallow the
	 * kernel creating (and or operating) RT threads.
	 */
	if (tg == &root_task_group && rt_runtime == 0)
		return -EINVAL;

	/* No period doesn't make any sense. */
	if (rt_period == 0)
		return -EINVAL;

	/*
	 * Bound quota to defend quota against overflow during bandwidth shift.
	 */
	if (rt_runtime != RUNTIME_INF && rt_runtime > max_rt_runtime)
		return -EINVAL;

	mutex_lock(&rt_constraints_mutex);
	err = __rt_schedulable(tg, rt_period, rt_runtime);
	if (err)
		goto unlock;

	raw_spin_lock_irq(&tg->rt_bandwidth.rt_runtime_lock);
	tg->rt_bandwidth.rt_period = ns_to_ktime(rt_period);
	tg->rt_bandwidth.rt_runtime = rt_runtime;

	for_each_possible_cpu(i) {
		struct rt_rq *rt_rq = tg->rt_rq[i];

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = rt_runtime;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
	raw_spin_unlock_irq(&tg->rt_bandwidth.rt_runtime_lock);
unlock:
	mutex_unlock(&rt_constraints_mutex);

	return err;
}

int sched_group_set_rt_runtime(struct task_group *tg, long rt_runtime_us)
{
	u64 rt_runtime, rt_period;

	rt_period = ktime_to_ns(tg->rt_bandwidth.rt_period);
	rt_runtime = (u64)rt_runtime_us * NSEC_PER_USEC;
	if (rt_runtime_us < 0)
		rt_runtime = RUNTIME_INF;
	else if ((u64)rt_runtime_us > U64_MAX / NSEC_PER_USEC)
		return -EINVAL;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

long sched_group_rt_runtime(struct task_group *tg)
{
	u64 rt_runtime_us;

	if (tg->rt_bandwidth.rt_runtime == RUNTIME_INF)
		return -1;

	rt_runtime_us = tg->rt_bandwidth.rt_runtime;
	do_div(rt_runtime_us, NSEC_PER_USEC);
	return rt_runtime_us;
}

int sched_group_set_rt_period(struct task_group *tg, u64 rt_period_us)
{
	u64 rt_runtime, rt_period;

	if (rt_period_us > U64_MAX / NSEC_PER_USEC)
		return -EINVAL;

	rt_period = rt_period_us * NSEC_PER_USEC;
	rt_runtime = tg->rt_bandwidth.rt_runtime;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

long sched_group_rt_period(struct task_group *tg)
{
	u64 rt_period_us;

	rt_period_us = ktime_to_ns(tg->rt_bandwidth.rt_period);
	do_div(rt_period_us, NSEC_PER_USEC);
	return rt_period_us;
}

static int sched_rt_global_constraints(void)
{
	int ret = 0;

	mutex_lock(&rt_constraints_mutex);
	ret = __rt_schedulable(NULL, 0, 0);
	mutex_unlock(&rt_constraints_mutex);

	return ret;
}

int sched_rt_can_attach(struct task_group *tg, struct task_struct *tsk)
{
	/* Don't accept realtime tasks when there is no way for them to run */
	if (rt_task(tsk) && tg->rt_bandwidth.rt_runtime == 0)
		return 0;

	return 1;
}

#else /* !CONFIG_RT_GROUP_SCHED */
static int sched_rt_global_constraints(void)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&def_rt_bandwidth.rt_runtime_lock, flags);
	for_each_possible_cpu(i) {
		struct rt_rq *rt_rq = &cpu_rq(i)->rt;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = global_rt_runtime();
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
	raw_spin_unlock_irqrestore(&def_rt_bandwidth.rt_runtime_lock, flags);

	return 0;
}
#endif /* CONFIG_RT_GROUP_SCHED */

static int sched_rt_global_validate(void)
{
	if (sysctl_sched_rt_period <= 0)
		return -EINVAL;

	if ((sysctl_sched_rt_runtime != RUNTIME_INF) &&
		((sysctl_sched_rt_runtime > sysctl_sched_rt_period) ||
		 ((u64)sysctl_sched_rt_runtime *
			NSEC_PER_USEC > max_rt_runtime)))
		return -EINVAL;

	return 0;
}

static void sched_rt_do_global(void)
{
	def_rt_bandwidth.rt_runtime = global_rt_runtime();
	def_rt_bandwidth.rt_period = ns_to_ktime(global_rt_period());
}

int sched_rt_handler(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	int old_period, old_runtime;
	static DEFINE_MUTEX(mutex);
	int ret;

	mutex_lock(&mutex);
	old_period = sysctl_sched_rt_period;
	old_runtime = sysctl_sched_rt_runtime;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret && write) {
		ret = sched_rt_global_validate();
		if (ret)
			goto undo;

		ret = sched_dl_global_validate();
		if (ret)
			goto undo;

		ret = sched_rt_global_constraints();
		if (ret)
			goto undo;

		sched_rt_do_global();
		sched_dl_do_global();
	}
	if (0) {
undo:
		sysctl_sched_rt_period = old_period;
		sysctl_sched_rt_runtime = old_runtime;
	}
	mutex_unlock(&mutex);

	return ret;
}

int sched_rr_handler(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	int ret;
	static DEFINE_MUTEX(mutex);

	mutex_lock(&mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	/*
	 * Make sure that internally we keep jiffies.
	 * Also, writing zero resets the timeslice to default:
	 */
	if (!ret && write) {
		sched_rr_timeslice =
			sysctl_sched_rr_timeslice <= 0 ? RR_TIMESLICE :
			msecs_to_jiffies(sysctl_sched_rr_timeslice);
	}
	mutex_unlock(&mutex);

	return ret;
}

#ifdef CONFIG_SCHED_DEBUG
void print_rt_stats(struct seq_file *m, int cpu)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	rcu_read_lock();
	for_each_rt_rq(rt_rq, iter, cpu_rq(cpu))
		print_rt_rq(m, cpu, rt_rq);
	rcu_read_unlock();
}
#endif /* CONFIG_SCHED_DEBUG */
