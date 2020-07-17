// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA Machine Driver
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/uio.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <linux/input.h>
#include <linux/version.h>

#include <../sound/soc/codecs/rt5682.h>
#include "aoc_alsa_drv.h"
#include "aoc_alsa.h"
#include "google-aoc-enum.h"

extern int snd_soc_component_set_jack(struct snd_soc_component *component,
				      struct snd_soc_jack *jack, void *data);

static struct aoc_chip *g_chip = NULL;

#define MK_BE_PARAMS(id, fmt, chan, sr)                                        \
	[id] = { .format = fmt, .channel = chan, .rate = sr, .clk_id = 0 },

#define MK_TDM_BE_PARAMS(id, fmt, chan, sr, nslot, slotfmt)                    \
	[id] = { .format = fmt,                                                \
		 .channel = chan,                                              \
		 .rate = sr,                                                   \
		 .slot_num = nslot,                                            \
		 .slot_fmt = slotfmt,                                          \
		 .clk_id = 0 },

#define MK_HW_SR_CTRL(port, xenum, xget, xput)                                 \
	SOC_ENUM_EXT(port " Sample Rate", xenum, xget, xput),

#define MK_HW_FMT_CTRL(port, xenum, xget, xput)                                \
	SOC_ENUM_EXT(port " Format", xenum, xget, xput),

#define MK_HW_CH_CTRL(port, xenum, xget, xput)                                 \
	SOC_ENUM_EXT(port " Chan", xenum, xget, xput),

#define MK_HW_SLOT_NUM_CTRL(port, xenum, xget, xput)                           \
	SOC_ENUM_EXT(port " nSlot", xenum, xget, xput),

#define MK_HW_SLOT_FMT_CTRL(port, xenum, xget, xput)                           \
	SOC_ENUM_EXT(port " SlotFmt", xenum, xget, xput),

#define MK_HW_PARAM_CTRLS(port, name)                                          \
	static const struct snd_kcontrol_new port##_ctrls[] = {                \
		MK_HW_SR_CTRL(name, enum_sr, aoc_be_sr_get, aoc_be_sr_put)     \
			MK_HW_FMT_CTRL(name, enum_fmt, aoc_be_fmt_get,         \
				       aoc_be_fmt_put)                         \
				MK_HW_CH_CTRL(name, enum_ch, aoc_be_ch_get,    \
					      aoc_be_ch_put)                   \
	};

#define MK_TDM_HW_PARAM_CTRLS(port, name)                                      \
	static const struct snd_kcontrol_new port##_ctrls[] = {                \
		MK_HW_SR_CTRL(name, enum_sr, aoc_be_sr_get,                    \
			      aoc_be_sr_put) MK_HW_FMT_CTRL(name, enum_fmt,    \
							    aoc_be_fmt_get,    \
							    aoc_be_fmt_put)    \
			MK_HW_CH_CTRL(name, enum_ch, aoc_be_ch_get,            \
				      aoc_be_ch_put)                           \
				MK_HW_SLOT_NUM_CTRL(name, enum_ch,             \
						    aoc_slot_num_get,          \
						    aoc_slot_num_put)          \
					MK_HW_SLOT_FMT_CTRL(name, enum_fmt,    \
							    aoc_slot_fmt_get,  \
							    aoc_slot_fmt_put)  \
	};

#define HW_PARAM_CTRLS_SIZE(port) ARRAY_SIZE(port##_ctrls)

#define MK_BE_RES_ITEM(port, xops, xfixup)                                     \
	[port] = {                                                             \
		.ops = xops,                                                   \
		.fixup = xfixup,                                               \
		.num_controls = HW_PARAM_CTRLS_SIZE(port),                     \
		.controls = port##_ctrls,                                      \
	},

#define MK_STR_MAP(xstr, xval) { .str = xstr, .value = xval },

#if (KERNEL_VERSION(4, 18, 0) <= LINUX_VERSION_CODE)
typedef int (*fixup_fn)(struct snd_soc_pcm_runtime *,
			struct snd_pcm_hw_params *, int stream);
#else
typedef int (*fixup_fn)(struct snd_soc_pcm_runtime *,
			struct snd_pcm_hw_params *);
#endif

struct dai_link_res_map {
	const struct snd_soc_ops *ops;
	fixup_fn fixup;
	int num_controls;
	const struct snd_kcontrol_new *controls;
};

struct be_param_cache {
	snd_pcm_format_t format;
	u32 channel;
	u32 rate;
	u32 slot_num;
	u32 slot_fmt;
	u32 clk_id;
};

struct str_to_val {
	const char *str;
	u32 value;
};

static int i2s_startup(struct snd_pcm_substream *);
static void i2s_shutdown(struct snd_pcm_substream *);
static int i2s_hw_params(struct snd_pcm_substream *,
			 struct snd_pcm_hw_params *);

#if (KERNEL_VERSION(4, 18, 0) <= LINUX_VERSION_CODE)
static int hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_hw_params *params, int stream);
#else
static int hw_params_fixup(struct snd_soc_pcm_runtime *,
			   struct snd_pcm_hw_params *);
#endif

static int tdm_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *param);

static struct mutex card_mutex;

static const struct snd_soc_ops aoc_i2s_ops = {
	.startup = i2s_startup,
	.shutdown = i2s_shutdown,
	.hw_params = i2s_hw_params,
};

