// SPDX-License-Identifier: GPL-2.0
/*
 *  Kernel internal timers
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-01-28  Modified by Finn Arne Gangstad to make timers scale better.
 *
 *  1997-09-10  Updated NTP code according to technical memorandum Jan '96
 *              "A Kernel Model for Precision Timekeeping" by Dave Mills
 *  1998-12-24  Fixed a xtime SMP race (we need the xtime_lock rw spinlock to
 *              serialize accesses to xtime/lost_ticks).
 *                              Copyright (C) 1998  Andrea Arcangeli
 *  1999-03-10  Improved NTP compatibility by Ulrich Windl
 *  2002-05-31	Move sys_sysinfo here and make its locking sane, Robert Love
 *  2000-10-05  Implemented scalable SMP per-CPU timer handling.
 *                              Copyright (C) 2000, 2001, 2002  Ingo Molnar
 *              Designed by David S. Miller, Alexey Kuznetsov and Ingo Molnar
 */

#include <linux/kernel_stat.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/pid_namespace.h>
#include <linux/notifier.h>
#include <linux/thread_info.h>
#include <linux/time.h>
#include <linux/jiffies.h>
#include <linux/posix-timers.h>
#include <linux/cpu.h>
#include <linux/syscalls.h>
#include <linux/delay.h>
#include <linux/tick.h>
#include <linux/kallsyms.h>
#include <linux/irq_work.h>
#include <linux/sched/signal.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/nohz.h>
#include <linux/sched/debug.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/random.h>

#include <linux/uaccess.h>
#include <asm/unistd.h>
#include <asm/div64.h>
#include <asm/timex.h>
#include <asm/io.h>

#include "tick-internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/timer.h>

/*
 * IAMROOT, 2022.08.27:
 * - jiffies_64 정의.
 */
__visible u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;

EXPORT_SYMBOL(jiffies_64);

/*
 * The timer wheel has LVL_DEPTH array levels. Each level provides an array of
 * LVL_SIZE buckets. Each level is driven by its own clock and therefor each
 * level has a different granularity.
 *
 * The level granularity is:		LVL_CLK_DIV ^ lvl
 * The level clock frequency is:	HZ / (LVL_CLK_DIV ^ level)
 *
 * The array level of a newly armed timer depends on the relative expiry
 * time. The farther the expiry time is away the higher the array level and
 * therefor the granularity becomes.
 *
 * Contrary to the original timer wheel implementation, which aims for 'exact'
 * expiry of the timers, this implementation removes the need for recascading
 * the timers into the lower array levels. The previous 'classic' timer wheel
 * implementation of the kernel already violated the 'exact' expiry by adding
 * slack to the expiry time to provide batched expiration. The granularity
 * levels provide implicit batching.
 *
 * This is an optimization of the original timer wheel implementation for the
 * majority of the timer wheel use cases: timeouts. The vast majority of
 * timeout timers (networking, disk I/O ...) are canceled before expiry. If
 * the timeout expires it indicates that normal operation is disturbed, so it
 * does not matter much whether the timeout comes with a slight delay.
 *
 * The only exception to this are networking timers with a small expiry
 * time. They rely on the granularity. Those fit into the first wheel level,
 * which has HZ granularity.
 *
 * We don't have cascading anymore. timers with a expiry time above the
 * capacity of the last wheel level are force expired at the maximum timeout
 * value of the last wheel level. From data sampling we know that the maximum
 * value observed is 5 days (network connection tracking), so this should not
 * be an issue.
 *
 * The currently chosen array constants values are a good compromise between
 * array size and granularity.
 *
 * This results in the following granularity and range levels:
 *
 * HZ 1000 steps
 * Level Offset  Granularity            Range
 *  0      0         1 ms                0 ms -         63 ms
 *  1     64         8 ms               64 ms -        511 ms
 *  2    128        64 ms              512 ms -       4095 ms (512ms - ~4s)
 *  3    192       512 ms             4096 ms -      32767 ms (~4s - ~32s)
 *  4    256      4096 ms (~4s)      32768 ms -     262143 ms (~32s - ~4m)
 *  5    320     32768 ms (~32s)    262144 ms -    2097151 ms (~4m - ~34m)
 *  6    384    262144 ms (~4m)    2097152 ms -   16777215 ms (~34m - ~4h)
 *  7    448   2097152 ms (~34m)  16777216 ms -  134217727 ms (~4h - ~1d)
 *  8    512  16777216 ms (~4h)  134217728 ms - 1073741822 ms (~1d - ~12d)
 *
 * HZ  300
 * Level Offset  Granularity            Range
 *  0	   0         3 ms                0 ms -        210 ms
 *  1	  64        26 ms              213 ms -       1703 ms (213ms - ~1s)
 *  2	 128       213 ms             1706 ms -      13650 ms (~1s - ~13s)
 *  3	 192      1706 ms (~1s)      13653 ms -     109223 ms (~13s - ~1m)
 *  4	 256     13653 ms (~13s)    109226 ms -     873810 ms (~1m - ~14m)
 *  5	 320    109226 ms (~1m)     873813 ms -    6990503 ms (~14m - ~1h)
 *  6	 384    873813 ms (~14m)   6990506 ms -   55924050 ms (~1h - ~15h)
 *  7	 448   6990506 ms (~1h)   55924053 ms -  447392423 ms (~15h - ~5d)
 *  8    512  55924053 ms (~15h) 447392426 ms - 3579139406 ms (~5d - ~41d)
 *
 * HZ  250
 * Level Offset  Granularity            Range
 *  0	   0         4 ms                0 ms -        255 ms
 *  1	  64        32 ms              256 ms -       2047 ms (256ms - ~2s)
 *  2	 128       256 ms             2048 ms -      16383 ms (~2s - ~16s)
 *  3	 192      2048 ms (~2s)      16384 ms -     131071 ms (~16s - ~2m)
 *  4	 256     16384 ms (~16s)    131072 ms -    1048575 ms (~2m - ~17m)
 *  5	 320    131072 ms (~2m)    1048576 ms -    8388607 ms (~17m - ~2h)
 *  6	 384   1048576 ms (~17m)   8388608 ms -   67108863 ms (~2h - ~18h)
 *  7	 448   8388608 ms (~2h)   67108864 ms -  536870911 ms (~18h - ~6d)
 *  8    512  67108864 ms (~18h) 536870912 ms - 4294967288 ms (~6d - ~49d)
 *
 * HZ  100
 * Level Offset  Granularity            Range
 *  0	   0         10 ms               0 ms -        630 ms
 *  1	  64         80 ms             640 ms -       5110 ms (640ms - ~5s)
 *  2	 128        640 ms            5120 ms -      40950 ms (~5s - ~40s)
 *  3	 192       5120 ms (~5s)     40960 ms -     327670 ms (~40s - ~5m)
 *  4	 256      40960 ms (~40s)   327680 ms -    2621430 ms (~5m - ~43m)
 *  5	 320     327680 ms (~5m)   2621440 ms -   20971510 ms (~43m - ~5h)
 *  6	 384    2621440 ms (~43m) 20971520 ms -  167772150 ms (~5h - ~1d)
 *  7	 448   20971520 ms (~5h) 167772160 ms - 1342177270 ms (~1d - ~15d)
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   타이머 휠에는 LVL_DEPTH 배열 레벨이 있습니다. 각 레벨은 LVL_SIZE 버킷의
 *   배열을 제공합니다. 각 레벨은 자체 클록에 의해 구동되므로 각 레벨은 서로
 *   다른 세분성을 갖습니다.
 *
 * The level granularity is:		LVL_CLK_DIV ^ lvl
 * The level clock frequency is:	HZ / (LVL_CLK_DIV ^ level)
 *
 * 새로 무장한 타이머의 배열 수준은 상대 만료 시간에 따라 다릅니다. 만료 
 * 시간이 멀어질수록 어레이 수준이 높아지므로 입도가 높아집니다.
 *
 * 타이머의 '정확한' 만료를 목표로 하는 원래 타이머 휠 구현과 달리 이
 * 구현에서는 타이머를 하위 어레이 레벨로 다시 캐스케이딩할 필요가 없습니다.
 * 커널의 이전 '클래식' 타이머 휠 구현은 일괄 만료를 제공하기 위해 만료
 * 시간에 여유를 추가함으로써 이미 '정확한' 만료를 위반했습니다. 세분성
 * 수준은 암시적 일괄 처리를 제공합니다.
 *
 * 이것은 대부분의 타이머 휠 사용 사례에 대한 원래 타이머 휠 구현의
 * 최적화입니다.
 * 타임아웃. 대부분의 타임아웃 타이머(네트워킹, 디스크 I/O ...)는 만료되기
 * 전에 취소됩니다. 타임아웃이 만료되면 정상적인 작동이 방해받는 것을
 * 나타내므로 타임아웃이 약간 지연되어 오는지는 중요하지 않습니다.
 *
 * 이에 대한 유일한 예외는 만료 시간이 짧은 네트워킹 타이머입니다. 그들은
 * 세분성에 의존합니다. 그것들은 HZ 세분성을 가진 첫 번째 휠 레벨에 맞습니다.
 *
 * 우리는 더 이상 캐스케이딩이 없습니다. 만료 시간이 마지막 휠 레벨의 용량을
 * 초과하는 타이머는 마지막 휠 레벨의 최대 타임아웃 값에서 강제 만료됩니다.
 * 데이터 샘플링을 통해 관찰된 최대 값이 5일(네트워크 연결 추적)임을 알고
 * 있으므로 이는 문제가 되지 않습니다.
 *
 * 현재 선택된 배열 상수 값은 배열 크기와 세분성 사이의 적절한 절충안입니다.
 *
 * 그 결과 다음과 같은 세분성 및 범위 수준이 생성됩니다. 
 * (이 아래는 원본주석 참고)
 */

/* Clock divisor for the next level */
#define LVL_CLK_SHIFT	3
#define LVL_CLK_DIV	(1UL << LVL_CLK_SHIFT)
#define LVL_CLK_MASK	(LVL_CLK_DIV - 1)
#define LVL_SHIFT(n)	((n) * LVL_CLK_SHIFT)

/*
 * IAMROOT, 2022.09.03:
 * ex) 1000hz 기준.
 * n    LVL_SHIFT(n)  LVL_GRAN(n)
 * 0    0 * 3 = 0     1 << 0   = 1
 * 1    1 * 3 = 3     1 << 3   = 8
 * 2    2 * 3 = 6     1 << 6   = 64
 * 3    3 * 3 = 9     1 << 9   = 512
 * 4    4 * 3 = 12    1 << 12  = 4096
 * 5    5 * 3 = 15    1 << 15  = 32768
 * 6    6 * 3 = 18    1 << 18  = 262144
 * 7    7 * 3 = 21    1 << 21  = 2097152
 * 8    8 * 3 = 24    1 << 24  = 16777216
 */
#define LVL_GRAN(n)	(1UL << LVL_SHIFT(n))

/*
 * The time start value for each level to select the bucket at enqueue
 * time. We start from the last possible delta of the previous level
 * so that we can later add an extra LVL_GRAN(n) to n (see calc_index()).
 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   대기열에 넣을 때 버킷을 선택하기 위한 각 레벨의 시간 시작 값입니다. 이전
 *   레벨의 마지막 가능한 델타에서 시작하여 나중에 LVL_GRAN(n)을 n에 추가할 수
 *   있습니다(calc_index() 참조).
 *
 * n  (((n) - 1) * LVL_CLK_SHIFT) LEVL_START(n)    
 * 1  0                           0b111111 << 0 
 * 2  3                           0b111111 << 3    
 * 3  6                           0b111111 << 6
 * 4  9                           0b111111 << 9
 * 5  12                          0b111111 << 12
 * 6  15                          0b111111 << 15
 * 7  18                          0b111111 << 18
 * 8  21                          0b111111 << 21
 *
 */
#define LVL_START(n)	((LVL_SIZE - 1) << (((n) - 1) * LVL_CLK_SHIFT))

/* Size of each clock level */
#define LVL_BITS	6
#define LVL_SIZE	(1UL << LVL_BITS)
#define LVL_MASK	(LVL_SIZE - 1)
#define LVL_OFFS(n)	((n) * LVL_SIZE)

/* Level depth */
#if HZ > 100
# define LVL_DEPTH	9
# else
# define LVL_DEPTH	8
#endif

/* The cutoff (max. capacity of the wheel) */
#define WHEEL_TIMEOUT_CUTOFF	(LVL_START(LVL_DEPTH))
#define WHEEL_TIMEOUT_MAX	(WHEEL_TIMEOUT_CUTOFF - LVL_GRAN(LVL_DEPTH - 1))

