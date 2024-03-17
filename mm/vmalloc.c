// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  SMP-safe vmalloc/vfree/ioremap, Tigran Aivazian <tigran@veritas.com>, May 2000
 *  Major rework to support vmap/vunmap, Christoph Hellwig, SGI, August 2002
 *  Numa awareness, Christoph Lameter, SGI, June 2005
 *  Improving global KVA allocator, Uladzislau Rezki, Sony, May 2019
 */

#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/set_memory.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/rbtree.h>
#include <linux/xarray.h>
#include <linux/io.h>
#include <linux/rcupdate.h>
#include <linux/pfn.h>
#include <linux/kmemleak.h>
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/llist.h>
#include <linux/bitops.h>
#include <linux/rbtree_augmented.h>
#include <linux/overflow.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>
#include <linux/hugetlb.h>
#include <asm/tlbflush.h>
#include <asm/shmparam.h>

#include "internal.h"
#include "pgalloc-track.h"

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
static unsigned int __ro_after_init ioremap_max_page_shift = BITS_PER_LONG - 1;

static int __init set_nohugeiomap(char *str)
{
	ioremap_max_page_shift = PAGE_SHIFT;
	return 0;
}
early_param("nohugeiomap", set_nohugeiomap);
#else /* CONFIG_HAVE_ARCH_HUGE_VMAP */
static const unsigned int ioremap_max_page_shift = PAGE_SHIFT;
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMAP */

#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
/*
 * IAMROOT, 2022.07.02: 
 * - vmap_allow_huge: 
 *   2M 단위의 huge(pmd) 매핑을 허용할지 여부(default=true)
 * - "nohugevmalloc" 커널 파라미터를 사용하면 huge 매핑을 사용하지 않게 한다.
 */
static bool __ro_after_init vmap_allow_huge = true;

static int __init set_nohugevmalloc(char *str)
{
	vmap_allow_huge = false;
	return 0;
}
early_param("nohugevmalloc", set_nohugevmalloc);
#else /* CONFIG_HAVE_ARCH_HUGE_VMALLOC */
static const bool vmap_allow_huge = false;
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMALLOC */

bool is_vmalloc_addr(const void *x)
{
	unsigned long addr = (unsigned long)x;

	return addr >= VMALLOC_START && addr < VMALLOC_END;
}
EXPORT_SYMBOL(is_vmalloc_addr);

struct vfree_deferred {
	struct llist_head list;
	struct work_struct wq;
};
static DEFINE_PER_CPU(struct vfree_deferred, vfree_deferred);

static void __vunmap(const void *, int);

/*
 * IAMROOT, 2022.07.02: 
 * 워크큐의 워커 스레드가 호출해주는 함수로 인터럽트 context에서 vfree
 * 또는 vunmap API를 호출하면 처리를 워커 스레드에 이관하여 처리하도록 
 * 한다.
 */
static void free_work(struct work_struct *w)
{
	struct vfree_deferred *p = container_of(w, struct vfree_deferred, wq);
	struct llist_node *t, *llnode;

	llist_for_each_safe(llnode, t, llist_del_all(&p->list))
		__vunmap((void *)llnode, 1);
}

/*** Page table manipulation functions ***/
static int vmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	pte_t *pte;
	u64 pfn;
	unsigned long size = PAGE_SIZE;

	pfn = phys_addr >> PAGE_SHIFT;
	pte = pte_alloc_kernel_track(pmd, addr, mask);
	if (!pte)
		return -ENOMEM;
	do {
		BUG_ON(!pte_none(*pte));

#ifdef CONFIG_HUGETLB_PAGE
		size = arch_vmap_pte_range_map_size(addr, end, pfn, max_page_shift);
		if (size != PAGE_SIZE) {
			pte_t entry = pfn_pte(pfn, prot);

			entry = pte_mkhuge(entry);
			entry = arch_make_huge_pte(entry, ilog2(size), 0);
			set_huge_pte_at(&init_mm, addr, pte, entry);
			pfn += PFN_DOWN(size);
			continue;
		}
#endif
		set_pte_at(&init_mm, addr, pte, pfn_pte(pfn, prot));
		pfn++;
	} while (pte += PFN_DOWN(size), addr += size, addr != end);
	*mask |= PGTBL_PTE_MODIFIED;
	return 0;
}

static int vmap_try_huge_pmd(pmd_t *pmd, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	if (max_page_shift < PMD_SHIFT)
		return 0;

	if (!arch_vmap_pmd_supported(prot))
		return 0;

	if ((end - addr) != PMD_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, PMD_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, PMD_SIZE))
		return 0;

	if (pmd_present(*pmd) && !pmd_free_pte_page(pmd, addr))
		return 0;

	return pmd_set_huge(pmd, phys_addr, prot);
}

static int vmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_alloc_track(&init_mm, pud, addr, mask);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pmd_addr_end(addr, end);

		if (vmap_try_huge_pmd(pmd, addr, next, phys_addr, prot,
					max_page_shift)) {
			*mask |= PGTBL_PMD_MODIFIED;
			continue;
		}

		if (vmap_pte_range(pmd, addr, next, phys_addr, prot, max_page_shift, mask))
			return -ENOMEM;
	} while (pmd++, phys_addr += (next - addr), addr = next, addr != end);
	return 0;
}

static int vmap_try_huge_pud(pud_t *pud, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	if (max_page_shift < PUD_SHIFT)
		return 0;

	if (!arch_vmap_pud_supported(prot))
		return 0;

	if ((end - addr) != PUD_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, PUD_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, PUD_SIZE))
		return 0;

	if (pud_present(*pud) && !pud_free_pmd_page(pud, addr))
		return 0;

	return pud_set_huge(pud, phys_addr, prot);
}

static int vmap_pud_range(p4d_t *p4d, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_alloc_track(&init_mm, p4d, addr, mask);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);

		if (vmap_try_huge_pud(pud, addr, next, phys_addr, prot,
					max_page_shift)) {
			*mask |= PGTBL_PUD_MODIFIED;
			continue;
		}

		if (vmap_pmd_range(pud, addr, next, phys_addr, prot,
					max_page_shift, mask))
			return -ENOMEM;
	} while (pud++, phys_addr += (next - addr), addr = next, addr != end);
	return 0;
}

static int vmap_try_huge_p4d(p4d_t *p4d, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	if (max_page_shift < P4D_SHIFT)
		return 0;

	if (!arch_vmap_p4d_supported(prot))
		return 0;

	if ((end - addr) != P4D_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, P4D_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, P4D_SIZE))
		return 0;

	if (p4d_present(*p4d) && !p4d_free_pud_page(p4d, addr))
		return 0;

	return p4d_set_huge(p4d, phys_addr, prot);
}

static int vmap_p4d_range(pgd_t *pgd, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_alloc_track(&init_mm, pgd, addr, mask);
	if (!p4d)
		return -ENOMEM;
	do {
		next = p4d_addr_end(addr, end);

		if (vmap_try_huge_p4d(p4d, addr, next, phys_addr, prot,
					max_page_shift)) {
			*mask |= PGTBL_P4D_MODIFIED;
			continue;
		}

		if (vmap_pud_range(p4d, addr, next, phys_addr, prot,
					max_page_shift, mask))
			return -ENOMEM;
	} while (p4d++, phys_addr += (next - addr), addr = next, addr != end);
	return 0;
}

/*
 * IAMROOT, 2022.07.02: 
 * 물리 주소 @phys_addr를 가상 주소 @addr에 @prot 속성으로 매핑하되, 
 * 매핑 크기는 max_page_shift로 결정한다.
 */
static int vmap_range_noflush(unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	pgd_t *pgd;
	unsigned long start;
	unsigned long next;
	int err;
	pgtbl_mod_mask mask = 0;

	might_sleep();
	BUG_ON(addr >= end);

	start = addr;
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		err = vmap_p4d_range(pgd, addr, next, phys_addr, prot,
					max_page_shift, &mask);
		if (err)
			break;
	} while (pgd++, phys_addr += (next - addr), addr = next, addr != end);

/*
 * IAMROOT, 2022.07.08:
 * - arch_sync_kernel_mappings()은 기본 구현이 없다. ARCH_PAGE_TABLE_SYNC_MASK
 *   가 0이면 compiler에 의해서 삭제되고 arm64는 0이므로 삭제.
 */
	if (mask & ARCH_PAGE_TABLE_SYNC_MASK)
		arch_sync_kernel_mappings(start, end);

	return err;
}

/*
 * IAMROOT, 2022.07.16:
 * - mapping.
 */
int ioremap_page_range(unsigned long addr, unsigned long end,
		phys_addr_t phys_addr, pgprot_t prot)
{
	int err;

	err = vmap_range_noflush(addr, end, phys_addr, pgprot_nx(prot),
				 ioremap_max_page_shift);
	flush_cache_vmap(addr, end);
	return err;
}

static void vunmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	pte_t *pte;

	pte = pte_offset_kernel(pmd, addr);
	do {
		pte_t ptent = ptep_get_and_clear(&init_mm, addr, pte);
		WARN_ON(!pte_none(ptent) && !pte_present(ptent));
	} while (pte++, addr += PAGE_SIZE, addr != end);
	*mask |= PGTBL_PTE_MODIFIED;
}

static void vunmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	pmd_t *pmd;
	unsigned long next;
	int cleared;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);

		cleared = pmd_clear_huge(pmd);
		if (cleared || pmd_bad(*pmd))
			*mask |= PGTBL_PMD_MODIFIED;

		if (cleared)
			continue;
		if (pmd_none_or_clear_bad(pmd))
			continue;
		vunmap_pte_range(pmd, addr, next, mask);

		cond_resched();
	} while (pmd++, addr = next, addr != end);
}

static void vunmap_pud_range(p4d_t *p4d, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	pud_t *pud;
	unsigned long next;
	int cleared;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);

		cleared = pud_clear_huge(pud);
		if (cleared || pud_bad(*pud))
			*mask |= PGTBL_PUD_MODIFIED;

		if (cleared)
			continue;
		if (pud_none_or_clear_bad(pud))
			continue;
		vunmap_pmd_range(pud, addr, next, mask);
	} while (pud++, addr = next, addr != end);
}

static void vunmap_p4d_range(pgd_t *pgd, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	p4d_t *p4d;
	unsigned long next;
	int cleared;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);

		cleared = p4d_clear_huge(p4d);
		if (cleared || p4d_bad(*p4d))
			*mask |= PGTBL_P4D_MODIFIED;

		if (cleared)
			continue;
		if (p4d_none_or_clear_bad(p4d))
			continue;
		vunmap_pud_range(p4d, addr, next, mask);
	} while (p4d++, addr = next, addr != end);
}

/*
 * vunmap_range_noflush is similar to vunmap_range, but does not
 * flush caches or TLBs.
 *
 * The caller is responsible for calling flush_cache_vmap() before calling
 * this function, and flush_tlb_kernel_range after it has returned
 * successfully (and before the addresses are expected to cause a page fault
 * or be re-mapped for something else, if TLB flushes are being delayed or
 * coalesced).
 *
 * This is an internal function only. Do not use outside mm/.
 */
void vunmap_range_noflush(unsigned long start, unsigned long end)
{
	unsigned long next;
	pgd_t *pgd;
	unsigned long addr = start;
	pgtbl_mod_mask mask = 0;

	BUG_ON(addr >= end);
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_bad(*pgd))
			mask |= PGTBL_PGD_MODIFIED;
		if (pgd_none_or_clear_bad(pgd))
			continue;
		vunmap_p4d_range(pgd, addr, next, &mask);
	} while (pgd++, addr = next, addr != end);

	if (mask & ARCH_PAGE_TABLE_SYNC_MASK)
		arch_sync_kernel_mappings(start, end);
}

/**
 * vunmap_range - unmap kernel virtual addresses
 * @addr: start of the VM area to unmap
 * @end: end of the VM area to unmap (non-inclusive)
 *
 * Clears any present PTEs in the virtual address range, flushes TLBs and
 * caches. Any subsequent access to the address before it has been re-mapped
 * is a kernel bug.
 */
void vunmap_range(unsigned long addr, unsigned long end)
{
	flush_cache_vunmap(addr, end);
	vunmap_range_noflush(addr, end);
	flush_tlb_kernel_range(addr, end);
}

static int vmap_pages_pte_range(pmd_t *pmd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	pte_t *pte;

	/*
	 * nr is a running index into the array which helps higher level
	 * callers keep track of where we're up to.
	 */

	pte = pte_alloc_kernel_track(pmd, addr, mask);
	if (!pte)
		return -ENOMEM;
	do {
		struct page *page = pages[*nr];

		if (WARN_ON(!pte_none(*pte)))
			return -EBUSY;
		if (WARN_ON(!page))
			return -ENOMEM;
		set_pte_at(&init_mm, addr, pte, mk_pte(page, prot));
		(*nr)++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
	*mask |= PGTBL_PTE_MODIFIED;
	return 0;
}

static int vmap_pages_pmd_range(pud_t *pud, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_alloc_track(&init_mm, pud, addr, mask);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pmd_addr_end(addr, end);
		if (vmap_pages_pte_range(pmd, addr, next, prot, pages, nr, mask))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static int vmap_pages_pud_range(p4d_t *p4d, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_alloc_track(&init_mm, p4d, addr, mask);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);
		if (vmap_pages_pmd_range(pud, addr, next, prot, pages, nr, mask))
			return -ENOMEM;
	} while (pud++, addr = next, addr != end);
	return 0;
}

static int vmap_pages_p4d_range(pgd_t *pgd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_alloc_track(&init_mm, pgd, addr, mask);
	if (!p4d)
		return -ENOMEM;
	do {
		next = p4d_addr_end(addr, end);
		if (vmap_pages_pud_range(p4d, addr, next, prot, pages, nr, mask))
			return -ENOMEM;
	} while (p4d++, addr = next, addr != end);
	return 0;
}

static int vmap_small_pages_range_noflush(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages)
{
	unsigned long start = addr;
	pgd_t *pgd;
	unsigned long next;
	int err = 0;
	int nr = 0;
	pgtbl_mod_mask mask = 0;

	BUG_ON(addr >= end);
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_bad(*pgd))
			mask |= PGTBL_PGD_MODIFIED;
		err = vmap_pages_p4d_range(pgd, addr, next, prot, pages, &nr, &mask);
		if (err)
			return err;
	} while (pgd++, addr = next, addr != end);

	if (mask & ARCH_PAGE_TABLE_SYNC_MASK)
		arch_sync_kernel_mappings(start, end);

	return 0;
}

/*
 * vmap_pages_range_noflush is similar to vmap_pages_range, but does not
 * flush caches.
 *
 * The caller is responsible for calling flush_cache_vmap() after this
 * function returns successfully and before the addresses are accessed.
 *
 * This is an internal function only. Do not use outside mm/.
 */

/*
 * IAMROOT, 2022.07.02: 
 * 요청한 @pages 구조체 배열로 @addr 주소에 @prot 속성으로 매핑한다.
 * 매핑 단위는 page_shift 값을 사용한다.
 */
int vmap_pages_range_noflush(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages, unsigned int page_shift)
{
	unsigned int i, nr = (end - addr) >> PAGE_SHIFT;

	WARN_ON(page_shift < PAGE_SHIFT);

	if (!IS_ENABLED(CONFIG_HAVE_ARCH_HUGE_VMALLOC) ||
			page_shift == PAGE_SHIFT)
		return vmap_small_pages_range_noflush(addr, end, prot, pages);

	for (i = 0; i < nr; i += 1U << (page_shift - PAGE_SHIFT)) {
		int err;

		err = vmap_range_noflush(addr, addr + (1UL << page_shift),
					__pa(page_address(pages[i])), prot,
					page_shift);
		if (err)
			return err;

		addr += 1UL << page_shift;
	}

	return 0;
}

/**
 * vmap_pages_range - map pages to a kernel virtual address
 * @addr: start of the VM area to map
 * @end: end of the VM area to map (non-inclusive)
 * @prot: page protection flags to use
 * @pages: pages to map (always PAGE_SIZE pages)
 * @page_shift: maximum shift that the pages may be mapped with, @pages must
 * be aligned and contiguous up to at least this shift.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * IAMROOT, 2022.07.02: 
 * 요청한 @pages 구조체 배열로 @addr 주소에 @prot 속성으로 매핑한다.
 * 매핑 단위는 page_shift 값을 사용하고, 매핑 후 flush 한다.
 */
static int vmap_pages_range(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages, unsigned int page_shift)
{
	int err;

	err = vmap_pages_range_noflush(addr, end, prot, pages, page_shift);
	flush_cache_vmap(addr, end);
	return err;
}

/*
 * IAMROOT, 2022.07.02: 
 * 가상 주소 @x가 vmalloc 공간에 존재하는 주소 
 * 또는 module 공간에 존재하는 여부를 판단
 */
int is_vmalloc_or_module_addr(const void *x)
{
	/*
	 * ARM, x86-64 and sparc64 put modules in a special place,
	 * and fall back on vmalloc() if that fails. Others
	 * just put it in the vmalloc space.
	 */
#if defined(CONFIG_MODULES) && defined(MODULES_VADDR)
	unsigned long addr = (unsigned long)x;
	if (addr >= MODULES_VADDR && addr < MODULES_END)
		return 1;
#endif
	return is_vmalloc_addr(x);
}

/*
 * Walk a vmap address to the struct page it maps. Huge vmap mappings will
 * return the tail page that corresponds to the base page address, which
 * matches small vmap mappings.
 */
/*
 * IAMROOT, 2022.07.02: 
 * vmalloc 공간에서 사용된 @vmalloc_addr 가상 주소로 매핑된
 * 물리 페이지를 찾아 반환한다.
 */
