// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/mmu.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/cache.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <linux/sizes.h>
#include <asm/tlb.h>
#include <asm/mmu_context.h>
#include <asm/ptdump.h>
#include <asm/tlbflush.h>
#include <asm/pgalloc.h>

#define NO_BLOCK_MAPPINGS	BIT(0)
#define NO_CONT_MAPPINGS	BIT(1)
#define NO_EXEC_MAPPINGS	BIT(2)	/* assumes FEAT_HPDS is not used */

u64 idmap_t0sz = TCR_T0SZ(VA_BITS_MIN);
u64 idmap_ptrs_per_pgd = PTRS_PER_PGD;

u64 __section(".mmuoff.data.write") vabits_actual;
EXPORT_SYMBOL(vabits_actual);

/* IAMROOT, 2021.10.02:
 * - randomize 결과로 나온 offset을 저장하는 변수
 */
u64 kimage_voffset __ro_after_init;
EXPORT_SYMBOL(kimage_voffset);

/*
 * IAMROOT, 2021.10.16:
 * - 처음에 user memory를 할당할때 이 page를 가리키게 한다.
 *   한개 페이지가 0만이 존재한다.
 *
 * - read fault가 들어오면 이 페이지를 할당한다.
 *
 * - write fault가 들어오면 이 페이지가 아닌 실제 페이지를 할당해 준다.
 *
 * - ttbr0를 비울때 빈 page table 용도로도 사용한다.
 */
/*
 * Empty_zero_page is a special page that is used for zero-initialized data
 * and COW.
 */
unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] __page_aligned_bss;
EXPORT_SYMBOL(empty_zero_page);

