// SPDX-License-Identifier: GPL-2.0
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/dma-fence.h>
#include "rvt2_drv.h"

/* ---- dma_fence ops ---- */

static const char *rvt2_fence_get_driver_name(struct dma_fence *fence)
{
    return "rvt2";
}

static const char *rvt2_fence_get_timeline_name(struct dma_fence *fence)
{
    return "rvt2_submit";
}

static void rvt2_fence_release(struct dma_fence *fence)
{
    struct rvt2_fence_state *fs = container_of(fence, struct rvt2_fence_state, base);
    kfree(fs);
}

static const struct dma_fence_ops rvt2_fence_ops = {
    .get_driver_name = rvt2_fence_get_driver_name,
    .get_timeline_name = rvt2_fence_get_timeline_name,
    .release = rvt2_fence_release,
};

/* ---- Init ---- */

void rvt2_submit_init(struct rvt2_device *rdev)
{
    rdev->next_seqno = 1;
    rdev->last_completed_seqno = 0;
    atomic_set(&rdev->unread_completions, 0);
    rdev->fence_context = dma_fence_context_alloc(1);
    INIT_LIST_HEAD(&rdev->fences);
}

/* ---- Fence helpers ---- */

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

static void rvt2_gc_fences_locked(struct rvt2_device *rdev)
{
    struct rvt2_fence_state *fence, *tmp;

    list_for_each_entry_safe(fence, tmp, &rdev->fences, node) {
        if (fence->consumed) {
            list_del(&fence->node);
            dma_fence_put(&fence->base);
        }
    }
}

static void rvt2_complete_fence(struct rvt2_device *rdev, u64 seqno, u32 status)
{
    struct rvt2_fence_state *fence;

    spin_lock(&rdev->fence_lock);
    fence = rvt2_find_fence_locked(rdev, seqno);
    if (fence && !fence->completed) {
        fence->hw_status = status;
        fence->completed = true;
        dma_fence_signal_locked(&fence->base);
        atomic_inc(&rdev->unread_completions);
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
    return atomic_read(&rdev->unread_completions) > 0;
}

void rvt2_fences_cleanup(struct rvt2_device *rdev)
{
    struct rvt2_fence_state *fence, *tmp;

    spin_lock(&rdev->fence_lock);
    list_for_each_entry_safe(fence, tmp, &rdev->fences, node) {
        list_del(&fence->node);
        if (!fence->completed)
            dma_fence_signal_locked(&fence->base);
        dma_fence_put(&fence->base);
    }
    spin_unlock(&rdev->fence_lock);
}

/* ---- Submit ioctl ---- */

int rvt2_submit_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_submit req;
    struct rvt2_bo *bo_a, *bo_b, *bo_c, *bo_d;
    struct rvt2_fence_state *fence;
    struct rvt2_descriptor *desc;
    u64 seqno;
    u32 head, next_tail, status;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    status = rvt2_read(rdev, RVT2_REG_STATUS);
    if (status & RVT2_STATUS_ERROR)
        return -EIO;
    if (!rdev->gsp.ready || !rdev->gsp.heartbeat_alive)
        return -EIO;

    bo_a = rvt2_bo_lookup(rdev, req.bo_a);
    if (!bo_a) return -EINVAL;
    bo_b = rvt2_bo_lookup(rdev, req.bo_b);
    if (!bo_b) { rvt2_bo_put(bo_a); return -EINVAL; }
    bo_c = rvt2_bo_lookup(rdev, req.bo_c);
    if (!bo_c) { rvt2_bo_put(bo_a); rvt2_bo_put(bo_b); return -EINVAL; }
    bo_d = rvt2_bo_lookup(rdev, req.bo_d);
    if (!bo_d) { rvt2_bo_put(bo_a); rvt2_bo_put(bo_b); rvt2_bo_put(bo_c); return -EINVAL; }

    fence = kzalloc(sizeof(*fence), GFP_KERNEL);
    if (!fence) {
        rvt2_bo_put(bo_a); rvt2_bo_put(bo_b);
        rvt2_bo_put(bo_c); rvt2_bo_put(bo_d);
        return -ENOMEM;
    }

    mutex_lock(&rdev->submit_lock);

    spin_lock(&rdev->fence_lock);
    rvt2_gc_fences_locked(rdev);
    spin_unlock(&rdev->fence_lock);

    head = rvt2_read(rdev, RVT2_REG_CMDQ_HEAD) % RVT2_CMDQ_ENTRIES;
    next_tail = (rdev->cmdq_tail + 1) % RVT2_CMDQ_ENTRIES;
    if (next_tail == head) {
        mutex_unlock(&rdev->submit_lock);
        rvt2_bo_put(bo_a); rvt2_bo_put(bo_b);
        rvt2_bo_put(bo_c); rvt2_bo_put(bo_d);
        kfree(fence);
        return -EBUSY;
    }

    spin_lock(&rdev->fence_lock);
    seqno = rdev->next_seqno++;
    fence->seqno = seqno;
    dma_fence_init(&fence->base, &rvt2_fence_ops, &rdev->fence_lock,
                   rdev->fence_context, seqno);
    list_add_tail(&fence->node, &rdev->fences);
    spin_unlock(&rdev->fence_lock);

    desc = rdev->cmdq_cpu + (rdev->cmdq_tail % RVT2_CMDQ_ENTRIES) * RVT2_DESC_SIZE;
    memset(desc, 0, RVT2_DESC_SIZE);
    desc->opcode = RVT2_OP_TERNARY_MATMUL;
    desc->input_a_addr = bo_a->dma_addr;
    desc->input_b_addr = bo_b->dma_addr;
    desc->input_c_addr = bo_c->dma_addr;
    desc->output_d_addr = bo_d->dma_addr;
    desc->m = req.m; desc->n = req.n; desc->k = req.k;
    desc->dtype = req.dtype;
    desc->fence_seqno = seqno;

    rdev->cmdq_tail = next_tail;
    rvt2_write(rdev, RVT2_REG_CMDQ_TAIL, rdev->cmdq_tail);
    wmb();
    rvt2_write(rdev, RVT2_REG_DOORBELL, 1);
    mutex_unlock(&rdev->submit_lock);

    rvt2_bo_put(bo_a); rvt2_bo_put(bo_b);
    rvt2_bo_put(bo_c); rvt2_bo_put(bo_d);

    req.fence_seqno = seqno;
    if (copy_to_user(arg, &req, sizeof(req))) {
        spin_lock(&rdev->fence_lock);
        list_del(&fence->node);
        spin_unlock(&rdev->fence_lock);
        dma_fence_put(&fence->base);
        return -EFAULT;
    }

    return 0;
}

