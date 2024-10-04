// SPDX-License-Identifier: GPL-2.0-only
/*
 * NUMA support, based on the x86 implementation.
 *
 * Copyright (C) 2015 Cavium Inc.
 * Author: Ganapatrao Kulkarni <gkulkarni@cavium.com>
 */

#define pr_fmt(fmt) "NUMA: " fmt

#include <linux/acpi.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of.h>

#include <asm/sections.h>

struct pglist_data *node_data[MAX_NUMNODES] __read_mostly;
EXPORT_SYMBOL(node_data);
/*
 * IAMROOT, 2021.11.06:
 * - 일단 parsing이 된 node들을 저장해놓는 용도.
 */
nodemask_t numa_nodes_parsed __initdata;
/*
 * IAMROOT, 2022.01.02:
 * - of_parse_and_init_cpus에서 초기화된다.
 *   cpu의 numa node id가 저장된다.
 */
static int cpu_to_node_map[NR_CPUS] = { [0 ... NR_CPUS-1] = NUMA_NO_NODE };

/*
 * IAMROOT, 2021.11.06:
 * - numa_alloc_distance에서 memblock으로 할당되고,
 *   초기값으로 from, to가 같은, 즉 자기자신만을
 *   LOCAL_DISTANCE로, 나머지를 REMOTE_DISTANCE로 초기화한다.
 * - of_numa_parse_distance_map_v1에서 dt에서 읽은 값으로 재설정을 한다.
 * - numa_distance[from * numa_distance_cnt + to] 의 방식으로 접근한다.
 * IAMROOT, 2023.04.08:
 * - hip07-d05.dts
 *   distance-map {
 *		compatible = "numa-distance-map-v1";
 *		distance-matrix = <0 0 10>,
 *				  <0 1 15>,
 *				  <0 2 20>,
 *				  <0 3 25>,
 *				  <1 0 15>,
 *				  <1 1 10>,
 *				  <1 2 25>,
 *				  <1 3 30>,
 *				  <2 0 20>,
 *				  <2 1 25>,
 *				  <2 2 10>,
 *				  <2 3 15>,
 *				  <3 0 25>,
 *				  <3 1 30>,
 *				  <3 2 15>,
 *				  <3 3 10>;
 *	};
 */
static int numa_distance_cnt;
static u8 *numa_distance;

/* IAMROOT, 2021.11.06:
 * - 초기값은 false 이므로 numa는 on인 상태이다.
 */
bool numa_off;

static __init int numa_parse_early_param(char *opt)
{
	if (!opt)
		return -EINVAL;
	if (str_has_prefix(opt, "off"))
		numa_off = true;

	return 0;
}
early_param("numa", numa_parse_early_param);

cpumask_var_t node_to_cpumask_map[MAX_NUMNODES];
EXPORT_SYMBOL(node_to_cpumask_map);

#ifdef CONFIG_DEBUG_PER_CPU_MAPS

/*
 * Returns a pointer to the bitmask of CPUs on Node 'node'.
 */
const struct cpumask *cpumask_of_node(int node)
{

	if (node == NUMA_NO_NODE)
		return cpu_all_mask;

	if (WARN_ON(node < 0 || node >= nr_node_ids))
		return cpu_none_mask;

	if (WARN_ON(node_to_cpumask_map[node] == NULL))
		return cpu_online_mask;

	return node_to_cpumask_map[node];
}
EXPORT_SYMBOL(cpumask_of_node);

#endif

static void numa_update_cpu(unsigned int cpu, bool remove)
{
	int nid = cpu_to_node(cpu);

	if (nid == NUMA_NO_NODE)
		return;

	if (remove)
		cpumask_clear_cpu(cpu, node_to_cpumask_map[nid]);
	else
		cpumask_set_cpu(cpu, node_to_cpumask_map[nid]);
}

void numa_add_cpu(unsigned int cpu)
{
	numa_update_cpu(cpu, false);
}

void numa_remove_cpu(unsigned int cpu)
{
	numa_update_cpu(cpu, true);
}

void numa_clear_node(unsigned int cpu)
{
	numa_remove_cpu(cpu);
	set_cpu_numa_node(cpu, NUMA_NO_NODE);
}

