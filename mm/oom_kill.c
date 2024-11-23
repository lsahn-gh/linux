// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/oom_kill.c
 * 
 *  Copyright (C)  1998,2000  Rik van Riel
 *	Thanks go out to Claus Fischer for some serious inspiration and
 *	for goading me into coding this file...
 *  Copyright (C)  2010  Google, Inc.
 *	Rewritten by David Rientjes
 *
 *  The routines in this file are used to kill a process when
 *  we're seriously out of memory. This gets called from __alloc_pages()
 *  in mm/page_alloc.c when we really run out of memory.
 *
 *  Since we won't call these routines often (on a well-configured
 *  machine) this file will double as a 'coding guide' and a signpost
 *  for newbie kernel hackers. It features several pointers to major
 *  kernel subsystems and hints as to where to find out what things do.
 */

#include <linux/oom.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/coredump.h>
#include <linux/sched/task.h>
#include <linux/sched/debug.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/timex.h>
#include <linux/jiffies.h>
#include <linux/cpuset.h>
#include <linux/export.h>
#include <linux/notifier.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/security.h>
#include <linux/ptrace.h>
#include <linux/freezer.h>
#include <linux/ftrace.h>
#include <linux/ratelimit.h>
#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/mmu_notifier.h>

#include <asm/tlb.h>
#include "internal.h"
#include "slab.h"

#define CREATE_TRACE_POINTS
#include <trace/events/oom.h>

/*
 * IAMROOT, 2022.05.07:
 * --- sysctl_panic_on_oom
 *
 * - /proc/sys/vm/panic_on_oom
 * - Documentation 참고
 *  0으로 설정하면 커널은 oom_killer라는 일부 악성 프로세스를 종료합니다.
 *  일반적으로 oom_killer는 악성 프로세스를 제거할 수 있으며 시스템은
 *  존속합니다.
 *
 * 1로 설정하면 메모리 부족이 발생할 때 커널이 패닉에 빠집니다. 그러나
 * 프로세스가 mempolicy/cpusets에 의해 노드 사용을 제한하고 이러한 노드가
 * 메모리 소진 상태가 되면 하나의 프로세스가 oom-killer에 의해 중단될 수 있다.
 * 이 경우 패닉은 발생하지 않습니다. 다른 노드의 메모리가 비어 있을 수 있기
 * 때문입니다. 이는 시스템 전체 상태가 아직 치명적이지 않을 수 있음을
 * 의미합니다.
 *
 * 만약 이것이 2로 설정된다면, 위에서 언급한 것에도 커널은 강제적으로 패닉을
 * 일으킨다. memcg 아래서 oom도 발생하며, 전체 시스템이 공황상태에 빠진다. 
 *
 * --- sysctl_oom_kill_allocating_task
 *  oom이 일어날때 alloc을 시도했던 current를 최우선으로 죽일지를 정한다.
 *  설정이 되면 oom score를 할 필요없이 할당을 시도했던 current를 oom kill을 한다.
 *  (Documentation oom_kill_allocating_task 참고)
 */
int sysctl_panic_on_oom;
int sysctl_oom_kill_allocating_task;
int sysctl_oom_dump_tasks = 1;

/*
 * Serializes oom killer invocations (out_of_memory()) from all contexts to
 * prevent from over eager oom killing (e.g. when the oom killer is invoked
 * from different domains).
 *
 * oom_killer_disable() relies on this lock to stabilize oom_killer_disabled
 * and mark_oom_victim
 */
DEFINE_MUTEX(oom_lock);
/* Serializes oom_score_adj and oom_score_adj_min updates */
DEFINE_MUTEX(oom_adj_mutex);

/*
 * IAMROOT, 2022.05.07:
 * - oom controller에 memcg가 지정되있는지 확인한다.
 */
static inline bool is_memcg_oom(struct oom_control *oc)
{
	return oc->memcg != NULL;
}

#ifdef CONFIG_NUMA
/**
 * oom_cpuset_eligible() - check task eligibility for kill
 * @start: task struct of which task to consider
 * @oc: pointer to struct oom_control
 *
 * Task eligibility is determined by whether or not a candidate task, @tsk,
 * shares the same mempolicy nodes as current if it is bound by such a policy
 * and whether or not it has the same set of allowed cpuset nodes.
 *
 * This function is assuming oom-killer context and 'current' has triggered
 * the oom-killer.
 */
/*
 * IAMROOT, 2022.05.07:
 * - @start process의 thread들을 iterate하며 oc->nodemask에 따라 같은
 *   node영역을 가진 thread를 찾는다.
 * - 같은 node영역을 찾는다는 것은 현재 memory가 모자르는 node를 사용하는 다른 thread
 *   를 찾아 해당 thread를 죽이면 node에 free memory가 생기므로 node를 찾는것이다.
 */
static bool oom_cpuset_eligible(struct task_struct *start,
				struct oom_control *oc)
{
	struct task_struct *tsk;
	bool ret = false;
	const nodemask_t *mask = oc->nodemask;

	if (is_memcg_oom(oc))
		return true;

	rcu_read_lock();
	for_each_thread(start, tsk) {
/*
 * IAMROOT, 2022.05.07:
 * - mask != null이면 mempolicy로 처리한다.
 */
		if (mask) {
			/*
			 * If this is a mempolicy constrained oom, tsk's
			 * cpuset is irrelevant.  Only return true if its
			 * mempolicy intersects current, otherwise it may be
			 * needlessly killed.
			 */
			ret = mempolicy_in_oom_domain(tsk, mask);
		} else {
			/*
			 * This is not a mempolicy constrained oom, so only
			 * check the mems of tsk's cpuset.
			 */
/*
 * IAMROOT, 2022.05.07:
 * - current의 allow node가 task allow node에 포함되있는지에 대한 여부를 확인한다.
 */
			ret = cpuset_mems_allowed_intersects(current, tsk);
		}
/*
 * IAMROOT, 2022.05.07:
 * - node 영역이 겹치는 thread를 찾는순간 return true.
 */
		if (ret)
			break;
	}
	rcu_read_unlock();

	return ret;
}
#else
static bool oom_cpuset_eligible(struct task_struct *tsk, struct oom_control *oc)
{
	return true;
}
#endif /* CONFIG_NUMA */

/*
 * The process p may have detached its own ->mm while exiting or through
 * kthread_use_mm(), but one or more of its subthreads may still have a valid
 * pointer.  Return p, or any of its subthreads with a valid ->mm, with
 * task_lock() held.
 */
struct task_struct *find_lock_task_mm(struct task_struct *p)
{
	struct task_struct *t;

	rcu_read_lock();

	for_each_thread(p, t) {
		task_lock(t);
		if (likely(t->mm))
			goto found;
		task_unlock(t);
	}
	t = NULL;
found:
	rcu_read_unlock();

	return t;
}

/*
 * order == -1 means the oom kill is required by sysrq, otherwise only
 * for display purposes.
 */
