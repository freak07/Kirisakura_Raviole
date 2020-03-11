// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA Driver on  AoC Audio Control
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "aoc_alsa_drv.h"
#include "aoc_alsa.h"
#include "../aoc-interface.h"

enum { NONBLOCKING = 0, BLOCKING = 1 };
enum { START, STOP };

static int aoc_service_audio_control(struct aoc_service_dev *dev,
				     const uint8_t *cmd, size_t cmd_size);

int aoc_audio_volume_set(struct aoc_alsa_stream *alsa_stream, uint32_t volume,
			 int src, int dst)
{
	int err;
	struct aoc_service_dev *dev;
	struct CMD_AUDIO_OUTPUT_SET_PARAMETER cmd;

	/* No volume control for capturing */
	if (alsa_stream->substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	dev = alsa_stream->chip->dev_alsa_output_control;
	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_PARAMETER_ID,
		     sizeof(cmd));

	/* Sink 0-4 with block ID from 16-20 */
	cmd.block = dst + AOC_AUDIO_SINK_BLOCK_ID_BASE;
	cmd.component = 0;
	cmd.key = src;
	cmd.val = volume;
	pr_debug("volume changed to: %d\n", volume);

	/* Send cmd to AOC */
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd));
	if (err < 0)
		pr_err("error in volme set\n");

	return err;
}

/*
 * Sending commands to AoC for setting parameters and start/stop the streams
 */
static int aoc_service_audio_control(struct aoc_service_dev *dev,
				     const uint8_t *cmd, size_t cmd_size)
{
	int err, count;
	uint8_t *buffer;
	int buffer_size = 1024;
	struct timeval tv0, tv1;

	buffer = kmalloc_array(buffer_size, sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		err = -ENOMEM;
		pr_err("memory allocation error!\n");
		return err;
	}

	/*
	 * TODO:   assuming only one user for the audio control channel.
	 * clear all the response messages from previous commands
	 */
	count = 0;
	while (((err = aoc_service_read(dev, buffer, buffer_size,
					NONBLOCKING)) >= 1) &&
	       (count < MAX_NUM_TRIALS_TO_GET_RESPONSE_FROM_AOC)) {
		count++;
	}
	if (count > 0)
		pr_debug("%d messages read for previous commands\n", count);

	/* Sending cmd to AoC */
	if ((aoc_service_write(dev, cmd, cmd_size, BLOCKING)) != cmd_size) {
		pr_err("failed to source command to audio-control\n");
		return -EAGAIN;
	}

#ifdef AOC_CMD_DEBUG_ENABLE
	do_gettimeofday(&tv0);
#endif /* AOC_CMD_DEBUG_ENABLE */

	/* Getting responses from Aoc for the command just sent */
	count = 0;
	while (((err = aoc_service_read(dev, buffer, buffer_size,
					NONBLOCKING)) < 1) &&
	       (count < MAX_NUM_TRIALS_TO_GET_RESPONSE_FROM_AOC)) {
		count++;
	}

#ifdef AOC_CMD_DEBUG_ENABLE
	do_gettimeofday(&tv1);
	pr_debug("Elapsed: %lu (usecs)\n", tv1.tv_usec - tv0.tv_usec);

	if (count > 0)
		pr_debug("%d times tried for response\n", count);
#endif /* AOC_CMD_DEBUG_ENABLE */

	if (err < 1) {
		pr_err("failed to get reply from audio-control: err %d\n",
			 err);
	} else if (err == 4) {
		pr_debug("audio control err code %#x\n", *(uint32_t *)buffer);
	} else {
		pr_debug("audio control err struct %d bytes\n", err);
	}

	kfree(buffer);
	return err < 1 ? -EAGAIN : 0;
}

static int
aoc_audio_playback_trigger_source(struct aoc_alsa_stream *alsa_stream, int cmd,
				  int src)
{
	int err;

	struct aoc_service_dev *dev;
	struct CMD_AUDIO_OUTPUT_SOURCE source;

	dev = alsa_stream->chip->dev_alsa_output_control;
	AocCmdHdrSet(&(source.parent), CMD_AUDIO_OUTPUT_SOURCE_ID,
		     sizeof(source));
	/* source On/Off */
	source.source = src;
	source.on = (cmd == START) ? 1 : 0;
	err = aoc_service_audio_control(dev, (uint8_t *)&source,
					sizeof(source));

	pr_debug("Source %d %s !\n", alsa_stream->idx,
		 cmd == START ? "on" : "off");

	return err;
}

