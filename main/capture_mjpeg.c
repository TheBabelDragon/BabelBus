/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hands-off while CSI delivers real pixels. Only recover on true death:
 *   - no DMA frames for several seconds, OR
 *   - sustained blank (0x80) fill while DMA still advances
 *
 * Do NOT periodically rewrite TC358743 registers — that is what turned a
 * healthy boot stream into permanent blank fill after ~10–15 s.
 */
#include "capture_priv.h"

#include "sdkconfig.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "jpeg_frame.h"
#include "tc358743.h"
#include "tc358743_hdmi_debug.h"

static jpeg_encoder_handle_t s_jpeg_enc;
static uint8_t *s_encode_scratch;
static size_t s_encode_scratch_bytes;

#ifndef BABELBUS_TARGET_FPS
#define BABELBUS_TARGET_FPS 20
#endif

/* ~2 s of blank at 20 fps before full HPD recover */
#define BLANK_STREAK_BEFORE_RECOVER 40

/** Mid-grey CSI fill: 256 samples of ~0x80 → sum ≈ 32768. */
static bool frame_looks_blank_fill(const uint8_t *p, size_t nbytes)
{
    if (!p || nbytes < 256) {
        return true;
    }
    uint32_t sum = 0;
    size_t mid = nbytes / 2u;
    for (size_t i = 0; i < 128; i++) {
        sum += p[i];
    }
    for (size_t i = 0; i < 128 && (mid + i) < nbytes; i++) {
        sum += p[mid + i];
    }
    return (sum > 32000u && sum < 33536u);
}

static void encode_scratch_ensure(size_t need)
{
    if (need == 0) {
        return;
    }
    if (s_encode_scratch && s_encode_scratch_bytes >= need) {
        return;
    }
    if (s_encode_scratch) {
        heap_caps_free(s_encode_scratch);
        s_encode_scratch = NULL;
        s_encode_scratch_bytes = 0;
    }
    s_encode_scratch = heap_caps_aligned_alloc(64, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_encode_scratch) {
        s_encode_scratch = heap_caps_aligned_alloc(64, need, MALLOC_CAP_DEFAULT);
    }
    if (s_encode_scratch) {
        s_encode_scratch_bytes = need;
        ESP_LOGI(CAPTURE_LOG_TAG, "encode scratch %zu bytes", need);
    } else {
        ESP_LOGW(CAPTURE_LOG_TAG, "encode scratch alloc failed (%zu) — inplace",
                 need);
    }
}

