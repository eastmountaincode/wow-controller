/* Translate BEAT gestures only on the factory keyboard. The MIDI reader always
 * receives the original evdev stream. Stock begins timing a hold on key-down. */
#ifndef WOW_KEYBOARD_ROUTER_H
#define WOW_KEYBOARD_ROUTER_H
#include <stdint.h>

enum { WOW_BEAT_KEY = 165, WOW_START_KEY = 164, WOW_SAMPLE_KEY = 163 };
enum { WOW_TAP_MS = 225, WOW_DOUBLE_GAP_MS = 260, WOW_HOLD_GUARD_MS = 950 };
enum beat_phase { BEAT_IDLE, BEAT_FIRST_DOWN, BEAT_GAP, BEAT_SECOND_DOWN, BEAT_HELD, BEAT_PASSTHROUGH };
struct wow_keys {
    enum beat_phase phase;
    uint64_t deadline, down_at;
    unsigned char physical[96];
    int held, single_toggles;
};

typedef void (*wow_emit_key)(unsigned code, int value);

static int wow_physical_key(const struct wow_keys *k, unsigned code) {
    return code < sizeof(k->physical) * 8 && (k->physical[code / 8] & (1u << (code % 8)));
}

static void wow_track_key(struct wow_keys *k, unsigned code, int value) {
    if (code >= sizeof(k->physical) * 8 || (value != 0 && value != 1)) return;
    if (!!wow_physical_key(k, code) == value) return;
    k->physical[code / 8] ^= 1u << (code % 8);
    k->held += value ? 1 : -1;
}

static void wow_transport_chord(int beat_already_down, wow_emit_key emit) {
    if (!beat_already_down) emit(WOW_BEAT_KEY, 1);
    emit(WOW_START_KEY, 1);
    emit(WOW_START_KEY, 0);
    emit(WOW_BEAT_KEY, 0);
}

/* Only a completed first tap may toggle transport. A hold, pitch edit or
 * input overrun instead ends the native key at its real release boundary. */
static int wow_pitch_edited(void);
static void wow_end_taps(struct wow_keys *k, wow_emit_key emit, int toggle) {
    if (k->phase == BEAT_GAP || k->phase == BEAT_SECOND_DOWN) {
        if (toggle && k->single_toggles) wow_transport_chord(1, emit);
        else emit(WOW_BEAT_KEY, 0);
    }
    if (k->phase == BEAT_SECOND_DOWN) emit(WOW_BEAT_KEY, 1);
    k->phase = wow_physical_key(k, WOW_BEAT_KEY) ? BEAT_PASSTHROUGH : BEAT_IDLE;
    k->deadline = 0;
}

static void wow_finish_taps(struct wow_keys *k, wow_emit_key emit) {
    wow_end_taps(k, emit, 1);
}

static void wow_key_tick(struct wow_keys *k, uint64_t now, wow_emit_key emit) {
    if (k->phase == BEAT_IDLE || k->phase == BEAT_PASSTHROUGH) return;
    if (k->single_toggles && wow_pitch_edited()) {
        wow_end_taps(k, emit, 0);
        return;
    }
    if (!k->deadline || now < k->deadline) return;
    int held = k->phase == BEAT_FIRST_DOWN || k->phase == BEAT_SECOND_DOWN;
    wow_finish_taps(k, emit);
    if (held && k->single_toggles) k->phase = BEAT_HELD;
}

static void wow_route_key(struct wow_keys *k, unsigned code, int value,
                          uint64_t now, wow_emit_key emit) {
    wow_key_tick(k, now, emit);
    if (value != 0 && value != 1) {
        /* Autorepeats don't count toward taps; the factory ignores value 2. */
        emit(code, value);
        return;
    }
    if (code != WOW_BEAT_KEY) {
        wow_finish_taps(k, emit);
        wow_track_key(k, code, value);
        emit(code, value);
        return;
    }
    int was_down = wow_physical_key(k, code);
    wow_track_key(k, code, value);
    if (was_down == !!value) return;
    if (value) {
        k->down_at = now;
        if (k->phase == BEAT_GAP) {
            k->phase = BEAT_SECOND_DOWN;
            k->deadline = now + WOW_TAP_MS;
        } else {
            emit(code, 1);
            k->phase = k->held == 1 ? BEAT_FIRST_DOWN : BEAT_PASSTHROUGH;
            k->deadline = k->phase == BEAT_FIRST_DOWN ? now + WOW_TAP_MS : 0;
        }
    } else if (k->phase == BEAT_FIRST_DOWN) {
        k->phase = BEAT_GAP;
        k->deadline = now + WOW_DOUBLE_GAP_MS;
    } else if (k->phase == BEAT_SECOND_DOWN) {
        if (k->single_toggles) emit(code, 0); /* Native next keeps the transport state. */
        else wow_transport_chord(1, emit);
        k->phase = BEAT_IDLE;
        k->deadline = 0;
    } else {
        /* An ordinary press can outlast the tap window. Leave genuine
         * factory holds and button combinations to their existing handlers. */
        if (k->phase == BEAT_HELD && now - k->down_at < WOW_HOLD_GUARD_MS)
            wow_transport_chord(1, emit);
        else emit(code, 0);
        k->phase = BEAT_IDLE;
        k->deadline = 0;
    }
}
#endif
