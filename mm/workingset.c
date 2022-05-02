// SPDX-License-Identifier: GPL-2.0
/*
 * Workingset detection
 *
 * Copyright (C) 2013 Red Hat, Inc., Johannes Weiner
 */

#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/writeback.h>
#include <linux/shmem_fs.h>
#include <linux/pagemap.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>

/*
 *		Double CLOCK lists
 *
 * Per node, two clock lists are maintained for file pages: the
 * inactive and the active list.  Freshly faulted pages start out at
 * the head of the inactive list and page reclaim scans pages from the
 * tail.  Pages that are accessed multiple times on the inactive list
 * are promoted to the active list, to protect them from reclaim,
 * whereas active pages are demoted to the inactive list when the
 * active list grows too big.
 */

/*
 * IAMROOT, 2022.04.30:
 * - papgo
 *   노드당 파일 페이지에 대해 두 개의 클럭 목록이 유지됩니다.
 *   비활성 및 활성 목록입니다. 새로 결함이 발생한 페이지는 비활성 목록의 선두에서
 *   시작하여 페이지 회수 검색 페이지를 꼬리에서 검색합니다. 비활성 목록에서
 *   여러 번 액세스한 페이지는 회수로부터 보호하기 위해 활성 목록으로 승격되는
 *   반면 활성 페이지는 활성 목록이 너무 커지면 비활성 목록으로 강등됩니다.
 */
/*
 *
 *   fault ------------------------+
 *                                 |
 *              +--------------+   |            +-------------+
 *   reclaim <- |   inactive   | <-+-- demotion |    active   | <--+
 *              +--------------+                +-------------+    |
 *                     |                                           |
 *                     +-------------- promotion ------------------+
 *
 *
 *		Access frequency and refault distance
 *
 * A workload is thrashing when its pages are frequently used but they
 * are evicted from the inactive list every time before another access
 * would have promoted them to the active list.
 *
 * In cases where the average access distance between thrashing pages
 * is bigger than the size of memory there is nothing that can be
 * done - the thrashing set could never fit into memory under any
 * circumstance.
 *
 * However, the average access distance could be bigger than the
 * inactive list, yet smaller than the size of memory.  In this case,
 * the set could fit into memory if it weren't for the currently
 * active pages - which may be used more, hopefully less frequently:
 */

/*
 * IAMROOT, 2022.04.30:
 * - papgo
 * 페이지는 자주 사용되지만 다른 액세스로 페이지가 활성 목록으로 승격되기 전에
 * 매번 비활성 목록에서 eviction이 됩니다.
 *
 * 스래싱 페이지 간의 평균 액세스 거리가 메모리 크기보다 큰 경우, 할 수 있는 일은
 * 없습니다. - 스래싱 세트가 어떤 상황에서도 메모리에 들어가지 않을 수 있습니다.
 * (평균 액세스 거리가 메모리 크기보다 큰 경우라는것은 지금 상황이 현재 메모리보다
 * 큰 용량을 사용하고 있다는 얘기. 즉 disk에서 reload까지 하면서  쓰고 있는데
 * 전체 메모리가 계속 사용 중이라 어쩔수없이 thrashing이 일어나고 있다는 뜻이다.)
 *
 * 그러나 평균 액세스 거리는 비활성 목록보다 크지만 메모리 크기보다 작을 수 있습니다.
 * 이 경우, 현재 활성 페이지가 없었다면 이 집합은 메모리에 들어갈 수 있었는데, 
 * 이 페이지들은 더 많이, 더 적게 사용되었으면 좋겠다.
 */
/*
 *      +-memory available to cache-+
 *      |                           |
 *      +-inactive------+-active----+
 *  a b | c d e f g h i | J K L M N |
 *      +---------------+-----------+
 *
 * It is prohibitively expensive to accurately track access frequency
 * of pages.  But a reasonable approximation can be made to measure
 * thrashing on the inactive list, after which refaulting pages can be
 * activated optimistically to compete with the existing active pages.
 *
 * Approximating inactive page access frequency - Observations:
 *
 * 1. When a page is accessed for the first time, it is added to the
 *    head of the inactive list, slides every existing inactive page
 *    towards the tail by one slot, and pushes the current tail page
 *    out of memory.
 *
 * 2. When a page is accessed for the second time, it is promoted to
 *    the active list, shrinking the inactive list by one slot.  This
 *    also slides all inactive pages that were faulted into the cache
 *    more recently than the activated page towards the tail of the
 *    inactive list.
 */

