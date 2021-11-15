/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ALIGN_H
#define _LINUX_ALIGN_H

#include <linux/const.h>

/*
 * IAMROOT, 2021.10.09: 
 * - ALIGN(x, a)
 *   x 값을 a 정렬 단위로 round up 한다. (a 값은 2의 승수값만 사용가능하다)
 *   예) 0x1234 -> 0x2000
 *       0x1000 -> 0x1000
 */
/* @a is a power of 2 value */
#define ALIGN(x, a)		__ALIGN_KERNEL((x), (a))
#define ALIGN_DOWN(x, a)	__ALIGN_KERNEL((x) - ((a) - 1), (a))
#define __ALIGN_MASK(x, mask)	__ALIGN_KERNEL_MASK((x), (mask))
#define PTR_ALIGN(p, a)		((typeof(p))ALIGN((unsigned long)(p), (a)))
#define PTR_ALIGN_DOWN(p, a)	((typeof(p))ALIGN_DOWN((unsigned long)(p), (a)))
#define IS_ALIGNED(x, a)		(((x) & ((typeof(x))(a) - 1)) == 0)

#endif	/* _LINUX_ALIGN_H */
