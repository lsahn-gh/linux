/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NSPROXY_H
#define _LINUX_NSPROXY_H

#include <linux/spinlock.h>
#include <linux/sched.h>

struct mnt_namespace;
struct uts_namespace;
struct ipc_namespace;
struct pid_namespace;
struct cgroup_namespace;
struct fs_struct;

/*
 * A structure to contain pointers to all per-process
 * namespaces - fs (mount), uts, network, sysvipc, etc.
 *
 * The pid namespace is an exception -- it's accessed using
 * task_active_pid_ns.  The pid namespace here is the
 * namespace that children will use.
 *
 * 'count' is the number of tasks holding a reference.
 * The count for each namespace, then, will be the number
 * of nsproxies pointing to it, not the number of tasks.
 *
 * The nsproxy is shared by tasks which share all namespaces.
 * As soon as a single namespace is cloned or unshared, the
 * nsproxy is copied.
 */
/*
 * IAMROOT, 2023.04.01:
 * - papago
 *   모든 프로세스별 네임스페이스(fs(마운트), uts, 네트워크, 
 *   sysvipc 등)에 대한 포인터를 포함하는 구조입니다. 
 *
 *   pid 네임스페이스는 예외입니다. task_active_pid_ns를 
 *   사용하여 액세스합니다. 여기서 pid 네임스페이스는 
 *   아이들이 사용할 네임스페이스입니다.
 *
 *   'count'는 참조를 보유한 작업의 수입니다.
 *   그러면 각 네임스페이스에 대한 개수는 작업 수가 아니라 
 *   해당 네임스페이스를 가리키는 nsproxy의 수가 됩니다.
 *
 *   nsproxy는 모든 네임스페이스를 공유하는 작업에서 공유됩니다.
 *   단일 네임스페이스가 복제되거나 공유 해제되는 즉시 nsproxy가 복사됩니다.
 */
struct nsproxy {
	atomic_t count;
	struct uts_namespace *uts_ns;
	struct ipc_namespace *ipc_ns;
	struct mnt_namespace *mnt_ns;
	struct pid_namespace *pid_ns_for_children;
	struct net 	     *net_ns;
	struct time_namespace *time_ns;
	struct time_namespace *time_ns_for_children;
	struct cgroup_namespace *cgroup_ns;
};
extern struct nsproxy init_nsproxy;

/*
 * A structure to encompass all bits needed to install
 * a partial or complete new set of namespaces.
 *
 * If a new user namespace is requested cred will
 * point to a modifiable set of credentials. If a pointer
 * to a modifiable set is needed nsset_cred() must be
 * used and tested.
 */
struct nsset {
	unsigned flags;
	struct nsproxy *nsproxy;
	struct fs_struct *fs;
	const struct cred *cred;
};

static inline struct cred *nsset_cred(struct nsset *set)
{
	if (set->flags & CLONE_NEWUSER)
		return (struct cred *)set->cred;

	return NULL;
}

/*
 * the namespaces access rules are:
 *
 *  1. only current task is allowed to change tsk->nsproxy pointer or
 *     any pointer on the nsproxy itself.  Current must hold the task_lock
 *     when changing tsk->nsproxy.
 *
 *  2. when accessing (i.e. reading) current task's namespaces - no
 *     precautions should be taken - just dereference the pointers
 *
 *  3. the access to other task namespaces is performed like this
 *     task_lock(task);
 *     nsproxy = task->nsproxy;
 *     if (nsproxy != NULL) {
 *             / *
 *               * work with the namespaces here
 *               * e.g. get the reference on one of them
 *               * /
 *     } / *
 *         * NULL task->nsproxy means that this task is
 *         * almost dead (zombie)
 *         * /
 *     task_unlock(task);
 *
 */

int copy_namespaces(unsigned long flags, struct task_struct *tsk);
void exit_task_namespaces(struct task_struct *tsk);
void switch_task_namespaces(struct task_struct *tsk, struct nsproxy *new);
void free_nsproxy(struct nsproxy *ns);
int unshare_nsproxy_namespaces(unsigned long, struct nsproxy **,
	struct cred *, struct fs_struct *);
int __init nsproxy_cache_init(void);

/*
 * IAMROOT, 2023.04.01:
 * - ref down / free
 */
static inline void put_nsproxy(struct nsproxy *ns)
{
	if (atomic_dec_and_test(&ns->count)) {
		free_nsproxy(ns);
	}
}

/*
 * IAMROOT, 2023.04.01:
 * - ref up
 */
static inline void get_nsproxy(struct nsproxy *ns)
{
	atomic_inc(&ns->count);
}

#endif
