/*
 * cpuidle.c - core cpuidle infrastructure
 *
 * (C) 2006-2007 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *               Shaohua Li <shaohua.li@intel.com>
 *               Adam Belay <abelay@novell.com>
 *
 * This code is licenced under the GPL.
 */

#include <linux/clockchips.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/tick.h>
#include <linux/mmu_context.h>
#include <trace/events/power.h>

#include "cpuidle.h"

DEFINE_PER_CPU(struct cpuidle_device *, cpuidle_devices);
DEFINE_PER_CPU(struct cpuidle_device, cpuidle_dev);

DEFINE_MUTEX(cpuidle_lock);
LIST_HEAD(cpuidle_detected_devices);

static int enabled_devices;
static int off __read_mostly;
static int initialized __read_mostly;

int cpuidle_disabled(void)
{
	return off;
}
void disable_cpuidle(void)
{
	off = 1;
}

bool cpuidle_not_available(struct cpuidle_driver *drv,
			   struct cpuidle_device *dev)
{
	return off || !initialized || !drv || !dev || !dev->enabled;
}

/**
 * cpuidle_play_dead - cpu off-lining
 *
 * Returns in case of an error or no driver
 */
int cpuidle_play_dead(void)
{
	struct cpuidle_device *dev = __this_cpu_read(cpuidle_devices);
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	int i;

	if (!drv)
		return -ENODEV;

	/* Find lowest-power state that supports long-term idle */
	for (i = drv->state_count - 1; i >= 0; i--)
		if (drv->states[i].enter_dead)
			return drv->states[i].enter_dead(dev, i);

	return -ENODEV;
}

/*
 * IAMROOT, 2023.03.18:
 * @forbidden_flags 해당 flag가 지정되면 건너 뛴다.
 * @return 0    : 조건에 맞는 C state 못찾음
 *         != 0 : 조건에 맞는 C state index.
 *
 * - 인자 조건에 맞는 최대 exit_latency_ns를 구해서 해당 C state index를
 *   return 한다.
 *   max_latency_ns값이 U64_MAX가 아니라면 < max_latency_ns의 이내의 범위에서
 *   최대 값을 찾는다.
 * - s2idle이 true라면 s2idle에서 사용할수있는 C state를 찾는다.
 */
static int find_deepest_state(struct cpuidle_driver *drv,
			      struct cpuidle_device *dev,
			      u64 max_latency_ns,
			      unsigned int forbidden_flags,
			      bool s2idle)
{
	u64 latency_req = 0;
	int i, ret = 0;

	for (i = 1; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];

		if (dev->states_usage[i].disable ||
		    s->exit_latency_ns <= latency_req ||
		    s->exit_latency_ns > max_latency_ns ||
		    (s->flags & forbidden_flags) ||
		    (s2idle && !s->enter_s2idle))
			continue;

		latency_req = s->exit_latency_ns;
		ret = i;
	}
	return ret;
}

/**
 * cpuidle_use_deepest_state - Set/unset governor override mode.
 * @latency_limit_ns: Idle state exit latency limit (or no override if 0).
 *
 * If @latency_limit_ns is nonzero, set the current CPU to use the deepest idle
 * state with exit latency within @latency_limit_ns (override governors going
 * forward), or do not override governors if it is zero.
 */
/*
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   cpuidle_use_deepest_state - 거버너 오버라이드 모드를 설정/해제합니다.
 *   @latency_limit_ns: 유휴 상태 종료 대기 시간 제한(또는 0인 경우 재정의 없음).
 *
 *   @latency_limit_ns가 0이 아닌 경우 @latency_limit_ns 내에서 종료
 *   대기 시간이 있는 가장 깊은 유휴 상태를 사용하도록 현재 CPU를 설정하거나(앞으로
 *   거버너 재정의) 0인 경우 거버너를 재정의하지 마십시오.
 */
void cpuidle_use_deepest_state(u64 latency_limit_ns)
{
	struct cpuidle_device *dev;

	preempt_disable();
	dev = cpuidle_get_device();
	if (dev)
		dev->forced_idle_latency_limit_ns = latency_limit_ns;
	preempt_enable();
}