/*
 * Allocate node_to_cpumask_map based on number of available nodes
 * Requires node_possible_map to be valid.
 *
 * Note: cpumask_of_node() is not valid until after this is done.
 * (Use CONFIG_DEBUG_PER_CPU_MAPS to check this.)
 */
/*
 * IAMROOT, 2021.11.13:
 * - node에 대응하는 cpumask_map을 초기화한다.
 */
static void __init setup_node_to_cpumask_map(void)
{
	int node;

	/* setup nr_node_ids if not done yet */
	if (nr_node_ids == MAX_NUMNODES)
		setup_nr_node_ids();

	/* allocate and clear the mapping */
	for (node = 0; node < nr_node_ids; node++) {
		alloc_bootmem_cpumask_var(&node_to_cpumask_map[node]);
		cpumask_clear(node_to_cpumask_map[node]);
	}

	/* cpumask_of_node() will now work */
	pr_debug("Node to cpumask map for %u nodes\n", nr_node_ids);
}

/*
 * Set the cpu to node and mem mapping
 */
void numa_store_cpu_info(unsigned int cpu)
{
	set_cpu_numa_node(cpu, cpu_to_node_map[cpu]);
}

/* IAMROOT, 2022.01.02:
 * - @nid를 cpu_to_node_map[@cpu]에 매핑한다.
 */
void __init early_map_cpu_to_node(unsigned int cpu, int nid)
{
	/* fallback to node 0 */
	if (nid < 0 || nid >= MAX_NUMNODES || numa_off)
		nid = 0;

	cpu_to_node_map[cpu] = nid;

	/*
	 * We should set the numa node of cpu0 as soon as possible, because it
	 * has already been set up online before. cpu_to_node(0) will soon be
	 * called.
	 */
	/* IAMROOT, 2024.10.04:
	 * - @cpu == 0 (bootcpu) 라면 set_cpu_numa_node(@cpu, @nid)를 호출한다.
	 *
	 *   per-cpu로 구성된 변수에 자신의 @nid 번호를 저장한다.
	 */
	if (!cpu)
		set_cpu_numa_node(cpu, nid);
}

#ifdef CONFIG_HAVE_SETUP_PER_CPU_AREA

/*
 * IAMROOT, 2022.02.05:
 * - delta = pcpu_base_addr - __per_cpu_start;
 *   __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
 */
