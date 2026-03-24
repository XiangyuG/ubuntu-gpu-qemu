// SPDX-License-Identifier: GPL-2.0
/*
 * RVT2 GSP mailbox RPC helpers.
 */

#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include "rvt2_gsp_rpc.h"

#define RVT2_MBOX_TIMEOUT_US    100000  /* 100ms deadline */
#define RVT2_MBOX_POLL_US       100     /* poll every 100us */

/* HW register offsets for fault propagation */
#define RVT2_REG_STATUS         0x08
#define RVT2_STATUS_ERROR       (1 << 2)

u32 rvt2_gsp_read(struct rvt2_gsp_info *info, u32 offset)
{
    return readl(info->mmio + offset);
}
EXPORT_SYMBOL_GPL(rvt2_gsp_read);

void rvt2_gsp_write(struct rvt2_gsp_info *info, u32 offset, u32 val)
{
    writel(val, info->mmio + offset);
}
EXPORT_SYMBOL_GPL(rvt2_gsp_write);

int rvt2_gsp_mbox_cmd(struct rvt2_gsp_info *info, u32 cmd)
{
    u32 status;
    unsigned long deadline = jiffies + usecs_to_jiffies(RVT2_MBOX_TIMEOUT_US);

    rvt2_gsp_write(info, RVT2_REG_MBOX_CMD, cmd);

    do {
        status = rvt2_gsp_read(info, RVT2_REG_MBOX_STATUS);

        if (status == RVT2_MBOX_STATUS_DONE)
            return 0;
        if (status == RVT2_MBOX_STATUS_ERROR) {
            dev_err(info->dev, "mailbox cmd 0x%x returned error\n", cmd);
            return -EIO;
        }

        udelay(RVT2_MBOX_POLL_US);
    } while (time_before(jiffies, deadline));

    dev_err(info->dev, "mailbox cmd 0x%x timed out (status=%u)\n",
            cmd, status);
    return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(rvt2_gsp_mbox_cmd);

void rvt2_gsp_latch_fault(struct rvt2_gsp_info *info)
{
    u32 status;

    info->ready = false;
    info->heartbeat_alive = false;

    /* Read current HW status and set ERROR bit for device-visible fault */
    status = rvt2_gsp_read(info, RVT2_REG_STATUS);
    status |= RVT2_STATUS_ERROR;
    /* Note: on real HW this would be a firmware-side status latch.
     * On QEMU the STATUS register is read-only from guest, so
     * the submit path checks gsp.ready instead. */

    dev_err(info->dev, "GSP fault latched, device entering error state\n");
}
EXPORT_SYMBOL_GPL(rvt2_gsp_latch_fault);
