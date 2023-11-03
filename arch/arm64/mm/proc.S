/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/mm/proc.S
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/pgtable.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/asm_pointer_auth.h>
#include <asm/hwcap.h>
#include <asm/pgtable-hwdef.h>
#include <asm/cpufeature.h>
#include <asm/alternative.h>
#include <asm/smp.h>
#include <asm/sysreg.h>

#ifdef CONFIG_ARM64_64K_PAGES
#define TCR_TG_FLAGS	TCR_TG0_64K | TCR_TG1_64K
#elif defined(CONFIG_ARM64_16K_PAGES)
#define TCR_TG_FLAGS	TCR_TG0_16K | TCR_TG1_16K
#else /* CONFIG_ARM64_4K_PAGES */
#define TCR_TG_FLAGS	TCR_TG0_4K | TCR_TG1_4K
#endif

#ifdef CONFIG_RANDOMIZE_BASE
#define TCR_KASLR_FLAGS	TCR_NFD1
#else
#define TCR_KASLR_FLAGS	0
#endif

#define TCR_SMP_FLAGS	TCR_SHARED

/* PTWs cacheable, inner/outer WBWA */
#define TCR_CACHE_FLAGS	TCR_IRGN_WBWA | TCR_ORGN_WBWA

#ifdef CONFIG_KASAN_SW_TAGS
#define TCR_KASAN_SW_FLAGS TCR_TBI1 | TCR_TBID1
#else
#define TCR_KASAN_SW_FLAGS 0
#endif

#ifdef CONFIG_KASAN_HW_TAGS
#define TCR_MTE_FLAGS SYS_TCR_EL1_TCMA1 | TCR_TBI1 | TCR_TBID1
#else
/*
 * The mte_zero_clear_page_tags() implementation uses DC GZVA, which relies on
 * TBI being enabled at EL1.
 */
#define TCR_MTE_FLAGS TCR_TBI1 | TCR_TBID1
#endif

/* IAMROOT, 2021.08.21:
 * - el1 mair reg 값 세팅에 필요한 attrs를 정의한다.
 *   memory 제어에는 여러 종류가 있지만 linux kernel은 아래 5가지만 선택하여
 *   사용한다.
 *
 *   mair: memory attribute indirection reg.
 */
/*
 * Default MAIR_EL1. MT_NORMAL_TAGGED is initially mapped as Normal memory and
 * changed during __cpu_setup to Normal Tagged if the system supports MTE.
 */
#define MAIR_EL1_SET							\
	(MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRnE, MT_DEVICE_nGnRnE) |	\
	 MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRE, MT_DEVICE_nGnRE) |	\
	 MAIR_ATTRIDX(MAIR_ATTR_NORMAL_NC, MT_NORMAL_NC) |		\
	 MAIR_ATTRIDX(MAIR_ATTR_NORMAL, MT_NORMAL) |			\
	 MAIR_ATTRIDX(MAIR_ATTR_NORMAL, MT_NORMAL_TAGGED))

#ifdef CONFIG_CPU_PM
/**
 * cpu_do_suspend - save CPU registers context
 *
 * x0: virtual address of context pointer
 *
 * This must be kept in sync with struct cpu_suspend_ctx in <asm/suspend.h>.
 */