static const struct snd_soc_ops aoc_tdm_ops = {
	.startup = i2s_startup,
	.shutdown = i2s_shutdown,
	.hw_params = tdm_hw_params,
};

static const struct str_to_val sr_map[] = {
	MK_STR_MAP("SR_8K", 8000) MK_STR_MAP("SR_11P025K", 11025) MK_STR_MAP(
		"SR_16K", 16000) MK_STR_MAP("SR_22P05K", 22050)
		MK_STR_MAP("SR_32K", 32000) MK_STR_MAP("SR_44P1K", 44100)
			MK_STR_MAP("SR_48K", 48000) MK_STR_MAP(
				"SR_88P2K", 88200) MK_STR_MAP("SR_96K", 96000)
				MK_STR_MAP("SR_176P4K", 176400)
					MK_STR_MAP("SR_192K", 192000)
};

static const struct str_to_val fmt_map[] = {
	MK_STR_MAP("S16_LE", SNDRV_PCM_FORMAT_S16_LE)
		MK_STR_MAP("S24_LE", SNDRV_PCM_FORMAT_S24_LE)
			MK_STR_MAP("S24_3LE", SNDRV_PCM_FORMAT_S24_3LE)
				MK_STR_MAP("S32_LE", SNDRV_PCM_FORMAT_S32_LE)
					MK_STR_MAP("FLOAT_LE",
						   SNDRV_PCM_FORMAT_FLOAT_LE)
};

static const struct str_to_val ch_map[] = {
	MK_STR_MAP("One", 1) MK_STR_MAP("Two", 2) MK_STR_MAP("Three", 3)
		MK_STR_MAP("Four", 4) MK_STR_MAP("Five", 5) MK_STR_MAP("Six", 6)
			MK_STR_MAP("Seven", 7) MK_STR_MAP("Eight", 8)
};

static const char *sr_text[ARRAY_SIZE(sr_map)] = {};

static const char *fmt_text[ARRAY_SIZE(fmt_map)] = {};

static const char *ch_text[ARRAY_SIZE(ch_map)] = {};

static struct soc_enum enum_sr =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sr_text), sr_text);

static struct soc_enum enum_fmt =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(fmt_text), fmt_text);

static struct soc_enum enum_ch =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(ch_text), ch_text);

static struct be_param_cache be_params[PORT_MAX] = {
	MK_BE_PARAMS(PORT_I2S_0_RX, SNDRV_PCM_FORMAT_S16_LE, 2,
		     48000) MK_BE_PARAMS(PORT_I2S_0_TX, SNDRV_PCM_FORMAT_S16_LE,
					 2, 48000)

		MK_BE_PARAMS(PORT_I2S_1_RX, SNDRV_PCM_FORMAT_S16_LE, 2,
			     48000) MK_BE_PARAMS(PORT_I2S_1_TX,
						 SNDRV_PCM_FORMAT_S16_LE, 2,
						 48000)

			MK_BE_PARAMS(PORT_I2S_2_RX, SNDRV_PCM_FORMAT_S16_LE, 2,
				     48000) MK_BE_PARAMS(PORT_I2S_2_TX,
							 SNDRV_PCM_FORMAT_S16_LE,
							 2, 48000)

				MK_TDM_BE_PARAMS(
					PORT_TDM_0_RX, SNDRV_PCM_FORMAT_S16_LE,
					2, 48000, 4, SNDRV_PCM_FORMAT_S32_LE)
					MK_TDM_BE_PARAMS(
						PORT_TDM_0_TX,
						SNDRV_PCM_FORMAT_S16_LE, 2,
						48000, 4,
						SNDRV_PCM_FORMAT_S32_LE)

						MK_TDM_BE_PARAMS(
							PORT_TDM_1_RX,
							SNDRV_PCM_FORMAT_S16_LE,
							2, 48000, 4,
							SNDRV_PCM_FORMAT_S32_LE)
							MK_TDM_BE_PARAMS(
								PORT_TDM_1_TX,
								SNDRV_PCM_FORMAT_S16_LE,
								2, 48000, 4,
								SNDRV_PCM_FORMAT_S32_LE)
};

#if (KERNEL_VERSION(4, 18, 0) <= LINUX_VERSION_CODE)
static int hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_hw_params *params, int stream)
#else
static int hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_hw_params *params)
#endif
{
	struct snd_mask *fmt_mask =
		hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	struct snd_interval *rate =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);
	u32 sr, ch;
	snd_pcm_format_t fmt;

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid id %u found for %s", __func__, id,
		       rtd->dai_link->name);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	sr = be_params[id].rate;
	ch = be_params[id].channel;
	fmt = be_params[id].format;
	mutex_unlock(&card_mutex);

	pr_debug("%s: fixup ch %u rate %u fmt %u for %s", __func__, ch, sr, fmt,
		rtd->dai_link->name);

	rate->min = rate->max = sr;
	channels->min = channels->max = ch;

	memset(fmt_mask, 0, sizeof(struct snd_mask));

	//4bytes based bit-array
	fmt_mask->bits[fmt >> 5] = (1 << (fmt & 31));
	return 0;
}

