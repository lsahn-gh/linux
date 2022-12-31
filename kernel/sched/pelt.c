// SPDX-License-Identifier: GPL-2.0
/*
 * Per Entity Load Tracking
 *
 *  Copyright (C) 2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 *  Interactivity improvements by Mike Galbraith
 *  (C) 2007 Mike Galbraith <efault@gmx.de>
 *
 *  Various enhancements by Dmitry Adamushko.
 *  (C) 2007 Dmitry Adamushko <dmitry.adamushko@gmail.com>
 *
 *  Group scheduling enhancements by Srivatsa Vaddagiri
 *  Copyright IBM Corporation, 2007
 *  Author: Srivatsa Vaddagiri <vatsa@linux.vnet.ibm.com>
 *
 *  Scaled math optimizations by Thomas Gleixner
 *  Copyright (C) 2007, Thomas Gleixner <tglx@linutronix.de>
 *
 *  Adaptive scheduling granularity, math enhancements by Peter Zijlstra
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 *
 *  Move PELT related code from fair.c into this pelt.c file
 *  Author: Vincent Guittot <vincent.guittot@linaro.org>
 */

#include <linux/sched.h>
#include "sched.h"
#include "pelt.h"

/*
 * Approximate:
 *   val * y^n,    where y^32 ~= 0.5 (~1 scheduling period)
 */
/*
 * IAMROOT, 2022.12.31:
 * --- LOAD_AVG_PERIOD.
 * - 0.9785^n, n은 1부터 무한의 sum값
 *   sum = a/(1-r)
 *       = 0.9785 / (1 - 0.9785)
 *       = 0.9789 / 0.0215
 *       = 45.3777..
 *   이걸 이진화정수화.
 *   45.3777 * 1024 = 46467
 *   약 LOAD_AVG_PERIOD값 비슷하게 나온다.
 * - LOAD_AVG_PERIOD는 n = 0부터 무한대이므로 1024를 더해준다.
 *   46467 + 1024 = 47491
 *   내림한 수까지 고려하며ㅓㄴ LOAD_AVG_PERIOD값이 나올것이다.
 * ---
 * - ex) val = 8192
 *   -- n == 1인경우
	   x = mul_u64_u32_shr(8192, runnable_avg_yN_inv[1], 32);
	     = mul_u64_u32_shr(8192, 0xfa83b2da, 32);
		 = (8192 * 0xfa83b2da) >> 32
		 = 8016
 *
 *   -- n == 65인경우
		val >>= local_n / LOAD_AVG_PERIOD;
		      = 8192 >> (65 / 32)
			  = 8192 >> 2 //즉 0.5 * 0.5의 개념
			  = 2048 
		local_n %= LOAD_AVG_PERIOD;
		         = 65 % 32;
				 = 1
	   x = mul_u64_u32_shr(2048, runnable_avg_yN_inv[1], 32);
	     = (2048 * 0xfa83b2da) >> 32
		 = 2004
 * - @val이 n ms의 period로 진입했을때 LOAD_AVG_PERIOD와 runnable_avg_yN_inv table로
 *   배율만큼 감소킨다.
 */
static u64 decay_load(u64 val, u64 n)
{
	unsigned int local_n;

/*
 * IAMROOT, 2022.12.31:
 * - 너무크면 결국 0에 가깝다.
 */
	if (unlikely(n > LOAD_AVG_PERIOD * 63))
		return 0;

	/* after bounds checking we can collapse to 32-bit */
	local_n = n;

	/*
	 * As y^PERIOD = 1/2, we can combine
	 *    y^n = 1/2^(n/PERIOD) * y^(n%PERIOD)
	 * With a look-up table which covers y^n (n<PERIOD)
	 *
	 * To achieve constant time decay_load.
	 */

/*
 * IAMROOT, 2022.12.31:
 * - table의 범위는 32이고, table 끝은 0.5배라는 개념.
 * - n이 32번보다 크면 32배단위로 0.5배를 해주고, 나머지에 대해서만
 *   table을 사용한다.
 */
	if (unlikely(local_n >= LOAD_AVG_PERIOD)) {
		val >>= local_n / LOAD_AVG_PERIOD;
		local_n %= LOAD_AVG_PERIOD;
	}

/*
 * IAMROOT, 2022.12.31:
 * - table을 사용해 mult shift를 사용한다.
 */
	val = mul_u64_u32_shr(val, runnable_avg_yN_inv[local_n], 32);
	return val;
}

