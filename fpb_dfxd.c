/*
 * fpb_dfxd.c — FPB DFX rig, reader side. Stands in for the userspace DVFS
 * governor. Reads and logs only; the sole shared field it ever writes is
 * header.waiters. Implements FPB_DFX_RIG_SPEC.md v1.1.
 *
 * Build:  cc -O2 -std=c11 -Wall -Wextra -Werror -o fpb_dfxd fpb_dfxd.c
 * Dialect: g++ -std=c++17 -fsyntax-only -x c++ fpb_dfxd.c
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* ==== FPB-ABI-BEGIN — byte-identical in fpb_producer.c and fpb_dfxd.c ==== */
/*
 * Shared ABI, specification §3. This block is duplicated verbatim into both
 * source files by requirement (§3.1); it is deliberately NOT a header. The
 * drift that duplication invites is mitigated, not eliminated, by three
 * mechanisms: the size assertions below, the per-field offset assertions
 * below, and the runtime shape check the consumer performs at attach (§6.2).
 *
 * Sizes alone do not catch reordering. Exchanging two uint32_t fields leaves
 * every size assertion passing while the reader misinterprets the payload
 * indefinitely, so there is one offset assertion per field. §7 T9 is the
 * negative control that proves this net is live.
 *
 * No _Atomic, no volatile, no std::atomic: ordering lives in the accessors of
 * §3.4, not in the types. That is what keeps the layout byte-identical between
 * C and C++ and lets this block transfer to the C++ UMD unchanged (§2.3, §3.3).
 */

#define FPB_MAGIC          0x30425046u   /* "FPB0" */
#define FPB_ABI_VERSION    1u
#define FPB_SLOTS          8u            /* power of two, fixed */
#define FPB_NOTCH_MAX      32u
#define FPB_KIND_DELTA     0u
#define FPB_KIND_ANCHOR    1u
#define FPB_READ_RETRY_MAX 8u

struct fpb_payload {          /* everything the seqlock protects */
    uint32_t frame_id;
    uint32_t e_total_us;      /* stamped schedule total */
    uint32_t notch_count;
    uint32_t cursor_us;       /* clamped to e_total_us writer-side */
    int32_t  snap_us;         /* signed; last anchor reconciliation */
    uint32_t flags;
    uint32_t t_open_lo, t_open_hi;   /* CLOCK_MONOTONIC ns, split into words */
    uint32_t t_tick_lo, t_tick_hi;   /* ns of the most recent publish */
    uint32_t fired_lo, fired_hi;
    uint32_t notch_us[FPB_NOTCH_MAX];
    uint32_t notch_kind[FPB_NOTCH_MAX];
};

struct fpb_slot {
    uint32_t gen;             /* seqlock; odd => write in flight */
    uint32_t _pad0[15];       /* keep gen off the payload's cacheline */
    struct fpb_payload p;
    uint32_t _pad1[16];
};

struct fpb_header {
    uint32_t magic;           /* published last at init; gates the layout */
    uint32_t abi_version;
    uint32_t header_bytes;
    uint32_t slot_bytes;
    uint32_t payload_bytes;
    uint32_t slot_count;
    uint32_t notch_max;
    uint32_t writer_pid;
    uint32_t cur_frame;       /* monotonic frame id */
    uint32_t futex_word;      /* incremented on anchor publish */
    uint32_t waiters;         /* consumers currently parked */
    uint32_t heartbeat_ms;    /* writer liveness */
    uint32_t writer_syscalls; /* futex wakes issued */
    uint32_t _pad[1011];      /* pad header to exactly 4096 */
};

/* Total mapping: one header page followed by the slot ring. */
#define FPB_MAP_BYTES (sizeof(struct fpb_header) + FPB_SLOTS * sizeof(struct fpb_slot))

/*
 * Accessors (§3.4). Compiler builtins rather than <stdatomic.h>: identical
 * codegen, but the same spelling compiles in C11, C++ and kernel C. Only the
 * fields listed in §3.6 use these; every other field is plain.
 */
