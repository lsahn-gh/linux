/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#ifndef __ASSEMBLY__
#ifndef __GENERATING_BOUNDS_H

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/threads.h>
#include <linux/numa.h>
#include <linux/init.h>
#include <linux/seqlock.h>
#include <linux/nodemask.h>
#include <linux/pageblock-flags.h>
#include <linux/page-flags-layout.h>
#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/local_lock.h>
#include <asm/page.h>

/* Free memory management - zoned buddy allocator.  */
/*
 * IAMROOT, 2021.11.13:
 * - buddy system에서 사용하는 메모리 승수
 *   2^0 ~ 2^10까지 관리한다는것.
 * - MAX_ORDER_NR_PAGES = 1024
 */
#ifndef CONFIG_FORCE_MAX_ZONEORDER
#define MAX_ORDER 11
#else
#define MAX_ORDER CONFIG_FORCE_MAX_ZONEORDER
#endif
#define MAX_ORDER_NR_PAGES (1 << (MAX_ORDER - 1))

/*
 * PAGE_ALLOC_COSTLY_ORDER is the order at which allocations are deemed
 * costly to service.  That is between allocation orders which should
 * coalesce naturally under reasonable reclaim pressure and those which
 * will not.
 */
/*
 * IAMROOT, 2022.02.12:
 * - buddy system의 단편화 문제를 어느정도 타협하기 위한 order 범위(8 pages)
 */
#define PAGE_ALLOC_COSTLY_ORDER 3

/*
 * IAMROOT, 2021.11.20:
 * - PCP Type이라함은 이 3개를 가리킨다.
 *   PCP Type : buddy system에서 cache역할을 하는 memory.
 *              memory에서 가장 많이 사용하는 type을 이런 cache
 *              로 쓰겟다는것.
 *   MIGRATE_UNMOVABLE : kernel 이 사용하는것과 비슷한 제거 불가능한것.
 *   MIGRATE_MOVABLE : 보통 hotplug로 추가 되는 memory들.
 *   MIGRATE_RECLAIMABLE : 회수가 가능한 memory. disk를 불러올때 
 *                         많은 memory를 load하는 상황등의 memory
 * - MIGRATE_HIGHATOMIC
 *   각 zone마다 예비로 atomic요청에 응답을 빠르게 할수 있도록 하는 공간.
 *   order 0나 1로 오면 대부분 성공하지만, order가 큰 경우 실패를 할수있는데,
 *   이 경우 MIGRATE_HIGHATOMIC 공간에서 할당 요청을 할것이다.
 *
 * - MIGRATE_CMA
 *   kernel에서 연속된 공간을 할당하기엔 매우 어려움이 있는데,
 *   이를 해결하기 위한 공간. driver등(application)에서만 사용하며
 *   kernel에서는 사용하지 않음. 이동(movable, reclimable)이 가능하다.
 *   dt에서 영역을 설정한다.
 *
 * - MIGRATE_ISOLATE
 *   memory를 remove하는 상황에서 잠깐 isolate type등으로 바꾼다.
 *   이동, 삭제등의 작업중의 memory에 접근하지 말라는 의미
 */
enum migratetype {
	MIGRATE_UNMOVABLE,
	MIGRATE_MOVABLE,
	MIGRATE_RECLAIMABLE,
	MIGRATE_PCPTYPES,	/* the number of types on the pcp lists */
	MIGRATE_HIGHATOMIC = MIGRATE_PCPTYPES,
#ifdef CONFIG_CMA
	/*
	 * MIGRATE_CMA migration type is designed to mimic the way
	 * ZONE_MOVABLE works.  Only movable pages can be allocated
	 * from MIGRATE_CMA pageblocks and page allocator never
	 * implicitly change migration type of MIGRATE_CMA pageblock.
	 *
	 * The way to use it is to change migratetype of a range of
	 * pageblocks to MIGRATE_CMA which can be done by
	 * __free_pageblock_cma() function.  What is important though
	 * is that a range of pageblocks must be aligned to
	 * MAX_ORDER_NR_PAGES should biggest page be bigger than
	 * a single pageblock.
	 */
	MIGRATE_CMA,
#endif
#ifdef CONFIG_MEMORY_ISOLATION
	MIGRATE_ISOLATE,	/* can't allocate from here */
#endif
	MIGRATE_TYPES
};

/* In mm/page_alloc.c; keep in sync also with show_migration_types() there */
extern const char * const migratetype_names[MIGRATE_TYPES];

#ifdef CONFIG_CMA
#  define is_migrate_cma(migratetype) unlikely((migratetype) == MIGRATE_CMA)
#  define is_migrate_cma_page(_page) (get_pageblock_migratetype(_page) == MIGRATE_CMA)
#else
#  define is_migrate_cma(migratetype) false
#  define is_migrate_cma_page(_page) false
#endif

static inline bool is_migrate_movable(int mt)
{
	return is_migrate_cma(mt) || mt == MIGRATE_MOVABLE;
}

/*
 * IAMROOT, 2021.12.11:
 * - order 단위로 MIGRATE_TYPES을 순회한다.
 */
#define for_each_migratetype_order(order, type) \
	for (order = 0; order < MAX_ORDER; order++) \
		for (type = 0; type < MIGRATE_TYPES; type++)

/*
 * IAMROOT, 2022.02.26: 
 * 버디 시스템이 각 order에 대한 리스트를 migratetype별로 관리하지만,
 * 메모리가 작은 시스템에서는 disable하여 메모리를 절약해야 한다.
 */
extern int page_group_by_mobility_disabled;

#define MIGRATETYPE_MASK ((1UL << PB_migratetype_bits) - 1)

/*
 * IAMROOT, 2022.03.12:
 * - @page에 소속되있는 pageblock(usemap)에서 MIGRATETYPE을 가져온다.
 */
#define get_pageblock_migratetype(page)					\
	get_pfnblock_flags_mask(page, page_to_pfn(page), MIGRATETYPE_MASK)

/*
 * IAMROOT, 2021.12.11:
 * - zone_init_free_lists에서 초기화된다.
 * - add_to_free_list등에서 add 된다.
 * - nr_free : buddy free page. buddy에서는 free page만을
 *   관리한다.
 */
struct free_area {
	struct list_head	free_list[MIGRATE_TYPES];
	unsigned long		nr_free;
};

/*
 * IAMROOT, 2022.03.05:
 * - order free area의 @migratetype free_list에서 page를 얻어온다.
 */
static inline struct page *get_page_from_free_area(struct free_area *area,
					    int migratetype)
{
	return list_first_entry_or_null(&area->free_list[migratetype],
					struct page, lru);
}

/*
 * IAMROOT, 2022.03.05:
 * - @migratetype에 대해서 free_area가 없다면 true, 아니면 false
 */
static inline bool free_area_empty(struct free_area *area, int migratetype)
{
	return list_empty(&area->free_list[migratetype]);
}

struct pglist_data;

/*
 * Add a wild amount of padding here to ensure data fall into separate
 * cachelines.  There are very few zone structures in the machine, so space
 * consumption is not a concern here.
 */
#if defined(CONFIG_SMP)
struct zone_padding {
	char x[0];
} ____cacheline_internodealigned_in_smp;
#define ZONE_PADDING(name)	struct zone_padding name;
#else
#define ZONE_PADDING(name)
#endif

#ifdef CONFIG_NUMA
enum numa_stat_item {
/*
 * IAMROOT, 2022.03.05:
 * - zone_statistics 참고
 * - NUMA_HIT : 원하는 node에서 할당 받은 경우, 할당된 node에 기록
 *   NUMA_MISS : 원하지 않은 node에서 할당 받은 경우, 할당된 node에 기록
 *   NUMA_FOREIGN : 원하지 않은 zone에서 할당 받은 경우, 원래 원하던 node에 기록
 *   NUMA_LOCAL : 요청한 cpu의 numa와 일치한 node에 할당한경우.(local node)
 *   NUMA_OTHER : remote node에 할당한경우.
 */
	NUMA_HIT,		/* allocated in intended node */
	NUMA_MISS,		/* allocated in non intended node */
	NUMA_FOREIGN,		/* was intended here, hit elsewhere */
	NUMA_INTERLEAVE_HIT,	/* interleaver preferred this zone */
	NUMA_LOCAL,		/* allocation from local node */
	NUMA_OTHER,		/* allocation from other node */
	NR_VM_NUMA_EVENT_ITEMS
};
#else
#define NR_VM_NUMA_EVENT_ITEMS 0
#endif

/*
 * IAMROOT, 2022.03.05:
 * - zone에서 관리하는 counter stat
 */
enum zone_stat_item {
	/* First 128 byte cacheline (assuming 64 bit words) */
/*
 * IAMROOT, 2022.03.05:
 * - buddy system에서 할당가능한 잔여 page
 */
	NR_FREE_PAGES,
	NR_ZONE_LRU_BASE, /* Used only for compaction and reclaim retry */
	NR_ZONE_INACTIVE_ANON = NR_ZONE_LRU_BASE,
	NR_ZONE_ACTIVE_ANON,
	NR_ZONE_INACTIVE_FILE,
	NR_ZONE_ACTIVE_FILE,
	NR_ZONE_UNEVICTABLE,
	NR_ZONE_WRITE_PENDING,	/* Count of dirty, writeback and unstable pages */
	NR_MLOCK,		/* mlock()ed pages found and moved off LRU */
	/* Second 128 byte cacheline */
	NR_BOUNCE,
#if IS_ENABLED(CONFIG_ZSMALLOC)
	NR_ZSPAGES,		/* allocated in zsmalloc */
#endif
	NR_FREE_CMA_PAGES,
	NR_VM_ZONE_STAT_ITEMS };


/*
 * IAMROOT, 2022.03.05:
 * - node에서 관리하는 conter stat
 */
