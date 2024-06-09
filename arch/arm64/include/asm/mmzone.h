/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_MMZONE_H
#define __ASM_MMZONE_H

#ifdef CONFIG_NUMA

#include <asm/numa.h>

/* IAMROOT, 2021.11.13:
 * - drivers/base/arch_numa.c의 setup_node_data(..)에서 초기화한다.
 *
 * - nid의 각 node_data는 해당 nid의 mem_block에 존재하도록 노력한다.
 * - nid의 start pfn, size등이 존재해서 특정 nid의 start pfn과 size를
 *   바로 알 수 있다.
 */
extern struct pglist_data *node_data[];
#define NODE_DATA(nid)		(node_data[(nid)])

#endif /* CONFIG_NUMA */
#endif /* __ASM_MMZONE_H */
