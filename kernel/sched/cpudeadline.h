/* SPDX-License-Identifier: GPL-2.0 */

#define IDX_INVALID		-1

/*
 * IAMROOT, 2023.02.27:
 * - @dl: dl_rq의 earliest_dl.curr(leftmost deadline) 저장
 * - @cpu : dl_rq의 cpu
 * - @idx : cpudl elements 배열의 해당 cpu 설정에 대한 index
 *          ex. idx가 2이면
 *          elements[2].cpu - 2번 cpu
 *          elements[2].dl 은 - 2번 cpu의 leftmost deadline
 * - heap의 node.
 */
struct cpudl_item {
	u64			dl;
	int			cpu;
	int			idx;
};

/*
 * IAMROOT, 2022.12.29:
 * - cpudl_init 에서 초기화
 */
struct cpudl {
	raw_spinlock_t		lock;
	int			size;
	cpumask_var_t		free_cpus;
/*
 * IAMROOT, 2023.03.03:
 * - heap으로 관리.
 */
	struct cpudl_item	*elements;
};

#ifdef CONFIG_SMP
int  cpudl_find(struct cpudl *cp, struct task_struct *p, struct cpumask *later_mask);
void cpudl_set(struct cpudl *cp, int cpu, u64 dl);
void cpudl_clear(struct cpudl *cp, int cpu);
int  cpudl_init(struct cpudl *cp);
void cpudl_set_freecpu(struct cpudl *cp, int cpu);
void cpudl_clear_freecpu(struct cpudl *cp, int cpu);
void cpudl_cleanup(struct cpudl *cp);
#endif /* CONFIG_SMP */
