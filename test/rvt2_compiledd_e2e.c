// SPDX-License-Identifier: MIT
/*
 * RVT2 compiledd end-to-end test (AC-7)
 *
 * Tests both single and multi-descriptor chain submission through
 * the compiledd → SUBMIT_RAW → device pipeline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include "../lib/libtmatmulrt/rvt2_lib.h"
#include "../../include/uapi/rvt2_drm.h"

#define M 4
#define N 4
#define K 4
#define EPSILON 1e-4f

static int pass_count, fail_count;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass_count++; } \
    else { printf("  FAIL: %s\n", msg); fail_count++; } \
} while (0)

static char compiledd_path[PATH_MAX];

static void resolve_compiledd(void)
{
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) {
        snprintf(compiledd_path, sizeof(compiledd_path),
                 "../lib/compiledd/compiledd");
        return;
    }
    exe[len] = '\0';
    snprintf(compiledd_path, sizeof(compiledd_path),
             "%s/../lib/compiledd/compiledd", dirname(exe));
}

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

static int test_single_desc(rvt2_dev_t *dev)
{
    rvt2_bo_t bo_a, bo_b, bo_c, bo_d;
    uint64_t seqno;
    float *a, *b, *c, *d, *d_ref;
    struct rvt2_descriptor desc;
    char cmd[512];
    FILE *proc;
    int ret, i;

    printf("[Test: single descriptor via compiledd]\n");

    ret = rvt2_bo_alloc(dev, M*K*sizeof(float), 0, &bo_a);
    ret |= rvt2_bo_alloc(dev, K*N*sizeof(float), 0, &bo_b);
    ret |= rvt2_bo_alloc(dev, M*N*sizeof(float), 0, &bo_c);
    ret |= rvt2_bo_alloc(dev, M*N*sizeof(float), 0, &bo_d);
    if (ret) { printf("  FAIL: BO alloc\n"); fail_count++; return -1; }

    a = rvt2_bo_map(dev, &bo_a); b = rvt2_bo_map(dev, &bo_b);
    c = rvt2_bo_map(dev, &bo_c); d = rvt2_bo_map(dev, &bo_d);
    for (i = 0; i < M*K; i++) a[i] = (float)(i+1);
    for (i = 0; i < K*N; i++) b[i] = (float)(i+1)*0.5f;
    for (i = 0; i < M*N; i++) c[i] = 1.0f;
    memset(d, 0, M*N*sizeof(float));

    snprintf(cmd, sizeof(cmd),
             "echo 'ternary_matmul %d %d %d 0 %lx %lx %lx %lx' | %s",
             M, N, K,
             (unsigned long)bo_a.dma_addr, (unsigned long)bo_b.dma_addr,
             (unsigned long)bo_c.dma_addr, (unsigned long)bo_d.dma_addr,
             compiledd_path);

    proc = popen(cmd, "r");
    if (!proc || fread(&desc, 1, sizeof(desc), proc) != sizeof(desc)) {
        CHECK(0, "compiledd single desc"); pclose(proc);
        goto cleanup;
    }
    CHECK(pclose(proc) == 0, "compiledd single desc produced 64B");

    ret = rvt2_submit_raw(dev, &desc, 1, &seqno);
    CHECK(ret == 0, "submit_raw single desc");
    if (ret) goto cleanup;

    ret = rvt2_wait(dev, seqno, 5000000000LL);
    CHECK(ret == 0, "wait single desc");

    d_ref = malloc(M*N*sizeof(float));
    ref_matmul(a, b, c, d_ref, M, N, K);
    int ok = 1;
    for (i = 0; i < M*N; i++)
        if (fabsf(d[i] - d_ref[i]) > EPSILON) { ok = 0; break; }
    CHECK(ok, "D=A*B+C matches reference (single)");
    free(d_ref);

cleanup:
    rvt2_bo_free(dev, &bo_a); rvt2_bo_free(dev, &bo_b);
    rvt2_bo_free(dev, &bo_c); rvt2_bo_free(dev, &bo_d);
    return 0;
}

static int test_multi_desc(rvt2_dev_t *dev)
{
    rvt2_bo_t bo_a1, bo_b1, bo_c1, bo_d1;
    rvt2_bo_t bo_a2, bo_b2, bo_c2, bo_d2;
    uint64_t seqno;
    float *a1, *b1, *c1, *d1, *a2, *b2, *c2, *d2;
    float *ref1, *ref2;
    struct rvt2_descriptor descs[2];
    char cmd[1024];
    FILE *proc;
    int ret, i;
    size_t nread;

    printf("[Test: 2-descriptor chain via compiledd]\n");

    ret = rvt2_bo_alloc(dev, M*K*sizeof(float), 0, &bo_a1);
    ret |= rvt2_bo_alloc(dev, K*N*sizeof(float), 0, &bo_b1);
    ret |= rvt2_bo_alloc(dev, M*N*sizeof(float), 0, &bo_c1);
    ret |= rvt2_bo_alloc(dev, M*N*sizeof(float), 0, &bo_d1);
    ret |= rvt2_bo_alloc(dev, M*K*sizeof(float), 0, &bo_a2);
    ret |= rvt2_bo_alloc(dev, K*N*sizeof(float), 0, &bo_b2);
    ret |= rvt2_bo_alloc(dev, M*N*sizeof(float), 0, &bo_c2);
    ret |= rvt2_bo_alloc(dev, M*N*sizeof(float), 0, &bo_d2);
    if (ret) { printf("  FAIL: BO alloc\n"); fail_count++; return -1; }

    a1 = rvt2_bo_map(dev, &bo_a1); b1 = rvt2_bo_map(dev, &bo_b1);
    c1 = rvt2_bo_map(dev, &bo_c1); d1 = rvt2_bo_map(dev, &bo_d1);
    a2 = rvt2_bo_map(dev, &bo_a2); b2 = rvt2_bo_map(dev, &bo_b2);
    c2 = rvt2_bo_map(dev, &bo_c2); d2 = rvt2_bo_map(dev, &bo_d2);

    for (i = 0; i < M*K; i++) { a1[i] = (float)(i+1); a2[i] = (float)(i+2); }
    for (i = 0; i < K*N; i++) { b1[i] = 0.5f; b2[i] = 0.25f; }
    for (i = 0; i < M*N; i++) { c1[i] = 1.0f; c2[i] = 2.0f; }
    memset(d1, 0, M*N*sizeof(float));
    memset(d2, 0, M*N*sizeof(float));

    /* Generate 2-line IR for 2 descriptors */
    snprintf(cmd, sizeof(cmd),
             "printf 'ternary_matmul %d %d %d 0 %lx %lx %lx %lx\\n"
             "ternary_matmul %d %d %d 0 %lx %lx %lx %lx\\n' | %s",
             M, N, K,
             (unsigned long)bo_a1.dma_addr, (unsigned long)bo_b1.dma_addr,
             (unsigned long)bo_c1.dma_addr, (unsigned long)bo_d1.dma_addr,
             M, N, K,
             (unsigned long)bo_a2.dma_addr, (unsigned long)bo_b2.dma_addr,
             (unsigned long)bo_c2.dma_addr, (unsigned long)bo_d2.dma_addr,
             compiledd_path);

    proc = popen(cmd, "r");
    if (!proc) { CHECK(0, "popen compiledd"); goto cleanup2; }
    nread = fread(descs, 1, sizeof(descs), proc);
    CHECK(pclose(proc) == 0 && nread == 128,
          "compiledd produced 128-byte (2 descriptors)");
    if (nread != 128) goto cleanup2;

    /* Submit both as a single chain */
    ret = rvt2_submit_raw(dev, descs, 2, &seqno);
    CHECK(ret == 0, "submit_raw 2-descriptor chain");
    if (ret) goto cleanup2;

    ret = rvt2_wait(dev, seqno, 5000000000LL);
    CHECK(ret == 0, "wait 2-descriptor chain");

    /* Verify both results */
    ref1 = malloc(M*N*sizeof(float));
    ref2 = malloc(M*N*sizeof(float));
    ref_matmul(a1, b1, c1, ref1, M, N, K);
    ref_matmul(a2, b2, c2, ref2, M, N, K);

    int ok1 = 1, ok2 = 1;
    for (i = 0; i < M*N; i++)
        if (fabsf(d1[i] - ref1[i]) > EPSILON) { ok1 = 0; break; }
    for (i = 0; i < M*N; i++)
        if (fabsf(d2[i] - ref2[i]) > EPSILON) { ok2 = 0; break; }
    CHECK(ok1, "chain desc[0] D=A*B+C correct");
    CHECK(ok2, "chain desc[1] D=A*B+C correct");
    free(ref1); free(ref2);

cleanup2:
    rvt2_bo_free(dev, &bo_a1); rvt2_bo_free(dev, &bo_b1);
    rvt2_bo_free(dev, &bo_c1); rvt2_bo_free(dev, &bo_d1);
    rvt2_bo_free(dev, &bo_a2); rvt2_bo_free(dev, &bo_b2);
    rvt2_bo_free(dev, &bo_c2); rvt2_bo_free(dev, &bo_d2);
    return 0;
}

int main(void)
{
    rvt2_dev_t dev;
    int ret;

    printf("=== RVT2 compiledd End-to-End Test ===\n\n");
    resolve_compiledd();

    ret = rvt2_open(&dev);
    if (ret) {
        printf("  FAIL: cannot open device (ret=%d)\n", ret);
        return 1;
    }

    test_single_desc(&dev);
    test_multi_desc(&dev);

    rvt2_close(&dev);
    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