static int i2s_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai **codec_dais = rtd->codec_dais;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai_link *dai_link = rtd->dai_link;
	int i, ret;

	pr_debug("i2s startup\n");

	ret = snd_soc_dai_set_fmt(cpu_dai, dai_link->dai_fmt);
	if (ret && ret != -ENOTSUPP) {
		pr_warn("%s: set fmt 0x%x for %s fail %d", __func__,
			dai_link->dai_fmt, cpu_dai->name, ret);
	}

	for (i = 0; i < rtd->num_codecs; i++) {
		ret = snd_soc_dai_set_fmt(codec_dais[i], dai_link->dai_fmt);

		pr_debug("dai_link->dai_fmt = %u\n", dai_link->dai_fmt);

		if (ret && ret != -ENOTSUPP) {
			pr_warn("%s: set fmt 0x%x for %s fail %d", __func__,
				dai_link->dai_fmt, codec_dais[i]->name, ret);
		}
	}
	return 0;
}

static void i2s_shutdown(struct snd_pcm_substream *substream)
{
	return;
}

static int i2s_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *param)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai **codec_dais = rtd->codec_dais;
	struct snd_soc_jack *jack;
	u32 rate, clk, channel;
	int i, bit_width, ret, clk_id;
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);

	struct aoc_chip *chip =
		(struct aoc_chip *)snd_soc_card_get_drvdata(rtd->card); //XH

	pr_debug("rt5682 start\n");
	pr_debug("i2s hw_params\n");
	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid id %u found for %s", __func__, id,
		       rtd->dai_link->name);
		return -EINVAL;
	}

	bit_width = params_physical_width(param);
	if (bit_width < 0) {
		pr_err("%s: invalid bit width %d", __func__, bit_width);
		return -EINVAL;
	}

	clk_id = (int)be_params[id].clk_id;
	channel = params_channels(param);
	rate = params_rate(param);
	clk = rate * ((u32)bit_width) * channel;

	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, clk, SND_SOC_CLOCK_OUT);
	if (ret && ret != -ENOTSUPP)
		pr_warn("%s: set cpu_dai %s fail %d", __func__, cpu_dai->name,
			ret);

	for (i = 0; i < rtd->num_codecs; i++) {
		ret = snd_soc_dai_set_sysclk(codec_dais[i], clk_id, clk,
					     SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP)
			pr_warn("%s: set codec_dai clk %s fail %d", __func__,
				codec_dais[i]->name, ret);

		//TODO  to set it up for RT5682
		//ret = soc_dai_hw_params(substream, param, codec_dais[i]); //
		ret = snd_soc_dai_set_fmt(codec_dais[i],
					  SND_SOC_DAIFMT_CBS_CFS |
						  SND_SOC_DAIFMT_I2S);
		if (ret && ret != -ENOTSUPP)
			pr_warn("%s: set codec_dai set fmt %s fail %d",
				__func__, codec_dais[i]->name, ret);

		ret = -1;
		if (codec_dais[i]->driver->ops &&
		    codec_dais[i]->driver->ops->hw_params) {
			ret = codec_dais[i]->driver->ops->hw_params(
				substream, param, codec_dais[i]);
		}
		if (ret && ret != -ENOTSUPP)
			pr_warn("%s: set codec_dai hw_params %s fail %d",
				__func__, codec_dais[i]->name, ret);

		ret = snd_soc_dai_set_tdm_slot(codec_dais[i], 0x0, 0x0, 2, 32);
		if (ret && ret != -ENOTSUPP)
			pr_warn("%s: set codec set_tdm_slot %s fail %d",
				__func__, codec_dais[i]->name, ret);

		ret = snd_soc_component_set_pll(
			codec_dais[i]->component, 0,
			RT5682_PLL1_S_BCLK1, (48000 * 64), (48000 * 512));
		if (ret && ret != -ENOTSUPP) {
			pr_warn("%s: set codec pll clk %s fail %d", __func__,
				codec_dais[i]->name, ret);
		}

		ret = snd_soc_component_set_sysclk(codec_dais[i]->component,
						   RT5682_SCLK_S_PLL1, 0,
						   (48000 * 512),
						   SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP) {
			pr_warn("%s: set codec clk %s fail %d", __func__,
				codec_dais[i]->name, ret);
		}

		/*
	 	 * Headset buttons map to the google Reference headset.
	 	 * These can be configured by userspace.
	 	 */
		pr_debug("rt5682 set jack start\n");
		ret = snd_soc_card_jack_new(
			rtd->card, "Headset Jack",
			SND_JACK_HEADSET | SND_JACK_BTN_0 | SND_JACK_BTN_1 |
				SND_JACK_BTN_2 | SND_JACK_BTN_3 |
				SND_JACK_LINEOUT,
			&chip->jack, NULL, 0);
		if (ret) {
			dev_err(rtd->dev, "Headset Jack creation failed: %d\n",
				ret);
			return ret;
		}
		jack = &chip->jack;
		snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);
		pr_notice("rt5682 set jack\n");
		ret = snd_soc_component_set_jack(codec_dais[i]->component, jack,
						 NULL);
		if (ret) {
			dev_err(rtd->dev, "Headset Jack call-back failed: %d\n",
				ret);
			return ret;
		}
	}
	return 0;
}

