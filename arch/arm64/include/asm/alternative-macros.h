/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_ALTERNATIVE_MACROS_H
#define __ASM_ALTERNATIVE_MACROS_H

#include <asm/cpucaps.h>
#include <asm/insn-def.h>

#define ARM64_CB_PATCH ARM64_NCAPS

#ifndef __ASSEMBLY__

#include <linux/stringify.h>

/*
 * IAMROOT, 2021.09.11:
 * - struct alt_instr 의 형태로 저장한다.
 * - .word 661b - . : oldinstr 시작주소에서 이 위치까지의 offset(orig_offset)
 * - .word 663f - . : newinstr 시작주소에서 이 위치까지의 offset(alt_offset)
 * - .hword #feature : (cpufeature)
 * - .byte 662b-bb1b : oldinstr의 크기(orig_len)
 * - .byte 664f-663f : newinstr의 크기(alt_len)
 *
 * - 5.10 -> 5.15 변경점.
 *   arch/arm64/include/asm/alternative.h 에서 위치 변경
 */
#define ALTINSTR_ENTRY(feature)					              \
	" .word 661b - .\n"				/* label           */ \
	" .word 663f - .\n"				/* new instruction */ \
	" .hword " __stringify(feature) "\n"		/* feature bit     */ \
	" .byte 662b-661b\n"				/* source len      */ \
	" .byte 664f-663f\n"				/* replacement len */

/*
 * IAMROOT, 2022.02.17:
 * - alternative때 cb를 호출하게 한다.
 */
#define ALTINSTR_ENTRY_CB(feature, cb)					      \
	" .word 661b - .\n"				/* label           */ \
	" .word " __stringify(cb) "- .\n"		/* callback */	      \
	" .hword " __stringify(feature) "\n"		/* feature bit     */ \
	" .byte 662b-661b\n"				/* source len      */ \
	" .byte 664f-663f\n"				/* replacement len */


/*
 * IAMROOT, 2021.09.11:
 * - newinstr과 oldinstr의 크기가 같지 않으면 error
 * - booting할때는 old명령으로 실행되며 부팅이 완료된 후 조건에 따라서
 *   (cpu가 feature를 가지고있는지, kernel option지원 여부) old를 쓸지
 *   new를 쓸지를 정해서 replace를 한다.
 * - subsecion 1 을 쓰는 이유
 *   Git blame을 참고. 원래는 altinstructions_replacement를 사용했는데
 *   매우 큰 kernel에서 문제가 생겨 현재 사용하는 section근처에 생성되는
 *   subsection을 사용하는걸로 바꿈
 *
 * - 5.10 -> 5.15 변경점.
 *   arch/arm64/include/asm/alternative.h 에서 위치 변경
 */
/*
 * alternative assembly primitive:
 *
 * If any of these .org directive fail, it means that insn1 and insn2
 * don't have the same length. This used to be written as
 *
 * .if ((664b-663b) != (662b-661b))
 * 	.error "Alternatives instruction length mismatch"
 * .endif
 *
 * but most assemblers die if insn1 or insn2 have a .inst. This should
 * be fixed in a binutils release posterior to 2.25.51.0.2 (anything
 * containing commit 4e4d08cf7399b606 or c1baaddf8861).
 *
 * Alternatives with callbacks do not generate replacement instructions.
 */
#define __ALTERNATIVE_CFG(oldinstr, newinstr, feature, cfg_enabled)	\
	".if "__stringify(cfg_enabled)" == 1\n"				\
	"661:\n\t"							\
	oldinstr "\n"							\
	"662:\n"							\
	".pushsection .altinstructions,\"a\"\n"				\
	ALTINSTR_ENTRY(feature)						\
	".popsection\n"							\
	".subsection 1\n"						\
	"663:\n\t"							\
	newinstr "\n"							\
	"664:\n\t"							\
	".org	. - (664b-663b) + (662b-661b)\n\t"			\
	".org	. - (662b-661b) + (664b-663b)\n\t"			\
	".previous\n"							\
	".endif\n"

