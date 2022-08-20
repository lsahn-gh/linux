// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/clocksource.h>

/*
 * IAMROOT, 2022.08.20:
 * - compile time에 등록된 timer들.
 *   ex) TIMER_OF_DECLARE(armv7_arch_timer, "arm,armv7-timer", arch_timer_of_init);
 */
extern struct of_device_id __timer_of_table[];

static const struct of_device_id __timer_of_table_sentinel
	__used __section("__timer_of_table_end");

/*
 * IAMROOT, 2022.08.20:
 * - 
 */
void __init timer_probe(void)
{
	struct device_node *np;
	const struct of_device_id *match;
	of_init_fn_1_ret init_func_ret;
	unsigned timers = 0;
	int ret;

/*
 * IAMROOT, 2022.08.20:
 * - compile time에 __timer_of_table에 등록된 driver들을 순회하며 dts와
 *   매칭하며 ok된것들에 대해서 init함수를 호출한다
 *   ex) TIMER_OF_DECLARE(armv7_arch_timer, "arm,armv7-timer", arch_timer_of_init);
 *   로 등록됬으면 arch_timer_of_init을 호출한다.
 *
 * ex) dts 예제
 *
 * timer {
 *	compatible = "arm,armv8-timer";
 *	interrupts = <GIC_PPI 13 IRQ_TYPE_LEVEL_LOW>, // Physical Secure
 *		     <GIC_PPI 14 IRQ_TYPE_LEVEL_LOW>, // Physical Non-Secure
 *		     <GIC_PPI 11 IRQ_TYPE_LEVEL_LOW>, // Virtual
 *		     <GIC_PPI 10 IRQ_TYPE_LEVEL_LOW>; // Hypervisor
 *	interrupt-parent = <&gic>;
 *	arm,no-tick-in-suspend;
 * };
 */
	for_each_matching_node_and_match(np, __timer_of_table, &match) {
		if (!of_device_is_available(np))
			continue;

		init_func_ret = match->data;

		ret = init_func_ret(np);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				pr_err("Failed to initialize '%pOF': %d\n", np,
				       ret);
			continue;
		}

		timers++;
	}

/*
 * IAMROOT, 2022.08.20:
 * - acpi 호출.
 */
	timers += acpi_probe_device_table(timer);

	if (!timers)
		pr_crit("%s: no matching timers found\n", __func__);
}
