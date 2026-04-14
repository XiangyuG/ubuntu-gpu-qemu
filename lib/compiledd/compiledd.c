// SPDX-License-Identifier: MIT
/*
 * compiledd - one-shot RVT2 IR to descriptor compiler.
 */

#include <stdio.h>
#include <string.h>

#include "rvt2_compile.h"

int main(int argc, char **argv)
{
    int count = 0, errors = 0;
    int ret;

    if (argc > 1 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Usage: compiledd < input.ir > output.bin\n"
               "IR format: ternary_matmul <m> <n> <k> <dtype> "
               "<addr_a> <addr_b> <addr_c> <addr_d>\n");
        return 0;
    }

    ret = rvt2_compile_ir_stream(stdin, stdout, stderr, &count, &errors);
    fprintf(stderr, "compiledd: %d descriptors generated, %d errors\n",
            count, errors);
    return ret ? 1 : 0;
}
