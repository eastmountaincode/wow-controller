#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#include "wow-power.h"
#include "wow-lamp.h"
#include "wow-beat-settings.h"
#include "wow-mix.h"
#include "wow-beat-ipc.h"
#include "wow-tracks.h"
#include "wow-playhead.h"
#include "wow-preferences.h"

#ifndef WOW_MEDIA_ROOT
#define WOW_MEDIA_ROOT "/var/nona"
#define REQUIRE_FAT_MOUNT 1
#endif

enum { CHUNK = 24576, ENCODED = (CHUNK / 3) * 4, NAME_BYTES = 255 };
static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char partial[] = "incoming.part";
static unsigned char bytes[CHUNK];
static char encoded[ENCODED + 1], line[ENCODED + 32];
static uint32_t crc_table[256];
static int root = -1, staging = -1, bank[2] = {-1, -1};
static struct {
    int fd, destination, firmware;
    uint64_t size, received;
    uint32_t crc;
    char name[NAME_BYTES + 1];
} upload = { .fd = -1 };

static void ok(void) { puts("OK"); }
static void error(const char *message) { printf("ERR %s\n", message); }
static void system_error(void) { error(strerror(errno)); }

static void init_crc(void) {
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int b = 0; b < 8; ++b)
            c = (c >> 1) ^ ((c & 1) ? UINT32_C(0xedb88320) : 0);
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const unsigned char *data, size_t n) {
    for (size_t i = 0; i < n; ++i)
        crc = (crc >> 8) ^ crc_table[(crc ^ data[i]) & 255];
    return crc;
}

static void encode64(const unsigned char *data, size_t len) {
    char *out = encoded;
    for (size_t i = 0; i < len; i += 3) {
        size_t remain = len - i;
        unsigned w = (unsigned)data[i] << 16;
        if (remain > 1) w |= (unsigned)data[i+1] << 8;
        if (remain > 2) w |= data[i+2];
        *out++ = alphabet[w >> 18];
        *out++ = alphabet[(w >> 12) & 63];
        *out++ = remain > 1 ? alphabet[(w >> 6) & 63] : '=';
        *out++ = remain > 2 ? alphabet[w & 63] : '=';
    }
    *out = 0;
}

static int b64val(char c) {
    const char *p = c ? strchr(alphabet, c) : NULL;
    return p ? (int)(p - alphabet) : -1;
}

static int decode64(const char *s, unsigned char *out, size_t cap) {
    size_t len = strlen(s), n = 0;
    if (!len || len % 4) return -1;
    for (size_t i = 0; i < len; i += 4) {
        int pad2 = s[i+2]=='=', pad3 = s[i+3]=='=';
        int a = b64val(s[i]), b = b64val(s[i+1]);
        int c = pad2 ? 0 : b64val(s[i+2]), d = pad3 ? 0 : b64val(s[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) ||
            ((pad2 || pad3) && i+4 != len) || (pad2 && (b&15)) ||
            (pad3 && !pad2 && (c&3)) || n+3-pad2-pad3 > cap) return -1;
        out[n++] = (unsigned char)((a<<2) | (b>>4));
        if (!pad2) out[n++] = (unsigned char)((b<<4) | (c>>2));
        if (!pad3) out[n++] = (unsigned char)((c<<6) | d);
    }
    return (int)n;
}

static int audio_name(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext || name[0]=='.') return 0;
    return !strcasecmp(ext, ".wav") || !strcasecmp(ext, ".mp3") ||
           !strcasecmp(ext, ".ogg") || !strcasecmp(ext, ".flac") ||
           !strcasecmp(ext, ".asf") || !strcasecmp(ext, ".aac");
}

static int filename(const char *token, char *name, int importing) {
    int len = decode64(token, (unsigned char *)name, NAME_BYTES);
    if (len < 1 || memchr(name, '\0', len)) return 0;
    name[len] = 0;
    if (!audio_name(name)) return 0;
    for (int i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) return 0;
        // The factory decoder constructs its command line using the filename.
        if (importing && (c == '$' || c == 0x60)) return 0;
    }
    const char *ext = strrchr(name, '.');
    return !importing || !strcasecmp(ext, ".wav") || !strcasecmp(ext, ".mp3");
}

static int resolve_bank(const char *name) {
    if (!strcmp(name, "beats")) return bank[0];
    if (!strcmp(name, "samples")) return bank[1];
    return -1;
}

