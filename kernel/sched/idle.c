// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic entry points for the idle threads and
 * implementation of the idle task scheduling class.
 *
 * (NOTE: these are not related to SCHED_IDLE batch scheduled
 *        tasks which are handled in sched/fair.c )
 */
#include "sched.h"

#include <trace/events/power.h>

/* Linker adds these: start and end of __cpuidle functions */
extern char __cpuidle_text_start[], __cpuidle_text_end[];

/**
 * sched_idle_set_state - Record idle state for the current CPU.
 * @idle_state: State to record.
 */
void sched_idle_set_state(struct cpuidle_state *idle_state)
{
	idle_set_state(this_rq(), idle_state);
}

static int __read_mostly cpu_idle_force_poll;

void cpu_idle_poll_ctrl(bool enable)
{
	if (enable) {
		cpu_idle_force_poll++;
	} else {
		cpu_idle_force_poll--;
		WARN_ON_ONCE(cpu_idle_force_poll < 0);
	}
}

#ifdef CONFIG_GENERIC_IDLE_POLL_SETUP
static int __init cpu_idle_poll_setup(char *__unused)
{
	cpu_idle_force_poll = 1;

	return 1;
}
__setup("nohlt", cpu_idle_poll_setup);

static int __init cpu_idle_nopoll_setup(char *__unused)
{
	cpu_idle_force_poll = 0;

	return 1;
}
__setup("hlt", cpu_idle_nopoll_setup);
#endif

/*
 * IAMROOT, 2023.03.11:
 * - cpu 전원을 꺼지 않고 reschedule 요청이 있을때 까지 계속 polling
 */
static noinline int __cpuidle cpu_idle_poll(void)
{
	trace_cpu_idle(0, smp_processor_id());
	stop_critical_timings();
	rcu_idle_enter();
	local_irq_enable();

	while (!tif_need_resched() &&
	       (cpu_idle_force_poll || tick_check_broadcast_expired()))
		cpu_relax();

	rcu_idle_exit();
	start_critical_timings();
	trace_cpu_idle(PWR_EVENT_EXIT, smp_processor_id());

	return 1;
}

/* Weak implementations for optional arch specific functions */
void __weak arch_cpu_idle_prepare(void) { }
void __weak arch_cpu_idle_enter(void) { }
void __weak arch_cpu_idle_exit(void) { }
void __weak arch_cpu_idle_dead(void) { }
void __weak arch_cpu_idle(void)
{
	cpu_idle_force_poll = 1;
	raw_local_irq_enable();
}

/**
 * default_idle_call - Default CPU idle routine.
 *
 * To use when the cpuidle framework cannot be used.
 */
/*
 * IAMROOT, 2023.03.11:
 * - cpuidle 드라이버가 없을때 동작하는 함수. (wfi 로 대기)
 */
void __cpuidle default_idle_call(void)
{
	if (current_clr_polling_and_test()) {
		local_irq_enable();
	} else {

		trace_cpu_idle(1, smp_processor_id());
		stop_critical_timings();

		/*
		 * arch_cpu_idle() is supposed to enable IRQs, however
		 * we can't do that because of RCU and tracing.
		 *
		 * Trace IRQs enable here, then switch off RCU, and have
		 * arch_cpu_idle() use raw_local_irq_enable(). Note that
		 * rcu_idle_enter() relies on lockdep IRQ state, so switch that
		 * last -- this is very similar to the entry code.
		 */
		trace_hardirqs_on_prepare();
		lockdep_hardirqs_on_prepare(_THIS_IP_);
		rcu_idle_enter();
		lockdep_hardirqs_on(_THIS_IP_);

		arch_cpu_idle();

		/*
		 * OK, so IRQs are enabled here, but RCU needs them disabled to
		 * turn itself back on.. funny thing is that disabling IRQs
		 * will cause tracing, which needs RCU. Jump through hoops to
		 * make it 'work'.
		 */
		raw_local_irq_disable();
		lockdep_hardirqs_off(_THIS_IP_);
		rcu_idle_exit();
		lockdep_hardirqs_on(_THIS_IP_);
		raw_local_irq_enable();

		start_critical_timings();
		trace_cpu_idle(PWR_EVENT_EXIT, smp_processor_id());
	}
}

