/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_TASK_STACK_H
#define _LINUX_SCHED_TASK_STACK_H

/*
 * task->stack (kernel stack) handling interfaces:
 */

#include <linux/sched.h>
#include <linux/magic.h>

/* IAMROOT, 2021.09.11:
 * - 참고: include/linux/sched.h thread_union 주석
 */
#ifdef CONFIG_THREAD_INFO_IN_TASK

/*
 * When accessing the stack of a non-current task that might exit, use
 * try_get_task_stack() instead.  task_stack_page will return a pointer
 * that could get freed out from under you.
 */
static inline void *task_stack_page(const struct task_struct *task)
{
	return task->stack;
}

/*
 * IAMROOT, 2023.04.01:
 * - thread info가 task 구조체 안에 있는에 있는 config이므로 
 *   parent에서 복사할 필요가없다.
 */
#define setup_thread_stack(new,old)	do { } while(0)

/* IAMROOT, 2021.09.14:
 * - CONFIG_THREAD_INFO_IN_TASK == on.
 * - thread_info는 struct task_struct에 존재하므로 stack은 stack으로만 사용된다.
 *   따라서 stack addr가 stack end를 가리킨다.
 */
static inline unsigned long *end_of_stack(const struct task_struct *task)
{
	return task->stack;
}

#elif !defined(__HAVE_THREAD_FUNCTIONS)

#define task_stack_page(task)	((void *)(task)->stack)

/*
 * IAMROOT, 2023.04.01:
 * - parent thread info를 child 에 복사한다.
 * - thread info가 task struct에 없는 설정인경우이다
 */
static inline void setup_thread_stack(struct task_struct *p, struct task_struct *org)
{
	*task_thread_info(p) = *task_thread_info(org);
	task_thread_info(p)->task = p;
}

/* IAMROOT, 2021.09.14:
 * - CONFIG_THREAD_INFO_IN_TASK == off.
 * - stack은 stack으로 사용될 뿐만 아니라 thread_info도 저장하므로 그에 알맞게
 *   pointer를 조정하여 stack end를 구한다.
 *
 * - GROW UP
 *   task_thread_info(..)를 통해 thread_info의 addr를 구해오는데, 이 addr 가
 *   stack의 start addr 이므로 stack end는 + THREAD_SIZE에 -1을 하여 구한다.
 *
 *   +--------- stack ---------------------------------------+ << THREAD_SIZE
 *   | thread_info |                                         |
 *   +-------------------------------------------------------+
 *                 ^                                         ^
 *                 +-- stack start                           +-- stack end
 *
 * - GROW DOWN
 *   task_thread_info(..)를 통해 thread_info의 addr를 구해오는데, 이 addr에
 *   sizeof(thread_info)를 해주면 stack end를 구할 수 있다.
 *
 *   +--------- stack ---------------------------------------+ << THREAD_SIZE
 *   | thread_info |                                         |
 *   +-------------------------------------------------------+
 *                 ^                                         ^
 *                 +-- stack end                             +-- stack start
 *
 * - Arm arch는 GROW DOWN 방식을 사용한다.
 */
/*
 * Return the address of the last usable long on the stack.
 *
 * When the stack grows down, this is just above the thread
 * info struct. Going any lower will corrupt the threadinfo.
 *
 * When the stack grows up, this is the highest address.
 * Beyond that position, we corrupt data on the next page.
 */
static inline unsigned long *end_of_stack(struct task_struct *p)
{
#ifdef CONFIG_STACK_GROWSUP
	return (unsigned long *)((unsigned long)task_thread_info(p) + THREAD_SIZE) - 1;
#else
	return (unsigned long *)(task_thread_info(p) + 1);
#endif
}

#endif

#ifdef CONFIG_THREAD_INFO_IN_TASK
static inline void *try_get_task_stack(struct task_struct *tsk)
{
	return refcount_inc_not_zero(&tsk->stack_refcount) ?
		task_stack_page(tsk) : NULL;
}

extern void put_task_stack(struct task_struct *tsk);
#else
static inline void *try_get_task_stack(struct task_struct *tsk)
{
	return task_stack_page(tsk);
}

static inline void put_task_stack(struct task_struct *tsk) {}
#endif

#define task_stack_end_corrupted(task) \
		(*(end_of_stack(task)) != STACK_END_MAGIC)

static inline int object_is_on_stack(const void *obj)
{
	void *stack = task_stack_page(current);

	return (obj >= stack) && (obj < (stack + THREAD_SIZE));
}

extern void thread_stack_cache_init(void);

#ifdef CONFIG_DEBUG_STACK_USAGE
static inline unsigned long stack_not_used(struct task_struct *p)
{
	unsigned long *n = end_of_stack(p);

	do { 	/* Skip over canary */
# ifdef CONFIG_STACK_GROWSUP
		n--;
# else
		n++;
# endif
	} while (!*n);

# ifdef CONFIG_STACK_GROWSUP
	return (unsigned long)end_of_stack(p) - (unsigned long)n;
# else
	return (unsigned long)n - (unsigned long)end_of_stack(p);
# endif
}
#endif
extern void set_task_stack_end_magic(struct task_struct *tsk);

#ifndef __HAVE_ARCH_KSTACK_END
static inline int kstack_end(void *addr)
{
	/* Reliable end of stack detection:
	 * Some APM bios versions misalign the stack
	 */
	return !(((unsigned long)addr+sizeof(void*)-1) & (THREAD_SIZE-sizeof(void*)));
}
#endif

#endif /* _LINUX_SCHED_TASK_STACK_H */
