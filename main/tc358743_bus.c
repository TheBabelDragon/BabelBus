/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bus health: after init, CONFCTL must not be a uniform fill (0x9F9F etc).
 * Uniform fill means I2C read/write is not actually programming the chip.
 */
#include "tc358743.h"

#include "esp_log.h"
#include "i2c_guard.h"

static const char *TAG = "tc358743";

/*
 * These use the same I2C device as the main driver via public sys_status only
 * is not enough — we need CONFCTL. Implemented by duplicating a minimal
 * register read through the probe handle stored in the opaque struct.
 *
 * The opaque struct layout must match tc358743.c:
 *   i2c_master_dev_handle_t i2c;
 *   tc358743_cfg_t cfg;
 *   bool csi_uyvy422;
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
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    return i2c_master_transmit_receive(d->i2c, addr, 2, data, len, -1);
}

bool tc358743_bus_ok(tc358743_t *d)
{
    if (!d || !d->i2c) {
        return false;
    }
    uint8_t sys = 0, vi1 = 0, vout2 = 0;
    uint8_t conf[2] = {0, 0};
    if (raw_rd(d, SYS_STATUS, &sys, 1) != ESP_OK) {
        ESP_LOGE(TAG, "bus_ok: SYS_STATUS read fail");
        return false;
    }
    if (raw_rd(d, VI_STATUS1, &vi1, 1) != ESP_OK) {
        ESP_LOGE(TAG, "bus_ok: VI_STATUS1 read fail");
        return false;
    }
    if (raw_rd(d, VOUT_SET2, &vout2, 1) != ESP_OK) {
        ESP_LOGE(TAG, "bus_ok: VOUT_SET2 read fail");
        return false;
    }
    if (raw_rd(d, CONFCTL, conf, 2) != ESP_OK) {
        ESP_LOGE(TAG, "bus_ok: CONFCTL read fail");
        return false;
    }
    uint16_t confctl = (uint16_t)conf[0] | ((uint16_t)conf[1] << 8);

    /* Uniform fill across unrelated regs => I2C not talking to a real map. */
    if (tc358743_triple_looks_poisoned(sys, vi1, vout2)) {
        ESP_LOGE(TAG, "bus_ok: FAIL uniform fill SYS=VI1=VOUT2=0x%02x CONFCTL=0x%04x", sys, confctl);
        return false;
    }
    if (tc358743_word_is_bus_garbage(confctl)) {
        ESP_LOGE(TAG, "bus_ok: FAIL CONFCTL fill 0x%04x (writes never stuck)", confctl);
        return false;
    }
    /* After init CONFCTL should have some structure; 0x9F9F/0x7F7F are not valid. */
    if (conf[0] == conf[1] && conf[0] != 0) {
        ESP_LOGE(TAG, "bus_ok: FAIL CONFCTL 0x%04x looks like fill", confctl);
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
    ESP_LOGI(TAG, "link SYS_STATUS=0x%02x bus_ok=%d", st, (int)tc358743_bus_ok(dev));
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
    /* Timing regs only trustworthy when bus_ok. */
    uint8_t h0 = 0, h1 = 0, v0 = 0, v1 = 0;
    if (raw_rd(dev, 0x8582, &h0, 1) != ESP_OK || raw_rd(dev, 0x8583, &h1, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    if (raw_rd(dev, 0x8588, &v0, 1) != ESP_OK || raw_rd(dev, 0x8589, &v1, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    *hact = (uint16_t)h0 | (uint16_t)((h1 & 0x1fu) << 8);
    *vact = (uint16_t)v0 | (uint16_t)((v1 & 0x1fu) << 8);
    return ESP_OK;
}
