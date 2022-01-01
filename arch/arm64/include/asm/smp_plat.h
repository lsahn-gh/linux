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

/*
 * IAMROOT, 2022.01.01: 
 * 예) big/little: 4 core + 4 core
 *     mask: 0x0000_0303;
 *     shift_aff[] = { 0, 6, 12, * };
 *     bits = 4;
 *
 * 추후 cpu_suspend() 및 cpu_resume() 내부의 어셈블리 코드에서 사용한다.
 */
struct mpidr_hash {
	u64	mask;
	u32	shift_aff[4];
	u32	bits;
};

extern struct mpidr_hash mpidr_hash;

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
