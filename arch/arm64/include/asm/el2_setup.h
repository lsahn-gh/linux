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
/*
 * IAMROOT, 2021.11.14:
 * - mmu off
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
/*
 * IAMROOT, 2021.11.14:
 * - el0, el1 물리 timer와 counter의 접근을 허용한다.
 */
.macro __init_el2_timers
	mov	x0, #3				// Enable EL1 physical timers
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr		// Clear virtual offset
.endm

/*
 * IAMROOT, 2021.08.07:
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
 * LORegion: Limited Ordering Region 어드레스 범위를 오더링 제한
 * LO [19:16] 
 * 0x1 LoReigion Supported
 * 
 * LORC : LOR control
 * DS [3:2]
 * LORSA_EL1, LOREA_EL1 시작과 끝의 범위를 지정
 * EN [0]
 * Enable 1 , disable 0
 */
.macro __init_el2_lor
	mrs	x1, id_aa64mmfr1_el1
	ubfx	x0, x1, #ID_AA64MMFR1_LOR_SHIFT, 4
	cbz	x0, .Lskip_lor_\@
    /*
     * IAMROOT, 2021.08.14
     * 만약 LORegion Feature가 implement되있으면
     * LORegion을 disable시킨다.
     */
	msr_s	SYS_LORC_EL1, xzr
.Lskip_lor_\@:
.endm

/* Stage-2 translation */
/* IAMROOT, 2021.08.07: 
 * TTBR0_EL2 
 *   하이퍼바이저가 사용
 * VTTBR0_EL2 
 *   Guest OS(또는 application EL0) 에서 진짜 주소로 변환하는 과정에서 
 *   stage1과 stage2를 거쳐 physical memory를 받음, 이 때 stage2에서 사용
 *   TTBR0_EL1은 stage1에서 사용
 *
 * TTBR & VTTBR 현재 돌아가는 운영체제와 에플리케이션을 look up할 때 사용
 * TTBR의 ASID(운영체제), VTTBR의 VMID(애플리케이션)
 */
.macro __init_el2_stage2
	msr	vttbr_el2, xzr
.endm


/* GICv3 system register access */
.macro __init_el2_gicv3
/* IAMROOT, 2021.08.07:
 * PE에 구현된 기능에 대해 정보 제공
 * ID_AA64_*: 지원 기능들을 4bit 단위로 제공함을 의미
 */
	mrs	x0, id_aa64pfr0_el1
/* IAMROOT, 2021.08.07:
 * GIC: GIC 시스템 레지스터 인터페이스 제공 여부
 * - 0b0000: GIC 인터페이스 제공(X), Memory-mapped 방식 이용
 * - 0b0001: v3/4 버전 인터페이스 사용
 * - 0b0011: v4.1 버전 인터페이스 사용
 */
	ubfx	x0, x0, #ID_AA64PFR0_GIC_SHIFT, #4
	cbz	x0, .Lskip_gicv3_\@

/* IAMROOT, 2021.08.07:
 * EL2에서 사용하는 GIC 시스템 레지스터를 enable 하기위한 레지스터
 *
 * - SRE: System Register Enable
 *   0x0: Memory-mapped 이용
 *   0x1: GIC 시스템 레지스터 이용
 *
 * - ENABLE: EL0에서 ICC_SRE_EL1 reg를 사용하려고 할 때 trap(에러) 발생 여부 의미.
 *   0x0: Non-secure EL1 accesses to ICC_SRE_EL1 trap to EL2
 *        EL2로 trap 한다는 의미는 시스템 에러로 판단하고 동작이 안되도록 만듬.
 *   0x1: Non-secure EL1 accesses to ICC_SRE_EL1 do not trap to EL2
 *        trap 하지 않는다는 의미는 EL2가 ICC_SRE_EL1에 접근할 수 있음을 의미.
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
 * ICC_SRE_EL2의 SRE(0bit)를 다시 읽고 체크하여 0이면 초기화가 되지
 * 않았으므로 레이블 3으로 점프
 *
 * TBZ: test #<imm> bit and branch if zero
 */
	mrs_s	x0, SYS_ICC_SRE_EL2		// Read SRE back,
/* IAMROOT, 2021.08.07:
 * Hypervisor에서 사용하는 GIC용 Hyper control 레지스터 값 초기화
 * ICH_*: per-cpu 레지스터
 */
	tbz	x0, #0, 1f			// and check that it sticks
	msr_s	SYS_ICH_HCR_EL2, xzr		// Reset ICC_HCR_EL2 to defaults
.Lskip_gicv3_\@:
.endm

/* IAMROOT, 2021.08.07:
 * 32-bit S/W의 경우 CP15를 접근하는 부분이 있으므로 AARCH32 EL0, EL1이
 * System Register에 접근할 시 EL2로 trap하는 것을 disable.
 * - 5.10 -> 5.15 변경사항
 *   CONFIG_COMPAT일때만 해당코드가 동작했었는데 CONFIG상관없이 그냥 함.
 */
.macro __init_el2_hstr
	msr	hstr_el2, xzr			// Disable CP15 traps to EL2
.endm

/* Virtual CPU ID registers */
/*
 * IAMROOT, 2021.11.14:
 * - TODO
 */
