/* Capture the factory's own selection diagnostics and the result of its PCM
 * decoder. Track names are committed only by the worker that read audio. */
#include "wow-tracks.h"
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/wait.h>

#ifndef WOW_TRACK_MEDIA
#define WOW_TRACK_MEDIA "/var/nona"
#endif
#ifndef WOW_NATIVE_IDS
#define WOW_NATIVE_IDS "/var/wow"
#endif
static const char *const track_dirs[2] = {WOW_TRACK_MEDIA "/beats", WOW_TRACK_MEDIA "/samples"};
struct factory_track {
    char name[256];
    int index, decoded;
    unsigned gen;
    uint32_t frames, audio_object, previous_audio;
};
static struct factory_track factory_tracks[2];
static pthread_mutex_t track_lock = PTHREAD_MUTEX_INITIALIZER;
static int track_listener = -1, track_enabled;
static struct stat track_dirs_stat[2];
static atomic_int filter_mode;
static int filter_bank;
static char filter_name[256];
enum { TRACK_ALL, TRACK_ONLY, TRACK_THROUGH };
static _Thread_local DIR *directory_finished;
static _Thread_local struct { unsigned gen; int bank, audio; uint64_t frames; FILE *stream; } decoder;
static int (*factory_vprintf)(int, const char *, va_list);
static size_t (*factory_fread)(void *,size_t,size_t,FILE *);
static int (*factory_pclose)(FILE *);
static struct dirent64 *(*factory_readdir)(DIR *);

#include "wow-keyboard-playhead.h"

static void track_public_snapshot(void) {
    /* Caller holds track_lock. Rename makes the pair coherent for library
     * sessions; the snapshot lives on devtmpfs and never writes the SD card. */
    struct wow_track_status state = {.pid=getpid()};
    for (int i=0; i<2; i++)
        if (factory_tracks[i].decoded == 1) strcpy(state.name[i],factory_tracks[i].name);
    char tmp[] = WOW_TRACK_STATUS "-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    int success = write(fd,&state,sizeof state)==sizeof state;
    close(fd);
    if (!success || rename(tmp,WOW_TRACK_STATUS)) unlink(tmp);
}

static void track_symbols(void) {
    factory_vprintf = dlsym(RTLD_NEXT,"__vprintf_chk");
    factory_fread = dlsym(RTLD_NEXT,"fread");
    factory_pclose = dlsym(RTLD_NEXT,"pclose");
    factory_readdir = dlsym(RTLD_NEXT,"readdir64");
}

