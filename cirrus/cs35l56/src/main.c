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

struct audio_codec_cfg cfg = {
	.dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_32_BITS,
	.dai_cfg.i2s.channels = CODEC_DAI_CFG_I2S_STEREO,
	.dai_cfg.i2s.frame_clk_freq = AUDIO_PCM_RATE_48K,
	.dai_cfg.i2s.format = CODEC_DAI_CFG_I2S_FORMAT,
	.dai_cfg.i2s.options = CODEC_DAI_CFG_I2S_OPTION,

	.dai_route = AUDIO_ROUTE_PLAYBACK,
	.dai_type = AUDIO_DAI_TYPE_I2S,
};

int main(void)
{
	const struct device *amp_l = DEVICE_DT_GET(DT_NODELABEL(cs35l56_l));
	const struct device *amp_r = DEVICE_DT_GET(DT_NODELABEL(cs35l56_r));
	audio_property_value_t val;
	int ret;

	if (amp_l == NULL || amp_r == NULL ) {
		printk("No CS35L56 stereo pair found...\n");
		return -ENODEV;
	}

	if (!device_is_ready(amp_l)) {
		printk("\nError: Left \"%s\" is not ready; "
			"check the driver initialization logs for errors.\n",
			amp_l->name);
			return -EIO;
	}

	if (!device_is_ready(amp_r)) {
		printk("\nError: Right \"%s\" is not ready; "
			"check the driver initialization logs for errors.\n",
			amp_r->name);
			return -EIO;
	}

	ret = audio_codec_configure(amp_l, &cfg);
	if (ret < 0) {
		printk("\nError: Failed to configure left codec.\n");
		return ret;
	}

	ret = audio_codec_configure(amp_r, &cfg);
	if (ret < 0) {
		printk("\nError: Failed to configure right codec.\n");
		return ret;
	}

	ret = audio_codec_route_input(amp_l, AUDIO_CHANNEL_FRONT_LEFT, 1);
	if (ret < 0) {
		printk("\nError: Failed to route left input path.\n");
		return ret;
	}

	ret = audio_codec_route_input(amp_r, AUDIO_CHANNEL_FRONT_RIGHT, 2);
	if (ret < 0) {
		printk("\nError: Failed to route right input path.\n");
		return ret;
	}

	val.mute = false;

	ret = audio_codec_set_property(amp_l, AUDIO_PROPERTY_INPUT_MUTE, AUDIO_CHANNEL_FRONT_LEFT, val);
	if (ret < 0) {
		printk("\nError: Failed to unmute left amp\n");
		return ret;
	}

	ret = audio_codec_set_property(amp_r, AUDIO_PROPERTY_INPUT_MUTE, AUDIO_CHANNEL_FRONT_RIGHT, val);
	if (ret < 0) {
		printk("\nError: Failed to unmute right amp\n");
		return ret;
	}

	audio_codec_start_output(amp_l);
	audio_codec_start_output(amp_r);

	while (1) {
		printk("Audio playback in progress\n");
		k_sleep(K_SECONDS(10));
	}

	return 0;
}
