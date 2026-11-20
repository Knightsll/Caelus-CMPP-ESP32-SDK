/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_media.h"
#include "voice_audio.h"
#include "voice_volume.h"
#include "voice_wire.h"
#include "voice_status.h"
#include "voice_lifecycle.h"
#include "voice_socket.h"
#include "voice_upload_queue.h"
#include "network.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PLAYBACK_QUEUE_FRAMES 10 /* Existing bounded downstream playback. */
#define HELLO_TIMEOUT_MS 5000
#define HEARTBEAT_TIMEOUT_MS 30000
#define OPEN_TIMEOUT_MS 30000

typedef voice_packet_t packet_t;
_Static_assert(VOICE_FRAME_BYTES == VOICE_UPLOAD_PACKET_BYTES, "PCM packet size mismatch");

struct voice_media {
    SemaphoreHandle_t lock;
    voice_upload_queue_t *upload;
    QueueHandle_t playback;
    httpd_handle_t server;
    device_identity_t identity;
    int fd;
    uint32_t input_turn;
    uint32_t playback_generation;
    voice_lifecycle_t lifecycle;
    voice_diagnostics_t diagnostics;
    int64_t last_rx_ms, opened_ms, connected_ms;
    char channel_id[33];
    char last_closed_id[33];
    voice_wire_t wire;
    voice_ptt_t ptt;
    bool recording, playing, awaiting_response, output_busy, stopped_confirmed;
};

static const char *TAG = "voice_board";
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void take(voice_media_t *b) { xSemaphoreTake(b->lock, portMAX_DELAY); }
static void give(voice_media_t *b) { xSemaphoreGive(b->lock); }

/* Called with lock held. Epoch invalidates queued callbacks as well as PCM. */
static esp_err_t stop_locked(voice_media_t *b, voice_stop_reason_t reason)
{
    bool first_stop = voice_lifecycle_stop(&b->lifecycle, reason);
    if (!first_stop && b->stopped_confirmed) return ESP_OK;
    if (b->channel_id[0]) strcpy(b->last_closed_id, b->channel_id);
    b->channel_id[0] = '\0';
    b->recording = b->playing = b->awaiting_response = b->output_busy = false;
    ++b->playback_generation;
    voice_status_update(VOICE_LED_MEDIA | VOICE_LED_FAULT,
                        b->lifecycle.fault ? VOICE_LED_FAULT : 0);
    memset(&b->wire, 0, sizeof(b->wire));
    memset(&b->ptt, 0, sizeof(b->ptt));
    voice_upload_queue_reset(b->upload);
    xQueueReset(b->playback);
    esp_err_t result = voice_audio_mute(true);
    /* Flush old DMA audio while muted before any later channel can unmute. */
    const int16_t silence[VOICE_FRAME_SAMPLES] = {0};
    for (int i = 0; i < 3; ++i) {
        esp_err_t err = voice_audio_write(silence, VOICE_FRAME_SAMPLES);
        if (err != ESP_OK) result = err;
    }
    if (b->server && b->fd >= 0) (void)httpd_sess_trigger_close(b->server, b->fd);
    b->stopped_confirmed = result == ESP_OK;
    if (first_stop) ESP_LOGI(TAG, "Channel stopped: %s", voice_stop_reason_name(reason));
    if (result != ESP_OK) {
        voice_lifecycle_audio_stop_failed(&b->lifecycle);
        voice_status_update(VOICE_LED_FAULT, VOICE_LED_FAULT);
        ESP_LOGE(TAG, "Could not confirm audio stopped: %s", esp_err_to_name(result));
    }
    return result;
}

static esp_err_t send_frame(voice_media_t *b, int fd, bool text, const void *data, size_t size)
{
    httpd_ws_frame_t frame = {
        .final = true, .type = text ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data, .len = size,
    };
    return httpd_ws_send_frame_async(b->server, fd, &frame);
}

static void upload_work(void *arg);

static int submit_upload_work(void *arg)
{
    voice_media_t *b = arg;
    return httpd_queue_work(b->server, upload_work, b);
}