/*
 * IAMROOT, 2022.12.31:
 *           d1          d2           d3
 *           ^           ^            ^
 *           |           |            |
 *         |<->|<----------------->|<--->|
 * ... |---x---|------| ... |------|-----x (now)
 * 
 * - periods에 대한 decay를 수행한다.
 * - 기본적인 개념은 다음과 같다.
 * 1. d1은 d2에서 사용한 periods로 계산을 한다.
 * 2. d3은 새로온거니까 decay없이 남겨둔다.
 *
 * - 결론적으로는 그 전에 본 주석의 식대로 계산한다.
 *      d1 y^p + 1024 \Sum y^n + d3 y^0		(Step 2)
 *                     n=1
 */
static u32 __accumulate_pelt_segments(u64 periods, u32 d1, u32 d3)
{
	u32 c1, c2, c3 = d3; /* y^0 == 1 */

	/*
	 * c1 = d1 y^p
	 */
	c1 = decay_load((u64)d1, periods);

	/*
	 *            p-1
	 * c2 = 1024 \Sum y^n
	 *            n=1
	 *
	 *              inf        inf
	 *    = 1024 ( \Sum y^n - \Sum y^n - y^0 )
	 *              n=0        n=p
	 */

/*
 * IAMROOT, 2022.12.31:
 *           d1          d2           d3
 *           ^           ^            ^
 *           |           |            |
 *         |<->|<----------------->|<--->|
 * ... |---x---|------| ... |------|-----x (now)
 *
 * - LOAD_AVG_MAX - decay_load(LOAD_AVG_MAX, periods)
 *   0 ~ inf까지 의 시간에서 d2의 periods만큼 뺀시간
 *
 * - ex) d1 = 300, d2 = 4096, d3 = 800, periods = 5
 *
 *   c1 = decay_load(300, 5) 
 *      = (300 * 0xe5b906e6) >> 32
 *      = 269
 *   c2 = 47742 -  decay_load(47742, 5) - 1024
 *      = 47742 - mul_u64_u32_shr(47742, runnable_avg_yN_inv[5], 32) - 1024
 *      = 47742 - (47742 * 0xe5b906e6) >> 32 - 1024
 *		= 47742 - 42,841 - 1024
 * 	 	= 3877
 * 	 ret = 269 + 3877 + 800
 * 	     = 5046
 */
	c2 = LOAD_AVG_MAX - decay_load(LOAD_AVG_MAX, periods) - 1024;

	return c1 + c2 + c3;
}

/*
 * Accumulate the three separate parts of the sum; d1 the remainder
 * of the last (incomplete) period, d2 the span of full periods and d3
 * the remainder of the (incomplete) current period.
 *
 *           d1          d2           d3
 *           ^           ^            ^
 *           |           |            |
 *         |<->|<----------------->|<--->|
 * ... |---x---|------| ... |------|-----x (now)
 *
 *                           p-1
 * u' = (u + d1) y^p + 1024 \Sum y^n + d3 y^0
 *                           n=1
 *
 *    = u y^p +					(Step 1)
 *
 *                     p-1
 *      d1 y^p + 1024 \Sum y^n + d3 y^0		(Step 2)
 *                     n=1
 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *  합계의 세 부분을 합산하십시오. d1 마지막 (불완전한) 기간의 나머지, 
 *  d2 전체 기간의 범위 및 d3 (불완전한) 현재 기간의 나머지.
 *
 * @return decay가 됬으면 return periods. 아니면 0.
 *
 * - load_sum, runnable_sum, util_sum을 1ms단위의 periods주기 동안으로
 *   decay시킨다.
 * - periods밖으로 남겨진 값들은 decay없이 적산한다(d3)
 */
