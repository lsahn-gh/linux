/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level exception handling code
 *
 * Copyright (C) 2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/alternative.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/asm_pointer_auth.h>
#include <asm/bug.h>
#include <asm/cpufeature.h>
#include <asm/errno.h>
#include <asm/esr.h>
#include <asm/irq.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/scs.h>
#include <asm/thread_info.h>
#include <asm/asm-uaccess.h>
#include <asm/unistd.h>

/*
 * IAMROOT, 2022.11.08:
 * - x0 ~ x29까지 mov xN, xzr
 */
	.macro	clear_gp_regs
	.irp	n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
	mov	x\n, xzr
	.endr
	.endm

/*
 * IAMROOT, 2022.02.08:
 * 인자로 만들어지는 branch 함수들은 entry_handler macro에 의해 만들어지는
 * 함수들이며 결국 해당 함수에서 _handler가 붙은 함수를 호출한다.
 * ex) el1h_64_sync -> el1h_64_sync_handler
 */
/*
 * IAMROOT, 2022.11.07:
 * - macro 정리
 *   el ht regsize label
 *   1  t  64      sync	
 *   1  t  64      irq	
 *   1  t  64      fiq	
 *   1  t  64      error
 *                
 *   1  h  64      sync	
 *   1  h  64      irq		
 *   1  h  64      fiq		
 *   1  h  64      error	
 *                
 *   0  t  64      sync
 *   0  t  64      irq	
 *   0  t  64      fiq	
 *   0  t  64      error
 *                
 *   0  t  32      sync
 *   0  t  32      irq		
 *   0  t  32      fiq		
 *   0  t  32      error		
 * - ex) el1 64bit irq발생시 흐름
 *   vectors
 *     v
 *   kernel_ventry 1, h, 64, irq  <--현재 여기
 *     v
 *   el1h_64_irq
 *     v
 *   kernel_entry 1, 64
 *     v
 *   el1h_64_irq_handler
 *     v
 *   (chip -> flow -> irq handler 수행후 복귀)
 *     v
 *   ret_to_kernel
 *
 * - irq 흐름.
 *   vectors(arch/arm64/kernel/entry.S)
 *     |
 *     |(kernel_ventry) <--현재 여기
 *     |(entry_handler)
 *     |(el0_interrupt or el1_interrupt)
 *     |(do_interrupt_handler) 
 *     |(handler_arch_irq 실행) 
 *     v
 *   chip handler(handler_arch_irq, gic의 경우 gic_handle_irq)
 *     v
 *   flow handler(handle_fasteoi_nmi()등)
 *     v
 *   irq_handler(user isr)
 */
	.macro kernel_ventry, el:req, ht:req, regsize:req, label:req
	.align 7
/*
 * IAMROOT, 2022.11.07:
 * - UNMAP_KERNEL_AT_EL0
 *   bool "Unmap kernel when running in userspace (aka \"KAISER\")" if EXPERT
 *   default y
 *   help
 *   Speculation attacks against some high-performance processors can
 *   be used to bypass MMU permission checks and leak kernel data to
 *   userspace. This can be defended against by unmapping the kernel
 *   when running in userspace, mapping it back in on exception entry
 *   via a trampoline page in the vector table.  
 *   -- papago
 *   일부 고성능 프로세서에 대한 추측 공격은 MMU 권한 검사를 우회하고 커널
 *   데이터를 사용자 공간으로 유출하는 데 사용할 수 있습니다. 이것은 사용자
 *   공간에서 실행될 때 커널을 매핑 해제하고 벡터 테이블의 트램폴린 페이지를
 *   통해 예외 항목에 다시 매핑하여 방어할 수 있습니다.
 *
 *   user 영역에서 kernel영역이 보이지 않게 unmap을 하겠다는것.
 */
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * IAMROOT, 2022.11.07:
 * - tpidrro_el0은 원래용도로 사용안하고 scratch로 사용한다.
 * - CONFIG_UNMAP_KERNEL_AT_EL0을 사용했다는건 exception이 tramp_ventry
 *   를 통해서 진입을 한 상태고 tramp_ventry에서 tpidrro_el0에
 *   x30(lr)을 넣어놓은 상태이다. 해당 값을 x30으로 다시 가져오고
 *   tpidrro_el0를 지워놓는다.
 *
 * - tpidrro_el0의 원래 용도.
 *   TPIDRRO_EL0, EL0 Read-Only Software Thread ID Register
 *   참고) https://developer.arm.com/documentation/ddi0601/2021-12/AArch64-Registers/TPIDRRO-EL0--EL0-Read-Only-Software-Thread-ID-Register
 *   Provides a location where software executing at EL1 or higher can store
 *   thread identifying information that is visible to software executing
 *   at EL0, for OS management purposes.
 *
 *   The PE makes no use of this register.
 *   -- papago
 *   OS 관리 목적으로 EL1 이상에서 실행되는 소프트웨어가 EL0에서 실행하는
 *   소프트웨어에 표시되는 스레드 식별 정보를 저장할 수 있는 위치를 제공합니다.
 *   PE는 이 레지스터를 사용하지 않습니다.
 *
 *   -- Bits [63:0]
 *   Thread ID. Thread identifying information stored by software running at
 *   this Exception level.
 *
 */
	.if	\el == 0
alternative_if ARM64_UNMAP_KERNEL_AT_EL0
	.if	\regsize == 64
	mrs	x30, tpidrro_el0
	msr	tpidrro_el0, xzr
	.else
	mov	x30, xzr
	.endif
alternative_else_nop_endif
	.endif
#endif

/*
 * IAMROOT, 2022.11.05: 
 * context save(모든 레지스터)를 위해 struct pt_regs 구조체 크기만 스택을 확보한다.
 */
	sub	sp, sp, #PT_REGS_SIZE
#ifdef CONFIG_VMAP_STACK
	/*
	 * Test whether the SP has overflowed, without corrupting a GPR.
	 * Task and IRQ stacks are aligned so that SP & (1 << THREAD_SHIFT)
	 * should always be zero.
	 */
/*
 * IAMROOT, 2022.11.07:
 * -papago
 *  GPR을 손상시키지 않고 SP가 오버플로되었는지 테스트합니다.
 *  Task 및 IRQ 스택은 SP & (1 << THREAD_SHIFT)가 항상 0이 되도록 정렬됩니다.
 *
 * - x0에 stack size를 구한다. stack size는 THREAD_SHIFT로 정렬되잇다.
 *   THREAD_SHIFT bit가 깨지면 align이 깨졌다고 러프하게 판단한다.
 *
 * - sp <-> x0값을 교환해서 sp를 검사하는 방식으로 수행한다.
 *   register끼리의 교환이므로 속도가 빠르다.
 *
 * - add sp, sp, x0 --> sp : sp(old) + x0(old)
 *   sub x0, sp, x0 --> x0 : sp - x0(old) = sp(old) + x0(old) - x0(old) = sp(old).
 *                      x0에 sp(old) 위치완료.
 *   tbnz	x0, #THREAD_SHIFT, 0f --> x0(old sp) 의 THREAD_SHIFT bit 검사.
 *                                    0가 아니면 깨진것으로 판단
 *   sub x0, sp, x0 --> x0 : sp - x0 = (sp(old) + x0(old)) - sp(old) = x0(old)
 *                      x0원복 완료
 *   sub sp, sp, x0 --> sp : sp - x0(old) = sp(old) + x0(old) - x0(old)
 *                      sp 원복 완료
 */
	add	sp, sp, x0			// sp' = sp + x0
	sub	x0, sp, x0			// x0' = sp' - x0 = (sp + x0) - x0 = sp
	tbnz	x0, #THREAD_SHIFT, 0f
	sub	x0, sp, x0			// x0'' = sp' - x0' = (sp + x0) - sp = x0
	sub	sp, sp, x0			// sp'' = sp' - x0 = (sp + x0) - x0 = sp
