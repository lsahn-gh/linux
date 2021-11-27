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
 * - arm64_memblock_init에서 초기화된다. 이 값이 초기화 되기전엔
 *   리니어 매핑이 완료되지 않은 상태이므로
 *   phys_to_virt등의 변환 매크로를 사용할수 없다.
 *
 *   해당값이 초기화할때 VA_BITS, vabits_actual에 따라서 보정을 해주는데
 *   다음과 같은 예제 사유를 따른다.
 *   
 * ex) VA_BITS = 48, vabits_actual = 48, 
 *     PAGE_SIZE = 4k, memstart = 0x8000_0000
 *
 *	PAGE_OFFSET = 0xffff_0000_0000_0000
 *
 *	phys_addr = 0x9000_0000
 *	
 *	__phys_to_virt(x) = (((x) - PHYS_OFFSET) | PAGE_OFFSET)
 *	__phys_to_virt(0x9000_0000) = 0x9000_0000 - 0x8000_0000 |
 *				      0xffff_0000_0000_0000
 *				    = 0xffff_0000_1000_0000
 *
 * ex) VA_BITS = 52,  vabits_actual = 48,
 *     PAGE_SIZE = 16, memstart = 0x8000_0000 (틀린사례)
 *				    
 *	PAGE_OFFSET = 0xfff0_0000_0000_0000
 *
 *	phys_addr = 0x9000_0000
 *	
 *	__phys_to_virt(x) = (((x) - PHYS_OFFSET) | PAGE_OFFSET)
 *	__phys_to_virt(0x9000_0000) = 0x9000_0000 - 0x8000_0000 |
 *				      0xfff0_0000_0000_0000
 *				    = 0xfff0_0000_1000_0000
 *
 * ex) VA_BITS = 52,  vabits_actual = 48,
 *     PAGE_SIZE = 16, memstart = -0xe_ffff_8000_0000 (memstart가 보정된 사례)
 *				    
 *	PAGE_OFFSET = 0xfff0_0000_0000_0000
 *
 *	phys_addr = 0x9000_0000
 *	
 *	__phys_to_virt(x) = (((x) - PHYS_OFFSET) | PAGE_OFFSET)
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
/*
 * IAMROOT, 2021.10.23:
 * - arm64_memblock_init에서 ARM64_ZONE_DMA_BITS 와 ram 크기를
 *   비교해 작은 값으로 초기화 된다.
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
/*
 * IAMROOT, 2021.10.23:
 * - crash dump 용으로 작은 kernel을 올릴 영역을 reserve 한다.
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
/*
 * IAMROOT, 2021.10.23:
 * - ram 크기 or zone_bits 이내로 범위를 조정한다.
 */
static phys_addr_t __init max_zone_phys(unsigned int zone_bits)
{
	phys_addr_t zone_mask = DMA_BIT_MASK(zone_bits);
	phys_addr_t phys_start = memblock_start_of_DRAM();
/*
 * IAMROOT, 2021.11.27:
 * - DRAM start 주소가 32bit max보다 크면 memblock_end_of_DRAM()을 쓰겟다는것.
 * - DRAM start 주소가 zone_mask보다 크면 min(U32_MAX, memblock_end_of_DRAM())
 */
	if (phys_start > U32_MAX)
		zone_mask = PHYS_ADDR_MAX;
	else if (phys_start > zone_mask)
		zone_mask = U32_MAX;

	return min(zone_mask, memblock_end_of_DRAM() - 1) + 1;
}

/*
 * IAMROOT, 2021.11.27:
 * @min DRAM start pfn
 * @max DRAM end pfn + 1
 *
 * ZONE_DMA    : dt에서 읽은 device max address pfn
 * ZONE_DMA32  : 32bit pfn
 * ZONE_NORMAL : DRAM end pfn + 1
 *
 * arm64에서는 보통 ZONE_DMA가 없고 ZONE_DMA32를 쓴다.
 */
static void __init zone_sizes_init(unsigned long min, unsigned long max)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES]  = {0};
	unsigned int __maybe_unused acpi_zone_dma_bits;
	unsigned int __maybe_unused dt_zone_dma_bits;
	phys_addr_t __maybe_unused dma32_phys_limit = max_zone_phys(32);

