// SPDX-License-Identifier: MIT
/*
 * RVT2 compiledd end-to-end test (AC-7)
 *
 * Allocates BOs, generates IR with real DMA addresses, runs compiledd
 * to produce a descriptor blob, then submits that blob through the
 * raw descriptor ioctl and verifies the result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../lib/libtmatmulrt/rvt2_lib.h"
#include "../../include/uapi/rvt2_drm.h"

#define M 4
#define N 4
#define K 4
#define EPSILON 1e-4f

static void ref_matmul(const float *a, const float *b, const float *c,
                       float *d, int m, int n, int k)
{
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            float sum = 0;
            for (int p = 0; p < k; p++)
                sum += a[i * k + p] * b[p * n + j];
            d[i * n + j] = sum + c[i * n + j];
        }
}

int main(void)
{
    rvt2_dev_t dev;
    rvt2_bo_t bo_a, bo_b, bo_c, bo_d;
    uint64_t seqno;
    float *a, *b, *c, *d, *d_ref;
    int ret, i;
    int pass = 0, fail = 0;
    char ir_cmd[512];
    struct rvt2_descriptor desc_blob;
    FILE *proc;

    printf("=== RVT2 compiledd End-to-End Test ===\n\n");

    /* Open device — required for this test */
    ret = rvt2_open(&dev);
    if (ret) {
        printf("  FAIL: cannot open device (ret=%d)\n", ret);
        return 1;
    }

    /* Allocate BOs */
    ret = rvt2_bo_alloc(&dev, M * K * sizeof(float), 0, &bo_a);
    ret |= rvt2_bo_alloc(&dev, K * N * sizeof(float), 0, &bo_b);
    ret |= rvt2_bo_alloc(&dev, M * N * sizeof(float), 0, &bo_c);
    ret |= rvt2_bo_alloc(&dev, M * N * sizeof(float), 0, &bo_d);
    if (ret) {
        printf("  FAIL: BO allocation failed\n");
        rvt2_close(&dev);
        return 1;
    }

    /* Fill input data */
    a = rvt2_bo_map(&dev, &bo_a);
    b = rvt2_bo_map(&dev, &bo_b);
    c = rvt2_bo_map(&dev, &bo_c);
    d = rvt2_bo_map(&dev, &bo_d);

    for (i = 0; i < M * K; i++) a[i] = (float)(i + 1);
    for (i = 0; i < K * N; i++) b[i] = (float)(i + 1) * 0.5f;
    for (i = 0; i < M * N; i++) c[i] = 1.0f;
    memset(d, 0, M * N * sizeof(float));

    /*
     * Step 1: Generate IR with REAL DMA addresses from the BOs,
     * pipe through compiledd, read back the binary descriptor.
     */
    printf("[Step 1: compiledd IR → descriptor with real DMA addresses]\n");
    snprintf(ir_cmd, sizeof(ir_cmd),
             "echo 'ternary_matmul %d %d %d 0 %lx %lx %lx %lx' | "
             "../lib/compiledd/compiledd",
             M, N, K,
             (unsigned long)bo_a.dma_addr, (unsigned long)bo_b.dma_addr,
             (unsigned long)bo_c.dma_addr, (unsigned long)bo_d.dma_addr);

    proc = popen(ir_cmd, "r");
    if (!proc) {
        printf("  FAIL: popen compiledd failed\n");
        fail++;
        goto cleanup;
    }

    size_t nread = fread(&desc_blob, 1, sizeof(desc_blob), proc);
    int pstat = pclose(proc);
    if (pstat != 0 || nread != sizeof(desc_blob)) {
        printf("  FAIL: compiledd returned error or wrong size (%zu bytes)\n", nread);
        fail++;
        goto cleanup;
    }
    printf("  PASS: compiledd produced 64-byte descriptor with real DMA addrs\n");
    pass++;

    /*
     * Step 2: Submit the compiledd-generated descriptor blob through
     * the raw descriptor ioctl — this is the TRUE e2e path.
     */
    printf("[Step 2: submit compiledd descriptor through raw ioctl]\n");
    ret = rvt2_submit_raw(&dev, &desc_blob, 1, &seqno);
    if (ret) {
        printf("  FAIL: rvt2_submit_raw failed (ret=%d)\n", ret);
        fail++;
        goto cleanup;
    }

    ret = rvt2_wait(&dev, seqno, 5000000000LL);
    if (ret) {
        printf("  FAIL: rvt2_wait failed (ret=%d)\n", ret);
        fail++;
        goto cleanup;
    }

    /* Verify result */
    d_ref = malloc(M * N * sizeof(float));
    ref_matmul(a, b, c, d_ref, M, N, K);
    int correct = 1;
    for (i = 0; i < M * N; i++) {
        if (fabsf(d[i] - d_ref[i]) > EPSILON) {
            printf("  MISMATCH at [%d]: got %f expected %f\n",
                   i, d[i], d_ref[i]);
            correct = 0;
            break;
        }
    }
    if (correct) {
        printf("  PASS: D=A*B+C via compiledd raw descriptor matches reference\n");
        pass++;
    } else {
        printf("  FAIL: result mismatch\n");
        fail++;
    }
    free(d_ref);

cleanup:
    rvt2_bo_free(&dev, &bo_a);
    rvt2_bo_free(&dev, &bo_b);
    rvt2_bo_free(&dev, &bo_c);
    rvt2_bo_free(&dev, &bo_d);
    rvt2_close(&dev);

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
