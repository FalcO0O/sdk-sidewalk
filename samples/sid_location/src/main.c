/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <app_ui.h>

#include <app_ble_config.h>
#include <app_mfg_config.h>
#include <app_subGHz_config.h>
#include <json_printer/sidTypes2str.h>
#include <sid_api.h>
#include <sid_error.h>
#include <sid_location.h>
#include <sid_pal_common_ifc.h>
#include <sidewalk_version.h>
#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
#include <sid_pal_radio_ifc.h>
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */
#ifdef CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110
#include <app_location_lr11xx_config.h>
#endif /* CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(sid_location_app, CONFIG_SIDEWALK_LOG_LEVEL);

#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
#define APP_LINK_MASK (SID_LINK_TYPE_1 | SID_LINK_TYPE_3)
#else
#define APP_LINK_MASK (SID_LINK_TYPE_1)
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */

/* With L1 as the only level, managed effort has nothing to manage and rejects DEFAULT runs */
#if defined(CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110)
#define APP_LOCATION_METHODS (SID_LOCATION_METHOD_ALL)
#define APP_LOCATION_MAX_EFFORT (SID_LOCATION_EFFORT_L4)
#define APP_LOCATION_MANAGED (true)
#define APP_LOCATION_PERIODIC_EFFORT (SID_LOCATION_EFFORT_DEFAULT)
#else
#define APP_LOCATION_METHODS (SID_LOCATION_METHOD_BLE_GATEWAY)
#define APP_LOCATION_MAX_EFFORT (SID_LOCATION_EFFORT_L1)
#define APP_LOCATION_MANAGED (false)
#define APP_LOCATION_PERIODIC_EFFORT (SID_LOCATION_EFFORT_L1)
#endif /* CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110 */

/* Lets the library step down to L1 before the next periodic run */
#define APP_LOCATION_STEPDOWN_MS 10000

/* Button index to the location level it runs. Other buttons do nothing. */
static const enum sid_location_effort_mode button_effort[] = {
	SID_LOCATION_EFFORT_L1,
#if defined(CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110)
	SID_LOCATION_EFFORT_L3,
	SID_LOCATION_EFFORT_L4,
#endif /* CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110 */
};

enum app_event_type {
	APP_EVENT_SID_PROCESS,
	APP_EVENT_SID_STATUS,
	APP_EVENT_CYCLE,
	APP_EVENT_BUTTON,
	APP_EVENT_LOCATION_RESULT,
	APP_EVENT_RUN_TIMEOUT,
};

struct app_event {
	uint8_t type;
	uint8_t arg;
};

K_MSGQ_DEFINE(app_msgq, sizeof(struct app_event), 16, 4);

enum app_state {
	APP_STATE_CONNECTING,
	APP_STATE_IDLE,
	APP_STATE_LOCATING,
};

struct app_ctx {
	struct smf_ctx smf;
	struct app_event event;
	struct sid_handle *handle;
	struct sid_status status;
	struct sid_location_result result;
	enum sid_location_effort_mode run_effort;
	enum sid_location_effort_mode failed_effort;
	uint8_t attempts;
	bool l1_pending;
};

static struct app_ctx app;
static const struct smf_state app_states[];

static void app_event_post(enum app_event_type type, uint8_t arg)
{
	struct app_event event = { .type = type, .arg = arg };

	if (k_msgq_put(&app_msgq, &event, K_NO_WAIT)) {
		LOG_WRN("Event %d dropped, queue full", type);
	}
}

static void cycle_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	app_event_post(APP_EVENT_CYCLE, 0);
}

static void run_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	app_event_post(APP_EVENT_RUN_TIMEOUT, 0);
}

static K_TIMER_DEFINE(cycle_timer, cycle_timer_cb, NULL);
static K_TIMER_DEFINE(run_timer, run_timer_cb, NULL);

static void cycle_timer_restart(void)
{
	k_timer_start(&cycle_timer, K_SECONDS(CONFIG_SID_LOCATION_INTERVAL_S),
		      K_SECONDS(CONFIG_SID_LOCATION_INTERVAL_S));
}

/* Sidewalk callbacks. All but on_event run in sid_process(), in the application thread. */

static void on_sidewalk_event(bool in_isr, void *context)
{
	ARG_UNUSED(in_isr);
	ARG_UNUSED(context);
	app_event_post(APP_EVENT_SID_PROCESS, 0);
}

static void on_sidewalk_status_changed(const struct sid_status *status, void *context)
{
	ARG_UNUSED(context);

	app.status = *status;
	LOG_INF("Sidewalk state %d, registered %d, time synced %d, links 0x%x", status->state,
		status->detail.registration_status == SID_STATUS_REGISTERED,
		status->detail.time_sync_status == SID_STATUS_TIME_SYNCED,
		status->detail.link_status_mask);
	app_event_post(APP_EVENT_SID_STATUS, 0);
}

