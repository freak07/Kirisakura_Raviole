// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2019 Google LLC. All Rights Reserved.
 *
 * Character device interface for AoC services
 * ACD - AoC Character Device
 */

#define pr_fmt(fmt) "aoc_char: " fmt

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "aoc.h"
#include "aoc_ipc_core.h"

#define ACD_CHARDEV_NAME "aoc_char"

static int acd_major = -1;
static int acd_major_dev;
static struct class *acd_class;

#define ACD_MAX_DEVICES 64
static struct device *acd_devices[ACD_MAX_DEVICES];
static unsigned long opened_devices;

/* Driver methods */
static int acd_probe(struct aoc_service_dev *dev);
static int acd_remove(struct aoc_service_dev *dev);

static struct aoc_driver aoc_char_driver = {
	.drv = {
			.name = "aoc_char",
		},
	.probe = acd_probe,
	.remove = acd_remove,
};

struct file_prvdata {
	struct aoc_service_dev *service;
	int device_index;
};

/* File methods */
static int acd_open(struct inode *inode, struct file *file);
static int acd_release(struct inode *inode, struct file *file);
static long acd_unlocked_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg);
static ssize_t acd_read(struct file *file, char __user *buf, size_t count,
			loff_t *off);
static ssize_t acd_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *off);
static unsigned int acd_poll(struct file *file, poll_table *wait);

static const struct file_operations fops = {
	.open = acd_open,
	.release = acd_release,
	.unlocked_ioctl = acd_unlocked_ioctl,
	.read = acd_read,
	.write = acd_write,
	.poll = acd_poll,

	.owner = THIS_MODULE,
};

static struct aoc_service_dev *service_for_inode(struct inode *inode)
{
	int minor = MINOR(inode->i_rdev);

	if (minor < 0 || minor >= ACD_MAX_DEVICES)
		return NULL;

	if (acd_devices[minor] == NULL)
		return NULL;

	return dev_get_drvdata(acd_devices[minor]);
}

static char *acd_devnode(struct device *dev, umode_t *mode)
{
	if (!mode || !dev)
		return NULL;

	if (MAJOR(dev->devt) == acd_major)
		*mode = 0666;

	return kasprintf(GFP_KERNEL, "%s", dev_name(dev));
}

static int create_character_device(struct aoc_service_dev *dev)
{
	int i;

	for (i = 0; i < ACD_MAX_DEVICES; i++) {
		if (acd_devices[i] == NULL) {
			acd_devices[i] =
				device_create(acd_class, &dev->dev,
					      MKDEV(acd_major, i), NULL,
					      "acd-%s", dev_name(&(dev->dev)));
			if (IS_ERR(acd_devices[i])) {
				pr_err("device_create failed: %ld\n",
				       PTR_ERR(acd_devices[i]));
				acd_devices[i] = NULL;
				return -EINVAL;
			}

			dev_set_drvdata(acd_devices[i], dev);

			return 0;
		}
	}

	return -ENODEV;
}

static int acd_open(struct inode *inode, struct file *file)
{
	struct file_prvdata *prvdata;
	aoc_service *s;
	int minor = MINOR(inode->i_rdev);
	int owned = 0;

	pr_debug("attempt to open major:%d minor:%d\n", MAJOR(inode->i_rdev),
		 MINOR(inode->i_rdev));

	s = service_for_inode(inode);
	if (!s)
		return -ENODEV;

	owned = test_and_set_bit(minor, &opened_devices);
	if (owned)
		return -EBUSY;

	prvdata = kmalloc(sizeof(struct file_prvdata), GFP_KERNEL);
	if (!prvdata) {
		clear_bit(minor, &opened_devices);
		return -ENOMEM;
	}

	prvdata->service = s;
	prvdata->device_index = minor;
	file->private_data = prvdata;

	return 0;
}

