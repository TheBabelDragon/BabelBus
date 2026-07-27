/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tc358743.h"

#include <string.h>

#include "esp_log.h"
#include "i2c_guard.h"

static const char *TAG = "tc358743";

/*
 * Opaque layout must match tc358743.c exactly (i2c is first member).
 */
struct tc358743 {
    i2c_master_dev_handle_t i2c;
    tc358743_cfg_t cfg;
    bool csi_uyvy422;
};

#define CONFCTL 0x0004
#define SYS_STATUS 0x8520
#define VI_STATUS1 0x8522
#define VOUT_SET2 0x8573

static esp_err_t raw_rd(tc358743_t *d, uint16_t reg, uint8_t *data, size_t len)
{
    if (!d || !d->i2c || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    memset(data, 0, len);
    /* 100 ms timeout instead of -1 so a wedged bus cannot hang forever */
    return i2c_master_transmit_receive(d->i2c, addr, 2, data, len, 100);
}

bool tc358743_bus_ok(tc358743_t *d)
{
    if (!d || !d->i2c) {
        return false;
    }

    /* Prefer the same path as the rest of the driver when possible. */
    uint8_t sys = 0;
    if (tc358743_sys_status(d, &sys) != ESP_OK) {
        ESP_LOGE(TAG, "bus_ok: sys_status fail");
        return false;
    }

    uint8_t vi1 = 0, vout2 = 0, conf[2] = {0, 0};
    esp_err_t e1 = raw_rd(d, VI_STATUS1, &vi1, 1);
    esp_err_t e2 = raw_rd(d, VOUT_SET2, &vout2, 1);
    esp_err_t e3 = raw_rd(d, CONFCTL, conf, 2);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        /* Fall back: sys alone — if it's an obvious mono-fill, bus is bad. */
        if (tc358743_byte_is_obvious_fill(sys)) {
            ESP_LOGE(TAG, "bus_ok: FAIL SYS fill 0x%02x (extra reads %s/%s/%s)", sys,
                     esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
            return false;
        }
        ESP_LOGW(TAG, "bus_ok: partial I2C err e1=%s e2=%s e3=%s SYS=0x%02x", esp_err_to_name(e1),
                 esp_err_to_name(e2), esp_err_to_name(e3), sys);
        return false;
    }

    uint16_t confctl = (uint16_t)conf[0] | ((uint16_t)conf[1] << 8);

    if (tc358743_triple_looks_poisoned(sys, vi1, vout2)) {
        ESP_LOGE(TAG, "bus_ok: FAIL uniform fill 0x%02x CONFCTL=0x%04x", sys, confctl);
        return false;
    }
    if (conf[0] == conf[1] && conf[0] != 0) {
        ESP_LOGE(TAG, "bus_ok: FAIL CONFCTL fill 0x%04x", confctl);
        return false;
    }
    if (tc358743_byte_is_obvious_fill(sys)) {
        ESP_LOGE(TAG, "bus_ok: FAIL SYS obvious fill 0x%02x", sys);
        return false;
    }

    ESP_LOGI(TAG, "bus_ok: SYS=0x%02x VI1=0x%02x VOUT2=0x%02x CONFCTL=0x%04x", sys, vi1, vout2, confctl);
    return true;
}

void tc358743_log_link_state(tc358743_t *dev)
{
    if (!dev) {
        return;
    }
    uint8_t st = 0;
    (void)tc358743_sys_status(dev, &st);
    bool ok = tc358743_bus_ok(dev);
    ESP_LOGI(TAG, "link SYS=0x%02x bus_ok=%d", st, (int)ok);
}

esp_err_t tc358743_get_detected_timing(tc358743_t *dev, uint16_t *hact, uint16_t *vact)
{
    if (!dev || !hact || !vact) {
        return ESP_ERR_INVALID_ARG;
    }
    *hact = 0;
    *vact = 0;
    if (!tc358743_bus_ok(dev)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t h0 = 0, h1 = 0, v0 = 0, v1 = 0;
    if (raw_rd(dev, 0x8582, &h0, 1) != ESP_OK || raw_rd(dev, 0x8583, &h1, 1) != ESP_OK ||
        raw_rd(dev, 0x8588, &v0, 1) != ESP_OK || raw_rd(dev, 0x8589, &v1, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    *hact = (uint16_t)h0 | (uint16_t)((h1 & 0x1fu) << 8);
    *vact = (uint16_t)v0 | (uint16_t)((v1 & 0x1fu) << 8);
    return ESP_OK;
}
