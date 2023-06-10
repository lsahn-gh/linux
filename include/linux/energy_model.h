/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ENERGY_MODEL_H
#define _LINUX_ENERGY_MODEL_H
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/jump_label.h>
#include <linux/kobject.h>
#include <linux/rcupdate.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/topology.h>
#include <linux/types.h>

/**
 * struct em_perf_state - Performance state of a performance domain
 * @frequency:	The frequency in KHz, for consistency with CPUFreq
 * @power:	The power consumed at this level (by 1 CPU or by a registered
 *		device). It can be a total power: static and dynamic.
 * @cost:	The cost coefficient associated with this level, used during
 *		energy calculation. Equal to: power * max_frequency / frequency
 */
/*
 * IAMROOT. 2023.05.27:
 * - google-translate
 * struct em_perf_state - 성능 도메인의 성능 상태
 * @frequency: CPUFreq와의 일관성을 위한 KHz 단위의 주파수
 * @power: 이 수준에서 소비되는 전력(CPU 1개 또는 등록된 장치에 의해). 정적 및 동적 총
 * 전력이 될 수 있습니다.
 * @cost: 에너지 계산 중에 사용되는 이 수준과 관련된 비용 계수입니다.
 * 같음: power * max_frequency / frequency
 *
 * - cost 사용은 em_cpu_energy() 참고 
 *
 * - cost 계산은 _get_power(), em_create_perf_table() 함수 참고
 * ------- ex --------
 * - rk3399-opp.dtsi
 * 		opp00 {
 *			opp-hz = /bits/ 64 <408000000>;
 *			opp-microvolt = <825000 825000 1250000>;
 *		};
 *		..
 *		opp05 {
 *			opp-hz = /bits/ 64 <1416000000>;
 *			opp-microvolt = <1125000 1125000 1250000>;
 *		};
 *		
 * - 가정 첫번째 408MHZ에 대한 opp를 산출
 *   1. power 계산 예. power(mW) = (100 * mV * mV * MHZ)
 *   100 * 825 * 825 * (408000000/1000000) = 27769500000
 *   27769500000 / 1000000000 = 27.7695 = 27
 *
 *   2. frequency(kHZ) 계산 예
 *   408000000 / 1000 = 408000
 *
 *   3. cost. cost = fMAX(kHZ) * power(uW) / dst_f(kHZ) 
 *   1416000 * 27000 / 408000 = 93705.8823529 =  93705
 */
struct em_perf_state {
	unsigned long frequency;
	unsigned long power;
	unsigned long cost;
};

/**
 * struct em_perf_domain - Performance domain
 * @table:		List of performance states, in ascending order
 * @nr_perf_states:	Number of performance states
 * @milliwatts:		Flag indicating the power values are in milli-Watts
 *			or some other scale.
 * @cpus:		Cpumask covering the CPUs of the domain. It's here
 *			for performance reasons to avoid potential cache
 *			misses during energy calculations in the scheduler
 *			and simplifies allocating/freeing that memory region.
 *
 * In case of CPU device, a "performance domain" represents a group of CPUs
 * whose performance is scaled together. All CPUs of a performance domain
 * must have the same micro-architecture. Performance domains often have
 * a 1-to-1 mapping with CPUFreq policies. In case of other devices the @cpus
 * field is unused.
 */
struct em_perf_domain {
	struct em_perf_state *table;
	int nr_perf_states;
	int milliwatts;
	unsigned long cpus[];
};

#define em_span_cpus(em) (to_cpumask((em)->cpus))

#ifdef CONFIG_ENERGY_MODEL
#define EM_MAX_POWER 0xFFFF

/*
 * Increase resolution of energy estimation calculations for 64-bit
 * architectures. The extra resolution improves decision made by EAS for the
 * task placement when two Performance Domains might provide similar energy
 * estimation values (w/o better resolution the values could be equal).
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. 64-bit). The costs for increasing resolution when 32-bit
 * are pretty high and the returns do not justify the increased costs.
 */
/*
 * IAMROOT. 2023.05.31:
 * - google-translate
 * 64비트 아키텍처에 대한 에너지 추정 계산의 해상도를 높입니다. 추가 해상도는 두
 * 개의 성능 도메인이 유사한 에너지 추정 값을 제공할 수 있는 경우(더 나은 해상도
 * 없이 값이 동일할 수 있음) 작업 배치에 대한 EAS의 결정을 개선합니다.
 *
 * 이 증가된 해상도(예: 64비트)를 허용하기에 충분한 비트가 있는 경우에만 해상도를
 * 높입니다. 32비트에서 해상도를 높이는 데 드는 비용은 상당히 높고 수익은 증가된
 * 비용을 정당화하지 못합니다.
 */