/*
 * IAMROOT, 2022.04.30:
 * - papgo
 *   페이지의 액세스 빈도를 정확하게 추적하는 것은 엄청나게 비싸다. 그러나 비활성
 *   목록에서 스래싱을 측정하기 위해 합리적인 근사치를 만들 수 있으며, 그 후 리폴트
 *   페이지를 최적화하여 기존 활성 페이지와 경쟁할 수 있다.
 *
 * 비활성 페이지 액세스 빈도 근사 - 관찰:
 * 
 * 1. 페이지가 처음 액세스되면 비활성 목록의 맨 앞에 추가되고, 모든 비활성 페이지를
 * 하나의 슬롯만큼 꼬리 쪽으로 슬라이드하고, 현재 테일 페이지를 메모리 밖으로
 * 밀어냅니다.
 *
 * 2. 페이지가 두 번째로 액세스되면 활성 목록으로 승격되어 비활성 목록이 한 슬롯씩
 * 줄어듭니다. 또한 활성화된 페이지보다 최근에 fault가 발생한 모든 비활성 페이지를
 * 비활성 목록의 꼬리 쪽으로 밀어 넣습니다.
 */
 /*
 * Thus:
 *
 * 1. The sum of evictions and activations between any two points in
 *    time indicate the minimum number of inactive pages accessed in
 *    between.
 *
 * 2. Moving one inactive page N page slots towards the tail of the
 *    list requires at least N inactive page accesses.
 *
 * Combining these:
 *
 * 1. When a page is finally evicted from memory, the number of
 *    inactive pages accessed while the page was in cache is at least
 *    the number of page slots on the inactive list.
 *
 * 2. In addition, measuring the sum of evictions and activations (E)
 *    at the time of a page's eviction, and comparing it to another
 *    reading (R) at the time the page faults back into memory tells
 *    the minimum number of accesses while the page was not cached.
 *    This is called the refault distance.
 */

/*
 * IAMROOT, 2022.04.30:
 * - papgo
 *   따라서:
 *
 *   1. 두 시점 사이의 eviction들 및 activation들 합계는 그 사이에 액세스된 최소
 *   비활성 페이지 수를 나타냅니다.
 *   (inacive 이지만 access가 이루어졌던 page들의 합이 eviction + activation)
 *   
 *   2. 비활성 페이지 N개의 페이지 슬롯을 목록의 꼬리 쪽으로 이동하려면 N개 이상의
 *   비활성 페이지 액세스가 필요합니다.
 *   (inactive 이지만 access를 했다는것을 판정할때마다 inacive list내에서의 이동이
 *   일어난다.)
 *
 * 이러한 조합:
 *
 * 1. 페이지가 최종적으로 메모리에서 eviction될 때 페이지가 캐시되어 있는 동안
 * 액세스된 비활성 페이지 수는 비활성 목록의 페이지 슬롯 수 이상입니다.
 *
 * 2. 또한 페이지 eviction 시점에 eviction들 과 activation들의 합계(E)를 측정하고,
 * page fault가 다시 메모리에 저장될 때 다른 판독치(R. refault)와 비교하면 페이지가
 * 캐시되지 않은 동안 최소 액세스 수를 알 수 있습니다.
 * 이를 refault distance라고 합니다.
 * (R은 refault 시점의 eviction들과 activation들의 합계. E가 R값에 비해 과거이기
 * 때문에 R값은 E보다 항상 크다. refault distacne = R - E)
 */
/*
 *
 * Because the first access of the page was the fault and the second
 * access the refault, we combine the in-cache distance with the
 * out-of-cache distance to get the complete minimum access distance
 * of this page:
 *
 *      NR_inactive + (R - E)
 *
 * And knowing the minimum access distance of a page, we can easily
 * tell if the page would be able to stay in cache assuming all page
 * slots in the cache were available:
 *
 *   NR_inactive + (R - E) <= NR_inactive + NR_active
 *
 * which can be further simplified to
 *
 *   (R - E) <= NR_active
 *
 * Put into words, the refault distance (out-of-cache) can be seen as
 * a deficit in inactive list space (in-cache).  If the inactive list
 * had (R - E) more page slots, the page would not have been evicted
 * in between accesses, but activated instead.  And on a full system,
 * the only thing eating into inactive list space is active pages.
 */

/*
 * IAMROOT, 2022.04.30:
 * - papgo 
 *   페이지의 첫 번째 액세스는 fault이고 두 번째 액세스는 refault이기 때문에 캐시
 *   내 거리(NR_inactive) 와 캐시 외 거리(refault distance. R - E)를 결합하여
 *   이 페이지의 complete minimum access distance를 얻습니다.
 *   (cache는 NR_active + NR_inactive 둘다 포함하지만 여기선 NR_active는 idle상태
 *   일수도 있기때문에(정확한 측정이 거의 불가능) minimum의 개념으로 NR_inactive만을
 *   포함해서 설명한다.)
 *
 *	NR_inactive + (R - E) = complete minimum access distance
 *
 * 또한 페이지의 minimum access distance를 알고 있기 때문에 캐시의 모든 페이지 슬롯을
 * 사용할 수 있다고 가정할 때 페이지가 캐시에 유지되는지 여부를 쉽게 알 수 있습니다.
 *
 *   NR_inactive + (R - E)              <= NR_inactive + NR_active
 *   (complete minimum access distance) <= memory(cache)
 *
 * 더 단순화할 수 있는 것은
 *
 *   (R - E) <= NR_active
 *
 * 다시 말해 리폴트 거리(out-of-cache)는 비활성 목록 공간(in-cache)의 부족으로 볼 수
 * 있습니다. 비활성 목록에 (R - E) 더 많은 페이지 슬롯이 있는 경우, 페이지는 액세스
 * 사이에 제거되지 않고 대신 활성화되었을 것입니다. 전체 시스템에서 비활성 목록
 * 공간을 잠식하는 유일한 것은 활성 페이지입니다.
 * (refault distance가 active list에 비해 더 짧다는것은 결국 active list에 있는
 * page들과 경쟁을 하여 실제 active list에 있는 inactive page들을 걸러내기 위함이다.)
 */
