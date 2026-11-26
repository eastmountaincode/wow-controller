/* A native "previous" from track zero wraps to the final visible track.
 * Limiting one enumeration to the requested file establishes zero. Limiting
 * the next enumeration to the prefix through that file restores its true
 * index. The deck then resumes its ordinary full-bank navigation. */
enum load_phase { LOAD_IDLE, LOAD_MUTE, LOAD_START, LOAD_ZERO, LOAD_HOLD, LOAD_RELEASE,
                  LOAD_RESTORE, LOAD_DECODE, LOAD_END };
static struct {
    struct wow_track_request req;
    enum load_phase phase;
    unsigned generation, ticket;
    uint64_t after;
    struct timespec ids_before;
    int reply, restore_stop, started, held;
    char result;
} selection = {.reply=-1};

static int selectable(int bank, const char *name) {
    if (!memchr(name,0,256) || !*name || *name=='.' || strpbrk(name,"/\\:\"*$?`<>|")) return 0;
    const char *ext = strrchr(name,'.');
    if (!ext || (strcasecmp(ext,".wav") && strcasecmp(ext,".mp3") && strcasecmp(ext,".ogg") &&
        strcasecmp(ext,".flac") && strcasecmp(ext,".aac") && strcasecmp(ext,".asf"))) return 0;
    for (const char *p=name; *p; p++) if ((unsigned char)*p<32 || *p==127) return 0;
    int dir = open(track_dirs[bank],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir < 0) return 0;
    struct stat st;
    int ok = !fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && st.st_size>0;
    close(dir);
    return ok;
}

static void track_reply(int fd, char result) {
    if (fd < 0) return;
    /* The existing per-thread reply routine contains SIGPIPE locally. */
    int saved = reply_fd, toggle = acknowledge_toggle;
    reply_fd = fd;
    answer(result);
    reply_fd = saved;
    acknowledge_toggle = toggle;
}

static void finish_selection(void) {
    track_filter(TRACK_ALL,0,NULL);
    atomic_store(&link_state->beat_mute_until_ms,0);
    atomic_store(&link_state->beat_mute_pid,0);
    track_reply(selection.reply,selection.result);
    selection.reply=-1;
    selection.phase=LOAD_IDLE;
}

static void stop_selection(char result) {
    int code = selection.req.bank == TRACK_SAMPLE ? WOW_SAMPLE_KEY : WOW_BEAT_KEY;
    if (selection.held) {
        /* A short early release would otherwise load an unrelated next file. */
        track_filter(TRACK_ONLY,selection.req.bank,selection.req.name);
        queue_key(code,0);
        selection.held=0;
    }
    if (selection.started) {
        wow_transport_chord(0,queue_key);
        selection.started=0;
    }
    selection.result=result;
    selection.phase=LOAD_END;
    selection.after=wow_track_ms()+100;
}

/* Guard against hardware gestures during the selection sequence. The MIDI
 * reader remains independent; every physical key is returned to stock after
 * any pending virtual key release/stop edges in this same output batch. */
static void interrupt_selection(void) {
    if (selection.phase!=LOAD_IDLE && selection.phase!=LOAD_END) stop_selection('H');
}