#ifdef CONFIG_64BIT
#define em_scale_power(p) ((p) * 1000)
#else
#define em_scale_power(p) (p)
#endif

struct em_data_callback {
	/**
	 * active_power() - Provide power at the next performance state of
	 *		a device
	 * @power	: Active power at the performance state
	 *		(modified)
	 * @freq	: Frequency at the performance state in kHz
	 *		(modified)
	 * @dev		: Device for which we do this operation (can be a CPU)
	 *
	 * active_power() must find the lowest performance state of 'dev' above
	 * 'freq' and update 'power' and 'freq' to the matching active power
	 * and frequency.
	 *
	 * In case of CPUs, the power is the one of a single CPU in the domain,
	 * expressed in milli-Watts or an abstract scale. It is expected to
	 * fit in the [0, EM_MAX_POWER] range.
	 *
	 * Return 0 on success.
	 */
	/*
	 * IAMROOT. 2023.05.31:
	 * - google-translate
	 * active_power() - 장치의 다음 성능 상태에서 전력 제공
	 * @power : 성능 상태에서 유효 전력(수정됨)
	 * @freq : 성능 상태에서 kHz 단위의 주파수(수정됨)
	 * @dev : 이 작업을 수행하는 장치( CPU일 수 있음)
	 *
	 * active_power()는 'freq' 위에서 'dev'의 가장 낮은 성능 상태를 찾고
	 * 'power' 및 'freq'를 일치하는 활성 전력 및 주파수로 업데이트해야 합니다.
	 *
	 * CPU의 경우 전력은 도메인의 단일 CPU 중 하나이며 밀리와트 또는 추상적
	 * 척도로 표현됩니다. [0, EM_MAX_POWER] 범위에 맞을 것으로 예상됩니다.
	 *
	 * 성공하면 0을 반환합니다.
	 */
	int (*active_power)(unsigned long *power, unsigned long *freq,
			    struct device *dev);
};
#define EM_DATA_CB(_active_power_cb) { .active_power = &_active_power_cb }

struct em_perf_domain *em_cpu_get(int cpu);
struct em_perf_domain *em_pd_get(struct device *dev);
int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
				struct em_data_callback *cb, cpumask_t *span,
				bool milliwatts);
void em_dev_unregister_perf_domain(struct device *dev);

/**
 * em_cpu_energy() - Estimates the energy consumed by the CPUs of a
 *		performance domain
 * @pd		: performance domain for which energy has to be estimated
 * @max_util	: highest utilization among CPUs of the domain
 * @sum_util	: sum of the utilization of all CPUs in the domain
 * @allowed_cpu_cap	: maximum allowed CPU capacity for the @pd, which
 *			  might reflect reduced frequency (due to thermal)
 *
 * This function must be used only for CPU devices. There is no validation,
 * i.e. if the EM is a CPU type and has cpumask allocated. It is called from
 * the scheduler code quite frequently and that is why there is not checks.
 *
 * Return: the sum of the energy consumed by the CPUs of the domain assuming
 * a capacity state satisfying the max utilization of the domain.
 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   em_cpu_energy() - 성능 도메인의 CPU가 소비하는 에너지를 추정합니다.
 *
 *   @pd: 에너지를 추정해야 하는 성능 영역
 *   @max_util: 도메인의 CPU 중 가장 사용률이 높음
 *   @sum_util: 도메인의 모든 CPU 사용률 합계
 *   @allowed_cpu_cap: 감소된 주파수를 반영할 수 있는 @pd의 최대 허용 CPU 용량(열로 인해)
 *
 *   이 기능은 CPU 장치에만 사용해야 합니다. 유효성 검사가 없습니다. 즉, 
 *   EM이 CPU 유형이고 cpumask가 할당된 경우입니다. 스케줄러 코드에서 꽤 
 *   자주 호출되기 때문에 검사가 없습니다.
 *
 *   Return: 도메인의 최대 사용률을 충족하는 용량 상태를 가정하여 도메인의
 *   CPU에서 소비한 에너지의 합계입니다.
 *
 * @max_util max freq model 값
 * @sum_util energy model 합값
 * @allowed_cpu_cap 온도가 고려된 max cpu capa
 *
 * - @pd의 energy 소비량 추정. 수식결과는 아래 주석 참고.
 *
 * IAMROOT, 2023.06.07:
 * - @max_util에 해당하는 cost를 @sum_util에 적용한 pd의 총 에너지 합을 반환한다
 */
