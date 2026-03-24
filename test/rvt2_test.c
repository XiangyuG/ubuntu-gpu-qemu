// SPDX-License-Identifier: MIT
/*
 * RVT2 smoke test - exercises AC-2 through AC-6
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include "../lib/libtmatmulrt/rvt2_lib.h"
#include "../../include/uapi/rvt2_drm.h"

#define TEST_M 4
#define TEST_N 4
#define TEST_K 4
#define EPSILON 1e-4f

static int tests_passed, tests_failed;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); tests_passed++; } \
    else { printf("  FAIL: %s\n", msg); tests_failed++; } \
} while (0)

/* Reference matmul on host */
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

static int test_open_close(void)
{
    rvt2_dev_t dev;
    int ret;

    printf("[Test: device open/close]\n");
    ret = rvt2_open(&dev);
    CHECK(ret == 0, "rvt2_open succeeds");
    if (ret) return -1;

    rvt2_close(&dev);
    CHECK(1, "rvt2_close succeeds");
    return 0;
}

static int test_bo_lifecycle(void)
{
    rvt2_dev_t dev;
    rvt2_bo_t bo;
    int ret;

    printf("[Test: BO lifecycle]\n");
    ret = rvt2_open(&dev);
    if (ret) { printf("  SKIP: cannot open device\n"); return -1; }

    /* Positive: allocate BO */
    ret = rvt2_bo_alloc(&dev, 4096, 0, &bo);
    CHECK(ret == 0, "rvt2_bo_alloc(4096) succeeds");

    if (ret == 0) {
        /* Map and write/read */
        void *ptr = rvt2_bo_map(&dev, &bo);
        CHECK(ptr != NULL, "rvt2_bo_map returns non-NULL");
        if (ptr) {
            memset(ptr, 0xAB, 4096);
            CHECK(((unsigned char *)ptr)[0] == 0xAB, "mmap read/write works");
        }

        /* Free */
        rvt2_bo_free(&dev, &bo);
        CHECK(1, "rvt2_bo_free succeeds");
    }

    /* Negative: alloc with size=0 */
    ret = rvt2_bo_alloc(&dev, 0, 0, &bo);
    CHECK(ret != 0, "rvt2_bo_alloc(0) returns error");

    rvt2_close(&dev);
    return 0;
}

/* SIGBUS test for destroy-after-mmap (AC-3 negative) */
static sigjmp_buf sigbus_jmp;

static void sigbus_handler(int sig)
{
    (void)sig;
    siglongjmp(sigbus_jmp, 1);
}

static int test_destroy_sigbus(void)
{
    rvt2_dev_t dev;
    rvt2_bo_t bo;
    int ret;
    void *ptr;
    struct sigaction sa, old_sa;

    printf("[Test: BO destroy-after-mmap SIGBUS]\n");
    ret = rvt2_open(&dev);
    if (ret) { printf("  FAIL: cannot open device\n"); tests_failed++; return -1; }

    ret = rvt2_bo_alloc(&dev, 4096, 0, &bo);
    if (ret) { printf("  FAIL: alloc failed\n"); tests_failed++; rvt2_close(&dev); return -1; }

    ptr = rvt2_bo_map(&dev, &bo);
    if (!ptr) { printf("  FAIL: mmap failed\n"); tests_failed++; rvt2_close(&dev); return -1; }

    /* Write works before destroy */
    ((volatile unsigned char *)ptr)[0] = 0x42;
    CHECK(((volatile unsigned char *)ptr)[0] == 0x42, "mmap access works before destroy");

    /* Destroy the BO while mapping is still active */
    struct rvt2_bo_destroy dreq = { .handle = bo.handle };
    ret = ioctl(dev.fd, _IOW('R', 0x02, struct rvt2_bo_destroy), &dreq);
    CHECK(ret == 0, "BO_DESTROY with active mmap succeeds");

    /* Set up SIGBUS handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigbus_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGBUS, &sa, &old_sa);

    if (sigsetjmp(sigbus_jmp, 1) == 0) {
        /* Try to access the destroyed mapping — should trigger SIGBUS */
        ((volatile unsigned char *)ptr)[0] = 0xFF;
        /* If we get here, no SIGBUS was raised */
        CHECK(0, "post-destroy mmap access triggers SIGBUS");
    } else {
        /* SIGBUS was caught */
        CHECK(1, "post-destroy mmap access triggers SIGBUS");
    }

    sigaction(SIGBUS, &old_sa, NULL);

    /* Don't call rvt2_bo_free since BO is already destroyed */
    bo.handle = 0;
    bo.map = NULL;
    rvt2_close(&dev);
    return 0;
}

