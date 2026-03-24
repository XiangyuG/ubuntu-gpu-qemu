// SPDX-License-Identifier: GPL-2.0
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include "rvt2_drv.h"

struct rvt2_vma_node {
    struct list_head node;
    struct vm_area_struct *vma;
};

static void rvt2_bo_release(struct kref *ref)
{
    struct rvt2_bo *bo = container_of(ref, struct rvt2_bo, ref);

    if (bo->is_hdm) {
        clear_bit(bo->hdm_page_idx, &bo->rdev->hdm_bitmap);
    } else {
        dma_free_coherent(&bo->rdev->pdev->dev, bo->size,
                          bo->cpu_addr, bo->dma_addr);
    }
    mutex_destroy(&bo->vma_lock);
    kfree(bo);
}

void rvt2_bo_put(struct rvt2_bo *bo)
{
    kref_put(&bo->ref, rvt2_bo_release);
}

struct rvt2_bo *rvt2_bo_lookup(struct rvt2_device *rdev, u32 handle)
{
    struct rvt2_bo *bo;

    mutex_lock(&rdev->bo_lock);
    bo = idr_find(&rdev->bo_idr, handle);
    if (bo && !bo->destroyed)
        kref_get(&bo->ref);
    else
        bo = NULL;
    mutex_unlock(&rdev->bo_lock);
    return bo;
}

static vm_fault_t rvt2_bo_vm_fault(struct vm_fault *vmf)
{
    struct rvt2_bo *bo = vmf->vma->vm_private_data;

    if (!bo || bo->destroyed)
        return VM_FAULT_SIGBUS;

    return VM_FAULT_SIGBUS; /* should not reach here if pages are mapped */
}

static void rvt2_bo_vma_open(struct vm_area_struct *vma)
{
    struct rvt2_bo *bo = vma->vm_private_data;
    struct rvt2_vma_node *vn;

    if (!bo)
        return;

    kref_get(&bo->ref);

    vn = kzalloc(sizeof(*vn), GFP_KERNEL);
    if (vn) {
        vn->vma = vma;
        mutex_lock(&bo->vma_lock);
        list_add_tail(&vn->node, &bo->vma_list);
        mutex_unlock(&bo->vma_lock);
    }
}

static void rvt2_bo_vma_close(struct vm_area_struct *vma)
{
    struct rvt2_bo *bo = vma->vm_private_data;
    struct rvt2_vma_node *vn, *tmp;

    if (!bo)
        return;

    mutex_lock(&bo->vma_lock);
    list_for_each_entry_safe(vn, tmp, &bo->vma_list, node) {
        if (vn->vma == vma) {
            list_del(&vn->node);
            kfree(vn);
            break;
        }
    }
    mutex_unlock(&bo->vma_lock);

    rvt2_bo_put(bo);
}

static const struct vm_operations_struct rvt2_bo_vm_ops = {
    .open = rvt2_bo_vma_open,
    .close = rvt2_bo_vma_close,
    .fault = rvt2_bo_vm_fault,
};

int rvt2_bo_create_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_bo_create req;
    struct rvt2_bo *bo;
    int ret;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    if (req.size == 0)
        return -EINVAL;

    req.size = PAGE_ALIGN(req.size);

    bo = kzalloc(sizeof(*bo), GFP_KERNEL);
    if (!bo)
        return -ENOMEM;

    kref_init(&bo->ref);
    bo->rdev = rdev;
    bo->size = req.size;
    mutex_init(&bo->vma_lock);
    INIT_LIST_HEAD(&bo->vma_list);

    if (req.flags & RVT2_BO_FLAG_HDM) {
        /* Allocate from BAR2 HDM window */
        unsigned long pages_needed = req.size >> PAGE_SHIFT;
        unsigned long idx;

        if (!rdev->hdm_io || !rdev->hdm_size) {
            mutex_destroy(&bo->vma_lock);
            kfree(bo);
            return -ENODEV;
        }
        if (req.size > rdev->hdm_size) {
            mutex_destroy(&bo->vma_lock);
            kfree(bo);
            return -ENOMEM;
        }

        idx = find_first_zero_bit(&rdev->hdm_bitmap, BITS_PER_LONG);
        if (idx >= rdev->hdm_size >> PAGE_SHIFT) {
            mutex_destroy(&bo->vma_lock);
            kfree(bo);
            return -ENOMEM;
        }
        set_bit(idx, &rdev->hdm_bitmap);

        bo->is_hdm = true;
        bo->hdm_page_idx = idx;
        bo->cpu_addr = rdev->hdm_io + (idx << PAGE_SHIFT);
        bo->dma_addr = rdev->hdm_phys + (idx << PAGE_SHIFT);
        (void)pages_needed;
    } else {
        bo->cpu_addr = dma_alloc_coherent(&rdev->pdev->dev, bo->size,
                                          &bo->dma_addr, GFP_KERNEL);
        if (!bo->cpu_addr) {
            mutex_destroy(&bo->vma_lock);
            kfree(bo);
            return -ENOMEM;
        }
    }

    mutex_lock(&rdev->bo_lock);
    ret = idr_alloc(&rdev->bo_idr, bo, 1, 0, GFP_KERNEL);
    mutex_unlock(&rdev->bo_lock);
    if (ret < 0) {
        dma_free_coherent(&rdev->pdev->dev, bo->size,
                          bo->cpu_addr, bo->dma_addr);
        mutex_destroy(&bo->vma_lock);
        kfree(bo);
        return ret;
    }
    bo->handle = ret;

    req.handle = bo->handle;
    req.dma_addr = bo->dma_addr;
    if (copy_to_user(arg, &req, sizeof(req))) {
        mutex_lock(&rdev->bo_lock);
        idr_remove(&rdev->bo_idr, bo->handle);
        mutex_unlock(&rdev->bo_lock);
        rvt2_bo_put(bo);
        return -EFAULT;
    }

    return 0;
}