#define FPB_LOAD_ACQ(p)     __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define FPB_LOAD_RLX(p)     __atomic_load_n(&(p), __ATOMIC_RELAXED)
#define FPB_LOAD_SEQ(p)     __atomic_load_n(&(p), __ATOMIC_SEQ_CST)
#define FPB_STORE_REL(p,v)  __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)
#define FPB_STORE_RLX(p,v)  __atomic_store_n(&(p), (v), __ATOMIC_RELAXED)
#define FPB_ADD_REL(p,v)    __atomic_fetch_add(&(p), (v), __ATOMIC_RELEASE)
#define FPB_ADD_RLX(p,v)    __atomic_fetch_add(&(p), (v), __ATOMIC_RELAXED)
#define FPB_ADD_SEQ(p,v)    __atomic_fetch_add(&(p), (v), __ATOMIC_SEQ_CST)
#define FPB_SUB_SEQ(p,v)    __atomic_fetch_sub(&(p), (v), __ATOMIC_SEQ_CST)
#define FPB_FENCE_ACQ()     __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define FPB_FENCE_REL()     __atomic_thread_fence(__ATOMIC_RELEASE)

/*
 * Required assertions (§3.5). Spelled static_assert with <assert.h> included:
 * _Static_assert is C-only and would fail the C++ dialect check in §2.3.
 */
static_assert(sizeof(struct fpb_header)  == 4096, "header must be one page");
static_assert(sizeof(struct fpb_payload) == 304,  "payload size");
static_assert(sizeof(struct fpb_slot)    == 432,  "slot size");
static_assert(sizeof(struct fpb_payload) % 4 == 0, "payload must be whole words");
static_assert(FPB_MAP_BYTES == 7552, "total mapping size");

/* One offset assertion per field. These are what catch reordering. */
static_assert(offsetof(struct fpb_payload, frame_id)    ==   0, "payload.frame_id");
static_assert(offsetof(struct fpb_payload, e_total_us)  ==   4, "payload.e_total_us");
static_assert(offsetof(struct fpb_payload, notch_count) ==   8, "payload.notch_count");
static_assert(offsetof(struct fpb_payload, cursor_us)   ==  12, "payload.cursor_us");
static_assert(offsetof(struct fpb_payload, snap_us)     ==  16, "payload.snap_us");
static_assert(offsetof(struct fpb_payload, flags)       ==  20, "payload.flags");
static_assert(offsetof(struct fpb_payload, t_open_lo)   ==  24, "payload.t_open_lo");
static_assert(offsetof(struct fpb_payload, t_open_hi)   ==  28, "payload.t_open_hi");
static_assert(offsetof(struct fpb_payload, t_tick_lo)   ==  32, "payload.t_tick_lo");
static_assert(offsetof(struct fpb_payload, t_tick_hi)   ==  36, "payload.t_tick_hi");
static_assert(offsetof(struct fpb_payload, fired_lo)    ==  40, "payload.fired_lo");
static_assert(offsetof(struct fpb_payload, fired_hi)    ==  44, "payload.fired_hi");
static_assert(offsetof(struct fpb_payload, notch_us)    ==  48, "payload.notch_us");
static_assert(offsetof(struct fpb_payload, notch_kind)  == 176, "payload.notch_kind");

static_assert(offsetof(struct fpb_slot, gen)   ==   0, "slot.gen");
static_assert(offsetof(struct fpb_slot, _pad0) ==   4, "slot._pad0");
static_assert(offsetof(struct fpb_slot, p)     ==  64, "slot.p");
static_assert(offsetof(struct fpb_slot, _pad1) == 368, "slot._pad1");

static_assert(offsetof(struct fpb_header, magic)           ==  0, "header.magic");
static_assert(offsetof(struct fpb_header, abi_version)     ==  4, "header.abi_version");
static_assert(offsetof(struct fpb_header, header_bytes)    ==  8, "header.header_bytes");
static_assert(offsetof(struct fpb_header, slot_bytes)      == 12, "header.slot_bytes");
static_assert(offsetof(struct fpb_header, payload_bytes)   == 16, "header.payload_bytes");
static_assert(offsetof(struct fpb_header, slot_count)      == 20, "header.slot_count");
static_assert(offsetof(struct fpb_header, notch_max)       == 24, "header.notch_max");
static_assert(offsetof(struct fpb_header, writer_pid)      == 28, "header.writer_pid");
static_assert(offsetof(struct fpb_header, cur_frame)       == 32, "header.cur_frame");
static_assert(offsetof(struct fpb_header, futex_word)      == 36, "header.futex_word");
static_assert(offsetof(struct fpb_header, waiters)         == 40, "header.waiters");
static_assert(offsetof(struct fpb_header, heartbeat_ms)    == 44, "header.heartbeat_ms");
static_assert(offsetof(struct fpb_header, writer_syscalls) == 48, "header.writer_syscalls");
static_assert(offsetof(struct fpb_header, _pad)            == 52, "header._pad");
/* ==== FPB-ABI-END ==== */

