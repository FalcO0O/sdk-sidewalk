/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/devicetree.h>

#include <sid_pal_serial_bus_ifc.h>
#include <sid_pal_gpio_ifc.h>
#include <sid_pal_serial_bus_spi_pm.h>
#include <app_subGHz_config.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sid_spi_bus, CONFIG_SPI_BUS_LOG_LEVEL);

#define LORA_DT_NODE DT_CHOSEN(zephyr_lora_transceiver)

#define SPI_OPTIONS                                                                                \
	(uint16_t)(SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER | SPI_FULL_DUPLEX)

static const struct spi_dt_spec bus_serial_spec = SPI_DT_SPEC_GET(LORA_DT_NODE, SPI_OPTIONS);

#if defined(CONFIG_SIDEWALK_SPI_BUS_IDLE_PINS)

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/atomic.h>
#include <hal/nrf_gpio.h>

/* Pin functions and numbers of the bus are taken from the default pinctrl state
 * of the SPI controller the transceiver hangs on.
 */
#define SPI_BUS_NODE DT_BUS(LORA_DT_NODE)
#define SPI_BUS_PINCTRL_DEFAULT DT_PINCTRL_BY_IDX(SPI_BUS_NODE, 0, 0)

#define SPI_BUS_PSEL(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx)
#define SPI_BUS_GROUP_PSELS(group_id)                                                              \
	DT_FOREACH_PROP_ELEM_SEP(group_id, psels, SPI_BUS_PSEL, (, ))

static const uint32_t spi_bus_psels[] = {
	DT_FOREACH_CHILD_SEP(SPI_BUS_PINCTRL_DEFAULT, SPI_BUS_GROUP_PSELS, (, ))
};

static atomic_t spi_bus_idling;

void sid_pal_serial_bus_spi_idle(void)
{
	if (!atomic_cas(&spi_bus_idling, 0, 1)) {
		return;
	}

	int err = pm_device_action_run(bus_serial_spec.bus, PM_DEVICE_ACTION_SUSPEND);

	if (err < 0 && err != -EALREADY) {
		LOG_ERR("spi suspend err %d", err);
		atomic_clear(&spi_bus_idling);
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(spi_bus_psels); i++) {
		uint32_t pin = NRF_GET_PIN(spi_bus_psels[i]);

		if (pin == NRF_PIN_DISCONNECTED) {
			continue;
		}

		switch (NRF_GET_FUN(spi_bus_psels[i])) {
		case NRF_FUN_SPIM_SCK:
		case NRF_FUN_SPIM_MOSI:
		case NRF_FUN_SPIM_MISO:
			/* High, never low: a level translator with pull-ups to both
			 * rails (TXS0102 on PCA63569) burns ~0.6 mA per line that is
			 * held low.
			 */
			nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLUP);
			break;
		default:
			break;
		}
	}
}

static void spi_bus_activate(void)
{
	if (!atomic_cas(&spi_bus_idling, 1, 0)) {
		return;
	}

	int err = pm_device_action_run(bus_serial_spec.bus, PM_DEVICE_ACTION_RESUME);

	if (err < 0 && err != -EALREADY) {
		LOG_ERR("spi resume err %d", err);
	}
}

#else

static inline void spi_bus_activate(void)
{
}

#endif /* CONFIG_SIDEWALK_SPI_BUS_IDLE_PINS */

static sid_error_t zephyr_spi_bus_xfer(const struct sid_pal_serial_bus_iface *iface,
				       const struct sid_pal_serial_bus_client *client, uint8_t *tx,
				       uint8_t *rx, size_t xfer_size);
static sid_error_t zephyr_spi_bus_destroy(const struct sid_pal_serial_bus_iface *iface);

static const struct sid_pal_serial_bus_iface zephyr_spi_bus_iface = {
	.xfer = zephyr_spi_bus_xfer,
	.destroy = zephyr_spi_bus_destroy,
};

static sid_error_t zephyr_spi_bus_xfer(const struct sid_pal_serial_bus_iface *iface,
				       const struct sid_pal_serial_bus_client *client, uint8_t *tx,
				       uint8_t *rx, size_t xfer_size)
{
	LOG_DBG("%s(%p, %p, %p, %p, %d)", __func__, iface, client, (void *)tx, (void *)rx,
		xfer_size);

	sid_error_t ret = SID_ERROR_NONE;

	if (iface != &zephyr_spi_bus_iface || (!tx && !rx) || !xfer_size || !client) {
		return SID_ERROR_INVALID_ARGS;
	}

	struct spi_buf tx_buff[] = {
		{
			.buf = tx,
			.len = xfer_size,
		},
	};

	struct spi_buf_set tx_set = { .buffers = tx_buff, .count = 1 };

	struct spi_buf rx_buff[] = {
		{
			.buf = rx,
			.len = xfer_size,
		},
	};

	struct spi_buf_set rx_set = { .buffers = rx_buff, .count = 1 };

	spi_bus_activate();

	int err = spi_transceive_dt(&bus_serial_spec, ((tx) ? &tx_set : NULL),
				    ((rx) ? &rx_set : NULL));

	if (err < 0) {
		LOG_ERR("spi xfer err %d", err);
		ret = SID_ERROR_GENERIC;
	}

	return ret;
}

static sid_error_t zephyr_spi_bus_destroy(const struct sid_pal_serial_bus_iface *iface)
{
	LOG_DBG("%s(%p)", __func__, iface);
	if (!iface) {
		return SID_ERROR_INVALID_ARGS;
	}
	return SID_ERROR_NONE;
}

sid_error_t sid_pal_serial_bus_nordic_spi_create(const struct sid_pal_serial_bus_iface **iface,
						 const void *cfg)
{
	ARG_UNUSED(cfg);

	if (!iface) {
		return SID_ERROR_INVALID_ARGS;
	}

	if (!spi_is_ready_dt(&bus_serial_spec)) {
		LOG_ERR("SPI device not ready");
		return SID_ERROR_IO_ERROR;
	}

	*iface = &zephyr_spi_bus_iface;

	return SID_ERROR_NONE;
}
