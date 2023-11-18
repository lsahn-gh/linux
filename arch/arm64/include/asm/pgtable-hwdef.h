/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_PGTABLE_HWDEF_H
#define __ASM_PGTABLE_HWDEF_H

#include <asm/memory.h>

/*
 * Number of page-table levels required to address 'va_bits' wide
 * address, without section mapping. We resolve the top (va_bits - PAGE_SHIFT)
 * bits with (PAGE_SHIFT - 3) bits at each page table level. Hence:
 *
 *  levels = DIV_ROUND_UP((va_bits - PAGE_SHIFT), (PAGE_SHIFT - 3))
 *
 * where DIV_ROUND_UP(n, d) => (((n) + (d) - 1) / (d))
 *
 * We cannot include linux/kernel.h which defines DIV_ROUND_UP here
 * due to build issues. So we open code DIV_ROUND_UP here:
 *
 *	((((va_bits) - PAGE_SHIFT) + (PAGE_SHIFT - 3) - 1) / (PAGE_SHIFT - 3))
 *
 * which gets simplified as :
 */
#define ARM64_HW_PGTABLE_LEVELS(va_bits) (((va_bits) - 4) / (PAGE_SHIFT - 3))

/*
 * Size mapped by an entry at level n ( 0 <= n <= 3)
 * We map (PAGE_SHIFT - 3) at all translation levels and PAGE_SHIFT bits
 * in the final page. The maximum number of translation levels supported by
 * the architecture is 4. Hence, starting at level n, we have further
 * ((4 - n) - 1) levels of translation excluding the offset within the page.
 * So, the total number of bits mapped by an entry at level n is :
 *
 *  ((4 - n) - 1) * (PAGE_SHIFT - 3) + PAGE_SHIFT
 *
 * Rearranging it a bit we get :
 *   (4 - n) * (PAGE_SHIFT - 3) + 3
 */
/* IAMROOT, 2021.08.14:
 * - 4단계 기준으로 정리
 *
 *  PAGE_SIZE | PAGE_SHIFT | n | ARM64_HW_PGTABLE_LEVEL_SHIFT(n)
 * -----------+------------+---+--------------------------------------------
 *  4kb       | 12         | 0 | 39 (PGDIR_SHIFT)
 *            |            | 1 | 30 (PUD_SHIFT)
 *            |            | 2 | 21 (PMD_SHIFT)
 *            |            | 3 | 12 (PTE 테이블의 SHIFT)
 * -----------+------------+---+--------------------------------------------
 *  16Kb      | 14         | 0 | 47 (PGDIR_SHIFT)
 *            |            | 1 | 36 (PUD_SHIFT)
 *            |            | 2 | 25 (PMD_SHIFT)
 *            |            | 3 | 14 (PTE 테이블의 SHIFT)
 * -----------+------------+---+--------------------------------------------
 *  64kb      | 16         | 0 | 55 (PGDIR_SHIFT)
 *            |            | 1 | 42 (PUD_SHIFT)
 *            |            | 2 | 29 (PMD_SHIFT)
 *            |            | 3 | 16 (PTE 테이블의 SHIFT)
 */
#define ARM64_HW_PGTABLE_LEVEL_SHIFT(n)	((PAGE_SHIFT - 3) * (4 - (n)) + 3)

/* IAMROOT, 2021.08.14:
 * 가장 하위 페이지 PTE 테이블에 들어가는 엔트리 수
 * - 4K : 2^9  = 512개
 * - 16K: 2^11 = 2048개
 */
#define PTRS_PER_PTE		(1 << (PAGE_SHIFT - 3))

/*
 * PMD_SHIFT determines the size a level 2 page table entry can map.
 */
#if CONFIG_PGTABLE_LEVELS > 2
/* IAMROOT, 2021.08.14:
 * - 4K 기준으로 정리
 *   PMD_SHIFT   : PMD에 사용할 SHIFT는 21
 *   PMD_SIZE    : 2M (2^21)
 *   PMD_MASK    : 0b1111 ....   0000000000000 (0 개수가 21개)
 *   PTRS_PER_PUD: 512개
 */
#define PMD_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(2)
#define PMD_SIZE		(_AC(1, UL) << PMD_SHIFT)
#define PMD_MASK		(~(PMD_SIZE-1))
#define PTRS_PER_PMD		PTRS_PER_PTE
#endif

/*
 * PUD_SHIFT determines the size a level 1 page table entry can map.
 */
