// SPDX-License-Identifier: GPL-2.0
/*
 * Arch specific cpu topology information
 *
 * Copyright (C) 2016, ARM Ltd.
 * Written by: Juri Lelli, ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sched/topology.h>
#include <linux/cpuset.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/smp.h>

static DEFINE_PER_CPU(struct scale_freq_data __rcu *, sft_data);
static struct cpumask scale_freq_counters_mask;
/*
 * IAMROOT, 2022.12.14:
 * - 현재 freq invariance가 지원가능 상태를 저장한다.
 */
static bool scale_freq_invariant;

/*
 * IAMROOT, 2022.12.14:
 * - scale_freq_counters_mask에 @cpus들이 전부 포함되는지 확인한다.
 */
static bool supports_scale_freq_counters(const struct cpumask *cpus)
{
	return cpumask_subset(cpus, &scale_freq_counters_mask);
}

/*
 * IAMROOT, 2022.12.14:
 * - freq invariance를 지원하거나, online cpu들이 scale_freq_counters에
 *   포함이 전부 된다면 true.
 * - freq invariance 지원가능하면 return true.
 */
bool topology_scale_freq_invariant(void)
{
	return cpufreq_supports_freq_invariance() ||
	       supports_scale_freq_counters(cpu_online_mask);
}

/*
 * IAMROOT, 2022.12.10:
 * - TODO
 * - 현재 system freq invariance상태와 @status가 일치하다면 아무일도 안한다.
 * - 그게 아니라면 다시한번 topology_scale_freq_invariant()으로 
 *   현재 system freq invariance상태를 읽어 요청 @status와 일치한다면
 *   scale_freq_invariant를 @status로 갱신하고 rebuild_sched_domains_energy
 *   를 수행한다.
 */
static void update_scale_freq_invariant(bool status)
{
	if (scale_freq_invariant == status)
		return;

	/*
	 * Task scheduler behavior depends on frequency invariance support,
	 * either cpufreq or counter driven. If the support status changes as
	 * a result of counter initialisation and use, retrigger the build of
	 * scheduling domains to ensure the information is propagated properly.
	 */
	/*
	 * IAMROOT. 2022.12.10:
	 * - google-translate
	 *   작업 스케줄러 동작은 주파수 불변 지원(cpufreq 또는 카운터 기반)에 따라
	 *   달라집니다. 카운터 초기화 및 사용으로 인해 지원 상태가 변경되면 예약 도메인의
	 *   빌드를 다시 트리거하여 정보가 올바르게 전파되도록 합니다.
	 */
	if (topology_scale_freq_invariant() == status) {
		scale_freq_invariant = status;
		rebuild_sched_domains_energy();
	}
}

