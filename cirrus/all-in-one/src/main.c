/*
 * Copyright (c) 2026 Cirrus Logic, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <audio/cs42l45.h>
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#if CONFIG_SHELL
#include <stdlib.h>
#endif /* CONFIG_SHELL */

LOG_MODULE_REGISTER(main);

#define AUDIO_AMP_LEFT       0
#define AUDIO_AMP_RIGHT      1
#define AUDIO_AMP_BACK_LEFT  2
#define AUDIO_AMP_BACK_RIGHT 3

#define ASP1_RX1 1
#define ASP1_RX2 2

#define AMP_DAI_CFG_I2S_STEREO 2

struct audio_codec_cfg amp_cfg = {
	.dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_24_BITS,
	.dai_cfg.i2s.channels = AMP_DAI_CFG_I2S_STEREO,
	.dai_cfg.i2s.frame_clk_freq = AUDIO_PCM_RATE_48K,
	.dai_cfg.i2s.format = (I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB),
	.dai_cfg.i2s.options =
		(I2S_OPT_BIT_CLK_GATED | I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET),
	.dai_route = AUDIO_ROUTE_PLAYBACK,
	.dai_type = AUDIO_DAI_TYPE_I2S,
};

const struct device *amps[] = {
	[AUDIO_AMP_LEFT] = DEVICE_DT_GET(DT_ALIAS(amp_l)),
	[AUDIO_AMP_RIGHT] = DEVICE_DT_GET(DT_ALIAS(amp_r)),
	[AUDIO_AMP_BACK_LEFT] = DEVICE_DT_GET_OR_NULL(DT_ALIAS(amp_bl)),
	[AUDIO_AMP_BACK_RIGHT] = DEVICE_DT_GET_OR_NULL(DT_ALIAS(amp_br)),
};

static const struct device *codec = DEVICE_DT_GET(DT_ALIAS(codec0));

#if !DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw0))
#error "Unsupported board: sw0 devicetree alias is not defined"
#else  /* DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw0)) */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
#endif /* !DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw0)) */

static bool global_mute = false;
static bool monitor_mode = false;
static bool uaj_plug = false;

static void mute_amps(const bool mute)
{
	const audio_property_value_t val = {.mute = mute};
	int ret;

	for (int i = 0; i < ARRAY_SIZE(amps); i++) {
		if (amps[i] != NULL) {
			ret = audio_codec_set_property(amps[i], AUDIO_PROPERTY_OUTPUT_MUTE,
						       AUDIO_CHANNEL_ALL, val);
			if (ret < 0) {
				LOG_ERR("failed to %s %s (%d)", mute ? "mute" : "unmute",
					amps[i]->name, ret);
				return;
			}
		}
	}
}

static void mute_codec(const bool mute)
{
	const audio_property_value_t val = {.mute = mute};
	int ret;

	ret = audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, val);
	if (ret < 0) {
		LOG_DBG("failed to %s %s (%d)", mute ? "mute" : "unmute", codec->name, ret);
		return;
	}

	ret = audio_codec_apply_properties(codec);
	if (ret < 0) {
		LOG_DBG("failed to apply properties for %s (%d)", codec->name, ret);
	}
}

static void start_amps(const bool start)
{
	for (int i = 0; i < ARRAY_SIZE(amps); i++) {
		if (amps[i] != NULL) {
			start ? audio_codec_start_output(amps[i])
			      : audio_codec_stop_output(amps[i]);
		}
	}
}

static inline void switch_to_monitor_mode(void)
{
	if (!global_mute) {
		if (uaj_plug) {
			mute_amps(true);
			mute_codec(false);
		} else {
			mute_codec(true);
			mute_amps(false);
		}
	}

	start_amps(true);
	audio_codec_start_output(codec);

	monitor_mode = true;
}

static inline void switch_to_pc_mode(void)
{
	audio_codec_stop_output(codec);

	/*
	 * The codec holds onto SDCA interrupts in monitor mode and then unconditionally asserts
	 * the SDCA UAJ interrupt when transitioning to PC mode. If the Windows host was previously
	 * playing audio out of the amps, and we transition to PC mode, then there is a brief time
	 * where the amps are active before the SDCA UAJ interrupt is processed by the Windows
	 * host, introducing undesirable behavior where the amps are active for a fraction of a
	 * second before the Windows host switches to the codec. We can prevent this audio artifact
	 * by holding the amps in monitor mode longer than the codec to give the Windows host time
	 * to process the SDCA UAJ interrupt.
	 */
	if (uaj_plug) {
		(void)k_msleep(1000);
	}

	start_amps(false);

	monitor_mode = false;
}