static int aoc_audio_playback_trigger_bind(struct aoc_alsa_stream *alsa_stream,
					   int cmd, int src, int dst)
{
	int err;
	struct aoc_service_dev *dev;
	struct CMD_AUDIO_OUTPUT_BIND bind;

	dev = alsa_stream->chip->dev_alsa_output_control;
	AocCmdHdrSet(&(bind.parent), CMD_AUDIO_OUTPUT_BIND_ID, sizeof(bind));
	bind.bind = (cmd == START) ? 1 : 0;
	bind.src = src;
	bind.dst = dst;
	err = aoc_service_audio_control(dev, (uint8_t *)&bind, sizeof(bind));

	/* bind/unbind the source and dest */
	pr_debug("%s: src: %d- sink: %d!\n", cmd == START ? "bind" : "unbind",
		 src, dst);

	return err;
}

int aoc_audio_playback_set_params(struct aoc_alsa_stream *alsa_stream,
				  uint32_t channels, uint32_t samplerate,
				  uint32_t bps, bool pcm_float_fmt)
{
	int err;
	struct CMD_AUDIO_OUTPUT_EP_SETUP cmd;
	struct aoc_service_dev *dev;

	dev = alsa_stream->chip->dev_alsa_output_control;
	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_EP_SETUP_ID, sizeof(cmd));
	cmd.d.channel = alsa_stream->entry_point_idx;
	cmd.d.watermark = PLAYBACK_WATERMARK_DEFAULT;
	cmd.d.length = 0;
	cmd.d.address = 0;
	cmd.d.wraparound = true;
	cmd.d.metadata.offset = 0;

	switch (bps) {
	case 32:
		cmd.d.metadata.bits = WIDTH_32_BIT;
		break;
	case 24:
		cmd.d.metadata.bits = WIDTH_24_BIT;
		break;
	case 16:
		cmd.d.metadata.bits = WIDTH_16_BIT;
		break;
	case 8:
		cmd.d.metadata.bits = WIDTH_8_BIT;
		break;
	default:
		cmd.d.metadata.bits = WIDTH_32_BIT;
	}

	cmd.d.metadata.format =
		(pcm_float_fmt) ? FRMT_FLOATING_POINT : FRMT_FIXED_POINT;
	cmd.d.metadata.chan = channels;

	switch (samplerate) {
	case 48000:
		cmd.d.metadata.sr = SR_48KHZ;
		break;
	case 44100:
		cmd.d.metadata.sr = SR_44K1HZ;
		break;
	case 16000:
		cmd.d.metadata.sr = SR_16KHZ;
		break;
	case 8000:
		cmd.d.metadata.sr = SR_8KHZ;
		break;
	default:
		cmd.d.metadata.sr = SR_48KHZ;
	}

	pr_debug("chan =%d, sr=%d, bits=%d\n", cmd.d.metadata.chan,
		 cmd.d.metadata.sr, cmd.d.metadata.bits);

	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd));
	if (err < 0)
		pr_err("Error in set parameters\n");

	err = aoc_audio_playback_trigger_source(alsa_stream, START,
						alsa_stream->entry_point_idx);
	if (err < 0)
		pr_err("Error in set the source on\n");

	return err;
}

int aoc_audio_capture_set_params(struct aoc_alsa_stream *alsa_stream,
				 uint32_t channels, uint32_t samplerate,
				 uint32_t bps, bool pcm_float_fmt)
{
	int err = 0;
	int left, right; //two channels
	struct CMD_AUDIO_INPUT_MIC_RECORD_AP_SET_PARAMS cmd;
	struct aoc_service_dev *dev;

	dev = alsa_stream->chip->dev_alsa_input_control;
	if (!aoc_ring_flush_read_data(alsa_stream->dev->service, AOC_UP, 0)) {
		pr_err("aoc ring buffer could not be flushed\n");
		/* TODO differentiate different cases */
		return -EINVAL;
	}
	pr_debug("aoc ring buffer flushed\n");

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_MIC_RECORD_AP_SET_PARAMS_ID,
		     sizeof(cmd));

	left = alsa_stream->chip->default_mic_id;
	right = (channels > 1) ? left + 1 : left;
	cmd.pdm_mask = (1 << left) | (1 << right);
	cmd.period_ms = 10; /*TODO: how to make it configuratable*/
	cmd.num_periods = 4; /*TODO: how to make it configuratable*/

	switch (samplerate) {
	case 48000:
		cmd.sample_rate = SR_48KHZ;
		break;
	case 44100:
		cmd.sample_rate = SR_44K1HZ;
		break;
	case 16000:
		cmd.sample_rate = SR_16KHZ;
		break;
	case 8000:
		cmd.sample_rate = SR_8KHZ;
		break;
	default:
		cmd.sample_rate = SR_48KHZ;
	}

	switch (bps) {
	case 32:
		cmd.requested_format.bits = WIDTH_32_BIT;
		break;
	case 24:
		cmd.requested_format.bits = WIDTH_24_BIT;
		break;
	case 16:
		cmd.requested_format.bits = WIDTH_16_BIT;
		break;
	case 8:
		cmd.requested_format.bits = WIDTH_8_BIT;
		break;
	default:
		cmd.requested_format.bits = WIDTH_32_BIT;
	}
	cmd.requested_format.sr =
		cmd.sample_rate; /* TODO: double check format*/
	cmd.requested_format.format =
		(pcm_float_fmt) ? FRMT_FLOATING_POINT : FRMT_FIXED_POINT;

	cmd.requested_format.chan = channels;

	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd));
	if (err < 0)
		pr_err("capture parameter setup fail. errcode = %d\n", err);

	return err;
}

