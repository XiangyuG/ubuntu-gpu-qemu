// SPDX-License-Identifier: GPL-2.0
/*
 * RVT2 Ternary MatMul Accelerator - PCI driver core
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include "rvt2_drv.h"

static int debug_level;
module_param(debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug verbosity level (0=off, 3=max)");

static struct class *rvt2_class;

/* ---- File operations ---- */

static long rvt2_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct rvt2_device *rdev = filp->private_data;
    void __user *uarg = (void __user *)arg;

    if (!rdev->fw_ready && cmd != RVT2_IOCTL_BO_CREATE &&
        cmd != RVT2_IOCTL_BO_INFO && cmd != RVT2_IOCTL_BO_DESTROY)
        return -EIO;

    switch (cmd) {
    case RVT2_IOCTL_BO_CREATE:
        return rvt2_bo_create_ioctl(rdev, uarg);
    case RVT2_IOCTL_BO_INFO:
        return rvt2_bo_info_ioctl(rdev, uarg);
    case RVT2_IOCTL_BO_DESTROY:
        return rvt2_bo_destroy_ioctl(rdev, uarg);
    case RVT2_IOCTL_SUBMIT:
        if (!rdev->fw_ready)
            return -EIO;
        return rvt2_submit_ioctl(rdev, uarg);
    case RVT2_IOCTL_WAIT:
        return rvt2_wait_ioctl(rdev, uarg);
    default:
        return -ENOTTY;
    }
}

static int rvt2_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct rvt2_device *rdev = filp->private_data;
    return rvt2_bo_mmap(rdev, vma);
}

static unsigned int rvt2_poll(struct file *filp, struct poll_table_struct *wait)
{
    struct rvt2_device *rdev = filp->private_data;
    unsigned int mask = 0;

    poll_wait(filp, &rdev->fence_wq, wait);

    if (rvt2_poll_ready(rdev))
        mask |= EPOLLIN | EPOLLRDNORM;

    return mask;
}

static int rvt2_open(struct inode *inode, struct file *filp)
{
    struct miscdevice *misc = filp->private_data;
    struct rvt2_device *rdev = container_of(misc, struct rvt2_device, miscdev);
    filp->private_data = rdev;
    return 0;
}

static const struct file_operations rvt2_fops = {
    .owner          = THIS_MODULE,
    .open           = rvt2_open,
    .unlocked_ioctl = rvt2_ioctl,
    .mmap           = rvt2_mmap,
    .poll           = rvt2_poll,
};

/* ---- Firmware init via mailbox ---- */

/* ---- Queue setup ---- */

static int rvt2_queue_init(struct rvt2_device *rdev)
{
    struct device *dev = &rdev->pdev->dev;

    rdev->cmdq_cpu = dma_alloc_coherent(dev,
                                        RVT2_CMDQ_ENTRIES * RVT2_DESC_SIZE,
                                        &rdev->cmdq_dma, GFP_KERNEL);
    if (!rdev->cmdq_cpu)
        return -ENOMEM;

    rdev->cplq_cpu = dma_alloc_coherent(dev,
                                        RVT2_CPLQ_ENTRIES * RVT2_CPL_SIZE,
                                        &rdev->cplq_dma, GFP_KERNEL);
    if (!rdev->cplq_cpu) {
        dma_free_coherent(dev, RVT2_CMDQ_ENTRIES * RVT2_DESC_SIZE,
                          rdev->cmdq_cpu, rdev->cmdq_dma);
        return -ENOMEM;
    }

    /* Program queue addresses into device */
    rvt2_write(rdev, RVT2_REG_CMDQ_BASE_LO, lower_32_bits(rdev->cmdq_dma));
    rvt2_write(rdev, RVT2_REG_CMDQ_BASE_HI, upper_32_bits(rdev->cmdq_dma));
    rvt2_write(rdev, RVT2_REG_CMDQ_SIZE, RVT2_CMDQ_ENTRIES);

    rvt2_write(rdev, RVT2_REG_CPLQ_BASE_LO, lower_32_bits(rdev->cplq_dma));
    rvt2_write(rdev, RVT2_REG_CPLQ_BASE_HI, upper_32_bits(rdev->cplq_dma));
    rvt2_write(rdev, RVT2_REG_CPLQ_SIZE, RVT2_CPLQ_ENTRIES);

    /* Enable device */
    rvt2_write(rdev, RVT2_REG_CONTROL, RVT2_CTRL_ENABLE);

    rdev->cmdq_tail = 0;
    rdev->cplq_head = 0;

    return 0;
}

static void rvt2_queue_fini(struct rvt2_device *rdev)
{
    struct device *dev = &rdev->pdev->dev;

    rvt2_write(rdev, RVT2_REG_CONTROL, RVT2_CTRL_RESET);

    if (rdev->cmdq_cpu)
        dma_free_coherent(dev, RVT2_CMDQ_ENTRIES * RVT2_DESC_SIZE,
                          rdev->cmdq_cpu, rdev->cmdq_dma);
    if (rdev->cplq_cpu)
        dma_free_coherent(dev, RVT2_CPLQ_ENTRIES * RVT2_CPL_SIZE,
                          rdev->cplq_cpu, rdev->cplq_dma);
}