#if CONFIG_PGTABLE_LEVELS > 3
/* IAMROOT, 2021.08.14:
 * - 4K 기준으로 정리
 *   PUD_SHIFT   : PUD에 사용할 SHIFT는 30
 *   PUD_SIZE    : 1G (2^30)
 *   PUD_MASK    : 0b1111 ....   0000000000000 (0 개수가 30개)
 *   PTRS_PER_PUD: 512개
 */
#define PUD_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(1)
#define PUD_SIZE		(_AC(1, UL) << PUD_SHIFT)
#define PUD_MASK		(~(PUD_SIZE-1))
#define PTRS_PER_PUD		PTRS_PER_PTE
#endif

/*
 * PGDIR_SHIFT determines the size a top-level page table entry can map
 * (depending on the configuration, this level can be 0, 1 or 2).
 */
/* IAMROOT, 2021.08.14:
 * - arch/(arm|arm64)/Kconfig의 PGTABLE_LEVELS config에서
 *   page size와 vabits의 값에 따라 정해진 PGTABLE_LEVELS을 확인할 수 있다.
 *
 * +-----------+--------+-----------------+-----+-----+-----+
 * | PAGE_SIZE | vabits | PGTABLE_LEVELS  | PGD | PUD | PMD |
 * +-----------+--------+-----------------+-----+-----+-----+
 * | 4k(12)    | 39     | 3               | 30  | -   | 21  |
 * |           | 48     | 4               | 39  | 30  | 21  |
 * +-----------+--------+-----------------+-----+-----+-----+
 * | 16k(14)   | 36     | 2               | 25  | -   | -   |
 * |           | 47     | 3               | 36  | -   | 25  |
 * |           | 48     | 4               | 47  | 36  | 25  |
 * +-----------+--------+-----------------+-----+-----+-----+
 * | 64k(16)   | 42     | 2               | 29  | -   | -   |
 * |           | 48     | 3               | 42  | -   | 29  |
 * |           | 52     | 3               | 42  | -   | 29  |
 * +-----------+--------+-----------------+-----+-----+-----+
 *
 * - VA_BITS 48, page size 4K 기준
 *   CONFIG_PGTABLE_LEVELS : 4단계
 *
 *   PGDIR_SHIFT : PGD에 사용할 SHIFT는 39
 *   PGDIR_SIZE  : 512G (2^39)
 *   PGDIR_MASK  : 0b1111 ....   0000000000000 (0 개수가 39개)
 *   PTRS_PER_PGD: 512개
 *
 * - VA_BITS 52, page size 64K 기준
 *   CONFIG_PGTABLE_LEVELS : 3단계
 *
 *   PGDIR_SHIFT : PGD에 사용할 SHIFT는 42
 *   PGDIR_SIZE  : 4T (2^42)
 *   PGDIR_MASK  : 0b1111 ....   0000000000000 (0 개수가 42개)
 *   PTRS_PER_PGD: 1024개
 *
 * - VA_BITS 48, page size 16K 기준
 *   CONFIG_PGTABLE_LEVELS : 4단계
 *
 *   PGDIR_SHIFT : PGD에 사용할 SHIFT는 47
 *   PGDIR_SIZE  : 128T (2^47)
 *   PGDIR_MASK  : 0b1111 ....   0000000000000 (0 개수가 47개)
 *   PTRS_PER_PGD: 2개
 */
#define PGDIR_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(4 - CONFIG_PGTABLE_LEVELS)
#define PGDIR_SIZE		(_AC(1, UL) << PGDIR_SHIFT)
#define PGDIR_MASK		(~(PGDIR_SIZE-1))
#define PTRS_PER_PGD		(1 << (VA_BITS - PGDIR_SHIFT))

/*
 * Contiguous page definitions.
 */
/* IAMROOT, 2021.10.09:
 * - 4K page size, 4 levels table 기준
 *   CONT_PTE_SHIFT : 16
 *   CONT_PTES      : 16
 *   CONT_PTE_SIZE  : 64K
 *   CONT_PTE_MASK  : 0xffff_ffff_ffff_0000
 *
 *   CONT_PMD_SHIFT : 24
 *   CONT_PMDS      : 16
 *   CONT_PMD_SIZE  : 32M
 *   CONT_PMD_MASK  : 0xffff_ffff_fe00_0000
 */
