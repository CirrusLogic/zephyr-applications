/*
 * Copyright (c) 2026 Cirrus Logic, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/audio/codec.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main);

#define AUDIO_AMP_LEFT			0
#define AUDIO_AMP_RIGHT			1

#define ASP1_RX1	1
#define ASP1_RX2	2

const struct device *amps[] = {
	[AUDIO_AMP_LEFT] = DEVICE_DT_GET(DT_NODELABEL(cs35l45_l)),
	[AUDIO_AMP_RIGHT] = DEVICE_DT_GET(DT_NODELABEL(cs35l45_r)),
};

#define SW0_NODE	DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#else /* DT_NODE_HAS_STATUS_OKAY(SW0_NODE) */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, {0});
#endif

static volatile bool playback = false;

static void set_codec_output_state(const bool state)
{
	LOG_DBG("%s audio playback", state ? "Starting" : "Pausing");
	if (state) {
		for (int i = 0; i < ARRAY_SIZE(amps); i++) {
			if (amps[i] == NULL) {
				continue;
			}
			audio_codec_start_output(amps[i]);
		}
	} else {
		for (int i = 0; i < ARRAY_SIZE(amps); i++) {
			if (amps[i] == NULL) {
				continue;
			}
			audio_codec_stop_output(amps[i]);
		}
	}
}

static struct k_work_delayable irq_worker;
static void button_worker(struct k_work *work)
{
	/* Debounce the switch button. */
	if (gpio_pin_get_dt(&button) == 0) {
		return;
	}

	/* If device is currently in monitor mode, transition to PC mode. */
	if (playback) {
		set_codec_output_state(false);
	} else {
		set_codec_output_state(true);
	}

	playback = !playback;
}

static struct gpio_callback button_cb_data;
static void button_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	(void)k_work_schedule(&irq_worker, K_USEC(500));
}


#define CODEC_DAI_CFG_I2S_STEREO	2

#define CODEC_DAI_CFG_I2S_FORMAT	(I2S_FMT_DATA_FORMAT_I2S | \
									 I2S_FMT_CLK_NF_NB)

#define CODEC_DAI_CFG_I2S_OPTION	(I2S_OPT_BIT_CLK_GATED | \
									 I2S_OPT_BIT_CLK_SLAVE | \
									 I2S_OPT_FRAME_CLK_SLAVE)

struct audio_codec_cfg cfg = {
	.dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_24_BITS,
	.dai_cfg.i2s.channels = CODEC_DAI_CFG_I2S_STEREO,
	.dai_cfg.i2s.frame_clk_freq = AUDIO_PCM_RATE_48K,
	.dai_cfg.i2s.format = CODEC_DAI_CFG_I2S_FORMAT,
	.dai_cfg.i2s.options = CODEC_DAI_CFG_I2S_OPTION,

	.dai_route = AUDIO_ROUTE_PLAYBACK,
	.dai_type = AUDIO_DAI_TYPE_I2S,
};

static int init_codecs(void)
{
	audio_property_value_t val;
	int ret;

	if (amps[AUDIO_AMP_LEFT] == NULL || amps[AUDIO_AMP_RIGHT] == NULL ) {
		LOG_ERR("No CS35L45 stereo pair found...");
		return -ENODEV;
	}

	val.mute = false;

	for (int i = 0; i < ARRAY_SIZE(amps); i++) {
		if (amps[i] == NULL) {
			continue;
		}

		if (!device_is_ready(amps[i])) {
			LOG_ERR("Error: %s is not ready", amps[i]->name);
			return -EIO;
		}

		ret = audio_codec_configure(amps[i], &cfg);
		if (ret < 0) {
			LOG_ERR("Error: Failed to configure %s codec.", amps[i]->name);
			return ret;
		}

		if ((i % 2) == 0) {
			ret = audio_codec_route_output(amps[i], AUDIO_CHANNEL_FRONT_LEFT, 1);
			if (ret < 0) {
				LOG_ERR("Error: Failed to route left output path.");
				return ret;
			}

			ret = audio_codec_route_input(amps[i], AUDIO_CHANNEL_ALL, 1);
			if (ret < 0) {
				LOG_ERR("Error: Failed to route left input path.");
				return ret;
			}

			ret = audio_codec_set_property(amps[i], AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, val);
			if (ret < 0) {
				LOG_ERR("Error: Failed to unmute left amp, %d", ret);
				return ret;
			}
		} else {
			ret = audio_codec_route_output(amps[i], AUDIO_CHANNEL_FRONT_RIGHT, 2);
			if (ret < 0) {
				LOG_ERR("Error: Failed to route right output path.");
				return ret;
			}

			ret = audio_codec_route_input(amps[i], AUDIO_CHANNEL_ALL, 2);
			if (ret < 0) {
				LOG_ERR("Error: Failed to route right input path.");
				return ret;
			}

			ret = audio_codec_set_property(amps[i], AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, val);
			if (ret < 0) {
				LOG_ERR("Error: Failed to unmute right amp");
				return ret;
			}
		}
	}

	return 0;
}

static int init_button(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}

	k_work_init_delayable(&irq_worker, button_worker);

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("failed to configure %s", button.port->name);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("failed to configure %s", button.port->name);
		return ret;
	}

	gpio_init_callback(&button_cb_data, button_handler, BIT(button.pin));

	return gpio_add_callback(button.port, &button_cb_data);
}

int main(void)
{
	int ret;

	ret = init_button();
	if (ret != 0) {
		LOG_ERR("Failed to initialize button");
	}

	ret = init_codecs();
	if (ret != 0) {
		LOG_ERR("Failed to initialize codecs\n");
		return ret;
	}

	return 0;
}
