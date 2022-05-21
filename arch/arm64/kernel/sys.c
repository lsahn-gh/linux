// SPDX-License-Identifier: GPL-2.0-only
/*
 * AArch64-specific system calls implementation
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

#include <asm/cpufeature.h>
#include <asm/syscall.h>

/*
 * IAMROOT, 2022.05.21:
 * - arm64 sys_mmap
 *
 * ex) user에서 malloc을 사용했을때 strace 사용결과
 * sh) strace ./test
 * == strace
 * mmap(NULL, 1000001536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7fb4af7ee000
 *
 * == smaps log(heap 측 부분)
 * 7fb4af7ee000-7fb4eb19b000 rw-p 00000000 00:00 0 
 * Size:             976564 kB
 * KernelPageSize:        4 kB
 * MMUPageSize:           4 kB
 * Rss:              976564 kB
 * Pss:              976564 kB
 * Shared_Clean:          0 kB
 * Shared_Dirty:          0 kB
 * Private_Clean:         0 kB
 * Private_Dirty:    976564 kB
 * Referenced:       976564 kB
 * Anonymous:        976564 kB
 * LazyFree:              0 kB
 * AnonHugePages:         0 kB
 * ShmemPmdMapped:        0 kB
 * FilePmdMapped:         0 kB
 * Shared_Hugetlb:        0 kB
 * Private_Hugetlb:       0 kB
 * Swap:                  0 kB
 * SwapPss:               0 kB
 * Locked:                0 kB
 * THPeligible:    0
 * VmFlags: rd wr mr(may read) mw(may write) me(may execute) ac sd 
 *
 * read, write 권한이 있는 vma라는것을 알수있다.
 * malloc을 호출하면 항상 위와같은 prot, flag를 사용한다.
 */
SYSCALL_DEFINE6(mmap, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, off)
{
	if (offset_in_page(off) != 0)
		return -EINVAL;

	return ksys_mmap_pgoff(addr, len, prot, flags, fd, off >> PAGE_SHIFT);
}

SYSCALL_DEFINE1(arm64_personality, unsigned int, personality)
{
	if (personality(personality) == PER_LINUX32 &&
		!system_supports_32bit_el0())
		return -EINVAL;
	return ksys_personality(personality);
}

asmlinkage long sys_ni_syscall(void);

asmlinkage long __arm64_sys_ni_syscall(const struct pt_regs *__unused)
{
	return sys_ni_syscall();
}

/*
 * Wrappers to pass the pt_regs argument.
 */
#define __arm64_sys_personality		__arm64_sys_arm64_personality

#undef __SYSCALL
#define __SYSCALL(nr, sym)	asmlinkage long __arm64_##sym(const struct pt_regs *);
#include <asm/unistd.h>

#undef __SYSCALL
#define __SYSCALL(nr, sym)	[nr] = __arm64_##sym,

const syscall_fn_t sys_call_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = __arm64_sys_ni_syscall,
#include <asm/unistd.h>
};
