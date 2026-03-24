// SPDX-License-Identifier: GPL-2.0
/*
 * RVT2 GSP firmware shim helper entrypoints.
 */

#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/string.h>
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

    release_firmware(fw);

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

void rvt2_gsp_detach(struct rvt2_gsp_info *info)
{
    if (!info->dev)
        return;

    cancel_delayed_work_sync(&info->heartbeat_work);
    memset(info, 0, sizeof(*info));
}
