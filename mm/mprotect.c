// SPDX-License-Identifier: GPL-2.0
/*
 *  mm/mprotect.c
 *
 *  (C) Copyright 1994 Linus Torvalds
 *  (C) Copyright 2002 Christoph Hellwig
 *
 *  Address space accounting code	<alan@lxorguk.ukuu.org.uk>
 *  (C) Copyright 2002 Red Hat Inc, All Rights Reserved
 */

#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/mempolicy.h>
#include <linux/personality.h>
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/perf_event.h>
#include <linux/pkeys.h>
#include <linux/ksm.h>
#include <linux/uaccess.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>

#include "internal.h"

/*
 * IAMROOT, 2023.06.17:
 * - numa fault page 전환인 경우(change_prot_numa() 확인)
 *   @cp_flags  MM_CP_PROT_NUMA.
 *   @newprot PAGE_NONE
 *   @addr부터 @end까지에 대해 다음을 처리한다.
 *   1. old를 가져오면서 0 clear를 한다.(modify중 잠깐)
 *   2. hw dirty가 발생했을경우 sw dirty를 기록해준다.
 *   3. @newprot(PTE_PROT_NONE)를 설정해준다.(기존 oldpte중에 일부 bit는 가져온다.)
 */
static unsigned long change_pte_range(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long addr, unsigned long end, pgprot_t newprot,
		unsigned long cp_flags)
{
	pte_t *pte, oldpte;
	spinlock_t *ptl;
	unsigned long pages = 0;
	int target_node = NUMA_NO_NODE;
	bool dirty_accountable = cp_flags & MM_CP_DIRTY_ACCT;
	bool prot_numa = cp_flags & MM_CP_PROT_NUMA;
	bool uffd_wp = cp_flags & MM_CP_UFFD_WP;
	bool uffd_wp_resolve = cp_flags & MM_CP_UFFD_WP_RESOLVE;

	/*
	 * Can be called with only the mmap_lock for reading by
	 * prot_numa so we must check the pmd isn't constantly
	 * changing from under us from pmd_none to pmd_trans_huge
	 * and/or the other way around.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   prot_numa에서 읽기 위해 mmap_lock으로만 호출할 수 있으므로 
 *   pmd가 계속해서 pmd_none에서 pmd_trans_huge로 변경되거나 
 *   그 반대가 아닌지 확인해야 합니다.
 */
	if (pmd_trans_unstable(pmd))
		return 0;

	/*
	 * The pmd points to a regular pte so the pmd can't change
	 * from under us even if the mmap_lock is only hold for
	 * reading.
	 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   pmd는 일반 pte를 가리키므로 mmap_lock이 읽기 전용인 경우에도 
 *   pmd가 변경되지 않습니다.
 */
	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);

	/* Get target node for single threaded private VMAs */
/*
 * IAMROOT, 2023.06.17:
 * - numa fault일때 single task이면 this cpu node로 옮긴다.
 */
	if (prot_numa && !(vma->vm_flags & VM_SHARED) &&
	    atomic_read(&vma->vm_mm->mm_users) == 1)
		target_node = numa_node_id();

	flush_tlb_batched_pending(vma->vm_mm);
	arch_enter_lazy_mmu_mode();