/*
 *
 *		Refaulting inactive pages
 *
 * All that is known about the active list is that the pages have been
 * accessed more than once in the past.  This means that at any given
 * time there is actually a good chance that pages on the active list
 * are no longer in active use.
 *
 * So when a refault distance of (R - E) is observed and there are at
 * least (R - E) active pages, the refaulting page is activated
 * optimistically in the hope that (R - E) active pages are actually
 * used less frequently than the refaulting page - or even not used at
 * all anymore.
 *
 * That means if inactive cache is refaulting with a suitable refault
 * distance, we assume the cache workingset is transitioning and put
 * pressure on the current active list.
 *
 * If this is wrong and demotion kicks in, the pages which are truly
 * used more frequently will be reactivated while the less frequently
 * used once will be evicted from memory.
 *
 * But if this is right, the stale pages will be pushed out of memory
 * and the used pages get to stay in cache.
 */
/*
 * IAMROOT, 2022.04.30:
 * - papago
 *
 *		Refaulting inactive pages
 *
 * 활성 목록에 대해 알려진 모든 내용은 페이지에 과거에 두 번 이상 액세스했다는
 * 것입니다. 즉, 언제든지 활성 목록에 있는 페이지가 더 이상 활성 상태로 사용되지
 * 않을 가능성이 높습니다.
 *
 * 따라서 (R - E)의 리폴트 거리가 관찰되고 적어도 (R - E) 활성 페이지가 있을 때,
 * (R - E) 활성 페이지가 리폴트 페이지보다 실제로 덜 사용되거나 심지어 더 이상
 * 사용되지 않도록 하기 위해 리폴트 페이지가 최적으로 활성화됩니다.
 *
 * 즉, 비활성 캐시가 적절한 리폴트 거리로 리폴트되는 경우 캐시 workingset이 전환되고
 * 있다고 가정하고 현재 활성 목록에 압력을 가합니다.
 *
 * 만약 이것이 잘못되어 강등이 시작되면, 실제로 더 자주 사용되는 페이지는 다시
 * 활성화되고 덜 자주 사용되는 페이지는 memory에서 eviction될 것이다.
 * 
 * 그러나 이것이 맞다면 오래된 페이지는 메모리에서 밀려나고 사용된 페이지는 캐시에
 * 남게 됩니다.
 */
 /*
 *		Refaulting active pages
 *
 * If on the other hand the refaulting pages have recently been
 * deactivated, it means that the active list is no longer protecting
 * actively used cache from reclaim. The cache is NOT transitioning to
 * a different workingset; the existing workingset is thrashing in the
 * space allocated to the page cache.
 *
 *
 *		Implementation
 *
 * For each node's LRU lists, a counter for inactive evictions and
 * activations is maintained (node->nonresident_age).
 *
 * On eviction, a snapshot of this counter (along with some bits to
 * identify the node) is stored in the now empty page cache
 * slot of the evicted page.  This is called a shadow entry.
 *
 * On cache misses for which there are shadow entries, an eligible
 * refault distance will immediately activate the refaulting page.
 */
/*
 * IAMROOT, 2022.04.30:
 * - papago
 *
 *		Refaulting active pages
 *
 * 반면에 refaulting 페이지가 최근에 비활성화되면 활성 목록이 더 이상 활성 캐시
 * 회수를 방지하지 않음을 의미합니다.
 * (refault 되어 active됬던 page가 또 inactive가 되면 recalim이 될수있다)
 * 캐시가 다른 workingset으로 전환되지 않습니다.
 * 기존 workingset은 페이지 캐시에 할당된 공간을 스래싱하고 있습니다.
 *
 *		Implementation
 *
 * 각 노드의 LRU 목록에 대해 inactive evictions 및 activations 카운터가 유지됩니다
 * (node->nonresident_age).
 *
 * eviction 시 이 카운터의 스냅샷(노드를 식별하기 위한 일부 비트와 함께)이 제거된
 * 페이지의 빈 페이지 캐시 슬롯에 저장됩니다. 이를 shadow entry라고 합니다.
 * 
 * shadow entries이 있는 cache misses 시 적합한 리폴트 거리(짧은)는 리폴트 페이지를
 * 즉시 활성화합니다.
 */