#ifdef CONFIG_ZONE_DMA
/*
 * IAMROOT, 2021.11.27:
 * - acpi가 disable일경우 fls64는 64.
 */
	acpi_zone_dma_bits = fls64(acpi_iort_dma_get_max_cpu_address());
	dt_zone_dma_bits = fls64(of_dma_get_max_cpu_address(NULL));
	zone_dma_bits = min3(32U, dt_zone_dma_bits, acpi_zone_dma_bits);
	arm64_dma_phys_limit = max_zone_phys(zone_dma_bits);
	max_zone_pfns[ZONE_DMA] = PFN_DOWN(arm64_dma_phys_limit);
#endif
#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(dma32_phys_limit);
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = dma32_phys_limit;
#endif
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = PHYS_MASK + 1;
	max_zone_pfns[ZONE_NORMAL] = max;

	free_area_init(max_zone_pfns);
}

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
	if (PHYS_PFN(addr) != pfn)
		return 0;

	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;

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
	if (!early_section(ms))
		return pfn_section_valid(ms, pfn);

	return memblock_is_memory(addr);
}
EXPORT_SYMBOL(pfn_valid);

int pfn_is_map_memory(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);

	/* avoid false positives for bogus PFNs, see comment in pfn_valid() */
	if (PHYS_PFN(addr) != pfn)
		return 0;

	return memblock_is_map_memory(addr);
}
EXPORT_SYMBOL(pfn_is_map_memory);

/*
 * IAMROOT, 2021.10.23:
 * - command line이나 dt에서 해당값을 초기화 할수 있다.
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
/*
 * IAMROOT, 2021.10.23:
 * - kernel 영역에서 절반은 lm 영역이므로 lm 영역 사이즈를 구한다. (128TB)
 *
 * - VA_BITS 48 환경
 *     vabits_actual = 48
 *     PAGE_END      = 0xffff800000000000
 *     _PAGE_OFFSET  = 0xffff000000000000
 *     128TB = PAGE_END - _PAGE_OFFSET = 0x800000000000
 *
 * ----- 5.10 -> 5.15 변경점에 대하여 (KASAN 고려 안함)
 *  5.10 코드 : linear_region_size = BIT(vabits_actual - 1);
 *
 *  - 4k, 48bits -> VA_BITS_MIN=48, vabits_actual=48
 *	 5.10 : BIT(47) = 0x800000000000 = 128TB
 *	 5.15 : 0xffff800000000000 - 0xffff000000000000 = 0x800000000000 = 128TB
 *
 * 위와 같은 상황에서는 기존과 변화가 없지만 lva support하는 52bit 환경에서는
 * 다음과 같이 달라진다.
 *
 * 52bit _PAGE_OFFSET = 0xfff0000000000000
 *  - 64k, 52bits -> VA_BITS_MIN=48, vabits_actual=52(lva support)
 *	 5.10 : BIT(51) = 0x8000000000000 = 2048TB
 *	 5.15 : 0xffff800000000000 - 0xfff0000000000000 = 0xf800000000000 = 3968TB
 *
 * 즉 4k, 48bits에서 변한건 없지만 64k, 52bits에서는 범위가 늘어난것이 확인된다.
 *
 * 기존 BIT만을 사용해 영역을 절반사용했을때에는 linear 영역이
 *  PAGE_OFFSET(52) ~ (PAGE_OFFSET(52) + BIT(51))인 2048TB 영역을 사용했었는데
 *  PAGE_END가 실제론 VA_BITS_MIN을 쓰는 _PAGE_END(48)이다 보니
 *  (PAGE_OFFSET(52) + BIT(51) ~ _PAGE_END(48)) 영역을 안쓰게 되는 되어 버렸다.
 *
 * (PAGE_OFFSET(52) + BIT(51) = _PAGE_END(52) 로 표현된다)
 *
 * ==== 5.10 에서 vabits_actual=52 linear_region 영역 ====
 *
 *   +--------------------+
 *   | valloc             |
 *   +--------------------+ _PAGE_END(48) = 0xffff800000000000
 *   + 사용안함           |
 *   +--------------------+ _PAGE_OFFSET(52) + BIT(51) =  0xfff8000000000000
 *   |                    |
 *   | linear_region_size | BIT(51) = 2048TB = 0x800000000000
 *   | (BIT(51) size)     |
 *   |                    |
 *   +--------------------+ _PAGE_OFFSET(52) = 0xfff0000000000000
 *
 * 그러다 보니 이 "사용안함" 영역도 linear 영역에 포함 시킬려고 현재와 같이
 * 수정이 된것이다.
 *
 * ==== 5.15 에서 vabits_actual=52 linear_region 영역 ====
 *
 *   +--------------------+
 *   | valloc             |
 *   +--------------------+ _PAGE_END(48) = 0xffff800000000000
 *   |                    |
 *   | linear_region_size | 0xf800000000000 = 3968TB
 *   |                    |
 *   +--------------------+ _PAGE_OFFSET(52) = 0xfff0000000000000
 *
 * 하지만 현재 저 "사용안함" 이였던 영역은 실제 매핑하지 않아 보이므로
 * linear_region_size는 2048TB영역이라 봐도 무방하다.
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

/*
 * IAMROOT, 2021.10.23:
 * - 물리주소 범위를 벗어나는 영역을 지운다
 *
 *   예) pa 48bits 시스템에서 PHYS_MASK_SHIFT: 48
 *       1ULL << 48 = 0x1_0000_0000_0000
 *       0x1_0000_0000_0000 ~ 0xffff_ffff_ffff_ffff 사이
 */
	/* Remove memory above our supported physical address size */
	memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

	/*
	 * Select a suitable value for the base of physical memory.
	 */
