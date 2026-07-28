/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * MJPEG for <img src="/stream">.
 * Grey = worker died or multipart stalled with no JPEG.
 * Keepalive only when encode stalls — never flood the browser.
 */
#include "http_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "i2s_audio.h"
#include "jpeg_frame.h"

static const char *TAG = "babelbus";

#define STREAM_WORKER_STACK (12 * 1024)
#define STREAM_WORKER_PRIO (tskIDLE_PRIORITY + 7)
#define AUDIO_WORKER_STACK (8 * 1024)
#define AUDIO_CHUNK 2048
#define STREAM_COPY_CAP (768 * 1024)
#define STREAM_SEND_RETRIES 4
/* Only re-send last JPEG when encode has been quiet this long (not a flood). */
#define STREAM_KEEPALIVE_MS 500

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, len);
}

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
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return true;
    }
    return false;
}

static esp_err_t stream_send_parts(httpd_req_t *req, const char *hdr, int hl, const uint8_t *body,
                                   size_t body_len)
{
    for (int attempt = 0; attempt < STREAM_SEND_RETRIES; attempt++) {
        if (stream_peer_disconnected(req)) {
            return ESP_FAIL;
        }
        esp_err_t se = httpd_resp_send_chunk(req, hdr, hl);
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, (const char *)body, body_len);
        }
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        if (se == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20 + attempt * 25));
    }
    return ESP_FAIL;
}