/*
 * IAMROOT, 2023.06.24:
 * - numa fault인 경우
 *  1. old를 가져오면서 0 clear를 한다.
 *  2. hw dirty가 발생했을경우 sw dirty를 기록해준다.
 *  3. @newprot(PTE_PROT_NONE)중 pte_modity()의 mask에 해당하는 부분들을 old에서 update한다.
 */
	do {
		oldpte = *pte;
/*
 * IAMROOT, 2023.06.17:
 * - pte가 vaild하거나 numa scan중인 경우
 *   numa fault page는 원래 mapping이 안되있는 상태이지만 잠시 지운 상태의 개념이므로
 *   마치 entry가 있는것처럼 취급한다.
 */
		if (pte_present(oldpte)) {
			pte_t ptent;
			bool preserve_write = prot_numa && pte_write(oldpte);

			/*
			 * Avoid trapping faults against the zero or KSM
			 * pages. See similar comment in change_huge_pmd.
			 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   0 또는 KSM 페이지에 대한 트래핑 fault를 피하십시오. 
 *   change_huge_pmd에서 유사한 설명을 참조하십시오.
 */
			if (prot_numa) {
				struct page *page;

				/* Avoid TLB flush if possible */
/*
 * IAMROOT, 2023.06.17:
 * - 이미 numa fault 되있다. skip
 */
				if (pte_protnone(oldpte))
					continue;

/*
 * IAMROOT, 2023.06.17:
 * - phy page얻어옴
 */
				page = vm_normal_page(vma, addr, oldpte);
				if (!page || PageKsm(page))
					continue;

				/* Also skip shared copy-on-write pages */
/*
 * IAMROOT, 2023.06.17:
 * - cow이면서 mapcount면 skip. 1: N의 mapping이라 cost가 너무 크다.
 */
				if (is_cow_mapping(vma->vm_flags) &&
				    page_mapcount(page) != 1)
					continue;

				/*
				 * While migration can move some dirty pages,
				 * it cannot move them all from MIGRATE_ASYNC
				 * context.
				 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   마이그레이션은 일부 더러운 페이지를 이동할 수 있지만 
 *   MIGRATE_ASYNC 컨텍스트에서 모두 이동할 수는 없습니다.
 * - dirty는 memory가 정말 부족할때만 이동하는 개념이다. cost가 너무 높다.
 *   skip
 */
				if (page_is_file_lru(page) && PageDirty(page))
					continue;

				/*
				 * Don't mess with PTEs if page is already on the node
				 * a single-threaded process is running on.
				 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   단일 스레드 프로세스가 실행 중인 노드에 페이지가 이미 있는 경우 PTE를 
 *   건드리지 마십시오.
 * - single task이면 this node에 이미 있는경우 옮길 필요없으므로 skip 
 */
				if (target_node == page_to_nid(page))
					continue;
			}

/*
 * IAMROOT, 2023.06.17:
 * - old를 가져오면서 0 clear를 한다.
 */
			oldpte = ptep_modify_prot_start(vma, addr, pte);
/*
 * IAMROOT, 2023.06.24:
 * - mask (PTE_USER | PTE_PXN | PTE_UXN | PTE_RDONLY | 
 *   PTE_PROT_NONE | PTE_VALID | PTE_WRITE | PTE_GP |
 *   PTE_ATTRINDX_MASK)에서 @newprot에 포함된것들을 oldpte에
 *   추가해서 가져온다. 그리고 oldpte가 hw dirty라면 sw dirty를
 *   추가한다.
 */
			ptent = pte_modify(oldpte, newprot);
/*
 * IAMROOT, 2023.06.24:
 * - numa fault면서 hw dirty였으면 rdonly를 지우는 개념으로 해준다.
 */
			if (preserve_write)
				ptent = pte_mk_savedwrite(ptent);

/*
 * IAMROOT, 2023.06.24:
 * - uffd pass
 */
			if (uffd_wp) {
				ptent = pte_wrprotect(ptent);
				ptent = pte_mkuffd_wp(ptent);
			} else if (uffd_wp_resolve) {
				/*
				 * Leave the write bit to be handled
				 * by PF interrupt handler, then
				 * things like COW could be properly
				 * handled.
				 */
				ptent = pte_clear_uffd_wp(ptent);
			}

			/* Avoid taking write faults for known dirty pages */
/*
 * IAMROOT, 2023.06.24:
 * - dirty account pass
 */
			if (dirty_accountable && pte_dirty(ptent) &&
					(pte_soft_dirty(ptent) ||
					 !(vma->vm_flags & VM_SOFTDIRTY))) {
				ptent = pte_mkwrite(ptent);
			}
/*
 * IAMROOT, 2023.06.24:
 * - pte에 ptent를 최종적으로 기록한다.
 */
			ptep_modify_prot_commit(vma, addr, pte, oldpte, ptent);
			pages++;
		} else if (is_swap_pte(oldpte)) {
			swp_entry_t entry = pte_to_swp_entry(oldpte);
			pte_t newpte;

			if (is_writable_migration_entry(entry)) {
				/*
				 * A protection check is difficult so
				 * just be safe and disable write
				 */
				entry = make_readable_migration_entry(
							swp_offset(entry));
				newpte = swp_entry_to_pte(entry);
				if (pte_swp_soft_dirty(oldpte))
					newpte = pte_swp_mksoft_dirty(newpte);
				if (pte_swp_uffd_wp(oldpte))
					newpte = pte_swp_mkuffd_wp(newpte);
			} else if (is_writable_device_private_entry(entry)) {
				/*
				 * We do not preserve soft-dirtiness. See
				 * copy_one_pte() for explanation.
				 */
				entry = make_readable_device_private_entry(
							swp_offset(entry));
				newpte = swp_entry_to_pte(entry);
				if (pte_swp_uffd_wp(oldpte))
					newpte = pte_swp_mkuffd_wp(newpte);
			} else if (is_writable_device_exclusive_entry(entry)) {
				entry = make_readable_device_exclusive_entry(
							swp_offset(entry));
				newpte = swp_entry_to_pte(entry);
				if (pte_swp_soft_dirty(oldpte))
					newpte = pte_swp_mksoft_dirty(newpte);
				if (pte_swp_uffd_wp(oldpte))
					newpte = pte_swp_mkuffd_wp(newpte);
			} else {
				newpte = oldpte;
			}

			if (uffd_wp)
				newpte = pte_swp_mkuffd_wp(newpte);
			else if (uffd_wp_resolve)
				newpte = pte_swp_clear_uffd_wp(newpte);

			if (!pte_same(oldpte, newpte)) {
				set_pte_at(vma->vm_mm, addr, pte, newpte);
				pages++;
			}
		}
	} while (pte++, addr += PAGE_SIZE, addr != end);
	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(pte - 1, ptl);

	return pages;
}

