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

#define pr_fmt(fmt) "aoc: " fmt

#include "aoc.h"

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/glob.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_data/sscoredump.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <soc/google/acpm_ipc_ctrl.h>

#include "ion_physical_heap.h"

#include "aoc_firmware.h"
#include "aoc_ipc_core.h"
#include "aoc_ramdump_regions.h"

/* TODO: Remove internal calls, or promote to "public" */
#include "aoc_ipc_core_internal.h"

#define MAX_FIRMWARE_LENGTH 128
#define AOC_S2MPU_CTRL0 0x0
#define AOC_PCU_RESET_CONTROL 0x0
#define AOC_PCU_RESET_CONTROL_RESET_VALUE 0x0

static struct platform_device *aoc_platform_device;
static struct device *aoc_device;
static struct class *aoc_class;

static int aoc_major_dev;
static bool aoc_online;

struct aoc_prvdata {
	struct mbox_client mbox_client;
	struct work_struct online_work;
	struct resource dram_resource;
	struct ion_heap *sensor_heap;
	aoc_map_handler map_handler;
	void *map_handler_ctx;

	struct mbox_chan *mbox_channel;
	struct device *dev;
	struct iommu_domain *domain;
	void *ipc_base;

	void *sram_virt;
	void *dram_virt;
	void *aoc_req_virt;
	void *aoc_s2mpu_virt;
	size_t sram_size;
	size_t dram_size;
	size_t aoc_req_size;
	u32 aoc_s2mpu_saved_value;

	int watchdog_irq;
	struct work_struct watchdog_work;
	bool aoc_reset_done;
	wait_queue_head_t aoc_reset_wait_queue;
	unsigned int acpm_async_id;

	char firmware_name[MAX_FIRMWARE_LENGTH];
};

/* TODO: Reduce the global variables (move into a driver structure) */
/* Resources found from the device tree */
static struct resource *aoc_sram_resource;

struct sscd_info {
	char *name;
	struct sscd_segment segs[256];
	u16 seg_count;
};

static void sscd_release(struct device *dev);

static struct sscd_info sscd_info;
static struct sscd_platform_data sscd_pdata;
static struct platform_device sscd_dev = { .name = "aoc",
					   .driver_override = SSCD_NAME,
					   .id = -1,
					   .dev = {
						   .platform_data = &sscd_pdata,
						   .release = sscd_release,
					   } };

static void *aoc_sram_virt_mapping;
static void *aoc_dram_virt_mapping;

static int aoc_irq;

static struct aoc_control_block *aoc_control;

static int aoc_major;

static const char *default_firmware = "aoc.bin";
static bool aoc_autoload_firmware;
module_param(aoc_autoload_firmware, bool, 0644);
MODULE_PARM_DESC(aoc_autoload_firmware, "Automatically load firmware if true");

static int aoc_bus_match(struct device *dev, struct device_driver *drv);
static int aoc_bus_probe(struct device *dev);
static int aoc_bus_remove(struct device *dev);

static struct bus_type aoc_bus_type = {
	.name = "aoc",
	.match = aoc_bus_match,
	.probe = aoc_bus_probe,
	.remove = aoc_bus_remove,
};

struct aoc_client {
	int client_id;
	int endpoint;
};

struct aoc_service_metadata {
	wait_queue_head_t read_queue;
	wait_queue_head_t write_queue;
};

static unsigned long read_blocked_mask;
static unsigned long write_blocked_mask;
static struct aoc_service_metadata *metadata;

static bool aoc_fpga_reset(struct aoc_prvdata *prvdata);
static bool write_reset_trampoline(u32 addr);
static bool aoc_a32_reset(void);
static int aoc_watchdog_restart(struct aoc_prvdata *prvdata);
static void acpm_aoc_reset_callback(unsigned int *cmd, unsigned int size);

static int start_firmware_load(struct device *dev);
static void aoc_take_offline(struct aoc_prvdata *prvdata);
static void signal_aoc(struct mbox_chan *channel);

static void aoc_process_services(struct aoc_prvdata *prvdata);

static irqreturn_t watchdog_int_handler(int irq, void *dev);
static void aoc_watchdog(struct work_struct *work);

static inline void *aoc_sram_translate(u32 offset)
{
	BUG_ON(aoc_sram_virt_mapping == NULL);
	if (offset > resource_size(aoc_sram_resource))
		return NULL;

	return aoc_sram_virt_mapping + offset;
}

static inline void *aoc_dram_translate(struct aoc_prvdata *p, u32 offset)
{
	BUG_ON(p->dram_virt == NULL);
	if (offset > p->dram_size)
		return NULL;

	return p->dram_virt + offset;
}

static bool aoc_is_valid_dram_address(struct aoc_prvdata *prv, void *addr)
{
	ptrdiff_t offset;

	if (addr < prv->dram_virt)
		return false;

	offset = addr - prv->dram_virt;
	return (offset < prv->dram_size);
}

static inline phys_addr_t aoc_dram_translate_to_aoc(struct aoc_prvdata *p,
					    phys_addr_t addr)
{
	phys_addr_t phys_start = p->dram_resource.start;
	phys_addr_t phys_end = phys_start + resource_size(&p->dram_resource);
	u32 offset;

	if (addr < phys_start || addr >= phys_end)
		return 0;

	offset = addr - phys_start;
	return AOC_BINARY_DRAM_BASE + offset;
}

static inline bool aoc_is_online(void)
{
	return aoc_control != NULL && aoc_control->magic == AOC_MAGIC;
}

static inline int aoc_num_services(void)
{
	return aoc_is_online() ? le32_to_cpu(aoc_control->services) : 0;
}

static inline aoc_service *service_at_index(struct aoc_prvdata *prvdata,
					    int index)
{
	if (!aoc_is_online() || index > aoc_num_services())
		return NULL;

	return (((uint8_t *)prvdata->ipc_base) + aoc_control->services_offset +
		(le32_to_cpu(aoc_control->service_size) * index));
}

static bool validate_service(struct aoc_prvdata *prv, int i)
{
	struct aoc_ipc_service_header *hdr = service_at_index(prv, i);
	struct device *dev = prv->dev;

	if (!aoc_is_valid_dram_address(prv, hdr)) {
		dev_err(dev, "service %d is not in DRAM region\n", i);
		return false;
	}

	if (hdr->regions[0].slots == 0 && hdr->regions[1].slots == 0) {
		dev_err(dev, "service %d is not readable or writable\n", i);

		return false;
	}

	if (aoc_service_is_ring(hdr) &&
	    (hdr->regions[0].slots > 1 || hdr->regions[1].slots > 1)) {
		dev_err(dev, "service %d has invalid ring slot configuration\n",
			i);

		return false;
	}

	return true;
}

static int driver_matches_service_by_name(struct device_driver *drv, void *name)
{
	struct aoc_driver *aoc_drv = AOC_DRIVER(drv);
	const char *service_name = name;
	const char *const *driver_names = aoc_drv->service_names;

	while (driver_names && *driver_names) {
		if (glob_match(*driver_names, service_name) == true)
			return 1;

		driver_names++;
	}

	return 0;
}

static bool has_name_matching_driver(const char *service_name)
{
	return bus_for_each_drv(&aoc_bus_type, NULL, (char *)service_name,
				driver_matches_service_by_name) != 0;
}

