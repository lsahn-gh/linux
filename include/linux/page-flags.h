/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Macros for manipulating and testing page->flags
 */

#ifndef PAGE_FLAGS_H
#define PAGE_FLAGS_H

#include <linux/types.h>
#include <linux/bug.h>
#include <linux/mmdebug.h>
#ifndef __GENERATING_BOUNDS_H
#include <linux/mm_types.h>
#include <generated/bounds.h>
#endif /* !__GENERATING_BOUNDS_H */

/*
 * IAMROOT, 2022.03.30:
 * --- non-lru movable page migration 에 대한 내용중 PG_isolated, PG_movable
 *  Git blame / papago
 *   tree 4e3b462d23437d7521081758c2005ae0025978f7
 *   parent c6c919eb90e021fbcfcbfa9dd3d55930cdbb67f9
 *
 * 4. non-lru movable page flags
 *
 * There are two page flags for supporting non-lru movable page.
 *
 * non-lru movable page를 지원하기 위한 두 개의 페이지 플래그가 있습니다.
 *
 * - PG_movable
 *
 * Driver should use the below function to make page movable under page_lock.
 *
 * void __SetPageMovable(struct page *page, struct address_space *mapping).
 *
 * It needs argument of address_space for registering migration family functions
 * which will be called by VM.Exactly speaking, PG_movable is not a real flag
 * of struct page. Rather than, VM reuses page->mapping's lower bits to
 * represent it.
 *
 * #define PAGE_MAPPING_MOVABLE 0x2
 * page->mapping = page->mapping | PAGE_MAPPING_MOVABLE;
 *
 * so driver shouldn't access page->mapping directly. Instead, driver should
 * use page_mapping which mask off the low two bits of page->mapping so it can
 * get right struct address_space.
 *
 * For testing of non-lru movable page, VM supports __PageMovable function.
 * However, it doesn't guarantee to identify non-lru movable page because
 * page->mapping field is unified with other variables in struct page. As well,
 * if driver releases the page after isolation by VM, page->mapping doesn't have
 * stable value although it has PAGE_MAPPING_MOVABLE(Look at __ClearPageMovable).
 * But __PageMovable is cheap to catch whether page is LRU or non-lru movable
 * once the page has been isolated. Because LRU pages never can have
 * PAGE_MAPPING_MOVABLE in page->mapping. It is also good for just peeking to
 * test non-lru movable pages before more expensive checking with lock_page in
 * pfn scanning to select victim.
 *
 * For guaranteeing non-lru movable page, VM provides PageMovable function.
 * Unlike __PageMovable, PageMovable functions validates page->mapping and
 * mapping->a_ops->isolate_page under lock_page. The lock_page prevents sudden
 * destroying of page->mapping.
 *
 * Driver using __SetPageMovable should clear the flag via __ClearMovablePage
 * under page_lock before the releasing the page.
 *
 * page_lock에서 페이지를 이동하려면 다음 기능을 사용해야 합니다.
 *
 *		void __SetPageMovable(struct page *page, struct address_space *mapping)
 *
 * VM에서 호출되는 마이그레이션 패밀리 기능을 등록하려면 address_space 인수가
 * 필요합니다.정확히 말하면, PG_movable은 struct page의 실제 플래그가 아닙니다.
 * VM은 page->mapping의 하위비트를 재사용하여 나타냅니다.
 *
 *		#param PAGE_MAPPING_MOVERABLE 0x2
 *		page->param = page->paraming | PAGE_MAPPING_MOVABLE;
*
 * 따라서 드라이버는 page->mapping에 직접 액세스 할 수 없습니다.
 * 대신 드라이버는 page_mapping을 사용해야 합니다.page_mapping은 page->mapping의
 * 하위2비트를 마스크하여 올바른 struct address_space를 얻을 수 있도록 합니다.
 *
 * non-lru movable page를 테스트하기 위해 VM은 __PageMovable
 * 함수를 지원합니다. 단, page->mapping필드는 struct page 내의 다른 변수와
 * 통일되어 있기 때문에 non-lru movable page를 식별할 수 있는 것은 아닙니다.
 * 또, 드라이버가 VM에 의해서 페이지를 분리한 후에 페이지를 해방하는 경우,
 * 페이지 매핑은 PAGE_MAPPING_MOVABLE이 되어 있어도 안정된 값을 가지지 않습니다
 * (__ClearPageMovable을 참조). 그러나 __PageMovable은 페이지가 분리되면
 * 페이지가 lru / non-lru movable page인지 파악하는 데 비용이 적게 듭니다.
 * LRU 페이지의 페이지에는 PAGE_MAPPING_MOVABLE을 사용할 수 없기 때문입니다.
* 또한 victim을 선택하기 위해 pfn 스캔에서 lock_page를 사용하여 보다 비싼
* 검사를 하기 전에 non-lru movable page인지 훔쳐보는 것만으로 좋습니다.
*
* non-lru page인지 보증하기 위해 VM는 PageMovable 기능을 제공합니다.
* __PageMovable과 달리 PageMovable 함수는 lock_page에서 page->mapping 및
* 매핑->a_ops->isolate_page를 검증합니다. lock_page는 페이지 > 매핑이 갑자기
* 파괴되는 것을 방지합니다.
*
* __SetPageMovable을 사용하는 드라이버는 페이지를 해제하기 전에 page_lock
* 아래의 __ClearMovablePage를 통해 플래그를 클리어해야 합니다.
 *
 * - PG_isolated
 *
 *   To prevent concurrent isolation among several CPUs, VM marks isolated page
 *   as PG_isolated under lock_page. So if a CPU encounters PG_isolated non-lru
 *   movable page, it can skip it. Driver doesn't need to manipulate the flag
 *   because VM will set/clear it automatically. Keep in mind that if driver
 *   sees PG_isolated page, it means the page have been isolated by VM so it
 *   shouldn't touch page.lru field. PG_isolated is alias with PG_reclaim flag
 *   so driver shouldn't use the flag for own purpose.
 *
 *   여러 CPU 간의 동시 분리를 방지하기 위해 VM은 lock_page 아래에 분리된
 *   페이지를 PG_isolated로 표시합니다. 따라서 CPU에서 PG_solated non-ru
 *   movable 페이지가 발견되면 CPU는 이 페이지를 건너뛸 수 있습니다.
 *   VM은 플래그를 자동으로 설정/삭제하므로 드라이버는 플래그를 조작할 필요가
 *   없습니다. 드라이버가 PG_isolated 페이지를 표시할 경우 VM에 의해 페이지가
 *   분리되었기 때문에 page.lru 필드를 터치하지 않아야 한다는 것을 의미합니다.
 *   PG_isolated는 PG_reclaim 플래그가 있는 별칭이므로 운전자는 플래그를
 *   자신의 목적으로 사용할 수 없습니다.
 */