/*
 * IAMROOT, 2022.11.07:
 * - entry_handler 참고
 *   el1t_64_sync	
 *   el1t_64_irq	
 *   el1t_64_fiq	
 *   el1t_64_error
 *  
 *   el1h_64_sync	
 *   el1h_64_irq		
 *   el1h_64_fiq		
 *   el1h_64_error	
 *  
 *   el0t_64_sync
 *   el0t_64_irq	
 *   el0t_64_fiq	
 *   el0t_64_error
 *  
 *   el0t_32_sync
 *   el0t_32_irq		
 *   el0t_32_fiq		
 *   el0t_32_error		
 *
 * - ex) el1 64bit irq발생시 flow
 *   vectors 
 *     v
 *   kernel_ventry 1, h, 64, irq
 *     v
 *   el1h_64_irq <-- 여기로 이제 진입.
 *     v
 *   kernel_entry 1, 64
 *     v
 *   el1h_64_irq_handler
 *     v
 *   ret_to_kernel
 */
	b	el\el\ht\()_\regsize\()_\label

0:
	/*
	 * Either we've just detected an overflow, or we've taken an exception
	 * while on the overflow stack. Either way, we won't return to
	 * userspace, and can clobber EL0 registers to free up GPRs.
	 */

	/* Stash the original SP (minus PT_REGS_SIZE) in tpidr_el0. */
/*
 * IAMROOT, 2022.11.07:
 * - papago
 *   방금 오버플로를 감지했거나 오버플로 스택에서 예외가 발생했습니다.
 *   어느 쪽이든, 우리는 사용자 공간으로 돌아가지 않을 것이며 GPR을 해제하기
 *   위해 EL0 레지스터를 방해할 수 있습니다.
 *
 *   원래 SP(PT_REGS_SIZE 빼기)를 tpidr_el0에 숨깁니다.
 * - 여기 라벨이 온시점에 x0엔 sp가 있을것이다. 그걸 tpidr_el0로 넣어놓는다.
 */
	msr	tpidr_el0, x0

	/* Recover the original x0 value and stash it in tpidrro_el0 */
/*
 * IAMROOT, 2022.11.07:
 * - x0도 원래의 x0값으로 원복해주고 tpidrro_el0에 넣어놓는다.
 */
	sub	x0, sp, x0
	msr	tpidrro_el0, x0

	/* Switch to the overflow stack */
/*
 * IAMROOT, 2022.11.07:
 * - adr_this_cpu
 *   arch/arm64/include/asm/stacktrace.h 에 pcpu로 overflow_stack이 정의되있다.
 *   현재 cpu에 대한 pcpu address를 가져온다.
 * - overflow_stack + OVERFLOW_STACK_SIZE 의 현재 cpu에 대한 pcpu address를
 *   가져와 sp로 사용한다.
 *   x0는 임시변수로 쓴다.
 */
	adr_this_cpu sp, overflow_stack + OVERFLOW_STACK_SIZE, x0

	/*
	 * Check whether we were already on the overflow stack. This may happen
	 * after panic() re-enables interrupts.
	 */
/*
 * IAMROOT, 2022.11.07:
 * - tpider_el0에 저장되있는 old sp를 가져와서 현재 sp와 비교한다
 *   OVERFLOW_STACK_SIZE보다 차이가 안난다면 overflow stack처리과정에서 다시한번
 *   overflow stack처리로 진입하게 된것이다.
 *
 * - tst is 0 => 이미 overflow_stack 진입한 상태. __bad_stack으로
 *               안가고 entry_handler로 간다. 어짜피 곧 죽을거 적당히 처리하는듯.
 *   not 0 => overflow_stack으로 진입하기 시작. __bad_stack으로 이동한다.
 */
	mrs	x0, tpidr_el0			// sp of interrupted context
	sub	x0, sp, x0			// delta with top of overflow stack
	tst	x0, #~(OVERFLOW_STACK_SIZE - 1)	// within range?
	b.ne	__bad_stack			// no? -> bad stack pointer

	/* We were already on the overflow stack. Restore sp/x0 and carry on. */
	sub	sp, sp, x0
	mrs	x0, tpidrro_el0
#endif
/*
 * IAMROOT, 2022.11.05: 
 * 예) el0 64bit user -----(irq)----> b el0t_64_irq
 * entry_handler 참고
 */

	b	el\el\ht\()_\regsize\()_\label
	.endm

/*
 * IAMROOT, 2022.11.09:
 * - TRAMP_VALIAS + offset으로  sym의 주소를 얻어온다.
 */
	.macro tramp_alias, dst, sym
	mov_q	\dst, TRAMP_VALIAS
	add	\dst, \dst, #(\sym - .entry.tramp.text)
	.endm

	/*
	 * This macro corrupts x0-x3. It is the caller's duty  to save/restore
	 * them if required.
	 */
/*
 * IAMROOT, 2022.11.08:
 * - SSBD(Speculative Store Bypass Disabl)
 * - SKIP
 */
	.macro	apply_ssbd, state, tmp1, tmp2
alternative_cb	spectre_v4_patch_fw_mitigation_enable
	b	.L__asm_ssbd_skip\@		// Patched to NOP
