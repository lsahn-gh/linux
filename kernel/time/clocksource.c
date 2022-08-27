// SPDX-License-Identifier: GPL-2.0+
/*
 * This file contains the functions which manage clocksource drivers.
 *
 * Copyright (C) 2004, 2005 IBM, John Stultz (johnstul@us.ibm.com)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h> /* for spin_unlock_irq() using preempt_count() m68k */
#include <linux/tick.h>
#include <linux/kthread.h>
#include <linux/prandom.h>
#include <linux/cpu.h>

#include "tick-internal.h"
#include "timekeeping_internal.h"

/**
 * clocks_calc_mult_shift - calculate mult/shift factors for scaled math of clocks
 * @mult:	pointer to mult variable
 * @shift:	pointer to shift variable
 * @from:	frequency to convert from
 * @to:		frequency to convert to
 * @maxsec:	guaranteed runtime conversion range in seconds
 *
 * The function evaluates the shift/mult pair for the scaled math
 * operations of clocksources and clockevents.
 *
 * @to and @from are frequency values in HZ. For clock sources @to is
 * NSEC_PER_SEC == 1GHz and @from is the counter frequency. For clock
 * event @to is the counter frequency and @from is NSEC_PER_SEC.
 *
 * The @maxsec conversion range argument controls the time frame in
 * seconds which must be covered by the runtime conversion with the
 * calculated mult and shift factors. This guarantees that no 64bit
 * overflow happens when the input value of the conversion is
 * multiplied with the calculated mult factor. Larger ranges may
 * reduce the conversion accuracy by choosing smaller mult and shift
 * factors.
 */
/*
 * IAMROOT, 2022.08.20:
 * ex) from = 19200000, to = 10^9, maxsec = 600
 * - sftacc 계산
 *   600 * 19200000 = 0x2_AEA5_4000 => shift32 => 0x2
 *   두번 loop를 돌면 sftacc = 30가 된다.
 *
 * - 이진화 정수화.
 */
void
clocks_calc_mult_shift(u32 *mult, u32 *shift, u32 from, u32 to, u32 maxsec)
{
	u64 tmp;
/*
 * IAMROOT, 2022.08.20:
 * - 이진화 정확도는 최대 32bit.
 */
	u32 sft, sftacc= 32;

	/*
	 * Calculate the shift factor which is limiting the conversion
	 * range:
	 */
/*
 * IAMROOT, 2022.08.20:
 * 63..      55                                                      0
 * | unusable |            counter                                   |
 *
 * |          |            empty            | sec(최대600) | freq    |
 *
 * 여기선 정확도용으로 사용할 bit를 계산한다.
 *
 * | 정확도용                 |               sec(최대600) | freq     |
 *                            ^
 *                            이 부분일거같은 bit를 찾는것.
 *
 * | 정확도용                 |               sec(최대600) | freq     |
 * <----------- sftacc ------> <------- maxsec * from ---------------->
 *                                       ^  
 * ex) from = 19200000, to = 10^9, maxsec = 600, sftacc = 30
 *
 * | 정확도용                 |               sec(최대600) | freq     |
 * <----------- sftacc ------> <------- maxsec * from ---------------->
 *              30 bits       ^  34bit
 */
	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) {
		tmp >>=1;
		sftacc--;
	}

	/*
	 * Find the conversion shift/mult pair which has the best
	 * accuracy and fits the maxsec conversion range:
	 */
/*
 * IAMROOT, 2022.08.20:
 * - from * mult >> shift = to의 개념에서 mult, shift값을 구한다.
 *   tmp = (to << sft + from / 2) / from
 *
 * - sftacc 내에서 처리될수있는 mult, shift값을 계산한다.
 * - from / 2는 나머지를 고려해서, do_div(mult, from)를 할때 정확성을 조금 올리는것. 
 * - from으로 위애서 sftacc(정확도)를 구하고, sftacc내의 범위에서 to값의 최종 정확도를 shift
 *   한값이 들어오는지 계산하여 mult, sft값을 구한다.
 *
 * ex) from = 19200000, to = 10^9, maxsec = 600, sftacc = 30
 * sft = 24이면
 * tmp = 1000000000 << 24 = 16777216000000000
 * tmp += 9600000
 * tmp = do_div(16777216000000000,  19200000) = 873813333 = 0x3415_5555
 * tmp >> sftasce = 0x3415_5555 >> 30 == 0
 * mult = 0x3415_5555, shift = 24가 된다.
 *
 * - ex) 0.1을 이진화 정수화.
 *
 *   0            | (0.1에 대한 값)
 *
 *   0.1 * 2 = 0.2 -> 0
 *   0.2 * 2 = 0.4 -> 0
 *   0.4 * 2 = 0.8 -> 0
 *   0.8 * 2 = 1.6 -> 1
 *    .------- 0.6
 *   0.6 * 2 = 1.2 -> 1
 *   0.2 * 2 = 0.4 -> 0
 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (u64) to << sft;

/*
 * IAMROOT, 2022.08.27:
 * - round up
 */
		tmp += from / 2;
		do_div(tmp, from);
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = tmp;
	*shift = sft;
}
EXPORT_SYMBOL_GPL(clocks_calc_mult_shift);

/*[Clocksource internal variables]---------
 * curr_clocksource:
 *	currently selected clocksource.
 * suspend_clocksource:
 *	used to calculate the suspend time.
 * clocksource_list:
 *	linked list with the registered clocksources
 * clocksource_mutex:
 *	protects manipulations to curr_clocksource and the clocksource_list
 * override_name:
 *	Name of the user-specified clocksource.
 */
static struct clocksource *curr_clocksource;
static struct clocksource *suspend_clocksource;
static LIST_HEAD(clocksource_list);
static DEFINE_MUTEX(clocksource_mutex);
static char override_name[CS_NAME_LEN];
static int finished_booting;
static u64 suspend_start;

/*
 * Threshold: 0.0312s, when doubled: 0.0625s.
 * Also a default for cs->uncertainty_margin when registering clocks.
 */
#define WATCHDOG_THRESHOLD (NSEC_PER_SEC >> 5)

/*
 * Maximum permissible delay between two readouts of the watchdog
 * clocksource surrounding a read of the clocksource being validated.
 * This delay could be due to SMIs, NMIs, or to VCPU preemptions.  Used as
 * a lower bound for cs->uncertainty_margin values when registering clocks.
 */
#define WATCHDOG_MAX_SKEW (50 * NSEC_PER_USEC)

#ifdef CONFIG_CLOCKSOURCE_WATCHDOG
static void clocksource_watchdog_work(struct work_struct *work);
static void clocksource_select(void);

static LIST_HEAD(watchdog_list);
static struct clocksource *watchdog;
static struct timer_list watchdog_timer;
static DECLARE_WORK(watchdog_work, clocksource_watchdog_work);
static DEFINE_SPINLOCK(watchdog_lock);
static int watchdog_running;
static atomic_t watchdog_reset_pending;

