/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/mmu_context.h
 *
 * Copyright (C) 1996 Russell King.
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_MMU_CONTEXT_H
#define __ASM_MMU_CONTEXT_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/sched/hotplug.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>

#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/proc-fns.h>
#include <asm-generic/mm_hooks.h>
#include <asm/cputype.h>
#include <asm/sysreg.h>
#include <asm/tlbflush.h>

extern bool rodata_full;

/*
 * IAMROOT, 2023.01.28:
 * - pid 저장용도.
 * - CONTEXTIDR_EL1, Context ID Register (EL1) 
 *   AArch64 상태에서 CONTEXTIDR_EL1은 ASID와 독립적이며 EL1&0 변환 체제의 
 *   경우 TTBR0_EL1 또는 TTBR1_EL1이 ASID를 보유합니다.
 */
static inline void contextidr_thread_switch(struct task_struct *next)
{
	if (!IS_ENABLED(CONFIG_PID_IN_CONTEXTIDR))
		return;

	write_sysreg(task_pid_nr(next), contextidr_el1);
	isb();
}

/*
 * Set TTBR0 to reserved_pg_dir. No translations will be possible via TTBR0.
 */
/*
 * IAMROOT, 2021.10.16:
 * - ttbr0를 비우게 하기 위해 zero-page로 할당하는 역할
 */
static inline void cpu_set_reserved_ttbr0(void)
{
	unsigned long ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));

	write_sysreg(ttbr, ttbr0_el1);
	isb();
}

void cpu_do_switch_mm(phys_addr_t pgd_phys, struct mm_struct *mm);

/*
 * IAMROOT, 2021.10.30:
 * - user공간 mm을 교체하기 위한 함수.,
 *
 * - booting중에 idmap로 잠시 교체할때 idmap도 user 영역인 ttbr0를 사용하므로 이
 *   함수를 사용한다.
 */
static inline void cpu_switch_mm(pgd_t *pgd, struct mm_struct *mm)
{
	BUG_ON(pgd == swapper_pg_dir);
	cpu_set_reserved_ttbr0();
	cpu_do_switch_mm(virt_to_phys(pgd),mm);
}

/*
 * TCR.T0SZ value to use when the ID map is active. Usually equals
 * TCR_T0SZ(VA_BITS), unless system RAM is positioned very high in
 * physical memory, in which case it will be smaller.
 */
extern u64 idmap_t0sz;
extern u64 idmap_ptrs_per_pgd;

/*
 * Ensure TCR.T0SZ is set to the provided value.
 */
/*
 * IAMROOT, 2021.10.30:
 * -----(old 5.10)
 * - tcr_el1 에서 ttbr0 va사이즈를 재설정하는데, 그전에 설정한 값과 같은 경우라면
 *   하지 않는다.
 * ------
 *  - tcr_el1을 t0sz로 설정하는데, 이미 t0sz라면 설정하지 않는다.
 */
static inline void __cpu_set_tcr_t0sz(unsigned long t0sz)
{
	unsigned long tcr = read_sysreg(tcr_el1);

	if ((tcr & TCR_T0SZ_MASK) >> TCR_T0SZ_OFFSET == t0sz)
		return;

	tcr &= ~TCR_T0SZ_MASK;
	tcr |= t0sz << TCR_T0SZ_OFFSET;
	write_sysreg(tcr, tcr_el1);
	isb();
}

/*
 * IAMROOT, 2021.10.16:
 * - id mdapping을 할때 특수한 경우에 대해서 vabits_actual을 좀 다르게 설정해야되는
 *   경우가 있었는데 여기서 이제 vabits_actual로 완전히 설정하는것.
 */
#define cpu_set_default_tcr_t0sz()	__cpu_set_tcr_t0sz(TCR_T0SZ(vabits_actual))
#define cpu_set_idmap_tcr_t0sz()	__cpu_set_tcr_t0sz(idmap_t0sz)