enum node_stat_item {
	NR_LRU_BASE,
	NR_INACTIVE_ANON = NR_LRU_BASE, /* must match order of LRU_[IN]ACTIVE */
	NR_ACTIVE_ANON,		/*  "     "     "   "       "         */
	NR_INACTIVE_FILE,	/*  "     "     "   "       "         */
	NR_ACTIVE_FILE,		/*  "     "     "   "       "         */
	NR_UNEVICTABLE,		/*  "     "     "   "       "         */
	NR_SLAB_RECLAIMABLE_B,
	NR_SLAB_UNRECLAIMABLE_B,
	NR_ISOLATED_ANON,	/* Temporary isolated pages from anon lru */
	NR_ISOLATED_FILE,	/* Temporary isolated pages from file lru */
	WORKINGSET_NODES,
	WORKINGSET_REFAULT_BASE,
	WORKINGSET_REFAULT_ANON = WORKINGSET_REFAULT_BASE,
	WORKINGSET_REFAULT_FILE,
	WORKINGSET_ACTIVATE_BASE,
	WORKINGSET_ACTIVATE_ANON = WORKINGSET_ACTIVATE_BASE,
	WORKINGSET_ACTIVATE_FILE,
	WORKINGSET_RESTORE_BASE,
	WORKINGSET_RESTORE_ANON = WORKINGSET_RESTORE_BASE,
	WORKINGSET_RESTORE_FILE,
	WORKINGSET_NODERECLAIM,
	NR_ANON_MAPPED,	/* Mapped anonymous pages */
	NR_FILE_MAPPED,	/* pagecache pages mapped into pagetables.
			   only modified from process context */
	NR_FILE_PAGES,
	NR_FILE_DIRTY,
	NR_WRITEBACK,
	NR_WRITEBACK_TEMP,	/* Writeback using temporary buffers */
	NR_SHMEM,		/* shmem pages (included tmpfs/GEM pages) */
	NR_SHMEM_THPS,
	NR_SHMEM_PMDMAPPED,
	NR_FILE_THPS,
	NR_FILE_PMDMAPPED,
	NR_ANON_THPS,
	NR_VMSCAN_WRITE,
	NR_VMSCAN_IMMEDIATE,	/* Prioritise for reclaim when writeback ends */
	NR_DIRTIED,		/* page dirtyings since bootup */
	NR_WRITTEN,		/* page writings since bootup */
	NR_KERNEL_MISC_RECLAIMABLE,	/* reclaimable non-slab kernel pages */
	NR_FOLL_PIN_ACQUIRED,	/* via: pin_user_page(), gup flag: FOLL_PIN */
	NR_FOLL_PIN_RELEASED,	/* pages returned via unpin_user_page() */
	NR_KERNEL_STACK_KB,	/* measured in KiB */
#if IS_ENABLED(CONFIG_SHADOW_CALL_STACK)
	NR_KERNEL_SCS_KB,	/* measured in KiB */
#endif
	NR_PAGETABLE,		/* used for pagetables */
#ifdef CONFIG_SWAP
	NR_SWAPCACHE,
#endif
	NR_VM_NODE_STAT_ITEMS
};

/*
 * Returns true if the item should be printed in THPs (/proc/vmstat
 * currently prints number of anon, file and shmem THPs. But the item
 * is charged in pages).
 */
static __always_inline bool vmstat_item_print_in_thp(enum node_stat_item item)
{
	if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
		return false;

	return item == NR_ANON_THPS ||
	       item == NR_FILE_THPS ||
	       item == NR_SHMEM_THPS ||
	       item == NR_SHMEM_PMDMAPPED ||
	       item == NR_FILE_PMDMAPPED;
}

/*
 * Returns true if the value is measured in bytes (most vmstat values are
 * measured in pages). This defines the API part, the internal representation
 * might be different.
 */
static __always_inline bool vmstat_item_in_bytes(int idx)
{
	/*
	 * Global and per-node slab counters track slab pages.
	 * It's expected that changes are multiples of PAGE_SIZE.
	 * Internally values are stored in pages.
	 *
	 * Per-memcg and per-lruvec counters track memory, consumed
	 * by individual slab objects. These counters are actually
	 * byte-precise.
	 */
	return (idx == NR_SLAB_RECLAIMABLE_B ||
		idx == NR_SLAB_UNRECLAIMABLE_B);
}

/*
 * We do arithmetic on the LRU lists in various places in the code,
 * so it is important to keep the active lists LRU_ACTIVE higher in
 * the array than the corresponding inactive lists, and to keep
 * the *_FILE lists LRU_FILE higher than the corresponding _ANON lists.
 *
 * This has to be kept in sync with the statistics in zone_stat_item
 * above and the descriptions in vmstat_text in mm/vmstat.c
 */
#define LRU_BASE 0
#define LRU_ACTIVE 1
#define LRU_FILE 2

/*
 * IAMROOT, 2022.03.26: 
 * free 페이지를 관리하는 버디 시스템과,
 * 반대의 위치에서 할당되어 사용중인 페이지들중 회수가 가능한 페이지들을 
 * 대상으로만 관리하는 회수 관리 시스템에서 사용된다. 이러한 회수 시스템에서
 * lru(least recently used) 알고리즘 기반으로 동작시킨다.
 * 
 * 총 5개의 리스트로 구성되어 있으며, 다음과 같은 의미를 갖는다.
 * inactive anon(0): 자주사용되지 않는 페이지들이 있을 가능성이 많은 
 *                   사용자 페이지들이 관리된다. 
 *   active anon(1): 사용되는 페이지들이 있을 가능성이 많은 
 *                   사용자 페이지들이 관리된다. 
 * inactive file(2): 자주사용되지 않는 페이지들이 있을 가능성이 많은 
 *                   파일 캐시 페이지들이 관리된다. 
 *   active file(3): 사용되는 페이지들이 있을 가능성이 많은 
 *                   파일 캐시 페이지들이 관리된다. 
 * unevictable(4):   회수 시스템에서 관리하지 않을 페이지들이 존재한다.
 */

enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,
	LRU_UNEVICTABLE,
	NR_LRU_LISTS
};

#define for_each_lru(lru) for (lru = 0; lru < NR_LRU_LISTS; lru++)

#define for_each_evictable_lru(lru) for (lru = 0; lru <= LRU_ACTIVE_FILE; lru++)

static inline bool is_file_lru(enum lru_list lru)
{
	return (lru == LRU_INACTIVE_FILE || lru == LRU_ACTIVE_FILE);
}

static inline bool is_active_lru(enum lru_list lru)
{
	return (lru == LRU_ACTIVE_ANON || lru == LRU_ACTIVE_FILE);
}

/*
 * IAMROOT, 2022.04.23:
 * - 0번 : anon
 *   1번 : file
 */
#define ANON_AND_FILE 2

enum lruvec_flags {
	LRUVEC_CONGESTED,		/* lruvec has many dirty pages
					 * backed by a congested BDI
					 */
};

struct lruvec {
	struct list_head		lists[NR_LRU_LISTS];
	/* per lruvec lru_lock for memcg */
	spinlock_t			lru_lock;
	/*
	 * These track the cost of reclaiming one LRU - file or anon -
	 * over the other. As the observed cost of reclaiming one LRU
	 * increases, the reclaim scan balance tips toward the other.
	 */
	unsigned long			anon_cost;
	unsigned long			file_cost;
	/* Non-resident age, driven by LRU movement */
	atomic_long_t			nonresident_age;
	/* Refaults at the time of last reclaim cycle */
	unsigned long			refaults[ANON_AND_FILE];
	/* Various lruvec state flags (enum lruvec_flags) */
	unsigned long			flags;
#ifdef CONFIG_MEMCG
	struct pglist_data *pgdat;
#endif
};

/*
 * IAMROOT, 2022.03.26: 
 * 아래 3가지 요청은 각 비트별로 중북되어 요청될 수 있다.
 * 1) ISOLATE_UNMAPPED:	  umapped only 요청인 경우
 * 2) ISOLATE_ASYNC_MIGRATE: sync migrate 이외의 모든 경우
 * 3) ISOLATE_UNEVICTABLE:   sysctl_compact_unevictable_allowed 설정값에 따라 결정
 *                           rt 커널의 경우 default=0, 그 외의 경우 default=1
 *
 * --- ISOLATE_UNEVICTABLE / Git blame, papago
 *
 * CMA: migrate mlocked pages.
 *
 * Presently CMA cannot migrate mlocked pages so it ends up failing to allocate
 * contiguous memory space.
 *
 * This patch makes mlocked pages be migrated out. Of course, it can affect
 * realtime processes but in CMA usecase, contiguous memory allocation failing
 * is far worse than access latency to an mlocked page being variable while CMA
 * is running. If someone wants to make the system realtime, he shouldn't enable
 * CMA because stalls can still happen at random times.
 *
 * CMA: mlocked 페이지를 마이그레이션합니다.
 *
 * 현재 CMA는 mlocked 페이지를 이행할 수 없기 때문에 연속 메모리 영역 할당에
 * 실패합니다.
 *
 * 이 패치는 잠긴 페이지를 밖으로 마이그레이션합니다. 물론 실시간 프로세스에
 * 영향을 줄 수 있지만 CMA 사용 사례에서 연속 메모리 할당 실패는 CMA 실행 중
 * mlocked 페이지에 대한 액세스 지연보다 훨씬 심각합니다. 시스템을 실시간으로
 * 만들고 싶다면 CMA를 활성화하지 마십시오.그것은 여전히 랜덤한 시간에 정지할 수
 * 있기 때문입니다.
 *
 * - CMA을 사용하는 mlocked pages에 대해 migrate를 하기위한 flag. migrate 도중
 *   process 실행에 영향을 줄수 있기때문에 rt kernel일 경우 default = 0로
 *   하는 듯 보인다.
 */

/* Isolate unmapped pages */
#define ISOLATE_UNMAPPED	((__force isolate_mode_t)0x2)
/* Isolate for asynchronous migration */
#define ISOLATE_ASYNC_MIGRATE	((__force isolate_mode_t)0x4)
/* Isolate unevictable pages */
#define ISOLATE_UNEVICTABLE	((__force isolate_mode_t)0x8)

/* LRU Isolation modes. */
typedef unsigned __bitwise isolate_mode_t;

enum zone_watermarks {
/*
 * IAMROOT, 2022.04.26:
 * - __setup_per_zone_wmarks()에서 설정된다.
 * - compact에서 watermark사용
 *   WMARK_MIN : compact를 해야되는 상황에서
 *				 order <= PAGE_ALLOC_COSTLY_ORDER 인 order에 대해서 주로 사용.
 *				 (__compaction_suitable 참고)
 *   WMARK_LOW : compact를 해야되는 상황에서
 *				 order > PAGE_ALLOC_COSTLY_ORDER인 order에 주로 사용.
 *				 (__compaction_suitable 참고)
 *   WMARK_HIGH : costly order에 대해서 compact를 하는게 확정인 상태
 *				 (order에 대한 할당이 안되는 상태) 인데, reclaim까지 해야 될수
 *				 있는 상황에서, reclaim을 skip하기 위해(reclaim 작업이 오래걸리고
 *				 costly order 할당이면 low ~ high사이정도면 그래도 좀 compact
 *				 할만하다고 생각) 조건을 높일때 사용한다.
 *				 (compaction_ready 참고)
 *
 * - direct reclaim
 *   WMARK_MIN : ZONE_DMA ~ ZONE_NORMAL의 WMARK_MIN 합의 절반보다
 *               free_page가 작으면 kswapd를 깨운다.
 *               (allow_direct_reclaim 참고)
 *               direct reclaim시 throttle 중이였으면 allow_direct_reclaim을 통해
 *               중단 여부를 판별한다.
 *               (throttle_direct_reclaim 참고)
 *
 * - oom
 *   WMARK_HIGH : oom을 하기전에 최후에 WMARK_HIGH로 buddy로 부터 page를 얻는걸
 *				  시도해본다.
 *				  (__alloc_pages_may_oom() 참고)
 */
	WMARK_MIN,
	WMARK_LOW,
	WMARK_HIGH,
	NR_WMARK
};