static __always_inline u32
accumulate_sum(u64 delta, struct sched_avg *sa,
	       unsigned long load, unsigned long runnable, int running)
{
	u32 contrib = (u32)delta; /* p == 0 -> delta < 1024 */
	u64 periods;


/*
 * IAMROOT, 2022.12.31:
 * - 이전에 남겨진 시간을 고려한다.
 */
	delta += sa->period_contrib;

/*
 * IAMROOT, 2022.12.31:
 * - 1ms단위의 주기가 몇번 생기는지 계산한다.
 */
	periods = delta / 1024; /* A period is 1024us (~1ms) */

	/*
	 * Step 1: decay old *_sum if we crossed period boundaries.
	 */
/*
 * IAMROOT, 2022.12.31:
 * - period동안의 load_sum, runnable_sum, util_sum을 decay 시킨다.
 */
	if (periods) {
/*
 * IAMROOT, 2022.12.31:
 *
 *           d1          d2           d3
 *           ^           ^            ^
 *           |           |            |
 *         |<->|<----------------->|<--->|
 * ... |---x---|------| ... |------|-----x (now)
 *         ^
 *         step1
 *
 *                           p-1
 * u' = (u + d1) y^p + 1024 \Sum y^n + d3 y^0
 *                           n=1
 *
 *    = u y^p +					(Step 1) <-- 이부분에 대한 계산.
 *
 *                     p-1
 *      d1 y^p + 1024 \Sum y^n + d3 y^0		(Step 2)
 *                     n=1
 * - 지나간 시간 만큼 u를 decay 시킨다.
 */
		sa->load_sum = decay_load(sa->load_sum, periods);
		sa->runnable_sum =
			decay_load(sa->runnable_sum, periods);
		sa->util_sum = decay_load((u64)(sa->util_sum), periods);

		/*
		 * Step 2
		 */
		delta %= 1024;
		if (load) {
			/*
			 * This relies on the:
			 *
			 * if (!load)
			 *	runnable = running = 0;
			 *
			 * clause from ___update_load_sum(); this results in
			 * the below usage of @contrib to disappear entirely,
			 * so no point in calculating it.
			 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   이것은 다음에 의존합니다.
 *
 *    if (!load)
 *	    runnable = running = 0;
 *
 *   ___update_load_sum()의 절; 이로 인해 아래 @contrib 사용이 완전히 
 *   사라지므로 계산할 필요가 없습니다.
 *
 * ---
 *   d1 : 1024 - sa->period_contrib
 *   d3 : delta
 *           d1          d2           d3
 *           ^           ^            ^
 *           |           |            |
 *         |<->|<----------------->|<--->|
 * ... |---x---|------| ... |------|-----x (now)
 *         <=============================>
 *     <-->             step2
 *      ^ sa->period_contrib
 * --
 *
 * - periods에 대한 d1 ~ d3까지의 decay된 값을 구한다.
 * - d1 = 1024 - sa->period_contrib 
 *   d3 = delta
 */
			contrib = __accumulate_pelt_segments(periods,
					1024 - sa->period_contrib, delta);
		}
	}

/*
 * IAMROOT, 2022.12.31:
 * - 남겨진 시간을 보관한다.
 */
	sa->period_contrib = delta;

/*
 * IAMROOT, 2022.12.31:
 * - decay했던 periods를 value에 적용하여 적산한다.
 */
	if (load)
		sa->load_sum += load * contrib;
	if (runnable)
		sa->runnable_sum += runnable * contrib << SCHED_CAPACITY_SHIFT;
	if (running)
		sa->util_sum += contrib << SCHED_CAPACITY_SHIFT;

	return periods;
}

