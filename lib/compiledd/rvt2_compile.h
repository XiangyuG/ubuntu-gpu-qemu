/* SPDX-License-Identifier: MIT */
#ifndef RVT2_COMPILE_H
#define RVT2_COMPILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

int rvt2_compile_ir_stream(FILE *in, FILE *out, FILE *err,
                           int *count, int *errors);
int rvt2_compile_ir_buffer(const char *input, size_t input_len,
                           unsigned char **output, size_t *output_len,
                           char **log, int *count, int *errors);
void rvt2_compile_free(void *ptr);

#endif /* RVT2_COMPILE_H */