static int call_cpuidle_s2idle(struct cpuidle_driver *drv,
			       struct cpuidle_device *dev)
{
	if (current_clr_polling_and_test())
		return -EBUSY;

	return cpuidle_enter_s2idle(drv, dev);
}

static int call_cpuidle(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		      int next_state)
{
	/*
	 * The idle task must be scheduled, it is pointless to go to idle, just
	 * update no idle residency and return.
	 */
	if (current_clr_polling_and_test()) {
		dev->last_residency_ns = 0;
		local_irq_enable();
		return -EBUSY;
	}

	/*
	 * Enter the idle state previously returned by the governor decision.
	 * This function will block until an interrupt occurs and will take
	 * care of re-enabling the local interrupts
	 */
	return cpuidle_enter(drv, dev, next_state);
}

/**
 * cpuidle_idle_call - the main idle function
 *
 * NOTE: no locks or semaphores should be used here
 *
 * On architectures that support TIF_POLLING_NRFLAG, is called with polling
 * set, and it returns with polling set.  If it ever stops polling, it
 * must clear the polling bit.
 */
/*
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   cpuidle_idle_call - 주 idle function
 *
 *   참고: 여기서 잠금이나 세마포어를 사용해서는 안 됩니다.
 *
 *   TIF_POLLING_NRFLAG를 지원하는 아키텍처에서 폴링 세트로 호출되고 폴링
 *   세트로 반환됩니다. 폴링이 중지되면 폴링 비트를 지워야 합니다.
 */
/*
 * IAMROOT, 2023.03.11:
 * - min-residency-us : 최소 상주 시간. 켜진후 꺼지기 전까지 최소 유지해야할 시간
 * - rk3399.rtsi
 *   idle-states {
 *			entry-method = "psci";
 *
 *			CPU_SLEEP: cpu-sleep {
 *				compatible = "arm,idle-state";
 *				local-timer-stop;
 *				arm,psci-suspend-param = <0x0010000>;
 *				entry-latency-us = <120>;
 *				exit-latency-us = <250>;
 *				min-residency-us = <900>;
 *			};
 *
 *			CLUSTER_SLEEP: cluster-sleep {
 *				compatible = "arm,idle-state";
 *				local-timer-stop;
 *				arm,psci-suspend-param = <0x1010000>;
 *				entry-latency-us = <400>;
 *				exit-latency-us = <500>;
 *				min-residency-us = <2000>;
 *			};
 *		};
 *
 */