static pte_t bm_pte[PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss __maybe_unused;

static DEFINE_SPINLOCK(swapper_pgdir_lock);

void set_swapper_pgd(pgd_t *pgdp, pgd_t pgd)
{
	pgd_t *fixmap_pgdp;

	spin_lock(&swapper_pgdir_lock);
	fixmap_pgdp = pgd_set_fixmap(__pa_symbol(pgdp));
	WRITE_ONCE(*fixmap_pgdp, pgd);
	/*
	 * We need dsb(ishst) here to ensure the page-table-walker sees
	 * our new entry before set_p?d() returns. The fixmap's
	 * flush_tlb_kernel_range() via clear_fixmap() does this for us.
	 */
	pgd_clear_fixmap();
	spin_unlock(&swapper_pgdir_lock);
}

pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_is_map_memory(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);

/* IAMROOT, 2021.10.30:
 * - 1개의 page table을 생성하고 fixmap에 mapping 하여 memset을 통해 내용을
 *   0으로 초기한다. 반환은 paddr 지만 vaddr를 사용하는 함수로 인해 fixmap에
 *   mapping 한 것이고 memset이 완료되면 다시 FIX_PTE를 clear 한다.
 */
static phys_addr_t __init early_pgtable_alloc(int shift)
{
	phys_addr_t phys;
	void *ptr;

	/* IAMROOT, 2024.02.22:
	 * - memory region에서 PAGE_SIZE 만큼 alloc을 요청한다.
	 */
	phys = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
	if (!phys)
		panic("Failed to allocate page table page\n");

	/* IAMROOT, 2024.02.22:
	 * - memblock에서 alloc 하여 return 받은건 phys 이므로 mmu가 사용하는
	 *   page table에는 mapping 되어 있지 않은 상태이다. 따라서 fixmap을
	 *   사용하여 mmu가 처리할 수 있도록 임시로 mapping을 한 뒤에
	 *   memset으로 내용을 clear 한다.
	 */
	/*
	 * The FIX_{PGD,PUD,PMD} slots may be in active use, but the FIX_PTE
	 * slot will be free, so we can (ab)use the FIX_PTE slot to initialise
	 * any level of table.
	 */
	ptr = pte_set_fixmap(phys);

	memset(ptr, 0, PAGE_SIZE);

	/* IAMROOT, 2024.02.22:
	 * - memset으로 내용을 clear 하였으니 임시로 mapping 한 fixmap 영역을
	 *   clear 한다.
	 */
	/*
	 * Implicit barriers also ensure the zeroed page is visible to the page
	 * table walker
	 */
	pte_clear_fixmap();

	return phys;
}

/*
 * IAMROOT, 2021.12.18:
 * - 경우에 따라 old, new에 따른 type검사를 수행한다.
 */
static bool pgattr_change_is_safe(u64 old, u64 new)
{
	/*
	 * The following mapping attributes may be updated in live
	 * kernel mappings without the need for break-before-make.
	 */
	pteval_t mask = PTE_PXN | PTE_RDONLY | PTE_WRITE | PTE_NG;

	/* creating or taking down mappings is always safe */
/*
 * IAMROOT, 2021.12.18:
 * - old == 0 : 이전이 없음. 즉 새로 생기는 상황.
 * - new == 0 : 새로운게 없음. 즉 삭제 되는 상황.
 * - 새로 생기거나 삭제되는 경우에는 type을 따질 필요가 없으므로 trun
 */
	if (old == 0 || new == 0)
		return true;

	/* live contiguous mappings may not be manipulated at all */
	if ((old | new) & PTE_CONT)
		return false;

	/* Transitioning from Non-Global to Global is unsafe */
	if (old & ~new & PTE_NG)
		return false;

	/*
	 * Changing the memory type between Normal and Normal-Tagged is safe
	 * since Tagged is considered a permission attribute from the
	 * mismatched attribute aliases perspective.
	 */
	if (((old & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL) ||
	     (old & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL_TAGGED)) &&
	    ((new & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL) ||
	     (new & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL_TAGGED)))
		mask |= PTE_ATTRINDX_MASK;

	return ((old ^ new) & ~mask) == 0;
}

static void init_pte(pmd_t *pmdp, unsigned long addr, unsigned long end,
		     phys_addr_t phys, pgprot_t prot)
{
	pte_t *ptep;

	/* IAMROOT, 2023.12.04:
	 * - @pmdp와 @addr를 이용하여 pmd entry에 pte table을 mapping 한다.
	 *   이때 entry의 attr은 page table을 명시하는 값으로 셋업된다.
	 */
	ptep = pte_set_fixmap_offset(pmdp, addr);
	do {
		pte_t old_pte = READ_ONCE(*ptep);

		/* IAMROOT, 2023.12.18:
		 * - pte entry 에 page를 mapping 한다.
		 */
		set_pte(ptep, pfn_pte(__phys_to_pfn(phys), prot));

		/*
		 * After the PTE entry has been populated once, we
		 * only allow updates to the permission attributes.
		 */
		BUG_ON(!pgattr_change_is_safe(pte_val(old_pte),
					      READ_ONCE(pte_val(*ptep))));

		phys += PAGE_SIZE;
	} while (ptep++, addr += PAGE_SIZE, addr != end);

	pte_clear_fixmap();
}

/* IAMROOT, 2021.10.09:
 * - PTE table을 매핑을 한다.
 *   pte entry를 16개 연속 mapping 가능한 경우 64KB block mapping을 시도하고
 *   그렇지 않은 경우 single pte entry mapping을 한다.
 *
 *   block mapping: PAGE_SIZE == 4KB, 4 LVL 기준 4KB * 16 == 64KB
 */
static void alloc_init_cont_pte(pmd_t *pmdp, unsigned long addr,
				unsigned long end, phys_addr_t phys,
				pgprot_t prot,
				phys_addr_t (*pgtable_alloc)(int),
				int flags)
{
	unsigned long next;
	pmd_t pmd = READ_ONCE(*pmdp);

	BUG_ON(pmd_sect(pmd));

	/* IAMROOT, 2023.12.17:
	 * - @pmd가 mapping 되지 않은 경우를 대비한 코드이며
	 *   @pgtable_alloc 함수를 통해 pmd table page를 alloc 한다.
	 */
	if (pmd_none(pmd)) {
		pmdval_t pmdval = PMD_TYPE_TABLE | PMD_TABLE_UXN;
		phys_addr_t pte_phys;

		if (flags & NO_EXEC_MAPPINGS)
			pmdval |= PMD_TABLE_PXN;
		BUG_ON(!pgtable_alloc);
		pte_phys = pgtable_alloc(PAGE_SHIFT);
		__pmd_populate(pmdp, pte_phys, pmdval);
		pmd = READ_ONCE(*pmdp);
	}
	BUG_ON(pmd_bad(pmd));

	do {
		pgprot_t __prot = prot;

		/* IAMROOT, 2021.10.09:
		 * - 아래 식을 통해 계산하여 다음 va(pte table)을 구한다.
		 *   @next = va(@addr) + (PTE_SIZE * CONT_PTES(16))
		 */
		next = pte_cont_addr_end(addr, end);

		/* IAMROOT, 2023.12.07:
		 * - ALIGN(@addr & @next & @phys, CONT_PTE_MASK) 이고 BLOCK_MAPPING이
		 *   허용일 때 64KB 단위의 block mapping이 가능하니 PTE_CONT flag를
		 *   설정한다.
		 *
		 *   @addr과 @next이 CONT_PTE_SIZE로 정렬되어 있다면 @addr과 @next의
		 *   사이에 64KB 크기의 phys memory 갭이 있고 연속 사용 가능한 것이므로
		 *   4KB 단위로 끊는 것이 아닌 64KB 통으로 매핑한다.
		 */
		/* use a contiguous mapping if the range is suitably aligned */
		if ((((addr | next | phys) & ~CONT_PTE_MASK) == 0) &&
		    (flags & NO_CONT_MAPPINGS) == 0)
			__prot = __pgprot(pgprot_val(prot) | PTE_CONT);

		init_pte(pmdp, addr, next, phys, __prot);

		phys += next - addr;
	} while (addr = next, addr != end);
}

static void init_pmd(pud_t *pudp, unsigned long addr, unsigned long end,
		     phys_addr_t phys, pgprot_t prot,
		     phys_addr_t (*pgtable_alloc)(int), int flags)
{
	unsigned long next;
	pmd_t *pmdp;

	/* IAMROOT, 2023.12.04:
	 * - @pudp와 @addr를 이용하여 pud entry에 pmd table을 mapping 한다.
	 *   이때 entry의 attr은 page table을 명시하는 값으로 셋업된다.
	 */
	pmdp = pmd_set_fixmap_offset(pudp, addr);
	do {
		pmd_t old_pmd = READ_ONCE(*pmdp);

		next = pmd_addr_end(addr, end);

		/* IAMROOT, 2023.12.07:
		 * - ALIGN(@addr && @next && @phys, PMD_SIZE) 이고 BLOCK_MAPPING이
		 *   허용일 때 PMD 단위로 block mapping 을 수행한다.
		 */
		/* try section mapping first */
		if (((addr | next | phys) & ~PMD_MASK) == 0 &&
		    (flags & NO_BLOCK_MAPPINGS) == 0) {
			pmd_set_huge(pmdp, phys, prot);

			/*
			 * After the PMD entry has been populated once, we
			 * only allow updates to the permission attributes.
			 */
			BUG_ON(!pgattr_change_is_safe(pmd_val(old_pmd),
						      READ_ONCE(pmd_val(*pmdp))));
		} else {
			/* IAMROOT, 2023.12.10:
			 * - PTE entry를 초기화하기 위해 관련 함수 호출.
			 *   만약 64KB 단위의 block mapping이 가능하면 PTE_CONT flag를
			 *   설정하고 그렇지 않으면 4KB page 단위로 할당하도록 table을
			 *   세팅한다.
			 */
			alloc_init_cont_pte(pmdp, addr, next, phys, prot,
                    pgtable_alloc, flags);

			BUG_ON(pmd_val(old_pmd) != 0 &&
			       pmd_val(old_pmd) != READ_ONCE(pmd_val(*pmdp)));
		}
		phys += next - addr;
	} while (pmdp++, addr = next, addr != end);

	pmd_clear_fixmap();
}

/* IAMROOT, 2021.10.09:
 * - PMD table을 매핑을 한다.
 *   pmd entry를 16개 연속 mapping 가능한 경우 32MB block mapping을 시도하고
 *   그렇지 않은 경우 single pmd entry mapping을 한다.
 *
 *   block mapping: PAGE_SIZE == 4KB, 4 LVL 기준 2MB * 16 == 32M
 */
static void alloc_init_cont_pmd(pud_t *pudp, unsigned long addr,
				unsigned long end, phys_addr_t phys,
				pgprot_t prot,
				phys_addr_t (*pgtable_alloc)(int), int flags)
{
	unsigned long next;
	pud_t pud = READ_ONCE(*pudp);

	/*
	 * Check for initial section mappings in the pgd/pud.
	 */
	BUG_ON(pud_sect(pud));

	/* IAMROOT, 2023.12.17:
	 * - @pud가 mapping 되지 않은 경우를 대비한 코드이며
	 *   @pgtable_alloc 함수를 통해 pud table page를 alloc 한다.
	 */
	if (pud_none(pud)) {
		pudval_t pudval = PUD_TYPE_TABLE | PUD_TABLE_UXN;
		phys_addr_t pmd_phys;

		if (flags & NO_EXEC_MAPPINGS)
			pudval |= PUD_TABLE_PXN;
		BUG_ON(!pgtable_alloc);
		pmd_phys = pgtable_alloc(PMD_SHIFT);
		__pud_populate(pudp, pmd_phys, pudval);
		pud = READ_ONCE(*pudp);
	}
	BUG_ON(pud_bad(pud));

	do {
		pgprot_t __prot = prot;

		/* IAMROOT, 2021.10.09:
		 * - 아래 식을 통해 계산하여 다음 va(pmd table)을 구한다.
		 *   @next = va(@addr) + (PMD_SIZE * CONT_PMDS(16))
		 */
		next = pmd_cont_addr_end(addr, end);

		/* IAMROOT, 2023.12.07:
		 * - ALIGN(@addr && @next && @phys, CONT_PMD_MASK) 이고 BLOCK_MAPPING이
		 *   허용일 때 32MB 단위의 block mapping이 가능하니 PTE_CONT flag를
		 *   설정한다.
		 *
		 *   @addr과 @next이 CONT_PMD_SIZE로 정렬되어 있다면 @addr과 @next의
		 *   사이에 32MB 크기의 phys memory 갭이 있고 연속 사용 가능한 것이므로
		 *   4KB 단위로 끊는 것이 아닌 32MB 통으로 매핑한다.
		 */
		/* use a contiguous mapping if the range is suitably aligned */
		if ((((addr | next | phys) & ~CONT_PMD_MASK) == 0) &&
		    (flags & NO_CONT_MAPPINGS) == 0)
			__prot = __pgprot(pgprot_val(prot) | PTE_CONT);

		init_pmd(pudp, addr, next, phys, __prot, pgtable_alloc, flags);

		phys += next - addr;
	} while (addr = next, addr != end);
}

/* IAMROOT, 2021.10.09:
 * - PAGE_SIZE == 4KB 일때 @addr, @next 및 @phys가 모두 PUD 크기로 정렬된 경우
 *   1G 블럭 매핑을 허용한다.
 *   (PUD_MASK: 0xffff_ffff_c000_0000)
 *
 *   예) addr=0x4000_0000, next=8000_0000, phys=0xc000_0000 -> true
 *       addr=0x4000_0000, next=4100_0000, phys=0xc000_0000 -> false
 *       addr=0x4000_0000, next=8000_0000, phys=0xc200_0000 -> false
 */
static inline bool use_1G_block(unsigned long addr, unsigned long next,
			unsigned long phys)
{
	if (PAGE_SHIFT != 12)
		return false;

	/* IAMROOT, 2021.10.09:
	 * - PUD_MASK가 커버하는 bits 범위 중에 하나라도 1로 세팅되어 있으면
	 *   PUD 크기로 정렬되어 있는게 아니므로 false 반환.
	 */
	if (((addr | next | phys) & ~PUD_MASK) != 0)
		return false;

	return true;
}

static void alloc_init_pud(pgd_t *pgdp, unsigned long addr, unsigned long end,
			   phys_addr_t phys, pgprot_t prot,
			   phys_addr_t (*pgtable_alloc)(int),
			   int flags)
{
	unsigned long next;
	pud_t *pudp;
	p4d_t *p4dp = p4d_offset(pgdp, addr);
	p4d_t p4d = READ_ONCE(*p4dp);

	/* IAMROOT, 2023.12.17:
	 * - @p4d(pgd)가 mapping 되지 않은 경우를 대비한 코드이며
	 *   @pgtable_alloc 함수를 통해 p4d table page를 alloc 한다.
	 */
	if (p4d_none(p4d)) {
		p4dval_t p4dval = P4D_TYPE_TABLE | P4D_TABLE_UXN;
		phys_addr_t pud_phys;

		if (flags & NO_EXEC_MAPPINGS)
			p4dval |= P4D_TABLE_PXN;

		/* IAMROOT, 2023.11.24:
		 * - @pgtable_alloc arg의 값은 다음 중에 하나가 된다.
		 *   1. NULL: -> fixmap_remap_fdt()
		 *      할당할 수 없는 상황에서 사용한다.
		 *      정규 메모리 할당자, memblock도 사용할 수 없으므로 fixmap을 통해
		 *      static allocated region에서 mapping을 수행한다.
		 *
		 *   2. early_pgtable_alloc(): -> map_kernel_segment()
		 *      정규 메모리 할당자는 사용할 수 없지만 memblock은 사용 가능할 때.
		 *
		 *   3. pgd_pgtable_alloc(): TODO
		 *   4. __pgd_pgtable_alloc(): TODO
		 */
		BUG_ON(!pgtable_alloc);
		pud_phys = pgtable_alloc(PUD_SHIFT);
		__p4d_populate(p4dp, pud_phys, p4dval);
		p4d = READ_ONCE(*p4dp);
	}
	BUG_ON(p4d_bad(p4d));

	/* IAMROOT, 2023.12.04:
	 * - @p4dp와 @addr를 이용하여 pgd entry에 pud table을 mapping 한다.
	 *   이때 entry의 attr은 page table을 명시하는 값으로 셋업된다.
	 */
	pudp = pud_set_fixmap_offset(p4dp, addr);
	do {
		pud_t old_pud = READ_ONCE(*pudp);

		next = pud_addr_end(addr, end);

		/*
		 * For 4K granule only, attempt to put down a 1GB block
		 */
		if (use_1G_block(addr, next, phys) &&
		    (flags & NO_BLOCK_MAPPINGS) == 0)
		{
			/* IAMROOT, 2023.12.07:
			 * - PAGE_SIZE == 4KB && ALIGN(@addr && @next && @phys, PUD_SIZE)
			 *   이고 BLOCK_MAPPING이 허용일 때 1G block mapping을 시도한다.
			 *
			 *   @addr과 @next이 PUD_SIZE로 정렬되어 있다면 @addr과 @next의
			 *   사이에 1G 크기의 phys memory 갭이 있고 연속 매핑
			 *   가능한 것이므로 4KB 단위로 끊는 것이 아닌 1G 통으로 매핑한다.
			 */
			pud_set_huge(pudp, phys, prot);

			/*
			 * After the PUD entry has been populated once, we
			 * only allow updates to the permission attributes.
			 */
			BUG_ON(!pgattr_change_is_safe(pud_val(old_pud),
						      READ_ONCE(pud_val(*pudp))));
		} else {
			/* IAMROOT, 2023.12.10:
			 * - PMD entry를 초기화하기 위해 관련 함수 호출.
			 *   만약 32MB 단위의 block mapping이 가능하면 PTE_CONT flag를
			 *   설정하고 그렇지 않으면 4KB page 단위로 할당하도록 table을
			 *   세팅한다.
			 */
			alloc_init_cont_pmd(pudp, addr, next, phys, prot,
					    pgtable_alloc, flags);

			BUG_ON(pud_val(old_pud) != 0 &&
			       pud_val(old_pud) != READ_ONCE(pud_val(*pudp)));
		}
		phys += next - addr;
	} while (pudp++, addr = next, addr != end);

	pud_clear_fixmap();
}

static void __create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
				 unsigned long virt, phys_addr_t size,
				 pgprot_t prot,
				 phys_addr_t (*pgtable_alloc)(int),
				 int flags)
{
	unsigned long addr, end, next;
	pgd_t *pgdp = pgd_offset_pgd(pgdir, virt);

	/*
	 * If the virtual and physical address don't have the same offset
	 * within a page, we cannot map the region as the caller expects.
	 */
	if (WARN_ON((phys ^ virt) & ~PAGE_MASK))
		return;

	/* IAMROOT, 2021.10.09:
	 * - @virt와 @size를 이용하여 mapping start/end addr를 계산한다.
	 *
	 *   @phys: page 단위로 round down 하여 정렬한다.
	 *   @addr: @virt addr를 page 단위로 round down 하여 정렬한다.
	 *          (시작 주소)
	 *   @end : (@virt + @size) addr를 page 단위로 round up 하여 정렬한다.
	 *          (끝 주소)
	 */
	phys &= PAGE_MASK;
	addr = virt & PAGE_MASK;
	end = PAGE_ALIGN(virt + size);

	/* IAMROOT, 2021.10.14:
	 * - [addr .. next/end] region을 pgd 단위마다 pud mapping을 수행한다.
	 *   (alloc_init_pud 함수 호출)
	 *
	 *   예) addr: 1GB, end: 1025GB, PGDIR_SIZE: 512GB인 경우
	 *       1st loop: 1G .. 513G
	 *       2nd loop: 513G .. 1025G
	 *       3th loop: None
	 *
	 *       총 2번 수행된다.
	 */
	do {
		next = pgd_addr_end(addr, end);
		alloc_init_pud(pgdp, addr, next, phys, prot, pgtable_alloc,
			       flags);
		phys += next - addr;
	} while (pgdp++, addr = next, addr != end);
}

