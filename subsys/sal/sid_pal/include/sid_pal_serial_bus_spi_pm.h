/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SID_PAL_SERIAL_BUS_SPI_PM_H
#define SID_PAL_SERIAL_BUS_SPI_PM_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_SIDEWALK_SPI_BUS_IDLE_PINS)

/**
 * @brief Suspend the radio SPI bus and park its pins at a defined level.
 *
 * Call it once the transceiver has been put to sleep. SCK, MOSI and MISO are
 * released and pulled up, so that no line of a sleeping transceiver is left
 * floating or held low. The bus is resumed automatically before the next
 * transfer.
 */
void sid_pal_serial_bus_spi_idle(void);

#else

static inline void sid_pal_serial_bus_spi_idle(void)
{
}

#endif /* CONFIG_SIDEWALK_SPI_BUS_IDLE_PINS */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SID_PAL_SERIAL_BUS_SPI_PM_H */
