// SPDX-License-Identifier: GPL-2.0-only
/*
 *  kernel/sched/cpudl.c
 *
 *  Global CPU deadline management
 *
 *  Author: Juri Lelli <j.lelli@sssup.it>
 */
#include "sched.h"

static inline int parent(int i)
{
	return (i - 1) >> 1;
}

static inline int left_child(int i)
{
	return (i << 1) + 1;
}

static inline int right_child(int i)
{
	return (i << 1) + 2;
}

static void cpudl_heapify_down(struct cpudl *cp, int idx)
{
	int l, r, largest;

	int orig_cpu = cp->elements[idx].cpu;
	u64 orig_dl = cp->elements[idx].dl;

	if (left_child(idx) >= cp->size)
		return;

	/* adapted from lib/prio_heap.c */
	while (1) {
		u64 largest_dl;

		l = left_child(idx);
		r = right_child(idx);
		largest = idx;
		largest_dl = orig_dl;

		if ((l < cp->size) && dl_time_before(orig_dl,
						cp->elements[l].dl)) {
			largest = l;
			largest_dl = cp->elements[l].dl;
		}
		if ((r < cp->size) && dl_time_before(largest_dl,
						cp->elements[r].dl))
			largest = r;

		if (largest == idx)
			break;

		/* pull largest child onto idx */
		cp->elements[idx].cpu = cp->elements[largest].cpu;
		cp->elements[idx].dl = cp->elements[largest].dl;
		cp->elements[cp->elements[idx].cpu].idx = idx;
		idx = largest;
	}
	/* actual push down of saved original values orig_* */
	cp->elements[idx].cpu = orig_cpu;
	cp->elements[idx].dl = orig_dl;
	cp->elements[cp->elements[idx].cpu].idx = idx;
}

static void cpudl_heapify_up(struct cpudl *cp, int idx)
{
	int p;

	int orig_cpu = cp->elements[idx].cpu;
	u64 orig_dl = cp->elements[idx].dl;

	if (idx == 0)
		return;

	do {
		p = parent(idx);
		if (dl_time_before(orig_dl, cp->elements[p].dl))
			break;
		/* pull parent onto idx */
		cp->elements[idx].cpu = cp->elements[p].cpu;
		cp->elements[idx].dl = cp->elements[p].dl;
		cp->elements[cp->elements[idx].cpu].idx = idx;
		idx = p;
	} while (idx != 0);
	/* actual push up of saved original values orig_* */
	cp->elements[idx].cpu = orig_cpu;
	cp->elements[idx].dl = orig_dl;
	cp->elements[cp->elements[idx].cpu].idx = idx;
}

static void cpudl_heapify(struct cpudl *cp, int idx)
{
	if (idx > 0 && dl_time_before(cp->elements[parent(idx)].dl,
				cp->elements[idx].dl))
		cpudl_heapify_up(cp, idx);
	else
		cpudl_heapify_down(cp, idx);
}

/*
 * IAMROOT, 2023.03.04:
 * - heapify 자료구조의 0번째. 즉 가장 deadline이 높은 cpu.
 *   (마감시간이 가장 많이남은)
 */
static inline int cpudl_maximum(struct cpudl *cp)
{
	return cp->elements[0].cpu;
}

/*
 * cpudl_find - find the best (later-dl) CPU in the system
 * @cp: the cpudl max-heap context
 * @p: the task
 * @later_mask: a mask to fill in with the selected CPUs (or NULL)
 *
 * Returns: int - CPUs were found
 */
/*
 * IAMROOT, 2023.03.04:
 * @return 1. 동작할수있는 cpu를 찾았다. HMP면 cpu cap까지 고려한다.
 *         0: 못찾았다.
 */