#define WORKINGSET_SHIFT 1
#define EVICTION_SHIFT	((BITS_PER_LONG - BITS_PER_XA_VALUE) +	\
			 WORKINGSET_SHIFT + NODES_SHIFT + \
			 MEM_CGROUP_ID_SHIFT)
#define EVICTION_MASK	(~0UL >> EVICTION_SHIFT)

/*
 * Eviction timestamps need to be able to cover the full range of
 * actionable refaults. However, bits are tight in the xarray
 * entry, and after storing the identifier for the lruvec there might
 * not be enough left to represent every single actionable refault. In
 * that case, we have to sacrifice granularity for distance, and group
 * evictions into coarser buckets by shaving off lower timestamp bits.
 */
/*
 * IAMROOT, 2022.04.30:
 * - 제거 타임스탬프는 실행 가능한 모든 장애 범위를 커버할 수 있어야 합니다.
 *   그러나 xarray 엔트리에서 비트가 빠듯하며, lruvec의 식별자를 저장한 후에는 모든
 *   실행 가능한 리폴트를 나타내기에 충분하지 않을 수 있다. 이 경우 거리에 대한
 *   세분성을 희생하고 더 낮은 타임스탬프 비트를 제거하여 더 거친 버킷으로 evictions를
 *   그룹화해야 한다.
 */
static unsigned int bucket_order __read_mostly;

/*
 * IAMROOT, 2022.04.30:
 * - 만들어지는 eviction
 * - bit     =   31    21         5        1   0
 *   evicion =  |     age | memcgid | nodeid | workingset
 * - return value
 *   return eviction << 1 | 1
 */

static void *pack_shadow(int memcgid, pg_data_t *pgdat, unsigned long eviction,
			 bool workingset)
{
	eviction >>= bucket_order;
	eviction &= EVICTION_MASK;
	eviction = (eviction << MEM_CGROUP_ID_SHIFT) | memcgid;
	eviction = (eviction << NODES_SHIFT) | pgdat->node_id;
	eviction = (eviction << WORKINGSET_SHIFT) | workingset;

	return xa_mk_value(eviction);
}

/*
 * IAMROOT, 2022.04.30:
 * - pack_shadow에서 조립된상태대로 unpack한다.
 */
static void unpack_shadow(void *shadow, int *memcgidp, pg_data_t **pgdat,
			  unsigned long *evictionp, bool *workingsetp)
{
	unsigned long entry = xa_to_value(shadow);
	int memcgid, nid;
	bool workingset;

	workingset = entry & ((1UL << WORKINGSET_SHIFT) - 1);
	entry >>= WORKINGSET_SHIFT;
	nid = entry & ((1UL << NODES_SHIFT) - 1);
	entry >>= NODES_SHIFT;
	memcgid = entry & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);
	entry >>= MEM_CGROUP_ID_SHIFT;

	*memcgidp = memcgid;
	*pgdat = NODE_DATA(nid);
	*evictionp = entry << bucket_order;
	*workingsetp = workingset;
}

/**
 * workingset_age_nonresident - age non-resident entries as LRU ages
 * @lruvec: the lruvec that was aged
 * @nr_pages: the number of pages to count
 *
 * As in-memory pages are aged, non-resident pages need to be aged as
 * well, in order for the refault distances later on to be comparable
 * to the in-memory dimensions. This function allows reclaim and LRU
 * operations to drive the non-resident aging along in parallel.
 */
/*
 * IAMROOT, 2022.04.23:
 * - papgo
 *   workingset_age_nonresident - LRU가 에이징될 때 비거주 항목을 에이징합니다.
 *   @lruvec: the lruvec that was aged
 *   @nr_pages: the numbver of pages to count
 *   카운트할 페이지 수 메모리 내 페이지가 에이징되면 나중에 리폴트 거리가
 *   메모리 내 치수와 비슷해지려면 비거주 페이지도 에이징해야 합니다.
 *   이 기능을 통해 회수 및 LRU 작업을 통해 비거주자 노화를 병렬로 진행할 수
 *   있습니다.
 *
 * - root까지 @nr_pages만큼 nonresident_age를 증가시킨다.
 */
