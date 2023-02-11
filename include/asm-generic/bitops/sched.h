/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_BITOPS_SCHED_H_
#define _ASM_GENERIC_BITOPS_SCHED_H_

#include <linux/compiler.h>	/* unlikely() */
#include <asm/types.h>

/*
 * Every architecture must define this function. It's the fastest
 * way of searching a 100-bit bitmap.  It's guaranteed that at least
 * one of the 100 bits is cleared.
 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   모든 아키텍처는 이 기능을 정의해야 합니다. 100비트 비트맵을 
 *   검색하는 가장 빠른 방법입니다. 100비트 중 적어도 하나는 
 *   지워지는 것이 보장됩니다.
 * - sched 전용으로 100개 전용으로 비트맵 검색을 만든것.
 */
static inline int sched_find_first_bit(const unsigned long *b)
{
#if BITS_PER_LONG == 64
	if (b[0])
		return __ffs(b[0]);
	return __ffs(b[1]) + 64;
#elif BITS_PER_LONG == 32
	if (b[0])
		return __ffs(b[0]);
	if (b[1])
		return __ffs(b[1]) + 32;
	if (b[2])
		return __ffs(b[2]) + 64;
	return __ffs(b[3]) + 96;
#else
#error BITS_PER_LONG not defined
#endif
}

#endif /* _ASM_GENERIC_BITOPS_SCHED_H_ */
