/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_MM_H
#define _LINUX_SCHED_MM_H

#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>
#include <linux/sync_core.h>

/*
 * Routines for handling mm_structs
 */
extern struct mm_struct *mm_alloc(void);

/**
 * mmgrab() - Pin a &struct mm_struct.
 * @mm: The &struct mm_struct to pin.
 *
 * Make sure that @mm will not get freed even after the owning task
 * exits. This doesn't guarantee that the associated address space
 * will still exist later on and mmget_not_zero() has to be used before
 * accessing it.
 *
 * This is a preferred way to pin @mm for a longer/unbounded amount
 * of time.
 *
 * Use mmdrop() to release the reference acquired by mmgrab().
 *
 * See also <Documentation/vm/active_mm.rst> for an in-depth explanation
 * of &mm_struct.mm_count vs &mm_struct.mm_users.
 */
/*
 * IAMROOT, 2022.05.07:
 * - @mm에 대한 refcnt(pin)를 증가시킨다.
 */
static inline void mmgrab(struct mm_struct *mm)
{
	atomic_inc(&mm->mm_count);
}

extern void __mmdrop(struct mm_struct *mm);

static inline void mmdrop(struct mm_struct *mm)
{
	/*
	 * The implicit full barrier implied by atomic_dec_and_test() is
	 * required by the membarrier system call before returning to
	 * user-space, after storing to rq->curr.
	 */
	if (unlikely(atomic_dec_and_test(&mm->mm_count)))
		__mmdrop(mm);
}

/**
 * mmget() - Pin the address space associated with a &struct mm_struct.
 * @mm: The address space to pin.
 *
 * Make sure that the address space of the given &struct mm_struct doesn't
 * go away. This does not protect against parts of the address space being
 * modified or freed, however.
 *
 * Never use this function to pin this address space for an
 * unbounded/indefinite amount of time.
 *
 * Use mmput() to release the reference acquired by mmget().
 *
 * See also <Documentation/vm/active_mm.rst> for an in-depth explanation
 * of &mm_struct.mm_count vs &mm_struct.mm_users.
 */
static inline void mmget(struct mm_struct *mm)
{
	atomic_inc(&mm->mm_users);
}

static inline bool mmget_not_zero(struct mm_struct *mm)
{
	return atomic_inc_not_zero(&mm->mm_users);
}

/* mmput gets rid of the mappings and all user-space */
extern void mmput(struct mm_struct *);
#ifdef CONFIG_MMU
/* same as above but performs the slow path from the async context. Can
 * be called from the atomic context as well
 */
void mmput_async(struct mm_struct *);
#endif

/* Grab a reference to a task's mm, if it is not already going away */
extern struct mm_struct *get_task_mm(struct task_struct *task);
/*
 * Grab a reference to a task's mm, if it is not already going away
 * and ptrace_may_access with the mode parameter passed to it
 * succeeds.
 */
extern struct mm_struct *mm_access(struct task_struct *task, unsigned int mode);
/* Remove the current tasks stale references to the old mm_struct on exit() */
extern void exit_mm_release(struct task_struct *, struct mm_struct *);
/* Remove the current tasks stale references to the old mm_struct on exec() */
extern void exec_mm_release(struct task_struct *, struct mm_struct *);

#ifdef CONFIG_MEMCG
extern void mm_update_next_owner(struct mm_struct *mm);
#else
static inline void mm_update_next_owner(struct mm_struct *mm)
{
}
#endif /* CONFIG_MEMCG */

#ifdef CONFIG_MMU
extern void arch_pick_mmap_layout(struct mm_struct *mm,
				  struct rlimit *rlim_stack);
extern unsigned long
arch_get_unmapped_area(struct file *, unsigned long, unsigned long,
		       unsigned long, unsigned long);
extern unsigned long
arch_get_unmapped_area_topdown(struct file *filp, unsigned long addr,
			  unsigned long len, unsigned long pgoff,
			  unsigned long flags);
#else
static inline void arch_pick_mmap_layout(struct mm_struct *mm,
					 struct rlimit *rlim_stack) {}
#endif

static inline bool in_vfork(struct task_struct *tsk)
{
	bool ret;

	/*
	 * need RCU to access ->real_parent if CLONE_VM was used along with
	 * CLONE_PARENT.
	 *
	 * We check real_parent->mm == tsk->mm because CLONE_VFORK does not
	 * imply CLONE_VM
	 *
	 * CLONE_VFORK can be used with CLONE_PARENT/CLONE_THREAD and thus
	 * ->real_parent is not necessarily the task doing vfork(), so in
	 * theory we can't rely on task_lock() if we want to dereference it.
	 *
	 * And in this case we can't trust the real_parent->mm == tsk->mm
	 * check, it can be false negative. But we do not care, if init or
	 * another oom-unkillable task does this it should blame itself.
	 */
	rcu_read_lock();
	ret = tsk->vfork_done &&
			rcu_dereference(tsk->real_parent)->mm == tsk->mm;
	rcu_read_unlock();

	return ret;
}

