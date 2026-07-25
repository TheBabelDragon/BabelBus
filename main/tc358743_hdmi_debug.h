/* SPDX-FileCopyrightText: 2026 SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_log.h"
#include "tc358743.h"

/**
 * Linux tc358743_regs.h SYS_STATUS (0x8520):
 *  bit0 DDC5V  bit1 TMDS  bit2 PHY_PLL  bit3 PHY_SCDT
 *  bit4 HDMI   bit5 HDCP  bit6 AVMUTE  bit7 SYNC
 */
static inline void tc358743_debug_status(tc358743_t *dev)
{
    if (!dev) {
        return;
    }
    uint8_t st = 0;
    if (tc358743_sys_status(dev, &st) != ESP_OK) {
        return;
    }
    ESP_LOGI("tc358743",
             "SYS_STATUS=0x%02x DDC5V=%d TMDS=%d PLL=%d SCDT=%d HDMI=%d HDCP=%d AVMUTE=%d SYNC=%d",
             st, (int)(st & 1), (int)((st >> 1) & 1), (int)((st >> 2) & 1), (int)((st >> 3) & 1),
             (int)((st >> 4) & 1), (int)((st >> 5) & 1), (int)((st >> 6) & 1), (int)((st >> 7) & 1));
}