struct page *vmalloc_to_page(const void *vmalloc_addr)
{
	unsigned long addr = (unsigned long) vmalloc_addr;
	struct page *page = NULL;
	pgd_t *pgd = pgd_offset_k(addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;

	/*
	 * XXX we might need to change this if we add VIRTUAL_BUG_ON for
	 * architectures that do not vmalloc module space
	 */
	VIRTUAL_BUG_ON(!is_vmalloc_or_module_addr(vmalloc_addr));

	if (pgd_none(*pgd))
		return NULL;
	if (WARN_ON_ONCE(pgd_leaf(*pgd)))
		return NULL; /* XXX: no allowance for huge pgd */
	if (WARN_ON_ONCE(pgd_bad(*pgd)))
		return NULL;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return NULL;
	if (p4d_leaf(*p4d))
		return p4d_page(*p4d) + ((addr & ~P4D_MASK) >> PAGE_SHIFT);
	if (WARN_ON_ONCE(p4d_bad(*p4d)))
		return NULL;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return NULL;
	if (pud_leaf(*pud))
		return pud_page(*pud) + ((addr & ~PUD_MASK) >> PAGE_SHIFT);
	if (WARN_ON_ONCE(pud_bad(*pud)))
		return NULL;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return NULL;
	if (pmd_leaf(*pmd))
		return pmd_page(*pmd) + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
	if (WARN_ON_ONCE(pmd_bad(*pmd)))
		return NULL;

	ptep = pte_offset_map(pmd, addr);
	pte = *ptep;
	if (pte_present(pte))
		page = pte_page(pte);
	pte_unmap(ptep);

	return page;
}
EXPORT_SYMBOL(vmalloc_to_page);

/*
 * Map a vmalloc()-space virtual address to the physical page frame number.
 */
unsigned long vmalloc_to_pfn(const void *vmalloc_addr)
{
	return page_to_pfn(vmalloc_to_page(vmalloc_addr));
}
EXPORT_SYMBOL(vmalloc_to_pfn);


/*** Global kva allocator ***/

#define DEBUG_AUGMENT_PROPAGATE_CHECK 0
#define DEBUG_AUGMENT_LOWEST_MATCH_CHECK 0


static DEFINE_SPINLOCK(vmap_area_lock);
static DEFINE_SPINLOCK(free_vmap_area_lock);
/* Export for kexec only */

/*
 * IAMROOT, 2022.07.02: 
 * 1) vmalloc 자료 구조에서 할당 공간을 관리하는 list와 rb tree
 */
LIST_HEAD(vmap_area_list);
static struct rb_root vmap_area_root = RB_ROOT;
static bool vmap_initialized __read_mostly;

/*
 * IAMROOT, 2022.07.02: 
 * 2) vmalloc 자료 구조에서 lazy free 요청을 관리하는 list와 rb tree
 */
static struct rb_root purge_vmap_area_root = RB_ROOT;
static LIST_HEAD(purge_vmap_area_list);
static DEFINE_SPINLOCK(purge_vmap_area_lock);

/*
 * This kmem_cache is used for vmap_area objects. Instead of
 * allocating from slab we reuse an object from this cache to
 * make things faster. Especially in "no edge" splitting of
 * free block.
 */
static struct kmem_cache *vmap_area_cachep;

/*
 * IAMROOT, 2022.07.02: 
 * 3) vmalloc 자료 구조에서 free 공간을 관리하는 list와 rb tree
 */
/*
 * This linked list is used in pair with free_vmap_area_root.
 * It gives O(1) access to prev/next to perform fast coalescing.
 */
static LIST_HEAD(free_vmap_area_list);

/*
 * This augment red-black tree represents the free vmap space.
 * All vmap_area objects in this tree are sorted by va->va_start
 * address. It is used for allocation and merging when a vmap
 * object is released.
 *
 * Each vmap_area node contains a maximum available free block
 * of its sub-tree, right or left. Therefore it is possible to
 * find a lowest match of free area.
 */
static struct rb_root free_vmap_area_root = RB_ROOT;

/*
 * Preload a CPU with one object for "no edge" split case. The
 * aim is to get rid of allocations from the atomic context, thus
 * to use more permissive allocation masks.
 */
static DEFINE_PER_CPU(struct vmap_area *, ne_fit_preload_node);

static __always_inline unsigned long
va_size(struct vmap_area *va)
{
	return (va->va_end - va->va_start);
}

static __always_inline unsigned long
get_subtree_max_size(struct rb_node *node)
{
	struct vmap_area *va;

	va = rb_entry_safe(node, struct vmap_area, rb_node);
	return va ? va->subtree_max_size : 0;
}

/*
 * Gets called when remove the node and rotate.
 */
static __always_inline unsigned long
compute_subtree_max_size(struct vmap_area *va)
{
	return max3(va_size(va),
		get_subtree_max_size(va->rb_node.rb_left),
		get_subtree_max_size(va->rb_node.rb_right));
}

RB_DECLARE_CALLBACKS_MAX(static, free_vmap_area_rb_augment_cb,
	struct vmap_area, rb_node, unsigned long, subtree_max_size, va_size)

static void purge_vmap_area_lazy(void);
static BLOCKING_NOTIFIER_HEAD(vmap_notify_list);
static unsigned long lazy_max_pages(void);

static atomic_long_t nr_vmalloc_pages;

unsigned long vmalloc_nr_pages(void)
{
	return atomic_long_read(&nr_vmalloc_pages);
}

static struct vmap_area *find_vmap_area_exceed_addr(unsigned long addr)
{
	struct vmap_area *va = NULL;
	struct rb_node *n = vmap_area_root.rb_node;

	while (n) {
		struct vmap_area *tmp;

		tmp = rb_entry(n, struct vmap_area, rb_node);
		if (tmp->va_end > addr) {
			va = tmp;
			if (tmp->va_start <= addr)
				break;

			n = n->rb_left;
		} else
			n = n->rb_right;
	}

	return va;
}

/*
 * iamroot, 2022.07.02: 
 * vmalloc 자료구조에서 @addr에 해당하는 vmap_area를 찾는다.
 * 못 잧은 경우 null을 반환한다.
 */
static struct vmap_area *__find_vmap_area(unsigned long addr)
{
	struct rb_node *n = vmap_area_root.rb_node;

	while (n) {
		struct vmap_area *va;

		va = rb_entry(n, struct vmap_area, rb_node);
		if (addr < va->va_start)
			n = n->rb_left;
		else if (addr >= va->va_end)
			n = n->rb_right;
		else
			return va;
	}

	return NULL;
}

/*
 * This function returns back addresses of parent node
 * and its left or right link for further processing.
 *
 * Otherwise NULL is returned. In that case all further
 * steps regarding inserting of conflicting overlap range
 * have to be declined and actually considered as a bug.
 */
/*
 * IAMROOT, 2022.07.02: 
 * - RB 트리를 @root 또는 @from부터 아래로 탐색하여 @va 영역을 추가할 link를 알아온다.
 *   이 때 출력 인자 @parent에 노드를 기록한다.
 *
 * 예) @va의 시작 주소가 A보다 더 작은 경우
 *     A 노드의 좌측 link를 반환한다.
 *
 *             @root
 *            /     \
 *           A      B
 *          / \    / \
 *       link
 */
static __always_inline struct rb_node **
find_va_links(struct vmap_area *va,
	struct rb_root *root, struct rb_node *from,
	struct rb_node **parent)
{
	struct vmap_area *tmp_va;
	struct rb_node **link;

/*
 * IAMROOT, 2022.07.02: 
 * @root가 지정된 경우 link는 root 노드부터 시작하고, 
 * @root가 지정되지 않는 경우 link는 @from 노드부터 시작한다.
 */
	if (root) {
		link = &root->rb_node;
		if (unlikely(!*link)) {
			*parent = NULL;
			return link;
		}
	} else {
		link = &from;
	}

	/*
	 * Go to the bottom of the tree. When we hit the last point
	 * we end up with parent rb_node and correct direction, i name
	 * it link, where the new va->rb_node will be attached to.
	 */
	do {
		tmp_va = rb_entry(*link, struct vmap_area, rb_node);

		/*
		 * During the traversal we also do some sanity check.
		 * Trigger the BUG() if there are sides(left/right)
		 * or full overlaps.
		 */
		if (va->va_start < tmp_va->va_end &&
				va->va_end <= tmp_va->va_start)
			link = &(*link)->rb_left;
		else if (va->va_end > tmp_va->va_start &&
				va->va_start >= tmp_va->va_end)
			link = &(*link)->rb_right;
		else {
			WARN(1, "vmalloc bug: 0x%lx-0x%lx overlaps with 0x%lx-0x%lx\n",
				va->va_start, va->va_end, tmp_va->va_start, tmp_va->va_end);

			return NULL;
		}
	} while (*link);

	*parent = &tmp_va->rb_node;
	return link;
}

/*
 * IAMROOT, 2023.02.17:
 * - link의 next va를 기존 rtbree/list구조에서 가져온다.
 *   link가 parent right에 있을 경우 link의 next는 parent의 next값이 된다.
 *   link가 parent의 left에 있을경우 link의 next는 parent가 된다.
 *   (rbtree의 구조상 이런 원리가 된다.)
 *
 * ex) 다음과 같은 rb tree가 있다고 가정한다.
 *       53
 *      /   \
 *    38     70
 *    / \    / \
 *  30  39  66 87
 *               \
 *                90
 *
 * ex1) va가 67값이 들어올 경우
 *        53
 *      /    \
 *    38      70
 *    / \    /  \
 *  30  39  66   87
 *            \    \
 *          (link)  90
 *
 *  link자리에 67이 위치하게 될것이다.
 *  link의 parent은 66이고 66의 next는 위 트리에서 70이 된다.
 *  67의 next는 66의 list를 통해 66->next = 70으로 이어져있을 것이므로
 *  66->next를 통해 next를 얻어온다.
 *  70의 값은 실제 link의 next값이 될것이다.
 *
 * ex2) va가 60값이 들어올 경우
 *        53
 *      /    \
 *    38      70
 *    / \    /  \
 *  30  39  66   87
 *          /      \
 *       (link)     90
 *
 *  link자리에 60값이 위치하게 될것이다.
 *  rbtree구조상 left에 위치하게되면 parent가 무조건 next값이 된다.
 *  그래서 그냥 next 값으로 parent를 가져오는것이다.
 */
static __always_inline struct list_head *
get_va_next_sibling(struct rb_node *parent, struct rb_node **link)
{
	struct list_head *list;

	if (unlikely(!parent))
		/*
		 * The red-black tree where we try to find VA neighbors
		 * before merging or inserting is empty, i.e. it means
		 * there is no free vmap space. Normally it does not
		 * happen but we handle this case anyway.
	 	 */
/*
 * IAMROOT, 2023.02.17:
 * - papago
 *   병합 또는 삽입하기 전에 VA 이웃을 찾으려고 시도하는 red-black 트리는 
 *   비어 있습니다. 즉, 사용 가능한 vmap 공간이 없음을 의미합니다. 
 *   일반적으로 발생하지 않지만 어쨌든 이 경우를 처리합니다.
 */
		return NULL;

	list = &rb_entry(parent, struct vmap_area, rb_node)->list;
	return (&parent->rb_right == link ? list->next : list);
}

/*
 * IAMROOT, 2022.07.02: 
 * @va를 RB tree의 link 위치로 연결한다. 또한 @head 리스트에도 추가하되, 
 * 리스트는 시작 주소 순으로 sorting되어 있으므로 해당 위치에 추가한다.
 */
static __always_inline void
link_va(struct vmap_area *va, struct rb_root *root,
	struct rb_node *parent, struct rb_node **link, struct list_head *head)
{
	/*
	 * VA is still not in the list, but we can
	 * identify its future previous list_head node.
	 */
/*
 * IAMROOT, 2022.07.02: 
 * - @parent 노드가 지정된 경우 @link가 @parent 노드의 우측이 아닌 경우에는 
 *   이전 노드를 대상(head)로 둔다.
 *
 * 예) @va의 시작 주소가 A보다 더 작은 경우
 *     A 노드의 좌측 link를 반환한다.
 *
 *             @root
 *            /     \
 *           A      B
 *          / \    / \
 *       link
 *    
 *      list ->   head----A----root----B----
 * 
 *    추가후 ->   head----(va)----A----root----B----
 *               
 * - va가 parent의 left에 있는 경우
 *   parent보다 작으므로 list의 tail쪽에 위치해야 되므로 head를 head->prev로
 *   고친다.
 * - va가 parent의 right에 있는 경우
 *   parent보다 크므로 list next에 위치해야 되므로 그냥 하면된다.
 *
 * ex) 다음과 같은 구조에서 0이 들어올경우
 * 1) 0 입력전
 *         tree             list
 *             2          1----->2----->3------>1..
 *           /  \         ^head         ^head->prev
 *          1     3
 *        /
 *      link(0이 들어올 위치)
 *
 * 2) head 변경
 *
 * 1----->2----->3------>1..
 *               ^head
 * 3) 0입력
 *
 * 1----->2----->3------>(0)----->1..
 *               ^head
 *
 * ex) 다음과 같은 구조에서 2이 들어올경우
 * 1) 2 입력전
 *         tree             list
 *             3          1----->3----->4------>1..
 *           /  \         ^head         
 *          1     4
 *           \
 *         link(2이 들어올 위치)
 *
 * 2) 2입력
 *
 * 1----->(2)------>3----->4------>
 * ^head             
 * 
 */
	if (likely(parent)) {
		head = &rb_entry(parent, struct vmap_area, rb_node)->list;
		if (&parent->rb_right != link)
			head = head->prev;
	}

	/* Insert to the rb-tree */
	rb_link_node(&va->rb_node, parent, link);
/*
 * IAMROOT, 2022.07.07:
 * - root가 free_vmap_area_root면 free_vma_area 방식으로, 아니면 일반 방식으로
 *   inssert한다.
 */
	if (root == &free_vmap_area_root) {
		/*
		 * Some explanation here. Just perform simple insertion
		 * to the tree. We do not set va->subtree_max_size to
		 * its current size before calling rb_insert_augmented().
		 * It is because of we populate the tree from the bottom
		 * to parent levels when the node _is_ in the tree.
		 *
		 * Therefore we set subtree_max_size to zero after insertion,
		 * to let __augment_tree_propagate_from() puts everything to
		 * the correct order later on.
		 */
/*
 * IAMROOT, 2022.07.07:
 * - papgo
 *   여기에 약간의 설명이 있습니다. 트리에 단순 삽입만 수행하면 됩니다.
 *   rb_insert_augmented()를 호출하기 전에 va->subtree_max_size를 현재 크기로
 *   설정하지 않습니다. 
 *   노드가 트리에 있을 때 맨 아래에서 상위 레벨로 트리를 채우기 때문입니다.
 *
 *   따라서 삽입 후 subtree_max_size를 0으로 설정하여
 *   __augment_tree_propagate_from()이 나중에 모든 것을 올바른 순서로
 *   배치하도록 합니다.
 *
 * - insert를 하고나면 subtree_max_size가 계산되서 넣어지는데,
 *   후에 augment_tree_propagate_from() 함수를 통해서 subtree_max_size를 재계산
 *   하기 때문에 0으로 초기화 해준다.
 */
		rb_insert_augmented(&va->rb_node,
			root, &free_vmap_area_rb_augment_cb);
		va->subtree_max_size = 0;
	} else {
		rb_insert_color(&va->rb_node, root);
	}

	/* Address-sort this list */
	list_add(&va->list, head);
}

static __always_inline void
unlink_va(struct vmap_area *va, struct rb_root *root)
{
	if (WARN_ON(RB_EMPTY_NODE(&va->rb_node)))
		return;

	if (root == &free_vmap_area_root)
		rb_erase_augmented(&va->rb_node,
			root, &free_vmap_area_rb_augment_cb);
	else
		rb_erase(&va->rb_node, root);

	list_del(&va->list);
	RB_CLEAR_NODE(&va->rb_node);
}

#if DEBUG_AUGMENT_PROPAGATE_CHECK
static void
augment_tree_propagate_check(void)
{
	struct vmap_area *va;
	unsigned long computed_size;

	list_for_each_entry(va, &free_vmap_area_list, list) {
		computed_size = compute_subtree_max_size(va);
		if (computed_size != va->subtree_max_size)
			pr_emerg("tree is corrupted: %lu, %lu\n",
				va_size(va), va->subtree_max_size);
	}
}
#endif

/*
 * This function populates subtree_max_size from bottom to upper
 * levels starting from VA point. The propagation must be done
 * when VA size is modified by changing its va_start/va_end. Or
 * in case of newly inserting of VA to the tree.
 *
 * It means that __augment_tree_propagate_from() must be called:
 * - After VA has been inserted to the tree(free path);
 * - After VA has been shrunk(allocation path);
 * - After VA has been increased(merging path).
 *
 * Please note that, it does not mean that upper parent nodes
 * and their subtree_max_size are recalculated all the time up
 * to the root node.
 *
 *       4--8
 *        /\
 *       /  \
 *      /    \
 *    2--2  8--8
 *
 * For example if we modify the node 4, shrinking it to 2, then
 * no any modification is required. If we shrink the node 2 to 1
 * its subtree_max_size is updated only, and set to 1. If we shrink
 * the node 8 to 6, then its subtree_max_size is set to 6 and parent
 * node becomes 4--6.
 */
/*
 * IAMROOT, 2022.07.07:
 * - papago
 *   이 함수는 VA 포인트에서 시작하여 하위 레벨에서 상위 레벨로
 *   subtree_max_size를 채웁니다. va_start/va_end를 변경하여 VA 크기를 수정할
 *   때 전파를 수행해야 합니다. 또는 트리에 VA를 새로 삽입하는 경우.
 *
 *   이는 __augment_tree_propagate_from()을 호출해야 함을 의미합니다.
 *   - VA가 트리(자유 경로)에 삽입된 후;
 *   - VA가 축소된 후(할당 경로);
 *   - VA가 증가한 후(병합 경로).
 *
 *   상위 부모 노드와 해당 subtree_max_size가 루트 노드까지 항상 다시
 *   계산된다는 의미는 아닙니다.
 */
static __always_inline void
augment_tree_propagate_from(struct vmap_area *va)
{
	/*
	 * Populate the tree from bottom towards the root until
	 * the calculated maximum available size of checked node
	 * is equal to its current one.
	 */
/*
 * IAMROOT, 2022.07.07:
 * - papgo
 *   확인된 노드의 계산된 최대 사용 가능한 크기가 현재 노드와 같을 때까지
 *   트리를 아래쪽에서 루트 방향으로 채웁니다.
 *
 * - RB_DECLARE_CALLBACKS_MAX -> RB_DECLARE_CALLBACKS를 통해서
 *   _propagate, _copy, _rotate 함수가 생긴다.
 * - recompute 는 va_size()함수를 통해서 수행한다.
 */
	free_vmap_area_rb_augment_cb_propagate(&va->rb_node, NULL);

#if DEBUG_AUGMENT_PROPAGATE_CHECK
	augment_tree_propagate_check();
#endif
}

/*
 * IAMROOT, 2022.07.02: 
 * va를 vmalloc 자료구조(RB tree + list)에 추가한다.
 */
static void
insert_vmap_area(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	struct rb_node **link;
	struct rb_node *parent;

	link = find_va_links(va, root, NULL, &parent);
	if (link)
		link_va(va, root, parent, link, head);
}

/*
 * IAMROOT, 2022.07.08:
 * - insert + propagate
 */
static void
insert_vmap_area_augment(struct vmap_area *va,
	struct rb_node *from, struct rb_root *root,
	struct list_head *head)
{
	struct rb_node **link;
	struct rb_node *parent;

	if (from)
		link = find_va_links(va, NULL, from, &parent);
	else
		link = find_va_links(va, root, NULL, &parent);