#define CONT_PTE_SHIFT		(CONFIG_ARM64_CONT_PTE_SHIFT + PAGE_SHIFT)
#define CONT_PTES		(1 << (CONT_PTE_SHIFT - PAGE_SHIFT))
#define CONT_PTE_SIZE		(CONT_PTES * PAGE_SIZE)
#define CONT_PTE_MASK		(~(CONT_PTE_SIZE - 1))

#define CONT_PMD_SHIFT		(CONFIG_ARM64_CONT_PMD_SHIFT + PMD_SHIFT)
#define CONT_PMDS		(1 << (CONT_PMD_SHIFT - PMD_SHIFT))
#define CONT_PMD_SIZE		(CONT_PMDS * PMD_SIZE)
#define CONT_PMD_MASK		(~(CONT_PMD_SIZE - 1))

/*
 * Hardware page table definitions.
 *
 * Level 0 descriptor (P4D).
 */
#define P4D_TYPE_TABLE		(_AT(p4dval_t, 3) << 0)
#define P4D_TABLE_BIT		(_AT(p4dval_t, 1) << 1)
#define P4D_TYPE_MASK		(_AT(p4dval_t, 3) << 0)
#define P4D_TYPE_SECT		(_AT(p4dval_t, 1) << 0)
#define P4D_SECT_RDONLY		(_AT(p4dval_t, 1) << 7)		/* AP[2] */
#define P4D_TABLE_PXN		(_AT(p4dval_t, 1) << 59)
#define P4D_TABLE_UXN		(_AT(p4dval_t, 1) << 60)

/*
 * Level 1 descriptor (PUD).
 */
#define PUD_TYPE_TABLE		(_AT(pudval_t, 3) << 0)
#define PUD_TABLE_BIT		(_AT(pudval_t, 1) << 1)
#define PUD_TYPE_MASK		(_AT(pudval_t, 3) << 0)
#define PUD_TYPE_SECT		(_AT(pudval_t, 1) << 0)
#define PUD_SECT_RDONLY		(_AT(pudval_t, 1) << 7)		/* AP[2] */
#define PUD_TABLE_PXN		(_AT(pudval_t, 1) << 59)
#define PUD_TABLE_UXN		(_AT(pudval_t, 1) << 60)

/*
 * Level 2 descriptor (PMD).
 */
#define PMD_TYPE_MASK		(_AT(pmdval_t, 3) << 0)
#define PMD_TYPE_TABLE		(_AT(pmdval_t, 3) << 0)
#define PMD_TYPE_SECT		(_AT(pmdval_t, 1) << 0)
#define PMD_TABLE_BIT		(_AT(pmdval_t, 1) << 1)

/*
 * Section
 */
/* IAMROOT, 2021.11.17:
 * - *_SECT_VALID : block/page descriptor의 valid 표시 bit.
 *                  1: valid, MMU가 사용.
 *                  0: invalid, MMU가 사용하지 않음.
 *   *_SECT_USER  : AP[1]
 *   *_SECT_RDONLY: AP[2]
 *                : data access permission bits.
 *                +---------+-----------------------+-----------------+
 *                | AP[2:1] | Access from higher EL | Access from EL0 |
 *                +---------+-----------------------+-----------------+
 *                |     00  |   Read/Write          |   None          |
 *                |     01  |   Read/Write          |   Read/Write    |
 *                |     10  |   Read-Only           |   None          |
 *                |     11  |   Read-Only           |   Read-Only     |
 *                +---------+-----------------------+-----------------+
 *   *_SECT_S     : TODO
 *   *_SECT_AF    : access flag bit.
 *                  access 된 적이 있으면 1, 아니면 0으로 설정된다.
 *   *_SECT_NG    : not-Global bit.
 *         nG == 0: trans-table 전역(global) 접근을 의미하며 이는
 *                  모든 processes가 해당 region에 접근할 수 있음을 의미한다.
 *         nG == 1: trans-table global 접근이 불가능함을 의미하며 이는
 *                  process-specific을 뜻하기도 하고 현재 ASID에서만 가능함을
 *                  의미한다.
 *   *_SECT_CONT  : contiguous bit.
 *                  TODO
 *
 * - *_SECT_*XN : block/page에 적용하는 access bits.
 * - *_TABLE_*XN: page table에 적용하는 access bits.
 *                TABLE bits가 설정되면 자식 table/block/page 모두
 *                *XN bits가 설정된다. (상속)
 * - PXN: kernel이 접근할 수 없도록 설정.
 *        주로 application code/data, device peripherals addr에 설정된다.
 * - UXN: application이 접근할 수 없도록 설정.
 *        커널 영역 addr에 설정된다.
 *
 * link: https://developer.arm.com/documentation/den0024/a/BABCEADG
 */
