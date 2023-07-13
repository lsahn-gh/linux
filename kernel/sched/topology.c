// SPDX-License-Identifier: GPL-2.0
/*
 * Scheduler topology setup/handling methods
 */
#include "sched.h"

DEFINE_MUTEX(sched_domains_mutex);

/* Protected by sched_domains_mutex: */
static cpumask_var_t sched_domains_tmpmask;
static cpumask_var_t sched_domains_tmpmask2;

#ifdef CONFIG_SCHED_DEBUG

static int __init sched_debug_setup(char *str)
{
	sched_debug_verbose = true;

	return 0;
}
early_param("sched_verbose", sched_debug_setup);

static inline bool sched_debug(void)
{
	return sched_debug_verbose;
}

#define SD_FLAG(_name, mflags) [__##_name] = { .meta_flags = mflags, .name = #_name },
const struct sd_flag_debug sd_flag_debug[] = {
#include <linux/sched/sd_flags.h>
};
#undef SD_FLAG

static int sched_domain_debug_one(struct sched_domain *sd, int cpu, int level,
				  struct cpumask *groupmask)
{
	struct sched_group *group = sd->groups;
	unsigned long flags = sd->flags;
	unsigned int idx;

	cpumask_clear(groupmask);

	printk(KERN_DEBUG "%*s domain-%d: ", level, "", level);
	printk(KERN_CONT "span=%*pbl level=%s\n",
	       cpumask_pr_args(sched_domain_span(sd)), sd->name);

	if (!cpumask_test_cpu(cpu, sched_domain_span(sd))) {
		printk(KERN_ERR "ERROR: domain->span does not contain CPU%d\n", cpu);
	}
	if (group && !cpumask_test_cpu(cpu, sched_group_span(group))) {
		printk(KERN_ERR "ERROR: domain->groups does not contain CPU%d\n", cpu);
	}

	for_each_set_bit(idx, &flags, __SD_FLAG_CNT) {
		unsigned int flag = BIT(idx);
		unsigned int meta_flags = sd_flag_debug[idx].meta_flags;

		if ((meta_flags & SDF_SHARED_CHILD) && sd->child &&
		    !(sd->child->flags & flag))
			printk(KERN_ERR "ERROR: flag %s set here but not in child\n",
			       sd_flag_debug[idx].name);

		if ((meta_flags & SDF_SHARED_PARENT) && sd->parent &&
		    !(sd->parent->flags & flag))
			printk(KERN_ERR "ERROR: flag %s set here but not in parent\n",
			       sd_flag_debug[idx].name);
	}

	printk(KERN_DEBUG "%*s groups:", level + 1, "");
	do {
		if (!group) {
			printk("\n");
			printk(KERN_ERR "ERROR: group is NULL\n");
			break;
		}

		if (!cpumask_weight(sched_group_span(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: empty group\n");
			break;
		}

		if (!(sd->flags & SD_OVERLAP) &&
		    cpumask_intersects(groupmask, sched_group_span(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: repeated CPUs\n");
			break;
		}

		cpumask_or(groupmask, groupmask, sched_group_span(group));

		printk(KERN_CONT " %d:{ span=%*pbl",
				group->sgc->id,
				cpumask_pr_args(sched_group_span(group)));

		if ((sd->flags & SD_OVERLAP) &&
		    !cpumask_equal(group_balance_mask(group), sched_group_span(group))) {
			printk(KERN_CONT " mask=%*pbl",
				cpumask_pr_args(group_balance_mask(group)));
		}

		if (group->sgc->capacity != SCHED_CAPACITY_SCALE)
			printk(KERN_CONT " cap=%lu", group->sgc->capacity);

		if (group == sd->groups && sd->child &&
		    !cpumask_equal(sched_domain_span(sd->child),
				   sched_group_span(group))) {
			printk(KERN_ERR "ERROR: domain->groups does not match domain->child\n");
		}

		printk(KERN_CONT " }");

		group = group->next;

		if (group != sd->groups)
			printk(KERN_CONT ",");

	} while (group != sd->groups);
	printk(KERN_CONT "\n");

	if (!cpumask_equal(sched_domain_span(sd), groupmask))
		printk(KERN_ERR "ERROR: groups don't span domain->span\n");

	if (sd->parent &&
	    !cpumask_subset(groupmask, sched_domain_span(sd->parent)))
		printk(KERN_ERR "ERROR: parent span is not a superset of domain->span\n");
	return 0;
}

static void sched_domain_debug(struct sched_domain *sd, int cpu)
{
	int level = 0;

	if (!sched_debug_verbose)
		return;

	if (!sd) {
		printk(KERN_DEBUG "CPU%d attaching NULL sched-domain.\n", cpu);
		return;
	}

	printk(KERN_DEBUG "CPU%d attaching sched-domain(s):\n", cpu);

	for (;;) {
		if (sched_domain_debug_one(sd, cpu, level, sched_domains_tmpmask))
			break;
		level++;
		sd = sd->parent;
		if (!sd)
			break;
	}
}
#else /* !CONFIG_SCHED_DEBUG */

# define sched_debug_verbose 0
# define sched_domain_debug(sd, cpu) do { } while (0)
static inline bool sched_debug(void)
{
	return false;
}
#endif /* CONFIG_SCHED_DEBUG */

/*
 * IAMROOT, 2023.04.29:
 * - static const unsigned int SD_DEGENERATE_GROUPS_MASK = SD_BALANCE_NEWIDLE |
 *   SD_ASYM_CPUCAPACITY | ...
 *   SD_WAKE_AFFINE flag 외에 모든 값이 현재 SDF_NEEDS_GROUPS 에 설정되어 있다
 */
/* Generate a mask of SD flags with the SDF_NEEDS_GROUPS metaflag */
#define SD_FLAG(name, mflags) (name * !!((mflags) & SDF_NEEDS_GROUPS)) |
static const unsigned int SD_DEGENERATE_GROUPS_MASK =
#include <linux/sched/sd_flags.h>
0;
#undef SD_FLAG

/*
 * IAMROOT, 2023.04.22:
 * @return 1 : 삭제해도된다는 의미이다.
 * - @sd의 삭제 여부를 결정한다.
 * - 삭제 경우
 *   1. parent(@sd) weight가 1인 경우. 즉 child가 대신하면 되므로 삭제 해도된다.
 *   2. 기타 유지 경우를 제외한 예외
 *
 * - 유지 경우
 *   1. SD_DEGENERATE_GROUPS_MASK이 있으면서 groups이 2개이상인 경우 삭제 안한다.
 *   2. SD_WAKE_AFFINE가 있는 경우.
 */
static int sd_degenerate(struct sched_domain *sd)
{
	if (cpumask_weight(sched_domain_span(sd)) == 1)
		return 1;

	/* Following flags need at least 2 groups */
/*
 * IAMROOT, 2023.04.22:
 * - SD_DEGENERATE_GROUPS_MASK이 있으면서 groups이 2개이상인 경우 삭제 안한다.
 */
	if ((sd->flags & SD_DEGENERATE_GROUPS_MASK) &&
	    (sd->groups != sd->groups->next))
		return 0;

	/* Following flags don't use groups */
	if (sd->flags & (SD_WAKE_AFFINE))
		return 0;

	return 1;
}

/*
 * IAMROOT, 2023.04.22:
 * - return 1 삭제를 해도된다.
 * - @parent의 삭제 여부를 결정한다.
 * - 삭제 경우
 *   1. @parent 가 sd_degenerate 에 해당하는 경우
 *   2. 기타 유지 경우를 제외한 예외
 *
 * - 유지 경우
 *   1. @sd 와 @parent 의 span cpumask 가 같지 않을 경우
 *   2. @parent 그룹이 하나 일 경우
 *   2.1. @sd에는 AFFINE 설정이 없는데 부모에만 있는 경우
 *   3. @parent 그룹이 둘 이상일 경우
 *   3.1. @sd에 없는 flag 가 @parent 에 있는 경우
 */
static int
sd_parent_degenerate(struct sched_domain *sd, struct sched_domain *parent)
{
 	unsigned long cflags = sd->flags, pflags = parent->flags;

	if (sd_degenerate(parent))
		return 1;

	if (!cpumask_equal(sched_domain_span(sd), sched_domain_span(parent)))
		return 0;

	/* Flags needing groups don't count if only 1 group in parent */
	if (parent->groups == parent->groups->next)
		pflags &= ~SD_DEGENERATE_GROUPS_MASK;

	if (~cflags & pflags)
		return 0;

	return 1;
}

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
/*
 * IAMROOT, 2023.05.30:
 * - EAS also maintains a static key (sched_energy_present) which is enabled
 *   when at least one root domain meets all conditions for EAS to start.
 *   (Documentation/scheduler/sched-energy.rst)
 */
DEFINE_STATIC_KEY_FALSE(sched_energy_present);
unsigned int sysctl_sched_energy_aware = 1;
DEFINE_MUTEX(sched_energy_mutex);
bool sched_energy_update;

/*
 * IAMROOT, 2022.12.10:
 * - TODO
 */
void rebuild_sched_domains_energy(void)
{
	mutex_lock(&sched_energy_mutex);
	sched_energy_update = true;
	rebuild_sched_domains();
	sched_energy_update = false;
	mutex_unlock(&sched_energy_mutex);
}

#ifdef CONFIG_PROC_SYSCTL
int sched_energy_aware_handler(struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret, state;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (!ret && write) {
		state = static_branch_unlikely(&sched_energy_present);
		if (state != sysctl_sched_energy_aware)
			rebuild_sched_domains_energy();
	}

	return ret;
}
#endif

static void free_pd(struct perf_domain *pd)
{
	struct perf_domain *tmp;

	while (pd) {
		tmp = pd->next;
		kfree(pd);
		pd = tmp;
	}
}

/*
 * IAMROOT, 2023.05.27:
 * - pd 를 순회하며 pd cpumask 에 @cpu 가 있다면 pd를 반환하고 아니면 NULL 반환
 */
static struct perf_domain *find_pd(struct perf_domain *pd, int cpu)
{
	while (pd) {
		if (cpumask_test_cpu(cpu, perf_domain_span(pd)))
			return pd;
		pd = pd->next;
	}

	return NULL;
}

/*
 * IAMROOT, 2023.05.27:
 * - pd를 할당하고 cpu에 대한 em_perf_domain을 연결한다.
 */
static struct perf_domain *pd_init(int cpu)
{
	struct em_perf_domain *obj = em_cpu_get(cpu);
	struct perf_domain *pd;

	if (!obj) {
		if (sched_debug())
			pr_info("%s: no EM found for CPU%d\n", __func__, cpu);
		return NULL;
	}

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return NULL;
	pd->em_pd = obj;

	return pd;
}

static void perf_domain_debug(const struct cpumask *cpu_map,
						struct perf_domain *pd)
{
	if (!sched_debug() || !pd)
		return;

	printk(KERN_DEBUG "root_domain %*pbl:", cpumask_pr_args(cpu_map));

	while (pd) {
		printk(KERN_CONT " pd%d:{ cpus=%*pbl nr_pstate=%d }",
				cpumask_first(perf_domain_span(pd)),
				cpumask_pr_args(perf_domain_span(pd)),
				em_pd_nr_perf_states(pd->em_pd));
		pd = pd->next;
	}

	printk(KERN_CONT "\n");
}

static void destroy_perf_domain_rcu(struct rcu_head *rp)
{
	struct perf_domain *pd;

	pd = container_of(rp, struct perf_domain, rcu);
	free_pd(pd);
}

/*
 * IAMROOT, 2023.05.20:
 * - has_eas 여부에 따라 static key 설정을 변경한다.
 */
static void sched_energy_set(bool has_eas)
{
	if (!has_eas && static_branch_unlikely(&sched_energy_present)) {
		if (sched_debug())
			pr_info("%s: stopping EAS\n", __func__);
		static_branch_disable_cpuslocked(&sched_energy_present);
	} else if (has_eas && !static_branch_unlikely(&sched_energy_present)) {
		if (sched_debug())
			pr_info("%s: starting EAS\n", __func__);
		static_branch_enable_cpuslocked(&sched_energy_present);
	}
}

/*
 * EAS can be used on a root domain if it meets all the following conditions:
 *    1. an Energy Model (EM) is available;
 *    2. the SD_ASYM_CPUCAPACITY flag is set in the sched_domain hierarchy.
 *    3. no SMT is detected.
 *    4. the EM complexity is low enough to keep scheduling overheads low;
 *    5. schedutil is driving the frequency of all CPUs of the rd;
 *    6. frequency invariance support is present;
 *
 * The complexity of the Energy Model is defined as:
 *
 *              C = nr_pd * (nr_cpus + nr_ps)
 *
 * with parameters defined as:
 *  - nr_pd:    the number of performance domains
 *  - nr_cpus:  the number of CPUs
 *  - nr_ps:    the sum of the number of performance states of all performance
 *              domains (for example, on a system with 2 performance domains,
 *              with 10 performance states each, nr_ps = 2 * 10 = 20).
 *
 * It is generally not a good idea to use such a model in the wake-up path on
 * very complex platforms because of the associated scheduling overheads. The
 * arbitrary constraint below prevents that. It makes EAS usable up to 16 CPUs
 * with per-CPU DVFS and less than 8 performance states each, for example.
 */
/*
 * IAMROOT. 2023.05.27:
 * - google-translate
 * 다음 조건을 모두 충족하는 경우 루트 도메인에서 EAS를 사용할 수
 * 있습니다.
 * 1. EM(에너지 모델)을 사용할 수 있습니다.
 * 2. SD_ASYM_CPUCAPACITY 플래그가 sched_domain 계층 구조에 설정됩니다.
 * 3. SMT가 감지되지 않습니다.
 * 4. EM 복잡성은 일정 오버헤드를 낮게 유지하기에 충분히 낮습니다.
 * 5. schedutil은 rd의 모든 CPU 주파수를 구동합니다.
 * 6. 주파수 불변성 지원이 있습니다.
 *
 * 에너지 모델의 복잡성은 다음과 같이 정의됩니다.
 *
 * C = nr_pd * (nr_cpus + nr_ps)
 *
 * 매개변수는 다음과 같이 정의됩니다.
 * - nr_pd: 성능 도메인 수
 * - nr_cpus: CPU 수
 * - nr_ps: 성능 상태 수의 합 모든 성능 도메인(예: 각각 10개의 성능 상태가 있는 2개의
 * 성능 도메인이 있는 시스템에서 nr_ps = 2 * 10 = 20).
 *
 * 관련된 스케줄링 오버헤드 때문에 매우 복잡한 플랫폼의 웨이크업 경로에서 이러한 모델을
 * 사용하는 것은 일반적으로 좋은 생각이 아닙니다. 아래의 임의 제약 조건은 이를 방지합니다.
 * 예를 들어 CPU당 DVFS가 있는 최대 16개의 CPU와 각각 8개 미만의 성능 상태까지 EAS를
 * 사용할 수 있습니다.
 *
 * IAMROOT, 2023.06.01:
 * - 조건에 맞다면 @cpu_map 의 cpu에 대해 pd를 만들고 리스트로 서로 연결한다.
 *   @cpu_map의 첫번째 cpu의 rd에 위에서 연결한 리스트의 처음 pd를 설정한다.
 * - Return: 조건에 충족하여 pd 가 하나라도 생성되었다면 true 반환
 */
#define EM_MAX_COMPLEXITY 2048

extern struct cpufreq_governor schedutil_gov;
/*
 * IAMROOT, 2023.05.30:
 * - 1. EM_MAX_COMPLEXITY주석내용(1~6)대로의 조건검사를 수행한다.
 *   2. pd가 등록되있지 않은 cpu를 찾아 pd를 등록한다.
 *   3. complexity of the Energy Model 식에 따라 복잡성이 EM_MAX_COMPLEXITY을 넘는지
 *   검사한다.
 *   4. pd가 성공적으로 추가 됬으면 return true.
 */
static bool build_perf_domains(const struct cpumask *cpu_map)
{
	int i, nr_pd = 0, nr_ps = 0, nr_cpus = cpumask_weight(cpu_map);
	struct perf_domain *pd = NULL, *tmp;
	int cpu = cpumask_first(cpu_map);
	struct root_domain *rd = cpu_rq(cpu)->rd;
	struct cpufreq_policy *policy;
	struct cpufreq_governor *gov;

	if (!sysctl_sched_energy_aware)
		goto free;

	/* EAS is enabled for asymmetric CPU capacity topologies. */
	/*
	 * IAMROOT. 2023.05.30:
	 * - google-translate
	 * EAS는 비대칭 CPU 용량 토폴로지에 대해 활성화됩니다.
	 */
	if (!per_cpu(sd_asym_cpucapacity, cpu)) {
		if (sched_debug()) {
			pr_info("rd %*pbl: CPUs do not have asymmetric capacities\n",
					cpumask_pr_args(cpu_map));
		}
		goto free;
	}

	/* EAS definitely does *not* handle SMT */
	/*
	 * IAMROOT. 2023.05.30:
	 * - google-translate
	 * EAS는 확실히 SMT를 처리하지 *않습니다*
	 */
	if (sched_smt_active()) {
		pr_warn("rd %*pbl: Disabling EAS, SMT is not supported\n",
			cpumask_pr_args(cpu_map));
		goto free;
	}

	if (!arch_scale_freq_invariant()) {
		if (sched_debug()) {
			pr_warn("rd %*pbl: Disabling EAS: frequency-invariant load tracking not yet supported",
				cpumask_pr_args(cpu_map));
		}
		goto free;
	}

/*
 * IAMROOT, 2023.05.30:
 * - pd가 등록되있지 않은 cpu를 찾는다. cpu의 cpufreq_policy로부터 governor를
 *   얻어오고, 해당 governor가 schedutil_gov인 것만 pd 생성을 수행한다.
 */
	for_each_cpu(i, cpu_map) {
		/* Skip already covered CPUs. */
/*
 * IAMROOT, 2023.05.30:
 * - 이미 있으면 pass
 */
		if (find_pd(pd, i))
			continue;

		/* Do not attempt EAS if schedutil is not being used. */
/*
 * IAMROOT, 2023.05.30:
 * - i cpu로부터 ref up하면서 policy를 가져와 gov를 가져오고 ref down을 한다.
 */
		policy = cpufreq_cpu_get(i);
		if (!policy)
			goto free;
		gov = policy->governor;
		cpufreq_cpu_put(policy);
		/*
		 * IAMROOT, 2023.05.27:
		 * - 찾은 gov 가 schedutil_gov(default) 가 아니면 빠져나간다
		 */
		if (gov != &schedutil_gov) {
			if (rd->pd)
				pr_warn("rd %*pbl: Disabling EAS, schedutil is mandatory\n",
						cpumask_pr_args(cpu_map));
			goto free;
		}

		/* Create the new pd and add it to the local list. */
		/*
		 * IAMROOT. 2023.05.30:
		 * - google-translate
		 * 새 pd를 만들고 로컬 목록에 추가합니다.
		 * - 새로 생성한 pd의 next에 이전 루프에서 생성한 pd를 연결한다.
		 */
		tmp = pd_init(i);
		if (!tmp)
			goto free;
		tmp->next = pd;
		pd = tmp;

		/*
		 * Count performance domains and performance states for the
		 * complexity check.
		 */
		/*
		 * IAMROOT. 2023.05.27:
		 * - google-translate
		 * 복잡성 검사를 위해 성능 도메인 및 성능 상태를 계산합니다.
		 * - cpumap 이 사용하는 cluster 만큼 pd가 만들어진다
		 */
		nr_pd++;
		nr_ps += em_pd_nr_perf_states(pd->em_pd);
	}

	/* Bail out if the Energy Model complexity is too high. */
/*
 * IAMROOT, 2023.06.01:
 * - 에너지 복잡성식은 다음과 같다.(EM_MAX_COMPLEXITY 주석 참고)
 *   C = nr_pd * (nr_cpus + nr_ps)
 *   해당식이 EM_MAX_COMPLEXITY값을 넘는 경우엔 에러 처리한다.
 */
	if (nr_pd * (nr_ps + nr_cpus) > EM_MAX_COMPLEXITY) {
		WARN(1, "rd %*pbl: Failed to start EAS, EM complexity is too high\n",
						cpumask_pr_args(cpu_map));
		goto free;
	}

	perf_domain_debug(cpu_map, pd);

	/* Attach the new list of performance domains to the root domain. */
	/*
	 * IAMROOT. 2023.05.30:
	 * - google-translate
	 * 새 성능 도메인 목록을 루트 도메인에 연결합니다.
	 * - 리스트의 처음 pd를 rd 에 연결한다
	 */
	tmp = rd->pd;
	rcu_assign_pointer(rd->pd, pd);
	if (tmp)
		call_rcu(&tmp->rcu, destroy_perf_domain_rcu);

	return !!pd;

free:
	free_pd(pd);
	tmp = rd->pd;
	rcu_assign_pointer(rd->pd, NULL);
	if (tmp)
		call_rcu(&tmp->rcu, destroy_perf_domain_rcu);

	return false;
}
#else
static void free_pd(struct perf_domain *pd) { }
#endif /* CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL*/

static void free_rootdomain(struct rcu_head *rcu)
{
	struct root_domain *rd = container_of(rcu, struct root_domain, rcu);

	cpupri_cleanup(&rd->cpupri);
	cpudl_cleanup(&rd->cpudl);
	free_cpumask_var(rd->dlo_mask);
	free_cpumask_var(rd->rto_mask);
	free_cpumask_var(rd->online);
	free_cpumask_var(rd->span);
	free_pd(rd->pd);
	kfree(rd);
}

/*
 * IAMROOT, 2022.11.26:
 * - @rq에 @rd를 등록한다.
 *   @rq에 rd가 있었다면 old에 대한 기록을 지우고 @rd를 등록한다.
 *   @rq->rd = @rd
 */
void rq_attach_root(struct rq *rq, struct root_domain *rd)
{
	struct root_domain *old_rd = NULL;
	unsigned long flags;

	raw_spin_rq_lock_irqsave(rq, flags);

	if (rq->rd) {
		old_rd = rq->rd;

		if (cpumask_test_cpu(rq->cpu, old_rd->online))
			set_rq_offline(rq);

		cpumask_clear_cpu(rq->cpu, old_rd->span);

		/*
		 * If we dont want to free the old_rd yet then
		 * set old_rd to NULL to skip the freeing later
		 * in this function:
		 */
		if (!atomic_dec_and_test(&old_rd->refcount))
			old_rd = NULL;
	}

	atomic_inc(&rd->refcount);
	rq->rd = rd;

	cpumask_set_cpu(rq->cpu, rd->span);
	if (cpumask_test_cpu(rq->cpu, cpu_active_mask))
		set_rq_online(rq);

	raw_spin_rq_unlock_irqrestore(rq, flags);

	if (old_rd)
		call_rcu(&old_rd->rcu, free_rootdomain);
}

/*
 * IAMROOT, 2023.02.11:
 * - ref up
 */
void sched_get_rd(struct root_domain *rd)
{
	atomic_inc(&rd->refcount);
}

void sched_put_rd(struct root_domain *rd)
{
	if (!atomic_dec_and_test(&rd->refcount))
		return;

	call_rcu(&rd->rcu, free_rootdomain);
}

/*
 * IAMROOT, 2022.11.26:
 * - rootdomain은 load_balance에 사용한다.
 *   dl, cpudl, cpupri를 초기화한다.
 */
static int init_rootdomain(struct root_domain *rd)
{
	if (!zalloc_cpumask_var(&rd->span, GFP_KERNEL))
		goto out;
	if (!zalloc_cpumask_var(&rd->online, GFP_KERNEL))
		goto free_span;
	if (!zalloc_cpumask_var(&rd->dlo_mask, GFP_KERNEL))
		goto free_online;
	if (!zalloc_cpumask_var(&rd->rto_mask, GFP_KERNEL))
		goto free_dlo_mask;

#ifdef HAVE_RT_PUSH_IPI
	rd->rto_cpu = -1;
	raw_spin_lock_init(&rd->rto_lock);
	init_irq_work(&rd->rto_push_work, rto_push_irq_work_func);
#endif

	rd->visit_gen = 0;
	init_dl_bw(&rd->dl_bw);
	if (cpudl_init(&rd->cpudl) != 0)
		goto free_rto_mask;

	if (cpupri_init(&rd->cpupri) != 0)
		goto free_cpudl;
	return 0;

free_cpudl:
	cpudl_cleanup(&rd->cpudl);
free_rto_mask:
	free_cpumask_var(rd->rto_mask);
free_dlo_mask:
	free_cpumask_var(rd->dlo_mask);
free_online:
	free_cpumask_var(rd->online);
free_span:
	free_cpumask_var(rd->span);
out:
	return -ENOMEM;
}

/*
 * By default the system creates a single root-domain with all CPUs as
 * members (mimicking the global state we have today).
 */
struct root_domain def_root_domain;

/*
 * IAMROOT, 2022.11.26:
 * - rootdomain은 load_balance에 사용한다.
 *   dl, cpudl, cpupri를 초기화한다.
 */
void init_defrootdomain(void)
{
	init_rootdomain(&def_root_domain);

	atomic_set(&def_root_domain.refcount, 1);
}

/*
 * IAMROOT, 2023.04.15:
 * - rootdomain에 대한 default값을 하나 할당하고 초기화해서 return한다.
 *   isolate cpu를 제외한 모든 cpu들이 기본적으로 root domain에 참가한다.
 */
static struct root_domain *alloc_rootdomain(void)
{
	struct root_domain *rd;

	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return NULL;

	if (init_rootdomain(rd) != 0) {
		kfree(rd);
		return NULL;
	}

	return rd;
}

/*
 * IAMROOT, 2023.04.29:
 * - @sg 에 연결된 모든 그룹을 free 한다. @free_sgc 가 1이면 sgc도 free 한다
 */
static void free_sched_groups(struct sched_group *sg, int free_sgc)
{
	struct sched_group *tmp, *first;

	if (!sg)
		return;

	first = sg;
	do {
		tmp = sg->next;

		if (free_sgc && atomic_dec_and_test(&sg->sgc->ref))
			kfree(sg->sgc);

		if (atomic_dec_and_test(&sg->ref))
			kfree(sg);
		sg = tmp;
	} while (sg != first);
}

/*
 * IAMROOT, 2023.04.29:
 * - @sd->groups @sd->shared @sd 를 free 한다
 */
static void destroy_sched_domain(struct sched_domain *sd)
{
	/*
	 * A normal sched domain may have multiple group references, an
	 * overlapping domain, having private groups, only one.  Iterate,
	 * dropping group/capacity references, freeing where none remain.
	 */
	/*
	 * IAMROOT. 2023.04.29:
	 * - google-translate
	 * 일반 sched 도메인에는 여러 그룹 참조, 겹치는 도메인, 개인 그룹이 하나만 있을 수
	 * 있습니다. 반복하고, 그룹/용량 참조를 삭제하고, 남아 있지 않은 곳을 해제합니다.
	 */
	free_sched_groups(sd->groups, 1);

	if (sd->shared && atomic_dec_and_test(&sd->shared->ref))
		kfree(sd->shared);
	kfree(sd);
}

static void destroy_sched_domains_rcu(struct rcu_head *rcu)
{
	struct sched_domain *sd = container_of(rcu, struct sched_domain, rcu);

	while (sd) {
		struct sched_domain *parent = sd->parent;
		destroy_sched_domain(sd);
		sd = parent;
	}
}

static void destroy_sched_domains(struct sched_domain *sd)
{
	if (sd)
		call_rcu(&sd->rcu, destroy_sched_domains_rcu);
}

/*
 * Keep a special pointer to the highest sched_domain that has
 * SD_SHARE_PKG_RESOURCE set (Last Level Cache Domain) for this
 * allows us to avoid some pointer chasing select_idle_sibling().
 *
 * Also keep a unique ID per domain (we use the first CPU number in
 * the cpumask of the domain), this allows us to quickly tell if
 * two CPUs are in the same cache domain, see cpus_share_cache().
 */
/*
 * IAMROOT, 2023.05.12:
 * - papago
 *   SD_SHARE_PKG_RESOURCE가 설정된 가장 높은 sched_domain(마지막 
 *   레벨 캐시 도메인)에 대한 특수 포인터를 유지하면 일부 
 *   포인터가 select_idle_sibling()을 쫓는 것을 피할 수 있습니다.
 *
 *   또한 도메인마다 고유한 ID를 유지합니다(도메인의 cpumask에서 
 *   첫 번째 CPU 번호 사용). 이렇게 하면 두 개의 CPU가 동일한 
 *   캐시 도메인에 있는지 빠르게 알 수 있습니다. 
 *   cpus_share_cache()를 참조하십시오.
 */
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_llc);
DEFINE_PER_CPU(int, sd_llc_size);
DEFINE_PER_CPU(int, sd_llc_id);
DEFINE_PER_CPU(struct sched_domain_shared __rcu *, sd_llc_shared);
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_numa);
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_asym_packing);
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_asym_cpucapacity);

