/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define JPEG_SLOT_COUNT 3

typedef struct {
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t xmit_mutex;
    volatile uint32_t frame_seq;
    SemaphoreHandle_t frame_ready_sem;
    uint8_t *jpeg_buf[JPEG_SLOT_COUNT];
    size_t jpeg_len[JPEG_SLOT_COUNT];
    uint8_t slot_ref[JPEG_SLOT_COUNT];
    int front_idx;
    size_t jpeg_cap;
    volatile uint8_t jpeg_quality;
} jpeg_frame_slot_t;

extern jpeg_frame_slot_t g_jpeg_frame;

void jpeg_frame_stream_enter(void);
void jpeg_frame_stream_leave(void);
int jpeg_frame_stream_client_count(void);
/** Force client count to 0 — stale browser tab after refresh. */
void jpeg_frame_stream_force_clear(void);
void jpeg_frame_notify_new_frame(void);
int jpeg_frame_pick_encode_slot(void);
void jpeg_quality_load_from_nvs(void);
esp_err_t jpeg_quality_save_to_nvs(uint8_t q);