/*
 * One per migratetype for each PAGE_ALLOC_COSTLY_ORDER plus one additional
 * for pageblock size for THP if configured.
 */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define NR_PCP_THP 1
#else
#define NR_PCP_THP 0
#endif

/*
 * IAMROOT, 2022.02.12:
 * - 3 * (3 + 1 + (1)) = 15
 * - order 0, 1, 2, 3, 9에 대한 각 MIGRATE_PCPTYPES의 list수
 * - index구하는 함수는 order_to_pindex참고
 */
#define NR_PCP_LISTS (MIGRATE_PCPTYPES * (PAGE_ALLOC_COSTLY_ORDER + 1 + NR_PCP_THP))

/*
 * Shift to encode migratetype and order in the same integer, with order
 * in the least significant bits.
 */
#define NR_PCP_ORDER_WIDTH 8
#define NR_PCP_ORDER_MASK ((1<<NR_PCP_ORDER_WIDTH) - 1)

/*
 * IAMROOT, 2022.02.12:
 * - /proc/zoneinfo 의 zone 정보에서 min, low, high에 + boost 값
 */
#define min_wmark_pages(z) (z->_watermark[WMARK_MIN] + z->watermark_boost)
#define low_wmark_pages(z) (z->_watermark[WMARK_LOW] + z->watermark_boost)
#define high_wmark_pages(z) (z->_watermark[WMARK_HIGH] + z->watermark_boost)

/*
 * IAMROOT, 2022.03.05:
 * - @i mark에 대한 page(+ watermark_boost) 개수를 가져온다.
 */
#define wmark_pages(z, i) (z->_watermark[i] + z->watermark_boost)

/* Fields and list protected by pagesets local_lock in page_alloc.c */
struct per_cpu_pages {
/*
 * IAMROOT, 2022.02.12:
 * - count : 현재 가지고 있는 cache pages. 이 page는 lock이 필요없이
 *   가져올수있다. 소진되면 batch만큼 buddy system에 요청한다.
 * - high : 최대 가지고 있을수있는 cache page size
 * - batch : 한번에 buddy system에 add, remove하는 양
 */
	int count;		/* number of pages in the list */
	int high;		/* high watermark, emptying needed */
	int batch;		/* chunk size for buddy add/remove */
	short free_factor;	/* batch scaling factor during free */
#ifdef CONFIG_NUMA
	short expire;		/* When 0, remote pagesets are drained */
#endif

	/* Lists of pages, one per migrate type stored on the pcp-lists */
	struct list_head lists[NR_PCP_LISTS];
};

/*
 * IAMROOT, 2022.03.05:
 * - vm_stat에 합산전인 값들이 들어가있다.
 */
struct per_cpu_zonestat {
#ifdef CONFIG_SMP
	s8 vm_stat_diff[NR_VM_ZONE_STAT_ITEMS];
/*
 * IAMROOT, 2022.03.05:
 * - vm_stat에 vm_stat_diff를 반영할때 한계값으로 사용한다.
 */
	s8 stat_threshold;
#endif
#ifdef CONFIG_NUMA
	/*
	 * Low priority inaccurate counters that are only folded
	 * on demand. Use a large type to avoid the overhead of
	 * folding during refresh_cpu_vm_stats.
	 */
	unsigned long vm_numa_event[NR_VM_NUMA_EVENT_ITEMS];
#endif
};

struct per_cpu_nodestat {
	s8 stat_threshold;
	s8 vm_node_stat_diff[NR_VM_NODE_STAT_ITEMS];
};

#endif /* !__GENERATING_BOUNDS.H */

enum zone_type {
	/*
	 * ZONE_DMA and ZONE_DMA32 are used when there are peripherals not able
	 * to DMA to all of the addressable memory (ZONE_NORMAL).
	 * On architectures where this area covers the whole 32 bit address
	 * space ZONE_DMA32 is used. ZONE_DMA is left for the ones with smaller
	 * DMA addressing constraints. This distinction is important as a 32bit
	 * DMA mask is assumed when ZONE_DMA32 is defined. Some 64-bit
	 * platforms may need both zones as they support peripherals with
	 * different DMA addressing limitations.
	 */
/*
 * IAMROOT, 2021.11.27:
 * - kernel에서 memory를 할당할때 가능한 ZONE_DMA에 할당하지 않도록
 *   노력을 한다
 */
#ifdef CONFIG_ZONE_DMA
	ZONE_DMA,
#endif
#ifdef CONFIG_ZONE_DMA32
	ZONE_DMA32,
#endif
	/*
	 * Normal addressable memory is in ZONE_NORMAL. DMA operations can be
	 * performed on pages in ZONE_NORMAL if the DMA devices support
	 * transfers to all addressable memory.
	 */
	ZONE_NORMAL,
#ifdef CONFIG_HIGHMEM
	/*
	 * A memory area that is only addressable by the kernel through
	 * mapping portions into its own address space. This is for example
	 * used by i386 to allow the kernel to address the memory beyond
	 * 900MB. The kernel will set up special mappings (page
	 * table entries on i386) for each page that the kernel needs to
	 * access.
	 */
	ZONE_HIGHMEM,
#endif
	/*
	 * ZONE_MOVABLE is similar to ZONE_NORMAL, except that it contains
	 * movable pages with few exceptional cases described below. Main use
	 * cases for ZONE_MOVABLE are to make memory offlining/unplug more
	 * likely to succeed, and to locally limit unmovable allocations - e.g.,
	 * to increase the number of THP/huge pages. Notable special cases are:
	 *
	 * 1. Pinned pages: (long-term) pinning of movable pages might
	 *    essentially turn such pages unmovable. Therefore, we do not allow
	 *    pinning long-term pages in ZONE_MOVABLE. When pages are pinned and
	 *    faulted, they come from the right zone right away. However, it is
	 *    still possible that address space already has pages in
	 *    ZONE_MOVABLE at the time when pages are pinned (i.e. user has
	 *    touches that memory before pinning). In such case we migrate them
	 *    to a different zone. When migration fails - pinning fails.
	 * 2. memblock allocations: kernelcore/movablecore setups might create
	 *    situations where ZONE_MOVABLE contains unmovable allocations
	 *    after boot. Memory offlining and allocations fail early.
	 * 3. Memory holes: kernelcore/movablecore setups might create very rare
	 *    situations where ZONE_MOVABLE contains memory holes after boot,
	 *    for example, if we have sections that are only partially
	 *    populated. Memory offlining and allocations fail early.
	 * 4. PG_hwpoison pages: while poisoned pages can be skipped during
	 *    memory offlining, such pages cannot be allocated.
	 * 5. Unmovable PG_offline pages: in paravirtualized environments,
	 *    hotplugged memory blocks might only partially be managed by the
	 *    buddy (e.g., via XEN-balloon, Hyper-V balloon, virtio-mem). The
	 *    parts not manged by the buddy are unmovable PG_offline pages. In
	 *    some cases (virtio-mem), such pages can be skipped during
	 *    memory offlining, however, cannot be moved/allocated. These
	 *    techniques might use alloc_contig_range() to hide previously
	 *    exposed pages from the buddy again (e.g., to implement some sort
	 *    of memory unplug in virtio-mem).
	 * 6. ZERO_PAGE(0), kernelcore/movablecore setups might create
	 *    situations where ZERO_PAGE(0) which is allocated differently
	 *    on different platforms may end up in a movable zone. ZERO_PAGE(0)
	 *    cannot be migrated.
	 * 7. Memory-hotplug: when using memmap_on_memory and onlining the
	 *    memory to the MOVABLE zone, the vmemmap pages are also placed in
	 *    such zone. Such pages cannot be really moved around as they are
	 *    self-stored in the range, but they are treated as movable when
	 *    the range they describe is about to be offlined.
	 *
	 * In general, no unmovable allocations that degrade memory offlining
	 * should end up in ZONE_MOVABLE. Allocators (like alloc_contig_range())
	 * have to expect that migrating pages in ZONE_MOVABLE can fail (even
	 * if has_unmovable_pages() states that there are no unmovable pages,
	 * there can be false negatives).
	 */
	ZONE_MOVABLE,
#ifdef CONFIG_ZONE_DEVICE
/*
 * IAMROOT, 2021.11.20:
 * - device등에서 dram보다 느린 memory를 할당하는경우.
 */
	ZONE_DEVICE,
#endif
	__MAX_NR_ZONES

};

#ifndef __GENERATING_BOUNDS_H

#define ASYNC_AND_SYNC 2

struct zone {
	/* Read-mostly fields */

	/* zone watermarks, access with *_wmark_pages(zone) macros */
/*
 * IAMROOT, 2022.02.12:
 * - /proc/zoneinfo 의 zone 정보에서 min, low, high
 */
	unsigned long _watermark[NR_WMARK];
	unsigned long watermark_boost;

	unsigned long nr_reserved_highatomic;