/*
 * IAMROOT, 2023.02.11:
 * - HMP로 동작하는 경우.
 */
DEFINE_STATIC_KEY_FALSE(sched_asym_cpucapacity);

/*
 * IAMROOT, 2023.04.29:
 * - 많이 찾는 domain 설정에 대해 pcpu 캐쉬로 설정한다
 *   llc = last level cache
 */
static void update_top_cache_domain(int cpu)
{
	struct sched_domain_shared *sds = NULL;
	struct sched_domain *sd;
	int id = cpu;
	int size = 1;

	sd = highest_flag_domain(cpu, SD_SHARE_PKG_RESOURCES);
	if (sd) {
		id = cpumask_first(sched_domain_span(sd));
		size = cpumask_weight(sched_domain_span(sd));
		sds = sd->shared;
	}

	rcu_assign_pointer(per_cpu(sd_llc, cpu), sd);
	per_cpu(sd_llc_size, cpu) = size;
	per_cpu(sd_llc_id, cpu) = id;
	rcu_assign_pointer(per_cpu(sd_llc_shared, cpu), sds);

	sd = lowest_flag_domain(cpu, SD_NUMA);
	rcu_assign_pointer(per_cpu(sd_numa, cpu), sd);

	sd = highest_flag_domain(cpu, SD_ASYM_PACKING);
	rcu_assign_pointer(per_cpu(sd_asym_packing, cpu), sd);

	sd = lowest_flag_domain(cpu, SD_ASYM_CPUCAPACITY_FULL);
	rcu_assign_pointer(per_cpu(sd_asym_cpucapacity, cpu), sd);
}

/*
 * Attach the domain 'sd' to 'cpu' as its base domain. Callers must
 * hold the hotplug lock.
 */
/*
 * IAMROOT, 2023.04.22:
 * - 1. 삭제할 수 있는 도메인 삭제
 *   2. rq 의 root domain 설정. cpu_rq(@cpu)->rd = @rd
 *   3. rq 의 sd 설정.(최하위 레벨 sd)
 *   4. sd_llc 등 많이 찾는 domain pcpu 캐쉬 설정
 */
static void
cpu_attach_domain(struct sched_domain *sd, struct root_domain *rd, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct sched_domain *tmp;
	int numa_distance = 0;

	/* Remove the sched domains which do not contribute to scheduling. */
	/*
	 * IAMROOT, 2023.04.29:
	 * - @parent 를 최상위 레벨까지 따라 가며 삭제할 수 있는 조건 이면 삭제
	 */
	for (tmp = sd; tmp; ) {
		struct sched_domain *parent = tmp->parent;
		if (!parent)
			break;

		if (sd_parent_degenerate(tmp, parent)) {
			tmp->parent = parent->parent;
			if (parent->parent)
				parent->parent->child = tmp;
			/*
			 * Transfer SD_PREFER_SIBLING down in case of a
			 * degenerate parent; the spans match for this
			 * so the property transfers.
			 */
			/*
			 * IAMROOT. 2023.04.29:
			 * - google-translate
			 * 타락한 부모의 경우 SD_PREFER_SIBLING을 아래로 전송합니다.
			 * 이에 대한 스팬이 일치하므로 속성이 전송됩니다.
			 */
			if (parent->flags & SD_PREFER_SIBLING)
				tmp->flags |= SD_PREFER_SIBLING;
			destroy_sched_domain(parent);
		} else
			tmp = tmp->parent;
	}

	/*
	 * IAMROOT, 2023.04.29:
	 * - 제일 아래 레벨 @sd 가 삭제 조건 이면 삭제
	 */
	if (sd && sd_degenerate(sd)) {
		tmp = sd;
		sd = sd->parent;
		destroy_sched_domain(tmp);
		if (sd)
			sd->child = NULL;
	}

	/*
	 * IAMROOT, 2023.04.29:
	 * - XXX numa_distance 사용 않는 변수임.
	 * - 현재 버전(v6.3) 에서는 삭제되었다.
	 */
	for (tmp = sd; tmp; tmp = tmp->parent)
		numa_distance += !!(tmp->flags & SD_NUMA);

	sched_domain_debug(sd, cpu);

	rq_attach_root(rq, rd);
	tmp = rq->sd;
	/*
	 * IAMROOT, 2023.04.30:
	 * - 최하위 레벨 sd 가 rq에 연결된다.
	 */
	rcu_assign_pointer(rq->sd, sd);
	dirty_sched_domain_sysctl(cpu);
	/*
	 * IAMROOT, 2023.04.29:
	 * - XXX destroy 하는 것은 백업해둔 기존 rq->sd 이다.
	 */
	destroy_sched_domains(tmp);

	update_top_cache_domain(cpu);
}

struct s_data {
	struct sched_domain * __percpu *sd;
	struct root_domain	*rd;
};

enum s_alloc {
	sa_rootdomain,
	sa_sd,
	sa_sd_storage,
	sa_none,
};

/*
 * Return the canonical balance CPU for this group, this is the first CPU
 * of this group that's also in the balance mask.
 *
 * The balance mask are all those CPUs that could actually end up at this
 * group. See build_balance_mask().
 *
 * Also see should_we_balance().
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   이 그룹에 대한 정식 균형 CPU를 반환합니다. 이는 균형 마스크에도 있는 
 *   이 그룹의 첫 번째 CPU입니다.
 *
 *   밸런스 마스크는 실제로 이 그룹에 도달할 수 있는 모든 CPU입니다. 
 *   build_balance_mask()를 참조하십시오.
 *
 *   should_we_balance()도 참조하십시오.
 */
int group_balance_cpu(struct sched_group *sg)
{
	return cpumask_first(group_balance_mask(sg));
}