/*
 * IAMROOT, 2022.12.10:
 * -----------------
 *  - AMU, PMU(openai)
 *  In the context of ARM64 (also known as AArch64), PMU and AMU refer to 
 *  two different types of hardware performance counters that are used to 
 *  measure and monitor the performance of the system.
 *
 *  PMU stands for Performance Monitoring Unit, and it is a hardware 
 *  component that is used to count and measure various events and 
 *  activities on the system. For example, a PMU might be used to count 
 *  the number of instructions executed by the CPU, the number of cache 
 *  misses, or the number of memory accesses.
 *
 *  AMU stands for Architectural Monitoring Unit, and it is a similar 
 *  type of hardware performance counter. Like a PMU, an AMU can be used 
 *  to count and measure various events and activities on the system. 
 *  However, an AMU is typically more focused on measuring the performance 
 *  of specific instructions or operations, rather than general events.  
 *
 *  Both PMU and AMU are commonly used in ARM64 systems to monitor and 
 *  measure the performance of the system and its components. They can 
 *  provide valuable information to software developers and system 
 *  administrators, who can use this data to optimize the performance of 
 *  the system and diagnose any performance issues.
 *  -- papago
 *  ARM64(AArch64라고도 함)와 관련하여 PMU와 AMU는 시스템 성능을 측정하고 
 *  모니터링하는 데 사용되는 두 가지 유형의 하드웨어 성능 카운터를 나타냅니다.  
 *  PMU는 Performance Monitoring Unit의 약자로 시스템의 다양한 이벤트 및 
 *  활동을 계산하고 측정하는 데 사용되는 하드웨어 구성 요소입니다. 
 *  예를 들어, PMU는 CPU가 실행한 명령어 수, 캐시 미스 수 또는 메모리 
 *  액세스 수를 계산하는 데 사용될 수 있습니다. 
 *
 *  AMU는 Architectural Monitoring Unit의 약자이며 유사한 유형의 하드웨어 
 *  성능 카운터입니다. PMU와 마찬가지로 AMU를 사용하여 시스템의 다양한 
 *  이벤트와 활동을 계산하고 측정할 수 있습니다. 그러나 AMU는 일반적으로 
 *  일반적인 이벤트보다는 특정 명령 또는 작업의 성능을 측정하는 데 더 
 *  중점을 둡니다.  
 *
 *  PMU와 AMU는 일반적으로 ARM64 시스템에서 시스템과 해당 구성 요소의 
 *  성능을 모니터링하고 측정하는 데 사용됩니다. 그들은 이 데이터를 사용하여 
 *  시스템 성능을 최적화하고 성능 문제를 진단할 수 있는 소프트웨어 개발자 
 *  및 시스템 관리자에게 귀중한 정보를 제공할 수 있습니다.
 *
 * ------------------
 * - CPPC
 *   Documentation/admin-guide/acpi/cppc_sysfs.rst 참고.
 * - CPPC(openai)
 *  Collaborative Processor Performance Control (CPPC) is a power management 
 *  technology that allows multiple processors in a computer system to 
 *  coordinate their performance in order to optimize power usage. 
 *  It was developed by the Processor Power and Efficiency Working Group 
 *  (PPEWG), a consortium of leading computer and electronics companies, 
 *  including Intel, AMD, and ARM.  
 *
 *  CPPC works by allowing each processor in the system to communicate with 
 *  the others, sharing information about their current workloads and power 
 *  usage. This allows the processors to coordinate their performance 
 *  in order to balance the workload across the system and reduce overall 
 *  power consumption.
 *
 *  For example, if one processor is heavily loaded and using a lot of 
 *  power, while another processor is idle, CPPC can allow the idle 
 *  processor to power down or reduce its clock speed, while the busy 
 *  processor continues to run at full speed. This can save energy and 
 *  improve the overall power efficiency of the system. 
 *
 *  CPPC is designed to be transparent to the operating system and 
 *  applications, so it can work with any software that is designed to 
 *  run on multiple processors. It is supported by many modern processors, 
 *  and is often used in laptops, tablets, and other devices to improve 
 *  their battery life and power efficiency. 
 *
 * -- papago 
 *  CPPC(Collaborative Processor Performance Control)는 전원 사용을 
 *  최적화하기 위해 컴퓨터 시스템의 여러 프로세서가 성능을 조정할 수 있도록 
 *  하는 전원 관리 기술입니다. Intel, AMD 및 ARM을 포함한 주요 컴퓨터 및 
 *  전자 제품 회사의 컨소시엄인 PPEWG(Processor Power and Efficiency Working 
 *  Group)에서 개발했습니다.
 *
 *  CPPC는 시스템의 각 프로세서가 다른 프로세서와 통신할 수 있도록 하여 
 *  현재 워크로드 및 전력 사용량에 대한 정보를 공유합니다. 이를 통해 
 *  프로세서는 시스템 전체에서 워크로드의 균형을 유지하고 전체 전력 소비를 
 *  줄이기 위해 성능을 조정할 수 있습니다.
 *
 *  예를 들어, 한 프로세서가 과도하게 로드되어 많은 전력을 사용하고 
 *  다른 프로세서가 유휴 상태인 경우 CPPC는 유휴 프로세서의 전원을 끄거나 
 *  클록 속도를 낮추고 바쁜 프로세서는 계속 최고 속도로 실행되도록 할 수 
 *  있습니다. 이를 통해 에너지를 절약하고 시스템의 전체 전력 효율성을 
 *  향상시킬 수 있습니다.
 *
 *  CPPC는 운영 체제 및 응용 프로그램에 투명하도록 설계되었으므로 여러 
 *  프로세서에서 실행되도록 설계된 모든 소프트웨어에서 작동할 수 있습니다. 
 *  많은 최신 프로세서에서 지원되며 노트북, 태블릿 및 기타 장치에서 배터리 
 *  수명과 전력 효율성을 개선하기 위해 자주 사용됩니다.
 * ------------
 *
 * - arm64 amu fie driver나 Collaborative Processor Performance Control
 *   (CPPC) driver에 의해서 FIE(Frequency Invariant Engine) init이
 *   필요할경우 call된다.
 *
 * - 모든 cpu들의 pcpu sft_data가 SCALE_FREQ_SOURCE_ARCH로 등록이
 *   안되있다면 @data로 갱신한다.
 *   scale_freq_counters_mask 에 해당 cpus가 추가된다.
 */