static bool service_names_are_valid(struct aoc_prvdata *prv)
{
	int services, i, j;

	services = aoc_num_services();
	if (services == 0)
		return false;

	/* All names have a valid length */
	for (i = 0; i < services; i++) {
		const char *name = aoc_service_name(service_at_index(prv, i));
		size_t name_len;

		if (!name) {
			dev_err(prv->dev,
				"failed to retrieve service name for service %d\n",
				i);
			return false;
		}

		name_len = strnlen(name, AOC_SERVICE_NAME_LENGTH);
		if (name_len == 0 || name_len == AOC_SERVICE_NAME_LENGTH) {
			dev_err(prv->dev,
				"service %d has a name that is too long\n", i);
			return false;
		}

		dev_dbg(prv->dev, "validated service %d name %s\n", i, name);
	}

	/* No duplicate names */
	for (i = 0; i < services; i++) {
		char name1[AOC_SERVICE_NAME_LENGTH],
			name2[AOC_SERVICE_NAME_LENGTH];
		memcpy_fromio(name1, aoc_service_name(service_at_index(prv, i)),
			      sizeof(name1));

		for (j = i + 1; j < services; j++) {
			memcpy_fromio(
				name2,
				aoc_service_name(service_at_index(prv, j)),
				sizeof(name2));

			if (strncmp(name1, name2, AOC_SERVICE_NAME_LENGTH) ==
			    0) {
				dev_err(prv->dev,
					"service %d and service %d have the same name\n",
					i, j);
				return false;
			}
		}
	}

	return true;
}

static void aoc_mbox_rx_callback(struct mbox_client *cl, void *mssg)
{
	struct aoc_prvdata *prvdata =
		container_of(cl, struct aoc_prvdata, mbox_client);

	/* Transitioning from offline to online */
	if (aoc_online == false && aoc_is_online()) {
		aoc_online = true;
		schedule_work(&prvdata->online_work);
	} else {
		aoc_process_services(prvdata);
	}
}

static void aoc_mbox_tx_prepare(struct mbox_client *cl, void *mssg)
{
}

static void aoc_mbox_tx_done(struct mbox_client *cl, void *mssg, int r)
{
}

static void aoc_req_assert(struct aoc_prvdata *p, bool assert)
{
	iowrite32(!!assert, p->aoc_req_virt);
}

extern int gs_chipid_get_ap_hw_tune_array(const u8 **array);

static bool aoc_sram_was_repaired(struct aoc_prvdata *prvdata)
{
	const u8 *array;
	struct device *dev = prvdata->dev;
	int ret;

	ret = gs_chipid_get_ap_hw_tune_array(&array);

	if (ret == -EPROBE_DEFER) {
		dev_err(dev, "Unable to determine SRAM repair state.  Leaving monitor mode disabled\n");
		return false;
	}

	if (ret != 32) {
		dev_err(dev, "Unexpected hw_tune_array size.  Leaving monitor mode disabled\n");
		return false;
	}

	/* Bit 65 says that AoC SRAM was repaired */
	return ((array[8] & 0x2) != 0);
}

struct aoc_fw_data {
	u32 key;
	u32 value;
};

static u32 dt_property(struct device_node *node, const char *key)
{
	u32 ret;

	if (of_property_read_u32(node, key, &ret))
		return 0xffffffff;

	return ret;
}

static void aoc_pass_fw_information(void *base, const struct aoc_fw_data *fwd,
				    size_t num)
{
	u32 *data = base;
	int i;

	writel_relaxed(AOC_PARAMETER_MAGIC, data++);
	writel_relaxed(num, data++);
	writel_relaxed(12 + (num * (3 * sizeof(u32))), data++);

	for (i = 0; i < num; i++) {
		writel_relaxed(fwd[i].key, data++);
		writel_relaxed(sizeof(u32), data++);
		writel_relaxed(fwd[i].value, data++);
	}
}

static void aoc_fw_callback(const struct firmware *fw, void *ctx)
{
	struct device *dev = ctx;
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);
	u32 board_id = dt_property(prvdata->dev->of_node, "board_id");
	u32 board_rev = dt_property(prvdata->dev->of_node, "board_rev");
	u32 sram_was_repaired = aoc_sram_was_repaired(prvdata);
	struct aoc_fw_data fw_data[] = {
		{ .key = kAOCBoardID, .value = board_id },
		{ .key = kAOCBoardRevision, .value = board_rev },
		{ .key = kAOCSRAMRepaired, .value = sram_was_repaired } };
	const char *version;

	u32 ipc_offset, bootloader_offset;

	aoc_req_assert(prvdata, true);

	if (!fw || !fw->data) {
		dev_err(dev, "failed to load firmware image\n");
		goto free_fw;
	}

	if (!_aoc_fw_is_valid(fw)) {
		dev_err(dev, "firmware validation failed\n");
		goto free_fw;
	}

	ipc_offset = _aoc_fw_ipc_offset(fw);
	bootloader_offset = _aoc_fw_bootloader_offset(fw);
	version = _aoc_fw_version(fw);

	pr_notice("successfully loaded firmware version %s type %s",
		  version ? version : "unknown",
		  _aoc_fw_is_release(fw) ? "release" : "development");

	if (sram_was_repaired)
		dev_err(dev, "SRAM was repaired on this device.  Stability/power will be impacted\n");

	if (!_aoc_fw_is_compatible(fw)) {
		dev_err(dev, "firmware and drivers are incompatible\n");
		goto free_fw;
	}

	aoc_control = aoc_dram_translate(prvdata, ipc_offset);

	aoc_fpga_reset(prvdata);

	_aoc_fw_commit(fw, aoc_dram_virt_mapping + AOC_BINARY_DRAM_OFFSET);

	aoc_pass_fw_information(aoc_dram_translate(prvdata, ipc_offset),
				fw_data, ARRAY_SIZE(fw_data));

	write_reset_trampoline(AOC_BINARY_LOAD_ADDRESS + bootloader_offset);

	aoc_a32_reset();

	prvdata->ipc_base = aoc_dram_translate(prvdata, ipc_offset);
free_fw:
	release_firmware(fw);
}

ssize_t aoc_service_read(struct aoc_service_dev *dev, uint8_t *buffer,
			 size_t count, bool block)
{
	const struct device *parent;
	struct aoc_prvdata *prvdata;
	aoc_service *service;

	size_t msg_size;
	int service_number;
	int ret = 0;

	if (!dev || !buffer || !count)
		return -EINVAL;

	if (dev->dead)
		return -ENODEV;

	if (!aoc_online)
		return -ENODEV;

	parent = dev->dev.parent;
	prvdata = dev_get_drvdata(parent);

	service_number = dev->service_index;
	service = service_at_index(prvdata, dev->service_index);

	BUG_ON(!aoc_is_valid_dram_address(prvdata, service));

	if (aoc_service_message_slots(service, AOC_UP) == 0)
		return -EBADF;

	if (!aoc_service_can_read_message(service, AOC_UP)) {
		if (!block)
			return -EAGAIN;

		set_bit(service_number, &read_blocked_mask);
		ret = wait_event_interruptible(
			metadata[service_number].read_queue,
			!aoc_online || dev->dead ||
				aoc_service_can_read_message(service, AOC_UP));
		clear_bit(service_number, &read_blocked_mask);
	}

	if (dev->dead)
		return -ENODEV;

	if (!aoc_online)
		return -ENODEV;

	/*
	 * The wait can fail if the AoC goes offline in the middle of a
	 * blocking read, so check again after the wait
	 */
	if (ret != 0)
		return -EAGAIN;

	if (!aoc_service_is_ring(service) &&
	    count < aoc_service_current_message_size(service, prvdata->ipc_base,
						     AOC_UP))
		return -EFBIG;

	msg_size = count;
	aoc_service_read_message(service, prvdata->ipc_base, AOC_UP, buffer,
				 &msg_size);

	return msg_size;
}
EXPORT_SYMBOL(aoc_service_read);

