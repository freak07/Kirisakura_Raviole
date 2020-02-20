// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA Driver - Mixer controls
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "aoc_alsa.h"

/* Volume maximum and minimum */
#define CTRL_VOL_MIN 0
#define CTRL_VOL_MAX 1000

static int snd_aoc_ctl_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = 1;
		uinfo->value.integer.min = CTRL_VOL_MIN;
		uinfo->value.integer.max = CTRL_VOL_MAX;
	} else if (kcontrol->private_value == PCM_PLAYBACK_MUTE) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
		uinfo->count = 1;
		uinfo->value.integer.min = 0;
		uinfo->value.integer.max = 1;
	}

	return 0;
}

/*
 * Toggle mute on/off depending on the value of nmute, and returns
 * 1 if the mute value was changed, otherwise 0
 */
static int toggle_mute(struct aoc_chip *chip, int nmute)
{
	if (chip->mute == nmute)
		return 0;

	if (chip->mute == CTRL_VOL_MUTE) {
		chip->volume = chip->old_volume;
		pr_debug("Unmuting, old_volume = %d, volume = %d\n",
			 chip->old_volume, chip->volume);
	} else {
		chip->old_volume = chip->volume;
		chip->volume = 0;
		pr_debug("Muting, old_volume = %d, volume = %d\n",
			 chip->old_volume, chip->volume);
	}

	chip->mute = nmute;
	return 1;
}

static int snd_aoc_ctl_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	BUG_ON(!chip && !(chip->avail_substreams & AVAIL_SUBSTREAMS_MASK));

	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME)
		ucontrol->value.integer.value[0] = chip2alsa(chip->volume);
	else if (kcontrol->private_value == PCM_PLAYBACK_MUTE)
		ucontrol->value.integer.value[0] = chip->mute;

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int snd_aoc_ctl_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);
	int changed = 0;

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME) {
		pr_debug(
			"volume change attempted.. volume = %d new_volume = %d\n",
			chip->volume, (int)ucontrol->value.integer.value[0]);
		if (chip->mute == CTRL_VOL_MUTE) {
			changed = 1;
			goto unlock;
		}
		if (changed || (ucontrol->value.integer.value[0] !=
				chip2alsa(chip->volume))) {
			chip->volume =
				alsa2chip(ucontrol->value.integer.value[0]);
			changed = 1;
		}
	} else if (kcontrol->private_value == PCM_PLAYBACK_MUTE) {
		pr_debug("mute attempted\n");
		changed = toggle_mute(chip, ucontrol->value.integer.value[0]);
	}

	if (changed) {
		if (aoc_audio_set_ctls(chip))
			pr_err("failed to set ALSA controls\n");
	}

unlock:
	mutex_unlock(&chip->audio_mutex);
	return changed;
}

static struct snd_kcontrol_new snd_aoc_ctl[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "PCM Playback Volume",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.private_value = PCM_PLAYBACK_VOLUME,
		.info = snd_aoc_ctl_info,
		.get = snd_aoc_ctl_get,
		.put = snd_aoc_ctl_put,
		.count = 1,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "PCM Playback Switch",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.private_value = PCM_PLAYBACK_MUTE,
		.info = snd_aoc_ctl_info,
		.get = snd_aoc_ctl_get,
		.put = snd_aoc_ctl_put,
		.count = 1,
	},
};

int snd_aoc_new_ctl(struct aoc_chip *chip)
{
	int err;
	unsigned int idx;

	strcpy(chip->card->mixername, "Aoc Mixer");
	for (idx = 0; idx < ARRAY_SIZE(snd_aoc_ctl); idx++) {
		err = snd_ctl_add(chip->card,
				  snd_ctl_new1(&snd_aoc_ctl[idx], chip));
		if (err < 0)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL(snd_aoc_new_ctl);
