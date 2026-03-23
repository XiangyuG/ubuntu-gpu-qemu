# SPDX-License-Identifier: MIT
CC ?= gcc
CFLAGS := -Wall -Wextra -O2 -fPIC
LDFLAGS := -shared

TARGET := libtmatmulrt.so
OBJS := rvt2_lib.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
