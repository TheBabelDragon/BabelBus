/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single place for bus health. Uses only tc358743 public APIs (no second
 * I2C client, no RMW, no register spam).
 *
 * Linux drivers/media/i2c/tc358743.c: CHIPID register must be 0 for a real chip.
 */
#include "tc358743.h"

#include "esp_log.h"

static const char *TAG = "tc358743";

bool tc358743_bus_ok(tc358743_t *dev)
{
    if (!dev) {
        return false;
    }
    uint16_t id = 0xffff;
    if (tc358743_read_chip_id(dev, &id) != ESP_OK) {
        ESP_LOGE(TAG, "CHIPID read failed");
        return false;
    }
    /* Real TC358743: CHIPID == 0. Fill words (0xFDFD, 0x7F7F, ...) are not. */
    if (id != 0) {
        ESP_LOGE(TAG, "CHIPID=0x%04x (need 0) — bridge not responding as TC358743", id);
        return false;
    }
    return true;
}

void tc358743_log_link_state(tc358743_t *dev)
{
    if (!dev) {
        return;
    }
    uint8_t st = 0;
    uint16_t id = 0;
    (void)tc358743_sys_status(dev, &st);
    (void)tc358743_read_chip_id(dev, &id);
    ESP_LOGI(TAG, "CHIPID=0x%04x SYS=0x%02x bus_ok=%d", id, st, (int)tc358743_bus_ok(dev));
}

esp_err_t tc358743_get_detected_timing(tc358743_t *dev, uint16_t *hact, uint16_t *vact)
{
    if (!dev || !hact || !vact) {
        return ESP_ERR_INVALID_ARG;
    }
    *hact = 0;
    *vact = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
