/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level CPU initialisation
 * Based on arch/arm/kernel/head.S
 *
 * Copyright (C) 1994-2002 Russell King
 * Copyright (C) 2003-2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/pgtable.h>

#include <asm/asm_pointer_auth.h>
#include <asm/assembler.h>
#include <asm/boot.h>
#include <asm/bug.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/cache.h>
#include <asm/cputype.h>
#include <asm/el2_setup.h>
#include <asm/elf.h>
#include <asm/image.h>
#include <asm/kernel-pgtable.h>
#include <asm/kvm_arm.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/page.h>
#include <asm/scs.h>
#include <asm/smp.h>
#include <asm/sysreg.h>
#include <asm/thread_info.h>
#include <asm/virt.h>

#include "efi-header.S"

/* IAMROOT, 2021.07.10:
 * - 해당 코드는 bootloader에 의한 최소한의 초기화 작업이후의 코드임을 알린다.
 * - KERNEL_START --> 커널 시작 가상 주소(_text)
 * - 컴파일 타임에(페이지 프레임 크기와 페이지 테이블 사용 단계) 결정
 *   -> 0xFFFF_****_****_****
 *   -> KASLR 옵션을 사용하는 경우 실제 커널 가상 주소는 런타임에 변경된다.
 *
 * KERNEL_START가 __PHYS_OFFSET으로 alias된 이유는 현재 mmu가 켜져있지 않아
 * VA가 동작하지 않으며 대신 code readability를 높이기 위해 __PHYS_OFFSET란
 * 이름으로 정의해둔 것임.
 *
 * - 예) VA_BITS = 48 bits 라면?
 *      PAGE_OFFSET         = 0xffff_0000_0000_0000
 *      PAGE_END            = 0xffff_8000_0000_0000
 *      BPF_JIT_REGION_END  = 0xffff_8000_0800_0000 (SZ_128M)
 *      MODULES_END         = 0xffff_8000_1000_0000 (SZ_128M)
 *      MODULES_END == KIMAGE_VADDR == _text == KERNEL_START
 */
#define __PHYS_OFFSET	KERNEL_START

#if (PAGE_OFFSET & 0x1fffff) != 0
#error PAGE_OFFSET must be at least 2MB aligned
#endif

/* IAMROOT, 2021.07.10:
 * - DTB(Device Tree Blob)
 *   디바이스 트리가 컴파일되어 빅엔디언 형태의 바이너리이다.
 *   FDT(Flattened Device Tree)라고도 불리운다.
 *   커널이 동작하기 위한 디바이스의 물리주소가 기록되어 있음.
 *   서버나 pc는 ACPI table이 있기때문에 필요가 없음
 *   SoC 기반의(ARM, embeded) 제품들은 UEFI가 없기때문에 device tree가 필요함.
 *   최근에 UEFI BIOS를 가진 ARM server는 ACPI 와 DT를 둘다 사용가능하다.
 *
 * 아래 요구사항이 필요한 이유:
 * 1. mmu를 키려고 하기때문에 켜있으면 안됌.
 * 2. 기존에 mmu 없이 boot loader가 동작했기 때문에 꺼져 있는게 정상.
 *    (ARM 계열은 MMU가 꺼져 있으면 데이터 캐시를 킬수 없다.
 *    일부 SoC에서 MMU off 상태에서 명령 캐시는 on 가능하다고 함)
 * - mmu = off 이유
 *   Startup entry point에서 mmu가 on 이라면 CPU가 명령어를 수행할 때
 *   메모리 주소값을 가상 주소로 해석하여 MMU가 물리 주소로 변환함.
 *   매핑 테이블이 존재하지 않기 때문에 page fault가 발생함.
 * - d-cache = off 이유
 *   Startup 코드에서는 물리 주소에서 데이터를 직접 읽거나 써야하는데
 *   d-cache가 on 되어 있다면 캐시 메모리로 접근을 시도하게 되어
 *   의도치 않은 작업들이 수행됨.
 * reference
 * - https://www.programmersought.com/article/20796227528/
 *
 * bootloader가 DRAM의 위치와 관계없이 relocatable한 code로 만들어주는 부분.
 * 아래 문맥의 callee : kernel caller: bootloader
 */
/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * The requirements are:
 *   MMU = off, D-cache = off, I-cache = on or off,
 *   x0 = physical address to the FDT blob.
 *
 * This code is mostly position independent so you call this at
 * __pa(PAGE_OFFSET).
 *
 * Note that the callee-saved registers are used for storing variables
 * that are useful before the MMU is enabled. The allocations are described
 * in the entry routines.
 */
	__HEAD
	/*
	 * DO NOT MODIFY. Image header expected by Linux boot-loaders.
	 */
	efi_signature_nop			// special NOP to identity as PE/COFF executable
	b	primary_entry			// branch to kernel start, magic
	.quad	0				// Image load offset from start of RAM, little-endian
	le64sym	_kernel_size_le			// Effective size of kernel image, little-endian
	le64sym	_kernel_flags_le		// Informative flags, little-endian
	.quad	0				// reserved
	.quad	0				// reserved
	.quad	0				// reserved
	.ascii	ARM64_IMAGE_MAGIC		// Magic number
	.long	.Lpe_header_offset		// Offset to the PE header.

	__EFI_PE_HEADER

	__INIT

	/*
	 * The following callee saved general purpose registers are used on the
	 * primary lowlevel boot path:
	 *
	 *  Register   Scope                      Purpose
	 *  x21        primary_entry() .. start_kernel()        FDT pointer passed at boot in x0
	 *  x23        primary_entry() .. start_kernel()        physical misalignment/KASLR offset
	 *  x28        __create_page_tables()                   callee preserved temp register
	 *  x19/x20    __primary_switch()                       callee preserved temp registers
	 *  x24        __primary_switch() .. relocate_kernel()  current RELR displacement
	 */
/* IAMROOT, 2021.07.17:
 *
 * - SYM_CODE_START() 해석:
 *      .globl primary_entry;
 *      .align 2;
 *      primary_entry:
 *
 * - .globl 의미
 *   해당 label을 외부에서 볼 수 있도록 하는 지시자.
 *   소스 코드가 여러개로 분리되어 있다면 linker에서 해당 symbol을
 *   찾을 수 있도록 사용하는 지시자.
 *
 * - b와 bl의 차이
 *   b  : 분기 이후에 이전의 흐름으로 돌아가지 않고 계속 진행할 때 사용.
 *   bl : 분기 후 로직을 수행하고 분기 이전의 주소로 복귀하여 계속 수행하고자
 *        할때 사용함. (함수 호출 후 return 시)
 */
SYM_CODE_START(primary_entry)
	bl	preserve_boot_args
	bl	init_kernel_el			// w0=cpu_boot_mode

/* IAMROOT, 2021.08.14:
 * - 커널 phys offset을 읽어 2M 단위로 정렬하기 위한 mask 값을 구하고
 *   @x23에 저장한다.
 */
	adrp	x23, __PHYS_OFFSET
	and	x23, x23, MIN_KIMG_ALIGN - 1	// KASLR offset, defaults to 0
	bl	set_cpu_boot_mode_flag
	bl	__create_page_tables
	/*
	 * The following calls CPU setup code, see arch/arm64/mm/proc.S for
	 * details.
	 * On return, the CPU will be ready for the MMU to be turned on and
	 * the TCR will have been set.
	 */
	bl	__cpu_setup			// initialise processor
	b	__primary_switch
SYM_CODE_END(primary_entry)

/*
 * Preserve the arguments passed by the bootloader in x0 .. x3
 */
/*
 * IAMROOT, 2021.07.17:
 * - bootloader에서 x0, x1, x2, x3 reg로 boot arguments을 넘겨주는데 이 값들을
 *   boot_args 배열에 저장하는 작업을 수행한다.
 *
 * - SYM_CODE_START_LOCAL() 해석
 *      .align 2;
 *      preserve_boot_args:
 */
SYM_CODE_START_LOCAL(preserve_boot_args)
/*
 * IAMROOT, 2022.01.26:
 * - x0에 있는 FDT phys addr을 x21에 옮긴 후 boot_args주소를 x0에 저장하고
 *   stp 명령어를 통해서 x21, x1, x2, x3 값을 boot_args[0..3]에 저장한다.
 *   (primary_entry 주석에서 x21은 FDT pointer를 저장하기로 했었다.)
 *
 * - Arm64는 현재 kernel version에서 x1, x2, x3 인자는 안쓰니 0으로 초기화
 *   될 것이다. (setup_arch의 마지막 boot_args 부분 참고)
 */
	mov	x21, x0				// x21=FDT
/*
 * IAMROOT, 2023.09.07:
 * x0 = &boot_args[0]
 */
	adr_l	x0, boot_args			// record the contents of
