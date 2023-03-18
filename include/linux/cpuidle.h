/*
 * cpuidle.h - a generic framework for CPU idle power management
 *
 * (C) 2007 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *          Shaohua Li <shaohua.li@intel.com>
 *          Adam Belay <abelay@novell.com>
 *
 * This code is licenced under the GPL.
 */

#ifndef _LINUX_CPUIDLE_H
#define _LINUX_CPUIDLE_H

#include <linux/percpu.h>
#include <linux/list.h>
#include <linux/hrtimer.h>

#define CPUIDLE_STATE_MAX	10
#define CPUIDLE_NAME_LEN	16
#define CPUIDLE_DESC_LEN	32

struct module;

struct cpuidle_device;
struct cpuidle_driver;


/****************************
 * CPUIDLE DEVICE INTERFACE *
 ****************************/

#define CPUIDLE_STATE_DISABLED_BY_USER		BIT(0)
#define CPUIDLE_STATE_DISABLED_BY_DRIVER	BIT(1)

struct cpuidle_state_usage {
	unsigned long long	disable;
	unsigned long long	usage;
	u64			time_ns;
	unsigned long long	above; /* Number of times it's been too deep */
	unsigned long long	below; /* Number of times it's been too shallow */
	unsigned long long	rejected; /* Number of times idle entry was rejected */
#ifdef CONFIG_SUSPEND
	unsigned long long	s2idle_usage;
	unsigned long long	s2idle_time; /* in US */
#endif
};

/*
 * IAMROOT, 2023.03.11:
 * ----- c state -------
 *  Intel의 C State
 *
 *  Intel에서 제조한 프로세서는 절전 모드에서 세 가지 C-state (C1, C2, C3)를 
 *  지원합니다.
 *
 *  C1 상태는 "Auto Halt" 또는 "MWAIT"라고도 불리며, 프로세서가 대기하면서 
 *  최소한의 전력을 소비하도록 하는 가장 간단한 절전 모드입니다. 프로세서는 
 *  일시 중지되어 있지만, 빠른 복귀를 위해 기다리고 있는 시스템 이벤트를 
 *  감지합니다.
 *
 *  C2 상태는 "Stop-Clock" 또는 "HALT"라고도 불리며, 프로세서의 클럭을 
 *  중지하고 캐시를 비우는 것을 포함하여 C1보다 더 많은 전력을 절약합니다. 
 *  프로세서는 일시 중지 상태이지만, C1보다 더 많은 시간을 소비하므로 
 *  기다리고 있는 이벤트를 탐지하기 위해 더 많은 시간이 걸립니다.
 *
 *  C3 상태는 "Deep Sleep" 또는 "Sleep"라고도 불리며, 프로세서의 코어 전압과 
 *  클럭을 낮추어서 더 많은 전력을 절약합니다. C3 상태는 대개 C2보다 더 깊은
 *  수면 상태이며, 프로세서는 기다리고 있는 이벤트를 탐지하기 위해 C2보다
 *  더 많은 시간이 걸립니다.
 *
 *  이 세 가지 C-state는 프로세서가 실행되는 로드와 같은 여러 가지 요소에 
 *  따라 다릅니다. 예를 들어, 높은 로드를 처리하는 경우 C1 상태가 더 많이
 *  사용됩니다. 그러나 낮은 로드를 처리하는 경우에는 C3 상태가 더 많이
 *  사용됩니다.
 *
 *  - C1 : core 정지 
 *    C2 : c1 + cache clear
 *    C3 : C2 + 전압 다운. option으로 timer off. 최대 절전
 *    C5 : core 전원 off
 * ---------------------
 *
 * - 디바이스 트리에서 아래 변수를 가져옮.
 *   1. "entry-latency-us" + "exit-latency-us" => idle_state->exit_latency
 *      또는 "wakeup-latency-us" => idle_state->exit_latency
 *   2. "min-residency-us" => idle_state->target_residency
 *   3. "local-timer-stop" 이 존재하면 CPUIDLE_FLAG_TIMER_STOP flags 설정
 */