static inline unsigned long em_cpu_energy(struct em_perf_domain *pd,
				unsigned long max_util, unsigned long sum_util,
				unsigned long allowed_cpu_cap)
{
	unsigned long freq, scale_cpu;
	struct em_perf_state *ps;
	int i, cpu;

	if (!sum_util)
		return 0;

	/*
	 * In order to predict the performance state, map the utilization of
	 * the most utilized CPU of the performance domain to a requested
	 * frequency, like schedutil. Take also into account that the real
	 * frequency might be set lower (due to thermal capping). Thus, clamp
	 * max utilization to the allowed CPU capacity before calculating
	 * effective frequency.
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   성능 상태를 예측하기 위해 성능 영역에서 가장 많이 사용되는 CPU의 
 *   사용률을 schedutil과 같이 요청된 빈도에 매핑합니다. 실제 주파수가 
 *   더 낮게 설정될 수 있다는 점도 고려하십시오(열 캡핑으로 인해). 
 *   따라서 유효 주파수를 계산하기 전에 최대 사용률을 허용된 CPU 용량으로 
 *   고정합니다.
 *
 * - 최대 freq를 가르키는 ps를 가져온다.
 *
 * IAMROOT, 2023.06.06:
 * - pd->cpus는 capa가 모두 같을 것이므로 그중에 하나(아래서는 첫번째)를 가져온다.
 */
	cpu = cpumask_first(to_cpumask(pd->cpus));
	scale_cpu = arch_scale_cpu_capacity(cpu);
	ps = &pd->table[pd->nr_perf_states - 1];

/*
 * IAMROOT, 2023.06.03:
 * - 125%증가를 한후 온도가 고려된 cpu capa와 min비교를 하여 max_utll
 *   로 결과값을 내고, 계산된 결과값의 본래 cpu capa비율만큼 ps->frequency를
 *   줄인 값을 freq에 저장한다.
 *
 * - ex) max_util = 215, scale_cpu = 430, ps->frequency = 1.8G
 *   freq = 900M
 *
 * IAMROOT, 2023.06.06:
 * - @max_util에 해당하는 freq를 비율을 사용하여 예측하여 계산한다.
 */
	max_util = map_util_perf(max_util);
	max_util = min(max_util, allowed_cpu_cap);
	freq = map_util_freq(max_util, ps->frequency, scale_cpu);

	/*
	 * Find the lowest performance state of the Energy Model above the
	 * requested frequency.
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   요청된 주파수보다 높은 에너지 모델의 가장 낮은 성능 상태를 찾습니다.
 *
 * - freq랑 제일 높은쪽으로 가까운 ps를 찾는다.
 *   ex) freq = 900M, ps0 = 890, ps1 = 910 일때 ps1를 고른다.
 *
 * IAMROOT, 2023.06.06:
 * - 위에서 계산한 freq 이상인 첫번째 table을 찾는다. 찾은 ps는 ps->cost를
 *   가져와서 최종 nrg 계산에 사용한다.
 */
	for (i = 0; i < pd->nr_perf_states; i++) {
		ps = &pd->table[i];
		if (ps->frequency >= freq)
			break;
	}

	/*
	 * The capacity of a CPU in the domain at the performance state (ps)
	 * can be computed as:
	 *
	 *             ps->freq * scale_cpu
	 *   ps->cap = --------------------                          (1)
	 *                 cpu_max_freq
	 *
	 * So, ignoring the costs of idle states (which are not available in
	 * the EM), the energy consumed by this CPU at that performance state
	 * is estimated as:
	 *
	 *             ps->power * cpu_util
	 *   cpu_nrg = --------------------                          (2)
	 *                   ps->cap
	 *
	 * since 'cpu_util / ps->cap' represents its percentage of busy time.
	 *
	 *   NOTE: Although the result of this computation actually is in
	 *         units of power, it can be manipulated as an energy value
	 *         over a scheduling period, since it is assumed to be
	 *         constant during that interval.
	 *
	 * By injecting (1) in (2), 'cpu_nrg' can be re-expressed as a product
	 * of two terms:
	 *
	 *             ps->power * cpu_max_freq   cpu_util
	 *   cpu_nrg = ------------------------ * ---------          (3)
	 *                    ps->freq            scale_cpu
	 *
	 * The first term is static, and is stored in the em_perf_state struct
	 * as 'ps->cost'.
	 *
	 * Since all CPUs of the domain have the same micro-architecture, they
	 * share the same 'ps->cost', and the same CPU capacity. Hence, the
	 * total energy of the domain (which is the simple sum of the energy of
	 * all of its CPUs) can be factorized as:
	 *
	 *            ps->cost * \Sum cpu_util
	 *   pd_nrg = ------------------------                       (4)
	 *                  scale_cpu
	 */
/*
 * IAMROOT, 2023.06.03:
 * - papago
 *   performance state(ps)에서 도메인의 CPU 용량은 다음과 같이 계산할 수 있습니다.
 *             ps->freq * scale_cpu
 *   ps->cap = --------------------                          (1)
 *                 cpu_max_freq
 *
 *  따라서 유휴 상태 비용(EM에서는 사용할 수 없음)을 무시하면 해당 성능 
 *  상태에서 이 CPU가 소비하는 에너지는 다음과 같이 추정됩니다.
 *  (nrg : energy)
 *
 *             ps->power * cpu_util
 *   cpu_nrg = --------------------                          (2)
 *                   ps->cap
 *
 *  'cpu_util / ps->cap'은 바쁜 시간의 백분율을 나타냅니다.
 *
 *  NOTE: 이 계산의 결과는 실제로 전력 단위이지만 일정 기간 동안 
 *  일정하다고 가정하기 때문에 일정 기간 동안 에너지 값으로 조작할 수 있습니다.
 *
 *  (2)에 (1)을 삽입하면 'cpu_nrg'는 두 항의 곱으로 다시 표현할 수 있습니다.
 *
 *             ps->power * cpu_max_freq   cpu_util
 *   cpu_nrg = ------------------------ * ---------          (3)
 *                    ps->freq            scale_cpu
 *            ^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *                    ps->cost
 *
 *  첫 번째 용어는 정적이며 em_perf_state 구조체에 'ps->cost'로 저장됩니다.
 *
 *  도메인의 모든 CPU는 동일한 마이크로 아키텍처를 가지므로 동일한 'ps->cost'과 
 *  동일한 CPU 용량을 공유합니다. 따라서 도메인의 총 에너지 
 *  (모든 CPU 에너지의 단순 합)는 다음과 같이 분해할 수 있습니다.
 *
 *            ps->cost * \Sum cpu_util
 *   pd_nrg = ------------------------                       (4)
 *                  scale_cpu
 *
 * - ps : max_util의 값은 근전한 freq를 고를 때사용하여 ps를 골랐다.
 *   sun_util : energy model 합산값.
 *   scale_cpu : cpu최대성능
 */
	return ps->cost * sum_util / scale_cpu;
}

