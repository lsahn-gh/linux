// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains functions which emulate a local clock-event
 * device via a broadcast event source.
 *
 * Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/module.h>

#include "tick-internal.h"

/*
 * Broadcast support for broken x86 hardware, where the local apic
 * timer stops in C3 state.
 */

static struct tick_device tick_broadcast_device;
static cpumask_var_t tick_broadcast_mask __cpumask_var_read_mostly;
static cpumask_var_t tick_broadcast_on __cpumask_var_read_mostly;
static cpumask_var_t tmpmask __cpumask_var_read_mostly;
static int tick_broadcast_forced;

static __cacheline_aligned_in_smp DEFINE_RAW_SPINLOCK(tick_broadcast_lock);

#ifdef CONFIG_TICK_ONESHOT
static DEFINE_PER_CPU(struct clock_event_device *, tick_oneshot_wakeup_device);

static void tick_broadcast_setup_oneshot(struct clock_event_device *bc);
static void tick_broadcast_clear_oneshot(int cpu);
static void tick_resume_broadcast_oneshot(struct clock_event_device *bc);
# ifdef CONFIG_HOTPLUG_CPU
static void tick_broadcast_oneshot_offline(unsigned int cpu);
# endif
#else
static inline void tick_broadcast_setup_oneshot(struct clock_event_device *bc) { BUG(); }
static inline void tick_broadcast_clear_oneshot(int cpu) { }
static inline void tick_resume_broadcast_oneshot(struct clock_event_device *bc) { }
# ifdef CONFIG_HOTPLUG_CPU
static inline void tick_broadcast_oneshot_offline(unsigned int cpu) { }
# endif
#endif

/*
 * Debugging: see timer_list.c
 */
struct tick_device *tick_get_broadcast_device(void)
{
	return &tick_broadcast_device;
}

struct cpumask *tick_get_broadcast_mask(void)
{
	return tick_broadcast_mask;
}

static struct clock_event_device *tick_get_oneshot_wakeup_device(int cpu);

const struct clock_event_device *tick_get_wakeup_device(int cpu)
{
	return tick_get_oneshot_wakeup_device(cpu);
}

/*
 * Start the device in periodic mode
 */
/*
 * IAMROOT, 2023.01.03:
 * - @bc의 event_handler를 tick_handle_periodic_broadcast로 설정한다.
 *   @bc가 동작중이면 fetures와 tick_broadcast_device mode에 따라
 *   @dev의 state를 periodic or oneshot으로 설정한다.
 *   oneshot일 경우 tick_next_period에 맞춰 expire 시간을 program한다.
 */
static void tick_broadcast_start_periodic(struct clock_event_device *bc)
{
	if (bc)
		tick_setup_periodic(bc, 1);
}

/*
 * Check, if the device can be utilized as broadcast device:
 */
/*
 * IAMROOT, 2023.01.03:
 * - broadcast로 사용할수있는지 검사한다.
 */
static bool tick_check_broadcast_device(struct clock_event_device *curdev,
					struct clock_event_device *newdev)
{
	if ((newdev->features & CLOCK_EVT_FEAT_DUMMY) ||
	    (newdev->features & CLOCK_EVT_FEAT_PERCPU) ||
	    (newdev->features & CLOCK_EVT_FEAT_C3STOP))
		return false;

/*
 * IAMROOT, 2023.01.03:
 * - broadcast이 oneshot 인데 @newdev가 oneshot이 아니면 false.
 */
	if (tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT &&
	    !(newdev->features & CLOCK_EVT_FEAT_ONESHOT))
		return false;

/*
 * IAMROOT, 2023.01.03:
 * - rating이 더 큰게 우선.
 */
	return !curdev || newdev->rating > curdev->rating;
}

#ifdef CONFIG_TICK_ONESHOT
/*
 * IAMROOT, 2023.01.03:
 * - @cpu에 해당하는 tick_oneshot_wakeup_device가져온다.
 */
static struct clock_event_device *tick_get_oneshot_wakeup_device(int cpu)
{
	return per_cpu(tick_oneshot_wakeup_device, cpu);
}

static void tick_oneshot_wakeup_handler(struct clock_event_device *wd)
{
	/*
	 * If we woke up early and the tick was reprogrammed in the
	 * meantime then this may be spurious but harmless.
	 */
	tick_receive_broadcast();
}

/*
 * IAMROOT, 2023.01.03:
 * - wakeup 으로 설정할지를 정한다.
 *   pcpu, oneshot이여야 하고, 기존 dev에 비해 rating이 작거나 같아야되는등의
 *   제약조건을 검사한다.
 */
static bool tick_set_oneshot_wakeup_device(struct clock_event_device *newdev,
					   int cpu)
{
	struct clock_event_device *curdev = tick_get_oneshot_wakeup_device(cpu);

	if (!newdev)
		goto set_device;

	if ((newdev->features & CLOCK_EVT_FEAT_DUMMY) ||
	    (newdev->features & CLOCK_EVT_FEAT_C3STOP))
		 return false;

	if (!(newdev->features & CLOCK_EVT_FEAT_PERCPU) ||
	    !(newdev->features & CLOCK_EVT_FEAT_ONESHOT))
		return false;

	if (!cpumask_equal(newdev->cpumask, cpumask_of(cpu)))
		return false;

	if (curdev && newdev->rating <= curdev->rating)
		return false;

	if (!try_module_get(newdev->owner))
		return false;

	newdev->event_handler = tick_oneshot_wakeup_handler;
set_device:
/*
 * IAMROOT, 2023.01.03:
 * - @curdev는 ref put시키고 @newdev는 set을 위해 일단 shutdown
 */
	clockevents_exchange_device(curdev, newdev);
/*
 * IAMROOT, 2023.01.03:
 * - @cpu의 pcp tick_oneshot_wakeup_device를 @newdev로 설정한다.
 */
	per_cpu(tick_oneshot_wakeup_device, cpu) = newdev;
	return true;
}
#else
static struct clock_event_device *tick_get_oneshot_wakeup_device(int cpu)
{
	return NULL;
}

