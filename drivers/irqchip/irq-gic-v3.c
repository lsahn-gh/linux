// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013-2017 ARM Limited, All Rights Reserved.
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#define pr_fmt(fmt)	"GICv3: " fmt

#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/refcount.h>
#include <linux/slab.h>

#include <linux/irqchip.h>
#include <linux/irqchip/arm-gic-common.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/irqchip/irq-partition-percpu.h>

#include <asm/cputype.h>
#include <asm/exception.h>
#include <asm/smp_plat.h>
#include <asm/virt.h>

#include "irq-gic-common.h"

/*
 * IAMROOT, 2022.10.15:
 * - GICD_INT_DEF_PRI == 0xA0
 *   GICD_INT_NMI_PRI = (0xA0 & ~0x80) = 0x20
 */
#define GICD_INT_NMI_PRI	(GICD_INT_DEF_PRI & ~0x80)

#define FLAGS_WORKAROUND_GICR_WAKER_MSM8996	(1ULL << 0)
#define FLAGS_WORKAROUND_CAVIUM_ERRATUM_38539	(1ULL << 1)

#define GIC_IRQ_TYPE_PARTITION	(GIC_IRQ_TYPE_LPI + 1)

struct redist_region {
	void __iomem		*redist_base;
	phys_addr_t		phys_base;
	bool			single_redist;
};

struct gic_chip_data {
	struct fwnode_handle	*fwnode;
	void __iomem		*dist_base;
	struct redist_region	*redist_regions;
	struct rdists		rdists;
	struct irq_domain	*domain;
	u64			redist_stride;
	u32			nr_redist_regions;
	u64			flags;
	bool			has_rss;

/*
 * IAMROOT, 2022.10.08:
 * - __gic_update_rdist_properties() 참고
 */
	unsigned int		ppi_nr;
	struct partition_desc	**ppi_descs;
};

static struct gic_chip_data gic_data __read_mostly;

/*
 * IAMROOT, 2022.10.01:
 * - gic_init_bases에서 hyp_mode가 안켜져있으면 disable시킨다.
 *   EL2로 진입하여 hyp_mode가 사용되면 eoi를 drop 시켜 사용한다.
 *   그리고, Guest OS가 EL1으로 진입하여 hyp_mode가 사용되지 않으면 eoi를 
 *   deactivate 하여 사용하도록, eoi를 2개로 분리하여 처리한다.
 */
static DEFINE_STATIC_KEY_TRUE(supports_deactivate_key);

#define GIC_ID_NR	(1U << GICD_TYPER_ID_BITS(gic_data.rdists.gicd_typer))
#define GIC_LINE_NR	min(GICD_TYPER_SPIS(gic_data.rdists.gicd_typer), 1020U)

/*
 * IAMROOT, 2022.10.01:
 * - SPIs 확장. Extended SPIs
 *   1020개 이상 확장 시킬수있는것인지.
 */
#define GIC_ESPI_NR	GICD_TYPER_ESPIS(gic_data.rdists.gicd_typer)

/*
 * The behaviours of RPR and PMR registers differ depending on the value of
 * SCR_EL3.FIQ, and the behaviour of non-secure priority registers of the
 * distributor and redistributors depends on whether security is enabled in the
 * GIC.
 *
 * When security is enabled, non-secure priority values from the (re)distributor
 * are presented to the GIC CPUIF as follow:
 *     (GIC_(R)DIST_PRI[irq] >> 1) | 0x80;
 *
 * If SCR_EL3.FIQ == 1, the values written to/read from PMR and RPR at non-secure
 * EL1 are subject to a similar operation thus matching the priorities presented
 * from the (re)distributor when security is enabled. When SCR_EL3.FIQ == 0,
 * these values are unchanged by the GIC.
 *
 * see GICv3/GICv4 Architecture Specification (IHI0069D):
 * - section 4.8.1 Non-secure accesses to register fields for Secure interrupt
 *   priorities.
 * - Figure 4-7 Secure read of the priority field for a Non-secure Group 1
 *   interrupt.
 */
static DEFINE_STATIC_KEY_FALSE(supports_pseudo_nmis);

/*
 * Global static key controlling whether an update to PMR allowing more
 * interrupts requires to be propagated to the redistributor (DSB SY).
 * And this needs to be exported for modules to be able to enable
 * interrupts...
 */
DEFINE_STATIC_KEY_FALSE(gic_pmr_sync);
EXPORT_SYMBOL(gic_pmr_sync);

DEFINE_STATIC_KEY_FALSE(gic_nonsecure_priorities);
EXPORT_SYMBOL(gic_nonsecure_priorities);

/*
 * When the Non-secure world has access to group 0 interrupts (as a
 * consequence of SCR_EL3.FIQ == 0), reading the ICC_RPR_EL1 register will
 * return the Distributor's view of the interrupt priority.
 *
 * When GIC security is enabled (GICD_CTLR.DS == 0), the interrupt priority
 * written by software is moved to the Non-secure range by the Distributor.
 *
 * If both are true (which is when gic_nonsecure_priorities gets enabled),
 * we need to shift down the priority programmed by software to match it
 * against the value returned by ICC_RPR_EL1.
 */
/*
 * IAMROOT, 2022.10.15:
 * - papago
 *   non-secure world가 group 0 인터럽트에 액세스할 때(SCR_EL3.FIQ == 0의 결과로),
 *   ICC_RPR_EL1 레지스터를 읽으면 인터럽트 우선순위에 대한 distributor의 view가
 *   반환됩니다.
 *
 *   GIC 보안이 활성화되면(GICD_CTLR.DS == 0) 소프트웨어에 의해 작성된 인터럽트
 *   우선 순위가 distributor에 의해 non-secure 범위로 이동됩니다.
 *
 *   둘 다 true인 경우(gic_nonsecure_priorities가 활성화된 경우) ICC_RPR_EL1에서
 *   반환된 값과 일치하도록 소프트웨어에 의해 프로그래밍된 우선 순위를 낮추어야
 *   합니다.
 */
#define GICD_INT_RPR_PRI(priority)					\
	({								\
		u32 __priority = (priority);				\
		if (static_branch_unlikely(&gic_nonsecure_priorities))	\
			__priority = 0x80 | (__priority >> 1);		\
									\
		__priority;						\
	})

/* ppi_nmi_refs[n] == number of cpus having ppi[n + 16] set as NMI */
/*
 * IAMROOT, 2022.10.29:
 * - gic_enable_nmi_support()에서 gic_data.ppi_nr개수만큼 초기화된다. 
 */
static refcount_t *ppi_nmi_refs;

static struct gic_kvm_info gic_v3_kvm_info __initdata;
static DEFINE_PER_CPU(bool, has_rss);

/*
 * IAMROOT, 2022.10.28:
 * - affinity가 RS(Range Selector)가 필요한지 확인한다.
 * - The ICC_CTLR_EL1.RSS
 *   0 Targeted SGIs with affinity level 0 values of 0 - 15 are supported.
 *   1 Targeted SGIs with affinity level 0 values of 0 - 255 are supported.*
 *   즉 0xf0를 추출한다. 해당값이 있으면 RSS를 지원해줘야한다.
 */
#define MPIDR_RS(mpidr)			(((mpidr) & 0xF0UL) >> 4)
#define gic_data_rdist()		(this_cpu_ptr(gic_data.rdists.rdist))
/*
 * IAMROOT, 2022.10.08:
 * - gic_populate_rdist()에서 설정된다.
 */
#define gic_data_rdist_rd_base()	(gic_data_rdist()->rd_base)
/*
 * IAMROOT, 2022.10.01:
 * - sgi_base = rd_base + 64k
 */
#define gic_data_rdist_sgi_base()	(gic_data_rdist_rd_base() + SZ_64K)

/* Our default, arbitrary priority value. Linux only uses one anyway. */
/*
 * IAMROOT, 2022.10.08:
 * - 모든 interrupt 허용.
 */
#define DEFAULT_PMR_VALUE	0xf0

enum gic_intid_range {
	SGI_RANGE,
	PPI_RANGE,
	SPI_RANGE,
	EPPI_RANGE,
	ESPI_RANGE,
	LPI_RANGE,
	__INVALID_RANGE__
};

/*
 * IAMROOT, 2022.10.01:
 * - @hwirq로 어떤 gic인지 알아온다.
 */
static enum gic_intid_range __get_intid_range(irq_hw_number_t hwirq)
{
	switch (hwirq) {
	case 0 ... 15:
		return SGI_RANGE;
	case 16 ... 31:
		return PPI_RANGE;
	case 32 ... 1019:
		return SPI_RANGE;
	case EPPI_BASE_INTID ... (EPPI_BASE_INTID + 63):
		return EPPI_RANGE;
	case ESPI_BASE_INTID ... (ESPI_BASE_INTID + 1023):
		return ESPI_RANGE;
	case 8192 ... GENMASK(23, 0):
		return LPI_RANGE;
	default:
		return __INVALID_RANGE__;
	}
}

static enum gic_intid_range get_intid_range(struct irq_data *d)
{
	return __get_intid_range(d->hwirq);
}

static inline unsigned int gic_irq(struct irq_data *d)
{
	return d->hwirq;
}

/*
 * IAMROOT, 2022.10.01:
 * - rdist(redirect distribute)에 있는 interrupt인지 확인
 */
static inline bool gic_irq_in_rdist(struct irq_data *d)
{
	switch (get_intid_range(d)) {
	case SGI_RANGE:
	case PPI_RANGE:
	case EPPI_RANGE:
		return true;
	default:
		return false;
	}
}

static inline void __iomem *gic_dist_base(struct irq_data *d)
{
	switch (get_intid_range(d)) {
	case SGI_RANGE:
	case PPI_RANGE:
	case EPPI_RANGE:
		/* SGI+PPI -> SGI_base for this CPU */
		return gic_data_rdist_sgi_base();

	case SPI_RANGE:
	case ESPI_RANGE:
		/* SPI -> dist_base */
		return gic_data.dist_base;

	default:
		return NULL;
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - rwp(register write pendig)가 풀릴때까지 기다린다. max 1초
 */
static void gic_do_wait_for_rwp(void __iomem *base)
{
	u32 count = 1000000;	/* 1s! */

	while (readl_relaxed(base + GICD_CTLR) & GICD_CTLR_RWP) {
		count--;
		if (!count) {
			pr_err_ratelimited("RWP timeout, gone fishing\n");
			return;
		}
		cpu_relax();
		udelay(1);
	}
}

/* Wait for completion of a distributor change */
/*
 * IAMROOT, 2022.10.01:
 * - rwp wait.
 */
static void gic_dist_wait_for_rwp(void)
{
	gic_do_wait_for_rwp(gic_data.dist_base);
}

/* Wait for completion of a redistributor change */
/*
 * IAMROOT, 2022.10.01:
 * - rd base에서 rwp wait.
 */
static void gic_redist_wait_for_rwp(void)
{
	gic_do_wait_for_rwp(gic_data_rdist_rd_base());
}

#ifdef CONFIG_ARM64

static u64 __maybe_unused gic_read_iar(void)
{
	if (cpus_have_const_cap(ARM64_WORKAROUND_CAVIUM_23154))
		return gic_read_iar_cavium_thunderx();
	else
		return gic_read_iar_common();
}
#endif

/*
 * IAMROOT, 2022.10.08:
 * - @enable true이면 rdist에 연결된 cpu wakeup.
 *           false이면 rdist에 연결된 cpu sleep.
 */
static void gic_enable_redist(bool enable)
{
	void __iomem *rbase;
	u32 count = 1000000;	/* 1s! */
	u32 val;

	if (gic_data.flags & FLAGS_WORKAROUND_GICR_WAKER_MSM8996)
		return;

	rbase = gic_data_rdist_rd_base();

	val = readl_relaxed(rbase + GICR_WAKER);

/*
 * IAMROOT, 2022.10.08:
 * - 저전력 상태를 막는다.
 */
	if (enable)
		/* Wake up this CPU redistributor */
		val &= ~GICR_WAKER_ProcessorSleep;
	else
		val |= GICR_WAKER_ProcessorSleep;
	writel_relaxed(val, rbase + GICR_WAKER);

/*
 * IAMROOT, 2022.10.08:
 * - GICR_WAKER_ProcessorSleep를 set했는데도 불구하고 set이 안됫으면
 *   No PM support라고 생각해서 그냥 빠져 나간다.
 */
	if (!enable) {		/* Check that GICR_WAKER is writeable */
		val = readl_relaxed(rbase + GICR_WAKER);
		if (!(val & GICR_WAKER_ProcessorSleep))
			return;	/* No PM support in this redistributor */
	}

	while (--count) {
		val = readl_relaxed(rbase + GICR_WAKER);
/*
 * IAMROOT, 2022.10.08:
 * - enable 1 -> GICR_WAKER_ProcessorSleep == 0. wakeup 요청
 *   GICR_WAKER_ChildrenAsleep == 0이면 break.
 *   (하나의 cpu라도 깨어있는 상태).
 *
 * - enable 0 -> GICR_WAKER_ProcessorSleep == 1. sleep 요청
 *   GICR_WAKER_ChildrenAsleep == 1이면 break.
 *   (모든 rdist에 연결된 cpu가 잠든상태.)
 */
		if (enable ^ (bool)(val & GICR_WAKER_ChildrenAsleep))
			break;
		cpu_relax();
		udelay(1);
	}

/*
 * IAMROOT, 2022.10.08:
 * - timeout
 */
	if (!count)
		pr_err_ratelimited("redistributor failed to %s...\n",
				   enable ? "wakeup" : "sleep");
}

/*
 * Routines to disable, enable, EOI and route interrupts
 */
/*
 * IAMROOT, 2022.10.01:
 * @index hwirq
 * @offset base register offset
 * @return base register offset
 */
static u32 convert_offset_index(struct irq_data *d, u32 offset, u32 *index)
{
	switch (get_intid_range(d)) {
	case SGI_RANGE:
	case PPI_RANGE:
	case SPI_RANGE:
		*index = d->hwirq;
		return offset;
	case EPPI_RANGE:
		/*
		 * Contrary to the ESPI range, the EPPI range is contiguous
		 * to the PPI range in the registers, so let's adjust the
		 * displacement accordingly. Consistency is overrated.
		 */
		*index = d->hwirq - EPPI_BASE_INTID + 32;
		return offset;
	case ESPI_RANGE:
		*index = d->hwirq - ESPI_BASE_INTID;
		switch (offset) {
		case GICD_ISENABLER:
			return GICD_ISENABLERnE;
		case GICD_ICENABLER:
			return GICD_ICENABLERnE;
		case GICD_ISPENDR:
			return GICD_ISPENDRnE;
		case GICD_ICPENDR:
			return GICD_ICPENDRnE;
		case GICD_ISACTIVER:
			return GICD_ISACTIVERnE;
		case GICD_ICACTIVER:
			return GICD_ICACTIVERnE;
		case GICD_IPRIORITYR:
			return GICD_IPRIORITYRnE;
		case GICD_ICFGR:
			return GICD_ICFGRnE;
		case GICD_IROUTER:
			return GICD_IROUTERnE;
		default:
			break;
		}
		break;
	default:
		break;
	}

	WARN_ON(1);
	*index = d->hwirq;
	return offset;
}

/*
 * IAMROOT, 2022.10.01:
 * - offset에 해당하는 bit가 set이면 return 1, 없으면 0
 */
static int gic_peek_irq(struct irq_data *d, u32 offset)
{
	void __iomem *base;
	u32 index, mask;

	offset = convert_offset_index(d, offset, &index);
	mask = 1 << (index % 32);

	if (gic_irq_in_rdist(d))
		base = gic_data_rdist_sgi_base();
	else
		base = gic_data.dist_base;

	return !!(readl_relaxed(base + offset + (index / 32) * 4) & mask);
}

/*
 * IAMROOT, 2022.10.01:
 * - @d에 해당하는 irq bit를 set하고 rwp wait를 한다.
 */
static void gic_poke_irq(struct irq_data *d, u32 offset)
{
	void (*rwp_wait)(void);
	void __iomem *base;
	u32 index, mask;

/*
 * IAMROOT, 2022.10.01:
 * - index에 hwirq 번호가 가져와지고,
 */
	offset = convert_offset_index(d, offset, &index);

/*
 * IAMROOT, 2022.10.01:
 * - 가져온 hwirq번호로 irq bit를 찾아 mask에 기록한다.
 */
	mask = 1 << (index % 32);

	if (gic_irq_in_rdist(d)) {
		base = gic_data_rdist_sgi_base();
		rwp_wait = gic_redist_wait_for_rwp;
	} else {
		base = gic_data.dist_base;
		rwp_wait = gic_dist_wait_for_rwp;
	}

	writel_relaxed(mask, base + offset + (index / 32) * 4);
	rwp_wait();
}

/*
 * IAMROOT, 2022.10.01:
 * -  Interrupt Clear-Enable Registers
 *    clear 요청. interrupt를 disable한다.
 */
static void gic_mask_irq(struct irq_data *d)
{
	gic_poke_irq(d, GICD_ICENABLER);
}

static void gic_eoimode1_mask_irq(struct irq_data *d)
{
	gic_mask_irq(d);
	/*
	 * When masking a forwarded interrupt, make sure it is
	 * deactivated as well.
	 *
	 * This ensures that an interrupt that is getting
	 * disabled/masked will not get "stuck", because there is
	 * noone to deactivate it (guest is being terminated).
	 */
	if (irqd_is_forwarded_to_vcpu(d))
		gic_poke_irq(d, GICD_ICACTIVER);
}

/*
 * IAMROOT, 2022.10.01:
 * -  Interrupt Set-Enable Registers
 *    Set 요청. interrupt를 enable한다.
 */
static void gic_unmask_irq(struct irq_data *d)
{
	gic_poke_irq(d, GICD_ISENABLER);
}

/*
 * IAMROOT, 2022.10.08:
 * - nmi를 위해서 pmr로 제어 하는지에 대한 여부. arm에는 실제 nmi가 구현되있지 않아
 *   pmr로 간접적으로 사용한다.
 */
static inline bool gic_supports_nmi(void)
{
	return IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI) &&
	       static_branch_likely(&supports_pseudo_nmis);
}

/*
 * IAMROOT, 2022.10.08:
 * - IRQCHIP_STATE_PENDING
 *   irq_retrigger(gic_retrigger). GICD_ISPENDR bit set.
 */
static int gic_irq_set_irqchip_state(struct irq_data *d,
				     enum irqchip_irq_state which, bool val)
{
	u32 reg;

	if (d->hwirq >= 8192) /* SGI/PPI/SPI only */
		return -EINVAL;

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		reg = val ? GICD_ISPENDR : GICD_ICPENDR;
		break;

	case IRQCHIP_STATE_ACTIVE:
		reg = val ? GICD_ISACTIVER : GICD_ICACTIVER;
		break;

	case IRQCHIP_STATE_MASKED:
		reg = val ? GICD_ICENABLER : GICD_ISENABLER;
		break;

	default:
		return -EINVAL;
	}

	gic_poke_irq(d, reg);
	return 0;
}

