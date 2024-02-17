// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/setup.c
 *
 * Copyright (C) 1995-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/cache.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/panic_notifier.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/efi.h>
#include <linux/psci.h>
#include <linux/sched/task.h>
#include <linux/mm.h>

#include <asm/acpi.h>
#include <asm/fixmap.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/elf.h>
#include <asm/cpufeature.h>
#include <asm/cpu_ops.h>
#include <asm/kasan.h>
#include <asm/numa.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/efi.h>
#include <asm/xen/hypervisor.h>
#include <asm/mmu_context.h>

static int num_standard_resources;
static struct resource *standard_resources;

/* IAMROOT, 2021.09.04:
 * - mmu enable 이후에 __primary_switched 에서 변수 초기화가 이루어진다.
 *
 *   fdt: flattened device tree
 *   __initdata : include/linux/init.h
 */
phys_addr_t __fdt_pointer __initdata;

/*
 * Standard memory resources
 */
static struct resource mem_res[] = {
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	}
};

#define kernel_code mem_res[0]
#define kernel_data mem_res[1]

/*
 * The recorded values of x0 .. x3 upon kernel entry.
 */
u64 __cacheline_aligned boot_args[4];

void __init smp_setup_processor_id(void)
{
/*
 * IAMROOT, 2021.09.11:
 * - cpu 0번이면 mpidr 0인경우가 많지만 아닌 경우도 있다.
 * - MPIDR_HWID_BITMASK: MPIDR_EL1에서 {Aff3, Aff2, Aff1, Aff0}만 빼온다.
 * - 각 PE의 {Aff3, Aff2, Aff1, Aff0} 값은 unique하다.
 */
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	set_cpu_logical_map(0, mpidr);

	pr_info("Booting Linux on physical CPU 0x%010lx [0x%08x]\n",
		(unsigned long)mpidr, read_cpuid_id());
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

struct mpidr_hash mpidr_hash;
/**
 * smp_build_mpidr_hash - Pre-compute shifts required at each affinity
 *			  level in order to build a linear index from an
 *			  MPIDR value. Resulting algorithm is a collision
 *			  free hash carried out through shifting and ORing
 */

/*
 * IAMROOT, 2022.01.01: 
 * MPIDR 값을 읽어서 affinity level 별로 필요한 shift 값들을 저장해두고, 
 * 추후 cpu_suspend() 및 cpu_resume() 내부의 어셈블리 코드에서 사용한다.
 *
 * - cpu_resume()등의 함수에서 cpu mpidr로 hash table에 접근한다.
 *   즉 cpu mpidr로 key값을 만드는데, 모든 cpu의 mpidr에 대해서 중복되는
 *   key값이 안생기면서 최소한의 hash table을 만들어야되는데,
 *   이 함수에서는 mpdir로 key값을 만들때 필요한 shift값들과
 *   hash table에 필요한 bits를 계산한다.
 *   이때 affinity level이라는 개념을 도입해서 각 level별로 범위를
 *   나누고 첫 3bit를 
 * 
 * - mpidr 로 hash의 범위를 정하는 과정.
 *
 *   1) 0번 cpu를 제외한 모든 cpu를 0번 cpu와 xor하고 모두 or 시킨다.
 *
 *   2) affinity level이라는 개념을 도입한다. level은 0 ~ 3까지 존재하며
 *   각 범위는 다음과 같다.
 *	level | 0     | 1      | 2       | 3       |
 *	bits  | 0 ~ 2 | 8 ~ 10 | 16 ~ 18 | 32 ~ 35 |
 *   (해당 범위의 첫 3bit만 사용한다.)
 *
 *   3) 각각의 affinity level 범위에서 fisrt bit 번호를 구한다.
 *   4) 각각의 affinity level 범위에서
 *   last bit + 1 - fisrt bit 번호를 구한다.
 *   5) 각 affinity level에서 사용할 shift값을 3), 4)에서 구한값으로
 *   계산한다.
 */
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity, fs[4], bits[4], ls;
	u64 mask = 0;
	/*
	 * Pre-scan the list of MPIDRS and filter out bits that do
	 * not contribute to affinity levels, ie they never toggle.
	 */
/*
 * IAMROOT, 2022.01.02:
 * - boot cpu의 mpidr과 나머지 cpu들의 mpidr을 xor 시킨 값들을 mask에
 *   전부 or 시켜 저장한다.
 * ex) cpu 0 mpdir : 0x1, cpu 1 mpdir :0x2
 *     mask = 0x3
 * ex) cpu 0 mpdir : 0xf01, cpu 1 mpdir :0xf02
 *     mask = 0x3
 */
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits %#llx\n", mask);
	/*
	 * Find and stash the last and first bit set at all affinity levels to
	 * check how many bits are required to represent them.
	 */
	for (i = 0; i < 4; i++) {
/*
 * IAMROOT, 2022.01.02:
 * 각 level별로 mpidr의 bits를 다음과 같이 가져온다.
 * level | 0     | 1      | 2       | 3       |
 * bits  | 0 ~ 2 | 8 ~ 10 | 16 ~ 18 | 32 ~ 35 |
 */
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		/*
		 * Find the MSB bit and LSB bits position
		 * to determine how many bits are required
		 * to express the affinity level.
		 */
		ls = fls(affinity);
/*
 * IAMROOT, 2022.01.02:
 * - bit가 존재한다면 first bit number를 저장한다.
 */
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
/*
 * IAMROOT, 2022.01.02:
 * - last bit 번호와 first bit 번호의 차이를 저장한다.
 *   affinity | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7
 *   ls       | 0 | 1 | 2 | 2 | 4 | 4 | 4 | 4
 *   ffs      | 0 | 1 | 2 | 1 | 4 | 1 | 2 | 1
 *   fs       | 0 | 0 | 1 | 0 | 3 | 0 | 1 | 0
 *   bits     | 0 | 1 | 1 | 2 | 1 | 4 | 3 | 4
 *
 * - bits
 *   ls - fs로 그 사이에 bit가 몇개 필요한지 적당히 계산한다.
 *   affinity가 1, 2, 4이라면 bit가 1개만 존재할것이므로 1
 *   affinity가 3이라면 bit가 2개 존재할것이므로 2로 정확하게 되지만
 *   affinity가 5, 6, 7인 경우엔 조금 많게 잡히는것이 보인다.
 */
		bits[i] = ls - fs[i];
	}
	/*
	 * An index can be created from the MPIDR_EL1 by isolating the
	 * significant bits at each affinity level and by shifting
	 * them in order to compress the 32 bits values space to a
	 * compressed set of values. This is equivalent to hashing
	 * the MPIDR_EL1 through shifting and ORing. It is a collision free
	 * hash though not minimal since some levels might contain a number
	 * of CPUs that is not an exact power of 2 and their bit
	 * representation might contain holes, eg MPIDR_EL1[7:0] = {0x2, 0x80}.
	 */
/*
 * IAMROOT, 2022.01.02:
 * level                     | 0 | 1 | 2  | 3  |
 * --------------------------+---+---+----+----+
 * MPIDR_LEVEL_SHIFT(level)  | 0 | 8 | 16 | 32 |
 *
 * ex) mask = 0x050404 라고 가정한다.
 * affinity level별 mask 값은 다음과 같을것이다.
 *
 * level    | 3 |  2    | 1     | 0
 * affinity | 0 | 0b101 | 0b100 | 0b100 |
 *
 * 여기서 사용할것은 결국 1로 set된 bit들일 것이다.
 * level 0
 * affinity 값이 0b100인데 2번 bit만을 사용하는거나 다름없으므로 이것을
 * 0번으로 움직인다.
 * affinityt level 0 : 0b100 -> 0b01
 *
 * level 1
 * affinity 값이 0b100인데 2번 bit만을 사용하는거나 다름없으므로 이것을
 * 0번으로 움직인다.
 * affinityt level 1 : 0b100 -> 0b01
 * 그런데 level0에서 bit를 1개 쓰고 있으므로 이 칸을 고려해준다.
 * affinityt level 1 : 0b100 -> 0b01 -> 0b10
 *
 * level 2
 * affinity 값이 0b101인데 이 경우는 그대로 사용한다.
 * 그런데 level0, 1에서 bit를 2개 쓰고 있으므로 이 칸을 고려해준다.
 * affinityt level 2 : 0b101 -> 0b10100
 * 
 * affinity level 1, 2, 3을 전부 or시키면 0b10111 이라는 범위가 나올것이고
 * 이 값으로 mpidr를 모두 표현할수있게 된다.
 *
 * - 즉 first bit번호를 만큼은 사용안하는 bit이므로 제거(압축)하는
 *   개념으로 사용한다.
 * - fist bit ~ last bit 번호 차이를 각 affinity level에 필요한
 *   bit수로 계산하여 bit를 남겨놓는 개념으로 사용한다.
 * - 각 affinity level의 first bit와 bits로 각 affinity 범위의
 *   shift 개수를 구해놓는다.
 * - 구해놓은 shift로 나중에 mpidr로 key값을 만들때 사용한다.
 *   Git blame 참고
 * u32 hash(u64 mpidr_el1) {
 *	u32 l[4];
 *	u64 mpidr_el1_masked = mpidr_el1 & mpidr_el1_mask;
 *	l[0] = mpidr_el1_masked & 0xff;
 *	l[1] = mpidr_el1_masked & 0xff00;
 *	l[2] = mpidr_el1_masked & 0xff0000;
 *	l[3] = mpidr_el1_masked & 0xff00000000;
 *	return (l[0] >> aff0_shift | l[1] >> aff1_shift |
 *	l[2] >> aff2_shift | l[3] >> aff3_shift);
 * }
 *
 * - 즉 masking을 8bit의 범위로 나누고 각 범위를 몇 bit shift시켜
 *   hash의 key값을 만들기 위한 각 범위별 shift bit 개수를 계산해낸다.
 *
 */
	mpidr_hash.shift_aff[0] = MPIDR_LEVEL_SHIFT(0) + fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_SHIFT(1) + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = MPIDR_LEVEL_SHIFT(2) + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.shift_aff[3] = MPIDR_LEVEL_SHIFT(3) +
				  fs[3] - (bits[2] + bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[3] + bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] aff3[%u] mask[%#llx] bits[%u]\n",
		mpidr_hash.shift_aff[0],
		mpidr_hash.shift_aff[1],
		mpidr_hash.shift_aff[2],
		mpidr_hash.shift_aff[3],
		mpidr_hash.mask,
		mpidr_hash.bits);
	/*
	 * 4x is an arbitrary value used to warn on a hash table much bigger
	 * than expected on most systems.
	 */
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");
}