unsigned long __per_cpu_offset[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(__per_cpu_offset);

static int __init early_cpu_to_node(int cpu)
{
	return cpu_to_node_map[cpu];
}

/*
 * IAMROOT, 2022.01.11:
 * cpu의 numa node id를 구한후, node끼리의 거리를 return 한다.
 */
static int __init pcpu_cpu_distance(unsigned int from, unsigned int to)
{
	return node_distance(early_cpu_to_node(from), early_cpu_to_node(to));
}

static void * __init pcpu_fc_alloc(unsigned int cpu, size_t size,
				       size_t align)
{
	int nid = early_cpu_to_node(cpu);

	return  memblock_alloc_try_nid(size, align,
			__pa(MAX_DMA_ADDRESS), MEMBLOCK_ALLOC_ACCESSIBLE, nid);
}

static void __init pcpu_fc_free(void *ptr, size_t size)
{
	memblock_free_early(__pa(ptr), size);
}

/*
 * IAMROOT, 2022.01.11:
 * - embed vs paged
 *   embed(arm64) : vmalloc의 위에서부터 아래로 자라는 방식으로 메모리 할당.
 *   paged : vmalloc의 밑에서부터 위로 자라는 방식으로 메모리 할당.
 *
 * - percpu 관련 api
 *   --- DEFINE_PER_CPU 시리즈
 *   --- per_cpu_ptr, per_cpu_offset, this_cpu_offset, this_cpu_ptr(많이쓰임),
 * __verify_pcu_ptr, 
 */
void __init setup_per_cpu_areas(void)
{
	unsigned long delta;
	unsigned int cpu;
	int rc;

	/*
	 * Always reserve area for module percpu variables.  That's
	 * what the legacy allocator did.
	 */
/*
 * IAMROOT, 2022.01.08: 
 * arm64의 경우 embed 방식으로 first chunk를 생성한다.
 * 그 외의 경우 아키텍처에 따라 page 방식으로 만드는 방법도 있다.
 */
	rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
				    PERCPU_DYNAMIC_RESERVE, PAGE_SIZE,
				    pcpu_cpu_distance,
				    pcpu_fc_alloc, pcpu_fc_free);
	if (rc < 0)
		panic("Failed to initialize percpu areas.");

/*
 * IAMROOT, 2022.01.08: 
 * delta: 
 *     delta = pcpu_base_addr - __per_cpu_start
 *         1) pcpu_base_addr: 
 *            1st chunk의 첫 static 주소
 *              (run 타입에 결정, 실제 per-cpu 접근할 주소가 담긴다)
 *         2) __per_cpu_start:
 *           .data.percpu.* 섹션의 시작 주소
 *		(compile 타임에 결정, static 변수에 접근할 때 사용하지 않는다.)
 */
	delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
	for_each_possible_cpu(cpu)
		__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
#endif

/**
 * numa_add_memblk() - Set node id to memblk
 * @nid: NUMA node ID of the new memblk
 * @start: Start address of the new memblk
 * @end:  End address of the new memblk
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * IAMROOT, 2021.11.06:
 * - 해당 범위의 memblock을 nid로 설정한다.
 */
int __init numa_add_memblk(int nid, u64 start, u64 end)
{
	int ret;

	ret = memblock_set_node(start, (end - start), &memblock.memory, nid);
	if (ret < 0) {
		pr_err("memblock [0x%llx - 0x%llx] failed to add on node %d\n",
			start, (end - 1), nid);
		return ret;
	}

	node_set(nid, numa_nodes_parsed);
	return ret;
}

/*
 * Initialize NODE_DATA for a node on the local memory
 */
/*
 * IAMROOT, 2021.11.13:
 * - 해당 nid의 node_data를 초기화한다.
 *   node_data의 memory를 할당하고 node_data에 nid, start pfn, pfn size를
 *   설정한다.
 */
static void __init setup_node_data(int nid, u64 start_pfn, u64 end_pfn)
{
/*
 * IAMROOT, 2021.11.13:
 * - node_data가 pg_data_t이므로 거기에 필요한 size.
 *   64byte(SMPP_CACHE_BYTES)단위로 라운드업한다.
 */
	const size_t nd_size = roundup(sizeof(pg_data_t), SMP_CACHE_BYTES);
	u64 nd_pa;
	void *nd;
	int tnid;

	if (start_pfn >= end_pfn)
		pr_info("Initmem setup node %d [<memory-less node>]\n", nid);

	nd_pa = memblock_phys_alloc_try_nid(nd_size, SMP_CACHE_BYTES, nid);
	if (!nd_pa)
		panic("Cannot allocate %zu bytes for node %d data\n",
		      nd_size, nid);

	nd = __va(nd_pa);

	/* report and initialize */
	pr_info("NODE_DATA [mem %#010Lx-%#010Lx]\n",
		nd_pa, nd_pa + nd_size - 1);
/*
 * IAMROOT, 2021.11.13:
 * - nid의 node_data가 할당된 memory가 할당될 node_data의 nid와 같지 않으면
 *   한번 print를 출력한다.
 */
	tnid = early_pfn_to_nid(nd_pa >> PAGE_SHIFT);
	if (tnid != nid)
		pr_info("NODE_DATA(%d) on node %d\n", nid, tnid);

	node_data[nid] = nd;
	memset(NODE_DATA(nid), 0, sizeof(pg_data_t));
	NODE_DATA(nid)->node_id = nid;
	NODE_DATA(nid)->node_start_pfn = start_pfn;
	NODE_DATA(nid)->node_spanned_pages = end_pfn - start_pfn;
}

/*
 * numa_free_distance
 *
 * The current table is freed.
 */
void __init numa_free_distance(void)
{
	size_t size;

	if (!numa_distance)
		return;

	size = numa_distance_cnt * numa_distance_cnt *
		sizeof(numa_distance[0]);

	memblock_free_ptr(numa_distance, size);
	numa_distance_cnt = 0;
	numa_distance = NULL;
}

/*
 * Create a new NUMA distance table.
 */
/* IAMROOT, 2021.11.06:
 * - NUMA distance table을 생성하고 초기화한다.
 */
static int __init numa_alloc_distance(void)
{
	size_t size;
	u64 phys;
	int i, j;

	/* IAMROOT, 2021.11.06:
	 * - NUMA node 간 distance 정보를 가지고 있는 table을 위한 memory alloc.
	 *
	 *   compile time에는 MAX_NUMNODES로 설정되었다가 boottime에
	 *   setup_nr_node_ids(..)가 호출되면 dt에서 parsing 한것과 MAX_NUMNODES
	 *   중에서 가장 큰 값을 nr_node_ids로 초기화한다.
	 *
	 *   nr_node_ids 변수는 mm/page_alloc.c 파일에 위치한다.
	 */
	size = nr_node_ids * nr_node_ids * sizeof(numa_distance[0]);
	phys = memblock_phys_alloc_range(size, PAGE_SIZE, 0, PFN_PHYS(max_pfn));
	if (WARN_ON(!phys))
		return -ENOMEM;

	numa_distance = __va(phys);
	numa_distance_cnt = nr_node_ids;

	/* fill with the default distances */
	/* IAMROOT, 2024.06.09:
	 * - nr_node_ids 만큼 loop 수행하며 distance table을 초기화한다.
	 *
	 *   i == j 라면 동일한 node에 있는 것이므로 LOCAL_DISTANCE로 초기화한다.
	 */
	for (i = 0; i < numa_distance_cnt; i++)
		for (j = 0; j < numa_distance_cnt; j++)
			numa_distance[i * numa_distance_cnt + j] = i == j ?
				LOCAL_DISTANCE : REMOTE_DISTANCE;

	pr_debug("Initialized distance table, cnt=%d\n", numa_distance_cnt);

	return 0;
}

/**
 * numa_set_distance() - Set inter node NUMA distance from node to node.
 * @from: the 'from' node to set distance
 * @to: the 'to'  node to set distance
 * @distance: NUMA distance
 *
 * Set the distance from node @from to @to to @distance.
 * If distance table doesn't exist, a warning is printed.
 *
 * If @from or @to is higher than the highest known node or lower than zero
 * or @distance doesn't make sense, the call is ignored.
 */
/*
 * IAMROOT, 2021.11.13:
 * - from, to 값으로 배열 인덱스를 만들고 해당 위치에 distacne값을 넣는다.
 */
void __init numa_set_distance(int from, int to, int distance)
{
	if (!numa_distance) {
		pr_warn_once("Warning: distance table not allocated yet\n");
		return;
	}

	if (from >= numa_distance_cnt || to >= numa_distance_cnt ||
			from < 0 || to < 0) {
		pr_warn_once("Warning: node ids are out of bound, from=%d to=%d distance=%d\n",
			    from, to, distance);
		return;
	}

	if ((u8)distance != distance ||
	    (from == to && distance != LOCAL_DISTANCE)) {
		pr_warn_once("Warning: invalid distance parameter, from=%d to=%d distance=%d\n",
			     from, to, distance);
		return;
	}

	numa_distance[from * numa_distance_cnt + to] = distance;
}

/*
 * Return NUMA distance @from to @to
 */
/*
 * IAMROOT. 2023.04.08:
 * - google-translate
 * NUMA 거리 @from을 @to로 반환
 * - from 에서 to 까지 거리를 가져온다
 */
int __node_distance(int from, int to)
{
	if (from >= numa_distance_cnt || to >= numa_distance_cnt)
		return from == to ? LOCAL_DISTANCE : REMOTE_DISTANCE;
	return numa_distance[from * numa_distance_cnt + to];
}
EXPORT_SYMBOL(__node_distance);

/*
 * IAMROOT, 2021.11.13:
 * - node_data를 초기화하고 node를 online 한다.
 */
static int __init numa_register_nodes(void)
{
	int nid;
	struct memblock_region *mblk;
/*
 * IAMROOT, 2021.11.13:
 * - memblock의 node id가 정상으로 있는지 한번 검사.
 */
	/* Check that valid nid is set to memblks */
	for_each_mem_region(mblk) {
		int mblk_nid = memblock_get_region_node(mblk);
		phys_addr_t start = mblk->base;
		phys_addr_t end = mblk->base + mblk->size - 1;

		if (mblk_nid == NUMA_NO_NODE || mblk_nid >= MAX_NUMNODES) {
			pr_warn("Warning: invalid memblk node %d [mem %pap-%pap]\n",
				mblk_nid, &start, &end);
			return -EINVAL;
		}
	}

/*
 * IAMROOT, 2021.11.13:
 * - nid에 대해서 node_data를 만들고 해당 nid를 online한다.
 */
	/* Finally register nodes. */
	for_each_node_mask(nid, numa_nodes_parsed) {
		unsigned long start_pfn, end_pfn;

		get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);
		setup_node_data(nid, start_pfn, end_pfn);
		node_set_online(nid);
	}

	/* Setup online nodes to actual nodes*/
	node_possible_map = numa_nodes_parsed;

	return 0;
}

