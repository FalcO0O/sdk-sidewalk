/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef APP_UI_H
#define APP_UI_H

#include <stdint.h>

enum app_ui_led {
	APP_UI_LED_BLE,
	APP_UI_LED_WIFI,
	APP_UI_LED_GNSS,
	APP_UI_LED_ERROR,
	APP_UI_LED_COUNT,
};

/** @brief Called from the system work queue when a button is pressed.
 *
 *  @param button index of the pressed button, starting from 0.
 */
typedef void (*app_ui_button_cb_t)(uint8_t button);

/** @brief Initialize the LEDs and the buttons.
 *
 *  @param button_cb handler for button presses.
 *
 *  @return 0 in case of success, negative error code otherwise.
 */
int app_ui_init(app_ui_button_cb_t button_cb);

/** @brief Blink the LED once.
 *
 *  Does nothing if the board has no LED for this role.
 */
void app_ui_blink(enum app_ui_led led);

#endif /* APP_UI_H */
