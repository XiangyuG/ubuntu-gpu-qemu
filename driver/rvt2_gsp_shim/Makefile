# SPDX-License-Identifier: GPL-2.0
obj-m := rvt2_gsp_shim.o
rvt2_gsp_shim-objs := rvt2_gsp.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
