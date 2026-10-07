/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * ElevenLabs text to speech: POST /v1/text-to-speech/{voice}/stream returns
 * the speech as a chunked MP3 stream, read here a couple of KB at a time into
 * a stream buffer that the voice session drains. When the session falls
 * behind, the buffer fills, the read stops and TCP holds the server back.
 *
 * A request is a job number: cancelling bumps it, and the task, which checks
 * after every read and every write to the buffer, cleans up and exits on its
 * own. Nothing here waits for it. A new request waits until the last task has
 * gone (busy), so at most one task ever writes the buffer.
 */

#include "muse_tts.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "muse_settings.h"

static const char *TAG = "muse_tts";

#define TTS_URL "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=mp3_22050_32"
#define TTS_STACK (16 * 1024)          /* in PSRAM; the TLS buffers are on the heap */
#define TTS_PRIORITY 4                 /* under the session (5) and the voice task (6) */
#define TTS_SB_BYTES (32 * 1024)
#define TTS_READ_BYTES 2048
#define TTS_CONNECT_MS 15000
#define TTS_READ_MS 1000               /* how often a stalled read checks for a cancel */
#define TTS_DEADLINE_US (60 * 1000000LL)
/* A second TLS connection beside the session's: its socket and the small
 * allocations under SPIRAM_MALLOC_ALWAYSINTERNAL come from internal RAM, as
 * image_fetch.c found (8 KB for TLS and 4 KB to spare). */
#define TTS_INTERNAL_FLOOR (12 * 1024)

static StreamBufferHandle_t s_sb;
static atomic_bool s_busy;             /* a task exists */
static atomic_uint s_job;
static atomic_int s_state = MUSE_TTS_IDLE;
static atomic_size_t s_received;

typedef struct {
    unsigned job;
    char text[];
} job_t;

bool muse_tts_configured(void)
{
    return muse_settings_tts_key_len() > 0;
}

static bool cancelled(unsigned job)
{
    return atomic_load(&s_job) != job;
}

/* Hands `len` bytes to the session, waiting while its buffer is full. */
static bool push(unsigned job, const uint8_t *p, size_t len)
{
    while (len) {
        size_t n = xStreamBufferSend(s_sb, p, len, pdMS_TO_TICKS(100));
        p += n;
        len -= n;
        if (cancelled(job)) {
            return false;
        }
    }
    return true;
}

static char *request_body(const char *text)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddStringToObject(o, "model_id", CONFIG_MUSE_TTS_MODEL);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return body;
}

/* Opens the request and sends the body; one retry, as the first DNS lookup
 * after Wi-Fi comes up often fails. Returns the HTTP status, or -1. */
static int send_request(esp_http_client_handle_t c, unsigned job, const char *body, int64_t deadline)
{
    int len = (int)strlen(body);
    for (int attempt = 0; attempt < 2 && !cancelled(job); attempt++) {
        if (esp_http_client_open(c, len) != ESP_OK) {
            ESP_LOGW(TAG, "connect failed%s", attempt ? "" : ", retrying");
            continue;
        }
        if (esp_http_client_write(c, body, len) != len) {
            ESP_LOGW(TAG, "sending the request failed");
            return -1;
        }
        int64_t got;
        while ((got = esp_http_client_fetch_headers(c)) == -ESP_ERR_HTTP_EAGAIN) {
            if (cancelled(job) || esp_timer_get_time() > deadline) {
                return -1;
            }
        }
        return got < 0 ? -1 : esp_http_client_get_status_code(c);
    }
    return -1;
}