static phys_addr_t __pgd_pgtable_alloc(int shift)
{
	void *ptr = (void *)__get_free_page(GFP_PGTABLE_KERNEL);
	BUG_ON(!ptr);

	/* Ensure the zeroed page is visible to the page table walker */
	dsb(ishst);
	return __pa(ptr);
}

static phys_addr_t pgd_pgtable_alloc(int shift)
{
	phys_addr_t pa = __pgd_pgtable_alloc(shift);

	/*
	 * Call proper page table ctor in case later we need to
	 * call core mm functions like apply_to_page_range() on
	 * this pre-allocated page table.
	 *
	 * We don't select ARCH_ENABLE_SPLIT_PMD_PTLOCK if pmd is
	 * folded, and if so pgtable_pmd_page_ctor() becomes nop.
	 */
	if (shift == PAGE_SHIFT)
		BUG_ON(!pgtable_pte_page_ctor(phys_to_page(pa)));
	else if (shift == PMD_SHIFT)
		BUG_ON(!pgtable_pmd_page_ctor(phys_to_page(pa)));

	return pa;
}

/*
 * This function can only be used to modify existing table entries,
 * without allocating new levels of table. Note that this permits the
 * creation of new section or page entries.
 */
/* IAMROOT, 2021.10.09:
 * - fixmap_remap_fdt() 함수에서 fdt를 매핑할 때 사용한다.
 *   early_fixmap_init() 함수를 통해 초기화한 bm_pud, bm_pmd, bm_pte 테이블을
 *   사용하므로 추가적으로 page table을 위한 메모리를 alloc 하지 않는다.
 *   NO_CONT_MAPPINGS 옵션을 사용하고, 함수 포인터 arg는 null로 넘긴다.
 */
static void __init create_mapping_noalloc(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, pgprot_t prot)
{
	if ((virt >= PAGE_END) && (virt < VMALLOC_START)) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}
	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot, NULL,
			     NO_CONT_MAPPINGS);
}

void __init create_pgd_mapping(struct mm_struct *mm, phys_addr_t phys,
			       unsigned long virt, phys_addr_t size,
			       pgprot_t prot, bool page_mappings_only)
{
	int flags = 0;

	BUG_ON(mm == &init_mm);

	if (page_mappings_only)
		flags = NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

	__create_pgd_mapping(mm->pgd, phys, virt, size, prot,
			     pgd_pgtable_alloc, flags);
}

