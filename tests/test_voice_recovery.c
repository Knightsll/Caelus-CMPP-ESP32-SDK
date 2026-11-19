/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_recovery_logic.h"

static voice_recovery_gesture_t boot_hold(void)
{
    voice_recovery_gesture_t s = {0};
    assert(voice_recovery_step(&s, true, 100) == RECOVERY_NO_EVENT);
    return s;
}

static voice_recovery_gesture_t factory_prompt(void)
{
    voice_recovery_gesture_t s = boot_hold();
    assert(voice_recovery_step(&s, true, 12100) == RECOVERY_FACTORY_ARMED);
    assert(voice_recovery_step(&s, false, 13100) == RECOVERY_CONFIRM_PROMPT);
    assert(s.action == VOICE_RECOVERY_NONE);
    return s;
}

static void gestures(void)
{
    voice_recovery_gesture_t s = {0};
    assert(voice_recovery_step(&s, false, 100) == RECOVERY_CANCELLED);
    assert(voice_recovery_step(&s, true, 6000) == RECOVERY_NO_EVENT);
    assert(s.action == VOICE_RECOVERY_NONE); /* Finished boot detector stays inert. */
    s = boot_hold();
    assert(voice_recovery_step(&s, false, 5099) == RECOVERY_CANCELLED);
    s = boot_hold();
    assert(voice_recovery_step(&s, true, 5100) == RECOVERY_NETWORK_ARMED);
    assert(voice_recovery_step(&s, false, 6100) == RECOVERY_NETWORK_SELECTED);
    assert(s.action == VOICE_RECOVERY_NETWORK);
    assert(voice_recovery_step(&s, false, 99999) == RECOVERY_NO_EVENT);
    s = boot_hold();
    assert(voice_recovery_step(&s, false, 12099) == RECOVERY_NETWORK_SELECTED);
    s = boot_hold();
    assert(voice_recovery_step(&s, false, 12100) == RECOVERY_CONFIRM_PROMPT);
    s = boot_hold();
    assert(voice_recovery_step(&s, true, 20100) == RECOVERY_CANCELLED);
    assert(s.action == VOICE_RECOVERY_NONE); /* Stuck at boot. */
    s = factory_prompt();
    assert(voice_recovery_step(&s, false, 23100) == RECOVERY_CANCELLED);
    s = factory_prompt();
    assert(voice_recovery_step(&s, true, 14000) == RECOVERY_NO_EVENT);
    assert(voice_recovery_step(&s, false, 16999) == RECOVERY_CANCELLED);
    s = factory_prompt();
    voice_recovery_step(&s, true, 14000);
    assert(voice_recovery_step(&s, true, 17000) == RECOVERY_CONFIRM_ARMED);
    assert(s.action == VOICE_RECOVERY_NONE); /* No operation until release. */
    assert(voice_recovery_step(&s, false, 17100) == RECOVERY_FACTORY_SELECTED);
    assert(s.action == VOICE_RECOVERY_FACTORY);
    s = factory_prompt();
    voice_recovery_step(&s, true, 22000);
    assert(voice_recovery_step(&s, false, 25000) == RECOVERY_CANCELLED);
    s = factory_prompt();
    voice_recovery_step(&s, true, 14000);
    assert(voice_recovery_step(&s, true, 23100) == RECOVERY_CANCELLED);
}

static voice_recovery_runtime_t runtime_hold(void)
{
    voice_recovery_runtime_t s = {0};
    assert(voice_recovery_runtime_step(&s, false, 0) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, false, 50) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, true, 100) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, true, 149) == RECOVERY_NO_EVENT);
    assert(!s.holding);
    assert(voice_recovery_runtime_step(&s, true, 150) == RECOVERY_NO_EVENT);
    assert(s.holding);
    return s;
}