static bool tick_set_oneshot_wakeup_device(struct clock_event_device *newdev,
					   int cpu)
{
	return false;
}
#endif

/*
 * Conditionally install/replace broadcast device
 */
/*
 * IAMROOT, 2022.12.03:
 * 1. wakeup으로 사용할지 결정한다.
 * 2. broadcast로 설정할지 결정한다. 
 *    @dev를 pcpu tick_broadcast_device에 등록한다.
 */
void tick_install_broadcast_device(struct clock_event_device *dev, int cpu)
{
	struct clock_event_device *cur = tick_broadcast_device.evtdev;

/*
 * IAMROOT, 2023.01.03:
 * - wakeup으로 설정할수있는경우 tick_oneshot_wakeup_handler를 설정하고
 *   빠져나간다.
 */
	if (tick_set_oneshot_wakeup_device(dev, cpu))
		return;

/*
 * IAMROOT, 2023.01.03:
 * - broadcast로 사용 못하면 reutrn.
 */
	if (!tick_check_broadcast_device(cur, dev))
		return;

	if (!try_module_get(dev->owner))
		return;

/*
 * IAMROOT, 2023.01.03:
 * - @cur과 교체한다.
 */
	clockevents_exchange_device(cur, dev);
	if (cur)
		cur->event_handler = clockevents_handle_noop;
	tick_broadcast_device.evtdev = dev;

/*
 * IAMROOT, 2023.01.03:
 * - broadcast할게 이미 있는 경우 여기서 시작한다.
 */
	if (!cpumask_empty(tick_broadcast_mask))
	{
		tick_broadcast_start_periodic(dev);
	}

	if (!(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return;

	/*
	 * If the system already runs in oneshot mode, switch the newly
	 * registered broadcast device to oneshot mode explicitly.
	 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   시스템이 이미 원샷 모드로 실행 중인 경우 새로 등록된 브로드캐스트 
 *   장치를 명시적으로 원샷 모드로 전환합니다.
 *
 * - 위에서 등록만한 tick_broadcast_device를 실제 설정한다.
 */
	if (tick_broadcast_oneshot_active()) {
		tick_broadcast_switch_to_oneshot();
		return;
	}

	/*
	 * Inform all cpus about this. We might be in a situation
	 * where we did not switch to oneshot mode because the per cpu
	 * devices are affected by CLOCK_EVT_FEAT_C3STOP and the lack
	 * of a oneshot capable broadcast device. Without that
	 * notification the systems stays stuck in periodic mode
	 * forever.
	 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   이에 대해 모든 CPU에 알립니다. CPU당 장치가 CLOCK_EVT_FEAT_C3STOP의 
 *   영향을 받고 원샷 가능 브로드캐스트 장치가 없기 때문에 원샷 모드로 
 *   전환하지 않은 상황일 수 있습니다. 해당 알림이 없으면 시스템은 
 *   주기적 모드에 영원히 고정됩니다.
 */
	tick_clock_notify();
}

/*
 * Check, if the device is the broadcast device
 */
/*
 * IAMROOT, 2023.01.03:
 * - chat open ai
 *   브로드캐스트 장치는 시스템의 모든 CPU에 클록 틱을 전달하는 데 사용되는 
 *   클록 이벤트 장치입니다. 클록 틱은 스케줄링, 시간 관리 및 선점과 같은 
 *   시간 기반 작업의 실행을 허용하기 위해 시스템에서 생성되는 주기적인 
 *   인터럽트입니다. 
 */
int tick_is_broadcast_device(struct clock_event_device *dev)
{
	return (dev && tick_broadcast_device.evtdev == dev);
}

int tick_broadcast_update_freq(struct clock_event_device *dev, u32 freq)
{
	int ret = -ENODEV;

	if (tick_is_broadcast_device(dev)) {
		raw_spin_lock(&tick_broadcast_lock);
		ret = __clockevents_update_freq(dev, freq);
		raw_spin_unlock(&tick_broadcast_lock);
	}
	return ret;
}


static void err_broadcast(const struct cpumask *mask)
{
	pr_crit_once("Failed to broadcast timer tick. Some CPUs may be unresponsive.\n");
}

/*
 * IAMROOT, 2023.01.03:
 * - @dev의 broadcast가 아직 설정이 안되있을경우 tick_broadcast로 설정한다.
 */
static void tick_device_setup_broadcast_func(struct clock_event_device *dev)
{
	if (!dev->broadcast)
		dev->broadcast = tick_broadcast;
	if (!dev->broadcast) {
		pr_warn_once("%s depends on broadcast, but no broadcast function available\n",
			     dev->name);
		dev->broadcast = err_broadcast;
	}
}

/*
 * Check, if the device is dysfunctional and a placeholder, which
 * needs to be handled by the broadcast device.
 */
/*
 * IAMROOT, 2023.01.03:
 * @return 1 : broadcast로 동작한다.
 *         0 : broadcast로 동작안한다.
 */
int tick_device_uses_broadcast(struct clock_event_device *dev, int cpu)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/*
	 * Devices might be registered with both periodic and oneshot
	 * mode disabled. This signals, that the device needs to be
	 * operated from the broadcast device and is a placeholder for
	 * the cpu local device.
	 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *  주기적 및 원샷 모드가 비활성화된 상태로 장치가 등록될 수 있습니다. 
 *  이는 장치가 브로드캐스트 장치에서 작동해야 하며 CPU 로컬 장치에 대한 
 *  자리 표시자임을 나타냅니다.
 */
	if (!tick_device_is_functional(dev)) {
/*
 * IAMROOT, 2023.01.03:
 * - @dev가 동작 하고 있지 않다면 
 *   1. @dev->event_handler = tick_handle_periodic 
 *      @dev->broadcast = tick_broadcast
 *   2. @cpu를 tick_broadcast_mask에 추가.
 *   3. - tick_broadcast_device가 periodic일 경우
 *      @bc의 event_handler를 tick_handle_periodic_broadcast로 설정
 *      - oneshot일 경우
 *      @bc의 event_handler를 tick_handle_oneshot_broadcast로 설정.
 */
		dev->event_handler = tick_handle_periodic;
		tick_device_setup_broadcast_func(dev);
		cpumask_set_cpu(cpu, tick_broadcast_mask);
		if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
			tick_broadcast_start_periodic(bc);
		else
			tick_broadcast_setup_oneshot(bc);
/*
 * IAMROOT, 2023.01.03:
 * - broadcast 설정완료.
 */
		ret = 1;
	} else {
		/*
		 * Clear the broadcast bit for this cpu if the
		 * device is not power state affected.
		 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   장치가 영향을 받는 전원 상태가 아닌 경우 이 CPU에 대한 브로드캐스트 
 *   비트를 지웁니다.
 */
		if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
			cpumask_clear_cpu(cpu, tick_broadcast_mask);
		else
			tick_device_setup_broadcast_func(dev);

		/*
		 * Clear the broadcast bit if the CPU is not in
		 * periodic broadcast on state.
		 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   CPU가 주기적인 브로드캐스트 온 상태가 아니면 브로드캐스트 비트를 지웁니다.
 */
		if (!cpumask_test_cpu(cpu, tick_broadcast_on))
			cpumask_clear_cpu(cpu, tick_broadcast_mask);

		switch (tick_broadcast_device.mode) {
		case TICKDEV_MODE_ONESHOT:
			/*
			 * If the system is in oneshot mode we can
			 * unconditionally clear the oneshot mask bit,
			 * because the CPU is running and therefore
			 * not in an idle state which causes the power
			 * state affected device to stop. Let the
			 * caller initialize the device.
			 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   시스템이 원샷 모드에 있으면 무조건 원샷 마스크 비트를 지울 수 있습니다. 
 *   CPU가 실행 중이고 따라서 전원 상태에 영향을 받는 장치가 중지되는 유휴 
 *   상태가 아니기 때문입니다. 발신자가 장치를 초기화하도록 합니다.
 *
 * - system이 oneshot을 사용하고 있으면 broadcast에서 current cpu를 한번 빼준다.
 *   oneshot으로 동작을 하기 때문에 굳이 broadcast에서 동작할 필요가없다.
 *   oneshot이면 결국 broadcast를 안하는 개념이므로 ret = 0
 */
			tick_broadcast_clear_oneshot(cpu);
			ret = 0;
			break;

		case TICKDEV_MODE_PERIODIC:
			/*
			 * If the system is in periodic mode, check
			 * whether the broadcast device can be
			 * switched off now.
			 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   시스템이 주기적 모드인 경우 브로드캐스트 장치를 지금 끌 수 있는지 
 *   확인하십시오.
 *
 * - broadcast할게 없는데 @bc가 살아 있다면 그냥 끄는듯 하다.
 */
			if (cpumask_empty(tick_broadcast_mask) && bc)
				clockevents_shutdown(bc);
			/*
			 * If we kept the cpu in the broadcast mask,
			 * tell the caller to leave the per cpu device
			 * in shutdown state. The periodic interrupt
			 * is delivered by the broadcast device, if
			 * the broadcast device exists and is not
			 * hrtimer based.
			 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   CPU를 브로드캐스트 마스크에 유지한 경우 호출자에게 CPU별 장치를 종료 
 *   상태로 두도록 지시합니다. 주기적 인터럽트는 브로드캐스트 장치가 존재하고 
 *   hrtimer 기반이 아닌 경우 브로드캐스트 장치에 의해 전달됩니다.
 *
 * - chat open ai
 *   hrtimer 기반 장치가 브로드캐스트 틱을 사용하지 않고 주기적 모드로 작동할 
 *   수 있기 때문입니다. hrtimer는 hrtimer API를 사용하는 고해상도 타이머로, 
 *   기존 타이머 API보다 더 정확한 타이머 인터럽트 스케줄링이 가능합니다.
 *
 *   hrtimer 기반 장치의 경우 브로드캐스트 모드를 사용할 필요가 없을 정도로 
 *   정확하게 자체 인터럽트를 생성할 수 있습니다. 따라서 이 경우 함수는 
 *   호출자가 브로드캐스트 장치가 아닌 CPU당 장치를 사용해야 함을 나타내는 
 *   0을 반환합니다.
 *
 *   브로드캐스트 장치가 hrtimer 기반이 아닌 경우 매우 빈번하거나 고해상도 
 *   인터럽트를 생성할 수 없습니다. 이 경우 주기적인 인터럽트를 위해 
 *   브로드캐스트 장치를 사용하는 것이 더 효율적일 수 있습니다. 
 *   모든 유휴 CPU에 대해 타이머를 한 번에 중지하거나 속도를 늦출 수 있기 
 *   때문입니다. 이 경우 함수는 CPU가 브로드캐스트 마스크에 있으면 1을 
 *   반환하여 호출자가 CPU당 장치가 아닌 브로드캐스트 장치를 사용해야 함을 
 *   나타냅니다. 
 *
 * - hrtimer를 지원하는경우 broadcast를 사용하지 않는다. 그러므로 return 0
 * - hritmer를 사용안하는경우 broadcast에 cpu가 사용중인지 확인한다.
 */
			if (bc && !(bc->features & CLOCK_EVT_FEAT_HRTIMER))
				ret = cpumask_test_cpu(cpu, tick_broadcast_mask);
			break;
		default:
			break;
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
	return ret;
}

int tick_receive_broadcast(void)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	struct clock_event_device *evt = td->evtdev;

	if (!evt)
		return -ENODEV;

	if (!evt->event_handler)
		return -EINVAL;

	evt->event_handler(evt);
	return 0;
}

/*
 * Broadcast the event to the cpus, which are set in the mask (mangled).
 */
static bool tick_do_broadcast(struct cpumask *mask)
{
	int cpu = smp_processor_id();
	struct tick_device *td;
	bool local = false;

	/*
	 * Check, if the current cpu is in the mask
	 */
	if (cpumask_test_cpu(cpu, mask)) {
		struct clock_event_device *bc = tick_broadcast_device.evtdev;

		cpumask_clear_cpu(cpu, mask);
		/*
		 * We only run the local handler, if the broadcast
		 * device is not hrtimer based. Otherwise we run into
		 * a hrtimer recursion.
		 *
		 * local timer_interrupt()
		 *   local_handler()
		 *     expire_hrtimers()
		 *       bc_handler()
		 *         local_handler()
		 *	     expire_hrtimers()
		 */
		local = !(bc->features & CLOCK_EVT_FEAT_HRTIMER);
	}

	if (!cpumask_empty(mask)) {
		/*
		 * It might be necessary to actually check whether the devices
		 * have different broadcast functions. For now, just use the
		 * one of the first device. This works as long as we have this
		 * misfeature only on x86 (lapic)
		 */
		td = &per_cpu(tick_cpu_device, cpumask_first(mask));
		td->evtdev->broadcast(mask);
	}
	return local;
}

/*
 * Periodic broadcast:
 * - invoke the broadcast handlers
 */
static bool tick_do_periodic_broadcast(void)
{
	cpumask_and(tmpmask, cpu_online_mask, tick_broadcast_mask);
	return tick_do_broadcast(tmpmask);
}

/*
 * Event handler for periodic broadcast ticks
 */
/*
 * IAMROOT, 2023.01.03:
 * - PASS
 * - 고성능 arm에서는 broadcast를 거의 사용안한다.
 * - event_handler에 등록되서 호출된다.
 * - broadcast deivce가 아닐경우 tick_handle_periodic이 호출될것이다.
 */
static void tick_handle_periodic_broadcast(struct clock_event_device *dev)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	bool bc_local;

	raw_spin_lock(&tick_broadcast_lock);

	/* Handle spurious interrupts gracefully */
	if (clockevent_state_shutdown(tick_broadcast_device.evtdev)) {
		raw_spin_unlock(&tick_broadcast_lock);
		return;
	}

	bc_local = tick_do_periodic_broadcast();

	if (clockevent_state_oneshot(dev)) {
		ktime_t next = ktime_add_ns(dev->next_event, TICK_NSEC);

		clockevents_program_event(dev, next, true);
	}
	raw_spin_unlock(&tick_broadcast_lock);

	/*
	 * We run the handler of the local cpu after dropping
	 * tick_broadcast_lock because the handler might deadlock when
	 * trying to switch to oneshot mode.
	 */
	if (bc_local)
		td->evtdev->event_handler(td->evtdev);
}

/**
 * tick_broadcast_control - Enable/disable or force broadcast mode
 * @mode:	The selected broadcast mode
 *
 * Called when the system enters a state where affected tick devices
 * might stop. Note: TICK_BROADCAST_FORCE cannot be undone.
 */
void tick_broadcast_control(enum tick_broadcast_mode mode)
{
	struct clock_event_device *bc, *dev;
	struct tick_device *td;
	int cpu, bc_stopped;
	unsigned long flags;

	/* Protects also the local clockevent device. */
	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);
	td = this_cpu_ptr(&tick_cpu_device);
	dev = td->evtdev;

	/*
	 * Is the device not affected by the powerstate ?
	 */
	if (!dev || !(dev->features & CLOCK_EVT_FEAT_C3STOP))
		goto out;

	if (!tick_device_is_functional(dev))
		goto out;

	cpu = smp_processor_id();
	bc = tick_broadcast_device.evtdev;
	bc_stopped = cpumask_empty(tick_broadcast_mask);

	switch (mode) {
	case TICK_BROADCAST_FORCE:
		tick_broadcast_forced = 1;
		fallthrough;
	case TICK_BROADCAST_ON:
		cpumask_set_cpu(cpu, tick_broadcast_on);
		if (!cpumask_test_and_set_cpu(cpu, tick_broadcast_mask)) {
			/*
			 * Only shutdown the cpu local device, if:
			 *
			 * - the broadcast device exists
			 * - the broadcast device is not a hrtimer based one
			 * - the broadcast device is in periodic mode to
			 *   avoid a hiccup during switch to oneshot mode
			 */
			if (bc && !(bc->features & CLOCK_EVT_FEAT_HRTIMER) &&
			    tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
				clockevents_shutdown(dev);
		}
		break;

	case TICK_BROADCAST_OFF:
		if (tick_broadcast_forced)
			break;
		cpumask_clear_cpu(cpu, tick_broadcast_on);
		if (cpumask_test_and_clear_cpu(cpu, tick_broadcast_mask)) {
			if (tick_broadcast_device.mode ==
			    TICKDEV_MODE_PERIODIC)
				tick_setup_periodic(dev, 0);
		}
		break;
	}

	if (bc) {
		if (cpumask_empty(tick_broadcast_mask)) {
			if (!bc_stopped)
				clockevents_shutdown(bc);
		} else if (bc_stopped) {
			if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
				tick_broadcast_start_periodic(bc);
			else
				tick_broadcast_setup_oneshot(bc);
		}
	}
out:
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}
EXPORT_SYMBOL_GPL(tick_broadcast_control);

/*
 * Set the periodic handler depending on broadcast on/off
 */
/*
 * IAMROOT, 2023.01.03:
 * - @broadcast에 따라서 event_handler를 설정한다.
 */
void tick_set_periodic_handler(struct clock_event_device *dev, int broadcast)
{
	if (!broadcast)
		dev->event_handler = tick_handle_periodic;
	else
		dev->event_handler = tick_handle_periodic_broadcast;
}

#ifdef CONFIG_HOTPLUG_CPU
static void tick_shutdown_broadcast(void)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC) {
		if (bc && cpumask_empty(tick_broadcast_mask))
			clockevents_shutdown(bc);
	}
}

