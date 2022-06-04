// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/copypage.c
 *
 * Copyright (C) 2002 Deep Blue Solutions Ltd, All Rights Reserved.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/bitops.h>
#include <linux/mm.h>

#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/mte.h>

/*
 * IAMROOT, 2022.04.09:
 * - arm64는 highmem이 없으므로 그냥 copy한다.
 */
void copy_highpage(struct page *to, struct page *from)
{
/*
 * IAMROOT, 2022.04.09:
 * - to, from : 물리주소를 가리킴.
 *   kto, kfrom : 가상주소를 가리킴. void *로 해야 맞는데 그냥 struct page *를
 *   쓴거 같다.
 */
	struct page *kto = page_address(to);
	struct page *kfrom = page_address(from);

	copy_page(kto, kfrom);

/*
 * IAMROOT, 2022.04.09:
 * - @from에 mte tag가 있다면 @to측에도 mte tag를 설정해준다.
 */
	if (system_supports_mte() && test_bit(PG_mte_tagged, &from->flags)) {
		set_bit(PG_mte_tagged, &to->flags);
		page_kasan_tag_reset(to);
		/*
		 * We need smp_wmb() in between setting the flags and clearing the
		 * tags because if another thread reads page->flags and builds a
		 * tagged address out of it, there is an actual dependency to the
		 * memory access, but on the current thread we do not guarantee that
		 * the new page->flags are visible before the tags were updated.
		 */
		smp_wmb();
		mte_copy_page_tags(kto, kfrom);
	}
}
EXPORT_SYMBOL(copy_highpage);

/*
 * IAMROOT, 2022.06.04:
 * - from을 to로 복사후 PG_dcache_clean bit clear
 */
void copy_user_highpage(struct page *to, struct page *from,
			unsigned long vaddr, struct vm_area_struct *vma)
{
	copy_highpage(to, from);
	flush_dcache_page(to);
}
EXPORT_SYMBOL_GPL(copy_user_highpage);
