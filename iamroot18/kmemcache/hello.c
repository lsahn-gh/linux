#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

static struct kmem_cache * g_cache;
static struct kmem_cache * g_cache2;

#define CNT	100
#define CNT2	50
#define ALLOC_BYTES	(832)
#define ALLOC_BYTES2	(798)

static void *arr[CNT];
static void *arr2[CNT];

/*
 * IAMROOT, 2022.07.30:
 * - 직접 slab cache를 생성한다.
 * - unmerged가 아니면(보통의 경우이면) :xxxx.. 로 unique id를 가진
 *   slab cache가 생기고 거기에 link되는 식으로 만들어진다.
 * - /sys/kernel/slab/iamroot_cache 에 생성된다.
 */
static int hello_init(void) {
	uint32_t i;

	if (g_cache == NULL)
	{
/*
 * IAMROOT, 2022.07.30:
 * - align은 방식은 보통3가지 방식을 쓴다.
 *   1. 0. 최소 size(long size)로 align
 *   2. size랑 똑같게한다.
 *   3. 0을 쓰고 flag로 SLAB_HWCACHE_ALIGN. cache align.
 */
		g_cache = kmem_cache_create("iamroot_cache", ALLOC_BYTES, 0,
				SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
		g_cache2 = kmem_cache_create("iamroot_cache2", ALLOC_BYTES2, 0,
				SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
	}

	printk(KERN_INFO "alloc : %u byte. cnt %u\n", ALLOC_BYTES, CNT);
	for (i = 0; i < CNT; i++) {
		if (arr[i])
			continue;
		arr[i] = kmem_cache_alloc(g_cache, GFP_KERNEL);
		if (arr[i] == NULL)
		{
			printk(KERN_INFO "%d alloc fail.\n", i);
		}
	}

	for (i = 0; i < CNT2; i++) {
		if (arr2[i])
			continue;
		arr2[i] = kmem_cache_alloc(g_cache2, GFP_KERNEL);
		if (arr2[i] == NULL)
		{
			printk(KERN_INFO "%d alloc fail.\n", i);
		}
	}
#if 0
	uint8_t *data = page_address(arr[0]);
	memset(data, 0, ALLOCn);
#endif
	return 0;
}

static void hello_exit(void) {
	uint32_t i;
	//int order = get_order(ALLOC_BYTES);

	for (i = 0; i < CNT; i++) {
		if (arr[i] == NULL)
			continue;
		kmem_cache_free(g_cache, arr[i]);
		arr[i] = NULL;
	}

	for (i = 0; i < CNT2; i++) {
		if (arr2[i] == NULL)
			continue;
		kmem_cache_free(g_cache2, arr2[i]);
		arr2[i] = NULL;
	}

	if(g_cache)
	{
		kmem_cache_destroy(g_cache);
		kmem_cache_destroy(g_cache2);
		g_cache = NULL;
		g_cache2 = NULL;
	}
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkr");

