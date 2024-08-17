/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BITMAP_H
#define __LINUX_BITMAP_H

#ifndef __ASSEMBLY__

#include <linux/align.h>
#include <linux/bitops.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/types.h>

struct device;

/* IAMROOT, 2021.09.30:
 * - bitmap은 기본적으로 unsigned long 타입을 써야된다.
 * - static inline code이기 때문에 가능하면 상수를 써야된다.
 * - DECLARE_BITMAP 매크로를 사용해서 bitmap을 선언해도된다.
 * - bitmap 함수는 굳이 code를 볼필요없이 주석 overview만 봐도 동작을 이해할수있다.
 */
/*
 * bitmaps provide bit arrays that consume one or more unsigned
 * longs.  The bitmap interface and available operations are listed
 * here, in bitmap.h
 *
 * Function implementations generic to all architectures are in
 * lib/bitmap.c.  Functions implementations that are architecture
 * specific are in various include/asm-<arch>/bitops.h headers
 * and other arch/<arch> specific files.
 *
 * See lib/bitmap.c for more details.
 */

/**
 * DOC: bitmap overview
 *
 * The available bitmap operations and their rough meaning in the
 * case that the bitmap is a single unsigned long are thus:
 *
 * The generated code is more efficient when nbits is known at
 * compile-time and at most BITS_PER_LONG.
 *
 * ::
 *
 *  bitmap_zero(dst, nbits)                     *dst = 0UL
 *  bitmap_fill(dst, nbits)                     *dst = ~0UL
 *  bitmap_copy(dst, src, nbits)                *dst = *src
 *  bitmap_and(dst, src1, src2, nbits)          *dst = *src1 & *src2
 *  bitmap_or(dst, src1, src2, nbits)           *dst = *src1 | *src2
 *  bitmap_xor(dst, src1, src2, nbits)          *dst = *src1 ^ *src2
 *  bitmap_andnot(dst, src1, src2, nbits)       *dst = *src1 & ~(*src2)
 *  bitmap_complement(dst, src, nbits)          *dst = ~(*src)
 *  bitmap_equal(src1, src2, nbits)             Are *src1 and *src2 equal?
 *  bitmap_intersects(src1, src2, nbits)        Do *src1 and *src2 overlap?
 *  bitmap_subset(src1, src2, nbits)            Is *src1 a subset of *src2?
 *  bitmap_empty(src, nbits)                    Are all bits zero in *src?
 *  bitmap_full(src, nbits)                     Are all bits set in *src?
 *  bitmap_weight(src, nbits)                   Hamming Weight: number set bits
 *  bitmap_set(dst, pos, nbits)                 Set specified bit area
 *  bitmap_clear(dst, pos, nbits)               Clear specified bit area
 *  bitmap_find_next_zero_area(buf, len, pos, n, mask)  Find bit free area
 *  bitmap_find_next_zero_area_off(buf, len, pos, n, mask, mask_off)  as above
 *  bitmap_next_clear_region(map, &start, &end, nbits)  Find next clear region
 *  bitmap_next_set_region(map, &start, &end, nbits)  Find next set region
 *  bitmap_for_each_clear_region(map, rs, re, start, end)
 *  						Iterate over all clear regions
 *  bitmap_for_each_set_region(map, rs, re, start, end)
 *  						Iterate over all set regions
 *  bitmap_shift_right(dst, src, n, nbits)      *dst = *src >> n
 *  bitmap_shift_left(dst, src, n, nbits)       *dst = *src << n
 *  bitmap_cut(dst, src, first, n, nbits)       Cut n bits from first, copy rest
 *  bitmap_replace(dst, old, new, mask, nbits)  *dst = (*old & ~(*mask)) | (*new & *mask)
 *  bitmap_remap(dst, src, old, new, nbits)     *dst = map(old, new)(src)
 *  bitmap_bitremap(oldbit, old, new, nbits)    newbit = map(old, new)(oldbit)
 *  bitmap_onto(dst, orig, relmap, nbits)       *dst = orig relative to relmap
 *  bitmap_fold(dst, orig, sz, nbits)           dst bits = orig bits mod sz
 *  bitmap_parse(buf, buflen, dst, nbits)       Parse bitmap dst from kernel buf
 *  bitmap_parse_user(ubuf, ulen, dst, nbits)   Parse bitmap dst from user buf
 *  bitmap_parselist(buf, dst, nbits)           Parse bitmap dst from kernel buf
 *  bitmap_parselist_user(buf, dst, nbits)      Parse bitmap dst from user buf
 *  bitmap_find_free_region(bitmap, bits, order)  Find and allocate bit region
 *  bitmap_release_region(bitmap, pos, order)   Free specified bit region
 *  bitmap_allocate_region(bitmap, pos, order)  Allocate specified bit region
 *  bitmap_from_arr32(dst, buf, nbits)          Copy nbits from u32[] buf to dst
 *  bitmap_to_arr32(buf, src, nbits)            Copy nbits from buf to u32[] dst
 *  bitmap_get_value8(map, start)               Get 8bit value from map at start
 *  bitmap_set_value8(map, value, start)        Set 8bit value to map at start
 *
 * Note, bitmap_zero() and bitmap_fill() operate over the region of
 * unsigned longs, that is, bits behind bitmap till the unsigned long
 * boundary will be zeroed or filled as well. Consider to use
 * bitmap_clear() or bitmap_set() to make explicit zeroing or filling
 * respectively.
 */

