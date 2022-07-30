#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

/*
 * IAMROOT, 2022.07.30:
 * - slab cache에서 할당해온다. slabtop -sa 명령어 등으로 할당을
 *   확인할 수 있다.
 */
#define CNT	10000
//static struct page *arr[CNT];
static void *arr[CNT];

static int hello_init(void) {
	uint32_t i;
#define ALLOC_BYTES	(4 * 1024)
	//int order = get_order(ALLOC_BYTES);

	printk(KERN_INFO "alloc : %u byte. order %d. cnt %u\n",
			ALLOC_BYTES, -1, CNT);
	for (i = 0; i < CNT; i++) {
		if (arr[i])
			continue;
#if 0
		arr[i] = alloc_pages(GFP_KERNEL, order);
#else
		arr[i] = kmalloc(ALLOC_BYTES, GFP_KERNEL);
#endif
		if (arr[i] == NULL)
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
#if 0
		__free_pages(arr[i], order);
#else
		kfree(arr[i]);
#endif
		arr[i] = NULL;
	}
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkr");