/*
 * The resulting wheel size. If NOHZ is configured we allocate two
 * wheels so we have a separate storage for the deferrable timers.
 */
/*
 * IAMROOT, 2022.09.03:
 * - LVL_DEPTH == 9
 *   2^6 * 9 = 576
 *
 * - LVL_DEPTH == 8
 *   2^6 * 8 = 512
 */
#define WHEEL_SIZE	(LVL_SIZE * LVL_DEPTH)

#ifdef CONFIG_NO_HZ_COMMON
# define NR_BASES	2
# define BASE_STD	0
/*
 * IAMROOT, 2022.09.03:
 * - NOHZ용 (deffered 가능한).
 */
# define BASE_DEF	1
#else
# define NR_BASES	1
# define BASE_STD	0
# define BASE_DEF	0
#endif

struct timer_base {
	raw_spinlock_t		lock;
	struct timer_list	*running_timer;
#ifdef CONFIG_PREEMPT_RT
	spinlock_t		expiry_lock;
	atomic_t		timer_waiters;
#endif
	unsigned long		clk;
	unsigned long		next_expiry;
	unsigned int		cpu;
	bool			next_expiry_recalc;
	bool			is_idle;
	bool			timers_pending;
	DECLARE_BITMAP(pending_map, WHEEL_SIZE);
	struct hlist_head	vectors[WHEEL_SIZE];
} ____cacheline_aligned;

static DEFINE_PER_CPU(struct timer_base, timer_bases[NR_BASES]);

#ifdef CONFIG_NO_HZ_COMMON

static DEFINE_STATIC_KEY_FALSE(timers_nohz_active);
static DEFINE_MUTEX(timer_keys_mutex);

static void timer_update_keys(struct work_struct *work);
static DECLARE_WORK(timer_update_work, timer_update_keys);

#ifdef CONFIG_SMP
unsigned int sysctl_timer_migration = 1;

DEFINE_STATIC_KEY_FALSE(timers_migration_enabled);

static void timers_update_migration(void)
{
	if (sysctl_timer_migration && tick_nohz_active)
		static_branch_enable(&timers_migration_enabled);
	else
		static_branch_disable(&timers_migration_enabled);
}
#else
static inline void timers_update_migration(void) { }
#endif /* !CONFIG_SMP */

static void timer_update_keys(struct work_struct *work)
{
	mutex_lock(&timer_keys_mutex);
	timers_update_migration();
	static_branch_enable(&timers_nohz_active);
	mutex_unlock(&timer_keys_mutex);
}

/*
 * IAMROOT, 2022.12.03:
 * - timers_nohz_active,timers_migration_enabled enable.
 *   schedule이 동작하고나서부터 돌라는 사전 예약의 의미가 있다.
 *   (schedule이 동작안하고 있을때에는 nohz가 필요없기때문)
 */
void timers_update_nohz(void)
{
	schedule_work(&timer_update_work);
}

int timer_migration_handler(struct ctl_table *table, int write,
			    void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&timer_keys_mutex);
	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (!ret && write)
		timers_update_migration();
	mutex_unlock(&timer_keys_mutex);
	return ret;
}

static inline bool is_timers_nohz_active(void)
{
	return static_branch_unlikely(&timers_nohz_active);
}
#else
static inline bool is_timers_nohz_active(void) { return false; }
#endif /* NO_HZ_COMMON */

static unsigned long round_jiffies_common(unsigned long j, int cpu,
		bool force_up)
{
	int rem;
	unsigned long original = j;

	/*
	 * We don't want all cpus firing their timers at once hitting the
	 * same lock or cachelines, so we skew each extra cpu with an extra
	 * 3 jiffies. This 3 jiffies came originally from the mm/ code which
	 * already did this.
	 * The skew is done by adding 3*cpunr, then round, then subtract this
	 * extra offset again.
	 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   우리는 모든 CPU가 동일한 잠금 또는 캐시라인에 도달하는 타이머를
 *   한 번에 실행하는 것을 원하지 않으므로 추가 3jiffies로 각 추가 CPU를
 *   왜곡합니다. 이 3개의 jiffies는 이미 이 작업을 수행한 mm/ 코드에서
 *   원래 나온 것입니다.
 *   스큐는 3*cpunr을 더한 다음 반올림하고 이 추가 오프셋을 다시 빼서
 *   수행됩니다.
 */
	j += cpu * 3;

	rem = j % HZ;

	/*
	 * If the target jiffie is just after a whole second (which can happen
	 * due to delays of the timer irq, long irq off times etc etc) then
	 * we should round down to the whole second, not up. Use 1/4th second
	 * as cutoff for this rounding as an extreme upper bound for this.
	 * But never round down if @force_up is set.
	 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   목표 jiffie가 1초 후(타이머 irq의 지연, 긴 irq 꺼짐 시간 등으로 인해
 *   발생할 수 있음)인 경우 전체 초로 내림해야 합니다. 이 반올림에 대한
 *   컷오프로 1/4초를 이에 대한 극단적인 상한으로 사용합니다.
 *   그러나 @force_up이 설정되어 있으면 절대 내림하지 마십시오.
 */
	if (rem < HZ/4 && !force_up) /* round down */
		j = j - rem;
	else /* round up */
		j = j - rem + HZ;

	/* now that we have rounded, subtract the extra skew again */
	j -= cpu * 3;

	/*
	 * Make sure j is still in the future. Otherwise return the
	 * unmodified value.
	 */
	return time_is_after_jiffies(j) ? j : original;
}

/**
 * __round_jiffies - function to round jiffies to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * __round_jiffies() rounds an absolute time in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The exact rounding is skewed for each processor to avoid all
 * processors firing at the exact same time, which could lead
 * to lock contention or spurious cache line bouncing.
 *
 * The return value is the rounded version of the @j parameter.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   __round_jiffies - jiffies를 전체 초로 반올림하는 기능. 
 *   @j: 반올림해야 하는 (절대) jiffies의 시간.
 *   @cpu: 시간 초과가 발생할 프로세서 번호입니다.
 *
 *   __round_jiffies()는 미래의 절대 시간(jiffies 단위)을 (대략) 전체 초로
 *   반올림합니다. 이는 대략 X초마다 실행되는 한 정확한 실행 시간이 그다지
 *   중요하지 않은 타이머에 유용합니다.
 *
 *   이 타이머를 전체 초로 반올림하면 이러한 모든 타이머가 여러 시간에
 *   분산되지 않고 동시에 실행됩니다. 이것의 목표는 CPU가 덜 깨어나도록
 *   하여 전력을 절약하는 것입니다.
 *
 *   모든 프로세서가 정확히 동시에 실행되는 것을 방지하기 위해 각 
 *   프로세서에 대해 정확한 반올림이 왜곡되어 잠금 경합 또는 가짜 캐시 
 *   라인 바운싱이 발생할 수 있습니다.
 *
 *   반환 값은 @j 매개 변수의 반올림된 버전입니다. 
 */
unsigned long __round_jiffies(unsigned long j, int cpu)
{
	return round_jiffies_common(j, cpu, false);
}
EXPORT_SYMBOL_GPL(__round_jiffies);

/**
 * __round_jiffies_relative - function to round jiffies to a full second
 * @j: the time in (relative) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * __round_jiffies_relative() rounds a time delta  in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The exact rounding is skewed for each processor to avoid all
 * processors firing at the exact same time, which could lead
 * to lock contention or spurious cache line bouncing.
 *
 * The return value is the rounded version of the @j parameter.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   __round_jiffies_relative - jiffies를 전체 초로 반올림하는 기능.
 *   @j: 반올림해야 하는 (상대적) jiffies의 시간.
 *   @cpu: 시간 초과가 발생할 프로세서 번호입니다.
 *
 *   __round_jiffies_relative()는 미래의 시간 델타(jiffies 단위)를 (대략)
 *   완전한 초로 반올림합니다. 이는 대략 X초마다 실행되는 한 정확한 실행
 *   시간이 그다지 중요하지 않은 타이머에 유용합니다.
 *
 *   이 타이머를 전체 초로 반올림하면 이러한 모든 타이머가 여러 시간에
 *   분산되지 않고 동시에 실행됩니다. 이것의 목표는 CPU가 덜 깨어나도록
 *   하여 전력을 절약하는 것입니다.
 *
 *   모든 프로세서가 정확히 동시에 실행되는 것을 방지하기 위해 각
 *   프로세서에 대해 정확한 반올림이 왜곡되어 잠금 경합 또는 가짜 캐시
 *   라인 바운싱이 발생할 수 있습니다.
 *
 *   반환 값은 @j 매개 변수의 반올림된 버전입니다. 
 */
unsigned long __round_jiffies_relative(unsigned long j, int cpu)
{
	unsigned long j0 = jiffies;

	/* Use j0 because jiffies might change while we run */
	return round_jiffies_common(j + j0, cpu, false) - j0;
}
EXPORT_SYMBOL_GPL(__round_jiffies_relative);

/**
 * round_jiffies - function to round jiffies to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 *
 * round_jiffies() rounds an absolute time in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The return value is the rounded version of the @j parameter.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   round_jiffies - jiffies를 전체 초로 반올림하는 기능.
 *   @j: 반올림해야 하는 (절대) jiffies의 시간.
 *
 *   round_jiffies()는 미래의 절대 시간(jiffies 단위)을 (대략) 전체
 *   초로 반올림하거나 반올림합니다. 이는 대략 X초마다 실행되는 한
 *   정확한 실행 시간이 그다지 중요하지 않은 타이머에 유용합니다.
 *
 *   이 타이머를 전체 초로 반올림하면 이러한 모든 타이머가 여러 시간에
 *   분산되지 않고 동시에 실행됩니다. 이것의 목표는 CPU가 덜 깨어나도록
 *   하여 전력을 절약하는 것입니다. 
 *
 *   반환 값은 @j 매개 변수의 반올림된 버전입니다.
 */
unsigned long round_jiffies(unsigned long j)
{
	return round_jiffies_common(j, raw_smp_processor_id(), false);
}
EXPORT_SYMBOL_GPL(round_jiffies);

/**
 * round_jiffies_relative - function to round jiffies to a full second
 * @j: the time in (relative) jiffies that should be rounded
 *
 * round_jiffies_relative() rounds a time delta  in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The return value is the rounded version of the @j parameter.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   round_jiffies_relative - jiffies를 전체 초로 반올림하는 기능.
 *   @j: 반올림해야 하는 (상대적) jiffies의 시간.
 *
 *   round_jiffies_relative()는 미래의 시간 델타(jiffies 단위)를 (대략)
 *   전체 초까지 반올림합니다. 이는 대략 X초마다 실행되는 한 정확한 실행
 *   시간이 그다지 중요하지 않은 타이머에 유용합니다.
 *
 *   이 타이머를 전체 초로 반올림하면 이러한 모든 타이머가 여러 시간에
 *   분산되지 않고 동시에 실행됩니다. 이것의 목표는 CPU가 덜 깨어나도록
 *   하여 전력을 절약하는 것입니다. 
 *
 *   반환 값은 @j 매개 변수의 반올림된 버전입니다. 
 */
unsigned long round_jiffies_relative(unsigned long j)
{
	return __round_jiffies_relative(j, raw_smp_processor_id());
}
EXPORT_SYMBOL_GPL(round_jiffies_relative);

/**
 * __round_jiffies_up - function to round jiffies up to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * This is the same as __round_jiffies() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   __round_jiffies_up - jiffies를 1초까지 반올림하는 기능.
 *   @j: 반올림해야 하는 (절대) jiffies의 시간.
 *   @cpu: 시간 초과가 발생할 프로세서 번호입니다.
 *
 *   내림하지 않는다는 점을 제외하고는 __round_jiffies() 와 동일합니다.
 *   이는 너무 일찍 실행되지 않는 한 정확한 실행 시간이 그다지 중요하지
 *   않은 타임아웃에 유용합니다. 
 */
unsigned long __round_jiffies_up(unsigned long j, int cpu)
{
	return round_jiffies_common(j, cpu, true);
}
EXPORT_SYMBOL_GPL(__round_jiffies_up);

/**
 * __round_jiffies_up_relative - function to round jiffies up to a full second
 * @j: the time in (relative) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * This is the same as __round_jiffies_relative() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   __round_jiffies_up_relative - jiffies를 1초까지 반올림하는 기능.
 *   @j: 반올림해야 하는 (상대적) jiffies의 시간.
 *   @cpu: 시간 초과가 발생할 프로세서 번호입니다.
 *
 *   내림하지 않는다는 점을 제외하고는 __round_jiffies_relative() 와 동일합니다.
 *   이는 너무 일찍 실행되지 않는 한 정확한 실행 시간이 그다지 중요하지 않은
 *   타임아웃에 유용합니다.
 */