static void update_mapping_prot(phys_addr_t phys, unsigned long virt,
				phys_addr_t size, pgprot_t prot)
{
	if ((virt >= PAGE_END) && (virt < VMALLOC_START)) {
		pr_warn("BUG: not updating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}

	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot, NULL,
			     NO_CONT_MAPPINGS);

	/* flush the TLBs after updating live kernel mappings */
	flush_tlb_kernel_range(virt, virt + size);
}

static void __init __map_memblock(pgd_t *pgdp, phys_addr_t start,
				  phys_addr_t end, pgprot_t prot, int flags)
{
	__create_pgd_mapping(pgdp, start, __phys_to_virt(start), end - start,
			     prot, early_pgtable_alloc, flags);
}

void __init mark_linear_text_alias_ro(void)
{
	/*
	 * Remove the write permissions from the linear alias of .text/.rodata
	 */
	update_mapping_prot(__pa_symbol(_stext), (unsigned long)lm_alias(_stext),
			    (unsigned long)__init_begin - (unsigned long)_stext,
			    PAGE_KERNEL_RO);
}

static bool crash_mem_map __initdata;

static int __init enable_crash_mem_map(char *arg)
{
	/*
	 * Proper parameter parsing is done by reserve_crashkernel(). We only
	 * need to know if the linear map has to avoid block mappings so that
	 * the crashkernel reservations can be unmapped later.
	 */
	crash_mem_map = true;

	return 0;
}
early_param("crashkernel", enable_crash_mem_map);

/* IAMROOT, 2021.10.30:
 * - @pgdp에 kernel/etc memory에 대한 mapping을 수행한다.
 */
static void __init map_mem(pgd_t *pgdp)
{
	static const u64 direct_map_end = _PAGE_END(VA_BITS_MIN);

	/* IAMROOT, 2021.10.30:
	 * - @kernel_start .. @kernel_end 까지는 아래 영역을 포함한다.
	 *   KIMG(text) + got + RO_DATA + pgtable(idmap + reserved + swapper)
	 */
	phys_addr_t kernel_start = __pa_symbol(_stext);
	phys_addr_t kernel_end = __pa_symbol(__init_begin);
	phys_addr_t start, end;
	int flags = NO_EXEC_MAPPINGS;
	u64 i;

	/*
	 * Setting hierarchical PXNTable attributes on table entries covering
	 * the linear region is only possible if it is guaranteed that no table
	 * entries at any level are being shared between the linear region and
	 * the vmalloc region. Check whether this is true for the PGD level, in
	 * which case it is guaranteed to be true for all other levels as well.
	 */
	BUILD_BUG_ON(pgd_index(direct_map_end - 1) == pgd_index(direct_map_end));

	if (can_set_direct_map() || crash_mem_map || IS_ENABLED(CONFIG_KFENCE))
		flags |= NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

	/* IAMROOT, 2021.10.30:
	 * - 아래 for_each loop에서 mapping 되는 것을 방지하기 위해
	 *   memory region[@kernel_start .. size] 영역에 임시로 NOMAP flag를
	 *   세팅한다.
	 */
	/*
	 * Take care not to create a writable alias for the
	 * read-only text and rodata sections of the kernel image.
	 * So temporarily mark them as NOMAP to skip mappings in
	 * the following for-loop
	 */
	memblock_mark_nomap(kernel_start, kernel_end - kernel_start);

	/* IAMROOT, 2024.03.01:
	 * - memblock memory region을 순회하면서 MEMBLOCK_NONE인 flag의 region에
	 *   대해서만 __map_memblock(..)을 호출한다.
	 *
	 *   Kernel 영역은 위에서 NOMAP으로 flag 설정을 했으므로 제외된다.
	 */
	/* map all the memory banks */
	for_each_mem_range(i, &start, &end) {
		if (start >= end)
			break;
		/*
		 * The linear map must allow allocation tags reading/writing
		 * if MTE is present. Otherwise, it has the same attributes as
		 * PAGE_KERNEL.
		 */
		__map_memblock(pgdp, start, end, pgprot_tagged(PAGE_KERNEL),
			       flags);
	}

	/* IAMROOT, 2021.10.30:
	 * - memory region[@kernel_start .. size] 제외하고 위에서 mapping이
	 *   끝났으므로 @kernel_start  영역의 property를 PAGE_KERNEL로 설정하고
	 *   NO_CONT_MAPPINGS flag를 설정한다.
	 *
	 *   NO_CONT_MAPPINGS flag를 설정하는 이유는 후에 영역별로 r/rw 등을
	 *   나누어야 하기 때문이다.
	 */
	/*
	 * Map the linear alias of the [_stext, __init_begin) interval
	 * as non-executable now, and remove the write permission in
	 * mark_linear_text_alias_ro() below (which will be called after
	 * alternative patching has completed). This makes the contents
	 * of the region accessible to subsystems such as hibernate,
	 * but protects it from inadvertent modification or execution.
	 * Note that contiguous mappings cannot be remapped in this way,
	 * so we should avoid them here.
	 */
	__map_memblock(pgdp, kernel_start, kernel_end,
		       PAGE_KERNEL, NO_CONT_MAPPINGS);

	/* IAMROOT, 2021.10.30:
	 * - 위에서 설정한 NOMAP flag를 여기서 다시 해제한다.
	 */
	memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
}

void mark_rodata_ro(void)
{
	unsigned long section_size;

	/*
	 * mark .rodata as read only. Use __init_begin rather than __end_rodata
	 * to cover NOTES and EXCEPTION_TABLE.
	 */
	section_size = (unsigned long)__init_begin - (unsigned long)__start_rodata;
	update_mapping_prot(__pa_symbol(__start_rodata), (unsigned long)__start_rodata,
			    section_size, PAGE_KERNEL_RO);

	debug_checkwx();
}

static void __init map_kernel_segment(pgd_t *pgdp, void *va_start, void *va_end,
				      pgprot_t prot, struct vm_struct *vma,
				      int flags, unsigned long vm_flags)
{
	phys_addr_t pa_start = __pa_symbol(va_start);
	unsigned long size = va_end - va_start;

	BUG_ON(!PAGE_ALIGNED(pa_start));
	BUG_ON(!PAGE_ALIGNED(size));

	/* IAMROOT, 2021.11.17:
	 * - @va_start .. @va_end 까지의 kernel 영역을 @pgdp에 mapping 하되
	 *   alloc이 필요하면 early_pgtable_alloc(..) handler를 사용한다.
	 */
	__create_pgd_mapping(pgdp, pa_start, (unsigned long)va_start, size, prot,
			     early_pgtable_alloc, flags);

	/* IAMROOT, 2021.10.30:
	 * - (@vm_flags & VM_NO_GUARD) == 0 이라면 GUARD 요청이므로
	 *   guard page를 추가하기 위해 (@size += PAGE_SIZE) 한다.
	 */
	if (!(vm_flags & VM_NO_GUARD))
		size += PAGE_SIZE;

	/* IAMROOT, 2024.02.25:
	 * - 위에서 생성한 page table 데이터를 @vma에 저장한다.
	 */
	vma->addr	= va_start;
	vma->phys_addr	= pa_start;
	vma->size	= size;
	vma->flags	= VM_MAP | vm_flags;
	vma->caller	= __builtin_return_address(0);

	/* IAMROOT, 2024.02.25:
	 * - 현재 vmalloc 초기화가 이루어지지 않아 vmlist를 사용하여 임시로
	 *   관리한다.
	 */
	vm_area_add_early(vma);
}

/*
 * IAMROOT, 2021.10.30: 
 * 1) "rodata=off"
 *     rodata_enabled=false
 *     rodata_full=false
 *     -> 커널 및 rodata 영역을 모두 RW로 매핑하여 사용한다.
 * 2) "rodata=on"
 *     rodata_enabled=true
 *     rodata_full=false
 *     -> 커널만 Read only로 매핑하여 사용한다.
 * 3) "rodata=full"
 *     rodata_enabled=true
 *     rodata_full=true
 *     -> 커널 및 rodata 영역을 모두 Read only로 매핑하여 사용한다.
 *
 * - rodata_enabled=true(default)인 경우 커널 코드를 read only로 매핑한다.
 * - rodata_full=true(default)인 경우 커널 및 rodata 영역을 read only로 매핑한다.
 *
 * 주의: early param이 아닌 정규 param은 set_debug_rodata() 함수에도 존재한다.
 */
static int __init parse_rodata(char *arg)
{
	int ret = strtobool(arg, &rodata_enabled);
	if (!ret) {
		rodata_full = false;
		return 0;
	}

	/* permit 'full' in addition to boolean options */
	if (strcmp(arg, "full"))
		return -EINVAL;

	rodata_enabled = true;
	rodata_full = true;
	return 0;
}
early_param("rodata", parse_rodata);

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * IAMROOT, 2022.11.10:
 * - fixmap에 tramp 관련 데이터들을 mapping한다.
 */
static int __init map_entry_trampoline(void)
{
	pgprot_t prot = rodata_enabled ? PAGE_KERNEL_ROX : PAGE_KERNEL_EXEC;
	phys_addr_t pa_start = __pa_symbol(__entry_tramp_text_start);

	/* The trampoline is always mapped and can therefore be global */
	pgprot_val(prot) &= ~PTE_NG;

	/* Map only the text into the trampoline page table */
	memset(tramp_pg_dir, 0, PGD_SIZE);
	__create_pgd_mapping(tramp_pg_dir, pa_start, TRAMP_VALIAS, PAGE_SIZE,
			     prot, __pgd_pgtable_alloc, 0);

	/* Map both the text and data into the kernel page table */
	__set_fixmap(FIX_ENTRY_TRAMP_TEXT, pa_start, prot);
/*
 * IAMROOT, 2022.11.10:
 * - FIX_ENTRY_TRAMP_DATA는 FIX_ENTRY_TRAMP_TEXT + PAGE_SIZE 에 위치할것이다.
 *   해당 fixmap으로 __entry_tramp_data_start를 mapping한다.
 *   이걸 함으로써 가상주소적으로
 *
 *   -- high --
 *   .quad vectors
 *   __entry_tramp_data_start
 *   (PAGE_SIZE)
 *   tramp_vectors
 *   -- low --
 *
 *   의 address 체계가 성립함으로서 tramp_vectors로 vectors address가 저장된
 *   .quad로 접근이 가능하며, 이 값을 ldr X, [.quad vecors]함으로써 
 *   vectors의 주소를 가져올수있다.
 */
	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		extern char __entry_tramp_data_start[];

		__set_fixmap(FIX_ENTRY_TRAMP_DATA,
			     __pa_symbol(__entry_tramp_data_start),
			     PAGE_KERNEL_RO);
	}

	return 0;
}
core_initcall(map_entry_trampoline);
#endif

/*
 * Open coded check for BTI, only for use to determine configuration
 * for early mappings for before the cpufeature code has run.
 */
static bool arm64_early_this_cpu_has_bti(void)
{
	u64 pfr1;

	if (!IS_ENABLED(CONFIG_ARM64_BTI_KERNEL))
		return false;

	pfr1 = __read_sysreg_by_encoding(SYS_ID_AA64PFR1_EL1);
	return cpuid_feature_extract_unsigned_field(pfr1,
						    ID_AA64PFR1_BT_SHIFT);
}

/*
 * Create fine-grained mappings for the kernel.
 */
/* IAMROOT, 2021.11.16:
 * - @pgdp에 kernel 영역을 매핑한다.
 */
