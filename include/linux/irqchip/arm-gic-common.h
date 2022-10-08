/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * include/linux/irqchip/arm-gic-common.h
 *
 * Copyright (C) 2016 ARM Limited, All Rights Reserved.
 */
#ifndef __LINUX_IRQCHIP_ARM_GIC_COMMON_H
#define __LINUX_IRQCHIP_ARM_GIC_COMMON_H

#include <linux/irqchip/arm-vgic-info.h>

/*
 * IAMROOT, 2022.10.08:
 * - default priority.
 *   일반 interrupt용 우선순위.
 */
#define GICD_INT_DEF_PRI		0xa0

/*
 * IAMROOT, 2022.10.08:
 * - 0xA0A0A0A0
 *   실제 들어가는 값은 0xD0D0D0D0
 *   non-secure에서 동작하는 kernel에서 보는 0xa0값은 기록할때 우측 shift를 하면서
 *   bit7이 하나더 들어간다. 
 *   0b1010_0000 - (shift) -> 0b0101_0000 - (bit 7 add) -> 0b1101_0000 == 0xD0
 *      
 */
#define GICD_INT_DEF_PRI_X4		((GICD_INT_DEF_PRI << 24) |\
					(GICD_INT_DEF_PRI << 16) |\
					(GICD_INT_DEF_PRI << 8) |\
					GICD_INT_DEF_PRI)

struct irq_domain;
struct fwnode_handle;
int gicv2m_init(struct fwnode_handle *parent_handle,
		struct irq_domain *parent);

#endif /* __LINUX_IRQCHIP_ARM_GIC_COMMON_H */