/**
 * DOC: bitmap bitops
 *
 * Also the following operations in asm/bitops.h apply to bitmaps.::
 *
 *  set_bit(bit, addr)                  *addr |= bit
 *  clear_bit(bit, addr)                *addr &= ~bit
 *  change_bit(bit, addr)               *addr ^= bit
 *  test_bit(bit, addr)                 Is bit set in *addr?
 *  test_and_set_bit(bit, addr)         Set bit and return old value
 *  test_and_clear_bit(bit, addr)       Clear bit and return old value
 *  test_and_change_bit(bit, addr)      Change bit and return old value
 *  find_first_zero_bit(addr, nbits)    Position first zero bit in *addr
 *  find_first_bit(addr, nbits)         Position first set bit in *addr
 *  find_next_zero_bit(addr, nbits, bit)
 *                                      Position next zero bit in *addr >= bit
 *  find_next_bit(addr, nbits, bit)     Position next set bit in *addr >= bit
 *  find_next_and_bit(addr1, addr2, nbits, bit)
 *                                      Same as find_next_bit, but in
 *                                      (*addr1 & *addr2)
 *
 */

/**
 * DOC: declare bitmap
 * The DECLARE_BITMAP(name,bits) macro, in linux/types.h, can be used
 * to declare an array named 'name' of just enough unsigned longs to
 * contain all bit positions from 0 to 'bits' - 1.
 */

/*
 * Allocation and deallocation of bitmap.
 * Provided in lib/bitmap.c to avoid circular dependency.
 */
unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags);
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags);
void bitmap_free(const unsigned long *bitmap);

/* Managed variants of the above. */
unsigned long *devm_bitmap_alloc(struct device *dev,
				 unsigned int nbits, gfp_t flags);
unsigned long *devm_bitmap_zalloc(struct device *dev,
				  unsigned int nbits, gfp_t flags);

/*
 * lib/bitmap.c provides these functions:
 */

int __bitmap_equal(const unsigned long *bitmap1,
		   const unsigned long *bitmap2, unsigned int nbits);
bool __pure __bitmap_or_equal(const unsigned long *src1,
			      const unsigned long *src2,
			      const unsigned long *src3,
			      unsigned int nbits);
void __bitmap_complement(unsigned long *dst, const unsigned long *src,
			 unsigned int nbits);
void __bitmap_shift_right(unsigned long *dst, const unsigned long *src,
			  unsigned int shift, unsigned int nbits);
void __bitmap_shift_left(unsigned long *dst, const unsigned long *src,
			 unsigned int shift, unsigned int nbits);
void bitmap_cut(unsigned long *dst, const unsigned long *src,
		unsigned int first, unsigned int cut, unsigned int nbits);
