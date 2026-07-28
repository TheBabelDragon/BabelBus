/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "tc358743.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CAPTURE_LOG_TAG "babelbus"

#define CAPTURE_FB_COUNT 3

#define CAPTURE_MAX_H 1920u
#define CAPTURE_MAX_V 1080u

typedef struct {
    uint32_t hres;
    uint32_t vres;
    size_t frame_bytes;
    size_t fb_alloc_bytes;
    void *fb[CAPTURE_FB_COUNT];
    void *volatile done_fb;
    volatile int done_fb_idx;
    volatile int ping_fb_idx;
    SemaphoreHandle_t csi_done_sem;
    tc358743_t *tc;
    volatile uint32_t csi_dma_done_irqs;
    volatile uint32_t csi_get_new_irqs;
} capture_ctx_t;

capture_ctx_t *capture_hw_init_start(void);

/** Progressive recover: soft TX first, HPD only after repeated stalls. */
esp_err_t capture_hw_hdmi_recover(capture_ctx_t *c);

void capture_hw_recover_streak_reset(void);

void capture_debug_csi_timeout(capture_ctx_t *c, unsigned bpp, size_t fb_bytes);

unsigned capture_csi_bpp(void);

void capture_fill_esp_cam_color_types(esp_cam_ctlr_csi_config_t *csi, esp_isp_processor_cfg_t *isp);

void capture_mjpeg_run(capture_ctx_t *c);
