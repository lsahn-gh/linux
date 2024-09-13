/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_REF_H
#define _LINUX_PAGE_REF_H

#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/tracepoint-defs.h>

DECLARE_TRACEPOINT(page_ref_set);
DECLARE_TRACEPOINT(page_ref_mod);
DECLARE_TRACEPOINT(page_ref_mod_and_test);
DECLARE_TRACEPOINT(page_ref_mod_and_return);
DECLARE_TRACEPOINT(page_ref_mod_unless);
DECLARE_TRACEPOINT(page_ref_freeze);
DECLARE_TRACEPOINT(page_ref_unfreeze);

#ifdef CONFIG_DEBUG_PAGE_REF

/*
 * Ideally we would want to use the trace_<tracepoint>_enabled() helper
 * functions. But due to include header file issues, that is not
 * feasible. Instead we have to open code the static key functions.
 *
 * See trace_##name##_enabled(void) in include/linux/tracepoint.h
 */
#define page_ref_tracepoint_active(t) tracepoint_enabled(t)

extern void __page_ref_set(struct page *page, int v);
extern void __page_ref_mod(struct page *page, int v);
extern void __page_ref_mod_and_test(struct page *page, int v, int ret);
extern void __page_ref_mod_and_return(struct page *page, int v, int ret);
extern void __page_ref_mod_unless(struct page *page, int v, int u);
extern void __page_ref_freeze(struct page *page, int v, int ret);
extern void __page_ref_unfreeze(struct page *page, int v);

#else

#define page_ref_tracepoint_active(t) false

static inline void __page_ref_set(struct page *page, int v)
{
}
static inline void __page_ref_mod(struct page *page, int v)
{
}
static inline void __page_ref_mod_and_test(struct page *page, int v, int ret)
{
}
static inline void __page_ref_mod_and_return(struct page *page, int v, int ret)
{
}
static inline void __page_ref_mod_unless(struct page *page, int v, int u)
{
}
static inline void __page_ref_freeze(struct page *page, int v, int ret)
{
}
static inline void __page_ref_unfreeze(struct page *page, int v)
{
}

#endif

static inline int page_ref_count(const struct page *page)
{
	return atomic_read(&page->_refcount);
}

static inline int page_count(const struct page *page)
{
	return atomic_read(&compound_head(page)->_refcount);
}

/* IAMROOT, 2022.02.26:
 * - @page의 refcount를 @v로 변경한다.
 */
static inline void set_page_count(struct page *page, int v)
{
	atomic_set(&page->_refcount, v);
	if (page_ref_tracepoint_active(page_ref_set))
		__page_ref_set(page, v);
}

/*
 * Setup the page count before being freed into the page allocator for
 * the first time (boot or memory hotplug)
 */
/* IAMROOT, 2021.12.11:
 * - @page의 refcount를 1로 설정한다.
 */
static inline void init_page_count(struct page *page)
{
	set_page_count(page, 1);
}

static inline void page_ref_add(struct page *page, int nr)
{
	atomic_add(nr, &page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, nr);
}

static inline void page_ref_sub(struct page *page, int nr)
{
	atomic_sub(nr, &page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, -nr);
}

static inline int page_ref_sub_return(struct page *page, int nr)
{
	int ret = atomic_sub_return(nr, &page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_return))
		__page_ref_mod_and_return(page, -nr, ret);
	return ret;
}

static inline void page_ref_inc(struct page *page)
{
	atomic_inc(&page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, 1);
}

static inline void page_ref_dec(struct page *page)
{
	atomic_dec(&page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, -1);
}

static inline int page_ref_sub_and_test(struct page *page, int nr)
{
	int ret = atomic_sub_and_test(nr, &page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_test))
		__page_ref_mod_and_test(page, -nr, ret);
	return ret;
}

static inline int page_ref_inc_return(struct page *page)
{
	int ret = atomic_inc_return(&page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_return))
		__page_ref_mod_and_return(page, 1, ret);
	return ret;
}

static inline int page_ref_dec_and_test(struct page *page)
{
	int ret = atomic_dec_and_test(&page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_test))
		__page_ref_mod_and_test(page, -1, ret);
	return ret;
}

static inline int page_ref_dec_return(struct page *page)
{
	int ret = atomic_dec_return(&page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_return))
		__page_ref_mod_and_return(page, -1, ret);
	return ret;
}

/*
 * IAMROOT, 2022.03.30:
 * - @page의 참조카운터에 add에 성공하면(add 전 or add 후값이 @u가 아니면)
 *   return true. add에 실패하면(전후값이 @u이면) return false
 *
 * 1. @page의 참조카운터가 처음부터 @u인경우
 *  -> add를 안하고 @return 0(false)
 *
 * 2. @page의 참조카운터가 @u이 아닌 경우
 *  -> add @nr을 하고 난 결과값이 @u인 경우 @return 0(false), 아니면
 *  @return 1(true)
 */
static inline int page_ref_add_unless(struct page *page, int nr, int u)
{
	int ret = atomic_add_unless(&page->_refcount, nr, u);

	if (page_ref_tracepoint_active(page_ref_mod_unless))
		__page_ref_mod_unless(page, nr, ret);
	return ret;
}

/*
 * IAMROOT, 2022.04.09:
 * - @page를 참조 못하도록 막아둔다.
 * - @page의 refcount가 @count랑 같으면 0으로 set한다.
 */
static inline int page_ref_freeze(struct page *page, int count)
{
	int ret = likely(atomic_cmpxchg(&page->_refcount, count, 0) == count);

	if (page_ref_tracepoint_active(page_ref_freeze))
		__page_ref_freeze(page, count, ret);
	return ret;
}

static inline void page_ref_unfreeze(struct page *page, int count)
{
	VM_BUG_ON_PAGE(page_count(page) != 0, page);
	VM_BUG_ON(count == 0);

	atomic_set_release(&page->_refcount, count);
	if (page_ref_tracepoint_active(page_ref_unfreeze))
		__page_ref_unfreeze(page, count);
}

#endif