int rvt2_bo_info_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_bo_info req;
    struct rvt2_bo *bo;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    bo = rvt2_bo_lookup(rdev, req.handle);
    if (!bo)
        return -ENOENT;

    req.size = bo->size;
    req.dma_addr = bo->dma_addr;
    req.offset = (u64)bo->handle << PAGE_SHIFT;
    rvt2_bo_put(bo);

    if (copy_to_user(arg, &req, sizeof(req)))
        return -EFAULT;

    return 0;
}

int rvt2_bo_destroy_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_bo_destroy req;
    struct rvt2_bo *bo;
    struct rvt2_vma_node *vn;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    mutex_lock(&rdev->bo_lock);
    bo = idr_find(&rdev->bo_idr, req.handle);
    if (!bo) {
        mutex_unlock(&rdev->bo_lock);
        return -ENOENT;
    }
    idr_remove(&rdev->bo_idr, req.handle);
    bo->destroyed = true;
    mutex_unlock(&rdev->bo_lock);

    /*
     * Zap all existing mappings so subsequent access triggers SIGBUS
     * via the fault handler which checks bo->destroyed.
     */
    mutex_lock(&bo->vma_lock);
    list_for_each_entry(vn, &bo->vma_list, node) {
        zap_vma_ptes(vn->vma, vn->vma->vm_start,
                     vn->vma->vm_end - vn->vma->vm_start);
    }
    mutex_unlock(&bo->vma_lock);

    rvt2_bo_put(bo);
    return 0;
}

int rvt2_bo_mmap(struct rvt2_device *rdev, struct vm_area_struct *vma)
{
    u32 handle = vma->vm_pgoff;
    struct rvt2_bo *bo;
    int ret;

    bo = rvt2_bo_lookup(rdev, handle);
    if (!bo)
        return -ENOENT;
    if (bo->destroyed) {
        rvt2_bo_put(bo);
        return -ENODEV;
    }

    if (vma->vm_end - vma->vm_start > bo->size) {
        rvt2_bo_put(bo);
        return -EINVAL;
    }

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_IO);
    vma->vm_private_data = bo;
    vma->vm_ops = &rvt2_bo_vm_ops;
    vma->vm_pgoff = 0;

    if (bo->is_hdm) {
        ret = io_remap_pfn_range(vma, vma->vm_start,
                                 bo->dma_addr >> PAGE_SHIFT,
                                 vma->vm_end - vma->vm_start,
                                 vma->vm_page_prot);
    } else {
        ret = dma_mmap_coherent(&rdev->pdev->dev, vma,
                                bo->cpu_addr, bo->dma_addr, bo->size);
    }
    if (ret) {
        vma->vm_private_data = NULL;
        vma->vm_ops = NULL;
        rvt2_bo_put(bo);
        return ret;
    }

    rvt2_bo_vma_open(vma);
    rvt2_bo_put(bo);
    return 0;
}

static int rvt2_bo_destroy_cb(int id, void *ptr, void *data)
{
    struct rvt2_bo *bo = ptr;
    rvt2_bo_put(bo);
    return 0;
}

void rvt2_bo_cleanup(struct rvt2_device *rdev)
{
    mutex_lock(&rdev->bo_lock);
    idr_for_each(&rdev->bo_idr, rvt2_bo_destroy_cb, NULL);
    idr_destroy(&rdev->bo_idr);
    mutex_unlock(&rdev->bo_lock);
}
