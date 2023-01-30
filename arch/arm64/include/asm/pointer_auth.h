/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_POINTER_AUTH_H
#define __ASM_POINTER_AUTH_H

#include <linux/bitops.h>
#include <linux/prctl.h>
#include <linux/random.h>

#include <asm/cpufeature.h>
#include <asm/memory.h>
#include <asm/sysreg.h>

#define PR_PAC_ENABLED_KEYS_MASK                                               \
	(PR_PAC_APIAKEY | PR_PAC_APIBKEY | PR_PAC_APDAKEY | PR_PAC_APDBKEY)

#ifdef CONFIG_ARM64_PTR_AUTH
/*
 * Each key is a 128-bit quantity which is split across a pair of 64-bit
 * registers (Lo and Hi).
 */
/*
 * IAMROOT, 2023.01.30:
 * - chat openai
 *   ptrauth는 iOS, macOS, watchOS 및 tvOS에서 포인터 인증을 제공하는 일련의 
 *   함수입니다. 포인터 인증은 메모리 포인터의 변조를 방지하고 시스템 보안을 
 *   강화하는 데 도움이 됩니다.
 *
 *   ptrauth_keys_install_user 기능은 시스템의 "사용자"용 키를 설치합니다. 
 *   ptrauth_key_install_nosync 함수는 지정된 포인터 인증 유형(APIB, APDA, 
 *   APDB, APGA)에 대해 지정된 인증 키를 설치하지만 프로세서 간에 강제로 
 *   동기화하지는 않습니다.
 *
 *   nosync 속성은 키 설치가 코드 실행 속도를 높일 수 있는 메모리 장벽을 
 *   생성하지 않음을 의미합니다. 그러나 이는 키가 모든 프로세서에 즉시 
 *   표시되지 않을 수 있으며 키 가시성을 보장하기 위해 일부 수동 동기화가 
 *   필요할 수 있음을 의미합니다.
 *
 *   인증 유형은 시스템에서 사용되는 포인터 인증 유형을 나타냅니다. 
 *   ptrauth 함수는 네 가지 다른 인증 유형을 사용합니다.
 *
 *   APIB: 코드 블록에 대한 포인터와 이를 호출한 명령을 인증합니다.
 *   APDA: 데이터 개체에 대한 포인터와 이를 참조한 명령을 인증합니다.
 *   APDB: 데이터 개체에 대한 포인터를 인증하지만 이를 참조한 명령은 인증하지 않음
 *   APGA: 일반 데이터 개체에 대한 포인터와 이를 참조한 명령을 인증합니다.
 *   이러한 인증 유형은 메모리 포인터의 변조를 방지하고 시스템의 보안을 강화하는 
 *   데 사용됩니다. 사용되는 특정 인증 유형은 제공되는 보안 수준과 시스템 
 *   성능에 영향을 줄 수 있습니다. 
 */
struct ptrauth_key {
	unsigned long lo, hi;
};

/*
 * We give each process its own keys, which are shared by all threads. The keys
 * are inherited upon fork(), and reinitialised upon exec*().
 */
struct ptrauth_keys_user {
	struct ptrauth_key apia;
	struct ptrauth_key apib;
	struct ptrauth_key apda;
	struct ptrauth_key apdb;
	struct ptrauth_key apga;
};