static void cpuidle_idle_call(void)
{
	struct cpuidle_device *dev = cpuidle_get_device();
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	int next_state, entered_state;

	/*
	 * Check if the idle task must be rescheduled. If it is the
	 * case, exit the function after re-enabling the local irq.
	 */
	/*
	 * IAMROOT. 2023.03.11:
	 * - google-translate
	 *   유휴 작업을 다시 예약해야 하는지 확인합니다. 이 경우 로컬 irq를 다시
	 *   활성화한 후 기능을 종료하십시오.
	 */
	if (need_resched()) {
		local_irq_enable();
		return;
	}

	/*
	 * The RCU framework needs to be told that we are entering an idle
	 * section, so no more rcu read side critical sections and one more
	 * step to the grace period
	 */
	/*
	 * IAMROOT. 2023.03.11:
	 * - google-translate
	 *   RCU 프레임워크는 우리가 유휴 섹션에 들어가고 있다는 것을 알려야 합니다. 따라서
	 *   rcu는 더 이상 중요한 섹션을 읽지 않고 유예 기간에 한 단계 더 있습니다.
	 */

	if (cpuidle_not_available(drv, dev)) {
		tick_nohz_idle_stop_tick();

		default_idle_call();
		goto exit_idle;
	}

	/*
	 * Suspend-to-idle ("s2idle") is a system state in which all user space
	 * has been frozen, all I/O devices have been suspended and the only
	 * activity happens here and in interrupts (if any). In that case bypass
	 * the cpuidle governor and go straight for the deepest idle state
	 * available.  Possibly also suspend the local tick and the entire
	 * timekeeping to prevent timer interrupts from kicking us out of idle
	 * until a proper wakeup interrupt happens.
	 */
	/*
	 * IAMROOT. 2023.03.11:
	 * - google-translate
	 *   Suspend-to-idle("s2idle")은 모든 사용자 공간이 정지되고 모든 I/O 장치가 일시
	 *   중단되고 여기와 인터럽트(있는 경우)에서만 활동이 발생하는 시스템 상태입니다. 이
	 *   경우 cpuidle 거버너를 우회하고 사용 가능한 가장 깊은 유휴 상태로 바로
	 *   이동합니다. 적절한 웨이크업 인터럽트가 발생할 때까지 타이머 인터럽트가 유휴
	 *   상태에서 벗어나는 것을 방지하기 위해 로컬 틱과 전체 타임키핑을 일시 중지할 수도
	 *   있습니다.
	 */

	if (idle_should_enter_s2idle() || dev->forced_idle_latency_limit_ns) {
		u64 max_latency_ns;

		if (idle_should_enter_s2idle()) {

			entered_state = call_cpuidle_s2idle(drv, dev);
			if (entered_state > 0)
				goto exit_idle;

			max_latency_ns = U64_MAX;
		} else {
			max_latency_ns = dev->forced_idle_latency_limit_ns;
		}

		tick_nohz_idle_stop_tick();

		next_state = cpuidle_find_deepest_state(drv, dev, max_latency_ns);
		call_cpuidle(drv, dev, next_state);
	} else {
		bool stop_tick = true;

		/*
		 * Ask the cpuidle framework to choose a convenient idle state.
		 */
		next_state = cpuidle_select(drv, dev, &stop_tick);

		if (stop_tick || tick_nohz_tick_stopped())
			tick_nohz_idle_stop_tick();
		else
			tick_nohz_idle_retain_tick();

		entered_state = call_cpuidle(drv, dev, next_state);
		/*
		 * Give the governor an opportunity to reflect on the outcome
		 */
		cpuidle_reflect(dev, entered_state);
	}

exit_idle:
	__current_set_polling();

	/*
	 * It is up to the idle functions to reenable local interrupts
	 */
	if (WARN_ON_ONCE(irqs_disabled()))
		local_irq_enable();
}

/*
 * Generic idle loop implementation
 *
 * Called with polling cleared.
 */
/*
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   일반 유휴 루프 구현
 *
 *   폴링이 지워진 상태로 호출됩니다.
 */