/*
 * Remove a CPU from broadcasting
 */
void tick_broadcast_offline(unsigned int cpu)
{
	raw_spin_lock(&tick_broadcast_lock);
	cpumask_clear_cpu(cpu, tick_broadcast_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_on);
	tick_broadcast_oneshot_offline(cpu);
	tick_shutdown_broadcast();
	raw_spin_unlock(&tick_broadcast_lock);
}

#endif

void tick_suspend_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;
	if (bc)
		clockevents_shutdown(bc);

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * This is called from tick_resume_local() on a resuming CPU. That's
 * called from the core resume function, tick_unfreeze() and the magic XEN
 * resume hackery.
 *
 * In none of these cases the broadcast device mode can change and the
 * bit of the resuming CPU in the broadcast mask is safe as well.
 */
bool tick_resume_check_broadcast(void)
{
	if (tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT)
		return false;
	else
		return cpumask_test_cpu(smp_processor_id(), tick_broadcast_mask);
}

void tick_resume_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;

	if (bc) {
		clockevents_tick_resume(bc);

		switch (tick_broadcast_device.mode) {
		case TICKDEV_MODE_PERIODIC:
			if (!cpumask_empty(tick_broadcast_mask))
				tick_broadcast_start_periodic(bc);
			break;
		case TICKDEV_MODE_ONESHOT:
			if (!cpumask_empty(tick_broadcast_mask))
				tick_resume_broadcast_oneshot(bc);
			break;
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

#ifdef CONFIG_TICK_ONESHOT

static cpumask_var_t tick_broadcast_oneshot_mask __cpumask_var_read_mostly;
static cpumask_var_t tick_broadcast_pending_mask __cpumask_var_read_mostly;
static cpumask_var_t tick_broadcast_force_mask __cpumask_var_read_mostly;

/*
 * Exposed for debugging: see timer_list.c
 */
struct cpumask *tick_get_broadcast_oneshot_mask(void)
{
	return tick_broadcast_oneshot_mask;
}

/*
 * Called before going idle with interrupts disabled. Checks whether a
 * broadcast event from the other core is about to happen. We detected
 * that in tick_broadcast_oneshot_control(). The callsite can use this
 * to avoid a deep idle transition as we are about to get the
 * broadcast IPI right away.
 */
int tick_check_broadcast_expired(void)
{
	return cpumask_test_cpu(smp_processor_id(), tick_broadcast_force_mask);
}

/*
 * Set broadcast interrupt affinity
 */
/*
 * IAMROOT, 2023.01.03:
 * - @cpumask가 @bc와 불일치하면 affinity를 set한다.
 */
static void tick_broadcast_set_affinity(struct clock_event_device *bc,
					const struct cpumask *cpumask)
{
	if (!(bc->features & CLOCK_EVT_FEAT_DYNIRQ))
		return;

	if (cpumask_equal(bc->cpumask, cpumask))
		return;

	bc->cpumask = cpumask;
	irq_set_affinity(bc->irq, bc->cpumask);
}

/*
 * IAMROOT, 2023.01.03:
 * - @expires로 program.
 */
static void tick_broadcast_set_event(struct clock_event_device *bc, int cpu,
				     ktime_t expires)
{
	if (!clockevent_state_oneshot(bc))
		clockevents_switch_state(bc, CLOCK_EVT_STATE_ONESHOT);

	clockevents_program_event(bc, expires, 1);
	tick_broadcast_set_affinity(bc, cpumask_of(cpu));
}

static void tick_resume_broadcast_oneshot(struct clock_event_device *bc)
{
	clockevents_switch_state(bc, CLOCK_EVT_STATE_ONESHOT);
}

/*
 * Called from irq_enter() when idle was interrupted to reenable the
 * per cpu device.
 */
void tick_check_oneshot_broadcast_this_cpu(void)
{
	if (cpumask_test_cpu(smp_processor_id(), tick_broadcast_oneshot_mask)) {
		struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

		/*
		 * We might be in the middle of switching over from
		 * periodic to oneshot. If the CPU has not yet
		 * switched over, leave the device alone.
		 */
		if (td->mode == TICKDEV_MODE_ONESHOT) {
			clockevents_switch_state(td->evtdev,
					      CLOCK_EVT_STATE_ONESHOT);
		}
	}
}

/*
 * Handle oneshot mode broadcasting
 */
/*
 * IAMROOT, 2023.01.03:
 * - PASS
 * - 고성능 arm에서는 broadcast를 거의 사용안한다.
 * - broadcast deivce의 event handler 의해 실행된다.
 *   (tick_broadcast_setup_oneshot() 참고)
 */
static void tick_handle_oneshot_broadcast(struct clock_event_device *dev)
{
	struct tick_device *td;
	ktime_t now, next_event;
	int cpu, next_cpu = 0;
	bool bc_local;

	raw_spin_lock(&tick_broadcast_lock);
	dev->next_event = KTIME_MAX;
	next_event = KTIME_MAX;
	cpumask_clear(tmpmask);
	now = ktime_get();
	/* Find all expired events */
	for_each_cpu(cpu, tick_broadcast_oneshot_mask) {
		/*
		 * Required for !SMP because for_each_cpu() reports
		 * unconditionally CPU0 as set on UP kernels.
		 */
		if (!IS_ENABLED(CONFIG_SMP) &&
		    cpumask_empty(tick_broadcast_oneshot_mask))
			break;

		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev->next_event <= now) {
			cpumask_set_cpu(cpu, tmpmask);
			/*
			 * Mark the remote cpu in the pending mask, so
			 * it can avoid reprogramming the cpu local
			 * timer in tick_broadcast_oneshot_control().
			 */
			cpumask_set_cpu(cpu, tick_broadcast_pending_mask);
		} else if (td->evtdev->next_event < next_event) {
			next_event = td->evtdev->next_event;
			next_cpu = cpu;
		}
	}

	/*
	 * Remove the current cpu from the pending mask. The event is
	 * delivered immediately in tick_do_broadcast() !
	 */
	cpumask_clear_cpu(smp_processor_id(), tick_broadcast_pending_mask);

	/* Take care of enforced broadcast requests */
	cpumask_or(tmpmask, tmpmask, tick_broadcast_force_mask);
	cpumask_clear(tick_broadcast_force_mask);

	/*
	 * Sanity check. Catch the case where we try to broadcast to
	 * offline cpus.
	 */
	if (WARN_ON_ONCE(!cpumask_subset(tmpmask, cpu_online_mask)))
		cpumask_and(tmpmask, tmpmask, cpu_online_mask);

	/*
	 * Wakeup the cpus which have an expired event.
	 */
	bc_local = tick_do_broadcast(tmpmask);

	/*
	 * Two reasons for reprogram:
	 *
	 * - The global event did not expire any CPU local
	 * events. This happens in dyntick mode, as the maximum PIT
	 * delta is quite small.
	 *
	 * - There are pending events on sleeping CPUs which were not
	 * in the event mask
	 */
	if (next_event != KTIME_MAX)
		tick_broadcast_set_event(dev, next_cpu, next_event);

	raw_spin_unlock(&tick_broadcast_lock);

	if (bc_local) {
		td = this_cpu_ptr(&tick_cpu_device);
		td->evtdev->event_handler(td->evtdev);
	}
}

static int broadcast_needs_cpu(struct clock_event_device *bc, int cpu)
{
	if (!(bc->features & CLOCK_EVT_FEAT_HRTIMER))
		return 0;
	if (bc->next_event == KTIME_MAX)
		return 0;
	return bc->bound_on == cpu ? -EBUSY : 0;
}

static void broadcast_shutdown_local(struct clock_event_device *bc,
				     struct clock_event_device *dev)
{
	/*
	 * For hrtimer based broadcasting we cannot shutdown the cpu
	 * local device if our own event is the first one to expire or
	 * if we own the broadcast timer.
	 */
	if (bc->features & CLOCK_EVT_FEAT_HRTIMER) {
		if (broadcast_needs_cpu(bc, smp_processor_id()))
			return;
		if (dev->next_event < bc->next_event)
			return;
	}
	clockevents_switch_state(dev, CLOCK_EVT_STATE_SHUTDOWN);
}

static int ___tick_broadcast_oneshot_control(enum tick_broadcast_state state,
					     struct tick_device *td,
					     int cpu)
{
	struct clock_event_device *bc, *dev = td->evtdev;
	int ret = 0;
	ktime_t now;

	raw_spin_lock(&tick_broadcast_lock);
	bc = tick_broadcast_device.evtdev;

	if (state == TICK_BROADCAST_ENTER) {
		/*
		 * If the current CPU owns the hrtimer broadcast
		 * mechanism, it cannot go deep idle and we do not add
		 * the CPU to the broadcast mask. We don't have to go
		 * through the EXIT path as the local timer is not
		 * shutdown.
		 */
		ret = broadcast_needs_cpu(bc, cpu);
		if (ret)
			goto out;

		/*
		 * If the broadcast device is in periodic mode, we
		 * return.
		 */
		if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC) {
			/* If it is a hrtimer based broadcast, return busy */
			if (bc->features & CLOCK_EVT_FEAT_HRTIMER)
				ret = -EBUSY;
			goto out;
		}

		if (!cpumask_test_and_set_cpu(cpu, tick_broadcast_oneshot_mask)) {
			WARN_ON_ONCE(cpumask_test_cpu(cpu, tick_broadcast_pending_mask));

			/* Conditionally shut down the local timer. */
			broadcast_shutdown_local(bc, dev);

			/*
			 * We only reprogram the broadcast timer if we
			 * did not mark ourself in the force mask and
			 * if the cpu local event is earlier than the
			 * broadcast event. If the current CPU is in
			 * the force mask, then we are going to be
			 * woken by the IPI right away; we return
			 * busy, so the CPU does not try to go deep
			 * idle.
			 */
			if (cpumask_test_cpu(cpu, tick_broadcast_force_mask)) {
				ret = -EBUSY;
			} else if (dev->next_event < bc->next_event) {
				tick_broadcast_set_event(bc, cpu, dev->next_event);
				/*
				 * In case of hrtimer broadcasts the
				 * programming might have moved the
				 * timer to this cpu. If yes, remove
				 * us from the broadcast mask and
				 * return busy.
				 */
				ret = broadcast_needs_cpu(bc, cpu);
				if (ret) {
					cpumask_clear_cpu(cpu,
						tick_broadcast_oneshot_mask);
				}
			}
		}
	} else {
		if (cpumask_test_and_clear_cpu(cpu, tick_broadcast_oneshot_mask)) {
			clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
			/*
			 * The cpu which was handling the broadcast
			 * timer marked this cpu in the broadcast
			 * pending mask and fired the broadcast
			 * IPI. So we are going to handle the expired
			 * event anyway via the broadcast IPI
			 * handler. No need to reprogram the timer
			 * with an already expired event.
			 */
			if (cpumask_test_and_clear_cpu(cpu,
				       tick_broadcast_pending_mask))
				goto out;

			/*
			 * Bail out if there is no next event.
			 */
			if (dev->next_event == KTIME_MAX)
				goto out;
			/*
			 * If the pending bit is not set, then we are
			 * either the CPU handling the broadcast
			 * interrupt or we got woken by something else.
			 *
			 * We are no longer in the broadcast mask, so
			 * if the cpu local expiry time is already
			 * reached, we would reprogram the cpu local
			 * timer with an already expired event.
			 *
			 * This can lead to a ping-pong when we return
			 * to idle and therefore rearm the broadcast
			 * timer before the cpu local timer was able
			 * to fire. This happens because the forced
			 * reprogramming makes sure that the event
			 * will happen in the future and depending on
			 * the min_delta setting this might be far
			 * enough out that the ping-pong starts.
			 *
			 * If the cpu local next_event has expired
			 * then we know that the broadcast timer
			 * next_event has expired as well and
			 * broadcast is about to be handled. So we
			 * avoid reprogramming and enforce that the
			 * broadcast handler, which did not run yet,
			 * will invoke the cpu local handler.
			 *
			 * We cannot call the handler directly from
			 * here, because we might be in a NOHZ phase
			 * and we did not go through the irq_enter()
			 * nohz fixups.
			 */
			now = ktime_get();
			if (dev->next_event <= now) {
				cpumask_set_cpu(cpu, tick_broadcast_force_mask);
				goto out;
			}
			/*
			 * We got woken by something else. Reprogram
			 * the cpu local timer device.
			 */
			tick_program_event(dev->next_event, 1);
		}
	}
out:
	raw_spin_unlock(&tick_broadcast_lock);
	return ret;
}

