/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * Copyright IBM Corporation, 2001
 *
 * Author: Dipankar Sarma <dipankar@in.ibm.com>
 *
 * Based on the original work by Paul McKenney <paulmck@vnet.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */

/*
 * IAMROOT, 2023.07.17:
 * - 참고
 *	 http://lse.sourceforge.net/locking/rcupdate.html
 */
#ifndef __LINUX_RCUPDATE_H
#define __LINUX_RCUPDATE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/bottom_half.h>
#include <linux/lockdep.h>
#include <asm/processor.h>
#include <linux/cpumask.h>

#define ULONG_CMP_GE(a, b)	(ULONG_MAX / 2 >= (a) - (b))
/*
 * IAMROOT, 2023.07.22:
 * - a < b
 *   a = 1, b = 2 => true 
 *   b = 2, a = 1 => false
 *   b = 1, b = 1 => false
 */
#define ULONG_CMP_LT(a, b)	(ULONG_MAX / 2 < (a) - (b))
#define ulong2long(a)		(*(long *)(&(a)))
#define USHORT_CMP_GE(a, b)	(USHRT_MAX / 2 >= (unsigned short)((a) - (b)))
#define USHORT_CMP_LT(a, b)	(USHRT_MAX / 2 < (unsigned short)((a) - (b)))

/* Exported common interfaces */
void call_rcu(struct rcu_head *head, rcu_callback_t func);
void rcu_barrier_tasks(void);
void rcu_barrier_tasks_rude(void);
void synchronize_rcu(void);

#ifdef CONFIG_PREEMPT_RCU

void __rcu_read_lock(void);
void __rcu_read_unlock(void);

/*
 * Defined as a macro as it is a very low level header included from
 * areas that don't even know about current.  This gives the rcu_read_lock()
 * nesting depth, but makes sense only if CONFIG_PREEMPT_RCU -- in other
 * types of kernel builds, the rcu_read_lock() nesting depth is unknowable.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   현재도 모르는 영역에서 포함된 매우 낮은 수준의 헤더이므로 매크로로
 *   정의됩니다. 이것은 rcu_read_lock() 중첩 깊이를 제공하지만
 *   CONFIG_PREEMPT_RCU가 다른 유형의 커널 빌드에서 rcu_read_lock()
 *   중첩 깊이를 알 수 없는 경우에만 의미가 있습니다.
 */
#define rcu_preempt_depth() READ_ONCE(current->rcu_read_lock_nesting)

#else /* #ifdef CONFIG_PREEMPT_RCU */

#ifdef CONFIG_TINY_RCU
#define rcu_read_unlock_strict() do { } while (0)
#else
void rcu_read_unlock_strict(void);
#endif

static inline void __rcu_read_lock(void)
{
	preempt_disable();
}

static inline void __rcu_read_unlock(void)
{
	preempt_enable();
	rcu_read_unlock_strict();
}

static inline int rcu_preempt_depth(void)
{
	return 0;
}

#endif /* #else #ifdef CONFIG_PREEMPT_RCU */

/* Internal to kernel */
void rcu_init(void);
extern int rcu_scheduler_active __read_mostly;
void rcu_sched_clock_irq(int user);
void rcu_report_dead(unsigned int cpu);
void rcutree_migrate_callbacks(int cpu);

#ifdef CONFIG_TASKS_RCU_GENERIC
void rcu_init_tasks_generic(void);
#else
static inline void rcu_init_tasks_generic(void) { }
#endif

#ifdef CONFIG_RCU_STALL_COMMON
void rcu_sysrq_start(void);
void rcu_sysrq_end(void);
#else /* #ifdef CONFIG_RCU_STALL_COMMON */
static inline void rcu_sysrq_start(void) { }
static inline void rcu_sysrq_end(void) { }
#endif /* #else #ifdef CONFIG_RCU_STALL_COMMON */

#ifdef CONFIG_NO_HZ_FULL
void rcu_user_enter(void);
void rcu_user_exit(void);
#else
static inline void rcu_user_enter(void) { }
static inline void rcu_user_exit(void) { }
#endif /* CONFIG_NO_HZ_FULL */

#ifdef CONFIG_RCU_NOCB_CPU
void rcu_init_nohz(void);
int rcu_nocb_cpu_offload(int cpu);
int rcu_nocb_cpu_deoffload(int cpu);
void rcu_nocb_flush_deferred_wakeup(void);
#else /* #ifdef CONFIG_RCU_NOCB_CPU */
static inline void rcu_init_nohz(void) { }
static inline int rcu_nocb_cpu_offload(int cpu) { return -EINVAL; }
static inline int rcu_nocb_cpu_deoffload(int cpu) { return 0; }
static inline void rcu_nocb_flush_deferred_wakeup(void) { }
#endif /* #else #ifdef CONFIG_RCU_NOCB_CPU */

/**
 * RCU_NONIDLE - Indicate idle-loop code that needs RCU readers
 * @a: Code that RCU needs to pay attention to.
 *
 * RCU read-side critical sections are forbidden in the inner idle loop,
 * that is, between the rcu_idle_enter() and the rcu_idle_exit() -- RCU
 * will happily ignore any such read-side critical sections.  However,
 * things like powertop need tracepoints in the inner idle loop.
 *
 * This macro provides the way out:  RCU_NONIDLE(do_something_with_RCU())
 * will tell RCU that it needs to pay attention, invoke its argument
 * (in this example, calling the do_something_with_RCU() function),
 * and then tell RCU to go back to ignoring this CPU.  It is permissible
 * to nest RCU_NONIDLE() wrappers, but not indefinitely (but the limit is
 * on the order of a million or so, even on 32-bit systems).  It is
 * not legal to block within RCU_NONIDLE(), nor is it permissible to
 * transfer control either into or out of RCU_NONIDLE()'s statement.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU_NONIDLE - RCU 리더가 필요한 유휴 루프 코드를 나타냅니다.
 *   @a: RCU가 주의를 기울여야 하는 코드입니다.
 *
 *   RCU 읽기 측 임계 섹션은 내부 유휴 루프, 즉 rcu_idle_enter()와
 *   rcu_idle_exit() 사이에서 금지됩니다. RCU는 그러한 읽기 측
 *   임계 섹션을 기꺼이 무시합니다. 그러나 powertop과 같은 것에는
 *   내부 유휴 루프에 추적점이 필요합니다.
 *
 *   이 매크로는 탈출구를 제공합니다.
 *   RCU_NONIDLE(do_something_with_RCU())는 RCU에 주의를 기울여야
 *   한다고 알리고 인수(이 예에서는 do_something_with_RCU() 함수 호출)를
 *   호출한 다음 RCU에 이 CPU를 무시하도록 다시 지시합니다. RCU_NONIDLE()
 *   래퍼를 중첩하는 것은 허용되지만 무한정은 아닙니다(그러나 제한은 32비트
 *   시스템에서도 백만 개 정도입니다). RCU_NONIDLE() 내에서 차단하는 것은
 *   합법적이지 않으며 RCU_NONIDLE()의 명령문 안팎으로 제어를 이전하는 것도
 *   허용되지 않습니다.
 */
#define RCU_NONIDLE(a) \
	do { \
		rcu_irq_enter_irqson(); \
		do { a; } while (0); \
		rcu_irq_exit_irqson(); \
	} while (0)

/*
 * Note a quasi-voluntary context switch for RCU-tasks's benefit.
 * This is a macro rather than an inline function to avoid #include hell.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU 작업의 이점을 위한 준 자발적 컨텍스트 전환에 유의하십시오.
 *   이것은 #include 지옥을 피하기 위한 인라인 함수가 아닌 매크로입니다.
 */
#ifdef CONFIG_TASKS_RCU_GENERIC

# ifdef CONFIG_TASKS_RCU
# define rcu_tasks_classic_qs(t, preempt)				\
	do {								\
		if (!(preempt) && READ_ONCE((t)->rcu_tasks_holdout))	\
			WRITE_ONCE((t)->rcu_tasks_holdout, false);	\
	} while (0)
void call_rcu_tasks(struct rcu_head *head, rcu_callback_t func);
void synchronize_rcu_tasks(void);
# else
# define rcu_tasks_classic_qs(t, preempt) do { } while (0)
# define call_rcu_tasks call_rcu
# define synchronize_rcu_tasks synchronize_rcu
# endif

