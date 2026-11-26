#ifndef WOW_LAMP_H
#define WOW_LAMP_H

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/statfs.h>
#include <linux/magic.h>
#endif

/* Stock UART TX is motor (little endian), red, blue, green, newline. */
static inline int wow_lamp_rgb(const uint8_t command[6]) {
    return ((int)command[2] << 16) | ((int)command[4] << 8) | command[3];
}

static inline int wow_lamp_palette(int rgb) {
    if (rgb < 0) return 127;
    if (!rgb) return 0;
    int red = rgb >> 16, green = (rgb >> 8) & 255, blue = rgb & 255;
    int peak = red > green ? red : green;
    if (blue > peak) peak = blue;
    return (red * 3 >= peak * 2) | ((green * 3 >= peak * 2) << 1) |
           ((blue * 3 >= peak * 2) << 2);
}

/* One nonzero PWM step is still visible to the MIDI receiver. */
static inline unsigned wow_lamp_level(unsigned raw) {
    return (raw * 127 + 254) / 255;
}

static inline int wow_lamp_midi(int rgb) {
    int palette = wow_lamp_palette(rgb);
    if (rgb < 0) return palette;
    return palette | (wow_lamp_level((unsigned)rgb >> 16) << 7) |
           (wow_lamp_level((rgb >> 8) & 255) << 14) | (wow_lamp_level(rgb & 255) << 21);
}

#ifndef WOW_LAMP_RAM_DIR
#define WOW_LAMP_RAM_DIR "/dev"
#endif
#define WOW_LAMP_STATE WOW_LAMP_RAM_DIR "/wow-lamp-rgb"
enum wow_lamp_mode { LAMP_FACTORY, LAMP_RED, LAMP_GREEN, LAMP_BLUE, LAMP_WHITE, LAMP_OFF };
static const char *const wow_lamp_names[] = {NULL, "red", "green", "blue", "white", "off"};
/* Peak values come from the factory's normal white START light. */
static const int wow_lamp_colors[] = {0, 0x7f0000, 0x006e00, 0x00005a, 0x7f6e5a, 0};
enum { WOW_LAMP_TEST_MS = 4000, WOW_LAMP_KNOWN = 1 << 27 };

struct wow_lamp_state {
    _Atomic uint64_t request, applied, snapshot, mix_knobs, mix_seen_ns;
};
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "Lamp requests need lock-free 64-bit atomics");

static inline uint64_t wow_lamp_now_ns(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return 0;
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

/* Requests carry the deck's monotonic deadline and one of the fixed colors.
 * A stopped browser cannot prolong an override. */
static inline uint64_t wow_lamp_override(struct wow_lamp_state *state, uint8_t frame[6]) {
    if (!state) return 0;
    uint64_t request = atomic_load(&state->request);
    if (!request) return 0;
    int mode = request & 7;
    uint64_t now = wow_lamp_now_ns();
    if (!now || now >= (request >> 3) || mode > LAMP_OFF) {
        atomic_compare_exchange_strong(&state->request, &request, 0);
        return 0;
    }
    if (mode != LAMP_FACTORY) {
        int rgb = wow_lamp_colors[mode];
        frame[2] = (uint8_t)(rgb >> 16);
        frame[3] = (uint8_t)rgb;
        frame[4] = (uint8_t)(rgb >> 8);
    }
    return request;
}

static inline int wow_lamp_remaining_ms(uint64_t snap) {
    if (!((snap >> 24) & 7)) return 0;
    uint32_t now_ms = (uint32_t)(wow_lamp_now_ns() / UINT64_C(1000000));
    uint32_t remaining = (uint32_t)(snap >> 32) - now_ms;
    return remaining <= WOW_LAMP_TEST_MS ? (int)remaining : 0;
}

static inline void wow_lamp_forwarded(struct wow_lamp_state *state, int rgb, uint64_t request) {
    if (!state) return;
    int mode = request & 7;
    uint64_t expires_ms = mode ? (request >> 3) / UINT64_C(1000000) : 0;
    uint64_t snap = (expires_ms << 32) | WOW_LAMP_KNOWN | (mode << 24) | (unsigned)rgb;
    if (atomic_load(&state->snapshot) != snap) atomic_store(&state->snapshot, snap);
    if (atomic_load(&state->applied) != request) atomic_store(&state->applied, request);
}

/* For the LAMP diagnostic, the writer keeps its lock until exit. Only store
 * samples in RAM. MIDI observation works independently of this diagnostic. */
static inline struct wow_lamp_state *wow_open_lamp_writer(int *lease) {
#ifdef __linux__
    struct statfs fs;
    if (statfs(WOW_LAMP_RAM_DIR, &fs) || (fs.f_type != TMPFS_MAGIC && fs.f_type != RAMFS_MAGIC)) return NULL;
#endif
    int fd = open(WOW_LAMP_STATE, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || flock(fd, LOCK_EX | LOCK_NB) ||
        ftruncate(fd, sizeof(struct wow_lamp_state))) {
        close(fd); return NULL;
    }
    struct wow_lamp_state *state = mmap(NULL, sizeof *state, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (state == MAP_FAILED) { close(fd); return NULL; }
    atomic_store(&state->request, 0);
    atomic_store(&state->applied, 0);
    atomic_store(&state->snapshot, 0);
    atomic_store(&state->mix_knobs, 0);
    atomic_store(&state->mix_seen_ns, 0);
    *lease = fd;
    return state;
}

static inline struct wow_lamp_state *wow_open_lamp_client(int writable) {
    int access = writable ? O_RDWR : O_RDONLY;
    int fd = open(WOW_LAMP_STATE, access | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    struct wow_lamp_state *state = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof *state &&
        flock(fd, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK) {
        state = mmap(NULL, sizeof *state, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
        if (state == MAP_FAILED) state = NULL;
    }
    close(fd);
    return state;
}

#endif