unsigned long __round_jiffies_up_relative(unsigned long j, int cpu)
{
	unsigned long j0 = jiffies;

	/* Use j0 because jiffies might change while we run */
	return round_jiffies_common(j + j0, cpu, true) - j0;
}
EXPORT_SYMBOL_GPL(__round_jiffies_up_relative);

/**
 * round_jiffies_up - function to round jiffies up to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 *
 * This is the same as round_jiffies() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   round_jiffies_up - jiffies를 1초까지 반올림하는 기능.
 *   @j: 반올림해야 하는 (절대) jiffies의 시간.
 *
 *   내림하지 않는다는 점을 제외하면 round_jiffies()와 동일합니다.
 *   이는 너무 일찍 실행되지 않는 한 정확한 실행 시간이 그다지 중요하지
 *   않은 타임아웃에 유용합니다. 
 */
unsigned long round_jiffies_up(unsigned long j)
{
	return round_jiffies_common(j, raw_smp_processor_id(), true);
}
EXPORT_SYMBOL_GPL(round_jiffies_up);

/**
 * round_jiffies_up_relative - function to round jiffies up to a full second
 * @j: the time in (relative) jiffies that should be rounded
 *
 * This is the same as round_jiffies_relative() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   round_jiffies_up_relative - jiffies를 최대 1초까지 반올림하는 기능.
 *   @j: 반올림해야 하는 (상대적) jiffies의 시간.
 *
 *   내림하지 않는다는 점을 제외하면 round_jiffies_relative()와 동일합니다.
 *   이는 너무 일찍 실행되지 않는 한 정확한 실행 시간이 그다지 중요하지 않은
 *   타임아웃에 유용합니다.
 */
unsigned long round_jiffies_up_relative(unsigned long j)
{
	return __round_jiffies_up_relative(j, raw_smp_processor_id());
}
EXPORT_SYMBOL_GPL(round_jiffies_up_relative);

/*
 * IAMROOT, 2022.09.03:
 * - flags로부터 timer가 위치한 vector idx를 가져온다.
 */
static inline unsigned int timer_get_idx(struct timer_list *timer)
{
	return (timer->flags & TIMER_ARRAYMASK) >> TIMER_ARRAYSHIFT;
}

/*
 * IAMROOT, 2022.09.03:
 * - flags에 timer vector @idx를 set한다.
 */
static inline void timer_set_idx(struct timer_list *timer, unsigned int idx)
{
	timer->flags = (timer->flags & ~TIMER_ARRAYMASK) |
			idx << TIMER_ARRAYSHIFT;
}

/*
 * Helper function to calculate the array index for a given expiry
 * time.
 */
/*
 * IAMROOT, 2022.09.03:
 * - Git blame
 *   timers: base->next_expiry 값에 대해 버킷 만료만 사용합니다.
 *
 *   버킷 만료 시간은 타이머의 유효 만료 시간이며 요청된 타이머 만료 시간보다
 *   크거나 같습니다. 이는 타이머가 일찍 만료되지 않는다는 보장과 보조 휠 레벨의
 *   만료 세분성 감소 때문입니다.
 *
 *   타이머가 대기열에 추가되면 trigger_dyntick_cpu()는 타이머가 새로운 첫 번째
 *   타이머인지 확인합니다. 이 검사는 next_expiry를 요청된 타이머 만료 값과
 *   비교하며 타이머가 대기열에 포함된 버킷의 유효 만료 값과 비교하지 않습니다. 
 *
 *   요청된 타이머 만료 값을 base->next_expiry에 저장하면 요청된 타이머 만료 값이
 *   base->clk보다 작은 경우 base->clk가 뒤로 이동할 수 있습니다. 커밋 
 *   30c66fc30ee7("타이머: 기본->clk가 뒤로 이동하는 것을 방지")은 timer->expire가
 *   기본->clk 이전일 때 저장소를 방지하여 이 문제를 해결했지만 근본적인 문제는
 *   수정하지 않았습니다.
 *
 *   타이머가 대기 중인 버킷의 만료 값을 사용하여 새로운 첫 번째 타이머 확인을
 *   수행합니다. 이것은 base->clk가 거꾸로 가는 문제를 수정합니다.
 *
 *   trigger_dyntick_cpu()의 커밋 30c66fc30ee7("타이머: 기본->clk가 뒤로 이동하는
 *   것을 방지")의 해결 방법은 타이머 버킷 만료가 base->clk보다 크거나 같도록
 *   보장되므로 더 이상 필요하지 않습니다.
 *
 * @bucket_expiry LVL_GRAN(lvl) 단위로 올림.
 * ex) exipres = 0x43dd, lvl = 2일때 return, bucket_expiry
 * expires_new = (0x43dd + LVL_GRAN(2)) >> LVL_SHIFT(2)
 *             = (0x43dd + 0x40) >> 6 = 0x441d >> 6
 *             = 0x110
 * bucket_expiry = 0x110 << 6
 *               = 0x4400 = 17408
 * return  = LVL_OFFS(2) + (0x110 & 0x3f)
 *         = 0x80 + 0x10 = 0x90
 */
static inline unsigned calc_index(unsigned long expires, unsigned lvl,
				  unsigned long *bucket_expiry)
{

	/*
	 * The timer wheel has to guarantee that a timer does not fire
	 * early. Early expiry can happen due to:
	 * - Timer is armed at the edge of a tick
	 * - Truncation of the expiry time in the outer wheel levels
	 *
	 * Round up with level granularity to prevent this.
	 */
	expires = (expires + LVL_GRAN(lvl)) >> LVL_SHIFT(lvl);
	*bucket_expiry = expires << LVL_SHIFT(lvl);
	return LVL_OFFS(lvl) + (expires & LVL_MASK);
}

/*
 * IAMROOT, 2022.09.03:
 * @clk base->clk
 * wheel idx를 구한다.
 *
 * ex) clk = 2, expires = 67
 * delta = 65 -> calc_index(expires, 1, bucket_expiry) 호출
 *
 * expires_new = (67 + 8) >> 3 = 9
 * bucket_expiry = 9 << 3 = 72
 * return = 64 + (9 & 0x3f) = 73
 *
 * bucket_expiry = 72, idx = 73
 */
static int calc_wheel_index(unsigned long expires, unsigned long clk,
			    unsigned long *bucket_expiry)
{
	unsigned long delta = expires - clk;
	unsigned int idx;

/*
 * IAMROOT, 2022.09.03:
 * - delta가 들어갈 idx를 찾는다.
 */
	if (delta < LVL_START(1)) {
		idx = calc_index(expires, 0, bucket_expiry);
	} else if (delta < LVL_START(2)) {
		idx = calc_index(expires, 1, bucket_expiry);
	} else if (delta < LVL_START(3)) {
		idx = calc_index(expires, 2, bucket_expiry);
	} else if (delta < LVL_START(4)) {
		idx = calc_index(expires, 3, bucket_expiry);
	} else if (delta < LVL_START(5)) {
		idx = calc_index(expires, 4, bucket_expiry);
	} else if (delta < LVL_START(6)) {
		idx = calc_index(expires, 5, bucket_expiry);
	} else if (delta < LVL_START(7)) {
		idx = calc_index(expires, 6, bucket_expiry);
	} else if (LVL_DEPTH > 8 && delta < LVL_START(8)) {
/*
 * IAMROOT, 2022.09.03:
 * - LVL_DEPTH가 9가 존재하면 lvl 7까지.(0 ~ 7)
 *   LVL_DEPTH가 8까지라면 lvl 6까지만한다. (0 ~ 6)
 */
		idx = calc_index(expires, 7, bucket_expiry);
	} else if ((long) delta < 0) {
/*
 * IAMROOT, 2022.09.03:
 * - delta가 -면 lvl 0로 본다.
 */
		idx = clk & LVL_MASK;
		*bucket_expiry = clk;
	} else {
/*
 * IAMROOT, 2022.09.03:
 * - delta가 너무 큰상황. 마지막 값으로 fixup 한후 idx를 재계산한다.
 */
		/*
		 * Force expire obscene large timeouts to expire at the
		 * capacity limit of the wheel.
		 */
		if (delta >= WHEEL_TIMEOUT_CUTOFF)
			expires = clk + WHEEL_TIMEOUT_MAX;

		idx = calc_index(expires, LVL_DEPTH - 1, bucket_expiry);
	}
	return idx;
}

/*
 * IAMROOT, 2022.09.03:
 * - @base->cpu가 nohz로 동작중일때 깨운다.
 *   1. 일반 timer인 경우 idle 상태면 깨운다.
 *   2. defferable일경우 nohz full일 경우에만 깨운다.
 *      nohz idle 상태일경우엔 그냥 빠져올것이다.
 */
static void
trigger_dyntick_cpu(struct timer_base *base, struct timer_list *timer)
{
	if (!is_timers_nohz_active())
		return;

	/*
	 * TODO: This wants some optimizing similar to the code below, but we
	 * will do that when we switch from push to pull for deferrable timers.
	 */
/*
 * IAMROOT, 2022.09.03:
 * - deffered로 등록한 timer인 경우 deffer를 하지만, cpu가 nohz full인 상태라면
 *   cpu를 wakeup.
 */
	if (timer->flags & TIMER_DEFERRABLE) {
		if (tick_nohz_full_cpu(base->cpu))
			wake_up_nohz_cpu(base->cpu);
		return;
	}

	/*
	 * We might have to IPI the remote CPU if the base is idle and the
	 * timer is not deferrable. If the other CPU is on the way to idle
	 * then it can't set base->is_idle as we hold the base lock:
	 */
/*
 * IAMROOT, 2022.09.03:
 * -papago
 *  베이스가 idle 상태이고 타이머를 연기할 수 없는 경우 원격 CPU를 IPI해야 할 수도
 *  있습니다. 다른 CPU가 유휴 상태가 되면 기본 잠금을 유지하므로 base->is_idle을
 *  설정할 수 없습니다. 
 * - base->is_idle : base쪽 timer tick이 꺼져있다는뜻.
 */
	if (base->is_idle)
		wake_up_nohz_cpu(base->cpu);
}

/*
 * Enqueue the timer into the hash bucket, mark it pending in
 * the bitmap, store the index in the timer flags then wake up
 * the target CPU if needed.
 */
/*
 * IAMROOT, 2022.09.03:
 * - timer를 idx에 add한다. cpu가 idle인 상태인경우 next_expiry가
 *   갱신되는 상황에서 필요에 따라 cpu를 깨운다.
 */
static void enqueue_timer(struct timer_base *base, struct timer_list *timer,
			  unsigned int idx, unsigned long bucket_expiry)
{

	hlist_add_head(&timer->entry, base->vectors + idx);
	__set_bit(idx, base->pending_map);
	timer_set_idx(timer, idx);

	trace_timer_start(timer, timer->expires, timer->flags);

	/*
	 * Check whether this is the new first expiring timer. The
	 * effective expiry time of the timer is required here
	 * (bucket_expiry) instead of timer->expires.
	 */

/*
 * IAMROOT, 2022.09.03:
 * - bucket_expire가 timer_base보다 먼저라면 bucket_expiry로 갱신한다.
 * - 기존에 있었던 next timer보다 새로 추가되는 timer의 bucket_expiry가
 *   빠르면 bucket_expiry로 갱신하라는것이다.
 */
	if (time_before(bucket_expiry, base->next_expiry)) {
		/*
		 * Set the next expiry time and kick the CPU so it
		 * can reevaluate the wheel:
		 */
		base->next_expiry = bucket_expiry;
		base->timers_pending = true;
		base->next_expiry_recalc = false;
		trigger_dyntick_cpu(base, timer);
	}
}

/*
 * IAMROOT, 2022.09.03:
 * - idx를 산출해서 enqueue 한다.
 */
static void internal_add_timer(struct timer_base *base, struct timer_list *timer)
{
	unsigned long bucket_expiry;
	unsigned int idx;

	idx = calc_wheel_index(timer->expires, base->clk, &bucket_expiry);
	enqueue_timer(base, timer, idx, bucket_expiry);
}

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS

static const struct debug_obj_descr timer_debug_descr;

static void *timer_debug_hint(void *addr)
{
	return ((struct timer_list *) addr)->function;
}

static bool timer_is_static_object(void *addr)
{
	struct timer_list *timer = addr;

	return (timer->entry.pprev == NULL &&
		timer->entry.next == TIMER_ENTRY_STATIC);
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static bool timer_fixup_init(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		del_timer_sync(timer);
		debug_object_init(timer, &timer_debug_descr);
		return true;
	default:
		return false;
	}
}

