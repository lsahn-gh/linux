/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
 * Internal non-public definitions that provide either classic
 * or preemptible semantics.
 *
 * Copyright Red Hat, 2009
 * Copyright IBM Corporation, 2009
 *
 * Author: Ingo Molnar <mingo@elte.hu>
 *	   Paul E. McKenney <paulmck@linux.ibm.com>
 */

#include "../locking/rtmutex_common.h"

static bool rcu_rdp_is_offloaded(struct rcu_data *rdp)
{
	/*
	 * In order to read the offloaded state of an rdp is a safe
	 * and stable way and prevent from its value to be changed
	 * under us, we must either hold the barrier mutex, the cpu
	 * hotplug lock (read or write) or the nocb lock. Local
	 * non-preemptible reads are also safe. NOCB kthreads and
	 * timers have their own means of synchronization against the
	 * offloaded state updaters.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rdp의 오프로드된 상태를 읽는 것이 안전하고 안정적인 방법이며 그 값이
 *   변경되는 것을 방지하려면 배리어 뮤텍스, CPU 핫플러그 잠금(읽기 또는 쓰기)
 *   또는 nocb 잠금을 유지해야 합니다. 로컬 비선점형 읽기도 안전합니다.
 *   NOCB kthread 및 타이머는 오프로드된 상태 업데이터에 대한 고유한 동기화
 *   수단을 가지고 있습니다.
 */
	RCU_LOCKDEP_WARN(
		!(lockdep_is_held(&rcu_state.barrier_mutex) ||
		  (IS_ENABLED(CONFIG_HOTPLUG_CPU) && lockdep_is_cpus_held()) ||
		  rcu_lockdep_is_held_nocb(rdp) ||
		  (rdp == this_cpu_ptr(&rcu_data) &&
		   !(IS_ENABLED(CONFIG_PREEMPT_COUNT) && preemptible())) ||
		  rcu_current_is_nocb_kthread(rdp)),
		"Unsafe read of RCU_NOCB offloaded state"
	);

	return rcu_segcblist_is_offloaded(&rdp->cblist);
}

/*
 * Check the RCU kernel configuration parameters and print informative
 * messages about anything out of the ordinary.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU 커널 구성 매개변수를 확인하고 비정상적인 사항에 대한 정보 메시지를
 *   인쇄합니다.
 */
static void __init rcu_bootup_announce_oddness(void)
{
	if (IS_ENABLED(CONFIG_RCU_TRACE))
		pr_info("\tRCU event tracing is enabled.\n");
	if ((IS_ENABLED(CONFIG_64BIT) && RCU_FANOUT != 64) ||
	    (!IS_ENABLED(CONFIG_64BIT) && RCU_FANOUT != 32))
		pr_info("\tCONFIG_RCU_FANOUT set to non-default value of %d.\n",
			RCU_FANOUT);
	if (rcu_fanout_exact)
		pr_info("\tHierarchical RCU autobalancing is disabled.\n");
	if (IS_ENABLED(CONFIG_RCU_FAST_NO_HZ))
		pr_info("\tRCU dyntick-idle grace-period acceleration is enabled.\n");
	if (IS_ENABLED(CONFIG_PROVE_RCU))
		pr_info("\tRCU lockdep checking is enabled.\n");
	if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD))
		pr_info("\tRCU strict (and thus non-scalable) grace periods enabled.\n");
	if (RCU_NUM_LVLS >= 4)
		pr_info("\tFour(or more)-level hierarchy is enabled.\n");
	if (RCU_FANOUT_LEAF != 16)
		pr_info("\tBuild-time adjustment of leaf fanout to %d.\n",
			RCU_FANOUT_LEAF);
	if (rcu_fanout_leaf != RCU_FANOUT_LEAF)
		pr_info("\tBoot-time adjustment of leaf fanout to %d.\n",
			rcu_fanout_leaf);
	if (nr_cpu_ids != NR_CPUS)
		pr_info("\tRCU restricting CPUs from NR_CPUS=%d to nr_cpu_ids=%u.\n", NR_CPUS, nr_cpu_ids);
#ifdef CONFIG_RCU_BOOST
	pr_info("\tRCU priority boosting: priority %d delay %d ms.\n",
		kthread_prio, CONFIG_RCU_BOOST_DELAY);
