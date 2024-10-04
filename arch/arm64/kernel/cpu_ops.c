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

/* IAMROOT, 2022.01.02:
 * - acpi_disabled == true : dt_supported_cpu_ops
 *   acpi_disabled == false: acpi_supported_cpu_ops
 *
 *   init_cpu_ops 에서 초기화된다.
 */
static const struct cpu_operations *cpu_ops[NR_CPUS] __ro_after_init;

/* IAMROOT, 2024.10.04:
 * - dt support 시 'spin-table' and 'psci' 방식을 지원한다.
 */
static const struct cpu_operations *const dt_supported_cpu_ops[] __initconst = {
	&smp_spin_table_ops,
	&cpu_psci_ops,
	NULL,
};

/* IAMROOT, 2024.10.04:
 * - acpi support 시 'parking protocol' and 'psci' 방식을 지원한다.
 */
static const struct cpu_operations *const acpi_supported_cpu_ops[] __initconst = {
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
	&acpi_parking_protocol_ops,
#endif
	&cpu_psci_ops,
	NULL,
};

/* IAMROOT, 2024.10.04:
 * - @name(enable_method)에 해당하는 cpu_ops를 찾아서 반환한다.
 */
static const struct cpu_operations * __init cpu_get_ops(const char *name)
{
	const struct cpu_operations *const *ops;

	/* IAMROOT, 2024.10.04:
	 * - acpi_disabled가 true인 경우 dt_supported_cpu_ops를 사용한다.
	 */
	ops = acpi_disabled ? dt_supported_cpu_ops : acpi_supported_cpu_ops;

	while (*ops) {
		if (!strcmp(name, (*ops)->name))
			return *ops;

		ops++;
	}

	return NULL;
}

/* IAMROOT, 2022.01.02:
 * - @cpu 번호에 해당하는 cpu의 enable-method를 읽어온다.
 *   (dt 또는 acpi)
 */
static const char *__init cpu_read_enable_method(int cpu)
{
	const char *enable_method;

	/* IAMROOT, 2024.10.03:
	 * - acpi_disabled가 true인 경우 dt에서 enable-method를 읽어온다.
	 */
	if (acpi_disabled) {
		struct device_node *dn = of_get_cpu_node(cpu, NULL);

		if (!dn) {
			if (!cpu)
				pr_err("Failed to find device node for boot cpu\n");
			return NULL;
		}

		/* IAMROOT, 2024.10.03:
		 * - dt의 'enable-method' property에서 값을 읽어 오며
		 *   'spin-table' or 'psci' 둘중 하나 이다.
		 *
		 *   spin-table:
		 *       system booting시 secondary cpu 각각 마다 지속적으로 모니터링
		 *       하는 table이 존재한다. 해당 table에는 booting 시작 여부등이
		 *       존재하는데 각각의 cpu들은 해당 값을 spin하며 모니터링하다가
		 *       set되면 동작하는 구조이다. 부팅시 boot cpu가 secondary cpu
		 *       각각의 spin table을 조작해서 secondary cpu를 제어한다.
		 *
		 *       hvc, smc 등 exception call을 통해 상위 level에 cpu on/off를
		 *       알린다.
		 *
		 *       legacy system에서는 el1이 직접 spin table을 제어했지만
		 *       보안상의 이유로 el2나 el3가 spin table을 제어한다.
		 */
		enable_method = of_get_property(dn, "enable-method", NULL);

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
		/* IAMROOT, 2024.10.04:
		 * - acpi라면 'psci' or 'parking-protocol' 방식을 사용한다.
		 */
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
/* IAMROOT, 2022.01.02:
 * - @cpu 번호에 해당하는 cpu의 enable-method를 읽어서 cpu_ops에 저장한다.
 */
int __init init_cpu_ops(int cpu)
{
	/* IAMROOT, 2022.01.01:
	 * - dt 또는 acpi에서 cpu의 enable-method를 읽어온다.
	 *
	 *   예) dt:
	 *   cpu#0 {
	 *       device_type = "cpu";
	 *       compatible = "arm,cortex-a72";
	 *       enable-method = "spin-table";   // or psci
	 *       ...
	 */
	const char *enable_method = cpu_read_enable_method(cpu);

	if (!enable_method)
		return -ENODEV;

	/* IAMROOT, 2024.10.04:
	 * - enable_method에 해당하는 ops를 찾아서 cpu_ops에 저장한다.
	 */
	cpu_ops[cpu] = cpu_get_ops(enable_method);
	if (!cpu_ops[cpu]) {
		pr_warn("Unsupported enable-method: %s\n", enable_method);
		return -EOPNOTSUPP;
	}

	return 0;
}

/* IAMROOT, 2024.10.04:
 * - @cpu 번호에 해당하는 cpu_ops를 반환한다.
 */
const struct cpu_operations *get_cpu_ops(int cpu)
{
	return cpu_ops[cpu];
}