/**
 * cpuidle_find_deepest_state - Find the deepest available idle state.
 * @drv: cpuidle driver for the given CPU.
 * @dev: cpuidle device for the given CPU.
 * @latency_limit_ns: Idle state exit latency limit
 *
 * Return: the index of the deepest available idle state.
 */
/*
 * IAMROOT, 2023.03.18:
 * @return 0    : 조건에 맞는 C state 못찾음
 *         != 0 : 조건에 맞는 C state index.
 * - @latency_limit_ns 이내의 C state idx를 구한다.
 */
int cpuidle_find_deepest_state(struct cpuidle_driver *drv,
			       struct cpuidle_device *dev,
			       u64 latency_limit_ns)
{
	return find_deepest_state(drv, dev, latency_limit_ns, 0, false);
}

#ifdef CONFIG_SUSPEND
/*
 * IAMROOT, 2023.03.18:
 * - @index의 cpudile state를 가져와 enter_s2idle(deep sleep)을 수행한다.
 * - deel sleep전후로 sched timer를 off / on(or system suspend, resume)한다.
 * - 깨어난 이후에는 deep sleep의 기간누적시키고 발생 빈도를 증가시킨다.
 */
static void enter_s2idle_proper(struct cpuidle_driver *drv,
				struct cpuidle_device *dev, int index)
{
	ktime_t time_start, time_end;
	struct cpuidle_state *target_state = &drv->states[index];

	time_start = ns_to_ktime(local_clock());

	tick_freeze();
	/*
	 * The state used here cannot be a "coupled" one, because the "coupled"
	 * cpuidle mechanism enables interrupts and doing that with timekeeping
	 * suspended is generally unsafe.
	 */
	stop_critical_timings();
	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE))
		rcu_idle_enter();
/*
 * IAMROOT, 2023.03.18:
 * - 여기서 deepsleep에 들어간다.
 */
	target_state->enter_s2idle(dev, drv, index);
	if (WARN_ON_ONCE(!irqs_disabled()))
		local_irq_disable();
	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE))
		rcu_idle_exit();
	tick_unfreeze();
	start_critical_timings();

	time_end = ns_to_ktime(local_clock());

	dev->states_usage[index].s2idle_time += ktime_us_delta(time_end, time_start);
	dev->states_usage[index].s2idle_usage++;
}

/**
 * cpuidle_enter_s2idle - Enter an idle state suitable for suspend-to-idle.
 * @drv: cpuidle driver for the given CPU.
 * @dev: cpuidle device for the given CPU.
 *
 * If there are states with the ->enter_s2idle callback, find the deepest of
 * them and enter it with frozen tick.
 */
/*
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   cpuidle_enter_s2idle - 유휴 일시 중단에 적합한 유휴 상태를 입력합니다.
 *   @drv: 주어진 CPU에 대한 cpuidle 드라이버.
 *   @dev: 주어진 CPU에 대한 cpuidle장치.
 *
 *   ->enter_s2idle 콜백이 있는 상태가 있는 경우 가장 깊은 상태를 찾아 고정된
 *   틱으로 입력합니다.
 *
 * - @return 0    : s2idle로 동작하는 C state를 못찾음.
 *   @return != 0 : s2idle로 동작하는 C state중에서 최대 latency ms state.
 * - s2idle로 동작할 수 있는 최대 latency ms를 가진 C state찾는다.
 *   찾은 경우 deep sleep을 하고 마친 후에는 irq를 enable한다.
 */
int cpuidle_enter_s2idle(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	int index;

	/*
	 * Find the deepest state with ->enter_s2idle present, which guarantees
	 * that interrupts won't be enabled when it exits and allows the tick to
	 * be frozen safely.
	 */
	/*
	 * IAMROOT. 2023.03.11:
	 * - google-translate
	 *   ->enter_s2idle이 존재하는 가장 깊은 상태를 찾으십시오. 이는 종료될 때
	 *   인터럽트가 활성화되지 않고 틱이 안전하게 동결되도록 합니다.
	 */
	index = find_deepest_state(drv, dev, U64_MAX, 0, true);
	if (index > 0) {
		enter_s2idle_proper(drv, dev, index);
		local_irq_enable();
	}
	return index;
}
#endif /* CONFIG_SUSPEND */

