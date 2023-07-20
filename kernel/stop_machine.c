// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * kernel/stop_machine.c
 *
 * Copyright (C) 2008, 2005	IBM Corporation.
 * Copyright (C) 2008, 2005	Rusty Russell rusty@rustcorp.com.au
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 */
#include <linux/compiler.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/stop_machine.h>
#include <linux/interrupt.h>
#include <linux/kallsyms.h>
#include <linux/smpboot.h>
#include <linux/atomic.h>
#include <linux/nmi.h>
#include <linux/sched/wake_q.h>

/*
 * Structure to determine completion condition and record errors.  May
 * be shared by works on different cpus.
 */
struct cpu_stop_done {
	atomic_t		nr_todo;	/* nr left to execute */
	int			ret;		/* collected return value */
	struct completion	completion;	/* fired if nr_todo reaches 0 */
};

/* the actual stopper, one per every possible cpu, enabled on online cpus */
struct cpu_stopper {
	struct task_struct	*thread;

	raw_spinlock_t		lock;
	bool			enabled;	/* is this stopper enabled? */
	struct list_head	works;		/* list of pending works */

	struct cpu_stop_work	stop_work;	/* for stop_cpus */
	unsigned long		caller;
	cpu_stop_fn_t		fn;
};

static DEFINE_PER_CPU(struct cpu_stopper, cpu_stopper);
static bool stop_machine_initialized = false;

void print_stop_info(const char *log_lvl, struct task_struct *task)
{
	/*
	 * If @task is a stopper task, it cannot migrate and task_cpu() is
	 * stable.
	 */
	struct cpu_stopper *stopper = per_cpu_ptr(&cpu_stopper, task_cpu(task));

	if (task != stopper->thread)
		return;

	printk("%sStopper: %pS <- %pS\n", log_lvl, stopper->fn, (void *)stopper->caller);
}

/* static data for stop_cpus */
static DEFINE_MUTEX(stop_cpus_mutex);
static bool stop_cpus_in_progress;

/*
 * IAMROOT, 2023.03.25:
 * - @nr_todo설정및, completion 구조체를 초기화한다
 */
static void cpu_stop_init_done(struct cpu_stop_done *done, unsigned int nr_todo)
{
	memset(done, 0, sizeof(*done));
	atomic_set(&done->nr_todo, nr_todo);
	init_completion(&done->completion);
}

/* signal completion unless @done is NULL */
/*
 * IAMROOT, 2023.02.11:
 * - @done에 대해서 complete를 해준다.
 * - 요청 cpu 수(nr_todo)만큼 작업이 끝나면 complete 호출
 */
static void cpu_stop_signal_done(struct cpu_stop_done *done)
{
	if (atomic_dec_and_test(&done->nr_todo))
		complete(&done->completion);
}

/*
 * IAMROOT, 2023.02.11:
 * - @work->list를 @stopper->works에 추가한다.
 *   @wakeq에 @stopper->thread 를 추가한다.
 *
 * IAMROOT, 2023.07.16:
 * - 이후 wake_up_q 함수에 wakeq를 인자로 호출하면 wakeq 에 추가된 @stopper->thread를
 *   깨우게 된다.
 * - stopper thread는 깨어나면 @stopper->works에 연결된 @work의 fn 함수를 수행한다
 */
static void __cpu_stop_queue_work(struct cpu_stopper *stopper,
					struct cpu_stop_work *work,
					struct wake_q_head *wakeq)
{
	list_add_tail(&work->list, &stopper->works);
	wake_q_add(wakeq, stopper->thread);
}

/* queue @work to @stopper.  if offline, @work is completed immediately */
/*
 * IAMROOT, 2023.02.11:
 * - 1. @work->list와 per_cpu stopper 를 stopper->works와 wakeq에 추가
 *   2. stopper를 깨워서 @work의 fn을 처리한다.
 * - Return: stopper->enabled
 */
static bool cpu_stop_queue_work(unsigned int cpu, struct cpu_stop_work *work)
{
	struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);
	DEFINE_WAKE_Q(wakeq);
	unsigned long flags;
	bool enabled;

	preempt_disable();
	raw_spin_lock_irqsave(&stopper->lock, flags);
	enabled = stopper->enabled;

/*
 * IAMROOT, 2023.02.11:
 * - enable : queue work동작을 한다.,
 *   work->done : 기존 작업이 done인 상태. complete전송.
 *  
 */
	if (enabled)
		__cpu_stop_queue_work(stopper, work, &wakeq);
	else if (work->done)
		cpu_stop_signal_done(work->done);
	raw_spin_unlock_irqrestore(&stopper->lock, flags);

