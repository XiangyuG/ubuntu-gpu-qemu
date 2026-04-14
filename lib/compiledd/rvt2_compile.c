// SPDX-License-Identifier: MIT
/*
 * Shared RVT2 IR compiler core.
 */

#include "rvt2_compile.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return s;
}

static int parse_u32_dec(const char *text, uint32_t *out)
{
    unsigned long value;
    char *end;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end != '\0' || value > UINT_MAX)
        return -1;

    *out = (uint32_t)value;
    return 0;
}

static int parse_u64_hex(const char *text, uint64_t *out)
{
    unsigned long long value;
    char *end;

    errno = 0;
    value = strtoull(text, &end, 16);
    if (errno || *end != '\0')
        return -1;

    *out = (uint64_t)value;
    return 0;
}

static int compile_line(const char *line, unsigned int line_no,
                        uint64_t *next_seqno,
                        struct rvt2_descriptor *desc, FILE *err)
{
    char *copy, *tok, *saveptr = NULL;
    char *tokens[9];
    int ntokens = 0;
    uint32_t m, n, k, dtype;
    uint64_t addr_a, addr_b, addr_c, addr_d;
    int ret = -1;

    copy = strdup(line);
    if (!copy) {
        fprintf(err, "error:%u: out of memory\n", line_no);
        return -1;
    }

    for (tok = strtok_r(copy, " \t\r\n", &saveptr);
         tok && ntokens < 9;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        tokens[ntokens++] = tok;
    }

    if (ntokens != 9 || strtok_r(NULL, " \t\r\n", &saveptr)) {
        fprintf(err, "error:%u: malformed IR line: %s", line_no, line);
        if (line[0] && line[strlen(line) - 1] != '\n')
            fputc('\n', err);
        goto out;
    }

    if (strcmp(tokens[0], "ternary_matmul") != 0) {
        fprintf(err, "error:%u: unsupported operation '%s'\n",
                line_no, tokens[0]);
        goto out;
    }

    if (parse_u32_dec(tokens[1], &m) ||
        parse_u32_dec(tokens[2], &n) ||
        parse_u32_dec(tokens[3], &k) ||
        parse_u32_dec(tokens[4], &dtype)) {
        fprintf(err, "error:%u: invalid numeric shape or dtype\n", line_no);
        goto out;
    }

    if (parse_u64_hex(tokens[5], &addr_a) ||
        parse_u64_hex(tokens[6], &addr_b) ||
        parse_u64_hex(tokens[7], &addr_c) ||
        parse_u64_hex(tokens[8], &addr_d)) {
        fprintf(err, "error:%u: invalid DMA address\n", line_no);
        goto out;
    }

    if (m == 0 || n == 0 || k == 0 || m > 4096 || n > 4096 || k > 4096) {
        fprintf(err, "error:%u: invalid dimensions m=%u n=%u k=%u\n",
                line_no, m, n, k);
        goto out;
    }

    if (dtype > 2) {
        fprintf(err, "error:%u: invalid dtype %u\n", line_no, dtype);
        goto out;
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
    desc->fence_seqno = (*next_seqno)++;
    ret = 0;

out:
    free(copy);
    return ret;
}

int rvt2_compile_ir_stream(FILE *in, FILE *out, FILE *err,
                           int *count, int *errors)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    uint64_t next_seqno = 1;
    unsigned int line_no = 0;
    int local_count = 0;
    int local_errors = 0;

    while ((len = getline(&line, &cap, in)) != -1) {
        struct rvt2_descriptor desc;
        char *trimmed;

        line_no++;
        (void)len;

        trimmed = skip_ws(line);
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        if (compile_line(trimmed, line_no, &next_seqno, &desc, err) != 0) {
            local_errors++;
            continue;
        }

        if (fwrite(&desc, RVT2_DESC_SIZE, 1, out) != 1) {
            fprintf(err, "error:%u: failed to write descriptor\n", line_no);
            free(line);
            if (count)
                *count = local_count;
            if (errors)
                *errors = local_errors + 1;
            return -EIO;
        }
        local_count++;
    }

    free(line);
    if (count)
        *count = local_count;
    if (errors)
        *errors = local_errors;

    return local_errors ? -EINVAL : 0;
}

int rvt2_compile_ir_buffer(const char *input, size_t input_len,
                           unsigned char **output, size_t *output_len,
                           char **log, int *count, int *errors)
{
    FILE *in = NULL;
    FILE *out = NULL;
    FILE *err = NULL;
    char *out_buf = NULL;
    char *err_buf = NULL;
    size_t out_size = 0;
    size_t err_size = 0;
    int ret;

    if (!input || !output || !output_len || !log)
        return -EINVAL;

    *output = NULL;
    *output_len = 0;
    *log = NULL;

    in = fmemopen((void *)input, input_len, "r");
    if (!in)
        return -errno;

    out = open_memstream(&out_buf, &out_size);
    if (!out) {
        ret = -errno;
        goto out_close_in;
    }

    err = open_memstream(&err_buf, &err_size);
    if (!err) {
        ret = -errno;
        goto out_close_out;
    }

    ret = rvt2_compile_ir_stream(in, out, err, count, errors);
    fflush(out);
    fflush(err);

    fclose(err);
    err = NULL;
    fclose(out);
    out = NULL;
    fclose(in);
    in = NULL;

    if (ret == 0) {
        *output = (unsigned char *)out_buf;
        *output_len = out_size;
        out_buf = NULL;
    }

    if (err_size == 0 && ret != 0) {
        free(err_buf);
        err_buf = strdup("error: compilation failed\n");
        if (!err_buf)
            ret = -ENOMEM;
    }
    *log = err_buf;
    err_buf = NULL;

out_close_out:
    if (out)
        fclose(out);
out_close_in:
    if (in)
        fclose(in);
    if (err)
        fclose(err);
    free(out_buf);
    free(err_buf);
    return ret;
}

void rvt2_compile_free(void *ptr)
{
    free(ptr);
}