#endif
	if (blimit != DEFAULT_RCU_BLIMIT)
		pr_info("\tBoot-time adjustment of callback invocation limit to %ld.\n", blimit);
	if (qhimark != DEFAULT_RCU_QHIMARK)
		pr_info("\tBoot-time adjustment of callback high-water mark to %ld.\n", qhimark);
	if (qlowmark != DEFAULT_RCU_QLOMARK)
		pr_info("\tBoot-time adjustment of callback low-water mark to %ld.\n", qlowmark);
	if (qovld != DEFAULT_RCU_QOVLD)
		pr_info("\tBoot-time adjustment of callback overload level to %ld.\n", qovld);
	if (jiffies_till_first_fqs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of first FQS scan delay to %ld jiffies.\n", jiffies_till_first_fqs);
	if (jiffies_till_next_fqs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of subsequent FQS scan delay to %ld jiffies.\n", jiffies_till_next_fqs);
	if (jiffies_till_sched_qs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of scheduler-enlistment delay to %ld jiffies.\n", jiffies_till_sched_qs);
	if (rcu_kick_kthreads)
		pr_info("\tKick kthreads if too-long grace period.\n");
	if (IS_ENABLED(CONFIG_DEBUG_OBJECTS_RCU_HEAD))
		pr_info("\tRCU callback double-/use-after-free debug enabled.\n");
	if (gp_preinit_delay)
		pr_info("\tRCU debug GP pre-init slowdown %d jiffies.\n", gp_preinit_delay);
	if (gp_init_delay)
		pr_info("\tRCU debug GP init slowdown %d jiffies.\n", gp_init_delay);
	if (gp_cleanup_delay)
		pr_info("\tRCU debug GP init slowdown %d jiffies.\n", gp_cleanup_delay);
	if (!use_softirq)
		pr_info("\tRCU_SOFTIRQ processing moved to rcuc kthreads.\n");
	if (IS_ENABLED(CONFIG_RCU_EQS_DEBUG))
		pr_info("\tRCU debug extended QS entry/exit.\n");
	rcupdate_announce_bootup_oddness();
}

#ifdef CONFIG_PREEMPT_RCU

static void rcu_report_exp_rnp(struct rcu_node *rnp, bool wake);
static void rcu_read_unlock_special(struct task_struct *t);

/*
 * Tell them what RCU they are running.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   실행 중인 RCU를 알려주십시오.
 */
static void __init rcu_bootup_announce(void)
{
	pr_info("Preemptible hierarchical RCU implementation.\n");
	rcu_bootup_announce_oddness();
}

/* Flags for rcu_preempt_ctxt_queue() decision table. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_preempt_ctxt_queue() 결정 테이블용 플래그. 
 */
#define RCU_GP_TASKS	0x8
#define RCU_EXP_TASKS	0x4
#define RCU_GP_BLKD	0x2
#define RCU_EXP_BLKD	0x1

/*
 * Queues a task preempted within an RCU-preempt read-side critical
 * section into the appropriate location within the ->blkd_tasks list,
 * depending on the states of any ongoing normal and expedited grace
 * periods.  The ->gp_tasks pointer indicates which element the normal
 * grace period is waiting on (NULL if none), and the ->exp_tasks pointer
 * indicates which element the expedited grace period is waiting on (again,
 * NULL if none).  If a grace period is waiting on a given element in the
 * ->blkd_tasks list, it also waits on all subsequent elements.  Thus,
 * adding a task to the tail of the list blocks any grace period that is
 * already waiting on one of the elements.  In contrast, adding a task
 * to the head of the list won't block any grace period that is already
 * waiting on one of the elements.
 *
 * This queuing is imprecise, and can sometimes make an ongoing grace
 * period wait for a task that is not strictly speaking blocking it.
 * Given the choice, we needlessly block a normal grace period rather than
 * blocking an expedited grace period.
 *
 * Note that an endless sequence of expedited grace periods still cannot
 * indefinitely postpone a normal grace period.  Eventually, all of the
 * fixed number of preempted tasks blocking the normal grace period that are
 * not also blocking the expedited grace period will resume and complete
 * their RCU read-side critical sections.  At that point, the ->gp_tasks
 * pointer will equal the ->exp_tasks pointer, at which point the end of
 * the corresponding expedited grace period will also be the end of the
 * normal grace period.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   진행 중인 일반 및 긴급 유예 기간의 상태에 따라 ->blkd_tasks 목록 내의
 *   적절한 위치에 RCU 선점 읽기 측 중요 섹션 내에서 선점된 작업을 대기열에
 *   넣습니다. ->gp_tasks 포인터는 일반 유예 기간이 대기 중인
 *   요소(없으면 NULL)를 나타내고 ->exp_tasks 포인터는 긴급 유예 기간이 대기
 *   중인 요소(없으면 NULL)를 나타냅니다. 유예 기간이 ->blkd_tasks 목록의
 *   지정된 요소를 기다리는 경우 모든 후속 요소도 기다립니다. 따라서 목록의
 *   끝에 작업을 추가하면 요소 중 하나에서 이미 대기 중인 유예 기간이
 *   차단됩니다. 반대로 목록의 헤드에 작업을 추가해도 요소 중 하나에서 이미
 *   대기 중인 유예 기간은 차단되지 않습니다.
 *
 *   이 대기열은 정확하지 않으며 때로는 진행 중인 유예 기간이 엄밀히 말하면
 *   차단하지 않는 작업을 기다리게 할 수 있습니다.
 *   선택권이 주어지면 긴급 유예 기간을 차단하는 대신 일반 유예 기간을
 *   불필요하게 차단합니다.
 *
 *   급행 유예 기간이 끝없이 이어져도 정상적인 유예 기간을 무기한 연기할 수는
 *   없습니다. 결국 긴급 유예 기간을 차단하지 않고 일반 유예 기간을 차단하는
 *   모든 고정된 수의 선점된 작업이 재개되어 RCU 읽기 측 임계 섹션을
 *   완료합니다. 이 시점에서 ->gp_tasks 포인터는 ->exp_tasks 포인터와 같으며,
 *   이 시점에서 해당 신속 유예 기간의 끝은 일반 유예 기간의 끝이기도 합니다.
 */
static void rcu_preempt_ctxt_queue(struct rcu_node *rnp, struct rcu_data *rdp)
	__releases(rnp->lock) /* But leaves rrupts disabled. */
{
	int blkd_state = (rnp->gp_tasks ? RCU_GP_TASKS : 0) +
			 (rnp->exp_tasks ? RCU_EXP_TASKS : 0) +
			 (rnp->qsmask & rdp->grpmask ? RCU_GP_BLKD : 0) +
			 (rnp->expmask & rdp->grpmask ? RCU_EXP_BLKD : 0);
	struct task_struct *t = current;

	raw_lockdep_assert_held_rcu_node(rnp);
	WARN_ON_ONCE(rdp->mynode != rnp);
	WARN_ON_ONCE(!rcu_is_leaf_node(rnp));
	/* RCU better not be waiting on newly onlined CPUs! */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU는 새로 온라인된 CPU를 기다리지 않는 것이 좋습니다! 
 */
	WARN_ON_ONCE(rnp->qsmaskinitnext & ~rnp->qsmaskinit & rnp->qsmask &
		     rdp->grpmask);

	/*
	 * Decide where to queue the newly blocked task.  In theory,
	 * this could be an if-statement.  In practice, when I tried
	 * that, it was quite messy.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   새로 차단된 작업을 대기열에 넣을 위치를 결정합니다. 이론적으로 이것은 if
 *   문일 수 있습니다. 실제로 사용해 보니 꽤 지저분했습니다.
 */
	switch (blkd_state) {
	case 0:
	case                RCU_EXP_TASKS:
	case                RCU_EXP_TASKS + RCU_GP_BLKD:
	case RCU_GP_TASKS:
	case RCU_GP_TASKS + RCU_EXP_TASKS:

		/*
		 * Blocking neither GP, or first task blocking the normal
		 * GP but not blocking the already-waiting expedited GP.
		 * Queue at the head of the list to avoid unnecessarily
		 * blocking the already-waiting GPs.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   GP 또는 일반 GP를 차단하는 첫 번째 작업을 차단하지 않고 이미 대기 중인
 *   긴급 GP를 차단하지 않습니다.
 *   이미 대기 중인 GP를 불필요하게 차단하지 않도록 목록의 맨 앞에
 *   대기합니다.
 */
		list_add(&t->rcu_node_entry, &rnp->blkd_tasks);
		break;

	case                                              RCU_EXP_BLKD:
	case                                RCU_GP_BLKD:
	case                                RCU_GP_BLKD + RCU_EXP_BLKD:
	case RCU_GP_TASKS +                               RCU_EXP_BLKD:
	case RCU_GP_TASKS +                 RCU_GP_BLKD + RCU_EXP_BLKD:
	case RCU_GP_TASKS + RCU_EXP_TASKS + RCU_GP_BLKD + RCU_EXP_BLKD:

		/*
		 * First task arriving that blocks either GP, or first task
		 * arriving that blocks the expedited GP (with the normal
		 * GP already waiting), or a task arriving that blocks
		 * both GPs with both GPs already waiting.  Queue at the
		 * tail of the list to avoid any GP waiting on any of the
		 * already queued tasks that are not blocking it.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   GP를 차단하는 첫 번째 작업 도착, 또는 긴급 GP를 차단하는 첫 번째 작업
 *   도착(일반 GP가 이미 대기 중) 또는 두 GP가 이미 대기 중인 두 GP를
 *   차단하는 작업 도착. 이미 대기 중인 작업을 차단하지 않는 GP가 대기하는
 *   것을 방지하기 위해 목록의 끝에 대기합니다.
 */
		list_add_tail(&t->rcu_node_entry, &rnp->blkd_tasks);
		break;

	case                RCU_EXP_TASKS +               RCU_EXP_BLKD:
	case                RCU_EXP_TASKS + RCU_GP_BLKD + RCU_EXP_BLKD:
	case RCU_GP_TASKS + RCU_EXP_TASKS +               RCU_EXP_BLKD:

		/*
		 * Second or subsequent task blocking the expedited GP.
		 * The task either does not block the normal GP, or is the
		 * first task blocking the normal GP.  Queue just after
		 * the first task blocking the expedited GP.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   긴급 GP를 차단하는 두 번째 또는 후속 작업.
 *   해당 작업은 일반 GP를 차단하지 않거나 일반 GP를 차단하는 첫 번째
 *   작업입니다. 긴급 GP를 차단하는 첫 번째 작업 직후 대기열.
 */
		list_add(&t->rcu_node_entry, rnp->exp_tasks);
		break;

	case RCU_GP_TASKS +                 RCU_GP_BLKD:
	case RCU_GP_TASKS + RCU_EXP_TASKS + RCU_GP_BLKD:

		/*
		 * Second or subsequent task blocking the normal GP.
		 * The task does not block the expedited GP. Queue just
		 * after the first task blocking the normal GP.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   일반 GP를 차단하는 두 번째 또는 후속 작업.
 *   이 작업은 긴급 GP를 차단하지 않습니다. 일반 GP를 차단하는 첫 번째 작업
 *   직후 대기열.
 */
		list_add(&t->rcu_node_entry, rnp->gp_tasks);
		break;

	default:

		/* Yet another exercise in excessive paranoia. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   과도한 편집증의 또 다른 연습. 
 */
		WARN_ON_ONCE(1);
		break;
	}

	/*
	 * We have now queued the task.  If it was the first one to
	 * block either grace period, update the ->gp_tasks and/or
	 * ->exp_tasks pointers, respectively, to reference the newly
	 * blocked tasks.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이제 작업을 대기열에 넣었습니다. 유예 기간을 처음으로 차단한 경우 새로
 *   차단된 작업을 참조하도록 각각 ->gp_tasks 및/또는 ->exp_tasks 포인터를
 *   업데이트합니다.
 */
	if (!rnp->gp_tasks && (blkd_state & RCU_GP_BLKD)) {
		WRITE_ONCE(rnp->gp_tasks, &t->rcu_node_entry);
		WARN_ON_ONCE(rnp->completedqs == rnp->gp_seq);
	}
	if (!rnp->exp_tasks && (blkd_state & RCU_EXP_BLKD))
		WRITE_ONCE(rnp->exp_tasks, &t->rcu_node_entry);
	WARN_ON_ONCE(!(blkd_state & RCU_GP_BLKD) !=
		     !(rnp->qsmask & rdp->grpmask));
	WARN_ON_ONCE(!(blkd_state & RCU_EXP_BLKD) !=
		     !(rnp->expmask & rdp->grpmask));
	raw_spin_unlock_rcu_node(rnp); /* interrupts remain disabled. */

	/*
	 * Report the quiescent state for the expedited GP.  This expedited
	 * GP should not be able to end until we report, so there should be
	 * no need to check for a subsequent expedited GP.  (Though we are
	 * still in a quiescent state in any case.)
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   긴급 GP에 대한 정지 상태를 보고합니다. 이 긴급 GP는 우리가 보고할 때까지
 *   종료될 수 없어 후속 긴급 GP를 확인할 필요가 없습니다. (어쨌든 우리는
 *   여전히 정지 상태에 있습니다.). 
 */
	if (blkd_state & RCU_EXP_BLKD && rdp->exp_deferred_qs)
		rcu_report_exp_rdp(rdp);
	else
		WARN_ON_ONCE(rdp->exp_deferred_qs);
}

/*
 * Record a preemptible-RCU quiescent state for the specified CPU.
 * Note that this does not necessarily mean that the task currently running
 * on the CPU is in a quiescent state:  Instead, it means that the current
 * grace period need not wait on any RCU read-side critical section that
 * starts later on this CPU.  It also means that if the current task is
 * in an RCU read-side critical section, it has already added itself to
 * some leaf rcu_node structure's ->blkd_tasks list.  In addition to the
 * current task, there might be any number of other tasks blocked while
 * in an RCU read-side critical section.
 *
 * Callers to this function must disable preemption.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   지정된 CPU에 대한 선점형 RCU 정지 상태를 기록합니다.
 *   이는 CPU에서 현재 실행 중인 작업이 반드시 정지 상태에 있음을 의미하지는
 *   않습니다.
 *   대신, 현재 유예 기간이 이 CPU에서 나중에 시작하는 RCU 읽기 측 중요
 *   섹션을 기다릴 필요가 없음을 의미합니다. 또한 현재 작업이 RCU 읽기 측
 *   임계 섹션에 있는 경우 일부 리프 rcu_node 구조의 ->blkd_tasks 목록에 이미
 *   자신을 추가했음을 의미합니다. 현재 작업 외에도 RCU 읽기 측 중요 섹션에
 *   있는 동안 차단된 다른 작업이 얼마든지 있을 수 있습니다.
 *
 *   이 함수에 대한 호출자는 선점을 비활성화해야 합니다.
 */
static void rcu_qs(void)
{
	RCU_LOCKDEP_WARN(preemptible(), "rcu_qs() invoked with preemption enabled!!!\n");
	if (__this_cpu_read(rcu_data.cpu_no_qs.s)) {
		trace_rcu_grace_period(TPS("rcu_preempt"),
				       __this_cpu_read(rcu_data.gp_seq),
				       TPS("cpuqs"));
		__this_cpu_write(rcu_data.cpu_no_qs.b.norm, false);
		barrier(); /* Coordinate with rcu_flavor_sched_clock_irq(). */
		WRITE_ONCE(current->rcu_read_unlock_special.b.need_qs, false);
	}
}

/*
 * We have entered the scheduler, and the current task might soon be
 * context-switched away from.  If this task is in an RCU read-side
 * critical section, we will no longer be able to rely on the CPU to
 * record that fact, so we enqueue the task on the blkd_tasks list.
 * The task will dequeue itself when it exits the outermost enclosing
 * RCU read-side critical section.  Therefore, the current grace period
 * cannot be permitted to complete until the blkd_tasks list entries
 * predating the current grace period drain, in other words, until
 * rnp->gp_tasks becomes NULL.
 *
 * Caller must disable interrupts.
 */
 /*
  * IAMROOT, 2023.01.14:
  * - papago
  *   스케줄러에 들어갔고 현재 작업이 곧 컨텍스트 전환될 수 있습니다. 이
  *   작업이 RCU 읽기 측 중요 섹션에 있는 경우 더 이상 CPU에 의존하여 해당
  *   사실을 기록할 수 없으므로 blkd_tasks 목록에 작업을 큐에 넣습니다.
  *   작업은 RCU 읽기 측 임계 섹션을 포함하는 가장 바깥쪽을 종료할 때
  *   자체적으로 대기열에서 제외됩니다. 따라서 현재 유예 기간 이전의
  *   blkd_tasks 목록 항목이 소진될 때까지, 즉 rnp->gp_tasks가 NULL이 될
  *   때까지 현재 유예 기간을 완료할 수 없습니다.
  *
  *   호출자는 인터럽트를 비활성화해야 합니다.
  * - TODO.
  */
void rcu_note_context_switch(bool preempt)
{
	struct task_struct *t = current;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp;

	trace_rcu_utilization(TPS("Start context switch"));
	lockdep_assert_irqs_disabled();
	WARN_ONCE(!preempt && rcu_preempt_depth() > 0, "Voluntary context switch within RCU read-side critical section!");
	if (rcu_preempt_depth() > 0 &&
	    !t->rcu_read_unlock_special.b.blocked) {

		/* Possibly blocking in an RCU read-side critical section. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU 읽기 측 중요 섹션에서 차단할 수 있습니다. 
 */
		rnp = rdp->mynode;
		raw_spin_lock_rcu_node(rnp);
		t->rcu_read_unlock_special.b.blocked = true;
		t->rcu_blocked_node = rnp;

		/*
		 * Verify the CPU's sanity, trace the preemption, and
		 * then queue the task as required based on the states
		 * of any ongoing and expedited grace periods.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   CPU의 상태를 확인하고 선점을 추적한 다음 진행 중인 유예 기간 및 긴급
 *   유예 기간의 상태에 따라 필요에 따라 작업을 대기열에 넣습니다.
 */
		WARN_ON_ONCE((rdp->grpmask & rcu_rnp_online_cpus(rnp)) == 0);
		WARN_ON_ONCE(!list_empty(&t->rcu_node_entry));
		trace_rcu_preempt_task(rcu_state.name,
				       t->pid,
				       (rnp->qsmask & rdp->grpmask)
				       ? rnp->gp_seq
				       : rcu_seq_snap(&rnp->gp_seq));
		rcu_preempt_ctxt_queue(rnp, rdp);
	} else {
		rcu_preempt_deferred_qs(t);
	}

	/*
	 * Either we were not in an RCU read-side critical section to
	 * begin with, or we have now recorded that critical section
	 * globally.  Either way, we can now note a quiescent state
	 * for this CPU.  Again, if we were in an RCU read-side critical
	 * section, and if that critical section was blocking the current
	 * grace period, then the fact that the task has been enqueued
	 * means that we continue to block the current grace period.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   처음부터 RCU 읽기 측 중요 섹션에 없었거나 이제 해당 중요 섹션을
 *   전역적으로 기록했습니다. 어느 쪽이든 이제 이 CPU의 정지 상태를 확인할 수
 *   있습니다. 다시 말하지만, RCU 읽기 측 임계 섹션에 있고 해당 임계 섹션이
 *   현재 유예 기간을 차단하고 있는 경우 작업이 대기열에 추가되었다는 사실은
 *   현재 유예 기간을 계속 차단한다는 의미입니다.
 */
	rcu_qs();
	if (rdp->exp_deferred_qs)
		rcu_report_exp_rdp(rdp);
	rcu_tasks_qs(current, preempt);
	trace_rcu_utilization(TPS("End context switch"));
}
EXPORT_SYMBOL_GPL(rcu_note_context_switch);

/*
 * Check for preempted RCU readers blocking the current grace period
 * for the specified rcu_node structure.  If the caller needs a reliable
 * answer, it must hold the rcu_node's ->lock.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   지정된 rcu_node 구조에 대한 현재 유예 기간을 차단하는 선점된
 *   RCU 판독기가 있는지 확인하십시오. 호출자가 신뢰할 수 있는 응답이 필요한
 *   경우 rcu_node의 -> 잠금을 보유해야 합니다.
 */
static int rcu_preempt_blocked_readers_cgp(struct rcu_node *rnp)
{
	return READ_ONCE(rnp->gp_tasks) != NULL;
}

/* limit value for ->rcu_read_lock_nesting. */
#define RCU_NEST_PMAX (INT_MAX / 2)

static void rcu_preempt_read_enter(void)
{
	WRITE_ONCE(current->rcu_read_lock_nesting, READ_ONCE(current->rcu_read_lock_nesting) + 1);
}

static int rcu_preempt_read_exit(void)
{
	int ret = READ_ONCE(current->rcu_read_lock_nesting) - 1;

	WRITE_ONCE(current->rcu_read_lock_nesting, ret);
	return ret;
}

static void rcu_preempt_depth_set(int val)
{
	WRITE_ONCE(current->rcu_read_lock_nesting, val);
}

/*
 * Preemptible RCU implementation for rcu_read_lock().
 * Just increment ->rcu_read_lock_nesting, shared state will be updated
 * if we block.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_lock()에 대한 선점형 RCU 구현.
 *   증가 ->rcu_read_lock_nesting, 차단하면 공유 상태가 업데이트됩니다.
 */
void __rcu_read_lock(void)
{
	rcu_preempt_read_enter();
	if (IS_ENABLED(CONFIG_PROVE_LOCKING))
		WARN_ON_ONCE(rcu_preempt_depth() > RCU_NEST_PMAX);
	if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD) && rcu_state.gp_kthread)
		WRITE_ONCE(current->rcu_read_unlock_special.b.need_qs, true);
	barrier();  /* critical section after entry code. */
}
EXPORT_SYMBOL_GPL(__rcu_read_lock);

