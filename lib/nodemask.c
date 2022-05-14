// SPDX-License-Identifier: GPL-2.0
#include <linux/nodemask.h>
#include <linux/module.h>
#include <linux/random.h>

/*
 * IAMROOT, 2022.05.14:
 * - @srcp에서 @node의 next를 찾아서 return한다. @srcp를 끝까지 조회한경우
 *   @srcp의 first return.
 */
int __next_node_in(int node, const nodemask_t *srcp)
{
	int ret = __next_node(node, srcp);

	if (ret == MAX_NUMNODES)
		ret = __first_node(srcp);
	return ret;
}
EXPORT_SYMBOL(__next_node_in);

#ifdef CONFIG_NUMA
/*
 * Return the bit number of a random bit set in the nodemask.
 * (returns NUMA_NO_NODE if nodemask is empty)
 */
int node_random(const nodemask_t *maskp)
{
	int w, bit = NUMA_NO_NODE;

	w = nodes_weight(*maskp);
	if (w)
		bit = bitmap_ord_to_pos(maskp->bits,
			get_random_int() % w, MAX_NUMNODES);
	return bit;
}
#endif
