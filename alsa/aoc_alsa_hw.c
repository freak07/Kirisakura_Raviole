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

#include "../aoc-interface.h"
#include "aoc_alsa.h"
#include "aoc_alsa_drv.h"

enum { NONBLOCKING = 0, BLOCKING = 1 };
enum { START, STOP, VOICE_TX_MODE, VOICE_RX_MODE, OFFLOAD_MODE };

static int aoc_service_audio_control(struct aoc_service_dev *dev,
				     const uint8_t *cmd, size_t cmd_size,
				     uint8_t *response);

int aoc_audio_volume_set(struct aoc_alsa_stream *alsa_stream, uint32_t volume,
			 int src, int dst)
{
	int err;
	struct aoc_service_dev *dev;
	struct CMD_AUDIO_OUTPUT_SET_PARAMETER cmd;

	/* No volume control for capturing */
	if (alsa_stream->substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	/* Haptics in AoC does not have adjustable volume */
	if (src == HAPTICS)
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
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					NULL);
	if (err < 0)
		pr_err("ERR:%d in volume set\n", err);

	return err;
}
/*
 * Sending commands to AoC for setting parameters and start/stop the streams
 */
static int aoc_service_audio_control(struct aoc_service_dev *dev,
				     const uint8_t *cmd, size_t cmd_size,
				     uint8_t *response)
{
	int err, count;
	uint8_t *buffer;
	int buffer_size = 1024;
	struct timeval tv0, tv1;

	if (!dev || !cmd)
		return -EINVAL;

#ifndef ALSA_AOC_CMD_LOG_DISABLE
	pr_notice(ALSA_AOC_CMD " cmd [%s] id %#06x, size %zu\n",
		  CMD_CHANNEL(dev), ((struct CMD_HDR *)cmd)->id, cmd_size);
#endif

	buffer = kmalloc_array(buffer_size, sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		err = -ENOMEM;
		pr_err("ERR: no memory!\n");
		return err;
	}

	/*
	 * TODO: assuming only one user for the audio control channel.
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
	if ((aoc_service_write(dev, cmd, cmd_size, NONBLOCKING)) != cmd_size) {
		pr_err(ALSA_AOC_CMD " ERR: ring full - cmd id %#06x\n",
		  ((struct CMD_HDR *)cmd)->id);
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
		pr_err(ALSA_AOC_CMD " ERR:timeout - cmd [%s] id %#06x\n",
		       CMD_CHANNEL(dev), ((struct CMD_HDR *)cmd)->id);
		print_hex_dump(KERN_ERR, ALSA_AOC_CMD" :mem ", DUMP_PREFIX_OFFSET, 16,
			       1, cmd, cmd_size, false);
	} else if (err == 4) {
		pr_err(ALSA_AOC_CMD " ERR:%#x - cmd [%s] id %#06x\n",
		       *(uint32_t *)buffer, CMD_CHANNEL(dev),
		       ((struct CMD_HDR *)cmd)->id);
		print_hex_dump(KERN_ERR, ALSA_AOC_CMD" :mem ", DUMP_PREFIX_OFFSET, 16,
			       1, cmd, cmd_size, false);
	} else {
		pr_debug(ALSA_AOC_CMD
			 " cmd [%s] id %#06x, reply mesg size %d\n",
			 ((struct CMD_HDR *)cmd)->id, err);
	}

	if (response != NULL)
		memcpy(response, buffer, cmd_size);

	kfree(buffer);
	return err < 1 ? -EAGAIN : 0;
}

int aoc_set_builtin_mic_power_state(struct aoc_chip *chip, int iMic, int state)
{
	int err;
	struct aoc_service_dev *dev;
	struct CMD_AUDIO_INPUT_MIC_POWER_ON cmd_on;
	struct CMD_AUDIO_INPUT_MIC_POWER_OFF cmd_off;

	dev = chip->dev_alsa_input_control;
	if (state == 1) {
		AocCmdHdrSet(&(cmd_on.parent), CMD_AUDIO_INPUT_MIC_POWER_ON_ID,
			     sizeof(cmd_on));
		cmd_on.mic_index = iMic;
		err = aoc_service_audio_control(dev, (uint8_t *)&cmd_on,
						sizeof(cmd_on), NULL);
	} else {
		AocCmdHdrSet(&(cmd_off.parent),
			     CMD_AUDIO_INPUT_MIC_POWER_OFF_ID, sizeof(cmd_off));
		cmd_off.mic_index = iMic;
		err = aoc_service_audio_control(dev, (uint8_t *)&cmd_off,
						sizeof(cmd_off), NULL);
	}

	if (err < 0)
		pr_err("ERR:%d in set mic state\n", err);

	return err < 0 ? err : 0;
}

int aoc_get_builtin_mic_power_state(struct aoc_chip *chip, int iMic)
{
	int err;
	struct CMD_AUDIO_INPUT_MIC_GET_POWER_STATE cmd;
	struct aoc_service_dev *dev;

	dev = chip->dev_alsa_input_control;
	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_MIC_GET_POWER_STATE_ID,
		     sizeof(cmd));

	cmd.mic_index = iMic;

	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					(uint8_t *)&cmd);
	if (err < 0)
		pr_err("ERR:%d in get mic state\n", err);

	return err < 0 ? err : cmd.power_state;
}

int aoc_get_dsp_state(struct aoc_chip *chip)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_DSP_STATE cmd;
	struct aoc_service_dev *dev;

	dev = chip->dev_alsa_output_control;
	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_DSP_STATE_ID,
		     sizeof(cmd));

	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					(uint8_t *)&cmd);
	if (err < 0)
		pr_err("Error in get aoc dsp state !\n");

	return err < 0 ? err : cmd.mode;
}

int aoc_get_sink_state(struct aoc_chip *chip, int iSink)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_SINK_PROCESSING_STATE cmd;
	struct aoc_service_dev *dev;

	dev = chip->dev_alsa_output_control;
	AocCmdHdrSet(&(cmd.parent),
		     CMD_AUDIO_OUTPUT_GET_SINK_PROCESSING_STATE_ID,
		     sizeof(cmd));

	cmd.sink = iSink;
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					(uint8_t *)&cmd);
	if (err < 0)
		pr_err("Error in get aoc sink processing state !\n");

	pr_info("sink_state:%d - %d\n", iSink, cmd.mode);
	return err < 0 ? err : cmd.mode;
}

static int aoc_haptics_set_mode(struct aoc_alsa_stream *alsa_stream, int mode)
{
	int err;
	struct CMD_AUDIO_OUTPUT_CFG_HAPTICS cmd;
	struct aoc_service_dev *dev;

	dev = alsa_stream->chip->dev_alsa_output_control;
	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_CFG_HAPTICS_ID,
		     sizeof(cmd));

	cmd.mode = mode;

	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					NULL);
	if (err < 0)
		pr_err("ERR:%d in set haptics mode\n", err);

	return err < 0 ? err : 0;
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
	/* source mode: playbckOn/Off/TX/Rx/Offload */
	source.source = src;
	switch (cmd) {
	case START:
		source.mode = ENTRYPOINT_MODE_PLAYBACK;
		if (alsa_stream->entry_point_idx == HAPTICS) {
			source.mode = ENTRYPOINT_MODE_HAPTICS;
		}
		break;
	case STOP:
		source.mode = ENTRYPOINT_MODE_OFF;
		break;
	case VOICE_TX_MODE:
		source.mode = ENTRYPOINT_MODE_VOICE_TX;
		break;
	case VOICE_RX_MODE:
		source.mode = ENTRYPOINT_MODE_VOICE_RX;
		break;
	case OFFLOAD_MODE:
		source.mode = ENTRYPOINT_MODE_DECODE_OFFLOAD;
		break;
	default:
		source.mode = ENTRYPOINT_MODE_OFF;
	}

	/*source.mode = (cmd == START) ? ENTRYPOINT_MODE_PLAYBACK :
   * ENTRYPOINT_MODE_OFF;*/
	err = aoc_service_audio_control(dev, (uint8_t *)&source, sizeof(source),
					NULL);

	pr_debug("Source %d %s !\n", alsa_stream->idx,
		 cmd == START ? "on" : "off");

	if (alsa_stream->entry_point_idx == HAPTICS) {
		//source.mode = ENTRYPOINT_MODE_HAPTICS ;
		err = aoc_haptics_set_mode(alsa_stream, HAPTICS_MODE_PCM);
		if (err < 0)
			pr_err("ERR:%d in haptics setup\n", err);
	}

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
	err = aoc_service_audio_control(dev, (uint8_t *)&bind, sizeof(bind),
					NULL);

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

	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					NULL);
	if (err < 0)
		pr_err("ERR:%d in set parameters\n", err);

	err = aoc_audio_playback_trigger_source(alsa_stream, START,
						alsa_stream->entry_point_idx);
	if (err < 0)
		pr_err("ERR:%d in source on\n", err);

	return err;
}