static int tdm_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *param)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai **codec_dais = rtd->codec_dais;
	u32 rate, clk, channel, tdmslot;
	int i, bit_width, ret, slot_width, clk_id;
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);

	pr_debug("%s: startup\n", __func__);

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid id %u found for %s", __func__, id,
		       rtd->dai_link->name);
		return -EINVAL;
	}

	tdmslot = be_params[id].slot_num;
	slot_width = snd_pcm_format_physical_width(be_params[id].slot_fmt);

	bit_width = params_physical_width(param);
	if (bit_width < 0) {
		pr_err("%s: invalid bit width %d", __func__, bit_width);
		return -EINVAL;
	}

	channel = params_channels(param);
	if (tdmslot < channel || slot_width < bit_width) {
		pr_err("%s: invalid ch %u slot %u, bit %d, slot_bit %d",
		       __func__, channel, tdmslot, bit_width, slot_width);
		return -EINVAL;
	}

	clk_id = (int)be_params[id].clk_id;
	rate = params_rate(param);
	clk = rate * ((u32)slot_width) * tdmslot;
	pr_debug("ch %u tdm slot %u bit %d, slot_bit %d", channel, tdmslot,
		bit_width, slot_width);

	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, clk, SND_SOC_CLOCK_OUT);
	if (ret && ret != -ENOTSUPP)
		pr_warn("%s: set cpu_dai %s fail %d", __func__, cpu_dai->name,
			ret);

	for (i = 0; i < rtd->num_codecs; i++) {
		ret = snd_soc_dai_set_sysclk(codec_dais[i], clk_id, clk,
					     SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP)
			pr_warn("%s: set codec_dai clk %s fail %d", __func__,
				codec_dais[i]->name, ret);
		//Do we need to consider the redundant case?
		ret = snd_soc_component_set_sysclk(codec_dais[i]->component,
						   clk_id, 0, clk,
						   SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP)
			pr_warn("%s: set codec clk %s fail %d", __func__,
				codec_dais[i]->name, ret);
	}
	return 0;
}

static int aoc_slot_num_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id), i;

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	for (i = 0; i < ARRAY_SIZE(ch_map); i++) {
		if (be_params[id].slot_num == ch_map[i].value) {
			break;
		}
	}
	mutex_unlock(&card_mutex);

	if (i == ARRAY_SIZE(ch_map))
		return -EINVAL;

	ucontrol->value.integer.value[0] = (int)i;
	return 0;
}

static int aoc_slot_num_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);
	int idx = ucontrol->value.integer.value[0];

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	if (idx < 0 || idx >= ARRAY_SIZE(ch_map)) {
		pr_err("%s: invalid idx %d", __func__, idx);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	be_params[id].slot_num = ch_map[idx].value;
	mutex_unlock(&card_mutex);
	return 0;
}

static int aoc_slot_fmt_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id), i;

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	for (i = 0; i < ARRAY_SIZE(fmt_map); i++) {
		if (be_params[id].slot_fmt == fmt_map[i].value) {
			break;
		}
	}
	mutex_unlock(&card_mutex);

	if (i == ARRAY_SIZE(fmt_map))
		return -EINVAL;

	ucontrol->value.integer.value[0] = (int)i;
	return 0;
}

static int aoc_slot_fmt_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);
	int idx = ucontrol->value.integer.value[0];

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	if (idx < 0 || idx >= ARRAY_SIZE(fmt_map))
		return -EINVAL;

	mutex_lock(&card_mutex);
	be_params[id].slot_fmt = fmt_map[idx].value;
	mutex_unlock(&card_mutex);
	return 0;
}

static int aoc_be_sr_get(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id), i;

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	for (i = 0; i < ARRAY_SIZE(sr_map); i++) {
		if (be_params[id].rate == sr_map[i].value) {
			break;
		}
	}
	mutex_unlock(&card_mutex);

	if (i == ARRAY_SIZE(sr_map))
		return -EINVAL;

	ucontrol->value.integer.value[0] = (int)i;
	return 0;
}

static int aoc_be_sr_put(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);
	int idx = ucontrol->value.integer.value[0];

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	if (idx < 0 || idx >= ARRAY_SIZE(sr_map)) {
		pr_err("%s: invalid idx %d", __func__, idx);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	be_params[id].rate = sr_map[idx].value;
	mutex_unlock(&card_mutex);
	return 0;
}

static int aoc_be_fmt_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id), i;

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	for (i = 0; i < ARRAY_SIZE(fmt_map); i++) {
		if (be_params[id].format == fmt_map[i].value) {
			break;
		}
	}
	mutex_unlock(&card_mutex);

	if (i == ARRAY_SIZE(fmt_map))
		return -EINVAL;

	ucontrol->value.integer.value[0] = (int)i;
	return 0;
}

static int aoc_be_fmt_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);
	int idx = ucontrol->value.integer.value[0];

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	if (idx < 0 || idx >= ARRAY_SIZE(fmt_map))
		return -EINVAL;

	mutex_lock(&card_mutex);
	be_params[id].format = fmt_map[idx].value;
	mutex_unlock(&card_mutex);
	return 0;
}

static int aoc_be_ch_get(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id), i;

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	mutex_lock(&card_mutex);
	for (i = 0; i < ARRAY_SIZE(ch_map); i++) {
		if (be_params[id].channel == ch_map[i].value) {
			break;
		}
	}
	mutex_unlock(&card_mutex);

	if (i == ARRAY_SIZE(ch_map))
		return -EINVAL;

	ucontrol->value.integer.value[0] = (int)i;
	return 0;
}