static void __init map_kernel(pgd_t *pgdp)
{
	static struct vm_struct vmlinux_text, vmlinux_rodata, vmlinux_inittext,
				vmlinux_initdata, vmlinux_data;

	/* IAMROOT, 2021.10.30:
	 * - PAGE_KERNEL_ROX : Kernel Read Only + Exec     (default)
	 *   PAGE_KERNEL_EXEC: Kernel Read + Write + Exec
	 */
	/*
	 * External debuggers may need to write directly to the text
	 * mapping to install SW breakpoints. Allow this (only) when
	 * explicitly requested with rodata=off.
	 */
	pgprot_t text_prot = rodata_enabled ? PAGE_KERNEL_ROX : PAGE_KERNEL_EXEC;

	/* IAMROOT, 2021.10.30:
	 * - cpu가 BTI(Branch Target Identification) 기능을 지원하는 경우
	 *   커널 영역 페이지의 매핑 속성에 PTE_GP를 추가한다. 이 기능은
	 *   JOP(Jump Oriented Program) & ROP(Return Oriented Program) Attack을
	 *   회피하기 위해 ARMv8.5 & GCC 9.1에서 PAC(Pointer Authentication Code)를
	 *   만들어 방어한다. 만일 위조된 주소를 사용하면 Branch Target Exception이
	 *   발생한다.
	 *
	 *   BTI: 만약 br/blr insts가 가리키는 주소의 insts가 bti가 아니라면
	 *   Branch Target Exception을 발생시킨다.[1]
	 *
	 *        성공          |       예외
	 *   -------------------+-------------------
	 *      app code        |     app code
	 *                      |
	 *     br x9 ------.    |    br x9 ------.
	 *                 |    |                |
	 *                 |    |                |
	 *    -------------+--  |   -------------+--
	 *       library   |    |      library   |
	 *                 |    |                |
	 *                 |    |                |
	 *     bti <-------'    |    add <-------'
	 *
	 *   [1]: https://developer.arm.com/documentation/102433/0100/Jump-oriented-programming
	 */
	/*
	 * If we have a CPU that supports BTI and a kernel built for
	 * BTI then mark the kernel executable text as guarded pages
	 * now so we don't have to rewrite the page tables later.
	 */
	if (arm64_early_this_cpu_has_bti())
		text_prot = __pgprot_modify(text_prot, PTE_GP, PTE_GP);

	/* IAMROOT, 2021.10.30:
	 * - 다음 kernel 커널 영역에 대해 prot를 달리하여 mapping 한다.
	 *   1) '.text' section
	 *   2) '.rodata' section <- 일단 RW (NO_CONT_MAPPINGS)
	 *   3) '.init.text' section
	 *   4) '.init.data' section
	 *   5) '.data' section
	 */
	/*
	 * Only rodata will be remapped with different permissions later on,
	 * all other segments are allowed to use contiguous mappings.
	 */
	map_kernel_segment(pgdp, _stext, _etext, text_prot, &vmlinux_text, 0,
			   VM_NO_GUARD);
	map_kernel_segment(pgdp, __start_rodata, __inittext_begin, PAGE_KERNEL,
			   &vmlinux_rodata, NO_CONT_MAPPINGS, VM_NO_GUARD);
	map_kernel_segment(pgdp, __inittext_begin, __inittext_end, text_prot,
			   &vmlinux_inittext, 0, VM_NO_GUARD);
	map_kernel_segment(pgdp, __initdata_begin, __initdata_end, PAGE_KERNEL,
			   &vmlinux_initdata, 0, VM_NO_GUARD);
	map_kernel_segment(pgdp, _data, _end, PAGE_KERNEL, &vmlinux_data, 0, 0);

	/* IAMROOT, 2021.10.30:
	 * - init_pg_dir에는 매핑되어 있지만 swapper_pg_dir에 fixmap(FIXADDR_START)
	 *   주소 공간이 매핑되어 있지 않으면 해당 공간을 매핑한다.
	 *
	 *   단 16K, 4레벨 페이지 테이블을 사용하는 경우는 else if 조건을 사용한다.
	 *   예) 0x11111111_11111111 pgd(1) pud(11) pmd(11) pte(11) offset(14)
	 */
	if (!READ_ONCE(pgd_val(*pgd_offset_pgd(pgdp, FIXADDR_START)))) {
		/*
		 * The fixmap falls in a separate pgd to the kernel, and doesn't
		 * live in the carveout for the swapper_pg_dir. We can simply
		 * re-use the existing dir for the fixmap.
		 */
		set_pgd(pgd_offset_pgd(pgdp, FIXADDR_START),
			READ_ONCE(*pgd_offset_k(FIXADDR_START)));
	} else if (CONFIG_PGTABLE_LEVELS > 3) {
		pgd_t *bm_pgdp;
		p4d_t *bm_p4dp;
		pud_t *bm_pudp;
		/*
		 * The fixmap shares its top level pgd entry with the kernel
		 * mapping. This can really only occur when we are running
		 * with 16k/4 levels, so we can simply reuse the pud level
		 * entry instead.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
		bm_pgdp = pgd_offset_pgd(pgdp, FIXADDR_START);
		bm_p4dp = p4d_offset(bm_pgdp, FIXADDR_START);
		bm_pudp = pud_set_fixmap_offset(bm_p4dp, FIXADDR_START);
		pud_populate(&init_mm, bm_pudp, lm_alias(bm_pmd));
		pud_clear_fixmap();
	} else {
		BUG();
	}

	/* IAMROOT, 2021.10.30:
	 * - KASAN이 사용하는 용도의 page table을 생성한다.
	 */
	kasan_copy_shadow(pgdp);
}

/* IAMROOT, 2021.10.30:
 * - memory 전체에 mapping을 swapper_pg_dir로 이동 후에 init_pg_dir 삭제.
 */
void __init paging_init(void)
{
	/* IAMROOT, 2021.10.30:
	 * - mapping을 위해 잠시 FIX_PGD를 사용한다.
	 *
	 *   보안상의 이유로 FIXMAP을 사용하며 va(swapper_pg_dir)을 사용해도 의도한
	 *   정규 매핑 작업에는 지장이 없다.
	 */
	pgd_t *pgdp = pgd_set_fixmap(__pa_symbol(swapper_pg_dir));

	map_kernel(pgdp);
	map_mem(pgdp);

	/* IAMROOT, 2021.10.30:
	 * - mapping이 끝났으므로 FIX_PGD를 해제한다.
	 */
	pgd_clear_fixmap();

	cpu_replace_ttbr1(lm_alias(swapper_pg_dir));

	/* IAMROOT, 2021.10.30:
	 * - 지금부터는 정규 page table인 swapper_pg_dir을 사용한다.
	 */
	init_mm.pgd = swapper_pg_dir;

	/* IAMROOT, 2021.10.30:
	 * - init_pg_dir은 더이상 필요하지 않으니 삭제한다.
	 */
	memblock_free(__pa_symbol(init_pg_dir),
		      __pa_symbol(init_pg_end) - __pa_symbol(init_pg_dir));

	memblock_allow_resize();
}

/*
 * Check whether a kernel address is valid (derived from arch/x86/).
 */
int kern_addr_valid(unsigned long addr)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;

	addr = arch_kasan_reset_tag(addr);
	if ((((long)addr) >> VA_BITS) != -1UL)
		return 0;

	pgdp = pgd_offset_k(addr);
	if (pgd_none(READ_ONCE(*pgdp)))
		return 0;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(READ_ONCE(*p4dp)))
		return 0;

	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return 0;

	if (pud_sect(pud))
		return pfn_valid(pud_pfn(pud));

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return 0;

	if (pmd_sect(pmd))
		return pfn_valid(pmd_pfn(pmd));

	ptep = pte_offset_kernel(pmdp, addr);
	pte = READ_ONCE(*ptep);
	if (pte_none(pte))
		return 0;

	return pfn_valid(pte_pfn(pte));
}

#ifdef CONFIG_MEMORY_HOTPLUG
static void free_hotplug_page_range(struct page *page, size_t size,
				    struct vmem_altmap *altmap)
{
	if (altmap) {
		vmem_altmap_free(altmap, size >> PAGE_SHIFT);
	} else {
		WARN_ON(PageReserved(page));
		free_pages((unsigned long)page_address(page), get_order(size));
	}
}

static void free_hotplug_pgtable_page(struct page *page)
{
	free_hotplug_page_range(page, PAGE_SIZE, NULL);
}

static bool pgtable_range_aligned(unsigned long start, unsigned long end,
				  unsigned long floor, unsigned long ceiling,
				  unsigned long mask)
{
	start &= mask;
	if (start < floor)
		return false;

	if (ceiling) {
		ceiling &= mask;
		if (!ceiling)
			return false;
	}

	if (end - 1 > ceiling - 1)
		return false;
	return true;
}

static void unmap_hotplug_pte_range(pmd_t *pmdp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	pte_t *ptep, pte;

	do {
		ptep = pte_offset_kernel(pmdp, addr);
		pte = READ_ONCE(*ptep);
		if (pte_none(pte))
			continue;

		WARN_ON(!pte_present(pte));
		pte_clear(&init_mm, addr, ptep);
		flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
		if (free_mapped)
			free_hotplug_page_range(pte_page(pte),
						PAGE_SIZE, altmap);
	} while (addr += PAGE_SIZE, addr < end);
}

