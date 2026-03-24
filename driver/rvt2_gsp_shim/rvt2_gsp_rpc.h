/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RVT2_GSP_RPC_H_
#define _RVT2_GSP_RPC_H_

#include <linux/device.h>
#include <linux/types.h>
#include <linux/workqueue.h>

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

struct rvt2_gsp_info {
    bool ready;
    bool heartbeat_alive;
    u32 engine_count;
    u32 max_desc_size;
    u32 fw_version;
    u32 supported_ops;
    struct device *dev;
    void __iomem *mmio;
    struct delayed_work heartbeat_work;
};

int rvt2_gsp_attach(struct rvt2_gsp_info *info, struct device *dev,
                    void __iomem *mmio);
void rvt2_gsp_detach(struct rvt2_gsp_info *info);
u32 rvt2_gsp_read(struct rvt2_gsp_info *info, u32 offset);
void rvt2_gsp_write(struct rvt2_gsp_info *info, u32 offset, u32 val);
int rvt2_gsp_mbox_cmd(struct rvt2_gsp_info *info, u32 cmd);
void rvt2_gsp_latch_fault(struct rvt2_gsp_info *info);

#endif /* _RVT2_GSP_RPC_H_ */