struct cpuidle_state {
	char		name[CPUIDLE_NAME_LEN];
	char		desc[CPUIDLE_DESC_LEN];

	s64		exit_latency_ns;
	s64		target_residency_ns;
	unsigned int	flags;
	unsigned int	exit_latency; /* in US */
	int		power_usage; /* in mW */
	unsigned int	target_residency; /* in US */

	/*
	 * IAMROOT, 2023.03.11:
	 * - enter 연결함수: psci_enter_idle_state
	 */
	int (*enter)	(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index);

	int (*enter_dead) (struct cpuidle_device *dev, int index);

	/*
	 * CPUs execute ->enter_s2idle with the local tick or entire timekeeping
	 * suspended, so it must not re-enable interrupts at any point (even
	 * temporarily) or attempt to change states of clock event devices.
	 *
	 * This callback may point to the same function as ->enter if all of
	 * the above requirements are met by it.
	 */
	/*
	 * IAMROOT. 2023.03.11:
	 * - google-translate
	 *   CPU는 로컬 틱 또는 전체 시간 유지가 일시 중단된 상태에서 ->enter_s2idle을
	 *   실행하므로 어느 시점에서든(일시적으로라도) 인터럽트를 다시 활성화하거나 클록
	 *   이벤트 장치의 상태를 변경하려고 시도해서는 안 됩니다.
	 *
	 *   이 콜백은 위의 모든 요구
	 *   사항이 충족되는 경우 -> enter와 동일한 기능을 가리킬 수 있습니다.
	 *
	 * - enter_s2idle 연결 함수: psci_enter_s2idle_domain_idle_state
	 */
	int (*enter_s2idle)(struct cpuidle_device *dev,
			    struct cpuidle_driver *drv,
			    int index);
};

/* Idle State Flags */
#define CPUIDLE_FLAG_NONE       	(0x00)
#define CPUIDLE_FLAG_POLLING		BIT(0) /* polling state */
/*
 * IAMROOT, 2023.03.18:
 * - cluster로 묶은 cpu.
 */
#define CPUIDLE_FLAG_COUPLED		BIT(1) /* state applies to multiple cpus */

/*
 * IAMROOT, 2023.03.18:
 * - dt 
 *   local-timer-stop prop
 */
#define CPUIDLE_FLAG_TIMER_STOP 	BIT(2) /* timer is stopped on this state */
#define CPUIDLE_FLAG_UNUSABLE		BIT(3) /* avoid using this state */
#define CPUIDLE_FLAG_OFF		BIT(4) /* disable this state by default */
#define CPUIDLE_FLAG_TLB_FLUSHED	BIT(5) /* idle-state flushes TLBs */
#define CPUIDLE_FLAG_RCU_IDLE		BIT(6) /* idle-state takes care of RCU */

struct cpuidle_device_kobj;
struct cpuidle_state_kobj;
struct cpuidle_driver_kobj;

struct cpuidle_device {
	unsigned int		registered:1;
	unsigned int		enabled:1;
	unsigned int		poll_time_limit:1;
	unsigned int		cpu;
	ktime_t			next_hrtimer;

	int			last_state_idx;
/*
 * IAMROOT, 2023.03.18:
 * - 마지막에 했던 idle시간. 실패했을 경우 0
 */
	u64			last_residency_ns;
	u64			poll_limit_ns;
	u64			forced_idle_latency_limit_ns;
	struct cpuidle_state_usage	states_usage[CPUIDLE_STATE_MAX];
	struct cpuidle_state_kobj *kobjs[CPUIDLE_STATE_MAX];
	struct cpuidle_driver_kobj *kobj_driver;
	struct cpuidle_device_kobj *kobj_dev;
	struct list_head 	device_list;

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
	cpumask_t		coupled_cpus;
	struct cpuidle_coupled	*coupled;
#endif
};

DECLARE_PER_CPU(struct cpuidle_device *, cpuidle_devices);
DECLARE_PER_CPU(struct cpuidle_device, cpuidle_dev);

