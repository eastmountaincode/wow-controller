/* Factory-side adapter for the documented keyboard device. All opens, ioctls
 * and ordinary reads retain their libc implementation. Only the factory's
 * single-event read on its actual evdev keyboard goes through this filter. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "wow-beat-ipc.h"
#include "wow-beat-observe.h"
#include "wow-keyboard-link.h"
#include "wow-keyboard-router.h"
#include "wow-keyboard-tracks.h"
#include "wow-keyboard-effects.h"

#if defined(__arm__) && defined(__linux__)
_Static_assert(sizeof(struct input_event) == 16, "The factory reads the 32-bit evdev ABI");
#endif

static ssize_t (*libc_read)(int, void *, size_t);
static struct stat keyboard_device;
static struct wow_keyboard_link *link_state;
static int commands = -1;
static int reply_fd = -1, acknowledge_toggle;
static int playing = -1, audio_observed;
static struct wow_keys keys;
static int engine_beat, engine_sample, beat_consumed, stream_dropped, resync_batch;
static uint64_t beat_down_ms;
static struct input_event stamp, output[KEY_MAX + 16];
static unsigned first, count;
static int deferred_physical;
static struct input_event physical_edge;

static ssize_t ordinary_read(int fd, void *buffer, size_t size) {
    return libc_read ? libc_read(fd, buffer, size) : syscall(SYS_read, fd, buffer, size);
}

static int inherited_fd(const char *name) {
    const char *value = getenv(name);
    if (!value || !*value) return -1;
    char *end;
    long fd = strtol(value, &end, 10);
    return *end || fd < 3 || fd > INT_MAX ? -1 : (int)fd;
}

__attribute__((constructor)) static void keyboard_init(void) {
    libc_read = dlsym(RTLD_NEXT, "read");
    track_symbols();
    const char *mode = getenv("WOW_BEAT_CONTROL");
    if (!mode || strcmp(mode, "1")) return;
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) return;
    exe[n] = 0;
    if (strcmp(exe, "/bin/wowmachine")) {
        const char *performance = getenv("WOW_PERFORMANCE_ENGINE");
        if (!performance || strcmp(exe, performance)) return;
        int image = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        if (image < 0) return;
        performance_keys = wow_beat_factory_image(image) == WOW_ENGINE_PERFORMANCE;
        close(image);
        if (!performance_keys) return;
    }
    int pipefd = inherited_fd("WOW_KEYBOARD_COMMAND_FD");
    int pagefd = inherited_fd("WOW_KEYBOARD_SENSOR_FD");
    struct stat pipe_stat, page_stat;
    if (pipefd < 0 || pagefd < 0 || fstat(pipefd, &pipe_stat) || !S_ISFIFO(pipe_stat.st_mode) ||
        fstat(pagefd, &page_stat) || page_stat.st_size != sizeof *link_state ||
        stat("/dev/input/event0", &keyboard_device) || !S_ISCHR(keyboard_device.st_mode)) return;
    link_state = mmap(NULL, sizeof *link_state, PROT_READ | PROT_WRITE, MAP_SHARED, pagefd, 0);
    if (link_state == MAP_FAILED) { link_state = NULL; return; }
    commands = pipefd;
    atomic_store(&link_state->beat_mute_until_ms,0);
    atomic_store(&link_state->beat_mute_pid,0);
    cursor_start();
    track_start();
    (void)fcntl(pipefd, F_SETFD, FD_CLOEXEC);
    close(pagefd);
    unsetenv("LD_PRELOAD");
    const char *startup = getenv("WOW_BEAT_STARTUP");
    if (startup && (!strcmp(startup, "0") || !strcmp(startup, "1"))) playing = *startup - '0';
}

__attribute__((destructor)) static void keyboard_exit(void) {
    if (link_state) {
        int mine = getpid();
        atomic_compare_exchange_strong(&link_state->engine_pid, &mine, 0);
    }
}

static void queue_key(unsigned code, int value) {
    /* The reader empties this batch before it polls either source again. The
     * largest batch reconstructs at most KEY_MAX + 1 edges after an overrun. */
    struct input_event ev = stamp;
    ev.type = EV_KEY;
    ev.code = (unsigned short)code;
    ev.value = value;
    output[count++] = ev;
}

/* Track only events actually handed to the factory. It finishes each handler
 * before requesting the next edge. A BEAT pitch edit consumes a bare release,
 * as do the START transport and SAMPLE+BEAT+START calibration chords. */
