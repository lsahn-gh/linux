// SPDX-License-Identifier: GPL-2.0-only
/*
 * alternative runtime patching
 * inspired by the x86 version
 *
 * Copyright (C) 2014 ARM Ltd.
 */

#define pr_fmt(fmt) "alternatives: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <asm/cacheflush.h>
#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/insn.h>
#include <asm/sections.h>
#include <linux/stop_machine.h>

/*
 * IAMROOT, 2022.02.17:
 * - ALTINSTR_ENTRY 참고.
 *   offset 값이 위치한 주소에서 inst가 offset으로 저장을 했엇으므로
 *   offset 변수위치 + offset 값을 하면 inst 주소가 나올것이다.
 */
#define __ALT_PTR(a, f)		((void *)&(a)->f + (a)->f)
#define ALT_ORIG_PTR(a)		__ALT_PTR(a, orig_offset)
#define ALT_REPL_PTR(a)		__ALT_PTR(a, alt_offset)

/* Volatile, as we may be patching the guts of READ_ONCE() */
static volatile int all_alternatives_applied;
/*
 * IAMROOT, 2022.02.17:
 * - __apply_alternatives등에서 설정된다.
 *   alternative된 caps에 대한 bit가 set된다. (module이 아닌 경우에만 해당된다.)
 */
static DECLARE_BITMAP(applied_alternatives, ARM64_NCAPS);

struct alt_region {
	struct alt_instr *begin;
	struct alt_instr *end;
};

bool alternative_is_applied(u16 cpufeature)
{
	if (WARN_ON(cpufeature >= ARM64_NCAPS))
		return false;

	return test_bit(cpufeature, applied_alternatives);
}

/*
 * Check if the target PC is within an alternative block.
 */
/*
 * IAMROOT, 2022.02.18:
 * - pc가 alternative 안에서 일어나는 경우 대체할 필요가 없다는것을 확인하기위함.
 */
static bool branch_insn_requires_update(struct alt_instr *alt, unsigned long pc)
{
	unsigned long replptr = (unsigned long)ALT_REPL_PTR(alt);
	return !(pc >= replptr && pc <= (replptr + alt->alt_len));
}

#define align_down(x, a)	((unsigned long)(x) & ~(((unsigned long)(a)) - 1))

/*
 * IAMROOT, 2022.02.18:
 * - @altinsnptr에서 insn를 구한다. jump류 일경우 offset을 보정해준다.
 */
static u32 get_alt_insn(struct alt_instr *alt, __le32 *insnptr, __le32 *altinsnptr)
{
	u32 insn;

	insn = le32_to_cpu(*altinsnptr);

	if (aarch64_insn_is_branch_imm(insn)) {
		s32 offset = aarch64_get_branch_offset(insn);
		unsigned long target;

		target = (unsigned long)altinsnptr + offset;

		/*
		 * If we're branching inside the alternate sequence,
		 * do not rewrite the instruction, as it is already
		 * correct. Otherwise, generate the new instruction.
		 */
		if (branch_insn_requires_update(alt, target)) {
			offset = target - (unsigned long)insnptr;
			insn = aarch64_set_branch_offset(insn, offset);
		}
/*
 * IAMROOT, 2022.02.17:
 * - __AARCH64_INSN_FUNCS 매크로로 aarch64_insn_is_adrp 등의 함수가 만들어진다.
 *   adrp inst인지 확인한다.
 */
	} else if (aarch64_insn_is_adrp(insn)) {
		s32 orig_offset, new_offset;
		unsigned long target;

		/*
		 * If we're replacing an adrp instruction, which uses PC-relative
		 * immediate addressing, adjust the offset to reflect the new
		 * PC. adrp operates on 4K aligned addresses.
		 */
/*
 * IAMROOT, 2022.02.18:
 * - adrp는 code생성 당시 code 위치에 따른 대상의 offset을 기준으로 만들어 졌다.
 *   code 위치가 달라지므로 그에 따른 offset을 재계산하는 작업을 한다.
 */
		orig_offset  = aarch64_insn_adrp_get_offset(insn);
		target = align_down(altinsnptr, SZ_4K) + orig_offset;
		new_offset = target - align_down(insnptr, SZ_4K);
		insn = aarch64_insn_adrp_set_offset(insn, new_offset);
/*
 * IAMROOT, 2022.02.18:
 * - adrp를 제외한 literal 류는 패치를 진행하지 않나보다.
 */
	} else if (aarch64_insn_uses_literal(insn)) {
		/*
		 * Disallow patching unhandled instructions using PC relative
		 * literal addresses
		 */
		BUG();
	}

	return insn;
}

/*
 * IAMROOT, 2022.02.17:
 * - @nr_inst만큼 updptr을 origptr에 대체한다.
 */
static void patch_alternative(struct alt_instr *alt,
			      __le32 *origptr, __le32 *updptr, int nr_inst)
{
	__le32 *replptr;
	int i;

	replptr = ALT_REPL_PTR(alt);
	for (i = 0; i < nr_inst; i++) {
		u32 insn;

		insn = get_alt_insn(alt, origptr + i, replptr + i);
		updptr[i] = cpu_to_le32(insn);
	}
}

/*
 * We provide our own, private D-cache cleaning function so that we don't
 * accidentally call into the cache.S code, which is patched by us at
 * runtime.
 */
/*
 * IAMROOT, 2022.02.17:
 * - start ~ end까지 data cache clean.
 */
