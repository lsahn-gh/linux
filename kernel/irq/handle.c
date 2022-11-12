// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code. Detailed
 * information is available in Documentation/core-api/genericirq.rst
 *
 */

#include <linux/irq.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>

#include <trace/events/irq.h>

#include "internals.h"

#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init;
#endif

/**
 * handle_bad_irq - handle spurious and unhandled irqs
 * @desc:      description of the interrupt
 *
 * Handles spurious and unhandled IRQ's. It also prints a debugmessage.
 */
/*
 * IAMROOT, 2022.10.01:
 * - invalid handle이 등록될려할때 대신 등록되는것. 
 */
void handle_bad_irq(struct irq_desc *desc)
{
	unsigned int irq = irq_desc_get_irq(desc);

	print_irq_desc(irq, desc);
	kstat_incr_irqs_this_cpu(desc);
	ack_bad_irq(irq);
}
EXPORT_SYMBOL_GPL(handle_bad_irq);

/*
 * Special, empty irq handler:
 */
irqreturn_t no_action(int cpl, void *dev_id)
{
	return IRQ_NONE;
}
EXPORT_SYMBOL_GPL(no_action);

static void warn_no_thread(unsigned int irq, struct irqaction *action)
{
	if (test_and_set_bit(IRQTF_WARNED, &action->thread_flags))
		return;

	printk(KERN_WARNING "IRQ %d device %s returned IRQ_WAKE_THREAD "
	       "but no thread function available.", irq, action->name);
}

/*
 * IAMROOT, 2022.11.12:
 * - flag를 정리하고 wake_up_process를 한다.
 */
void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action)
{
	/*
	 * In case the thread crashed and was killed we just pretend that
	 * we handled the interrupt. The hardirq handler has disabled the
	 * device interrupt, so no irq storm is lurking.
	 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   쓰레드가 크래시를 일으키고 죽으면 우리는 인터럽트를 처리한 척합니다.
 *   hardirq 처리기가 장치 인터럽트를 비활성화했으므로 irq 스톰이 숨어 있지
 *   않습니다.
 *
 * - @desc를 사용하는 thread가 종료중. 굳이 처리 할필요없다.
 */
	if (action->thread->flags & PF_EXITING)
		return;

	/*
	 * Wake up the handler thread for this action. If the
	 * RUNTHREAD bit is already set, nothing to do.
	 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   이 작업에 대한 핸들러 스레드를 깨우십시오. RUNTHREAD 비트가 이미
 *   설정되어 있으면 아무 작업도 수행할 수 없습니다.
 *
 * - IRQTF_RUNTHREAD이게 있다면 이미 thread 깨웠다. 빠져나간다.
 *   아니라면 set한다. (test and set)
 */
	if (test_and_set_bit(IRQTF_RUNTHREAD, &action->thread_flags))
		return;

	/*
	 * It's safe to OR the mask lockless here. We have only two
	 * places which write to threads_oneshot: This code and the
	 * irq thread.
	 *
	 * This code is the hard irq context and can never run on two
	 * cpus in parallel. If it ever does we have more serious
	 * problems than this bitmask.
	 *
	 * The irq threads of this irq which clear their "running" bit
	 * in threads_oneshot are serialized via desc->lock against
	 * each other and they are serialized against this code by
	 * IRQS_INPROGRESS.
	 *
	 * Hard irq handler:
	 *
	 *	spin_lock(desc->lock);
	 *	desc->state |= IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *	set_bit(IRQTF_RUNTHREAD, &action->thread_flags);
	 *	desc->threads_oneshot |= mask;
	 *	spin_lock(desc->lock);
	 *	desc->state &= ~IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *
	 * irq thread:
	 *
	 * again:
	 *	spin_lock(desc->lock);
	 *	if (desc->state & IRQS_INPROGRESS) {
	 *		spin_unlock(desc->lock);
	 *		while(desc->state & IRQS_INPROGRESS)
	 *			cpu_relax();
	 *		goto again;
	 *	}
	 *	if (!test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
	 *		desc->threads_oneshot &= ~mask;
	 *	spin_unlock(desc->lock);
	 *
	 * So either the thread waits for us to clear IRQS_INPROGRESS
	 * or we are waiting in the flow handler for desc->lock to be
	 * released before we reach this point. The thread also checks
	 * IRQTF_RUNTHREAD under desc->lock. If set it leaves
	 * threads_oneshot untouched and runs the thread another time.
	 */

/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   여기에서 마스크 잠금 장치를 사용하지 않는 것이 안전합니다.
 *   thread_oneshot에 쓰는 곳은 두 곳뿐입니다: 이 코드와 irq 스레드.
 *
 *   이 코드는 하드 irq 컨텍스트이며 두 개의 CPU에서 병렬로 실행할 수
 *   없습니다. 그렇다면 이 비트마스크보다 더 심각한 문제가 발생합니다
 *
 *   thread_oneshot에서 실행 비트를 지우는 이 irq의 irq 스레드는 서로에 대해
 *   desc->lock을 통해 직렬화되고 IRQS_INPROGRESS에 의해 이 코드에 대해
 *   직렬화됩니다.
 *
 *   ...
 *
 *   따라서 스레드는 우리가 IRQS_INPROGRESS를 지우기를 기다리거나 이 지점에
 *   도달하기 전에 desc->lock이 해제되기 위해 flow handler에서 기다리고
 *   있습니다. 스레드는 또한 desc->lock에서 IRQTF_RUNTHREAD를 확인합니다.
 *   설정하면 thread_oneshot을 그대로 두고 스레드를 다시 실행합니다.
 *
 * - hard_irq
 *   spin_lock은 flag제어 할때에만 사용하고, spin_lock안에서 flag로
 *   직렬화 처리를 한다.(handle_irq_event() 참고)
 *
 *  - irq thread
 *    spin_lock을 통해서 inprogress를 check한다.
 */
	desc->threads_oneshot |= action->thread_mask;

	/*
	 * We increment the threads_active counter in case we wake up
	 * the irq thread. The irq thread decrements the counter when
	 * it returns from the handler or in the exit path and wakes
	 * up waiters which are stuck in synchronize_irq() when the
	 * active count becomes zero. synchronize_irq() is serialized
	 * against this code (hard irq handler) via IRQS_INPROGRESS
	 * like the finalize_oneshot() code. See comment above.
	 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   irq 스레드를 깨울 경우에 대비하여 thread_active 카운터를 증가시킵니다.
 *   irq 스레드는 핸들러에서 또는 종료 경로로 돌아올 때 카운터를 감소시키고
 *   활성 카운트가 0이 될 때 synchronized_irq()에 갇혀 있는 웨이터를
 *   깨웁니다. synchronize_irq()는 finalize_oneshot() 코드와 같이
 *   IRQS_INPROGRESS를 통해 이 코드(하드 irq 핸들러)에 대해 직렬화됩니다.
 *   위의 주석을 참조하십시오.
 */
	atomic_inc(&desc->threads_active);

	wake_up_process(action->thread);
}