static inline void clocksource_watchdog_lock(unsigned long *flags)
{
	spin_lock_irqsave(&watchdog_lock, *flags);
}

static inline void clocksource_watchdog_unlock(unsigned long *flags)
{
	spin_unlock_irqrestore(&watchdog_lock, *flags);
}

static int clocksource_watchdog_kthread(void *data);
static void __clocksource_change_rating(struct clocksource *cs, int rating);

/*
 * Interval: 0.5sec.
 */
#define WATCHDOG_INTERVAL (HZ >> 1)

static void clocksource_watchdog_work(struct work_struct *work)
{
	/*
	 * We cannot directly run clocksource_watchdog_kthread() here, because
	 * clocksource_select() calls timekeeping_notify() which uses
	 * stop_machine(). One cannot use stop_machine() from a workqueue() due
	 * lock inversions wrt CPU hotplug.
	 *
	 * Also, we only ever run this work once or twice during the lifetime
	 * of the kernel, so there is no point in creating a more permanent
	 * kthread for this.
	 *
	 * If kthread_run fails the next watchdog scan over the
	 * watchdog_list will find the unstable clock again.
	 */
	kthread_run(clocksource_watchdog_kthread, NULL, "kwatchdog");
}

static void __clocksource_unstable(struct clocksource *cs)
{
	cs->flags &= ~(CLOCK_SOURCE_VALID_FOR_HRES | CLOCK_SOURCE_WATCHDOG);
	cs->flags |= CLOCK_SOURCE_UNSTABLE;

	/*
	 * If the clocksource is registered clocksource_watchdog_kthread() will
	 * re-rate and re-select.
	 */
	if (list_empty(&cs->list)) {
		cs->rating = 0;
		return;
	}

	if (cs->mark_unstable)
		cs->mark_unstable(cs);

	/* kick clocksource_watchdog_kthread() */
	if (finished_booting)
		schedule_work(&watchdog_work);
}

/**
 * clocksource_mark_unstable - mark clocksource unstable via watchdog
 * @cs:		clocksource to be marked unstable
 *
 * This function is called by the x86 TSC code to mark clocksources as unstable;
 * it defers demotion and re-selection to a kthread.
 */
void clocksource_mark_unstable(struct clocksource *cs)
{
	unsigned long flags;

	spin_lock_irqsave(&watchdog_lock, flags);
	if (!(cs->flags & CLOCK_SOURCE_UNSTABLE)) {
		if (!list_empty(&cs->list) && list_empty(&cs->wd_list))
			list_add(&cs->wd_list, &watchdog_list);
		__clocksource_unstable(cs);
	}
	spin_unlock_irqrestore(&watchdog_lock, flags);
}

ulong max_cswd_read_retries = 3;
module_param(max_cswd_read_retries, ulong, 0644);
EXPORT_SYMBOL_GPL(max_cswd_read_retries);
static int verify_n_cpus = 8;
module_param(verify_n_cpus, int, 0644);

static bool cs_watchdog_read(struct clocksource *cs, u64 *csnow, u64 *wdnow)
{
	unsigned int nretries;
	u64 wd_end, wd_delta;
	int64_t wd_delay;

	for (nretries = 0; nretries <= max_cswd_read_retries; nretries++) {
		local_irq_disable();
		*wdnow = watchdog->read(watchdog);
		*csnow = cs->read(cs);
		wd_end = watchdog->read(watchdog);
		local_irq_enable();

		wd_delta = clocksource_delta(wd_end, *wdnow, watchdog->mask);
		wd_delay = clocksource_cyc2ns(wd_delta, watchdog->mult,
					      watchdog->shift);
		if (wd_delay <= WATCHDOG_MAX_SKEW) {
			if (nretries > 1 || nretries >= max_cswd_read_retries) {
				pr_warn("timekeeping watchdog on CPU%d: %s retried %d times before success\n",
					smp_processor_id(), watchdog->name, nretries);
			}
			return true;
		}
	}

	pr_warn("timekeeping watchdog on CPU%d: %s read-back delay of %lldns, attempt %d, marking unstable\n",
		smp_processor_id(), watchdog->name, wd_delay, nretries);
	return false;
}

static u64 csnow_mid;
static cpumask_t cpus_ahead;
static cpumask_t cpus_behind;
static cpumask_t cpus_chosen;

