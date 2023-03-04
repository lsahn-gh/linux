/* SPDX-License-Identifier: GPL-2.0 */

#define CPUPRI_NR_PRIORITIES	(MAX_RT_PRIO+1)

/*
 * IAMROOT, 2023.02.11:
 * - cpupri는 번호가 높을수록 우선순위가 높다.
 */
#define CPUPRI_INVALID		-1

/*
 * IAMROOT, 2023.03.04:
 * - CPUPRI 0      : CFS, RT0(RT0 p->prio == 99)
 *   CPURPI 1 ~ 99 : RT1 ~ RT99(RT99 p->prio = 0)
 *   CPUPRI 100    : deadline(deadline p->prio = -1)
 */
#define CPUPRI_NORMAL		 0
/* values 1-99 are for RT1-RT99 priorities */
#define CPUPRI_HIGHER		100

/*
 * IAMROOT, 2022.12.29:
 * - cpupri_init()에서 초기화.
 */
struct cpupri_vec {
	atomic_t		count;
	cpumask_var_t		mask;
};

/*
 * IAMROOT, 2022.12.29:
 * - cpupri_init()에서 초기화.
 */
struct cpupri {
	struct cpupri_vec	pri_to_cpu[CPUPRI_NR_PRIORITIES];
	int			*cpu_to_pri;
};

#ifdef CONFIG_SMP
int  cpupri_find(struct cpupri *cp, struct task_struct *p,
		 struct cpumask *lowest_mask);
int  cpupri_find_fitness(struct cpupri *cp, struct task_struct *p,
			 struct cpumask *lowest_mask,
			 bool (*fitness_fn)(struct task_struct *p, int cpu));
void cpupri_set(struct cpupri *cp, int cpu, int pri);
int  cpupri_init(struct cpupri *cp);
void cpupri_cleanup(struct cpupri *cp);
#endif
