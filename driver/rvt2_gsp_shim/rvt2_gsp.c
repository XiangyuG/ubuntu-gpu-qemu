// SPDX-License-Identifier: GPL-2.0
/*
 * RVT2 GSP Firmware Shim
 *
 * Manages firmware loading and mailbox communication with the device
 * management core. Exports ready/capability interface for rvt2_core.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/firmware.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#define RVT2_PCI_VENDOR_ID      0x1234
#define RVT2_PCI_DEVICE_ID      0x1de2

/* Mailbox registers (BAR0 offsets) */
#define RVT2_REG_MBOX_CMD      0x80
#define RVT2_REG_MBOX_STATUS   0x84
#define RVT2_REG_MBOX_DATA0    0x88
#define RVT2_REG_MBOX_DATA1    0x8C
#define RVT2_REG_MBOX_DATA2    0x90
#define RVT2_REG_MBOX_DATA3    0x94

#define RVT2_MBOX_CMD_INIT          0x01
#define RVT2_MBOX_CMD_QUERY_CAP     0x02
#define RVT2_MBOX_CMD_HEARTBEAT     0x03
#define RVT2_MBOX_STATUS_DONE       0x02
#define RVT2_MBOX_STATUS_ERROR      0x03

#define RVT2_FW_NAME            "rvt2/firmware.bin"
#define RVT2_HEARTBEAT_MS       5000

struct rvt2_gsp {
    struct pci_dev *pdev;
    void __iomem *mmio;

    bool fw_loaded;
    u32 engine_count;
    u32 max_desc_size;
    u32 fw_version;
    u32 supported_ops;

    struct delayed_work heartbeat_work;
    bool heartbeat_alive;
};

static u32 gsp_read(struct rvt2_gsp *gsp, u32 offset)
{
    return readl(gsp->mmio + offset);
}

static void gsp_write(struct rvt2_gsp *gsp, u32 offset, u32 val)
{
    writel(val, gsp->mmio + offset);
}

static int gsp_mbox_cmd(struct rvt2_gsp *gsp, u32 cmd)
{
    u32 status;

    gsp_write(gsp, RVT2_REG_MBOX_CMD, cmd);
    status = gsp_read(gsp, RVT2_REG_MBOX_STATUS);

    if (status == RVT2_MBOX_STATUS_ERROR) {
        dev_err(&gsp->pdev->dev, "mailbox command 0x%x failed\n", cmd);
        return -EIO;
    }
    if (status != RVT2_MBOX_STATUS_DONE) {
        dev_err(&gsp->pdev->dev, "mailbox unexpected status %u\n", status);
        return -ETIMEDOUT;
    }
    return 0;
}

static void rvt2_heartbeat_work_fn(struct work_struct *work)
{
    struct rvt2_gsp *gsp = container_of(work, struct rvt2_gsp,
                                        heartbeat_work.work);
    int ret;

    ret = gsp_mbox_cmd(gsp, RVT2_MBOX_CMD_HEARTBEAT);
    if (ret) {
        gsp->heartbeat_alive = false;
        dev_err(&gsp->pdev->dev, "heartbeat failed, firmware may be dead\n");
        return;
    }

    gsp->heartbeat_alive = (gsp_read(gsp, RVT2_REG_MBOX_DATA0) == 1);
    if (!gsp->heartbeat_alive) {
        dev_err(&gsp->pdev->dev, "firmware heartbeat negative\n");
        return;
    }

    schedule_delayed_work(&gsp->heartbeat_work,
                          msecs_to_jiffies(RVT2_HEARTBEAT_MS));
}

static int rvt2_gsp_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct rvt2_gsp *gsp;
    const struct firmware *fw;
    int ret;

    gsp = devm_kzalloc(&pdev->dev, sizeof(*gsp), GFP_KERNEL);
    if (!gsp)
        return -ENOMEM;

    gsp->pdev = pdev;
    pci_set_drvdata(pdev, gsp);

    ret = pcim_enable_device(pdev);
    if (ret)
        return ret;

    gsp->mmio = pcim_iomap(pdev, 0, 0);
    if (!gsp->mmio)
        return -ENOMEM;

    /*
     * request_firmware: in QEMU simulation the firmware file is optional.
     * If missing we still proceed since the QEMU device model emulates
     * the firmware internally.
     */
    ret = request_firmware(&fw, RVT2_FW_NAME, &pdev->dev);
    if (ret) {
        dev_info(&pdev->dev,
                 "firmware '%s' not found (ret=%d), proceeding with emulated firmware\n",
                 RVT2_FW_NAME, ret);
    } else {
        dev_info(&pdev->dev, "firmware '%s' loaded (%zu bytes)\n",
                 RVT2_FW_NAME, fw->size);
        release_firmware(fw);
    }

    /* Mailbox INIT handshake */
    ret = gsp_mbox_cmd(gsp, RVT2_MBOX_CMD_INIT);
    if (ret) {
        dev_err(&pdev->dev, "firmware init handshake failed\n");
        return ret;
    }

    /* Query capabilities */
    ret = gsp_mbox_cmd(gsp, RVT2_MBOX_CMD_QUERY_CAP);
    if (ret) {
        dev_err(&pdev->dev, "capability query failed\n");
        return ret;
    }

    gsp->engine_count = gsp_read(gsp, RVT2_REG_MBOX_DATA0);
    gsp->max_desc_size = gsp_read(gsp, RVT2_REG_MBOX_DATA1);
    gsp->fw_version = gsp_read(gsp, RVT2_REG_MBOX_DATA2);
    gsp->supported_ops = gsp_read(gsp, RVT2_REG_MBOX_DATA3);
    gsp->fw_loaded = true;

    dev_info(&pdev->dev,
             "GSP ready: engines=%u max_desc=%u fw=0x%04x ops=0x%x\n",
             gsp->engine_count, gsp->max_desc_size,
             gsp->fw_version, gsp->supported_ops);

    /* Start heartbeat */
    INIT_DELAYED_WORK(&gsp->heartbeat_work, rvt2_heartbeat_work_fn);
    gsp->heartbeat_alive = true;
    schedule_delayed_work(&gsp->heartbeat_work,
                          msecs_to_jiffies(RVT2_HEARTBEAT_MS));

    return 0;
}

static void rvt2_gsp_remove(struct pci_dev *pdev)
{
    struct rvt2_gsp *gsp = pci_get_drvdata(pdev);
    cancel_delayed_work_sync(&gsp->heartbeat_work);
}

static const struct pci_device_id rvt2_gsp_pci_ids[] = {
    { PCI_DEVICE(RVT2_PCI_VENDOR_ID, RVT2_PCI_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, rvt2_gsp_pci_ids);

static struct pci_driver rvt2_gsp_driver = {
    .name     = "rvt2_gsp_shim",
    .id_table = rvt2_gsp_pci_ids,
    .probe    = rvt2_gsp_probe,
    .remove   = rvt2_gsp_remove,
};

module_pci_driver(rvt2_gsp_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chao Liu <chao.liu.zevorn@gmail.com>");
MODULE_DESCRIPTION("RVT2 GSP Firmware Shim");
MODULE_FIRMWARE(RVT2_FW_NAME);