static void do_idle(void)
{
	int cpu = smp_processor_id();

	/*
	 * Check if we need to update blocked load
	 */
	nohz_run_idle_balance(cpu);

	/*
	 * If the arch has a polling bit, we maintain an invariant:
	 *
	 * Our polling bit is clear if we're not scheduled (i.e. if rq->curr !=
	 * rq->idle). This means that, if rq->idle has the polling bit set,
	 * then setting need_resched is guaranteed to cause the CPU to
	 * reschedule.
	 */
	/*
	 * IAMROOT. 2023.03.11:
	 * - google-translate
	 *   아치에 폴링 비트가 있는 경우 불변성을 유지합니다. 일정이 지정되지 않은 경우(즉,
	 *   rq->curr != rq->idle인 경우) 폴링 비트는 명확합니다. 즉, rq->idle에 폴링 비트가
	 *   설정되어 있으면 need_resched를 설정하면 CPU가 다시 일정을 잡게 됩니다.
	 */

	__current_set_polling();
	tick_nohz_idle_enter();

	while (!need_resched()) {
		rmb();

		local_irq_disable();

		if (cpu_is_offline(cpu)) {
			tick_nohz_idle_stop_tick();
			cpuhp_report_idle_dead();
			arch_cpu_idle_dead();
		}

		arch_cpu_idle_enter();
		rcu_nocb_flush_deferred_wakeup();

		/*
		 * In poll mode we reenable interrupts and spin. Also if we
		 * detected in the wakeup from idle path that the tick
		 * broadcast device expired for us, we don't want to go deep
		 * idle as we know that the IPI is going to arrive right away.
		 */
		/*
		 * IAMROOT. 2023.03.11:
		 * - google-translate
		 *   폴링 모드에서는 인터럽트와 스핀을 다시 활성화합니다. 또한
		 *   틱 브로드캐스트 장치가 만료된 유휴 경로에서 깨어남을 감지한 경우
		 *   IPI가 바로 도착할 것이라는 것을 알기 때문에 깊은 유휴 상태로
		 *   가고 싶지 않습니다.
		 */
		if (cpu_idle_force_poll || tick_check_broadcast_expired()) {
			tick_nohz_idle_restart_tick();
			cpu_idle_poll();
		} else {
			cpuidle_idle_call();
		}
		arch_cpu_idle_exit();
	}

	/*
	 * Since we fell out of the loop above, we know TIF_NEED_RESCHED must
	 * be set, propagate it into PREEMPT_NEED_RESCHED.
	 *
	 * This is required because for polling idle loops we will not have had
	 * an IPI to fold the state for us.
	 */
	preempt_set_need_resched();
	tick_nohz_idle_exit();
	__current_clr_polling();

	/*
	 * We promise to call sched_ttwu_pending() and reschedule if
	 * need_resched() is set while polling is set. That means that clearing
	 * polling needs to be visible before doing these things.
	 */
	smp_mb__after_atomic();

	/*
	 * RCU relies on this call to be done outside of an RCU read-side
	 * critical section.
	 */
	flush_smp_call_function_from_idle();
	schedule_idle();

	if (unlikely(klp_patch_pending(current)))
		klp_update_patch_state(current);
}

bool cpu_in_idle(unsigned long pc)
{
	return pc >= (unsigned long)__cpuidle_text_start &&
		pc < (unsigned long)__cpuidle_text_end;
}

struct idle_timer {
	struct hrtimer timer;
	int done;
};

static enum hrtimer_restart idle_inject_timer_fn(struct hrtimer *timer)
{
	struct idle_timer *it = container_of(timer, struct idle_timer, timer);

	WRITE_ONCE(it->done, 1);
	set_tsk_need_resched(current);

	return HRTIMER_NORESTART;
}