	if (link) {
		link_va(va, root, parent, link, head);
		augment_tree_propagate_from(va);
	}
}

/*
 * Merge de-allocated chunk of VA memory with previous
 * and next free blocks. If coalesce is not done a new
 * free area is inserted. If VA has been merged, it is
 * freed.
 *
 * Please note, it can return NULL in case of overlap
 * ranges, followed by WARN() report. Despite it is a
 * buggy behaviour, a system can be alive and keep
 * ongoing.
 */
/*
 * IAMROOT, 2023.02.03:
 * - papago
 *   할당 해제된 VA 메모리 청크를 이전 및 다음 사용 가능한 블록과 병합합니다.
 *   병합이 완료되지 않으면 새로운 빈 영역이 삽입됩니다. VA가 병합된 경우 
 *   해제됩니다.
 *
 *   범위가 겹치는 경우 NULL을 반환하고 WARN() 보고서를 반환할 수 있습니다. 
 *   버그가 있는 동작임에도 불구하고 시스템은 살아 있고 계속 진행될 수 
 *   있습니다.
 *
 * - @va의 next가 될 next va를 구하고, 해당 자리에 va를 추가한다.
 *   이때 next, next->prev와 비교해서 merge가 가능하면 기존 next, next->prev
 *   로 merge를 진행하고 address를 갱신한후 @va는 버린다.
 */
static __always_inline struct vmap_area *
merge_or_add_vmap_area(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	struct vmap_area *sibling;
	struct list_head *next;
	struct rb_node **link;
	struct rb_node *parent;
	bool merged = false;

	/*
	 * Find a place in the tree where VA potentially will be
	 * inserted, unless it is merged with its sibling/siblings.
	 */
/*
 * IAMROOT, 2023.02.17:
 * - papago
 *   VA가 형제/형제와 병합되지 않는 한 잠재적으로 VA가 삽입될 
 *   위치를 트리에서 찾으십시오.
 */
	link = find_va_links(va, root, NULL, &parent);
	if (!link)
		return NULL;

	/*
	 * Get next node of VA to check if merging can be done.
	 */
/*
 * IAMROOT, 2023.02.17:
 * - papago
 *   병합이 가능한지 확인하기 위해 VA의 다음 노드를 가져옵니다. 
 */
	next = get_va_next_sibling(parent, link);
	if (unlikely(next == NULL))
		goto insert;

	/*
	 * start            end
	 * |                |
	 * |<------VA------>|<-----Next----->|
	 *                  |                |
	 *                  start            end
	 */
	if (next != head) {
		sibling = list_entry(next, struct vmap_area, list);
/*
 * IAMROOT, 2023.02.17:
 * - va와 next가 연속된 경우(va -> next 순서) 되므로 합친다.
 *   next를 그대로 사용하고 va를 해제해버린다. next의 start만 갱신하면
 *   merge가 될것이다.
 */
		if (sibling->va_start == va->va_end) {
			sibling->va_start = va->va_start;

			/* Free vmap_area object. */
			kmem_cache_free(vmap_area_cachep, va);

			/* Point to the new merged area. */
			va = sibling;
			merged = true;
		}
	}

	/*
	 * start            end
	 * |                |
	 * |<-----Prev----->|<------VA------>|
	 *                  |                |
	 *                  start            end
	 */
	if (next->prev != head) {
/*
 * IAMROOT, 2023.02.17:
 * - next->prev와 va가 연속된 경우(next->prev -> va 순서) 되므로 합친다.
 */
		sibling = list_entry(next->prev, struct vmap_area, list);
		if (sibling->va_end == va->va_start) {
			/*
			 * If both neighbors are coalesced, it is important
			 * to unlink the "next" node first, followed by merging
			 * with "previous" one. Otherwise the tree might not be
			 * fully populated if a sibling's augmented value is
			 * "normalized" because of rotation operations.
			 */
/*
 * IAMROOT, 2023.02.17:
 * - 이전에 next와 merge가 된상황. next->prev와도 merge가 되는 상황이므로
 *   next와도 merge한다.
 */
			if (merged)
				unlink_va(va, root);

/*
 * IAMROOT, 2023.02.17:
 * - next->prev를 사용하고 va를 지워버린다.
 */
			sibling->va_end = va->va_end;

			/* Free vmap_area object. */
			kmem_cache_free(vmap_area_cachep, va);

			/* Point to the new merged area. */
			va = sibling;
			merged = true;
		}
	}

insert:
/*
 * IAMROOT, 2023.02.17:
 * - merge가 안된상황이므로 새로 추가한다.
 */
	if (!merged)
		link_va(va, root, parent, link, head);

	return va;
}

static __always_inline struct vmap_area *
merge_or_add_vmap_area_augment(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	va = merge_or_add_vmap_area(va, root, head);
	if (va)
		augment_tree_propagate_from(va);

	return va;
}

static __always_inline bool
is_within_this_va(struct vmap_area *va, unsigned long size,
	unsigned long align, unsigned long vstart)
{
	unsigned long nva_start_addr;

	if (va->va_start > vstart)
		nva_start_addr = ALIGN(va->va_start, align);
	else
		nva_start_addr = ALIGN(vstart, align);

	/* Can be overflowed due to big size or alignment. */
	if (nva_start_addr + size < nva_start_addr ||
			nva_start_addr < vstart)
		return false;

	return (nva_start_addr + size <= va->va_end);
}

/*
 * Find the first free block(lowest start address) in the tree,
 * that will accomplish the request corresponding to passing
 * parameters.
 */
/*
 * IAMROOT, 2022.07.02: 
 * @size와 @align이 확보되는 vmalloc의 빈 공간을 아래부터 탐색하여 반환한다.
 */
static __always_inline struct vmap_area *
find_vmap_lowest_match(unsigned long size,
	unsigned long align, unsigned long vstart)
{
	struct vmap_area *va;
	struct rb_node *node;
	unsigned long length;

	/* Start from the root. */
	node = free_vmap_area_root.rb_node;

	/* Adjust the search size for alignment overhead. */
	length = size + align - 1;

	while (node) {
		va = rb_entry(node, struct vmap_area, rb_node);

		if (get_subtree_max_size(node->rb_left) >= length &&
				vstart < va->va_start) {
			node = node->rb_left;
		} else {
			if (is_within_this_va(va, size, align, vstart))
				return va;

			/*
			 * Does not make sense to go deeper towards the right
			 * sub-tree if it does not have a free block that is
			 * equal or bigger to the requested search length.
			 */
			if (get_subtree_max_size(node->rb_right) >= length) {
				node = node->rb_right;
				continue;
			}

			/*
			 * OK. We roll back and find the first right sub-tree,
			 * that will satisfy the search criteria. It can happen
			 * only once due to "vstart" restriction.
			 */
			while ((node = rb_parent(node))) {
				va = rb_entry(node, struct vmap_area, rb_node);
				if (is_within_this_va(va, size, align, vstart))
					return va;

				if (get_subtree_max_size(node->rb_right) >= length &&
						vstart <= va->va_start) {
					node = node->rb_right;
					break;
				}
			}
		}
	}

	return NULL;
}

#if DEBUG_AUGMENT_LOWEST_MATCH_CHECK
#include <linux/random.h>

static struct vmap_area *
find_vmap_lowest_linear_match(unsigned long size,
	unsigned long align, unsigned long vstart)
{
	struct vmap_area *va;

	list_for_each_entry(va, &free_vmap_area_list, list) {
		if (!is_within_this_va(va, size, align, vstart))
			continue;

		return va;
	}

	return NULL;
}

static void
find_vmap_lowest_match_check(unsigned long size)
{
	struct vmap_area *va_1, *va_2;
	unsigned long vstart;
	unsigned int rnd;

	get_random_bytes(&rnd, sizeof(rnd));
	vstart = VMALLOC_START + rnd;

	va_1 = find_vmap_lowest_match(size, 1, vstart);
	va_2 = find_vmap_lowest_linear_match(size, 1, vstart);

	if (va_1 != va_2)
		pr_emerg("not lowest: t: 0x%p, l: 0x%p, v: 0x%lx\n",
			va_1, va_2, vstart);
}
#endif

enum fit_type {
	NOTHING_FIT = 0,
	FL_FIT_TYPE = 1,	/* full fit */
	LE_FIT_TYPE = 2,	/* left edge fit */
	RE_FIT_TYPE = 3,	/* right edge fit */
	NE_FIT_TYPE = 4		/* no edge fit */
};

/*
 * IAMROOT, 2022.07.02: 
 * 찾은 빈 공간(va)에서 align이 적용되어 실제 할당하여 사용할 공간의 edge 
 * 접촉 여부를 알아온다.
 *
 * - FL_FIT_TYPE: 찾은 va의 영역을 모두 다 사용하는 경우
 * - LE_FIT_TYPE: 찾은 va 내에서 좌측 edge에 접해 할당할 수 있는 경우
 * - RE_FIT_TYPE: 찾은 va 내에서 우측 edge에 접해 할당할 수 있는 경우
 * - NE_FIT_TYPE: 찾은 va가 전체 공간의 양측 edge에 하나도 접해있지 않은 상태로
 *                할당해야 하는 경우 
 *
 * 에) NE_FIT_TYPE 
 *     align이 적용되어 nva_start_addr이 찾은 빈 공간의 va_start를 초과하였고,
 *     size 만큼 사용하므로 우측 공간도 남아 있는 상태이다.
 *                            <-----size------>
 *         +--------------+---------------------------+---------------+
 *         |      B       |   :        F      :       |       B       |
 *         +--------------+---------------------------+---------------+
 *                        ^   ^nva_start_addr         ^
 *                      va_start                    va_end
 *
 * 에) LE_FIT_TYPE 
 *     align이 적용되었어도 va_start == nva_start_addr 상태이고,
 *     size를 적용하여 우측 공간이 남아 있는 상태이다.
 *                        <-----size---------->
 *         +--------------+---------------------------+---------------+
 *         |      B       |            F      :       |       B       |
 *         +--------------+---------------------------+---------------+
 *                        ^                           ^
 *                      va_start                    va_end
 *                     nva_start_addr
 */
static __always_inline enum fit_type
classify_va_fit_type(struct vmap_area *va,
	unsigned long nva_start_addr, unsigned long size)
{
	enum fit_type type;

	/* Check if it is within VA. */
	if (nva_start_addr < va->va_start ||
			nva_start_addr + size > va->va_end)
		return NOTHING_FIT;

	/* Now classify. */
	if (va->va_start == nva_start_addr) {
		if (va->va_end == nva_start_addr + size)
			type = FL_FIT_TYPE;
		else
			type = LE_FIT_TYPE;
	} else if (va->va_end == nva_start_addr + size) {
		type = RE_FIT_TYPE;
	} else {
		type = NE_FIT_TYPE;
	}

	return type;
}

/*
 * IAMROOT, 2022.07.08:
 * - @type에 따라 free_vmap_area를 수정한다.
 */
static __always_inline int
adjust_va_to_fit_type(struct vmap_area *va,
	unsigned long nva_start_addr, unsigned long size,
	enum fit_type type)
{
	struct vmap_area *lva = NULL;

	if (type == FL_FIT_TYPE) {
		/*
		 * No need to split VA, it fully fits.
		 *
		 * |               |
		 * V      NVA      V
		 * |---------------|
		 */
/*
 * IAMROOT, 2022.07.02: 
 * 전체 공간을 사용해야 하므로 남은 공간에서 제거한다.
 */
		unlink_va(va, &free_vmap_area_root);
		kmem_cache_free(vmap_area_cachep, va);
	} else if (type == LE_FIT_TYPE) {
		/*
		 * Split left edge of fit VA.
		 *
		 * |       |
		 * V  NVA  V   R
		 * |-------|-------|
		 */
/*
 * IAMROOT, 2022.07.02: 
 * 남은 우측(R) 공간으로 free(va) 공간의 시작 위치를 변경한다.
 */
		va->va_start += size;
	} else if (type == RE_FIT_TYPE) {
		/*
		 * Split right edge of fit VA.
		 *
		 *         |       |
		 *     L   V  NVA  V
		 * |-------|-------|
		 */
/*
 * IAMROOT, 2022.07.02: 
 * 남은 좌측(L) 공간으로 free(va) 공간의 끝 위치를 변경한다.
 */
		va->va_end = nva_start_addr;
	} else if (type == NE_FIT_TYPE) {
		/*
		 * Split no edge of fit VA.
		 *
		 *     |       |
		 *   L V  NVA  V R
		 * |---|-------|---|
		 */
/*
 * IAMROOT, 2022.07.02: 
 * 남은 좌측(L) 및 우측(R) 공간으로 free(va) 공간의 시작과 끝 위치를 변경한다.
 * - preload되있던 va를 사용한다. 없으면 slap에서 할당받는다.
 */
		lva = __this_cpu_xchg(ne_fit_preload_node, NULL);
		if (unlikely(!lva)) {
			/*
			 * For percpu allocator we do not do any pre-allocation
			 * and leave it as it is. The reason is it most likely
			 * never ends up with NE_FIT_TYPE splitting. In case of
			 * percpu allocations offsets and sizes are aligned to
			 * fixed align request, i.e. RE_FIT_TYPE and FL_FIT_TYPE
			 * are its main fitting cases.
			 *
			 * There are a few exceptions though, as an example it is
			 * a first allocation (early boot up) when we have "one"
			 * big free space that has to be split.
			 *
			 * Also we can hit this path in case of regular "vmap"
			 * allocations, if "this" current CPU was not preloaded.
			 * See the comment in alloc_vmap_area() why. If so, then
			 * GFP_NOWAIT is used instead to get an extra object for
			 * split purpose. That is rare and most time does not
			 * occur.
			 *
			 * What happens if an allocation gets failed. Basically,
			 * an "overflow" path is triggered to purge lazily freed
			 * areas to free some memory, then, the "retry" path is
			 * triggered to repeat one more time. See more details
			 * in alloc_vmap_area() function.
			 */
/*
 * IAMROOT, 2022.07.08:
 * - papgo
 *   percpu 할당자의 경우 사전 할당을 수행하지 않고 그대로 둡니다. 그 이유는
 *   NE_FIT_TYPE 분할로 끝나지 않을 가능성이 높기 때문입니다. percpu 할당의
 *   경우 오프셋과 크기가 고정 정렬 요청에 맞춰 정렬됩니다. 즉, RE_FIT_TYPE 및
 *   FL_FIT_TYPE이 주요 피팅 케이스입니다.
 *
 *   그러나 몇 가지 예외가 있습니다. 예를 들어 분할해야 하는 하나의 큰 여유
 *   공간이 있을 때 첫 번째 할당(조기 부팅)입니다.
 *
 *   또한 이 현재 CPU가 사전 로드되지 않은 경우 일반 vmap 할당의 경우 이
 *   경로에 도달할 수 있습니다. 
 *   alloc_vmap_area()의 주석을 참조하십시오. 이유. 그렇다면 GFP_NOWAIT를
 *   사용하여 분할 목적으로 추가 개체를 가져옵니다. 그것은 드물고 대부분의
 *   시간이 발생하지 않습니다.
 *
 *   할당이 실패하면 어떻게 됩니까? 기본적으로 오버플로 경로가 트리거되어
 *   지연 해제된 영역을 제거하여 일부 메모리를 해제한 다음 재시도 경로가
 *   트리거되어 한 번 더 반복됩니다. alloc_vmap_area() 함수에서 자세한 내용을
 *   참조하십시오.
 */
			lva = kmem_cache_alloc(vmap_area_cachep, GFP_NOWAIT);
			if (!lva)
				return -1;
		}

		/*
		 * Build the remainder.
		 */
		lva->va_start = va->va_start;
		lva->va_end = nva_start_addr;

		/*
		 * Shrink this VA to remaining size.
		 */
		va->va_start = nva_start_addr + size;
	} else {
		return -1;
	}

/*
 * IAMROOT, 2022.07.02: 
 * 빈 공간의 사이즈 정보가 달라진 경우 상위 노드로 전파를 한다.
 * 그리고, 좌 우측으로 갈라진 경우 좌측의 빈 공간을 자료구조에 추가한다.
 */
	if (type != FL_FIT_TYPE) {
		augment_tree_propagate_from(va);

		if (lva)	/* type == NE_FIT_TYPE */
			insert_vmap_area_augment(lva, &va->rb_node,
				&free_vmap_area_root, &free_vmap_area_list);
	}

	return 0;
}

/*
 * Returns a start address of the newly allocated area, if success.
 * Otherwise a vend is returned that indicates failure.
 */
/*
 * IAMROOT, 2022.07.02: 
 * @size 및 @align이 확보되는 @vstart ~ @vend 내(보통 vmalloc 공간)의 
 * 빈 공간을 아래부터 탐색하고, @align에 맞춘 시작 주소를 반환한다.
 * 만일 실패하면 @vend가 반환된다.
 */
static __always_inline unsigned long
__alloc_vmap_area(unsigned long size, unsigned long align,
	unsigned long vstart, unsigned long vend)
{
	unsigned long nva_start_addr;
	struct vmap_area *va;
	enum fit_type type;
	int ret;

/*
 * IAMROOT, 2022.07.02: 
 * @size와 @align이 확보되는 vmalloc의 빈 공간을 아래부터 탐색하여 알아온다.
 */
	va = find_vmap_lowest_match(size, align, vstart);
	if (unlikely(!va))
		return vend;

	if (va->va_start > vstart)
		nva_start_addr = ALIGN(va->va_start, align);
	else
		nva_start_addr = ALIGN(vstart, align);

	/* Check the "vend" restriction. */
	if (nva_start_addr + size > vend)
		return vend;

/*
 * IAMROOT, 2022.07.02: 
 * 찾은 free 공간 va의 edge 영역에 할당할 영역이 접하는 타입을 알아온다.
 */
	/* Classify what we have found. */
	type = classify_va_fit_type(va, nva_start_addr, size);
	if (WARN_ON_ONCE(type == NOTHING_FIT))
		return vend;

	/* Update the free vmap_area. */
	ret = adjust_va_to_fit_type(va, nva_start_addr, size, type);
	if (ret)
		return vend;

#if DEBUG_AUGMENT_LOWEST_MATCH_CHECK
	find_vmap_lowest_match_check(size);
#endif

	return nva_start_addr;
}

/*
 * Free a region of KVA allocated by alloc_vmap_area
 */
static void free_vmap_area(struct vmap_area *va)
{
	/*
	 * Remove from the busy tree/list.
	 */
	spin_lock(&vmap_area_lock);
	unlink_va(va, &vmap_area_root);
	spin_unlock(&vmap_area_lock);

	/*
	 * Insert/Merge it back to the free tree/list.
	 */
	spin_lock(&free_vmap_area_lock);
	merge_or_add_vmap_area_augment(va, &free_vmap_area_root, &free_vmap_area_list);
	spin_unlock(&free_vmap_area_lock);
}

static inline void
preload_this_cpu_lock(spinlock_t *lock, gfp_t gfp_mask, int node)
{
	struct vmap_area *va = NULL;

	/*
	 * Preload this CPU with one extra vmap_area object. It is used
	 * when fit type of free area is NE_FIT_TYPE. It guarantees that
	 * a CPU that does an allocation is preloaded.
	 *
	 * We do it in non-atomic context, thus it allows us to use more
	 * permissive allocation masks to be more stable under low memory
	 * condition and high memory pressure.
	 */
	if (!this_cpu_read(ne_fit_preload_node))
		va = kmem_cache_alloc_node(vmap_area_cachep, gfp_mask, node);

	spin_lock(lock);

	if (va && __this_cpu_cmpxchg(ne_fit_preload_node, NULL, va))
		kmem_cache_free(vmap_area_cachep, va);
}

/*
 * Allocate a region of KVA of the specified size and alignment, within the
 * vstart and vend.
 */
/*
 * IAMROOT, 2022.07.02: 
 * @vstart ~ @vend 영역내에서 빈 공간을 아래부터 탐색하여 찾은 후 @align에 
 * 맞춘 주소로 vmap_area 정보를 할당하여 채운 후 반환한다.
 */
static struct vmap_area *alloc_vmap_area(unsigned long size,
				unsigned long align,
				unsigned long vstart, unsigned long vend,
				int node, gfp_t gfp_mask)
{
	struct vmap_area *va;
	unsigned long freed;
	unsigned long addr;
	int purged = 0;
	int ret;

	BUG_ON(!size);
	BUG_ON(offset_in_page(size));
	BUG_ON(!is_power_of_2(align));

/*
 * IAMROOT, 2022.07.02: 
 * vmalloc_init()이 완료되지 않은 시점에 이 함수를 요청하면 -EBUSY 에러를 반환.
 */
	if (unlikely(!vmap_initialized))
		return ERR_PTR(-EBUSY);

/*
 * IAMROOT, 2022.07.02: 
 * vmalloc() 및 vmap() API는 sleepable로 운영된다. 따라서 reclaim을 허용한다.
 */
	might_sleep();
	gfp_mask = gfp_mask & GFP_RECLAIM_MASK;

	va = kmem_cache_alloc_node(vmap_area_cachep, gfp_mask, node);
	if (unlikely(!va))
		return ERR_PTR(-ENOMEM);

	/*
	 * Only scan the relevant parts containing pointers to other objects
	 * to avoid false negatives.
	 */
	kmemleak_scan_area(&va->rb_node, SIZE_MAX, gfp_mask);

retry:
/*
 * IAMROOT, 2022.07.02: 
 * align 및 size 정보로 빈 공간을 찾아 align에 정렬한 주소 addr을 알아온다.
 */
	preload_this_cpu_lock(&free_vmap_area_lock, gfp_mask, node);
	addr = __alloc_vmap_area(size, align, vstart, vend);
	spin_unlock(&free_vmap_area_lock);

	/*
	 * If an allocation fails, the "vend" address is
	 * returned. Therefore trigger the overflow path.
	 */
	if (unlikely(addr == vend))
		goto overflow;

/*
 * IAMROOT, 2022.07.02: 
 * 알아온 시작주소 addr을 사용하여 va를 구성후 vmalloc(vmap) 자료 구조에 추가
 */
	va->va_start = addr;
	va->va_end = addr + size;
	va->vm = NULL;

	spin_lock(&vmap_area_lock);
	insert_vmap_area(va, &vmap_area_root, &vmap_area_list);
	spin_unlock(&vmap_area_lock);

	BUG_ON(!IS_ALIGNED(va->va_start, align));
	BUG_ON(va->va_start < vstart);
	BUG_ON(va->va_end > vend);

	ret = kasan_populate_vmalloc(addr, size);
	if (ret) {
		free_vmap_area(va);
		return ERR_PTR(ret);
	}

	return va;

overflow:
	if (!purged) {
		purge_vmap_area_lazy();
		purged = 1;
		goto retry;
	}

	freed = 0;
	blocking_notifier_call_chain(&vmap_notify_list, 0, &freed);

	if (freed > 0) {
		purged = 0;
		goto retry;
	}

	if (!(gfp_mask & __GFP_NOWARN) && printk_ratelimit())
		pr_warn("vmap allocation for size %lu failed: use vmalloc=<size> to increase size\n",
			size);

	kmem_cache_free(vmap_area_cachep, va);
	return ERR_PTR(-EBUSY);
}

int register_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_vmap_purge_notifier);