ssize_t aoc_service_write(struct aoc_service_dev *dev, const uint8_t *buffer,
			  size_t count, bool block)
{
	const struct device *parent;
	struct aoc_prvdata *prvdata;

	aoc_service *service;
	int service_number;
	int ret = 0;

	if (!dev || !buffer || !count)
		return -EINVAL;

	if (dev->dead)
		return -ENODEV;

	if (!aoc_online)
		return -ENODEV;

	parent = dev->dev.parent;
	prvdata = dev_get_drvdata(parent);

	service_number = dev->service_index;
	service = service_at_index(prvdata, service_number);

	BUG_ON(!aoc_is_valid_dram_address(prvdata, service));

	if (aoc_service_message_slots(service, AOC_DOWN) == 0)
		return -EBADF;

	if (count > aoc_service_message_size(service, AOC_DOWN))
		return -EFBIG;

	if (!aoc_service_can_write_message(service, AOC_DOWN)) {
		if (!block)
			return -EAGAIN;

		set_bit(service_number, &write_blocked_mask);
		ret = wait_event_interruptible(
			metadata[service_number].write_queue,
			!aoc_online || dev->dead ||
				aoc_service_can_write_message(service, AOC_DOWN));
		clear_bit(service_number, &write_blocked_mask);
	}

	if (dev->dead)
		return -ENODEV;

	if (!aoc_online)
		return -ENODEV;

	/*
	 * The wait can fail if the AoC goes offline in the middle of a
	 * blocking write, so check again after the wait
	 */
	if (ret != 0)
		return -EAGAIN;

	ret = aoc_service_write_message(service, prvdata->ipc_base, AOC_DOWN,
					buffer, count);

	if (!aoc_service_is_ring(service) || aoc_ring_is_push(service))
		signal_aoc(prvdata->mbox_channel);

	return count;
}
EXPORT_SYMBOL(aoc_service_write);

int aoc_service_can_read(struct aoc_service_dev *dev)
{
	const struct device *parent;
	struct aoc_prvdata *prvdata;
	aoc_service *service;

	parent = dev->dev.parent;
	prvdata = dev_get_drvdata(parent);
	service = service_at_index(prvdata, dev->service_index);

	if (aoc_service_message_slots(service, AOC_UP) == 0)
		return 0;

	return aoc_service_can_read_message(service, AOC_UP);
}
EXPORT_SYMBOL_GPL(aoc_service_can_read);

int aoc_service_can_write(struct aoc_service_dev *dev)
{
	const struct device *parent;
	struct aoc_prvdata *prvdata;
	aoc_service *service;

	parent = dev->dev.parent;
	prvdata = dev_get_drvdata(parent);
	service = service_at_index(prvdata, dev->service_index);

	if (aoc_service_message_slots(service, AOC_DOWN) == 0)
		return 0;

	return aoc_service_can_write_message(service, AOC_DOWN);
}
EXPORT_SYMBOL_GPL(aoc_service_can_write);

void aoc_service_set_read_blocked(struct aoc_service_dev *dev)
{
	int service_number;

	service_number = dev->service_index;
	set_bit(service_number, &read_blocked_mask);
}
EXPORT_SYMBOL_GPL(aoc_service_set_read_blocked);

void aoc_service_set_write_blocked(struct aoc_service_dev *dev)
{
	int service_number;

	service_number = dev->service_index;
	set_bit(service_number, &write_blocked_mask);
}
EXPORT_SYMBOL_GPL(aoc_service_set_write_blocked);

wait_queue_head_t *aoc_service_get_read_queue(struct aoc_service_dev *dev)
{
	int service_number;

	service_number = dev->service_index;
	return &metadata[service_number].read_queue;
}
EXPORT_SYMBOL_GPL(aoc_service_get_read_queue);

wait_queue_head_t *aoc_service_get_write_queue(struct aoc_service_dev *dev)
{
	int service_number;

	service_number = dev->service_index;
	return &metadata[service_number].write_queue;
}
EXPORT_SYMBOL_GPL(aoc_service_get_write_queue);

static bool write_reset_trampoline(u32 addr)
{
	u32 *reset;
	u32 instructions[] = {
		0xe59f0030, /* ldr r0, .PCU_SLC_MIF_REQ_ADDR */
		0xe3a01003, /* mov r1, #3 */
		0xe5801000, /* str r1, [r0] */
		/* mif_ack_loop: */
		0xe5902004, /* ldr r2, [r0, #4] */
		0xe3520002, /* cmp r2, #2 */
		0x1afffffc, /* bne mif_ack_loop */
		0xe59f0014, /* ldr r0, .PCU_POWER_STATUS_ADDR*/
		0xe3a01004, /* mov r1, #4 */
		0xe5801004, /* str r1, [r0, #4] */
		/* blk_aoc_on_loop: */
		0xe5902000, /* ldr r2, [r0] */
		0xe3120004, /* tst r2, #4 */
		0x0afffffc, /* beq blk_aoc_on_loop */
		0xe59ff004, /* ldr pc, BOOTLOADER_START_ADDR */
		0x00b02000, /* PCU_TOP_POWER_STATUS_ADDR */
		0x00b0819c, /* PCU_SLC_MIF_REQ_ADDR */
		addr /* BOOTLOADER_START_ADDR */
	};

	pr_notice("writing reset trampoline to addr %#x\n", addr);

	reset = aoc_sram_translate(0);
	if (!reset)
		return false;

	memcpy_toio(reset, instructions, sizeof(instructions));

	return true;
}

static bool aoc_fpga_reset(struct aoc_prvdata *prvdata)
{
#ifdef AOC_JUNO
	u32 *reset = aoc_sram_translate(0x1000000);

	if (!reset)
		return false;

	aoc_take_offline(prvdata);

	/* Assert and deassert reset */
	iowrite32(0, reset);
	iowrite32(1, reset);
#endif

	return true;
}

static bool aoc_a32_reset(void)
{
	u32 pcu_value;
	u32 *pcu = aoc_sram_translate(AOC_PCU_BASE);

	if (!pcu)
		return false;

	pcu_value = ioread32(pcu);

	pcu_value |= 1;
	iowrite32(pcu_value, pcu);

	return true;
}

static int aoc_watchdog_restart(struct aoc_prvdata *prvdata)
{
	const int aoc_reset_timeout_ms = 1000;
	int rc;
	u32 *pcu;

	dev_info(prvdata->dev, "waiting for aoc reset to finish\n");
	if (wait_event_timeout(prvdata->aoc_reset_wait_queue, prvdata->aoc_reset_done,
			       aoc_reset_timeout_ms) == 0) {
		dev_err(prvdata->dev, "timed out waiting for aoc reset\n");
		return -ETIMEDOUT;
	}
	dev_info(prvdata->dev, "aoc reset finished\n");
	prvdata->aoc_reset_done = false;

	pcu = aoc_sram_translate(AOC_PCU_BASE);
	if (!pcu)
		return -ENODEV;

	if (readl(pcu + AOC_PCU_RESET_CONTROL) != AOC_PCU_RESET_CONTROL_RESET_VALUE) {
		dev_err(prvdata->dev, "aoc watchdog reset failed\n");
		return -ENODEV;
	}

	/*
	 * AOC_TZPC has been restored by ACPM, so we can access AOC_S2MPU.
	 * Restore AOC_S2MPU.
	 */
	writel(prvdata->aoc_s2mpu_saved_value, prvdata->aoc_s2mpu_virt + AOC_S2MPU_CTRL0);

	/* Restore SysMMU settings by briefly setting AoC to runtime active. Since SysMMU is a
	 * supplier to AoC, it will be set to runtime active as a side effect. */
	rc = pm_runtime_set_active(prvdata->dev);
	if (rc < 0) {
		dev_err(prvdata->dev, "sysmmu restore failed: pm_runtime_resume rc = %d\n", rc);
		return rc;
	}
	rc = pm_runtime_set_suspended(prvdata->dev);
	if (rc < 0) {
		dev_err(prvdata->dev, "sysmmu restore failed: pm_runtime_suspend rc = %d\n", rc);
		return rc;
	}

	prvdata->mbox_channel =
		mbox_request_channel_byname(&prvdata->mbox_client, "aoc2ap");
	if (IS_ERR(prvdata->mbox_channel)) {
		rc = PTR_ERR(prvdata->mbox_channel);
		dev_err(prvdata->dev, "failed to find mailbox interface : %d\n", rc);
		prvdata->mbox_channel = NULL;
		return rc;
	}

	rc = start_firmware_load(prvdata->dev);
	if (rc) {
		dev_err(prvdata->dev, "load aoc firmware failed: rc = %d\n", rc);
		return rc;
	}

	enable_irq(prvdata->watchdog_irq);
	return rc;
}

