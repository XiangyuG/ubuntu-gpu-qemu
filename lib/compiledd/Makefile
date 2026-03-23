# SPDX-License-Identifier: MIT
CC ?= gcc
CFLAGS := -Wall -Wextra -O2

all: compiledd

compiledd: compiledd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f compiledd

.PHONY: all clean