int aoc_audio_capture_set_params(struct aoc_alsa_stream *alsa_stream,
				 uint32_t channels, uint32_t samplerate,
				 uint32_t bps, bool pcm_float_fmt)
{
	int i, iMic, err = 0;
	//int left, right; // two channels
	struct CMD_AUDIO_INPUT_MIC_RECORD_AP_SET_PARAMS cmd;
	struct aoc_service_dev *dev;

	dev = alsa_stream->chip->dev_alsa_input_control;
	if (!aoc_ring_flush_read_data(alsa_stream->dev->service, AOC_UP, 0)) {
		pr_err("ERR: ring buffer flush fail\n");
		/* TODO differentiate different cases */
		return -EINVAL;
	}
	pr_debug("aoc ring buffer flushed\n");

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_MIC_RECORD_AP_SET_PARAMS_ID,
		     sizeof(cmd));

	if (channels < 1 || channels > NUM_OF_BUILTIN_MIC)
		pr_err("ERR: wrong channel number %d for capture\n", channels);

	/* TODO: more checks on mic id */
	cmd.pdm_mask = 0; /* in case it is not initialized as zero */
	for (i = 0; i < channels; i++) {
		iMic = alsa_stream->chip->buildin_mic_id_list[i];
		if (iMic != -1) {
			cmd.pdm_mask = cmd.pdm_mask | (1 << iMic);
		} else {
			pr_err("ERR: wrong mic id -1\n");
		}
	}

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
		cmd.requested_format.bits = WIDTH_32_BIT; /* TODO: tinycap limitation */
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

	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					NULL);
	if (err < 0)
		pr_err("ERR:%d in capture parameter setup\n", err);

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
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					NULL);
	if (err < 0)
		pr_err("ERR:%d in capture trigger\n", err);

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
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					NULL);
	if (err < 0)
		pr_err("ERR:%d in mic loopback\n", err);

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
				pr_err("ERR:%d in playback bind start\n", err);
		}

	} else {
		err = aoc_audio_capture_trigger(alsa_stream, START);
		if (err < 0)
			pr_err("ERR:%d in capture start\n", err);
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
				pr_err("ERR:%d in playback unbind\n", err);
		}

		/* Turn off audio source after unbinding source/sinks */
		err = aoc_audio_playback_trigger_source(alsa_stream, STOP, src);
		if (err < 0)
			pr_err("ERR:%d in playback source off\n", err);
	} else {
		err = aoc_audio_capture_trigger(alsa_stream, STOP);
		if (err < 0)
			pr_err("ERR:%d in capture stop\n",err);
	}

	return err;
}

