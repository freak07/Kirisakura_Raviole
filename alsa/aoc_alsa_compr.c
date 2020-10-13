// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA  Driver on Compr offload
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/version.h>

#include "aoc_alsa.h"
#include "aoc_alsa_drv.h"

/* TODO: the handler has to be changed based on the compress offload */
/*  the pointer should be modified based on the interrupt from AoC */
static enum hrtimer_restart aoc_compr_hrtimer_irq_handler(struct hrtimer *timer)
{
	struct aoc_alsa_stream *alsa_stream;
	struct aoc_service_dev *dev;
	unsigned long consumed;

	if (!timer) {
		pr_err("ERR: NULL timer pointer\n");
		return HRTIMER_NORESTART;
	}

	alsa_stream = container_of(timer, struct aoc_alsa_stream, hr_timer);

	if (!alsa_stream || !alsa_stream->cstream) {
		pr_err("ERR: NULL compress offload stream pointer\n");
		return HRTIMER_NORESTART;
	}

	/* Start the hr timer immediately for next period */
	aoc_timer_restart(alsa_stream);

	/*
	 * The number of bytes read/writtien should be the bytes in the buffer
	 * already played out in the case of playback. But this may not be true
	 * in the AoC ring buffer implementation, since the reader pointer in
	 * the playback case represents what has been read from the buffer,
	 * not what already played out .
	 */
	dev = alsa_stream->dev;

	if (aoc_ring_bytes_available_to_read(dev->service, AOC_DOWN) == 0) {
		pr_info("compress offload ring buffer is depleted\n");
		snd_compr_drain_notify(alsa_stream->cstream);
		return HRTIMER_RESTART;
	}

	consumed = aoc_ring_bytes_read(dev->service, AOC_DOWN);

	/* TODO: To do more on no pointer update? */
	if (consumed == alsa_stream->prev_consumed)
		return HRTIMER_RESTART;

	pr_debug("consumed = %lu, hw_ptr_base = %lu\n", consumed,
		 alsa_stream->hw_ptr_base);

	/* To deal with overlfow in Tx or Rx in int32_t */
	if (consumed < alsa_stream->prev_consumed) {
		alsa_stream->n_overflow++;
		pr_notice("overflow in Tx/Rx: %lu - %lu - %d times\n", consumed,
			  alsa_stream->prev_consumed, alsa_stream->n_overflow);
	}
	alsa_stream->prev_consumed = consumed;

	/* Update the pcm pointer */
	if (unlikely(alsa_stream->n_overflow)) {
		alsa_stream->pos =
			(consumed + 0x100000000 * alsa_stream->n_overflow -
			 alsa_stream->hw_ptr_base) % alsa_stream->buffer_size;
	} else {
		alsa_stream->pos = (consumed - alsa_stream->hw_ptr_base) %
				   alsa_stream->buffer_size;
	}

	/* Wake up the sleeping thread */
	if (alsa_stream->cstream)
		snd_compr_fragment_elapsed(alsa_stream->cstream);

	return HRTIMER_RESTART;
}

