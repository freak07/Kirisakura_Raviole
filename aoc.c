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
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/glob.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "aoc_firmware.h"
#include "aoc_ipc_core.h"

/* TODO: Remove internal calls, or promote to "public" */
#include "aoc_ipc_core_internal.h"

#define MAX_FIRMWARE_LENGTH 128

static struct platform_device *aoc_platform_device;

static bool aoc_online;

struct aoc_prvdata {
	struct mbox_client mbox_client;
	struct mbox_chan *mbox_channel;

	struct device *char_dev;
	struct device *dev;

	struct iommu_domain *domain;
	struct work_struct online_work;

	void *ipc_base;

	void *sram_virt;
	void *dram_virt;
	size_t sram_size;
	size_t dram_size;

	char firmware_name[MAX_FIRMWARE_LENGTH];
};

/* TODO: Reduce the global variables (move into a driver structure) */
/* Resources found from the device tree */
static struct resource *aoc_sram_resource;
static struct resource *aoc_dram_resource;

static void *aoc_sram_virt_mapping = NULL;
static void *aoc_dram_virt_mapping = NULL;

static int aoc_irq = 0;

static struct aoc_control_block *aoc_control = NULL;

static int aoc_major;
static int aoc_major_dev;

static struct class *aoc_class = NULL;

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

static volatile unsigned long read_blocked_mask;
static volatile unsigned long write_blocked_mask;
static struct aoc_service_metadata *metadata;

static bool aoc_fpga_reset(void);
static bool write_reset_trampoline(u32 addr);
static bool aoc_a32_reset(void);

static void aoc_take_offline(void);
static void signal_aoc(struct mbox_chan *channel);

static void aoc_configure_sysmmu(struct iommu_domain *domain);
static void aoc_clear_sysmmu(struct iommu_domain *domain);

static void aoc_process_services(struct aoc_prvdata *prvdata);

static inline void *aoc_sram_translate(u32 offset)
{
	BUG_ON(aoc_sram_virt_mapping == NULL);
	if (offset > resource_size(aoc_sram_resource))
		return NULL;

	return aoc_sram_virt_mapping + offset;
}

static inline void *aoc_dram_translate(u32 offset)
{
	BUG_ON(aoc_dram_virt_mapping == NULL);
	if (offset > resource_size(aoc_dram_resource))
		return NULL;

	return aoc_dram_virt_mapping + offset;
}

static bool aoc_is_valid_dram_address(struct aoc_prvdata *prv, void *addr)
{
	ptrdiff_t offset;

	if (addr < prv->dram_virt)
		return false;

	offset = addr - prv->dram_virt;
	return (offset < prv->dram_size);
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
	const char **driver_names = aoc_drv->service_names;

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
		size_t name_len = strnlen(name, AOC_SERVICE_NAME_LENGTH);
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

	return;
}

static void aoc_mbox_tx_prepare(struct mbox_client *cl, void *mssg)
{
	return;
}

static void aoc_mbox_tx_done(struct mbox_client *cl, void *mssg, int r)
{
	return;
}

static void aoc_fw_callback(const struct firmware *fw, void *ctx)
{
	struct device *dev = ctx;
	struct aoc_prvdata *prvdata = dev_get_drvdata(dev);
	u32 ipc_offset, bootloader_offset;

	if (!fw || !fw->data) {
		dev_err(dev, "failed to load firmware image\n");
		goto free_fw;
	}

	dev_notice(dev, "loaded firmware with size %d bytes\n", fw->size);

	if (!_aoc_fw_is_valid(fw)) {
		dev_err(dev, "firmware validation failed\n");
		goto free_fw;
	}

	ipc_offset = _aoc_fw_ipc_offset(fw);
	bootloader_offset = _aoc_fw_bootloader_offset(fw);

	pr_notice(
		"firmware image commit.  ipc_offset %u bootloader_offset %u\n",
		ipc_offset, bootloader_offset);

	aoc_fpga_reset();

	_aoc_fw_commit(fw, aoc_dram_virt_mapping + AOC_BINARY_DRAM_OFFSET);

	write_reset_trampoline(AOC_BINARY_LOAD_ADDRESS + bootloader_offset);

	aoc_a32_reset();

	prvdata->ipc_base = aoc_dram_translate(ipc_offset);
free_fw:
	release_firmware(fw);
}