/*
 * Used when setting automatic NUMA hinting protection where it is
 * critical that a numa hinting PMD is not confused with a bad PMD.
 */
static inline int pmd_none_or_clear_bad_unless_trans_huge(pmd_t *pmd)
{
	pmd_t pmdval = pmd_read_atomic(pmd);

	/* See pmd_none_or_trans_huge_or_clear_bad for info on barrier */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	barrier();
#endif

	if (pmd_none(pmdval))
		return 1;
	if (pmd_trans_huge(pmdval))
		return 0;
	if (unlikely(pmd_bad(pmdval))) {
		pmd_clear_bad(pmd);
		return 1;
	}

	return 0;
}

/*
 * IAMROOT, 2023.06.17:
 * - pte를 순회한다.
 */
static inline unsigned long change_pmd_range(struct vm_area_struct *vma,
		pud_t *pud, unsigned long addr, unsigned long end,
		pgprot_t newprot, unsigned long cp_flags)
{
	pmd_t *pmd;
	unsigned long next;
	unsigned long pages = 0;
	unsigned long nr_huge_updates = 0;
	struct mmu_notifier_range range;

	range.start = 0;

	pmd = pmd_offset(pud, addr);
	do {
		unsigned long this_pages;

		next = pmd_addr_end(addr, end);

		/*
		 * Automatic NUMA balancing walks the tables with mmap_lock
		 * held for read. It's possible a parallel update to occur
		 * between pmd_trans_huge() and a pmd_none_or_clear_bad()
		 * check leading to a false positive and clearing.
		 * Hence, it's necessary to atomically read the PMD value
		 * for all the checks.
		 */
/*
 * IAMROOT, 2023.06.17:
 * - papago
 *   자동 NUMA 밸런싱은 mmap_lock이 읽기용으로 유지된 상태에서 테이블을 
 *   탐색합니다. pmd_trans_huge()와 pmd_none_or_clear_bad() 검사 사이에 
 *   병렬 업데이트가 발생하여 거짓 긍정 및 지우기로 이어질 수 있습니다. 
 *   따라서 모든 검사에 대해 PMD 값을 원자적으로 읽어야 합니다.
 */
		if (!is_swap_pmd(*pmd) && !pmd_devmap(*pmd) &&
		     pmd_none_or_clear_bad_unless_trans_huge(pmd))
			goto next;

		/* invoke the mmu notifier if the pmd is populated */
		if (!range.start) {
			mmu_notifier_range_init(&range,
				MMU_NOTIFY_PROTECTION_VMA, 0,
				vma, vma->vm_mm, addr, end);
			mmu_notifier_invalidate_range_start(&range);
		}

		if (is_swap_pmd(*pmd) || pmd_trans_huge(*pmd) || pmd_devmap(*pmd)) {
			if (next - addr != HPAGE_PMD_SIZE) {
				__split_huge_pmd(vma, pmd, addr, false, NULL);
			} else {
				int nr_ptes = change_huge_pmd(vma, pmd, addr,
							      newprot, cp_flags);

				if (nr_ptes) {
					if (nr_ptes == HPAGE_PMD_NR) {
						pages += HPAGE_PMD_NR;
						nr_huge_updates++;
					}

					/* huge pmd was handled */
					goto next;
				}
			}
			/* fall through, the trans huge pmd just split */
		}
		this_pages = change_pte_range(vma, pmd, addr, next, newprot,
					      cp_flags);
		pages += this_pages;
next:
		cond_resched();
	} while (pmd++, addr = next, addr != end);

	if (range.start)
		mmu_notifier_invalidate_range_end(&range);

	if (nr_huge_updates)
		count_vm_numa_events(NUMA_HUGE_PTE_UPDATES, nr_huge_updates);
	return pages;
}