/*
 * IAMROOT, 2021.11.13:
 * - dt에서 node에 대한 정보를 가져오고, node_data를 초기화 하고,
 *   node들을 online시키고 해당 node의 cpumask_map을 초기화한다.
 */
static int __init numa_init(int (*init_func)(void))
{
	int ret;

	nodes_clear(numa_nodes_parsed);
	nodes_clear(node_possible_map);
	nodes_clear(node_online_map);

	/* IAMROOT, 2024.06.09:
	 * - node distance table을 생성하고 초기화한다.
	 */
	ret = numa_alloc_distance();
	if (ret < 0)
		return ret;

	/* IAMROOT, 2024.06.09:
	 * - callback 함수 호출
	 *
	 *   ACPI 또는 DT의 callback을 사용한다.
	 */
	ret = init_func();
	if (ret < 0)
		goto out_free_distance;

	/* IAMROOT, 2024.06.09:
	 * - parsing 할 데이터가 없으면 configuration 하지 않고 나간다.
	 */
	if (nodes_empty(numa_nodes_parsed)) {
		pr_info("No NUMA configuration found\n");
		ret = -EINVAL;
		goto out_free_distance;
	}

	ret = numa_register_nodes();
	if (ret < 0)
		goto out_free_distance;

	setup_node_to_cpumask_map();

	return 0;
out_free_distance:
	numa_free_distance();
	return ret;
}