static int gic_irq_get_irqchip_state(struct irq_data *d,
				     enum irqchip_irq_state which, bool *val)
{
	if (d->hwirq >= 8192) /* PPI/SPI only */
		return -EINVAL;

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		*val = gic_peek_irq(d, GICD_ISPENDR);
		break;

	case IRQCHIP_STATE_ACTIVE:
		*val = gic_peek_irq(d, GICD_ISACTIVER);
		break;

	case IRQCHIP_STATE_MASKED:
		*val = !gic_peek_irq(d, GICD_ISENABLER);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static void gic_irq_set_prio(struct irq_data *d, u8 prio)
{
	void __iomem *base = gic_dist_base(d);
	u32 offset, index;

	offset = convert_offset_index(d, GICD_IPRIORITYR, &index);

	writeb_relaxed(prio, base + offset + index);
}

/*
 * IAMROOT, 2022.10.29:
 * - @hwirq로 ppi index를 구해온다. ppi가 아닐경우 unreachable.
 */
static u32 __gic_get_ppi_index(irq_hw_number_t hwirq)
{
	switch (__get_intid_range(hwirq)) {
	case PPI_RANGE:
		return hwirq - 16;
	case EPPI_RANGE:
		return hwirq - EPPI_BASE_INTID + 16;
	default:
		unreachable();
	}
}

static u32 gic_get_ppi_index(struct irq_data *d)
{
	return __gic_get_ppi_index(d->hwirq);
}

static int gic_irq_nmi_setup(struct irq_data *d)
{
	struct irq_desc *desc = irq_to_desc(d->irq);

	if (!gic_supports_nmi())
		return -EINVAL;

	if (gic_peek_irq(d, GICD_ISENABLER)) {
		pr_err("Cannot set NMI property of enabled IRQ %u\n", d->irq);
		return -EINVAL;
	}

	/*
	 * A secondary irq_chip should be in charge of LPI request,
	 * it should not be possible to get there
	 */
	if (WARN_ON(gic_irq(d) >= 8192))
		return -EINVAL;

	/* desc lock should already be held */
	if (gic_irq_in_rdist(d)) {
		u32 idx = gic_get_ppi_index(d);

		/* Setting up PPI as NMI, only switch handler for first NMI */
		if (!refcount_inc_not_zero(&ppi_nmi_refs[idx])) {
			refcount_set(&ppi_nmi_refs[idx], 1);
			desc->handle_irq = handle_percpu_devid_fasteoi_nmi;
		}
	} else {
		desc->handle_irq = handle_fasteoi_nmi;
	}

	gic_irq_set_prio(d, GICD_INT_NMI_PRI);

	return 0;
}

static void gic_irq_nmi_teardown(struct irq_data *d)
{
	struct irq_desc *desc = irq_to_desc(d->irq);

	if (WARN_ON(!gic_supports_nmi()))
		return;

	if (gic_peek_irq(d, GICD_ISENABLER)) {
		pr_err("Cannot set NMI property of enabled IRQ %u\n", d->irq);
		return;
	}

	/*
	 * A secondary irq_chip should be in charge of LPI request,
	 * it should not be possible to get there
	 */
	if (WARN_ON(gic_irq(d) >= 8192))
		return;

	/* desc lock should already be held */
	if (gic_irq_in_rdist(d)) {
		u32 idx = gic_get_ppi_index(d);

		/* Tearing down NMI, only switch handler for last NMI */
		if (refcount_dec_and_test(&ppi_nmi_refs[idx]))
			desc->handle_irq = handle_percpu_devid_irq;
	} else {
		desc->handle_irq = handle_fasteoi_irq;
	}

	gic_irq_set_prio(d, GICD_INT_DEF_PRI);
}

/*
 * IAMROOT, 2022.11.12:
 * - write eoi.
 */
static void gic_eoi_irq(struct irq_data *d)
{
	gic_write_eoir(gic_irq(d));
}

static void gic_eoimode1_eoi_irq(struct irq_data *d)
{
	/*
	 * No need to deactivate an LPI, or an interrupt that
	 * is is getting forwarded to a vcpu.
	 */
	if (gic_irq(d) >= 8192 || irqd_is_forwarded_to_vcpu(d))
		return;
	gic_write_dir(gic_irq(d));
}

/*
 * IAMROOT, 2022.10.01:
 * @return 0 = IRQ_SET_MASK_OK
 */
static int gic_set_type(struct irq_data *d, unsigned int type)
{
	enum gic_intid_range range;
	unsigned int irq = gic_irq(d);
	void (*rwp_wait)(void);
	void __iomem *base;
	u32 offset, index;
	int ret;

	range = get_intid_range(d);

/*
 * IAMROOT, 2022.10.01:
 * - sgi는 항상 EDGE_RISING이여야 한다. 맞다면 IRQ_SET_MASK_OK를 return.
 */
	/* Interrupt configuration for SGIs can't be changed */
	if (range == SGI_RANGE)
		return type != IRQ_TYPE_EDGE_RISING ? -EINVAL : 0;

/*
 * IAMROOT, 2022.10.01:
 * - spi는 IRQ_TYPE_LEVEL_HIGH이거나 EDGE_RISING이여야 한다.
 */
	/* SPIs have restrictions on the supported types */
	if ((range == SPI_RANGE || range == ESPI_RANGE) &&
	    type != IRQ_TYPE_LEVEL_HIGH && type != IRQ_TYPE_EDGE_RISING)
		return -EINVAL;

	if (gic_irq_in_rdist(d)) {
		base = gic_data_rdist_sgi_base();
		rwp_wait = gic_redist_wait_for_rwp;
	} else {
		base = gic_data.dist_base;
		rwp_wait = gic_dist_wait_for_rwp;
	}

	offset = convert_offset_index(d, GICD_ICFGR, &index);

	ret = gic_configure_irq(index, type, base + offset, rwp_wait);
	if (ret && (range == PPI_RANGE || range == EPPI_RANGE)) {
		/* Misconfigured PPIs are usually not fatal */
		pr_warn("GIC: PPI INTID%d is secure or misconfigured\n", irq);
		ret = 0;
	}

	return ret;
}

static int gic_irq_set_vcpu_affinity(struct irq_data *d, void *vcpu)
{
	if (get_intid_range(d) == SGI_RANGE)
		return -EINVAL;

	if (vcpu)
		irqd_set_forwarded_to_vcpu(d);
	else
		irqd_clr_forwarded_to_vcpu(d);
	return 0;
}

static u64 gic_mpidr_to_affinity(unsigned long mpidr)
{
	u64 aff;

	aff = ((u64)MPIDR_AFFINITY_LEVEL(mpidr, 3) << 32 |
	       MPIDR_AFFINITY_LEVEL(mpidr, 2) << 16 |
	       MPIDR_AFFINITY_LEVEL(mpidr, 1) << 8  |
	       MPIDR_AFFINITY_LEVEL(mpidr, 0));

	return aff;
}

static void gic_deactivate_unhandled(u32 irqnr)
{
	if (static_branch_likely(&supports_deactivate_key)) {
		if (irqnr < 8192)
			gic_write_dir(irqnr);
	} else {
		gic_write_eoir(irqnr);
	}
}

static inline void gic_handle_nmi(u32 irqnr, struct pt_regs *regs)
{
	bool irqs_enabled = interrupts_enabled(regs);
	int err;

	if (irqs_enabled)
		nmi_enter();

	if (static_branch_likely(&supports_deactivate_key))
		gic_write_eoir(irqnr);
	/*
	 * Leave the PSR.I bit set to prevent other NMIs to be
	 * received while handling this one.
	 * PSR.I will be restored when we ERET to the
	 * interrupted context.
	 */
	err = handle_domain_nmi(gic_data.domain, irqnr, regs);
	if (err)
		gic_deactivate_unhandled(irqnr);

	if (irqs_enabled)
		nmi_exit();
}

/*
 * IAMROOT, 2022.11.05: 
 * Interrupt Controller Interrupt Acknowledge Register를 읽어
 * 인터럽트 번호를 알아온다.
 */
static u32 do_read_iar(struct pt_regs *regs)
{
	u32 iar;

/*
 * IAMROOT, 2022.11.05: 
 * Pesudo-NMI를 사용하면서 일반 irq가 disable 상태라면 pmr이 설정된 상태인것이다.
 */
	if (gic_supports_nmi() && unlikely(!interrupts_enabled(regs))) {
		u64 pmr;

		/*
		 * We were in a context with IRQs disabled. However, the
		 * entry code has set PMR to a value that allows any
		 * interrupt to be acknowledged, and not just NMIs. This can
		 * lead to surprising effects if the NMI has been retired in
		 * the meantime, and that there is an IRQ pending. The IRQ
		 * would then be taken in NMI context, something that nobody
		 * wants to debug twice.
		 *
		 * Until we sort this, drop PMR again to a level that will
		 * actually only allow NMIs before reading IAR, and then
		 * restore it to what it was.
		 */
/*
 * IAMROOT, 2022.11.05: 
 * - papago
 *   우리는 IRQ가 비활성화된 상황에 있었습니다. 그러나 입력 코드는
 *   PMR을 NMI뿐만 아니라 모든 인터럽트를 승인할 수 있는 값으로 설정했습니다.
 *   NMI가 그 사이에 폐기되고 IRQ가 보류 중인 경우 이는 놀라운 효과로 이어질
 *   수 있습니다. 그런 다음 IRQ는 NMI 컨텍스트에서 수행되며 아무도 두 번
 *   디버그하기를 원하지 않습니다.
 *
 *   이것을 정렬할 때까지 PMR을 IAR을 읽기 전에 실제로 NMI만 허용하는 수준으로
 *   다시 떨어뜨린 다음 원래 상태로 복원합니다.
 *
 * - spurious interrupts
 *   https://developer.arm.com/documentation/ihi0048/b/Introduction/Terminology/Spurious-interrupts
 *   https://en.wikipedia.org/wiki/Interrupt
 * - Git Blame
 *   spurious interrupt(nmi)가 발생(iar을 읽기전에 retired)한 상황에서 irq가
 *   pending중일때, nmi context에서 pending중인 irq에 대한 ack가 나갈수있다는것
 *   같다.
 *   그래서 spurious interrupt를 handle_bad_irq로 처리할려고
 *   (core-api/generic.rst) 확실히 off시킨후 iar을 얻어올려는거 같다.
 *   (부정확)
 *
 * - pmr을 사용하여 일반 인터럽트를 disable 한상태에서 iar을 통해 인터럽트 번호를
 * 읽어온다. 그 후 pmr을 원래 값으로 복구한다.
 */
		pmr = gic_read_pmr();
		gic_pmr_mask_irqs();
		isb();

		iar = gic_read_iar();

		gic_write_pmr(pmr);
	} else {
		iar = gic_read_iar();
	}

	return iar;
}

/*
 * IAMROOT, 2022.10.08:
 * - TODO
 * - gic control handler.
 *   interrupt가 vector table다음으로 받는 handler.
 * - irq 흐름
 *   vectors(arch/arm64/kernel/entry.S)
 *     v
 *   chip handler(handler_arch_irq, gic의 경우 gic_handle_irq) <--- 현재
 *     v
 *   flow handler
 *     v
 *   irq_handler
 *
 * - gic 흐름
 *   interrupt 발생
 *    v
 *   register backup
 *    v 
 *    gic_handle_irq(진입)
 *                v
 *               ack(iar)
 *                v
 *               interrupt enable(PSR의 IF clear 및 PMR-NMI 적용)
 *              (NMI의 경우 PMR-NMI 우선순위보다 높은 interrupt만
 *              들어올수있게 enable된다.)
 *                v
 *              (eoimode1: drop)
 *                v
 *              flow handler 실행
 *                v
 *              (eoimode:0 eoi + drop)
 *                v
 *    gic_handle_irq(완료)
 *    v
 *    register restore
 */
static asmlinkage void __exception_irq_entry gic_handle_irq(struct pt_regs *regs)
{
	u32 irqnr;

	irqnr = do_read_iar(regs);

/*
 * IAMROOT, 2022.11.05: 
 * 1020~1023번 인터럽트는 스페셜 인터럽트로 핸들러에 대한 호출을 할 필요 없다.
 */
	/* Check for special IDs first */
	if ((irqnr >= 1020 && irqnr <= 1023))
		return;

/*
 * IAMROOT, 2022.11.05: 
 * Pesudo-NMI 인터럽트에 대한 핸들러 호출이다.
 * 예) request_nmi() & request_percpu_nmi()
 */
	if (gic_supports_nmi() &&
	    unlikely(gic_read_rpr() == GICD_INT_RPR_PRI(GICD_INT_NMI_PRI))) {
		gic_handle_nmi(irqnr, regs);
		return;
	}

/*
 * IAMROOT, 2022.11.05: 
 * Pesudo-NMI를 지원하면서 GIC 역시 priority masking을 지원하는 경우 
 * pmr을 사용하여 일반 인터럽트를 disable 하고, DAIF 중 IF를 clear한다.
 *
 * 중요) 이렇게 하면 인터럽트 처리 중에도 IF를 clear하여 NMI 인터럽트는 
 * 진입이 가능하도록 허용한다.
 */
	if (gic_prio_masking_enabled()) {
		gic_pmr_mask_irqs();
		gic_arch_enable_irqs();
	}

/*
 * IAMROOT, 2022.11.05: 
 * EL2 하이퍼 바이저를 지원하여 eoimode=1을 사용하는 경우에는
 * 이 시점에서 eoi drop을 한다. 이러한 경우 해당 인터럽트 번호만
 * drop 되기 때문에 다른 인터럽트들이 진입할 수 있게된다.
 *
 * 물론, Pesudo-NMI를 제외하면 아직 cpu의 irq가 enable(IF mask)상태는 아니다.
 */
	if (static_branch_likely(&supports_deactivate_key))
		gic_write_eoir(irqnr);
	else
		isb();

	if (handle_domain_irq(gic_data.domain, irqnr, regs)) {
		WARN_ONCE(true, "Unexpected interrupt received!\n");
		gic_deactivate_unhandled(irqnr);
	}
}

/*
 * IAMROOT, 2022.10.08:
 * - pribits(우선순위 평탄화 bits) 를 가져온다.
 * - PRIbits, bits [10:8] 
 *
 *   ICC_CTLR_EL1.PRIbits         GROUP0, GROUP1      support group1
 *   (Read only)          pribits single    사용여부  two             사용여부
 *   0                    1       g.sssssss  X        ssssssss         X
 *   1                    2       gg.ssssss  X        g.sssssss        X
 *   2                    3       ggg.sssss  X        gg.ssssss        X
 *   3                    4       gggg.ssss  O        ggg.sssss        X
 *   4                    5       ggggg.sss  O        gggg.ssss        O
 *   5                    6       gggggg.ss  O        ggggg.sss        O
 *   6                    7       ggggggg.s  O        gggggg.ss        O
 *   7                    8       gggggggg.  O        ggggggg.s        O
 */
static u32 gic_get_pribits(void)
{
	u32 pribits;

	pribits = gic_read_ctlr();
	pribits &= ICC_CTLR_EL1_PRI_BITS_MASK;
	pribits >>= ICC_CTLR_EL1_PRI_BITS_SHIFT;
	pribits++;

	return pribits;
}

/*
 * IAMROOT, 2022.10.08:
 * - pmr에 prbits를 set해보아 kernel이 group0를 다룰수있는지 확인한다.
 */
static bool gic_has_group0(void)
{
	u32 val;
	u32 old_pmr;

	old_pmr = gic_read_pmr();

	/*
	 * Let's find out if Group0 is under control of EL3 or not by
	 * setting the highest possible, non-zero priority in PMR.
	 *
	 * If SCR_EL3.FIQ is set, the priority gets shifted down in
	 * order for the CPU interface to set bit 7, and keep the
	 * actual priority in the non-secure range. In the process, it
	 * looses the least significant bit and the actual priority
	 * becomes 0x80. Reading it back returns 0, indicating that
	 * we're don't have access to Group0.
	 */
/*
 * IAMROOT, 2022.10.08:
 * - papago
 *   PMR에서 0이 아닌 가능한 가장 높은 우선 순위를 설정하여 Group0이 EL3의 제어
 *   하에 있는지 여부를 알아보겠습니다.
 *
 *   SCR_EL3.FIQ가 설정되면 CPU 인터페이스가 비트 7을 설정하고 실제 우선 순위를
 *   비보안 범위로 유지하기 위해 우선 순위가 아래로 이동합니다. 이 과정에서
 *   최하위 비트가 손실되고 실제 우선 순위는 0x80이 됩니다. 다시 읽으면 0이
 *   반환되어 Group0에 액세스할 수 없음을 나타냅니다.
 *
 * - GIC문서 4.8 Interrupt prioritization 참고
 *
 *   ICC_CTLR_EL1.PRIbits         GROUP0, GROUP1      support group1
 *   (Read only)          pribits single    사용여부  two             사용여부
 *   0                    1       g.sssssss  X        ssssssss         X
 *   1                    2       gg.ssssss  X        g.sssssss        X
 *   2                    3       ggg.sssss  X        gg.ssssss        X
 *   3                    4       gggg.ssss  O        ggg.sssss        X
 *   4                    5       ggggg.sss  O        gggg.ssss        O
 *   5                    6       gggggg.ss  O        ggggg.sss        O
 *   6                    7       ggggggg.s  O        gggggg.ss        O
 *   7                    8       gggggggg.  O        ggggggg.s        O
 *
 *   - ex) gic_get_pribits == 5, BIT(3)
 *        
 *                                GROUP0, GROUP1      support group1
 *   ICC_CTLR_EL1.PRIbits pribits single              two                     
 *   4                    5       ggggg.sss           gggg.ssss         
 *                                    ^                    ^
 *                                kernel이            kernel이
 *                                group0를 다를수     group0를 못다루는 상태
 *                                있는 상태.          set후 값을 읽으면 없다.
 *                                set후 읽으면
 *                                값이 있다.
 */
	gic_write_pmr(BIT(8 - gic_get_pribits()));

/*
 * IAMROOT, 2022.10.08:
 * - 값이 다시 읽어지면 single secure state으로 동작하고, 이때는 kernel이
 *   group0, group1을 다룰수있다.
 */
	val = gic_read_pmr();

/*
 * IAMROOT, 2022.10.08:
 * - 원복
 */
	gic_write_pmr(old_pmr);

	return val != 0;
}

/*
 * IAMROOT, 2022.10.08:
 * - GICD류를 초기값으로 설정한다.
 * - GICv3 Software Overview Official 의 4. Configuring the GIC참고
 *   register 초기화 방법이 있다.
 */
static void __init gic_dist_init(void)
{
	unsigned int i;
	u64 affinity;
	void __iomem *base = gic_data.dist_base;
	u32 val;

/*
 * IAMROOT, 2022.10.08:
 * - disable후 register를 초기화 하고 다시 enable한다.
 *   GICD_CTLR.DS = 0.
 */
	/* Disable the distributor */
	writel_relaxed(0, base + GICD_CTLR);

/*
 * IAMROOT, 2022.10.08:
 * - disable wait
 */
	gic_dist_wait_for_rwp();

	/*
	 * Configure SPIs as non-secure Group-1. This will only matter
	 * if the GIC only has a single security state. This will not
	 * do the right thing if the kernel is running in secure mode,
	 * but that's not the intended use case anyway.
	 */

/*
 * IAMROOT, 2022.10.08:
 * - group 개념
 * - group0 secure
 *   group1 - non-secure 
 *          - secure
 * - all bits set
 *
 * - GICD_CTLR.DS 상태에서 따라 아래 초기화하는 레지스터의 기능이 바뀐다.
 *
 * --- GICD_CTLR는 다음 상태에 따라 register가 바뀐다. (Bank register)
 *  - When access is Secure, in a system that supports two Security states:
 *    EL3를 제외하고 나머지 EL에서 secure 상태를 나눠서 운영을 할지 말지.
 *    secure가 접근하게되는 GICD_CTLR.
 *
 *  - When access is Non-secure, in a system that supports two Security states:
 *    secure가 있는상태. kernel이 DS를 조작할이유가 없으므로 아에 DS bit가 없다.
 *
 *  - When in a system that supports only a single Security state:
 *    secure가 없는상태.(kernel만 있는 상태) 무조건 disable security이므로
 *    DS가 RAO/WI.
 *
 * --- GICD_CTLR.DS(disable security)

 *  0b0 Non-secure accesses are not permitted to access and modify registers
 *  that control Group 0 interrupts.
 *  0b1 Non-secure accesses are permitted to access and modify registers
 *  that control Group 0 interrupts.
 *
 *  If DS is written from 0 to 1 when GICD_CTLR.ARE_S == 1, then GICD_CTLR.ARE
 *  for the single Security state is RAO/WI.
 *  If the Distributor only supports a single Security state, this bit is RAO/WI.
 *  If the Distributor supports two Security states, it IMPLEMENTATION
 *  whether this bit is programmable or implemented as RAZ/WI.
 *  When this field is set to 1, all accesses to GICD_CTLR access the single
 *  Security state view, and all bits are accessible.
 *
 *  - DS가 RAO/WI(read as one / write ignore) 인 경우
 *    1. ARE_S set인 경우
 *    2. Distributor가 single security state만을 지원하는 경우
 *
 */
	for (i = 32; i < GIC_LINE_NR; i += 32)
		writel_relaxed(~0, base + GICD_IGROUPR + i / 8);

	/* Extended SPI range, not handled by the GICv2/GICv3 common code */
/*
 * IAMROOT, 2022.10.08:
 * - gic espi 까지 전부 disable, inactivate
 */
	for (i = 0; i < GIC_ESPI_NR; i += 32) {
		writel_relaxed(~0U, base + GICD_ICENABLERnE + i / 8);
		writel_relaxed(~0U, base + GICD_ICACTIVERnE + i / 8);
	}

/*
 * IAMROOT, 2022.10.08:
 * - 위에 GICD_IGROUPR와 똑같이 설정한다. espi측.
 */
	for (i = 0; i < GIC_ESPI_NR; i += 32)
		writel_relaxed(~0U, base + GICD_IGROUPRnE + i / 8);

/*
 * IAMROOT, 2022.10.08:
 * - level-sensitive로 초기화한다.
 */
	for (i = 0; i < GIC_ESPI_NR; i += 16)
		writel_relaxed(0, base + GICD_ICFGRnE + i / 4);

/*
 * IAMROOT, 2022.10.08:
 * - default interrupt 우선순위를 0xa0(실제는 0xd0)으로 전부 설정한다.
 */
	for (i = 0; i < GIC_ESPI_NR; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4, base + GICD_IPRIORITYRnE + i);

/*
 * IAMROOT, 2022.10.08:
 * - v1, v2 common들도 초기화해준다.
 */
	/* Now do the common stuff, and wait for the distributor to drain */
	gic_dist_config(base, GIC_LINE_NR, gic_dist_wait_for_rwp);

/*
 * IAMROOT, 2022.10.08:
 * - When access is Non-secure, in a system that supports two Security states.
 *   G1A, G1, ARE_NS enable.
 */
	val = GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1A | GICD_CTLR_ENABLE_G1;
	if (gic_data.rdists.gicd_typer2 & GICD_TYPER2_nASSGIcap) {
		pr_info("Enabling SGIs without active state\n");
		val |= GICD_CTLR_nASSGIreq;
	}

	/* Enable distributor with ARE, Group1 */
	writel_relaxed(val, base + GICD_CTLR);

	/*
	 * Set all global interrupts to the boot CPU only. ARE must be
	 * enabled.
	 */
/*
 * IAMROOT, 2022.10.08:
 * - affinity를 읽어서 gic에 기록한다.
 */
	affinity = gic_mpidr_to_affinity(cpu_logical_map(smp_processor_id()));
	for (i = 32; i < GIC_LINE_NR; i++)
		gic_write_irouter(affinity, base + GICD_IROUTER + i * 8);

	for (i = 0; i < GIC_ESPI_NR; i++)
		gic_write_irouter(affinity, base + GICD_IROUTERnE + i * 8);
}

/*
 * IAMROOT, 2022.10.08:
 * @fn giv3의 경우 __gic_update_rdist_properties
 * - nr_redist_regions수만큼 iterate하며 v3, v4 gic에 대해 fn을 호출한다.
 */
static int gic_iterate_rdists(int (*fn)(struct redist_region *, void __iomem *))
{
	int ret = -ENODEV;
	int i;

/*
 * IAMROOT, 2022.10.08:
 * - nr_redist_regions은 일반적으로 numa수와 비슷하다.
 */
	for (i = 0; i < gic_data.nr_redist_regions; i++) {
		void __iomem *ptr = gic_data.redist_regions[i].redist_base;
		u64 typer;
		u32 reg;

		reg = readl_relaxed(ptr + GICR_PIDR2) & GIC_PIDR2_ARCH_MASK;
		if (reg != GIC_PIDR2_ARCH_GICv3 &&
		    reg != GIC_PIDR2_ARCH_GICv4) { /* We're in trouble... */
			pr_warn("No redistributor present @%p\n", ptr);
			break;
		}

		do {
			typer = gic_read_typer(ptr + GICR_TYPER);
			ret = fn(gic_data.redist_regions + i, ptr);
			if (!ret)
				return 0;

/*
 * IAMROOT, 2022.10.08:
 * - single이 아니면 건너뛰면서 last까지 재실행.
 */
			if (gic_data.redist_regions[i].single_redist)
				break;

			if (gic_data.redist_stride) {
				ptr += gic_data.redist_stride;
/*
 * IAMROOT, 2022.10.27:
 * - gic 문서 참고
 *   Each Redistributor defines two 64KB frames in the physical address map:
 *   • RD_base for controlling the overall behavior of the Redistributor,
 *     for controlling LPIs, and for generating LPIs in a system that does not include
 *     at least one ITS..
 *   • SGI_base for controlling and generating PPIs and SGIs. 
 *     The frame for each Redistributor must be contiguous and must be ordered as
 *     follows: 
 *     1. RD_base
 *     2. SGI_base
 *
 *   In GICv4, there are two additional 64KB frames:
 *   • A frame to control virtual LPIs. The base address of this frame is referred to
 *     as VLPI_base.
 *   • A frame for a reserved page.
 */
			} else {
				ptr += SZ_64K * 2; /* Skip RD_base + SGI_base */
				if (typer & GICR_TYPER_VLPIS)
					ptr += SZ_64K * 2; /* Skip VLPI_base + reserved page */
			}
		} while (!(typer & GICR_TYPER_LAST));
	}

	return ret ? -ENODEV : 0;
}

/*
 * IAMROOT, 2022.10.08:
 * - 현재 cpu에 해당하는 rdist를 찾아서 현재 cpu에 해당하는
 *   rdist_rd_base와 phy address를 저장한다.
 */
static int __gic_populate_rdist(struct redist_region *region, void __iomem *ptr)
{
	unsigned long mpidr = cpu_logical_map(smp_processor_id());
	u64 typer;
	u32 aff;

	/*
	 * Convert affinity to a 32bit value that can be matched to
	 * GICR_TYPER bits [63:32].
	 */
	aff = (MPIDR_AFFINITY_LEVEL(mpidr, 3) << 24 |
	       MPIDR_AFFINITY_LEVEL(mpidr, 2) << 16 |
	       MPIDR_AFFINITY_LEVEL(mpidr, 1) << 8 |
	       MPIDR_AFFINITY_LEVEL(mpidr, 0));

	typer = gic_read_typer(ptr + GICR_TYPER);
/*
 * IAMROOT, 2022.10.08:
 * - 현재 cpu인것을 찾는다.
 */
	if ((typer >> 32) == aff) {
		u64 offset = ptr - region->redist_base;
		raw_spin_lock_init(&gic_data_rdist()->rd_lock);
		gic_data_rdist_rd_base() = ptr;
		gic_data_rdist()->phys_base = region->phys_base + offset;

		pr_info("CPU%d: found redistributor %lx region %d:%pa\n",
			smp_processor_id(), mpidr,
			(int)(region - gic_data.redist_regions),
			&gic_data_rdist()->phys_base);
/*
 * IAMROOT, 2022.10.08:
 * - 찾은 시점에서 종료.
 */
		return 0;
	}

	/* Try next one */
/*
 * IAMROOT, 2022.10.08:
 * - 못찾았으면 continue
 */
	return 1;
}

/*
 * IAMROOT, 2022.10.08:
 * - 현재 cpu를 찾아서__gic_populate_rdist()를 설정한다.
 */
static int gic_populate_rdist(void)
{
	if (gic_iterate_rdists(__gic_populate_rdist) == 0)
		return 0;

	/* We couldn't even deal with ourselves... */
	WARN(true, "CPU%d: mpidr %lx has no re-distributor!\n",
	     smp_processor_id(),
	     (unsigned long)cpu_logical_map(smp_processor_id()));
	return -ENODEV;
}

/*
 * IAMROOT, 2022.10.08:
 * - cpu별의 GICR_TYPER를 읽어서 지원하는 feature를 설정한다.
 *   ppi개수도 여기서 읽어 진다.
 */
static int __gic_update_rdist_properties(struct redist_region *region,
					 void __iomem *ptr)
{
	u64 typer = gic_read_typer(ptr + GICR_TYPER);

	gic_data.rdists.has_vlpis &= !!(typer & GICR_TYPER_VLPIS);

	/* RVPEID implies some form of DirectLPI, no matter what the doc says... :-/ */
	gic_data.rdists.has_rvpeid &= !!(typer & GICR_TYPER_RVPEID);
	gic_data.rdists.has_direct_lpi &= (!!(typer & GICR_TYPER_DirectLPIS) |
					   gic_data.rdists.has_rvpeid);
	gic_data.rdists.has_vpend_valid_dirty &= !!(typer & GICR_TYPER_DIRTY);

	/* Detect non-sensical configurations */
	if (WARN_ON_ONCE(gic_data.rdists.has_rvpeid && !gic_data.rdists.has_vlpis)) {
		gic_data.rdists.has_direct_lpi = false;
		gic_data.rdists.has_vlpis = false;
		gic_data.rdists.has_rvpeid = false;
	}

	gic_data.ppi_nr = min(GICR_TYPER_NR_PPIS(typer), gic_data.ppi_nr);

	return 1;
}

/*
 * IAMROOT, 2022.10.08:
 * - redist_regions 개수 * redist set만큼 iterate하며(cpu개수와 동일) 각각의
 *   redist register에서 __gic_update_rdist_properties 수행
 * - 설정된 feature에 대한 정보를 print한다.
 */
static void gic_update_rdist_properties(void)
{
	gic_data.ppi_nr = UINT_MAX;
	gic_iterate_rdists(__gic_update_rdist_properties);
	if (WARN_ON(gic_data.ppi_nr == UINT_MAX))
		gic_data.ppi_nr = 0;
	pr_info("%d PPIs implemented\n", gic_data.ppi_nr);
	if (gic_data.rdists.has_vlpis)
		pr_info("GICv4 features: %s%s%s\n",
			gic_data.rdists.has_direct_lpi ? "DirectLPI " : "",
			gic_data.rdists.has_rvpeid ? "RVPEID " : "",
			gic_data.rdists.has_vpend_valid_dirty ? "Valid+Dirty " : "");
}

/* Check whether it's single security state view */
/*
 * IAMROOT, 2022.10.29:
 * return true : security disable
 *        false : security enable
 *
 * - DS.0 == two security
 * - DS.1 == single security
 */
static inline bool gic_dist_security_disabled(void)
{
	return readl_relaxed(gic_data.dist_base + GICD_CTLR) & GICD_CTLR_DS;
}

/*
 * IAMROOT, 2022.10.08:
 * -  group0, group1 int에 대한 설정 및 group1 int enable.
 */
static void gic_cpu_sys_reg_init(void)
{
	int i, cpu = smp_processor_id();
	u64 mpidr = cpu_logical_map(cpu);
	u64 need_rss = MPIDR_RS(mpidr);
	bool group0;
	u32 pribits;

	/*
	 * Need to check that the SRE bit has actually been set. If
	 * not, it means that SRE is disabled at EL2. We're going to
	 * die painfully, and there is nothing we can do about it.
	 *
	 * Kindly inform the luser.
	 */
/*
 * IAMROOT, 2022.10.08:
 * - papgo
 *   SRE 비트가 실제로 설정되었는지 확인해야 합니다. 그렇지 않은 경우 SRE가
 *   EL2에서 비활성화되었음을 의미합니다. 우리는 고통스럽게 죽을 것이고 우리가
 *   그것에 대해 할 수 있는 것은 아무것도 없습니다.
 *
 *   루저에게 알려주세요.
 */
	if (!gic_enable_sre())
		pr_err("GIC: unable to set SRE (disabled at EL2), panic ahead\n");

/*
 * IAMROOT, 2022.10.08:
 * - pribits(우선순위 평탄화 bits) 를 가져온다.
 */
	pribits = gic_get_pribits();

/*
 * IAMROOT, 2022.10.08:
 * - kernel이 group0를 지원하는지.
 */
	group0 = gic_has_group0();

	/* Set priority mask register */
	if (!gic_prio_masking_enabled()) {
/*
 * IAMROOT, 2022.10.08:
 * - masking을 지원안하는경우 pmr을 통해서 막는건 지원안한다.
 */
		write_gicreg(DEFAULT_PMR_VALUE, ICC_PMR_EL1);
	} else if (gic_supports_nmi()) {
		/*
		 * Mismatch configuration with boot CPU, the system is likely
		 * to die as interrupt masking will not work properly on all
		 * CPUs
		 *
		 * The boot CPU calls this function before enabling NMI support,
		 * and as a result we'll never see this warning in the boot path
		 * for that CPU.
		 */
/*
 * IAMROOT, 2022.10.08:
 * - papago
 *   부팅 CPU와 구성이 일치하지 않습니다. 모든 CPU에서 인터럽트 마스킹이 제대로
 *   작동하지 않기 때문에 시스템이 중단될 가능성이 높습니다. 부팅 CPU는 NMI
 *   지원을 활성화하기 전에 이 함수를 호출합니다. 따라서 해당 CPU의 부팅 경로에는
 *   이 경고가 표시되지 않습니다.
 *
 * - single secure state일때 group0 & group1 둘다 kernel이 제어한다.
 *   secure라면 group1만 제어한다.
 */
		if (static_branch_unlikely(&gic_nonsecure_priorities))
			WARN_ON(!group0 || gic_dist_security_disabled());
		else
			WARN_ON(group0 && !gic_dist_security_disabled());
	}

	/*
	 * Some firmwares hand over to the kernel with the BPR changed from
	 * its reset value (and with a value large enough to prevent
	 * any pre-emptive interrupts from working at all). Writing a zero
	 * to BPR restores is reset value.
	 */
/*
 * IAMROOT, 2022.10.08:
 * - papago
 *   일부 펌웨어는 리셋 값에서 변경된 BPR(선점형 인터럽트가 전혀 작동하지 않도록
 *   충분히 큰 값)과 함께 커널로 전달합니다. BPR 복원에 0을 쓰는 것은 값을
 *   재설정하는 것입니다.
 * - BPR(Binary Point Register)
 */
	gic_write_bpr1(0);

/*
 * IAMROOT, 2022.10.08:
 * - eoimode 설명(GICv3 Software Overview Official 참고)
 *   EOImode = 0
 *   A write to ICC_EOIR0_EL1 for Group 0 interrupts, or ICC_EOIR1_EL1 for Group 1
 *   interrupts, performs both the priority drop and deactivation. This is the
 *   model typically used for a simple bare metal environment.
 *
 *   EOImode = 1
 *   A write to ICC_EOIR_EL10 for Group 0 interrupts, or ICC_EOIR1_EL1
 *   for Group 1 interrupts results in a priority drop. A separate write to
 *   ICC_DIR_EL1 is required for deactivation. This mode is often used for
 *   virtualization purposes.
 *
 * --- eoimode example,  guest os한테 int50이 들어오는 상황
 *  1. eoimode0
 *    guest os한테 넘겨줌. drop / inactivate 둘다 발생. 
 *    guest os가 다 처리할때까지 int50을 못받음.
 *
 *  1. eoimode1
 *    guest os한테 넘겨줌. drop 발생. 
 *    guest os가 처리중이여도 int50을 다시 받을수 있는 상태. 다른 guest os한테도
 *    넘길수있음.
 * ---
 *
 * - eoimode 1로 할지, mode 0로 할지 결정.
 */
	if (static_branch_likely(&supports_deactivate_key)) {
		/* EOI drops priority only (mode 1) */
		gic_write_ctlr(ICC_CTLR_EL1_EOImode_drop);
	} else {
		/* EOI deactivates interrupt too (mode 0) */
		gic_write_ctlr(ICC_CTLR_EL1_EOImode_drop_dir);
	}

	/* Always whack Group0 before Group1 */

/*
 * IAMROOT, 2022.10.08:
 * - ICC_AP0R<n>_EL1, Interrupt Controller Active Priorities Group 0 Registers
 * - Group 0 active priorities.
 * - R0는 무조건
 *   R1는 6bit이상일때,
 *   R2, R3는 7bit이상일때.
 * - ICC_AP0R을 reset한다.
 */
	if (group0) {
		switch(pribits) {
		case 8:
		case 7:
			write_gicreg(0, ICC_AP0R3_EL1);
			write_gicreg(0, ICC_AP0R2_EL1);
			fallthrough;
		case 6:
			write_gicreg(0, ICC_AP0R1_EL1);
			fallthrough;
		case 5:
		case 4:
			write_gicreg(0, ICC_AP0R0_EL1);
		}

		isb();
	}

/*
 * IAMROOT, 2022.10.08:
 * - group1에 대한것도 수행한다.
 */
	switch(pribits) {
	case 8:
	case 7:
		write_gicreg(0, ICC_AP1R3_EL1);
		write_gicreg(0, ICC_AP1R2_EL1);
		fallthrough;
	case 6:
		write_gicreg(0, ICC_AP1R1_EL1);
		fallthrough;
	case 5:
	case 4:
		write_gicreg(0, ICC_AP1R0_EL1);
	}

	isb();

	/* ... and let's hit the road... */
/*
 * IAMROOT, 2022.10.15:
 * - group1 int enable
 */
	gic_write_grpen1(1);

	/* Keep the RSS capability status in per_cpu variable */
	per_cpu(has_rss, cpu) = !!(gic_read_ctlr() & ICC_CTLR_EL1_RSS);

	/* Check all the CPUs have capable of sending SGIs to other CPUs */
/*
 * IAMROOT, 2022.10.15:
 * - 현재 cpu가 다른 cpu에 대해서 rss가 필요한데 rss가 없는 경우를 확인한다.
 *
 * - MPID_RS를 통해 0xF0를 검사함으로써 RSS가 필요한지 확인한다.
 *   have_rss가 false. 즉 ICC_CTLR_EL1.RSS = 0인데 affinity가 0x0f를 넘어버리면
 *   error가 된다.
 */
	for_each_online_cpu(i) {
		bool have_rss = per_cpu(has_rss, i) && per_cpu(has_rss, cpu);

		need_rss |= MPIDR_RS(cpu_logical_map(i));
		if (need_rss && (!have_rss))
			pr_crit("CPU%d (%lx) can't SGI CPU%d (%lx), no RSS\n",
				cpu, (unsigned long)mpidr,
				i, (unsigned long)cpu_logical_map(i));
	}

	/**
	 * GIC spec says, when ICC_CTLR_EL1.RSS==1 and GICD_TYPER.RSS==0,
	 * writing ICC_ASGI1R_EL1 register with RS != 0 is a CONSTRAINED
	 * UNPREDICTABLE choice of :
	 *   - The write is ignored.
	 *   - The RS field is treated as 0.
	 */
	if (need_rss && (!gic_data.has_rss))
		pr_crit_once("RSS is required but GICD doesn't support it\n");
}

static bool gicv3_nolpi;

static int __init gicv3_nolpi_cfg(char *buf)
{
	return strtobool(buf, &gicv3_nolpi);
}
early_param("irqchip.gicv3_nolpi", gicv3_nolpi_cfg);

/*
 * IAMROOT, 2022.10.28:
 * - LPIS. gic 문서참고
 *   Locality-specific Peripheral Interrupts (LPIs) are edge-triggered
 *   message-based interrupts that can use an Interrupt Translation Service
 *   (ITS), if it is implemented, to route an interrupt to a specific
 *   Redistributor and connected PE.
 */
static int gic_dist_supports_lpis(void)
{
	return (IS_ENABLED(CONFIG_ARM_GIC_V3_ITS) &&
		!!(readl_relaxed(gic_data.dist_base + GICD_TYPER) & GICD_TYPER_LPIS) &&
		!gicv3_nolpi);
}

/*
 * IAMROOT, 2022.10.08:
 * - redist, group0/1등에 대한 설정.
 */
static void gic_cpu_init(void)
{
	void __iomem *rbase;
	int i;

	/* Register ourselves with the rest of the world */
/*
 * IAMROOT, 2022.10.08:
 * - 현재 cpu에 대한 rdist설정이 성공했으면 다음으로 넘어간다.
 */
	if (gic_populate_rdist())
		return;

	gic_enable_redist(true);

/*
 * IAMROOT, 2022.10.08:
 * - ppi 16개 초과한다는건 Extend range를 지원하고있다는 의미다.
 *   그런데 정작 register가 extend range를 지원안하고있으면 말이 안되므로 warning.
 */
	WARN((gic_data.ppi_nr > 16 || GIC_ESPI_NR != 0) &&
	     !(gic_read_ctlr() & ICC_CTLR_EL1_ExtRange),
	     "Distributor has extended ranges, but CPU%d doesn't\n",
	     smp_processor_id());

	rbase = gic_data_rdist_sgi_base();

	/* Configure SGIs/PPIs as non-secure Group-1 */
/*
 * IAMROOT, 2022.10.08:
 * - non-secure group1로 전부 설정한다.
 * - 16은 sgi 개수. GICR_TYPER_NR_PPIS 참고
 */
	for (i = 0; i < gic_data.ppi_nr + 16; i += 32)
		writel_relaxed(~0, rbase + GICR_IGROUPR0 + i / 8);

	gic_cpu_config(rbase, gic_data.ppi_nr + 16, gic_redist_wait_for_rwp);

	/* initialise system registers */
	gic_cpu_sys_reg_init();
}

#ifdef CONFIG_SMP

#define MPIDR_TO_SGI_RS(mpidr)	(MPIDR_RS(mpidr) << ICC_SGI1R_RS_SHIFT)
#define MPIDR_TO_SGI_CLUSTER_ID(mpidr)	((mpidr) & ~0xFUL)

/*
 * IAMROOT, 2022.10.15:
 * - secondary cpu들의 gic초기화.
 */
static int gic_starting_cpu(unsigned int cpu)
{
	gic_cpu_init();

	if (gic_dist_supports_lpis())
		its_cpu_init();

	return 0;
}

static u16 gic_compute_target_list(int *base_cpu, const struct cpumask *mask,
				   unsigned long cluster_id)
{
	int next_cpu, cpu = *base_cpu;
	unsigned long mpidr = cpu_logical_map(cpu);
	u16 tlist = 0;

	while (cpu < nr_cpu_ids) {
		tlist |= 1 << (mpidr & 0xf);

		next_cpu = cpumask_next(cpu, mask);
		if (next_cpu >= nr_cpu_ids)
			goto out;
		cpu = next_cpu;

		mpidr = cpu_logical_map(cpu);

		if (cluster_id != MPIDR_TO_SGI_CLUSTER_ID(mpidr)) {
			cpu--;
			goto out;
		}
	}
out:
	*base_cpu = cpu;
	return tlist;
}

#define MPIDR_TO_SGI_AFFINITY(cluster_id, level) \
	(MPIDR_AFFINITY_LEVEL(cluster_id, level) \
		<< ICC_SGI1R_AFFINITY_## level ##_SHIFT)

static void gic_send_sgi(u64 cluster_id, u16 tlist, unsigned int irq)
{
	u64 val;

	val = (MPIDR_TO_SGI_AFFINITY(cluster_id, 3)	|
	       MPIDR_TO_SGI_AFFINITY(cluster_id, 2)	|
	       irq << ICC_SGI1R_SGI_ID_SHIFT		|
	       MPIDR_TO_SGI_AFFINITY(cluster_id, 1)	|
	       MPIDR_TO_SGI_RS(cluster_id)		|
	       tlist << ICC_SGI1R_TARGET_LIST_SHIFT);

	pr_devel("CPU%d: ICC_SGI1R_EL1 %llx\n", smp_processor_id(), val);
	gic_write_sgi1r(val);
}

static void gic_ipi_send_mask(struct irq_data *d, const struct cpumask *mask)
{
	int cpu;

	if (WARN_ON(d->hwirq >= 16))
		return;

	/*
	 * Ensure that stores to Normal memory are visible to the
	 * other CPUs before issuing the IPI.
	 */
	wmb();

	for_each_cpu(cpu, mask) {
		u64 cluster_id = MPIDR_TO_SGI_CLUSTER_ID(cpu_logical_map(cpu));
		u16 tlist;

		tlist = gic_compute_target_list(&cpu, mask, cluster_id);
		gic_send_sgi(cluster_id, tlist, d->hwirq);
	}

	/* Force the above writes to ICC_SGI1R_EL1 to be executed */
	isb();
}

/*
 * IAMROOT, 2022.10.15:
 * - sgi irq 설정.
 */
static void __init gic_smp_init(void)
{
	struct irq_fwspec sgi_fwspec = {
		.fwnode		= gic_data.fwnode,
		.param_count	= 1,
	};
	int base_sgi;

	cpuhp_setup_state_nocalls(CPUHP_AP_IRQ_GIC_STARTING,
				  "irqchip/arm/gicv3:starting",
				  gic_starting_cpu, NULL);

	/* Register all 8 non-secure SGIs */
/*
 * IAMROOT, 2022.10.15:
 * - 8개만큼(non-secure SGI개수)의 virq를 할당한다.
 */
	base_sgi = __irq_domain_alloc_irqs(gic_data.domain, -1, 8,
					   NUMA_NO_NODE, &sgi_fwspec,
					   false, NULL);
	if (WARN_ON(base_sgi <= 0))
		return;

/*
 * IAMROOT, 2022.10.15:
 * - 위에서 구해온 start sgi virq번호로 set.
 */
	set_smp_ipi_range(base_sgi, 8);
}

/*
 * IAMROOT, 2022.10.01:
 * - @d에 해당하는 gic register를 가져와 @mask_val의 cpu중 하나를 골라서
 *   mpidr을 가져와 gic register에 기록한다.
 */
static int gic_set_affinity(struct irq_data *d, const struct cpumask *mask_val,
			    bool force)
{
	unsigned int cpu;
	u32 offset, index;
	void __iomem *reg;
	int enabled;
	u64 val;

/*
 * IAMROOT, 2022.10.01:
 * - mask_val중에 한개 선택한다.
 */
	if (force)
		cpu = cpumask_first(mask_val);
	else
		cpu = cpumask_any_and(mask_val, cpu_online_mask);

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

/*
 * IAMROOT, 2022.10.01:
 * - spi류들만 가능하다. per cpu류들은 안된다.
 */
	if (gic_irq_in_rdist(d))
		return -EINVAL;

	/* If interrupt was enabled, disable it first */
	enabled = gic_peek_irq(d, GICD_ISENABLER);

/*
 * IAMROOT, 2022.10.01:
 * - enabled되있있으면 잠깐 mask해놓는다.
 */
	if (enabled)
		gic_mask_irq(d);

	offset = convert_offset_index(d, GICD_IROUTER, &index);
	reg = gic_dist_base(d) + offset + (index * 8);
	val = gic_mpidr_to_affinity(cpu_logical_map(cpu));

/*
 * IAMROOT, 2022.10.01:
 * - 해당 irq에 대한 gic register를 가져와서 cpu mpdir을 write한다
 * - *reg = val
 */
	gic_write_irouter(val, reg);

	/*
	 * If the interrupt was enabled, enabled it again. Otherwise,
	 * just wait for the distributor to have digested our changes.
	 */
/*
 * IAMROOT, 2022.10.01:
 * - mask해놨으면 다시 unmask한다. 그게 아니면 wait rwp
 */
	if (enabled)
		gic_unmask_irq(d);
	else
		gic_dist_wait_for_rwp();

	irq_data_update_effective_affinity(d, cpumask_of(cpu));

	return IRQ_SET_MASK_OK_DONE;
}
#else
#define gic_set_affinity	NULL
#define gic_ipi_send_mask	NULL
#define gic_smp_init()		do { } while(0)
#endif

/*
 * IAMROOT, 2022.10.08:
 * @return 1. success. 0. fail.
 */
static int gic_retrigger(struct irq_data *data)
{
	return !gic_irq_set_irqchip_state(data, IRQCHIP_STATE_PENDING, true);
}

#ifdef CONFIG_CPU_PM
/*
 * IAMROOT, 2022.10.29:
 * - 절전 해제, 진입, security 상태에 따라 gic 설정.
 */
static int gic_cpu_pm_notifier(struct notifier_block *self,
			       unsigned long cmd, void *v)
{
/*
 * IAMROOT, 2022.10.29:
 * - CPU_PM_EXIT -> 절전 해제
 * - CPU_PM_ENTER -> 절전
 */
	if (cmd == CPU_PM_EXIT) {
/*
 * IAMROOT, 2022.10.29:
 * - single이면 kernel이 제어.
 */
		if (gic_dist_security_disabled())
			gic_enable_redist(true);
		gic_cpu_sys_reg_init();
	} else if (cmd == CPU_PM_ENTER && gic_dist_security_disabled()) {
		gic_write_grpen1(0);
		gic_enable_redist(false);
	}
	return NOTIFY_OK;
}

static struct notifier_block gic_cpu_pm_notifier_block = {
	.notifier_call = gic_cpu_pm_notifier,
};

/*
 * IAMROOT, 2022.10.29:
 * - gic에 대한 pm을 등록한다.
 */
static void gic_cpu_pm_init(void)
{
	cpu_pm_register_notifier(&gic_cpu_pm_notifier_block);
}

#else
static inline void gic_cpu_pm_init(void) { }
#endif /* CONFIG_CPU_PM */

/*
 * IAMROOT, 2022.10.29:
 * - guest os or hyp가 없을때.
 */
static struct irq_chip gic_chip = {
	.name			= "GICv3",
	.irq_mask		= gic_mask_irq,
	.irq_unmask		= gic_unmask_irq,
	.irq_eoi		= gic_eoi_irq,
	.irq_set_type		= gic_set_type,
	.irq_set_affinity	= gic_set_affinity,
	.irq_retrigger          = gic_retrigger,
	.irq_get_irqchip_state	= gic_irq_get_irqchip_state,
	.irq_set_irqchip_state	= gic_irq_set_irqchip_state,
	.irq_nmi_setup		= gic_irq_nmi_setup,
	.irq_nmi_teardown	= gic_irq_nmi_teardown,
	.ipi_send_mask		= gic_ipi_send_mask,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE |
				  IRQCHIP_MASK_ON_SUSPEND,
};

/*
 * IAMROOT, 2022.10.29:
 * - eoimode1 (hypmode 운영)
 */
static struct irq_chip gic_eoimode1_chip = {
	.name			= "GICv3",
	.irq_mask		= gic_eoimode1_mask_irq,
	.irq_unmask		= gic_unmask_irq,
	.irq_eoi		= gic_eoimode1_eoi_irq,
	.irq_set_type		= gic_set_type,
	.irq_set_affinity	= gic_set_affinity,
	.irq_retrigger          = gic_retrigger,
	.irq_get_irqchip_state	= gic_irq_get_irqchip_state,
	.irq_set_irqchip_state	= gic_irq_set_irqchip_state,
	.irq_set_vcpu_affinity	= gic_irq_set_vcpu_affinity,
	.irq_nmi_setup		= gic_irq_nmi_setup,
	.irq_nmi_teardown	= gic_irq_nmi_teardown,
	.ipi_send_mask		= gic_ipi_send_mask,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE |
				  IRQCHIP_MASK_ON_SUSPEND,
};

/*
 * IAMROOT, 2022.10.08:
 * - @virq로 @hw를 등록한다. 
 *   1. chip ops 선택
 *   2. @hw irq type에따라 flow handler를 설정.
 */
static int gic_irq_domain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hw)
{
	struct irq_chip *chip = &gic_chip;
	struct irq_data *irqd = irq_desc_get_irq_data(irq_to_desc(irq));

/*
 * IAMROOT, 2022.10.01:
 * - supports_deactivate_key를 지원하면 eoimode1로 한다.
 */
	if (static_branch_likely(&supports_deactivate_key))
		chip = &gic_eoimode1_chip;

	switch (__get_intid_range(hw)) {
	case SGI_RANGE:
	case PPI_RANGE:
	case EPPI_RANGE:
/*
 * IAMROOT, 2022.10.01:
 * - 위 irq들은 cpu별로 하는것이므로 per cpu로 설정해줘야한다. 
 * - flow handler(handle_percpu_devid_irq())를 per cpu방식으로 설정한다.
 */
		irq_set_percpu_devid(irq);
		irq_domain_set_info(d, irq, hw, chip, d->host_data,
				    handle_percpu_devid_irq, NULL, NULL);
		break;

	case SPI_RANGE:
	case ESPI_RANGE:
		irq_domain_set_info(d, irq, hw, chip, d->host_data,
				    handle_fasteoi_irq, NULL, NULL);
/*
 * IAMROOT, 2022.10.08:
 * - driver용 irq개념이라 probe set.
 */
		irq_set_probe(irq);
		irqd_set_single_target(irqd);
		break;

	case LPI_RANGE:
		if (!gic_dist_supports_lpis())
			return -EPERM;
		irq_domain_set_info(d, irq, hw, chip, d->host_data,
				    handle_fasteoi_irq, NULL, NULL);
		break;

	default:
		return -EPERM;
	}

	/* Prevents SW retriggers which mess up the ACK/EOI ordering */
/*
 * IAMROOT, 2022.10.08:
 * - gic는 hw를 지원하기때문에 sw방식은 disable한다.
 * - irq_sw_resend() 참고
 */
	irqd_set_handle_enforce_irqctx(irqd);
	return 0;
}

/*
 * IAMROOT, 2022.10.01:
 * - @fwspec 정보로 @d를 통해서 @hwirq, @type을 알아온다.
 */
static int gic_irq_domain_translate(struct irq_domain *d,
				    struct irq_fwspec *fwspec,
				    unsigned long *hwirq,
				    unsigned int *type)
{
/*
 * IAMROOT, 2022.10.01:
 * - 1개의 인자 + 16미만의 번호. coner case로 변환 없이 그대로 반환한다.
 */
	if (fwspec->param_count == 1 && fwspec->param[0] < 16) {
		*hwirq = fwspec->param[0];
		*type = IRQ_TYPE_EDGE_RISING;
		return 0;
	}

/*
 * IAMROOT, 2022.10.01:
 * - __get_intid_range() 참고
 * - dt에서 왓으면 인자는 3개정도 와야된다.
 *   param[0]                param[1] (irq nr)
 *   0 => spi라는 의미.      spi. +32
 *   1 => ppi                ppi. +16
 *   2 => ESPI               ESPI. +ESPI_BASE_INTID
 *   3 => EPPI               EPPI. +EPPI_BASE_INTID
 *   GIC_IRQ_TYPE_LPI        LPI. 그대로 사용.
 *   GIC_IRQ_TYPE_PARTITION  PMU처리를 위해 한개의 계층을 또 만드는 개념.
 *                           16이상이면 +EPPI_BASE_INTID - 16
 *                           16미만이면 +16
 *
 *   param[2] = irq line trigger 속성.
 * - SGI는 여기에 진입할 일이 없다.
 */
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count < 3)
			return -EINVAL;