/*
 * Preemptible RCU implementation for rcu_read_unlock().
 * Decrement ->rcu_read_lock_nesting.  If the result is zero (outermost
 * rcu_read_unlock()) and ->rcu_read_unlock_special is non-zero, then
 * invoke rcu_read_unlock_special() to clean up after a context switch
 * in an RCU read-side critical section and other special cases.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_unlock()에 대한 선점형 RCU 구현.
 *   감소 ->rcu_read_lock_nesting. 결과가 0이고(가장 바깥쪽
 *   rcu_read_unlock()) ->rcu_read_unlock_special이 0이 아닌 경우
 *   rcu_read_unlock_special()을 호출하여 RCU 읽기 측 중요 섹션 및 기타
 *   특수한 경우에서 컨텍스트 전환 후 정리합니다.
 */
void __rcu_read_unlock(void)
{
	struct task_struct *t = current;

	barrier();  // critical section before exit code.
	if (rcu_preempt_read_exit() == 0) {
		barrier();  // critical-section exit before .s check.
		if (unlikely(READ_ONCE(t->rcu_read_unlock_special.s)))
			rcu_read_unlock_special(t);
	}
	if (IS_ENABLED(CONFIG_PROVE_LOCKING)) {
		int rrln = rcu_preempt_depth();

		WARN_ON_ONCE(rrln < 0 || rrln > RCU_NEST_PMAX);
	}
}
EXPORT_SYMBOL_GPL(__rcu_read_unlock);

/*
 * Advance a ->blkd_tasks-list pointer to the next entry, instead
 * returning NULL if at the end of the list.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   ->blkd_tasks-list 포인터를 다음 항목으로 이동하고 대신 목록 끝에
 *   있으면 NULL을 반환합니다.
 */
static struct list_head *rcu_next_node_entry(struct task_struct *t,
					     struct rcu_node *rnp)
{
	struct list_head *np;

	np = t->rcu_node_entry.next;
	if (np == &rnp->blkd_tasks)
		np = NULL;
	return np;
}

/*
 * Return true if the specified rcu_node structure has tasks that were
 * preempted within an RCU read-side critical section.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   지정된 rcu_node 구조에 RCU 읽기 측 중요 섹션 내에서 선점된 작업이 있는
 *   경우 true를 반환합니다.
 */
static bool rcu_preempt_has_tasks(struct rcu_node *rnp)
{
	return !list_empty(&rnp->blkd_tasks);
}

/*
 * Report deferred quiescent states.  The deferral time can
 * be quite short, for example, in the case of the call from
 * rcu_read_unlock_special().
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   지연된 정지 상태를 보고합니다. 지연 시간은 예를 들어
 *   rcu_read_unlock_special()에서 호출하는 경우 매우 짧을 수 있습니다.
 */