/****************************
 * CPUIDLE DRIVER INTERFACE *
 ****************************/

struct cpuidle_driver {
	const char		*name;
	struct module 		*owner;

        /* used by the cpuidle framework to setup the broadcast timer */
	unsigned int            bctimer:1;
	/* states array must be ordered in decreasing power consumption */
	struct cpuidle_state	states[CPUIDLE_STATE_MAX];
	int			state_count;
	int			safe_state_index;

	/* the driver handles the cpus in cpumask */
	struct cpumask		*cpumask;

	/* preferred governor to switch at register time */
	const char		*governor;
};

#ifdef CONFIG_CPU_IDLE
extern void disable_cpuidle(void);
extern bool cpuidle_not_available(struct cpuidle_driver *drv,
				  struct cpuidle_device *dev);

extern int cpuidle_select(struct cpuidle_driver *drv,
			  struct cpuidle_device *dev,
			  bool *stop_tick);
extern int cpuidle_enter(struct cpuidle_driver *drv,
			 struct cpuidle_device *dev, int index);
extern void cpuidle_reflect(struct cpuidle_device *dev, int index);
extern u64 cpuidle_poll_time(struct cpuidle_driver *drv,
			     struct cpuidle_device *dev);

extern int cpuidle_register_driver(struct cpuidle_driver *drv);
extern struct cpuidle_driver *cpuidle_get_driver(void);
extern void cpuidle_driver_state_disabled(struct cpuidle_driver *drv, int idx,
					bool disable);
extern void cpuidle_unregister_driver(struct cpuidle_driver *drv);
extern int cpuidle_register_device(struct cpuidle_device *dev);
extern void cpuidle_unregister_device(struct cpuidle_device *dev);
extern int cpuidle_register(struct cpuidle_driver *drv,
			    const struct cpumask *const coupled_cpus);
extern void cpuidle_unregister(struct cpuidle_driver *drv);
extern void cpuidle_pause_and_lock(void);
extern void cpuidle_resume_and_unlock(void);
extern void cpuidle_pause(void);
extern void cpuidle_resume(void);
extern int cpuidle_enable_device(struct cpuidle_device *dev);
extern void cpuidle_disable_device(struct cpuidle_device *dev);
extern int cpuidle_play_dead(void);

extern struct cpuidle_driver *cpuidle_get_cpu_driver(struct cpuidle_device *dev);
static inline struct cpuidle_device *cpuidle_get_device(void)
{return __this_cpu_read(cpuidle_devices); }
#else
static inline void disable_cpuidle(void) { }
static inline bool cpuidle_not_available(struct cpuidle_driver *drv,
					 struct cpuidle_device *dev)
{return true; }
static inline int cpuidle_select(struct cpuidle_driver *drv,
				 struct cpuidle_device *dev, bool *stop_tick)
{return -ENODEV; }
static inline int cpuidle_enter(struct cpuidle_driver *drv,
				struct cpuidle_device *dev, int index)
{return -ENODEV; }
static inline void cpuidle_reflect(struct cpuidle_device *dev, int index) { }
static inline u64 cpuidle_poll_time(struct cpuidle_driver *drv,
			     struct cpuidle_device *dev)
{return 0; }
static inline int cpuidle_register_driver(struct cpuidle_driver *drv)
{return -ENODEV; }
static inline struct cpuidle_driver *cpuidle_get_driver(void) {return NULL; }
static inline void cpuidle_driver_state_disabled(struct cpuidle_driver *drv,
					       int idx, bool disable) { }
