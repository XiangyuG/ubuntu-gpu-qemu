// SPDX-License-Identifier: MIT
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include "rvt2_lib.h"
#include "../../include/uapi/rvt2_drm.h"

int rvt2_open(rvt2_dev_t *dev)
{
    if (!dev)
        return -EINVAL;

    dev->fd = open("/dev/rvt2_0", O_RDWR);
    if (dev->fd < 0)
        return -errno;

    return 0;
}

void rvt2_close(rvt2_dev_t *dev)
{
    if (dev && dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

int rvt2_bo_alloc(rvt2_dev_t *dev, size_t size, uint32_t flags, rvt2_bo_t *bo)
{
    struct rvt2_bo_create req = { 0 };
    int ret;

    if (!dev || !bo || size == 0)
        return -EINVAL;

    req.size = size;
    req.flags = flags;

    ret = ioctl(dev->fd, RVT2_IOCTL_BO_CREATE, &req);
    if (ret)
        return -errno;

    bo->handle = req.handle;
    bo->size = req.size;
    bo->dma_addr = req.dma_addr;
    bo->map = NULL;
    return 0;
}

void *rvt2_bo_map(rvt2_dev_t *dev, rvt2_bo_t *bo)
{
    if (!dev || !bo || bo->map)
        return bo ? bo->map : NULL;

    bo->map = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   dev->fd, (off_t)bo->handle << 12);
    if (bo->map == MAP_FAILED) {
        bo->map = NULL;
        return NULL;
    }
    return bo->map;
}

void rvt2_bo_unmap(rvt2_bo_t *bo)
{
    if (bo && bo->map) {
        munmap(bo->map, bo->size);
        bo->map = NULL;
    }
}

void rvt2_bo_free(rvt2_dev_t *dev, rvt2_bo_t *bo)
{
    struct rvt2_bo_destroy req = { 0 };

    if (!dev || !bo)
        return;

    rvt2_bo_unmap(bo);
    req.handle = bo->handle;
    ioctl(dev->fd, RVT2_IOCTL_BO_DESTROY, &req);
    memset(bo, 0, sizeof(*bo));
}

int rvt2_submit(rvt2_dev_t *dev, uint32_t bo_a, uint32_t bo_b,
                uint32_t bo_c, uint32_t bo_d,
                uint32_t m, uint32_t n, uint32_t k,
                uint32_t dtype, uint64_t *fence_seqno)
{
    struct rvt2_submit req = { 0 };
    int ret;

    if (!dev || !fence_seqno)
        return -EINVAL;

    req.bo_a = bo_a;
    req.bo_b = bo_b;
    req.bo_c = bo_c;
    req.bo_d = bo_d;
    req.m = m;
    req.n = n;
    req.k = k;
    req.dtype = dtype;

    ret = ioctl(dev->fd, RVT2_IOCTL_SUBMIT, &req);
    if (ret)
        return -errno;

    *fence_seqno = req.fence_seqno;
    return 0;
}

int rvt2_wait(rvt2_dev_t *dev, uint64_t fence_seqno, int64_t timeout_ns)
{
    struct rvt2_wait req = { 0 };
    int ret;

    if (!dev)
        return -EINVAL;

    req.fence_seqno = fence_seqno;
    req.timeout_ns = timeout_ns;

    ret = ioctl(dev->fd, RVT2_IOCTL_WAIT, &req);
    if (ret)
        return -errno;

    if (req.status == 1)
        return -ETIMEDOUT;
    if (req.status == 2)
        return -EIO;

    return 0;
}
