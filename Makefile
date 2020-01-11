obj-$(CONFIG_WC_MBOX) += mailbox-wc.o

obj-$(CONFIG_AOC_DRIVER)	+= aoc_core.o
aoc_core-objs := aoc.o ../aoc_ipc/aoc_ipc_core.o aoc_firmware.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

KBUILD_OPTIONS += CONFIG_AOC_DRIVER=m CONFIG_WC_MBOX=m

EXTRA_CFLAGS=-I$(KERNEL_SRC)/../google-modules/aoc_ipc

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(KBUILD_OPTIONS) $(@)
