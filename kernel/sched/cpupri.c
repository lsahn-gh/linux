// SPDX-License-Identifier: GPL-2.0-only
/*
 *  kernel/sched/cpupri.c
 *
 *  CPU priority management
 *
 *  Copyright (C) 2007-2008 Novell
 *
 *  Author: Gregory Haskins <ghaskins@novell.com>
 *
 *  This code tracks the priority of each CPU so that global migration
 *  decisions are easy to calculate.  Each CPU can be in a state as follows:
 *
 *                 (INVALID), NORMAL, RT1, ... RT99, HIGHER
 *
 *  going from the lowest priority to the highest.  CPUs in the INVALID state
 *  are not eligible for routing.  The system maintains this state with
 *  a 2 dimensional bitmap (the first for priority class, the second for CPUs
 *  in that class).  Therefore a typical application without affinity
 *  restrictions can find a suitable CPU with O(1) complexity (e.g. two bit
 *  searches).  For tasks with affinity restrictions, the algorithm has a
 *  worst case complexity of O(min(101, nr_domcpus)), though the scenario that
 *  yields the worst case search is fairly contrived.
 */
#include "sched.h"

/*
 * p->rt_priority   p->prio   newpri   cpupri
 *
 *				              -1       -1(CPUPRI_INVALID)
 *
 *			0(RT0)   99       99       0(CPUPRI_NORMAL)
 *
 *		    1(RT1)   98       98        1
 *	      ...
 *	       49        50       50       49
 *	       50        49       49       50
 *	      ...
 *	       99(RT99)   0        0       99
 *
 *				             100	  100 (CPUPRI_HIGHER)
 */
/*
 * IAMROOT, 2023.02.11:
 * - 원래 prio는 낮을수록 우선순위가 높은데, 이를 역으로 변환하여
 *   높을수록 우선순위가 높도록 한다.
 *   (cpupri 또는 user 관점 priority는 번호가 높을수록 우선순위가 높다.)
 * ----
 * - 옛날에는 102개 체재 였다(idle이란게 있었다.). 지금은 idle이 빠져 
 *   101개 체재가 됬다.
 *   101개 체재에선 RT0을 NORMAL과 동일하게 처리한다.
 */
static int convert_prio(int prio)
{
	int cpupri;

	switch (prio) {
	case CPUPRI_INVALID:
		cpupri = CPUPRI_INVALID;	/* -1 */
		break;

	case 0 ... 98:
		cpupri = MAX_RT_PRIO-1 - prio;	/* 1 ... 99 */
		break;

	case MAX_RT_PRIO-1:
		cpupri = CPUPRI_NORMAL;		/*  0 */
		break;

	case MAX_RT_PRIO:
/*
 * IAMROOT, 2023.02.11:
 * - deadline용
 */
		cpupri = CPUPRI_HIGHER;		/* 100 */
		break;
	}

	return cpupri;
}

/*
 * IAMROOT, 2023.02.11:
 * - @p->cpumask와 @idx 우선순위에 해당하는 cpupri에 둘다 포함되는 cpu가 
 *   있으면 return 1.
 *   @lowest_mask 가 NULL이 아니면 겹치는 cpu가 lowest_mask에 기록된다.
 */
static inline int __cpupri_find(struct cpupri *cp, struct task_struct *p,
				struct cpumask *lowest_mask, int idx)
{

/*
 * IAMROOT, 2023.02.11:
 * - @cp의 @idx에 
 */
	struct cpupri_vec *vec  = &cp->pri_to_cpu[idx];
	int skip = 0;