#define PMD_SECT_VALID		(_AT(pmdval_t, 1) << 0)
#define PMD_SECT_USER		(_AT(pmdval_t, 1) << 6)		/* AP[1] */
#define PMD_SECT_RDONLY		(_AT(pmdval_t, 1) << 7)		/* AP[2] */
#define PMD_SECT_S		(_AT(pmdval_t, 3) << 8)
#define PMD_SECT_AF		(_AT(pmdval_t, 1) << 10)
#define PMD_SECT_NG		(_AT(pmdval_t, 1) << 11)
#define PMD_SECT_CONT		(_AT(pmdval_t, 1) << 52)
#define PMD_SECT_PXN		(_AT(pmdval_t, 1) << 53)
#define PMD_SECT_UXN		(_AT(pmdval_t, 1) << 54)
#define PMD_TABLE_PXN		(_AT(pmdval_t, 1) << 59)
#define PMD_TABLE_UXN		(_AT(pmdval_t, 1) << 60)

/*
 * AttrIndx[2:0] encoding (mapping attributes defined in the MAIR* registers).
 */
#define PMD_ATTRINDX(t)		(_AT(pmdval_t, (t)) << 2)
#define PMD_ATTRINDX_MASK	(_AT(pmdval_t, 7) << 2)

/*
 * Level 3 descriptor (PTE).
 */
/* IAMROOT, 2022.06.04:
 * - PTE_VALID
 *   hardware mapping이 되있는지 확인.
 *   cpu가 접근 가능하게 한다는뜻.
 */
#define PTE_VALID		(_AT(pteval_t, 1) << 0)
#define PTE_TYPE_MASK		(_AT(pteval_t, 3) << 0)
#define PTE_TYPE_PAGE		(_AT(pteval_t, 3) << 0)
#define PTE_TABLE_BIT		(_AT(pteval_t, 1) << 1)
#define PTE_USER		(_AT(pteval_t, 1) << 6)		/* AP[1] */
#define PTE_RDONLY		(_AT(pteval_t, 1) << 7)		/* AP[2] */
#define PTE_SHARED		(_AT(pteval_t, 3) << 8)		/* SH[1:0], inner shareable */
#define PTE_AF			(_AT(pteval_t, 1) << 10)	/* Access Flag */
#define PTE_NG			(_AT(pteval_t, 1) << 11)	/* nG */
#define PTE_GP			(_AT(pteval_t, 1) << 50)	/* BTI guarded */
/* IAMROOT, 2023.06.24:
 * - cpu가 관리한다.
 */
#define PTE_DBM			(_AT(pteval_t, 1) << 51)	/* Dirty Bit Management */
#define PTE_CONT		(_AT(pteval_t, 1) << 52)	/* Contiguous range */
#define PTE_PXN			(_AT(pteval_t, 1) << 53)	/* Privileged XN */
#define PTE_UXN			(_AT(pteval_t, 1) << 54)	/* User XN */

/* IAMROOT, 2021.08.21:
 * - PTE_ADDR_LOW
 *   하위 PAGE_SHIFT 만큼의 bits를 제외한 나머지 bit를 1로 하여 Page 크기만큼
 *   align된 주소를 얻는다.
 *
 *   예) PAGE_SHIFT == 12,
 *       1 << (48 - 12) == 1 << 36
 *       (1 << 36) - 1  == 0x10_0000_0000 - 1
 *                      == 0x0f_ffff_ffff
 *       0x0f_ffff_ffff << PAGE_SHIFT(12) == 0xf_fff_ffff_000
 *
 * - PTE_ADDR_HIGH
 *   0xf << 12 == 0xf000
 *
 *   PTE_ADDR_HIGH | PTE_ADDR_LOW == 0xff ... ffff_f000
 */
#define PTE_ADDR_LOW		(((_AT(pteval_t, 1) << (48 - PAGE_SHIFT)) - 1) << PAGE_SHIFT)
#ifdef CONFIG_ARM64_PA_BITS_52
#define PTE_ADDR_HIGH		(_AT(pteval_t, 0xf) << 12)
#define PTE_ADDR_MASK		(PTE_ADDR_LOW | PTE_ADDR_HIGH)
#else
#define PTE_ADDR_MASK		PTE_ADDR_LOW
#endif

/*
 * AttrIndx[2:0] encoding (mapping attributes defined in the MAIR* registers).
 */