/*
 * Applies per-task gfp context to the given allocation flags.
 * PF_MEMALLOC_NOIO implies GFP_NOIO
 * PF_MEMALLOC_NOFS implies GFP_NOFS
 * PF_MEMALLOC_PIN  implies !GFP_MOVABLE
 */
/*
 * IAMROOT, 2022.01.23:
 * - 아래에서 나오는 PF_XXX.. flag들은 주로 memory가 부족할때 신경써야
 *   되는 것들이다
 * - memory가 부족할때 어떻게 동작해야된다는게 current->flags에 설정되있고,
 *   해당 설정으로 요청된 flags를 고친다는 개념으로 보면된다.
 *
 * --- 대략적인 flag 설명
 * - PF_MEMALLOC_NOIO 존재 -> __GFP_IO, __GFP_FS 제거
 *   할당을 하면서 어떤 interface를 써야되는 상황이 있는데, 그러한것을
 *   하지 말라는뜻. 이 '상황'에는 swap memory등이 포함된다고 하며 즉
 *   NOFS보다 더 넓은 의미(weak)라고 생각하면 되는거같다.
 * - PF_MEMALLOC_NOFS 존재 -> __GFP_FS제거
 *   swap영역같은 fs를 쓰면서까지 할당시도를 하지 말라는것.
 * - PF_MEMALLOC_PIN 존재 -> __GFP_MOVABLE 제거
 */
static inline gfp_t current_gfp_context(gfp_t flags)
{
	unsigned int pflags = READ_ONCE(current->flags);

	if (unlikely(pflags & (PF_MEMALLOC_NOIO | PF_MEMALLOC_NOFS | PF_MEMALLOC_PIN))) {
		/*
		 * NOIO implies both NOIO and NOFS and it is a weaker context
		 * so always make sure it makes precedence
		 */
		if (pflags & PF_MEMALLOC_NOIO)
			flags &= ~(__GFP_IO | __GFP_FS);
		else if (pflags & PF_MEMALLOC_NOFS)
			flags &= ~__GFP_FS;

		if (pflags & PF_MEMALLOC_PIN)
			flags &= ~__GFP_MOVABLE;
	}
	return flags;
}

#ifdef CONFIG_LOCKDEP
extern void __fs_reclaim_acquire(unsigned long ip);
extern void __fs_reclaim_release(unsigned long ip);
extern void fs_reclaim_acquire(gfp_t gfp_mask);
extern void fs_reclaim_release(gfp_t gfp_mask);
#else
static inline void __fs_reclaim_acquire(unsigned long ip) { }
static inline void __fs_reclaim_release(unsigned long ip) { }
static inline void fs_reclaim_acquire(gfp_t gfp_mask) { }
static inline void fs_reclaim_release(gfp_t gfp_mask) { }
#endif

/**
 * might_alloc - Mark possible allocation sites
 * @gfp_mask: gfp_t flags that would be used to allocate
 *
 * Similar to might_sleep() and other annotations, this can be used in functions
 * that might allocate, but often don't. Compiles to nothing without
 * CONFIG_LOCKDEP. Includes a conditional might_sleep() if @gfp allows blocking.
 */

/*
 * IAMROOT, 2022.07.23:
 * - preemption point. 잠깐 쉰다.
 */
static inline void might_alloc(gfp_t gfp_mask)
{
	fs_reclaim_acquire(gfp_mask);
	fs_reclaim_release(gfp_mask);

	might_sleep_if(gfpflags_allow_blocking(gfp_mask));
}

/**
 * memalloc_noio_save - Marks implicit GFP_NOIO allocation scope.
 *
 * This functions marks the beginning of the GFP_NOIO allocation scope.
 * All further allocations will implicitly drop __GFP_IO flag and so
 * they are safe for the IO critical section from the allocation recursion
 * point of view. Use memalloc_noio_restore to end the scope with flags
 * returned by this function.
 *
 * This function is safe to be used from any context.
 */
static inline unsigned int memalloc_noio_save(void)
{
	unsigned int flags = current->flags & PF_MEMALLOC_NOIO;
	current->flags |= PF_MEMALLOC_NOIO;
	return flags;
}

/**
 * memalloc_noio_restore - Ends the implicit GFP_NOIO scope.
 * @flags: Flags to restore.
 *
 * Ends the implicit GFP_NOIO scope started by memalloc_noio_save function.
 * Always make sure that the given flags is the return value from the
 * pairing memalloc_noio_save call.
 */
static inline void memalloc_noio_restore(unsigned int flags)
{
	current->flags = (current->flags & ~PF_MEMALLOC_NOIO) | flags;
}

