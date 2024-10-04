// SPDX-License-Identifier: GPL-2.0
/*
 * OF NUMA Parsing support.
 *
 * Copyright (C) 2015 - 2016 Cavium Inc.
 */

#define pr_fmt(fmt) "OF: NUMA: " fmt

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/nodemask.h>

#include <asm/numa.h>

/* define default numa node to 0 */
#define DEFAULT_NODE 0

/*
 * Even though we connect cpus to numa domains later in SMP
 * init, we need to know the node ids now for all cpus.
*/
/*
 * IAMROOT, 2021.11.06:
 * device node를 돌면서 cpu node를 찾아서 numa_nodes_parsed를 set한다
 *
 * - dt hip07.dtsi 참고
 * cpu0: cpu@10000 {
 *	device_type = "cpu";
 *	compatible = "arm,cortex-a72";
 *	reg = <0x10000>;
 *	enable-method = "psci";
 *	next-level-cache = <&cluster0_l2>;
 *	numa-node-id = <0>;
 * };
 *
 * 총4개의 node를 가지고 있으며 1개 node당 16개의 cpu를 가진것이 확인된다.
 * 총 cpu숫자는 64개
 */
static void __init of_numa_parse_cpu_nodes(void)
{
	u32 nid;
	int r;
	struct device_node *np;

	for_each_of_cpu_node(np) {
		r = of_property_read_u32(np, "numa-node-id", &nid);
		if (r)
			continue;

		pr_debug("CPU on %u\n", nid);
		if (nid >= MAX_NUMNODES)
			pr_warn("Node id %u exceeds maximum value\n", nid);
		else
			node_set(nid, numa_nodes_parsed);
	}
}

/*
 * IAMROOT, 2021.11.06:
 * - device node에서 memory node를 순회해 numa-node-id, address를 가져온다.
 *   그리고 해당 address 범위에 numa node id를 설정한다.
 *
 * ex) node 예
 * memory@0 {
 *	device_type = "memory";
 *	reg = <0x0 0x00000000 0x0 0x40000000>;
 *	numa-node-id = <0>;
 * };
 */
static int __init of_numa_parse_memory_nodes(void)
{
	struct device_node *np = NULL;
	struct resource rsrc;
	u32 nid;
	int i, r;

	for_each_node_by_type(np, "memory") {
		r = of_property_read_u32(np, "numa-node-id", &nid);
		if (r == -EINVAL)
			/*
			 * property doesn't exist if -EINVAL, continue
			 * looking for more memory nodes with
			 * "numa-node-id" property
			 */
			continue;

		if (nid >= MAX_NUMNODES) {
			pr_warn("Node id %u exceeds maximum value\n", nid);
			r = -EINVAL;
		}

		for (i = 0; !r && !of_address_to_resource(np, i, &rsrc); i++)
			r = numa_add_memblk(nid, rsrc.start, rsrc.end + 1);

		if (!i || r) {
			of_node_put(np);
			pr_err("bad property in memory node\n");
			return r ? : -EINVAL;
		}
	}

	return 0;
}
/*
 * IAMROOT, 2021.11.13:
 * - distance-map의 distance-matrix의 크기를 읽고
 *   from, to, distance값을 각각 읽어 에러검사를 수행하여
 *   numa_distance 배열을 초기화한다.
 */
static int __init of_numa_parse_distance_map_v1(struct device_node *map)
{
	const __be32 *matrix;
	int entry_count;
	int i;

	pr_info("parsing numa-distance-map-v1\n");

	matrix = of_get_property(map, "distance-matrix", NULL);
	if (!matrix) {
		pr_err("No distance-matrix property in distance-map\n");
		return -EINVAL;
	}

	entry_count = of_property_count_u32_elems(map, "distance-matrix");
	if (entry_count <= 0) {
		pr_err("Invalid distance-matrix\n");
		return -EINVAL;
	}

	for (i = 0; i + 2 < entry_count; i += 3) {
		u32 nodea, nodeb, distance;

		nodea = of_read_number(matrix, 1);
		matrix++;
		nodeb = of_read_number(matrix, 1);
		matrix++;
		distance = of_read_number(matrix, 1);
		matrix++;

		if ((nodea == nodeb && distance != LOCAL_DISTANCE) ||
		    (nodea != nodeb && distance <= LOCAL_DISTANCE)) {
			pr_err("Invalid distance[node%d -> node%d] = %d\n",
			       nodea, nodeb, distance);
			return -EINVAL;
		}

		numa_set_distance(nodea, nodeb, distance);

		/* Set default distance of node B->A same as A->B */
		if (nodeb > nodea)
			numa_set_distance(nodeb, nodea, distance);
	}

	return 0;
}