/*
 * IAMROOT, 2023.02.11:
 * - stopper를 깨운다.
 */
	wake_up_q(&wakeq);
	preempt_enable();

	return enabled;
}

/**
 * stop_one_cpu - stop a cpu
 * @cpu: cpu to stop
 * @fn: function to execute
 * @arg: argument to @fn
 *
 * Execute @fn(@arg) on @cpu.  @fn is run in a process context with
 * the highest priority preempting any task on the cpu and
 * monopolizing it.  This function returns after the execution is
 * complete.
 *
 * This function doesn't guarantee @cpu stays online till @fn
 * completes.  If @cpu goes down in the middle, execution may happen
 * partially or fully on different cpus.  @fn should either be ready
 * for that or the caller should ensure that @cpu stays online until
 * this function completes.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * -ENOENT if @fn(@arg) was not executed because @cpu was offline;
 * otherwise, the return value of @fn.
 */
/*
 * IAMROOT. 2023.07.08:
 * - google-translate
 * stop_one_cpu - cpu 중지
 * @cpu: 중지할 cpu
 * @fn: 실행할 함수
 * @arg: @fn에 대한 인수
 *
 * @cpu에서 @fn(@arg)을 실행합니다. @fn은 CPU의 모든 작업을 선점하고 독점하는 우선
 * 순위가 가장 높은 프로세스 컨텍스트에서 실행됩니다. 이 함수는 실행이 완료된 후
 * 반환됩니다.
 *
 * 이 함수는 @fn이 완료될 때까지 @cpu가 온라인 상태를 유지하도록
 * 보장하지 않습니다. @cpu가 중간에 다운되면 실행이 부분적으로 또는 완전히 다른
 * cpus에서 발생할 수 있습니다. @fn은 이를 위해 준비되어 있거나 호출자는 이 함수가
 * 완료될 때까지 @cpu가 온라인 상태를 유지하도록 해야 합니다.
 *
 * 컨텍스트:
 * 잘 수 있습니다.
 *
 * 반환값:
 * @cpu가 오프라인이어서 @fn(@arg)이 실행되지 않은 경우 -ENOENT;
 * 그렇지 않으면 @fn의 반환 값입니다.
 *
 * - @fn을 @cpu에서 실행하게 push하고 기다린다.
 */
int stop_one_cpu(unsigned int cpu, cpu_stop_fn_t fn, void *arg)
{
	struct cpu_stop_done done;
	struct cpu_stop_work work = { .fn = fn, .arg = arg, .done = &done, .caller = _RET_IP_ };

	cpu_stop_init_done(&done, 1);
	if (!cpu_stop_queue_work(cpu, &work))
		return -ENOENT;
	/*
	 * In case @cpu == smp_proccessor_id() we can avoid a sleep+wakeup
	 * cycle by doing a preemption:
	 */
	cond_resched();
	wait_for_completion(&done.completion);
	return done.ret;
}

/* This controls the threads on each CPU. */
enum multi_stop_state {
	/* Dummy starting state for thread. */
	MULTI_STOP_NONE,
	/* Awaiting everyone to be scheduled. */
	MULTI_STOP_PREPARE,
	/* Disable interrupts. */
	MULTI_STOP_DISABLE_IRQ,
	/* Run the function */
	MULTI_STOP_RUN,
	/* Exit */
	MULTI_STOP_EXIT,
};

/*
 * IAMROOT, 2023.03.27:
 * --- chat openai ---
 *  - fn: 각 CPU를 정지시키기 위해 호출되는 함수 포인터.
 *  - data: stop 함수에 전달되는 데이터에 대한 포인터.
 *  - num_threads: 중지해야 하는 스레드 또는 CPU의 수입니다.
 *  - active_cpus: 현재 활성화된 CPU 마스크에 대한 포인터.
 *  - state: 다중 중지 작업의 현재 상태를 나타내는 열거형입니다.
 *  - thread_ack: 중지 명령을 승인한 스레드 또는 CPU 수를 추적하는
 *                원자 카운터입니다.
 * --------------------
 */
struct multi_stop_data {
	cpu_stop_fn_t		fn;
	void			*data;
	/* Like num_online_cpus(), but hotplug cpu uses us, so we need this. */
	unsigned int		num_threads;
	const struct cpumask	*active_cpus;

	enum multi_stop_state	state;
	atomic_t		thread_ack;
};

/*
 * IAMROOT, 2023.03.27:
 * - num_threads만큼의 thread_ack에 set하고,
 *   @msdata의 state를 newstate로 교체한다.
 */
static void set_state(struct multi_stop_data *msdata,
		      enum multi_stop_state newstate)
{
	/* Reset ack counter. */
	atomic_set(&msdata->thread_ack, msdata->num_threads);
	smp_wmb();
	WRITE_ONCE(msdata->state, newstate);
}