ssize_t aoc_service_read(struct aoc_service_dev *dev, uint8_t *buffer,
			 size_t count, bool block)
{
	const struct device *parent = dev->dev.parent;
	struct aoc_prvdata *prvdata = dev_get_drvdata(parent);
	aoc_service *service;

	size_t msg_size;
	int service_number;
	int ret = 0;

	if (!dev || !buffer || !count)
		return -EINVAL;

	service_number = dev->service_index;
	service = service_at_index(prvdata, dev->service_index);

	BUG_ON(!aoc_is_valid_dram_address(prvdata, service));

	if (aoc_service_message_slots(service, AOC_UP) == 0)
		return -EBADF;

	if (!aoc_service_is_ring(service) &&
	    count < aoc_service_message_size(service, AOC_UP))
		return -EFBIG;

	if (!aoc_service_can_read_message(service, AOC_UP)) {
		if (!block)
			return -EAGAIN;

		set_bit(service_number, &read_blocked_mask);
		ret = wait_event_interruptible(
			metadata[service_number].read_queue,
			aoc_service_can_read_message(service, AOC_UP));
		clear_bit(service_number, &read_blocked_mask);
	}

	/*
	 * The wait can fail if the AoC goes offline in the middle of a
	 * blocking read, so check again after the wait
	 */
	if (ret != 0)
		return -EAGAIN;

	msg_size = count;
	aoc_service_read_message(service, prvdata->ipc_base, AOC_UP, buffer,
				 &msg_size);

	return msg_size;
}
EXPORT_SYMBOL(aoc_service_read);

ssize_t aoc_service_write(struct aoc_service_dev *dev, const uint8_t *buffer,
			  size_t count, bool block)
{
	const struct device *parent = dev->dev.parent;
	struct aoc_prvdata *prvdata = dev_get_drvdata(parent);

	aoc_service *service;
	int service_number;
	int ret = 0;

	if (!dev || !buffer || !count)
		return -EINVAL;

	service_number = dev->service_index;
	service = service_at_index(prvdata->ipc_base, service_number);

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
			aoc_service_can_write_message(service, AOC_DOWN));
		clear_bit(service_number, &write_blocked_mask);
	}

	/*
	 * The wait can fail if the AoC goes offline in the middle of a
	 * blocking write, so check again after the wait
	 */
	if (ret != 0)
		return -EAGAIN;

	ret = aoc_service_write_message(service, prvdata->ipc_base, AOC_DOWN,
					buffer, count);

	signal_aoc(prvdata->mbox_channel);

	return count;
}
EXPORT_SYMBOL(aoc_service_write);

static bool write_reset_trampoline(u32 addr)
{
	u32 instruction;
	u32 *reset;

	pr_notice("writing reset trampoline to addr %#x\n", addr);

	instruction = 0xe51ff004; /* ldr pc, [pc, #-4] */
	reset = aoc_sram_translate(0);
	if (!reset)
		return false;

	iowrite32(cpu_to_le32(instruction), reset++);
	iowrite32(cpu_to_le32(addr), reset);

	return true;
}

static bool aoc_fpga_reset(void)
{
#ifdef AOC_JUNO
	volatile u32 *reset = aoc_sram_translate(0x1000000);
	if (!reset)
		return false;

	aoc_take_offline();

	/* Assert and deassert reset */
	iowrite32(0, reset);
	iowrite32(1, reset);
#endif

	return true;
}