int unregister_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_vmap_purge_notifier);

/*
 * lazy_max_pages is the maximum amount of virtual address space we gather up
 * before attempting to purge with a TLB flush.
 *
 * There is a tradeoff here: a larger number will cover more kernel page tables
 * and take slightly longer to purge, but it will linearly reduce the number of
 * global TLB flushes that must be performed. It would seem natural to scale
 * this number up linearly with the number of CPUs (because vmapping activity
 * could also scale linearly with the number of CPUs), however it is likely
 * that in practice, workloads might be constrained in other ways that mean
 * vmap activity will not scale linearly with CPUs. Also, I want to be
 * conservative and not introduce a big latency on huge systems, so go with
 * a less aggressive log scale. It will still be an improvement over the old
 * code, and it will be simple to change the scale factor if we find that it
 * becomes a problem on bigger systems.
 */
/*
 * IAMROOT, 2022.07.02: 
 * = log2(online cpu + 1) * 32M에 해당하는 페이지 수
 *
 * 예) 4 cpus
 *     3 * 32M에 해당하는 page 수
 */
static unsigned long lazy_max_pages(void)
{
	unsigned int log;

	log = fls(num_online_cpus());

	return log * (32UL * 1024 * 1024 / PAGE_SIZE);
}

static atomic_long_t vmap_lazy_nr = ATOMIC_LONG_INIT(0);

/*
 * Serialize vmap purging.  There is no actual critical section protected
 * by this look, but we want to avoid concurrent calls for performance
 * reasons and to make the pcpu_get_vm_areas more deterministic.
 */
static DEFINE_MUTEX(vmap_purge_lock);

/* for per-CPU blocks */
static void purge_fragmented_blocks_allcpus(void);

#ifdef CONFIG_X86_64
/*
 * called before a call to iounmap() if the caller wants vm_area_struct's
 * immediately freed.
 */
void set_iounmap_nonlazy(void)
{
	atomic_long_set(&vmap_lazy_nr, lazy_max_pages()+1);
}
#endif /* CONFIG_X86_64 */

/*
 * Purges all lazily-freed vmap areas.
 */
/*
 * IAMROOT, 2022.07.02: 
 * lazy free 상태의 vmap area들을 모두 vmalloc 자료 구조의 free 영역을 
 * 관리하는 자료 구조(RB tree + list)로 옮기고 해당 영역에 대해 
 * 모든 inner-share cpu들에 대해 TLB flush를 수행한다.
 *
 * @return false : purge_vmap_area_list가 없는 경우
 *         true  : tlb flush + purge를 수행한경우.
 */
static bool __purge_vmap_area_lazy(unsigned long start, unsigned long end)
{
	unsigned long resched_threshold;
	struct list_head local_pure_list;
	struct vmap_area *va, *n_va;

	lockdep_assert_held(&vmap_purge_lock);

/*
 * IAMROOT, 2022.07.02: 
 * purge용 자료 구조 rb tree에는 RB_ROOT로 초기화하고,
 * list의 내용은 local로 옮긴다.
 */
	spin_lock(&purge_vmap_area_lock);
	purge_vmap_area_root = RB_ROOT;
	list_replace_init(&purge_vmap_area_list, &local_pure_list);
	spin_unlock(&purge_vmap_area_lock);

	if (unlikely(list_empty(&local_pure_list)))
		return false;

/*
 * IAMROOT, 2022.07.02: 
 * purge 리스트의 첫 엔트리 시작 주소 부터 마지막 엔트리 끝 주소까지
 * tlb flush를 진행한다.
 */
	start = min(start,
		list_first_entry(&local_pure_list,
			struct vmap_area, list)->va_start);

	end = max(end,
		list_last_entry(&local_pure_list,
			struct vmap_area, list)->va_end);

/*
 * IAMROOT, 2022.07.02: 
 * start ~ end 범위까지 모든 inner-share core들의 TLB 캐시를 flush 한다.
 */
	flush_tlb_kernel_range(start, end);
	resched_threshold = lazy_max_pages() << 1;

/*
 * IAMROOT, 2022.07.02: 
 * 기존에 사용 중인 area들을 모두 local로 담아왔었는데, 이 번에는 이들을 모두
 * free 영역의 자료 구조로 추가한다.
 */
	spin_lock(&free_vmap_area_lock);
	list_for_each_entry_safe(va, n_va, &local_pure_list, list) {
		unsigned long nr = (va->va_end - va->va_start) >> PAGE_SHIFT;
		unsigned long orig_start = va->va_start;
		unsigned long orig_end = va->va_end;

		/*
		 * Finally insert or merge lazily-freed area. It is
		 * detached and there is no need to "unlink" it from
		 * anything.
		 */
		va = merge_or_add_vmap_area_augment(va, &free_vmap_area_root,
				&free_vmap_area_list);

		if (!va)
			continue;

		if (is_vmalloc_or_module_addr((void *)orig_start))
			kasan_release_vmalloc(orig_start, orig_end,
					      va->va_start, va->va_end);

		atomic_long_sub(nr, &vmap_lazy_nr);

		if (atomic_long_read(&vmap_lazy_nr) < resched_threshold)
			cond_resched_lock(&free_vmap_area_lock);
	}
	spin_unlock(&free_vmap_area_lock);
	return true;
}

/*
 * Kick off a purge of the outstanding lazy areas. Don't bother if somebody
 * is already purging.
 */
static void try_purge_vmap_area_lazy(void)
{
	if (mutex_trylock(&vmap_purge_lock)) {
		__purge_vmap_area_lazy(ULONG_MAX, 0);
		mutex_unlock(&vmap_purge_lock);
	}
}

/*
 * Kick off a purge of the outstanding lazy areas.
 */
/*
 * IAMROOT, 2022.07.02: 
 * lazy free 상태의 vmap area들을 모두 vmalloc 자료 구조의 free 영역을 
 * 관리하는 자료 구조(RB tree + list)로 옮기고 해당 영역에 대해 
 * 모든 inner-share cpu들에 대해 TLB flush를 수행한다.
 */
static void purge_vmap_area_lazy(void)
{
	mutex_lock(&vmap_purge_lock);
	purge_fragmented_blocks_allcpus();
	__purge_vmap_area_lazy(ULONG_MAX, 0);
	mutex_unlock(&vmap_purge_lock);
}

/*
 * Free a vmap area, caller ensuring that the area has been unmapped
 * and flush_cache_vunmap had been called for the correct range
 * previously.
 */
/*
 * IAMROOT, 2022.07.02: 
 * vmalloc 자료 구조에서 vmap_area 구조체를 unlink하고, 
 * purge용 rb tree와 list에 추가한다.
 * (이때 purge에 연속된 va가 있다면 해당 va와 merge된다.)
 * 단 일정 수 이상의 lazy된 페이지들인 경우 한꺼번에 TLB flush 처리한다.
 */
static void free_vmap_area_noflush(struct vmap_area *va)
{
	unsigned long nr_lazy;

	spin_lock(&vmap_area_lock);
	unlink_va(va, &vmap_area_root);
	spin_unlock(&vmap_area_lock);

	nr_lazy = atomic_long_add_return((va->va_end - va->va_start) >>
				PAGE_SHIFT, &vmap_lazy_nr);

	/*
	 * Merge or place it to the purge tree/list.
	 */
	spin_lock(&purge_vmap_area_lock);
	merge_or_add_vmap_area(va,
		&purge_vmap_area_root, &purge_vmap_area_list);
	spin_unlock(&purge_vmap_area_lock);

	/* After this point, we may free va at any time */
/*
 * IAMROOT, 2022.07.02: 
 * 적절한 용량을 초과하는 nr_lazy 수인 경우 해당 vmap area들을 free 자료 구조로
 * 이동시키고, inner-share core들을 대상으로 lazy된 TLB flush를 수행한다.
 */
	if (unlikely(nr_lazy > lazy_max_pages()))
		try_purge_vmap_area_lazy();
}

/*
 * Free and unmap a vmap area
 */
static void free_unmap_vmap_area(struct vmap_area *va)
{
/*
 * IAMROOT, 2022.07.02: 
 * ARM64의 경우 아래 API는 아무일도 하지 않는다.
 */
	flush_cache_vunmap(va->va_start, va->va_end);
/*
 * IAMROOT, 2022.07.02: 
 * 해당 영역을 모두 페이지 테이블에서 언매핑한다.
 */
	vunmap_range_noflush(va->va_start, va->va_end);
	if (debug_pagealloc_enabled_static())
		flush_tlb_kernel_range(va->va_start, va->va_end);

/*
 * IAMROOT, 2022.07.02: 
 * vmalloc 자료 구조에서 vmap_area 구조체를 unlink하고, 
 * purge용 rb tree와 list에 추가한다.
 * 단 일정 수 이상의 lazy된 페이지들인 경우 한꺼번에 TLB flush 처리한다.
 */
	free_vmap_area_noflush(va);
}

/*
 * iamroot, 2022.07.02: 
 * vmalloc 자료구조에서 @addr에 해당하는 vmap_area를 찾는다.
 * 못 잧은 경우 null을 반환한다.
 */
static struct vmap_area *find_vmap_area(unsigned long addr)
{
	struct vmap_area *va;

	spin_lock(&vmap_area_lock);
	va = __find_vmap_area(addr);
	spin_unlock(&vmap_area_lock);

	return va;
}

/*** Per cpu kva allocator ***/

/*
 * vmap space is limited especially on 32 bit architectures. Ensure there is
 * room for at least 16 percpu vmap blocks per CPU.
 */
/*
 * IAMROOT, 2023.02.03:
 * - papago
 *   vmap 공간은 특히 32비트 아키텍처에서 제한됩니다. CPU당 최소 16개의 
 *   percpu vmap 블록을 위한 공간이 있는지 확인합니다.
 */
/*
 * If we had a constant VMALLOC_START and VMALLOC_END, we'd like to be able
 * to #define VMALLOC_SPACE		(VMALLOC_END-VMALLOC_START). Guess
 * instead (we just need a rough idea)
 */
/*
 * IAMROOT, 2023.02.03:
 * - papago
 *   상수 VMALLOC_START 및 VMALLOC_END가 있는 경우 
 *   VMALLOC_SPACE(VMALLOC_END-VMALLOC_START)를 #define할 수 있기를 바랍니다.
 *   대신 추측해 보세요(대략적인 아이디어만 있으면 됩니다). 
 */
#if BITS_PER_LONG == 32
#define VMALLOC_SPACE		(128UL*1024*1024)
#else
#define VMALLOC_SPACE		(128UL*1024*1024*1024)
#endif

/*
 * IAMROOT, 2023.02.03:
 * - 64bit 4kpage 의 경우 32 * 1024 * 1024
 */
#define VMALLOC_PAGES		(VMALLOC_SPACE / PAGE_SIZE)
#define VMAP_MAX_ALLOC		BITS_PER_LONG	/* 256K with 4K pages */
#define VMAP_BBMAP_BITS_MAX	1024	/* 4MB with 4K pages */
#define VMAP_BBMAP_BITS_MIN	(VMAP_MAX_ALLOC*2)
#define VMAP_MIN(x, y)		((x) < (y) ? (x) : (y)) /* can't use min() */
#define VMAP_MAX(x, y)		((x) > (y) ? (x) : (y)) /* can't use max() */
/*
 * IAMROOT, 2023.02.03:
 * - 128 ~ 1024
 *   VMALLOC_PAGES / 16 = 2 * 1024 * 1024
 *   NR_CPUS  VMAP_BBMAP_BITS
 *   <= 2048  1024 
 *   <= 4096  512
 *   <= 8192  256
 *   > 8192   128
 */
#define VMAP_BBMAP_BITS		\
		VMAP_MIN(VMAP_BBMAP_BITS_MAX,	\
		VMAP_MAX(VMAP_BBMAP_BITS_MIN,	\
			VMALLOC_PAGES / roundup_pow_of_two(NR_CPUS) / 16))

#define VMAP_BLOCK_SIZE		(VMAP_BBMAP_BITS * PAGE_SIZE)

struct vmap_block_queue {
	spinlock_t lock;
	struct list_head free;
};

struct vmap_block {
	spinlock_t lock;
	struct vmap_area *va;
	unsigned long free, dirty;
	unsigned long dirty_min, dirty_max; /*< dirty range */
	struct list_head free_list;
	struct rcu_head rcu_head;
	struct list_head purge;
};

/* Queue of free and dirty vmap blocks, for allocation and flushing purposes */
static DEFINE_PER_CPU(struct vmap_block_queue, vmap_block_queue);

/*
 * XArray of vmap blocks, indexed by address, to quickly find a vmap block
 * in the free path. Could get rid of this if we change the API to return a
 * "cookie" from alloc, to be passed to free. But no big deal yet.
 */
static DEFINE_XARRAY(vmap_blocks);

/*
 * We should probably have a fallback mechanism to allocate virtual memory
 * out of partially filled vmap blocks. However vmap block sizing should be
 * fairly reasonable according to the vmalloc size, so it shouldn't be a
 * big problem.
 */

static unsigned long addr_to_vb_idx(unsigned long addr)
{
	addr -= VMALLOC_START & ~(VMAP_BLOCK_SIZE-1);
	addr /= VMAP_BLOCK_SIZE;
	return addr;
}

static void *vmap_block_vaddr(unsigned long va_start, unsigned long pages_off)
{
	unsigned long addr;

	addr = va_start + (pages_off << PAGE_SHIFT);
	BUG_ON(addr_to_vb_idx(addr) != addr_to_vb_idx(va_start));
	return (void *)addr;
}

/**
 * new_vmap_block - allocates new vmap_block and occupies 2^order pages in this
 *                  block. Of course pages number can't exceed VMAP_BBMAP_BITS
 * @order:    how many 2^order pages should be occupied in newly allocated block
 * @gfp_mask: flags for the page level allocator
 *
 * Return: virtual address in a newly allocated block or ERR_PTR(-errno)
 */
static void *new_vmap_block(unsigned int order, gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	struct vmap_area *va;
	unsigned long vb_idx;
	int node, err;
	void *vaddr;

	node = numa_node_id();

	vb = kmalloc_node(sizeof(struct vmap_block),
			gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!vb))
		return ERR_PTR(-ENOMEM);

	va = alloc_vmap_area(VMAP_BLOCK_SIZE, VMAP_BLOCK_SIZE,
					VMALLOC_START, VMALLOC_END,
					node, gfp_mask);
	if (IS_ERR(va)) {
		kfree(vb);
		return ERR_CAST(va);
	}

	vaddr = vmap_block_vaddr(va->va_start, 0);
	spin_lock_init(&vb->lock);
	vb->va = va;
	/* At least something should be left free */
	BUG_ON(VMAP_BBMAP_BITS <= (1UL << order));
	vb->free = VMAP_BBMAP_BITS - (1UL << order);
	vb->dirty = 0;
	vb->dirty_min = VMAP_BBMAP_BITS;
	vb->dirty_max = 0;
	INIT_LIST_HEAD(&vb->free_list);

	vb_idx = addr_to_vb_idx(va->va_start);
	err = xa_insert(&vmap_blocks, vb_idx, vb, gfp_mask);
	if (err) {
		kfree(vb);
		free_vmap_area(va);
		return ERR_PTR(err);
	}

	vbq = &get_cpu_var(vmap_block_queue);
	spin_lock(&vbq->lock);
	list_add_tail_rcu(&vb->free_list, &vbq->free);
	spin_unlock(&vbq->lock);
	put_cpu_var(vmap_block_queue);

	return vaddr;
}

/*
 * IAMROOT, 2023.02.03:
 * 1. vmap_blocks에서 @vb 제거
 * 2. vb의 va를 vmap_area_root에서 삭제하고 purge_vmap_area_root에 넣어놓는다.
 *    (lazy 시킨다.) lazy된 va가 많을경우 purge를 진행한다.
 */
static void free_vmap_block(struct vmap_block *vb)
{
	struct vmap_block *tmp;

	tmp = xa_erase(&vmap_blocks, addr_to_vb_idx(vb->va->va_start));
	BUG_ON(tmp != vb);

	free_vmap_area_noflush(vb->va);
	kfree_rcu(vb, rcu_head);
}

/*
 * IAMROOT, 2023.02.03:
 * - fully frag된것을 찾아서 purge한다.
 *   일단 fully frag를 찾아서 local purge list에 옮기고, purge를 수행하는
 *   식으로 한다.
 */
static void purge_fragmented_blocks(int cpu)
{
	LIST_HEAD(purge);
	struct vmap_block *vb;
	struct vmap_block *n_vb;
	struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);

	rcu_read_lock();
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {

/*
 * IAMROOT, 2023.02.03:
 * - fully fragemented block만 찾는다. fully frag가 아니라면 continue
 */
		if (!(vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS))
			continue;

		spin_lock(&vb->lock);
/*
 * IAMROOT, 2023.02.03:
 * - free_list에서 purge로 옮긴다.
 *   spin_lock을 거는 사이테 free, dirty가 변경될수있으므로 다시한번 if조건
 *   이 있는것처럼 보인다.
 */
		if (vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS) {
			vb->free = 0; /* prevent further allocs after releasing lock */
			vb->dirty = VMAP_BBMAP_BITS; /* prevent purging it again */
			vb->dirty_min = 0;
			vb->dirty_max = VMAP_BBMAP_BITS;
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
			spin_unlock(&vb->lock);
			list_add_tail(&vb->purge, &purge);
		} else
			spin_unlock(&vb->lock);
	}
	rcu_read_unlock();

	list_for_each_entry_safe(vb, n_vb, &purge, purge) {
		list_del(&vb->purge);
		free_vmap_block(vb);
	}
}