static void
rcu_preempt_deferred_qs_irqrestore(struct task_struct *t, unsigned long flags)
{
	bool empty_exp;
	bool empty_norm;
	bool empty_exp_now;
	struct list_head *np;
	bool drop_boost_mutex = false;
	struct rcu_data *rdp;
	struct rcu_node *rnp;
	union rcu_special special;

	/*
	 * If RCU core is waiting for this CPU to exit its critical section,
	 * report the fact that it has exited.  Because irqs are disabled,
	 * t->rcu_read_unlock_special cannot change.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU 코어가 이 CPU가 임계 섹션을 종료하기를 기다리고 있는 경우 종료
 *   사실을 보고합니다. irq가 비활성화되어 있으므로 t->rcu_read_unlock_special
 *   은 변경할 수 없습니다.
 */
	special = t->rcu_read_unlock_special;
	rdp = this_cpu_ptr(&rcu_data);
	if (!special.s && !rdp->exp_deferred_qs) {
		local_irq_restore(flags);
		return;
	}
	t->rcu_read_unlock_special.s = 0;
	if (special.b.need_qs) {
		if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD)) {
			rcu_report_qs_rdp(rdp);
			udelay(rcu_unlock_delay);
		} else {
			rcu_qs();
		}
	}

	/*
	 * Respond to a request by an expedited grace period for a
	 * quiescent state from this CPU.  Note that requests from
	 * tasks are handled when removing the task from the
	 * blocked-tasks list below.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이 CPU의 정지 상태에 대한 긴급 유예 기간으로 요청에 응답합니다. 아래의
 *   차단된 작업 목록에서 작업을 제거하면 작업의 요청이 처리됩니다.
 */
	if (rdp->exp_deferred_qs)
		rcu_report_exp_rdp(rdp);

	/* Clean up if blocked during RCU read-side critical section. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU 읽기 측 중요 섹션 중에 차단된 경우 정리합니다. 
 */
	if (special.b.blocked) {

		/*
		 * Remove this task from the list it blocked on.  The task
		 * now remains queued on the rcu_node corresponding to the
		 * CPU it first blocked on, so there is no longer any need
		 * to loop.  Retain a WARN_ON_ONCE() out of sheer paranoia.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   차단된 목록에서 이 작업을 제거하세요. 작업은 이제 처음 차단된 CPU에
 *   해당하는 rcu_node에서 대기열에 남아 있으므로 더 이상 루프를 돌릴 필요가
 *   없습니다. 순수한 편집증에서 WARN_ON_ONCE()를 유지하십시오.
 */
		rnp = t->rcu_blocked_node;
		raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
		WARN_ON_ONCE(rnp != t->rcu_blocked_node);
		WARN_ON_ONCE(!rcu_is_leaf_node(rnp));
		empty_norm = !rcu_preempt_blocked_readers_cgp(rnp);
		WARN_ON_ONCE(rnp->completedqs == rnp->gp_seq &&
			     (!empty_norm || rnp->qsmask));
		empty_exp = sync_rcu_exp_done(rnp);
		smp_mb(); /* ensure expedited fastpath sees end of RCU c-s. */
		np = rcu_next_node_entry(t, rnp);
		list_del_init(&t->rcu_node_entry);
		t->rcu_blocked_node = NULL;
		trace_rcu_unlock_preempted_task(TPS("rcu_preempt"),
						rnp->gp_seq, t->pid);
		if (&t->rcu_node_entry == rnp->gp_tasks)
			WRITE_ONCE(rnp->gp_tasks, np);
		if (&t->rcu_node_entry == rnp->exp_tasks)
			WRITE_ONCE(rnp->exp_tasks, np);
		if (IS_ENABLED(CONFIG_RCU_BOOST)) {
			/* Snapshot ->boost_mtx ownership w/rnp->lock held. */
			drop_boost_mutex = rt_mutex_owner(&rnp->boost_mtx.rtmutex) == t;
			if (&t->rcu_node_entry == rnp->boost_tasks)
				WRITE_ONCE(rnp->boost_tasks, np);
		}

		/*
		 * If this was the last task on the current list, and if
		 * we aren't waiting on any CPUs, report the quiescent state.
		 * Note that rcu_report_unblock_qs_rnp() releases rnp->lock,
		 * so we must take a snapshot of the expedited state.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이것이 현재 목록의 마지막 작업이고 CPU에서 대기하고 있지 않은 경우 정지
 *   상태를 보고합니다.
 *   rcu_report_unblock_qs_rnp()는 rnp->lock을 해제하므로 신속 상태의
 *   스냅샷을 찍어야 합니다.
 */
		empty_exp_now = sync_rcu_exp_done(rnp);
		if (!empty_norm && !rcu_preempt_blocked_readers_cgp(rnp)) {
			trace_rcu_quiescent_state_report(TPS("preempt_rcu"),
							 rnp->gp_seq,
							 0, rnp->qsmask,
							 rnp->level,
							 rnp->grplo,
							 rnp->grphi,
							 !!rnp->gp_tasks);
			rcu_report_unblock_qs_rnp(rnp, flags);
		} else {
			raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		}

		/* Unboost if we were boosted. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   부스트를 받았다면 부스트를 해제하십시오. 
 */
		if (IS_ENABLED(CONFIG_RCU_BOOST) && drop_boost_mutex)
			rt_mutex_futex_unlock(&rnp->boost_mtx.rtmutex);

		/*
		 * If this was the last task on the expedited lists,
		 * then we need to report up the rcu_node hierarchy.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이것이 긴급 목록의 마지막 작업인 경우 rcu_node 계층 구조를 보고해야
 *   합니다.
 */
		if (!empty_exp && empty_exp_now)
			rcu_report_exp_rnp(rnp, true);
	} else {
		local_irq_restore(flags);
	}
}

/*
 * Is a deferred quiescent-state pending, and are we also not in
 * an RCU read-side critical section?  It is the caller's responsibility
 * to ensure it is otherwise safe to report any deferred quiescent
 * states.  The reason for this is that it is safe to report a
 * quiescent state during context switch even though preemption
 * is disabled.  This function cannot be expected to understand these
 * nuances, so the caller must handle them.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   연기된 정지 상태가 보류 중이고 RCU 읽기 측 임계 섹션에도 있지 않습니까?
 *   지연된 정지 상태를 보고하는 것이 안전한지 확인하는 것은 호출자의
 *   책임입니다. 그 이유는 선점이 비활성화되어 있어도 컨텍스트 전환 중에 정지
 *   상태를 보고하는 것이 안전하기 때문입니다. 이 함수는 이러한 뉘앙스를
 *   이해할 것으로 기대할 수 없으므로 호출자가 이를 처리해야 합니다.
 */
static bool rcu_preempt_need_deferred_qs(struct task_struct *t)
{
	return (__this_cpu_read(rcu_data.exp_deferred_qs) ||
		READ_ONCE(t->rcu_read_unlock_special.s)) &&
	       rcu_preempt_depth() == 0;
}

/*
 * Report a deferred quiescent state if needed and safe to do so.
 * As with rcu_preempt_need_deferred_qs(), "safe" involves only
 * not being in an RCU read-side critical section.  The caller must
 * evaluate safety in terms of interrupt, softirq, and preemption
 * disabling.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   필요하고 안전한 경우 연기된 정지 상태를 보고합니다.
 *   rcu_preempt_need_deferred_qs()와 마찬가지로 안전에는 RCU 읽기 측 중요
 *   섹션에 있지 않은 것만 포함됩니다. 호출자는 인터럽트, softirq 및 선점
 *   비활성화 측면에서 안전성을 평가해야 합니다.
 */
static void rcu_preempt_deferred_qs(struct task_struct *t)
{
	unsigned long flags;

	if (!rcu_preempt_need_deferred_qs(t))
		return;
	local_irq_save(flags);
	rcu_preempt_deferred_qs_irqrestore(t, flags);
}

/*
 * Minimal handler to give the scheduler a chance to re-evaluate.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   스케줄러에게 재평가 기회를 제공하는 최소한의 핸들러.
 */
static void rcu_preempt_deferred_qs_handler(struct irq_work *iwp)
{
	struct rcu_data *rdp;

	rdp = container_of(iwp, struct rcu_data, defer_qs_iw);
	rdp->defer_qs_iw_pending = false;
}

/*
 * Handle special cases during rcu_read_unlock(), such as needing to
 * notify RCU core processing or task having blocked during the RCU
 * read-side critical section.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   rcu_read_unlock() 중에 RCU 코어 처리를 통지해야 하거나 RCU 읽기 측
 *   중요 섹션 동안 작업이 차단되는 것과 같은 특수한 경우를 처리합니다.
 */
static void rcu_read_unlock_special(struct task_struct *t)
{
	unsigned long flags;
	bool irqs_were_disabled;
	bool preempt_bh_were_disabled =
			!!(preempt_count() & (PREEMPT_MASK | SOFTIRQ_MASK));

	/* NMI handlers cannot block and cannot safely manipulate state. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   NMI 핸들러는 상태를 차단하거나 안전하게 조작할 수 없습니다.
 */
	if (in_nmi())
		return;

	local_irq_save(flags);
	irqs_were_disabled = irqs_disabled_flags(flags);
	if (preempt_bh_were_disabled || irqs_were_disabled) {
		bool expboost; // Expedited GP in flight or possible boosting.
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   비행 중 GP를 신속하게 처리하거나 부스트할 수 있습니다. 
 */
		struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
		struct rcu_node *rnp = rdp->mynode;

		expboost = (t->rcu_blocked_node && READ_ONCE(t->rcu_blocked_node->exp_tasks)) ||
			   (rdp->grpmask & READ_ONCE(rnp->expmask)) ||
			   IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD) ||
			   (IS_ENABLED(CONFIG_RCU_BOOST) && irqs_were_disabled &&
			    t->rcu_blocked_node);
		// Need to defer quiescent state until everything is enabled.
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   모든 것이 활성화될 때까지 정지 상태를 연기해야 합니다. 
 */
		if (use_softirq && (in_irq() || (expboost && !irqs_were_disabled))) {
			// Using softirq, safe to awaken, and either the
			// wakeup is free or there is either an expedited
			// GP in flight or a potential need to deboost.
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   softirq를 사용하여 깨우기에 안전하며 깨우기가 무료이거나 비행 중에 신속한
 *   GP가 있거나 부스트를 해제해야 할 잠재적인 필요성이 있습니다. 
 */
			raise_softirq_irqoff(RCU_SOFTIRQ);
		} else {
			// Enabling BH or preempt does reschedule, so...
			// Also if no expediting and no possible deboosting,
			// slow is OK.  Plus nohz_full CPUs eventually get
			// tick enabled.
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   BH를 활성화하거나 선점하면 일정이 변경되므로...또한 가속이 없고 부스트가
 *   가능하지 않으면 느린 것이 좋습니다. 또한 nohz_full CPU는 결국 틱이
 *   활성화됩니다. 
 */
			set_tsk_need_resched(current);
			set_preempt_need_resched();
			if (IS_ENABLED(CONFIG_IRQ_WORK) && irqs_were_disabled &&
			    expboost && !rdp->defer_qs_iw_pending && cpu_online(rdp->cpu)) {
				// get scheduler to re-evaluate and call hooks.
				// if !irq_work, fqs scan will eventually ipi.
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   스케줄러가 재평가하고 후크를 호출하도록 합니다.
 *   !irq_work인 경우 fqs 스캔은 결국 ipi가 됩니다. 
 */
				init_irq_work(&rdp->defer_qs_iw, rcu_preempt_deferred_qs_handler);
				rdp->defer_qs_iw_pending = true;
				irq_work_queue_on(&rdp->defer_qs_iw, rdp->cpu);
			}
		}
		local_irq_restore(flags);
		return;
	}
	rcu_preempt_deferred_qs_irqrestore(t, flags);
}

/*
 * Check that the list of blocked tasks for the newly completed grace
 * period is in fact empty.  It is a serious bug to complete a grace
 * period that still has RCU readers blocked!  This function must be
 * invoked -before- updating this rnp's ->gp_seq.
 *
 * Also, if there are blocked tasks on the list, they automatically
 * block the newly created grace period, so set up ->gp_tasks accordingly.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   새로 완료된 유예 기간 동안 차단된 작업 목록이 실제로 비어 있는지
 *   확인하십시오. 여전히 RCU 리더가 차단된 유예 기간을 완료하는 것은 심각한
 *   버그입니다! 이 함수는 이 rnp의 ->gp_seq를 업데이트하기 전에 호출해야
 *   합니다.
 *
 *   또한 목록에 차단된 작업이 있으면 새로 생성된 유예 기간을 자동으로
 *   차단하므로 그에 따라 ->gp_tasks를 설정합니다.
 */
static void rcu_preempt_check_blocked_tasks(struct rcu_node *rnp)
{
	struct task_struct *t;

	RCU_LOCKDEP_WARN(preemptible(), "rcu_preempt_check_blocked_tasks() invoked with preemption enabled!!!\n");
	raw_lockdep_assert_held_rcu_node(rnp);
	if (WARN_ON_ONCE(rcu_preempt_blocked_readers_cgp(rnp)))
		dump_blkd_tasks(rnp, 10);
	if (rcu_preempt_has_tasks(rnp) &&
	    (rnp->qsmaskinit || rnp->wait_blkd_tasks)) {
		WRITE_ONCE(rnp->gp_tasks, rnp->blkd_tasks.next);
		t = container_of(rnp->gp_tasks, struct task_struct,
				 rcu_node_entry);
		trace_rcu_unlock_preempted_task(TPS("rcu_preempt-GPS"),
						rnp->gp_seq, t->pid);
	}
	WARN_ON_ONCE(rnp->qsmask);
}

