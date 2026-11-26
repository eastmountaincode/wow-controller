#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "wow-beat-control.h"
#include "wow-mix.h"
#include "wow-lamp.h"

/* The factory engine owns every motor command. A timed START-light test may
 * replace its three color bytes. Volume trims affect the two rear knobs. */
enum { ANGLE, FADER_LEFT, FADER_RIGHT, PITCH, REAR_MASTER, REAR_BLEND, ANALOG_COUNT };
static const uint8_t analog_cc[ANALOG_COUNT] = {
    [ANGLE]=16, [FADER_LEFT]=17, [FADER_RIGHT]=18, [PITCH]=19, [REAR_MASTER]=23, [REAR_BLEND]=24
};
static const int key_codes[] = {164, 165, 163, 59, 60, 61, 62};
static volatile sig_atomic_t stopping;
static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void on_signal(int sig) { (void)sig; stopping = 1; }
static int u16le(const uint8_t *b) { return b[0] | (b[1] << 8); }

struct midi {
    int fd, values[ANALOG_COUNT], sent[ANALOG_COUNT];
    int motor, sent_motor, keys, sent_keys;
    int left_cut, right_cut, sent_edges;
    int performance, sent_mod, sent_bend, center_bend;
    int lamp, sent_lamp;
    struct wow_mix mix;
    struct wow_lamp_state *lamp_state;
    struct wow_keyboard_link *keyboard_link;
    uint8_t buf[2048];
    size_t len;
};
static void invalidate(struct midi *m) {
    for (int i = 0; i < ANALOG_COUNT; i++) m->sent[i] = -1;
    m->sent_motor = m->sent_keys = m->sent_edges = m->sent_lamp = -1;
}
static int queue(struct midi *m, const uint8_t *msg, size_t n) {
    if (sizeof(m->buf) - m->len < n) return 0;
    memcpy(m->buf + m->len, msg, n);
    m->len += n;
    return 1;
}
static int flush(struct midi *m) {
    if (!m->len) return 0;
    ssize_t n = write(m->fd, m->buf, m->len);
    if (n < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    m->len -= (size_t)n;
    memmove(m->buf, m->buf + n, m->len);
    return 0;
}
static void read_cutoffs(struct midi *m) {
    FILE *settings = fopen("/var/nona/wow_settings.txt", "r");
    if (!settings) return;
    char line[160], key[40];
    int value;
    while (fgets(line, sizeof(line), settings)) {
        if (sscanf(line, " %39[a-z_] = %d", key, &value) != 2 || value < 0 || value > 4095) continue;
        if (!strcmp(key, "left_cut_value")) m->left_cut = value;
        else if (!strcmp(key, "right_cut_value")) m->right_cut = value;
    }
    fclose(settings);
}
static void select_profile(struct midi *m, int performance) {
    if (m->performance == performance) return;
    m->center_bend = m->performance && !performance;
    m->performance = performance;
    m->sent_mod = m->sent_bend = -1;
    invalidate(m);
}

static void publish_performance(struct midi *m) {
    if (m->center_bend) {
        uint8_t center[] = {0xe0, 0, 64};
        if (queue(m, center, sizeof(center))) m->center_bend = 0;
    }
    if (!m->performance) return;
    int angle = m->values[ANGLE];
    if (angle >= 0 && (angle >> 7) != m->sent_mod) {
        uint8_t mod[] = {0xb0, 1, angle >> 7};
        if (queue(m, mod, sizeof(mod))) m->sent_mod = angle >> 7;
    }
    int bend = m->values[PITCH];
    if (bend < 0) return;
    if (abs(bend - 8192) <= 48) bend = 8192;
    if (bend < 32) bend = 0;
    if (bend > 16351) bend = 16383;
    if (m->sent_bend < 0 || abs(bend - m->sent_bend) >= 24 ||
        (bend != m->sent_bend && (bend == 8192 || bend == 0 || bend == 16383))) {
        uint8_t msg[] = {0xe0, bend & 127, bend >> 7};
        if (queue(m, msg, sizeof(msg))) m->sent_bend = bend;
    }
}

static void publish(struct midi *m) {
    publish_performance(m);
    int lamp = wow_lamp_midi(m->lamp);
    if (lamp != m->sent_lamp) {
        uint8_t msg[] = {0xb0, 105, lamp & 127,
                         0xb0, 106, (lamp >> 7) & 127,
                         0xb0, 107, (lamp >> 14) & 127,
                         0xb0, 108, (lamp >> 21) & 127};
        if (queue(m, msg, m->lamp < 0 ? 3 : sizeof msg)) m->sent_lamp = lamp;
    }
    if (m->values[FADER_LEFT] >= 0 && m->values[FADER_RIGHT] >= 0) {
        int edges = ((m->values[FADER_LEFT] >> 2) > m->left_cut)
                  | (((m->values[FADER_RIGHT] >> 2) > m->right_cut) << 1);
        if (edges != m->sent_edges) {
            uint8_t msg[] = {0xb0, m->performance ? 104 : 22, edges};
            if (queue(m, msg, sizeof(msg))) m->sent_edges = edges;
        }
    }
    for (int i = 0; i < ANALOG_COUNT; i++) {
        int value = m->values[i];
        if (value < 0 || value == m->sent[i]) continue;
        uint8_t cc = analog_cc[i];
        uint8_t pair[] = {0xb0, cc, value >> 7, 0xb0, cc + 32, value & 127};
        if (queue(m, pair, sizeof(pair))) m->sent[i] = value;
    }
    if (m->motor >= 0 && m->motor != m->sent_motor) {
        uint8_t msg[] = {0xb0, m->performance ? 102 : 20, m->motor ? 127 : 0};
        if (queue(m, msg, sizeof(msg))) m->sent_motor = m->motor;
    }
    if (m->keys != m->sent_keys) {
        uint8_t msg[] = {0xb0, m->performance ? 103 : 21, m->keys};
        if (queue(m, msg, sizeof(msg))) m->sent_keys = m->keys;
    }
}
static void buttons_now(struct midi *m, int evdev) {
    uint8_t keys[(KEY_MAX + 8) / 8] = {0};
    if (ioctl(evdev, EVIOCGKEY(sizeof(keys)), keys) < 0) return;
    m->keys = 0;
    for (size_t i = 0; i < sizeof(key_codes)/sizeof(*key_codes); i++) {
        int key = key_codes[i];
        if (keys[key / 8] & (1u << (key % 8))) m->keys |= 1u << i;
    }
}
static void handle_key(struct midi *m, const struct input_event *ev) {
    if (ev->type != EV_KEY || (ev->value != 0 && ev->value != 1)) return;
    for (size_t i = 0; i < sizeof(key_codes)/sizeof(*key_codes); i++) {
        if (ev->code != key_codes[i]) continue;
        int bit = 1 << i;
        if (!!(m->keys & bit) == ev->value) break;
        m->keys ^= bit;
        uint8_t msg[] = {ev->value ? 0x90 : 0x80, 36 + i, ev->value ? 127 : 0};
        queue(m, msg, sizeof(msg));
        break;
    }
}
static int angle14(int fd) {
    uint8_t tx[] = {0xa0, 0x03, 0, 0, 0, 0}, rx[sizeof(tx)] = {0};
    struct spi_ioc_transfer tr = {
        .tx_buf = (uintptr_t)tx, .rx_buf = (uintptr_t)rx, .len = sizeof(tx),
        .speed_hz = 10000000, .bits_per_word = 8
    };
    if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) != (int)sizeof(tx)) return -1;
    return ((rx[2] << 7) | (rx[3] >> 1)) >> 1;
}
static void read_adc(struct midi *m, const uint8_t reply[11]) {
    const int offsets[ANALOG_COUNT] = {
        [FADER_LEFT]=8, [FADER_RIGHT]=6, [PITCH]=4, [REAR_MASTER]=0, [REAR_BLEND]=2
    };
    int pitch = u16le(reply + 4);
    if (pitch <= 4095) wow_keyboard_pitch(m->keyboard_link, pitch);
    for (int i = FADER_LEFT; i < ANALOG_COUNT; i++) {
        int raw = u16le(reply + offsets[i]);
        if (raw <= 4095) m->values[i] = (raw << 2) | (raw >> 10);
    }
}
static int serial_write(int fd, const uint8_t *data, size_t size) {
    while (size && !stopping) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            struct pollfd p = {.fd=fd, .events=POLLOUT};
            if (poll(&p, 1, 50) > 0) continue;
        }
        if (n <= 0) return -1;
        data += n; size -= (size_t)n;
    }
    return size ? -1 : 0;
}
struct relay {
    int uart, master, slave, configured;
    uint8_t cmd[6], reply[11];
    size_t ncmd, nreply;
    struct wow_mix mix;
    uint32_t mute_until;
    int mute_pid;
    unsigned muted_frames;
    int expecting, received;
};
static int forward_request(struct relay *r, struct midi *m) {
    ssize_t n = read(r->master, r->cmd + r->ncmd, sizeof(r->cmd) - r->ncmd);
    if (n <= 0) return (n < 0 && (errno == EINTR || errno == EAGAIN)) ? 0 : -1;
    r->ncmd += (size_t)n;
    if (r->ncmd < sizeof(r->cmd)) return 0;
    r->ncmd = 0;
    if (!r->configured) {
        struct termios line;
        if (tcgetattr(r->slave, &line) || tcsetattr(r->uart, TCSANOW, &line)) return -1;
        r->configured = 1;
    }
    int valid_frame = r->cmd[5] == '\n' && u16le(r->cmd) <= 1000;
    uint64_t lamp_request = valid_frame ? wow_lamp_override(m->lamp_state, r->cmd) : 0;
    if (tcflush(r->uart, TCIFLUSH) || serial_write(r->uart, r->cmd, sizeof(r->cmd))) return -1;
    r->mix = m->mix;
    /* A stopped beat can be loaded through stock gestures. Silence only its
     * contribution while the adapter performs that operation. A crashed
     * adapter cannot leave a persisted trim or an unbounded mute behind. */
    uint32_t until = 0;
    int owner = 0;
    if (valid_frame && m->keyboard_link) {
        uint32_t requested = atomic_load(&m->keyboard_link->beat_mute_until_ms);
        int engine = atomic_load(&m->keyboard_link->engine_pid);
        int32_t remaining = (int32_t)(requested - (uint32_t)now_ms());
        if (requested && remaining > 0 && remaining <= 30000 && engine > 0 &&
            atomic_load(&m->keyboard_link->beat_mute_pid) == engine) {
            until = requested;
            owner = engine;
            r->mix.beat = 0;
        }
    }
    if (r->mute_until != until || r->mute_pid != owner) r->muted_frames = 0;
    r->mute_until = until;
    r->mute_pid = owner;
    r->nreply = 0; r->expecting = 1;
    if (r->cmd[5] == '\n') m->motor = u16le(r->cmd) != 0;
    if (valid_frame) {
        m->lamp = wow_lamp_rgb(r->cmd);
        wow_lamp_forwarded(m->lamp_state, m->lamp, lamp_request);
    }
    return 0;
}
static int forward_reply(struct relay *r, struct midi *m) {
    uint8_t data[64];
    ssize_t n = read(r->uart, data, sizeof(data));
    if (n <= 0) return (n == 0 || errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (!r->configured) return 0;
    if (!r->expecting) return serial_write(r->master, data, (size_t)n);
    int trim = r->cmd[5] == '\n' && wow_mix_needs_trim(r->mix);
    if (!trim && serial_write(r->master, data, (size_t)n)) return -1;
    size_t remaining = sizeof(r->reply) - r->nreply;
    size_t take = (size_t)n < remaining ? (size_t)n : remaining;
    memcpy(r->reply + r->nreply, data, take); r->nreply += take;
    if (r->nreply == sizeof(r->reply)) {
        read_adc(m, r->reply); r->expecting = 0;
        unsigned volume = u16le(r->reply), balance = u16le(r->reply + 2);
        if (trim) {
            wow_trim_reply(r->reply, r->mix);
            if (serial_write(r->master, r->reply, sizeof r->reply)) return -1;
            if ((size_t)n > take && serial_write(r->master, data + take, (size_t)n - take)) return -1;
        }
        if (r->mute_until) {
            /* Stock averages 32 consecutive control frames. Receipt of the
             * next request proves those 32 frames reached the engine. */
            if (volume > 4095 || balance > 4095) r->muted_frames = 0;
            else if (r->muted_frames < 33 && ++r->muted_frames == 33 &&
                     atomic_load(&m->keyboard_link->beat_mute_until_ms) == r->mute_until &&
                     atomic_load(&m->keyboard_link->beat_mute_pid) == r->mute_pid)
                atomic_store(&m->keyboard_link->beat_mute_ack_ms, r->mute_until);
        }
        if (m->lamp_state && r->cmd[5] == '\n' && volume <= 4095 && balance <= 4095) {
            uint64_t knobs = UINT64_C(1) << 48 | volume | (uint64_t)balance << 12 |
                (uint64_t)u16le(r->reply) << 24 | (uint64_t)u16le(r->reply + 2) << 36;
            atomic_store(&m->lamp_state->mix_knobs, knobs);
            atomic_store(&m->lamp_state->mix_seen_ns, wow_lamp_now_ns());
        }
        if (!r->received) {
            fprintf(stderr, "First control-board reply: pitch %d, left %d, right %d.\n",
                    u16le(r->reply+4), u16le(r->reply+8), u16le(r->reply+6));
            fsync(STDERR_FILENO);
            r->received = 1;
        }
    }
    return 0;
}
static int open_relay(struct relay *r) {
    struct termios line;
    r->uart = open("/dev/ttyS1", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    r->master = posix_openpt(O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (r->uart < 0 || r->master < 0 || grantpt(r->master) || unlockpt(r->master)) return -1;
    char *path = ptsname(r->master);
    if (!path) return -1;
    r->slave = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (r->slave < 0 || tcgetattr(r->slave, &line)) return -1;
    cfmakeraw(&line);
    if (tcsetattr(r->slave, TCSANOW, &line)) return -1;
    if (symlink(path, "/dev/wow-midi-pty") || rename("/dev/wow-midi-pty", "/dev/mcu")) return -1;
    return 0;
}
static int usb_configured(void) {
    char state[24];
    int fd = open("/sys/class/udc/musb-hdrc.1.auto/state", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    ssize_t n = read(fd, state, sizeof(state));
    close(fd);
    return n < 0 || (n >= 10 && !memcmp(state, "configured", 10));
}
static int monitor(struct midi *m, struct relay *r, int spi, int keys,
                   pid_t child, const char *port, int keyboard_command) {
    const char *mode = getenv("WOW_BEAT_CONTROL");
    char permission = mode && !strcmp(mode, "1") ? 'K' : mode && !strcmp(mode, "looper") ? 'L' : 'P';
    /* The reply FIFO may close with a disconnected USB session. */
    signal(SIGPIPE, SIG_IGN);
    int listener = open_beat_listener();
    if (listener < 0) perror("Open beat command pipe");
    struct pollfd fds[] = {{r->master,POLLIN,0}, {r->uart,POLLIN,0},
                           {keys,POLLIN,0}, {listener,POLLIN,0}};
    long long next_angle = 0, next_snapshot = 0, next_usb = 0;
    long long next_cutoffs = 0;
    long long next_profile = 0;
    int dropped = 0;
    while (!stopping) {
        long long now = now_ms();
        if (now >= next_profile) {
            m->mix = wow_mix_settings();
            select_profile(m, wow_performance_profile());
            next_profile = now + 250;
        }
        if (now >= next_cutoffs) {
            read_cutoffs(m);
            next_cutoffs = now + 5000;
        }
        if (now >= next_usb) {
            int attached = usb_configured();
            if (!attached && m->fd >= 0) {
                close(m->fd);
                m->fd = -1;
            } else if (attached && m->fd < 0) {
                m->fd = open(port, O_WRONLY | O_CLOEXEC | O_NONBLOCK);
                if (m->fd >= 0) {
                    invalidate(m);
                    m->sent_bend = m->sent_mod = -1;
                }
            }
            next_usb = now + 100;
        }
        if (now >= next_angle) {
            if (m->fd >= 0) {
                int v = angle14(spi);
                if (v >= 0) m->values[ANGLE] = v;
            }
            next_angle = now + 4;
        }
        if (now >= next_snapshot) {
            if (waitpid(child, NULL, WNOHANG) != 0) return -1;
            invalidate(m); next_snapshot = now + 500;
        }
        if (m->fd >= 0) {
            publish(m);
            if (flush(m) < 0) {
                close(m->fd);
                m->fd = -1;
                m->len = 0;
            }
        } else {
            m->len = 0;
        }
        int ready = poll(fds, sizeof fds / sizeof *fds, (int)(next_angle - now));
        if (ready < 0) { if (errno == EINTR) continue; return -1; }
        if (fds[1].revents & POLLIN && forward_reply(r, m)) return -1;
        if (fds[0].revents & POLLIN && forward_request(r, m)) return -1;
        if (fds[2].revents & POLLIN) {
            struct input_event ev[16];
            ssize_t n = read(keys, ev, sizeof(ev));
            for (ssize_t i = 0; i < n/(ssize_t)sizeof(*ev); i++) {
                if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) dropped = 1;
                else if (dropped && ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
                    buttons_now(m, keys); dropped = 0;
                } else if (!dropped) handle_key(m, &ev[i]);
            }
        }
        if (fds[3].revents & POLLIN)
            handle_beat_request(listener, keyboard_command, m->keyboard_link, r->received, permission);
        for (size_t i = 0; i < 3; i++)
            if (fds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) return -1;
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    struct midi m = {.fd=-1, .motor=-1, .lamp=-1, .sent_mod=-1, .sent_bend=-1, .left_cut=3580, .right_cut=3841};
    for (int i=0; i<ANALOG_COUNT; i++) m.values[i]=-1;
    int spi = open("/dev/magenc", O_RDWR | O_CLOEXEC);
    if (spi < 0) { perror("Open platter encoder"); return 1; }
    int keys = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (keys < 0) { perror("Open deck buttons"); return 1; }
    uint8_t mode=SPI_MODE_3, bits=8;
    if (ioctl(spi, SPI_IOC_WR_MODE, &mode) || ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &bits)) {
        perror("Configure platter encoder"); return 1;
    }
    struct relay relay = {0};
    if (open_relay(&relay)) { perror("Control-board serial setup"); return 1; }
    buttons_now(&m, keys);
    struct keyboard_bridge keyboard = {.command={-1, -1}, .shared=-1};
    const char *adapter = getenv("WOW_KEYBOARD_ADAPTER");
    if (adapter && !access(adapter, R_OK)) open_keyboard_bridge(&keyboard);
    m.keyboard_link = keyboard.link;
    pid_t child = fork();
    if (child < 0) { perror("Start factory engine"); return 1; }
    if (child == 0) {
        setpgid(0,0);
        if (keyboard.command[1] >= 0) close(keyboard.command[1]);
        enable_factory_keyboard(&keyboard);
        int console = open("/dev/console",O_WRONLY);
        if (console >= 0) { dup2(console,STDOUT_FILENO); dup2(console,STDERR_FILENO); close(console); }
        execl("/bin/serve","serve","wowmachine",(char *)NULL);
        _exit(127);
    }
    if (keyboard.command[0] >= 0) close(keyboard.command[0]);
    if (keyboard.shared >= 0) close(keyboard.shared);
    setpgid(child,child);
    fprintf(stderr,"USB MIDI on %s. Factory engine started.\n",argv[1]);
    int lamp_lease = -1;
    m.lamp_state = wow_open_lamp_writer(&lamp_lease);
    int result = monitor(&m,&relay,spi,keys,child,argv[1],keyboard.command[1]);
    if (keyboard.command[1] >= 0) close(keyboard.command[1]);
    if (keyboard.link) munmap(keyboard.link, sizeof *keyboard.link);
    if (m.lamp_state) {
        atomic_store(&m.lamp_state->snapshot, 0);
        munmap(m.lamp_state, sizeof *m.lamp_state);
        close(lamp_lease);
    }
    if (result) perror("Controller stopped");
    kill(-child,SIGTERM);
    for (int i=0; i<20 && waitpid(child,NULL,WNOHANG)==0; i++) usleep(10000);
    if (kill(-child,0)==0) kill(-child,SIGKILL);
    return result ? 1 : 0;
}