	/*
	 * We don't know if the memory that we're going to allocate will be
	 * freeable or/and it will be released eventually, so to avoid totally
	 * wasting several GB of ram we must reserve some of the lower zone
	 * memory (otherwise we risk to run OOM on the lower zones despite
	 * there being tons of freeable ram on the higher zones).  This array is
	 * recalculated at runtime if the sysctl_lowmem_reserve_ratio sysctl
	 * changes.
	 */
/*
 * IAMROOT, 2022.04.25:
 * - lowmem 영역을 일정부분(default 1/32 or 1/256(0.32%)) 예약해놓아 일반적인
 *   상황외에 사용하기 위한 제한.
 *   zone 0번부터 축적되는 개념으로 저장되어 제일 높은 zone에 대한 lowmem_reserve
 *   를 검사하면 0번부터의 총 lowmem_reserve를 알 수 있다.
 *   ex) 0번 zone lowmem reserve : 10
 *       1번 zone lowmem reserve : 20
 *       2번 zone lowmem reserve : 40
 *       lowmem_reserve[0] = 10
 *       lowmem_reserve[1] = lowmem_reserve[0] + 20 = 30
 *       lowmem_reserve[2] = lowmem_reserve[1] + 40 = 70
 * - setup함수
 *   setup_per_zone_lowmem_reserve 참고
 * - lowmem_reserve_ratio
 *   Document lowmem_reserve_ratio 참고
 *
 * --- lowmem_reserve_ratio 제어
 *  - 참고
 *  https://support.hpe.com/hpesc/public/docDisplay?docId=c02742536&docLocale=en_US
 *  # cat /proc/sys/vm/lowmem_reserve_ratio
 *  256     256        32
 *  DMA   Normal    HighMem
 *  On normal zone, 256 means ½56. Number of protection pages are reserved.
 *
 * To reserve 1/32 number of protection pages on normal zone:
 *
 * # echo 256 32 32 > /proc/sys/vm/lowmem_reserve_ratio
 * # cat /proc/sys/vm/lowmem_reserve_ratio 256 32 32
 * To set the value permanently, edit /etc/sysctl.conf and add the below:
 *
 * vm.lowmem_reserve_ratio = 256 32 32
 * # sysctl -p
 * # cat /proc/sys/vm/lowmem_reserve_ratio 
 * 256     32     32
 */
	long lowmem_reserve[MAX_NR_ZONES];

#ifdef CONFIG_NUMA
	int node;
#endif
  /*
   * IAMROOT, 2021.12.11:
   * - zone_pcp_init 에서 초기화된다.
   */
	struct pglist_data	*zone_pgdat;
	struct per_cpu_pages	__percpu *per_cpu_pageset;
/*
 * IAMROOT, 2022.03.05:
 * - lock을 걸지 않고 stat을 관리한다. 특정 값 이상이 vm_stat과 차이나면
 *   vm_stat에 적용한다.(threshold 방식)
 */
	struct per_cpu_zonestat	__percpu *per_cpu_zonestats;
	/*
	 * the high and batch values are copied to individual pagesets for
	 * faster access
	 */
	int pageset_high;
	int pageset_batch;

#ifndef CONFIG_SPARSEMEM
	/*
	 * Flags for a pageblock_nr_pages block. See pageblock-flags.h.
	 * In SPARSEMEM, this map is stored in struct mem_section
	 */
	unsigned long		*pageblock_flags;
#endif /* CONFIG_SPARSEMEM */

	/* zone_start_pfn == zone_start_paddr >> PAGE_SHIFT */
	unsigned long		zone_start_pfn;

	/*
	 * spanned_pages is the total pages spanned by the zone, including
	 * holes, which is calculated as:
	 * 	spanned_pages = zone_end_pfn - zone_start_pfn;
	 *
	 * present_pages is physical pages existing within the zone, which
	 * is calculated as:
	 *	present_pages = spanned_pages - absent_pages(pages in holes);
	 *
	 * present_early_pages is present pages existing within the zone
	 * located on memory available since early boot, excluding hotplugged
	 * memory.
	 *
	 * managed_pages is present pages managed by the buddy system, which
	 * is calculated as (reserved_pages includes pages allocated by the
	 * bootmem allocator):
	 *	managed_pages = present_pages - reserved_pages;
	 *
	 * cma pages is present pages that are assigned for CMA use
	 * (MIGRATE_CMA).
	 *
	 * So present_pages may be used by memory hotplug or memory power
	 * management logic to figure out unmanaged pages by checking
	 * (present_pages - managed_pages). And managed_pages should be used
	 * by page allocator and vm scanner to calculate all kinds of watermarks
	 * and thresholds.
	 *
	 * Locking rules:
	 *
	 * zone_start_pfn and spanned_pages are protected by span_seqlock.
	 * It is a seqlock because it has to be read outside of zone->lock,
	 * and it is done in the main allocator path.  But, it is written
	 * quite infrequently.
	 *
	 * The span_seq lock is declared along with zone->lock because it is
	 * frequently read in proximity to zone->lock.  It's good to
	 * give them a chance of being in the same cacheline.
	 *
	 * Write access to present_pages at runtime should be protected by
	 * mem_hotplug_begin/end(). Any reader who can't tolerant drift of
	 * present_pages should get_online_mems() to get a stable value.
	 */
/*
 * IAMROOT, 2021.12.04:
 *
 * - managed_pages : buddy system이 관리하는 pages. 대표적으로
 *   reserved pages(mem_map)를 제외한 것들.
 *   present_pages - managed_pages를 통해 간접적으로 unmanaged
 *   page를 알 수 있다고 주석에 나온다.
 * - cma_pages : 보통 driver를 짜면 cma_pages를 자주쓴다.
 *
 *----
 * calculate_node_totalpages, zone_init_internals 에서 설정된다.
 * - spanned_pages : hole을 포함한 해당 node및 zone의 page수
 * - present_pages : hole을 제외한 해당 node zone의 page수
 * - present_early_pages : 일단 present_pages와 같게 설정된다.
*/
	atomic_long_t		managed_pages;
	unsigned long		spanned_pages;
	unsigned long		present_pages;
#if defined(CONFIG_MEMORY_HOTPLUG)
	unsigned long		present_early_pages;
#endif
#ifdef CONFIG_CMA
	unsigned long		cma_pages;
#endif

	const char		*name;

#ifdef CONFIG_MEMORY_ISOLATION
	/*
	 * Number of isolated pageblock. It is used to solve incorrect
	 * freepage counting problem due to racy retrieving migratetype
	 * of pageblock. Protected by zone->lock.
	 */
	unsigned long		nr_isolate_pageblock;
#endif

#ifdef CONFIG_MEMORY_HOTPLUG
	/* see spanned/present_pages for more description */
	seqlock_t		span_seqlock;
#endif

  /*
   * IAMROOT, 2021.12.11:
   * - zone_init_free_lists에서 set 된다.
   */
	int initialized;

	/* Write-intensive fields used from the page allocator */
	ZONE_PADDING(_pad1_)

/*
 * IAMROOT, 2021.12.11:
 * - zone_init_free_lists에서 초기화된다.
 */
	/* free areas of different sizes */
	struct free_area	free_area[MAX_ORDER];

	/* zone flags, see below */
	unsigned long		flags;

	/* Primarily protects free_area */
	spinlock_t		lock;

	/* Write-intensive fields used by compaction and vmstats. */
	ZONE_PADDING(_pad2_)

	/*
	 * When free pages are below this point, additional steps are taken
	 * when reading the number of free pages to avoid per-cpu counter
	 * drift allowing watermarks to be breached
	 */
/*
 * IAMROOT, 2022.03.05:
 * 커널 메모리 관리 시 남은 free 페이지 수를 읽어 워터마크와 비교하는 것으로
 * 메모리 부족을 판단하는 루틴들이 많이 사용된다. 그런데 정확한 free 페이지 값을
 * 읽어내려면 존 카운터와 per-cpu 카운터를 모두 읽어 더해야하는데, 이렇게 매번
 * 계산을 하는 경우 성능을 떨어뜨리므로, 존 카운터 값만 읽어 high 워터마크보다
 * 일정 기준 더 큰 크기로 설정된 percpu_drift_mark 값과 비교하여 이 값 이하일
 * 때에만 보다 정확한 연산을 하도록 유도하는 방법을 사용하여 성능을 유지시킨다.
 */
	unsigned long percpu_drift_mark;

#if defined CONFIG_COMPACTION || defined CONFIG_CMA
	/* pfn where compaction free scanner should start */
	unsigned long		compact_cached_free_pfn;
	/* pfn where compaction migration scanner should start */
/*
 * IAMROOT, 2022.03.29:
 * - 0 : async에 대한 cache
 *   1 : sync에 대한 cache
 */
	unsigned long		compact_cached_migrate_pfn[ASYNC_AND_SYNC];
	unsigned long		compact_init_migrate_pfn;
	unsigned long		compact_init_free_pfn;
#endif

/*
 * IAMROOT, 2022.03.25:
 * - 아래 값들은 defer_compaction, compaction_defer_reset에서 등에서 설정된다.
 * --- 이전 compaction 결과값 저장
 * - compaction 이 실패했다면 해당 order를 저장한다. 그 이전에도
 *   compaction이 실패했다면 낮은 order가 우선적으로 저장된다.
 *   compaction이 성공하고 실제 할당가능한 상태라면 compaction_defer_reset
 *   함수에서 defer skip조건을 초기화하고 compact조건을 완화한다.
 * --- compaction 지연(defer)
 * - compaction을 하기 전에 compaction을 위한 defer검사를 하는데, 이때
 *   이전 compaction 결과를 참조한다. defer가 충분히 됬다면 defer를 안하고
 *   한번 compaction을 시도해보는 등의 로직이 존재한다.
 */
#ifdef CONFIG_COMPACTION
	/*
	 * On compaction failure, 1<<compact_defer_shift compactions
	 * are skipped before trying again. The number attempted since
	 * last failure is tracked with compact_considered.
	 * compact_order_failed is the minimum compaction failed order.
	 */
/*
 * IAMROOT, 2022.03.25:
 * - 지연 검사(compaction_deferred)를 할때 마다 상승한다. 지연이 많이
 *   됬다면 지연을 안시키는 limit 개념으로 사용한다.
 */
	unsigned int		compact_considered;
/*
 * IAMROOT, 2022.03.25:
 * - defer_compaction에서 설정된다. 지연을 얼마나 시키는지에 대한 limit
 */
	unsigned int		compact_defer_shift;
/*
 * IAMROOT, 2022.03.25:
 * - compact가 실패된 order의 가장 작은 값을 저장한다.
 */
	int			compact_order_failed;
#endif

#if defined CONFIG_COMPACTION || defined CONFIG_CMA
	/* Set to true when the PG_migrate_skip bits should be cleared */
	bool			compact_blockskip_flush;
#endif

	bool			contiguous;

	ZONE_PADDING(_pad3_)
	/* Zone statistics */
/*
 * IAMROOT, 2022.03.05:
 * - 대략적인 값. atomic lock을 걸고 사용한다. per_cpu_zonestat값을 고려해야
 *   정확한 값을 얻을수 있다.
 */
	atomic_long_t		vm_stat[NR_VM_ZONE_STAT_ITEMS];
	atomic_long_t		vm_numa_event[NR_VM_NUMA_EVENT_ITEMS];
} ____cacheline_internodealigned_in_smp;

enum pgdat_flags {
	PGDAT_DIRTY,			/* reclaim scanning has recently found
					 * many dirty file pages at the tail
					 * of the LRU.
					 */
	PGDAT_WRITEBACK,		/* reclaim scanning has recently found
					 * many pages under writeback
					 */
	PGDAT_RECLAIM_LOCKED,		/* prevents concurrent reclaim */
};

enum zone_flags {
	ZONE_BOOSTED_WATERMARK,		/* zone recently boosted watermarks.
					 * Cleared when kswapd is woken.
					 */
	ZONE_RECLAIM_ACTIVE,		/* kswapd may be scanning the zone. */
};

/*
 * IAMROOT, 2022.02.12: 
 * 부트업중 reserved 페이지를 제외하고, 버디 시스템이 관리하는 페이지가
 * 존재하면 해당 페이지 수를 반환한다.
 */
static inline unsigned long zone_managed_pages(struct zone *zone)
{
	return (unsigned long)atomic_long_read(&zone->managed_pages);
}