/**
 * cpuidle_enter_state - enter the state and update stats
 * @dev: cpuidle device for this cpu
 * @drv: cpuidle driver for this cpu
 * @index: index into the states table in @drv of the state to enter
 */
/*
 * IAMROOT, 2023.03.18:
 * - c state @index에 해당하는 idle을 수행하고 통계를 계산한다.
 * - broadcast인 경우 wakeup oneshot device에 맡기는걸 시도하여
 *   CPUIDLE_FLAG_TIMER_STOP이 없는 c state index를 새로 찾아본다.
 */
int cpuidle_enter_state(struct cpuidle_device *dev, struct cpuidle_driver *drv,
			int index)
{
	int entered_state;

	struct cpuidle_state *target_state = &drv->states[index];
	bool broadcast = !!(target_state->flags & CPUIDLE_FLAG_TIMER_STOP);
	ktime_t time_start, time_end;

	/*
	 * Tell the time framework to switch to a broadcast timer because our
	 * local timer will be shut down.  If a local timer is used from another
	 * CPU as a broadcast timer, this call may fail if it is not available.
	 */
/*
 * IAMROOT, 2023.03.18:
 * - papago
 *   로컬 타이머가 종료되므로 브로드캐스트 타이머로 전환하도록 시간
 *   프레임워크에 알립니다. 로컬 타이머가 다른 CPU에서 브로드캐스트
 *   타이머로 사용되는 경우 이 호출을 사용할 수 없으면 이 호출이 실패할 
 *   수 있습니다.
 *
 * - broadcast라면, timer stop을 하고 oneshot wakeup device or
 *   broadcast devie에 this cpu를 next_event에 깨우도록 요청한다.
 */
	if (broadcast && tick_broadcast_enter()) {
/*
 * IAMROOT, 2023.03.18:
 * - CPUIDLE_FLAG_TIMER_STOP가 제외된 exit_latency_ns이내의 c state를 찾는다.
 */
		index = find_deepest_state(drv, dev, target_state->exit_latency_ns,
					   CPUIDLE_FLAG_TIMER_STOP, false);
/*
 * IAMROOT, 2023.03.18:
 * - 못찾앗으면 default idle로 동작하고 끝낸다.
 */
		if (index < 0) {
			default_idle_call();
			return -EBUSY;
		}
		target_state = &drv->states[index];
		broadcast = false;
	}

	if (target_state->flags & CPUIDLE_FLAG_TLB_FLUSHED)
		leave_mm(dev->cpu);

	/* Take note of the planned idle state. */
	sched_idle_set_state(target_state);

	trace_cpu_idle(index, dev->cpu);
	time_start = ns_to_ktime(local_clock());

	stop_critical_timings();
	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE))
		rcu_idle_enter();

/*
 * IAMROOT, 2023.03.18:
 * - c state index에 따른 idle상태에 진입한다.
 */
	entered_state = target_state->enter(dev, drv, index);
	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE))
		rcu_idle_exit();
	start_critical_timings();

	sched_clock_idle_wakeup_event();
	time_end = ns_to_ktime(local_clock());
	trace_cpu_idle(PWR_EVENT_EXIT, dev->cpu);

	/* The cpu is no longer idle or about to enter idle. */

/*
 * IAMROOT, 2023.03.18:
 * - 이제 idle이 끝났으므로 null로 설정한다.
 */
	sched_idle_set_state(NULL);

/*
 * IAMROOT, 2023.03.18:
 * - broadcast였지만 위에서 전환이 실패한경우.
 */
	if (broadcast) {
		if (WARN_ON_ONCE(!irqs_disabled()))
			local_irq_disable();

		tick_broadcast_exit();
	}

	if (!cpuidle_state_is_coupled(drv, index))
		local_irq_enable();