/*
 * Remove the idmap from TTBR0_EL1 and install the pgd of the active mm.
 *
 * The idmap lives in the same VA range as userspace, but uses global entries
 * and may use a different TCR_EL1.T0SZ. To avoid issues resulting from
 * speculative TLB fetches, we must temporarily install the reserved page
 * tables while we invalidate the TLBs and set up the correct TCR_EL1.T0SZ.
 *
 * If current is a not a user task, the mm covers the TTBR1_EL1 page tables,
 * which should not be installed in TTBR0_EL1. In this case we can leave the
 * reserved page tables in place.
 */
/*
 * IAMROOT, 2021.10.30:
 * - idmap이 사용이 끝낫으므로 ttbr0을 다시 비운다.
 */
static inline void cpu_uninstall_idmap(void)
{
	struct mm_struct *mm = current->active_mm;

	cpu_set_reserved_ttbr0();
/*
 * IAMROOT, 2021.10.16:
 * - page table을 위에서 비웠기때문에 tlb cache를 flush해주는것.
 */
	local_flush_tlb_all();
	cpu_set_default_tcr_t0sz();

/*
 * IAMROOT, 2021.10.16:
 * - cpu_replace_ttbr1 에서 호출되는
 * - kernel에서 kernel로 변경을 하는건 의미가 없으므로 mm != init_mm이 있다.
 */
	if (mm != &init_mm && !system_uses_ttbr0_pan())
		cpu_switch_mm(mm->pgd, mm);
}

/*
 * IAMROOT, 2021.10.30:
 * - idmap을 사용하기 위해 ttbr register를 설정한다.
 * - cpu_switch_mm에서 ttbr1도 설정하지만 사실상 ttbr1은 그대로 쓰는거나 마찬가지라
 *   변함이없지만 idmap자체가 user용 ttbr0을 쓰는거라 user용 ttbr0교체 함수인
 *   cpu_switch_mm을 사용하는것이다.
 */
static inline void cpu_install_idmap(void)
{
/*
 * IAMROOT, 2021.10.30:
 * - ttbr0원래 user용도지만 는 head.S부터 임시로 idmap으로 사용하고 잇었다.
 *   idmap을 임시로 다시 사용하기위해 ttbr0를 reserved 시켜준다.
 */
	cpu_set_reserved_ttbr0();
	local_flush_tlb_all();
	cpu_set_idmap_tcr_t0sz();

/*
 * IAMROOT, 2021.10.30:
 * - 공통함수를 사용하기위해 init_mm을 그냥 사용했다.
 */
	cpu_switch_mm(lm_alias(idmap_pg_dir), &init_mm);
}

/*
 * Atomically replaces the active TTBR1_EL1 PGD with a new VA-compatible PGD,
 * avoiding the possibility of conflicting TLB entries being allocated.
 */
/*
 * IAMROOT, 2021.11.13:
 * - 5.10 -> 5.15 변경사항
 *   __nocfi 추가
 *
 * - Git blame을 참고
 *   Disable CFI checking for functions that switch to linear mapping and
 *   make an indirect call to a physical address, since the compiler only
 *   understands virtual addresses and the CFI check for such indirect calls
 *   would always fail.
 */
static inline void __nocfi cpu_replace_ttbr1(pgd_t *pgdp)
{
/*
 * IAMROOT, 2021.10.30:
 * - mm/proc.S 에 해당 함수 존재.
 */
	typedef void (ttbr_replace_func)(phys_addr_t);
	extern ttbr_replace_func idmap_cpu_replace_ttbr1;
	ttbr_replace_func *replace_phys;

	/* phys_to_ttbr() zeros lower 2 bits of ttbr with 52-bit PA */
	phys_addr_t ttbr1 = phys_to_ttbr(virt_to_phys(pgdp));

	if (system_supports_cnp() && !WARN_ON(pgdp != lm_alias(swapper_pg_dir))) {
		/*
		 * cpu_replace_ttbr1() is used when there's a boot CPU
		 * up (i.e. cpufeature framework is not up yet) and
		 * latter only when we enable CNP via cpufeature's
		 * enable() callback.
		 * Also we rely on the cpu_hwcap bit being set before
		 * calling the enable() function.
		 */
		ttbr1 |= TTBR_CNP_BIT;
	}

	replace_phys = (void *)__pa_symbol(function_nocfi(idmap_cpu_replace_ttbr1));

	cpu_install_idmap();
	replace_phys(ttbr1);
	cpu_uninstall_idmap();
}