static void on_sidewalk_msg_received(const struct sid_msg_desc *msg_desc, const struct sid_msg *msg,
				     void *context)
{
	ARG_UNUSED(msg_desc);
	ARG_UNUSED(msg);
	ARG_UNUSED(context);
}

static void on_sidewalk_msg_sent(const struct sid_msg_desc *msg_desc, void *context)
{
	ARG_UNUSED(msg_desc);
	ARG_UNUSED(context);
}

static void on_sidewalk_send_error(sid_error_t error, const struct sid_msg_desc *msg_desc,
				   void *context)
{
	ARG_UNUSED(msg_desc);
	ARG_UNUSED(context);
	LOG_WRN("Send error %d (%s)", (int)error, SID_ERROR_T_STR(error));
}

static void on_sidewalk_factory_reset(void *context)
{
	ARG_UNUSED(context);
}

static void on_location_update(const struct sid_location_result *const result, void *context)
{
	ARG_UNUSED(context);

	app.result = *result;
	app_event_post(APP_EVENT_LOCATION_RESULT, 0);
}

static void on_button(uint8_t button)
{
	app_event_post(APP_EVENT_BUTTON, button);
}

/* Location */

static enum app_ui_led effort_to_led(enum sid_location_effort_mode mode)
{
	switch (mode) {
	case SID_LOCATION_EFFORT_L1:
		return APP_UI_LED_BLE;
	case SID_LOCATION_EFFORT_L3:
		return APP_UI_LED_WIFI;
	case SID_LOCATION_EFFORT_L4:
		return APP_UI_LED_GNSS;
	default:
		return APP_UI_LED_COUNT;
	}
}

static int location_init(void)
{
	static struct sid_location_config config = {
		.sid_location_type_mask = APP_LOCATION_METHODS,
		.max_effort = APP_LOCATION_MAX_EFFORT,
		.manage_effort = APP_LOCATION_MANAGED,
		.callbacks = {
			.on_update = on_location_update,
		},
		.stepdowns = {
			.l4_to_l3 = APP_LOCATION_STEPDOWN_MS,
			.l3_to_l2 = APP_LOCATION_STEPDOWN_MS,
			.l2_to_l1 = APP_LOCATION_STEPDOWN_MS,
		},
		.auto_link_pause_resume = IS_ENABLED(CONFIG_SIDEWALK_SUBGHZ_SUPPORT),
	};

	sid_error_t err = sid_location_init(app.handle, &config);

	if (err) {
		LOG_ERR("Location init err %d (%s)", (int)err, SID_ERROR_T_STR(err));
		return -EIO;
	}
	return 0;
}

static void location_run(enum sid_location_effort_mode effort)
{
	struct sid_location_run_config config = {
		.type = SID_LOCATION_SCAN_AND_SEND,
		.mode = effort,
	};

	sid_error_t err = sid_location_run(app.handle, &config, 0);
	enum sid_location_effort_mode managed = sid_location_get_effort_mode(app.handle);

	/* After a run with an explicit level, the library can step the managed level down
	 * below L1 and then rejects every DEFAULT run. Init sets the managed level again.
	 */
	if (err && effort == SID_LOCATION_EFFORT_DEFAULT &&
	    (managed < SID_LOCATION_EFFORT_L1 || managed > APP_LOCATION_MAX_EFFORT)) {
		LOG_WRN("Managed effort %d out of range, location init again", managed);
		(void)sid_location_deinit(app.handle);
		if (!location_init()) {
			err = sid_location_run(app.handle, &config, 0);
		}
	}

	if (err) {
		LOG_ERR("Location run err %d (%s)", (int)err, SID_ERROR_T_STR(err));
		app_ui_blink(APP_UI_LED_ERROR);
		smf_set_state(SMF_CTX(&app), &app_states[APP_STATE_IDLE]);
		return;
	}

	LOG_INF("Location run started, effort %d", effort);
	app.run_effort = effort;
	smf_set_state(SMF_CTX(&app), &app_states[APP_STATE_LOCATING]);
}

static void location_start(enum sid_location_effort_mode effort)
{
	/* L1 has nothing to send that would trigger auto-connect, so it needs the
	 * Bluetooth LE link up before the run.
	 */
	if (effort == SID_LOCATION_EFFORT_L1 &&
	    !(app.status.detail.link_status_mask & SID_LINK_TYPE_1)) {
		LOG_INF("L1 waits for a Bluetooth LE connection");
		app.l1_pending = true;
		(void)sid_ble_bcn_connection_request(app.handle, true);
		return;
	}
	app.l1_pending = false;
	location_run(effort);
}