/* Last one to ack a state moves to the next state. */
/*
 * IAMROOT, 2023.03.27:
 * - thread_ack를 1씩 감소한다. 만약 0이면 state를 up한다.
 *   즉 모든 thread가 확인이됬는지 검사하고, 마지막 thread가 state를
 *   up하는 방식이다.
 */
static void ack_state(struct multi_stop_data *msdata)
{
	if (atomic_dec_and_test(&msdata->thread_ack))
		set_state(msdata, msdata->state + 1);
}

notrace void __weak stop_machine_yield(const struct cpumask *cpumask)
{
	cpu_relax();
}

/* This is the cpu_stop function which stops the CPU. */
/*
 * IAMROOT, 2023.03.25:
 * - @data의 fn을 호출(msdata->fn(msdata->data))
 * - ex. stop_two_cpus 에서 호출한 경우
 *   2개의 stopper가 이 함수를 호출하지만 사용자 함수(msdata->fn)는 msdata의
 *   active_cpus mask에 설정된 cpu(cpu1) 에서만 호출
 */
static int multi_cpu_stop(void *data)
{
	struct multi_stop_data *msdata = data;
	enum multi_stop_state newstate, curstate = MULTI_STOP_NONE;
	int cpu = smp_processor_id(), err = 0;
	const struct cpumask *cpumask;
	unsigned long flags;
	bool is_active;

	/*
	 * When called from stop_machine_from_inactive_cpu(), irq might
	 * already be disabled.  Save the state and restore it on exit.
	 */
	/*
	 * IAMROOT. 2023.07.16:
	 * - google-translate
	 * stop_machine_from_inactive_cpu()에서 호출하면 irq가 이미 비활성화되었을 수
	 * 있습니다. 상태를 저장하고 종료 시 복원합니다.
	 */
	local_save_flags(flags);

	/*
	 * IAMROOT, 2023.07.16:
	 * - is_active가 true 인 조건
	 *   1. active_cpus가 설정되지 않았으면 첫번째 현재 cpu(smp_processor_id)가
	 *      첫번째 online cpu이다.
	 *   2. 현재 cpu가 active_cpus mask 에 있다.
	 */
	if (!msdata->active_cpus) {
		cpumask = cpu_online_mask;
		is_active = cpu == cpumask_first(cpumask);
	} else {
		cpumask = msdata->active_cpus;
		is_active = cpumask_test_cpu(cpu, cpumask);
	}

	/* Simple state machine */
	do {
		/* Chill out and ensure we re-read multi_stop_state. */
		stop_machine_yield(cpumask);
		newstate = READ_ONCE(msdata->state);
		if (newstate != curstate) {
			curstate = newstate;
			switch (curstate) {
			case MULTI_STOP_DISABLE_IRQ:
				/*
				 * IAMROOT, 2023.07.17:
				 * - run 하기 전에 irq 먼저 disable
				 */
				local_irq_disable();
				hard_irq_disable();
				break;
			case MULTI_STOP_RUN:
				/*
				 * IAMROOT, 2023.07.17:
				 * - msdata->active_cpus만 fn 호출
				 */
				if (is_active)
					err = msdata->fn(msdata->data);
				break;
			default:
				break;
			}
			/*
			 * IAMROOT, 2023.07.17:
			 * - msdata->num_threads 수만큼 호출되어야 다음 state(+1)
			 *   로 넘어간다
			 */
			ack_state(msdata);
		} else if (curstate > MULTI_STOP_PREPARE) {
			/*
			 * At this stage all other CPUs we depend on must spin
			 * in the same loop. Any reason for hard-lockup should
			 * be detected and reported on their side.
			 */
			/*
			 * IAMROOT. 2023.07.17:
			 * - google-translate
			 * 이 단계에서 우리가 의존하는 다른 모든 CPU는 동일한 루프에서
			 * 회전해야 합니다. 하드 록업에 대한 모든 이유를 감지하고 고객
			 * 측에서 보고해야 합니다.
			 * - XXX 무한루프에 빠졌을 경우를 위한 watchdog 설정?
			 */
			touch_nmi_watchdog();
		}
		rcu_momentary_dyntick_idle();
	} while (curstate != MULTI_STOP_EXIT);

	local_irq_restore(flags);
	return err;
}