int __bitmap_and(unsigned long *dst, const unsigned long *bitmap1,
		 const unsigned long *bitmap2, unsigned int nbits);
void __bitmap_or(unsigned long *dst, const unsigned long *bitmap1,
		 const unsigned long *bitmap2, unsigned int nbits);
void __bitmap_xor(unsigned long *dst, const unsigned long *bitmap1,
		  const unsigned long *bitmap2, unsigned int nbits);
int __bitmap_andnot(unsigned long *dst, const unsigned long *bitmap1,
		    const unsigned long *bitmap2, unsigned int nbits);
void __bitmap_replace(unsigned long *dst,
		      const unsigned long *old, const unsigned long *new,
		      const unsigned long *mask, unsigned int nbits);
int __bitmap_intersects(const unsigned long *bitmap1,
			const unsigned long *bitmap2, unsigned int nbits);
int __bitmap_subset(const unsigned long *bitmap1,
		    const unsigned long *bitmap2, unsigned int nbits);
int __bitmap_weight(const unsigned long *bitmap, unsigned int nbits);
void __bitmap_set(unsigned long *map, unsigned int start, int len);
void __bitmap_clear(unsigned long *map, unsigned int start, int len);

unsigned long bitmap_find_next_zero_area_off(unsigned long *map,
					     unsigned long size,
					     unsigned long start,
					     unsigned int nr,
					     unsigned long align_mask,
					     unsigned long align_offset);

/**
 * bitmap_find_next_zero_area - find a contiguous aligned zero area
 * @map: The address to base the search on
 * @size: The bitmap size in bits
 * @start: The bitnumber to start searching at
 * @nr: The number of zeroed bits we're looking for
 * @align_mask: Alignment mask for zero area
 *
 * The @align_mask should be one less than a power of 2; the effect is that
 * the bit offset of all zero areas this function finds is multiples of that
 * power of 2. A @align_mask of 0 means no alignment is required.
 */
static inline unsigned long
bitmap_find_next_zero_area(unsigned long *map,
			   unsigned long size,
			   unsigned long start,
			   unsigned int nr,
			   unsigned long align_mask)
{
	return bitmap_find_next_zero_area_off(map, size, start, nr,
					      align_mask, 0);
}

int bitmap_parse(const char *buf, unsigned int buflen,
			unsigned long *dst, int nbits);
int bitmap_parse_user(const char __user *ubuf, unsigned int ulen,
			unsigned long *dst, int nbits);
int bitmap_parselist(const char *buf, unsigned long *maskp,
			int nmaskbits);
int bitmap_parselist_user(const char __user *ubuf, unsigned int ulen,
			unsigned long *dst, int nbits);
void bitmap_remap(unsigned long *dst, const unsigned long *src,
		const unsigned long *old, const unsigned long *new, unsigned int nbits);
int bitmap_bitremap(int oldbit,
		const unsigned long *old, const unsigned long *new, int bits);
void bitmap_onto(unsigned long *dst, const unsigned long *orig,
		const unsigned long *relmap, unsigned int bits);
void bitmap_fold(unsigned long *dst, const unsigned long *orig,
		unsigned int sz, unsigned int nbits);
int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order);
void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order);
int bitmap_allocate_region(unsigned long *bitmap, unsigned int pos, int order);

#ifdef __BIG_ENDIAN
void bitmap_copy_le(unsigned long *dst, const unsigned long *src, unsigned int nbits);
#else
#define bitmap_copy_le bitmap_copy
#endif
unsigned int bitmap_ord_to_pos(const unsigned long *bitmap, unsigned int ord, unsigned int nbits);
int bitmap_print_to_pagebuf(bool list, char *buf,
				   const unsigned long *maskp, int nmaskbits);


extern int bitmap_print_bitmask_to_buf(char *buf, const unsigned long *maskp,
				      int nmaskbits, loff_t off, size_t count);

extern int bitmap_print_list_to_buf(char *buf, const unsigned long *maskp,
				      int nmaskbits, loff_t off, size_t count);

