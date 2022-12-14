// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/sched/loadavg.c
 *
 * This file contains the magic bits required to compute the global loadavg
 * figure. Its a silly number but people think its important. We go through
 * great pains to make it work on big machines and tickless kernels.
 */
#include "sched.h"

/*
 * Global load-average calculations
 *
 * We take a distributed and async approach to calculating the global load-avg
 * in order to minimize overhead.
 *
 * The global load average is an exponentially decaying average of nr_running +
 * nr_uninterruptible.
 *
 * Once every LOAD_FREQ:
 *
 *   nr_active = 0;
 *   for_each_possible_cpu(cpu)
 *	nr_active += cpu_of(cpu)->nr_running + cpu_of(cpu)->nr_uninterruptible;
 *
 *   avenrun[n] = avenrun[0] * exp_n + nr_active * (1 - exp_n)
 *
 * Due to a number of reasons the above turns in the mess below:
 *
 *  - for_each_possible_cpu() is prohibitively expensive on machines with
 *    serious number of CPUs, therefore we need to take a distributed approach
 *    to calculating nr_active.
 *
 *        \Sum_i x_i(t) = \Sum_i x_i(t) - x_i(t_0) | x_i(t_0) := 0
 *                      = \Sum_i { \Sum_j=1 x_i(t_j) - x_i(t_j-1) }
 *
 *    So assuming nr_active := 0 when we start out -- true per definition, we
 *    can simply take per-CPU deltas and fold those into a global accumulate
 *    to obtain the same result. See calc_load_fold_active().
 *
 *    Furthermore, in order to avoid synchronizing all per-CPU delta folding
 *    across the machine, we assume 10 ticks is sufficient time for every
 *    CPU to have completed this task.
 *
 *    This places an upper-bound on the IRQ-off latency of the machine. Then
 *    again, being late doesn't loose the delta, just wrecks the sample.
 *
 *  - cpu_rq()->nr_uninterruptible isn't accurately tracked per-CPU because
 *    this would add another cross-CPU cacheline miss and atomic operation
 *    to the wakeup path. Instead we increment on whatever CPU the task ran
 *    when it went into uninterruptible state and decrement on whatever CPU
 *    did the wakeup. This means that only the sum of nr_uninterruptible over
 *    all CPUs yields the correct result.
 *
 *  This covers the NO_HZ=n code, for extra head-aches, see the comment below.
 */

/* Variables and functions for calc_load */
atomic_long_t calc_load_tasks;

/*
 * IAMROOT, 2022.12.03:
 * - calc_global_load에서 load를 계산해야될 시각.(jiffies)
 */
unsigned long calc_load_update;
unsigned long avenrun[3];
EXPORT_SYMBOL(avenrun); /* should be removed */

/**
 * get_avenrun - get the load average array
 * @loads:	pointer to dest load array
 * @offset:	offset to add
 * @shift:	shift count to shift the result left
 *
 * These values are estimates at best, so no need for locking.
 */
void get_avenrun(unsigned long *loads, unsigned long offset, int shift)
{
	loads[0] = (avenrun[0] + offset) << shift;
	loads[1] = (avenrun[1] + offset) << shift;
	loads[2] = (avenrun[2] + offset) << shift;
}

/*
 * IAMROOT, 2022.12.03:
 * - running + uninterruptible의 변화된값을 return한다.
 */
long calc_load_fold_active(struct rq *this_rq, long adjust)
{
	long nr_active, delta = 0;

	nr_active = this_rq->nr_running - adjust;
	nr_active += (int)this_rq->nr_uninterruptible;

	if (nr_active != this_rq->calc_load_active) {
		delta = nr_active - this_rq->calc_load_active;
		this_rq->calc_load_active = nr_active;
	}

	return delta;
}

