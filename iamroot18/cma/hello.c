#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>

static dma_addr_t g_dma_addr;
static void *g_dma;
#define ALLOC_SIZE	1024

/*
 * IAMROOT, 2022.07.30:
 * ------ default_cma.dts
 * - menuconfig에서 libraries에서 dma-api debug on후 /sys/kerenel/debug/dma-api/dump에서 확인
 * - dump 내용
 *  |  dev_name | driver name  | map type    | hash index | phys addr   | pfn    | dev_addr   | size  | 
 *     iamroot    IAMROOT_NAME      coherent   idx 4736      P=42500000  N=42500   D=42500000   L=400  
 *
 *  | dir              | map error check string             |
 *   DMA_BIDIRECTIONAL   dma map error check not applicable
 *
 *
 * ------ cma2.dts
 *  - default cma와 다른 reserved가 있는상태.
 *  - default cma는 즉시 cma로 잡히고 다른 한개는 reserved로만 예약된다.
 *  - log
 *  [    0.000000] Memory: 3817648K/4194304K available (14912K kernel code, 3068K rwdata,
 *  8252K rodata, 6208K init, 880K bss, 245584K reserved, 131072K cma-reserved)

 */
static int iamroot_probe(struct platform_device *pdev)
{
	int err;

	printk(KERN_INFO "%s %d %px\n", __func__, __LINE__, pdev->dev.dma_mem);

#if 0
	struct device_node *mem_node;
	struct reserved_mem *rmem;
	mem_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!mem_node) {
		dev_err(&pdev->dev, "No memory-region found for index %d\n", 0);
		return -ENODEV;
	}

	rmem = of_reserved_mem_lookup(mem_node);
	if (!rmem) {
		dev_err(&pdev->dev, "of_reserved_mem_lookup() returned NULL\n");
		return -ENODEV;
	}

	pdev->dev.dma_mem = rmem->priv;

	printk(KERN_INFO "%s %d %px %x %x %s\n", __func__, __LINE__, pdev->dev.dma_mem,
			rmem->base, (uint32_t)rmem->size, rmem->name);
#endif

	err = of_reserved_mem_device_init_by_name(&pdev->dev, pdev->dev.of_node, "iamroot_buffer2");
	if (err < 0) {
		dev_err(&pdev->dev, "failed to get nominal EMC table: %d\n", err);
		return err;
	}
	g_dma = dma_alloc_coherent(&pdev->dev, ALLOC_SIZE, &g_dma_addr, GFP_KERNEL);
	return 0;
}

static int iamroot_remove(struct platform_device *pdev)
{
	printk(KERN_INFO "%s %d\n", __func__, __LINE__);
	dma_free_coherent(&pdev->dev, ALLOC_SIZE, g_dma, g_dma_addr);
	return 0;
}

static const struct of_device_id iamroot_ids[] = {
	{.compatible = "iamroot_comp"},
	{},
};

static struct platform_driver iamroot_driver = {
	.driver		= {
		.name	= "IAMROOT_NAME",
		.of_match_table = iamroot_ids,
	},
	.probe		= iamroot_probe,
	.remove         = iamroot_remove
};

static int hello_init(void) {
	printk(KERN_INFO "%s %d\n", __func__, __LINE__);
	return platform_driver_register(&iamroot_driver);
}

static void hello_exit(void) {
	printk(KERN_INFO "%s %d\n", __func__, __LINE__);
	platform_driver_unregister(&iamroot_driver);
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkr");