#define PTE_ATTRINDX(t)		(_AT(pteval_t, (t)) << 2)
#define PTE_ATTRINDX_MASK	(_AT(pteval_t, 7) << 2)

/*
 * Memory Attribute override for Stage-2 (MemAttr[3:0])
 */
#define PTE_S2_MEMATTR(t)	(_AT(pteval_t, (t)) << 2)

/*
 * Highest possible physical address supported.
 */
#define PHYS_MASK_SHIFT		(CONFIG_ARM64_PA_BITS)
#define PHYS_MASK		((UL(1) << PHYS_MASK_SHIFT) - 1)

#define TTBR_CNP_BIT		(UL(1) << 0)

/*
 * TCR flags.
 */
#define TCR_T0SZ_OFFSET		0
#define TCR_T1SZ_OFFSET		16
#define TCR_T0SZ(x)		((UL(64) - (x)) << TCR_T0SZ_OFFSET)
#define TCR_T1SZ(x)		((UL(64) - (x)) << TCR_T1SZ_OFFSET)

/* IAMROOT, 2021.08.21:
 * - TCR_T0SZ 값과 TCR_T1SZ 값 모두 한번에 가져온다.
 */
#define TCR_TxSZ(x)		(TCR_T0SZ(x) | TCR_T1SZ(x))
#define TCR_TxSZ_WIDTH		6
#define TCR_T0SZ_MASK		(((UL(1) << TCR_TxSZ_WIDTH) - 1) << TCR_T0SZ_OFFSET)
#define TCR_T1SZ_MASK		(((UL(1) << TCR_TxSZ_WIDTH) - 1) << TCR_T1SZ_OFFSET)

#define TCR_EPD0_SHIFT		7
#define TCR_EPD0_MASK		(UL(1) << TCR_EPD0_SHIFT)
#define TCR_IRGN0_SHIFT		8
#define TCR_IRGN0_MASK		(UL(3) << TCR_IRGN0_SHIFT)
#define TCR_IRGN0_NC		(UL(0) << TCR_IRGN0_SHIFT)
#define TCR_IRGN0_WBWA		(UL(1) << TCR_IRGN0_SHIFT)
#define TCR_IRGN0_WT		(UL(2) << TCR_IRGN0_SHIFT)
#define TCR_IRGN0_WBnWA		(UL(3) << TCR_IRGN0_SHIFT)

#define TCR_EPD1_SHIFT		23
#define TCR_EPD1_MASK		(UL(1) << TCR_EPD1_SHIFT)
#define TCR_IRGN1_SHIFT		24
#define TCR_IRGN1_MASK		(UL(3) << TCR_IRGN1_SHIFT)
#define TCR_IRGN1_NC		(UL(0) << TCR_IRGN1_SHIFT)
#define TCR_IRGN1_WBWA		(UL(1) << TCR_IRGN1_SHIFT)
#define TCR_IRGN1_WT		(UL(2) << TCR_IRGN1_SHIFT)
#define TCR_IRGN1_WBnWA		(UL(3) << TCR_IRGN1_SHIFT)

#define TCR_IRGN_NC		(TCR_IRGN0_NC | TCR_IRGN1_NC)
#define TCR_IRGN_WBWA		(TCR_IRGN0_WBWA | TCR_IRGN1_WBWA)
#define TCR_IRGN_WT		(TCR_IRGN0_WT | TCR_IRGN1_WT)
#define TCR_IRGN_WBnWA		(TCR_IRGN0_WBnWA | TCR_IRGN1_WBnWA)
#define TCR_IRGN_MASK		(TCR_IRGN0_MASK | TCR_IRGN1_MASK)


#define TCR_ORGN0_SHIFT		10
#define TCR_ORGN0_MASK		(UL(3) << TCR_ORGN0_SHIFT)
#define TCR_ORGN0_NC		(UL(0) << TCR_ORGN0_SHIFT)
#define TCR_ORGN0_WBWA		(UL(1) << TCR_ORGN0_SHIFT)
#define TCR_ORGN0_WT		(UL(2) << TCR_ORGN0_SHIFT)
#define TCR_ORGN0_WBnWA		(UL(3) << TCR_ORGN0_SHIFT)

