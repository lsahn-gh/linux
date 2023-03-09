/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_PRIO_H
#define _LINUX_SCHED_PRIO_H

#define MAX_NICE	19
#define MIN_NICE	-20
#define NICE_WIDTH	(MAX_NICE - MIN_NICE + 1)

/*
 * Priority of a process goes from 0..MAX_PRIO-1, valid RT
 * priority is 0..MAX_RT_PRIO-1, and SCHED_NORMAL/SCHED_BATCH
 * tasks are in the range MAX_RT_PRIO..MAX_PRIO-1. Priority
 * values are inverted: lower p->prio value means higher priority.
 */
/*
 * IAMROOT, 2022.11.26:
 * - papago
 *   프로세스의 우선 순위는 0..MAX_PRIO-1이고 유효한 RT 우선 순위는
 *   0..MAX_RT_PRIO-1이며 SCHED_NORMAL/SCHED_BATCH 작업은
 *   MAX_RT_PRIO..MAX_PRIO-1 범위에 있습니다. 우선 순위 값은 반전됩니다.
 *   낮은 p->prio 값은 높은 우선 순위를 의미합니다.
 *
 * - 0 ~ 99    : RT
 *   100 ~ 139 : CFS
 */
#define MAX_RT_PRIO		100

/*
 * IAMROOT, 2022.11.26:
 * - 140
 */
#define MAX_PRIO		(MAX_RT_PRIO + NICE_WIDTH)
#define DEFAULT_PRIO		(MAX_RT_PRIO + NICE_WIDTH / 2)

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to static priority [ MAX_RT_PRIO..MAX_PRIO-1 ],
 * and back.
 */
#define NICE_TO_PRIO(nice)	((nice) + DEFAULT_PRIO)
#define PRIO_TO_NICE(prio)	((prio) - DEFAULT_PRIO)

/*
 * Convert nice value [19,-20] to rlimit style value [1,40].
 */
static inline long nice_to_rlimit(long nice)
{
	return (MAX_NICE - nice + 1);
}

/*
 * Convert rlimit style value [1,40] to nice value [-20, 19].
 */
static inline long rlimit_to_nice(long prio)
{
	return (MAX_NICE - prio + 1);
}

#endif /* _LINUX_SCHED_PRIO_H */