#define __ALTERNATIVE_CFG_CB(oldinstr, feature, cfg_enabled, cb)	\
	".if "__stringify(cfg_enabled)" == 1\n"				\
	"661:\n\t"							\
	oldinstr "\n"							\
	"662:\n"							\
	".pushsection .altinstructions,\"a\"\n"				\
	ALTINSTR_ENTRY_CB(feature, cb)					\
	".popsection\n"							\
	"663:\n\t"							\
	"664:\n\t"							\
	".endif\n"

#define _ALTERNATIVE_CFG(oldinstr, newinstr, feature, cfg, ...)	\
	__ALTERNATIVE_CFG(oldinstr, newinstr, feature, IS_ENABLED(cfg))

#define ALTERNATIVE_CB(oldinstr, cb) \
	__ALTERNATIVE_CFG_CB(oldinstr, ARM64_CB_PATCH, 1, cb)
#else

#include <asm/assembler.h>

.macro altinstruction_entry orig_offset alt_offset feature orig_len alt_len
	.word \orig_offset - .
	.word \alt_offset - .
	.hword \feature
	.byte \orig_len
	.byte \alt_len
.endm

.macro alternative_insn insn1, insn2, cap, enable = 1
	.if \enable
661:	\insn1
662:	.pushsection .altinstructions, "a"
	altinstruction_entry 661b, 663f, \cap, 662b-661b, 664f-663f
	.popsection
	.subsection 1
663:	\insn2
664:	.org	. - (664b-663b) + (662b-661b)
	.org	. - (662b-661b) + (664b-663b)
	.previous
	.endif
.endm

/*
 * Alternative sequences
 *
 * The code for the case where the capability is not present will be
 * assembled and linked as normal. There are no restrictions on this
 * code.
 *
 * The code for the case where the capability is present will be
 * assembled into a special section to be used for dynamic patching.
 * Code for that case must:
 *
 * 1. Be exactly the same length (in bytes) as the default code
 *    sequence.
 *
 * 2. Not contain a branch target that is used outside of the
 *    alternative sequence it is defined in (branches into an
 *    alternative sequence are not fixed up).
 */

/*
 * Begin an alternative code sequence.
 */
/* IAMROOT, 2023.09.17:
 * - Kernel booting 및 runtime에 instruction을 swap 하기 위한 macro 정의.
 *   이러한 macro는 performance 또는 bug workaround 패치를 위해 사용된다.
 *   @cpucap에 사용되는 cap들은 kernel 빌드 타임에 script를 통해 정의되며
 *   정수로 매핑되어 있다.
 *
 *   예) ARM64_MISMATCHED_CACHE_TYPE
 *       kernel 빌드 타임에 생성되는 define이고 cpucaps.h 파일에 정의.
 *       define의 갯수에 따라 값이 바뀌긴 하지만 make defconfig을 사용하면
 *       정수 34로 매핑된다.
 *
 * - alternative framework 레퍼런스
 *   https://blogs.oracle.com/linux/post/exploring-arm64-runtime-patching-alternatives
 *   https://sourceware.org/binutils/docs-2.41/as.html
 *
 * - alternative_if:
 *   @cpucap이 지원되는 시스템이면 codeB(.text 0)로 동작하고
 *             지원되지 않는 시스템이면 codeA(.text 1)로 변경되어 동작한다.
 *
 *   alternative_if \cpucap
 *      codeA
 *   alternative_else
 *      codeB
 *   alternative_endif
 *
 * - alternative_if_not:
 *   @cpucap이 지원되지 않는 시스템이면 codeA(.text 0)를 실행하고
 *             지원되는 시스템이라면 codeB(.text 1)로 동작한다.
 *
 *   alternative_if_not \cpucap
 *      codeA
 *   alternative_else
 *      codeB
 *   alternative_endif
 *
 * ----------------------------------------------
 * - alternative_if_not 예제는 아래처럼 확장된다.
 *  # alternative_if_not cap 시작
 *      .set .Lasm_alt_mode, 0
 *      .pushsection .altinstructions, "a"
 *  # altinstruction_entry 시작 (661f, 663f, \cap, 662f-661f, 664f-663f)
 *      .word 661f - .
 *      .word 663f - .
 *      .hword \cap
 *      .byte \size of codeA
 *      .byte \size of codeB
 *  # altinstruction_entry 끝
 *      .popsection
 *  661:
 *  # alternative_if_not cap 끝
 *          // 현재 section == .text 0
 *      ...
 *      codeA
 *      ...
 *  # alternative_else 시작
 *  662:
 *      .if .Lasm_alt_mode==0
 *      .subsection 1
 *          // 현재 section == .text 1
 *      .else
 *      .previous
 *          // alternative_if 와 함께 쓰이며 이때는 .text 0으로 변경.
 *          // 현재 section == .text 0
 *      .endif
 *  663:
 *  # alternative_else 끝
 *      ...
 *      codeB
 *      ...
 *  # alternative_endif 시작
 *  664:
 *      .org	. - (664b-663b) + (662b-661b)
 *      .org	. - (662b-661b) + (664b-663b)
 *      .if .Lasm_alt_mode==0
 *      .previous
 *      .endif
 *  # alternative_endif 끝
 */
