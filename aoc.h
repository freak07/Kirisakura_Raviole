// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC Core Driver
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/sizes.h>
#include "aoc_ipc_core.h"

struct aoc_service_dev {
	struct device dev;
	const char *name;
	int service_index;
	aoc_service *service;
	void *ipc_base;
};

#define AOC_DEVICE(_d) container_of((_d), struct aoc_service_dev, dev)

ssize_t aoc_service_read(struct aoc_service_dev *dev, uint8_t *buffer,
			 size_t count, bool block);
ssize_t aoc_service_write(struct aoc_service_dev *dev, const uint8_t *buffer,
			  size_t count, bool block);

struct aoc_driver {
	struct device_driver drv;

	/* Array of service names to match against.  Last entry must be NULL */
	const char **service_names;
	int (*probe)(struct aoc_service_dev *);
	int (*remove)(struct aoc_service_dev *);
};
#define AOC_DRIVER(_d) container_of((_d), struct aoc_driver, drv)

int aoc_driver_register(struct aoc_driver *driver);
void aoc_driver_unregister(struct aoc_driver *driver);

#define AOC_IOCTL_MAGIC 0xac
#define AOC_SERVICE_NAME_LENGTH 32

#define AOC_SET_ENDPOINT _IOW(AOC_IOCTL_MAGIC, 3, int)
#define AOC_RELEASE_ENDPOINT _IOW(AOC_IOCTL_MAGIC, 4, int)
#define AOC_IS_ONLINE _IOR(AOC_IOCTL_MAGIC, 5, int)

#define AOC_START_FIRMWARE_DOWNLOAD _IOW(AOC_IOCTL_MAGIC, 200, int)
#define AOC_COMMIT_FIRMWARE_DOWNLOAD _IOW(AOC_IOCTL_MAGIC, 201, int)
#define AOC_ABORT_FIRMWARE_DOWNLOAD _IOW(AOC_IOCTL_MAGIC, 202, int)
#define AOC_FPGA_RESET _IOW(AOC_IOCTL_MAGIC, 203, int)

#ifdef __KERNEL__

/* Rings should have the ring flag set, slots = 1, size = ring size
 * tx/rx stats for rings are measured in bytes, otherwise msg sends */
#define AOC_MAX_ENDPOINTS 32
#define AOC_ENDPOINT_NONE 0xffffffff

/* Offset from the beginning of the DRAM region for the firmware to be stored */
#define AOC_CHARDEV_NAME "aoc"

#define AOC_DOWNCALL_DOORBELL 12

#define AOC_GPIO_BASE 0xB70000

#define AOC_PCU_BASE 0xB00000
#define AOC_PCU_DB_SET_OFFSET 0xD004
#define AOC_PCU_DB_CLR_OFFSET 0xD008
#define AOC_PCU_REVISION_OFFSET 0xF000

#define AOC_BINARY_DRAM_BASE 0x98000000
#define AOC_BINARY_LOAD_ADDRESS 0x98000000
#define AOC_BINARY_DRAM_OFFSET (AOC_BINARY_LOAD_ADDRESS - AOC_BINARY_DRAM_BASE)

#endif /* __KERNEL__ */