/*
 * IAMROOT, 2022.05.07:
 * - sysrq요청인지(강제로 oom상태 확인. 사용자 요청) 확인한다.
 * --- /proc/sys/kernel/sysrq
 *  - help(sudo echo "g" > /proc/sysrq-trigger)
 *  sysrq: HELP : loglevel(0-9) reboot(b) crash(c) terminate-all-tasks(e)
 *  memory-full-oom-kill(f) kill-all-tasks(i) thaw-filesystems(j) sak(k)
 *  show-backtrace-all-active-cpus(l) show-memory-usage(m) nice-all-RT-tasks(n)
 *  poweroff(o) show-registers(p) show-all-timers(q) unraw(r) sync(s)
 *  show-task-states(t) unmount(u) show-blocked-tasks(w) dump-ftrace-buffer(z)
 */
static inline bool is_sysrq_oom(struct oom_control *oc)
{
	return oc->order == -1;
}

/* return true if the task is not adequate as candidate victim task. */
static bool oom_unkillable_task(struct task_struct *p)
{
	if (is_global_init(p))
		return true;
	if (p->flags & PF_KTHREAD)
		return true;
	return false;
}

/*
 * Check whether unreclaimable slab amount is greater than
 * all user memory(LRU pages).
 * dump_unreclaimable_slab() could help in the case that
 * oom due to too much unreclaimable slab used by kernel.
*/
static bool should_dump_unreclaim_slab(void)
{
	unsigned long nr_lru;

	nr_lru = global_node_page_state(NR_ACTIVE_ANON) +
		 global_node_page_state(NR_INACTIVE_ANON) +
		 global_node_page_state(NR_ACTIVE_FILE) +
		 global_node_page_state(NR_INACTIVE_FILE) +
		 global_node_page_state(NR_ISOLATED_ANON) +
		 global_node_page_state(NR_ISOLATED_FILE) +
		 global_node_page_state(NR_UNEVICTABLE);

	return (global_node_page_state_pages(NR_SLAB_UNRECLAIMABLE_B) > nr_lru);
}

/**
 * oom_badness - heuristic function to determine which candidate task to kill
 * @p: task struct of which task we should calculate
 * @totalpages: total present RAM allowed for page allocation
 *
 * The heuristic for determining which task to kill is made to be as simple and
 * predictable as possible.  The goal is to return the highest value for the
 * task consuming the most memory to avoid subsequent oom failures.
 */
/*
 * IAMROOT, 2022.05.07:
 * @return adj가중치 + @p의 실제 메모리사용량.
 * --- badness의 범위
 *
 *  [LONG_MIN, totalpages * adj / 1000 + point]
 *
 * LOING_MIN은 대상이 아니거나 adj가 -1000일때 return하므로 이것을 예외사항이라 치면
 *
 *  [0, totalpages * adj / 1000 + point]
 *
 * --- return값
 *  @p의 memory 사용값을 points라고 하면 return값은 다음과 같다.
 *
 *  adj[-1000, 1000] * totalpages / 1000 + points
 *
 *  즉 totalpage의 값의 @p의 adj(100.0% 단위)%의 비율값을 가중하여
 *  @p의 oom point에 더한다.
 *
 *  (사용량 가중치 + @p의 실제 메모리 사용량)
 * ---
 *
 * - @p에 대해서 oom_score_adj 및 memory 사용등을 적용해 point를 계산한다.
 *
 * ---
 *  /proc/${pid}/oom_score 를 통해서 점수를 볼수있지만 proc_oom_score()함수를
 *  참고하면 실제 user에서 볼때는 0 ~ 2000 값으로 변환을 하여 보여줄려고한다.
 *
 * ---
 *  proc_oom_score, proc_oom_adj_operations, proc_oom_score_adj_operations 참고
 */
long oom_badness(struct task_struct *p, unsigned long totalpages)
{
	long points;
	long adj;

	if (oom_unkillable_task(p))
		return LONG_MIN;

	p = find_lock_task_mm(p);
	if (!p)
		return LONG_MIN;

	/*
	 * Do not even consider tasks which are explicitly marked oom
	 * unkillable or have been already oom reaped or the are in
	 * the middle of vfork
	 */
/*
 * IAMROOT, 2022.05.07:
 * - signal->oom_score_adj
 *   각 thread들이 공유하고 있는 oom_score_adj.
 */
	adj = (long)p->signal->oom_score_adj;
	if (adj == OOM_SCORE_ADJ_MIN ||
			test_bit(MMF_OOM_SKIP, &p->mm->flags) ||
			in_vfork(p)) {
		task_unlock(p);
		return LONG_MIN;
	}

	/*
	 * The baseline for the badness score is the proportion of RAM that each
	 * task's rss, pagetable and swap space use.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - 실제 사용 memory(rss) + swap한 memory + pgtable 사용 memory 
 */
	points = get_mm_rss(p->mm) + get_mm_counter(p->mm, MM_SWAPENTS) +
		mm_pgtables_bytes(p->mm) / PAGE_SIZE;
	task_unlock(p);

	/* Normalize to oom_score_adj units */
/*
 * IAMROOT, 2022.05.07:
 * - adj * (totalpages / 1000) + point 값을 return한다.
 * ex) points = 100, adj = 10, totalpages = 200000
 * return = 10 * (200000 / 1000) + 100
 *        = 2100
 * @p의 실제 메모리사용량 100에 totalpage의 10%(adj)인 2000만큼 가중되어 2100이
 * 된다.
 *
 * ex) points = 100, adj = 500, totalpages = 200000
 * return = 500 * (200000 / 1000) + 100
 *        = 100100
 * @p의 실제 메모리사용량 100에 totalpage의 50%(adj)인 100000만큼 가중되어
 * 100100이 된다.
 */
	adj *= totalpages / 1000;
	points += adj;

	return points;
}

static const char * const oom_constraint_text[] = {
	[CONSTRAINT_NONE] = "CONSTRAINT_NONE",
	[CONSTRAINT_CPUSET] = "CONSTRAINT_CPUSET",
	[CONSTRAINT_MEMORY_POLICY] = "CONSTRAINT_MEMORY_POLICY",
	[CONSTRAINT_MEMCG] = "CONSTRAINT_MEMCG",
};

/*
 * Determine the type of allocation constraint.
 */
/*
 * IAMROOT, 2022.05.07:
 * - @oc에 따라 oom에 어떻게 동작할지를 정한다.
 *   memcg > node > cpu 순의 우선순위로 동작한다.
 */
static enum oom_constraint constrained_alloc(struct oom_control *oc)
{
	struct zone *zone;
	struct zoneref *z;
	enum zone_type highest_zoneidx = gfp_zone(oc->gfp_mask);
	bool cpuset_limited = false;
	int nid;

	if (is_memcg_oom(oc)) {
		oc->totalpages = mem_cgroup_get_max(oc->memcg) ?: 1;
		return CONSTRAINT_MEMCG;
	}

	/* Default to all available memory */
	oc->totalpages = totalram_pages() + total_swap_pages;

	if (!IS_ENABLED(CONFIG_NUMA))
		return CONSTRAINT_NONE;