alternative_cb_end
	ldr_this_cpu	\tmp2, arm64_ssbd_callback_required, \tmp1
	cbz	\tmp2,	.L__asm_ssbd_skip\@
	ldr	\tmp2, [tsk, #TSK_TI_FLAGS]
	tbnz	\tmp2, #TIF_SSBD, .L__asm_ssbd_skip\@
	mov	w0, #ARM_SMCCC_ARCH_WORKAROUND_2
	mov	w1, #\state
alternative_cb	spectre_v4_patch_fw_mitigation_conduit
	nop					// Patched to SMC/HVC #0
alternative_cb_end
.L__asm_ssbd_skip\@:
	.endm

	/* Check for MTE asynchronous tag check faults */
/*
 * IAMROOT, 2022.11.08:
 * - async tag check fault mode 인지 확인.
 *   맞다면 SYS_TFSR_EL1_TF0_SHIFT를 검사했는지 확인하고 발생했으면
 *   ti-flags에 _TIF_MTE_ASYNC_FAULT를 추가한다.
 *
 * - tcf(tag check fault)
 *   Documentation/arm64/memory-tagging-extension.rst
 * - SCTLR_EL1_TCF0_SHIFT + 1가 set됬으면
 *   Tag Check Faults are asynchronously accumulated.
 */
	.macro check_mte_async_tcf, tmp, ti_flags, thread_sctlr
#ifdef CONFIG_ARM64_MTE
	.arch_extension lse
alternative_if_not ARM64_MTE
	b	1f
alternative_else_nop_endif
	/*
	 * Asynchronous tag check faults are only possible in ASYNC (2) or
	 * ASYM (3) modes. In each of these modes bit 1 of SCTLR_EL1.TCF0 is
	 * set, so skip the check if it is unset.
	 */
/*
 * IAMROOT, 2022.11.08:
 * - papago
 *   비동기식 태그 검사 오류는 ASYNC(2) 또는 ASYM(3) 모드에서만 가능합니다.
 *   이러한 각 모드에서 SCTLR_EL1.TCF0의 비트 1이 설정되므로 설정되지 않은 경우
 *   확인을 건너뜁니다.
 * - async tcf mode인지 확인한다. 아니면 1f로 이동하고 종료
 */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	mrs_s	\tmp, SYS_TFSRE0_EL1
/*
 * IAMROOT, 2022.11.08:
 * - SYS_TFSR_EL1_TF0_SHIFT를 검사한다. set되있다면 async tcf가 발생한것이므로
 *   ti_flags에 _TIF_MTE_ASYNC_FAULT flag bit를 추가해준다. 이는 추후 처리된다.
 */
	tbz	\tmp, #SYS_TFSR_EL1_TF0_SHIFT, 1f
	/* Asynchronous TCF occurred for TTBR0 access, set the TI flag */
	mov	\tmp, #_TIF_MTE_ASYNC_FAULT
	add	\ti_flags, tsk, #TSK_TI_FLAGS
/*
 * IAMROOT, 2022.11.08:
 * - stset
 *  ti_flags |= tmp
 *  https://developer.arm.com/documentation/dui0801/g/A64-Data-Transfer-Instructions/STSET--STSETL--STSETL
 */
	stset	\tmp, [\ti_flags]
1:
#endif
	.endm

	/* Clear the MTE asynchronous tag check faults */
/*
 * IAMROOT, 2022.11.09:
 * - async2, async3 mode인지 판단하여 clear
 */
	.macro clear_mte_async_tcf thread_sctlr
#ifdef CONFIG_ARM64_MTE
alternative_if ARM64_MTE
	/* See comment in check_mte_async_tcf above. */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	dsb	ish
	msr_s	SYS_TFSRE0_EL1, xzr
1:
alternative_else_nop_endif
#endif
	.endm

	.macro mte_set_gcr, mte_ctrl, tmp
#ifdef CONFIG_ARM64_MTE
	ubfx	\tmp, \mte_ctrl, #MTE_CTRL_GCR_USER_EXCL_SHIFT, #16
	orr	\tmp, \tmp, #SYS_GCR_EL1_RRND
	msr_s	SYS_GCR_EL1, \tmp
#endif
	.endm

/*
 * IAMROOT, 2022.11.08:
 * - kernel의 gcr 설정값을 hw에 적용한다.
 */
	.macro mte_set_kernel_gcr, tmp, tmp2
#ifdef CONFIG_KASAN_HW_TAGS
alternative_if_not ARM64_MTE
	b	1f
alternative_else_nop_endif
	mov	\tmp, KERNEL_GCR_EL1
	msr_s	SYS_GCR_EL1, \tmp
1:
#endif
	.endm

/*
 * IAMROOT, 2022.11.09:
 * - user(tsk)의 gcr 설정값을 hw에 적용한다.
 */
	.macro mte_set_user_gcr, tsk, tmp, tmp2
#ifdef CONFIG_ARM64_MTE
alternative_if_not ARM64_MTE
	b	1f
alternative_else_nop_endif
	ldr	\tmp, [\tsk, #THREAD_MTE_CTRL]

	mte_set_gcr \tmp, \tmp2
1:
#endif
	.endm

/*
 * IAMROOT, 2022.11.05: 
 * - @x0 pt_regs 저장할 주소
 * exception이 발생하여 context 를 백업한다. (전체 레지스터)
 * 그 외 mte, ptr_auth, hw debuger 관련 동작 등을 체크한다.
 */
	.macro	kernel_entry, el, regsize = 64
/*
 * IAMROOT, 2022.11.05: 
 * 32bit 유저에서 진입한 경우 x0의 상위 32비트를 클리어한다.
 * - pt_regs 상태
 *   regs[0~28] = registers
 *   regs[29]   =  x29(fp)
 */
	.if	\regsize == 32
	mov	w0, w0				// zero upper 32 bits of x0
	.endif
	stp	x0, x1, [sp, #16 * 0]
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]

/*
 * IAMROOT, 2022.11.08:
 * - el0
 */
	.if	\el == 0
/*
 * IAMROOT, 2022.11.08:
 * 1. x0~x29 clear
 * 2. sp_el0를 x21에 backup
 * 3. tsk(x29), sp_el0 에 pcp __entry_task(armch/arm64/process.c) 를 가져온다.
 *   (pcp task_struct *)
 * - sp_el0
 *   user에서 kernel mode를 진입할때 current task를 저장하고 kernel mode에서는
 *   현재 thread_info를 빠르게 알아내는 용도로 사용한다.
 * 4. ss(debug single step), MTE, PAC, SCS, SSBD 처리
 *
*/
	clear_gp_regs
	mrs	x21, sp_el0
	ldr_this_cpu	tsk, __entry_task, x20
	msr	sp_el0, tsk

	/*
	 * Ensure MDSCR_EL1.SS is clear, since we can unmask debug exceptions
	 * when scheduling.
	 */
	ldr	x19, [tsk, #TSK_TI_FLAGS]
/*
 * IAMROOT, 2022.11.09:
 * - ret_to_user등에서 다시 enable한다.
 */
	disable_step_tsk x19, x20

	/* Check for asynchronous tag check faults in user space */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	check_mte_async_tcf x22, x23, x0

/*
 * IAMROOT, 2022.11.08:
 * - PAC(Pointer Authentication)
 *   Documentation/arm64/pointer-authentication.rst
 *   https://www.kernel.org/doc/html/latest/arm64/pointer-authentication.html
 *   https://community.arm.com/arm-community-blogs/b/tools-software-ides-blog/posts/code-reuse-attacks-the-compiler-story
 * - ENAI
 *   Controls enabling of pointer authentication (using the APIAKey_EL1 key)
 *   of instruction addresses in the EL1&0 translation regime.
 */
#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * Enable IA for in-kernel PAC if the task had it disabled. Although
	 * this could be implemented with an unconditional MRS which would avoid
	 * a load, this was measured to be slower on Cortex-A75 and Cortex-A76.
	 *
	 * Install the kernel IA key only if IA was enabled in the task. If IA
	 * was disabled on kernel exit then we would have left the kernel IA
	 * installed so there is no need to install it again.
	 */
/*
 * IAMROOT, 2022.11.08:
 * - papago
 *   task에서 비활성화된 경우 커널 내 PAC에 대해 IA를 활성화합니다. 이것은
 *   부하를 피하는 무조건적인 MRS로 구현할 수 있지만 Cortex-A75 및
 *   Cortex-A76에서 더 느린 것으로 측정되었습니다.
 *
 *   task에서 IA가 활성화된 경우에만 커널 IA 키를 설치합니다. kernel exit시
 *   IA가 비활성화된 경우 커널 IA가 설치된 상태로남아 있으므로 다시 설치할
 *   필요가 없습니다.
 * - PAC IA가 disable되있다면 install한다. 
 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_kernel_nosync tsk, x20, x22, x23
	b	2f
1:
	mrs	x0, sctlr_el1
	orr	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	apply_ssbd 1, x22, x23

	mte_set_kernel_gcr x22, x23

	/*
	 * Any non-self-synchronizing system register updates required for
	 * kernel entry should be placed before this point.
	 */
/*
 * IAMROOT, 2022.11.08:
 * - MTE, PAC 둘중하나라도 사용중이라면 isb를 한번 하겠다는것.
 */
alternative_if ARM64_MTE
	isb
	b	1f
alternative_else_nop_endif
alternative_if ARM64_HAS_ADDRESS_AUTH
	isb
alternative_else_nop_endif
1:

	scs_load tsk
/*
 * IAMROOT, 2022.11.08:
 * - el1
 *   1. el0와는 다르게 register clear를 안한다.
 *   2. sp를 x21에 백업한다.
 *   3. task를 current task로 그대로 사용한다.
 */
	.else
	add	x21, sp, #PT_REGS_SIZE
	get_current_task tsk
	.endif /* \el == 0 */
/*
 * IAMROOT, 2022.11.08:
 * - elr_el1, spsr_el1을 x22, x23에 백업한다.
 * - lr, x21(backup sp)를 sp(pt_regs.regs[30], pt_regs.sp)에 backup한다.
 * - pt_regs 상태
 *   regs[0~28] = registers
 *   regs[29] =  x29(fp)
 *   regs[30] = lr
 *   sp = sp(before) + PT_REGS_SIZE
 */
	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]

	/*
	 * For exceptions from EL0, create a final frame record.
	 * For exceptions from EL1, create a synthetic frame record so the
	 * interrupted code shows up in the backtrace.
	 */
/*
 * IAMROOT, 2022.11.08:
 * - papago
 *   EL0의 예외에 대해 최종 프레임 레코드를 생성합니다. 
 *   EL1 예외의 경우 중단된 코드가 역추적에 표시되도록 합성 프레임 레코드를
 *   만듭니다.
 *
 * - el1인 경우 fp, elr_el1이 backup될것이다.
 * - pt_regs 상태
 *   regs[0~28]    = registers
 *   regs[29]      = x29(fp)
 *   regs[30]      = lr
 *   sp            = sp(before) + PT_REGS_SIZE
 *   stackframe[0] = el0: 0, el1: fp
 *   stackframe[1] = el0: 0, el1: elr_el1
 */
	.if \el == 0
	stp	xzr, xzr, [sp, #S_STACKFRAME]
	.else
	stp	x29, x22, [sp, #S_STACKFRAME]
	.endif
	add	x29, sp, #S_STACKFRAME

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
/*
 * IAMROOT, 2022.11.08:
 * - el1인 경우 이미 disabled 됬을 경우 x23(backup spsr)에 PSR_PAN_BIT가
 *   추가될 수 있다.
 */
	bl	__swpan_entry_el\el
alternative_else_nop_endif
#endif
/*
 * IAMROOT, 2022.11.08:
 * - pt_regs 상태
 *   regs[0~28]    = registers
 *   regs[29]      = x29(fp)
 *   regs[30]      = lr
 *   sp            = sp(before) + PT_REGS_SIZE
 *   stackframe[0] = el0: 0, el1: fp
 *   stackframe[1] = el0: 0, el1: elr_el1
 *   pc            = elr_el1
 *   pstate        = spsr
 */
	stp	x22, x23, [sp, #S_PC]

	/* Not in a syscall by default (el0_svc overwrites for real syscall) */
	.if	\el == 0
	mov	w21, #NO_SYSCALL
	str	w21, [sp, #S_SYSCALLNO]
	.endif
/*
 * IAMROOT, 2022.11.08:
 * - pt_regs 상태
 *   regs[0~28]    = registers
 *   regs[29]      = x29(fp)
 *   regs[30]      = lr
 *   sp            = sp + PT_REGS_SIZE
 *   stackframe[0] = el0: 0, el1: fp
 *   stackframe[1] = el0: 0, el1: elr_el1
 *   pc            = elr_el1
 *   pstate        = spsr
 *   syscallno     = el0 : -1
 */

	/* Save pmr */
alternative_if ARM64_HAS_IRQ_PRIO_MASKING
	mrs_s	x20, SYS_ICC_PMR_EL1
/*
 * IAMROOT, 2022.11.08:
 * - pt_regs 상태
 *   regs[0~28]    = registers
 *   regs[29]      = x29(fp)
 *   regs[30]      = lr
 *   sp            = sp + PT_REGS_SIZE
 *   stackframe[0] = el0: 0, el1: fp
 *   stackframe[1] = el0: 0, el1: elr_el1
 *   pc            = elr_el1
 *   pstate        = spsr
 *   syscallno     = el0 : -1
 *   pmr_save      = SYS_ICC_PMR_EL1(before)
 */
	str	x20, [sp, #S_PMR_SAVE]
/*
 * IAMROOT, 2022.11.08:
 * - git blame 참고
 *   debug시 경고문때문인듯 하며 local_daif_save와 일관성을 유지한다.
 */
	mov	x20, #GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET
	msr_s	SYS_ICC_PMR_EL1, x20
alternative_else_nop_endif

	/* Re-enable tag checking (TCO set on exception entry) */
#ifdef CONFIG_ARM64_MTE
/*
 * IAMROOT, 2022.11.09:
 * - set 0 를 함으로써 enable시킨다.
 */
alternative_if ARM64_MTE
	SET_PSTATE_TCO(0)
alternative_else_nop_endif
#endif

	/*
	 * Registers that may be useful after this macro is invoked:
	 *
	 * x20 - ICC_PMR_EL1
	 * x21 - aborted SP
	 * x22 - aborted PC
	 * x23 - aborted PSTATE
	*/
	.endm

/*
 * IAMROOT, 2022.11.05: 
 * exceptino 처리 후 커널 또는 유저로 복귀하기 전에 수행할 루틴
 * - kernel_entry에서 백업해뒀던 값들을 복구한다.
 */
	.macro	kernel_exit, el
/*
 * IAMROOT, 2022.11.05: 
 * 커널로 북귀하는 경우 DAIF 플래그를 모두 disable 한다.
 * el0의 경우 handler에서 이미 disable했었다.
 */
	.if	\el != 0
	disable_daif
	.endif

/*
 * IAMROOT, 2022.11.05: 
 * kernel_entry에서 backup해두었던 pmr을 다시 복구한다.
 */
	/* Restore pmr */
alternative_if ARM64_HAS_IRQ_PRIO_MASKING
	ldr	x20, [sp, #S_PMR_SAVE]
	msr_s	SYS_ICC_PMR_EL1, x20
	mrs_s	x21, SYS_ICC_CTLR_EL1
	tbz	x21, #6, .L__skip_pmr_sync\@	// Check for ICC_CTLR_EL1.PMHE
	dsb	sy				// Ensure priority change is seen by redistributor
.L__skip_pmr_sync\@:
alternative_else_nop_endif

	ldp	x21, x22, [sp, #S_PC]		// load ELR, SPSR

/*
 * IAMROOT, 2022.11.05: 
 * ttbr0를 조작하는 방법으로 즉, sw emulation 방법으로 커널에서 유저 영역의
 * 가상 주소에 접근하지 못하게 한다.
 * (최신 armv8에는 hardware 방식의 PAN 방식을 지원한다)
 */
#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
	bl	__swpan_exit_el\el
alternative_else_nop_endif
#endif

/*
 * IAMROOT, 2022.11.05: 
 * 유저용 sp를 복구한다.
 */

	.if	\el == 0
	ldr	x23, [sp, #S_SP]		// load return stack pointer
	msr	sp_el0, x23
/*
 * IAMROOT, 2022.11.10:
 * - x22에 #PSR_MODE32_BIT set.
 *   tst => 1 => Z 0 => b.eq동작안함
 * - x22에 #PSR_MODE32_BIT unset
 *   tst => 0 => Z 1 => b.eq동작.
 */
	tst	x22, #PSR_MODE32_BIT		// native task?
	b.eq	3f

/*
 * IAMROOT, 2022.11.05: 
 * 32bit 유저로 돌아가는 경우에만 workaround 코드를 수행한다.
 */
#ifdef CONFIG_ARM64_ERRATUM_845719
alternative_if ARM64_WORKAROUND_845719
#ifdef CONFIG_PID_IN_CONTEXTIDR
	mrs	x29, contextidr_el1
	msr	contextidr_el1, x29
#else
	msr contextidr_el1, xzr
#endif
alternative_else_nop_endif
#endif
3:
/*
 * IAMROOT, 2022.11.05: 
 * shadow call stack 커널 옵션을 사용하는 경우엔 관련 정보도 복구한다.
 */
	scs_save tsk

/*
 * IAMROOT, 2022.11.05: 
 * mte 관련
 */
	/* Ignore asynchronous tag check faults in the uaccess routines */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	clear_mte_async_tcf x0

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * IA was enabled for in-kernel PAC. Disable it now if needed, or
	 * alternatively install the user's IA. All other per-task keys and
	 * SCTLR bits were updated on task switch.
	 *
	 * No kernel C function calls after this.
	 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_user tsk, x0, x1, x2
	b	2f
1:
	mrs	x0, sctlr_el1
	bic	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	mte_set_user_gcr tsk, x0, x1

	apply_ssbd 0, x0, x1
	.endif

/*
 * IAMROOT, 2022.11.05: 
 * 나머지 레지스터들을 복구한다.
 */
	msr	elr_el1, x21			// set up the return data
	msr	spsr_el1, x22
	ldp	x0, x1, [sp, #16 * 0]
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]
	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE		// restore sp

	.if	\el == 0
alternative_insn eret, nop, ARM64_UNMAP_KERNEL_AT_EL0
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * IAMROOT, 2022.11.09:
 * - el0
 *   ARM64_UNMAP_KERNEL_AT_EL0가 없으면 eret을 하며 user로 복귀.
 *   그게 아니면 nop가 대신 한번 실행되면서 해당 처리를 해준다.
 *   kernel_ventry에서 64bit인경우 x30에 tramp_ventry에서 저장해놨던 lr,
 *   32bit인경우 0를 저장해 놨엇다.
 * - trampoline
 *   https://en.wikipedia.org/wiki/Trampoline_(computing)
 *   https://xiayingp.gitbook.io/build_a_os/traps-and-interrupts/untitled-3
 *
 * - 위에 tst	x22, #PSR_MODE32_BIT
 *   이 code에서 user mode가 32bit인지 64bit인지 확인한채로 Z가
 *   그대로 있는상태다.
 *   64bit면 tramp_exit_native, 32bit면 tramp_exit_compat.
 * - far_el1은 scratch register로 사용한다. x30(tramp_ventry에서 가져온 lr)을 여기서
 *   far_el1에 넣어놓는다.
 *   x30을 계속 다른용도로 사용해야되기 때문이다.
 */
	bne	4f
	msr	far_el1, x30
/*
 * IAMROOT, 2022.11.09:
 * - 각각의 가상주소를 가져와서 br tramp_exit_native, tramp_exit_compat로 넘어간다.
 * - 해당함수에서 eret이 일어난다.
 */
	tramp_alias	x30, tramp_exit_native
	br	x30
4:
	tramp_alias	x30, tramp_exit_compat
	br	x30
#endif
/*
 * IAMROOT, 2022.11.09:
 * - el1인 경우 eret으로 el1 -> el1으로 복귀 되거나 할것이다.
 */
	.else
	/* Ensure any device/NC reads complete */
	alternative_insn nop, "dmb sy", ARM64_WORKAROUND_1508412

	eret
	.endif
/*
 * IAMROOT, 2022.11.09:
 * - SB(Speculation Barrier) sequence
 *   (Git blame 참고, ARM ref. Speculation Barrier (SB) 참고)
 *   https://developer.arm.com/documentation/ddi0597/2022-06/Base-Instructions/SB--Speculation-Barrier-
 * - eret의 뒤에 보통 위치한다. 일부 cpu에서 eret지점에서 낮은 권한 수준에 의해 제어되고,
 *   eret이 수행되기전에 memory speculative access가 수행될수있어 side-channel공격에
 *   사용될수 있다고 한다.
 *   sb 명령을 사용함으로 이전 명령어가 완료될때까지 speculative를 실행할수없게한다.
 * - What is speculative execution?
 *   https://www.extremetech.com/computing/261792-what-is-speculative-execution
 * - isb, dsb, sb
 *   처음에 isb, dsb에서 i,d를 뺀거인줄알았는데 아니였다.
 *   sb : Speculation Barrier
 *   isb : Instruction Synchronization Barrier
 *   dsb : data Synchronization Barrier
 */
	sb
	.endm

/*
 * IAMROOT, 2022.11.08:
 * - Emulate Privileged Access Never using TTBR0_EL1 switching
 */
#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Set the TTBR0 PAN bit in SPSR. When the exception is taken from
	 * EL0, there is no need to check the state of TTBR0_EL1 since
	 * accesses are always enabled.
	 * Note that the meaning of this bit differs from the ARMv8.1 PAN
	 * feature as all TTBR0_EL1 accesses are disabled, not just those to
	 * user mappings.
	 */
/*
 * IAMROOT, 2022.11.08:
 * - papago
 *   SPSR에서 TTBR0 PAN 비트를 설정합니다. EL0에서 예외가 발생하면 항상 액세스가
 *   활성화되어 있으므로 TTBR0_EL1의 상태를 확인할 필요가 없습니다. 
 *   사용자 매핑뿐만 아니라 모든 TTBR0_EL1 액세스가 비활성화되므로 이 비트의
 *   의미는 ARMv8.1 PAN 기능과 다릅니다.
 * - @x23 : register로 backup해놓은 spsr
 * - ttbr0 user access disable을 수행한다.
 */
SYM_CODE_START_LOCAL(__swpan_entry_el1)
	mrs	x21, ttbr0_el1
	tst	x21, #TTBR_ASID_MASK		// Check for the reserved ASID
	orr	x23, x23, #PSR_PAN_BIT		// Set the emulated PAN in the saved SPSR
/*
 * IAMROOT, 2022.11.08:
 * - TTBR_ASID_MASK가 0이라는건 이미 꺼놧다는것. 1f로 점프
 */
	b.eq	1f				// TTBR0 access already disabled
/*
 * IAMROOT, 2022.11.08:
 * - 다시 spsr을 복원하고 __uaccess_ttbr0_disable을 수행한다.
 */
	and	x23, x23, #~PSR_PAN_BIT		// Clear the emulated PAN in the saved SPSR
SYM_INNER_LABEL(__swpan_entry_el0, SYM_L_LOCAL)
	__uaccess_ttbr0_disable x21
1:	ret
SYM_CODE_END(__swpan_entry_el1)

	/*
	 * Restore access to TTBR0_EL1. If returning to EL0, no need for SPSR
	 * PAN bit checking.
	 */
/*
 * IAMROOT, 2022.11.09:
 * @x22 spsr
 * - ttbr0 user access enable을 수행하고 PSR_PAN_BIT를 clear한다.
 */
SYM_CODE_START_LOCAL(__swpan_exit_el1)
/*
 * IAMROOT, 2022.11.09:
 * - PSR_PAN_BIT가 set되있다면 ttbr0를 그대로 놔뚠다.(disable 상태 유지)
 */
	tbnz	x22, #22, 1f			// Skip re-enabling TTBR0 access if the PSR_PAN_BIT is set
	__uaccess_ttbr0_enable x0, x1
1:	and	x22, x22, #~PSR_PAN_BIT		// ARMv8.0 CPUs do not understand this bit
	ret
SYM_CODE_END(__swpan_exit_el1)

/*
 * IAMROOT, 2022.11.09:
 * - ttbr0 user access enable을 수행한다. el0는 PSR_PAN_BIT확인을 안하므로 필요없다.
 */
SYM_CODE_START_LOCAL(__swpan_exit_el0)
	__uaccess_ttbr0_enable x0, x1
	/*
	 * Enable errata workarounds only if returning to user. The only
	 * workaround currently required for TTBR0_EL1 changes are for the
	 * Cavium erratum 27456 (broadcast TLBI instructions may cause I-cache
	 * corruption).
	 */
/*
 * IAMROOT, 2022.11.09:
 * - papago
 *   사용자에게 돌아가는 경우에만 에라타 해결 방법을 활성화하십시오.
 *   현재 TTBR0_EL1 변경에 필요한 유일한 해결 방법은 Cavium erratum 27456에 대한
 *   것입니다(방송 TLBI 명령으로 인해 I-캐시 손상이 발생할 수 있음).
 */
	b	post_ttbr_update_workaround
SYM_CODE_END(__swpan_exit_el0)
#endif

/* GPRs used by entry code */
tsk	.req	x28		// current thread_info

	.text

/*
 * Exception vectors.
 */
	.pushsection ".entry.text", "ax"

/*
 * IAMROOT, 2022.02.08:
 * - exception 발생시 발생하는 일들
 *   1. pstate -> spsr_elx로 백업
 *   2. pc -> elr_elx로 백업
 *   3. pstate에서 i가 autu set.
 *   참고
 *   https://developer.arm.com/documentation/100933/0100/Exception-handling
 *   aarch64_exception_and_interrupt_handling
 *
 * - exception 함수들은 include/asm/exception.h에 위치
 * 예) el0 64bit user -----(irq)----> b el0t_64_irq_handler
 * - t(thread)
 * - h(handler)
 * - ex) el1 64bit irq발생시 flow
 *   vectors 
 *     v
 *   kernel_ventry 1, h, 64, irq
 *     v
 *   el1h_64_irq
 *     v
 *   kernel_entry 1, 64
 *     v
 *   el1h_64_irq_handler
 *     v
 *   ret_to_kernel
 */
	.align	11
SYM_CODE_START(vectors)
/*
 * IAMROOT, 2022.11.07:
 * - thread
 * - sp가 sp_el0를 가리킬때 동작. user app용. kernel은 미사용.
 */
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1h
	kernel_ventry	1, t, 64, error		// Error EL1t
/*
 * IAMROOT, 2022.11.07:
 * - handler
 * - sp가 sp_el1을 가리킬때 동작.
 *   kernel thread등의 동작중일때 진입.
 */
	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h
/*
 * IAMROOT, 2022.11.07:
 * - lower64
 *   64bit app용
 */
	kernel_ventry	0, t, 64, sync		// Synchronous 64-bit EL0
	kernel_ventry	0, t, 64, irq		// IRQ 64-bit EL0
	kernel_ventry	0, t, 64, fiq		// FIQ 64-bit EL0
	kernel_ventry	0, t, 64, error		// Error 64-bit EL0
/*
 * IAMROOT, 2022.11.07:
 * - lower32
 *   32bit app용
 */
	kernel_ventry	0, t, 32, sync		// Synchronous 32-bit EL0
	kernel_ventry	0, t, 32, irq		// IRQ 32-bit EL0
	kernel_ventry	0, t, 32, fiq		// FIQ 32-bit EL0
	kernel_ventry	0, t, 32, error		// Error 32-bit EL0
SYM_CODE_END(vectors)

#ifdef CONFIG_VMAP_STACK
/*
 * IAMROOT, 2022.11.07:
 * - overflow_stack진입시 들어간다.
 */
SYM_CODE_START_LOCAL(__bad_stack)
	/*
	 * We detected an overflow in kernel_ventry, which switched to the
	 * overflow stack. Stash the exception regs, and head to our overflow
	 * handler.
	 */

	/* Restore the original x0 value */
/*
 * IAMROOT, 2022.11.07:
 * - papago
 *   오버플로 스택으로 전환된 kernel_ventry에서 오버플로를 감지했습니다.
 *   예외 규정을 숨기고 오버플로 처리기로 이동합니다.
 *
 * - tpidrro_el0에 x0(pt_regs address)를 이 함수 진입전에 저장해놨었다.
 *   x0에 복원한다.
 */
	mrs	x0, tpidrro_el0

	/*
	 * Store the original GPRs to the new stack. The orginal SP (minus
	 * PT_REGS_SIZE) was stashed in tpidr_el0 by kernel_ventry.
	 */
	sub	sp, sp, #PT_REGS_SIZE
	kernel_entry 1
/*
 * IAMROOT, 2022.11.07:
 * - overflow_stack진입전 sp를 tpidr_el0에 넣어놨었다. x0에 불러와서
 *   sp + PT_REGS_SIZE의 주소를 overflow_stack의 sp에 저장한다.
 *
 */
	mrs	x0, tpidr_el0
	add	x0, x0, #PT_REGS_SIZE
	str	x0, [sp, #S_SP]

	/* Stash the regs for handle_bad_stack */
	mov	x0, sp

	/* Time to die */
	bl	handle_bad_stack
	ASM_BUG()
SYM_CODE_END(__bad_stack)
#endif /* CONFIG_VMAP_STACK */

/*
 * IAMROOT, 2022.02.08:
 *
 * 1. kernel_entry에서 exception이 발생하여 context 를 백업한다. (전체 레지스터)
 * 2. exception에 해당하는 핸들러를 호출한다.
 *    ex) el1h_64_sync_handler
 * 3. 유저 or 커널로 복귀하기 전에 context를 복구한다.
 */
	.macro entry_handler el:req, ht:req, regsize:req, label:req
/*
 * IAMROOT, 2022.11.07:
 * - 함수가 macro로 만들어진다. 내부에선 뒤에 _handler가 붙은 함수로 호출된다.
 *   el1t_64_sync	0x000
 *   el1t_64_irq	0x080
 *   el1t_64_fiq	0x100
 *   el1t_64_error	0x180
 *  
 *   el1h_64_sync	0x200
 *   el1h_64_irq	0x280	
 *   el1h_64_fiq	0x300
 *   el1h_64_error	0x380
 *  
 *   el0t_64_sync	0x400
 *   el0t_64_irq	0x480
 *   el0t_64_fiq	0x500
 *   el0t_64_error	0x580
 *  
 *   el0t_32_sync	0x600
 *   el0t_32_irq	0x680
 *   el0t_32_fiq	0x700
 *   el0t_32_error	0x780
 *
 * - ex) el1 64bit irq발생시 flow
 *   vectors 
 *     v
 *   kernel_ventry 1, h, 64, irq
 *     v
 *   el1h_64_irq  <-- 시작
 *     v
 *   kernel_entry 1, 64
 *     v
 *   el1h_64_irq_handler
 *     v
 *   ret_to_kernel <-- 끝
 */
SYM_CODE_START_LOCAL(el\el\ht\()_\regsize\()_\label)
	kernel_entry \el, \regsize
	mov	x0, sp
	bl	el\el\ht\()_\regsize\()_\label\()_handler
	.if \el == 0
	b	ret_to_user
	.else
	b	ret_to_kernel
	.endif
SYM_CODE_END(el\el\ht\()_\regsize\()_\label)
	.endm

/*
 * Early exception handlers
 */
/*
 * IAMROOT, 2022.11.05: 
 * 부팅 후 곧바로 동작할 16개의 함수
 * 예) el0t_64_irq
 */
	entry_handler	1, t, 64, sync
	entry_handler	1, t, 64, irq
	entry_handler	1, t, 64, fiq
	entry_handler	1, t, 64, error

	entry_handler	1, h, 64, sync
	entry_handler	1, h, 64, irq
	entry_handler	1, h, 64, fiq
	entry_handler	1, h, 64, error

	entry_handler	0, t, 64, sync
	entry_handler	0, t, 64, irq
	entry_handler	0, t, 64, fiq
	entry_handler	0, t, 64, error

	entry_handler	0, t, 32, sync
	entry_handler	0, t, 32, irq
	entry_handler	0, t, 32, fiq
	entry_handler	0, t, 32, error

/*
 * IAMROOT, 2022.11.09:
 * - 원래 지점으로 돌아간다.
 */
SYM_CODE_START_LOCAL(ret_to_kernel)
	kernel_exit 1
SYM_CODE_END(ret_to_kernel)

/*
 * IAMROOT, 2022.11.05: 
 * 유저로 복귀하기 전에 동작하는 루틴.
 *  x19 <- tsk->thread_info.flags
 * - el0 exception시 exception handler를 완료하고 진입한다.
 * - kernel_entry에서 disable했던 single step debug를 다시 enable한다.
 */
SYM_CODE_START_LOCAL(ret_to_user)
	ldr	x19, [tsk, #TSK_TI_FLAGS]	// re-check for single-step
	enable_step_tsk x19, x2
#ifdef CONFIG_GCC_PLUGIN_STACKLEAK
	bl	stackleak_erase
#endif
	kernel_exit 0
SYM_CODE_END(ret_to_user)

	.popsection				// .entry.text

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * Exception vectors trampoline.
 */
	.pushsection ".entry.tramp.text", "ax"

	// Move from tramp_pg_dir to swapper_pg_dir
/*
 * IAMROOT, 2022.11.10:
 * - tramp_pg_dir -> swapper_pg_dir로 전환. 전환하며 USER_ASID_FLAG를 unset한다.
 */
	.macro tramp_map_kernel, tmp
	mrs	\tmp, ttbr1_el1
	add	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET
	bic	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1003
alternative_if ARM64_WORKAROUND_QCOM_FALKOR_E1003
	/* ASID already in \tmp[63:48] */
	movk	\tmp, #:abs_g2_nc:(TRAMP_VALIAS >> 12)
	movk	\tmp, #:abs_g1_nc:(TRAMP_VALIAS >> 12)
	/* 2MB boundary containing the vectors, so we nobble the walk cache */
	movk	\tmp, #:abs_g0_nc:((TRAMP_VALIAS & ~(SZ_2M - 1)) >> 12)
	isb
	tlbi	vae1, \tmp
	dsb	nsh
alternative_else_nop_endif
#endif /* CONFIG_QCOM_FALKOR_ERRATUM_1003 */
	.endm

/*
 * IAMROOT, 2022.11.09:
 * - swapper_pg_dir -> tramp_pg_dir로 교체하면서 USER_ASID_FLAG로 넣어놓는다.
 */
	// Move from swapper_pg_dir to tramp_pg_dir
	.macro tramp_unmap_kernel, tmp
	mrs	\tmp, ttbr1_el1
	sub	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET
	orr	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
	/*
	 * We avoid running the post_ttbr_update_workaround here because
	 * it's only needed by Cavium ThunderX, which requires KPTI to be
	 * disabled.
	 */
	.endm

/*
 * IAMROOT, 2022.11.09:
 * - exception -> tramp_ventry -> kernel_ventry ....
 * - ttbr1_el1이 tramp_pg_dir -> swapper_pg_dir로 전환된다.
 */
	.macro tramp_ventry, regsize = 64
	.align	7
1:
/*
 * IAMROOT, 2022.11.10:
 * - tpidrro_el0에 x30(lr) 에 저장해놨다가 kernel_ventry에서 꺼내서 사용한다.
 */
	.if	\regsize == 64
	msr	tpidrro_el0, x30	// Restored in kernel_ventry
	.endif
	/*
	 * Defend against branch aliasing attacks by pushing a dummy
	 * entry onto the return stack and using a RET instruction to
	 * enter the full-fat kernel vectors.
	 */
/*
 * IAMROOT, 2022.11.10:
 * - papago
 *   반환 스택에 더미 항목을 밀어넣고 RET 명령을 사용하여 전체 팻 커널 벡터를
 *   입력하여 분기 앨리어싱 공격으로부터 방어합니다.
 */
	bl	2f
	b	.
/*
 * IAMROOT, 2022.11.10:
 * - tramp_pg_dir -> swapper_pg_dir로 전환.
 */
2:
	tramp_map_kernel	x30
/*
 * IAMROOT, 2022.11.10:
 * - randomize가 없으면 vectors를 즉시 가져온다.
 *   그게 아니라면 tramp_vectors + PAGE_SIZE에 vectors가 있다.
 *   (map_entry_trampoline()참고)
 */
#ifdef CONFIG_RANDOMIZE_BASE
	adr	x30, tramp_vectors + PAGE_SIZE
alternative_insn isb, nop, ARM64_WORKAROUND_QCOM_FALKOR_E1003
	ldr	x30, [x30]
#else
	ldr	x30, =vectors
#endif
alternative_if_not ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM
	prfm	plil1strm, [x30, #(1b - tramp_vectors)]
alternative_else_nop_endif
/*
 * IAMROOT, 2022.11.10:
 * - vbar_el1에 vectors를 넣어놓고 x30(lr)을 수정후 ret으로 수정된 x30으로
 *   점프한다.
 * - 1b - tramp_vectors
 *   tramp_vectors에서 현재 발생한 tramp_ventry의 offset.
 *   즉 이걸 vectors에 적용하면 vectors + offset이므로 현재 offset에 해당하는
 *   kernel_ventry로 진입할수있게된다.
 */
	msr	vbar_el1, x30
	add	x30, x30, #(1b - tramp_vectors)
	isb
	ret
	.endm

/*
 * IAMROOT, 2022.11.09:
 * - vbar_el1에 tramp_vectors를 넣고, ttbr1_el1도 tramp_pg_dir로
 *   USER_ASID_FLAG를 넣어서 교체해준다.
 *   far_el1에 저장해놨던 lr를 x30에 원복한다.
 */
	.macro tramp_exit, regsize = 64
	adr	x30, tramp_vectors
	msr	vbar_el1, x30
	tramp_unmap_kernel	x30
	.if	\regsize == 64
	mrs	x30, far_el1
	.endif
	eret
	sb
	.endm

	.align	11

/*
 * IAMROOT, 2022.11.10:
 * - user용만 있는게 확인된다.
 */
SYM_CODE_START_NOALIGN(tramp_vectors)
	.space	0x400

	tramp_ventry
	tramp_ventry
	tramp_ventry
	tramp_ventry

	tramp_ventry	32
	tramp_ventry	32
	tramp_ventry	32
	tramp_ventry	32
SYM_CODE_END(tramp_vectors)

/*
 * IAMROOT, 2022.11.09:
 * - 64로 실행
 */
SYM_CODE_START(tramp_exit_native)
	tramp_exit
SYM_CODE_END(tramp_exit_native)

/*
 * IAMROOT, 2022.11.09:
 * - 32bit로 실행
*/
SYM_CODE_START(tramp_exit_compat)
	tramp_exit	32
SYM_CODE_END(tramp_exit_compat)

	.ltorg
	.popsection				// .entry.tramp.text
/*
 * IAMROOT, 2022.11.10:
 * - randomize가 적용되면 vecotrs address를 여기에 저장해놓는다.
 *   해당주소는 fixmap으로 TRAMP_VALIAS + PAGE_SIZE에 mapping된다.
 *   (map_entry_trampoline() 참고)
 */
#ifdef CONFIG_RANDOMIZE_BASE
	.pushsection ".rodata", "a"
	.align PAGE_SHIFT
SYM_DATA_START(__entry_tramp_data_start)
	.quad	vectors
SYM_DATA_END(__entry_tramp_data_start)
	.popsection				// .rodata
#endif /* CONFIG_RANDOMIZE_BASE */
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Register switch for AArch64. The callee-saved registers need to be saved
 * and restored. On entry:
 *   x0 = previous task_struct (must be preserved across the switch)
 *   x1 = next task_struct
 * Previous and next are guaranteed not to be the same.
 *
 */


/*
 * IAMROOT, 2023.01.28: 
 * - x0 : prev, x1 : next (__switch_to() 참고)
 * - 일부 레지스터만 prev에 backup / next에 restore.
 *   thread.cpu_context에 backup한다.
 * - sp_el0에 x1(next)가 등록된다.
 *   ret을 통해 x0가(prev) return 된다.
 */

SYM_FUNC_START(cpu_switch_to)
	mov	x10, #THREAD_CPU_CONTEXT
	add	x8, x0, x10
	mov	x9, sp
	stp	x19, x20, [x8], #16		// store callee-saved registers
	stp	x21, x22, [x8], #16
	stp	x23, x24, [x8], #16
	stp	x25, x26, [x8], #16
	stp	x27, x28, [x8], #16
	stp	x29, x9, [x8], #16
	str	lr, [x8]
	add	x8, x1, x10
	ldp	x19, x20, [x8], #16		// restore callee-saved registers
	ldp	x21, x22, [x8], #16
	ldp	x23, x24, [x8], #16
	ldp	x25, x26, [x8], #16
	ldp	x27, x28, [x8], #16
	ldp	x29, x9, [x8], #16
	ldr	lr, [x8]
	mov	sp, x9
	msr	sp_el0, x1
	ptrauth_keys_install_kernel x1, x8, x9, x10
	scs_save x0
	scs_load x1
	ret
SYM_FUNC_END(cpu_switch_to)
NOKPROBE(cpu_switch_to)

/*
 * This is how we return from a fork.
 */
SYM_CODE_START(ret_from_fork)
	bl	schedule_tail
	cbz	x19, 1f				// not a kernel thread
	mov	x0, x20
	blr	x19
1:	get_current_task tsk
	mov	x0, sp
	bl	asm_exit_to_user_mode
	b	ret_to_user
SYM_CODE_END(ret_from_fork)
NOKPROBE(ret_from_fork)

/*
 * void call_on_irq_stack(struct pt_regs *regs,
 * 		          void (*func)(struct pt_regs *));
 *
 * Calls func(regs) using this CPU's irq stack and shadow irq stack.
 */
/*
 * IAMROOT, 2022.11.08:
 * - pcp irq_stack_ptr로 sp를 전환하여 @func(x1)을 실행한다.
 * - @func실행 전후로 fp, lr 저장 / 원복 등이 이루어진다.
 * - SCS(Shadow call stack)
 *   https://clang.llvm.org/docs/ShadowCallStack.html
 */
SYM_FUNC_START(call_on_irq_stack)
#ifdef CONFIG_SHADOW_CALL_STACK
	stp	scs_sp, xzr, [sp, #-16]!
	ldr_this_cpu scs_sp, irq_shadow_call_stack_ptr, x17
#endif
	/* Create a frame record to save our LR and SP (implicit in FP) */
/*
 * IAMROOT, 2022.11.08:
 * - x30은 lr로 사용한다
 *   https://developer.arm.com/documentation/100933/0100/Returning-from-an-exception
 * - 기존 x29, x30(fp, lr)을 sp에 저장해놓고, 갱신된 sp를 x29에 저장해놓는다.
 */
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
/*
 * IAMROOT, 2022.11.08:
 * - pcp irq_stack_ptr을 구해와 sp를 대체한다.
 */
	ldr_this_cpu x16, irq_stack_ptr, x17
	mov	x15, #IRQ_STACK_SIZE
	add	x16, x16, x15

	/* Move to the new stack and call the function there */
	mov	sp, x16
/*
 * IAMROOT, 2022.11.08:
 * - func실행. func(regs). 형태로 0번인자가 regs로 진입한다.
 */
	blr	x1

	/*
	 * Restore the SP from the FP, and restore the FP and LR from the frame
	 * record.
	 */
/*
 * IAMROOT, 2022.11.08:
 * - func실행전 x29에 저장해놓은 sp를 복구하고, 저장해놓은 sp에서 기존 fp, lr을
 *   x29, x30에 원복한다.
 */
	mov	sp, x29
	ldp	x29, x30, [sp], #16
#ifdef CONFIG_SHADOW_CALL_STACK
	ldp	scs_sp, xzr, [sp], #16
#endif
	ret
SYM_FUNC_END(call_on_irq_stack)
NOKPROBE(call_on_irq_stack)

#ifdef CONFIG_ARM_SDE_INTERFACE

#include <asm/sdei.h>
#include <uapi/linux/arm_sdei.h>

.macro sdei_handler_exit exit_mode
	/* On success, this call never returns... */
	cmp	\exit_mode, #SDEI_EXIT_SMC
	b.ne	99f
	smc	#0
	b	.
99:	hvc	#0
	b	.
.endm

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * The regular SDEI entry point may have been unmapped along with the rest of
 * the kernel. This trampoline restores the kernel mapping to make the x1 memory
 * argument accessible.
 *
 * This clobbers x4, __sdei_handler() will restore this from firmware's
 * copy.
 */
.ltorg
.pushsection ".entry.tramp.text", "ax"
SYM_CODE_START(__sdei_asm_entry_trampoline)
	mrs	x4, ttbr1_el1
	tbz	x4, #USER_ASID_BIT, 1f

	tramp_map_kernel tmp=x4
	isb
	mov	x4, xzr

	/*
	 * Remember whether to unmap the kernel on exit.
	 */
1:	str	x4, [x1, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]

#ifdef CONFIG_RANDOMIZE_BASE
	adr	x4, tramp_vectors + PAGE_SIZE
	add	x4, x4, #:lo12:__sdei_asm_trampoline_next_handler
	ldr	x4, [x4]
#else
	ldr	x4, =__sdei_asm_handler
#endif
	br	x4
SYM_CODE_END(__sdei_asm_entry_trampoline)
NOKPROBE(__sdei_asm_entry_trampoline)

/*
 * Make the exit call and restore the original ttbr1_el1
 *
 * x0 & x1: setup for the exit API call
 * x2: exit_mode
 * x4: struct sdei_registered_event argument from registration time.
 */
SYM_CODE_START(__sdei_asm_exit_trampoline)
	ldr	x4, [x4, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]
	cbnz	x4, 1f

	tramp_unmap_kernel	tmp=x4

1:	sdei_handler_exit exit_mode=x2
SYM_CODE_END(__sdei_asm_exit_trampoline)
NOKPROBE(__sdei_asm_exit_trampoline)
	.ltorg
.popsection		// .entry.tramp.text
#ifdef CONFIG_RANDOMIZE_BASE
.pushsection ".rodata", "a"
SYM_DATA_START(__sdei_asm_trampoline_next_handler)
	.quad	__sdei_asm_handler
SYM_DATA_END(__sdei_asm_trampoline_next_handler)
.popsection		// .rodata
#endif /* CONFIG_RANDOMIZE_BASE */
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Software Delegated Exception entry point.
 *
 * x0: Event number
 * x1: struct sdei_registered_event argument from registration time.
 * x2: interrupted PC
 * x3: interrupted PSTATE
 * x4: maybe clobbered by the trampoline
 *
 * Firmware has preserved x0->x17 for us, we must save/restore the rest to
 * follow SMC-CC. We save (or retrieve) all the registers as the handler may
 * want them.
 */
SYM_CODE_START(__sdei_asm_handler)
	stp     x2, x3, [x1, #SDEI_EVENT_INTREGS + S_PC]
	stp     x4, x5, [x1, #SDEI_EVENT_INTREGS + 16 * 2]
	stp     x6, x7, [x1, #SDEI_EVENT_INTREGS + 16 * 3]
	stp     x8, x9, [x1, #SDEI_EVENT_INTREGS + 16 * 4]
	stp     x10, x11, [x1, #SDEI_EVENT_INTREGS + 16 * 5]
	stp     x12, x13, [x1, #SDEI_EVENT_INTREGS + 16 * 6]
	stp     x14, x15, [x1, #SDEI_EVENT_INTREGS + 16 * 7]
	stp     x16, x17, [x1, #SDEI_EVENT_INTREGS + 16 * 8]
	stp     x18, x19, [x1, #SDEI_EVENT_INTREGS + 16 * 9]
	stp     x20, x21, [x1, #SDEI_EVENT_INTREGS + 16 * 10]
	stp     x22, x23, [x1, #SDEI_EVENT_INTREGS + 16 * 11]
	stp     x24, x25, [x1, #SDEI_EVENT_INTREGS + 16 * 12]
	stp     x26, x27, [x1, #SDEI_EVENT_INTREGS + 16 * 13]
	stp     x28, x29, [x1, #SDEI_EVENT_INTREGS + 16 * 14]
	mov	x4, sp
	stp     lr, x4, [x1, #SDEI_EVENT_INTREGS + S_LR]

	mov	x19, x1

#if defined(CONFIG_VMAP_STACK) || defined(CONFIG_SHADOW_CALL_STACK)
	ldrb	w4, [x19, #SDEI_EVENT_PRIORITY]
#endif

#ifdef CONFIG_VMAP_STACK
	/*
	 * entry.S may have been using sp as a scratch register, find whether
	 * this is a normal or critical event and switch to the appropriate
	 * stack for this CPU.
	 */
	cbnz	w4, 1f
	ldr_this_cpu dst=x5, sym=sdei_stack_normal_ptr, tmp=x6
	b	2f
1:	ldr_this_cpu dst=x5, sym=sdei_stack_critical_ptr, tmp=x6
2:	mov	x6, #SDEI_STACK_SIZE
	add	x5, x5, x6
	mov	sp, x5
#endif

#ifdef CONFIG_SHADOW_CALL_STACK
	/* Use a separate shadow call stack for normal and critical events */
	cbnz	w4, 3f
	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_normal_ptr, tmp=x6
	b	4f
3:	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_critical_ptr, tmp=x6
4:
#endif

	/*
	 * We may have interrupted userspace, or a guest, or exit-from or
	 * return-to either of these. We can't trust sp_el0, restore it.
	 */
	mrs	x28, sp_el0
	ldr_this_cpu	dst=x0, sym=__entry_task, tmp=x1
	msr	sp_el0, x0

	/* If we interrupted the kernel point to the previous stack/frame. */
	and     x0, x3, #0xc
	mrs     x1, CurrentEL
	cmp     x0, x1
	csel	x29, x29, xzr, eq	// fp, or zero
	csel	x4, x2, xzr, eq		// elr, or zero

	stp	x29, x4, [sp, #-16]!
	mov	x29, sp

	add	x0, x19, #SDEI_EVENT_INTREGS
	mov	x1, x19
	bl	__sdei_handler

	msr	sp_el0, x28
	/* restore regs >x17 that we clobbered */
	mov	x4, x19         // keep x4 for __sdei_asm_exit_trampoline
	ldp	x28, x29, [x4, #SDEI_EVENT_INTREGS + 16 * 14]
	ldp	x18, x19, [x4, #SDEI_EVENT_INTREGS + 16 * 9]
	ldp	lr, x1, [x4, #SDEI_EVENT_INTREGS + S_LR]
	mov	sp, x1

	mov	x1, x0			// address to complete_and_resume
	/* x0 = (x0 <= 1) ? EVENT_COMPLETE:EVENT_COMPLETE_AND_RESUME */
	cmp	x0, #1
	mov_q	x2, SDEI_1_0_FN_SDEI_EVENT_COMPLETE
	mov_q	x3, SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME
	csel	x0, x2, x3, ls

	ldr_l	x2, sdei_exit_mode

alternative_if_not ARM64_UNMAP_KERNEL_AT_EL0
	sdei_handler_exit exit_mode=x2
alternative_else_nop_endif

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	tramp_alias	dst=x5, sym=__sdei_asm_exit_trampoline
	br	x5
#endif
SYM_CODE_END(__sdei_asm_handler)
NOKPROBE(__sdei_asm_handler)
#endif /* CONFIG_ARM_SDE_INTERFACE */