/*
 * Various page->flags bits:
 *
 * PG_reserved is set for special pages. The "struct page" of such a page
 * should in general not be touched (e.g. set dirty) except by its owner.
 * Pages marked as PG_reserved include:
 * - Pages part of the kernel image (including vDSO) and similar (e.g. BIOS,
 *   initrd, HW tables)
 * - Pages reserved or allocated early during boot (before the page allocator
 *   was initialized). This includes (depending on the architecture) the
 *   initial vmemmap, initial page tables, crashkernel, elfcorehdr, and much
 *   much more. Once (if ever) freed, PG_reserved is cleared and they will
 *   be given to the page allocator.
 * - Pages falling into physical memory gaps - not IORESOURCE_SYSRAM. Trying
 *   to read/write these pages might end badly. Don't touch!
 * - The zero page(s)
 * - Pages not added to the page allocator when onlining a section because
 *   they were excluded via the online_page_callback() or because they are
 *   PG_hwpoison.
 * - Pages allocated in the context of kexec/kdump (loaded kernel image,
 *   control pages, vmcoreinfo)
 * - MMIO/DMA pages. Some architectures don't allow to ioremap pages that are
 *   not marked PG_reserved (as they might be in use by somebody else who does
 *   not respect the caching strategy).
 * - Pages part of an offline section (struct pages of offline sections should
 *   not be trusted as they will be initialized when first onlined).
 * - MCA pages on ia64
 * - Pages holding CPU notes for POWER Firmware Assisted Dump
 * - Device memory (e.g. PMEM, DAX, HMM)
 * Some PG_reserved pages will be excluded from the hibernation image.
 * PG_reserved does in general not hinder anybody from dumping or swapping
 * and is no longer required for remap_pfn_range(). ioremap might require it.
 * Consequently, PG_reserved for a page mapped into user space can indicate
 * the zero page, the vDSO, MMIO pages or device memory.
 *
 * The PG_private bitflag is set on pagecache pages if they contain filesystem
 * specific data (which is normally at page->private). It can be used by
 * private allocations for its own usage.
 *
 * During initiation of disk I/O, PG_locked is set. This bit is set before I/O
 * and cleared when writeback _starts_ or when read _completes_. PG_writeback
 * is set before writeback starts and cleared when it finishes.
 *
 * PG_locked also pins a page in pagecache, and blocks truncation of the file
 * while it is held.
 *
 * page_waitqueue(page) is a wait queue of all tasks waiting for the page
 * to become unlocked.
 *
 * PG_swapbacked is set when a page uses swap as a backing storage.  This are
 * usually PageAnon or shmem pages but please note that even anonymous pages
 * might lose their PG_swapbacked flag when they simply can be dropped (e.g. as
 * a result of MADV_FREE).
 *
 * PG_uptodate tells whether the page's contents is valid.  When a read
 * completes, the page becomes uptodate, unless a disk I/O error happened.
 *
 * PG_referenced, PG_reclaim are used for page reclaim for anonymous and
 * file-backed pagecache (see mm/vmscan.c).
 *
 * PG_error is set to indicate that an I/O error occurred on this page.
 *
 * PG_arch_1 is an architecture specific page state bit.  The generic code
 * guarantees that this bit is cleared for a page when it first is entered into
 * the page cache.
 *
 * PG_hwpoison indicates that a page got corrupted in hardware and contains
 * data with incorrect ECC bits that triggered a machine check. Accessing is
 * not safe since it may cause another machine check. Don't touch!
 */

/*
 * Don't use the pageflags directly.  Use the PageFoo macros.
 *
 * The page flags field is split into two parts, the main flags area
 * which extends from the low bits upwards, and the fields area which
 * extends from the high bits downwards.
 *
 *  | FIELD | ... | FLAGS |
 *  N-1           ^       0
 *               (NR_PAGEFLAGS)
 *
 * The fields area is reserved for fields mapping zone, node (for NUMA) and
 * SPARSEMEM section (for variants of SPARSEMEM that require section ids like
 * SPARSEMEM_EXTREME with !SPARSEMEM_VMEMMAP).
 */
enum pageflags {
	PG_locked,		/* Page is locked. Don't touch. */
	PG_referenced,
	PG_uptodate,
	PG_dirty,
	PG_lru,
	PG_active,
	PG_workingset,
	PG_waiters,		/* Page has waiters, check its waitqueue. Must be bit #7 and in the same byte as "PG_locked" */
	PG_error,
	PG_slab,
	PG_owner_priv_1,	/* Owner use. If pagecache, fs may use*/
	PG_arch_1,
	PG_reserved,
	PG_private,		/* If pagecache, has fs-private data */
	PG_private_2,		/* If pagecache, has fs aux data */
	PG_writeback,		/* Page is under writeback */
	PG_head,		/* A head page */
	PG_mappedtodisk,	/* Has blocks allocated on-disk */
	PG_reclaim,		/* To be reclaimed asap */
	PG_swapbacked,		/* Page is backed by RAM/swap */
	PG_unevictable,		/* Page is "unevictable"  */
#ifdef CONFIG_MMU
	PG_mlocked,		/* Page is vma mlocked */
#endif
#ifdef CONFIG_ARCH_USES_PG_UNCACHED
	PG_uncached,		/* Page has been mapped as uncached */
#endif
#ifdef CONFIG_MEMORY_FAILURE
	PG_hwpoison,		/* hardware poisoned page. Don't touch */
#endif
#if defined(CONFIG_PAGE_IDLE_FLAG) && defined(CONFIG_64BIT)
	PG_young,
	PG_idle,
#endif
#ifdef CONFIG_64BIT
	PG_arch_2,
#endif
#ifdef CONFIG_KASAN_HW_TAGS
	PG_skip_kasan_poison,
#endif
	__NR_PAGEFLAGS,

	/* Filesystems */
	PG_checked = PG_owner_priv_1,

	/* SwapBacked */
	PG_swapcache = PG_owner_priv_1,	/* Swap page: swp_entry_t in private */

