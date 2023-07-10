/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NUMA memory policies for Linux.
 * Copyright 2003,2004 Andi Kleen SuSE Labs
 */
#ifndef _LINUX_MEMPOLICY_H
#define _LINUX_MEMPOLICY_H 1

#include <linux/sched.h>
#include <linux/mmzone.h>
#include <linux/dax.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/nodemask.h>
#include <linux/pagemap.h>
#include <uapi/linux/mempolicy.h>

struct mm_struct;

#ifdef CONFIG_NUMA

/*
 * Describe a memory policy.
 *
 * A mempolicy can be either associated with a process or with a VMA.
 * For VMA related allocations the VMA policy is preferred, otherwise
 * the process policy is used. Interrupts ignore the memory policy
 * of the current process.
 *
 * Locking policy for interleave:
 * In process context there is no locking because only the process accesses
 * its own state. All vma manipulation is somewhat protected by a down_read on
 * mmap_lock.
 *
 * Freeing policy:
 * Mempolicy objects are reference counted.  A mempolicy will be freed when
 * mpol_put() decrements the reference count to zero.
 *
 * Duplicating policy objects:
 * mpol_dup() allocates a new mempolicy and copies the specified mempolicy
 * to the new storage.  The reference count of the new object is initialized
 * to 1, representing the caller of mpol_dup().
 */
/*
 * IAMROOT. 2023.06.30:
 * - google-translate
 * 메모리 정책을 설명합니다.
 *
 * Mempolicy는 프로세스 또는 VMA와 연결될 수 있습니다. VMA 관련 할당의 경우 VMA 정책이
 * 선호되고 그렇지 않으면 프로세스 정책이 사용됩니다. 인터럽트는 현재 프로세스의 메모리
 * 정책을 무시합니다.
 *
 * 인터리브에 대한 잠금 정책:
 * 프로세스 컨텍스트에서는 프로세스만 자신의 상태에 액세스하기 때문에
 * 잠금이 없습니다. 모든 vma 조작은 mmap_lock의 down_read에 의해 어느 정도
 * 보호됩니다.
 *
 * 해제 정책:
 * Mempolicy 객체는 참조 카운트됩니다. mempolicy는
 * mpol_put()이 참조 횟수를 0으로 감소시키면 해제됩니다.
 *
 * 복제 정책 개체:
 * mpol_dup()은 새 mempolicy를 할당하고 지정된 mempolicy를 새 스토리지에
 * 복사합니다. 새 개체의 참조 횟수는 mpol_dup()의 호출자를 나타내는 1로
 * 초기화됩니다.
 */
struct mempolicy {
	atomic_t refcnt;
	unsigned short mode; 	/* See MPOL_* above */
	unsigned short flags;	/* See set_mempolicy() MPOL_F_* above */
	nodemask_t nodes;	/* interleave/bind/perfer */

	union {
		nodemask_t cpuset_mems_allowed;	/* relative to these nodes */
		nodemask_t user_nodemask;	/* nodemask passed by user */
	} w;
};

/*
 * Support for managing mempolicy data objects (clone, copy, destroy)
 * The default fast path of a NULL MPOL_DEFAULT policy is always inlined.
 */

extern void __mpol_put(struct mempolicy *pol);
static inline void mpol_put(struct mempolicy *pol)
{
	if (pol)
		__mpol_put(pol);
}

/*
 * Does mempolicy pol need explicit unref after use?
 * Currently only needed for shared policies.
 */
static inline int mpol_needs_cond_ref(struct mempolicy *pol)
{
	return (pol && (pol->flags & MPOL_F_SHARED));
}

static inline void mpol_cond_put(struct mempolicy *pol)
{
	if (mpol_needs_cond_ref(pol))
		__mpol_put(pol);
}

extern struct mempolicy *__mpol_dup(struct mempolicy *pol);

/*
 * IAMROOT, 2023.04.01:
 * - @pol과 동일한 새로운 mempolicy을 할당해 가져온다.
 */
static inline struct mempolicy *mpol_dup(struct mempolicy *pol)
{
	if (pol)
		pol = __mpol_dup(pol);
	return pol;
}

#define vma_policy(vma) ((vma)->vm_policy)

static inline void mpol_get(struct mempolicy *pol)
{
	if (pol)
		atomic_inc(&pol->refcnt);
}

extern bool __mpol_equal(struct mempolicy *a, struct mempolicy *b);

/*
 * IAMROOT, 2022.05.28:
 * - @a, @b의 memory policy가 일치하는지 확인한다.
 */
static inline bool mpol_equal(struct mempolicy *a, struct mempolicy *b)
{
	if (a == b)
		return true;
	return __mpol_equal(a, b);
}

/*
 * Tree of shared policies for a shared memory region.
 * Maintain the policies in a pseudo mm that contains vmas. The vmas
 * carry the policy. As a special twist the pseudo mm is indexed in pages, not
 * bytes, so that we can work with shared memory segments bigger than
 * unsigned long.
 */

struct sp_node {
	struct rb_node nd;
	unsigned long start, end;
	struct mempolicy *policy;
};

struct shared_policy {
	struct rb_root root;
	rwlock_t lock;
};

