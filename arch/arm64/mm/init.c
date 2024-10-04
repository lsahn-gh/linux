// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/init.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/cache.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/efi.h>
#include <linux/swiotlb.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/crash_dump.h>
#include <linux/hugetlb.h>
#include <linux/acpi_iort.h>
#include <linux/kmemleak.h>

#include <asm/boot.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/kvm_host.h>
#include <asm/memory.h>
#include <asm/numa.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <linux/sizes.h>
#include <asm/tlb.h>
#include <asm/alternative.h>
#include <asm/xen/swiotlb-xen.h>

/*
 * IAMROOT, 2021.10.23:
 * - memstart_addr: 
 *   커널 va <-> pa 변환에 사용되는 DRAM 물리 시작 주소가 사용한다.
 *   단, VA_BITS=52 환경에서는 DRAM 물리 시작 주소를 보정하여 사용한다.
 *
 *   arm64_memblock_init에서 초기화된다. 이 값이 초기화 되기전엔
 *   리니어 매핑이 완료되지 않은 상태이므로
 *   phys_to_virt등의 변환 매크로를 사용할수 없다.
 *
 *   해당값이 초기화할때 VA_BITS, vabits_actual에 따라서 보정을 해주는데
 *   다음과 같은 예제 사유를 따른다.
 *   
 * ex) VA_BITS = 48, vabits_actual = 48, 
 *     PAGE_SIZE = 4k, PHYS_OFFSET = memstart_addr = 0x8000_0000
 *
 *	PAGE_OFFSET = 0xffff_0000_0000_0000
 *
 *	__phys_to_virt(x) = (((x) - PHYS_OFFSET) | PAGE_OFFSET)
 *	x = 0x9000_0000
 *	__phys_to_virt(0x9000_0000) = 0x9000_0000 - 0x8000_0000 |
 *				      0xffff_0000_0000_0000
 *				    = 0xffff_0000_1000_0000
 *
 * ex) VA_BITS = 52,  vabits_actual = 48,
 *     PAGE_SIZE = 16, PHYS_OFFSWET = memstart_addr = 0x8000_0000 (틀린사례)
 *				    
 *	PAGE_OFFSET = 0xfff0_0000_0000_0000
 *	
 *	__phys_to_virt(x) = (((x) - PHYS_OFFSET) | PAGE_OFFSET)
 *	x = 0x9000_0000
 *	__phys_to_virt(0x9000_0000) = 0x9000_0000 - 0x8000_0000 |
 *				      0xfff0_0000_0000_0000
 *				    = 0xfff0_0000_1000_0000
 *
 * ex) VA_BITS = 52,  vabits_actual = 48,
 *     PAGE_SIZE = 16, PHYS_OFFSET = memstart_addr = -0xe_ffff_8000_0000 (memstart가 보정된 사례)
 *				    
 *	PAGE_OFFSET = 0xfff0_0000_0000_0000
 *
 *	__phys_to_virt(x) = (((x) - PHYS_OFFSET) | PAGE_OFFSET)
 *	x = 0x9000_0000
 *	__phys_to_virt(0x9000_0000) = 0x9000_0000 - (-0xe_ffff_8000_0000) |
 *				      0xfff0_0000_0000_0000
 *				    = 0xffff_0000_1000_0000
 *
 * 즉 실제 vabits와 compile 타임에 계산된 VA_BITS의 차이를 보정해줘야
 * 제대로된 물리, 가상주소 변환값이 계산된다.
 */
/*
 * We need to be able to catch inadvertent references to memstart_addr
 * that occur (potentially in generic code) before arm64_memblock_init()
 * executes, which assigns it its actual value. So use a default value
 * that cannot be mistaken for a real physical address.
 */
s64 memstart_addr __ro_after_init = -1;
EXPORT_SYMBOL(memstart_addr);

/*
 * If the corresponding config options are enabled, we create both ZONE_DMA
 * and ZONE_DMA32. By default ZONE_DMA covers the 32-bit addressable memory
 * unless restricted on specific platforms (e.g. 30-bit on Raspberry Pi 4).
 * In such case, ZONE_DMA32 covers the rest of the 32-bit addressable memory,
 * otherwise it is empty.
 */
/* IAMROOT, 2021.10.23: TODO
 * - zone_sizes_init(..)에서 DMA/DMA32/NORMAL의 max pfn을 구하면서
 *   설정한다.
 */