/* Start or stop the stream */
static int aoc_audio_capture_trigger(struct aoc_alsa_stream *alsa_stream,
				     int record_cmd)
{
	int err;
	struct CMD_HDR cmd;
	struct aoc_service_dev *dev;

	AocCmdHdrSet(&cmd,
		     (record_cmd == START) ?
			     CMD_AUDIO_INPUT_MIC_RECORD_AP_START_ID :
			     CMD_AUDIO_INPUT_MIC_RECORD_AP_STOP_ID,
		     sizeof(cmd));

	dev = alsa_stream->chip->dev_alsa_input_control;
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd));
	if (err < 0)
		pr_err("capture trigger fail!\n");

	return err;
}

int aoc_mic_loopback(struct aoc_chip *chip, int enable)
{
	int err;
	struct aoc_service_dev *dev;
	struct CMD_AUDIO_INPUT_ENABLE_MIC_LOOPBACK cmd;

	AocCmdHdrSet(&(cmd.parent),
		     ((enable == 1) ? CMD_AUDIO_INPUT_MIC_LOOPBACK_START_ID :
				      CMD_AUDIO_INPUT_MIC_LOOPBACK_STOP_ID),
		     sizeof(cmd));
	cmd.sample_rate = SR_48KHZ;

	dev = chip->dev_alsa_input_control;
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd));
	if (err < 0)
		pr_err("loopback fail!\n");

	return err;
}

/*
 * For capture: start recording
 * for playback: source on and bind source/sinks
 *
 * TODO: Capturing from four mics on board differ from other sources
 * (BT, USB,... I2S interface from headphone)
 */
int aoc_audio_start(struct aoc_alsa_stream *alsa_stream)
{
	int i, err = 0;
	int src, dst;

	src = alsa_stream->entry_point_idx;
	dst = alsa_stream->chip->default_sink_id;

	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		for (i = 0; i < MAX_NUM_OF_SINKS_PER_STREAM; i++) {
			/* TODO  should change to list for each stream */
			dst = alsa_stream->chip->sink_id_list[i];
			if (dst == -1)
				break;
			/* TODO on return value */
			err = aoc_audio_playback_trigger_bind(alsa_stream,
							      START, src, dst);
			if (err < 0)
				pr_err("audio playback binding start failed. errcode = %d\n",
				       err);
		}

	} else {
		err = aoc_audio_capture_trigger(alsa_stream, START);
		if (err < 0)
			pr_err("audio capture triggering start failed. errcode = %d\n",
			       err);
	}

	return err;
}

/*
 * For capture: stop recording
 * For playback: source off and unbind source/sinks
 */
int aoc_audio_stop(struct aoc_alsa_stream *alsa_stream)
{
	int i, err = 0;
	int src, dst;

	src = alsa_stream->entry_point_idx;
	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/* Unbinding source/sinks */
		for (i = MAX_NUM_OF_SINKS_PER_STREAM - 1; i >= 0; i--) {
			dst = alsa_stream->chip->sink_id_list[i];
			if (dst == -1)
				continue;
			err = aoc_audio_playback_trigger_bind(alsa_stream, STOP,
							      src, dst);
			if (err < 0)
				pr_err("audio playback unbinding failed. errcode = %d\n",
				       err);
		}

		/* Turn off audio source after unbinding source/sinks */
		err = aoc_audio_playback_trigger_source(alsa_stream, STOP, src);
		if (err < 0)
			pr_err("audio playback close source failed. errcode = %d\n",
			       err);
	} else {
		err = aoc_audio_capture_trigger(alsa_stream, STOP);
		if (err < 0)
			pr_err("audio capture stop failed. errcode = %d\n",
			       err);
	}

	return err;
}