/*
 * We can represent the historical contribution to runnable average as the
 * coefficients of a geometric series.  To do this we sub-divide our runnable
 * history into segments of approximately 1ms (1024us); label the segment that
 * occurred N-ms ago p_N, with p_0 corresponding to the current period, e.g.
 *
 * [<- 1024us ->|<- 1024us ->|<- 1024us ->| ...
 *      p0            p1           p2
 *     (now)       (~1ms ago)  (~2ms ago)
 *
 * Let u_i denote the fraction of p_i that the entity was runnable.
 *
 * We then designate the fractions u_i as our co-efficients, yielding the
 * following representation of historical load:
 *   u_0 + u_1*y + u_2*y^2 + u_3*y^3 + ...
 *
 * We choose y based on the with of a reasonably scheduling period, fixing:
 *   y^32 = 0.5
 *
 * This means that the contribution to load ~32ms ago (u_32) will be weighted
 * approximately half as much as the contribution to load within the last ms
 * (u_0).
 *
 * When a period "rolls over" and we have new u_0`, multiplying the previous
 * sum again by y is sufficient to update:
 *   load_avg = u_0` + y*(u_0 + u_1*y + u_2*y^2 + ... )
 *            = u_0 + u_1*y + u_2*y^2 + ... [re-labeling u_i --> u_{i+1}]
 */