/*
 * NUMA topology (first read the regular topology blurb below)
 *
 * Given a node-distance table, for example:
 *
 *   node   0   1   2   3
 *     0:  10  20  30  20
 *     1:  20  10  20  30
 *     2:  30  20  10  20
 *     3:  20  30  20  10
 *
 * which represents a 4 node ring topology like:
 *
 *   0 ----- 1
 *   |       |
 *   |       |
 *   |       |
 *   3 ----- 2
 *
 * We want to construct domains and groups to represent this. The way we go
 * about doing this is to build the domains on 'hops'. For each NUMA level we
 * construct the mask of all nodes reachable in @level hops.
 *
 * For the above NUMA topology that gives 3 levels:
 *
 * NUMA-2	0-3		0-3		0-3		0-3
 *  groups:	{0-1,3},{1-3}	{0-2},{0,2-3}	{1-3},{0-1,3}	{0,2-3},{0-2}
 *
 * NUMA-1	0-1,3		0-2		1-3		0,2-3
 *  groups:	{0},{1},{3}	{0},{1},{2}	{1},{2},{3}	{0},{2},{3}
 *
 * NUMA-0	0		1		2		3
 *
 *
 * As can be seen; things don't nicely line up as with the regular topology.
 * When we iterate a domain in child domain chunks some nodes can be
 * represented multiple times -- hence the "overlap" naming for this part of
 * the topology.
 *
 * In order to minimize this overlap, we only build enough groups to cover the
 * domain. For instance Node-0 NUMA-2 would only get groups: 0-1,3 and 1-3.
 *
 * Because:
 *
 *  - the first group of each domain is its child domain; this
 *    gets us the first 0-1,3
 *  - the only uncovered node is 2, who's child domain is 1-3.
 *
 * However, because of the overlap, computing a unique CPU for each group is
 * more complicated. Consider for instance the groups of NODE-1 NUMA-2, both
 * groups include the CPUs of Node-0, while those CPUs would not in fact ever
 * end up at those groups (they would end up in group: 0-1,3).
 *
 * To correct this we have to introduce the group balance mask. This mask
 * will contain those CPUs in the group that can reach this group given the
 * (child) domain tree.
 *
 * With this we can once again compute balance_cpu and sched_group_capacity
 * relations.
 *
 * XXX include words on how balance_cpu is unique and therefore can be
 * used for sched_group_capacity links.
 *
 *
 * Another 'interesting' topology is:
 *
 *   node   0   1   2   3
 *     0:  10  20  20  30
 *     1:  20  10  20  20
 *     2:  20  20  10  20
 *     3:  30  20  20  10
 *
 * Which looks a little like:
 *
 *   0 ----- 1
 *   |     / |
 *   |   /   |
 *   | /     |
 *   2 ----- 3
 *
 * This topology is asymmetric, nodes 1,2 are fully connected, but nodes 0,3
 * are not.
 *
 * This leads to a few particularly weird cases where the sched_domain's are
 * not of the same number for each CPU. Consider:
 *
 * NUMA-2	0-3						0-3
 *  groups:	{0-2},{1-3}					{1-3},{0-2}
 *
 * NUMA-1	0-2		0-3		0-3		1-3
 *
 * NUMA-0	0		1		2		3
 *
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 * NUMA topology (first read the regular topology blurb below)
 *
 * Given a node-distance table, for example:
 *
 *   node   0   1   2   3
 *     0:  10  20  30  20
 *     1:  20  10  20  30
 *     2:  30  20  10  20
 *     3:  20  30  20  10
 *
 * which represents a 4 node ring topology like:
 *
 *   0 ----- 1
 *   |       |
 *   |       |
 *   |       |
 *   3 ----- 2
 *
 * 이를 대표할 도메인과 그룹을 구성하고자 합니다. 이를 수행하는 방법은 
 * 'hops'에 도메인을 구축하는 것입니다. 각 NUMA 수준에 대해 @level hops에서 
 * 도달할 수 있는 모든 노드의 마스크를 구성합니다.
 *
 * 3개 수준을 제공하는 위의 NUMA 토폴로지의 경우:
 *
 * NUMA-2	0-3		         0-3		     0-3		     0-3
 *  groups:	{0-1,3},{1-3}	{0-2},{0,2-3}	{1-3},{0-1,3}	{0,2-3},{0-2}
 *
 * NUMA-1	0-1,3		    0-2		        1-3		        0,2-3
 *  groups:	{0},{1},{3}	    {0},{1},{2}	    {1},{2},{3}    	{0},{2},{3}
 *
 * NUMA-0	0		        1		         2		        3
 *
 *
 * 알 수있는 바와 같이; 일반 토폴로지와 같이 잘 정렬되지 않습니다.
 * 하위 도메인 청크에서 도메인을 반복할 때 일부 노드는 여러 번 표시될 수 
 * 있습니다. 따라서 토폴로지의 이 부분에 대한 중첩 이름이 지정됩니다.
 *
 * 이 중복을 최소화하기 위해 도메인을 포함하기에 충분한 그룹만 구성합니다. 
 * 예를 들어 Node-0 NUMA-2는 그룹(0-1,3 및 1-3)만 가져옵니다.
 *
 * 왜냐하면:
 *
 * - 각 도메인의 첫 번째 그룹은 하위 도메인입니다. 이것은 우리에게 
 *   첫 번째 0-1,3을 얻습니다. 유일한 노출되지 않은 노드는 2이고 자식 도메인은 1-3입니다.
 *
 *   그러나 중복으로 인해 각 그룹에 대해 고유한 CPU를 계산하는 것이 더 
 *   복잡합니다. 예를 들어 NODE-1 NUMA-2 그룹을 고려하십시오. 두 그룹 모두 
 *   Node-0의 CPU를 포함하지만 해당 CPU는 실제로 해당 그룹에 도달하지 
 *   않습니다(그들은 다음 그룹에 있게 됩니다.  0-1,3).
 *
 *   이를 수정하려면 그룹 밸런스 마스크를 도입해야 합니다. 이 마스크는 (자식) 
 *   도메인 트리가 주어진 이 그룹에 도달할 수 있는 그룹의 CPU를 포함합니다. 
 *
 *   이를 통해 balance_cpu 및 sched_group_capacity 관계를 다시 한 번 계산할 수 있습니다.
 *
 *   XXX에는 balance_cpu가 고유하므로 sched_group_capacity 링크에 
 *   사용할 수 있는 방법에 대한 단어가 포함됩니다.
 *
 *   또 다른 '흥미로운' 토폴로지는 다음과 같습니다.
 *
 * Another 'interesting' topology is:
 *
 *   node   0   1   2   3
 *     0:  10  20  20  30
 *     1:  20  10  20  20
 *     2:  20  20  10  20
 *     3:  30  20  20  10
 *
 * Which looks a little like:
 *
 *   0 ----- 1
 *   |     / |
 *   |   /   |
 *   | /     |
 *   2 ----- 3
 *
 * 이 토폴로지는 비대칭이며 노드 1,2는 완전히 연결되어 있지만 노드 0,3은 
 * 그렇지 않습니다.
 *
 * 이로 인해 sched_domain이 각 CPU에 대해 동일한 번호가 아닌 몇 가지 특히 
 * 이상한 경우가 발생합니다. 고려하다:
 *
 * NUMA-2	0-3					         0-3
 *  groups:	{0-2},{1-3}				     {1-3},{0-2}
 *
 * NUMA-1	0-2		     0-3	0-3	 	 1-3
 *
 * NUMA-0	0		     1		2		 3
 */


/*
 * Build the balance mask; it contains only those CPUs that can arrive at this
 * group and should be considered to continue balancing.
 *
 * We do this during the group creation pass, therefore the group information
 * isn't complete yet, however since each group represents a (child) domain we
 * can fully construct this using the sched_domain bits (which are already
 * complete).
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   밸런스 마스크를 만듭니다. 여기에는 이 그룹에 도달할 수 있는 CPU만 포함되며 
 *   균형을 계속 유지하는 것으로 간주되어야 합니다.
 *
 *   그룹 생성 단계에서 이 작업을 수행하므로 그룹 정보가 아직 완전하지 않지만 
 *   각 그룹이 (하위) 도메인을 나타내므로 sched_domain 비트(이미 완료된)를 
 *   사용하여 이를 완전히 구성할 수 있습니다.
 *
 * - balance mask를 생성하고 설정한다.
 *   balance mask는 schedule group의 span과 거기에 포함되있는 하위 domain의 span이 같은 node의
 *   cpu들만 설정된다.
 *
 * - ex) node3 numa-2의 {2-3}이 @sg로 들어왔다고 가정한다.
 *              0                    1                2               3
 *                                  
 * NUMA-2       0-2                   0-3             0-3             1-3
 *  groups:     {0-1},1-3 c의{2}     {0-2},{2-3}     {1-3},{0-1}     {2-3},0-2의 child {1}
 * balance mask   0  ,        X        1  ,  3         2  ,  0         3  ,             X
 *                                   
 * NUMA-1       0-1                   0-2             1-3             2-3
 *  groups:     {0},{1}               {1},{2},{0}     {2},{3},{1}     {3},{2}
 *                                   
 * NUMA-0       0                     1               2               3
 */
static void
build_balance_mask(struct sched_domain *sd, struct sched_group *sg, struct cpumask *mask)
{
	const struct cpumask *sg_span = sched_group_span(sg);
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	cpumask_clear(mask);

/*
 * IAMROOT, 2023.04.22:
 * - sg->span과 child->span이 같은 경우가 있는 cpu들을 기록한다.
 */
	for_each_cpu(i, sg_span) {
		sibling = *per_cpu_ptr(sdd->sd, i);

		/*
		 * Can happen in the asymmetric case, where these siblings are
		 * unused. The mask will not be empty because those CPUs that
		 * do have the top domain _should_ span the domain.
		 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   이러한 sibling이 사용되지 않는 비대칭의 경우에 발생할 수 있습니다. 최상위 
 *   도메인이 있는 CPU가 도메인에 _반드시_ 있기 때문에 마스크가 비어 있지 않습니다.
 */
		if (!sibling->child)
			continue;

		/* If we would not end up here, we can't continue from here */
		if (!cpumask_equal(sg_span, sched_domain_span(sibling->child)))
			continue;

		cpumask_set_cpu(i, mask);
	}

	/* We must not have empty masks here */
	WARN_ON_ONCE(cpumask_empty(mask));
}

/*
 * XXX: This creates per-node group entries; since the load-balancer will
 * immediately access remote memory to construct this group's load-balance
 * statistics having the groups node local is of dubious benefit.
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   XXX: 이렇게 하면 노드별 그룹 항목이 생성됩니다. 로드 밸런서는 그룹 
 *   노드 로컬을 갖는 이 그룹의 로드 밸런싱 통계를 구성하기 위해 즉시 원격 
 *   메모리에 액세스하므로 의심스러운 이점이 있습니다.
 *
 * - 결정된 sibling domain으로 group을 만든다.
 * - ex) 
 * NUMA-2       0-2             
 *  groups:     {0-1},1-3 c의{2} 
 *                               
 * NUMA-1       0-1               
 *  groups:     {0},{1}        
 *                                 
 * NUMA-0       0                  
 *
 * 1) @sd가 numa-2인 경우, child는 numa-1
 * 2) @sd가 numa-1인 경우, child는 numa-0
 * 3) @sd가 numa-0인 경우, 자기자신으로 선택
 */
static struct sched_group *
build_group_from_child_sched_domain(struct sched_domain *sd, int cpu)
{
	struct sched_group *sg;
	struct cpumask *sg_span;

	sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
			GFP_KERNEL, cpu_to_node(cpu));

	if (!sg)
		return NULL;

	sg_span = sched_group_span(sg);
	if (sd->child)
		cpumask_copy(sg_span, sched_domain_span(sd->child));
	else
		cpumask_copy(sg_span, sched_domain_span(sd));

	atomic_inc(&sg->ref);
	return sg;
}

/*
 * IAMROOT, 2023.04.22:
 * - balance mask 설정 및 capacity 값들을 설정한다.
 */
static void init_overlap_sched_group(struct sched_domain *sd,
				     struct sched_group *sg)
{
	struct cpumask *mask = sched_domains_tmpmask2;
	struct sd_data *sdd = sd->private;
	struct cpumask *sg_span;
	int cpu;

	build_balance_mask(sd, sg, mask);
	cpu = cpumask_first(mask);

	sg->sgc = *per_cpu_ptr(sdd->sgc, cpu);

/*
 * IAMROOT, 2023.04.22:
 * - temp에 만들었던것을 최초 ref up때만 copy하고 이후는 기존과 비교해
 *   warn 출력만한다.
 */
	if (atomic_inc_return(&sg->sgc->ref) == 1)
		cpumask_copy(group_balance_mask(sg), mask);
	else
		WARN_ON_ONCE(!cpumask_equal(group_balance_mask(sg), mask));

	/*
	 * Initialize sgc->capacity such that even if we mess up the
	 * domains and no possible iteration will get us here, we won't
	 * die on a /0 trap.
	 */
	sg_span = sched_group_span(sg);
	sg->sgc->capacity = SCHED_CAPACITY_SCALE * cpumask_weight(sg_span);
	sg->sgc->min_capacity = SCHED_CAPACITY_SCALE;
	sg->sgc->max_capacity = SCHED_CAPACITY_SCALE;
}

/*
 * IAMROOT, 2023.04.22:
 * - @sd span에 포함이 되있는 sibling을 검색한다.
 */
static struct sched_domain *
find_descended_sibling(struct sched_domain *sd, struct sched_domain *sibling)
{
	/*
	 * The proper descendant would be the one whose child won't span out
	 * of sd
	 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   적절한 후손은 자녀가 sd를 벗어나지 않는 것입니다. 
 *
 * - @sd 범위에 있는 sibling->child가 있는 child를 찾는다.
 */
	while (sibling->child &&
	       !cpumask_subset(sched_domain_span(sibling->child),
			       sched_domain_span(sd)))
		sibling = sibling->child;

	/*
	 * As we are referencing sgc across different topology level, we need
	 * to go down to skip those sched_domains which don't contribute to
	 * scheduling because they will be degenerated in cpu_attach_domain
	 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   서로 다른 토폴로지 레벨에서 sgc를 참조하므로 스케줄링에 기여하지 않는 
 *   sched_domains는 cpu_attach_domain에서 퇴화되기 때문에 아래로 내려가서 
 *   건너뛰어야 합니다. 
 *
 * - sibling->child과 sibling의 span이 같으면 한번 더 내려간다.
 */
	while (sibling->child &&
	       cpumask_equal(sched_domain_span(sibling->child),
			     sched_domain_span(sibling)))
		sibling = sibling->child;

	return sibling;
}

/*
 * IAMROOT, 2023.04.22:
 * - @sd에 대해서 sg(schedule group)을 생성하고 sg의 balance mask 및 capacity값들을 
 *   설정한다.
 */
static int
build_overlap_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL, *sg;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered = sched_domains_tmpmask;
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	cpumask_clear(covered);

	for_each_cpu_wrap(i, span, cpu) {
		struct cpumask *sg_span;

		if (cpumask_test_cpu(i, covered))
			continue;

/*
 * IAMROOT, 2023.04.22:
 * - sd_data에서 i에 해당하는 sibling을 가져온다.
 */
		sibling = *per_cpu_ptr(sdd->sd, i);

		/*
		 * Asymmetric node setups can result in situations where the
		 * domain tree is of unequal depth, make sure to skip domains
		 * that already cover the entire range.
		 *
		 * In that case build_sched_domains() will have terminated the
		 * iteration early and our sibling sd spans will be empty.
		 * Domains should always include the CPU they're built on, so
		 * check that.
		 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   비대칭 노드 설정으로 인해 도메인 트리의 깊이가 다른 상황이 발생할 
 *   수 있으므로 이미 전체 범위를 포함하는 도메인을 건너뛰어야 합니다.
 *
 *   이 경우 build_sched_domains()는 반복을 일찍 종료하고 형제 SD 
 *   범위는 비어 있게 됩니다.
 *   도메인은 항상 도메인이 구축된 CPU를 포함해야 하므로 확인하십시오.
 */
		if (!cpumask_test_cpu(i, sched_domain_span(sibling)))
			continue;

		/*
		 * Usually we build sched_group by sibling's child sched_domain
		 * But for machines whose NUMA diameter are 3 or above, we move
		 * to build sched_group by sibling's proper descendant's child
		 * domain because sibling's child sched_domain will span out of
		 * the sched_domain being built as below.
		 *
		 * Smallest diameter=3 topology is:
		 *
		 *   node   0   1   2   3
		 *     0:  10  20  30  40
		 *     1:  20  10  20  30
		 *     2:  30  20  10  20
		 *     3:  40  30  20  10
		 *
		 *   0 --- 1 --- 2 --- 3
		 *
		 * NUMA-3       0-3             N/A             N/A             0-3
		 *  groups:     {0-2},{1-3}                                     {1-3},{0-2}
		 *
		 * NUMA-2       0-2             0-3             0-3             1-3
		 *  groups:     {0-1},{1-3}     {0-2},{2-3}     {1-3},{0-1}     {2-3},{0-2}
		 *
		 * NUMA-1       0-1             0-2             1-3             2-3
		 *  groups:     {0},{1}         {1},{2},{0}     {2},{3},{1}     {3},{2}
		 *
		 * NUMA-0       0               1               2               3
		 *
		 * The NUMA-2 groups for nodes 0 and 3 are obviously buggered, as the
		 * group span isn't a subset of the domain span.
		 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 * -----------------------
 *  위 주석은 child 개념이 도입되기전에 대한 설명이다. child 도입 이후로 groups 선택이 
 *  개선되었다.
 * ------------------------
 *   일반적으로 우리는 형제의 자식 sched_domain으로 sched_group을 
 *   빌드합니다. 그러나 NUMA 직경이 3 이상인 시스템의 경우 형제의 자식 
 *   sched_domain이 아래와 같이 빌드되는 sched_domain을 벗어나기 때문에 
 *   형제의 적절한 자손의 자식 도메인으로 sched_group을 빌드합니다.
 *
 *   최소 직경=3 토폴로지는 다음과 같습니다.
 *   node   0   1   2   3
 *     0:  10  20  30  40
 *     1:  20  10  20  30
 *     2:  30  20  10  20
 *     3:  40  30  20  10
 *
 *   0 --- 1 --- 2 --- 3
 *
 *              0                    1                2               3
 * NUMA-3       0-3                   N/A             N/A             0-3
 *  groups:     {0-2},{1-3}                                           {1-3},{0-2}
 * (40)         0, 3                                                   3, 0
 *                                  
 * NUMA-2       0-2                   0-3             0-3             1-3
 *  groups:     {0-1},1-3 c의{2}     {0-2},{2-3}     {1-3},{0-1}     {2-3},0-2의 child {1}
 * (30)         0, 2                  1, 3            2, 0            3, 1
 *                                   
 * NUMA-1       0-1                   0-2             1-3             2-3
 *  groups:     {0},{1}               {1},{2},{0}     {2},{3},{1}     {3},{2}
 * (20)                              
 *                                   
 * NUMA-0       0                     1               2               3
 *
 * ------------------------
 *
 * 그룹 범위가 도메인 범위의 하위 집합이 아니므로 노드 0 및 3에 대한 NUMA-2 
 * 그룹은 분명히 버그가 있습니다.
 *
 * - 주석해석
 *   > node는 다음과 같이 연결되있다는 예로 되있다. 자기자신(10)에 거리마다 10씩 증가.
 *          +10      +10      +10
 *       0 ------ 1 ------ 2 ----- 3
 *   > NUMA-2의 node 0와 node 3에서, 범위에 distacke 40에 해당하는 node 3이 포함되며,
 *   이런 로직이 될수 밖에 없다는 구조로 설명하고 있다.
 *
 * - 현재 범위에 포함이 안된 sibling이면 next를 검색한다.
 *
 * ex) 위 주석 예제의 NUMA-2 node3의 예로 든다.
 *   step1) node 3으로 시작 
 *   step3) node 3의 numa-2가 sibling으로 선택 (1-3)
 *   step3) node 3, numa-2의 child(2-3)가 span에 전부 포함되므로 {2-3}으로 group이 생성
 *   step4) 이후 for wrap을 통해 node 1로 for동작시작.
 *   step5) node 1, numa-2가 sibling으로 선택 (0-3)
 *   step6) node 1, numa-2의 child(0-2)으로 선택되지만 subset이 아니게 판정. child을 sibling으로
 *          재선택
 *   step7) sibling(0-2)의 child인 node1 numa-0가 span에 전부 포함되므로 {1}으로 group 생성
 */
		if (sibling->child &&
		    !cpumask_subset(sched_domain_span(sibling->child), span))
			sibling = find_descended_sibling(sd, sibling);

/*
 * IAMROOT, 2023.04.22:
 * - sg 생성 및 설정
 */
		sg = build_group_from_child_sched_domain(sibling, cpu);
		if (!sg)
			goto fail;

		sg_span = sched_group_span(sg);
		cpumask_or(covered, covered, sg_span);

		init_overlap_sched_group(sibling, sg);

/*
 * IAMROOT, 2023.04.22:
 * - 순환연결리스트 구성.ㅣ
 */
		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
		last->next = first;
	}
	sd->groups = first;

	return 0;