SYM_FUNC_START(cpu_do_suspend)
	mrs	x2, tpidr_el0
	mrs	x3, tpidrro_el0
	mrs	x4, contextidr_el1
	mrs	x5, osdlr_el1
	mrs	x6, cpacr_el1
	mrs	x7, tcr_el1
	mrs	x8, vbar_el1
	mrs	x9, mdscr_el1
	mrs	x10, oslsr_el1
	mrs	x11, sctlr_el1
	get_this_cpu_offset x12
	mrs	x13, sp_el0
	stp	x2, x3, [x0]
	stp	x4, x5, [x0, #16]
	stp	x6, x7, [x0, #32]
	stp	x8, x9, [x0, #48]
	stp	x10, x11, [x0, #64]
	stp	x12, x13, [x0, #80]
	/*
	 * Save x18 as it may be used as a platform register, e.g. by shadow
	 * call stack.
	 */
	str	x18, [x0, #96]
	ret
SYM_FUNC_END(cpu_do_suspend)

/**
 * cpu_do_resume - restore CPU register context
 *
 * x0: Address of context pointer
 */
	.pushsection ".idmap.text", "awx"
SYM_FUNC_START(cpu_do_resume)
	ldp	x2, x3, [x0]
	ldp	x4, x5, [x0, #16]
	ldp	x6, x8, [x0, #32]
	ldp	x9, x10, [x0, #48]
	ldp	x11, x12, [x0, #64]
	ldp	x13, x14, [x0, #80]
	/*
	 * Restore x18, as it may be used as a platform register, and clear
	 * the buffer to minimize the risk of exposure when used for shadow
	 * call stack.
	 */
	ldr	x18, [x0, #96]
	str	xzr, [x0, #96]
	msr	tpidr_el0, x2
	msr	tpidrro_el0, x3
	msr	contextidr_el1, x4
	msr	cpacr_el1, x6

	/* Don't change t0sz here, mask those bits when restoring */
	mrs	x7, tcr_el1
	bfi	x8, x7, TCR_T0SZ_OFFSET, TCR_TxSZ_WIDTH

	msr	tcr_el1, x8
	msr	vbar_el1, x9

	/*
	 * __cpu_setup() cleared MDSCR_EL1.MDE and friends, before unmasking
	 * debug exceptions. By restoring MDSCR_EL1 here, we may take a debug
	 * exception. Mask them until local_daif_restore() in cpu_suspend()
	 * resets them.
	 */
	disable_daif
	msr	mdscr_el1, x10

	msr	sctlr_el1, x12
	set_this_cpu_offset x13
	msr	sp_el0, x14
	/*
	 * Restore oslsr_el1 by writing oslar_el1
	 */
	msr	osdlr_el1, x5
	ubfx	x11, x11, #1, #1
	msr	oslar_el1, x11
	reset_pmuserenr_el0 x0			// Disable PMU access from EL0
	reset_amuserenr_el0 x0			// Disable AMU access from EL0

alternative_if ARM64_HAS_RAS_EXTN
	msr_s	SYS_DISR_EL1, xzr
alternative_else_nop_endif

	ptrauth_keys_install_kernel_nosync x14, x1, x2, x3
	isb
	ret
SYM_FUNC_END(cpu_do_resume)
	.popsection
#endif

	.pushsection ".idmap.text", "awx"

/*
 * IAMROOT, 2021.10.30:
 * - reserved(zero) page로 ttbr1을 설정해준다.
 */
.macro	__idmap_cpu_set_reserved_ttbr1, tmp1, tmp2
	adrp	\tmp1, reserved_pg_dir
	phys_to_ttbr \tmp2, \tmp1
	offset_ttbr1 \tmp2, \tmp1
	msr	ttbr1_el1, \tmp2
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
.endm

/*
 * void idmap_cpu_replace_ttbr1(phys_addr_t ttbr1)
 *
 * This is the low-level counterpart to cpu_replace_ttbr1, and should not be
 * called by anything else. It can only be executed from a TTBR0 mapping.
 */
/*
 * IAMROOT, 2021.10.30:
 * - idmap은 이미 ttbr0에 설정되있고, 그후 ttbr1을 재설정하기 위해 진입하는 함수.
 
 * - 스터디 최초에 봤을때 cpu_replace_ttbr1에서 호출되었으며 x0는 ttbr1
 */
SYM_FUNC_START(idmap_cpu_replace_ttbr1)
	save_and_disable_daif flags=x2

	__idmap_cpu_set_reserved_ttbr1 x1, x3

	offset_ttbr1 x0, x3
	msr	ttbr1_el1, x0
	isb

	restore_daif x2

	ret
SYM_FUNC_END(idmap_cpu_replace_ttbr1)
	.popsection

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	.pushsection ".idmap.text", "awx"

	.macro	__idmap_kpti_get_pgtable_ent, type
	dc	cvac, cur_\()\type\()p		// Ensure any existing dirty
	dmb	sy				// lines are written back before
	ldr	\type, [cur_\()\type\()p]	// loading the entry
	tbz	\type, #0, skip_\()\type	// Skip invalid and
	tbnz	\type, #11, skip_\()\type	// non-global entries
	.endm

	.macro __idmap_kpti_put_pgtable_ent_ng, type
	orr	\type, \type, #PTE_NG		// Same bit for blocks and pages
	str	\type, [cur_\()\type\()p]	// Update the entry and ensure
	dmb	sy				// that it is visible to all
	dc	civac, cur_\()\type\()p		// CPUs.
	.endm

/*
 * void __kpti_install_ng_mappings(int cpu, int num_cpus, phys_addr_t swapper)
 *
 * Called exactly once from stop_machine context by each CPU found during boot.
 */
__idmap_kpti_flag:
	.long	1
SYM_FUNC_START(idmap_kpti_install_ng_mappings)
	cpu		.req	w0
	num_cpus	.req	w1
	swapper_pa	.req	x2
	swapper_ttb	.req	x3
	flag_ptr	.req	x4
	cur_pgdp	.req	x5
	end_pgdp	.req	x6
	pgd		.req	x7
	cur_pudp	.req	x8
	end_pudp	.req	x9
	pud		.req	x10
	cur_pmdp	.req	x11
	end_pmdp	.req	x12
	pmd		.req	x13
	cur_ptep	.req	x14
	end_ptep	.req	x15
	pte		.req	x16

	mrs	swapper_ttb, ttbr1_el1
	restore_ttbr1	swapper_ttb
	adr	flag_ptr, __idmap_kpti_flag

	cbnz	cpu, __idmap_kpti_secondary

	/* We're the boot CPU. Wait for the others to catch up */
	sevl
1:	wfe
	ldaxr	w17, [flag_ptr]
	eor	w17, w17, num_cpus
	cbnz	w17, 1b

	/* We need to walk swapper, so turn off the MMU. */
	pre_disable_mmu_workaround
	mrs	x17, sctlr_el1
	bic	x17, x17, #SCTLR_ELx_M
	msr	sctlr_el1, x17
	isb

	/* Everybody is enjoying the idmap, so we can rewrite swapper. */
	/* PGD */
	mov	cur_pgdp, swapper_pa
	add	end_pgdp, cur_pgdp, #(PTRS_PER_PGD * 8)
do_pgd:	__idmap_kpti_get_pgtable_ent	pgd
	tbnz	pgd, #1, walk_puds
next_pgd:
	__idmap_kpti_put_pgtable_ent_ng	pgd
skip_pgd:
	add	cur_pgdp, cur_pgdp, #8
	cmp	cur_pgdp, end_pgdp
	b.ne	do_pgd

	/* Publish the updated tables and nuke all the TLBs */
	dsb	sy
	tlbi	vmalle1is
	dsb	ish
	isb

	/* We're done: fire up the MMU again */
	mrs	x17, sctlr_el1
	orr	x17, x17, #SCTLR_ELx_M
	set_sctlr_el1	x17

	/* Set the flag to zero to indicate that we're all done */
	str	wzr, [flag_ptr]
	ret

	/* PUD */
walk_puds:
	.if CONFIG_PGTABLE_LEVELS > 3
	pte_to_phys	cur_pudp, pgd
	add	end_pudp, cur_pudp, #(PTRS_PER_PUD * 8)
do_pud:	__idmap_kpti_get_pgtable_ent	pud
	tbnz	pud, #1, walk_pmds
next_pud:
	__idmap_kpti_put_pgtable_ent_ng	pud
skip_pud:
	add	cur_pudp, cur_pudp, 8
	cmp	cur_pudp, end_pudp
	b.ne	do_pud
	b	next_pgd
	.else /* CONFIG_PGTABLE_LEVELS <= 3 */
	mov	pud, pgd
	b	walk_pmds
next_pud:
	b	next_pgd
	.endif

	/* PMD */
walk_pmds:
	.if CONFIG_PGTABLE_LEVELS > 2
	pte_to_phys	cur_pmdp, pud
	add	end_pmdp, cur_pmdp, #(PTRS_PER_PMD * 8)
do_pmd:	__idmap_kpti_get_pgtable_ent	pmd
	tbnz	pmd, #1, walk_ptes
next_pmd:
	__idmap_kpti_put_pgtable_ent_ng	pmd
skip_pmd:
	add	cur_pmdp, cur_pmdp, #8
	cmp	cur_pmdp, end_pmdp
	b.ne	do_pmd
	b	next_pud
	.else /* CONFIG_PGTABLE_LEVELS <= 2 */
	mov	pmd, pud
	b	walk_ptes
next_pmd:
	b	next_pud
	.endif

	/* PTE */
walk_ptes:
	pte_to_phys	cur_ptep, pmd
	add	end_ptep, cur_ptep, #(PTRS_PER_PTE * 8)
do_pte:	__idmap_kpti_get_pgtable_ent	pte
	__idmap_kpti_put_pgtable_ent_ng	pte
skip_pte:
	add	cur_ptep, cur_ptep, #8
	cmp	cur_ptep, end_ptep
	b.ne	do_pte
	b	next_pmd

	.unreq	cpu
	.unreq	num_cpus
	.unreq	swapper_pa
	.unreq	cur_pgdp
	.unreq	end_pgdp
	.unreq	pgd
	.unreq	cur_pudp
	.unreq	end_pudp
	.unreq	pud
	.unreq	cur_pmdp
	.unreq	end_pmdp
	.unreq	pmd
	.unreq	cur_ptep
	.unreq	end_ptep
	.unreq	pte

	/* Secondary CPUs end up here */
__idmap_kpti_secondary:
	/* Uninstall swapper before surgery begins */
	__idmap_cpu_set_reserved_ttbr1 x16, x17

	/* Increment the flag to let the boot CPU we're ready */
1:	ldxr	w16, [flag_ptr]
	add	w16, w16, #1
	stxr	w17, w16, [flag_ptr]
	cbnz	w17, 1b

	/* Wait for the boot CPU to finish messing around with swapper */
	sevl
1:	wfe
	ldxr	w16, [flag_ptr]
	cbnz	w16, 1b

	/* All done, act like nothing happened */
	offset_ttbr1 swapper_ttb, x16
	msr	ttbr1_el1, swapper_ttb
	isb
	ret

	.unreq	swapper_ttb
	.unreq	flag_ptr
SYM_FUNC_END(idmap_kpti_install_ng_mappings)
	.popsection
#endif

/*
 *	__cpu_setup
 *
 *	Initialise the processor for turning the MMU on.
 *
 * Output:
 *	Return in x0 the value of the SCTLR_EL1 register.
 */
	.pushsection ".idmap.text", "awx"

/* IAMROOT, 2022.01.28:
 * - mair_el1, tcr_el1을 설정한다.
 */
SYM_FUNC_START(__cpu_setup)
/* IAMROOT, 2021.08.21:
 * - tlbi: Translation Lookaside Buffer Invalidate
 *   vmallel1: vm + all + el1
 *   - vm     : 현재 OS
 *     all    : stage1의 모든 tlb cache
 *     alls12 : stage1, stage2의 모든 tlb cache
 *     el1    : exception level 1
 *     vmall  : 현재 OS에 해당하는 stage1의 모든 tlb cache를 invalidation.
 *
 * - Program Ref p165
 *   ASID : 특정 ASID에 해당하는 tlb를 지운다는듯.
 *   VA : ASID + VA
 *   VAA : any ASID + VA
 *   - VMALLE1 TLB invalidate by VMID, All at stage 1, EL1
 *
 * - dsb: data synchronization barrier
 *   nsh: Non-shareable
 */
	tlbi	vmalle1				// Invalidate local TLB
	dsb	nsh

/* IAMROOT, 2021.08.21:
 * - cpacr.fpen[21:20] bits를 1로 세팅.
 *   trace, SVE, Advanced SIMD, floating-point 명령어 실행시 trap 하지 않도록
 *   off 함. 결국 해당 명령어들을 사용하겠다는 의미이다.
 *
 * - cpacr: architectural feature access control reg.
 *   trace, SVE, Advanced SIMD, floating-point 기능에 대해 접근 제어.
 */
	mov	x1, #3 << 20
	msr	cpacr_el1, x1			// Enable FP/ASIMD

/* IAMROOT, 2023.10.27:
 * - mdscr.tdcc: EL0가 DCC (Debug Communication Channel) 레지스터 접근시
 *               trap 발생하도록 bit 설정.
 *   DCC  : PE 와 debugger 사이에 데이터를 주고 받기 위한 채널.
 *   mdscr: monitor debug system control reg.
 *
 * - enable_dbg : PSTATE에서 DAIF의 D bit 클리어 (exception 허용).
 */
	mov	x1, #1 << 12			// Reset mdscr_el1 and disable
	msr	mdscr_el1, x1			// access to the DCC from EL0
	isb					// Unmask debug exceptions now,
	enable_dbg				// since this is per-cpu

/* IAMROOT, 2021.08.21:
 * - el0의 PMU, AMU 접근을 disable (trap) 한다.
 *   PMU (Performance Monitors), AMU (Activity Monitors)
 *   https://developer.arm.com/documentation/ka004659/latest
 */
	reset_pmuserenr_el0 x1			// Disable PMU access from EL0
	reset_amuserenr_el0 x1			// Disable AMU access from EL0

	/*
	 * Default values for VMSA control registers. These will be adjusted
	 * below depending on detected CPU features.
	 */
/* IAMROOT, 2022.01.28:
 * - @x17에 접근할 수 있는 mair 라는 이름의 alias 정의.
 *   @x16에 접근할 수 있는 tcr 라는 이름의 alias 정의.
 */
	mair	.req	x17
	tcr	.req	x16

/* IAMROOT, 2023.10.28:
 * - mair_el1과 tcr_el1 reg에 설정할 각각의 bits 값 세팅.
 *
 * -- mair_el1: memory attribute indirection reg for el1
 *    MAIR_EL1_SET를 따라가면 정의된 attrindex를 확인할 수 있다.
 *
 * -- tcr_el1 : translation control reg for el1
 *    1. TCR_TxSZ
 *       - T0SZ[5:0]
 *       - T1SZ[21:16]
 *         TTBR{0/1}_EL1의 memory region addr 크기 offset.
 *         region 크기는 2^(64-T{0/1}SZ) bytes 이다.
 *    2. TCR_CACHE_FLAGS: cache를 Inner/Outer WBWA로 설정 (0b01)
 *       - I[nner]RGN0[9:8]:
 *       - O[uter]RGN0[11:10]:
 *       - I[nner]RGN1[25:24]:
 *       - O[uter]RGN1[27:26]:
 *         TTBR{0/1}_EL1을 통해 translation table walk시 사용될 memory와 관련된
 *         {inner/outer} cacheability attr 설정.
 *             0b00: Normal memory, {Inner/Outer} Non-cacheable
 *             0b01: Normal memory, {Inner/Outer} Write-Back Read-Allocate
 *                                  Write-Allocate Cacheable
 *             0b10: Normal memory, {Inner/Outer} Write-Through Read-Allocate
 *                                  No Write-Allocate Cacheable
 *             0b11: Normal memory, {Inner/Outer} Write-Back Read-Allocate
 *                                  No Write-Allocate Cacheable
 *    3. TCR_SMP_FLAGS: Inner Shareable로 설정 (0b11). Linux가 사용하는 모든
 *                      cpu에서 접근 가능하도록 한다.
 *       - SH0[13:12]:
 *       - SH1[29:28]:
 *         TTBR{0/1}_EL1을 통해 translation table walk시 사용될 memory와 관련된
 *         shareability attr 설정.
 *             0b00: Non-shareable
 *             0b10: Outer Shareable
 *             0b11: Inner Shareable
 *    4. TCR_TG_FLAGS (page size == 4K)
 *       - TG0_4K[15:14]:
 *       - TG1_4K[31:30]:
 *         TTBR{0/1}_EL1에서 사용되는 Granule 크기.
 *             0b01: 16KB
 *             0b10:  4KB
 *             0b11: 64KB
 *    5. TCR_KASLR_FLAGS:
 *       - NFD1[54] if RANDOMIZE_BASE is on
 *         'FEAT_SVE'가 구현되어 있을때만 동작하고 아니면 RES0.
 *         SVE사용시 특정 조건에서 커널 주소공간에 대한 timing attack의 공격을
 *         완하기 위함이라고 하며, 이를 안할경우 SVE load에서
 *         suppressed fault와 FFR을 사용해서 커널 주소 공간을 probe 할수있다고
 *         한다. (git log 참고)
 *    6. TCR_ASID16
 *       - AS[36]: ASID 크기 결정.
 *             0b0: 8 bit, TTBRx_EL1의 상위 8 bit를 무시한다.
 *             0b1: 16 bit, TTBRx_EL1의 상위 16 bit를 TLB allocation과 match에
 *                  사용한다.
 *    7. TCR_TBI0
 *       - TBI0[37]: vaddr의 top byte를 addr match(TTBR0_EL1)에 사용할지
 *                   무시하고 tagged addr에 사용할지 결정.
 *             0b0: vaddr의 top byte를 addr calc에 사용한다.
 *             0b1: vaddr의 top byte를 addr calc에서 제외한다.
 *                  tagged addr에 사용한다는 의미이다.
 *    8. TCR_A1
 *       - A1[22]: TTBR0_EL1 또는 TTBR1_EL1 중 어느것이 ASID를 정의할지 선택.
 *            0b0: TTBR0_EL1.ASID 값을 ASID로 정의.
 *            0b1: TTBR1_EL1.ASID 값을 ASID로 정의.
 *    9. TCR_KASAN_SW_FLAGS
 *       TTBR1_EL1 addr 계산시 top byte를 무시. KASAN을 사용할려면 top byte를
 *       무시해야 한다고 CONFIG_KASAN_SW_TAGS 설명에 나와있음. KASAN을 사용하면
 *       해당영역은 TAG로 사용된다.
 *       - TBI1[38]: vaddr의 top byte를 addr match(TTBR1_EL1)에 사용할지
 *                   무시하고 tagged addr에 사용할지 결정.
 *             0b0: vaddr의 top byte를 addr calc에 사용한다.
 *             0b1: vaddr의 top byte를 addr calc에서 제외한다.
 *                  tagged addr에 사용한다는 의미이다.
 *       - TBID1[52]: TODO
 *         'FEAT_PAuth'가 구현되어 있을때만 동작하고 아니면 RES0.
 */
	mov_q	mair, MAIR_EL1_SET
	mov_q	tcr, TCR_TxSZ(VA_BITS) | TCR_CACHE_FLAGS | TCR_SMP_FLAGS | \
			TCR_TG_FLAGS | TCR_KASLR_FLAGS | TCR_ASID16 | \
			TCR_TBI0 | TCR_A1 | TCR_KASAN_SW_FLAGS

#ifdef CONFIG_ARM64_MTE
	/*
	 * Update MAIR_EL1, GCR_EL1 and TFSR*_EL1 if MTE is supported
	 * (ID_AA64PFR1_EL1[11:8] > 1).
	 */
/* IAMROOT, 2023.10.30:
 * - MTE 기능이 구현되어 있는지 확인해서 미구현이면 '1f'로 점프한다.
 *   0b0000: MTE 기능 미구현.
 *   0b0001: Instruction-only MTE 기능 구현.
 *   0b0010: Full MTE 기능 구현.
 *   0b0011: MTE 기능 및 asymmetric Tag Check Fault handling 지원.
 *
 * - MTE: Memory Tagging Extension
 */
	mrs	x10, ID_AA64PFR1_EL1
	ubfx	x10, x10, #ID_AA64PFR1_MTE_SHIFT, #4
	cmp	x10, #ID_AA64PFR1_MTE
	b.lt	1f

/* IAMROOT, 2021.08.21:
 * - MAIR_EL1_SET directive에서는 MT_NORMAL_TAGGED index의 값을
 *   MAIR_ATTR_NORMAL 설정했지만 현재 MTE 기능을 지원하므로 기존의 값을
 *   MAIR_ATTR_NORMAL_TAGGED로 변경하는 동작을 수행한다.
 */
	/* Normal Tagged memory type at the corresponding MAIR index */
	mov	x10, #MAIR_ATTR_NORMAL_TAGGED
	bfi	mair, x10, #(8 *  MT_NORMAL_TAGGED), #8

/* IAMROOT, 2023.10.31:
 * - Tag Control reg 값을 설정한다.
 */
	mov	x10, #KERNEL_GCR_EL1
	msr_s	SYS_GCR_EL1, x10

	/*
	 * If GCR_EL1.RRND=1 is implemented the same way as RRND=0, then
	 * RGSR_EL1.SEED must be non-zero for IRG to produce
	 * pseudorandom numbers. As RGSR_EL1 is UNKNOWN out of reset, we
	 * must initialize it.
	 */
	mrs	x10, CNTVCT_EL0
	ands	x10, x10, #SYS_RGSR_EL1_SEED_MASK
	csinc	x10, x10, xzr, ne
	lsl	x10, x10, #SYS_RGSR_EL1_SEED_SHIFT
	msr_s	SYS_RGSR_EL1, x10

/* IAMROOT, 2021.08.21:
 * - TFSR_EL1, TFSRE0_EL1의 Tag Check Fault bit 모두 zero로 초기화한다.
 *
 *   TFSR[E0]_EL1 : Tag Check Fault가 el{0/1}에서 발생시 처리 방식 결정.
 *         .TF1[1]: vaddr[55] == 0b1일때 Tag Check Fault가 발생하면 async하게
 *                  해당 bit가 1로 설정된다.
 *         .TF0[0]: vaddr[55] == 0b0일때 Tag Check Fault가 발생하면 async하게
 *                  해당 bit가 1로 설정된다.
 */
	/* clear any pending tag check faults in TFSR*_EL1 */
	msr_s	SYS_TFSR_EL1, xzr
	msr_s	SYS_TFSRE0_EL1, xzr

/* IAMROOT, 2023.10.31: TODO */
	/* set the TCR_EL1 bits */
	mov_q	x10, TCR_MTE_FLAGS
	orr	tcr, tcr, x10
1:
#endif

/* IAMROOT, 2021.08.21:
 * - Fujitsu-A64FX 슈퍼컴퓨터에서 문제가 생겨 보완한 코드.
 *   NFDx bit를 초기화한다.
 */
	tcr_clear_errata_bits tcr, x9, x5

/* IAMROOT, 2023.10.31:
 * - H/W support 여부에 따라 va bits가 달라져 vabits_actual, idmap_t0sz에
 *   저장한 값을 사용하여 tcr.t1sz, tcr.t0sz를 설정한다.
 */
#ifdef CONFIG_ARM64_VA_BITS_52
/* IAMROOT, 2021.08.28:
 * - VA_BITS == 52면 실행.
 *
 *   vabits_actual은 CONFIG_ARM64_VA_BITS_52가 enable 되었을때 h/w 52-bits
 *   support 여부에 따라서 아래와 같아진다.
 *     -     supported: 52 (vabits_actual)
 *     - NOT supported: 48 (vabits_actual)
 *
 * - 설령 VA_BITS == 52로 설정되어 있더라도 H/W support가 되지 않으면 여전히
 *   48 bits 가 되도록 한다.
 *
 * - vabits_actual == 52 라면 아래 공식에 따라 다음처럼 동작한다.
 *   @x9 = 0 - 52(@vabits_actual) + 64 = 12
 *   TCR_EL1.t1sz = 12
 *   TCR_EL1.t0sz = 12
 */
	ldr_l		x9, vabits_actual
	sub		x9, xzr, x9
	add		x9, x9, #64
	tcr_set_t1sz	tcr, x9
#else
/* IAMROOT, 2021.08.28:
 * - VA_BITS <= 48면 실행.
 *
 *   __idmap_text_end의 'pa bit > va bit'라면 매핑할 va space가 부족할 수 있어
 *   va bit 확장을 했는데 그것을 반영하기 위한 코드.
 *
 * - VA_BITS == 52에서 'pa bit > 52'인 경우는 없으므로 아래 코드가 필요없다.
 */
	ldr_l		x9, idmap_t0sz
#endif
	tcr_set_t0sz	tcr, x9

/* IAMROOT, 2021.08.28:
 * - aa64mmfr0_el1 reg의 PARange값이 kernel에 하드코딩된 MAX PARange 값보다
 *   큰지 확인하고 크면 size를 제한한다.
 */
	/*
	 * Set the IPS bits in TCR_EL1.
	 */
	tcr_compute_pa_size tcr, #TCR_IPS_SHIFT, x5, x6

/* IAMROOT, 2021.08.28:
 * - aa64mmfr1_el1 reg를 참조하여 HAFDBS[3:0] bit fields의 값이 0이 아니면
 *   tcr의 h/w access flag(ha)를 1로 설정한다.
 *
 * - aa64mmfr1_el1
 *   HAFDBS[3:0]: translation table의 access flag 및 dirty state 업데이트하는
 *                h/w 기능이 지원되는지 확인하는 bit fields.
 *         0b000: access flag / dirty state 업데이트 기능 미지원.
 *         0b001: access flag 업데이트 기능만 지원.
 *         0b010: access flag / dirty state 업데이트 기능 모두 지원.
 *
 * - tcr
 *   HA[39]: EL0/1의 stage 1 translation access flag 업데이트를 h/w로 동작
 *           하도록 기능 enable/disable 여부.
 *      0b0: stage 1 access flag 업데이트 disable.
 *      0b1: stage 1 access flag 업데이트 enable.
 */
#ifdef CONFIG_ARM64_HW_AFDBM
	/*
	 * Enable hardware update of the Access Flags bit.
	 * Hardware dirty bit management is enabled later,
	 * via capabilities.
	 */
	mrs	x9, ID_AA64MMFR1_EL1
	and	x9, x9, #0xf
	cbz	x9, 1f
	orr	tcr, tcr, #TCR_HA		// hardware Access flag update
1:
#endif	/* CONFIG_ARM64_HW_AFDBM */

/* IAMROOT, 2023.10.29:
 * - 지금까지 설정한 mair, tcr alias의 값을 mair_el1과 tcr_el1에 설정.
 */
	msr	mair_el1, mair
	msr	tcr_el1, tcr
	/*
	 * Prepare SCTLR
	 */
/* IAMROOT, 2023.11.01:
 * - arm64 linux kernel에서 사용하는 system control reg bit fields 설정.
 */
	mov_q	x0, INIT_SCTLR_EL1_MMU_ON
	ret					// return to head.S

	.unreq	mair
	.unreq	tcr
SYM_FUNC_END(__cpu_setup)
