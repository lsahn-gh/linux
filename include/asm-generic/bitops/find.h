/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_BITOPS_FIND_H_
#define _ASM_GENERIC_BITOPS_FIND_H_

extern unsigned long _find_next_bit(const unsigned long *addr1,
		const unsigned long *addr2, unsigned long nbits,
		unsigned long start, unsigned long invert, unsigned long le);
extern unsigned long _find_first_bit(const unsigned long *addr, unsigned long size);
extern unsigned long _find_first_zero_bit(const unsigned long *addr, unsigned long size);
extern unsigned long _find_last_bit(const unsigned long *addr, unsigned long size);

#ifndef find_next_bit
/**
 * find_next_bit - find the next set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The bitmap size in bits
 *
 * Returns the bit number for the next set bit
 * If no bits are set, returns @size.
 */
/* IAMROOT, 2021.09.30.
 * @addr	bitmap 주소
 * @size	bitmap 사이즈
 * @offset	요청한 비트를 포함하여 검색
 *
 * return	비트번호(based 0)를 반환, 몿잧은 경우 @size 반환
 *
 * ex) unsigned long addr[2] = {3, 3}; 
 *
 * bit0          bit63 bit64      bit127
 *    v              v v               v
 *    11000........000 11000........0000
 *    lowest -----(search)-----> highest
 *
 * find_next_bit(bitmap, 128, 0)    ->  0번 부터 검색 -> 0
 * find_next_bit(bitmap, 128, 1)    ->  1번 부터 검색 -> 1
 * find_next_bit(bitmap, 128, 2)    ->  2번 부터 검색 -> 64
 * find_next_bit(bitmap, 128, 65)   -> 65번 부터 검색 -> 65  
 * find_next_bit(bitmap, 128, 66)   -> 66번 부터 검색 -> 128 (not found)
 */
static inline
unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
			    unsigned long offset)
{
	if (small_const_nbits(size)) {
		unsigned long val;

		if (unlikely(offset >= size))
			return size;

		val = *addr & GENMASK(size - 1, offset);
		return val ? __ffs(val) : size;
	}

	return _find_next_bit(addr, NULL, size, offset, 0UL, 0);
}
#endif

#ifndef find_next_and_bit
/**
 * find_next_and_bit - find the next set bit in both memory regions
 * @addr1: The first address to base the search on
 * @addr2: The second address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The bitmap size in bits
 *
 * Returns the bit number for the next set bit
 * If no bits are set, returns @size.
 */
/* IAMROOT, 2021.09.30:
 * addr2로 and를 한후 찾겠다는 함수. find_next_bit에서 addr2만 추가되고
 * _find_next_bit에서 addr2을 and하여 구하는것이 보인다.
 */
static inline
unsigned long find_next_and_bit(const unsigned long *addr1,
		const unsigned long *addr2, unsigned long size,
		unsigned long offset)
{
	if (small_const_nbits(size)) {
		unsigned long val;

		if (unlikely(offset >= size))
			return size;

		val = *addr1 & *addr2 & GENMASK(size - 1, offset);
		return val ? __ffs(val) : size;
	}

	return _find_next_bit(addr1, addr2, size, offset, 0UL, 0);
}
#endif

#ifndef find_next_zero_bit
/**
 * find_next_zero_bit - find the next cleared bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The bitmap size in bits
 *
 * Returns the bit number of the next zero bit
 * If no bits are zero, returns @size.
 */
/* IAMROOT, 2021.09.30:
 * @addr:	비트맵
 * @size:	비트맵 사이즈
 * @offset:	검색 시작 비트 위치(자기자신부터 검색)
 *
 * _find_next_bit가 set bit만을 찾는구조로 되있으므로 ~0UL로 bit들을 invert해서
 * set bit를 찾는식이된다.
 *
 * ex) unsigned long addr[] = { 0xff00fff000 }; 
 *
 *                 +--(search)----------------> @size 만큼
 *                 |
 *     bit0        |                      bit39
 *     v           |                          v
 * 예) 0000000000001111111111110000000011111111 (0xff00fff000)
 *                 ^           ^
 *                 |           +-return
 *                 +-@offset 
 *
 * find_next_zero_bit(bitmap, 40,  0)   ->   0번 부터 검색 -> 0
 * find_next_zero_bit(bitmap, 40, 12)   ->  12번 부터 검색 -> 24 (위그림 상황)
 * find_next_zero_bit(bitmap, 40, 32)   ->  32번 부터 검색 -> 40 (not found)
 */