/*
 * IAMROOT, 2021.10.23:
 * - memstart_addr를 round_down 하여 1GB 크기만큼 정렬하고 재설정한다.
 *   ARM64에서 ARM64_MEMSTART_ALIGN은 항상 1GB 크기이다.
 */
	memstart_addr = round_down(memblock_start_of_DRAM(),
				   ARM64_MEMSTART_ALIGN);

	if ((memblock_end_of_DRAM() - memstart_addr) > linear_region_size)
		pr_warn("Memory doesn't fit in the linear mapping, VA_BITS too small\n");

	/*
	 * Remove the memory that we will not be able to cover with the
	 * linear mapping. Take care not to clip the kernel which may be
	 * high in memory.
	 */
/*
 * IAMROOT, 2021.10.23:
 * - lm 영역보다 큰 영역, 또는 pa(_end)가 higher인 영역을 base로 하여 그보다
 *   상위인 영역을 memblock_remove로 제거한다.
 */
	memblock_remove(max_t(u64, memstart_addr + linear_region_size,
			__pa_symbol(_end)), ULLONG_MAX);
/*
 * IAMROOT, 2021.11.04:
 * - 처음에 dts를 통해 memblock_start_of_DRAM() - memblock_end_of_DRAM()까지
 *   memory region을 추가하였을 것이다.
 *
 * - 'memstart_addr + linear_region_size' < memblock_end_of_DRAM() 라면 
 *   ( memblock_end_of_DRAM() > 128TB )
 *   memstart_addr을 보정하고 '(new) memstart_addr - 0' 사이의 영역은 기존에
 *   추가되어 있던 region이므로 memblock_remove로 삭제한다.
 *
 *   memstart_addr을 보정하더라도 '+ linear_region_size'를 통해
 *   size(memstart_addr + linear_region_size) == memblock_end_of_DRAM()임을
 *   보장할 수 있다.
 *
 * memstart_addr 수정 전
 * ---------------------
 * memblock_end_of_DRAM()
 * |
 * '-> +--------+ 
 *     |        |
 *     |        |
 *     |        |  <-- size (memstart_addr + linear_region_size)
 *          ...
 *     |        |
 *     |        |  <-- memstart_addr
 *
 * memstart_addr 수정 후
 * ---------------------
 * memblock_end_of_DRAM()
 * |
 * '-> +--------+  <-- size (memstart_addr + linear_region_size)
 *     |        |
 *     |        |
 *     |        |  <-- memstart_addr (new) -+----
 *          ...                             | 보정이 필요한 영역
 *     |        |                           | memblock_remove() 호출
 *     |        |  <-- memstart_addr (old) -+----
 *
 */
	if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
		/* ensure that memstart_addr remains sufficiently aligned */
		memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
					 ARM64_MEMSTART_ALIGN);
		memblock_remove(0, memstart_addr);
	}

	/*
	 * If we are running with a 52-bit kernel VA config on a system that
	 * does not support it, we have to place the available physical
	 * memory in the 48-bit addressable part of the linear region, i.e.,
	 * we have to move it upward. Since memstart_addr represents the
	 * physical address of PAGE_OFFSET, we have to *subtract* from it.
	 */