/*
 * IAMROOT, 2023.02.17:
 * - 모든 cpu에 대해서 purge를 진행한다.
 */
static void purge_fragmented_blocks_allcpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		purge_fragmented_blocks(cpu);
}

static void *vb_alloc(unsigned long size, gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	void *vaddr = NULL;
	unsigned int order;

	BUG_ON(offset_in_page(size));
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);
	if (WARN_ON(size == 0)) {
		/*
		 * Allocating 0 bytes isn't what caller wants since
		 * get_order(0) returns funny result. Just warn and terminate
		 * early.
		 */
		return NULL;
	}
	order = get_order(size);

	rcu_read_lock();
	vbq = &get_cpu_var(vmap_block_queue);
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {
		unsigned long pages_off;

		spin_lock(&vb->lock);
		if (vb->free < (1UL << order)) {
			spin_unlock(&vb->lock);
			continue;
		}

		pages_off = VMAP_BBMAP_BITS - vb->free;
		vaddr = vmap_block_vaddr(vb->va->va_start, pages_off);
		vb->free -= 1UL << order;
		if (vb->free == 0) {
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
		}

		spin_unlock(&vb->lock);
		break;
	}

	put_cpu_var(vmap_block_queue);
	rcu_read_unlock();

	/* Allocate new block if nothing was found */
	if (!vaddr)
		vaddr = new_vmap_block(order, gfp_mask);

	return vaddr;
}

static void vb_free(unsigned long addr, unsigned long size)
{
	unsigned long offset;
	unsigned int order;
	struct vmap_block *vb;

	BUG_ON(offset_in_page(size));
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);

	flush_cache_vunmap(addr, addr + size);

	order = get_order(size);
	offset = (addr & (VMAP_BLOCK_SIZE - 1)) >> PAGE_SHIFT;
	vb = xa_load(&vmap_blocks, addr_to_vb_idx(addr));

	vunmap_range_noflush(addr, addr + size);

	if (debug_pagealloc_enabled_static())
		flush_tlb_kernel_range(addr, addr + size);

	spin_lock(&vb->lock);

	/* Expand dirty range */
	vb->dirty_min = min(vb->dirty_min, offset);
	vb->dirty_max = max(vb->dirty_max, offset + (1UL << order));

	vb->dirty += 1UL << order;
	if (vb->dirty == VMAP_BBMAP_BITS) {
		BUG_ON(vb->free);
		spin_unlock(&vb->lock);
		free_vmap_block(vb);
	} else
		spin_unlock(&vb->lock);
}

/*
 * IAMROOT, 2023.02.03:
 * --- chat openai ----
 * - unmmap을 하는 memory를 왜 굳이 tlb flush를 하는 이유
 *   메모리 매핑을 해제한 후 TLB를 플러시하는 이유는 오래된 항목이 제거되고 
 *   동일한 가상 주소에 대한 후속 액세스로 인해 올바른 물리적 주소 매핑으로 
 *   TLB 항목의 다시 로드를 트리거하는 페이지 오류가 발생하도록 하기 
 *   위함입니다. 이 작업을 수행하지 않으면 잘못된 물리적 메모리 위치를 
 *   가리키는 오래된 TLB 항목으로 인해 데이터 손상 또는 보안 취약성의 위험이 
 *   있습니다.
 * --------------------
 * - lazy aliases(vbq->free)에 대한 unmap + @start ~ @end 범위에 대한
 *   purge및 tlb flush를 수행한다.
 * - free list에서 dirty값이 있는 경우 start, end를 갱신하며 flush를 set한다.
 *   frag block들에 대해서 purge를 수행하고, 수행후
 *   purge_vmap_area_list가 비엇고 flush set이라면 flush_tlb_kernel_range를
 *   수행한다.
 */
static void _vm_unmap_aliases(unsigned long start, unsigned long end, int flush)
{
	int cpu;

	if (unlikely(!vmap_initialized))
		return;

	might_sleep();

	for_each_possible_cpu(cpu) {
		struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);
		struct vmap_block *vb;

		rcu_read_lock();
		list_for_each_entry_rcu(vb, &vbq->free, free_list) {
			spin_lock(&vb->lock);
			if (vb->dirty && vb->dirty != VMAP_BBMAP_BITS) {
				unsigned long va_start = vb->va->va_start;
				unsigned long s, e;

				s = va_start + (vb->dirty_min << PAGE_SHIFT);
				e = va_start + (vb->dirty_max << PAGE_SHIFT);

				start = min(s, start);
				end   = max(e, end);

				flush = 1;
			}
			spin_unlock(&vb->lock);
		}
		rcu_read_unlock();
	}

	mutex_lock(&vmap_purge_lock);
	purge_fragmented_blocks_allcpus();
/*
 * IAMROOT, 2023.02.17:
 * - purge + tlb flush를 시도한다.
 * - purge를 안했는데 , flush 요청이 있었다면 flush만을 해준다.
 *   (purge를 했을경우 tlb flush도 하기 때문에 flush요청을 무시해도된다.)
 */
	if (!__purge_vmap_area_lazy(start, end) && flush)
		flush_tlb_kernel_range(start, end);
	mutex_unlock(&vmap_purge_lock);
}

/**
 * vm_unmap_aliases - unmap outstanding lazy aliases in the vmap layer
 *
 * The vmap/vmalloc layer lazily flushes kernel virtual mappings primarily
 * to amortize TLB flushing overheads. What this means is that any page you
 * have now, may, in a former life, have been mapped into kernel virtual
 * address by the vmap layer and so there might be some CPUs with TLB entries
 * still referencing that page (additional to the regular 1:1 kernel mapping).
 *
 * vm_unmap_aliases flushes all such lazy mappings. After it returns, we can
 * be sure that none of the pages we have control over will have any aliases
 * from the vmap layer.
 */
/*
 * IAMROOT, 2023.02.03:
 * - papago
 *   vm_unmap_filename - vmap 계층에서 미결 지연 별칭을 매핑 해제합니다.
 *
 *   vmap/vmalloc 계층은 주로 TLB 플러시 오버헤드를 상각하기 위해 커널 가상 
 *   매핑을 느리게 플러시합니다. 이것은 당신이 현재 가지고 있는 페이지가 전생에 
 *   vmap 계층에 의해 커널 가상 주소로 매핑되었을 수 있으며, 따라서 여전히 그 
 *   페이지를 참조하는 TLB 항목이 있는 CPU가 있을 수 있다는 것을 
 *   의미한다(일반적인 1:1 커널 매핑에 추가).
 *
 *   vm_unmap_mapping은 이러한 모든 느린 매핑을 플러시합니다. 반환된 후에는 
 *   제어할 수 있는 페이지 중 vmap 계층의 별칭을 가진 페이지가 없는지 확인할 
 *   수 있습니다.
 *
 * - 모든 vm영역에 대한 lazy aliases의 ummap을 수행한다.
 *   한개도 unmap한개 없다면 tlb flush도 안하겠지만, 그렇지 않을 경우 purge를 
 *   하고 tlb flush를 수행할것이다.
 */
void vm_unmap_aliases(void)
{
	unsigned long start = ULONG_MAX, end = 0;
	int flush = 0;

	_vm_unmap_aliases(start, end, flush);
}
EXPORT_SYMBOL_GPL(vm_unmap_aliases);

/**
 * vm_unmap_ram - unmap linear kernel address space set up by vm_map_ram
 * @mem: the pointer returned by vm_map_ram
 * @count: the count passed to that vm_map_ram call (cannot unmap partial)
 */
void vm_unmap_ram(const void *mem, unsigned int count)
{
	unsigned long size = (unsigned long)count << PAGE_SHIFT;
	unsigned long addr = (unsigned long)mem;
	struct vmap_area *va;

	might_sleep();
	BUG_ON(!addr);
	BUG_ON(addr < VMALLOC_START);
	BUG_ON(addr > VMALLOC_END);
	BUG_ON(!PAGE_ALIGNED(addr));

	kasan_poison_vmalloc(mem, size);

	if (likely(count <= VMAP_MAX_ALLOC)) {
		debug_check_no_locks_freed(mem, size);
		vb_free(addr, size);
		return;
	}

	va = find_vmap_area(addr);
	BUG_ON(!va);
	debug_check_no_locks_freed((void *)va->va_start,
				    (va->va_end - va->va_start));
	free_unmap_vmap_area(va);
}
EXPORT_SYMBOL(vm_unmap_ram);

/**
 * vm_map_ram - map pages linearly into kernel virtual address (vmalloc space)
 * @pages: an array of pointers to the pages to be mapped
 * @count: number of pages
 * @node: prefer to allocate data structures on this node
 *
 * If you use this function for less than VMAP_MAX_ALLOC pages, it could be
 * faster than vmap so it's good.  But if you mix long-life and short-life
 * objects with vm_map_ram(), it could consume lots of address space through
 * fragmentation (especially on a 32bit machine).  You could see failures in
 * the end.  Please use this function for short-lived objects.
 *
 * Returns: a pointer to the address that has been mapped, or %NULL on failure
 */
void *vm_map_ram(struct page **pages, unsigned int count, int node)
{
	unsigned long size = (unsigned long)count << PAGE_SHIFT;
	unsigned long addr;
	void *mem;

	if (likely(count <= VMAP_MAX_ALLOC)) {
		mem = vb_alloc(size, GFP_KERNEL);
		if (IS_ERR(mem))
			return NULL;
		addr = (unsigned long)mem;
	} else {
		struct vmap_area *va;
		va = alloc_vmap_area(size, PAGE_SIZE,
				VMALLOC_START, VMALLOC_END, node, GFP_KERNEL);
		if (IS_ERR(va))
			return NULL;

		addr = va->va_start;
		mem = (void *)addr;
	}

	kasan_unpoison_vmalloc(mem, size);

	if (vmap_pages_range(addr, addr + size, PAGE_KERNEL,
				pages, PAGE_SHIFT) < 0) {
		vm_unmap_ram(mem, count);
		return NULL;
	}

	return mem;
}
EXPORT_SYMBOL(vm_map_ram);

static struct vm_struct *vmlist __initdata;

static inline unsigned int vm_area_page_order(struct vm_struct *vm)
{
#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
	return vm->page_order;
#else
	return 0;
#endif
}

static inline void set_vm_area_page_order(struct vm_struct *vm, unsigned int order)
{
#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
	vm->page_order = order;
#else
	BUG_ON(order != 0);
#endif
}

/**
 * vm_area_add_early - add vmap area early during boot
 * @vm: vm_struct to add
 *
 * This function is used to add fixed kernel vm area to vmlist before
 * vmalloc_init() is called.  @vm->addr, @vm->size, and @vm->flags
 * should contain proper values and the other fields should be zero.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
/* IAMROOT, 2021.10.30:
 * - vmap_instalized == false (vmalloc(vmap)을 사용할 수 없는 상태) 에서
 *   mapping 한 영역이 있는 경우 임시로 vmlist를 사용하여 @vm을 관리하며
 *   추후에 vmalloc_init(..) 함수가 호출되면 vmalloc 관리 영역에 추가된다.
 *
 *   여기서는 단순히 linked-list로 vmlist를 관리한다.
 */
void __init vm_area_add_early(struct vm_struct *vm)
{
	struct vm_struct *tmp, **p;

	BUG_ON(vmap_initialized);
	for (p = &vmlist; (tmp = *p) != NULL; p = &tmp->next) {
		if (tmp->addr >= vm->addr) {
			BUG_ON(tmp->addr < vm->addr + vm->size);
			break;
		} else
			BUG_ON(tmp->addr + tmp->size > vm->addr);
	}
	vm->next = *p;
	*p = vm;
}

/**
 * vm_area_register_early - register vmap area early during boot
 * @vm: vm_struct to register
 * @align: requested alignment
 *
 * This function is used to register kernel vm area before
 * vmalloc_init() is called.  @vm->size and @vm->flags should contain
 * proper values on entry and other fields should be zero.  On return,
 * vm->addr contains the allocated address.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
void __init vm_area_register_early(struct vm_struct *vm, size_t align)
{
	static size_t vm_init_off __initdata;
	unsigned long addr;

	addr = ALIGN(VMALLOC_START + vm_init_off, align);
	vm_init_off = PFN_ALIGN(addr + vm->size) - VMALLOC_START;

	vm->addr = (void *)addr;

	vm_area_add_early(vm);
}

static void vmap_init_free_space(void)
{
	unsigned long vmap_start = 1;
	const unsigned long vmap_end = ULONG_MAX;
	struct vmap_area *busy, *free;

	/*
	 *     B     F     B     B     B     F
	 * -|-----|.....|-----|-----|-----|.....|-
	 *  |           The KVA space           |
	 *  |<--------------------------------->|
	 */
/*
 * IAMROOT, 2022.07.02: 
 * 위의 그림은 4개의 va(busy)가 등록되어 있다.
 * 이후 추가 2개의 va(free)가 free 공간 관리용 RB 트리와 list에 추가될 예정이다.
 */
	list_for_each_entry(busy, &vmap_area_list, list) {
/*
 * IAMROOT, 2022.07.02: 
 * busy 공간 사이의 free 영역용 공간을 할당받아 초기화한 후,
 * free 공간 관리용 RB 트리와 list에 추가한다.
 */
		if (busy->va_start - vmap_start > 0) {
			free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
			if (!WARN_ON_ONCE(!free)) {
				free->va_start = vmap_start;
				free->va_end = busy->va_start;

				insert_vmap_area_augment(free, NULL,
					&free_vmap_area_root,
						&free_vmap_area_list);
			}
		}

		vmap_start = busy->va_end;
	}

/*
 * IAMROOT, 2022.07.02: 
 * 마지막 빈 공간에 대한 처리
 */
	if (vmap_end - vmap_start > 0) {
		free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
		if (!WARN_ON_ONCE(!free)) {
			free->va_start = vmap_start;
			free->va_end = vmap_end;

			insert_vmap_area_augment(free, NULL,
				&free_vmap_area_root,
					&free_vmap_area_list);
		}
	}
}

void __init vmalloc_init(void)
{
	struct vmap_area *va;
	struct vm_struct *tmp;
	int i;

	/*
	 * Create the cache for vmap_area objects.
	 */
/*
 * IAMROOT, 2022.07.02: 
 * - vmap_area 구조체를 위해 슬랩 캐시를 준비한다.
 * - KMEM_CACHE() 매크로를 사용하는 경우 size와 align은 해당 구조체 크기만큼 
 *   내부에서 자동 지정한다.
 */
	vmap_area_cachep = KMEM_CACHE(vmap_area, SLAB_PANIC);

/*
 * IAMROOT, 2022.07.02: 
 * 전역 vmap_block_queue 및 전역 vfree_deferred를 초기화한다.
 *    vfree_deferred의 wq에는 free_work 함수를 호출할 수 있도록 준비한다.
 */
	for_each_possible_cpu(i) {
		struct vmap_block_queue *vbq;
		struct vfree_deferred *p;

		vbq = &per_cpu(vmap_block_queue, i);
		spin_lock_init(&vbq->lock);
		INIT_LIST_HEAD(&vbq->free);
		p = &per_cpu(vfree_deferred, i);
		init_llist_head(&p->list);
		INIT_WORK(&p->wq, free_work);
	}

/*
 * IAMROOT, 2022.07.02: 
 * map_kernel_segment() -> vm_area_add_early()을 통해 등록한 영역을 
 * 실제 vmalloc 자료 구조(RB tree + list)에 추가한다.
 */
	/* Import existing vmlist entries. */
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		va = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
		if (WARN_ON_ONCE(!va))
			continue;

		va->va_start = (unsigned long)tmp->addr;
		va->va_end = va->va_start + tmp->size;
		va->vm = tmp;
		insert_vmap_area(va, &vmap_area_root, &vmap_area_list);
	}

	/*
	 * Now we can initialize a free vmap space.
	 */
/*
 * IAMROOT, 2022.07.02: 
 * 빈 공간에 대한 자료 구조도 빈 공간용 vmallo 자료 구조(RB tree + list)에 
 * 추가한다.
 */
	vmap_init_free_space();
	vmap_initialized = true;
}

/*
 * IAMROOT, 2022.07.02: 
 * vm 구조체를 전달받은 인자 값으로 채워 초기화한다.
 */
static inline void setup_vmalloc_vm_locked(struct vm_struct *vm,
	struct vmap_area *va, unsigned long flags, const void *caller)
{
	vm->flags = flags;
	vm->addr = (void *)va->va_start;
	vm->size = va->va_end - va->va_start;
	vm->caller = caller;
	va->vm = vm;
}

/*
 * IAMROOT, 2022.07.02: 
 * vm 구조체를 전달받은 인자 값으로 채워 초기화한다.
 */
static void setup_vmalloc_vm(struct vm_struct *vm, struct vmap_area *va,
			      unsigned long flags, const void *caller)
{
	spin_lock(&vmap_area_lock);
	setup_vmalloc_vm_locked(vm, va, flags, caller);
	spin_unlock(&vmap_area_lock);
}

static void clear_vm_uninitialized_flag(struct vm_struct *vm)
{
	/*
	 * Before removing VM_UNINITIALIZED,
	 * we should make sure that vm has proper values.
	 * Pair with smp_rmb() in show_numa_info().
	 */
	smp_wmb();
	vm->flags &= ~VM_UNINITIALIZED;
}

/*
 * IAMROOT, 2022.07.02: 
 * @start ~ @end까지 영역내에서 @size 및 @align에 해당하는 빈 공간을 찾아 
 * vm_struct를 할당하고, 정보를 구성한 후 반환한다.
 * - vm + vma 할당
 */
static struct vm_struct *__get_vm_area_node(unsigned long size,
		unsigned long align, unsigned long shift, unsigned long flags,
		unsigned long start, unsigned long end, int node,
		gfp_t gfp_mask, const void *caller)
{
	struct vmap_area *va;
	struct vm_struct *area;
	unsigned long requested_size = size;

	BUG_ON(in_interrupt());
	size = ALIGN(size, 1ul << shift);
	if (unlikely(!size))
		return NULL;

	if (flags & VM_IOREMAP)
		align = 1ul << clamp_t(int, get_count_order_long(size),
				       PAGE_SHIFT, IOREMAP_MAX_ORDER);

	area = kzalloc_node(sizeof(*area), gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!area))
		return NULL;

/*
 * IAMROOT, 2022.07.02: 
 * vmalloc 할당마다 보통 1페이지의 guard 페이지를 둔다.(매핑하지 않은 상태)
 */
	if (!(flags & VM_NO_GUARD))
		size += PAGE_SIZE;

/*
 * IAMROOT, 2022.07.02: 
 * @start ~ @end 영역내에서 빈 공간을 아래부터 탐색하여 찾은 후 @align에 
 * 맞춘 주소로 vmap_area 정보를 할당해온다.
 */
	va = alloc_vmap_area(size, align, start, end, node, gfp_mask);
	if (IS_ERR(va)) {
		kfree(area);
		return NULL;
	}

	kasan_unpoison_vmalloc((void *)va->va_start, requested_size);

/*
 * IAMROOT, 2022.07.02: 
 * vm 구조체를 전달받은 인자 값으로 채워 초기화한다.
 *
 *      vmap_area(va)       /-->  vm_struct(area)
 *      -------------      /      ---------------
 *      va_start          /       addr
 *      va_end           |        size
 *      vm --------------+
 */
	setup_vmalloc_vm(area, va, flags, caller);

	return area;
}