	if (!oc->zonelist)
		return CONSTRAINT_NONE;
	/*
	 * Reach here only when __GFP_NOFAIL is used. So, we should avoid
	 * to kill current.We have to random task kill in this case.
	 * Hopefully, CONSTRAINT_THISNODE...but no way to handle it, now.
	 */
/*
 * IAMROOT, 2022.05.13:
 * - papago
 *   __GFP_NOFAIL이 사용된 경우에만 여기에 도달하십시오. 그래서, 우리는 current를
 *   죽이는 것을 피해야 한다.이 경우엔 무작위로 과업살인을 해야 한다.
 *   바라건대, CONTRESTRECT_THISNODE... 하지만 지금은 처리할 방법이 없습니다.
 */
	if (oc->gfp_mask & __GFP_THISNODE)
		return CONSTRAINT_NONE;

	/*
	 * This is not a __GFP_THISNODE allocation, so a truncated nodemask in
	 * the page allocator means a mempolicy is in effect.  Cpuset policy
	 * is enforced in get_page_from_freelist().
	 */
/*
 * IAMROOT, 2022.05.07:
 * - papgo
 *   이것은 __GFP_THISNODE 할당이 아니므로 페이지 할당기에서 잘린 노드 마스크는
 *   mem 정책이 적용됨을 의미합니다. CPUset 정책은 get_page_from_freeelist()에
 *   적용됩니다.
 * - @oc->nodemask에 memory node가 전부 포함이 안되있으면 @oc->nodemask
 *   에 설정된 node에서 pages개수 + total_swap_pages를 가져오고 memory policy를
 *   따른다. (numactl 설정에 따른다.)
 */
	if (oc->nodemask &&
	    !nodes_subset(node_states[N_MEMORY], *oc->nodemask)) {
		oc->totalpages = total_swap_pages;
		for_each_node_mask(nid, *oc->nodemask)
			oc->totalpages += node_present_pages(nid);
		return CONSTRAINT_MEMORY_POLICY;
	}

	/* Check this allocation failure is caused by cpuset's wall function */
/*
 * IAMROOT, 2022.05.07:
 * - zonelist를 iterate 하면서 해당 zone의 node가 memcg에서 허용한 node인지 확인한다.
 *   허락을 안했다면 cpuset_limited가 true가된다.
 * - memory node에 oc->nodemask가 전부 포함이 되었지만 일부 memcg에는 허락이
 *   안됭상태가 발견되면 cpuset으로 적용하도록 한다.
 */
	for_each_zone_zonelist_nodemask(zone, z, oc->zonelist,
			highest_zoneidx, oc->nodemask)
		if (!cpuset_zone_allowed(zone, oc->gfp_mask))
			cpuset_limited = true;

/*
 * IAMROOT, 2022.05.07:
 * - 현재 task에 허락된 node에 대해서 totalpage를 구하고 CONSTRAINT_CPUSET으로
 *   return.
 * - memcg에 허용되지 않은 node가 발견되엇으니 cpuset으로 적용하라는 의미이다.
 */
	if (cpuset_limited) {
		oc->totalpages = total_swap_pages;
		for_each_node_mask(nid, cpuset_current_mems_allowed)
			oc->totalpages += node_present_pages(nid);
		return CONSTRAINT_CPUSET;
	}
	return CONSTRAINT_NONE;
}

/*
 * IAMROOT, 2022.05.07:
 * - @task를 oom을 위해 평가한다.
 * - select된 task는 oc->chosen에 갱신되고 해당 점수가 oc->chosen_points에 갱신된다.
 */
static int oom_evaluate_task(struct task_struct *task, void *arg)
{
	struct oom_control *oc = arg;
	long points;

/*
 * IAMROOT, 2022.05.07:
 * - 못죽이거나 memory 영향이 없는 경우엔 next로 처리한다.
 */
	if (oom_unkillable_task(task))
		goto next;

	/* p may not have freeable memory in nodemask */
	if (!is_memcg_oom(oc) && !oom_cpuset_eligible(task, oc))
		goto next;

	/*
	 * This task already has access to memory reserves and is being killed.
	 * Don't allow any other task to have access to the reserves unless
	 * the task has MMF_OOM_SKIP because chances that it would release
	 * any memory is quite low.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - papago
 *   이 작업은 이미 메모리 예약에 액세스할 수 있으므로 중지되고 있습니다.
 *   작업에 MMF_OOM_SKIP가 없는 한 다른 작업이 예약에 액세스할 수 있도록
 *   허용하지 마십시오. 메모리를 방출할 가능성이 매우 낮기 때문입니다.
 * - kernel에서 직접 발생한 oom상황에서 @task가 이미 oom 중이다.
 *   oom이 발생예정이니 현재 oom을 중단시킨다.
 */
	if (!is_sysrq_oom(oc) && tsk_is_oom_victim(task)) {
		if (test_bit(MMF_OOM_SKIP, &task->signal->oom_mm->flags))
			goto next;
		goto abort;
	}

	/*
	 * If task is allocating a lot of memory and has been marked to be
	 * killed first if it triggers an oom, then select it.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - origin flag가 set되있으면 많은 memory를 사용중으로 예상하며,
 *   첫번째 타겟으로한다.
 */
	if (oom_task_origin(task)) {
		points = LONG_MAX;
		goto select;
	}

	points = oom_badness(task, oc->totalpages);
/*
 * IAMROOT, 2022.05.07:
 * - 이전에 저장된 최대 값과 비교한다. LONG_MIN이면 아에 선택안한다.
 */
	if (points == LONG_MIN || points < oc->chosen_points)
		goto next;

select:
/*
 * IAMROOT, 2022.05.13:
 * - 선택되있던 task를 refdrop하고 새로운 task로 갱신한다.
 */
	if (oc->chosen)
		put_task_struct(oc->chosen);
	get_task_struct(task);
	oc->chosen = task;
	oc->chosen_points = points;
next:
	return 0;
abort:
	if (oc->chosen)
		put_task_struct(oc->chosen);
	oc->chosen = (void *)-1UL;
	return 1;
}

/*
 * Simple selection loop. We choose the process with the highest number of
 * 'points'. In case scan was aborted, oc->chosen is set to -1.
 */
/*
 * IAMROOT, 2022.05.07:
 * - oom point가 가장 높은 process를 찾아서 @oc에 기록한다.
 */
static void select_bad_process(struct oom_control *oc)
{
	oc->chosen_points = LONG_MIN;

/*
 * IAMROOT, 2022.05.07:
 * - memcg에서의 oom이면 memcg에서, 아니면 전체 process에서 oom_evaluate_task를
 *   수행한다.
 */
	if (is_memcg_oom(oc))
		mem_cgroup_scan_tasks(oc->memcg, oom_evaluate_task, oc);
	else {
		struct task_struct *p;

		rcu_read_lock();
		for_each_process(p)
			if (oom_evaluate_task(p, oc))
				break;
		rcu_read_unlock();
	}
}

static int dump_task(struct task_struct *p, void *arg)
{
	struct oom_control *oc = arg;
	struct task_struct *task;

	if (oom_unkillable_task(p))
		return 0;

	/* p may not have freeable memory in nodemask */
	if (!is_memcg_oom(oc) && !oom_cpuset_eligible(p, oc))
		return 0;

	task = find_lock_task_mm(p);
	if (!task) {
		/*
		 * All of p's threads have already detached their mm's. There's
		 * no need to report them; they can't be oom killed anyway.
		 */
		return 0;
	}

	pr_info("[%7d] %5d %5d %8lu %8lu %8ld %8lu         %5hd %s\n",
		task->pid, from_kuid(&init_user_ns, task_uid(task)),
		task->tgid, task->mm->total_vm, get_mm_rss(task->mm),
		mm_pgtables_bytes(task->mm),
		get_mm_counter(task->mm, MM_SWAPENTS),
		task->signal->oom_score_adj, task->comm);
	task_unlock(task);

	return 0;
}