static void stream_worker_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char hdr[96];

    uint8_t *copy = heap_caps_malloc(STREAM_COPY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *last_good = heap_caps_malloc(STREAM_COPY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = heap_caps_malloc(STREAM_COPY_CAP, MALLOC_CAP_DEFAULT);
    }
    if (!last_good) {
        last_good = heap_caps_malloc(STREAM_COPY_CAP, MALLOC_CAP_DEFAULT);
    }
    if (!copy) {
        ESP_LOGE(TAG, "stream copy buf alloc failed");
        free(last_good);
        (void)httpd_resp_sendstr_chunk(req, NULL);
        (void)httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    jpeg_frame_stream_enter();

    uint32_t last_seq = 0;
    size_t last_good_len = 0;
    TickType_t last_send_tick = xTaskGetTickCount();

    while (1) {
        if (stream_peer_disconnected(req)) {
            break;
        }

        bool stop = false;
        bool want_keepalive = false;

        while (g_jpeg_frame.frame_seq == last_seq) {
            if (stream_peer_disconnected(req)) {
                stop = true;
                break;
            }
            if (last_good_len > 0 &&
                (xTaskGetTickCount() - last_send_tick) >= pdMS_TO_TICKS(STREAM_KEEPALIVE_MS)) {
                want_keepalive = true;
                break;
            }
            if (g_jpeg_frame.frame_ready_sem) {
                (void)xSemaphoreTake(g_jpeg_frame.frame_ready_sem, pdMS_TO_TICKS(80));
                while (xSemaphoreTake(g_jpeg_frame.frame_ready_sem, 0) == pdTRUE) {
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (g_jpeg_frame.frame_seq != last_seq) {
                break;
            }
        }
        if (stop) {
            break;
        }

        const uint8_t *send_ptr = NULL;
        size_t send_len = 0;
        uint32_t seq_snap = last_seq;

        if (!want_keepalive && g_jpeg_frame.frame_seq != last_seq) {
            if (xSemaphoreTake(g_jpeg_frame.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int f = g_jpeg_frame.front_idx;
                seq_snap = g_jpeg_frame.frame_seq;
                if (f >= 0 && f < JPEG_SLOT_COUNT && g_jpeg_frame.jpeg_buf[f]) {
                    size_t n = g_jpeg_frame.jpeg_len[f];
                    if (n > 0 && n <= g_jpeg_frame.jpeg_cap && n <= STREAM_COPY_CAP) {
                        memcpy(copy, g_jpeg_frame.jpeg_buf[f], n);
                        send_ptr = copy;
                        send_len = n;
                        if (last_good) {
                            memcpy(last_good, copy, n);
                            last_good_len = n;
                        } else {
                            last_good_len = n;
                        }
                    } else if (n > STREAM_COPY_CAP) {
                        ESP_LOGW(TAG, "JPEG %zu > %u — lower quality in ⚙",
                                 n, (unsigned)STREAM_COPY_CAP);
                    }
                }
                xSemaphoreGive(g_jpeg_frame.mutex);
            }
        }

        if (send_len == 0 && last_good_len > 0 && last_good) {
            send_ptr = last_good;
            send_len = last_good_len;
        } else if (send_len == 0 && last_good_len > 0) {
            send_ptr = copy;
            send_len = last_good_len;
        }

        if (send_len == 0 || !send_ptr) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        int hl = snprintf(hdr, sizeof(hdr),
                          "--frame\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          send_len);
        if (hl <= 0 || hl >= (int)sizeof(hdr)) {
            continue;
        }

        if (stream_send_parts(req, hdr, hl, send_ptr, send_len) != ESP_OK) {
            break;
        }

        if (!want_keepalive) {
            last_seq = seq_snap;
        }
        last_send_tick = xTaskGetTickCount();
    }

    jpeg_frame_stream_leave();
    free(copy);
    free(last_good);
    (void)httpd_resp_sendstr_chunk(req, NULL);
    (void)httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    if (!g_jpeg_frame.jpeg_buf[0] || !g_jpeg_frame.jpeg_buf[1] || !g_jpeg_frame.jpeg_buf[2] ||
        !g_jpeg_frame.xmit_mutex || !g_jpeg_frame.frame_ready_sem) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera starting");
    }

    if (jpeg_frame_stream_client_count() >= 1) {
        ESP_LOGW(TAG, "/stream: reclaiming previous client");
        jpeg_frame_stream_force_clear();
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    if (res != ESP_OK) {
        return res;
    }

    httpd_req_t *async_req = NULL;
    res = httpd_req_async_handler_begin(req, &async_req);
    if (res != ESP_OK) {
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

static void audio_worker_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    uint8_t buf[AUDIO_CHUNK];
    while (1) {
        if (stream_peer_disconnected(req)) {
            break;
        }
        size_t n = i2s_audio_read(buf, sizeof(buf), 200);
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            break;
        }
    }
    (void)httpd_resp_sendstr_chunk(req, NULL);
    (void)httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t audio_get(httpd_req_t *req)
{
    if (!i2s_audio_ready()) {
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "audio disabled or not ready");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-BabelBus-Audio", "s16le,2ch,48000");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "audio busy");
    }
    if (xTaskCreate(audio_worker_task, "bb_audio", AUDIO_WORKER_STACK, async_req, tskIDLE_PRIORITY + 4, NULL) !=
        pdPASS) {
        httpd_req_async_handler_complete(async_req);
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "audio task");
    }
    return ESP_OK;
}

httpd_handle_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 16 * 1024;
    cfg.task_priority = tskIDLE_PRIORITY + 6;
    cfg.send_wait_timeout = 10;
    cfg.recv_wait_timeout = 5;
    cfg.keep_alive_enable = false;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;
    cfg.max_uri_handlers = 12;

    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start");
        return NULL;
    }

    httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_register_uri_handler(h, &u_root);
    httpd_uri_t u_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
    httpd_register_uri_handler(h, &u_stream);
    httpd_uri_t u_jpeg_q = {.uri = "/jpeg-quality", .method = HTTP_GET, .handler = jpeg_quality_get};
    httpd_register_uri_handler(h, &u_jpeg_q);
    httpd_uri_t u_audio = {.uri = "/audio", .method = HTTP_GET, .handler = audio_get};
    httpd_register_uri_handler(h, &u_audio);
    return h;
}