/**
 * memalloc_nofs_save - Marks implicit GFP_NOFS allocation scope.
 *
 * This functions marks the beginning of the GFP_NOFS allocation scope.
 * All further allocations will implicitly drop __GFP_FS flag and so
 * they are safe for the FS critical section from the allocation recursion
 * point of view. Use memalloc_nofs_restore to end the scope with flags
 * returned by this function.
 *
 * This function is safe to be used from any context.
 */
static inline unsigned int memalloc_nofs_save(void)
{
	unsigned int flags = current->flags & PF_MEMALLOC_NOFS;
	current->flags |= PF_MEMALLOC_NOFS;
	return flags;
}

/**
 * memalloc_nofs_restore - Ends the implicit GFP_NOFS scope.
 * @flags: Flags to restore.
 *
 * Ends the implicit GFP_NOFS scope started by memalloc_nofs_save function.
 * Always make sure that the given flags is the return value from the
 * pairing memalloc_nofs_save call.
 */
static inline void memalloc_nofs_restore(unsigned int flags)
{
	current->flags = (current->flags & ~PF_MEMALLOC_NOFS) | flags;
}

/*
 * IAMROOT, 2022.03.19:
 * - 현재 task에 PF_MEMALLOC을 설정하고 이전 PF_MEMALLOC값을 반환한다.
 */
static inline unsigned int memalloc_noreclaim_save(void)
{
	unsigned int flags = current->flags & PF_MEMALLOC;
	current->flags |= PF_MEMALLOC;
	return flags;
}

static inline void memalloc_noreclaim_restore(unsigned int flags)
{
	current->flags = (current->flags & ~PF_MEMALLOC) | flags;
}

static inline unsigned int memalloc_pin_save(void)
{
	unsigned int flags = current->flags & PF_MEMALLOC_PIN;

	current->flags |= PF_MEMALLOC_PIN;
	return flags;
}

static inline void memalloc_pin_restore(unsigned int flags)
{
	current->flags = (current->flags & ~PF_MEMALLOC_PIN) | flags;
}

#ifdef CONFIG_MEMCG
DECLARE_PER_CPU(struct mem_cgroup *, int_active_memcg);
/**
 * set_active_memcg - Starts the remote memcg charging scope.
 * @memcg: memcg to charge.
 *
 * This function marks the beginning of the remote memcg charging scope. All the
 * __GFP_ACCOUNT allocations till the end of the scope will be charged to the
 * given memcg.
 *
 * NOTE: This function can nest. Users must save the return value and
 * reset the previous value after their own charging scope is over.
 */
static inline struct mem_cgroup *
set_active_memcg(struct mem_cgroup *memcg)
{
	struct mem_cgroup *old;

	if (!in_task()) {
		old = this_cpu_read(int_active_memcg);
		this_cpu_write(int_active_memcg, memcg);
	} else {
		old = current->active_memcg;
		current->active_memcg = memcg;
	}

	return old;
}
#else
static inline struct mem_cgroup *
set_active_memcg(struct mem_cgroup *memcg)
{
	return NULL;
}
#endif

#ifdef CONFIG_MEMBARRIER
enum {
	MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY		= (1U << 0),
	MEMBARRIER_STATE_PRIVATE_EXPEDITED			= (1U << 1),
	MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY			= (1U << 2),
	MEMBARRIER_STATE_GLOBAL_EXPEDITED			= (1U << 3),
	MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY	= (1U << 4),
	MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE		= (1U << 5),
	MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY		= (1U << 6),
	MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ			= (1U << 7),
};

enum {
	MEMBARRIER_FLAG_SYNC_CORE	= (1U << 0),
	MEMBARRIER_FLAG_RSEQ		= (1U << 1),
};

#ifdef CONFIG_ARCH_HAS_MEMBARRIER_CALLBACKS
#include <asm/membarrier.h>
#endif

static inline void membarrier_mm_sync_core_before_usermode(struct mm_struct *mm)
{
	if (current->mm != mm)
		return;
	if (likely(!(atomic_read(&mm->membarrier_state) &
		     MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE)))
		return;
	sync_core_before_usermode();
}

extern void membarrier_exec_mmap(struct mm_struct *mm);

extern void membarrier_update_current_mm(struct mm_struct *next_mm);

#else
#ifdef CONFIG_ARCH_HAS_MEMBARRIER_CALLBACKS
static inline void membarrier_arch_switch_mm(struct mm_struct *prev,
					     struct mm_struct *next,
					     struct task_struct *tsk)
{
}
#endif
static inline void membarrier_exec_mmap(struct mm_struct *mm)
{
}
static inline void membarrier_mm_sync_core_before_usermode(struct mm_struct *mm)
{
}
static inline void membarrier_update_current_mm(struct mm_struct *next_mm)
{
}
#endif

#endif /* _LINUX_SCHED_MM_H */
