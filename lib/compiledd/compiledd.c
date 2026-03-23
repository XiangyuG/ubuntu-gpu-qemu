// SPDX-License-Identifier: MIT
/*
 * compiledd - Minimal compilation service for RVT2
 *
 * Reads a simple IR format from stdin, validates shape, and produces
 * a descriptor chain on stdout (binary rvt2_descriptor structs).
 *
 * IR format (text, one per line):
 *   ternary_matmul <m> <n> <k> <dtype> <addr_a> <addr_b> <addr_c> <addr_d>
 *
 * dtype: float32=0, float16=1, int8=2
 * addresses: hex, 0x-prefixed DMA addresses
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RVT2_OP_TERNARY_MATMUL 0x01
#define RVT2_DESC_SIZE 64

struct rvt2_descriptor {
    uint32_t opcode;
    uint32_t flags;
    uint64_t input_a_addr;
    uint64_t input_b_addr;
    uint64_t input_c_addr;
    uint64_t output_d_addr;
    uint32_t m, n, k;
    uint32_t dtype;
    uint64_t fence_seqno;
};

static uint64_t next_seqno = 1;

static int compile_line(const char *line, struct rvt2_descriptor *desc)
{
    char op[64];
    uint32_t m, n, k, dtype;
    uint64_t addr_a, addr_b, addr_c, addr_d;

    int parsed = sscanf(line, "%63s %u %u %u %u %lx %lx %lx %lx",
                        op, &m, &n, &k, &dtype,
                        (unsigned long *)&addr_a, (unsigned long *)&addr_b,
                        (unsigned long *)&addr_c, (unsigned long *)&addr_d);

    if (parsed != 9) {
        fprintf(stderr, "error: malformed IR line: %s\n", line);
        return -1;
    }

    if (strcmp(op, "ternary_matmul") != 0) {
        fprintf(stderr, "error: unsupported operation '%s'\n", op);
        return -1;
    }

    if (m == 0 || n == 0 || k == 0 || m > 4096 || n > 4096 || k > 4096) {
        fprintf(stderr, "error: invalid dimensions m=%u n=%u k=%u\n", m, n, k);
        return -1;
    }

    if (dtype > 2) {
        fprintf(stderr, "error: invalid dtype %u\n", dtype);
        return -1;
    }

    memset(desc, 0, sizeof(*desc));
    desc->opcode = RVT2_OP_TERNARY_MATMUL;
    desc->m = m;
    desc->n = n;
    desc->k = k;
    desc->dtype = dtype;
    desc->input_a_addr = addr_a;
    desc->input_b_addr = addr_b;
    desc->input_c_addr = addr_c;
    desc->output_d_addr = addr_d;
    desc->fence_seqno = next_seqno++;

    return 0;
}

int main(int argc, char **argv)
{
    char line[1024];
    int count = 0, errors = 0;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Usage: compiledd < input.ir > output.bin\n"
               "IR format: ternary_matmul <m> <n> <k> <dtype> <addr_a> <addr_b> <addr_c> <addr_d>\n");
        return 0;
    }

    while (fgets(line, sizeof(line), stdin)) {
        struct rvt2_descriptor desc;

        /* Skip empty lines and comments */
        if (line[0] == '\n' || line[0] == '#')
            continue;

        if (compile_line(line, &desc) != 0) {
            errors++;
            continue;
        }

        if (fwrite(&desc, RVT2_DESC_SIZE, 1, stdout) != 1) {
            fprintf(stderr, "error: failed to write descriptor\n");
            return 1;
        }
        count++;
    }

    fprintf(stderr, "compiledd: %d descriptors generated, %d errors\n",
            count, errors);
    return errors ? 1 : 0;
}
