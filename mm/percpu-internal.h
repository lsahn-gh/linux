/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_PERCPU_INTERNAL_H
#define _MM_PERCPU_INTERNAL_H

#include <linux/types.h>
#include <linux/percpu.h>

/*
 * pcpu_block_md is the metadata block struct.
 * Each chunk's bitmap is split into a number of full blocks.
 * All units are in terms of bits.
 *
 * The scan hint is the largest known contiguous area before the contig hint.
 * It is not necessarily the actual largest contig hint though.  There is an
 * invariant that the scan_hint_start > contig_hint_start iff
 * scan_hint == contig_hint.  This is necessary because when scanning forward,
 * we don't know if a new contig hint would be better than the current one.
 */

/*
 * IAMROOT, 2022.01.18: 
 * pcpu_block_md는 메타 데이터 블록 구조체이다.
 * 각 청크의 비트 맵은 여러 개의 전체(full) 블록으로 분할된다.
 * 모든 단위는 비트 단위이다.
 *
 * scan_hint는 contig_hint 이전에 알려진 가장 큰 연속 영역이다.
 * 그러나 그것이 반드시 실제 가장 큰 contig 힌트는 아니다. 
 * 불변성: contig_hint_start < scan_hint_start 
 *         (iff contig_hint == scan_hint)
 * 이것은 앞으로(forward) 스캔할 때 새로운 contig 힌트가 현재 힌트보다 
 * 더 나은지 알 수 없기 때문에 필요하다.
 */
struct pcpu_block_md {
/*
 * IAMROOT, 2022.01.15:
 * - 
 */
	int			scan_hint;	/* scan hint for block */
	int			scan_hint_start; /* block relative starting
						    position of the scan hint */
	int                     contig_hint;    /* contig hint for block */
	int                     contig_hint_start; /* block relative starting
						      position of the contig hint */
	int                     left_free;      /* size of free space along
						   the left side of the block */
	int                     right_free;     /* size of free space along
						   the right side of the block */
	int                     first_free;     /* block position of first free */
	int			nr_bits;	/* total bits responsible for */
};

struct pcpu_chunk {
#ifdef CONFIG_PERCPU_STATS
	int			nr_alloc;	/* # of allocations */
	size_t			max_alloc_size; /* largest allocation size */
#endif

	struct list_head	list;		/* linked to pcpu_slot lists */
	int			free_bytes;	/* free bytes in the chunk */
/*
 * IAMROOT, 2022.01.15:
 * - md_blocks는 4096byte단위의 관리라면 chunk_md는 chunk의 전체에 대한
 *   alloc_map 관리를 담당한다.
 */
	struct pcpu_block_md	chunk_md;
	void			*base_addr;	/* base address of this chunk */

/*
 * IAMROOT, 2022.01.15:
 * - region 영역에 대한 4byte단위(PCPU_MIN_ALLOC_SIZE)의 관리 bitmap이다.
 * - bound_map으로 free, alloc area를 구별한다.
 * - page단위로 chunk->md_blocks로 관리된다. 1024(PCPU_BITMAP_BLOCK_BITS)단위
 *
 * 실제 4byte당 할당여부는 alloc_map을 통하고, alloc, free에 대한 영역은
 * 4096byte단위로 mb_blocks를 통해 관리하는 식이다.
 */
	unsigned long		*alloc_map;	/* allocation map */
/*
 * IAMROOT, 2022.01.15:
 * - 사용중인 영역과 free영역의 시작마다 1로 set된다. 끝을 구별
 *   해야되므로 1bit를 더 관리해야된다.
 * - alloc_map보다 1bit를 더 관리해야된다.
 * - start_offset이 있으면 0번 bit, start_offset 지점의 영역 bit에
 *   1이 set된다. 즉 처음부터 안쓰는범위를 제외한다.
 * - end_offset이 있으면 region last bit, start_offset + map_size
 *   지점의 영역 bit에 1이 set된다. 즉 처음부터 안쓰는범위를 제외한다.
 */
	unsigned long		*bound_map;	/* boundary map */
/*
 * IAMROOT, 2022.01.15:
 * - region 영역에 대해서 PCPU_BITMAP_BLOCK_SIZE 단위로 관리한다.
 */
	struct pcpu_block_md	*md_blocks;	/* metadata blocks */