fail:
	free_sched_groups(first, 0);

	return -ENOMEM;
}


/*
 * Package topology (also see the load-balance blurb in fair.c)
 *
 * The scheduler builds a tree structure to represent a number of important
 * topology features. By default (default_topology[]) these include:
 *
 *  - Simultaneous multithreading (SMT)
 *  - Multi-Core Cache (MC)
 *  - Package (DIE)
 *
 * Where the last one more or less denotes everything up to a NUMA node.
 *
 * The tree consists of 3 primary data structures:
 *
 *	sched_domain -> sched_group -> sched_group_capacity
 *	    ^ ^             ^ ^
 *          `-'             `-'
 *
 * The sched_domains are per-CPU and have a two way link (parent & child) and
 * denote the ever growing mask of CPUs belonging to that level of topology.
 *
 * Each sched_domain has a circular (double) linked list of sched_group's, each
 * denoting the domains of the level below (or individual CPUs in case of the
 * first domain level). The sched_group linked by a sched_domain includes the
 * CPU of that sched_domain [*].
 *
 * Take for instance a 2 threaded, 2 core, 2 cache cluster part:
 *
 * CPU   0   1   2   3   4   5   6   7
 *
 * DIE  [                             ]
 * MC   [             ] [             ]
 * SMT  [     ] [     ] [     ] [     ]
 *
 *  - or -
 *
 * DIE  0-7 0-7 0-7 0-7 0-7 0-7 0-7 0-7
 * MC	0-3 0-3 0-3 0-3 4-7 4-7 4-7 4-7
 * SMT  0-1 0-1 2-3 2-3 4-5 4-5 6-7 6-7
 *
 * CPU   0   1   2   3   4   5   6   7
 *
 * One way to think about it is: sched_domain moves you up and down among these
 * topology levels, while sched_group moves you sideways through it, at child
 * domain granularity.
 *
 * sched_group_capacity ensures each unique sched_group has shared storage.
 *
 * There are two related construction problems, both require a CPU that
 * uniquely identify each group (for a given domain):
 *
 *  - The first is the balance_cpu (see should_we_balance() and the
 *    load-balance blub in fair.c); for each group we only want 1 CPU to
 *    continue balancing at a higher domain.
 *
 *  - The second is the sched_group_capacity; we want all identical groups
 *    to share a single sched_group_capacity.
 *
 * Since these topologies are exclusive by construction. That is, its
 * impossible for an SMT thread to belong to multiple cores, and cores to
 * be part of multiple caches. There is a very clear and unique location
 * for each CPU in the hierarchy.
 *
 * Therefore computing a unique CPU for each group is trivial (the iteration
 * mask is redundant and set all 1s; all CPUs in a group will end up at _that_
 * group), we can simply pick the first CPU in each group.
 *
 *
 * [*] in other words, the first group of each domain is its child domain.
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   패키지 토폴로지(fair.c의 로드 밸런스 설명 참조) 스케줄러는 여러 중요한 
 *   토폴로지 기능을 나타내는 트리 구조를 구축합니다. 기본적으로(default_topology[]) 
 *   여기에는 다음이 포함됩니다.
 *
 *   - Simultaneous multithreading (SMT)
 *   - Multi-Core Cache (MC)
 *   - Package (DIE)
 *  
 *   여기서 마지막 하나는 NUMA 노드까지의 모든 것을 나타냅니다.
 *
 *   트리는 3가지 기본 데이터 구조로 구성됩니다.
 *
 *		sched_domain -> sched_group -> sched_group_capacity
 *		    ^ ^             ^ ^
 *            `-'             `-'
 *  
 *   sched_domains는 CPU당이며 양방향 링크(상위 및 하위)를 가지며 해당 토폴로지
 *   수준에 속하는 CPU의 계속 증가하는 마스크를 나타냅니다. 
 *
 *   각 sched_domain에는 sched_group의 순환(이중) 연결 목록이 있으며, 각각은 
 *   아래 수준의 도메인(또는 첫 번째 도메인 수준의 경우 개별 CPU)을 나타냅니다. 
 *   sched_domain으로 연결된 sched_group에는 해당 sched_domain[*]의 CPU가 포함됩니다.
 *
 *   2개의 스레드, 2개의 코어, 2개의 캐시 클러스터 부분을 예로 들어 보겠습니다.
 *
 *    CPU   0   1   2   3   4   5   6   7
 *   
 *    DIE  [                             ]
 *    MC   [             ] [             ]
 *    SMT  [     ] [     ] [     ] [     ]
 *   
 *     - or -
 *   
 *    DIE  0-7 0-7 0-7 0-7 0-7 0-7 0-7 0-7
 *    MC   0-3 0-3 0-3 0-3 4-7 4-7 4-7 4-7
 *    SMT  0-1 0-1 2-3 2-3 4-5 4-5 6-7 6-7
 *   
 *    CPU   0   1   2   3   4   5   6   7
 *   
 *  그것에 대해 생각하는 한 가지 방법은 다음과 같습니다.
 *  sched_domain은 이러한 토폴로지 수준 사이에서 위아래로 이동하는 반면 sched_group은 
 *  하위 도메인 세분성에서 옆으로 이동합니다.
 *
 *  sched_group_capacity는 각각의 고유한 sched_group이 shared storage를 갖도록 합니다.
 *  (ps. sgc : sg에 있는 똑같은것 데이터를 모아놓는 곳)
 *
 *  두 가지 관련 구성 문제가 있으며 둘 다 각 그룹을 고유하게 식별하는 CPU가 
 *  필요합니다(주어진 도메인에 대해).
 *
 *  - 첫 번째는 balance_cpu입니다(should_we_balance() 및 fair.c의 부하 균형 참조). 
 *  각 그룹에 대해 더 높은 도메인에서 1개의 CPU만 계속 균형을 유지하기를 원합니다.
 *
 *  - 두 번째는 sched_group_capacity입니다. 모든 동일한 그룹이 단일 sched_group_capacity를 
 *  공유하기를 원합니다.
 *
 *  이러한 토폴로지는 구축에 의해 배타적이기 때문입니다. 즉, SMT 스레드가 여러 코어에 
 *  속하고 코어가 여러 캐시의 일부가 되는 것은 불가능합니다. 계층 구조의 각 CPU에는 
 *  매우 명확하고 고유한 위치가 있습니다.
 *
 *  따라서 각 그룹에 대해 고유한 CPU를 계산하는 것은 간단합니다(반복 마스크는 중복되고 
 *  모두 1로 설정됩니다. 그룹의 모든 CPU는 _that_ 그룹에서 끝납니다). 각 그룹에서 
 *  첫 번째 CPU를 간단히 선택할 수 있습니다.
 *
 *  [*] 즉, 각 도메인의 첫 번째 그룹은 자식 도메인입니다.
 *
 *  - @sdd, @cpu에 해당하는 @sd의 하위 doamin의 가장 처음 cpu의 sg를 return한다.
 *
 *  - sd1 sd2 sd3 sd4
 *     |  /   /   /
 *     +-+----+--+
*      |
 *     sg
 *     |
 *     sgc
 *
 *
 * - 예시.)
 *   > die의 0-3까지에선 first cpu가 0, 4-7에선 first cpu가 4가 된다.
 *   > MC는 마지막 sd이므로 한개씩 연결된다.
 *   > sg끼리는 순환연결리스트로 이뤄진다.
 *   > sgc는 각 sg에 한개씩 연결된다.
 *
 * DIE | 0 1 2 3         4 5 6 7 | <-- sd (span : 0xff)
 *       |               |
 *      +0---------------4+        <-- sg (sgc는 sg 밑에 하나씩.)
 *      +-----------------+               (0,4의 span : 0x0f, 0xf0)
 *
 * MC  | 0 1 2 3 |     | 4 5 6 7 | <-- sd (span : 0x0f, 0xf0)
 *       | | | |         | | | |
 *      +0-1-2-3+       +4-5-6-7+  <-- sg (sgc는 sg 밑에 하나씩.)
 *      +-------+       +-------+
 */
static struct sched_group *get_group(int cpu, struct sd_data *sdd)
{
	struct sched_domain *sd = *per_cpu_ptr(sdd->sd, cpu);
	struct sched_domain *child = sd->child;
	struct sched_group *sg;
	bool already_visited;

/*
 * IAMROOT, 2023.04.22:
 * - 하위 domain과 동일한 group을 사용하고, cpu번호는 그중 가장 처음 cpu를 사용한다.
 */
	if (child)
		cpu = cpumask_first(sched_domain_span(child));

	sg = *per_cpu_ptr(sdd->sg, cpu);
	sg->sgc = *per_cpu_ptr(sdd->sgc, cpu);

	/* Increase refcounts for claim_allocations: */
	already_visited = atomic_inc_return(&sg->ref) > 1;
	/* sgc visits should follow a similar trend as sg */
	WARN_ON(already_visited != (atomic_inc_return(&sg->sgc->ref) > 1));

	/* If we have already visited that group, it's already initialized. */
/*
 * IAMROOT, 2023.04.22:
 * - 한번이라도 만들어졌으면 ref만 증가시키면 된다. return.
 */
	if (already_visited)
		return sg;

/*
 * IAMROOT, 2023.04.22:
 * - child domain span을 그대로 group span으로 복사한다.
 */
	if (child) {
		cpumask_copy(sched_group_span(sg), sched_domain_span(child));
		cpumask_copy(group_balance_mask(sg), sched_group_span(sg));
	} else {

/*
 * IAMROOT, 2023.04.22:
 * - 최하단인 경우는 @cpu를 사용하면 된다.
 */
		cpumask_set_cpu(cpu, sched_group_span(sg));
		cpumask_set_cpu(cpu, group_balance_mask(sg));
	}

	sg->sgc->capacity = SCHED_CAPACITY_SCALE * cpumask_weight(sched_group_span(sg));
	sg->sgc->min_capacity = SCHED_CAPACITY_SCALE;
	sg->sgc->max_capacity = SCHED_CAPACITY_SCALE;

	return sg;
}

/*
 * build_sched_groups will build a circular linked list of the groups
 * covered by the given span, will set each group's ->cpumask correctly,
 * and will initialize their ->sgc.
 *
 * Assumes the sched_domain tree is fully constructed
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   build_sched_groups는 주어진 span에 포함되는 그룹의 순환 연결 목록을 
 *   작성하고 각 그룹의 -> cpumask를 올바르게 설정하고 -> sgc를 초기화합니다.
 *
 *   sched_domain 트리가 완전히 구성되었다고 가정합니다.
 *
 * - @sd의 span을 범위로 각 cpu의 sg를 순환연결리스트로 구성한다.
 * - 일반 domain의 경우 group과 동일하다. span이 child를 전부 포함한다.
 */
static int
build_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL;
	struct sd_data *sdd = sd->private;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered;
	int i;

	lockdep_assert_held(&sched_domains_mutex);
	covered = sched_domains_tmpmask;

	cpumask_clear(covered);

/*
 * IAMROOT, 2023.04.22:
 * - @span 범위를 순환한다.
 * - ex) span값이 다음과 같다고 가정한다.
 *   (0, 1, 2, 3) (4, 5, 6, 7)
 *   이경우 group이 2번만들어지게 되는 개념이되고 iterate는 2번만 get_group을 할것이다.
 */
	for_each_cpu_wrap(i, span, cpu) {
		struct sched_group *sg;

/*
 * IAMROOT, 2023.04.22:
 * - 한번한건 pass
 */
		if (cpumask_test_cpu(i, covered))
			continue;

		sg = get_group(i, sdd);

		cpumask_or(covered, covered, sched_group_span(sg));

/*
 * IAMROOT, 2023.04.22:
 * - 순환단방향연결리스트 형식으로 연결한다.
 */
		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
	}
	last->next = first;
	sd->groups = first;

	return 0;
}

/*
 * Initialize sched groups cpu_capacity.
 *
 * cpu_capacity indicates the capacity of sched group, which is used while
 * distributing the load between different sched groups in a sched domain.
 * Typically cpu_capacity for all the groups in a sched domain will be same
 * unless there are asymmetries in the topology. If there are asymmetries,
 * group having more cpu_capacity will pickup more load compared to the
 * group having less cpu_capacity.
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   스케줄 그룹 cpu_capacity를 초기화합니다.
 *
 *   cpu_capacity는 sched 그룹의 용량을 나타내며, sched 도메인에서 서로 
 *   다른 sched 그룹 간에 부하를 분산할 때 사용됩니다.
 *   일반적으로 sched 도메인의 모든 그룹에 대한 cpu_capacity는 토폴로지에 
 *   비대칭이 없는 한 동일합니다. 비대칭이 있는 경우 cpu_capacity가 더 
 *   많은 그룹은 cpu_capacity가 더 적은 그룹에 비해 더 많은 부하를 받습니다.
 *
 * - 1. smt일 경우 각 sg마다 asym_prefer_cpu(가장빠른 cpu)를 비교하여 업데이트한다.
 *   2. 각 group마다 balance cpu에 한하여 group capacity를 계산한다.
 */
static void init_sched_groups_capacity(int cpu, struct sched_domain *sd)
{
	struct sched_group *sg = sd->groups;

	WARN_ON(!sg);

/*
 * IAMROOT, 2023.04.22:
 * - smt domapin인 경우, 가장 빠른 cpu를 알아낸다.
 */
	do {
		int cpu, max_cpu = -1;

		sg->group_weight = cpumask_weight(sched_group_span(sg));

/*
 * IAMROOT, 2023.04.22:
 * - SMT domain이 아닌것들은 continue
 */
		if (!(sd->flags & SD_ASYM_PACKING))
			goto next;

		for_each_cpu(cpu, sched_group_span(sg)) {
/*
 * IAMROOT, 2023.04.22:
 * - max_cpu를 정한다.
 */
			if (max_cpu < 0)
				max_cpu = cpu;
			else if (sched_asym_prefer(cpu, max_cpu))
				max_cpu = cpu;
		}
		sg->asym_prefer_cpu = max_cpu;

next:
		sg = sg->next;
	} while (sg != sd->groups);

/*
 * IAMROOT, 2023.04.22:
 * - balance mask의 첫번째 cpu에 한해서만 update_group_capacity()가 동작한다.
 */
	if (cpu != group_balance_cpu(sg))
		return;

/*
 * IAMROOT, 2023.04.22:
 * - group capacity를 산출한다.
 */
	update_group_capacity(sd, cpu);
}

/*
 * Asymmetric CPU capacity bits
 */
struct asym_cap_data {
	struct list_head link;
	unsigned long capacity;
	unsigned long cpus[];
};

/*
 * Set of available CPUs grouped by their corresponding capacities
 * Each list entry contains a CPU mask reflecting CPUs that share the same
 * capacity.
 * The lifespan of data is unlimited.
 */
/*
 * IAMROOT, 2023.04.15:
 * - asym_cpu_capacity_update_data()참고 
 * - ex) CPU0~3 : 1024, CPU4-7 : 436 인 경우
 *   1024, 436에 대한 node가 존재한다.
 *   node1->capacity = 1024;
 *   node1->cpus = cpu bitmaps에 cpu 0~3 set.
 *   node2->capacity = 436;
 *   node2->cpus = cpu bitmaps에 cpu 4~7 set.
 */
static LIST_HEAD(asym_cap_list);

#define cpu_capacity_span(asym_data) to_cpumask((asym_data)->cpus)

