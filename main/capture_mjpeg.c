/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture_priv.h"

#include "sdkconfig.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "jpeg_frame.h"
#include "tc358743_hdmi_debug.h"

static jpeg_encoder_handle_t s_jpeg_enc;

/* Soft cap — only drop when encode cannot keep up (no free slot). */
#ifndef BABELBUS_TARGET_FPS
#define BABELBUS_TARGET_FPS 30
#endif

void capture_mjpeg_run(capture_ctx_t *c)
{
    jpeg_encode_engine_cfg_t jcfg = {.intr_priority = 0, .timeout_ms = 200};
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jcfg, &s_jpeg_enc));

    if (g_jpeg_frame.jpeg_quality < 1u || g_jpeg_frame.jpeg_quality > 100u) {
        g_jpeg_frame.jpeg_quality = 50u;
    }
    jpeg_quality_load_from_nvs();

    const size_t jpeg_cap = (size_t)CAPTURE_MAX_H * (size_t)CAPTURE_MAX_V + 384u * 1024u;
    jpeg_encode_memory_alloc_cfg_t jmem = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    size_t smallest_alloc = SIZE_MAX;
    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        size_t ja = 0;
        g_jpeg_frame.jpeg_buf[i] = jpeg_alloc_encoder_mem(jpeg_cap, &jmem, &ja);
        if (!g_jpeg_frame.jpeg_buf[i]) {
            ESP_LOGE(CAPTURE_LOG_TAG, "jpeg_alloc_encoder_mem failed slot %d", i);
            vTaskDelete(NULL);
            return;
        }
        if (ja < smallest_alloc) {
            smallest_alloc = ja;
        }
    }
    g_jpeg_frame.jpeg_cap = smallest_alloc;
    g_jpeg_frame.front_idx = -1;
    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        g_jpeg_frame.jpeg_len[i] = 0;
        g_jpeg_frame.slot_ref[i] = 0;
    }
    g_jpeg_frame.frame_seq = 0;
    g_jpeg_frame.mutex = xSemaphoreCreateMutex();
    g_jpeg_frame.xmit_mutex = xSemaphoreCreateMutex();
    g_jpeg_frame.frame_ready_sem = xSemaphoreCreateCounting(128, 0);
    if (!g_jpeg_frame.mutex || !g_jpeg_frame.xmit_mutex || !g_jpeg_frame.frame_ready_sem) {
        ESP_LOGE(CAPTURE_LOG_TAG, "JPEG mutex alloc failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(CAPTURE_LOG_TAG, "JPEG path ready q=%u target_fps=%d",
             (unsigned)g_jpeg_frame.jpeg_quality, BABELBUS_TARGET_FPS);

    const unsigned bpp = capture_csi_bpp();
    int64_t hdmi_recover_cooldown_until_us = 0;
    uint32_t last_logged_done = 0;
    int64_t last_encode_us = 0;
    const int64_t min_encode_interval_us = (int64_t)(1000000 / BABELBUS_TARGET_FPS);

    while (1) {
        if (xSemaphoreTake(c->csi_done_sem, pdMS_TO_TICKS(1500)) != pdTRUE) {
            ESP_LOGW(CAPTURE_LOG_TAG, "csi frame wait timeout (dma_done_irqs=%lu)",
                     (unsigned long)c->csi_dma_done_irqs);
            capture_debug_csi_timeout(c, bpp, c->frame_bytes);
            int64_t now = (int64_t)esp_timer_get_time();
            if (now >= hdmi_recover_cooldown_until_us) {
                (void)capture_hw_hdmi_recover(c);
                hdmi_recover_cooldown_until_us = now + (int64_t)2 * 1000000;
            }
            continue;
        }
        hdmi_recover_cooldown_until_us = 0;
        while (xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
        }

        void *src = (void *)c->done_fb;
        if (!src) {
            continue;
        }

        if (jpeg_frame_stream_client_count() <= 0) {
            continue;
        }

        int64_t now = (int64_t)esp_timer_get_time();
        if ((now - last_encode_us) < min_encode_interval_us) {
            continue;
        }

        /* Pick slot first — if busy, drop without cache sync cost. */
        int back = -1;
        if (xSemaphoreTake(g_jpeg_frame.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            back = jpeg_frame_pick_encode_slot();
            xSemaphoreGive(g_jpeg_frame.mutex);
        }
        if (back < 0) {
            continue;
        }

        (void)esp_cache_msync(src, c->frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        if (c->csi_dma_done_irqs == 1 ||
            (c->csi_dma_done_irqs - last_logged_done) >= 300u) {
            const uint8_t *p = (const uint8_t *)src;
            uint32_t sum = 0;
            size_t mid = c->frame_bytes / 2u;
            for (size_t i = 0; i < 128 && i < c->frame_bytes; i++) {
                sum += p[i];
            }
            for (size_t i = 0; i < 128 && (mid + i) < c->frame_bytes; i++) {
                sum += p[mid + i];
            }
            ESP_LOGI(CAPTURE_LOG_TAG, "frame ok done=%lu jpeg_seq=%lu %ux%u pixsum256=%lu",
                     (unsigned long)c->csi_dma_done_irqs, (unsigned long)g_jpeg_frame.frame_seq,
                     (unsigned)c->hres, (unsigned)c->vres, (unsigned long)sum);
            last_logged_done = c->csi_dma_done_irqs;
        }

        uint8_t q = g_jpeg_frame.jpeg_quality;
        if (q < 1u) {
            q = 1u;
        } else if (q > 100u) {
            q = 100u;
        }
        jpeg_encode_cfg_t enc = {.width = c->hres,
                                 .height = c->vres,
                                 .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
                                 .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
                                 .image_quality = q};
        uint32_t out_sz = 0;
        esp_err_t er = jpeg_encoder_process(s_jpeg_enc, &enc, src, (uint32_t)c->frame_bytes,
                                            g_jpeg_frame.jpeg_buf[back], (uint32_t)g_jpeg_frame.jpeg_cap,
                                            &out_sz);
        if (er != ESP_OK || out_sz == 0 || out_sz > g_jpeg_frame.jpeg_cap) {
            if (er != ESP_OK) {
                ESP_LOGW(CAPTURE_LOG_TAG, "jpeg_encoder_process %s", esp_err_to_name(er));
            }
            continue;
        }
        if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) == pdTRUE) {
            g_jpeg_frame.jpeg_len[back] = (size_t)out_sz;
            g_jpeg_frame.front_idx = back;
            g_jpeg_frame.frame_seq++;
            xSemaphoreGive(g_jpeg_frame.mutex);
        }
        jpeg_frame_notify_new_frame();
        last_encode_us = now;
    }
}
