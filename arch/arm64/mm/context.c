// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/context.c
 *
 * Copyright (C) 2002-2003 Deep Blue Solutions Ltd, all rights reserved.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include <asm/cpufeature.h>
#include <asm/mmu_context.h>
#include <asm/smp.h>
#include <asm/tlbflush.h>

/*
 * IAMROOT, 2023.01.28:
 * - 8bit or 16bit
 *   8bit일 경우 256
 *   16bit일 경우 65536
 */
static u32 asid_bits;
static DEFINE_RAW_SPINLOCK(cpu_asid_lock);

/*
 * IAMROOT, 2023.01.28:
 * - 8bit일 경우
 *   0 ~ 0x100, 0x200, 0x300..
 * - base asid
 */
static atomic64_t asid_generation;
static unsigned long *asid_map;

static DEFINE_PER_CPU(atomic64_t, active_asids);
static DEFINE_PER_CPU(u64, reserved_asids);
static cpumask_t tlb_flush_pending;

static unsigned long max_pinned_asids;
static unsigned long nr_pinned_asids;
static unsigned long *pinned_asid_map;

#define ASID_MASK		(~GENMASK(asid_bits - 1, 0))

/*
 * IAMROOT, 2023.01.28:
 * - asid 단위.
 */
#define ASID_FIRST_VERSION	(1UL << asid_bits)

#define NUM_USER_ASIDS		ASID_FIRST_VERSION
#define asid2idx(asid)		((asid) & ~ASID_MASK)
#define idx2asid(idx)		asid2idx(idx)

/* Get the ASIDBits supported by the current CPU */
static u32 get_cpu_asid_bits(void)
{
	u32 asid;
	int fld = cpuid_feature_extract_unsigned_field(read_cpuid(ID_AA64MMFR0_EL1),
						ID_AA64MMFR0_ASID_SHIFT);

	switch (fld) {
	default:
		pr_warn("CPU%d: Unknown ASID size (%d); assuming 8-bit\n",
					smp_processor_id(),  fld);
		fallthrough;
	case 0:
		asid = 8;
		break;
	case 2:
		asid = 16;
	}

	return asid;
}

/* Check if the current cpu's ASIDBits is compatible with asid_bits */
void verify_cpu_asid_bits(void)
{
	u32 asid = get_cpu_asid_bits();

	if (asid < asid_bits) {
		/*
		 * We cannot decrease the ASID size at runtime, so panic if we support
		 * fewer ASID bits than the boot CPU.
		 */
		pr_crit("CPU%d: smaller ASID size(%u) than boot CPU (%u)\n",
				smp_processor_id(), asid, asid_bits);
		cpu_panic_kernel();
	}
}


/*
 * IAMROOT, 2023.01.28:
 * - KPTI asid bits set.
 *   0xaa로 @map을 채운다.
 */
static void set_kpti_asid_bits(unsigned long *map)
{
	unsigned int len = BITS_TO_LONGS(NUM_USER_ASIDS) * sizeof(unsigned long);
	/*
	 * In case of KPTI kernel/user ASIDs are allocated in
	 * pairs, the bottom bit distinguishes the two: if it
	 * is set, then the ASID will map only userspace. Thus
	 * mark even as reserved for kernel.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   KPTI 커널/사용자 ASID가 쌍으로 할당된 경우 하단 비트는 두 가지를 구분합니다.
 *   설정되어 있으면 ASID는 사용자 공간만 매핑합니다. 따라서 커널용으로 예약된 
 *   것으로 표시합니다.
 */
	memset(map, 0xaa, len);
}

/*
 * IAMROOT, 2023.01.28:
 * - pinned_asid_map이 있다면 pinned_asid_map를 asid_map에 복사한다.
 *   kpti를 사용하는경우 asid_map을 0xaa로 채운다.
 *   그것도 아니라면 asid_map을 0로 clear한다.
 */
static void set_reserved_asid_bits(void)
{
	if (pinned_asid_map)
		bitmap_copy(asid_map, pinned_asid_map, NUM_USER_ASIDS);
	else if (arm64_kernel_unmapped_at_el0())
		set_kpti_asid_bits(asid_map);
	else
		bitmap_clear(asid_map, 0, NUM_USER_ASIDS);
}

/*
 * IAMROOT, 2023.01.28:
 * - 세대(generation)이 바뀌었는지 확인ㄴ한다.
 * - 앞자리가 바뀐지를 확인하는 개념.
 * - ex) asid 0x503, asid_generation = 0x400, bit = 8bit
 *   결과 false.
 */