/*
 * Verify whether there is any CPU capacity asymmetry in a given sched domain.
 * Provides sd_flags reflecting the asymmetry scope.
 */
/*
 * IAMROOT, 2023.04.15:
 * - papago
 *   지정된 sched 도메인에 CPU 용량 비대칭이 있는지 확인합니다.
 *   비대칭 범위를 반영하는 sd_flags를 제공합니다.
 * - ex) 0,1 = 1024 capacity, 0,2 = 512 capacity, asym_cap_list에 전부있다고 가정
 *
 *   SMT 0,1 / 2,3 -> 0,1 : count 1, miss 1 
 *                    2,3 : count 1, miss 1
 *                    각각 동일한 cpu capacity의 그룹이므로 비대칭이 아니다.
 *                    return 0.
 *   DIE 0,1,2,3 -> count 2, miss 0
 *                  다른 cpu capacity에 있기 때문에 비대칭이다. return 
 *                  return SD_ASYM_CPUCAPACITY | SD_ASYM_CPUCAPACITY_FULL
 * - return
 *   0 : asym이 없으면 return0.
 *   SD_ASYM_CPUCAPACITY : asym_cap_list가 @sd_span에 2번 이상 있으면서,
 *                         asm_cap_list에 없지만 cpu_map에도 있는 경우
 *   SD_ASYM_CPUCAPACITY | SD_ASYM_CPUCAPACITY_FULL : @sd_span이 @asym_cap_list에 2번 이상 
 *   존재하면서 sd_span에 없는 asym_cap_list도 모두 cpu_map에 없는 경우
 *
 *   case1) full 인 경우
 *   | asym_cap_list | sd_span |  cpu_map
 *         B             O           -
 *         M             O           -
 *         L             O           -
 *
 *   case2) full 이 아닌 경우
 *   | asym_cap_list | sd_span |  cpu_map
 *         B             O           -
 *         M             O           -
 *         L             X           O
 *
 *   case3) asym_cap_list에 하나만 존재하는경우. 즉 asym이 아닌경우
 *   | asym_cap_list | sd_span |  cpu_map
 *         B             O           -
 *         M             X           -
 *         L             X           -
 */
static inline int
asym_cpu_capacity_classify(const struct cpumask *sd_span,
			   const struct cpumask *cpu_map)
{
	struct asym_cap_data *entry;
	int count = 0, miss = 0;

	/*
	 * Count how many unique CPU capacities this domain spans across
	 * (compare sched_domain CPUs mask with ones representing  available
	 * CPUs capacities). Take into account CPUs that might be offline:
	 * skip those.
	 */
/*
 * IAMROOT, 2023.04.15:
 * - papago
 *   이 도메인에 걸쳐 있는 고유한 CPU 용량의 수를 계산합니다(sched_domain 
 *   CPU 마스크를 사용 가능한 CPU 용량을 나타내는 마스크와 비교).
 *   오프라인일 수 있는 CPU를 고려하십시오.
 *   건너 뛰십시오.
 * - span이 포함되있으면 count, topology범위를 벗어난거라면 miss가 된다.
 */
	list_for_each_entry(entry, &asym_cap_list, link) {
		if (cpumask_intersects(sd_span, cpu_capacity_span(entry)))
			++count;
		else if (cpumask_intersects(cpu_map, cpu_capacity_span(entry)))
			++miss;
	}

	WARN_ON_ONCE(!count && !list_empty(&asym_cap_list));

	/* No asymmetry detected */
	if (count < 2)
		return 0;
	/* Some of the available CPU capacity values have not been detected */
	if (miss)
		return SD_ASYM_CPUCAPACITY;

	/* Full asymmetry */
	return SD_ASYM_CPUCAPACITY | SD_ASYM_CPUCAPACITY_FULL;

}

/*
 * IAMROOT, 2023.04.15:
 * - @cpu의 capacity와 동일한게 이미 asym_cap_list에 있는지 확인하고, 없으면 
 *   추가한다. 처리후 해당 entry의 cpu bitmap에 @cpu를 set한다.
 */
static inline void asym_cpu_capacity_update_data(int cpu)
{
	unsigned long capacity = arch_scale_cpu_capacity(cpu);
	struct asym_cap_data *entry = NULL;

/*
 * IAMROOT, 2023.04.15:
 * - 해당 capactiy에 대해 이미 존재하면 cpu bitmap에만 cpu set..
 */
	list_for_each_entry(entry, &asym_cap_list, link) {
		if (capacity == entry->capacity)
			goto done;
	}

/*
 * IAMROOT, 2023.04.15:
 * - 자료구조를 할당하고 asym_cap_list에 추가한다.
 */
	entry = kzalloc(sizeof(*entry) + cpumask_size(), GFP_KERNEL);
	if (WARN_ONCE(!entry, "Failed to allocate memory for asymmetry data\n"))
		return;
	entry->capacity = capacity;
	list_add(&entry->link, &asym_cap_list);
done:
	__cpumask_set_cpu(cpu, cpu_capacity_span(entry));
}

/*
 * Build-up/update list of CPUs grouped by their capacities
 * An update requires explicit request to rebuild sched domains
 * with state indicating CPU topology changes.
 */
/*
 * IAMROOT. 2023.04.15:
 * - google-translate
 * cpu capacities로 그룹화된 CPU의 빌드업/업데이트 목록 업데이트에는 CPU 토폴로지 변경을
 * 나타내는 상태로 sched 도메인을 재구축하라는 명시적 요청이 필요합니다.
 *
 * - cpu capacity별 자료구조인 asym_cap_list를 설정한다. 
 */
static void asym_cpu_capacity_scan(void)
{
	struct asym_cap_data *entry, *next;
	int cpu;

	list_for_each_entry(entry, &asym_cap_list, link)
		cpumask_clear(cpu_capacity_span(entry));

/*
 * IAMROOT, 2023.04.15:
 * - HK_FLAG_DOMAIN이 set되있다면 isolate가 안된 cpu들에 한한 iterate를 하면서,
 *   해당 cpu capacity에 대해 asym_cap_list에 추가한다
 */
	for_each_cpu_and(cpu, cpu_possible_mask, housekeeping_cpumask(HK_FLAG_DOMAIN))
		asym_cpu_capacity_update_data(cpu);

/*
 * IAMROOT, 2023.04.15:
 * - 해당 cpapcity에 대한 cpu가 한개도 없으면 그냥 entry를 지워버린다.
 */
	list_for_each_entry_safe(entry, next, &asym_cap_list, link) {
		if (cpumask_empty(cpu_capacity_span(entry))) {
			list_del(&entry->link);
			kfree(entry);
		}
	}

	/*
	 * Only one capacity value has been detected i.e. this system is symmetric.
	 * No need to keep this data around.
	 */
/*
 * IAMROOT, 2023.04.15:
 * - 단일 capacity면 관리할 필요가없으므로(모든 cpu가 같다는것.) 그냥 다 지워버린다.
 */
	if (list_is_singular(&asym_cap_list)) {
		entry = list_first_entry(&asym_cap_list, typeof(*entry), link);
		list_del(&entry->link);
		kfree(entry);
	}
}

/*
 * Initializers for schedule domains
 * Non-inlined to reduce accumulated stack pressure in build_sched_domains()
 */

static int default_relax_domain_level = -1;
int sched_domain_level_max;

static int __init setup_relax_domain_level(char *str)
{
	if (kstrtoint(str, 0, &default_relax_domain_level))
		pr_warn("Unable to set relax_domain_level\n");

	return 1;
}
__setup("relax_domain_level=", setup_relax_domain_level);

/*
 * IAMROOT, 2023.04.22:
 * - 1. attr없거나 relax_domain_level이 설정이 안되있는 경우.
 *   default(kernel param : relax_domain_level)를 request로 사용한다.
 *   default가 disable 되있으면 아무것도 안한다.
 *   2. attr에 relax_domain_level이 있는 경우 해당 값을 request로 사용한다.,
 *
 *   request가 sd->level보다 높다면 SD_BALANCE_WAKE, SD_BALANCE_NEWIDLE을 끈다.
 *
 * - | attr->relax_domain_level | default_relax_domain_level | 결과
 *   |  X                       |  X                         | none
 *   |  X                       |  O                         | default_relax_domain_level
 *   |  O                       |  -                         | attr->relax_domain_level
 */
static void set_domain_attribute(struct sched_domain *sd,
				 struct sched_domain_attr *attr)
{
	int request;

	if (!attr || attr->relax_domain_level < 0) {
		if (default_relax_domain_level < 0)
			return;
		request = default_relax_domain_level;
	} else
		request = attr->relax_domain_level;

	if (sd->level > request) {
		/* Turn off idle balance on this domain: */
		sd->flags &= ~(SD_BALANCE_WAKE|SD_BALANCE_NEWIDLE);
	}
}

static void __sdt_free(const struct cpumask *cpu_map);
static int __sdt_alloc(const struct cpumask *cpu_map);

/*
 * IAMROOT, 2023.04.30:
 * - sdt 빌드시 임시로 사용한 pcpu 와 할당된 구조체 메모리중 ref가 없는 것 free.
 * - sd 구조체는 모두 남겨지게 된다.(degenerate 로 삭제된 것 제외)
 */
static void __free_domain_allocs(struct s_data *d, enum s_alloc what,
				 const struct cpumask *cpu_map)
{
	switch (what) {
	case sa_rootdomain:
		if (!atomic_read(&d->rd->refcount))
			free_rootdomain(&d->rd->rcu);
		fallthrough;
	case sa_sd:
		free_percpu(d->sd);
		fallthrough;
	case sa_sd_storage:
		__sdt_free(cpu_map);
		fallthrough;
	case sa_none:
		break;
	}
}

/*
 * IAMROOT, 2023.04.15:
 * - sdt(schedule domain topology)별 pcpu 할당 및 초기화,
 *   @d에 대한 schedule domain, root domain 초기화.
 */
static enum s_alloc
__visit_domain_allocation_hell(struct s_data *d, const struct cpumask *cpu_map)
{
	memset(d, 0, sizeof(*d));

/*
 * IAMROOT, 2023.04.15:
 * - 각 sdt별 pcp할당 및 초기화.
 */
	if (__sdt_alloc(cpu_map))
		return sa_sd_storage;
	d->sd = alloc_percpu(struct sched_domain *);
	if (!d->sd)
		return sa_sd_storage;

/*
 * IAMROOT, 2023.04.15:
 * - rootdomain 생성.
 */
	d->rd = alloc_rootdomain();
	if (!d->rd)
		return sa_sd;

	return sa_rootdomain;
}

/*
 * NULL the sd_data elements we've used to build the sched_domain and
 * sched_group structure so that the subsequent __free_domain_allocs()
 * will not free the data we're using.
 */
/*
 * IAMROOT, 2023.04.22:
 * - papago
 *   후속 __free_domain_allocs()가 사용 중인 데이터를 해제하지 않도록 
 *   sched_domain 및 sched_group 구조를 빌드하는 데 사용한 sd_data 
 *   요소를 NULL로 설정합니다.
 *
 * - ref up이 된것들은 삭제하지 말라는 의미에서 NULL을 넣는다.
 */
static void claim_allocations(int cpu, struct sched_domain *sd)
{
	struct sd_data *sdd = sd->private;

	WARN_ON_ONCE(*per_cpu_ptr(sdd->sd, cpu) != sd);
	*per_cpu_ptr(sdd->sd, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sds, cpu))->ref))
		*per_cpu_ptr(sdd->sds, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sg, cpu))->ref))
		*per_cpu_ptr(sdd->sg, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sgc, cpu))->ref))
		*per_cpu_ptr(sdd->sgc, cpu) = NULL;
}

#ifdef CONFIG_NUMA
enum numa_topology_type sched_numa_topology_type;

static int			sched_domains_numa_levels;
/*
 * IAMROOT, 2023.04.15:
 * - build중인 schedule domain 표시
 */
static int			sched_domains_curr_level;

int				sched_max_numa_distance;
static int			*sched_domains_numa_distance;

/*
 * IAMROOT, 2023.04.15:
 * - sched_domains_numa_masks[distance][node id] = nodemask
 * - sched_init_numa()에서 만들어진다.
 * - node id를 기준으로 distance이하의 인접 node에대한 nodemask가 설정된다.
 */
static struct cpumask		***sched_domains_numa_masks;
int __read_mostly		node_reclaim_distance = RECLAIM_DISTANCE;

static unsigned long __read_mostly *sched_numa_onlined_nodes;
#endif

/*
 * SD_flags allowed in topology descriptions.
 *
 * These flags are purely descriptive of the topology and do not prescribe
 * behaviour. Behaviour is artificial and mapped in the below sd_init()
 * function:
 *
 *   SD_SHARE_CPUCAPACITY   - describes SMT topologies
 *   SD_SHARE_PKG_RESOURCES - describes shared caches
 *   SD_NUMA                - describes NUMA topologies
 *
 * Odd one out, which beside describing the topology has a quirk also
 * prescribes the desired behaviour that goes along with it:
 *
 *   SD_ASYM_PACKING        - describes SMT quirks
 */
#define TOPOLOGY_SD_FLAGS		\
	(SD_SHARE_CPUCAPACITY	|	\
	 SD_SHARE_PKG_RESOURCES |	\
	 SD_NUMA		|	\
	 SD_ASYM_PACKING)

/*
 * IAMROOT, 2023.04.15:
 * - schedule domain을 초기화한다.
 *   1. smt/mc/die/numa 에 따른 sd_flags() 설정
 *   2. sd_span에 따른 asym flag 추가.
 *   3. 설정한 flags에 따라 imbalance_pct, cache_nice_tries, flags 값 설정.
 *   4. cache공유가 있다면 schedule domain share를 shared에 등록.
 */
static struct sched_domain *
sd_init(struct sched_domain_topology_level *tl,
	const struct cpumask *cpu_map,
	struct sched_domain *child, int cpu)
{
	struct sd_data *sdd = &tl->data;
	struct sched_domain *sd = *per_cpu_ptr(sdd->sd, cpu);
	int sd_id, sd_weight, sd_flags = 0;
	struct cpumask *sd_span;

#ifdef CONFIG_NUMA
	/*
	 * Ugly hack to pass state to sd_numa_mask()...
	 */
/*
 * IAMROOT, 2023.04.15:
 * - 현재 진행중인 sdl을 표시해놓는 개념이다.
 */
	sched_domains_curr_level = tl->numa_level;
#endif

/*
 * IAMROOT, 2023.04.15:
 * - tl별 mask함수에서 @@cpu에 맞는 mask를 가져오고, 해당 mask의 weight를 가져온다.
 *   ex) cpu_smt_mask,
 *       cpu_coregroup_mask,
 *       cpu_cpu_mask,
 *       sd_numa_mask
 */
	sd_weight = cpumask_weight(tl->mask(cpu));


/*
 * IAMROOT, 2023.04.15:
 * - cpu_numa_flags : SD_NUMA
 *   cpu_core_flags : SD_SHARE_PKG_RESOURCES
 *   cpu_smt_flags  : D_SHARE_CPUCAPACITY | SD_SHARE_PKG_RESOURCES
 */
	if (tl->sd_flags)
		sd_flags = (*tl->sd_flags)();

/*
 * IAMROOT, 2023.04.15:
 * - 검사처리.
 */
	if (WARN_ONCE(sd_flags & ~TOPOLOGY_SD_FLAGS,
			"wrong sd_flags in topology description\n"))
		sd_flags &= TOPOLOGY_SD_FLAGS;

	*sd = (struct sched_domain){
		.min_interval		= sd_weight,
		.max_interval		= 2*sd_weight,
		.busy_factor		= 16,
		.imbalance_pct		= 117,

		.cache_nice_tries	= 0,

		.flags			= 1*SD_BALANCE_NEWIDLE
					| 1*SD_BALANCE_EXEC
					| 1*SD_BALANCE_FORK
					| 0*SD_BALANCE_WAKE
					| 1*SD_WAKE_AFFINE
					| 0*SD_SHARE_CPUCAPACITY
					| 0*SD_SHARE_PKG_RESOURCES
					| 0*SD_SERIALIZE
					| 1*SD_PREFER_SIBLING
					| 0*SD_NUMA
					| sd_flags
					,

		.last_balance		= jiffies,
		.balance_interval	= sd_weight,
		.max_newidle_lb_cost	= 0,
		.next_decay_max_lb_cost	= jiffies,
		.child			= child,
#ifdef CONFIG_SCHED_DEBUG
		.name			= tl->name,
#endif
	};

	sd_span = sched_domain_span(sd);
	cpumask_and(sd_span, cpu_map, tl->mask(cpu));
	sd_id = cpumask_first(sd_span);

/*
 * IAMROOT, 2023.04.22:
 * - sd_span의 asym flag를 추가한다.
 */
	sd->flags |= asym_cpu_capacity_classify(sd_span, cpu_map);

	WARN_ONCE((sd->flags & (SD_SHARE_CPUCAPACITY | SD_ASYM_CPUCAPACITY)) ==
		  (SD_SHARE_CPUCAPACITY | SD_ASYM_CPUCAPACITY),
		  "CPU capacity asymmetry not supported on SMT\n");