static void acpm_aoc_reset_callback(unsigned int *cmd, unsigned int size)
{
	struct aoc_prvdata *prvdata;

	if (!aoc_platform_device)
		return;

	prvdata = platform_get_drvdata(aoc_platform_device);
	prvdata->aoc_reset_done = true;
	wake_up(&prvdata->aoc_reset_wait_queue);
}

static ssize_t revision_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	u32 fw_rev, hw_rev;

	if (!aoc_is_online())
		return scnprintf(buf, PAGE_SIZE, "Offline\n");

	fw_rev = le32_to_cpu(aoc_control->fw_version);
	hw_rev = le32_to_cpu(aoc_control->hw_version);
	return scnprintf(buf, PAGE_SIZE,
			 "FW Revision : %#x\nHW Revision : %#x\n", fw_rev,
			 hw_rev);
}

static DEVICE_ATTR_RO(revision);

static uint64_t clock_offset(void)
{
	u64 clock_offset;

	if (!aoc_is_online())
		return 0;

	memcpy_fromio(&clock_offset, &aoc_control->system_clock_offset,
		      sizeof(clock_offset));

	return le64_to_cpu(clock_offset);
}

static inline u64 sys_tick_to_aoc_tick(u64 sys_tick)
{
	return (sys_tick - clock_offset()) / 6;
}

static ssize_t aoc_clock_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	u64 counter;

	if (!aoc_is_online())
		return scnprintf(buf, PAGE_SIZE, "0\n");

	counter = arch_timer_read_counter();

	return scnprintf(buf, PAGE_SIZE, "%llu\n",
			 sys_tick_to_aoc_tick(counter));
}

static DEVICE_ATTR_RO(aoc_clock);

static ssize_t aoc_clock_and_kernel_boottime_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	u64 counter;
	ktime_t kboottime;

	if (!aoc_is_online())
		return scnprintf(buf, PAGE_SIZE, "0 0\n");

	counter = arch_timer_read_counter();
	kboottime = ktime_get_boottime();

	return scnprintf(buf, PAGE_SIZE, "%llu %llu\n",
			 sys_tick_to_aoc_tick(counter), (u64)kboottime);
}

static DEVICE_ATTR_RO(aoc_clock_and_kernel_boottime);

static ssize_t clock_offset_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	if (!aoc_is_online())
		return scnprintf(buf, PAGE_SIZE, "0\n");

	return scnprintf(buf, PAGE_SIZE, "%lld\n", clock_offset());
}

static DEVICE_ATTR_RO(clock_offset);

static ssize_t services_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);
	int services = aoc_num_services();
	int ret = 0;
	int i;

	ret += scnprintf(buf, PAGE_SIZE, "Services : %d\n", services);
	for (i = 0; i < services && ret < (PAGE_SIZE - 1); i++) {
		aoc_service *s = service_at_index(prvdata, i);
		struct aoc_ipc_service_header *hdr =
			(struct aoc_ipc_service_header *)s;

		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%d : name %s\n",
				 i, aoc_service_name(s));
		if (hdr->regions[0].slots > 0) {
			ret += scnprintf(
				buf + ret, PAGE_SIZE - ret,
				"  Up   - Slots:%u Size:%u Tx:%u Rx:%u\n",
				hdr->regions[0].slots, hdr->regions[0].size,
				hdr->regions[0].tx, hdr->regions[0].rx);
		}

		if (hdr->regions[1].slots > 0) {
			ret += scnprintf(
				buf + ret, PAGE_SIZE - ret,
				"  Down - Slots:%u Size:%u Tx:%u Rx:%u\n",
				hdr->regions[1].slots, hdr->regions[1].size,
				hdr->regions[1].tx, hdr->regions[1].rx);
		}
	}

	return ret;
}

static DEVICE_ATTR_RO(services);

static int start_firmware_load(struct device *dev)
{
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);

	dev_notice(dev, "attempting to load firmware \"%s\"\n",
		   prvdata->firmware_name);
	return request_firmware_nowait(THIS_MODULE, true,
				       prvdata->firmware_name, dev, GFP_KERNEL,
				       dev, aoc_fw_callback);
}

static ssize_t firmware_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%s", prvdata->firmware_name);
}

static ssize_t firmware_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);
	char buffer[MAX_FIRMWARE_LENGTH];
	char *trimmed = NULL;

	if (strscpy(buffer, buf, sizeof(buffer)) <= 0)
		return -E2BIG;

	if (strchr(buffer, '/') != NULL) {
		dev_err(dev, "firmware path must not contain '/'\n");
		return -EINVAL;
	}

	/* Strip whitespace (including \n) */
	trimmed = strim(buffer);

	strscpy(prvdata->firmware_name, trimmed,
		sizeof(prvdata->firmware_name));
	start_firmware_load(dev);

	return count;
}

static DEVICE_ATTR_RW(firmware);

static struct attribute *aoc_attrs[] = {
	&dev_attr_firmware.attr,
	&dev_attr_revision.attr,
	&dev_attr_services.attr,
	&dev_attr_clock_offset.attr,
	&dev_attr_aoc_clock.attr,
	&dev_attr_aoc_clock_and_kernel_boottime.attr,
	NULL
};

ATTRIBUTE_GROUPS(aoc);

static int aoc_platform_probe(struct platform_device *dev);
static int aoc_platform_remove(struct platform_device *dev);
static void aoc_platform_shutdown(struct platform_device *dev);

static const struct of_device_id aoc_match[] = {
	{
		.compatible = "google,aoc",
	},
	{},
};

static struct platform_driver aoc_driver = {
	.probe = aoc_platform_probe,
	.remove = aoc_platform_remove,
	.shutdown = aoc_platform_shutdown,
	.driver = {
			.name = "aoc",
			.owner = THIS_MODULE,
			.of_match_table = of_match_ptr(aoc_match),
		},
};

static int aoc_bus_match(struct device *dev, struct device_driver *drv)
{
	struct aoc_service_dev *device = AOC_DEVICE(dev);
	struct aoc_driver *driver = AOC_DRIVER(drv);
	struct device *aoc = dev->parent;
	struct aoc_prvdata *prvdata = dev_get_drvdata(aoc);

	aoc_service *s = service_at_index(prvdata, device->service_index);
	struct aoc_ipc_service_header *header =
		(struct aoc_ipc_service_header *)s;
	const char *device_name = dev_name(dev);
	bool driver_matches_by_name = (driver->service_names != NULL);
	const char *service_name = header->name;

	pr_debug("bus match dev:%s drv:%s\n", device_name, drv->name);

	/*
	 * If the drviver matches by name, only call probe if the name matches.
	 *
	 * If there is a specific driver matching this service, do not allow a
	 * generic driver to claim the service
	 */
	if (!driver_matches_by_name && has_name_matching_driver(service_name)) {
		pr_debug("ignoring generic driver for service %s\n",
			 service_name);
		return 0;
	}

	/* Drivers with a name only match services with that name */
	if (driver_matches_by_name &&
	    !driver_matches_service_by_name(drv, (char *)service_name)) {
		return 0;
	}

	return 1;
}