/*
 * IAMROOT, 2023.06.17:
 * - pmd를 순회한다.
 */
static inline unsigned long change_pud_range(struct vm_area_struct *vma,
		p4d_t *p4d, unsigned long addr, unsigned long end,
		pgprot_t newprot, unsigned long cp_flags)
{
	pud_t *pud;
	unsigned long next;
	unsigned long pages = 0;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		pages += change_pmd_range(vma, pud, addr, next, newprot,
					  cp_flags);
	} while (pud++, addr = next, addr != end);

	return pages;
}

/*
 * IAMROOT, 2023.06.17:
 * - pud를 순회한다
 */
static inline unsigned long change_p4d_range(struct vm_area_struct *vma,
		pgd_t *pgd, unsigned long addr, unsigned long end,
		pgprot_t newprot, unsigned long cp_flags)
{
	p4d_t *p4d;
	unsigned long next;
	unsigned long pages = 0;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;
		pages += change_pud_range(vma, p4d, addr, next, newprot,
					  cp_flags);
	} while (p4d++, addr = next, addr != end);

	return pages;
}

/*
 * IAMROOT, 2023.06.17:
 * - p4d를 순회한다.
 */
static unsigned long change_protection_range(struct vm_area_struct *vma,
		unsigned long addr, unsigned long end, pgprot_t newprot,
		unsigned long cp_flags)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	unsigned long next;
	unsigned long start = addr;
	unsigned long pages = 0;

	BUG_ON(addr >= end);
	pgd = pgd_offset(mm, addr);
	flush_cache_range(vma, addr, end);
	inc_tlb_flush_pending(mm);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		pages += change_p4d_range(vma, pgd, addr, next, newprot,
					  cp_flags);
	} while (pgd++, addr = next, addr != end);

	/* Only flush the TLB if we actually modified any entries: */
	if (pages)
		flush_tlb_range(vma, start, end);
	dec_tlb_flush_pending(mm);

	return pages;
}

/*
 * IAMROOT, 2023.06.17:
 * - @start ~ @end까지 @newprot를 적용한다.
 */
unsigned long change_protection(struct vm_area_struct *vma, unsigned long start,
		       unsigned long end, pgprot_t newprot,
		       unsigned long cp_flags)
{
	unsigned long pages;

	BUG_ON((cp_flags & MM_CP_UFFD_WP_ALL) == MM_CP_UFFD_WP_ALL);

	if (is_vm_hugetlb_page(vma))
		pages = hugetlb_change_protection(vma, start, end, newprot);
	else
		pages = change_protection_range(vma, start, end, newprot,
						cp_flags);

	return pages;
}

static int prot_none_pte_entry(pte_t *pte, unsigned long addr,
			       unsigned long next, struct mm_walk *walk)
{
	return pfn_modify_allowed(pte_pfn(*pte), *(pgprot_t *)(walk->private)) ?
		0 : -EACCES;
}

static int prot_none_hugetlb_entry(pte_t *pte, unsigned long hmask,
				   unsigned long addr, unsigned long next,
				   struct mm_walk *walk)
{
	return pfn_modify_allowed(pte_pfn(*pte), *(pgprot_t *)(walk->private)) ?
		0 : -EACCES;
}

static int prot_none_test(unsigned long addr, unsigned long next,
			  struct mm_walk *walk)
{
	return 0;
}

static const struct mm_walk_ops prot_none_walk_ops = {
	.pte_entry		= prot_none_pte_entry,
	.hugetlb_entry		= prot_none_hugetlb_entry,
	.test_walk		= prot_none_test,
};

