/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MMU_CONTEXT_H
#define _LINUX_MMU_CONTEXT_H

#include <asm/mmu_context.h>
#include <asm/mmu.h>

/* Architectures that care about IRQ state in switch_mm can override this. */
/*
 * IAMROOT, 2023.01.28:
 * - @next로 mm교체.
 */
#ifndef switch_mm_irqs_off
# define switch_mm_irqs_off switch_mm
#endif

#ifndef leave_mm
static inline void leave_mm(int cpu) { }
#endif

/*
 * CPUs that are capable of running user task @p. Must contain at least one
 * active CPU. It is assumed that the kernel can run on all CPUs, so calling
 * this for a kernel thread is pointless.
 *
 * By default, we assume a sane, homogeneous system.
 */
#ifndef task_cpu_possible_mask
# define task_cpu_possible_mask(p)	cpu_possible_mask
# define task_cpu_possible(cpu, p)	true
#else
/*
 * IAMROOT, 2023.04.13:
 * - @p가 들어갈수있는 cpumask를 선택하고(일반적으로 cpu_possible_mask),
 *   그 mask에서 @cpu가 있는지 확인한다.
 *   있다면 return 1. 없다면 return 0.
 */
# define task_cpu_possible(cpu, p)	cpumask_test_cpu((cpu), task_cpu_possible_mask(p))
#endif

#endif