static int aoc_compr_playback_open(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd =
		(struct snd_soc_pcm_runtime *)cstream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct aoc_chip *chip =
		(struct aoc_chip *)snd_soc_card_get_drvdata(card);

	struct snd_compr_runtime *runtime = cstream->runtime;

	struct aoc_alsa_stream *alsa_stream = NULL;
	struct aoc_service_dev *dev = NULL;
	int idx;
	int err;

	if (mutex_lock_interruptible(&chip->audio_mutex)) {
		pr_err("ERR: interrupted whilst waiting for lock\n");
		return -EINTR;
	}

	idx = cstream->device->device;
	pr_notice("alsa compr offload open (%d)\n", idx);
	pr_debug("chip open (%d)\n", chip->opened);

	/* Find the corresponding aoc audio service */
	err = alloc_aoc_audio_service(rtd->dai_link->name, &dev);
	if (err < 0) {
		pr_err("ERR: fail to alloc service for %s",
		       rtd->dai_link->name);
		goto out;
	}

	alsa_stream = kzalloc(sizeof(struct aoc_alsa_stream), GFP_KERNEL);
	if (alsa_stream == NULL) {
		err = -ENOMEM;
		pr_err("ERR: no memory for %s", rtd->dai_link->name);
		goto out;
	}

	/* Initialise alsa_stream */
	alsa_stream->chip = chip;
	alsa_stream->cstream = cstream;
	alsa_stream->substream = NULL;
	alsa_stream->dev = dev;
	alsa_stream->idx = idx;

	/* No prepare() for compress offload, so do buffer flushing here */
	err = aoc_compr_offload_flush_buffer(alsa_stream);
	if (err != 0) {
		pr_err("fail to flush compress offload buffer: %s",
		       rtd->dai_link->name);
		goto out;
	}

	alsa_stream->hw_ptr_base =
		(cstream->direction == SND_COMPRESS_PLAYBACK) ?
			      aoc_ring_bytes_read(dev->service, AOC_DOWN) :
			      aoc_ring_bytes_written(dev->service, AOC_UP);
	pr_debug("compress offload hw_ptr_base =%lu\n",
		 alsa_stream->hw_ptr_base);

	alsa_stream->prev_consumed = alsa_stream->hw_ptr_base;
	alsa_stream->n_overflow = 0;

	err = aoc_audio_open(alsa_stream);
	if (err != 0) {
		pr_err("fail to audio open for %s", rtd->dai_link->name);
		goto out;
	}
	runtime->private_data = alsa_stream;
	chip->alsa_stream[idx] = alsa_stream;
	chip->opened |= (1 << idx);
	alsa_stream->open = 1;
	alsa_stream->draining = 1;

	alsa_stream->timer_interval_ns = COMPR_OFFLOAD_TIMER_INTERVAL_NANOSECS;
	hrtimer_init(&(alsa_stream->hr_timer), CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);
	alsa_stream->hr_timer.function = &aoc_compr_hrtimer_irq_handler;

	alsa_stream->entry_point_idx = idx;

	/* TODO: temporary compr offload volume set to protect speaker*/
	aoc_audio_volume_set(chip, 50, idx, 0);

	mutex_unlock(&chip->audio_mutex);

	return 0;
out:
	kfree(alsa_stream);
	if (dev) {
		free_aoc_audio_service(rtd->dai_link->name, dev);
		dev = NULL;
	}
	mutex_unlock(&chip->audio_mutex);

	pr_debug("pcm open err=%d\n", err);
	return err;
}

static int aoc_compr_playback_free(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd =
		(struct snd_soc_pcm_runtime *)cstream->private_data;
	struct snd_compr_runtime *runtime = cstream->runtime;

	struct aoc_alsa_stream *alsa_stream = runtime->private_data;
	struct aoc_chip *chip = alsa_stream->chip;
	int err;

	pr_debug("dai name %s, cstream %pK\n", rtd->dai_link->name, cstream);
	aoc_timer_stop_sync(alsa_stream);

	if (mutex_lock_interruptible(&chip->audio_mutex)) {
		pr_err("ERR: interrupted while waiting for lock\n");
		return -EINTR;
	}

	pr_notice("alsa compr offload close\n");
	free_aoc_audio_service(rtd->dai_link->name, alsa_stream->dev);
	/*
	 * Call stop if it's still running. This happens when app
	 * is force killed and we don't get a stop trigger.
	 */
	if (alsa_stream->running) {
		err = aoc_audio_stop(alsa_stream);
		alsa_stream->running = 0;
		if (err != 0)
			pr_err("ERR: failed to stop the stream\n");
	}

	if (alsa_stream->open) {
		alsa_stream->open = 0;
		aoc_audio_close(alsa_stream);
	}
	if (alsa_stream->chip)
		alsa_stream->chip->alsa_stream[alsa_stream->idx] = NULL;
	kfree(alsa_stream);
	/*
	 * Do not free up alsa_stream here, it will be freed up by
	 * runtime->private_free callback we registered in *_open above
	 * TODO: no such operation for compress offload
	 */
	chip->opened &= ~(1 << alsa_stream->idx);

	mutex_unlock(&chip->audio_mutex);

	return 0;
}

static int aoc_compr_open(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream)
{
	int ret = 0;
	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		ret = aoc_compr_playback_open(cstream);

	return ret;
}

static int aoc_compr_free(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream)
{
	int ret = 0;
	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		ret = aoc_compr_playback_free(cstream);

	return ret;
}

static int aoc_compr_prepare(struct snd_compr_stream *cstream)
{
	int err;
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct aoc_alsa_stream *alsa_stream = runtime->private_data;
	struct aoc_service_dev *dev = alsa_stream->dev;

	/* No prepare() for compress offload, so do buffer flushing here */
	err = aoc_compr_offload_flush_buffer(alsa_stream);
	if (err != 0) {
		pr_err("ERR: fail to flush compress offload buffer\n");
		return -EFAULT;
	}

	alsa_stream->hw_ptr_base =
		(cstream->direction == SND_COMPRESS_PLAYBACK) ?
			      aoc_ring_bytes_read(dev->service, AOC_DOWN) :
			      aoc_ring_bytes_written(dev->service, AOC_UP);
	pr_debug("compress offload hw_ptr_base =%lu\n",
		 alsa_stream->hw_ptr_base);

	return 0;
}