phys_addr_t arm64_dma_phys_limit __ro_after_init;

#ifdef CONFIG_KEXEC_CORE
/*
 * reserve_crashkernel() - reserves memory for crash kernel
 *
 * This function reserves memory area given in "crashkernel=" kernel command
 * line parameter. The memory reserved is used by dump capture kernel when
 * primary kernel is crashing.
 */
/* IAMROOT, 2021.10.23:
 * - crash dump 용도로 작은 kernel을 올릴 영역을 reserve 한다.
 */
static void __init reserve_crashkernel(void)
{
	unsigned long long crash_base, crash_size;
	unsigned long long crash_max = arm64_dma_phys_limit;
	int ret;

	ret = parse_crashkernel(boot_command_line, memblock_phys_mem_size(),
				&crash_size, &crash_base);
	/* no crashkernel= or invalid value specified */
	if (ret || !crash_size)
		return;

	crash_size = PAGE_ALIGN(crash_size);

	/* User specifies base address explicitly. */
	if (crash_base)
		crash_max = crash_base + crash_size;

	/* Current arm64 boot protocol requires 2MB alignment */
	crash_base = memblock_phys_alloc_range(crash_size, SZ_2M,
					       crash_base, crash_max);
	if (!crash_base) {
		pr_warn("cannot allocate crashkernel (size:0x%llx)\n",
			crash_size);
		return;
	}

	pr_info("crashkernel reserved: 0x%016llx - 0x%016llx (%lld MB)\n",
		crash_base, crash_base + crash_size, crash_size >> 20);

	/*
	 * The crashkernel memory will be removed from the kernel linear
	 * map. Inform kmemleak so that it won't try to access it.
	 */
	kmemleak_ignore_phys(crash_base);
	crashk_res.start = crash_base;
	crashk_res.end = crash_base + crash_size - 1;
}
#else
static void __init reserve_crashkernel(void)
{
}
#endif /* CONFIG_KEXEC_CORE */

/*
 * Return the maximum physical address for a zone accessible by the given bits
 * limit. If DRAM starts above 32-bit, expand the zone to the maximum
 * available memory, otherwise cap it at 32-bit.
 */
/* IAMROOT, 2024.08.06:
 * - @zone_bits를 입력받아 해당 zone이 수용할 수 있는 최대 paddr를 구한다.
 *   이때 paddr는 memblock_end_of_DRAM() 크기를 넘을 수 없다.
 */
static phys_addr_t __init max_zone_phys(unsigned int zone_bits)
{
	phys_addr_t zone_mask = DMA_BIT_MASK(zone_bits);
	phys_addr_t phys_start = memblock_start_of_DRAM();

	/* IAMROOT, 2024.08.06:
	 * - zone_mask 값:
	 *   PHYS_ADDR_MAX  (if) phys_start > U32_MAX
	 *   U32_MAX        (if) phys_start > zone_mask
	 *   DMA_BIT_MASK() (if) else
	 */
	if (phys_start > U32_MAX)
		zone_mask = PHYS_ADDR_MAX;
	else if (phys_start > zone_mask)
		zone_mask = U32_MAX;

	return min(zone_mask, memblock_end_of_DRAM() - 1) + 1;
}

/* IAMROOT, 2021.11.27:
 * - ZONE_DMA(opt), DMA32(opt), and NORMAL의 max pfn을 구한다.
 *
 *   @min: PFN_UP(memblock_start_of_DRAM())
 *   @max: PFN_DOWN(memblock_end_of_DRAM())
 *
 *   arm64은 일반적으로 ZONE_DMA 대신 ZONE_DMA32가 enable 된다.
 */
static void __init zone_sizes_init(unsigned long min, unsigned long max)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES]  = {0};
	unsigned int __maybe_unused acpi_zone_dma_bits;
	unsigned int __maybe_unused dt_zone_dma_bits;
	phys_addr_t __maybe_unused dma32_phys_limit = max_zone_phys(32);