	/*
	 * Convert topological properties into behaviour.
	 */
	/* Don't attempt to spread across CPUs of different capacities. */
/*
 * IAMROOT, 2023.04.22:
 * - asym이면 성능이 다른 cpu가 여러개있는것인데, 이 경우 sibling prefer를 사용하지 못한다.
 */
	if ((sd->flags & SD_ASYM_CPUCAPACITY) && sd->child)
		sd->child->flags &= ~SD_PREFER_SIBLING;

/*
 * IAMROOT, 2023.04.22:
 * - smt(issue pipeline 공유)인 경우 조금 낮춰서 load balance를 좀 더 우호적으로 동작하도록 한다.
 */
	if (sd->flags & SD_SHARE_CPUCAPACITY) {
		sd->imbalance_pct = 110;

/*
 * IAMROOT, 2023.04.22:
 * - smt(L1 공유), MC(L2 or L3를 공유하는 경우). smt에 비해 좀더 높이고, cache 공유이므로 관련 값을 설정한다.
 */
	} else if (sd->flags & SD_SHARE_PKG_RESOURCES) {
		sd->imbalance_pct = 117;
		sd->cache_nice_tries = 1;

#ifdef CONFIG_NUMA
/*
 * IAMROOT, 2023.04.22:
 * - numa인 경우 성능이 다른 cpu이므로 위 code에서 처럼 sibling perfer를 제거한다.
 */
	} else if (sd->flags & SD_NUMA) {
		sd->cache_nice_tries = 2;

		sd->flags &= ~SD_PREFER_SIBLING;
		sd->flags |= SD_SERIALIZE;

/*
 * IAMROOT, 2023.04.22:
 * - reclaim distance보다 큰 경우, 즉 멀리 떨어진 node에서 task를 실행 / fork / wake affine등을
 *   안하도록 한다.
 */
		if (sched_domains_numa_distance[tl->numa_level] > node_reclaim_distance) {
			sd->flags &= ~(SD_BALANCE_EXEC |
				       SD_BALANCE_FORK |
				       SD_WAKE_AFFINE);
		}

#endif
	} else {

/*
 * IAMROOT, 2023.04.22:
 * - DIE
 */
		sd->cache_nice_tries = 1;
	}

	/*
	 * For all levels sharing cache; connect a sched_domain_shared
	 * instance.
	 */
/*
 * IAMROOT, 2023.04.22:
 * - cache 공유가 있다면 shared에 sdd->sds를 등록한다.
 */
	if (sd->flags & SD_SHARE_PKG_RESOURCES) {
		sd->shared = *per_cpu_ptr(sdd->sds, sd_id);
		atomic_inc(&sd->shared->ref);
		atomic_set(&sd->shared->nr_busy_cpus, sd_weight);
	}

	sd->private = sdd;

	return sd;
}

/*
 * Topology list, bottom-up.
 */
static struct sched_domain_topology_level default_topology[] = {
#ifdef CONFIG_SCHED_SMT
	{ cpu_smt_mask, cpu_smt_flags, SD_INIT_NAME(SMT) },
#endif
#ifdef CONFIG_SCHED_MC
	{ cpu_coregroup_mask, cpu_core_flags, SD_INIT_NAME(MC) },
#endif
	{ cpu_cpu_mask, SD_INIT_NAME(DIE) },
	{ NULL, },
};

static struct sched_domain_topology_level *sched_domain_topology =
	default_topology;

/*
 * IAMROOT, 2023.04.15:
 * - schedule domain topology level만큼을 iterate한다.
 * - sched_domain_topology_level 참고
 */
#define for_each_sd_topology(tl)			\
	for (tl = sched_domain_topology; tl->mask; tl++)

void set_sched_topology(struct sched_domain_topology_level *tl)
{
	if (WARN_ON_ONCE(sched_smp_initialized))
		return;

	sched_domain_topology = tl;
}

#ifdef CONFIG_NUMA

/*
 * IAMROOT, 2023.04.15:
 * - @sched_domains_curr_level를 기준으로 @cpu가 속한 node에 대한 
 *   sched_domains_numa_masks를 return한다.
 *
 * - ex)   A, B, C, D는 node 이름.
 
 *         A-- 20 -- B
 *         |         |
 *        15        20
 *         |         |
 *         C-- 20 -- D
 *
 *   
 *   1. 요청한 cpu가 node A에 있고,
 *      sched_domains_curr_level = 1(1 == 15라는 의미)인 경우.
 *      node A, node C에 포함된 cpu들(cpumask) return.
 *
 *   2. 요청한 cpu가 node C에 있고,
 *      sched_domains_curr_level = 2(2 == 20라는 의미)인 경우.
 *      node A, node C, node D에 포함된 cpu들(cpumask) return.
 *
 *   3. 요청한 cpu가 node D에 있고,
 *      sched_domains_curr_level = 2(2 == 20라는 의미)인 경우.
 *      node C, node D, node B에 포함된 cpu들(cpumask) return.
 */
static const struct cpumask *sd_numa_mask(int cpu)
{
	return sched_domains_numa_masks[sched_domains_curr_level][cpu_to_node(cpu)];
}

static void sched_numa_warn(const char *str)
{
	static int done = false;
	int i,j;

	if (done)
		return;

	done = true;

	printk(KERN_WARNING "ERROR: %s\n\n", str);

	for (i = 0; i < nr_node_ids; i++) {
		printk(KERN_WARNING "  ");
		for (j = 0; j < nr_node_ids; j++)
			printk(KERN_CONT "%02d ", node_distance(i,j));
		printk(KERN_CONT "\n");
	}
	printk(KERN_WARNING "\n");
}

/*
 * IAMROOT, 2023.07.12:
 * - @distance와 일치하는 numa가 있으면 return true.
 */
bool find_numa_distance(int distance)
{
	int i;

	if (distance == node_distance(0, 0))
		return true;

	for (i = 0; i < sched_domains_numa_levels; i++) {
		if (sched_domains_numa_distance[i] == distance)
			return true;
	}

	return false;
}

/*
 * A system can have three types of NUMA topology:
 * NUMA_DIRECT: all nodes are directly connected, or not a NUMA system
 * NUMA_GLUELESS_MESH: some nodes reachable through intermediary nodes
 * NUMA_BACKPLANE: nodes can reach other nodes through a backplane
 *
 * The difference between a glueless mesh topology and a backplane
 * topology lies in whether communication between not directly
 * connected nodes goes through intermediary nodes (where programs
 * could run), or through backplane controllers. This affects
 * placement of programs.
 *
 * The type of topology can be discerned with the following tests:
 * - If the maximum distance between any nodes is 1 hop, the system
 *   is directly connected.
 * - If for two nodes A and B, located N > 1 hops away from each other,
 *   there is an intermediary node C, which is < N hops away from both
 *   nodes A and B, the system is a glueless mesh.
 */
/*
 * IAMROOT. 2023.04.15:
 * - google-translate
 * 시스템은 세 가지 유형의 NUMA 토폴로지를 가질 수 있습니다.
 * NUMA_DIRECT: 모든 노드가 직접 연결되거나 NUMA 시스템이 아님
 * NUMA_GLUELESS_MESH: 중간 노드를 통해 도달 가능한 일부 노드
 * NUMA_BACKPLANE: 노드가 백플레인을 통해 다른 노드에 도달 가능
 *
 * 백플레인 토폴로지는 직접 연결되지 않은 노드 간의 통신이 중간
 * 노드(프로그램이 실행될 수 있는 곳) 또는 백플레인 컨트롤러를 통해 이루어지는지에
 * 달려 있습니다. 이는 프로그램 배치에 영향을 미칩니다.
 *
 * 다음 테스트를 통해 토폴로지 유형을 식별할 수 있습니다.
 * - 노드 간 최대 거리가 1hops이면 시스템이 직접 연결된 것입니다.
 * - 두 개의 노드 A와 B에 대해 N > 1 hops 떨어져 있는 경우 중간 노드 C가
 *   있고 노드 A와 B 모두에서 < N hops 떨어져 있는 경우 시스템은 글루리스 메시입니다.
 */
/*
 * IAMROOT, 2023.04.15:
 * - 모든 노드간 연결이 1hop 에 갈 수 있으면 NUMA_DIRECT 로 설정
 *   모든 노드간 연결이 2hop 이내에 갈 수 있으면 NUMA_GLUELESS_MESH 로 설정
 *   그외 모든 노드간 연결이 2hop 초과이면 NUMA_BACKPLANE 로 설정
 */
static void init_numa_topology_type(void)
{
	int a, b, c, n;

	n = sched_max_numa_distance;

	if (sched_domains_numa_levels <= 2) {
		sched_numa_topology_type = NUMA_DIRECT;
		return;
	}

	for_each_online_node(a) {
		for_each_online_node(b) {
			/* Find two nodes furthest removed from each other. */
			if (node_distance(a, b) < n)
				continue;

			/* Is there an intermediary node between a and b? */
			for_each_online_node(c) {
				if (node_distance(a, c) < n &&
				    node_distance(b, c) < n) {
					sched_numa_topology_type =
							NUMA_GLUELESS_MESH;
					return;
				}
			}

			sched_numa_topology_type = NUMA_BACKPLANE;
			return;
		}
	}
}


#define NR_DISTANCE_VALUES (1 << DISTANCE_BITS)

/*
 * IAMROOT, 2023.04.30:
 * - 1. sched_domains_numa_distance[] 에 node 의 모든 distance 값 설정
 *   2. 해당 distance level 에서 node id 별로 접근할 수 있는 cpumask
 *      (sched_domains_numa_masks) 설정. 이후 sd의 span cpumask 로 사용된다
 *   3. sched_domain_topology_level 구조체 초기화
 *   4. numa_levels, max_numa_distance, topology type, numa_onlinenodes 설정
 */
void sched_init_numa(void)
{
	struct sched_domain_topology_level *tl;
	unsigned long *distance_map;
	int nr_levels = 0;
	int i, j;

	/*
	 * O(nr_nodes^2) deduplicating selection sort -- in order to find the
	 * unique distances in the node_distance() table.
	 */
	distance_map = bitmap_alloc(NR_DISTANCE_VALUES, GFP_KERNEL);
	if (!distance_map)
		return;

	bitmap_zero(distance_map, NR_DISTANCE_VALUES);
	/*
	 * IAMROOT, 2023.04.08:
	 * - ex.
	 *   hip07-d05.dts
	 *   distance-map {
	 *	compatible = "numa-distance-map-v1";
	 *	distance-matrix = <0 0 10>,
	 *			  <0 1 15>,
	 *			  <0 2 20>,
	 *			  <0 3 25>,
	 *			  <1 0 15>,
	 *			  <1 1 10>,
	 *			  <1 2 25>,
	 *    		          <1 3 30>,
	 *		          <2 0 20>,
	 *		          <2 1 25>,
	 *		          <2 2 10>,
	 *		          <2 3 15>,
	 *		          <3 0 25>,
	 *		          <3 1 30>,
	 *		          <3 2 15>,
	 *		          <3 3 10>;
	 *   };
	 *   위의 경우 10, 15, 20, 25, 30 이 bitmap에 설정될 것이다.
	 *   distantmap = 0b..._0100_0010_0001_0000_1000_0100_0000_0000
	 */
	for (i = 0; i < nr_node_ids; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			int distance = node_distance(i, j);

			if (distance < LOCAL_DISTANCE || distance >= NR_DISTANCE_VALUES) {
				sched_numa_warn("Invalid distance value range");
				return;
			}

			bitmap_set(distance_map, distance, 1);
		}
	}
	/*
	 * We can now figure out how many unique distance values there are and
	 * allocate memory accordingly.
	 */
	/*
	 * IAMROOT. 2023.04.15:
	 * - google-translate
	 * 이제 얼마나 많은 고유한 거리 값이 있는지 파악하고 그에 따라 메모리를 할당할 수
	 * 있습니다.
	 * - 위 dts 예에서는 nr_levels 이 5로 설정된다.
	 */
	nr_levels = bitmap_weight(distance_map, NR_DISTANCE_VALUES);

	sched_domains_numa_distance = kcalloc(nr_levels, sizeof(int), GFP_KERNEL);
	if (!sched_domains_numa_distance) {
		bitmap_free(distance_map);
		return;
	}

	/*
	 * IAMROOT, 2023.04.15:
	 * - ex. sched_domains_numa_distance = {10, 15, 20, 25, 30} 으로 설정됨.
	 */
	for (i = 0, j = 0; i < nr_levels; i++, j++) {
		j = find_next_bit(distance_map, NR_DISTANCE_VALUES, j);
		sched_domains_numa_distance[i] = j;
	}

	bitmap_free(distance_map);

	/*
	 * 'nr_levels' contains the number of unique distances
	 *
	 * The sched_domains_numa_distance[] array includes the actual distance
	 * numbers.
	 */
	/*
	 * IAMROOT. 2023.04.15:
	 * - google-translate
	 * 'nr_levels'에는 고유한 거리의 수가 포함됩니다. sched_domains_numa_distance[]
	 * 배열에는 실제 거리 수가 포함됩니다.
	 */

	/*
	 * Here, we should temporarily reset sched_domains_numa_levels to 0.
	 * If it fails to allocate memory for array sched_domains_numa_masks[][],
	 * the array will contain less then 'nr_levels' members. This could be
	 * dangerous when we use it to iterate array sched_domains_numa_masks[][]
	 * in other functions.
	 *
	 * We reset it to 'nr_levels' at the end of this function.
	 */
	/*
	 * IAMROOT. 2023.04.15:
	 * - google-translate
	 * 여기에서 일시적으로 sched_domains_numa_levels를 0으로 재설정해야 합니다. 배열
	 * sched_domains_numa_masks[][]에 대한 메모리 할당에 실패하면 배열에 'nr_levels'
	 * 구성원보다 적게 포함됩니다. 이것은 다른 함수에서 배열
	 * sched_domains_numa_masks[][]를 반복하는 데 사용할 때 위험할 수 있습니다. 이
	 * 함수의 끝에서 'nr_levels'로 재설정합니다.
	 */
	sched_domains_numa_levels = 0;

	sched_domains_numa_masks = kzalloc(sizeof(void *) * nr_levels, GFP_KERNEL);
	if (!sched_domains_numa_masks)
		return;

	/*
	 * Now for each level, construct a mask per node which contains all
	 * CPUs of nodes that are that many hops away from us.
	 */
	/*
	 * IAMROOT. 2023.04.15:
	 * - google-translate
	 * 이제 각 레벨에 대해 우리로부터 그만큼 많은 hops 거리에 있는 노드의 모든 CPU를
	 * 포함하는 노드당 마스크를 구성합니다.
	 */
	for (i = 0; i < nr_levels; i++) {
		sched_domains_numa_masks[i] =
			kzalloc(nr_node_ids * sizeof(void *), GFP_KERNEL);
		if (!sched_domains_numa_masks[i])
			return;

		for (j = 0; j < nr_node_ids; j++) {
			struct cpumask *mask = kzalloc(cpumask_size(), GFP_KERNEL);
			int k;

			if (!mask)
				return;

			/*
			 * IAMROOT, 2023.04.15:
			 * - [numa_distance_level][node_id] 형식의 이차원 배열
			 */
			sched_domains_numa_masks[i][j] = mask;

			/*
			 * IAMROOT, 2023.04.15:
			 * - N_POSSIBLE 노드 만큼 루프
			 */
			for_each_node(k) {
				/*
				 * Distance information can be unreliable for
				 * offline nodes, defer building the node
				 * masks to its bringup.
				 * This relies on all unique distance values
				 * still being visible at init time.
				 */
				/*
				 * IAMROOT. 2023.04.15:
				 * - google-translate
				 * 거리 정보는 오프라인 노드에 대해 신뢰할 수 없을 수
				 * 있으므로 노드 마스크 작성을 가져오기까지 연기하십시오.
				 * 이것은 초기화 시점에 여전히 보이는 모든 고유한 거리
				 * 값에 의존합니다.
				 */
				if (!node_online(j))
					continue;

				if (sched_debug() && (node_distance(j, k) != node_distance(k, j)))
					sched_numa_warn("Node-distance not symmetric");

				if (node_distance(j, k) > sched_domains_numa_distance[i])
					continue;

				/*
				 * IAMROOT, 2023.04.15:
				 * - j node에서 distance level i에 해당하는
				 *   distance 값이하 node들의 cpumask 합.
				 *   아래 그림및 주석 참조.
				 */
				cpumask_or(mask, mask, cpumask_of_node(k));
			}
		}
	}

	/*
	 * IAMROOT, 2023.04.15:
	 * - [0]----15----[1]
	 *    |            |
	 *    |   \    /   |
	 *   20     25     30
	 *    |   /    \   |
	 *    |            |
	 *   [2]----15----[3]
	 *
	 *   static struct cpumask ***sched_domains_numa_masks; 는
	 *   sched_domains_numa_masks[numa_distance_level][node_id] 형식이다.
	 *   cpumask_of_node(0) 에서 가져오는 cpumask를 m0,
	 *   cpumask_of_node(1) 에서 가져오는 cpumask를 m1 등으로 가정하고
	 *   sched_domains_numa_masks 변수의 설정을 예상해본다
	 *
	 *   *sched_domains_numa_masks[0][0] = *m0          distance 10, node 0
	 *                            [0][1] = *m1          distance 10, node 1
	 *                            [0][2] = *m2          distance 10, node 2
	 *                            [0][3] = *m3          distance 10, node 3
	 *                            [1][0] = *m0|*m1      distance 15, node 0
	 *                            [1][1] = *m0|*m1      distance 15, node 1
	 *                            [1][2] = *m2|*m3      distance 15, node 2
	 *                            [1][3] = *m2|*m3      distance 15, node 3
	 *                            [2][0] = *m0|*m1|*m2  distance 20, node 0
	 *                            [2][1] = *m0|*m1      distance 20, node 1
	 *                            [2][2] = *m0|*m2|*m3  distance 20, node 2
	 *                            [2][3] = *m2|*m3      distance 20, node 3
	 *                            ...
	 *
	 *   각각의 노드에 설정된 cpumask 가 아래와 같다고 가정하면
	 *   node                cpumask
	 *   0                   0b..._0000_0000_0000_1111
	 *   1                   0b..._0000_0000_1111_0000
	 *   2                   0b..._0000_1111_0000_0000
	 *   3                   0b..._1111_0000_0000_0000
	 *   sched_domains_numa_masks 배열에 설정되는 값들은 다음과 같을 것이다.
	 *   *sched_domains_numa_masks[0][0] =  0b..._0000_0000_0000_1111
	 *   *sched_domains_numa_masks[0][1] =  0b..._0000_0000_1111_0000
	 *   *sched_domains_numa_masks[0][2] =  0b..._0000_1111_0000_0000
	 *   *sched_domains_numa_masks[0][3] =  0b..._1111_0000_0000_0000
	 *   *sched_domains_numa_masks[1][0] =  0b..._0000_0000_1111_1111
	 *   *sched_domains_numa_masks[1][1] =  0b..._0000_0000_1111_1111
	 *   *sched_domains_numa_masks[1][2] =  0b..._1111_1111_0000_0000
	 *   *sched_domains_numa_masks[1][3] =  0b..._1111_1111_0000_0000
	 *   *sched_domains_numa_masks[2][0] =  0b..._0000_1111_1111_1111
	 *   *sched_domains_numa_masks[2][1] =  0b..._0000_0000_1111_1111
	 *   *sched_domains_numa_masks[2][2] =  0b..._1111_1111_0000_1111
	 *   *sched_domains_numa_masks[2][3] =  0b..._1111_1111_0000_0000
	 *                            ...
	 */

	/* Compute default topology size */
	/*
	 * IAMROOT, 2023.04.15:
	 * - 기본은 SMT, MC, DIE 가 있고 config 설정에 따라 i값이 달라짐. 최대 3
	 */
	for (i = 0; sched_domain_topology[i].mask; i++);

	/*
	 * IAMROOT, 2023.04.15:
	 * - ex. i는 기본 3, nr_leves 은 dts 예로 5, + 1(null) = 9
	 *       sched_domain_topology_level 9개 만듦
	 */
	tl = kzalloc((i + nr_levels + 1) *
			sizeof(struct sched_domain_topology_level), GFP_KERNEL);
	if (!tl)
		return;

	/*
	 * Copy the default topology bits..
	 */
	/*
	 * IAMROOT, 2023.04.15:
	 * - 생성한 sched_domain_topology_level 에 default_topology 복사
	 */
	for (i = 0; sched_domain_topology[i].mask; i++)
		tl[i] = sched_domain_topology[i];

	/*
	 * Add the NUMA identity distance, aka single NODE.
	 */
	/*
	 * IAMROOT. 2023.04.15:
	 * - google-translate
	 * NUMA 식별 거리(일명 단일 NODE)를 추가합니다.
	 */
	tl[i++] = (struct sched_domain_topology_level){
		.mask = sd_numa_mask,
		.numa_level = 0,
		SD_INIT_NAME(NODE)
	};

	/*
	 * .. and append 'j' levels of NUMA goodness.
	 */
	for (j = 1; j < nr_levels; i++, j++) {
		tl[i] = (struct sched_domain_topology_level){
			.mask = sd_numa_mask,
			.sd_flags = cpu_numa_flags,
			.flags = SDTL_OVERLAP,
			.numa_level = j,
			SD_INIT_NAME(NUMA)
		};
	}

	/*
	 * IAMROOT, 2023.04.15:
	 * - default_topology 에서 설정된 tl로 변경
	 */
	sched_domain_topology = tl;

	/*
	 * IAMROOT, 2023.04.15:
	 * - ex. 위 dts예를 들면 sched_domains_numa_levels = 5
	 *   sched_max_numa_distance = 30
	 *   sched_numa_topology_type = NUMA_DIRECT
	 */
	sched_domains_numa_levels = nr_levels;
	sched_max_numa_distance = sched_domains_numa_distance[nr_levels - 1];

	init_numa_topology_type();

	sched_numa_onlined_nodes = bitmap_alloc(nr_node_ids, GFP_KERNEL);
	if (!sched_numa_onlined_nodes)
		return;

	bitmap_zero(sched_numa_onlined_nodes, nr_node_ids);
	for_each_online_node(i)
		bitmap_set(sched_numa_onlined_nodes, i, 1);
}