static void clocksource_verify_choose_cpus(void)
{
	int cpu, i, n = verify_n_cpus;

	if (n < 0) {
		/* Check all of the CPUs. */
		cpumask_copy(&cpus_chosen, cpu_online_mask);
		cpumask_clear_cpu(smp_processor_id(), &cpus_chosen);
		return;
	}

	/* If no checking desired, or no other CPU to check, leave. */
	cpumask_clear(&cpus_chosen);
	if (n == 0 || num_online_cpus() <= 1)
		return;

	/* Make sure to select at least one CPU other than the current CPU. */
	cpu = cpumask_next(-1, cpu_online_mask);
	if (cpu == smp_processor_id())
		cpu = cpumask_next(cpu, cpu_online_mask);
	if (WARN_ON_ONCE(cpu >= nr_cpu_ids))
		return;
	cpumask_set_cpu(cpu, &cpus_chosen);

	/* Force a sane value for the boot parameter. */
	if (n > nr_cpu_ids)
		n = nr_cpu_ids;

	/*
	 * Randomly select the specified number of CPUs.  If the same
	 * CPU is selected multiple times, that CPU is checked only once,
	 * and no replacement CPU is selected.  This gracefully handles
	 * situations where verify_n_cpus is greater than the number of
	 * CPUs that are currently online.
	 */
	for (i = 1; i < n; i++) {
		cpu = prandom_u32() % nr_cpu_ids;
		cpu = cpumask_next(cpu - 1, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			cpu = cpumask_next(-1, cpu_online_mask);
		if (!WARN_ON_ONCE(cpu >= nr_cpu_ids))
			cpumask_set_cpu(cpu, &cpus_chosen);
	}

	/* Don't verify ourselves. */
	cpumask_clear_cpu(smp_processor_id(), &cpus_chosen);
}

static void clocksource_verify_one_cpu(void *csin)
{
	struct clocksource *cs = (struct clocksource *)csin;

	csnow_mid = cs->read(cs);
}

void clocksource_verify_percpu(struct clocksource *cs)
{
	int64_t cs_nsec, cs_nsec_max = 0, cs_nsec_min = LLONG_MAX;
	u64 csnow_begin, csnow_end;
	int cpu, testcpu;
	s64 delta;

	if (verify_n_cpus == 0)
		return;
	cpumask_clear(&cpus_ahead);
	cpumask_clear(&cpus_behind);
	cpus_read_lock();
	preempt_disable();
	clocksource_verify_choose_cpus();
	if (cpumask_weight(&cpus_chosen) == 0) {
		preempt_enable();
		cpus_read_unlock();
		pr_warn("Not enough CPUs to check clocksource '%s'.\n", cs->name);
		return;
	}
	testcpu = smp_processor_id();
	pr_warn("Checking clocksource %s synchronization from CPU %d to CPUs %*pbl.\n", cs->name, testcpu, cpumask_pr_args(&cpus_chosen));
	for_each_cpu(cpu, &cpus_chosen) {
		if (cpu == testcpu)
			continue;
		csnow_begin = cs->read(cs);
		smp_call_function_single(cpu, clocksource_verify_one_cpu, cs, 1);
		csnow_end = cs->read(cs);
		delta = (s64)((csnow_mid - csnow_begin) & cs->mask);
		if (delta < 0)
			cpumask_set_cpu(cpu, &cpus_behind);
		delta = (csnow_end - csnow_mid) & cs->mask;
		if (delta < 0)
			cpumask_set_cpu(cpu, &cpus_ahead);
		delta = clocksource_delta(csnow_end, csnow_begin, cs->mask);
		cs_nsec = clocksource_cyc2ns(delta, cs->mult, cs->shift);
		if (cs_nsec > cs_nsec_max)
			cs_nsec_max = cs_nsec;
		if (cs_nsec < cs_nsec_min)
			cs_nsec_min = cs_nsec;
	}
	preempt_enable();
	cpus_read_unlock();
	if (!cpumask_empty(&cpus_ahead))
		pr_warn("        CPUs %*pbl ahead of CPU %d for clocksource %s.\n",
			cpumask_pr_args(&cpus_ahead), testcpu, cs->name);
	if (!cpumask_empty(&cpus_behind))
		pr_warn("        CPUs %*pbl behind CPU %d for clocksource %s.\n",
			cpumask_pr_args(&cpus_behind), testcpu, cs->name);
	if (!cpumask_empty(&cpus_ahead) || !cpumask_empty(&cpus_behind))
		pr_warn("        CPU %d check durations %lldns - %lldns for clocksource %s.\n",
			testcpu, cs_nsec_min, cs_nsec_max, cs->name);
}
EXPORT_SYMBOL_GPL(clocksource_verify_percpu);

static void clocksource_watchdog(struct timer_list *unused)
{
	u64 csnow, wdnow, cslast, wdlast, delta;
	int next_cpu, reset_pending;
	int64_t wd_nsec, cs_nsec;
	struct clocksource *cs;
	u32 md;

	spin_lock(&watchdog_lock);
	if (!watchdog_running)
		goto out;

	reset_pending = atomic_read(&watchdog_reset_pending);

	list_for_each_entry(cs, &watchdog_list, wd_list) {

		/* Clocksource already marked unstable? */
		if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
			if (finished_booting)
				schedule_work(&watchdog_work);
			continue;
		}

		if (!cs_watchdog_read(cs, &csnow, &wdnow)) {
			/* Clock readout unreliable, so give it up. */
			__clocksource_unstable(cs);
			continue;
		}

		/* Clocksource initialized ? */
		if (!(cs->flags & CLOCK_SOURCE_WATCHDOG) ||
		    atomic_read(&watchdog_reset_pending)) {
			cs->flags |= CLOCK_SOURCE_WATCHDOG;
			cs->wd_last = wdnow;
			cs->cs_last = csnow;
			continue;
		}

		delta = clocksource_delta(wdnow, cs->wd_last, watchdog->mask);
		wd_nsec = clocksource_cyc2ns(delta, watchdog->mult,
					     watchdog->shift);

		delta = clocksource_delta(csnow, cs->cs_last, cs->mask);
		cs_nsec = clocksource_cyc2ns(delta, cs->mult, cs->shift);
		wdlast = cs->wd_last; /* save these in case we print them */
		cslast = cs->cs_last;
		cs->cs_last = csnow;
		cs->wd_last = wdnow;

		if (atomic_read(&watchdog_reset_pending))
			continue;

		/* Check the deviation from the watchdog clocksource. */
		md = cs->uncertainty_margin + watchdog->uncertainty_margin;
		if (abs(cs_nsec - wd_nsec) > md) {
			pr_warn("timekeeping watchdog on CPU%d: Marking clocksource '%s' as unstable because the skew is too large:\n",
				smp_processor_id(), cs->name);
			pr_warn("                      '%s' wd_nsec: %lld wd_now: %llx wd_last: %llx mask: %llx\n",
				watchdog->name, wd_nsec, wdnow, wdlast, watchdog->mask);
			pr_warn("                      '%s' cs_nsec: %lld cs_now: %llx cs_last: %llx mask: %llx\n",
				cs->name, cs_nsec, csnow, cslast, cs->mask);
			if (curr_clocksource == cs)
				pr_warn("                      '%s' is current clocksource.\n", cs->name);
			else if (curr_clocksource)
				pr_warn("                      '%s' (not '%s') is current clocksource.\n", curr_clocksource->name, cs->name);
			else
				pr_warn("                      No current clocksource.\n");
			__clocksource_unstable(cs);
			continue;
		}

		if (cs == curr_clocksource && cs->tick_stable)
			cs->tick_stable(cs);

		if (!(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES) &&
		    (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS) &&
		    (watchdog->flags & CLOCK_SOURCE_IS_CONTINUOUS)) {
			/* Mark it valid for high-res. */
			cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;

			/*
			 * clocksource_done_booting() will sort it if
			 * finished_booting is not set yet.
			 */
			if (!finished_booting)
				continue;

			/*
			 * If this is not the current clocksource let
			 * the watchdog thread reselect it. Due to the
			 * change to high res this clocksource might
			 * be preferred now. If it is the current
			 * clocksource let the tick code know about
			 * that change.
			 */
			if (cs != curr_clocksource) {
				cs->flags |= CLOCK_SOURCE_RESELECT;
				schedule_work(&watchdog_work);
			} else {
				tick_clock_notify();
			}
		}
	}

	/*
	 * We only clear the watchdog_reset_pending, when we did a
	 * full cycle through all clocksources.
	 */
	if (reset_pending)
		atomic_dec(&watchdog_reset_pending);

	/*
	 * Cycle through CPUs to check if the CPUs stay synchronized
	 * to each other.
	 */
	next_cpu = cpumask_next(raw_smp_processor_id(), cpu_online_mask);
	if (next_cpu >= nr_cpu_ids)
		next_cpu = cpumask_first(cpu_online_mask);

	/*
	 * Arm timer if not already pending: could race with concurrent
	 * pair clocksource_stop_watchdog() clocksource_start_watchdog().
	 */
	if (!timer_pending(&watchdog_timer)) {
		watchdog_timer.expires += WATCHDOG_INTERVAL;
		add_timer_on(&watchdog_timer, next_cpu);
	}
out:
	spin_unlock(&watchdog_lock);
}

static inline void clocksource_start_watchdog(void)
{
	if (watchdog_running || !watchdog || list_empty(&watchdog_list))
		return;
	timer_setup(&watchdog_timer, clocksource_watchdog, 0);
	watchdog_timer.expires = jiffies + WATCHDOG_INTERVAL;
	add_timer_on(&watchdog_timer, cpumask_first(cpu_online_mask));
	watchdog_running = 1;
}