static int tick_oneshot_wakeup_control(enum tick_broadcast_state state,
				       struct tick_device *td,
				       int cpu)
{
	struct clock_event_device *dev, *wd;

	dev = td->evtdev;
	if (td->mode != TICKDEV_MODE_ONESHOT)
		return -EINVAL;

	wd = tick_get_oneshot_wakeup_device(cpu);
	if (!wd)
		return -ENODEV;

	switch (state) {
	case TICK_BROADCAST_ENTER:
		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT_STOPPED);
		clockevents_switch_state(wd, CLOCK_EVT_STATE_ONESHOT);
		clockevents_program_event(wd, dev->next_event, 1);
		break;
	case TICK_BROADCAST_EXIT:
		/* We may have transitioned to oneshot mode while idle */
		if (clockevent_get_state(wd) != CLOCK_EVT_STATE_ONESHOT)
			return -ENODEV;
	}

	return 0;
}

int __tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	int cpu = smp_processor_id();

	if (!tick_oneshot_wakeup_control(state, td, cpu))
		return 0;

	if (tick_broadcast_device.evtdev)
		return ___tick_broadcast_oneshot_control(state, td, cpu);

	/*
	 * If there is no broadcast or wakeup device, tell the caller not
	 * to go into deep idle.
	 */
	return -EBUSY;
}

