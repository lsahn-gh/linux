/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* const.h: Macros for dealing with constants.  */

#ifndef _UAPI_LINUX_CONST_H
#define _UAPI_LINUX_CONST_H

/* Some constant macros are used in both assembler and
 * C code.  Therefore we cannot annotate them always with
 * 'UL' and other type specifiers unilaterally.  We
 * use the following macros to deal with this.
 *
 * Similarly, _AT() will cast an expression with a type in C, but
 * leave it unchanged in asm.
 */

#ifdef __ASSEMBLY__
#define _AC(X,Y)	X
#define _AT(T,X)	X
#else
#define __AC(X,Y)	(X##Y)
#define _AC(X,Y)	__AC(X,Y)
#define _AT(T,X)	((T)(X))
#endif

#define _UL(x)		(_AC(x, UL))
#define _ULL(x)		(_AC(x, ULL))

#define _BITUL(x)	(_UL(1) << (x))
#define _BITULL(x)	(_ULL(1) << (x))

/* IAMROOT, 2021.10.09:
 * __ALIGN_KERNEL(x, a):
 *   @x 값을 round up 하여 @a 단위로 정렬한다.
 *   - 예제1:
 *     @x: 0x1234 / @a: 0x1000 (4KB)
 *     = 0x2233 = (0x1234 + 0xfff)
 *     = 0x2233 & (0xffff_ffff_ffff_f000)
 *     = 0x2000
 *   - 예제2:
 *     @x: 0x1000 / @a: 0x1000 (4KB)
 *     = 0x1000
 */
#define __ALIGN_KERNEL(x, a)		__ALIGN_KERNEL_MASK(x, (typeof(x))(a) - 1)
#define __ALIGN_KERNEL_MASK(x, mask)	(((x) + (mask)) & ~(mask))

#define __KERNEL_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#endif /* _UAPI_LINUX_CONST_H */