#define asid_gen_match(asid) \
	(!(((asid) ^ atomic64_read(&asid_generation)) >> asid_bits))

/*
 * IAMROOT, 2023.01.28:
 * - 1. asid_map clear
 *   2. reserved_asids에 마지막에 사용했던 active_asids를 기록. 
 *   3. 마지막 사용했던 asid를 asid_map에 기록.
 *   4. 모든 cpu에 tlb flush pending 등록.
 */
static void flush_context(void)
{
	int i;
	u64 asid;

	/* Update the list of reserved ASIDs and the ASID bitmap. */
	set_reserved_asid_bits();

/*
 * IAMROOT, 2023.01.28:
 * - pcpu active_asids를 0으로 clear하면서 이전값을 asid로 가져온다.
 */
	for_each_possible_cpu(i) {
		asid = atomic64_xchg_relaxed(&per_cpu(active_asids, i), 0);
		/*
		 * If this CPU has already been through a
		 * rollover, but hasn't run another task in
		 * the meantime, we must preserve its reserved
		 * ASID, as this is the only trace we have of
		 * the process it is still running.
		 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   이 CPU가 이미 롤오버를 겪었지만 그 동안 다른 작업을 실행하지 
 *   않은 경우 예약된 ASID를 보존해야 합니다. 이것이 여전히 실행 
 *   중인 프로세스에 대한 유일한 추적이기 때문입니다.
 * - 롤오버가 된 경우 reserved_asids를 asid로 가져온다.
 * - 맨 마지막의 asid를 reserved_asids로 등록한다.
 */
		if (asid == 0)
			asid = per_cpu(reserved_asids, i);
		__set_bit(asid2idx(asid), asid_map);
		per_cpu(reserved_asids, i) = asid;
	}

	/*
	 * Queue a TLB invalidation for each CPU to perform on next
	 * context-switch
	 */
/*
 * IAMROOT, 2023.01.28:
 * - next context switch에 tlb flush가 되도록 모든 cpu에 설정한다.
 *   해당 cpu들은 context switch일때 local tlb flush를 할것이다.
 */
	cpumask_setall(&tlb_flush_pending);
}

/*
 * IAMROOT, 2023.01.28:
 * - pcpu reserved_asids에 @asid가 있다면 hit로 하고, newasid를 
 *   reserved_asids로 등록한다.
 *   버려지게 될 asid가 이미 reserved로 등록되있다면 바뀌게될
 *   newasid로 전환해줘야하기때문이다.
 */
static bool check_update_reserved_asid(u64 asid, u64 newasid)
{
	int cpu;
	bool hit = false;

	/*
	 * Iterate over the set of reserved ASIDs looking for a match.
	 * If we find one, then we can update our mm to use newasid
	 * (i.e. the same ASID in the current generation) but we can't
	 * exit the loop early, since we need to ensure that all copies
	 * of the old ASID are updated to reflect the mm. Failure to do
	 * so could result in us missing the reserved ASID in a future
	 * generation.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   일치를 찾는 예약된 ASID 세트를 반복합니다. 
 *   하나를 찾으면 newasid(즉, 현재 세대의 동일한 ASID)를 사용하도록 
 *   mm을 업데이트할 수 있지만 루프를 조기에 종료할 수는 없습니다. 
 *   mm. 그렇게 하지 않으면 미래 세대에서 예약된 ASID가 누락될 수 
 *   있습니다.
. 
 */
	for_each_possible_cpu(cpu) {
		if (per_cpu(reserved_asids, cpu) == asid) {
			hit = true;
			per_cpu(reserved_asids, cpu) = newasid;
		}
	}

	return hit;
}

/*
 * IAMROOT, 2023.01.28:
 * - asid를 generation을 붙여 newasid를 만든다.
 *   1. newasid가 reserved_asid에 있으면 reserved_asids를 사용한다.
 *   2. 요청한 asid가 asid_map에서 비어있는경우 해당자리를 사용한다.
 *   3. 요청한 자리가 asid_map에서 이미 사용중인경우 cur_idx부터
 *      빈자리를 찾아서 새로 asid를 구해온다.
 *   4. asid_map이 full인경우 generation을 증가시키고 
 *      모든 cpu에 대해 flush tlb pending후 asid_map을 새로 설정하여 다시 
 *      asid를 가져온다.
 */
