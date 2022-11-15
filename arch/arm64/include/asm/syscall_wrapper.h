/* SPDX-License-Identifier: GPL-2.0 */
/*
 * syscall_wrapper.h - arm64 specific wrappers to syscall definitions
 *
 * Based on arch/x86/include/asm_syscall_wrapper.h
 */

#ifndef __ASM_SYSCALL_WRAPPER_H
#define __ASM_SYSCALL_WRAPPER_H

struct pt_regs;

#define SC_ARM64_REGS_TO_ARGS(x, ...)				\
	__MAP(x,__SC_ARGS					\
	      ,,regs->regs[0],,regs->regs[1],,regs->regs[2]	\
	      ,,regs->regs[3],,regs->regs[4],,regs->regs[5])

#ifdef CONFIG_COMPAT

#define COMPAT_SYSCALL_DEFINEx(x, name, ...)						\
	asmlinkage long __arm64_compat_sys##name(const struct pt_regs *regs);		\
	ALLOW_ERROR_INJECTION(__arm64_compat_sys##name, ERRNO);				\
	static long __se_compat_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));		\
	static inline long __do_compat_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));	\
	asmlinkage long __arm64_compat_sys##name(const struct pt_regs *regs)		\
	{										\
		return __se_compat_sys##name(SC_ARM64_REGS_TO_ARGS(x,__VA_ARGS__));	\
	}										\
	static long __se_compat_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))		\
	{										\
		return __do_compat_sys##name(__MAP(x,__SC_DELOUSE,__VA_ARGS__));	\
	}										\
	static inline long __do_compat_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))