/*
 * IAMROOT, 2021.10.23:
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
	if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
		memstart_addr -= _PAGE_OFFSET(48) - _PAGE_OFFSET(52);

	/*
	 * Apply the memory limit if it was set. Since the kernel may be loaded
	 * high up in memory, add back the kernel region that must be accessible
	 * via the linear mapping.
	 */
/*
 * IAMROOT, 2021.10.23:
 * - 외부에서 memory_limis값이 지정이되면 해당값을 기준으로 초기화가 이루어진다.
 *   memory_limit ~ PHYS_ADDR_MAX를 전부 remove 한다.
 *
 * - kernel이 dram의 위쪽에 load가 될수있는데 이 때 kernel 영역이
 *   memory_limit 을 걸치고있거나 위에 있을 수 있어 kernel image 영역이
 *   제거될수있으므로 다시 add를 해준다.
 */
	if (memory_limit != PHYS_ADDR_MAX) {
		memblock_mem_limit_remove_map(memory_limit);
		memblock_add(__pa_symbol(_text), (u64)(_end - _text));
	}

/*
 * IAMROOT, 2021.10.23:
 * - initrd 영역을 reserved한다.
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
/*
 * IAMROOT, 2021.10.23:
 * - 해당영역이 flag가 존재할수있으므로 그냥 remove를 한다.
 */
			memblock_remove(base, size); /* clear MEMBLOCK_ flags */
			memblock_add(base, size);
			memblock_reserve(base, size);
		}
	}

/*
 * IAMROOT, 2021.10.23:
 * - CONFIG_RANDOMIZE_BASE가 적용되있으면 다시한번 memstart_addr를 고쳐준다.
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
/*
 * IAMROOT, 2021.10.27:
 * - memstart_offset_seed는 seed 상위 16bit를 사용했었다.
 *   즉 memstart_offset_seed는 16bit 이하의 값인데 여기에 range를 곱해서
 *   memstart_offset_seed의 범위인 16 bit를 넘는 값만을 사용해서 마진을 구할려고
 *   다음과 같은 식들을 사용한거 같다.
 */
			memstart_addr -= ARM64_MEMSTART_ALIGN *
					 ((range * memstart_offset_seed) >> 16);
		}
	}

	/*
	 * Register the kernel text, kernel data, initrd, and initial
	 * pagetables with memblock.
	 */
/*
 * IAMROOT, 2021.10.23:
 * - kernel 영역을 reserve
 */
	memblock_reserve(__pa_symbol(_stext), _end - _stext);
	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/* the generic initrd code expects virtual addresses */
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	}
/*
 * IAMROOT, 2021.10.23:
 * - dt에서 지정한 reserved 영역 등록
 */
	early_init_fdt_scan_reserved_mem();

/*
 * IAMROOT, 2021.10.23:
 * - va를 사용시 범위를 벗어나면 안되서 -1을 해줬다가 결과 가상주소에 + 1을 한다.
 */
	high_memory = __va(memblock_end_of_DRAM() - 1) + 1;
}

/*
 * IAMROOT, 2021.11.06:
 * - ex) 0x8000_0000 ~ 0xffff_ffff(2GB)
 *   memblock_start_of_DRAM = 0x8000_0000
 *   memblock_end_of_DRAM = 0x1_0000_0000
 *   min =  0x8_0000
 *   max = 0x10_0000
 */
void __init bootmem_init(void)
{
	unsigned long min, max;

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
	sparse_init();
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
	if (swiotlb_force == SWIOTLB_FORCE ||
	    max_pfn > PFN_DOWN(arm64_dma_phys_limit))
		swiotlb_init(1);
	else if (!xen_swiotlb_detect())
		swiotlb_force = SWIOTLB_NO_FORCE;

	set_max_mapnr(max_pfn - PHYS_PFN_OFFSET);

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
