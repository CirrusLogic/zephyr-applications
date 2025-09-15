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

#define AUDIO_AMP_LEFT			0
#define AUDIO_AMP_RIGHT			1
#define AUDIO_AMP_BACK_LEFT		2
#define AUDIO_AMP_BACK_RIGHT	3

#define ASP1_RX1	1
#define ASP1_RX2	2

const struct device *amps[] = {
	[AUDIO_AMP_LEFT] = DEVICE_DT_GET(DT_NODELABEL(cs35l56_l)),
	[AUDIO_AMP_RIGHT] = DEVICE_DT_GET(DT_NODELABEL(cs35l56_r)),
	[AUDIO_AMP_BACK_LEFT] = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(cs35l56_bl)),
	[AUDIO_AMP_BACK_RIGHT] = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(cs35l56_br)),
};

static volatile bool toggle_playback = false;

#define SW0_NODE	DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios,
							      {0});
static struct gpio_callback button_cb_data;

/*
 * The led0 devicetree alias is optional. If present, we'll use it
 * to turn on the LED whenever the button is pressed.
 */
static struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios,
						     {0});

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	toggle_playback = true;
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
		printk("No CS35L56 stereo pair found...\n");
		return -ENODEV;
	}

	val.mute = false;

	for (int i = 0; i < ARRAY_SIZE(amps); i++) {
		if (amps[i] == NULL) {
			continue;
		}

		if (!device_is_ready(amps[i])) {
			printk("\n Error: %s is not ready;\n", amps[i]->name);
			return -EIO;
		}

		ret = audio_codec_configure(amps[i], &cfg);
		if (ret < 0) {
			printk("\nError: Failed to configure %s codec.\n", amps[i]->name);
			return ret;
		}

		if ((i % 2) == 0) {
			ret = audio_codec_route_input(amps[i], AUDIO_CHANNEL_FRONT_LEFT, ASP1_RX1);
			if (ret < 0) {
				printk("\nError: Failed to route left input path.\n");
				return ret;
			}

			ret = audio_codec_set_property(amps[i], AUDIO_PROPERTY_INPUT_MUTE, AUDIO_CHANNEL_FRONT_LEFT, val);
			if (ret < 0) {
				printk("\nError: Failed to unmute left amp, %d\n", ret);
				return ret;
			}
		} else {
			ret = audio_codec_route_input(amps[i], AUDIO_CHANNEL_FRONT_RIGHT, ASP1_RX2);
			if (ret < 0) {
				printk("\nError: Failed to route right input path.\n");
				return ret;
			}

			ret = audio_codec_set_property(amps[i], AUDIO_PROPERTY_INPUT_MUTE, AUDIO_CHANNEL_FRONT_RIGHT, val);
			if (ret < 0) {
				printk("\nError: Failed to unmute right amp\n");
				return ret;
			}
		}
	}

	return 0;
}

static void set_codec_output_state(const bool playback)
{
	printk("%s audio playback\n", playback ? "Starting" : "Pausing");
	if (playback) {
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
	gpio_pin_set_dt(&led, playback);
}

static void init_led(void)
{
	int ret = 0;

	if (led.port && !gpio_is_ready_dt(&led)) {
		printk("Error %d: LED device %s is not ready; ignoring it\n",
		       ret, led.port->name);
		led.port = NULL;
	}
	if (led.port) {
		ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT);
		if (ret != 0) {
			printk("Error %d: failed to configure LED device %s pin %d\n",
			       ret, led.port->name, led.pin);
			led.port = NULL;
		} else {
			printk("Set up LED at %s pin %d\n", led.port->name, led.pin);
		}
	}
}

static int init_button(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button)) {
		printk("Error: button device %s is not ready\n",
		       button.port->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n",
		       ret, button.port->name, button.pin);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n",
			ret, button.port->name, button.pin);
		return ret;
	}

	return 0;
}

int main(void)
{
	bool playback = false;
	int ret;

	ret = init_button();
	if (ret != 0) {
		printk("Failed to initialize button\n");
	}

	init_led();

	ret = init_codecs();
	if (ret != 0) {
		printk("Failed to initialize codecs\n");
		return ret;
	}

	set_codec_output_state(playback);

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);
	printk("Set up button at %s pin %d\n", button.port->name, button.pin);

	while (1) {
		if (toggle_playback) {
			playback = !playback;
			set_codec_output_state(playback);
			toggle_playback = false;
		}

		printk("Audio playback %s\n", playback ? "in progress" : "is paused");

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
