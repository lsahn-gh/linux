// SPDX-License-Identifier: GPL-2.0-only
/*
 * Low-level idle sequences
 */

#include <linux/cpu.h>
#include <linux/irqflags.h>

#include <asm/barrier.h>
#include <asm/cpuidle.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>

/*
 *	cpu_do_idle()
 *
 *	Idle the processor (wait for interrupt).
 *
 *	If the CPU supports priority masking we must do additional work to
 *	ensure that interrupts are not masked at the PMR (because the core will
 *	not wake up if we block the wake up signal in the interrupt controller).
 */
/*
 * IAMROOT. 2023.03.11:
 * - google-translate
 *   cpu_do_idle() 프로세서를 유휴 상태로 둡니다(인터럽트 대기).
 *
 *   CPU가 우선 순위
 *   마스킹을 지원하는 경우 PMR에서 인터럽트가 마스킹되지 않도록 추가 작업을 수행해야
 *   합니다(인터럽트 컨트롤러에서 깨우기 신호를 차단하면 코어가 깨어나지 않기
 *   때문입니다).
 */
void noinstr cpu_do_idle(void)
{
	struct arm_cpuidle_irq_context context;

	arm_cpuidle_save_irq_context(&context);

	dsb(sy);
	wfi();

	arm_cpuidle_restore_irq_context(&context);
}

/*
 * This is our default idle handler.
 */
void noinstr arch_cpu_idle(void)
{
	/*
	 * This should do all the clock switching and wait for interrupt
	 * tricks
	 */
	cpu_do_idle();
	raw_local_irq_enable();
}
