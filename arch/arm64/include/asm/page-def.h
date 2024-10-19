/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/page.h
 *
 * Copyright (C) 1995-2003 Russell King
 * Copyright (C) 2017 ARM Ltd.
 */
#ifndef __ASM_PAGE_DEF_H
#define __ASM_PAGE_DEF_H

#include <linux/const.h>

/* PAGE_SHIFT determines the page size */
#define PAGE_SHIFT		CONFIG_ARM64_PAGE_SHIFT
/* IAMROOT, 2024.10.12:
 * - 4k인 경우 : 12
 */
#define PAGE_SIZE		(_AC(1, UL) << PAGE_SHIFT)

/* IAMROOT, 2021.10.13:
 * - 4k인 경우 : 0xffff_ffff_ffff_f000
 */
#define PAGE_MASK		(~(PAGE_SIZE-1))

#endif /* __ASM_PAGE_DEF_H */
