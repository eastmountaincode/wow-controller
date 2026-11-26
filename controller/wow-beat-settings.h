/* The factory engine reads these two INI keys at launch. It also writes its
 * in-memory dictionary to the card after calibration. Keep the user's startup
 * choice separately and apply it before launching the engine. */
#define WOW_FACTORY_SETTINGS "wow_settings.txt"
#define WOW_BEAT_PREFERENCE ".wow-beat-startup"

struct beat_settings {
    int startup, looper, preference;
    char *text, *value, *end, *section;
};

static int matches_name(const char *start, const char *end, const char *name) {
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    return (size_t)(end - start) == strlen(name) && !strncasecmp(start, name, end - start);
}

static int ini_boolean(const char *value, const char *end, int fallback) {
    if (value == end) return fallback;
    /* iniparser_getboolean uses the first character of the decoded value. */
    switch (tolower((unsigned char)*value)) {
        case '1': case 't': case 'y': return 1;
        case '0': case 'f': case 'n': return 0;
        default: return fallback;
    }
}

static void parse_beat_settings(struct beat_settings *s) {
    int in_section = 0;
    for (char *row = s->text; *row;) {
        char *next = strchr(row, '\n');
        char *end = next ? next : row + strlen(row);
        next = *end ? end + 1 : end;
        char *start = row;
        while (start < end && isspace((unsigned char)*start)) ++start;
        if (start < end && *start == '[') {
            char *close = memchr(start, ']', end - start);
            in_section = close && matches_name(start + 1, close, "wowmachine");
            if (in_section) s->section = next;
        } else if (in_section) {
            char *eq = memchr(start, '=', end - start);
            int startup = eq && matches_name(start, eq, "beat_play_at_start");
            if (eq && (startup || matches_name(start, eq, "looper_mode"))) {
                char *value = eq + 1;
                while (value < end && (*value == ' ' || *value == '\t')) ++value;
                char *tail = value;
                if (value < end && (*value == '"' || *value == '\'')) {
                    char quote = *value++;
                    tail = value;
                    while (tail < end && *tail != quote) ++tail;
                } else {
                    while (tail < end && *tail != '#' && *tail != ';') ++tail;
                    while (tail > value && isspace((unsigned char)tail[-1])) --tail;
                }
                if (startup) {
                    s->startup = ini_boolean(value, tail, 1);
                    s->value = value;
                    s->end = tail;
                } else {
                    s->looper = ini_boolean(value, tail, 0);
                }
            }
        }
        row = next;
    }
}

static int load_beat_settings(int dir, struct beat_settings *s) {
    *s = (struct beat_settings){.startup=1, .preference=-1};
    int fd = openat(dir, WOW_FACTORY_SETTINGS, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 128 * 1024) {
        close(fd); errno = EINVAL; return -1;
    }
    size_t length = (size_t)st.st_size;
    s->text = malloc(length + 1);
    if (!s->text) { close(fd); return -1; }
    size_t used = 0;
    while (used < length) {
        ssize_t n = read(fd, s->text + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (used != length || memchr(s->text, 0, used)) { errno = EIO; return -1; }
    s->text[used] = 0;
    parse_beat_settings(s);
    fd = openat(dir, WOW_BEAT_PREFERENCE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    char preference[3] = {0};
    ssize_t count = read(fd, preference, sizeof preference);
    close(fd);
    if (count != 2 || !strchr("01", preference[0]) || !preference[0] || preference[1] != '\n') {
        errno = EINVAL; return -1;
    }
    s->preference = preference[0] - '0';
    return 0;
}

static int save_beat_file(int dir, const char *name, const char *data, size_t size) {
    /* open_media serializes writes to this card. Remove any incomplete copy
     * from a previous power loss, keeping the original until the replacement is flushed. */
    const char *temp = ".wow-beat-setting.part";
    if (unlinkat(dir, temp, 0) && errno != ENOENT) return -1;
    int fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int result = 0;
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = -1; break; }
        data += n;
        size -= (size_t)n;
    }
    if (result == 0) result = fsync(fd);
    if (close(fd) < 0) result = -1;
    if (result == 0) result = renameat(dir, temp, dir, name);
    if (result) {
        int saved = errno;
        unlinkat(dir, temp, 0);
        errno = saved;
        return -1;
    }
    return fsync(dir);
}

static int apply_beat_preference(int dir, const struct beat_settings *s) {
    if (s->preference < 0 || s->preference == s->startup) return 0;
    const char *choice = s->preference ? "yes" : "no";
    const char *end = s->text + strlen(s->text);
    const char *from = s->value ? s->value : s->section ? s->section : end;
    const char *to = s->end ? s->end : from;
    const char *eol = strstr(s->text, "\r\n") ? "\r\n" : "\n";
    char insert[80];
    if (s->value) snprintf(insert, sizeof insert, "%s", choice);
    else snprintf(insert, sizeof insert, "%s%s%sbeat_play_at_start = %s%s",
                  from > s->text && from[-1] != '\n' ? eol : "",
                  s->section ? "" : "[wowmachine]", s->section ? "" : eol, choice, eol);
    char *new_text = NULL;
    int size = asprintf(&new_text, "%.*s%s%s", (int)(from - s->text), s->text, insert, to);
    if (size < 0) return -1;
    int result = save_beat_file(dir, WOW_FACTORY_SETTINGS, new_text, (size_t)size);
    free(new_text);
    return result;
}