	if (!atomic_read(&(vec)->count))
		skip = 1;
	/*
	 * When looking at the vector, we need to read the counter,
	 * do a memory barrier, then read the mask.
	 *
	 * Note: This is still all racy, but we can deal with it.
	 *  Ideally, we only want to look at masks that are set.
	 *
	 *  If a mask is not set, then the only thing wrong is that we
	 *  did a little more work than necessary.
	 *
	 *  If we read a zero count but the mask is set, because of the
	 *  memory barriers, that can only happen when the highest prio
	 *  task for a run queue has left the run queue, in which case,
	 *  it will be followed by a pull. If the task we are processing
	 *  fails to find a proper place to go, that pull request will
	 *  pull this task if the run queue is running at a lower
	 *  priority.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   벡터를 볼 때 카운터를 읽고 메모리 배리어를 수행한 다음 마스크를 읽어야 
 *   합니다.
 *
 *   Note: 이것은 여전히 정교하지만 처리 할 수 있습니다.
 *   이상적으로는 설정된 마스크만 보고자 합니다.
 *
 *   마스크가 설정되지 않은 경우 유일한 잘못된 점은 필요한 것보다 조금 더 
 *   많은 작업을 수행한 것입니다.
 *
 *   메모리 장벽으로 인해 0 카운트를 읽었지만 마스크가 설정된 경우 실행 
 *   대기열에 대한 가장 높은 우선 순위 작업이 실행 대기열을 떠났을 때만 
 *   발생할 수 있으며 이 경우 풀이 이어집니다. 우리가 처리하고 있는 작업이 
 *   갈 적절한 위치를 찾는 데 실패하면 실행 대기열이 낮은 우선순위에서 실행 
 *   중인 경우 풀 요청이 이 작업을 풀합니다.
 */
	smp_rmb();

	/* Need to do the rmb for every iteration */
/*
 * IAMROOT, 2023.02.11:
 * - 해당 우선순위에 해당하는 cpu가 하나도 없으면 return.
 */
	if (skip)
		return 0;

/*
 * IAMROOT, 2023.02.11:
 * - 미포함. return 0.
 */
	if (cpumask_any_and(&p->cpus_mask, vec->mask) >= nr_cpu_ids)
		return 0;

/*
 * IAMROOT, 2023.02.11:
 * - cpu_mask와 vec mask 와 겹치는 mask가 있으면 return 1.
 *   없으면 return 0
 */
	if (lowest_mask) {
		cpumask_and(lowest_mask, &p->cpus_mask, vec->mask);

		/*
		 * We have to ensure that we have at least one bit
		 * still set in the array, since the map could have
		 * been concurrently emptied between the first and
		 * second reads of vec->mask.  If we hit this
		 * condition, simply act as though we never hit this
		 * priority level and continue on.
		 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   vec->mask의 첫 번째 읽기와 두 번째 읽기 사이에 맵이 동시에 비워질 
 *   수 있기 때문에 배열에 적어도 하나의 비트가 여전히 설정되어 있는지 
 *   확인해야 합니다. 이 조건에 도달하면 이 우선 순위 수준에 도달하지 않은 
 *   것처럼 행동하고 계속 진행하십시오.
 */
		if (cpumask_empty(lowest_mask))
			return 0;
	}

	return 1;
}

/*
 * IAMROOT, 2023.02.11:
 * - fitness_fn 수행을 안하고 그냥 @p의 우선순위 범위에 대해서 @cp에 
 *   해당하는 cpu를 찾는다.
 */
int cpupri_find(struct cpupri *cp, struct task_struct *p,
		struct cpumask *lowest_mask)
{
	return cpupri_find_fitness(cp, p, lowest_mask, NULL);
}

