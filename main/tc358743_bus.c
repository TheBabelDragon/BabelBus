/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare ESP32-P4-WIFI6-DEV-KIT: CSI I2C is GPIO7=SDA GPIO8=SCL
 * (wiki + schematic). Shared with onboard ES8311. External 2.2k pullups.
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

#define CONFCTL 0x0004
#define MASK_YCBCRFMT_422_8_BIT 0x00c0
#define MASK_VBUFEN 0x0001
#define MASK_ABUFEN 0x0002
#define MASK_AUDCHNUM_2 0x0c00
#define MASK_AUDOUTSEL_I2S 0x0010
#define MASK_AUTOINDEX 0x0004
#define FIFOCTL 0x0006

static esp_err_t raw_rd(tc358743_t *d, uint16_t reg, uint8_t *data, size_t len)
{
    if (!d || !d->i2c || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    memset(data, 0, len);
    return i2c_master_transmit_receive(d->i2c, addr, 2, data, len, 200);
}

static esp_err_t raw_wr16(tc358743_t *d, uint16_t reg, uint16_t val)
{
    uint8_t buf[4] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff), (uint8_t)(val & 0xff),
                      (uint8_t)(val >> 8)};
    return i2c_master_transmit(d->i2c, buf, 4, 200);
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
    (void)raw_wr16(d, FIFOCTL, 374);
    uint16_t fifo_rb = raw_rd16(d, FIFOCTL);
    ESP_LOGI(TAG, "FIFOCTL wrote 374 readback 0x%04x", fifo_rb);

    if (raw_wr16(d, CONFCTL, want) != ESP_OK) {
        ESP_LOGE(TAG, "CONFCTL write failed");
        return false;
    }
    uint16_t conf_rb = raw_rd16(d, CONFCTL);
    ESP_LOGI(TAG, "CONFCTL wrote 0x%04x readback 0x%04x", want, conf_rb);
    if (conf_rb == 0xffff || is_fill16(conf_rb) || (conf_rb & MASK_VBUFEN) == 0) {
        ESP_LOGE(TAG, "CONFCTL still bad 0x%04x", conf_rb);
        return false;
    }
    return true;
}

bool tc358743_bus_ok(tc358743_t *d)
{
    if (!d) {
        return false;
    }

    /* Prefer the same path as init_streaming (not a second I2C client path). */
    uint16_t id = 0;
    if (tc358743_read_chip_id(d, &id) != ESP_OK) {
        ESP_LOGE(TAG, "bus_ok: CHIPID API fail");
        return false;
    }

    /*
     * Linux tc358743: CHIPID register field is 0 for a real chip.
     * Fill words (0xFDFD, 0xF7F7, 0x7F7F, ...) mean the bus is not returning
     * real register data from a TC358743.
     */
    if (is_fill16(id)) {
        ESP_LOGE(TAG,
                 "bus_ok: CHIPID=0x%04x is fill — not a TC358743 register map. "
                 "Waveshare DEV-KIT CSI I2C is GPIO7/SDA GPIO8/SCL (correct in firmware). "
                 "Bridge on CSI ribbon is not returning valid Toshiba data.",
                 id);
        return false;
    }

    uint16_t conf = raw_rd16(d, CONFCTL);
    if (conf == 0xffff || is_fill16(conf) || (conf & MASK_VBUFEN) == 0) {
        ESP_LOGW(TAG, "bus_ok: programming CONFCTL (was 0x%04x)", conf);
        if (!force_program_confctl_uyvy(d)) {
            return false;
        }
    }

    uint8_t sys = 0;
    (void)tc358743_sys_status(d, &sys);
    ESP_LOGI(TAG, "bus_ok: CHIPID=0x%04x SYS=0x%02x", id, sys);
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
    return ESP_ERR_NOT_SUPPORTED;
}