/* ---------------------------------------------------------------------------
 * §4 protocol
 * ------------------------------------------------------------------------- */

/*
 * §4.5: define the opcodes here rather than including <linux/futex.h>. The
 * mapping is shared between processes, so FUTEX_PRIVATE_FLAG (128) must never
 * appear: the private variant hashes on mm plus virtual address, the two sides
 * land in different buckets, and every wake is silently lost.
 */
#define FPB_FUTEX_WAIT 0
#define FPB_FUTEX_WAKE 1

struct fpb_read_stats {
    uint64_t reads_ok;
    uint64_t retries;
    uint64_t odd_hits;
    uint64_t torn;
    uint64_t eagain;
};

static inline void fpb_cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/*
 * §4.2: NOT memcpy. A bulk copy out of a region the writer may be mutating is
 * formally a data race whatever the retry logic concludes afterwards. Copy
 * word-wise with relaxed atomic loads — 76 of them for a 304-byte payload.
 */
static inline void fpb_copy_payload(struct fpb_payload *dst, const struct fpb_payload *src)
{
    const uint32_t *s = (const uint32_t *)(const void *)src;
    uint32_t *d = (uint32_t *)(void *)dst;
    size_t i;

    for (i = 0; i < sizeof(*dst) / sizeof(uint32_t); i++)
        d[i] = __atomic_load_n(&s[i], __ATOMIC_RELAXED);
}

/* §4.2 seqlock, read side. */
static int fpb_read_slot(const struct fpb_slot *s, struct fpb_payload *out,
                         struct fpb_read_stats *st)
{
    unsigned i;

    for (i = 0; i < FPB_READ_RETRY_MAX; i++) {
        uint32_t g0 = FPB_LOAD_ACQ(s->gen);
        if (g0 & 1u) { st->odd_hits++; fpb_cpu_relax(); continue; }

        fpb_copy_payload(out, &s->p);

        FPB_FENCE_ACQ();
        if (FPB_LOAD_RLX(s->gen) == g0) { st->retries += i; st->reads_ok++; return 0; }
        st->torn++;
    }
    st->eagain++;
    return -EAGAIN;
}

static inline uint64_t fpb_join64(uint32_t hi, uint32_t lo)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* ---------------------------------------------------------------------------
 * §6 consumer
 * ------------------------------------------------------------------------- */

#define FPB_LAT_CAP 65536u

static volatile sig_atomic_t fpb_stop = 0;

static void fpb_on_signal(int sig)
{
    (void)sig;
    fpb_stop = 1;
}

static uint64_t fpb_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fpb_sleep_until_ns(uint64_t deadline_ns)
{
    struct timespec ts;
    int r;

    ts.tv_sec  = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    do {
        r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (r == EINTR && !fpb_stop);
}

static void fpb_try_fifo(void)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = 40;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "fpb_dfxd: warning: SCHED_FIFO 40 not granted (%s); continuing\n",
                strerror(errno));
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "fpb_dfxd: warning: mlockall failed (%s); continuing\n",
                strerror(errno));
}

static int fpb_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static uint64_t fpb_percentile(const uint64_t *sorted, size_t n, double p)
{
    size_t idx;
    if (n == 0u) return 0u;
    idx = (size_t)(p * (double)(n - 1u) + 0.5);
    if (idx >= n) idx = n - 1u;
    return sorted[idx];
}