static int aoc_bus_probe(struct device *dev)
{
	struct aoc_service_dev *the_dev = AOC_DEVICE(dev);
	struct aoc_driver *driver = AOC_DRIVER(dev->driver);

	pr_debug("bus probe dev:%s\n", dev_name(dev));
	if (!driver->probe)
		return -ENODEV;

	return driver->probe(the_dev);
}

static int aoc_bus_remove(struct device *dev)
{
	struct aoc_service_dev *aoc_dev = AOC_DEVICE(dev);
	struct aoc_driver *drv = AOC_DRIVER(dev->driver);
	int ret = -EINVAL;

	pr_notice("bus remove %s\n", dev_name(dev));

	if (drv->remove)
		ret = drv->remove(aoc_dev);

	return ret;
}

int aoc_driver_register(struct aoc_driver *driver)
{
	driver->drv.bus = &aoc_bus_type;
	return driver_register(&driver->drv);
}
EXPORT_SYMBOL(aoc_driver_register);

void aoc_driver_unregister(struct aoc_driver *driver)
{
	driver_unregister(&driver->drv);
}
EXPORT_SYMBOL(aoc_driver_unregister);

static void aoc_clear_gpio_interrupt(void)
{
#ifndef AOC_JUNO
	int reg = 93, val;
	u32 *gpio_register =
		aoc_sram_translate(AOC_GPIO_BASE + ((reg / 32) * 12));

	val = ioread32(gpio_register);
	val &= ~(1 << (reg % 32));
	iowrite32(val, gpio_register);
#endif
}

static void aoc_configure_interrupt(void)
{
	aoc_clear_gpio_interrupt();
}

static int aoc_remove_device(struct device *dev, void *ctx)
{
	struct aoc_service_dev *the_dev = AOC_DEVICE(dev);
	int service_number;

	/*
	 * Once dead is set to true, function calls using this AoC device will return error.
	 * Clients may still hold a refcount on the AoC device, so freeing is delayed.
	 */
	the_dev->dead = true;
	// Allow any pending reads and writes to finish before removing devices
	service_number = the_dev->service_index;
	wake_up(&metadata[service_number].read_queue);
	wake_up(&metadata[service_number].write_queue);

	device_unregister(dev);

	return 0;
}

static void aoc_device_release(struct device *dev)
{
	struct aoc_service_dev *the_dev = AOC_DEVICE(dev);

	pr_debug("%s %s\n", __func__, dev_name(dev));

	kfree(the_dev);
}

static struct aoc_service_dev *register_service_device(int index,
						       struct device *parent)
{
	struct aoc_prvdata *prv;
	char service_name[32];
	aoc_service *s;
	struct aoc_service_dev *dev;
	int ret;

	prv = dev_get_drvdata(parent);
	s = service_at_index(prv, index);
	if (!s)
		return NULL;

	dev = kzalloc(sizeof(struct aoc_service_dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	memcpy_fromio(service_name, aoc_service_name(s), sizeof(service_name));

	dev_set_name(&dev->dev, "%s", service_name);
	dev->dev.parent = parent;
	dev->dev.bus = &aoc_bus_type;
	dev->dev.release = aoc_device_release;

	dev->service_index = index;
	dev->service = s;
	dev->ipc_base = prv->ipc_base;
	dev->dead = false;

	/*
	 * Bus corruption has been seen during reboot cycling.  Check for it
	 * explictly so more information can be part of the panic log
	 */
	if (aoc_bus_type.p == NULL) {
		panic("corrupted bus found when adding service (%d) %s\n",
		      index, dev_name(&dev->dev));
	}

	ret = device_register(&dev->dev);
	if (ret) {
		kfree(dev);
		dev = NULL;
	}

	return dev;
}

static void signal_aoc(struct mbox_chan *channel)
{
#ifdef AOC_JUNO
	(void)channel;

	u32 mask = (1 << AOC_DOWNCALL_DOORBELL);

	/* The signal is called as directly after writing a message to shared
	 * memory, so make sure all pending writes are flushed before actually
	 * sending the signal
	 */
	wmb();
	iowrite32(mask,
		  aoc_sram_translate(AOC_PCU_BASE + AOC_PCU_DB_SET_OFFSET));
#else
	mbox_send_message(channel, NULL);
#endif
}

static int aoc_iommu_fault_handler(struct iommu_domain *domain,
				   struct device *dev, unsigned long iova,
				   int flags, void *token)
{
	dev_err(dev, "iommu fault at aoc address %#010lx, flags %#010x\n", iova,
		flags);

	return 0;
}

static void aoc_configure_sysmmu(struct aoc_prvdata *p)
{
#ifndef AOC_JUNO
	struct iommu_domain *domain = p->domain;
	struct device *dev = p->dev;

	iommu_set_fault_handler(domain, aoc_iommu_fault_handler, NULL);

	/* Map in the AoC carveout */
	if (iommu_map(domain, 0x98000000, p->dram_resource.start, p->dram_size,
		      IOMMU_READ | IOMMU_WRITE))
		dev_err(dev, "mapping carveout failed\n");

	/* Use a 1MB mapping instead of individual mailboxes for now */
	/* TODO: Turn the mailbox address ranges into dtb entries */
	if (iommu_map(domain, 0x9A000000, 0x17600000, SZ_1M,
		      IOMMU_READ | IOMMU_WRITE))
		dev_err(dev, "mapping mailboxes failed\n");

	/* Map in GSA mailbox */
	if (iommu_map(domain, 0x9A100000, 0x17C00000, SZ_1M,
		      IOMMU_READ | IOMMU_WRITE))
		dev_err(dev, "mapping gsa mailbox failed\n");

	/* Map in USB for low power audio */
	if (iommu_map(domain, 0x9A200000, 0x11100000, SZ_1M,
		      IOMMU_READ | IOMMU_WRITE))
		dev_err(dev, "mapping usb failed\n");

	/* Map in modem registers */
	if (iommu_map(domain, 0x9A300000, 0x40000000, SZ_1M,
		      IOMMU_READ | IOMMU_WRITE))
		dev_err(dev, "mapping modem failed\n");
#endif
}

static void aoc_clear_sysmmu(struct aoc_prvdata *p)
{
#ifndef AOC_JUNO
	struct iommu_domain *domain = p->domain;

	/* Memory carveout */
	iommu_unmap(domain, 0x98000000, p->dram_size);

	/* Device registers */
	iommu_unmap(domain, 0x9A000000, SZ_1M);
	iommu_unmap(domain, 0x9A100000, SZ_1M);
	iommu_unmap(domain, 0x9A200000, SZ_1M);
	iommu_unmap(domain, 0x9A300000, SZ_1M);
#endif
}

static void aoc_did_become_online(struct work_struct *work)
{
	struct aoc_prvdata *prvdata =
		container_of(work, struct aoc_prvdata, online_work);
	struct device *dev = prvdata->dev;
	int i, s;

	s = aoc_num_services();

	aoc_req_assert(prvdata, false);

	pr_notice("firmware version %u did become online with %d services\n",
		  le32_to_cpu(aoc_control->fw_version), aoc_num_services());

	if (s > AOC_MAX_ENDPOINTS) {
		dev_err(dev, "Firmware supports too many (%d) services\n", s);
		return;
	}

	if (!service_names_are_valid(prvdata)) {
		pr_err("invalid service names found.  Ignoring\n");
		return;
	}

	metadata = kmalloc(s * sizeof(struct aoc_service_metadata), GFP_KERNEL);
	for (i = 0; i < s; i++) {
		init_waitqueue_head(&metadata[i].read_queue);
		init_waitqueue_head(&metadata[i].write_queue);
	}

	for (i = 0; i < s; i++) {
		if (!validate_service(prvdata, i)) {
			pr_err("service %d invalid\n", i);
			continue;
		}

		register_service_device(i, prvdata->dev);
	}
}

static void aoc_take_offline(struct aoc_prvdata *prvdata)
{
	pr_notice("taking aoc offline\n");

	if (prvdata->mbox_channel) {
		mbox_free_channel(prvdata->mbox_channel);
		prvdata->mbox_channel = NULL;
	}

	aoc_online = false;

	bus_for_each_dev(&aoc_bus_type, NULL, NULL, aoc_remove_device);

	if (aoc_control)
		aoc_control->magic = 0;
}

static void aoc_process_services(struct aoc_prvdata *prvdata)
{
	int services;
	int i;

	services = aoc_num_services();
	for (i = 0; i < services; i++) {
		if (test_bit(i, &read_blocked_mask) &&
		    aoc_service_can_read_message(service_at_index(prvdata, i),
						 AOC_UP))
			wake_up(&metadata[i].read_queue);
	}

	for (i = 0; i < services; i++) {
		if (test_bit(i, &write_blocked_mask) &&
		    aoc_service_can_write_message(service_at_index(prvdata, i),
						  AOC_DOWN))
			wake_up(&metadata[i].write_queue);
	}
}

void aoc_set_map_handler(struct aoc_service_dev *dev, aoc_map_handler handler,
			 void *ctx)
{
	struct device *parent = dev->dev.parent;
	struct aoc_prvdata *prvdata = dev_get_drvdata(parent);

	prvdata->map_handler = handler;
	prvdata->map_handler_ctx = ctx;
}
EXPORT_SYMBOL(aoc_set_map_handler);

void aoc_remove_map_handler(struct aoc_service_dev *dev)
{
	struct device *parent = dev->dev.parent;
	struct aoc_prvdata *prvdata = dev_get_drvdata(parent);

	prvdata->map_handler = NULL;
	prvdata->map_handler_ctx = NULL;
}
EXPORT_SYMBOL(aoc_remove_map_handler);

static void aoc_pheap_alloc_cb(struct ion_buffer *buffer, void *ctx)
{
	struct device *dev = ctx;
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);
	struct sg_table *sg = buffer->sg_table;
	phys_addr_t phys;
	size_t size;

	if (sg->nents != 1) {
		dev_warn(dev, "Unable to map sg_table with %d ents\n",
			 sg->nents);
		return;
	}

	phys = sg_phys(&sg->sgl[0]);
	phys = aoc_dram_translate_to_aoc(prvdata, phys);
	size = sg->sgl[0].length;

	if (prvdata->map_handler) {
		prvdata->map_handler((u32)buffer->priv_virt, phys, size, true,
				     prvdata->map_handler_ctx);
	}
}

static void aoc_pheap_free_cb(struct ion_buffer *buffer, void *ctx)
{
	struct device *dev = ctx;
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);
	struct sg_table *sg = buffer->sg_table;
	phys_addr_t phys;
	size_t size;

