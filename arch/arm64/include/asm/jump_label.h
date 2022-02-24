/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Huawei Ltd.
 * Author: Jiang Liu <liuj97@gmail.com>
 *
 * Based on arch/arm/include/asm/jump_label.h
 */
#ifndef __ASM_JUMP_LABEL_H
#define __ASM_JUMP_LABEL_H

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <asm/insn.h>

#define JUMP_LABEL_NOP_SIZE		AARCH64_INSN_SIZE

/*
 * IAMROOT, 2022.02.24:
 * - struct jump_entry의 형태로 저장한다.
 *
 * struct jump_entry {
 *		s32 code; // 1b - . => nop 까지의 offset
 *		s32 target; // l_yes - . => l_yes까지의 offset 
 *		long key;	// static_key - . => static_key 까지의 offset
 *	};
 *
 * - &((char *)key)[branch]
 *   struct4 jump_entry의 key member에 위 값을 넣는것을 볼수있다.
 *   결국엔 key의 주소를 그대로 넣냐 + 1을 해서의 넣냐의 차이가 된다.
 *   static_key is branch 이면 즉 0번 bit를 set하기 위한 용법이다.
 *   주소야 무조건 4byte정렬이 되있을테니 쓰지 않은 bit들을 마치 flag처럼
 *   사용한다. 실제 접근할때 flag bit들을 clear해서 사용한다.
 *
 * - key의 0번 bit
 *   jump_entry_is_branch를 살펴보면 0번 bit를 검사하는것으로 해당 static_key가
 *   true(branch)로 정의 됬는지, false(nop)로 정의됬는지 확인하는것이 보인다.
 */
static __always_inline bool arch_static_branch(struct static_key *key,
					       bool branch)
{
	asm_volatile_goto(
		"1:	nop					\n\t"
		 "	.pushsection	__jump_table, \"aw\"	\n\t"
		 "	.align		3			\n\t"
		 "	.long		1b - ., %l[l_yes] - .	\n\t"
		 "	.quad		%c0 - .			\n\t"
		 "	.popsection				\n\t"
		 :  :  "i"(&((char *)key)[branch]) :  : l_yes);

	return false;
l_yes:
	return true;
}

static __always_inline bool arch_static_branch_jump(struct static_key *key,
						    bool branch)
{
	asm_volatile_goto(
		"1:	b		%l[l_yes]		\n\t"
		 "	.pushsection	__jump_table, \"aw\"	\n\t"
		 "	.align		3			\n\t"
		 "	.long		1b - ., %l[l_yes] - .	\n\t"
		 "	.quad		%c0 - .			\n\t"
		 "	.popsection				\n\t"
		 :  :  "i"(&((char *)key)[branch]) :  : l_yes);

	return false;
l_yes:
	return true;
}

#endif  /* __ASSEMBLY__ */
#endif	/* __ASM_JUMP_LABEL_H */
