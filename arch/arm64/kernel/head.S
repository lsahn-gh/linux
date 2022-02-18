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

/*
 * IAMROOT, 2021.07.10:
 * - 해당 코드는 bootloader에 의한 최소한의 초기화 작업이후의 코드임을 알린다.
 * - KERNEL_START --> 커널 시작 가상 주소(_text)
 * - 컴파일 타임에(페이지 프레임 크기와 페이지 테이블 사용 단계) 결정
 *   -> 0xFFFF_****_****_****
 *   -> KASLR 옵션을 사용하는 경우 실제 커널 가상 주소는 런타임에 변경된다.
 */
#define __PHYS_OFFSET	KERNEL_START

#if (PAGE_OFFSET & 0x1fffff) != 0
#error PAGE_OFFSET must be at least 2MB aligned
#endif

/*
 * IAMROOT, 2021.07.10:
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
/*
 * IAMROOT, 2021.07.17:
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
 */
SYM_CODE_START(primary_entry)
	bl	preserve_boot_args
/*
 * IAMROOT, 2021.11.14:
 * - 5.10 -> 5.15 변경사항
 *   원래 el2_setup을 호출했는데 init_kernel_el으로 rename 되면서
 *   내부 루틴도 살짝 바뀜.
 */
	bl	init_kernel_el			// w0=cpu_boot_mode
/*
 * IAMROOT, 2021.08.14: 
 * - nVHE로 동작하는 경우 아래 코드는 EL2->EL1으로 변경된 채 동작한다.
 *
 * - 커널 물리주소 위치를 읽어온 후 2M 정렬단위의 하위 offset를 
 *   x23에 기록한다.
 *
 * ----
 *  - TCR : Table Control Register
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
 * - boot arguments를 boot_args로 옮긴다.
 *
 * - SYM_CODE_START_LOCAL() 해석
 *      .align 2;
 *      preserve_boot_args:
 */
SYM_CODE_START_LOCAL(preserve_boot_args)
/*
 * IAMROOT, 2022.01.26:
 * x0, x1, x2, x3으로 들어온 boot arg들을 x21, boot_args에 옮기는
 * 작업을 수행한다.
 * (primary_entry 주석에서 x21은 FDT pointer를 저장하기로 했었다.)
 *
 * 일단 x0에 있는 fdt를 x21에 옮겨 놓은 다음에 boot_args주소를 x0에 저장해놓고
 * stp 명령어를 통해서 x0, x0 + 8, x0 + 16, x0 + 24에 접근하면서
 * x21(x0), x1, x2, x3을 넣는다.
 *
 * 현재 kernel version에서 arm64는 x1, x2, x3인자는 안쓰고 있기 때문에 0으로
 * 들어와야 될것이다. (setup_arch의 마지막 boot_args 부분 참고)
 */
	mov	x21, x0				// x21=FDT

/*
 * IAMROOT, 2021.07.17:
 * x0 = &boot_args[0]
 */
	adr_l	x0, boot_args			// record the contents of
/*
 * IAMROOT, 2021.07.17:
 * str x21, boot_args
 * str x1, boot_args + 8 = boot_args[1];
 */
	stp	x21, x1, [x0]			// x0 .. x3 at kernel entry
