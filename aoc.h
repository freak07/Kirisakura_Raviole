/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
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
#include "uapi/aoc.h"

#ifdef __KERNEL__

struct aoc_service_dev {
	struct device dev;
	int service_index;
	aoc_service *service;
	void *ipc_base;
	bool dead;
};

#define AOC_DEVICE(_d) container_of((_d), struct aoc_service_dev, dev)

ssize_t aoc_service_read(struct aoc_service_dev *dev, uint8_t *buffer,
			 size_t count, bool block);
ssize_t aoc_service_write(struct aoc_service_dev *dev, const uint8_t *buffer,
			  size_t count, bool block);
int aoc_service_can_read(struct aoc_service_dev *dev);
int aoc_service_can_write(struct aoc_service_dev *dev);
void aoc_service_set_read_blocked(struct aoc_service_dev *dev);
void aoc_service_set_write_blocked(struct aoc_service_dev *dev);
wait_queue_head_t *aoc_service_get_read_queue(struct aoc_service_dev *dev);
wait_queue_head_t *aoc_service_get_write_queue(struct aoc_service_dev *dev);

struct aoc_driver {
	struct device_driver drv;

	/* Array of service names to match against.  Last entry must be NULL */
	const char * const *service_names;
	int (*probe)(struct aoc_service_dev *dev);
	int (*remove)(struct aoc_service_dev *dev);
};
#define AOC_DRIVER(_d) container_of((_d), struct aoc_driver, drv)

int aoc_driver_register(struct aoc_driver *driver);
void aoc_driver_unregister(struct aoc_driver *driver);

typedef int (*aoc_map_handler)(u32 handle, phys_addr_t p, size_t size,
				bool mapped, void *ctx);
void aoc_set_map_handler(struct aoc_service_dev *dev, aoc_map_handler handler,
			 void *ctx);
void aoc_remove_map_handler(struct aoc_service_dev *dev);

#define AOC_SERVICE_NAME_LENGTH 32

/* Rings should have the ring flag set, slots = 1, size = ring size
 * tx/rx stats for rings are measured in bytes, otherwise msg sends
 */
#define AOC_MAX_ENDPOINTS 64
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

#define AOC_PARAMETER_MAGIC 0x0a0cda7a
enum AOC_FIRMWARE_INFORMATION {
	kAOCBoardID = 0x1001,
	kAOCBoardRevision = 0x1002,
	kAOCSRAMRepaired = 0x1003,
};

#define module_aoc_driver(__aoc_driver)                                        \
	module_driver(__aoc_driver, aoc_driver_register, aoc_driver_unregister)

#endif /* __KERNEL__ */
