// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Housekeeping management. Manage the targets for routine code that can run on
 *  any CPU: unbound workqueues, timers, kthreads and any offloadable work.
 *
 * Copyright (C) 2017 Red Hat, Inc., Frederic Weisbecker
 * Copyright (C) 2017-2018 SUSE, Frederic Weisbecker
 *
 */
#include "sched.h"

DEFINE_STATIC_KEY_FALSE(housekeeping_overridden);
EXPORT_SYMBOL_GPL(housekeeping_overridden);

/*
 * IAMROOT, 2023.04.15:
 * - housekeeping_setup() 참고.
 *   isolate cpu가 아닌 cpu-list
 */
static cpumask_var_t housekeeping_mask;
/*
 * IAMROOT, 2023.04.15:
 * - housekeeping_setup() 참고.
 *   isolate 종류(HK_FLAG_DOMAIN)
 */
static unsigned int housekeeping_flags;

bool housekeeping_enabled(enum hk_flags flags)
{
	return !!(housekeeping_flags & flags);
}
EXPORT_SYMBOL_GPL(housekeeping_enabled);

int housekeeping_any_cpu(enum hk_flags flags)
{
	int cpu;

	if (static_branch_unlikely(&housekeeping_overridden)) {
		if (housekeeping_flags & flags) {
			cpu = sched_numa_find_closest(housekeeping_mask, smp_processor_id());
			if (cpu < nr_cpu_ids)
				return cpu;

			return cpumask_any_and(housekeeping_mask, cpu_online_mask);
		}
	}
	return smp_processor_id();
}
EXPORT_SYMBOL_GPL(housekeeping_any_cpu);

/*
 * IAMROOT, 2023.04.15:
 * - housekeeping_flags에 flags가 set되있으면 해당 flags로 housekeeping 설정이 있다는 뜻.
 *   housekeeping이 cpu-list를 reteurn한다.
 *   flag가 없다면 cpu_possible_mask(전체 cpu라는 뜻) return.
 */
const struct cpumask *housekeeping_cpumask(enum hk_flags flags)
{
	if (static_branch_unlikely(&housekeeping_overridden))
		if (housekeeping_flags & flags)
			return housekeeping_mask;
	return cpu_possible_mask;
}
EXPORT_SYMBOL_GPL(housekeeping_cpumask);

void housekeeping_affine(struct task_struct *t, enum hk_flags flags)
{
	if (static_branch_unlikely(&housekeeping_overridden))
		if (housekeeping_flags & flags)
			set_cpus_allowed_ptr(t, housekeeping_mask);
}
EXPORT_SYMBOL_GPL(housekeeping_affine);

bool housekeeping_test_cpu(int cpu, enum hk_flags flags)
{
	if (static_branch_unlikely(&housekeeping_overridden))
		if (housekeeping_flags & flags)
			return cpumask_test_cpu(cpu, housekeeping_mask);
	return true;
}
EXPORT_SYMBOL_GPL(housekeeping_test_cpu);

void __init housekeeping_init(void)
{
	if (!housekeeping_flags)
		return;

	static_branch_enable(&housekeeping_overridden);

	if (housekeeping_flags & HK_FLAG_TICK)
		sched_tick_offload_init();

	/* We need at least one CPU to handle housekeeping work */
	WARN_ON_ONCE(cpumask_empty(housekeeping_mask));
}

/*
 * IAMROOT, 2023.04.15:
 * - @str format은 다음과 같은 상황이였는데
 *   Format: [flag-list,]<cpu-list>
 *   이전에 flag-list를 건너 뛴 상태이므로 cpu-list만 남겨져 있다.
 *   (ex, 1,2,10-15)
 *   이 string을 bitmap으로 만든다.
 */
static int __init housekeeping_setup(char *str, enum hk_flags flags)
{
	cpumask_var_t non_housekeeping_mask;
	cpumask_var_t tmp;

/*
 * IAMROOT, 2023.04.15:
 * - flag가 설정된 cpu들이 non_housekeeping_mask에 설정된다.
 */
	alloc_bootmem_cpumask_var(&non_housekeeping_mask);
	if (cpulist_parse(str, non_housekeeping_mask) < 0) {
		pr_warn("Housekeeping: nohz_full= or isolcpus= incorrect CPU range\n");
		free_bootmem_cpumask_var(non_housekeeping_mask);
		return 0;
	}

	alloc_bootmem_cpumask_var(&tmp);

/*
 * IAMROOT, 2023.04.15:
 * - 기존에 한번도 설정한적이 없는경우.
 */
	if (!housekeeping_flags) {
		alloc_bootmem_cpumask_var(&housekeeping_mask);

/*
 * IAMROOT, 2023.04.15:
 * - cpu-list 범위 밖 cpu들을 housekeeping_mask에 저장한다.
 */
		cpumask_andnot(housekeeping_mask,
			       cpu_possible_mask, non_housekeeping_mask);

/*
 * IAMROOT, 2023.04.15:
 * - 전부 present된 cpu들만 flag가 set되있다면 평범하게 동작할수있는 cpu가 1개도
 *   없는 개념이 되는 상황이다. 거기에 대한 예외처리.
 */
		cpumask_andnot(tmp, cpu_present_mask, non_housekeeping_mask);
		if (cpumask_empty(tmp)) {
			pr_warn("Housekeeping: must include one present CPU, "
				"using boot CPU:%d\n", smp_processor_id());
			__cpumask_set_cpu(smp_processor_id(), housekeeping_mask);
			__cpumask_clear_cpu(smp_processor_id(), non_housekeeping_mask);
		}

/*
 * IAMROOT, 2023.04.15:
 * - 한번이라도 설정한경우, cpu 검사만을 하고 flag를 추가하는 식으로 진행한다.
 */
	} else {

/*
 * IAMROOT, 2023.04.15:
 * - tmp에서는 non_housekeeping_mask이 설정안된 cpu들만 남게된다.
 *   present_mask를 통해 최소 1개의 cpu라도 있도록 예외처리를 하고,
 *   최종적으로 non_housekeeping_mask가 설정이 안된 possible cpu만이 tmp에 남게된다.
 */
		cpumask_andnot(tmp, cpu_present_mask, non_housekeeping_mask);
		if (cpumask_empty(tmp))
			__cpumask_clear_cpu(smp_processor_id(), non_housekeeping_mask);
		cpumask_andnot(tmp, cpu_possible_mask, non_housekeeping_mask);
		if (!cpumask_equal(tmp, housekeeping_mask)) {
			pr_warn("Housekeeping: nohz_full= must match isolcpus=\n");
			free_bootmem_cpumask_var(tmp);
			free_bootmem_cpumask_var(non_housekeeping_mask);
			return 0;
		}
	}
	free_bootmem_cpumask_var(tmp);

	if ((flags & HK_FLAG_TICK) && !(housekeeping_flags & HK_FLAG_TICK)) {
		if (IS_ENABLED(CONFIG_NO_HZ_FULL)) {
			tick_nohz_full_setup(non_housekeeping_mask);
		} else {
			pr_warn("Housekeeping: nohz unsupported."
				" Build with CONFIG_NO_HZ_FULL\n");
			free_bootmem_cpumask_var(non_housekeeping_mask);
			return 0;
		}
	}

	housekeeping_flags |= flags;

	free_bootmem_cpumask_var(non_housekeeping_mask);

	return 1;
}

