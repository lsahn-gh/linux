/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BITS_H
#define __LINUX_BITS_H

#include <linux/const.h>
#include <vdso/bits.h>
#include <asm/bitsperlong.h>

/* IAMROOT, 2021.09.27:
 * - bit 시작 번호가 0이 아닌 1임을 주의
 * - BIT_WORD 는 Bitmap에서 해당 bit의 index를 구하는 개념
 */
#define BIT_ULL(nr)		(ULL(1) << (nr))
#define BIT_MASK(nr)		(UL(1) << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)	(ULL(1) << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define BITS_PER_BYTE		8

/*
 * Create a contiguous bitmask starting at bit position @l and ending at
 * position @h. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#if !defined(__ASSEMBLY__)
#include <linux/build_bug.h>
/*
 * IAMROOT, 2022.02.15:
 * - h, l가 상수일경우, h >= l 조건을 만족하는지 build time에 검사한다.
 *   만족하지 않으면 build error.
 * - h, l 둘중 하나라도 변수이면 검사하지 않는다.
 * --- __builtin_choose_expr(a, b, c)
 *  a가 true면 b, 아니면 c를 build time에 정한다. 이때 a는 상수여야 한다.
 *
 * --- __is_constexpr((l) > (h))
 * - 여기서 l, h의 값 자체를 검사하는 목적이 아니다, l, h 둘다
 *   상수인지 검사한다. l, h 둘다 상수면 true아니면 false가 return된다.
 * - __is_constexpr에서 true면 2번째 인자인 (l) > (h) , 아니면 3번째 인자 인
 *   0가 선택되어 BUILD_BUG_ON_ZERO에 진입한다.
 */
#define GENMASK_INPUT_CHECK(h, l) \
	(BUILD_BUG_ON_ZERO(__builtin_choose_expr( \
		__is_constexpr((l) > (h)), (l) > (h), 0)))
#else
/*
 * BUILD_BUG_ON_ZERO is not available in h files included from asm files,
 * disable the input check if that is the case.
 */
#define GENMASK_INPUT_CHECK(h, l) 0
#endif

/*
 * IAMROOT, 2022.02.15:
 * @h width bits
 * @l mask bits
 * h + 1만큼 set, l개 만큼 clear된 mask bits를 만든다.
 *
 * ex) h = 3, l = 0 => 0b00..001111
 *     h = 3, l = 1 => 0b00..001110
 *     h = 3, l = 2 => 0b00..001100
 *
 * --- ((~UL(0)) - (UL(1) << (l)) + 1)
 * bits >= l를 set, 아닌것을 clear한다.
 * ex) l = 0 => 0xffff_ffff_ffff_fffff
 *         1 => 0xffff_ffff_ffff_ffffe
 *         2 => 0xffff_ffff_ffff_ffffc
 * all set bits에서 l번 bit를 clear하고 + 1을 하여 0번 bit부터 l직전까지
 * 0으로 올림시켜 l이전을 0으로 clear 시키는 원리가된다.
 *
 * --- (~UL(0) >> (BITS_PER_LONG - 1 - (h)))
 * h >= bits를 set한다.
 * ex) h = 0 => 1
 *         1 => 3
 *         2 => 7
 * all set bits를 right shift시켜 h + 1개만큼 set 비트를 남겨놓는다.
 *
 * ---
 * 즉 h + 1만큼 mask bits를 만들되, l 개만큼은 clear 시킨 mask 를 만드는 목적
 */
#define __GENMASK(h, l) \
	(((~UL(0)) - (UL(1) << (l)) + 1) & \
	 (~UL(0) >> (BITS_PER_LONG - 1 - (h))))
/*
 * IAMROOT, 2022.02.15:
 * - h >= l인지 GENMASK_INPUT_CHECK 에서 검사한다.
 *   GENMASK_INPUT_CHECK값 자체는 0으로 출력되니 0 + __GENMASK(h, l) 이된다.
 */
#define GENMASK(h, l) \
	(GENMASK_INPUT_CHECK(h, l) + __GENMASK(h, l))

#define __GENMASK_ULL(h, l) \
	(((~ULL(0)) - (ULL(1) << (l)) + 1) & \
	 (~ULL(0) >> (BITS_PER_LONG_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) \
	(GENMASK_INPUT_CHECK(h, l) + __GENMASK_ULL(h, l))

#endif	/* __LINUX_BITS_H */