/**
 * em_pd_nr_perf_states() - Get the number of performance states of a perf.
 *				domain
 * @pd		: performance domain for which this must be done
 *
 * Return: the number of performance states in the performance domain table
 */
/*
 * IAMROOT. 2023.05.27:
 * - google-translate
 * em_pd_nr_perf_states() - perf domain의 performance states 수를 가져옵니다.
 * @pd : 이것이 수행되어야 하는 성능 도메인
 *
 * Return: 성능 도메인 테이블의 성능 상태 수
 *
 * - @pd의 em_pd_nr_perf_states를 가져온다.
 */
static inline int em_pd_nr_perf_states(struct em_perf_domain *pd)
{
	return pd->nr_perf_states;
}

#else
struct em_data_callback {};
#define EM_DATA_CB(_active_power_cb) { }

static inline
int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
				struct em_data_callback *cb, cpumask_t *span,
				bool milliwatts)
{
	return -EINVAL;
}
static inline void em_dev_unregister_perf_domain(struct device *dev)
{
}
static inline struct em_perf_domain *em_cpu_get(int cpu)
{
	return NULL;
}
static inline struct em_perf_domain *em_pd_get(struct device *dev)
{
	return NULL;
}
static inline unsigned long em_cpu_energy(struct em_perf_domain *pd,
			unsigned long max_util, unsigned long sum_util,
			unsigned long allowed_cpu_cap)
{
	return 0;
}
static inline int em_pd_nr_perf_states(struct em_perf_domain *pd)
{
	return 0;
}
#endif

#endif