/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   실행 가능한 평균에 대한 historical 기여도를 기하학적 계열의 계수로 나타낼 수 
 *   있습니다. 이를 위해 실행 가능한 기록을 약 1ms(1024us)의 세그먼트로 
 *   세분화합니다. 현재 기간에 해당하는 p_0을 사용하여 p_N 전에 N-ms 발생한 
 *   세그먼트에 레이블을 지정합니다.
 *
 *   [<- 1024us ->|<- 1024us ->|<- 1024us ->| ...
 *        p0            p1           p2
 *       (now)       (~1ms ago)  (~2ms ago)
 *  
 *   u_i는 엔티티가 실행 가능한 p_i의 비율을 나타냅니다.
 *
 *   그런 다음 분수 u_i를 계수로 지정하여 다음과 같은 과거 부하 표현을 생성합니다.
 *     u_0 + u_1*y + u_2*y^2 + u_3*y^3 + ...
 *
 *  합리적인 일정 기간을 기준으로 y를 선택하고 다음을 수정합니다.
 *     y^32 = 0.5
 *
 *  이는 ~32ms 전(u_32) 로드 기여도가 마지막 ms(u_0) 내 로드 기여도의 약 절반 
 *  정도 가중치가 부여됨을 의미합니다.
 *
 *  기간이 "롤오버"되고 새 u_0`이 있으면 이전 합계에 다시 y를 곱하면 
 *  업데이트하기에 충분합니다. 
 *   load_avg = u_0` + y*(u_0 + u_1*y + u_2*y^2 + ... )
 *            = u_0 + u_1*y + u_2*y^2 + ... [re-labeling u_i --> u_{i+1}]
 *
 * - 32ms가 지난시점에서 감소율이 50%가 되게 설계한 수식을 적용.
 * ---
 * - y값 대략적인 계산
 *   32logY = log0.5
 *   logY = log0.5 / 32
 *   Y = 10^(log0.5 / 32)
 *     = 0.9786
 * ---
 *  @running curr가 지금 동작중인지의 여부.
 *  @runnable run을 할수있는 상태를 의미.
 *
 * --- running과 runable의 차이
 *  A    B    running runable
 *  20%  30%  50%     50%
 *  100% 50%  100%    150%          
 *  
 * ---
 *
 * @return decay가 수행됬으면 return 1
 *
 * - last_update_time을 갱신하고 load, runnable, util을 누적한다.
 */
static __always_inline int
___update_load_sum(u64 now, struct sched_avg *sa,
		  unsigned long load, unsigned long runnable, int running)
{
	u64 delta;

	delta = now - sa->last_update_time;
	/*
	 * This should only happen when time goes backwards, which it
	 * unfortunately does during sched clock init when we swap over to TSC.
	 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   이것은 시간이 거꾸로 갈 때만 발생해야 합니다. 불행하게도 TSC로 전환할 때 
 *   sched clock init 중에 발생합니다.
. 
 */
	if ((s64)delta < 0) {
		sa->last_update_time = now;
		return 0;
	}

	/*
	 * Use 1024ns as the unit of measurement since it's a reasonable
	 * approximation of 1us and fast to compute.
	 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   1us의 합리적인 근사치이고 계산 속도가 빠르므로 1024ns를 측정 단위로 사용합니다.
 * - us단위로 계산을 진행한다. 1us이상인 경우에만 처리한다.
 */
	delta >>= 10;
	if (!delta)
		return 0;


/*
 * IAMROOT, 2022.12.31:
 * - delta(us)를 일단 누적시킨다.
 */
	sa->last_update_time += delta << 10;

	/*
	 * running is a subset of runnable (weight) so running can't be set if
	 * runnable is clear. But there are some corner cases where the current
	 * se has been already dequeued but cfs_rq->curr still points to it.
	 * This means that weight will be 0 but not running for a sched_entity
	 * but also for a cfs_rq if the latter becomes idle. As an example,
	 * this happens during idle_balance() which calls
	 * update_blocked_averages().
	 *
	 * Also see the comment in accumulate_sum().
	 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   running은 runnable(가중치)의 하위 집합이므로 runnable이 명확한 경우 
 *   running을 설정할 수 없습니다. 그러나 현재 se가 이미 대기열에서 
 *   제거되었지만 cfs_rq->curr이 여전히 그것을 가리키는 코너 케이스가 있습니다.
 *   이는 가중치가 0이 되지만 sched_entity에 대해 실행되지 않고 cfs_rq가 유휴 
 *   상태가 되면 cfs_rq에 대해서도 실행됨을 의미합니다. 예를 들어, 이것은 
 *   update_blocked_averages()를 호출하는 idle_balance() 중에 발생합니다.
 *   
 *   accumulate_sum()의 주석도 참조하십시오.
 *
 * - corner case 처리. load == 0 이면 se가 rq에서 이미 제거되었다.
 *   그 와중에 여기에 들어온 상태. 계산을 안하기 위해 0처리.
 */
	if (!load)
		runnable = running = 0;

	/*
	 * Now we know we crossed measurement unit boundaries. The *_avg
	 * accrues by two steps:
	 *
	 * Step 1: accumulate *_sum since last_update_time. If we haven't
	 * crossed period boundaries, finish.
	 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   이제 우리는 측정 단위 경계를 넘었다는 것을 알고 있습니다. *_avg는 
 *   두 단계로 발생합니다.
 *
 *   1 단계: last_update_time 이후 *_sum을 누적합니다. 기간 경계를 넘지 
 *   않았다면 끝내십시오.
 *
 * - delta 시간에대해서 load, runnable, util을 누적시킨다.
 */
	if (!accumulate_sum(delta, sa, load, runnable, running))
		return 0;

	return 1;
}

/*
 * When syncing *_avg with *_sum, we must take into account the current
 * position in the PELT segment otherwise the remaining part of the segment
 * will be considered as idle time whereas it's not yet elapsed and this will
 * generate unwanted oscillation in the range [1002..1024[.
 *
 * The max value of *_sum varies with the position in the time segment and is
 * equals to :
 *
 *   LOAD_AVG_MAX*y + sa->period_contrib
 *
 * which can be simplified into:
 *
 *   LOAD_AVG_MAX - 1024 + sa->period_contrib
 *
 * because LOAD_AVG_MAX*y == LOAD_AVG_MAX-1024
 *
 * The same care must be taken when a sched entity is added, updated or
 * removed from a cfs_rq and we need to update sched_avg. Scheduler entities
 * and the cfs rq, to which they are attached, have the same position in the
 * time segment because they use the same clock. This means that we can use
 * the period_contrib of cfs_rq when updating the sched_avg of a sched_entity
 * if it's more convenient.
 */
/*
 * IAMROOT, 2022.12.31:
 * - papago
 *   *_avg를 *_sum과 동기화할 때 PELT 세그먼트의 현재 위치를 고려해야 합니다.
 *   그렇지 않으면 세그먼트의 나머지 부분이 아직 경과되지 않은 유휴 시간으로 
 *   간주되어 범위 [1002..1024]에서 원치 않는 진동이 생성됩니다.
 *
 *   *_sum의 최대값은 시간 세그먼트의 위치에 따라 다르며 다음과 같습니다.
 *
 *   LOAD_AVG_MAX*y + sa->period_contrib
 *
 *   다음과 같이 단순화할 수 있습니다.
 *
 *   LOAD_AVG_MAX - 1024 + sa->period_contrib
 *
 *   LOAD_AVG_MAX*y == LOAD_AVG_MAX-1024이기 때문에
 *
 *   sched 엔터티가 cfs_rq에서 추가, 업데이트 또는 제거되고 sched_avg를 
 *   업데이트해야 하는 경우에도 동일한 주의를 기울여야 합니다. 스케줄러 
 *   엔티티와 이들이 연결된 cfs rq는 동일한 시계를 사용하기 때문에 시간 
 *   세그먼트에서 동일한 위치를 갖습니다. 이는 더 편리하다면 sched_entity의 
 *   sched_avg를 업데이트할 때 cfs_rq의 period_contrib를 사용할 수 있음을 
 *   의미합니다.
 *
 * - 기존 sum에 대한 avg를 구한다.
 * - @load는 일종의 배율 개념으로 작동(1 or weight)
 *   __update_load_avg_se() : se_weight(se)로 들어온다.
 */
static __always_inline void
___update_load_avg(struct sched_avg *sa, unsigned long load)
{
	u32 divider = get_pelt_divider(sa);

	/*
	 * Step 2: update *_avg.
	 */
	sa->load_avg = div_u64(load * sa->load_sum, divider);
	sa->runnable_avg = div_u64(sa->runnable_sum, divider);
	WRITE_ONCE(sa->util_avg, sa->util_sum / divider);
}

/*
 * sched_entity:
 *
 *   task:
 *     se_weight()   = se->load.weight
 *     se_runnable() = !!on_rq
 *
 *   group: [ see update_cfs_group() ]
 *     se_weight()   = tg->weight * grq->load_avg / tg->load_avg
 *     se_runnable() = grq->h_nr_running
 *
 *   runnable_sum = se_runnable() * runnable = grq->runnable_sum
 *   runnable_avg = runnable_sum
 *
 *   load_sum := runnable
 *   load_avg = se_weight(se) * load_sum
 *
 * cfq_rq:
 *
 *   runnable_sum = \Sum se->avg.runnable_sum
 *   runnable_avg = \Sum se->avg.runnable_avg
 *
 *   load_sum = \Sum se_weight(se) * se->avg.load_sum
 *   load_avg = \Sum se->avg.load_avg
 */

int __update_load_avg_blocked_se(u64 now, struct sched_entity *se)
{
	if (___update_load_sum(now, &se->avg, 0, 0, 0)) {
		___update_load_avg(&se->avg, se_weight(se));
		trace_pelt_se_tp(se);
		return 1;
	}

	return 0;
}

/*
 * IAMROOT, 2022.12.22:
 * @return 갱신된게 있으면 1. 없으면 0
 *
 * - @now시간을 기준으로 @se에 대한 load sum과 load avg를 구한다.
 */
int __update_load_avg_se(u64 now, struct cfs_rq *cfs_rq, struct sched_entity *se)
{
/*
 * IAMROOT, 2022.12.31:
 * - on_rq를 load로 판단한다.
 * - cfs_rq->curr == se
 *   curr와 일치하면 현재 running중인 task라는것. 그래서 running의 의미가 된다.
 */
	if (___update_load_sum(now, &se->avg, !!se->on_rq, se_runnable(se),
				cfs_rq->curr == se)) {

/*
 * IAMROOT, 2022.12.31:
 * - decay(충분한 시간이 흐름)가 됬다면 아래 구문들을 수행한다.
 * - avg를 구한다.
 * - util_est을 change가능으로 변경한다.
 */
		___update_load_avg(&se->avg, se_weight(se));
		cfs_se_util_change(&se->avg);
		trace_pelt_se_tp(se);
		return 1;
	}

	return 0;
}

/*
 * IAMROOT, 2022.12.31:
 * - @now시간을 기준으로 @cfs_rq에 대한 load sum과 load avg를 구한다.
 * - cfs_rq->load.weight
 *   ex) nice가 1024인 task가 3개있ㅎ으면 3072가 된다.
 */
int __update_load_avg_cfs_rq(u64 now, struct cfs_rq *cfs_rq)
{
	if (___update_load_sum(now, &cfs_rq->avg,
				scale_load_down(cfs_rq->load.weight),
				cfs_rq->h_nr_running,
				cfs_rq->curr != NULL)) {

		___update_load_avg(&cfs_rq->avg, 1);
		trace_pelt_cfs_tp(cfs_rq);
		return 1;
	}

	return 0;
}

/*
 * rt_rq:
 *
 *   util_sum = \Sum se->avg.util_sum but se->avg.util_sum is not tracked
 *   util_sum = cpu_scale * load_sum
 *   runnable_sum = util_sum
 *
 *   load_avg and runnable_avg are not supported and meaningless.
 *
 */

int update_rt_rq_load_avg(u64 now, struct rq *rq, int running)
{
	if (___update_load_sum(now, &rq->avg_rt,
				running,
				running,
				running)) {

		___update_load_avg(&rq->avg_rt, 1);
		trace_pelt_rt_tp(rq);
		return 1;
	}

	return 0;
}

/*
 * dl_rq:
 *
 *   util_sum = \Sum se->avg.util_sum but se->avg.util_sum is not tracked
 *   util_sum = cpu_scale * load_sum
 *   runnable_sum = util_sum
 *
 *   load_avg and runnable_avg are not supported and meaningless.
 *
 */

int update_dl_rq_load_avg(u64 now, struct rq *rq, int running)
{
	if (___update_load_sum(now, &rq->avg_dl,
				running,
				running,
				running)) {

		___update_load_avg(&rq->avg_dl, 1);
		trace_pelt_dl_tp(rq);
		return 1;
	}

	return 0;
}

#ifdef CONFIG_SCHED_THERMAL_PRESSURE
/*
 * thermal:
 *
 *   load_sum = \Sum se->avg.load_sum but se->avg.load_sum is not tracked
 *
 *   util_avg and runnable_load_avg are not supported and meaningless.
 *
 * Unlike rt/dl utilization tracking that track time spent by a cpu
 * running a rt/dl task through util_avg, the average thermal pressure is
 * tracked through load_avg. This is because thermal pressure signal is
 * time weighted "delta" capacity unlike util_avg which is binary.
 * "delta capacity" =  actual capacity  -
 *			capped capacity a cpu due to a thermal event.
 */

int update_thermal_load_avg(u64 now, struct rq *rq, u64 capacity)
{
	if (___update_load_sum(now, &rq->avg_thermal,
			       capacity,
			       capacity,
			       capacity)) {
		___update_load_avg(&rq->avg_thermal, 1);
		trace_pelt_thermal_tp(rq);
		return 1;
	}

	return 0;
}
#endif

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
/*
 * irq:
 *
 *   util_sum = \Sum se->avg.util_sum but se->avg.util_sum is not tracked
 *   util_sum = cpu_scale * load_sum
 *   runnable_sum = util_sum
 *
 *   load_avg and runnable_avg are not supported and meaningless.
 *
 */

int update_irq_load_avg(struct rq *rq, u64 running)
{
	int ret = 0;

	/*
	 * We can't use clock_pelt because irq time is not accounted in
	 * clock_task. Instead we directly scale the running time to
	 * reflect the real amount of computation
	 */
	running = cap_scale(running, arch_scale_freq_capacity(cpu_of(rq)));
	running = cap_scale(running, arch_scale_cpu_capacity(cpu_of(rq)));

	/*
	 * We know the time that has been used by interrupt since last update
	 * but we don't when. Let be pessimistic and assume that interrupt has
	 * happened just before the update. This is not so far from reality
	 * because interrupt will most probably wake up task and trig an update
	 * of rq clock during which the metric is updated.
	 * We start to decay with normal context time and then we add the
	 * interrupt context time.
	 * We can safely remove running from rq->clock because
	 * rq->clock += delta with delta >= running
	 */
	ret = ___update_load_sum(rq->clock - running, &rq->avg_irq,
				0,
				0,
				0);
	ret += ___update_load_sum(rq->clock, &rq->avg_irq,
				1,
				1,
				1);

	if (ret) {
		___update_load_avg(&rq->avg_irq, 1);
		trace_pelt_irq_tp(rq);
	}

	return ret;
}
#endif