#ifdef CONFIG_ZONE_DMA
	acpi_zone_dma_bits = fls64(acpi_iort_dma_get_max_cpu_address());
	dt_zone_dma_bits = fls64(of_dma_get_max_cpu_address(NULL));
	zone_dma_bits = min3(32U, dt_zone_dma_bits, acpi_zone_dma_bits);
	/* IAMROOT, 2024.08.06:
	 * - ZONE_DMA는 dt에서 읽은 device max addr 값으로 max pfn을 구한다.
	 */
	arm64_dma_phys_limit = max_zone_phys(zone_dma_bits);
	max_zone_pfns[ZONE_DMA] = PFN_DOWN(arm64_dma_phys_limit);
#endif
#ifdef CONFIG_ZONE_DMA32
	/* IAMROOT, 2024.08.06:
	 * - ZONE_DMA32는 상수 32를 이용하여 max pfn을 구한다.
	 */
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(dma32_phys_limit);
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = dma32_phys_limit;
#endif
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = PHYS_MASK + 1;
	/* IAMROOT, 2024.08.06:
	 * - ZONE_NORMAL은 @max 값을 이용하며 arm64 arch에서는
	 *   memblock_end_of_DRAM() 값이 넘어온다.
	*/
	max_zone_pfns[ZONE_NORMAL] = max;

	free_area_init(max_zone_pfns);
}

/* IAMROOT, 2021.12.18:
 * - @pfn이 유효한지 validation을 수행한다.
 */
int pfn_valid(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);
	struct mem_section *ms;

	/*
	 * Ensure the upper PAGE_SHIFT bits are clear in the
	 * pfn. Else it might lead to false positives when
	 * some of the upper bits are set, but the lower bits
	 * match a valid pfn.
	 */
	/* IAMROOT, 2024.09.22: TODO
	 */
	if (PHYS_PFN(addr) != pfn)
		return 0;

	/* IAMROOT, 2024.09.22:
	 * - section(@pfn) 값이 kernel이 가질 수 있는 max(nr sections)을
	 *   넘기면 false.
	 */
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;

	/* IAMROOT, 2024.09.22:
	 * - section(@pfn)이 유효한지 검사 후 invalid 라면 false.
	 */
	ms = __pfn_to_section(pfn);
	if (!valid_section(ms))
		return 0;

	/*
	 * ZONE_DEVICE memory does not have the memblock entries.
	 * memblock_is_map_memory() check for ZONE_DEVICE based
	 * addresses will always fail. Even the normal hotplugged
	 * memory will never have MEMBLOCK_NOMAP flag set in their
	 * memblock entries. Skip memblock search for all non early
	 * memory sections covering all of hotplug memory including
	 * both normal and ZONE_DEVICE based.
	 */
	/* IAMROOT, 2021.12.18:
	 * - early section이 아니라면 kernel config에 따라 VMEMMAP인 경우도
	 *   있으므로 이를 고려해 validation을 수행한다.
	 */
	if (!early_section(ms))
		return pfn_section_valid(ms, pfn);

	/* IAMROOT, 2024.09.22:
	 * - early section인 경우 memblock을 통해서 검사한다.
	 */
	return memblock_is_memory(addr);
}
EXPORT_SYMBOL(pfn_valid);

/*
 * IAMROOT, 2022.07.16:
 * - @pfn이 lm mapping되있는지 확인한다.
 */
int pfn_is_map_memory(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);

	/* avoid false positives for bogus PFNs, see comment in pfn_valid() */
	if (PHYS_PFN(addr) != pfn)
		return 0;

	return memblock_is_map_memory(addr);
}
EXPORT_SYMBOL(pfn_is_map_memory);

/* IAMROOT, 2021.10.23:
 * - kernel boottime에 bootargs에 넘겨주는 값 또는 dt를 통해 해당 변수를
 *   초기화할 수 있다.
 */
static phys_addr_t memory_limit = PHYS_ADDR_MAX;

/*
 * Limit the memory size that was specified via FDT.
 */
static int __init early_mem(char *p)
{
	if (!p)
		return 1;

	memory_limit = memparse(p, &p) & PAGE_MASK;
	pr_notice("Memory limited to %lldMB\n", memory_limit >> 20);

	return 0;
}
early_param("mem", early_mem);