# ifdef CONFIG_TASKS_TRACE_RCU
# define rcu_tasks_trace_qs(t)						\
	do {								\
		if (!likely(READ_ONCE((t)->trc_reader_checked)) &&	\
		    !unlikely(READ_ONCE((t)->trc_reader_nesting))) {	\
			smp_store_release(&(t)->trc_reader_checked, true); \
			smp_mb(); /* Readers partitioned by store. */	\
		}							\
	} while (0)
# else
# define rcu_tasks_trace_qs(t) do { } while (0)
# endif

#define rcu_tasks_qs(t, preempt)					\
do {									\
	rcu_tasks_classic_qs((t), (preempt));				\
	rcu_tasks_trace_qs((t));					\
} while (0)

# ifdef CONFIG_TASKS_RUDE_RCU
void call_rcu_tasks_rude(struct rcu_head *head, rcu_callback_t func);
void synchronize_rcu_tasks_rude(void);
# endif

#define rcu_note_voluntary_context_switch(t) rcu_tasks_qs(t, false)
void exit_tasks_rcu_start(void);
void exit_tasks_rcu_finish(void);
#else /* #ifdef CONFIG_TASKS_RCU_GENERIC */
#define rcu_tasks_qs(t, preempt) do { } while (0)
#define rcu_note_voluntary_context_switch(t) do { } while (0)
#define call_rcu_tasks call_rcu
#define synchronize_rcu_tasks synchronize_rcu
static inline void exit_tasks_rcu_start(void) { }
static inline void exit_tasks_rcu_finish(void) { }
#endif /* #else #ifdef CONFIG_TASKS_RCU_GENERIC */

/**
 * cond_resched_tasks_rcu_qs - Report potential quiescent states to RCU
 *
 * This macro resembles cond_resched(), except that it is defined to
 * report potential quiescent states to RCU-tasks even if the cond_resched()
 * machinery were to be shut off, as some advocate for PREEMPTION kernels.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   cond_resched_tasks_rcu_qs - 잠재적 정지 상태를 RCU에 보고합니다.
 *
 *   이 매크로는 cond_resched()와 비슷하지만 일부 PREEMPTION 커널을 옹호하는
 *   것처럼 cond_resched() 기계가 꺼지더라도 잠재적 정지 상태를 RCU 작업에
 *   보고하도록 정의되어 있습니다.
 */
#define cond_resched_tasks_rcu_qs() \
do { \
	rcu_tasks_qs(current, false); \
	cond_resched(); \
} while (0)

/*
 * Infrastructure to implement the synchronize_() primitives in
 * TREE_RCU and rcu_barrier_() primitives in TINY_RCU.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   TREE_RCU에서 synchronize_() 프리미티브를 구현하고 TINY_RCU에서
 *   rcu_barrier_() 프리미티브를 구현하기 위한 인프라.
 */
#if defined(CONFIG_TREE_RCU)
#include <linux/rcutree.h>
#elif defined(CONFIG_TINY_RCU)
#include <linux/rcutiny.h>
#else
#error "Unknown RCU implementation specified to kernel configuration"
#endif

/*
 * The init_rcu_head_on_stack() and destroy_rcu_head_on_stack() calls
 * are needed for dynamic initialization and destruction of rcu_head
 * on the stack, and init_rcu_head()/destroy_rcu_head() are needed for
 * dynamic initialization and destruction of statically allocated rcu_head
 * structures.  However, rcu_head structures allocated dynamically in the
 * heap don't need any initialization.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   init_rcu_head_on_stack() 및 destroy_rcu_head_on_stack() 호출은
 *   스택에서 rcu_head의 동적 초기화 및 소멸을 위해 필요하며
 *   init_rcu_head()/destroy_rcu_head()는 정적으로 할당된 rcu_head 구조의
 *   동적 초기화 및 소멸을 위해 필요합니다. 그러나 힙에 동적으로 할당된
 *   rcu_head 구조는 초기화가 필요하지 않습니다.
 */
#ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD
void init_rcu_head(struct rcu_head *head);
void destroy_rcu_head(struct rcu_head *head);
void init_rcu_head_on_stack(struct rcu_head *head);
void destroy_rcu_head_on_stack(struct rcu_head *head);
#else /* !CONFIG_DEBUG_OBJECTS_RCU_HEAD */
static inline void init_rcu_head(struct rcu_head *head) { }
static inline void destroy_rcu_head(struct rcu_head *head) { }
static inline void init_rcu_head_on_stack(struct rcu_head *head) { }
static inline void destroy_rcu_head_on_stack(struct rcu_head *head) { }
#endif	/* #else !CONFIG_DEBUG_OBJECTS_RCU_HEAD */

#if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU)
bool rcu_lockdep_current_cpu_online(void);
#else /* #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU) */
static inline bool rcu_lockdep_current_cpu_online(void) { return true; }
#endif /* #else #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU) */

extern struct lockdep_map rcu_lock_map;
extern struct lockdep_map rcu_bh_lock_map;
extern struct lockdep_map rcu_sched_lock_map;
extern struct lockdep_map rcu_callback_map;

#ifdef CONFIG_DEBUG_LOCK_ALLOC

static inline void rcu_lock_acquire(struct lockdep_map *map)
{
	lock_acquire(map, 0, 0, 2, 0, NULL, _THIS_IP_);
}

static inline void rcu_lock_release(struct lockdep_map *map)
{
	lock_release(map, _THIS_IP_);
}

int debug_lockdep_rcu_enabled(void);
int rcu_read_lock_held(void);
int rcu_read_lock_bh_held(void);
int rcu_read_lock_sched_held(void);
int rcu_read_lock_any_held(void);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

# define rcu_lock_acquire(a)		do { } while (0)
# define rcu_lock_release(a)		do { } while (0)

static inline int rcu_read_lock_held(void)
{
	return 1;
}

static inline int rcu_read_lock_bh_held(void)
{
	return 1;
}

static inline int rcu_read_lock_sched_held(void)
{
	return !preemptible();
}

static inline int rcu_read_lock_any_held(void)
{
	return !preemptible();
}

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

#ifdef CONFIG_PROVE_RCU

/**
 * RCU_LOCKDEP_WARN - emit lockdep splat if specified condition is met
 * @c: condition to check
 * @s: informative message
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU_LOCKDEP_WARN - 지정된 조건이 충족되면 lockdep splat을 내보냅니다.
 *   @c: 확인할 조건.
 *   @s: 유익한 메시지. 
 */
#define RCU_LOCKDEP_WARN(c, s)						\
	do {								\
		static bool __section(".data.unlikely") __warned;	\
		if ((c) && debug_lockdep_rcu_enabled() && !__warned) {	\
			__warned = true;				\
			lockdep_rcu_suspicious(__FILE__, __LINE__, s);	\
		}							\
	} while (0)

#if defined(CONFIG_PROVE_RCU) && !defined(CONFIG_PREEMPT_RCU)
static inline void rcu_preempt_sleep_check(void)
{
	RCU_LOCKDEP_WARN(lock_is_held(&rcu_lock_map),
			 "Illegal context switch in RCU read-side critical section");
}
#else /* #ifdef CONFIG_PROVE_RCU */
static inline void rcu_preempt_sleep_check(void) { }
#endif /* #else #ifdef CONFIG_PROVE_RCU */

#define rcu_sleep_check()						\
	do {								\
		rcu_preempt_sleep_check();				\
		if (!IS_ENABLED(CONFIG_PREEMPT_RT))			\
		    RCU_LOCKDEP_WARN(lock_is_held(&rcu_bh_lock_map),	\
				 "Illegal context switch in RCU-bh read-side critical section"); \
		RCU_LOCKDEP_WARN(lock_is_held(&rcu_sched_lock_map),	\
				 "Illegal context switch in RCU-sched read-side critical section"); \
	} while (0)

#else /* #ifdef CONFIG_PROVE_RCU */

#define RCU_LOCKDEP_WARN(c, s) do { } while (0 && (c))
#define rcu_sleep_check() do { } while (0)

#endif /* #else #ifdef CONFIG_PROVE_RCU */

/*
 * Helper functions for rcu_dereference_check(), rcu_dereference_protected()
 * and rcu_assign_pointer().  Some of these could be folded into their
 * callers, but they are left separate in order to ease introduction of
 * multiple pointers markings to match different RCU implementations
 * (e.g., __srcu), should this make sense in the future.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference_check(), rcu_dereference_protected() 및 rcu_assign_pointer()에
 *   대한 헬퍼 함수. 이들 중 일부는 호출자로 접힐 수 있지만, 다른 RCU
 *   구현(예: __srcu)과 일치하도록 여러 포인터 표시를 쉽게 도입하기 위해 분리되어
 *   있습니다.
 */

