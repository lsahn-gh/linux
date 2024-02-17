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
 * - Git blame
 *   하드웨어 트레이스를 분석할 때 유용합니다. 이 레지스터에 대한 쓰기가 
 *   트레이스 스트림으로 이벤트를 내보내도록 구성될 수 있기 때문입니다. 
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
/* IAMROOT, 2021.10.16:
 * - reserved_pg_dir을 ttbr0에 설정하고 instruction pipeline을 비운다.
 */
static inline void cpu_set_reserved_ttbr0(void)
{
	unsigned long ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));

	write_sysreg(ttbr, ttbr0_el1);
	isb();
}

void cpu_do_switch_mm(phys_addr_t pgd_phys, struct mm_struct *mm);

/* IAMROOT, 2021.10.30:
 * - el0가 사용하는 ttbr0의 pgdir을 교체하기 위한 wrapper 함수.
 *   보안을 위해 교체하기 이전에 ttbr0를 reserved_pg_dir로 미리 mapping 하고
 *   난 뒤에 @pgd로 re-mapping 한다.
 *   @mm는 ttbr1의 asid 값을 parsing 하기 위해 사용한다.
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
/* IAMROOT, 2021.10.30:
 * - tcr_el1.t0sz에 @t0sz 값을 설정하는데 미리읽어온 값이 @t0sz와 같다면
 *   설정하지 않고 그냥 return 한다. 마지막에 instruction pipeline을
 *   flush 하므로 이미 값이 같다면 이러한 overhead를 발생시킬 이유가 없다.
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

/* IAMROOT, 2021.10.16:
 * - cpu_set_default_tcr_t0sz():
 *   idmap을 수행할 때 특수한 경우에 대해 vabits_actual을 다르게
 *   설정했는데 여기서 real vabits_actual로 다시 설정한다.
 * - cpu_set_idmap_tcr_t0sz():
 *   ttbr0에 idmap pgd을 사용하기 위해 idmap_t0sz을 설정한다.
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
/* IAMROOT, 2021.10.30:
 * - idmap 사용이 끝났으므로 ttbr0에서 unmapping 하고 다른 것으로 교체한다.
 *   mm == init_mm: reserved_pg_dir
 *   mm != init_mm: mm->pgd
 */
static inline void cpu_uninstall_idmap(void)
{
	struct mm_struct *mm = current->active_mm;

	/* IAMROOT, 2024.02.10:
	 * - ttbr0를 reserved_pg_dir로 mapping하고 tlb cache를 flush 한다.
	 */
	cpu_set_reserved_ttbr0();
	local_flush_tlb_all();
	cpu_set_default_tcr_t0sz();

	/* IAMROOT, 2021.10.16:
	 * - @mm == init_mm이라면 kernel 영역이고 kernel의 pgdir을 ttbr0에
	 *   설정하게됨. kernel 보안에 허점이 발생하므로 kernel pgdir이 ttbr0에
	 *   설정되지 않도록 '@mm != init_mm'인 경우에만 pgdir을 re-mapping 하게끔
	 *   작성되어 있음. 이 경우에는 ttbr0이 계속 reserved_pg_dir을 가리킴.
	 */
	if (mm != &init_mm && !system_uses_ttbr0_pan())
		cpu_switch_mm(mm->pgd, mm);
}

/* IAMROOT, 2021.10.30:
 * - ttbr1의 pgdir을 변경하기 이전에 idmap_pg_dir을 임시로 사용하기 위해
 *   ttbr0에 idmap_pg_dir을 mapping 하는 작업을 수행한다.
 */
static inline void cpu_install_idmap(void)
{
	/* IAMROOT, 2021.10.30:
	 * - ttbr0은 el0 apps이 사용하는 page table 용도지만 커널의 pgd를
	 *   변경할 때도 사용하므로 이 경우를 위해 setup 한다.
	 */
	cpu_set_reserved_ttbr0();
	local_flush_tlb_all();
	cpu_set_idmap_tcr_t0sz();

	/* IAMROOT, 2021.10.30:
	 * - ttbr0의 pgdir를 idmap_pg_dir로 mapping 한다.
	 *   ttbr0을 변경하므로 el0의 pgdir을 변경하는데 사용하는 함수를
	 *   그대로 사용한다.
	 */
	cpu_switch_mm(lm_alias(idmap_pg_dir), &init_mm);
}