/* States */

static enum smf_state_result connecting_run(void *o)
{
	ARG_UNUSED(o);

	if (app.event.type != APP_EVENT_SID_STATUS || app.status.state != SID_STATE_READY ||
	    app.status.detail.registration_status != SID_STATUS_REGISTERED ||
	    app.status.detail.time_sync_status != SID_STATUS_TIME_SYNCED) {
		return SMF_EVENT_HANDLED;
	}

	if (location_init()) {
		app_ui_blink(APP_UI_LED_ERROR);
		return SMF_EVENT_HANDLED;
	}

	smf_set_state(SMF_CTX(&app), &app_states[APP_STATE_IDLE]);
	cycle_timer_restart();
	location_start(APP_LOCATION_PERIODIC_EFFORT);
	return SMF_EVENT_HANDLED;
}

static enum smf_state_result idle_run(void *o)
{
	ARG_UNUSED(o);

	switch (app.event.type) {
	case APP_EVENT_CYCLE:
		location_start(APP_LOCATION_PERIODIC_EFFORT);
		break;
	case APP_EVENT_BUTTON:
		if (app.event.arg < ARRAY_SIZE(button_effort)) {
			cycle_timer_restart();
			location_start(button_effort[app.event.arg]);
		}
		break;
	case APP_EVENT_SID_STATUS:
		if (app.l1_pending && (app.status.detail.link_status_mask & SID_LINK_TYPE_1)) {
			location_start(SID_LOCATION_EFFORT_L1);
		}
		break;
	default:
		break;
	}

	return SMF_EVENT_HANDLED;
}

static void locating_entry(void *o)
{
	ARG_UNUSED(o);
	app.attempts = 1;
	app.failed_effort = 0;
	k_timer_start(&run_timer, K_SECONDS(CONFIG_SID_LOCATION_RUN_TIMEOUT_S), K_NO_WAIT);
}

static enum smf_state_result locating_run(void *o)
{
	ARG_UNUSED(o);
	const struct sid_location_result *result = &app.result;

	switch (app.event.type) {
	case APP_EVENT_LOCATION_RESULT:
		LOG_INF("Location err %d, effort %d, link 0x%x", (int)result->err, result->mode,
			result->link);
		/* The library also reports events that are not a run result, without a level */
		if (result->mode < SID_LOCATION_EFFORT_L1 || result->mode > SID_LOCATION_EFFORT_L4) {
			break;
		}
		if (result->err == SID_ERROR_NONE) {
			app_ui_blink(effort_to_led(result->mode));
			smf_set_state(SMF_CTX(&app), &app_states[APP_STATE_IDLE]);
			break;
		}
		/* Managed effort raises the level after a failure, but the next try is up to
		 * the application. Retry only while the level keeps going up.
		 */
		if (app.run_effort == SID_LOCATION_EFFORT_DEFAULT &&
		    app.attempts < CONFIG_SID_LOCATION_MAX_ATTEMPTS &&
		    result->mode > app.failed_effort && result->mode < APP_LOCATION_MAX_EFFORT) {
			struct sid_location_run_config config = {
				.type = SID_LOCATION_SCAN_AND_SEND,
				.mode = SID_LOCATION_EFFORT_DEFAULT,
			};

			app.failed_effort = result->mode;
			if (sid_location_run(app.handle, &config, 0) == SID_ERROR_NONE) {
				app.attempts++;
				break;
			}
		}
		app_ui_blink(APP_UI_LED_ERROR);
		smf_set_state(SMF_CTX(&app), &app_states[APP_STATE_IDLE]);
		break;
	case APP_EVENT_RUN_TIMEOUT:
		/* Recovers from a stuck LR11xx scan, see the known issues */
		LOG_ERR("Location run timed out, rebooting");
		LOG_PANIC();
		sys_reboot(SYS_REBOOT_COLD);
		break;
	default:
		break;
	}

	return SMF_EVENT_HANDLED;
}

static void locating_exit(void *o)
{
	ARG_UNUSED(o);
	k_timer_stop(&run_timer);
}

static const struct smf_state app_states[] = {
	[APP_STATE_CONNECTING] = SMF_CREATE_STATE(NULL, connecting_run, NULL, NULL, NULL),
	[APP_STATE_IDLE] = SMF_CREATE_STATE(NULL, idle_run, NULL, NULL, NULL),
	[APP_STATE_LOCATING] = SMF_CREATE_STATE(locating_entry, locating_run, locating_exit,
						NULL, NULL),
};

/* Boot */

#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
static sid_pal_radio_rx_packet_t radio_rx_packet;

static void radio_event_notifier(sid_pal_radio_events_t event)
{
	ARG_UNUSED(event);
}

static void radio_irq_handler(void)
{
}
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */

static int platform_init(void)
{
	platform_parameters_t platform_parameters = {
		.mfg_store_region.addr_start = APP_MFG_CFG_FLASH_START,
		.mfg_store_region.addr_end = APP_MFG_CFG_FLASH_END,
#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
		.platform_init_parameters.radio_cfg = get_radio_cfg(),
#ifdef CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110
		.platform_init_parameters.gnss_wifi_cfg =
			(lr11xx_gnss_wifi_config_t *)get_location_cfg(),
#endif /* CONFIG_SIDEWALK_SUBGHZ_RADIO_LR1110 */
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */
	};

	sid_error_t err = sid_platform_init(&platform_parameters);

	if (err) {
		LOG_ERR("Sidewalk platform init err %d (%s)", (int)err, SID_ERROR_T_STR(err));
		return -EIO;
	}

	if (app_mfg_cfg_is_empty()) {
		LOG_ERR("No manufacturing data at 0x%08x, provision the device",
			APP_MFG_CFG_FLASH_START);
		return -ENODATA;
	}

#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
	int32_t radio_err =
		sid_pal_radio_init(radio_event_notifier, radio_irq_handler, &radio_rx_packet);

	if (radio_err) {
		LOG_ERR("Radio init err %d", radio_err);
		return -EIO;
	}
	(void)sid_pal_radio_sleep(0);
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */

	return 0;
}

static int sidewalk_start(void)
{
	static struct sid_event_callbacks callbacks = {
		.context = &app,
		.on_event = on_sidewalk_event,
		.on_msg_received = on_sidewalk_msg_received,
		.on_msg_sent = on_sidewalk_msg_sent,
		.on_send_error = on_sidewalk_send_error,
		.on_status_changed = on_sidewalk_status_changed,
		.on_factory_reset = on_sidewalk_factory_reset,
	};
	static uint16_t sync_intervals_h[] = { 2, 4, 8, 12 };
	static struct sid_time_sync_config time_sync_config = {
		.adaptive_sync_intervals_h = sync_intervals_h,
		.num_intervals = ARRAY_SIZE(sync_intervals_h),
	};
	struct sid_config config = {
		.link_mask = APP_LINK_MASK,
		.dev_ch = {
			.type = SID_END_DEVICE_TYPE_STATIC,
			.power_type = SID_END_DEVICE_POWERED_BY_BATTERY_AND_LINE_POWER,
			.qualification_id = 0x0001,
		},
		.callbacks = &callbacks,
		.link_config = app_get_ble_config(),
		.sub_ghz_link_config = app_get_sub_ghz_config(),
		.time_sync_config = &time_sync_config,
	};

	sid_error_t err = sid_init(&config, &app.handle);

	if (err) {
		LOG_ERR("Sidewalk init err %d (%s)", (int)err, SID_ERROR_T_STR(err));
		return -EIO;
	}
	err = sid_start(app.handle, APP_LINK_MASK);
	if (err) {
		LOG_ERR("Sidewalk start err %d (%s)", (int)err, SID_ERROR_T_STR(err));
		return -EIO;
	}

	/* Connect over Bluetooth LE on demand, when the stack has data to send */
	enum sid_link_connection_policy policy = SID_LINK_CONNECTION_POLICY_AUTO_CONNECT;
	struct sid_link_auto_connect_params params = {
		.link_type = SID_LINK_TYPE_1,
		.enable = true,
		.connection_attempt_timeout_seconds = 30,
	};

	(void)sid_option(app.handle, SID_OPTION_SET_LINK_CONNECTION_POLICY, &policy,
			 sizeof(policy));
	(void)sid_option(app.handle, SID_OPTION_SET_LINK_POLICY_AUTO_CONNECT_PARAMS, &params,
			 sizeof(params));

	return 0;
}

int main(void)
{
	PRINT_SIDEWALK_VERSION();

	/* sid_process() runs in this thread, keep the priority of the Sidewalk thread */
	k_thread_priority_set(k_current_get(), CONFIG_SIDEWALK_THREAD_PRIORITY);

	if (app_ui_init(on_button)) {
		LOG_ERR("UI init failed");
	}

	if (platform_init() || sidewalk_start()) {
		app_ui_blink(APP_UI_LED_ERROR);
		return 0;
	}

	LOG_INF("Waiting for Sidewalk registration and time sync");
	smf_set_initial(SMF_CTX(&app), &app_states[APP_STATE_CONNECTING]);

	while (true) {
		k_msgq_get(&app_msgq, &app.event, K_FOREVER);

		if (app.event.type == APP_EVENT_SID_PROCESS) {
			(void)sid_process(app.handle);
			continue;
		}
		smf_run_state(SMF_CTX(&app));
	}

	return 0;
}
