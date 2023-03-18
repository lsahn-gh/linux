/*
 * governor.c - governor support
 *
 * (C) 2006-2007 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *               Shaohua Li <shaohua.li@intel.com>
 *               Adam Belay <abelay@novell.com>
 *
 * This code is licenced under the GPL.
 */

#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/pm_qos.h>

#include "cpuidle.h"

char param_governor[CPUIDLE_NAME_LEN];

LIST_HEAD(cpuidle_governors);

/*
 * IAMROOT, 2023.03.18:
 * --- chat openai ----
 * - governor
 *   CPUIdle 거버너는 전력 소비를 최적화하기 위해 CPU의 전원 상태 전환을 관리하는 
 *   Linux 커널의 소프트웨어 구성 요소입니다. 거버너는 현재 워크로드 및 시스템 활동에 
 *   따라 CPU를 저전력 상태로 전환할 시기를 결정합니다. 거버너는 CPU의 유휴 시간을 
 *   모니터링하고 유휴 시간이 특정 임계값에 도달하면 전환할 적절한 유휴 상태를 
 *   선택합니다. 사용 가능한 유휴 상태는 특정 프로세서 및 하드웨어 플랫폼에 따라 
 *   다르지만 일반적으로 C0(활성), C1(정지), C2(저전력 상태) 등과 같은 여러 수준의 
 *   유휴 상태를 포함합니다. 거버너는 또한 처리가 필요한 들어오는 작업이 있는 경우 
 *   저전력 상태에서 CPU를 깨우도록 결정할 수 있습니다. CPUIdle 거버너는 Linux 커널 
 *   전원 관리 프레임워크의 중요한 구성 요소로, 에너지 소비를 줄이고 모바일 장치 및 
 *   랩톱의 배터리 수명을 연장하는 데 도움이 됩니다. 
 * ---------------------
 *
 * - cpuidle governor
 *   cpuidle에서 사용하는 governor는 다음과 같습니다.
 *
 *   menu governor : 최소한의 전력 소비를 위해 가능한한 적은 C-state으로 
 *   CPU를 유지하고, 사용자 요청이 있을 때 빠르게 복귀합니다.
 *
 *   ladder governor : 시스템 부하에 따라서 다른 C-state으로 이동합니다. 
 *   부하가 낮을 때는 더 깊은 C-state으로 이동하여 전력 소비를 최소화하고, 부하가 
 *   높을 때는 더 얕은 C-state으로 이동하여 빠른 응답성을 유지합니다.
 *
 *   menu-ladder governor : menu와 ladder governor의 조합입니다. 일반적으로 
 *   ladder보다 더 나은 전력 절약을 제공합니다. 
 *
 *   power aware governor : Intel의 P-state 기능을 활용하여 CPU의 주파수와 전압을 
 *   동적으로 조절하면서, 전력 소비를 최소화합니다. 
 *
 *   conservative governor : CPU 사용률이 낮을 때는 더 깊은 C-state으로 이동하여
 *   전력 소비를 최소화하고, 사용률이 높을 때는 더 얕은 C-state으로 이동하여 빠른 
 *   응답성을 유지합니다.
 *
 *   performance governor : CPU를 항상 최대 주파수에서 동작하도록 유지합니다.
 *   이 governor는 전력 소비를 최소화하지 않으며, CPU 성능을 우선시하는 경우에
 *   사용됩니다.
 *
 *   ondemand governor : CPU 사용률에 따라 C-state을 선택합니다. 사용률이 높을 때는
 *   더 얕은 C-state으로 이동하여 빠른 응답성을 유지하고, 사용률이 낮을 때는 더 깊은
 *   C-state으로 이동하여 전력 소비를 최소화합니다.
 *
 *   따라서 cpuidle에서는 총 7가지의 governor가 있습니다.
 *
 * - sysfss
 *   1. 
 *   cat /sys/devices/system/cpu/cpuidle/available_governors 
 *   menu 
 *   cat /sys/devices/system/cpu/cpuidle/current_governors 
 *   menu
 *
 *   2.
 *   cat /sys/devices/system/cpu/cpuidle/available_governors 
 *   ladder menu teo haltpoll
 *   cat /sys/devices/system/cpu/cpuidle/current_governors 
 *   menu
 */
struct cpuidle_governor *cpuidle_curr_governor;
struct cpuidle_governor *cpuidle_prev_governor;

/**
 * cpuidle_find_governor - finds a governor of the specified name
 * @str: the name
 *
 * Must be called with cpuidle_lock acquired.
 */
struct cpuidle_governor *cpuidle_find_governor(const char *str)
{
	struct cpuidle_governor *gov;

	list_for_each_entry(gov, &cpuidle_governors, governor_list)
		if (!strncasecmp(str, gov->name, CPUIDLE_NAME_LEN))
			return gov;

	return NULL;
}

/**
 * cpuidle_switch_governor - changes the governor
 * @gov: the new target governor
 * Must be called with cpuidle_lock acquired.
 */
int cpuidle_switch_governor(struct cpuidle_governor *gov)
{
	struct cpuidle_device *dev;

	if (!gov)
		return -EINVAL;

	if (gov == cpuidle_curr_governor)
		return 0;

	cpuidle_uninstall_idle_handler();

	if (cpuidle_curr_governor) {
		list_for_each_entry(dev, &cpuidle_detected_devices, device_list)
			cpuidle_disable_device(dev);
	}

	cpuidle_curr_governor = gov;

	if (gov) {
		list_for_each_entry(dev, &cpuidle_detected_devices, device_list)
			cpuidle_enable_device(dev);
		cpuidle_install_idle_handler();
		printk(KERN_INFO "cpuidle: using governor %s\n", gov->name);
	}

	return 0;
}

/**
 * cpuidle_register_governor - registers a governor
 * @gov: the governor
 */
int cpuidle_register_governor(struct cpuidle_governor *gov)
{
	int ret = -EEXIST;

	if (!gov || !gov->select)
		return -EINVAL;

	if (cpuidle_disabled())
		return -ENODEV;

	mutex_lock(&cpuidle_lock);
	if (cpuidle_find_governor(gov->name) == NULL) {
		ret = 0;
		list_add_tail(&gov->governor_list, &cpuidle_governors);
		if (!cpuidle_curr_governor ||
		    !strncasecmp(param_governor, gov->name, CPUIDLE_NAME_LEN) ||
		    (cpuidle_curr_governor->rating < gov->rating &&
		     strncasecmp(param_governor, cpuidle_curr_governor->name,
				 CPUIDLE_NAME_LEN)))
			cpuidle_switch_governor(gov);
	}
	mutex_unlock(&cpuidle_lock);

	return ret;
}

/**
 * cpuidle_governor_latency_req - Compute a latency constraint for CPU
 * @cpu: Target CPU
 */
s64 cpuidle_governor_latency_req(unsigned int cpu)
{
	struct device *device = get_cpu_device(cpu);
	int device_req = dev_pm_qos_raw_resume_latency(device);
	int global_req = cpu_latency_qos_limit();

	if (device_req > global_req)
		device_req = global_req;

	return (s64)device_req * NSEC_PER_USEC;
}