/**
 * dump_tasks - dump current memory state of all system tasks
 * @oc: pointer to struct oom_control
 *
 * Dumps the current memory state of all eligible tasks.  Tasks not in the same
 * memcg, not in the same cpuset, or bound to a disjoint set of mempolicy nodes
 * are not shown.
 * State information includes task's pid, uid, tgid, vm size, rss,
 * pgtables_bytes, swapents, oom_score_adj value, and name.
 */
static void dump_tasks(struct oom_control *oc)
{
	pr_info("Tasks state (memory values in pages):\n");
	pr_info("[  pid  ]   uid  tgid total_vm      rss pgtables_bytes swapents oom_score_adj name\n");

	if (is_memcg_oom(oc))
		mem_cgroup_scan_tasks(oc->memcg, dump_task, oc);
	else {
		struct task_struct *p;

		rcu_read_lock();
		for_each_process(p)
			dump_task(p, oc);
		rcu_read_unlock();
	}
}

static void dump_oom_summary(struct oom_control *oc, struct task_struct *victim)
{
	/* one line summary of the oom killer context. */
	pr_info("oom-kill:constraint=%s,nodemask=%*pbl",
			oom_constraint_text[oc->constraint],
			nodemask_pr_args(oc->nodemask));
	cpuset_print_current_mems_allowed();
	mem_cgroup_print_oom_context(oc->memcg, victim);
	pr_cont(",task=%s,pid=%d,uid=%d\n", victim->comm, victim->pid,
		from_kuid(&init_user_ns, task_uid(victim)));
}

static void dump_header(struct oom_control *oc, struct task_struct *p)
{
	pr_warn("%s invoked oom-killer: gfp_mask=%#x(%pGg), order=%d, oom_score_adj=%hd\n",
		current->comm, oc->gfp_mask, &oc->gfp_mask, oc->order,
			current->signal->oom_score_adj);
	if (!IS_ENABLED(CONFIG_COMPACTION) && oc->order)
		pr_warn("COMPACTION is disabled!!!\n");

	dump_stack();
	if (is_memcg_oom(oc))
		mem_cgroup_print_oom_meminfo(oc->memcg);
	else {
		show_mem(SHOW_MEM_FILTER_NODES, oc->nodemask);
		if (should_dump_unreclaim_slab())
			dump_unreclaimable_slab();
	}
	if (sysctl_oom_dump_tasks)
		dump_tasks(oc);
	if (p)
		dump_oom_summary(oc, p);
}

/*
 * Number of OOM victims in flight
 */
static atomic_t oom_victims = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(oom_victims_wait);

static bool oom_killer_disabled __read_mostly;

#define K(x) ((x) << (PAGE_SHIFT-10))

/*
 * task->mm can be NULL if the task is the exited group leader.  So to
 * determine whether the task is using a particular mm, we examine all the
 * task's threads: if one of those is using this mm then this task was also
 * using it.
 */
/*
 * IAMROOT, 2022.05.13:
 * - papgo
 *   작업이 종료된 그룹 리더인 경우 task->mm는 NULL일 수 있습니다.
 *   따라서 작업이 특정 mm를 사용하고 있는지 확인하기 위해 모든 작업의 스레드를
 *   검사합니다.
 *   그 중 하나가 이 mm를 사용하고 있다면, 이 작업도 그것을 사용하고 있었다.
 */
bool process_shares_mm(struct task_struct *p, struct mm_struct *mm)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		struct mm_struct *t_mm = READ_ONCE(t->mm);
		if (t_mm)
			return t_mm == mm;
	}
	return false;
}

#ifdef CONFIG_MMU
/*
 * OOM Reaper kernel thread which tries to reap the memory used by the OOM
 * victim (if that is possible) to help the OOM killer to move on.
 */
static struct task_struct *oom_reaper_th;
static DECLARE_WAIT_QUEUE_HEAD(oom_reaper_wait);
static struct task_struct *oom_reaper_list;
static DEFINE_SPINLOCK(oom_reaper_lock);

bool __oom_reap_task_mm(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	bool ret = true;

	/*
	 * Tell all users of get_user/copy_from_user etc... that the content
	 * is no longer stable. No barriers really needed because unmapping
	 * should imply barriers already and the reader would hit a page fault
	 * if it stumbled over a reaped memory.
	 */
	set_bit(MMF_UNSTABLE, &mm->flags);

	for (vma = mm->mmap ; vma; vma = vma->vm_next) {
		if (!can_madv_lru_vma(vma))
			continue;

		/*
		 * Only anonymous pages have a good chance to be dropped
		 * without additional steps which we cannot afford as we
		 * are OOM already.
		 *
		 * We do not even care about fs backed pages because all
		 * which are reclaimable have already been reclaimed and
		 * we do not want to block exit_mmap by keeping mm ref
		 * count elevated without a good reason.
		 */
		if (vma_is_anonymous(vma) || !(vma->vm_flags & VM_SHARED)) {
			struct mmu_notifier_range range;
			struct mmu_gather tlb;

			mmu_notifier_range_init(&range, MMU_NOTIFY_UNMAP, 0,
						vma, mm, vma->vm_start,
						vma->vm_end);
			tlb_gather_mmu(&tlb, mm);
			if (mmu_notifier_invalidate_range_start_nonblock(&range)) {
				tlb_finish_mmu(&tlb);
				ret = false;
				continue;
			}
			unmap_page_range(&tlb, vma, range.start, range.end, NULL);
			mmu_notifier_invalidate_range_end(&range);
			tlb_finish_mmu(&tlb);
		}
	}

	return ret;
}

/*
 * Reaps the address space of the give task.
 *
 * Returns true on success and false if none or part of the address space
 * has been reclaimed and the caller should retry later.
 */
static bool oom_reap_task_mm(struct task_struct *tsk, struct mm_struct *mm)
{
	bool ret = true;

	if (!mmap_read_trylock(mm)) {
		trace_skip_task_reaping(tsk->pid);
		return false;
	}

	/*
	 * MMF_OOM_SKIP is set by exit_mmap when the OOM reaper can't
	 * work on the mm anymore. The check for MMF_OOM_SKIP must run
	 * under mmap_lock for reading because it serializes against the
	 * mmap_write_lock();mmap_write_unlock() cycle in exit_mmap().
	 */
	if (test_bit(MMF_OOM_SKIP, &mm->flags)) {
		trace_skip_task_reaping(tsk->pid);
		goto out_unlock;
	}

	trace_start_task_reaping(tsk->pid);

	/* failed to reap part of the address space. Try again later */
	ret = __oom_reap_task_mm(mm);
	if (!ret)
		goto out_finish;

	pr_info("oom_reaper: reaped process %d (%s), now anon-rss:%lukB, file-rss:%lukB, shmem-rss:%lukB\n",
			task_pid_nr(tsk), tsk->comm,
			K(get_mm_counter(mm, MM_ANONPAGES)),
			K(get_mm_counter(mm, MM_FILEPAGES)),
			K(get_mm_counter(mm, MM_SHMEMPAGES)));
out_finish:
	trace_finish_task_reaping(tsk->pid);
out_unlock:
	mmap_read_unlock(mm);

	return ret;
}