static void unmap_hotplug_pmd_range(pud_t *pudp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	unsigned long next;
	pmd_t *pmdp, pmd;

	do {
		next = pmd_addr_end(addr, end);
		pmdp = pmd_offset(pudp, addr);
		pmd = READ_ONCE(*pmdp);
		if (pmd_none(pmd))
			continue;

		WARN_ON(!pmd_present(pmd));
		if (pmd_sect(pmd)) {
			pmd_clear(pmdp);

			/*
			 * One TLBI should be sufficient here as the PMD_SIZE
			 * range is mapped with a single block entry.
			 */
			flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
			if (free_mapped)
				free_hotplug_page_range(pmd_page(pmd),
							PMD_SIZE, altmap);
			continue;
		}
		WARN_ON(!pmd_table(pmd));
		unmap_hotplug_pte_range(pmdp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);
}

static void unmap_hotplug_pud_range(p4d_t *p4dp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	unsigned long next;
	pud_t *pudp, pud;

	do {
		next = pud_addr_end(addr, end);
		pudp = pud_offset(p4dp, addr);
		pud = READ_ONCE(*pudp);
		if (pud_none(pud))
			continue;

		WARN_ON(!pud_present(pud));
		if (pud_sect(pud)) {
			pud_clear(pudp);

			/*
			 * One TLBI should be sufficient here as the PUD_SIZE
			 * range is mapped with a single block entry.
			 */
			flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
			if (free_mapped)
				free_hotplug_page_range(pud_page(pud),
							PUD_SIZE, altmap);
			continue;
		}
		WARN_ON(!pud_table(pud));
		unmap_hotplug_pmd_range(pudp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);
}

static void unmap_hotplug_p4d_range(pgd_t *pgdp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	unsigned long next;
	p4d_t *p4dp, p4d;

	do {
		next = p4d_addr_end(addr, end);
		p4dp = p4d_offset(pgdp, addr);
		p4d = READ_ONCE(*p4dp);
		if (p4d_none(p4d))
			continue;

		WARN_ON(!p4d_present(p4d));
		unmap_hotplug_pud_range(p4dp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);
}

static void unmap_hotplug_range(unsigned long addr, unsigned long end,
				bool free_mapped, struct vmem_altmap *altmap)
{
	unsigned long next;
	pgd_t *pgdp, pgd;

	/*
	 * altmap can only be used as vmemmap mapping backing memory.
	 * In case the backing memory itself is not being freed, then
	 * altmap is irrelevant. Warn about this inconsistency when
	 * encountered.
	 */
	WARN_ON(!free_mapped && altmap);

	do {
		next = pgd_addr_end(addr, end);
		pgdp = pgd_offset_k(addr);
		pgd = READ_ONCE(*pgdp);
		if (pgd_none(pgd))
			continue;

		WARN_ON(!pgd_present(pgd));
		unmap_hotplug_p4d_range(pgdp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);
}

static void free_empty_pte_table(pmd_t *pmdp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	pte_t *ptep, pte;
	unsigned long i, start = addr;

	do {
		ptep = pte_offset_kernel(pmdp, addr);
		pte = READ_ONCE(*ptep);

		/*
		 * This is just a sanity check here which verifies that
		 * pte clearing has been done by earlier unmap loops.
		 */
		WARN_ON(!pte_none(pte));
	} while (addr += PAGE_SIZE, addr < end);

	if (!pgtable_range_aligned(start, end, floor, ceiling, PMD_MASK))
		return;

	/*
	 * Check whether we can free the pte page if the rest of the
	 * entries are empty. Overlap with other regions have been
	 * handled by the floor/ceiling check.
	 */
	ptep = pte_offset_kernel(pmdp, 0UL);
	for (i = 0; i < PTRS_PER_PTE; i++) {
		if (!pte_none(READ_ONCE(ptep[i])))
			return;
	}

	pmd_clear(pmdp);
	__flush_tlb_kernel_pgtable(start);
	free_hotplug_pgtable_page(virt_to_page(ptep));
}

static void free_empty_pmd_table(pud_t *pudp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	pmd_t *pmdp, pmd;
	unsigned long i, next, start = addr;

	do {
		next = pmd_addr_end(addr, end);
		pmdp = pmd_offset(pudp, addr);
		pmd = READ_ONCE(*pmdp);
		if (pmd_none(pmd))
			continue;

		WARN_ON(!pmd_present(pmd) || !pmd_table(pmd) || pmd_sect(pmd));
		free_empty_pte_table(pmdp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);

	if (CONFIG_PGTABLE_LEVELS <= 2)
		return;

	if (!pgtable_range_aligned(start, end, floor, ceiling, PUD_MASK))
		return;

	/*
	 * Check whether we can free the pmd page if the rest of the
	 * entries are empty. Overlap with other regions have been
	 * handled by the floor/ceiling check.
	 */
	pmdp = pmd_offset(pudp, 0UL);
	for (i = 0; i < PTRS_PER_PMD; i++) {
		if (!pmd_none(READ_ONCE(pmdp[i])))
			return;
	}

	pud_clear(pudp);
	__flush_tlb_kernel_pgtable(start);
	free_hotplug_pgtable_page(virt_to_page(pmdp));
}

static void free_empty_pud_table(p4d_t *p4dp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	pud_t *pudp, pud;
	unsigned long i, next, start = addr;

	do {
		next = pud_addr_end(addr, end);
		pudp = pud_offset(p4dp, addr);
		pud = READ_ONCE(*pudp);
		if (pud_none(pud))
			continue;

		WARN_ON(!pud_present(pud) || !pud_table(pud) || pud_sect(pud));
		free_empty_pmd_table(pudp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);

	if (CONFIG_PGTABLE_LEVELS <= 3)
		return;

	if (!pgtable_range_aligned(start, end, floor, ceiling, PGDIR_MASK))
		return;

	/*
	 * Check whether we can free the pud page if the rest of the
	 * entries are empty. Overlap with other regions have been
	 * handled by the floor/ceiling check.
	 */
	pudp = pud_offset(p4dp, 0UL);
	for (i = 0; i < PTRS_PER_PUD; i++) {
		if (!pud_none(READ_ONCE(pudp[i])))
			return;
	}

	p4d_clear(p4dp);
	__flush_tlb_kernel_pgtable(start);
	free_hotplug_pgtable_page(virt_to_page(pudp));
}

static void free_empty_p4d_table(pgd_t *pgdp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	unsigned long next;
	p4d_t *p4dp, p4d;

	do {
		next = p4d_addr_end(addr, end);
		p4dp = p4d_offset(pgdp, addr);
		p4d = READ_ONCE(*p4dp);
		if (p4d_none(p4d))
			continue;

		WARN_ON(!p4d_present(p4d));
		free_empty_pud_table(p4dp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);
}

static void free_empty_tables(unsigned long addr, unsigned long end,
			      unsigned long floor, unsigned long ceiling)
{
	unsigned long next;
	pgd_t *pgdp, pgd;

	do {
		next = pgd_addr_end(addr, end);
		pgdp = pgd_offset_k(addr);
		pgd = READ_ONCE(*pgdp);
		if (pgd_none(pgd))
			continue;

		WARN_ON(!pgd_present(pgd));
		free_empty_p4d_table(pgdp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);
}
#endif

/*
 * IAMROOT, 2021.12.18:
 * - ARM64_KERNEL_USES_PMD_MAPS을 지원안하는 경우 4k단위로, 아니면 2MB단위로
 *   memmap을 mapping한다.
 * - vmemmap의 영역에 memmap을 mapping한다.
 */
#if !ARM64_KERNEL_USES_PMD_MAPS
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node,
		struct vmem_altmap *altmap)
{
	WARN_ON((start < VMEMMAP_START) || (end > VMEMMAP_END));
	return vmemmap_populate_basepages(start, end, node, altmap);
}
#else	/* !ARM64_KERNEL_USES_PMD_MAPS */
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node,
		struct vmem_altmap *altmap)
{
	unsigned long addr = start;
	unsigned long next;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;

	WARN_ON((start < VMEMMAP_START) || (end > VMEMMAP_END));
	do {
/*
 * IAMROOT, 2021.12.18:
 * - PMD_SIZE(2MB) 씩 자른다.
 */
		next = pmd_addr_end(addr, end);

		pgdp = vmemmap_pgd_populate(addr, node);
		if (!pgdp)
			return -ENOMEM;

		p4dp = vmemmap_p4d_populate(pgdp, addr, node);
		if (!p4dp)
			return -ENOMEM;

		pudp = vmemmap_pud_populate(p4dp, addr, node);
		if (!pudp)
			return -ENOMEM;

		pmdp = pmd_offset(pudp, addr);
/*
 * IAMROOT, 2021.12.18:
 * - 한번에 2MB씩 초기화를 할예정이므로 pmd populate를 안한다.
 */
		if (pmd_none(READ_ONCE(*pmdp))) {
			void *p = NULL;

			p = vmemmap_alloc_block_buf(PMD_SIZE, node, altmap);
			if (!p) {
/*
 * IAMROOT, 2021.12.18:
 * - 2MB mpping이 실패했으면 4k(PAGE_SIZE)단위로 다시 mapping을 시도한다.
 */
				if (vmemmap_populate_basepages(addr, next, node, altmap))
					return -ENOMEM;
				continue;
			}

			pmd_set_huge(pmdp, __pa(p), __pgprot(PROT_SECT_NORMAL));
		} else
			vmemmap_verify((pte_t *)pmdp, node, addr, next);
	} while (addr = next, addr != end);

	return 0;
}
#endif	/* !ARM64_KERNEL_USES_PMD_MAPS */

#ifdef CONFIG_MEMORY_HOTPLUG
void vmemmap_free(unsigned long start, unsigned long end,
		struct vmem_altmap *altmap)
{
	WARN_ON((start < VMEMMAP_START) || (end > VMEMMAP_END));

	unmap_hotplug_range(start, end, true, altmap);
	free_empty_tables(start, end, VMEMMAP_START, VMEMMAP_END);
}
#endif /* CONFIG_MEMORY_HOTPLUG */

/* IAMROOT, 2021.10.02:
 * - va(@addr)의 va(pud table)을 반환한다.
 *   @return: va(pud table) == va(p4dp + pud offset)
 *
 *   bm_pud는 statically allocate 되어 있으므로 kernel image 공간에 저장되고
 *   symbol로 접근 가능하므로 xxx_offset_kimg(..)를 사용한다.
 */
static inline pud_t *fixmap_pud(unsigned long addr)
{
	pgd_t *pgdp = pgd_offset_k(addr);
	p4d_t *p4dp = p4d_offset(pgdp, addr);
	p4d_t p4d = READ_ONCE(*p4dp);

	/* IAMROOT, 2023.11.18:
	 * - 이 시점에 *p4dp에는 이미 pa(bm_pud)가 매핑되어 있어야 한다.
	 */
	BUG_ON(p4d_none(p4d) || p4d_bad(p4d));

	return pud_offset_kimg(p4dp, addr);
}

/* IAMROOT, 2021.10.02:
 * - va(@addr)에 해당하는 pmd entry주소를 반환한다.
 *   fixmap은 kernel image안의 symbol 정의이므로 xxx_offset_kimg를 사용한다.
 */
static inline pmd_t *fixmap_pmd(unsigned long addr)
{
	pud_t *pudp = fixmap_pud(addr);
	pud_t pud = READ_ONCE(*pudp);

	BUG_ON(pud_none(pud) || pud_bad(pud));

	return pmd_offset_kimg(pudp, addr);
}

/* IAMROOT, 2021.10.09:
 * - va(@addr)의 pte index를 계산하여 fixmap이 사용하는 bm_pte 테이블의
 *   entry addr를 반환한다.
 */
static inline pte_t *fixmap_pte(unsigned long addr)
{
	return &bm_pte[pte_index(addr)];
}

/*
 * The p*d_populate functions call virt_to_phys implicitly so they can't be used
 * directly on kernel symbols (bm_p*d). This function is called too early to use
 * lm_alias so __p*d_populate functions must be used to populate with the
 * physical address from __pa_symbol.
 */
/* IAMROOT, 2023.11.18:
 * - compile-time에 vaddr가 결정되는 공간이며 주로 dynamic mapping subsystem이
 *   활성화되기 전 vaddr 매핑이 필요할 때 사용하는 공간을 fixmap이라 한다.
 *   주로 early console 사용이나 runtime에 kernel code 변경, 페이지 테이블 갱신
 *   용도로 사용된다.
 *
 *                      pgd=p4d  ->    pud  ->      pmd   ->     pte
 *                   ------------------------------------------------
 * - normal case:    init_pg_dir -> bm_pud  ->   bm_pmd   ->  bm_pte
 * - 16K, 4lvl case: init_pg_dir -> 기존pud ->   bm_pmd   ->  bm_pte
 */
void __init early_fixmap_init(void)
{
	pgd_t *pgdp;
	p4d_t *p4dp, p4d;
	pud_t *pudp;
	pmd_t *pmdp;
	unsigned long addr = FIXADDR_START;

	pgdp = pgd_offset_k(addr);
	p4dp = p4d_offset(pgdp, addr);
	p4d = READ_ONCE(*p4dp);

	/* IAMROOT, 2023.11.18:
	 * - pgdp, p4dp는 init_pg_dir을 가리킨다.
	 *   (kernel은 아직 p4dp를 사용하지 않으므로 pgdp == p4dp 이다.
	 */

	if (CONFIG_PGTABLE_LEVELS > 3 &&
	    !(p4d_none(p4d) || p4d_page_paddr(p4d) == __pa_symbol(bm_pud))) {
		/* IAMROOT, 2023.11.18:
		 * - 1) CONFIG_PGTABLE_LEVELS >= 4 이고
		 *   2) p4d가 가리키는 pud entry가 이미 세팅되어 있는데,
		 *      pa(p4d) != pa(bm_pud)라면 진입한다.
		 *
		 *   +-----+------------+----------+----------+----------+-------------+
		 *   | k/u | PGDIR bits | PUD bits | PMD bits | PTE bits | offset bits |
		 *   | 16  | 1          | 11       | 11       | 11       | 14          |
		 *   +-----+------------+----------+----------+----------+-------------+
		 *
		 * - fixmap의 특정 주소를 예로 했을때 산출되는 각 table별 masking 값.
		 *   1 : ---
		 *   111_1101_1111 : 0x7df
		 *   111_1111_1111 : 0x7ff
		 *   001_0111_1110 : 0x17e
		 *
		 * - 16k / 4 level일 경우 pgd는 2개 entry밖에 존재 하지 않는다.
		 *
		 * - p4d가 이미 kernel에 의해 mapping 되어 있으므로 있는걸 사용한다.
		 *   bm_pud를 매핑하지 않고 va(pud table)만 구한다.
		 */
		/*
		 * We only end up here if the kernel mapping and the fixmap
		 * share the top level pgd entry, which should only happen on
		 * 16k/4 levels configurations.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
		pudp = pud_offset_kimg(p4dp, addr);
	} else {
		/* IAMROOT, 2021.10.02:
		 * - 1) p4d가 가리키는 pud entry가 세팅되어 있지 않거나,
		 *   2) pa(p4d) == pa(bm_pud)라면 진입한다.
		 *
		 *   만약 p4d에 pud가 매핑되어 있지 않다면 p4d에 bm_pud를 매핑하고
		 *   fixmap_pud를 통해 va(pud table)을 구한다.
		 *
		 * - C 코드 해석
		 *   if (p4d == 0)
		 *      *p4dp = __pa(bm_pud);
		 *   pudp = __va(*p4dp + pud_index(addr) * 8);
		 *   pudp == va(bm_pud table);
		 */
		if (p4d_none(p4d))
			__p4d_populate(p4dp, __pa_symbol(bm_pud), P4D_TYPE_TABLE);
		pudp = fixmap_pud(addr);
	}

	/* IAMROOT, 2021.10.02:
	 * - 만약 pud에 pmd가 매핑되어 있지 않다면 pud에 bm_pmd를 매핑하고
	 *   fixmap_pmd를 통해 va(pmd table)을 구한다.
	 *
	 * - C 코드 해석
	 *   if (*pudp == 0)
	 *      *pudp = __pa(bm_pmd);
	 *   pmdp == va(bm_pmd table);
	 */
	if (pud_none(READ_ONCE(*pudp)))
		__pud_populate(pudp, __pa_symbol(bm_pmd), PUD_TYPE_TABLE);
	pmdp = fixmap_pmd(addr);

	/* IAMROOT, 2021.10.02:
	 * - pmd table entry에 pa(bm_pte)를 매핑한다.
	 *   pte는 최종 4KB page entries 가리키므로 fixmap에서 사용할 table을
	 *   더이상 매핑하지 않는다.
	 *
	 * - C 코드 해석
	 *   *pmdp = __pa(bm_pte);
	 */
	__pmd_populate(pmdp, __pa_symbol(bm_pte), PMD_TYPE_TABLE);

	/* IAMROOT, 2021.10.18:
	 * - 일반적인 상황에서는 early fixmap이 다음과 같은 메모리 공간을 커버한다.
	 *   pgd (1 table) >> bm_pud (1 table) >> bm_pmd (1 table) >> bm_pte (512 pages)
	 *   == 512 * 4KB == 2MB
	 */

	/*
	 * The boot-ioremap range spans multiple pmds, for which
	 * we are not prepared:
	 */
	BUILD_BUG_ON((__fix_to_virt(FIX_BTMAP_BEGIN) >> PMD_SHIFT)
		     != (__fix_to_virt(FIX_BTMAP_END) >> PMD_SHIFT));

	/* IAMROOT, 2021.10.09:
	 * - bm_pmd table로 FIX_BTMAP_* 영역을 커버할 수 있는지 확인한다.
	 *   BTMAP 영역은 early_ioremap() 용도로 사용되며 부팅 후
	 *   unmapping 될 수 있다.
	 */
	if ((pmdp != fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)))
	     || pmdp != fixmap_pmd(fix_to_virt(FIX_BTMAP_END))) {
		WARN_ON(1);
		pr_warn("pmdp %p != %p, %p\n",
			pmdp, fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)),
			fixmap_pmd(fix_to_virt(FIX_BTMAP_END)));
		pr_warn("fix_to_virt(FIX_BTMAP_BEGIN): %08lx\n",
			fix_to_virt(FIX_BTMAP_BEGIN));
		pr_warn("fix_to_virt(FIX_BTMAP_END):   %08lx\n",
			fix_to_virt(FIX_BTMAP_END));

		pr_warn("FIX_BTMAP_END:       %d\n", FIX_BTMAP_END);
		pr_warn("FIX_BTMAP_BEGIN:     %d\n", FIX_BTMAP_BEGIN);
	}
}

/*
 * Unusually, this is also called in IRQ context (ghes_iounmap_irq) so if we
 * ever need to use IPIs for TLB broadcasting, then we're in trouble here.
 */
/* IAMROOT, 2021.10.09:
 * - pa(@phys)를 fixmap @idx가 사용하는 bm_pte의 entry에 mapping 한다.
 */
void __set_fixmap(enum fixed_addresses idx,
			       phys_addr_t phys, pgprot_t flags)
{
	unsigned long addr = __fix_to_virt(idx);
	pte_t *ptep;

	BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses);

	/* IAMROOT, 2023.12.09:
	 * - bm_pte[..] table만 사용하고 있는데 @idx를 통해서 @addr를 구하므로
	 *   FIX_PUD, PMD, PTE에 따라 PAGE 크기만큼 정렬되어 있어 @idx만 같지
	 *   않으면 동일한 elem을 중복 사용하지 않으므로 bm_pte 하나로
	 *   3 level table을 커버할 수 있게 된다.
	 */
	ptep = fixmap_pte(addr);

	if (pgprot_val(flags)) {
		set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, flags));
	} else {
		pte_clear(&init_mm, addr, ptep);
		flush_tlb_kernel_range(addr, addr+PAGE_SIZE);
	}
}