/* ---- Wait ioctl ---- */

int rvt2_wait_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_wait req;
    struct rvt2_fence_state *fence;
    long timeout_jiffies, ret;
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
        if (!fence->consumed) {
            fence->consumed = true;
            atomic_dec(&rdev->unread_completions);
        }
        rvt2_gc_fences_locked(rdev);
        spin_unlock(&rdev->fence_lock);
        req.status = hw_status ? 2 : 0;
        goto out;
    }
    spin_unlock(&rdev->fence_lock);

    if (req.timeout_ns == 0) {
        req.status = 1;
        goto out;
    }

    timeout_jiffies = (req.timeout_ns < 0) ? MAX_SCHEDULE_TIMEOUT
                                            : nsecs_to_jiffies(req.timeout_ns);

    if (rdev->irq_vecs <= 0) {
        unsigned long deadline = 0;

        if (req.timeout_ns >= 0)
            deadline = jiffies + timeout_jiffies;

        for (;;) {
            rvt2_harvest_completions(rdev);

            spin_lock(&rdev->fence_lock);
            fence = rvt2_find_fence_locked(rdev, req.fence_seqno);
            if (fence && fence->completed) {
                hw_status = fence->hw_status;
                if (!fence->consumed) {
                    fence->consumed = true;
                    atomic_dec(&rdev->unread_completions);
                }
                rvt2_gc_fences_locked(rdev);
                spin_unlock(&rdev->fence_lock);
                req.status = hw_status ? 2 : 0;
                goto out;
            }
            spin_unlock(&rdev->fence_lock);

            if (req.timeout_ns >= 0 && time_after_eq(jiffies, deadline)) {
                req.status = 1;
                goto out;
            }
            if (signal_pending(current))
                return -ERESTARTSYS;

            schedule_timeout_interruptible(msecs_to_jiffies(1));
        }
    }

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
        req.status = 1;
    } else if (ret < 0) {
        return -ERESTARTSYS;
    } else {
        spin_lock(&rdev->fence_lock);
        fence = rvt2_find_fence_locked(rdev, req.fence_seqno);
        if (fence) {
            hw_status = fence->hw_status;
            if (!fence->consumed) {
                fence->consumed = true;
                atomic_dec(&rdev->unread_completions);
            }
        }
        rvt2_gc_fences_locked(rdev);
        spin_unlock(&rdev->fence_lock);
        req.status = hw_status ? 2 : 0;
    }