static inline void clocksource_stop_watchdog(void)
{
	if (!watchdog_running || (watchdog && !list_empty(&watchdog_list)))
		return;
	del_timer(&watchdog_timer);
	watchdog_running = 0;
}

static inline void clocksource_reset_watchdog(void)
{
	struct clocksource *cs;

	list_for_each_entry(cs, &watchdog_list, wd_list)
		cs->flags &= ~CLOCK_SOURCE_WATCHDOG;
}

static void clocksource_resume_watchdog(void)
{
	atomic_inc(&watchdog_reset_pending);
}

static void clocksource_enqueue_watchdog(struct clocksource *cs)
{
	INIT_LIST_HEAD(&cs->wd_list);

	if (cs->flags & CLOCK_SOURCE_MUST_VERIFY) {
		/* cs is a clocksource to be watched. */
		list_add(&cs->wd_list, &watchdog_list);
		cs->flags &= ~CLOCK_SOURCE_WATCHDOG;
	} else {
		/* cs is a watchdog. */
		if (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS)
			cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;
	}
}

static void clocksource_select_watchdog(bool fallback)
{
	struct clocksource *cs, *old_wd;
	unsigned long flags;

	spin_lock_irqsave(&watchdog_lock, flags);
	/* save current watchdog */
	old_wd = watchdog;
	if (fallback)
		watchdog = NULL;

	list_for_each_entry(cs, &clocksource_list, list) {
		/* cs is a clocksource to be watched. */
		if (cs->flags & CLOCK_SOURCE_MUST_VERIFY)
			continue;

		/* Skip current if we were requested for a fallback. */
		if (fallback && cs == old_wd)
			continue;

		/* Pick the best watchdog. */
		if (!watchdog || cs->rating > watchdog->rating)
			watchdog = cs;
	}
	/* If we failed to find a fallback restore the old one. */
	if (!watchdog)
		watchdog = old_wd;

	/* If we changed the watchdog we need to reset cycles. */
	if (watchdog != old_wd)
		clocksource_reset_watchdog();

	/* Check if the watchdog timer needs to be started. */
	clocksource_start_watchdog();
	spin_unlock_irqrestore(&watchdog_lock, flags);
}

static void clocksource_dequeue_watchdog(struct clocksource *cs)
{
	if (cs != watchdog) {
		if (cs->flags & CLOCK_SOURCE_MUST_VERIFY) {
			/* cs is a watched clocksource. */
			list_del_init(&cs->wd_list);
			/* Check if the watchdog timer needs to be stopped. */
			clocksource_stop_watchdog();
		}
	}
}

static int __clocksource_watchdog_kthread(void)
{
	struct clocksource *cs, *tmp;
	unsigned long flags;
	int select = 0;

	/* Do any required per-CPU skew verification. */
	if (curr_clocksource &&
	    curr_clocksource->flags & CLOCK_SOURCE_UNSTABLE &&
	    curr_clocksource->flags & CLOCK_SOURCE_VERIFY_PERCPU)
		clocksource_verify_percpu(curr_clocksource);

	spin_lock_irqsave(&watchdog_lock, flags);
	list_for_each_entry_safe(cs, tmp, &watchdog_list, wd_list) {
		if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
			list_del_init(&cs->wd_list);
			__clocksource_change_rating(cs, 0);
			select = 1;
		}
		if (cs->flags & CLOCK_SOURCE_RESELECT) {
			cs->flags &= ~CLOCK_SOURCE_RESELECT;
			select = 1;
		}
	}
	/* Check if the watchdog timer needs to be stopped. */
	clocksource_stop_watchdog();
	spin_unlock_irqrestore(&watchdog_lock, flags);

	return select;
}

static int clocksource_watchdog_kthread(void *data)
{
	mutex_lock(&clocksource_mutex);
	if (__clocksource_watchdog_kthread())
		clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}

static bool clocksource_is_watchdog(struct clocksource *cs)
{
	return cs == watchdog;
}

#else /* CONFIG_CLOCKSOURCE_WATCHDOG */

static void clocksource_enqueue_watchdog(struct clocksource *cs)
{
	if (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS)
		cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;
}

static void clocksource_select_watchdog(bool fallback) { }
static inline void clocksource_dequeue_watchdog(struct clocksource *cs) { }
static inline void clocksource_resume_watchdog(void) { }
static inline int __clocksource_watchdog_kthread(void) { return 0; }
static bool clocksource_is_watchdog(struct clocksource *cs) { return false; }
void clocksource_mark_unstable(struct clocksource *cs) { }

static inline void clocksource_watchdog_lock(unsigned long *flags) { }
static inline void clocksource_watchdog_unlock(unsigned long *flags) { }

#endif /* CONFIG_CLOCKSOURCE_WATCHDOG */

static bool clocksource_is_suspend(struct clocksource *cs)
{
	return cs == suspend_clocksource;
}

static void __clocksource_suspend_select(struct clocksource *cs)
{
	/*
	 * Skip the clocksource which will be stopped in suspend state.
	 */
	if (!(cs->flags & CLOCK_SOURCE_SUSPEND_NONSTOP))
		return;

	/*
	 * The nonstop clocksource can be selected as the suspend clocksource to
	 * calculate the suspend time, so it should not supply suspend/resume
	 * interfaces to suspend the nonstop clocksource when system suspends.
	 */
	if (cs->suspend || cs->resume) {
		pr_warn("Nonstop clocksource %s should not supply suspend/resume interfaces\n",
			cs->name);
	}

	/* Pick the best rating. */
	if (!suspend_clocksource || cs->rating > suspend_clocksource->rating)
		suspend_clocksource = cs;
}

/**
 * clocksource_suspend_select - Select the best clocksource for suspend timing
 * @fallback:	if select a fallback clocksource
 */
static void clocksource_suspend_select(bool fallback)
{
	struct clocksource *cs, *old_suspend;

	old_suspend = suspend_clocksource;
	if (fallback)
		suspend_clocksource = NULL;

	list_for_each_entry(cs, &clocksource_list, list) {
		/* Skip current if we were requested for a fallback. */
		if (fallback && cs == old_suspend)
			continue;

		__clocksource_suspend_select(cs);
	}
}

/**
 * clocksource_start_suspend_timing - Start measuring the suspend timing
 * @cs:			current clocksource from timekeeping
 * @start_cycles:	current cycles from timekeeping
 *
 * This function will save the start cycle values of suspend timer to calculate
 * the suspend time when resuming system.
 *
 * This function is called late in the suspend process from timekeeping_suspend(),
 * that means processes are frozen, non-boot cpus and interrupts are disabled
 * now. It is therefore possible to start the suspend timer without taking the
 * clocksource mutex.
 */