/*
 * IAMROOT, 2021.07.17:
 * - stp Xt1, Xt2, [Xn] : @Xt1, @Xt2 reg에 저장된 값을 @Xn 주소에 저장하며
 *                        @Xt1은 [@Xn] + 0, @Xt1은 [@Xn] + 8로 접근함.
 *                        stp 명령어는 두개의 str 명령어를 수행한 것과 동일한
 *                        결과를 가짐.
 *
 * - @x21 = FDT, @x1, @x2, @x3에 저장된 값을 boot_args(@x0) 배열 0, 1, 2, 3에
 *   순서대로 저장함.
 *
 * str x21, boot_args[0] (boot_args)
 * str x1, boot_args[1]  (boot_args + 8)
 * str x2, boot_args[2]  (boot_args + 16)
 * str x3, boot_args[3]  ...
 */
	stp	x21, x1, [x0]			// x0 .. x3 at kernel entry
	stp	x2, x3, [x0, #16]

/*
 * IAMROOT, 2021.07.17:
 *
 * dmb: Data Memory Barrier
 * sy : Any - Any : ALL (시스템에 있는 모든 코어)
 *
 * - Modern SMP arch에서 발생할 수 있는 문제
 *   stp와 dcache_inval_poc 중 어느 것이 더 먼저 수행될지 보장하지 못함.
 *   dcache_inval_poc가 먼저 수행되는것을 방지하고 stp 명령어의 수행 결과를
 *   보장하기 위해 memory barrier를 사용한다.
 *
 * - cache와의 관계성은?
 *   x0 .. x3까지의 모든 데이터를 boot_args에 저장하고 난 뒤 확실하게 정리하고
 *   그 이후에 데이터 캐시를 클린하도록 중간에 베리어 명령을 수행한다.
 */
	dmb	sy				// needed before dc ivac with
						// MMU off

/*
 * IAMROOT, 2021.07.17:
 * - boot_args의 start, end addr을 @x0, @x1에 각각 저장하고 args로 넘긴다.
 *   x0   : 'boot_args' 시작 주소
 *   x1   : 'boot_args' 끝 주소
 *   0x20 : 'boot_args' 배열 크기
 */
	add	x1, x0, #0x20			// 4 x 8 bytes
	b	dcache_inval_poc		// tail call
SYM_CODE_END(preserve_boot_args)

/*
 * Macro to create a table entry to the next page.
 *
 *	tbl:	page table address
 *	virt:	virtual address
 *	shift:	#imm page table shift
 *	ptrs:	#imm pointers per table page
 *
 * Preserves:	virt
 * Corrupts:	ptrs, tmp1, tmp2
 * Returns:	tbl -> next level table page address
 */
/* IAMROOT, 2021.08.28:
 * - @tbl entry 중 하나에 next level page table인 PMD를 연결하기 위한 macro.
 *
 *   memory 저장 구조
 *   +------------------+
 *   | @tbl             | -------> page size 만큼 가지고 있음.
 *   | ...              |
 *   | entry (index)  --+----+
 *   | ...              |    |
 *   +------------------+ <--+
 *   | PMD table        |
 *   +------------------+
 *
 * - C언어 구현
 *   #define create_table_entry(tbl, virt, shift, ptrs, tmp1, tmp2) \
 *       @tmp2 = phys_to_pte(@tbl + PAGE_SIZE)                      \
 *       @tmp2 = @tmp2 | PMD_TYPE_TABLE (page table attrs)          \
 *       @tmp1 = va(@virt) >> @shift                                \
 *       @ptrs = @ptrs - 1 (masking)                                \
 *       @tmp1 = @tmp1 & @ptrs                                      \
 *       *(@tbl + (@tmp1 << 3)) = @tmp2                             \
 *       @tbl  = @tbl + PAGE_SIZE
 */
	.macro	create_table_entry, tbl, virt, shift, ptrs, tmp1, tmp2
	add	\tmp1, \tbl, #PAGE_SIZE
	phys_to_pte \tmp2, \tmp1
	orr	\tmp2, \tmp2, #PMD_TYPE_TABLE	// address of next table and entry type
	lsr	\tmp1, \virt, #\shift
	sub	\ptrs, \ptrs, #1
	and	\tmp1, \tmp1, \ptrs		// table index
	str	\tmp2, [\tbl, \tmp1, lsl #3]
	add	\tbl, \tbl, #PAGE_SIZE		// next level table page
	.endm

/* IAMROOT, 2021.08.21:
 * - compute_indices macro를 통해 구한 @index를 가지고 table entry를 돌면서
 *   각 entry에 속성(@flags)을 매핑하는 역할을 한다.
 *
 * - b.ls : unsigned less or same
 *
 * - @의 의미: macro에서 branch가 필요할 경우 일반 분기문처럼 label을
 *   지정해야 하는데 macro에서는 할 수 없어 compile가 자동으로 label의 번호를
 *   생성하도록 한다.
 *
 * - 예제 (@tbl이 idmap_pg_dir일 때)
 *   @tbl: pa(idmap_pg_dir)
 *   @rtbl: pa(idmap_pg_dir + 4k)
 *
 *   - C 언어 코드
 *     do {
 *       tmp1 = @rtbl | PMD_TYPE_TABLE
 *       @tbl[0x0f * 8(2^3) == 0x78] = tmp1
 *       @rtbl = @rtbl + 4k
 *       index = index + 1
 *     } while (index < eindex)
 */
/*
 * Macro to populate page table entries, these entries can be pointers to the next level
 * or last level entries pointing to physical memory.
 *
 *	tbl:	page table address
 *	rtbl:	pointer to page table or physical memory
 *	index:	start index to write
 *	eindex:	end index to write - [index, eindex] written to
 *	flags:	flags for pagetable entry to or in
 *	inc:	increment to rtbl between each entry
 *	tmp1:	temporary variable
 *
 * Preserves:	tbl, eindex, flags, inc
 * Corrupts:	index, tmp1
 * Returns:	rtbl
 */
	.macro populate_entries, tbl, rtbl, index, eindex, flags, inc, tmp1
.Lpe\@:	phys_to_pte \tmp1, \rtbl
	orr	\tmp1, \tmp1, \flags	// tmp1 = table entry
	str	\tmp1, [\tbl, \index, lsl #3]
	add	\rtbl, \rtbl, \inc	// rtbl = pa next level
	add	\index, \index, #1
	cmp	\index, \eindex
	b.ls	.Lpe\@
	.endm

/* IAMROOT, 2021.08.21:
 * - table의 @vstart, @vend와 @shift, @ptrs를 통해 @istart, @iend의 index와
 *   @count를 계산한다. @count는 실제 entry - 1 로 계산된다.
 *   해당 macro를 수행하고 나면 @istart, @iend, @count가 저장되어 out argument
 *   특성을 가진다.
 *
 *   - PGD table : table 크기가 작으므로 entry값은 보통 1(@count + 1) 이다.
 *       @shift  = 39
 *       @istart = (@vstart >> 39) & (0x1ff)
 *       @iend   = (@vend >> 39) & (0x1ff)
 *       @count  = @iend - @istart (일반적으로 0)
 *
 *   - PUD table : PGD와 마찬가지로 보통 1(@count + 1)개의 entry만 나온다.
 *       @shift = 30로 치환하여 PGD와 동일하게 계산
 *
 *   - PMD table : 2MB 범위이므로 여러개의 entry가 나올 수 있다.
 *       @shift = 21로 치환하여 PGD와 동일하게 계산.
 */
/*
 * Compute indices of table entries from virtual address range. If multiple entries
 * were needed in the previous page table level then the next page table level is assumed
 * to be composed of multiple pages. (This effectively scales the end index).
 *
 *	vstart:	virtual address of start of range
 *	vend:	virtual address of end of range - we map [vstart, vend]
 *	shift:	shift used to transform virtual address into index
 *	ptrs:	number of entries in page table
 *	istart:	index in table corresponding to vstart
 *	iend:	index in table corresponding to vend
 *	count:	On entry: how many extra entries were required in previous level, scales
 *			  our end index.
 *		On exit: returns how many extra entries required for next page table level
 *
 * Preserves:	vstart, vend, shift, ptrs
 * Returns:	istart, iend, count
 */
	.macro compute_indices, vstart, vend, shift, ptrs, istart, iend, count
	lsr	\iend, \vend, \shift
	mov	\istart, \ptrs
	sub	\istart, \istart, #1
	and	\iend, \iend, \istart	// iend = (vend >> shift) & (ptrs - 1)
	mov	\istart, \ptrs
	mul	\istart, \istart, \count
	add	\iend, \iend, \istart	// iend += count * ptrs
					// our entries span multiple tables

	lsr	\istart, \vstart, \shift
	mov	\count, \ptrs
	sub	\count, \count, #1
	and	\istart, \istart, \count

	sub	\count, \iend, \istart
/* IAMROOT, 2023.10.17:
 * - 위 macro를 해석하면 아래 C 코드와 같음.
 *   iend = (vend >> shift) & (ptrs - 1)
 *   iend = iend + (count * ptrs)
 *   istart = (vstart >> shift) & (ptrs - 1)
 *   count = iend - istart
 */
	.endm

/* IAMROOT, 2021.08.21:
 * - 가상메모리 범위의 page table의 모든 entry를 초기화 한다.
 *   일반적으로 SWAPPER_PGTABLE_LEVELS == 3 이므로 PGD, PMD, PTE만 초기화한다.
 *   원래 PGD, PUD, PMD 지만 code의 통일성을 위해 PGD, PMD, PTE로 되어있다.
 *
 *   +------+------+-------+
 *   | Code | 실제 | Shift |
 *   +------+------+-------+
 *   | PGD  | PGD  |  39   |
 *   | PMD  | PUD  |  30   |
 *   | PTE  | PMD  |  21   |
 *   +------+------+-------+
 *
 * - PTE는 compute_indices 와 populate_entries 사이에 bic 명령어를 수행하는데
 *   block의 align을 2MB 로 맞추기 위함이다.
 *
 *   bic : bic A, B, #C ==> A = (B & ~C)
 */
/*
 * Map memory for specified virtual address range. Each level of page table needed supports
 * multiple entries. If a level requires n entries the next page table level is assumed to be
 * formed from n pages.
 *
 *	tbl:	location of page table
 *	rtbl:	address to be used for first level page table entry (typically tbl + PAGE_SIZE)
 *	vstart:	virtual address of start of range
 *	vend:	virtual address of end of range - we map [vstart, vend - 1]
 *	flags:	flags to use to map last level entries
 *	phys:	physical address corresponding to vstart - physical memory is contiguous
 *	pgds:	the number of pgd entries
 *
 * Temporaries:	istart, iend, tmp, count, sv - these need to be different registers
 * Preserves:	vstart, flags
 * Corrupts:	tbl, rtbl, vend, istart, iend, tmp, count, sv
 */
	.macro map_memory, tbl, rtbl, vstart, vend, flags, phys, pgds, istart, iend, tmp, count, sv
/* IAMROOT, 2021.08.24:
 * PGD->PUD(or PMD, when SWAPPER_PGTABLE_LEVELS <= 3) 매핑 시작.
 * - PGD, PUD, PMD의 flags가 전부 PMD_TYPE_TABLE 인데 이는 해당 entry가
 *   table이라는 것을 나타낸다.
 */
	sub \vend, \vend, #1
	add \rtbl, \tbl, #PAGE_SIZE
	mov \sv, \rtbl
	mov \count, #0
	compute_indices \vstart, \vend, #PGDIR_SHIFT, \pgds, \istart, \iend, \count
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
	mov \sv, \rtbl

/* IAMROOT, 2021.08.24:
 * PUD->PMD 매핑 시작.
 */
#if SWAPPER_PGTABLE_LEVELS > 3
	compute_indices \vstart, \vend, #PUD_SHIFT, #PTRS_PER_PUD, \istart, \iend, \count
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
	mov \sv, \rtbl
#endif

/* IAMROOT, 2021.08.24:
 * PMD->PTE 매핑 시작.
 */
#if SWAPPER_PGTABLE_LEVELS > 2
	compute_indices \vstart, \vend, #SWAPPER_TABLE_SHIFT, #PTRS_PER_PMD, \istart, \iend, \count
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
#endif

/* IAMROOT, 2021.08.24:
 * PTE->Section 매핑 시작.
 * - Section 매핑에서는 flags에 SWAPPER_MM_MMUFLAGS가 적용되고 count는
 *   phys addr를 2MB 단위로 정렬하여 rtbl로 사용한다.
 */
	compute_indices \vstart, \vend, #SWAPPER_BLOCK_SHIFT, #PTRS_PER_PTE, \istart, \iend, \count
	bic \count, \phys, #SWAPPER_BLOCK_SIZE - 1
	populate_entries \tbl, \count, \istart, \iend, \flags, #SWAPPER_BLOCK_SIZE, \tmp
	.endm

/*
 * Setup the initial page tables. We only setup the barest amount which is
 * required to get the kernel running. The following sections are required:
 *   - identity mapping to enable the MMU (low address, TTBR0)
 *   - first few MB of the kernel linear mapping to jump to once the MMU has
 *     been enabled
 */
/* IAMROOT, 2022.01.28:
 * - idmap_pg_dir, init_pg_dir을 초기화한다.
 *   idmap_pg_dir: pa, va mem 1:1 매핑을 위한 테이블이며 __idmap_text_start를
 *                 pa, va mem 모두 같게 mapping 한다.
 *   init_pg_dir : kernel page 테이블이며 pa의 경우 adrp로 불러온 __text,
 *                 va는 KIMAGE_VADDR가 되므로 두 값으로 mapping 한다.
 */
SYM_FUNC_START_LOCAL(__create_page_tables)
	mov	x28, lr

	/*
	 * Invalidate the init page tables to avoid potential dirty cache lines
	 * being evicted. Other page tables are allocated in rodata as part of
	 * the kernel image, and thus are clean to the PoC per the boot
	 * protocol.
	 */
/* IAMROOT, 2021.08.14:
 * - 'init_pg_dir' 크기만큼 해당 주소의 d-cache를 모두 invalidation.
 *
 * - arch/arm64/kernel/vmlinux.lds.S
 *
 *   . = ALIGN(PAGE_SIZE);
 *   init_pg_dir = .;
 *   . += INIT_DIR_SIZE;
 *   init_pg_end = .;
 *
 * - 위에서 보는 것 처럼 init_pg_dir는 PAGE_SIZE로 align되어 있다. 따라서
 *   adrp 명령어로 주소를 가져올 수 있다. 마찬가지로, init_pg_end의 경우도
 *   INIT_DIR_SIZE가 PAGE_SIZE의 배수이기 때문에 init_pg_end도 PAGE_SIZE의
 *   배수가 되므로 adrp 명령어로 주소를 가져올 수 있다.
 *
 * - 그러나 아래의 __idmap_text_end의 경우처럼 PAGE_SIZE의 배수가 보장되지
 *   않는 경우에는 adr_l 명령어를 사용하여 하위 12 bits까지 가져올 수 있도록
 *   한다.
 *
 * - adrp로 불러오는 주소가 현재 물리 주소인 이유.
 *   adrp명령어는 load pc-relative 라고 하며 현재 pc 기준으로 symbol 위치가
 *   계산된다.
 *   예를들어 compile time에 init_pg_dir이 0x8000이고
 *   "adrp x0, init_pg_dir" 위치가 0x4000 이라면 이 둘의 offset은 0x4000이고
 *   "adrp x0, init_pg_dir"엔 offset 0x4000이라는 정보로 저장될 것이다.
 *   (인코딩이 어떻게 될것인지는 무시)
 *
 *   그리고 후에 kernel Image가 RAM에 올라가 동작할때는 pc는 실제 RAM에서의
 *   code 위치인 물리주소가 될것이다.
 *
 *   "adrp x0, init_pg_dir"를 실행할때는 offset이 0x4000이라는 정보로
 *   pc + 0x4000로 init_pg_dir을 load할 것이다. pc와 offset 계산으로도
 *   symbol addr을 예측할 수 있다.
 *
 *   head.S는 이렇게 pc-relative instr만으로 구성되어 symbol이 어디에 있던지
 *   상대적으로 계산하여 abs addr 상관없고 이게 head.S의 첫 주석에서 말한
 *   position independent 의미이다.
 */
	adrp	x0, init_pg_dir
	adrp	x1, init_pg_end
	bl	dcache_inval_poc

	/*
	 * Clear the init page tables.
	 */
/* IAMROOT, 2021.08.14:
 * - 'init_pg_dir' 크기만큼 해당 주소의 phys mem을 0으로 초기화한다.
 */
	adrp	x0, init_pg_dir
	adrp	x1, init_pg_end
	sub	x1, x1, x0
1:	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	subs	x1, x1, #64
	b.ne	1b

/* IAMROOT, 2021.08.14:
 * - 커널 매핑에 필요한 디폴트 속성값을 가져오며 다음과 같다.
 *   - Normal memory
 *   - Section type
 *   - Access Flag
 *   - Inner Share
 */
	mov	x7, SWAPPER_MM_MMUFLAGS

	/*
	 * Create the identity mapping.
	 */
/* IAMROOT, 2023.10.12:
 * - idmap은 pa == va 이므로 별다른 매크로없이 접근해도 상관없다.
 */
	adrp	x0, idmap_pg_dir
	adrp	x3, __idmap_text_start		// __pa(__idmap_text_start)

/* IAMROOT, 2021.08.14:
 * - mmfr2_el1.lva (1=52bits va support, 0=none support(48))
 *
 * - 런타임에 64K 페이지 사용 시 최대 52비트 매핑 또는 48비트 매핑을 결정한다.
 *   -  4K, 48bits -> VA_BITS_MIN=48, vabits_actual=48
 *   - 64K, 52bits -> VA_BITS_MIN=48, vabits_actual=52(lva support) or 48
 */
#ifdef CONFIG_ARM64_VA_BITS_52
	mrs_s	x6, SYS_ID_AA64MMFR2_EL1
	and	x6, x6, #(0xf << ID_AA64MMFR2_LVA_SHIFT)
	mov	x5, #52
	cbnz	x6, 1f
#endif

/* IAMROOT, 2023.10.12:
 * - 52bits va NOT supported
 */
	mov	x5, #VA_BITS_MIN
1:
	adr_l	x6, vabits_actual
	str	x5, [x6]
	dmb	sy
	dc	ivac, x6		// Invalidate potentially stale cache line

	/*
	 * VA_BITS may be too small to allow for an ID mapping to be created
	 * that covers system RAM if that is located sufficiently high in the
	 * physical address space. So for the ID map, use an extended virtual
	 * range in that case, and configure an additional translation level
	 * if needed.
	 *
	 * Calculate the maximum allowed value for TCR_EL1.T0SZ so that the
	 * entire ID map region can be mapped. As T0SZ == (64 - #bits used),
	 * this number conveniently equals the number of leading zeroes in
	 * the physical address of __idmap_text_end.
	 */
/* IAMROOT, 2021.08.14:
 * - VA 확장이 필요한지 확인한다.
 *   clz를 통해 pa(__idmap_text_end)의 연속적인 0의 갯수를 알아와
 *   TCR_T0SZ(VA_BITS=예:39)=25와 비교한다. pa에 사용한 0이 더 많은 경우
 *   va 크기가 충분하여 va 영역 확장이 필요 없으니 1f 레이블로 이동한다.
 *
 * - clz: count leading zero bits
 *        MSB에서부터 연속적인 0의 갯수를 알아오는 명령어.
 */
	adrp	x5, __idmap_text_end
	clz	x5, x5
	cmp	x5, TCR_T0SZ(VA_BITS_MIN) // default T0SZ small enough?
	b.ge	1f			// .. then skip VA range extension

/* IAMROOT, 2023.10.12:
 * - 확장이 필요한 경우이므로 pa(__idmap_text_end)의 연속적인 0의 갯수를
 *   idmap_t0sz에 저장하고 va 크기에 따라 다르게 처리한다.
 */
	adr_l	x6, idmap_t0sz
	str	x5, [x6]
	dmb	sy
	dc	ivac, x6		// Invalidate potentially stale cache line

#if (VA_BITS < 48)
/* IAMROOT, 2023.10.14:
 * - VA_BITS < 48 인 경우.
 *   table-table-level 을 확장.
 */
#define EXTRA_SHIFT	(PGDIR_SHIFT + PAGE_SHIFT - 3)
#define EXTRA_PTRS	(1 << (PHYS_MASK_SHIFT - EXTRA_SHIFT))

	/*
	 * If VA_BITS < 48, we have to configure an additional table level.
	 * First, we have to verify our assumption that the current value of
	 * VA_BITS was chosen such that all translation levels are fully
	 * utilised, and that lowering T0SZ will always result in an additional
	 * translation level to be configured.
	 */
/* IAMROOT, 2023.10.14:
 * - PGDIR_SHIFT의 주석을 참고하면 EXTRA_SHIFT와 vabits가 mismatch인 경우가
 *   있다. (64k page size & vabits 48)
 */
#if VA_BITS != EXTRA_SHIFT
#error "Mismatch between VA_BITS and page size/number of translation levels"
#endif

	mov	x4, EXTRA_PTRS
/* IAMROOT, 2021.08.28:
 * - VA_BITS == 39 가정.
 *   VA 확장을 위해 map_memory 실행전 미리 table 1개를 생성한다. 그리고
 *   PGD entry중에 하나를 다음 테이블인 PMD table에 연결하기 위한 코드.
 *   PGD entry에 연결된 다음 PMD table은 @tbl + PAGE_SIZE 이다.
 *
 *   tbl    : @x0: idmap_pg_dir
 *   virt   : @x3: __idmap_text_start
 *   shift  : EXTRA_SHIFT (39 = 30 + 12 - 3)
 *   ptrs   : @x4: EXTRA_PTRS (512 = 2^9)
 *   tmp1   : @x5
 *   tmp2   : @x6
 *
 * - C언어 구현
 *   tmp2 = idmap_pg_dir + 4k
 *   tmp2 = tmp2 | PMD_TYPE_TABLE (page table attrs)
 *   tmp1 = va(__idmap_text_start) >> 39
 *   ptrs = 0x1FF (512 - 1)
 *   tmp1 = tmp1 & 0x1FF
 *   *(idmap_pg_dir + (tmp1 << 3)) = tmp2;
 *   idmap_pg_dir += 4k
 */
	create_table_entry x0, x3, EXTRA_SHIFT, x4, x5, x6
#else
	/*
	 * If VA_BITS == 48, we don't have to configure an additional
	 * translation level, but the top-level table has more entries.
	 */
/* IAMROOT, 2023.10.14:
 * - VA_BITS == 48 인 경우.
 *   PGD table의 entry를 PHYS_MASK_SHIFT - PGDIR_SHIFT 만큼 늘린다.
 *
 *     PHYS_MASK_SHIFT: 48
 *   - PGDIR_SHIFT    : 39
 *   ---------------------
 *                       9
 *   2^9 = 512개
 */
	mov	x4, #1 << (PHYS_MASK_SHIFT - PGDIR_SHIFT)
	str_l	x4, idmap_ptrs_per_pgd, x5
#endif
1:
	ldr_l	x4, idmap_ptrs_per_pgd
	adr_l	x6, __idmap_text_end		// __pa(__idmap_text_end)

/* IAMROOT, 2021.08.21:
 * - idmap_pg_dir 과 init_pg_dir table을 초기화한다.
 *   두 테이블의 크기는 compile time에 정해지고 idmap이 linear mapping table에
 *   비해 상대적으로 작은 크기를 가진다.
 */
/* IAMROOT, 2021.08.24:
 * - Identity Mapping table 초기화.
 *
 *   idmap은 va == pa 라는것을 유념해서 __pa(..) 이더라도
 *   __va(..)와 같아서 vstart, vend에서도 사용 가능하다.
 *
 * idmap_pg_dir
 * +--------+-----+----------+-------------------------------+
 * | role   | dir |          | desc                          |
 * +--------+-----+----------+-------------------------------+
 * | tbl    | i/o | corrupt  | x0 : __pa(idmap_pg_dir)       |
 * | rtbl   | o   | corrupt  | x1 : temp register            |
 * | vstart | i   | preserve | x3 : __pa(__idmap_text_start) |
 * | vend   | i   | preserve | x6 : __pa(__idmap_text_end)   |
 * | flags  | i   | preserve | x7 : SWAPPER_MM_MMUFLAGS      |
 * | phys   | i   | preserve | x3 : __pa(__idmap_text_start) |
 * | pgds   | i   | preserve | x4 : idmap_ptrs_per_pgd       |
 * +--------+-----+----------+-------------------------------+
 *
 * @x10 부터는 temp regs.
 * +--------+-----+----------+-------------------------------------+
 * | istart | o   | corrupt  | x10 : vstart에 대응하는 table index |
 * | iend   | o   | corrupt  | x11 : vend에 대응하는 table index   |
 * | tmp    | o   | corrupt  | x12 : 내부 계산용                   |
 * | count  | i/o | corrupt  | x13 : extra count(iend - istart)    |
 * | sv     | o   | corrupt  | x14 : 내부 계산용                   |
 * +--------+-----+----------+-------------------------------------+
 *
 * - 연습문제
 *   @vend   : 0x0000_0800_0000_0fff
 *   @vstart : 0x0000_07ff_ffff_f000
 *   -------------------------------
 *             0x1fff (size: 8191 bytes)
 *
 *   - PGD: shift(39), ptrs(512), count(0)
 *     1. @iend   = (@vend >> 39) & (512 - 1)
 *                  0x10 (0x10 & 0x1ff)
 *     2. @iend   = @iend + (count * ptrs)
 *                  0x10
 *     3. @istart = (@vstart >> 39) & (512 - 1)
 *                  0xf (0xf & 0x1ff)
 *     4. @count  = @iend - @istart
 *                  1
 *     PGD table: 0xf .. 0x10번 indice의 entry들만 사용.
 *
 *   - PUD: shift(30), ptrs(512), count(1)
 *     1. @iend   = (@vend >> 30) & (512 - 1)
 *                  0x0 (0x2000 & 0x1ff)
 *     2. @iend   = @iend + (count * ptrs)
 *                  0x200 (512)
 *     3. @istart = (@vstart >> 30) & (512 - 1)
 *                  0x1ff (0x1fff & 0x1ff)
 *     4. @count  = @iend - @istart
 *                  1
 *     PUD table: 0x1ff .. 0x200번 indice의 entry들만 사용.
 *
 *   - PMD: shift(21), ptrs(512), count(1)
 *     1. @iend   = (@vend >> 21) & (512 - 1)
 *                  0x0 (0x40_0000 & 0x1ff)
 *     2. @iend   = @iend + (count * ptrs)
 *                  0x200 (512)
 *     3. @istart = (@vstart >> 21) & (512 - 1)
 *                  0x1ff (0x3f_ffff & 0x1ff)
 *     4. @count  = @iend - @istart
 *                  1
 *     PMD table: 0x1ff .. 0x200번 indice의 entry들만 사용.
 */
	map_memory x0, x1, x3, x6, x7, x3, x4, x10, x11, x12, x13, x14

	/*
	 * Map the kernel image (starting with PHYS_OFFSET).
	 */
/* IAMROOT, 2021.08.21:
 * - linear mapping table 초기화를 위해 va(_text), va(_end)를 계산한다.
 *   @x5에 KIMAGE_VADDR을 저장하는데 이는 compile time에 계산된 값이다.
 *   pa(_end) - pa(_text) 의 결과 값에 va(_text)를 더하여 va(_end)를 구한다.
 */
	adrp	x0, init_pg_dir
	mov_q	x5, KIMAGE_VADDR		// compile time __va(_text)
	add	x5, x5, x23			// add KASLR displacement
	mov	x4, PTRS_PER_PGD
	adrp	x6, _end			// runtime __pa(_end)
	adrp	x3, _text			// runtime __pa(_text)
	sub	x6, x6, x3			// _end - _text
	add	x6, x6, x5			// runtime __va(_end)

/* IAMROOT, 2021.08.24:
 * - Linear mapping table 초기화.
 *
 * init_pg_dir
 * +--------+-----+----------+-------------------------------+
 * | role   | dir |          | desc                          |
 * +--------+-----+----------+-------------------------------+
 * | tbl    | i/o | corrupt  | x0 : __pa(init_pg_dir)        |
 * | rtbl   | o   | corrupt  | x1 : temp register            |
 * | vstart | i   | preserve | x5 : __va(_text)              |
 * |        |     |          |      (KIMAGE_VADDR + KASLR)   |
 * | vend   | i   | preserve | x6 : __va(_end)               |
 * | flags  | i   | preserve | x7 : SWAPPER_MM_MMUFLAGS      |
 * | phys   | i   | preserve | x3 : __pa(_text)              |
 * | pgds   | i   | preserve | x4 : PTRS_PER_PGD             |
 * +--------+-----+----------+-------------------------------+
 *
 * @x10 부터는 temp regs.
 * +--------+-----+----------+-------------------------------------+
 * | istart | o   | corrupt  | x10 : vstart에 대응하는 table index |
 * | iend   | o   | corrupt  | x11 : vend에 대응하는 table index   |
 * | tmp    | o   | corrupt  | x12 : 내부 계산용                   |
 * | count  | i/o | corrupt  | x13 : extra count(iend - istart)    |
 * | sv     | o   | corrupt  | x14 : 내부 계산용                   |
 * +--------+-----+----------+-------------------------------------+
 *
 * - 연습문제
 *   @vend   : 0xffff_0000_11f3_f000
 *   @vstart : 0xffff_0000_1008_0000
 *   -------------------------------
 *             0x01eb_f000 (kimage size: 32MB)
 *
 *   - PGD: shift(39), ptrs(512), count(0)
 *     1. @iend   = (@vend >> 39) & (512 - 1)
 *                  0 (0x01ff_fe00 & 0x1ff)
 *     2. @iend   = @iend + (count * ptrs)
 *                  0
 *     3. @istart = (@vstart >> 39) & (512 - 1)
 *                  0 (0x01ff_fe00 & 0x1ff)
 *     4. @count  = @iend - @istart
 *                  0
 *     PGD table: 0번 index의 entry만 사용.
 *
 *   - PUD: shift(30), ptrs(512), count(0)
 *     1. @iend   = (@vend >> 30) & (512 - 1)
 *                  0 (0xfffc_0000 & 0x1ff)
 *     2. @iend   = @iend + (count * ptrs)
 *                  0
 *     3. @istart = (@vstart >> 30) & (512 - 1)
 *                  0 (0xfffc_0000 & 0x1ff)
 *     4. @count  = @iend - @istart
 *                  0
 *     PUD table: 0번 index의 entry만 사용.
 *
 *   - PMD: shift(21), ptrs(512), count(0)
 *     1. @iend   = (@vend >> 21) & (512 - 1)
 *                  0x8f (0xf800_008f & 0x1ff)
 *     2. @iend   = @iend + (count * ptrs)
 *                  0x8f
 *     3. @istart = (@vstart >> 21) & (512 - 1)
 *                  0x80 (0xf800_0080 & 0x1ff)
 *     4. @count  = @iend - @istart
 *                  15
 *     PMD table: 0x80 .. 0x8f번 indice의 entry들만 사용.
 *
 *   마지막 entry가 가르키는 것은 2MB에 align된 block.
 */
	map_memory x0, x1, x5, x6, x7, x3, x4, x10, x11, x12, x13, x14

	/*
	 * Since the page tables have been populated with non-cacheable
	 * accesses (MMU disabled), invalidate those tables again to
	 * remove any speculatively loaded cache lines.
	 */
	dmb	sy

/* IAMROOT, 2021.08.21:
 * - idmap, linear mapping 모두 시스템 레벨에서 sync-up 하기 위해 poc 레벨까지
 *   invalidation 한다.
 */
	adrp	x0, idmap_pg_dir
	adrp	x1, idmap_pg_end
	bl	dcache_inval_poc

	adrp	x0, init_pg_dir
	adrp	x1, init_pg_end
	bl	dcache_inval_poc

	ret	x28
SYM_FUNC_END(__create_page_tables)

	/*
	 * Initialize CPU registers with task-specific and cpu-specific context.
	 *
	 * Create a final frame record at task_pt_regs(current)->stackframe, so
	 * that the unwinder can identify the final frame record of any task by
	 * its location in the task stack. We reserve the entire pt_regs space
	 * for consistency with user tasks and kthreads.
	 */
	.macro	init_cpu_task tsk, tmp1, tmp2
/* IAMROOT, 2021.09.04:
 * - sp, sp_el0, and sp_el1의 차이
 *   sp_el0, sp_el1에 user, kernel에서 사용할 stack 주소를 각각 저장하고
 *   el0 또는 el1 sp를 사용하기 위해 spsel 값을 설정하면 h/w에 의해 sp가
 *   자동으로 sp_el0, sp_el1에 설정했던 주소로 바뀐다.
 *
 *   sp는 다른 레지스터처럼 ldr, str등의 제어를 직접할 수 있는 레지스터이고
 *   sp_elX는 user, kernel에서 사용할 sp addr를 저장하는 공간이된다.
 *   하지만 kernel에서는 sp_el0를 위처럼 사용하진 않는다.
 *
 *   sp_el0:
 *     sp_el0는 유저 공간으로 context switch 된 후 유저 task의 스택 위치를
 *     가리키는 용도로 사용된다. 그러나 커널(el1)에서는 사용하지 않는 스크래치
 *     레지스터 이므로 이를 활용하여 thread_info를 가리키도록 한다.
 *
 *     https://github.com/torvalds/linux/commit/6cdf9c7ca687e01840d0215437620a20263012fc
 *
 *     sp_el0는 el0에서 el1으로 변경될 때 현재 태스크 정보를 (어디로?) 저장하고
 *     el1에서 현재 thread_info를 빠르게 알아내는 용도로 사용된다.
 *     el1에서 sp_el0는 스택과는 관련이 없고 el1 스택은 sp 니모닉으로 바로
 *     접근하며 이때 sp는 sp_el1이다.
 *
 *   https://stackoverflow.com/questions/65059491/why-save-init-task-struct-address-to-sp-el0-in-arm64-boot-code-primary-switche
 *
 *   위 링크를 통해 assembly 예제를 확인할 수 있다.
 *
 * - get_current 함수 참고
 *
 * - scs(shadow call stack)
 *   func return address overflow를 방지하는 기능.
 */
	msr	sp_el0, \tsk

/* IAMROOT, 2023.11.07:
 * - init_stack에 struct pt_regs의 정보를 저장할 영역을 설정하여 pt_regs의
 *   첫 offset에서 stackframe offset 까지를 0으로 초기화한다. 그리고
 *   va(stackframe)를 @x29에 저장.
 */
	ldr	\tmp1, [\tsk, #TSK_STACK]
	add	sp, \tmp1, #THREAD_SIZE
	sub	sp, sp, #PT_REGS_SIZE

	stp	xzr, xzr, [sp, #S_STACKFRAME]
	add	x29, sp, #S_STACKFRAME

	scs_load \tsk

/* IAMROOT, 2022.02.07:
 * - 현재 cpu 번호는 #TSK_CPU를 통해서 가져오고 이를 index로 하여
 *   __per_cpu_offset array의 bucket을 결정한다. bucket에 저장된 값을
 *   읽어와 set_this_cpu_offset 매크로를 통해 tpidr reg에 저장한다.
 */
	adr_l	\tmp1, __per_cpu_offset
	ldr	w\tmp2, [\tsk, #TSK_CPU]
	ldr	\tmp1, [\tmp1, \tmp2, lsl #3]
	set_this_cpu_offset \tmp1
	.endm

/*
 * The following fragment of code is executed with the MMU enabled.
 *
 *   x0 = __PHYS_OFFSET
 */
SYM_FUNC_START_LOCAL(__primary_switched)
/* IAMROOT, 2021.09.04:
 * - el1 sp(stack pointer)를 통해 init_stack에 존재하는 struct pt_regs 구조체를
 *   0으로 초기화하고 sp_el0을 init_thread_union(init_task)를 바라보도록
 *   설정한다. 또한 @x29 레지스터는 struct pt_regs의 stackframe을 바라본다.
 *
 *   init_thread_union >> INIT_TASK_DATA >> RW_DATA >> vmlinux.lds.S
 */
	adr_l	x4, init_task
	init_cpu_task x4, x5, x6

/* IAMROOT, 2023.11.08:
 * - 현재 init_stack의 memory layout 다이어그램 (CONFIG에 따라 달라짐)
 *
 *           .           . --> 0xffff ... higher addr
 *           | ...       |
 *           | ...       |
 *   +-------+-----------+ !!- end: init_stack
 *           | 0000 .... |          stack size: THREAD_SIZE (16KB: kasan off)
 *           | 0000 0000 |
 *           |-----------| <-- struct pt_regs object의 offset
 *    STACK  | xxxx .... |
 *    RANGE  |           | <-- sp (el1 stack pointer, grow down)
 *           |           |     pt_regs offset을 넘어서 사용할 수 없음
 *           |           |
 *           |           |
 *   +-------+-----------+ !!- start: init_stack == init_thread_union
 *           | ...       |
 *           | ...       |
 *           '           ' --> 0xffff ... lower addr
 */

/* IAMROOT, 2021.09.04:
 * - el1 vector table 주소 설정 ('vectors' in kernel/entry.S).
 */
	adr_l	x8, vectors			// load VBAR_EL1 with virtual
	msr	vbar_el1, x8			// vector table address
	isb

/* IAMROOT, 2021.09.04:
 * - init_stack을 초기화하였으니 previous stack frame 정보(fp, lr)를 kernel의
 *   swapper stack에 저장한다. 이유는 현재 __primary_switched 함수를 호출한
 *   상태이기 때문이다.
 *   이제 nested procedure call이 가능하고 현재 stack frame을 가리키도록
 *   fp(@x29)를 보정한다.
 *
 * - FYI, 현재 stack frame에는 fp', lr'의 정보가 저장되어 있으며 위 내용은
 *   AAPCS64 std를 확인하면 더 자세하게 알 수 있다.
 *
 * - 만약 fp, lr을 저장하지 않으면 아래 bl을 실행할 때 해당 정보가 손실된다.
 *
 *   @fp: frame pointer
 *        이전 fp 정보를 가리키는 pointer.
 *   @lr: link register
 *        caller의 주소를 가지고 있는 reg.
 */
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

/* IAMROOT, 2021.09.04:
 * - 부팅시 부트로더에서 넘겨받은 pa(fdt)를 __fdt_pointer 변수에 저장한다.
 *   @x5는 temp로 사용한다.
 *
 *   mmu enable 되어 먼거리로 branch가 가능하니 지금 변수에 저장한다.
 */
	str_l	x21, __fdt_pointer, x5		// Save FDT pointer

/* IAMROOT, 2021.09.04:
 * - va(_text)와 pa(_text) 간의 offset을 구해서 kimage_voffset에 저장한다.
 *   voffset은 va와 pa간 주소변환에 사용된다.
 *
 * - kimage_voffset = kimage_vaddr - __PHYS_OFFSET
 *   kimage_vaddr  == va(_text)
 *   __PHYS_OFFSET == pa(_text)
 */
	ldr_l	x4, kimage_vaddr		// Save the offset between
	sub	x4, x4, x0			// the kernel virtual and
	str_l	x4, kimage_voffset, x5		// physical mappings

/* IAMROOT, 2021.09.04:
 * - el0 app을 동작시킬때는 loader가 bss를 초기화하지만 kernel은 자신이
 *   해야한다. AAPCS64 std에 따라 @x0, @x1, @x2 args를 세팅하고 memset을
 *   호출한다.
 */
	// Clear BSS
	adr_l	x0, __bss_start
	mov	x1, xzr
	adr_l	x2, __bss_stop
	sub	x2, x2, x0
	bl	__pi_memset
	dsb	ishst				// Make zero page visible to PTW

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	bl	kasan_early_init
#endif

/* IAMROOT, 2023.11.11:
 * - early_fdt_map, init_feature_override는 c로 작성된 procedure 이므로
 *   stack frame 생성, sp 보정은 compiler가 처리한다.
 */
	mov	x0, x21				// pass FDT address in x0
	bl	early_fdt_map			// Try mapping the FDT early
	bl	init_feature_override		// Parse cpu feature overrides

#ifdef CONFIG_RANDOMIZE_BASE
/* IAMROOT, 2021.09.04:
 * - CONFIG_RANDOMIZE_BASE == on이면 __primary_switch에서 __primary_switched를
 *   2번 호출하고, 처음에는 ret을 실행하며 두번째에는 tst를 통해 0번 label로
 *   이동하여 start_kernel을 실행하는 구조이다.
 *
 * - CONFIG_RELOCATABLE == on, CONFIG_RANDOMIZE_BASE == on시
 *   __primary_switch ~ start_kernel 사이의
 *   kimage_vaddr, kimage_voffset, init_pg_dir 변화 과정
 *
 *                         kimage_vaddr  | kimage_voffset | init_pg_dir
 * ----------------------+---------------+----------------+-------------
 * __primary_switch      | va(_text)     | 계산전         | 
 * ----------------------+---------------+----------------+-------------
 * __relocate_kernel(1)  | rel va(_text) | 계산전         | 
 * ----------------------+---------------+----------------+-------------
 * __primary_switched(1) | rel va(_text) | rel va(_text)  | 
 *                       |               | -   pa(_text)  |
 * ----------------------+---------------+----------------+-------------
 * __create_page_tables  | rel va(_text) | rel va(_text)  | + offset
 *                       |               | -   pa(_text)  |
 * ----------------------+---------------+----------------+-------------
 * __relocate_kernel(2)  | rel, offset   | rel va(_text)  | + offset
 *                       | va(_text)     | -   pa(_text)  |
 * ----------------------+---------------+----------------+-------------
 * __primary_switched(2) | rel, offset   | rel, offset    | + offset
 *                       | va(_text)     | va(_text)      | 
 *                       |               | -   pa(_text)  |
 * ----------------------+---------------+----------------+-------------
 */
	tst	x23, ~(MIN_KIMG_ALIGN - 1)	// already running randomized?
	b.ne	0f
	bl	kaslr_early_init		// parse FDT for KASLR options
	cbz	x0, 0f				// KASLR disabled? just proceed
	orr	x23, x23, x0			// record KASLR offset
	ldp	x29, x30, [sp], #16		// we must enable KASLR, return
	ret					// to __primary_switch()
0:
#endif

/* IAMROOT, 2021.09.04: TODO
 * - VHE로 동작할 수 있는 환경이면 hvc-call을 호출하여 host-OS를 el2에서
 *   동작시킬 수 있도록 mode를 변경한다.
 */
	bl	switch_to_vhe			// Prefer VHE if possible

/* IAMROOT, 2023.11.11:
 * - start_kernel(..)을 호출할 준비를 한다.
 *   start_kernel가 호출되면 저장해둔 stack frame 정보(fp', lr')는
 *   불필요하므로 stack에서 pop하여 버린다.
 *
 * - 현재 kernel은 다음 중 하나의 exception level에서 동작중이다.
 *   el1        : bootmode == el1 && CurrentEL == el1
 *   el1 w/ nVHE: bootmode == el2 && CurrentEL == el1 (hypervisor 환경)
 *   el2 w/ VHE : bootmode == el2 && CurrentEL == el2 (kvm 환경)
 */
	ldp	x29, x30, [sp], #16
	bl	start_kernel
	ASM_BUG()
SYM_FUNC_END(__primary_switched)

	.pushsection ".rodata", "a"
SYM_DATA_START(kimage_vaddr)
	.quad		_text
SYM_DATA_END(kimage_vaddr)
EXPORT_SYMBOL(kimage_vaddr)
	.popsection

/*
 * end early head section, begin head code that is also used for
 * hotplug and needs to have the same protections as the text region
 */
	.section ".idmap.text","awx"

/*
 * Starting from EL2 or EL1, configure the CPU to execute at the highest
 * reachable EL supported by the kernel in a chosen default state. If dropping
 * from EL2 to EL1, configure EL2 before configuring EL1.
 *
 * Since we cannot always rely on ERET synchronizing writes to sysregs (e.g. if
 * SCTLR_ELx.EOS is clear), we place an ISB prior to ERET.
 *
 * Returns either BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2 in w0 if
 * booted in EL1 or EL2 respectively.
 */
/* IAMROOT, 2022.01.27:
 * - CurrentEL 1 -> EL1
 * - CurrentEL 2 and nVHE - > EL1
 * - CurrentEL 2 and VHE - > EL2
 *   EL2로 부팅을하여 VHE를 지원시 원래는 EL1 nVHE로 동작을 해야되는데
 *   아래 주석에 있다 시피 일부 Fruity cpu (ex M1. Git blame 참고)에서
 *   EL1에서 nVHE로 동작을 못하는 현상이 있다. 그래서 EL2 VHE로
 *   여기서 초기화를 시킨다.
 */
SYM_FUNC_START(init_kernel_el)
/* IAMROOT, 2021.07.24:
 * - 현재 실행중인 EL(Exception Level) 값을 @x0에 저장하고 아래 조건문 수행.
 *   @x0 == CurrentEL_EL2 ? init_el2 실행 (hypervisor)
 *                        : init_el1 실행 (kernel)
 */
	mrs	x0, CurrentEL
	cmp	x0, #CurrentEL_EL2
	b.eq	init_el2

/* IAMROOT, 2021.07.24:
 * - CurrentEL == EL1이면 실행되는 코드
 *   @return x0(BOOT_CPU_MODE_EL1)
 */
SYM_INNER_LABEL(init_el1, SYM_L_LOCAL)
/* IAMROOT, 2023.09.26:
 * - SCTLR_EL1 reg에 EL1_MMU_OFF bits 설정.
 */
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr	sctlr_el1, x0
	isb

/* IAMROOT, 2023.09.25:
 * - spsr reg에 PSTATE.DAIF bits를 1로 세팅 및 EL1 SP를 사용하도록 값을
 *   저장하고 eret 호출시 return할 'init_kernel_el' caller의 주소를
 *   elr_el1 reg에 저장한다.
 *   - PSTATE.DAIF bits: 1로 세팅하면 all async exceptions disabled 의미.
 *                       Nested interrupt가 아닌 이상 mask bits가 설정되면
 *                       이후에 발생하는 interrupt는 pending 된다.
 *   - EL1 SP: M[3:0] bits를 EL1h로 설정하여 ELx mode + ELx SP 사용 표시.
 *             아래 설명 참고.
 *
 * - elr_elx: exception link reg.
 *            exception handler 처리 후 돌아갈 return 주소를 담고 있으며
 *            exception 전용 reg.
 *
 * - PSTATE: process의 상태를 나타내는 개념이며 실제로는 cpsr, spsr reg가
 *           쓰인다.
 * - CPSR  : Current Program Status reg
 *           프로그램의 현재 상태(ALU 결과, exception flags)를 가지고 있는 reg.
 *           한개만 존재한다.
 * - SPSR  : Saved Program Status reg
 *           ELx mode 변경시에 현재 cpsr 값을 저장하기 위해 사용되는 reg이며
 *           ELx mode 별로 다른 reg가 존재한다.
 *
 *   condition flags
 *      - N: Negative condition flag
 *           1 (결과가 음수)
 *           0 (결과가 0 또는 양수)
 *      - Z: Zero condition flag
 *           1 (instr의 결과가 0)
 *           0 (그 외 나머지)
 *      - C: Carry condition flag
 *           1 (instr의 결과로 carry 발생시 masking 됨)
 *           0 (그 외 나머지)
 *      - V: Overflow condition flag
 *           1 (instr의 결과로 overflow 발생시 masking 됨)
 *           0 (그 외 나머지)
 */
	mov_q	x0, INIT_PSTATE_EL1
	msr	spsr_el1, x0
	msr	elr_el1, lr

/* IAMROOT, 2023.09.26:
 * - cpu boot가 EL1이라고 설정한다.
 * - eret: exception return 명령어.
 *   exception이 발생한 ELx의 spsr이 pstate로 복원되며 ELR_ELx에 저장된
 *   address로 점프한다.
 *   ex) currentEL == 1 이면 spsr_el1, elr_el1 접근
 *       currentEL == 2, nVHE이면 spsr_el2, elr_el2 접근
 *
 * - el1 -> el1으로 이동하는 개념인데, 변화가 없는것 같지만 eret을 통해
 *   pstate가 일관되게 초기화되는 효과가 있다고한다.
 *   그래서 eret을 한번 써주는것이다.
 */
	mov	w0, #BOOT_CPU_MODE_EL1
	eret


/* IAMROOT, 2021.11.14:
 * - CurrentEL == EL2면 실행되는 코드
 *   @return x0(BOOT_CPU_MODE_EL2)
 */
SYM_INNER_LABEL(init_el2, SYM_L_LOCAL)
/* IAMROOT, 2021.07.24:
 * - nVHE flags로 hcr_el2를 설정하여 시작하며 el2 관련 초기화를 진행한다.
 *
 * - hcr_el2: hypervisor configuration reg.
 *            가상화 설정 관련 register.
 *
 * - isb: instruction synchronization barrier.
 *        endian 설정이 끝나면 i-cache pipeline을 비워 바뀐 endian으로
 *        실행될 수 있도록 barrier를 수행한다.
 */
	mov_q	x0, HCR_HOST_NVHE_FLAGS
	msr	hcr_el2, x0
	isb

/* IAMROOT, 2021.11.14:
 * - arch/arm64/include/asm/el2_setup.h
 */
	init_el2_state

/* IAMROOT, 2021.08.14:
 * - EL2 hypervisor vector addr을 vbar_el2 reg에 기록한다.
 *   vbar_el2: Vector Base Addr reg.
 *             any el에서 exception이 발생하여 el2로 점프해야 하는 경우 처리할
 *             exception handler vector table의 addr 저장.
 */
	/* Hypervisor stub */
	adr_l	x0, __hyp_stub_vectors
	msr	vbar_el2, x0
	isb

	/*
	 * Fruity CPUs seem to have HCR_EL2.E2H set to RES1,
	 * making it impossible to start in nVHE mode. Is that
	 * compliant with the architecture? Absolutely not!
	 */
/* IAMROOT, 2021.11.14:
 * - HCR_E2H bit가 clear 되었는지 확인한다. 예상대로면 HCR_HOST_NVHE_FLAGS 에서
 *   clear 되어야 하지만 몇몇 cpu 에서는 RES1로 동작하여 clear 되지 않는다고 한다.
 *   HCR_E2H bit가 clear 되었다면 일반 동작(1f)로 이동하고 그게 아니면 VHE로
 *   동작한다.
 *
 * - E2H : FEAT_VHE 지원하는 경우 host-OS를 EL2에서 동작시키기 위한 bit.
 */
	mrs	x0, hcr_el2
	and	x0, x0, #HCR_E2H
	cbz	x0, 1f

/* IAMROOT, 2021.11.14: TODO
 * (E2H bit가 clear 되지 않는 예외 처리 로직)
 *
 * - E2H == 1인 상황에서는 바로 VHE mode로 전환할 준비를 한다.
 *   EL2에서 동작하는 host-OS가 EL1 reg값을 동일하게 rw 하도록 준비.
 *   VHE 동작 전에 해당 reg를 초기화하며 hvc 동작시 사용된다.
 *
 *   HCR_E2H == 1 이면 EL1에서 동작해야 할 host-OS가 EL2에서 동작하므로
 *   EL1 reg에 접근할 수 있도록 alias 사용.
 *
 * - FEAT_VHE 지원 및 E2H == 1 인 상황에서 아래와 같이 동작한다.
 *   <reg>_EL1 접근 : EL2로 redirect.
 *   <reg>_EL12 접근: EL1로 redirect.
 *   <reg>_EL02 접근: EL0로 redirect.
 */
	/* Switching to VHE requires a sane SCTLR_EL1 as a start */
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr_s	SYS_SCTLR_EL12, x0

	/*
	 * Force an eret into a helper "function", and let it return
	 * to our original caller... This makes sure that we have
	 * initialised the basic PSTATE state.
	 */
/* IAMROOT, 2021.11.14: TODO
 * - spsr_el2, elr_el2에 EL2 daif, SP(el1)을 설정하고 return addr을
 *   __cpu_stick_to_vhe로 설정한다. 따라서 eret 수행시 해당 함수로 점프한다.
 *
 * - FEAT_VHE 및 E2H == 1 인 현재 상황에서는 <sys-reg>_el1은 모두 el2로
 *   redirect 된다. 따라서 el1이지만 h/w 레벨에서 el2로 접근하도록 동작한다.
 *
 * - eret 호출 전
 *             pc: init_kernel_el
 *   PSTATE(cpsr): unknown but current one
 *
 * - eret 호출 후
 *             pc: __cpu_stick_to_vhe (예외 처리 함수)
 *   PSTATE(cpsr): INIT_PSTATE_EL2
 */
	mov	x0, #INIT_PSTATE_EL2
	msr	spsr_el1, x0
	adr	x0, __cpu_stick_to_vhe
	msr	elr_el1, x0
	eret

/* IAMROOT, 2021.11.14:
 * - nVHE 모드이므로 eret시 EL1로 변경할 준비를 한다.
 *   guest-OS는 EL1에서, hypervisor는 EL2에서 동작중인 상황.
 *   따라서 spsr_el2, elr_el2 모두 EL1과 관련된 정보들로 채워진다.
 */
1:
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr	sctlr_el1, x0

	msr	elr_el2, lr
	mov	w0, #BOOT_CPU_MODE_EL2
	eret

__cpu_stick_to_vhe:
/* IAMROOT, 2021.11.14: TODO
 * - FEAT_VHE / E2H == 1 인 상황이자 E2H bit clear를 시도했지만 RES1로 인해
 *   예외 처리가 필요한 상황에서 호출되는 helper 함수.
 *
 * - hvc (synchronous exception)을 발생시키며 __hyp_stub_vectors의 elx_sync가
 *   호출된다. arg는 HVC_VHE_RESTART로 세팅되어 hyp-call handler 호출.
 *
 *   1. hvc 호출
 *      elr_el2 reg에 pc 저장.
 *      spsr_el2 reg에 PSTATE 저장.
 *   2. hyp-call handler 수행 (sync exception)
 *   3. hvc 종료
 *      elr_el2 reg 값을 pc에 복구.
 *      spsr_el2 reg 값을 PSTATE에 복구.
 */
	mov	x0, #HVC_VHE_RESTART
	hvc	#0
	mov	x0, #BOOT_CPU_MODE_EL2
	ret
SYM_FUNC_END(init_kernel_el)

/*
 * Sets the __boot_cpu_mode flag depending on the CPU boot mode passed
 * in w0. See arch/arm64/include/asm/virt.h for more info.
 */
/* IAMROOT, 2021.08.14:
 * - w0에 저장된 boot mode 값을 __boot_cpu_mode에 저장한다.
 *   해당 함수가 호출되고 나면 __boot_cpu_mode에 있는 2개의 배열 값은 아래와
 *   같이 동일해진다.
 *
 *   - 호출 전
 *      __boot_cpu_mode[] = { EL2, EL1 }
 *   - 호출 후
 *      __boot_cpu_mode[] = { EL2, EL2 } 또는 { EL1, El1 }
 */
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
	adr_l	x1, __boot_cpu_mode
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f
	add	x1, x1, #4
1:	str	w0, [x1]			// Save CPU boot mode
	dmb	sy
	dc	ivac, x1			// Invalidate potentially stale cache line
	ret
SYM_FUNC_END(set_cpu_boot_mode_flag)

/*
 * These values are written with the MMU off, but read with the MMU on.
 * Writers will invalidate the corresponding address, discarding up to a
 * 'Cache Writeback Granule' (CWG) worth of data. The linker script ensures
 * sufficient alignment that the CWG doesn't overlap another section.
 */
	.pushsection ".mmuoff.data.write", "aw"
/*
 * We need to find out the CPU boot mode long after boot, so we need to
 * store it in a writable variable.
 *
 * This is not in .bss, because we set it sufficiently early that the boot-time
 * zeroing of .bss would clobber it.
 */
/* IAMROOT, 2021.08.14:
 * - init_kernel_el에서 결정된 boot mode 값을 저장한다.
 *
 *   - 초기화 시
 *      __boot_cpu_mode[] = { EL2, EL1 }
 *   - set_cpu_boot_mode_flag 함수 호출 후
 *      __boot_cpu_mode[] = { EL2, EL2 } 또는 { EL1, El1 }
 *
 * - .long: 4 bytes
 *
 * - 배열은 '.mmuoff.data.write' section에 저장되며 아래 C 코드와 유사한
 *   의미를 가진다.
 *
 *   static int __boot_cpu_mode[] = {
 *      BOOT_CPU_MODE_EL2, BOOT_CPU_MODE_EL1
 *   };
 */
SYM_DATA_START(__boot_cpu_mode)
	.long	BOOT_CPU_MODE_EL2
	.long	BOOT_CPU_MODE_EL1
SYM_DATA_END(__boot_cpu_mode)
/*
 * The booting CPU updates the failed status @__early_cpu_boot_status,
 * with MMU turned off.
 */
SYM_DATA_START(__early_cpu_boot_status)
	.quad 	0
SYM_DATA_END(__early_cpu_boot_status)

	.popsection

	/*
	 * This provides a "holding pen" for platforms to hold all secondary
	 * cores are held until we're ready for them to initialise.
	 */
SYM_FUNC_START(secondary_holding_pen)
	bl	init_kernel_el			// w0=cpu_boot_mode
	bl	set_cpu_boot_mode_flag
	mrs	x0, mpidr_el1
	mov_q	x1, MPIDR_HWID_BITMASK
	and	x0, x0, x1
	adr_l	x3, secondary_holding_pen_release
pen:	ldr	x4, [x3]
	cmp	x4, x0
	b.eq	secondary_startup
	wfe
	b	pen
SYM_FUNC_END(secondary_holding_pen)

	/*
	 * Secondary entry point that jumps straight into the kernel. Only to
	 * be used where CPUs are brought online dynamically by the kernel.
	 */
SYM_FUNC_START(secondary_entry)
	bl	init_kernel_el			// w0=cpu_boot_mode
	bl	set_cpu_boot_mode_flag
	b	secondary_startup
SYM_FUNC_END(secondary_entry)

SYM_FUNC_START_LOCAL(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 */
	bl	switch_to_vhe
	bl	__cpu_secondary_check52bitva
	bl	__cpu_setup			// initialise processor
	adrp	x1, swapper_pg_dir
	bl	__enable_mmu
	ldr	x8, =__secondary_switched
	br	x8
SYM_FUNC_END(secondary_startup)

SYM_FUNC_START_LOCAL(__secondary_switched)
	adr_l	x5, vectors
	msr	vbar_el1, x5
	isb

	adr_l	x0, secondary_data
	ldr	x2, [x0, #CPU_BOOT_TASK]
	cbz	x2, __secondary_too_slow

	init_cpu_task x2, x1, x3

#ifdef CONFIG_ARM64_PTR_AUTH
	ptrauth_keys_init_cpu x2, x3, x4, x5
#endif

	bl	secondary_start_kernel
	ASM_BUG()
SYM_FUNC_END(__secondary_switched)

SYM_FUNC_START_LOCAL(__secondary_too_slow)
	wfe
	wfi
	b	__secondary_too_slow
SYM_FUNC_END(__secondary_too_slow)

/* IAMROOT, 2021.08.28:
 * - @status를 __early_cpu_boot_status 변수에 저장한다.
 *   @status == 0: 정상
 */
/*
 * The booting CPU updates the failed status @__early_cpu_boot_status,
 * with MMU turned off.
 *
 * update_early_cpu_boot_status tmp, status
 *  - Corrupts tmp1, tmp2
 *  - Writes 'status' to __early_cpu_boot_status and makes sure
 *    it is committed to memory.
 */

	.macro	update_early_cpu_boot_status status, tmp1, tmp2
	mov	\tmp2, #\status
	adr_l	\tmp1, __early_cpu_boot_status
	str	\tmp2, [\tmp1]
	dmb	sy
	dc	ivac, \tmp1			// Invalidate potentially stale cache line
	.endm

/*
 * Enable the MMU.
 *
 *  x0  = SCTLR_EL1 value for turning on the MMU.
 *  x1  = TTBR1_EL1 value
 *
 * Returns to the caller via x30/lr. This requires the caller to be covered
 * by the .idmap.text section.
 *
 * Checks if the selected granule size is supported by the CPU.
 * If it isn't, park the CPU
 */
SYM_FUNC_START(__enable_mmu)
/* IAMROOT, 2021.08.28:
 * - 크기가 어찌되었든 granule size를 지원하지 않으면 __no_granule_support로
 *   점프하여 오류 값을 설정하고 inf-loop를 수행한다. 지원하면 정상을 의미하는
 *   상태코드, 0을 __early_cpu_boot_status 변수에 저장한다. early startup에서
 *   H/W 디버거를 이용하여 status 코드를 확인할 수 있다.
 *
 * - TGRAN  : Translation Granule Size.
 * - TGran4[31:28] : 4KB granule size.
 *           0b0000: supported
 *           0b0001: supports 52-bit I/O addr
 *           0b1111: NOT supported
 * - TGran16[23:20]: 16KB granule size.
 *           0b0000: NOT supported
 *           0b0001: supported
 *           0b0010: supports 52-bit I/O addr
 * - TGran64[27:24]: 64KB granule size
 *           0b0000: supported
 *           0b1111: NOT supported
 */
	mrs	x2, ID_AA64MMFR0_EL1
	ubfx	x2, x2, #ID_AA64MMFR0_TGRAN_SHIFT, 4
	cmp     x2, #ID_AA64MMFR0_TGRAN_SUPPORTED_MIN
	b.lt    __no_granule_support
	cmp     x2, #ID_AA64MMFR0_TGRAN_SUPPORTED_MAX
	b.gt    __no_granule_support
	update_early_cpu_boot_status 0, x2, x3
	adrp	x2, idmap_pg_dir
	phys_to_ttbr x1, x1
	phys_to_ttbr x2, x2

/* IAMROOT, 2021.08.28:
 * - ttbr{0/1}_el1 reg 각각에 idmap_pg_dir, init_pg_dir addr을 설정한다.
 *
 * - @x1: init_pg_dir의 ttbr
 *   @x2: idmap_pg_dir의 ttbr
 *   @ttbr0_el1 : user에서 사용되지만 부팅중에는 idmap을 위해 임시로 사용한다.
 *   @ttbr1_el1 : kernel에서 사용되며 'init_pg_dir'을 가리킨다.
 */
	msr	ttbr0_el1, x2			// load TTBR0
	offset_ttbr1 x1, x3
	msr	ttbr1_el1, x1			// load TTBR1
	isb

/* IAMROOT, 2021.08.28:
 * - @x0에 설정된 INIT_SCTLR_EL1_MMU_ON 값을 sctlr_el1에 set하여
 *   MMU, trapping, d/i-cache 등 메모리와 관련된 기능을 on 한다.
 */
	set_sctlr_el1	x0

	ret
SYM_FUNC_END(__enable_mmu)

SYM_FUNC_START(__cpu_secondary_check52bitva)
#ifdef CONFIG_ARM64_VA_BITS_52
	ldr_l	x0, vabits_actual
	cmp	x0, #52
	b.ne	2f

	mrs_s	x0, SYS_ID_AA64MMFR2_EL1
	and	x0, x0, #(0xf << ID_AA64MMFR2_LVA_SHIFT)
	cbnz	x0, 2f

	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_52_BIT_VA, x0, x1
1:	wfe
	wfi
	b	1b

#endif
2:	ret
SYM_FUNC_END(__cpu_secondary_check52bitva)

/* IAMROOT, 2021.08.28:
 * - arch가 granule을 지원하지 않아 'status = error'로 설정하고 inf-wait 상태로
 *   들어간다. H/W 디버깅을 할 때 해당 변수 값을 확인하여 error를 확인하기 위해
 *   status 값을 세팅한다. (__early_cpu_boot_status)
 *
 *   inf-loop에서 빠르게 동작하지 않도록 하여 전력 소비를 줄인다.
 *   - wfe: wait for event
 *   - wfi: wait for interrupt
 */
SYM_FUNC_START_LOCAL(__no_granule_support)
	/* Indicate that this CPU can't boot and is stuck in the kernel */
	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2
1:
	wfe
	wfi
	b	1b
SYM_FUNC_END(__no_granule_support)

#ifdef CONFIG_RELOCATABLE
SYM_FUNC_START_LOCAL(__relocate_kernel)
	/*
	 * Iterate over each entry in the relocation table, and apply the
	 * relocations in place.
	 */
	ldr	w9, =__rela_offset		// offset to reloc table
	ldr	w10, =__rela_size		// size of reloc table

	mov_q	x11, KIMAGE_VADDR		// default virtual offset
/*
 * IAMROOT, 2021.08.28:
 * - x23 : compile time에 계산된 kernel 주소의 2MB 정렬의 나머지값.
 *   보통 0으로 보면된다. randomized가 적용된경우 이 x23이 조금 틀어질수도있다.
 *
 * - relocate start address(x9)
 *   x9 = x23(0) + KIMAGE_VADDR(x11) + __rela_offset(w9)
 *   
 * - relocate end address(x10)
 *   x10 = x9 + __rela_size(w10)
 */
	add	x11, x11, x23			// actual virtual offset
	add	x9, x9, x11			// __va(.rela)
	add	x10, x9, x10			// __va(.rela) + sizeof(.rela)

0:	cmp	x9, x10
	b.hs	1f
/*
 * IAMROOT, 2021.08.28:
 * =====================================================================
 *  - ELF Relocation Table Entry Format
 *  https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-54839.html
 *
 *  +-------------------------------+--------+------+------------+
 *  | Description                   | offset | size | filed name |
 *  +-------------------------------+--------+------+------------+
 *  | Offset used to cacluate reloc | 0x00   | 8    | r_offset   |
 *  | Reloc typing meta-data        | 0x08   | 8    | r_info     |
 *  | Extra argument used in reloc  | 0x10   | 8    | r_addend   |
 *  +-------------------------------+--------+------+------------+
 *
 *  elf 스펙상 현재 Table Entry를 24byte씩 사용한다.
 *  Table 정보에는 offset, type, addend등이 있으며 그중에 type을 비교한다.
 *
 * =====================================================================
 *
 *  다음은 실제 vmlinux의 .rel.dyn section을 72byte만
 *  dump 한 예제이며, 현재 x9는 첫번째줄의 데이터 0번째를
 *  가리키고 있을것이다.
 *
 * -----------------------------------------------------------
 * (x9) 989fdd10 0080ffff 03040000 00000000 3c9cdb10 0080ffff
 *      a09fdd10 0080ffff 03040000 00000000 d86ae010 0080ffff
 *      a89fdd10 0080ffff 03040000 00000000 4058dd10 0080ffff
 * (little endian으로 되있다는것을 고려한다.)
 * (x9 : x9에 저장되있는 주소의 위치)
 * -----------------------------------------------------------
 *
 * 행별로 첫 8byte가 r_offset, 그다음 8byte가 r_info, 그 다음 8byte가 r_addend
 * 가 된다.
 * 
 * x12, x13 register에 어떻게 데이터가 저장되는지 살펴보면
 * (편의상 little endian dump를 그대로 사용한다)
 * 
 * ldp	x12, x13, [x9], #24 이 명령어가 실행되면 첫번쨰줄의
 *
 * -----------------------------------------------------------
 * (x9) 989fdd10 0080ffff 03040000 00000000 3c9cdb10 0080ffff
 * -----------------------------------------------------------
 * 에서
 *
 * x12 = [x9] = 989fdd10 0080ffff
 * x13 = [x9, 8] = 03040000 00000000
 *
 * 이렇게 x12, x13에 데이터가 저장될것이고
 * 3c9cdb10 0080ffff 이 다음이 x9가 될것이다.
 *
 * -----------------------------------------------------------
 *      989fdd10 0080ffff 03040000 00000000 3c9cdb10 0080ffff
 * (x9) a09fdd10 0080ffff 03040000 00000000 d86ae010 0080ffff
 *      a89fdd10 0080ffff 03040000 00000000 4058dd10 0080ffff
 * -----------------------------------------------------------
 *
 * ldr	x14, [x9, #-8] 이 명령어가 실행되면
 * 
 * 3c9cdb10 0080ffff .여기에 x9가 위치하고 있으므로 x9의 -8만큼의 위치는
 * 
 * -----------------------------------------------------------
 *      989fdd10 0080ffff 03040000 00000000 (x9 - 8) 3c9cdb10 0080ffff
 * (x9) a09fdd10 0080ffff 03040000 00000000          d86ae010 0080ffff
 *      a89fdd10 0080ffff 03040000 00000000          4058dd10 0080ffff
 * -----------------------------------------------------------
 *  위와 같을것이다.
 * 
 * 즉 x14엔 3c9cdb10 0080ffff가 저장될것이다.
 * 다시 dump당 x9의 포지션과 x12, x13, x14값을 정리하면 다음과 같다.
 *
 * loop당
 *  |  x9위치 | x12(symbol addr)  | x13(type)         | x14(rel addr)       
 * -------+--------------------+---------------------+-----------------
 * 0|  x9    | 989fdd10 0080ffff | 03040000 00000000 | 3c9cdb10 0080ffff
 * 1|  x9    | a09fdd10 0080ffff | 03040000 00000000 | d86ae010 0080ffff
 * 2|  x9    | a89fdd10 0080ffff | 03040000 00000000 | 4058dd10 0080ffff
 *
 * 그 후 x13을 R_AARCH64_RELATIVE값과 비교해서 해당값과 맞다면
 * (R_AARCH64_RELATIVE의 hex값은 0x403이고 현재 dump는
 * little endian임으로 해당값이랑 일치하고 있다.)
 * 
 * x14에 x23(0, 혹은 조금 틀어진)을 더한후
 * x12 + x23의 주소에 x14값을 넣는, 즉 dynamic relocation이 수행된다.
 *
 * =====================================================================
 *
 * ldp : regiser 2개에 8byte씩 읽어온다.
 * x12 : x9가 가리키는 값이 저장.
 * x13 : x9 + 8이 가리키는값이 저장.
 * 계산후 x9는 #24만큼 증가.
 *
 *  ----------------------------------
 *  ldp + ldr을 표현하면 아래와 같다.
 *  x12 = [x9];
 *  x13 = [x9, 8];
 *  x9 += 24;
 *  x14 = [x9 - 8];
 *  ----------------------------------
 *
 * 그리고 relative type이 아니면 skip하고. type이 맞다면
 * x14가 가리키는곳에 x12를 저장한다.
 *
 * randomized를 하면 kernel 주소가 조금씩 틀어지는데 
 * 이 경우 x23이 조금바뀐다. 이것에 대한 보정을 해준다.
 *  
 * ---
 *
 */
	ldp	x12, x13, [x9], #24
	ldr	x14, [x9, #-8]
	cmp	w13, #R_AARCH64_RELATIVE
	b.ne	0b
	add	x14, x14, x23			// relocate
	str	x14, [x12, x23]
	b	0b

1:
#ifdef CONFIG_RELR
	/*
	 * Apply RELR relocations.
	 *
	 * RELR is a compressed format for storing relative relocations. The
	 * encoded sequence of entries looks like:
	 * [ AAAAAAAA BBBBBBB1 BBBBBBB1 ... AAAAAAAA BBBBBB1 ... ]
	 *
	 * i.e. start with an address, followed by any number of bitmaps. The
	 * address entry encodes 1 relocation. The subsequent bitmap entries
	 * encode up to 63 relocations each, at subsequent offsets following
	 * the last address entry.
	 *
	 * The bitmap entries must have 1 in the least significant bit. The
	 * assumption here is that an address cannot have 1 in lsb. Odd
	 * addresses are not supported. Any odd addresses are stored in the RELA
	 * section, which is handled above.
	 *
	 * Excluding the least significant bit in the bitmap, each non-zero
	 * bit in the bitmap represents a relocation to be applied to
	 * a corresponding machine word that follows the base address
	 * word. The second least significant bit represents the machine
	 * word immediately following the initial address, and each bit
	 * that follows represents the next word, in linear order. As such,
	 * a single bitmap can encode up to 63 relocations in a 64-bit object.
	 *
	 * In this implementation we store the address of the next RELR table
	 * entry in x9, the address being relocated by the current address or
	 * bitmap entry in x13 and the address being relocated by the current
	 * bit in x14.
	 *
	 * Because addends are stored in place in the binary, RELR relocations
	 * cannot be applied idempotently. We use x24 to keep track of the
	 * currently applied displacement so that we can correctly relocate if
	 * __relocate_kernel is called twice with non-zero displacements (i.e.
	 * if there is both a physical misalignment and a KASLR displacement).
	 */
	ldr	w9, =__relr_offset		// offset to reloc table
	ldr	w10, =__relr_size		// size of reloc table
	add	x9, x9, x11			// __va(.relr)
	add	x10, x9, x10			// __va(.relr) + sizeof(.relr)

	sub	x15, x23, x24			// delta from previous offset
	cbz	x15, 7f				// nothing to do if unchanged
	mov	x24, x23			// save new offset

2:	cmp	x9, x10
	b.hs	7f
	ldr	x11, [x9], #8
	tbnz	x11, #0, 3f			// branch to handle bitmaps
	add	x13, x11, x23
	ldr	x12, [x13]			// relocate address entry
	add	x12, x12, x15
	str	x12, [x13], #8			// adjust to start of bitmap
	b	2b

3:	mov	x14, x13
4:	lsr	x11, x11, #1
	cbz	x11, 6f
	tbz	x11, #0, 5f			// skip bit if not set
	ldr	x12, [x14]			// relocate bit
	add	x12, x12, x15
	str	x12, [x14]

5:	add	x14, x14, #8			// move to next bit's address
	b	4b

6:	/*
	 * Move to the next bitmap's address. 8 is the word size, and 63 is the
	 * number of significant bits in a bitmap entry.
	 */
	add	x13, x13, #(8 * 63)
	b	2b

7:
#endif
	ret

SYM_FUNC_END(__relocate_kernel)
#endif

/* IAMROOT, 2021.08.28:
 * - CONFIG_RELR : 일단 생략
 *
 * @x0: __cpu_setup에서 마지막에 INIT_SCTLR_EL1_MMU_ON 매크로를 통해
 *      sctrl_el1에 설정하려는 bit 값이 저장되어 있음.
 */
SYM_FUNC_START_LOCAL(__primary_switch)

#ifdef CONFIG_RANDOMIZE_BASE
/* IAMROOT, 2021.09.04:
 * - @x19에 INIT_SCTLR_EL1_MMU_ON 값을 백업하고 @x20에 sctlr_el1을 읽어온다.
 *
 * - Kconfig을 보면 CONFIG_RANDOMIZE_BASE가 enable 되었을때
 *   CONFIG_RELOCATABLE도 반드시 enable 되어야 하는 것처럼 보인다.
 *   이는 kimage addr가 randomized 되어 보정이 필요하기 때문이다.
 */
	mov	x19, x0				// preserve new SCTLR_EL1 value
	mrs	x20, sctlr_el1			// preserve old SCTLR_EL1 value
#endif

	adrp	x1, init_pg_dir
	bl	__enable_mmu

#ifdef CONFIG_RELOCATABLE
/* IAMROOT, 2021.09.06:
 * - CONFIG_RELOCATABLE
 *   AArch64의 경우 PIE(position independant execution)를 위해 런타임에 
 *   커널이 초기화될 때 리로케이션 엔트리를 갱신해야 하기위해 이 커널 옵션을
 *   사용해야 한다.
 * - CONFIG_RANDOMIZE_BASE
 *   KASLR(Kernel Address Space Layout Randomization)이라고 불리운다.
 *   커널과 모듈의 시작 위치를 랜더마이즈하게 하여 위치를 숨겨 보안을 높인다.
 *   이 커널 옵션을 사용하는 경우 __relocation_kernel: 을 한 번 더 호출한다.
 *   즉, 커널의 시작 위치가 변경되면 심볼 위치등을 모두 재 리로케이션한다.
 */
#ifdef CONFIG_RELR
	mov	x24, #0				// no RELR displacement yet
#endif
	bl	__relocate_kernel
#ifdef CONFIG_RANDOMIZE_BASE
	ldr	x8, =__primary_switched
	adrp	x0, __PHYS_OFFSET
	blr	x8

	/*
	 * If we return here, we have a KASLR displacement in x23 which we need
	 * to take into account by discarding the current kernel mapping and
	 * creating a new one.
	 */
	pre_disable_mmu_workaround
	msr	sctlr_el1, x20			// disable the MMU
	isb

/* IAMROOT, 2021.09.04:
 * - relocate를 했으므로 다시 한번 page table을 만든다.
 */
	bl	__create_page_tables		// recreate kernel mapping

	tlbi	vmalle1				// Remove any stale TLB entries
	dsb	nsh
	isb

	set_sctlr_el1	x19			// re-enable the MMU

	bl	__relocate_kernel
#endif
#endif

/* IAMROOT, 2021.09.04:
 * - 현재 MMU, d/i-cache 등 메모리 기능이 모두 enable 된 상태이며
 *   va(__primary_switched)로 점프한다.
 *
 * - 현재 상황을 정리하면 다음과 같다.
 *   __primary_switch 내부에 있다. section은 .idmap.text 이며 mmu는 on인 상태.
 *   __primary_switched는 .init.text section에 존재하고 ttbr0_el1을 통해
 *   va를 얻어 접근한다.
 *
 * - mmu on / off에 따른 pc와 ttbrX_el1의 해석
 *
 *   현재 위치인 __primary_switch 은 .idmap.texts section에 존재하며, 이 섹션은
 *   가상주소와 물리주소가 1:1 매칭이되는 영역이고 ttbr0_el1에 설정되 있다.
 *
 *   mmu의 on/off 여부에 따라 pc 값을 다음과 같이 정의할 수 있다.
 *   - mmu == off: pc는 pa를 통해 direct로 메모리에 접근한다.
 *   - mmu == on : pc는 ttbr0_el1을 통해 계산되는 va로 메모리에 접근한다.
 *
 *   '.idmap.text' 외 다른 section은 ttbr1_el1을 통해 접근하므로 addr간 간격이
 *   완전 달라지고 '.idmap.text' 와의 offset이 상당히 커진다. init_pg_dir은
 *   linear mapping 되어 있으므로 0xffff... 로 시작하는 va 이기 때문이다.
 *
 *   ttbr0_el1: idmap_pg_dir (.idmap.text) 를 바라보고 있음.
 *   ttbr1_el1: init_pg_dir (kimage) 를 바라보고 있음.
 *
 *   그러므로 __primary_switch label에서 mmu == off 라면 점프하는 label이
 *   같은 섹션(.idmap.text)에 존재하므로 b, bl을 사용해서 가깝게 점프하지만
 *   mmu == on이면 addr간 offset이 엄청나게 큰 다른 section으로 점프해야 하므로
 *   ttbr0_el1에서 ttbr1_el1으로 이동하게 되고 offset 대신 va(label) 값 자체를
 *   통해 점프한다.
 *
 *   결국 ldr 명령어를 통해 va(__primary_switched)로 점프하는 것은 다음과 같다.
 *   +-----------+-------------+-----------+-------------------------------+
 *   | 현재 위치 |    section  | ttbrX_el1 | pc                            |
 *   +-----------+-------------+-----------+-------------------------------+
 *   |  점프 전  | .idmap.text | ttbr0_el1 | ttbr0_el1의 주소 체계         |
 *   +-----------+-------------+-----------+-------------------------------+
 *   |  점프 후  | .init.text  | ttbr1_el1 | ttbr1_el1의 주소 체계         |
 *   |           |             |           | (relocate, randomized 설정됨) |
 *   +-----------+-------------+-----------+-------------------------------+
 *
 * - __primary_switch에서 mmu on/off 에 따른 b, bl 정리
 *   bl __enable_mmu      : mmu == off 이고 offset이 작아 b, bl 가능.
 *   bl __relocate_kernel : mmu == on 이지만 offset이 작아 b, bl 가능.
 *                          (같은 section)
 *   bl __create_page_tables : mmu == off 이고 offset이 작아 b, bl 가능
 *   blr __primary_switched  : mmu == on 이고 __primary_switch 와 다른 section,
 *                             addr간 offset이 매우 커 va 전체를 가져와 점프.
 *
 * - ldr, adr 차이
 *   1. 'ldr x0, label' : label에 위치한 8바이트 값을 로드한다.
 *   2. 'ldr x0, =label': psuedo instr로 compiler가 label addr를 8 bytes 크기의
 *                        임시 공간에 담아두고 읽어오는 코드를 만들어낸다.
 *                        compile-time에 label addr가 결정된다.
 *   3. 'adr x0, label' : compile-time에는 pc에서 label까지의 offset을 계산해
 *                        저장하고 runtime에 pc + offset으로 addr를 알아낸다.
 */
	ldr	x8, =__primary_switched
	adrp	x0, __PHYS_OFFSET
	br	x8
SYM_FUNC_END(__primary_switch)