/* IAMROOT, 2021.09.30:
 * start bit보다 낮은 bit들은 전부 clear하기 위한것.
 * ex)
 * BITMAP_FIRST_WORD_MASK 0 ffffffffffffffff
 * BITMAP_FIRST_WORD_MASK 1 fffffffffffffffe
 * BITMAP_FIRST_WORD_MASK 2 fffffffffffffffc
 * ...
 * BITMAP_FIRST_WORD_MASK 63 8000000000000000
 * BITMAP_FIRST_WORD_MASK 64 ffffffffffffffff
 * BITMAP_FIRST_WORD_MASK 65 fffffffffffffffe
 *
 * IAMROOT, 2023.04.15:
 * - BITMAP_FIRST_WORD_MASK(start)
 * (~0UL << ((start) & (BITS_PER_LONG - 1)))
 * (0xffff_ffff_ffff_ffff << ((start) & (64 -1)))
 * (0xffff_ffff_ffff_ffff << ((start) & 0b111111))
 *                                        start(=3)
 *                                          v
 * 0b1111_1111_1111_1111_..._1111_1111_1111_1000
 * start 비트 및에 하위 비트가 0으로 설정됨. 상위 비트는 모두 1
 *
 * BITMAP_FIRST_WORD_MASK(1)
 * 0b1111_1111_1111_1111_..._1111_1111_1111_1110
 * BITMAP_FIRST_WORD_MASK(2)
 * 0b1111_1111_1111_1111_..._1111_1111_1111_1100
 *
 * - BITMAP_LAST_WORD_MASK(nbits)
 * (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))
 * (0xffff_ffff_ffff_ffff >> (-(nbits) & (64 -1)))
 * (0xffff_ffff_ffff_ffff >> (-(nbits) & 0b111111))
 *
 * 2의 보수 방식에 의한 음수 표현법
 * 해당 수의 2진 표현을 뒤집은 후 1을 더하는 방법을 사용한다.
 * ex. -1
 * 1           = 0000 0000 0000 0000 0000 0000 0000 0001
 * 1를 뒤집은 값 = 1111 1111 1111 1111 1111 1111 1111 1110
 * -1의 2의 보수 = 1111 1111 1111 1111 1111 1111 1111 1111
 *
 * nbits      -(nbits)    (-(nbits) & 0b111111)  (~0UL >> (-(nbits) & 0b1111)))
 * 1      ... 1111 1111   11 1111 (63)           (~0UL >> 63)
 * 2      ... 1111 1110   11 1110 (62)           (~0UL >> 62)
 * 3      ... 1111 1101   11 1101 (61)           (~0UL >> 61)
 * 4      ... 1111 1100   11 1100 (60)           (~0UL >> 60)
 * 결국 2의 보수 방식에 의해 nbits 위쪽 상위 비트가 0으로 설정됨. 하위 비트는 모두 1.
 * 바꾸어 말하면 nbits 갯수 만큼의 하위 bits만 1로 설정되고 상위 비트는 모두 0 이 된다.
 *
 *                                        nbits(=3)
 *                                          v
 * 0b0000_0000_0000_0000_..._0000_0000_0000_0111
 * nbits 갯수 만큼의 하위 비트만 1로 설정. 상위 비트는 모두 0
 *
 * BITMAP_LAST_WORD_MASK(0)
 * 0b0000_0000_0000_0000_..._0000_0000_0000_0000
 * BITMAP_LAST_WORD_MASK(1)
 * 0b0000_0000_0000_0000_..._0000_0000_0000_0001
 * BITMAP_LAST_WORD_MASK(2)
 * 0b0000_0000_0000_0000_..._0000_0000_0000_0011
 * ...
 * BITMAP_LAST_WORD_MASK(63)
 * 0b0111_1111_1111_1111_..._1111_1111_1111_1111
 * BITMAP_LAST_WORD_MASK(64)
 * 0b1111_1111_1111_1111_..._1111_1111_1111_1111
 * BITMAP_LAST_WORD_MASK(65)
 * 0b0000_0000_0000_0000_..._0000_0000_0000_0001
 */
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
	memset(dst, 0, len);
}

