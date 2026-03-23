// SPDX-License-Identifier: GPL-2.0
/*
 * RVT2 GSP mailbox RPC helpers.
 */

#include <linux/io.h>
#include <linux/errno.h>
#include "rvt2_gsp_rpc.h"

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

    rvt2_gsp_write(info, RVT2_REG_MBOX_CMD, cmd);
    status = rvt2_gsp_read(info, RVT2_REG_MBOX_STATUS);

    if (status == RVT2_MBOX_STATUS_ERROR)
        return -EIO;
    if (status != RVT2_MBOX_STATUS_DONE)
        return -ETIMEDOUT;

    return 0;
}
EXPORT_SYMBOL_GPL(rvt2_gsp_mbox_cmd);
