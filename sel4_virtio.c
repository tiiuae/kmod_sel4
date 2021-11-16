// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017 Linaro Ltd.
 * Copyright (c) 2021 Technology Innovation Institute
 *
 * Based on soc/qcom/rmtfs_mem.c
 */

#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>

#define SEL4_VIRTIO_DEV_MAX  (MINORMASK + 1)

static const struct of_device_id sel4_virtio_dt_match[] = {
        { .compatible = "sel4-virtio", },
        { }
};
MODULE_DEVICE_TABLE(of, sel4_virtio_dt_match);

struct sel4_virtio {
	struct platform_device *pdev;
	dma_addr_t paddr;
	void *vaddr;
	u32 size;

	unsigned int client_id;

	struct device dev;
	struct cdev cdev;
};

static dev_t sel4_virtio_major;

static struct class sel4_virtio_class = {
	.owner  = THIS_MODULE,
	.name   = "sel4_virtio",
};

static int sel4_virtio_open(struct inode *inode, struct file *filp)
{
	struct sel4_virtio *be = container_of(inode->i_cdev,
					      struct sel4_virtio, cdev);

	get_device(&be->dev);
	filp->private_data = be;

	return 0;
}

static ssize_t sel4_virtio_read(struct file *filp,
				char __user *buf, size_t count, loff_t *f_pos)
{
	struct sel4_virtio *be = filp->private_data;

	if (*f_pos >= be->size)
		return 0;

	if (*f_pos + count >= be->size)
		count = be->size - *f_pos;

	if (copy_to_user(buf, be->vaddr + *f_pos, count))
		return -EFAULT;

	*f_pos += count;
		return count;
}

static ssize_t sel4_virtio_write(struct file *filp,
				 const char __user *buf, size_t count,
				 loff_t *f_pos)
{
	struct sel4_virtio *be = filp->private_data;

	if (*f_pos >= be->size)
		return 0;

	if (*f_pos + count >= be->size)
		count = be->size - *f_pos;

	if (copy_from_user(be->vaddr + *f_pos, buf, count))
		return -EFAULT;

	*f_pos += count;
		return count;
}

static int sel4_virtio_release(struct inode *inode, struct file *filp)
{
	struct sel4_virtio *be = filp->private_data;

	put_device(&be->dev);

	return 0;
}

static int sel4_virtio_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct sel4_virtio *be = filep->private_data;

	if (vma->vm_end - vma->vm_start > be->size) {
		dev_dbg(&be->dev,
		"vm_end[%lu] - vm_start[%lu] [%lu] > be->size[%pa]\n",
		vma->vm_end, vma->vm_start,
		(vma->vm_end - vma->vm_start), &be->size);
		return -EINVAL;
	}

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma,
			       vma->vm_start,
			       be->paddr >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
}

static const struct file_operations sel4_virtio_fops = {
	.owner = THIS_MODULE,
	.open = sel4_virtio_open,
	.read = sel4_virtio_read,
	.write = sel4_virtio_write,
	.release = sel4_virtio_release,
	.llseek = default_llseek,
	.mmap = sel4_virtio_mmap,
};

static const struct attribute_group *sel4_virtio_groups[] = {
	NULL,
};

static void sel4_virtio_release_device(struct device *dev)
{
	struct sel4_virtio *be = container_of(dev, struct sel4_virtio, dev);
	kfree(be);
}

static int sel4_virtio_probe(struct platform_device *pdev)
{
	struct device *dev;
	int ret;
	struct sel4_virtio *be;
	struct device_node *node;
	struct resource r;
	u32 client_id;

	dev = &pdev->dev;

	be = kzalloc(sizeof(*be), GFP_KERNEL);
	if (!be)
		return -ENOMEM;

	platform_set_drvdata(pdev, be);

	be->pdev = pdev;

	ret = of_property_read_u32(dev->of_node, "client-id", &client_id);
	if (ret) {
		dev_err(&pdev->dev, "failed to parse \"client-id\"\n");
		return ret;
	}

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(dev, "seL4 virtio device does not define memory-region\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret) {
		dev_err(dev, "failed to resolve virtio memory region\n");
		return ret;
	}

	of_node_put(node);

	be->paddr = r.start;
	be->size = resource_size(&r);
	be->client_id = client_id;

	device_initialize(&be->dev);
	be->dev.parent = &pdev->dev;
	be->dev.groups = sel4_virtio_groups;
	be->dev.release = sel4_virtio_release_device;

	be->vaddr = devm_memremap(dev, be->paddr, be->size, MEMREMAP_WB);
	if (IS_ERR(be->vaddr)) {
		dev_err(dev, "failed to map memory region: %pa\n", &be->paddr);
		return PTR_ERR(be->vaddr);
	}

	dev_info(&pdev->dev, "mapped %x bytes at %pa\n", be->size, &be->paddr);

	cdev_init(&be->cdev, &sel4_virtio_fops);
	be->cdev.owner = THIS_MODULE;

	dev_set_name(&be->dev, "sel4_virtio%d", client_id);
	be->dev.id = client_id;
	be->dev.class = &sel4_virtio_class;
	be->dev.devt = MKDEV(MAJOR(sel4_virtio_major), client_id);

	ret = cdev_device_add(&be->cdev, &be->dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to add cdev: %d\n", ret);
		goto put_device;
	}

	dev_set_drvdata(&pdev->dev, be);

	return 0;

put_device:
	put_device(&be->dev);

	return ret;
}

static int sel4_virtio_remove(struct platform_device *pdev)
{
	struct sel4_virtio *be = dev_get_drvdata(&pdev->dev);

	cdev_device_del(&be->cdev, &be->dev);
	put_device(&be->dev);

	devm_memunmap(&pdev->dev, be->vaddr);

	return 0;
}

static struct platform_driver sel4_virtio_driver = {
	.probe  = sel4_virtio_probe,
	.remove = sel4_virtio_remove,
	.driver = {
		.name   = "sel4_virtio",
		.of_match_table = sel4_virtio_dt_match,
	},
};

static int __init sel4_virtio_init(void)
{
	int ret;

	ret = class_register(&sel4_virtio_class);
	if (ret)
		return ret;

	ret = alloc_chrdev_region(&sel4_virtio_major, 0,
				  SEL4_VIRTIO_DEV_MAX, "sel4_virtio");
	if (ret < 0) {
		pr_err("sel4_virtio: failed to allocate char dev region\n");
		goto unregister_class;
	}

	ret = platform_driver_register(&sel4_virtio_driver);
	if (ret < 0) {
		pr_err("sel4_virtio: failed to register sel4_virtio driver\n");
		goto unregister_chrdev;
	}

	return 0;

unregister_chrdev:
	unregister_chrdev_region(sel4_virtio_major, SEL4_VIRTIO_DEV_MAX);
unregister_class:
	class_unregister(&sel4_virtio_class);
	return ret;
}
module_init(sel4_virtio_init);

static void __exit sel4_virtio_exit(void)
{
	platform_driver_unregister(&sel4_virtio_driver);
	unregister_chrdev_region(sel4_virtio_major, SEL4_VIRTIO_DEV_MAX);
	class_unregister(&sel4_virtio_class);
}
module_exit(sel4_virtio_exit);

MODULE_AUTHOR("Technology Innovation Institute");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Driver support seL4 virtio backend");
