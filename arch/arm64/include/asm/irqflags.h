/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_IRQFLAGS_H
#define __ASM_IRQFLAGS_H

#include <asm/alternative.h>
#include <asm/barrier.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>

/*
 * Aarch64 has flags for masking: Debug, Asynchronous (serror), Interrupts and
 * FIQ exceptions, in the 'daif' register. We mask and unmask them in 'daif'
 * order:
 * Masking debug exceptions causes all other exceptions to be masked too/
 * Masking SError masks IRQ/FIQ, but not debug exceptions. IRQ and FIQ are
 * always masked and unmasked together, and have no side effects for other
 * flags. Keeping to this order makes it easier for entry.S to know which
 * exceptions should be unmasked.
 */
/*
 * IAMROOT, 2023.03.16:
 * - papago
 *   Aarch64에는 'daif' 레지스터에 디버그, 비동기(serror), 인터럽트 및 
 *   FIQ 예외와 같은 마스킹 플래그가 있습니다. 우리는 'daif'에서
 *   그것들을 마스킹하고 마스킹 해제합니다.
 *   order:
 *   디버그 예외를 마스킹하면 다른 모든 예외도 마스킹됩니다. SError
 *   마스킹은 IRQ/FIQ를 마스킹하지만 디버그 예외는 마스킹하지 않습니다.
 *   IRQ 및 FIQ는 항상 함께 마스킹 및 마스킹 해제되며 다른 플래그에 대한
 *   부작용이 없습니다. 이 순서를 유지하면 entry.S가 마스크를 해제해야
 *   하는 예외를 더 쉽게 알 수 있습니다. 
 */

/*
 * CPU interrupt mask handling.
 */
/*
 * IAMROOT, 2023.03.16:
 * - irq enable
 */
static inline void arch_local_irq_enable(void)
{
	if (system_has_prio_mask_debugging()) {
		u32 pmr = read_sysreg_s(SYS_ICC_PMR_EL1);

		WARN_ON_ONCE(pmr != GIC_PRIO_IRQON && pmr != GIC_PRIO_IRQOFF);
	}

	asm volatile(ALTERNATIVE(
		"msr	daifclr, #3		// arch_local_irq_enable",
		__msr_s(SYS_ICC_PMR_EL1, "%0"),
		ARM64_HAS_IRQ_PRIO_MASKING)
		:
		: "r" ((unsigned long) GIC_PRIO_IRQON)
		: "memory");

	pmr_sync();
}

/*
 * IAMROOT, 2021.09.11:
 * ---- (old 5.10)
 * - daifset : pstate register 에서 daif만 따서 만든 register.
 *             해당 register에서는 [9:6]을 사용하는데 #2는 7bit를 set한다는것
 *             즉 irq만을 disable한다.
 * ----
 * - daifset : pstate register 에서 daif만 따서 만든 register.
 *             해당 register에서는 [9:6]을 사용하는데 #3는 6,7bit를 set한다.
 *             즉 irq, fiq를 disable한다.
 * - daifset[9] : D. debug
 * - daifset[8] : A. SError
 * - daifset[7] : I. IRQ
 * - daifset[6] : F. FIQ
 *             
 */
static inline void arch_local_irq_disable(void)
{
	if (system_has_prio_mask_debugging()) {
		u32 pmr = read_sysreg_s(SYS_ICC_PMR_EL1);

		WARN_ON_ONCE(pmr != GIC_PRIO_IRQON && pmr != GIC_PRIO_IRQOFF);
	}

	asm volatile(ALTERNATIVE(
		"msr	daifset, #3		// arch_local_irq_disable",
		__msr_s(SYS_ICC_PMR_EL1, "%0"),
		ARM64_HAS_IRQ_PRIO_MASKING)
		:
		: "r" ((unsigned long) GIC_PRIO_IRQOFF)
		: "memory");
}

/*
 * Save the current interrupt enable state.
 */
/*
 * IAMROOT, 2021.10.16:
 * daif 상태를 가져온다.
 */
static inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;

	asm volatile(ALTERNATIVE(
		"mrs	%0, daif",
		__mrs_s("%0", SYS_ICC_PMR_EL1),
		ARM64_HAS_IRQ_PRIO_MASKING)
		: "=&r" (flags)
		:
		: "memory");

	return flags;
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	int res;

	asm volatile(ALTERNATIVE(
		"and	%w0, %w1, #" __stringify(PSR_I_BIT),
		"eor	%w0, %w1, #" __stringify(GIC_PRIO_IRQON),
		ARM64_HAS_IRQ_PRIO_MASKING)
		: "=&r" (res)
		: "r" ((int) flags)
		: "memory");

	return res;
}

static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags;

	flags = arch_local_save_flags();

	/*
	 * There are too many states with IRQs disabled, just keep the current
	 * state if interrupts are already disabled/masked.
	 */
	if (!arch_irqs_disabled_flags(flags))
		arch_local_irq_disable();

	return flags;
}

/*
 * restore saved IRQ state
 */
static inline void arch_local_irq_restore(unsigned long flags)
{
	asm volatile(ALTERNATIVE(
		"msr	daif, %0",
		__msr_s(SYS_ICC_PMR_EL1, "%0"),
		ARM64_HAS_IRQ_PRIO_MASKING)
		:
		: "r" (flags)
		: "memory");

	pmr_sync();
}

#endif /* __ASM_IRQFLAGS_H */
