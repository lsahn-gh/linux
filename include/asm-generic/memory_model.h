/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_MEMORY_MODEL_H
#define __ASM_MEMORY_MODEL_H

#include <linux/pfn.h>

#ifndef __ASSEMBLY__

/* IAMROOT, 2023.11.27:
 * - Kernel에서는 2가지의 Phys Memory Model을 사용하는데 그에 따라서
 *   pfn <-> page로 변환하는 과정이 달라 아래 define으로 conf에 따라
 *   다르게 정의한다.
 *
 * - Phys Memory-Models
 *   1. FLATMEM
 *      - 가장 simple한 mem-model이며 non-NUMA에 최적화되어 있다.
 *        phys mem이 linear하게 존재하는 시스템이다.
 *   2. SPARSEMEM
 *      - linux를 사용하는 대부분의 시스템에서 유연하게 사용할 수 있는
 *        mem-model이며 hot-plug/remove가 가능하다.
 *
 * link: https://www.kernel.org/doc/html/v5.15/vm/memory-model.html
 */

/*
 * supports 3 memory models.
 */
#if defined(CONFIG_FLATMEM)

#ifndef ARCH_PFN_OFFSET
#define ARCH_PFN_OFFSET		(0UL)
#endif

/* IAMROOT, 2023.11.27:
 * - FLATMEM을 사용하는 경우 global mem_map을 통해 관리한다.
 */
#define __pfn_to_page(pfn)	(mem_map + ((pfn) - ARCH_PFN_OFFSET))
#define __page_to_pfn(page)	((unsigned long)((page) - mem_map) + \
				 ARCH_PFN_OFFSET)

#elif defined(CONFIG_SPARSEMEM_VMEMMAP)
/* IAMROOT, 2021.12.11:
 * - SPARSEMEM + VMEMMAP을 사용하는 경우 vmemmap을 통해 pfn을 구하면 되므로
 *   FLATMEM과 동일한 성능으로 변환이 가능하다.
 */
/* memmap is virtually contiguous.  */
#define __pfn_to_page(pfn)	(vmemmap + (pfn))
#define __page_to_pfn(page)	(unsigned long)((page) - vmemmap)

#elif defined(CONFIG_SPARSEMEM)
/* IAMROOT, 2021.12.11:
 * - SPARSEMEM + non-VMEMMAP인 경우 mem_map 구조체를 통해 구해야된다.
 */

/*
 * Note: section's mem_map is encoded to reflect its start_pfn.
 * section[i].section_mem_map == mem_map's address - start_pfn;
 */
#define __page_to_pfn(pg)					\
({	const struct page *__pg = (pg);				\
	int __sec = page_to_section(__pg);			\
	(unsigned long)(__pg - __section_mem_map_addr(__nr_to_section(__sec)));	\
})

/* IAMROOT, 2024.09.05:
 * - @pfn을 입력받아 ptr(struct page)로 변환한다.
 *
 *   1) @pfn이 속한 section을 구한다.
 *   2) section의 mem_map 주소를 가져온다.
 *   3) mem_map 주소에 @pfn을 더해 해당 page를 구한다.
 *
 */
#define __pfn_to_page(pfn)				\
({	unsigned long __pfn = (pfn);			\
	struct mem_section *__sec = __pfn_to_section(__pfn);	\
	__section_mem_map_addr(__sec) + __pfn;		\
})
#endif /* CONFIG_FLATMEM/SPARSEMEM */

/*
 * Convert a physical address to a Page Frame Number and back
 */
/* IAMROOT, 2021.10.09:
 * - __phys_to_pfn(paddr):
 *   @paddr를 pfn으로 변환한다.
 *
 * - __pfn_to_phys(pfn):
 *   @pfn을 paddr로 변환한다.
 */
#define	__phys_to_pfn(paddr)	PHYS_PFN(paddr)
#define	__pfn_to_phys(pfn)	PFN_PHYS(pfn)

/* IAMROOT, 2021.12.11:
 * - page_to_pfn(page):
 *   @page를 pfn으로 변환한다.
 *
 * - pfn_to_page(pfn):
 *   @pfn을 page로 변환한다.
 */
#define page_to_pfn __page_to_pfn
#define pfn_to_page __pfn_to_page

#endif /* __ASSEMBLY__ */

#endif
