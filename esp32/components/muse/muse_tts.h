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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Spoken replies from ElevenLabs. One request at a time: a task of its own
 * posts the text and streams the MP3 it gets back into a buffer, which the
 * voice session drains on its own task with muse_tts_pump(), so nothing on
 * the session's task blocks on the network.
 *
 * The API key and voice are settings (tts.key, tts.voice), kept in NVS.
 */

typedef enum {
    MUSE_TTS_IDLE,
    MUSE_TTS_RUNNING,
    MUSE_TTS_DONE,      /* the whole MP3 has arrived */
    MUSE_TTS_FAILED,
} muse_tts_state_t;

/* An API key is set. */
bool muse_tts_configured(void);

/* Starts speaking `text` (copied). ESP_ERR_INVALID_STATE: no key, or the last
 * request is still winding down, so try again shortly. ESP_ERR_NO_MEM: too
 * little internal RAM for another TLS connection, or no task. */
esp_err_t muse_tts_start(const char *text);

/* The current request's state, and how many bytes it has received. */
muse_tts_state_t muse_tts_state(size_t *received);

/* Moves up to `room` bytes of MP3 into `dst` without blocking; 0 if none. */
size_t muse_tts_pump(uint8_t *dst, size_t room);

/* Drops the current request without waiting: its task notices and exits. */
void muse_tts_cancel(void);

#ifdef __cplusplus
}
#endif
