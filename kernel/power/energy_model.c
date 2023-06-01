// SPDX-License-Identifier: GPL-2.0
/*
 * Energy Model of devices
 *
 * Copyright (c) 2018-2020, Arm ltd.
 * Written by: Quentin Perret, Arm ltd.
 * Improvements provided by: Lukasz Luba, Arm ltd.
 */

#define pr_fmt(fmt) "energy_model: " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/energy_model.h>
#include <linux/sched/topology.h>
#include <linux/slab.h>

/*
 * Mutex serializing the registrations of performance domains and letting
 * callbacks defined by drivers sleep.
 */
static DEFINE_MUTEX(em_pd_mutex);

static bool _is_cpu_device(struct device *dev)
{
	return (dev->bus == &cpu_subsys);
}

#ifdef CONFIG_DEBUG_FS
static struct dentry *rootdir;

static void em_debug_create_ps(struct em_perf_state *ps, struct dentry *pd)
{
	struct dentry *d;
	char name[24];

	snprintf(name, sizeof(name), "ps:%lu", ps->frequency);

	/* Create per-ps directory */
	d = debugfs_create_dir(name, pd);
	debugfs_create_ulong("frequency", 0444, d, &ps->frequency);
	debugfs_create_ulong("power", 0444, d, &ps->power);
	debugfs_create_ulong("cost", 0444, d, &ps->cost);
}

static int em_debug_cpus_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "%*pbl\n", cpumask_pr_args(to_cpumask(s->private)));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(em_debug_cpus);

