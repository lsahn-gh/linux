// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Arm Limited
 */

#define pr_fmt(fmt) "smccc: " fmt

#include <linux/cache.h>
#include <linux/init.h>
#include <linux/arm-smccc.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <asm/archrandom.h>

/*
 * IAMROOT, 2021.12.18:
 * - psci 0.2이상인 경우 smccc version까지 획득해오는 루틴이 있으며
 *   해당 루틴에서 version, conduit, trng 지원여부를 설정한다.
 */
static u32 smccc_version = ARM_SMCCC_VERSION_1_0;
static enum arm_smccc_conduit smccc_conduit = SMCCC_CONDUIT_NONE;

bool __ro_after_init smccc_trng_available = false;
u64 __ro_after_init smccc_has_sve_hint = false;

/*
 * IAMROOT, 2021.12.18:
 * - smccc version, conduit, trng available등을 설정한다.
 */
void __init arm_smccc_version_init(u32 version, enum arm_smccc_conduit conduit)
{
	smccc_version = version;
	smccc_conduit = conduit;

	smccc_trng_available = smccc_probe_trng();
	if (IS_ENABLED(CONFIG_ARM64_SVE) &&
	    smccc_version >= ARM_SMCCC_VERSION_1_3)
		smccc_has_sve_hint = true;
}

enum arm_smccc_conduit arm_smccc_1_1_get_conduit(void)
{
	if (smccc_version < ARM_SMCCC_VERSION_1_1)
		return SMCCC_CONDUIT_NONE;

	return smccc_conduit;
}
EXPORT_SYMBOL_GPL(arm_smccc_1_1_get_conduit);

u32 arm_smccc_get_version(void)
{
	return smccc_version;
}
EXPORT_SYMBOL_GPL(arm_smccc_get_version);

static int __init smccc_devices_init(void)
{
	struct platform_device *pdev;

	if (smccc_trng_available) {
		pdev = platform_device_register_simple("smccc_trng", -1,
						       NULL, 0);
		if (IS_ERR(pdev))
			pr_err("smccc_trng: could not register device: %ld\n",
			       PTR_ERR(pdev));
	}

	return 0;
}
device_initcall(smccc_devices_init);
