/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 ARM Ltd.
 */
#ifndef __ASM_VDSO_PROCESSOR_H
#define __ASM_VDSO_PROCESSOR_H

#ifndef __ASSEMBLY__

/* IAMROOT, 2021.09.25:
 * - yield instr은 arch에게 다음과 같은 hint를 준다.
 *   다른 thread(hyper-threading 구조)에게 pipeline의 실행 유닛을
 *   양보할 수 있다.
 *
 * - 현재까지 출시된 Arm64 cores는 single hyper-threading per core이므로
 *   nop instr와 거의 유사하다고 추측됨.
 *
 * - busy wait
 */
static inline void cpu_relax(void)
{
	asm volatile("yield" ::: "memory");
}

#endif /* __ASSEMBLY__ */

#endif /* __ASM_VDSO_PROCESSOR_H */