static int aoc_be_ch_put(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai =
		(struct snd_soc_dai *)snd_kcontrol_chip(kcontrol);
	u32 id = AOC_ID_TO_INDEX(cpu_dai->id);
	int idx = ucontrol->value.integer.value[0];

	if (id >= ARRAY_SIZE(be_params)) {
		pr_err("%s: invalid idx %u", __func__, id);
		return -EINVAL;
	}

	if (idx < 0 || idx >= ARRAY_SIZE(ch_map))
		return -EINVAL;

	mutex_lock(&card_mutex);
	be_params[id].channel = ch_map[idx].value;
	mutex_unlock(&card_mutex);
	return 0;
}

/*
 * Declare the Sample rate, bit width, Channel controls
 * of the hardware backend port.
 *
 * Examples:
 * "I2S_0_RX Sample Rate"
 * "I2S_0_RX Format"
 * "I2S_0_RX Chan"
 */
MK_HW_PARAM_CTRLS(PORT_I2S_0_RX, "I2S_0_RX")
MK_HW_PARAM_CTRLS(PORT_I2S_0_TX, "I2S_0_TX")
MK_HW_PARAM_CTRLS(PORT_I2S_1_RX, "I2S_1_RX")
MK_HW_PARAM_CTRLS(PORT_I2S_1_TX, "I2S_1_TX")
MK_HW_PARAM_CTRLS(PORT_I2S_2_RX, "I2S_2_RX")
MK_HW_PARAM_CTRLS(PORT_I2S_2_TX, "I2S_2_TX")
MK_TDM_HW_PARAM_CTRLS(PORT_TDM_0_RX, "TDM_0_RX")
MK_TDM_HW_PARAM_CTRLS(PORT_TDM_0_TX, "TDM_0_TX")
MK_TDM_HW_PARAM_CTRLS(PORT_TDM_1_RX, "TDM_1_RX")
MK_TDM_HW_PARAM_CTRLS(PORT_TDM_1_TX, "TDM_1_TX")

/*
 * The resource array that have ALSA controls, ops and fixup
 * funciton of each backend port.
 *
 */
static const struct dai_link_res_map be_res_map[PORT_MAX] = {
	MK_BE_RES_ITEM(PORT_I2S_0_RX, &aoc_i2s_ops,
		       hw_params_fixup) MK_BE_RES_ITEM(PORT_I2S_0_TX,
						       &aoc_i2s_ops,
						       hw_params_fixup)

		MK_BE_RES_ITEM(PORT_I2S_1_RX, &aoc_i2s_ops,
			       hw_params_fixup) MK_BE_RES_ITEM(PORT_I2S_1_TX,
							       &aoc_i2s_ops,
							       hw_params_fixup)

			MK_BE_RES_ITEM(
				PORT_I2S_2_RX, &aoc_i2s_ops,
				hw_params_fixup) MK_BE_RES_ITEM(PORT_I2S_2_TX,
								&aoc_i2s_ops,
								hw_params_fixup)

				MK_BE_RES_ITEM(PORT_TDM_0_RX, &aoc_tdm_ops,
					       hw_params_fixup)
					MK_BE_RES_ITEM(PORT_TDM_0_TX,
						       &aoc_tdm_ops,
						       hw_params_fixup)

						MK_BE_RES_ITEM(PORT_TDM_1_RX,
							       &aoc_tdm_ops,
							       hw_params_fixup)
							MK_BE_RES_ITEM(
								PORT_TDM_1_TX,
								&aoc_tdm_ops,
								hw_params_fixup)
};

static int of_parse_one_dai(struct device_node *node, struct device *dev,
			    struct snd_soc_dai_link *dai)
{
	int ret = 0;
	bool ops, fixup;
	u32 trigger, id;
	struct device_node *daifmt = NULL;
	struct device_node *np_cpu = NULL, *np_codec = NULL;
	const char *str;

	if (!node || !dai)
		return -EINVAL;

	ret = of_property_read_string(node, "dai-name", &dai->name);
	if (ret) {
		pr_err("%s: fail to get dai name %d", __func__, ret);
		goto err;
	}

	ret = of_property_read_string(node, "stream-name", &dai->stream_name);
	if (ret) {
		pr_err("%s: fail to get dai stream name %d", __func__, ret);
		goto err;
	}

	dai->platform_of_node = of_parse_phandle(node, "platform", 0);
	if (!dai->platform_of_node) {
		ret = of_property_read_string(node, "platform-name", &str);
		if (ret) {
			pr_err("%s: fail to get platform for %s", __func__,
			       dai->name);
			ret = -EINVAL;
			goto err;
		}
		dai->platform_name = str;
	}

	np_cpu = of_get_child_by_name(node, "cpu");
	if (!np_cpu) {
		pr_err("%s: can't find cpu node for %s", __func__, dai->name);
		ret = -EINVAL;
		goto err;
	}

	/* Only support single cpu dai */
	dai->cpu_of_node = of_parse_phandle(np_cpu, "sound-dai", 0);
	if (!dai->cpu_of_node) {
		pr_err("%s: fail to get cpu dai for %s", __func__, dai->name);
		ret = -EINVAL;
		goto err;
	}

	ret = snd_soc_of_get_dai_name(np_cpu, &dai->cpu_dai_name);
	if (ret) {
		if (ret == -EPROBE_DEFER) {
			pr_info("%s: wait cpu_dai for %s", __func__, dai->name);
		} else
			pr_err("%s: get cpu_dai fail for %s", __func__,
			       dai->name);
		goto err;
	}