/* Stub timer callback for improperly used timers. */
static void stub_timer(struct timer_list *unused)
{
	WARN_ON(1);
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown non-static object is activated
 */
static bool timer_fixup_activate(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_NOTAVAILABLE:
		timer_setup(timer, stub_timer, 0);
		return true;

	case ODEBUG_STATE_ACTIVE:
		WARN_ON(1);
		fallthrough;
	default:
		return false;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static bool timer_fixup_free(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		del_timer_sync(timer);
		debug_object_free(timer, &timer_debug_descr);
		return true;
	default:
		return false;
	}
}

/*
 * fixup_assert_init is called when:
 * - an untracked/uninit-ed object is found
 */
static bool timer_fixup_assert_init(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_NOTAVAILABLE:
		timer_setup(timer, stub_timer, 0);
		return true;
	default:
		return false;
	}
}

static const struct debug_obj_descr timer_debug_descr = {
	.name			= "timer_list",
	.debug_hint		= timer_debug_hint,
	.is_static_object	= timer_is_static_object,
	.fixup_init		= timer_fixup_init,
	.fixup_activate		= timer_fixup_activate,
	.fixup_free		= timer_fixup_free,
	.fixup_assert_init	= timer_fixup_assert_init,
};

static inline void debug_timer_init(struct timer_list *timer)
{
	debug_object_init(timer, &timer_debug_descr);
}

static inline void debug_timer_activate(struct timer_list *timer)
{
	debug_object_activate(timer, &timer_debug_descr);
}

static inline void debug_timer_deactivate(struct timer_list *timer)
{
	debug_object_deactivate(timer, &timer_debug_descr);
}

static inline void debug_timer_assert_init(struct timer_list *timer)
{
	debug_object_assert_init(timer, &timer_debug_descr);
}

static void do_init_timer(struct timer_list *timer,
			  void (*func)(struct timer_list *),
			  unsigned int flags,
			  const char *name, struct lock_class_key *key);

void init_timer_on_stack_key(struct timer_list *timer,
			     void (*func)(struct timer_list *),
			     unsigned int flags,
			     const char *name, struct lock_class_key *key)
{
	debug_object_init_on_stack(timer, &timer_debug_descr);
	do_init_timer(timer, func, flags, name, key);
}
EXPORT_SYMBOL_GPL(init_timer_on_stack_key);