static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
	memset(dst, 0xff, len);
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src,
			unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
	memcpy(dst, src, len);
}

/*
 * Copy bitmap and clear tail bits in last word.
 */
static inline void bitmap_copy_clear_tail(unsigned long *dst,
		const unsigned long *src, unsigned int nbits)
{
	bitmap_copy(dst, src, nbits);
	if (nbits % BITS_PER_LONG)
		dst[nbits / BITS_PER_LONG] &= BITMAP_LAST_WORD_MASK(nbits);
}

/*
 * On 32-bit systems bitmaps are represented as u32 arrays internally, and
 * therefore conversion is not needed when copying data from/to arrays of u32.
 */
#if BITS_PER_LONG == 64
void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf,
							unsigned int nbits);
void bitmap_to_arr32(u32 *buf, const unsigned long *bitmap,
							unsigned int nbits);
#else
#define bitmap_from_arr32(bitmap, buf, nbits)			\
	bitmap_copy_clear_tail((unsigned long *) (bitmap),	\
			(const unsigned long *) (buf), (nbits))
#define bitmap_to_arr32(buf, bitmap, nbits)			\
	bitmap_copy_clear_tail((unsigned long *) (buf),		\
			(const unsigned long *) (bitmap), (nbits))
#endif

static inline int bitmap_and(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return (*dst = *src1 & *src2 & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	return __bitmap_and(dst, src1, src2, nbits);
}

static inline void bitmap_or(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = *src1 | *src2;
	else
		__bitmap_or(dst, src1, src2, nbits);
}

static inline void bitmap_xor(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = *src1 ^ *src2;
	else
		__bitmap_xor(dst, src1, src2, nbits);
}

static inline int bitmap_andnot(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return (*dst = *src1 & ~(*src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	return __bitmap_andnot(dst, src1, src2, nbits);
}

static inline void bitmap_complement(unsigned long *dst, const unsigned long *src,
			unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = ~(*src);
	else
		__bitmap_complement(dst, src, nbits);
}

#ifdef __LITTLE_ENDIAN
#define BITMAP_MEM_ALIGNMENT 8
#else
#define BITMAP_MEM_ALIGNMENT (8 * sizeof(unsigned long))
#endif
#define BITMAP_MEM_MASK (BITMAP_MEM_ALIGNMENT - 1)


/*
 * IAMROOT, 2022.12.30:
 * - return true : @src1 == @src2
 */
static inline int bitmap_equal(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return !((*src1 ^ *src2) & BITMAP_LAST_WORD_MASK(nbits));
	if (__builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
	    IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		return !memcmp(src1, src2, nbits / 8);
	return __bitmap_equal(src1, src2, nbits);
}

/**
 * bitmap_or_equal - Check whether the or of two bitmaps is equal to a third
 * @src1:	Pointer to bitmap 1
 * @src2:	Pointer to bitmap 2 will be or'ed with bitmap 1
 * @src3:	Pointer to bitmap 3. Compare to the result of *@src1 | *@src2
 * @nbits:	number of bits in each of these bitmaps
 *
 * Returns: True if (*@src1 | *@src2) == *@src3, false otherwise
 */
static inline bool bitmap_or_equal(const unsigned long *src1,
				   const unsigned long *src2,
				   const unsigned long *src3,
				   unsigned int nbits)
{
	if (!small_const_nbits(nbits))
		return __bitmap_or_equal(src1, src2, src3, nbits);

	return !(((*src1 | *src2) ^ *src3) & BITMAP_LAST_WORD_MASK(nbits));
}

static inline int bitmap_intersects(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ((*src1 & *src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	else
		return __bitmap_intersects(src1, src2, nbits);
}

/*
 * IAMROOT, 2022.05.07:
 * @return src1이 src2에 모두 포함이면 true. 아니면 false.
 * - @src1이 @src2에 모두 포함되어있는지 판별한다.
 */
static inline int bitmap_subset(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ! ((*src1 & ~(*src2)) & BITMAP_LAST_WORD_MASK(nbits));
	else
		return __bitmap_subset(src1, src2, nbits);
}

static inline bool bitmap_empty(const unsigned long *src, unsigned nbits)
{
	if (small_const_nbits(nbits))
		return ! (*src & BITMAP_LAST_WORD_MASK(nbits));

	return find_first_bit(src, nbits) == nbits;
}

static inline bool bitmap_full(const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ! (~(*src) & BITMAP_LAST_WORD_MASK(nbits));

	return find_first_zero_bit(src, nbits) == nbits;
}

/* IAMROOT, 2021.11.27:
 * - @src 비트맵에서 @nbits 이내에 설정된 비트의 개수 반환
 */
static __always_inline int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return hweight_long(*src & BITMAP_LAST_WORD_MASK(nbits));
	return __bitmap_weight(src, nbits);
}

/* IAMROOT, 2023.04.08:
 * - @start부터 @nbits 수만큼의 bit를 @map에 설정
 */
static __always_inline void bitmap_set(unsigned long *map, unsigned int start,
		unsigned int nbits)
{
	if (__builtin_constant_p(nbits) && nbits == 1)
		__set_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
		 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0xff, nbits / 8);
	else
		__bitmap_set(map, start, nbits);
}

/*
 * IAMROOT, 2023.04.08:
 * - @map 에서 @start부터 @nbits 수만큼 clear
 */
static __always_inline void bitmap_clear(unsigned long *map, unsigned int start,
		unsigned int nbits)
{
	if (__builtin_constant_p(nbits) && nbits == 1)
		__clear_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
		 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0, nbits / 8);
	else
		__bitmap_clear(map, start, nbits);
}

static inline void bitmap_shift_right(unsigned long *dst, const unsigned long *src,
				unsigned int shift, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = (*src & BITMAP_LAST_WORD_MASK(nbits)) >> shift;
	else
		__bitmap_shift_right(dst, src, shift, nbits);
}

static inline void bitmap_shift_left(unsigned long *dst, const unsigned long *src,
				unsigned int shift, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = (*src << shift) & BITMAP_LAST_WORD_MASK(nbits);
	else
		__bitmap_shift_left(dst, src, shift, nbits);
}

static inline void bitmap_replace(unsigned long *dst,
				  const unsigned long *old,
				  const unsigned long *new,
				  const unsigned long *mask,
				  unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = (*old & ~(*mask)) | (*new & *mask);
	else
		__bitmap_replace(dst, old, new, mask, nbits);
}

/*
 * IAMROOT, 2022.01.15:
 * - unset bit를 찾아서 rs, set bit를 찾아서 re로 return한다.
 */
static inline void bitmap_next_clear_region(unsigned long *bitmap,
					    unsigned int *rs, unsigned int *re,
					    unsigned int end)
{
	*rs = find_next_zero_bit(bitmap, end, *rs);
	*re = find_next_bit(bitmap, end, *rs + 1);
}

static inline void bitmap_next_set_region(unsigned long *bitmap,
					  unsigned int *rs, unsigned int *re,
					  unsigned int end)
{
	*rs = find_next_bit(bitmap, end, *rs);
	*re = find_next_zero_bit(bitmap, end, *rs + 1);
}

/*
 * Bitmap region iterators.  Iterates over the bitmap between [@start, @end).
 * @rs and @re should be integer variables and will be set to start and end
 * index of the current clear or set region.
 */
/*
 * IAMROOT, 2022.01.17:
 * bits는 PCPU_BITMAP_BLOCK_BITS개일 것이다 (1024개)
 * 
 * ex) 11111000011111000001111100000
 *          ^   ^    ^    ^    ^    ^
 *          <--->    <---->    <----->
 *           0회차    1회차     2회차
 *          이 영역들을 찾아서 각 iter마다 rs, re로 나올것이다.
 */
#define bitmap_for_each_clear_region(bitmap, rs, re, start, end)	     \
	for ((rs) = (start),						     \
	     bitmap_next_clear_region((bitmap), &(rs), &(re), (end));	     \
	     (rs) < (re);						     \
	     (rs) = (re) + 1,						     \
	     bitmap_next_clear_region((bitmap), &(rs), &(re), (end)))

#define bitmap_for_each_set_region(bitmap, rs, re, start, end)		     \
	for ((rs) = (start),						     \
	     bitmap_next_set_region((bitmap), &(rs), &(re), (end));	     \
	     (rs) < (re);						     \
	     (rs) = (re) + 1,						     \
	     bitmap_next_set_region((bitmap), &(rs), &(re), (end)))

/**
 * BITMAP_FROM_U64() - Represent u64 value in the format suitable for bitmap.
 * @n: u64 value
 *
 * Linux bitmaps are internally arrays of unsigned longs, i.e. 32-bit
 * integers in 32-bit environment, and 64-bit integers in 64-bit one.
 *
 * There are four combinations of endianness and length of the word in linux
 * ABIs: LE64, BE64, LE32 and BE32.
 *
 * On 64-bit kernels 64-bit LE and BE numbers are naturally ordered in
 * bitmaps and therefore don't require any special handling.
 *
 * On 32-bit kernels 32-bit LE ABI orders lo word of 64-bit number in memory
 * prior to hi, and 32-bit BE orders hi word prior to lo. The bitmap on the
 * other hand is represented as an array of 32-bit words and the position of
 * bit N may therefore be calculated as: word #(N/32) and bit #(N%32) in that
 * word.  For example, bit #42 is located at 10th position of 2nd word.
 * It matches 32-bit LE ABI, and we can simply let the compiler store 64-bit
 * values in memory as it usually does. But for BE we need to swap hi and lo
 * words manually.
 *
 * With all that, the macro BITMAP_FROM_U64() does explicit reordering of hi and
 * lo parts of u64.  For LE32 it does nothing, and for BE environment it swaps
 * hi and lo words, as is expected by bitmap.
 */
#if __BITS_PER_LONG == 64
#define BITMAP_FROM_U64(n) (n)
#else
#define BITMAP_FROM_U64(n) ((unsigned long) ((u64)(n) & ULONG_MAX)), \
				((unsigned long) ((u64)(n) >> 32))
#endif

/**
 * bitmap_from_u64 - Check and swap words within u64.
 *  @mask: source bitmap
 *  @dst:  destination bitmap
 *
 * In 32-bit Big Endian kernel, when using ``(u32 *)(&val)[*]``
 * to read u64 mask, we will get the wrong word.
 * That is ``(u32 *)(&val)[0]`` gets the upper 32 bits,
 * but we expect the lower 32-bits of u64.
 */
static inline void bitmap_from_u64(unsigned long *dst, u64 mask)
{
	dst[0] = mask & ULONG_MAX;

	if (sizeof(mask) > sizeof(unsigned long))
		dst[1] = mask >> 32;
}

/**
 * bitmap_get_value8 - get an 8-bit value within a memory region
 * @map: address to the bitmap memory region
 * @start: bit offset of the 8-bit value; must be a multiple of 8
 *
 * Returns the 8-bit value located at the @start bit offset within the @src
 * memory region.
 */
static inline unsigned long bitmap_get_value8(const unsigned long *map,
					      unsigned long start)
{
	const size_t index = BIT_WORD(start);
	const unsigned long offset = start % BITS_PER_LONG;

	return (map[index] >> offset) & 0xFF;
}

/**
 * bitmap_set_value8 - set an 8-bit value within a memory region
 * @map: address to the bitmap memory region
 * @value: the 8-bit value; values wider than 8 bits may clobber bitmap
 * @start: bit offset of the 8-bit value; must be a multiple of 8
 */
static inline void bitmap_set_value8(unsigned long *map, unsigned long value,
				     unsigned long start)
{
	const size_t index = BIT_WORD(start);
	const unsigned long offset = start % BITS_PER_LONG;

	map[index] &= ~(0xFFUL << offset);
	map[index] |= value << offset;
}

#endif /* __ASSEMBLY__ */

#endif /* __LINUX_BITMAP_H */