/*
 * IAMROOT, 2023.03.18:
 * - idle이 끝난후, idle이 동장했던 entered_state에 대한 통계처리를 진행한다.
 */
	if (entered_state >= 0) {
		s64 diff, delay = drv->states[entered_state].exit_latency_ns;
		int i;

		/*
		 * Update cpuidle counters
		 * This can be moved to within driver enter routine,
		 * but that results in multiple copies of same code.
		 */
		diff = ktime_sub(time_end, time_start);

		dev->last_residency_ns = diff;
		dev->states_usage[entered_state].time_ns += diff;
		dev->states_usage[entered_state].usage++;

		if (diff < drv->states[entered_state].target_residency_ns) {
			for (i = entered_state - 1; i >= 0; i--) {
				if (dev->states_usage[i].disable)
					continue;

				/* Shallower states are enabled, so update. */
				dev->states_usage[entered_state].above++;
				break;
			}
		} else if (diff > delay) {
			for (i = entered_state + 1; i < drv->state_count; i++) {
				if (dev->states_usage[i].disable)
					continue;

				/*
				 * Update if a deeper state would have been a
				 * better match for the observed idle duration.
				 */
				if (diff - delay >= drv->states[i].target_residency_ns)
					dev->states_usage[entered_state].below++;

				break;
			}
		}
	} else {
/*
 * IAMROOT, 2023.03.18:
 * - 실패 통계 처리
 */
		dev->last_residency_ns = 0;
		dev->states_usage[index].rejected++;
	}

	return entered_state;
}

/**
 * cpuidle_select - ask the cpuidle framework to choose an idle state
 *
 * @drv: the cpuidle driver
 * @dev: the cpuidle device
 * @stop_tick: indication on whether or not to stop the tick
 *
 * Returns the index of the idle state.  The return value must not be negative.
 *
 * The memory location pointed to by @stop_tick is expected to be written the
 * 'false' boolean value if the scheduler tick should not be stopped before
 * entering the returned state.
 */
/*
 * IAMROOT, 2023.03.18:
 * - papago
 *   cpuidle_select - 유휴 상태를 선택하도록 cpuidle 프레임워크에 요청합니다.
 *   @drv: cpuidle 드라이버.
 *   @dev: cpuidle 장치.
 *   @stop_tick: 틱 중지 여부 표시.
 *
 *   유휴 상태의 인덱스를 반환합니다. 반환 값은 음수가 아니어야 합니다.
 *
 *   반환된 상태에 들어가기 전에 스케줄러 틱이 중지되지 않아야 하는 경우
 *   @stop_tick이 가리키는 메모리 위치는 'false' 부울 값으로 기록될 것으로 예상됩니다.
 *
 * - curr governor에서 cpuidle을 고른다. 주석에 따르면 schedule tick이 중지되지
 *   않아야되는 경우 stop_tick은 false로 업데이트 된다.
 * - curr governor에서 cpuidle state를 골라서 return한다.
 * - ex) menu_select
 */
int cpuidle_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		   bool *stop_tick)
{
	return cpuidle_curr_governor->select(drv, dev, stop_tick);
}

/**
 * cpuidle_enter - enter into the specified idle state
 *
 * @drv:   the cpuidle driver tied with the cpu
 * @dev:   the cpuidle device
 * @index: the index in the idle state table
 *
 * Returns the index in the idle state, < 0 in case of error.
 * The error code depends on the backend driver
 */
/*
 * IAMROOT, 2023.03.18:
 * - c state index해 당하는 idle을 수행한다.
 */
