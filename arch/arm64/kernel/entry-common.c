// SPDX-License-Identifier: GPL-2.0
/*
 * Exception handling code
 *
 * Copyright (C) 2019 ARM Ltd.
 */

#include <linux/context_tracking.h>
#include <linux/linkage.h>
#include <linux/lockdep.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/thread_info.h>

#include <asm/cpufeature.h>
#include <asm/daifflags.h>
#include <asm/esr.h>
#include <asm/exception.h>
#include <asm/kprobes.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/sdei.h>
#include <asm/stacktrace.h>
#include <asm/sysreg.h>
#include <asm/system_misc.h>

/*
 * Handle IRQ/context state management when entering from kernel mode.
 * Before this function is called it is not safe to call regular kernel code,
 * intrumentable code, or any code which may trigger an exception.
 *
 * This is intended to match the logic in irqentry_enter(), handling the kernel
 * mode transitions only.
 */
static __always_inline void __enter_from_kernel_mode(struct pt_regs *regs)
{
	regs->exit_rcu = false;

	if (!IS_ENABLED(CONFIG_TINY_RCU) && is_idle_task(current)) {
		lockdep_hardirqs_off(CALLER_ADDR0);
		rcu_irq_enter();
		trace_hardirqs_off_finish();

		regs->exit_rcu = true;
		return;
	}

	lockdep_hardirqs_off(CALLER_ADDR0);
	rcu_irq_enter_check_tick();
	trace_hardirqs_off_finish();
}

static void noinstr enter_from_kernel_mode(struct pt_regs *regs)
{
	__enter_from_kernel_mode(regs);
	mte_check_tfsr_entry();
}

/*
 * Handle IRQ/context state management when exiting to kernel mode.
 * After this function returns it is not safe to call regular kernel code,
 * intrumentable code, or any code which may trigger an exception.
 *
 * This is intended to match the logic in irqentry_exit(), handling the kernel
 * mode transitions only, and with preemption handled elsewhere.
 */
static __always_inline void __exit_to_kernel_mode(struct pt_regs *regs)
{
	lockdep_assert_irqs_disabled();

	if (interrupts_enabled(regs)) {
		if (regs->exit_rcu) {
			trace_hardirqs_on_prepare();
			lockdep_hardirqs_on_prepare(CALLER_ADDR0);
			rcu_irq_exit();
			lockdep_hardirqs_on(CALLER_ADDR0);
			return;
		}

		trace_hardirqs_on();
	} else {
		if (regs->exit_rcu)
			rcu_irq_exit();
	}
}

static void noinstr exit_to_kernel_mode(struct pt_regs *regs)
{
	mte_check_tfsr_exit();
	__exit_to_kernel_mode(regs);
}

/*
 * Handle IRQ/context state management when entering from user mode.
 * Before this function is called it is not safe to call regular kernel code,
 * intrumentable code, or any code which may trigger an exception.
 */
/*
 * IAMROOT, 2022.11.07:
 * - papago
 *   사용자 모드에서 진입할 때 IRQ/컨텍스트 상태 관리를 처리합니다. 
 *   이 함수가 호출되기 전에는 일반 커널 코드, 계측 가능 코드 또는 예외를
 *   유발할 수 있는 모든 코드를 호출하는 것이 안전하지 않습니다.
 * - dubug, context tracking 관련.
 */
static __always_inline void __enter_from_user_mode(void)
{
	lockdep_hardirqs_off(CALLER_ADDR0);
	CT_WARN_ON(ct_state() != CONTEXT_USER);
	user_exit_irqoff();
	trace_hardirqs_off_finish();
}

/*
 * IAMROOT, 2022.11.07:
 * - irq 진입시 debug, context tracking 관련 처리.
 */
static __always_inline void enter_from_user_mode(struct pt_regs *regs)
{
	__enter_from_user_mode();
}

/*
 * Handle IRQ/context state management when exiting to user mode.
 * After this function returns it is not safe to call regular kernel code,
 * intrumentable code, or any code which may trigger an exception.
 */
/*
 * IAMROOT, 2022.11.07:
 * - irq 퇴장시 debug, context tracking 관련 처리.
 */
static __always_inline void __exit_to_user_mode(void)
{
	trace_hardirqs_on_prepare();
	lockdep_hardirqs_on_prepare(CALLER_ADDR0);
	user_enter_irqoff();
	lockdep_hardirqs_on(CALLER_ADDR0);
}
/*
 * IAMROOT, 2022.11.07:
 * - irq disable
 */