/*
 * Reset the one shot broadcast for a cpu
 *
 * Called with tick_broadcast_lock held
 */
/*
 * IAMROOT, 2023.01.03:
 * - @cpu에 대한 tick_broadcast의 oneshot, pending의 cpumask bit clear
 */
static void tick_broadcast_clear_oneshot(int cpu)
{
	cpumask_clear_cpu(cpu, tick_broadcast_oneshot_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_pending_mask);
}

/*
 * IAMROOT, 2023.01.03:
 * - @mask에 해당하는 cpu들에 대하여 @expires로 next event 동작시간을 설정.
 */
static void tick_broadcast_init_next_event(struct cpumask *mask,
					   ktime_t expires)
{
	struct tick_device *td;
	int cpu;

	for_each_cpu(cpu, mask) {
		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev)
			td->evtdev->next_event = expires;
	}
}

static inline ktime_t tick_get_next_period(void)
{
	ktime_t next;

	/*
	 * Protect against concurrent updates (store /load tearing on
	 * 32bit). It does not matter if the time is already in the
	 * past. The broadcast device which is about to be programmed will
	 * fire in any case.
	 */
	raw_spin_lock(&jiffies_lock);
	next = tick_next_period;
	raw_spin_unlock(&jiffies_lock);
	return next;
}

