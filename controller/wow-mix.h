#ifndef WOW_MIX_H
#define WOW_MIX_H
#include "wow-preferences.h"
#include <math.h>
#include <stdint.h>

#define WOW_MIX_PATH WOW_STATE_DIR "/mix-levels"
struct wow_mix { int beat, sample, master, blend_enabled; };

static inline int wow_mix_percent(const char *text) {
    size_t n = strlen(text);
    if (!n || n > 3 || strspn(text, "0123456789") != n) return -1;
    int value = atoi(text);
    return value <= 100 ? value : -1;
}

static inline struct wow_mix wow_mix_settings(void) {
    const struct wow_mix defaults = {100, 100, 100, 1};
    char line[40], *rest;
    if (wow_read_line(WOW_MIX_PATH, line, sizeof line)) return defaults;
    int column[4], count = 0;
    for (char *field = strtok_r(line, " \t", &rest); field; field = strtok_r(NULL, " \t", &rest)) {
        if (count == 4 || (column[count] = wow_mix_percent(field)) < 0) return defaults;
        ++count;
    }
    /* Existing two-column preferences retain the physical blend and full master.
     * The next ordinary write atomically upgrades the settings file. */
    if (count == 2) return (struct wow_mix){column[0], column[1], 100, 1};
    if (count == 4 && column[3] <= 1)
        return (struct wow_mix){column[0], column[1], column[2], column[3]};
    return defaults;
}

static inline int wow_mix_needs_trim(struct wow_mix v) {
    return v.beat != 100 || v.sample != 100 || v.master != 100 || !v.blend_enabled;
}

/* Stock scales (ADC - 220) by 4096/3756, clamps to 0..4095, then
 * squares master volume. The balance gives both tracks unity at center. */
static inline float wow_knob_level(unsigned raw) {
    if (raw <= 220) return 0;
    unsigned step = (raw - 220) * 4096 / 3756;
    return (step < 4095 ? step : 4095) / 4095.0f;
}

static inline void wow_encode_knob(uint8_t *bytes, float level) {
    unsigned raw = level >= 1 ? 3976 : 220 + (unsigned)(level * (3756.0f * 4095 / 4096) + 0.5f);
    bytes[0] = (uint8_t)raw;
    bytes[1] = (uint8_t)(raw >> 8);
}

static inline void wow_trim_reply(uint8_t reply[11], struct wow_mix mix) {
    if (!wow_mix_needs_trim(mix)) return;
    unsigned master = reply[0] | ((unsigned)reply[1] << 8);
    unsigned balance = reply[2] | ((unsigned)reply[3] << 8);
    if (master > 4095 || balance > 4095) return;
    float position = mix.blend_enabled ? wow_knob_level(balance) : 0.5f;
    float beat = fminf(1, 2 * (1 - position)) * (mix.beat / 100.0f);
    float sample = fminf(1, 2 * position) * (mix.sample / 100.0f);
    float peak = fmaxf(beat, sample);
    float gain = peak * (mix.master / 100.0f);
    if (gain < 1) wow_encode_knob(reply, wow_knob_level(master) * sqrtf(gain));
    if (!mix.blend_enabled || (peak > 0 && mix.beat != mix.sample)) {
        float new_balance = 0.5f;
        if (peak > 0) new_balance = beat >= sample ? sample / (2 * peak) : 1 - beat / (2 * peak);
        wow_encode_knob(reply + 2, new_balance);
    }
}
#endif
