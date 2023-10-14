/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#ifndef __ARM_KVM_INIT_H__
#define __ARM_KVM_INIT_H__

#ifndef __ASSEMBLY__
#error Assembly-only header
#endif

#include <asm/kvm_arm.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>
#include <linux/irqchip/arm-gic-v3.h>
/* IAMROOT, 2021.11.14:
 * - EL1에서 한 것처럼 EL2도 우선 mmu off 한다.
 */
.macro __init_el2_sctlr
	mov_q	x0, INIT_SCTLR_EL2_MMU_OFF
	msr	sctlr_el2, x0
	isb
.endm

/*
 * Allow Non-secure EL1 and EL0 to access physical timer and counter.
 * This is not necessary for VHE, since the host kernel runs in EL2,
 * and EL0 accesses are configured in the later stage of boot process.
 * Note that when HCR_EL2.E2H == 1, CNTHCTL_EL2 has the same bit layout
 * as CNTKCTL_EL1, and CNTKCTL_EL1 accessing instructions are redefined
 * to access CNTHCTL_EL2. This allows the kernel designed to run at EL1
 * to transparently mess with the EL0 bits via CNTKCTL_EL1 access in
 * EL2.
 */
/* IAMROOT, 2021.11.14: TODO
 * - el0, el1 물리 timer와 counter의 접근을 허용하고 voffset 값을 0으로 설정.
 *   cnthctl_el3[1:0]: 0x3 값 설정하여 아래 bits를 1로 세팅.
 *      - EL1PCEN[1] :
 *      - EL1PCTEN[0]: 
 *
 * - cnthctl_el2: counter-timer hypervisor control reg.
 *                el2의 timer, counter 설정에 대한 reg.
 * - cntvoff_el2: counter-timer virtual offset reg.
 *                VM에서 사용하는 가상 timer, counter의 값을 구하기 위한 offset
 *                정보를 담고 있으며 vcnt = phys_cnt - offset을 통해 구함.
 * - xzr: zero reg.
 *        zero value 리턴.
 */
.macro __init_el2_timers
	mov	x0, #3				// Enable EL1 physical timers
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr		// Clear virtual offset
.endm

/* IAMROOT, 2021.08.07:
 * PASS
 */
.macro __init_el2_debug
	mrs	x1, id_aa64dfr0_el1
	sbfx	x0, x1, #ID_AA64DFR0_PMUVER_SHIFT, #4
	cmp	x0, #1
	b.lt	.Lskip_pmu_\@			// Skip if no PMU present
	mrs	x0, pmcr_el0			// Disable debug access traps
	ubfx	x0, x0, #11, #5			// to EL2 and allow access to
.Lskip_pmu_\@:
	csel	x2, xzr, x0, lt			// all PMU counters from EL1

	/* Statistical profiling */
	ubfx	x0, x1, #ID_AA64DFR0_PMSVER_SHIFT, #4
	cbz	x0, .Lskip_spe_\@		// Skip if SPE not present

	mrs_s	x0, SYS_PMBIDR_EL1              // If SPE available at EL2,
	and	x0, x0, #(1 << SYS_PMBIDR_EL1_P_SHIFT)
	cbnz	x0, .Lskip_spe_el2_\@		// then permit sampling of physical
	mov	x0, #(1 << SYS_PMSCR_EL2_PCT_SHIFT | \
		      1 << SYS_PMSCR_EL2_PA_SHIFT)
	msr_s	SYS_PMSCR_EL2, x0		// addresses and physical counter
.Lskip_spe_el2_\@:
	mov	x0, #(MDCR_EL2_E2PB_MASK << MDCR_EL2_E2PB_SHIFT)
	orr	x2, x2, x0			// If we don't have VHE, then
						// use EL1&0 translation.

.Lskip_spe_\@:
	/* Trace buffer */
	ubfx	x0, x1, #ID_AA64DFR0_TRBE_SHIFT, #4
	cbz	x0, .Lskip_trace_\@		// Skip if TraceBuffer is not present

	mrs_s	x0, SYS_TRBIDR_EL1
	and	x0, x0, TRBIDR_PROG
	cbnz	x0, .Lskip_trace_\@		// If TRBE is available at EL2

	mov	x0, #(MDCR_EL2_E2TB_MASK << MDCR_EL2_E2TB_SHIFT)
	orr	x2, x2, x0			// allow the EL1&0 translation
						// to own it.

.Lskip_trace_\@:
	msr	mdcr_el2, x2			// Configure debug traps
