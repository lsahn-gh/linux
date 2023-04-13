/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PID_H
#define _LINUX_PID_H

#include <linux/rculist.h>
#include <linux/wait.h>
#include <linux/refcount.h>

/*
 * IAMROOT, 2023.04.13:
 * ---- chat openai ----
 *  PIDTYPE_PID: 시스템에서 단일 프로세스를 고유하게 식별하는 일반 
 *  프로세스 ID(PID)를 나타냅니다. Linux 시스템에서 실행되는 각 프로세스에는 
 *  고유한 PID가 할당됩니다.
 *
 *  PIDTYPE_TGID: 쓰레드 그룹의 리더 쓰레드의 PID와 동일한 쓰레드 그룹 ID(TGID)를
 *  나타낸다. Linux에서 프로세스 내의 스레드는 스레드 그룹으로 구성되며 
 *  각 스레드 그룹에는 프로세스의 PID이기도 한 TGID로 식별되는 단일 리더 
 *  스레드가 있습니다.
 *
 *  PIDTYPE_PGID: 이것은 프로세스 그룹 ID(PGID)를 나타내며, 동일한 터미널
 *  세션에서 시작되거나 동일한 상위 프로세스에 의해 생성된 관련 프로세스의 
 *  모음입니다. 각 프로세스 그룹은 그룹에 있는 프로세스 중 하나의 PID인 PGID로
 *  식별됩니다. 
 *
 *  PIDTYPE_SID: 동일한 터미널 세션과 관련된 프로세스 그룹의 모음인 세션 
 *  ID(SID)를 나타냅니다. 터미널 세션은 사용자와 터미널 장치 간의 일련의 상호 
 *  작용이며 연결된 여러 프로세스 그룹을 가질 수 있습니다. 각 세션은 세션에서 
 *  첫 번째 프로세스의 PGID인 고유한 SID로 식별됩니다. 
 * ---------------------
 */
enum pid_type
{
	PIDTYPE_PID,
	PIDTYPE_TGID,
	PIDTYPE_PGID,
	PIDTYPE_SID,
	PIDTYPE_MAX,
};

/*
 * What is struct pid?
 *
 * A struct pid is the kernel's internal notion of a process identifier.
 * It refers to individual tasks, process groups, and sessions.  While
 * there are processes attached to it the struct pid lives in a hash
 * table, so it and then the processes that it refers to can be found
 * quickly from the numeric pid value.  The attached processes may be
 * quickly accessed by following pointers from struct pid.
 *
 * Storing pid_t values in the kernel and referring to them later has a
 * problem.  The process originally with that pid may have exited and the
 * pid allocator wrapped, and another process could have come along
 * and been assigned that pid.
 *
 * Referring to user space processes by holding a reference to struct
 * task_struct has a problem.  When the user space process exits
 * the now useless task_struct is still kept.  A task_struct plus a
 * stack consumes around 10K of low kernel memory.  More precisely
 * this is THREAD_SIZE + sizeof(struct task_struct).  By comparison
 * a struct pid is about 64 bytes.
 *
 * Holding a reference to struct pid solves both of these problems.
 * It is small so holding a reference does not consume a lot of
 * resources, and since a new struct pid is allocated when the numeric pid
 * value is reused (when pids wrap around) we don't mistakenly refer to new
 * processes.
 */
