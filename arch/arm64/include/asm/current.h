/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_CURRENT_H
#define __ASM_CURRENT_H

#include <linux/compiler.h>

#ifndef __ASSEMBLY__

struct task_struct;

/*
 * We don't use read_sysreg() as we want the compiler to cache the value where
 * possible.
 */
/*
 * IAMROOT, 2023.06.10:
 * - sp_el0 2가지 용도
 *   1. user app 관점
 *      el0 - user stack
 *      el1 - task_struct의 주소 저장
 *   2. kernel thread 관점
 *      el1 - task_struct의 주소 저장
 * - sp_el1 2가지 용도
 *   1. user app 관점
 *      el0 - N/A
 *      el1 - irq 또는 kernel 스택(init_task)을 가리킨다
 *   2. kernel thread 관점
 *      el1 - 자신의 커널 스택
 */
static __always_inline struct task_struct *get_current(void)
{
	unsigned long sp_el0;

	asm ("mrs %0, sp_el0" : "=r" (sp_el0));

	return (struct task_struct *)sp_el0;
}

#define current get_current()

#endif /* __ASSEMBLY__ */

#endif /* __ASM_CURRENT_H */