#define MAX_OOM_REAP_RETRIES 10
/*
 * IAMROOT, 2022.05.14:
 * - lock때문에 못죽이는 process들에 대해서 해제될까지 기다리며 잘 죽었는지에 대해
 *   최대 1초동안 감시한다.
 */
static void oom_reap_task(struct task_struct *tsk)
{
	int attempts = 0;
	struct mm_struct *mm = tsk->signal->oom_mm;

	/* Retry the mmap_read_trylock(mm) a few times */
	while (attempts++ < MAX_OOM_REAP_RETRIES && !oom_reap_task_mm(tsk, mm))
		schedule_timeout_idle(HZ/10);

	if (attempts <= MAX_OOM_REAP_RETRIES ||
	    test_bit(MMF_OOM_SKIP, &mm->flags))
		goto done;

	pr_info("oom_reaper: unable to reap pid:%d (%s)\n",
		task_pid_nr(tsk), tsk->comm);
	sched_show_task(tsk);
	debug_show_all_locks();

done:
	tsk->oom_reaper_list = NULL;

	/*
	 * Hide this mm from OOM killer because it has been either reaped or
	 * somebody can't call mmap_write_unlock(mm).
	 */
	set_bit(MMF_OOM_SKIP, &mm->flags);

	/* Drop a reference taken by wake_oom_reaper */
	put_task_struct(tsk);
}

/*
 * IAMROOT, 2022.05.14:
 * - oom_reaper_list에 member가 있으면 oom_reap_task를 호출한다.
 */
static int oom_reaper(void *unused)
{
	while (true) {
		struct task_struct *tsk = NULL;

		wait_event_freezable(oom_reaper_wait, oom_reaper_list != NULL);
		spin_lock(&oom_reaper_lock);
		if (oom_reaper_list != NULL) {
			tsk = oom_reaper_list;
			oom_reaper_list = tsk->oom_reaper_list;
		}
		spin_unlock(&oom_reaper_lock);

		if (tsk)
			oom_reap_task(tsk);
	}

	return 0;
}

/*
 * IAMROOT, 2022.05.07:
 * - @task를 oom_reaper에 등록시키고 깨운다.
 */
static void wake_oom_reaper(struct task_struct *tsk)
{
	/* mm is already queued? */
	if (test_and_set_bit(MMF_OOM_REAP_QUEUED, &tsk->signal->oom_mm->flags))
		return;

	get_task_struct(tsk);

	spin_lock(&oom_reaper_lock);
	tsk->oom_reaper_list = oom_reaper_list;
	oom_reaper_list = tsk;
	spin_unlock(&oom_reaper_lock);
	trace_wake_reaper(tsk->pid);
	wake_up(&oom_reaper_wait);
}

static int __init oom_init(void)
{
	oom_reaper_th = kthread_run(oom_reaper, NULL, "oom_reaper");
	return 0;
}
subsys_initcall(oom_init)
#else
static inline void wake_oom_reaper(struct task_struct *tsk)
{
}
#endif /* CONFIG_MMU */

/**
 * mark_oom_victim - mark the given task as OOM victim
 * @tsk: task to mark
 *
 * Has to be called with oom_lock held and never after
 * oom has been disabled already.
 *
 * tsk->mm has to be non NULL and caller has to guarantee it is stable (either
 * under task_lock or operate on the current).
 */
/*
 * IAMROOT, 2022.05.07:
 * - @task의 flag 및 oom_mm을 설정하고. frozen일경우 wakeup시킨다.
 */
static void mark_oom_victim(struct task_struct *tsk)
{
	struct mm_struct *mm = tsk->mm;

	WARN_ON(oom_killer_disabled);

/*
 * IAMROOT, 2022.05.07:
 * - 이미 TIF_MEMDIE가 설정되있으면 return.
 */
	/* OOM killer might race with memcg OOM */
	if (test_and_set_tsk_thread_flag(tsk, TIF_MEMDIE))
		return;

	/* oom_mm is bound to the signal struct life time. */
/*
 * IAMROOT, 2022.05.07:
 * - oom_mm == NULL이면 mm으로 교체.
 */
	if (!cmpxchg(&tsk->signal->oom_mm, NULL, mm)) {
		mmgrab(tsk->signal->oom_mm);
		set_bit(MMF_OOM_VICTIM, &mm->flags);
	}

	/*
	 * Make sure that the task is woken up from uninterruptible sleep
	 * if it is frozen because OOM killer wouldn't be able to free
	 * any memory and livelock. freezing_slow_path will tell the freezer
	 * that TIF_MEMDIE tasks should be ignored.
	 */
	__thaw_task(tsk);
	atomic_inc(&oom_victims);
	trace_mark_victim(tsk->pid);
}

/**
 * exit_oom_victim - note the exit of an OOM victim
 */
void exit_oom_victim(void)
{
	clear_thread_flag(TIF_MEMDIE);

	if (!atomic_dec_return(&oom_victims))
		wake_up_all(&oom_victims_wait);
}

/**
 * oom_killer_enable - enable OOM killer
 */
void oom_killer_enable(void)
{
	oom_killer_disabled = false;
	pr_info("OOM killer enabled.\n");
}

/**
 * oom_killer_disable - disable OOM killer
 * @timeout: maximum timeout to wait for oom victims in jiffies
 *
 * Forces all page allocations to fail rather than trigger OOM killer.
 * Will block and wait until all OOM victims are killed or the given
 * timeout expires.
 *
 * The function cannot be called when there are runnable user tasks because
 * the userspace would see unexpected allocation failures as a result. Any
 * new usage of this function should be consulted with MM people.
 *
 * Returns true if successful and false if the OOM killer cannot be
 * disabled.
 */
bool oom_killer_disable(signed long timeout)
{
	signed long ret;

	/*
	 * Make sure to not race with an ongoing OOM killer. Check that the
	 * current is not killed (possibly due to sharing the victim's memory).
	 */
	if (mutex_lock_killable(&oom_lock))
		return false;
	oom_killer_disabled = true;
	mutex_unlock(&oom_lock);

	ret = wait_event_interruptible_timeout(oom_victims_wait,
			!atomic_read(&oom_victims), timeout);
	if (ret <= 0) {
		oom_killer_enable();
		return false;
	}
	pr_info("OOM killer disabled.\n");

	return true;
}

/*
 * IAMROOT, 2022.05.07:
 * - task가 free memory를 반환할수있는 상태인지 확인한다.
 *   signal group이 죽고 있거나 task가 죽고있는지 확인한다.
 */
