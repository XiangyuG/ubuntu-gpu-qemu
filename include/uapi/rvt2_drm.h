/* SPDX-License-Identifier: MIT */
/*
 * RVT2 Ternary MatMul Accelerator - User-Kernel Interface
 *
 * Shared definitions for ioctl commands, descriptor format, and data
 * structures used by both the kernel driver and userspace runtime.
 */

#ifndef _UAPI_RVT2_DRM_H_
#define _UAPI_RVT2_DRM_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define RVT2_IOCTL_BASE 'R'

/* BO allocation flags */
#define RVT2_BO_FLAG_HDM        (1 << 0)    /* Allocate from CXL HDM window */

/* ---- Buffer Object (BO) ioctls ---- */

struct rvt2_bo_create {
    __u64 size;         /* in: requested size in bytes */
    __u32 flags;        /* in: allocation flags */
    __u32 handle;       /* out: BO handle */
    __u64 dma_addr;     /* out: device DMA address */
};

struct rvt2_bo_info {
    __u32 handle;       /* in: BO handle */
    __u32 pad;
    __u64 size;         /* out: actual size */
    __u64 dma_addr;     /* out: device DMA address */
    __u64 offset;       /* out: mmap offset */
};

struct rvt2_bo_destroy {
    __u32 handle;       /* in: BO handle to free */
    __u32 pad;
};

#define RVT2_IOCTL_BO_CREATE    _IOWR(RVT2_IOCTL_BASE, 0x00, struct rvt2_bo_create)
#define RVT2_IOCTL_BO_INFO      _IOWR(RVT2_IOCTL_BASE, 0x01, struct rvt2_bo_info)
#define RVT2_IOCTL_BO_DESTROY   _IOW(RVT2_IOCTL_BASE, 0x02, struct rvt2_bo_destroy)

/* ---- Descriptor format (matches QEMU device model) ---- */

#define RVT2_OP_TERNARY_MATMUL  0x01

#define RVT2_DTYPE_FLOAT32      0x00
#define RVT2_DTYPE_FLOAT16      0x01
#define RVT2_DTYPE_INT8         0x02

struct rvt2_descriptor {
    __u32 opcode;
    __u32 flags;
    __u64 input_a_addr;
    __u64 input_b_addr;
    __u64 input_c_addr;
    __u64 output_d_addr;
    __u32 m, n, k;
    __u32 dtype;
    __u64 fence_seqno;
};

/* ---- Submit / Wait ioctls ---- */

struct rvt2_submit {
    __u32 bo_a;         /* in: BO handle for matrix A */
    __u32 bo_b;         /* in: BO handle for matrix B */
    __u32 bo_c;         /* in: BO handle for matrix C */
    __u32 bo_d;         /* in: BO handle for result D */
    __u32 m, n, k;      /* in: matrix dimensions */
    __u32 dtype;        /* in: data type */
    __u64 fence_seqno;  /* out: assigned fence sequence number */
};

struct rvt2_wait {
    __u64 fence_seqno;      /* in: fence to wait for */
    __s64 timeout_ns;       /* in: timeout in nanoseconds (-1 = infinite) */
    __u32 status;           /* out: 0 = signaled, 1 = timeout, 2 = device fault */
    __u32 pad;
};

#define RVT2_IOCTL_SUBMIT   _IOWR(RVT2_IOCTL_BASE, 0x10, struct rvt2_submit)
#define RVT2_IOCTL_WAIT     _IOWR(RVT2_IOCTL_BASE, 0x11, struct rvt2_wait)

#endif /* _UAPI_RVT2_DRM_H_ */