void destroy_timer_on_stack(struct timer_list *timer)
{
	debug_object_free(timer, &timer_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_timer_on_stack);

#else
static inline void debug_timer_init(struct timer_list *timer) { }
static inline void debug_timer_activate(struct timer_list *timer) { }
static inline void debug_timer_deactivate(struct timer_list *timer) { }
static inline void debug_timer_assert_init(struct timer_list *timer) { }
#endif

static inline void debug_init(struct timer_list *timer)
{
	debug_timer_init(timer);
	trace_timer_init(timer);
}

static inline void debug_deactivate(struct timer_list *timer)
{
	debug_timer_deactivate(timer);
	trace_timer_cancel(timer);
}

static inline void debug_assert_init(struct timer_list *timer)
{
	debug_timer_assert_init(timer);
}

/*
 * IAMROOT, 2022.09.03:
 * - @timer를 초기화한다. 초기화시 함수를 실행한 cpu id를 flags에 기록한다.
 */
static void do_init_timer(struct timer_list *timer,
			  void (*func)(struct timer_list *),
			  unsigned int flags,
			  const char *name, struct lock_class_key *key)
{
	timer->entry.pprev = NULL;
	timer->function = func;
	if (WARN_ON_ONCE(flags & ~TIMER_INIT_FLAGS))
		flags &= TIMER_INIT_FLAGS;
	timer->flags = flags | raw_smp_processor_id();
	lockdep_init_map(&timer->lockdep_map, name, key, 0);
}

/**
 * init_timer_key - initialize a timer
 * @timer: the timer to be initialized
 * @func: timer callback function
 * @flags: timer flags
 * @name: name of the timer
 * @key: lockdep class key of the fake lock used for tracking timer
 *       sync lock dependencies
 *
 * init_timer_key() must be done to a timer prior calling *any* of the
 * other timer functions.
 */
/*
 * IAMROOT, 2022.09.03:
 * - @timer를 초기화한다.
 */
void init_timer_key(struct timer_list *timer,
		    void (*func)(struct timer_list *), unsigned int flags,
		    const char *name, struct lock_class_key *key)
{
	debug_init(timer);
	do_init_timer(timer, func, flags, name, key);
}
EXPORT_SYMBOL(init_timer_key);

/*
 * IAMROOT, 2022.09.03:
 * - 0timer를 detach 한다.
 */
static inline void detach_timer(struct timer_list *timer, bool clear_pending)
{
	struct hlist_node *entry = &timer->entry;

	debug_deactivate(timer);

	__hlist_del(entry);
	if (clear_pending)
		entry->pprev = NULL;
	entry->next = LIST_POISON2;
}

/*
 * IAMROOT, 2022.09.03:
 * - @timer를 @base로부터 분리를 시도한다.
 */
static int detach_if_pending(struct timer_list *timer, struct timer_base *base,
			     bool clear_pending)
{
	unsigned idx = timer_get_idx(timer);

/*
 * IAMROOT, 2022.09.03:
 * - 이미 분리 되있다. 아무것도 안한다.
 */
	if (!timer_pending(timer))
		return 0;

/*
 * IAMROOT, 2022.09.03:
 * - @idx vector에 timer가 1개만 등록되있다면. pending_map을 clear한다.
 *   최종적으로 한개를 분리해내면 idx에서 아무것도 없어지기 때문이다.
 */
	if (hlist_is_singular_node(&timer->entry, base->vectors + idx)) {
		__clear_bit(idx, base->pending_map);
		base->next_expiry_recalc = true;
	}

	detach_timer(timer, clear_pending);
	return 1;
}

/*
 * IAMROOT, 2022.09.03:
 * - @cpu에 해당하는 timer_base를 가져온다.
 */
static inline struct timer_base *get_timer_cpu_base(u32 tflags, u32 cpu)
{
	struct timer_base *base = per_cpu_ptr(&timer_bases[BASE_STD], cpu);

	/*
	 * If the timer is deferrable and NO_HZ_COMMON is set then we need
	 * to use the deferrable base.
	 */
/*
 * IAMROOT, 2022.09.03:
 * - nohz enable이면서 deffered로 등록한 timer라면 nohz용 timer_base를 가져온다.
 */
	if (IS_ENABLED(CONFIG_NO_HZ_COMMON) && (tflags & TIMER_DEFERRABLE))
		base = per_cpu_ptr(&timer_bases[BASE_DEF], cpu);
	return base;
}

/*
 * IAMROOT, 2022.09.03:
 * - 현재 cpu의 timer base를 가져온다.
 */
static inline struct timer_base *get_timer_this_cpu_base(u32 tflags)
{
	struct timer_base *base = this_cpu_ptr(&timer_bases[BASE_STD]);

	/*
	 * If the timer is deferrable and NO_HZ_COMMON is set then we need
	 * to use the deferrable base.
	 */
	if (IS_ENABLED(CONFIG_NO_HZ_COMMON) && (tflags & TIMER_DEFERRABLE))
		base = this_cpu_ptr(&timer_bases[BASE_DEF]);
	return base;
}

/*
 * IAMROOT, 2022.09.03:
 * - @tflags의 cpu로 timer_base를 가져온다.
 */
static inline struct timer_base *get_timer_base(u32 tflags)
{
	return get_timer_cpu_base(tflags, tflags & TIMER_CPUMASK);
}

/*
 * IAMROOT, 2022.09.03:
 * - 1. busy cpu에서 timer base 얻어오기.
 *   2. 현재 cpu로 timer base 얻어오기.
 */
static inline struct timer_base *
get_target_base(struct timer_base *base, unsigned tflags)
{
#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
/*
 * IAMROOT, 2022.09.03:
 * - migration이 가능하고, pinned를 해야된다는 flag가 없다면 busy cpu에서
 *   timer_base를 가져온다.
 *
 */
	if (static_branch_likely(&timers_migration_enabled) &&
	    !(tflags & TIMER_PINNED))
		return get_timer_cpu_base(tflags, get_nohz_timer_target());
#endif

/*
 * IAMROOT, 2022.09.03:
 * - pinned요청이거나, migration을 못한다면 현재 cpu의 timer_base를 가져온다.
 */
	return get_timer_this_cpu_base(tflags);
}

/*
 * IAMROOT, 2022.09.03:
 * - @base를 현재 시간 기준으로 udpate한다.
 *
 * - 기본적으로 @base->clk을 현재 시간으로 update를 한다.
 *   만약 next_expiry가 현재 과거(ex. nohz)라면 base->clk으로 update한다.
 */
static inline void forward_timer_base(struct timer_base *base)
{
	unsigned long jnow = READ_ONCE(jiffies);

	/*
	 * No need to forward if we are close enough below jiffies.
	 * Also while executing timers, base->clk is 1 offset ahead
	 * of jiffies to avoid endless requeuing to current jiffies.
	 */
/*
 * IAMROOT, 2022.09.03:
 * - 이미 timer start 시간이 지낫으면 return.
 */
	if ((long)(jnow - base->clk) < 1)
		return;

	/*
	 * If the next expiry value is > jiffies, then we fast forward to
	 * jiffies otherwise we forward to the next expiry value.
	 */
/*
 * IAMROOT, 2022.09.03:
 * - next_expiry가 현재시간 이후라면 현재시간을 base->clk에 등록.
 *   그게 아니면 clk를 next_expiry로 등록.
 */
	if (time_after(base->next_expiry, jnow)) {
/*
 * IAMROOT, 2022.09.03:
 * - 매 tick마다 갱신된다.(normal case)
 *                 now
 * -----------------|-----------|-------
 *                  |          next
 *           update base->clk
 */
		base->clk = jnow;
	} else {
/*
 * IAMROOT, 2022.09.03:
 * - error 처리.
 * - timer가 이미 지낫으면 return.
 * - next_expiry가 base->clk이전 warn후 return.
 *                          now
 *                           |
 * ---------|-------|--------|----------
 *        next    base->clk    
 */
		if (WARN_ON_ONCE(time_before(base->next_expiry, base->clk)))
			return;

/*
 * IAMROOT, 2022.09.03:
 * - nohz로 인해서 발생하는 상황.
 * - base->clk <--> now사이에 sleep timer가 tick이 들어오지 않아서
 *   갱신이 안됬을때.
 *                               now
 *                                |
 * --------|------------|---------|---
 *       base->clk    next        
 *                      |
 *                update base->clk
 */
		base->clk = base->next_expiry;
	}
}


/*
 * We are using hashed locking: Holding per_cpu(timer_bases[x]).lock means
 * that all timers which are tied to this base are locked, and the base itself
 * is locked too.
 *
 * So __run_timers/migrate_timers can safely modify all timers which could
 * be found in the base->vectors array.
 *
 * When a timer is migrating then the TIMER_MIGRATING flag is set and we need
 * to wait until the migration is done.
 */
/*
 * IAMROOT, 2022.09.03:
 * - @flags에 등록된 cpu의 timer base를 가져온다.
 */
static struct timer_base *lock_timer_base(struct timer_list *timer,
					  unsigned long *flags)
	__acquires(timer->base->lock)
{
	for (;;) {
		struct timer_base *base;
		u32 tf;

		/*
		 * We need to use READ_ONCE() here, otherwise the compiler
		 * might re-read @tf between the check for TIMER_MIGRATING
		 * and spin_lock().
		 */
		tf = READ_ONCE(timer->flags);

/*
 * IAMROOT, 2022.09.03:
 * - migrating 자체가 매우빠르기 때문에 for문으로 migrating이
 *   풀릴때까지 적당히 기다린다.
 * - migrating이 끝나면 기본적으로 cpu가 바뀐다.
 */
		if (!(tf & TIMER_MIGRATING)) {
/*
 * IAMROOT, 2022.09.03:
 * - @timer의 flag(tf)에 timer를 생성한 cpu nr이 등록되있어, 해당
 *   cpu의 timer_base를 가져온다.
 */
			base = get_timer_base(tf);
/*
 * IAMROOT, 2022.09.03:
 * - tf가 같지 않으면 경쟁상황. loop를 계속돈다.
 */
			raw_spin_lock_irqsave(&base->lock, *flags);
			if (timer->flags == tf)
				return base;
			raw_spin_unlock_irqrestore(&base->lock, *flags);
		}
		cpu_relax();
	}
}

#define MOD_TIMER_PENDING_ONLY		0x01
#define MOD_TIMER_REDUCE		0x02
#define MOD_TIMER_NOTPENDING		0x04

/*
 * IAMROOT, 2022.09.03:
 * - timer를 추가하거나 expired를 변경한다.
 */
static inline int
__mod_timer(struct timer_list *timer, unsigned long expires, unsigned int options)
{
	unsigned long clk = 0, flags, bucket_expiry;
	struct timer_base *base, *new_base;
	unsigned int idx = UINT_MAX;
	int ret = 0;

	BUG_ON(!timer->function);

	/*
	 * This is a common optimization triggered by the networking code - if
	 * the timer is re-modified to have the same timeout or ends up in the
	 * same array bucket then just return:
	 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   이것은 네트워킹 코드에 의해 트리거되는 일반적인 최적화입니다. 타이머가 동일한
 *   시간 초과를 갖도록 다시 수정되거나 동일한 어레이 버킷에서 끝나는 경우 다음을
 *   반환합니다.
 * - pending중인 timer이면서 option이
 *   MOD_TIMER_PENDING_ONLY (mod_timer_pending())
 *   MOD_TIMER_REDUCE       (timer_reduce()),
 *   0                      (mod_timer())
 *   경우 if문 동작.
 *
 * - 즉 add_timer()가 아닌 경우
 */
	if (!(options & MOD_TIMER_NOTPENDING) && timer_pending(timer)) {
		/*
		 * The downside of this optimization is that it can result in
		 * larger granularity than you would get from adding a new
		 * timer with this expiry.
		 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   이 최적화의 단점은 이 만료로 새 타이머를 추가하는 것보다 더 큰 세분성을
 *   초래할 수 있다는 것입니다.
 */
		long diff = timer->expires - expires;

/*
 * IAMROOT, 2022.09.03:
 * - 갱신하는 expires시간이 기존과 같다면 아무것도 할필요없을것이다.
 */
		if (!diff)
			return 1;

/*
 * IAMROOT, 2022.09.03:
 * - reduce일때 expire 시간을 더 증가시키는 경우는 무시한다.
 */
		if (options & MOD_TIMER_REDUCE && diff <= 0)
			return 1;

		/*
		 * We lock timer base and calculate the bucket index right
		 * here. If the timer ends up in the same bucket, then we
		 * just update the expiry time and avoid the whole
		 * dequeue/enqueue dance.
		 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   여기에서 타이머 베이스를 잠그고 버킷 인덱스를 계산합니다. 타이머가
 *   동일한 버킷에서 끝나면 만료 시간을 업데이트하고 전체 대기열에서
 *   빼기/대기하기 댄스를 방지합니다.
 *
 * - lock을 건다.
 */
		base = lock_timer_base(timer, &flags);
		forward_timer_base(base);

/*
 * IAMROOT, 2022.09.03:
 * - reduce일때, @expires값이 기존보다 감소되지 않았으면 무시.
 */
		if (timer_pending(timer) && (options & MOD_TIMER_REDUCE) &&
		    time_before_eq(timer->expires, expires)) {
			ret = 1;
			goto out_unlock;
		}

		clk = base->clk;
		idx = calc_wheel_index(expires, clk, &bucket_expiry);

		/*
		 * Retrieve and compare the array index of the pending
		 * timer. If it matches set the expiry to the new value so a
		 * subsequent call will exit in the expires check above.
		 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   보류 중인 타이머의 배열 인덱스를 검색하고 비교합니다. 일치하는 경우 만료를
 *   새 값으로 설정하여 위의 만료 확인에서 후속 호출이 종료되도록 합니다.
 * - idx가 변한게 없는경우, expires만 재갱신한다.
 *   reduce인 경우 전진만 허용한다.
 */
		if (idx == timer_get_idx(timer)) {
			if (!(options & MOD_TIMER_REDUCE))
				timer->expires = expires;
			else if (time_after(timer->expires, expires))
				timer->expires = expires;
			ret = 1;
			goto out_unlock;
		}
	} else {
/*
 * IAMROOT, 2022.09.03:
 * - @timer의 cpu에 해당하는 timer base를 가져오고 update한다.
 */
		base = lock_timer_base(timer, &flags);
		forward_timer_base(base);
	}

	ret = detach_if_pending(timer, base, false);

/*
 * IAMROOT, 2022.09.03:
 * - mod_timer_pending()으로 pending중인 timer에 한해서 수정 요청이 왔는데
 *   이미 timer가 없는 상황. 종료한다.
 */
	if (!ret && (options & MOD_TIMER_PENDING_ONLY))
		goto out_unlock;

	new_base = get_target_base(base, timer->flags);

/*
 * IAMROOT, 2022.09.03:
 * - timer에서 사용하는 cpu와 변경되었는지 확인한다.
 * - ex) cpu0에서 timer0를 add_timer했다고 가정.
 *   cpu0 -> add_timer().   timer0가 cpu0번으로 등록
 *   cpu1 -> mod_timer(timer0).
 *   이런 상황일 경우 base != new_base가 된다.
 */
	if (base != new_base) {
		/*
		 * We are trying to schedule the timer on the new base.
		 * However we can't change timer's base while it is running,
		 * otherwise del_timer_sync() can't detect that the timer's
		 * handler yet has not finished. This also guarantees that the
		 * timer is serialized wrt itself.
		 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   new base에서 타이머를 예약하려고 합니다.,
 *   그러나 타이머가 실행되는 동안에는 타이머의 기반을 변경할 수 없습니다.
 *   그렇지 않으면 del_timer_sync()가 타이머의 핸들러가 아직 완료되지 않았음을
 *   감지할 수 없습니다. 이것은 또한 타이머가 자체적으로 직렬화됨을 보장합니다.
 *
 * - running_timer : expired 되서 동작중인 timer.
 * - running_timer와 현재 timer가 다를경우, 즉 동작중인 timer가 아닐경우.
 *   new_base로 base를 갱신하고 timer의 cpu를 갱신된 new_base로 고친후 update.
 */
		if (likely(base->running_timer != timer)) {
			/* See the comment in lock_timer_base() */
			timer->flags |= TIMER_MIGRATING;

			raw_spin_unlock(&base->lock);
			base = new_base;
			raw_spin_lock(&base->lock);
			WRITE_ONCE(timer->flags,
				   (timer->flags & ~TIMER_BASEMASK) | base->cpu);
			forward_timer_base(base);
		}
	}

	debug_timer_activate(timer);

	timer->expires = expires;
	/*
	 * If 'idx' was calculated above and the base time did not advance
	 * between calculating 'idx' and possibly switching the base, only
	 * enqueue_timer() is required. Otherwise we need to (re)calculate
	 * the wheel index via internal_add_timer().
	 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   'idx'가 위에서 계산되었고 기본 시간이 'idx' 계산과 기본 전환 사이에 진행되지
 *   않은 경우 enqueue_timer()만 필요합니다. 그렇지 않으면 internal_add_timer()를
 *   통해 휠 인덱스를 (재)계산해야 합니다.
 *
 * - 기존 timer가 존재하여 이미 idx를 알아온 경우 즉시. enqueue.
 *   새로 들어온 timer인 경우 idx가 없으므로 idx를 새로 산출해서 enqueue한다.
 */
	if (idx != UINT_MAX && clk == base->clk)
		enqueue_timer(base, timer, idx, bucket_expiry);
	else
		internal_add_timer(base, timer);

out_unlock:
	raw_spin_unlock_irqrestore(&base->lock, flags);

	return ret;
}

/**
 * mod_timer_pending - modify a pending timer's timeout
 * @timer: the pending timer to be modified
 * @expires: new timeout in jiffies
 *
 * mod_timer_pending() is the same for pending timers as mod_timer(),
 * but will not re-activate and modify already deleted timers.
 *
 * It is useful for unserialized use of timers.
 */
/*
 * IAMROOT, 2022.09.03:
 * - timer가 pending중일때, expires를 변경한다.
 */
int mod_timer_pending(struct timer_list *timer, unsigned long expires)
{
	return __mod_timer(timer, expires, MOD_TIMER_PENDING_ONLY);
}
EXPORT_SYMBOL(mod_timer_pending);

/**
 * mod_timer - modify a timer's timeout
 * @timer: the timer to be modified
 * @expires: new timeout in jiffies
 *
 * mod_timer() is a more efficient way to update the expire field of an
 * active timer (if the timer is inactive it will be activated)
 *
 * mod_timer(timer, expires) is equivalent to:
 *
 *     del_timer(timer); timer->expires = expires; add_timer(timer);
 *
 * Note that if there are multiple unserialized concurrent users of the
 * same timer, then mod_timer() is the only safe way to modify the timeout,
 * since add_timer() cannot modify an already running timer.
 *
 * The function returns whether it has modified a pending timer or not.
 * (ie. mod_timer() of an inactive timer returns 0, mod_timer() of an
 * active timer returns 1.)
 */

/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   mod_timer()는 활성 타이머의 만료 필드를 업데이트하는 더 효율적인
 *   방법입니다(타이머가 비활성화된 경우 활성화됨)
 *
 *   mod_timer(timer, expires)는 다음과 같습니다.
 *
 *     del_timer(timer); timer->expires = expires; add_timer(timer);
 *
 *   동일한 타이머의 직렬화되지 않은 동시 사용자가 여러 명 있는 경우
 *   add_timer()가 이미 실행 중인 타이머를 수정할 수 없으므로
 *   mod_timer()가 시간 초과를 수정하는 유일한 안전한 방법입니다.
 *
 *   함수는 보류 중인 타이머를 수정했는지 여부를 반환합니다.
 *   (즉, 비활성 타이머의 mod_timer()는 0을 반환하고 활성 타이머의
 *   mod_timer()는 1을 반환합니다.)
 */
int mod_timer(struct timer_list *timer, unsigned long expires)
{
	return __mod_timer(timer, expires, 0);
}
EXPORT_SYMBOL(mod_timer);

/**
 * timer_reduce - Modify a timer's timeout if it would reduce the timeout
 * @timer:	The timer to be modified
 * @expires:	New timeout in jiffies
 *
 * timer_reduce() is very similar to mod_timer(), except that it will only
 * modify a running timer if that would reduce the expiration time (it will
 * start a timer that isn't running).
 */
/*
 * IAMROOT, 2022.09.03:
 * - timer가 pending중일때, expired를 줄인다.
 */
int timer_reduce(struct timer_list *timer, unsigned long expires)
{
	return __mod_timer(timer, expires, MOD_TIMER_REDUCE);
}
EXPORT_SYMBOL(timer_reduce);

/**
 * add_timer - start a timer
 * @timer: the timer to be added
 *
 * The kernel will do a ->function(@timer) callback from the
 * timer interrupt at the ->expires point in the future. The
 * current time is 'jiffies'.
 *
 * The timer's ->expires, ->function fields must be set prior calling this
 * function.
 *
 * Timers with an ->expires field in the past will be executed in the next
 * timer tick.
 */
/*
 * IAMROOT, 2022.09.03:
 * - @timer 등록.
 */
void add_timer(struct timer_list *timer)
{
	BUG_ON(timer_pending(timer));
	__mod_timer(timer, timer->expires, MOD_TIMER_NOTPENDING);
}
EXPORT_SYMBOL(add_timer);

/**
 * add_timer_on - start a timer on a particular CPU
 * @timer: the timer to be added
 * @cpu: the CPU to start it on
 *
 * This is not very scalable on SMP. Double adds are not possible.
 */
void add_timer_on(struct timer_list *timer, int cpu)
{
	struct timer_base *new_base, *base;
	unsigned long flags;

	BUG_ON(timer_pending(timer) || !timer->function);

	new_base = get_timer_cpu_base(timer->flags, cpu);

	/*
	 * If @timer was on a different CPU, it should be migrated with the
	 * old base locked to prevent other operations proceeding with the
	 * wrong base locked.  See lock_timer_base().
	 */
	base = lock_timer_base(timer, &flags);
	if (base != new_base) {
		timer->flags |= TIMER_MIGRATING;

		raw_spin_unlock(&base->lock);
		base = new_base;
		raw_spin_lock(&base->lock);
		WRITE_ONCE(timer->flags,
			   (timer->flags & ~TIMER_BASEMASK) | cpu);
	}
	forward_timer_base(base);

	debug_timer_activate(timer);
	internal_add_timer(base, timer);
	raw_spin_unlock_irqrestore(&base->lock, flags);
}
EXPORT_SYMBOL_GPL(add_timer_on);

/**
 * del_timer - deactivate a timer.
 * @timer: the timer to be deactivated
 *
 * del_timer() deactivates a timer - this works on both active and inactive
 * timers.
 *
 * The function returns whether it has deactivated a pending timer or not.
 * (ie. del_timer() of an inactive timer returns 0, del_timer() of an
 * active timer returns 1.)
 */
/*
 * IAMROOT, 2022.09.03:
 * - @timer가 pending중이라면 detach.
 */
int del_timer(struct timer_list *timer)
{
	struct timer_base *base;
	unsigned long flags;
	int ret = 0;

	debug_assert_init(timer);

	if (timer_pending(timer)) {
		base = lock_timer_base(timer, &flags);
		ret = detach_if_pending(timer, base, true);
		raw_spin_unlock_irqrestore(&base->lock, flags);
	}

	return ret;
}
EXPORT_SYMBOL(del_timer);

/**
 * try_to_del_timer_sync - Try to deactivate a timer
 * @timer: timer to delete
 *
 * This function tries to deactivate a timer. Upon successful (ret >= 0)
 * exit the timer is not queued and the handler is not running on any CPU.
 */
/*
 * IAMROOT, 2022.09.03:
 * @return -1. running중인 timer라면 삭제를 못한다는것.
 *          1. 삭제 성공.
 *          0. 이미 expire되서 없다.
 */
int try_to_del_timer_sync(struct timer_list *timer)
{
	struct timer_base *base;
	unsigned long flags;
	int ret = -1;

	debug_assert_init(timer);

	base = lock_timer_base(timer, &flags);

	if (base->running_timer != timer)
		ret = detach_if_pending(timer, base, true);

	raw_spin_unlock_irqrestore(&base->lock, flags);

	return ret;
}
EXPORT_SYMBOL(try_to_del_timer_sync);

#ifdef CONFIG_PREEMPT_RT

/*
 * IAMROOT, 2022.09.03:
 * - spinlock을 사용한다.
 */
static __init void timer_base_init_expiry_lock(struct timer_base *base)
{
	spin_lock_init(&base->expiry_lock);
}


/*
 * IAMROOT, 2022.09.03:
 * - spin_lock
 */
static inline void timer_base_lock_expiry(struct timer_base *base)
{
	spin_lock(&base->expiry_lock);
}

/*
 * IAMROOT, 2022.09.03:
 * - spin_unlock
 */
static inline void timer_base_unlock_expiry(struct timer_base *base)
{
	spin_unlock(&base->expiry_lock);
}

/*
 * The counterpart to del_timer_wait_running().
 *
 * If there is a waiter for base->expiry_lock, then it was waiting for the
 * timer callback to finish. Drop expiry_lock and reacquire it. That allows
 * the waiter to acquire the lock and make progress.
 */

/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   del_timer_wait_running()의 대응물.
 *
 *   base->expiry_lock에 대한 웨이터가 있는 경우 타이머 콜백이 완료되기를 기다리고
 *   있는 것입니다. expiry_lock을 삭제하고 다시 획득하십시오. 그러면 웨이터가
 *   잠금을 획득하고 진행할 수 있습니다.
 * - 
 */
static void timer_sync_wait_running(struct timer_base *base)
{
	if (atomic_read(&base->timer_waiters)) {
		raw_spin_unlock_irq(&base->lock);
/*
 * IAMROOT, 2022.09.03:
 * - del_timer_wait_running()에서 del wait를 하고 있는경우 deadlock풀어서
 *   callback이 끝난걸 알려 delete가 가능하게 풀어준다.
 */
		spin_unlock(&base->expiry_lock);
		spin_lock(&base->expiry_lock);
		raw_spin_lock_irq(&base->lock);
	}
}

/*
 * This function is called on PREEMPT_RT kernels when the fast path
 * deletion of a timer failed because the timer callback function was
 * running.
 *
 * This prevents priority inversion, if the softirq thread on a remote CPU
 * got preempted, and it prevents a life lock when the task which tries to
 * delete a timer preempted the softirq thread running the timer callback
 * function.
 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   이 함수는 타이머 콜백 함수가 실행 중이기 때문에 타이머의 빠른 경로 삭제가
 *   실패한 경우 PREEMPT_RT 커널에서 호출됩니다.
 *
 *   이것은 원격 CPU의 softirq 스레드가 선점된 경우 우선 순위 반전을 방지하고
 *   타이머를 삭제하려는 작업이 타이머 콜백 기능을 실행하는 softirq 스레드를
 *   선점할 때 생명 잠금을 방지합니다.
 * - timer가 irqsafe가 아니라면 timer_waiters에 등록하고 deadlock을 걸어
 *   callback이 끝날떄까지 기다린다.
 */
static void del_timer_wait_running(struct timer_list *timer)
{
	u32 tf;

	tf = READ_ONCE(timer->flags);
	if (!(tf & (TIMER_MIGRATING | TIMER_IRQSAFE))) {
		struct timer_base *base = get_timer_base(tf);

		/*
		 * Mark the base as contended and grab the expiry lock,
		 * which is held by the softirq across the timer
		 * callback. Drop the lock immediately so the softirq can
		 * expire the next timer. In theory the timer could already
		 * be running again, but that's more than unlikely and just
		 * causes another wait loop.
		 */
		atomic_inc(&base->timer_waiters);

/*
 * IAMROOT, 2022.09.03:
 * - timer_sync_wait_running()의 spin_unlock을 deadlock을 걸어 기다린다.
 */
		spin_lock_bh(&base->expiry_lock);
		atomic_dec(&base->timer_waiters);
		spin_unlock_bh(&base->expiry_lock);
	}
}
#else
static inline void timer_base_init_expiry_lock(struct timer_base *base) { }
static inline void timer_base_lock_expiry(struct timer_base *base) { }
static inline void timer_base_unlock_expiry(struct timer_base *base) { }
static inline void timer_sync_wait_running(struct timer_base *base) { }
static inline void del_timer_wait_running(struct timer_list *timer) { }
#endif

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
/**
 * del_timer_sync - deactivate a timer and wait for the handler to finish.
 * @timer: the timer to be deactivated
 *
 * This function only differs from del_timer() on SMP: besides deactivating
 * the timer it also makes sure the handler has finished executing on other
 * CPUs.
 *
 * Synchronization rules: Callers must prevent restarting of the timer,
 * otherwise this function is meaningless. It must not be called from
 * interrupt contexts unless the timer is an irqsafe one. The caller must
 * not hold locks which would prevent completion of the timer's
 * handler. The timer's handler must not call add_timer_on(). Upon exit the
 * timer is not queued and the handler is not running on any CPU.
 *
 * Note: For !irqsafe timers, you must not hold locks that are held in
 *   interrupt context while calling this function. Even if the lock has
 *   nothing to do with the timer in question.  Here's why::
 *
 *    CPU0                             CPU1
 *    ----                             ----
 *                                     <SOFTIRQ>
 *                                       call_timer_fn();
 *                                       base->running_timer = mytimer;
 *    spin_lock_irq(somelock);
 *                                     <IRQ>
 *                                        spin_lock(somelock);
 *    del_timer_sync(mytimer);
 *    while (base->running_timer == mytimer);
 *
 * Now del_timer_sync() will never return and never release somelock.
 * The interrupt on the other CPU is waiting to grab somelock but
 * it has interrupted the softirq that CPU0 is waiting to finish.
 *
 * The function returns whether it has deactivated a pending timer or not.
 */
/*
 * IAMROOT, 2022.09.03:
 * - @timer를 삭제한다.
 *   1. pending중인 경우. 삭제후 return 0.
 *   2. expire된 경우 별거 안하고 return 1.
 *   3. running중인 경우 deadlock을 걸어 callback이 끝날때까지 wait.
 */
int del_timer_sync(struct timer_list *timer)
{
	int ret;

#ifdef CONFIG_LOCKDEP
	unsigned long flags;

	/*
	 * If lockdep gives a backtrace here, please reference
	 * the synchronization rules above.
	 */
	local_irq_save(flags);
	lock_map_acquire(&timer->lockdep_map);
	lock_map_release(&timer->lockdep_map);
	local_irq_restore(flags);
#endif
	/*
	 * don't use it in hardirq context, because it
	 * could lead to deadlock.
	 */
	WARN_ON(in_irq() && !(timer->flags & TIMER_IRQSAFE));

	/*
	 * Must be able to sleep on PREEMPT_RT because of the slowpath in
	 * del_timer_wait_running().
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && !(timer->flags & TIMER_IRQSAFE))
		lockdep_assert_preemption_enabled();

	do {
		ret = try_to_del_timer_sync(timer);

/*
 * IAMROOT, 2022.09.03:
 * - running중인 timer를 삭제하라는 요청이 왔다.
 *   irqsave가 없는 timer인 경우 callback이 완료 될때까지
 *   deadlock으로 기다리고, 그렇지 않을 경우 cpu_relax를 한번한후 while.
 */
		if (unlikely(ret < 0)) {
			del_timer_wait_running(timer);
			cpu_relax();
		}
	} while (ret < 0);

	return ret;
}
EXPORT_SYMBOL(del_timer_sync);
#endif

