// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA Driver
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/string.h>
#include "aoc_alsa_drv.h"
#include "aoc_alsa.h"

#define AOC_ALSA_NAME "aoc_alsa"

/* Driver methods */
static int aoc_alsa_probe(struct aoc_service_dev *dev);
static int aoc_alsa_remove(struct aoc_service_dev *dev);

struct aoc_service_resource {
	const char *name;
	struct aoc_service_dev *dev;
	int ref;
};

/* TODO: audio_haptics should be added, capture1-3 needs to be determined */
static const char *const audio_service_names[] = {
	"audio_output_control",
	"audio_input_control",
	"audio_playback0",
	"audio_playback1",
	"audio_playback2",
	"audio_playback3",
	"audio_playback4",
	"audio_playback5",
	"audio_playback6",
	"audio_haptics",
	"audio_capture0",
	"audio_capture1",
	"audio_capture2",
	"audio_capture3",
	NULL,
};

static struct aoc_service_resource
	service_lists[ARRAY_SIZE(audio_service_names) - 1];

static struct mutex service_mutex;
static int8_t n_services;
static bool drv_registered;

void print_aoc_dev_info(struct aoc_service_dev *dev)
{
	pr_notice("--------------------------------\n");
	pr_notice("probe service with name (alsa) %s\n", dev_name(&dev->dev));
	pr_notice("name:  %s\n", dev_name(&dev->dev));
	pr_notice("service index:  %d\n", dev->service_index);
	pr_notice("ipc base:  %p\n", dev->ipc_base);
	pr_notice("--------------------------------\n");
}
EXPORT_SYMBOL(print_aoc_dev_info);

int8_t aoc_audio_service_num(void)
{
	return n_services;
}
EXPORT_SYMBOL(aoc_audio_service_num);

int alloc_aoc_audio_service(const char *name, struct aoc_service_dev **dev)
{
	int i, err = -EINVAL;

	if (!name || !dev)
		return -EINVAL;

	*dev = NULL;

	mutex_lock(&service_mutex);
	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		if (!strcmp(name, service_lists[i].name)) {
			/*Only can be allocated by one client?*/
			if (service_lists[i].dev) {
				if (service_lists[i].ref != 0) {
					pr_err("%s has been alloctaed %d\n",
					       name, service_lists[i].ref);
				} else {
					*dev = service_lists[i].dev;
					service_lists[i].ref++;
					err = 0;
				}
			} else {
				err = -EPROBE_DEFER;
			}
			break;
		}
	}
	mutex_unlock(&service_mutex);

	return err;
}
EXPORT_SYMBOL(alloc_aoc_audio_service);

int free_aoc_audio_service(const char *name, struct aoc_service_dev *dev)
{
	int i, err = -EINVAL;

	if (!name || !dev)
		return -EINVAL;

	mutex_lock(&service_mutex);
	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		if (dev == service_lists[i].dev) {
			if (service_lists[i].ref > 0) {
				service_lists[i].ref--;
				err = 0;
			} else {
				pr_err("%s ref is abnormal %d\n", name,
				       service_lists[i].ref);
			}
			break;
		}
	}
	mutex_unlock(&service_mutex);

	if (err != 0)
		pr_err("%s can't free audio service\n", name);

	return err;
}
EXPORT_SYMBOL(free_aoc_audio_service);

static int snd_aoc_alsa_probe(void)
{
	int err;

	err = aoc_pcm_init();
	if (err) {
		pr_err("%s: failed to init aoc pcm\n", __func__);
		goto err_free;
	}

	err = aoc_voice_init();
	if (err) {
		pr_err("%s: failed to init aoc voice\n", __func__);
		goto err_free;
	}

	err = aoc_path_init();
	if (err) {
		pr_err("%s: failed to init aoc path\n", __func__);
		goto err_free;
	}

	return 0;

err_free:
	return err;
}

static int snd_aoc_alsa_remove(void)
{
	aoc_path_exit();
	aoc_voice_exit();
	aoc_pcm_exit();

	return 0;
}

static int aoc_alsa_probe(struct aoc_service_dev *dev)
{
	int i = 0;
	int8_t nservices;

	print_aoc_dev_info(dev);
	pr_notice("num of aoc services : %ld\n", ARRAY_SIZE(service_lists));

	mutex_lock(&service_mutex);
	/* put the aoc service devices in order */
	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		if (!strcmp(service_lists[i].name, dev_name(&dev->dev))) {
			service_lists[i].dev = dev;
			service_lists[i].ref = 0;
			pr_notice("services %d: %s vs. %s\n", n_services,
				  service_lists[i].name, dev_name(&dev->dev));
			break;
		}
	}

	BUG_ON(i == ARRAY_SIZE(service_lists));
	n_services++;
	nservices = n_services;
	mutex_unlock(&service_mutex);

	if (nservices == ARRAY_SIZE(service_lists) && !drv_registered) {
		snd_aoc_alsa_probe();
		drv_registered = true;
	}

	return 0;
}

static int aoc_alsa_remove(struct aoc_service_dev *dev)
{
	int8_t nservices;
	int i = 0;

	mutex_lock(&service_mutex);
	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		if (!strcmp(service_lists[i].name, dev_name(&dev->dev))) {
			service_lists[i].dev = NULL;
			service_lists[i].ref = 0;
			break;
		}
	}
	nservices = n_services;
	n_services--;
	mutex_unlock(&service_mutex);

	pr_notice("remove service with name %s\n", dev_name(&dev->dev));

	return 0;
}

/*TODO: ? */
static void cleanup_resources(void)
{
}

static struct aoc_driver aoc_alsa_driver = {
	.drv = {
		.name = AOC_ALSA_NAME,
	},
	.service_names = audio_service_names,
	.probe = aoc_alsa_probe,
	.remove = aoc_alsa_remove,
};

static int __init aoc_alsa_init(void)
{
	int i;

	pr_debug("aoc alsa driver init\n");
	mutex_init(&service_mutex);
	drv_registered = false;

	for (i = 0; i < ARRAY_SIZE(service_lists); i++)
		service_lists[i].name = audio_service_names[i];

	aoc_driver_register(&aoc_alsa_driver);
	return 0;
}

static void __exit aoc_alsa_exit(void)
{
	pr_debug("aoc driver exit\n");

	if (drv_registered) {
		snd_aoc_alsa_remove();
		drv_registered = false;
	}
	aoc_driver_unregister(&aoc_alsa_driver);
	cleanup_resources();
	mutex_destroy(&service_mutex);
}

module_init(aoc_alsa_init);
module_exit(aoc_alsa_exit);

MODULE_DESCRIPTION("Whitechapel AoC ALSA Driver");
MODULE_AUTHOR("Xinhui Zhou and Carter Hsu (Google)");
MODULE_LICENSE("GPL v2");