struct vm_struct *__get_vm_area_caller(unsigned long size, unsigned long flags,
				       unsigned long start, unsigned long end,
				       const void *caller)
{
	return __get_vm_area_node(size, 1, PAGE_SHIFT, flags, start, end,
				  NUMA_NO_NODE, GFP_KERNEL, caller);
}

/**
 * get_vm_area - reserve a contiguous kernel virtual area
 * @size:	 size of the area
 * @flags:	 %VM_IOREMAP for I/O mappings or VM_ALLOC
 *
 * Search an area of @size in the kernel virtual mapping area,
 * and reserved it for out purposes.  Returns the area descriptor
 * on success or %NULL on failure.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
struct vm_struct *get_vm_area(unsigned long size, unsigned long flags)
{
	return __get_vm_area_node(size, 1, PAGE_SHIFT, flags,
				  VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL,
				  __builtin_return_address(0));
}

/*
 * IAMROOT, 2022.07.16:
 * - vmalloc 공간에 요청 영역을 mapping한다.
 */
struct vm_struct *get_vm_area_caller(unsigned long size, unsigned long flags,
				const void *caller)
{
	return __get_vm_area_node(size, 1, PAGE_SHIFT, flags,
				  VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL, caller);
}

/**
 * find_vm_area - find a continuous kernel virtual area
 * @addr:	  base address
 *
 * Search for the kernel VM area starting at @addr, and return it.
 * It is up to the caller to do all required locking to keep the returned
 * pointer valid.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
/*
 * iamroot, 2022.07.02: 
 * vmalloc 자료구조에서 @addr에 해당하는 vmap_area를 찾은 후 연결된 vm_struct를 
 * 반홚나다. 못 잧은 경우 null을 반환한다.
 */
struct vm_struct *find_vm_area(const void *addr)
{
	struct vmap_area *va;

	va = find_vmap_area((unsigned long)addr);
	if (!va)
		return NULL;

	return va->vm;
}

/**
 * remove_vm_area - find and remove a continuous kernel virtual area
 * @addr:	    base address
 *
 * Search for the kernel VM area starting at @addr, and remove it.
 * This function returns the found VM area, but using it is NOT safe
 * on SMP machines, except for its size or flags.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
/*
 * IAMROOT, 2022.07.02: 
 * - vmalloc 자료 구조에서 @addr 주소로 등록되어 있는 vmap_area를 찾은 후 
 * 연결된 vm_struct의 매핑들을 해제한다.
 */
struct vm_struct *remove_vm_area(const void *addr)
{
	struct vmap_area *va;

	might_sleep();

	spin_lock(&vmap_area_lock);
	va = __find_vmap_area((unsigned long)addr);
	if (va && va->vm) {
		struct vm_struct *vm = va->vm;

		va->vm = NULL;
		spin_unlock(&vmap_area_lock);

		kasan_free_shadow(vm);
		free_unmap_vmap_area(va);

		return vm;
	}

	spin_unlock(&vmap_area_lock);
	return NULL;
}

/*
 * IAMROOT, 2023.02.24:
 * - @area의 pages에 대해서 set_direct_map을 호출하게 한다.
 */
static inline void set_area_direct_map(const struct vm_struct *area,
				       int (*set_direct_map)(struct page *page))
{
	int i;

	/* HUGE_VMALLOC passes small pages to set_direct_map */
	for (i = 0; i < area->nr_pages; i++)
		if (page_address(area->pages[i]))
			set_direct_map(area->pages[i]);
}

/* Handle removing and resetting vm mappings related to the vm_struct. */
/*
 * IAMROOT, 2023.02.03:
 * --- chat open ai ---
 * - direct map
 *   직접 매핑은 물리적 메모리에 대한 직접 액세스를 제공하기 위해 운영 
 *   체제에서 사용하는 메모리 관리 기술입니다. 직접 매핑 시스템에서 각 
 *   메모리 주소는 중간 변환이나 매핑 없이 메인 메모리의 특정 물리적 주소에 
 *   매핑됩니다. 이는 가상 메모리와 같은 다른 매핑 기술을 사용하는 
 *   시스템보다 메모리 액세스를 더 빠르고 효율적으로 만듭니다.
 *
 *   Linux에서 커널은 직접 매핑 기술을 사용하여 전체 물리적 메모리를 커널의 
 *   가상 주소 공간에 매핑함으로써 물리적 메모리에 대한 빠른 액세스를 
 *   제공합니다. 이를 통해 커널은 주소 변환이나 페이지 오류의 오버헤드 없이 
 *   빠르고 효율적으로 메모리에 액세스할 수 있습니다. Linux의 직접 매핑 
 *   영역은 직접 메모리 액세스가 필요한 커널 데이터 구조 및 하드웨어 
 *   장치를 매핑하는 데에도 사용됩니다. 
 * --------------------
 *  @area->addr vm_area mapping을 해제한다.
 *  - @deallocate_pages == 0인 경우 
 *    모든 vm영역에 대한 lazy aliases에 대해서 unmap을 수행한다.
 *  - @deallocate_pages != 0인 경우
 *    @area영역에 대해서 flush할 dmap 범위를 찾아 vm_unmap_aliases()에 
 *    포함되도록 한다.
 *    그런 다음 플러시 후 액세스가 있는 경우 캐시되지 않도록
 *    dmap을 invalid로 설정한 다음 tlb flush후 default로 재설정한다.
 */
static void vm_remove_mappings(struct vm_struct *area, int deallocate_pages)
{
	unsigned long start = ULONG_MAX, end = 0;
	unsigned int page_order = vm_area_page_order(area);
	int flush_reset = area->flags & VM_FLUSH_RESET_PERMS;
	int flush_dmap = 0;
	int i;

	remove_vm_area(area->addr);

	/* If this is not VM_FLUSH_RESET_PERMS memory, no need for the below. */
	if (!flush_reset)
		return;

	/*
	 * If not deallocating pages, just do the flush of the VM area and
	 * return.
	 */
/*
 * IAMROOT, 2023.02.17:
 * - papago
 *   페이지 할당을 해제하지 않으면 VM 영역을 플러시하고 반환하십시오.
 * - dealloc 요청이 없었다면 모든 vm 영역에 대한 lazy aliases에 대해서 unmap을
 *   수행한다.
 */
	if (!deallocate_pages) {
		vm_unmap_aliases();
		return;
	}

	/*
	 * If execution gets here, flush the vm mapping and reset the direct
	 * map. Find the start and end range of the direct mappings to make sure
	 * the vm_unmap_aliases() flush includes the direct map.
	 */
/*
 * IAMROOT, 2023.02.17:
 * - papago
 *   실행이 여기에 도달하면 vm 매핑을 플러시하고 직접 매핑을 재설정합니다. 
 *   vm_unmap_aliases() 플러시가 직접 매핑을 포함하는지 확인하기 위해 직접 
 *   매핑의 시작 및 끝 범위를 찾습니다.
 * - @area으 pages address로 unmap할 start, end 범위를 찾는다.
 */
	for (i = 0; i < area->nr_pages; i += 1U << page_order) {
		unsigned long addr = (unsigned long)page_address(area->pages[i]);
		if (addr) {
			unsigned long page_size;

			page_size = PAGE_SIZE << page_order;
			start = min(addr, start);
			end = max(addr + page_size, end);
			flush_dmap = 1;
		}
	}

	/*
	 * Set direct map to something invalid so that it won't be cached if
	 * there are any accesses after the TLB flush, then flush the TLB and
	 * reset the direct map permissions to the default.
	 */
/*
 * IAMROOT, 2023.02.17:
 * - papago
 *   TLB 플러시 후 액세스가 있는 경우 캐시되지 않도록 직접 매핑을 잘못된 
 *   것으로 설정한 다음 TLB를 플러시하고 직접 매핑 권한을 기본값으로 
 *   재설정합니다.
 * - chat open ai
 *  직접 매핑이 먼저 invalid 상태로 설정되었다가 매핑 해제 후 default 상태로 
 *  돌아가는 이유는 잠재적인 캐싱 문제를 방지하고 직접 매핑에 대한 페이지 
 *  테이블 항목(PTE)이 적절하게 업데이트되고 무효화되도록 하기 위함입니다.
 *
 *  페이지 테이블 항목이 invalid되면(이 경우 PTE_VALID 비트를 지워서) 
 *  더 이상 주소 변환에 사용할 수 없음을 의미합니다. 그러나 일부 프로세서 
 *  또는 메모리 관리 장치(MMU)는 여전히 유효하지 않은 페이지 테이블 항목을 
 *  캐시할 수 있으며, 이는 해당 가상 메모리 주소에 액세스하는 경우 불일치 및 
 *  잘못된 매핑으로 이어질 수 있습니다. 
 *
 *  이를 방지하기 위해 캐시된 페이지 테이블 항목이 플러시되고 주소 변환에 
 *  사용되지 않도록 직접 매핑이 먼저 유효하지 않은 상태로 설정됩니다. 매핑 
 *  해제가 완료되면 주소 변환에 올바른 페이지 테이블 항목이 사용되도록 직접 
 *  매핑이 기본 상태로 다시 설정됩니다.
 *
 *  이 프로세스는 메모리의 특정 영역에 액세스하기 위해 커널이 사용하는 특수 
 *  유형의 매핑인 직접 매핑에만 적용됩니다. 다른 유형의 매핑에는 페이지 
 *  테이블 항목을 업데이트하고 무효화하기 위한 요구 사항이 다를 수 있습니다. 
 *
 * -  page table entires 를 invalid로 변경 -> 
 *    purge / tlb flush(memory mapping 해제) -> 
 *    page table entires 를 write only로 변경. 
 */
	set_area_direct_map(area, set_direct_map_invalid_noflush);
	_vm_unmap_aliases(start, end, flush_dmap);
	set_area_direct_map(area, set_direct_map_default_noflush);
}

/*
 * IAMROOT, 2022.07.02: 
 * 가상 주소 @addr을 언매핑한다. @deallocate_pages 요청이 1인 경우 
 * 메모리도 회수한다. (vfree에서 요청한 경우 1, vunmap에서 요청한 경우 0)
 */
static void __vunmap(const void *addr, int deallocate_pages)
{
	struct vm_struct *area;

	if (!addr)
		return;

	if (WARN(!PAGE_ALIGNED(addr), "Trying to vfree() bad address (%p)\n",
			addr))
		return;

/*
 * iamroot, 2022.07.02: 
 * vmalloc 자료구조에서 @addr에 해당하는 vmap_area를 찾은 후 연결된 vm_struct를 
 * 찾아온다.
 */
	area = find_vm_area(addr);
	if (unlikely(!area)) {
		WARN(1, KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n",
				addr);
		return;
	}

	debug_check_no_locks_freed(area->addr, get_vm_area_size(area));
	debug_check_no_obj_freed(area->addr, get_vm_area_size(area));

	kasan_poison_vmalloc(area->addr, get_vm_area_size(area));
/*
 * IAMROOT, 2023.02.24:
 * - unmaping 및 dmap flush
 */
	vm_remove_mappings(area, deallocate_pages);

/*
 * IAMROOT, 2022.07.02: 
 * 언매핑 후 @deallocate_pages(vfree 요청)==1 이면 모든 관련 페이지들을
 * 할당 해제한다.
 * vmap에서 flag에 VM_MAP_PUT_PAGES가 있었을경우 pages 정보(nr_pages, 
 * area->pages)가 set되었을 것이다. pages를 해제한다.
 */
	if (deallocate_pages) {
		unsigned int page_order = vm_area_page_order(area);
		int i;

		for (i = 0; i < area->nr_pages; i += 1U << page_order) {
			struct page *page = area->pages[i];

			BUG_ON(!page);
			__free_pages(page, page_order);
			cond_resched();
		}
		atomic_long_sub(area->nr_pages, &nr_vmalloc_pages);

		kvfree(area->pages);
	}

	kfree(area);
}

static inline void __vfree_deferred(const void *addr)
{
	/*
	 * Use raw_cpu_ptr() because this can be called from preemptible
	 * context. Preemption is absolutely fine here, because the llist_add()
	 * implementation is lockless, so it works even if we are adding to
	 * another cpu's list. schedule_work() should be fine with this too.
	 */
/*
 * IAMROOT, 2022.07.02: 
 * vfree 해야할 주소 @addr을 per-cpu 리스트인 vfee_deferred 리스트에 추가한 후
 * 워크큐를 스케쥴-인하여 워커스레드가 free_work() 함수를 호출하여 
 * vunmap 처리를 수행하도록 한다.
 */

	struct vfree_deferred *p = raw_cpu_ptr(&vfree_deferred);

	if (llist_add((struct llist_node *)addr, &p->list))
		schedule_work(&p->wq);
}

/**
 * vfree_atomic - release memory allocated by vmalloc()
 * @addr:	  memory base address
 *
 * This one is just like vfree() but can be called in any atomic context
 * except NMIs.
 */
void vfree_atomic(const void *addr)
{
	BUG_ON(in_nmi());

	kmemleak_free(addr);

	if (!addr)
		return;
	__vfree_deferred(addr);
}

/*
 * IAMROOT, 2022.07.02: 
 * - vmalloc으로 할당한 메모리를 할당 해제하고, 언매핑한다.
 *   (인터럽트 핸들러에서 호출된 경우 워커스레드를 사용하여 
 *    deferred 처리를 수행한다.)
 */
static void __vfree(const void *addr)
{
	if (unlikely(in_interrupt()))
		__vfree_deferred(addr);
	else
		__vunmap(addr, 1);
}

/**
 * vfree - Release memory allocated by vmalloc()
 * @addr:  Memory base address
 *
 * Free the virtually continuous memory area starting at @addr, as obtained
 * from one of the vmalloc() family of APIs.  This will usually also free the
 * physical memory underlying the virtual allocation, but that memory is
 * reference counted, so it will not be freed until the last user goes away.
 *
 * If @addr is NULL, no operation is performed.
 *
 * Context:
 * May sleep if called *not* from interrupt context.
 * Must not be called in NMI context (strictly speaking, it could be
 * if we have CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG, but making the calling
 * conventions for vfree() arch-dependent would be a really bad idea).
 */
/*
 * IAMROOT, 2022.07.02: 
 * - papago
 * vfree - Release memory allocated by vmalloc()
 * @addr:  Memory base address
 *
 * vmalloc() API 제품군 중 하나에서 얻은 @addr에서 시작하는 가상 연속 메모리
 * 영역을 해제합니다. 이렇게 하면 일반적으로 가상 할당의 기본이 되는 물리적
 * 메모리도 해제되지만 해당 메모리는 참조 카운트되므로 마지막 사용자가 사라질
 * 때까지 해제되지 않습니다.
 *
 * If @addr is NULL, no operation is performed.
 *
 * Context:
 * 인터럽트 컨텍스트에서 *not* 호출된 경우 절전 모드일 수 있습니다.
 * NMI 컨텍스트에서 호출하면 안 됩니다(엄밀히 말하면
 * CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG가 있는 경우일 수 있지만 vfree()에 대한
 * 호출 규칙을 arch 종속으로 만드는 것은 정말 나쁜 생각입니다).
 *
 * - vmalloc으로 할당한 메모리를 할당 해제하고, 언매핑한다.
 */
void vfree(const void *addr)
{
	BUG_ON(in_nmi());

	kmemleak_free(addr);

	might_sleep_if(!in_interrupt());

	if (!addr)
		return;

	__vfree(addr);
}
EXPORT_SYMBOL(vfree);

/**
 * vunmap - release virtual mapping obtained by vmap()
 * @addr:   memory base address
 *
 * Free the virtually contiguous memory area starting at @addr,
 * which was created from the page array passed to vmap().
 *
 * Must not be called in interrupt context.
 */
/*
 * IAMROOT, 2023.02.03:
 * --- chat open ai ----
 *  - interrupt중 메모리 해제를 하면 안되는 이유
 *  인터럽트 내부의 메모리를 해제하면 시스템에 여러 가지 문제가 발생할 수 
 *  있습니다. 인터럽트는 비동기적으로 발생하고 언제든지 발생할 수 있는 
 *  이벤트로, 커널에서 정상적인 실행 흐름을 방해합니다. 따라서 인터럽트 
 *  내부의 메모리를 해제하면 많은 문제가 발생할 수 있습니다.
 *
 *  첫째, 해제되는 메모리가 여전히 커널의 다른 부분이나 사용자 프로세스에서 
 *  사용되고 있는 경합 상태로 이어질 수 있습니다. 메모리에 계속 액세스하는 
 *  동안 메모리가 해제되면 메모리 손상이 발생하여 시스템 충돌 또는 데이터 
 *  손실이 발생할 수 있습니다.
 *
 *  둘째, 메모리 할당 하위 시스템이 인터럽트 컨텍스트 내에서 메모리를 해제할 
 *  때 발생하는 재진입 문제를 처리하도록 설계되지 않은 경우 인터럽트 내부의 
 *  메모리를 해제하면 교착 상태가 발생할 수 있습니다. 이는 인터럽트 핸들러 
 *  자체가 메모리를 할당해야 하는 경우 발생할 수 있습니다. 메모리가 해제될 
 *  때까지 차단되기 때문에 인터럽트 핸들러가 반환될 때까지 메모리가 차단됩니다.
 *  마지막으로 인터럽트 내부의 메모리를 해제하면 메모리를 해제하는 데 시간이 
 *  오래 걸리는 프로세스가 될 수 있으므로 성능 문제가 발생할 수 있으며 
 *  시스템의 정상 작동을 방해하지 않도록 인터럽트를 신속하게 처리해야 합니다.
 *
 *  이러한 이유로 인터럽트 내에서 메모리를 해제하는 것은 일반적으로 권장되지 
 *  않습니다. 대신 커널 스레드나 작업 대기열을 사용하는 것과 같이 시스템이 
 *  인터럽트 컨텍스트에 있지 않은 나중 시점으로 메모리 해제 작업을 연기하는 
 *  것이 가장 좋습니다. 
 * ---------------------
 *
 * - @addr의 mapping을 해제한다.
 */
void vunmap(const void *addr)
{
	BUG_ON(in_interrupt());
	might_sleep();
	if (addr)
		__vunmap(addr, 0);
}
EXPORT_SYMBOL(vunmap);

/**
 * vmap - map an array of pages into virtually contiguous space
 * @pages: array of page pointers
 * @count: number of pages to map
 * @flags: vm_area->flags
 * @prot: page protection for the mapping
 *
 * Maps @count pages from @pages into contiguous kernel virtual space.
 * If @flags contains %VM_MAP_PUT_PAGES the ownership of the pages array itself
 * (which must be kmalloc or vmalloc memory) and one reference per pages in it
 * are transferred from the caller to vmap(), and will be freed / dropped when
 * vfree() is called on the return value.
 *
 * Return: the address of the area or %NULL on failure
 */
/*
 * IAMROOT, 2023.02.03:
 * - papago
 *   vmap - 페이지 배열을 거의 연속적인 공간으로 매핑 @pages: 페이지 포인터 배열
 *   @count: 매핑할 페이지 수
 *   @플래그: vm_area->플래그
 *   @prot: 매핑을 위한 페이지 보호
 *   @pages의 @count 페이지를 인접한 커널 가상 공간으로 매핑합니다.
 *   @flags에 %VM_MAP_PUT_PAGES가 포함되어 있으면 페이지 배열 자체의 
 *   소유권(kmalloc 또는 vmalloc 메모리여야 함)과 페이지당 하나의 참조가 
 *   호출자에서 vmap()으로 전송되고 vfree()가 실행될 때 해제/삭제됩니다. 
 *   반환 값을 호출했습니다.
 *
 *   Return: 영역의 주소 또는 실패 시 %NULL.
 *
 *   - 요청인자로 vm_struct를 가져오고, vm_struct에 pages를
 *   mapping한다.
 *   - flags VM_MAP_PUT_PAGES가 있을경우 pages정보가 vm_struct에 설정되어,
 *   vfree로 호출될때 같이 해제할수있게 한다.
 */