static inline bool __task_will_free_mem(struct task_struct *task)
{
	struct signal_struct *sig = task->signal;

	/*
	 * A coredumping process may sleep for an extended period in exit_mm(),
	 * so the oom killer cannot assume that the process will promptly exit
	 * and release memory.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - papgo
 *   코어 덤핑 프로세스는 exit_mm()에서 장기간 sleep할 수 있으므로, oom 킬러는
 *   프로세스가 즉시 종료되고 메모리를 방출한다고 가정할 수 없다. 
 * - memory가 늦게 반환되니 false를 반환한다.
 */
	if (sig->flags & SIGNAL_GROUP_COREDUMP)
		return false;

	if (sig->flags & SIGNAL_GROUP_EXIT)
		return true;

/*
 * IAMROOT, 2022.05.07:
 * - thread group에 포함이 되지 않았고, 해당 task가 종료중이면 곧 종료중이다.
 *   return true.
 */
	if (thread_group_empty(task) && (task->flags & PF_EXITING))
		return true;

	return false;
}

/*
 * Checks whether the given task is dying or exiting and likely to
 * release its address space. This means that all threads and processes
 * sharing the same mm have to be killed or exiting.
 * Caller has to make sure that task->mm is stable (hold task_lock or
 * it operates on the current).
 */
/*
 * IAMROOT, 2022.05.07:
 * - papgo
 *   지정된 작업이 exit 중인지 die 중인지, 주소 공간이 해제될 가능성이 있는지
 *   확인합니다. 즉, 동일한 mm를 공유하는 모든 스레드 및 프로세스를 중지하거나
 *   종료해야 합니다.
 *   호출자는 task->mm가 안정적인지 확인해야 합니다(task_lock을 유지하거나
 *   current에서 작동합니다).
 * - @task가 죽어가면서 free memrory를 반환할수있는 상태인지 확인한다.
 */
static bool task_will_free_mem(struct task_struct *task)
{
	struct mm_struct *mm = task->mm;
	struct task_struct *p;
	bool ret = true;

	/*
	 * Skip tasks without mm because it might have passed its exit_mm and
	 * exit_oom_victim. oom_reaper could have rescued that but do not rely
	 * on that for now. We can consider find_lock_task_mm in future.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - exit_mm이나 exit_oom_victim으로 task가 죽고 있는중인지 확인한다.
 */
	if (!mm)
		return false;

/*
 * IAMROOT, 2022.05.07:
 * - task가 종료되고있는데, memory를 반환할수있는 상태인지 확인한다.
 *   종료되는 task임에도 불구하고 memory를 확보할수 없으면 return false.
 */
	if (!__task_will_free_mem(task))
		return false;

	/*
	 * This task has already been drained by the oom reaper so there are
	 * only small chances it will free some more
	 */
/*
 * IAMROOT, 2022.05.07:
 * - mm이 이미 oom중이니 return false.
 */
	if (test_bit(MMF_OOM_SKIP, &mm->flags))
		return false;

	if (atomic_read(&mm->mm_users) <= 1)
		return true;

	/*
	 * Make sure that all tasks which share the mm with the given tasks
	 * are dying as well to make sure that a) nobody pins its mm and
	 * b) the task is also reapable by the oom reaper.
	 */
	rcu_read_lock();
/*
 * IAMROOT, 2022.05.07:
 * - @task의 mm을 공유하는 모든 process와 thread가 죽고있는지 확인한다.
 *   하나라도 살아있으면 return false.
 * - 모든 process를 iterate한다.
 *   mm을 share 하고, 같은 thread group이 아닌 process를 찾아서
 *   __task_will_free_mem()으로 확인한다.
 */
	for_each_process(p) {
/*
 * IAMROOT, 2022.05.13:
 * - @task와 mm을 share하고 있는 process를 찾는다.
 */
		if (!process_shares_mm(p, mm))
			continue;
/*
 * IAMROOT, 2022.05.13:
 * - @task와 @p가 같은 thread group인지 확인한다.
 */
		if (same_thread_group(task, p))
			continue;
		ret = __task_will_free_mem(p);
		if (!ret)
			break;
	}
	rcu_read_unlock();

	return ret;
}

/*
 * IAMROOT, 2022.05.14:
 * - @victim을 포함하여 관련있는 process에 sig kill을 보낸다.
 */
static void __oom_kill_process(struct task_struct *victim, const char *message)
{
	struct task_struct *p;
	struct mm_struct *mm;
	bool can_oom_reap = true;


/*
 * IAMROOT, 2022.05.14:
 * - @victim에 속한 task중에 아직 종료중이 아닌걸 찾는다. 자기자신이 종료중이 아니면
 *   자기가 대상이 될것이다.(p == victim)
 */
	p = find_lock_task_mm(victim);
	if (!p) {
		pr_info("%s: OOM victim %d (%s) is already exiting. Skip killing the task\n",
			message, task_pid_nr(victim), victim->comm);
		put_task_struct(victim);
		return;
	} else if (victim != p) {
		get_task_struct(p);
		put_task_struct(victim);
		victim = p;
	}

	/* Get a reference to safely compare mm after task_unlock(victim) */
	mm = victim->mm;
	mmgrab(mm);

	/* Raise event before sending signal: task reaper must see this */
	count_vm_event(OOM_KILL);
	memcg_memory_event_mm(mm, MEMCG_OOM_KILL);

	/*
	 * We should send SIGKILL before granting access to memory reserves
	 * in order to prevent the OOM victim from depleting the memory
	 * reserves from the user space under its control.
	 */
	do_send_sig_info(SIGKILL, SEND_SIG_PRIV, victim, PIDTYPE_TGID);
	mark_oom_victim(victim);
/*
 * IAMROOT, 2022.05.07:
 * - ex)
 *   Out of memory: Killed process 2446 (gnome-shell) total-vm:5424564kB,
 *   anon-rss:255676kB, file-rss:100240kB, shmem-rss:38948kB, UID:1000
 *   pgtables:1496kB oom_score_adj:0
 */
	pr_err("%s: Killed process %d (%s) total-vm:%lukB, anon-rss:%lukB, file-rss:%lukB, shmem-rss:%lukB, UID:%u pgtables:%lukB oom_score_adj:%hd\n",
		message, task_pid_nr(victim), victim->comm, K(mm->total_vm),
		K(get_mm_counter(mm, MM_ANONPAGES)),
		K(get_mm_counter(mm, MM_FILEPAGES)),
		K(get_mm_counter(mm, MM_SHMEMPAGES)),
		from_kuid(&init_user_ns, task_uid(victim)),
		mm_pgtables_bytes(mm) >> 10, victim->signal->oom_score_adj);
	task_unlock(victim);

	/*
	 * Kill all user processes sharing victim->mm in other thread groups, if
	 * any.  They don't get access to memory reserves, though, to avoid
	 * depletion of all memory.  This prevents mm->mmap_lock livelock when an
	 * oom killed thread cannot exit because it requires the semaphore and
	 * its contended by another thread trying to allocate memory itself.
	 * That thread will now get access to memory reserves since it has a
	 * pending fatal signal.
	 */
/*
 * IAMROOT, 2022.05.14:
 * - papgo
 *   다른 스레드 그룹에서 victim->mm를 공유하는 모든 사용자 프로세스를 삭제합니다
 *   (있는 경우). 하지만 그들은 모든 기억의 고갈을 피하기 위해 기억 보유량에
 *   접근하지 못한다. 이것은 oom killed 스레드를 종료할 수 없을 때 mm->mmap_lock
 *   라이브락을 방지한다. 왜냐하면 메모리 자체를 할당하려는 다른 스레드와
 *   세마포어가 필요하기 때문이다.
 *   이 스레드는 대기 중인 치명적인 신호를 가지고 있으므로 이제 메모리 예약에
 *   액세스할 수 있습니다.
 */
	rcu_read_lock();
	
/*
 * IAMROOT, 2022.05.14:
 * - mm을 공유하고있는 process를 찾아 SIGKILL을 보낸다.
 */
	for_each_process(p) {
		if (!process_shares_mm(p, mm))
			continue;
		if (same_thread_group(p, victim))
			continue;
		if (is_global_init(p)) {
			can_oom_reap = false;
			set_bit(MMF_OOM_SKIP, &mm->flags);
			pr_info("oom killer %d (%s) has mm pinned by %d (%s)\n",
					task_pid_nr(victim), victim->comm,
					task_pid_nr(p), p->comm);
			continue;
		}
		/*
		 * No kthread_use_mm() user needs to read from the userspace so
		 * we are ok to reap it.
		 */
		if (unlikely(p->flags & PF_KTHREAD))
			continue;
		do_send_sig_info(SIGKILL, SEND_SIG_PRIV, p, PIDTYPE_TGID);
	}
	rcu_read_unlock();

/*
 * IAMROOT, 2022.05.14:
 * - init process와 mm을 공유하고 있지 않았을때만 reaper를 호출한다.
 */
	if (can_oom_reap)
		wake_oom_reaper(victim);

	mmdrop(mm);
	put_task_struct(victim);
}
#undef K