	np_codec = of_get_child_by_name(node, "codec");
	if (!np_codec) {
		pr_err("%s: can't find codec node for %s", __func__, dai->name);
		ret = -EINVAL;
		goto err;
	}

	ret = of_property_read_string(np_codec, "codec-name", &str);
	if (ret) {
		ret = snd_soc_of_get_dai_link_codecs(dev, np_codec, dai);

		pr_debug("dai->num_codecs = %d\n", dai->num_codecs);

		if (ret) {
			if (ret == -EPROBE_DEFER)
				pr_info("%s: %d wait codec for %s", __func__,
					ret, dai->name);
			else
				pr_err("%s: %d fail to get codec for %s",
				       __func__, ret, dai->name);
			goto err;
		}
	} else {
		dai->codec_name = str;
		ret = of_property_read_string(np_codec, "codec-dai-name", &str);
		if (ret) {
			pr_err("%s: %d fail to get codec dai for %s", __func__,
			       ret, dai->name);
			goto err;
		} else
			dai->codec_dai_name = str;
	}

	ret = of_property_read_u32_index(node, "trigger", 0, &trigger);
	if (ret == 0) {
		switch (trigger) {
		case 1:
			dai->trigger[0] = SND_SOC_DPCM_TRIGGER_POST;
			dai->trigger[1] = SND_SOC_DPCM_TRIGGER_POST;
			break;
		case 2:
			dai->trigger[0] = SND_SOC_DPCM_TRIGGER_BESPOKE;
			dai->trigger[1] = SND_SOC_DPCM_TRIGGER_BESPOKE;
			break;
		default:
			dai->trigger[0] = SND_SOC_DPCM_TRIGGER_PRE;
			dai->trigger[1] = SND_SOC_DPCM_TRIGGER_PRE;
			break;
		}
	}

	ret = of_property_read_u32_index(node, "id", 0, &id);
	if (ret == 0) {
		dai->id = id;
		id = AOC_ID_TO_INDEX(id);

		if (dai->id & AOC_BE) {
			if (id < ARRAY_SIZE(be_res_map)) {
				ops = of_property_read_bool(node, "useops");
				fixup = of_property_read_bool(node, "usefixup");

				if (ops)
					dai->ops = be_res_map[id].ops;

				if (fixup)
					dai->be_hw_params_fixup =
						be_res_map[id].fixup;
			}

			if (id < ARRAY_SIZE(be_params)) {
				u32 clk_id;
				ret = of_property_read_u32_index(node, "clk_id",
								 0, &clk_id);
				if (ret == 0)
					be_params[id].clk_id = clk_id;
			}
		}
	}

	daifmt = of_get_child_by_name(node, "daifmt");
	if (daifmt) {
		dai->dai_fmt =
			snd_soc_of_parse_daifmt(daifmt, NULL, NULL, NULL);
		of_node_put(daifmt);
		pr_debug("%s: daifmt 0x%x for %s", __func__, dai->dai_fmt,
			dai->name);
	}

	dai->dpcm_playback = of_property_read_bool(node, "playback");
	dai->dpcm_capture = of_property_read_bool(node, "capture");
	dai->no_pcm = of_property_read_bool(node, "no-pcm");
	dai->dynamic = of_property_read_bool(node, "dynamic");
	dai->ignore_pmdown_time =
		of_property_read_bool(node, "ignore-pmdown-time");
	dai->ignore_suspend = of_property_read_bool(node, "ignore-suspend");

	of_node_put(np_cpu);
	of_node_put(np_codec);
	return 0;
err:
	if (dai->platform_of_node) {
		of_node_put(dai->platform_of_node);
		dai->platform_of_node = NULL;
	}

	if (dai->cpu_of_node) {
		of_node_put(dai->cpu_of_node);
		dai->cpu_of_node = NULL;
	}

	if (dai->num_codecs > 0) {
		int i;
		for (i = 0; i < dai->num_codecs; i++) {
			of_node_put(dai->codecs[i].of_node);
			dai->codecs[i].of_node = NULL;
		}
		dai->num_codecs = 0;
	}

	if (np_cpu)
		of_node_put(np_cpu);

	if (np_codec)
		of_node_put(np_codec);

	return ret;
}

static int aoc_of_parse_dai_link(struct device_node *node,
				 struct snd_soc_card *card)
{
	int ret = 0, count;
	struct device_node *np_dai;
	struct device_node *np = NULL;
	struct device *dev = card->dev;
	struct snd_soc_dai_link *dai_link;

	np_dai = of_get_child_by_name(node, "dai_link");
	if (!np_dai) {
		pr_err("%s: can't find dai-link node", __func__);
		return -EINVAL;
	}

	count = (int)of_get_available_child_count(np_dai);
	if (count <= 0) {
		pr_err("%s: count %d invalid", __func__, count);
		ret = -EINVAL;
		goto err;
	}