.endm

/* LORegions */
/* IAMROOT, 2021.08.07:
 * - id_aa64mmfr1_el1: AArch64 Memory Model Feature Reg 1
 *                     AArch64에 구현된 memory model 및 memory management
 *                     지원 기능에 대한 정보를 담고 있는 reg.
 *   LO[19:16]: LORegions 기능 support 여부.
 *      0b0000: Not supported
 *      0b0001: supported
 *
 * - LOregion(limited-ordering region):
 *   LOregion으로 정해진 memory 주소 범위는 re-ordering, out-of-ordering 문제를
 *   HW 레벨에서 제한할 수 있게 된다.
 *
 *   ChatGPT: This feature plays a crucial role in ensuring that software
 *   behaves as expected in a concurrent or multi-core environment by limiting
 *   the potential for unexpected re-ordering of memory operations.
 */
.macro __init_el2_lor
	mrs	x1, id_aa64mmfr1_el1
	ubfx	x0, x1, #ID_AA64MMFR1_LOR_SHIFT, 4
	cbz	x0, .Lskip_lor_\@

/* IAMROOT, 2021.08.14
 * - LORegion feature를 지원하는 h/w라면 bit clear를 통해 disable 한다.
 *
 *   LORC : LORegion control reg.
 *          feature enable/disable 및 현재 LOR descriptor 선택.
 *   DS[3:2]: descriptor 선택
 *   EN[0]  : Feature enable/disable 여부
 */
	msr_s	SYS_LORC_EL1, xzr
.Lskip_lor_\@:
.endm

/* Stage-2 translation */
/* IAMROOT, 2021.08.07:
 * - vttbr_el2 reg 초기화 작업을 수행한다.
 *   vttbr_el2: Virtualization Translation Table Base reg.
 *              stage 2 trans-table 정보를 가지고 있다.
 *
 *   el2가 enable 되어 있다면 addr translation regime은 총 2개의 stages로
 *   이루어진다. 이는 hyp가 vm의 view of memory를 control 할 수 있도록 하고
 *   vm의 memory access를 isolation/sandboxing 하여 자신에게 할당된 memory만
 *   접근할 수 있도록 한다.
 *
 * - 1 stage
 *    el1-OS를 통해 PA에 접근하게 된다.
 *
 *    - for el1-OS trans stage
 *      VA -> (TTBRx_EL1) -> PA
 *              el1-OS
 *            trans-table
 *
 * - 2 stages
 *    guest-OS는 스스로가 PA 접근한다고 생각하지만 사실 IPA에 접근하고 PA는
 *    hyp가 접근하게 된다.
 *
 *    - for Guest-OS trans stages
 *      VA -> (TTBRx_EL1) -> IPA -> (VTTBR0_EL2) --> PA       * EL0/1
 *             guest-OS              trans-table     ^
 *            trans-table                            |
 *                                                   |
 *    - for hyp translation table                    |
 *      VA -------------> (TTBR0_EL2) ---------------+        * EL2
 *
 *
 * - TTBR0_EL2 : hyp의 VA를 PA로 변환하는데 사용하는 trans-table.
 * - VTTBR0_EL2: guest-OS/application (EL0)에서의 VA가 PA로 변환하는 과정에서
 *               stage1과 stage2를 거치는데 stage 2에서 사용하는 trans-table.
 * - TTBRx_EL1 : guest-OS에서 EL0/1의 VA를 IPA로 변환하는데 사용하는 trans-
 *               table.
 */
.macro __init_el2_stage2
	msr	vttbr_el2, xzr
.endm


/* GICv3 system register access */
/* IAMROOT, 2021.08.07:
 * - id_aa64pfr0_el1: AArch64 Processor Feature Reg 0
 *                    AArch64에 구현된 PE 기능에 대한 정보를 담고 있는 reg.
 *   GIC[27:24]: GIC CPU interface 제공 정보.
 *       0b0000: GIC CPU interface Not supported, Memory-mapped 방식 이용.
 *       0b0001: GICv3/4 CPU interface supported.
 *       0b0011: GICv4.1 CPU interface supported.
 */
.macro __init_el2_gicv3
	mrs	x0, id_aa64pfr0_el1
	ubfx	x0, x0, #ID_AA64PFR0_GIC_SHIFT, #4
	cbz	x0, .Lskip_gicv3_\@