/*
 * IAMROOT, 2023.07.15:
 * - 두 cpu의 work를 각각 stopper를 통해서 실행하게 한다.
 * - 1. @cpu1 @cpu2의 per cpu  cpu_stoper 각각 설정
 *   2. 설정된 stopper1,2를 wakeq에 추가하고 @work1,2(같음)도
 *      stopper1,2->works에 추가
 *   3. wakeq 에 추가된 stopper1,2 thread를 깨운다.
 * - 아래는 이후 진행상황 예측
 *   1. 깨어난 stopper thread는 work1,2(같음)에 설정된 callback함수(multi_cpu_stop)
 *      를  호출
 *   2. multi_cpu_stop 함수에서 work 구조체 arg 멤버로 설정된 msdata
 */
static int cpu_stop_queue_two_works(int cpu1, struct cpu_stop_work *work1,
				    int cpu2, struct cpu_stop_work *work2)
{
	struct cpu_stopper *stopper1 = per_cpu_ptr(&cpu_stopper, cpu1);
	struct cpu_stopper *stopper2 = per_cpu_ptr(&cpu_stopper, cpu2);
	DEFINE_WAKE_Q(wakeq);
	int err;

retry:
	/*
	 * The waking up of stopper threads has to happen in the same
	 * scheduling context as the queueing.  Otherwise, there is a
	 * possibility of one of the above stoppers being woken up by another
	 * CPU, and preempting us. This will cause us to not wake up the other
	 * stopper forever.
	 */
	/*
	 * IAMROOT. 2023.07.15:
	 * - google-translate
	 * 스토퍼 스레드의 깨우기는 대기열과 동일한 스케줄링 컨텍스트에서 발생해야
	 * 합니다. 그렇지 않으면 위의 스토퍼 중 하나가 다른 CPU에 의해 깨어나 선점할
	 * 가능성이 있습니다. 이것은 우리가 다른 스토퍼를 영원히 깨우지 못하게 할 것입니다.
	 */
	preempt_disable();
	raw_spin_lock_irq(&stopper1->lock);
	raw_spin_lock_nested(&stopper2->lock, SINGLE_DEPTH_NESTING);

	/*
	 * IAMROOT, 2023.07.16:
	 * - 두 stopper가 모두 enabled 여야 한다.
	 */
	if (!stopper1->enabled || !stopper2->enabled) {
		err = -ENOENT;
		goto unlock;
	}

	/*
	 * Ensure that if we race with __stop_cpus() the stoppers won't get
	 * queued up in reverse order leading to system deadlock.
	 *
	 * We can't miss stop_cpus_in_progress if queue_stop_cpus_work() has
	 * queued a work on cpu1 but not on cpu2, we hold both locks.
	 *
	 * It can be falsely true but it is safe to spin until it is cleared,
	 * queue_stop_cpus_work() does everything under preempt_disable().
	 */
	/*
	 * IAMROOT. 2023.07.15:
	 * - google-translate
	 * __stop_cpus()로 경쟁하는 경우 스토퍼가 시스템 교착 상태로 이어지는 역순으로
	 * 대기하지 않도록 합니다.
	 *
	 * queue_stop_cpus_work()가 cpu1에는 작업을 대기했지만
	 * cpu2에는 대기하지 않은 경우 stop_cpus_in_progress를 놓칠 수 없습니다.
	 * 두 잠금을 모두 보유합니다.
	 *
	 * 거짓일 수 있지만 해제될 때까지 회전하는 것이 안전합니다.
	 * queue_stop_cpus_work()는 preempt_disable()에서 모든 작업을 수행합니다.
	 */
	if (unlikely(stop_cpus_in_progress)) {
		err = -EDEADLK;
		goto unlock;
	}

	err = 0;
	__cpu_stop_queue_work(stopper1, work1, &wakeq);
	__cpu_stop_queue_work(stopper2, work2, &wakeq);

unlock:
	raw_spin_unlock(&stopper2->lock);
	raw_spin_unlock_irq(&stopper1->lock);

	/*
	 * IAMROOT, 2023.07.16:
	 * - 이미 진행중이라면 끝날때 까지 대기하다 재시도하도록 한다.
	 */
	if (unlikely(err == -EDEADLK)) {
		preempt_enable();

		while (stop_cpus_in_progress)
			cpu_relax();

		goto retry;
	}

	/*
	 * IAMROOT, 2023.07.16:
	 * - wakeq에 연결된 stopper1,2 thread를 깨운다.게 되고 깨어난 stopper1,2는
	 *   work1,2(같음) 설정된 callback함수(multi_cpu_stop) 을 호출한다.
	 */
	wake_up_q(&wakeq);
	preempt_enable();

	return err;
}
/**
 * stop_two_cpus - stops two cpus
 * @cpu1: the cpu to stop
 * @cpu2: the other cpu to stop
 * @fn: function to execute
 * @arg: argument to @fn
 *
 * Stops both the current and specified CPU and runs @fn on one of them.
 *
 * returns when both are completed.
 */