/**
 * fixed_power_int - compute: x^n, in O(log n) time
 *
 * @x:         base of the power
 * @frac_bits: fractional bits of @x
 * @n:         power to raise @x to.
 *
 * By exploiting the relation between the definition of the natural power
 * function: x^n := x*x*...*x (x multiplied by itself for n times), and
 * the binary encoding of numbers used by computers: n := \Sum n_i * 2^i,
 * (where: n_i \elem {0, 1}, the binary vector representing n),
 * we find: x^n := x^(\Sum n_i * 2^i) := \Prod x^(n_i * 2^i), which is
 * of course trivially computable in O(log_2 n), the length of our binary
 * vector.
 */
/*
 * IAMROOT, 2022.12.03:
 * - e^N의 이진화정수 계산.
 */
static unsigned long
fixed_power_int(unsigned long x, unsigned int frac_bits, unsigned int n)
{
	unsigned long result = 1UL << frac_bits;

	if (n) {
		for (;;) {
			if (n & 1) {
				result *= x;
				result += 1UL << (frac_bits - 1);
				result >>= frac_bits;
			}
			n >>= 1;
			if (!n)
				break;
			x *= x;
			x += 1UL << (frac_bits - 1);
			x >>= frac_bits;
		}
	}

	return result;
}

/*
 * a1 = a0 * e + a * (1 - e)
 *
 * a2 = a1 * e + a * (1 - e)
 *    = (a0 * e + a * (1 - e)) * e + a * (1 - e)
 *    = a0 * e^2 + a * (1 - e) * (1 + e)
 *
 * a3 = a2 * e + a * (1 - e)
 *    = (a0 * e^2 + a * (1 - e) * (1 + e)) * e + a * (1 - e)
 *    = a0 * e^3 + a * (1 - e) * (1 + e + e^2)
 *
 *  ...
 *
 * an = a0 * e^n + a * (1 - e) * (1 + e + ... + e^n-1) [1]
 *    = a0 * e^n + a * (1 - e) * (1 - e^n)/(1 - e)
 *    = a0 * e^n + a * (1 - e^n)
 *
 * [1] application of the geometric series:
 *
 *              n         1 - x^(n+1)
 *     S_n := \Sum x^i = -------------
 *             i=0          1 - x
 */

/*
 * IAMROOT, 2022.12.03:
 * n = 1 ) x1 = old * e + new * (1 - e)
 * n = 2 ) x2 = x1 * e + new * (1 - e)
 * n = 3 ) x3 = x2 * e + new * (1 - e)
 *
 * n = N ) x1 = old * e^(N) + new * (1 - e^(N))
 *
 * - old * e^n + new * (1 - e^n)
 */