void clocksource_start_suspend_timing(struct clocksource *cs, u64 start_cycles)
{
	if (!suspend_clocksource)
		return;

	/*
	 * If current clocksource is the suspend timer, we should use the
	 * tkr_mono.cycle_last value as suspend_start to avoid same reading
	 * from suspend timer.
	 */
	if (clocksource_is_suspend(cs)) {
		suspend_start = start_cycles;
		return;
	}

	if (suspend_clocksource->enable &&
	    suspend_clocksource->enable(suspend_clocksource)) {
		pr_warn_once("Failed to enable the non-suspend-able clocksource.\n");
		return;
	}

	suspend_start = suspend_clocksource->read(suspend_clocksource);
}

/**
 * clocksource_stop_suspend_timing - Stop measuring the suspend timing
 * @cs:		current clocksource from timekeeping
 * @cycle_now:	current cycles from timekeeping
 *
 * This function will calculate the suspend time from suspend timer.
 *
 * Returns nanoseconds since suspend started, 0 if no usable suspend clocksource.
 *
 * This function is called early in the resume process from timekeeping_resume(),
 * that means there is only one cpu, no processes are running and the interrupts
 * are disabled. It is therefore possible to stop the suspend timer without
 * taking the clocksource mutex.
 */
u64 clocksource_stop_suspend_timing(struct clocksource *cs, u64 cycle_now)
{
	u64 now, delta, nsec = 0;

	if (!suspend_clocksource)
		return 0;

	/*
	 * If current clocksource is the suspend timer, we should use the
	 * tkr_mono.cycle_last value from timekeeping as current cycle to
	 * avoid same reading from suspend timer.
	 */
	if (clocksource_is_suspend(cs))
		now = cycle_now;
	else
		now = suspend_clocksource->read(suspend_clocksource);

	if (now > suspend_start) {
		delta = clocksource_delta(now, suspend_start,
					  suspend_clocksource->mask);
		nsec = mul_u64_u32_shr(delta, suspend_clocksource->mult,
				       suspend_clocksource->shift);
	}

	/*
	 * Disable the suspend timer to save power if current clocksource is
	 * not the suspend timer.
	 */
	if (!clocksource_is_suspend(cs) && suspend_clocksource->disable)
		suspend_clocksource->disable(suspend_clocksource);

	return nsec;
}

/**
 * clocksource_suspend - suspend the clocksource(s)
 */
void clocksource_suspend(void)
{
	struct clocksource *cs;

	list_for_each_entry_reverse(cs, &clocksource_list, list)
		if (cs->suspend)
			cs->suspend(cs);
}

/**
 * clocksource_resume - resume the clocksource(s)
 */
void clocksource_resume(void)
{
	struct clocksource *cs;

	list_for_each_entry(cs, &clocksource_list, list)
		if (cs->resume)
			cs->resume(cs);

	clocksource_resume_watchdog();
}

/**
 * clocksource_touch_watchdog - Update watchdog
 *
 * Update the watchdog after exception contexts such as kgdb so as not
 * to incorrectly trip the watchdog. This might fail when the kernel
 * was stopped in code which holds watchdog_lock.
 */
void clocksource_touch_watchdog(void)
{
	clocksource_resume_watchdog();
}

/**
 * clocksource_max_adjustment- Returns max adjustment amount
 * @cs:         Pointer to clocksource
 *
 */
/*
 * IAMROOT, 2022.08.27:
 * - mult의 0.11배값을 return.
 */
static u32 clocksource_max_adjustment(struct clocksource *cs)
{
	u64 ret;
	/*
	 * We won't try to correct for more than 11% adjustments (110,000 ppm),
	 */
	ret = (u64)cs->mult * 11;
	do_div(ret,100);
	return (u32)ret;
}

/**
 * clocks_calc_max_nsecs - Returns maximum nanoseconds that can be converted
 * @mult:	cycle to nanosecond multiplier
 * @shift:	cycle to nanosecond divisor (power of two)
 * @maxadj:	maximum adjustment value to mult (~11%)
 * @mask:	bitmask for two's complement subtraction of non 64 bit counters
 * @max_cyc:	maximum cycle value before potential overflow (does not include
 *		any safety margin)
 *
 * NOTE: This function includes a safety margin of 50%, in other words, we
 * return half the number of nanoseconds the hardware counter can technically
 * cover. This is done so that we can potentially detect problems caused by
 * delayed timers or bad hardware, which might result in time intervals that
 * are larger than what the math used can handle without overflows.
 */

/*
 * IAMROOT, 2022.08.27:
 * - papago
 *   이 함수에는 50%의 safety margin가 포함됩니다. 즉, 하드웨어 카운터가 기술적으로
 *   처리할 수 있는 나노초 수의 절반을 반환합니다. 이는 지연된 타이머 또는 잘못된
 *   하드웨어로 인해 발생하는 문제를 잠재적으로 감지할 수 있도록 하기 위해 수행되며,
 *   이로 인해 사용된 수학이 오버플로 없이 처리할 수 있는 것보다 더 큰 시간 간격이
 *   발생할 수 있습니다.
 */
/*
 * IAMROOT, 2022.08.27:
 * - overlfow가 발생할수있는 제한을 구한다.
 */
u64 clocks_calc_max_nsecs(u32 mult, u32 shift, u32 maxadj, u64 mask, u64 *max_cyc)
{
	u64 max_nsecs, max_cycles;

	/*
	 * Calculate the maximum number of cycles that we can pass to the
	 * cyc2ns() function without overflowing a 64-bit result.
	 */
/*
 * IAMROOT, 2022.08.27:
 * - max_cycles = UNLLONG_MAX / (mult + maxadj)
 */
	max_cycles = ULLONG_MAX;
	do_div(max_cycles, mult+maxadj);

	/*
	 * The actual maximum number of cycles we can defer the clocksource is
	 * determined by the minimum of max_cycles and mask.
	 * Note: Here we subtract the maxadj to make sure we don't sleep for
	 * too long if there's a large negative adjustment.
	 */
/*
 * IAMROOT, 2022.08.27:
 * - papago
 *   클럭 소스를 연기할 수 있는 실제 최대 사이클 수는 max_cycles와
 *   마스크의 최소값에 의해 결정됩니다.
 *   note:
 *   여기서 우리는 큰 음수 조정이 있는 경우 너무 오랫동안 잠을 자지
 *   않도록 maxadj를 뺍니다. 
 *
 * - mask값(ex. arm arch timer CLOCKSOURCE_MASK(56)과 min 비교를 한다.
 *   즉 mask값보다 높은 값을 사용하지 않겟다는것.
 */
	max_cycles = min(max_cycles, mask);

/*
 * IAMROOT, 2022.08.27:
 * - max_cyc값에 해당하는 max_nsecs값을 구한다
 */
	max_nsecs = clocksource_cyc2ns(max_cycles, mult - maxadj, shift);

	/* return the max_cycles value as well if requested */
	if (max_cyc)
		*max_cyc = max_cycles;

	/* Return 50% of the actual maximum, so we can detect bad values */
	max_nsecs >>= 1;

	return max_nsecs;
}

