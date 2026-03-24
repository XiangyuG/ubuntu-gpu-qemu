// SPDX-License-Identifier: GPL-2.0
/*
 * RVT2 GSP firmware shim module (rvt2_gsp_shim.ko)
 *
 * Manages firmware loading and mailbox communication. Exports attach/detach
 * API for rvt2_core.ko to consume via KBUILD_EXTRA_SYMBOLS.
 */

#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>
#include "rvt2_gsp_rpc.h"

#define RVT2_FW_NAME            "rvt2/firmware.bin"
#define RVT2_HEARTBEAT_MS       5000

static void rvt2_heartbeat_work_fn(struct work_struct *work)
{
    struct rvt2_gsp_info *info = container_of(work, struct rvt2_gsp_info,
                                              heartbeat_work.work);
    int ret;

    ret = rvt2_gsp_mbox_cmd(info, RVT2_MBOX_CMD_HEARTBEAT);
    if (ret) {
        rvt2_gsp_latch_fault(info);
        return;
    }

    info->heartbeat_alive = (rvt2_gsp_read(info, RVT2_REG_MBOX_DATA0) == 1);
    if (!info->heartbeat_alive) {
        rvt2_gsp_latch_fault(info);
        return;
    }

    schedule_delayed_work(&info->heartbeat_work,
                          msecs_to_jiffies(RVT2_HEARTBEAT_MS));
}

int rvt2_gsp_attach(struct rvt2_gsp_info *info, struct device *dev,
                    void __iomem *mmio)
{
    const struct firmware *fw;
    int ret;

    memset(info, 0, sizeof(*info));
    info->dev = dev;
    info->mmio = mmio;

    ret = request_firmware(&fw, RVT2_FW_NAME, dev);
    if (ret) {
        dev_err(dev, "request_firmware(%s) failed: %d\n", RVT2_FW_NAME, ret);
        return ret;
    }

    /* DMA-upload firmware blob to device */
    {
        void *fw_buf;
        dma_addr_t fw_dma;
        size_t fw_sz = fw->size;

        fw_buf = dma_alloc_coherent(dev, fw_sz, &fw_dma, GFP_KERNEL);
        if (!fw_buf) {
            release_firmware(fw);
            return -ENOMEM;
        }
        memcpy(fw_buf, fw->data, fw_sz);
        release_firmware(fw);

        rvt2_gsp_write(info, RVT2_REG_FW_ADDR_LO, lower_32_bits(fw_dma));
        rvt2_gsp_write(info, RVT2_REG_FW_ADDR_HI, upper_32_bits(fw_dma));
        rvt2_gsp_write(info, RVT2_REG_FW_SIZE, (u32)fw_sz);

        ret = rvt2_gsp_mbox_cmd(info, RVT2_MBOX_CMD_LOAD_FW);
        dma_free_coherent(dev, fw_sz, fw_buf, fw_dma);
        if (ret) {
            dev_err(dev, "firmware upload failed: %d\n", ret);
            return ret;
        }
        dev_info(dev, "firmware uploaded (%zu bytes)\n", fw_sz);
    }

    ret = rvt2_gsp_mbox_cmd(info, RVT2_MBOX_CMD_INIT);
    if (ret) {
        dev_err(dev, "firmware init handshake failed: %d\n", ret);
        return ret;
    }

    ret = rvt2_gsp_mbox_cmd(info, RVT2_MBOX_CMD_QUERY_CAP);
    if (ret) {
        dev_err(dev, "capability query failed: %d\n", ret);
        return ret;
    }

    info->engine_count = rvt2_gsp_read(info, RVT2_REG_MBOX_DATA0);
    info->max_desc_size = rvt2_gsp_read(info, RVT2_REG_MBOX_DATA1);
    info->fw_version = rvt2_gsp_read(info, RVT2_REG_MBOX_DATA2);
    info->supported_ops = rvt2_gsp_read(info, RVT2_REG_MBOX_DATA3);
    info->ready = true;
    info->heartbeat_alive = true;

    INIT_DELAYED_WORK(&info->heartbeat_work, rvt2_heartbeat_work_fn);
    schedule_delayed_work(&info->heartbeat_work,
                          msecs_to_jiffies(RVT2_HEARTBEAT_MS));

    return 0;
}
EXPORT_SYMBOL_GPL(rvt2_gsp_attach);

void rvt2_gsp_detach(struct rvt2_gsp_info *info)
{
    if (!info->dev)
        return;

    cancel_delayed_work_sync(&info->heartbeat_work);
    memset(info, 0, sizeof(*info));
}
EXPORT_SYMBOL_GPL(rvt2_gsp_detach);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chao Liu <chao.liu.zevorn@gmail.com>");
MODULE_DESCRIPTION("RVT2 GSP Firmware Shim");
MODULE_FIRMWARE(RVT2_FW_NAME);
