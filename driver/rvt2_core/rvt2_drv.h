/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RVT2_DRV_H_
#define _RVT2_DRV_H_

#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include "../../include/uapi/rvt2_drm.h"
#include "../rvt2_gsp_shim/rvt2_gsp_rpc.h"

#define RVT2_PCI_VENDOR_ID      0x1234
#define RVT2_PCI_DEVICE_ID      0x1de2

/* BAR0 register offsets (must match QEMU device model) */
#define RVT2_REG_ID             0x00
#define RVT2_REG_VERSION        0x04
#define RVT2_REG_STATUS         0x08
#define RVT2_REG_CONTROL        0x0C
#define RVT2_REG_IRQ_STATUS     0x10
#define RVT2_REG_IRQ_MASK       0x14
#define RVT2_REG_CMDQ_BASE_LO  0x20
#define RVT2_REG_CMDQ_BASE_HI  0x24
#define RVT2_REG_CMDQ_SIZE     0x28
#define RVT2_REG_CMDQ_HEAD     0x2C
#define RVT2_REG_CMDQ_TAIL     0x30
#define RVT2_REG_DOORBELL       0x34
#define RVT2_REG_CPLQ_BASE_LO  0x40
#define RVT2_REG_CPLQ_BASE_HI  0x44
#define RVT2_REG_CPLQ_SIZE     0x48
#define RVT2_REG_CPLQ_HEAD     0x4C
#define RVT2_REG_CPLQ_TAIL     0x50
#define RVT2_REG_ENGINE_COUNT   0x60
#define RVT2_REG_MAX_DESC_SIZE  0x64
#define RVT2_REG_MBOX_CMD      0x80
#define RVT2_REG_MBOX_STATUS   0x84
#define RVT2_REG_MBOX_DATA0    0x88
#define RVT2_REG_MBOX_DATA1    0x8C
#define RVT2_REG_MBOX_DATA2    0x90
#define RVT2_REG_MBOX_DATA3    0x94

#define RVT2_STATUS_READY       (1 << 0)
#define RVT2_STATUS_BUSY        (1 << 1)
#define RVT2_STATUS_ERROR       (1 << 2)
#define RVT2_STATUS_FW_LOADED   (1 << 3)

#define RVT2_CTRL_ENABLE        (1 << 0)
#define RVT2_CTRL_RESET         (1 << 1)

#define RVT2_MBOX_CMD_INIT          0x01
#define RVT2_MBOX_CMD_QUERY_CAP     0x02
#define RVT2_MBOX_CMD_HEARTBEAT     0x03
#define RVT2_MBOX_STATUS_DONE       0x02

#define RVT2_CMDQ_ENTRIES       256
#define RVT2_CPLQ_ENTRIES       256
#define RVT2_DESC_SIZE          64
#define RVT2_CPL_SIZE           16

struct rvt2_bo {
    struct kref ref;
    struct rvt2_device *rdev;
    u32 handle;
    size_t size;
    void *cpu_addr;
    dma_addr_t dma_addr;
    bool destroyed;
    bool is_hdm;                    /* allocated from BAR2 HDM window */
    unsigned long hdm_start_page;   /* start page index in HDM */
    unsigned long hdm_page_count;   /* number of pages allocated */
    struct list_head vma_list;
    struct mutex vma_lock;
};

struct rvt2_fence_state {
    struct dma_fence base;
    struct list_head node;
    u64 seqno;
    u32 hw_status;
    bool completed;
    bool consumed;      /* set by wait, enables GC */
};

struct rvt2_device {
    struct pci_dev *pdev;
    void __iomem *mmio;
    void __iomem *hdm_io;              /* BAR2: HDM window */
    resource_size_t hdm_phys;          /* BAR2 physical address */
    resource_size_t hdm_size;          /* BAR2 size */
    unsigned long hdm_npages;          /* total pages in HDM window */
    unsigned long *hdm_bitmap;         /* page allocation bitmap */
    struct miscdevice miscdev;
    struct device *class_dev;

    /* BO management */
    struct idr bo_idr;
    struct mutex bo_lock;
    struct mutex submit_lock;

    /* Command queue (host-allocated DMA) */
    void *cmdq_cpu;
    dma_addr_t cmdq_dma;
    u32 cmdq_tail;

    /* Completion queue */
    void *cplq_cpu;
    dma_addr_t cplq_dma;
    u32 cplq_head;

    /* Fence tracking */
    u64 next_seqno;
    u64 last_completed_seqno;
    int irq_vecs;
    atomic_t unread_completions;
    u64 fence_context;
    spinlock_t fence_lock;
    wait_queue_head_t fence_wq;
    struct list_head fences;

    /* Firmware state */
    bool fw_ready;
    u32 engine_count;
    u32 fw_version;
    struct rvt2_gsp_info gsp;
};

/* Register access helpers */
static inline u32 rvt2_read(struct rvt2_device *rdev, u32 offset)
{
    return readl(rdev->mmio + offset);
}

static inline void rvt2_write(struct rvt2_device *rdev, u32 offset, u32 val)
{
    writel(val, rdev->mmio + offset);
}

/* rvt2_bo.c */
int rvt2_bo_create_ioctl(struct rvt2_device *rdev, void __user *arg);
int rvt2_bo_info_ioctl(struct rvt2_device *rdev, void __user *arg);
int rvt2_bo_destroy_ioctl(struct rvt2_device *rdev, void __user *arg);
int rvt2_bo_mmap(struct rvt2_device *rdev, struct vm_area_struct *vma);
struct rvt2_bo *rvt2_bo_lookup(struct rvt2_device *rdev, u32 handle);
void rvt2_bo_put(struct rvt2_bo *bo);
void rvt2_bo_cleanup(struct rvt2_device *rdev);

/* rvt2_submit.c */
int rvt2_submit_ioctl(struct rvt2_device *rdev, void __user *arg);
int rvt2_submit_raw_ioctl(struct rvt2_device *rdev, void __user *arg);
int rvt2_wait_ioctl(struct rvt2_device *rdev, void __user *arg);
void rvt2_submit_init(struct rvt2_device *rdev);
bool rvt2_poll_ready(struct rvt2_device *rdev);
void rvt2_fences_cleanup(struct rvt2_device *rdev);

/* rvt2_irq.c */
int rvt2_irq_init(struct rvt2_device *rdev);
void rvt2_irq_fini(struct rvt2_device *rdev);

/* rvt2_sysfs.c */
int rvt2_sysfs_init(struct rvt2_device *rdev);
void rvt2_sysfs_fini(struct rvt2_device *rdev);

#endif /* _RVT2_DRV_H_ */