/* IAMROOT, 2021.08.07:
 * - ICC_SRE_EL2: Interrupt Controller System Register Enable reg.
 *                EL2에서 사용하는 GIC sys reg를 제어하는데 사용.
 *
 * - SRE[0]: System Register Enable
 *      0b0: GIC대신 Memory-mapped 이용
 *      0b1: GIC 시스템 레지스터 이용
 *
 * - ENABLE[3]: EL0에서 ICC_SRE_EL1 reg를 사용하려고 할 때 trap 발생 여부 기록.
 *         0b0: EL1 accesses to ICC_SRE_EL1 trap to EL2
 *              EL1에서 ICC_SRE_EL1에 접근하려고 하면 EL2로 trap 한다는 의미는
 *              시스템 에러로 판단하고 동작이 안되도록 만듬.
 *         0b1: EL1 accesses to ICC_SRE_EL1 do not trap to EL2
 *              EL1에서 ICC_SRE_EL1에 접급하려고 하면 EL2로 trap 하지 않는다는
 *              것은 접근할 수 있음을 의미.
 *
 *   elseif EL2Enabled() && ICC_SRE_EL2.Enable == '0' then
 *      AArch64.SystemAccessTrap(EL2, 0x18);
 *   ...
 *   return ICC_SRE_EL1;
 *
 * - RAO/WI type bit:
 *   해당 bit 값을 항상 1로 읽음 (쓰기는 항상 무시)
 *   (Read as One, Write Ignored):
 */
	mrs_s	x0, SYS_ICC_SRE_EL2
	orr	x0, x0, #ICC_SRE_EL2_SRE	// Set ICC_SRE_EL2.SRE==1
	orr	x0, x0, #ICC_SRE_EL2_ENABLE	// Set ICC_SRE_EL2.Enable==1
	msr_s	SYS_ICC_SRE_EL2, x0
	isb					// Make sure SRE is now set

/* IAMROOT, 2021.08.07:
 * - ICC_SRE_EL2의 SRE bit를 다시 읽어 해당 값에 따라 처리 로직이 아래와 같음.
 *   @x0 == 0: 초기화되지 않았으므로 skip_gicv3로 분기.
 *       == 1: ICH_HCR_EL2 reg 초기화.
 *
 * - tbz x0, #0, 1f은 잘못된 label로 점프하고 있어 bug이며 mainline에는 patch됨.
 *   https://github.com/torvalds/linux/blob/633b47cb009d09dc8f4ba9cdb3a0ca138809c7c7/arch/arm64/include/asm/el2_setup.h#L128
 *
 * - TBZ: test #<imm> bit and branch if zero
 * - ICH_HCR_EL2: Interrupt Controller Hyp Control reg.
 *                VM env를 control 하는데 사용.
 *   ICH_*: per-cpu 레지스터
 */
	mrs_s	x0, SYS_ICC_SRE_EL2		// Read SRE back,
	tbz	x0, #0, 1f			// and check that it sticks
	msr_s	SYS_ICH_HCR_EL2, xzr		// Reset ICC_HCR_EL2 to defaults
.Lskip_gicv3_\@:
.endm

/* IAMROOT, 2021.08.07:
 * - 32-bit S/W의 경우 CP15를 접근하는 부분이 있으므로 Aarch32 EL0, EL1이
 *   System Register에 접근할 시 EL2로 trap하는 것을 disable.
 *
 * - hstr_el2: Hypervisor System Trap Register
 */
.macro __init_el2_hstr
	msr	hstr_el2, xzr			// Disable CP15 traps to EL2
.endm

