/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * NUMA memory policies for Linux.
 * Copyright 2003,2004 Andi Kleen SuSE Labs
 */
#ifndef _UAPI_LINUX_MEMPOLICY_H
#define _UAPI_LINUX_MEMPOLICY_H

#include <linux/errno.h>


/*
 * Both the MPOL_* mempolicy mode and the MPOL_F_* optional mode flags are
 * passed by the user to either set_mempolicy() or mbind() in an 'int' actual.
 * The MPOL_MODE_FLAGS macro determines the legal set of optional mode flags.
 */

/* Policies */
enum {
	MPOL_DEFAULT,
/*
 * IAMROOT, 2022.05.07:
 * - 선호 node(반드시는 아님)
 */
	MPOL_PREFERRED,
/*
 * IAMROOT, 2022.05.07:
 * - 특정 node에 memory를 할당할수있는.
 */
	MPOL_BIND,
/*
 * IAMROOT, 2022.05.07:
 * - 2개 이상의 node에서 교대로 할당.
 */
	MPOL_INTERLEAVE,
/*
 * IAMROOT, 2022.05.07:
 * - cpu가 있는 node에서만 할당.
 */
	MPOL_LOCAL,
/*
 * IAMROOT, 2022.05.14:
 * - 선호
 */
	MPOL_PREFERRED_MANY,
	MPOL_MAX,	/* always last member of enum */
};

/* Flags for set_mempolicy */
#define MPOL_F_STATIC_NODES	(1 << 15)
#define MPOL_F_RELATIVE_NODES	(1 << 14)
#define MPOL_F_NUMA_BALANCING	(1 << 13) /* Optimize with NUMA balancing if possible */

/*
 * MPOL_MODE_FLAGS is the union of all possible optional mode flags passed to
 * either set_mempolicy() or mbind().
 */
#define MPOL_MODE_FLAGS							\
	(MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES | MPOL_F_NUMA_BALANCING)

/* Flags for get_mempolicy */
#define MPOL_F_NODE	(1<<0)	/* return next IL mode instead of node mask */
#define MPOL_F_ADDR	(1<<1)	/* look up vma using address */
#define MPOL_F_MEMS_ALLOWED (1<<2) /* return allowed memories */

/* Flags for mbind */
#define MPOL_MF_STRICT	(1<<0)	/* Verify existing pages in the mapping */
#define MPOL_MF_MOVE	 (1<<1)	/* Move pages owned by this process to conform
				   to policy */
#define MPOL_MF_MOVE_ALL (1<<2)	/* Move every page to conform to policy */
#define MPOL_MF_LAZY	 (1<<3)	/* Modifies '_MOVE:  lazy migrate on fault */
#define MPOL_MF_INTERNAL (1<<4)	/* Internal flags start here */

#define MPOL_MF_VALID	(MPOL_MF_STRICT   | 	\
			 MPOL_MF_MOVE     | 	\
			 MPOL_MF_MOVE_ALL)

/*
 * Internal flags that share the struct mempolicy flags word with
 * "mode flags".  These flags are allocated from bit 0 up, as they
 * are never OR'ed into the mode in mempolicy API arguments.
 */
/*
 * IAMROOT. 2023.06.29:
 * - google-translate
 * struct mempolicy 플래그 단어를 "모드 플래그"와 공유하는 내부
 * 플래그입니다. 이러한 플래그는 mempolicy API 인수에서 모드로 OR되지 않으므로 비트
 * 0부터 할당됩니다.
 */
#define MPOL_F_SHARED  (1 << 0)	/* identify shared policies */
#define MPOL_F_MOF	(1 << 3) /* this policy wants migrate on fault */
/*
 * IAMROOT, 2023.06.24:
 * - numa balaning에서 사용하는 flag
 */
#define MPOL_F_MORON	(1 << 4) /* Migrate On protnone Reference On Node */

/*
 * These bit locations are exposed in the vm.zone_reclaim_mode sysctl
 * ABI.  New bits are OK, but existing bits can never change.
 */
#define RECLAIM_ZONE	(1<<0)	/* Run shrink_inactive_list on the zone */
#define RECLAIM_WRITE	(1<<1)	/* Writeout pages during reclaim */
#define RECLAIM_UNMAP	(1<<2)	/* Unmap pages during reclaim */

#endif /* _UAPI_LINUX_MEMPOLICY_H */