out:
    if (copy_to_user(arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;
}

/* ---- Raw descriptor submit ioctl (for compiledd pipeline) ---- */

int rvt2_submit_raw_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_submit_raw req;
    struct rvt2_descriptor *descs = NULL;
    struct rvt2_fence_state **fences = NULL;
    struct rvt2_descriptor *slot;
    u64 seqno = 0;
    u32 head, avail, status, i, tail_save;
    void __user *blob;
    int ret = 0;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;
    if (req.desc_count == 0 || req.desc_count > RVT2_CMDQ_ENTRIES)
        return -EINVAL;

    status = rvt2_read(rdev, RVT2_REG_STATUS);
    if (status & RVT2_STATUS_ERROR)
        return -EIO;
    if (!rdev->gsp.ready || !rdev->gsp.heartbeat_alive)
        return -EIO;

    blob = (void __user *)(unsigned long)req.desc_addr;

    /* Phase 1: Copy all descriptors from userspace */
    descs = kmalloc_array(req.desc_count, RVT2_DESC_SIZE, GFP_KERNEL);
    if (!descs)
        return -ENOMEM;
    if (copy_from_user(descs, blob, req.desc_count * RVT2_DESC_SIZE)) {
        ret = -EFAULT;
        goto err_free_descs;
    }

    /* Phase 2: Pre-allocate all fences (kcalloc ensures NULL init) */
    fences = kcalloc(req.desc_count, sizeof(*fences), GFP_KERNEL);
    if (!fences) {
        ret = -ENOMEM;
        goto err_free_descs;
    }
    for (i = 0; i < req.desc_count; i++) {
        fences[i] = kzalloc(sizeof(*fences[i]), GFP_KERNEL);
        if (!fences[i]) {
            ret = -ENOMEM;
            goto err_free_fences;
        }
    }

    mutex_lock(&rdev->submit_lock);

    /* Phase 3: Check ring has enough space for entire chain */
    spin_lock(&rdev->fence_lock);
    rvt2_gc_fences_locked(rdev);
    spin_unlock(&rdev->fence_lock);

    head = rvt2_read(rdev, RVT2_REG_CMDQ_HEAD) % RVT2_CMDQ_ENTRIES;
    if (head <= rdev->cmdq_tail)
        avail = RVT2_CMDQ_ENTRIES - 1 - rdev->cmdq_tail + head;
    else
        avail = head - rdev->cmdq_tail - 1;

    if (req.desc_count > avail) {
        mutex_unlock(&rdev->submit_lock);
        ret = -EBUSY;
        goto err_free_fences;
    }

    /* Phase 4: All checks passed — batch enqueue (no failure possible) */
    tail_save = rdev->cmdq_tail;
    spin_lock(&rdev->fence_lock);
    for (i = 0; i < req.desc_count; i++) {
        seqno = rdev->next_seqno++;
        fences[i]->seqno = seqno;
        dma_fence_init(&fences[i]->base, &rvt2_fence_ops, &rdev->fence_lock,
                       rdev->fence_context, seqno);
        list_add_tail(&fences[i]->node, &rdev->fences);

        slot = rdev->cmdq_cpu + (rdev->cmdq_tail % RVT2_CMDQ_ENTRIES) * RVT2_DESC_SIZE;
        memcpy(slot, &descs[i], RVT2_DESC_SIZE);
        slot->fence_seqno = seqno;
        rdev->cmdq_tail = (rdev->cmdq_tail + 1) % RVT2_CMDQ_ENTRIES;
    }
    spin_unlock(&rdev->fence_lock);

    rvt2_write(rdev, RVT2_REG_CMDQ_TAIL, rdev->cmdq_tail);
    wmb();
    rvt2_write(rdev, RVT2_REG_DOORBELL, 1);
    mutex_unlock(&rdev->submit_lock);

    kfree(fences);
    kfree(descs);

    req.fence_seqno = seqno;
    if (copy_to_user(arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;

err_free_fences:
    for (i = 0; i < req.desc_count; i++)
        kfree(fences[i]);
    kfree(fences);
err_free_descs:
    kfree(descs);
    return ret;
}
