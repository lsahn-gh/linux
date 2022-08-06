// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 * Sylwester Nawrocki <s.nawrocki@samsung.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/clk-conf.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/printk.h>

/*
 * IAMROOT, 2022.08.06:
 * - default parent를 지정한다.
 * - ex)
 *
 *   &uart1 {
 *	pinctrl-names = "default";
 *	pinctrl-0 = <&pinctrl_uart1>;
 *	assigned-clocks = <&clk IMX8MM_CLK_UART1>;
 *	assigned-clock-parents = <&clk IMX8MM_SYS_PLL1_80M>;
 *	uart-has-rtscts;
 *	status = "okay";
 *	..
 *   }
 */
static int __set_clk_parents(struct device_node *node, bool clk_supplier)
{
	struct of_phandle_args clkspec;
	int index, rc, num_parents;
	struct clk *clk, *pclk;

/*
 * IAMROOT, 2022.08.06:
 * - assigned-clock-parents
 *   clk parents
 * - #clock-cells
 *   자신을 지정할때 사용해야되는 인자개수.
 * - ex)
 *   #clock-cells = <1>; <-- 인자개수 1개를 의미
 *   assigned-clock-parents = <&clk IMX8MM_SYS_PLL1_800M>, <&clk IMX8MM_ARM_PLL_OUT>
 *                                   ^인자 1개 있음.             ^인자 1개 있음
 *   num_parents의 개수는 2개.
 */
	num_parents = of_count_phandle_with_args(node, "assigned-clock-parents",
						 "#clock-cells");
	if (num_parents == -EINVAL)
		pr_err("clk: invalid value of clock-parents property at %pOF\n",
		       node);

/*
 * IAMROOT, 2022.08.06:
 * - 위에서 구해온 clk parents을 iteration.
 */
	for (index = 0; index < num_parents; index++) {
/*
 * IAMROOT, 2022.08.06:
 * - iterator를 돌며 index에 해당하는
 *   assigned-clock-parents, assigned-clocks를 가져오고 각각 provider로 등록되있는지
 *   확인한다.
 *   둘다 등록되있다면 assigned-clock-parent를 provider로, assigned-clocks를 consumer로
 *   하여 parent등록을한다.
 */
		rc = of_parse_phandle_with_args(node, "assigned-clock-parents",
					"#clock-cells",	index, &clkspec);
		if (rc < 0) {
			/* skip empty (null) phandles */
			if (rc == -ENOENT)
				continue;
			else
				return rc;
		}
		if (clkspec.np == node && !clk_supplier)
			return 0;
		pclk = of_clk_get_from_provider(&clkspec);
		if (IS_ERR(pclk)) {
			if (PTR_ERR(pclk) != -EPROBE_DEFER)
				pr_warn("clk: couldn't get parent clock %d for %pOF\n",
					index, node);
			return PTR_ERR(pclk);
		}

		rc = of_parse_phandle_with_args(node, "assigned-clocks",
					"#clock-cells", index, &clkspec);
		if (rc < 0)
			goto err;
		if (clkspec.np == node && !clk_supplier) {
			rc = 0;
			goto err;
		}
		clk = of_clk_get_from_provider(&clkspec);
		if (IS_ERR(clk)) {
			if (PTR_ERR(clk) != -EPROBE_DEFER)
				pr_warn("clk: couldn't get assigned clock %d for %pOF\n",
					index, node);
			rc = PTR_ERR(clk);
			goto err;
		}

		rc = clk_set_parent(clk, pclk);
		if (rc < 0)
			pr_err("clk: failed to reparent %s to %s: %d\n",
			       __clk_get_name(clk), __clk_get_name(pclk), rc);
		clk_put(clk);
		clk_put(pclk);
	}
	return 0;
err:
	clk_put(pclk);
	return rc;
}

static int __set_clk_rates(struct device_node *node, bool clk_supplier)
{
	struct of_phandle_args clkspec;
	struct property	*prop;
	const __be32 *cur;
	int rc, index = 0;
	struct clk *clk;
	u32 rate;

	of_property_for_each_u32(node, "assigned-clock-rates", prop, cur, rate) {
		if (rate) {
			rc = of_parse_phandle_with_args(node, "assigned-clocks",
					"#clock-cells",	index, &clkspec);
			if (rc < 0) {
				/* skip empty (null) phandles */
				if (rc == -ENOENT)
					continue;
				else
					return rc;
			}
			if (clkspec.np == node && !clk_supplier)
				return 0;

			clk = of_clk_get_from_provider(&clkspec);
			if (IS_ERR(clk)) {
				if (PTR_ERR(clk) != -EPROBE_DEFER)
					pr_warn("clk: couldn't get clock %d for %pOF\n",
						index, node);
				return PTR_ERR(clk);
			}

			rc = clk_set_rate(clk, rate);
			if (rc < 0)
				pr_err("clk: couldn't set %s clk rate to %u (%d), current rate: %lu\n",
				       __clk_get_name(clk), rate, rc,
				       clk_get_rate(clk));
			clk_put(clk);
		}
		index++;
	}
	return 0;
}

/**
 * of_clk_set_defaults() - parse and set assigned clocks configuration
 * @node: device node to apply clock settings for
 * @clk_supplier: true if clocks supplied by @node should also be considered
 *
 * This function parses 'assigned-{clocks/clock-parents/clock-rates}' properties
 * and sets any specified clock parents and rates. The @clk_supplier argument
 * should be set to true if @node may be also a clock supplier of any clock
 * listed in its 'assigned-clocks' or 'assigned-clock-parents' properties.
 * If @clk_supplier is false the function exits returning 0 as soon as it
 * determines the @node is also a supplier of any of the clocks.
 */

/*
 * IAMROOT, 2022.08.06:
 * - papago
 *   이 함수는 'assigned-{clocks/clock-parents/clock-rates}' 속성을 구문 분석하고
 *   지정된 클록 부모 및 속도를 설정합니다. @node가 'assigned-clocks' 또는
 *   'assigned-clock-parents' 속성에 나열된 clok의 clock 공급자일 수도 있는 경우
 *   @clk_supplier 인수는 true로 설정되어야 합니다. @clk_supplier가 false이면 함수는
 *   @node가 시계 공급자이기도 하다고 결정하자마자 0을 반환하며 종료됩니다.
 */
int of_clk_set_defaults(struct device_node *node, bool clk_supplier)
{
	int rc;

	if (!node)
		return 0;

	rc = __set_clk_parents(node, clk_supplier);
	if (rc < 0)
		return rc;

	return __set_clk_rates(node, clk_supplier);
}
EXPORT_SYMBOL_GPL(of_clk_set_defaults);
