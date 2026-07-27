/* SPDX-FileCopyrightText: 2026 SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_log.h"
#include "tc358743.h"

/**
 * Linux tc358743_regs.h SYS_STATUS (0x8520):
 *  bit0 DDC5V  bit1 TMDS  bit2 PHY_PLL  bit3 PHY_SCDT
 *  bit4 HDMI   bit5 HDCP  bit6 AVMUTE  bit7 SYNC
 *
 * Stuck I2C fill patterns observed when CSI ribbon / bus is bad:
 *   0x7F, 0x97, 0xFF on *every* register + AVI ESP_ERR_INVALID_RESPONSE.
 * Do NOT treat 0x00 as garbage — that is normal with HDMI unplugged.
 */
static inline bool tc358743_status_looks_like_i2c_garbage(uint8_t st)
{
    return st == 0x7fu || st == 0x97u || st == 0xffu;
}

static inline void tc358743_debug_status(tc358743_t *dev)
{
    if (!dev) {
        return;
    }
    uint8_t st = 0;
    if (tc358743_sys_status(dev, &st) != ESP_OK) {
        ESP_LOGE("tc358743", "SYS_STATUS I2C read failed");
        return;
    }
    const int ddc = (int)(st & 1);
    const int tmds = (int)((st >> 1) & 1);
    const int pll = (int)((st >> 2) & 1);
    const int scdt = (int)((st >> 3) & 1);
    const int hdmi = (int)((st >> 4) & 1);
    const int hdcp = (int)((st >> 5) & 1);
    const int avmute = (int)((st >> 6) & 1);
    const int sync = (int)((st >> 7) & 1);
    ESP_LOGI("tc358743",
             "SYS_STATUS=0x%02x DDC5V=%d TMDS=%d PLL=%d SCDT=%d HDMI=%d HDCP=%d AVMUTE=%d SYNC=%d",
             st, ddc, tmds, pll, scdt, hdmi, hdcp, avmute, sync);

    if (tc358743_status_looks_like_i2c_garbage(st)) {
        ESP_LOGE("tc358743",
                 "SYS_STATUS 0x%02x is a stuck I2C fill (CONFCTL/VI often match; AVI INVALID_RESPONSE). "
                 "Reseat CSI ribbon (SDA/SCL), check RESETN, power-cycle. Not an HDMI lock.",
                 st);
        return;
    }

    if (tmds && !sync) {
        if (avmute || hdcp) {
            ESP_LOGW("tc358743",
                     "TMDS up but SYNC=0 (AVMUTE=%d HDCP=%d): source mute/HDCP possible",
                     avmute, hdcp);
        } else {
            ESP_LOGW("tc358743", "TMDS up but SYNC=0 — waiting for stable frame sync");
        }
    }
}
