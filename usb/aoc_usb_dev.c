// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2020 Google LLC. All Rights Reserved.
 *
 * Interface to the AoC USB control service
 */

#define pr_fmt(fmt) "aoc_usb_control: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "aoc.h"
#include "aoc-interface.h"

#define AOC_USB_NAME "aoc_usb"

enum { NONBLOCKING = 0, BLOCKING = 1 };

uint32_t aoc_usb_get_xhci_version(struct aoc_service_dev *adev)
{
	int ret;
	struct CMD_USB_CONTROL_GET_XHCI_VERSION cmd_get_xhci_version = {0};

	AocCmdHdrSet(&cmd_get_xhci_version.parent,
				 CMD_USB_CONTROL_GET_XHCI_VERSION_ID,
				 sizeof(cmd_get_xhci_version));

	ret = aoc_service_write(adev, (uint8_t*)&cmd_get_xhci_version,
				sizeof(cmd_get_xhci_version), BLOCKING);
	if (ret < 0) {
		dev_err(&adev->dev, "%s write command fail!\n", __func__);
		return ret;
	}

	ret = aoc_service_read(adev, (uint8_t*)&cmd_get_xhci_version,
			       sizeof(cmd_get_xhci_version), BLOCKING);
	if (ret < 0) {
		dev_err(&adev->dev, "%s read command fail!\n", __func__);
		return ret;
	}

	return cmd_get_xhci_version.xhci_version;
}

static ssize_t aoc_usb_xhci_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct aoc_service_dev *adev = AOC_DEVICE(dev);
	uint32_t xhci_version;
	ssize_t ret = 0;

	xhci_version = aoc_usb_get_xhci_version(adev);
	dev_info(dev, "Xhci Version: 0x%x\n", xhci_version);
	if (xhci_version)
		ret = scnprintf(buf, PAGE_SIZE, "Xhci Version: 0x%x\n",
				xhci_version);

	return ret;
}

DEVICE_ATTR(usb_xhci_version, 0440, aoc_usb_xhci_version_show, NULL);

static struct attribute *aoc_usb_control_attrs[] = {
	&dev_attr_usb_xhci_version.attr,
	NULL,
};

ATTRIBUTE_GROUPS(aoc_usb_control);

static int aoc_usb_probe(struct aoc_service_dev *adev)
{
	int ret = 0;
	dev_info(&adev->dev, "%s++\n", __func__);

	ret = devm_device_add_groups(&adev->dev, aoc_usb_control_groups);
	if (ret)
		dev_err(&adev->dev, "Create attribute groups failed.\n");

	dev_info(&adev->dev, "%s--, ret = %d\n", __func__, ret);
	return ret;
}

static int aoc_usb_remove(struct aoc_service_dev *adev)
{
	int ret = 0;
	dev_info(&adev->dev, "%s++\n", __func__);

	devm_device_remove_groups(&adev->dev, aoc_usb_control_groups);
	if (ret)
		dev_err(&adev->dev, "Remove attribute groups failed.\n");

	dev_info(&adev->dev, "%s--, ret = %d\n", __func__, ret);
	return ret;
}

static const char *const aoc_usb_service_names[] = {
	"usb_control",
	NULL,
};

static struct aoc_driver aoc_usb_driver = {
	.drv = {
		.name = AOC_USB_NAME,
	},
	.service_names = aoc_usb_service_names,
	.probe = aoc_usb_probe,
	.remove = aoc_usb_remove,
};

module_aoc_driver(aoc_usb_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Howard Yen (Google)");
MODULE_DESCRIPTION("USB driver for AoC");
