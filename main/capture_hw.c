/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Progressive recover — do not HPD on the first glitch (that caused grey-after-start).
 */
#include "capture_priv.h"

#include <inttypes.h>
#include <string.h>

#include "p4kvm_hw_defaults.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_SPIRAM
#error "Enable CONFIG_SPIRAM"
#endif

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_ldo_regulator.h"
#include "hal/mipi_csi_types.h"
#include "hal/color_types.h"
#include "soc/clk_tree_defs.h"
#include "soc/isp_struct.h"
#include "soc/mipi_csi_bridge_struct.h"

#include "tc358743.h"
#include "tc358743_hdmi_debug.h"

static esp_cam_ctlr_handle_t s_cam;
static isp_proc_handle_t s_isp_bypass;
static bool s_cam_started;
static bool s_isp_created;
static const uint32_t s_csi_expected_dt = 0x24u;
static capture_ctx_t s_cap;
static uint32_t s_cam_h;
static uint32_t s_cam_v;
static int64_t s_last_hpd_us;
static int s_recover_streak;

static void tc358743_resetn_pulse(void)
{
#if CONFIG_P4KVM_TC358743_RST_GPIO >= 0
    const int rst = CONFIG_P4KVM_TC358743_RST_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << rst,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(rst, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(CAPTURE_LOG_TAG, "TC358743 RESETN released on GPIO %d", rst);
#else
    vTaskDelay(pdMS_TO_TICKS(500));
#endif
}

static bool tc358743_present(i2c_master_bus_handle_t bus)
{
    return i2c_master_probe(bus, TC358743_I2C_ADDR, 200) == ESP_OK;
}

static bool tc_locked(tc358743_t *tc)
{
    uint8_t st = 0;
    if (tc358743_sys_status(tc, &st) != ESP_OK) {
        return false;
    }
    return ((st & 0x02) != 0) && ((st & 0x08) != 0) && ((st & 0x80) != 0);
}

static void wait_hdmi_lock(tc358743_t *tc, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (tc_locked(tc)) {
            uint8_t st = 0;
            (void)tc358743_sys_status(tc, &st);
            ESP_LOGI(CAPTURE_LOG_TAG, "HDMI locked SYS=0x%02x after %" PRIu32 " ms", st, waited);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    ESP_LOGW(CAPTURE_LOG_TAG, "HDMI lock timeout");
    tc358743_debug_status(tc);
}

static bool read_hdmi_timing(tc358743_t *tc, uint32_t *hres, uint32_t *vres)
{
    uint16_t hact = 0, vact = 0;
    if (!tc || tc358743_get_detected_timing(tc, &hact, &vact) != ESP_OK) {
        return false;
    }
    if (hact < 320u || vact < 240u || hact > CAPTURE_MAX_H || vact > CAPTURE_MAX_V) {
        return false;
    }
    if (hact & 1u) {
        hact--;
    }
    *hres = hact;
    *vres = vact;
    return true;
}

static void drain_done_sem(capture_ctx_t *c)
{
    while (c->csi_done_sem && xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
    }
}

static void capture_configure_p4_csi_bridge(uint32_t hres, uint32_t vres)
{
    MIPI_CSI_BRIDGE.frame_cfg.hadr_num = hres;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num = vres;
    MIPI_CSI_BRIDGE.frame_cfg.has_hsync_e = 0u;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num_check = 0u;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_min = s_csi_expected_dt;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_max = s_csi_expected_dt;
    MIPI_CSI_BRIDGE.int_clr.val = 0x3fu;
}

void capture_debug_csi_timeout(capture_ctx_t *c, unsigned bpp, size_t fb_bytes)
{
    ESP_LOGW(CAPTURE_LOG_TAG, "CSI stall done=%" PRIu32 " get_new=%" PRIu32 " fb=%zu bpp=%u",
             c->csi_dma_done_irqs, c->csi_get_new_irqs, fb_bytes, bpp);
    if (c && c->tc) {
        tc358743_debug_status(c->tc);
        tc358743_debug_bridge(c->tc);
    }
}

unsigned capture_csi_bpp(void)
{
    return 24u;
}

void capture_fill_esp_cam_color_types(esp_cam_ctlr_csi_config_t *csi, esp_isp_processor_cfg_t *isp)
{
    csi->input_data_color_type = CAM_CTLR_COLOR_RGB888;
    csi->output_data_color_type = CAM_CTLR_COLOR_RGB888;
    isp->input_data_color_type = ISP_COLOR_RGB888;
    isp->output_data_color_type = ISP_COLOR_RGB888;
}

static bool IRAM_ATTR cam_on_get_new(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_get_new_irqs, 1);
    int i = c->ping_fb_idx % CAPTURE_FB_COUNT;
    trans->buffer = c->fb[i];
    trans->buflen = c->frame_bytes;
    c->ping_fb_idx = (c->ping_fb_idx + 1) % CAPTURE_FB_COUNT;
    return false;
}

static bool IRAM_ATTR cam_on_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_dma_done_irqs, 1);
    if (trans && trans->buffer) {
        c->done_fb = trans->buffer;
    }
    BaseType_t woken = pdFALSE;
    if (c->csi_done_sem) {
        (void)xSemaphoreGiveFromISR(c->csi_done_sem, &woken);
    }
    return woken;
}