static inline void cpuidle_unregister_driver(struct cpuidle_driver *drv) { }
static inline int cpuidle_register_device(struct cpuidle_device *dev)
{return -ENODEV; }
static inline void cpuidle_unregister_device(struct cpuidle_device *dev) { }
static inline int cpuidle_register(struct cpuidle_driver *drv,
				   const struct cpumask *const coupled_cpus)
{return -ENODEV; }
static inline void cpuidle_unregister(struct cpuidle_driver *drv) { }
static inline void cpuidle_pause_and_lock(void) { }
static inline void cpuidle_resume_and_unlock(void) { }
static inline void cpuidle_pause(void) { }
static inline void cpuidle_resume(void) { }
static inline int cpuidle_enable_device(struct cpuidle_device *dev)
{return -ENODEV; }
static inline void cpuidle_disable_device(struct cpuidle_device *dev) { }
static inline int cpuidle_play_dead(void) {return -ENODEV; }
static inline struct cpuidle_driver *cpuidle_get_cpu_driver(
	struct cpuidle_device *dev) {return NULL; }
static inline struct cpuidle_device *cpuidle_get_device(void) {return NULL; }
#endif

#ifdef CONFIG_CPU_IDLE
extern int cpuidle_find_deepest_state(struct cpuidle_driver *drv,
				      struct cpuidle_device *dev,
				      u64 latency_limit_ns);
extern int cpuidle_enter_s2idle(struct cpuidle_driver *drv,
				struct cpuidle_device *dev);
extern void cpuidle_use_deepest_state(u64 latency_limit_ns);
#else
static inline int cpuidle_find_deepest_state(struct cpuidle_driver *drv,
					     struct cpuidle_device *dev,
					     u64 latency_limit_ns)
{return -ENODEV; }
static inline int cpuidle_enter_s2idle(struct cpuidle_driver *drv,
				       struct cpuidle_device *dev)
{return -ENODEV; }
static inline void cpuidle_use_deepest_state(u64 latency_limit_ns)
{
}
#endif

/* kernel/sched/idle.c */
extern void sched_idle_set_state(struct cpuidle_state *idle_state);
extern void default_idle_call(void);

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
void cpuidle_coupled_parallel_barrier(struct cpuidle_device *dev, atomic_t *a);
#else
static inline void cpuidle_coupled_parallel_barrier(struct cpuidle_device *dev, atomic_t *a)
{
}
#endif

#if defined(CONFIG_CPU_IDLE) && defined(CONFIG_ARCH_HAS_CPU_RELAX)
void cpuidle_poll_state_init(struct cpuidle_driver *drv);
#else
static inline void cpuidle_poll_state_init(struct cpuidle_driver *drv) {}
#endif

/******************************
 * CPUIDLE GOVERNOR INTERFACE *
 ******************************/

struct cpuidle_governor {
	char			name[CPUIDLE_NAME_LEN];
	struct list_head 	governor_list;
	unsigned int		rating;

	int  (*enable)		(struct cpuidle_driver *drv,
					struct cpuidle_device *dev);
	void (*disable)		(struct cpuidle_driver *drv,
					struct cpuidle_device *dev);

	int  (*select)		(struct cpuidle_driver *drv,
					struct cpuidle_device *dev,
					bool *stop_tick);
	void (*reflect)		(struct cpuidle_device *dev, int index);
};

extern int cpuidle_register_governor(struct cpuidle_governor *gov);
extern s64 cpuidle_governor_latency_req(unsigned int cpu);

#define __CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter,			\
				idx,					\
				state,					\
				is_retention)				\
({									\
	int __ret = 0;							\
									\
	if (!idx) {							\
		cpu_do_idle();						\
		return idx;						\
	}								\
									\
	if (!is_retention)						\
		__ret =  cpu_pm_enter();				\
	if (!__ret) {							\
		__ret = low_level_idle_enter(state);			\
		if (!is_retention)					\
			cpu_pm_exit();					\
	}								\
									\
	__ret ? -1 : idx;						\
})

#define CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, idx, 0)

#define CPU_PM_CPU_IDLE_ENTER_RETENTION(low_level_idle_enter, idx)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, idx, 1)

#define CPU_PM_CPU_IDLE_ENTER_PARAM(low_level_idle_enter, idx, state)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, state, 0)

#define CPU_PM_CPU_IDLE_ENTER_RETENTION_PARAM(low_level_idle_enter, idx, state)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, state, 1)

#endif /* _LINUX_CPUIDLE_H */