static inline unsigned long zone_cma_pages(struct zone *zone)
{
#ifdef CONFIG_CMA
	return zone->cma_pages;
#else
	return 0;
#endif
}

static inline unsigned long zone_end_pfn(const struct zone *zone)
{
	return zone->zone_start_pfn + zone->spanned_pages;
}

static inline bool zone_spans_pfn(const struct zone *zone, unsigned long pfn)
{
	return zone->zone_start_pfn <= pfn && pfn < zone_end_pfn(zone);
}

static inline bool zone_is_initialized(struct zone *zone)
{
	return zone->initialized;
}

static inline bool zone_is_empty(struct zone *zone)
{
	return zone->spanned_pages == 0;
}

/*
 * Return true if [start_pfn, start_pfn + nr_pages) range has a non-empty
 * intersection with the given zone
 */
static inline bool zone_intersects(struct zone *zone,
		unsigned long start_pfn, unsigned long nr_pages)
{
	if (zone_is_empty(zone))
		return false;
	if (start_pfn >= zone_end_pfn(zone) ||
	    start_pfn + nr_pages <= zone->zone_start_pfn)
		return false;

	return true;
}

/*
 * The "priority" of VM scanning is how much of the queues we will scan in one
 * go. A value of 12 for DEF_PRIORITY implies that we will scan 1/4096th of the
 * queues ("queue_length >> 12") during an aging round.
 */
#define DEF_PRIORITY 12

/*
 * IAMROOT, 2022.02.12: 
 * ARM64 디폴트 설정 시: 16개 노드 * 4개 존 사용(dma, dma32, normal, movable)
 */

/* Maximum number of zones on a zonelist */
#define MAX_ZONES_PER_ZONELIST (MAX_NUMNODES * MAX_NR_ZONES)

/*
 * IAMROOT, 2022.02.12: 
 * 메모리 할당 시 빠른 fallback 선택을 위해 미리 구성하는 존리스트이며, 
 * 다음과 같이 2개의 fallback용 zonelist를 만들어서 사용한다.
 *
 * - 1) ZONELIST_FALLBACK은 모든 노드와 존을 포함한 존리스트이다. (default)
 *   가장가까운 node 순으로 만들어진다.
 * - 2) ZONELIST_NOFALLBACK은 현재 노드의 존에서만 fallback 존을 찾아 
 *      사용하도록 구성한 존리스트이다. (메모리 할당 요구시 __GFP_THISNODE)
 */
enum {
	ZONELIST_FALLBACK,	/* zonelist with fallback */
#ifdef CONFIG_NUMA
	/*
	 * The NUMA zonelists are doubled because we need zonelists that
	 * restrict the allocations to a single node for __GFP_THISNODE.
	 */
	ZONELIST_NOFALLBACK,	/* zonelist without fallback (__GFP_THISNODE) */
#endif
	MAX_ZONELISTS
};

/*
 * This struct contains information about a zone in a zonelist. It is stored
 * here to avoid dereferences into large structures and lookups of tables
 */
struct zoneref {
	struct zone *zone;	/* Pointer to actual zone */
	int zone_idx;		/* zone_idx(zoneref->zone) */
};

/*
 * One allocation request operates on a zonelist. A zonelist
 * is a list of zones, the first one is the 'goal' of the
 * allocation, the other zones are fallback zones, in decreasing
 * priority.
 *
 * To speed the reading of the zonelist, the zonerefs contain the zone index
 * of the entry being read. Helper functions to access information given
 * a struct zoneref are
 *
 * zonelist_zone()	- Return the struct zone * for an entry in _zonerefs
 * zonelist_zone_idx()	- Return the index of the zone for an entry
 * zonelist_node_idx()	- Return the index of the node for an entry
 */
struct zonelist {
	struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1];
};

/*
 * The array of struct pages for flatmem.
 * It must be declared for SPARSEMEM as well because there are configurations
 * that rely on that.
 */
extern struct page *mem_map;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
struct deferred_split {
	spinlock_t split_queue_lock;
	struct list_head split_queue;
	unsigned long split_queue_len;
};
#endif

/*
 * On NUMA machines, each NUMA node would have a pg_data_t to describe
 * it's memory layout. On UMA machines there is a single pglist_data which
 * describes the whole memory.
 *
 * Memory statistics and page replacement data structures are maintained on a
 * per-zone basis.
 */
typedef struct pglist_data {
	/*
	 * node_zones contains just the zones for THIS node. Not all of the
	 * zones may be populated, but it is the full list. It is referenced by
	 * this node's node_zonelists as well as other node's node_zonelists.
	 */
/*
 * IAMROOT, 2021.12.04:
 * - calculate_node_totalpages에서 초기화 된다.
 *
 * - 해당 node에 대한 zone을 가지고 있으며 'node_zonelists'이 각 원소를 참조한다.
 *   예) ZONE_DMA, NORMAL, HIGHMEM ...
 */
	struct zone node_zones[MAX_NR_ZONES];

	/*
	 * node_zonelists contains references to all zones in all nodes.
	 * Generally the first zones will be references to this node's
	 * node_zones.
	 */
/*
 * IAMROOT, 2022.02.18:
 * - build_zonelists_in_node_order 에서 초기화된다.
 * - ZONELIST_FALLBACK : node in order로 zone의 역순으로 구성된다.
 * - ZONELIST_NOFALLBACK : 해당 node에 대해서만 zone의 역순으로 구성된다.
 *
 * - Node 기반 order 방법
 *   (Node 0 -> Node 2 -> Node 1 ...)
 *
 * - 해당 node가 memoryless라면 위 order 방법에 기반해 가장 근접한 node의
 *   zone들을 가지며 각 zone은 버디 시스템이 관리하는 페이지가 포함된 zone으로
 *   구성된다.
 *
 * - node_zonelist등의 함수로 node_zonelist를 선택한다. __GFP_THISNODE등의
 *   flag의 set여부에 따라 zonelist가 선택될것이다.
 */
	struct zonelist node_zonelists[MAX_ZONELISTS];

	int nr_zones; /* number of populated zones in this node */
#ifdef CONFIG_FLATMEM	/* means !SPARSEMEM */
	struct page *node_mem_map;
#ifdef CONFIG_PAGE_EXTENSION
	struct page_ext *node_page_ext;
#endif
#endif
#if defined(CONFIG_MEMORY_HOTPLUG) || defined(CONFIG_DEFERRED_STRUCT_PAGE_INIT)
	/*
	 * Must be held any time you expect node_start_pfn,
	 * node_present_pages, node_spanned_pages or nr_zones to stay constant.
	 * Also synchronizes pgdat->first_deferred_pfn during deferred page
	 * init.
	 *
	 * pgdat_resize_lock() and pgdat_resize_unlock() are provided to
	 * manipulate node_size_lock without checking for CONFIG_MEMORY_HOTPLUG
	 * or CONFIG_DEFERRED_STRUCT_PAGE_INIT.
	 *
	 * Nests above zone->lock and zone->span_seqlock
	 */
/*
 * IAMROOT, 2021.12.04:
 * - pgdat_init_internals에서 초기화된다.
 */
	spinlock_t node_size_lock;
#endif
/*
 * IAMROOT, 2021.12.04:
 * - node_start_pfn
 *   free_area_init_node에서 초기화된다. nid가 속한 memblock을 찾아서
 *   해당 start_pfn으로 설정된다.
 * -node_present_pages, node_spanned_pages
 *  calculate_node_totalpages에서 초기화된다. node_present_pages에는
 *  node_spanned_pages 에서 hole을 제외한 실제 pages 수가 저장된다.
 */
	unsigned long node_start_pfn;
	unsigned long node_present_pages; /* total number of physical pages */
	unsigned long node_spanned_pages; /* total size of physical page
					     range, including holes */
/*
 * IAMROOT, 2021.12.04:
 * - free_area_init_node에서 초기화된다. 해당 구조체의 nid.
 */
	int node_id;
/*
 * IAMROOT, 2021.12.04:
 * - pgdat_init_internals에서 kswapd_wait, pfmemalloc_wait
 *   가 초기화된다.
 * - kswapd_wait
 *   kswapd가 잠들거나 깨울때 사용한다. direct reclaim시 free page가
 *   많이 부족하면 깨운다.(allow_direct_reclaim 참고)
 */
	wait_queue_head_t kswapd_wait;
/*
 * IAMROOT, 2022.04.26:
 * - throttle_direct_reclaim 에서 wait를 수행한다.
 *   direct reclaim시  throttle이 필요한경우에 사용한다.
 */
	wait_queue_head_t pfmemalloc_wait;
	struct task_struct *kswapd;	/* Protected by
					   mem_hotplug_begin/end() */
	int kswapd_order;
	enum zone_type kswapd_highest_zoneidx;

	int kswapd_failures;		/* Number of 'reclaimed == 0' runs */

#ifdef CONFIG_COMPACTION
	int kcompactd_max_order;
	enum zone_type kcompactd_highest_zoneidx;
	wait_queue_head_t kcompactd_wait;
	struct task_struct *kcompactd;
	bool proactive_compact_trigger;
#endif
	/*
	 * This is a per-node reserve of pages that are not available
	 * to userspace allocations.
	 */
	unsigned long		totalreserve_pages;

#ifdef CONFIG_NUMA
	/*
	 * node reclaim becomes active if more unmapped pages exist.
	 */
	unsigned long		min_unmapped_pages;
	unsigned long		min_slab_pages;
#endif /* CONFIG_NUMA */

	/* Write-intensive fields used by page reclaim */
	ZONE_PADDING(_pad1_)

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
	/*
	 * If memory initialisation on large machines is deferred then this
	 * is the first PFN that needs to be initialised.
	 */
/*
 * IAMROOT, 2021.12.04:
 * - struct page 초기화를 언제 하는지에 대한 판단.
 * - pgdat_set_deferred_range에서 ULONG_MAX로 초기화된다. 즉 defer를 안한다는뜻.
 * - defer_init에서 init 개수 많고 크기가 128MB이상(PAGES_PER_SECTION)일 경우
 *   값이 고쳐진다. 즉 defer후 초기화한다는뜻.
 */
	unsigned long first_deferred_pfn;
#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	struct deferred_split deferred_split_queue;
#endif

	/* Fields commonly accessed by the page reclaim scanner */

	/*
	 * NOTE: THIS IS UNUSED IF MEMCG IS ENABLED.
	 *
	 * Use mem_cgroup_lruvec() to look up lruvecs.
	 */
/*
 * IAMROOT, 2021.12.04:
 * - pgdat_init_internals에서 초기화된다.
 */
	struct lruvec		__lruvec;

	unsigned long		flags;

	ZONE_PADDING(_pad2_)

	/* Per-node vmstats */
/*
 * IAMROOT, 2021.12.04:
 * - free_area_init_core에서 boot_nodestat가 넣어진다.
 */
	struct per_cpu_nodestat __percpu *per_cpu_nodestats;
	atomic_long_t		vm_stat[NR_VM_NODE_STAT_ITEMS];
} pg_data_t;

