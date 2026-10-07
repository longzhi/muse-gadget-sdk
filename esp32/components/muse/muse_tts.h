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
 * Spoken replies from ElevenLabs, a sentence at a time. A message's speech is
 * a job: muse_tts_start() opens it, muse_tts_say() hands it each sentence as
 * the reply's text arrives, and muse_tts_finish() says there are no more. A
 * task of its own posts them in order (whatever is waiting goes as one
 * request) and streams the MP3 back into a buffer, which the voice session
 * drains on its own task with muse_tts_pump(), so nothing on the session's
 * task blocks on the network. The MP3s follow one another in the buffer.
 *
 * The API key and voice are settings (tts.key, tts.voice), kept in NVS.
 */

typedef enum {
    MUSE_TTS_IDLE,
    MUSE_TTS_RUNNING,
    MUSE_TTS_DONE,      /* every sentence's MP3 has arrived, after muse_tts_finish */
    MUSE_TTS_FAILED,
} muse_tts_state_t;

/* An API key is set. */
bool muse_tts_configured(void);

/* Opens a job. ESP_ERR_INVALID_STATE: no key, or the last job is still
 * winding down, so try again shortly. ESP_ERR_NO_MEM: too little internal
 * RAM for another TLS connection, or no task. */
esp_err_t muse_tts_start(void);

/* Queues `len` bytes of text (copied) to be spoken after what's queued.
 * ESP_ERR_INVALID_STATE: no job running (finished, failed or cancelled).
 * ESP_ERR_NO_MEM: the queue is full; try again later. */
esp_err_t muse_tts_say(const char *text, size_t len);

/* No more text for this job: it ends DONE once the last MP3 has arrived. */
esp_err_t muse_tts_finish(void);

/* The current job's state, and how many bytes of MP3 it has received. */
muse_tts_state_t muse_tts_state(size_t *received);

/* Every MP3 asked for so far has been handed over and the job is waiting for
 * text: the last one in the buffer is whole, with nothing following it yet. */
bool muse_tts_waiting(void);

/* Moves up to `room` bytes of MP3 into `dst` without blocking; 0 if none. */
size_t muse_tts_pump(uint8_t *dst, size_t room);

/* Drops the current job without waiting: its task notices and exits. */
void muse_tts_cancel(void);

#ifdef __cplusplus
}
#endif
