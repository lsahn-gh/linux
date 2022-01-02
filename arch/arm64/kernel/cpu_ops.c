// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU kernel entry/exit control
 *
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/cache.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/string.h>
#include <asm/acpi.h>
#include <asm/cpu_ops.h>
#include <asm/smp_plat.h>

extern const struct cpu_operations smp_spin_table_ops;
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
extern const struct cpu_operations acpi_parking_protocol_ops;
#endif
extern const struct cpu_operations cpu_psci_ops;

/*
 * IAMROOT, 2022.01.02:
 * - init_cpu_ops 에서 초기화된다.
 * - acpi_disabled이면 dt_supported_cpu_ops,
 *   아닌경우에는 acpi_supported_cpu_ops을 참조한다.
 * - dt의 경우 dt에서 해당 cpu의 enable_method가 spin-table인지 psci방식인지 
 *   확인하여 해당 구조체로 매핑된다.
 */
static const struct cpu_operations *cpu_ops[NR_CPUS] __ro_after_init;

static const struct cpu_operations *const dt_supported_cpu_ops[] __initconst = {
	&smp_spin_table_ops,
	&cpu_psci_ops,
	NULL,
};

static const struct cpu_operations *const acpi_supported_cpu_ops[] __initconst = {
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
	&acpi_parking_protocol_ops,
#endif
	&cpu_psci_ops,
	NULL,
};

static const struct cpu_operations * __init cpu_get_ops(const char *name)
{
	const struct cpu_operations *const *ops;

	ops = acpi_disabled ? dt_supported_cpu_ops : acpi_supported_cpu_ops;

	while (*ops) {
		if (!strcmp(name, (*ops)->name))
			return *ops;

		ops++;
	}

	return NULL;
}

/*
 * IAMROOT, 2022.01.02:
 * - dt에서 cpu에 해당하는 node를 읽어오고 enable-method prop를 찾는다
 *   spin-table or psci 둘중 하나일것이며 없을 경우 warn 경고를 한다.
 * - secondary cpu가 spin-table 방식일 경우 boot cput인 경우 dt에서
 *   method가 없을수도 있다.
 */
static const char *__init cpu_read_enable_method(int cpu)
{
	const char *enable_method;

	if (acpi_disabled) {
		struct device_node *dn = of_get_cpu_node(cpu, NULL);

		if (!dn) {
			if (!cpu)
				pr_err("Failed to find device node for boot cpu\n");
			return NULL;
		}

		enable_method = of_get_property(dn, "enable-method", NULL);
/*
 * IAMROOT, 2022.01.02:
 * - spin table
 *   system booting시 secondary cpu 각각 마다 지속적으로 모니터링 하는
 *   table이 존재한다. 해당 table에는 booting 시작 여부등이 존재하는데
 *   각각의 cpu들은 해당 값을 spin하며 모니터링 하고있다가 set되면
 *   동작하게 되는 구조로 되있다.
 *   부팅시의 경우엔 boot cpu가 secondary cpu 각각의 spin table을 조작해서
 *   secondary cpu를 제어 할것이다.
 * - hvc, smc등을 통해서 상위로 cpu on, off등을 알린다.
 * - 옛날에는 el1이 직접 spin table을 제어했지만
 *   보안상의 이유로 el2나 el3가 spin table을 제어한다. 
 */
		if (!enable_method) {
			/*
			 * The boot CPU may not have an enable method (e.g.
			 * when spin-table is used for secondaries).
			 * Don't warn spuriously.
			 */
			if (cpu != 0)
				pr_err("%pOF: missing enable-method property\n",
					dn);
		}
		of_node_put(dn);
	} else {
		enable_method = acpi_get_enable_method(cpu);
		if (!enable_method) {
			/*
			 * In ACPI systems the boot CPU does not require
			 * checking the enable method since for some
			 * boot protocol (ie parking protocol) it need not
			 * be initialized. Don't warn spuriously.
			 */
			if (cpu != 0)
				pr_err("Unsupported ACPI enable-method\n");
		}
	}

	return enable_method;
}

/*
 * Read a cpu's enable method and record it in cpu_ops.
 */
/*
 * IAMROOT, 2022.01.02:
 * - 해당 cpu의 method 방식이 spin-table인지 psci인지 dt에서 읽어서
 *   해당 method ops를 매핑한다.
 */
int __init init_cpu_ops(int cpu)
{
/*
 * IAMROOT, 2022.01.01: 
 * 예)
 *    cpu#0 {
 *         device_type = "cpu";
 *         compatible = "arm,cortex-a72";
 *         enable-method = "spin-table";   // or psci
 *         ...
 */
	const char *enable_method = cpu_read_enable_method(cpu);

	if (!enable_method)
		return -ENODEV;

	cpu_ops[cpu] = cpu_get_ops(enable_method);
	if (!cpu_ops[cpu]) {
		pr_warn("Unsupported enable-method: %s\n", enable_method);
		return -EOPNOTSUPP;
	}

	return 0;
}

const struct cpu_operations *get_cpu_ops(int cpu)
{
	return cpu_ops[cpu];
}