int
mprotect_fixup(struct vm_area_struct *vma, struct vm_area_struct **pprev,
	unsigned long start, unsigned long end, unsigned long newflags)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long oldflags = vma->vm_flags;
	long nrpages = (end - start) >> PAGE_SHIFT;
	unsigned long charged = 0;
	pgoff_t pgoff;
	int error;
	int dirty_accountable = 0;

	if (newflags == oldflags) {
		*pprev = vma;
		return 0;
	}

	/*
	 * Do PROT_NONE PFN permission checks here when we can still
	 * bail out without undoing a lot of state. This is a rather
	 * uncommon case, so doesn't need to be very optimized.
	 */
	if (arch_has_pfn_modify_check() &&
	    (vma->vm_flags & (VM_PFNMAP|VM_MIXEDMAP)) &&
	    (newflags & VM_ACCESS_FLAGS) == 0) {
		pgprot_t new_pgprot = vm_get_page_prot(newflags);

		error = walk_page_range(current->mm, start, end,
				&prot_none_walk_ops, &new_pgprot);
		if (error)
			return error;
	}

	/*
	 * If we make a private mapping writable we increase our commit;
	 * but (without finer accounting) cannot reduce our commit if we
	 * make it unwritable again. hugetlb mapping were accounted for
	 * even if read-only so there is no need to account for them here
	 */
	if (newflags & VM_WRITE) {
		/* Check space limits when area turns into data. */
		if (!may_expand_vm(mm, newflags, nrpages) &&
				may_expand_vm(mm, oldflags, nrpages))
			return -ENOMEM;
		if (!(oldflags & (VM_ACCOUNT|VM_WRITE|VM_HUGETLB|
						VM_SHARED|VM_NORESERVE))) {
			charged = nrpages;
			if (security_vm_enough_memory_mm(mm, charged))
				return -ENOMEM;
			newflags |= VM_ACCOUNT;
		}
	}

	/*
	 * First try to merge with previous and/or next vma.
	 */
	pgoff = vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT);
	*pprev = vma_merge(mm, *pprev, start, end, newflags,
			   vma->anon_vma, vma->vm_file, pgoff, vma_policy(vma),
			   vma->vm_userfaultfd_ctx);
	if (*pprev) {
		vma = *pprev;
		VM_WARN_ON((vma->vm_flags ^ newflags) & ~VM_SOFTDIRTY);
		goto success;
	}

	*pprev = vma;

	if (start != vma->vm_start) {
		error = split_vma(mm, vma, start, 1);
		if (error)
			goto fail;
	}

	if (end != vma->vm_end) {
		error = split_vma(mm, vma, end, 0);
		if (error)
			goto fail;
	}

success:
	/*
	 * vm_flags and vm_page_prot are protected by the mmap_lock
	 * held in write mode.
	 */
	vma->vm_flags = newflags;
	dirty_accountable = vma_wants_writenotify(vma, vma->vm_page_prot);
	vma_set_page_prot(vma);

	change_protection(vma, start, end, vma->vm_page_prot,
			  dirty_accountable ? MM_CP_DIRTY_ACCT : 0);

	/*
	 * Private VM_LOCKED VMA becoming writable: trigger COW to avoid major
	 * fault on access.
	 */
	if ((oldflags & (VM_WRITE | VM_SHARED | VM_LOCKED)) == VM_LOCKED &&
			(newflags & VM_WRITE)) {
		populate_vma_page_range(vma, start, end, NULL);
	}

	vm_stat_account(mm, oldflags, -nrpages);
	vm_stat_account(mm, newflags, nrpages);
	perf_event_mmap(vma);
	return 0;

fail:
	vm_unacct_memory(charged);
	return error;
}

/*
 * pkey==-1 when doing a legacy mprotect()
 */