static void *early_fdt_ptr __initdata;

void __init *get_early_fdt_ptr(void)
{
	return early_fdt_ptr;
}

asmlinkage void __init early_fdt_map(u64 dt_phys)
{
	int fdt_size;

/* IAMROOT, 2023.11.18:
 * - fdt 매핑 이전에 fixmap을 초기화하여 임시 page table을 생성한다.
 *   현재는 dynamic mapping subsystem이 활성화되기 전이라 fixmap을 이용한다.
 */
	early_fixmap_init();
	early_fdt_ptr = fixmap_remap_fdt(dt_phys, &fdt_size, PAGE_KERNEL);
}

/* IAMROOT, 2021.10.09:
 * - FDT를 fixmap에 매핑한 후 스캔하여 몇개의 주요 정보를 알아온다.
 */
static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
	int size;
	void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
	const char *name;

	/* IAMROOT, 2024.01.20:
	 * - fdt를 fixmap mapping에 성공하면 memblock reserved region에도
	 *   추가한다.
	 */
	if (dt_virt)
		memblock_reserve(dt_phys, size);

	/* IAMROOT, 2021.10.14:
	 * - dt_virt가 null이거나 early_init_dt_scan(..)이 실패하면
	 *   cpu_relax(..) 호출하며 inf-loop를 수행한다.
	 */
	if (!dt_virt || !early_init_dt_scan(dt_virt)) {
		pr_crit("\n"
			"Error: invalid device tree blob at physical address %pa (virtual address 0x%p)\n"
			"The dtb must be 8-byte aligned and must not exceed 2 MB in size\n"
			"\nPlease check your bootloader.",
			&dt_phys, dt_virt);

		while (true)
			cpu_relax();
	}
	/* IAMROOT, 2024.01.20:
	 * - dt에서 물리 메모리 정보를 읽어 memblock에 추가한 상태.
	 */

	/* IAMROOT, 2021.10.16:
	 * - fdt fixmap의 page table attr만 RO로 변경한다.
	 */
	/* Early fixups are done, map the FDT as read-only now */
	fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);

	name = of_flat_dt_get_machine_name();
	if (!name)
		return;

	pr_info("Machine model: %s\n", name);
	dump_stack_set_arch_desc("%s (DT)", name);
}