/**
 * dummy_numa_init() - Fallback dummy NUMA init
 *
 * Used if there's no underlying NUMA architecture, NUMA initialization
 * fails, or NUMA is disabled on the command line.
 *
 * Must online at least one node (node 0) and add memory blocks that cover all
 * allowed memory. It is unlikely that this function fails.
 *
 * Return: 0 on success, -errno on failure.
 */
static int __init dummy_numa_init(void)
{
	phys_addr_t start = memblock_start_of_DRAM();
	phys_addr_t end = memblock_end_of_DRAM() - 1;
	int ret;

	if (numa_off)
		pr_info("NUMA disabled\n"); /* Forced off on command line. */
	pr_info("Faking a node at [mem %pap-%pap]\n", &start, &end);

	ret = numa_add_memblk(0, start, end + 1);
	if (ret) {
		pr_err("NUMA init failed\n");
		return ret;
	}

	numa_off = true;
	return 0;
}

#ifdef CONFIG_ACPI_NUMA
static int __init arch_acpi_numa_init(void)
{
	int ret;

	ret = acpi_numa_init();
	if (ret) {
		pr_info("Failed to initialise from firmware\n");
		return ret;
	}

	return srat_disabled() ? -EINVAL : 0;
}
#else
static int __init arch_acpi_numa_init(void)
{
	return -EOPNOTSUPP;
}
#endif

/**
 * arch_numa_init() - Initialize NUMA
 *
 * Try each configured NUMA initialization method until one succeeds. The
 * last fallback is dummy single node config encompassing whole memory.
 */
/* IAMROOT, 2021.11.13: TODO
 * - numa 관련 초기화 수행.
 */
void __init arch_numa_init(void)
{
	if (!numa_off) {
		if (!acpi_disabled && !numa_init(arch_acpi_numa_init))
			return;
		if (acpi_disabled && !numa_init(of_numa_init))
			return;
	}

	numa_init(dummy_numa_init);
}
