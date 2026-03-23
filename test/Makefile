# SPDX-License-Identifier: MIT
CC ?= gcc
CFLAGS := -Wall -Wextra -O2 -I../lib/libtmatmulrt -I../include/uapi
LDFLAGS := -L../lib/libtmatmulrt -ltmatmulrt -lm -Wl,-rpath,../lib/libtmatmulrt

all: rvt2_test rvt2_bench rvt2_cxl_test

rvt2_test: rvt2_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rvt2_bench: rvt2_bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rvt2_cxl_test: rvt2_cxl_test.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

clean:
	rm -f rvt2_test rvt2_bench rvt2_cxl_test

.PHONY: all clean