static int aoc_compr_trigger(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream, int cmd)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct aoc_alsa_stream *alsa_stream = runtime->private_data;
	int err = 0;

	pr_debug("%s: cmd = %d\n", __func__, cmd);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		pr_debug("%s: SNDRV_PCM_TRIGGER_START\n", __func__);
		if (alsa_stream->running)
			break;

		/* start timer first to avoid underrun/overrun */
		pr_debug("%s: start timer\n", __func__);
		aoc_timer_start(alsa_stream);

		/* Decoder type, MP3 or AAC, hardcoded as MP3 for now */
		err = aoc_compr_offload_setup(alsa_stream, 1);
		if (err < 0) {
			pr_err("ERR:%d decoder setup fail\n", err);
			goto out;
		}

		err = aoc_audio_start(alsa_stream);
		if (err == 0) {
			alsa_stream->running = 1;
		} else {
			pr_err(" Failed to START alsa device (%d)\n", err);
		}
		break;

	case SNDRV_PCM_TRIGGER_STOP:
		pr_debug("%s: SNDRV_PCM_TRIGGER_STOP\n", __func__);
		if (alsa_stream->running) {
			err = aoc_audio_stop(alsa_stream);
			if (err != 0)
				pr_err("failed to STOP alsa device (%d)\n",
				       err);
			alsa_stream->running = 0;
		}

		aoc_compr_prepare(cstream);
		break;

	case SND_COMPR_TRIGGER_DRAIN:
		pr_debug("%s: SNDRV_PCM_TRIGGER_DRAIN\n", __func__);
		break;

	case SND_COMPR_TRIGGER_PARTIAL_DRAIN:
		pr_debug("%s: SNDRV_PCM_TRIGGER_PARTIAL_DRAIN\n", __func__);
		break;

	case SND_COMPR_TRIGGER_NEXT_TRACK:
		pr_debug("%s: SND_COMPR_TRIGGER_NEXT_TRACK\n", __func__);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pr_debug("%s: SNDRV_PCM_TRIGGER_PAUSE_PUSH\n", __func__);
		if (alsa_stream->running) {
			err = aoc_compr_pause(alsa_stream);
			if (err != 0)
				pr_err("failed to pause alsa device (%d)\n",
				       err);
		}
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pr_debug("%s: SNDRV_PCM_TRIGGER_PAUSE_RELEASE\n", __func__);
		if (alsa_stream->running) {
			err = aoc_compr_resume(alsa_stream);
			if (err != 0)
				pr_err("failed to resume alsa device (%d)\n",
				       err);
		}
		break;

	default:
		err = -EINVAL;
	}
out:
	return err;
}

static int aoc_compr_pointer(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream,
			     struct snd_compr_tstamp *arg)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct aoc_alsa_stream *alsa_stream = runtime->private_data;

	pr_debug("%s, %pK, %pK\n", __func__, runtime, arg);

	arg->byte_offset = alsa_stream->pos;
	arg->copied_total =
		alsa_stream->prev_consumed - alsa_stream->hw_ptr_base;

	arg->pcm_io_frames = aoc_compr_offload_get_io_samples(alsa_stream);
	arg->sampling_rate = alsa_stream->params_rate;

	pr_debug(
		"aoc compr pointer - total bytes avail: %llu  copied: %u  diff: %llu, iosampes=%u\n",
		runtime->total_bytes_available, arg->copied_total,
		runtime->total_bytes_available - arg->copied_total,
		arg->pcm_io_frames);

	return 0;
}

static int aoc_compr_ack(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream, size_t count)
{
	struct snd_compr_runtime *runtime = cstream->runtime;

	pr_debug("%s, %pK, %zu\n", __func__, runtime, count);

	return 0;
}

static int aoc_compr_playback_copy(struct snd_compr_stream *cstream,
				   char __user *buf, size_t count)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct aoc_alsa_stream *alsa_stream = runtime->private_data;
	int err = 0;

	err = aoc_audio_write(alsa_stream, buf, count);
	if (err < 0) {
		pr_err("ERR:%d failed to write to buffer\n", err);
		return err;
	}

	return count;
}

static int aoc_compr_copy(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream, char __user *buf,
			  size_t count)
{
	int ret = 0;

	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		ret = aoc_compr_playback_copy(cstream, buf, count);

	return ret;
}

static int aoc_compr_get_caps(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream,
			      struct snd_compr_caps *arg)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	int ret = 0;

	pr_debug("%s, %pK, %pK\n", __func__, runtime, arg);

	return ret;
}

static int aoc_compr_get_codec_caps(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream,
				    struct snd_compr_codec_caps *codec)
{
	pr_debug("%s, %d\n", __func__, codec->codec);