/*
 * Check for a quiescent state from the current CPU, including voluntary
 * context switches for Tasks RCU.  When a task blocks, the task is
 * recorded in the corresponding CPU's rcu_node structure, which is checked
 * elsewhere, hence this function need only check for quiescent states
 * related to the current CPU, not to those related to tasks.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   작업 RCU에 대한 자발적 컨텍스트 전환을 포함하여 현재 CPU에서 정지 상태를
 *   확인합니다. 작업이 차단되면 해당 CPU의 rcu_node 구조에 작업을 기록하고
 *   다른 곳에서 확인하므로 이 기능은 작업 관련이 아닌 현재 CPU와 관련된 정지
 *   상태만 확인하면 됩니다.
 */
static void rcu_flavor_sched_clock_irq(int user)
{
	struct task_struct *t = current;

	lockdep_assert_irqs_disabled();
	if (user || rcu_is_cpu_rrupt_from_idle()) {
		rcu_note_voluntary_context_switch(current);
	}
	if (rcu_preempt_depth() > 0 ||
	    (preempt_count() & (PREEMPT_MASK | SOFTIRQ_MASK))) {
		/* No QS, force context switch if deferred. */
		if (rcu_preempt_need_deferred_qs(t)) {
			set_tsk_need_resched(t);
			set_preempt_need_resched();
		}
	} else if (rcu_preempt_need_deferred_qs(t)) {
		rcu_preempt_deferred_qs(t); /* Report deferred QS. */
		return;
	} else if (!WARN_ON_ONCE(rcu_preempt_depth())) {
		rcu_qs(); /* Report immediate QS. */
		return;
	}

	/* If GP is oldish, ask for help from rcu_read_unlock_special(). */
	if (rcu_preempt_depth() > 0 &&
	    __this_cpu_read(rcu_data.core_needs_qs) &&
	    __this_cpu_read(rcu_data.cpu_no_qs.b.norm) &&
	    !t->rcu_read_unlock_special.b.need_qs &&
	    time_after(jiffies, rcu_state.gp_start + HZ))
		t->rcu_read_unlock_special.b.need_qs = true;
}

/*
 * Check for a task exiting while in a preemptible-RCU read-side
 * critical section, clean up if so.  No need to issue warnings, as
 * debug_check_no_locks_held() already does this if lockdep is enabled.
 * Besides, if this function does anything other than just immediately
 * return, there was a bug of some sort.  Spewing warnings from this
 * function is like as not to simply obscure important prior warnings.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   선점형 RCU 읽기 측 중요 섹션에 있는 동안 작업이 종료되는지 확인하고
 *   있으면 정리합니다. lockdep이 활성화된 경우 debug_check_no_locks_held()가
 *   이미 경고를 발행하므로 경고를 발행할 필요가 없습니다.
 *   게다가 이 함수가 즉시 반환하는 것 외에 다른 작업을 수행하면 일종의
 *   버그가 있는 것입니다. 이 기능에서 경고를 분출하는 것은 단순히 중요한
 *   이전 경고를 가리지 않는 것과 같습니다.
 */
void exit_rcu(void)
{
	struct task_struct *t = current;

	if (unlikely(!list_empty(&current->rcu_node_entry))) {
		rcu_preempt_depth_set(1);
		barrier();
		WRITE_ONCE(t->rcu_read_unlock_special.b.blocked, true);
	} else if (unlikely(rcu_preempt_depth())) {
		rcu_preempt_depth_set(1);
	} else {
		return;
	}
	__rcu_read_unlock();
	rcu_preempt_deferred_qs(current);
}

/*
 * Dump the blocked-tasks state, but limit the list dump to the
 * specified number of elements.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   차단된 작업 상태를 덤프하지만 목록 덤프를 지정된 요소 수로 제한합니다.
 */
static void
dump_blkd_tasks(struct rcu_node *rnp, int ncheck)
{
	int cpu;
	int i;
	struct list_head *lhp;
	bool onl;
	struct rcu_data *rdp;
	struct rcu_node *rnp1;

	raw_lockdep_assert_held_rcu_node(rnp);
	pr_info("%s: grp: %d-%d level: %d ->gp_seq %ld ->completedqs %ld\n",
		__func__, rnp->grplo, rnp->grphi, rnp->level,
		(long)READ_ONCE(rnp->gp_seq), (long)rnp->completedqs);
	for (rnp1 = rnp; rnp1; rnp1 = rnp1->parent)
		pr_info("%s: %d:%d ->qsmask %#lx ->qsmaskinit %#lx ->qsmaskinitnext %#lx\n",
			__func__, rnp1->grplo, rnp1->grphi, rnp1->qsmask, rnp1->qsmaskinit, rnp1->qsmaskinitnext);
	pr_info("%s: ->gp_tasks %p ->boost_tasks %p ->exp_tasks %p\n",
		__func__, READ_ONCE(rnp->gp_tasks), data_race(rnp->boost_tasks),
		READ_ONCE(rnp->exp_tasks));
	pr_info("%s: ->blkd_tasks", __func__);
	i = 0;
	list_for_each(lhp, &rnp->blkd_tasks) {
		pr_cont(" %p", lhp);
		if (++i >= ncheck)
			break;
	}
	pr_cont("\n");
	for (cpu = rnp->grplo; cpu <= rnp->grphi; cpu++) {
		rdp = per_cpu_ptr(&rcu_data, cpu);
		onl = !!(rdp->grpmask & rcu_rnp_online_cpus(rnp));
		pr_info("\t%d: %c online: %ld(%d) offline: %ld(%d)\n",
			cpu, ".o"[onl],
			(long)rdp->rcu_onl_gp_seq, rdp->rcu_onl_gp_flags,
			(long)rdp->rcu_ofl_gp_seq, rdp->rcu_ofl_gp_flags);
	}
}

#else /* #ifdef CONFIG_PREEMPT_RCU */

/*
 * If strict grace periods are enabled, and if the calling
 * __rcu_read_unlock() marks the beginning of a quiescent state, immediately
 * report that quiescent state and, if requested, spin for a bit.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   엄격한 유예 기간이 활성화되고 __rcu_read_unlock() 호출이 정지 상태의
 *   시작을 표시하는 경우 즉시 해당 정지 상태를 보고하고 요청이 있으면 잠시
 *   회전합니다.
 */
void rcu_read_unlock_strict(void)
{
	struct rcu_data *rdp;

	if (!IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD) ||
	   irqs_disabled() || preempt_count() || !rcu_state.gp_kthread)
		return;
	rdp = this_cpu_ptr(&rcu_data);
	rcu_report_qs_rdp(rdp);
	udelay(rcu_unlock_delay);
}
EXPORT_SYMBOL_GPL(rcu_read_unlock_strict);

/*
 * Tell them what RCU they are running.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   실행 중인 RCU를 알려주십시오. 
 */
static void __init rcu_bootup_announce(void)
{
	pr_info("Hierarchical RCU implementation.\n");
	rcu_bootup_announce_oddness();
}

/*
 * Note a quiescent state for PREEMPTION=n.  Because we do not need to know
 * how many quiescent states passed, just if there was at least one since
 * the start of the grace period, this just sets a flag.  The caller must
 * have disabled preemption.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   PREEMPTION=n에 대한 정지 상태에 유의하십시오. 얼마나 많은 정지 상태가
 *   통과했는지 알 필요가 없기 때문에 유예 기간이 시작된 이후에 적어도 하나가
 *   있었다면 플래그를 설정할 뿐입니다. 호출자는 선점을 비활성화해야 합니다.
 */
static void rcu_qs(void)
{
	RCU_LOCKDEP_WARN(preemptible(), "rcu_qs() invoked with preemption enabled!!!");
	if (!__this_cpu_read(rcu_data.cpu_no_qs.s))
		return;
	trace_rcu_grace_period(TPS("rcu_sched"),
			       __this_cpu_read(rcu_data.gp_seq), TPS("cpuqs"));
	__this_cpu_write(rcu_data.cpu_no_qs.b.norm, false);
	if (!__this_cpu_read(rcu_data.cpu_no_qs.b.exp))
		return;
	__this_cpu_write(rcu_data.cpu_no_qs.b.exp, false);
	rcu_report_exp_rdp(this_cpu_ptr(&rcu_data));
}

/*
 * Register an urgently needed quiescent state.  If there is an
 * emergency, invoke rcu_momentary_dyntick_idle() to do a heavy-weight
 * dyntick-idle quiescent state visible to other CPUs, which will in
 * some cases serve for expedited as well as normal grace periods.
 * Either way, register a lightweight quiescent state.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   긴급하게 필요한 정지 상태를 등록합니다. 비상 상황이 발생하면
 *   rcu_momentary_dyntick_idle()을 호출하여 다른 CPU에서 볼 수 있는 강력한
 *   dyntick-idle 정지 상태를 수행합니다. 이는 경우에 따라 일반 유예 기간뿐만
 *   아니라 긴급 유예 기간에도 사용됩니다.
 *   어느 쪽이든 가벼운 정지 상태를 등록하십시오.
 */
void rcu_all_qs(void)
{
	unsigned long flags;

	if (!raw_cpu_read(rcu_data.rcu_urgent_qs))
		return;
	preempt_disable();
	/* Load rcu_urgent_qs before other flags. */
	if (!smp_load_acquire(this_cpu_ptr(&rcu_data.rcu_urgent_qs))) {
		preempt_enable();
		return;
	}
	this_cpu_write(rcu_data.rcu_urgent_qs, false);
	if (unlikely(raw_cpu_read(rcu_data.rcu_need_heavy_qs))) {
		local_irq_save(flags);
		rcu_momentary_dyntick_idle();
		local_irq_restore(flags);
	}
	rcu_qs();
	preempt_enable();
}
EXPORT_SYMBOL_GPL(rcu_all_qs);

/*
 * Note a PREEMPTION=n context switch. The caller must have disabled interrupts.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   PREEMPTION=n 컨텍스트 스위치에 유의하십시오. 호출자는 인터럽트를
 *   비활성화해야 합니다. 
 */