static void clean_dcache_range_nopatch(u64 start, u64 end)
{
	u64 cur, d_size, ctr_el0;

	ctr_el0 = read_sanitised_ftr_reg(SYS_CTR_EL0);
	d_size = 4 << cpuid_feature_extract_unsigned_field(ctr_el0,
							   CTR_DMINLINE_SHIFT);
	cur = start & ~(d_size - 1);
	do {
		/*
		 * We must clean+invalidate to the PoC in order to avoid
		 * Cortex-A53 errata 826319, 827319, 824069 and 819472
		 * (this corresponds to ARM64_WORKAROUND_CLEAN_CACHE)
		 */
		asm volatile("dc civac, %0" : : "r" (cur) : "memory");
	} while (cur += d_size, cur < end);
}

/*
 * IAMROOT, 2022.02.17:
 * - region에서 @is_module, @feature_mask에 대한 alt를 찾아 alternative 한다.
 */
static void __nocfi __apply_alternatives(struct alt_region *region, bool is_module,
				 unsigned long *feature_mask)
{
	struct alt_instr *alt;
	__le32 *origptr, *updptr;
	alternative_cb_t alt_cb;

	for (alt = region->begin; alt < region->end; alt++) {
		int nr_inst;

		if (!test_bit(alt->cpufeature, feature_mask))
			continue;

		/* Use ARM64_CB_PATCH as an unconditional patch */
		if (alt->cpufeature < ARM64_CB_PATCH &&
		    !cpus_have_cap(alt->cpufeature))
			continue;

		if (alt->cpufeature == ARM64_CB_PATCH)
			BUG_ON(alt->alt_len != 0);
		else
			BUG_ON(alt->alt_len != alt->orig_len);

		pr_info_once("patching kernel code\n");

/*
 * IAMROOT, 2022.02.17:
 * - origin instr addr(origptr)과 대체할 inst(updtr)을 구한다.
 */
		origptr = ALT_ORIG_PTR(alt);
		updptr = is_module ? origptr : lm_alias(origptr);
		nr_inst = alt->orig_len / AARCH64_INSN_SIZE;

/*
 * IAMROOT, 2022.02.17:
 * - ALTINSTR_ENTRY형태면 patch_alternative, ALTINSTR_ENTRY_CB형태면
 *   ALT_REPL_PTR로 callback 함수주소를 알아내어 실행한다.
 */
		if (alt->cpufeature < ARM64_CB_PATCH)
			alt_cb = patch_alternative;
		else
			alt_cb  = ALT_REPL_PTR(alt);

		alt_cb(alt, origptr, updptr, nr_inst);
/*
 * IAMROOT, 2022.02.17:
 * - origptr ~ nr_inst 까지 대체 됬으니 clean시켜준다.
 */
		if (!is_module) {
			clean_dcache_range_nopatch((u64)origptr,
						   (u64)(origptr + nr_inst));
		}
	}

	/*
	 * The core module code takes care of cache maintenance in
	 * flush_module_icache().
	 */
	if (!is_module) {
/*
 * IAMROOT, 2022.02.17:
 * - inst cache invaildate
 */
		dsb(ish);
		icache_inval_all_pou();
		isb();
/*
 * IAMROOT, 2022.02.17:
 * - 현재 feature_mask에 대한것을 적용하되 현재 지원중인 caps만으로 다시 필터링한다.
 */
		/* Ignore ARM64_CB bit from feature mask */
		bitmap_or(applied_alternatives, applied_alternatives,
			  feature_mask, ARM64_NCAPS);
		bitmap_and(applied_alternatives, applied_alternatives,
			   cpu_hwcaps, ARM64_NCAPS);
	}
}

/*
 * We might be patching the stop_machine state machine, so implement a
 * really simple polling protocol here.
 */
static int __apply_alternatives_multi_stop(void *unused)
{
	struct alt_region region = {
		.begin	= (struct alt_instr *)__alt_instructions,
		.end	= (struct alt_instr *)__alt_instructions_end,
	};

	/* We always have a CPU 0 at this point (__init) */
	if (smp_processor_id()) {
		while (!all_alternatives_applied)
			cpu_relax();
		isb();
	} else {
		DECLARE_BITMAP(remaining_capabilities, ARM64_NPATCHABLE);

		bitmap_complement(remaining_capabilities, boot_capabilities,
				  ARM64_NPATCHABLE);

		BUG_ON(all_alternatives_applied);
		__apply_alternatives(&region, false, remaining_capabilities);
		/* Barriers provided by the cache flushing */
		all_alternatives_applied = 1;
	}

	return 0;
}

void __init apply_alternatives_all(void)
{
	/* better not try code patching on a live SMP system */
	stop_machine(__apply_alternatives_multi_stop, NULL, cpu_online_mask);
}

/*
 * This is called very early in the boot process (directly after we run
 * a feature detect on the boot CPU). No need to worry about other CPUs
 * here.
 */
/*
 * IAMROOT, 2022.02.17:
 * - 여지껏 봐왔던 boot_capabilities에 대한것들에 대해
 */
void __init apply_boot_alternatives(void)
{
	struct alt_region region = {
		.begin	= (struct alt_instr *)__alt_instructions,
		.end	= (struct alt_instr *)__alt_instructions_end,
	};

	/* If called on non-boot cpu things could go wrong */
	WARN_ON(smp_processor_id() != 0);

	__apply_alternatives(&region, false, &boot_capabilities[0]);
}

#ifdef CONFIG_MODULES
void apply_alternatives_module(void *start, size_t length)
{
	struct alt_region region = {
		.begin	= start,
		.end	= start + length,
	};
	DECLARE_BITMAP(all_capabilities, ARM64_NPATCHABLE);

	bitmap_fill(all_capabilities, ARM64_NPATCHABLE);

	__apply_alternatives(&region, true, &all_capabilities[0]);
}
#endif