static int acd_release(struct inode *inode, struct file *file)
{
	struct file_prvdata *private = file->private_data;

	if (!private)
		return -ENODEV;

	clear_bit(private->device_index, &opened_devices);
	kfree(private);
	file->private_data = NULL;

	return 0;
}

static long acd_unlocked_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	/*
	 * struct file_prvdata *private = file->private_data;
	 *
	 * if (!private)
	 *	return -ENODEV;
	 */

	return -EINVAL;
}

static ssize_t acd_read(struct file *file, char __user *buf, size_t count,
			loff_t *off)
{
	struct file_prvdata *private = file->private_data;
	char *buffer;
	size_t leftover;
	ssize_t retval = 0;
	bool should_block = ((file->f_flags & O_NONBLOCK) == 0);

	if (!private)
		return -ENODEV;

	buffer = kmalloc(count, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	retval =
		aoc_service_read(private->service, buffer, count, should_block);
	if (retval >= 0) {
		leftover = copy_to_user(buf, buffer, retval);
		retval = retval - leftover;
	}

	kfree(buffer);
	return retval;
}

static ssize_t acd_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *off)
{
	struct file_prvdata *private = file->private_data;
	bool should_block = ((file->f_flags & O_NONBLOCK) == 0);
	char *buffer;
	size_t leftover;
	ssize_t retval = 0;

	if (!private)
		return -ENODEV;

	buffer = kmalloc(count, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	leftover = copy_from_user(buffer, buf, count);
	if (leftover == 0) {
		retval = aoc_service_write(private->service, buffer, count,
					   should_block);
	} else {
		retval = -ENOMEM;
	}

	kfree(buffer);
	return retval;
}

static unsigned int acd_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	struct file_prvdata *private = file->private_data;

	poll_wait(file, aoc_service_get_read_queue(private->service), wait);
	poll_wait(file, aoc_service_get_write_queue(private->service), wait);
	aoc_service_set_read_blocked(private->service);
	aoc_service_set_write_blocked(private->service);
	if (aoc_service_can_read(private->service))
		mask |= POLLIN | POLLRDNORM;
	if (aoc_service_can_write(private->service))
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

static int acd_probe(struct aoc_service_dev *dev)
{
	int ret;

	pr_debug("probe service with name %s\n", dev_name(&dev->dev));
	ret = create_character_device(dev);

	return ret;
}

static int acd_remove(struct aoc_service_dev *dev)
{
	int i;

	for (i = 0; i < ACD_MAX_DEVICES; i++) {
		if (acd_devices[i] && acd_devices[i]->parent == &dev->dev) {
			pr_debug("remove service with name %s\n",
				 dev_name(&dev->dev));

			device_destroy(acd_class, acd_devices[i]->devt);
			acd_devices[i] = NULL;
		}
	}

	return 0;
}

static void cleanup_resources(void)
{
	aoc_driver_unregister(&aoc_char_driver);

	if (acd_class) {
		class_destroy(acd_class);
		acd_class = NULL;
	}

	if (acd_major >= 0) {
		unregister_chrdev(acd_major, ACD_CHARDEV_NAME);
		acd_major = -1;
	}
}

static int __init acd_init(void)
{
	pr_debug("driver init\n");

	acd_major = register_chrdev(0, ACD_CHARDEV_NAME, &fops);
	if (acd_major < 0) {
		pr_err("Failed to register character major number\n");
		goto fail;
	}

	acd_major_dev = MKDEV(acd_major, 0);

	acd_class = class_create(THIS_MODULE, ACD_CHARDEV_NAME);
	if (!acd_class) {
		pr_err("Failed to create class\n");
		goto fail;
	}

	acd_class->devnode = acd_devnode;

	aoc_driver_register(&aoc_char_driver);

	return 0;

fail:
	cleanup_resources();
	return -1;
}

static void __exit acd_exit(void)
{
	pr_debug("driver exit\n");

	cleanup_resources();
}

module_init(acd_init);
module_exit(acd_exit);

MODULE_LICENSE("GPL v2");
