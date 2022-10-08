// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner
 *
 * This file contains the IRQ-resend code
 *
 * If the interrupt is waiting to be processed, we try to re-run it.
 * We can't directly run it from here since the caller might be in an
 * interrupt-protected region. Not all irq controller chips can
 * retrigger interrupts at the hardware level, so in those cases
 * we allow the resending of IRQs via a tasklet.
 */

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/interrupt.h>

#include "internals.h"

#ifdef CONFIG_HARDIRQS_SW_RESEND

/* Bitmap to handle software resend of interrupts: */
static DECLARE_BITMAP(irqs_resend, IRQ_BITMAP_BITS);

/*
 * Run software resends of IRQ's
 */
/*
 * IAMROOT, 2022.10.08:
 * - irq에 해당하는 handle_irq를 호출한다
 */
static void resend_irqs(struct tasklet_struct *unused)
{
	struct irq_desc *desc;
	int irq;

	while (!bitmap_empty(irqs_resend, nr_irqs)) {
		irq = find_first_bit(irqs_resend, nr_irqs);
		clear_bit(irq, irqs_resend);
		desc = irq_to_desc(irq);
		if (!desc)
			continue;
		local_irq_disable();
		desc->handle_irq(desc);
		local_irq_enable();
	}
}

/* Tasklet to handle resend: */
static DECLARE_TASKLET(resend_tasklet, resend_irqs);

/*
 * IAMROOT, 2022.10.08:
 * - irq_retrigger등이 구현이 안됬을경우 들어올수있다.
 */
static int irq_sw_resend(struct irq_desc *desc)
{
	unsigned int irq = irq_desc_get_irq(desc);

	/*
	 * Validate whether this interrupt can be safely injected from
	 * non interrupt context
	 */
/*
 * IAMROOT, 2022.10.08:
 * - sw 지원 여부를 확인한다.
 * - gic같은 경우엔 hw방식을 지원하기 때문에 sw 방식은 지원하지 않는다.
 */
	if (handle_enforce_irqctx(&desc->irq_data))
		return -EINVAL;

	/*
	 * If the interrupt is running in the thread context of the parent
	 * irq we need to be careful, because we cannot trigger it
	 * directly.
	 */
	if (irq_settings_is_nested_thread(desc)) {
		/*
		 * If the parent_irq is valid, we retrigger the parent,
		 * otherwise we do nothing.
		 */
		if (!desc->parent_irq)
			return -EINVAL;
		irq = desc->parent_irq;
	}

	/* Set it pending and activate the softirq: */
	set_bit(irq, irqs_resend);
	tasklet_schedule(&resend_tasklet);
	return 0;
}

#else
static int irq_sw_resend(struct irq_desc *desc)
{
	return -EINVAL;
}
#endif

/*
 * IAMROOT, 2022.10.08:
 * - @irq_retrigger:	resend an IRQ to the CPU
 * - resend하기전에 irq_retrigger callback func을 수행한다.
 * - ex) gic v3의 경우 gic_irq_set_irqchip_state을 통해
 *   GICD_ISPENDR bit를 set한다.
 */
static int try_retrigger(struct irq_desc *desc)
{
	if (desc->irq_data.chip->irq_retrigger)
		return desc->irq_data.chip->irq_retrigger(&desc->irq_data);

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	return irq_chip_retrigger_hierarchy(&desc->irq_data);
#else
	return 0;
#endif
}

/*
 * IRQ resend
 *
 * Is called with interrupts disabled and desc->lock held.
 */
/*
 * IAMROOT, 2022.10.08:
 * - irq_retrigger를 수행한다. 중복수행 여부, busy check등을 겸한다.
 */
int check_irq_resend(struct irq_desc *desc, bool inject)
{
	int err = 0;

	/*
	 * We do not resend level type interrupts. Level type interrupts
	 * are resent by hardware when they are still active. Clear the
	 * pending bit so suspend/resume does not get confused.
	 */
/*
 * IAMROOT, 2022.10.08:
 * - level type은 안한다.
 */
	if (irq_settings_is_level(desc)) {
		desc->istate &= ~IRQS_PENDING;
		return -EINVAL;
	}

/*
 * IAMROOT, 2022.10.08:
 * - 방금 보냈다면.
 */
	if (desc->istate & IRQS_REPLAY)
		return -EBUSY;

/*
 * IAMROOT, 2022.10.08:
 * - inject가 false인 경우, pending flag를 확인한다.
 *   inject가 true인 경우엔 pedning flag가 없어도 수행한다.
 */
	if (!(desc->istate & IRQS_PENDING) && !inject)
		return 0;

	desc->istate &= ~IRQS_PENDING;

/*
 * IAMROOT, 2022.10.08:
 * - 실패하거나 구현이 안된경우  irq_sw_resend.
 *   실패조건은 잘못된 irq번호등의 이유가된다.
 *   아마 보통은 구현이 안됬을때 sw로 resend를 한다는 의미일것이다.
 */
	if (!try_retrigger(desc))
		err = irq_sw_resend(desc);

	/* If the retrigger was successful, mark it with the REPLAY bit */
	if (!err)
		desc->istate |= IRQS_REPLAY;
	return err;
}

#ifdef CONFIG_GENERIC_IRQ_INJECTION
/**
 * irq_inject_interrupt - Inject an interrupt for testing/error injection
 * @irq:	The interrupt number
 *
 * This function must only be used for debug and testing purposes!
 *
 * Especially on x86 this can cause a premature completion of an interrupt
 * affinity change causing the interrupt line to become stale. Very
 * unlikely, but possible.
 *
 * The injection can fail for various reasons:
 * - Interrupt is not activated
 * - Interrupt is NMI type or currently replaying
 * - Interrupt is level type
 * - Interrupt does not support hardware retrigger and software resend is
 *   either not enabled or not possible for the interrupt.
 */
int irq_inject_interrupt(unsigned int irq)
{
	struct irq_desc *desc;
	unsigned long flags;
	int err;

	/* Try the state injection hardware interface first */
	if (!irq_set_irqchip_state(irq, IRQCHIP_STATE_PENDING, true))
		return 0;

	/* That failed, try via the resend mechanism */
	desc = irq_get_desc_buslock(irq, &flags, 0);
	if (!desc)
		return -EINVAL;

	/*
	 * Only try to inject when the interrupt is:
	 *  - not NMI type
	 *  - activated
	 */
	if ((desc->istate & IRQS_NMI) || !irqd_is_activated(&desc->irq_data))
		err = -EINVAL;
	else
		err = check_irq_resend(desc, true);

	irq_put_desc_busunlock(desc, flags);
	return err;
}
EXPORT_SYMBOL_GPL(irq_inject_interrupt);
#endif