/*
 * It would be nice to return ASIDs back to the allocator, but unfortunately
 * that introduces a race with a generation rollover where we could erroneously
 * free an ASID allocated in a future generation. We could workaround this by
 * freeing the ASID from the context of the dying mm (e.g. in arch_exit_mmap),
 * but we'd then need to make sure that we didn't dirty any TLBs afterwards.
 * Setting a reserved TTBR0 or EPD0 would work, but it all gets ugly when you
 * take CPU migration into account.
 */
void check_and_switch_context(struct mm_struct *mm);

#define init_new_context(tsk, mm) init_new_context(tsk, mm)
static inline int
init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	atomic64_set(&mm->context.id, 0);
	refcount_set(&mm->context.pinned, 0);
	return 0;
}

#ifdef CONFIG_ARM64_SW_TTBR0_PAN

/*
 * IAMROOT, 2022.11.26:
 * - sw기능 사용할때만 동작한다.
 */
static inline void update_saved_ttbr0(struct task_struct *tsk,
				      struct mm_struct *mm)
{
	u64 ttbr;

	if (!system_uses_ttbr0_pan())
		return;

	if (mm == &init_mm)
		ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
	else
		ttbr = phys_to_ttbr(virt_to_phys(mm->pgd)) | ASID(mm) << 48;

	WRITE_ONCE(task_thread_info(tsk)->ttbr0, ttbr);
}
#else
static inline void update_saved_ttbr0(struct task_struct *tsk,
				      struct mm_struct *mm)
{
}
#endif

#define enter_lazy_tlb enter_lazy_tlb
static inline void
/*
 * IAMROOT, 2022.11.26:
 * - update만 하고 나중에 갱신
 */
enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
	/*
	 * We don't actually care about the ttbr0 mapping, so point it at the
	 * zero page.
	 */
	update_saved_ttbr0(tsk, &init_mm);
}

/*
 * IAMROOT, 2023.01.28:
 * - next가 kernel : 교체안한다. ttbr0를 rserved로 set.
 *   next가 user   : @next로 mm교체.
 */
static inline void __switch_mm(struct mm_struct *next)
{
	/*
	 * init_mm.pgd does not contain any user mappings and it is always
	 * active for kernel addresses in TTBR1. Just set the reserved TTBR0.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - next가 kernel이면 kernel의 user접근을 막기위해
 *   ttbr0를 비운다.
 */
	if (next == &init_mm) {
		cpu_set_reserved_ttbr0();
		return;
	}

	check_and_switch_context(next);
}

/*
 * IAMROOT, 2023.01.28:
 * - @next로 mm교체
 */
static inline void
switch_mm(struct mm_struct *prev, struct mm_struct *next,
	  struct task_struct *tsk)
{
	if (prev != next)
		__switch_mm(next);

	/*
	 * Update the saved TTBR0_EL1 of the scheduled-in task as the previous
	 * value may have not been initialised yet (activate_mm caller) or the
	 * ASID has changed since the last run (following the context switch
	 * of another thread of the same process).
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   이전 값이 아직 초기화되지 않았거나(activate_mm 호출자) 마지막 실행 
 *   이후 ASID가 변경되었을 수 있으므로 예약된 작업의 저장된 TTBR0_EL1을 
 *   업데이트합니다(동일한 프로세스의 다른 스레드의 컨텍스트 전환 후).
 */
	update_saved_ttbr0(tsk, next);
}

static inline const struct cpumask *
task_cpu_possible_mask(struct task_struct *p)
{
	if (!static_branch_unlikely(&arm64_mismatched_32bit_el0))
		return cpu_possible_mask;

	if (!is_compat_thread(task_thread_info(p)))
		return cpu_possible_mask;

	return system_32bit_el0_cpumask();
}
#define task_cpu_possible_mask	task_cpu_possible_mask

void verify_cpu_asid_bits(void);
void post_ttbr_update_workaround(void);

unsigned long arm64_mm_context_get(struct mm_struct *mm);
void arm64_mm_context_put(struct mm_struct *mm);

#include <asm-generic/mmu_context.h>

#endif /* !__ASSEMBLY__ */

#endif /* !__ASM_MMU_CONTEXT_H */