int aoc_audio_read(struct aoc_alsa_stream *alsa_stream, void *dest,
		   uint32_t count)
{
	int err = 0;
	void *tmp;
	struct aoc_service_dev *dev = alsa_stream->dev;
	int avail;

	avail = aoc_ring_bytes_available_to_read(dev->service, AOC_UP);
	if (unlikely(avail < count)) {
		pr_err("error in read data from ringbuffer. avail = %d, toread = %d\n",
		       avail, count);
		err = -EFAULT;
		goto out;
	}

	tmp = (void *)(alsa_stream->substream->runtime->dma_area);
	err = aoc_service_read(dev, (void *)tmp, count, NONBLOCKING);
	if (unlikely(err != count)) {
		pr_err("error in read from buffer, unread data: %d bytes\n",
		       count - err);
		err = -EFAULT;
	}

	err = copy_to_user(dest, tmp, count);
	if (err != 0) {
		pr_err("error in copy data to user space. data uncopied = %d\n",
		       err);
		err = -EFAULT;
	}

out:
	return err < 0 ? err : 0;
}

int aoc_audio_write(struct aoc_alsa_stream *alsa_stream, void *src,
		    uint32_t count)
{
	int err = 0;
	struct aoc_service_dev *dev = alsa_stream->dev;
	void *tmp;
	int avail;

	avail = aoc_ring_bytes_available_to_write(dev->service, AOC_DOWN);
	if (unlikely(avail < count)) {
		pr_err("error in write data to ringbuffer. avail = %d, towrite = %d\n",
		       avail, count);
		err = -EFAULT;
		goto out;
	}

	tmp = (void *)(alsa_stream->substream->runtime->dma_area);
	err = copy_from_user(tmp, src, count);
	if (err != 0) {
		pr_err("error in copy data from user space. data unread = %d\n",
		       err);
		err = -EFAULT;
		goto out;
	}

	err = aoc_service_write(dev, tmp, count, NONBLOCKING);
	if (err != count) {
		pr_err("error in write data to buffer, unwritten data: %d bytes\n",
		       count - err);
		err = -EFAULT;
	}

out:
	return err < 0 ? err : 0;
}

/* PCM channel setup ??? */
static int aoc_audio_set_ctls_chan(struct aoc_alsa_stream *alsa_stream,
				   struct aoc_chip *chip)
{
	int err = 0;
	int src, dst, i;

	pr_debug(" Setting ALSA  volume(%d)\n", chip->volume);
	src = alsa_stream->entry_point_idx;
	for (i = 0; i < MAX_NUM_OF_SINKS_PER_STREAM; i++) {
		dst = alsa_stream->chip->sink_id_list[i];
		if (dst != -1) {
			err = aoc_audio_volume_set(alsa_stream, chip->volume,
						   src, dst);
			if (err < 0) {
				pr_err("error in volume setting. errcode = %d\n",
				       err);
				goto out;
			}
		}
	}

out:
	return err;
}

int aoc_audio_set_ctls(struct aoc_chip *chip)
{
	int i;
	int err = 0;

	/* change ctls for all substreams */
	for (i = 0; i < MAX_NUM_OF_SUBSTREAMS; i++) {
		if (chip->avail_substreams & (1 << i)) {
			pr_debug(" Setting %d stream i =%d\n",
				 chip->avail_substreams, i);

			if (!chip->alsa_stream[i]) {
				pr_debug(
					" No ALSA stream available?! %i:%p (%x)\n",
					i, chip->alsa_stream[i],
					chip->avail_substreams);
				err = 0;
			} else if (aoc_audio_set_ctls_chan(chip->alsa_stream[i],
							   chip) != 0) {
				pr_err("couldn't set the controls for stream %d\n",
				       i);
				err = -EINVAL;
			} else
				pr_debug("controls set for stream %d\n", i);
		}
	}
	return err;
}

int aoc_audio_set_params(struct aoc_alsa_stream *alsa_stream, uint32_t channels,
			 uint32_t samplerate, uint32_t bps, bool pcm_float_fmt)
{
	int err = 0;

	pr_debug(
		"setting ALSA channels(%d), samplerate(%d), bits-per-sample(%d)\n",
		channels, samplerate, bps);

	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		err = aoc_audio_playback_set_params(
			alsa_stream, channels, samplerate, bps, pcm_float_fmt);
	} else {
		err = aoc_audio_capture_set_params(
			alsa_stream, channels, samplerate, bps, pcm_float_fmt);
	}

	if (err < 0)
		goto out;

	/* Resend volume ctls-alsa_stream may not be open when first send */
	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		err = aoc_audio_set_ctls_chan(alsa_stream, alsa_stream->chip);
		if (err != 0) {
			pr_debug(
				"alsa controls in setting params not supported\n");
			err = -EINVAL;
		}
	}

out:
	return err;
}

int aoc_audio_setup(struct aoc_alsa_stream *alsa_stream)
{
	return 0;
}

int aoc_audio_open(struct aoc_alsa_stream *alsa_stream)
{
	return 0;
}

int aoc_audio_close(struct aoc_alsa_stream *alsa_stream)
{
	return 0;
}
