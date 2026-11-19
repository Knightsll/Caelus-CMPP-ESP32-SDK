/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_recovery_logic.h"

static voice_recovery_event_t cancel(voice_recovery_gesture_t *s)
{
    s->phase = RECOVERY_DONE;
    s->action = VOICE_RECOVERY_NONE;
    return RECOVERY_CANCELLED;
}

voice_recovery_event_t voice_recovery_step(voice_recovery_gesture_t *s,
                                         bool pressed, uint64_t now)
{
    if (s->phase == RECOVERY_DONE) return RECOVERY_NO_EVENT;
    if (s->phase == RECOVERY_WAIT_BOOT_KEY) {
        if (!pressed) return cancel(s);
        s->phase = RECOVERY_FIRST_HOLD;
        s->since_ms = now;
        return RECOVERY_NO_EVENT;
    }
    uint64_t held = now - s->since_ms;
    if (s->phase == RECOVERY_FIRST_HOLD) {
        if (held >= 20000) return cancel(s); /* Stuck key never clears data. */
        if (!pressed) {
            if (held < 5000) return cancel(s);
            if (held < 12000) {
                s->action = VOICE_RECOVERY_NETWORK;
                s->phase = RECOVERY_DONE;
                return RECOVERY_NETWORK_SELECTED;
            }
            s->phase = RECOVERY_WAIT_CONFIRM;
            s->confirm_deadline_ms = now + 10000;
            return RECOVERY_CONFIRM_PROMPT;
        }
        if (held >= 12000 && s->announced < 2) {
            s->announced = 2;
            return RECOVERY_FACTORY_ARMED;
        }
        if (held >= 5000 && s->announced < 1) {
            s->announced = 1;
            return RECOVERY_NETWORK_ARMED;
        }
    } else if (s->phase == RECOVERY_WAIT_CONFIRM) {
        if (now >= s->confirm_deadline_ms) return cancel(s);
        if (pressed) {
            s->phase = RECOVERY_CONFIRM_HOLD;
            s->since_ms = now;
            s->announced = 0;
        }
    } else if (s->phase == RECOVERY_CONFIRM_HOLD) {
        /* Both the second press AND release must fit in the 10 s window. */
        if (now >= s->confirm_deadline_ms) return cancel(s);
        if (!pressed) {
            if (held < 3000) return cancel(s);
            s->action = VOICE_RECOVERY_FACTORY;
            s->phase = RECOVERY_DONE;
            return RECOVERY_FACTORY_SELECTED;
        }
        if (held >= 3000 && !s->announced) {
            s->announced = 1;
            return RECOVERY_CONFIRM_ARMED;
        }
    }
    return RECOVERY_NO_EVENT;
}

voice_recovery_event_t voice_recovery_runtime_step(voice_recovery_runtime_t *s,
                                                 bool pressed, uint64_t now)
{
    if (!s->initialized) {
        s->initialized = true;
        s->candidate = pressed;
        s->changed_ms = now;
        return RECOVERY_NO_EVENT;
    }
    if (pressed != s->candidate) {
        s->candidate = pressed;
        s->changed_ms = now;
    }
    if (now - s->changed_ms < 50) return RECOVERY_NO_EVENT;
    /* A held/cancelled boot key must be released before a runtime gesture. */
    if (!s->released) {
        if (!pressed) s->released = true;
        return RECOVERY_NO_EVENT;
    }
    if (!s->holding) {
        if (pressed) {
            s->holding = true;
            s->announced = false;
            s->since_ms = now;
        }
        return RECOVERY_NO_EVENT;
    }
    uint64_t held = now - s->since_ms;
    if (held >= 12000 || !pressed) {
        s->holding = false;
        s->released = !pressed;
        return held >= 5000 && held < 12000 ? RECOVERY_NETWORK_SELECTED : RECOVERY_CANCELLED;
    }
    if (held >= 5000 && !s->announced) {
        s->announced = true;
        return RECOVERY_NETWORK_ARMED;
    }
    return RECOVERY_NO_EVENT;
}

int voice_recovery_apply(voice_recovery_action_t action,
                         const voice_recovery_ops_t *ops)
{
    if (!ops || !ops->mark_unpaired || !ops->reset_wifi ||
        !ops->erase_namespace || !ops->finish ||
        (action != VOICE_RECOVERY_NETWORK && action != VOICE_RECOVERY_FACTORY)) return -1;
    int err = ops->mark_unpaired(ops->context);
    if (err) return err;
    err = ops->reset_wifi(ops->context);
    if (err) return err;
    err = ops->erase_namespace(ops->context, "cmpp_wifi");
    if (err) return err;
    if (action == VOICE_RECOVERY_FACTORY) {
        const char *names[] = {"cmpp_mqtt", "cmpp", "mcp_light"};
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            err = ops->erase_namespace(ops->context, names[i]);
            if (err) return err;
        }
    }
    return ops->finish(ops->context);
}