		switch (fwspec->param[0]) {
		case 0:			/* SPI */
			*hwirq = fwspec->param[1] + 32;
			break;
		case 1:			/* PPI */
			*hwirq = fwspec->param[1] + 16;
			break;
		case 2:			/* ESPI */
			*hwirq = fwspec->param[1] + ESPI_BASE_INTID;
			break;
		case 3:			/* EPPI */
			*hwirq = fwspec->param[1] + EPPI_BASE_INTID;
			break;
		case GIC_IRQ_TYPE_LPI:	/* LPI */
			*hwirq = fwspec->param[1];
			break;
/*
 * IAMROOT, 2022.10.29:
 * - gic_populate_ppi_partitions() 참고
 */
		case GIC_IRQ_TYPE_PARTITION:
			*hwirq = fwspec->param[1];
			if (fwspec->param[1] >= 16)
				*hwirq += EPPI_BASE_INTID - 16;
			else
				*hwirq += 16;
			break;
		default:
			return -EINVAL;
		}

		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;

		/*
		 * Make it clear that broken DTs are... broken.
		 * Partitioned PPIs are an unfortunate exception.
		 */
		WARN_ON(*type == IRQ_TYPE_NONE &&
			fwspec->param[0] != GIC_IRQ_TYPE_PARTITION);
		return 0;
	}


