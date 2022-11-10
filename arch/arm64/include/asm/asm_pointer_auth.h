/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_ASM_POINTER_AUTH_H
#define __ASM_ASM_POINTER_AUTH_H

#include <asm/alternative.h>
#include <asm/asm-offsets.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>

#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL

/*
 * IAMROOT, 2022.11.08:
 * - @tsk에서 thread.keys_kernel.apia의 lo, hi를 를 가져와서
 *   SYS_APIAKEYLO_EL1, SYS_APIAKEYHI_EL1에 넣는다.
 */
	.macro __ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
	mov	\tmp1, #THREAD_KEYS_KERNEL
	add	\tmp1, \tsk, \tmp1
	ldp	\tmp2, \tmp3, [\tmp1, #PTRAUTH_KERNEL_KEY_APIA]
	msr_s	SYS_APIAKEYLO_EL1, \tmp2
	msr_s	SYS_APIAKEYHI_EL1, \tmp3
	.endm

	.macro ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
alternative_if ARM64_HAS_ADDRESS_AUTH
	__ptrauth_keys_install_kernel_nosync \tsk, \tmp1, \tmp2, \tmp3
alternative_else_nop_endif
	.endm

	.macro ptrauth_keys_install_kernel tsk, tmp1, tmp2, tmp3
alternative_if ARM64_HAS_ADDRESS_AUTH
	__ptrauth_keys_install_kernel_nosync \tsk, \tmp1, \tmp2, \tmp3
	isb
alternative_else_nop_endif
	.endm

#else /* CONFIG_ARM64_PTR_AUTH_KERNEL */

	.macro __ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
	.endm

	.macro ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
	.endm

	.macro ptrauth_keys_install_kernel tsk, tmp1, tmp2, tmp3
	.endm

#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */

#ifdef CONFIG_ARM64_PTR_AUTH
/*
 * thread.keys_user.ap* as offset exceeds the #imm offset range
 * so use the base value of ldp as thread.keys_user and offset as
 * thread.keys_user.ap*.
 */
/*
 * IAMROOT, 2022.11.09:
 * - papago
 *   thread.keys_user.ap*는 오프셋이 #imm 오프셋 범위를 초과하므로 ldp의 기본 값을
 *   thread.keys_user로 사용하고 오프셋을 thread.keys_user.ap*으로 사용합니다.
 *
 * - @tsk에서 thread.keys_user.apia의 lo, hi를 를 가져와서
 *   SYS_APIAKEYLO_EL1, SYS_APIAKEYHI_EL1에 넣는다.
 */
	.macro __ptrauth_keys_install_user tsk, tmp1, tmp2, tmp3
	mov	\tmp1, #THREAD_KEYS_USER
	add	\tmp1, \tsk, \tmp1
	ldp	\tmp2, \tmp3, [\tmp1, #PTRAUTH_USER_KEY_APIA]
	msr_s	SYS_APIAKEYLO_EL1, \tmp2
	msr_s	SYS_APIAKEYHI_EL1, \tmp3
	.endm

	.macro __ptrauth_keys_init_cpu tsk, tmp1, tmp2, tmp3
	mrs	\tmp1, id_aa64isar1_el1
	ubfx	\tmp1, \tmp1, #ID_AA64ISAR1_APA_SHIFT, #8
	cbz	\tmp1, .Lno_addr_auth\@
	mov_q	\tmp1, (SCTLR_ELx_ENIA | SCTLR_ELx_ENIB | \
			SCTLR_ELx_ENDA | SCTLR_ELx_ENDB)
	mrs	\tmp2, sctlr_el1
	orr	\tmp2, \tmp2, \tmp1
	msr	sctlr_el1, \tmp2
	__ptrauth_keys_install_kernel_nosync \tsk, \tmp1, \tmp2, \tmp3
	isb
.Lno_addr_auth\@:
	.endm

	.macro ptrauth_keys_init_cpu tsk, tmp1, tmp2, tmp3
alternative_if_not ARM64_HAS_ADDRESS_AUTH
	b	.Lno_addr_auth\@
alternative_else_nop_endif
	__ptrauth_keys_init_cpu \tsk, \tmp1, \tmp2, \tmp3
.Lno_addr_auth\@:
	.endm

#else /* !CONFIG_ARM64_PTR_AUTH */

	.macro ptrauth_keys_install_user tsk, tmp1, tmp2, tmp3
	.endm

#endif /* CONFIG_ARM64_PTR_AUTH */

#endif /* __ASM_ASM_POINTER_AUTH_H */
