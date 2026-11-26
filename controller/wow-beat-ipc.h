/* The deck kernel provides pipes even with its networking stack disabled.
 * The exclusive library session is the only client; every request gets its
 * own reply FIFO so an abandoned request cannot toggle the next session. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef WOW_BEAT_FIFO
#define WOW_BEAT_FIFO "/dev/wow-beat-command"
#endif

enum { WOW_BEAT_QUERY, WOW_BEAT_PLAY, WOW_BEAT_STOP, WOW_BEAT_TOGGLE,
       WOW_EFFECT_QUERY, WOW_EFFECT_ON, WOW_EFFECT_OFF };

struct beat_request {
    uint64_t expires_ns;
    uint32_t pid;
    uint32_t sequence;
    uint32_t action;
};
_Static_assert(sizeof(struct beat_request) <= PIPE_BUF, "Beat commands must be atomic");

static inline uint64_t beat_time_ns(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return 0;
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static inline void beat_reply_path(char path[PATH_MAX], const struct beat_request *request) {
    snprintf(path, PATH_MAX, "%s-reply-%u-%u", WOW_BEAT_FIFO, request->pid, request->sequence);
}

static inline int open_beat_listener(void) {
    /* Keep both ends open so poll sleeps even when the USB session disconnects. */
    if (mkfifo(WOW_BEAT_FIFO, 0600) < 0 && errno != EEXIST) return -1;
    int fd = open(WOW_BEAT_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISFIFO(st.st_mode)) {
        close(fd); errno = EINVAL; return -1;
    }
    return fd;
}

static inline char request_beat_action(uint32_t action) {
    static uint32_t sequence;
    struct beat_request request = {.pid=(uint32_t)getpid(), .sequence=++sequence, .action=action};
    uint64_t now = beat_time_ns();
    if (!now) return 'I';
    request.expires_ns = now + UINT64_C(1200000000);
    int command = open(WOW_BEAT_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (command < 0) return 'A';
    char path[PATH_MAX], result = 'I';
    beat_reply_path(path, &request);
    /* A reboot can reuse a PID after a previous session lost power. */
    int reply = -1;
    if (unlink(path) && errno != ENOENT) goto done;
    if (mkfifo(path, 0600)) goto done;
    reply = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (reply < 0) goto done;
    ssize_t n;
    do { n = write(command, &request, sizeof request); } while (n < 0 && errno == EINTR);
    if (n != sizeof request) goto done;
    struct pollfd p = {.fd=reply, .events=POLLIN};
    result = 'T';
    int ready;
    do { ready = poll(&p, 1, 1500); } while (ready < 0 && errno == EINTR);
    if (ready > 0 && read(reply, &result, 1) != 1) result = 'I';
done:
    if (reply >= 0) close(reply);
    unlink(path);
    close(command);
    return result;
}
