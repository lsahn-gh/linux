#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

static struct kmem_cache * g_cache;

#define ALLOC_BYTES	(10)
//#define DOUBLE_FREE
//#define OVERWRITE_LEFT
//#define OVERWRITE_RIGHT
//#define OVERWRITE_RIGHT2
#define POISON_TEST


static uint8_t *arr;

static int hello_init(void) {
	if (g_cache == NULL)
	{
		g_cache = kmem_cache_create("iamroot_cache", ALLOC_BYTES, 0,
				SLAB_HWCACHE_ALIGN, NULL);
	}

	arr = kmem_cache_alloc(g_cache, GFP_KERNEL);
#ifdef OVERWRITE_LEFT
	memset(arr - 1, 0x1, 1);
#endif

#ifdef OVERWRITE_RIGHT
	memset(&arr[ALLOC_BYTES], 0x1, 1);
#endif

#ifdef OVERWRITE_RIGHT2
	memset(&arr[ALLOC_BYTES+7], 0x1, 1);
#endif

#ifdef POISON_TEST
	kmem_cache_free(g_cache, arr);
	memset(arr, 1, 1);
	arr = kmem_cache_alloc(g_cache, GFP_KERNEL);
#endif
	return 0;
}

static void hello_exit(void) {

	kmem_cache_free(g_cache, arr);
#ifdef DOUBLE_FREE
	kmem_cache_free(g_cache, arr);
#endif

	if(g_cache)
	{
		kmem_cache_destroy(g_cache);
		g_cache = NULL;
	}
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkr");