static void track_request(int keyboard_fd) {
    struct wow_track_request req;
    if (ordinary_read(track_listener,&req,sizeof req)!=sizeof req) return;
    uint64_t now=wow_track_ms();
    if (!now || req.until_ms<=now || req.until_ms-now>WOW_TRACK_TIMEOUT || req.bank>=2) return;
    char path[PATH_MAX];
    wow_track_reply_name(path,&req);
    int fd=open(path,O_WRONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return;
    if (req.engine_pid!=getpid()) { track_reply(fd,'N'); return; }
    if (selection.phase!=LOAD_IDLE || reply_fd>=0 || keys.held || keys.phase!=BEAT_IDLE || stream_dropped) {
        track_reply(fd,'H'); return;
    }
    if (!selectable(req.bank,req.name)) { track_reply(fd,'F'); return; }
    /* A deleted/reuploaded name may still be cached in factory RAM. Always
     * load the selected file again, including the currently displayed name. */
    int state=req.bank==TRACK_BEAT ? current_beat() : 1;
    if (state<0 || keyboard_fd<0) { track_reply(fd,'U'); return; }
    char busy=keyboard_available(keyboard_fd);
    if (busy) { track_reply(fd,busy=='H' ? 'H' : 'N'); return; }
    selection.req=req;
    selection.reply=fd;
    selection.started=selection.held=0;
    selection.restore_stop=!state;
    selection.result='T';
    selection.after=0;
    selection.phase=state ? LOAD_START : LOAD_MUTE;
    if (!state) {
        selection.ticket=(unsigned)req.until_ms+400;
        atomic_store(&link_state->beat_mute_ack_ms,0);
        atomic_store(&link_state->beat_mute_pid,getpid());
        atomic_store(&link_state->beat_mute_until_ms,selection.ticket);
    }
}

static void previous_for_selection(int bank, unsigned generation, int code) {
    struct stat st;
    selection.ids_before = (struct timespec){0};
    if (!stat(WOW_NATIVE_IDS,&st)) selection.ids_before=st.st_mtim;
    track_filter(TRACK_THROUGH,bank,selection.req.name);
    selection.generation=generation;
    queue_key(code,1);
    selection.held=1;
    selection.phase=LOAD_HOLD;
}

static void advance_selection(uint64_t now) {
    if (selection.phase==LOAD_IDLE) return;
    if (selection.phase==LOAD_END) {
        if (now>=selection.after) finish_selection();
        return;
    }
    struct pollfd client={.fd=selection.reply,.events=POLLOUT};
    /* Send a phase-specific failure before the bounded client wait expires. */
    if (now+180>=selection.req.until_ms) {
        char reason = 'T';
        switch (selection.phase) {
        case LOAD_MUTE: reason='M'; break;
        case LOAD_ZERO: reason='Z'; break;
        case LOAD_HOLD: case LOAD_RELEASE: reason='V'; break;
        case LOAD_RESTORE: reason='P'; break;
        case LOAD_DECODE: reason='D'; break;
        default: break;
        }
        stop_selection(reason);
        return;
    }
    if (poll(&client,1,0)<0 || client.revents&(POLLHUP|POLLERR|POLLNVAL)) {
        stop_selection('T');
        return;
    }
    int bank=selection.req.bank, code=bank==TRACK_SAMPLE ? WOW_SAMPLE_KEY : WOW_BEAT_KEY;
    struct factory_track s=track_state(bank);
    switch (selection.phase) {
    case LOAD_MUTE:
        if (atomic_load(&link_state->beat_mute_ack_ms)!=selection.ticket) return;
        if (!selection.after) selection.after=now+80;
        if (now<selection.after) return;
        wow_transport_chord(0,queue_key);
        selection.started=1;
        selection.phase=LOAD_START;
        break;
    case LOAD_START:
        if (selection.restore_stop && playing != 1) { stop_selection('P'); return; }
        if (s.gen && s.index==0) {
            previous_for_selection(bank,s.gen,code);
            break;
        }
        track_filter(TRACK_ONLY,bank,selection.req.name);
        selection.generation=s.gen;
        queue_key(code,1);
        queue_key(code,0);
        selection.phase=LOAD_ZERO;
        break;
    case LOAD_ZERO:
        if (s.gen==selection.generation) return;
        if (s.index!=0 || strcmp(s.name,selection.req.name)) { stop_selection('Z'); return; }
        previous_for_selection(bank,s.gen,code);
        break;
    case LOAD_HOLD:
        if (s.gen==selection.generation) return;
        if (strcmp(s.name,selection.req.name)) { stop_selection('V'); return; }
        /* writeLastTrackId() runs after loadSample()/loadBeat(). Wait for its
         * actual bank index before releasing a stock long press. */
        selection.phase=LOAD_RELEASE;
        selection.after=now+80;
        break;
    case LOAD_RELEASE: {
        if (now<selection.after) return;
        int32_t ids[2];
        int fd=open(WOW_NATIVE_IDS,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        struct stat st;
        int saved=fd>=0 && ordinary_read(fd,ids,sizeof ids)==sizeof ids && ids[bank]==s.index &&
            !fstat(fd,&st) && (st.st_mtim.tv_sec!=selection.ids_before.tv_sec ||
                               st.st_mtim.tv_nsec!=selection.ids_before.tv_nsec);
        if (fd>=0) close(fd);
        if (!saved) return;
        queue_key(code,0);
        selection.held=0;
        selection.phase=LOAD_RESTORE;
        break;
    }
    case LOAD_RESTORE:
        track_filter(TRACK_ALL,0,NULL);
        if (selection.started) {
            wow_transport_chord(0,queue_key);
            selection.started=0;
        }
        selection.phase=LOAD_DECODE;
        break;
    case LOAD_DECODE:
        if (s.gen==selection.generation || strcmp(s.name,selection.req.name)) { stop_selection('F'); return; }
        if (!s.decoded) return;
        if (selection.restore_stop && current_beat()!=0) { stop_selection('P'); return; }
        stop_selection(s.decoded==1 ? 'K' : 'D');
        break;
    default: break;
    }
}