void workingset_age_nonresident(struct lruvec *lruvec, unsigned long nr_pages)
{
	/*
	 * Reclaiming a cgroup means reclaiming all its children in a
	 * round-robin fashion. That means that each cgroup has an LRU
	 * order that is composed of the LRU orders of its child
	 * cgroups; and every page has an LRU position not just in the
	 * cgroup that owns it, but in all of that group's ancestors.
	 *
	 * So when the physical inactive list of a leaf cgroup ages,
	 * the virtual inactive lists of all its parents, including
	 * the root cgroup's, age as well.
	 */
/*
 * IAMROOT, 2022.04.23:
 * - papago
 *   그룹 회수란 모든 하위 그룹을 라운드 로빈 방식으로 회수하는 것을 의미합니다.
 *   즉, 각 cgroup은 하위 그룹의 LRU 순서로 구성된 LRU 순서를 가지고 있으며,
 *   모든 페이지는 해당 그룹을 소유한 cgroup뿐만 아니라 해당 그룹의 모든
 *   상위 그룹에서도 LRU 위치를 가지고 있다.
 *
 *   따라서 리프 cgroup의 물리적 비활성 목록이 에이징되면 루트 cgroup을 포함한
 *   모든 상위 그룹의 가상 비활성 목록도 에이징됩니다.
 */
	do {
		atomic_long_add(nr_pages, &lruvec->nonresident_age);
	} while ((lruvec = parent_lruvec(lruvec)));
}

/**
 * workingset_eviction - note the eviction of a page from memory
 * @target_memcg: the cgroup that is causing the reclaim
 * @page: the page being evicted
 *
 * Return: a shadow entry to be stored in @page->mapping->i_pages in place
 * of the evicted @page so that a later refault can be detected.
 */
/*
 * IAMROOT, 2022.04.30:
 * - @page에 대한 shadow를 만들고 xarray value 결과값을 return받는다.
 *   @target_memcg에서 root까지 lruvec의 nonresident_age를 page수만큼 증가시킨다.
 */
void *workingset_eviction(struct page *page, struct mem_cgroup *target_memcg)
{
	struct pglist_data *pgdat = page_pgdat(page);
	unsigned long eviction;
	struct lruvec *lruvec;
	int memcgid;

	/* Page is fully exclusive and pins page's memory cgroup pointer */
	VM_BUG_ON_PAGE(PageLRU(page), page);
	VM_BUG_ON_PAGE(page_count(page), page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);

	lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	/* XXX: target_memcg can be NULL, go through lruvec */
	memcgid = mem_cgroup_id(lruvec_memcg(lruvec));
/*
 * IAMROOT, 2022.04.30:
 * - 증가시키전의 nonresident_age를 evction으로 가져온다.
 */
	eviction = atomic_long_read(&lruvec->nonresident_age);
/*
 * IAMROOT, 2022.04.30:
 * - root까지 page count만큼 nonresident_age를 증가시켜준다.
 */
	workingset_age_nonresident(lruvec, thp_nr_pages(page));
	return pack_shadow(memcgid, pgdat, eviction, PageWorkingset(page));
}

/**
 * workingset_refault - evaluate the refault of a previously evicted page
 * @page: the freshly allocated replacement page
 * @shadow: shadow entry of the evicted page
 *
 * Calculates and evaluates the refault distance of the previously
 * evicted page in the context of the node and the memcg whose memory
 * pressure caused the eviction.
 */
/*
 * IAMROOT, 2022.04.30:
 * - @page가 refault됬음을 확인하고 @shadow의 데이터를 가져와 refault distance를
 *   구한후 현재 workingset_size값을 구한다. refault distance가 workingset_size에
 *   비해 클 경우 자주사용하지 않은 page라 간주하고, 그게 아닐 경우 activation한다.
 *   (activation을 할때 @shadow에 workingset bit가 set으로 기록되있었다면 set을
 *   시킨다.)
 */