int vma_dup_policy(struct vm_area_struct *src, struct vm_area_struct *dst);
void mpol_shared_policy_init(struct shared_policy *sp, struct mempolicy *mpol);
int mpol_set_shared_policy(struct shared_policy *info,
				struct vm_area_struct *vma,
				struct mempolicy *new);
void mpol_free_shared_policy(struct shared_policy *p);
struct mempolicy *mpol_shared_policy_lookup(struct shared_policy *sp,
					    unsigned long idx);

struct mempolicy *get_task_policy(struct task_struct *p);
struct mempolicy *__get_vma_policy(struct vm_area_struct *vma,
		unsigned long addr);
bool vma_policy_mof(struct vm_area_struct *vma);

extern void numa_default_policy(void);
extern void numa_policy_init(void);
extern void mpol_rebind_task(struct task_struct *tsk, const nodemask_t *new);
extern void mpol_rebind_mm(struct mm_struct *mm, nodemask_t *new);

extern int huge_node(struct vm_area_struct *vma,
				unsigned long addr, gfp_t gfp_flags,
				struct mempolicy **mpol, nodemask_t **nodemask);
extern bool init_nodemask_of_mempolicy(nodemask_t *mask);
extern bool mempolicy_in_oom_domain(struct task_struct *tsk,
				const nodemask_t *mask);
extern nodemask_t *policy_nodemask(gfp_t gfp, struct mempolicy *policy);

static inline nodemask_t *policy_nodemask_current(gfp_t gfp)
{
	struct mempolicy *mpol = get_task_policy(current);

	return policy_nodemask(gfp, mpol);
}

extern unsigned int mempolicy_slab_node(void);

extern enum zone_type policy_zone;

/*
 * IAMROOT, 2022.02.12: 
 * policy_zone: 
 *	movable을 제외한 최상위 zone 타입을 갱신한다.
 */
static inline void check_highest_zone(enum zone_type k)
{
	if (k > policy_zone && k != ZONE_MOVABLE)
		policy_zone = k;
}

int do_migrate_pages(struct mm_struct *mm, const nodemask_t *from,
		     const nodemask_t *to, int flags);


#ifdef CONFIG_TMPFS
extern int mpol_parse_str(char *str, struct mempolicy **mpol);
#endif

extern void mpol_to_str(char *buffer, int maxlen, struct mempolicy *pol);

/* Check if a vma is migratable */
extern bool vma_migratable(struct vm_area_struct *vma);

extern int mpol_misplaced(struct page *, struct vm_area_struct *, unsigned long);
extern void mpol_put_task_policy(struct task_struct *);

extern bool numa_demotion_enabled;

static inline bool mpol_is_preferred_many(struct mempolicy *pol)
{
	return  (pol->mode == MPOL_PREFERRED_MANY);
}


#else

struct mempolicy {};

static inline bool mpol_equal(struct mempolicy *a, struct mempolicy *b)
{
	return true;
}

static inline void mpol_put(struct mempolicy *p)
{
}

static inline void mpol_cond_put(struct mempolicy *pol)
{
}

static inline void mpol_get(struct mempolicy *pol)
{
}

struct shared_policy {};

static inline void mpol_shared_policy_init(struct shared_policy *sp,
						struct mempolicy *mpol)
{
}

static inline void mpol_free_shared_policy(struct shared_policy *p)
{
}

static inline struct mempolicy *
mpol_shared_policy_lookup(struct shared_policy *sp, unsigned long idx)
{
	return NULL;
}

#define vma_policy(vma) NULL

static inline int
vma_dup_policy(struct vm_area_struct *src, struct vm_area_struct *dst)
{
	return 0;
}

static inline void numa_policy_init(void)
{
}

static inline void numa_default_policy(void)
{
}

static inline void mpol_rebind_task(struct task_struct *tsk,
				const nodemask_t *new)
{
}

static inline void mpol_rebind_mm(struct mm_struct *mm, nodemask_t *new)
{
}

static inline int huge_node(struct vm_area_struct *vma,
				unsigned long addr, gfp_t gfp_flags,
				struct mempolicy **mpol, nodemask_t **nodemask)
{
	*mpol = NULL;
	*nodemask = NULL;
	return 0;
}

static inline bool init_nodemask_of_mempolicy(nodemask_t *m)
{
	return false;
}

static inline int do_migrate_pages(struct mm_struct *mm, const nodemask_t *from,
				   const nodemask_t *to, int flags)
{
	return 0;
}

static inline void check_highest_zone(int k)
{
}

#ifdef CONFIG_TMPFS
static inline int mpol_parse_str(char *str, struct mempolicy **mpol)
{
	return 1;	/* error */
}
#endif

static inline int mpol_misplaced(struct page *page, struct vm_area_struct *vma,
				 unsigned long address)
{
	return -1; /* no node preference */
}

static inline void mpol_put_task_policy(struct task_struct *task)
{
}

static inline nodemask_t *policy_nodemask_current(gfp_t gfp)
{
	return NULL;
}

#define numa_demotion_enabled	false

static inline bool mpol_is_preferred_many(struct mempolicy *pol)
{
	return  false;
}

#endif /* CONFIG_NUMA */
#endif