void __init arm64_memblock_init(void)
{
	/* IAMROOT, 2021.10.23:
	 * - kernel 영역에서 half는 linear mapping region 이므로 lm region size를
	 *   구한다. (128TB)
	 *
	 * - VA_BITS == 48 환경
	 *   vabits_actual: 48
	 *   PAGE_END     : 0xffff800000000000
	 *   _PAGE_OFFSET : 0xffff000000000000
	 *   -----------------------------------
	 *                  0x0000800000000000 (128TB)
	 *
	 * - 4k, 48bits -> VA_BITS_MIN: 48, vabits_actual: 48
	 *      0xffff800000000000 - 0xffff000000000000 = 0x800000000000 == 128TB
	 *
	 *   lva support, 52bit 지원 환경에서는 다음과 같이 달라진다.
	 *
	 *   64k, 52bits -> VA_BITS_MIN: 48, vabits_actual: 52 (lva support)
	 *   -> 52bit, _PAGE_OFFSET: 0xfff0000000000000
	 *      0xffff800000000000 - 0xfff0000000000000 = 0xf800000000000 == 3968TB
	 *
	 *   4k, 48bits 환경에서는 변화가 없지만 64k, 52bit 환경에서는 범위가
	 *   늘어난다.
	 *
	 *   +--------------------+
	 *   | valloc             |
	 *   +--------------------+ <-- _PAGE_END(48) == 0xffff800000000000
	 *   |                    |
	 *   | linear_region_size |     0xf800000000000 = 3968TB
	 *   |                    |
	 *   +--------------------+ <-- _PAGE_OFFSET(52) == 0xfff0000000000000
	 */
	s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);

	/*
	 * Corner case: 52-bit VA capable systems running KVM in nVHE mode may
	 * be limited in their ability to support a linear map that exceeds 51
	 * bits of VA space, depending on the placement of the ID map. Given
	 * that the placement of the ID map may be randomized, let's simply
	 * limit the kernel's linear map to 51 bits as well if we detect this
	 * configuration.
	 */
	if (IS_ENABLED(CONFIG_KVM) && vabits_actual == 52 &&
	    is_hyp_mode_available() && !is_kernel_in_hyp_mode()) {
		pr_info("Capping linear region to 51 bits for KVM in nVHE mode on LVA capable hardware.\n");
		linear_region_size = min_t(u64, linear_region_size, BIT(51));
	}

	/* IAMROOT, 2021.10.23:
	 * - 지원되는 paddr 범위를 벗어나는 region을 memblock에서 제거한다.
	 *
	 *   예) pa 48bits 시스템에서 PHYS_MASK_SHIFT 값은 48.
	 *       0x0001_0000_0000_0000 == 1ULL << 48
	 *       0x0001_0000_0000_0000 .. 0xffff_ffff_ffff_ffff 사이.
	 */
	/* Remove memory above our supported physical address size */
	memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

	/* IAMROOT, 2021.10.23:
	 * - memstart_addr를 round_down 하여 ARM64_MEMSTART_ALIGN 크기만큼
	 *   정렬하고 start addr을 설정한다.
	 *
	 *   4k page size 기준 Arm64는 항상 1GB 단위로 정렬된다.
	 */
	/*
	 * Select a suitable value for the base of physical memory.
	 */
	memstart_addr = round_down(memblock_start_of_DRAM(),
				   ARM64_MEMSTART_ALIGN);

	if ((memblock_end_of_DRAM() - memstart_addr) > linear_region_size)
		pr_warn("Memory doesn't fit in the linear mapping, VA_BITS too small\n");

	/* IAMROOT, 2021.10.23:
	 * - linear region 또는 symbol(_end) 중에 addr가 높은 쪽을 선택하고
	 *   해당 addr가 커버할 수 있는 범위 이상인 region을 memblock에서
	 *   제거한다.
	 */
	/*
	 * Remove the memory that we will not be able to cover with the
	 * linear mapping. Take care not to clip the kernel which may be
	 * high in memory.
	 */
	memblock_remove(max_t(u64, memstart_addr + linear_region_size,
			__pa_symbol(_end)), ULLONG_MAX);

	/* IAMROOT, 2021.11.04:
	 * - DRAM 크기가 linear mapping (128TB) 보다 클 경우 phys 파편화가
	 *   발생할 수 있으므로 memstart_addr = (end_of_DRAM() - lm_size) 보정
	 *   작업을 통해 lm 영역을 DRAM의 high addr 영역으로 보낸다.
	 *
	 *   boot-up시에 dts를 통해 DRAM의 range(start .. end)의 region을
	 *   memblock에 미리 추가한다.
	 *
	 *   memstart_addr을 보정하더라도 '+ linear_region_size'를 통해
	 *   (memstart_addr + linear_region_size) == memblock_end_of_DRAM()임을
	 *   보장할 수 있다.
	 *
	 *   memstart_addr 수정 전
	 *   ---------------------
	 *   memblock_end_of_DRAM()
	 *   |
	 *   |    high addr
	 *   '-> +--------+
	 *       |        |
	 *       |        |
	 *       |        |
	 *       |        |  <-- (memstart_addr + linear_region_size)
	 *            ...
	 *       |        |
	 *       |        |  <-- memstart_addr
	 *
	 *   memstart_addr 수정 후
	 *   ---------------------
	 *   memblock_end_of_DRAM()
	 *   |
	 *   |    high addr
	 *   '-> +--------+  <-+-- (memstart_addr + linear_region_size)
	 *       |        |    |
	 *       |        |    |   새로 보정된 영역 (128TB)
	 *       |        |    |
	 *       |        |  <-+-- memstart_addr (new)  +----
	 *            ...                               | 삭제가 필요한 영역
	 *       |        |                             | memblock_remove() 호출
	 *       |        |  <---- memstart_addr (old)  +----
	 */
	if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
		/* ensure that memstart_addr remains sufficiently aligned */
		memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
					 ARM64_MEMSTART_ALIGN);
		memblock_remove(0, memstart_addr);
	}

	/* IAMROOT, 2021.10.23: TODO
	 * memstart_addr의 주석에 있는 내용과 같은 상황(config의 VA_BITS와
	 * vabits_actual이 다른 상황)에서 와 같이 그냥 memstart_addr을 사용하면
	 * 오차가 발생해버린다. 이 오차를 이 시점에서 보정을 해준다.
	 *
	 * memstart_addr = 0x8000_0000
	 * _PAGE_OFFSET(48) = 0xffff_0000_0000_0000
	 * _PAGE_OFFSET(52) = 0xfff0_0000_0000_0000
	 *
	 * memstart_addr = 0x8000_0000 - 0xf_0000_0000_0000
	 *               = -0xe_ffff_8000_0000
	 *
	 * 해당 주석의 마지막 예제로가면 보정된 결과가 있다.
	 */
	/*
	 * If we are running with a 52-bit kernel VA config on a system that
	 * does not support it, we have to place the available physical
	 * memory in the 48-bit addressable part of the linear region, i.e.,
	 * we have to move it upward. Since memstart_addr represents the
	 * physical address of PAGE_OFFSET, we have to *subtract* from it.
	 */
	if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
		memstart_addr -= _PAGE_OFFSET(48) - _PAGE_OFFSET(52);

	/* IAMROOT, 2021.10.23:
	 * - kernel boottime 또는 dt를 통해 memory_limit이 설정되면 해당 값을
	 *   기준으로 아래 작업을 수행한다.
	 *
	 *   1) memory/reserved region[0 .. memory_limit]을 제외한 region을
	 *      memblock에서 삭제한다. (memory_limit .. PHYS_ADDR_MAX 범위 삭제)
	 *   2) kernel image (_text .. _end) 영역이 memory_limit boundary에
	 *      걸쳐 제거될 가능성이 있으므로 re-add 한다.
	 */
	/*
	 * Apply the memory limit if it was set. Since the kernel may be loaded
	 * high up in memory, add back the kernel region that must be accessible
	 * via the linear mapping.
	 */
	if (memory_limit != PHYS_ADDR_MAX) {
		memblock_mem_limit_remove_map(memory_limit);
		memblock_add(__pa_symbol(_text), (u64)(_end - _text));
	}

	/* IAMROOT, 2021.10.23:
	 * - initrd을 위해 해당 paddr을 memory/reserved region에 추가한다.
	 */
	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/*
		 * Add back the memory we just removed if it results in the
		 * initrd to become inaccessible via the linear mapping.
		 * Otherwise, this is a no-op
		 */
		u64 base = phys_initrd_start & PAGE_MASK;
		u64 size = PAGE_ALIGN(phys_initrd_start + phys_initrd_size) - base;

		/*
		 * We can only add back the initrd memory if we don't end up
		 * with more memory than we can address via the linear mapping.
		 * It is up to the bootloader to position the kernel and the
		 * initrd reasonably close to each other (i.e., within 32 GB of
		 * each other) so that all granule/#levels combinations can
		 * always access both.
		 */
		if (WARN(base < memblock_start_of_DRAM() ||
			 base + size > memblock_start_of_DRAM() +
				       linear_region_size,
			"initrd not fully accessible via the linear mapping -- please check your bootloader ...\n")) {
			phys_initrd_size = 0;
		} else {
			/* IAMROOT, 2021.10.23:
			 * - region[base .. size]의 flag를 clear 하기 위해 우선 삭제한다.
			 */
			memblock_remove(base, size); /* clear MEMBLOCK_ flags */
			memblock_add(base, size);
			memblock_reserve(base, size);
		}
	}

	/* IAMROOT, 2021.10.23: TODO
	 * - CONFIG_RANDOMIZE_BASE == enable 라면 memstart_addr를 보정한다.
	 */
	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		extern u16 memstart_offset_seed;
		u64 mmfr0 = read_cpuid(ID_AA64MMFR0_EL1);
		int parange = cpuid_feature_extract_unsigned_field(
					mmfr0, ID_AA64MMFR0_PARANGE_SHIFT);
		s64 range = linear_region_size -
			    BIT(id_aa64mmfr0_parange_to_phys_shift(parange));

		/*
		 * If the size of the linear region exceeds, by a sufficient
		 * margin, the size of the region that the physical memory can
		 * span, randomize the linear region as well.
		 */
		if (memstart_offset_seed > 0 && range >= (s64)ARM64_MEMSTART_ALIGN) {
			range /= ARM64_MEMSTART_ALIGN;
			/* IAMROOT, 2021.10.27: TODO
			 * - memstart_offset_seed는 seed 상위 16bit를 사용했었다.
			 *   즉 memstart_offset_seed는 16bit 이하의 값인데 여기에 range를
			 *   곱해서 memstart_offset_seed의 범위인 16 bit를 넘는 값만을
			 *   사용해서 마진을 구할려고 다음과 같은 식들을 사용한거 같다.
			 */
			memstart_addr -= ARM64_MEMSTART_ALIGN *
					 ((range * memstart_offset_seed) >> 16);
		}
	}

	/* IAMROOT, 2021.10.23:
	 * - kernel의 ".text" .. ".end" 까지의 영역을 reserved region에 추가한다.
	 *   여기에는 kernel text, data, initrd, initial pagetables도 포함된다.
	 */
	/*
	 * Register the kernel text, kernel data, initrd, and initial
	 * pagetables with memblock.
	 */
	memblock_reserve(__pa_symbol(_stext), _end - _stext);
	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/* the generic initrd code expects virtual addresses */
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	}

	/* IAMROOT, 2021.10.23:
	 * - dts에서 지정한 memreserve, reserved-memory node 들을 NOMAP으로
	 *   marking 하거나 reserved region에 추가한다.
	 */
	early_init_fdt_scan_reserved_mem();

	/* IAMROOT, 2021.10.23:
	 * - vaddr 사용시 range를 벗어나면 안되므로 아래처럼 계산하여 저장한다.
	 */
	high_memory = __va(memblock_end_of_DRAM() - 1) + 1;
}

