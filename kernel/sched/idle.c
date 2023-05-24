// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic entry points for the idle threads and
 * implementation of the idle task scheduling class.
 *
 * (NOTE: these are not related to SCHED_IDLE batch scheduled
 *        tasks which are handled in sched/fair.c )
 */
/*
 * IAMROOT, 2023.05.24:
 * - papago
 *   유휴 스레드에 대한 일반 진입점 및 유휴 작업 스케줄링 클래스 구현.
 *
 *   (NOTE: 이들은 sched/fair.c에서 처리되는 SCHED_IDLE 일괄 예약 
 *   작업과 관련이 없습니다. 
 */
#include "sched.h"

#include <trace/events/power.h>

/* Linker adds these: start and end of __cpuidle functions */
extern char __cpuidle_text_start[], __cpuidle_text_end[];

/**
 * sched_idle_set_state - Record idle state for the current CPU.
 * @idle_state: State to record.
 */
/*
 * IAMROOT, 2023.03.18:
 * - this rq의 현재 idle state를 @idle_state로 설정한다.
 */
void sched_idle_set_state(struct cpuidle_state *idle_state)
{
	idle_set_state(this_rq(), idle_state);
}

/*
 * IAMROOT, 2023.03.15:
 * --- chat gpt ----
 * - cpu_idle_force_poll
 *  Linux 커널에서 cpu_idle_force_poll은 CPU가 유휴 상태일 때 깊은 유휴 
 *  상태가 아닌 폴링 모드로 전환해야 하는지 여부를 결정하는 플래그입니다.
 *
 *  CPU가 유휴 상태일 때 커널은 에너지를 절약하고 열을 줄이는 저전력 상태로
 *  전환하도록 선택할 수 있습니다. 그러나 일부 CPU는 깊은 유휴 상태를
 *  지원하지 않거나 이러한 상태를 자주 시작하고 종료할 때 성능이 저하될 수
 *  있습니다. 이러한 경우 커널은 폴링 모드에 들어갈 수 있습니다. 이 모드에서는
 *  CPU가 깊은 유휴 상태에 들어가는 대신 인터럽트가 도착하기를 기다리는
 *  루프에서 회전합니다.
 *
 *  cpu_idle_force_poll 플래그는 커널이 CPU가 깊은 유휴 상태로 들어가는 데
 *  문제가 있음을 감지하면 true로 설정됩니다. 이는 CPU가 자주 중단되는 경우,
 *  짧은 대기 시간이 필요한 작업을 실행하는 경우 또는 CPU가 깊은 유휴 상태를
 *  지원하지 않는 경우에 발생할 수 있습니다. 이 플래그가 설정되면 커널은 깊은
 *  유휴 상태 대신 폴링 모드로 들어가 성능을 개선하고 에너지 소비를 줄이는 데
 *  도움이 될 수 있습니다.
 *
 *  그러나 폴링 모드는 CPU가 실제로 유휴 상태가 아니며 여전히 에너지를
 *  소비하기 때문에 상당한 양의 전력을 소비할 수 있다는 점에 유의해야 합니다.
 *  또한 커널은 전원을 절약하기 위해 오랜 시간 동안 인터럽트가 없으면 폴링
 *  모드를 종료하고 깊은 유휴 상태로 들어갈 수 있습니다. 따라서 
 *  cpu_idle_force_poll의 사용은 깊은 유휴 상태의 잠재적인 에너지 절약과
 *  균형을 이루어야 합니다. 
 * -----------------
 * - != 0
 *   deep idle에 진입하지 말아야되는 상황.
 */
static int __read_mostly cpu_idle_force_poll;

/*
 * IAMROOT, 2023.03.18:
 * - cpuidle을 poll force 하기 위한 enalbe / disable
 */
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
/*
 * IAMROOT, 2023.03.18:
 * - cpuidle을 poll force 시킨다.
 */
static int __init cpu_idle_poll_setup(char *__unused)
{
	cpu_idle_force_poll = 1;

	return 1;
}
__setup("nohlt", cpu_idle_poll_setup);

/*
 * IAMROOT, 2023.03.18:
 * - cpuidle의 poll force를 disable 한다.
 */
static int __init cpu_idle_nopoll_setup(char *__unused)
{
	cpu_idle_force_poll = 0;

	return 1;
}
__setup("hlt", cpu_idle_nopoll_setup);
#endif

