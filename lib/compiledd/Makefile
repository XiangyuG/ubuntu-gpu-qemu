# SPDX-License-Identifier: MIT
CC ?= gcc
CFLAGS := -D_GNU_SOURCE -Wall -Wextra -O2

all: compiledd compilerd

compiledd: compiledd.o rvt2_compile.o
	$(CC) $(CFLAGS) -o $@ $^

compilerd: compilerd.o rvt2_compile.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c rvt2_compile.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f compiledd compilerd *.o

.PHONY: all clean
