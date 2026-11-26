/* Translate BEAT gestures only on the factory keyboard. The MIDI reader always
 * receives the original evdev stream. Stock begins timing a hold on key-down. */
#ifndef WOW_KEYBOARD_ROUTER_H
#define WOW_KEYBOARD_ROUTER_H
#include <stdint.h>

enum { WOW_BEAT_KEY = 165, WOW_START_KEY = 164, WOW_SAMPLE_KEY = 163 };
enum { WOW_TAP_MS = 225, WOW_DOUBLE_GAP_MS = 260 };
enum beat_phase { BEAT_IDLE, BEAT_FIRST_DOWN, BEAT_GAP, BEAT_SECOND_DOWN, BEAT_PASSTHROUGH };
struct wow_keys {
    enum beat_phase phase;
    uint64_t deadline;
    unsigned char physical[96];
    int held;
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

/* Resolve the deferred first release. When a second press has turned into
 * a hold or a chord, return its original press to the engine as well. */
static void wow_finish_taps(struct wow_keys *k, wow_emit_key emit) {
    if (k->phase == BEAT_GAP || k->phase == BEAT_SECOND_DOWN) emit(WOW_BEAT_KEY, 0);
    if (k->phase == BEAT_SECOND_DOWN) emit(WOW_BEAT_KEY, 1);
    k->phase = wow_physical_key(k, WOW_BEAT_KEY) ? BEAT_PASSTHROUGH : BEAT_IDLE;
    k->deadline = 0;
}

static void wow_key_tick(struct wow_keys *k, uint64_t now, wow_emit_key emit) {
    if (k->deadline && now >= k->deadline) wow_finish_taps(k, emit);
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
        wow_transport_chord(1, emit);
        k->phase = BEAT_IDLE;
        k->deadline = 0;
    } else {
        emit(code, 0);
        k->phase = BEAT_IDLE;
        k->deadline = 0;
    }
}
#endif