/*
 * IAMROOT, 2023.03.11:
 * - cpu 전원을 꺼지 않고 poll에 진입을 했어야되는 원인이 해결될때까지
 *   (reschedule 요청, force poll 상황, ipi 예상)계속 polling한다.
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

/*
 * IAMROOT, 2023.03.18:
 * - idle을 한다해도 deep sleep 하지 않기 위한 방식. 
 */
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
/*
 * IAMROOT, 2023.03.16:
 * - reschedule 요청이 있면 irq enable후 빠져나간다.
 */
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
/*
 * IAMROOT, 2023.03.16:
 * - papago
 *   arch_cpu_idle()은 IRQ를 활성화해야 하지만 RCU 및 추적
 *   때문에 그렇게 할 수 없습니다.
 *
 *   여기에서 추적 IRQ를 활성화한 다음 RCU를 끄고 arch_cpu_idle()이
 *   raw_local_irq_enable()을 사용하도록 합니다. rcu_idle_enter()는
 *   lockdep IRQ 상태에 의존하므로 마지막으로 전환합니다.
 *   이것은 진입 코드와 매우 유사합니다.
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
/*
 * IAMROOT, 2023.03.16:
 * - papago.
 *   좋아요, IRQ는 여기서 활성화되지만, RCU는 IRQ를 다시 켜려면
 *   비활성화해야 합니다.. 재미있는 것은 IRQ를 비활성화하면 추적이
 *   발생하여 RCU가 필요하다는 것입니다. '작동'하려면 후프를
 *   통과하십시오.
 * ---- chat openai -----
 * - hoops의 의미.
 *   주석의 맥락에서 "후프스"는 "장애물" 또는 "어려움"을 의미하는
 *   은유적 용어입니다. 주석은 RCU에 대한 인터럽트를 비활성화해야
 *   할 필요성, 추적을 위해 인터럽트를 활성화해야 할 필요성,
 *   인터럽트를 비활성화하면 추적이 발생한다는 사실 사이에 복잡한
 *   상호 작용이 있다고 설명하고 있습니다. "jump through hoops"라는
 *   문구는 이러한 장애물을 탐색하고 모든 것이 올바르게 작동하도록
 *   하기 위해 취해야 하는 몇 가지 단계 또는 해결 방법이 있음을
 *   의미합니다. 
 * ----------------------
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

/*
 * IAMROOT, 2023.03.17:
 * - reschedule 요청있으면 return busy.
 *   그게 아니면 deep sleep을 수행한다.
 */
static int call_cpuidle_s2idle(struct cpuidle_driver *drv,
			       struct cpuidle_device *dev)
{
	if (current_clr_polling_and_test())
		return -EBUSY;

	return cpuidle_enter_s2idle(drv, dev);
}

/*
 * IAMROOT, 2023.03.18:
 * - reschedule 요청이 있으면 잠들지 않고 return busy.
 *   @next_state해 해당하는 idle을 수행한다.
 */