/**
 * clocksource_update_max_deferment - Updates the clocksource max_idle_ns & max_cycles
 * @cs:         Pointer to clocksource to be updated
 *
 */
/*
 * IAMROOT, 2022.08.27:
 * - overflow당하지 않을 cycle값과 ns값을 구한다.
 */
static inline void clocksource_update_max_deferment(struct clocksource *cs)
{
	cs->max_idle_ns = clocks_calc_max_nsecs(cs->mult, cs->shift,
						cs->maxadj, cs->mask,
						&cs->max_cycles);
}

static struct clocksource *clocksource_find_best(bool oneshot, bool skipcur)
{
	struct clocksource *cs;

	if (!finished_booting || list_empty(&clocksource_list))
		return NULL;

	/*
	 * We pick the clocksource with the highest rating. If oneshot
	 * mode is active, we pick the highres valid clocksource with
	 * the best rating.
	 */
	list_for_each_entry(cs, &clocksource_list, list) {
		if (skipcur && cs == curr_clocksource)
			continue;
		if (oneshot && !(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES))
			continue;
		return cs;
	}
	return NULL;
}

static void __clocksource_select(bool skipcur)
{
	bool oneshot = tick_oneshot_mode_active();
	struct clocksource *best, *cs;

	/* Find the best suitable clocksource */
	best = clocksource_find_best(oneshot, skipcur);
	if (!best)
		return;

	if (!strlen(override_name))
		goto found;

	/* Check for the override clocksource. */
	list_for_each_entry(cs, &clocksource_list, list) {
		if (skipcur && cs == curr_clocksource)
			continue;
		if (strcmp(cs->name, override_name) != 0)
			continue;
		/*
		 * Check to make sure we don't switch to a non-highres
		 * capable clocksource if the tick code is in oneshot
		 * mode (highres or nohz)
		 */
		if (!(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES) && oneshot) {
			/* Override clocksource cannot be used. */
			if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
				pr_warn("Override clocksource %s is unstable and not HRT compatible - cannot switch while in HRT/NOHZ mode\n",
					cs->name);
				override_name[0] = 0;
			} else {
				/*
				 * The override cannot be currently verified.
				 * Deferring to let the watchdog check.
				 */
				pr_info("Override clocksource %s is not currently HRT compatible - deferring\n",
					cs->name);
			}
		} else
			/* Override clocksource can be used. */
			best = cs;
		break;
	}

found:
	if (curr_clocksource != best && !timekeeping_notify(best)) {
		pr_info("Switched to clocksource %s\n", best->name);
		curr_clocksource = best;
	}
}

/**
 * clocksource_select - Select the best clocksource available
 *
 * Private function. Must hold clocksource_mutex when called.
 *
 * Select the clocksource with the best rating, or the clocksource,
 * which is selected by userspace override.
 */
static void clocksource_select(void)
{
	__clocksource_select(false);
}

static void clocksource_select_fallback(void)
{
	__clocksource_select(true);
}

/*
 * clocksource_done_booting - Called near the end of core bootup
 *
 * Hack to avoid lots of clocksource churn at boot time.
 * We use fs_initcall because we want this to start before
 * device_initcall but after subsys_initcall.
 */
static int __init clocksource_done_booting(void)
{
	mutex_lock(&clocksource_mutex);
	curr_clocksource = clocksource_default_clock();
	finished_booting = 1;
	/*
	 * Run the watchdog first to eliminate unstable clock sources
	 */
	__clocksource_watchdog_kthread();
	clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}
fs_initcall(clocksource_done_booting);

/*
 * Enqueue the clocksource sorted by rating
 */
static void clocksource_enqueue(struct clocksource *cs)
{
	struct list_head *entry = &clocksource_list;
	struct clocksource *tmp;

	list_for_each_entry(tmp, &clocksource_list, list) {
		/* Keep track of the place, where to insert */
		if (tmp->rating < cs->rating)
			break;
		entry = &tmp->list;
	}
	list_add(&cs->list, entry);
}

/**
 * __clocksource_update_freq_scale - Used update clocksource with new freq
 * @cs:		clocksource to be registered
 * @scale:	Scale factor multiplied against freq to get clocksource hz
 * @freq:	clocksource frequency (cycles per second) divided by scale
 *
 * This should only be called from the clocksource->enable() method.
 *
 * This *SHOULD NOT* be called directly! Please use the
 * __clocksource_update_freq_hz() or __clocksource_update_freq_khz() helper
 * functions.
 */
/*
 * IAMROOT, 2022.08.27:
 * - @cs에 대한 mult, shift값을 구하고 margin, adj, max_idle_ns,
 *   max_cycles 값을 구한다.
 */
void __clocksource_update_freq_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	u64 sec;

	/*
	 * Default clocksources are *special* and self-define their mult/shift.
	 * But, you're not special, so you should specify a freq value.
	 */
	if (freq) {
		/*
		 * Calc the maximum number of seconds which we can run before
		 * wrapping around. For clocksources which have a mask > 32-bit
		 * we need to limit the max sleep time to have a good
		 * conversion precision. 10 minutes is still a reasonable
		 * amount. That results in a shift value of 24 for a
		 * clocksource with mask >= 40-bit and f >= 4GHz. That maps to
		 * ~ 0.06ppm granularity for NTP.
		 */

/*
 * IAMROOT, 2022.08.20:
 * - papago
 *   둘러싸기 전에 실행할 수 있는 최대 시간(초)을 계산합니다. 마스크가 32비트보다 큰 클록
 *   소스의 경우 좋은 변환 정밀도를 갖기 위해 최대 절전 시간을 제한해야 합니다. 10분은 여전히
 *   적당한 양입니다. 그 결과 마스크 >= 40비트 및 f >= 4GHz인 클록 소스에 대해 시프트 값이 24가
 *   됩니다. 이는 NTP의 경우 ~ 0.06ppm 단위로 매핑됩니다.
 */
		sec = cs->mask;
/*
 * IAMROOT, 2022.08.20:
 * - sec = sec / freq / scale
 * ex) cs->mask = CLOCKSOURCE_MASK(56), freq = 19200000, scale = 1
 * sec = 0xff_ffff_ffff_ffff / 19200000 = 3752999689 => clamp => 600
 *
 * clamp로 10분을 한계로 두고 정확도를 높이는 개념.
 */
		do_div(sec, freq);
		do_div(sec, scale);
		if (!sec)
			sec = 1;
		else if (sec > 600 && cs->mask > UINT_MAX)
			sec = 600;

/*
 * IAMROOT, 2022.08.20:
 * - sec * scale시간을 한계로 freq -> NSEC_PER_SEC / scale이 되기 위한 mult, shift를 계산한다.
 * ex) clocksource_counter에서 mask = CLOCKSOURCE_MASK(56)
 * ex) cs->mask = CLOCKSOURCE_MASK(56), freq = 19200000, scale = 1, sec = 600
 */
		clocks_calc_mult_shift(&cs->mult, &cs->shift, freq,
				       NSEC_PER_SEC / scale, sec * scale);
	}

	/*
	 * If the uncertainty margin is not specified, calculate it.
	 * If both scale and freq are non-zero, calculate the clock
	 * period, but bound below at 2*WATCHDOG_MAX_SKEW.  However,
	 * if either of scale or freq is zero, be very conservative and
	 * take the tens-of-milliseconds WATCHDOG_THRESHOLD value for the
	 * uncertainty margin.  Allow stupidly small uncertainty margins
	 * to be specified by the caller for testing purposes, but warn
	 * to discourage production use of this capability.
	 */