static void cancel(void) {
    if (upload.fd >= 0) {
        close(upload.fd);
        upload.fd = -1;
        unlinkat(staging, partial, 0);
    }
}

static int list_files(int dir) {
    int fd = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR *stream = fdopendir(fd);
    if (!stream) { close(fd); return -1; }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            if (errno) result = -1;
            break;
        }
        if (!audio_name(entry->d_name)) continue;
        struct stat st;
        if (fstatat(dir, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;
            result = -1; break;
        }
        if (!S_ISREG(st.st_mode)) continue;
        encode64((const unsigned char *)entry->d_name, strlen(entry->d_name));
        printf("FILE %" PRIu64 " %s\n", (uint64_t)st.st_size, encoded);
    }
    int saved = errno;
    closedir(stream);
    errno = saved;
    return result;
}

static void download(int dir, const char *name) {
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { system_error(); return; }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        error("Cannot read audio file"); close(fd); return;
    }
    uint64_t remaining = (uint64_t)st.st_size;
    uint32_t crc = UINT32_MAX;
    printf("SIZE %" PRIu64 "\n", remaining);
    while (remaining) {
        ssize_t n = read(fd, bytes, remaining < CHUNK ? (size_t)remaining : CHUNK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error("Audio file could not be read in full"); close(fd); return; }
        encode64(bytes, (size_t)n);
        if (printf("DATA %s\n", encoded) < 0) { close(fd); return; }
        crc = crc_update(crc, bytes, (size_t)n);
        remaining -= (size_t)n;
    }
    close(fd);
    printf("END %08" PRIx32 "\n", crc ^ UINT32_MAX);
}

static int parse_size(const char *value, uint64_t limit, uint64_t *out) {
    if (!*value || value[strspn(value, "0123456789")]) return 0;
    errno = 0;
    unsigned long long n = strtoull(value, NULL, 10);
    if (errno || !n || n > limit) return 0;
    *out = n;
    return 1;
}

static void begin(int dir, const char *name, const char *size, int firmware) {
    uint64_t n;
    if (!parse_size(size, firmware ? (8u << 20) : UINT32_MAX, &n)) {
        error("File size is out of range"); return;
    }
    struct stat st;
    if (!firmware) {
        if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0) { error("File already exists"); return; }
        if (errno != ENOENT) { system_error(); return; }
    }
    if (staging < 0) {
        if (mkdirat(root,".wow-library-staging",0700) < 0 && errno != EEXIST) {
            system_error(); return;
        }
        staging = openat(root,".wow-library-staging",O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staging < 0) { system_error(); return; }
    }
    struct statvfs space;
    if (fstatvfs(staging, &space) < 0) { system_error(); return; }
    if (n > (uint64_t)space.f_frsize * space.f_bavail) { error("Insufficient space on card"); return; }
    int fd = openat(staging, partial, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { system_error(); return; }
    upload.fd = fd;
    upload.destination = dir;
    upload.firmware = firmware;
    upload.size = n;
    upload.received = 0;
    upload.crc = UINT32_MAX;
    strcpy(upload.name, name);
    ok();
}

static void append(const char *data) {
    int n = decode64(data, bytes, CHUNK);
    if (n < 1 || (uint64_t)n > upload.size - upload.received) {
        error("Invalid data chunk or file size"); cancel(); return;
    }
    int done = 0;
    while (done < n) {
        ssize_t written = write(upload.fd, bytes + done, (size_t)(n-done));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { error("Could not write file data"); cancel(); return; }
        done += (int)written;
    }
    upload.received += (unsigned)n;
    upload.crc = crc_update(upload.crc, bytes, (size_t)n);
    ok();
}

static int move_new(int from, const char *old, int to, const char *name) {
#ifdef __APPLE__
    return renameatx_np(from, old, to, name, RENAME_EXCL);
#else
    return (int)syscall(SYS_renameat2, from, old, to, name, RENAME_NOREPLACE);
#endif
}

static int audio_header_ok(void) {
    unsigned char h[12];
    ssize_t n = pread(upload.fd, h, sizeof(h), 0);
    const char *ext = strrchr(upload.name, '.');
    if (!strcasecmp(ext, ".wav"))
        return n == 12 && !memcmp(h,"RIFF",4) && !memcmp(h+8,"WAVE",4);
    return n >= 3 && (!memcmp(h,"ID3",3) || (h[0]==0xff && (h[1]&0xe0)==0xe0));
}

