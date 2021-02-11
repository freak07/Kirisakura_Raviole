/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright 2020 Google LLC. All Rights Reserved.
 *
 * aoc service to send cmds to aoc
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>

#include "aoc.h"
#include "aoc-interface.h"
#include "aoc_uwb_service_dev.h"

#define AOC_UWB_SERVICE_DEV_NAME "aoc_uwb_sdev"
#define AOC_SERVICE_NAME "uwb_service"

static struct aoc_service_dev *aoc_uwb_service = NULL;
static const char * const service_names[] = {
	AOC_SERVICE_NAME,
	NULL,
};

static int aoc_uwb_service_probe(struct aoc_service_dev *sd)
{
	struct device *dev = &sd->dev;

	dev_dbg(dev, "probe service sd=%p\n", sd);
	aoc_uwb_service = sd;
	return 0;
}

static int aoc_uwb_service_remove(struct aoc_service_dev *sd)
{
	return 0;
}

static struct aoc_driver aoc_uwb_sdev = {
	.drv = {
		.name = AOC_UWB_SERVICE_DEV_NAME,
	},
	.service_names = service_names,
	.probe = aoc_uwb_service_probe,
	.remove = aoc_uwb_service_remove,
};

static int __init aoc_uwb_service_init(void)
{
	int ret;

	ret = aoc_driver_register(&aoc_uwb_sdev);
	return 0;
}

static void __exit aoc_uwb_service_exit(void)
{
	aoc_driver_unregister(&aoc_uwb_sdev);
}

ssize_t aoc_uwb_service_send(void *cmd, size_t size)
{
	int ret;

	ret = aoc_service_write(aoc_uwb_service, cmd, size, true);
	if (ret < 0)
		goto out;

	ret = aoc_service_read(aoc_uwb_service, cmd, size, true);

out:
	return ret;
}
EXPORT_SYMBOL_GPL(aoc_uwb_service_send);

bool aoc_uwb_service_ready(void)
{
	return (aoc_uwb_service != NULL);
}
EXPORT_SYMBOL_GPL(aoc_uwb_service_ready);

module_init(aoc_uwb_service_init);
module_exit(aoc_uwb_service_exit);

MODULE_LICENSE("GPL v2");