/*
 * IAMROOT, 2021.12.18:
 * - PASS
 */
static void __init request_standard_resources(void)
{
	struct memblock_region *region;
	struct resource *res;
	unsigned long i = 0;
	size_t res_size;

	kernel_code.start   = __pa_symbol(_stext);
	kernel_code.end     = __pa_symbol(__init_begin - 1);
	kernel_data.start   = __pa_symbol(_sdata);
	kernel_data.end     = __pa_symbol(_end - 1);

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc(res_size, SMP_CACHE_BYTES);
	if (!standard_resources)
		panic("%s: Failed to allocate %zu bytes\n", __func__, res_size);

	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (memblock_is_nomap(region)) {
			res->name  = "reserved";
			res->flags = IORESOURCE_MEM;
		} else {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
		}
		res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
		res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;

		request_resource(&iomem_resource, res);

		if (kernel_code.start >= res->start &&
		    kernel_code.end <= res->end)
			request_resource(res, &kernel_code);
		if (kernel_data.start >= res->start &&
		    kernel_data.end <= res->end)
			request_resource(res, &kernel_data);
#ifdef CONFIG_KEXEC_CORE
		/* Userspace will find "Crash kernel" region in /proc/iomem. */
		if (crashk_res.end && crashk_res.start >= res->start &&
		    crashk_res.end <= res->end)
			request_resource(res, &crashk_res);
#endif
	}
}

