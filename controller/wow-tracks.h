#ifndef WOW_TRACKS_H
#define WOW_TRACKS_H
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef WOW_TRACK_FIFO
#define WOW_TRACK_FIFO "/dev/wow-track-command"
#endif
#ifndef WOW_TRACK_STATUS
#define WOW_TRACK_STATUS "/dev/wow-current-tracks"
#endif
#define WOW_TRACK_TIMEOUT 20000
enum { TRACK_BEAT, TRACK_SAMPLE, TRACK_BANKS };
struct wow_track_status { int32_t pid; char name[2][256]; };
struct wow_track_request {
    uint64_t until_ms;
    uint32_t pid, seq, bank;
    int32_t engine_pid;
    char name[256];
};
_Static_assert(sizeof(struct wow_track_request) <= PIPE_BUF, "Atomic selection request");

static inline uint64_t wow_track_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static inline int wow_track_status_read(struct wow_track_status *state) {
    int fd = open(WOW_TRACK_STATUS, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, state, sizeof *state);
    close(fd);
    return n == sizeof *state && state->pid > 1 && kill(state->pid, 0) == 0 &&
           !state->name[0][255] && !state->name[1][255];
}

static inline int wow_tracks_current(char samples[256], char beats[256]) {
    struct wow_track_status state;
    samples[0] = beats[0] = 0;
    if (!wow_track_status_read(&state)) return 0;
    strcpy(samples, state.name[TRACK_SAMPLE]);
    strcpy(beats, state.name[TRACK_BEAT]);
    return 1;
}

static inline void wow_track_reply_name(char path[PATH_MAX], const struct wow_track_request *req) {
    snprintf(path, PATH_MAX, "%s-%u-%u", WOW_TRACK_FIFO, req->pid, req->seq);
}

static inline char wow_track_load(int sample_bank, const char *filename) {
    static uint32_t seq;
    size_t len = strnlen(filename, 256);
    if (!len || len >= 256) return 'F';
    struct wow_track_status state;
    if (!wow_track_status_read(&state)) return 'N';
    struct wow_track_request req = {.engine_pid=state.pid,.until_ms=wow_track_ms()+WOW_TRACK_TIMEOUT,
        .pid=(uint32_t)getpid(), .seq=++seq, .bank=sample_bank ? TRACK_SAMPLE : TRACK_BEAT};
    memcpy(req.name, filename, len+1);
    int command = open(WOW_TRACK_FIFO, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (command < 0) return 'N';
    char path[PATH_MAX], result = 'N';
    wow_track_reply_name(path, &req);
    int reply = -1;
    if (unlink(path) < 0 && errno != ENOENT) goto done;
    if (mkfifo(path, 0600)) goto done;
    reply = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (reply < 0 || write(command, &req, sizeof req) != sizeof req) goto done;
    struct pollfd p = {.fd=reply,.events=POLLIN};
    for (;;) {
        uint64_t now = wow_track_ms();
        result = 'T';
        if (now >= req.until_ms+200) break;
        int ready = poll(&p, 1, (int)(req.until_ms+200-now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || read(reply,&result,1) != 1) result = 'T';
        break;
    }
done:
    if (reply >= 0) close(reply);
    close(command);
    unlink(path);
    return result;
}
#endif
