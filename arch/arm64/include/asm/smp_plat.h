/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Definitions specific to SMP platforms.
 *
 * Copyright (C) 2013 ARM Ltd.
 */

#ifndef __ASM_SMP_PLAT_H
#define __ASM_SMP_PLAT_H

#include <linux/cpumask.h>

#include <asm/smp.h>
#include <asm/types.h>

/* IAMROOT, 2022.01.01:
 * - 수많은 cpu core(PE)들을 구분하기 위한 hash table
 *   (smp_build_mpidr_hash 에서 초기화한다.)
 *
 *   cpu_suspend(), cpu_resume() 함수에서 사용한다.
 */
struct mpidr_hash {
	u64	mask;
	u32	shift_aff[4];
	u32	bits;
};

extern struct mpidr_hash mpidr_hash;

/*
 * IAMROOT, 2022.01.02:
 * - 2^(bits)
 */
static inline u32 mpidr_hash_size(void)
{
	return 1 << mpidr_hash.bits;
}

/*
 * Retrieve logical cpu index corresponding to a given MPIDR.Aff*
 *  - mpidr: MPIDR.Aff* bits to be used for the look-up
 *
 * Returns the cpu logical index or -EINVAL on look-up error
 */
static inline int get_logical_index(u64 mpidr)
{
	int cpu;
	for (cpu = 0; cpu < nr_cpu_ids; cpu++)
		if (cpu_logical_map(cpu) == mpidr)
			return cpu;
	return -EINVAL;
}

#endif /* __ASM_SMP_PLAT_H */
