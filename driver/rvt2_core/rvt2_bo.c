// SPDX-License-Identifier: GPL-2.0
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "rvt2_drv.h"

static void rvt2_bo_release(struct kref *ref)
{
    struct rvt2_bo *bo = container_of(ref, struct rvt2_bo, ref);
    struct device *dev = &bo->rdev->pdev->dev;

    dma_free_coherent(dev, bo->size, bo->cpu_addr, bo->dma_addr);
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
    if (bo)
        kref_get(&bo->ref);
    mutex_unlock(&rdev->bo_lock);
    return bo;
}

int rvt2_bo_create_ioctl(struct rvt2_device *rdev, void __user *arg)
{
    struct rvt2_bo_create req;
    struct rvt2_bo *bo;
    int ret;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    if (req.size == 0)
        return -EINVAL;

    /* Round up to page size */
    req.size = PAGE_ALIGN(req.size);

    bo = kzalloc(sizeof(*bo), GFP_KERNEL);
    if (!bo)
        return -ENOMEM;

    kref_init(&bo->ref);
    bo->rdev = rdev;
    bo->size = req.size;

    bo->cpu_addr = dma_alloc_coherent(&rdev->pdev->dev, bo->size,
                                      &bo->dma_addr, GFP_KERNEL);
    if (!bo->cpu_addr) {
        kfree(bo);
        return -ENOMEM;
    }

    mutex_lock(&rdev->bo_lock);
    ret = idr_alloc(&rdev->bo_idr, bo, 1, 0, GFP_KERNEL);
    mutex_unlock(&rdev->bo_lock);
    if (ret < 0) {
        dma_free_coherent(&rdev->pdev->dev, bo->size,
                          bo->cpu_addr, bo->dma_addr);
        kfree(bo);
        return ret;
    }
    bo->handle = ret;

    req.handle = bo->handle;
    req.dma_addr = bo->dma_addr;
    if (copy_to_user(arg, &req, sizeof(req)))
        return -EFAULT;

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

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;

    mutex_lock(&rdev->bo_lock);
    bo = idr_find(&rdev->bo_idr, req.handle);
    if (!bo) {
        mutex_unlock(&rdev->bo_lock);
        return -ENOENT;
    }
    idr_remove(&rdev->bo_idr, req.handle);
    mutex_unlock(&rdev->bo_lock);

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

    if (vma->vm_end - vma->vm_start > bo->size) {
        rvt2_bo_put(bo);
        return -EINVAL;
    }

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    ret = dma_mmap_coherent(&rdev->pdev->dev, vma,
                            bo->cpu_addr, bo->dma_addr, bo->size);
    rvt2_bo_put(bo);
    return ret;
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
