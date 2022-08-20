/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/barrier.h
 *
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_BARRIER_H
#define __ASM_BARRIER_H

#ifndef __ASSEMBLY__

#include <linux/kasan-checks.h>

#define __nops(n)	".rept	" #n "\nnop\n.endr\n"
#define nops(n)		asm volatile(__nops(n))

/*
 * IAMROOT, 2021.09.18:
 * - wfi : wait for interrupt
 *   예를들어 0번 cpu가 wfi에 빠지면 다른 cpu가 IPI(inter process interrupt)를 통해
 *   0번 cpu를 깨울때까지 기다린다는것. 안보내준다면 timer interrupt등을 통해서
 *   깨어난다.
 *
 * - wfe : wait for event
 *   sev나 주기적으로 event stream에 의해서 깨어나게된다.
 *   
 * - sev : send event
 */
#define sev()		asm volatile("sev" : : : "memory")
#define wfe()		asm volatile("wfe" : : : "memory")
#define wfi()		asm volatile("wfi" : : : "memory")

#define isb()		asm volatile("isb" : : : "memory")
#define dmb(opt)	asm volatile("dmb " #opt : : : "memory")
#define dsb(opt)	asm volatile("dsb " #opt : : : "memory")

#define psb_csync()	asm volatile("hint #17" : : : "memory")
#define tsb_csync()	asm volatile("hint #18" : : : "memory")
#define csdb()		asm volatile("hint #20" : : : "memory")

#ifdef CONFIG_ARM64_PSEUDO_NMI
#define pmr_sync()						\
	do {							\
		extern struct static_key_false gic_pmr_sync;	\
								\
		if (static_branch_unlikely(&gic_pmr_sync))	\
			dsb(sy);				\
	} while(0)
#else
#define pmr_sync()	do {} while (0)
#endif

#define mb()		dsb(sy)
#define rmb()		dsb(ld)
#define wmb()		dsb(st)

#define dma_mb()	dmb(osh)
#define dma_rmb()	dmb(oshld)
#define dma_wmb()	dmb(oshst)

/*
 * Generate a mask for array_index__nospec() that is ~0UL when 0 <= idx < sz
 * and 0 otherwise.
 */
#define array_index_mask_nospec array_index_mask_nospec
static inline unsigned long array_index_mask_nospec(unsigned long idx,
						    unsigned long sz)
{
	unsigned long mask;

	asm volatile(
	"	cmp	%1, %2\n"
	"	sbc	%0, xzr, xzr\n"
	: "=r" (mask)
	: "r" (idx), "Ir" (sz)
	: "cc");

	csdb();
	return mask;
}

/*
 * Ensure that reads of the counter are treated the same as memory reads
 * for the purposes of ordering by subsequent memory barriers.
 *
 * This insanity brought to you by speculative system register reads,
 * out-of-order memory accesses, sequence locks and Thomas Gleixner.
 *
 * https://lore.kernel.org/r/alpine.DEB.2.21.1902081950260.1662@nanos.tec.linutronix.de/
 */
/*
 * IAMROOT, 2022.08.20:
 * - papago
 *   카운터 읽기가 후속 메모리 배리어에 의한 순서를 위해 메모리 읽기와 동일하게 처리되는지
 *   확인하십시오.
 *
 *   이 광기는 추측성 시스템 레지스터 읽기, 비순차적 메모리 액세스, 시퀀스 잠금 및
 *   Thomas Gleixner로 인해 발생했습니다. 
 * - eor %0, %1, %1 => 같은값 xor을 하면 결국 0.
 *   add x0, sp, %0 => sp + 0 = sp
 *   ldr xzr, [%0]  => xzr에 sp가 가리키는 data를 load. 아무 일 안함.
 *
 * - git blame
 *   arm64: vdso: Avoid ISB after reading from cntvct_el0.
 *
 *   We can avoid the expensive ISB instruction after reading the counter in the vDSO gettime
 *   functions by creating a fake address hazard against a dummy stack read, just like we do
 *   inside the kernel.
 *
 * - papgo
 *   arm64: vdso: cntvct_el0에서 읽은 후에는 ISB를 피하십시오.
 *
 *   커널 내부에서처럼 더미 스택 읽기에 대해 가짜 주소 위험을 생성하여 vDSO gettime 함수의
 *   카운터를 읽은 후 값비싼 ISB 명령을 피할 수 있다.
 *
 * - git blame2
 *   arm64: arch_timer: Ensure counter register reads occur with seqlock held.
 *
 *   When executing clock_gettime(), either in the vDSO or via a system call, we need to ensure
 *   that the read of the counter register occurs within the seqlock reader critical section.
 *   This ensures that updates to the clocksource parameters (e.g. the multiplier) are consistent
 *   with the counter value and therefore avoids the situation where time appears to go backwards
 *   across multiple reads.
 *
 *   Extend the vDSO logic so that the seqlock critical section covers the read of the counter
 *   register as well as accesses to the data page. Since reads of the counter system registers
 *   are not ordered by memory barrier instructions, introduce dependency ordering from the counter
 *   read to a subsequent memory access so that the seqlock memory barriers apply to the counter
 *   access in both the vDSO and the system call paths.
 *
 * - papago
 *   arm64: arch_timer: 카운터 레지스터 읽기가 시퀀스를 누른 상태에서 수행되는지 확인합니다.
 *
 *   vDSO에서 또는 시스템 호출을 통해 clock_gettime()을 실행할 때는 카운터 레지스터 읽기가 seqlock
 *   Reader critical 섹션 내에서 수행되도록 해야 합니다. 이렇게 하면 클럭 소스 매개 변수(예: 승수)에
 *   대한 업데이트가 카운터 값과 일치하므로 여러 판독에서 시간이 거꾸로 가는 상황을 피할 수 있다.
 *
 *   seqlock 중요 섹션이 카운터 레지스터 읽기와 데이터 페이지 액세스를 포함하도록 vDSO 논리를
 *   확장합니다. 카운터 시스템 레지스터 읽기는 메모리 장벽 지침에 따라 정렬되지 않으므로 카운터
 *   읽기부터 후속 메모리 액세스까지 종속성 순서를 도입하여 seqlock 메모리 장벽이 vDSO 및 시스템
 *   호출 경로 모두에서 카운터 액세스에 적용되도록 합니다.  
 */
