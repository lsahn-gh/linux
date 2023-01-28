/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_SCHEDSTATS

/*
 * Expects runqueue lock to be held for atomicity of update
 */
/*
 * IAMROOT, 2023.01.28:
 * - sched stats처리.
 */
static inline void
rq_sched_info_arrive(struct rq *rq, unsigned long long delta)
{
	if (rq) {
		rq->rq_sched_info.run_delay += delta;
		rq->rq_sched_info.pcount++;
	}
}

/*
 * Expects runqueue lock to be held for atomicity of update
 */
/*
 * IAMROOT, 2023.01.28:
 * - sched stats처리.
 */
static inline void
rq_sched_info_depart(struct rq *rq, unsigned long long delta)
{
	if (rq)
		rq->rq_cpu_time += delta;
}

static inline void
rq_sched_info_dequeue(struct rq *rq, unsigned long long delta)
{
	if (rq)
		rq->rq_sched_info.run_delay += delta;
}
#define   schedstat_enabled()		static_branch_unlikely(&sched_schedstats)
#define __schedstat_inc(var)		do { var++; } while (0)
#define   schedstat_inc(var)		do { if (schedstat_enabled()) { var++; } } while (0)
#define __schedstat_add(var, amt)	do { var += (amt); } while (0)
#define   schedstat_add(var, amt)	do { if (schedstat_enabled()) { var += (amt); } } while (0)
#define __schedstat_set(var, val)	do { var = (val); } while (0)
#define   schedstat_set(var, val)	do { if (schedstat_enabled()) { var = (val); } } while (0)
#define   schedstat_val(var)		(var)
#define   schedstat_val_or_zero(var)	((schedstat_enabled()) ? (var) : 0)

#else /* !CONFIG_SCHEDSTATS: */
static inline void rq_sched_info_arrive  (struct rq *rq, unsigned long long delta) { }
static inline void rq_sched_info_dequeue(struct rq *rq, unsigned long long delta) { }
static inline void rq_sched_info_depart  (struct rq *rq, unsigned long long delta) { }
# define   schedstat_enabled()		0
# define __schedstat_inc(var)		do { } while (0)
# define   schedstat_inc(var)		do { } while (0)
# define __schedstat_add(var, amt)	do { } while (0)
# define   schedstat_add(var, amt)	do { } while (0)
# define __schedstat_set(var, val)	do { } while (0)
# define   schedstat_set(var, val)	do { } while (0)
# define   schedstat_val(var)		0
# define   schedstat_val_or_zero(var)	0
#endif /* CONFIG_SCHEDSTATS */

#ifdef CONFIG_PSI
/*
 * PSI tracks state that persists across sleeps, such as iowaits and
 * memory stalls. As a result, it has to distinguish between sleeps,
 * where a task's runnable state changes, and requeues, where a task
 * and its state are being moved between CPUs and runqueues.
 */
static inline void psi_enqueue(struct task_struct *p, bool wakeup)
{
	int clear = 0, set = TSK_RUNNING;

	if (static_branch_likely(&psi_disabled))
		return;

	if (!wakeup || p->sched_psi_wake_requeue) {
		if (p->in_memstall)
			set |= TSK_MEMSTALL;
		if (p->sched_psi_wake_requeue)
			p->sched_psi_wake_requeue = 0;
	} else {
		if (p->in_iowait)
			clear |= TSK_IOWAIT;
	}

	psi_task_change(p, clear, set);
}

static inline void psi_dequeue(struct task_struct *p, bool sleep)
{
	int clear = TSK_RUNNING;

	if (static_branch_likely(&psi_disabled))
		return;

	/*
	 * A voluntary sleep is a dequeue followed by a task switch. To
	 * avoid walking all ancestors twice, psi_task_switch() handles
	 * TSK_RUNNING and TSK_IOWAIT for us when it moves TSK_ONCPU.
	 * Do nothing here.
	 */
	if (sleep)
		return;

	if (p->in_memstall)
		clear |= TSK_MEMSTALL;

	psi_task_change(p, clear, 0);
}