static u64 new_context(struct mm_struct *mm)
{
	static u32 cur_idx = 1;
	u64 asid = atomic64_read(&mm->context.id);
	u64 generation = atomic64_read(&asid_generation);

/*
 * IAMROOT, 2023.01.28:
 * - 최초가 아니면
 */
	if (asid != 0) {

/*
 * IAMROOT, 2023.01.28:
 * - 세대(앞자리) + mask값.
 *   ex) generation = 0x300, asid = 0x150인경우 
 *       newasid = 0x350
 */
		u64 newasid = generation | (asid & ~ASID_MASK);

		/*
		 * If our current ASID was active during a rollover, we
		 * can continue to use it and this was just a false alarm.
		 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   롤오버 중에 현재 ASID가 활성화된 경우 계속 사용할 수 있으며 
 *   이는 잘못된 경보일 뿐입니다.
 * - reserved_asids에 asid가 있는지 확인한다. asid가 있었다면 
 *   newasid로 바뀌고 return한다.
 */
		if (check_update_reserved_asid(asid, newasid))
			return newasid;

		/*
		 * If it is pinned, we can keep using it. Note that reserved
		 * takes priority, because even if it is also pinned, we need to
		 * update the generation into the reserved_asids.
		 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *  고정되어 있으면 계속 사용할 수 있습니다. reserved도 고정된 경우에도 
 *  reserved_asids로 세대를 업데이트해야 하기 때문에 reserved가 우선 
 *  순위입니다.
 */
		if (refcount_read(&mm->context.pinned))
			return newasid;

		/*
		 * We had a valid ASID in a previous life, so try to re-use
		 * it if possible.
		 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   전생에 유효한 ASID가 있었으므로 가능하면 다시 사용하십시오.
 * - asid_map에 asid에 이미 set되있었는지 검사하면서 set한다.
 *   사용중이 아니라면 return newasid.
 */
		if (!__test_and_set_bit(asid2idx(asid), asid_map))
			return newasid;
	
/*
 * IAMROOT, 2023.01.28:
 * -  이 부분까지왔다면 fork등의 이유로 누군가가 asid_map의 asid자리를
 *    차지한 상태가된다.
 */
	}

	/*
	 * Allocate a free ASID. If we can't find one, take a note of the
	 * currently active ASIDs and mark the TLBs as requiring flushes.  We
	 * always count from ASID #2 (index 1), as we use ASID #0 when setting
	 * a reserved TTBR0 for the init_mm and we allocate ASIDs in even/odd
	 * pairs.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   무료 ASID를 할당합니다. 찾을 수 없는 경우 현재 활성 ASID를 기록하고 
 *   TLB를 플러시가 필요한 것으로 표시합니다. init_mm에 대해 예약된 TTBR0을
 *   설정할 때 ASID #0을 사용하고 짝수/홀수 쌍으로 ASID를 할당하므로 
 *   항상 ASID #2(색인 1)부터 계산합니다.
 *
 * - asid_map의 첫 zero를 찾는다. 찾아졌다면 return.
 */
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);
	if (asid != NUM_USER_ASIDS)
		goto set_asid;

/*
 * IAMROOT, 2023.01.28:
 * - asid_map이 가득 찻다면 asid_generation을 update하고, asid_map을 새로
 *   설정한다.
 */
	/* We're out of ASIDs, so increment the global generation count */
	generation = atomic64_add_return_relaxed(ASID_FIRST_VERSION,
						 &asid_generation);
	flush_context();

/*
 * IAMROOT, 2023.01.28:
 * - 새로설정된 asid_map에서 asid를 얻어온다.
 */
	/* We have more ASIDs than CPUs, so this will always succeed */
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, 1);

set_asid:
	__set_bit(asid, asid_map);
	cur_idx = asid;
	return idx2asid(asid) | generation;
}

/*
 * IAMROOT, 2023.01.28:
 * - 상황에따라 fastpath / slowpath로 동작한다.
 *   1. fastpath 조건.
 *     - active_asids가 0이 아니다
 *     - 세대가 같다.
 *     - active_asids update가 성공.
 *   2. fastpath 실패시 slowpath로 동작한다. 
 *      실패 조건에따라 asid를 새로 얻어올수 있다.
 *   3. tlb flush pending조건이 있으면 수행한다.
 *   4. mm을 교체한다.
 */
