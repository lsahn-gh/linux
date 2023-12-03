/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PFN_H_
#define _LINUX_PFN_H_

#ifndef __ASSEMBLY__
#include <linux/types.h>

/*
 * pfn_t: encapsulates a page-frame number that is optionally backed
 * by memmap (struct page).  Whether a pfn_t has a 'struct page'
 * backing is indicated by flags in the high bits of the value.
 */
typedef struct {
	u64 val;
} pfn_t;
#endif

/* IAMROOT, 2021.11.06:
 * - PFN: Page Frame Number
 *   pfn은 phys page를 의미하므로 page size로 align 되어 있어야 한다.
 *   따라서 pte >> PAGE_SHIFT를 통해 간단하게 계산할 수 있다.
 *   A physical page of memory is identified by the Page Frame Number (PFN).
 *   The PFN can be easily computed from the physical address by dividing it
 *   with the size of the page (or by shifting the physical address with
 *   PAGE_SHIFT bits to the right).
 */
#define PFN_ALIGN(x)	(((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)
#define PFN_UP(x)	(((x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)
#define PFN_PHYS(x)	((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

#endif
