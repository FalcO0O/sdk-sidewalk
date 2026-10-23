/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <app_ui.h>

#include <dk_buttons_and_leds.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/math_extras.h>

LOG_MODULE_REGISTER(app_ui, CONFIG_SIDEWALK_LOG_LEVEL);

#define LED_BLINK_MS 100
#define LED_SPEC(alias) GPIO_DT_SPEC_GET_OR(DT_ALIAS(alias), gpios, {0})

static const struct gpio_dt_spec leds[APP_UI_LED_COUNT] = {
	[APP_UI_LED_BLE] = LED_SPEC(loc_led_ble),
	[APP_UI_LED_WIFI] = LED_SPEC(loc_led_wifi),
	[APP_UI_LED_GNSS] = LED_SPEC(loc_led_gnss),
	[APP_UI_LED_ERROR] = LED_SPEC(loc_led_error),
};

static app_ui_button_cb_t button_handler;

static void leds_off(struct k_work *work)
{
	ARG_UNUSED(work);

	for (int i = 0; i < APP_UI_LED_COUNT; i++) {
		if (leds[i].port) {
			(void)gpio_pin_set_dt(&leds[i], 0);
		}
	}
}

static K_WORK_DELAYABLE_DEFINE(leds_off_work, leds_off);

static void on_buttons_changed(uint32_t button_state, uint32_t has_changed)
{
	uint32_t pressed = button_state & has_changed;

	while (pressed && button_handler) {
		uint8_t button = u32_count_trailing_zeros(pressed);

		pressed &= ~BIT(button);
		button_handler(button);
	}
}

int app_ui_init(app_ui_button_cb_t button_cb)
{
	for (int i = 0; i < APP_UI_LED_COUNT; i++) {
		if (!leds[i].port) {
			continue;
		}
		if (!gpio_is_ready_dt(&leds[i])) {
			LOG_ERR("LED %d not ready", i);
			return -ENODEV;
		}
		int err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);

		if (err) {
			LOG_ERR("LED %d configure err %d", i, err);
			return err;
		}
	}

	button_handler = button_cb;

	return dk_buttons_init(on_buttons_changed);
}

void app_ui_blink(enum app_ui_led led)
{
	if (led >= APP_UI_LED_COUNT || !leds[led].port) {
		return;
	}

	(void)gpio_pin_set_dt(&leds[led], 1);
	(void)k_work_reschedule(&leds_off_work, K_MSEC(LED_BLINK_MS));
}