#define arch_counter_enforce_ordering(val) do {				\
	u64 tmp, _val = (val);						\
									\
	asm volatile(							\
	"	eor	%0, %1, %1\n"					\
	"	add	%0, sp, %0\n"					\
	"	ldr	xzr, [%0]"					\
	: "=r" (tmp) : "r" (_val));					\
} while (0)

#define __smp_mb()	dmb(ish)
#define __smp_rmb()	dmb(ishld)
#define __smp_wmb()	dmb(ishst)

/*
 * IAMROOT, 2021.09.18:
 * - store + release barrier
 */
#define __smp_store_release(p, v)					\
do {									\
	typeof(p) __p = (p);						\
	union { __unqual_scalar_typeof(*p) __val; char __c[1]; } __u =	\
		{ .__val = (__force __unqual_scalar_typeof(*p)) (v) };	\
	compiletime_assert_atomic_type(*p);				\
	kasan_check_write(__p, sizeof(*p));				\
	switch (sizeof(*p)) {						\
	case 1:								\
		asm volatile ("stlrb %w1, %0"				\
				: "=Q" (*__p)				\
				: "r" (*(__u8 *)__u.__c)		\
				: "memory");				\
		break;							\
	case 2:								\
		asm volatile ("stlrh %w1, %0"				\
				: "=Q" (*__p)				\
				: "r" (*(__u16 *)__u.__c)		\
				: "memory");				\
		break;							\
	case 4:								\
		asm volatile ("stlr %w1, %0"				\
				: "=Q" (*__p)				\
				: "r" (*(__u32 *)__u.__c)		\
				: "memory");				\
		break;							\
	case 8:								\
		asm volatile ("stlr %1, %0"				\
				: "=Q" (*__p)				\
				: "r" (*(__u64 *)__u.__c)		\
				: "memory");				\
		break;							\
	}								\
} while (0)

/*
 * IAMROOT, 2021.09.18:
 * - load + acquire barrier
 */

#define __smp_load_acquire(p)						\
({									\
	union { __unqual_scalar_typeof(*p) __val; char __c[1]; } __u;	\
	typeof(p) __p = (p);						\
	compiletime_assert_atomic_type(*p);				\
	kasan_check_read(__p, sizeof(*p));				\
	switch (sizeof(*p)) {						\
	case 1:								\
		asm volatile ("ldarb %w0, %1"				\
			: "=r" (*(__u8 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	case 2:								\
		asm volatile ("ldarh %w0, %1"				\
			: "=r" (*(__u16 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	case 4:								\
		asm volatile ("ldar %w0, %1"				\
			: "=r" (*(__u32 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	case 8:								\
		asm volatile ("ldar %0, %1"				\
			: "=r" (*(__u64 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	}								\
	(typeof(*p))__u.__val;						\
})

/*
 * IAMROOT, 2021.09.18:
 * - barrier 미사용(relaxed)
 */

#define smp_cond_load_relaxed(ptr, cond_expr)				\
({									\
	typeof(ptr) __PTR = (ptr);					\
	__unqual_scalar_typeof(*ptr) VAL;				\
	for (;;) {							\
		VAL = READ_ONCE(*__PTR);				\
		if (cond_expr)						\
			break;						\
		__cmpwait_relaxed(__PTR, VAL);				\
	}								\
	(typeof(*ptr))VAL;						\
})

#define smp_cond_load_acquire(ptr, cond_expr)				\
({									\
	typeof(ptr) __PTR = (ptr);					\
	__unqual_scalar_typeof(*ptr) VAL;				\
	for (;;) {							\
		VAL = smp_load_acquire(__PTR);				\
		if (cond_expr)						\
			break;						\
		__cmpwait_relaxed(__PTR, VAL);				\
	}								\
	(typeof(*ptr))VAL;						\
})

#include <asm-generic/barrier.h>

#endif	/* __ASSEMBLY__ */

#endif	/* __ASM_BARRIER_H */
