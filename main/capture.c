/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "capture_priv.h"

static void camera_task(void *arg)
{
    (void)arg;
    capture_ctx_t *ctx = capture_hw_init_start();
    if (!ctx) {
        return;
    }
    capture_mjpeg_run(ctx);
}

void capture_start(void)
{
    const uint32_t cam_stack = 10240;
    xTaskCreatePinnedToCore(camera_task, "cam", cam_stack, NULL, 5, NULL, 0);
}