	switch (codec->codec) {
	case SND_AUDIOCODEC_MP3:
		break;
	case SND_AUDIOCODEC_AAC:
		break;
	default:
		pr_err("%s: Unsupported audio codec %d\n", __func__,
		       codec->codec);
		return -EINVAL;
	}

	return 0;
}

static int aoc_compr_set_metadata(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream,
				  struct snd_compr_metadata *metadata)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	int ret = 0;

	pr_debug("%s %pK, %pK\n", __func__, runtime, metadata);

	return ret;
}

static int aoc_compr_get_metadata(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream,
				  struct snd_compr_metadata *metadata)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	int ret = 0;

	pr_debug("%s %pK, %pK\n", __func__, runtime, metadata);

	return ret;
}

static int aoc_compr_set_params(EXTRA_ARG_LINUX_5_9 struct snd_compr_stream *cstream,
				struct snd_compr_params *params)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct aoc_alsa_stream *alsa_stream = runtime->private_data;

	uint8_t *buffer;
	int buffer_size;

	pr_debug("%s, fragment size = %d, number of fragment = %d\n", __func__,
		 params->buffer.fragment_size, params->buffer.fragments);

	/* Memory allocation in runtime, based on segment size and the number of segment */
	buffer_size = params->buffer.fragment_size * params->buffer.fragments;

	pr_debug("%s buffer size: %d\n", __func__, buffer_size);

	buffer = kmalloc_array(buffer_size, sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		pr_err("ERR: no memory\n");
		return -ENOMEM;
	}

	runtime->buffer = buffer;
	alsa_stream->buffer_size = buffer_size;
	alsa_stream->period_size = params->buffer.fragment_size;
	alsa_stream->params_rate = params->codec.sample_rate;

	/* TODO: send the codec info to AoC ? */

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
static const struct snd_compress_ops snd_aoc_compr_ops = {
#else
static const struct snd_compr_ops snd_aoc_compr_ops = {
#endif
	.open = aoc_compr_open,
	.free = aoc_compr_free,
	.set_params = aoc_compr_set_params,
	.set_metadata = aoc_compr_set_metadata,
	.get_metadata = aoc_compr_get_metadata,
	.trigger = aoc_compr_trigger,
	.pointer = aoc_compr_pointer,
	.copy = aoc_compr_copy,
	.ack = aoc_compr_ack,
	.get_caps = aoc_compr_get_caps,
	.get_codec_caps = aoc_compr_get_codec_caps,
};

static int aoc_compr_new(EXTRA_ARG_LINUX_5_9 struct snd_soc_pcm_runtime *rtd)
{
	pr_debug("%s, %pK", __func__, rtd);

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
static const struct snd_soc_component_driver aoc_compr_component = {
	.name = "AoC COMPR",
	.compress_ops = &snd_aoc_compr_ops,
	.pcm_construct = aoc_compr_new,
};
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0))
static const struct snd_soc_component_driver aoc_compr_component = {
	.name = "AoC COMPR",
	.compr_ops = &snd_aoc_compr_ops,
	.pcm_new = aoc_compr_new,
};
#else
static const struct snd_soc_platform_driver aoc_compr_platform = {
	.compr_ops = &snd_aoc_compr_ops,
	.pcm_new = aoc_compr_new,
};
#endif

static int aoc_compr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int err = 0;

	pr_debug("%s", __func__);

	if (!np)
		return -EINVAL;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0))
	err = devm_snd_soc_register_component(dev, &aoc_compr_component, NULL,
					      0);
	if (err)
		pr_err("ERR:%d fail to reigster aoc pcm comp\n", err);
#else
	err = devm_snd_soc_register_platform(dev, &aoc_compr_platform);
	if (err) {
		pr_err("ERR:%d fail to reigster aoc pcm platform %d", err);
	}
#endif
	return err;
}

static const struct of_device_id aoc_compr_of_match[] = {
	{
		.compatible = "google-aoc-snd-compr",
	},
	{},
};
MODULE_DEVICE_TABLE(of, aoc_compr_of_match);

static struct platform_driver aoc_compr_drv = {
    .driver =
        {
            .name = "google-aoc-snd-compr",
            .of_match_table = aoc_compr_of_match,
        },
    .probe = aoc_compr_probe,
};

int aoc_compr_init(void)
{
	int err;

	pr_debug("%s", __func__);
	err = platform_driver_register(&aoc_compr_drv);
	if (err) {
		pr_err("ERR:%d fail in registering aoc compr drv\n", err);
		return err;
	}

	return 0;
}

void aoc_compr_exit(void)
{
	platform_driver_unregister(&aoc_compr_drv);
}