static __always_inline void prepare_exit_to_user_mode(struct pt_regs *regs)
{
	unsigned long flags;

	local_daif_mask();

	flags = READ_ONCE(current_thread_info()->flags);
	if (unlikely(flags & _TIF_WORK_MASK))
		do_notify_resume(regs, flags);
}

/*
 * IAMROOT, 2022.11.07:
 * - irq disable
 */
static __always_inline void exit_to_user_mode(struct pt_regs *regs)
{
	prepare_exit_to_user_mode(regs);
	mte_check_tfsr_exit();
	__exit_to_user_mode();
}

asmlinkage void noinstr asm_exit_to_user_mode(struct pt_regs *regs)
{
	exit_to_user_mode(regs);
}

/*
 * Handle IRQ/context state management when entering an NMI from user/kernel
 * mode. Before this function is called it is not safe to call regular kernel
 * code, intrumentable code, or any code which may trigger an exception.
 */
static void noinstr arm64_enter_nmi(struct pt_regs *regs)
{
	regs->lockdep_hardirqs = lockdep_hardirqs_enabled();

	__nmi_enter();
	lockdep_hardirqs_off(CALLER_ADDR0);
	lockdep_hardirq_enter();
	rcu_nmi_enter();

	trace_hardirqs_off_finish();
	ftrace_nmi_enter();
}

/*
 * Handle IRQ/context state management when exiting an NMI from user/kernel
 * mode. After this function returns it is not safe to call regular kernel
 * code, intrumentable code, or any code which may trigger an exception.
 */
static void noinstr arm64_exit_nmi(struct pt_regs *regs)
{
	bool restore = regs->lockdep_hardirqs;

	ftrace_nmi_exit();
	if (restore) {
		trace_hardirqs_on_prepare();
		lockdep_hardirqs_on_prepare(CALLER_ADDR0);
	}

	rcu_nmi_exit();
	lockdep_hardirq_exit();
	if (restore)
		lockdep_hardirqs_on(CALLER_ADDR0);
	__nmi_exit();
}

/*
 * Handle IRQ/context state management when entering a debug exception from
 * kernel mode. Before this function is called it is not safe to call regular
 * kernel code, intrumentable code, or any code which may trigger an exception.
 */
static void noinstr arm64_enter_el1_dbg(struct pt_regs *regs)
{
	regs->lockdep_hardirqs = lockdep_hardirqs_enabled();

	lockdep_hardirqs_off(CALLER_ADDR0);
	rcu_nmi_enter();

	trace_hardirqs_off_finish();
}

/*
 * Handle IRQ/context state management when exiting a debug exception from
 * kernel mode. After this function returns it is not safe to call regular
 * kernel code, intrumentable code, or any code which may trigger an exception.
 */
static void noinstr arm64_exit_el1_dbg(struct pt_regs *regs)
{
	bool restore = regs->lockdep_hardirqs;

	if (restore) {
		trace_hardirqs_on_prepare();
		lockdep_hardirqs_on_prepare(CALLER_ADDR0);
	}

	rcu_nmi_exit();
	if (restore)
		lockdep_hardirqs_on(CALLER_ADDR0);
}

static void noinstr enter_el1_irq_or_nmi(struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI) && !interrupts_enabled(regs))
		arm64_enter_nmi(regs);
	else
		enter_from_kernel_mode(regs);
}

static void noinstr exit_el1_irq_or_nmi(struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI) && !interrupts_enabled(regs))
		arm64_exit_nmi(regs);
	else
		exit_to_kernel_mode(regs);
}

static void __sched arm64_preempt_schedule_irq(void)
{
	lockdep_assert_irqs_disabled();

	/*
	 * DAIF.DA are cleared at the start of IRQ/FIQ handling, and when GIC
	 * priority masking is used the GIC irqchip driver will clear DAIF.IF
	 * using gic_arch_enable_irqs() for normal IRQs. If anything is set in
	 * DAIF we must have handled an NMI, so skip preemption.
	 */
	if (system_uses_irq_prio_masking() && read_sysreg(daif))
		return;

	/*
	 * Preempting a task from an IRQ means we leave copies of PSTATE
	 * on the stack. cpufeature's enable calls may modify PSTATE, but
	 * resuming one of these preempted tasks would undo those changes.
	 *
	 * Only allow a task to be preempted once cpufeatures have been
	 * enabled.
	 */
	if (system_capabilities_finalized())
		preempt_schedule_irq();
}

