// SPDX-License-Identifier: MIT
/*
 * RVT2 throughput/latency benchmark
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../lib/libtmatmulrt/rvt2_lib.h"

#define WARMUP_ITERS 2
#define BENCH_ITERS  20

static double time_diff_ms(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1e6;
}

static int bench_matmul(int m, int n, int k)
{
    rvt2_dev_t dev;
    rvt2_bo_t bo_a, bo_b, bo_c, bo_d;
    uint64_t seqno;
    struct timespec t0, t1;
    int ret;

    ret = rvt2_open(&dev);
    if (ret) { printf("Cannot open device\n"); return -1; }

    size_t sz_a = m * k * sizeof(float);
    size_t sz_b = k * n * sizeof(float);
    size_t sz_cd = m * n * sizeof(float);

    rvt2_bo_alloc(&dev, sz_a, 0, &bo_a);
    rvt2_bo_alloc(&dev, sz_b, 0, &bo_b);
    rvt2_bo_alloc(&dev, sz_cd, 0, &bo_c);
    rvt2_bo_alloc(&dev, sz_cd, 0, &bo_d);

    float *a = rvt2_bo_map(&dev, &bo_a);
    float *b = rvt2_bo_map(&dev, &bo_b);
    float *c = rvt2_bo_map(&dev, &bo_c);

    for (int i = 0; i < m * k; i++) a[i] = 1.0f;
    for (int i = 0; i < k * n; i++) b[i] = 1.0f;
    for (int i = 0; i < m * n; i++) c[i] = 0.0f;

    /* Warmup */
    for (int i = 0; i < WARMUP_ITERS; i++) {
        rvt2_submit(&dev, bo_a.handle, bo_b.handle,
                    bo_c.handle, bo_d.handle, m, n, k, 0, &seqno);
        rvt2_wait(&dev, seqno, -1);
    }

    /* Benchmark */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < BENCH_ITERS; i++) {
        rvt2_submit(&dev, bo_a.handle, bo_b.handle,
                    bo_c.handle, bo_d.handle, m, n, k, 0, &seqno);
        rvt2_wait(&dev, seqno, -1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double total_ms = time_diff_ms(&t0, &t1);
    double per_iter = total_ms / BENCH_ITERS;
    double gflops = (2.0 * m * n * k + m * n) * BENCH_ITERS / (total_ms * 1e6);

    printf("  %4dx%4dx%4d : %.2f ms/iter, %.4f GFLOPS (emulated)\n",
           m, n, k, per_iter, gflops);

    rvt2_bo_free(&dev, &bo_a);
    rvt2_bo_free(&dev, &bo_b);
    rvt2_bo_free(&dev, &bo_c);
    rvt2_bo_free(&dev, &bo_d);
    rvt2_close(&dev);
    return 0;
}

int main(void)
{
    int rc = 0;

    printf("=== RVT2 Benchmark ===\n");
    rc |= bench_matmul(4, 4, 4);
    rc |= bench_matmul(16, 16, 16);
    rc |= bench_matmul(32, 32, 32);
    rc |= bench_matmul(64, 64, 64);
    return rc ? 1 : 0;
}