/*
 * Kill provided task unless it's secured by setting
 * oom_score_adj to OOM_SCORE_ADJ_MIN.
 */
static int oom_kill_memcg_member(struct task_struct *task, void *message)
{
	if (task->signal->oom_score_adj != OOM_SCORE_ADJ_MIN &&
	    !is_global_init(task)) {
		get_task_struct(task);
		__oom_kill_process(task, message);
	}
	return 0;
}

/*
 * IAMROOT, 2022.05.14:
 * - @oc->chosen 및 관련 process들을 죽인다.
 */
static void oom_kill_process(struct oom_control *oc, const char *message)
{
	struct task_struct *victim = oc->chosen;
	struct mem_cgroup *oom_group;

/*
 * IAMROOT, 2022.05.14:
 * - 너무 자주 print를 안하기 위한 방어. static변수.
 */
	static DEFINE_RATELIMIT_STATE(oom_rs, DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);

	/*
	 * If the task is already exiting, don't alarm the sysadmin or kill
	 * its children or threads, just give it access to memory reserves
	 * so it can die quickly
	 */

/*
 * IAMROOT, 2022.05.14:
 * - oom kill을 하기전에 victim이 이미 죽고 있는 task라면 mark, puit을 하고 끝낸다.
 */
	task_lock(victim);
	if (task_will_free_mem(victim)) {
		mark_oom_victim(victim);
		wake_oom_reaper(victim);
		task_unlock(victim);
		put_task_struct(victim);
		return;
	}
	task_unlock(victim);


/*
 * IAMROOT, 2022.05.14:
 * - print가 자주 출력이 안되게 조정.
 */
	if (__ratelimit(&oom_rs))
		dump_header(oc, victim);

	/*
	 * Do we need to kill the entire memory cgroup?
	 * Or even one of the ancestor memory cgroups?
	 * Check this out before killing the victim task.
	 */
	oom_group = mem_cgroup_get_oom_group(victim, oc->memcg);

	__oom_kill_process(victim, message);

	/*
	 * If necessary, kill all tasks in the selected memory cgroup.
	 */
/*
 * IAMROOT, 2022.05.14:
 * - memcg에 속한 task들에 대해서 oom_kill_memcg_member()를 실행한다.
 */
	if (oom_group) {
		mem_cgroup_print_oom_group(oom_group);
		mem_cgroup_scan_tasks(oom_group, oom_kill_memcg_member,
				      (void *)message);
		mem_cgroup_put(oom_group);
	}
}

/*
 * Determines whether the kernel must panic because of the panic_on_oom sysctl.
 */
/*
 * IAMROOT, 2022.05.07:
 * - sysctl_panic_on_oom과 oc->constraint에 따라 panic을 일으킨다.
 */
static void check_panic_on_oom(struct oom_control *oc)
{
/*
 * IAMROOT, 2022.05.07:
 * - sysctl_panic_on_oom == 0
 *   oom만 동작시킨다.
 */
	if (likely(!sysctl_panic_on_oom))
		return;
	if (sysctl_panic_on_oom != 2) {
		/*
		 * panic_on_oom == 1 only affects CONSTRAINT_NONE, the kernel
		 * does not panic for cpuset, mempolicy, or memcg allocation
		 * failures.
		 */
/*
 * IAMROOT, 2022.05.07:
 * - sysctl_panic_on_oom == 1이면 mempolicy/cpusets, memcg인 상황에서는 oom에만
 *   동작시킨다.
 */
		if (oc->constraint != CONSTRAINT_NONE)
			return;
	}

/*
 * IAMROOT, 2022.05.07:
 * - sysctl_panic_on_oom == 2이거나 CONSTRAINT_NONE인 경우 무조건 panic을 일으킨다.
 * - 사용자 요청이면 panic을 안한다.
 */
	/* Do not panic for oom kills triggered by sysrq */
	if (is_sysrq_oom(oc))
		return;
	dump_header(oc, NULL);
	panic("Out of memory: %s panic_on_oom is enabled\n",
		sysctl_panic_on_oom == 2 ? "compulsory" : "system-wide");
}

static BLOCKING_NOTIFIER_HEAD(oom_notify_list);

int register_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_oom_notifier);

int unregister_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_oom_notifier);

/**
 * out_of_memory - kill the "best" process when we run out of memory
 * @oc: pointer to struct oom_control
 *
 * If we run out of memory, we have the choice between either
 * killing a random task (bad), letting the system crash (worse)
 * OR try to be smart about which process to kill. Note that we
 * don't have to be perfect here, we just have to be good.
 */
/* IAMROOT, 2022.05.14:
 * - oom 로직을 수행한다.
 *
 *   kill 대상을 탐색하고 kill을 수행하여 memroy가 확보되는지 확인한다.
 */