#define __ptrauth_key_install_nosync(k, v)			\
do {								\
	struct ptrauth_key __pki_v = (v);			\
	write_sysreg_s(__pki_v.lo, SYS_ ## k ## KEYLO_EL1);	\
	write_sysreg_s(__pki_v.hi, SYS_ ## k ## KEYHI_EL1);	\
} while (0)

#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL

struct ptrauth_keys_kernel {
	struct ptrauth_key apia;
};

static __always_inline void ptrauth_keys_init_kernel(struct ptrauth_keys_kernel *keys)
{
	if (system_supports_address_auth())
		get_random_bytes(&keys->apia, sizeof(keys->apia));
}

static __always_inline void ptrauth_keys_switch_kernel(struct ptrauth_keys_kernel *keys)
{
	if (!system_supports_address_auth())
		return;

	__ptrauth_key_install_nosync(APIA, keys->apia);
	isb();
}

#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */

static inline void ptrauth_keys_install_user(struct ptrauth_keys_user *keys)
{
	if (system_supports_address_auth()) {
		__ptrauth_key_install_nosync(APIB, keys->apib);
		__ptrauth_key_install_nosync(APDA, keys->apda);
		__ptrauth_key_install_nosync(APDB, keys->apdb);
	}

	if (system_supports_generic_auth())
		__ptrauth_key_install_nosync(APGA, keys->apga);
}

static inline void ptrauth_keys_init_user(struct ptrauth_keys_user *keys)
{
	if (system_supports_address_auth()) {
		get_random_bytes(&keys->apia, sizeof(keys->apia));
		get_random_bytes(&keys->apib, sizeof(keys->apib));
		get_random_bytes(&keys->apda, sizeof(keys->apda));
		get_random_bytes(&keys->apdb, sizeof(keys->apdb));
	}

	if (system_supports_generic_auth())
		get_random_bytes(&keys->apga, sizeof(keys->apga));

	ptrauth_keys_install_user(keys);
}

extern int ptrauth_prctl_reset_keys(struct task_struct *tsk, unsigned long arg);

extern int ptrauth_set_enabled_keys(struct task_struct *tsk, unsigned long keys,
				    unsigned long enabled);
extern int ptrauth_get_enabled_keys(struct task_struct *tsk);

static inline unsigned long ptrauth_strip_insn_pac(unsigned long ptr)
{
	return ptrauth_clear_pac(ptr);
}

static __always_inline void ptrauth_enable(void)
{
	if (!system_supports_address_auth())
		return;
	sysreg_clear_set(sctlr_el1, 0, (SCTLR_ELx_ENIA | SCTLR_ELx_ENIB |
					SCTLR_ELx_ENDA | SCTLR_ELx_ENDB));
	isb();
}

#define ptrauth_suspend_exit()                                                 \
	ptrauth_keys_install_user(&current->thread.keys_user)

#define ptrauth_thread_init_user()                                             \
	do {                                                                   \
		ptrauth_keys_init_user(&current->thread.keys_user);            \
									       \
		/* enable all keys */                                          \
		if (system_supports_address_auth())                            \
			ptrauth_set_enabled_keys(current,                      \
						 PR_PAC_ENABLED_KEYS_MASK,     \
						 PR_PAC_ENABLED_KEYS_MASK);    \
	} while (0)

/*
 * IAMROOT, 2023.01.28:
 * - PASS
 */
#define ptrauth_thread_switch_user(tsk)                                        \
	ptrauth_keys_install_user(&(tsk)->thread.keys_user)

#else /* CONFIG_ARM64_PTR_AUTH */
#define ptrauth_enable()
#define ptrauth_prctl_reset_keys(tsk, arg)	(-EINVAL)
#define ptrauth_set_enabled_keys(tsk, keys, enabled)	(-EINVAL)
#define ptrauth_get_enabled_keys(tsk)	(-EINVAL)
#define ptrauth_strip_insn_pac(lr)	(lr)
#define ptrauth_suspend_exit()
#define ptrauth_thread_init_user()
#define ptrauth_thread_switch_user(tsk)
#endif /* CONFIG_ARM64_PTR_AUTH */

#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL
#define ptrauth_thread_init_kernel(tsk)					\
	ptrauth_keys_init_kernel(&(tsk)->thread.keys_kernel)
#define ptrauth_thread_switch_kernel(tsk)				\
	ptrauth_keys_switch_kernel(&(tsk)->thread.keys_kernel)
#else
#define ptrauth_thread_init_kernel(tsk)
#define ptrauth_thread_switch_kernel(tsk)
#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */

#endif /* __ASM_POINTER_AUTH_H */