/*
 * IAMROOT, 2021.11.13:
 * - distance-map node에서 compatible이
 *   numa-distance-map-v1인것을 찾아 numa_distance 를 초기화한다.
 * ex)
 * distance-map {
 *	compatible = "numa-distance-map-v1";
 *	distance-matrix = <0 0 10>,
 *			  <0 1 15>,
 *			  <0 2 20>,
 *			  <0 3 25>,
 *			  <1 0 15>
 *			  <1 1 10>,
 *			  <1 2 25>,
 *			  <1 3 30>,
 *			  <2 0 20>,
 *			  <2 1 25>,
 *			  <2 2 10>,
 *			  <2 3 15>,
 *			  <3 0 25>,
 *			  <3 1 30>,
 *			  <3 2 15>,
 *			  <3 3 10>;
 * };
 *
 * | from | to  | distance |
 * | 0    | 0   | 10       |
 * | 0    | 1   | 15       |
 * | 0    | 2   | 20       |
 * | 0    | 3   | 25       |
 * | 1    | 0   | 15       |
 * | 1    | 1   | 10       |
 * | 1    | 2   | 25       |
 * | 1    | 3   | 30       |
 * | 2    | 0   | 20       |
 * | 2    | 1   | 25       |
 * | 2    | 2   | 10       |
 * | 2    | 3   | 15       |
 * | 3    | 0   | 25       |
 * | 3    | 1   | 30       |
 * | 3    | 2   | 15       |
 * | 3    | 3   | 10       |
 *
 * +-----+   15   +-----+
 * |  0  | ------ |  1  |
 * +-----+        +-----+
 *   |     \   / 25   |
 *   |20     X        |30
 *   |     /    \25   |
 * +-----+        +-----+
 * |  2  | ------ |  3  |
 * +-----+  15    +-----+
 */
static int __init of_numa_parse_distance_map(void)
{
	int ret = 0;
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL,
				     "numa-distance-map-v1");
	if (np)
		ret = of_numa_parse_distance_map_v1(np);

	of_node_put(np);
	return ret;
}

/* IAMROOT, 2022.01.02:
 * - @device의 'numa-node-id' property를 dt에서 파싱한다.
 */
int of_node_to_nid(struct device_node *device)
{
	struct device_node *np;
	u32 nid;
	int r = -ENODATA;

	np = of_node_get(device);

	while (np) {
		r = of_property_read_u32(np, "numa-node-id", &nid);
		/*
		 * -EINVAL indicates the property was not found, and
		 *  we walk up the tree trying to find a parent with a
		 *  "numa-node-id".  Any other type of error indicates
		 *  a bad device tree and we give up.
		 */
		if (r != -EINVAL)
			break;

		np = of_get_next_parent(np);
	}
	if (np && r)
		pr_warn("Invalid \"numa-node-id\" property in node %pOFn\n",
			np);
	of_node_put(np);

	/*
	 * If numa=off passed on command line, or with a defective
	 * device tree, the nid may not be in the set of possible
	 * nodes.  Check for this case and return NUMA_NO_NODE.
	 */
	if (!r && nid < MAX_NUMNODES && node_possible(nid))
		return nid;

	return NUMA_NO_NODE;
}

int __init of_numa_init(void)
{
	int r;

	of_numa_parse_cpu_nodes();
	r = of_numa_parse_memory_nodes();
	if (r)
		return r;
	return of_numa_parse_distance_map();
}