static inline void psi_ttwu_dequeue(struct task_struct *p)
{
	if (static_branch_likely(&psi_disabled))
		return;
	/*
	 * Is the task being migrated during a wakeup? Make sure to
	 * deregister its sleep-persistent psi states from the old
	 * queue, and let psi_enqueue() know it has to requeue.
	 */
	if (unlikely(p->in_iowait || p->in_memstall)) {
		struct rq_flags rf;
		struct rq *rq;
		int clear = 0;

		if (p->in_iowait)
			clear |= TSK_IOWAIT;
		if (p->in_memstall)
			clear |= TSK_MEMSTALL;

		rq = __task_rq_lock(p, &rf);
		psi_task_change(p, clear, 0);
		p->sched_psi_wake_requeue = 1;
		__task_rq_unlock(rq, &rf);
	}
}

/*
 * IAMROOT, 2023.01.28:
 * - psi 처리
 */
static inline void psi_sched_switch(struct task_struct *prev,
				    struct task_struct *next,
				    bool sleep)
{
	if (static_branch_likely(&psi_disabled))
		return;

	psi_task_switch(prev, next, sleep);
}

#else /* CONFIG_PSI */
static inline void psi_enqueue(struct task_struct *p, bool wakeup) {}
static inline void psi_dequeue(struct task_struct *p, bool sleep) {}
static inline void psi_ttwu_dequeue(struct task_struct *p) {}
static inline void psi_sched_switch(struct task_struct *prev,
				    struct task_struct *next,
				    bool sleep) {}
#endif /* CONFIG_PSI */

#ifdef CONFIG_SCHED_INFO
/*
 * We are interested in knowing how long it was from the *first* time a
 * task was queued to the time that it finally hit a CPU, we call this routine
 * from dequeue_task() to account for possible rq->clock skew across CPUs. The
 * delta taken on each CPU would annul the skew.
 */
/*
 * IAMROOT. 2023.01.14:
 * - google-translate
 *   우리는 태스크가 큐에 들어간 *처음* 시간부터 마침내 CPU에 도달한 시간까지의
 *   시간을 알고 싶습니다. 우리는 dequeue_task()에서 이 루틴을 호출하여 CPU에서
 *   가능한 rq->clock skew를 설명합니다. 각 CPU에서 취한 델타는 skew를 무효화합니다.
 *
 * -  task 가 rq에 진입한 후 실행 되기 까지 delay 시간을 run_delay에 저장
 */
static inline void sched_info_dequeue(struct rq *rq, struct task_struct *t)
{
	unsigned long long delta = 0;

	if (!t->sched_info.last_queued)
		return;

	delta = rq_clock(rq) - t->sched_info.last_queued;
	t->sched_info.last_queued = 0;
	t->sched_info.run_delay += delta;

	rq_sched_info_dequeue(rq, delta);
}

/*
 * Called when a task finally hits the CPU.  We can now calculate how
 * long it was waiting to run.  We also note when it began so that we
 * can keep stats on how long its timeslice is.
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   작업이 마침내 CPU에 도달하면 호출됩니다. 이제 실행 대기 시간을 
 *   계산할 수 있습니다. 또한 타임슬라이스의 길이에 대한 통계를 유지할 
 *   수 있도록 언제 시작되었는지 기록합니다.
 * - 대기큐에 있다가 cpu로 올라가는상황. sched_info 정보를 기록한다.
 */
static void sched_info_arrive(struct rq *rq, struct task_struct *t)
{
	unsigned long long now, delta = 0;

/*
 * IAMROOT, 2023.01.28:
 * - 대기큐에 돌아갈때 set되엇을것이다.
 */
	if (!t->sched_info.last_queued)
		return;

	now = rq_clock(rq);

/*
 * IAMROOT, 2023.01.28:
 * - 기다렸던 시간.
 */
	delta = now - t->sched_info.last_queued;
	t->sched_info.last_queued = 0;
	t->sched_info.run_delay += delta;
	t->sched_info.last_arrival = now;
	t->sched_info.pcount++;

	rq_sched_info_arrive(rq, delta);
}