/*
 * IAMROOT. 2023.07.15:
 * - google-translate
 * stop_two_cpus - 2개의 cpus를 중지합니다.
 * @cpu1: 중지할 cpu
 * @cpu2: 중지할 다른 cpu
 * @fn: 실행할 함수
 * @arg: @fn에 대한 인수
 *
 * 현재 CPU와 지정된 CPU를 모두 중지하고 그 중 하나에서 @fn을 실행합니다.
 *
 * 둘 다 완료되면 반환됩니다.
 *
 * - @cpu1, @cpu2를 중지하고 @cpu1 에서 @fn 호출
 * - 전달된 인자
 *   @cpu1: dst_cpu
 *   @cpu2: src_cpu
 *   @fn: migrate_swap_stop
 *   @arg: migration_swap_arg
 * - 1. msdata, work1,2 구조체 설정
 *   2. nr_todo 2로설정및 completion 구조체를 초기화
 *   3. @cpu1 @cpu2의 per cpu  cpu_stoper 각각 설정
 *   4. 설정된 stopper1,2를 wakeq에 추가하고 @work1,2(같음)도
 *      stopper1,2->works에 추가
 *   5. wakeq 에 추가된 stopper1,2 thread를 깨운다.
 *   6. 깨어난 stopper thread는 work1,2(같음)에 설정된 callback함수(multi_cpu_stop)
 *      를 호출
 *   7. multi_cpu_stop 함수에서 work 구조체 arg 멤버로 설정된 msdata 구조체의
 *      @fn 함수(migrate_swap_stop)를 @arg 를 인자로 호출
 *
 * - @cpu1에서 @fn을 수행하고 @cpu2에서는 @cpu1이 끝날떄까지 기다린다.
 *   해당 작업은 multi_cpu_stop()에서 수행된다.
 */
int stop_two_cpus(unsigned int cpu1, unsigned int cpu2, cpu_stop_fn_t fn, void *arg)
{
	struct cpu_stop_done done;
	struct cpu_stop_work work1, work2;
	struct multi_stop_data msdata;

	/*
	 * IAMROOT, 2023.07.17:
	 * - num_threads: threads 갯수만큼 실행되어야 다음 state로 넘어간다
	 * - active_cpus: dst_cpu의 cpumask. active_cpus 에만 @fn 함수가 호출된다.
	 */
	msdata = (struct multi_stop_data){
		.fn = fn,
		.data = arg,
		.num_threads = 2,
		.active_cpus = cpumask_of(cpu1),
	};

	work1 = work2 = (struct cpu_stop_work){
		.fn = multi_cpu_stop,
		.arg = &msdata,
		.done = &done,
		.caller = _RET_IP_,
	};

/*
 * IAMROOT, 2023.07.20:
 * - 2개의 thread에서 응답을 받아야되므로 2로 설정한다.
 */
	cpu_stop_init_done(&done, 2);
	/*
	 * IAMROOT, 2023.07.17:
	 * - 1. thread_ack 를 num_threads로 초기화
	 *   2. state를 MULTI_STOP_PREPARE로 설정
	 */
	set_state(&msdata, MULTI_STOP_PREPARE);

	/*
	 * IAMROOT, 2023.07.17:
	 * - XXX swap 하는 이유는?
	 *
	 * - cpu번호가 작은게 @fn을 수행하도록 정렬한다.
	 */
	if (cpu1 > cpu2)
		swap(cpu1, cpu2);
	/*
	 * IAMROOT, 2023.07.17:
	 * - XXX 같은 구조체의 reference를 인자(work1,2)로 호출하고 있고
	 *   multi_cpu_stop 함수의 인자인 arg는 msdata의 reference 이다.
	 *   따라서 두개의 stopper가 multi_cpu_stop 함수를 호출하게 되고
	 *   인자인 msdata는 같은 주소를 참조한다. 이는 state 동기화시 같은 데이터를
	 *   읽고 쓰게된다.
	 */
	if (cpu_stop_queue_two_works(cpu1, &work1, cpu2, &work2))
		return -ENOENT;

	wait_for_completion(&done.completion);
	return done.ret;
}

/**
 * stop_one_cpu_nowait - stop a cpu but don't wait for completion
 * @cpu: cpu to stop
 * @fn: function to execute
 * @arg: argument to @fn
 * @work_buf: pointer to cpu_stop_work structure
 *
 * Similar to stop_one_cpu() but doesn't wait for completion.  The
 * caller is responsible for ensuring @work_buf is currently unused
 * and will remain untouched until stopper starts executing @fn.
 *
 * CONTEXT:
 * Don't care.
 *
 * RETURNS:
 * true if cpu_stop_work was queued successfully and @fn will be called,
 * false otherwise.
 */