static void __sched_domains_numa_masks_set(unsigned int node)
{
	int i, j;

	/*
	 * NUMA masks are not built for offline nodes in sched_init_numa().
	 * Thus, when a CPU of a never-onlined-before node gets plugged in,
	 * adding that new CPU to the right NUMA masks is not sufficient: the
	 * masks of that CPU's node must also be updated.
	 */
	if (test_bit(node, sched_numa_onlined_nodes))
		return;

	bitmap_set(sched_numa_onlined_nodes, node, 1);

	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			if (!node_online(j) || node == j)
				continue;

			if (node_distance(j, node) > sched_domains_numa_distance[i])
				continue;

			/* Add remote nodes in our masks */
			cpumask_or(sched_domains_numa_masks[i][node],
				   sched_domains_numa_masks[i][node],
				   sched_domains_numa_masks[0][j]);
		}
	}

	/*
	 * A new node has been brought up, potentially changing the topology
	 * classification.
	 *
	 * Note that this is racy vs any use of sched_numa_topology_type :/
	 */
	init_numa_topology_type();
}

void sched_domains_numa_masks_set(unsigned int cpu)
{
	int node = cpu_to_node(cpu);
	int i, j;

	__sched_domains_numa_masks_set(node);

	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			if (!node_online(j))
				continue;

			/* Set ourselves in the remote node's masks */
			if (node_distance(j, node) <= sched_domains_numa_distance[i])
				cpumask_set_cpu(cpu, sched_domains_numa_masks[i][j]);
		}
	}
}

void sched_domains_numa_masks_clear(unsigned int cpu)
{
	int i, j;

	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++)
			cpumask_clear_cpu(cpu, sched_domains_numa_masks[i][j]);
	}
}

/*
 * sched_numa_find_closest() - given the NUMA topology, find the cpu
 *                             closest to @cpu from @cpumask.
 * cpumask: cpumask to find a cpu from
 * cpu: cpu to be close to
 *
 * returns: cpu, or nr_cpu_ids when nothing found.
 */
int sched_numa_find_closest(const struct cpumask *cpus, int cpu)
{
	int i, j = cpu_to_node(cpu);

	for (i = 0; i < sched_domains_numa_levels; i++) {
		cpu = cpumask_any_and(cpus, sched_domains_numa_masks[i][j]);
		if (cpu < nr_cpu_ids)
			return cpu;
	}
	return nr_cpu_ids;
}

#endif /* CONFIG_NUMA */

/*
 * IAMROOT, 2023.04.15:
 * - sdt level만큼의 pcpu를 생성 및 초기화한다.
 *   1. sdt 관련 구조체 포인터에 대한 pcpu 할당
 *   2. sdt 관련 구조체 메모리를 할당하고 pcpu에 연결
 */
static int __sdt_alloc(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;


	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

/*
 * IAMROOT, 2023.04.15:
 * - pcpu를 할당한다.
 */
		sdd->sd = alloc_percpu(struct sched_domain *);
		if (!sdd->sd)
			return -ENOMEM;

		sdd->sds = alloc_percpu(struct sched_domain_shared *);
		if (!sdd->sds)
			return -ENOMEM;

		sdd->sg = alloc_percpu(struct sched_group *);
		if (!sdd->sg)
			return -ENOMEM;

		sdd->sgc = alloc_percpu(struct sched_group_capacity *);
		if (!sdd->sgc)
			return -ENOMEM;


/*
 * IAMROOT, 2023.04.15:
 * - 만든 pcpu에 대해서 초기화를 수행한다.
 */
		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;
			struct sched_domain_shared *sds;
			struct sched_group *sg;
			struct sched_group_capacity *sgc;

			sd = kzalloc_node(sizeof(struct sched_domain) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sd)
				return -ENOMEM;

			*per_cpu_ptr(sdd->sd, j) = sd;

			sds = kzalloc_node(sizeof(struct sched_domain_shared),
					GFP_KERNEL, cpu_to_node(j));
			if (!sds)
				return -ENOMEM;

			*per_cpu_ptr(sdd->sds, j) = sds;

			sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sg)
				return -ENOMEM;

			sg->next = sg;

			*per_cpu_ptr(sdd->sg, j) = sg;

			sgc = kzalloc_node(sizeof(struct sched_group_capacity) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sgc)
				return -ENOMEM;

#ifdef CONFIG_SCHED_DEBUG
			sgc->id = j;
#endif

			*per_cpu_ptr(sdd->sgc, j) = sgc;
		}
	}

	return 0;
}

/*
 * IAMROOT, 2023.04.30:
 * - 1. sdt 빌드시 임시로 사용한 pcpu 이중 포인터(sd_data 멤버들(sd, sds, sg, sgc))를
 *     모두 free
 *   2. sds, sg, sgc 에 연결된 ref가 0인 구조체 메모리도 모두 해제한다.
 * - claim_allocations 함수에서 free 하지 않을 구조체 메모리의 pcpu는 NULL로 설정함
 * - sd 의 경우는 전부가 NULL로 설정되었다.
 * - 최종. sd 구조체 전부와 ref 가 있는 sds, sg, sgc 구조체 메모리만 남겨진다.
 */
static void __sdt_free(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;

	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

		/*
		 * IAMROOT, 2023.04.29:
		 * - claim_allocations 함수에서 NULL로 설정되지 않은 것들 free.
		 *   실제로 pcpu 이중 포인터에 연결된 구조체 메모리를 해제한다
		 */
		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;

			if (sdd->sd) {
				sd = *per_cpu_ptr(sdd->sd, j);
				if (sd && (sd->flags & SD_OVERLAP))
					free_sched_groups(sd->groups, 0);
				kfree(*per_cpu_ptr(sdd->sd, j));
			}

			if (sdd->sds)
				kfree(*per_cpu_ptr(sdd->sds, j));
			if (sdd->sg)
				kfree(*per_cpu_ptr(sdd->sg, j));
			if (sdd->sgc)
				kfree(*per_cpu_ptr(sdd->sgc, j));
		}
		/*
		 * IAMROOT, 2023.04.29:
		 * - sd_data 멤버 pcpu 포인터 변수 4개 free
		 *   위에서 pcpu에 연결된 구조체 메모리를 해제 했고 여기서는
		 *   pcpu를 free 한다.
		 */
		free_percpu(sdd->sd);
		sdd->sd = NULL;
		free_percpu(sdd->sds);
		sdd->sds = NULL;
		free_percpu(sdd->sg);
		sdd->sg = NULL;
		free_percpu(sdd->sgc);
		sdd->sgc = NULL;
	}
}

/*
 * IAMROOT, 2023.04.15:
 * - @child   DIE의 child는 mc, MC의 child는 smt의 개념
 * - @tl, @cpu_map, @child, @cpu에 따른 @sd를 생성하고, @child를 등록한다.
 * - @attr에 따라 SD_BALANCE_WAKE, SD_BALANCE_NEWIDLE flag 삭제 여부를 결정한다.
 */
static struct sched_domain *build_sched_domain(struct sched_domain_topology_level *tl,
		const struct cpumask *cpu_map, struct sched_domain_attr *attr,
		struct sched_domain *child, int cpu)
{
	struct sched_domain *sd = sd_init(tl, cpu_map, child, cpu);

/*
 * IAMROOT, 2023.04.22:
 * - @child(이전 domain. 현재 mc였다면 child는 smt)가 있었다면 child에 만들어진 @sd를
 *   등록한다.
 */
	if (child) {
/*
 * IAMROOT, 2023.04.22:
 * - child보다는 1이 높은 level로 설정하고, max값을 업데이트한다.
 */
		sd->level = child->level + 1;
		sched_domain_level_max = max(sched_domain_level_max, sd->level);
		child->parent = sd;

/*
 * IAMROOT, 2023.04.22:
 * - child는 반드시 parent에 속하는 개념이 되야된다. 안되면 버그다. 예외처리를 한다.
 */
		if (!cpumask_subset(sched_domain_span(child),
				    sched_domain_span(sd))) {
			pr_err("BUG: arch topology borken\n");
#ifdef CONFIG_SCHED_DEBUG
			pr_err("     the %s domain not a subset of the %s domain\n",
					child->name, sd->name);
#endif
			/* Fixup, ensure @sd has at least @child CPUs. */
			cpumask_or(sched_domain_span(sd),
				   sched_domain_span(sd),
				   sched_domain_span(child));
		}

	}

/*
 * IAMROOT, 2023.04.22:
 * - relax_domain_level보다 높은 @sd에 대해서 SD_BALANCE_WAKE, SD_BALANCE_NEWIDLE flag 삭제한다.
 */
	set_domain_attribute(sd, attr);

	return sd;
}

/*
 * Ensure topology masks are sane, i.e. there are no conflicts (overlaps) for
 * any two given CPUs at this (non-NUMA) topology level.
 */
/*
 * IAMROOT, 2023.04.15:
 * - sanity check. 설정이 잘못됬는지 확인한다. overlap이 없는 level인데 overlap이
 *   된것을 확인한다.
 */
static bool topology_span_sane(struct sched_domain_topology_level *tl,
			      const struct cpumask *cpu_map, int cpu)
{
	int i;

	/* NUMA levels are allowed to overlap */
	if (tl->flags & SDTL_OVERLAP)
		return true;

	/*
	 * Non-NUMA levels cannot partially overlap - they must be either
	 * completely equal or completely disjoint. Otherwise we can end up
	 * breaking the sched_group lists - i.e. a later get_group() pass
	 * breaks the linking done for an earlier span.
	 */
/*
 * IAMROOT, 2023.04.15:
 * - papago
 *   NUMA가 아닌 수준은 부분적으로 겹칠 수 없습니다. 완전히 같거나 
 *   완전히 분리되어야 합니다. 그렇지 않으면 sched_group 목록이 깨질 수 
 *   있습니다. 즉, 나중에 get_group() 패스가 이전 범위에 대해 수행된 
 *   연결을 깨뜨립니다.
 */
	for_each_cpu(i, cpu_map) {
		if (i == cpu)
			continue;
		/*
		 * We should 'and' all those masks with 'cpu_map' to exactly
		 * match the topology we're about to build, but that can only
		 * remove CPUs, which only lessens our ability to detect
		 * overlaps
		 */
/*
 * IAMROOT, 2023.04.15:
 * - papago
 *   우리가 만들려는 토폴로지와 정확히 일치하도록 'cpu_map'을 사용하여 
 *   모든 마스크를 '및' 해야 하지만 이렇게 하면 CPU만 제거할 수 있으므로 
 *   겹침을 감지하는 능력이 줄어들 뿐입니다.
 * - 겹치면 안된다.
 */
		if (!cpumask_equal(tl->mask(cpu), tl->mask(i)) &&
		    cpumask_intersects(tl->mask(cpu), tl->mask(i)))
			return false;
	}

	return true;
}

/*
 * Build sched domains for a given set of CPUs and attach the sched domains
 * to the individual CPUs
 */
/*
 * IAMROOT. 2023.05.29:
 * - google-translate
 * 주어진 CPU 집합에 대한 sched 도메인을 구축하고 sched 도메인을 개별 CPU에 연결
 *
 * IAMROOT, 2023.04.15:
 * - 1. sd build 및 초기화 (span(cpumask), flag, vars etc)
 *   2. sg build - child domain 의 cpumask 값으로 설정. balance group 설정
 *      SD_OVERLAP(numa) flag 여부에 따라 구분하여 빌드한다.
 *   3. group capacity 설정
 *   4. rq 에 rootdomain 과 최하위 sd 연결
 *   5. 임시 pcpu 와 ref 없는 구조체 메모리 삭제
 */
static int
build_sched_domains(const struct cpumask *cpu_map, struct sched_domain_attr *attr)
{
	enum s_alloc alloc_state = sa_none;
	struct sched_domain *sd;
	struct s_data d;
	struct rq *rq = NULL;
	int i, ret = -ENOMEM;
	bool has_asym = false;

	if (WARN_ON(cpumask_empty(cpu_map)))
		goto error;

	alloc_state = __visit_domain_allocation_hell(&d, cpu_map);
	if (alloc_state != sa_rootdomain)
		goto error;

	/* Set up domains for CPUs specified by the cpu_map: */
/*
 * IAMROOT, 2023.04.22:
 * - pcpu별로 tl을 순회한다.
 */
	for_each_cpu(i, cpu_map) {
		struct sched_domain_topology_level *tl;

		sd = NULL;

/*
 * IAMROOT, 2023.04.22:
 * - tl을 순회하면서 schedule domain을 만든다.
 */
		for_each_sd_topology(tl) {

			if (WARN_ON(!topology_span_sane(tl, cpu_map, i)))
				goto error;

/*
 * IAMROOT, 2023.04.22:
 * - 최하단(smt)는 child가 NULL로 들어가고, 이후 mc는 smt의 sd가 child, die는 mc의 sd가
 *   child로 들어가는 개념으로 동작한다.
 */
			sd = build_sched_domain(tl, cpu_map, attr, sd, i);

/*
 * IAMROOT, 2023.04.22:
 * - ASYM여부를 저장. 
 */
			has_asym |= sd->flags & SD_ASYM_CPUCAPACITY;

/*
 * IAMROOT, 2023.04.22:
 * - 첫 level은 pcpu에 저장한다.
 */
			if (tl == sched_domain_topology)
				*per_cpu_ptr(d.sd, i) = sd;

/*
 * IAMROOT, 2023.04.22:
 * - tl이 SDTL_OVERLAP인 경우 sd flags에도 overlap을 달아준다.
 */
			if (tl->flags & SDTL_OVERLAP)
				sd->flags |= SD_OVERLAP;

/*
 * IAMROOT, 2023.04.22:
 * - level이 cpu_map까지가 범위이므로 범위에 도달하면 break한다.
 */
			if (cpumask_equal(cpu_map, sched_domain_span(sd)))
				break;
		}
	}

	/* Build the groups for the domains */
	for_each_cpu(i, cpu_map) {
		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent) {
			sd->span_weight = cpumask_weight(sched_domain_span(sd));
/*
 * IAMROOT, 2023.04.22:
 * - overlap이 있는 domain, 아닌 domain에 따라 scheduling group을 build한다.
 */
			if (sd->flags & SD_OVERLAP) {
				if (build_overlap_sched_groups(sd, i))
					goto error;
			} else {
				if (build_sched_groups(sd, i))
					goto error;
			}
		}
	}

	/* Calculate CPU capacity for physical packages and nodes */
	for (i = nr_cpumask_bits-1; i >= 0; i--) {
		if (!cpumask_test_cpu(i, cpu_map))
			continue;

/*
 * IAMROOT, 2023.04.22:
 * - 최하단 domain부터 시작하여 parent로 올라가면서 정리한다. 
 *   삭제를 안할 pointer에 NULL을 넣는 작업 및 sg의 capacity를 update한다.
 */
		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent) {
			claim_allocations(i, sd);
			init_sched_groups_capacity(i, sd);
		}
	}

	/* Attach the domains */
	rcu_read_lock();
	for_each_cpu(i, cpu_map) {
		rq = cpu_rq(i);
		sd = *per_cpu_ptr(d.sd, i);

		/* Use READ_ONCE()/WRITE_ONCE() to avoid load/store tearing: */
/*
 * IAMROOT, 2023.04.22:
 * - root domain의 max보다 방금 설정된 cpu성능값이 클경우 root domain의 max값을 고친다.
 */
		if (rq->cpu_capacity_orig > READ_ONCE(d.rd->max_cpu_capacity))
			WRITE_ONCE(d.rd->max_cpu_capacity, rq->cpu_capacity_orig);

		cpu_attach_domain(sd, d.rd, i);
	}
	rcu_read_unlock();