/*
 * IAMROOT, 2022.09.03:
 * - @fn callback 수행.
 */
static void call_timer_fn(struct timer_list *timer,
			  void (*fn)(struct timer_list *),
			  unsigned long baseclk)
{
	int count = preempt_count();

#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the timer from inside the
	 * function that is called from it, this we need to take into
	 * account for lockdep too. To avoid bogus "held lock freed"
	 * warnings as well as problems when looking into
	 * timer->lockdep_map, make a copy and use that here.
	 */
	struct lockdep_map lockdep_map;

	lockdep_copy_map(&lockdep_map, &timer->lockdep_map);
#endif
	/*
	 * Couple the lock chain with the lock chain at
	 * del_timer_sync() by acquiring the lock_map around the fn()
	 * call here and in del_timer_sync().
	 */
	lock_map_acquire(&lockdep_map);

	trace_timer_expire_entry(timer, baseclk);
	fn(timer);
	trace_timer_expire_exit(timer);

	lock_map_release(&lockdep_map);

/*
 * IAMROOT, 2022.09.03:
 * - @fn 안에서 preempt_enable/disable을 증감 하면 count가 증감하게된다.
 *   이러면안된다.
 */
	if (count != preempt_count()) {
		WARN_ONCE(1, "timer: %pS preempt leak: %08x -> %08x\n",
			  fn, count, preempt_count());
		/*
		 * Restore the preempt count. That gives us a decent
		 * chance to survive and extract information. If the
		 * callback kept a lock held, bad luck, but not worse
		 * than the BUG() we had.
		 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   선점 횟수를 복원합니다. 그것은 우리에게 생존하고 정보를 추출할 적절한 기회를
 *   제공합니다. 콜백이 잠금을 유지했다면 운이 나빴지만 BUG()보다 나쁘지는
 *   않았습니다.
 */
		preempt_count_set(count);
	}
}

/*
 * IAMROOT, 2022.09.03:
 * - @head에 있는 timer에 등록된 callback function을 호출한다.
 */
static void expire_timers(struct timer_base *base, struct hlist_head *head)
{
	/*
	 * This value is required only for tracing. base->clk was
	 * incremented directly before expire_timers was called. But expiry
	 * is related to the old base->clk value.
	 */
/*
 * IAMROOT, 2022.09.03:
 * - papago
 *   이 값은 추적에만 필요합니다. base->clk는 expire_timers가 호출되기 직전에
 *   증분되었습니다. 그러나 만료는 이전 base->clk 값과 관련이 있습니다.
 */
	unsigned long baseclk = base->clk - 1;

	while (!hlist_empty(head)) {
		struct timer_list *timer;
		void (*fn)(struct timer_list *);

/*
 * IAMROOT, 2022.09.03:
 * - 1. timer를 한개빼고
 *   2. running에 넣은후
 *   3. callback 함수 호출.
 */
		timer = hlist_entry(head->first, struct timer_list, entry);

		base->running_timer = timer;
		detach_timer(timer, true);

		fn = timer->function;

/*
 * IAMROOT, 2022.09.03:
 * - timer가 irq safe라는 일반 spinlock, 그렇지 않은 경우 irq spinlock수행.
 * - irq에 영향이 없는 timer인경우.
 */
		if (timer->flags & TIMER_IRQSAFE) {
			raw_spin_unlock(&base->lock);
			call_timer_fn(timer, fn, baseclk);
			raw_spin_lock(&base->lock);
			base->running_timer = NULL;
		} else {
/*
 * IAMROOT, 2022.09.03:
 * - irq에 영향이 있는경우,
 */
			raw_spin_unlock_irq(&base->lock);
			call_timer_fn(timer, fn, baseclk);
			raw_spin_lock_irq(&base->lock);
			base->running_timer = NULL;
			timer_sync_wait_running(base);
		}
	}
}