	/* Two page bits are conscripted by FS-Cache to maintain local caching
	 * state.  These bits are set on pages belonging to the netfs's inodes
	 * when those inodes are being locally cached.
	 */
	PG_fscache = PG_private_2,	/* page backed by cache */

	/* XEN */
	/* Pinned in Xen as a read-only pagetable page. */
	PG_pinned = PG_owner_priv_1,
	/* Pinned as part of domain save (see xen_mm_pin_all()). */
	PG_savepinned = PG_dirty,
	/* Has a grant mapping of another (foreign) domain's page. */
	PG_foreign = PG_owner_priv_1,
	/* Remapped by swiotlb-xen. */
	PG_xen_remapped = PG_owner_priv_1,

	/* SLOB */
	PG_slob_free = PG_private,

	/* Compound pages. Stored in first tail page's flags */
	PG_double_map = PG_workingset,

#ifdef CONFIG_MEMORY_FAILURE
	/*
	 * Compound pages. Stored in first tail page's flags.
	 * Indicates that at least one subpage is hwpoisoned in the
	 * THP.
	 */
	PG_has_hwpoisoned = PG_mappedtodisk,
#endif

	/* non-lru isolated movable page */
	PG_isolated = PG_reclaim,

	/* Only valid for buddy pages. Used to track pages that are reported */
	PG_reported = PG_uptodate,
};

#define PAGEFLAGS_MASK		((1UL << NR_PAGEFLAGS) - 1)

#ifndef __GENERATING_BOUNDS_H

/*
 * IAMROOT, 2022.03.05:
 * - compound head 설정을 했을때 head + 1을 했었다.
 */
static inline unsigned long _compound_head(const struct page *page)
{
	unsigned long head = READ_ONCE(page->compound_head);

	if (unlikely(head & 1))
		return head - 1;
	return (unsigned long)page;
}

#define compound_head(page)	((typeof(page))_compound_head(page))

/*
 * IAMROOT, 2022.03.11:
 * tail page의 경우엔 compound_head에 head 주소 + 1이 설정되었을 것이므로
 * 0번 bit가 set되어 있을것이다.
 */
static __always_inline int PageTail(struct page *page)
{
	return READ_ONCE(page->compound_head) & 1;
}

/*
 * IAMROOT, 2022.02.26: 
 * compound 페이지 여부를 체크한다.
 * compound 페이지의 경우 head 페이지의 플래그에 PG_head가 존재하고,
 * 나머지 페이지의 플래그에는 PG_tail이 존재한다.
 *
 * - compound page일 경우, head는 prep_compound_page함수에서 __SetPageHead 함수를
 *   통해 flags에 PG_HEAD가 set됫을 것이고, tail page의 경우엔 compound_head에
 *   head 주소 + 1이 설정되었을 것이므로 0번 bit가 set되어 있을것이다.
 */
static __always_inline int PageCompound(struct page *page)
{
	return test_bit(PG_head, &page->flags) || PageTail(page);
}

#define	PAGE_POISON_PATTERN	-1l
static inline int PagePoisoned(const struct page *page)
{
	return page->flags == PAGE_POISON_PATTERN;
}

#ifdef CONFIG_DEBUG_VM
void page_init_poison(struct page *page, size_t size);
#else
static inline void page_init_poison(struct page *page, size_t size)
{
}
#endif

/*
 * Page flags policies wrt compound pages
 *
 * PF_POISONED_CHECK
 *     check if this struct page poisoned/uninitialized
 *
 * PF_ANY:
 *     the page flag is relevant for small, head and tail pages.
 *
 * PF_HEAD:
 *     for compound page all operations related to the page flag applied to
 *     head page.
 *
 * PF_ONLY_HEAD:
 *     for compound page, callers only ever operate on the head page.
 *
 * PF_NO_TAIL:
 *     modifications of the page flag must be done on small or head pages,
 *     checks can be done on tail pages too.
 *
 * PF_NO_COMPOUND:
 *     the page flag is not relevant for compound pages.
 *
 * PF_SECOND:
 *     the page flag is stored in the first tail page.
 */
/*
 * IAMROOT, 2021.12.11:
 * - order로 page를 할당할때 struct page가 head, tail등이 될수있다.
 */
#define PF_POISONED_CHECK(page) ({					\
		VM_BUG_ON_PGFLAGS(PagePoisoned(page), page);		\
		page; })
#define PF_ANY(page, enforce)	PF_POISONED_CHECK(page)
#define PF_HEAD(page, enforce)	PF_POISONED_CHECK(compound_head(page))
#define PF_ONLY_HEAD(page, enforce) ({					\
		VM_BUG_ON_PGFLAGS(PageTail(page), page);		\
		PF_POISONED_CHECK(page); })
#define PF_NO_TAIL(page, enforce) ({					\
		VM_BUG_ON_PGFLAGS(enforce && PageTail(page), page);	\
		PF_POISONED_CHECK(compound_head(page)); })
#define PF_NO_COMPOUND(page, enforce) ({				\
		VM_BUG_ON_PGFLAGS(enforce && PageCompound(page), page);	\
		PF_POISONED_CHECK(page); })
#define PF_SECOND(page, enforce) ({					\
		VM_BUG_ON_PGFLAGS(!PageHead(page), page);		\
		PF_POISONED_CHECK(&page[1]); })

/*
 * Macros to create function definitions for page flags
 */
