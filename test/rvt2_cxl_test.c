// SPDX-License-Identifier: MIT
/*
 * RVT2 CXL Type-2 HDM stub test (AC-8)
 *
 * Tests that BAR2 (HDM window) is visible and accessible from host.
 * Verifies read/write coherence within the HDM window.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

/* HDM window size must match QEMU device model */
#define RVT2_HDM_SIZE (1 * 1024 * 1024)  /* 1MiB */

static int tests_passed, tests_failed;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); tests_passed++; } \
    else { printf("  FAIL: %s\n", msg); tests_failed++; } \
} while (0)

/*
 * BAR2 is a PCI memory BAR. Access it via sysfs resource file.
 * Path: /sys/bus/pci/devices/0000:00:01.0/resource2
 */
static int test_hdm_access(void)
{
    const char *res_path = "/sys/bus/pci/devices/0000:00:01.0/resource2";
    int fd;
    void *hdm;
    uint32_t *p;

    printf("[Test: CXL Type-2 HDM window access]\n");

    fd = open(res_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        printf("  SKIP: cannot open %s (need root or sysfs access)\n", res_path);
        return -1;
    }

    hdm = mmap(NULL, RVT2_HDM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hdm == MAP_FAILED) {
        printf("  SKIP: mmap failed\n");
        close(fd);
        return -1;
    }

    /* Positive: write and read back */
    p = (uint32_t *)hdm;
    p[0] = 0xDEADBEEF;
    p[1] = 0xCAFEBABE;
    CHECK(p[0] == 0xDEADBEEF, "HDM write/read word 0 coherent");
    CHECK(p[1] == 0xCAFEBABE, "HDM write/read word 1 coherent");

    /* Positive: write pattern across window */
    for (int i = 0; i < 256; i++)
        p[i] = (uint32_t)i;
    int ok = 1;
    for (int i = 0; i < 256; i++) {
        if (p[i] != (uint32_t)i) { ok = 0; break; }
    }
    CHECK(ok, "HDM 1KiB pattern write/read coherent");

    /* Positive: access near end of window */
    uint32_t *last = (uint32_t *)((char *)hdm + RVT2_HDM_SIZE - 4);
    *last = 0x12345678;
    CHECK(*last == 0x12345678, "HDM last word accessible");

    munmap(hdm, RVT2_HDM_SIZE);
    close(fd);
    return 0;
}

int main(void)
{
    printf("=== RVT2 CXL Type-2 HDM Test ===\n\n");

    test_hdm_access();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
