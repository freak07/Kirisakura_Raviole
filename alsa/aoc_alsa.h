/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Google Whitechapel AoC ALSA Driver
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef AOC_ALSA_H
#define AOC_ALSA_H

#include <linux/device.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/jack.h>
#include <sound/soc.h>

#define ALSA_AOC_CMD "alsa-aoc"
#define CMD_INPUT_CHANNEL "audio_input_control"
#define CMD_OUTPUT_CHANNEL "audio_output_control"
#define CMD_CHANNEL(_dev)                                                      \
	(strcmp(dev_name(&(_dev)->dev), CMD_INPUT_CHANNEL)) ? "output" : "input"

#define AOC_CMD_DEBUG_ENABLE
#define MAX_NUM_TRIALS_TO_GET_RESPONSE_FROM_AOC 1000000

/* Default mic and sink for audio capturing/playback */
#define DEFAULT_MICPHONE_ID 0
#define NUM_OF_BUILTIN_MIC 4
#define DEFAULT_AUDIO_SINK_ID 0
#define MAX_NUM_OF_SINKS_PER_STREAM 2

/* TODO: the exact number has to be determined based on hardware platform*/
#define MAX_NUM_OF_SUBSTREAMS 12
#define MAX_NUM_OF_SINKS 5
#define AVAIL_SUBSTREAMS_MASK 0x0fff

#define AOC_AUDIO_SINK_BLOCK_ID_BASE 16

/* TODO: may not needed*/
#define PLAYBACK_WATERMARK_DEFAULT 48000

#define alsa2chip(vol) (vol) /* Convert alsa to chip volume */
#define chip2alsa(vol) (vol) /* Convert chip to alsa volume */

enum { CTRL_VOL_MUTE, CTRL_VOL_UNMUTE };
enum {
	PCM_PLAYBACK_VOLUME,
	PCM_PLAYBACK_MUTE,
	BUILDIN_MIC_POWER_STATE,
	BUILDIN_MIC_CAPTURE_LIST
};
enum { ULL = 0, LL0, LL1, LL2, LL3, DEEP_BUFFER, OFF_LOAD, HAPTICS };
enum { BUILTIN_MIC0 = 0, BUILTIN_MIC1, BUILTIN_MIC2, BUILTIN_MIC3 };

struct aoc_chip {
	struct snd_card *card;
	struct snd_soc_jack jack; /* TODO: temporary use, need refactor  */

	uint32_t avail_substreams;
	struct aoc_alsa_stream *alsa_stream[MAX_NUM_OF_SUBSTREAMS];

	struct aoc_service_dev *dev_alsa_output_control;
	struct aoc_service_dev *dev_alsa_input_control;
	struct aoc_service_dev *dev_alsa_stream[MAX_NUM_OF_SUBSTREAMS];

	int default_mic_id;
	int buildin_mic_id_list[NUM_OF_BUILTIN_MIC];

	int default_sink_id;
	int sink_id_list[MAX_NUM_OF_SINKS_PER_STREAM];

	int volume;
	int old_volume; /* Store the volume value while muted */
	int mute;

	int mic_loopback_enabled;
	unsigned int opened;
	struct mutex audio_mutex;
};

struct aoc_alsa_stream {
	struct aoc_chip *chip;
	struct snd_pcm_substream *substream;
	struct timer_list timer; /* For advancing the hw ptr */
	struct hrtimer hr_timer; /* For advancing the hw ptr */

	struct aoc_service_dev *dev;
	int idx; /* PCM device number */
	int entry_point_idx; /* Index of entry point, same as idx in playback */

	int channels; /* Number of channels in audio */
	int params_rate; /* Sampling rate */
	int pcm_format_width; /* Number of bits */
	bool pcm_float_fmt; /* Floating point */

	unsigned int period_size;
	unsigned int buffer_size;
	unsigned int pos;
	unsigned long hw_ptr_base; /* read/write pointers in ring buffer */
	unsigned long prev_consumed;
	int n_overflow;
	int open;
	int running;
	int draining;
};

void aoc_timer_start(struct aoc_alsa_stream *alsa_stream);
void aoc_timer_restart(struct aoc_alsa_stream *alsa_stream);
void aoc_timer_stop(struct aoc_alsa_stream *alsa_stream);
void aoc_timer_stop_sync(struct aoc_alsa_stream *alsa_stream);

int snd_aoc_new_ctl(struct aoc_chip *chip);
int snd_aoc_new_pcm(struct aoc_chip *chip);

int aoc_audio_setup(struct aoc_alsa_stream *alsa_stream);
int aoc_audio_open(struct aoc_alsa_stream *alsa_stream);
int aoc_audio_close(struct aoc_alsa_stream *alsa_stream);
int aoc_audio_set_params(struct aoc_alsa_stream *alsa_stream, uint32_t channels,
			 uint32_t samplerate, uint32_t bps, bool pcm_float_fmt);
int aoc_audio_start(struct aoc_alsa_stream *alsa_stream);
int aoc_audio_stop(struct aoc_alsa_stream *alsa_stream);
int aoc_audio_set_ctls(struct aoc_chip *chip);

int aoc_set_builtin_mic_power_state(struct aoc_chip *chip, int iMic, int state);
int aoc_get_builtin_mic_power_state(struct aoc_chip *chip, int iMic);
int aoc_get_dsp_state(struct aoc_chip *chip);
int aoc_get_sink_state(struct aoc_chip *chip, int iSink);

int aoc_audio_write(struct aoc_alsa_stream *alsa_stream, void *src,
		    uint32_t count);
int aoc_audio_read(struct aoc_alsa_stream *alsa_stream, void *dest,
		   uint32_t count);

int prepare_phonecall(struct aoc_alsa_stream *alsa_stream);
int teardown_phonecall(struct aoc_alsa_stream *alsa_stream);

int aoc_mic_loopback(struct aoc_chip *chip, int enable);

int aoc_pcm_init(void);
void aoc_pcm_exit(void);
int aoc_voice_init(void);
void aoc_voice_exit(void);
int aoc_path_init(void);
void aoc_path_exit(void);
#endif