/*
 * IAMROOT, 2022.09.03:
 * lvl 0..LVL_DEPTH 까지 순회하며 expired된 timer들을 heads에 옮기며 가장 높은
 * level을 기록한다.
 */
static int collect_expired_timers(struct timer_base *base,
				  struct hlist_head *heads)
{
	unsigned long clk = base->clk = base->next_expiry;
	struct hlist_head *vec;
	int i, levels = 0;
	unsigned int idx;

/*
 * IAMROOT, 2022.09.03:
 *
 * lvl 0..LVL_DEPTH 까지 순회하며 expired된 timer들을 heads에 옮기며 가져온
 * 레벨 개수를 return 한다.
 *
 * level 0 -> timer0, timer1, timer2 -> heads[0]에 통째로 넣어진다.
 * level 1 -> timer3, timer4,        -> heads[1]에 통째로 넣어진다.
 * level 2 -> NULL                   -> 여기서 중단되고 return level2
 * level 3 -> timer5.
 */
	for (i = 0; i < LVL_DEPTH; i++) {
		idx = (clk & LVL_MASK) + i * LVL_SIZE;

		if (__test_and_clear_bit(idx, base->pending_map)) {
			vec = base->vectors + idx;
			hlist_move_list(vec, heads++);
			levels++;
		}
		/* Is it time to look at the next level? */

/*
 * IAMROOT, 2022.09.03:
 * - 중간이 비면 그만한다.
 */
		if (clk & LVL_CLK_MASK)
			break;
		/* Shift clock for the next level granularity */
		clk >>= LVL_CLK_SHIFT;
	}
	return levels;
}

/*
 * Find the next pending bucket of a level. Search from level start (@offset)
 * + @clk upwards and if nothing there, search from start of the level
 * (@offset) up to @offset + clk.
 */
static int next_pending_bucket(struct timer_base *base, unsigned offset,
			       unsigned clk)
{
	unsigned pos, start = offset + clk;
	unsigned end = offset + LVL_SIZE;

	pos = find_next_bit(base->pending_map, end, start);
	if (pos < end)
		return pos - start;

	pos = find_next_bit(base->pending_map, start, offset);
	return pos < start ? pos + LVL_SIZE - start : -1;
}

/*
 * Search the first expiring timer in the various clock levels. Caller must
 * hold base->lock.
 */
/*
 * IAMROOT, 2022.09.03:
 * - TODO
 */