static inline
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size,
				 unsigned long offset)
{
	if (small_const_nbits(size)) {
		unsigned long val;

		if (unlikely(offset >= size))
			return size;

		val = *addr | ~GENMASK(size - 1, offset);
		return val == ~0UL ? size : ffz(val);
	}

	return _find_next_bit(addr, NULL, size, offset, ~0UL, 0);
}
#endif

#ifdef CONFIG_GENERIC_FIND_FIRST_BIT

/**
 * find_first_bit - find the first set bit in a memory region
 * @addr: The address to start the search at
 * @size: The maximum number of bits to search
 *
 * Returns the bit number of the first set bit.
 * If no bits are set, returns @size.
 */
/* IAMROOT, 2021.09.30:
 * addr bitmap에서 제일 처음 set되 있는 bit index를 구한다.
 */
static inline
unsigned long find_first_bit(const unsigned long *addr, unsigned long size)
{
	if (small_const_nbits(size)) {
		unsigned long val = *addr & GENMASK(size - 1, 0);

		return val ? __ffs(val) : size;
	}

	return _find_first_bit(addr, size);
}

/**
 * find_first_zero_bit - find the first cleared bit in a memory region
 * @addr: The address to start the search at
 * @size: The maximum number of bits to search
 *
 * Returns the bit number of the first cleared bit.
 * If no bits are zero, returns @size.
 */
static inline
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	if (small_const_nbits(size)) {
		unsigned long val = *addr | ~GENMASK(size - 1, 0);

		return val == ~0UL ? size : ffz(val);
	}

	return _find_first_zero_bit(addr, size);
}
#else /* CONFIG_GENERIC_FIND_FIRST_BIT */

#ifndef find_first_bit
#define find_first_bit(addr, size) find_next_bit((addr), (size), 0)
#endif
#ifndef find_first_zero_bit
#define find_first_zero_bit(addr, size) find_next_zero_bit((addr), (size), 0)
#endif

#endif /* CONFIG_GENERIC_FIND_FIRST_BIT */

#ifndef find_last_bit
/**
 * find_last_bit - find the last set bit in a memory region
 * @addr: The address to start the search at
 * @size: The number of bits to search
 *
 * Returns the bit number of the last set bit, or size.
 */
/* IAMROOT, 2021.09.30:
 * addr bitmap에서 제일 마지막에 set되 있는 bit index를 구한다.
 * set 된 bit가 없으면 size를 반환한다
 *
 * ex) size == 100 일때
 * 0x1 => return 0
 * 0x1f => return 4
 * 0x1f_ffff => return 20
 * 0x00 => return 100;
 */
static inline
unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	if (small_const_nbits(size)) {
		unsigned long val = *addr & GENMASK(size - 1, 0);

		return val ? __fls(val) : size;
	}

	return _find_last_bit(addr, size);
}
#endif

/**
 * find_next_clump8 - find next 8-bit clump with set bits in a memory region
 * @clump: location to store copy of found clump
 * @addr: address to base the search on
 * @size: bitmap size in number of bits
 * @offset: bit offset at which to start searching
 *
 * Returns the bit offset for the next set clump; the found clump value is
 * copied to the location pointed by @clump. If no bits are set, returns @size.
 */
extern unsigned long find_next_clump8(unsigned long *clump,
				      const unsigned long *addr,
				      unsigned long size, unsigned long offset);

#define find_first_clump8(clump, bits, size) \
	find_next_clump8((clump), (bits), (size), 0)

#endif /*_ASM_GENERIC_BITOPS_FIND_H_ */