/*
 * IAMROOT, 2022.08.27:
 * - papgo
 *   불확실성 마진이 지정되지 않은 경우 계산합니다. scale과 freq가 모두 0이 아닌 경우 클록 주기를
 *   계산하지만 2*WATCHDOG_MAX_SKEW에서 아래로 제한됩니다. 그러나 scale 또는 freq 중 하나가 0이면
 *   매우 보수적이며 불확실성 마진에 대해 수십 밀리초 WATCHDOG_THRESHOLD 값을 사용합니다. 테스트
 *   목적으로 호출자가 지정하는 어리석게 작은 불확실성 마진을 허용하지만 이 기능의 프로덕션
 *   사용을 권장하지 않도록 경고합니다.
 */
/*
 * IAMROOT, 2022.08.27:
 * - cs->uncertainty_margin이 0인 경우
 *   scale, freq둘다 0이 아니면 계산. 그렇지 않은경우 고정값 사용.
 * - 1초에대한 margin값을 정의 하는것.
 *   ex) margin이 0.1이라고 하면 0.9 ~ 1.1 사이값이 오면 1초라고 생각한다는것.
 */
	if (scale && freq && !cs->uncertainty_margin) {
		cs->uncertainty_margin = NSEC_PER_SEC / (scale * freq);
		if (cs->uncertainty_margin < 2 * WATCHDOG_MAX_SKEW)
			cs->uncertainty_margin = 2 * WATCHDOG_MAX_SKEW;
	} else if (!cs->uncertainty_margin) {
		cs->uncertainty_margin = WATCHDOG_THRESHOLD;
	}
	WARN_ON_ONCE(cs->uncertainty_margin < 2 * WATCHDOG_MAX_SKEW);

	/*
	 * Ensure clocksources that have large 'mult' values don't overflow
	 * when adjusted.
	 */
	cs->maxadj = clocksource_max_adjustment(cs);
/*
 * IAMROOT, 2022.08.27:
 * - adj값을 계산하고 계산된 mult에 적용해봐서 overflow, underflow가 안될때까지
 *   재조정한다.
 */
	while (freq && ((cs->mult + cs->maxadj < cs->mult)
		|| (cs->mult - cs->maxadj > cs->mult))) {
		cs->mult >>= 1;
		cs->shift--;
		cs->maxadj = clocksource_max_adjustment(cs);
	}

	/*
	 * Only warn for *special* clocksources that self-define
	 * their mult/shift values and don't specify a freq.
	 */
	WARN_ONCE(cs->mult + cs->maxadj < cs->mult,
		"timekeeping: Clocksource %s might overflow on 11%% adjustment\n",
		cs->name);


/*
 * IAMROOT, 2022.08.27:
 * - overflow당하지 않은 제한값(cs->max_idle_ns)을 구한다.
 */
	clocksource_update_max_deferment(cs);

/*
 * IAMROOT, 2022.08.27:
 * - ex) x64 ubuntu
 *   clocksource: tsc: mask: 0xffffffffffffffff
 *   max_cycles: 0x6d581b92771, max_idle_ns: 881590605997 ns
 * - 위 예제로 봤을때 600초를 제한으로 했지만, 881초까지 여유를 두는것이
 *   확인된다.
 *   881초도 50%줄인 숫자(max_idle_ns)이고, 실제로는 max_cycles에 있는
 *   881 * 2초 만큼 버티게된다.
 */
	pr_info("%s: mask: 0x%llx max_cycles: 0x%llx, max_idle_ns: %lld ns\n",
		cs->name, cs->mask, cs->max_cycles, cs->max_idle_ns);
}
EXPORT_SYMBOL_GPL(__clocksource_update_freq_scale);

/**
 * __clocksource_register_scale - Used to install new clocksources
 * @cs:		clocksource to be registered
 * @scale:	Scale factor multiplied against freq to get clocksource hz
 * @freq:	clocksource frequency (cycles per second) divided by scale
 *
 * Returns -EBUSY if registration fails, zero otherwise.
 *
 * This *SHOULD NOT* be called directly! Please use the
 * clocksource_register_hz() or clocksource_register_khz helper functions.
 */
/*
 * IAMROOT, 2022.08.20:
 * - scale * freq = 실제 freq
 */
int __clocksource_register_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	unsigned long flags;

	clocksource_arch_init(cs);

	if (WARN_ON_ONCE((unsigned int)cs->id >= CSID_MAX))
		cs->id = CSID_GENERIC;

