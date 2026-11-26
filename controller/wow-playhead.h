#ifndef WOW_PLAYHEAD_H
#define WOW_PLAYHEAD_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WOW_PLAYHEAD_FIFO
#define WOW_PLAYHEAD_FIFO "/dev/wow-sample-position-requests"
#endif
#define WOW_ENGINE_RATE 48000u

enum wow_cursor_status {
    WOW_CURSOR_READY, WOW_CURSOR_OFFLINE, WOW_CURSOR_TIMEOUT, WOW_CURSOR_DECODING,
    WOW_CURSOR_TRACK, WOW_CURSOR_LENGTH, WOW_CURSOR_MOVING
};
struct wow_playhead {
    uint32_t frame, length;
    char sample[256];
    uint32_t status;
};
struct wow_cursor_request { uint32_t pid, sequence; };
_Static_assert(sizeof(struct wow_playhead) <= PIPE_BUF, "A position must fit in one FIFO write");

static inline void wow_cursor_reply_path(char path[PATH_MAX], struct wow_cursor_request request) {
    snprintf(path, PATH_MAX, "%s-%u-%u", WOW_PLAYHEAD_FIFO, request.pid, request.sequence);
}

static inline int wow_cursor_open_fifo(const char *path, int flags) {
    int fd = open(path, flags | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISFIFO(st.st_mode) || st.st_uid != geteuid()) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The deck kernel supports named pipes, including when networking is disabled.
 * The keyboard loop takes a fresh snapshot only when the browser requests one.
 * Each reply goes to one USB request, even if a transfer is interrupted. */
static inline int wow_playhead_read(struct wow_playhead *value) {
    static uint32_t sequence;
    value->status = WOW_CURSOR_OFFLINE;
    int command = wow_cursor_open_fifo(WOW_PLAYHEAD_FIFO, O_WRONLY);
    if (command < 0) return 0;
    struct wow_cursor_request request = {.pid=(uint32_t)getpid(), .sequence=++sequence};
    char path[PATH_MAX];
    wow_cursor_reply_path(path, request);
    int success = 0, reply = -1;
    if (unlink(path) && errno != ENOENT) goto done;
    if (mkfifo(path, 0600)) goto done;
    reply = wow_cursor_open_fifo(path, O_RDWR);
    if (reply < 0) goto cleanup;
    if (write(command, &request, sizeof request) != sizeof request) goto cleanup;
    struct pollfd pending = {.fd=reply, .events=POLLIN};
    value->status = WOW_CURSOR_TIMEOUT;
    if (poll(&pending, 1, 100) > 0 && read(reply, value, sizeof *value) == sizeof *value)
        success = value->status == WOW_CURSOR_READY && value->length && value->frame < value->length &&
                  value->sample[0] && !value->sample[255];
cleanup:
    if (reply >= 0) close(reply);
    unlink(path);
done:
    close(command);
    return success;
}
#endif