/*
 * IAMROOT, 2022.11.08:
 * - @handler 실행
 * - irq 흐름.
 *   vectors(arch/arm64/kernel/entry.S)
 *     |
 *     |(kernel_ventry)
 *     |(entry_handler)
 *     |(el0_interrupt or el1_interrupt)
 *     |(do_interrupt_handler) <---------- 현재
 *     |(handler_arch_irq 실행) <----------/
 *     v
 *   chip handler(handler_arch_irq, gic의 경우 gic_handle_irq)
 *     v
 *   flow handler
 *     v
 *   irq_handler
 */
static void do_interrupt_handler(struct pt_regs *regs,
				 void (*handler)(struct pt_regs *))
{
/*
 * IAMROOT, 2022.11.05: 
 * sp가 현재 태스크의 스택 포인터 범위안에 존재하는 경우
 * call_on_irq_stack() 내부에서 전용 irq 스택에 LR, SP를 백업하고,
 * handler(regs)를 호출한다.
 */
	if (on_thread_stack())
		call_on_irq_stack(regs, handler);
	else
		handler(regs);
}

extern void (*handle_arch_irq)(struct pt_regs *);
extern void (*handle_arch_fiq)(struct pt_regs *);

static void noinstr __panic_unhandled(struct pt_regs *regs, const char *vector,
				      unsigned int esr)
{
	arm64_enter_nmi(regs);

	console_verbose();

	pr_crit("Unhandled %s exception on CPU%d, ESR 0x%08x -- %s\n",
		vector, smp_processor_id(), esr,
		esr_get_class_string(esr));

	__show_regs(regs);
	panic("Unhandled exception");
}

#define UNHANDLED(el, regsize, vector)							\
asmlinkage void noinstr el##_##regsize##_##vector##_handler(struct pt_regs *regs)	\
{											\
	const char *desc = #regsize "-bit " #el " " #vector;				\
	__panic_unhandled(regs, desc, read_sysreg(esr_el1));				\
}

#ifdef CONFIG_ARM64_ERRATUM_1463225
static DEFINE_PER_CPU(int, __in_cortex_a76_erratum_1463225_wa);

static void cortex_a76_erratum_1463225_svc_handler(void)
{
	u32 reg, val;

	if (!unlikely(test_thread_flag(TIF_SINGLESTEP)))
		return;

	if (!unlikely(this_cpu_has_cap(ARM64_WORKAROUND_1463225)))
		return;

	__this_cpu_write(__in_cortex_a76_erratum_1463225_wa, 1);
	reg = read_sysreg(mdscr_el1);
	val = reg | DBG_MDSCR_SS | DBG_MDSCR_KDE;
	write_sysreg(val, mdscr_el1);
	asm volatile("msr daifclr, #8");
	isb();

	/* We will have taken a single-step exception by this point */

	write_sysreg(reg, mdscr_el1);
	__this_cpu_write(__in_cortex_a76_erratum_1463225_wa, 0);
}

static bool cortex_a76_erratum_1463225_debug_handler(struct pt_regs *regs)
{
	if (!__this_cpu_read(__in_cortex_a76_erratum_1463225_wa))
		return false;

	/*
	 * We've taken a dummy step exception from the kernel to ensure
	 * that interrupts are re-enabled on the syscall path. Return back
	 * to cortex_a76_erratum_1463225_svc_handler() with debug exceptions
	 * masked so that we can safely restore the mdscr and get on with
	 * handling the syscall.
	 */
	regs->pstate |= PSR_D_BIT;
	return true;
}
#else /* CONFIG_ARM64_ERRATUM_1463225 */
static void cortex_a76_erratum_1463225_svc_handler(void) { }
static bool cortex_a76_erratum_1463225_debug_handler(struct pt_regs *regs)
{
	return false;
}
#endif /* CONFIG_ARM64_ERRATUM_1463225 */

UNHANDLED(el1t, 64, sync)
UNHANDLED(el1t, 64, irq)
UNHANDLED(el1t, 64, fiq)
UNHANDLED(el1t, 64, error)