void topology_set_scale_freq_source(struct scale_freq_data *data,
				    const struct cpumask *cpus)
{
	struct scale_freq_data *sfd;
	int cpu;

	/*
	 * Avoid calling rebuild_sched_domains() unnecessarily if FIE is
	 * supported by cpufreq.
	 */
/*
 * IAMROOT, 2022.12.14:
 * - scale_freq_counters_mask가 비어있다면 system상에서의 
 *   freq invariance를 지원하는지만 결국 검사할것이다.
 * - scale_freq_invariant는 update_scale_freq_invariant에서도 갱신을 
 *   하는데, 이때에는 rebuild까지 수행을 한다. 최초의 수행에서는
 *   rebuild가 필요없으므로 이렇게 scale_freq_invariant만 update한다.
 */
	if (cpumask_empty(&scale_freq_counters_mask))
		scale_freq_invariant = topology_scale_freq_invariant();

	rcu_read_lock();

/*
 * IAMROOT, 2022.12.14:
 * - @cpus들에 대해서 sfd가 등록이 안됬다면 data로 등록하면서 
 *   scale_freq_counters에 추가한다.
 */
	for_each_cpu(cpu, cpus) {
		sfd = rcu_dereference(*per_cpu_ptr(&sft_data, cpu));

		/* Use ARCH provided counters whenever possible */
		if (!sfd || sfd->source != SCALE_FREQ_SOURCE_ARCH) {
			rcu_assign_pointer(per_cpu(sft_data, cpu), data);
			cpumask_set_cpu(cpu, &scale_freq_counters_mask);
		}
	}

	rcu_read_unlock();

	update_scale_freq_invariant(true);
}
EXPORT_SYMBOL_GPL(topology_set_scale_freq_source);

void topology_clear_scale_freq_source(enum scale_freq_source source,
				      const struct cpumask *cpus)
{
	struct scale_freq_data *sfd;
	int cpu;

	rcu_read_lock();

	for_each_cpu(cpu, cpus) {
		sfd = rcu_dereference(*per_cpu_ptr(&sft_data, cpu));

		if (sfd && sfd->source == source) {
			rcu_assign_pointer(per_cpu(sft_data, cpu), NULL);
			cpumask_clear_cpu(cpu, &scale_freq_counters_mask);
		}
	}

	rcu_read_unlock();

	/*
	 * Make sure all references to previous sft_data are dropped to avoid
	 * use-after-free races.
	 */
	synchronize_rcu();

	update_scale_freq_invariant(false);
}
EXPORT_SYMBOL_GPL(topology_clear_scale_freq_source);

/*
 * IAMROOT, 2022.12.14:
 * - pcpu sft_data를 가져와 null이 아니라면,
 *   즉 scale freq invariant를 지원하는 cpu라면 set_freq_scale을 호출한다.
 */
void topology_scale_freq_tick(void)
{
	struct scale_freq_data *sfd = rcu_dereference_sched(*this_cpu_ptr(&sft_data));

	/*
	 * IAMROOT, 2022.12.10:
	 * - arm64 - amu_scale_freq_tick 호출
	 */
	if (sfd)
		sfd->set_freq_scale();
}

/*
 * IAMROOT, 2022.12.15:
 * - 현재 core의 freq scale을 나타낸다. 높을수록 빠르게 동작하고 있는것이며
 *   최고 1024 값이 된다.
 * - 최초에 dt를 통해 설정되며 후에는 runtime시
 *   amu_scale_freq_tick()을 통해 특정 주기에 따라 update된다.
 */
DEFINE_PER_CPU(unsigned long, arch_freq_scale) = SCHED_CAPACITY_SCALE;
EXPORT_PER_CPU_SYMBOL_GPL(arch_freq_scale);

void topology_set_freq_scale(const struct cpumask *cpus, unsigned long cur_freq,
			     unsigned long max_freq)
{
	unsigned long scale;
	int i;

	if (WARN_ON_ONCE(!cur_freq || !max_freq))
		return;

	/*
	 * If the use of counters for FIE is enabled, just return as we don't
	 * want to update the scale factor with information from CPUFREQ.
	 * Instead the scale factor will be updated from arch_scale_freq_tick.
	 */
/*
 * IAMROOT, 2022.12.17: 
 * 최근 cpu들은 절전 기능을 위해 freq를 변화시킨다.
 * 이러한 기능이 없는 cpu들은 그냥 return한다.
 */
	if (supports_scale_freq_counters(cpus))
		return;

/*
 * IAMROOT, 2022.12.19:
 * - max_freq를 기준으로 1024로 정규화 
 */
	scale = (cur_freq << SCHED_CAPACITY_SHIFT) / max_freq;

	for_each_cpu(i, cpus)
		per_cpu(arch_freq_scale, i) = scale;
}