#define node_present_pages(nid)	(NODE_DATA(nid)->node_present_pages)
#define node_spanned_pages(nid)	(NODE_DATA(nid)->node_spanned_pages)
#ifdef CONFIG_FLATMEM
#define pgdat_page_nr(pgdat, pagenr)	((pgdat)->node_mem_map + (pagenr))
#else
#define pgdat_page_nr(pgdat, pagenr)	pfn_to_page((pgdat)->node_start_pfn + (pagenr))
#endif
#define nid_page_nr(nid, pagenr) 	pgdat_page_nr(NODE_DATA(nid),(pagenr))

#define node_start_pfn(nid)	(NODE_DATA(nid)->node_start_pfn)
#define node_end_pfn(nid) pgdat_end_pfn(NODE_DATA(nid))

/*
 * IAMROOT, 2021.12.11:
 * - node의 end_pfn
 */
static inline unsigned long pgdat_end_pfn(pg_data_t *pgdat)
{
	return pgdat->node_start_pfn + pgdat->node_spanned_pages;
}

static inline bool pgdat_is_empty(pg_data_t *pgdat)
{
	return !pgdat->node_start_pfn && !pgdat->node_spanned_pages;
}

#include <linux/memory_hotplug.h>

void build_all_zonelists(pg_data_t *pgdat);
void wakeup_kswapd(struct zone *zone, gfp_t gfp_mask, int order,
		   enum zone_type highest_zoneidx);
bool __zone_watermark_ok(struct zone *z, unsigned int order, unsigned long mark,
			 int highest_zoneidx, unsigned int alloc_flags,
			 long free_pages);
bool zone_watermark_ok(struct zone *z, unsigned int order,
		unsigned long mark, int highest_zoneidx,
		unsigned int alloc_flags);
bool zone_watermark_ok_safe(struct zone *z, unsigned int order,
		unsigned long mark, int highest_zoneidx);
/*
 * Memory initialization context, use to differentiate memory added by
 * the platform statically or via memory hotplug interface.
 */
enum meminit_context {
	MEMINIT_EARLY,
	MEMINIT_HOTPLUG,
};

extern void init_currently_empty_zone(struct zone *zone, unsigned long start_pfn,
				     unsigned long size);

extern void lruvec_init(struct lruvec *lruvec);

static inline struct pglist_data *lruvec_pgdat(struct lruvec *lruvec)
{
#ifdef CONFIG_MEMCG
	return lruvec->pgdat;
#else
	return container_of(lruvec, struct pglist_data, __lruvec);
#endif
}

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
int local_memory_node(int node_id);
#else
static inline int local_memory_node(int node_id) { return node_id; };
#endif

/*
 * zone_idx() returns 0 for the ZONE_DMA zone, 1 for the ZONE_NORMAL zone, etc.
 */
/*
 * IAMROOT, 2022.02.12: 
 * 요청한 zone 구조체 포인터 주소를 사용하여 존 인덱스를 구한다.
 *
 * 해당 존 배열에서 0번 존을 뺴면 존에 대한 인덱스가 구해진다.
 *	(node)->node_zones[대상존] - (node)->node_zones[0]
 *
 * 예) dma+dma32+normal+movable이 있는 시스템에서 
 *     -> dma32의 인덱스는 1
 */

#define zone_idx(zone)		((zone) - (zone)->zone_pgdat->node_zones)

#ifdef CONFIG_ZONE_DEVICE
static inline bool zone_is_zone_device(struct zone *zone)
{
	return zone_idx(zone) == ZONE_DEVICE;
}
#else
static inline bool zone_is_zone_device(struct zone *zone)
{
	return false;
}
#endif

/*
 * Returns true if a zone has pages managed by the buddy allocator.
 * All the reclaim decisions have to use this function rather than
 * populated_zone(). If the whole zone is reserved then we can easily
 * end up with populated_zone() && !managed_zone().
 */
/*
 * IAMROOT, 2022.02.12: 
 * 부트업중 reserved 페이지를 제외하고, 버디 시스템이 관리하는 페이지가
 * 존재하면 true를 반환한다.
 */
static inline bool managed_zone(struct zone *zone)
{
	return zone_managed_pages(zone);
}

/* Returns true if a zone has memory */
static inline bool populated_zone(struct zone *zone)
{
	return zone->present_pages;
}

#ifdef CONFIG_NUMA
static inline int zone_to_nid(struct zone *zone)
{
	return zone->node;
}

static inline void zone_set_nid(struct zone *zone, int nid)
{
	zone->node = nid;
}
#else
static inline int zone_to_nid(struct zone *zone)
{
	return 0;
}

static inline void zone_set_nid(struct zone *zone, int nid) {}
#endif

extern int movable_zone;

/*
 * IAMROOT, 2021.12.04:
 * - arm64는 high을 쓰지 않는다.
 */
static inline int is_highmem_idx(enum zone_type idx)
{
#ifdef CONFIG_HIGHMEM
	return (idx == ZONE_HIGHMEM ||
		(idx == ZONE_MOVABLE && movable_zone == ZONE_HIGHMEM));
#else
	return 0;
#endif
}

/**
 * is_highmem - helper function to quickly check if a struct zone is a
 *              highmem zone or not.  This is an attempt to keep references
 *              to ZONE_{DMA/NORMAL/HIGHMEM/etc} in general code to a minimum.
 * @zone: pointer to struct zone variable
 * Return: 1 for a highmem zone, 0 otherwise
 */
static inline int is_highmem(struct zone *zone)
{
#ifdef CONFIG_HIGHMEM
	return is_highmem_idx(zone_idx(zone));
#else
	return 0;
#endif
}

/* These two functions are used to setup the per zone pages min values */
struct ctl_table;

int min_free_kbytes_sysctl_handler(struct ctl_table *, int, void *, size_t *,
		loff_t *);
int watermark_scale_factor_sysctl_handler(struct ctl_table *, int, void *,
		size_t *, loff_t *);
extern int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES];
int lowmem_reserve_ratio_sysctl_handler(struct ctl_table *, int, void *,
		size_t *, loff_t *);
int percpu_pagelist_high_fraction_sysctl_handler(struct ctl_table *, int,
		void *, size_t *, loff_t *);
int sysctl_min_unmapped_ratio_sysctl_handler(struct ctl_table *, int,
		void *, size_t *, loff_t *);
int sysctl_min_slab_ratio_sysctl_handler(struct ctl_table *, int,
		void *, size_t *, loff_t *);
int numa_zonelist_order_handler(struct ctl_table *, int,
		void *, size_t *, loff_t *);
extern int percpu_pagelist_high_fraction;
extern char numa_zonelist_order[];
#define NUMA_ZONELIST_ORDER_LEN	16

#ifndef CONFIG_NUMA

extern struct pglist_data contig_page_data;
static inline struct pglist_data *NODE_DATA(int nid)
{
	return &contig_page_data;
}
#define NODE_MEM_MAP(nid)	mem_map

#else /* CONFIG_NUMA */

#include <asm/mmzone.h>

#endif /* !CONFIG_NUMA */

extern struct pglist_data *first_online_pgdat(void);
extern struct pglist_data *next_online_pgdat(struct pglist_data *pgdat);
extern struct zone *next_zone(struct zone *zone);

/**
 * for_each_online_pgdat - helper macro to iterate over all online nodes
 * @pgdat: pointer to a pg_data_t variable
 */
#define for_each_online_pgdat(pgdat)			\
	for (pgdat = first_online_pgdat();		\
	     pgdat;					\
	     pgdat = next_online_pgdat(pgdat))
/**
 * for_each_zone - helper macro to iterate over all memory zones
 * @zone: pointer to struct zone variable
 *
 * The user only needs to declare the zone variable, for_each_zone
 * fills it in.
 */
#define for_each_zone(zone)			        \
	for (zone = (first_online_pgdat())->node_zones; \
	     zone;					\
	     zone = next_zone(zone))

#define for_each_populated_zone(zone)		        \
	for (zone = (first_online_pgdat())->node_zones; \
	     zone;					\
	     zone = next_zone(zone))			\
		if (!populated_zone(zone))		\
			; /* do nothing */		\
		else

static inline struct zone *zonelist_zone(struct zoneref *zoneref)
{
	return zoneref->zone;
}

static inline int zonelist_zone_idx(struct zoneref *zoneref)
{
	return zoneref->zone_idx;
}

static inline int zonelist_node_idx(struct zoneref *zoneref)
{
	return zone_to_nid(zoneref->zone);
}

struct zoneref *__next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes);

/**
 * next_zones_zonelist - Returns the next zone at or below highest_zoneidx within the allowed nodemask using a cursor within a zonelist as a starting point
 * @z: The cursor used as a starting point for the search
 * @highest_zoneidx: The zone index of the highest zone to return
 * @nodes: An optional nodemask to filter the zonelist with
 *
 * This function returns the next zone at or below a given zone index that is
 * within the allowed nodemask using a cursor as the starting point for the
 * search. The zoneref returned is a cursor that represents the current zone
 * being examined. It should be advanced by one before calling
 * next_zones_zonelist again.
 *
 * Return: the next zone at or below highest_zoneidx within the allowed
 * nodemask using a cursor within a zonelist as a starting point
 */
/*
 * IAMROOT, 2022.02.12:
 * @z 시작 zoneref
 * @highest_zoneidx zoneidx 검색범위
 * @nodes 요청 node의 bitmap 예를들어 0b1100 인경우 2, 3node에 대해서만 검색을
 * 수행한다. null일 경우 모든 node에 대해 검색을 수행한다.
 *
 * - 모든 node에 대한 검색일 경우 zone idx만을 먼저 검사해 first zoneref을
 *   return 하거나(fast path) 조건에 맞는 zone을 찾는다.
 */
static __always_inline struct zoneref *next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes)
{
	if (likely(!nodes && zonelist_zone_idx(z) <= highest_zoneidx))
		return z;
	return __next_zones_zonelist(z, highest_zoneidx, nodes);
}

/**
 * first_zones_zonelist - Returns the first zone at or below highest_zoneidx within the allowed nodemask in a zonelist
 * @zonelist: The zonelist to search for a suitable zone
 * @highest_zoneidx: The zone index of the highest zone to return
 * @nodes: An optional nodemask to filter the zonelist with
 *
 * This function returns the first zone at or below a given zone index that is
 * within the allowed nodemask. The zoneref returned is a cursor that can be
 * used to iterate the zonelist with next_zones_zonelist by advancing it by
 * one before calling.
 *
 * When no eligible zone is found, zoneref->zone is NULL (zoneref itself is
 * never NULL). This may happen either genuinely, or due to concurrent nodemask
 * update due to cpuset modification.
 *
 * Return: Zoneref pointer for the first suitable zone found
 */
