/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_SCS_H
#define _ASM_SCS_H

#ifdef __ASSEMBLY__

#include <asm/asm-offsets.h>

#ifdef CONFIG_SHADOW_CALL_STACK
	scs_sp	.req	x18
/*
 * IAMROOT, 2022.11.08:
 * - x18에 thread_info.scs_sp 를 load
 */
	.macro scs_load tsk
	ldr	scs_sp, [\tsk, #TSK_TI_SCS_SP]
	.endm
/*
 * IAMROOT, 2022.11.08:
 * - x18을 thread_ifo.scs_sp 로 넣기.
 */
	.macro scs_save tsk
	str	scs_sp, [\tsk, #TSK_TI_SCS_SP]
	.endm
#else
	.macro scs_load tsk
	.endm

	.macro scs_save tsk
	.endm
#endif /* CONFIG_SHADOW_CALL_STACK */

#endif /* __ASSEMBLY __ */

#endif /* _ASM_SCS_H */
