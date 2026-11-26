#ifndef WOW_PREFERENCES_H
#define WOW_PREFERENCES_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WOW_STATE_DIR
#define WOW_STATE_DIR "/wow-controller"
#endif

/* Names supplied by the firmware only. Every preference fits on one line. */
static inline int wow_read_line(const char *path, char *value, size_t capacity) {
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int valid = fgets(value, capacity, file) != NULL;
    if (valid) {
        size_t length = strcspn(value, "\r\n");
        valid = (value[length] || feof(file)) && length > 0;
        value[length] = 0;
    }
    fclose(file);
    return valid ? 0 : -1;
}

static inline int wow_write_setting(const char *path, const char *value) {
    char temp[] = WOW_STATE_DIR "/setting-XXXXXX";
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    size_t length = strlen(value);
    int failed = write(fd, value, length) != (ssize_t)length || fsync(fd) < 0;
    if (close(fd) < 0) failed = 1;
    if (failed || rename(temp, path) < 0) {
        int saved = errno;
        unlink(temp);
        errno = saved;
        return -1;
    }
    int dir = open(WOW_STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return -1;
    int result = fsync(dir);
    close(dir);
    return result;
}

#define WOW_PROFILE_PATH WOW_STATE_DIR "/midi-profile"

static inline int wow_performance_profile(void) {
    char name[16];
    return !wow_read_line(WOW_PROFILE_PATH, name, sizeof(name)) &&
           (!strcmp(name, "performance") || !strcmp(name, "cicada"));
}
#endif
