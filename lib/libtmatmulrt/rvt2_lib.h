/* SPDX-License-Identifier: MIT */
#ifndef _RVT2_LIB_H_
#define _RVT2_LIB_H_

#include <stddef.h>
#include <stdint.h>

typedef struct rvt2_dev {
    int fd;
} rvt2_dev_t;

typedef struct rvt2_bo {
    uint32_t handle;
    uint64_t size;
    uint64_t dma_addr;
    void *map;
} rvt2_bo_t;

int rvt2_open(rvt2_dev_t *dev);
void rvt2_close(rvt2_dev_t *dev);

int rvt2_bo_alloc(rvt2_dev_t *dev, size_t size, uint32_t flags, rvt2_bo_t *bo);
void *rvt2_bo_map(rvt2_dev_t *dev, rvt2_bo_t *bo);
void rvt2_bo_unmap(rvt2_bo_t *bo);
void rvt2_bo_free(rvt2_dev_t *dev, rvt2_bo_t *bo);

int rvt2_submit(rvt2_dev_t *dev, uint32_t bo_a, uint32_t bo_b,
                uint32_t bo_c, uint32_t bo_d,
                uint32_t m, uint32_t n, uint32_t k,
                uint32_t dtype, uint64_t *fence_seqno);

int rvt2_wait(rvt2_dev_t *dev, uint64_t fence_seqno, int64_t timeout_ns);

#endif /* _RVT2_LIB_H_ */
