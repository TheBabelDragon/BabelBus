/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Upstream p4kvm / Linux log CHIPID but do not abort init on it.
 * bus_ok is always true if the device handle exists — HDMI lock uses SYS_STATUS.
 */
#include "tc358743.h"

#include "esp_log.h"

static const char *TAG = "tc358743";

bool tc358743_bus_ok(tc358743_t *dev)
{
    return dev != NULL;
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
    ESP_LOGI(TAG, "CHIPID=0x%04x SYS_STATUS=0x%02x", id, st);
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