/**
 * cpupri_find_fitness - find the best (lowest-pri) CPU in the system
 * @cp: The cpupri context
 * @p: The task
 * @lowest_mask: A mask to fill in with selected CPUs (or NULL)
 * @fitness_fn: A pointer to a function to do custom checks whether the CPU
 *              fits a specific criteria so that we only return those CPUs.
 *
 * Note: This function returns the recommended CPUs as calculated during the
 * current invocation.  By the time the call returns, the CPUs may have in
 * fact changed priorities any number of times.  While not ideal, it is not
 * an issue of correctness since the normal rebalancer logic will correct
 * any discrepancies created by racing against the uncertainty of the current
 * priority configuration.
 *
 * Return: (int)bool - CPUs were found
 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   cpupri_find_fitness - 시스템에서 최상의(가장 낮은 pri) CPU 찾기 
 *   @cp: cpupri 컨텍스트 
 *   @p: 작업 
 *   @lowest_mask: 선택한 CPU(또는 NULL)로 채울 마스크 
 *   @fitness_fn: 사용자 지정 검사를 수행하는 함수에 대한 포인터는 해당 
 *   CPU만 반환하도록 CPU가 특정 기준에 맞는지 여부를 확인합니다.
 *
 *   참고: 이 함수는 현재 호출 중에 계산된 권장 CPU를 반환합니다. 
 *   호출이 반환될 때까지 CPU는 실제로 여러 번 우선 순위를 변경했을 수 
 *   있습니다. 이상적이지는 않지만 일반 리밸런서 로직이 현재 우선 순위 
 *   구성의 불확실성에 대한 경주로 인해 생성된 모든 불일치를 수정하므로 
 *   정확성의 문제는 아닙니다.
 *
 *   return: (int)bool - CPU가 발견되었습니다.  
 *
 * - 1. @p의 우선순위범위에서 @cp 범위와 겹치는 cpu를 찾는다.
 *   2. @lowest_mask, @fitness_fn 이 있는 경우 찾은 cpu를 기록하고
 *   fitness_fn을 수행하여 성공한 cpu를 lowest_mask에 기록한다.
 *   3. @fitness_fn이 없는 경우 cpu를 찾기만 하고 끝낸다.
 */
int cpupri_find_fitness(struct cpupri *cp, struct task_struct *p,
		struct cpumask *lowest_mask,
		bool (*fitness_fn)(struct task_struct *p, int cpu))
{
/*
 * IAMROOT, 2023.02.11:
 * - task priority로 max priority를 구한다.
 *   자신보다 낮는것을 찾아야되는 상황이다.
 */
	int task_pri = convert_prio(p->prio);
	int idx, cpu;

	BUG_ON(task_pri >= CPUPRI_NR_PRIORITIES);

/*
 * IAMROOT, 2023.02.11:
 * - 낮은것부터 높은순으로 iterate
 */
	for (idx = 0; idx < task_pri; idx++) {

/*
 * IAMROOT, 2023.02.11:
 * - @cp의 idx 우선순위의 cpumask와 @p의 cpumask 둘다 포함되는 cpu를
 *   lowest_mask에 기록한다. 없으면 continue.
 */
		if (!__cpupri_find(cp, p, lowest_mask, idx))
			continue;

/*
 * IAMROOT, 2023.02.11:
 * - @lowest_mask, fitness_fn 가 인자로 안들어왔으면 그냥 cpu를 찾고 
 *   끝낸다.
 */
		if (!lowest_mask || !fitness_fn)
			return 1;

		/* Ensure the capacity of the CPUs fit the task */
/*
 * IAMROOT, 2023.02.11:
 * - 찾아낸 lowest_mask에 대해서 @fitness_fn을 수행한다.
 */
		for_each_cpu(cpu, lowest_mask) {
			if (!fitness_fn(p, cpu))
/*
 * IAMROOT, 2023.02.11:
 * - 적합하지 않으면 lowest_mask에서 해당 cpu를 clear한다.
 */
				cpumask_clear_cpu(cpu, lowest_mask);
		}

		/*
		 * If no CPU at the current priority can fit the task
		 * continue looking
		 */
/*
 * IAMROOT, 2023.02.11:
 * - 전부 실패한 개념이되므로 다음 priority로 이동.
 */
		if (cpumask_empty(lowest_mask))
			continue;

/*
 * IAMROOT, 2023.02.11:
 * - fitness_fn을 해당 priority에서 성공한 lowest_mask cpu가 있었다면 
 *   return 1.
 */
		return 1;
	}

	/*
	 * If we failed to find a fitting lowest_mask, kick off a new search
	 * but without taking into account any fitness criteria this time.
	 *
	 * This rule favours honouring priority over fitting the task in the
	 * correct CPU (Capacity Awareness being the only user now).
	 * The idea is that if a higher priority task can run, then it should
	 * run even if this ends up being on unfitting CPU.
	 *
	 * The cost of this trade-off is not entirely clear and will probably
	 * be good for some workloads and bad for others.
	 *
	 * The main idea here is that if some CPUs were over-committed, we try
	 * to spread which is what the scheduler traditionally did. Sys admins
	 * must do proper RT planning to avoid overloading the system if they
	 * really care.
	 */
/*
 * IAMROOT, 2023.02.11:
 * - papago
 *   적합한 최하위 마스크를 찾지 못한 경우 이번에는 적합성 기준을 고려하지 
 *   않고 새 검색을 시작합니다. 
 *
 *   이 규칙은 올바른 CPU에 작업을 맞추는 것보다 우선 순위를 존중하는 것을 
 *   선호합니다(현재 용량 인식이 유일한 사용자임).
 *   아이디어는 더 높은 우선 순위 작업이 실행될 수 있는 경우 적합하지 않은 
 *   CPU에서 종료되더라도 실행되어야 한다는 것입니다.
 *
 *   이 트레이드 오프의 비용은 완전히 명확하지 않으며 일부 워크로드에는 좋고 
 *   다른 워크로드에는 좋지 않을 수 있습니다.
 *
 *   여기서 주요 아이디어는 일부 CPU가 과도하게 커밋된 경우 스케줄러가 
 *   전통적으로 수행한 대로 확산을 시도한다는 것입니다. 시스템 관리자는 
 *   정말로 관심이 있는 경우 시스템 과부하를 피하기 위해 적절한 RT 계획을 
 *   수행해야 합니다.
 *
 * - lowest cpu를 priority범위에서 하나도 못찾았다. fitness_fn 함수 수행
 *   없이 한번더 시도한다.
 */
	if (fitness_fn)
		return cpupri_find(cp, p, lowest_mask);

/*
 * IAMROOT, 2023.02.11:
 * - fitness_fn 수행요청없이도 cpu를 못찾은상태. return 0.
 */
	return 0;
}