/* TODO: this function is modified to deal with the issue where ALSA appl_ptr
 * and the reader pointer in AoC ringer buffer are out-of-sync due to overflow
 */
int aoc_audio_read(struct aoc_alsa_stream *alsa_stream, void *dest,
		   uint32_t count)
{
	int err = 0;
	void *tmp;
	struct aoc_service_dev *dev = alsa_stream->dev;
	int avail;

	avail = aoc_ring_bytes_available_to_read(dev->service, AOC_UP);

	if (unlikely(avail < count)) {
		pr_err("ERR: overrun in audio capture. avail = %d, toread = %d\n",
		       avail, count);
	}

	/* Only read bytes available in the ring buffer */
	count = avail < count ? avail : count;
	if (count == 0)
		return 0;

	tmp = (void *)(alsa_stream->substream->runtime->dma_area);
	err = aoc_service_read(dev, (void *)tmp, count, NONBLOCKING);
	if (unlikely(err != count)) {
		pr_err("ERR: %d bytes not read from ring buffer\n", count - err);
	}

	err = copy_to_user(dest, tmp, count);
	if (err != 0) {
		pr_err("ERR: %d bytes not copied to user space\n", err);
		err = -EFAULT;
	}

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
		pr_err("ERR: inconsistent write/read pointers, avail = %d, towrite = %d\n", avail,
		       count);
		err = -EFAULT;
		goto out;
	}

	tmp = (void *)(alsa_stream->substream->runtime->dma_area);
	err = copy_from_user(tmp, src, count);
	if (err != 0) {
		pr_err("ERR: %d bytes not read from user space\n",
		       err);
		err = -EFAULT;
		goto out;
	}

	err = aoc_service_write(dev, tmp, count, NONBLOCKING);
	if (err != count) {
		pr_err("ERR: unwritten data - %d bytes\n", count - err);
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
				pr_err("ERR:%d in volume setting\n", err);
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
				pr_err("ERR: couldn't set controls for stream %d\n",
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

static int aoc_audio_modem_input(struct aoc_alsa_stream *alsa_stream,
				 int input_cmd)
{
	int err;
	struct CMD_HDR cmd;
	struct aoc_service_dev *dev;

	AocCmdHdrSet(&cmd,
		     (input_cmd == START) ?
			     CMD_AUDIO_INPUT_MODEM_INPUT_START_ID :
			     CMD_AUDIO_INPUT_MODEM_INPUT_STOP_ID,
		     sizeof(cmd));

	dev = alsa_stream->chip->dev_alsa_input_control;
	err = aoc_service_audio_control(dev, (uint8_t *)&cmd, sizeof(cmd),
					NULL);
	if (err < 0)
		pr_err("ERR:%d modem input setup fail!\n", err);

	return err;
}

/* TODO: entry point idx and sink id should be specified  in the alsa_stream */
int prepare_phonecall(struct aoc_alsa_stream *alsa_stream)
{
	int err;
	int src = alsa_stream->entry_point_idx;

	pr_debug("prepare phone call - dev %d\n", alsa_stream->entry_point_idx);
	if (src != 4)
		return 0;

	err = aoc_audio_modem_input(alsa_stream, START);
	if (err < 0)
		pr_err("ERR:%d modem input start fail\n", err);
	pr_notice("modem input STARTED\n");

	/* Tx */
	err = aoc_audio_playback_trigger_bind(alsa_stream, START, 3, 3);
	if (err < 0)
		pr_err("ERR:%d playback bind start fail\n", err);

	/* refactor needed */
	alsa_stream->entry_point_idx = 3;
	err = aoc_audio_playback_set_params(alsa_stream, 2, 48000, 32, false);
	alsa_stream->entry_point_idx = 4;
	if (err < 0)
		pr_err("ERR:%d playback set params fail\n", err);

	err = aoc_audio_playback_trigger_source(alsa_stream, VOICE_TX_MODE, 3);
	if (err < 0)
		pr_err("ERR:%d playback source start fail\n", err);

	/* Rx */
	err = aoc_audio_playback_trigger_bind(alsa_stream, START, 4, 0);
	if (err < 0)
		pr_err("ERR:%d playback bind start fail\n", err);

	err = aoc_audio_playback_set_params(alsa_stream, 2, 48000, 32, false);
	if (err < 0)
		pr_err("ERR:%d playback set params fail\n", err);

	err = aoc_audio_playback_trigger_source(alsa_stream, VOICE_RX_MODE, 4);
	if (err < 0)
		pr_err("ERR:%d playback source voice RX mode setup fail\n", err);

	return 0;
}

int teardown_phonecall(struct aoc_alsa_stream *alsa_stream)
{
	int err = 0;
	int src = alsa_stream->entry_point_idx;

	pr_debug("stop phone call - dev %d\n", alsa_stream->entry_point_idx);
	if (src != 4)
		return 0;

	/* unbind */
	err = aoc_audio_playback_trigger_bind(alsa_stream, STOP, 4, 0);
	if (err < 0)
		pr_err("ERR:%d playback bind stop fail\n", err);

	err = aoc_audio_playback_trigger_bind(alsa_stream, STOP, 3, 3);
	if (err < 0)
		pr_err("ERR:%d playback bind stop fail\n", err);

	/* source off */
	err = aoc_audio_playback_trigger_source(alsa_stream, STOP, 4);
	if (err < 0)
		pr_err("ERR:%d playback source stop fail\n", err);

	err = aoc_audio_playback_trigger_source(alsa_stream, STOP, 3);
	if (err < 0)
		pr_err("ERR:%d playback source stop fail\n", err);

	err = aoc_audio_modem_input(alsa_stream, STOP);
	if (err < 0)
		pr_err("ERR:%d modem input stop fail\n", err);

	pr_notice("modem input STOPPED\n");

	return 0;
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