	if (sg->nents != 1) {
		dev_warn(dev, "Unable to map sg_table with %d ents\n",
			 sg->nents);
		return;
	}

	phys = sg_phys(&sg->sgl[0]);
	phys = aoc_dram_translate_to_aoc(prvdata, phys);
	size = sg->sgl[0].length;

	if (prvdata->map_handler) {
		prvdata->map_handler((u32)buffer->priv_virt, phys, size, false,
				     prvdata->map_handler_ctx);
	}
}

#ifdef AOC_JUNO
static irqreturn_t aoc_int_handler(int irq, void *dev)
{
	aoc_clear_gpio_interrupt();

	/* Transitioning from offline to online */
	if (aoc_online == false && aoc_is_online()) {
		aoc_online = true;
		schedule_work(&aoc_online_work);
	} else {
		aoc_process_services(dev_get_drvdata(dev));
	}

	return IRQ_HANDLED;
}
#else
static irqreturn_t watchdog_int_handler(int irq, void *dev)
{
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);

	/* AP shouldn't access AoC registers to clear the IRQ. */
	/* Mask the IRQ until the IRQ gets cleared by AoC reset during SSR. */
	disable_irq_nosync(irq);
	schedule_work(&prvdata->watchdog_work);

	return IRQ_HANDLED;
}

static void aoc_watchdog(struct work_struct *work)
{
	struct aoc_prvdata *prvdata =
		container_of(work, struct aoc_prvdata, watchdog_work);

	struct aoc_ramdump_header *ramdump_header =
		(struct aoc_ramdump_header *)((unsigned long)prvdata->dram_virt +
					      RAMDUMP_HEADER_OFFSET);
	unsigned long ramdump_timeout;
	unsigned long carveout_paddr_from_aoc;
	unsigned long carveout_vaddr_from_aoc;
	size_t i;
	size_t num_pages;
	struct page **dram_pages;
	void *dram_cached;
	int sscd_retries = 20;
	const int sscd_retry_ms = 1000;
	int sscd_rc;
	char crash_info[RAMDUMP_SECTION_CRASH_INFO_SIZE];
	int restart_rc;

	dev_err(prvdata->dev, "aoc watchdog triggered, generating coredump\n");
	if (!sscd_pdata.sscd_report) {
		dev_err(prvdata->dev, "aoc coredump failed: no sscd driver\n");
		goto err_coredump;
	}

	ramdump_timeout = jiffies + (5 * HZ);
	while (time_before(jiffies, ramdump_timeout)) {
		if (ramdump_header->valid)
			break;
		msleep(100);
	}

	if (!ramdump_header->valid) {
		dev_err(prvdata->dev, "aoc coredump failed: timed out\n");
		goto err_coredump;
	}

	if (memcmp(ramdump_header, RAMDUMP_MAGIC, sizeof(RAMDUMP_MAGIC))) {
		dev_err(prvdata->dev,
			"aoc coredump failed: invalid magic (corruption or incompatible firmware?)\n");
		goto err_coredump;
	}

	num_pages = DIV_ROUND_UP(prvdata->dram_size, PAGE_SIZE);
	dram_pages = kmalloc_array(num_pages, sizeof(*dram_pages), GFP_KERNEL);
	if (!dram_pages) {
		dev_err(prvdata->dev,
			"aoc coredump failed: alloc dram_pages failed\n");
		goto err_kmalloc;
	}
	for (i = 0; i < num_pages; i++)
		dram_pages[i] = phys_to_page(prvdata->dram_resource.start +
					     (i * PAGE_SIZE));
	dram_cached = vmap(dram_pages, num_pages, VM_MAP, PAGE_KERNEL_RO);
	if (!dram_cached) {
		dev_err(prvdata->dev,
			"aoc coredump failed: vmap dram_pages failed\n");
		goto err_vmap;
	}

	sscd_info.name = "aoc";
	if (ramdump_header->sections[RAMDUMP_SECTION_CRASH_INFO_INDEX].flags & RAMDUMP_FLAG_VALID)
		strscpy(crash_info, (const char *)ramdump_header +
			RAMDUMP_SECTION_CRASH_INFO_OFFSET, RAMDUMP_SECTION_CRASH_INFO_SIZE);
	else
		strscpy(crash_info, "Unknown", RAMDUMP_SECTION_CRASH_INFO_SIZE);

	/* TODO(siqilin): Get paddr and vaddr base from firmware instead */
	carveout_paddr_from_aoc = 0x98000000;
	carveout_vaddr_from_aoc = 0x78000000;
	/* Entire AoC DRAM carveout, coredump is stored within the carveout */
	sscd_info.segs[0].addr = dram_cached;
	sscd_info.segs[0].size = prvdata->dram_size;
	sscd_info.segs[0].paddr = (void *)carveout_paddr_from_aoc;
	sscd_info.segs[0].vaddr = (void *)carveout_vaddr_from_aoc;
	sscd_info.seg_count = 1;
	/*
	 * sscd_report() returns -EAGAIN if there are no readers to consume a
	 * coredump. Retry sscd_report() with a sleep to handle the race condition
	 * where AoC crashes before the userspace daemon starts running.
	 */
	for (i = 0; i <= sscd_retries; i++) {
		sscd_rc = sscd_pdata.sscd_report(&sscd_dev, sscd_info.segs,
						 sscd_info.seg_count,
						 SSCD_FLAGS_ELFARM64HDR,
						 crash_info);
		if (sscd_rc != -EAGAIN)
			break;

		msleep(sscd_retry_ms);
	}
	if (sscd_rc == 0)
		dev_info(prvdata->dev, "aoc coredump done\n");
	else
		dev_err(prvdata->dev, "aoc coredump failed: sscd_rc = %d\n",
			sscd_rc);

	vunmap(dram_cached);
err_vmap:
	kfree(dram_pages);
err_kmalloc:
err_coredump:
	aoc_take_offline(prvdata);
	restart_rc = aoc_watchdog_restart(prvdata);
	if (restart_rc)
		dev_info(prvdata->dev, "aoc subsystem restart failed: rc = %d\n", restart_rc);
	else
		dev_info(prvdata->dev, "aoc subsystem restart succeeded\n");
}
#endif