static void track_start(void) {
    if (!factory_vprintf || !factory_fread || !factory_pclose || !factory_readdir) return;
    if (stat(track_dirs[0],&track_dirs_stat[0]) || stat(track_dirs[1],&track_dirs_stat[1])) return;
    if (mkfifo(WOW_TRACK_FIFO,0600) && errno != EEXIST) return;
    track_listener = open(WOW_TRACK_FIFO,O_RDWR|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if (track_listener < 0 || fstat(track_listener,&st) || !S_ISFIFO(st.st_mode)) {
        if (track_listener >= 0) close(track_listener);
        track_listener = -1;
        return;
    }
    track_enabled = 1;
    track_public_snapshot();
}

static int track_path(const char *path, const char **name) {
    for (int bank=0; bank<2; bank++) {
        size_t n = strlen(track_dirs[bank]);
        if (!strncmp(path,track_dirs[bank],n) && path[n]=='/' && path[n+1] &&
            !strchr(path+n+1,'/') && strlen(path+n+1) < 256) {
            *name = path+n+1;
            return bank;
        }
    }
    return -1;
}

static void track_diagnostic(const char *fmt, va_list args) {
    int sample = !strcmp(fmt,"[DBG] load sample %d: %s\n");
    int beat = !strcmp(fmt,"[DBG] load beat %d: %s\n");
    if (!sample && !beat && strcmp(fmt,"[DBG] loadTrack Thread %s\n")) return;
    int index = sample || beat ? va_arg(args,int) : -1;
    const char *path = va_arg(args,const char *), *name = NULL;
    int bank = track_path(path,&name);
    if (bank < 0) return;
    pthread_mutex_lock(&track_lock);
    struct factory_track *slot = &factory_tracks[bank];
    if ((sample || beat) && bank == sample) {
        if (bank == TRACK_SAMPLE) {
            /* loadSample logs *before* allocating the new track, and keeps
             * the previous object alive until the replacement is installed. */
            struct cursor_track current;
            slot->previous_audio = cursor_objects(&current) ? current.audio : slot->audio_object;
            slot->audio_object = 0;
            slot->frames = 0;
        }
        strcpy(slot->name,name);
        slot->index = index;
        slot->decoded = 0;
        ++slot->gen;
        track_public_snapshot();
    } else if (!sample && !beat && !strcmp(slot->name,name)) {
        decoder.bank = bank;
        decoder.gen = slot->gen;
        decoder.audio = 0;
        decoder.frames = 0;
        decoder.stream = NULL;
    }
    pthread_mutex_unlock(&track_lock);
}

int __printf_chk(int flag, const char *fmt, ...) {
    va_list args;
    va_start(args,fmt);
    if (track_enabled) {
        va_list copy;
        va_copy(copy,args);
        track_diagnostic(fmt,copy);
        va_end(copy);
    }
    if (!factory_vprintf) factory_vprintf = dlsym(RTLD_NEXT,"__vprintf_chk");
    int result = factory_vprintf ? factory_vprintf(flag,fmt,args) : vprintf(fmt,args);
    va_end(args);
    return result;
}

size_t fread(void *buf, size_t size, size_t nmemb, FILE *stream) {
    if (!factory_fread) factory_fread = dlsym(RTLD_NEXT,"fread");
    size_t n = factory_fread(buf,size,nmemb,stream);
    if (decoder.gen && size==4 && (!decoder.stream || decoder.stream==stream)) {
        decoder.stream = stream;
        if (n) decoder.audio = 1;
        decoder.frames += n;
    }
    return n;
}

int pclose(FILE *stream) {
    if (!factory_pclose) factory_pclose = dlsym(RTLD_NEXT,"pclose");
    int ours = decoder.gen && decoder.stream==stream;
    int complete = ours && decoder.audio && feof(stream) && !ferror(stream);
    int status = factory_pclose(stream);
    int saved_errno = errno;
    if (ours) {
        pthread_mutex_lock(&track_lock);
        struct factory_track *slot = &factory_tracks[decoder.bank];
        if (slot->gen == decoder.gen) {
            slot->decoded = complete && status==0 ? 1 : -1;
            if (decoder.bank == TRACK_SAMPLE)
                slot->frames = decoder.frames <= UINT32_MAX ? (uint32_t)decoder.frames : 0;
            track_public_snapshot();
        }
        decoder.gen = 0;
        pthread_mutex_unlock(&track_lock);
    }
    errno = saved_errno;
    return status;
}

/* This stock libstdc++ imports glibc readdir64 for filesystem::directory_iterator.
 * Return only real entries from that one bank, for one native key callback.
 * All other directories retain their normal view, including the library. */
struct dirent64 *readdir64(DIR *dir) {
    if (!factory_readdir) factory_readdir = dlsym(RTLD_NEXT,"readdir64");
    if (directory_finished == dir) {
        directory_finished = NULL;
        errno = 0;
        return NULL;
    }
    if (!atomic_load(&filter_mode)) return factory_readdir(dir);
    pthread_mutex_lock(&track_lock);
    int mode = atomic_load(&filter_mode);
    struct stat st;
    if (!mode || fstat(dirfd(dir),&st) ||
        st.st_dev != track_dirs_stat[filter_bank].st_dev ||
        st.st_ino != track_dirs_stat[filter_bank].st_ino) {
        pthread_mutex_unlock(&track_lock);
        return factory_readdir(dir);
    }
    struct dirent64 *entry;
    while ((entry=factory_readdir(dir))) {
        int hit = !strcmp(entry->d_name,filter_name);
        if (mode == TRACK_THROUGH) {
            if (hit) directory_finished = dir;
            break;
        }
        if (hit) break;
    }
    pthread_mutex_unlock(&track_lock);
    return entry;
}

static struct factory_track track_state(int bank) {
    pthread_mutex_lock(&track_lock);
    struct factory_track current = factory_tracks[bank];
    pthread_mutex_unlock(&track_lock);
    return current;
}

static void track_filter(int mode, int bank, const char *name) {
    pthread_mutex_lock(&track_lock);
    if (mode) { filter_bank=bank; strcpy(filter_name,name); }
    atomic_store(&filter_mode,mode);
    pthread_mutex_unlock(&track_lock);
}