/*
 * IAMROOT, 2023.04.13:
 * - papago
 *   struct PID란 무엇입니까?
 *
 *   struct pid는 프로세스 식별자에 대한 커널의 내부 개념입니다.
 *   개별 작업, 프로세스 그룹 및 세션을 나타냅니다. 연결된 프로세스가 있는 동안 
 *   struct pid는 해시 테이블에 상주하므로 숫자 pid 값에서 해당 프로세스와
 *   참조하는 프로세스를 빠르게 찾을 수 있습니다. 연결된 프로세스는 
 *   struct pid의 포인터를 따라 빠르게 액세스할 수 있습니다.
 *
 *   커널에 pid_t 값을 저장하고 나중에 참조하는 데 문제가 있습니다. 원래 해당 
 *   pid가 있는 프로세스가 종료되고 pid 할당자가 래핑되었을 수 있으며 다른 
 *   프로세스가 와서 해당 pid를 할당했을 수 있습니다.
 *
 *   struct task_struct에 대한 참조를 보유하여 사용자 공간 프로세스를 참조하는 
 *   데 문제가 있습니다. 사용자 공간 프로세스가 종료되면 이제 쓸모없는 
 *   task_struct가 계속 유지됩니다. task_struct와 스택은 약 10K의 낮은 커널 
 *   메모리를 사용합니다. 보다 정확하게는 
 *   THREAD_SIZE + sizeof(struct task_struct)입니다. 비교해 보면 struct pid는 
 *   약 64바이트입니다. 
 *
 *   struct pid에 대한 참조를 보유하면 이 두 가지 문제가 모두 해결됩니다.
 *   작기 때문에 참조를 유지하는 데 많은 리소스가 소비되지 않으며 숫자 pid 값을 
 *   재사용할 때(pid가 랩 어라운드할 때) 새 struct pid가 할당되므로 실수로
 *   새 프로세스를 참조하지 않습니다.
 */


/*
 * struct upid is used to get the id of the struct pid, as it is
 * seen in particular namespace. Later the struct pid is found with
 * find_pid_ns() using the int nr and struct pid_namespace *ns.
 */
/*
 * IAMROOT, 2023.04.13:
 * - papago
 *   struct upid는 특정 네임스페이스에서 볼 수 있는 것처럼 struct pid의 id를 
 *   가져오는 데 사용됩니다. 나중에 struct pid는 int nr 및 
 *   struct pid_namespace *ns를 사용하여 find_pid_ns()로 찾을 수 있습니다.
 */

struct upid {
	int nr;
	struct pid_namespace *ns;
};

struct pid
{
	refcount_t count;
	unsigned int level;
	spinlock_t lock;
	/* lists of tasks that use this pid */
	struct hlist_head tasks[PIDTYPE_MAX];
	struct hlist_head inodes;
	/* wait queue for pidfd notifications */
	wait_queue_head_t wait_pidfd;
	struct rcu_head rcu;
/*
 * IAMROOT, 2023.04.13:
 * - create_pid_cachep()를 통한 pid_cachep에 의해서 pid가 할당되는데, 이때
 *   level에 따라서 numbers 크기를 키워준다. 
 */
	struct upid numbers[1];
};

extern struct pid init_struct_pid;

extern const struct file_operations pidfd_fops;

struct file;

extern struct pid *pidfd_pid(const struct file *file);
struct pid *pidfd_get_pid(unsigned int fd, unsigned int *flags);
int pidfd_create(struct pid *pid, unsigned int flags);

/*
 * IAMROOT, 2023.04.13:
 * - ref up
 */
static inline struct pid *get_pid(struct pid *pid)
{
	if (pid)
		refcount_inc(&pid->count);
	return pid;
}

extern void put_pid(struct pid *pid);
extern struct task_struct *pid_task(struct pid *pid, enum pid_type);
static inline bool pid_has_task(struct pid *pid, enum pid_type type)
{
	return !hlist_empty(&pid->tasks[type]);
}
extern struct task_struct *get_pid_task(struct pid *pid, enum pid_type);

extern struct pid *get_task_pid(struct task_struct *task, enum pid_type type);

/*
 * these helpers must be called with the tasklist_lock write-held.
 */
extern void attach_pid(struct task_struct *task, enum pid_type);
extern void detach_pid(struct task_struct *task, enum pid_type);
extern void change_pid(struct task_struct *task, enum pid_type,
			struct pid *pid);
extern void exchange_tids(struct task_struct *task, struct task_struct *old);
extern void transfer_pid(struct task_struct *old, struct task_struct *new,
			 enum pid_type);

struct pid_namespace;
extern struct pid_namespace init_pid_ns;

extern int pid_max;
extern int pid_max_min, pid_max_max;