/**
 * tick_broadcast_setup_oneshot - setup the broadcast device
 */
/*
 * IAMROOT, 2023.01.03:
 * - @bc event_handler를 tick_handle_oneshot_broadcast로 설정한다.
 *   periodic설정으로 되있엇고, current cpu를 제외한 cpu들이 남아있었다면,
 *   해당 cpu에 대해서 잔여처리를 수행한다.
 */
static void tick_broadcast_setup_oneshot(struct clock_event_device *bc)
{
	int cpu = smp_processor_id();

	if (!bc)
		return;

	/* Set it up only once ! */
	if (bc->event_handler != tick_handle_oneshot_broadcast) {
		int was_periodic = clockevent_state_periodic(bc);

		bc->event_handler = tick_handle_oneshot_broadcast;

		/*
		 * We must be careful here. There might be other CPUs
		 * waiting for periodic broadcast. We need to set the
		 * oneshot_mask bits for those and program the
		 * broadcast device to fire.
		 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   여기서 주의해야 합니다. 주기적인 브로드캐스트를 기다리는 다른 CPU가 있을 
 *   수 있습니다. 이를 위해 oneshot_mask 비트를 설정하고 브로드캐스트 장치를 
 *   실행하도록 프로그래밍해야 합니다.
 *
 * - tick_broadcast_mask에서 @cpu bit를 제외한 나머지를 
 *   tick_broadcast_oneshot_mask에 추가한다.
 */
		cpumask_copy(tmpmask, tick_broadcast_mask);
		cpumask_clear_cpu(cpu, tmpmask);
		cpumask_or(tick_broadcast_oneshot_mask,
			   tick_broadcast_oneshot_mask, tmpmask);
/*
 * IAMROOT, 2023.01.03:
 * - periodic이 였는데, @cpu를 제외한 tick_broadcast_mask에 cpu들이 존재한다면
 *   해당 cpu들에 대해 expire시간을 정하고 @bc는 oneshot으로 정한다.
 */
		if (was_periodic && !cpumask_empty(tmpmask)) {
			ktime_t nextevt = tick_get_next_period();

			clockevents_switch_state(bc, CLOCK_EVT_STATE_ONESHOT);
			tick_broadcast_init_next_event(tmpmask, nextevt);
			tick_broadcast_set_event(bc, cpu, nextevt);
		} else
/*
 * IAMROOT, 2023.01.03:
 * - perioidc이 아니였거나, periodic이였어도 잔여 cpu처리를 할 필요없으면
 *   periodic을 expire off.
 */
			bc->next_event = KTIME_MAX;
	} else {
		/*
		 * The first cpu which switches to oneshot mode sets
		 * the bit for all other cpus which are in the general
		 * (periodic) broadcast mask. So the bit is set and
		 * would prevent the first broadcast enter after this
		 * to program the bc device.
		 */
/*
 * IAMROOT, 2023.01.03:
 * - papago
 *   원샷 모드로 전환하는 첫 번째 CPU는 일반(주기적) 브로드캐스트 마스크에 
 *   있는 다른 모든 CPU에 대한 비트를 설정합니다. 따라서 비트가 설정되고 bc 
 *   장치를 프로그래밍하기 위해 이 이후에 첫 번째 브로드캐스트 입력을 
 *   방지합니다.
 */
		tick_broadcast_clear_oneshot(cpu);
	}
}