void rcu_note_context_switch(bool preempt)
{
	trace_rcu_utilization(TPS("Start context switch"));
	rcu_qs();
	/* Load rcu_urgent_qs before other flags. */
	if (!smp_load_acquire(this_cpu_ptr(&rcu_data.rcu_urgent_qs)))
		goto out;
	this_cpu_write(rcu_data.rcu_urgent_qs, false);
	if (unlikely(raw_cpu_read(rcu_data.rcu_need_heavy_qs)))
		rcu_momentary_dyntick_idle();
	rcu_tasks_qs(current, preempt);
out:
	trace_rcu_utilization(TPS("End context switch"));
}
EXPORT_SYMBOL_GPL(rcu_note_context_switch);

/*
 * Because preemptible RCU does not exist, there are never any preempted
 * RCU readers.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   선점형 RCU가 존재하지 않기 때문에 선점된 RCU 리더가 없습니다.
 */
static int rcu_preempt_blocked_readers_cgp(struct rcu_node *rnp)
{
	return 0;
}

/*
 * Because there is no preemptible RCU, there can be no readers blocked.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   선점형 RCU가 없기 때문에 차단된 판독기가 있을 수 없습니다.
 */
static bool rcu_preempt_has_tasks(struct rcu_node *rnp)
{
	return false;
}

/*
 * Because there is no preemptible RCU, there can be no deferred quiescent
 * states.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   선점형 RCU가 없기 때문에 지연된 정지 상태가 있을 수 없습니다.
 */
static bool rcu_preempt_need_deferred_qs(struct task_struct *t)
{
	return false;
}
static void rcu_preempt_deferred_qs(struct task_struct *t) { }

/*
 * Because there is no preemptible RCU, there can be no readers blocked,
 * so there is no need to check for blocked tasks.  So check only for
 * bogus qsmask values.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   선점형 RCU가 없기 때문에 차단된 리더가 없을 수 있으므로 차단된 작업을
 *   확인할 필요가 없습니다. 따라서 가짜 qsmask 값만 확인하십시오.
 */
static void rcu_preempt_check_blocked_tasks(struct rcu_node *rnp)
{
	WARN_ON_ONCE(rnp->qsmask);
}

/*
 * Check to see if this CPU is in a non-context-switch quiescent state,
 * namely user mode and idle loop.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이 CPU가 non-context-switch 정지 상태, 즉 사용자 모드 및 유휴 루프에
 *   있는지 확인하십시오.
 */
static void rcu_flavor_sched_clock_irq(int user)
{
	if (user || rcu_is_cpu_rrupt_from_idle()) {

		/*
		 * Get here if this CPU took its interrupt from user
		 * mode or from the idle loop, and if this is not a
		 * nested interrupt.  In this case, the CPU is in
		 * a quiescent state, so note it.
		 *
		 * No memory barrier is required here because rcu_qs()
		 * references only CPU-local variables that other CPUs
		 * neither access nor modify, at least not while the
		 * corresponding CPU is online.
		 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이 CPU가 사용자 모드 또는 유휴 루프에서 인터럽트를 받았고 이것이 중첩
 *   인터럽트가 아닌 경우 여기로 이동하십시오. 이 경우 CPU는 정지 상태이므로
 *   주의하십시오.
 *
 *   rcu_qs()는 적어도 해당 CPU가 온라인 상태인 동안에는 다른 CPU가
 *   액세스하거나 수정하지 않는 CPU 로컬 변수만 참조하기 때문에 여기에는
 *   메모리 장벽이 필요하지 않습니다.
 */
		rcu_qs();
	}
}

/*
 * Because preemptible RCU does not exist, tasks cannot possibly exit
 * while in preemptible RCU read-side critical sections.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   선점형 RCU가 존재하지 않기 때문에 선점형 RCU 읽기 측 임계 섹션에 있는
 *   동안 작업이 종료될 가능성이 없습니다.
 */
void exit_rcu(void)
{
}

/*
 * Dump the guaranteed-empty blocked-tasks state.  Trust but verify.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   보장된 비어 있는 차단된 작업 상태를 덤프합니다. 신뢰하지만 확인하십시오. 
 */
static void
dump_blkd_tasks(struct rcu_node *rnp, int ncheck)
{
	WARN_ON_ONCE(!list_empty(&rnp->blkd_tasks));
}

#endif /* #else #ifdef CONFIG_PREEMPT_RCU */

/*
 * If boosting, set rcuc kthreads to realtime priority.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   부스팅하는 경우 rcuc kthreads를 실시간 우선순위로 설정하십시오. 
 */
static void rcu_cpu_kthread_setup(unsigned int cpu)
{
#ifdef CONFIG_RCU_BOOST
	struct sched_param sp;

	sp.sched_priority = kthread_prio;
	sched_setscheduler_nocheck(current, SCHED_FIFO, &sp);
#endif /* #ifdef CONFIG_RCU_BOOST */
}

#ifdef CONFIG_RCU_BOOST

/*
 * Carry out RCU priority boosting on the task indicated by ->exp_tasks
 * or ->boost_tasks, advancing the pointer to the next task in the
 * ->blkd_tasks list.
 *
 * Note that irqs must be enabled: boosting the task can block.
 * Returns 1 if there are more tasks needing to be boosted.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   ->exp_tasks 또는 ->boost_tasks로 표시된 작업에서 RCU 우선 순위 부스팅을
 *   수행하여 포인터를 ->blkd_tasks 목록의 다음 작업으로 이동시킵니다.
 *
 *   irqs를 활성화해야 합니다.
 *   작업을 부스트하면 차단할 수 있습니다.
 *   부스트해야 할 작업이 더 있으면 1을 반환합니다.
 */
static int rcu_boost(struct rcu_node *rnp)
{
	unsigned long flags;
	struct task_struct *t;
	struct list_head *tb;

	if (READ_ONCE(rnp->exp_tasks) == NULL &&
	    READ_ONCE(rnp->boost_tasks) == NULL)
		return 0;  /* Nothing left to boost. */

	raw_spin_lock_irqsave_rcu_node(rnp, flags);

	/*
	 * Recheck under the lock: all tasks in need of boosting
	 * might exit their RCU read-side critical sections on their own.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   잠금 상태에서 다시 확인: 부스트가 필요한 모든 작업은 자체적으로 RCU 읽기
 *   측 중요 섹션을 종료할 수 있습니다.
 */
	if (rnp->exp_tasks == NULL && rnp->boost_tasks == NULL) {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		return 0;
	}

	/*
	 * Preferentially boost tasks blocking expedited grace periods.
	 * This cannot starve the normal grace periods because a second
	 * expedited grace period must boost all blocked tasks, including
	 * those blocking the pre-existing normal grace period.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   긴급 유예 기간을 차단하는 작업을 우선적으로 부스트합니다.
 *   두 번째 긴급 유예 기간은 기존의 일반 유예 기간을 차단하는 작업을
 *   포함하여 차단된 모든 작업을 강화해야 하므로 일반 유예 기간을 중단할 수
 *   없습니다.
 */
	if (rnp->exp_tasks != NULL)
		tb = rnp->exp_tasks;
	else
		tb = rnp->boost_tasks;

	/*
	 * We boost task t by manufacturing an rt_mutex that appears to
	 * be held by task t.  We leave a pointer to that rt_mutex where
	 * task t can find it, and task t will release the mutex when it
	 * exits its outermost RCU read-side critical section.  Then
	 * simply acquiring this artificial rt_mutex will boost task
	 * t's priority.  (Thanks to tglx for suggesting this approach!)
	 *
	 * Note that task t must acquire rnp->lock to remove itself from
	 * the ->blkd_tasks list, which it will do from exit() if from
	 * nowhere else.  We therefore are guaranteed that task t will
	 * stay around at least until we drop rnp->lock.  Note that
	 * rnp->lock also resolves races between our priority boosting
	 * and task t's exiting its outermost RCU read-side critical
	 * section.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   우리는 작업 t에 의해 유지되는 것처럼 보이는 rt_mutex를 제조하여 작업 t를
 *   부스트합니다. 태스크 t가 찾을 수 있는 rt_mutex에 대한 포인터를 남겨두고
 *   태스크 t는 가장 바깥쪽 RCU 읽기 측 임계 섹션을 종료할 때 뮤텍스를
 *   해제합니다. 그런 다음 단순히 이 인공 rt_mutex를 획득하면 작업 t의 우선
 *   순위가 높아집니다. (이 접근 방식을 제안한 tglx에게 감사드립니다!) 작업
 *   t는 ->blkd_tasks 목록에서 자신을 제거하기 위해 rnp->lock을 획득해야
 *   합니다. 다른 곳에서 수행하지 않는 경우 exit()에서 수행합니다. 따라서
 *   우리는 최소한 rnp->lock을 해제할 때까지 태스크 t가 유지될 것이라고
 *   보장합니다. rnp->lock은 또한 우선 순위 부스팅과 작업 t가 가장 바깥쪽
 *   RCU 읽기 측 임계 섹션을 종료하는 사이의 경합을 해결합니다.
 */
	t = container_of(tb, struct task_struct, rcu_node_entry);
	rt_mutex_init_proxy_locked(&rnp->boost_mtx.rtmutex, t);
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	/* Lock only for side effect: boosts task t's priority. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   부작용에 대해서만 잠금: 작업 t의 우선 순위를 높입니다.
 */
	rt_mutex_lock(&rnp->boost_mtx);
	rt_mutex_unlock(&rnp->boost_mtx);  /* Then keep lockdep happy. */
	rnp->n_boosts++;

	return READ_ONCE(rnp->exp_tasks) != NULL ||
	       READ_ONCE(rnp->boost_tasks) != NULL;
}

/*
 * Priority-boosting kthread, one per leaf rcu_node.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   리프 rcu_node당 하나의 우선순위 부스팅 kthread.
 */
static int rcu_boost_kthread(void *arg)
{
	struct rcu_node *rnp = (struct rcu_node *)arg;
	int spincnt = 0;
	int more2boost;

	trace_rcu_utilization(TPS("Start boost kthread@init"));
	for (;;) {
		WRITE_ONCE(rnp->boost_kthread_status, RCU_KTHREAD_WAITING);
		trace_rcu_utilization(TPS("End boost kthread@rcu_wait"));
		rcu_wait(READ_ONCE(rnp->boost_tasks) ||
			 READ_ONCE(rnp->exp_tasks));
		trace_rcu_utilization(TPS("Start boost kthread@rcu_wait"));
		WRITE_ONCE(rnp->boost_kthread_status, RCU_KTHREAD_RUNNING);
		more2boost = rcu_boost(rnp);
		if (more2boost)
			spincnt++;
		else
			spincnt = 0;
		if (spincnt > 10) {
			WRITE_ONCE(rnp->boost_kthread_status, RCU_KTHREAD_YIELDING);
			trace_rcu_utilization(TPS("End boost kthread@rcu_yield"));
			schedule_timeout_idle(2);
			trace_rcu_utilization(TPS("Start boost kthread@rcu_yield"));
			spincnt = 0;
		}
	}
	/* NOTREACHED */
	trace_rcu_utilization(TPS("End boost kthread@notreached"));
	return 0;
}

