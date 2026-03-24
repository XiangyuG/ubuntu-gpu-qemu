// SPDX-License-Identifier: GPL-2.0
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include "rvt2_drv.h"

void rvt2_submit_init(struct rvt2_device *rdev)
{
    rdev->next_seqno = 1;
    rdev->last_completed_seqno = 0;
    rdev->new_completions = false;
    INIT_LIST_HEAD(&rdev->fences);
}

static struct rvt2_fence_state *rvt2_find_fence_locked(struct rvt2_device *rdev,
                                                       u64 seqno)
{
    struct rvt2_fence_state *fence;

    list_for_each_entry(fence, &rdev->fences, node) {
        if (fence->seqno == seqno)
            return fence;
    }

    return NULL;
}

/* Remove and free a single fence under fence_lock */
static void rvt2_reclaim_fence_locked(struct rvt2_fence_state *fence)
{
    list_del(&fence->node);
    kfree(fence);
}

/* Garbage collect all completed and consumed fences */
static void rvt2_gc_fences_locked(struct rvt2_device *rdev)
{
    struct rvt2_fence_state *fence, *tmp;

    list_for_each_entry_safe(fence, tmp, &rdev->fences, node) {
        if (fence->completed && fence->consumed)
            rvt2_reclaim_fence_locked(fence);
    }
}

static void rvt2_complete_fence(struct rvt2_device *rdev, u64 seqno, u32 status)
{
    struct rvt2_fence_state *fence;

    spin_lock(&rdev->fence_lock);
    fence = rvt2_find_fence_locked(rdev, seqno);
    if (fence) {
        fence->hw_status = status;
        fence->completed = true;
        rdev->new_completions = true;
    }
    if (status == 0 && seqno > rdev->last_completed_seqno)
        rdev->last_completed_seqno = seqno;
    spin_unlock(&rdev->fence_lock);
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
        rvt2_complete_fence(rdev, cpl->fence_seqno, cpl->status);

        rdev->cplq_head = (rdev->cplq_head + 1) % RVT2_CPLQ_ENTRIES;
    }

    rvt2_write(rdev, RVT2_REG_CPLQ_HEAD, rdev->cplq_head);
    wake_up_all(&rdev->fence_wq);
}

bool rvt2_poll_ready(struct rvt2_device *rdev)
{
    rvt2_harvest_completions(rdev);

    /* Only report ready when there are NEW (unconsumed) completions */
    return rdev->new_completions;
}

/* Clean up all fences (called from remove path) */
void rvt2_fences_cleanup(struct rvt2_device *rdev)
{
    struct rvt2_fence_state *fence, *tmp;

    spin_lock(&rdev->fence_lock);
    list_for_each_entry_safe(fence, tmp, &rdev->fences, node) {
        list_del(&fence->node);
        kfree(fence);
    }
    spin_unlock(&rdev->fence_lock);
}

int rvt2_submit_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_submit req;
    struct rvt2_bo *bo_a, *bo_b, *bo_c, *bo_d;
    struct rvt2_fence_state *fence;
    struct rvt2_descriptor *desc;
    u64 seqno;
    u32 head;
    u32 next_tail;
    u32 status;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    /* Validate device state */
    status = rvt2_read(rdev, RVT2_REG_STATUS);
    if (status & RVT2_STATUS_ERROR)
        return -EIO;
    if (!rdev->gsp.ready || !rdev->gsp.heartbeat_alive)
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

    fence = kzalloc(sizeof(*fence), GFP_KERNEL);
    if (!fence) {
        rvt2_bo_put(bo_a);
        rvt2_bo_put(bo_b);
        rvt2_bo_put(bo_c);
        rvt2_bo_put(bo_d);
        return -ENOMEM;
    }

    mutex_lock(&rdev->submit_lock);

    /* GC old consumed fences before adding new ones */
    spin_lock(&rdev->fence_lock);
    rvt2_gc_fences_locked(rdev);
    spin_unlock(&rdev->fence_lock);

    head = rvt2_read(rdev, RVT2_REG_CMDQ_HEAD) % RVT2_CMDQ_ENTRIES;
    next_tail = (rdev->cmdq_tail + 1) % RVT2_CMDQ_ENTRIES;
    if (next_tail == head) {
        mutex_unlock(&rdev->submit_lock);
        rvt2_bo_put(bo_a);
        rvt2_bo_put(bo_b);
        rvt2_bo_put(bo_c);
        rvt2_bo_put(bo_d);
        kfree(fence);
        return -EBUSY;
    }

    spin_lock(&rdev->fence_lock);
    seqno = rdev->next_seqno++;
    fence->seqno = seqno;
    list_add_tail(&fence->node, &rdev->fences);
    spin_unlock(&rdev->fence_lock);

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

    rdev->cmdq_tail = next_tail;
    rvt2_write(rdev, RVT2_REG_CMDQ_TAIL, rdev->cmdq_tail);
    wmb();
    rvt2_write(rdev, RVT2_REG_DOORBELL, 1);
    mutex_unlock(&rdev->submit_lock);

    rvt2_bo_put(bo_a);
    rvt2_bo_put(bo_b);
    rvt2_bo_put(bo_c);
    rvt2_bo_put(bo_d);

    req.fence_seqno = seqno;
    if (copy_to_user(arg, &req, sizeof(req))) {
        spin_lock(&rdev->fence_lock);
        list_del(&fence->node);
        spin_unlock(&rdev->fence_lock);
        kfree(fence);
        return -EFAULT;
    }

    return 0;
}

int rvt2_wait_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_wait req;
    struct rvt2_fence_state *fence;
    long timeout_jiffies;
    long ret;
    u32 hw_status = 0;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    if (req.timeout_ns < -1)
        return -EINVAL;

    rvt2_harvest_completions(rdev);

    spin_lock(&rdev->fence_lock);
    fence = rvt2_find_fence_locked(rdev, req.fence_seqno);
    if (fence && fence->completed) {
        hw_status = fence->hw_status;
        fence->consumed = true;
        /* Clear new_completions if no more unconsumed fences */
        rdev->new_completions = false;
        spin_unlock(&rdev->fence_lock);
        req.status = hw_status ? 2 : 0;
        goto out;
    }
    spin_unlock(&rdev->fence_lock);

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
            bool done;
            spin_lock(&rdev->fence_lock);
            fence = rvt2_find_fence_locked(rdev, req.fence_seqno);
            done = fence && fence->completed;
            spin_unlock(&rdev->fence_lock);
            done;
        }),
        timeout_jiffies);

    if (ret == 0) {
        req.status = 1; /* timeout */
    } else if (ret < 0) {
        return -ERESTARTSYS;
    } else {
        spin_lock(&rdev->fence_lock);
        fence = rvt2_find_fence_locked(rdev, req.fence_seqno);
        if (fence) {
            hw_status = fence->hw_status;
            fence->consumed = true;
            rdev->new_completions = false;
        }
        spin_unlock(&rdev->fence_lock);
        req.status = hw_status ? 2 : 0;
    }

out:
    if (copy_to_user(arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;
}
