// SPDX-License-Identifier: GPL-2.0
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include "rvt2_drv.h"

void rvt2_submit_init(struct rvt2_device *rdev)
{
    rdev->next_seqno = 1;
    rdev->last_completed_seqno = 0;
}

static void rvt2_harvest_completions(struct rvt2_device *rdev)
{
    u32 tail = rvt2_read(rdev, RVT2_REG_CPLQ_TAIL);

    while (rdev->cplq_head != tail) {
        struct {
            u64 fence_seqno;
            u32 status;
            u32 reserved;
        } *cpl;

        cpl = rdev->cplq_cpu + (rdev->cplq_head % RVT2_CPLQ_ENTRIES) * RVT2_CPL_SIZE;

        spin_lock(&rdev->fence_lock);
        if (cpl->fence_seqno > rdev->last_completed_seqno)
            rdev->last_completed_seqno = cpl->fence_seqno;
        spin_unlock(&rdev->fence_lock);

        rdev->cplq_head = (rdev->cplq_head + 1) % RVT2_CPLQ_ENTRIES;
    }

    rvt2_write(rdev, RVT2_REG_CPLQ_HEAD, rdev->cplq_head);
    wake_up_all(&rdev->fence_wq);
}

int rvt2_submit_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_submit req;
    struct rvt2_bo *bo_a, *bo_b, *bo_c, *bo_d;
    struct rvt2_descriptor *desc;
    u64 seqno;
    u32 status;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    /* Validate device state */
    status = rvt2_read(rdev, RVT2_REG_STATUS);
    if (status & RVT2_STATUS_ERROR)
        return -EIO;

    bo_a = rvt2_bo_lookup(rdev, req.bo_a);
    if (!bo_a)
        return -EINVAL;
    bo_b = rvt2_bo_lookup(rdev, req.bo_b);
    if (!bo_b) {
        rvt2_bo_put(bo_a);
        return -EINVAL;
    }
    bo_c = rvt2_bo_lookup(rdev, req.bo_c);
    if (!bo_c) {
        rvt2_bo_put(bo_a);
        rvt2_bo_put(bo_b);
        return -EINVAL;
    }
    bo_d = rvt2_bo_lookup(rdev, req.bo_d);
    if (!bo_d) {
        rvt2_bo_put(bo_a);
        rvt2_bo_put(bo_b);
        rvt2_bo_put(bo_c);
        return -EINVAL;
    }

    /* Assign fence seqno */
    spin_lock(&rdev->fence_lock);
    seqno = rdev->next_seqno++;
    spin_unlock(&rdev->fence_lock);

    /* Write descriptor to command queue */
    desc = rdev->cmdq_cpu + (rdev->cmdq_tail % RVT2_CMDQ_ENTRIES) * RVT2_DESC_SIZE;
    memset(desc, 0, RVT2_DESC_SIZE);
    desc->opcode = RVT2_OP_TERNARY_MATMUL;
    desc->input_a_addr = bo_a->dma_addr;
    desc->input_b_addr = bo_b->dma_addr;
    desc->input_c_addr = bo_c->dma_addr;
    desc->output_d_addr = bo_d->dma_addr;
    desc->m = req.m;
    desc->n = req.n;
    desc->k = req.k;
    desc->dtype = req.dtype;
    desc->fence_seqno = seqno;

    /* Advance tail and ring doorbell */
    rdev->cmdq_tail = (rdev->cmdq_tail + 1) % RVT2_CMDQ_ENTRIES;
    rvt2_write(rdev, RVT2_REG_CMDQ_TAIL, rdev->cmdq_tail);
    wmb();
    rvt2_write(rdev, RVT2_REG_DOORBELL, 1);

    rvt2_bo_put(bo_a);
    rvt2_bo_put(bo_b);
    rvt2_bo_put(bo_c);
    rvt2_bo_put(bo_d);

    req.fence_seqno = seqno;
    if (copy_to_user(arg, &req, sizeof(req)))
        return -EFAULT;

    return 0;
}

int rvt2_wait_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_wait req;
    long timeout_jiffies;
    long ret;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    if (req.timeout_ns < -1)
        return -EINVAL;

    /* Harvest any pending completions first */
    rvt2_harvest_completions(rdev);

    if (rdev->last_completed_seqno >= req.fence_seqno) {
        req.status = 0;
        goto out;
    }

    if (req.timeout_ns == 0) {
        req.status = 1; /* timeout */
        goto out;
    }

    if (req.timeout_ns < 0)
        timeout_jiffies = MAX_SCHEDULE_TIMEOUT;
    else
        timeout_jiffies = nsecs_to_jiffies(req.timeout_ns);

    ret = wait_event_interruptible_timeout(rdev->fence_wq,
        ({
            rvt2_harvest_completions(rdev);
            rdev->last_completed_seqno >= req.fence_seqno;
        }),
        timeout_jiffies);

    if (ret == 0) {
        req.status = 1; /* timeout */
    } else if (ret < 0) {
        return -ERESTARTSYS;
    } else {
        req.status = 0; /* signaled */
    }

out:
    if (copy_to_user(arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;
}