/*
 * Check to see if it is time to start boosting RCU readers that are
 * blocking the current grace period, and, if so, tell the per-rcu_node
 * kthread to start boosting them.  If there is an expedited grace
 * period in progress, it is always time to boost.
 *
 * The caller must hold rnp->lock, which this function releases.
 * The ->boost_kthread_task is immortal, so we don't need to worry
 * about it going away.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   현재 유예 기간을 차단하고 있는 RCU 판독기 부스팅을 시작할 시간인지
 *   확인하고, 그렇다면 per-rcu_node kthread에 부스팅을 시작하라고 알립니다.
 *   진행 중인 긴급 유예 기간이 있는 경우 항상 부스트할 시간입니다.
 *
 *   호출자는 이 함수가 해제하는 rnp->lock을 보유해야 합니다.
 *   ->boost_kthread_task는 불멸이므로 사라지는 것에 대해 걱정할 필요가
 *   없습니다.
 */
static void rcu_initiate_boost(struct rcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	raw_lockdep_assert_held_rcu_node(rnp);
	if (!rcu_preempt_blocked_readers_cgp(rnp) && rnp->exp_tasks == NULL) {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		return;
	}
	if (rnp->exp_tasks != NULL ||
	    (rnp->gp_tasks != NULL &&
	     rnp->boost_tasks == NULL &&
	     rnp->qsmask == 0 &&
	     (!time_after(rnp->boost_time, jiffies) || rcu_state.cbovld))) {
		if (rnp->exp_tasks == NULL)
			WRITE_ONCE(rnp->boost_tasks, rnp->gp_tasks);
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		rcu_wake_cond(rnp->boost_kthread_task,
			      READ_ONCE(rnp->boost_kthread_status));
	} else {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	}
}

/*
 * Is the current CPU running the RCU-callbacks kthread?
 * Caller must have preemption disabled.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   현재 CPU가 RCU 콜백 kthread를 실행하고 있습니까? 발신자는 선점을
 *   비활성화해야 합니다.
. 
 */
static bool rcu_is_callbacks_kthread(void)
{
	return __this_cpu_read(rcu_data.rcu_cpu_kthread_task) == current;
}

#define RCU_BOOST_DELAY_JIFFIES DIV_ROUND_UP(CONFIG_RCU_BOOST_DELAY * HZ, 1000)

/*
 * Do priority-boost accounting for the start of a new grace period.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   새로운 유예 기간의 시작을 위해 우선 순위 부스트 계정을 수행하십시오.
 */
static void rcu_preempt_boost_start_gp(struct rcu_node *rnp)
{
	rnp->boost_time = jiffies + RCU_BOOST_DELAY_JIFFIES;
}

/*
 * Create an RCU-boost kthread for the specified node if one does not
 * already exist.  We only create this kthread for preemptible RCU.
 * Returns zero if all is well, a negated errno otherwise.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   아직 존재하지 않는 경우 지정된 노드에 대한 RCU 부스트 kthread를
 *   생성합니다. 선점형 RCU에 대해서만 이 kthread를 생성합니다.
 *   모든 것이 정상이면 0을 반환하고 그렇지 않으면 부정된 errno를 반환합니다.
 */
static void rcu_spawn_one_boost_kthread(struct rcu_node *rnp)
{
	unsigned long flags;
	int rnp_index = rnp - rcu_get_root();
	struct sched_param sp;
	struct task_struct *t;

	if (rnp->boost_kthread_task || !rcu_scheduler_fully_active)
		return;

	rcu_state.boost = 1;

	t = kthread_create(rcu_boost_kthread, (void *)rnp,
			   "rcub/%d", rnp_index);
	if (WARN_ON_ONCE(IS_ERR(t)))
		return;

	raw_spin_lock_irqsave_rcu_node(rnp, flags);
	rnp->boost_kthread_task = t;
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	sp.sched_priority = kthread_prio;
	sched_setscheduler_nocheck(t, SCHED_FIFO, &sp);
	wake_up_process(t); /* get to TASK_INTERRUPTIBLE quickly. */
}

/*
 * Set the per-rcu_node kthread's affinity to cover all CPUs that are
 * served by the rcu_node in question.  The CPU hotplug lock is still
 * held, so the value of rnp->qsmaskinit will be stable.
 *
 * We don't include outgoingcpu in the affinity set, use -1 if there is
 * no outgoing CPU.  If there are no CPUs left in the affinity set,
 * this function allows the kthread to execute on any CPU.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   해당 rcu_node에서 제공하는 모든 CPU를 포함하도록 per-rcu_node kthread의
 *   선호도를 설정하십시오. CPU 핫플러그 잠금이 여전히 유지되므로
 *   rnp->qsmaskinit 값이 안정적입니다.
 *
 *   선호도 세트에 outgoingcpu를 포함하지 않습니다. 나가는 CPU가 없으면 -1을
 *   사용합니다. 선호도 세트에 남아 있는 CPU가 없는 경우 이 함수를 사용하면
 *   kthread가 모든 CPU에서 실행될 수 있습니다.
 */
static void rcu_boost_kthread_setaffinity(struct rcu_node *rnp, int outgoingcpu)
{
	struct task_struct *t = rnp->boost_kthread_task;
	unsigned long mask = rcu_rnp_online_cpus(rnp);
	cpumask_var_t cm;
	int cpu;

	if (!t)
		return;
	if (!zalloc_cpumask_var(&cm, GFP_KERNEL))
		return;
	for_each_leaf_node_possible_cpu(rnp, cpu)
		if ((mask & leaf_node_cpu_bit(rnp, cpu)) &&
		    cpu != outgoingcpu)
			cpumask_set_cpu(cpu, cm);
	if (cpumask_weight(cm) == 0)
		cpumask_setall(cm);
	set_cpus_allowed_ptr(t, cm);
	free_cpumask_var(cm);
}

/*
 * Spawn boost kthreads -- called as soon as the scheduler is running.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   스폰 부스트 kthreads -- 스케줄러가 실행되는 즉시 호출됩니다.
 */
static void __init rcu_spawn_boost_kthreads(void)
{
	struct rcu_node *rnp;

	rcu_for_each_leaf_node(rnp)
		if (rcu_rnp_online_cpus(rnp))
			rcu_spawn_one_boost_kthread(rnp);
}

#else /* #ifdef CONFIG_RCU_BOOST */

static void rcu_initiate_boost(struct rcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
}

static bool rcu_is_callbacks_kthread(void)
{
	return false;
}

static void rcu_preempt_boost_start_gp(struct rcu_node *rnp)
{
}

static void rcu_spawn_one_boost_kthread(struct rcu_node *rnp)
{
}

static void rcu_boost_kthread_setaffinity(struct rcu_node *rnp, int outgoingcpu)
{
}

static void __init rcu_spawn_boost_kthreads(void)
{
}

#endif /* #else #ifdef CONFIG_RCU_BOOST */

#if !defined(CONFIG_RCU_FAST_NO_HZ)

/*
 * Check to see if any future non-offloaded RCU-related work will need
 * to be done by the current CPU, even if none need be done immediately,
 * returning 1 if so.  This function is part of the RCU implementation;
 * it is -not- an exported member of the RCU API.
 *
 * Because we not have RCU_FAST_NO_HZ, just check whether or not this
 * CPU has RCU callbacks queued.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   향후 오프로드되지 않은 RCU 관련 작업을 현재 CPU에서 수행해야 하는지
 *   확인하고, 즉시 수행할 필요가 없는 경우에도 1을 반환합니다. 이 기능은 RCU
 *   구현의 일부입니다. RCU API의 내보낸 구성원이 -아닙니다-.
 *
 *   RCU_FAST_NO_HZ가 없기 때문에 이 CPU에 대기 중인 RCU 콜백이 있는지
 *   확인하십시오.
 */
int rcu_needs_cpu(u64 basemono, u64 *nextevt)
{
	*nextevt = KTIME_MAX;
	return !rcu_segcblist_empty(&this_cpu_ptr(&rcu_data)->cblist) &&
		!rcu_rdp_is_offloaded(this_cpu_ptr(&rcu_data));
}

/*
 * Because we do not have RCU_FAST_NO_HZ, don't bother cleaning up
 * after it.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   우리는 RCU_FAST_NO_HZ가 없기 때문에 그 이후에 귀찮게 정리하지 마십시오.
 */
static void rcu_cleanup_after_idle(void)
{
}

/*
 * Do the idle-entry grace-period work, which, because CONFIG_RCU_FAST_NO_HZ=n,
 * is nothing.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   CONFIG_RCU_FAST_NO_HZ=n이므로 아무것도 아닌 유휴 진입 유예 기간 작업을
 *   수행합니다.
 */
static void rcu_prepare_for_idle(void)
{
}

#else /* #if !defined(CONFIG_RCU_FAST_NO_HZ) */

/*
 * This code is invoked when a CPU goes idle, at which point we want
 * to have the CPU do everything required for RCU so that it can enter
 * the energy-efficient dyntick-idle mode.
 *
 * The following preprocessor symbol controls this:
 *
 * RCU_IDLE_GP_DELAY gives the number of jiffies that a CPU is permitted
 *	to sleep in dyntick-idle mode with RCU callbacks pending.  This
 *	is sized to be roughly one RCU grace period.  Those energy-efficiency
 *	benchmarkers who might otherwise be tempted to set this to a large
 *	number, be warned: Setting RCU_IDLE_GP_DELAY too high can hang your
 *	system.  And if you are -that- concerned about energy efficiency,
 *	just power the system down and be done with it!
 *
 * The value below works well in practice.  If future workloads require
 * adjustment, they can be converted into kernel config parameters, though
 * making the state machine smarter might be a better option.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이 코드는 CPU가 유휴 상태가 될 때 호출되며, 이 시점에서 CPU가 에너지
 *   효율적인 dyntick-idle 모드로 들어갈 수 있도록 RCU에 필요한 모든 작업을
 *   수행하도록 합니다.
 *
 *   다음 전처리기 기호가 이를 제어합니다.
 *
 *   RCU_IDLE_GP_DELAY는 보류 중인 RCU 콜백과 함께 CPU가 dyntick-idle
 *   모드에서 휴면하도록 허용되는 jiffies 수를 제공합니다. 이 크기는 대략
 *   하나의 RCU 유예 기간입니다. 그렇지 않으면 이것을 큰 숫자로 설정하려는
 *   유혹을 받을 수 있는 에너지 효율성 벤치마커는 다음과 같이 경고합니다.
 *   RCU_IDLE_GP_DELAY를 너무 높게 설정하면 시스템이 중단될 수 있습니다.
 *   그리고 에너지 효율성이 걱정된다면 시스템의 전원을 끄고 그대로 사용하면
 *   됩니다!
 *
 *   아래 값은 실제로 잘 작동합니다. 향후 워크로드에 조정이 필요한 경우 커널
 *   구성 매개변수로 변환할 수 있지만 상태 시스템을 더 스마트하게 만드는 것이
 *   더 나은 옵션일 수 있습니다.
 */
