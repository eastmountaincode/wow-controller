#include "wow-beat-ipc.h"
#include "wow-keyboard-link.h"
#include <sys/mman.h>

struct keyboard_bridge {
    int command[2], shared;
    struct wow_keyboard_link *link;
};

static void open_keyboard_bridge(struct keyboard_bridge *b) {
    char path[] = "/dev/.wow-keyboard-XXXXXX";
    b->shared = mkstemp(path);
    if (b->shared < 0) return;
    unlink(path);
    if (ftruncate(b->shared, sizeof *b->link) < 0) goto failed;
    b->link = mmap(NULL, sizeof *b->link, PROT_READ | PROT_WRITE, MAP_SHARED, b->shared, 0);
    if (b->link == MAP_FAILED) { b->link = NULL; goto failed; }
    atomic_init(&b->link->engine_pid, 0);
    atomic_init(&b->link->beat_mute_pid, 0);
    atomic_init(&b->link->beat_mute_until_ms, 0);
    atomic_init(&b->link->beat_mute_ack_ms, 0);
    atomic_init(&b->link->pitch, -1);
    atomic_init(&b->link->pitch_watch, -1);
    if (!pipe2(b->command, O_NONBLOCK | O_CLOEXEC)) return;
    munmap(b->link, sizeof *b->link);
    b->link = NULL;
failed:
    close(b->shared);
    b->shared = -1;
}

static int inherit_keyboard_fd(const char *name, int fd) {
    char value[24];
    snprintf(value, sizeof value, "%d", fd);
    return fcntl(fd, F_SETFD, 0) || setenv(name, value, 1);
}

/* serve may restart wowmachine. Its children inherit the command reader and
 * sensor page; only the actual wowmachine executable opts into keyboard I/O. */
static void enable_factory_keyboard(struct keyboard_bridge *b) {
    const char *adapter = getenv("WOW_KEYBOARD_ADAPTER");
    if (b->command[0] < 0 || !adapter || access(adapter, R_OK)) return;
    if (!inherit_keyboard_fd("WOW_KEYBOARD_COMMAND_FD", b->command[0]) &&
        !inherit_keyboard_fd("WOW_KEYBOARD_SENSOR_FD", b->shared))
        setenv("LD_PRELOAD", adapter, 1);
}

static void handle_beat_request(int listener, int command, struct wow_keyboard_link *link, int engine_ready, char permission) {
    struct beat_request request;
    if (read(listener, &request, sizeof request) != sizeof request) return;
    uint64_t now = beat_time_ns();
    if (!now || now > request.expires_ns) return;
    char result = permission;
    if (permission == 'K') {
        result = 'N';
        int pid = link ? atomic_load(&link->engine_pid) : 0;
        if (engine_ready && command >= 0 && pid > 0 && !kill(pid, 0)) {
            struct wow_keyboard_command msg = {.request=request, .engine_pid=pid};
            ssize_t n;
            do { n = write(command, &msg, sizeof msg); } while (n < 0 && errno == EINTR);
            if (n == sizeof msg) return; /* the factory-side adapter replies */
        }
    }
    char path[PATH_MAX];
    beat_reply_path(path, &request);
    int reply = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (reply >= 0) {
        (void)write(reply, &result, 1);
        close(reply);
    }
}