void play_idle_precise(u64 duration_ns, u64 latency_ns)
{
	struct idle_timer it;

	/*
	 * Only FIFO tasks can disable the tick since they don't need the forced
	 * preemption.
	 */
	WARN_ON_ONCE(current->policy != SCHED_FIFO);
	WARN_ON_ONCE(current->nr_cpus_allowed != 1);
	WARN_ON_ONCE(!(current->flags & PF_KTHREAD));
	WARN_ON_ONCE(!(current->flags & PF_NO_SETAFFINITY));
	WARN_ON_ONCE(!duration_ns);
	WARN_ON_ONCE(current->mm);

	rcu_sleep_check();
	preempt_disable();
	current->flags |= PF_IDLE;
	cpuidle_use_deepest_state(latency_ns);

	it.done = 0;
	hrtimer_init_on_stack(&it.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	it.timer.function = idle_inject_timer_fn;
	hrtimer_start(&it.timer, ns_to_ktime(duration_ns),
		      HRTIMER_MODE_REL_PINNED_HARD);

	while (!READ_ONCE(it.done))
		do_idle();

	cpuidle_use_deepest_state(0);
	current->flags &= ~PF_IDLE;

	preempt_fold_need_resched();
	preempt_enable();
}
EXPORT_SYMBOL_GPL(play_idle_precise);

/*
 * IAMROOT, 2023.03.11:
 * - boot up 마지막에서 호출된다.
 *   cpu 0 : start_kernel -> arch_call_rest_init -> rest_init 에서 호출
 *   그외 cpu : secondary_start_kernel 에서 호출
 */
void cpu_startup_entry(enum cpuhp_state state)
{
	arch_cpu_idle_prepare();
	cpuhp_online_idle(state);
	while (1)
		do_idle();
}

/*
 * idle-task scheduling class.
 */

#ifdef CONFIG_SMP
static int
select_task_rq_idle(struct task_struct *p, int cpu, int flags)
{
	return task_cpu(p); /* IDLE tasks as never migrated */
}

static int
balance_idle(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	return WARN_ON_ONCE(1);
}
#endif

/*
 * Idle tasks are unconditionally rescheduled:
 */
static void check_preempt_curr_idle(struct rq *rq, struct task_struct *p, int flags)
{
	resched_curr(rq);
}

static void put_prev_task_idle(struct rq *rq, struct task_struct *prev)
{
}

static void set_next_task_idle(struct rq *rq, struct task_struct *next, bool first)
{
	update_idle_core(rq);
	schedstat_inc(rq->sched_goidle);
	queue_core_balance(rq);
}

#ifdef CONFIG_SMP
static struct task_struct *pick_task_idle(struct rq *rq)
{
	return rq->idle;
}
#endif

struct task_struct *pick_next_task_idle(struct rq *rq)
{
	struct task_struct *next = rq->idle;

	set_next_task_idle(rq, next, true);

	return next;
}

/*
 * It is not legal to sleep in the idle task - print a warning
 * message if some code attempts to do it:
 */
static void
dequeue_task_idle(struct rq *rq, struct task_struct *p, int flags)
{
	raw_spin_rq_unlock_irq(rq);
	printk(KERN_ERR "bad: scheduling from the idle thread!\n");
	dump_stack();
	raw_spin_rq_lock_irq(rq);
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
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   스케줄링 클래스의 작업을 치는 스케줄러 틱.
 *
 *   참고: 이 함수는 완전한 dynticks를
 *   따라가는 틱 오프로드에 의해 원격으로 호출될 수 있습니다. 따라서 로컬 가정을 할
 *   수 없으며 매개 변수에 전달된 @rq 및 @curr를 통해 모든 항목에 액세스해야 합니다.
 */
static void task_tick_idle(struct rq *rq, struct task_struct *curr, int queued)
{
}

static void switched_to_idle(struct rq *rq, struct task_struct *p)
{
	BUG();
}

static void
prio_changed_idle(struct rq *rq, struct task_struct *p, int oldprio)
{
	BUG();
}

static void update_curr_idle(struct rq *rq)
{
}

/*
 * Simple, special scheduling class for the per-CPU idle tasks:
 */
DEFINE_SCHED_CLASS(idle) = {

	/* no enqueue/yield_task for idle tasks */

	/* dequeue is not valid, we print a debug message there: */
	.dequeue_task		= dequeue_task_idle,

	.check_preempt_curr	= check_preempt_curr_idle,

	.pick_next_task		= pick_next_task_idle,
	.put_prev_task		= put_prev_task_idle,
	.set_next_task          = set_next_task_idle,

#ifdef CONFIG_SMP
	.balance		= balance_idle,
	.pick_task		= pick_task_idle,
	.select_task_rq		= select_task_rq_idle,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.task_tick		= task_tick_idle,

	.prio_changed		= prio_changed_idle,
	.switched_to		= switched_to_idle,
	.update_curr		= update_curr_idle,
};
