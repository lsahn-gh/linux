/*
 * fixmap.h: compile-time virtual memory allocation
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1998 Ingo Molnar
 * Copyright (C) 2013 Mark Salter <msalter@redhat.com>
 *
 * Adapted from arch/x86 version.
 *
 */

#ifndef _ASM_ARM64_FIXMAP_H
#define _ASM_ARM64_FIXMAP_H

#ifndef __ASSEMBLY__
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <asm/boot.h>
#include <asm/page.h>
#include <asm/pgtable-prot.h>

/* IAMROOT, 2021.09.04:
 * - fixmap은 가상주소의 특정범위를 특정용도로 고정하여 사용하기 위함이다.
 *   전체 크기는 대략 6MB 정도 나온다.
 *
 * - fixed map
 *   compile-time에 고정된 vaddr. 모든 메모리는 dynamic memory mapping에 의해
 *   관리가 되야 되는데 초기엔 아직 기능이 동작을 안하므로 임시로 사용하기 위함.
 *   또한 몇가지 extra 기능을 위한 고정 주소로 사용되기도 한다.
 *
 * - FIX_EARLYCON_MEM_BASE:
 *   부팅하고 driver를 셋업하기 전에 어떤 driver가 존재하는지 관련 정보 제공.
 *
 * - FIX_TEXT_POKE0:
 *   kernel code가 read only 영역에 mapping 되있는데, 해당 fixmap region은
 *   r/w 영역으로써 kernel code를 수정할때 사용된다.
 *
 * - FIX_ENTRY_TRAMP_DATA:
 *   보안을 위해 el0로부터 el1을 감추기 위해 사용한다.
 *
 * - FIX_PGD ...:
 *   FIX_TEXT_POKE0과 비슷한 이유로 page table을 수정할때 잠시 mapping하기
 *   위한 것이며, 4개의 page table을 전부 mapping 한 후 atomic하게 수정한다.
 *
 * - fixed_address는 __end_of_permanent_fixed_addresses를 기준으로 2개로 나뉜다.
 *   여기서 FIXMAP_SIZE는 __end_of_fixed_addresses 아래의 FIX_PGD ... 등은
 *   제외한 크기가 된다.
 *
 * - 각 enum item의 크기가 PAGE_SIZE라 할 때 nr * PAGE_SIZE로 계산하면
 *   전체 fixmap의 크기를 예측할 수 있다.
 */
/*
 * Here we define all the compile-time 'special' virtual
 * addresses. The point is to have a constant address at
 * compile time, but to set the physical address only
 * in the boot process.
 *
 * Each enum increment in these 'compile-time allocated'
 * memory buffers is page-sized. Use set_fixmap(idx,phys)
 * to associate physical memory with a fixmap index.
 */
enum fixed_addresses {
	FIX_HOLE,

	/*
	 * Reserve a virtual window for the FDT that is 2 MB larger than the
	 * maximum supported size, and put it at the top of the fixmap region.
	 * The additional space ensures that any FDT that does not exceed
	 * MAX_FDT_SIZE can be mapped regardless of whether it crosses any
	 * 2 MB alignment boundaries.
	 *
	 * Keep this at the top so it remains 2 MB aligned.
	 */
#define FIX_FDT_SIZE		(MAX_FDT_SIZE + SZ_2M)
	FIX_FDT_END,
	/* IAMROOT, 2021.10.14:
	 * - FIX_FDT == 1024 by the following values ...
	 *     FIX_FDT_SIZE == 4MB
	 *     PAGE_SIZE    == 4KB
	 *     FIX_FDT_END  == 1
	 */
	FIX_FDT = FIX_FDT_END + FIX_FDT_SIZE / PAGE_SIZE - 1,

