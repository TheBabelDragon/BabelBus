/* SPDX-FileCopyrightText: 2026 SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_log.h"
#include "i2c_guard.h"
#include "tc358743.h"

/**
 * Linux SYS_STATUS bits — only meaningful if the rest of the chip is not a
 * uniform fill. Real TC358743 never has SYS==VI1==VI2==CONFCTL_lo after init.
 */
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
    ESP_LOGI("tc358743",
             "SYS_STATUS=0x%02x DDC5V=%d TMDS=%d PLL=%d SCDT=%d HDMI=%d HDCP=%d AVMUTE=%d SYNC=%d",
             st, (int)(st & 1), (int)((st >> 1) & 1), (int)((st >> 2) & 1), (int)((st >> 3) & 1),
             (int)((st >> 4) & 1), (int)((st >> 5) & 1), (int)((st >> 6) & 1), (int)((st >> 7) & 1));
}