void check_and_switch_context(struct mm_struct *mm)
{
	unsigned long flags;
	unsigned int cpu;
	u64 asid, old_active_asid;

	if (system_supports_cnp())
		cpu_set_reserved_ttbr0();

/*
 * IAMROOT, 2023.01.28:
 * - asid + 가상주소로 물리주소를 매핑하는 식으로 사용한다.
 *   pid를 특수한 방식으로 가공하여 asid를 만듣나.
 */
	asid = atomic64_read(&mm->context.id);

	/*
	 * The memory ordering here is subtle.
	 * If our active_asids is non-zero and the ASID matches the current
	 * generation, then we update the active_asids entry with a relaxed
	 * cmpxchg. Racing with a concurrent rollover means that either:
	 *
	 * - We get a zero back from the cmpxchg and end up waiting on the
	 *   lock. Taking the lock synchronises with the rollover and so
	 *   we are forced to see the updated generation.
	 *
	 * - We get a valid ASID back from the cmpxchg, which means the
	 *   relaxed xchg in flush_context will treat us as reserved
	 *   because atomic RmWs are totally ordered for a given location.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   여기서 메모리 순서는 미묘합니다.
 *   active_asids가 0이 아니고 ASID가 현재 세대와 일치하는 경우 완화된 
 *   cmpxchg로 active_asids 항목을 업데이트합니다. 동시 롤오버가 있는 
 *   레이싱은 다음 중 하나를 의미합니다.
 *
 *   - 우리는 cmpxchg에서 0을 되찾고 잠금을 기다리게 됩니다. 잠금을 
 *   해제하면 롤오버와 동기화되므로 업데이트된 세대를 확인해야 합니다.
 *
 *   - 우리는 cmpxchg에서 유효한 ASID를 다시 얻습니다. 이는 원자 RmW가 
 *   주어진 위치에 대해 완전히 주문되기 때문에 flush_context의 완화된 
 *   xchg가 우리를 예약된 것으로 취급함을 의미합니다.
 *
 * - fastpath mm 구간
 */
	old_active_asid = atomic64_read(this_cpu_ptr(&active_asids));
/*
 * IAMROOT, 2023.01.28:
 * - 세대가 같고, active_asids를 asid로 바꾸는데 성공했으면 
 *   fastpath 성공
 */
	if (old_active_asid && asid_gen_match(asid) &&
	    atomic64_cmpxchg_relaxed(this_cpu_ptr(&active_asids),
				     old_active_asid, asid))
		goto switch_mm_fastpath;

/*
 * IAMROOT, 2023.01.28:
 * - slowpath
 *   old_active_asid 0이던가, 세대가 바뀌었던가, asid를 넣는게 
 *   실패했을때 진입.
 */
	raw_spin_lock_irqsave(&cpu_asid_lock, flags);
	/* Check that our ASID belongs to the current generation. */
	asid = atomic64_read(&mm->context.id);

/*
 * IAMROOT, 2023.01.28:
 * - 세대비교가 실패하면 asid를 새로 생성한다.
 */
	if (!asid_gen_match(asid)) {
		asid = new_context(mm);
/*
 * IAMROOT, 2023.01.28:
 * - 새로만든 asid를 mm에 set해준다.
 */
		atomic64_set(&mm->context.id, asid);
	}

	cpu = smp_processor_id();

/*
 * IAMROOT, 2023.01.28:
 * - tlb flush pending요청이 있으면 flush한다.
 * - new_context()에서 현재 tlb flush pending요청을 했을경우
 *   자기신의 경우엔 여기서 즉시 될것이다.
 */
	if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending))
		local_flush_tlb_all();

/*
 * IAMROOT, 2023.01.28:
 * - 바뀐 asid로 active_asids를 update한다.
 */
	atomic64_set(this_cpu_ptr(&active_asids), asid);
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

switch_mm_fastpath:

	arm64_apply_bp_hardening();

	/*
	 * Defer TTBR0_EL1 setting for user threads to uaccess_enable() when
	 * emulating PAN.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - hw가 지원하면 여기서 cpu_switch_mm을 한다.
 *   sw로 pan을 하는 경우엔 나중에(uaccess_enable()) 한다.
 */
	if (!system_uses_ttbr0_pan())
		cpu_switch_mm(mm->pgd, mm);
}

unsigned long arm64_mm_context_get(struct mm_struct *mm)
{
	unsigned long flags;
	u64 asid;

	if (!pinned_asid_map)
		return 0;

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);

	asid = atomic64_read(&mm->context.id);

	if (refcount_inc_not_zero(&mm->context.pinned))
		goto out_unlock;

	if (nr_pinned_asids >= max_pinned_asids) {
		asid = 0;
		goto out_unlock;
	}

	if (!asid_gen_match(asid)) {
		/*
		 * We went through one or more rollover since that ASID was
		 * used. Ensure that it is still valid, or generate a new one.
		 */
		asid = new_context(mm);
		atomic64_set(&mm->context.id, asid);
	}

	nr_pinned_asids++;
	__set_bit(asid2idx(asid), pinned_asid_map);
	refcount_set(&mm->context.pinned, 1);

