#
# ASoC-based USB driver
#
obj-$(CONFIG_AOC_DRIVER) += aoc_usb_driver.o
aoc_usb_driver-objs := aoc_usb_dev.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)
KBUILD_OPTIONS += CONFIG_AOC_DRIVER=m
EXTRA_CFLAGS=-I$(KERNEL_SRC)/../google-modules/aoc_ipc -I$(KERNEL_SRC)/../google-modules/aoc

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(KBUILD_OPTIONS) $(EXTRA_CFLAGS) KBUILD_EXTRA_SYMBOLS=$(OUT_DIR)/../google-modules/aoc/Module.symvers $(@)