/* Some FAT cards have returned success from the staging directory before the
 * moved entry's final size was durable. Verify the named update the bootloader
 * will open and flush the media filesystem before allowing a power cycle. */
static int verify_staged_update(void) {
    int fd = openat(root, "WOWCTRL.UPD", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    uint64_t total = 0;
    uint32_t crc = UINT32_MAX;
    ssize_t n;
    for (;;) {
        n = read(fd, bytes, sizeof bytes);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        total += (size_t)n;
        crc = crc_update(crc, bytes, (size_t)n);
    }
    int valid = n == 0 && total == upload.size && crc == upload.crc && fsync(fd) == 0;
    if (close(fd)) valid = 0;
#ifdef __linux__
    if (valid && syncfs(root) != 0) valid = 0;
#endif
    return valid ? 0 : -1;
}

static void commit(const char *crc) {
    if (strlen(crc)!=8 || strspn(crc,"0123456789abcdefABCDEF")!=8 ||
        upload.size != upload.received || strtoul(crc,NULL,16) != (upload.crc ^ UINT32_MAX)) {
        error("Length or checksum mismatch"); cancel(); return;
    }
    if (!upload.firmware && !audio_header_ok()) {
        error("Audio header does not match WAV or MP3 extension"); cancel(); return;
    }
    if (fsync(upload.fd) < 0) { system_error(); cancel(); return; }
    if (close(upload.fd) < 0) {
        upload.fd = -1; system_error(); unlinkat(staging,partial,0); return;
    }
    upload.fd = -1;
    int moved = upload.firmware ? renameat(staging,partial,upload.destination,upload.name)
                                : move_new(staging,partial,upload.destination,upload.name);
    if (moved < 0) { system_error(); unlinkat(staging,partial,0); return; }
    if (fsync(upload.destination) < 0 || fsync(staging) < 0) {
        error("File installed; card could not confirm metadata flush"); return;
    }
    if (upload.firmware && verify_staged_update() < 0) {
        error("The card could not verify the staged update. Load the update again before restarting.");
        return;
    }
    ok();
}

static void remove_audio(int dir, const char *name) {
    struct stat st;
    if (fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW) < 0) { system_error(); return; }
    if (!S_ISREG(st.st_mode)) { error("Expected an audio file"); return; }
    if (unlinkat(dir,name,0) < 0) { system_error(); return; }
    if (fsync(dir) < 0) { error("Removed file; card could not confirm metadata flush"); return; }
    ok();
}

#define WOW_VERSION "1.5.1"
#ifndef WOW_BUILD_UTC
#define WOW_BUILD_UTC "development"
#endif

