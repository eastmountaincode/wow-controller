#ifndef WOW_KEYBOARD_LINK_H
#define WOW_KEYBOARD_LINK_H
#include <stdatomic.h>
#include <stdint.h>

/* Owned only by our controller and keyboard adapter. These are the latest
 * public control-board sensor readings and this adapter's pitch-edit watch. */
struct wow_keyboard_link {
    atomic_int engine_pid;
    atomic_int beat_mute_pid;
    atomic_uint beat_mute_until_ms, beat_mute_ack_ms;
    atomic_int pitch;
    atomic_int pitch_watch;
};
struct wow_keyboard_command {
    struct beat_request request;
    int engine_pid;
};
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Shared pitch readings must be lock-free");

static inline void wow_keyboard_pitch(struct wow_keyboard_link *link, int value) {
    if (!link) return;
    atomic_store(&link->pitch, value);
    int watch = atomic_load(&link->pitch_watch);
    /* The stock threshold is 88 raw counts; leave margin for the one serial
     * report the engine may be processing when the button is pressed. */
    if (watch >= 0 && (value - watch > 72 || watch - value > 72))
        atomic_compare_exchange_strong(&link->pitch_watch, &watch, -2);
}
#endif