.macro alternative_if_not cap
	.set .Lasm_alt_mode, 0
	.pushsection .altinstructions, "a"
	altinstruction_entry 661f, 663f, \cap, 662f-661f, 664f-663f
	.popsection
661:
.endm

/* IAMROOT, 2023.09.17:
 * - *_else와 함께 쓰기 위해 미리 .text 1 subsection으로 변경한다.
 */
.macro alternative_if cap
	.set .Lasm_alt_mode, 1
	.pushsection .altinstructions, "a"
	altinstruction_entry 663f, 661f, \cap, 664f-663f, 662f-661f
	.popsection
	.subsection 1
	.align 2	/* So GAS knows label 661 is suitably aligned */
661:
.endm

.macro alternative_cb cb
	.set .Lasm_alt_mode, 0
	.pushsection .altinstructions, "a"
	altinstruction_entry 661f, \cb, ARM64_CB_PATCH, 662f-661f, 0
	.popsection
661:
.endm

/*
 * Provide the other half of the alternative code sequence.
 */
/* IAMROOT, 2023.09.17:
 * - .subsection @num: 현재 subsection을 @num로 변경한다.
 *                     예) 현재 .text 0 이라면 .subsection 1 호출시
 *                         .text 1로 변경된다.
 * - .previous: 직전에 참조한 section, subsection으로 변경한다.
 *              예) 현재 .text 1이고 이전에 .text 0에 있었다면 .previous 호출시
 *                  .text 0으로 변경된다.
 */
.macro alternative_else
662:
	.if .Lasm_alt_mode==0
	.subsection 1
	.else
	.previous
	.endif
663:
.endm

/*
 * Complete an alternative code sequence.
 */
.macro alternative_endif
664:
	.org	. - (664b-663b) + (662b-661b)
	.org	. - (662b-661b) + (664b-663b)
	.if .Lasm_alt_mode==0
	.previous
	.endif
.endm

/*
 * Callback-based alternative epilogue
 */
.macro alternative_cb_end
662:
.endm

/*
 * Provides a trivial alternative or default sequence consisting solely
 * of NOPs. The number of NOPs is chosen automatically to match the
 * previous case.
 */
.macro alternative_else_nop_endif
alternative_else
	nops	(662b-661b) / AARCH64_INSN_SIZE
alternative_endif
.endm

#define _ALTERNATIVE_CFG(insn1, insn2, cap, cfg, ...)	\
	alternative_insn insn1, insn2, cap, IS_ENABLED(cfg)

#endif  /*  __ASSEMBLY__  */

/*
 * Usage: asm(ALTERNATIVE(oldinstr, newinstr, feature));
 *
 * Usage: asm(ALTERNATIVE(oldinstr, newinstr, feature, CONFIG_FOO));
 * N.B. If CONFIG_FOO is specified, but not selected, the whole block
 *      will be omitted, including oldinstr.
 */
#define ALTERNATIVE(oldinstr, newinstr, ...)   \
	_ALTERNATIVE_CFG(oldinstr, newinstr, __VA_ARGS__, 1)

#endif /* __ASM_ALTERNATIVE_MACROS_H */