static void runtime_gestures(void)
{
    /* Exact stable-edge boundaries, with a raw release debounced for 50 ms. */
    const unsigned held[] = {100, 4999, 5000, 6000, 8000, 11999, 12000, 25000};
    for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); ++i) {
        voice_recovery_runtime_t s = runtime_hold();
        assert(voice_recovery_runtime_step(&s, false, 100 + held[i]) == RECOVERY_NO_EVENT);
        voice_recovery_event_t event = voice_recovery_runtime_step(&s, false, 150 + held[i]);
        assert(event == (held[i] >= 5000 && held[i] < 12000
            ? RECOVERY_NETWORK_SELECTED : RECOVERY_CANCELLED));
        assert(voice_recovery_runtime_step(&s, false, 30000) == RECOVERY_NO_EVENT);
    }
    /* Boot-held / stuck key cannot request another reset until released. */
    voice_recovery_runtime_t s = {0};
    for (unsigned now = 0; now < 30000; now += 10)
        assert(voice_recovery_runtime_step(&s, true, now) == RECOVERY_NO_EVENT);
    voice_recovery_runtime_step(&s, false, 30000);
    voice_recovery_runtime_step(&s, false, 30050);
    voice_recovery_runtime_step(&s, true, 30100);
    voice_recovery_runtime_step(&s, true, 30150);
    assert(voice_recovery_runtime_step(&s, true, 35150) == RECOVERY_NETWORK_ARMED);
    assert(voice_recovery_runtime_step(&s, true, 35160) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, true, 42150) == RECOVERY_CANCELLED);
    assert(voice_recovery_runtime_step(&s, true, 50000) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, false, 50010) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, false, 50060) == RECOVERY_NO_EVENT);
    /* Release bounce must not restart early. A new press after cancellation works. */
    voice_recovery_runtime_step(&s, true, 50100);
    voice_recovery_runtime_step(&s, true, 50150);
    assert(voice_recovery_runtime_step(&s, true, 55150) == RECOVERY_NETWORK_ARMED);
    assert(voice_recovery_runtime_step(&s, false, 55160) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, true, 55180) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, true, 55230) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, false, 56100) == RECOVERY_NO_EVENT);
    assert(voice_recovery_runtime_step(&s, false, 56150) == RECOVERY_NETWORK_SELECTED);
    assert(voice_recovery_runtime_step(&s, false, 57000) == RECOVERY_NO_EVENT);
    /* A <50 ms press is ignored and contributes no time to the next hold. */
    voice_recovery_runtime_step(&s, true, 58000);
    voice_recovery_runtime_step(&s, false, 58020);
    voice_recovery_runtime_step(&s, false, 58070);
    assert(!s.holding);
}

typedef struct {
    bool paired, sdk_wifi, saved_wifi, identity, mqtt, cmpp, unrelated;
    voice_recovery_action_t intent;
    unsigned calls, fail_call;
    bool fail_after_write;
} store_t;

static int mutate(store_t *s, bool *value)
{
    ++s->calls;
    bool fail = s->calls == s->fail_call;
    if (!fail || s->fail_after_write) *value = false;
    return fail ? 99 : 0;
}

static int unpair(void *p) { store_t *s = p; return mutate(s, &s->paired); }
static int wifi(void *p) { store_t *s = p; return mutate(s, &s->sdk_wifi); }
static int erase(void *p, const char *name)
{
    store_t *s = p;
    if (!strcmp(name, "cmpp_wifi")) return mutate(s, &s->saved_wifi);
    if (!strcmp(name, "cmpp_mqtt")) return mutate(s, &s->mqtt);
    if (!strcmp(name, "cmpp")) return mutate(s, &s->cmpp);
    assert(!strcmp(name, "mcp_light"));
    return mutate(s, &s->identity);
}
static int finish(void *p)
{
    store_t *s = p;
    assert(!s->paired && !s->sdk_wifi && !s->saved_wifi);
    if (s->intent == VOICE_RECOVERY_FACTORY) assert(!s->identity && !s->mqtt && !s->cmpp);
    else assert(s->identity && s->mqtt && s->cmpp);
    bool pending = true;
    int err = mutate(s, &pending);
    if (!pending) s->intent = VOICE_RECOVERY_NONE;
    return err;
}

static void interrupted_cleanup(void)
{
    for (unsigned mode = VOICE_RECOVERY_NETWORK; mode <= VOICE_RECOVERY_FACTORY; ++mode) {
        unsigned count = mode == VOICE_RECOVERY_NETWORK ? 4 : 7;
        for (unsigned failure = 0; failure <= count; ++failure) {
            for (unsigned after = 0; after <= 1; ++after) {
                store_t s = {.paired = true, .sdk_wifi = true, .saved_wifi = true,
                    .identity = true, .mqtt = true, .cmpp = true, .unrelated = true,
                    .intent = mode, .fail_call = failure, .fail_after_write = after};
                voice_recovery_ops_t ops = {.context = &s, .mark_unpaired = unpair,
                    .reset_wifi = wifi, .erase_namespace = erase, .finish = finish};
                int err = voice_recovery_apply(s.intent, &ops);
                assert(err == (failure ? 99 : 0));
                if (failure) assert(s.calls == failure); /* Stop on first error. */
                if (s.intent != VOICE_RECOVERY_NONE) {
                    s.calls = s.fail_call = 0;
                    assert(voice_recovery_apply(s.intent, &ops) == 0);
                }
                assert(!s.paired && !s.sdk_wifi && !s.saved_wifi && s.unrelated);
                assert(s.intent == VOICE_RECOVERY_NONE);
                bool retained = mode == VOICE_RECOVERY_NETWORK;
                assert(s.identity == retained && s.mqtt == retained && s.cmpp == retained);
            }
        }
    }
    assert(voice_recovery_apply(VOICE_RECOVERY_NONE, NULL) != 0);
}

int main(void)
{
    gestures();
    runtime_gestures();
    interrupted_cleanup();
    puts("voice recovery: boot/runtime gestures, debounce, confirmation, deadlines and interrupted cleanup passed");
    return 0;
}