/*
 * IAMROOT, 2021.07.17:
 *
 * str x2, boot_args[2];
 * str x3, boot_args[3];
 */
	stp	x2, x3, [x0, #16]

/*
 * IAMROOT, 2021.07.17:
 *
 * dmb: Data Memory Barrier
 * sy : Any - Any : ALL (시스템에 있는 모든 코어)
 *
 * - Modern SMP arch에서 발생할 수 있는 문제
 *   stp와 dcache_inval_poc 중 어느 것이 더 먼저 수행될 지 보장하지 못함.
 *   dcache_inval_poc가 먼저 수행되는것을 방지하고 stp 명령어의 수행 결과를
 *   보장하기 위해 memory barrier 명령어를 이용한다.
 * - 캐시와의 관계성은?
 *   x0 .. x3까지의 모든 데이터를 boot_args에 저장하고 난 뒤 확실하게
 *   정리하고, 그 이후에 데이터 캐시를 클린하도록 중간에 베리어 명령을 수행한다.
 */
	dmb	sy				// needed before dc ivac with
						// MMU off

/*
 * IAMROOT, 2021.07.17:
 * 0x20: boot_args 배열의 전체 크기, sizeof(boot_args)
 * x0: &boot_args
 * x1: boot_args의 end address
 *
 * boot_args의 크기가 20byte이니 사이즈를 계산해 start address, end address를
 * 넘긴다.
 */
	add	x1, x0, #0x20			// 4 x 8 bytes
	b	dcache_inval_poc		// tail call
SYM_CODE_END(preserve_boot_args)

/*
 * IAMROOT, 2021.08.28:
 * - pgd entry중에 하나를 그다음 테이블에 연결하기 위함.
 *   page table을 하나 추가하는 개념이 되고,
 *   현재 tbl은 PGD이고 이 다음 PUD를 잇기 위함이며 
 *   다음 page table은 tbl + PAGE_SIZE address이므로,
 *   그 주소를 가져와 해당 pud address에 해당하는 index에 값을 저장한다.
 */
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

/*
 * IAMROOT, 2021.08.21:
 * - compute_indices에서 구해온 index를 가지고,
 * 해당 table entry에 해당하는 물리주소(rtbl)의 속성을 더하고 각 table entry에 mapping하는
 * 역할을 수행한다.
 *
 * b.ls : unsgiend less or same
 * @의 의미 : macro에서 branch가 필요할경우 일반 분기문처럼 tag를 지정해줘야되는데 macro에서는
 * 그렇게 하질 못하므로 compile가 자동으로 tag의 번호를 생성해주게 한다.
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

/*
 * IAMROOT, 2021.08.21:
 * - macro에 들어온 table의 vstart, vend의 범위와 shift, ptrs를 가지고
 *   istart, iend의 index값과 해당 table의 count를 계산한다.
 *   count는 실제 entry의 개수보다 1이 작은 값이 된다.
 *
 *   ---
 *   ptrs
 *   
 *   - PGD table : entry 값 (count + 1) 이 1이 나옴.
 *       istart = (vstart >> 39) & (0x1ff)
 *       iend = (vend >> 39) & (0x1ff)
 *       count = iend - istart = 0
 *       table 크기가 작기때문에 보통 entry 값이 한개가 나옴.
 *
 *   - PUD table : shift 39값을 30으로 치환해서 계산. 역시 1개의 entry만 나옴
 *   - PMD table : shift를 21값으로 치환해서 계산. 2MB 범위 이므로 여러개가 나올수있음.
 *
 *   그래서 해당 table의 istart, iend, count의 계산 결과 값이 나온다.
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
	.endm

/*
 * IAMROOT, 2021.08.21:
 * - 가상메모리 범위의 page table level의 entry를 초기화 한다.
 *   현재 SWAPPER_PGTABLE_LEVELS은 3이므로 PGD, PMD, PTE만을 초기화 하게 된다.
 *   실제로는 PGD, PUD, PMD이지만 code의 통일성을 위해 PGD, PMD, PTE로 되어있다.
 *
 *   code(실제) | shift
 *   ---------------
 *   PGD(PGD)   : 39
 *   PMD(PUD)   : 30
 *   PTE(PMD)   : 21
 *
 *   bic : bic A, B, #C ==> A = (B & ~C)
 *
 *   PTE을 봣을때 다른것과 다르게 compute_indices와 populate_entries사이에
 *   bic를 수행하는데 혹시 모르니 2MB align을 맞추기 위한 것으로 보인다.
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
/*
 * IAMROOT, 2021.08.24:
 * PGD->PUD(or PMD, when SWAPPER_PGTABLE_LEVELS <= 3) 매핑 시작.
 * PGD, PUD, PMD의 flags가 전부 PMD_TYPE_TABLE 인 것에 주목.
 * 해당 page table entry는 section (실제 값)이 아니라 table이라는 것을 나타낸다.
 */
	sub \vend, \vend, #1
	add \rtbl, \tbl, #PAGE_SIZE
	mov \sv, \rtbl
	mov \count, #0
	compute_indices \vstart, \vend, #PGDIR_SHIFT, \pgds, \istart, \iend, \count
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
	mov \sv, \rtbl

/*
 * IAMROOT, 2021.08.24:
 * PUD->PMD 매핑 시작.
 */
#if SWAPPER_PGTABLE_LEVELS > 3
	compute_indices \vstart, \vend, #PUD_SHIFT, #PTRS_PER_PUD, \istart, \iend, \count
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
	mov \sv, \rtbl
#endif

/*
 * IAMROOT, 2021.08.24:
 * PMD->PTE 매핑 시작.
 */
#if SWAPPER_PGTABLE_LEVELS > 2
	compute_indices \vstart, \vend, #SWAPPER_TABLE_SHIFT, #PTRS_PER_PMD, \istart, \iend, \count
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
#endif

/*
 * IAMROOT, 2021.08.24:
 * PTE->Section 매핑 시작.
 * flags에 SWAPPER_MM_MMUFLAGS가 들어가는 것에 주목.
 * SWAPPER_MM_MMUFLAGS는 section type이다.
 */
	compute_indices \vstart, \vend, #SWAPPER_BLOCK_SHIFT, #PTRS_PER_PTE, \istart, \iend, \count
/*
 * IAMROOT, 2021.08.28:
 * count는 마지막 단계에서 더 이상 사용하지 않으므로 2MB 단위 정렬 후 rtbl에 사용한다.
 */
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
/*
 * IAMROOT, 2022.01.28:
 * - idmap_pg_dir, init_pg_dir을 초기화한다.
 *   idmap은 va == pa 이므로 __idmap_text_start를 pa, va 모두 같게 mapping
 *   하는게 보이고, init은 pa의 경우 adrp로 불러온 __text, va는
 *   KIMAGE_VADDR가 되므로 두 값으로 mapping한다.
 */
SYM_FUNC_START_LOCAL(__create_page_tables)
	mov	x28, lr

	/*
	 * Invalidate the init page tables to avoid potential dirty cache lines
	 * being evicted. Other page tables are allocated in rodata as part of
	 * the kernel image, and thus are clean to the PoC per the boot
	 * protocol.
	 */
/*
 * IAMROOT, 2021.08.14:
 * 커널용 페이지 테이블에 대한 캐시를 모두 invalidate
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
 *   pc + 0x4000로 init_pg_dir을 load할 것이다.
 *   즉 현재 pc와 상관없이 offset만으로. 즉 pc와의 상대주소로 sympbol 주소를
 *   알수 있을것이다.
 *
 *   head.S는 이렇게 pc-relative만으로 구성되어 pc가, 즉 첫 시작 주소가
 *   어디서 시작하든 상대적으로 계산되어 symbol을 불러올수있으므로
 *   시작 주소가 상관없다는것이고 이게 head.S의 첫 주석에서 말한
 *   position independent의 의미이다.
 */
	adrp	x0, init_pg_dir
	adrp	x1, init_pg_end
	bl	dcache_inval_poc

	/*
	 * Clear the init page tables.
	 */
/*
 * IAMROOT, 2021.08.14: 커널용 페이지 테이블을 0으로 모두 초기화한다.
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

/*
 * IAMROOT, 2021.08.14: 커널 매핑에 필요한 디폴트 속성값을 가져온다.
 *
 * 디폴트 속성값을 요약하면 다음과 같다.
 * - Normal memory
 * - Section type
 * - Access Flag
 * - Inner Share
 */
	mov	x7, SWAPPER_MM_MMUFLAGS

	/*
	 * Create the identity mapping.
	 */
	adrp	x0, idmap_pg_dir
	adrp	x3, __idmap_text_start		// __pa(__idmap_text_start)

/*
 * IAMROOT, 2021.08.14: 
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
/*
 * IAMROOT, 2021.08.14: 
 * - VA 확장이 필요한지 점검한다.
 *   __idmap_text_end의 주소를 물리주소로 알아와서 이 주소의 선두 비트들이 0인
 *   개수를 알아와서 TCR_T0SZ(VA_BITS=예:39)=25와 비교한다.
 *   비교하여 물리 주소에 사용한 0이 더 많은 경우 가상 주소 크기가 충분하다.
 *   이 경우 가상 주소 영역의 확장이 필요 없으니  1f 레이블로 이동한다.
 *   확장이 필요한 경우 __idmap_text_end 물리주소에서 산출된 선두 0의 개수를
 *   idmap_t0sz에 저장한다.
 */
	adrp	x5, __idmap_text_end
	clz	x5, x5
	cmp	x5, TCR_T0SZ(VA_BITS_MIN) // default T0SZ small enough?
	b.ge	1f			// .. then skip VA range extension

	adr_l	x6, idmap_t0sz
	str	x5, [x6]
	dmb	sy
	dc	ivac, x6		// Invalidate potentially stale cache line

/*
 * IAMROOT, 2021.08.21:
 * - VA < 48
 *   table table level 을 확장.
 * - VA >= 48
 *   table table을 늘리기만 하는데 PGD table을
 *   PHYS_MASK_SHIFT - PGDIR_SHIFT만큼 늘렸다.
 *   (48 - 39 = 2^9 = 512)
 * - PGDIR_SHIFT의 주석에 써놓는걸 참조하면 EXTRA_SHIFT가 vabits가 안 맞는
 *   경우가 있다.(64k page size, vabits 48 인 경우
 */
#if (VA_BITS < 48)
#define EXTRA_SHIFT	(PGDIR_SHIFT + PAGE_SHIFT - 3)
#define EXTRA_PTRS	(1 << (PHYS_MASK_SHIFT - EXTRA_SHIFT))

	/*
	 * If VA_BITS < 48, we have to configure an additional table level.
	 * First, we have to verify our assumption that the current value of
	 * VA_BITS was chosen such that all translation levels are fully
	 * utilised, and that lowering T0SZ will always result in an additional
	 * translation level to be configured.
	 */
#if VA_BITS != EXTRA_SHIFT
#error "Mismatch between VA_BITS and page size/number of translation levels"
#endif

	mov	x4, EXTRA_PTRS
	/*
	 * IAMROOT, 2021.08.28:
	 * VA_BITS 가 39라고 가정
	 *
	 * tbl  : x0 -> idmap_pg_dir
	 * virt : x3 -> __idmap_text_start
	 * shift : EXTRA_SHIFT -> 30 + 12 - 3 = 39
	 * ptrs : EXTRA_PTRS -> 2^(9) = 512
	 * x5, x6 : temp
	 */
	create_table_entry x0, x3, EXTRA_SHIFT, x4, x5, x6
#else
	/*
	 * If VA_BITS == 48, we don't have to configure an additional
	 * translation level, but the top-level table has more entries.
	 */
	mov	x4, #1 << (PHYS_MASK_SHIFT - PGDIR_SHIFT)
	str_l	x4, idmap_ptrs_per_pgd, x5
#endif
1:
	ldr_l	x4, idmap_ptrs_per_pgd
	adr_l	x6, __idmap_text_end		// __pa(__idmap_text_end)

/*
 * IAMROOT, 2021.08.21:
 *   ---
 *   .macro map_memory, tbl, rtbl, vstart, vend, flags, phys, pgds,
 *   istart, iend, tmp, count, sv
 *
 *   idmap은 va == pa라는것을 유념
 *
 *         | dir | corrupt  | idmap                         | init_pg_dir
 *  tbl    | i/o | corrupt  | x0 : __pa(idmap_pg_dir)       | x0 : __pa(init_pg_dir)
 *  rtbl   | o   | corrupt  | x1 : temp register            | x1 : temp register
 *  vstart | i   | preserve | x3 : __pa(__idmap_text_start) | x5 : __va(_text)(= KIMAGE_VADDR) + KASLR
 *  vend   | i   | preserve | x6 : __pa(__idmap_text_end)   | x6 : __va(_end)
 *  flags  | i   | preserve | x7 : SWAPPER_MM_MMUFLAGS
 *  phys   | i   | preserve | x3 : __pa(__idmap_text_start) | x3 : __pa(_text)
 *  pgds   | i   | preserve | x4 : idmap_ptrs_per_pgd       | x4 : PTRS_PER_PGD
 *  ---------------------------------------------------------------------------
 *  istart | o   | corrupt  | x10 : vstart에 대응하는 table index
 *  iend   | o   | corrupt  | x11 : vend에 대응하는 table index
 *  tmp    | o   | corrupt  | x12 : 내부 계산용
 *  count  | io  | corrupt  | x13 : extra count(iend - istart)
 *  sv     | o   | corrupt  | x14 : 내부 계산용
 *
 *   x10부터는 temp register.
 *   ---
 *
 * - idmap table 크기와 init_pg_dir table크기를 집고 넘어가보면
 *   둘다 compile time에 크기가 정해지고 idmap table이 init_pg_dir에 비해
 *   상대적으로 작은 size를 가진다.
 *
 * IAMROOT, 2021.08.24:
 * Identity Mapping 시작.
 *
 * x0: __pa(idmap_pg_dir)
 * x1: temp register
 * x3: __pa(__idmap_text_start)
 * x6: __pa(__idmap_text_end)
 * x7: SWAPPER_MM_MMUFLAGS
 * x3: __pa(__idmap_text_start)
 * x4: idmap_ptrs_per_pgd
 * x10 ~ x14: temp register
 */
	map_memory x0, x1, x3, x6, x7, x3, x4, x10, x11, x12, x13, x14

	/*
	 * Map the kernel image (starting with PHYS_OFFSET).
	 */
	adrp	x0, init_pg_dir
	mov_q	x5, KIMAGE_VADDR		// compile time __va(_text)
	add	x5, x5, x23			// add KASLR displacement
	mov	x4, PTRS_PER_PGD
/*
 * IAMROOT, 2021.08.21:
 * - kernel의 가상주소의 vend를 구해야한다.
 *   vstart는 바로위 코드 x5를 통해 구했고.
 *   _end(가상주소)를 x6으로 adrp를 통해 물리 주소를 알아온다.
 *   _text(가상주소)를 x3으로 adrp를 통해 물리 주소로 알아온다.
 *   x3과 x6의 차이는 kernel이미지의 크기이미지이므로,
 *   kernel 가상주소 start은 x5에 해당 크기를 더하면
 *   kernel 가상주소의 end값이 구해진다.(x6)
 */
	adrp	x6, _end			// runtime __pa(_end)
	adrp	x3, _text			// runtime __pa(_text)
	sub	x6, x6, x3			// _end - _text
	add	x6, x6, x5			// runtime __va(_end)

/*
 * IAMROOT, 2021.08.24:
 * Linear Mapping 시작.
 *
 * x0: __pa(init_pg_dir)
 * x1: temp register
 * x5: __va(_text)
 * x6: __va(_end)
 * x7: SWAPPER_MM_MMUFLAGS
 * x3: __pa(_text)
 * x4: PTRS_PER_PGD
 * x10 ~ x14: temp register
 *
 * > 연습문제
 *  vend  : 0xffff_0000_11f3_f000
 * vstart : 0xffff_0000_1008_0000
 * ------------------------------
 * = 0x01eb_f000 (kimage size: 32MB)
 *
 * - PGD: shift(39), ptrs(512), count(0)
 *   iend   = vend   >> 39 = 0x01ff_fe00 & 0x2ff(ptrs) = 0x0100
 *   iend  += count * ptrs = 0
 *   istart = vstart >> 39 = 0x01ff_fe00 & 0x2ff       = 0x0100
 *   count  = iend - istart = 0
 *   PGD table: 0x100번 엔트리만 사용
 *
 * - PUD: shift(30), ptrs(512), count(0)
 *   iend   = vend   >> 30 = 0xfffc_0000 & 0x2ff = 0x0000
 *   iend  += count * ptrs = 0
 *   istart = vstart >> 30 = 0xfffc_0000 & 0x2ff = 0x0000
 *   count  = iend - istart = 0
 *   PUD table: 0번 엔트리만 사용
 *
 * - PMD: shift(21), ptrs(512), count(0)
 *   iend   = vend   >> 21 = 0xf800_008f & 0x2ff = 0x008f
 *   iend  += count * ptrs = 0
 *   istart = vstart >> 21 = 0xf800_0080 & 0x2ff = 0x0080
 *   count  = iend - istart = 15
 *   PMD table: 0x80 ~ 0x8f번까지의 엔트리만 사용
 *
 * 최종 엔트리들이 가르키는 것은 2MB 단위로 정렬된 블럭
 */
	map_memory x0, x1, x5, x6, x7, x3, x4, x10, x11, x12, x13, x14

/*
 * IAMROOT, 2021.08.21:
 * - 위에 명령어들은 결국 load/store이고 해당 명령어들이 전부 완료되게
 *   알려주는 역할
 */
	/*
	 * Since the page tables have been populated with non-cacheable
	 * accesses (MMU disabled), invalidate those tables again to
	 * remove any speculatively loaded cache lines.
	 */
	dmb	sy

/*
 * IAMROOT, 2021.08.21:
 * - 위에서 memory mapping을 다했으므로 해당 memory의 cache를 전부 한번
 *   정리해주는 코드
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
/*
 * IAMROOT, 2021.09.04:
 * - sp, sp_elX의 차이점
 *   https://stackoverflow.com/questions/65059491/why-save-init-task-struct-address-to-sp-el0-in-arm64-boot-code-primary-switche
 *   
 *   위 링크에서는 다음과 같은 예제를 들어준다.
 *
 *   movz x1, 0x1000
 *   movz x2, 0x2000
 *   msr sp_el0, x1
 *   msr sp_el1, x2
 *   msr spsel, 0
 *   add x3, sp, 0x10
 *   msr spsel, 1
 *   add x4, sp, 0x20
 *   
 *   sp_el0, sp_el1에 user, kernel에서 사용할 stack 주소를 넣어주고
 *   spsel을 통해서 sp를 뭘쓸지 정해주면 sp가 자동으로 sp_el0, sp_el1에
 *   설정했던 주소로 바뀌는 모습이다.(링크에서는 spsel뿐만 아니라 excetpion이
 *   발생하면 자동으로 바꿔준다는것처럼 설명되있다.)
 *
 *   즉 sp는 다른 레지스터처럼 ldr, str등의 제어를 직접할 수 있는 레지스터이고
 *   sp_elX는 user, kernel에서 사용할 sp를 저장해놓는 공간이된다.
 *   하지만 kernel에서는 sp_el0를 위처럼 사용하진 않는다.
 *
 * sp_el0:
 * - sp_el0는 유저 공간으로 context switch 된 후 유저용 스택 위치를
 *   가리키는 용도로 사용된다. 그러나 커널(el1)에서는 사용하지 않는 
 *   스크래치 레지스터 용도일 뿐이므로 이를 활용하여 thread_info를 가리키는 
 *   레지스터로 사용합니다.
 * - 참고: https://github.com/torvalds/linux/commit/6cdf9c7ca687e01840d0215437620a20263012fc
 *
 * - sp_el0는 유저에서 커널 모드 들어올 때 현재 태스크를 저장하고 커널 모드에서
 *   현재 thread_info 를 빠르게 알아내는 용도로 사용되는데 사용된다.
 *   커널모드에서 sp_el0는 스택과는 관련이 없다.
 *   커널모드 스택은 sp 니모닉으로 바로 접근하며 이때 sp는 sp_el1이다.
 *
 * - get_current 함수 참고
 *
 * scs(Shadow Call Stack)
 * - 함수반환주소 overflow를 방지하는 기능.
 */
	msr	sp_el0, \tsk

	ldr	\tmp1, [\tsk, #TSK_STACK]
	add	sp, \tmp1, #THREAD_SIZE
	sub	sp, sp, #PT_REGS_SIZE

	stp	xzr, xzr, [sp, #S_STACKFRAME]
	add	x29, sp, #S_STACKFRAME

	scs_load \tsk

/*
 * IAMROOT, 2022.02.07:
 * - 현재 cpu번호를 tpidr register에 저장한다.
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
/*
 * IAMROOT, 2021.09.04:
 * ---- (old 5.10)
 * - EL1 SP(Stack Pointer) 설정
 *   init_thread_union의 source 상 위치
 *   vmlinux.lds.S -> RW_DATA -> INIT_TASK_DATA -> init_thread_union
 *
 *   task 영역을 stack으로 사용하기 위해 task address를 가져온다.
 *
 * - Memory Layout
 *   
 *   (높은 주소)
 *   ..
 *   ..
 *   ..
 *   -----------------
 *   (end)   - init_thread_union, stack <- SP (Stack Pointer)
 *   THREAD_SIZE (Stack Size, 16k: CONFIG_KASAN off)
 *   (start) - init_thread_union, stack
 *   -----------------
 *   (낮은 주소)
 *
 * - 참고 : Documentation/x86/kernel-stacks.rst 
 *   -----
 */
	adr_l	x4, init_task
	init_cpu_task x4, x5, x6

/*
 * IAMROOT, 2021.09.04:
 * - kernel용 vector table 주소 설정.
 * - vectors 위치 : kernel/entry.S
 */
	adr_l	x8, vectors			// load VBAR_EL1 with virtual
	msr	vbar_el1, x8			// vector table address
	isb

/*
 * IAMROOT, 2021.09.04:
 *  stack_push(x29), stack_push(x30) 과 동일 의미
 *
 *  sp(before)
 *  ----------------
 *  (sp -= 16)
 *  sp - 16 : x29
 *  sp - 8  : x30
 *  ----------------
 *  sp(current)
 */
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

/*
 * IAMROOT, 2021.09.04:
 * - 처음 부팅시 부트로더부터 가져온 fdt 주소를 x21에 backup 해놨었는데
 *   해당 값을 __fdt_pointer에 저장함
 */
	str_l	x21, __fdt_pointer, x5		// Save FDT pointer

/*
 * IAMROOT, 2021.09.04:
 * 
 * - kimage_voffset = kimage_vaddr - __PHYS_OFFSET
 *   kimage_vaddr은 va(_text)이다. 그리고 __PHYS_OFFSET은 pa(_text)이므로
 *
 *   kimage_voffset = va(_text) - pa(_text)
 *
 *   즉 kimage_voffset은 가상주소에서 물리주소 변환을 위한 offset을 의미한다.
 *
 *   여기서 relocate 를 한다면 va(_text)는 relocate 된 va(_text)가 될것이다.
 *
 * - CONFIG_RANDOMIZE_BASE on시의 kimage_voffset
 *   CONFIG_RANDOMIZE_BASE가 on이 되면 __primary_switched를 총 2번 진입하는데
 *
 *   첫번째 진입할때에는 randomized가 되기 전인 relocate만 한 va(_text)이고,
 *   이후 뒷 code에서 kaslr_early_init의 결과값으로 주어진 random offset을
 *   사용해서 다시 relocate된 kimage_vaddr을 사용해서 offset을 다시 구한다.
 */
	ldr_l	x4, kimage_vaddr		// Save the offset between
	sub	x4, x4, x0			// the kernel virtual and
	str_l	x4, kimage_voffset, x5		// physical mappings

/*
 * IAMROOT, 2021.09.04:
 * - 보통 일반 application을 동작시킬시에는 loader가 bss를 초기화 시켜주지만
 *   kernel은 자신이 초기화한다.
 *   memset에 대한 instruction을 내리고 해당 instruction이(store)끝날때까지
 *   기다린다.
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
	mov	x0, x21				// pass FDT address in x0
	bl	early_fdt_map			// Try mapping the FDT early
	bl	init_feature_override		// Parse cpu feature overrides
/*
 * IAMROOT, 2021.09.04:
 * - CONFIG_RANDOMIZE_BASE가 on이면 __primary_switch에서 __primary_switched
 *   가 2번을 호출하고 첫번째에는 ret을 통해 빠져나가고 두번째 호출에는
 *   tst를 통해 0번 label로 이동해서 start_kernel을 실행하는 구조이다.
 */
#ifdef CONFIG_RANDOMIZE_BASE
	tst	x23, ~(MIN_KIMG_ALIGN - 1)	// already running randomized?
	b.ne	0f
	bl	kaslr_early_init		// parse FDT for KASLR options
	cbz	x0, 0f				// KASLR disabled? just proceed
	orr	x23, x23, x0			// record KASLR offset
	ldp	x29, x30, [sp], #16		// we must enable KASLR, return
	ret					// to __primary_switch()
0:
#endif
/*
 * IAMROOT, 2021.09.10:
 * - CONFIG_RELOCATABLE, CONFIG_RANDOMIZE_BASE on시
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
/*
 * IAMROOT, 2021.09.04:
 * 위 코드에서 stack에 값2개를 push했었지만 CONFIG_RANDOMIZE_BASE가
 * off라고 했을때 해당값을 사용안하고 그냥 버린다.
 *
 * - PCS에 따르면 x29는 fp(frame pointer), x30은 lr(link register)를 사용한다고
 *   하며, 해당 값들을 0으로 초기화한다.
 *
 * To avoid backtracing using PCs that fall in the idmap, or are controlled
 * by userspace, we must explcitly zero the FP and LR in startup paths, and
 * must ensure that the frame embedded in pt_regs is zeroed upon entry from
 * EL0. To avoid these NULL entries showin in the backtrace, unwind_frame()
 * is updated to avoid them.
 */
	bl	switch_to_vhe			// Prefer VHE if possible
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

/*
 * IAMROOT, 2022.01.27:
 * - CurrentEL 1 -> EL1
 * - CurrentEL 2 and nVHE - > EL1
 * - CurrentEL 2 and VHE - > EL2
 *   EL2로 부팅을하여 VHE를 지원시 원래는 EL1 NVHE로 동작을 해야되는데
 *   아래 주석에 있다 시피 일부 Fruity cpu (ex M1. Git blame 참고)에서
 *   EL1에서 nVHE로 동작을 못하는 현상이 있다. 그래서 EL2 VHE로
 *   여기서 초기화를 시킨다.
 */
SYM_FUNC_START(init_kernel_el)
/*
 * IAMROOT, 2021.07.24:
 * - 현재 EL 을 x0 로 가져온다.
 */
	mrs	x0, CurrentEL
/*
 * IAMROOT, 2021.07.24:
 * - CurrentEL_EL2 는 8 로 정의됨
 */
	cmp	x0, #CurrentEL_EL2
/*
 * IAMROOT, 2021.11.14:
 * - 현재 EL이 EL2면 init_el2를 실행한다.
 *   그렇지 않으면 바로 아래 init_el1으로 내려가게된다.
 */
	b.eq	init_el2

/*
 * IAMROOT, 2021.07.24:
 * @return x0 BOOT_CPU_MODE_EL1
 * - CurrentEL_EL1 실행되는 코드
 *   모든 interrupt를 disable하고 boot mode가 el1이라는것을 설정한다.
 * - spsr_el1에 el1 stack 으로 설정한다.
 */
SYM_INNER_LABEL(init_el1, SYM_L_LOCAL)
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr	sctlr_el1, x0
	isb
	mov_q	x0, INIT_PSTATE_EL1
	msr	spsr_el1, x0
	msr	elr_el1, lr
/*
 * IAMROOT, 2021.11.14:
 * - cpu boot 가 EL1이라는것.
 * - elr_el1
 *   eret호출시 jump할 주소를 저장하는 register
 * - eret
 *   exception이 발생한 ELx의 spsr이 pstate로 복원되며 ELR_ELx에 저장된
 *   address로 점프하고 pstate가 spsr로 된다.
 *   ex) currentEL == 1 이면 spsr_el1, elr_el1 접근
 *       currentEL == 2, nVHE이면 spsr_el2, elr_el2 접근
 *
 * - el1 -> el1으로 이동하는 개념인데, 변화가 없는것 같지만 eret을 통해
 *   pstate가 일관되게 초기화되는 효과가 있다고한다.
 *   그래서 eret을 한번 써주는것이다.
 */
	mov	w0, #BOOT_CPU_MODE_EL1
	eret
/*
 * IAMROOT, 2021.11.14:
 * - 현재 Exception Level은 EL2
 * - 일단 NVHE 로 hcr_el2로 시작을하여, el2 관련 초기화를 진행한다.
 * - hcr_el2
 *   Hypervisor Configuration Register. 가상화 설정 관련 register.
 */
SYM_INNER_LABEL(init_el2, SYM_L_LOCAL)
	mov_q	x0, HCR_HOST_NVHE_FLAGS
	msr	hcr_el2, x0
/*
 * IAMROOT, 2021.07.24:
 * - isb: Instruction Synchronization Barrier.
 *   파이프라인을 비운다.
 *   엔디안 설정이 끝나면 파이프라인을 비워서
 *   바뀐 엔디안으로 실행되도록 한다.
 */
	isb
/*
 * IAMROOT, 2021.11.14:
 * - arch/arm64/include/asm/el2_setup.h
 */
	init_el2_state

/*
 * IAMROOT, 2021.08.14: EL2용 하이퍼 벡터 주소를 vbar_el2 레지스터에 기록
 * - vbar_el2
 *   el2에서 exception 발생시 점프할 address 저장
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
/*
 * IAMROOT, 2021.11.14:
 * - E2H : EL2 Host. Enables a configuration where a Host Operating System
 *   is running in EL2, and the Host Operating System's applications are
 *   running in EL0.
 *
 * - HCR_E2H이 clear됬는지 확인한다. 원래 대로라면 위에 HCR_HOST_NVHE_FLAGS
 *   로 설정하면서 clear됬어야됬는데 몇몇 cpu에서 이게 안된다고 한다.
 *   HCR_E2H가 clear됬으면 일반 동작(1f)로 이동하고 그게 아니면 stub로 동작한다.
 */
	mrs	x0, hcr_el2
	and	x0, x0, #HCR_E2H
	cbz	x0, 1f
/*
 * IAMROOT, 2021.11.14:
 * - 원래는 vhe mode로 나중에 전환 하지만 HCR_E2H가 clear안되는 cpu들은
 *   바로 vhe mode로 전환할 준비를 한다. HCR_E2H가 set되있으면 el1 관련
 *   register에 접근해도 el2 register에 접근하는 것처럼 되기 때문에
 *   문제가 발생하기 때문이다.
 * - el1에서 설정한 값이 el2에서도 똑같이 동작가능하도록
 *   SYS_SCTLR_EL12에 설정하는것이 보인다. 일단 vhe를 하기전에 무조건
 *   해당 register를 초기화해준다. hvc 동작시 해당 register를 사용한다.
 */
	/* Switching to VHE requires a sane SCTLR_EL1 as a start */
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr_s	SYS_SCTLR_EL12, x0

	/*
	 * Force an eret into a helper "function", and let it return
	 * to our original caller... This makes sure that we have
	 * initialised the basic PSTATE state.
	 */
/*
 * IAMROOT, 2021.11.14:
 * - 원래는 EL2 mode에서 eret을 하면 elr_el2로 접근을 하는데
 *   HCR_E2H가 set되있으므로 elr_el1을 설정해도 elr_el2가
 *   elr_el1처럼 동작할것이다.
 * - spsr_el2에는 el1 복귀로 넣어놨으므로 eret시 el1이 되면서 __cpu_stick_to_vhe
 *   로 진입할것이다.
 * - PSTATE 초기화를 위해 spsr_el1에도 초기값을 넣어둔다.
 */
	mov	x0, #INIT_PSTATE_EL2
	msr	spsr_el1, x0
	adr	x0, __cpu_stick_to_vhe
	msr	elr_el1, x0
/*
 * IAMROOT, 2021.11.14:
 * - el1로 전환을 한다. elr_el1에서 __cpu_stick_to_vhe를 넣어놧으므로
 *   그쪽으로 branch가 될것이다.
 *   (현재 el2 mode에서, spsr_el2를 el1으로 __init_el2_nvhe_prepare_eret에서
 *   설정해놨으므로 eret을 통해 el1가 된다.)
 * ---
 *  1) eret 진입전
 *  Current EL : EL2
 *  spsr_el1 : INIT_PSTATE_EL2
 *  spsr_el2 : INIT_PSTATE_EL1 
 *  elr_el1  : __cpu_stick_to_vhe
 *  PSTATE   : (아직 초기화안됨)
 *
 * 2) eret 진입 직후 (__cpu_stick_to_vhe 진입)
 * Current EL : EL1 (spsr_el2에 EL1이 되있으므로 EL2 -> EL1)
 * PSTATE     : INIT_PSATE_EL1
 *
 * 3) __cpu_stick_to_vhe 완료후
 * Current EL : EL2 (hvc를 통해서 EL1 -> EL2)
 * PSTATE     : INIT_PSATE_EL2
 */
	eret

/*
 * IAMROOT, 2021.11.14:
 * - el1(nvhe)로 start할 준비를 한다.
 *   후에 switch_to_vhe에서 el2로 전환될것이다.
 */
1:
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr	sctlr_el1, x0

	msr	elr_el2, lr
	mov	w0, #BOOT_CPU_MODE_EL2
/*
 * IAMROOT, 2021.11.14:
 * - init_el2_state에서 __init_el2_nvhe_prepare_eret에서 el1으로
 *   복귀하도록 설정했엇을것이다. w0에 cpu가 el2로 일단 부팅을
 *   했다는걸 저장하고 실제 eret을 통해 el1으로 mode가 변경될것이다.
 *   elr_el2에 lr을 넣어놨으므로 lr로 바로 가게 될것이다.
 */
	eret

__cpu_stick_to_vhe:
/*
 * IAMROOT, 2021.11.14:
 * - 원래는 vhe를 지원해도 el1으로 동작하다가 switch_to_vhe을 했어야 됬는데
 *   일부 cpu의 문제로 여기서 el2로 전환시킨다.
 *
 * --- hvc(Hypervisor Call)
 *
 * - el1 -> el2을 하기위해 Synchronous exception을 발생시킨다.
 *   __hyp_stub_vectors의 (vbar_el2에 set했었다.) elx_sync가 호출된다.
 *   (hvc의 #0는 system에서 무시되는것처럼 보이는데 필요에 따라 인자로
 *   쓸수있는 듯하다)
 *   PSTATE는 hvc가 발생한(el2에서 발생하면 el2->el2가 되는거같다.)
 *   spsr_elx에 저장을 해놓고, pc도 elr_elx에 저장을 해놓고 hvc가 끝나면
 *   다시 복귀시킨다.
 *   
 *   1. hvc 실행 : pc -> elr_el1, PSTATE -> spsr_el1
 *   2. hvc(sync vector 동작)
 *   3. hvc 종료(vector에서 eret을 통해 동작) : elr_el1 -> pc, spsr_el1 -> PSTATE
 *
 * - x0를 hvc 특별 인자로 사용한다. HVC_SET_VECTORS의 주석 참고
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
/*
 * IAMROOT, 2021.08.14: 
 * - w0로 알아온 부트(el1 or el2) 모드 값을 저장한다.
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
/*
 * IAMROOT, 2021.08.14: 
 * - 지시어 .long은 4바이트 값이다.
 *   set_cpu_boot_mode_flag 함수가 아래 값을
 *   부팅한 모드(el1 or el2) 값으로 동일해지도록 기록한다.
 *
 * - 해당 데이터는 '.mmuoff.data.write' section에 저장되며 아래 C 코드와
 *   유사한 의미를 가진다. (bss section이 아님에 유의하자)
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

/*
 * IAMROOT, 2021.08.28:
 * - status를 저장한다.
 *   status가 0이면 정상이다.
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
/*
 * IAMROOT, 2021.08.28:
 * - Config 에 따른 Granule를 아키텍처가 지원하는지를 확인해서
 *   지원하지 않으면 __no_granule_support에서 무한루프 돌도록 하며
 *   지원하면 정상임을 의미하는 status(0)를 __early_cpu_boot_status에 저장한다.
 *   early startup에서는 H/W debugger 이용하여 status 코드를
 *   확인할 수 있다.
 *
 * - TGRAN  : Translation Granule Size.
 * - TGRAN4 : 4KB TGRAN
 * - TGRAN16: 16KB TGRAN
 * - TGRAN64: 64KB TGRAN
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
/*
 * IAMROOT, 2021.08.28:
 * - x1 : init_pg_dir의 ttbr
 *   x2 : idmap_pg_dir의 ttbr
 * - ttbr0_el1 : user용이지만 부팅중에는 임시로 idmap으로 사용한다.
 *   ttbr1_el1 : kernel용이며 init_pg_dir을 가리킨다.
 */
	msr	ttbr0_el1, x2			// load TTBR0
	offset_ttbr1 x1, x3
	msr	ttbr1_el1, x1			// load TTBR1
	isb

/*
 * IAMROOT, 2021.08.28:
 * - x0에 설정된 bits를 sctlr_el1에 set하며 MMU enable, D/I-cache 등
 *   메모리와 관련된 기능을 on 한다.
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

/*
 * IAMROOT, 2021.08.28:
 * - 아키텍처가 지원하지 않은 Granule를 설정하게 할려고 했기때문에
 *   status를 error bit로 설정하고 무한 wait로 들어간다.
 *
 *   후에 하드웨어 디버깅을 할때 해당 변수를 확인하면 무슨 error 보게 하기
 *   위함이다. (해당 변수: __early_cpu_boot_status)
 *
 *   wfe : wait for event
 *   wfi : wait for interrupt
 *   너무 빠르게 계속 동작하지 않게 하기 위함(전력 소비를 줄이기위함)
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

/*
 * IAMROOT, 2021.08.28:
 * - CONFIG_RELR : 일단 생략
 *
 * x0: __cpu_setup에서 제일 마지막에 INIT_SCTLR_EL1_MMU_ON define을 이용하여
 *     sctrl_el1에 설정하려는 bits 값이 들어 있음.
 *     x0 값은 sctrl_el1에서 읽은것이 아니라 bitmask를 통해 하드코딩으로
 *     설정된 값.
 */
SYM_FUNC_START_LOCAL(__primary_switch)
/*
 * IAMROOT, 2021.09.04:
 * - CONFIG_RANDOMIZE_BASE를 살펴보면 해당 CONFIG가
 *   on시 반드시 CONFIG_RELOCATABLE도 on이 되야함이 보인다.
 *   kernel 주소가 randomized가 됬으므로 그에 따른 보정이 반드시 필요하기
 *   때문이다.
 */
#ifdef CONFIG_RANDOMIZE_BASE
/*
 * IAMROOT, 2021.09.04:
 * - x19에 sctrl_el1 값을 backup하고 설정되있던 sctlr_el1값을 가져온다.
 */
	mov	x19, x0				// preserve new SCTLR_EL1 value
	mrs	x20, sctlr_el1			// preserve old SCTLR_EL1 value
#endif

	adrp	x1, init_pg_dir
	bl	__enable_mmu
/*
 * IAMROOT, 2021.09.06: 
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
#ifdef CONFIG_RELOCATABLE
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
/*
 * IAMROOT, 2021.09.04:
 * - relocate를 했으므로 다시 한번 page table을 만든다.
 */
	bl	__create_page_tables		// recreate kernel mapping

/*
 * IAMROOT, 2022.02.07:
 * - tlbi(TLB Invalidate)
 */
	tlbi	vmalle1				// Remove any stale TLB entries
	dsb	nsh
	isb

	set_sctlr_el1	x19			// re-enable the MMU

	bl	__relocate_kernel
#endif
#endif
/*
 * IAMROOT, 2021.09.04:
 * - ldr, adr 차이
 *   1) ldr x0, label
 *      label에 위치한 8바이트 값을 로드한다.
 *   2) ldr x0, =label
 *      pesudo-insutruction 으로 컴파일러가 label 주소를 8바이트에 담아두고,
 *      이를 읽어오는 코드를 만들어낸다. 즉, 컴파일 타임의 label 주소를 알아온다.
 *   3) adr x0, label
 *      compile time에는 pc에서 label까징의 offset을 계산해 저장해 놓고,
 *      런타임에는 런타임 당시 pc + offset으로 주소를 알아온다.
 *
 * ---------------------------------------------------------------------------
 *
 * - 현재 상황을 정리하면 다음과 같다.
 *   - 현재 __primary_switch내부에 있다. section은 .idmap.text
 *   - mmu는 on or off인 상태
 *   - __primary_switched는 .init.text section에 존재한다.
 *   - mmu가 on인 경우 ttbr0_el1에서 동작
 *
 * - mmu on / off에 따른 pc와 ttbrX_el1의 해석
 *
 *   현재 위치인 __primary_switch 은 .idmap.texts section에 존재하며, 이 섹션은
 *   가상주소와 물리주소가 1:1 매칭이되는 영역이고 ttbr0_el1에 설정되 있다.
 *
 *   mmu가 on / off에 따라 pc의 정의를 다음과 같이 생각할수 있다.
 *
 *   mmu가 off : 여지껏 그래왔던 것처럼 pc는 물리주소인 상태,
 *   mmu가 on : pc는 ttbrX_el1으로 계산되는 mmu를 거친 주소.
 *   
 *   즉 .idmap.text외에 다른섹션은 ttbr1_el1에 연결되므로 주소값이 완전 다른
 *   체계를 가지되 .idmap.text와의 offset이 매우 커진다.
 *
 *   ttbr0_el1 : idmap_pg_dir(.idmap.text)가 연결되어있음.
 *   ttbr1_el1 : init_pg_dir(kernel image)가 연결되있음.
 *
 *   그러므로 __primary_switch에서 mmu가 off인 상태거나 점프하는 함수가
 *   같은 섹션(.idmap.text)이라면 b, bl을 사용해서 그냥 점프하면 되지만,
 *   mmu가 on이되면 에 다른 섹션으로 점프하는 경우에는 ttbr1_el0에서
 *   ttbr1_el1으로 이동을 해야되므로 offset을 사용하지 말고 해당 함수의
 *   가상주소값 그자체를 가져와서 점프를 하여 ttbr1_el1으로 이동하는 개념으로
 *   사용해야된다.
 *
 * 즉 ldr 을 사용해서 __primary_switched로 넘어가는 것은 다음과 같이 정리된다.
 *
 * 현재 위치 ||    section  | ttbrX_el1 | pc 
 * ----------++------------+-----------+--------------------------------
 *  점프 전  || .idmap.text | ttbr0_el1 | ttbr0_el1의 주소 체계
 * ----------++------------+-----------+--------------------------------
 *  점프 후  || .init.text  | ttbr1_el1 | ttbr1_el1의 주소 체계
 *           ||             |           | (relocate, randomized 설정됨)
 * ----------++------------+-----------+--------------------------------
 *
 * - __primary_switch에서 mmu on /off에 따른 b, bl정리
 *   bl __enable_mmu : mmu off인상태이므로 offset이 작기때문에 b, bl가능
 *   bl __relocate_kernel : mmu on이지만 offset이 작기때문에
 *                          (section이므로) b, bl 가능
 *   bl __create_page_tables : mmu off상태이고 offset이 작아
 *                             b, bl 사용가능
 *   blr x8(__primary_switched 점프) : mmu on상태에서 __primary_switch가 다른
 *    섹션에 위치하므로 offset이 매우커 가상주소 전체를 불러와서 점프.
 */
	ldr	x8, =__primary_switched
	adrp	x0, __PHYS_OFFSET
	br	x8
SYM_FUNC_END(__primary_switch)