void workingset_refault(struct page *page, void *shadow)
{
	bool file = page_is_file_lru(page);
	struct mem_cgroup *eviction_memcg;
	struct lruvec *eviction_lruvec;
	unsigned long refault_distance;
	unsigned long workingset_size;
	struct pglist_data *pgdat;
	struct mem_cgroup *memcg;
	unsigned long eviction;
	struct lruvec *lruvec;
	unsigned long refault;
	bool workingset;
	int memcgid;

	unpack_shadow(shadow, &memcgid, &pgdat, &eviction, &workingset);

	rcu_read_lock();
	/*
	 * Look up the memcg associated with the stored ID. It might
	 * have been deleted since the page's eviction.
	 *
	 * Note that in rare events the ID could have been recycled
	 * for a new cgroup that refaults a shared page. This is
	 * impossible to tell from the available data. However, this
	 * should be a rare and limited disturbance, and activations
	 * are always speculative anyway. Ultimately, it's the aging
	 * algorithm's job to shake out the minimum access frequency
	 * for the active cache.
	 *
	 * XXX: On !CONFIG_MEMCG, this will always return NULL; it
	 * would be better if the root_mem_cgroup existed in all
	 * configurations instead.
	 */
/*
 * IAMROOT, 2022.04.30:
 * - eviction당시의 memcg를 가져온다.
 */
	eviction_memcg = mem_cgroup_from_id(memcgid);
	if (!mem_cgroup_disabled() && !eviction_memcg)
		goto out;
/*
 * IAMROOT, 2022.04.30:
 * - eviction당시의 lruvec을 가져온다.
 */
	eviction_lruvec = mem_cgroup_lruvec(eviction_memcg, pgdat);
/*
 * IAMROOT, 2022.04.30:
 * - 현재의 eviction값(R)을 가져온다.
 */
	refault = atomic_long_read(&eviction_lruvec->nonresident_age);

	/*
	 * Calculate the refault distance
	 *
	 * The unsigned subtraction here gives an accurate distance
	 * across nonresident_age overflows in most cases. There is a
	 * special case: usually, shadow entries have a short lifetime
	 * and are either refaulted or reclaimed along with the inode
	 * before they get too old.  But it is not impossible for the
	 * nonresident_age to lap a shadow entry in the field, which
	 * can then result in a false small refault distance, leading
	 * to a false activation should this old entry actually
	 * refault again.  However, earlier kernels used to deactivate
	 * unconditionally with *every* reclaim invocation for the
	 * longest time, so the occasional inappropriate activation
	 * leading to pressure on the active list is not a problem.
	 */
/*
 * IAMROOT, 2022.04.30:
 * - evicion : eviction당시의 nonresident_age값.(E)
 * - refault_distance = R - E
 */
	refault_distance = (refault - eviction) & EVICTION_MASK;

	/*
	 * The activation decision for this page is made at the level
	 * where the eviction occurred, as that is where the LRU order
	 * during page reclaim is being determined.
	 *
	 * However, the cgroup that will own the page is the one that
	 * is actually experiencing the refault event.
	 */
	memcg = page_memcg(page);
	lruvec = mem_cgroup_lruvec(memcg, pgdat);

	inc_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + file);

	mem_cgroup_flush_stats();
	/*
	 * Compare the distance to the existing workingset size. We
	 * don't activate pages that couldn't stay resident even if
	 * all the memory was available to the workingset. Whether
	 * workingset competition needs to consider anon or not depends
	 * on having swap.
	 */
/*
 * IAMROOT, 2022.04.30:
 * workingset_size를 구하는 방법.
 * (refault된 page가 속한 영역(file inactive or anon inactive)을 제외한 영역들의
 * size.)
 *
 * - file인 경우.
 *   file inactive를 제외한 나머지 3개.
 *   swap page가 부족한 경우 : file active만.
 *                             swap공간이 부족한 경우 anon측은 건드리지 않는다.
 *
 * - anon인 경우.
 *   anon inactive를 제외한 나머지 3개.
 *   swap page가 부족한경우 : file active + file inactive
 *                            swap공간이 부족한 경우 anon측은 건드리지 않는다.
 *
 * ---
 * - 제일 위 주석에서는 (R - E) + NR_inactive <= NR_active + NR_inactive를 기본으로
 *   설명하는데 실제 code에서는 상대방(file <=> anon)의 active / inactive까지
 *   고려한다.
 *   
 * - file
 *   (R - E) + NR_INACTIVE_FILE <= NR_ACTIVE_FILE + NR_INACTIVE_FILE +
 *                                 NR_ACTIVE_ANON + NR_INACTIVE_ANON
 *  => (R - E) <= NR_ACTIVE_FILE + NR_ACTIVE_ANON + NR_INACTIVE_ANON
 *   
 * - anon
 *   (R - E) + NR_INACTIVE_ANON <= NR_ACTIVE_FILE + NR_INACTIVE_FILE +
 *                                 NR_ACTIVE_ANON + NR_INACTIVE_ANON
 *  => (R - E) <= NR_ACTIVE_ANON + NR_ACTIVE_FILE + NR_INACTIVE_FILE
 */
	workingset_size = lruvec_page_state(eviction_lruvec, NR_ACTIVE_FILE);
	if (!file) {
		workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_INACTIVE_FILE);
	}

/*
 * IAMROOT, 2022.04.30:
 * - swap공간이 존재하면 anon을 고려한다.
 */
	if (mem_cgroup_get_nr_swap_pages(memcg) > 0) {
		workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_ACTIVE_ANON);
		if (file) {
			workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_INACTIVE_ANON);
		}
	}
/*
 * IAMROOT, 2022.04.30:
 * - refault distacne가 길면 사용시간이 길엇던 page라고 간주한다.
 */
	if (refault_distance > workingset_size)
		goto out;

/*
 * IAMROOT, 2022.04.30:
 * - nonresident_age가 증가되는 상황.
 *   1. active할때의 상황.
 *	inactive -> active
 *	(mark_page_accessed())
 *	disk(refault) -> active
 *
 *   2. eviction할때의 상황.
 *	inactive -> disk(eviction)
 *
 *	3. reclaim시 deactive를 안한 active pages
 *	(shrink_active_list()참고)
 */
	SetPageActive(page);
	workingset_age_nonresident(lruvec, thp_nr_pages(page));
	inc_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + file);

	/* Page was active prior to eviction */
	if (workingset) {
		SetPageWorkingset(page);
		/* XXX: Move to lru_cache_add() when it supports new vs putback */
		lru_note_cost_page(page);
		inc_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + file);
	}