/*
 * Select oneshot operating mode for the broadcast device
 */
/*
 * IAMROOT, 2022.12.03:
 * - broadcast 설정을 한다.
 *   고성능 arm계열에서는 명시적으로만 설정할뿐
 *   거의 사용하지 않는다. 
 */
void tick_broadcast_switch_to_oneshot(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	tick_broadcast_device.mode = TICKDEV_MODE_ONESHOT;
/*
 * IAMROOT, 2022.12.03:
 * - 설정되있는경우에만 setup.
 */
	bc = tick_broadcast_device.evtdev;
	if (bc)
		tick_broadcast_setup_oneshot(bc);

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

#ifdef CONFIG_HOTPLUG_CPU
void hotplug_cpu__broadcast_tick_pull(int deadcpu)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);
	bc = tick_broadcast_device.evtdev;

	if (bc && broadcast_needs_cpu(bc, deadcpu)) {
		/* This moves the broadcast assignment to this CPU: */
		clockevents_program_event(bc, bc->next_event, 1);
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Remove a dying CPU from broadcasting
 */
static void tick_broadcast_oneshot_offline(unsigned int cpu)
{
	if (tick_get_oneshot_wakeup_device(cpu))
		tick_set_oneshot_wakeup_device(NULL, cpu);

	/*
	 * Clear the broadcast masks for the dead cpu, but do not stop
	 * the broadcast device!
	 */
	cpumask_clear_cpu(cpu, tick_broadcast_oneshot_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_pending_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_force_mask);
}
#endif

/*
 * Check, whether the broadcast device is in one shot mode
 */
/*
 * IAMROOT, 2023.01.03:
 * @return 1 : tick_broadcast_device mode TICKDEV_MODE_ONESHOT
 * - tick_broadcast_device가 oneshot인지 확인.
 */
int tick_broadcast_oneshot_active(void)
{
	return tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT;
}

/*
 * Check whether the broadcast device supports oneshot.
 */
/*
 * IAMROOT, 2022.12.03:
 * - CLOCK_EVT_FEAT_ONESHOT 이 있는지 확인.
 */
bool tick_broadcast_oneshot_available(void)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	return bc ? bc->features & CLOCK_EVT_FEAT_ONESHOT : false;
}

#else
int __tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	if (!bc || (bc->features & CLOCK_EVT_FEAT_HRTIMER))
		return -EBUSY;

	return 0;
}
#endif

void __init tick_broadcast_init(void)
{
	zalloc_cpumask_var(&tick_broadcast_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&tick_broadcast_on, GFP_NOWAIT);
	zalloc_cpumask_var(&tmpmask, GFP_NOWAIT);
#ifdef CONFIG_TICK_ONESHOT
	zalloc_cpumask_var(&tick_broadcast_oneshot_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&tick_broadcast_pending_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&tick_broadcast_force_mask, GFP_NOWAIT);
#endif
}