/*
 * Atomically replaces the active TTBR1_EL1 PGD with a new VA-compatible PGD,
 * avoiding the possibility of conflicting TLB entries being allocated.
 */
/* IAMROOT, 2021.11.13:
 * - 현재 ttbr1의 내용을 @pgdp로 교체한다.
 */
static inline void __nocfi cpu_replace_ttbr1(pgd_t *pgdp)
{
	typedef void (ttbr_replace_func)(phys_addr_t);
	extern ttbr_replace_func idmap_cpu_replace_ttbr1;
	ttbr_replace_func *replace_phys;

	/* IAMROOT, 2024.02.11:
	 * - vaddr(@pgdp)를 paddr로 변환한다.
	 */
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

	/* IAMROOT, 2024.02.11:
	 * - replace에 사용할 'idmap_cpu_replace_ttbr1' function 선언.
	 */
	replace_phys = (void *)__pa_symbol(function_nocfi(idmap_cpu_replace_ttbr1));

	/* IAMROOT, 2024.02.11:
	 * - ttbr1_el1의 pgdir 값을 @ttbr1로 mapping 한다.
	 *
	 *   다음과 같이 변경되어도 init_mm.pgd 값은 여전히 init_pg_dir 임에
	 *   주의하라.
	 */
	cpu_install_idmap();
	/* IAMROOT, 2024.02.12:
	 * - 현재 변수별 pgdir 값:
	 *   @ttbr1   : swapper_pg_dir
	 *   ttbr1_el1: init_pg_dir
	 *   ttbr0_el1: idmap_pg_dir
	 */
	replace_phys(ttbr1);
	/* IAMROOT, 2024.02.12:
	 * - 현재 변수별 pgdir 값:
	 *   @ttbr1   : swapper_pg_dir
	 *   ttbr1_el1: swapper_pg_dir
	 *   ttbr0_el1: idmap_pg_dir
	 */
	cpu_uninstall_idmap();
	/* IAMROOT, 2024.02.12:
	 * - 현재 변수별 pgdir 값:
	 *   @ttbr1   : swapper_pg_dir
	 *   ttbr1_el1: swapper_pg_dir
	 *   ttbr0_el1: reserved_pg_dir (if mm == init_mm)
	 */
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


/*
 * IAMROOT, 2023.04.01:
 * - context id 초기화.
 */
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

/*
 * IAMROOT, 2023.04.13:
 * - @p가 들어갈수있는 cpumask를 return 한다.
 * - system 상태, @p의 el0 32bit mode 여부에 따른 cpu mask를 return한다.
 *   최초에 32bit mode 구별이 없이 동일한 cpu를 사용한다면 cpu_possible_mask
 *   가 일반적으로 return된다.
 *   그 상황이 아니라면,
 *
 *   @p가 32bit mode가 아니라면 cpu_possible_mask,
 *   @p가 32bit mode인데 system이 el0 32bit mode 미지원 이면 cpu_none_mask,
 *   @p가 32bit mode인데 system이 el0 32bit mode 지원 이면 cpu_32bit_el0_mask
 *   가 선택된다.
 *
 *   arm64_      | 32bitmode | system_     | return
 *   mismatched_ |           | 32bit_      |
 *   32bit_el0   |           | el0_cpumask |
 *   ------------+-----------+-------------+-----------------------------+
 *   disable     | -         | -           | cpu_possible_mask
 *   ------------+-----------+-------------+-----------------------------+
 *   enable      | disable   | -           | cpu_possible_mask
 *   ------------+-----------+-------------+-----------------------------+
 *   enable      | enable    | 32bit mode  | cpu_none_mask
 *                             not support | 
 *   ------------+-----------+-------------+-----------------------------+
 *   enable      | enable    | 32bit mode   | cpu_32bit_el0_mask
 *                             support
 *   ------------+-----------+-------------+-----------------------------+
 */
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