DEFINE_PER_CPU(unsigned long, cpu_scale) = SCHED_CAPACITY_SCALE;
EXPORT_PER_CPU_SYMBOL_GPL(cpu_scale);

/*
 * IAMROOT, 2022.12.14:
 * - pcp cpu_scale에 @cpu자리로 @capacity를 설정한다.
 */
void topology_set_cpu_scale(unsigned int cpu, unsigned long capacity)
{
	per_cpu(cpu_scale, cpu) = capacity;
}

DEFINE_PER_CPU(unsigned long, thermal_pressure);

void topology_set_thermal_pressure(const struct cpumask *cpus,
			       unsigned long th_pressure)
{
	int cpu;

	for_each_cpu(cpu, cpus)
		WRITE_ONCE(per_cpu(thermal_pressure, cpu), th_pressure);
}
EXPORT_SYMBOL_GPL(topology_set_thermal_pressure);

static ssize_t cpu_capacity_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct cpu *cpu = container_of(dev, struct cpu, dev);

	return sysfs_emit(buf, "%lu\n", topology_get_cpu_scale(cpu->dev.id));
}

static void update_topology_flags_workfn(struct work_struct *work);
static DECLARE_WORK(update_topology_flags_work, update_topology_flags_workfn);

static DEVICE_ATTR_RO(cpu_capacity);

static int register_cpu_capacity_sysctl(void)
{
	int i;
	struct device *cpu;

	for_each_possible_cpu(i) {
		cpu = get_cpu_device(i);
		if (!cpu) {
			pr_err("%s: too early to get CPU%d device!\n",
			       __func__, i);
			continue;
		}
		device_create_file(cpu, &dev_attr_cpu_capacity);
	}

	return 0;
}
subsys_initcall(register_cpu_capacity_sysctl);

static int update_topology;

int topology_update_cpu_topology(void)
{
	return update_topology;
}

/*
 * Updating the sched_domains can't be done directly from cpufreq callbacks
 * due to locking, so queue the work for later.
 */
static void update_topology_flags_workfn(struct work_struct *work)
{
	update_topology = 1;
	rebuild_sched_domains();
	pr_debug("sched_domain hierarchy rebuilt, flags updated\n");
	update_topology = 0;
}

/*
 * IAMROOT, 2022.12.14:
 * - khz단위 cpu clock. topology_parse_cpu_capacity()에서 초기화된다.
 */
static DEFINE_PER_CPU(u32, freq_factor) = 1;
/*
 * IAMROOT, 2022.12.10:
 * - raw_capacity 의 cpu 배열에는 device tree에서 읽은 값이 들어간다
 *   ex. msm8998.dtsi
 *   Little CPU. capacity-dmips-mhz = <1024>
 *   BIG CPU. capacity-dmips-mhz = <1536>;
 */
static u32 *raw_capacity;

static int free_raw_capacity(void)
{
	kfree(raw_capacity);
	raw_capacity = NULL;

	return 0;
}

/*
 * IAMROOT, 2022.12.10:
 * -  Little CPU. capacity-dmips-mhz = <1024>, 1.8G
 *    BIG CPU. capacity-dmips-mhz = <1536>, 2.8 G
 *    (1024 * 1.8)/(1536 * 2.8) * 1024 = 438.85714285714295 = 438 <- Little
 *    (1536 * 2.8)/(1536 * 2.8) * 1024 = 1024 <- Big
 *    cat /sys/devices/system/cpu/cpu0/cpu_capacity
 *    가장 성능이 높은 cpu를 1024로 설정하고 이를 기준으로 나머지 cpu를 계산한다.
 */