static int release_id(char id[15]) {
    const char *location = getenv("WOW_BIN");
    int dir = location ? open(location, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    int fd = dir < 0 ? -1 : openat(dir, "release-id", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (dir >= 0) close(dir);
    if (fd < 0) { strcpy(id, "baseline"); return 0; }
    struct stat st;
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) n = read(fd, id, 14);
    close(fd);
    if (n == 13 && id[12] == '\n') --n;
    if (n != 12) return -1;
    id[n] = 0;
    return strspn(id, "0123456789abcdefABCDEF") == 12 ? 0 : -1;
}

static void version(void) {
    char id[15];
    if (release_id(id)) error("Invalid installed release ID");
    else printf("OK %s\n", id);
}

static void lamp_status(void) {
    struct wow_lamp_state *state = wow_open_lamp_client(0);
    uint64_t snapshot = state ? atomic_load(&state->snapshot) : 0;
    if (state) munmap(state, sizeof *state);
    if (!(snapshot & WOW_LAMP_KNOWN)) {
        puts("LAMP {\"bits\":127,\"source\":\"unknown\",\"rgb\":null,\"test\":null,\"remainingMs\":0}");
        return;
    }
    int rgb = snapshot & 0xffffff;
    int mode = (snapshot >> 24) & 7;
    printf("LAMP {\"bits\":%d,\"source\":\"factory-uart\",\"rgb\":[%d,%d,%d],\"test\":",
           wow_lamp_palette(rgb), rgb >> 16, (rgb >> 8) & 255, rgb & 255);
    if (mode > 0 && mode <= LAMP_OFF) printf("\"%s\"", wow_lamp_names[mode]);
    else fputs("null", stdout);
    printf(",\"remainingMs\":%d}\n", wow_lamp_remaining_ms(snapshot));
}

static void lamp_command(char **v, int n) {
    int mode = LAMP_FACTORY;
    if (n == 3 && !strcmp(v[1], "TEST")) {
        for (mode = LAMP_RED; mode <= LAMP_OFF; ++mode)
            if (!strcmp(v[2], wow_lamp_names[mode])) break;
        if (mode > LAMP_OFF) { error("Choose red, green, blue, white or off"); return; }
    } else if (n != 2 || strcmp(v[1], "RESET")) {
        error("Unknown lamp command"); return;
    }
    struct wow_lamp_state *state = wow_open_lamp_client(1);
    if (!state) { error("The deck lamp controller is offline"); return; }
    uint64_t now = wow_lamp_now_ns();
    int confirmed = 0;
    if (now) {
        uint64_t expiry = now + UINT64_C(1000000) * WOW_LAMP_TEST_MS;
        uint64_t request = (expiry << 3) | (unsigned)mode;
        atomic_store(&state->request, request);
        /* Wait for an actual forwarded frame, usually one serial exchange. */
        for (int i = 0; i < 100; ++i) {
            if (atomic_load(&state->applied) == request) { confirmed = 1; break; }
            const struct timespec pause = {.tv_nsec=5000000};
            nanosleep(&pause, NULL);
        }
        if (!confirmed) atomic_compare_exchange_strong(&state->request, &request, 0);
    }
    munmap(state, sizeof *state);
    if (confirmed) ok(); else error("The deck did not confirm the lamp command");
}

#define CONFIRM_PATH WOW_STATE_DIR "/last-confirmed"

static void release_info(const char *expected, const char *time) {
    char id[15], saved[48], stored_id[15], stamp[16];
    if (release_id(id)) { error("Invalid installed release ID"); return; }
    uint64_t confirmed = 0;
    if (!wow_read_line(CONFIRM_PATH, saved, sizeof(saved)) &&
        sscanf(saved, "%14s %15s", stored_id, stamp) == 2 && !strcmp(id, stored_id))
        parse_size(stamp, UINT64_C(4102444799), &confirmed);
    if (expected) {
        uint64_t seconds = 0;
        if (strlen(expected) != 12 || strcmp(expected, id) ||
            !parse_size(time, UINT64_C(4102444799), &seconds) || seconds < 1577836800) {
            error("Confirmation must match the running release and a valid date"); return;
        }
        if (!confirmed) {
            snprintf(saved, sizeof(saved), "%s %" PRIu64 "\n", id, seconds);
            if (wow_write_setting(CONFIRM_PATH, saved) < 0) { system_error(); return; }
        }
        ok();
        return;
    }
    char receipt[24] = "null";
    if (confirmed) snprintf(receipt, sizeof(receipt), "%" PRIu64, confirmed);
    printf("RELEASE {\"version\":\"" WOW_VERSION "\",\"builtAt\":\"" WOW_BUILD_UTC
           "\",\"confirmedAt\":%s}\n", receipt);
}

static void midi_profile(const char *value, int legacy) {
    const char *mapping = legacy ? "cicada" : "performance";
    int current = wow_performance_profile();
    if (value) {
        if (strcmp(value, "raw") && strcmp(value, mapping)) { error("Unknown MIDI mapping"); return; }
        int desired = !strcmp(value, mapping);
        /* Keep the on-card token readable by the built-in rescue release. */
        if (desired != current && wow_write_setting(WOW_PROFILE_PATH, desired ? "cicada\n" : "raw\n") < 0) {
            system_error(); return;
        }
        current = desired;
    }
    printf("OK %s\n", current ? mapping : "raw");
}

static void beat_error(char result) {
    switch (result) {
        case 'H': error("Release the deck buttons and try again"); break;
        case 'L': error("Beat transport is unavailable in looper mode"); break;
        case 'N': case 'A': error("The deck's playback controls are starting"); break;
        case 'T': error("The deck did not acknowledge the beat command"); break;
        case 'U': error("The deck could not confirm playback yet"); break;
        default: error("The deck's playback controls are unavailable"); break;
    }
}

static void beat_command(char **v, int n) {
    struct beat_settings settings;
    if (load_beat_settings(root, &settings)) {
        free(settings.text);
        error("Cannot read the deck's beat settings");
        return;
    }
    int startup = settings.preference >= 0 ? settings.preference : settings.startup;
    int looper = settings.looper;
    free(settings.text);
    unsigned action = WOW_BEAT_QUERY;
    if (n == 3 && !strcmp(v[1], "STARTUP") && (!strcmp(v[2], "0") || !strcmp(v[2], "1"))) {
        if (settings.preference < 0 || startup != v[2][0] - '0') {
            char choice[] = {v[2][0], '\n'};
            if (save_beat_file(root, WOW_BEAT_PREFERENCE, choice, sizeof choice)) {
                error("Could not save the startup preference");
                return;
            }
            startup = v[2][0] - '0';
        }
    } else if (n == 2 && !strcmp(v[1], "PLAY")) action = WOW_BEAT_PLAY;
    else if (n == 2 && !strcmp(v[1], "STOP")) action = WOW_BEAT_STOP;
    else if (n == 2 && !strcmp(v[1], "TOGGLE")) action = WOW_BEAT_TOGGLE;
    else if (n != 1) { error("Unknown beat command"); return; }

    signal(SIGPIPE, SIG_IGN);
    char result = looper ? 'L' : request_beat_action(action);
    int measured = result == 'p' || result == 's';
    int playing = result == 'p' || result == '1';
    int known = measured || result == '0' || result == '1';
    if (action != WOW_BEAT_QUERY && !known &&
        !(action == WOW_BEAT_TOGGLE && result == 'K')) {
        beat_error(result);
        return;
    }
    if (action == WOW_BEAT_TOGGLE) { ok(); return; }
    printf("BEAT {\"startup\":%s,\"looper\":%s,\"playing\":%s,\"source\":%s}\n",
           startup ? "true" : "false", looper ? "true" : "false",
           known ? (playing ? "true" : "false") : "null",
           measured ? "\"mixer\"" : known ? "\"controls\"" : "null");
}

static void effect_command(char **v, int n) {
    unsigned action = WOW_EFFECT_QUERY;
    if (n == 2 && !strcmp(v[1], "ON")) action = WOW_EFFECT_ON;
    else if (n == 2 && !strcmp(v[1], "OFF")) action = WOW_EFFECT_OFF;
    else if (n != 1) { error("Choose EFFECT ON or OFF"); return; }
    signal(SIGPIPE, SIG_IGN);
    char state = request_beat_action(action);
    if (state < 'a' || state > 'f') {
        if (action == WOW_EFFECT_QUERY) puts("EFFECT {\"enabled\":null,\"mode\":null}");
        else error(state == 'H' ? "The deck is loading a track" : "The deck could not confirm the effect");
        return;
    }
    const char *modes[] = {"reverb", "delay", "both"};
    printf("EFFECT {\"enabled\":%s,\"mode\":\"%s\"}\n",
           ((state - 'a') % 2) ? "true" : "false", modes[(state - 'a') / 2]);
}

static void mix_command(char **v, int n) {
    struct wow_mix mix = wow_mix_settings();
    if (n != 1) {
        int *setting = NULL;
        if (n == 3) {
            if (!strcmp(v[1], "BEAT")) setting = &mix.beat;
            else if (!strcmp(v[1], "SAMPLE")) setting = &mix.sample;
            else if (!strcmp(v[1], "MASTER")) setting = &mix.master;
            else if (!strcmp(v[1], "BLEND")) setting = &mix.blend_enabled;
        }
        if (!setting) { error("Choose BEAT, SAMPLE, MASTER or BLEND and a value"); return; }
        int desired = wow_mix_percent(v[2]);
        if (setting == &mix.blend_enabled && (desired < 0 || desired > 1)) {
            error("Choose 0 or 1 for the rear blend knob"); return;
        }
        if (desired < 0) { error("Choose a volume from 0 to 100"); return; }
        if (*setting != desired) {
            *setting = desired;
            char value[32];
            snprintf(value, sizeof value, "%d %d %d %d\n", mix.beat, mix.sample, mix.master, mix.blend_enabled);
            if (wow_write_setting(WOW_MIX_PATH, value)) { error("Could not save deck levels"); return; }
        }
    }
    struct wow_lamp_state *live = wow_open_lamp_client(0);
    uint64_t knobs = 0;
    if (live) {
        uint64_t seen = atomic_load(&live->mix_seen_ns), now = wow_lamp_now_ns();
        if (seen && now >= seen && now - seen <= UINT64_C(1500000000)) knobs = atomic_load(&live->mix_knobs);
        munmap(live, sizeof *live);
    }
    printf("MIX {\"beat\":%d,\"sample\":%d,\"master\":%d,\"blendEnabled\":%s",
           mix.beat, mix.sample, mix.master, mix.blend_enabled ? "true" : "false");
    if (knobs) {
        unsigned master = (unsigned)(knobs & 4095), blend = (unsigned)((knobs >> 12) & 4095);
        printf(",\"rear\":[%u,%u],\"effective\":[%u,%u],\"rearMaster\":%d,\"rearBlend\":%d",
               master, blend, (unsigned)((knobs >> 24) & 4095), (unsigned)((knobs >> 36) & 4095),
               (int)(100 * wow_knob_level(master) + 0.5f), (int)(100 * wow_knob_level(blend) + 0.5f));
    } else fputs(",\"rearMaster\":null,\"rearBlend\":null", stdout);
    puts("}");
}

/* Send names in the same lossless encoding as LIST; quotes and non-ASCII
 * names cannot change the structure of a response. */
static void print_track_key(const char *name) {
    if (!*name) { fputs("null", stdout); return; }
    encode64((const unsigned char *)name, strlen(name));
    printf("\"%s\"", encoded);
}

static void playhead(void) {
    signal(SIGPIPE, SIG_IGN);
    struct wow_playhead value = {0};
    if (!wow_playhead_read(&value)) {
        static const char *const reasons[] = {"invalid", "offline", "timeout", "decoding", "track", "length", "changing"};
        const char *reason = value.status < sizeof reasons / sizeof *reasons ? reasons[value.status] : "invalid";
        printf("PLAYHEAD {\"sample\":null,\"seconds\":null,\"duration\":null,\"reason\":\"%s\"}\n", reason);
        return;
    }
    fputs("PLAYHEAD {\"sample\":", stdout);
    print_track_key(value.sample);
    printf(",\"seconds\":%.6f,\"duration\":%.6f}\n",
           (double)value.frame / WOW_ENGINE_RATE, (double)value.length / WOW_ENGINE_RATE);
}

static void tracks(void) {
    char samples[256] = {0}, beats[256] = {0};
    wow_tracks_current(samples, beats);
    fputs("TRACKS {\"samples\":", stdout);
    print_track_key(samples);
    fputs(",\"beats\":", stdout);
    print_track_key(beats);
    puts("}");
}

static void load_track(int dir, const char *name) {
    struct stat st;
    if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_size == 0) {
        error("The selected file is missing or empty");
        return;
    }
    if (strpbrk(name, "$`")) {
        error("Rename this file to remove the dollar sign or backtick before loading it on the WOW");
        return;
    }
    signal(SIGPIPE, SIG_IGN);
    switch (wow_track_load(dir == bank[1], name)) {
        case 'K': tracks(); break;
        case 'H': error("Release the deck buttons and try loading again"); break;
        case 'T': error("The deck did not confirm the track before the timeout"); break;
        case 'M': error("The deck could not mute the beat for loading"); break;
        case 'Z': error("The selected file did not appear in the deck's library scan"); break;
        case 'V': error("The deck could not confirm the file's position in its library"); break;
        case 'D': error("The deck's audio decoder could not confirm this file"); break;
        case 'P': error("The deck could not confirm the beat playback state after loading"); break;
        case 'L': error("Track selection is unavailable in looper mode"); break;
        case 'F': error("The deck could not open the selected file"); break;
        case 'N': error("The deck's audio engine is starting"); break;
        default: error("The audio engine could not confirm the selected track"); break;
    }
}

