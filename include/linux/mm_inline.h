/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/swap.h>

/**
 * page_is_file_lru - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns 1 if @page is a regular filesystem backed page cache page or a lazily
 * freed anonymous page (e.g. via MADV_FREE).  Returns 0 if @page is a normal
 * anonymous page, a tmpfs page or otherwise ram or swap backed page.  Used by
 * functions that manipulate the LRU lists, to sort a page onto the right LRU
 * list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
/*
 * IAMROOT, 2022.04.23:
 * @return true  : file lru or clean anon page(lazy free)
 *         false : normal anon pages
 * - 이 함수를 사용해서 anon page를 판별하는 용도로도 사용한다.
 *   anon page를 고를 경우 (return false)의 경우나 (return true)에서 file page를
 *   제외하면 된다.
 *
 *   if ((return false) || ((return true) && !(file page)))
 *   => if (!page_is_file_lru(page) || (page_is_file_lru(page) && PageAnon(page)))
 *      (page_is_file_lru()로 true를 받았는데 anon인지 검사하는건 보기 이상하므로
 *      않으므로 함수를 안쓰고 그냥 !PageSwapBacked로 쓰는거 같다.)
 *   => if (!page_is_file_lru(page) || (!PageSwapBacked(page) && PageAnon(page)))
 *
 *   ---
 *   ex)
 *   if (!page_is_file_lru(page))
 *   {
 *		// normal anon pages
 *   }
 *
 *   if (page_is_file_lru(page)) // == !PageSwapBacked(page)
 *   {
 *		// clean anon page + file page
 *   }
 *
 *   if (PageAnon(page) && !PageSwapBacked(page)))
 *   {
 *		// clean anon page
 *   }
 *
 *   if (!page_is_file_lru(page) || (PageAnon(page) && !PageSwapBacked(page)))
 *   {
 *		// real anon pages + clean anon page
 *   }
 *
 * ----
 * - 일반 anon page들은 SwapBacked flag가 set되있다.(return false)
 * - SwapBacked flag가 unset인 경우(return true)
 *   1. clean anon page인 경우(lazy free)
 *   2. 정말 file page인 경우
 */
static inline int page_is_file_lru(struct page *page)
{
	return !PageSwapBacked(page);
}

/*
 * IAMROOT, 2022.04.02:
 * - lru 변경에 따른 state update
 * - @lruvec에서 관리하는 현재 page수를 관리한다.
 */
static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	__mod_lruvec_state(lruvec, NR_LRU_BASE + lru, nr_pages);
	__mod_zone_page_state(&pgdat->node_zones[zid],
				NR_ZONE_LRU_BASE + lru, nr_pages);
#ifdef CONFIG_MEMCG
	mem_cgroup_update_lru_size(lruvec, lru, zid, nr_pages);
#endif
}

/**
 * __clear_page_lru_flags - clear page lru flags before releasing a page
 * @page: the page that was on lru and now has a zero reference
 */
/*
 * IAMROOT, 2022.04.02:
 * - @page는 lru list에서 제거된상태. @page에서 lru 관련 flag를 clear한다.
 * - lru 관련 flags : PG_lru, PG_active, PG_unevictable
 */
static __always_inline void __clear_page_lru_flags(struct page *page)
{
	VM_BUG_ON_PAGE(!PageLRU(page), page);

	__ClearPageLRU(page);

	/* this shouldn't happen, so leave the flags to bad_page() */
/*
 * IAMROOT, 2022.04.02:
 * - Active와 unevictable는 절대로 같이 set될수없다.
 */
	if (PageActive(page) && PageUnevictable(page))
		return;

	__ClearPageActive(page);
	__ClearPageUnevictable(page);
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
/*
 * IAMROOT, 2022.04.02:
 * - page flag에 unevictable가 set되 있으면 unevictable lru list에 넣어야된다는
 *   의미이다.
 * - lru가 file인지 아닌지에 따라 LRU_INACTIVE_FILE / LRU_INACTIVE_ANON일지 1차적으로
 *   정하고 그 다음에 page가 active인지를 한번더 고려해 LRU_ACTIVE를 더할지를 고려하여,
 *   최종적으로 LRU_INACTIVE_FILE / LRU_ACTIVE_FILE / LRU_INACTIVE_ANON /
 *   LRU_ACTIVE_ANON 가 return될것이다.
 */
static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

/*
 * IAMROOT, 2022.04.02:
 * - @page가 actvie / unevictable flag가 동시에 있을수없다.
 */
	VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);

	if (PageUnevictable(page))
		return LRU_UNEVICTABLE;

	lru = page_is_file_lru(page) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	if (PageActive(page))
		lru += LRU_ACTIVE;

	return lru;
}

/*
 * IAMROOT, 2022.04.02:
 * - @page가 위치해야될 lru enum을 구하고 해당 lru에 추가한다.
 */
static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	update_lru_size(lruvec, lru, page_zonenum(page), thp_nr_pages(page));
	list_add(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	update_lru_size(lruvec, lru, page_zonenum(page), thp_nr_pages(page));
	list_add_tail(&page->lru, &lruvec->lists[lru]);
}

/*
 * IAMROOT, 2022.04.02:
 * - @page의 lru list에서 @page를 제거하고 @lruvec state를 갱신한다.
 */
static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	list_del(&page->lru);
	update_lru_size(lruvec, page_lru(page), page_zonenum(page),
			-thp_nr_pages(page));
}
#endif