bool out_of_memory(struct oom_control *oc)
{
	unsigned long freed = 0;

	if (oom_killer_disabled)
		return false;

	if (!is_memcg_oom(oc)) {
		/* IAMROOT, 2024.11.18:
		 * - oom notify list에 등록된 callback을 호출하여 reclaim을 시도한다.
		 *   주로 driver에서 cb을 등록하며 호출 후 'freed > 0' 이면 반환된
		 *   page가 존재하므로 oom 수행없이 바로 return 한다.
		 */
		blocking_notifier_call_chain(&oom_notify_list, 0, &freed);
		if (freed > 0)
			/* Got some memory back in the last second. */
			return true;
	}

	/*
	 * If current has a pending SIGKILL or is exiting, then automatically
	 * select it.  The goal is to allow it to allocate so that it may
	 * quickly exit and free its memory.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - current가 종료중이고 memory를 반환할수있는 상태라면 oom관련 설정, frozen상태일
 *   경우 wakepup, oom_reaper에 등록 및 oom_reaper wakepup등을 수행하고 return true.
 */
	if (task_will_free_mem(current)) {
		mark_oom_victim(current);
		wake_oom_reaper(current);
		return true;
	}

	/*
	 * The OOM killer does not compensate for IO-less reclaim.
	 * pagefault_out_of_memory lost its gfp context so we have to
	 * make sure exclude 0 mask - all other users should have at least
	 * ___GFP_DIRECT_RECLAIM to get here. But mem_cgroup_oom() has to
	 * invoke the OOM killer even if it is a GFP_NOFS allocation.
	 */
/*
 * IAMROOT, 2022.05.07:
 * - papgo
 *   OOM 킬러는 IO가 없는 회수를 보상하지 않습니다.
 *   pagefault_out_of_memory가 gfp 컨텍스트를 손실했기 때문에 0 마스크를 제외해야
 *   합니다. 다른 모든 사용자는 __GFP_DIRECT_RECLAIM을 가지고 있어야 합니다.
 *   그러나 mem_cgroup_oom()은 GFP_NOFS 할당인 경우에도 OOM 킬러를 호출해야 합니다. 
 */
	if (oc->gfp_mask && !(oc->gfp_mask & __GFP_FS) && !is_memcg_oom(oc))
		return true;

	/*
	 * Check if there were limitations on the allocation (only relevant for
	 * NUMA and memcg) that may require different handling.
	 */
	oc->constraint = constrained_alloc(oc);
	if (oc->constraint != CONSTRAINT_MEMORY_POLICY)
		oc->nodemask = NULL;
	check_panic_on_oom(oc);

/*
 * IAMROOT, 2022.05.07:
 * - check_panic_on_oom()에서 panic이 안일어났다면 oom을 준비한다.
 * - current를 oom대상으로 할지에 대한 여부를 판단해서 맞으면 수행한다.
 *   -- !is_memcg_oom(oc)
 *		memcg 요청이 아닌 경우에만.
 *   -- sysctl_oom_kill_allocating_task
 *		사용자가 alloc을 할려했던 task가 oom을 일으키라고 설정했을 경우.
 *	 -- current->mm
 *		아직 종료중이 아닌경우. 즉 current가 아직 memory 회수를 안하고 있을경우
 *		죽여버리기 위해.
 *	 -- !oom_unkillable_task(current)
 *		current가 죽일수없는 task이면 목죽이니 확인.
 *	 -- oom_cpuset_eligible(current, oc)
 *		current가 oom 요청 node 범위에 포함되는 node를 가지고 있는지 확인.
 *	 -- current->signal->oom_score_adj != OOM_SCORE_ADJ_MIN
 *		oom disable로 해놨는지 확인.
 */

	if (!is_memcg_oom(oc) && sysctl_oom_kill_allocating_task &&
	    current->mm && !oom_unkillable_task(current) &&
	    oom_cpuset_eligible(current, oc) &&
	    current->signal->oom_score_adj != OOM_SCORE_ADJ_MIN) {
/*
 * IAMROOT, 2022.05.07:
 * - oom_score_adj가 OOM_SCORE_ADJ_MIN이 아닌경우 현재 task를 oom한다.
 */
		get_task_struct(current);
		oc->chosen = current;
		oom_kill_process(oc, "Out of memory (oom_kill_allocating_task)");
		return true;
	}

/*
 * IAMROOT, 2022.05.13:
 * - current는 oom point 계산없이는 못죽이는 task다. oom point 계산으로
 *   죽일 process를 process를 찾아서 oom을 하러간다.
 */
	select_bad_process(oc);
	/* Found nothing?!?! */
	if (!oc->chosen) {
/*
 * IAMROOT, 2022.05.07:
 * - oom 대상을 못찾았다. deadlock
 */
		dump_header(oc, NULL);
		pr_warn("Out of memory and no killable processes...\n");
		/*
		 * If we got here due to an actual allocation at the
		 * system level, we cannot survive this and will enter
		 * an endless loop in the allocator. Bail out now.
		 */
		if (!is_sysrq_oom(oc) && !is_memcg_oom(oc))
			panic("System is deadlocked on memory\n");
	}

/*
 * IAMROOT, 2022.05.07:
 * - chosen != -1UL. abort가 아니라는뜻. 즉 oom 대상 process가 찾아졌다.
 */
	if (oc->chosen && oc->chosen != (void *)-1UL)
		oom_kill_process(oc, !is_memcg_oom(oc) ? "Out of memory" :
				 "Memory cgroup out of memory");
	return !!oc->chosen;
}

/*
 * The pagefault handler calls here because it is out of memory, so kill a
 * memory-hogging task. If oom_lock is held by somebody else, a parallel oom
 * killing is already in progress so do nothing.
 */
void pagefault_out_of_memory(void)
{
	struct oom_control oc = {
		.zonelist = NULL,
		.nodemask = NULL,
		.memcg = NULL,
		.gfp_mask = 0,
		.order = 0,
	};

	if (mem_cgroup_oom_synchronize(true))
		return;

	if (!mutex_trylock(&oom_lock))
		return;
	out_of_memory(&oc);
	mutex_unlock(&oom_lock);
}

SYSCALL_DEFINE2(process_mrelease, int, pidfd, unsigned int, flags)
{
#ifdef CONFIG_MMU
	struct mm_struct *mm = NULL;
	struct task_struct *task;
	struct task_struct *p;
	unsigned int f_flags;
	bool reap = false;
	struct pid *pid;
	long ret = 0;

	if (flags)
		return -EINVAL;

	pid = pidfd_get_pid(pidfd, &f_flags);
	if (IS_ERR(pid))
		return PTR_ERR(pid);

	task = get_pid_task(pid, PIDTYPE_TGID);
	if (!task) {
		ret = -ESRCH;
		goto put_pid;
	}

	/*
	 * Make sure to choose a thread which still has a reference to mm
	 * during the group exit
	 */
	p = find_lock_task_mm(task);
	if (!p) {
		ret = -ESRCH;
		goto put_task;
	}

	if (mmget_not_zero(p->mm)) {
		mm = p->mm;
		if (task_will_free_mem(p))
			reap = true;
		else {
			/* Error only if the work has not been done already */
			if (!test_bit(MMF_OOM_SKIP, &mm->flags))
				ret = -EINVAL;
		}
	}
	task_unlock(p);

	if (!reap)
		goto drop_mm;

	if (mmap_read_lock_killable(mm)) {
		ret = -EINTR;
		goto drop_mm;
	}
	if (!__oom_reap_task_mm(mm))
		ret = -EAGAIN;
	mmap_read_unlock(mm);

drop_mm:
	if (mm)
		mmput(mm);
put_task:
	put_task_struct(task);
put_pid:
	put_pid(pid);
	return ret;
#else
	return -ENOSYS;
#endif /* CONFIG_MMU */
}