/*
 * IAMROOT, 2022.10.01:
 * - fwnode는 param이 2개여야한다.
 *   0번은 hwirq, 1번은 type 고정
 */
	if (is_fwnode_irqchip(fwspec->fwnode)) {
		if(fwspec->param_count != 2)
			return -EINVAL;

		*hwirq = fwspec->param[0];
		*type = fwspec->param[1];

		WARN_ON(*type == IRQ_TYPE_NONE);
		return 0;
	}

	return -EINVAL;
}

/*
 * IAMROOT, 2022.10.01:
 * - @virq 부터 @nr_irqs개 까지 hwirq를 찾아 @domain과 함께 mapping한다.
 */
static int gic_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	int i, ret;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;

/*
 * IAMROOT, 2022.10.01:
 * - base가 되는 hwirq를 알아온다.
 */
	ret = gic_irq_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

/*
 * IAMROOT, 2022.10.01:
 * - @virq와 hwirq로 nr_irqs만큼 mapping을 해준다.
 */
	for (i = 0; i < nr_irqs; i++) {
		ret = gic_irq_domain_map(domain, virq + i, hwirq + i);
		if (ret)
			return ret;
	}

	return 0;
}

static void gic_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);
		irq_set_handler(virq + i, NULL);
		irq_domain_reset_irq_data(d);
	}
}

