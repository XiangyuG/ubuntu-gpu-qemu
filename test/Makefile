# SPDX-License-Identifier: MIT
CC ?= gcc
CFLAGS := -Wall -Wextra -O2 -I../lib/libtmatmulrt -I../include/uapi
LDFLAGS := -L../lib/libtmatmulrt -ltmatmulrt -lm -Wl,-rpath,'$$ORIGIN/../lib/libtmatmulrt'

all: runtime compiledd rvt2_test rvt2_bench rvt2_cxl_test

runtime:
	$(MAKE) -C ../lib/libtmatmulrt

compiledd:
	$(MAKE) -C ../lib/compiledd

rvt2_test: rvt2_test.c | runtime
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rvt2_bench: rvt2_bench.c | runtime
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rvt2_cxl_test: rvt2_cxl_test.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

clean:
	$(MAKE) -C ../lib/libtmatmulrt clean
	$(MAKE) -C ../lib/compiledd clean
	rm -f rvt2_test rvt2_bench rvt2_cxl_test

.PHONY: all clean runtime compiledd