static esp_err_t recreate_csi_at_size(capture_ctx_t *c, uint32_t hres, uint32_t vres)
{
    if (s_cam_started && s_cam) {
        (void)esp_cam_ctlr_stop(s_cam);
        s_cam_started = false;
    }
    if (s_cam) {
        (void)esp_cam_ctlr_disable(s_cam);
        (void)esp_cam_ctlr_del(s_cam);
        s_cam = NULL;
    }
    if (s_isp_created && s_isp_bypass) {
        (void)esp_isp_del_processor(s_isp_bypass);
        s_isp_bypass = NULL;
        s_isp_created = false;
    }

    size_t need = (size_t)hres * (size_t)vres * 3u;
    if (need > c->fb_alloc_bytes) {
        return ESP_ERR_NO_MEM;
    }
    c->hres = hres;
    c->vres = vres;
    c->frame_bytes = need;
    s_cam_h = hres;
    s_cam_v = vres;

    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = hres,
        .v_res = vres,
        .data_lane_num = 2,
        .lane_bit_rate_mbps = P4KVM_MIPI_LANE_MBPS,
        .queue_items = CAPTURE_FB_COUNT,
        .byte_swap_en = false,
        .bk_buffer_dis = true,
    };
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 80 * 1000000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .yuv_range = ISP_COLOR_RANGE_LIMIT,
        .yuv_std = ISP_YUV_CONV_STD_BT709,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = hres,
        .v_res = vres,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
        .intr_priority = 0,
        .flags = {.bypass_isp = true, .byte_swap_en = false},
    };
    capture_fill_esp_cam_color_types(&csi_cfg, &isp_cfg);

    esp_err_t er = esp_cam_new_csi_ctlr(&csi_cfg, &s_cam);
    if (er != ESP_OK) {
        return er;
    }
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = cam_on_get_new,
        .on_trans_finished = cam_on_done,
    };
    er = esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, c);
    if (er != ESP_OK) {
        return er;
    }
    er = esp_cam_ctlr_enable(s_cam);
    if (er != ESP_OK) {
        return er;
    }
    er = esp_isp_new_processor(&isp_cfg, &s_isp_bypass);
    if (er != ESP_OK) {
        return er;
    }
    s_isp_created = true;
    ISP.cntl.isp_en = 0;
    capture_configure_p4_csi_bridge(hres, vres);
    return ESP_OK;
}

