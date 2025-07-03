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
#include <zephyr/drivers/gpio.h>

#ifdef CONFIG_TOGGLE_AMP_MUTE_SW0
static volatile bool mute_enabled = false;
#define SW0_NODE        DT_ALIAS(sw0)
#endif

#ifdef CONFIG_TOGGLE_AMP_MUTE_SW0
static struct gpio_dt_spec sw0_spec = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
#endif

#ifdef CONFIG_TOGGLE_AMP_MUTE_SW0
static void sw0_handler(const struct device *dev, struct gpio_callback *cb,
			uint32_t pins)
{
	audio_property_value_t val;
	int ret;

	val.mute = !mute_enabled;
	ret = audio_codec_set_property(dev, AUDIO_PROPERTY_INPUT_MUTE, AUDIO_CHANNEL_FRONT_LEFT, val);
	if (ret < 0) {
		printk("\nError: Failed to toggle mute\n");
		return;
	}

	mute_enabled = val.mute;
	printk("Mute %sabled\n", (mute_enabled ? "en" : "dis"));
}
#endif

#ifdef CONFIG_TOGGLE_AMP_MUTE_SW0
static bool init_button(void)
{
	static struct gpio_callback sw0_cb_data;
	int ret;

	if (!gpio_is_ready_dt(&sw0_spec)) {
		printk("%s is not ready\n", sw0_spec.port->name);
		return false;
	}

	ret = gpio_pin_configure_dt(&sw0_spec, GPIO_INPUT);
	if (ret < 0) {
		printk("Failed to configure %s pin %d: %d\n",
		       sw0_spec.port->name, sw0_spec.pin, ret);
		return false;
	}

	ret = gpio_pin_interrupt_configure_dt(&sw0_spec,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		printk("Failed to configure interrupt on %s pin %d: %d\n",
		       sw0_spec.port->name, sw0_spec.pin, ret);
		return false;
	}

	gpio_init_callback(&sw0_cb_data, sw0_handler, BIT(sw0_spec.pin));
	gpio_add_callback(sw0_spec.port, &sw0_cb_data);
	printk("Press \"%s\" to toggle the amp mute\n", sw0_spec.port->name);

	return true;
}
#endif

#define CODEC_DAI_CFG_I2S_STEREO	2

#define CODEC_DAI_CFG_I2S_FORMAT	(I2S_FMT_DATA_FORMAT_I2S | \
									 I2S_FMT_CLK_NF_NB)

#define CODEC_DAI_CFG_I2S_OPTION	(I2S_OPT_BIT_CLK_GATED | \
									 I2S_OPT_BIT_CLK_SLAVE | \
									 I2S_OPT_FRAME_CLK_SLAVE)

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(cs35l56_l));
	struct audio_codec_cfg cfg = {0};
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

	cfg.dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_32_BITS;
	cfg.dai_cfg.i2s.channels = CODEC_DAI_CFG_I2S_STEREO;
	cfg.dai_cfg.i2s.frame_clk_freq = AUDIO_PCM_RATE_48K;
	cfg.dai_cfg.i2s.format = CODEC_DAI_CFG_I2S_FORMAT;
	cfg.dai_cfg.i2s.options = CODEC_DAI_CFG_I2S_OPTION;

	cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
	cfg.dai_type = AUDIO_DAI_TYPE_I2S;

	ret = audio_codec_configure(dev, &cfg);
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

#ifdef CONFIG_TOGGLE_AMP_MUTE_SW0
	if (!init_button()) {
		return 0;
	}
#endif

	audio_codec_start_output(dev);

	while (1) {
		printk("Audio playback in progress\n");
		k_sleep(K_SECONDS(10));
	}

	return 0;
}
