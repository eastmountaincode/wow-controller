#ifndef WOW_PLAYHEAD_H
#define WOW_PLAYHEAD_H

#include <fcntl.h>
#include <sys/time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef WOW_PLAYHEAD_SOCKET
#define WOW_PLAYHEAD_SOCKET "/dev/wow-sample-position"
#endif
#define WOW_ENGINE_RATE 48000u

struct wow_playhead {
    uint32_t frame, length;
    char sample[256];
};
static const struct sockaddr_un wow_playhead_address = {
    .sun_family = AF_UNIX, .sun_path = WOW_PLAYHEAD_SOCKET
};

/* Connecting asks for one snapshot. No request body, filenames or addresses
 * are accepted. The factory-side keyboard loop sleeps until there is demand. */
static inline int wow_playhead_read(struct wow_playhead *value) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int success = 0;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
        connect(fd, (const struct sockaddr *)&wow_playhead_address, sizeof wow_playhead_address) == 0) {
        struct timeval timeout = {.tv_sec=0, .tv_usec=100000};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0 &&
            fcntl(fd, F_SETFL, 0) == 0 &&
            recv(fd, value, sizeof *value, MSG_WAITALL) == sizeof *value)
            success = value->length && value->frame < value->length &&
                      value->sample[0] && !value->sample[255];
    }
    close(fd);
    return success;
}
#endif