	dai_link = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link) * count,
				GFP_KERNEL);
	if (!dai_link) {
		pr_err("%s: fail to allocate memory for dai_link", __func__);
		ret = -ENOMEM;
		goto err;
	}

	card->num_links = count;
	card->dai_link = dai_link;

	count = 0;
	for_each_available_child_of_node (np_dai, np) {
		if (count >= card->num_links) {
			pr_err("%s: dai link num is full %u", __func__, count);
			break;
		}

		ret = of_parse_one_dai(np, card->dev, dai_link);
		if (ret) {
			if (ret == -EPROBE_DEFER) {
				pr_info("%s: register sound card later",
					__func__);
				break;
			} else {
				pr_warn("%s: fail to parse %s", __func__,
					np->name);
				memset(dai_link, 0, sizeof(*dai_link));
				continue;
			}
		}
#ifdef DUMP_DAI_LINK_INFO
		pr_info("dai: %s\n", dai_link->name);
		pr_info("id: %u\n", (uint32_t)dai_link->id);
		pr_info("playback %u capture %u\n", dai_link->dpcm_playback,
			dai_link->dpcm_capture);
		pr_info("no-pcm: %u\n", dai_link->no_pcm);
		pr_info("dynamic: %u\n", dai_link->dynamic);
		pr_info("\n");
#endif
		dai_link++;
		count++;
	}

	if (ret != -EPROBE_DEFER)
		ret = 0;

	card->num_links = count;

err:
	of_node_put(np_dai);
	return ret;
}

static int of_parse_one_codec_cfg(struct device_node *node,
				  struct snd_soc_codec_conf *codec_cfg)
{
	int ret = 0;

	if (!node || !codec_cfg)
		return -EINVAL;

	codec_cfg->of_node = of_parse_phandle(node, "of_node", 0);
	if (!codec_cfg->of_node) {
		pr_err("%s: fail to get of_node for %s", __func__, node->name);
		ret = -EINVAL;
		goto err;
	}

	ret = of_property_read_string(node, "prefix", &codec_cfg->name_prefix);
	if (ret) {
		pr_err("%s: fail to get prefix for %s %d", __func__, node->name,
		       ret);
		goto err;
	}
	return 0;

err:
	return ret;
}

static int aoc_of_parse_codec_conf(struct device_node *node,
				   struct snd_soc_card *card)
{
	int ret = 0, count;
	struct device_node *np_cfg;
	struct device_node *np = NULL;
	struct snd_soc_codec_conf *codec_cfg;
	struct device *dev = card->dev;

	np_cfg = of_get_child_by_name(node, "codec_cfg");
	if (!np_cfg) {
		pr_info("%s: can't find codec cfg node", __func__);
		return 0;
	}

	count = (int)of_get_available_child_count(np_cfg);
	if (count <= 0) {
		pr_err("%s: count %d invalid", __func__, count);
		ret = -EINVAL;
		goto err;
	}

	codec_cfg = devm_kzalloc(dev, sizeof(struct snd_soc_codec_conf) * count,
				 GFP_KERNEL);
	if (!codec_cfg) {
		pr_err("%s: fail to allocate memory for codec_cfg", __func__);
		ret = -ENOMEM;
		goto err;
	}

	card->num_configs = count;
	card->codec_conf = codec_cfg;

	count = 0;
	for_each_available_child_of_node (np_cfg, np) {
		if (count >= card->num_configs) {
			pr_err("%s: conf num is full %u", __func__, count);
			break;
		}

		ret = of_parse_one_codec_cfg(np, codec_cfg);
		if (ret) {
			memset(codec_cfg, 0, sizeof(*codec_cfg));
			continue;
		}

		codec_cfg++;
		count++;
	}
	card->num_configs = count;
	ret = 0;

err:
	of_node_put(np_cfg);
	return ret;
}

static int aoc_snd_card_parse_of(struct device_node *node,
				 struct snd_soc_card *card)
{
	int ret;

	ret = aoc_of_parse_dai_link(node, card);
	if (ret) {
		pr_err("%s: fail to parse fai_link %d", __func__, ret);
		goto err;
	}

	ret = aoc_of_parse_codec_conf(node, card);
	if (ret) {
		pr_err("%s: fail to parse codec conf %d", __func__, ret);
		goto err;
	}

	ret = snd_soc_of_parse_card_name(card, "aoc-card-name");
	if (ret) {
		pr_err("%s: fail to parse snd card name %d", __func__, ret);
		goto err;
	}

err:
	return ret;
}

static int aoc_card_late_probe(struct snd_soc_card *card)
{
	struct aoc_chip *chip =
		(struct aoc_chip *)snd_soc_card_get_drvdata(card);
	int err, i;
	struct snd_soc_pcm_runtime *rtd;
	u32 id;

	chip->card = card->snd_card;

	//TODO  make the service list NOT have to be in the same order as pcm device list
	for (i = 0; i < aoc_audio_service_num() - 2; i++) {
		chip->avail_substreams |= (1 << i);
	}

	err = snd_aoc_new_ctl(chip);
	if (err < 0)
		pr_err("%s: fail to new ctrl %d", __func__, err);

	/* Register HW PARAM control */
	list_for_each_entry (rtd, &card->rtd_list, list) {
		if (rtd->dai_link->no_pcm) {
			id = rtd->dai_link->id;
			if (!(id & AOC_BE))
				continue;

			id = AOC_ID_TO_INDEX(id);
			if (id >= ARRAY_SIZE(be_res_map))
				continue;

			if (be_res_map[id].num_controls == 0 ||
			    !be_res_map[id].controls)
				continue;

			snd_soc_add_dai_controls(rtd->cpu_dai,
						 be_res_map[id].controls,
						 be_res_map[id].num_controls);
		}
	}

	return 0;
}