#ifdef __CHECKER__
#define rcu_check_sparse(p, space) \
	((void)(((typeof(*p) space *)p) == p))
#else /* #ifdef __CHECKER__ */
#define rcu_check_sparse(p, space)
#endif /* #else #ifdef __CHECKER__ */

/**
 * unrcu_pointer - mark a pointer as not being RCU protected
 * @p: pointer needing to lose its __rcu property
 *
 * Converts @p from an __rcu pointer to a __kernel pointer.
 * This allows an __rcu pointer to be used with xchg() and friends.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   unrcu_pointer - RCU가 보호되지 않는 포인터를 표시합니다.
 *   @p: __rcu 속성을 잃어야 하는 포인터.
 *
 *   @p를 __rcu 포인터에서 __kernel 포인터로 변환합니다.
 *   이를 통해 __rcu 포인터를 xchg() 및 친구들과 함께 사용할 수 있습니다.
 */
#define unrcu_pointer(p)						\
({									\
	typeof(*p) *_________p1 = (typeof(*p) *__force)(p);		\
	rcu_check_sparse(p, __rcu);					\
	((typeof(*p) __force __kernel *)(_________p1)); 		\
})

#define __rcu_access_pointer(p, space) \
({ \
	typeof(*p) *_________p1 = (typeof(*p) *__force)READ_ONCE(p); \
	rcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(_________p1)); \
})
#define __rcu_dereference_check(p, c, space) \
({ \
	/* Dependency order vs. p above. */ \
	typeof(*p) *________p1 = (typeof(*p) *__force)READ_ONCE(p); \
	RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_check() usage"); \
	rcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(________p1)); \
})
#define __rcu_dereference_protected(p, c, space) \
({ \
	RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_protected() usage"); \
	rcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(p)); \
})
#define rcu_dereference_raw(p) \
({ \
	/* Dependency order vs. p above. */ \
	typeof(p) ________p1 = READ_ONCE(p); \
	((typeof(*p) __force __kernel *)(________p1)); \
})

/**
 * RCU_INITIALIZER() - statically initialize an RCU-protected global variable
 * @v: The value to statically initialize with.
 */
#define RCU_INITIALIZER(v) (typeof(*(v)) __force __rcu *)(v)

/**
 * rcu_assign_pointer() - assign to RCU-protected pointer
 * @p: pointer to assign to
 * @v: value to assign (publish)
 *
 * Assigns the specified value to the specified RCU-protected
 * pointer, ensuring that any concurrent RCU readers will see
 * any prior initialization.
 *
 * Inserts memory barriers on architectures that require them
 * (which is most of them), and also prevents the compiler from
 * reordering the code that initializes the structure after the pointer
 * assignment.  More importantly, this call documents which pointers
 * will be dereferenced by RCU read-side code.
 *
 * In some special cases, you may use RCU_INIT_POINTER() instead
 * of rcu_assign_pointer().  RCU_INIT_POINTER() is a bit faster due
 * to the fact that it does not constrain either the CPU or the compiler.
 * That said, using RCU_INIT_POINTER() when you should have used
 * rcu_assign_pointer() is a very bad thing that results in
 * impossible-to-diagnose memory corruption.  So please be careful.
 * See the RCU_INIT_POINTER() comment header for details.
 *
 * Note that rcu_assign_pointer() evaluates each of its arguments only
 * once, appearances notwithstanding.  One of the "extra" evaluations
 * is in typeof() and the other visible only to sparse (__CHECKER__),
 * neither of which actually execute the argument.  As with most cpp
 * macros, this execute-arguments-only-once property is important, so
 * please be careful when making changes to rcu_assign_pointer() and the
 * other macros that it invokes.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_assign_pointer() - RCU 보호 포인터에 할당합니다.
 *   @p: 할당할 포인터.
 *   @v: 할당할 값(게시)
 *
 *   지정된 RCU 보호 포인터에 지정된 값을 할당하여 모든 동시 RCU 판독기가
 *   이전 초기화를 볼 수 있도록 합니다.
 *
 *   이를 필요로 하는 아키텍처(대부분)에 메모리 장벽을 삽입하고 컴파일러가
 *   포인터 할당 후 구조를 초기화하는 코드를 재정렬하지 못하도록 합니다.
 *   더 중요한 것은 이 호출이 RCU 읽기 측 코드에 의해 역참조될 포인터를
 *   문서화한다는 것입니다.
 *
 *   특별한 경우에는 rcu_assign_pointer() 대신 RCU_INIT_POINTER()를
 *   사용할 수 있습니다. RCU_INIT_POINTER()는 CPU나 컴파일러를 제한하지
 *   않기 때문에 조금 더 빠릅니다.
 *   즉, rcu_assign_pointer()를 사용해야 할 때 RCU_INIT_POINTER()를
 *   사용하는 것은 진단할 수 없는 메모리 손상을 초래하는 매우
 *   나쁜 일입니다. 그러니 조심하세요.
 *   자세한 내용은 RCU_INIT_POINTER() 주석 헤더를 참조하십시오.
 *   rcu_assign_pointer()는 모양에 관계없이 각 인수를 한 번만 평가합니다.
 *   추가 평가 중 하나는 typeof()에 있고 다른 하나는 sparse(__CHECKER__)에만
 *   표시되며 둘 다 실제로 인수를 실행하지 않습니다. 대부분의 cpp 매크로와
 *   마찬가지로 이 execute-arguments-only-once 속성이 중요하므로
 *   rcu_assign_pointer() 및 호출하는 다른 매크로를 변경할 때 주의하십시오.
 */
#define rcu_assign_pointer(p, v)					      \
do {									      \
	uintptr_t _r_a_p__v = (uintptr_t)(v);				      \
	rcu_check_sparse(p, __rcu);					      \
									      \
	if (__builtin_constant_p(v) && (_r_a_p__v) == (uintptr_t)NULL)	      \
		WRITE_ONCE((p), (typeof(p))(_r_a_p__v));		      \
	else								      \
		smp_store_release(&p, RCU_INITIALIZER((typeof(p))_r_a_p__v)); \
} while (0)

/**
 * rcu_replace_pointer() - replace an RCU pointer, returning its old value
 * @rcu_ptr: RCU pointer, whose old value is returned
 * @ptr: regular pointer
 * @c: the lockdep conditions under which the dereference will take place
 *
 * Perform a replacement, where @rcu_ptr is an RCU-annotated
 * pointer and @c is the lockdep argument that is passed to the
 * rcu_dereference_protected() call used to read that pointer.  The old
 * value of @rcu_ptr is returned, and @rcu_ptr is set to @ptr.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_replace_pointer() - RCU 포인터를 교체하여 이전 값을 반환합니다.
 *   @rcu_ptr: 이전 값이 반환되는 RCU 포인터.
 *   @ptr: 일반 포인터.
 *   @c: 역참조가 발생할 lockdep 조건입니다.
 *
 *   교체를 수행합니다. 여기서 @rcu_ptr은 RCU 주석 포인터이고 @c는 해당
 *   포인터를 읽는 데 사용되는 rcu_dereference_protected() 호출에 전달되는
 *   lockdep 인수입니다. @rcu_ptr의 이전 값이 반환되고 @rcu_ptr이 @ptr로
 *   설정됩니다.
 */
#define rcu_replace_pointer(rcu_ptr, ptr, c)				\
({									\
	typeof(ptr) __tmp = rcu_dereference_protected((rcu_ptr), (c));	\
	rcu_assign_pointer((rcu_ptr), (ptr));				\
	__tmp;								\
})

/**
 * rcu_access_pointer() - fetch RCU pointer with no dereferencing
 * @p: The pointer to read
 *
 * Return the value of the specified RCU-protected pointer, but omit the
 * lockdep checks for being in an RCU read-side critical section.  This is
 * useful when the value of this pointer is accessed, but the pointer is
 * not dereferenced, for example, when testing an RCU-protected pointer
 * against NULL.  Although rcu_access_pointer() may also be used in cases
 * where update-side locks prevent the value of the pointer from changing,
 * you should instead use rcu_dereference_protected() for this use case.
 *
 * It is also permissible to use rcu_access_pointer() when read-side
 * access to the pointer was removed at least one grace period ago, as
 * is the case in the context of the RCU callback that is freeing up
 * the data, or after a synchronize_rcu() returns.  This can be useful
 * when tearing down multi-linked structures after a grace period
 * has elapsed.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_access_pointer() - 참조 해제 없이 RCU 포인터를 가져옵니다.
 *   @p: 읽을 포인터입니다. 
 *
 *   지정된 RCU로 보호된 포인터의 값을 반환하지만, RCU 읽기 측 중요 섹션에
 *   있는지에 대한 lockdep 검사는 생략합니다. 이 기능은 이 포인터의 값에
 *   액세스할 때 유용하지만, 예를 들어 NULL에 대해 RCU로 보호된 포인터를
 *   테스트할 때 포인터가 참조되지 않습니다. 업데이트 측 잠금으로 인해
 *   포인터 값이 변경되지 않는 경우에도 rcu_access_pointer()를 사용할 수
 *   있지만 이 사용 사례에는 rcu_dereference_protected()를 사용해야 합니다.
 *
 *   또한 데이터를 개방하는 RCU 콜백의 맥락에서와 같이, 또는
 *   synchronize_rcu()가 반환된 후 적어도 하나의 유예 기간 전에 포인터에 대한
 *   읽기 측 액세스가 제거되었을 때 urcu_access_pointer()를 사용할 수
 *   있습니다. 유예 기간이 경과한 후 다중 연결 구조를 해체할 때 유용합니다.
 */
#define rcu_access_pointer(p) __rcu_access_pointer((p), __rcu)

/**
 * rcu_dereference_check() - rcu_dereference with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Do an rcu_dereference(), but check that the conditions under which the
 * dereference will take place are correct.  Typically the conditions
 * indicate the various locking conditions that should be held at that
 * point.  The check should return true if the conditions are satisfied.
 * An implicit check for being in an RCU read-side critical section
 * (rcu_read_lock()) is included.
 *
 * For example:
 *
 *	bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock));
 *
 * could be used to indicate to lockdep that foo->bar may only be dereferenced
 * if either rcu_read_lock() is held, or that the lock required to replace
 * the bar struct at foo->bar is held.
 *
 * Note that the list of conditions may also include indications of when a lock
 * need not be held, for example during initialisation or destruction of the
 * target struct:
 *
 *	bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock) ||
 *					      atomic_read(&foo->usage) == 0);
 *
 * Inserts memory barriers on architectures that require them
 * (currently only the Alpha), prevents the compiler from refetching
 * (and from merging fetches), and, more importantly, documents exactly
 * which pointers are protected by RCU and checks that the pointer is
 * annotated as __rcu.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference_check() - 디버그 검사가 포함된 rcu_dereference.
 *   @p: 역참조하기 전에 읽을 포인터입니다.
 *   @c: 역참조가 발생하는 조건입니다.
 *
 *   rcu_dereference()를 수행하되 역참조가 발생하는 조건이 올바른지
 *   확인하십시오. 일반적으로 조건은 해당 지점에서 유지되어야 하는 다양한
 *   잠금 조건을 나타냅니다. 조건이 충족되면 검사에서 true를 반환해야 합니다.
 *   RCU 읽기 측 임계 섹션(rcu_read_lock())에 있는지에 대한 암시적 검사가
 *   포함됩니다.
 *
 *   예를 들어:
 *
 *   bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock));
 *
 *   rcu_read_lock()이 유지되거나 foo->bar에서 bar 구조체를 교체하는 데 
 *   필요한 잠금이 유지되는 경우에만 foo->bar가 역참조될 수 있음을 lockdep에
 *   나타내는 데 사용할 수 있습니다.
 *
 *   조건 목록에는 잠금을 유지할 필요가 없는 경우(예: 대상 구조체의 초기화
 *   또는 소멸)에 대한 표시도 포함될 수 있습니다.
 *
 *   bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock) ||
 *   atomic_read(&foo->usage) == 0);
 *
 *   이를 필요로 하는 아키텍처(현재 Alpha만 해당)에 메모리 장벽을 삽입하고,
 *   컴파일러가 다시 페치(및 페치 병합)하는 것을 방지하고, 더 중요한 것은
 *   어떤 포인터가 RCU에 의해 보호되는지 정확하게 문서화하고 포인터가 __rcu로
 *   주석이 추가되었는지 확인합니다.
 */
#define rcu_dereference_check(p, c) \
	__rcu_dereference_check((p), (c) || rcu_read_lock_held(), __rcu)

/**
 * rcu_dereference_bh_check() - rcu_dereference_bh with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the RCU-bh counterpart to rcu_dereference_check().  However,
 * please note that starting in v5.0 kernels, vanilla RCU grace periods
 * wait for local_bh_disable() regions of code in addition to regions of
 * code demarked by rcu_read_lock() and rcu_read_unlock().  This means
 * that synchronize_rcu(), call_rcu, and friends all take not only
 * rcu_read_lock() but also rcu_read_lock_bh() into account.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference_bh_check() - rcu_dereference_bh 디버그 검사 포함.
 *
 *   @p: 역참조하기 전에 읽을 포인터입니다.
 *   @c: 역참조가 발생하는 조건입니다.
 *
 *   이것은 rcu_dereference_check()에 대응하는 RCU-bh입니다. 그러나 v5.0
 *   커널부터 바닐라 RCU 유예 기간은 rcu_read_lock() 및 rcu_read_unlock()에
 *   의해 지정된 코드 영역 외에도 local_bh_disable() 코드 영역을 기다립니다.
 *   즉, synchronize_rcu(), call_rcu 및 친구들은 모두 rcu_read_lock()뿐만
 *   아니라 rcu_read_lock_bh()도 고려합니다.
 */
#define rcu_dereference_bh_check(p, c) \
	__rcu_dereference_check((p), (c) || rcu_read_lock_bh_held(), __rcu)

/**
 * rcu_dereference_sched_check() - rcu_dereference_sched with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the RCU-sched counterpart to rcu_dereference_check().
 * However, please note that starting in v5.0 kernels, vanilla RCU grace
 * periods wait for preempt_disable() regions of code in addition to
 * regions of code demarked by rcu_read_lock() and rcu_read_unlock().
 * This means that synchronize_rcu(), call_rcu, and friends all take not
 * only rcu_read_lock() but also rcu_read_lock_sched() into account.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference_sched_check() - 디버그 검사가 포함된 rcu_dereference_sched.
 *
 *   @p: 역참조하기 전에 읽을 포인터입니다.
 *   @c: 역참조가 발생하는 조건입니다.
 *
 *   이것은 rcu_dereference_check()에 대응하는 RCU 스케줄입니다.
 *   그러나 v5.0 커널부터 바닐라 RCU 유예 기간은 rcu_read_lock() 및
 *   rcu_read_unlock()에 의해 지정된 코드 영역 외에도 코드의
 *   preempt_disable() 영역을 기다립니다.
 *   즉, synchronize_rcu(), call_rcu 및 친구들은 모두 rcu_read_lock()뿐만
 *   아니라 rcu_read_lock_sched()도 고려합니다.
 */
#define rcu_dereference_sched_check(p, c) \
	__rcu_dereference_check((p), (c) || rcu_read_lock_sched_held(), \
				__rcu)

/*
 * The tracing infrastructure traces RCU (we want that), but unfortunately
 * some of the RCU checks causes tracing to lock up the system.
 *
 * The no-tracing version of rcu_dereference_raw() must not call
 * rcu_read_lock_held().
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   추적 인프라는 RCU를 추적하지만(원하는 대로) 불행히도 일부 RCU 검사로
 *   인해 추적으로 인해 시스템이 잠깁니다.
 *
 *   rcu_dereference_raw()의 비추적 버전은 rcu_read_lock_held()를 호출하지
 *   않아야 합니다.
 */
#define rcu_dereference_raw_check(p) __rcu_dereference_check((p), 1, __rcu)

/**
 * rcu_dereference_protected() - fetch RCU pointer when updates prevented
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Return the value of the specified RCU-protected pointer, but omit
 * the READ_ONCE().  This is useful in cases where update-side locks
 * prevent the value of the pointer from changing.  Please note that this
 * primitive does *not* prevent the compiler from repeating this reference
 * or combining it with other references, so it should not be used without
 * protection of appropriate locks.
 *
 * This function is only for update-side use.  Using this function
 * when protected only by rcu_read_lock() will result in infrequent
 * but very ugly failures.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference_protected() - 업데이트가 금지된 경우 RCU 포인터를 가져옵니다.
 *   @p: 역참조하기 전에 읽을 포인터입니다.
 *   @c: 역참조가 발생하는 조건입니다.
 *
 *   지정된 RCU 보호 포인터의 값을 반환하지만 READ_ONCE()는 생략합니다.
 *   이는 업데이트측 잠금이 포인터 값이 변경되지 않도록 하는 경우에
 *   유용합니다. 이 프리미티브는 컴파일러가 이 참조를 반복하거나 다른 참조와
 *   결합하는 것을 막지 *않으므로* 적절한 잠금 보호 없이 사용해서는 안 됩니다.
 *
 *   이 기능은 업데이트 측에서만 사용됩니다. rcu_read_lock()에 의해서만
 *   보호될 때 이 함수를 사용하면 드물지만 매우 보기 흉한 실패가 발생합니다.
 */
#define rcu_dereference_protected(p, c) \
	__rcu_dereference_protected((p), (c), __rcu)


/**
 * rcu_dereference() - fetch RCU-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * This is a simple wrapper around rcu_dereference_check().
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference() - 역참조를 위해 RCU 보호 포인터를 가져옵니다.
 *   @p: 역참조하기 전에 읽을 포인터
 *
 *   이것은 rcu_dereference_check() 주변의 간단한 래퍼입니다. 
 */
#define rcu_dereference(p) rcu_dereference_check(p, 0)

/**
 * rcu_dereference_bh() - fetch an RCU-bh-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference_bh() - 역참조를 위해 RCU-bh 보호 포인터를 가져옵니다.
 *   @p: 역참조하기 전에 읽을 포인터입니다.
 *
 *   rcu_dereference_check()가 지저분한 작업을 수행하도록 합니다. 
 */
#define rcu_dereference_bh(p) rcu_dereference_bh_check(p, 0)

/**
 * rcu_dereference_sched() - fetch RCU-sched-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_dereference_sched() - 역참조를 위해 RCU-sched-protected 포인터를
 *   가져옵니다.
 *   @p: 역참조하기 전에 읽을 포인터입니다.
 *
 *   rcu_dereference_check()가 지저분한 작업을 수행하도록 합니다.
 */
#define rcu_dereference_sched(p) rcu_dereference_sched_check(p, 0)

/**
 * rcu_pointer_handoff() - Hand off a pointer from RCU to other mechanism
 * @p: The pointer to hand off
 *
 * This is simply an identity function, but it documents where a pointer
 * is handed off from RCU to some other synchronization mechanism, for
 * example, reference counting or locking.  In C11, it would map to
 * kill_dependency().  It could be used as follows::
 *
 *	rcu_read_lock();
 *	p = rcu_dereference(gp);
 *	long_lived = is_long_lived(p);
 *	if (long_lived) {
 *		if (!atomic_inc_not_zero(p->refcnt))
 *			long_lived = false;
 *		else
 *			p = rcu_pointer_handoff(p);
 *	}
 *	rcu_read_unlock();
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_pointer_handoff() - RCU에서 다른 메커니즘으로 포인터를 전달합니다.
 *   @p: 핸드오프할 포인터.
 *
 *   이것은 단순히 ID 기능이지만 포인터가 RCU에서 다른 동기화
 *   메커니즘(예: 참조 카운팅 또는 잠금)으로 전달되는 위치를 문서화합니다.
 *   C11에서는 kill_dependency()에 매핑됩니다. 다음과 같이 사용할 수 있습니다.
 */
#define rcu_pointer_handoff(p) (p)

/**
 * rcu_read_lock() - mark the beginning of an RCU read-side critical section
 *
 * When synchronize_rcu() is invoked on one CPU while other CPUs
 * are within RCU read-side critical sections, then the
 * synchronize_rcu() is guaranteed to block until after all the other
 * CPUs exit their critical sections.  Similarly, if call_rcu() is invoked
 * on one CPU while other CPUs are within RCU read-side critical
 * sections, invocation of the corresponding RCU callback is deferred
 * until after the all the other CPUs exit their critical sections.
 *
 * In v5.0 and later kernels, synchronize_rcu() and call_rcu() also
 * wait for regions of code with preemption disabled, including regions of
 * code with interrupts or softirqs disabled.  In pre-v5.0 kernels, which
 * define synchronize_sched(), only code enclosed within rcu_read_lock()
 * and rcu_read_unlock() are guaranteed to be waited for.
 *
 * Note, however, that RCU callbacks are permitted to run concurrently
 * with new RCU read-side critical sections.  One way that this can happen
 * is via the following sequence of events: (1) CPU 0 enters an RCU
 * read-side critical section, (2) CPU 1 invokes call_rcu() to register
 * an RCU callback, (3) CPU 0 exits the RCU read-side critical section,
 * (4) CPU 2 enters a RCU read-side critical section, (5) the RCU
 * callback is invoked.  This is legal, because the RCU read-side critical
 * section that was running concurrently with the call_rcu() (and which
 * therefore might be referencing something that the corresponding RCU
 * callback would free up) has completed before the corresponding
 * RCU callback is invoked.
 *
 * RCU read-side critical sections may be nested.  Any deferred actions
 * will be deferred until the outermost RCU read-side critical section
 * completes.
 *
 * You can avoid reading and understanding the next paragraph by
 * following this rule: don't put anything in an rcu_read_lock() RCU
 * read-side critical section that would block in a !PREEMPTION kernel.
 * But if you want the full story, read on!
 *
 * In non-preemptible RCU implementations (pure TREE_RCU and TINY_RCU),
 * it is illegal to block while in an RCU read-side critical section.
 * In preemptible RCU implementations (PREEMPT_RCU) in CONFIG_PREEMPTION
 * kernel builds, RCU read-side critical sections may be preempted,
 * but explicit blocking is illegal.  Finally, in preemptible RCU
 * implementations in real-time (with -rt patchset) kernel builds, RCU
 * read-side critical sections may be preempted and they may also block, but
 * only when acquiring spinlocks that are subject to priority inheritance.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_lock() - RCU 읽기측 중요 섹션의 시작을 표시합니다.
 *
 *   다른 CPU가 RCU 읽기 측 임계 섹션 내에 있는 동안 다른 CPU에서
 *   synchronize_rcu()가 호출되면 다른 모든 CPU가 임계 섹션을 종료할 때까지
 *   synchronize_rcu()가 차단됩니다. 마찬가지로 call_rcu()가 한 CPU에서
 *   호출되고 다른 CPU가 RCU 읽기 측 임계 섹션 내에 있는 경우 해당 RCU 콜백
 *   호출은 다른 모든 CPU가 임계 섹션을 종료할 때까지 연기됩니다.
 *
 *   v5.0 이상 커널에서 synchronize_rcu() 및 call_rcu()는 또한 인터럽트 또는
 *   softirq가 비활성화된 코드 영역을 포함하여 선점이 비활성화된 코드 영역을
 *   기다립니다. synchronize_sched()를 정의하는 v5.0 이전 커널에서는
 *   rcu_read_lock() 및 rcu_read_unlock() 내에 포함된 코드만 대기하도록
 *   보장됩니다.
 *
 *   그러나 RCU 콜백은 새로운 RCU 읽기측 중요 섹션과 동시에 실행될 수
 *   있습니다. 이것이 일어날 수 있는 한 가지 방법은 다음과 같은 일련의
 *   이벤트를 통해서입니다.
 *   (1) CPU 0이 RCU 읽기 측 임계 구역에 진입,
 *   (2) CPU 1이 call_rcu()를 호출하여 RCU 콜백을 등록,
 *   (3) CPU 0이 RCU 읽기 측 임계 구역을 종료,
 *   (4) CPU 2가 RCU 읽기 측 중요 섹션,
 *   (5) RCU 콜백이 호출됩니다.
 *   이는 call_rcu()와 동시에 실행되고 있는(따라서 해당 RCU 콜백이 해제할
 *   무언가를 참조할 수 있는) RCU 읽기 측 임계 섹션이 해당 RCU 콜백이
 *   호출되기 전에 완료되었기 때문에 합법적입니다.
 *
 *   RCU 읽기 측 임계 섹션은 중첩될 수 있습니다. 연기된 모든 작업은 가장
 *   바깥쪽 RCU 읽기측 임계 섹션이 완료될 때까지 연기됩니다.
 *
 *   다음 규칙을 따르면 다음 단락을 읽고 이해하는 것을 피할 수 있습니다.
 *   !PREEMPTION 커널에서 차단되는 rcu_read_lock() RCU 읽기 측 중요 섹션에
 *   아무 것도 넣지 마십시오.
 *   하지만 전체 내용을 알고 싶다면 계속 읽어보세요! 비선점형 RCU 구현(순수
 *   TREE_RCU 및 TINY_RCU)에서 RCU 읽기 측 중요 섹션에 있는 동안 차단하는
 *   것은 불법입니다.
 *   CONFIG_PREEMPTION 커널 빌드의 선점형 RCU 구현(PREEMPT_RCU)에서 RCU 읽기
 *   측 중요 섹션이 선점될 수 있지만 명시적 차단은 불법입니다. 마지막으로
 *   실시간(-rt 패치 세트 사용) 커널 빌드의 선점형 RCU 구현에서 RCU 읽기 측
 *   임계 섹션이 선점될 수 있으며 차단될 수도 있지만 우선 순위 상속이
 *   적용되는 스핀록을 획득할 때만 가능합니다.
 */
static __always_inline void rcu_read_lock(void)
{
	__rcu_read_lock();
	__acquire(RCU);
	rcu_lock_acquire(&rcu_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock() used illegally while idle");
}

/*
 * So where is rcu_write_lock()?  It does not exist, as there is no
 * way for writers to lock out RCU readers.  This is a feature, not
 * a bug -- this property is what provides RCU's performance benefits.
 * Of course, writers must coordinate with each other.  The normal
 * spinlock primitives work well for this, but any other technique may be
 * used as well.  RCU does not care how the writers keep out of each
 * others' way, as long as they do so.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_write_lock()은 어디에 있습니까? 작성기가 RCU 판독기를 잠글 수 있는
 *   방법이 없기 때문에 존재하지 않습니다. 이것은 버그가 아니라 기능입니다.
 *   이 속성은 RCU의 성능 이점을 제공합니다.
 *   물론 작가들은 서로 조율해야 한다. 일반 스핀록 프리미티브는 이를 위해 잘
 *   작동하지만 다른 기술도 사용할 수 있습니다. RCU는 작가들이 그렇게 하는
 *   한 어떻게 서로의 길을 막는지 신경 쓰지 않습니다.
 */

/**
 * rcu_read_unlock() - marks the end of an RCU read-side critical section.
 *
 * In almost all situations, rcu_read_unlock() is immune from deadlock.
 * In recent kernels that have consolidated synchronize_sched() and
 * synchronize_rcu_bh() into synchronize_rcu(), this deadlock immunity
 * also extends to the scheduler's runqueue and priority-inheritance
 * spinlocks, courtesy of the quiescent-state deferral that is carried
 * out when rcu_read_unlock() is invoked with interrupts disabled.
 *
 * See rcu_read_lock() for more information.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_unlock() - RCU 읽기 측 중요 섹션의 끝을 표시합니다.
 *
 *   거의 모든 상황에서 rcu_read_unlock()은 교착 상태에 영향을 받지 않습니다.
 *   synchronize_sched() 및 synchronize_rcu_bh()를 synchronize_rcu()로 통합한
 *   최근 커널에서 이 교착 상태 면역성은 rcu_read_unlock()이 호출될 때
 *   수행되는 대기 상태 연기 덕분에 스케줄러의 실행 대기열 및 우선순위
 *   상속 스핀록으로 확장됩니다. 인터럽트가 비활성화된 상태에서.
 *
 *   자세한 내용은 rcu_read_lock()을 참조하십시오.
 */
static inline void rcu_read_unlock(void)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock() used illegally while idle");
	__release(RCU);
	__rcu_read_unlock();
	rcu_lock_release(&rcu_lock_map); /* Keep acq info for rls diags. */
}

/**
 * rcu_read_lock_bh() - mark the beginning of an RCU-bh critical section
 *
 * This is equivalent to rcu_read_lock(), but also disables softirqs.
 * Note that anything else that disables softirqs can also serve as an RCU
 * read-side critical section.  However, please note that this equivalence
 * applies only to v5.0 and later.  Before v5.0, rcu_read_lock() and
 * rcu_read_lock_bh() were unrelated.
 *
 * Note that rcu_read_lock_bh() and the matching rcu_read_unlock_bh()
 * must occur in the same context, for example, it is illegal to invoke
 * rcu_read_unlock_bh() from one task if the matching rcu_read_lock_bh()
 * was invoked from some other task.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_lock_bh() - RCU-bh 임계 섹션의 시작을 표시합니다. 이것은
 *   rcu_read_lock()과 동일하지만 softirqs도 비활성화합니다.
 *   softirqs를 비활성화하는 다른 모든 항목은 RCU 읽기 측 중요 섹션으로도
 *   사용할 수 있습니다. 그러나 이 동등성은 v5.0 이상에만 적용된다는 점에
 *   유의하십시오. v5.0 이전에는 rcu_read_lock()과 rcu_read_lock_bh()가
 *   관련이 없었습니다.
 *
 *   rcu_read_lock_bh() 및 일치하는 rcu_read_unlock_bh()는 동일한
 *   컨텍스트에서 발생해야 합니다. 예를 들어 일치하는 rcu_read_lock_bh()가
 *   다른 작업에서 호출된 경우 한 작업에서 rcu_read_unlock_bh()를 호출하는
 *   것은 불법입니다.
 */
static inline void rcu_read_lock_bh(void)
{
	local_bh_disable();
	__acquire(RCU_BH);
	rcu_lock_acquire(&rcu_bh_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock_bh() used illegally while idle");
}

/**
 * rcu_read_unlock_bh() - marks the end of a softirq-only RCU critical section
 *
 * See rcu_read_lock_bh() for more information.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_unlock_bh() - softirq 전용 RCU 크리티컬 섹션의 끝을 표시합니다.
 *   자세한 내용은 rcu_read_lock_bh()를 참조하십시오.
 */
static inline void rcu_read_unlock_bh(void)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock_bh() used illegally while idle");
	rcu_lock_release(&rcu_bh_lock_map);
	__release(RCU_BH);
	local_bh_enable();
}

/**
 * rcu_read_lock_sched() - mark the beginning of a RCU-sched critical section
 *
 * This is equivalent to rcu_read_lock(), but also disables preemption.
 * Read-side critical sections can also be introduced by anything else that
 * disables preemption, including local_irq_disable() and friends.  However,
 * please note that the equivalence to rcu_read_lock() applies only to
 * v5.0 and later.  Before v5.0, rcu_read_lock() and rcu_read_lock_sched()
 * were unrelated.
 *
 * Note that rcu_read_lock_sched() and the matching rcu_read_unlock_sched()
 * must occur in the same context, for example, it is illegal to invoke
 * rcu_read_unlock_sched() from process context if the matching
 * rcu_read_lock_sched() was invoked from an NMI handler.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_lock_sched() - RCU 스케줄 임계 섹션의 시작을 표시합니다.
 *   이는 rcu_read_lock()과 동일하지만 선점도 비활성화합니다.
 *   읽기 측 임계 섹션은 local_irq_disable() 및 친구를 포함하여 선점을
 *   비활성화하는 다른 항목에 의해 도입될 수도 있습니다. 그러나
 *   rcu_read_lock()과 동등한 것은 v5.0 이상에만 적용된다는 점에
 *   유의하십시오. v5.0 이전에는 rcu_read_lock()과 rcu_read_lock_sched()가
 *   관련이 없었습니다.
 *
 *   rcu_read_lock_sched() 및 일치하는 rcu_read_unlock_sched()는 동일한
 *   컨텍스트에서 발생해야 합니다. 예를 들어 일치하는 rcu_read_lock_sched()가
 *   NMI 처리기에서 호출된 경우 프로세스 컨텍스트에서
 *   rcu_read_unlock_sched()를 호출하는 것은 불법입니다.
 */
static inline void rcu_read_lock_sched(void)
{
	preempt_disable();
	__acquire(RCU_SCHED);
	rcu_lock_acquire(&rcu_sched_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock_sched() used illegally while idle");
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   lockdep 및 추적에서 사용: 추적할 수 없으며 lockdep를 호출할 수 없습니다.
 */
static inline notrace void rcu_read_lock_sched_notrace(void)
{
	preempt_disable_notrace();
	__acquire(RCU_SCHED);
}

/**
 * rcu_read_unlock_sched() - marks the end of a RCU-classic critical section
 *
 * See rcu_read_lock_sched() for more information.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_unlock_sched() - RCU 클래식 크리티컬 섹션의 끝을 표시합니다.
 *
 *   자세한 내용은 rcu_read_lock_sched()를 참조하십시오.
 */
static inline void rcu_read_unlock_sched(void)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock_sched() used illegally while idle");
	rcu_lock_release(&rcu_sched_lock_map);
	__release(RCU_SCHED);
	preempt_enable();
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   lockdep 및 추적에서 사용: 추적할 수 없으며 lockdep를 호출할 수 없습니다.
 */
static inline notrace void rcu_read_unlock_sched_notrace(void)
{
	__release(RCU_SCHED);
	preempt_enable_notrace();
}

/**
 * RCU_INIT_POINTER() - initialize an RCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * Initialize an RCU-protected pointer in special cases where readers
 * do not need ordering constraints on the CPU or the compiler.  These
 * special cases are:
 *
 * 1.	This use of RCU_INIT_POINTER() is NULLing out the pointer *or*
 * 2.	The caller has taken whatever steps are required to prevent
 *	RCU readers from concurrently accessing this pointer *or*
 * 3.	The referenced data structure has already been exposed to
 *	readers either at compile time or via rcu_assign_pointer() *and*
 *
 *	a.	You have not made *any* reader-visible changes to
 *		this structure since then *or*
 *	b.	It is OK for readers accessing this structure from its
 *		new location to see the old state of the structure.  (For
 *		example, the changes were to statistical counters or to
 *		other state where exact synchronization is not required.)
 *
 * Failure to follow these rules governing use of RCU_INIT_POINTER() will
 * result in impossible-to-diagnose memory corruption.  As in the structures
 * will look OK in crash dumps, but any concurrent RCU readers might
 * see pre-initialized values of the referenced data structure.  So
 * please be very careful how you use RCU_INIT_POINTER()!!!
 *
 * If you are creating an RCU-protected linked structure that is accessed
 * by a single external-to-structure RCU-protected pointer, then you may
 * use RCU_INIT_POINTER() to initialize the internal RCU-protected
 * pointers, but you must use rcu_assign_pointer() to initialize the
 * external-to-structure pointer *after* you have completely initialized
 * the reader-accessible portions of the linked structure.
 *
 * Note that unlike rcu_assign_pointer(), RCU_INIT_POINTER() provides no
 * ordering guarantees for either the CPU or the compiler.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU_INIT_POINT() - RCU 보호 포인터를 초기화합니다.
 *   @p: 초기화할 포인터입니다.
 *   @v: 포인터를 초기화할 값입니다.
 *
 *   판독기가 CPU 또는 컴파일러에 대한 제약 조건을 주문할 필요가 없는 특수한
 *   경우 RCU로 보호되는 포인터를 초기화합니다. 다음과 같은 특수한 경우가
 *   있습니다:
 *
 *   1.RCU_INIT_POINT()의 이 사용은 포인터를 NULL로 하는 것입니다 *or*
 *   2. 호출자는 RCU 판독기가 동시에 이 포인터에 액세스하지 못하도록 하기
 *   위해 필요한 모든 조치를 취했습니다 *or*
 *   3. 참조된 데이터 구조는 컴파일 시 또는 rcu_assign_pointer()를 통해
 *   이미 판독기에 노출되었습니다 *and*
 *
 *   a. 그 이후로 이 구조에 대해 독자가 볼 수 있는 어떠한(*any*)변경도 하지
 *   않았습니다 *or*
 *   b. 새 위치에서 이 구조체에 액세스하는 판독기는 구조체의 이전 상태를
 *   확인할 수 있습니다. (예를 들어, 변경 사항은 통계 카운터 또는 정확한
 *   동기화가 필요하지 않은 다른 상태로 변경되었습니다.).
 *
 *   RCU_INIT_POINT()의 사용을 관리하는 이러한 규칙을 따르지 않으면 메모리가
 *   손상되어 진단할 수 없습니다. 충돌 덤프의 경우 구조가 정상으로 보이지만
 *   동시 RCU 판독기에는 참조된 데이터 구조의 사전 초기화된 값이 표시될 수
 *   있습니다. 그러므로 RCU_INIT_POINT()를 사용하는 방법에 매우 주의하시기
 *   바랍니다!!!
 *
 *   단일 외부-구조 간 RCU 보호 포인터로 액세스되는 RCU 보호 연결 구조를
 *   만드는 경우, RCU_INIT_POINT()를 사용하여 내부 RCU 보호 포인터를 초기화할
 *   수 있습니다, 그러나 링크된 구조체의 판독기 액세스 부분을 완전히 초기화한
 *   후에(*after*) 외부 구조체 포인터를 초기화하려면 rcu_interval_interval을
 *   사용해야 합니다.
 *
 *   rcu_assign_pointer()와 달리 RCU_INIT_POINT()는 CPU나 컴파일러에 대한
 *   주문 보증을 제공하지 않습니다.
 */
#define RCU_INIT_POINTER(p, v) \
	do { \
		rcu_check_sparse(p, __rcu); \
		WRITE_ONCE(p, RCU_INITIALIZER(v)); \
	} while (0)

/**
 * RCU_POINTER_INITIALIZER() - statically initialize an RCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * GCC-style initialization for an RCU-protected pointer in a structure field.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU_POINTER_INITIAZER() - RCU 보호 포인터를 정적으로 초기화합니다.
 *   @p: 초기화할 포인터입니다.
 *   @v: 포인터를 초기화할 값입니다.
 *
 *   구조체 필드에서 RCU로 보호된 포인터에 대한 GCC 스타일 초기화.
 */
#define RCU_POINTER_INITIALIZER(p, v) \
		.p = RCU_INITIALIZER(v)

/*
 * Does the specified offset indicate that the corresponding rcu_head
 * structure can be handled by kvfree_rcu()?
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   지정된 오프셋이 해당 rcu_head 구조를 kvfree_rcu()로 처리할 수 있음을
 *   나타냅니까? 
 */
#define __is_kvfree_rcu_offset(offset) ((offset) < 4096)

/**
 * kfree_rcu() - kfree an object after a grace period.
 * @ptr: pointer to kfree for both single- and double-argument invocations.
 * @rhf: the name of the struct rcu_head within the type of @ptr,
 *       but only for double-argument invocations.
 *
 * Many rcu callbacks functions just call kfree() on the base structure.
 * These functions are trivial, but their size adds up, and furthermore
 * when they are used in a kernel module, that module must invoke the
 * high-latency rcu_barrier() function at module-unload time.
 *
 * The kfree_rcu() function handles this issue.  Rather than encoding a
 * function address in the embedded rcu_head structure, kfree_rcu() instead
 * encodes the offset of the rcu_head structure within the base structure.
 * Because the functions are not allowed in the low-order 4096 bytes of
 * kernel virtual memory, offsets up to 4095 bytes can be accommodated.
 * If the offset is larger than 4095 bytes, a compile-time error will
 * be generated in kvfree_rcu_arg_2(). If this error is triggered, you can
 * either fall back to use of call_rcu() or rearrange the structure to
 * position the rcu_head structure into the first 4096 bytes.
 *
 * Note that the allowable offset might decrease in the future, for example,
 * to allow something like kmem_cache_free_rcu().
 *
 * The BUILD_BUG_ON check must not involve any function calls, hence the
 * checks are done in macros here.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   kfree_rcu() - 유예 기간 후에 객체를 해제합니다.
 *   @ptr: 단일 및 이중 인수 호출 모두에 대한 kfree에 대한 포인터입니다.
 *   @rhf: @ptr 유형 내의 struct rcu_head 이름이지만 이중 인수 호출에만
 *   해당됩니다.
 *
 *   많은 rcu 콜백 함수는 기본 구조에서 kfree()를 호출합니다.
 *   이러한 함수는 사소하지만 그 크기가 합산되며 커널 모듈에서 사용될 때 해당
 *   모듈은 모듈 언로드 시간에 대기 시간이 긴 rcu_barrier() 함수를 호출해야
 *   합니다.
 *
 *   kfree_rcu() 함수는 이 문제를 처리합니다. 임베디드 rcu_head 구조에서
 *   함수 주소를 인코딩하는 대신 kfree_rcu()는 대신 기본 구조 내에서
 *   rcu_head 구조의 오프셋을 인코딩합니다.
 *   커널 가상 메모리의 하위 4096바이트에서는 함수가 허용되지 않으므로 최대
 *   4095바이트의 오프셋을 수용할 수 있습니다.
 *   오프셋이 4095바이트보다 크면 kvfree_rcu_arg_2()에서 컴파일 타임 오류가
 *   생성됩니다. 이 오류가 발생하면 call_rcu()를 사용하도록 폴백하거나
 *   rcu_head 구조를 처음 4096바이트에 배치하도록 구조를 재정렬할 수 있습니다.
 * 
 *   예를 들어 kmem_cache_free_rcu()와 같은 것을 허용하기 위해 허용 가능한
 *   오프셋이 향후 감소할 수 있습니다.
 *
 *   BUILD_BUG_ON 확인에는 함수 호출이 포함되지 않아야 하므로 여기서 확인은
 *   매크로에서 수행됩니다.
 */
#define kfree_rcu(ptr, rhf...) kvfree_rcu(ptr, ## rhf)

/**
 * kvfree_rcu() - kvfree an object after a grace period.
 *
 * This macro consists of one or two arguments and it is
 * based on whether an object is head-less or not. If it
 * has a head then a semantic stays the same as it used
 * to be before:
 *
 *     kvfree_rcu(ptr, rhf);
 *
 * where @ptr is a pointer to kvfree(), @rhf is the name
 * of the rcu_head structure within the type of @ptr.
 *
 * When it comes to head-less variant, only one argument
 * is passed and that is just a pointer which has to be
 * freed after a grace period. Therefore the semantic is
 *
 *     kvfree_rcu(ptr);
 *
 * where @ptr is a pointer to kvfree().
 *
 * Please note, head-less way of freeing is permitted to
 * use from a context that has to follow might_sleep()
 * annotation. Otherwise, please switch and embed the
 * rcu_head structure within the type of @ptr.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   kvfree_rcu() - 유예 기간 후 객체를 kvfree합니다.
 *
 *   이 매크로는 하나 또는 두 개의 인수로 구성되며 개체에 헤드가 없는지
 *   여부를 기반으로 합니다. 헤드가 있는 경우 시맨틱은 이전과 동일하게
 *   유지됩니다.
 *
 *   kvfree_rcu(ptr, rhf);
 *
 *   여기서 @ptr은 kvfree()에 대한 포인터이고, @rhf는 @ptr 유형 내의
 *   rcu_head 구조 이름입니다.
 *
 *   헤드리스 변형의 경우 하나의 인수만 전달되며 이는 유예 기간 후에
 *   해제되어야 하는 포인터일 뿐입니다. 그러므로 시맨틱은
 *
 *   kvfree_rcu(ptr);
 *
 *   여기서 @ptr은 kvfree()에 대한 포인터입니다.
 *
 *   헤드리스 해제 방법은 might_sleep() 주석을 따라야 하는 컨텍스트에서
 *   사용할 수 있습니다. 그렇지 않으면 @ptr 유형 내에서 rcu_head 구조를
 *   전환하고 포함하십시오.
 */
#define kvfree_rcu(...) KVFREE_GET_MACRO(__VA_ARGS__,		\
	kvfree_rcu_arg_2, kvfree_rcu_arg_1)(__VA_ARGS__)

#define KVFREE_GET_MACRO(_1, _2, NAME, ...) NAME
#define kvfree_rcu_arg_2(ptr, rhf)					\
do {									\
	typeof (ptr) ___p = (ptr);					\
									\
	if (___p) {									\
		BUILD_BUG_ON(!__is_kvfree_rcu_offset(offsetof(typeof(*(ptr)), rhf)));	\
		kvfree_call_rcu(&((___p)->rhf), (rcu_callback_t)(unsigned long)		\
			(offsetof(typeof(*(ptr)), rhf)));				\
	}										\
} while (0)

#define kvfree_rcu_arg_1(ptr)					\
do {								\
	typeof(ptr) ___p = (ptr);				\
								\
	if (___p)						\
		kvfree_call_rcu(NULL, (rcu_callback_t) (___p));	\
} while (0)

/*
 * Place this after a lock-acquisition primitive to guarantee that
 * an UNLOCK+LOCK pair acts as a full barrier.  This guarantee applies
 * if the UNLOCK and LOCK are executed by the same CPU or if the
 * UNLOCK and LOCK operate on the same lock variable.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   잠금 획득 프리미티브 뒤에 배치하여 UNLOCK+LOCK 쌍이 전체 장벽으로
 *   작동하도록 보장합니다. 이 보장은 UNLOCK 및 LOCK이 동일한 CPU에 의해
 *   실행되거나 UNLOCK 및 LOCK이 동일한 잠금 변수에서 작동하는 경우에
 *   적용됩니다.
 */
#ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE
#define smp_mb__after_unlock_lock()	smp_mb()  /* Full ordering for lock. */
#else /* #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */
#define smp_mb__after_unlock_lock()	do { } while (0)
#endif /* #else #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */


/* Has the specified rcu_head structure been handed to call_rcu()? */

/**
 * rcu_head_init - Initialize rcu_head for rcu_head_after_call_rcu()
 * @rhp: The rcu_head structure to initialize.
 *
 * If you intend to invoke rcu_head_after_call_rcu() to test whether a
 * given rcu_head structure has already been passed to call_rcu(), then
 * you must also invoke this rcu_head_init() function on it just after
 * allocating that structure.  Calls to this function must not race with
 * calls to call_rcu(), rcu_head_after_call_rcu(), or callback invocation.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   지정된 rcu_head 구조가 call_rcu()에 전달되었습니까? 
 *
 *   rcu_head_init - rcu_head_after_call_rcu()를 위해 rcu_head를 초기화합니다.
 *   @rhp: 초기화할 rcu_head 구조.
 *
 *   rcu_head_after_call_rcu()를 호출하여 주어진 rcu_head 구조가 이미
 *   call_rcu()로 전달되었는지 여부를 테스트하려면 해당 구조를 할당한 직후에
 *   rcu_head_init() 함수도 호출해야 합니다. 이 함수에 대한 호출은 call_rcu(),
 *   rcu_head_after_call_rcu() 또는 콜백 호출에 대한 호출과 경합해서는
 *   안 됩니다.
 */
static inline void rcu_head_init(struct rcu_head *rhp)
{
	rhp->func = (rcu_callback_t)~0L;
}

/**
 * rcu_head_after_call_rcu() - Has this rcu_head been passed to call_rcu()?
 * @rhp: The rcu_head structure to test.
 * @f: The function passed to call_rcu() along with @rhp.
 *
 * Returns @true if the @rhp has been passed to call_rcu() with @func,
 * and @false otherwise.  Emits a warning in any other case, including
 * the case where @rhp has already been invoked after a grace period.
 * Calls to this function must not race with callback invocation.  One way
 * to avoid such races is to enclose the call to rcu_head_after_call_rcu()
 * in an RCU read-side critical section that includes a read-side fetch
 * of the pointer to the structure containing @rhp.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_head_after_call_rcu() - 이 rcu_head가 call_rcu()에 전달되었습니까?
 *   @rhp: 테스트할 rcu_head 구조.
 *   @f: @rhp와 함께 call_rcu()에 전달되는 함수.
 *
 *   @rhp가 @func와 함께 call_rcu()에 전달되면 @true를 반환하고 그렇지 않으면
 *   @false를 반환합니다. @rhp가 유예 기간 후에 이미 호출된 경우를 포함하여
 *   다른 모든 경우에 경고를 내보냅니다.
 *   이 함수에 대한 호출은 콜백 호출과 경합해서는 안 됩니다. 이러한 경합을
 *   피하는 한 가지 방법은 @rhp를 포함하는 구조에 대한 포인터의 읽기 측
 *   가져오기를 포함하는 RCU 읽기 측 임계 섹션에 rcu_head_after_call_rcu()에
 *   대한 호출을 포함하는 것입니다.
 */
static inline bool
rcu_head_after_call_rcu(struct rcu_head *rhp, rcu_callback_t f)
{
	rcu_callback_t func = READ_ONCE(rhp->func);

	if (func == f)
		return true;
	WARN_ON_ONCE(func != (rcu_callback_t)~0L);
	return false;
}

/* kernel/ksysfs.c definitions */
extern int rcu_expedited;
extern int rcu_normal;

#endif /* __LINUX_RCUPDATE_H */
