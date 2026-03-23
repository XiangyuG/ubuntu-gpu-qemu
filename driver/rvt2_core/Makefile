# SPDX-License-Identifier: GPL-2.0
obj-m := rvt2_core.o
rvt2_core-objs := rvt2_drv.o rvt2_bo.o rvt2_submit.o rvt2_irq.o rvt2_sysfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