int cpudl_find(struct cpudl *cp, struct task_struct *p,
	       struct cpumask *later_mask)
{
	const struct sched_dl_entity *dl_se = &p->dl;

/*
 * IAMROOT, 2023.03.04:
 * - free_cpu에서 @p->cpus_mask와 겹치는게 있으면 later_mask에 기록한다.
 */
	if (later_mask &&
	    cpumask_and(later_mask, cp->free_cpus, &p->cpus_mask)) {
		unsigned long cap, max_cap = 0;
		int cpu, max_cpu = -1;

/*
 * IAMROOT, 2023.03.04:
 * - HMP 모드가 아니면 그냥 cpu범위를 찾은것으로 성공 return.
 */
		if (!static_branch_unlikely(&sched_asym_cpucapacity))
			return 1;

		/* Ensure the capacity of the CPUs fits the task. */
/*
 * IAMROOT, 2023.03.04:
 * - HMP를 고려해서 한번더 찾는다.
 *   각 cpu성능을 대비해 @p의 runtime이 deadline이내에 처리가 될수있는
 *   적합한 cpu인지 검사한다.
 */
		for_each_cpu(cpu, later_mask) {
			if (!dl_task_fits_capacity(p, cpu)) {
/*
 * IAMROOT, 2023.03.04:
 * - cpu가 dl처리에 부적합한경우 clear시킨다.
 */
				cpumask_clear_cpu(cpu, later_mask);

				cap = capacity_orig_of(cpu);

/*
 * IAMROOT, 2023.03.04:
 * - 모든 cpu가 적합하지 않은것을 대비해 max만을 기록해놓는 작업을 한다.
 */
				if (cap > max_cap ||
				    (cpu == task_cpu(p) && cap == max_cap)) {
					max_cap = cap;
					max_cpu = cpu;
				}
			}
		}

/*
 * IAMROOT, 2023.03.04:
 * - 못찾았으면 기록한 max cpu로 기록한다.
 */
		if (cpumask_empty(later_mask))
			cpumask_set_cpu(max_cpu, later_mask);

		return 1;
	} else {
		int best_cpu = cpudl_maximum(cp);

		WARN_ON(best_cpu != -1 && !cpu_present(best_cpu));

/*
 * IAMROOT, 2023.03.04:
 * - bestcpu에서 @p를 동작시킬수있고, 만료시각이 cp보다 전의 시간이면 
 *   동작시킬수있다.
 */
		if (cpumask_test_cpu(best_cpu, &p->cpus_mask) &&
		    dl_time_before(dl_se->deadline, cp->elements[0].dl)) {
			if (later_mask)
				cpumask_set_cpu(best_cpu, later_mask);

			return 1;
		}
	}
	return 0;
}

/*
 * cpudl_clear - remove a CPU from the cpudl max-heap
 * @cp: the cpudl max-heap context
 * @cpu: the target CPU
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   cpudl_clear - cpudl max-heap에서 CPU 제거
 *   @cp: cpudl max-heap 컨텍스트
 *   @cpu:대상 CPU
 *
 *   참고: cpu_rq(cpu)->lock이 잠겨 있다고 가정
 *
 *   반환: (void)
 *   - cpudl 자료구조 @cp에서 @cpu 삭제
 */
void cpudl_clear(struct cpudl *cp, int cpu)
{
	int old_idx, new_cpu;
	unsigned long flags;

	WARN_ON(!cpu_present(cpu));

	raw_spin_lock_irqsave(&cp->lock, flags);

	old_idx = cp->elements[cpu].idx;
	if (old_idx == IDX_INVALID) {
		/*
		 * Nothing to remove if old_idx was invalid.
		 * This could happen if a rq_offline_dl is
		 * called for a CPU without -dl tasks running.
		 */
		/*
		 * IAMROOT. 2023.02.25:
		 * - google-translate
		 *   old_idx가 유효하지 않은 경우 제거할 항목이 없습니다.
		 *   이는 -dl 작업이 실행되지 않는 CPU에 대해 rq_offline_dl이
		 *   호출되는 경우에 발생할 수 있습니다.
		 */
	} else {
/*
 * IAMROOT, 2023.03.03:
 * - heap 자료구조의 삭제방식대로 수행한다.
 *   마지막 node를 삭제되는 node에 위치시키고 정렬한다.
 */
		new_cpu = cp->elements[cp->size - 1].cpu;
		cp->elements[old_idx].dl = cp->elements[cp->size - 1].dl;
		cp->elements[old_idx].cpu = new_cpu;
		cp->size--;
		cp->elements[new_cpu].idx = old_idx;
		cp->elements[cpu].idx = IDX_INVALID;
		cpudl_heapify(cp, old_idx);

		/*
		 * IAMROOT, 2023.02.25:
		 * - 삭제한 cpu를 free_cpus mask 에 설정
		 */
		cpumask_set_cpu(cpu, cp->free_cpus);
	}
	raw_spin_unlock_irqrestore(&cp->lock, flags);
}