out:
	rcu_read_unlock();
}

/**
 * workingset_activation - note a page activation
 * @page: page that is being activated
 */
/*
 * IAMROOT, 2022.04.30:
 * - @page를 activation할때 호출된다. @page가 속한 memcg ~ root까지 lruvec의
 *   nonresident_age를 page 수만큼 증가시킨다.
 */
void workingset_activation(struct page *page)
{
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;

	rcu_read_lock();
	/*
	 * Filter non-memcg pages here, e.g. unmap can call
	 * mark_page_accessed() on VDSO pages.
	 *
	 * XXX: See workingset_refault() - this should return
	 * root_mem_cgroup even for !CONFIG_MEMCG.
	 */
	memcg = page_memcg_rcu(page);
	if (!mem_cgroup_disabled() && !memcg)
		goto out;
	lruvec = mem_cgroup_page_lruvec(page);
	workingset_age_nonresident(lruvec, thp_nr_pages(page));
out:
	rcu_read_unlock();
}

/*
 * Shadow entries reflect the share of the working set that does not
 * fit into memory, so their number depends on the access pattern of
 * the workload.  In most cases, they will refault or get reclaimed
 * along with the inode, but a (malicious) workload that streams
 * through files with a total size several times that of available
 * memory, while preventing the inodes from being reclaimed, can
 * create excessive amounts of shadow nodes.  To keep a lid on this,
 * track shadow nodes and reclaim them when they grow way past the
 * point where they would still be useful.
 */

static struct list_lru shadow_nodes;

void workingset_update_node(struct xa_node *node)
{
	/*
	 * Track non-empty nodes that contain only shadow entries;
	 * unlink those that contain pages or are being freed.
	 *
	 * Avoid acquiring the list_lru lock when the nodes are
	 * already where they should be. The list_empty() test is safe
	 * as node->private_list is protected by the i_pages lock.
	 */
	VM_WARN_ON_ONCE(!irqs_disabled());  /* For __inc_lruvec_page_state */

	if (node->count && node->count == node->nr_values) {
		if (list_empty(&node->private_list)) {
			list_lru_add(&shadow_nodes, &node->private_list);
			__inc_lruvec_kmem_state(node, WORKINGSET_NODES);
		}
	} else {
		if (!list_empty(&node->private_list)) {
			list_lru_del(&shadow_nodes, &node->private_list);
			__dec_lruvec_kmem_state(node, WORKINGSET_NODES);
		}
	}
}

static unsigned long count_shadow_nodes(struct shrinker *shrinker,
					struct shrink_control *sc)
{
	unsigned long max_nodes;
	unsigned long nodes;
	unsigned long pages;

	nodes = list_lru_shrink_count(&shadow_nodes, sc);
	if (!nodes)
		return SHRINK_EMPTY;

	/*
	 * Approximate a reasonable limit for the nodes
	 * containing shadow entries. We don't need to keep more
	 * shadow entries than possible pages on the active list,
	 * since refault distances bigger than that are dismissed.
	 *
	 * The size of the active list converges toward 100% of
	 * overall page cache as memory grows, with only a tiny
	 * inactive list. Assume the total cache size for that.
	 *
	 * Nodes might be sparsely populated, with only one shadow
	 * entry in the extreme case. Obviously, we cannot keep one
	 * node for every eligible shadow entry, so compromise on a
	 * worst-case density of 1/8th. Below that, not all eligible
	 * refaults can be detected anymore.
	 *
	 * On 64-bit with 7 xa_nodes per page and 64 slots
	 * each, this will reclaim shadow entries when they consume
	 * ~1.8% of available memory:
	 *
	 * PAGE_SIZE / xa_nodes / node_entries * 8 / PAGE_SIZE
	 */
#ifdef CONFIG_MEMCG
	if (sc->memcg) {
		struct lruvec *lruvec;
		int i;

		lruvec = mem_cgroup_lruvec(sc->memcg, NODE_DATA(sc->nid));
		for (pages = 0, i = 0; i < NR_LRU_LISTS; i++)
			pages += lruvec_page_state_local(lruvec,
							 NR_LRU_BASE + i);
		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_RECLAIMABLE_B) >> PAGE_SHIFT;
		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_UNRECLAIMABLE_B) >> PAGE_SHIFT;
	} else
#endif
		pages = node_present_pages(sc->nid);

	max_nodes = pages >> (XA_CHUNK_SHIFT - 3);

	if (nodes <= max_nodes)
		return 0;
	return nodes - max_nodes;
}