static void command(char **v, int n) {
    if (!n) { error("Empty command"); return; }
    if (n == 1 && !strcmp(v[0],"HELLO")) { cancel(); puts("OK WOW-LIBRARY 1"); return; }
    if (n == 1 && !strcmp(v[0],"ABORT")) { cancel(); ok(); return; }
    if (n == 1 && !strcmp(v[0],"VERSION")) { version(); return; }
    if (n == 1 && !strcmp(v[0],"LAMP")) { lamp_status(); return; }
    if (n == 1 && !strcmp(v[0],"POWER")) {
        char json[512];
        if (wow_power_json(json,sizeof(json)) == 0) printf("POWER %s\n",json);
        else error("Power status unavailable");
        return;
    }
    if (upload.fd >= 0) {
        if (n == 2 && !strcmp(v[0],"DATA")) append(v[1]);
        else if (n == 2 && !strcmp(v[0],"COMMIT")) commit(v[1]);
        else error("An upload is already in progress");
        return;
    }
    if (!strcmp(v[0],"LAMP")) { lamp_command(v, n); return; }
    if (n == 1 && !strcmp(v[0],"RELEASE")) { release_info(NULL, NULL); return; }
    if (n == 3 && !strcmp(v[0],"CONFIRMED")) { release_info(v[1], v[2]); return; }
    if ((n == 1 || n == 2) && (!strcmp(v[0],"MAPPING") || !strcmp(v[0],"PROFILE"))) {
        midi_profile(n == 2 ? v[1] : NULL, !strcmp(v[0],"PROFILE")); return;
    }
    if (n == 1 && !strcmp(v[0],"PLAYHEAD")) { playhead(); return; }
    if (n == 1 && !strcmp(v[0],"TRACKS")) { tracks(); return; }
    if (!strcmp(v[0],"MIX")) { mix_command(v, n); return; }
    if (!strcmp(v[0],"BEAT")) { beat_command(v, n); return; }
    if (!strcmp(v[0],"EFFECT")) { effect_command(v, n); return; }
    if (n == 2 && !strcmp(v[0],"STAGE")) { begin(root,"WOWCTRL.UPD",v[1],1); return; }
    int dir = n >= 2 ? resolve_bank(v[1]) : -1;
    if (dir < 0) { error("Unknown command or bank"); return; }
    if (n == 2 && !strcmp(v[0],"LIST")) {
        if (list_files(dir) < 0) system_error(); else ok();
        return;
    }
    int importing = !strcmp(v[0],"PUT");
    char name[NAME_BYTES + 1];
    if (n < 3 || !filename(v[2],name,importing)) { error("Unsupported or invalid filename"); return; }
    if (n == 4 && importing) begin(dir,name,v[3],0);
    else if (n == 3 && !strcmp(v[0],"LOAD")) load_track(dir,name);
    else if (n == 3 && !strcmp(v[0],"GET")) download(dir,name);
    else if (n == 3 && !strcmp(v[0],"DELETE")) remove_audio(dir,name);
    else error("Unknown command");
}