void topology_normalize_cpu_scale(void)
{
	u64 capacity;
	u64 capacity_scale;
	int cpu;

	if (!raw_capacity)
		return;

	capacity_scale = 1;
/*
 * IAMROOT, 2022.12.14:
 * - raw_capacity * freq_factor의 값이 제일 큰것을 한개 고른다 .
 */
	for_each_possible_cpu(cpu) {
		capacity = raw_capacity[cpu] * per_cpu(freq_factor, cpu);
		capacity_scale = max(capacity, capacity_scale);
	}

	pr_debug("cpu_capacity: capacity_scale=%llu\n", capacity_scale);
	for_each_possible_cpu(cpu) {
		capacity = raw_capacity[cpu] * per_cpu(freq_factor, cpu);
/*
 * IAMROOT, 2022.12.14:
 * - value = (x * 1024) / max)의 연산을 수행한다.
 *   즉 max값을 기준으로 모든값들이 1024로 정규화된다.
 */
		capacity = div64_u64(capacity << SCHED_CAPACITY_SHIFT,
			capacity_scale);
		topology_set_cpu_scale(cpu, capacity);
		pr_debug("cpu_capacity: CPU%d cpu_capacity=%lu\n",
			cpu, topology_get_cpu_scale(cpu));
	}
}
/*
 * IAMROOT, 2022.12.10:
 * - rk3399.dtsi
 *              cpu_l1: cpu@1 {
 *			compatible = "arm,cortex-a53";
 *			capacity-dmips-mhz = <485>;
 *			...
 *			dynamic-power-coefficient = <100>;
 *		};
 *
 *		cpu_b0: cpu@100 {
 *			compatible = "arm,cortex-a72";
 *			capacity-dmips-mhz = <1024>;
 *			...
 *			dynamic-power-coefficient = <436>;
 *		};
 * - msm8993.dtsi
 *		CPU0: cpu@0 {
 *			compatible = "qcom,kryo280";
 *			capacity-dmips-mhz = <1024>;
 *			...
 *		};
 *		CPU4: cpu@100 {
 *			compatible = "qcom,kryo280";
 *			capacity-dmips-mhz = <1536>;
 *			...
 *		};
 *
 * - raw_capacity, pcpu freq_factor를 초기화한다.
 */
bool __init topology_parse_cpu_capacity(struct device_node *cpu_node, int cpu)
{
	struct clk *cpu_clk;
	static bool cap_parsing_failed;
	int ret;
	u32 cpu_capacity;

	if (cap_parsing_failed)
		return false;

	ret = of_property_read_u32(cpu_node, "capacity-dmips-mhz",
				   &cpu_capacity);
	if (!ret) {
		if (!raw_capacity) {
			raw_capacity = kcalloc(num_possible_cpus(),
					       sizeof(*raw_capacity),
					       GFP_KERNEL);
			if (!raw_capacity) {
				cap_parsing_failed = true;
				return false;
			}
		}
		raw_capacity[cpu] = cpu_capacity;
		pr_debug("cpu_capacity: %pOF cpu_capacity=%u (raw)\n",
			cpu_node, raw_capacity[cpu]);

		/*
		 * Update freq_factor for calculating early boot cpu capacities.
		 * For non-clk CPU DVFS mechanism, there's no way to get the
		 * frequency value now, assuming they are running at the same
		 * frequency (by keeping the initial freq_factor value).
		 */
		/*
		 * IAMROOT. 2022.12.10:
		 * - google-translate
		 *   초기 부팅 CPU 용량을 계산하기 위해 freq_factor를 업데이트합니다.
		 *   non-clk CPU DVFS 메커니즘의 경우, 동일한  주파수에서 실행한다고
		 *   가정하면(초기 freq_factor 값을 유지하여) 주파수 값을 얻을 수 있는
		 *   방법이 없습니다.
		 */
		cpu_clk = of_clk_get(cpu_node, 0);
		if (!PTR_ERR_OR_ZERO(cpu_clk)) {
			per_cpu(freq_factor, cpu) =
				clk_get_rate(cpu_clk) / 1000;
			clk_put(cpu_clk);
		}
	} else {
		if (raw_capacity) {
			pr_err("cpu_capacity: missing %pOF raw capacity\n",
				cpu_node);
			pr_err("cpu_capacity: partial information: fallback to 1024 for all CPUs\n");
		}
		cap_parsing_failed = true;
		free_raw_capacity();
	}

	return !ret;
}

#ifdef CONFIG_CPU_FREQ
static cpumask_var_t cpus_to_visit;
static void parsing_done_workfn(struct work_struct *work);
static DECLARE_WORK(parsing_done_work, parsing_done_workfn);

static int
init_cpu_capacity_callback(struct notifier_block *nb,
			   unsigned long val,
			   void *data)
{
	struct cpufreq_policy *policy = data;
	int cpu;

	if (!raw_capacity)
		return 0;

	if (val != CPUFREQ_CREATE_POLICY)
		return 0;

	pr_debug("cpu_capacity: init cpu capacity for CPUs [%*pbl] (to_visit=%*pbl)\n",
		 cpumask_pr_args(policy->related_cpus),
		 cpumask_pr_args(cpus_to_visit));

	cpumask_andnot(cpus_to_visit, cpus_to_visit, policy->related_cpus);

	for_each_cpu(cpu, policy->related_cpus)
		per_cpu(freq_factor, cpu) = policy->cpuinfo.max_freq / 1000;

	if (cpumask_empty(cpus_to_visit)) {
		topology_normalize_cpu_scale();
		schedule_work(&update_topology_flags_work);
		free_raw_capacity();
		pr_debug("cpu_capacity: parsing done\n");
		schedule_work(&parsing_done_work);
	}

	return 0;
}