static int __init housekeeping_nohz_full_setup(char *str)
{
	unsigned int flags;

	flags = HK_FLAG_TICK | HK_FLAG_WQ | HK_FLAG_TIMER | HK_FLAG_RCU |
		HK_FLAG_MISC | HK_FLAG_KTHREAD;

	return housekeeping_setup(str, flags);
}
__setup("nohz_full=", housekeeping_nohz_full_setup);

/*
 * IAMROOT, 2023.04.15:
 * - admin-guide/kernel-parameters.txt 참고 (papago)
 *   nohz
 *   단일 작업이 실행될 때 틱을 비활성화합니다. 
 *
 *   나머지 1Hz 틱은 작업 대기열로 오프로드되며, 작업 대기열은 
 *   /sys/devices/virtual/workqueue/cpumask sysfs 파일을 통해 구성된 
 *   전역 작업 대기열의 선호도를 통해 또는 아래에 설명된 '도메인' 플래그를 
 *   사용하여 하우스키핑에 연결해야 합니다.
 *
 *   참고: 기본적으로 글로벌 작업 대기열은 모든 CPU에서 실행되므로 
 *   개별 CPU를 보호하려면 부팅 후 'cpumask' 파일을 수동으로 구성해야 합니다. 
 *
 *   domain
 *   일반 SMP 밸런싱 및 스케줄링 알고리즘에서 격리합니다.
 *   이 방법으로 도메인 격리를 수행하는 것은 되돌릴 수 없습니다.
 *   isolcpus를 통해 격리된 CPU를 도메인으로 다시 가져올 수 없습니다.
 *   cpuset.sched_load_balance 파일을 통해 스케줄러 로드 밸런싱을 
 *   비활성화하려면 대신 cpuset를 사용하는 것이 좋습니다.
 *   CPU가 언제든지 격리된 세트 안팎으로 이동할 수 있는 훨씬 더 유연한 
 *   인터페이스를 제공합니다.
 *
 *   CPU 선호도 syscalls 또는 cpuset을 통해 격리된 CPU로 프로세스를 
 *   이동하거나 분리할 수 있습니다.
 *   <cpu number>는 0에서 시작하고 최대값은 시스템의 CPU 수 - 1입니다. 
 *
 * - flags가 없으면 domain이 기본값이다.
 *   ex) isolcpus=1,5,10-30 -> 23개의 cpu가 domain에 참여하지 않는다는뜻.
 *
 * - @str format은 다음과 같다.
 *   Format: [flag-list,]<cpu-list>
 *   flag-list에서 미리 관련 flag를 정리해서 설정한후, cpu-list만 남겨
 *   cpu-list string으로 bitmap을 만드는 구조가 된다.
 */
static int __init housekeeping_isolcpus_setup(char *str)
{
	unsigned int flags = 0;
	bool illegal = false;
	char *par;
	int len;

	while (isalpha(*str)) {
		if (!strncmp(str, "nohz,", 5)) {
			str += 5;
			flags |= HK_FLAG_TICK;
			continue;
		}

		if (!strncmp(str, "domain,", 7)) {
			str += 7;
			flags |= HK_FLAG_DOMAIN;
			continue;
		}

		if (!strncmp(str, "managed_irq,", 12)) {
			str += 12;
			flags |= HK_FLAG_MANAGED_IRQ;
			continue;
		}

		/*
		 * Skip unknown sub-parameter and validate that it is not
		 * containing an invalid character.
		 */
		for (par = str, len = 0; *str && *str != ','; str++, len++) {
			if (!isalpha(*str) && *str != '_')
				illegal = true;
		}

		if (illegal) {
			pr_warn("isolcpus: Invalid flag %.*s\n", len, par);
			return 0;
		}

		pr_info("isolcpus: Skipped unknown flag %.*s\n", len, par);
		str++;
	}

	/* Default behaviour for isolcpus without flags */
	if (!flags)
		flags |= HK_FLAG_DOMAIN;

	return housekeeping_setup(str, flags);
}
__setup("isolcpus=", housekeeping_isolcpus_setup);