static int snd_aoc_create(struct aoc_chip **rchip)
{
	struct aoc_chip *chip;
	int i;

	*rchip = NULL;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	chip->mic_loopback_enabled = 0;

	chip->default_mic_id = DEFAULT_MICPHONE_ID;
	chip->buildin_mic_id_list[0] = DEFAULT_MICPHONE_ID;
	for (i = 1; i < NUM_OF_BUILTIN_MIC; i++) {
		chip->buildin_mic_id_list[i] = -1;
	}

	chip->default_sink_id = DEFAULT_AUDIO_SINK_ID;
	chip->sink_id_list[0] = DEFAULT_AUDIO_SINK_ID;
	for (i = 1; i < MAX_NUM_OF_SINKS_PER_STREAM; i++) {
		chip->sink_id_list[i] = -1;
	}

	chip->voice_call_mic_mute = 0;

	mutex_init(&chip->audio_mutex);

	*rchip = chip;
	return 0;
}

static int aoc_snd_card_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct snd_soc_card *card;
	int ret;
	pr_info("%s", __func__);
	if (!np)
		return -ENOSYS;

	ret = alloc_aoc_audio_service("audio_output_control",
				      &g_chip->dev_alsa_output_control);
	if (ret < 0) {
		if (ret == -EPROBE_DEFER)
			pr_info("%s: wait for aoc output ctrl\n", __func__);
		else
			pr_err("%s: Failed to get aoc output ctrl %d\n",
			       __func__, ret);
		goto err;
	}

	ret = alloc_aoc_audio_service("audio_input_control",
				      &g_chip->dev_alsa_input_control);
	if (ret < 0) {
		if (ret == -EPROBE_DEFER)
			pr_info("%s: wait for aoc input ctrl\n", __func__);
		else
			pr_err("%s: Failed to get aoc input ctrl %d\n",
			       __func__, ret);
		goto err;
	}

	/* Allocate the private data and the DAI link array */
	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card) {
		pr_err("%s: fail to allocate mem", __func__);
		return -ENOMEM;
	}

	card->owner = THIS_MODULE;
	card->dev = dev;
	card->late_probe = aoc_card_late_probe;

	ret = aoc_snd_card_parse_of(np, card);
	if (ret) {
		goto err;
	}

	snd_soc_card_set_drvdata(card, g_chip);
	ret = snd_soc_register_card(card);
	if (ret < 0) {
		if (ret == -EPROBE_DEFER) {
			pr_info("%s: defer the probe %d", __func__, ret);
		} else
			pr_info("%s: snd register fail %d", __func__, ret);
		goto err;
	}

	return 0;

err:
	if (g_chip->dev_alsa_output_control) {
		free_aoc_audio_service("audio_output_control",
				       g_chip->dev_alsa_output_control);
		g_chip->dev_alsa_output_control = NULL;
	}

	if (g_chip->dev_alsa_input_control) {
		free_aoc_audio_service("audio_input_control",
				       g_chip->dev_alsa_input_control);
		g_chip->dev_alsa_input_control = NULL;
	}
	return ret;
}

static int aoc_snd_card_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	if (card) {
		snd_soc_unregister_card(card);
		snd_soc_card_set_drvdata(card, NULL);
	}

	if (g_chip->dev_alsa_output_control) {
		free_aoc_audio_service("audio_output_control",
				       g_chip->dev_alsa_output_control);
		g_chip->dev_alsa_output_control = NULL;
	}

	if (g_chip->dev_alsa_input_control) {
		free_aoc_audio_service("audio_input_control",
				       g_chip->dev_alsa_input_control);
		g_chip->dev_alsa_input_control = NULL;
	}

	return 0;
}

static const struct of_device_id aoc_snd_of_match[] = {
	{
		.compatible = "google-aoc-snd-card",
	},
	{},
};
MODULE_DEVICE_TABLE(of, aoc_snd_of_match);

static struct platform_driver aoc_snd_card_drv = {
	.driver = {
		.name = "google-aoc-snd-card",
		.of_match_table = aoc_snd_of_match,
	},
	.probe = aoc_snd_card_probe,
	.remove = aoc_snd_card_remove,
};

static int aoc_card_init(void)
{
	int ret = 0, i;
	pr_info("%s", __func__);
	for (i = 0; i < ARRAY_SIZE(sr_map); i++)
		sr_text[i] = sr_map[i].str;

	for (i = 0; i < ARRAY_SIZE(fmt_map); i++)
		fmt_text[i] = fmt_map[i].str;

	for (i = 0; i < ARRAY_SIZE(ch_map); i++)
		ch_text[i] = ch_map[i].str;

	ret = snd_aoc_create(&g_chip);
	if (ret < 0) {
		pr_err("%s: failed to create aoc chip\n", __func__);
		goto exit;
	}

	mutex_init(&card_mutex);
	ret = platform_driver_register(&aoc_snd_card_drv);
	if (ret) {
		pr_err("error registering aoc pcm drv %d .\n", ret);
		goto exit;
	}

exit:
	return ret;
}

static void aoc_card_exit(void)
{
	platform_driver_unregister(&aoc_snd_card_drv);
	mutex_destroy(&card_mutex);

	if (g_chip) {
		kfree(g_chip);
		g_chip = NULL;
	}
}

module_init(aoc_card_init);
module_exit(aoc_card_exit);

MODULE_AUTHOR("google aoc team");
MODULE_DESCRIPTION("Alsa driver for aoc sound card");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:aoc_alsa_card");
