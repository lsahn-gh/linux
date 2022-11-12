/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fallback per-CPU frame pointer holder
 *
 * Copyright (C) 2006 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#ifndef _ASM_GENERIC_IRQ_REGS_H
#define _ASM_GENERIC_IRQ_REGS_H

#include <linux/percpu.h>

/*
 * Per-cpu current frame pointer - the location of the last exception frame on
 * the stack
 */
/*
 * IAMROOT, 2022.11.12:
 * - 현재 실행중인 irq context register.
 */
DECLARE_PER_CPU(struct pt_regs *, __irq_regs);

static inline struct pt_regs *get_irq_regs(void)
{
	return __this_cpu_read(__irq_regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - @new_regs를 현재 regs에 넣으면서, old를 return한다.
 * - 전역 percpu __irq_regs에 현재 실행중인 irq의 regs를 저장해놓는다.
 */
static inline struct pt_regs *set_irq_regs(struct pt_regs *new_regs)
{
	struct pt_regs *old_regs;

	old_regs = __this_cpu_read(__irq_regs);
	__this_cpu_write(__irq_regs, new_regs);
	return old_regs;
}

#endif /* _ASM_GENERIC_IRQ_REGS_H */
