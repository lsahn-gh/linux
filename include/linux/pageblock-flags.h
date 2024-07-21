/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Macros for manipulating and testing flags related to a
 * pageblock_nr_pages number of pages.
 *
 * Copyright (C) IBM Corporation, 2006
 *
 * Original author, Mel Gorman
 * Major cleanups and reduction of bit operations, Andy Whitcroft
 */
#ifndef PAGEBLOCK_FLAGS_H
#define PAGEBLOCK_FLAGS_H

#include <linux/types.h>
/*
 * IAMROOT, 2021.11.20:
 * - migratetype 참고
 *   MIGRATE_PCPTYPES 들을 의미함.
 */
#define PB_migratetype_bits 3

/* Bit indices that affect a whole block of pages */
/*
 * IAMROOT, 2021.11.20:
 * - pageblock을 나타내는데 필요한 bits수
 * - PB_migrate_end = 0 + 3 - 1 = 2
 *   PB_migrate_skip            = 3
 *   NR_PAGEBLOCK_BITS          = 4
 *
 * - migrate가 가능한 memory, 불가능한 memory가 존재한다.
 *   (ex. kerenl이 사용하는 memory는 migrate가 불가능,
 *   application memory는 가능(hop plug))
 * - migrate라는 의미가 이동이 가능하다는 의미도 된다.
 * - page_order 단위로 관리한다.
 * - page scan할때 필요로하는 정보를 기록
 */
enum pageblock_bits {
	PB_migrate,
	PB_migrate_end = PB_migrate + PB_migratetype_bits - 1,
			/* 3 bits required for migrate types */
	PB_migrate_skip,/* If set the block is skipped by compaction */

	/*
	 * Assume the bits will always align on a word. If this assumption
	 * changes then get/set pageblock needs updating.
	 */
	NR_PAGEBLOCK_BITS
};

/*
 * IAMROOT, 2021.11.13:
 * - default set
 */
#ifdef CONFIG_HUGETLB_PAGE

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE

/* IAMROOT, 2021.11.13:
 * - Runtime에 결정되는 order를 사용한다.
 *   따라서 variable로 선언되어 있다.
 */
/* Huge page sizes are variable */
extern unsigned int pageblock_order;

#else /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/* IAMROOT, 2021.11.13:
 * - Compile-time에 결정된 order를 사용한다.
 *
 *   4kb page 크기에서 값은 '9'이며 이는 다음을 의미함.
 *   (2MB = 2^9 * 4K)
 *
 *   pageblock_order : 9 (HUGETLB_PAGE_ORDER)
 */
/* Huge pages are a constant size */
#define pageblock_order		HUGETLB_PAGE_ORDER

#endif /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

#else /* CONFIG_HUGETLB_PAGE */
/*
 * IAMROOT, 2021.11.13:
 * - buddy system에서 사용하는 것 그대로 그냥쓴다.
 *   2^0 ~ 2^9 로 사용.
 */
/* If huge pages are not used, group by MAX_ORDER_NR_PAGES */
#define pageblock_order		(MAX_ORDER-1)

#endif /* CONFIG_HUGETLB_PAGE */

#define pageblock_nr_pages	(1UL << pageblock_order)

/* Forward declaration */
struct page;

unsigned long get_pfnblock_flags_mask(const struct page *page,
				unsigned long pfn,
				unsigned long mask);

void set_pfnblock_flags_mask(struct page *page,
				unsigned long flags,
				unsigned long pfn,
				unsigned long mask);

/* Declarations for getting and setting flags. See mm/page_alloc.c */
#ifdef CONFIG_COMPACTION
/*
 * IAMROOT, 2022.03.19:
 * - @page가 소속되있는 pageblock에 skip bit가 있는지 확인한다.
 */
#define get_pageblock_skip(page) \
	get_pfnblock_flags_mask(page, page_to_pfn(page),	\
			(1 << (PB_migrate_skip)))
#define clear_pageblock_skip(page) \
	set_pfnblock_flags_mask(page, 0, page_to_pfn(page),	\
			(1 << PB_migrate_skip))
#define set_pageblock_skip(page) \
	set_pfnblock_flags_mask(page, (1 << PB_migrate_skip),	\
			page_to_pfn(page),			\
			(1 << PB_migrate_skip))
#else
static inline bool get_pageblock_skip(struct page *page)
{
	return false;
}
static inline void clear_pageblock_skip(struct page *page)
{
}
static inline void set_pageblock_skip(struct page *page)
{
}
#endif /* CONFIG_COMPACTION */

#endif	/* PAGEBLOCK_FLAGS_H */
