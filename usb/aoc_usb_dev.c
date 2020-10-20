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

#include "aoc_usb.h"

#define AOC_USB_NAME "aoc_usb"

enum { NONBLOCKING = 0, BLOCKING = 1 };

enum {
	TYPE_SCRATCH_PAD = 0,
	TYPE_DEVICE_CONTEXT,
	TYPE_END_OF_SETUP,
	TYPE_DCBAA
};

static ssize_t aoc_usb_send_command(struct aoc_usb_drvdata *drvdata,
				    void *in_cmd, size_t in_size, void *out_cmd,
				    size_t out_size)
{
	struct aoc_service_dev *adev = drvdata->adev;
	ssize_t ret;

	ret = mutex_lock_interruptible(&drvdata->lock);
	if (ret != 0)
		return ret;

	ret = aoc_service_write(adev, in_cmd, in_size, BLOCKING);
	if (ret != in_size) {
		ret = -EIO;
		goto out;
	}

	ret = aoc_service_read(adev, out_cmd, out_size, BLOCKING);
	if (ret != out_size)
		ret = -EIO;

out:
	mutex_unlock(&drvdata->lock);
	return ret;
}

static int aoc_usb_get_dev_ctx(struct aoc_usb_drvdata *drvdata,
			       unsigned int slot_id, size_t length, u8 *dev_ctx)
{
	int ret = 0;
	struct CMD_USB_CONTROL_GET_DEVICE_CONTEXT *cmd;

	cmd = kzalloc(sizeof(struct CMD_USB_CONTROL_GET_DEVICE_CONTEXT), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	AocCmdHdrSet(&cmd->parent,
		     CMD_USB_CONTROL_GET_DEVICE_CONTEXT_ID,
		     sizeof(*cmd));

	cmd->device_id = slot_id;
	cmd->length = length;

	dev_dbg(&drvdata->adev->dev, "cmd=(%u, %u)\n", cmd->device_id, cmd->length);
	ret = aoc_usb_send_command(drvdata, cmd, sizeof(*cmd), cmd, sizeof(*cmd));
	if (ret < 0)
		return ret;

	memcpy(dev_ctx, cmd->payload, length);

	kfree(cmd);

	return 0;
}

static int aoc_usb_get_dcbaa_ptr(struct aoc_usb_drvdata *drvdata,
				 u64 *aoc_dcbaa_ptr)
{
	int ret = 0;
	struct CMD_USB_CONTROL_GET_DCBAA_PTR *cmd;

	cmd = kzalloc(sizeof(struct CMD_USB_CONTROL_GET_DCBAA_PTR), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	AocCmdHdrSet(&cmd->parent,
		     CMD_USB_CONTROL_GET_DCBAA_PTR_ID,
		     sizeof(*cmd));

	ret = aoc_usb_send_command(drvdata, cmd, sizeof(*cmd), cmd, sizeof(*cmd));
	if (ret < 0)
		return ret;

	*aoc_dcbaa_ptr = cmd->aoc_dcbaa_ptr;

	kfree(cmd);

	return 0;
}

static int aoc_usb_setup_done(struct aoc_usb_drvdata *drvdata)
{
	int ret;
	struct CMD_USB_CONTROL_SETUP *cmd;
	uint64_t aoc_dcbaa;

	cmd = kzalloc(sizeof(struct CMD_USB_CONTROL_SETUP), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	AocCmdHdrSet(&cmd->parent,
		     CMD_USB_CONTROL_SETUP_ID,
		     sizeof(*cmd));

	cmd->type = TYPE_END_OF_SETUP;
	cmd->ctx_idx = 0;
	cmd->spbuf_idx = 0;
	cmd->length = 0;
	ret = aoc_usb_send_command(drvdata, cmd, sizeof(*cmd), cmd, sizeof(*cmd));
	if (ret < 0)
		return ret;

	aoc_dcbaa = cmd->aoc_dcbaa;

	kfree(cmd);

	return 0;
}

static int aoc_usb_get_isoc_tr_info(struct aoc_usb_drvdata *drvdata, void *args)
{
	int ret;
	struct get_isoc_tr_info_args *tr_info_args =
		(struct get_isoc_tr_info_args *)args;
	struct CMD_USB_CONTROL_GET_ISOC_TR_INFO *cmd;

	cmd = kzalloc(sizeof(struct CMD_USB_CONTROL_GET_ISOC_TR_INFO), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	AocCmdHdrSet(&cmd->parent,
		     CMD_USB_CONTROL_GET_ISOC_TR_INFO_ID,
		     sizeof(*cmd));

	cmd->ep_id = tr_info_args->ep_id;
	cmd->dir = tr_info_args->dir;

	dev_dbg(&drvdata->adev->dev, "ep_id=%u, dir=%u\n", cmd->ep_id, cmd->dir);
	ret = aoc_usb_send_command(drvdata, cmd, sizeof(*cmd), cmd, sizeof(*cmd));
	if (ret < 0)
		return ret;

	tr_info_args->type = cmd->type;
	tr_info_args->num_segs = cmd->num_segs;
	tr_info_args->seg_ptr = cmd->seg_ptr;
	tr_info_args->max_packet = cmd->max_packet;
	tr_info_args->cycle_state = cmd->cycle_state;
	tr_info_args->num_trbs_free = cmd->num_trbs_free;

	kfree(cmd);

	return 0;
}

static int aoc_usb_notify(struct notifier_block *this,
			  unsigned long code, void *data)
{
	struct aoc_usb_drvdata *drvdata =
		container_of(this, struct aoc_usb_drvdata, nb);
	int ret;
	struct get_dev_ctx_args *dev_ctx_args;

	switch (code) {
	case SYNC_DEVICE_CONTEXT:
		dev_ctx_args = data;
		ret = aoc_usb_get_dev_ctx(drvdata, dev_ctx_args->slot_id,
					  dev_ctx_args->length,
					  dev_ctx_args->dev_ctx);
		break;
	case GET_DCBAA_PTR:
		ret = aoc_usb_get_dcbaa_ptr(drvdata, data);
		break;
	case SETUP_DONE:
		ret = aoc_usb_setup_done(drvdata);
		break;
	case GET_ISOC_TR_INFO:
		ret = aoc_usb_get_isoc_tr_info(drvdata, data);
		break;
	default:
		dev_warn(&drvdata->adev->dev, "Code %lu is not supported\n", code);
		ret = -EINVAL;
		break;
	}

	return ret;
}

bool aoc_usb_probe_done;
static int aoc_usb_probe(struct aoc_service_dev *adev)
{
	struct device *dev = &adev->dev;
	struct aoc_usb_drvdata *drvdata;

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->adev = adev;

	mutex_init(&drvdata->lock);

	drvdata->nb.notifier_call = aoc_usb_notify;
	register_aoc_usb_notifier(&drvdata->nb);

	dev_set_drvdata(dev, drvdata);

	aoc_usb_probe_done = true;

	return 0;
}

static int aoc_usb_remove(struct aoc_service_dev *adev)
{
	struct aoc_usb_drvdata *drvdata = dev_get_drvdata(&adev->dev);

	unregister_aoc_usb_notifier(&drvdata->nb);
	mutex_destroy(&drvdata->lock);

	kfree(drvdata);

	aoc_usb_probe_done = false;

	return 0;
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

static int __init aoc_usb_init(void)
{
	xhci_vendor_helper_init();

	return aoc_driver_register(&aoc_usb_driver);
}

static void __exit aoc_usb_exit(void)
{
	aoc_driver_unregister(&aoc_usb_driver);
}

module_init(aoc_usb_init);
module_exit(aoc_usb_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Howard Yen (Google)");
MODULE_DESCRIPTION("USB driver for AoC");
