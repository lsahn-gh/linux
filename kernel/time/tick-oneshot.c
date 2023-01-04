// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains functions which manage high resolution tick
 * related events.
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

#include "tick-internal.h"

/**
 * tick_program_event
 */
/*
 * IAMROOT, 2022.12.03:
 * - @expire == KTIME_MAX
 *   stop
 * - tick_cpu_device가 stop상태였다면.
 *   oneshot mode로 변경후 program.
 * - @expires로 program한다.
 *
 * - @force == 1인 경우
 *   expires가 현재시각 이전이여도 prgram을 시도하고,
 *   program을 실패해도 min_delta_ns * 10의 시간까지 최대 10번을 재시도한다.
 */
int tick_program_event(ktime_t expires, int force)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	if (unlikely(expires == KTIME_MAX)) {
		/*
		 * We don't need the clock event device any more, stop it.
		 */
		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT_STOPPED);
		dev->next_event = KTIME_MAX;
		return 0;
	}

	if (unlikely(clockevent_state_oneshot_stopped(dev))) {
		/*
		 * We need the clock event again, configure it in ONESHOT mode
		 * before using it.
		 */
		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
	}

	return clockevents_program_event(dev, expires, force);
}

/**
 * tick_resume_oneshot - resume oneshot mode
 */
void tick_resume_oneshot(void)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
	clockevents_program_event(dev, ktime_get(), true);
}

/**
 * tick_setup_oneshot - setup the event device for oneshot mode (hres or nohz)
 */
/*
 * IAMROOT, 2023.01.03:
 * - @handler를 @newdev에 설정하고 @nexet_event로 program한다.
 */
void tick_setup_oneshot(struct clock_event_device *newdev,
			void (*handler)(struct clock_event_device *),
			ktime_t next_event)
{
	newdev->event_handler = handler;
	clockevents_switch_state(newdev, CLOCK_EVT_STATE_ONESHOT);
	clockevents_program_event(newdev, next_event, true);
}

/**
 * tick_switch_to_oneshot - switch to oneshot mode
 */
/*
 * IAMROOT, 2022.12.03:
 * --- periodic -> hrtimer oneshot 전환 ----
 * 1. 부팅시 최초에 td->mode는 TICKDEV_MODE_PERIODIC이 였다.
 * 2. event_handler는 tick_handle_periodic이였다.
 * 3. 현재 함수에 진입하면서 mode는 oneshot으로, handler는 @handler로 대체된다.
 * -----------------------------------------
 *
 *  - pcpu tick_cpu_device를 oneshot mode로 전환하고 event_handler를 @handler
 *  로 교체한다.
 */
int tick_switch_to_oneshot(void (*handler)(struct clock_event_device *))
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	struct clock_event_device *dev = td->evtdev;

	if (!dev || !(dev->features & CLOCK_EVT_FEAT_ONESHOT) ||
		    !tick_device_is_functional(dev)) {

		pr_info("Clockevents: could not switch to one-shot mode:");
		if (!dev) {
			pr_cont(" no tick device\n");
		} else {
			if (!tick_device_is_functional(dev))
				pr_cont(" %s is not functional.\n", dev->name);
			else
				pr_cont(" %s does not support one-shot mode.\n",
					dev->name);
		}
		return -EINVAL;
	}

	td->mode = TICKDEV_MODE_ONESHOT;
	dev->event_handler = handler;
	clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
	tick_broadcast_switch_to_oneshot();
	return 0;
}

/**
 * tick_check_oneshot_mode - check whether the system is in oneshot mode
 *
 * returns 1 when either nohz or highres are enabled. otherwise 0.
 */
/*
 * IAMROOT, 2023.01.03:
 * @return 1 oneshot mode. return 0 oneshot mode아님.
 * - TICKDEV_MODE_ONESHOT mode인지 확인.
 */
int tick_oneshot_mode_active(void)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = __this_cpu_read(tick_cpu_device.mode) == TICKDEV_MODE_ONESHOT;
	local_irq_restore(flags);

	return ret;
}

#ifdef CONFIG_HIGH_RES_TIMERS
/**
 * tick_init_highres - switch to high resolution mode
 *
 * Called with interrupts disabled.
 */
/*
 * IAMROOT, 2022.12.03:
 * - hrtimer_interrupt oneshot event_handler를 사용한다.
 */
int tick_init_highres(void)
{
	return tick_switch_to_oneshot(hrtimer_interrupt);
}
#endif