/*
 * IAMROOT, 2023.04.22:
 * - 한번이라도 ASYM cpucapacity가 있었다면
 */
	if (has_asym)
		static_branch_inc_cpuslocked(&sched_asym_cpucapacity);

	if (rq && sched_debug_verbose) {
		pr_info("root domain span: %*pbl (max cpu_capacity = %lu)\n",
			cpumask_pr_args(cpu_map), rq->rd->max_cpu_capacity);
	}

	ret = 0;
error:
	__free_domain_allocs(&d, alloc_state, cpu_map);

	return ret;
}

/* Current sched domains: */
static cpumask_var_t			*doms_cur;

/* Number of sched domains in 'doms_cur': */
static int				ndoms_cur;

/* Attributes of custom domains in 'doms_cur' */
static struct sched_domain_attr		*dattr_cur;

/*
 * Special case: If a kmalloc() of a doms_cur partition (array of
 * cpumask) fails, then fallback to a single sched domain,
 * as determined by the single cpumask fallback_doms.
 */
static cpumask_var_t			fallback_doms;

/*
 * arch_update_cpu_topology lets virtualized architectures update the
 * CPU core maps. It is supposed to return 1 if the topology changed
 * or 0 if it stayed the same.
 */
int __weak arch_update_cpu_topology(void)
{
	return 0;
}

/*
 * IAMROOT, 2023.04.15:
 * - @ndoms개수만큼을 만들고, 해당 ndoms에 cpumask를 생성한다.
 */
cpumask_var_t *alloc_sched_domains(unsigned int ndoms)
{
	int i;
	cpumask_var_t *doms;

	doms = kmalloc_array(ndoms, sizeof(*doms), GFP_KERNEL);
	if (!doms)
		return NULL;
	for (i = 0; i < ndoms; i++) {
		if (!alloc_cpumask_var(&doms[i], GFP_KERNEL)) {
			free_sched_domains(doms, i);
			return NULL;
		}
	}
	return doms;
}

void free_sched_domains(cpumask_var_t doms[], unsigned int ndoms)
{
	unsigned int i;
	for (i = 0; i < ndoms; i++)
		free_cpumask_var(doms[i]);
	kfree(doms);
}

/*
 * Set up scheduler domains and groups.  For now this just excludes isolated
 * CPUs, but could be used to exclude other special cases in the future.
 */
/*
 * IAMROOT. 2023.04.15:
 * - google-translate
 * 스케줄러 도메인 및 그룹을 설정합니다. 지금은 격리된 CPU만 제외하지만 향후 다른
 * 특별한 경우를 제외하는 데 사용할 수 있습니다.
 */
int sched_init_domains(const struct cpumask *cpu_map)
{
	int err;

	zalloc_cpumask_var(&sched_domains_tmpmask, GFP_KERNEL);
	zalloc_cpumask_var(&sched_domains_tmpmask2, GFP_KERNEL);
	zalloc_cpumask_var(&fallback_doms, GFP_KERNEL);

	/*
	 * IAMROOT, 2023.04.15:
	 * - arm64에서는 아무것도 안한다.
	 */
	arch_update_cpu_topology();

/*
 * IAMROOT, 2023.04.15:
 * - asym cpu에 대한 자료구조 설정
 */
	asym_cpu_capacity_scan();
	ndoms_cur = 1;

/*
 * IAMROOT, 2023.04.15:
 * - 일단 1개를 만든다. 실패할경우 fallback_doms를 사용한다.
 */
	doms_cur = alloc_sched_domains(ndoms_cur);
	if (!doms_cur)
		doms_cur = &fallback_doms;

/*
 * IAMROOT, 2023.04.15:
 * - @cpu_map & housekeeping를 대상으로 build를 진행한다.
 */
	cpumask_and(doms_cur[0], cpu_map, housekeeping_cpumask(HK_FLAG_DOMAIN));
	err = build_sched_domains(doms_cur[0], NULL);

	return err;
}

/*
 * Detach sched domains from a group of CPUs specified in cpu_map
 * These CPUs will now be attached to the NULL domain
 */
/*
 * IAMROOT. 2023.05.29:
 * - google-translate
 * cpu_map에 지정된 CPU 그룹에서 sched 도메인을 분리합니다. 이 CPU는 이제 NULL
 * 도메인에 연결됩니다.
 */
static void detach_destroy_domains(const struct cpumask *cpu_map)
{
	unsigned int cpu = cpumask_any(cpu_map);
	int i;

	if (rcu_access_pointer(per_cpu(sd_asym_cpucapacity, cpu)))
		static_branch_dec_cpuslocked(&sched_asym_cpucapacity);

	rcu_read_lock();
	for_each_cpu(i, cpu_map)
		cpu_attach_domain(NULL, &def_root_domain, i);
	rcu_read_unlock();
}

/* handle null as "default" */
static int dattrs_equal(struct sched_domain_attr *cur, int idx_cur,
			struct sched_domain_attr *new, int idx_new)
{
	struct sched_domain_attr tmp;

	/* Fast path: */
	if (!new && !cur)
		return 1;

	tmp = SD_ATTR_INIT;

	return !memcmp(cur ? (cur + idx_cur) : &tmp,
			new ? (new + idx_new) : &tmp,
			sizeof(struct sched_domain_attr));
}

/*
 * Partition sched domains as specified by the 'ndoms_new'
 * cpumasks in the array doms_new[] of cpumasks. This compares
 * doms_new[] to the current sched domain partitioning, doms_cur[].
 * It destroys each deleted domain and builds each new domain.
 *
 * 'doms_new' is an array of cpumask_var_t's of length 'ndoms_new'.
 * The masks don't intersect (don't overlap.) We should setup one
 * sched domain for each mask. CPUs not in any of the cpumasks will
 * not be load balanced. If the same cpumask appears both in the
 * current 'doms_cur' domains and in the new 'doms_new', we can leave
 * it as it is.
 *
 * The passed in 'doms_new' should be allocated using
 * alloc_sched_domains.  This routine takes ownership of it and will
 * free_sched_domains it when done with it. If the caller failed the
 * alloc call, then it can pass in doms_new == NULL && ndoms_new == 1,
 * and partition_sched_domains() will fallback to the single partition
 * 'fallback_doms', it also forces the domains to be rebuilt.
 *
 * If doms_new == NULL it will be replaced with cpu_online_mask.
 * ndoms_new == 0 is a special case for destroying existing domains,
 * and it will not create the default domain.
 *
 * Call with hotplug lock and sched_domains_mutex held
 */
/*
 * IAMROOT. 2023.05.20:
 * - google-translate
 * cpumasks의 doms_new[] 배열에서 'ndoms_new' cpumasks로 지정된 대로 sched 도메인을
 * 분할합니다. 이는 doms_new[]를 현재 sched 도메인 분할인 doms_cur[]와
 * 비교합니다. 삭제된 각 도메인을 파괴하고 각각의 새 도메인을
 * 구축합니다.
 *
 * 'doms_new'는 길이가 'ndoms_new'인 cpumask_var_t의
 * 배열입니다. 마스크는 교차하지 않습니다(중첩하지 마십시오). 각 마스크에 대해
 * 하나의 sched 도메인을 설정해야 합니다. 어떤 cpumask에도 없는 CPU는 로드
 * 밸런싱되지 않습니다. 현재 'doms_cur' 도메인과 새 'doms_new' 도메인 모두에 동일한
 * cpumask가 나타나면 그대로 둘 수 있습니다.
 *
 * 전달된 'doms_new'는
 * alloc_sched_domains를 사용하여 할당되어야 합니다. 이 루틴은 소유권을 가지며
 * 작업이 완료되면 free_sched_domains합니다. 호출자가 alloc 호출에 실패하면
 * doms_new == NULL && ndoms_new == 1을 전달할 수 있으며
 * partition_sched_domains()는 단일 파티션 'fallback_doms'로 폴백하고 도메인을
 * 강제로 다시 빌드합니다.
 *
 * doms_new == NULL이면 cpu_online_mask로
 * 대체됩니다. ndoms_new == 0은 기존 도메인을 파기하는 특수한 경우이며 기본
 * 도메인을 생성하지 않습니다.
 *
 * 핫플러그 잠금 및 sched_domains_mutex 보류로 호출
 *
 * - 호출과정 예
 * 1. cpufreq notifier 로 부터 호출되는 경우
 * partition_sched_domains_locked
 * ▶ partition_and_rebuild_sched_domains (cpuset.c:1006)
 *   ▶ rebuild_sched_domains_locked (cpuset.c:1026)
 *     ▶ rebuild_sched_domains (cpuset.c:1083)
 *       ╸update_topology_flags_workfn (arch_topology.c:401)
 * workfn 등록 과 호출은 아래과 같다
 * - workq callback 함수 등록
 *   DECLARE_WORK(update_topology_flags_work, update_topology_flags_workfn)
 * - cpufreq notifier 등록
 *   core_initcall(register_cpufreq_notifier) -> cpufreq_register_notifier
 *   (&init_cpu_capacity_notifier, CPUFREQ_POLICY_NOTIFIER)
 * - init_cpu_capacity_notifier 에서 init_cpu_capacity_callback 함수 설정.
 * - cpufreq policy 가 변경되면 notifier에 의해 callback 함수 호출
 *   init_cpu_capacity_callback -> schedule_work(&update_topology_flags_work)
 *
 * 2. cpu hotplug state 로 부터 호출되는 경우
 * partition_sched_domains_locked
 * ▶ partition_sched_domains (topology.c:3779)
 *   ▶ cpuset_cpu_active (core.c:10449)
 *      ╸sched_cpu_activate (core.c:10484)
 * - cpu hotplug state 의 startup 함수로 호출된다.
 *	[CPUHP_AP_ACTIVE] = {
 *		.name			= "sched:active",
 *		.startup.single		= sched_cpu_activate,
 *		.teardown.single	= sched_cpu_deactivate,
 *	},
 *
 * - 조건이 되면 ndoms_new 만큼 새 pd를 build 하고 doms_new등을 doms_cur 등의
 *   전역 변수에 설정한다.
 */
void partition_sched_domains_locked(int ndoms_new, cpumask_var_t doms_new[],
				    struct sched_domain_attr *dattr_new)
{
	bool __maybe_unused has_eas = false;
	int i, j, n;
	int new_topology;

	lockdep_assert_held(&sched_domains_mutex);

	/* Let the architecture update CPU core mappings: */
	/*
	 * IAMROOT. 2023.05.21:
	 * - google-translate
	 * 아키텍처가 CPU 코어 매핑을 업데이트하도록 합니다.
	 * - cpufreq notifier 로 부터 호출된 경우(즉 cpufreq policy가 변경된 경우)만
	 *   new_topology 가 1로 설정된다.
	 */
	new_topology = arch_update_cpu_topology();
	/* Trigger rebuilding CPU capacity asymmetry data */
	/*
	 * IAMROOT. 2023.05.29:
	 * - google-translate
	 * CPU 용량 비대칭 데이터 재구성 트리거
	 *
	 * - cpufreq policy가 변경되었다면 asym_cap_list(cpu capa list) 재설정
	 */
	if (new_topology)
		asym_cpu_capacity_scan();

	if (!doms_new) {
		WARN_ON_ONCE(dattr_new);
		n = 0;
		doms_new = alloc_sched_domains(1);
		if (doms_new) {
			n = 1;
			cpumask_and(doms_new[0], cpu_active_mask,
				    housekeeping_cpumask(HK_FLAG_DOMAIN));
		}
	} else {
		n = ndoms_new;
	}

	/* Destroy deleted domains: */
	/*
	 * IAMROOT, 2023.05.29:
	 * - google-translate
	 * 삭제된 도메인 폐기:
	 *
	 * - 1. cpufreq policy가 변경된 경우
	 *      기존 도메인 모두 제거
	 *   2. cpufreq policy가 변경되지 않은경우
	 *      1. cpumask 와 속성이 일치
	 *         dl_bw->total_bw = 0
	 *      2. cpumask 나 속성이 일치하지 않는 경우
	 *         기존 도메인 제거
	 */
	for (i = 0; i < ndoms_cur; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_cur[i], doms_new[j]) &&
			    dattrs_equal(dattr_cur, i, dattr_new, j)) {
				struct root_domain *rd;

				/*
				 * This domain won't be destroyed and as such
				 * its dl_bw->total_bw needs to be cleared.  It
				 * will be recomputed in function
				 * update_tasks_root_domain().
				 */
				/*
				 * IAMROOT. 2023.05.20:
				 * - google-translate
				 * 이 도메인은 삭제되지 않으므로 해당
				 * dl_bw->total_bw를 지워야 합니다.
				 * update_tasks_root_domain() 함수에서
				 * 다시 계산됩니다.
				 */
				rd = cpu_rq(cpumask_any(doms_cur[i]))->rd;
				dl_clear_root_domain(rd);
				goto match1;
			}
		}
		/* No match - a current sched domain not in new doms_new[] */
		detach_destroy_domains(doms_cur[i]);
match1:
		;
	}

	n = ndoms_cur;
	/*
	 * IAMROOT, 2023.05.20:
	 * - 메모리 할당에 실패하여 doms_new가 NULL인 경우
	 */
	if (!doms_new) {
		n = 0;
		doms_new = &fallback_doms;
		cpumask_and(doms_new[0], cpu_active_mask,
			    housekeeping_cpumask(HK_FLAG_DOMAIN));
	}

	/* Build new domains: */
	/*
	 * IAMROOT, 2023.05.29:
	 * - google-translate
	 * 새 도메인 구축:
	 *
	 * - 1. cpufreq policy가 변경되었다.
	 *      ndoms_new 갯수만큼 build_sched_domains 호출
	 * - 2. cpufreq policy가 변경되지 않았다.
	 *      1. cpumask와 속성이 같다.
	 *         아무것도 하지 않음
	 *      2. cpumask나 속성이 같지 않다
	 *         build_sched_domains 호출
	 */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_new[i], doms_cur[j]) &&
			    dattrs_equal(dattr_new, i, dattr_cur, j))
				goto match2;
		}
		/* No match - add a new doms_new */
		build_sched_domains(doms_new[i], dattr_new ? dattr_new + i : NULL);
match2:
		;
	}

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
	/* Build perf. domains: */
	/*
	 * - google-translate
	 * 빌드 perf domain:
	 *
	 * IAMROOT, 2023.05.20:
	 * - pd 구성이 변경된 경우에만 build 한다.
	 * - 1. sched_energy_update 가 true
	 *      build_perf_domains 호출
	 *   2. sched_energy_update 가 false
	 *      1. 기존 도메인들중 하나와 cpumask 가 같고 그 기존 도메인 cpumask 의
	 *         첫번째 cpu의 pd가 존재하는 new 도메인
	 *         - has_eas = true로 설정
	 *      2. 위가 아닌 new 도메인
	 *         build_perf_domains 호출
	 */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < n && !sched_energy_update; j++) {
			/*
			 * IAMROOT, 2023.05.20:
			 * - cpumask  같고 pd(perf domain)의 첫번째 cpu일 때만
			 *   pd를 만들지 않는다.
			 */
			if (cpumask_equal(doms_new[i], doms_cur[j]) &&
			    cpu_rq(cpumask_first(doms_cur[j]))->rd->pd) {
				has_eas = true;
				goto match3;
			}
		}
		/* No match - add perf. domains for a new rd */
		has_eas |= build_perf_domains(doms_new[i]);
match3:
		;
	}
	sched_energy_set(has_eas);
#endif

	/* Remember the new sched domains: */
	/*
	 * IAMROOT, 2023.05.20:
	 * - 기존 도메인과 속성을 지운다.
	 */
	if (doms_cur != &fallback_doms)
		free_sched_domains(doms_cur, ndoms_cur);

	kfree(dattr_cur);
	doms_cur = doms_new;
	dattr_cur = dattr_new;
	ndoms_cur = ndoms_new;

	update_sched_domain_debugfs();
}

/*
 * Call with hotplug lock held
 */
void partition_sched_domains(int ndoms_new, cpumask_var_t doms_new[],
			     struct sched_domain_attr *dattr_new)
{
	mutex_lock(&sched_domains_mutex);
	partition_sched_domains_locked(ndoms_new, doms_new, dattr_new);
	mutex_unlock(&sched_domains_mutex);
}
