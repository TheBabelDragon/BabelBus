/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tc358743.h"

#include <inttypes.h>

#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "tc358743";

#define CONFCTL 0x0004
#define SYSCTL 0x0002
#define PLLCTL0 0x0020
#define PLLCTL1 0x0022
#define CLW_CNTRL 0x0140
#define D0W_CNTRL 0x0144
#define D1W_CNTRL 0x0148
#define HSTXVREGEN 0x0234
#define TXOPTIONCNTRL 0x0238
#define CSI_STATUS 0x0410
#define CSI_CONTROL 0x040c
#define CSI_INT 0x0414
#define CSI_ERR 0x044c
#define CSI_START 0x0518
#define VI_MUTE 0x857f
#define VOUT_SET2 0x8573

struct tc358743 {
    i2c_master_dev_handle_t i2c;
};

static esp_err_t diag_read(tc358743_t *d, uint16_t reg, void *data, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    return i2c_master_transmit_receive(d->i2c, addr, 2, data, len, -1);
}

static uint16_t d16(tc358743_t *d, uint16_t r)
{
    uint8_t b[2] = {0};
    (void)diag_read(d, r, b, 2);
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t d32(tc358743_t *d, uint16_t r)
{
    uint8_t b[4] = {0};
    (void)diag_read(d, r, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint8_t d8(tc358743_t *d, uint16_t r)
{
    uint8_t v = 0;
    (void)diag_read(d, r, &v, 1);
    return v;
}

void tc358743_debug_bridge(tc358743_t *d)
{
    if (!d) {
        return;
    }
    uint16_t conf = d16(d, CONFCTL);
    uint16_t csi = d16(d, CSI_STATUS);
    uint16_t csi_ctl = d16(d, CSI_CONTROL);
    uint16_t csi_int = d16(d, CSI_INT);
    uint32_t csi_err = d32(d, CSI_ERR);
    uint32_t txopt = d32(d, TXOPTIONCNTRL);
    uint32_t hstx = d32(d, HSTXVREGEN);
    uint32_t clw = d32(d, CLW_CNTRL);
    uint32_t d0w = d32(d, D0W_CNTRL);
    uint32_t d1w = d32(d, D1W_CNTRL);
    uint32_t csi_start = d32(d, CSI_START);
    uint16_t pll0 = d16(d, PLLCTL0);
    uint16_t pll1 = d16(d, PLLCTL1);
    uint16_t sysctl = d16(d, SYSCTL);
    uint8_t vimute = d8(d, VI_MUTE);
    uint8_t vout2 = d8(d, VOUT_SET2);

    unsigned hlt = (unsigned)(csi & 1u);
    unsigned rxact = (unsigned)((csi >> 8) & 1u);
    unsigned txact = (unsigned)((csi >> 9) & 1u);
    unsigned wsync = (unsigned)((csi >> 10) & 1u);
    unsigned vbufen = (unsigned)(conf & 1u);
    unsigned abufen = (unsigned)((conf >> 1) & 1u);
    unsigned yfmt = (unsigned)((conf >> 6) & 3u);

    ESP_LOGW(TAG,
             "CSI TX: CONFCTL=0x%04x VBUFEN=%u ABUFEN=%u YFmt=%u (0=RGB 3=UYVY) "
             "VI_MUTE=0x%02x VOUT2=0x%02x SYSCTL=0x%04x",
             conf, vbufen, abufen, yfmt, vimute, vout2, sysctl);
    ESP_LOGW(TAG,
             "CSI_STATUS=0x%04x Hlt=%u RxAct=%u TxAct=%u WSync=%u  "
             "CSI_CONTROL=0x%04x CSI_INT=0x%04x CSI_ERR=0x%08" PRIx32,
             csi, hlt, rxact, txact, wsync, csi_ctl, csi_int, csi_err);
    ESP_LOGW(TAG,
             "PLL0=0x%04x PLL1=0x%04x (EN=%u CKEN=%u) TXOPT=0x%08" PRIx32 " CONTCLK=%u "
             "HSTXVREGEN=0x%08" PRIx32 " CSI_START=0x%08" PRIx32,
             pll0, pll1, (unsigned)(pll1 & 1u), (unsigned)((pll1 >> 4) & 1u), txopt,
             (unsigned)(txopt & 1u), hstx, csi_start);
    ESP_LOGW(TAG,
             "lanes CLW=0x%08" PRIx32 " D0W=0x%08" PRIx32 " D1W=0x%08" PRIx32
             " (bit0=1 means DISABLED)",
             clw, d0w, d1w);

    if (!vbufen) {
        ESP_LOGE(TAG, "VBUFEN=0 — video FIFO off");
    }
    /* TxAct is pulsed during active video only; reading 0 during blanking is normal. */
    if (!txact && !wsync && vbufen) {
        ESP_LOGD(TAG, "TxAct=0 sample (blanking window OK if dma_done climbing)");
    }
}

void tc358743_debug_stall_extras(tc358743_t *d)
{
    tc358743_debug_bridge(d);
}
