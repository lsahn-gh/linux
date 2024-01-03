/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hypervisor stub
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author:	Marc Zyngier <marc.zyngier@arm.com>
 */

#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/el2_setup.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/ptrace.h>
#include <asm/virt.h>

	.text
	.pushsection	.hyp.text, "ax"

	.align 11

/* IAMROOT, 2021.08.20:
 * - hyp exception vector table 정의.
 *
 * - ventry: vector entry
 *           exception handler로 branch 하도록 macro가 구성되어 있다.
 *           arch/arm64/include/asm/assembler.h 파일에 있음.
 *
 * - 아래 exception 케이스가 존재한다.
 *   1. nVHE: EL1 -> EL2
 *   2.  VHE: EL2 -> EL2
 *   따라서 위 2개의 케이스를 커버하기 위해 hvc instr 호출이 발생하면
 *   nVHE, VHE 모두 같은 handler(elx_sync)가 호출된다.
 *
 * http://egloos.zum.com/rousalome/v/10024114
 *
 * - exception
 *   https://developer.arm.com/documentation/100933/0100/Exception-handling
 * - exception vector table
 *   https://developer.arm.com/documentation/100933/0100/AArch64-exception-vector-table
 */
SYM_CODE_START(__hyp_stub_vectors)
	ventry	el2_sync_invalid		// Synchronous EL2t
	ventry	el2_irq_invalid			// IRQ EL2t
	ventry	el2_fiq_invalid			// FIQ EL2t
	ventry	el2_error_invalid		// Error EL2t

	ventry	elx_sync			// Synchronous EL2h
	ventry	el2_irq_invalid			// IRQ EL2h
	ventry	el2_fiq_invalid			// FIQ EL2h
	ventry	el2_error_invalid		// Error EL2h

	ventry	elx_sync			// Synchronous 64-bit EL1
	ventry	el1_irq_invalid			// IRQ 64-bit EL1
	ventry	el1_fiq_invalid			// FIQ 64-bit EL1
	ventry	el1_error_invalid		// Error 64-bit EL1

	ventry	el1_sync_invalid		// Synchronous 32-bit EL1
	ventry	el1_irq_invalid			// IRQ 32-bit EL1
	ventry	el1_fiq_invalid			// FIQ 32-bit EL1
	ventry	el1_error_invalid		// Error 32-bit EL1
SYM_CODE_END(__hyp_stub_vectors)

	.align 11

/* IAMROOT, 2022.02.08:
 * - 아래 모드에서 hvc 호출시 실행되는 handler. (synchronous exception)
 *   nVHE: EL1 -> EL2
 *    VHE: EL2 -> EL2
 */
SYM_CODE_START_LOCAL(elx_sync)
	cmp	x0, #HVC_SET_VECTORS
	b.ne	1f
	msr	vbar_el2, x1
	b	9f

/* IAMROOT, 2023.10.02:
 * - E2H bit clear가 되지 않는 예외 처리 요청시 mutate_to_vhe로 점프.
 */
1:	cmp	x0, #HVC_VHE_RESTART
	b.eq	mutate_to_vhe

2:	cmp	x0, #HVC_SOFT_RESTART
	b.ne	3f
	mov	x0, x2
	mov	x2, x4
	mov	x4, x1
	mov	x1, x3
	br	x4				// no return

3:	cmp	x0, #HVC_RESET_VECTORS
	beq	9f				// Nothing to reset!

	/* Someone called kvm_call_hyp() against the hyp-stub... */
	mov_q	x0, HVC_STUB_ERR
	eret

9:	mov	x0, xzr
	eret
SYM_CODE_END(elx_sync)

/* IAMROOT, 2022.02.08:
 * - vhe 전환.
 */
// nVHE? No way! Give me the real thing!
SYM_CODE_START_LOCAL(mutate_to_vhe)
/*
 * IAMROOT, 2022.02.08:
 * - mmu가 꺼져있는지, system에서 vhe를 지원하는지 확인한다.
 */
	// Sanity check: MMU *must* be off
	mrs	x1, sctlr_el2
	tbnz	x1, #0, 1f

	// Needs to be VHE capable, obviously
	mrs	x1, id_aa64mmfr1_el1
	ubfx	x1, x1, #ID_AA64MMFR1_VHE_SHIFT, #4
	cbz	x1, 1f

	// Check whether VHE is disabled from the command line
	adr_l	x1, id_aa64mmfr1_override
	ldr	x2, [x1, FTR_OVR_VAL_OFFSET]
	ldr	x1, [x1, FTR_OVR_MASK_OFFSET]
	ubfx	x2, x2, #ID_AA64MMFR1_VHE_SHIFT, #4
	ubfx	x1, x1, #ID_AA64MMFR1_VHE_SHIFT, #4
	cmp	x1, xzr
	and	x2, x2, x1
	csinv	x2, x2, xzr, ne
	cbnz	x2, 2f

1:	mov_q	x0, HVC_STUB_ERR
	eret
