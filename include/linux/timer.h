/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TIMER_H
#define _LINUX_TIMER_H

#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/stddef.h>
#include <linux/debugobjects.h>
#include <linux/stringify.h>

struct timer_list {
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct hlist_node	entry;
	unsigned long		expires;
	void			(*function)(struct timer_list *);
/*
 * IAMROOT, 2022.09.03:
 * - TIMER_BASEMASK 참고.
 *   cpu id(TIMER_CPUMASK. 18bits)와 같이 field를 사용한다.
 *   ex)
 *   timer->flags = flags | raw_smp_processor_id();
 */
	u32			flags;

#ifdef CONFIG_LOCKDEP
	struct lockdep_map	lockdep_map;
#endif
};

#ifdef CONFIG_LOCKDEP
/*
 * NB: because we have to copy the lockdep_map, setting the lockdep_map key
 * (second argument) here is required, otherwise it could be initialised to
 * the copy of the lockdep_map later! We use the pointer to and the string
 * "<file>:<line>" as the key resp. the name of the lockdep_map.
 */
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)				\
	.lockdep_map = STATIC_LOCKDEP_MAP_INIT(_kn, &_kn),
#else
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)
#endif

/**
 * @TIMER_DEFERRABLE: A deferrable timer will work normally when the
 * system is busy, but will not cause a CPU to come out of idle just
 * to service it; instead, the timer will be serviced when the CPU
 * eventually wakes up with a subsequent non-deferrable timer.
 *
 * @TIMER_IRQSAFE: An irqsafe timer is executed with IRQ disabled and
 * it's safe to wait for the completion of the running instance from
 * IRQ handlers, for example, by calling del_timer_sync().
 *
 * Note: The irq disabled callback execution is a special case for
 * workqueue locking issues. It's not meant for executing random crap
 * with interrupts disabled. Abuse is monitored!
 *
 * @TIMER_PINNED: A pinned timer will not be affected by any timer
 * placement heuristics (like, NOHZ) and will always expire on the CPU
 * on which the timer was enqueued.
 *
 * Note: Because enqueuing of timers can migrate the timer from one
 * CPU to another, pinned timers are not guaranteed to stay on the
 * initialy selected CPU.  They move to the CPU on which the enqueue
 * function is invoked via mod_timer() or add_timer().  If the timer
 * should be placed on a particular CPU, then add_timer_on() has to be
 * used.
 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   @TIMER_DEFERRABLE: 지연 가능한 타이머는 시스템이 사용 중일 때 정상적으로
 *   작동하지만 CPU를 서비스하기 위해 유휴 상태에서 벗어나게 하지는 않습니다.
 *   대신, CPU가 나중에 지연되지 않는 타이머로 깨어날 때 타이머가 서비스됩니다. 
 *
 *   @TIMER_IRQSAFE: irqsafe 타이머는 IRQ가 비활성화된 상태에서 실행되며, 예를 들어
 *   del_timer_sync()를 호출하여 IRQ 처리기에서 실행 중인 인스턴스가 완료될 때까지
 *   기다리는 것이 안전합니다.
 *
 *   Note: irq 비활성화된 콜백 실행은 작업 대기열 잠금 문제의 특별한 경우입니다.
 *   인터럽트가 비활성화된 상태에서 무작위 쓰레기를 실행하기 위한 것이 아닙니다.
 *   학대가 모니터링됩니다!
 *
 *   @TIMER_PINNED: 고정된 타이머는 타이머 배치 휴리스틱(예: NOHZ)의 영향을 받지 
 *   않으며 타이머가 대기열에 추가된 CPU에서 항상 만료됩니다.
 *
 *  Note: 타이머를 대기열에 넣으면 한 CPU에서 다른 CPU로 타이머를 마이그레이션할
 *  수 있으므로 고정된 타이머가 처음에 선택한 CPU에 유지된다는 보장은 없습니다.
 *  enqueue 기능이 mod_timer() 또는 add_timer()를 통해 호출되는 CPU로 이동합니다.
 *  타이머를 특정 CPU에 배치해야 하는 경우 add_timer_on()을 사용해야 합니다.
 *
 *  - field
 *  | 10bits    | 4bits   | 18bits |
 *  | timer idx | flags   | cpu id |
 */
#define TIMER_CPUMASK		0x0003FFFF
#define TIMER_MIGRATING		0x00040000
#define TIMER_BASEMASK		(TIMER_CPUMASK | TIMER_MIGRATING)
#define TIMER_DEFERRABLE	0x00080000
#define TIMER_PINNED		0x00100000
#define TIMER_IRQSAFE		0x00200000
#define TIMER_INIT_FLAGS	(TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE)
#define TIMER_ARRAYSHIFT	22
#define TIMER_ARRAYMASK		0xFFC00000

#define TIMER_TRACE_FLAGMASK	(TIMER_MIGRATING | TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE)

/*
 * IAMROOT, 2022.09.03:
 * - compile time시 timer 초기화.
 */
#define __TIMER_INITIALIZER(_function, _flags) {		\
		.entry = { .next = TIMER_ENTRY_STATIC },	\
		.function = (_function),			\
		.flags = (_flags),				\
		__TIMER_LOCKDEP_MAP_INITIALIZER(		\
			__FILE__ ":" __stringify(__LINE__))	\
	}

/*
 * IAMROOT, 2022.09.03:
 * - compile time시 timer 초기화.
 * - runtime시는 timer_setup
 */