/*
 * IAMROOT, 2023.02.11:
 * - stopper는 cpu_stopper_thread함수를 통해서 work의 @fn을 호출할것이다.
 */
bool stop_one_cpu_nowait(unsigned int cpu, cpu_stop_fn_t fn, void *arg,
			struct cpu_stop_work *work_buf)
{
	*work_buf = (struct cpu_stop_work){ .fn = fn, .arg = arg, .caller = _RET_IP_, };
	return cpu_stop_queue_work(cpu, work_buf);
}

/*
 * IAMROOT, 2023.03.25:
 * - stopper가 enable 된 경우 work를 추가 하고 stopper 를 깨워서 work를 처리하게 한다.
 * - Return: 한개 이상의 stopper 가 enabled 인 경우 true
 */
static bool queue_stop_cpus_work(const struct cpumask *cpumask,
				 cpu_stop_fn_t fn, void *arg,
				 struct cpu_stop_done *done)
{
	struct cpu_stop_work *work;
	unsigned int cpu;
	bool queued = false;

	/*
	 * Disable preemption while queueing to avoid getting
	 * preempted by a stopper which might wait for other stoppers
	 * to enter @fn which can lead to deadlock.
	 */
	/*
	 * IAMROOT. 2023.03.25:
	 * - google-translate
	 * 교착 상태로 이어질 수 있는 다른 스토퍼가 @fn에 들어갈 때까지 기다릴 수 있는
	 * 스토퍼에 의해 선점되지 않도록 대기열에 있는 동안 선점을 비활성화합니다.
	 */
	preempt_disable();
	stop_cpus_in_progress = true;
	barrier();
	for_each_cpu(cpu, cpumask) {
		work = &per_cpu(cpu_stopper.stop_work, cpu);
		work->fn = fn;
		work->arg = arg;
		work->done = done;
		work->caller = _RET_IP_;
		if (cpu_stop_queue_work(cpu, work))
			queued = true;
	}
	barrier();
	stop_cpus_in_progress = false;
	preempt_enable();

	return queued;
}

/*
 * IAMROOT, 2023.03.25:
 * - 1. done 구조체 초기화
 *   2. cpumask에 해당하는 stopper를 깨우고 fn 수행후 완료되면 함수를 빠져 나온다.
 */
static int __stop_cpus(const struct cpumask *cpumask,
		       cpu_stop_fn_t fn, void *arg)
{
	struct cpu_stop_done done;

	cpu_stop_init_done(&done, cpumask_weight(cpumask));
	if (!queue_stop_cpus_work(cpumask, fn, arg, &done))
		return -ENOENT;
	wait_for_completion(&done.completion);
	return done.ret;
}

/**
 * stop_cpus - stop multiple cpus
 * @cpumask: cpus to stop
 * @fn: function to execute
 * @arg: argument to @fn
 *
 * Execute @fn(@arg) on online cpus in @cpumask.  On each target cpu,
 * @fn is run in a process context with the highest priority
 * preempting any task on the cpu and monopolizing it.  This function
 * returns after all executions are complete.
 *
 * This function doesn't guarantee the cpus in @cpumask stay online
 * till @fn completes.  If some cpus go down in the middle, execution
 * on the cpu may happen partially or fully on different cpus.  @fn
 * should either be ready for that or the caller should ensure that
 * the cpus stay online until this function completes.
 *
 * All stop_cpus() calls are serialized making it safe for @fn to wait
 * for all cpus to start executing it.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * -ENOENT if @fn(@arg) was not executed at all because all cpus in
 * @cpumask were offline; otherwise, 0 if all executions of @fn
 * returned 0, any non zero return value if any returned non zero.
 */
