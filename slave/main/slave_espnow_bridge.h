/*
 * SPDX-FileCopyrightText: 2026 Tactility
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_ESPNOW_BRIDGE_H
#define __SLAVE_ESPNOW_BRIDGE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register ESP-NOW bridge custom-RPC handlers.
 *
 * Call once from app_main() / esp_hosted_coprocessor_init(), after the
 * custom RPC (peer data transfer) subsystem is up. Does not touch WiFi
 * init/mode/start — that's owned by slave_wifi_std.c.
 *
 * @return ESP_OK on success
 */
esp_err_t slave_espnow_bridge_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __SLAVE_ESPNOW_BRIDGE_H */