unsigned long
calc_load_n(unsigned long load, unsigned long exp,
	    unsigned long active, unsigned int n)
{
	return calc_load(load, fixed_power_int(exp, FSHIFT, n), active);
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * Handle NO_HZ for the global load-average.
 *
 * Since the above described distributed algorithm to compute the global
 * load-average relies on per-CPU sampling from the tick, it is affected by
 * NO_HZ.
 *
 * The basic idea is to fold the nr_active delta into a global NO_HZ-delta upon
 * entering NO_HZ state such that we can include this as an 'extra' CPU delta
 * when we read the global state.
 *
 * Obviously reality has to ruin such a delightfully simple scheme:
 *
 *  - When we go NO_HZ idle during the window, we can negate our sample
 *    contribution, causing under-accounting.
 *
 *    We avoid this by keeping two NO_HZ-delta counters and flipping them
 *    when the window starts, thus separating old and new NO_HZ load.
 *
 *    The only trick is the slight shift in index flip for read vs write.
 *
 *        0s            5s            10s           15s
 *          +10           +10           +10           +10
 *        |-|-----------|-|-----------|-|-----------|-|
 *    r:0 0 1           1 0           0 1           1 0
 *    w:0 1 1           0 0           1 1           0 0
 *
 *    This ensures we'll fold the old NO_HZ contribution in this window while
 *    accumulating the new one.
 *
 *  - When we wake up from NO_HZ during the window, we push up our
 *    contribution, since we effectively move our sample point to a known
 *    busy state.
 *
 *    This is solved by pushing the window forward, and thus skipping the
 *    sample, for this CPU (effectively using the NO_HZ-delta for this CPU which
 *    was in effect at the time the window opened). This also solves the issue
 *    of having to deal with a CPU having been in NO_HZ for multiple LOAD_FREQ
 *    intervals.
 *
 * When making the ILB scale, we should try to pull this in as well.
 */
/*
 * IAMROOT, 2022.12.03:
 * - cpu가 nohz상태일때 이전 task개수(running + uninterruptible)와 현재
 *   task개수의 변화값. 사용되면 0으로 초기화된다.
 */
static atomic_long_t calc_load_nohz[2];

/*
 * IAMROOT, 2022.12.03:
 * - calc_load_nohz의 lock
 */
static int calc_load_idx;

/*
 * IAMROOT, 2022.12.03:
 *                 0s            5s            10s           15s
 *                   +10           +10           +10           +10
 *                 |-|-----------|-|-----------|-|-----------|-|
 *             r:0 0 1           1 0           0 1           1 0
 *             w:0 1 1           0 0           1 1           0 0
 *                   ^             ^             ^             ^
 * calc_load_idx   0 1             2             3             4
 *
 * - idx == 0 이였던경우 reader는 idx == 0을 계속 읽을것이고
 *   writer는 idx = 1에 write를한다. write를 다했으면 idx++ 하여
 *   reader가 완전히 write된 idx = 1에 접근할수있게 할것이다.
 */
static inline int calc_load_write_idx(void)
{
	int idx = calc_load_idx;

	/*
	 * See calc_global_nohz(), if we observe the new index, we also
	 * need to observe the new update time.
	 */
	smp_rmb();

	/*
	 * If the folding window started, make sure we start writing in the
	 * next NO_HZ-delta.
	 */
	if (!time_before(jiffies, READ_ONCE(calc_load_update)))
		idx++;

	return idx & 1;
}

/*
 * IAMROOT, 2022.12.03:
 * - calc_global_nohz()에 의해 증가된 calc_load_idx의 홀/짝여부를 가져온다.
 */
static inline int calc_load_read_idx(void)
{
	return calc_load_idx & 1;
}

/*
 * IAMROOT, 2022.12.03:
 * - task개수 변동이 생겼으면 변동값을 calc_load_nohz에 저장한다.
 */
static void calc_load_nohz_fold(struct rq *rq)
{
	long delta;

	delta = calc_load_fold_active(rq, 0);
	if (delta) {
		int idx = calc_load_write_idx();

		atomic_long_add(delta, &calc_load_nohz[idx]);
	}
}

/*
 * IAMROOT, 2022.12.03:
 * - nohz가 시작될시 진입한다.
 *   task개수 변화를 감지하여 calc_load_nohz에 갱신한다.
 */
void calc_load_nohz_start(void)
{
	/*
	 * We're going into NO_HZ mode, if there's any pending delta, fold it
	 * into the pending NO_HZ delta.
	 */
	calc_load_nohz_fold(this_rq());
}

/*
 * Keep track of the load for NOHZ_FULL, must be called between
 * calc_load_nohz_{start,stop}().
 */
void calc_load_nohz_remote(struct rq *rq)
{
	calc_load_nohz_fold(rq);
}

void calc_load_nohz_stop(void)
{
	struct rq *this_rq = this_rq();

	/*
	 * If we're still before the pending sample window, we're done.
	 */
	this_rq->calc_load_update = READ_ONCE(calc_load_update);
	if (time_before(jiffies, this_rq->calc_load_update))
		return;

	/*
	 * We woke inside or after the sample window, this means we're already
	 * accounted through the nohz accounting, so skip the entire deal and
	 * sync up for the next window.
	 */
	if (time_before(jiffies, this_rq->calc_load_update + 10))
		this_rq->calc_load_update += LOAD_FREQ;
}

/*
 * IAMROOT, 2022.12.03:
 * - calc_load_idx의 홀짝에 따른 delta값을 가져온다 
 *   (seqcount latch와 비슷한 원리의 lock.). 없다면 return 0.
 * - delta
 *   cpu가 nohz상태일때 cpu에서 runnable tasks의 수.
 */
static long calc_load_nohz_read(void)
{
/*
 * IAMROOT, 2022.12.03:
 * - idx는 0 or 1
 */
	int idx = calc_load_read_idx();
	long delta = 0;

/*
 * IAMROOT, 2022.12.03:
 * - read를 해서 0이 아니라면 어떤 값(delta)이 있다는것. 0와 xchg를 하여 가져와서 return한다.
 */
	if (atomic_long_read(&calc_load_nohz[idx]))
		delta = atomic_long_xchg(&calc_load_nohz[idx], 0);

	return delta;
}

/*
 * NO_HZ can leave us missing all per-CPU ticks calling
 * calc_load_fold_active(), but since a NO_HZ CPU folds its delta into
 * calc_load_nohz per calc_load_nohz_start(), all we need to do is fold
 * in the pending NO_HZ delta if our NO_HZ period crossed a load cycle boundary.
 *
 * Once we've updated the global active value, we need to apply the exponential
 * weights adjusted to the number of cycles missed.
 */
/*
 * IAMROOT, 2022.12.03:
 * - papago
 *   NO_HZ는 calc_load_fold_active()를 호출하는 모든 CPU당 틱을 놓치게 할 수 있지만,
 *   NO_HZ CPU는 calc_load_nohz_start()마다 델타를 calc_load_nohz로 접기 때문에 NO_HZ 기간이
 *   로드 사이클을 넘으면 보류 중인 NO_HZ 델타를 접기만 하면 됩니다. 경계.
 *
 *   전역 활성 값을 업데이트한 후에는 놓친 주기 수에 맞게 조정된 지수 가중치를 적용해야
 *   합니다.
 *
 * - nohz구간이 존재한다면 해당구간의 시간을 보정해준다.
 * - calc_load_idx를 증가시켜 write가 완료됫음을 알린다.
 */
static void calc_global_nohz(void)
{
	unsigned long sample_window;
	long delta, active, n;

/*
 * IAMROOT, 2022.12.03:
 * - calc_global_load()에서 이함수를 진입한경우 calc_global_load가 old에서 LOAD_FREQ만큼
 *   증가됫을것이다. 증가 됬음에도 불구하고 현재시간 + 10보다 이전이라면 calc_load를 다시
 *   수행한다.
 * - 결국 sleep으로 인해서 고려가 안된 시간만큼 n회를 더 수행하기 위함
 *
 * - ex)
 *   |    sleep  |   sleep  | wakeup |
 *   ^last갱신                       ^new갱신
 *               ^miss      ^miss
 *   miss가 2번 발생했다.
 *   new갱신은 calc_global_load()에서 했지만 2번을 못해줬다. 2번에 대해서 여기서 처리한다.
 */
	sample_window = READ_ONCE(calc_load_update);
	if (!time_before(jiffies, sample_window + 10)) {
		/*
		 * Catch-up, fold however many we are behind still
		 */
		delta = jiffies - sample_window - 10;
		n = 1 + (delta / LOAD_FREQ);

		active = atomic_long_read(&calc_load_tasks);
		active = active > 0 ? active * FIXED_1 : 0;

/*
 * IAMROOT, 2022.12.03:
 * - n회를 구하여 n만큼 load를 구한다.
 */
		avenrun[0] = calc_load_n(avenrun[0], EXP_1, active, n);
		avenrun[1] = calc_load_n(avenrun[1], EXP_5, active, n);
		avenrun[2] = calc_load_n(avenrun[2], EXP_15, active, n);

		WRITE_ONCE(calc_load_update, sample_window + n * LOAD_FREQ);
	}

	/*
	 * Flip the NO_HZ index...
	 *
	 * Make sure we first write the new time then flip the index, so that
	 * calc_load_write_idx() will see the new time when it reads the new
	 * index, this avoids a double flip messing things up.
	 */
/*
 * IAMROOT, 2022.12.03:
 * - papago
 *   NO_HZ 인덱스 뒤집기...
 *   먼저 새 시간을 쓴 다음 색인을 뒤집어 calc_load_write_idx()가 새 색인을 읽을 때 새 시간을
 *   볼 수 있도록 해야 합니다. 이렇게 하면 이중 뒤집기가 일을 망치는 것을 방지할 수 있습니다.
 */
	smp_wmb();
	calc_load_idx++;
}
#else /* !CONFIG_NO_HZ_COMMON */

static inline long calc_load_nohz_read(void) { return 0; }
static inline void calc_global_nohz(void) { }

#endif /* CONFIG_NO_HZ_COMMON */

/*
 * calc_load - update the avenrun load estimates 10 ticks after the
 * CPUs have updated calc_load_tasks.
 *
 * Called from the global timer code.
 */
/*
 * IAMROOT, 2022.12.03:
 * - sample_window이후의 시간(5초마다)일때 avenrun, load를 갱신한다.
 *   또한 nohz 구간이 있었을경우 해당 구간을 고려한다.
 */
void calc_global_load(void)
{
	unsigned long sample_window;
	long active, delta;

/*
 * IAMROOT, 2022.12.03:
 * - sample_window + 10 전이라면 return.
 * - 10의 의미.
 *   task들이 동작을할때 calc_load_tasks를 갱신해줘야되는데 이 시간을 조금 고려한거 같다.
 *   calc_global_load함수는 상대적으로 비용이 많이드는 작업이라 필요할때만 한다.
 */
	sample_window = READ_ONCE(calc_load_update);
	if (time_before(jiffies, sample_window + 10))
		return;

	/*
	 * Fold the 'old' NO_HZ-delta to include all NO_HZ CPUs.
	 */
	delta = calc_load_nohz_read();
/*
 * IAMROOT, 2022.12.03:
 * - nohz때 계산된 delta값을 calc_load_tasks에 더해준다.
 */
	if (delta)
		atomic_long_add(delta, &calc_load_tasks);

/*
 * IAMROOT, 2022.12.03:
 * - active는 task숫자라고 생각하면된다.
 */
	active = atomic_long_read(&calc_load_tasks);

/*
 * IAMROOT, 2022.12.03:
 * - active * 2048
 */
	active = active > 0 ? active * FIXED_1 : 0;

/*
 * IAMROOT, 2022.12.03:
 * - /proc/loadavg
 *   ex) 1.33 0.63 0.40 1/712 40301
 *       ^0   ^1   ^2
 *      1분  5분  15분
 */
	avenrun[0] = calc_load(avenrun[0], EXP_1, active);
	avenrun[1] = calc_load(avenrun[1], EXP_5, active);
	avenrun[2] = calc_load(avenrun[2], EXP_15, active);

/*
 * IAMROOT, 2022.12.03:
 * - 이전 update시각 + LOAD_FREQ(5초 + 1tck)이후로 next update시각을 정한다.
 *   최종적으로 next update시각은 sample_window + LOAD_FREQ + 10tick이 될것이다.
 */
	WRITE_ONCE(calc_load_update, sample_window + LOAD_FREQ);

	/*
	 * In case we went to NO_HZ for multiple LOAD_FREQ intervals
	 * catch up in bulk.
	 */
	calc_global_nohz();
}

/*
 * Called from scheduler_tick() to periodically update this CPU's
 * active count.
 */
void calc_global_load_tick(struct rq *this_rq)
{
	long delta;

	if (time_before(jiffies, this_rq->calc_load_update))
		return;

	delta  = calc_load_fold_active(this_rq, 0);
	if (delta)
		atomic_long_add(delta, &calc_load_tasks);

	this_rq->calc_load_update += LOAD_FREQ;
}