static void fpb_usage(void)
{
    fprintf(stderr,
        "usage: fpb_dfxd [--shm PATH] [--mode poll|anchor] [--period-us N]\n"
        "                [--secs N] [--out FILE] [--verify] [--fifo]\n"
        "  --shm PATH        shared file path (default /tmp/fpb.shm)\n"
        "  --mode poll|anchor  poll reads at a fixed period; anchor parks in FUTEX_WAIT\n"
        "  --period-us N     poll period, ignored in anchor mode (default 4000)\n"
        "  --secs N          duration, 0 = until SIGINT/SIGTERM (default 10)\n"
        "  --out FILE        per-sample JSONL log\n"
        "  --verify          enable the §6.4 consistency checks\n"
        "  --fifo            attempt SCHED_FIFO 40 and mlockall\n");
}

int main(int argc, char **argv)
{
    const char *path = "/tmp/fpb.shm";
    const char *outpath = NULL;
    int anchor_mode = 0, verify = 0, use_fifo = 0;
    uint64_t period_us = 4000u, secs = 10u;
    int i;

    int fd;
    void *map;
    struct fpb_header *h;
    const struct fpb_slot *slots;
    struct stat stbuf;
    FILE *out = NULL;

    struct fpb_read_stats st;
    struct fpb_payload snap;
    uint64_t samples = 0u, slot_recycled = 0u, verify_violations = 0u, futex_waits = 0u;
    uint64_t stale_sum_ns = 0u, stale_max_ns = 0u;
    uint64_t *lat = NULL;
    size_t lat_n = 0u;
    uint64_t t_end_ns, next_deadline_ns;
    uint64_t prev_retries = 0u;

    int have_prev = 0;
    uint32_t prev_frame = 0u, prev_cursor = 0u, prev_fired_lo = 0u, prev_e_total = 0u;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)        path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   outpath = argv[++i];
        else if (!strcmp(argv[i], "--period-us") && i + 1 < argc) period_us = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc)  secs = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--verify"))                verify = 1;
        else if (!strcmp(argv[i], "--fifo"))                  use_fifo = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "anchor"))    anchor_mode = 1;
            else if (!strcmp(m, "poll")) anchor_mode = 0;
            else { fpb_usage(); return 2; }
        }
        else { fpb_usage(); return 2; }
    }

    if (period_us == 0u) {
        fprintf(stderr, "fpb_dfxd: --period-us must be > 0\n");
        return 2;
    }

    signal(SIGINT, fpb_on_signal);
    signal(SIGTERM, fpb_on_signal);

    if (use_fifo)
        fpb_try_fifo();

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "fpb_dfxd: open('%s'): %s\n  Is the producer running?\n",
                path, strerror(errno));
        return 1;
    }

    /* §6.2 check 1: size. */
    if (fstat(fd, &stbuf) != 0) {
        fprintf(stderr, "fpb_dfxd: fstat('%s'): %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if ((size_t)stbuf.st_size < (size_t)FPB_MAP_BYTES) {
        fprintf(stderr,
            "fpb_dfxd: '%s' is %lld bytes, expected at least %zu.\n"
            "  The producer is probably not running, or is still initialising.\n",
            path, (long long)stbuf.st_size, (size_t)FPB_MAP_BYTES);
        close(fd);
        return 1;
    }

    /* The consumer writes header.waiters, so the mapping is read-write. */
    map = mmap(NULL, (size_t)FPB_MAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "fpb_dfxd: mmap('%s'): %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    h     = (struct fpb_header *)map;
    slots = (const struct fpb_slot *)(const void *)((const char *)map + sizeof(struct fpb_header));

    /* §6.2 check 2: magic. */
    if (h->magic != FPB_MAGIC) {
        fprintf(stderr,
            "fpb_dfxd: bad magic in '%s': found 0x%08x, expected 0x%08x.\n"
            "  The file is not an FPB mapping, or the producer has not published yet.\n",
            path, h->magic, FPB_MAGIC);
        munmap(map, (size_t)FPB_MAP_BYTES);
        return 1;
    }

    /* §6.2 check 3: full shape. Print BOTH sides' numbers on any mismatch. */
    if (h->abi_version   != FPB_ABI_VERSION ||
        h->header_bytes  != (uint32_t)sizeof(struct fpb_header) ||
        h->slot_bytes    != (uint32_t)sizeof(struct fpb_slot) ||
        h->payload_bytes != (uint32_t)sizeof(struct fpb_payload) ||
        h->slot_count    != FPB_SLOTS ||
        h->notch_max     != FPB_NOTCH_MAX) {
        fprintf(stderr,
            "fpb_dfxd: ABI mismatch on '%s'. The two binaries disagree about the layout.\n"
            "  field           file      this binary\n"
            "  abi_version     %-9u %u\n"
            "  header_bytes    %-9u %u\n"
            "  slot_bytes      %-9u %u\n"
            "  payload_bytes   %-9u %u\n"
            "  slot_count      %-9u %u\n"
            "  notch_max       %-9u %u\n",
            path,
            h->abi_version,   FPB_ABI_VERSION,
            h->header_bytes,  (uint32_t)sizeof(struct fpb_header),
            h->slot_bytes,    (uint32_t)sizeof(struct fpb_slot),
            h->payload_bytes, (uint32_t)sizeof(struct fpb_payload),
            h->slot_count,    FPB_SLOTS,
            h->notch_max,     FPB_NOTCH_MAX);
        munmap(map, (size_t)FPB_MAP_BYTES);
        return 1;
    }

    if (outpath) {
        out = fopen(outpath, "w");
        if (!out) {
            fprintf(stderr, "fpb_dfxd: fopen('%s'): %s\n", outpath, strerror(errno));
            munmap(map, (size_t)FPB_MAP_BYTES);
            return 1;
        }
        setvbuf(out, NULL, _IOLBF, 0);
    }

    lat = (uint64_t *)calloc(FPB_LAT_CAP, sizeof(uint64_t));
    if (!lat) {
        fprintf(stderr, "fpb_dfxd: out of memory\n");
        if (out) fclose(out);
        munmap(map, (size_t)FPB_MAP_BYTES);
        return 1;
    }

    memset(&st, 0, sizeof st);
    memset(&snap, 0, sizeof snap);

    t_end_ns         = (secs == 0u) ? UINT64_MAX : fpb_now_ns() + secs * 1000000000ull;
    next_deadline_ns = fpb_now_ns();

    while (!fpb_stop && fpb_now_ns() < t_end_ns) {
        uint32_t seen, fid;
        int progressed = 0;

        /* §6.3: futex_word FIRST, before the progress check. */
        seen = FPB_LOAD_ACQ(h->futex_word);
        fid  = FPB_LOAD_ACQ(h->cur_frame);

        if (fpb_read_slot(&slots[fid & (FPB_SLOTS - 1u)], &snap, &st) == 0) {
            if (snap.frame_id != fid) {
                /* §4.4: coherent read of the wrong frame. The ring wrapped. */
                slot_recycled++;
            } else {
                uint64_t now_ns   = fpb_now_ns();
                uint64_t t_tick   = fpb_join64(snap.t_tick_hi, snap.t_tick_lo);
                uint64_t stale_ns = (now_ns > t_tick) ? (now_ns - t_tick) : 0u;
                uint64_t this_retries = st.retries - prev_retries;

                prev_retries = st.retries;
                samples++;
                stale_sum_ns += stale_ns;
                if (stale_ns > stale_max_ns) stale_max_ns = stale_ns;

                if (have_prev)
                    progressed = (snap.frame_id != prev_frame) || (snap.cursor_us != prev_cursor);
                else
                    progressed = 1;

                /* §6.4 verify checks. */
                if (verify) {
                    if (snap.cursor_us > snap.e_total_us)
                        verify_violations++;
                    if (snap.notch_count < 1u || snap.notch_count > FPB_NOTCH_MAX)
                        verify_violations++;
                    if (have_prev && snap.frame_id == prev_frame) {
                        if (snap.cursor_us < prev_cursor)
                            verify_violations++;
                        if ((snap.fired_lo & prev_fired_lo) != prev_fired_lo)
                            verify_violations++;
                        if (snap.e_total_us != prev_e_total)
                            verify_violations++;
                    }
                }

                if (out)
                    fprintf(out,
                        "{\"t_ns\":%llu,\"frame\":%u,\"cursor_us\":%u,\"e_total_us\":%u,"
                        "\"snap_us\":%d,\"fired\":%llu,\"stale_ns\":%llu,\"retries\":%llu}\n",
                        (unsigned long long)now_ns, snap.frame_id, snap.cursor_us,
                        snap.e_total_us, snap.snap_us,
                        (unsigned long long)fpb_join64(snap.fired_hi, snap.fired_lo),
                        (unsigned long long)stale_ns,
                        (unsigned long long)this_retries);

                if (anchor_mode && progressed && lat_n < FPB_LAT_CAP)
                    lat[lat_n++] = stale_ns;

                prev_frame    = snap.frame_id;
                prev_cursor   = snap.cursor_us;
                prev_fired_lo = snap.fired_lo;
                prev_e_total  = snap.e_total_us;
                have_prev     = 1;
            }
        }

        if (anchor_mode) {
            /*
             * §4.5 consumer side, v1.1. waiters is published seq_cst and the
             * re-check of futex_word is seq_cst — acquire would permit the load
             * to be hoisted above the store and reopen the lost wakeup (§4.6).
             */
            if (!progressed) {
                struct timespec to;
                to.tv_sec  = 0;
                to.tv_nsec = 100 * 1000 * 1000;   /* §4.5: bound the wait at 100 ms */

                FPB_ADD_SEQ(h->waiters, 1u);
                if (FPB_LOAD_SEQ(h->futex_word) == seen) {
                    futex_waits++;
                    syscall(SYS_futex, &h->futex_word, FPB_FUTEX_WAIT,
                            seen, &to, NULL, 0);
                }
                FPB_SUB_SEQ(h->waiters, 1u);
            }
        } else {
            next_deadline_ns += period_us * 1000ull;
            {
                uint64_t now_ns = fpb_now_ns();
                if (next_deadline_ns < now_ns)
                    next_deadline_ns = now_ns;    /* §5.5 resync */
            }
            fpb_sleep_until_ns(next_deadline_ns);
        }
    }

    /* §6.6 summary, in this order. */
    fprintf(stderr, "mode %s\n", anchor_mode ? "anchor" : "poll");
    fprintf(stderr, "samples %llu\n",           (unsigned long long)samples);
    fprintf(stderr, "reads_ok %llu\n",          (unsigned long long)st.reads_ok);
    fprintf(stderr, "retries %llu\n",           (unsigned long long)st.retries);
    fprintf(stderr, "odd_hits %llu\n",          (unsigned long long)st.odd_hits);
    fprintf(stderr, "torn %llu\n",              (unsigned long long)st.torn);
    fprintf(stderr, "eagain %llu\n",            (unsigned long long)st.eagain);
    fprintf(stderr, "slot_recycled %llu\n",     (unsigned long long)slot_recycled);
    fprintf(stderr, "verify_violations %llu\n", (unsigned long long)verify_violations);
    fprintf(stderr, "futex_waits %llu\n",       (unsigned long long)futex_waits);
    fprintf(stderr, "writer_syscalls %u\n",     FPB_LOAD_RLX(h->writer_syscalls));
    fprintf(stderr, "staleness_mean_us %llu\n",
            (unsigned long long)(samples ? (stale_sum_ns / samples) / 1000ull : 0ull));
    fprintf(stderr, "staleness_max_us %llu\n",  (unsigned long long)(stale_max_ns / 1000ull));

    if (anchor_mode) {
        qsort(lat, lat_n, sizeof(uint64_t), fpb_cmp_u64);
        fprintf(stderr, "wake_latency_p50_us %llu\n",
                (unsigned long long)(fpb_percentile(lat, lat_n, 0.50) / 1000ull));
        fprintf(stderr, "wake_latency_p90_us %llu\n",
                (unsigned long long)(fpb_percentile(lat, lat_n, 0.90) / 1000ull));
        fprintf(stderr, "wake_latency_p99_us %llu\n",
                (unsigned long long)(fpb_percentile(lat, lat_n, 0.99) / 1000ull));
        fprintf(stderr, "wake_latency_samples %zu\n", lat_n);
    } else {
        fprintf(stderr, "period_us %llu\n", (unsigned long long)period_us);
    }

    free(lat);
    if (out) fclose(out);
    munmap(map, (size_t)FPB_MAP_BYTES);
    return 0;
}