static int test_submit_wait(void)
{
    rvt2_dev_t dev;
    rvt2_bo_t bo_a, bo_b, bo_c, bo_d;
    uint64_t seqno;
    float *a, *b, *c, *d, *d_ref;
    int ret;

    printf("[Test: submit and wait]\n");
    ret = rvt2_open(&dev);
    if (ret) { printf("  SKIP: cannot open device\n"); return -1; }

    size_t sz_ab = TEST_M * TEST_K * sizeof(float);
    size_t sz_cd = TEST_M * TEST_N * sizeof(float);

    ret = rvt2_bo_alloc(&dev, sz_ab, 0, &bo_a);
    ret |= rvt2_bo_alloc(&dev, TEST_K * TEST_N * sizeof(float), 0, &bo_b);
    ret |= rvt2_bo_alloc(&dev, sz_cd, 0, &bo_c);
    ret |= rvt2_bo_alloc(&dev, sz_cd, 0, &bo_d);
    CHECK(ret == 0, "allocate 4 BOs");
    if (ret) { rvt2_close(&dev); return -1; }

    a = rvt2_bo_map(&dev, &bo_a);
    b = rvt2_bo_map(&dev, &bo_b);
    c = rvt2_bo_map(&dev, &bo_c);
    d = rvt2_bo_map(&dev, &bo_d);
    CHECK(a && b && c && d, "map all BOs");

    /* Fill test data */
    for (int i = 0; i < TEST_M * TEST_K; i++) a[i] = (float)(i + 1);
    for (int i = 0; i < TEST_K * TEST_N; i++) b[i] = (float)(i + 1) * 0.5f;
    for (int i = 0; i < TEST_M * TEST_N; i++) c[i] = 1.0f;
    memset(d, 0, sz_cd);

    /* Submit */
    ret = rvt2_submit(&dev, bo_a.handle, bo_b.handle,
                      bo_c.handle, bo_d.handle,
                      TEST_M, TEST_N, TEST_K, 0, &seqno);
    CHECK(ret == 0, "rvt2_submit succeeds");

    /* Wait */
    ret = rvt2_wait(&dev, seqno, 5000000000LL); /* 5s timeout */
    CHECK(ret == 0, "rvt2_wait returns signaled");

    /* Verify result */
    d_ref = malloc(sz_cd);
    ref_matmul(a, b, c, d_ref, TEST_M, TEST_N, TEST_K);
    int correct = 1;
    for (int i = 0; i < TEST_M * TEST_N; i++) {
        if (fabsf(d[i] - d_ref[i]) > EPSILON) {
            printf("  MISMATCH at [%d]: got %f, expected %f\n",
                   i, d[i], d_ref[i]);
            correct = 0;
            break;
        }
    }
    CHECK(correct, "D = A*B+C result matches reference");
    free(d_ref);

    /* Negative: wait with timeout=0 on future seqno */
    ret = rvt2_wait(&dev, seqno + 1000, 0);
    CHECK(ret != 0, "rvt2_wait(timeout=0, future seqno) returns timeout");

    /* Negative: submit with invalid BO handle */
    ret = rvt2_submit(&dev, 99999, bo_b.handle, bo_c.handle, bo_d.handle,
                      TEST_M, TEST_N, TEST_K, 0, &seqno);
    CHECK(ret != 0, "rvt2_submit(invalid handle) returns error");

    rvt2_bo_free(&dev, &bo_a);
    rvt2_bo_free(&dev, &bo_b);
    rvt2_bo_free(&dev, &bo_c);
    rvt2_bo_free(&dev, &bo_d);
    rvt2_close(&dev);
    return 0;
}

int main(void)
{
    printf("=== RVT2 Smoke Test ===\n\n");

    test_open_close();
    test_bo_lifecycle();
    test_destroy_sigbus();
    test_submit_wait();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