static void observe_edge(const struct input_event *ev, uint64_t now) {
    if (ev->type != EV_KEY || (ev->value != 0 && ev->value != 1)) return;
    if (ev->code == WOW_BEAT_KEY) {
        if (ev->value) {
            engine_beat = 1;
            beat_consumed = 0;
            beat_down_ms = now;
            atomic_store(&link_state->pitch_watch, atomic_load(&link_state->pitch));
        } else {
            int watch = atomic_exchange(&link_state->pitch_watch, -1);
            if (engine_beat && !beat_consumed && playing != 1)
                playing = watch >= 0 ? 1 : -1;
            engine_beat = 0;
        }
    } else if (ev->code == WOW_SAMPLE_KEY) {
        engine_sample = ev->value;
    } else if (ev->code == WOW_START_KEY && ev->value && engine_beat && !beat_consumed) {
        /* Once the stock long-hold callback fires it consumes BEAT and starts
         * or selects a beat. The exact boundary depends on its 200 ms tick. */
        if (now - beat_down_ms >= 950) playing = -1;
        else if (!engine_sample && playing >= 0) playing ^= 1;
        beat_consumed = 1;
        if (engine_sample) engine_sample = 0; /* calibration unpresses SAMPLE */
    }
}

static int current_beat(void) {
    int observed = wow_beat_observe(getpid());
    audio_observed = observed == 0 || observed == 1;
    if (audio_observed) playing = observed;
    else if (engine_beat && !beat_consumed && playing != 1 &&
             beat_time_ns() / 1000000 - beat_down_ms >= 950) return -1;
    return playing;
}

static char beat_result(int state) {
    if (state < 0) return 'U';
    return audio_observed ? (state ? 'p' : 's') : (state ? '1' : '0');
}

static void answer(char result) {
    /* A client may disconnect during a gesture. A blocked SIGPIPE on this
     * thread must never terminate the factory audio process. */
    sigset_t mask, before, pending;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &mask, &before) == 0) {
        sigpending(&pending);
        if (write(reply_fd, &result, 1) < 0 && errno == EPIPE &&
            !sigismember(&before, SIGPIPE) && !sigismember(&pending, SIGPIPE)) {
            struct timespec zero = {0};
            (void)sigtimedwait(&mask, NULL, &zero);
        }
        sigprocmask(SIG_SETMASK, &before, NULL);
    }
    close(reply_fd);
    reply_fd = -1;
    acknowledge_toggle = 0;
}

static char keyboard_available(int keyboard_fd) {
    struct pollfd keyboard = {.fd=keyboard_fd, .events=POLLIN};
    if (poll(&keyboard, 1, 0) != 0) return 'H';
    unsigned char held[sizeof keys.physical];
    int check = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (check < 0) return 'D';
    int got = ioctl(check, EVIOCGKEY(sizeof held), held);
    close(check);
    if (got < 0) return 'Q';
    if (memcmp(held, (unsigned char[sizeof held]){0}, sizeof held)) return 'H';
    return 0;
}

#include "wow-keyboard-selection.h"

static void read_command(int keyboard_fd) {
    struct wow_keyboard_command msg;
    if (ordinary_read(commands, &msg, sizeof msg) != sizeof msg) return;
    struct beat_request *req = &msg.request;
    uint64_t now = beat_time_ns();
    if (!now || now > req->expires_ns) return;
    char path[PATH_MAX];
    beat_reply_path(path, req);
    reply_fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (reply_fd < 0) return;
    if (msg.engine_pid != getpid()) { answer('N'); return; }
    if (req->action == WOW_EFFECT_QUERY) { answer(current_effect()); return; }
    if (selection.phase!=LOAD_IDLE) { answer('H'); return; }
    if (req->action == WOW_EFFECT_ON || req->action == WOW_EFFECT_OFF) {
        char state = current_effect();
        if (state == 'U' || ((state - 'a') % 2) == (req->action == WOW_EFFECT_ON)) {
            answer(state); return;
        }
        struct pollfd target = {.fd=reply_fd, .events=POLLOUT};
        if (beat_time_ns() > req->expires_ns || poll(&target, 1, 0) != 1 ||
            (target.revents & (POLLERR | POLLHUP | POLLNVAL))) { answer('T'); return; }
        pending_effect = req->action;
        queue_key(WOW_PANEL_FX_KEY, 1);
        queue_key(WOW_PANEL_FX_KEY, 0);
        return;
    }
    if (req->action == WOW_BEAT_QUERY) { answer(beat_result(current_beat())); return; }
    if (req->action != WOW_BEAT_PLAY && req->action != WOW_BEAT_STOP &&
        req->action != WOW_BEAT_TOGGLE) { answer('I'); return; }
    if (keys.held || keys.phase != BEAT_IDLE || stream_dropped) { answer('H'); return; }
    if (req->action != WOW_BEAT_TOGGLE) {
        int state = current_beat();
        if (state < 0) { answer('U'); return; }
        if (state == (req->action == WOW_BEAT_PLAY)) { answer(beat_result(state)); return; }
    }
    /* Sampling the audio track gives the physical deck time to change. Give
     * queued physical edges priority, including a button held before launch. */
    char busy = keyboard_available(keyboard_fd);
    if (busy) { answer(busy); return; }
    /* The client was live and the command was on time. Recheck after sampling
     * so a delayed query cannot trigger a gesture for an abandoned request. */
    struct pollfd target = {.fd=reply_fd, .events=POLLOUT};
    if (beat_time_ns() > req->expires_ns || poll(&target, 1, 0) != 1 ||
        (target.revents & (POLLERR | POLLHUP | POLLNVAL))) { answer('T'); return; }
    acknowledge_toggle = req->action == WOW_BEAT_TOGGLE;
    wow_transport_chord(0, queue_key);
    /* Reply on read() re-entry after the engine has consumed the final edge. */
}