void *vmap(struct page **pages, unsigned int count,
	   unsigned long flags, pgprot_t prot)
{
	struct vm_struct *area;
	unsigned long addr;
	unsigned long size;		/* In bytes */

	might_sleep();

	if (count > totalram_pages())
		return NULL;

	size = (unsigned long)count << PAGE_SHIFT;
	area = get_vm_area_caller(size, flags, __builtin_return_address(0));
	if (!area)
		return NULL;

	addr = (unsigned long)area->addr;
	if (vmap_pages_range(addr, addr + size, pgprot_nx(prot),
				pages, PAGE_SHIFT) < 0) {
		vunmap(area->addr);
		return NULL;
	}

	if (flags & VM_MAP_PUT_PAGES) {
		area->pages = pages;
		area->nr_pages = count;
	}
	return area->addr;
}
EXPORT_SYMBOL(vmap);

#ifdef CONFIG_VMAP_PFN
struct vmap_pfn_data {
	unsigned long	*pfns;
	pgprot_t	prot;
	unsigned int	idx;
};

static int vmap_pfn_apply(pte_t *pte, unsigned long addr, void *private)
{
	struct vmap_pfn_data *data = private;

	if (WARN_ON_ONCE(pfn_valid(data->pfns[data->idx])))
		return -EINVAL;
	*pte = pte_mkspecial(pfn_pte(data->pfns[data->idx++], data->prot));
	return 0;
}

/**
 * vmap_pfn - map an array of PFNs into virtually contiguous space
 * @pfns: array of PFNs
 * @count: number of pages to map
 * @prot: page protection for the mapping
 *
 * Maps @count PFNs from @pfns into contiguous kernel virtual space and returns
 * the start address of the mapping.
 */
void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot)
{
	struct vmap_pfn_data data = { .pfns = pfns, .prot = pgprot_nx(prot) };
	struct vm_struct *area;

	area = get_vm_area_caller(count * PAGE_SIZE, VM_IOREMAP,
			__builtin_return_address(0));
	if (!area)
		return NULL;
	if (apply_to_page_range(&init_mm, (unsigned long)area->addr,
			count * PAGE_SIZE, vmap_pfn_apply, &data)) {
		free_vm_area(area);
		return NULL;
	}
	return area->addr;
}
EXPORT_SYMBOL_GPL(vmap_pfn);
#endif /* CONFIG_VMAP_PFN */

/*
 * IAMROOT, 2022.07.08:
 * 1. @order 0이고 @nid가 지정된경우.
 *   bulk로 받아온다.
 * 2. 그외 or bulk로 할당실패
 *   slowpath로 받아온다.
 */
static inline unsigned int
vm_area_alloc_pages(gfp_t gfp, int nid,
		unsigned int order, unsigned int nr_pages, struct page **pages)
{
	unsigned int nr_allocated = 0;
	struct page *page;
	int i;

	/*
	 * For order-0 pages we make use of bulk allocator, if
	 * the page array is partly or not at all populated due
	 * to fails, fallback to a single page allocator that is
	 * more permissive.
	 */
/*
 * IAMROOT, 2022.07.02: 
 * order=0이고, node가 지정된 경우 최대 100개 이내로 bulk 방식으로 
 * 싱글 페이지들을 할당해온다.
 */

	if (!order && nid != NUMA_NO_NODE) {
		while (nr_allocated < nr_pages) {
			unsigned int nr, nr_pages_request;

			/*
			 * A maximum allowed request is hard-coded and is 100
			 * pages per call. That is done in order to prevent a
			 * long preemption off scenario in the bulk-allocator
			 * so the range is [1:100].
			 */
			nr_pages_request = min(100U, nr_pages - nr_allocated);

			nr = alloc_pages_bulk_array_node(gfp, nid,
				nr_pages_request, pages + nr_allocated);

			nr_allocated += nr;
			cond_resched();

			/*
			 * If zero or pages were obtained partly,
			 * fallback to a single page allocator.
			 */
			if (nr != nr_pages_request)
				break;
		}
	} else if (order)
		/*
		 * Compound pages required for remap_vmalloc_page if
		 * high-order pages.
		 */
		gfp |= __GFP_COMP;

	/* High-order pages or fallback path if "bulk" fails. */

/*
 * IAMROOT, 2022.07.02: 
 * 싱글 페이지들을 buik 방식으로 할당하지 못하엿거나, huge(pmd) 단위로
 * 할당해야 하는 경우 이 루틴을 사용한다.
 */
	while (nr_allocated < nr_pages) {
		if (nid == NUMA_NO_NODE)
			page = alloc_pages(gfp, order);
		else
			page = alloc_pages_node(nid, gfp, order);
		if (unlikely(!page))
			break;

		/*
		 * Careful, we allocate and map page-order pages, but
		 * tracking is done per PAGE_SIZE page so as to keep the
		 * vm_struct APIs independent of the physical/mapped size.
		 */
		for (i = 0; i < (1U << order); i++)
			pages[nr_allocated + i] = page + i;

		cond_resched();
		nr_allocated += 1U << order;
	}

	return nr_allocated;
}

/*
 * IAMROOT, 2022.07.02: 
 * area 정보(vm_struct)로 single 페이지들을 할당하고, 매핑한다.
 */
static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
				 pgprot_t prot, unsigned int page_shift,
				 int node)
{
	const gfp_t nested_gfp = (gfp_mask & GFP_RECLAIM_MASK) | __GFP_ZERO;
	unsigned long addr = (unsigned long)area->addr;
	unsigned long size = get_vm_area_size(area);
	unsigned long array_size;
	unsigned int nr_small_pages = size >> PAGE_SHIFT;
	unsigned int page_order;

/*
 * IAMROOT, 2022.07.02: 
 * area 정보의 **page에 필요한 페이지 수만큼 page 구조체들을 할당할 사이즈를 
 * 정한다.
 */
	array_size = (unsigned long)nr_small_pages * sizeof(struct page *);
	gfp_mask |= __GFP_NOWARN;
	if (!(gfp_mask & (GFP_DMA | GFP_DMA32)))
		gfp_mask |= __GFP_HIGHMEM;

/*
 * IAMROOT, 2022.07.02: 
 * page 배열이 1 페이지 사이즈를 초과하는 경우 vmalloc을 통해서 할당받고,
 * 그렇지 않은 경우 kmalloc으로 할당해온다.
 */
	/* Please note that the recursion is strictly bounded. */
	if (array_size > PAGE_SIZE) {
		area->pages = __vmalloc_node(array_size, 1, nested_gfp, node,
					area->caller);
	} else {
		area->pages = kmalloc_node(array_size, nested_gfp, node);
	}

	if (!area->pages) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, failed to allocated page array size %lu",
			nr_small_pages * PAGE_SIZE, array_size);
		free_vm_area(area);
		return NULL;
	}

/*
 * IAMROOT, 2022.07.02: 
 * huge 매핑이 허용된 경우 해당 order를 산출하여 vm에 지정한다.
 * huge 매핑이 아닌 경우 order 0를 vm에 지정한다.
 * 예) huge허용: page_shift=20, PAGE_SHIFT=12
 *     -> order=8 (4K 페이지가 2^8=256인 즉, 2MB)
 */
	set_vm_area_page_order(area, page_shift - PAGE_SHIFT);
	page_order = vm_area_page_order(area);

/*
 * IAMROOT, 2022.07.02: 
 * 싱글 페이지 또는 huge page들을 할당받고, 
 * 각 page 구조체들을 area->pages에 알아온다.
 */
	area->nr_pages = vm_area_alloc_pages(gfp_mask, node,
		page_order, nr_small_pages, area->pages);

	atomic_long_add(area->nr_pages, &nr_vmalloc_pages);

	/*
	 * If not enough pages were obtained to accomplish an
	 * allocation request, free them via __vfree() if any.
	 */
	if (area->nr_pages != nr_small_pages) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, page order %u, failed to allocate pages",
			area->nr_pages * PAGE_SIZE, page_order);
		goto fail;
	}

/*
 * IAMROOT, 2022.07.02: 
 * 정상적으로 할당받아온 경우 매핑을 수행한다.
 */
	if (vmap_pages_range(addr, addr + size, prot, area->pages,
			page_shift) < 0) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, failed to map pages",
			area->nr_pages * PAGE_SIZE);
		goto fail;
	}

	return area->addr;

fail:
	__vfree(area->addr);
	return NULL;
}

/**
 * __vmalloc_node_range - allocate virtually contiguous memory
 * @size:		  allocation size
 * @align:		  desired alignment
 * @start:		  vm area range start
 * @end:		  vm area range end
 * @gfp_mask:		  flags for the page level allocator
 * @prot:		  protection mask for the allocated pages
 * @vm_flags:		  additional vm area flags (e.g. %VM_NO_GUARD)
 * @node:		  node to use for allocation or NUMA_NO_NODE
 * @caller:		  caller's return address
 *
 * Allocate enough pages to cover @size from the page level
 * allocator with @gfp_mask flags.  Map them into contiguous
 * kernel virtual space, using a pagetable protection of @prot.
 *
 * Return: the address of the area or %NULL on failure
 */
/*
 * IAMROOT, 2022.07.02: 
 * @start ~ @end 가상 공간 범위내에서 @align 적용된 @size 만큼의 
 * 빈 공간을 탐색하여 싱글 물리 페이지들을 할당하고, @prot 속성으로 매핑한다.
 */
void *__vmalloc_node_range(unsigned long size, unsigned long align,
			unsigned long start, unsigned long end, gfp_t gfp_mask,
			pgprot_t prot, unsigned long vm_flags, int node,
			const void *caller)
{
	struct vm_struct *area;
	void *addr;
	unsigned long real_size = size;
	unsigned long real_align = align;
	unsigned int shift = PAGE_SHIFT;

	if (WARN_ON_ONCE(!size))
		return NULL;

	if ((size >> PAGE_SHIFT) > totalram_pages()) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, exceeds total pages",
			real_size);
		return NULL;
	}

/*
 * IAMROOT, 2022.07.02: 
 * 시스템은 huge 매핑을 지원하지만 사용자 요청에 huge 매핑을 사용하지 않도록
 * 요청하지 않은 경우이다. 이러한 경우 size와 align 값은 huge(pmd=2M) 매핑 
 * 단위로 정렬하여 사용한다.
 */
	if (vmap_allow_huge && !(vm_flags & VM_NO_HUGE_VMAP)) {
		unsigned long size_per_node;

		/*
		 * Try huge pages. Only try for PAGE_KERNEL allocations,
		 * others like modules don't yet expect huge pages in
		 * their allocations due to apply_to_page_range not
		 * supporting them.
		 */

		size_per_node = size;
		if (node == NUMA_NO_NODE)
			size_per_node /= num_online_nodes();
		if (arch_vmap_pmd_supported(prot) && size_per_node >= PMD_SIZE)
			shift = PMD_SHIFT;
		else
			shift = arch_vmap_pte_supported_shift(size_per_node);

		align = max(real_align, 1UL << shift);
		size = ALIGN(real_size, 1UL << shift);
	}

again:
/*
 * IAMROOT, 2022.07.02: 
 * start ~ end까지 영역내에서 real_size 및 align에 해당하는 빈 공간을 찾아 
 * vm_struct를 할당하고, page 구조체 배열 들의 정보를 구성하여 알아온다.
 */
	area = __get_vm_area_node(real_size, align, shift, VM_ALLOC |
				  VM_UNINITIALIZED | vm_flags, start, end, node,
				  gfp_mask, caller);
	if (!area) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, vm_struct allocation failed",
			real_size);
		goto fail;
	}

/*
 * IAMROOT, 2022.07.02: 
 *  area(vm_struct) 정보에 있는 pages 구조체 배열을 사용하여 싱글 페이지(또는
 *  huge 페이지)들을 할당받은 후 가상 주소 krea->addr 부터 매핑한다.
 */
	addr = __vmalloc_area_node(area, gfp_mask, prot, shift, node);
	if (!addr)
		goto fail;

	/*
	 * In this function, newly allocated vm_struct has VM_UNINITIALIZED
	 * flag. It means that vm_struct is not fully initialized.
	 * Now, it is fully initialized, so remove this flag here.
	 */
	clear_vm_uninitialized_flag(area);

	size = PAGE_ALIGN(size);
	kmemleak_vmalloc(area, size, gfp_mask);

	return addr;

fail:
	if (shift > PAGE_SHIFT) {
		shift = PAGE_SHIFT;
		align = real_align;
		size = real_size;
		goto again;
	}

	return NULL;
}

/**
 * __vmalloc_node - allocate virtually contiguous memory
 * @size:	    allocation size
 * @align:	    desired alignment
 * @gfp_mask:	    flags for the page level allocator
 * @node:	    node to use for allocation or NUMA_NO_NODE
 * @caller:	    caller's return address
 *
 * Allocate enough pages to cover @size from the page level allocator with
 * @gfp_mask flags.  Map them into contiguous kernel virtual space.
 *
 * Reclaim modifiers in @gfp_mask - __GFP_NORETRY, __GFP_RETRY_MAYFAIL
 * and __GFP_NOFAIL are not supported
 *
 * Any use of gfp flags outside of GFP_KERNEL should be consulted
 * with mm people.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *__vmalloc_node(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, int node, const void *caller)
{
	return __vmalloc_node_range(size, align, VMALLOC_START, VMALLOC_END,
				gfp_mask, PAGE_KERNEL, 0, node, caller);
}
/*
 * This is only for performance analysis of vmalloc and stress purpose.
 * It is required by vmalloc test module, therefore do not use it other
 * than that.
 */
#ifdef CONFIG_TEST_VMALLOC_MODULE
EXPORT_SYMBOL_GPL(__vmalloc_node);
#endif

