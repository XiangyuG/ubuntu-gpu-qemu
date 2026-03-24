# SPDX-License-Identifier: GPL-2.0
obj-m := rvt2_core.o
rvt2_core-objs := rvt2_drv.o rvt2_bo.o rvt2_submit.o rvt2_irq.o rvt2_sysfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build
CORE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
GSP_DIR := $(abspath $(CORE_DIR)/../rvt2_gsp_shim)
GSP_SYMVERS := $(GSP_DIR)/Module.symvers

all: gsp_shim
	$(MAKE) -C $(KDIR) M=$(CORE_DIR) KBUILD_EXTRA_SYMBOLS=$(GSP_SYMVERS) modules

gsp_shim:
	$(MAKE) -C $(KDIR) M=$(GSP_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CORE_DIR) clean
	$(MAKE) -C $(KDIR) M=$(GSP_DIR) clean
