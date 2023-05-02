/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/arch_topology.h - arch specific cpu topology information
 */
#ifndef _LINUX_ARCH_TOPOLOGY_H_
#define _LINUX_ARCH_TOPOLOGY_H_

#include <linux/types.h>
#include <linux/percpu.h>

void topology_normalize_cpu_scale(void);
int topology_update_cpu_topology(void);

struct device_node;
bool topology_parse_cpu_capacity(struct device_node *cpu_node, int cpu);

/*
 * IAMROOT, 2022.12.17: 
 * 1) cpu_scale
 * HMP에 따른 성능을 cpu_scale에 저장한다.
 * 가장 고성능의 cpu=1024(1.0)을 기준으로 사용한다.
 * 이 값은 cpu가 online될 때 한 번 저장된다.
 */
DECLARE_PER_CPU(unsigned long, cpu_scale);

static inline unsigned long topology_get_cpu_scale(int cpu)
{
	return per_cpu(cpu_scale, cpu);
}

void topology_set_cpu_scale(unsigned int cpu, unsigned long capacity);

/*
 * IAMROOT, 2022.12.17: 
 * 2) arch_freq_scale
 * cpu의 freq 운용에 따른 성능을 arch_freq_scale에 저장한다.
 * 가장 높은 freq로 운영할 때 1024(1.0)을 기준으로 사용한다.
 * 이 값은 cpu의 freq가 변경될 때 마다 저장된다.
 */
DECLARE_PER_CPU(unsigned long, arch_freq_scale);

static inline unsigned long topology_get_freq_scale(int cpu)
{
	return per_cpu(arch_freq_scale, cpu);
}

void topology_set_freq_scale(const struct cpumask *cpus, unsigned long cur_freq,
			     unsigned long max_freq);
bool topology_scale_freq_invariant(void);

enum scale_freq_source {
	SCALE_FREQ_SOURCE_CPUFREQ = 0, /* driver */
	SCALE_FREQ_SOURCE_ARCH,
	SCALE_FREQ_SOURCE_CPPC,	/* powerpc */
};

struct scale_freq_data {
	enum scale_freq_source source;
	void (*set_freq_scale)(void);
};

void topology_scale_freq_tick(void);
void topology_set_scale_freq_source(struct scale_freq_data *data, const struct cpumask *cpus);
void topology_clear_scale_freq_source(enum scale_freq_source source, const struct cpumask *cpus);

DECLARE_PER_CPU(unsigned long, thermal_pressure);

static inline unsigned long topology_get_thermal_pressure(int cpu)
{
	return per_cpu(thermal_pressure, cpu);
}

void topology_set_thermal_pressure(const struct cpumask *cpus,
				   unsigned long th_pressure);

/*
 * IAMROOT, 2023.04.15:
 * - arm64는 smt가 현재 없다. thread_id와 thread_sibling은 설정이 안될것이다.
 */
struct cpu_topology {
	int thread_id;
	int core_id;
	int package_id;
	int llc_id;
	cpumask_t thread_sibling;
	cpumask_t core_sibling;

/*
 * IAMROOT, 2023.04.15:
 * - cache를 공유하는 list.
 *   last level cache
 */
	cpumask_t llc_sibling;
};

#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
extern struct cpu_topology cpu_topology[NR_CPUS];

#define topology_physical_package_id(cpu)	(cpu_topology[cpu].package_id)
#define topology_core_id(cpu)		(cpu_topology[cpu].core_id)
#define topology_core_cpumask(cpu)	(&cpu_topology[cpu].core_sibling)
#define topology_sibling_cpumask(cpu)	(&cpu_topology[cpu].thread_sibling)
#define topology_llc_cpumask(cpu)	(&cpu_topology[cpu].llc_sibling)
void init_cpu_topology(void);
void store_cpu_topology(unsigned int cpuid);
const struct cpumask *cpu_coregroup_mask(int cpu);
void update_siblings_masks(unsigned int cpu);
void remove_cpu_topology(unsigned int cpuid);
void reset_cpu_topology(void);
int parse_acpi_topology(void);
#endif

#endif /* _LINUX_ARCH_TOPOLOGY_H_ */
