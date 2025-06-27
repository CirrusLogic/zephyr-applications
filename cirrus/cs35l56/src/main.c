/*
 * Copyright (c) 2025 Cirrus Logic, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/audio/codec.h>

#define CODEC_DAI_CFG_I2S_STEREO	2

#define CODEC_DAI_CFG_I2S_FORMAT	(I2S_FMT_DATA_FORMAT_I2S | \
									 I2S_FMT_CLK_NF_NB)

#define CODEC_DAI_CFG_I2S_OPTION	(I2S_OPT_BIT_CLK_GATED | \
									 I2S_OPT_BIT_CLK_SLAVE | \
									 I2S_OPT_FRAME_CLK_SLAVE)

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(cs35l56_l));
	struct audio_codec_cfg *cfg = {0};
	audio_property_value_t val;
	int ret;

	if (dev == NULL) {
		printk("No host CS35L56 found...\n");
		return -EIO;
	}

	if (!device_is_ready(dev)) {
		printk("\nError: Device \"%s\" is not ready; "
		       "check the driver initialization logs for errors.\n",
		       dev->name);
		return -EIO;
	}

	cfg->dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_24_BITS;
	cfg->dai_cfg.i2s.channels = CODEC_DAI_CFG_I2S_STEREO;
	cfg->dai_cfg.i2s.frame_clk_freq = AUDIO_PCM_RATE_48K;
	cfg->dai_cfg.i2s.format = CODEC_DAI_CFG_I2S_FORMAT;
	cfg->dai_cfg.i2s.options = CODEC_DAI_CFG_I2S_OPTION;

	cfg->dai_route = AUDIO_ROUTE_PLAYBACK;
	cfg->dai_type = AUDIO_DAI_TYPE_I2S;

	ret = audio_codec_configure(dev, cfg);
	if (ret < 0) {
		printk("\nError: Failed to configure codec.\n");
		return ret;
	}

	ret = audio_codec_route_input(dev, AUDIO_CHANNEL_FRONT_LEFT, 1);
	if (ret < 0) {
		printk("\nError: Failed to route input path.\n");
		return ret;
	}

	val.mute = false;

	ret = audio_codec_set_property(dev, AUDIO_PROPERTY_INPUT_MUTE, AUDIO_CHANNEL_FRONT_LEFT, val);
	if (ret < 0) {
		printk("\nError: Failed to unmute\n");
		return ret;
	}

	audio_codec_start_output(dev);

	return 0;
}