/**
 * cpupri_set - update the CPU priority setting
 * @cp: The cpupri context
 * @cpu: The target CPU
 * @newpri: The priority (INVALID,NORMAL,RT1-RT99,HIGHER) to assign to this CPU
 *
 * Note: Assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
/*
 * IAMROOT, 2023.02.18:
 * - cpupri은 다음과 같은 2개로 관리한다.
 *   1. pri_to_cpu : pri별 증감
 *   2. cpu_to_pri : 현재 pri 변경.
 *
 * - 현재 cpu별 pri에 대해서 @newpri로 갱신한다.
 * - @cpu에 대한 pri별 관리하는 것들에 대해선 @newpri에 대해서는 증가,
 *   oldpri에 대해선 감소를 수행한다.
 */
void cpupri_set(struct cpupri *cp, int cpu, int newpri)
{
	int *currpri = &cp->cpu_to_pri[cpu];
	int oldpri = *currpri;
	int do_mb = 0;

	newpri = convert_prio(newpri);

	BUG_ON(newpri >= CPUPRI_NR_PRIORITIES);

	if (newpri == oldpri)
		return;

	/*
	 * If the CPU was currently mapped to a different value, we
	 * need to map it to the new value then remove the old value.
	 * Note, we must add the new value first, otherwise we risk the
	 * cpu being missed by the priority loop in cpupri_find.
	 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   CPU가 현재 다른 값에 매핑된 경우 새 값에 매핑한 다음 이전 값을
 *   제거해야 합니다.
 *   새 값을 먼저 추가해야 합니다. 그렇지 않으면 cpupri_find의 우선
 *   순위 루프에서 CPU를 놓칠 위험이 있습니다.
 *
 * - newpri에 대해선 증가, 없어지는 oldpri에 대해선 감소를 수행한다.
 */
	if (likely(newpri != CPUPRI_INVALID)) {
		struct cpupri_vec *vec = &cp->pri_to_cpu[newpri];

		cpumask_set_cpu(cpu, vec->mask);
		/*
		 * When adding a new vector, we update the mask first,
		 * do a write memory barrier, and then update the count, to
		 * make sure the vector is visible when count is set.
		 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   새 벡터를 추가할 때 먼저 마스크를 업데이트하고 쓰기 메모리 배리어를 
 *   수행한 다음 카운트가 설정될 때 벡터가 표시되도록 카운트를 
 *   업데이트합니다.
 */
		smp_mb__before_atomic();
		atomic_inc(&(vec)->count);
		do_mb = 1;
	}
	if (likely(oldpri != CPUPRI_INVALID)) {
		struct cpupri_vec *vec  = &cp->pri_to_cpu[oldpri];

		/*
		 * Because the order of modification of the vec->count
		 * is important, we must make sure that the update
		 * of the new prio is seen before we decrement the
		 * old prio. This makes sure that the loop sees
		 * one or the other when we raise the priority of
		 * the run queue. We don't care about when we lower the
		 * priority, as that will trigger an rt pull anyway.
		 *
		 * We only need to do a memory barrier if we updated
		 * the new priority vec.
		 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   vec->count의 수정 순서가 중요하기 때문에 이전 prio를 감소시키기 전에 
 *   새로운 prio의 업데이트가 보이는지 확인해야 합니다. 이렇게 하면 
 *   실행 대기열의 우선 순위를 높일 때 루프가 둘 중 하나를 볼 수 있습니다. 
 *   어쨌든 rt 풀을 트리거하므로 우선 순위를 낮추는 경우에는 신경 쓰지 
 *   않습니다.
 *
 *   새로운 우선 순위 vec를 업데이트한 경우에만 메모리 장벽을 수행하면 
 *   됩니다.
 *
 * - newpri가 수행된경우 barrier를 한다.
 */
		if (do_mb)
			smp_mb__after_atomic();

		/*
		 * When removing from the vector, we decrement the counter first
		 * do a memory barrier and then clear the mask.
		 */
/*
 * IAMROOT, 2023.02.18:
 * - papago
 *   벡터에서 제거할 때 카운터를 먼저 감소시키고 메모리 배리어를 수행한 
 *   다음 마스크를 지웁니다.
 *
 * - 제거될때는 count를 먼저 빼고 mask를 clear한다.
 */
		atomic_dec(&(vec)->count);
		smp_mb__after_atomic();
		cpumask_clear_cpu(cpu, vec->mask);
	}

/*
 * IAMROOT, 2023.02.18:
 * - currpri를 newpri로 갱신한다.
 */
	*currpri = newpri;
}