static void resync_keys(int fd) {
    unsigned char held[sizeof keys.physical] = {0};
    if (ioctl(fd, EVIOCGKEY(sizeof held), held) < 0) return;
    for (unsigned code = 0; code <= KEY_MAX; ++code) {
        if (performance_keys && code == WOW_PANEL_FX_KEY) continue;
        int down = !!(held[code / 8] & (1u << (code % 8)));
        if (down == !!wow_physical_key(&keys, code)) continue;
        wow_track_key(&keys, code, down);
        queue_key(code, down);
    }
    keys.phase = wow_physical_key(&keys, WOW_BEAT_KEY) ? BEAT_PASSTHROUGH : BEAT_IDLE;
    stream_dropped = 0;
    resync_batch = 1;
}

static ssize_t routed_read(int fd, void *buffer) {
    for (;;) {
        uint64_t now = beat_time_ns() / 1000000;
        if (first < count) {
            const struct input_event *ev = &output[first++];
            observe_edge(ev, now);
            memcpy(buffer, ev, sizeof *ev);
            return sizeof *ev;
        }
        first = count = 0;
        if (resync_batch || stream_dropped) { playing = -1; resync_batch = 0; }
        if (reply_fd >= 0) {
            if (pending_effect) {
                char state = current_effect();
                answer(state != 'U' && ((state - 'a') % 2) == (pending_effect == WOW_EFFECT_ON) ? state : 'U');
                pending_effect = 0;
            } else {
                char result = beat_result(current_beat());
                answer(result == 'U' && acknowledge_toggle ? 'K' : result);
            }
        }
        if (deferred_physical) {
            deferred_physical=0;
            track_filter(TRACK_ALL,0,NULL);
            wow_route_key(&keys, physical_edge.code, physical_edge.value, now, queue_key);
            if (count) continue;
        }
        if (commands < 0) return ordinary_read(fd, buffer, sizeof stamp);
        if (atomic_load(&link_state->engine_pid) != getpid())
            atomic_store(&link_state->engine_pid, getpid());
        advance_selection(now);
        if (count) continue;
        wow_key_tick(&keys, now, queue_key);
        if (count) continue;
        int timeout = keys.deadline ? (int)(keys.deadline - now) : -1;
        if (selection.phase!=LOAD_IDLE && (timeout<0 || timeout>16)) timeout=16;
        struct pollfd source[] = {{fd,POLLIN,0}, {commands,POLLIN,0}, {track_listener,POLLIN,0},
                                  {cursor_listener,POLLIN,0}};
        int ready = poll(source, sizeof source / sizeof *source, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (source[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            /* Drain any physically deferred release before resuming native
             * reads. If our parent exits, the ordinary deck stays playable. */
            interrupt_selection();
            wow_finish_taps(&keys, queue_key);
            close(commands);
            commands = -1;
            atomic_store(&link_state->engine_pid, 0);
            continue;
        }
        if (!ready) continue;
        if (source[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            ssize_t n = ordinary_read(fd, &stamp, sizeof stamp);
            if (n != sizeof stamp) return n;
            if (stamp.type == EV_SYN && stamp.code == SYN_DROPPED) {
                wow_finish_taps(&keys, queue_key);
                playing = -1;
                stream_dropped = 1;
            } else if (stream_dropped) {
                if (stamp.type == EV_SYN && stamp.code == SYN_REPORT) resync_keys(fd);
            } else if (stamp.type == EV_KEY) {
                if (performance_keys && stamp.code == WOW_PANEL_FX_KEY) continue;
                if (selection.phase!=LOAD_IDLE && selection.phase!=LOAD_END) {
                    interrupt_selection();
                    physical_edge=stamp;
                    deferred_physical=1;
                    continue;
                }
                wow_route_key(&keys, stamp.code, stamp.value, beat_time_ns() / 1000000, queue_key);
            } else {
                memcpy(buffer, &stamp, sizeof stamp);
                return sizeof stamp;
            }
            continue;
        }
        if (source[1].revents & POLLIN) read_command(fd);
        else if (source[2].revents & POLLIN) track_request(fd);
        else if (source[3].revents & POLLIN) cursor_request();
    }
}

ssize_t read(int fd, void *buffer, size_t size) {
    if (!link_state || size != sizeof(struct input_event)) return ordinary_read(fd, buffer, size);
    int saved = errno;
    struct stat s;
    int keyboard = fstat(fd, &s) == 0 && s.st_dev == keyboard_device.st_dev &&
                   s.st_ino == keyboard_device.st_ino && s.st_rdev == keyboard_device.st_rdev;
    errno = saved;
    return keyboard ? routed_read(fd, buffer) : ordinary_read(fd, buffer, size);
}