static int call_cpuidle(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		      int next_state)
{
	/*
	 * The idle task must be scheduled, it is pointless to go to idle, just
	 * update no idle residency and return.
	 */
/*
 * IAMROOT, 2023.03.18:
 * - reschedule 요청이 있으면 return busy
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

/*
 * IAMROOT, 2023.03.18:
 * - papago
 *   governor 결정에 의해 이전에 반환된 유휴 상태를 입력합니다.
 *   이 기능은 인터럽트가 발생할 때까지 차단되며 로컬 인터럽트를 다시
 *   활성화합니다. 
 *
 * - @next_state해 해당하는 idle을 수행한다.
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
 * - devicetree/bindings/arm/idle-states.yaml 참고
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
 * - 1. resched 요청이 있으면 irq enable후 return.
 *   2. cpuidle driver가 없으면 default idle(wfi) 수행후 return.
 *   3. suspend-to-idle상황인 경우 s2idle을 지원하는 cpuidle state를 선택해서
 *      idle 수행.
 *   4. forced_idle_latency_limit_ns가 있는 경우 이 값을 max로 하여 cpuidle state
 *      를 선택해서 idle 수행.
 *   5. 일반적인 경우(윗 상황 들이 아닌 경우) curr governor를 통해서 cpuidle state
 *      를 찾아서 idle 수행.
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
	 * - cpuidle이 활성화 안됫거나 driver가 없는경우 wfi만 한번하고 끝낸다.
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
	 *
	 * - cpuidle 상태를 선택해야되는 상황이다. s2idle enter 중이거나
	 *   forced_idle_latency_limit_ns가 있다면.
	 *   1. s2idle enter 상황이면 s2idle일때의 cpuidle call을 하는 방법을 택해본다.
	 *      만약 실패했다면 max_latency_ns을 max로 설정해 무조건 최소값만 선택되게 한다.
	 *   2. forced_idle_latency_limit_ns이 있다면 그 값으로 max를 설정한다.
	 *   그후 선택된 값으로 deepest state를 고르고 해당 state로 cpu idle을 설정한다.
	 */
	if (idle_should_enter_s2idle() || dev->forced_idle_latency_limit_ns) {
		u64 max_latency_ns;

		if (idle_should_enter_s2idle()) {

/*
 * IAMROOT, 2023.03.18:
 * - deep sleep을 수행한다. 수행이 됬으면 goto exit_idle.
 *   그게 아니면 최대 sleep으로 cpuidle state를 검색한다.
 */
			entered_state = call_cpuidle_s2idle(drv, dev);
			if (entered_state > 0)
				goto exit_idle;

			max_latency_ns = U64_MAX;
		} else {

/*
 * IAMROOT, 2023.03.18:
 * - forced_idle_latency_limit_ns이하의 cpuidle state로 검색한다.
 */
			max_latency_ns = dev->forced_idle_latency_limit_ns;
		}

		tick_nohz_idle_stop_tick();

/*
 * IAMROOT, 2023.03.18:
 * - max_latency_ns이내의 c state를 골라서 idle을 수행한다.
 */
		next_state = cpuidle_find_deepest_state(drv, dev, max_latency_ns);
		call_cpuidle(drv, dev, next_state);
	} else {
		bool stop_tick = true;

		/*
		 * Ask the cpuidle framework to choose a convenient idle state.
		 */

/*
 * IAMROOT, 2023.03.18:
 * - curr governor에서 c state를 구해온다. 만약 tick을 멈추지 말아야되면 stop_tick
 *   이 false로 update되있을 것이다.
 */
		next_state = cpuidle_select(drv, dev, &stop_tick);

		if (stop_tick || tick_nohz_tick_stopped())
			tick_nohz_idle_stop_tick();
		else
			tick_nohz_idle_retain_tick();

/*
 * IAMROOT, 2023.03.18:
 * - governor에서 선택된 state로 cpuidle을 수행한다.
 */
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
 *
 * - 0. resched 요청이 있으면 idle에 진입안하고 자발적 schedule을 수행하러간다.,
 *   1. cpu가 offline이라면 cpu die로 진입한다.
 *   2. polling을 해야된다면 cpu_relax()를 polling을 해야되는 원인이 풀릴때까지 
 *      수행한다.
 *   3. 그게 아니면 cpuidle driver를 사용해서 idle을 수행한다.
 *   4. idle이 끝난후 pending된것들을 처리한다.
 *   5. 그 후 자발적 reschedule한다.
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
/*
 * IAMROOT, 2023.03.15:
 * - idle mode 진입 및 idle tick active 됨을 표시한다.
 */
	tick_nohz_idle_enter();

	while (!need_resched()) {
		rmb();

		local_irq_disable();

/*
 * IAMROOT, 2023.03.15:
 * - cpu가 offline이면 cpu die로 진입한다.
 */
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
		 * - 1. cpu_idle_force_poll != 0
		 *     deep idle에 진입하지 말아야되는 상황
		 *   2. tick_check_broadcast_expired() != 0
		 *     ipi가 this cpu에 올거라고 예상되는 상황
		 *   즉 deep sleep을 안해야되는 상황에서 poll로 idle을 돈다.
		 */
		if (cpu_idle_force_poll || tick_check_broadcast_expired()) {
			tick_nohz_idle_restart_tick();
			cpu_idle_poll();
/*
 * IAMROOT, 2023.03.18:
 * - poll이 아니면 cpuidle state를 선택해 idle을 수행한다.
 */
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
/*
 * IAMROOT, 2023.03.16:
 * - papago
 *   우리는 위의 루프에서 벗어났기 때문에 TIF_NEED_RESCHED가 설정되어야 한다는
 *   것을 알고 이를 PREEMPT_NEED_RESCHED로 전파합니다.
 *
 *   이는 유휴 루프를 폴링하기 위해 상태를 접을 IPI가 없기 때문에 필요합니다.
 *
 * - idle이 끝나서 resched을 한번 한다.
 */
	preempt_set_need_resched();
	tick_nohz_idle_exit();
	__current_clr_polling();

	/*
	 * We promise to call sched_ttwu_pending() and reschedule if
	 * need_resched() is set while polling is set. That means that clearing
	 * polling needs to be visible before doing these things.
	 */
/*
 * IAMROOT, 2023.03.18:
 * - papago
 *   폴링이 설정되어 있는 동안 need_resched()가 설정되면 sched_ttwu_pending()을 
 *   호출하고 일정을 변경할 것을 약속합니다. 이는 이러한 작업을 수행하기 전에 폴링 
 *   지우기가 표시되어야 함을 의미합니다.
 */
	smp_mb__after_atomic();

	/*
	 * RCU relies on this call to be done outside of an RCU read-side
	 * critical section.
	 */
/*
 * IAMROOT, 2023.03.18:
 * - papgo 
 *   RCU는 RCU 읽기측 중요 섹션 외부에서 수행되는 이 호출에 의존합니다.
 * - 여러 pending에 대해(softirq등) 처리한다.
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
 * - idle로 진입전 online이 되기 위한 @state처리를 하고 idle을 수행하며
 *   resched을 기다린다.
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

/*
 * IAMROOT, 2023.03.18:
 * - start_kernel()->rest_init()->kernel_init thread생성
 *   kernel_init -> kernel_init_freeable() -> smp_init()-> 
 *   idle_threads_init()->idle_init() - cpu for돌면서 fork_idle()->init_idle()에서
 *   idle이 설정됬다. 즉 booting cpu에 의해 각 cpu마다 idle task가 한개씩 생성된다.
 */
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