/* IAMROOT, 2021.10.14:
 * - bootloader에서 전달받은 fdt의 pa(dt_phys)를 FIX_FDT 영역(approx. 4MB)에
 *   mapping 하여 mmu를 통해 vaddr로 접근할 수 있게 셋업한다.
 *   (fixmap은 FIX_FDT 관련 주석 참고)
 */
void *__init fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot)
{
	const u64 dt_virt_base = __fix_to_virt(FIX_FDT);
	int offset;
	void *dt_virt;

	/*
	 * Check whether the physical FDT address is set and meets the minimum
	 * alignment requirement. Since we are relying on MIN_FDT_ALIGN to be
	 * at least 8 bytes so that we can always access the magic and size
	 * fields of the FDT header after mapping the first chunk, double check
	 * here if that is indeed the case.
	 */
/* IAMROOT, 2021.10.14:
 * - @dt_phys == NULL이거나 MIN_FDT_ALIGN(8)에 맞춰 정렬되어 있는지 검사.
 */
	BUILD_BUG_ON(MIN_FDT_ALIGN < 8);
	if (!dt_phys || dt_phys % MIN_FDT_ALIGN)
		return NULL;

	/*
	 * Make sure that the FDT region can be mapped without the need to
	 * allocate additional translation table pages, so that it is safe
	 * to call create_mapping_noalloc() this early.
	 *
	 * On 64k pages, the FDT will be mapped using PTEs, so we need to
	 * be in the same PMD as the rest of the fixmap.
	 * On 4k pages, we'll use section mappings for the FDT so we only
	 * have to be in the same PUD.
	 */
	BUILD_BUG_ON(dt_virt_base % SZ_2M);

	BUILD_BUG_ON(__fix_to_virt(FIX_FDT_END) >> SWAPPER_TABLE_SHIFT !=
		     __fix_to_virt(FIX_BTMAP_BEGIN) >> SWAPPER_TABLE_SHIFT);

	offset = dt_phys % SWAPPER_BLOCK_SIZE;
	dt_virt = (void *)dt_virt_base + offset;

/* IAMROOT, 2021.10.09:
 * - fdt header를 읽어오기 위해 SWAPPER_BLOCK_SIZE 만큼 한 chunk를 매핑한다.
 *   (커널 옵션에 따라 4KB or 2MB가 될 수 있음)
 */
	/* map the first chunk so we can read the size from the header */
	create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE),
			dt_virt_base, SWAPPER_BLOCK_SIZE, prot);

/* IAMROOT, 2021.10.09:
 * - magic number를 확인해서 fdt 데이터인지 확인한다.
 */
	if (fdt_magic(dt_virt) != FDT_MAGIC)
		return NULL;

/* IAMROOT, 2021.10.09:
 * - fdt 전체 size를 알아온 뒤 매핑되지 않은 나머지 부분을 계속 진행한다.
 */
	*size = fdt_totalsize(dt_virt);
	if (*size > MAX_FDT_SIZE)
		return NULL;

	if (offset + *size > SWAPPER_BLOCK_SIZE)
		create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE), dt_virt_base,
			       round_up(offset + *size, SWAPPER_BLOCK_SIZE), prot);

	return dt_virt;
}

