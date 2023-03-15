/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TICK_SCHED_H
#define _TICK_SCHED_H

#include <linux/hrtimer.h>

enum tick_device_mode {
	TICKDEV_MODE_PERIODIC,
	TICKDEV_MODE_ONESHOT,
};

struct tick_device {
	struct clock_event_device *evtdev;
	enum tick_device_mode mode;
};

enum tick_nohz_mode {
	NOHZ_MODE_INACTIVE,
	NOHZ_MODE_LOWRES,
	NOHZ_MODE_HIGHRES,
};

/**
 * struct tick_sched - sched tick emulation and no idle tick control/stats
 * @sched_timer:	hrtimer to schedule the periodic tick in high
 *			resolution mode
 * @check_clocks:	Notification mechanism about clocksource changes
 * @nohz_mode:		Mode - one state of tick_nohz_mode
 * @inidle:		Indicator that the CPU is in the tick idle mode
 * @tick_stopped:	Indicator that the idle tick has been stopped
 * @idle_active:	Indicator that the CPU is actively in the tick idle mode;
 *			it is reset during irq handling phases.
 * @do_timer_lst:	CPU was the last one doing do_timer before going idle
 * @got_idle_tick:	Tick timer function has run with @inidle set
 * @last_tick:		Store the last tick expiry time when the tick
 *			timer is modified for nohz sleeps. This is necessary
 *			to resume the tick timer operation in the timeline
 *			when the CPU returns from nohz sleep.
 * @next_tick:		Next tick to be fired when in dynticks mode.
 * @idle_jiffies:	jiffies at the entry to idle for idle time accounting
 * @idle_calls:		Total number of idle calls
 * @idle_sleeps:	Number of idle calls, where the sched tick was stopped
 * @idle_entrytime:	Time when the idle call was entered
 * @idle_waketime:	Time when the idle was interrupted
 * @idle_exittime:	Time when the idle state was left
 * @idle_sleeptime:	Sum of the time slept in idle with sched tick stopped
 * @iowait_sleeptime:	Sum of the time slept in idle with sched tick stopped, with IO outstanding
 * @timer_expires:	Anticipated timer expiration time (in case sched tick is stopped)
 * @timer_expires_base:	Base time clock monotonic for @timer_expires
 * @next_timer:		Expiry time of next expiring timer for debugging purpose only
 * @tick_dep_mask:	Tick dependency mask - is set, if someone needs the tick
 */
/*
 * IAMROOT, 2023.03.15:
 * - papago
 *   struct tick_sched - 스케줄된 tick 에뮬레이션 및 idle tick 제어/통계가 없습니다.
 *   @sched_timer: 고해상도 mode에서 주기적인 tick을 예약하는 hrtimer.
 *   @check_clocks: 클럭 소스 변경에 대한 알림 메커니즘입니다.
 *   @nohz_mode: mode - tick_nohz_mode의 한 상태입니다.
 *   @inidle: CPU가 tick idle mode에 있음을 나타냅니다.
 *   @tick_stopped: idle tick이 중지되었음을 나타내는 표시기입니다.
 *   @idle_active: CPU가 tick idle mode에 있음을 나타내는 표시기이며 irq 처리 단계 
 *   중에 재설정됩니다.
 *   @do_timer_lst: CPU는 idle 상태가 되기 전에 마지막으로 do_timer를 수행했습니다.
 *   @got_idle_tick: @inidle이 설정된 상태에서 tick 타이머 기능이 실행되었습니다.
 *   @last_tick: nohz sleeps에 대해 ticktimer가 수정된 경우 마지막 tick 만료 시간을
 *   저장합니다. 이는 CPU가 nohz 절전 mode에서 복귀할 때 타임라인에서 tick 타이머 작업을
 *   재개하는 데 필요합니다.
 *   @next_tick: dynticks mode일 때 실행할 다음 tick.
 *   @idle_jiffies: idle 시간 계산을 위해 idle 항목에서 jiffies.
 *   @idle_calls: 총 idle call 수입니다.
 *   @idle_sleeps: sched tick이 중지된 idle 호출 수입니다.
 *   @idle_entrytime: idle call가 입력된 시간입니다.
 *   @idle_waketime: idle가 중단된 시간입니다.
 *   @idle_exittime: idle 상태를 유지한 시간.
 *   @idle_sleeptime: sched tick이 중지된 상태에서 idle 상태로 잠든 시간의 합계입니다.
 *   @iowait_sleeptime: sched tick이 중지되고 IO 미해결 상태에서 idle 상태로 잠든
 *   시간의 합계입니다.
 *   @timer_expires: 예상 타이머 만료 시간(sched tick이 중지된 경우).
 *   @timer_expires_base: @timer_expires에 대한 단조로운 기본 시간 시계.
 *   @next_timer: 디버깅 목적으로만 사용되는 다음 만료 타이머의 만료 시간입니다.
 *   @tick_dep_mask: tick 종속성 마스크 - 누군가 tick이 필요한 경우 설정됩니다. 
 *
 * --- chat gpt ----
 * - inidle, idle_active 차이점.
 *   "inidle": CPU가 현재 유휴 상태인지 여부를 나타내는 비트 필드 플래그입니다. CPU가
 *   이미 유휴 상태일 때 타이머 인터럽트 처리기에서 불필요한 처리를 방지하는 데
 *   사용됩니다. CPU가 유휴 상태에 들어가면 플래그가 1로 설정되고 CPU가 깨어나면
 *   해제됩니다.
 *
 *   "idle_active": idle tick(즉, CPU를 idle 상태에서 깨우는 타이머 tick)이 현재
 *   활성화되어 있는지 여부를 나타내는 비트 필드 플래그입니다. 이 플래그는 커널의 nohz
 *   하위 시스템에서 nohz mode로 들어가는 것이 안전한지(즉, idle tick 비활성화) 여부를
 *   결정하는 데 사용됩니다. idle tick이 활성화되면 플래그가 1로 설정되고 idle tick이
 *   비활성화되면 해제됩니다.
 * -----------------
 */
struct tick_sched {
	struct hrtimer			sched_timer;
	unsigned long			check_clocks;
	enum tick_nohz_mode		nohz_mode;

	unsigned int			inidle		: 1;
	unsigned int			tick_stopped	: 1;
	unsigned int			idle_active	: 1;
	unsigned int			do_timer_last	: 1;
	unsigned int			got_idle_tick	: 1;

	ktime_t				last_tick;
	ktime_t				next_tick;
	unsigned long			idle_jiffies;
	unsigned long			idle_calls;
	unsigned long			idle_sleeps;
	ktime_t				idle_entrytime;
	ktime_t				idle_waketime;
	ktime_t				idle_exittime;
	ktime_t				idle_sleeptime;
	ktime_t				iowait_sleeptime;
	unsigned long			last_jiffies;
	u64				timer_expires;
	u64				timer_expires_base;
	u64				next_timer;
	ktime_t				idle_expires;
	atomic_t			tick_dep_mask;
};

extern struct tick_sched *tick_get_tick_sched(int cpu);

extern void tick_setup_sched_timer(void);
#if defined CONFIG_NO_HZ_COMMON || defined CONFIG_HIGH_RES_TIMERS
extern void tick_cancel_sched_timer(int cpu);
#else
static inline void tick_cancel_sched_timer(int cpu) { }
#endif

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
extern int __tick_broadcast_oneshot_control(enum tick_broadcast_state state);
#else
static inline int
__tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	return -EBUSY;
}
#endif

#endif
