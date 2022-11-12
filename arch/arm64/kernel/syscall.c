// SPDX-License-Identifier: GPL-2.0

#include <linux/compiler.h>
#include <linux/context_tracking.h>
#include <linux/errno.h>
#include <linux/nospec.h>
#include <linux/ptrace.h>
#include <linux/randomize_kstack.h>
#include <linux/syscalls.h>

#include <asm/daifflags.h>
#include <asm/debug-monitors.h>
#include <asm/exception.h>
#include <asm/fpsimd.h>
#include <asm/syscall.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>

long compat_arm_syscall(struct pt_regs *regs, int scno);
long sys_ni_syscall(void);

/*
 * IAMROOT, 2022.11.12:
 * - arm전용 syscall
 */
static long do_ni_syscall(struct pt_regs *regs, int scno)
{
#ifdef CONFIG_COMPAT
	long ret;
	if (is_compat_task()) {
		ret = compat_arm_syscall(regs, scno);
		if (ret != -ENOSYS)
			return ret;
	}
#endif

	return sys_ni_syscall();
}

/*
 * IAMROOT, 2022.11.12:
 * - syscall_fn 실행.
 *   ex)sys_clone
 */
static long __invoke_syscall(struct pt_regs *regs, syscall_fn_t syscall_fn)
{
	return syscall_fn(regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - @scno 에따라 syscall.
 */
static void invoke_syscall(struct pt_regs *regs, unsigned int scno,
			   unsigned int sc_nr,
			   const syscall_fn_t syscall_table[])
{
	long ret;

	add_random_kstack_offset();

	if (scno < sc_nr) {
/*
 * IAMROOT, 2022.11.12:
 * - posix syscall
 */
		syscall_fn_t syscall_fn;
		syscall_fn = syscall_table[array_index_nospec(scno, sc_nr)];
		ret = __invoke_syscall(regs, syscall_fn);
	} else {
		ret = do_ni_syscall(regs, scno);
	}

	syscall_set_return_value(current, regs, 0, ret);

	/*
	 * Ultimately, this value will get limited by KSTACK_OFFSET_MAX(),
	 * but not enough for arm64 stack utilization comfort. To keep
	 * reasonable stack head room, reduce the maximum offset to 9 bits.
	 *
	 * The actual entropy will be further reduced by the compiler when
	 * applying stack alignment constraints: the AAPCS mandates a
	 * 16-byte (i.e. 4-bit) aligned SP at function boundaries.
	 *
	 * The resulting 5 bits of entropy is seen in SP[8:4].
	 */
	choose_random_kstack_offset(get_random_int() & 0x1FF);
}

/*
 * IAMROOT, 2022.11.12:
 * - debug flag가 있는지 확인한다.
 */
static inline bool has_syscall_work(unsigned long flags)
{
	return unlikely(flags & _TIF_SYSCALL_WORK);
}

int syscall_trace_enter(struct pt_regs *regs);
void syscall_trace_exit(struct pt_regs *regs);

/*
 * IAMROOT, 2022.11.12:
 * - syscall
 */
static void el0_svc_common(struct pt_regs *regs, int scno, int sc_nr,
			   const syscall_fn_t syscall_table[])
{
	unsigned long flags = current_thread_info()->flags;
/*
 * IAMROOT, 2022.11.12:
 * - x0는 syscall nr로 사용해야되서 backup해놓는다.
 */
	regs->orig_x0 = regs->regs[0];
	regs->syscallno = scno;

	/*
	 * BTI note:
	 * The architecture does not guarantee that SPSR.BTYPE is zero
	 * on taking an SVC, so we could return to userspace with a
	 * non-zero BTYPE after the syscall.
	 *
	 * This shouldn't matter except when userspace is explicitly
	 * doing something stupid, such as setting PROT_BTI on a page
	 * that lacks conforming BTI/PACIxSP instructions, falling
	 * through from one executable page to another with differing
	 * PROT_BTI, or messing with BTYPE via ptrace: in such cases,
	 * userspace should not be surprised if a SIGILL occurs on
	 * syscall return.
	 *
	 * So, don't touch regs->pstate & PSR_BTYPE_MASK here.
	 * (Similarly for HVC and SMC elsewhere.)
	 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   BTI 참고:
 *   아키텍처는 SVC를 사용할 때 SPSR.BTYPE이 0임을 보장하지 않으므로 시스템 호출 후 0이 아닌
 *   BTYPE을 사용하여 사용자 공간으로 돌아갈 수 있습니다.
 *
 *   이는 사용자 공간이 BTI/PACIxSP 지침을 준수하지 않는 페이지에서 PROT_BTI를 설정하거나,
 *   PROT_BTI가 다른 실행 페이지에서 다른 실행 페이지로 넘어가거나, ptrace를 통해 BTYPE을
 *   엉망으로 만드는 것과 같이 명시적으로 어리석은 일을 하는 경우를 제외하고는 중요하지
 *   않습니다.
 *   이러한 경우 시스템 호출 반환 시 SIGILL이 발생하더라도 사용자 공간은 놀라지 않아야
 *   합니다.
 *
 *   따라서 여기에서 regs->pstate & PSR_BTYPE_MASK를 건드리지 마십시오. 
 *   (다른 곳에서 HVC 및 SMC와 유사합니다.).
 *
 * - PSTATE.BTYPE(Branch target identification bit)
 *
 * - irq enable
 *   syscall 호출 도중에 irq가 들어올수있다.
 */
	local_daif_restore(DAIF_PROCCTX);

	if (flags & _TIF_MTE_ASYNC_FAULT) {
		/*
		 * Process the asynchronous tag check fault before the actual
		 * syscall. do_notify_resume() will send a signal to userspace
		 * before the syscall is restarted.
		 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   실제 시스템 호출 전에 비동기 태그 검사 오류를 처리합니다. do_notify_resume()은
 *   시스템 호출이 다시 시작되기 전에 사용자 공간에 신호를 보냅니다.
 */
		syscall_set_return_value(current, regs, -ERESTARTNOINTR, 0);
		return;
	}

	if (has_syscall_work(flags)) {
		/*
		 * The de-facto standard way to skip a system call using ptrace
		 * is to set the system call to -1 (NO_SYSCALL) and set x0 to a
		 * suitable error code for consumption by userspace. However,
		 * this cannot be distinguished from a user-issued syscall(-1)
		 * and so we must set x0 to -ENOSYS here in case the tracer doesn't
		 * issue the skip and we fall into trace_exit with x0 preserved.
		 *
		 * This is slightly odd because it also means that if a tracer
		 * sets the system call number to -1 but does not initialise x0,
		 * then x0 will be preserved for all system calls apart from a
		 * user-issued syscall(-1). However, requesting a skip and not
		 * setting the return value is unlikely to do anything sensible
		 * anyway.
		 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   ptrace를 사용하여 시스템 호출을 건너뛰는 사실상의 표준 방법은 시스템 호출을
 *   -1(NO_SYSCALL)로 설정하고 x0을 사용자 공간에서 사용하기에 적합한 error 코드로 설정하는
 *   것입니다. 그러나 이것은 사용자가 발행한 syscall(-1)과 구별할 수 없으므로 추적
 *   프로그램에서 건너뛰기를 실행하지 않고 x0이 보존된 trace_exit에 빠질 경우를 대비하여
 *   여기에서 x0을 -ENOSYS로 설정해야 합니다.
 *
 *   이것은 추적자가 시스템 호출 번호를 -1로 설정하지만 x0을 초기화하지 않으면 사용자가
 *   발행한 syscall(-1)을 제외한 모든 시스템 호출에 대해 x0이 보존된다는 의미이기 때문에
 *   약간 이상합니다. 그러나 건너뛰기를 요청하고 반환 값을 설정하지 않는 것은 어쨌든
 *   합리적이지 않을 것입니다.
 */
		if (scno == NO_SYSCALL)
			syscall_set_return_value(current, regs, -ENOSYS, 0);
		scno = syscall_trace_enter(regs);
		if (scno == NO_SYSCALL)
			goto trace_exit;
	}

	invoke_syscall(regs, scno, sc_nr, syscall_table);

	/*
	 * The tracing status may have changed under our feet, so we have to
	 * check again. However, if we were tracing entry, then we always trace
	 * exit regardless, as the old entry assembly did.
	 */
	if (!has_syscall_work(flags) && !IS_ENABLED(CONFIG_DEBUG_RSEQ)) {
		local_daif_mask();
		flags = current_thread_info()->flags;
		if (!has_syscall_work(flags) && !(flags & _TIF_SINGLESTEP))
			return;
		local_daif_restore(DAIF_PROCCTX);
	}

trace_exit:
	syscall_trace_exit(regs);
}

/*
 * IAMROOT, 2022.11.12:
 * - sve를 지원중이라면 discard시킨다.
 */
static inline void sve_user_discard(void)
{
	if (!system_supports_sve())
		return;

/*
 * IAMROOT, 2022.11.12:
 * - SVE를 지원하는 system이라면 thread flag에서 TIF_SVE flag를 지운다.
 */
	clear_thread_flag(TIF_SVE);

	/*
	 * task_fpsimd_load() won't be called to update CPACR_EL1 in
	 * ret_to_user unless TIF_FOREIGN_FPSTATE is still set, which only
	 * happens if a context switch or kernel_neon_begin() or context
	 * modification (sigreturn, ptrace) intervenes.
	 * So, ensure that CPACR_EL1 is already correct for the fast-path case.
	 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   task_fpsimd_load()는 TIF_FOREIGN_FPSTATE가 아직 설정되어 있지 않으면
 *   ret_to_user에서 CPACR_EL1을 업데이트하기 위해 호출되지 않습니다.
 *   이는 컨텍스트 전환 또는 kernel_neon_begin() 또는 컨텍스트
 *   수정(sigreturn, ptrace)이 개입하는 경우에만 발생합니다.
 *   따라서 CPACR_EL1이 빠른 경로의 경우에 이미 올바른지 확인하십시오.
 */
	sve_user_disable();
}

/*
 * IAMROOT, 2022.11.12:
 * - syscall
 */
void do_el0_svc(struct pt_regs *regs)
{
	sve_user_discard();
	el0_svc_common(regs, regs->regs[8], __NR_syscalls, sys_call_table);
}

#ifdef CONFIG_COMPAT
void do_el0_svc_compat(struct pt_regs *regs)
{
	el0_svc_common(regs, regs->regs[7], __NR_compat_syscalls,
		       compat_sys_call_table);
}
#endif