#define DEFINE_TIMER(_name, _function)				\
	struct timer_list _name =				\
		__TIMER_INITIALIZER(_function, 0)

/*
 * LOCKDEP and DEBUG timer interfaces.
 */
void init_timer_key(struct timer_list *timer,
		    void (*func)(struct timer_list *), unsigned int flags,
		    const char *name, struct lock_class_key *key);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
extern void init_timer_on_stack_key(struct timer_list *timer,
				    void (*func)(struct timer_list *),
				    unsigned int flags, const char *name,
				    struct lock_class_key *key);
#else
static inline void init_timer_on_stack_key(struct timer_list *timer,
					   void (*func)(struct timer_list *),
					   unsigned int flags,
					   const char *name,
					   struct lock_class_key *key)
{
	init_timer_key(timer, func, flags, name, key);
}
#endif

#ifdef CONFIG_LOCKDEP
#define __init_timer(_timer, _fn, _flags)				\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_key((_timer), (_fn), (_flags), #_timer, &__key);\
	} while (0)

#define __init_timer_on_stack(_timer, _fn, _flags)			\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_on_stack_key((_timer), (_fn), (_flags),	\
					#_timer, &__key);		 \
	} while (0)
#else
/*
 * IAMROOT, 2022.09.03:
 * - @_timer를 초기화한다.
 */
#define __init_timer(_timer, _fn, _flags)				\
	init_timer_key((_timer), (_fn), (_flags), NULL, NULL)
#define __init_timer_on_stack(_timer, _fn, _flags)			\
	init_timer_on_stack_key((_timer), (_fn), (_flags), NULL, NULL)
#endif

/**
 * timer_setup - prepare a timer for first use
 * @timer: the timer in question
 * @callback: the function to call when timer expires
 * @flags: any TIMER_* flags
 *
 * Regular timer initialization should use either DEFINE_TIMER() above,
 * or timer_setup(). For timers on the stack, timer_setup_on_stack() must
 * be used and must be balanced with a call to destroy_timer_on_stack().
 */
/*
 * IAMROOT, 2022.09.03:
 * - runtime시 timer초기화.
 *   실제 timer 등록은 add_timer를 수행해야한다.
 */
#define timer_setup(timer, callback, flags)			\
	__init_timer((timer), (callback), (flags))

#define timer_setup_on_stack(timer, callback, flags)		\
	__init_timer_on_stack((timer), (callback), (flags))

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
extern void destroy_timer_on_stack(struct timer_list *timer);
#else
static inline void destroy_timer_on_stack(struct timer_list *timer) { }
#endif

#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)

/**
 * timer_pending - is a timer pending?
 * @timer: the timer in question
 *
 * timer_pending will tell whether a given timer is currently pending,
 * or not. Callers must ensure serialization wrt. other operations done
 * to this timer, eg. interrupt contexts, or other CPUs on SMP.
 *
 * return value: 1 if the timer is pending, 0 if not.
 */
/*
 * IAMROOT, 2022.09.03:
 * @return value 1 if the timer is pending, 0 if not.
 * - @timer가 wheel에 등록이 되있는지 안되있는지 확인한다.
 * - list의 pprev가 있다면 등록이 되있다는 의미.
 *   없다면 expire가 됬거나 등록이 안 됬다는 의미.
 */
static inline int timer_pending(const struct timer_list * timer)
{
	return !hlist_unhashed_lockless(&timer->entry);
}

extern void add_timer_on(struct timer_list *timer, int cpu);
extern int del_timer(struct timer_list * timer);
extern int mod_timer(struct timer_list *timer, unsigned long expires);
extern int mod_timer_pending(struct timer_list *timer, unsigned long expires);
extern int timer_reduce(struct timer_list *timer, unsigned long expires);

/*
 * The jiffies value which is added to now, when there is no timer
 * in the timer wheel:
 */
/*
 * IAMROOT, 2022.09.03:
 * - 2^30 - 1 
 */
#define NEXT_TIMER_MAX_DELTA	((1UL << 30) - 1)

extern void add_timer(struct timer_list *timer);

extern int try_to_del_timer_sync(struct timer_list *timer);

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
  extern int del_timer_sync(struct timer_list *timer);
#else
# define del_timer_sync(t)		del_timer(t)
#endif

#define del_singleshot_timer_sync(t) del_timer_sync(t)

extern void init_timers(void);
struct hrtimer;
extern enum hrtimer_restart it_real_fn(struct hrtimer *);

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
struct ctl_table;

extern unsigned int sysctl_timer_migration;
int timer_migration_handler(struct ctl_table *table, int write,
			    void *buffer, size_t *lenp, loff_t *ppos);
#endif

unsigned long __round_jiffies(unsigned long j, int cpu);
unsigned long __round_jiffies_relative(unsigned long j, int cpu);
unsigned long round_jiffies(unsigned long j);
unsigned long round_jiffies_relative(unsigned long j);

unsigned long __round_jiffies_up(unsigned long j, int cpu);
unsigned long __round_jiffies_up_relative(unsigned long j, int cpu);
unsigned long round_jiffies_up(unsigned long j);
unsigned long round_jiffies_up_relative(unsigned long j);

#ifdef CONFIG_HOTPLUG_CPU
int timers_prepare_cpu(unsigned int cpu);
int timers_dead_cpu(unsigned int cpu);
#else
#define timers_prepare_cpu	NULL
#define timers_dead_cpu		NULL
#endif

#endif