static bool aoc_a32_reset(void)
{
	u32 pcu_value;
	volatile u32 *pcu = aoc_sram_translate(AOC_PCU_BASE);
	if (!pcu)
		return false;

	pcu_value = ioread32(pcu);

	pcu_value |= 1;
	iowrite32(pcu_value, pcu);

	pcu_value &= ~(1);
	iowrite32(pcu_value, pcu);

	return true;
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

static inline void aoc_create_sysfs_nodes(struct device *dev)
{
	device_create_file(dev, &dev_attr_revision);
	device_create_file(dev, &dev_attr_services);
	device_create_file(dev, &dev_attr_firmware);
}

static inline void aoc_remove_sysfs_nodes(struct device *dev)
{
	device_remove_file(dev, &dev_attr_firmware);
	device_remove_file(dev, &dev_attr_services);
	device_remove_file(dev, &dev_attr_revision);
}

/* File operations */
static struct aoc_client *allocate_client(void)
{
	struct aoc_client *client =
		kmalloc(sizeof(struct aoc_client), GFP_KERNEL);
	if (!client)
		return NULL;

	client->endpoint = AOC_ENDPOINT_NONE;

	return client;
}

static void free_client(struct aoc_client *client)
{
	kfree(client);
}

static int aoc_open(struct inode *inode, struct file *file)
{
	file->private_data = allocate_client();
	if (!file->private_data)
		return -ENOMEM;

	return 0;
}

static int aoc_release(struct inode *inode, struct file *file)
{
	if (file->private_data) {
		struct aoc_client *client = file->private_data;

		free_client(client);
	}

	return 0;
}

static long aoc_unlocked_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct aoc_client *client = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret;

	if (!client) {
		pr_err("no client associated with request\n");
		return -EINVAL;
	}

	switch (cmd) {
	case AOC_IS_ONLINE: {
		int online = aoc_is_online();
		ret = copy_to_user(argp, &online, sizeof(online));
		return 0;
	}
	case AOC_FPGA_RESET: {
#ifdef AOC_JUNO
		aoc_fpga_reset();
		aoc_a32_reset();
#else
		pr_err("APM mailbox support required\n");
		return -EOPNOTSUPP;
#endif
		return 0;
	}
	default:
		/* man ioctl(2) The specified request does not apply to the kind of object
		 * that the file descriptor fd references */
		return -ENOTTY;
	}

	return -EINVAL;
}

static const struct file_operations fops = {
	.open = aoc_open,
	.release = aoc_release,
	.unlocked_ioctl = aoc_unlocked_ioctl,

	.owner = THIS_MODULE,
};

static int aoc_platform_probe(struct platform_device *dev);
static int aoc_platform_remove(struct platform_device *dev);

static const struct of_device_id aoc_match[] = {
	{
		.compatible = "google,aoc",
	},
	{},
};