/* ---- PCI probe/remove ---- */

static int rvt2_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct rvt2_device *rdev;
    u32 dev_id;
    int ret;

    rdev = devm_kzalloc(&pdev->dev, sizeof(*rdev), GFP_KERNEL);
    if (!rdev)
        return -ENOMEM;

    rdev->pdev = pdev;
    pci_set_drvdata(pdev, rdev);

    ret = pcim_enable_device(pdev);
    if (ret) {
        dev_err(&pdev->dev, "pcim_enable_device failed: %d\n", ret);
        return ret;
    }

    pci_set_master(pdev);

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret) {
        ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
        if (ret) {
            dev_err(&pdev->dev, "dma_set_mask failed: %d\n", ret);
            return ret;
        }
    }

    ret = pcim_iomap_regions(pdev, BIT(0), "rvt2_core");
    if (ret) {
        dev_err(&pdev->dev, "pcim_iomap_regions failed: %d\n", ret);
        return ret;
    }

    rdev->mmio = pcim_iomap_table(pdev)[0];
    if (!rdev->mmio)
        return -ENOMEM;

    /* Verify device identity */
    dev_id = rvt2_read(rdev, RVT2_REG_ID);
    dev_info(&pdev->dev, "device ID register: 0x%08x\n", dev_id);
    if (dev_id != 0x52565432) {
        dev_err(&pdev->dev, "unexpected device ID: 0x%08x\n", dev_id);
        return -ENODEV;
    }

    mutex_init(&rdev->bo_lock);
    mutex_init(&rdev->submit_lock);
    idr_init(&rdev->bo_idr);
    spin_lock_init(&rdev->fence_lock);
    init_waitqueue_head(&rdev->fence_wq);
    INIT_LIST_HEAD(&rdev->fences);
    rdev->next_seqno = 1;

    rvt2_submit_init(rdev);

    ret = rvt2_irq_init(rdev);
    if (ret) {
        dev_err(&pdev->dev, "rvt2_irq_init failed: %d\n", ret);
        return ret;
    }

    ret = rvt2_gsp_attach(&rdev->gsp, &pdev->dev, rdev->mmio);
    if (ret) {
        dev_err(&pdev->dev, "rvt2_gsp_attach failed: %d\n", ret);
        goto err_irq;
    }
    rdev->engine_count = rdev->gsp.engine_count;
    rdev->fw_version = rdev->gsp.fw_version;
    rdev->fw_ready = rdev->gsp.ready;

    ret = rvt2_queue_init(rdev);
    if (ret) {
        dev_err(&pdev->dev, "rvt2_queue_init failed: %d\n", ret);
        goto err_irq;
    }

    /* Register char device */
    rdev->miscdev.minor = MISC_DYNAMIC_MINOR;
    rdev->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "rvt2_%d", 0);
    rdev->miscdev.fops = &rvt2_fops;
    ret = misc_register(&rdev->miscdev);
    if (ret)
        goto err_queue;

    rdev->class_dev = rdev->miscdev.this_device;

    ret = rvt2_sysfs_init(rdev);
    if (ret)
        goto err_misc;

    dev_info(&pdev->dev, "RVT2 accelerator probed successfully\n");
    return 0;

err_misc:
    misc_deregister(&rdev->miscdev);
err_queue:
    rvt2_queue_fini(rdev);
    rvt2_gsp_detach(&rdev->gsp);
err_irq:
    rvt2_irq_fini(rdev);
    return ret;
}

static void rvt2_remove(struct pci_dev *pdev)
{
    struct rvt2_device *rdev = pci_get_drvdata(pdev);

    rvt2_sysfs_fini(rdev);
    misc_deregister(&rdev->miscdev);
    rvt2_queue_fini(rdev);
    rvt2_bo_cleanup(rdev);
    rvt2_gsp_detach(&rdev->gsp);
    rvt2_irq_fini(rdev);
    idr_destroy(&rdev->bo_idr);
}

static const struct pci_device_id rvt2_pci_ids[] = {
    { PCI_DEVICE(RVT2_PCI_VENDOR_ID, RVT2_PCI_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, rvt2_pci_ids);

static struct pci_driver rvt2_pci_driver = {
    .name     = "rvt2_core",
    .id_table = rvt2_pci_ids,
    .probe    = rvt2_probe,
    .remove   = rvt2_remove,
};

static int __init rvt2_init(void)
{
    int ret;

    rvt2_class = class_create("rvt2");
    if (IS_ERR(rvt2_class))
        return PTR_ERR(rvt2_class);

    ret = pci_register_driver(&rvt2_pci_driver);
    if (ret) {
        class_destroy(rvt2_class);
        return ret;
    }
    return 0;
}

static void __exit rvt2_exit(void)
{
    pci_unregister_driver(&rvt2_pci_driver);
    class_destroy(rvt2_class);
}

module_init(rvt2_init);
module_exit(rvt2_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chao Liu <chao.liu.zevorn@gmail.com>");
MODULE_DESCRIPTION("RVT2 Ternary MatMul Accelerator driver");
MODULE_FIRMWARE("rvt2/firmware.bin");