/*
 * IAMROOT, 2022.10.29:
 * - param, range등의 값 검사.
 * - 통과 조건.
 *   1. ppi_desc가 존재
 *   2. fwnode가 of_node 측이여야된다.
 *   3. param이 4개이상이고 param[3]이 NULL이 아니여야 된다.
 *   4. range가 ppi류여야 된다.
 *
 * ex) dts(rk3399.dtsi)
 * pmu_a53 {
 *		compatible = "arm,cortex-a53-pmu";
 *		interrupts = <GIC_PPI 7 IRQ_TYPE_LEVEL_LOW &ppi_cluster0>;
 * }
 * 마지막엔 반드시 연결된 cluster phandle이 등록되있다.
 */
static bool fwspec_is_partitioned_ppi(struct irq_fwspec *fwspec,
				      irq_hw_number_t hwirq)
{
	enum gic_intid_range range;

	if (!gic_data.ppi_descs)
		return false;

	if (!is_of_node(fwspec->fwnode))
		return false;

	if (fwspec->param_count < 4 || !fwspec->param[3])
		return false;

	range = __get_intid_range(hwirq);
	if (range != PPI_RANGE && range != EPPI_RANGE)
		return false;

	return true;
}

/*
 * IAMROOT, 2022.10.29:
 * @return 1 success. 0 false.
 *
 * - @fwspec와 @d가 매칭이 되는지 검사한다.
 * - @d, @fwspec으로 hwirq, type을 구하고, @d가 해당 hwriq의
 *   ppi parition domain인지 검사한다.
 */