static enum lru_status shadow_lru_isolate(struct list_head *item,
					  struct list_lru_one *lru,
					  spinlock_t *lru_lock,
					  void *arg) __must_hold(lru_lock)
{
	struct xa_node *node = container_of(item, struct xa_node, private_list);
	struct address_space *mapping;
	int ret;

	/*
	 * Page cache insertions and deletions synchronously maintain
	 * the shadow node LRU under the i_pages lock and the
	 * lru_lock.  Because the page cache tree is emptied before
	 * the inode can be destroyed, holding the lru_lock pins any
	 * address_space that has nodes on the LRU.
	 *
	 * We can then safely transition to the i_pages lock to
	 * pin only the address_space of the particular node we want
	 * to reclaim, take the node off-LRU, and drop the lru_lock.
	 */

	mapping = container_of(node->array, struct address_space, i_pages);

	/* Coming from the list, invert the lock order */
	if (!xa_trylock(&mapping->i_pages)) {
		spin_unlock_irq(lru_lock);
		ret = LRU_RETRY;
		goto out;
	}

	list_lru_isolate(lru, item);
	__dec_lruvec_kmem_state(node, WORKINGSET_NODES);

	spin_unlock(lru_lock);

	/*
	 * The nodes should only contain one or more shadow entries,
	 * no pages, so we expect to be able to remove them all and
	 * delete and free the empty node afterwards.
	 */
	if (WARN_ON_ONCE(!node->nr_values))
		goto out_invalid;
	if (WARN_ON_ONCE(node->count != node->nr_values))
		goto out_invalid;
	xa_delete_node(node, workingset_update_node);
	__inc_lruvec_kmem_state(node, WORKINGSET_NODERECLAIM);

out_invalid:
	xa_unlock_irq(&mapping->i_pages);
	ret = LRU_REMOVED_RETRY;
out:
	cond_resched();
	spin_lock_irq(lru_lock);
	return ret;
}

static unsigned long scan_shadow_nodes(struct shrinker *shrinker,
				       struct shrink_control *sc)
{
	/* list_lru lock nests inside the IRQ-safe i_pages lock */
	return list_lru_shrink_walk_irq(&shadow_nodes, sc, shadow_lru_isolate,
					NULL);
}

static struct shrinker workingset_shadow_shrinker = {
	.count_objects = count_shadow_nodes,
	.scan_objects = scan_shadow_nodes,
	.seeks = 0, /* ->count reports only fully expendable nodes */
	.flags = SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE,
};

/*
 * Our list_lru->lock is IRQ-safe as it nests inside the IRQ-safe
 * i_pages lock.
 */
static struct lock_class_key shadow_nodes_key;

static int __init workingset_init(void)
{
	unsigned int timestamp_bits;
	unsigned int max_order;
	int ret;

	BUILD_BUG_ON(BITS_PER_LONG < EVICTION_SHIFT);
	/*
	 * Calculate the eviction bucket size to cover the longest
	 * actionable refault distance, which is currently half of
	 * memory (totalram_pages/2). However, memory hotplug may add
	 * some more pages at runtime, so keep working with up to
	 * double the initial memory by using totalram_pages as-is.
	 */
/*
 * IAMROOT, 2022.04.30:
 * - timestamp_bits = nonresident_age에 사용될 bits
 * - max_order = memory pages를 표현할 최대 bit수
 * ex) 32bit, node 4 memcg 16, workingset 1, xarray 1
 * 32 - 22 = 10
 * ex) 64bit, node 0 memcg 16, workingset 1, xarray 1
 * 64 - 18 = 46
 * ex) 64bit, node 10 memcg 16, workingset 1, xarray 1
 * 64 - 28 = 36
 */
	timestamp_bits = BITS_PER_LONG - EVICTION_SHIFT;
	max_order = fls_long(totalram_pages() - 1);
/*
 * IAMROOT, 2022.04.30:
 * - memory page수가 nonresident_age에 다 집어넣을수 없는 경우에 이를 적당히
 *   자른다.
 * ex) workingset: timestamp_bits=36 max_order=21 bucket_order=0
 */
	if (max_order > timestamp_bits)
		bucket_order = max_order - timestamp_bits;
	pr_info("workingset: timestamp_bits=%d max_order=%d bucket_order=%u\n",
	       timestamp_bits, max_order, bucket_order);

	ret = prealloc_shrinker(&workingset_shadow_shrinker);
	if (ret)
		goto err;
	ret = __list_lru_init(&shadow_nodes, true, &shadow_nodes_key,
			      &workingset_shadow_shrinker);
	if (ret)
		goto err_list_lru;
	register_shrinker_prepared(&workingset_shadow_shrinker);
	return 0;
err_list_lru:
	free_prealloced_shrinker(&workingset_shadow_shrinker);
err:
	return ret;
}
module_init(workingset_init);