out_unlock:
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

	asid &= ~ASID_MASK;

	/* Set the equivalent of USER_ASID_BIT */
	if (asid && arm64_kernel_unmapped_at_el0())
		asid |= 1;

	return asid;
}
EXPORT_SYMBOL_GPL(arm64_mm_context_get);

void arm64_mm_context_put(struct mm_struct *mm)
{
	unsigned long flags;
	u64 asid = atomic64_read(&mm->context.id);

	if (!pinned_asid_map)
		return;

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);

	if (refcount_dec_and_test(&mm->context.pinned)) {
		__clear_bit(asid2idx(asid), pinned_asid_map);
		nr_pinned_asids--;
	}

	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);
}
EXPORT_SYMBOL_GPL(arm64_mm_context_put);

/* Errata workaround post TTBRx_EL1 update. */
asmlinkage void post_ttbr_update_workaround(void)
{
	if (!IS_ENABLED(CONFIG_CAVIUM_ERRATUM_27456))
		return;

	asm(ALTERNATIVE("nop; nop; nop",
			"ic iallu; dsb nsh; isb",
			ARM64_WORKAROUND_CAVIUM_27456));
}

void cpu_do_switch_mm(phys_addr_t pgd_phys, struct mm_struct *mm)
{
/*
 * IAMROOT, 2021.10.30:
 * - ttbr1의 경우 원래 값을 가져온후 mm에 있는 asid값으로 교체를 하고
 *   ttbr0는 인자로 가져온 page table + asid + cnp로 설정한다.
 */
	unsigned long ttbr1 = read_sysreg(ttbr1_el1);
	unsigned long asid = ASID(mm);
	unsigned long ttbr0 = phys_to_ttbr(pgd_phys);

	/* Skip CNP for the reserved ASID */
	if (system_supports_cnp() && asid)
		ttbr0 |= TTBR_CNP_BIT;

	/* SW PAN needs a copy of the ASID in TTBR0 for entry */
	if (IS_ENABLED(CONFIG_ARM64_SW_TTBR0_PAN))
		ttbr0 |= FIELD_PREP(TTBR_ASID_MASK, asid);

	/* Set ASID in TTBR1 since TCR.A1 is set */
	ttbr1 &= ~TTBR_ASID_MASK;
	ttbr1 |= FIELD_PREP(TTBR_ASID_MASK, asid);

	write_sysreg(ttbr1, ttbr1_el1);
	isb();
	write_sysreg(ttbr0, ttbr0_el1);
	isb();
	post_ttbr_update_workaround();
}

static int asids_update_limit(void)
{
	unsigned long num_available_asids = NUM_USER_ASIDS;

	if (arm64_kernel_unmapped_at_el0()) {
		num_available_asids /= 2;
		if (pinned_asid_map)
			set_kpti_asid_bits(pinned_asid_map);
	}
	/*
	 * Expect allocation after rollover to fail if we don't have at least
	 * one more ASID than CPUs. ASID #0 is reserved for init_mm.
	 */
	WARN_ON(num_available_asids - 1 <= num_possible_cpus());
	pr_info("ASID allocator initialised with %lu entries\n",
		num_available_asids);

	/*
	 * There must always be an ASID available after rollover. Ensure that,
	 * even if all CPUs have a reserved ASID and the maximum number of ASIDs
	 * are pinned, there still is at least one empty slot in the ASID map.
	 */
	max_pinned_asids = num_available_asids - num_possible_cpus() - 2;
	return 0;
}
arch_initcall(asids_update_limit);

static int asids_init(void)
{
	asid_bits = get_cpu_asid_bits();
	atomic64_set(&asid_generation, ASID_FIRST_VERSION);
	asid_map = bitmap_zalloc(NUM_USER_ASIDS, GFP_KERNEL);
	if (!asid_map)
		panic("Failed to allocate bitmap for %lu ASIDs\n",
		      NUM_USER_ASIDS);

	pinned_asid_map = bitmap_zalloc(NUM_USER_ASIDS, GFP_KERNEL);
	nr_pinned_asids = 0;

	/*
	 * We cannot call set_reserved_asid_bits() here because CPU
	 * caps are not finalized yet, so it is safer to assume KPTI
	 * and reserve kernel ASID's from beginning.
	 */
	if (IS_ENABLED(CONFIG_UNMAP_KERNEL_AT_EL0))
		set_kpti_asid_bits(asid_map);
	return 0;
}
early_initcall(asids_init);