static int gic_irq_domain_select(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 enum irq_domain_bus_token bus_token)
{
	unsigned int type, ret, ppi_idx;
	irq_hw_number_t hwirq;

	/* Not for us */
        if (fwspec->fwnode != d->fwnode)
		return 0;

	/* If this is not DT, then we have a single domain */
	if (!is_of_node(fwspec->fwnode))
		return 1;

	ret = gic_irq_domain_translate(d, fwspec, &hwirq, &type);
	if (WARN_ON_ONCE(ret))
		return 0;

/*
 * IAMROOT, 2022.10.29:
 * - domain이 gic_data.domain이면 true로 인정한다.
 */
	if (!fwspec_is_partitioned_ppi(fwspec, hwirq))
		return d == gic_data.domain;

	/*
	 * If this is a PPI and we have a 4th (non-null) parameter,
	 * then we need to match the partition domain.
	 */
/*
 * IAMROOT, 2022.10.29:
 * - 위에서 true로 왔을경우 hwirq->ppi->idx->partition domain으로 얻어온다.
 */
	ppi_idx = __gic_get_ppi_index(hwirq);
	return d == partition_get_domain(gic_data.ppi_descs[ppi_idx]);
}

/*
 * IAMROOT, 2022.10.01:
 * - tree
 */
static const struct irq_domain_ops gic_irq_domain_ops = {
	.translate = gic_irq_domain_translate,
	.alloc = gic_irq_domain_alloc,
	.free = gic_irq_domain_free,
	.select = gic_irq_domain_select,
};

/*
 * IAMROOT, 2022.10.29:
 * - hwirq에 partition id를 얻어온다. type은 IRQ_NONE
 */