static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

/*
 * IAMROOT, 2021.09.11:
 * - logical cpu to mpdir mapping.
 *   kernel code는 전부 logical cpu를 쓴다.
 *
 * - smp_setup_processor_id 에서 boot cput(0번)에 대해서 미리
 *   mpidr을 읽어와 설정한다.
 * - of_parse_and_init_cpus 에서 나머지 cpu에 대해서 dt에서
 *   해당 cpu mpidr을 읽어서 해당 값으로 설정된다.
 */
u64 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

u64 cpu_logical_map(unsigned int cpu)
{
	return __cpu_logical_map[cpu];
}

/* IAMROOT, 2024.01.09:
 * - arch에 의존적인 부분 early setup 영역.
 */
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
	setup_initial_init_mm(_stext, _etext, _edata, _end);

	*cmdline_p = boot_command_line;

	/*
	 * If know now we are going to need KPTI then use non-global
	 * mappings from the start, avoiding the cost of rewriting
	 * everything later.
	 */
	arm64_use_ng_mappings = kaslr_requires_kpti();

	/* IAMROOT, 2021.10.16:
	 * - 정규 매핑 전에 I/O 장치들이 memory를 사용할 수 있도록 fixmap을 이용한
	 *   early memory mapping을 준비한다.
	 *
	 *   early: 정규 매핑, memblock 초기화도 안된 상황.
	 *   late : 나중에 해도 되는 작업들
	 */
	early_fixmap_init();
	early_ioremap_init();

	setup_machine_fdt(__fdt_pointer);

	/*
	 * IAMROOT, 2021.10.16: TODO
	 * - static key
	 *   if의 조건에 사용되는 변수가 변경될일이 거의 없으면서 read를 많이 해야되는경우
	 *   매번 읽는것이 아니라 해당 조건문 자체를 nop나 branch로 교체해서 if문 자체를
	 *   없애는 방법. 또한 likely와 unlikely를 사용해 branch되는 code의 주변 배치여부도
	 *   정해 tlb cache나 data cache hit도 잘되도록 하여 효율을 높인다.
	 *
	 *   enable / disable의 비용이 매우크다. 해당 변수가 존재하는 모든 code를
	 *   교체하는식으로 진행하며 tlb cache등도 비워줘야 되기 때문이다.
	 *
	 *   관련 api : static_branch_likely, static_branch_unlikely
	 */
	/*
	 * Initialise the static keys early as they may be enabled by the
	 * cpufeature code and early parameters.
	 */
	jump_label_init();
	parse_early_param();

	/* IAMROOT, 2021.10.16:
	 * - daif에서 irq, fiq를 제외한 나머지 exceptions을 enable.
	 */
	/*
	 * Unmask asynchronous aborts and fiq after bringing up possible
	 * earlycon. (Report possible System Errors once we can report this
	 * occurred).
	 */
	local_daif_restore(DAIF_PROCCTX_NOIRQ);

	/* IAMROOT, 2021.10.16:
	 * - head.S에서 ttbr0에 mapping 했던 idmap을 해제한다.
	 */
	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	cpu_uninstall_idmap();

	xen_early_init();
	efi_init();

	if (!efi_enabled(EFI_BOOT) && ((u64)_text % MIN_KIMG_ALIGN) != 0)
	     pr_warn(FW_BUG "Kernel image misaligned at boot, please fix your bootloader!");

	arm64_memblock_init();