/* IAMROOT, 2021.11.06: TODO
 * - bootmem_init(..)이 어떤 역할을 수행하는지 서술.
 */
void __init bootmem_init(void)
{
	unsigned long min, max;

	/* IAMROOT, 2024.03.25:
	 * - memblock에 저장된 DRAM의 pa(start), pa(end)를 기반으로
	 *   아래 식을 통해 min, max 경계의 PFN을 구한다.
	 *
	 * - 예) range: 0x8000_0000 ~ 0xffff_ffff (2GB)
	 *
	 *   memblock_start_of_DRAM: 0x0_8000_0000
	 *   memblock_end_of_DRAM  : 0x1_0000_0000
	 *   min: 0x08_0000
	 *   max: 0x10_0000
	 */
	min = PFN_UP(memblock_start_of_DRAM());
	max = PFN_DOWN(memblock_end_of_DRAM());

	early_memtest(min << PAGE_SHIFT, max << PAGE_SHIFT);

	max_pfn = max_low_pfn = max;
	min_low_pfn = min;

	arch_numa_init();

	/*
	 * must be done after arch_numa_init() which calls numa_init() to
	 * initialize node_online_map that gets used in hugetlb_cma_reserve()
	 * while allocating required CMA size across online nodes.
	 */
#if defined(CONFIG_HUGETLB_PAGE) && defined(CONFIG_CMA)
	arm64_hugetlb_cma_reserve();
#endif

	dma_pernuma_cma_reserve();

	kvm_hyp_reserve();

	/*
	 * sparse_init() tries to allocate memory from memblock, so must be
	 * done after the fixed reservations
	 */
	/* IAMROOT, 2024.07.27:
	 * - phys memory 관리를 위해 sparse 방식으로 초기화한다.
	 */
	sparse_init();

	/* IAMROOT, 2024.07.28:
	 * - 모든 node, 모든 zone, 및 필요시 struct page를 초기화한다.
	 */
	zone_sizes_init(min, max);

	/*
	 * Reserve the CMA area after arm64_dma_phys_limit was initialised.
	 */
	dma_contiguous_reserve(arm64_dma_phys_limit);

	/*
	 * request_standard_resources() depends on crashkernel's memory being
	 * reserved, so do it here.
	 */
	reserve_crashkernel();

	memblock_dump_all();
}