#define TESTPAGEFLAG(uname, lname, policy)				\
static __always_inline int Page##uname(struct page *page)		\
	{ return test_bit(PG_##lname, &policy(page, 0)->flags); }

#define SETPAGEFLAG(uname, lname, policy)				\
static __always_inline void SetPage##uname(struct page *page)		\
	{ set_bit(PG_##lname, &policy(page, 1)->flags); }

#define CLEARPAGEFLAG(uname, lname, policy)				\
static __always_inline void ClearPage##uname(struct page *page)		\
	{ clear_bit(PG_##lname, &policy(page, 1)->flags); }

#define __SETPAGEFLAG(uname, lname, policy)				\
static __always_inline void __SetPage##uname(struct page *page)		\
	{ __set_bit(PG_##lname, &policy(page, 1)->flags); }

#define __CLEARPAGEFLAG(uname, lname, policy)				\
static __always_inline void __ClearPage##uname(struct page *page)	\
	{ __clear_bit(PG_##lname, &policy(page, 1)->flags); }

#define TESTSETFLAG(uname, lname, policy)				\
static __always_inline int TestSetPage##uname(struct page *page)	\
	{ return test_and_set_bit(PG_##lname, &policy(page, 1)->flags); }

#define TESTCLEARFLAG(uname, lname, policy)				\
static __always_inline int TestClearPage##uname(struct page *page)	\
	{ return test_and_clear_bit(PG_##lname, &policy(page, 1)->flags); }

#define PAGEFLAG(uname, lname, policy)					\
	TESTPAGEFLAG(uname, lname, policy)				\
	SETPAGEFLAG(uname, lname, policy)				\
	CLEARPAGEFLAG(uname, lname, policy)

#define __PAGEFLAG(uname, lname, policy)				\
	TESTPAGEFLAG(uname, lname, policy)				\
	__SETPAGEFLAG(uname, lname, policy)				\
	__CLEARPAGEFLAG(uname, lname, policy)

#define TESTSCFLAG(uname, lname, policy)				\
	TESTSETFLAG(uname, lname, policy)				\
	TESTCLEARFLAG(uname, lname, policy)

#define TESTPAGEFLAG_FALSE(uname)					\
static inline int Page##uname(const struct page *page) { return 0; }

#define SETPAGEFLAG_NOOP(uname)						\
static inline void SetPage##uname(struct page *page) {  }

#define CLEARPAGEFLAG_NOOP(uname)					\
static inline void ClearPage##uname(struct page *page) {  }

#define __CLEARPAGEFLAG_NOOP(uname)					\
static inline void __ClearPage##uname(struct page *page) {  }

#define TESTSETFLAG_FALSE(uname)					\
static inline int TestSetPage##uname(struct page *page) { return 0; }

#define TESTCLEARFLAG_FALSE(uname)					\
static inline int TestClearPage##uname(struct page *page) { return 0; }

#define PAGEFLAG_FALSE(uname) TESTPAGEFLAG_FALSE(uname)			\
	SETPAGEFLAG_NOOP(uname) CLEARPAGEFLAG_NOOP(uname)

#define TESTSCFLAG_FALSE(uname)						\
	TESTSETFLAG_FALSE(uname) TESTCLEARFLAG_FALSE(uname)

/*
 * IAMROOT, 2021.12.11:
 * - __PAGEFLAG 선언시 만들어지는 함수들. (Locked로 예로 듬)
 *   PageLocked
 *   __SetPageLocked
 *   __ClearPageLocked
 *   PG_locked bit가 제어된다.
 *
 * - PAGEFLAG 선언시 만들어지는 함수들. (Locked로 예로 듬)
 *   PageLocked
 *   SetPageLocked
 *   ClearPageLocked
 *   PG_locked bit가 제어된다.
 *
 * - __유무
 *   __가 없는건 atomic, __가 있는건 none atomic
 *
 * - 함수 정리
 *
 * t : test, c : clear, s : set
 *                 | atomic          | none atomic 
 *-----------------+-----------------+---------------+
 * define          | s | c | ts | tc | t | __s | __c |
 *-----------------+---|---|----+----+---+-----+-----+
 * TESTPAGEFLAG    |   |   |    |    | o |     |     |
 * ----------------+---------------------------------+
 * __SETPAGEFLAG   |   |   |    |    |   | o   |     |
 * __CLEARPAGEFLAG |   |   |    |    |   |     | o   |
 * __PAGEFLAG      |   |   |    |    | o | o   | o   |  
 * ----------------+---------------------------------+
 * SETPAGEFLAG     | o |   |    |    |   |     |     |
 * CLEARPAGEFLAG   |   | o |    |    |   |     |     |
 * PAGEFLAG        | o | o |    |    | o |     |     |
 * ----------------+---------------------------------+
 * TESTCLEARFLAG   |   |   |    | o  |   |     |     |
 * TESTSETFLAG     |   |   | o  |    |   |     |     |
 * TESTSCFLAG      |   |   | o  | o  |   |     |     |
 * ----------------+---------------------------------+
 *
 *                 | atomic          | none atomic
 * ----------------+-----------------+---------------+
 * lname           | s | c | ts | tc | t | __s | __c | policy
 * ----------------+---+---+----+----+---+-----+----+-------------
 * locked          |   |   |    |    | o | o   | o   | PF_NO_TAIL 
 * waiters         | o | o |    |    | o |     | o   | PF_ONLY_HEAD
 * error           | o | o |    | o  | o |     |     | PF_NO_TAIL
 * referenced      | o | o |    | o  | o | o   |     | PF_HEAD
 * dirty           | o | o | o  | o  | o |     | o   | PF_HEAD
 * lru             | o | o |    | o  | o |     | o   | PF_HEAD
 * active          | o | o |    | o  | o |     | o   | PH_HEAD
 * workingset      | o | o |    | o  | o |     |     | PH_HEAD
 * slab            |   |   |    |    | o | o   | o   | PF_NO_TAIL
 * slob_free       |   |   |    |    | o | o   | o   | PF_NO_TAIL
 * checked         | o | o |    |    | o |     |     | PF_NO_COMPOUND
 * pinned          | o | o | o  | o  | o |     |     | PF_NO_COMPOUND
 * savepinned      | o | o |    |    | o |     |     | PF_NO_COMPOUND
 * foreign         | o | o |    |    | o |     |     | PF_NO_COMPOUND
 * xen_remapped    | o | o |    | o  | o |     |     | PF_NO_COMPOUND
 * reserved        | o | o |    |    | o | o   | o   | PF_NO_COMPOUND
 * swapbacked      | o | o |    |    | o | o   | o   | PF_NO_TAIL
 * private         | o | o |    |    | o |     |     | PF_ANY
 * private_2       | o | o | o  | o  | o |     |     | PF_ANY
 * onwer_priv_1    | o | o |    | o  | o |     |     | PF_ANY
 * writeback       |   |   | o  | o  | o |     |     | PF_NO_TAIL
 * mappedtodisk    | o | o |    |    | o |     |     | PF_NO_TAIL
 * ----------------+---------------------------------+---------------
 * reclaim(Reclaim)| o | o |    | o  | o |     |     | PF_NO_TAIL
 * reclaim         | o | o |    | o  | o |     |     | PF_NO_COMPOUND
 * (Readahead)     |---------------------------------+--------------
 */
__PAGEFLAG(Locked, locked, PF_NO_TAIL)
PAGEFLAG(Waiters, waiters, PF_ONLY_HEAD) __CLEARPAGEFLAG(Waiters, waiters, PF_ONLY_HEAD)
PAGEFLAG(Error, error, PF_NO_TAIL) TESTCLEARFLAG(Error, error, PF_NO_TAIL)
PAGEFLAG(Referenced, referenced, PF_HEAD)
	TESTCLEARFLAG(Referenced, referenced, PF_HEAD)
	__SETPAGEFLAG(Referenced, referenced, PF_HEAD)

/*
 * IAMROOT, 2022.04.09:
 * - page를 기록중인 상태면 set.
 */
PAGEFLAG(Dirty, dirty, PF_HEAD) TESTSCFLAG(Dirty, dirty, PF_HEAD)
	__CLEARPAGEFLAG(Dirty, dirty, PF_HEAD)
PAGEFLAG(LRU, lru, PF_HEAD) __CLEARPAGEFLAG(LRU, lru, PF_HEAD)
	TESTCLEARFLAG(LRU, lru, PF_HEAD)
PAGEFLAG(Active, active, PF_HEAD) __CLEARPAGEFLAG(Active, active, PF_HEAD)
	TESTCLEARFLAG(Active, active, PF_HEAD)
/*
 * IAMROOT, 2022.04.23:
 * - set
 *   reclaim중 active page를 deactive할때 set된다.(shrink_active_list())
 * - clear
 *   한번 set되면 이후 clear안된다.
 * - test
 *   refault시 page가 active에 성공하고 workingset set이였다면 lru cost를
 *   증가시킨다. (workingset_refault())
 */
PAGEFLAG(Workingset, workingset, PF_HEAD)
	TESTCLEARFLAG(Workingset, workingset, PF_HEAD)
__PAGEFLAG(Slab, slab, PF_NO_TAIL)
__PAGEFLAG(SlobFree, slob_free, PF_NO_TAIL)
PAGEFLAG(Checked, checked, PF_NO_COMPOUND)	   /* Used by some filesystems */

/* Xen */
PAGEFLAG(Pinned, pinned, PF_NO_COMPOUND)
	TESTSCFLAG(Pinned, pinned, PF_NO_COMPOUND)
PAGEFLAG(SavePinned, savepinned, PF_NO_COMPOUND);
PAGEFLAG(Foreign, foreign, PF_NO_COMPOUND);
PAGEFLAG(XenRemapped, xen_remapped, PF_NO_COMPOUND)
	TESTCLEARFLAG(XenRemapped, xen_remapped, PF_NO_COMPOUND)

/* IAMROOT, 2024.09.19:
 * - PageReserved와 관련된 함수를 생성하는 매크로.
 *
 *   1) PageReserved(..)
 *      PG_reserved bit가 set되어 있는지 확인한다.
 *   2) SetPageReserved(..)
 *      PG_reserved bit를 set한다.
 *   3) ClearPageReserved(..)
 *      PG_reserved bit를 clear한다.
 *
 *   4) __ClearPageReserved(..)
 *      non-atomic으로 PG_reserved bit를 clear한다.
 *   5) __SetPageReserved(..)
 *      non-atomic으로 PG_reserved bit를 set한다.
 */
PAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
	__CLEARPAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)
	__SETPAGEFLAG(Reserved, reserved, PF_NO_COMPOUND)

/*
 * IAMROOT, 2022.06.04:
 * - anon page들은 일반적으로 만들어질때 SwapBacked가 set된다.
 *   (page_add_new_anon_rmap() 참고)
 *   clean anon일 경우 flag가 제거된다.
 */
PAGEFLAG(SwapBacked, swapbacked, PF_NO_TAIL)
	__CLEARPAGEFLAG(SwapBacked, swapbacked, PF_NO_TAIL)
	__SETPAGEFLAG(SwapBacked, swapbacked, PF_NO_TAIL)

/*
 * Private page markings that may be used by the filesystem that owns the page
 * for its own purposes.
 * - PG_private and PG_private_2 cause releasepage() and co to be invoked
 */
PAGEFLAG(Private, private, PF_ANY)
PAGEFLAG(Private2, private_2, PF_ANY) TESTSCFLAG(Private2, private_2, PF_ANY)
PAGEFLAG(OwnerPriv1, owner_priv_1, PF_ANY)
	TESTCLEARFLAG(OwnerPriv1, owner_priv_1, PF_ANY)

/*
 * Only test-and-set exist for PG_writeback.  The unconditional operators are
 * risky: they bypass page accounting.
 */
TESTPAGEFLAG(Writeback, writeback, PF_NO_TAIL)
	TESTSCFLAG(Writeback, writeback, PF_NO_TAIL)
PAGEFLAG(MappedToDisk, mappedtodisk, PF_NO_TAIL)

/* PG_readahead is only used for reads; PG_reclaim is only for writes */
PAGEFLAG(Reclaim, reclaim, PF_NO_TAIL)
	TESTCLEARFLAG(Reclaim, reclaim, PF_NO_TAIL)
PAGEFLAG(Readahead, reclaim, PF_NO_COMPOUND)
	TESTCLEARFLAG(Readahead, reclaim, PF_NO_COMPOUND)

#ifdef CONFIG_HIGHMEM
/*
 * Must use a macro here due to header dependency issues. page_zone() is not
 * available at this point.
 */
#define PageHighMem(__p) is_highmem_idx(page_zonenum(__p))
#else
PAGEFLAG_FALSE(HighMem)
#endif

#ifdef CONFIG_SWAP
static __always_inline int PageSwapCache(struct page *page)
{
#ifdef CONFIG_THP_SWAP
	page = compound_head(page);
#endif
	return PageSwapBacked(page) && test_bit(PG_swapcache, &page->flags);

}

/*
 * IAMROOT, 2022.04.23:
 * - swap 접근은 항상 swap cache를 통해서 하는데, 해당 page가 swap cache에 있을때
 *   사용.
 */
SETPAGEFLAG(SwapCache, swapcache, PF_NO_TAIL)
CLEARPAGEFLAG(SwapCache, swapcache, PF_NO_TAIL)
#else
PAGEFLAG_FALSE(SwapCache)
#endif

PAGEFLAG(Unevictable, unevictable, PF_HEAD)
	__CLEARPAGEFLAG(Unevictable, unevictable, PF_HEAD)
	TESTCLEARFLAG(Unevictable, unevictable, PF_HEAD)

/*
 * IAMROOT, 2022.04.02:
 * - memory를 특정목적으로 lock을 하여 swap, compaction, reclaim, migration,
 *   isolation등을 막아야 할때사용한다.
 * - pinned는 특정영역에 memory를 고정시켜놓아 node cpu변경등을 못하게 막는걸로,
 *   mlock과는 의미가 다르다.
 */
#ifdef CONFIG_MMU
PAGEFLAG(Mlocked, mlocked, PF_NO_TAIL)
	__CLEARPAGEFLAG(Mlocked, mlocked, PF_NO_TAIL)
	TESTSCFLAG(Mlocked, mlocked, PF_NO_TAIL)
#else
PAGEFLAG_FALSE(Mlocked) __CLEARPAGEFLAG_NOOP(Mlocked)
	TESTSCFLAG_FALSE(Mlocked)
#endif

#ifdef CONFIG_ARCH_USES_PG_UNCACHED
PAGEFLAG(Uncached, uncached, PF_NO_COMPOUND)
#else
PAGEFLAG_FALSE(Uncached)
#endif

#ifdef CONFIG_MEMORY_FAILURE
PAGEFLAG(HWPoison, hwpoison, PF_ANY)
TESTSCFLAG(HWPoison, hwpoison, PF_ANY)
#define __PG_HWPOISON (1UL << PG_hwpoison)
extern bool take_page_off_buddy(struct page *page);
#else
PAGEFLAG_FALSE(HWPoison)
#define __PG_HWPOISON 0
#endif

#if defined(CONFIG_PAGE_IDLE_FLAG) && defined(CONFIG_64BIT)
TESTPAGEFLAG(Young, young, PF_ANY)
SETPAGEFLAG(Young, young, PF_ANY)
TESTCLEARFLAG(Young, young, PF_ANY)
PAGEFLAG(Idle, idle, PF_ANY)
#endif

#ifdef CONFIG_KASAN_HW_TAGS
PAGEFLAG(SkipKASanPoison, skip_kasan_poison, PF_HEAD)
#else
PAGEFLAG_FALSE(SkipKASanPoison)
#endif

/*
 * PageReported() is used to track reported free pages within the Buddy
 * allocator. We can use the non-atomic version of the test and set
 * operations as both should be shielded with the zone lock to prevent
 * any possible races on the setting or clearing of the bit.
 */
__PAGEFLAG(Reported, reported, PF_NO_COMPOUND)

/*
 * On an anonymous page mapped into a user virtual memory area,
 * page->mapping points to its anon_vma, not to a struct address_space;
 * with the PAGE_MAPPING_ANON bit set to distinguish it.  See rmap.h.
 *
 * On an anonymous page in a VM_MERGEABLE area, if CONFIG_KSM is enabled,
 * the PAGE_MAPPING_MOVABLE bit may be set along with the PAGE_MAPPING_ANON
 * bit; and then page->mapping points, not to an anon_vma, but to a private
 * structure which KSM associates with that merged page.  See ksm.h.
 *
 * PAGE_MAPPING_KSM without PAGE_MAPPING_ANON is used for non-lru movable
 * page and then page->mapping points a struct address_space.
 *
 * Please note that, confusingly, "page_mapping" refers to the inode
 * address_space which maps the page from disk; whereas "page_mapped"
 * refers to user virtual address space into which the page is mapped.
 */
#define PAGE_MAPPING_ANON	0x1
#define PAGE_MAPPING_MOVABLE	0x2
#define PAGE_MAPPING_KSM	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)
#define PAGE_MAPPING_FLAGS	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)

/*
 * IAMROOT, 2022.04.09:
 * - anon or movable이 mapping되있는지 검사한다.
 */
static __always_inline int PageMappingFlags(struct page *page)
{
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) != 0;
}

static __always_inline int PageAnon(struct page *page)
{
	page = compound_head(page);
	return ((unsigned long)page->mapping & PAGE_MAPPING_ANON) != 0;
}

/*
 * IAMROOT, 2022.03.30:
 * - non-lru movable page인지를 peek 한다.
 * - page가 isolate된 상태에서 release되버리면 page->mapping은 안정된값을
 *   가지지 않는다.(__ClearPageMovable 참고)
 * - 하지만 lru page에서는 PAGE_MAPPING_MOVABLE을 사용할수 없기 때문에
 *   isolate가 된 page라면 non-lru movable or lru movable page인지를
 *   파악하는데 PAGE_MAPPING_MOVABLE만을 검사하면 되므로 적은 비용이 든다.
 * - pfn scanning을 하면서 비용이 많이드는 lock_page보다 이 함수를 사용하여
 *   peek 하기에 좋다.
 * - lock_page를 사용하는 non-lru page 함수는 PageMovable.
 */
static __always_inline int __PageMovable(struct page *page)
{
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) ==
				PAGE_MAPPING_MOVABLE;
}

#ifdef CONFIG_KSM
/*
 * A KSM page is one of those write-protected "shared pages" or "merged pages"
 * which KSM maps into multiple mms, wherever identical anonymous page content
 * is found in VM_MERGEABLE vmas.  It's a PageAnon page, pointing not to any
 * anon_vma, but to that page's node of the stable tree.
 */
/*
 * IAMROOT, 2022.04.09:
 * - KSM(Kernel same page merge)
 *   커널에 동일한 page가 많을때 여러개 가지고 있을 필요가 없는데, 거기에 대한
 *   처리 관련.
 */
static __always_inline int PageKsm(struct page *page)
{
	page = compound_head(page);
	return ((unsigned long)page->mapping & PAGE_MAPPING_FLAGS) ==
				PAGE_MAPPING_KSM;
}
#else
TESTPAGEFLAG_FALSE(Ksm)
#endif

u64 stable_page_flags(struct page *page);

static inline int PageUptodate(struct page *page)
{
	int ret;
	page = compound_head(page);
	ret = test_bit(PG_uptodate, &(page)->flags);
	/*
	 * Must ensure that the data we read out of the page is loaded
	 * _after_ we've loaded page->flags to check for PageUptodate.
	 * We can skip the barrier if the page is not uptodate, because
	 * we wouldn't be reading anything from it.
	 *
	 * See SetPageUptodate() for the other side of the story.
	 */
	if (ret)
		smp_rmb();

	return ret;
}

/*
 * IAMROOT, 2022.06.04:
 * - 최근에 page를 갱신했다는 flag.
 */
static __always_inline void __SetPageUptodate(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	smp_wmb();
	__set_bit(PG_uptodate, &page->flags);
}

static __always_inline void SetPageUptodate(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	/*
	 * Memory barrier must be issued before setting the PG_uptodate bit,
	 * so that all previous stores issued in order to bring the page
	 * uptodate are actually visible before PageUptodate becomes true.
	 */
	smp_wmb();
	set_bit(PG_uptodate, &page->flags);
}

CLEARPAGEFLAG(Uptodate, uptodate, PF_NO_TAIL)

int test_clear_page_writeback(struct page *page);
int __test_set_page_writeback(struct page *page, bool keep_write);

#define test_set_page_writeback(page)			\
	__test_set_page_writeback(page, false)
#define test_set_page_writeback_keepwrite(page)	\
	__test_set_page_writeback(page, true)

static inline void set_page_writeback(struct page *page)
{
	test_set_page_writeback(page);
}

static inline void set_page_writeback_keepwrite(struct page *page)
{
	test_set_page_writeback_keepwrite(page);
}
/*
 * IAMROOT, 2022.03.11:
 * ex) __SetPageHead => page->flags |= PG_head
 *   __SetPageHead(struct page *page)
 *   {
 *		__set_bit(PG_head, &PF_ANY(page, 1)->flags);
 *   }
 */
__PAGEFLAG(Head, head, PF_ANY) CLEARPAGEFLAG(Head, head, PF_ANY)

/*
 * IAMROOT, 2022.03.05:
 * - @page의 compound head가 @head를 가리키도록 한다. flag개념으로 +1을 한다.
 */
static __always_inline void set_compound_head(struct page *page, struct page *head)
{
	WRITE_ONCE(page->compound_head, (unsigned long)head + 1);
}

static __always_inline void clear_compound_head(struct page *page)
{
	WRITE_ONCE(page->compound_head, 0);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline void ClearPageCompound(struct page *page)
{
	BUG_ON(!PageHead(page));
	ClearPageHead(page);
}
#endif

#define PG_head_mask ((1UL << PG_head))

#ifdef CONFIG_HUGETLB_PAGE
int PageHuge(struct page *page);
int PageHeadHuge(struct page *page);
#else
TESTPAGEFLAG_FALSE(Huge)
TESTPAGEFLAG_FALSE(HeadHuge)
#endif


#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * PageHuge() only returns true for hugetlbfs pages, but not for
 * normal or transparent huge pages.
 *
 * PageTransHuge() returns true for both transparent huge and
 * hugetlbfs pages, but not normal pages. PageTransHuge() can only be
 * called only in the core VM paths where hugetlbfs pages can't exist.
 */
/*
 * IAMROOT, 2022.04.02:
 * - THP or hugetlbfs 인지 확인한다 (PageHuge는 hugetlb인지만을 확인)
 * - THP page인지 확인하는 방법은 아래처럼 사용한다.
 *  is_thp = PageTransHuge(page) && !PageHuge(page);
 */
static inline int PageTransHuge(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	return PageHead(page);
}

/*
 * PageTransCompound returns true for both transparent huge pages
 * and hugetlbfs pages, so it should only be called when it's known
 * that hugetlbfs pages aren't involved.
 */
static inline int PageTransCompound(struct page *page)
{
	return PageCompound(page);
}

/*
 * PageTransTail returns true for both transparent huge pages
 * and hugetlbfs pages, so it should only be called when it's known
 * that hugetlbfs pages aren't involved.
 */
static inline int PageTransTail(struct page *page)
{
	return PageTail(page);
}

/*
 * PageDoubleMap indicates that the compound page is mapped with PTEs as well
 * as PMDs.
 *
 * This is required for optimization of rmap operations for THP: we can postpone
 * per small page mapcount accounting (and its overhead from atomic operations)
 * until the first PMD split.
 *
 * For the page PageDoubleMap means ->_mapcount in all sub-pages is offset up
 * by one. This reference will go away with last compound_mapcount.
 *
 * See also __split_huge_pmd_locked() and page_remove_anon_compound_rmap().
 */
PAGEFLAG(DoubleMap, double_map, PF_SECOND)
	TESTSCFLAG(DoubleMap, double_map, PF_SECOND)
#else
TESTPAGEFLAG_FALSE(TransHuge)
TESTPAGEFLAG_FALSE(TransCompound)
TESTPAGEFLAG_FALSE(TransCompoundMap)
TESTPAGEFLAG_FALSE(TransTail)
PAGEFLAG_FALSE(DoubleMap)
	TESTSCFLAG_FALSE(DoubleMap)
#endif

#if defined(CONFIG_MEMORY_FAILURE) && defined(CONFIG_TRANSPARENT_HUGEPAGE)
/*
 * PageHasHWPoisoned indicates that at least one subpage is hwpoisoned in the
 * compound page.
 *
 * This flag is set by hwpoison handler.  Cleared by THP split or free page.
 */
PAGEFLAG(HasHWPoisoned, has_hwpoisoned, PF_SECOND)
	TESTSCFLAG(HasHWPoisoned, has_hwpoisoned, PF_SECOND)
#else
PAGEFLAG_FALSE(HasHWPoisoned)
	TESTSCFLAG_FALSE(HasHWPoisoned)
#endif

/*
 * Check if a page is currently marked HWPoisoned. Note that this check is
 * best effort only and inherently racy: there is no way to synchronize with
 * failing hardware.
 */
static inline bool is_page_hwpoison(struct page *page)
{
	if (PageHWPoison(page))
		return true;
	return PageHuge(page) && PageHWPoison(compound_head(page));
}

/*
 * For pages that are never mapped to userspace (and aren't PageSlab),
 * page_type may be used.  Because it is initialised to -1, we invert the
 * sense of the bit, so __SetPageFoo *clears* the bit used for PageFoo, and
 * __ClearPageFoo *sets* the bit used for PageFoo.  We reserve a few high and
 * low bits so that an underflow or overflow of page_mapcount() won't be
 * mistaken for a page type value.
 */

/*
 * IAMROOT, 2022.03.12:
 * - struct page의 page_type은 _mapcount 와 위치를 공유하는데 _mapcount는
 *   -1초기값을 가진다. 이것을 구별하기위 PAGE_TYPE_BASE가 사용되고
 *   PG_buddy의 값은 set이 bit clear, unset이 bit set이 되는 개념으로
 *   평범한 bit set개념의 반대개념으로 사용한다.
 */
#define PAGE_TYPE_BASE	0xf0000000
/* Reserve		0x0000007f to catch underflows of page_mapcount */
#define PAGE_MAPCOUNT_RESERVE	-128
#define PG_buddy	0x00000080
#define PG_offline	0x00000100
#define PG_table	0x00000200
#define PG_guard	0x00000400

/*
 * IAMROOT, 2022.03.12:
 * - page가 flag bit상태를 알아온다. page_type은 set과 reset이 반대이므로
 *   @return true이면 flag가 존재한다는것.
 */
#define PageType(page, flag)						\
	((page->page_type & (PAGE_TYPE_BASE | flag)) == PAGE_TYPE_BASE)

static inline int page_has_type(struct page *page)
{
	return (int)page->page_type < PAGE_MAPCOUNT_RESERVE;
}

/*
 * IAMROOT, 2022.03.12:
 * - page_type은 set과 reset이 반대이므로 아래 함수와같이 set일때는
 *   bit clear, clear일때는 bit set을 한다.
 */
#define PAGE_TYPE_OPS(uname, lname)					\
static __always_inline int Page##uname(struct page *page)		\
{									\
	return PageType(page, PG_##lname);				\
}									\
static __always_inline void __SetPage##uname(struct page *page)		\
{									\
	VM_BUG_ON_PAGE(!PageType(page, 0), page);			\
	page->page_type &= ~PG_##lname;					\
}									\
static __always_inline void __ClearPage##uname(struct page *page)	\
{									\
	VM_BUG_ON_PAGE(!Page##uname(page), page);			\
	page->page_type |= PG_##lname;					\
}

/*
 * PageBuddy() indicates that the page is free and in the buddy system
 * (see mm/page_alloc.c).
 */
/*
 * IAMROOT, 2022.03.12:
 * - ex) PageBuddy, __SetPageBuddy, __ClearPageBuddy
 */
PAGE_TYPE_OPS(Buddy, buddy)

/*
 * PageOffline() indicates that the page is logically offline although the
 * containing section is online. (e.g. inflated in a balloon driver or
 * not onlined when onlining the section).
 * The content of these pages is effectively stale. Such pages should not
 * be touched (read/write/dump/save) except by their owner.
 *
 * If a driver wants to allow to offline unmovable PageOffline() pages without
 * putting them back to the buddy, it can do so via the memory notifier by
 * decrementing the reference count in MEM_GOING_OFFLINE and incrementing the
 * reference count in MEM_CANCEL_OFFLINE. When offlining, the PageOffline()
 * pages (now with a reference count of zero) are treated like free pages,
 * allowing the containing memory block to get offlined. A driver that
 * relies on this feature is aware that re-onlining the memory block will
 * require to re-set the pages PageOffline() and not giving them to the
 * buddy via online_page_callback_t.
 *
 * There are drivers that mark a page PageOffline() and expect there won't be
 * any further access to page content. PFN walkers that read content of random
 * pages should check PageOffline() and synchronize with such drivers using
 * page_offline_freeze()/page_offline_thaw().
 */
PAGE_TYPE_OPS(Offline, offline)

extern void page_offline_freeze(void);
extern void page_offline_thaw(void);
extern void page_offline_begin(void);
extern void page_offline_end(void);

/*
 * Marks pages in use as page tables.
 */
PAGE_TYPE_OPS(Table, table)

/*
 * Marks guardpages used with debug_pagealloc.
 */
PAGE_TYPE_OPS(Guard, guard)

extern bool is_free_buddy_page(struct page *page);
/*
 * IAMROOT, 2022.03.30:
 * - isolate할시 cpu끼리 같은 page를 중복으로 isolate 안시키기 위한 장치.
 *   (상당 PG_isolated 주석 참고)
 */
__PAGEFLAG(Isolated, isolated, PF_ANY);

/*
 * If network-based swap is enabled, sl*b must keep track of whether pages
 * were allocated from pfmemalloc reserves.
 */
static inline int PageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	return PageActive(page);
}

/*
 * A version of PageSlabPfmemalloc() for opportunistic checks where the page
 * might have been freed under us and not be a PageSlab anymore.
 */
static inline int __PageSlabPfmemalloc(struct page *page)
{
	return PageActive(page);
}

static inline void SetPageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	SetPageActive(page);
}

static inline void __ClearPageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	__ClearPageActive(page);
}

static inline void ClearPageSlabPfmemalloc(struct page *page)
{
	VM_BUG_ON_PAGE(!PageSlab(page), page);
	ClearPageActive(page);
}

#ifdef CONFIG_MMU
#define __PG_MLOCKED		(1UL << PG_mlocked)
#else
#define __PG_MLOCKED		0
#endif

/*
 * Flags checked when a page is freed.  Pages being freed should not have
 * these flags set.  If they are, there is a problem.
 */
#define PAGE_FLAGS_CHECK_AT_FREE				\
	(1UL << PG_lru		| 1UL << PG_locked	|	\
	 1UL << PG_private	| 1UL << PG_private_2	|	\
	 1UL << PG_writeback	| 1UL << PG_reserved	|	\
	 1UL << PG_slab		| 1UL << PG_active 	|	\
	 1UL << PG_unevictable	| __PG_MLOCKED)

/*
 * Flags checked when a page is prepped for return by the page allocator.
 * Pages being prepped should not have these flags set.  If they are set,
 * there has been a kernel bug or struct page corruption.
 *
 * __PG_HWPOISON is exceptional because it needs to be kept beyond page's
 * alloc-free cycle to prevent from reusing the page.
 */
#define PAGE_FLAGS_CHECK_AT_PREP	\
	(PAGEFLAGS_MASK & ~__PG_HWPOISON)

#define PAGE_FLAGS_PRIVATE				\
	(1UL << PG_private | 1UL << PG_private_2)
/**
 * page_has_private - Determine if page has private stuff
 * @page: The page to be checked
 *
 * Determine if a page has private stuff, indicating that release routines
 * should be invoked upon it.
 */
static inline int page_has_private(struct page *page)
{
	return !!(page->flags & PAGE_FLAGS_PRIVATE);
}

#undef PF_ANY
#undef PF_HEAD
#undef PF_ONLY_HEAD
#undef PF_NO_TAIL
#undef PF_NO_COMPOUND
#undef PF_SECOND
#endif /* !__GENERATING_BOUNDS_H */

#endif	/* PAGE_FLAGS_H */