2:
/*
 * IAMROOT, 2022.02.08:
 * - VHE 활성화 관련 작업을 수행한다.
 */
	// Engage the VHE magic!
	mov_q	x0, HCR_HOST_VHE_FLAGS
	msr	hcr_el2, x0
	isb

	// Use the EL1 allocated stack, per-cpu offset
	mrs	x0, sp_el1
	mov	sp, x0
	mrs	x0, tpidr_el1
	msr	tpidr_el2, x0

	// FP configuration, vectors
	mrs_s	x0, SYS_CPACR_EL12
	msr	cpacr_el1, x0
	mrs_s	x0, SYS_VBAR_EL12
	msr	vbar_el1, x0

	// Use EL2 translations for SPE & TRBE and disable access from EL1
	mrs	x0, mdcr_el2
	bic	x0, x0, #(MDCR_EL2_E2PB_MASK << MDCR_EL2_E2PB_SHIFT)
	bic	x0, x0, #(MDCR_EL2_E2TB_MASK << MDCR_EL2_E2TB_SHIFT)
	msr	mdcr_el2, x0

	// Transfer the MM state from EL1 to EL2
	mrs_s	x0, SYS_TCR_EL12
	msr	tcr_el1, x0
	mrs_s	x0, SYS_TTBR0_EL12
	msr	ttbr0_el1, x0
	mrs_s	x0, SYS_TTBR1_EL12
	msr	ttbr1_el1, x0
	mrs_s	x0, SYS_MAIR_EL12
	msr	mair_el1, x0
	isb

/*
 * IAMROOT, 2022.02.08:
 * - el1에서 mode를 제외하곤 기존대로 쓴다. 모드만 el2로 변환한다.
 */
	// Hack the exception return to stay at EL2
	mrs	x0, spsr_el1
	and	x0, x0, #~PSR_MODE_MASK
	mov	x1, #PSR_MODE_EL2h
	orr	x0, x0, x1
	msr	spsr_el1, x0

	b	enter_vhe
SYM_CODE_END(mutate_to_vhe)

	// At the point where we reach enter_vhe(), we run with
	// the MMU off (which is enforced by mutate_to_vhe()).
	// We thus need to be in the idmap, or everything will
	// explode when enabling the MMU.

	.pushsection	.idmap.text, "ax"

/*
 * IAMROOT, 2022.02.08:
 * - vhe 진입.
 * - x0를 다쓰고 초기화하는게 보인다.
 * - eret을 통해 원래 hvc가 call했던 주소로 복귀한다.
 */
SYM_CODE_START_LOCAL(enter_vhe)
	// Invalidate TLBs before enabling the MMU
	tlbi	vmalle1
	dsb	nsh
	isb

	// Enable the EL2 S1 MMU, as set up from EL1
	mrs_s	x0, SYS_SCTLR_EL12
	set_sctlr_el1	x0

	// Disable the EL1 S1 MMU for a good measure
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr_s	SYS_SCTLR_EL12, x0

	mov	x0, xzr

	eret
SYM_CODE_END(enter_vhe)

	.popsection

.macro invalid_vector	label
SYM_CODE_START_LOCAL(\label)
	b \label
SYM_CODE_END(\label)
.endm

	invalid_vector	el2_sync_invalid
	invalid_vector	el2_irq_invalid
	invalid_vector	el2_fiq_invalid
	invalid_vector	el2_error_invalid
	invalid_vector	el1_sync_invalid
	invalid_vector	el1_irq_invalid
	invalid_vector	el1_fiq_invalid
	invalid_vector	el1_error_invalid

	.popsection

/*
 * __hyp_set_vectors: Call this after boot to set the initial hypervisor
 * vectors as part of hypervisor installation.  On an SMP system, this should
 * be called on each CPU.
 *
 * x0 must be the physical address of the new vector table, and must be
 * 2KB aligned.
 *
 * Before calling this, you must check that the stub hypervisor is installed
 * everywhere, by waiting for any secondary CPUs to be brought up and then
 * checking that is_hyp_mode_available() is true.
 *
 * If not, there is a pre-existing hypervisor, some CPUs failed to boot, or
 * something else went wrong... in such cases, trying to install a new
 * hypervisor is unlikely to work as desired.
 *
 * When you call into your shiny new hypervisor, sp_el2 will contain junk,
 * so you will need to set that to something sensible at the new hypervisor's
 * initialisation entry point.
 */

SYM_FUNC_START(__hyp_set_vectors)
	mov	x1, x0
	mov	x0, #HVC_SET_VECTORS
	hvc	#0
	ret
SYM_FUNC_END(__hyp_set_vectors)

SYM_FUNC_START(__hyp_reset_vectors)
	mov	x0, #HVC_RESET_VECTORS
	hvc	#0
	ret
SYM_FUNC_END(__hyp_reset_vectors)

/*
 * Entry point to switch to VHE if deemed capable
 */
/* IAMROOT, 2022.02.07:
 * - el1 -> el2
 * - el2로 boot 했지만 sys-regs 초기화했음에도 CurrentEL == el1 라면
 *   hypervisor 위에서 host-OS가 el1에서 동작하므로 그냥 return.
 *   그게 아니라면 el2 boot && CurrentEL == el2 이므로 kvm을 VHE 모드에서
 *   동작하도록 hvc-call 을 호출한다.
 *
 * - 예전 버전에서는 conf로 VHE를 enable/disable 할 수 있었지만 현재는
 *   H/W에서 VHE를 지원하면 default로 동작시키도록 한다.
 *   만약 VHE가 지원되는 H/W에서 nVHE로 동작시키고자 한다면 다음 bootargs를
 *   사용해야 한다. 'kvm-arm.mode=nvhe'
 *
 *   참고: git log -p 2d726d0db6ac479d91bf74490455badd34af6b1d
 */
SYM_FUNC_START(switch_to_vhe)
	// Need to have booted at EL2
	adr_l	x1, __boot_cpu_mode
	ldr	w0, [x1]
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f

	// and still be at EL1
	mrs	x0, CurrentEL
	cmp	x0, #CurrentEL_EL1
	b.ne	1f

	// Turn the world upside down
	mov	x0, #HVC_VHE_RESTART
	hvc	#0
1:
	ret
SYM_FUNC_END(switch_to_vhe)