static int partition_domain_translate(struct irq_domain *d,
				      struct irq_fwspec *fwspec,
				      unsigned long *hwirq,
				      unsigned int *type)
{
	unsigned long ppi_intid;
	struct device_node *np;
	unsigned int ppi_idx;
	int ret;

	if (!gic_data.ppi_descs)
		return -ENOMEM;

/*
 * IAMROOT, 2022.10.29:
 * - cluster phandle.
 */
	np = of_find_node_by_phandle(fwspec->param[3]);
	if (WARN_ON(!np))
		return -EINVAL;

/*
 * IAMROOT, 2022.10.29:
 * - hwirq(ppi_intid), type구해오고
 */
	ret = gic_irq_domain_translate(d, fwspec, &ppi_intid, type);
	if (WARN_ON_ONCE(ret))
		return 0;

/*
 * IAMROOT, 2022.10.29:
 * - ppi_descs에서 partition id를 얻어온다.
 */
	ppi_idx = __gic_get_ppi_index(ppi_intid);
	ret = partition_translate_id(gic_data.ppi_descs[ppi_idx],
				     of_node_to_fwnode(np));
	if (ret < 0)
		return ret;

	*hwirq = ret;
	*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

/*
 * IAMROOT, 2022.10.29:
 * - ppi의 경우 parititon을 만들시(partition_create_desc()) alloc, free ops에
 *   partition_domain_alloc, partition_domain_free가 추가된다.
 */
static const struct irq_domain_ops partition_domain_ops = {
	.translate = partition_domain_translate,
	.select = gic_irq_domain_select,
};

static bool gic_enable_quirk_msm8996(void *data)
{
	struct gic_chip_data *d = data;

	d->flags |= FLAGS_WORKAROUND_GICR_WAKER_MSM8996;

	return true;
}

static bool gic_enable_quirk_cavium_38539(void *data)
{
	struct gic_chip_data *d = data;

	d->flags |= FLAGS_WORKAROUND_CAVIUM_ERRATUM_38539;

	return true;
}

static bool gic_enable_quirk_hip06_07(void *data)
{
	struct gic_chip_data *d = data;

	/*
	 * HIP06 GICD_IIDR clashes with GIC-600 product number (despite
	 * not being an actual ARM implementation). The saving grace is
	 * that GIC-600 doesn't have ESPI, so nothing to do in that case.
	 * HIP07 doesn't even have a proper IIDR, and still pretends to
	 * have ESPI. In both cases, put them right.
	 */
	if (d->rdists.gicd_typer & GICD_TYPER_ESPI) {
		/* Zero both ESPI and the RES0 field next to it... */
		d->rdists.gicd_typer &= ~GENMASK(9, 8);
		return true;
	}

	return false;
}

static const struct gic_quirk gic_quirks[] = {
	{
		.desc	= "GICv3: Qualcomm MSM8996 broken firmware",
		.compatible = "qcom,msm8996-gic-v3",
		.init	= gic_enable_quirk_msm8996,
	},
	{
		.desc	= "GICv3: HIP06 erratum 161010803",
		.iidr	= 0x0204043b,
		.mask	= 0xffffffff,
		.init	= gic_enable_quirk_hip06_07,
	},
	{
		.desc	= "GICv3: HIP07 erratum 161010803",
		.iidr	= 0x00000000,
		.mask	= 0xffffffff,
		.init	= gic_enable_quirk_hip06_07,
	},
	{
		/*
		 * Reserved register accesses generate a Synchronous
		 * External Abort. This erratum applies to:
		 * - ThunderX: CN88xx
		 * - OCTEON TX: CN83xx, CN81xx
		 * - OCTEON TX2: CN93xx, CN96xx, CN98xx, CNF95xx*
		 */
		.desc	= "GICv3: Cavium erratum 38539",
		.iidr	= 0xa000034c,
		.mask	= 0xe8f00fff,
		.init	= gic_enable_quirk_cavium_38539,
	},
	{
	}
};

/*
 * IAMROOT, 2022.10.29:
 * - ppi_nmi_refs를 생성하고 nmi를 enable한다.
 */
static void gic_enable_nmi_support(void)
{
	int i;

	if (!gic_prio_masking_enabled())
		return;

	ppi_nmi_refs = kcalloc(gic_data.ppi_nr, sizeof(*ppi_nmi_refs), GFP_KERNEL);
	if (!ppi_nmi_refs)
		return;

	for (i = 0; i < gic_data.ppi_nr; i++)
		refcount_set(&ppi_nmi_refs[i], 0);

	/*
	 * Linux itself doesn't use 1:N distribution, so has no need to
	 * set PMHE. The only reason to have it set is if EL3 requires it
	 * (and we can't change it).
	 */
/*
 * IAMROOT, 2022.10.29:
 * - PMHE
 *   Priority Mask Hint Enable. Controls whether the priority mask register is used
 *   as a hint for interrupt distribution:
 * - PMHE is set -> enable pmr -> sync(dsb(sy))가 필요하다는 뜻.
 *   pmr_sync()참고
 */
	if (gic_read_ctlr() & ICC_CTLR_EL1_PMHE_MASK)
		static_branch_enable(&gic_pmr_sync);

	pr_info("Pseudo-NMIs enabled using %s ICC_PMR_EL1 synchronisation\n",
		static_branch_unlikely(&gic_pmr_sync) ? "forced" : "relaxed");

	/*
	 * How priority values are used by the GIC depends on two things:
	 * the security state of the GIC (controlled by the GICD_CTRL.DS bit)
	 * and if Group 0 interrupts can be delivered to Linux in the non-secure
	 * world as FIQs (controlled by the SCR_EL3.FIQ bit). These affect the
	 * the ICC_PMR_EL1 register and the priority that software assigns to
	 * interrupts:
	 *
	 * GICD_CTRL.DS | SCR_EL3.FIQ | ICC_PMR_EL1 | Group 1 priority
	 * -----------------------------------------------------------
	 *      1       |      -      |  unchanged  |    unchanged
	 * -----------------------------------------------------------
	 *      0       |      1      |  non-secure |    non-secure
	 * -----------------------------------------------------------
	 *      0       |      0      |  unchanged  |    non-secure
	 *
	 * where non-secure means that the value is right-shifted by one and the
	 * MSB bit set, to make it fit in the non-secure priority range.
	 *
	 * In the first two cases, where ICC_PMR_EL1 and the interrupt priority
	 * are both either modified or unchanged, we can use the same set of
	 * priorities.
	 *
	 * In the last case, where only the interrupt priorities are modified to
	 * be in the non-secure range, we use a different PMR value to mask IRQs
	 * and the rest of the values that we use remain unchanged.
	 */
/*
 * IAMROOT, 2022.10.29:
 * - papago
 *   GIC에서 우선 순위 값을 사용하는 방법은 다음 두 가지 사항에 따라 다릅니다.
 *   GIC의 보안 상태(GICD_CTRL.DS 비트에 의해 제어됨) 및 그룹 0 인터럽트가
 *   FIQ(SCR_EL3.FIQ 비트에 의해 제어됨)로 비보안 세계에서 Linux에 전달될 수 있는 경우.
 *   이는 ICC_PMR_EL1 레지스터와 소프트웨어가 인터럽트에 할당하는 우선 순위에 영향을
 *   줍니다.
 *
 *                  el3에서 FIQ를
 *                  받는지의 여부
 *   GICD_CTRL.DS | SCR_EL3.FIQ | ICC_PMR_EL1 | Group 1 priority
 *   -----------------------------------------------------------
 *        1       |      -      |  unchanged  |    unchanged     <- 모든 irq는 kernel이 제어.
 *   -----------------------------------------------------------
 *        0       |      1      |  non-secure |    non-secure    <- fiq는 모두 el3로.
 *   -----------------------------------------------------------
 *        0       |      0      |  unchanged  |    non-secure
 *
 *  여기서 비보안은 값이 1만큼 오른쪽으로 이동하고 MSB 비트가 설정되어 비보안 우선 순위
 *  범위에 맞도록 하는 것을 의미합니다.
 *
 *  ICC_PMR_EL1과 인터럽트 우선 순위가 모두 수정되거나 변경되지 않은 처음 두 경우에는
 *  동일한 우선 순위 집합을 사용할 수 있습니다. 
 *
 *  인터럽트 우선 순위만 비보안 범위로 수정되는 마지막 경우에는 다른 PMR 값을 사용하여
 *  IRQ를 마스킹하고 나머지 값은 변경되지 않은 상태로 유지합니다.
 *
 *  - ICC_PMR_EL1 -> 
 *
 * - (gic_has_group0() && !gic_dist_security_disabled())
 *   kernel이 group0 제어권이 있고. two security로 운영중이다.
 *   kernel이 priorities에 대해서 반절만 제어를 하겠다는것.
 *
 *   group0     secure
 *   true       single
 *   true       two     <-- 이경우. two secure지만 fiq를 kernel이 제어할수있음.
 *   false      two
 *
 *   (SCR_EL3.FIQ == 0 -> EL3가 fiq를 안받는다느것 -> kernel이 받는것 ->
 *   group0는 kernel제어)
 */
	if (gic_has_group0() && !gic_dist_security_disabled())
		static_branch_enable(&gic_nonsecure_priorities);

	static_branch_enable(&supports_pseudo_nmis);

/*
 * IAMROOT, 2022.10.29:
 * - eoimode1동작이면 eoimo1에 IRQCHIP_SUPPORTS_NMI를 설정. 아닌 경우 eoimode0에 설정한다.
 */
	if (static_branch_likely(&supports_deactivate_key))
		gic_eoimode1_chip.flags |= IRQCHIP_SUPPORTS_NMI;
	else
		gic_chip.flags |= IRQCHIP_SUPPORTS_NMI;
}

/*
 * IAMROOT, 2022.10.01:
 * - git init.
 */
static int __init gic_init_bases(void __iomem *dist_base,
				 struct redist_region *rdist_regs,
				 u32 nr_redist_regions,
				 u64 redist_stride,
				 struct fwnode_handle *handle)
{
	u32 typer;
	int err;

/*
 * IAMROOT, 2022.10.29:
 * - hyp가 아니면 딱히 eoimode1을 쓸필요없다. disable
 */
	if (!is_hyp_mode_available())
		static_branch_disable(&supports_deactivate_key);

	if (static_branch_likely(&supports_deactivate_key))
		pr_info("GIC: Using split EOI/Deactivate mode\n");

	gic_data.fwnode = handle;
	gic_data.dist_base = dist_base;
	gic_data.redist_regions = rdist_regs;
	gic_data.nr_redist_regions = nr_redist_regions;
	gic_data.redist_stride = redist_stride;

	/*
	 * Find out how many interrupts are supported.
	 */
	typer = readl_relaxed(gic_data.dist_base + GICD_TYPER);
	gic_data.rdists.gicd_typer = typer;

	gic_enable_quirks(readl_relaxed(gic_data.dist_base + GICD_IIDR),
			  gic_quirks, &gic_data);


/*
 * IAMROOT, 2022.10.01:
 * - SPIs(Shared Peripherals Interrupts)
 *   core에 공용으로 사용될 수 있는 IRQ 들로 INTID32 ~ 1019까지 사용한다.
 *   GIC에서 사용하는 클럭과 연동된다
 */
	pr_info("%d SPIs implemented\n", GIC_LINE_NR - 32);
	pr_info("%d Extended SPIs implemented\n", GIC_ESPI_NR);

	/*
	 * ThunderX1 explodes on reading GICD_TYPER2, in violation of the
	 * architecture spec (which says that reserved registers are RES0).
	 */
	if (!(gic_data.flags & FLAGS_WORKAROUND_CAVIUM_ERRATUM_38539))
		gic_data.rdists.gicd_typer2 = readl_relaxed(gic_data.dist_base + GICD_TYPER2);

	gic_data.domain = irq_domain_create_tree(handle, &gic_irq_domain_ops,
						 &gic_data);
	gic_data.rdists.rdist = alloc_percpu(typeof(*gic_data.rdists.rdist));
	gic_data.rdists.has_rvpeid = true;
	gic_data.rdists.has_vlpis = true;
	gic_data.rdists.has_direct_lpi = true;
	gic_data.rdists.has_vpend_valid_dirty = true;

	if (WARN_ON(!gic_data.domain) || WARN_ON(!gic_data.rdists.rdist)) {
		err = -ENOMEM;
		goto out_free;
	}

	irq_domain_update_bus_token(gic_data.domain, DOMAIN_BUS_WIRED);

	gic_data.has_rss = !!(typer & GICD_TYPER_RSS);
	pr_info("Distributor has %sRange Selector support\n",
		gic_data.has_rss ? "" : "no ");

/*
 * IAMROOT, 2022.10.08:
 * - MBIS를 지원하면.
 */
	if (typer & GICD_TYPER_MBIS) {
		err = mbi_init(handle, gic_data.domain);
		if (err)
			pr_err("Failed to initialize MBIs\n");
	}

/*
 * IAMROOT, 2022.10.08:
 * - gic v3 interrupt handler를 등록한다.
 */
	set_handle_irq(gic_handle_irq);

	gic_update_rdist_properties();

	gic_dist_init();
	gic_cpu_init();
	gic_smp_init();
	gic_cpu_pm_init();

/*
 * IAMROOT, 2022.10.29:
 * - SKIP
 */
	if (gic_dist_supports_lpis()) {
		its_init(handle, &gic_data.rdists, gic_data.domain);
		its_cpu_init();
	} else {
		if (IS_ENABLED(CONFIG_ARM_GIC_V2M))
			gicv2m_init(handle, gic_data.domain);
	}

/*
 * IAMROOT, 2022.10.29:
 * - enable nmi
 */
	gic_enable_nmi_support();

	return 0;

out_free:
	if (gic_data.domain)
		irq_domain_remove(gic_data.domain);
	free_percpu(gic_data.rdists.rdist);
	return err;
}

/*
 * IAMROOT, 2022.10.01:
 * - GICD_PIDR2 register의 GIC_PIDR2_ARCH를 읽어서 v3 or v4인지 확인한다.
 */
static int __init gic_validate_dist_version(void __iomem *dist_base)
{
	u32 reg = readl_relaxed(dist_base + GICD_PIDR2) & GIC_PIDR2_ARCH_MASK;

	if (reg != GIC_PIDR2_ARCH_GICv3 && reg != GIC_PIDR2_ARCH_GICv4)
		return -ENODEV;

	return 0;
}

/* Create all possible partitions at boot time */
/*
 * IAMROOT, 2022.10.29:
 * - ex) dts
 *
 *   ppi-partitions {
 *		ppi_cluster0: interrupt-partition-0 {
 *			affinity = <&cpu_l0 &cpu_l1 &cpu_l2 &cpu_l3>;
 *		};
 *		ppi_cluster1: interrupt-partition-1 {
 *			affinity = <&cpu_b0 &cpu_b1>;
 *		};
 *	 };
 *
 *	 이 예제로, nr_parts는 2개가 된다(ppi_cluster0, ppi_cluster1)
 */
static void __init gic_populate_ppi_partitions(struct device_node *gic_node)
{
	struct device_node *parts_node, *child_part;
	int part_idx = 0, i;
	int nr_parts;
	struct partition_affinity *parts;

	parts_node = of_get_child_by_name(gic_node, "ppi-partitions");
	if (!parts_node)
		return;

/*
 * IAMROOT, 2022.10.29:
 * - ppi_nr은 이전에 이미 설정되었을것이다. 해당 값을 기준으로 만든다.
 */
	gic_data.ppi_descs = kcalloc(gic_data.ppi_nr, sizeof(*gic_data.ppi_descs), GFP_KERNEL);
	if (!gic_data.ppi_descs)
		return;

/*
 * IAMROOT, 2022.10.29:
 * - child node가 몇개 있는지
 */
	nr_parts = of_get_child_count(parts_node);

	if (!nr_parts)
		goto out_put_node;

	parts = kcalloc(nr_parts, sizeof(*parts), GFP_KERNEL);
	if (WARN_ON(!parts))
		goto out_put_node;

	for_each_child_of_node(parts_node, child_part) {
		struct partition_affinity *part;
		int n;

		part = &parts[part_idx];

		part->partition_id = of_node_to_fwnode(child_part);

		pr_info("GIC: PPI partition %pOFn[%d] { ",
			child_part, part_idx);


/*
 * IAMROOT, 2022.10.29:
 * - ex) affinity = <&cpu_l0 &cpu_l1 &cpu_l2 &cpu_l3>;
 *   4개가 된다.
 */
		n = of_property_count_elems_of_size(child_part, "affinity",
						    sizeof(u32));
		WARN_ON(n <= 0);

		for (i = 0; i < n; i++) {
			int err, cpu;
			u32 cpu_phandle;
			struct device_node *cpu_node;

/*
 * IAMROOT, 2022.10.29:
 * - ex) affinity = <&cpu_l0 &cpu_l1 &cpu_l2 &cpu_l3>;
 *   에서 cpu_l0 의 예제
 *   cpu_l0: cpu@0 {
 *		device_type = "cpu";
 *		compatible = "arm,cortex-a53";
 *		reg = <0x0 0x0>;
 *		enable-method = "psci";
 *		capacity-dmips-mhz = <485>;
 *		clocks = <&cru ARMCLKL>;
 *		#cooling-cells = <2>; // min followed by max
 *		dynamic-power-coefficient = <100>;
 *		cpu-idle-states = <&CPU_SLEEP &CLUSTER_SLEEP>;
 *	};
 *	이걸 cpu_node로 가져온다.(cpu_phandle은 cpu_l0에 대한 node및 arg값.)
 *
 *	affinity에서 index(cpu_l0, cpu_l1, ..)로 해당 cpu_phandle을 구해온다
 *	-> 구해온 cpu_phandle로 실제 cpu_node(cpu_l0: cpu@0 {..})를 얻어온다.
 */
			err = of_property_read_u32_index(child_part, "affinity",
							 i, &cpu_phandle);
			if (WARN_ON(err))
				continue;

			cpu_node = of_find_node_by_phandle(cpu_phandle);
			if (WARN_ON(!cpu_node))
				continue;

/*
 * IAMROOT, 2022.10.29:
 * - cpu번호를 구해온다. 구해오고 해당 cpu로 part->mask에 set한다.
 */
			cpu = of_cpu_node_to_id(cpu_node);
			if (WARN_ON(cpu < 0))
				continue;

			pr_cont("%pOF[%d] ", cpu_node, cpu);

			cpumask_set_cpu(cpu, &part->mask);
		}

		pr_cont("}\n");
		part_idx++;
	}

	for (i = 0; i < gic_data.ppi_nr; i++) {
		unsigned int irq;
		struct partition_desc *desc;
		struct irq_fwspec ppi_fwspec = {
			.fwnode		= gic_data.fwnode,
			.param_count	= 3,
			.param		= {
				[0]	= GIC_IRQ_TYPE_PARTITION,
				[1]	= i,
				[2]	= IRQ_TYPE_NONE,
			},
		};

		irq = irq_create_fwspec_mapping(&ppi_fwspec);
		if (WARN_ON(!irq))
			continue;
		desc = partition_create_desc(gic_data.fwnode, parts, nr_parts,
					     irq, &partition_domain_ops);
		if (WARN_ON(!desc))
			continue;

		gic_data.ppi_descs[i] = desc;
	}

out_put_node:
	of_node_put(parts_node);
}

static void __init gic_of_setup_kvm_info(struct device_node *node)
{
	int ret;
	struct resource r;
	u32 gicv_idx;

	gic_v3_kvm_info.type = GIC_V3;

	gic_v3_kvm_info.maint_irq = irq_of_parse_and_map(node, 0);
	if (!gic_v3_kvm_info.maint_irq)
		return;

	if (of_property_read_u32(node, "#redistributor-regions",
				 &gicv_idx))
		gicv_idx = 1;

	gicv_idx += 3;	/* Also skip GICD, GICC, GICH */
	ret = of_address_to_resource(node, gicv_idx, &r);
	if (!ret)
		gic_v3_kvm_info.vcpu = r;

	gic_v3_kvm_info.has_v4 = gic_data.rdists.has_vlpis;
	gic_v3_kvm_info.has_v4_1 = gic_data.rdists.has_rvpeid;
	vgic_set_kvm_info(&gic_v3_kvm_info);
}

/*
 * IAMROOT, 2022.10.01:
 * - index 0 : dist_base register
 *   index 1 ~ nr_redist_regions + 1 : redist register 0 ~ nr_redist_regions의
 *                                     register
 * - #redistributors-regions : nr_redist_regions개수
 * - ex)
 *   gic: interrupt-controller@4d000000 {
 *		compatible = "arm,gic-v3";
 *		#interrupt-cells = <3>;
 *		#address-cells = <2>;
 *		#size-cells = <2>;
 *		ranges;
 *		interrupt-controller;
 *		#redistributor-regions = <4>;
 *		redistributor-stride = <0x0 0x40000>;
 *		reg = <0x0 0x4d000000 0x0 0x10000>,	// GICD
 *		<0x0 0x4d100000 0x0 0x400000>,	// p0 GICR node 0
 *		<0x0 0x6d100000 0x0 0x400000>,	// p0 GICR node 1
 *		<0x400 0x4d100000 0x0 0x400000>,	// p1 GICR node 2
 *		<0x400 0x6d100000 0x0 0x400000>,	// p1 GICR node 3
 *		<0x0 0xfe000000 0x0 0x10000>,	// GICC
 *		<0x0 0xfe010000 0x0 0x10000>,	// GICH
 *		<0x0 0xfe020000 0x0 0x10000>;	// GICV
 *		interrupts = <GIC_PPI 9 IRQ_TYPE_LEVEL_HIGH>;
 *		...
 */
static int __init gic_of_init(struct device_node *node, struct device_node *parent)
{
	void __iomem *dist_base;
	struct redist_region *rdist_regs;
	u64 redist_stride;
	u32 nr_redist_regions;
	int err, i;

/*
 * IAMROOT, 2022.10.01:
 * - 0 index에 대한 register의 가상 address를 dist_base로 가져온다.
 */
	dist_base = of_iomap(node, 0);
	if (!dist_base) {
		pr_err("%pOF: unable to map gic dist registers\n", node);
		return -ENXIO;
	}

	err = gic_validate_dist_version(dist_base);
	if (err) {
		pr_err("%pOF: no distributor detected, giving up\n", node);
		goto out_unmap_dist;
	}

/*
 * IAMROOT, 2022.10.01:
 * - redistributor-regions가 없으면 1. 있으면 읽어온 값으로.
 */
	if (of_property_read_u32(node, "#redistributor-regions", &nr_redist_regions))
		nr_redist_regions = 1;

/*
 * IAMROOT, 2022.10.01:
 * - 읽어온 redist_regions의 개수만큼 할당한다.
 *   (nr_redist_regions * sizeof(*rdist_regs))
 */
	rdist_regs = kcalloc(nr_redist_regions, sizeof(*rdist_regs),
			     GFP_KERNEL);
	if (!rdist_regs) {
		err = -ENOMEM;
		goto out_unmap_dist;
	}


/*
 * IAMROOT, 2022.10.01:
 * - index 1번 ~ nr_redist_regions + 1까지 읽으면서 redist register를 초기화한다.
 */
	for (i = 0; i < nr_redist_regions; i++) {
		struct resource res;
		int ret;

		ret = of_address_to_resource(node, 1 + i, &res);
		rdist_regs[i].redist_base = of_iomap(node, 1 + i);
		if (ret || !rdist_regs[i].redist_base) {
			pr_err("%pOF: couldn't map region %d\n", node, i);
			err = -ENODEV;
			goto out_unmap_rdist;
		}
		rdist_regs[i].phys_base = res.start;
	}

/*
 * IAMROOT, 2022.10.01:
 * - redistributor-stride
 * - 없으면 0로 초기화.
 *   redist_base의 간격을 64배수간격으로 조정
 * - ex) redistributor-stride = <0x0 0x40000>
 *   256k 간격으로 조정.
 */
	if (of_property_read_u64(node, "redistributor-stride", &redist_stride))
		redist_stride = 0;

	gic_enable_of_quirks(node, gic_quirks, &gic_data);

	err = gic_init_bases(dist_base, rdist_regs, nr_redist_regions,
			     redist_stride, &node->fwnode);
	if (err)
		goto out_unmap_rdist;

	gic_populate_ppi_partitions(node);

	if (static_branch_likely(&supports_deactivate_key))
		gic_of_setup_kvm_info(node);
	return 0;

out_unmap_rdist:
	for (i = 0; i < nr_redist_regions; i++)
		if (rdist_regs[i].redist_base)
			iounmap(rdist_regs[i].redist_base);
	kfree(rdist_regs);
out_unmap_dist:
	iounmap(dist_base);
	return err;
}

IRQCHIP_DECLARE(gic_v3, "arm,gic-v3", gic_of_init);

#ifdef CONFIG_ACPI
static struct
{
	void __iomem *dist_base;
	struct redist_region *redist_regs;
	u32 nr_redist_regions;
	bool single_redist;
	int enabled_rdists;
	u32 maint_irq;
	int maint_irq_mode;
	phys_addr_t vcpu_base;
} acpi_data __initdata;

static void __init
gic_acpi_register_redist(phys_addr_t phys_base, void __iomem *redist_base)
{
	static int count = 0;

	acpi_data.redist_regs[count].phys_base = phys_base;
	acpi_data.redist_regs[count].redist_base = redist_base;
	acpi_data.redist_regs[count].single_redist = acpi_data.single_redist;
	count++;
}

static int __init
gic_acpi_parse_madt_redist(union acpi_subtable_headers *header,
			   const unsigned long end)
{
	struct acpi_madt_generic_redistributor *redist =
			(struct acpi_madt_generic_redistributor *)header;
	void __iomem *redist_base;

	redist_base = ioremap(redist->base_address, redist->length);
	if (!redist_base) {
		pr_err("Couldn't map GICR region @%llx\n", redist->base_address);
		return -ENOMEM;
	}

	gic_acpi_register_redist(redist->base_address, redist_base);
	return 0;
}

static int __init
gic_acpi_parse_madt_gicc(union acpi_subtable_headers *header,
			 const unsigned long end)
{
	struct acpi_madt_generic_interrupt *gicc =
				(struct acpi_madt_generic_interrupt *)header;
	u32 reg = readl_relaxed(acpi_data.dist_base + GICD_PIDR2) & GIC_PIDR2_ARCH_MASK;
	u32 size = reg == GIC_PIDR2_ARCH_GICv4 ? SZ_64K * 4 : SZ_64K * 2;
	void __iomem *redist_base;

	/* GICC entry which has !ACPI_MADT_ENABLED is not unusable so skip */
	if (!(gicc->flags & ACPI_MADT_ENABLED))
		return 0;

	redist_base = ioremap(gicc->gicr_base_address, size);
	if (!redist_base)
		return -ENOMEM;

	gic_acpi_register_redist(gicc->gicr_base_address, redist_base);
	return 0;
}

static int __init gic_acpi_collect_gicr_base(void)
{
	acpi_tbl_entry_handler redist_parser;
	enum acpi_madt_type type;

	if (acpi_data.single_redist) {
		type = ACPI_MADT_TYPE_GENERIC_INTERRUPT;
		redist_parser = gic_acpi_parse_madt_gicc;
	} else {
		type = ACPI_MADT_TYPE_GENERIC_REDISTRIBUTOR;
		redist_parser = gic_acpi_parse_madt_redist;
	}

	/* Collect redistributor base addresses in GICR entries */
	if (acpi_table_parse_madt(type, redist_parser, 0) > 0)
		return 0;

	pr_info("No valid GICR entries exist\n");
	return -ENODEV;
}

static int __init gic_acpi_match_gicr(union acpi_subtable_headers *header,
				  const unsigned long end)
{
	/* Subtable presence means that redist exists, that's it */
	return 0;
}

static int __init gic_acpi_match_gicc(union acpi_subtable_headers *header,
				      const unsigned long end)
{
	struct acpi_madt_generic_interrupt *gicc =
				(struct acpi_madt_generic_interrupt *)header;

	/*
	 * If GICC is enabled and has valid gicr base address, then it means
	 * GICR base is presented via GICC
	 */
	if ((gicc->flags & ACPI_MADT_ENABLED) && gicc->gicr_base_address) {
		acpi_data.enabled_rdists++;
		return 0;
	}

	/*
	 * It's perfectly valid firmware can pass disabled GICC entry, driver
	 * should not treat as errors, skip the entry instead of probe fail.
	 */
	if (!(gicc->flags & ACPI_MADT_ENABLED))
		return 0;

	return -ENODEV;
}

static int __init gic_acpi_count_gicr_regions(void)
{
	int count;

	/*
	 * Count how many redistributor regions we have. It is not allowed
	 * to mix redistributor description, GICR and GICC subtables have to be
	 * mutually exclusive.
	 */
	count = acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_REDISTRIBUTOR,
				      gic_acpi_match_gicr, 0);
	if (count > 0) {
		acpi_data.single_redist = false;
		return count;
	}

	count = acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
				      gic_acpi_match_gicc, 0);
	if (count > 0) {
		acpi_data.single_redist = true;
		count = acpi_data.enabled_rdists;
	}

	return count;
}