#define TCR_ORGN1_SHIFT		26
#define TCR_ORGN1_MASK		(UL(3) << TCR_ORGN1_SHIFT)
#define TCR_ORGN1_NC		(UL(0) << TCR_ORGN1_SHIFT)
#define TCR_ORGN1_WBWA		(UL(1) << TCR_ORGN1_SHIFT)
#define TCR_ORGN1_WT		(UL(2) << TCR_ORGN1_SHIFT)
#define TCR_ORGN1_WBnWA		(UL(3) << TCR_ORGN1_SHIFT)

#define TCR_ORGN_NC		(TCR_ORGN0_NC | TCR_ORGN1_NC)
#define TCR_ORGN_WBWA		(TCR_ORGN0_WBWA | TCR_ORGN1_WBWA)
#define TCR_ORGN_WT		(TCR_ORGN0_WT | TCR_ORGN1_WT)
#define TCR_ORGN_WBnWA		(TCR_ORGN0_WBnWA | TCR_ORGN1_WBnWA)
#define TCR_ORGN_MASK		(TCR_ORGN0_MASK | TCR_ORGN1_MASK)

#define TCR_SH0_SHIFT		12
#define TCR_SH0_MASK		(UL(3) << TCR_SH0_SHIFT)
#define TCR_SH0_INNER		(UL(3) << TCR_SH0_SHIFT)

#define TCR_SH1_SHIFT		28
#define TCR_SH1_MASK		(UL(3) << TCR_SH1_SHIFT)
#define TCR_SH1_INNER		(UL(3) << TCR_SH1_SHIFT)
#define TCR_SHARED		(TCR_SH0_INNER | TCR_SH1_INNER)

#define TCR_TG0_SHIFT		14
#define TCR_TG0_MASK		(UL(3) << TCR_TG0_SHIFT)
#define TCR_TG0_4K		(UL(0) << TCR_TG0_SHIFT)
#define TCR_TG0_64K		(UL(1) << TCR_TG0_SHIFT)
#define TCR_TG0_16K		(UL(2) << TCR_TG0_SHIFT)

#define TCR_TG1_SHIFT		30
#define TCR_TG1_MASK		(UL(3) << TCR_TG1_SHIFT)
#define TCR_TG1_16K		(UL(1) << TCR_TG1_SHIFT)
#define TCR_TG1_4K		(UL(2) << TCR_TG1_SHIFT)
#define TCR_TG1_64K		(UL(3) << TCR_TG1_SHIFT)

#define TCR_IPS_SHIFT		32
#define TCR_IPS_MASK		(UL(7) << TCR_IPS_SHIFT)
#define TCR_A1			(UL(1) << 22)
#define TCR_ASID16		(UL(1) << 36)
#define TCR_TBI0		(UL(1) << 37)
#define TCR_TBI1		(UL(1) << 38)
#define TCR_HA			(UL(1) << 39)
#define TCR_HD			(UL(1) << 40)
#define TCR_TBID1		(UL(1) << 52)
#define TCR_NFD0		(UL(1) << 53)
#define TCR_NFD1		(UL(1) << 54)
#define TCR_E0PD0		(UL(1) << 55)
#define TCR_E0PD1		(UL(1) << 56)

/*
 * TTBR.
 */
#ifdef CONFIG_ARM64_PA_BITS_52
/*
 * This should be GENMASK_ULL(47, 2).
 * TTBR_ELx[1] is RES0 in this configuration.
 */
/* IAMROOT, 2021.09.02:
 * - TTBR_BADDR_MASK_52 == (47 ~ 2)번째 비트가 전부 1인 숫자.
 */
#define TTBR_BADDR_MASK_52	(((UL(1) << 46) - 1) << 2)
#endif

/* IAMROOT, 2021.08.28:
 * pgd entry를 확장할려고 계산하기 위한것.
 *
 * - VA_BITS 가 52bit이면 PAGE_SIZE가 64kb임을 고려한다.
 *
 * - 52 - 42 = 10  --> 2 ^10 = 1024
 * - 48 - 42 = 6   --> 2 ^ 6 = 64
 *   VA_BITS가 48일 때는 64개만을 원래 썻었는데
 *   VA_BITS가 52일 때는 1024개까지 확장을 한다는 뜻.
 *
 * (1024 - 64) * 8 = 7680 (0x1e00)
 */
#ifdef CONFIG_ARM64_VA_BITS_52
/* Must be at least 64-byte aligned to prevent corruption of the TTBR */
#define TTBR1_BADDR_4852_OFFSET	(((UL(1) << (52 - PGDIR_SHIFT)) - \
				 (UL(1) << (48 - PGDIR_SHIFT))) * 8)
#endif

#endif