/*
 * cpudl_set - update the cpudl max-heap
 * @cp: the cpudl max-heap context
 * @cpu: the target CPU
 * @dl: the new earliest deadline for this CPU
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
/*
 * IAMROOT. 2023.02.25:
 * - google-translate
 *   cpudl_set - cpudl max-heap 업데이트
 *   @cp: cpudl max-heap 컨텍스트
 *   @cpu: 대상 CPU
 *   @dl: 이 CPU에 대한 새로운 가장 빠른 데드라인
 *
 *   참고: cpu_rq(cpu)->lock이 잠겨 있다고 가정
 *
 *   반환: (void)
 *   - @cpu에 대한 가장 빠른 데드라인 @dl을 @cp 에 설정
 */
void cpudl_set(struct cpudl *cp, int cpu, u64 dl)
{
	int old_idx;
	unsigned long flags;

	WARN_ON(!cpu_present(cpu));

	raw_spin_lock_irqsave(&cp->lock, flags);

	old_idx = cp->elements[cpu].idx;
	if (old_idx == IDX_INVALID) {
		int new_idx = cp->size++;

		cp->elements[new_idx].dl = dl;
		cp->elements[new_idx].cpu = cpu;
		cp->elements[cpu].idx = new_idx;
		cpudl_heapify_up(cp, new_idx);
		cpumask_clear_cpu(cpu, cp->free_cpus);
	} else {
		cp->elements[old_idx].dl = dl;
		cpudl_heapify(cp, old_idx);
	}

	raw_spin_unlock_irqrestore(&cp->lock, flags);
}

/*
 * cpudl_set_freecpu - Set the cpudl.free_cpus
 * @cp: the cpudl max-heap context
 * @cpu: rd attached CPU
 */
void cpudl_set_freecpu(struct cpudl *cp, int cpu)
{
	cpumask_set_cpu(cpu, cp->free_cpus);
}

/*
 * cpudl_clear_freecpu - Clear the cpudl.free_cpus
 * @cp: the cpudl max-heap context
 * @cpu: rd attached CPU
 */
void cpudl_clear_freecpu(struct cpudl *cp, int cpu)
{
	cpumask_clear_cpu(cpu, cp->free_cpus);
}

/*
 * cpudl_init - initialize the cpudl structure
 * @cp: the cpudl max-heap context
 */
/*
 * IAMROOT, 2022.11.26:
 * - cpudl 초기화.
 */
int cpudl_init(struct cpudl *cp)
{
	int i;

	raw_spin_lock_init(&cp->lock);
	cp->size = 0;

	cp->elements = kcalloc(nr_cpu_ids,
			       sizeof(struct cpudl_item),
			       GFP_KERNEL);
	if (!cp->elements)
		return -ENOMEM;

	if (!zalloc_cpumask_var(&cp->free_cpus, GFP_KERNEL)) {
		kfree(cp->elements);
		return -ENOMEM;
	}

	for_each_possible_cpu(i)
		cp->elements[i].idx = IDX_INVALID;

	return 0;
}

/*
 * cpudl_cleanup - clean up the cpudl structure
 * @cp: the cpudl max-heap context
 */
void cpudl_cleanup(struct cpudl *cp)
{
	free_cpumask_var(cp->free_cpus);
	kfree(cp->elements);
}