/*
 * IAMROOT, 2022.02.12:
 * - zonelist에서 highest_zoneidx, nodes에 맞는 가장 처음의 zoneref를 검색한다.
 */
static inline struct zoneref *first_zones_zonelist(struct zonelist *zonelist,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes)
{
	return next_zones_zonelist(zonelist->_zonerefs,
							highest_zoneidx, nodes);
}

/**
 * for_each_zone_zonelist_nodemask - helper macro to iterate over valid zones in a zonelist at or below a given zone index and within a nodemask
 * @zone: The current zone in the iterator
 * @z: The current pointer within zonelist->_zonerefs being iterated
 * @zlist: The zonelist being iterated
 * @highidx: The zone index of the highest zone to return
 * @nodemask: Nodemask allowed by the allocator
 *
 * This iterator iterates though all zones at or below a given zone index and
 * within a given nodemask
 */
#define for_each_zone_zonelist_nodemask(zone, z, zlist, highidx, nodemask) \
	for (z = first_zones_zonelist(zlist, highidx, nodemask), zone = zonelist_zone(z);	\
		zone;							\
		z = next_zones_zonelist(++z, highidx, nodemask),	\
			zone = zonelist_zone(z))

#define for_next_zone_zonelist_nodemask(zone, z, highidx, nodemask) \
	for (zone = z->zone;	\
		zone;							\
		z = next_zones_zonelist(++z, highidx, nodemask),	\
			zone = zonelist_zone(z))


/**
 * for_each_zone_zonelist - helper macro to iterate over valid zones in a zonelist at or below a given zone index
 * @zone: The current zone in the iterator
 * @z: The current pointer within zonelist->zones being iterated
 * @zlist: The zonelist being iterated
 * @highidx: The zone index of the highest zone to return
 *
 * This iterator iterates though all zones at or below a given zone index.
 */
#define for_each_zone_zonelist(zone, z, zlist, highidx) \
	for_each_zone_zonelist_nodemask(zone, z, zlist, highidx, NULL)

#ifdef CONFIG_SPARSEMEM
#include <asm/sparsemem.h>
#endif

#ifdef CONFIG_FLATMEM
#define pfn_to_nid(pfn)		(0)
#endif

#ifdef CONFIG_SPARSEMEM

/*
 * PA_SECTION_SHIFT		physical address to/from section number
 * PFN_SECTION_SHIFT		pfn to/from section number
 */
#define PA_SECTION_SHIFT	(SECTION_SIZE_BITS)

/* IAMROOT, 2021.11.13:
 * - section과 관련된 계산에 사용되는 SHIFT 값.
 *
 *   SECTION_SIZE_BITS: 27
 *   PAGE_SHIFT       : 12 (page size == 4k)
 *   PFN_SECTION_SHIFT: 15 = 27 - 12
 */
#define PFN_SECTION_SHIFT	(SECTION_SIZE_BITS - PAGE_SHIFT)

/* IAMROOT, 2021.11.13:
 * - 전체 section 갯수.
 *
 *   SECTIONS_SHIFT : 21
 *   NR_MEM_SECTIONS: 2,000,000 == 2^21
 */
#define NR_MEM_SECTIONS		(1UL << SECTIONS_SHIFT)

/* IAMROOT, 2021.11.13:
 * - 하나의 section에서 관리하는 nr pages 값.
 *
 *   PFN_SECTION_SHIFT가 15 라면 section 당 32k pages 관리(1 << 15) 하며
 *   48bit, 4kb page 기준으로 하나의 section당 32k * 4KB == 128MB 크기의
 *   물리 메모리를 관리하게 된다.
 */
#define PAGES_PER_SECTION       (1UL << PFN_SECTION_SHIFT)

/* IAMROOT, 2021.11.13:
 * - section에서 관리하는 nr pages (or sizeof(section)) mask 값.
 *
 *   0x7fff (32767) == 0x8000 - 1
 *   ~0x7fff        == 0xffff_ffff_ffff_8000
 */
#define PAGE_SECTION_MASK	(~(PAGES_PER_SECTION-1))

/*
 * IAMROOT, 2021.11.20:
 * - 128MB범위를 관리한다.
 *
 * - pageblock_order = HUGETLB_PAGE_ORDER (9) 라고 가정
 *   (1 << (15 - 9)) * 4 = 2^6 * 4 = 256(bits) = 32byte
 *
 * - 4GB를 관리하는데 필요한 bits 계산.
 *   pageblock_order당 2MB를 커버한다. 2MB 당 4bit
 *   1024 * 2 * 4bit = 8192bits = 1kb
 *   (memmap인경우 64MB. 1.5%정도를사용한다.)
 */
#define SECTION_BLOCKFLAGS_BITS \
	((1UL << (PFN_SECTION_SHIFT - pageblock_order)) * NR_PAGEBLOCK_BITS)

#if (MAX_ORDER - 1 + PAGE_SHIFT) > SECTION_SIZE_BITS
#error Allocator MAX_ORDER exceeds SECTION_SIZE
#endif

/* IAMROOT, 2021.11.13:
 * - @pfn을 입력받아 해당 page frame이 속한 section number를 알아내는 함수.
 */
static inline unsigned long pfn_to_section_nr(unsigned long pfn)
{
	return pfn >> PFN_SECTION_SHIFT;
}

/* IAMROOT, 2021.11.20:
 * - @sec을 입력받아 해당 section이 pointing 하는 첫번째 pfn을 반환하는 함수.
 */
static inline unsigned long section_nr_to_pfn(unsigned long sec)
{
	return sec << PFN_SECTION_SHIFT;
}

#define SECTION_ALIGN_UP(pfn)	(((pfn) + PAGES_PER_SECTION - 1) & PAGE_SECTION_MASK)
#define SECTION_ALIGN_DOWN(pfn)	((pfn) & PAGE_SECTION_MASK)

#define SUBSECTION_SHIFT 21
#define SUBSECTION_SIZE (1UL << SUBSECTION_SHIFT)

/*
 * IAMROOT, 2021.12.04:
 * - 4k page기준. 21 - 12 = 9
 *   PFN_SUBSECTION_SHIFT = 9
 *   PAGES_PER_SUBSECTION = 2^9 = 512
 *   PAGE_SUBSECTION_MASK = ~(0x1ff)
 */
#define PFN_SUBSECTION_SHIFT (SUBSECTION_SHIFT - PAGE_SHIFT)
#define PAGES_PER_SUBSECTION (1UL << PFN_SUBSECTION_SHIFT)
#define PAGE_SUBSECTION_MASK (~(PAGES_PER_SUBSECTION-1))

#if SUBSECTION_SHIFT > SECTION_SIZE_BITS
#error Subsection size exceeds section size
#else
/* IAMROOT, 2021.11.20:
 * - Section 당 Subsection의 갯수 구함.
 *   64 == 2^6 == 1 << (27 - 21)
 */
#define SUBSECTIONS_PER_SECTION (1UL << (SECTION_SIZE_BITS - SUBSECTION_SHIFT))
#endif

#define SUBSECTION_ALIGN_UP(pfn) ALIGN((pfn), PAGES_PER_SUBSECTION)
#define SUBSECTION_ALIGN_DOWN(pfn) ((pfn) & PAGE_SUBSECTION_MASK)

/*
 * IAMROOT, 2021.11.20:
 * - SUBSECTIONS_PER_SECTION이 64이므로 64bit = 8byte
 * - mem_section에 연결되며 subsection은 SUBSECTIONS_PER_SECTION 단위로,
 *   pageblock_flags는 pageblok_order단위로 관리될것이다.
 */
struct mem_section_usage {
#ifdef CONFIG_SPARSEMEM_VMEMMAP
	/*
	 * IAMROOT, 2021.11.20:
	 * - Documentation/vm/memory-model.rst 참고
	 * - 4K page기준 subsection은 0 ~ 63까지의 index를 가진다.
	 *   사용하는 subsection index에 set bit가 된다.
	 */
	DECLARE_BITMAP(subsection_map, SUBSECTIONS_PER_SECTION);
#endif
	/* See declaration of similar field in struct zone */
	/* IAMROOT, 2021.11.20: TODO
	 */
	unsigned long pageblock_flags[0];
};

void subsection_map_init(unsigned long pfn, unsigned long nr_pages);

struct page;
struct page_ext;
/*
 * IAMROOT, 2021.11.13:
 * - 한 section을 관리하기 위한 자료구조. usage, mem_map이 mapping된다.
 * - 4K page일경우 section은 128MB. 32k개의 page를 관리한다.
 * - page는 mem_map이라는 이름으로 section_mem_map에 주소가 mapping된다.
 * - CONFIG_PAGE_EXTENSION이 unset인 경우에
 *   sizeof(struct mem_section) = 16byte
 */
struct mem_section {
	/*
	 * This is, logically, a pointer to an array of struct
	 * pages.  However, it is stored with some other magic.
	 * (see sparse.c::sparse_init_one_section())
	 *
	 * Additionally during early boot we encode node id of
	 * the location of the section here to guide allocation.
	 * (see sparse.c::memory_present())
	 *
	 * Making it a UL at least makes someone do a cast
	 * before using it wrong.
	 */
/*
 * IAMROOT, 2021.11.27:
 * - section mem map에 대한 pointer 겸 해서 flag(SECTION_MAP_MASK)를 저장한다.
 *   bit | 63...32 | 31...15 | 14...5 | 4     | 3     | 2      | 1   | 0       |
 *       | mem map - pfn     | -      | Z-DEV | EARLY | ONLINE | HAS | PRESENT |
 * - 참고 함수
 *   sparse_encode_mem_map
 *  pfn을 offset으로 하여 해당 pfn의 struct page에 바로 접근할수있게 하는
 *  형식이다.
 *
 * - sparse_init일때 잠깐 nid는 mem_section을 초기화 할때만 잠깐 사용한다.
 *   그래서 early nid라고 표현하고, 이 nid 정보는 mem_map 포인터와 
 *   같이 저장되는 일은 없다. early nid의 정보가 담긴 후에 나중에 실제
 *   엔코딩된 mem_map 포인터를 담아 사용한다.
 *   참고로 nid 정보는 mem_map 초기화 이후 page->flags에 저장된다.
 * 
 *   bit | 63....6 | 5 | 4     | 3     | 2      | 1   | 0       |
 *       | nid     | - | Z-DEV | EARLY | ONLINE | HAS | PRESENT |
 *
 * - mem_map
 *   mem_section에 대한 struct page들.
 *   section크기가 128MB, 4k page일경우 section 1개당 32k개의 page를 가져야되니
 *   mem_map은 32k개의 struct page가 되고, struct page가 64byte일경우 32k개의
 *   크기는 2MB가 되어 PMD size와 같아진다.
 */
	unsigned long section_mem_map;