/*
 * This function is only called from enqueue_task(), but also only updates
 * the timestamp if it is already not set.  It's assumed that
 * sched_info_dequeue() will clear that stamp when appropriate.
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   작업이 마침내 CPU에 도달하면 호출됩니다. 이제 실행 대기 시간을 계산할 
 *   수 있습니다. 또한 타임슬라이스의 길이에 대한 통계를 유지할 수 있도록 
 *   언제 시작되었는지 기록합니다.
 * - rq의 queueing했을대의 시각을 last_queued에 등록한다.
 */
static inline void sched_info_enqueue(struct rq *rq, struct task_struct *t)
{
	if (!t->sched_info.last_queued)
		t->sched_info.last_queued = rq_clock(rq);
}

/*
 * Called when a process ceases being the active-running process involuntarily
 * due, typically, to expiring its time slice (this may also be called when
 * switching to the idle task).  Now we can calculate how long we ran.
 * Also, if the process is still in the TASK_RUNNING state, call
 * sched_info_enqueue() to mark that it has now again started waiting on
 * the runqueue.
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   일반적으로 시간 조각 만료로 인해 프로세스가 비자발적으로 활성 실행 
 *   프로세스가 되는 것을 중단할 때 호출됩니다(유휴 작업으로 전환할 때도 호출될 
 *   수 있음). 이제 우리는 얼마나 오래 달렸는지 계산할 수 있습니다. 
 *   또한 프로세스가 여전히 TASK_RUNNING 상태인 경우 sched_info_enqueue()를 
 *   호출하여 이제 다시 실행 대기열에서 대기하기 시작했음을 표시합니다.
 * - @t sched_info의 last_queued를 rq clock으로 update.
 */
static inline void sched_info_depart(struct rq *rq, struct task_struct *t)
{
/*
 * IAMROOT, 2023.01.28:
 * - delat는 대기큐에 있는동안의 시간이 될것이다.
 */
	unsigned long long delta = rq_clock(rq) - t->sched_info.last_arrival;

	rq_sched_info_depart(rq, delta);

	if (task_is_running(t))
		sched_info_enqueue(rq, t);
}

/*
 * Called when tasks are switched involuntarily due, typically, to expiring
 * their time slice.  (This may also be called when switching to or from
 * the idle task.)  We are only called when prev != next.
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   작업이 일반적으로 타임 슬라이스 만료로 인해 비자발적으로 전환될 때 
 *   호출됩니다. (유휴 작업으로 전환하거나 전환할 때 호출될 수도 있습니다.) 
 *   prev != next일 때만 호출됩니다.
 * - prev, next 각각의 cpu에서의 제거, 등록에 따른 sched_info를 기록한다.
 */
static inline void
sched_info_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	/*
	 * prev now departs the CPU.  It's not interesting to record
	 * stats about how efficient we were at scheduling the idle
	 * process, however.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   prev는 이제 CPU를 떠납니다. 그러나 유휴 프로세스를 예약하는 데 얼마나 
 *   효율적이었는지에 대한 통계를 기록하는 것은 흥미롭지 않습니다.
 * - prev, next가 idle task가 아니라면 해당 동작들을 한다.
 */
	if (prev != rq->idle)
		sched_info_depart(rq, prev);

	if (next != rq->idle)
		sched_info_arrive(rq, next);
}

#else /* !CONFIG_SCHED_INFO: */
# define sched_info_enqueue(rq, t)	do { } while (0)
# define sched_info_dequeue(rq, t)	do { } while (0)
# define sched_info_switch(rq, t, next)	do { } while (0)
#endif /* CONFIG_SCHED_INFO */
