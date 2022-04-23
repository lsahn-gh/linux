/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_COUNTER_H
#define _LINUX_PAGE_COUNTER_H

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <asm/page.h>

/*
 * IAMROOT, 2022.04.16:
 * - cgroup1 기준 sysfs에서 각 인자 접근 이름(memsw_files, mem_cgroup_read_u64 참고)
 *   sysfs               | member name
 *   --------------------+-----------------------
 *   usage_in_bytes      | usage * PAGE_SIZE
 *   max_usage_in_bytes  | watermark * PAGE_SIZE
 *   limit_in_bytes      | max * PAGE_SIZE
 *   failcnt             | failcnt
 *   soft_limit_in_bytes | soft_limit * PAGE_SIZE
 * - cgroup2는 직접확인해야된다.(예)swap_files)
 */
struct page_counter {
	atomic_long_t usage;
	unsigned long min;
	unsigned long low;
	unsigned long high;
	unsigned long max;

	/* effective memory.min and memory.min usage tracking */
	unsigned long emin;
	atomic_long_t min_usage;
	atomic_long_t children_min_usage;

	/* effective memory.low and memory.low usage tracking */
	unsigned long elow;
	atomic_long_t low_usage;
	atomic_long_t children_low_usage;

	/* legacy */
	unsigned long watermark;
	unsigned long failcnt;

	/*
	 * 'parent' is placed here to be far from 'usage' to reduce
	 * cache false sharing, as 'usage' is written mostly while
	 * parent is frequently read for cgroup's hierarchical
	 * counting nature.
	 */
	struct page_counter *parent;
};

#if BITS_PER_LONG == 32
#define PAGE_COUNTER_MAX LONG_MAX
#else
#define PAGE_COUNTER_MAX (LONG_MAX / PAGE_SIZE)
#endif

static inline void page_counter_init(struct page_counter *counter,
				     struct page_counter *parent)
{
	atomic_long_set(&counter->usage, 0);
	counter->max = PAGE_COUNTER_MAX;
	counter->parent = parent;
}

/*
 * IAMROOT, 2022.04.23:
 * - file name : usage_in_bytes
 */
static inline unsigned long page_counter_read(struct page_counter *counter)
{
	return atomic_long_read(&counter->usage);
}

void page_counter_cancel(struct page_counter *counter, unsigned long nr_pages);
void page_counter_charge(struct page_counter *counter, unsigned long nr_pages);
bool page_counter_try_charge(struct page_counter *counter,
			     unsigned long nr_pages,
			     struct page_counter **fail);
void page_counter_uncharge(struct page_counter *counter, unsigned long nr_pages);
void page_counter_set_min(struct page_counter *counter, unsigned long nr_pages);
void page_counter_set_low(struct page_counter *counter, unsigned long nr_pages);

static inline void page_counter_set_high(struct page_counter *counter,
					 unsigned long nr_pages)
{
	WRITE_ONCE(counter->high, nr_pages);
}

int page_counter_set_max(struct page_counter *counter, unsigned long nr_pages);
int page_counter_memparse(const char *buf, const char *max,
			  unsigned long *nr_pages);

static inline void page_counter_reset_watermark(struct page_counter *counter)
{
	counter->watermark = page_counter_read(counter);
}

#endif /* _LINUX_PAGE_COUNTER_H */