static struct platform_driver aoc_driver = {
	.probe = aoc_platform_probe,
	.remove = aoc_platform_remove,
	.driver =
		{
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
	volatile u32 *gpio_register =
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
	pr_debug("aoc_remove_device %s\n", dev_name(dev));
	device_unregister(dev);

	return 0;
}

static void aoc_device_release(struct device *dev)
{
	struct aoc_service_dev *the_dev = AOC_DEVICE(dev);
	pr_debug("device release:%s\n", dev_name(dev));

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

	dev->name = aoc_service_name(s);
	dev->service_index = index;
	dev->service = s;
	dev->ipc_base = prv->ipc_base;

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

	wmb();
	iowrite32(mask,
		  aoc_sram_translate(AOC_PCU_BASE + AOC_PCU_DB_SET_OFFSET));
#else
	uint32_t message[8] = { 0 };
	pr_notice("signal\n");
	mbox_send_message(channel, &message);
#endif
}

static void aoc_configure_sysmmu(struct iommu_domain *domain)
{
#ifndef AOC_JUNO
	/* static inline int iommu_map(struct iommu_domain *domain, unsigned long
	 * iova, phys_addr_t paddr, size_t size, int prot) */
	iommu_map(domain, 0x98000000, aoc_dram_resource->start,
		  resource_size(aoc_dram_resource), 0);

	/* Use a 1MB mapping instead of individual mailboxes for now */
	/* TODO: Turn the mailbox address ranges into dtb entries */
	iommu_map(domain, 0x9A000000, 0x17600000, SZ_1M, 0);
#endif
}

static void aoc_clear_sysmmu(struct iommu_domain *domain)
{
#ifndef AOC_JUNO
	iommu_unmap(domain, 0x98000000, resource_size(aoc_dram_resource));

	iommu_unmap(domain, 0x9A000000, SZ_1M);
#endif
}

static char *aoc_devnode(struct device *dev, umode_t *mode)
{
	if (!mode || !dev)
		return NULL;

	if (MAJOR(dev->devt) == aoc_major)
		*mode = 0666;

	return kasprintf(GFP_KERNEL, "%s", dev_name(dev));
}

static void aoc_did_become_online(struct work_struct *work)
{
	struct aoc_prvdata *prvdata =
		container_of(work, struct aoc_prvdata, online_work);
	int i, s;

	s = aoc_num_services();

	pr_notice("did become online with %d services\n", aoc_num_services());

	if (!service_names_are_valid(prvdata)) {
		pr_err("invalid service names found.  Ignoring\n");
		return;
	}

	for (i = 0; i < s; i++) {
		if (!validate_service(prvdata, i)) {
			pr_err("service %d invalid\n", i);
			continue;
		}

		register_service_device(i, prvdata->dev);
	}
}

static void aoc_take_offline(void)
{
	pr_notice("taking aoc offline\n");

	bus_for_each_dev(&aoc_bus_type, NULL, NULL, aoc_remove_device);

	if (aoc_control)
		aoc_control->magic = 0;

	aoc_online = false;
}

static void aoc_process_services(struct aoc_prvdata *prvdata)
{
	volatile unsigned long mask;
	int services;
	int i;

	services = aoc_num_services();
	mask = read_blocked_mask;
	for (i = 0; i < services; i++) {
		if (test_bit(i, &read_blocked_mask) &&
		    aoc_service_can_read_message(service_at_index(prvdata, i),
						 AOC_UP))
			wake_up(&metadata[i].read_queue);
	}

	mask = write_blocked_mask;
	for (i = 0; i < services; i++) {
		if (test_bit(i, &write_blocked_mask) &&
		    aoc_service_can_write_message(service_at_index(prvdata, i),
						  AOC_DOWN))
			wake_up(&metadata[i].write_queue);
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
#endif

static struct device *aoc_create_chrdev(void)
{
	struct device *new_device;

	aoc_major = register_chrdev(0, AOC_CHARDEV_NAME, &fops);
	aoc_major_dev = MKDEV(aoc_major, 0);

	pr_notice("creating char device with class %p\n", aoc_class);
	new_device = device_create(aoc_class, NULL, aoc_major_dev, NULL,
				   AOC_CHARDEV_NAME);
	if (!new_device) {
		pr_err("failed to create aoc device\n");
		return NULL;
	}

	pr_notice("created character device with major %d minor %d\n",
		  aoc_major, 0);

	return new_device;
}

static void aoc_cleanup_resources(struct platform_device *pdev)
{
	struct aoc_prvdata *prvdata = platform_get_drvdata(pdev);

	pr_notice("cleaning up resources\n");

	if (prvdata) {
		if (prvdata->domain) {
			aoc_clear_sysmmu(prvdata->domain);
			prvdata->domain = NULL;
		}

		if (prvdata->mbox_channel) {
			mbox_free_channel(prvdata->mbox_channel);
			prvdata->mbox_channel = NULL;
		}

		aoc_take_offline();

		if (prvdata->char_dev) {
#ifdef AOC_JUNO
			free_irq(aoc_irq, prvdata->char_dev);
			aoc_irq = -1;
#endif
			pr_notice(
				"destroying device in cleanup with class %p\n",
				aoc_class);
			device_destroy(aoc_class, aoc_major_dev);
			prvdata->char_dev = NULL;
		}

		aoc_remove_sysfs_nodes(&pdev->dev);
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

static struct resource *memory_region_to_resource(struct device *dev,
						  struct device_node *node)
{
	u64 start, size;
	u32 region[4];

	int ret;

	BUG_ON(dev == NULL);
	BUG_ON(node == NULL);

	size = of_property_count_elems_of_size(node, "reg", sizeof(u32));
	if (size < 3 || size > 4) {
		dev_err(dev, "invalid array size %d\n", size);
		return NULL;
	}

	ret = of_property_read_u32_array(node, "reg", region, size);
	if (ret != 0) {
		dev_err(dev, "failed to read \"reg\" from memory node %pOF\n",
			node);
		return NULL;
	}

	if (size == 3) {
		/* 64-bit addr, 32-bit size */
		start = ((u64)region[0] << 32 | region[1]);
		size = region[2];
	} else if (size == 4) {
		/* 64-bit addr, 64-bit size (Juno) */
		start = ((u64)region[0] << 32 | region[1]);
		size = ((u64)region[2] | region[3]);
	}

	return devm_request_mem_region(dev, start, size, "carveout");
}

static int aoc_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aoc_prvdata *prvdata = NULL;
	struct device_node *aoc_node, *mem_node;
	int i;

	if (aoc_platform_device != NULL) {
		dev_err(dev,
			"already matched the AoC to another platform device");
		return -EEXIST;
	}

	aoc_node = dev->of_node;
	mem_node = of_parse_phandle(aoc_node, "memory-region", 0);

	if (!mem_node) {
		dev_err(dev,
			"failed to find reserve-memory in the device tree\n");
		return -EINVAL;
	}

	aoc_sram_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	aoc_dram_resource = memory_region_to_resource(dev, mem_node);

	if (!aoc_sram_resource || !aoc_dram_resource) {
		dev_err(dev,
			"failed to get memory resources for device sram %pR dram %pR\n",
			aoc_sram_resource, aoc_dram_resource);
		aoc_cleanup_resources(pdev);

		return -ENOMEM;
	}

	prvdata = devm_kzalloc(dev, sizeof(*prvdata), GFP_KERNEL);
	if (!prvdata) {
		return -ENOMEM;
	}

	prvdata->dev = dev;
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
#endif

	pr_notice("found aoc with interrupt:%d sram:%pR dram:%pR\n", aoc_irq,
		  aoc_sram_resource, aoc_dram_resource);
	aoc_platform_device = pdev;

#ifndef AOC_JUNO
	prvdata->domain = iommu_get_domain_for_dev(dev);
	if (!prvdata->domain) {
		pr_err("failed to find iommu domain\n");
		return -EIO;
	}

	aoc_configure_sysmmu(prvdata->domain);
#endif

	prvdata->char_dev = aoc_create_chrdev();
	if (!prvdata->char_dev) {
		pr_err("failed to create character device\n");
		aoc_cleanup_resources(pdev);
		return -ENXIO;
	}

	metadata =
		kmalloc(AOC_MAX_ENDPOINTS * sizeof(struct aoc_service_metadata),
			GFP_KERNEL);
	for (i = 0; i < AOC_MAX_ENDPOINTS; i++) {
		init_waitqueue_head(&metadata[i].read_queue);
		init_waitqueue_head(&metadata[i].write_queue);
	}

	aoc_sram_virt_mapping = devm_ioremap(dev, aoc_sram_resource->start,
					     resource_size(aoc_sram_resource));
	aoc_dram_virt_mapping = devm_ioremap(dev, aoc_dram_resource->start,
					     resource_size(aoc_dram_resource));

	prvdata->sram_virt = aoc_sram_virt_mapping;
	prvdata->sram_size = resource_size(aoc_sram_resource);

	prvdata->dram_virt = aoc_dram_virt_mapping;
	prvdata->dram_size = resource_size(aoc_dram_resource);

	if (IS_ERR(aoc_sram_virt_mapping) || IS_ERR(aoc_dram_virt_mapping)) {
		aoc_cleanup_resources(pdev);
		return -ENOMEM;
	}

	/* Default to 4MB if we are not loading the firmware (i.e. trace32) */
	aoc_control = aoc_dram_translate(0x400000);

	INIT_WORK(&prvdata->online_work, aoc_did_become_online);

	aoc_configure_interrupt();

#ifdef AOC_JUNO
	if ((ret = request_irq(aoc_irq, aoc_int_handler, IRQF_TRIGGER_HIGH,
			       "aoc", aoc_device)) != 0) {
		pr_err("failed to register interrupt handler : %d\n", ret);
		aoc_cleanup_resources(pdev);

		return -ENXIO;
	}
#endif

	aoc_create_sysfs_nodes(dev);

	if (aoc_autoload_firmware && !start_firmware_load(dev))
		pr_err("failed to start firmware download procedure\n");

	pr_notice("platform_probe matched\n");

	return 0;
}

static int aoc_platform_remove(struct platform_device *pdev)
{
	pr_notice("platform_remove\n");

	aoc_cleanup_resources(pdev);
	aoc_platform_device = NULL;

	return 0;
}

/* Module methods */
static int __init aoc_init(void)
{
	pr_debug("system driver init\n");

	aoc_class = class_create(THIS_MODULE, AOC_CHARDEV_NAME);
	if (!aoc_class) {
		pr_err("failed to create aoc class\n");
		bus_unregister(&aoc_bus_type);
		return -EIO;
	}

	aoc_class->devnode = aoc_devnode;

	platform_driver_register(&aoc_driver);

	if (bus_register(&aoc_bus_type) != 0)
		pr_err("failed to register AoC bus\n");

	return 0;
}

static void __exit aoc_exit(void)
{
	pr_debug("system driver exit\n");

	bus_unregister(&aoc_bus_type);

	platform_driver_unregister(&aoc_driver);

	if (aoc_class)
		class_destroy(aoc_class);
}

module_init(aoc_init);
module_exit(aoc_exit);

MODULE_LICENSE("GPL v2");
