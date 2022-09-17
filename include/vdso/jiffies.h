/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_JIFFIES_H
#define __VDSO_JIFFIES_H

#include <asm/param.h>			/* for HZ */
#include <vdso/time64.h>

/* TICK_NSEC is the time between ticks in nsec assuming SHIFTED_HZ */

/*
 * IAMROOT, 2022.09.17:
 * - ex) hz = 1000
 *   (10^9 + 1000 / 2) / 1000 = 1000_000 nsec = 1 msec
 */
#define TICK_NSEC ((NSEC_PER_SEC+HZ/2)/HZ)

#endif /* __VDSO_JIFFIES_H */