/*
 * IAMROOT, 2022.11.12:
 * - action handler를 수행후 필요하다면 thread도 생성한다.
 */
irqreturn_t __handle_irq_event_percpu(struct irq_desc *desc, unsigned int *flags)
{
	irqreturn_t retval = IRQ_NONE;
	unsigned int irq = desc->irq_data.irq;
	struct irqaction *action;

	record_irq_time(desc);

/*
 * IAMROOT, 2022.11.12:
 * - spi이므로 여러개가 있을수있다.
 */
	for_each_action_of_desc(desc, action) {
		irqreturn_t res;

		/*
		 * If this IRQ would be threaded under force_irqthreads, mark it so.
		 */

/*
 * IAMROOT, 2022.11.12:
 * - threaded irq 인지 확인한다.
 *   irq_settings_can_thread(desc) : contoller에서 결정.
 *   action->flags                 : user에서 결정.
 */
		if (irq_settings_can_thread(desc) &&
		    !(action->flags & (IRQF_NO_THREAD | IRQF_PERCPU | IRQF_ONESHOT)))
			lockdep_hardirq_threaded();

		trace_irq_handler_entry(irq, action);
		res = action->handler(irq, action->dev_id);
		trace_irq_handler_exit(irq, action, res);

		if (WARN_ONCE(!irqs_disabled(),"irq %u handler %pS enabled interrupts\n",
			      irq, action->handler))
			local_irq_disable();

/*
 * IAMROOT, 2022.11.12:
 * - result여부에 따라 thread를 wakeup한다.
 */
		switch (res) {
		case IRQ_WAKE_THREAD:
			/*
			 * Catch drivers which return WAKE_THREAD but
			 * did not set up a thread function
			 */
/*
 * IAMROOT, 2022.11.12:
 * - thread fn이 등록이 안됬는데 thread wake 요청이 온경우 경고를 띄우고
 *   break.
 */
			if (unlikely(!action->thread_fn)) {
				warn_no_thread(irq, action);
				break;
			}

			__irq_wake_thread(desc, action);

			fallthrough;	/* to add to randomness */
		case IRQ_HANDLED:
			*flags |= action->flags;
			break;

		default:
			break;
		}

		retval |= res;
	}

	return retval;
}

/*
 * IAMROOT, 2022.11.12:
 * - 
 */
irqreturn_t handle_irq_event_percpu(struct irq_desc *desc)
{
	irqreturn_t retval;
	unsigned int flags = 0;

	retval = __handle_irq_event_percpu(desc, &flags);

	add_interrupt_randomness(desc->irq_data.irq, flags);

	if (!irq_settings_no_debug(desc))
		note_interrupt(desc, retval);
	return retval;
}

/*
 * IAMROOT, 2022.11.12:
 *   pending flag가 여기서 지워지고, inprogress를 표시한다.
 *                   V
 *               handler 실행.
 *                   V
 *            inprogress clear.
 */
irqreturn_t handle_irq_event(struct irq_desc *desc)
{
	irqreturn_t ret;

	desc->istate &= ~IRQS_PENDING;
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	raw_spin_unlock(&desc->lock);

	ret = handle_irq_event_percpu(desc);

	raw_spin_lock(&desc->lock);
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	return ret;
}

#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER

/*
 * IAMROOT, 2022.10.08:
 * - 전역 함수에 등록한다. 이미 있으면 busy
 * - gic의 경우 gic_handle_irq
 */
int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	if (handle_arch_irq)
		return -EBUSY;

	handle_arch_irq = handle_irq;
	return 0;
}
#endif