void capture_mjpeg_run(capture_ctx_t *c)
{
    jpeg_encode_engine_cfg_t jcfg = {.intr_priority = 0, .timeout_ms = 400};
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jcfg, &s_jpeg_enc));

    if (g_jpeg_frame.jpeg_quality < 1u || g_jpeg_frame.jpeg_quality > 100u) {
        g_jpeg_frame.jpeg_quality = (uint8_t)CONFIG_P4KVM_JPEG_QUALITY;
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

    encode_scratch_ensure(c->frame_bytes);

    ESP_LOGI(CAPTURE_LOG_TAG, "MJPEG ready q=%u fps=%d frame=%zu scratch=%s (hands-off bridge)",
             (unsigned)g_jpeg_frame.jpeg_quality, BABELBUS_TARGET_FPS, c->frame_bytes,
             s_encode_scratch ? "ok" : "inplace");

    const unsigned bpp = capture_csi_bpp();
    int64_t recover_cooldown_until_us = 0;
    uint32_t last_logged_done = 0;
    int64_t last_encode_us = 0;
    int blank_streak = 0;
    const int64_t min_encode_interval_us = (int64_t)(1000000 / BABELBUS_TARGET_FPS);

    while (1) {
        /* 4 s with zero DMA → real stall (not blank-fill, which still gives sem). */
        if (xSemaphoreTake(c->csi_done_sem, pdMS_TO_TICKS(4000)) != pdTRUE) {
            ESP_LOGW(CAPTURE_LOG_TAG, "csi frame wait timeout (dma_done_irqs=%lu)",
                     (unsigned long)c->csi_dma_done_irqs);
            capture_debug_csi_timeout(c, bpp, c->frame_bytes);
            int64_t now = (int64_t)esp_timer_get_time();
            if (now >= recover_cooldown_until_us) {
                ESP_LOGW(CAPTURE_LOG_TAG, "DMA dead — full HPD recover (boot path)");
                (void)capture_hw_hdmi_recover(c);
                recover_cooldown_until_us = now + (int64_t)6 * 1000000;
                encode_scratch_ensure(c->frame_bytes);
                blank_streak = 0;
            }
            continue;
        }
        while (xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
        }

        void *src = (void *)c->done_fb;
        size_t nbytes = c->frame_bytes;
        if (!src || nbytes == 0 || nbytes > c->fb_alloc_bytes) {
            continue;
        }

        int64_t now = (int64_t)esp_timer_get_time();

        /*
         * NO periodic tc358743_csi_keepalive here.
         * Clock-kick / VBUFEN rewrite while live was turning real video into
         * permanent 0x80 fill after ~10–15 s (dma_done kept climbing).
         */

        if (jpeg_frame_stream_client_count() <= 0) {
            blank_streak = 0;
            continue;
        }

        if ((now - last_encode_us) < min_encode_interval_us) {
            continue;
        }

        int back = -1;
        if (xSemaphoreTake(g_jpeg_frame.mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            back = jpeg_frame_pick_encode_slot();
            xSemaphoreGive(g_jpeg_frame.mutex);
        }
        if (back < 0) {
            continue;
        }

        (void)esp_cache_msync(src, nbytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        const uint8_t *enc_src;
        if (s_encode_scratch && s_encode_scratch_bytes >= nbytes) {
            memcpy(s_encode_scratch, src, nbytes);
            enc_src = s_encode_scratch;
        } else {
            enc_src = (const uint8_t *)src;
        }

        if (frame_looks_blank_fill(enc_src, nbytes)) {
            blank_streak++;
            if ((blank_streak % 30) == 1) {
                ESP_LOGW(CAPTURE_LOG_TAG, "blank CSI fill done=%lu streak=%d",
                         (unsigned long)c->csi_dma_done_irqs, blank_streak);
            }
            /*
             * Blank while DMA advances is the boot-only failure mode:
             * video path muted/fill, stall timeout never trips.
             * Full HPD is the only thing that restored real pixels.
             */
            if (blank_streak >= BLANK_STREAK_BEFORE_RECOVER &&
                now >= recover_cooldown_until_us) {
                ESP_LOGW(CAPTURE_LOG_TAG,
                         "sustained blank fill (streak=%d) — full HPD recover",
                         blank_streak);
                (void)capture_hw_hdmi_recover(c);
                recover_cooldown_until_us = now + (int64_t)6 * 1000000;
                encode_scratch_ensure(c->frame_bytes);
                blank_streak = 0;
            }
            continue;
        }
        blank_streak = 0;

        if (c->csi_dma_done_irqs == 1 ||
            (c->csi_dma_done_irqs - last_logged_done) >= 300u) {
            uint32_t sum = 0;
            size_t mid = nbytes / 2u;
            for (size_t i = 0; i < 128 && i < nbytes; i++) {
                sum += enc_src[i];
            }
            for (size_t i = 0; i < 128 && (mid + i) < nbytes; i++) {
                sum += enc_src[mid + i];
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

        jpeg_encode_cfg_t enc = {
            .width = c->hres,
            .height = c->vres,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = q,
            .pixel_reverse = false,
        };
        uint32_t out_sz = 0;
        esp_err_t er = jpeg_encoder_process(s_jpeg_enc, &enc, (uint8_t *)enc_src, (uint32_t)nbytes,
                                            g_jpeg_frame.jpeg_buf[back], (uint32_t)g_jpeg_frame.jpeg_cap,
                                            &out_sz);
        if (er != ESP_OK || out_sz == 0 || out_sz > g_jpeg_frame.jpeg_cap) {
            ESP_LOGW(CAPTURE_LOG_TAG, "jpeg encode fail %s out=%lu",
                     esp_err_to_name(er), (unsigned long)out_sz);
            continue;
        }
        if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) == pdTRUE) {
            g_jpeg_frame.jpeg_len[back] = (size_t)out_sz;
            g_jpeg_frame.front_idx = back;
            g_jpeg_frame.frame_seq++;
            xSemaphoreGive(g_jpeg_frame.mutex);
        }
        jpeg_frame_notify_new_frame();
        last_encode_us = (int64_t)esp_timer_get_time();
    }
}
