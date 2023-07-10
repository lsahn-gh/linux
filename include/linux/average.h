/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_AVERAGE_H
#define _LINUX_AVERAGE_H

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/log2.h>

/*
 * Exponentially weighted moving average (EWMA)
 *
 * This implements a fixed-precision EWMA algorithm, with both the
 * precision and fall-off coefficient determined at compile-time
 * and built into the generated helper funtions.
 *
 * The first argument to the macro is the name that will be used
 * for the struct and helper functions.
 *
 * The second argument, the precision, expresses how many bits are
 * used for the fractional part of the fixed-precision values.
 *
 * The third argument, the weight reciprocal, determines how the
 * new values will be weighed vs. the old state, new values will
 * get weight 1/weight_rcp and old values 1-1/weight_rcp. Note
 * that this parameter must be a power of two for efficiency.
 */
/*
 * IAMROOT. 2023.06.16:
 * - google-translate
 * 지수 가중 이동 평균(EWMA)
 *
 * 이는 고정 정밀도 EWMA 알고리즘을 구현하며, 정밀도와 감소 계수는 컴파일 타임에 결정되고
 * 생성된 도우미 기능에 내장됩니다.
 *
 * 매크로의 첫번째 인수는 구조체 및 도우미 함수에 사용될 이름입니다.
 *
 * 두 번째 인수인 정밀도는 고정 정밀도 값의 소수 부분에 사용되는 비트 수를 나타냅니다.
 *
 * 세 번째 인수인 가중치 역수는 이전 상태와 비교하여 새 값의 가중치를 결정하는 방법을
 * 결정합니다. 새 값은 가중치 1/weight_rcp 및 이전 값은 1-1/weight_rcp를 갖게
 * 됩니다. 효율성을 위해 이 매개변수는 2의 거듭제곱이어야 합니다.
 */

#define DECLARE_EWMA(name, _precision, _weight_rcp)			\
	struct ewma_##name {						\
		unsigned long internal;					\
	};								\
	static inline void ewma_##name##_init(struct ewma_##name *e)	\
	{								\
		BUILD_BUG_ON(!__builtin_constant_p(_precision));	\
		BUILD_BUG_ON(!__builtin_constant_p(_weight_rcp));	\
		/*							\
		 * Even if you want to feed it just 0/1 you should have	\
		 * some bits for the non-fractional part...		\
		 */							\
		BUILD_BUG_ON((_precision) > 30);			\
		BUILD_BUG_ON_NOT_POWER_OF_2(_weight_rcp);		\
		e->internal = 0;					\
	}								\
	static inline unsigned long					\
	ewma_##name##_read(struct ewma_##name *e)			\
	{								\
		BUILD_BUG_ON(!__builtin_constant_p(_precision));	\
		BUILD_BUG_ON(!__builtin_constant_p(_weight_rcp));	\
		BUILD_BUG_ON((_precision) > 30);			\
		BUILD_BUG_ON_NOT_POWER_OF_2(_weight_rcp);		\
		return e->internal >> (_precision);			\
	}								\
	static inline void ewma_##name##_add(struct ewma_##name *e,	\
					     unsigned long val)		\
	{								\
		unsigned long internal = READ_ONCE(e->internal);	\
		unsigned long weight_rcp = ilog2(_weight_rcp);		\
		unsigned long precision = _precision;			\
									\
		BUILD_BUG_ON(!__builtin_constant_p(_precision));	\
		BUILD_BUG_ON(!__builtin_constant_p(_weight_rcp));	\
		BUILD_BUG_ON((_precision) > 30);			\
		BUILD_BUG_ON_NOT_POWER_OF_2(_weight_rcp);		\
									\
		WRITE_ONCE(e->internal, internal ?			\
			(((internal << weight_rcp) - internal) +	\
				(val << precision)) >> weight_rcp :	\
			(val << precision));				\
	}

#endif /* _LINUX_AVERAGE_H */
