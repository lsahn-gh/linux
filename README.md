# 최신 ARM64 리눅스 커널 5.10/5.15 분석

## 커뮤니티: IAMROOT 18차
- [www.iamroot.org][#iamroot] | IAMROOT 홈페이지
- [jake.dothome.co.kr][#moonc] | 문c 블로그

[#iamroot]: http://www.iamroot.org
[#moonc]: http://jake.dothome.co.kr

## History

- 첫 모임: 2021년 5월 22일 (50여명 zoom online 접속)

- [https://band.us/@iamroot18][#band] | iamroot18 밴드

[#iamroot]: http://www.iamroot.org
[#moonc]: http://jake.dothome.co.kr
[#band]: https://band.us/@iamroot18


### 0주차
2021.05.22, Zoom 온라인(5x명 접속)
- Orientation (3시간)

### 1주차
- 2021.05.29, Zoom 온라인(53명 참석).
- pm 03:00 ~ pm 10:00
- 리눅스 커널 내부구조 (p10 ~ p54 중간)

### 2주차
- 2021.06.05, Zoom 온라인(52명 참석)
- pm 03:00 ~ pm 10:00
- 리눅스 커널 내부구조 (p54 ~ p81)

### 3주차
- 2021.06.12, Zoom 온라인(44명 참석)
- pm 03:00 ~ pm 10:00
- 리눅스 커널 내부구조 (p81 ~ p100)

### 4주차
- 2021.06.19, Zoom 온라인(40명 참석)
- pm 03:00 ~ pm 10:00
- 리눅스 커널 내부구조 (p100 ~ p124)

### 5주차
- 2021.06.26, Zoom 온라인(36명 참석)
- pm 03:00 ~ pm 10:00
- 리눅스 커널 내부구조 (p124 ~ p171)

### 6주차
- 2021.07.03, Zoom 온라인(36명 참석)
- pm 03:00 ~ pm 10:00
- 리눅스 커널 내부구조 ( ~ end)

### 7주차
- 2021.07.10, Zoom 온라인(43명 참석)
- ARM System Developers Guide 책, ~p69, 3.3.3 Multiple-Register Transfer

### 8주차
- 2021.07.17, Zoom 온라인(34명 참석)
- ARM System Developers Guide 완료 
- head.S의 preserve_boot_args() -> __inval_dcache_area() 

### 9주차
- 2021.07.24, Zoom 온라인(36명 참석)
- ARM Programmers Guide: ~p161
- head.S의 el2_setup 진행중

### 10주차
- 2021.07.31, Zoom 온라인(31명 참석)
- ARM Programmers Guide: ~p190
- head.S의 el2_setup 진행중

### 11주차
- 2021.08.07, Zoom 온라인(26명 참석)
- ARM Programmers Guide: 완료
- head.S의 el2_setup 진행중

### 12주차
- 2021.08.14, Zoom 온라인(27명 참석)
- head.S의 __create_page_tables의 첫 번째 map_memory 직전 

### 13주차
- 2021.08.21, Zoom 온라인(21명 참석)
- head.S: __cpu_setup()의 tcr_compute_pa_size 매크로 직전

### 14주차
- 2021.08.28, Zoom 온라인(27명 참석)
- head.S: __primary_switch()의 __relocate_kernel() 진행중

### 15주차
- 2021.09.04, Zoom 온라인(22명 참석)
- head.S: CONFIG_RANDOMIZE_BASE 커널 옵션 부분 진행중

### 16주차
- 2021.09.11, Zoom 온라인(22명 참석)
- start_kernel() 시작 - boot_cpu_init() 직전까지

### 17주차
- 2021.09.18, Zoom 온라인(22명 참석)
- barrier, atomic

### 18주차
- 2021.09.25, Zoom 온라인(18명 참석)
- bitops, bitmap, cpumask, spinlock 진행중

### 19주차
- 2021.10.02, Zoom 온라인(19명 참석)
- spinlock, early_fixmap_init() 진행중

### 20주차
- 2021.10.09, Zoom 온라인(18명 참석)
- setup_machine_fdt() -> early_init_dt_scan_nodes() 진행중

### 21주차
- 2021.10.16, Zoom 온라인(21명 참석)
- memblock_add() -> memblock_add_range() 진행중

### 22주차
- 2021.10.23, Zoom 온라인(14명 참석)
- memblock 완료
- 
### 23주차
- 2021.10.30, Zoom 온라인(13명 참석)
- paging_init() 완료
- unflatten_dt_nodes() 진행중

### 24주차
- 2021.11.06, Zoom 온/오프라인(12명 참석)
- paging_init() 완료
- arm64_numa_init() 진행중

### 25주차
- 2021.11.13, Zoom 온/오프라인(12명 참석)
- sparse_init() 진행중

### 26주차
- 2021.11.20, Zoom 온/오프라인(14명 참석)
- sparse_init() -> sparse_init_nid() 진행중

### 27주차
- 2021.11.27, Zoom 온/오프라인(15명 참석)
- zone_sizes_init() 진행중

### 28주차
- 2021.12.04, Zoom 온/오프라인(9명 참석)
- zone_sizes_init() 진행중

### 29주차
- 2021.12.11, Zoom 온/오프라인(11명 참석)
- zone_sizes_init() 진행중

### 30주차
- 2021.12.18, Zoom 온/오프라인(11명 참석)
- psci_dt_init() 진행중

### 31주차
- 2021.12.25, 성탄 연휴

### 32주차
- 2022.01.01, Zoom 온라인(12명 참석)
- setup_nr_cpu_ids() 까지 진행완료

### 33주차
- 2022.01.08, Zoom 온라인(12명 참석)
- setup_per_cpu_areas() 진행중

### 34주차
- 2022.01.15, Zoom 온라인(12명 참석)
- setup_per_cpu_areas() 진행중

### 35주차
- 2022.01.22, Zoom 온라인(12명 참석)
- setup_per_cpu_areas() 진행중

### 36주차
- 2022.01.29, 구정 연휴

### 37주차
- 2022.02.05, Zoom 온라인(12명 참석)
- setup_per_cpu_areas() 진행완료

### 38주차
- 2022.02.12, Zoom 온라인(12명 참석)
- page_alloc_init() 진행중

### 39주차
- 2022.02.19, Zoom 온라인(12명 참석)
- mm_init()->mem_init() 진행중

### 40주차
- 2022.02.26, Zoom 온라인(7명 참석)
- alloc_page() 진행중 ... alloc_flags_nofragment()

### 41주차
- 2022.03.05, Zoom 온라인(12명 참석)
- alloc_page() 진행중 ... get_page_from_freelist()

### 42주차
- 2022.03.12, Zoom 온라인(11명 참석)
- alloc_page() 진행중 ... get_page_from_freelist()

### 43주차
- 2022.03.19, Zoom 온라인(8명 참석)
- alloc_page() 진행중 ... direct-compaction

### 44주차
- 2022.03.26, Zoom 온라인(9명 참석)
- alloc_page() 진행중 ... direct-compaction(isolation)

### 45주차
- 2022.04.02, Zoom 온라인(9명 참석)
- alloc_page() 진행중 ... direct-compaction(migration)

### 46주차
- 2022.04.09, Zoom 온라인(6명 참석)
- alloc_page() 진행중 ... direct-compaction(rmap-walk-control)

### 47주차
- 2022.04.16, Zoom 온라인(7명 참석)
- alloc_page() 진행중 ... direct-reclaim (LRU, pagevecs)

### 48주차
- 2022.04.23, Zoom 온라인(8명 참석)
- alloc_page() 진행중 ... direct-reclaim (scan & isolation)

### 49주차
- 2022.04.30, Zoom 온라인(7명 참석)
- alloc_page() 진행중 ... direct-reclaim (Workingset Detection)

### 50주차
- 2022.05.07, Zoom 온라인(8명 참석)
- alloc_page() 진행중 ... direct-reclaim (oom)

### 51주차
- 2022.05.14, Zoom 온라인(6명 참석)
- alloc_pages_vma() 진행중

### 52주차
- 2022.05.21, Zoom 온라인(5명 참석)
- sys_mmap() 진행중

### 53주차
- 2022.05.28, Zoom 온라인(5명 참석)
- sys_mmap() 진행중

### 54주차
- 2022.06.04, Zoom 온라인(4명 참석)
- sys_mmap() & do_anonymous_page() 완료

### 55주차
- 2022.06.11, Zoom 온라인(4명 참석)
- kmem_cache_init() 진행 중

### 56주차
- 2022.06.18, Zoom 온라인(5명 참석)
- kmem_cache_init() ... slab object size 및 debug 정보 초기화 완료

### 57주차
- 2022.06.25, Zoom 온라인(5명 참석)
- kmem_cache_init() && kmalloc() 완료, kfree 진행중

### 58주차
- 2022.07.02, Zoom 온라인(3명 참석)
- mm_init() 완료

### 59주차
- 2022.07.09, Zoom 온라인(5명 참석)
- cma 메모리 할당자 완료

### 60주차
- 2022.07.16, Zoom 온라인(5명 참석)
- dma_alloc_coherent() 진행중

### 61주차
- 2022.07.23, Zoom 온라인(5명 참석)
- dma memory allocator 완료

