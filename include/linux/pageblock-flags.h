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

/* IAMROOT, 2024.09.02:
 * - pageblock 이란?
 *
 *   page 들을 그룹화한 더 큰 단위. 메모리 관리의 효율성을 높이기 위해
 *   여러 page를 묶어 관리하는데, 이 묶음의 단위가 pageblock임.
 *   kernel은 필요시 여러 pages 를 한번에 할당하거나 해제하기 위해
 *   pageblock 개념을 사용함.
 *
 *   pageblock 단위는 메모리의 연속성을 유지하고,
 *   TLB(Translation Lookaside Buffer) 효율을 높이며 시스템 전반의 성능 향상.
 */

#include <linux/types.h>
/* IAMROOT, 2021.11.20:
 * - MIGRATE_PCPTYPES을 나타내는데 사용되는 bits 수.
 *
 *   'enum migratetype' 참고 필요.
 */
#define PB_migratetype_bits 3

/* Bit indices that affect a whole block of pages */
/* IAMROOT, 2021.11.20: TODO
 * - pageblock을 나타내는데 필요한 bits수
 *
 *   o PB_migrate                 = 0
 *   o PB_migrate_end (0 + 3 - 1) = 2
 *   o PB_migrate_skip            = 3
 *   o NR_PAGEBLOCK_BITS          = 4
 *
 *   o migration이 가능한 memory, 불가능한 memory 존재.
 *     - kernel이 사용하는 memory는 migration 불가능.
 *     - application memory는 가능 (hotplug)
 *   o page order 단위로 관리.
 *   o page scan시 필요로 하는 정보 기록.
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

/* If huge pages are not used, group by MAX_ORDER_NR_PAGES */
/* IAMROOT, 2021.11.13:
 * - buddy system에서 사용하는 order 그대로 사용.
 *
 *   MAX_ORDER      : 11
 *   pageblock_order: 10
 */
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