/*
 * IAMROOT. 2023.03.25:
 * - google-translate
 * stop_cpus - 다중 cpus 중지
 * @cpumask: 중지할 cpus
 * @fn: 실행할 함수:
 * @arg: @fn에 대한 인수
 *
 * @cpumask의 온라인 cpus에서 @fn(@arg)을 실행합니다. 각 대상 CPU에서
 * @fn은 CPU의 작업을 선점하고 독점하는 우선 순위가 가장 높은 프로세스 컨텍스트에서
 * 실행됩니다. 이 함수는 모든 실행이 완료된 후 반환됩니다.
 *
 * 이 함수는 @fn이 완료될 때까지 @cpumask의 CPU가 온라인 상태를 유지하도록 보장하지
 * 않습니다. 일부 CPU가 중간에 다운되면 CPU에서 실행이 부분적으로 또는 완전히 다른 CPU에서
 * 발생할 수 있습니다. @fn은 이에 대비하거나 호출자가 이 기능이 완료될 때까지 CPU가 온라인
 * 상태를 유지하도록 해야 합니다.
 *
 * 모든 stop_cpus() 호출은 @fn이 모든 CPU가 실행을 시작할 때까지 안전하게 기다릴 수
 * 있도록 직렬화됩니다.
 *
 * 컨텍스트:
 * 잘 수 있습니다.
 *
 * 반환값:
 * -ENOENT @cpumask의 모든 CPU가 오프라인 상태였기 때문에 @fn(@arg)이 전혀 실행되지
 * 않은 경우; 그렇지 않으면 @fn의 모든 실행이 0을 반환하면 0, 0이 아닌 값을 반환하면 0이
 * 아닌 값을 반환합니다.
 *
 * - NOTE. CONTEXT가 잠들수 있다는 의미는 전달된 fn에서 sleep api를 사용할 수 있다는 의미
 * - mutex_lock으로 인해 실제 진입하는 cpu는 1개만일 것이고, 해당 cpu가
 *   stopper를 기다리는 waiter 역할을 할 것이다.
 */
static int stop_cpus(const struct cpumask *cpumask, cpu_stop_fn_t fn, void *arg)
{
	int ret;

	/* static works are used, process one request at a time */
	mutex_lock(&stop_cpus_mutex);
	ret = __stop_cpus(cpumask, fn, arg);
	mutex_unlock(&stop_cpus_mutex);
	return ret;
}

static int cpu_stop_should_run(unsigned int cpu)
{
	struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);
	unsigned long flags;
	int run;

	raw_spin_lock_irqsave(&stopper->lock, flags);
	run = !list_empty(&stopper->works);
	raw_spin_unlock_irqrestore(&stopper->lock, flags);
	return run;
}

static void cpu_stopper_thread(unsigned int cpu)
{
	struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);
	struct cpu_stop_work *work;

repeat:
	work = NULL;
	raw_spin_lock_irq(&stopper->lock);
	if (!list_empty(&stopper->works)) {
		work = list_first_entry(&stopper->works,
					struct cpu_stop_work, list);
		list_del_init(&work->list);
	}
	raw_spin_unlock_irq(&stopper->lock);

	if (work) {
		cpu_stop_fn_t fn = work->fn;
		void *arg = work->arg;
		struct cpu_stop_done *done = work->done;
		int ret;

		/* cpu stop callbacks must not sleep, make in_atomic() == T */
		stopper->caller = work->caller;
		stopper->fn = fn;
		preempt_count_inc();
		ret = fn(arg);
		if (done) {
			if (ret)
				done->ret = ret;
			cpu_stop_signal_done(done);
		}
		preempt_count_dec();
		stopper->fn = NULL;
		stopper->caller = 0;
		WARN_ONCE(preempt_count(),
			  "cpu_stop: %ps(%p) leaked preempt count\n", fn, arg);
		goto repeat;
	}
}

void stop_machine_park(int cpu)
{
	struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);
	/*
	 * Lockless. cpu_stopper_thread() will take stopper->lock and flush
	 * the pending works before it parks, until then it is fine to queue
	 * the new works.
	 */
	stopper->enabled = false;
	kthread_park(stopper->thread);
}

extern void sched_set_stop_task(int cpu, struct task_struct *stop);

static void cpu_stop_create(unsigned int cpu)
{
	sched_set_stop_task(cpu, per_cpu(cpu_stopper.thread, cpu)); }

static void cpu_stop_park(unsigned int cpu)
{
	struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);

	WARN_ON(!list_empty(&stopper->works));
}

/*
 * IAMROOT, 2023.02.11:
 * - @cpu에 대한 stopper를 enable하고 thread를 unpark한다.
 */
void stop_machine_unpark(int cpu)
{
	struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);

	stopper->enabled = true;
	kthread_unpark(stopper->thread);
}

static struct smp_hotplug_thread cpu_stop_threads = {
	.store			= &cpu_stopper.thread,
	.thread_should_run	= cpu_stop_should_run,
	.thread_fn		= cpu_stopper_thread,
	.thread_comm		= "migration/%u",
	.create			= cpu_stop_create,
	.park			= cpu_stop_park,
	.selfparking		= true,
};

/*
 * IAMROOT, 2023.02.11:
 * - cpu_stopper 초기화. smpboot_register_percpu_thread()함수에서 
 *   thread가 만들어지고, stopper실행시 cpu_stopper_thread()을 통해
 *   cpu_stop_work에 등록되는 fn들이 호출될것이다.
 * - ps -ef를 통해서 migration/X가 항상 있는게 확인된다.
 * - stopper thread는 deadline보다도 높은 우선순위를 가진다.
 */
