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

void capture_mjpeg_run(capture_ctx_t *c)
{
    /*
     * timeout_ms: wall time allowed for one jpeg_encoder_process().
     * 1080p RGB888 → YUV420 encode often exceeds 120 ms on P4 (esp. first frames /
     * high quality / contested bus). IDF docs: value must exceed worst-case encode time.
     * Driver log text may say "decode" internally; this path is encode-only.
     */
    jpeg_encode_engine_cfg_t jcfg = {.intr_priority = 0, .timeout_ms = 500};
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jcfg, &s_jpeg_enc));

    const size_t jpeg_cap = (size_t)c->hres * (size_t)c->vres + 384u * 1024u;
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

    const unsigned bpp = capture_csi_bpp();
    int64_t hdmi_recover_cooldown_until_us = 0;

    while (1) {
        if (xSemaphoreTake(c->csi_done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
#if CONFIG_P4KVM_TC358743_ADV_DEBUG
            static uint32_t s_csi_timeout_logs;
#endif
            ESP_LOGW(CAPTURE_LOG_TAG, "csi frame wait timeout (dma_done_irqs=%lu)", (unsigned long)c->csi_dma_done_irqs);
            capture_debug_csi_timeout(c, bpp, c->frame_bytes);
#if CONFIG_P4KVM_TC358743_ADV_DEBUG
            tc358743_debug_stall_extras(c->tc);
            if ((s_csi_timeout_logs++ % 8u) == 0u) {
                tc358743_debug_status(c->tc);
                tc358743_debug_bridge(c->tc);
            }
#endif
            int64_t now = (int64_t)esp_timer_get_time();
            if (now >= hdmi_recover_cooldown_until_us) {
                (void)capture_hw_hdmi_recover(c);
                hdmi_recover_cooldown_until_us = now + (int64_t)8 * 1000000;
            }
            continue;
        }
        hdmi_recover_cooldown_until_us = 0;
        while (xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
            /* Drop stale completions; done_fb always points at the newest completed frame. */
        }

        void *src = (void *)c->done_fb;
        if (!src) {
            continue;
        }
        /* CSI DMA wrote frame → invalidate CPU cache before encoder reads RGB888. */
        ESP_ERROR_CHECK(esp_cache_msync(src, c->frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C));

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
        const uint32_t jpeg_in_bytes = (uint32_t)c->frame_bytes;
        uint32_t out_sz = 0;
        int back = -1;
        for (;;) {
            if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            back = jpeg_frame_pick_encode_slot();
            if (back >= 0) {
                xSemaphoreGive(g_jpeg_frame.mutex);
                break;
            }
            xSemaphoreGive(g_jpeg_frame.mutex);
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        esp_err_t er = jpeg_encoder_process(s_jpeg_enc, &enc, src, jpeg_in_bytes, g_jpeg_frame.jpeg_buf[back],
                                            (uint32_t)g_jpeg_frame.jpeg_cap, &out_sz);
        if (er != ESP_OK) {
            /* ESP_ERR_TIMEOUT = engine timeout_ms exceeded (not an HTTP/stream timeout). */
            ESP_LOGW(CAPTURE_LOG_TAG, "jpeg_encoder_process %s (timeout_ms=%d q=%u %ux%u in=%lu)",
                     esp_err_to_name(er), 500, (unsigned)q, (unsigned)c->hres, (unsigned)c->vres,
                     (unsigned long)jpeg_in_bytes);
            continue;
        }
        if (out_sz == 0 || out_sz > g_jpeg_frame.jpeg_cap) {
            ESP_LOGW(CAPTURE_LOG_TAG, "jpeg bad out_sz=%lu cap=%zu", (unsigned long)out_sz, g_jpeg_frame.jpeg_cap);
            continue;
        }
        if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) == pdTRUE) {
            g_jpeg_frame.jpeg_len[back] = (size_t)out_sz;
            g_jpeg_frame.front_idx = back;
            g_jpeg_frame.frame_seq++;
            xSemaphoreGive(g_jpeg_frame.mutex);
        }
        jpeg_frame_notify_new_frame();
    }
}