/*
 * look up a PID in the hash table. Must be called with the tasklist_lock
 * or rcu_read_lock() held.
 *
 * find_pid_ns() finds the pid in the namespace specified
 * find_vpid() finds the pid by its virtual id, i.e. in the current namespace
 *
 * see also find_task_by_vpid() set in include/linux/sched.h
 */
extern struct pid *find_pid_ns(int nr, struct pid_namespace *ns);
extern struct pid *find_vpid(int nr);

/*
 * Lookup a PID in the hash table, and return with it's count elevated.
 */
extern struct pid *find_get_pid(int nr);
extern struct pid *find_ge_pid(int nr, struct pid_namespace *);

extern struct pid *alloc_pid(struct pid_namespace *ns, pid_t *set_tid,
			     size_t set_tid_size);
extern void free_pid(struct pid *pid);
extern void disable_pid_allocation(struct pid_namespace *ns);

/*
 * ns_of_pid() returns the pid namespace in which the specified pid was
 * allocated.
 *
 * NOTE:
 * 	ns_of_pid() is expected to be called for a process (task) that has
 * 	an attached 'struct pid' (see attach_pid(), detach_pid()) i.e @pid
 * 	is expected to be non-NULL. If @pid is NULL, caller should handle
 * 	the resulting NULL pid-ns.
 */
/*
 * IAMROOT, 2023.04.13:
 * - papago
 *   ns_of_pid()는 지정된 pid가 할당된 pid 네임스페이스를 반환합니다.
 *
 *   NOTE:
 *   ns_of_pid()는 연결된 'struct pid'(attach_pid(), detach_pid() 참조)가 있는 
 *   프로세스(작업)에 대해 호출될 것으로 예상됩니다. 즉, @pid는 NULL이 아닌 
 *   것으로 예상됩니다. @pid가 NULL이면 호출자는 결과 NULL pid-ns를 처리해야
 *   합니다.
 *
 * - pid level에 따른  namespace를 return한다.
 */
static inline struct pid_namespace *ns_of_pid(struct pid *pid)
{
	struct pid_namespace *ns = NULL;
	if (pid)
		ns = pid->numbers[pid->level].ns;
	return ns;
}

/*
 * is_child_reaper returns true if the pid is the init process
 * of the current namespace. As this one could be checked before
 * pid_ns->child_reaper is assigned in copy_process, we check
 * with the pid number.
 */
static inline bool is_child_reaper(struct pid *pid)
{
	return pid->numbers[pid->level].nr == 1;
}

/*
 * the helpers to get the pid's id seen from different namespaces
 *
 * pid_nr()    : global id, i.e. the id seen from the init namespace;
 * pid_vnr()   : virtual id, i.e. the id seen from the pid namespace of
 *               current.
 * pid_nr_ns() : id seen from the ns specified.
 *
 * see also task_xid_nr() etc in include/linux/sched.h
 */

static inline pid_t pid_nr(struct pid *pid)
{
	pid_t nr = 0;
	if (pid)
		nr = pid->numbers[0].nr;
	return nr;
}

pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns);
pid_t pid_vnr(struct pid *pid);

#define do_each_pid_task(pid, type, task)				\
	do {								\
		if ((pid) != NULL)					\
			hlist_for_each_entry_rcu((task),		\
				&(pid)->tasks[type], pid_links[type]) {

			/*
			 * Both old and new leaders may be attached to
			 * the same pid in the middle of de_thread().
			 */
#define while_each_pid_task(pid, type, task)				\
				if (type == PIDTYPE_PID)		\
					break;				\
			}						\
	} while (0)

#define do_each_pid_thread(pid, type, task)				\
	do_each_pid_task(pid, type, task) {				\
		struct task_struct *tg___ = task;			\
		for_each_thread(tg___, task) {

#define while_each_pid_thread(pid, type, task)				\
		}							\
		task = tg___;						\
	} while_each_pid_task(pid, type, task)
#endif /* _LINUX_PID_H */
