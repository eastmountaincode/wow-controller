/* Read-only view of the factory renderer, enabled only on the fingerprinted
 * 32-bit ARM WOW executable. No playback thread calls this code. */
#ifndef WOW_KEYBOARD_PLAYHEAD_H
#define WOW_KEYBOARD_PLAYHEAD_H
#include "wow-playhead.h"

static int cursor_memory = -1, cursor_listener = -1;
static uint32_t cursor_base;

/* The named global 'deck' is its process-lifetime std::shared_ptr<WowDeck>.
 * The stock constructor installs the WowDeck vtable. loadSample installs its
 * TrackState at +0x28, distinct from the backing beat. onBufferEmpty passes
 * the SimpleTrack at state+4 and the double at state+32 to the loop renderer,
 * then stores the renderer's returned cursor in that same double. queSample
 * writes cue seconds * 48000 there. SimpleTrack::length reads frame count at
 * +16. A decoder worker sets +21 while loading. Both supported builds use
 * this layout. pread on our own process returns an error on an unmapped
 * object without dereferencing a pointer during a sample replacement. */
static int cursor_field(uint32_t object, uint32_t offset, void *out, size_t size) {
    if (cursor_memory < 0 || object < 4096 || (object & 3) ||
        (uint64_t)object + offset + size > UINT32_MAX) return 0;
#if defined(__linux__)
    return pread64(cursor_memory, out, size, (off64_t)object + offset) == (ssize_t)size;
#else
    return pread(cursor_memory, out, size, (off_t)object + offset) == (ssize_t)size;
#endif
}

struct cursor_track { uint32_t deck, state, audio; };
static int cursor_objects(struct cursor_track *track) {
    uint32_t tag;
    return cursor_field(cursor_base, 0x1a03c, &track->deck, 4) &&
           cursor_field(track->deck, 0, &tag, 4) && tag == cursor_base + 0x19514 &&
           cursor_field(track->deck, 0x28, &track->state, 4) &&
           cursor_field(track->state, 4, &track->audio, 4) &&
           cursor_field(track->audio, 0, &tag, 4) && tag == cursor_base + 0x19580;
}

static void cursor_start(void) {
#if defined(__arm__) && defined(__linux__)
    uintptr_t base = wow_beat_factory_base();
    if (!base || base > UINT32_MAX - 0x1a044) return;
    cursor_memory = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (cursor_memory < 0) return;
    cursor_base = (uint32_t)base;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct stat st;
        if (lstat(WOW_PLAYHEAD_SOCKET, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid())
            unlink(WOW_PLAYHEAD_SOCKET);
        if (bind(fd, (const struct sockaddr *)&wow_playhead_address, sizeof wow_playhead_address) == 0 &&
            chmod(WOW_PLAYHEAD_SOCKET, 0600) == 0 && listen(fd, 2) == 0) {
            cursor_listener = fd;
            return;
        }
        close(fd);
    }
#endif
}

static void cursor_snapshot(struct wow_playhead *out) {
    pthread_mutex_lock(&track_lock);
    struct factory_track *slot = &factory_tracks[TRACK_SAMPLE];
    struct cursor_track before, after;
    double position, confirm;
    uint32_t length;
    uint8_t loading;
    if (slot->decoded == 1 && cursor_objects(&before) &&
        before.audio != slot->previous_audio && (!slot->audio_object || before.audio == slot->audio_object) &&
        cursor_field(before.audio, 16, &length, sizeof length) && length && length == slot->frames &&
        cursor_field(before.audio, 21, &loading, 1) && !loading &&
        cursor_field(before.state, 32, &position, sizeof position) &&
        cursor_field(before.state, 32, &confirm, sizeof confirm) && position == confirm &&
        cursor_objects(&after) && memcmp(&before, &after, sizeof before) == 0 &&
        position >= -(double)UINT32_MAX && position <= UINT32_MAX) {
        /* The worker may finish decoding a short file before loadSample
         * installs its new TrackState. Bind only once that object is live. */
        slot->audio_object = before.audio;
        /* At a wrap the renderer may leave the next block's starting frame
         * one step outside the interval. Use the same cyclic frame domain. */
        int64_t frame = (int64_t)position;
        if ((double)frame > position) --frame;
        frame %= (int64_t)length;
        if (frame < 0) frame += length;
        out->frame = (uint32_t)frame;
        out->length = length;
        strcpy(out->sample, slot->name);
    }
    pthread_mutex_unlock(&track_lock);
}

static void cursor_request(void) {
    int fd = accept(cursor_listener, NULL, NULL);
    if (fd < 0) return;
    struct wow_playhead result = {0};
    cursor_snapshot(&result);
    (void)send(fd, &result, sizeof result, MSG_DONTWAIT | MSG_NOSIGNAL);
    close(fd);
}
#endif