void *__vmalloc(unsigned long size, gfp_t gfp_mask)
{
	return __vmalloc_node(size, 1, gfp_mask, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(__vmalloc);

/**
 * vmalloc - allocate virtually contiguous memory
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
/*
 * IAMROOT, 2022.07.02: 
 * - vmalloc 공간에 연속된 메모리를 할당한다.
 *    (내부적으로 요청한 @size 만큼의 single 페이지들을 할당받아 
 *     vmalloc 공간의 하단 공간 중 빈 공간을 찾아 vmap()을 통해 매핑한다.)
 * - vmalloc 공간의 상위 부분은 per-cpu의 chunk들이 추가될 때 사용된다.
 *   그리고 vmalloc 및 vmap API를 통해 아래 공간의 빈 공간 부터 검색되어 
 *   사용된다.
 *
 *    vmalloc 공간
 *    +---------------------------+
 *    |         per-cpu           |
 *    |            :              |
 *    |            v              |
 *    |                           |
 *    |                           |
 *    |                           |
 *    |            ^              |
 *    |            :              |
 *    |         vmalloc()         |
 *    +---------------------------+
 */
void *vmalloc(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_KERNEL, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc);

/**
 * vmalloc_no_huge - allocate virtually contiguous memory using small pages
 * @size:    allocation size
 *
 * Allocate enough non-huge pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_no_huge(unsigned long size)
{
	return __vmalloc_node_range(size, 1, VMALLOC_START, VMALLOC_END,
				    GFP_KERNEL, PAGE_KERNEL, VM_NO_HUGE_VMAP,
				    NUMA_NO_NODE, __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_no_huge);

/**
 * vzalloc - allocate virtually contiguous memory with zero fill
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_KERNEL | __GFP_ZERO, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(vzalloc);

/**
 * vmalloc_user - allocate zeroed virtually contiguous memory for userspace
 * @size: allocation size
 *
 * The resulting memory area is zeroed so it can be mapped to userspace
 * without leaking data.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_user(unsigned long size)
{
	return __vmalloc_node_range(size, SHMLBA,  VMALLOC_START, VMALLOC_END,
				    GFP_KERNEL | __GFP_ZERO, PAGE_KERNEL,
				    VM_USERMAP, NUMA_NO_NODE,
				    __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_user);

/**
 * vmalloc_node - allocate memory on a specific node
 * @size:	  allocation size
 * @node:	  numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_node(unsigned long size, int node)
{
	return __vmalloc_node(size, 1, GFP_KERNEL, node,
			__builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_node);

/**
 * vzalloc_node - allocate memory on a specific node with zero fill
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc_node(unsigned long size, int node)
{
	return __vmalloc_node(size, 1, GFP_KERNEL | __GFP_ZERO, node,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(vzalloc_node);

#if defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA32)
#define GFP_VMALLOC32 (GFP_DMA32 | GFP_KERNEL)
#elif defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA)
#define GFP_VMALLOC32 (GFP_DMA | GFP_KERNEL)
#else
/*
 * 64b systems should always have either DMA or DMA32 zones. For others
 * GFP_DMA32 should do the right thing and use the normal zone.
 */
#define GFP_VMALLOC32 (GFP_DMA32 | GFP_KERNEL)
#endif

/**
 * vmalloc_32 - allocate virtually contiguous memory (32bit addressable)
 * @size:	allocation size
 *
 * Allocate enough 32bit PA addressable pages to cover @size from the
 * page level allocator and map them into contiguous kernel virtual space.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_32(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_VMALLOC32, NUMA_NO_NODE,
			__builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32);

/**
 * vmalloc_32_user - allocate zeroed virtually contiguous 32bit memory
 * @size:	     allocation size
 *
 * The resulting memory area is 32bit addressable and zeroed so it can be
 * mapped to userspace without leaking data.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_32_user(unsigned long size)
{
	return __vmalloc_node_range(size, SHMLBA,  VMALLOC_START, VMALLOC_END,
				    GFP_VMALLOC32 | __GFP_ZERO, PAGE_KERNEL,
				    VM_USERMAP, NUMA_NO_NODE,
				    __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32_user);

/*
 * small helper routine , copy contents to buf from addr.
 * If the page is not present, fill zero.
 */

static int aligned_vread(char *buf, char *addr, unsigned long count)
{
	struct page *p;
	int copied = 0;

	while (count) {
		unsigned long offset, length;

		offset = offset_in_page(addr);
		length = PAGE_SIZE - offset;
		if (length > count)
			length = count;
		p = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need
		 * lock. But adding lock here means that we need to add
		 * overhead of vmalloc()/vfree() calls for this _debug_
		 * interface, rarely used. Instead of that, we'll use
		 * kmap() and get small overhead in this access function.
		 */
		if (p) {
			/* We can expect USER0 is not used -- see vread() */
			void *map = kmap_atomic(p);
			memcpy(buf, map + offset, length);
			kunmap_atomic(map);
		} else
			memset(buf, 0, length);

		addr += length;
		buf += length;
		copied += length;
		count -= length;
	}
	return copied;
}

/**
 * vread() - read vmalloc area in a safe way.
 * @buf:     buffer for reading data
 * @addr:    vm address.
 * @count:   number of bytes to be read.
 *
 * This function checks that addr is a valid vmalloc'ed area, and
 * copy data from that area to a given buffer. If the given memory range
 * of [addr...addr+count) includes some valid address, data is copied to
 * proper area of @buf. If there are memory holes, they'll be zero-filled.
 * IOREMAP area is treated as memory hole and no copy is done.
 *
 * If [addr...addr+count) doesn't includes any intersects with alive
 * vm_struct area, returns 0. @buf should be kernel's buffer.
 *
 * Note: In usual ops, vread() is never necessary because the caller
 * should know vmalloc() area is valid and can use memcpy().
 * This is for routines which have to access vmalloc area without
 * any information, as /proc/kcore.
 *
 * Return: number of bytes for which addr and buf should be increased
 * (same number as @count) or %0 if [addr...addr+count) doesn't
 * include any intersection with valid vmalloc area
 */
long vread(char *buf, char *addr, unsigned long count)
{
	struct vmap_area *va;
	struct vm_struct *vm;
	char *vaddr, *buf_start = buf;
	unsigned long buflen = count;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	spin_lock(&vmap_area_lock);
	va = find_vmap_area_exceed_addr((unsigned long)addr);
	if (!va)
		goto finished;

	/* no intersects with alive vmap_area */
	if ((unsigned long)addr + count <= va->va_start)
		goto finished;

	list_for_each_entry_from(va, &vmap_area_list, list) {
		if (!count)
			break;

		if (!va->vm)
			continue;

		vm = va->vm;
		vaddr = (char *) vm->addr;
		if (addr >= vaddr + get_vm_area_size(vm))
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			*buf = '\0';
			buf++;
			addr++;
			count--;
		}
		n = vaddr + get_vm_area_size(vm) - addr;
		if (n > count)
			n = count;
		if (!(vm->flags & VM_IOREMAP))
			aligned_vread(buf, addr, n);
		else /* IOREMAP area is treated as memory hole */
			memset(buf, 0, n);
		buf += n;
		addr += n;
		count -= n;
	}
finished:
	spin_unlock(&vmap_area_lock);

	if (buf == buf_start)
		return 0;
	/* zero-fill memory holes */
	if (buf != buf_start + buflen)
		memset(buf, 0, buflen - (buf - buf_start));

	return buflen;
}

/**
 * remap_vmalloc_range_partial - map vmalloc pages to userspace
 * @vma:		vma to cover
 * @uaddr:		target user address to start at
 * @kaddr:		virtual address of vmalloc kernel memory
 * @pgoff:		offset from @kaddr to start at
 * @size:		size of map area
 *
 * Returns:	0 for success, -Exxx on failure
 *
 * This function checks that @kaddr is a valid vmalloc'ed area,
 * and that it is big enough to cover the range starting at
 * @uaddr in @vma. Will return failure if that criteria isn't
 * met.
 *
 * Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range_partial(struct vm_area_struct *vma, unsigned long uaddr,
				void *kaddr, unsigned long pgoff,
				unsigned long size)
{
	struct vm_struct *area;
	unsigned long off;
	unsigned long end_index;

	if (check_shl_overflow(pgoff, PAGE_SHIFT, &off))
		return -EINVAL;

	size = PAGE_ALIGN(size);

	if (!PAGE_ALIGNED(uaddr) || !PAGE_ALIGNED(kaddr))
		return -EINVAL;

	area = find_vm_area(kaddr);
	if (!area)
		return -EINVAL;

	if (!(area->flags & (VM_USERMAP | VM_DMA_COHERENT)))
		return -EINVAL;

	if (check_add_overflow(size, off, &end_index) ||
	    end_index > get_vm_area_size(area))
		return -EINVAL;
	kaddr += off;

	do {
		struct page *page = vmalloc_to_page(kaddr);
		int ret;

		ret = vm_insert_page(vma, uaddr, page);
		if (ret)
			return ret;

		uaddr += PAGE_SIZE;
		kaddr += PAGE_SIZE;
		size -= PAGE_SIZE;
	} while (size > 0);

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

	return 0;
}

/**
 * remap_vmalloc_range - map vmalloc pages to userspace
 * @vma:		vma to cover (map full range of vma)
 * @addr:		vmalloc memory
 * @pgoff:		number of pages into addr before first page to map
 *
 * Returns:	0 for success, -Exxx on failure
 *
 * This function checks that addr is a valid vmalloc'ed area, and
 * that it is big enough to cover the vma. Will return failure if
 * that criteria isn't met.
 *
 * Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
						unsigned long pgoff)
{
	return remap_vmalloc_range_partial(vma, vma->vm_start,
					   addr, pgoff,
					   vma->vm_end - vma->vm_start);
}
EXPORT_SYMBOL(remap_vmalloc_range);

void free_vm_area(struct vm_struct *area)
{
	struct vm_struct *ret;
	ret = remove_vm_area(area->addr);
	BUG_ON(ret != area);
	kfree(area);
}
EXPORT_SYMBOL_GPL(free_vm_area);

#ifdef CONFIG_SMP
static struct vmap_area *node_to_va(struct rb_node *n)
{
	return rb_entry_safe(n, struct vmap_area, rb_node);
}

/**
 * pvm_find_va_enclose_addr - find the vmap_area @addr belongs to
 * @addr: target address
 *
 * Returns: vmap_area if it is found. If there is no such area
 *   the first highest(reverse order) vmap_area is returned
 *   i.e. va->va_start < addr && va->va_end < addr or NULL
 *   if there are no any areas before @addr.
 */
static struct vmap_area *
pvm_find_va_enclose_addr(unsigned long addr)
{
	struct vmap_area *va, *tmp;
	struct rb_node *n;

	n = free_vmap_area_root.rb_node;
	va = NULL;

	while (n) {
		tmp = rb_entry(n, struct vmap_area, rb_node);
		if (tmp->va_start <= addr) {
			va = tmp;
			if (tmp->va_end >= addr)
				break;

			n = n->rb_right;
		} else {
			n = n->rb_left;
		}
	}

	return va;
}

/**
 * pvm_determine_end_from_reverse - find the highest aligned address
 * of free block below VMALLOC_END
 * @va:
 *   in - the VA we start the search(reverse order);
 *   out - the VA with the highest aligned end address.
 * @align: alignment for required highest address
 *
 * Returns: determined end address within vmap_area
 */
static unsigned long
pvm_determine_end_from_reverse(struct vmap_area **va, unsigned long align)
{
	unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	unsigned long addr;

	if (likely(*va)) {
		list_for_each_entry_from_reverse((*va),
				&free_vmap_area_list, list) {
			addr = min((*va)->va_end & ~(align - 1), vmalloc_end);
			if ((*va)->va_start < addr)
				return addr;
		}
	}

	return 0;
}

/**
 * pcpu_get_vm_areas - allocate vmalloc areas for percpu allocator
 * @offsets: array containing offset of each area
 * @sizes: array containing size of each area
 * @nr_vms: the number of areas to allocate
 * @align: alignment, all entries in @offsets and @sizes must be aligned to this
 *
 * Returns: kmalloc'd vm_struct pointer array pointing to allocated
 *	    vm_structs on success, %NULL on failure
 *
 * Percpu allocator wants to use congruent vm areas so that it can
 * maintain the offsets among percpu areas.  This function allocates
 * congruent vmalloc areas for it with GFP_KERNEL.  These areas tend to
 * be scattered pretty far, distance between two areas easily going up
 * to gigabytes.  To avoid interacting with regular vmallocs, these
 * areas are allocated from top.
 *
 * Despite its complicated look, this allocator is rather simple. It
 * does everything top-down and scans free blocks from the end looking
 * for matching base. While scanning, if any of the areas do not fit the
 * base address is pulled down to fit the area. Scanning is repeated till
 * all the areas fit and then all necessary data structures are inserted
 * and the result is returned.
 */
struct vm_struct **pcpu_get_vm_areas(const unsigned long *offsets,
				     const size_t *sizes, int nr_vms,
				     size_t align)
{
	const unsigned long vmalloc_start = ALIGN(VMALLOC_START, align);
	const unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	struct vmap_area **vas, *va;
	struct vm_struct **vms;
	int area, area2, last_area, term_area;
	unsigned long base, start, size, end, last_end, orig_start, orig_end;
	bool purged = false;
	enum fit_type type;

	/* verify parameters and allocate data structures */
	BUG_ON(offset_in_page(align) || !is_power_of_2(align));
	for (last_area = 0, area = 0; area < nr_vms; area++) {
		start = offsets[area];
		end = start + sizes[area];

		/* is everything aligned properly? */
		BUG_ON(!IS_ALIGNED(offsets[area], align));
		BUG_ON(!IS_ALIGNED(sizes[area], align));

		/* detect the area with the highest address */
		if (start > offsets[last_area])
			last_area = area;

		for (area2 = area + 1; area2 < nr_vms; area2++) {
			unsigned long start2 = offsets[area2];
			unsigned long end2 = start2 + sizes[area2];

			BUG_ON(start2 < end && start < end2);
		}
	}
	last_end = offsets[last_area] + sizes[last_area];

	if (vmalloc_end - vmalloc_start < last_end) {
		WARN_ON(true);
		return NULL;
	}

	vms = kcalloc(nr_vms, sizeof(vms[0]), GFP_KERNEL);
	vas = kcalloc(nr_vms, sizeof(vas[0]), GFP_KERNEL);
	if (!vas || !vms)
		goto err_free2;

	for (area = 0; area < nr_vms; area++) {
		vas[area] = kmem_cache_zalloc(vmap_area_cachep, GFP_KERNEL);
		vms[area] = kzalloc(sizeof(struct vm_struct), GFP_KERNEL);
		if (!vas[area] || !vms[area])
			goto err_free;
	}
retry:
	spin_lock(&free_vmap_area_lock);

	/* start scanning - we scan from the top, begin with the last area */
	area = term_area = last_area;
	start = offsets[area];
	end = start + sizes[area];

	va = pvm_find_va_enclose_addr(vmalloc_end);
	base = pvm_determine_end_from_reverse(&va, align) - end;

	while (true) {
		/*
		 * base might have underflowed, add last_end before
		 * comparing.
		 */
		if (base + last_end < vmalloc_start + last_end)
			goto overflow;

		/*
		 * Fitting base has not been found.
		 */
		if (va == NULL)
			goto overflow;

		/*
		 * If required width exceeds current VA block, move
		 * base downwards and then recheck.
		 */
		if (base + end > va->va_end) {
			base = pvm_determine_end_from_reverse(&va, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * If this VA does not fit, move base downwards and recheck.
		 */
		if (base + start < va->va_start) {
			va = node_to_va(rb_prev(&va->rb_node));
			base = pvm_determine_end_from_reverse(&va, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * This area fits, move on to the previous one.  If
		 * the previous one is the terminal one, we're done.
		 */
		area = (area + nr_vms - 1) % nr_vms;
		if (area == term_area)
			break;

		start = offsets[area];
		end = start + sizes[area];
		va = pvm_find_va_enclose_addr(base + end);
	}

	/* we've found a fitting base, insert all va's */
	for (area = 0; area < nr_vms; area++) {
		int ret;

		start = base + offsets[area];
		size = sizes[area];

		va = pvm_find_va_enclose_addr(start);
		if (WARN_ON_ONCE(va == NULL))
			/* It is a BUG(), but trigger recovery instead. */
			goto recovery;

		type = classify_va_fit_type(va, start, size);
		if (WARN_ON_ONCE(type == NOTHING_FIT))
			/* It is a BUG(), but trigger recovery instead. */
			goto recovery;

		ret = adjust_va_to_fit_type(va, start, size, type);
		if (unlikely(ret))
			goto recovery;

		/* Allocated area. */
		va = vas[area];
		va->va_start = start;
		va->va_end = start + size;
	}

	spin_unlock(&free_vmap_area_lock);

	/* populate the kasan shadow space */
	for (area = 0; area < nr_vms; area++) {
		if (kasan_populate_vmalloc(vas[area]->va_start, sizes[area]))
			goto err_free_shadow;

		kasan_unpoison_vmalloc((void *)vas[area]->va_start,
				       sizes[area]);
	}

	/* insert all vm's */
	spin_lock(&vmap_area_lock);
	for (area = 0; area < nr_vms; area++) {
		insert_vmap_area(vas[area], &vmap_area_root, &vmap_area_list);

		setup_vmalloc_vm_locked(vms[area], vas[area], VM_ALLOC,
				 pcpu_get_vm_areas);
	}
	spin_unlock(&vmap_area_lock);

	kfree(vas);
	return vms;

recovery:
	/*
	 * Remove previously allocated areas. There is no
	 * need in removing these areas from the busy tree,
	 * because they are inserted only on the final step
	 * and when pcpu_get_vm_areas() is success.
	 */
	while (area--) {
		orig_start = vas[area]->va_start;
		orig_end = vas[area]->va_end;
		va = merge_or_add_vmap_area_augment(vas[area], &free_vmap_area_root,
				&free_vmap_area_list);
		if (va)
			kasan_release_vmalloc(orig_start, orig_end,
				va->va_start, va->va_end);
		vas[area] = NULL;
	}

overflow:
	spin_unlock(&free_vmap_area_lock);
	if (!purged) {
		purge_vmap_area_lazy();
		purged = true;

		/* Before "retry", check if we recover. */
		for (area = 0; area < nr_vms; area++) {
			if (vas[area])
				continue;

			vas[area] = kmem_cache_zalloc(
				vmap_area_cachep, GFP_KERNEL);
			if (!vas[area])
				goto err_free;
		}

		goto retry;
	}

err_free:
	for (area = 0; area < nr_vms; area++) {
		if (vas[area])
			kmem_cache_free(vmap_area_cachep, vas[area]);

		kfree(vms[area]);
	}
err_free2:
	kfree(vas);
	kfree(vms);
	return NULL;

err_free_shadow:
	spin_lock(&free_vmap_area_lock);
	/*
	 * We release all the vmalloc shadows, even the ones for regions that
	 * hadn't been successfully added. This relies on kasan_release_vmalloc
	 * being able to tolerate this case.
	 */
	for (area = 0; area < nr_vms; area++) {
		orig_start = vas[area]->va_start;
		orig_end = vas[area]->va_end;
		va = merge_or_add_vmap_area_augment(vas[area], &free_vmap_area_root,
				&free_vmap_area_list);
		if (va)
			kasan_release_vmalloc(orig_start, orig_end,
				va->va_start, va->va_end);
		vas[area] = NULL;
		kfree(vms[area]);
	}
	spin_unlock(&free_vmap_area_lock);
	kfree(vas);
	kfree(vms);
	return NULL;
}

/**
 * pcpu_free_vm_areas - free vmalloc areas for percpu allocator
 * @vms: vm_struct pointer array returned by pcpu_get_vm_areas()
 * @nr_vms: the number of allocated areas
 *
 * Free vm_structs and the array allocated by pcpu_get_vm_areas().
 */
void pcpu_free_vm_areas(struct vm_struct **vms, int nr_vms)
{
	int i;

	for (i = 0; i < nr_vms; i++)
		free_vm_area(vms[i]);
	kfree(vms);
}
#endif	/* CONFIG_SMP */

#ifdef CONFIG_PRINTK
bool vmalloc_dump_obj(void *object)
{
	struct vm_struct *vm;
	void *objp = (void *)PAGE_ALIGN((unsigned long)object);

	vm = find_vm_area(objp);
	if (!vm)
		return false;
	pr_cont(" %u-page vmalloc region starting at %#lx allocated at %pS\n",
		vm->nr_pages, (unsigned long)vm->addr, vm->caller);
	return true;
}
#endif

#ifdef CONFIG_PROC_FS
static void *s_start(struct seq_file *m, loff_t *pos)
	__acquires(&vmap_purge_lock)
	__acquires(&vmap_area_lock)
{
	mutex_lock(&vmap_purge_lock);
	spin_lock(&vmap_area_lock);

	return seq_list_start(&vmap_area_list, *pos);
}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	return seq_list_next(p, &vmap_area_list, pos);
}

static void s_stop(struct seq_file *m, void *p)
	__releases(&vmap_area_lock)
	__releases(&vmap_purge_lock)
{
	spin_unlock(&vmap_area_lock);
	mutex_unlock(&vmap_purge_lock);
}

static void show_numa_info(struct seq_file *m, struct vm_struct *v)
{
	if (IS_ENABLED(CONFIG_NUMA)) {
		unsigned int nr, *counters = m->private;

		if (!counters)
			return;

		if (v->flags & VM_UNINITIALIZED)
			return;
		/* Pair with smp_wmb() in clear_vm_uninitialized_flag() */
		smp_rmb();

		memset(counters, 0, nr_node_ids * sizeof(unsigned int));

		for (nr = 0; nr < v->nr_pages; nr++)
			counters[page_to_nid(v->pages[nr])]++;

		for_each_node_state(nr, N_HIGH_MEMORY)
			if (counters[nr])
				seq_printf(m, " N%u=%u", nr, counters[nr]);
	}
}

static void show_purge_info(struct seq_file *m)
{
	struct vmap_area *va;

	spin_lock(&purge_vmap_area_lock);
	list_for_each_entry(va, &purge_vmap_area_list, list) {
		seq_printf(m, "0x%pK-0x%pK %7ld unpurged vm_area\n",
			(void *)va->va_start, (void *)va->va_end,
			va->va_end - va->va_start);
	}
	spin_unlock(&purge_vmap_area_lock);
}

static int s_show(struct seq_file *m, void *p)
{
	struct vmap_area *va;
	struct vm_struct *v;

	va = list_entry(p, struct vmap_area, list);

	/*
	 * s_show can encounter race with remove_vm_area, !vm on behalf
	 * of vmap area is being tear down or vm_map_ram allocation.
	 */
	if (!va->vm) {
		seq_printf(m, "0x%pK-0x%pK %7ld vm_map_ram\n",
			(void *)va->va_start, (void *)va->va_end,
			va->va_end - va->va_start);

		return 0;
	}

	v = va->vm;

	seq_printf(m, "0x%pK-0x%pK %7ld",
		v->addr, v->addr + v->size, v->size);

	if (v->caller)
		seq_printf(m, " %pS", v->caller);

	if (v->nr_pages)
		seq_printf(m, " pages=%d", v->nr_pages);

	if (v->phys_addr)
		seq_printf(m, " phys=%pa", &v->phys_addr);

	if (v->flags & VM_IOREMAP)
		seq_puts(m, " ioremap");

	if (v->flags & VM_ALLOC)
		seq_puts(m, " vmalloc");

	if (v->flags & VM_MAP)
		seq_puts(m, " vmap");

	if (v->flags & VM_USERMAP)
		seq_puts(m, " user");

	if (v->flags & VM_DMA_COHERENT)
		seq_puts(m, " dma-coherent");

	if (is_vmalloc_addr(v->pages))
		seq_puts(m, " vpages");

	show_numa_info(m, v);
	seq_putc(m, '\n');

	/*
	 * As a final step, dump "unpurged" areas.
	 */
	if (list_is_last(&va->list, &vmap_area_list))
		show_purge_info(m);

	return 0;
}

static const struct seq_operations vmalloc_op = {
	.start = s_start,
	.next = s_next,
	.stop = s_stop,
	.show = s_show,
};

/*
 * IAMROOT, 2022.07.02: 
 * 
 * $ echo 1 > /proc/sys/kernel/kptr_restrict (root 권한 필요)
 * $ cat /proc/vmallocinfo
 *
 * 0xffff800000000000-0xffff800000002000    8192 bpf_jit_binary_alloc+0xa8/0x124 pages=1 vmalloc N0=1
 * 0xffff800000002000-0xffff800000004000    8192 bpf_jit_binary_alloc+0xa8/0x124 pages=1 vmalloc N0=1
 */
static int __init proc_vmalloc_init(void)
{
	if (IS_ENABLED(CONFIG_NUMA))
		proc_create_seq_private("vmallocinfo", 0400, NULL,
				&vmalloc_op,
				nr_node_ids * sizeof(unsigned int), NULL);
	else
		proc_create_seq("vmallocinfo", 0400, NULL, &vmalloc_op);
	return 0;
}
module_init(proc_vmalloc_init);

#endif