static void noinstr el1_abort(struct pt_regs *regs, unsigned long esr)
{
	unsigned long far = read_sysreg(far_el1);

	enter_from_kernel_mode(regs);
	local_daif_inherit(regs);
	do_mem_abort(far, esr, regs);
	local_daif_mask();
	exit_to_kernel_mode(regs);
}

static void noinstr el1_pc(struct pt_regs *regs, unsigned long esr)
{
	unsigned long far = read_sysreg(far_el1);

	enter_from_kernel_mode(regs);
	local_daif_inherit(regs);
	do_sp_pc_abort(far, esr, regs);
	local_daif_mask();
	exit_to_kernel_mode(regs);
}

static void noinstr el1_undef(struct pt_regs *regs)
{
	enter_from_kernel_mode(regs);
	local_daif_inherit(regs);
	do_undefinstr(regs);
	local_daif_mask();
	exit_to_kernel_mode(regs);
}

static void noinstr el1_dbg(struct pt_regs *regs, unsigned long esr)
{
	unsigned long far = read_sysreg(far_el1);

	arm64_enter_el1_dbg(regs);
	if (!cortex_a76_erratum_1463225_debug_handler(regs))
		do_debug_exception(far, esr, regs);
	arm64_exit_el1_dbg(regs);
}

static void noinstr el1_fpac(struct pt_regs *regs, unsigned long esr)
{
	enter_from_kernel_mode(regs);
	local_daif_inherit(regs);
	do_ptrauth_fault(regs, esr);
	local_daif_mask();
	exit_to_kernel_mode(regs);
}

asmlinkage void noinstr el1h_64_sync_handler(struct pt_regs *regs)
{
	unsigned long esr = read_sysreg(esr_el1);

	switch (ESR_ELx_EC(esr)) {
	case ESR_ELx_EC_DABT_CUR:
	case ESR_ELx_EC_IABT_CUR:
		el1_abort(regs, esr);
		break;
	/*
	 * We don't handle ESR_ELx_EC_SP_ALIGN, since we will have hit a
	 * recursive exception when trying to push the initial pt_regs.
	 */
	case ESR_ELx_EC_PC_ALIGN:
		el1_pc(regs, esr);
		break;
	case ESR_ELx_EC_SYS64:
	case ESR_ELx_EC_UNKNOWN:
		el1_undef(regs);
		break;
	case ESR_ELx_EC_BREAKPT_CUR:
	case ESR_ELx_EC_SOFTSTP_CUR:
	case ESR_ELx_EC_WATCHPT_CUR:
	case ESR_ELx_EC_BRK64:
		el1_dbg(regs, esr);
		break;
	case ESR_ELx_EC_FPAC:
		el1_fpac(regs, esr);
		break;
	default:
		__panic_unhandled(regs, "64-bit el1h sync", esr);
	}
}

/*
 * IAMROOT, 2023.01.14:
 * - schedule() 함수 호출 경로
 *   arm64_preempt_schedule_irq -> preempt_schedule_irq -> __schedule()
 */
static void noinstr el1_interrupt(struct pt_regs *regs,
				  void (*handler)(struct pt_regs *))
{
	write_sysreg(DAIF_PROCCTX_NOIRQ, daif);

	enter_el1_irq_or_nmi(regs);
	do_interrupt_handler(regs, handler);

	/*
	 * Note: thread_info::preempt_count includes both thread_info::count
	 * and thread_info::need_resched, and is not equivalent to
	 * preempt_count().
	 */
	if (IS_ENABLED(CONFIG_PREEMPTION) &&
	    READ_ONCE(current_thread_info()->preempt_count) == 0)
		arm64_preempt_schedule_irq();

	exit_el1_irq_or_nmi(regs);
}

asmlinkage void noinstr el1h_64_irq_handler(struct pt_regs *regs)
{
	el1_interrupt(regs, handle_arch_irq);
}

/*
 * IAMROOT, 2022.11.12:
 * - handler_arch_fiq실행
 */
asmlinkage void noinstr el1h_64_fiq_handler(struct pt_regs *regs)
{
	el1_interrupt(regs, handle_arch_fiq);
}

