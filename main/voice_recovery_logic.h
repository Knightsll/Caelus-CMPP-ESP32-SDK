/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VOICE_RECOVERY_NONE = 0,
    VOICE_RECOVERY_NETWORK = 1,
    VOICE_RECOVERY_FACTORY = 2,
} voice_recovery_action_t;

typedef enum {
    RECOVERY_WAIT_BOOT_KEY, RECOVERY_FIRST_HOLD, RECOVERY_WAIT_CONFIRM,
    RECOVERY_CONFIRM_HOLD, RECOVERY_DONE,
} voice_recovery_phase_t;

typedef enum {
    RECOVERY_NO_EVENT, RECOVERY_NETWORK_ARMED, RECOVERY_FACTORY_ARMED,
    RECOVERY_CONFIRM_PROMPT, RECOVERY_CONFIRM_ARMED, RECOVERY_CANCELLED,
    RECOVERY_NETWORK_SELECTED, RECOVERY_FACTORY_SELECTED,
} voice_recovery_event_t;

typedef struct {
    voice_recovery_phase_t phase;
    uint64_t since_ms, confirm_deadline_ms;
    unsigned announced;
    voice_recovery_action_t action;
} voice_recovery_gesture_t;

/* Input is already debounced. First call decides whether SW1 was held at boot. */
voice_recovery_event_t voice_recovery_step(voice_recovery_gesture_t *state,
                                         bool pressed, uint64_t now_ms);

typedef struct {
    bool initialized, candidate, released, holding, announced;
    uint64_t changed_ms, since_ms;
} voice_recovery_runtime_t;

/* Raw active-low key state: debounce 50 ms, require a fresh press after release.
 * Runtime permits network reset only: release after >=5 s and <12 s. */
voice_recovery_event_t voice_recovery_runtime_step(voice_recovery_runtime_t *state,
                                                 bool pressed, uint64_t now_ms);

typedef struct {
    void *context;
    int (*mark_unpaired)(void *context);
    int (*reset_wifi)(void *context);
    int (*erase_namespace)(void *context, const char *name);
    int (*finish)(void *context);
} voice_recovery_ops_t;

/* Durable intent must already exist. Stop on first failure; retry on next boot
 * before loading identity or starting any network service. */
int voice_recovery_apply(voice_recovery_action_t action,
                         const voice_recovery_ops_t *ops);
