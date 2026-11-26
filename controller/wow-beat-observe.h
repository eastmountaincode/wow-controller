#ifndef WOW_BEAT_OBSERVE_H
#define WOW_BEAT_OBSERVE_H

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <unistd.h>

/* A sample spanning a transport change or fewer than two loaded audio blocks
 * has no single trustworthy state. The two counters are in one perf group. */
static inline int wow_beat_sample_state(uint64_t blocks, uint64_t rendered) {
    if (blocks < 2) return -1;
    if (rendered == blocks) return 1;
    if (rendered == 0) return 0;
    return -1;
}

enum { WOW_ENGINE_UNKNOWN, WOW_ENGINE_STOCK, WOW_ENGINE_PERFORMANCE };

/* These two known executables have identical audio objects and renderer layout.
 * Match the build ID and all bytes, independently of the active slot path.
 * Stock SHA-256: 26e08d4431ce563a530decd40bf56f001efc3a92a23407c64f75222db67873af
 * Performance:   afd4b6c90f97e2d589d93e4bd410e32a1493243c855bed68da861427c3944633 */
static inline int wow_beat_factory_image(int fd) {
    static const uint8_t stock[] = {0x89,0x29,0x7c,0x1f,0xe2,0x75,0x6a,0xeb,0xac,0xfb,
                                    0xe3,0x31,0x26,0xd9,0xb9,0x79,0x9a,0x4d,0xb9,0xe8};
    static const uint8_t performance[] = {0xed,0xb3,0xa6,0x92,0x9b,0x49,0xe6,0x0a,0xac,0x22,
                                          0x8c,0x76,0x0d,0x9e,0x58,0x86,0x18,0x1b,0x74,0x6a};
    uint8_t id[sizeof stock], buf[4096];
    struct stat file;
    if (fstat(fd, &file) || file.st_size != 169640 ||
        pread(fd, id, sizeof id, 0x180) != sizeof id) return WOW_ENGINE_UNKNOWN;
    int kind;
    uint32_t expected;
    if (!memcmp(id, stock, sizeof id)) {
        kind = WOW_ENGINE_STOCK;
        expected = UINT32_C(0xfa00346f);
    } else if (!memcmp(id, performance, sizeof id)) {
        kind = WOW_ENGINE_PERFORMANCE;
        expected = UINT32_C(0x1ed1f768);
    } else return WOW_ENGINE_UNKNOWN;

    uint32_t hash = UINT32_C(2166136261);
    off_t offset = 0;
    while (offset < file.st_size) {
        ssize_t bytes = pread(fd, buf, sizeof buf, offset);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) return 0;
        for (ssize_t i = 0; i < bytes; i++)
            hash = (hash ^ buf[i]) * UINT32_C(16777619);
        offset += bytes;
    }
    return offset == file.st_size && hash == expected ? kind : WOW_ENGINE_UNKNOWN;
}

#if defined(__linux__) && defined(__arm__)

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>

static inline uintptr_t wow_beat_factory_base(void) {
    static atomic_int identity;
    int known = atomic_load(&identity);
    if (!known) {
        int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        known = wow_beat_factory_image(fd) ? 1 : -1;
        close(fd);
        atomic_store(&identity, known);
    }
    if (known < 0) return 0;
    /* The verified ELF's program headers start at RVA 0x34. The kernel
     * publishes their relocated address in the calling program's aux vector. */
    uintptr_t phdr = getauxval(AT_PHDR);
    return phdr > 0x34 ? phdr - 0x34 : 0;
}

struct wow_beat_counters {
    pid_t tid;
    int blocks;
    int rendered;
};

static inline int wow_beat_open_counters(struct wow_beat_counters *p,
                                         pid_t tid, uintptr_t base) {
    /* A loaded beat reaches 0x120f4 once per audio block. The play flag
     * branches to 0x123aa, whose straight-line path calls the beat renderer
     * at 0x123dc. Both probes are Thumb instruction starts. */
    struct perf_event_attr a = {0};
    a.size = PERF_ATTR_SIZE_VER1; /* includes both bp_addr and bp_len */
    a.type = PERF_TYPE_BREAKPOINT;
    a.bp_type = HW_BREAKPOINT_X;
    a.bp_len = HW_BREAKPOINT_LEN_2;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.pinned = 1;
    a.disabled = 1;
    a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                    PERF_FORMAT_TOTAL_TIME_RUNNING;
    a.bp_addr = base + 0x120f4;
    int blocks = syscall(SYS_perf_event_open, &a, tid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (blocks < 0) return -1;
    a.pinned = 0;
    a.disabled = 0;
    a.bp_addr = base + 0x123aa;
    int rendered = syscall(SYS_perf_event_open, &a, tid, -1, blocks, PERF_FLAG_FD_CLOEXEC);
    if (rendered < 0) {
        close(blocks);
        return -1;
    }
    if (ioctl(blocks, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) {
        close(rendered);
        close(blocks);
        return -1;
    }
    *p = (struct wow_beat_counters){ .tid=tid, .blocks=blocks, .rendered=rendered };
    return 0;
}

/* Call from one control worker inside wowmachine. It sleeps for 36 ms and
 * releases both hardware slots at the end of every observation. A failed
 * sample returns -1 so the caller can use its routed-key state if available. */
static inline int wow_beat_observe(pid_t engine_pid) {
    static atomic_int mixer_tid;
    if (engine_pid != getpid()) return -1;
    uintptr_t base = wow_beat_factory_base();
    if (!base) return -1;
    struct wow_beat_counters groups[24];
    unsigned count = 0;
    pid_t cached = atomic_load(&mixer_tid);
    if (cached) {
        /* Look up the TID in this engine's task directory before reuse. */
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d", (int)cached);
        if (access(path, F_OK) == 0 && wow_beat_open_counters(groups, cached, base) == 0)
            count = 1;
    }
    if (!count) {
        DIR *tasks = opendir("/proc/self/task");
        if (!tasks) return -1;
        pid_t self_tid = syscall(SYS_gettid);
        struct dirent *ent;
        while (count < sizeof groups / sizeof *groups && (ent = readdir(tasks))) {
            char *end;
            long tid = strtol(ent->d_name, &end, 10);
            if (*end || tid <= 0 || tid > INT_MAX || tid == self_tid) continue;
            if (wow_beat_open_counters(&groups[count], (pid_t)tid, base) == 0) ++count;
        }
        closedir(tasks);
    }
    if (!count) {
        atomic_store(&mixer_tid, 0);
        return -1;
    }
    const struct timespec window = { .tv_nsec = 36000000 };
    int slept = nanosleep(&window, NULL) == 0;
    int observed = -1, matched = 0;
    pid_t identified = 0;
    for (unsigned i = 0; i < count; ++i) {
        struct wow_beat_counters *p = &groups[i];
        struct { uint64_t nr, enabled, running, blocks, rendered; } sample = {0};
        if (!ioctl(p->blocks, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) && slept &&
            read(p->blocks, &sample, sizeof sample) == sizeof sample && sample.nr == 2 &&
            sample.running && sample.running <= sample.enabled &&
            sample.running >= sample.enabled - sample.enabled / 4 && sample.blocks) {
            ++matched;
            identified = p->tid;
            observed = wow_beat_sample_state(sample.blocks, sample.rendered);
        }
        close(p->rendered);
        close(p->blocks);
    }
    atomic_store(&mixer_tid, matched == 1 ? identified : 0);
    return matched == 1 ? observed : -1;
}

#else
static inline int wow_beat_observe(pid_t engine_pid) {
    (void)engine_pid;
    return -1;
}
#endif
#endif