/* IAMROOT, 2021.10.31:
 * - 현재 mmu config 상태
 *   1) ttbr1_el1 -> init_pg_dir
 *   2) ttbr0_el1 -> empty_zero_page
 */

	paging_init();

/* IAMROOT, 2021.10.31:
 * - 현재 mmu config 상태
 *   1) ttbr1_el1 -> swapper_pg_dir
 *   2) ttbr0_el1 -> empty_zero_page
 */

	acpi_table_upgrade();

	/* Parse the ACPI tables for possible boot-time configuration */
	acpi_boot_table_init();

	if (acpi_disabled)
		unflatten_device_tree();

	bootmem_init();

	kasan_init();

	request_standard_resources();

	early_ioremap_reset();

	if (acpi_disabled)
		psci_dt_init();
	else
		psci_acpi_init();

	init_bootcpu_ops();
	smp_init_cpus();
	smp_build_mpidr_hash();

	/* Init percpu seeds for random tags after cpus are set up. */
	kasan_init_sw_tags();

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Make sure init_thread_info.ttbr0 always generates translation
	 * faults in case uaccess_enable() is inadvertently called by the init
	 * thread.
	 */
	init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
#endif

	if (boot_args[1] || boot_args[2] || boot_args[3]) {
		pr_err("WARNING: x1-x3 nonzero in violation of boot protocol:\n"
			"\tx1: %016llx\n\tx2: %016llx\n\tx3: %016llx\n"
			"This indicates a broken bootloader or old kernel\n",
			boot_args[1], boot_args[2], boot_args[3]);
	}
}

static inline bool cpu_can_disable(unsigned int cpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_can_disable)
		return ops->cpu_can_disable(cpu);
#endif
	return false;
}

static int __init topology_init(void)
{
	int i;

	for_each_online_node(i)
		register_one_node(i);

	for_each_possible_cpu(i) {
		struct cpu *cpu = &per_cpu(cpu_data.cpu, i);
		cpu->hotpluggable = cpu_can_disable(i);
		register_cpu(cpu, i);
	}

	return 0;
}
subsys_initcall(topology_init);

static void dump_kernel_offset(void)
{
	const unsigned long offset = kaslr_offset();

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE) && offset > 0) {
		pr_emerg("Kernel Offset: 0x%lx from 0x%lx\n",
			 offset, KIMAGE_VADDR);
		pr_emerg("PHYS_OFFSET: 0x%llx\n", PHYS_OFFSET);
	} else {
		pr_emerg("Kernel Offset: disabled\n");
	}
}

static int arm64_panic_block_dump(struct notifier_block *self,
				  unsigned long v, void *p)
{
	dump_kernel_offset();
	dump_cpu_features();
	dump_mem_limit();
	return 0;
}

static struct notifier_block arm64_panic_block = {
	.notifier_call = arm64_panic_block_dump
};

static int __init register_arm64_panic_block(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &arm64_panic_block);
	return 0;
}
device_initcall(register_arm64_panic_block);
