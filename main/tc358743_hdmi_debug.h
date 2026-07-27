/* SPDX-FileCopyrightText: 2026 SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_log.h"
#include "tc358743.h"

static inline void tc358743_debug_status(tc358743_t *dev)
{
    if (!dev) {
        return;
    }
    uint8_t st = 0;
    if (tc358743_sys_status(dev, &st) != ESP_OK) {
        ESP_LOGE("tc358743", "SYS_STATUS read failed");
        return;
    }
    /*
     * SYS_STATUS bit map (Toshiba / Linux):
     *  0 DDC5V  1 TMDS  2 PLL  3 SCDT  4 HDMI  5 HDCP  6 AVMUTE  7 SYNC
     *
     * DDC5V is unreliable on many TC358743 adapter boards (floats high with
     * no cable). Treat TMDS+SCDT+SYNC as the real presence signal.
     */
    ESP_LOGI("tc358743",
             "SYS_STATUS=0x%02x DDC5V=%d(sticky?) TMDS=%d PLL=%d SCDT=%d HDMI=%d HDCP=%d AVMUTE=%d SYNC=%d",
             st, (int)(st & 1), (int)((st >> 1) & 1), (int)((st >> 2) & 1), (int)((st >> 3) & 1),
             (int)((st >> 4) & 1), (int)((st >> 5) & 1), (int)((st >> 6) & 1), (int)((st >> 7) & 1));
}
