/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * One-line pipeline health so a dead stream names the broken stage.
 */
#include "tc358743.h"

#include <inttypes.h>
#include <stdio.h>

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
#define SYS_STATUS 0x8520

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
}

void tc358743_debug_stall_extras(tc358743_t *d)
{
    tc358743_debug_bridge(d);
}

/**
 * One-line health for the whole path. Call every few seconds and on stall.
 *
 * dma_done / jpeg_seq = current counters
 * dma_delta / jpeg_delta = increase since last sample (pass 0 if unknown)
 * clients = active /stream count
 * stalled = true if CSI wait timed out
 */
void tc358743_log_pipeline(tc358743_t *d, uint32_t dma_done, uint32_t dma_delta,
                           uint32_t jpeg_seq, uint32_t jpeg_delta, int clients, bool stalled)
{
    uint8_t st = d ? d8(d, SYS_STATUS) : 0;
    uint16_t conf = d ? d16(d, CONFCTL) : 0;
    uint16_t csi = d ? d16(d, CSI_STATUS) : 0;
    uint32_t txopt = d ? d32(d, TXOPTIONCNTRL) : 0;
    uint8_t mute = d ? d8(d, VI_MUTE) : 0xff;

    unsigned tmds = (st >> 1) & 1u;
    unsigned scdt = (st >> 3) & 1u;
    unsigned hdmi = (st >> 4) & 1u;
    unsigned sync = (st >> 7) & 1u;
    unsigned vbufen = conf & 1u;
    unsigned txact = (csi >> 9) & 1u;
    unsigned contclk = txopt & 1u;

    const char *hdmi_s = (!tmds || !scdt || !sync) ? "NO_LOCK" : (hdmi ? "HDMI" : "DVI");

    /* Named failure stage — first match wins. */
    const char *verdict = "OK";
    if (!d) {
        verdict = "FAIL:no_bridge";
    } else if (!tmds || !scdt || !sync) {
        verdict = "FAIL:HDMI_unlock (no TMDS/SCDT/SYNC — cable/source)";
    } else if (!vbufen) {
        verdict = "FAIL:VBUFEN_off (video FIFO disabled)";
    } else if (mute == 0xc0u) {
        verdict = "FAIL:AUTO_MUTE (green/blank on DVI — need VI_MUTE=0)";
    } else if (mute != 0x00u && mute != 0xc0u) {
        verdict = "FAIL:VI_MUTE_set (video muted)";
    } else if (stalled && dma_delta == 0) {
        if (!txact) {
            verdict = "FAIL:CSI_TX (TxAct=0, DMA stuck — bridge not sending)";
        } else {
            verdict = "FAIL:DMA (TxAct=1 but no frames — P4 CSI RX)";
        }
    } else if (dma_delta > 0 && jpeg_delta == 0 && clients > 0) {
        verdict = "FAIL:ENCODE (DMA moves, JPEG seq stuck)";
    } else if (jpeg_delta > 0 && clients == 0) {
        verdict = "OK (encoding, no browser yet)";
    } else if (clients > 0 && jpeg_delta == 0 && dma_delta == 0) {
        verdict = "FAIL:NO_FRAMES (browser waiting, nothing flowing)";
    }

    ESP_LOGW("babelbus",
             "PIPE | %s | TxAct=%u VBUFEN=%u MUTE=0x%02x CONTCLK=%u | "
             "DMA=%" PRIu32 "(+%" PRIu32 ") JPEG=%" PRIu32 "(+%" PRIu32 ") clients=%d | %s",
             hdmi_s, txact, vbufen, mute, contclk, dma_done, dma_delta, jpeg_seq, jpeg_delta,
             clients, verdict);
}