/* Virtual CPU ID registers */
.macro __init_el2_nvhe_idregs
/* IAMROOT, 2021.08.07:
 * - midr_el1, mpidr_el1을 vpidr_el2, vmpidr_el2에서 복사한다.
 *
 *   trap을 이용해서 operation을 virtualize하는 것은 많은 비용이 든다.
 *   feature register들, 예를 들어 ID_AA64MMFR0_EL1 처럼 OS에 의해 자주
 *   접근되지 않는 레지스터는 크게 상관 없지만 midr_el1, mpidr_el1같이
 *   자주 접근되는 레지스터들에 접근하는 operation들에게는 부담이 된다.
 *
 *   이를 개선하기 위해서 guest-OS가 해당 operation을 수행하기 위해서
 *   hypervisor에 일일이 trap을 날리기 보다는 virtual value를 보도록
 *   하는 방법을 사용하여 성능 최적화를 한다.
 *
 *    vpidr_el2: el1이 midr_el1을 읽으면 trap없이 하드웨어가 자동으로
 *               vpidr_el2 값을 반환한다.
 *
 *   vmpidr_el2: el1이 mpidr_el1을 읽으면 trap없이 하드웨어가 자동으로
 *               vmpidr_el2 값을 반환한다.
 *
 * - midr: main id reg.
 *         PE에 대해 아래와 같은 ID 정보를 제공한다.
 *     - SoC Vendor
 *     - Architecture
 *     - Variant: 하위 SoC에 의해 몇가지 변경된 제품에서 사용
 *     - PartNum: 하위 SoC에 의해 몇가지 변경된 제품에서 사용
 *     - Rev.
 *
 *      Implementation Defined: ARM이 아닌 SoC들이 설정할 수 있는 비트 필드.
 *     !Implementation Defined: Global로서 ARM이 설정해놓는 비트 필드.
 *
 * - mpidr: multiprocessor affinity reg.
 *          scheduling 목적으로 사용되는 core id 정보 제공.
 *          per-cpu로 존재하는 시스템 레지스터이며 cpu 마다 다른값(Core Number)을 가짐.
 *          이 값들은 S/W Scheduling에 사용되며 CPU/Cluster를 구분하는데 사용.
 *
 *     - MT(bit): Hyper-threading 관련
 *         * MT 0:
 *           - aff0: Core Number
 *                   (rasp4: per-cpu aff0에 0, 1, 2, 3이 들어있음)
 *           - aff1: Cluster에 대한 정보
 *           - aff2: Cluster 단계에서 더 높은 레벨
 *         * MT 1:
 *           - aff0: H/W Thread Number
 *           - aff1: Core Number
 *           - aff2: Cluster에 대한 정보
 *           - aff3: Cluster 단계에서 더 높은 레벨
 */
	mrs	x0, midr_el1
	mrs	x1, mpidr_el1
	msr	vpidr_el2, x0
	msr	vmpidr_el2, x1
.endm

/* Coprocessor traps */
/* IAMROOT, 2021.08.07: 
 * - cptr_el2: Architectural Feature Trap reg.
 *             CPACR, CPACR_EL1, trace, activity monitor, SVE, advanced SIMD,
 *             floating-point 수행시 EL2로 trap 여부 제어.
 *
 *	 0x33ff == 0b0011 0011 1111 1111 bits에 대응하는 기능들을 enable
 *
 * 	 HCR_EL2.E2H == 1이라면 0x33ff bits는 모두 RES0으로 영향이 없지만 현재 상태는
 *   nVHE(E2H == 0)이므로 reserved가 아닌 다른 기능을 하는 bits이고 해당 기능들은
 *   아래와 같다.
 *
 *   TFP [10] - 0
 *     Advanced SIMD, Floating-Point 레지스터에 접근시
 *     0b0: trap 발생 X
 * 	   0b1: trap 발생 O
 *
 *   TZ [8] - 1 (SVE 기능이 구현되지 않으면 RES1임)
 *     0b0: trap 발생 X
 * 	   0b1: trap 발생 O
 * 
 * 	   SVE: Scalable Vector Extension
 * 	   Z: vector registers(Z0, Z1, ...) -> ZCR: control register.
 */
.macro __init_el2_nvhe_cptr
	mov	x0, #0x33ff
	msr	cptr_el2, x0			// Disable copro. traps to EL2
.endm

/* SVE register access */
.macro __init_el2_nvhe_sve
/* IAMROOT, 2021.08.07: 
 * - SVE 기능이 arch에서 지원되는지 확인 후 지원하지 않으면 skip_sve로 분기. 
 *   SVE: Scalable Vector Extension
 *        Advanced SIMD instr 와 관련된 기능인듯.
 */
	mrs	x1, id_aa64pfr0_el1
	ubfx	x1, x1, #ID_AA64PFR0_SVE_SHIFT, #4
	cbz	x1, .Lskip_sve_\@

/* IAMROOT, 2021.08.07: 
 * - cptr_el2의 TZ bit를 clear 하여 SVE trap to EL2 disable.
 *   SVE 기능이 arch에서 지원되면 trap하지 않고 관련 레지스터를 사용하겠다는 의미.
 */
	bic	x0, x0, #CPTR_EL2_TZ		// Also disable SVE traps
	msr	cptr_el2, x0			// Disable copro. traps to EL2
	isb