static struct k_work_delayable irq_worker;
static void button_worker(struct k_work *work)
{
	/* Debounce the switch button. */
	if (gpio_pin_get_dt(&button) == 0) {
		return;
	}

	LOG_INF("button pressed");

	if (monitor_mode) {
		switch_to_pc_mode();
	} else {
		switch_to_monitor_mode();
	}
}

static struct gpio_callback button_cb_data;
static void button_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	(void)k_work_schedule(&irq_worker, K_USEC(500));
}

/* Codec is responsible for monitoring the UAJ and reporting events. */
static void codec_callback(const struct device *const dev, const enum cs42l45_event event)
{
	switch (event) {
	/* If a headset is plugged in, mute amplifier output and unmute codec output. */
	case CS42L45_UAJ_PLUGGED:
		uaj_plug = true;

		if (monitor_mode && !global_mute) {
			mute_amps(true);
			mute_codec(false);
		}

		return;
	/* If a headset is unplugged, mute codec output and unmute amplifier output. */
	case CS42L45_UAJ_UNPLUGGED:
		uaj_plug = false;

		if (monitor_mode && !global_mute) {
			mute_codec(true);
			mute_amps(false);
		}

		return;
	/* Ignore these events. */
	case CS42L45_MONITOR_MODE_ENABLED:
		__fallthrough;
	case CS42L45_MONITOR_MODE_DISABLED:
		return;
	/* The codec should not fail to enter monitor mode if a BCLK is present. */
	case CS42L45_MONITOR_MODE_FAILURE:
		LOG_ERR("failed to enter monitor mode, is BCLK missing?");
		return;
	default:
		LOG_DBG("event %d not handled", event);
		return;
	}
}

#if CONFIG_SHELL
#define AIO_PC_HELP     SHELL_HELP("switch to PC mode", NULL)
#define AIO_MM_HELP     SHELL_HELP("switch to monitor mode", NULL)
#define AIO_MUTE_HELP   SHELL_HELP("mute volume in monitor mode", NULL)
#define AIO_UNMUTE_HELP SHELL_HELP("unmute volume in monitor mode", NULL)
#define AIO_VOLUME_HELP SHELL_HELP("set volume in monitor mode", "<volume>")

static int cmd_mm(const struct shell *sh, size_t argc, char **argv)
{
	if (!monitor_mode) {
		switch_to_monitor_mode();
	}

	return 0;
}

static int cmd_pc(const struct shell *sh, size_t argc, char **argv)
{
	if (monitor_mode) {
		switch_to_pc_mode();
	}

	return 0;
}

/* Assuming mutes are global, affecting codec and amps. */
static int cmd_mute(const struct shell *sh, size_t argc, char **argv)
{
	if (uaj_plug) {
		mute_codec(true);
	} else {
		mute_amps(true);
	}

	global_mute = true;

	return 0;
}

/* Assuming unmutes are global, affecting codec and amps. */
static int cmd_unmute(const struct shell *sh, size_t argc, char **argv)
{
	if (uaj_plug) {
		mute_codec(false);
	} else {
		mute_amps(false);
	}

	global_mute = false;

	return 0;
}

static int volume_amps(const audio_property_value_t vol)
{
	int ret;

	for (int i = 0; i < ARRAY_SIZE(amps); i++) {
		if (amps[i] != NULL) {
			ret = audio_codec_set_property(amps[i], AUDIO_PROPERTY_OUTPUT_VOLUME,
						       AUDIO_CHANNEL_ALL, vol);
			if (ret < 0) {
				LOG_DBG("failed to update %s volume (%d)", codec->name, ret);
				return ret;
			}
		}
	}

	return 0;
}

static int volume_codec(const audio_property_value_t vol)
{
	int ret;

	ret = audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, vol);
	if (ret < 0) {
		LOG_DBG("failed to update %s volume (%d)", codec->name, ret);
		return ret;
	}

	ret = audio_codec_apply_properties(codec);
	if (ret < 0) {
		LOG_DBG("failed to apply properties for %s (%d)", codec->name, ret);
	}

	return ret;
}

/* Assuming volume is device-specific, affecting only codec or amps based on current UAJ state. */
static int cmd_volume(const struct shell *sh, size_t argc, char **argv)
{
	audio_property_value_t vol = {.vol = strtol(argv[1], NULL, 10)};

	if (uaj_plug) {
		if (!IN_RANGE(vol.vol, -228, 12)) {
			shell_error(sh, "require -228 <= volume <= 12 for codec (unit: 0.5 db)");
			return -EINVAL;
		}

		return volume_codec(vol);
	} else {
		if (!IN_RANGE(vol.vol, -256, 255)) {
			shell_error(sh, "require -256 <= volume <= 255 for amps (unit: 0.5 dB)");
			return -EINVAL;
		}

		/* Amplifier volume must be in S7.8 format */
		vol.vol <<= 7;

		return volume_amps(vol);
	}
}