static int do_mprotect_pkey(unsigned long start, size_t len,
		unsigned long prot, int pkey)
{
	unsigned long nstart, end, tmp, reqprot;
	struct vm_area_struct *vma, *prev;
	int error = -EINVAL;
	const int grows = prot & (PROT_GROWSDOWN|PROT_GROWSUP);
	const bool rier = (current->personality & READ_IMPLIES_EXEC) &&
				(prot & PROT_READ);

	start = untagged_addr(start);

	prot &= ~(PROT_GROWSDOWN|PROT_GROWSUP);
	if (grows == (PROT_GROWSDOWN|PROT_GROWSUP)) /* can't be both */
		return -EINVAL;

	if (start & ~PAGE_MASK)
		return -EINVAL;
	if (!len)
		return 0;
	len = PAGE_ALIGN(len);
	end = start + len;
	if (end <= start)
		return -ENOMEM;
	if (!arch_validate_prot(prot, start))
		return -EINVAL;

	reqprot = prot;

	if (mmap_write_lock_killable(current->mm))
		return -EINTR;

	/*
	 * If userspace did not allocate the pkey, do not let
	 * them use it here.
	 */
	error = -EINVAL;
	if ((pkey != -1) && !mm_pkey_is_allocated(current->mm, pkey))
		goto out;

	vma = find_vma(current->mm, start);
	error = -ENOMEM;
	if (!vma)
		goto out;
	prev = vma->vm_prev;
	if (unlikely(grows & PROT_GROWSDOWN)) {
		if (vma->vm_start >= end)
			goto out;
		start = vma->vm_start;
		error = -EINVAL;
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto out;
	} else {
		if (vma->vm_start > start)
			goto out;
		if (unlikely(grows & PROT_GROWSUP)) {
			end = vma->vm_end;
			error = -EINVAL;
			if (!(vma->vm_flags & VM_GROWSUP))
				goto out;
		}
	}
	if (start > vma->vm_start)
		prev = vma;

	for (nstart = start ; ; ) {
		unsigned long mask_off_old_flags;
		unsigned long newflags;
		int new_vma_pkey;

		/* Here we know that vma->vm_start <= nstart < vma->vm_end. */

		/* Does the application expect PROT_READ to imply PROT_EXEC */
		if (rier && (vma->vm_flags & VM_MAYEXEC))
			prot |= PROT_EXEC;

		/*
		 * Each mprotect() call explicitly passes r/w/x permissions.
		 * If a permission is not passed to mprotect(), it must be
		 * cleared from the VMA.
		 */
		mask_off_old_flags = VM_READ | VM_WRITE | VM_EXEC |
					VM_FLAGS_CLEAR;

		new_vma_pkey = arch_override_mprotect_pkey(vma, prot, pkey);
		newflags = calc_vm_prot_bits(prot, new_vma_pkey);
		newflags |= (vma->vm_flags & ~mask_off_old_flags);

		/* newflags >> 4 shift VM_MAY% in place of VM_% */
		if ((newflags & ~(newflags >> 4)) & VM_ACCESS_FLAGS) {
			error = -EACCES;
			goto out;
		}

		/* Allow architectures to sanity-check the new flags */
		if (!arch_validate_flags(newflags)) {
			error = -EINVAL;
			goto out;
		}

		error = security_file_mprotect(vma, reqprot, prot);
		if (error)
			goto out;

		tmp = vma->vm_end;
		if (tmp > end)
			tmp = end;

		if (vma->vm_ops && vma->vm_ops->mprotect) {
			error = vma->vm_ops->mprotect(vma, nstart, tmp, newflags);
			if (error)
				goto out;
		}

		error = mprotect_fixup(vma, &prev, nstart, tmp, newflags);
		if (error)
			goto out;

		nstart = tmp;

		if (nstart < prev->vm_end)
			nstart = prev->vm_end;
		if (nstart >= end)
			goto out;

		vma = prev->vm_next;
		if (!vma || vma->vm_start != nstart) {
			error = -ENOMEM;
			goto out;
		}
		prot = reqprot;
	}
out:
	mmap_write_unlock(current->mm);
	return error;
}

SYSCALL_DEFINE3(mprotect, unsigned long, start, size_t, len,
		unsigned long, prot)
{
	return do_mprotect_pkey(start, len, prot, -1);
}

#ifdef CONFIG_ARCH_HAS_PKEYS

SYSCALL_DEFINE4(pkey_mprotect, unsigned long, start, size_t, len,
		unsigned long, prot, int, pkey)
{
	return do_mprotect_pkey(start, len, prot, pkey);
}

SYSCALL_DEFINE2(pkey_alloc, unsigned long, flags, unsigned long, init_val)
{
	int pkey;
	int ret;

	/* No flags supported yet. */
	if (flags)
		return -EINVAL;
	/* check for unsupported init values */
	if (init_val & ~PKEY_ACCESS_MASK)
		return -EINVAL;

	mmap_write_lock(current->mm);
	pkey = mm_pkey_alloc(current->mm);

	ret = -ENOSPC;
	if (pkey == -1)
		goto out;

	ret = arch_set_user_pkey_access(current, pkey, init_val);
	if (ret) {
		mm_pkey_free(current->mm, pkey);
		goto out;
	}
	ret = pkey;
out:
	mmap_write_unlock(current->mm);
	return ret;
}

SYSCALL_DEFINE1(pkey_free, int, pkey)
{
	int ret;

	mmap_write_lock(current->mm);
	ret = mm_pkey_free(current->mm, pkey);
	mmap_write_unlock(current->mm);

	/*
	 * We could provide warnings or errors if any VMA still
	 * has the pkey set here.
	 */
	return ret;
}

#endif /* CONFIG_ARCH_HAS_PKEYS */