static bool aoc_create_ion_heap(struct aoc_prvdata *prvdata)
{
	phys_addr_t base = prvdata->dram_resource.start + (28 * SZ_1M);
	struct device *dev = prvdata->dev;
	struct ion_heap *heap;
	size_t size = SZ_4M;
	size_t align = SZ_16K;
	const char *name = "sensor_direct_heap";

	heap = ion_physical_heap_create(base, size, align, name);
	if (IS_ERR(heap)) {
		dev_err(dev, "ION heap failure: %ld\n", PTR_ERR(heap));
	} else {
		prvdata->sensor_heap = heap;

		ion_physical_heap_set_allocate_callback(heap, aoc_pheap_alloc_cb, dev);
		ion_physical_heap_set_free_callback(heap, aoc_pheap_free_cb, dev);

		ion_device_add_heap(heap);
	}

	return !IS_ERR(heap);
}

static int aoc_open(struct inode *inode, struct file *file)
{
	return 0;
}

static long aoc_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dma_buf *dmabuf;
	struct ion_buffer *ionbuf;
	long ret = -EINVAL;

	switch (cmd) {
	case AOC_IOCTL_ION_FD_TO_HANDLE:
		{
			struct aoc_ion_handle handle;

			if (copy_from_user(&handle, (struct aoc_ion_handle *)arg, _IOC_SIZE(cmd))) {
				ret = -EFAULT;
				break;
			}

			dmabuf = dma_buf_get(handle.fd);
			if (IS_ERR(dmabuf)) {
				pr_err("fd is not an ion buffer\n");
				ret = PTR_ERR(dmabuf);
				break;
			}

			ionbuf = dmabuf->priv;
			handle.handle = (u32)ionbuf->priv_virt;

			dma_buf_put(dmabuf);

			if (copy_to_user((struct aoc_ion_handle *)arg, &handle, _IOC_SIZE(cmd)))
				ret = -EFAULT;
			else
				ret = 0;
		}
		break;

	default:
		/* ioctl(2) The specified request does not apply to the kind of object
		 * that the file descriptor fd references
		 */
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static int aoc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations fops = {
	.open = aoc_open,
	.release = aoc_release,
	.unlocked_ioctl = aoc_unlocked_ioctl,

	.owner = THIS_MODULE,
};

static char *aoc_devnode(struct device *dev, umode_t *mode)
{
	if (!mode || !dev)
		return NULL;

	if (MAJOR(dev->devt) == aoc_major)
		*mode = 0666;

	return kasprintf(GFP_KERNEL, "%s", dev_name(dev));
}

static int aoc_create_chrdev(struct platform_device *pdev)
{
	aoc_major = register_chrdev(0, AOC_CHARDEV_NAME, &fops);
	aoc_major_dev = MKDEV(aoc_major, 0);

	aoc_class = class_create(THIS_MODULE, AOC_CHARDEV_NAME);
	if (!aoc_class) {
		pr_err("failed to create aoc_class\n");
		return -ENXIO;
	}

	aoc_class->devnode = aoc_devnode;

	aoc_device = device_create(aoc_class, NULL, aoc_major_dev, NULL,
				   AOC_CHARDEV_NAME);
	if (!aoc_device) {
		pr_err("failed to create aoc_device\n");
		return -ENXIO;
	}

	return 0;
}
static void aoc_cleanup_resources(struct platform_device *pdev)
{
	struct aoc_prvdata *prvdata = platform_get_drvdata(pdev);

	pr_notice("cleaning up resources\n");

	if (prvdata) {
		aoc_take_offline(prvdata);

		if (prvdata->domain) {
			aoc_clear_sysmmu(prvdata);
			prvdata->domain = NULL;
		}

#ifdef AOC_JUNO
		free_irq(aoc_irq, prvdata->dev);
		aoc_irq = -1;
#endif
	}

	/*
	 * SRAM and DRAM were mapped with the device managed API, so they will
	 * be automatically detached
	 */

	if (aoc_major) {
		unregister_chrdev(aoc_major, AOC_CHARDEV_NAME);
		aoc_major = 0;
	}
}

static int aoc_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aoc_prvdata *prvdata = NULL;
	struct device_node *aoc_node, *mem_node;
	struct resource *rsrc;
	unsigned int acpm_async_size;
	int ret;

	if (aoc_platform_device != NULL) {
		dev_err(dev,
			"already matched the AoC to another platform device");
		return -EEXIST;
	}

	aoc_node = dev->of_node;
	mem_node = of_parse_phandle(aoc_node, "memory-region", 0);

	prvdata = devm_kzalloc(dev, sizeof(*prvdata), GFP_KERNEL);
	if (!prvdata)
		return -ENOMEM;

	prvdata->dev = dev;

	if (!mem_node) {
		dev_err(dev,
			"failed to find reserve-memory in the device tree\n");
		return -EINVAL;
	}

	aoc_sram_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "blk_aoc");

	ret = of_address_to_resource(mem_node, 0, &prvdata->dram_resource);
	of_node_put(mem_node);

	if (!aoc_sram_resource || ret != 0) {
		dev_err(dev,
			"failed to get memory resources for device sram %pR dram %pR\n",
			aoc_sram_resource, &prvdata->dram_resource);
		aoc_cleanup_resources(pdev);

		return -ENOMEM;
	}

#ifdef AOC_JUNO
	aoc_irq = platform_get_irq(pdev, 0);
	if (aoc_irq < 1) {
		dev_err(dev, "failed to configure aoc interrupt\n");
		return aoc_irq;
	}