static bool schedule_upload_locked(voice_media_t *b, const char *packet_kind)
{
    errno = 0;
    esp_err_t err = voice_upload_queue_schedule(b->upload, submit_upload_work, b);
    int saved_errno = errno;
    if (err == ESP_OK) return true;
    voice_diag_upload_failure(&b->diagnostics, VOICE_STOP_UPLOAD_WORK_FAILED,
                              packet_kind, b->lifecycle.generation, err, saved_errno);
    return false;
}

/* One frame per HTTP work item: retain a single socket writer while letting
 * the server process incoming control frames between outgoing packets. */
static void upload_work(void *arg)
{
    voice_media_t *b = arg;
    packet_t packet;
    take(b);
    bool queued = voice_upload_queue_pop(b->upload, &packet);
    int fd = b->fd;
    bool valid = queued && voice_lifecycle_accept(&b->lifecycle, packet.generation) && b->wire.ready && fd >= 0;
    give(b);
    if (valid) {
        int64_t started_us = esp_timer_get_time();
        esp_err_t err = send_frame(b, fd, packet.text, packet.pcm, packet.bytes);
        uint64_t elapsed_us = (uint64_t)(esp_timer_get_time() - started_us);
        take(b);
        voice_diag_send(&b->diagnostics, elapsed_us,
                        packet.text ? 0 : (uint32_t)packet.bytes, err != ESP_OK);
        if (err != ESP_OK && voice_lifecycle_accept(&b->lifecycle, packet.generation))
            stop_locked(b, VOICE_STOP_SEND_FAILED);
        give(b);
    }
    take(b);
    b->upload->work_pending = false;
    if (!schedule_upload_locked(b, "queued_packet"))
        stop_locked(b, VOICE_STOP_UPLOAD_WORK_FAILED);
    give(b);
}

static bool upload_locked(voice_media_t *b, bool text, const void *data, size_t bytes,
                          const char *packet_kind)
{
    packet_t packet = {.generation = b->lifecycle.generation, .bytes = bytes, .text = text};
    if (bytes == 0 || bytes > sizeof(packet.pcm) || (!text && bytes % sizeof(int16_t))) {
        voice_diag_upload_failure(&b->diagnostics, VOICE_STOP_UPLOAD_FAILED,
                                  packet_kind, b->lifecycle.generation, 0, 0);
        return false;
    }
    memcpy(packet.pcm, data, bytes);
    size_t dropped_bytes = 0;
    if (!voice_upload_queue_push(b->upload, &packet, &dropped_bytes)) {
        voice_diag_upload_failure(&b->diagnostics, VOICE_STOP_UPLOAD_CONTROL_FULL,
                                  packet_kind, b->lifecycle.generation, 0, 0);
        return false;
    }
    if (dropped_bytes) {
        voice_diag_add(&b->diagnostics.upload_queue_full_count, 1);
        voice_diag_add(&b->diagnostics.upload_pcm_dropped_frames, 1);
        voice_diag_add(&b->diagnostics.upload_pcm_dropped_bytes, (uint32_t)dropped_bytes);
    }
    uint32_t queued = (uint32_t)b->upload->count;
    if (queued > b->diagnostics.upload_queue_high_water)
        b->diagnostics.upload_queue_high_water = queued;
    if (!text) voice_diag_add(&b->diagnostics.upload_pcm_queued_bytes, (uint32_t)bytes);
    return schedule_upload_locked(b, packet_kind);
}

static bool input_event(voice_media_t *b, const char *type)
{
    char json[128];
    int size = snprintf(json, sizeof(json), "{\"type\":\"%s\",\"turn_id\":\"in_%lu\"}",
                         type, (unsigned long)b->input_turn);
    if (size <= 0 || (size_t)size >= sizeof(json)) {
        voice_diag_upload_failure(&b->diagnostics, VOICE_STOP_UPLOAD_FAILED,
                                  type, b->lifecycle.generation, 0, 0);
        return false;
    }
    return upload_locked(b, true, json, (size_t)size, type);
}

