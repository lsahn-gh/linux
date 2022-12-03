/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_LOADAVG_H
#define _LINUX_SCHED_LOADAVG_H

/*
 * These are the constant used to fake the fixed-point load-average
 * counting. Some notes:
 *  - 11 bit fractions expand to 22 bits by the multiplies: this gives
 *    a load-average precision of 10 bits integer + 11 bits fractional
 *  - if you want to count load-averages more often, you need more
 *    precision, or rounding will get you. With 2-second counting freq,
 *    the EXP_n values would be 1981, 2034 and 2043 if still using only
 *    11 bit fractions.
 */
extern unsigned long avenrun[];		/* Load averages */
extern void get_avenrun(unsigned long *loads, unsigned long offset, int shift);

#define FSHIFT		11		/* nr of bits of precision */
/*
 * IAMROOT, 2022.12.03:
 * - 이진화정수화한 정밀도. 2^11 = 2048
 */
#define FIXED_1		(1<<FSHIFT)	/* 1.0 as fixed-point */
/*
 * IAMROOT, 2022.11.26:
 * - 5초지난후 다음 tick.
 */
#define LOAD_FREQ	(5*HZ+1)	/* 5 sec intervals */
/*
 * IAMROOT, 2022.12.03:
 * - e = 2.718281828459045...
 *
 * - new값을 old대비 몇 % 비율을 넣느냐를 이진화정수화 해서 결정한다.
 *   EXP_1 : 1분은 약 92%
 *   EXP_5 : 5분은 약 98.35%
 *   EXP_15 : 15분은 약 99.45%
 *   이값을 100%를 2048로 이진화정수화 하여 계산한다.
 *
 * - e^(-1/12) = 0.9200..
 *   이값에 이진화정수 정밀도 적용
 *   0.9200 * 2048 = 1884
 * - e^(-1/60) = 
 * - e^(-1/180)
 */
#define EXP_1		1884		/* 1/exp(5sec/1min) as fixed-point */
#define EXP_5		2014		/* 1/exp(5sec/5min) */
#define EXP_15		2037		/* 1/exp(5sec/15min) */

/*
 * a1 = a0 * e + a * (1 - e)
 */
/*
 * IAMROOT, 2022.12.03:
 * - ex) task1개 만큼 증가한 case.
 *   load = 1024, exp = 1884, active = 2048
 *   newload = 1024 * 1884 + 2048 * (2048 - 1884)
 *           = 2,265,088
 *   return = (2,265,088 + 2047)/ 2048
 *          = 1106
 * - ex) task가 안돌때 감소한 case.
 *   load = 1024, exp = 1884, active = 0
 *   newload = 1024 * 1884 + 0 * (2048 - 1884)
 *           = 1,929,216
 *   return = 1,929,216 / 2048
 *          = 942
 */
static inline unsigned long
calc_load(unsigned long load, unsigned long exp, unsigned long active)
{
	unsigned long newload;

/*
 * IAMROOT, 2022.12.03:
 * - old * k + new * ( 1 - k)
 */
	newload = load * exp + active * (FIXED_1 - exp);
/*
 * IAMROOT, 2022.12.03:
 * - new값이 old보다 크거나 같으면 2048 - 1을 더해준다.
 *   new값이 클경우 올림처리.
 * - 글로벌 cpu 로드를 처리하는데 장시간 idle 상태인데에도 각 주기별로 0.00, 0.01, 0.05 이하 값이 더 이상 하강하지 않는 버그가 있어 수정하였다.
 * - 참고)
 *   https://github.com/torvalds/linux/commit/20878232c52329f92423d27a60e48b6a6389e0dd#diff-3a8f0c7004315fef3f3985bfd1945858*
 */
	if (active >= load)
		newload += FIXED_1-1;

/*
 * IAMROOT, 2022.12.03:
 * - 100 -> * 10 -> 1000 
 *   50     * 10  -> 500
 *   20     * 10  -> 200
 *
 *  700 / 10 = 70
 */
	return newload / FIXED_1;
}

extern unsigned long calc_load_n(unsigned long load, unsigned long exp,
				 unsigned long active, unsigned int n);

#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)

extern void calc_global_load(void);

#endif /* _LINUX_SCHED_LOADAVG_H */