asmlinkage void noinstr el1h_64_error_handler(struct pt_regs *regs)
{
	unsigned long esr = read_sysreg(esr_el1);

	local_daif_restore(DAIF_ERRCTX);
	arm64_enter_nmi(regs);
	do_serror(regs, esr);
	arm64_exit_nmi(regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - data fault
 */
static void noinstr el0_da(struct pt_regs *regs, unsigned long esr)
{
/*
 * IAMROOT, 2022.11.12:
 * - fault address register. fault 발생 주소를 가져온다.
 */
	unsigned long far = read_sysreg(far_el1);

	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_mem_abort(far, esr, regs);
	exit_to_user_mode(regs);
}


/*
 * IAMROOT, 2022.11.12:
 * - inst fault
 */
static void noinstr el0_ia(struct pt_regs *regs, unsigned long esr)
{
	unsigned long far = read_sysreg(far_el1);

	/*
	 * We've taken an instruction abort from userspace and not yet
	 * re-enabled IRQs. If the address is a kernel address, apply
	 * BP hardening prior to enabling IRQs and pre-emption.
	 */
	if (!is_ttbr0_addr(far))
		arm64_apply_bp_hardening();

	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_mem_abort(far, esr, regs);
	exit_to_user_mode(regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - warnning을 띄운다.
 */
static void noinstr el0_fpsimd_acc(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_fpsimd_acc(esr, regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_sve_acc(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_sve_acc(esr, regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_fpsimd_exc(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_fpsimd_exc(esr, regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_sys(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_sysinstr(esr, regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_pc(struct pt_regs *regs, unsigned long esr)
{
	unsigned long far = read_sysreg(far_el1);

	if (!is_ttbr0_addr(instruction_pointer(regs)))
		arm64_apply_bp_hardening();

	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_sp_pc_abort(far, esr, regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_sp(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_sp_pc_abort(regs->sp, esr, regs);
	exit_to_user_mode(regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - undef 처리.
 */
static void noinstr el0_undef(struct pt_regs *regs)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_undefinstr(regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_bti(struct pt_regs *regs)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_bti(regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_inv(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	bad_el0_sync(regs, 0, esr);
	exit_to_user_mode(regs);
}

static void noinstr el0_dbg(struct pt_regs *regs, unsigned long esr)
{
	/* Only watchpoints write FAR_EL1, otherwise its UNKNOWN */
	unsigned long far = read_sysreg(far_el1);

	enter_from_user_mode(regs);
	do_debug_exception(far, esr, regs);
	local_daif_restore(DAIF_PROCCTX);
	exit_to_user_mode(regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - syscall
 */
static void noinstr el0_svc(struct pt_regs *regs)
{
	enter_from_user_mode(regs);
	cortex_a76_erratum_1463225_svc_handler();
	do_el0_svc(regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_fpac(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_ptrauth_fault(regs, esr);
	exit_to_user_mode(regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - 
 */
asmlinkage void noinstr el0t_64_sync_handler(struct pt_regs *regs)
{

/*
 * IAMROOT, 2022.11.12:
 * - Exception Syndrome Register
 */
	unsigned long esr = read_sysreg(esr_el1);

	switch (ESR_ELx_EC(esr)) {

/*
 * IAMROOT, 2022.11.12:
 * - systemcall
 */
	case ESR_ELx_EC_SVC64:
		el0_svc(regs);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - data abort
 */
	case ESR_ELx_EC_DABT_LOW:
		el0_da(regs, esr);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - inst abort
 */
	case ESR_ELx_EC_IABT_LOW:
		el0_ia(regs, esr);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - advanced SIMD
 */
	case ESR_ELx_EC_FP_ASIMD:
		el0_fpsimd_acc(regs, esr);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - SVE
 */
	case ESR_ELx_EC_SVE:
		el0_sve_acc(regs, esr);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - float pointer
 */
	case ESR_ELx_EC_FP_EXC64:
		el0_fpsimd_exc(regs, esr);
		break;
/*
 * IAMROOT, 2022.11.12:
 * - mrs/msr exception
 */
	case ESR_ELx_EC_SYS64:
/*
 * IAMROOT, 2022.11.12:
 * - WFI,WFE exception
 */
	case ESR_ELx_EC_WFx:
		el0_sys(regs, esr);
		break;
/*
 * IAMROOT, 2022.11.12:
 * - sp align
 */
	case ESR_ELx_EC_SP_ALIGN:
		el0_sp(regs, esr);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - pc align
 */
	case ESR_ELx_EC_PC_ALIGN:
		el0_pc(regs, esr);
		break;
/*
 * IAMROOT, 2022.11.12:
 * - undef inst
 */
	case ESR_ELx_EC_UNKNOWN:
		el0_undef(regs);
		break;
/*
 * IAMROOT, 2022.11.12:
 * - byte top ignore
 */
	case ESR_ELx_EC_BTI:
		el0_bti(regs);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - debug용
 */
	case ESR_ELx_EC_BREAKPT_LOW:
	case ESR_ELx_EC_SOFTSTP_LOW:
	case ESR_ELx_EC_WATCHPT_LOW:
	case ESR_ELx_EC_BRK64:
		el0_dbg(regs, esr);
		break;

/*
 * IAMROOT, 2022.11.12:
 * - point authentication exception
 */
	case ESR_ELx_EC_FPAC:
		el0_fpac(regs, esr);
		break;
	default:
		el0_inv(regs, esr);
	}
}

/*
 * IAMROOT, 2022.11.07:
 * - irq, fiq disable후 @handler 수행
 * - schedule()함수 호출 경로
 *   exit_to_user_mode -> prepare_exit_to_user_mode -> do_notify_resume ->
 *   schedule()
 */
static void noinstr el0_interrupt(struct pt_regs *regs,
				  void (*handler)(struct pt_regs *))
{
/*
 * IAMROOT, 2022.11.05: 
 * 유저에서 진입하여 irq 처리가 시작 되기 전 trace 출력 등
 */
	enter_from_user_mode(regs);

/*
 * IAMROOT, 2022.11.05: 
 * PSTATE의 DAIF 중 IF에 해당하는 irq & fiq를 1로 mask하여 두 기능을 disable한다.
 */
	write_sysreg(DAIF_PROCCTX_NOIRQ, daif);

	if (regs->pc & BIT(55))
		arm64_apply_bp_hardening();

/*
 * IAMROOT, 2022.11.05: 
 * 인터럽트 핸들러로 연동
 */
	do_interrupt_handler(regs, handler);

/*
 * IAMROOT, 2022.11.05: 
 * irq 처리가 끝난 후 유저로 빠져나갈때의 trace 출력 등
 */
	exit_to_user_mode(regs);
}

/*
 * IAMROOT, 2022.11.05: 
 * 인터럽트가 진입하는 경우 대표 인터럽트 컨트롤러의 핸들러로 향한다.
 * 예) GICv3: gic_handle_irq
 * - handle_arch_irq
 *   arch/arm64/kernel/irq.c
 *   gic의 경우 gic_init_bases-> set_handle_irq를 통해서 gic_handle_irq가 설정된다.
 */
static void noinstr __el0_irq_handler_common(struct pt_regs *regs)
{
	el0_interrupt(regs, handle_arch_irq);
}

/*
 * IAMROOT, 2022.11.05: 
 * 64bit user appl ----(irq)-----> vectors -> 아래 핸들러 호출
 */

asmlinkage void noinstr el0t_64_irq_handler(struct pt_regs *regs)
{
	__el0_irq_handler_common(regs);
}

static void noinstr __el0_fiq_handler_common(struct pt_regs *regs)
{
	el0_interrupt(regs, handle_arch_fiq);
}

asmlinkage void noinstr el0t_64_fiq_handler(struct pt_regs *regs)
{
	__el0_fiq_handler_common(regs);
}

static void noinstr __el0_error_handler_common(struct pt_regs *regs)
{
	unsigned long esr = read_sysreg(esr_el1);

	enter_from_user_mode(regs);
	local_daif_restore(DAIF_ERRCTX);
	arm64_enter_nmi(regs);
	do_serror(regs, esr);
	arm64_exit_nmi(regs);
	local_daif_restore(DAIF_PROCCTX);
	exit_to_user_mode(regs);
}

asmlinkage void noinstr el0t_64_error_handler(struct pt_regs *regs)
{
	__el0_error_handler_common(regs);
}

#ifdef CONFIG_COMPAT
static void noinstr el0_cp15(struct pt_regs *regs, unsigned long esr)
{
	enter_from_user_mode(regs);
	local_daif_restore(DAIF_PROCCTX);
	do_cp15instr(esr, regs);
	exit_to_user_mode(regs);
}

static void noinstr el0_svc_compat(struct pt_regs *regs)
{
	enter_from_user_mode(regs);
	cortex_a76_erratum_1463225_svc_handler();
	do_el0_svc_compat(regs);
	exit_to_user_mode(regs);
}

asmlinkage void noinstr el0t_32_sync_handler(struct pt_regs *regs)
{
	unsigned long esr = read_sysreg(esr_el1);

	switch (ESR_ELx_EC(esr)) {
	case ESR_ELx_EC_SVC32:
		el0_svc_compat(regs);
		break;
	case ESR_ELx_EC_DABT_LOW:
		el0_da(regs, esr);
		break;
	case ESR_ELx_EC_IABT_LOW:
		el0_ia(regs, esr);
		break;
	case ESR_ELx_EC_FP_ASIMD:
		el0_fpsimd_acc(regs, esr);
		break;
	case ESR_ELx_EC_FP_EXC32:
		el0_fpsimd_exc(regs, esr);
		break;
	case ESR_ELx_EC_PC_ALIGN:
		el0_pc(regs, esr);
		break;
	case ESR_ELx_EC_UNKNOWN:
	case ESR_ELx_EC_CP14_MR:
	case ESR_ELx_EC_CP14_LS:
	case ESR_ELx_EC_CP14_64:
		el0_undef(regs);
		break;
	case ESR_ELx_EC_CP15_32:
	case ESR_ELx_EC_CP15_64:
		el0_cp15(regs, esr);
		break;
	case ESR_ELx_EC_BREAKPT_LOW:
	case ESR_ELx_EC_SOFTSTP_LOW:
	case ESR_ELx_EC_WATCHPT_LOW:
	case ESR_ELx_EC_BKPT32:
		el0_dbg(regs, esr);
		break;
	default:
		el0_inv(regs, esr);
	}
}

asmlinkage void noinstr el0t_32_irq_handler(struct pt_regs *regs)
{
	__el0_irq_handler_common(regs);
}

asmlinkage void noinstr el0t_32_fiq_handler(struct pt_regs *regs)
{
	__el0_fiq_handler_common(regs);
}

asmlinkage void noinstr el0t_32_error_handler(struct pt_regs *regs)
{
	__el0_error_handler_common(regs);
}
#else /* CONFIG_COMPAT */
UNHANDLED(el0t, 32, sync)
UNHANDLED(el0t, 32, irq)
UNHANDLED(el0t, 32, fiq)
UNHANDLED(el0t, 32, error)
#endif /* CONFIG_COMPAT */

#ifdef CONFIG_VMAP_STACK
/*
 * IAMROOT, 2022.11.07:
 * - interrupt수행중 overflow_stack시 진입.  arm64_enter_nmi, regs print후 죽는다
 */
asmlinkage void noinstr handle_bad_stack(struct pt_regs *regs)
{
	unsigned int esr = read_sysreg(esr_el1);
	unsigned long far = read_sysreg(far_el1);

	arm64_enter_nmi(regs);
	panic_bad_stack(regs, esr, far);
}
#endif /* CONFIG_VMAP_STACK */

#ifdef CONFIG_ARM_SDE_INTERFACE
asmlinkage noinstr unsigned long
__sdei_handler(struct pt_regs *regs, struct sdei_registered_event *arg)
{
	unsigned long ret;

	/*
	 * We didn't take an exception to get here, so the HW hasn't
	 * set/cleared bits in PSTATE that we may rely on.
	 *
	 * The original SDEI spec (ARM DEN 0054A) can be read ambiguously as to
	 * whether PSTATE bits are inherited unchanged or generated from
	 * scratch, and the TF-A implementation always clears PAN and always
	 * clears UAO. There are no other known implementations.
	 *
	 * Subsequent revisions (ARM DEN 0054B) follow the usual rules for how
	 * PSTATE is modified upon architectural exceptions, and so PAN is
	 * either inherited or set per SCTLR_ELx.SPAN, and UAO is always
	 * cleared.
	 *
	 * We must explicitly reset PAN to the expected state, including
	 * clearing it when the host isn't using it, in case a VM had it set.
	 */
	if (system_uses_hw_pan())
		set_pstate_pan(1);
	else if (cpu_has_pan())
		set_pstate_pan(0);

	arm64_enter_nmi(regs);
	ret = do_sdei_event(regs, arg);
	arm64_exit_nmi(regs);

	return ret;
}
#endif /* CONFIG_ARM_SDE_INTERFACE */