static int em_debug_units_show(struct seq_file *s, void *unused)
{
	struct em_perf_domain *pd = s->private;
	char *units = pd->milliwatts ? "milliWatts" : "bogoWatts";

	seq_printf(s, "%s\n", units);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(em_debug_units);

static void em_debug_create_pd(struct device *dev)
{
	struct dentry *d;
	int i;

	/* Create the directory of the performance domain */
	d = debugfs_create_dir(dev_name(dev), rootdir);

	if (_is_cpu_device(dev))
		debugfs_create_file("cpus", 0444, d, dev->em_pd->cpus,
				    &em_debug_cpus_fops);

	debugfs_create_file("units", 0444, d, dev->em_pd, &em_debug_units_fops);

	/* Create a sub-directory for each performance state */
	for (i = 0; i < dev->em_pd->nr_perf_states; i++)
		em_debug_create_ps(&dev->em_pd->table[i], d);

}

static void em_debug_remove_pd(struct device *dev)
{
	struct dentry *debug_dir;

	debug_dir = debugfs_lookup(dev_name(dev), rootdir);
	debugfs_remove_recursive(debug_dir);
}

static int __init em_debug_init(void)
{
	/* Create /sys/kernel/debug/energy_model directory */
	rootdir = debugfs_create_dir("energy_model", NULL);

	return 0;
}
fs_initcall(em_debug_init);
#else /* CONFIG_DEBUG_FS */
static void em_debug_create_pd(struct device *dev) {}
static void em_debug_remove_pd(struct device *dev) {}
#endif

/*
 * IAMROOT, 2023.05.27:
 * - opp table 값을 읽어서 @pd의 table 을 할당하고 구성한다(cost 포함)
 */
static int em_create_perf_table(struct device *dev, struct em_perf_domain *pd,
				int nr_states, struct em_data_callback *cb)
{
	unsigned long opp_eff, prev_opp_eff = ULONG_MAX;
	unsigned long power, freq, prev_freq = 0;
	struct em_perf_state *table;
	int i, ret;
	u64 fmax;

	table = kcalloc(nr_states, sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	/* Build the list of performance states for this performance domain */
	for (i = 0, freq = 0; i < nr_states; i++, freq++) {
		/*
		 * active_power() is a driver callback which ceils 'freq' to
		 * lowest performance state of 'dev' above 'freq' and updates
		 * 'power' and 'freq' accordingly.
		 */
		/*
		 * IAMROOT. 2023.05.27:
		 * - google-translate
		 * active_power()는 'freq'를 'freq' 위의 'dev'의 가장 낮은 성능
		 * 상태로 제한하고 그에 따라 'power' 및 'freq'를 업데이트하는
		 * 드라이버 콜백입니다.
		 * - DT: _get_power callback 함수가 등록 되어 있다는
		 *   (dev_pm_opp_of_register_em() 참고)
		 * -  rk3399-opp.dtsi
		 * 		opp00 {
		 *	opp-hz = /bits/ 64 <408000000>;
		 *	opp-microvolt = <825000 825000 1250000>;
		 *	clock-latency-ns = <40000>;
		 *    };
		 *  - * rk3399.dtsi 리틀 코어
		 *		cpu_l3: cpu@3 {
		 *	device_type = "cpu";
		 *	compatible = "arm,cortex-a53";
		 *	reg = <0x0 0x3>;
		 *	enable-method = "psci";
		 *	capacity-dmips-mhz = <485>;
		 *	clocks = <&cru ARMCLKL>;
		 *	#cooling-cells = <2>;
		 *	dynamic-power-coefficient = <100>;
		 *	cpu-idle-states = <&CPU_SLEEP &CLUSTER_SLEEP>;
		 *      };
                 *
		 * - power 계산 예
		 *   100 * 825 * 825 * (408000000/1000000) =
		 *   27769500000 / 1000000000 = 27.7695 = 27
		 * - freq 계산 예
		 *   408000000 / 1000 = 408000
		 */
		ret = cb->active_power(&power, &freq, dev);
		if (ret) {
			dev_err(dev, "EM: invalid perf. state: %d\n",
				ret);
			goto free_ps_table;
		}

		/*
		 * We expect the driver callback to increase the frequency for
		 * higher performance states.
		 */
		/*
		 * IAMROOT. 2023.05.27:
		 * - google-translate
		 * 우리는 드라이버 콜백이 더 높은 성능 상태를 위한 빈도를 증가시킬 것으로
		 * 기대합니다.
		 */
		if (freq <= prev_freq) {
			dev_err(dev, "EM: non-increasing freq: %lu\n",
				freq);
			goto free_ps_table;
		}

		/*
		 * The power returned by active_state() is expected to be
		 * positive and to fit into 16 bits.
		 */
		/*
		 * IAMROOT. 2023.05.27:
		 * - google-translate
		 * active_state()에 의해 반환된 전력은 양수이고 16비트에 맞을 것으로
		 * 예상됩니다.
		 */
		if (!power || power > EM_MAX_POWER) {
			dev_err(dev, "EM: invalid power: %lu\n",
				power);
			goto free_ps_table;
		}

		table[i].power = power;
		table[i].frequency = prev_freq = freq;

		/*
		 * The hertz/watts efficiency ratio should decrease as the
		 * frequency grows on sane platforms. But this isn't always
		 * true in practice so warn the user if a higher OPP is more
		 * power efficient than a lower one.
		 */
		/*
		 * IAMROOT. 2023.05.27:
		 * - google-translate
		 * 정상적인 플랫폼에서 주파수가 증가함에 따라 헤르츠/와트 효율성 비율이
		 * 감소해야 합니다. 그러나 이것이 실제로는 항상 사실이 아니므로 높은
		 * OPP가 낮은 것보다 전력 효율이 더 높은 경우 사용자에게 경고하십시오.
		 */
		opp_eff = freq / power;
		if (opp_eff >= prev_opp_eff)
			dev_dbg(dev, "EM: hertz/watts ratio non-monotonically decreasing: em_perf_state %d >= em_perf_state%d\n",
					i, i - 1);
		prev_opp_eff = opp_eff;
	}

	/* Compute the cost of each performance state. */
	/*
	 * IAMROOT, 2023.05.27:
	 * - 	cluster0_opp: opp-table0 {
	 *	compatible = "operating-points-v2";
	 *	opp-shared;
	 *
	 *	opp00 {
	 *		opp-hz = /bits/ 64 <408000000>;
	 *		opp-microvolt = <825000 825000 1250000>;
	 *		clock-latency-ns = <40000>;
	 *	};
	 *	opp01 {
	 *		opp-hz = /bits/ 64 <600000000>;
	 *		opp-microvolt = <825000 825000 1250000>;
	 *	};
	 *	opp02 {
	 *		opp-hz = /bits/ 64 <816000000>;
	 *		opp-microvolt = <850000 850000 1250000>;
	 *	};
	 *	opp03 {
	 *		opp-hz = /bits/ 64 <1008000000>;
	 *		opp-microvolt = <925000 925000 1250000>;
	 *	};
	 *	opp04 {
	 *		opp-hz = /bits/ 64 <1200000000>;
	 *		opp-microvolt = <1000000 1000000 1250000>;
	 *	};
	 *	opp05 {
	 *		opp-hz = /bits/ 64 <1416000000>;
	 *		opp-microvolt = <1125000 1125000 1250000>;
	 *	};
	 *    };
	 *
	 * 위 dts의 경우 fmax = 1416000
	 */
	fmax = (u64) table[nr_states - 1].frequency;
	for (i = 0; i < nr_states; i++) {
		unsigned long power_res = em_scale_power(table[i].power);
			/*
			 * IAMROOT, 2023.05.27:
			 * - 1416000 * 27 / 408000 = 93.7058823529 = 93
			 */
			table[i].cost = div64_u64(fmax * power_res,
					  table[i].frequency);
	}

	pd->table = table;
	pd->nr_perf_states = nr_states;

	return 0;

free_ps_table:
	kfree(table);
	return -EINVAL;
}

/*
 * IAMROOT, 2023.05.27:
 * - empd 생성 과정
 * - dt_cpufreq_probe ->
 *   cpufreq_register_driver (cpufreq.c:2808) ->
 *   subsys_interface_register ->
 *   sif->add_dev(dev, sif); ->
 *   ----
 *   static struct subsys_interface cpufreq_interface = {
 *	.name		= "cpufreq",
 *	.subsys		= &cpu_subsys,
 *	.add_dev	= cpufreq_add_dev,
 *	.remove_dev	= cpufreq_remove_dev,
 *   };
 *   ----
 *   cpufreq_add_dev (cpufreq.c:1561) ->
 *   cpufreq_online ->
 *   cpufreq_register_em_with_opp ->
 *   dev_pm_opp_of_register_em ->
 *   em_dev_register_perf_domain ->
 *   em_create_pd
 */
static int em_create_pd(struct device *dev, int nr_states,
			struct em_data_callback *cb, cpumask_t *cpus)
{
	struct em_perf_domain *pd;
	struct device *cpu_dev;
	int cpu, ret;

	if (_is_cpu_device(dev)) {
		pd = kzalloc(sizeof(*pd) + cpumask_size(), GFP_KERNEL);
		if (!pd)
			return -ENOMEM;

		cpumask_copy(em_span_cpus(pd), cpus);
	} else {
		pd = kzalloc(sizeof(*pd), GFP_KERNEL);
		if (!pd)
			return -ENOMEM;
	}

	ret = em_create_perf_table(dev, pd, nr_states, cb);
	if (ret) {
		kfree(pd);
		return ret;
	}

	if (_is_cpu_device(dev))
		for_each_cpu(cpu, cpus) {
			cpu_dev = get_cpu_device(cpu);
			cpu_dev->em_pd = pd;
		}

	dev->em_pd = pd;

	return 0;
}

/**
 * em_pd_get() - Return the performance domain for a device
 * @dev : Device to find the performance domain for
 *
 * Returns the performance domain to which @dev belongs, or NULL if it doesn't
 * exist.
 */
/*
 * IAMROOT. 2023.05.27:
 * - google-translate
 * em_pd_get() - 장치의 성능 도메인 반환
 * @dev : 성능 도메인을 찾을 장치
 *
 * @dev가 속한 성능 도메인을 반환하거나 존재하지 않는 경우 NULL을 반환합니다.
 *
 * - @dev의 em_pd를 가져온다.
 */
struct em_perf_domain *em_pd_get(struct device *dev)
{
	if (IS_ERR_OR_NULL(dev))
		return NULL;

	return dev->em_pd;
}
EXPORT_SYMBOL_GPL(em_pd_get);

/**
 * em_cpu_get() - Return the performance domain for a CPU
 * @cpu : CPU to find the performance domain for
 *
 * Returns the performance domain to which @cpu belongs, or NULL if it doesn't
 * exist.
 */
/*
 * IAMROOT. 2023.05.27:
 * - google-translate
 * em_cpu_get() - CPU @cpu에 대한 성능 도메인 반환 :
 *
 * @cpu : CPU에 대한 성능 영역 찾기
 *
 * @cpu가 속한 성능 도메인을 반환하거나 존재하지 않는 경우 NULL을 반환합니다.
 *
 * - @cpu의 em_pd를 가져온다.
 * - em_pd
 *   dev_pm_opp_of_register_em()에 의해 등록될것이다.
 */
struct em_perf_domain *em_cpu_get(int cpu)
{
	struct device *cpu_dev;

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev)
		return NULL;

	return em_pd_get(cpu_dev);
}
EXPORT_SYMBOL_GPL(em_cpu_get);

/**
 * em_dev_register_perf_domain() - Register the Energy Model (EM) for a device
 * @dev		: Device for which the EM is to register
 * @nr_states	: Number of performance states to register
 * @cb		: Callback functions providing the data of the Energy Model
 * @cpus	: Pointer to cpumask_t, which in case of a CPU device is
 *		obligatory. It can be taken from i.e. 'policy->cpus'. For other
 *		type of devices this should be set to NULL.
 * @milliwatts	: Flag indicating that the power values are in milliWatts or
 *		in some other scale. It must be set properly.
 *
 * Create Energy Model tables for a performance domain using the callbacks
 * defined in cb.
 *
 * The @milliwatts is important to set with correct value. Some kernel
 * sub-systems might rely on this flag and check if all devices in the EM are
 * using the same scale.
 *
 * If multiple clients register the same performance domain, all but the first
 * registration will be ignored.
 *
 * Return 0 on success
 */
/*
 * IAMROOT, 2023.06.01:
 * @cb _get_power
 * - em_create_pd() 주석 참고. em_dev_register_perf_domain()를 부르는 
 *   함수는 dev_pm_opp_of_register_em().
 */
int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
				struct em_data_callback *cb, cpumask_t *cpus,
				bool milliwatts)
{
	unsigned long cap, prev_cap = 0;
	int cpu, ret;

	if (!dev || !nr_states || !cb)
		return -EINVAL;

	/*
	 * Use a mutex to serialize the registration of performance domains and
	 * let the driver-defined callback functions sleep.
	 */
	mutex_lock(&em_pd_mutex);

	if (dev->em_pd) {
		ret = -EEXIST;
		goto unlock;
	}

	if (_is_cpu_device(dev)) {
		if (!cpus) {
			dev_err(dev, "EM: invalid CPU mask\n");
			ret = -EINVAL;
			goto unlock;
		}

		for_each_cpu(cpu, cpus) {
			if (em_cpu_get(cpu)) {
				dev_err(dev, "EM: exists for CPU%d\n", cpu);
				ret = -EEXIST;
				goto unlock;
			}
			/*
			 * All CPUs of a domain must have the same
			 * micro-architecture since they all share the same
			 * table.
			 */
			cap = arch_scale_cpu_capacity(cpu);
			if (prev_cap && prev_cap != cap) {
				dev_err(dev, "EM: CPUs of %*pbl must have the same capacity\n",
					cpumask_pr_args(cpus));

				ret = -EINVAL;
				goto unlock;
			}
			prev_cap = cap;
		}
	}

	ret = em_create_pd(dev, nr_states, cb, cpus);
	if (ret)
		goto unlock;

	dev->em_pd->milliwatts = milliwatts;

	em_debug_create_pd(dev);
	dev_info(dev, "EM: created perf domain\n");

unlock:
	mutex_unlock(&em_pd_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(em_dev_register_perf_domain);

/**
 * em_dev_unregister_perf_domain() - Unregister Energy Model (EM) for a device
 * @dev		: Device for which the EM is registered
 *
 * Unregister the EM for the specified @dev (but not a CPU device).
 */
void em_dev_unregister_perf_domain(struct device *dev)
{
	if (IS_ERR_OR_NULL(dev) || !dev->em_pd)
		return;

	if (_is_cpu_device(dev))
		return;

	/*
	 * The mutex separates all register/unregister requests and protects
	 * from potential clean-up/setup issues in the debugfs directories.
	 * The debugfs directory name is the same as device's name.
	 */
	mutex_lock(&em_pd_mutex);
	em_debug_remove_pd(dev);

	kfree(dev->em_pd->table);
	kfree(dev->em_pd);
	dev->em_pd = NULL;
	mutex_unlock(&em_pd_mutex);
}
EXPORT_SYMBOL_GPL(em_dev_unregister_perf_domain);