	void			*data;		/* chunk data */
	bool			immutable;	/* no [de]population allowed */
	bool			isolated;	/* isolated from active chunk
						   slots */
/*
 * IAMROOT, 2022.01.15:
 * - region이 PAGE_MASK로 masking하여 align했을때 align 된 offset.
 */
	int			start_offset;	/* the overlap with the previous
						   region to have a page aligned
						   base_addr */
/*
 * IAMROOT, 2022.01.15:
 * - align을 한후에 region 맨끝에 사용하지 않은 공간에 대한
 *   offset.
 */
	int			end_offset;	/* additional area required to
						   have the region end page
						   aligned */
#ifdef CONFIG_MEMCG_KMEM
	struct obj_cgroup	**obj_cgroups;	/* vector of object cgroups */
#endif
/*
 * IAMROOT, 2022.01.18:
 * - start_offset, end_offset이 포함된 전체 영역(region)에 대한 page 개수
 */
	int			nr_pages;	/* # of pages served by this chunk */
/*
 * IAMROOT, 2022.01.15:
 * - 초기값은 nr_pages와 같다.
 */
	int			nr_populated;	/* # of populated pages */
	int                     nr_empty_pop_pages; /* # of empty populated pages */
/*
 * IAMROOT, 2022.01.15:
 * - 실제 사용하는 mapping이 된 page를 표현한 bitmap.
 *   총 bits는 region size로 계산된다.
 *   1bit당 PAGE_SIZE.
 * - 초기값으로 모든 bit가 set된다.
 */
	unsigned long		populated[];	/* populated bitmap */
};

extern spinlock_t pcpu_lock;

extern struct list_head *pcpu_chunk_lists;
extern int pcpu_nr_slots;
extern int pcpu_sidelined_slot;
extern int pcpu_to_depopulate_slot;
extern int pcpu_nr_empty_pop_pages;

extern struct pcpu_chunk *pcpu_first_chunk;
extern struct pcpu_chunk *pcpu_reserved_chunk;

/**
 * pcpu_chunk_nr_blocks - converts nr_pages to # of md_blocks
 * @chunk: chunk of interest
 *
 * This conversion is from the number of physical pages that the chunk
 * serves to the number of bitmap blocks used.
 */
/*
 * IAMROOT, 2022.01.15:
 * - chunk의 크기를 block 단위로 나눈 개수
 */
static inline int pcpu_chunk_nr_blocks(struct pcpu_chunk *chunk)
{
	return chunk->nr_pages * PAGE_SIZE / PCPU_BITMAP_BLOCK_SIZE;
}

/**
 * pcpu_nr_pages_to_map_bits - converts the pages to size of bitmap
 * @pages: number of physical pages
 *
 * This conversion is from physical pages to the number of bits
 * required in the bitmap.
 */
/*
 * IAMROOT, 2022.01.15:
 * @return pages가 속한 alloc_map의 bit index
 */
static inline int pcpu_nr_pages_to_map_bits(int pages)
{
	return pages * PAGE_SIZE / PCPU_MIN_ALLOC_SIZE;
}

/**
 * pcpu_chunk_map_bits - helper to convert nr_pages to size of bitmap
 * @chunk: chunk of interest
 *
 * This conversion is from the number of physical pages that the chunk
 * serves to the number of bits in the bitmap.
 */
/*
 * IAMROOT, 2022.01.18:
 * @return pcpu_chunk.alloc_map 의 bits
 */
static inline int pcpu_chunk_map_bits(struct pcpu_chunk *chunk)
{
	return pcpu_nr_pages_to_map_bits(chunk->nr_pages);
}

#ifdef CONFIG_PERCPU_STATS

#include <linux/spinlock.h>