static int __init cpu_stop_init(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);

		raw_spin_lock_init(&stopper->lock);
		INIT_LIST_HEAD(&stopper->works);
	}

	BUG_ON(smpboot_register_percpu_thread(&cpu_stop_threads));
	stop_machine_unpark(raw_smp_processor_id());
	stop_machine_initialized = true;
	return 0;
}
early_initcall(cpu_stop_init);

/*
 * IAMROOT, 2023.03.27:
 * - 
 */
int stop_machine_cpuslocked(cpu_stop_fn_t fn, void *data,
			    const struct cpumask *cpus)
{
	struct multi_stop_data msdata = {
		.fn = fn,
		.data = data,
		.num_threads = num_online_cpus(),
		.active_cpus = cpus,
	};

	lockdep_assert_cpus_held();

	if (!stop_machine_initialized) {
		/*
		 * Handle the case where stop_machine() is called
		 * early in boot before stop_machine() has been
		 * initialized.
		 */
		/*
		 * IAMROOT. 2023.03.25:
		 * - google-translate
		 * stop_machine()이 초기화되기 전에 부팅 초기에 stop_machine()이
		 * 호출되는 경우를 처리합니다.
		 */
		unsigned long flags;
		int ret;

		WARN_ON_ONCE(msdata.num_threads != 1);

		local_irq_save(flags);
		hard_irq_disable();
		ret = (*fn)(data);
		local_irq_restore(flags);

		return ret;
	}

	/* Set the initial state and stop all online cpus. */
/*
 * IAMROOT, 2023.03.27:
 * - num_online_cpus() 에 대해서 stop을 대기하고 state를 MULTI_STOP_PREPARE로
 *   설정한다. num_online_cpus()개수만큼 stop이 됬으면 MULTI_STOP_DISABLE_IRQ
 *   로 넘어갈것이다.
 */
	set_state(&msdata, MULTI_STOP_PREPARE);
	/*
	 * IAMROOT, 2023.03.25:
	 * - stopper thread 는 multi_cpu_stop 함수를 통해서 msdata->fn을 호출한다
	 */
	return stop_cpus(cpu_online_mask, multi_cpu_stop, &msdata);
}

/*
 * IAMROOT, 2022.02.17:
 * - @cpus들을 멈추고 @fn을 stopper thread 에서 호출하여 실행후 완료될때까지 기다린다.
 */
int stop_machine(cpu_stop_fn_t fn, void *data, const struct cpumask *cpus)
{
	int ret;

	/* No CPUs can come up or down during this. */
	cpus_read_lock();
	ret = stop_machine_cpuslocked(fn, data, cpus);
	cpus_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(stop_machine);

/**
 * stop_machine_from_inactive_cpu - stop_machine() from inactive CPU
 * @fn: the function to run
 * @data: the data ptr for the @fn()
 * @cpus: the cpus to run the @fn() on (NULL = any online cpu)
 *
 * This is identical to stop_machine() but can be called from a CPU which
 * is not active.  The local CPU is in the process of hotplug (so no other
 * CPU hotplug can start) and not marked active and doesn't have enough
 * context to sleep.
 *
 * This function provides stop_machine() functionality for such state by
 * using busy-wait for synchronization and executing @fn directly for local
 * CPU.
 *
 * CONTEXT:
 * Local CPU is inactive.  Temporarily stops all active CPUs.
 *
 * RETURNS:
 * 0 if all executions of @fn returned 0, any non zero return value if any
 * returned non zero.
 */
int stop_machine_from_inactive_cpu(cpu_stop_fn_t fn, void *data,
				  const struct cpumask *cpus)
{
	struct multi_stop_data msdata = { .fn = fn, .data = data,
					    .active_cpus = cpus };
	struct cpu_stop_done done;
	int ret;

	/* Local CPU must be inactive and CPU hotplug in progress. */
	BUG_ON(cpu_active(raw_smp_processor_id()));
	msdata.num_threads = num_active_cpus() + 1;	/* +1 for local */

	/* No proper task established and can't sleep - busy wait for lock. */
	while (!mutex_trylock(&stop_cpus_mutex))
		cpu_relax();

	/* Schedule work on other CPUs and execute directly for local CPU */
	set_state(&msdata, MULTI_STOP_PREPARE);
	cpu_stop_init_done(&done, num_active_cpus());
	queue_stop_cpus_work(cpu_active_mask, multi_cpu_stop, &msdata,
			     &done);
	ret = multi_cpu_stop(&msdata);

	/* Busy wait for completion. */
	while (!completion_done(&done.completion))
		cpu_relax();

	mutex_unlock(&stop_cpus_mutex);
	return ret ?: done.ret;
}
