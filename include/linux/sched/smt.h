/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_SMT_H
#define _LINUX_SCHED_SMT_H

#include <linux/static_key.h>

#ifdef CONFIG_SCHED_SMT
extern struct static_key_false sched_smt_present;

/*
 * IAMROOT, 2023.05.30:
 * - arm6는 smt가 없으므로 무조건 false.
 *   cpu_smt_mask가 2로 증가 되면(sched_cpu_activate()) true,
 *   cpu_smt_mask가 2로 감소 되면(sched_cpu_deactivate()) false.
 */
static __always_inline bool sched_smt_active(void)
{
	return static_branch_likely(&sched_smt_present);
}
#else
static inline bool sched_smt_active(void) { return false; }
#endif

void arch_smt_update(void);

#endif
