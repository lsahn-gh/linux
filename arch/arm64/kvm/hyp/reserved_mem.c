// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 - Google LLC
 * Author: Quentin Perret <qperret@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/memblock.h>
#include <linux/sort.h>

#include <asm/kvm_host.h>

#include <nvhe/memory.h>
#include <nvhe/mm.h>

static struct memblock_region *hyp_memory = kvm_nvhe_sym(hyp_memory);
static unsigned int *hyp_memblock_nr_ptr = &kvm_nvhe_sym(hyp_memblock_nr);

/*
 * IAMROOT, 2021.11.13:
 * - kvm_hyp_reserve 에서 초기화된다.
 */
phys_addr_t hyp_mem_base;
phys_addr_t hyp_mem_size;

static int cmp_hyp_memblock(const void *p1, const void *p2)
{
	const struct memblock_region *r1 = p1;
	const struct memblock_region *r2 = p2;

	return r1->base < r2->base ? -1 : (r1->base > r2->base);
}

static void __init sort_memblock_regions(void)
{
	sort(hyp_memory,
	     *hyp_memblock_nr_ptr,
	     sizeof(struct memblock_region),
	     cmp_hyp_memblock,
	     NULL);
}

/*
 * IAMROOT, 2021.11.13:
 * - memory region들을 전부 hyp_memblock으로 복사한다.
 */
static int __init register_memblock_regions(void)
{
	struct memblock_region *reg;

	for_each_mem_region(reg) {
		if (*hyp_memblock_nr_ptr >= HYP_MEMBLOCK_REGIONS)
			return -ENOMEM;

		hyp_memory[*hyp_memblock_nr_ptr] = *reg;
		(*hyp_memblock_nr_ptr)++;
	}
	sort_memblock_regions();

	return 0;
}

/*
 * IAMROOT, 2021.11.13:
 * - kvm nvhe로 진입시(EL2부팅, EL1 동작) hyp_memory에 대해서 stage 1,
 *   stage 2에 필요한 page 개수를 구하고 memory를 할당한다.
 */
void __init kvm_hyp_reserve(void)
{
	u64 nr_pages, prev, hyp_mem_pages = 0;
	int ret;
/*
 * IAMROOT, 2021.11.13:
 * - hyp mode가 될수없거나 현재 mode가 EL2면 return.
 *   return이 안된다면, EL2로 부팅을 시도했지만 어떤 이유에서 EL1인 상황.
 *   kvm nvhe인 상황이다. 
 */
	if (!is_hyp_mode_available() || is_kernel_in_hyp_mode())
		return;
/*
 * IAMROOT, 2021.11.13:
 * - early param으로 mode가 설정됬는지 확인
 */
	if (kvm_get_mode() != KVM_MODE_PROTECTED)
		return;

	ret = register_memblock_regions();
	if (ret) {
		*hyp_memblock_nr_ptr = 0;
		kvm_err("Failed to register hyp memblocks: %d\n", ret);
		return;
	}
/*
 * IAMROOT, 2021.11.13:
 * - stage1, stage2에서 필요한 page table 개수를 구해온다.
 */
	hyp_mem_pages += hyp_s1_pgtable_pages();
	hyp_mem_pages += host_s2_pgtable_pages();

	/*
	 * The hyp_vmemmap needs to be backed by pages, but these pages
	 * themselves need to be present in the vmemmap, so compute the number
	 * of pages needed by looking for a fixed point.
	 */
	nr_pages = 0;
/*
 * IAMROOT, 2021.11.13:
 * - page마다 struct hyp_page이 필요해보이고 해당 자료구조도 또 page가
 *   필요하므로 관련해서 더 page 개수를 늘린다.
 */
	do {
		prev = nr_pages;
		nr_pages = hyp_mem_pages + prev;
		nr_pages = DIV_ROUND_UP(nr_pages * sizeof(struct hyp_page), PAGE_SIZE);
		nr_pages += __hyp_pgtable_max_pages(nr_pages);
	} while (nr_pages != prev);
	hyp_mem_pages += nr_pages;

	/*
	 * Try to allocate a PMD-aligned region to reduce TLB pressure once
	 * this is unmapped from the host stage-2, and fallback to PAGE_SIZE.
	 */
/*
 * IAMROOT, 2021.11.13:
 * - 일단 PMD_SIZE로 align해서 할당을 해보고 안되면 그냥 할당을 시도한다.
 */
	hyp_mem_size = hyp_mem_pages << PAGE_SHIFT;
	hyp_mem_base = memblock_phys_alloc(ALIGN(hyp_mem_size, PMD_SIZE),
					   PMD_SIZE);
	if (!hyp_mem_base)
		hyp_mem_base = memblock_phys_alloc(hyp_mem_size, PAGE_SIZE);
	else
		hyp_mem_size = ALIGN(hyp_mem_size, PMD_SIZE);

	if (!hyp_mem_base) {
		kvm_err("Failed to reserve hyp memory\n");
		return;
	}

	kvm_info("Reserved %lld MiB at 0x%llx\n", hyp_mem_size >> 20,
		 hyp_mem_base);
}