struct percpu_stats {
	u64 nr_alloc;		/* lifetime # of allocations */
	u64 nr_dealloc;		/* lifetime # of deallocations */
	u64 nr_cur_alloc;	/* current # of allocations */
	u64 nr_max_alloc;	/* max # of live allocations */
	u32 nr_chunks;		/* current # of live chunks */
	u32 nr_max_chunks;	/* max # of live chunks */
	size_t min_alloc_size;	/* min allocation size */
	size_t max_alloc_size;	/* max allocation size */
};

extern struct percpu_stats pcpu_stats;
extern struct pcpu_alloc_info pcpu_stats_ai;

/*
 * For debug purposes. We don't care about the flexible array.
 */
/*
 * IAMROOT, 2022.01.15:
 * - pcpu debug
 */
static inline void pcpu_stats_save_ai(const struct pcpu_alloc_info *ai)
{
	memcpy(&pcpu_stats_ai, ai, sizeof(struct pcpu_alloc_info));

	/* initialize min_alloc_size to unit_size */
	pcpu_stats.min_alloc_size = pcpu_stats_ai.unit_size;
}

/*
 * pcpu_stats_area_alloc - increment area allocation stats
 * @chunk: the location of the area being allocated
 * @size: size of area to allocate in bytes
 *
 * CONTEXT:
 * pcpu_lock.
 */
static inline void pcpu_stats_area_alloc(struct pcpu_chunk *chunk, size_t size)
{
	lockdep_assert_held(&pcpu_lock);

	pcpu_stats.nr_alloc++;
	pcpu_stats.nr_cur_alloc++;
	pcpu_stats.nr_max_alloc =
		max(pcpu_stats.nr_max_alloc, pcpu_stats.nr_cur_alloc);
	pcpu_stats.min_alloc_size =
		min(pcpu_stats.min_alloc_size, size);
	pcpu_stats.max_alloc_size =
		max(pcpu_stats.max_alloc_size, size);

	chunk->nr_alloc++;
	chunk->max_alloc_size = max(chunk->max_alloc_size, size);
}

/*
 * pcpu_stats_area_dealloc - decrement allocation stats
 * @chunk: the location of the area being deallocated
 *
 * CONTEXT:
 * pcpu_lock.
 */
static inline void pcpu_stats_area_dealloc(struct pcpu_chunk *chunk)
{
	lockdep_assert_held(&pcpu_lock);

	pcpu_stats.nr_dealloc++;
	pcpu_stats.nr_cur_alloc--;

	chunk->nr_alloc--;
}

/*
 * IAMROOT, 2022.01.22: 
 * 청크를 할당하고, 해제할 때 마다 아래 두 함수를 호출하며 관련 청크 수를 
 * 갱신한다.
 */

/*
 * pcpu_stats_chunk_alloc - increment chunk stats
 */
static inline void pcpu_stats_chunk_alloc(void)
{
	unsigned long flags;
	spin_lock_irqsave(&pcpu_lock, flags);

	pcpu_stats.nr_chunks++;
	pcpu_stats.nr_max_chunks =
		max(pcpu_stats.nr_max_chunks, pcpu_stats.nr_chunks);

	spin_unlock_irqrestore(&pcpu_lock, flags);
}

/*
 * pcpu_stats_chunk_dealloc - decrement chunk stats
 */
static inline void pcpu_stats_chunk_dealloc(void)
{
	unsigned long flags;
	spin_lock_irqsave(&pcpu_lock, flags);

	pcpu_stats.nr_chunks--;

	spin_unlock_irqrestore(&pcpu_lock, flags);
}

#else

static inline void pcpu_stats_save_ai(const struct pcpu_alloc_info *ai)
{
}

static inline void pcpu_stats_area_alloc(struct pcpu_chunk *chunk, size_t size)
{
}

static inline void pcpu_stats_area_dealloc(struct pcpu_chunk *chunk)
{
}

static inline void pcpu_stats_chunk_alloc(void)
{
}

static inline void pcpu_stats_chunk_dealloc(void)
{
}

#endif /* !CONFIG_PERCPU_STATS */

#endif