/*
 * mem_init() marks the free areas in the mem_map and tells us how much memory
 * is free.  This is done after various parts of the system have claimed their
 * memory after the kernel image.
 */
void __init mem_init(void)
{
/*
 * IAMROOT, 2022.02.19:
 * - swiotlb_force 위치 : kernel/dma/swiotlb.c
 *   default는 SWIOTLB_NORMAL. early param으로 SWIOTLB_FORCE등이 설정된다.
 * - 특정 device의 dma size에 memory보다 작을 경우 swiotlb_init이 될수있다.
 *
 * --- swiotlb(software io tlb, 혹은 iommu)
 * - 보통 device는 연속된 memory를 요구한다. 하지만 kernel은 연속된 memory를
 *   할당하는데 제한이 있어 보통 조각난 memory를 한대 묶어서 iommu에 mmapping을
 *   요청해 마치 연속된 memory인것처럼 device가 인식하도록 한다.
 * - 하지만 만약 device가 dma limit이 걸려 있어 특정 address이상을 사용하지
 *   못할 경우 위 방법을 하기가 불가능한데, 이 경우 bounce buffer를 둬서
 *   device가 해당 bounce buffer에 접근하게 하고 bound buffer의 값을 직접
 *   main memory에 cpu가 옮기는 방식으로 사용한다.
 */
	if (swiotlb_force == SWIOTLB_FORCE ||
	    max_pfn > PFN_DOWN(arm64_dma_phys_limit))
		swiotlb_init(1);
	else if (!xen_swiotlb_detect())
		swiotlb_force = SWIOTLB_NO_FORCE;

	set_max_mapnr(max_pfn - PHYS_PFN_OFFSET);

/*
 * IAMROOT, 2022.02.26: 
 * reserved meblock을 제외한 free 영역들을 모두 buddy 시스템으로 옮긴다.
 */
	/* this will put all unused low memory onto the freelists */
	memblock_free_all();

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can be
	 * detected at build time already.
	 */
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(TASK_SIZE_32 > DEFAULT_MAP_WINDOW_64);
#endif

	/*
	 * Selected page table levels should match when derived from
	 * scratch using the virtual address range and page size.
	 */
	BUILD_BUG_ON(ARM64_HW_PGTABLE_LEVELS(CONFIG_ARM64_VA_BITS) !=
		     CONFIG_PGTABLE_LEVELS);

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get anywhere without
		 * overcommit, so turn it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
	free_reserved_area(lm_alias(__init_begin),
			   lm_alias(__init_end),
			   POISON_FREE_INITMEM, "unused kernel");
	/*
	 * Unmap the __init region but leave the VM area in place. This
	 * prevents the region from being reused for kernel modules, which
	 * is not supported by kallsyms.
	 */
	vunmap_range((u64)__init_begin, (u64)__init_end);
}

void dump_mem_limit(void)
{
	if (memory_limit != PHYS_ADDR_MAX) {
		pr_emerg("Memory Limit: %llu MB\n", memory_limit >> 20);
	} else {
		pr_emerg("Memory Limit: none\n");
	}
}