SHELL_STATIC_SUBCMD_SET_CREATE(aio_cmds, SHELL_CMD_ARG(mm, NULL, AIO_PC_HELP, cmd_mm, 1, 0),
			       SHELL_CMD_ARG(pc, NULL, AIO_MM_HELP, cmd_pc, 1, 0),
			       SHELL_CMD_ARG(mute, NULL, AIO_MUTE_HELP, cmd_mute, 1, 0),
			       SHELL_CMD_ARG(unmute, NULL, AIO_UNMUTE_HELP, cmd_unmute, 1, 0),
			       SHELL_CMD_ARG(volume, NULL, AIO_VOLUME_HELP, cmd_volume, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(aio, &aio_cmds, "all-in-one shell commands", NULL);
#endif /* CONFIG_SHELL */

static int init_amps(void)
{
	const audio_property_value_t unmute = {.mute = false};
	int ret;

	if (amps[AUDIO_AMP_LEFT] == NULL || amps[AUDIO_AMP_RIGHT] == NULL) {
		LOG_DBG("No stereo pair found");
		return -ENODEV;
	}

	for (int i = 0; i < ARRAY_SIZE(amps); i++) {
		if (amps[i] == NULL) {
			continue;
		}

		if (!device_is_ready(amps[i])) {
			LOG_DBG("%s is not ready", amps[i]->name);
			return -EIO;
		}

		ret = audio_codec_configure(amps[i], &amp_cfg);
		if (ret < 0) {
			LOG_DBG("failed to configure %s (%d)", amps[i]->name, ret);
			return ret;
		}

		if (i % 2 == 0) {
			ret = audio_codec_route_output(amps[i], AUDIO_CHANNEL_FRONT_LEFT, ASP1_RX1);
		} else {
			ret = audio_codec_route_output(amps[i], AUDIO_CHANNEL_FRONT_RIGHT,
						       ASP1_RX2);
		}
		if (ret < 0) {
			LOG_DBG("failed to route %s (%d)", amps[i]->name, ret);
			return ret;
		}

		/* Unmute the amps by default. */
		ret = audio_codec_set_property(amps[i], AUDIO_PROPERTY_OUTPUT_MUTE,
					       AUDIO_CHANNEL_ALL, unmute);
		if (ret < 0) {
			LOG_DBG("failed to unmute %s (%d)", amps[i]->name, ret);
			return ret;
		}
	}

	return 0;
}

static int init_codec(void)
{
	const audio_property_value_t unmute = {.mute = false};
	int ret;

	if (!device_is_ready(codec)) {
		return -ENODEV;
	}

	/* Callback for monitoring plug/unplug events at the application level. */
	cs42l45_register_callback(codec, codec_callback);

	/*
	 * It's possible for the initial UAJ detection procedure to complete before the callback
	 * is registered, in which case the uaj_plug status may be erroneously false. Consequently,
	 * we need to read the initial UAJ status during bringup.
	 */
	ret = cs42l45_get_uaj_state(codec, &uaj_plug);
	if (ret < 0) {
		LOG_DBG("failed to get initial UAJ state for %s (%d)", codec->name, ret);
		return ret;
	}

	/* Unmute the codec by default. */
	ret = audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL,
				       unmute);
	if (ret < 0) {
		LOG_DBG("failed to configure %s (%d)", codec->name, ret);
		return ret;
	}

	return audio_codec_apply_properties(codec);
}

static int init_button(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}

	k_work_init_delayable(&irq_worker, button_worker);

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		LOG_DBG("failed to configure %s (%d)", button.port->name, ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_DBG("failed to configure %s (%d)", button.port->name, ret);
		return ret;
	}

	gpio_init_callback(&button_cb_data, button_handler, BIT(button.pin));

	return gpio_add_callback(button.port, &button_cb_data);
}

int main(void)
{
	int ret;

	ret = init_amps();
	if (ret < 0) {
		LOG_ERR("failed to initialize amps (%d)", ret);
		return ret;
	}

	ret = init_codec();
	if (ret < 0) {
		LOG_ERR("failed to initialize %s (%d)", codec->name, ret);
		return ret;
	}

	ret = init_button();
	if (ret < 0) {
		LOG_ERR("failed to initialize %s (%d)", button.port->name, ret);
		return ret;
	}

	return 0;
}
