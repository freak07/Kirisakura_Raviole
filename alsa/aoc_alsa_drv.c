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
	bool waiting;
	wait_queue_head_t wait_head;
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

static spinlock_t service_lock;
static int8_t n_services;
static bool drv_registered;

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

	spin_lock(&service_lock);
	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		if (strcmp(name, service_lists[i].name) == 0)
			break;
	}

	if (i == ARRAY_SIZE(service_lists))
		goto done;

	/* the AoC is not up yet, retrun -EPROBE_DEFER */
	if (!service_lists[i].dev) {
		err = -EPROBE_DEFER;
		goto done;
	}

	/* Only can be allocated by one client? */
	if (service_lists[i].ref != 0) {
		pr_err("%s has been allocated %d\n", name,
		       service_lists[i].ref);
		goto done;
	}

	*dev = service_lists[i].dev;
	service_lists[i].ref++;
	err = 0;

done:
	spin_unlock(&service_lock);

	return err;
}
EXPORT_SYMBOL(alloc_aoc_audio_service);

int free_aoc_audio_service(const char *name, struct aoc_service_dev *dev)
{
	int i, err = -EINVAL;

	if (!name || !dev)
		return -EINVAL;

	spin_lock(&service_lock);
	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		/*
		 * In normal case, we can find the service by testing
		 * the address of dev, but in AoC crash case, we should
		 * check the service name as well since the dev might
		 * have been set to NULL
		 */
		if (service_lists[i].dev) {
			if (dev == service_lists[i].dev)
				break;
		} else if (strcmp(name, service_lists[i].name) == 0)
			break;
	}

	if (i == ARRAY_SIZE(service_lists))
		goto done;

	if (service_lists[i].ref <= 0) {
		pr_err("ERR: %s ref = %d abnormal\n", name,
		       service_lists[i].ref);
		goto done;
	}

	service_lists[i].ref--;

	/* Wake up the remove thread if necessary */
	if (service_lists[i].ref == 0 &&
	    service_lists[i].waiting)
		wake_up(&service_lists[i].wait_head);

	err = 0;

done:
	spin_unlock(&service_lock);

	if (err != 0)
		pr_err("ERR: %s can't free audio service\n", name);

	return err;
}
EXPORT_SYMBOL(free_aoc_audio_service);

static int snd_aoc_alsa_probe(void)
{
	int err;

	err = aoc_pcm_init();
	if (err) {
		pr_err("ERR:fail to init aoc pcm\n");
		goto out;
	}

	err = aoc_voice_init();
	if (err) {
		pr_err("ERR: fail to init aoc voice\n");
		goto out;
	}

	err = aoc_compr_init();
	if (err) {
		pr_err("ERR:%d failed to init aoc compress offload\n", err);
		goto out;
	}

	err = aoc_path_init();
	if (err) {
		pr_err("ERR: fail to init aoc path\n");
		goto out;
	}

	return 0;

out:
	return err;
}

static int snd_aoc_alsa_remove(void)
{
	aoc_path_exit();
	aoc_compr_exit();
	aoc_voice_exit();
	aoc_pcm_exit();

	return 0;
}

static int aoc_alsa_probe(struct aoc_service_dev *dev)
{
	int i;
	int8_t nservices;

	spin_lock(&service_lock);

	/* Put the aoc service devices in order */
	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		if (strcmp(service_lists[i].name, dev_name(&dev->dev)) == 0)
			break;
	}

	if (i == ARRAY_SIZE(service_lists)) {
		pr_err("%s: invalid dev %s", __func__, dev_name(&dev->dev));
		spin_unlock(&service_lock);
		return -EINVAL;
	}

	service_lists[i].dev = dev;
	service_lists[i].ref = 0;
	service_lists[i].waiting = false;
	pr_notice("services %d: %s vs. %s\n", n_services,
		  service_lists[i].name, dev_name(&dev->dev));


	n_services++;
	nservices = n_services;

	spin_unlock(&service_lock);

	if (nservices == ARRAY_SIZE(service_lists) && !drv_registered) {
		snd_aoc_alsa_probe();
		drv_registered = true;
		pr_notice("alsa-aoc communication is ready!\n");
	}

	return 0;
}

static int aoc_alsa_remove(struct aoc_service_dev *dev)
{
	int i = 0;

	spin_lock(&service_lock);

	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		if (strcmp(service_lists[i].name, dev_name(&dev->dev)) == 0)
			break;
	}

	if (i == ARRAY_SIZE(service_lists)) {
		pr_err("%s: invalid dev %s\n", __func__, dev_name(&dev->dev));
		spin_unlock(&service_lock);
		return -EINVAL;
	}

	service_lists[i].dev = NULL;

	if (service_lists[i].ref < 0) {
		pr_warn("%s: invalid ref %d for %s\n", __func__,
			service_lists[i].ref, dev_name(&dev->dev));
		service_lists[i].ref = 0;
	}

	/*
	 * All clients have released the resource
	 */
	if (service_lists[i].ref == 0)
		goto done;

	service_lists[i].waiting = true;
	spin_unlock(&service_lock);
	pr_info("alsa wait %s\n", dev_name(&dev->dev));

	/*
	 * We should block the remove function until the ref
	 * is 0, otherwise, it might cause "use after free".
	 */
	wait_event(service_lists[i].wait_head,
		   service_lists[i].ref == 0);

	pr_info("alsa wait %s done\n", dev_name(&dev->dev));
	spin_lock(&service_lock);
	service_lists[i].waiting = false;

done:
	n_services--;

	spin_unlock(&service_lock);
	pr_notice("remove service %s\n", dev_name(&dev->dev));

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

	drv_registered = false;
	spin_lock_init(&service_lock);

	for (i = 0; i < ARRAY_SIZE(service_lists); i++) {
		service_lists[i].name = audio_service_names[i];
		init_waitqueue_head(&service_lists[i].wait_head);
	}

	aoc_driver_register(&aoc_alsa_driver);
	return 0;
}

static void __exit aoc_alsa_exit(void)
{
	pr_debug("aoc alsa driver exit\n");

	if (drv_registered) {
		snd_aoc_alsa_remove();
		drv_registered = false;
	}

	aoc_driver_unregister(&aoc_alsa_driver);
	cleanup_resources();
}

module_init(aoc_alsa_init);
module_exit(aoc_alsa_exit);

MODULE_DESCRIPTION("Whitechapel AoC ALSA Driver");
MODULE_AUTHOR("Xinhui Zhou and Carter Hsu (Google)");
MODULE_LICENSE("GPL v2");