static unsigned long __next_timer_interrupt(struct timer_base *base)
{
	unsigned long clk, next, adj;
	unsigned lvl, offset = 0;

	next = base->clk + NEXT_TIMER_MAX_DELTA;
	clk = base->clk;
	for (lvl = 0; lvl < LVL_DEPTH; lvl++, offset += LVL_SIZE) {
		int pos = next_pending_bucket(base, offset, clk & LVL_MASK);
		unsigned long lvl_clk = clk & LVL_CLK_MASK;

		if (pos >= 0) {
			unsigned long tmp = clk + (unsigned long) pos;

			tmp <<= LVL_SHIFT(lvl);
			if (time_before(tmp, next))
				next = tmp;

			/*
			 * If the next expiration happens before we reach
			 * the next level, no need to check further.
			 */
			if (pos <= ((LVL_CLK_DIV - lvl_clk) & LVL_CLK_MASK))
				break;
		}
		/*
		 * Clock for the next level. If the current level clock lower
		 * bits are zero, we look at the next level as is. If not we
		 * need to advance it by one because that's going to be the
		 * next expiring bucket in that level. base->clk is the next
		 * expiring jiffie. So in case of:
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1 LVL0
		 *  0    0    0    0    0    0
		 *
		 * we have to look at all levels @index 0. With
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1 LVL0
		 *  0    0    0    0    0    2
		 *
		 * LVL0 has the next expiring bucket @index 2. The upper
		 * levels have the next expiring bucket @index 1.
		 *
		 * In case that the propagation wraps the next level the same
		 * rules apply:
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1 LVL0
		 *  0    0    0    0    F    2
		 *
		 * So after looking at LVL0 we get:
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1
		 *  0    0    0    1    0
		 *
		 * So no propagation from LVL1 to LVL2 because that happened
		 * with the add already, but then we need to propagate further
		 * from LVL2 to LVL3.
		 *
		 * So the simple check whether the lower bits of the current
		 * level are 0 or not is sufficient for all cases.
		 */
		adj = lvl_clk ? 1 : 0;
		clk >>= LVL_CLK_SHIFT;
		clk += adj;
	}

	base->next_expiry_recalc = false;
	base->timers_pending = !(next == base->clk + NEXT_TIMER_MAX_DELTA);

	return next;
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * Check, if the next hrtimer event is before the next timer wheel
 * event:
 */
static u64 cmp_next_hrtimer_event(u64 basem, u64 expires)
{
	u64 nextevt = hrtimer_get_next_event();

	/*
	 * If high resolution timers are enabled
	 * hrtimer_get_next_event() returns KTIME_MAX.
	 */
	if (expires <= nextevt)
		return expires;

	/*
	 * If the next timer is already expired, return the tick base
	 * time so the tick is fired immediately.
	 */
	if (nextevt <= basem)
		return basem;

	/*
	 * Round up to the next jiffie. High resolution timers are
	 * off, so the hrtimers are expired in the tick and we need to
	 * make sure that this tick really expires the timer to avoid
	 * a ping pong of the nohz stop code.
	 *
	 * Use DIV_ROUND_UP_ULL to prevent gcc calling __divdi3
	 */
	return DIV_ROUND_UP_ULL(nextevt, TICK_NSEC) * TICK_NSEC;
}

/**
 * get_next_timer_interrupt - return the time (clock mono) of the next timer
 * @basej:	base time jiffies
 * @basem:	base time clock monotonic
 *
 * Returns the tick aligned clock monotonic time of the next pending
 * timer or KTIME_MAX if no timer is pending.
 */
u64 get_next_timer_interrupt(unsigned long basej, u64 basem)
{
	struct timer_base *base = this_cpu_ptr(&timer_bases[BASE_STD]);
	u64 expires = KTIME_MAX;
	unsigned long nextevt;

	/*
	 * Pretend that there is no timer pending if the cpu is offline.
	 * Possible pending timers will be migrated later to an active cpu.
	 */
	if (cpu_is_offline(smp_processor_id()))
		return expires;

	raw_spin_lock(&base->lock);
	if (base->next_expiry_recalc)
		base->next_expiry = __next_timer_interrupt(base);
	nextevt = base->next_expiry;

	/*
	 * We have a fresh next event. Check whether we can forward the
	 * base. We can only do that when @basej is past base->clk
	 * otherwise we might rewind base->clk.
	 */
	if (time_after(basej, base->clk)) {
		if (time_after(nextevt, basej))
			base->clk = basej;
		else if (time_after(nextevt, base->clk))
			base->clk = nextevt;
	}

	if (time_before_eq(nextevt, basej)) {
		expires = basem;
		base->is_idle = false;
	} else {
		if (base->timers_pending)
			expires = basem + (u64)(nextevt - basej) * TICK_NSEC;
		/*
		 * If we expect to sleep more than a tick, mark the base idle.
		 * Also the tick is stopped so any added timer must forward
		 * the base clk itself to keep granularity small. This idle
		 * logic is only maintained for the BASE_STD base, deferrable
		 * timers may still see large granularity skew (by design).
		 */
		if ((expires - basem) > TICK_NSEC)
			base->is_idle = true;
	}
	raw_spin_unlock(&base->lock);

	return cmp_next_hrtimer_event(basem, expires);
}

/**
 * timer_clear_idle - Clear the idle state of the timer base
 *
 * Called with interrupts disabled
 */
/*
 * IAMROOT, 2023.03.18:
 * - standard의 idle을 false로 하여 nohz를 푼다.
 */
void timer_clear_idle(void)
{
	struct timer_base *base = this_cpu_ptr(&timer_bases[BASE_STD]);

	/*
	 * We do this unlocked. The worst outcome is a remote enqueue sending
	 * a pointless IPI, but taking the lock would just make the window for
	 * sending the IPI a few instructions smaller for the cost of taking
	 * the lock in the exit from idle path.
	 */
/*
 * IAMROOT, 2023.03.18:
 * - papago
 *   우리는 이것을 잠금 해제합니다. 최악의 결과는 무의미한 IPI를 보내는 원격 
 *   인큐(enqueue)이지만, 잠금을 사용하면 유휴 경로에서 종료할 때 잠금을 사용하는 
 *   비용에 비해 IPI를 보내는 데 필요한 몇 가지 명령이 더 작아집니다. 
 */
	base->is_idle = false;
}
#endif

/**
 * __run_timers - run all expired timers (if any) on this CPU.
 * @base: the timer vector to be processed.
 */

/*
 * IAMROOT, 2022.09.03:
 * - next_expiry 시간이 지난 timer들을 levels 역순으로 expire 시킨다.
 */
static inline void __run_timers(struct timer_base *base)
{
	struct hlist_head heads[LVL_DEPTH];
	int levels;

/*
 * IAMROOT, 2022.09.03:
 * - 만료시각이 아직 안됬다. return.
 */
	if (time_before(jiffies, base->next_expiry))
		return;

	timer_base_lock_expiry(base);
	raw_spin_lock_irq(&base->lock);

/*
 * IAMROOT, 2022.09.03:
 * 
 * time ---------|--------------|-------------------------|--------
 *           base->clk      base->next_expiry        jiffies     
 *               ---------------------------------------->
 *                   이 사이에 있는 timer들을 expire 시킨다.
 */
	while (time_after_eq(jiffies, base->clk) &&
	       time_after_eq(jiffies, base->next_expiry)) {
		levels = collect_expired_timers(base, heads);
		/*
		 * The only possible reason for not finding any expired
		 * timer at this clk is that all matching timers have been
		 * dequeued.
		 */
		WARN_ON_ONCE(!levels && !base->next_expiry_recalc);
		base->clk++;
		base->next_expiry = __next_timer_interrupt(base);

/*
 * IAMROOT, 2022.09.03:
 * - 가져온 timer들을 역순으로 expire 시킨다.
 */
		while (levels--)
			expire_timers(base, heads + levels);
	}
	raw_spin_unlock_irq(&base->lock);
	timer_base_unlock_expiry(base);
}

/*
 * This function runs timers and the timer-tq in bottom half context.
 */
/*
 * IAMROOT, 2022.09.03:
 * - TIMER_SOFTIRQ 발생시 동작하는 함수.
 */
static __latent_entropy void run_timer_softirq(struct softirq_action *h)
{
	struct timer_base *base = this_cpu_ptr(&timer_bases[BASE_STD]);

	__run_timers(base);
	if (IS_ENABLED(CONFIG_NO_HZ_COMMON))
		__run_timers(this_cpu_ptr(&timer_bases[BASE_DEF]));
}

/*
 * Called by the local, per-CPU timer interrupt on SMP.
 */
/*
 * IAMROOT, 2022.12.03:
 * - expiry time을 확인하여 lowres timer를 동작시킨다.
 *   hrtimer가 아직 활성화가 안된경우 hrtimer까지 수행한다.
 */
static void run_local_timers(void)
{
	struct timer_base *base = this_cpu_ptr(&timer_bases[BASE_STD]);

	hrtimer_run_queues();
	/* Raise the softirq only if required. */
/*
 * IAMROOT, 2022.12.03:
 * - expiry 시간 전이면 return.
 *   nohz가 켜져있으면 deferred 까지 검사한다.
 */
	if (time_before(jiffies, base->next_expiry)) {
		if (!IS_ENABLED(CONFIG_NO_HZ_COMMON))
			return;
		/* CPU is awake, so check the deferrable base. */
		base++;
		if (time_before(jiffies, base->next_expiry))
			return;
	}
/*
 * IAMROOT, 2022.12.03:
 * - lowres timer 동작.
 */
	raise_softirq(TIMER_SOFTIRQ);
}

/*
 * Called from the timer interrupt handler to charge one tick to the current
 * process.  user_tick is 1 if the tick is user time, 0 for system.
 */
/*
 * IAMROOT, 2022.12.03:
 * - user상태인경우 @user_tick == 1.
 *   kernel인경우 @user_tick == 0
 *
 * - tick이 update될시 수행할 모든일을 수행한다.
 *   수행하는 일이 많으므로 내부 api각각 참조.
 */
void update_process_times(int user_tick)
{
	struct task_struct *p = current;

	PRANDOM_ADD_NOISE(jiffies, user_tick, p, 0);

	/* Note: this timer irq context must be accounted for as well. */
	account_process_tick(p, user_tick);
	run_local_timers();
	rcu_sched_clock_irq(user_tick);
#ifdef CONFIG_IRQ_WORK
	if (in_irq())
		irq_work_tick();
#endif
	scheduler_tick();
	if (IS_ENABLED(CONFIG_POSIX_TIMERS))
		run_posix_cpu_timers();
}

/*
 * Since schedule_timeout()'s timer is defined on the stack, it must store
 * the target task on the stack as well.
 */
struct process_timer {
	struct timer_list timer;
	struct task_struct *task;
};

static void process_timeout(struct timer_list *t)
{
	struct process_timer *timeout = from_timer(timeout, t, timer);

	wake_up_process(timeout->task);
}

/**
 * schedule_timeout - sleep until timeout
 * @timeout: timeout value in jiffies
 *
 * Make the current task sleep until @timeout jiffies have elapsed.
 * The function behavior depends on the current task state
 * (see also set_current_state() description):
 *
 * %TASK_RUNNING - the scheduler is called, but the task does not sleep
 * at all. That happens because sched_submit_work() does nothing for
 * tasks in %TASK_RUNNING state.
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout jiffies are guaranteed to
 * pass before the routine returns unless the current task is explicitly
 * woken up, (e.g. by wake_up_process()).
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task or the current task is explicitly woken
 * up.
 *
 * The current task state is guaranteed to be %TASK_RUNNING when this
 * routine returns.
 *
 * Specifying a @timeout value of %MAX_SCHEDULE_TIMEOUT will schedule
 * the CPU away without a bound on the timeout. In this case the return
 * value will be %MAX_SCHEDULE_TIMEOUT.
 *
 * Returns 0 when the timer has expired otherwise the remaining time in
 * jiffies will be returned. In all cases the return value is guaranteed
 * to be non-negative.
 */
/*
 * IAMROOT. 2023.03.13:
 * - google-translate
 *   schedule_timeout - 제한 시간까지 잠자기
 *   @timeout: 시간 제한 값(jiffies)
 *
 *   @timeout jiffies가 경과할 때까지 현재 작업을 잠자기 상태로 만듭니다. 함수 동작은 현재
 *   작업 상태에 따라 다릅니다(set_current_state() 설명 참조):
 *
 *   %TASK_RUNNING - 스케줄러가 호출되지만 작업이 전혀 잠들지 않습니다. 이는
 *   sched_submit_work()가 %TASK_RUNNING 상태의 작업에 대해 아무 작업도 수행하지
 *   않기 때문에 발생합니다.
 *
 *   %TASK_UNINTERRUPTIBLE - 현재 작업이 명시적으로 깨어나지 않는 한(예:
 *   wake_up_process()에 의해) 루틴이 반환되기 전에 최소한 @timeout jiffies가
 *   통과하도록 보장됩니다.
 *
 *   %TASK_INTERRUPTIBLE - 현재 작업에 신호가 전달되거나 현재 작업이 명시적으로
 *   깨어난 경우 루틴이 일찍 반환될 수 있습니다.
 *
 *   이 루틴이 반환되면 현재 태스크 상태는 %TASK_RUNNING이 됩니다.
 *
 *   %MAX_SCHEDULE_TIMEOUT의 @timeout 값을 지정하면 제한 시간 제한 없이 CPU를
 *   멀리 예약합니다. 이 경우 반환 값은 %MAX_SCHEDULE_TIMEOUT입니다.
 *
 *   타이머가 만료되면 0을 반환합니다. 그렇지 않으면 jiffies의 남은 시간이 반환됩니다.
 *   모든 경우에 반환 값은 음수가 아님이 보장됩니다.
 */
signed long __sched schedule_timeout(signed long timeout)
{
	struct process_timer timer;
	unsigned long expire;

	switch (timeout)
	{
	case MAX_SCHEDULE_TIMEOUT:
		/*
		 * These two special cases are useful to be comfortable
		 * in the caller. Nothing more. We could take
		 * MAX_SCHEDULE_TIMEOUT from one of the negative value
		 * but I' d like to return a valid offset (>=0) to allow
		 * the caller to do everything it want with the retval.
		 */
		/*
		 * IAMROOT. 2023.03.13:
		 * - google-translate
		 *   이 두 가지 특별한 경우는 발신자에게 편안함을 주는 데 유용합니다.
		 *   더 이상은 없습니다. 음수 값 중 하나에서 MAX_SCHEDULE_TIMEOUT을
		 *   취할 수 있지만 호출자가 retval로 원하는 모든 작업을 수행할 수 있도록
		 *   유효한 오프셋(>=0)을 반환하고 싶습니다.
		 */
		schedule();
		goto out;
	default:
		/*
		 * Another bit of PARANOID. Note that the retval will be
		 * 0 since no piece of kernel is supposed to do a check
		 * for a negative retval of schedule_timeout() (since it
		 * should never happens anyway). You just have the printk()
		 * that will tell you if something is gone wrong and where.
		 */
		/*
		 * IAMROOT. 2023.03.13:
		 * - google-translate
		 *   편집증의 또 다른 비트. 커널의 어떤 부분도 schedule_timeout()의
		 *   음수 retval에 대한 검사를 수행하지 않기 때문에 retval은 0이 됩니다
		 *   (어쨌든 발생해서는 안 되기 때문에). 뭔가 잘못되었는지, 어디에서
		 *   문제가 발생했는지 알려주는 printk()가 있습니다.
		 */
		if (timeout < 0) {
			printk(KERN_ERR "schedule_timeout: wrong timeout "
				"value %lx\n", timeout);
			dump_stack();
			__set_current_state(TASK_RUNNING);
			goto out;
		}
	}

	expire = timeout + jiffies;

	timer.task = current;
	timer_setup_on_stack(&timer.timer, process_timeout, 0);
	__mod_timer(&timer.timer, expire, MOD_TIMER_NOTPENDING);
	schedule();
	del_singleshot_timer_sync(&timer.timer);

	/* Remove the timer from the object tracker */
	destroy_timer_on_stack(&timer.timer);

	timeout = expire - jiffies;

 out:
	return timeout < 0 ? 0 : timeout;
}
EXPORT_SYMBOL(schedule_timeout);

/*
 * We can use __set_current_state() here because schedule_timeout() calls
 * schedule() unconditionally.
 */
signed long __sched schedule_timeout_interruptible(signed long timeout)
{
	__set_current_state(TASK_INTERRUPTIBLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_interruptible);

signed long __sched schedule_timeout_killable(signed long timeout)
{
	__set_current_state(TASK_KILLABLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_killable);

signed long __sched schedule_timeout_uninterruptible(signed long timeout)
{
	__set_current_state(TASK_UNINTERRUPTIBLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_uninterruptible);

/*
 * Like schedule_timeout_uninterruptible(), except this task will not contribute
 * to load average.
 */
signed long __sched schedule_timeout_idle(signed long timeout)
{
	__set_current_state(TASK_IDLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_idle);

#ifdef CONFIG_HOTPLUG_CPU
static void migrate_timer_list(struct timer_base *new_base, struct hlist_head *head)
{
	struct timer_list *timer;
	int cpu = new_base->cpu;

	while (!hlist_empty(head)) {
		timer = hlist_entry(head->first, struct timer_list, entry);
		detach_timer(timer, false);
		timer->flags = (timer->flags & ~TIMER_BASEMASK) | cpu;
		internal_add_timer(new_base, timer);
	}
}

int timers_prepare_cpu(unsigned int cpu)
{
	struct timer_base *base;
	int b;

	for (b = 0; b < NR_BASES; b++) {
		base = per_cpu_ptr(&timer_bases[b], cpu);
		base->clk = jiffies;
		base->next_expiry = base->clk + NEXT_TIMER_MAX_DELTA;
		base->timers_pending = false;
		base->is_idle = false;
	}
	return 0;
}

int timers_dead_cpu(unsigned int cpu)
{
	struct timer_base *old_base;
	struct timer_base *new_base;
	int b, i;

	BUG_ON(cpu_online(cpu));

	for (b = 0; b < NR_BASES; b++) {
		old_base = per_cpu_ptr(&timer_bases[b], cpu);
		new_base = get_cpu_ptr(&timer_bases[b]);
		/*
		 * The caller is globally serialized and nobody else
		 * takes two locks at once, deadlock is not possible.
		 */
		raw_spin_lock_irq(&new_base->lock);
		raw_spin_lock_nested(&old_base->lock, SINGLE_DEPTH_NESTING);

		/*
		 * The current CPUs base clock might be stale. Update it
		 * before moving the timers over.
		 */
		forward_timer_base(new_base);

		BUG_ON(old_base->running_timer);

		for (i = 0; i < WHEEL_SIZE; i++)
			migrate_timer_list(new_base, old_base->vectors + i);

		raw_spin_unlock(&old_base->lock);
		raw_spin_unlock_irq(&new_base->lock);
		put_cpu_ptr(&timer_bases);
	}
	return 0;
}

#endif /* CONFIG_HOTPLUG_CPU */

/*
 * IAMROOT, 2022.09.03:
 * - @cpu의 lowres timer 자료구조를 초기화한다.
 */
static void __init init_timer_cpu(int cpu)
{
	struct timer_base *base;
	int i;

	for (i = 0; i < NR_BASES; i++) {
		base = per_cpu_ptr(&timer_bases[i], cpu);
		base->cpu = cpu;
		raw_spin_lock_init(&base->lock);
/*
 * IAMROOT, 2022.09.03:
 * - 현재 clock을 저장해놓는다.
 */
		base->clk = jiffies;
		base->next_expiry = base->clk + NEXT_TIMER_MAX_DELTA;
		timer_base_init_expiry_lock(base);
	}
}

/*
 * IAMROOT, 2022.09.03:
 * - 전체 cpu의 lowres timer 자료구조를 초기화한다.
 */
static void __init init_timer_cpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		init_timer_cpu(cpu);
}

/*
 * IAMROOT, 2022.09.03:
 * - 1. 전체 cpu lowres timer 자료구조 초기화.
 *   2. posix timer 초기화.
 *   3. TIMER_SOFTIRQ callback 함수 등록.
 *      (raise_softirq)
 */
void __init init_timers(void)
{
	init_timer_cpus();
	posix_cputimers_init_work();
	open_softirq(TIMER_SOFTIRQ, run_timer_softirq);
}

/**
 * msleep - sleep safely even with waitqueue interruptions
 * @msecs: Time in milliseconds to sleep for
 */
void msleep(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	while (timeout)
		timeout = schedule_timeout_uninterruptible(timeout);
}

EXPORT_SYMBOL(msleep);

/**
 * msleep_interruptible - sleep waiting for signals
 * @msecs: Time in milliseconds to sleep for
 */
unsigned long msleep_interruptible(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	while (timeout && !signal_pending(current))
		timeout = schedule_timeout_interruptible(timeout);
	return jiffies_to_msecs(timeout);
}

EXPORT_SYMBOL(msleep_interruptible);

/**
 * usleep_range - Sleep for an approximate time
 * @min: Minimum time in usecs to sleep
 * @max: Maximum time in usecs to sleep
 *
 * In non-atomic context where the exact wakeup time is flexible, use
 * usleep_range() instead of udelay().  The sleep improves responsiveness
 * by avoiding the CPU-hogging busy-wait of udelay(), and the range reduces
 * power usage by allowing hrtimers to take advantage of an already-
 * scheduled interrupt instead of scheduling a new one just for this sleep.
 */
/*
 * IAMROOT, 2023.03.24:
 * - papago
 *   usleep_range - 대략적인 시간 동안 휴면합니다.
 *   @min: 휴면을 위한 최소 시간(usecs).
 *   @max: 최대 절전 시간(usecs)입니다.
 *
 *   정확한 깨우기 시간이 유연한 비원자적 컨텍스트에서는 udelay() 대신
 *   usleep_range()를 사용합니다. 수면은 udelay()의 CPU 독주 바쁜 대기를
 *   피함으로써 응답성을 향상시키고 범위는 hrtimer가 이 수면을 위해 새
 *   인터럽트를 예약하는 대신 이미 예약된 인터럽트를 활용할 수 있도록 하여 
 *   전력 사용량을 줄입니다.
. 
 */
void __sched usleep_range(unsigned long min, unsigned long max)
{
	ktime_t exp = ktime_add_us(ktime_get(), min);
	u64 delta = (u64)(max - min) * NSEC_PER_USEC;

	for (;;) {
		__set_current_state(TASK_UNINTERRUPTIBLE);
		/* Do not return before the requested sleep time has elapsed */
		if (!schedule_hrtimeout_range(&exp, delta, HRTIMER_MODE_ABS))
			break;
	}
}
EXPORT_SYMBOL(usleep_range);
