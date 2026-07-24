/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "jpeg_frame.h"

static const char *TAG = "babelbus";

static void stream_release_slot_ref(int slot)
{
    if (slot < 0 || slot >= JPEG_SLOT_COUNT) {
        return;
    }
    if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) == pdTRUE) {
        if (g_jpeg_frame.slot_ref[slot] > 0) {
            g_jpeg_frame.slot_ref[slot]--;
        }
        xSemaphoreGive(g_jpeg_frame.mutex);
    }
}

#define STREAM_WORKER_STACK (12 * 1024)
#define STREAM_WORKER_PRIO (tskIDLE_PRIORITY + 5)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const char favicon_ico_end[] asm("_binary_favicon_ico_end");

static esp_err_t root_get(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    const size_t len = (size_t)(favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, favicon_ico_start, len);
}

/** GET /jpeg-quality optional query `q=1..100` sets quality; response body is current quality (text/plain). */
static esp_err_t jpeg_quality_get(httpd_req_t *req)
{
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "q", val, sizeof(val)) == ESP_OK) {
            int q = atoi(val);
            if (q >= 1 && q <= 100) {
                g_jpeg_frame.jpeg_quality = (uint8_t)q;
                (void)jpeg_quality_save_to_nvs((uint8_t)q);
            }
        }
    }
    unsigned jq = (unsigned)g_jpeg_frame.jpeg_quality;
    if (jq < 1u) {
        jq = 1u;
    } else if (jq > 100u) {
        jq = 100u;
    }
    char resp[48];
    int n = snprintf(resp, sizeof(resp), "%u\n", jq);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "jpeg-quality");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, resp, (size_t)n);
}

static bool stream_peer_disconnected(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return true;
    }
    unsigned char b;
    int n = recv(fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
    if (n == 0) {
        return true;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return true;
    }
    return false;
}

static void stream_worker_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char hdr[96];

    jpeg_frame_stream_enter();
    uint32_t last_seq = g_jpeg_frame.frame_seq;

    while (1) {
        bool stop = false;
        while (g_jpeg_frame.frame_seq == last_seq) {
            if (!g_jpeg_frame.frame_ready_sem) {
                vTaskDelay(pdMS_TO_TICKS(5));
                if (stream_peer_disconnected(req)) {
                    stop = true;
                    break;
                }
                continue;
            }
            if (xSemaphoreTake(g_jpeg_frame.frame_ready_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
                if (stream_peer_disconnected(req)) {
                    stop = true;
                    break;
                }
                continue;
            }
            while (xSemaphoreTake(g_jpeg_frame.frame_ready_sem, 0) == pdTRUE) {
                /* coalesce */
            }
        }
        if (stop) {
            break;
        }

        if (xSemaphoreTake(g_jpeg_frame.xmit_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t copy_len = 0;
        uint32_t seq_snap = last_seq;
        int slot = -1;
        if (xSemaphoreTake(g_jpeg_frame.mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            xSemaphoreGive(g_jpeg_frame.xmit_mutex);
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        int f = g_jpeg_frame.front_idx;
        seq_snap = g_jpeg_frame.frame_seq;
        if (f >= 0 && f < JPEG_SLOT_COUNT && g_jpeg_frame.jpeg_buf[f]) {
            copy_len = g_jpeg_frame.jpeg_len[f];
            if (copy_len > 0 && copy_len <= g_jpeg_frame.jpeg_cap) {
                slot = f;
                g_jpeg_frame.slot_ref[slot]++;
            } else {
                copy_len = 0;
            }
        } else {
            copy_len = 0;
        }
        xSemaphoreGive(g_jpeg_frame.mutex);
        if (copy_len == 0 || slot < 0) {
            xSemaphoreGive(g_jpeg_frame.xmit_mutex);
            last_seq = seq_snap;
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int hl = snprintf(hdr, sizeof(hdr),
                          "--frame\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          copy_len);
        if (hl <= 0 || hl >= (int)sizeof(hdr)) {
            stream_release_slot_ref(slot);
            xSemaphoreGive(g_jpeg_frame.xmit_mutex);
            last_seq = seq_snap;
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_err_t se = httpd_resp_send_chunk(req, hdr, hl);
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, (const char *)g_jpeg_frame.jpeg_buf[slot], copy_len);
        }
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        stream_release_slot_ref(slot);
        xSemaphoreGive(g_jpeg_frame.xmit_mutex);

        if (se != ESP_OK) {
            ESP_LOGD(TAG, "stream end %s", esp_err_to_name(se));
            break;
        }
        last_seq = seq_snap;
    }
    jpeg_frame_stream_leave();
    httpd_resp_sendstr_chunk(req, NULL);
    if (httpd_req_async_handler_complete(req) != ESP_OK) {
        ESP_LOGW(TAG, "stream async complete failed");
    }
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    if (!g_jpeg_frame.jpeg_buf[0] || !g_jpeg_frame.jpeg_buf[1] || !g_jpeg_frame.jpeg_buf[2] ||
        !g_jpeg_frame.xmit_mutex || !g_jpeg_frame.frame_ready_sem) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera starting");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    if (res != ESP_OK) {
        return res;
    }

    httpd_req_t *async_req = NULL;
    res = httpd_req_async_handler_begin(req, &async_req);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "stream async begin: %s", esp_err_to_name(res));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream busy");
    }

    BaseType_t created =
        xTaskCreate(stream_worker_task, "bb_stream", STREAM_WORKER_STACK, async_req, STREAM_WORKER_PRIO, NULL);
    if (created != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "stream task");
    }
    return ESP_OK;
}

httpd_handle_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_req_hdr_len = 8192;
    cfg.server_port = 80;
    cfg.stack_size = 20 * 1024;
    cfg.task_priority = tskIDLE_PRIORITY + 6;
    cfg.send_wait_timeout = 30;
    cfg.keep_alive_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = false;
    cfg.max_open_sockets = 12;
    cfg.max_uri_handlers = 12;

    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start");
        return NULL;
    }

    httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_register_uri_handler(h, &u_root);
    httpd_uri_t u_favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
    httpd_register_uri_handler(h, &u_favicon);
    httpd_uri_t u_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
    httpd_register_uri_handler(h, &u_stream);
    httpd_uri_t u_jpeg_q = {.uri = "/jpeg-quality", .method = HTTP_GET, .handler = jpeg_quality_get};
    httpd_register_uri_handler(h, &u_jpeg_q);
    return h;
}