#define RCU_IDLE_GP_DELAY 4		/* Roughly one grace period. */

static int rcu_idle_gp_delay = RCU_IDLE_GP_DELAY;
module_param(rcu_idle_gp_delay, int, 0644);

/*
 * Try to advance callbacks on the current CPU, but only if it has been
 * awhile since the last time we did so.  Afterwards, if there are any
 * callbacks ready for immediate invocation, return true.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   현재 CPU에서 콜백을 진행하려고 시도하지만 마지막으로 수행한 이후로
 *   시간이 지난 경우에만 가능합니다. 그 후 즉시 호출할 수 있는 콜백이 있으면
 *   true를 반환합니다.
 */
static bool __maybe_unused rcu_try_advance_all_cbs(void)
{
	bool cbs_ready = false;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp;

	/* Exit early if we advanced recently. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   최근에 진행했다면 일찍 종료하십시오.
 */
	if (jiffies == rdp->last_advance_all)
		return false;
	rdp->last_advance_all = jiffies;

	rnp = rdp->mynode;

	/*
	 * Don't bother checking unless a grace period has
	 * completed since we last checked and there are
	 * callbacks not yet ready to invoke.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   마지막으로 확인한 후 유예 기간이 완료되었고 아직 호출할 준비가 되지 않은
 *   콜백이 있는 경우가 아니면 확인하지 마십시오.
 */
	if ((rcu_seq_completed_gp(rdp->gp_seq,
				  rcu_seq_current(&rnp->gp_seq)) ||
	     unlikely(READ_ONCE(rdp->gpwrap))) &&
	    rcu_segcblist_pend_cbs(&rdp->cblist))
		note_gp_changes(rdp);

	if (rcu_segcblist_ready_cbs(&rdp->cblist))
		cbs_ready = true;
	return cbs_ready;
}

/*
 * Allow the CPU to enter dyntick-idle mode unless it has callbacks ready
 * to invoke.  If the CPU has callbacks, try to advance them.  Tell the
 * caller about what to set the timeout.
 *
 * The caller must have disabled interrupts.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   호출 준비가 된 콜백이 없는 한 CPU가 dyntick-idle 모드로 들어가도록
 *   허용합니다. CPU에 콜백이 있는 경우 이를 진행하십시오. 발신자에게 제한
 *   시간을 설정할 항목을 알려줍니다.
 *
 *   호출자는 인터럽트를 비활성화해야 합니다.
 */
int rcu_needs_cpu(u64 basemono, u64 *nextevt)
{
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	unsigned long dj;

	lockdep_assert_irqs_disabled();

	/* If no non-offloaded callbacks, RCU doesn't need the CPU. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   오프로드되지 않은 콜백이 없으면 RCU에 CPU가 필요하지 않습니다.
 */
	if (rcu_segcblist_empty(&rdp->cblist) ||
	    rcu_rdp_is_offloaded(rdp)) {
		*nextevt = KTIME_MAX;
		return 0;
	}

	/* Attempt to advance callbacks. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   콜백 진행을 시도합니다. 
 */
	if (rcu_try_advance_all_cbs()) {
		/* Some ready to invoke, so initiate later invocation. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   일부는 호출할 준비가 되었으므로 나중에 호출을 시작합니다.
 */
		invoke_rcu_core();
		return 1;
	}
	rdp->last_accelerate = jiffies;

	/* Request timer and round. */
	dj = round_up(rcu_idle_gp_delay + jiffies, rcu_idle_gp_delay) - jiffies;

	*nextevt = basemono + dj * TICK_NSEC;
	return 0;
}

/*
 * Prepare a CPU for idle from an RCU perspective.  The first major task is to
 * sense whether nohz mode has been enabled or disabled via sysfs.  The second
 * major task is to accelerate (that is, assign grace-period numbers to) any
 * recently arrived callbacks.
 *
 * The caller must have disabled interrupts.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU 관점에서 CPU를 유휴 상태로 준비합니다. 첫 번째 주요 작업은 sysfs를
 *   통해 nohz 모드가 활성화되었는지 비활성화되었는지 감지하는 것입니다.
 *   두 번째 주요 작업은 최근에 도착한 콜백을 가속화(즉, 유예 기간 번호
 *   할당)하는 것입니다.
 *
 *   호출자는 인터럽트를 비활성화해야 합니다.
 */
static void rcu_prepare_for_idle(void)
{
	bool needwake;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp;
	int tne;

	lockdep_assert_irqs_disabled();
	if (rcu_rdp_is_offloaded(rdp))
		return;

	/* Handle nohz enablement switches conservatively. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   nohz 활성화 스위치를 보수적으로 처리합니다. 
 */
	tne = READ_ONCE(tick_nohz_active);
	if (tne != rdp->tick_nohz_enabled_snap) {
		if (!rcu_segcblist_empty(&rdp->cblist))
			invoke_rcu_core(); /* force nohz to see update. */
		rdp->tick_nohz_enabled_snap = tne;
		return;
	}
	if (!tne)
		return;

	/*
	 * If we have not yet accelerated this jiffy, accelerate all
	 * callbacks on this CPU.
	 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   아직 이 jiffy를 가속하지 않은 경우 이 CPU의 모든 콜백을 가속합니다.
 */
	if (rdp->last_accelerate == jiffies)
		return;
	rdp->last_accelerate = jiffies;
	if (rcu_segcblist_pend_cbs(&rdp->cblist)) {
		rnp = rdp->mynode;
		raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
		needwake = rcu_accelerate_cbs(rnp, rdp);
		raw_spin_unlock_rcu_node(rnp); /* irqs remain disabled. */
		if (needwake)
			rcu_gp_kthread_wake();
	}
}

/*
 * Clean up for exit from idle.  Attempt to advance callbacks based on
 * any grace periods that elapsed while the CPU was idle, and if any
 * callbacks are now ready to invoke, initiate invocation.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   유휴 종료를 위해 정리하십시오. CPU가 유휴 상태인 동안 경과한 유예 기간을
 *   기반으로 콜백을 진행하려고 시도하고 이제 콜백을 호출할 준비가 된 경우
 *   호출을 시작합니다.
 */
static void rcu_cleanup_after_idle(void)
{
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);

	lockdep_assert_irqs_disabled();
	if (rcu_rdp_is_offloaded(rdp))
		return;
	if (rcu_try_advance_all_cbs())
		invoke_rcu_core();
}

#endif /* #else #if !defined(CONFIG_RCU_FAST_NO_HZ) */

/*
 * Is this CPU a NO_HZ_FULL CPU that should ignore RCU so that the
 * grace-period kthread will do force_quiescent_state() processing?
 * The idea is to avoid waking up RCU core processing on such a
 * CPU unless the grace period has extended for too long.
 *
 * This code relies on the fact that all NO_HZ_FULL CPUs are also
 * CONFIG_RCU_NOCB_CPU CPUs.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   이 CPU는 유예 기간 kthread가 force_quiescent_state() 처리를 수행하도록
 *   RCU를 무시해야 하는 NO_HZ_FULL CPU입니까? 아이디어는 유예 기간이 너무
 *   오래 연장되지 않는 한 이러한 CPU에서 RCU 코어 처리를 깨우지 않도록 하는
 *   것입니다.
 *
 *   이 코드는 모든 NO_HZ_FULL CPU가 CONFIG_RCU_NOCB_CPU CPU이기도 한다는
 *   사실에 의존합니다.
 */
static bool rcu_nohz_full_cpu(void)
{
#ifdef CONFIG_NO_HZ_FULL
	if (tick_nohz_full_cpu(smp_processor_id()) &&
	    (!rcu_gp_in_progress() ||
	     time_before(jiffies, READ_ONCE(rcu_state.gp_start) + HZ)))
		return true;
#endif /* #ifdef CONFIG_NO_HZ_FULL */
	return false;
}

/*
 * Bind the RCU grace-period kthreads to the housekeeping CPU.
 */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   RCU 유예 기간 kthread를 하우스키핑 CPU에 바인딩합니다. 
 */
static void rcu_bind_gp_kthread(void)
{
	if (!tick_nohz_full_enabled())
		return;
	housekeeping_affine(current, HK_FLAG_RCU);
}

/* Record the current task on dyntick-idle entry. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   dyntick-idle 항목에 현재 작업을 기록합니다. 
 */
static void noinstr rcu_dynticks_task_enter(void)
{
#if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->rcu_tasks_idle_cpu, smp_processor_id());
#endif /* #if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL) */
}

/* Record no current task on dyntick-idle exit. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   dyntick-idle 종료 시 현재 작업을 기록하지 않습니다. 
 */
static void noinstr rcu_dynticks_task_exit(void)
{
#if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->rcu_tasks_idle_cpu, -1);
#endif /* #if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL) */
}

/* Turn on heavyweight RCU tasks trace readers on idle/user entry. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   유휴/사용자 항목에서 무거운 RCU 작업 추적 판독기를 켭니다. 
 */
static void rcu_dynticks_task_trace_enter(void)
{
#ifdef CONFIG_TASKS_TRACE_RCU
	if (IS_ENABLED(CONFIG_TASKS_TRACE_RCU_READ_MB))
		current->trc_reader_special.b.need_mb = true;
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */
}

/* Turn off heavyweight RCU tasks trace readers on idle/user exit. */
/*
 * IAMROOT, 2023.07.17:
 * - papago
 *   유휴/사용자 종료 시 무거운 RCU 작업 추적 판독기를 끕니다. 
 */
static void rcu_dynticks_task_trace_exit(void)
{
#ifdef CONFIG_TASKS_TRACE_RCU
	if (IS_ENABLED(CONFIG_TASKS_TRACE_RCU_READ_MB))
		current->trc_reader_special.b.need_mb = false;
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */
}