.macro __init_el2_nvhe_idregs
/* IAMROOT, 2021.08.07:
 * midr: PE에 대한 ID 정보 제공.
 * - SoC Vendor
 * - Architecture
 * - Variant: 하위 SoC에 의해 몇가지 변경된 제품에서 사용 
 * - PartNum: 하위 SoC에 의해 몇가지 변경된 제품에서 사용
 * - Rev.
 *
 *  Implementation Defined: ARM이 아닌 SoC들이 설정할 수 있는 비트 필드.
 * !Implementation Defined: Global로서 ARM이 설정해놓는 비트 필드.
 *
 * mpidr: Multiprocessor Affinity Register
 * Scheduling 목적으로 사용되는 Core ID 정보 제공.
 * - per-cpu로 존재하는 시스템 레지스터이며 cpu 마다 다른값(Core Number)을 가짐.
 *   이 값들은 S/W Scheduling에 사용되며 CPU/Cluster를 구분하는데 사용.
 *
 * MT(bit): Hyper-threading 관련
 *   * MT 0:
 *     - aff0: Core Number
 *             (rasp4: per-cpu aff0에 0, 1, 2, 3이 들어있음)
 *     - aff1: Cluster에 대한 정보
 *     - aff2: Cluster 단계에서 더 높은 레벨
 *   * MT 1:
 *     - aff0: H/W Thread Number
 *     - aff1: Core Number
 *     - aff2: Cluster에 대한 정보
 *     - aff3: Cluster 단계에서 더 높은 레벨
 */
	mrs	x0, midr_el1
	mrs	x1, mpidr_el1
/*
 * IAMROOT, 2021.08.07:
 * 위 정보들을 Hypervisor 시스템 레지스터에 저장
 *
 * Trap을 이용해서 operation을 virtualize하는 것은 많은 비용이 든다.
 * Feature register들, 예를 들어 ID_AA64MMFR0_EL1 처럼 OS에 의해 자주
 * 접근되지 않는 레지스터는 크게 상관 없지만 MIDR_EL1, MPIDR_EL1같이
 * 자주 접근되는 레지스터들에 접근하는 operation들에게는 부담이 된다.
 *
 * 이를 개선하기 위해서 Guest OS가 해당 operation을 수행하기 위해서
 * Hypervisor에 일일이 Trap을 날리기 보다는 virtual value를 보도록
 * 하는 방법을 사용하기도 한다.
 *
 *  VPIDR_EL2: EL1이 MIPDR_EL1을 읽으면 Trap없이 하드웨어가 자동으로
 *             VPIDR_EL2 값을 돌려준다.
 *
 * VMPIDR_EL2: EL1이 MPIPDR_EL1을 읽으면 Trap없이 하드웨어가 자동으로
 *             VMPIDR_EL2 값을 돌려준다.
 */
	msr	vpidr_el2, x0
	msr	vmpidr_el2, x1
.endm

/* Coprocessor traps */
/* IAMROOT, 2021.08.07: 
 * cptr_el2에 0x33ff
 *
 * TFP [10] - 0
 *   Advanced SIMD, Floating-Point 레지스터에 접근시
 *   0b0: trap이 일어나지 않도록, 0b1: trap이 일어나도록
 *
 * TZ [8] - 1 (SVE 명령을 사용할 때)
 *   0b0: trap이 일어나지 않도록, 0b1: trap이 일어나도록 
 * 나머지 비트는 RES1을 1로 
 *
 * SVE: Scalable Vector Extension
 * Z: vector registers(Z0, Z1, ...) -> ZCR: control register.
 */
.macro __init_el2_nvhe_cptr
	mov	x0, #0x33ff
	msr	cptr_el2, x0			// Disable copro. traps to EL2
.endm

/* SVE register access */
.macro __init_el2_nvhe_sve
/* IAMROOT, 2021.08.07: 
 * SVE가 내장되어 있는지 확인 후 (어차피 없으면 SVE를 사용하는 어플리케이션이 종료됨)
 * 없다면 branch 
 */
	mrs	x1, id_aa64pfr0_el1
	ubfx	x1, x1, #ID_AA64PFR0_SVE_SHIFT, #4
	cbz	x1, .Lskip_sve_\@

/* IAMROOT, 2021.08.07: 
 * cptr_el2의 TZ 클리어
 * 다시 말하면, SVE가 내장되어 있으면 Trap하지 않고 관련 레지스터를 사용하겠다는 의미.
 *
 * ZCR_ELx_LEN_MASK == 0x1ff
 * SYS_ZCR_EL2
 *   SVE를 위해 가장 큰 벡터길이로 활성화시킨다
 * LEN [3:0]
 *   이때 길이는 (LEN+1)x128 bits 
 */
	bic	x0, x0, #CPTR_EL2_TZ		// Also disable SVE traps
	msr	cptr_el2, x0			// Disable copro. traps to EL2
	isb
	mov	x1, #ZCR_ELx_LEN_MASK		// SVE: Enable full vector
	msr_s	SYS_ZCR_EL2, x1			// length for EL1.
.Lskip_sve_\@:
.endm

/* Disable any fine grained traps */
/*
 * IAMROOT, 2021.11.14:
 * - TODO
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

/*
 * IAMROOT, 2021.08.14: EL1으로 돌아갈때 DAIF 플래그들을 모두 설정하여
 *   인터럽트나 정렬 exception이 발생하지 않도록 막는다.
 *    - EL1으로 모드 변경 시 사용할 스택은 EL1용 스택이다.
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
/*
 * IAMROOT, 2021.11.14:
 * - el2 동작을 위한 초기화 작업을 진행한다.
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