/**
 * cpupri_init - initialize the cpupri structure
 * @cp: The cpupri context
 *
 * Return: -ENOMEM on memory allocation failure.
 */
/*
 * IAMROOT, 2022.11.26:
 * - cpu priority 초기화.
 */
int cpupri_init(struct cpupri *cp)
{
	int i;

	for (i = 0; i < CPUPRI_NR_PRIORITIES; i++) {
		struct cpupri_vec *vec = &cp->pri_to_cpu[i];

		atomic_set(&vec->count, 0);
		if (!zalloc_cpumask_var(&vec->mask, GFP_KERNEL))
			goto cleanup;
	}

	cp->cpu_to_pri = kcalloc(nr_cpu_ids, sizeof(int), GFP_KERNEL);
	if (!cp->cpu_to_pri)
		goto cleanup;

	for_each_possible_cpu(i)
		cp->cpu_to_pri[i] = CPUPRI_INVALID;

	return 0;

cleanup:
	for (i--; i >= 0; i--)
		free_cpumask_var(cp->pri_to_cpu[i].mask);
	return -ENOMEM;
}

/**
 * cpupri_cleanup - clean up the cpupri structure
 * @cp: The cpupri context
 */
void cpupri_cleanup(struct cpupri *cp)
{
	int i;

	kfree(cp->cpu_to_pri);
	for (i = 0; i < CPUPRI_NR_PRIORITIES; i++)
		free_cpumask_var(cp->pri_to_cpu[i].mask);
}