/*
 * IAMROOT, 2022.08.20:
 * ex) VDSO_CLOCKMODE_NONE, VDSO_CLOCKMODE_ARCHTIMER_NOCOMPAT
 */
	if (cs->vdso_clock_mode < 0 ||
	    cs->vdso_clock_mode >= VDSO_CLOCKMODE_MAX) {
		pr_warn("clocksource %s registered with invalid VDSO mode %d. Disabling VDSO support.\n",
			cs->name, cs->vdso_clock_mode);
		cs->vdso_clock_mode = VDSO_CLOCKMODE_NONE;
	}

	/* Initialize mult/shift and max_idle_ns */
	__clocksource_update_freq_scale(cs, scale, freq);

	/* Add clocksource to the clocksource list */
	mutex_lock(&clocksource_mutex);

	clocksource_watchdog_lock(&flags);
	clocksource_enqueue(cs);
	clocksource_enqueue_watchdog(cs);
	clocksource_watchdog_unlock(&flags);

	clocksource_select();
	clocksource_select_watchdog(false);
	__clocksource_suspend_select(cs);
	mutex_unlock(&clocksource_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(__clocksource_register_scale);

static void __clocksource_change_rating(struct clocksource *cs, int rating)
{
	list_del(&cs->list);
	cs->rating = rating;
	clocksource_enqueue(cs);
}

/**
 * clocksource_change_rating - Change the rating of a registered clocksource
 * @cs:		clocksource to be changed
 * @rating:	new rating
 */
void clocksource_change_rating(struct clocksource *cs, int rating)
{
	unsigned long flags;

	mutex_lock(&clocksource_mutex);
	clocksource_watchdog_lock(&flags);
	__clocksource_change_rating(cs, rating);
	clocksource_watchdog_unlock(&flags);

	clocksource_select();
	clocksource_select_watchdog(false);
	clocksource_suspend_select(false);
	mutex_unlock(&clocksource_mutex);
}
EXPORT_SYMBOL(clocksource_change_rating);

/*
 * Unbind clocksource @cs. Called with clocksource_mutex held
 */
static int clocksource_unbind(struct clocksource *cs)
{
	unsigned long flags;

	if (clocksource_is_watchdog(cs)) {
		/* Select and try to install a replacement watchdog. */
		clocksource_select_watchdog(true);
		if (clocksource_is_watchdog(cs))
			return -EBUSY;
	}

	if (cs == curr_clocksource) {
		/* Select and try to install a replacement clock source */
		clocksource_select_fallback();
		if (curr_clocksource == cs)
			return -EBUSY;
	}

	if (clocksource_is_suspend(cs)) {
		/*
		 * Select and try to install a replacement suspend clocksource.
		 * If no replacement suspend clocksource, we will just let the
		 * clocksource go and have no suspend clocksource.
		 */
		clocksource_suspend_select(true);
	}

	clocksource_watchdog_lock(&flags);
	clocksource_dequeue_watchdog(cs);
	list_del_init(&cs->list);
	clocksource_watchdog_unlock(&flags);

	return 0;
}

/**
 * clocksource_unregister - remove a registered clocksource
 * @cs:	clocksource to be unregistered
 */
int clocksource_unregister(struct clocksource *cs)
{
	int ret = 0;

	mutex_lock(&clocksource_mutex);
	if (!list_empty(&cs->list))
		ret = clocksource_unbind(cs);
	mutex_unlock(&clocksource_mutex);
	return ret;
}
EXPORT_SYMBOL(clocksource_unregister);

#ifdef CONFIG_SYSFS
/**
 * current_clocksource_show - sysfs interface for current clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	char buffer to be filled with clocksource list
 *
 * Provides sysfs interface for listing current clocksource.
 */
static ssize_t current_clocksource_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t count = 0;

	mutex_lock(&clocksource_mutex);
	count = snprintf(buf, PAGE_SIZE, "%s\n", curr_clocksource->name);
	mutex_unlock(&clocksource_mutex);

	return count;
}

ssize_t sysfs_get_uname(const char *buf, char *dst, size_t cnt)
{
	size_t ret = cnt;

	/* strings from sysfs write are not 0 terminated! */
	if (!cnt || cnt >= CS_NAME_LEN)
		return -EINVAL;

	/* strip of \n: */
	if (buf[cnt-1] == '\n')
		cnt--;
	if (cnt > 0)
		memcpy(dst, buf, cnt);
	dst[cnt] = 0;
	return ret;
}

/**
 * current_clocksource_store - interface for manually overriding clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	name of override clocksource
 * @count:	length of buffer
 *
 * Takes input from sysfs interface for manually overriding the default
 * clocksource selection.
 */
static ssize_t current_clocksource_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	ssize_t ret;

	mutex_lock(&clocksource_mutex);

	ret = sysfs_get_uname(buf, override_name, count);
	if (ret >= 0)
		clocksource_select();

	mutex_unlock(&clocksource_mutex);

	return ret;
}
static DEVICE_ATTR_RW(current_clocksource);

/**
 * unbind_clocksource_store - interface for manually unbinding clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	unused
 * @count:	length of buffer
 *
 * Takes input from sysfs interface for manually unbinding a clocksource.
 */
static ssize_t unbind_clocksource_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct clocksource *cs;
	char name[CS_NAME_LEN];
	ssize_t ret;

	ret = sysfs_get_uname(buf, name, count);
	if (ret < 0)
		return ret;

	ret = -ENODEV;
	mutex_lock(&clocksource_mutex);
	list_for_each_entry(cs, &clocksource_list, list) {
		if (strcmp(cs->name, name))
			continue;
		ret = clocksource_unbind(cs);
		break;
	}
	mutex_unlock(&clocksource_mutex);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(unbind_clocksource);

/**
 * available_clocksource_show - sysfs interface for listing clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	char buffer to be filled with clocksource list
 *
 * Provides sysfs interface for listing registered clocksources
 */
static ssize_t available_clocksource_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct clocksource *src;
	ssize_t count = 0;

	mutex_lock(&clocksource_mutex);
	list_for_each_entry(src, &clocksource_list, list) {
		/*
		 * Don't show non-HRES clocksource if the tick code is
		 * in one shot mode (highres=on or nohz=on)
		 */
		if (!tick_oneshot_mode_active() ||
		    (src->flags & CLOCK_SOURCE_VALID_FOR_HRES))
			count += snprintf(buf + count,
				  max((ssize_t)PAGE_SIZE - count, (ssize_t)0),
				  "%s ", src->name);
	}
	mutex_unlock(&clocksource_mutex);

	count += snprintf(buf + count,
			  max((ssize_t)PAGE_SIZE - count, (ssize_t)0), "\n");

	return count;
}
static DEVICE_ATTR_RO(available_clocksource);

static struct attribute *clocksource_attrs[] = {
	&dev_attr_current_clocksource.attr,
	&dev_attr_unbind_clocksource.attr,
	&dev_attr_available_clocksource.attr,
	NULL
};
ATTRIBUTE_GROUPS(clocksource);

static struct bus_type clocksource_subsys = {
	.name = "clocksource",
	.dev_name = "clocksource",
};

static struct device device_clocksource = {
	.id	= 0,
	.bus	= &clocksource_subsys,
	.groups	= clocksource_groups,
};

static int __init init_clocksource_sysfs(void)
{
	int error = subsys_system_register(&clocksource_subsys, NULL);

	if (!error)
		error = device_register(&device_clocksource);

	return error;
}

device_initcall(init_clocksource_sysfs);
#endif /* CONFIG_SYSFS */

/**
 * boot_override_clocksource - boot clock override
 * @str:	override name
 *
 * Takes a clocksource= boot argument and uses it
 * as the clocksource override name.
 */
static int __init boot_override_clocksource(char* str)
{
	mutex_lock(&clocksource_mutex);
	if (str)
		strlcpy(override_name, str, sizeof(override_name));
	mutex_unlock(&clocksource_mutex);
	return 1;
}

__setup("clocksource=", boot_override_clocksource);

/**
 * boot_override_clock - Compatibility layer for deprecated boot option
 * @str:	override name
 *
 * DEPRECATED! Takes a clock= boot argument and uses it
 * as the clocksource override name
 */
static int __init boot_override_clock(char* str)
{
	if (!strcmp(str, "pmtmr")) {
		pr_warn("clock=pmtmr is deprecated - use clocksource=acpi_pm\n");
		return boot_override_clocksource("acpi_pm");
	}
	pr_warn("clock= boot option is deprecated - use clocksource=xyz\n");
	return boot_override_clocksource(str);
}

__setup("clock=", boot_override_clock);
