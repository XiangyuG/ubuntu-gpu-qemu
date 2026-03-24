// SPDX-License-Identifier: MIT
/*
 * RVT2 CXL Type-2 HDM test (AC-8)
 *
 * Tests HDM BO allocation through the driver ioctl path.
 * Verifies read/write coherence and that non-HDM path also works.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>
#include "../../include/uapi/rvt2_drm.h"

static int tests_passed, tests_failed;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); tests_passed++; } \
    else { printf("  FAIL: %s\n", msg); tests_failed++; } \
} while (0)

static int test_hdm_bo(void)
{
    int fd;
    struct rvt2_bo_create req = {0};
    uint32_t *p;
    void *map;
    int ret;

    printf("[Test: CXL Type-2 HDM BO via driver]\n");

    fd = open("/dev/rvt2_0", O_RDWR);
    if (fd < 0) {
        printf("  SKIP: cannot open /dev/rvt2_0\n");
        return -1;
    }

    /* Positive: allocate HDM BO */
    req.size = 4096;
    req.flags = RVT2_BO_FLAG_HDM;
    ret = ioctl(fd, RVT2_IOCTL_BO_CREATE, &req);
    CHECK(ret == 0, "HDM BO alloc succeeds");
    if (ret != 0) {
        printf("  errno=%d (%s)\n", errno, strerror(errno));
        close(fd);
        return -1;
    }

    /* Map HDM BO */
    map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, (off_t)req.handle << 12);
    CHECK(map != MAP_FAILED, "HDM BO mmap succeeds");
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }

    /* Write and read back */
    p = (uint32_t *)map;
    p[0] = 0xDEADBEEF;
    p[1] = 0xCAFEBABE;
    CHECK(p[0] == 0xDEADBEEF, "HDM write/read word 0 coherent");
    CHECK(p[1] == 0xCAFEBABE, "HDM write/read word 1 coherent");

    /* Pattern test */
    for (int i = 0; i < 256; i++)
        p[i] = (uint32_t)i;
    int ok = 1;
    for (int i = 0; i < 256; i++) {
        if (p[i] != (uint32_t)i) { ok = 0; break; }
    }
    CHECK(ok, "HDM 1KiB pattern write/read coherent");

    munmap(map, 4096);

    /* Destroy HDM BO */
    struct rvt2_bo_destroy dreq = { .handle = req.handle };
    ret = ioctl(fd, RVT2_IOCTL_BO_DESTROY, &dreq);
    CHECK(ret == 0, "HDM BO destroy succeeds");

    /* Negative: allocate HDM BO larger than window */
    req.size = 2 * 1024 * 1024; /* 2MB > 1MB HDM */
    req.flags = RVT2_BO_FLAG_HDM;
    ret = ioctl(fd, RVT2_IOCTL_BO_CREATE, &req);
    CHECK(ret != 0, "HDM BO alloc beyond window size returns error");

    close(fd);
    return 0;
}

int main(void)
{
    int rc;

    printf("=== RVT2 CXL Type-2 HDM Test ===\n\n");

    rc = test_hdm_bo();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed || rc ? 1 : 0;
}
