/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tc358743.h"

#include <string.h>

#include "esp_log.h"
#include "i2c_guard.h"

static const char *TAG = "tc358743";

struct tc358743 {
    i2c_master_dev_handle_t i2c;
    tc358743_cfg_t cfg;
    bool csi_uyvy422;
};

#define CHIPID 0x0000
#define CONFCTL 0x0004
#define MASK_YCBCRFMT_422_8_BIT 0x00c0
#define MASK_VBUFEN 0x0001
#define MASK_ABUFEN 0x0002
#define MASK_AUDCHNUM_2 0x0c00
#define MASK_AUDOUTSEL_I2S 0x0010
#define MASK_AUTOINDEX 0x0004
#define FIFOCTL 0x0006
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
    return i2c_master_transmit_receive(d->i2c, addr, 2, data, len, 200);
}

static esp_err_t raw_wr(tc358743_t *d, uint16_t reg, const uint8_t *data, size_t len)
{
    if (!d || !d->i2c || len > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[18];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xff);
    if (data && len) {
        memcpy(buf + 2, data, len);
    }
    esp_err_t err = i2c_master_transmit(d->i2c, buf, 2 + len, 200);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C WR 0x%04x: %s", reg, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t raw_wr16(tc358743_t *d, uint16_t reg, uint16_t val)
{
    uint8_t le[2] = {(uint8_t)(val & 0xff), (uint8_t)(val >> 8)};
    return raw_wr(d, reg, le, 2);
}

static uint16_t raw_rd16(tc358743_t *d, uint16_t reg)
{
    uint8_t b[2] = {0, 0};
    if (raw_rd(d, reg, b, 2) != ESP_OK) {
        return 0xffff;
    }
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static bool is_fill16(uint16_t v)
{
    uint8_t lo = (uint8_t)(v & 0xffu);
    uint8_t hi = (uint8_t)(v >> 8);
    return lo == hi && lo != 0;
}

static bool force_program_confctl_uyvy(tc358743_t *d)
{
    const uint16_t want = (uint16_t)(MASK_YCBCRFMT_422_8_BIT | MASK_VBUFEN | MASK_ABUFEN |
                                     MASK_AUDCHNUM_2 | MASK_AUDOUTSEL_I2S | MASK_AUTOINDEX);
    const uint16_t fifo = 374;

    if (raw_wr16(d, FIFOCTL, fifo) != ESP_OK) {
        return false;
    }
    uint16_t fifo_rb = raw_rd16(d, FIFOCTL);
    ESP_LOGI(TAG, "FIFOCTL wrote %u readback 0x%04x", (unsigned)fifo, fifo_rb);

    if (raw_wr16(d, CONFCTL, want) != ESP_OK) {
        return false;
    }
    uint16_t conf_rb = raw_rd16(d, CONFCTL);
    ESP_LOGI(TAG, "CONFCTL wrote 0x%04x readback 0x%04x", want, conf_rb);

    if (conf_rb == 0xffff || is_fill16(conf_rb)) {
        ESP_LOGE(TAG, "CONFCTL readback still bad 0x%04x", conf_rb);
        return false;
    }
    if ((conf_rb & MASK_VBUFEN) == 0) {
        ESP_LOGE(TAG, "CONFCTL missing VBUFEN (0x%04x)", conf_rb);
        return false;
    }
    ESP_LOGI(TAG, "CONFCTL programmed OK (0x%04x)", conf_rb);
    return true;
}

bool tc358743_bus_ok(tc358743_t *d)
{
    if (!d || !d->i2c) {
        return false;
    }

    /* CHIPID: Linux expects 0 in the chip-id field. Fill words mean no device. */
    uint16_t id = raw_rd16(d, CHIPID);
    if (id == 0xffff) {
        ESP_LOGE(TAG, "bus_ok: CHIPID read fail — no ACK/data on I2C");
        return false;
    }
    if (is_fill16(id)) {
        ESP_LOGE(TAG,
                 "bus_ok: CHIPID=0x%04x is fill (not a TC358743). "
                 "I2C is not talking to the bridge (wrong bus, no power/refclk, or dead path).",
                 id);
        return false;
    }

    uint16_t conf = raw_rd16(d, CONFCTL);
    if (conf == 0xffff) {
        ESP_LOGE(TAG, "bus_ok: CONFCTL read fail");
        /* Still try absolute program */
        return force_program_confctl_uyvy(d);
    }
    if (is_fill16(conf) || (conf & MASK_VBUFEN) == 0) {
        ESP_LOGW(TAG, "bus_ok: CONFCTL 0x%04x needs program", conf);
        return force_program_confctl_uyvy(d);
    }

    uint8_t sys = 0;
    (void)tc358743_sys_status(d, &sys);
    ESP_LOGI(TAG, "bus_ok: CHIPID=0x%04x CONFCTL=0x%04x SYS=0x%02x", id, conf, sys);
    return true;
}

void tc358743_log_link_state(tc358743_t *dev)
{
    if (!dev) {
        return;
    }
    uint8_t st = 0;
    (void)tc358743_sys_status(dev, &st);
    ESP_LOGI(TAG, "link SYS=0x%02x bus_ok=%d", st, (int)tc358743_bus_ok(dev));
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
