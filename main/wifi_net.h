/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional Wi-Fi via ESP32-C6 co-processor (ESP-Hosted + esp_wifi_remote).
 * Waveshare ESP32-P4-WIFI6-Dev-Kit uses SDIO between P4 (host) and C6 (slave).
 */
#pragma once

#include "esp_err.h"

/**
 * Initialize Wi-Fi STA when CONFIG_BABELBUS_WIFI_ENABLE is set.
 * Requires esp_hosted + esp_wifi_remote components and a C6 flashed with
 * ESP-Hosted slave firmware (factory default on Waveshare kits).
 */
esp_err_t wifi_net_init(void);