/* IAMROOT, 2023.00.00:
 * - ZCR_ELx_LEN_MASK == 0x1ff (1f: RAZ/WI, f: LEN)
 *
 * - SYS_ZCR_EL2: SVE Control reg.
 *   LEN [3:0]: Effective SVE Vector Length (VL)
 *              EL2, 1, 0의 SVE length를 (LEN+1)x128 bits로 제한한다.
 */
	mov	x1, #ZCR_ELx_LEN_MASK		// SVE: Enable full vector
	msr_s	SYS_ZCR_EL2, x1			// length for EL1.
.Lskip_sve_\@:
.endm

/* Disable any fine grained traps */
/* IAMROOT, 2021.11.14:
 * - FGT: Fine-Grained Traps.
 * 
 * - arm64에서 FEAT_FGT 기능이 도입된 linux v5.15에서는 사용하지 않아 disable.
 */
.macro __init_el2_fgt
	mrs	x1, id_aa64mmfr0_el1
	ubfx	x1, x1, #ID_AA64MMFR0_FGT_SHIFT, #4
	cbz	x1, .Lskip_fgt_\@

	mov	x0, xzr
	mrs	x1, id_aa64dfr0_el1
	ubfx	x1, x1, #ID_AA64DFR0_PMSVER_SHIFT, #4
	cmp	x1, #3
	b.lt	.Lset_fgt_\@
	/* Disable PMSNEVFR_EL1 read and write traps */
	orr	x0, x0, #(1 << 62)

.Lset_fgt_\@:
	msr_s	SYS_HDFGRTR_EL2, x0
	msr_s	SYS_HDFGWTR_EL2, x0
	msr_s	SYS_HFGRTR_EL2, xzr
	msr_s	SYS_HFGWTR_EL2, xzr
	msr_s	SYS_HFGITR_EL2, xzr

	mrs	x1, id_aa64pfr0_el1		// AMU traps UNDEF without AMU
	ubfx	x1, x1, #ID_AA64PFR0_AMU_SHIFT, #4
	cbz	x1, .Lskip_fgt_\@

	msr_s	SYS_HAFGRTR_EL2, xzr
.Lskip_fgt_\@:
.endm

/* IAMROOT, 2021.08.14:
 * - eret 수행시 return할 ELx, SPx(stack) 설정 및 daif 플래그를 설정하여
 *   interrupt가 발생하지 않도록 한다. nVHE인 경우 EL1 -> EL2로 level이
 *   상승하므로 EL1으로 설정하는 것이다.
 *
 * - spsr_elx는 exception이 발생한 level의 PSTATE 정보를 담고 있다.
 */
.macro __init_el2_nvhe_prepare_eret
	mov	x0, #INIT_PSTATE_EL1
	msr	spsr_el2, x0
.endm

/**
 * Initialize EL2 registers to sane values. This should be called early on all
 * cores that were booted in EL2. Note that everything gets initialised as
 * if VHE was not evailable. The kernel context will be upgraded to VHE
 * if possible later on in the boot process
 *
 * Regs: x0, x1 and x2 are clobbered.
 */
/* IAMROOT, 2021.11.14:
 * - el2 동작을 위한 초기화 작업을 수행한다.
 *   __init_el2_sctlr : el2 mmu off.
 *   __init_el2_timers: el0/1 physical timers enable.
 *   __init_el2_debug :
 *   __init_el2_lor   : LORegion 기능 enable/disable 수행.
 *   __init_el2_stage2: vttbr_el2 reg 초기화.
 *   __init_el2_gicv3 : GIC enable
 *   __init_el2_hstr  : CP15 traps to EL2
 *   __init_el2_nvhe_idregs: vpidr_el2, vmpidr_el2 설정
 *   __init_el2_nvhe_cptr  : co-processor trap 설정
 *   __init_el2_nvhe_sve   : SVE length 설정 if it supports
 *   __init_el2_fgt        : FGT 기능 disable
 *   __init_el2_nvhe_prepare_eret: spsr_el2 초기화
 */
.macro init_el2_state
	__init_el2_sctlr
	__init_el2_timers
	__init_el2_debug
	__init_el2_lor
	__init_el2_stage2
	__init_el2_gicv3
	__init_el2_hstr
	__init_el2_nvhe_idregs
	__init_el2_nvhe_cptr
	__init_el2_nvhe_sve
	__init_el2_fgt
	__init_el2_nvhe_prepare_eret
.endm

#endif /* __ARM_KVM_INIT_H__ */