/* IAMROOT, 2023.12.05:
 * - 1G 크기의 block을 매핑할 수 있는 PUD entry를 가져온다.
 */
int pud_set_huge(pud_t *pudp, phys_addr_t phys, pgprot_t prot)
{
	pud_t new_pud = pfn_pud(__phys_to_pfn(phys), mk_pud_sect_prot(prot));

	/* Only allow permission changes for now */
	if (!pgattr_change_is_safe(READ_ONCE(pud_val(*pudp)),
				   pud_val(new_pud)))
		return 0;

	VM_BUG_ON(phys & ~PUD_MASK);
	set_pud(pudp, new_pud);
	return 1;
}

/* IAMROOT, 2023.12.05:
 * - 2MB 크기의 block을 매핑할 수 있는 PMD entry를 가져온다.
 */
int pmd_set_huge(pmd_t *pmdp, phys_addr_t phys, pgprot_t prot)
{
	pmd_t new_pmd = pfn_pmd(__phys_to_pfn(phys), mk_pmd_sect_prot(prot));

	/* Only allow permission changes for now */
	if (!pgattr_change_is_safe(READ_ONCE(pmd_val(*pmdp)),
				   pmd_val(new_pmd)))
		return 0;

	VM_BUG_ON(phys & ~PMD_MASK);
	set_pmd(pmdp, new_pmd);
	return 1;
}

int pud_clear_huge(pud_t *pudp)
{
	if (!pud_sect(READ_ONCE(*pudp)))
		return 0;
	pud_clear(pudp);
	return 1;
}

int pmd_clear_huge(pmd_t *pmdp)
{
	if (!pmd_sect(READ_ONCE(*pmdp)))
		return 0;
	pmd_clear(pmdp);
	return 1;
}

int pmd_free_pte_page(pmd_t *pmdp, unsigned long addr)
{
	pte_t *table;
	pmd_t pmd;

	pmd = READ_ONCE(*pmdp);

	if (!pmd_table(pmd)) {
		VM_WARN_ON(1);
		return 1;
	}

	table = pte_offset_kernel(pmdp, addr);
	pmd_clear(pmdp);
	__flush_tlb_kernel_pgtable(addr);
	pte_free_kernel(NULL, table);
	return 1;
}

int pud_free_pmd_page(pud_t *pudp, unsigned long addr)
{
	pmd_t *table;
	pmd_t *pmdp;
	pud_t pud;
	unsigned long next, end;

	pud = READ_ONCE(*pudp);

	if (!pud_table(pud)) {
		VM_WARN_ON(1);
		return 1;
	}

	table = pmd_offset(pudp, addr);
	pmdp = table;
	next = addr;
	end = addr + PUD_SIZE;
	do {
		pmd_free_pte_page(pmdp, next);
	} while (pmdp++, next += PMD_SIZE, next != end);

	pud_clear(pudp);
	__flush_tlb_kernel_pgtable(addr);
	pmd_free(NULL, table);
	return 1;
}

#ifdef CONFIG_MEMORY_HOTPLUG
static void __remove_pgd_mapping(pgd_t *pgdir, unsigned long start, u64 size)
{
	unsigned long end = start + size;

	WARN_ON(pgdir != init_mm.pgd);
	WARN_ON((start < PAGE_OFFSET) || (end > PAGE_END));

	unmap_hotplug_range(start, end, false, NULL);
	free_empty_tables(start, end, PAGE_OFFSET, PAGE_END);
}

struct range arch_get_mappable_range(void)
{
	struct range mhp_range;
	u64 start_linear_pa = __pa(_PAGE_OFFSET(vabits_actual));
	u64 end_linear_pa = __pa(PAGE_END - 1);

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		/*
		 * Check for a wrap, it is possible because of randomized linear
		 * mapping the start physical address is actually bigger than
		 * the end physical address. In this case set start to zero
		 * because [0, end_linear_pa] range must still be able to cover
		 * all addressable physical addresses.
		 */
		if (start_linear_pa > end_linear_pa)
			start_linear_pa = 0;
	}

	WARN_ON(start_linear_pa > end_linear_pa);

	/*
	 * Linear mapping region is the range [PAGE_OFFSET..(PAGE_END - 1)]
	 * accommodating both its ends but excluding PAGE_END. Max physical
	 * range which can be mapped inside this linear mapping range, must
	 * also be derived from its end points.
	 */
	mhp_range.start = start_linear_pa;
	mhp_range.end =  end_linear_pa;

	return mhp_range;
}

int arch_add_memory(int nid, u64 start, u64 size,
		    struct mhp_params *params)
{
	int ret, flags = NO_EXEC_MAPPINGS;

	VM_BUG_ON(!mhp_range_allowed(start, size, true));

	/*
	 * KFENCE requires linear map to be mapped at page granularity, so that
	 * it is possible to protect/unprotect single pages in the KFENCE pool.
	 */
	if (can_set_direct_map() || IS_ENABLED(CONFIG_KFENCE))
		flags |= NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

	__create_pgd_mapping(swapper_pg_dir, start, __phys_to_virt(start),
			     size, params->pgprot, __pgd_pgtable_alloc,
			     flags);

	memblock_clear_nomap(start, size);

	ret = __add_pages(nid, start >> PAGE_SHIFT, size >> PAGE_SHIFT,
			   params);
	if (ret)
		__remove_pgd_mapping(swapper_pg_dir,
				     __phys_to_virt(start), size);
	return ret;
}

void arch_remove_memory(u64 start, u64 size, struct vmem_altmap *altmap)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;

	__remove_pages(start_pfn, nr_pages, altmap);
	__remove_pgd_mapping(swapper_pg_dir, __phys_to_virt(start), size);
}

/*
 * This memory hotplug notifier helps prevent boot memory from being
 * inadvertently removed as it blocks pfn range offlining process in
 * __offline_pages(). Hence this prevents both offlining as well as
 * removal process for boot memory which is initially always online.
 * In future if and when boot memory could be removed, this notifier
 * should be dropped and free_hotplug_page_range() should handle any
 * reserved pages allocated during boot.
 */
static int prevent_bootmem_remove_notifier(struct notifier_block *nb,
					   unsigned long action, void *data)
{
	struct mem_section *ms;
	struct memory_notify *arg = data;
	unsigned long end_pfn = arg->start_pfn + arg->nr_pages;
	unsigned long pfn = arg->start_pfn;

	if ((action != MEM_GOING_OFFLINE) && (action != MEM_OFFLINE))
		return NOTIFY_OK;

	for (; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		unsigned long start = PFN_PHYS(pfn);
		unsigned long end = start + (1UL << PA_SECTION_SHIFT);

		ms = __pfn_to_section(pfn);
		if (!early_section(ms))
			continue;

		if (action == MEM_GOING_OFFLINE) {
			/*
			 * Boot memory removal is not supported. Prevent
			 * it via blocking any attempted offline request
			 * for the boot memory and just report it.
			 */
			pr_warn("Boot memory [%lx %lx] offlining attempted\n", start, end);
			return NOTIFY_BAD;
		} else if (action == MEM_OFFLINE) {
			/*
			 * This should have never happened. Boot memory
			 * offlining should have been prevented by this
			 * very notifier. Probably some memory removal
			 * procedure might have changed which would then
			 * require further debug.
			 */
			pr_err("Boot memory [%lx %lx] offlined\n", start, end);

			/*
			 * Core memory hotplug does not process a return
			 * code from the notifier for MEM_OFFLINE events.
			 * The error condition has been reported. Return
			 * from here as if ignored.
			 */
			return NOTIFY_DONE;
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block prevent_bootmem_remove_nb = {
	.notifier_call = prevent_bootmem_remove_notifier,
};

/*
 * This ensures that boot memory sections on the platform are online
 * from early boot. Memory sections could not be prevented from being
 * offlined, unless for some reason they are not online to begin with.
 * This helps validate the basic assumption on which the above memory
 * event notifier works to prevent boot memory section offlining and
 * its possible removal.
 */
static void validate_bootmem_online(void)
{
	phys_addr_t start, end, addr;
	struct mem_section *ms;
	u64 i;

	/*
	 * Scanning across all memblock might be expensive
	 * on some big memory systems. Hence enable this
	 * validation only with DEBUG_VM.
	 */
	if (!IS_ENABLED(CONFIG_DEBUG_VM))
		return;

	for_each_mem_range(i, &start, &end) {
		for (addr = start; addr < end; addr += (1UL << PA_SECTION_SHIFT)) {
			ms = __pfn_to_section(PHYS_PFN(addr));

			/*
			 * All memory ranges in the system at this point
			 * should have been marked as early sections.
			 */
			WARN_ON(!early_section(ms));

			/*
			 * Memory notifier mechanism here to prevent boot
			 * memory offlining depends on the fact that each
			 * early section memory on the system is initially
			 * online. Otherwise a given memory section which
			 * is already offline will be overlooked and can
			 * be removed completely. Call out such sections.
			 */
			if (!online_section(ms))
				pr_err("Boot memory [%llx %llx] is offline, can be removed\n",
					addr, addr + (1UL << PA_SECTION_SHIFT));
		}
	}
}

static int __init prevent_bootmem_remove_init(void)
{
	int ret = 0;

	if (!IS_ENABLED(CONFIG_MEMORY_HOTREMOVE))
		return ret;

	validate_bootmem_online();
	ret = register_memory_notifier(&prevent_bootmem_remove_nb);
	if (ret)
		pr_err("%s: Notifier registration failed %d\n", __func__, ret);

	return ret;
}
early_initcall(prevent_bootmem_remove_init);
#endif