	FIX_EARLYCON_MEM_BASE,
	FIX_TEXT_POKE0,

#ifdef CONFIG_ACPI_APEI_GHES
	/* Used for GHES mapping from assorted contexts */
	FIX_APEI_GHES_IRQ,
	FIX_APEI_GHES_SEA,
#ifdef CONFIG_ARM_SDE_INTERFACE
	FIX_APEI_GHES_SDEI_NORMAL,
	FIX_APEI_GHES_SDEI_CRITICAL,
#endif
#endif /* CONFIG_ACPI_APEI_GHES */

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	FIX_ENTRY_TRAMP_DATA,
	FIX_ENTRY_TRAMP_TEXT,
#define TRAMP_VALIAS		(__fix_to_virt(FIX_ENTRY_TRAMP_TEXT))
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

	/* IAMROOT, 2021.09.04:
	 * - 크기를 대략 계산하면 FIX_FDT_SIZE == 4MB 이고
	 *    - FIX_EARLYCON_MEM_BASE (4KB)
	 *    - FIX_TEXT_POKE0 (4KB)
	 *   위 items가 enable 된다면 4MB + 8KB 크기를 가진다.
	 */
	__end_of_permanent_fixed_addresses,

	/*
	 * Temporary boot-time mappings, used by early_ioremap(),
	 * before ioremap() is functional.
	 */
/* IAMROOT, 2023.11.12:
 * - 정규 매핑(paging_init)전에 I/O 장치들이 사용되는 경우를 위한 영역.
 *   1. ACPI 테이블 접근 후 디바이스 정보를 읽어야 하는 경우.
 *   2. EFI 테이블 접근 후 디바이스 정보를 읽어야 하는 경우.
 *   3. 일부 디바이스에서 설정 정보를 읽어야 하는 경우.
 *
 * - 총 7개의 slot에 slot당 64 page를 커버한다.
 *
 * - NR_FIX_BTMAPS    == 64 (256KB / 4KB == PAGE_SIZE)
 *   FIX_BTMAPS_SLOTS == 7
 *   TOTAL_FIX_BTMAPS == 448 (64 * 7)
 */
#define NR_FIX_BTMAPS		(SZ_256K / PAGE_SIZE)
#define FIX_BTMAPS_SLOTS	7
#define TOTAL_FIX_BTMAPS	(NR_FIX_BTMAPS * FIX_BTMAPS_SLOTS)

	FIX_BTMAP_END = __end_of_permanent_fixed_addresses,
	FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1,

	/*
	 * Used for kernel page table creation, so unmapped memory may be used
	 * for tables.
	 */
	FIX_PTE,
	FIX_PMD,
	FIX_PUD,
	FIX_PGD,

	/* IAMROOT, 2023.11.12:
	 * - 여기까지 크기는 5.8MB + 24KB (4MB + 8KB + 1.8MB + 16KB).
	 */

	__end_of_fixed_addresses
};

/* IAMROOT, 2021.09.07:
 * - fixmap addr size와 start point를 정의한다.
 *   __end_of_permanent_fixed_addresses 는 page 단위로 크기가 정해져있어
 *   PAGE_SHIFT를 사용하여 left shift를 수행하면 '* PAGE_SIZE'를 한 것과 같다.
 *
 * - arch/arm64/include/asm/memory.h에서 그린 memory map 참고하면 파악 가능.
 */
#define FIXADDR_SIZE	(__end_of_permanent_fixed_addresses << PAGE_SHIFT)
#define FIXADDR_START	(FIXADDR_TOP - FIXADDR_SIZE)

#define FIXMAP_PAGE_IO     __pgprot(PROT_DEVICE_nGnRE)

void __init early_fixmap_init(void);

#define __early_set_fixmap __set_fixmap

#define __late_set_fixmap __set_fixmap
#define __late_clear_fixmap(idx) __set_fixmap((idx), 0, FIXMAP_PAGE_CLEAR)

extern void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot);

#include <asm-generic/fixmap.h>

#endif /* !__ASSEMBLY__ */
#endif /* _ASM_ARM64_FIXMAP_H */