#else
	prvdata->mbox_client.dev = dev;
	prvdata->mbox_client.tx_block = false;
	prvdata->mbox_client.tx_tout = 100; /* 100ms timeout for tx */
	prvdata->mbox_client.knows_txdone = false;
	prvdata->mbox_client.rx_callback = aoc_mbox_rx_callback;
	prvdata->mbox_client.tx_done = aoc_mbox_tx_done;
	prvdata->mbox_client.tx_prepare = aoc_mbox_tx_prepare;

	strscpy(prvdata->firmware_name, default_firmware,
		sizeof(prvdata->firmware_name));

	platform_set_drvdata(pdev, prvdata);

	prvdata->mbox_channel =
		mbox_request_channel_byname(&prvdata->mbox_client, "aoc2ap");
	if (IS_ERR(prvdata->mbox_channel)) {
		dev_err(dev, "failed to find mailbox interface : %ld\n",
			PTR_ERR(prvdata->mbox_channel));
		prvdata->mbox_channel = NULL;
		return -EIO;
	}

	init_waitqueue_head(&prvdata->aoc_reset_wait_queue);
	INIT_WORK(&prvdata->watchdog_work, aoc_watchdog);

	prvdata->watchdog_irq = platform_get_irq_byname(pdev, "watchdog");
	if (prvdata->watchdog_irq < 0) {
		dev_err(dev, "failed to find watchdog irq\n");
		return -EIO;
	}

	ret = devm_request_irq(dev, prvdata->watchdog_irq, watchdog_int_handler,
			       IRQF_TRIGGER_HIGH, dev_name(dev), dev);
	if (ret != 0) {
		dev_err(dev, "failed to register watchdog irq handler: %d\n",
			ret);
		return -EIO;
	}
#endif

	aoc_create_chrdev(pdev);

	pr_notice("found aoc with interrupt:%d sram:%pR dram:%pR\n", aoc_irq,
		  aoc_sram_resource, &prvdata->dram_resource);
	aoc_platform_device = pdev;

	aoc_sram_virt_mapping = devm_ioremap_resource(dev, aoc_sram_resource);
	aoc_dram_virt_mapping =
		devm_ioremap_resource(dev, &prvdata->dram_resource);

	/* Change to devm_platform_ioremap_resource_byname when available */
	rsrc = platform_get_resource_byname(pdev, IORESOURCE_MEM, "aoc_req");
	if (rsrc) {
		prvdata->aoc_req_virt = devm_ioremap_resource(dev, rsrc);
		prvdata->aoc_req_size = resource_size(rsrc);

		if (IS_ERR(prvdata->aoc_req_virt)) {
			dev_err(dev, "failed to map aoc_req region at %pR\n",
				rsrc);
			prvdata->aoc_req_virt = NULL;
			prvdata->aoc_req_size = 0;
		} else {
			dev_dbg(dev, "found aoc_req at %pR\n", rsrc);
		}
	}

	prvdata->sram_virt = aoc_sram_virt_mapping;
	prvdata->sram_size = resource_size(aoc_sram_resource);

	prvdata->dram_virt = aoc_dram_virt_mapping;
	prvdata->dram_size = resource_size(&prvdata->dram_resource);

	if (IS_ERR(aoc_sram_virt_mapping) || IS_ERR(aoc_dram_virt_mapping)) {
		aoc_cleanup_resources(pdev);
		return -ENOMEM;
	}

#ifndef AOC_JUNO
	prvdata->aoc_s2mpu_virt = devm_platform_ioremap_resource_byname(pdev, "aoc_s2mpu");
	if (IS_ERR(prvdata->aoc_s2mpu_virt)) {
		dev_err(dev, "failed to map aoc_s2mpu: rc = %ld\n",
			PTR_ERR(prvdata->aoc_s2mpu_virt));
		aoc_cleanup_resources(pdev);
		return PTR_ERR(prvdata->aoc_s2mpu_virt);
	}
	prvdata->aoc_s2mpu_saved_value = ioread32(prvdata->aoc_s2mpu_virt + AOC_S2MPU_CTRL0);

	pm_runtime_set_active(dev);
	/* Leave AoC in suspended state. Otherwise, AoC SysMMU is set to active which results in the
	 * SysMMU driver trying to access SysMMU SFRs during device suspend/resume operations. The
	 * latter is problematic if AoC is in monitor mode and BLK_AOC is off. */
	pm_runtime_set_suspended(dev);

	prvdata->domain = iommu_get_domain_for_dev(dev);
	if (!prvdata->domain) {
		pr_err("failed to find iommu domain\n");
		return -EIO;
	}

	aoc_configure_sysmmu(prvdata);

	aoc_create_ion_heap(prvdata);
#endif

	/* Default to 6MB if we are not loading the firmware (i.e. trace32) */
	aoc_control = aoc_dram_translate(prvdata, 6 * SZ_1M);

	INIT_WORK(&prvdata->online_work, aoc_did_become_online);

	aoc_configure_interrupt();

#ifdef AOC_JUNO
	ret = request_irq(aoc_irq, aoc_int_handler, IRQF_TRIGGER_HIGH, "aoc",
			  aoc_device);
	if (ret != 0) {
		pr_err("failed to register interrupt handler : %d\n", ret);
		aoc_cleanup_resources(pdev);

		return -ENXIO;
	}
#endif

	ret = acpm_ipc_request_channel(aoc_node, acpm_aoc_reset_callback,
				       &prvdata->acpm_async_id, &acpm_async_size);
	if (ret < 0) {
		dev_err(dev, "failed to register acpm aoc reset callback\n");
		return ret;
	}

	if (aoc_autoload_firmware) {
		ret = start_firmware_load(dev);
		if (ret != 0)
			pr_err("failed to start firmware download: %d\n", ret);
	}

	ret = sysfs_create_groups(&dev->kobj, aoc_groups);

	pr_debug("platform_probe matched\n");

	return 0;
}

static int aoc_platform_remove(struct platform_device *pdev)
{
	struct aoc_prvdata *prvdata;

	pr_debug("platform_remove\n");

	prvdata = platform_get_drvdata(pdev);
	acpm_ipc_release_channel(pdev->dev.of_node, prvdata->acpm_async_id);
	sysfs_remove_groups(&pdev->dev.kobj, aoc_groups);

	aoc_cleanup_resources(pdev);
	aoc_platform_device = NULL;

	return 0;
}

static void sscd_release(struct device *dev)
{
}

static void aoc_platform_shutdown(struct platform_device *pdev)
{
	struct aoc_prvdata *prvdata = platform_get_drvdata(pdev);

	aoc_take_offline(prvdata);
}

/* Module methods */
static int __init aoc_init(void)
{
	pr_debug("system driver init\n");

	if (bus_register(&aoc_bus_type) != 0) {
		pr_err("failed to register AoC bus\n");
		goto err_aoc_bus;
	}

	if (platform_driver_register(&aoc_driver) != 0) {
		pr_err("failed to register platform driver\n");
		goto err_aoc_driver;
	}

	if (platform_device_register(&sscd_dev) != 0) {
		pr_err("failed to register AoC coredump device\n");
		goto err_aoc_coredump;
	}

	return 0;

err_aoc_coredump:
	platform_driver_unregister(&aoc_driver);
err_aoc_driver:
	bus_unregister(&aoc_bus_type);
err_aoc_bus:
	return -ENODEV;
}

static void __exit aoc_exit(void)
{
	pr_debug("system driver exit\n");

	platform_driver_unregister(&aoc_driver);

	bus_unregister(&aoc_bus_type);
}

module_init(aoc_init);
module_exit(aoc_exit);

MODULE_LICENSE("GPL v2");
