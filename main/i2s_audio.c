/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i2s_audio.h"

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_log.h"

#if CONFIG_BABELBUS_I2S_AUDIO_ENABLE

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_audio";
static i2s_chan_handle_t s_rx;
static bool s_ready;

esp_err_t i2s_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_BABELBUS_I2S_BCLK_GPIO,
                .ws = CONFIG_BABELBUS_I2S_WS_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din = CONFIG_BABELBUS_I2S_DIN_GPIO,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
    };
    /* Bridge is master: slave mode still needs a nominal rate for slot timing helpers. */
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "init_std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");

    s_ready = true;
    ESP_LOGI(TAG, "I2S RX slave: BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d (16-bit stereo ~48k)",
             CONFIG_BABELBUS_I2S_BCLK_GPIO, CONFIG_BABELBUS_I2S_WS_GPIO, CONFIG_BABELBUS_I2S_DIN_GPIO);
    return ESP_OK;
}

bool i2s_audio_ready(void)
{
    return s_ready;
}

size_t i2s_audio_read(void *dst, size_t max_bytes, uint32_t timeout_ms)
{
    if (!s_ready || !dst || max_bytes == 0) {
        return 0;
    }
    size_t n = 0;
    esp_err_t err = i2s_channel_read(s_rx, dst, max_bytes, &n, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGD(TAG, "read %s", esp_err_to_name(err));
        return 0;
    }
    return n;
}

#else /* !CONFIG_BABELBUS_I2S_AUDIO_ENABLE */

esp_err_t i2s_audio_init(void)
{
    return ESP_OK;
}

bool i2s_audio_ready(void)
{
    return false;
}

size_t i2s_audio_read(void *dst, size_t max_bytes, uint32_t timeout_ms)
{
    (void)dst;
    (void)max_bytes;
    (void)timeout_ms;
    return 0;
}

#endif
