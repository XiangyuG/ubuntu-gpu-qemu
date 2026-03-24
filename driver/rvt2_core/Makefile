# SPDX-License-Identifier: GPL-2.0
obj-m := rvt2_core.o
rvt2_core-objs := rvt2_drv.o rvt2_bo.o rvt2_submit.o rvt2_irq.o rvt2_sysfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build
GSP_SYMVERS := $(PWD)/../rvt2_gsp_shim/Module.symvers

all: gsp_shim
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(GSP_SYMVERS) modules

gsp_shim:
	$(MAKE) -C $(KDIR) M=$(PWD)/../rvt2_gsp_shim modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C $(KDIR) M=$(PWD)/../rvt2_gsp_shim clean