static struct notifier_block init_cpu_capacity_notifier = {
	.notifier_call = init_cpu_capacity_callback,
};

static int __init register_cpufreq_notifier(void)
{
	int ret;

	/*
	 * on ACPI-based systems we need to use the default cpu capacity
	 * until we have the necessary code to parse the cpu capacity, so
	 * skip registering cpufreq notifier.
	 */
	if (!acpi_disabled || !raw_capacity)
		return -EINVAL;

	if (!alloc_cpumask_var(&cpus_to_visit, GFP_KERNEL))
		return -ENOMEM;

	cpumask_copy(cpus_to_visit, cpu_possible_mask);

	ret = cpufreq_register_notifier(&init_cpu_capacity_notifier,
					CPUFREQ_POLICY_NOTIFIER);

	if (ret)
		free_cpumask_var(cpus_to_visit);

	return ret;
}
core_initcall(register_cpufreq_notifier);

static void parsing_done_workfn(struct work_struct *work)
{
	cpufreq_unregister_notifier(&init_cpu_capacity_notifier,
					 CPUFREQ_POLICY_NOTIFIER);
	free_cpumask_var(cpus_to_visit);
}

#else
core_initcall(free_raw_capacity);
#endif

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)
/*
 * This function returns the logic cpu number of the node.
 * There are basically three kinds of return values:
 * (1) logic cpu number which is > 0.
 * (2) -ENODEV when the device tree(DT) node is valid and found in the DT but
 * there is no possible logical CPU in the kernel to match. This happens
 * when CONFIG_NR_CPUS is configure to be smaller than the number of
 * CPU nodes in DT. We need to just ignore this case.
 * (3) -1 if the node does not exist in the device tree
 */
/*
 * IAMROOT, 2022.12.14:
 * - ex) cpu = <&A53_0>; 의 형식으로 되있을것이다.
 *   &A53_0에 대한 cpu_node를 가져오고 해당 cpu번호를 가져와서 
 *   topology_parse_cpu_capacity()에서 raw_capacity, pcp freq_factor를
 *   설정한다.
 */
static int __init get_cpu_for_node(struct device_node *node)
{
	struct device_node *cpu_node;
	int cpu;

	cpu_node = of_parse_phandle(node, "cpu", 0);
	if (!cpu_node)
		return -1;

	cpu = of_cpu_node_to_id(cpu_node);
	if (cpu >= 0)
		topology_parse_cpu_capacity(cpu_node, cpu);
	else
		pr_info("CPU node for %pOF exist but the possible cpu range is :%*pbl\n",
			cpu_node, cpumask_pr_args(cpu_possible_mask));

	of_node_put(cpu_node);
	return cpu;
}

/*
 * IAMROOT, 2022.12.14:
 * - 	cpus {
 *		#address-cells = <2>;
 *		#size-cells = <0>;
 *
 *		cpu-map {
 *			cluster0 {
 *				core0 {
 *					cpu = <&A53_0>;
 *				};
 *				..
 *			};
 *
 *			cluster1 {
 *				core0 {
 *					cpu = <&A72_0>;
 *				};
 *				..
 *			};
 *		};
 *
 * 의 구조에서 cluster를 iterate하면서 coreX, cpu에 대한 phandle을 parsing
 * 하는 get_cpu_for_node()에서 raw_capacity, pcp freq_factor를 초기화
 * 하면서 cpu_topology를 초기화한다.
 */
static int __init parse_core(struct device_node *core, int package_id,
			     int core_id)
{
	char name[20];
	bool leaf = true;
	int i = 0;
	int cpu;
	struct device_node *t;

	do {
		snprintf(name, sizeof(name), "thread%d", i);
		t = of_get_child_by_name(core, name);
		if (t) {
			leaf = false;
			cpu = get_cpu_for_node(t);
			if (cpu >= 0) {
				cpu_topology[cpu].package_id = package_id;
				cpu_topology[cpu].core_id = core_id;
				cpu_topology[cpu].thread_id = i;
			} else if (cpu != -ENODEV) {
				pr_err("%pOF: Can't get CPU for thread\n", t);
				of_node_put(t);
				return -EINVAL;
			}
			of_node_put(t);
		}
		i++;
	} while (t);

	cpu = get_cpu_for_node(core);
	if (cpu >= 0) {
		if (!leaf) {
			pr_err("%pOF: Core has both threads and CPU\n",
			       core);
			return -EINVAL;
		}

		cpu_topology[cpu].package_id = package_id;
		cpu_topology[cpu].core_id = core_id;
	} else if (leaf && cpu != -ENODEV) {
		pr_err("%pOF: Can't get CPU for leaf core\n", core);
		return -EINVAL;
	}

	return 0;
}