static void tts_task(void *arg)
{
    job_t *j = arg;
    unsigned job = j->job;
    const char *text = j->text;
    int64_t start = esp_timer_get_time(), deadline = start + TTS_DEADLINE_US;
    muse_tts_state_t result = MUSE_TTS_FAILED;
    int status = -1;
    size_t total = 0;

    char key[MUSE_TTS_KEY_MAX + 1], voice[MUSE_TTS_VOICE_MAX + 1], url[160];
    muse_settings_tts_key(key);
    muse_settings_tts_voice(voice);
    snprintf(url, sizeof(url), TTS_URL, voice[0] ? voice : CONFIG_MUSE_TTS_VOICE);

    char *body = request_body(text);
    uint8_t *buf = heap_caps_malloc(TTS_READ_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_CONNECT_MS,
        .buffer_size = TTS_READ_BYTES,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = body && buf ? esp_http_client_init(&cfg) : NULL;
    if (c) {
        esp_http_client_set_header(c, "xi-api-key", key);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_header(c, "Accept", "audio/mpeg");
        status = send_request(c, job, body, deadline);
    }
    memset(key, 0, sizeof(key));

    if (status / 100 == 2) {
        esp_http_client_set_timeout_ms(c, TTS_READ_MS);
        while (!cancelled(job)) {
            int n = esp_http_client_read(c, (char *)buf, TTS_READ_BYTES);
            if (n > 0) {
                total += (size_t)n;
                atomic_store(&s_received, total);
                if (!push(job, buf, (size_t)n)) {
                    break;
                }
            } else if (n == 0) {
                result = esp_http_client_is_complete_data_received(c) ? MUSE_TTS_DONE : MUSE_TTS_FAILED;
                break;
            } else if (n != -ESP_ERR_HTTP_EAGAIN || esp_timer_get_time() > deadline) {
                ESP_LOGW(TAG, "read failed after %u bytes (%d)", (unsigned)total, n);
                break;
            }
        }
    } else if (status > 0 && buf) {
        /* ElevenLabs says why in JSON: {"detail":{"status":"invalid_api_key",...}} */
        esp_http_client_set_timeout_ms(c, TTS_READ_MS);
        int n = esp_http_client_read(c, (char *)buf, 255);
        ESP_LOGW(TAG, "HTTP %d %.*s", status, n > 0 ? n : 0, (const char *)buf);
    }

    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    cJSON_free(body);
    heap_caps_free(buf);
    heap_caps_free(j);
    ESP_LOGI(TAG, "speech: %u bytes in %d ms, HTTP %d%s, stack %u free, internal %u free",
             (unsigned)total, (int)((esp_timer_get_time() - start) / 1000), status,
             cancelled(job) ? ", cancelled" : "", (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    /* A cancelled job may still store DONE here a moment after the cancel: the
     * session no longer looks, and the next start resets the state. */
    if (!cancelled(job)) {
        atomic_store(&s_state, result);
    }
    atomic_store(&s_busy, false);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t muse_tts_start(const char *text)
{
    if (!muse_tts_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal < TTS_INTERNAL_FLOOR) {
        ESP_LOGW(TAG, "%u bytes of internal RAM, under %u: no speech", (unsigned)internal, TTS_INTERNAL_FLOOR);
        return ESP_ERR_NO_MEM;
    }
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_busy, &idle, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sb) {
        s_sb = xStreamBufferCreateWithCaps(TTS_SB_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    size_t len = strlen(text);
    job_t *j = s_sb ? heap_caps_malloc(sizeof(*j) + len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!j) {
        atomic_store(&s_busy, false);
        return ESP_ERR_NO_MEM;
    }
    memcpy(j->text, text, len + 1);
    xStreamBufferReset(s_sb);   /* no task is left to block on it: busy was clear */
    j->job = atomic_fetch_add(&s_job, 1) + 1;
    atomic_store(&s_received, 0);
    atomic_store(&s_state, MUSE_TTS_RUNNING);

    if (xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", TTS_STACK, j, TTS_PRIORITY, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        heap_caps_free(j);
        atomic_store(&s_state, MUSE_TTS_FAILED);
        atomic_store(&s_busy, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

muse_tts_state_t muse_tts_state(size_t *received)
{
    if (received) {
        *received = atomic_load(&s_received);
    }
    return (muse_tts_state_t)atomic_load(&s_state);
}

size_t muse_tts_pump(uint8_t *dst, size_t room)
{
    return s_sb && room ? xStreamBufferReceive(s_sb, dst, room, 0) : 0;
}

void muse_tts_cancel(void)
{
    atomic_fetch_add(&s_job, 1);
    atomic_store(&s_state, MUSE_TTS_IDLE);
}
