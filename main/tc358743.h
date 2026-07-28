/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Toshiba TC358743 HDMI → MIPI CSI-2 bridge
 * Register sequence from drivers/media/i2c/tc358743.c in the Linux kernel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 7-bit I2C address (0x1e >> 1). */
#define TC358743_I2C_ADDR 0x0f

typedef struct tc358743 tc358743_t;

typedef struct {
    uint32_t refclk_hz;
    uint16_t pll_prd;
    uint16_t pll_fbd;
    uint16_t fifo_level;
    uint32_t lineinitcnt;
    uint32_t lptxtimecnt;
    uint32_t tclk_headercnt;
    uint32_t tclk_trailcnt;
    uint32_t ths_headercnt;
    uint32_t twakeup;
    uint32_t tclk_postcnt;
    uint32_t ths_trailcnt;
    uint32_t hstxvregcnt;
    uint8_t ddc5v_mode;
    unsigned lanes;
    bool enable_hdcp;
    uint8_t hdmi_detection_delay;
    bool hdmi_phy_auto_reset_tmds_detected;
    bool hdmi_phy_auto_reset_tmds_in_range;
    bool hdmi_phy_auto_reset_tmds_valid;
    bool hdmi_phy_auto_reset_hsync_out_of_range;
    bool hdmi_phy_auto_reset_vsync_out_of_range;
} tc358743_cfg_t;

esp_err_t tc358743_probe(i2c_master_bus_handle_t bus, const tc358743_cfg_t *cfg, tc358743_t **out_dev);

void tc358743_remove(tc358743_t *dev);

esp_err_t tc358743_init_streaming(tc358743_t *dev);

void tc358743_set_csi_uyvy422(tc358743_t *dev, bool uyvy422);

esp_err_t tc358743_enable_hdmi_output(tc358743_t *dev);

esp_err_t tc358743_hdmi_hotplug_reset(tc358743_t *dev);

esp_err_t tc358743_reapply_csi_path_after_hdmi(tc358743_t *dev);

/** Soft re-kick (color + VBUFEN + CSI_START) — no CTXRST / HPD. */
esp_err_t tc358743_soft_kick(tc358743_t *dev);

/** Full CSI TX rearm: CTXRST + lanes + continuous clock. No HPD. */
esp_err_t tc358743_csi_rearm(tc358743_t *dev);

void tc358743_debug_bridge(tc358743_t *dev);

void tc358743_debug_stall_extras(tc358743_t *dev);

void tc358743_log_link_state(tc358743_t *dev);

esp_err_t tc358743_set_streaming(tc358743_t *dev, bool on);

esp_err_t tc358743_read_chip_id(tc358743_t *dev, uint16_t *chip_id);

esp_err_t tc358743_sys_status(tc358743_t *dev, uint8_t *out_st);

/**
 * False if I2C returns uniform fill (SYS==VI==CONFCTL pattern) so TMDS bits
 * in SYS_STATUS must not be trusted for lock.
 */
bool tc358743_bus_ok(tc358743_t *dev);

esp_err_t tc358743_get_detected_timing(tc358743_t *dev, uint16_t *hact, uint16_t *vact);

esp_err_t tc358743_get_avi_color_format(tc358743_t *dev, uint8_t *out_y);

void tc358743_cfg_defaults_waveshare_pi(tc358743_cfg_t *c);

/** Low-level 8-bit register read (used by timing helpers). */
uint8_t tc358743_rd8(tc358743_t *dev, uint16_t reg);

#ifdef __cplusplus
}
#endif
