/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Upstream p4kvm / Linux log CHIPID but do not abort init on it.
 * bus_ok is always true if the device handle exists — HDMI lock uses SYS_STATUS.
 */
#include "tc358743.h"

#include "esp_log.h"

/* Measured input timing (same map as Linux / Toshiba REF). */
#define HACT0 0x8582
#define HACT1 0x8583
#define VACT0 0x8588
#define VACT1 0x8589

static const char *TAG = "tc358743";

/* Minimal register access for timing reads (same I2C device as main driver). */
extern esp_err_t tc358743_sys_status(tc358743_t *d, uint8_t *out_st);
extern esp_err_t tc358743_read_chip_id(tc358743_t *d, uint16_t *chip_id);

/* Forward decls for private rd helpers — implemented via public sys_status path
 * is not enough; we need raw register reads. Exposed as weak stubs if linked
 * from tc358743.c by including a small read API below. */
uint8_t tc358743_rd8(tc358743_t *d, uint16_t reg);

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
    uint8_t h0 = tc358743_rd8(dev, HACT0);
    uint8_t h1 = tc358743_rd8(dev, HACT1);
    uint8_t v0 = tc358743_rd8(dev, VACT0);
    uint8_t v1 = tc358743_rd8(dev, VACT1);
    *hact = (uint16_t)h0 | (uint16_t)((h1 & 0x1fu) << 8);
    *vact = (uint16_t)v0 | (uint16_t)((v1 & 0x1fu) << 8);
    return ESP_OK;
}