#define COMPAT_SYSCALL_DEFINE0(sname)							\
	asmlinkage long __arm64_compat_sys_##sname(const struct pt_regs *__unused);	\
	ALLOW_ERROR_INJECTION(__arm64_compat_sys_##sname, ERRNO);			\
	asmlinkage long __arm64_compat_sys_##sname(const struct pt_regs *__unused)

#define COND_SYSCALL_COMPAT(name) 							\
	asmlinkage long __weak __arm64_compat_sys_##name(const struct pt_regs *regs)	\
	{										\
		return sys_ni_syscall();						\
	}

#define COMPAT_SYS_NI(name) \
	SYSCALL_ALIAS(__arm64_compat_sys_##name, sys_ni_posix_timers);

#endif /* CONFIG_COMPAT */

/*
 * IAMROOT, 2022.11.15:
 * - ex) SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, umode_t, mode)
 *   1. SYSCALL_DEFINE3 확장.
 *   SYSCALL_DEFINE3(name, ...) SYSCALL_DEFINEx(3, _##name, __VA_ARGS__)
 *	 =>
 *	 SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, umode_t, mode)	\
 *		SYSCALL_DEFINEx(3, _open, const char __user *, filename, int, flags, umode_t, mode)
 *
 *  2. SYSCALL_DEFINEx 확장
 *  SYSCALL_DEFINEx(x, sname, ...)	\
 *			SYSCALL_METADATA(sname, x, __VA_ARGS__) --> pthread용.
 *			__SYSCALL_DEFINEx(x, sname, __VA_ARGS__)
 *	=>
 *	SYSCALL_DEFINEx(3, _open, const char __user *, filename, int, flags, umode_t, mode)	\
 *		__SYSCALL_DEFINEx(3, _open, const char __user *, filename, int, flags, umode_t, mode)
 *
 *  3. __SYSCALL_DEFINEx 확장
 *	__SYSCALL_DEFINEx(x, name, ...)			\
 *		asmlinkage long __arm64_sys##name(const struct pt_regs *regs);		\
 *		ALLOW_ERROR_INJECTION(__arm64_sys##name, ERRNO);	--> erro injection용
 *		static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));		\
 *		static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));	\
 *		asmlinkage long __arm64_sys##name(const struct pt_regs *regs)		\
 *		{									\
 *			return __se_sys##name(SC_ARM64_REGS_TO_ARGS(x,__VA_ARGS__));	\
 *		}									\
 *		static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))		\
 *		{									\
 *			long ret = __do_sys##name(__MAP(x,__SC_CAST,__VA_ARGS__));	\
 *			__MAP(x,__SC_TEST,__VA_ARGS__);					\
 *			__PROTECT(x, ret,__MAP(x,__SC_ARGS,__VA_ARGS__));		\
 *			return ret;							\
 *		}									\
 *		static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))
 * =>
 *	__SYSCALL_DEFINEx(3, _open, const char __user *, filename, int, flags, umode_t, mode)	\
 *		asmlinkage long __arm64_sys_open(const struct pt_regs *regs);		
 *		static long __se_sys_open(__MAP(3,__SC_LONG, const char __user *, filename, int, flags, umode_t, mode));		
 *		static inline long __do_sys_open(__MAP(3,__SC_DECL, const char __user *, filename, int, flags, umode_t, mode));	
 *
 *		asmlinkage long __arm64_sys_open(const struct pt_regs *regs)		
 *		{									
 *			return __se_sys_open(SC_ARM64_REGS_TO_ARGS(3, const char __user *, filename, int, flags, umode_t, mode));	
 *		}									
 *
 *		static long __se_sys_open(__MAP(3,__SC_LONG, const char __user *, filename, int, flags, umode_t, mode))		
 *		{									
 *			long ret = __do_sys_open(__MAP(3,__SC_CAST, const char __user *, filename, int, flags, umode_t, mode));	
 *			__MAP(3,__SC_TEST, const char __user *, filename, int, flags, umode_t, mode);					
 *			__PROTECT(x, ret,__MAP(3,__SC_ARGS, const char __user *, filename, int, flags, umode_t, mode));		
 *			return ret;							
 *		}									
 *
 *		static inline long __do_sys_open(__MAP(3,__SC_DECL, const char __user *, filename, int, flags, umode_t, mode))
 *		{ //code 직접 작성. fs/open.c }
 *
 * 4. MAP 확장
 * __MAP0(m,...)
 * __MAP1(m,t,a,...) m(t,a)
 * __MAP2(m,t,a,...) m(t,a), __MAP1(m,__VA_ARGS__)
 * __MAP3(m,t,a,...) m(t,a), __MAP2(m,__VA_ARGS__)
 * __MAP(n,...) __MAP##n(__VA_ARGS__)
 * =>
 * __MAP(3,__SC_DECL, const char __user *, filename, int, flags, umode_t, mode)	\
 *		__MAP3(__SC_DECL, const char __user *, filename, int, flags, umode_t, mode)
 * =>
 * __MAP3(__SC_DECL, const char __user *, filename, int, flags, umode_t, mode)	\
 *		__SC_DECL(const char __user *, filename), __MAP2(__SC_DECL, int, flags, umode_t, mode)
 * =>
 *		__SC_DECL(const char __user *, filename), __SC_DECL(int, flags), _MAP1(__SC_DECL, umode_t, mode)
*  =>
 *		__SC_DECL(const char __user *, filename), __SC_DECL(int, flags) __SC_DECL(umode_t, mode)
 * => const char __user *filename, int flags, umode_t mode
 * 
 * 5. SC_ARM64_REGS_TO_ARGS 확장
 *
 * SC_ARM64_REGS_TO_ARGS(x, ...)				\
 *	__MAP(x,__SC_ARGS,,regs->regs[0],,regs->regs[1],,regs->regs[2]	\
 *			,,regs->regs[3],,regs->regs[4],,regs->regs[5])
 * =>
 *	SC_ARM64_REGS_TO_ARGS(3, const char __user *, filename, int, flags, umode_t, mode)	\
 *	__MAP(3,__SC_ARGS,,regs->regs[0],,regs->regs[1],,regs->regs[2]	\
 *			,,regs->regs[3],,regs->regs[4],,regs->regs[5])
 * =>
 *	__SC_ARGS(,regs->regs[0]) , __SC_ARGS(,regs->regs[1]), __SC_ARGS(,regs->regs[2]), __SC_ARGS(,regs->regs[3])
 * => regs->regs[0], regs->regs[1], regs->regs[2], regs->regs[3]
 *
 * 6. 최종
 *	__SYSCALL_DEFINEx(3, _open, const char __user *, filename, int, flags, umode_t, mode)	\
 *		asmlinkage long __arm64_sys_open(const struct pt_regs *regs);		
 *		static long __se_sys_open(__MAP(3,__SC_LONG, const char __user *, filename, int, flags, umode_t, mode));		
 *		static inline long __do_sys_open(const char __user * filename, int flags, umode_t mode);	
 *
 *		asmlinkage long __arm64_sys_open(const struct pt_regs *regs)		
 *		{									
 *			return __se_sys_open(regs->regs[0], regs->regs[1], regs->regs[2], regs->regs[3]);	
 *		}									
 *
 *		static long __se_sys_open(long filename, long flags, long mode)		
 *		{									
 *			long ret = __do_sys_open((__force const char __user *) filename, (__force int) flags, (__force umode_t) mode);
 *
 *			BUILD_BUG_ON_ZERO(!__TYPE_IS_LL(const char __user *) && sizeof(const char __user *) > sizeof(long)),
 *			BUILD_BUG_ON_ZERO(!__TYPE_IS_LL(int) && sizeof(int) > sizeof(long)),
 *			BUILD_BUG_ON_ZERO(!__TYPE_IS_LL(umode_t) && sizeof(umode_t) > sizeof(long)),
 *
 *			return ret;							
 *		}									
 *
 *		static inline long __do_sys_open(const char __user * filename, int flags, umode_t mode)
 *		{
 *			//code 직접 작성. fs/open.c
 *		}
 */
#define __SYSCALL_DEFINEx(x, name, ...)						\
	asmlinkage long __arm64_sys##name(const struct pt_regs *regs);		\
	ALLOW_ERROR_INJECTION(__arm64_sys##name, ERRNO);			\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));		\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));	\
	asmlinkage long __arm64_sys##name(const struct pt_regs *regs)		\
	{									\
		return __se_sys##name(SC_ARM64_REGS_TO_ARGS(x,__VA_ARGS__));	\
	}									\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))		\
	{									\
		long ret = __do_sys##name(__MAP(x,__SC_CAST,__VA_ARGS__));	\
		__MAP(x,__SC_TEST,__VA_ARGS__);					\
		__PROTECT(x, ret,__MAP(x,__SC_ARGS,__VA_ARGS__));		\
		return ret;							\
	}									\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))

#define SYSCALL_DEFINE0(sname)							\
	SYSCALL_METADATA(_##sname, 0);						\
	asmlinkage long __arm64_sys_##sname(const struct pt_regs *__unused);	\
	ALLOW_ERROR_INJECTION(__arm64_sys_##sname, ERRNO);			\
	asmlinkage long __arm64_sys_##sname(const struct pt_regs *__unused)

#define COND_SYSCALL(name)							\
	asmlinkage long __weak __arm64_sys_##name(const struct pt_regs *regs)	\
	{									\
		return sys_ni_syscall();					\
	}

#define SYS_NI(name) SYSCALL_ALIAS(__arm64_sys_##name, sys_ni_posix_timers);

#endif /* __ASM_SYSCALL_WRAPPER_H */