static bool __init acpi_validate_gic_table(struct acpi_subtable_header *header,
					   struct acpi_probe_entry *ape)
{
	struct acpi_madt_generic_distributor *dist;
	int count;

	dist = (struct acpi_madt_generic_distributor *)header;
	if (dist->version != ape->driver_data)
		return false;

	/* We need to do that exercise anyway, the sooner the better */
	count = gic_acpi_count_gicr_regions();
	if (count <= 0)
		return false;

	acpi_data.nr_redist_regions = count;
	return true;
}

static int __init gic_acpi_parse_virt_madt_gicc(union acpi_subtable_headers *header,
						const unsigned long end)
{
	struct acpi_madt_generic_interrupt *gicc =
		(struct acpi_madt_generic_interrupt *)header;
	int maint_irq_mode;
	static int first_madt = true;

	/* Skip unusable CPUs */
	if (!(gicc->flags & ACPI_MADT_ENABLED))
		return 0;

	maint_irq_mode = (gicc->flags & ACPI_MADT_VGIC_IRQ_MODE) ?
		ACPI_EDGE_SENSITIVE : ACPI_LEVEL_SENSITIVE;

	if (first_madt) {
		first_madt = false;

		acpi_data.maint_irq = gicc->vgic_interrupt;
		acpi_data.maint_irq_mode = maint_irq_mode;
		acpi_data.vcpu_base = gicc->gicv_base_address;

		return 0;
	}

	/*
	 * The maintenance interrupt and GICV should be the same for every CPU
	 */
	if ((acpi_data.maint_irq != gicc->vgic_interrupt) ||
	    (acpi_data.maint_irq_mode != maint_irq_mode) ||
	    (acpi_data.vcpu_base != gicc->gicv_base_address))
		return -EINVAL;

	return 0;
}

static bool __init gic_acpi_collect_virt_info(void)
{
	int count;

	count = acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
				      gic_acpi_parse_virt_madt_gicc, 0);

	return (count > 0);
}

#define ACPI_GICV3_DIST_MEM_SIZE (SZ_64K)
#define ACPI_GICV2_VCTRL_MEM_SIZE	(SZ_4K)
#define ACPI_GICV2_VCPU_MEM_SIZE	(SZ_8K)

static void __init gic_acpi_setup_kvm_info(void)
{
	int irq;

	if (!gic_acpi_collect_virt_info()) {
		pr_warn("Unable to get hardware information used for virtualization\n");
		return;
	}

	gic_v3_kvm_info.type = GIC_V3;

	irq = acpi_register_gsi(NULL, acpi_data.maint_irq,
				acpi_data.maint_irq_mode,
				ACPI_ACTIVE_HIGH);
	if (irq <= 0)
		return;

	gic_v3_kvm_info.maint_irq = irq;

	if (acpi_data.vcpu_base) {
		struct resource *vcpu = &gic_v3_kvm_info.vcpu;

		vcpu->flags = IORESOURCE_MEM;
		vcpu->start = acpi_data.vcpu_base;
		vcpu->end = vcpu->start + ACPI_GICV2_VCPU_MEM_SIZE - 1;
	}

	gic_v3_kvm_info.has_v4 = gic_data.rdists.has_vlpis;
	gic_v3_kvm_info.has_v4_1 = gic_data.rdists.has_rvpeid;
	vgic_set_kvm_info(&gic_v3_kvm_info);
}

static int __init
gic_acpi_init(union acpi_subtable_headers *header, const unsigned long end)
{
	struct acpi_madt_generic_distributor *dist;
	struct fwnode_handle *domain_handle;
	size_t size;
	int i, err;

	/* Get distributor base address */
	dist = (struct acpi_madt_generic_distributor *)header;
	acpi_data.dist_base = ioremap(dist->base_address,
				      ACPI_GICV3_DIST_MEM_SIZE);
	if (!acpi_data.dist_base) {
		pr_err("Unable to map GICD registers\n");
		return -ENOMEM;
	}

	err = gic_validate_dist_version(acpi_data.dist_base);
	if (err) {
		pr_err("No distributor detected at @%p, giving up\n",
		       acpi_data.dist_base);
		goto out_dist_unmap;
	}

	size = sizeof(*acpi_data.redist_regs) * acpi_data.nr_redist_regions;
	acpi_data.redist_regs = kzalloc(size, GFP_KERNEL);
	if (!acpi_data.redist_regs) {
		err = -ENOMEM;
		goto out_dist_unmap;
	}

	err = gic_acpi_collect_gicr_base();
	if (err)
		goto out_redist_unmap;

	domain_handle = irq_domain_alloc_fwnode(&dist->base_address);
	if (!domain_handle) {
		err = -ENOMEM;
		goto out_redist_unmap;
	}

	err = gic_init_bases(acpi_data.dist_base, acpi_data.redist_regs,
			     acpi_data.nr_redist_regions, 0, domain_handle);
	if (err)
		goto out_fwhandle_free;

	acpi_set_irq_model(ACPI_IRQ_MODEL_GIC, domain_handle);

	if (static_branch_likely(&supports_deactivate_key))
		gic_acpi_setup_kvm_info();

	return 0;

out_fwhandle_free:
	irq_domain_free_fwnode(domain_handle);
out_redist_unmap:
	for (i = 0; i < acpi_data.nr_redist_regions; i++)
		if (acpi_data.redist_regs[i].redist_base)
			iounmap(acpi_data.redist_regs[i].redist_base);
	kfree(acpi_data.redist_regs);
out_dist_unmap:
	iounmap(acpi_data.dist_base);
	return err;
}
IRQCHIP_ACPI_DECLARE(gic_v3, ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR,
		     acpi_validate_gic_table, ACPI_MADT_GIC_VERSION_V3,
		     gic_acpi_init);
IRQCHIP_ACPI_DECLARE(gic_v4, ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR,
		     acpi_validate_gic_table, ACPI_MADT_GIC_VERSION_V4,
		     gic_acpi_init);
IRQCHIP_ACPI_DECLARE(gic_v3_or_v4, ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR,
		     acpi_validate_gic_table, ACPI_MADT_GIC_VERSION_NONE,
		     gic_acpi_init);
#endif