static void capture_task(void *arg)
{
    voice_media_t *b = arg;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    int16_t previous_pcm[VOICE_FRAME_SAMPLES] = {0};
    int64_t next_heap_log_ms = 0;
    for (;;) {
        esp_err_t err = voice_audio_read(pcm);
        bool pressed = voice_audio_pressed();
        take(b);
        bool ok = true;
        voice_stop_reason_t input_failure = VOICE_STOP_NONE;
        if (b->channel_id[0] && !network_is_connected()) {
            ESP_LOGW(TAG, "Network lost; discarding voice channel and buffered audio");
            stop_locked(b, VOICE_STOP_NETWORK_LOST);
        }
        bool blocked = (b->output_busy || b->awaiting_response) && !b->wire.interrupt_enabled;
        voice_ptt_event_t ptt = voice_ptt_step(&b->ptt, b->wire.ready, blocked,
                                              b->recording, pressed);
        if (b->wire.ready && !blocked && err == ESP_OK) {
            if (ptt == VOICE_PTT_START) {
                ++b->input_turn;
                if (b->wire.interrupt_enabled) {
                    ++b->playback_generation;
                    xQueueReset(b->playback);
                    ok = voice_audio_discard_output() == ESP_OK;
                    if (!ok) input_failure = VOICE_STOP_AUDIO_STOP_FAILED;
                    b->playing = b->output_busy = b->awaiting_response = false;
                    voice_status_update(VOICE_LED_PLAYING | VOICE_LED_WAITING, 0);
                }
                char turn[32];
                snprintf(turn, sizeof(turn), "in_%lu", (unsigned long)b->input_turn);
                ok = ok && voice_wire_begin_input(&b->wire, turn);
                b->recording = ok;
                ok = ok && input_event(b, "input.start");
                if (ok) {
                    voice_status_update(VOICE_LED_RECORDING, VOICE_LED_RECORDING);
                    ESP_LOGI(TAG, "PTT input.start queued in_%lu", (unsigned long)b->input_turn);
                }
                /* One 20 ms pre-roll frame covers button debounce, bounded. */
                ok = ok && upload_locked(b, false, previous_pcm, sizeof(previous_pcm), "pcm");
            }
            if (b->recording) {
                ok = ok && upload_locked(b, false, pcm, sizeof(pcm), "pcm");
                if (ptt == VOICE_PTT_END) {
                    ok = ok && input_event(b, "input.end");
                    b->recording = false;
                    voice_wire_end_input(&b->wire);
                    /* Close the thinking-window race before the box can reply. */
                    b->awaiting_response = true;
                    b->ptt.armed = false;
                    voice_status_update(VOICE_LED_RECORDING | VOICE_LED_WAITING, VOICE_LED_WAITING);
                    if (ok) ESP_LOGI(TAG, "PTT input.end queued in_%lu", (unsigned long)b->input_turn);
                }
            }
        } else if (b->recording && err != ESP_OK) {
            ok = false;
        }
        bool hello_expired = b->lifecycle.active && b->fd >= 0 && !b->wire.ready && now_ms() - b->connected_ms > HELLO_TIMEOUT_MS;
        bool dead = b->wire.ready && now_ms() - b->last_rx_ms > HEARTBEAT_TIMEOUT_MS;
        bool unused = b->channel_id[0] && b->fd < 0 && now_ms() - b->opened_ms > OPEN_TIMEOUT_MS;
        if (!ok || hello_expired || dead || unused) {
            voice_stop_reason_t reason = !ok ? (input_failure != VOICE_STOP_NONE ? input_failure
                : err != ESP_OK ? VOICE_STOP_CAPTURE_FAILED : b->diagnostics.last_upload_failure)
                : hello_expired ? VOICE_STOP_HELLO_TIMEOUT
                : dead ? VOICE_STOP_HEARTBEAT_TIMEOUT : VOICE_STOP_OPEN_TIMEOUT;
            stop_locked(b, reason);
        }
        give(b);
        if (now_ms() >= next_heap_log_ms) {
            next_heap_log_ms = now_ms() + 30000;
            ESP_LOGI(TAG, "Heap internal free=%u min=%u largest=%u psram_free=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        if (err == ESP_OK) memcpy(previous_pcm, pcm, sizeof(pcm));
        else memset(previous_pcm, 0, sizeof(previous_pcm));
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void playback_task(void *arg)
{
    voice_media_t *b = arg;
    packet_t packet;
    for (;;) {
        if (xQueueReceive(b->playback, &packet, portMAX_DELAY) != pdTRUE) continue;
        take(b);
        if (b->lifecycle.active && packet.generation == b->playback_generation &&
            b->wire.ready && !voice_wire_output_blocked(&b->wire)) {
            esp_err_t err = ESP_OK;
            voice_stop_reason_t reason = VOICE_STOP_PLAYBACK_FAILED;
            if (packet.bytes) {
                /* Set codec mute once per turn, not an I2C transaction for
                 * every 20 ms packet. LED follows submitted I2S audio. */
                if (!b->playing) err = voice_audio_mute(false);
                if (err == ESP_OK) err = voice_audio_write(packet.pcm, packet.bytes / 2);
                if (err == ESP_OK) voice_diag_add(&b->diagnostics.output_pcm_i2s_bytes, (uint32_t)packet.bytes);
                if (err == ESP_OK && !b->playing) {
                    b->playing = true;
                    voice_status_update(VOICE_LED_PLAYING, VOICE_LED_PLAYING);
                    ESP_LOGI(TAG, "Output playing %s", b->wire.turn_id);
                }
            } else {
                /* Drain previously queued hardware samples. No 'played' claim
                 * is sent: acoustic completion still requires bench testing. */
                const int16_t silence[VOICE_FRAME_SAMPLES] = {0};
                for (int i = 0; i < 3 && err == ESP_OK; ++i)
                    err = voice_audio_write(silence, VOICE_FRAME_SAMPLES);
                if (err == ESP_OK) err = voice_audio_mute(true);
                if (err == ESP_OK) {
                    char drained[160];
                    int length = snprintf(drained, sizeof(drained),
                        "{\"type\":\"output.drained\",\"turn_id\":\"%s\"}", b->wire.turn_id);
                    if (!upload_locked(b, true, drained, (size_t)length, "output.drained")) {
                        err = ESP_FAIL;
                        reason = b->diagnostics.last_upload_failure;
                    }
                }
                b->output_busy = false;
                b->playing = false;
                voice_status_update(VOICE_LED_PLAYING, 0);
                if (err == ESP_OK) ESP_LOGI(TAG, "Output drained %s", b->wire.turn_id);
            }
            if (err != ESP_OK) {
                stop_locked(b, reason);
            }
        }
        give(b);
    }
}

static voice_media_t *request_board(httpd_req_t *req) { return req->user_ctx; }

static esp_err_t before_handshake(httpd_req_t *req)
{
    voice_media_t *b = request_board(req);
    char authorization[sizeof("Bearer ") + DEVICE_AUTH_TOKEN_MAX_LEN];
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    bool authorized = len == 7 + strlen(b->identity.auth_token) && len < sizeof(authorization) &&
        httpd_req_get_hdr_value_str(req, "Authorization", authorization, sizeof(authorization)) == ESP_OK;
    unsigned char difference = 0;
    if (authorized) {
        for (size_t i = 0; i < 7; ++i) difference |= (unsigned char)(authorization[i] ^ "Bearer "[i]);
        for (size_t i = 0; i < strlen(b->identity.auth_token); ++i)
            difference |= (unsigned char)(authorization[i + 7] ^ b->identity.auth_token[i]);
        authorized = difference == 0;
    }
    memset(authorization, 0, sizeof(authorization));
    if (!authorized) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Authentication required");
        return ESP_FAIL;
    }
    take(b);
    bool available = b->channel_id[0] && b->fd < 0;
    if (available) {
        b->fd = httpd_req_to_sockfd(req);
        b->last_rx_ms = now_ms();
        b->connected_ms = b->last_rx_ms;
    }
    give(b);
    if (!available) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Prepare one voice channel first");
    }
    return available ? ESP_OK : ESP_FAIL;
}

static esp_err_t socket_opened(httpd_handle_t server, int fd)
{
    (void)server;
    if (voice_socket_nodelay(fd) != 0) {
        ESP_LOGE(TAG, "Voice TCP_NODELAY failed: errno=%d", errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Voice TCP_NODELAY readback=1");
    return ESP_OK;
}

static void socket_closed(httpd_handle_t server, int fd)
{
    voice_media_t *b = httpd_get_global_user_ctx(server);
    take(b);
    if (b->fd == fd) {
        b->fd = -1;
        stop_locked(b, VOICE_STOP_PEER_CLOSED);
    }
    give(b);
    close(fd); /* A custom close_fn owns socket closure. */
}

/* A failed/late callback may belong to a session already explicitly closed.
 * Preserve the first stop and never fault a later channel for old work. */
static esp_err_t websocket_failure(voice_media_t *b, int fd, uint32_t generation,
                                   voice_stop_reason_t reason)
{
    take(b);
    if (b->fd == fd && voice_lifecycle_accept(&b->lifecycle, generation))
        stop_locked(b, reason);
    give(b);
    return ESP_FAIL;
}

static esp_err_t websocket_handler(httpd_req_t *req)
{
    voice_media_t *b = request_board(req);
    int fd = httpd_req_to_sockfd(req);
    take(b);
    uint32_t generation = b->lifecycle.generation;
    bool active = b->fd == fd && voice_lifecycle_accept(&b->lifecycle, generation);
    give(b);
    if (!active) return ESP_FAIL; /* Already closing: no parse and no new fault. */

    uint8_t payload[VOICE_PCM_BYTES + 1];
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK)
        return websocket_failure(b, fd, generation, VOICE_STOP_RECEIVE_FAILED);
    if (frame.len > VOICE_PCM_BYTES || !frame.final || frame.fragmented)
        return websocket_failure(b, fd, generation, VOICE_STOP_INVALID_FRAME);
    frame.payload = payload;
    /* The length probe consumes the header even for an empty Ping/Close. */
    if (frame.len && httpd_ws_recv_frame(req, &frame, VOICE_PCM_BYTES) != ESP_OK)
        return websocket_failure(b, fd, generation, VOICE_STOP_RECEIVE_FAILED);
    payload[frame.len] = '\0';
    take(b);
    /* MCP close can run while the HTTP task is reading the frame payload. */
    if (b->fd != fd || !voice_lifecycle_accept(&b->lifecycle, generation)) {
        give(b);
        return ESP_FAIL;
    }
    b->last_rx_ms = now_ms();
    bool ok = true, reply_ready = false;
    voice_stop_reason_t reason = VOICE_STOP_INVALID_FRAME;
    char channel_id[33];
    strcpy(channel_id, b->channel_id);
    if (frame.type == HTTPD_WS_TYPE_PING) {
        give(b);
        frame.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &frame) == ESP_OK ? ESP_OK
            : websocket_failure(b, fd, generation, VOICE_STOP_SEND_FAILED);
    } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        (void)httpd_ws_send_frame(req, &frame);
        stop_locked(b, VOICE_STOP_PEER_CLOSED);
        give(b);
        return ESP_FAIL;
    } else if (frame.type == HTTPD_WS_TYPE_PONG) {
        give(b);
        return ESP_OK;
    } else if (frame.type == HTTPD_WS_TYPE_TEXT) {
        voice_message_t msg = voice_wire_text(&b->wire, b->channel_id, (char *)payload, frame.len);
        reason = msg == VOICE_BAD ? VOICE_STOP_INVALID_FRAME : VOICE_STOP_STATE_CONFLICT;
        if (msg == VOICE_HELLO) {
            reply_ready = true;
            voice_lifecycle_ready(&b->lifecycle);
            voice_status_update(VOICE_LED_MEDIA | VOICE_LED_FAULT, VOICE_LED_READY);
        } else if (msg == VOICE_CONFIGURED || msg == VOICE_INPUT_ACCEPTED || msg == VOICE_IGNORED) {
            /* Negotiation and the matching downlink barrier don't end capture. */
        } else if (msg == VOICE_OUTPUT_START && !b->output_busy && !b->recording) {
            b->output_busy = true;
            b->awaiting_response = true;
            voice_status_update(VOICE_LED_WAITING, VOICE_LED_WAITING);
            if (!b->wire.interrupt_enabled) b->ptt.armed = false;
        } else if (msg == VOICE_SESSION_THINKING && !b->recording) {
            b->awaiting_response = true;
            if (!b->wire.interrupt_enabled) b->ptt.armed = false;
            voice_status_update(VOICE_LED_WAITING, VOICE_LED_WAITING);
        } else if (msg == VOICE_SESSION_LISTENING && !b->recording && !b->output_busy) {
            b->awaiting_response = false;
            if (!b->wire.interrupt_enabled) b->ptt.armed = false;
            voice_status_update(VOICE_LED_WAITING, 0);
        } else if (msg == VOICE_OUTPUT_END) {
            packet_t end = {.generation = b->playback_generation};
            ok = xQueueSend(b->playback, &end, 0) == pdTRUE;
            reason = VOICE_STOP_PLAYBACK_QUEUE_FULL;
        } else ok = false;
    } else if (frame.type == HTTPD_WS_TYPE_BINARY) {
        if (voice_wire_output_blocked(&b->wire)) {
            /* Consume already-in-flight old PCM until the host's ordered fence.
             * Invalid frame sizes are still rejected, even while discarding. */
            ok = frame.len > 0 && frame.len <= VOICE_PCM_BYTES && frame.len % 2 == 0;
            if (!ok) stop_locked(b, VOICE_STOP_INVALID_FRAME);
            give(b);
            return ok ? ESP_OK : ESP_FAIL;
        }
        ok = voice_wire_pcm(&b->wire, frame.len);
        if (ok) {
            voice_diag_add(&b->diagnostics.output_pcm_received_bytes, (uint32_t)frame.len);
            packet_t packet = {.generation = b->playback_generation, .bytes = frame.len};
            memcpy(packet.pcm, payload, frame.len);
            ok = xQueueSend(b->playback, &packet, 0) == pdTRUE;
            reason = VOICE_STOP_PLAYBACK_QUEUE_FULL;
        }
    } else ok = false;
    if (!ok) stop_locked(b, reason);
    give(b);
    if (!ok) {
        static const char error[] = "{\"type\":\"error\",\"code\":\"invalid_frame_or_overflow\"}";
        (void)send_frame(b, fd, true, error, sizeof(error) - 1);
        return ESP_FAIL;
    }
    if (reply_ready) {
        char ready[256];
        int length = snprintf(ready, sizeof(ready), "{\"type\":\"ready\",\"protocol\":\"%s\",\"channel_id\":\"%s\",\"features\":[\"%s\"]}", VOICE_PROTOCOL, channel_id, VOICE_INTERRUPT_FEATURE);
        return send_frame(b, fd, true, ready, (size_t)length) == ESP_OK ? ESP_OK
            : websocket_failure(b, fd, generation, VOICE_STOP_SEND_FAILED);
    }
    return ESP_OK;
}

esp_err_t voice_media_create(voice_media_t **output)
{
    if (!output) return ESP_ERR_INVALID_ARG;
    *output = NULL;
    voice_media_t *b = calloc(1, sizeof(*b));
    if (!b) return ESP_ERR_NO_MEM;
    b->fd = -1;
#if CONFIG_SPIRAM
    /* This is a CPU-owned queue, not an I2S DMA buffer. */
    b->upload = heap_caps_calloc(1, sizeof(*b->upload), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    b->upload = calloc(1, sizeof(*b->upload));
#endif
    b->lock = xSemaphoreCreateMutex();
    b->playback = xQueueCreate(PLAYBACK_QUEUE_FRAMES, sizeof(packet_t));
    esp_err_t err = ESP_ERR_NO_MEM;
    TaskHandle_t capture = NULL;
    if (!b->upload || !b->lock || !b->playback) goto failed;
    ESP_LOGI(TAG, "Upload queue allocation: %u bytes in %s", (unsigned)sizeof(*b->upload),
             esp_ptr_external_ram(b->upload) ? "PSRAM" : "internal RAM");
    err = voice_audio_init();
    if (err != ESP_OK) goto failed;
    b->stopped_confirmed = true;
    ESP_LOGI(TAG, "Voice upload backlog: %u PCM frames, %u control slots, %u allocated bytes",
             VOICE_UPLOAD_PCM_FRAMES, VOICE_UPLOAD_CONTROL_SLOTS, (unsigned)sizeof(*b->upload));
    ESP_LOGI(TAG, "Voice heap after audio/queue init: free_internal=%u largest_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    err = ESP_ERR_NO_MEM;
    if (xTaskCreate(capture_task, "voice_capture", 6144, b, 5, &capture) != pdPASS) goto failed;
    if (xTaskCreate(playback_task, "voice_playback", 6144, b, 5, NULL) != pdPASS) goto failed;
    *output = b;
    return ESP_OK;
failed:
    if (capture) vTaskDelete(capture);
    if (b->lock) vSemaphoreDelete(b->lock);
    if (b->playback) vQueueDelete(b->playback);
    free(b->upload);
    free(b);
    return err;
}

esp_err_t voice_media_open(voice_media_t *b, bool dry_run)
{
    take(b);
    esp_err_t err = ESP_OK;
    if (!b->server || !network_is_connected() || b->channel_id[0] || b->fd >= 0 || !b->stopped_confirmed) err = ESP_ERR_INVALID_STATE;
    else if (!dry_run) {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); ++i)
            snprintf(b->channel_id + i * 2, 3, "%02x", random[i]);
        voice_lifecycle_open(&b->lifecycle);
        b->opened_ms = now_ms();
    }
    give(b);
    return err;
}

esp_err_t voice_media_close(voice_media_t *b, const char *id, bool dry_run, bool *changed)
{
    *changed = false;
    take(b);
    esp_err_t err = ESP_OK;
    if (!b->channel_id[0] && strcmp(id, b->last_closed_id) == 0) {
        if (!b->stopped_confirmed) err = ESP_FAIL;
    } else if (strcmp(id, b->channel_id) != 0) err = ESP_ERR_INVALID_STATE;
    else if (!dry_run) {
        *changed = true;
        err = stop_locked(b, VOICE_STOP_REQUESTED);
    }
    give(b);
    return err;
}

esp_err_t voice_media_set_volume(voice_media_t *b, int volume, bool dry_run, bool *changed)
{
    *changed = false;
    if (volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    take(b);
    esp_err_t err = dry_run ? ESP_OK : voice_volume_set(volume, changed);
    give(b);
    return err;
}

esp_err_t voice_media_stop(voice_media_t *b)
{
    take(b);
    esp_err_t err = stop_locked(b, VOICE_STOP_REQUESTED);
    give(b);
    return err;
}

void voice_media_observe(voice_media_t *b, voice_media_state_t *state)
{
    take(b);
    strcpy(state->channel_id, b->channel_id);
    state->ready = b->wire.ready;
    state->recording = b->recording;
    state->playing = b->playing;
    state->awaiting_response = b->awaiting_response;
    state->fault = b->lifecycle.fault;
    state->last_fault_reason = voice_stop_reason_name(b->lifecycle.last_fault_reason);
    state->last_stop_reason = voice_stop_reason_name(b->lifecycle.last_stop_reason);
    state->diagnostics = b->diagnostics;
    state->volume = voice_volume_get();
    state->codec_readback = *voice_volume_readback();
    give(b);
}

esp_err_t voice_media_start(voice_media_t *b, const device_identity_t *identity)
{
    if (!b || !identity || CONFIG_CMPP_VOICE_HTTP_PORT == CONFIG_CMPP_BOARD_HTTP_PORT) return ESP_ERR_INVALID_ARG;
    b->identity = *identity;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_CMPP_VOICE_HTTP_PORT;
    config.ctrl_port += 2;
    config.max_uri_handlers = 1;
    config.max_open_sockets = 2;
    config.stack_size = 8192;
    config.global_user_ctx = b;
    config.open_fn = socket_opened;
    config.close_fn = socket_closed;
    ESP_RETURN_ON_ERROR(httpd_start(&b->server, &config), TAG, "Voice HTTP server failed");
    const httpd_uri_t endpoint = {
        .uri = "/voice", .method = HTTP_GET, .handler = websocket_handler,
        .user_ctx = b, .is_websocket = true, .handle_ws_control_frames = true,
        .ws_pre_handshake_cb = before_handshake,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(b->server, &endpoint), TAG, "Voice WS registration failed");
    ESP_LOGW(TAG, "Development-only plaintext WS on port %d; pairing token required", CONFIG_CMPP_VOICE_HTTP_PORT);
    return ESP_OK;
}