static int open_media(void) {
    root = open(WOW_MEDIA_ROOT,O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0 || flock(root,LOCK_EX | LOCK_NB) < 0) return -1;
#if defined(__linux__) && defined(REQUIRE_FAT_MOUNT)
    struct statfs fs;
    if (fstatfs(root,&fs) < 0) return -1;
    if (fs.f_type != 0x4d44) { errno=ENODEV; return -1; }
#endif
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    bank[0]=openat(root,"beats",flags);
    bank[1]=openat(root,"samples",flags);
    if (bank[0] < 0 || bank[1] < 0) return -1;
    staging=openat(root,".wow-library-staging",flags);
    if (staging < 0) return errno == ENOENT ? 0 : -1;
    if (unlinkat(staging,partial,0) < 0 && errno!=ENOENT) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--prepare-beat")) {
        if (open_media() < 0) { perror("Open media for beat preference"); return 1; }
        struct beat_settings settings;
        int result = load_beat_settings(root, &settings);
        if (!result) result = apply_beat_preference(root, &settings);
        if (result) perror("Prepare deck beat preference");
        else printf("%d\n", settings.preference >= 0 ? settings.preference : settings.startup);
        free(settings.text);
        return result ? 1 : settings.looper ? 3 : 0;
    }
    if (argc != 2 || strcmp(argv[1],"--stdio")) return 2;
    if (open_media() < 0) { error("Cannot access deck media"); return 1; }
    init_crc();
    while (fgets(line,sizeof(line),stdin)) {
        char *end=strchr(line,'\n');
        if (!end) {
            int c; while ((c=getchar()) != EOF && c!='\n') {}
            cancel(); error("Invalid or oversized command");
        } else {
            if (end > line && end[-1]=='\r') --end;
            *end=0;
            char *v[5], *state=NULL;
            int n=0;
            for (char *s=strtok_r(line," ",&state); s && n<5; s=strtok_r(NULL," ",&state)) v[n++]=s;
            command(v,n);
        }
        if (fflush(stdout)==EOF) break;
    }
    cancel();
    return 0;
}