	struct mem_section_usage *usage;
#ifdef CONFIG_PAGE_EXTENSION
	/*
	 * If SPARSEMEM, pgdat doesn't have page_ext pointer. We use
	 * section. (see page_ext.h about this.)
	 */
	struct page_ext *page_ext;
	unsigned long pad;
#endif
	/*
	 * WARNING: mem_section must be a power-of-2 in size for the
	 * calculation and use of SECTION_ROOT_MASK to make sense.
	 */
};

/* IAMROOT, 2021.11.13:
 * - root 당 section 갯수 설정.
 *
 * - SPARSEMEM_EXTREME == enable
 *   256 == 4096 / 16 (PAGE_SIZE / sizeof(struct mem_sections))
 *   section size가 2^27(128MB)인 경우 root 당 32GB (256 * 128MB) 커버.
 *
 *   static인 경우 root를 사용하지 않으므로 1로 설정.
 */
#ifdef CONFIG_SPARSEMEM_EXTREME
#define SECTIONS_PER_ROOT       (PAGE_SIZE / sizeof (struct mem_section))
#else
#define SECTIONS_PER_ROOT	1
#endif

/* IAMROOT, 2024.06.04:
 * - section을 입력받아 해당 section이 소속된 root를 반환한다.
 *   @sec: section
 */
#define SECTION_NR_TO_ROOT(sec)	((sec) / SECTIONS_PER_ROOT)

/* IAMROOT, 2021.11.13:
 * - NR_SECTION_ROOTS: root 개수
 *
 *   ex) 48PA bits, 4k page, static인 경우
 *       2MB == 2MB / 1 (NR_MEM_SECTIONS / SECTIONS_PER_ROOT)
 *   ex) 48PA bits, 4k page, extreme인 경우
 *       8192 == 2MB / 256 (NR_MEM_SECTIONS / SECTIONS_PER_ROOT)
 *       root 한개당 32GB이므로 총 256TB 크기의 영역이다.
 */
#define NR_SECTION_ROOTS	DIV_ROUND_UP(NR_MEM_SECTIONS, SECTIONS_PER_ROOT)
#define SECTION_ROOT_MASK	(SECTIONS_PER_ROOT - 1)

#ifdef CONFIG_SPARSEMEM_EXTREME
extern struct mem_section **mem_section;
#else
extern struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT];
#endif

static inline unsigned long *section_to_usemap(struct mem_section *ms)
{
	return ms->usage->pageblock_flags;
}

/* IAMROOT, 2021.11.13:
 * - @nr (section 번호)에 해당하는 struct mem_section을 반환한다.
 *
 * - extream, 4KB page인 경우 root 한개당 32GB영역(SECTIONS_PER_ROOT)을
 *   커버하고 해당 root는 section size인 128MB씩 8192개(NR_SECTION_ROOTS)로
 *   관리한다.
 */
static inline struct mem_section *__nr_to_section(unsigned long nr)
{
#ifdef CONFIG_SPARSEMEM_EXTREME
	if (!mem_section)
		return NULL;
#endif
	if (!mem_section[SECTION_NR_TO_ROOT(nr)])
		return NULL;
	return &mem_section[SECTION_NR_TO_ROOT(nr)][nr & SECTION_ROOT_MASK];
}
extern size_t mem_section_usage_size(void);

/*
 * We use the lower bits of the mem_map pointer to store
 * a little bit of information.  The pointer is calculated
 * as mem_map - section_nr_to_pfn(pnum).  The result is
 * aligned to the minimum alignment of the two values:
 *   1. All mem_map arrays are page-aligned.
 *   2. section_nr_to_pfn() always clears PFN_SECTION_SHIFT
 *      lowest bits.  PFN_SECTION_SHIFT is arch-specific
 *      (equal SECTION_SIZE_BITS - PAGE_SHIFT), and the
 *      worst combination is powerpc with 256k pages,
 *      which results in PFN_SECTION_SHIFT equal 6.
 * To sum it up, at least 6 bits are available.
 */
/*
 * IAMROOT, 2021.11.13:
 * - 0~5번 bit까지 macro로 대로 사용하고 6bit부터 nid를
 *   저장할려는 용도로 사용한다.
 */
#define SECTION_MARKED_PRESENT		(1UL<<0)
#define SECTION_HAS_MEM_MAP		(1UL<<1)
#define SECTION_IS_ONLINE		(1UL<<2)
#define SECTION_IS_EARLY		(1UL<<3)
#define SECTION_TAINT_ZONE_DEVICE	(1UL<<4)
#define SECTION_MAP_LAST_BIT		(1UL<<5)
/*
 * IAMROOT, 2021.11.27:
 * - 하위 5bit. 0xffff_ffff_ffff_ffe0 
 */
#define SECTION_MAP_MASK		(~(SECTION_MAP_LAST_BIT-1))
#define SECTION_NID_SHIFT		6

static inline struct page *__section_mem_map_addr(struct mem_section *section)
{
	unsigned long map = section->section_mem_map;
	map &= SECTION_MAP_MASK;
	return (struct page *)map;
}

/* IAMROOT, 2021.11.13:
 * - phys memory가 존재하는 section인지 확인하는 함수.
 */
static inline int present_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_MARKED_PRESENT));
}

/* IAMROOT, 2021.11.13:
 * - @nr을 입력받아 phys memory가 존재하는 section인지 확인하는 함수.
 */
static inline int present_section_nr(unsigned long nr)
{
	return present_section(__nr_to_section(nr));
}

/*
 * IAMROOT, 2021.12.18:
 * - 해당 mem_section이 할당된 상태인지 검사하고, 할당되었으면 memmap을 가진지
 *   검사한다.
 * - static 일 경우 section은 항상 존재할테니 memmap이 있는지 flag를 검사해야된다.
 * - extreme일 경우 section이 null인 경우가 있을것이다.
 */
static inline int valid_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_HAS_MEM_MAP));
}

/*
 * IAMROOT, 2021.12.18:
 * - boot time때 sparse_init_nid에서 set됬었다.
 */
static inline int early_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_IS_EARLY));
}

static inline int valid_section_nr(unsigned long nr)
{
	return valid_section(__nr_to_section(nr));
}

static inline int online_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_IS_ONLINE));
}

/*
 * IAMROOT, 2022.03.19:
 * - device online memory인지 확인한다.
 */
static inline int online_device_section(struct mem_section *section)
{
	unsigned long flags = SECTION_IS_ONLINE | SECTION_TAINT_ZONE_DEVICE;

	return section && ((section->section_mem_map & flags) == flags);
}

static inline int online_section_nr(unsigned long nr)
{
	return online_section(__nr_to_section(nr));
}

#ifdef CONFIG_MEMORY_HOTPLUG
void online_mem_sections(unsigned long start_pfn, unsigned long end_pfn);
void offline_mem_sections(unsigned long start_pfn, unsigned long end_pfn);
#endif

static inline struct mem_section *__pfn_to_section(unsigned long pfn)
{
	return __nr_to_section(pfn_to_section_nr(pfn));
}

extern unsigned long __highest_present_section_nr;

/*
 * IAMROOT, 2021.12.04:
 * - SUBSECTION 단위의 subsection index를 가져온다. (0 ~ 63)
 * ex) 0x12345
 * (0x12345 & 0x7fff) / 0x200 
 * = (0x2345) / 0x200 = 0x11 = 17
 *  
 */
static inline int subsection_map_index(unsigned long pfn)
{
	return (pfn & ~(PAGE_SECTION_MASK)) / PAGES_PER_SUBSECTION;
}

/*
 * IAMROOT, 2021.12.18:
 * - subsection_map에서 해당 pfn이 set되있는지 확인한다.
 */
#ifdef CONFIG_SPARSEMEM_VMEMMAP
static inline int pfn_section_valid(struct mem_section *ms, unsigned long pfn)
{
	int idx = subsection_map_index(pfn);

	return test_bit(idx, ms->usage->subsection_map);
}
#else
static inline int pfn_section_valid(struct mem_section *ms, unsigned long pfn)
{
	return 1;
}
#endif

#ifndef CONFIG_HAVE_ARCH_PFN_VALID
/**
 * pfn_valid - check if there is a valid memory map entry for a PFN
 * @pfn: the page frame number to check
 *
 * Check if there is a valid memory map entry aka struct page for the @pfn.
 * Note, that availability of the memory map entry does not imply that
 * there is actual usable memory at that @pfn. The struct page may
 * represent a hole or an unusable page frame.
 *
 * Return: 1 for PFNs that have memory map entries and 0 otherwise
 */
static inline int pfn_valid(unsigned long pfn)
{
	struct mem_section *ms;

	/*
	 * Ensure the upper PAGE_SHIFT bits are clear in the
	 * pfn. Else it might lead to false positives when
	 * some of the upper bits are set, but the lower bits
	 * match a valid pfn.
	 */
	if (PHYS_PFN(PFN_PHYS(pfn)) != pfn)
		return 0;

	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;
	ms = __nr_to_section(pfn_to_section_nr(pfn));
	if (!valid_section(ms))
		return 0;
	/*
	 * Traditionally early sections always returned pfn_valid() for
	 * the entire section-sized span.
	 */
	return early_section(ms) || pfn_section_valid(ms, pfn);
}
#endif

static inline int pfn_in_present_section(unsigned long pfn)
{
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;
	return present_section(__nr_to_section(pfn_to_section_nr(pfn)));
}

/* IAMROOT, 2021.11.13:
 * - @section_nr + 1에서 __highest_present_section_nr까지 loop를 수행하며
 *   phys memory가 존재하는 section을 찾아 section 번호를 반환한다.
 */
static inline unsigned long next_present_section_nr(unsigned long section_nr)
{
	while (++section_nr <= __highest_present_section_nr) {
		if (present_section_nr(section_nr))
			return section_nr;
	}

	return -1;
}

/*
 * These are _only_ used during initialisation, therefore they
 * can use __initdata ...  They could have names to indicate
 * this restriction.
 */
#ifdef CONFIG_NUMA
#define pfn_to_nid(pfn)							\
({									\
	unsigned long __pfn_to_nid_pfn = (pfn);				\
	page_to_nid(pfn_to_page(__pfn_to_nid_pfn));			\
})
#else
#define pfn_to_nid(pfn)		(0)
#endif

void sparse_init(void);
#else
#define sparse_init()	do {} while (0)
#define sparse_index_init(_sec, _nid)  do {} while (0)
#define pfn_in_present_section pfn_valid
#define subsection_map_init(_pfn, _nr_pages) do {} while (0)
#endif /* CONFIG_SPARSEMEM */

#endif /* !__GENERATING_BOUNDS.H */
#endif /* !__ASSEMBLY__ */
#endif /* _LINUX_MMZONE_H */