int cpuidle_enter(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		  int index)
{
	int ret = 0;

	/*
	 * Store the next hrtimer, which becomes either next tick or the next
	 * timer event, whatever expires first. Additionally, to make this data
	 * useful for consumers outside cpuidle, we rely on that the governor's
	 * ->select() callback have decided, whether to stop the tick or not.
	 */
/*
 * IAMROOT, 2023.03.18:
 * - papago
 *   먼저 만료되는 것이 무엇이든 다음 틱 또는 다음 타이머 이벤트가 되는 
 *   다음 hrtimer를 저장합니다. 또한 이 데이터를 cpuidle 외부의 
 *   소비자에게 유용하게 만들기 위해 우리는 거버너의 ->select() 콜백이 틱 
 *   중지 여부를 결정했는지에 의존합니다.
 *
 * - 가장 먼저 완료될 시간을 dev->next_hrtimer에 저장한다.
 *   idle을 중에 next_hrtimer가 oneshot wakeup device나 braodcast들에 의해 
 *   next_hrtimer에 this cpu가 깨어나어 처리하게 할 것이다.
 */
	WRITE_ONCE(dev->next_hrtimer, tick_nohz_get_next_hrtimer());

/*
 * IAMROOT, 2023.03.18:
 * - c state index에 해당하는 idle을 수행하고 통계를 작성한다.
 */
	if (cpuidle_state_is_coupled(drv, index))
		ret = cpuidle_enter_state_coupled(dev, drv, index);
	else
		ret = cpuidle_enter_state(dev, drv, index);

/*
 * IAMROOT, 2023.03.18:
 * - 깨어나면 next_timer을 초기화한다.
 */
	WRITE_ONCE(dev->next_hrtimer, 0);
	return ret;
}

/**
 * cpuidle_reflect - tell the underlying governor what was the state
 * we were in
 *
 * @dev  : the cpuidle device
 * @index: the index in the idle state table
 *
 */
/*
 * IAMROOT, 2023.03.18:
 * - curr governor에서 reflect callback을 호출한다.
 *   idle 끝난 직후 기록 및 처리.
 *   ex) menu_reflect
 */
void cpuidle_reflect(struct cpuidle_device *dev, int index)
{
	if (cpuidle_curr_governor->reflect && index >= 0)
		cpuidle_curr_governor->reflect(dev, index);
}

/*
 * Min polling interval of 10usec is a guess. It is assuming that
 * for most users, the time for a single ping-pong workload like
 * perf bench pipe would generally complete within 10usec but
 * this is hardware dependant. Actual time can be estimated with
 *
 * perf bench sched pipe -l 10000
 *
 * Run multiple times to avoid cpufreq effects.
 */
#define CPUIDLE_POLL_MIN 10000
#define CPUIDLE_POLL_MAX (TICK_NSEC / 16)

/**
 * cpuidle_poll_time - return amount of time to poll for,
 * governors can override dev->poll_limit_ns if necessary
 *
 * @drv:   the cpuidle driver tied with the cpu
 * @dev:   the cpuidle device
 *
 */
u64 cpuidle_poll_time(struct cpuidle_driver *drv,
		      struct cpuidle_device *dev)
{
	int i;
	u64 limit_ns;

	BUILD_BUG_ON(CPUIDLE_POLL_MIN > CPUIDLE_POLL_MAX);

	if (dev->poll_limit_ns)
		return dev->poll_limit_ns;

	limit_ns = CPUIDLE_POLL_MAX;
	for (i = 1; i < drv->state_count; i++) {
		u64 state_limit;

		if (dev->states_usage[i].disable)
			continue;

		state_limit = drv->states[i].target_residency_ns;
		if (state_limit < CPUIDLE_POLL_MIN)
			continue;

		limit_ns = min_t(u64, state_limit, CPUIDLE_POLL_MAX);
		break;
	}

	dev->poll_limit_ns = limit_ns;

	return dev->poll_limit_ns;
}

/**
 * cpuidle_install_idle_handler - installs the cpuidle idle loop handler
 */
void cpuidle_install_idle_handler(void)
{
	if (enabled_devices) {
		/* Make sure all changes finished before we switch to new idle */
		smp_wmb();
		initialized = 1;
	}
}

/**
 * cpuidle_uninstall_idle_handler - uninstalls the cpuidle idle loop handler
 */
void cpuidle_uninstall_idle_handler(void)
{
	if (enabled_devices) {
		initialized = 0;
		wake_up_all_idle_cpus();
	}

	/*
	 * Make sure external observers (such as the scheduler)
	 * are done looking at pointed idle states.
	 */
	synchronize_rcu();
}

/**
 * cpuidle_pause_and_lock - temporarily disables CPUIDLE
 */
void cpuidle_pause_and_lock(void)
{
	mutex_lock(&cpuidle_lock);
	cpuidle_uninstall_idle_handler();
}

EXPORT_SYMBOL_GPL(cpuidle_pause_and_lock);

/**
 * cpuidle_resume_and_unlock - resumes CPUIDLE operation
 */
