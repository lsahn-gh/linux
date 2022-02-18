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
/* IAMROOT, 2021.07.17:
 * - 5.10 -> 5.15 변경점.
 *   arch/arm64/include/asm/alternative.h 에서 위치 변경
 *-------------------------
 * - c의 ALTERNATIVE의 assembly version.
 *   alternative가 일어나기전엔 codeX로 동작하다가
 *   alternative가 되면 codeA가 codeY로 변경된다.
 * - codeY는 subsecion이라는 codeX의 근처 section에 위치하고 있다가
 *   후에 alternative하는 code에 의해 대체되는 방식이다.
 * - cap에 따라서 alternative하는지를 판단한다.
 *-------------------------
 * - example( alternative_if를 다음과 같이 쓴다고 가정)
 *
 *.macro example_macro
 * alternative_if EXAMPLE_CAP
 *	codeA
 * alternative_else
 *  codeB
 * alternative_endif
 *
 * 매크로를 확장하면 다음과 같다
 * ----------
 *  1)
 * .set .Lasm_alt_mode, 1
 *	.pushsection .altinstructions, "a"
 *		altinstruction_entry 663f, 661f, \cap, 664f-663f, 662f-661f
 *	.popsection
 *  2)
 *	.subsection 1
 *	.align 2
 *	661:
 * ----------
 *
 *	codeA
 *
 * ----------
 * 662:
 * 3)
 *	.if .Lasm_alt_mode==0
 *		.subsection 1
 *	.else
 *		.previous
 *	.endif
 * 663:
 * ----------
 * 4)
 *
 * codeB
 *
 * ----------
 * 664:
 *	.org	. - (664b-663b) + (662b-661b)
 *	.org	. - (662b-661b) + (664b-663b)
 *	.if .Lasm_alt_mode==0
 *	.previous
 *	.endif
 * ----------
 *
 *	1) 일단 현재 위치의 codeA, codeB의 크기와 위치, capacity를
 *	altinstructions section에 저장한다. 후에 alternative를 하는 함수에서
 *	해당 section을 보고 대체할것이다.
 *	2) 2 ~3 까지. 즉 codeA를 일단 subsection으로 고려한다.
 *	3) .Lasm_alt_mode가 0이면 subseciont을 해당 위치로 갱신하고 하고 아니면
 *	3 전까지 subsection으로 쓴다는 얘기가 된다.
 *	1에서 Lasm_alt_mode가 1이였으므로 codeA위치는 subsection가 될것이다.
 *	4) codeB가 현재 위치에 들어가있을것이다.
 *
 * -----------------------------------
*  1) 
 * alternative_if EXAMPLE_CAP
 *	codeA
 * alternative_else
 *  codeB
 * alternative_endif
 *
 * codeB로 동작하다가 alternative에서 EXAMPLE_CAP이 지원되는 시스템인게
 * 확인되면 codeA로 동작할것이다.
 *
 * 2)
 * alternative_if_not EXAMPLE_CAP
 *	codeA
 * alternative_else
 *  codeB
 * alternative_endif
 *
 * codeA로 동작하다가 alternative에서 EXAMPLE_CAP이 지원되는 시스템인게
 * 확인되면 codeB로 동작할것이다.
 */
.macro alternative_if_not cap
	.set .Lasm_alt_mode, 0
	.pushsection .altinstructions, "a"
	altinstruction_entry 661f, 663f, \cap, 662f-661f, 664f-663f
	.popsection
661:
.endm

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