/*
 * IAMROOT, 2022.12.14:
 * - ex) parse_dt_topology에서 불러진경우
 *   	cpus {
 *		#address-cells = <2>;
 *		#size-cells = <0>;
 *
 *		cpu-map {
 *			cluster0 {
 *				core0 {
 *					cpu = <&A53_0>;
 *				};
 *				..
 *			};
 *
 *			cluster1 {
 *			..
 *			};
 *		};
 *	  };
 *	의 구조로 되있을거고, cluster0, core0, cpu을 이함수를 통해 parsing
 *	할것이다.
 *	- raw_capacity, pcp freq_factor cpu_topology를 초기화한다.
 */
static int __init parse_cluster(struct device_node *cluster, int depth)
{
	char name[20];
	bool leaf = true;
	bool has_cores = false;
	struct device_node *c;
	static int package_id __initdata;
	int core_id = 0;
	int i, ret;

	/*
	 * First check for child clusters; we currently ignore any
	 * information about the nesting of clusters and present the
	 * scheduler with a flat list of them.
	 */
	i = 0;
/*
 * IAMROOT, 2022.12.14:
 * - clusterX에 대한 자료구조를 만든다.
 */
	do {
		snprintf(name, sizeof(name), "cluster%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			leaf = false;
			ret = parse_cluster(c, depth + 1);
			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	/* Now check for cores */
	i = 0;
/*
 * IAMROOT, 2022.12.14:
 * - coreX를 parsing한다. 
 */
	do {
		snprintf(name, sizeof(name), "core%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			has_cores = true;

			if (depth == 0) {
				pr_err("%pOF: cpu-map children should be clusters\n",
				       c);
				of_node_put(c);
				return -EINVAL;
			}

			if (leaf) {
				ret = parse_core(c, package_id, core_id++);
			} else {
				pr_err("%pOF: Non-leaf cluster with core %s\n",
				       cluster, name);
				ret = -EINVAL;
			}

			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	if (leaf && !has_cores)
		pr_warn("%pOF: empty cluster\n", cluster);

	if (leaf)
		package_id++;

	return 0;
}
/*
 * IAMROOT, 2022.12.10:
 * - 	cpus {
 *		#address-cells = <2>;
 *		#size-cells = <0>;
 *
 *		cpu-map {
 *			cluster0 {
 *				core0 {
 *					cpu = <&A53_0>;
 *				};
 *				core1 {
 *					cpu = <&A53_1>;
 *				};
 *				core2 {
 *					cpu = <&A53_2>;
 *				};
 *				core3 {
 *					cpu = <&A53_3>;
 *				};
 *			};
 *
 *			cluster1 {
 *				core0 {
 *					cpu = <&A72_0>;
 *				};
 *				core1 {
 *					cpu = <&A72_1>;
 *				};
 *			};
 *		};
 *
 * 1. cpu별 cpu_topology를 초기화한다.
 * 2. cpu별 raw_capacity, pcp freq_factor를 설정한다.
 * 3. 초기화한 raw_capacity를 가지고 kenrel 에서 사용하는 scale로
 *    normalize하여 cpu_scale 을 초기화한다.
 */
static int __init parse_dt_topology(void)
{
	struct device_node *cn, *map;
	int ret = 0;
	int cpu;

/*
 * IAMROOT, 2022.12.14:
 * - cpus {...} 를 찾는다.
 */
	cn = of_find_node_by_path("/cpus");
	if (!cn) {
		pr_err("No CPU information found in DT\n");
		return 0;
	}

	/*
	 * When topology is provided cpu-map is essentially a root
	 * cluster with restricted subnodes.
	 */
/*
 * IAMROOT, 2022.12.14:
 * - cpus {
 *     cpu-map { ... } < -- 찾는다.
 *  }
 */
	map = of_get_child_by_name(cn, "cpu-map");
	if (!map)
		goto out;

/*
 * IAMROOT, 2022.12.14:
 * - cpus {
 *     cpu-map {
 *			cluster0 { ..} < -- 이부분을 찾아서 안에 내용을 parsing한다.
 *     }
 *	} 
 *
 *	parsing하면서 coreN의 cpu = <..> 내용을 cpu_topology 전역변수에 옮긴다.
 *	(parse_core)참고
 */
	ret = parse_cluster(map, 0);
	if (ret != 0)
		goto out_map;

	topology_normalize_cpu_scale();

	/*
	 * Check that all cores are in the topology; the SMP code will
	 * only mark cores described in the DT as possible.
	 */
	for_each_possible_cpu(cpu)
		if (cpu_topology[cpu].package_id == -1)
			ret = -EINVAL;

out_map:
	of_node_put(map);
out:
	of_node_put(cn);
	return ret;
}
#endif

/*
 * cpu topology table
 */
struct cpu_topology cpu_topology[NR_CPUS];
EXPORT_SYMBOL_GPL(cpu_topology);

/*
 * IAMROOT, 2023.04.15:
 * - @cpu가 속한 node에서 시작하여 core_sibling, llc_sibling이 각각 포함되있는지 
 *  검사해 해당 cpu-list가 전부 속해있다면 해당 cpu-list로 대체해서 return한다.
 * - arm64기준으로는 같은 cluster 내의 core cpumask.
 */
const struct cpumask *cpu_coregroup_mask(int cpu)
{
	const cpumask_t *core_mask = cpumask_of_node(cpu_to_node(cpu));

	/* Find the smaller of NUMA, core or LLC siblings */
/*
 * IAMROOT, 2023.04.15:
 * - sibling이 core_mask에 다 포함되있는거면 sibling으로 대체한다.
 */
	if (cpumask_subset(&cpu_topology[cpu].core_sibling, core_mask)) {
		/* not numa in package, lets use the package siblings */
		core_mask = &cpu_topology[cpu].core_sibling;
	}

/*
 * IAMROOT, 2023.04.15:
 * - llc(last level cache)가 있는경우 llc_sibling이 완전히 core_mask에 포함되있는지 검사한다.
 *   포함되면 llc_sibling으로 대체한다.
 */
	if (cpu_topology[cpu].llc_id != -1) {
		if (cpumask_subset(&cpu_topology[cpu].llc_sibling, core_mask))
			core_mask = &cpu_topology[cpu].llc_sibling;
	}

	return core_mask;
}

void update_siblings_masks(unsigned int cpuid)
{
	struct cpu_topology *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];
	int cpu;

	/* update core and thread sibling masks */
	for_each_online_cpu(cpu) {
		cpu_topo = &cpu_topology[cpu];

		if (cpuid_topo->llc_id == cpu_topo->llc_id) {
			cpumask_set_cpu(cpu, &cpuid_topo->llc_sibling);
			cpumask_set_cpu(cpuid, &cpu_topo->llc_sibling);
		}

		if (cpuid_topo->package_id != cpu_topo->package_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->core_sibling);
		cpumask_set_cpu(cpu, &cpuid_topo->core_sibling);

		if (cpuid_topo->core_id != cpu_topo->core_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->thread_sibling);
		cpumask_set_cpu(cpu, &cpuid_topo->thread_sibling);
	}
}

static void clear_cpu_topology(int cpu)
{
	struct cpu_topology *cpu_topo = &cpu_topology[cpu];

	cpumask_clear(&cpu_topo->llc_sibling);
	cpumask_set_cpu(cpu, &cpu_topo->llc_sibling);

	cpumask_clear(&cpu_topo->core_sibling);
	cpumask_set_cpu(cpu, &cpu_topo->core_sibling);
	cpumask_clear(&cpu_topo->thread_sibling);
	cpumask_set_cpu(cpu, &cpu_topo->thread_sibling);
}

void __init reset_cpu_topology(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct cpu_topology *cpu_topo = &cpu_topology[cpu];

		cpu_topo->thread_id = -1;
		cpu_topo->core_id = -1;
		cpu_topo->package_id = -1;
		cpu_topo->llc_id = -1;

		clear_cpu_topology(cpu);
	}
}

void remove_cpu_topology(unsigned int cpu)
{
	int sibling;

	for_each_cpu(sibling, topology_core_cpumask(cpu))
		cpumask_clear_cpu(cpu, topology_core_cpumask(sibling));
	for_each_cpu(sibling, topology_sibling_cpumask(cpu))
		cpumask_clear_cpu(cpu, topology_sibling_cpumask(sibling));
	for_each_cpu(sibling, topology_llc_cpumask(cpu))
		cpumask_clear_cpu(cpu, topology_llc_cpumask(sibling));

	clear_cpu_topology(cpu);
}

__weak int __init parse_acpi_topology(void)
{
	return 0;
}

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)
void __init init_cpu_topology(void)
{
	reset_cpu_topology();

	/*
	 * Discard anything that was parsed if we hit an error so we
	 * don't use partial information.
	 */
	if (parse_acpi_topology())
		reset_cpu_topology();
	else if (of_have_populated_dt() && parse_dt_topology())
		reset_cpu_topology();
}
#endif