void cpuidle_resume_and_unlock(void)
{
	cpuidle_install_idle_handler();
	mutex_unlock(&cpuidle_lock);
}

EXPORT_SYMBOL_GPL(cpuidle_resume_and_unlock);

/* Currently used in suspend/resume path to suspend cpuidle */
void cpuidle_pause(void)
{
	mutex_lock(&cpuidle_lock);
	cpuidle_uninstall_idle_handler();
	mutex_unlock(&cpuidle_lock);
}

/* Currently used in suspend/resume path to resume cpuidle */
void cpuidle_resume(void)
{
	mutex_lock(&cpuidle_lock);
	cpuidle_install_idle_handler();
	mutex_unlock(&cpuidle_lock);
}

/**
 * cpuidle_enable_device - enables idle PM for a CPU
 * @dev: the CPU
 *
 * This function must be called between cpuidle_pause_and_lock and
 * cpuidle_resume_and_unlock when used externally.
 */
int cpuidle_enable_device(struct cpuidle_device *dev)
{
	int ret;
	struct cpuidle_driver *drv;

	if (!dev)
		return -EINVAL;

	if (dev->enabled)
		return 0;

	if (!cpuidle_curr_governor)
		return -EIO;

	drv = cpuidle_get_cpu_driver(dev);

	if (!drv)
		return -EIO;

	if (!dev->registered)
		return -EINVAL;

	ret = cpuidle_add_device_sysfs(dev);
	if (ret)
		return ret;

	if (cpuidle_curr_governor->enable) {
		ret = cpuidle_curr_governor->enable(drv, dev);
		if (ret)
			goto fail_sysfs;
	}

	smp_wmb();

	dev->enabled = 1;

	enabled_devices++;
	return 0;

fail_sysfs:
	cpuidle_remove_device_sysfs(dev);

	return ret;
}

EXPORT_SYMBOL_GPL(cpuidle_enable_device);

/**
 * cpuidle_disable_device - disables idle PM for a CPU
 * @dev: the CPU
 *
 * This function must be called between cpuidle_pause_and_lock and
 * cpuidle_resume_and_unlock when used externally.
 */
void cpuidle_disable_device(struct cpuidle_device *dev)
{
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);

	if (!dev || !dev->enabled)
		return;

	if (!drv || !cpuidle_curr_governor)
		return;

	dev->enabled = 0;

	if (cpuidle_curr_governor->disable)
		cpuidle_curr_governor->disable(drv, dev);

	cpuidle_remove_device_sysfs(dev);
	enabled_devices--;
}

EXPORT_SYMBOL_GPL(cpuidle_disable_device);

static void __cpuidle_unregister_device(struct cpuidle_device *dev)
{
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);

	list_del(&dev->device_list);
	per_cpu(cpuidle_devices, dev->cpu) = NULL;
	module_put(drv->owner);

	dev->registered = 0;
}

static void __cpuidle_device_init(struct cpuidle_device *dev)
{
	memset(dev->states_usage, 0, sizeof(dev->states_usage));
	dev->last_residency_ns = 0;
	dev->next_hrtimer = 0;
}

/**
 * __cpuidle_register_device - internal register function called before register
 * and enable routines
 * @dev: the cpu
 *
 * cpuidle_lock mutex must be held before this is called
 */
static int __cpuidle_register_device(struct cpuidle_device *dev)
{
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	int i, ret;

	if (!try_module_get(drv->owner))
		return -EINVAL;

	for (i = 0; i < drv->state_count; i++) {
		if (drv->states[i].flags & CPUIDLE_FLAG_UNUSABLE)
			dev->states_usage[i].disable |= CPUIDLE_STATE_DISABLED_BY_DRIVER;

		if (drv->states[i].flags & CPUIDLE_FLAG_OFF)
			dev->states_usage[i].disable |= CPUIDLE_STATE_DISABLED_BY_USER;
	}

	per_cpu(cpuidle_devices, dev->cpu) = dev;
	list_add(&dev->device_list, &cpuidle_detected_devices);

	ret = cpuidle_coupled_register_device(dev);
	if (ret)
		__cpuidle_unregister_device(dev);
	else
		dev->registered = 1;

	return ret;
}