static esp_err_t hard_hpd_recover(capture_ctx_t *c)
{
    int64_t now = (int64_t)esp_timer_get_time();
    if ((now - s_last_hpd_us) < (int64_t)10 * 1000000) {
        ESP_LOGW(CAPTURE_LOG_TAG, "HPD rate-limited — arm TX only");
        (void)tc358743_arm_csi_tx(c->tc);
        return ESP_ERR_INVALID_STATE;
    }
    s_last_hpd_us = now;

    ESP_LOGW(CAPTURE_LOG_TAG, "PIPE action: HPD recover (streak=%d)", s_recover_streak);

    if (s_cam_started && s_cam) {
        (void)esp_cam_ctlr_stop(s_cam);
        s_cam_started = false;
    }
    drain_done_sem(c);
    c->ping_fb_idx = 0;
    c->done_fb = NULL;

    (void)tc358743_hdmi_hotplug_reset(c->tc);
    wait_hdmi_lock(c->tc, 5000);
    if (!tc_locked(c->tc)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t h = c->hres, v = c->vres;
    (void)read_hdmi_timing(c->tc, &h, &v);
    if (h != s_cam_h || v != s_cam_v) {
        esp_err_t er = recreate_csi_at_size(c, h, v);
        if (er != ESP_OK) {
            return er;
        }
    } else {
        c->frame_bytes = (size_t)h * (size_t)v * 3u;
        capture_configure_p4_csi_bridge(h, v);
    }

    ISP.cntl.isp_en = 0;
    MIPI_CSI_BRIDGE.int_clr.val = 0x3fu;
    esp_err_t er = esp_cam_ctlr_start(s_cam);
    if (er != ESP_OK) {
        return er;
    }
    s_cam_started = true;
    (void)tc358743_arm_csi_tx(c->tc);
    return ESP_OK;
}

void capture_hw_recover_streak_reset(void)
{
    if (s_recover_streak != 0) {
        ESP_LOGI(CAPTURE_LOG_TAG, "recover streak cleared (was %d)", s_recover_streak);
    }
    s_recover_streak = 0;
}

esp_err_t capture_hw_hdmi_recover(capture_ctx_t *c)
{
    ESP_RETURN_ON_FALSE(c && c->tc, ESP_ERR_INVALID_ARG, CAPTURE_LOG_TAG, "ctx");

    s_recover_streak++;

    if (s_recover_streak == 1) {
        ESP_LOGW(CAPTURE_LOG_TAG, "PIPE action: soft arm_csi_tx (streak=1)");
        return tc358743_arm_csi_tx(c->tc);
    }
    if (s_recover_streak == 2) {
        ESP_LOGW(CAPTURE_LOG_TAG, "PIPE action: soft_kick (streak=2)");
        (void)tc358743_soft_kick(c->tc);
        return tc358743_arm_csi_tx(c->tc);
    }

    /* 3+ : full HPD */
    return hard_hpd_recover(c);
}

capture_ctx_t *capture_hw_init_start(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = P4KVM_MIPI_LDO_CHAN_ID,
        .voltage_mv = P4KVM_MIPI_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    tc358743_resetn_pulse();

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = P4KVM_TC358743_I2C_SDA_GPIO,
        .scl_io_num = P4KVM_TC358743_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = true},
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));

    if (!tc358743_present(i2c_bus)) {
        ESP_LOGE(CAPTURE_LOG_TAG, "TC358743 not found");
        vTaskDelete(NULL);
        return NULL;
    }

    ESP_ERROR_CHECK(tc358743_probe(i2c_bus, NULL, &s_cap.tc));
    ESP_ERROR_CHECK(tc358743_init_streaming(s_cap.tc));

    s_cap.hres = 720;
    s_cap.vres = 480;
    s_cap.frame_bytes = (size_t)s_cap.hres * (size_t)s_cap.vres * 3u;
    s_cap.fb_alloc_bytes = (size_t)CAPTURE_MAX_H * (size_t)CAPTURE_MAX_V * 3u;
    s_cam_h = s_cap.hres;
    s_cam_v = s_cap.vres;

    size_t align = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align));
    uint8_t *blk = heap_caps_aligned_calloc(align, CAPTURE_FB_COUNT, s_cap.fb_alloc_bytes,
                                            MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blk) {
        ESP_LOGE(CAPTURE_LOG_TAG, "FB alloc fail");
        vTaskDelete(NULL);
        return NULL;
    }
    for (int i = 0; i < CAPTURE_FB_COUNT; i++) {
        s_cap.fb[i] = blk + ((size_t)i * s_cap.fb_alloc_bytes);
    }
    s_cap.ping_fb_idx = 0;
    s_cap.done_fb = NULL;
    s_cap.done_fb_idx = -1;
    s_cap.csi_dma_done_irqs = 0;
    s_cap.csi_get_new_irqs = 0;
    s_cap.csi_done_sem = xSemaphoreCreateCounting(64, 0);
    if (!s_cap.csi_done_sem) {
        vTaskDelete(NULL);
        return NULL;
    }

    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = s_cap.hres,
        .v_res = s_cap.vres,
        .data_lane_num = 2,
        .lane_bit_rate_mbps = P4KVM_MIPI_LANE_MBPS,
        .queue_items = CAPTURE_FB_COUNT,
        .byte_swap_en = false,
        .bk_buffer_dis = true,
    };
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 80 * 1000000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .yuv_range = ISP_COLOR_RANGE_LIMIT,
        .yuv_std = ISP_YUV_CONV_STD_BT709,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = s_cap.hres,
        .v_res = s_cap.vres,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
        .intr_priority = 0,
        .flags = {.bypass_isp = true, .byte_swap_en = false},
    };
    capture_fill_esp_cam_color_types(&csi_cfg, &isp_cfg);

    ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam));
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = cam_on_get_new,
        .on_trans_finished = cam_on_done,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, &s_cap));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(s_cam));
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_cfg, &s_isp_bypass));
    s_isp_created = true;
    ISP.cntl.isp_en = 0;
    capture_configure_p4_csi_bridge(s_cap.hres, s_cap.vres);

    ESP_ERROR_CHECK(tc358743_enable_hdmi_output(s_cap.tc));
    wait_hdmi_lock(s_cap.tc, 5000);

    uint32_t h = s_cap.hres, v = s_cap.vres;
    if (read_hdmi_timing(s_cap.tc, &h, &v) && (h != s_cap.hres || v != s_cap.vres)) {
        ESP_LOGI(CAPTURE_LOG_TAG, "timing %ux%u → recreate CSI", (unsigned)h, (unsigned)v);
        ESP_ERROR_CHECK(recreate_csi_at_size(&s_cap, h, v));
    }

    capture_configure_p4_csi_bridge(s_cap.hres, s_cap.vres);
    ESP_ERROR_CHECK(esp_cam_ctlr_start(s_cam));
    s_cam_started = true;
    (void)tc358743_arm_csi_tx(s_cap.tc);
    s_recover_streak = 0;

    ESP_LOGI(CAPTURE_LOG_TAG, "CSI started %ux%u", (unsigned)s_cap.hres, (unsigned)s_cap.vres);
    return &s_cap;
}