/**
 * cpuidle_register_device - registers a CPU's idle PM feature
 * @dev: the cpu
 */
int cpuidle_register_device(struct cpuidle_device *dev)
{
	int ret = -EBUSY;

	if (!dev)
		return -EINVAL;

	mutex_lock(&cpuidle_lock);

	if (dev->registered)
		goto out_unlock;

	__cpuidle_device_init(dev);

	ret = __cpuidle_register_device(dev);
	if (ret)
		goto out_unlock;

	ret = cpuidle_add_sysfs(dev);
	if (ret)
		goto out_unregister;

	ret = cpuidle_enable_device(dev);
	if (ret)
		goto out_sysfs;

	cpuidle_install_idle_handler();

out_unlock:
	mutex_unlock(&cpuidle_lock);

	return ret;

out_sysfs:
	cpuidle_remove_sysfs(dev);
out_unregister:
	__cpuidle_unregister_device(dev);
	goto out_unlock;
}

EXPORT_SYMBOL_GPL(cpuidle_register_device);

/**
 * cpuidle_unregister_device - unregisters a CPU's idle PM feature
 * @dev: the cpu
 */
void cpuidle_unregister_device(struct cpuidle_device *dev)
{
	if (!dev || dev->registered == 0)
		return;

	cpuidle_pause_and_lock();

	cpuidle_disable_device(dev);

	cpuidle_remove_sysfs(dev);

	__cpuidle_unregister_device(dev);

	cpuidle_coupled_unregister_device(dev);

	cpuidle_resume_and_unlock();
}

EXPORT_SYMBOL_GPL(cpuidle_unregister_device);

/**
 * cpuidle_unregister: unregister a driver and the devices. This function
 * can be used only if the driver has been previously registered through
 * the cpuidle_register function.
 *
 * @drv: a valid pointer to a struct cpuidle_driver
 */
void cpuidle_unregister(struct cpuidle_driver *drv)
{
	int cpu;
	struct cpuidle_device *device;

	for_each_cpu(cpu, drv->cpumask) {
		device = &per_cpu(cpuidle_dev, cpu);
		cpuidle_unregister_device(device);
	}

	cpuidle_unregister_driver(drv);
}
EXPORT_SYMBOL_GPL(cpuidle_unregister);

/**
 * cpuidle_register: registers the driver and the cpu devices with the
 * coupled_cpus passed as parameter. This function is used for all common
 * initialization pattern there are in the arch specific drivers. The
 * devices is globally defined in this file.
 *
 * @drv         : a valid pointer to a struct cpuidle_driver
 * @coupled_cpus: a cpumask for the coupled states
 *
 * Returns 0 on success, < 0 otherwise
 */
int cpuidle_register(struct cpuidle_driver *drv,
		     const struct cpumask *const coupled_cpus)
{
	int ret, cpu;
	struct cpuidle_device *device;

	ret = cpuidle_register_driver(drv);
	if (ret) {
		pr_err("failed to register cpuidle driver\n");
		return ret;
	}

	for_each_cpu(cpu, drv->cpumask) {
		device = &per_cpu(cpuidle_dev, cpu);
		device->cpu = cpu;

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
		/*
		 * On multiplatform for ARM, the coupled idle states could be
		 * enabled in the kernel even if the cpuidle driver does not
		 * use it. Note, coupled_cpus is a struct copy.
		 */
		if (coupled_cpus)
			device->coupled_cpus = *coupled_cpus;
#endif
		ret = cpuidle_register_device(device);
		if (!ret)
			continue;

		pr_err("Failed to register cpuidle device for cpu%d\n", cpu);

		cpuidle_unregister(drv);
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cpuidle_register);

/**
 * cpuidle_init - core initializer
 */
static int __init cpuidle_init(void)
{
	if (cpuidle_disabled())
		return -ENODEV;

	return cpuidle_add_interface(cpu_subsys.dev_root);
}

module_param(off, int, 0444);
module_param_string(governor, param_governor, CPUIDLE_NAME_LEN, 0444);
core_initcall(cpuidle_init);
