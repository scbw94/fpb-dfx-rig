/*
 * fpb_producer.c — FPB DFX rig, writer side. Stands in for the UMD.
 * Implements FPB_DFX_RIG_SPEC.md v1.1.
 *
 * Build:  cc -O2 -std=c11 -Wall -Wextra -Werror -o fpb_producer fpb_producer.c
 * Dialect: g++ -std=c++17 -fsyntax-only -x c++ fpb_producer.c
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

/* §4.1 seqlock, write side. Both fences are load-bearing; do not weaken them
 * because x86 appears not to need them (§8). */
static inline void fpb_write_begin(struct fpb_slot *s)
{
    uint32_t g = FPB_LOAD_RLX(s->gen);
    FPB_STORE_RLX(s->gen, g + 1u);
    FPB_FENCE_REL();                 /* gen odd before any payload store */
}

static inline void fpb_write_end(struct fpb_slot *s)
{
    uint32_t g = FPB_LOAD_RLX(s->gen);
    FPB_STORE_REL(s->gen, g + 1u);   /* payload visible before gen even */
}

/* ---------------------------------------------------------------------------
 * §5 producer
 * ------------------------------------------------------------------------- */

#define FPB_PROFILE_LEN 12u

struct fpb_milestone {
    double   frac;
    uint32_t kind;
    const char *name;
};

/* §5.3 replay profile. Fractions are of the stamped e_total_us. */
static const struct fpb_milestone fpb_profile[FPB_PROFILE_LEN] = {
    { 0.000, FPB_KIND_ANCHOR, "FRAME_OPEN" },
    { 0.060, FPB_KIND_DELTA,  "RP_BEGIN.0" },
    { 0.130, FPB_KIND_DELTA,  "DRAW.0"     },
    { 0.210, FPB_KIND_DELTA,  "DRAW.1"     },
    { 0.290, FPB_KIND_DELTA,  "RP_END.0"   },
    { 0.370, FPB_KIND_DELTA,  "RP_BEGIN.1" },
    { 0.470, FPB_KIND_DELTA,  "DRAW.2"     },
    { 0.660, FPB_KIND_DELTA,  "RP_END.1"   },
    { 0.740, FPB_KIND_DELTA,  "SUBMIT"     },
    { 0.820, FPB_KIND_ANCHOR, "ACQUIRE"    },
    { 0.920, FPB_KIND_DELTA,  "PRESENT"    },
    { 1.000, FPB_KIND_ANCHOR, "FRAME_END"  }
};

static volatile sig_atomic_t fpb_stop = 0;

static void fpb_on_signal(int sig)
{
    (void)sig;
    fpb_stop = 1;
}

/*
 * Prediction mode (--predict ema). NOT part of spec v1.1; see REPORT.md §9.4.
 *
 * In v1.1 the producer generates a jittered schedule, stamps it, and then sleeps
 * to its own offsets. "Scheduled" and "actual" are therefore the same object and
 * snap_us can only ever capture sleep overshoot — it is non-negative by
 * construction and half the signal's range is unreachable (§9.5 observation 17).
 *
 * In prediction mode the two are separated, which is what a real UMD does:
 *
 *   stamped notch_us[]  = an EMA over previous frames' ACTUAL offsets — a
 *                         forecast, which reality is free to beat or miss
 *   actual firing times = nominal profile x a per-frame difficulty x per-
 *                         milestone jitter
 *   snap_us             = actual - forecast, and so signed in both directions
 *
 * The per-frame difficulty is a common mode: it scales the whole frame, which is
 * what makes an early anchor informative about a later one. Without it every
 * milestone is independent and early deviation forecasts nothing (§9.5
 * observation 16). It is AR(1) so that difficulty also drifts across frames and
 * the EMA visibly lags, which is the interesting failure mode for a predictor.
 */
static double fpb_difficulty = 1.0;

/* §5.3: deterministic xorshift32, fixed seed, so runs reproduce exactly. */
static uint32_t fpb_rng = 0x9E3779B9u;

static uint32_t fpb_xorshift32(void)
{
    uint32_t x = fpb_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fpb_rng = x;
    return x;
}

static uint64_t fpb_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * §5.5: absolute deadlines only. Relative sleeps accumulate scheduling jitter
 * into drift across a long run. CLOCK_MONOTONIC, not RAW: clock_nanosleep does
 * not accept RAW.
 */
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

/*
 * §5.3 schedule generation. Jitter applies to milestones 1..10 only; index 0
 * is always 0 and index 11 is always e_total_us. Strict monotonicity is
 * enforced afterwards by bumping any offset that did not exceed its
 * predecessor.
 *
 * The jittered value is additionally capped so that the monotonicity repair can
 * never push milestone 10 to or past e_total_us, which would make the two
 * stated invariants (index 11 pinned, strict monotonicity) unsatisfiable
 * together. At the default --jitter-pct 8 the cap never binds; it only matters
 * at extreme jitter. Recorded in REPORT.md §9.4.
 */
static void fpb_build_schedule(uint32_t e_total_us, uint32_t jitter_pct,
                               uint32_t *notch_us, uint32_t *notch_kind)
{
    uint32_t i;

    for (i = 0; i < FPB_PROFILE_LEN; i++) {
        double   base_d = fpb_profile[i].frac * (double)e_total_us;
        uint32_t base   = (uint32_t)base_d;
        uint32_t v      = base;

        if (i >= 1u && i <= 10u && jitter_pct > 0u) {
            uint32_t span = (uint32_t)((base_d * (double)jitter_pct) / 100.0);
            if (span > 0u) {
                uint32_t r = fpb_xorshift32() % (2u * span + 1u);
                v = (base > span) ? (base - span + r) : (base + r);
            }
        }
        /* Leave room for every later milestone plus the pinned final one. */
        if (v + (FPB_PROFILE_LEN - i) > e_total_us)
            v = e_total_us - (FPB_PROFILE_LEN - i);

        notch_us[i]   = v;
        notch_kind[i] = fpb_profile[i].kind;
    }

    notch_us[0] = 0u;
    for (i = 1u; i <= 10u; i++)
        if (notch_us[i] <= notch_us[i - 1u])
            notch_us[i] = notch_us[i - 1u] + 1u;
    notch_us[11] = e_total_us;

    for (i = FPB_PROFILE_LEN; i < FPB_NOTCH_MAX; i++) {
        notch_us[i]   = 0u;
        notch_kind[i] = FPB_KIND_DELTA;
    }
}

/* Symmetric uniform in [-1, +1] from the same deterministic stream. */
static double fpb_unit(void)
{
    return ((double)(fpb_xorshift32() % 2001u) - 1000.0) / 1000.0;
}

/*
 * The ACTUAL milestone offsets for one frame, in prediction mode. Nominal
 * profile scaled by an AR(1) per-frame difficulty (common mode), then per
 * milestone jitter (independent mode). Strict monotonicity enforced as §5.3
 * requires; index 0 is pinned to 0 but index 11 is NOT pinned, because the
 * whole point is that the actual frame total varies.
 */
static void fpb_build_actual(uint32_t e_nom_us, uint32_t jitter_pct,
                             uint32_t drift_pct, uint32_t *act_us)
{
    uint32_t i;

    fpb_difficulty = 0.75 * fpb_difficulty
                   + 0.25 * (1.0 + fpb_unit() * (double)drift_pct / 100.0);
    if (fpb_difficulty < 0.5) fpb_difficulty = 0.5;
    if (fpb_difficulty > 1.8) fpb_difficulty = 1.8;

    for (i = 0; i < FPB_PROFILE_LEN; i++) {
        double base = fpb_profile[i].frac * (double)e_nom_us * fpb_difficulty;
        double v = base * (1.0 + fpb_unit() * (double)jitter_pct / 100.0);
        act_us[i] = (v < 0.0) ? 0u : (uint32_t)v;
    }
    act_us[0] = 0u;
    for (i = 1u; i < FPB_PROFILE_LEN; i++)
        if (act_us[i] <= act_us[i - 1u])
            act_us[i] = act_us[i - 1u] + 1u;
    for (i = FPB_PROFILE_LEN; i < FPB_NOTCH_MAX; i++)
        act_us[i] = 0u;
}

/* §5.1 --fifo: warn and continue on failure; never exit. */
static void fpb_try_fifo(void)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = 40;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "fpb_producer: warning: SCHED_FIFO 40 not granted (%s); continuing\n",
                strerror(errno));
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "fpb_producer: warning: mlockall failed (%s); continuing\n",
                strerror(errno));
}

static void fpb_usage(void)
{
    fprintf(stderr,
        "usage: fpb_producer [--shm PATH] [--fps N] [--frames N] [--load-pct N]\n"
        "                    [--jitter-pct N] [--fifo] [--quiet]\n"
        "  --shm PATH        shared file path (default /tmp/fpb.shm, must be ext4)\n"
        "  --fps N           replay frame rate (default 60)\n"
        "  --frames N        frames to publish, 0 = until killed (default 600)\n"
        "  --load-pct N      %% of frame budget the schedule spans (default 85)\n"
        "  --jitter-pct N    per-milestone jitter, integer percent (default 8)\n"
        "  --fifo            attempt SCHED_FIFO 40 and mlockall\n"
        "  --quiet           suppress per-frame progress on stderr\n"
        "\n"
        "  --predict ema     stamp an EMA forecast instead of the actual schedule,\n"
        "                    so snap_us becomes a signed forecast error (non-spec)\n"
        "  --ema-alpha N     EMA weight percent, default 20 (with --predict)\n"
        "  --drift-pct N     per-frame common-mode difficulty percent, default 12\n");
}

int main(int argc, char **argv)
{
    const char *path = "/tmp/fpb.shm";
    uint32_t fps = 60u, load_pct = 85u, jitter_pct = 8u;
    uint64_t frames = 600u;
    int use_fifo = 0, quiet = 0;
    int predict = 0;
    uint32_t ema_alpha = 20u, drift_pct = 12u;
    uint32_t pred_us[FPB_NOTCH_MAX];
    int i;

    int fd;
    void *map;
    struct fpb_header *h;
    struct fpb_slot *slots;
    uint32_t e_total_us;
    uint64_t period_ns, next_open_ns, t_start_ns;
    uint64_t frame, ticks = 0u;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)             path = argv[++i];
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)        fps = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)     frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--load-pct") && i + 1 < argc)   load_pct = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--jitter-pct") && i + 1 < argc) jitter_pct = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--fifo"))                       use_fifo = 1;
        else if (!strcmp(argv[i], "--quiet"))                      quiet = 1;
        else if (!strcmp(argv[i], "--ema-alpha") && i + 1 < argc)   ema_alpha = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--drift-pct") && i + 1 < argc)   drift_pct = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--predict") && i + 1 < argc) {
            if (!strcmp(argv[++i], "ema")) predict = 1;
            else { fpb_usage(); return 2; }
        }
        else { fpb_usage(); return 2; }
    }

    if (fps == 0u || load_pct == 0u || load_pct > 100u) {
        fprintf(stderr, "fpb_producer: --fps must be > 0 and --load-pct in 1..100\n");
        return 2;
    }

    /*
     * §2.1: the shared file must live on ext4. DrvFs is a 9p mount and neither
     * MAP_SHARED coherency nor futex key derivation is dependable over it, and
     * the resulting failure is indistinguishable from a lost-wakeup bug.
     */
    if (!strncmp(path, "/mnt/", 5)) {
        fprintf(stderr,
            "fpb_producer: refusing '%s': DrvFs path (§2.1).\n"
            "  Neither MAP_SHARED coherency nor futex key derivation is dependable\n"
            "  over 9p. Use a path on the ext4 root filesystem (/tmp or $HOME).\n", path);
        return 2;
    }

    signal(SIGINT, fpb_on_signal);
    signal(SIGTERM, fpb_on_signal);

    if (use_fifo)
        fpb_try_fifo();

    /* §5.2 backing store. Close the fd: the mapping keeps the file alive. */
    fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "fpb_producer: open('%s'): %s\n", path, strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)FPB_MAP_BYTES) != 0) {
        fprintf(stderr, "fpb_producer: ftruncate('%s', %zu): %s\n",
                path, (size_t)FPB_MAP_BYTES, strerror(errno));
        close(fd);
        return 1;
    }
    map = mmap(NULL, (size_t)FPB_MAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "fpb_producer: mmap('%s'): %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    h     = (struct fpb_header *)map;
    slots = (struct fpb_slot *)(void *)((char *)map + sizeof(struct fpb_header));

    /* Zero the whole mapping, populate the header, publish magic last (§4.3). */
    memset(map, 0, (size_t)FPB_MAP_BYTES);

    e_total_us = (1000000u / fps) * load_pct / 100u;

    h->abi_version   = FPB_ABI_VERSION;
    h->header_bytes  = (uint32_t)sizeof(struct fpb_header);
    h->slot_bytes    = (uint32_t)sizeof(struct fpb_slot);
    h->payload_bytes = (uint32_t)sizeof(struct fpb_payload);
    h->slot_count    = FPB_SLOTS;
    h->notch_max     = FPB_NOTCH_MAX;
    h->writer_pid    = (uint32_t)getpid();

    FPB_FENCE_REL();
    h->magic = FPB_MAGIC;   /* plain per §3.6; ordered by the fence above */

    /* Seed the forecast with the nominal §5.3 profile: frame 0 has no history. */
    for (i = 0; i < (int)FPB_NOTCH_MAX; i++)
        pred_us[i] = (i < (int)FPB_PROFILE_LEN)
                   ? (uint32_t)(fpb_profile[i].frac * (double)e_total_us) : 0u;
    pred_us[FPB_PROFILE_LEN - 1u] = e_total_us;

    period_ns    = 1000000000ull / (uint64_t)fps;
    t_start_ns   = fpb_now_ns();
    next_open_ns = t_start_ns;

    if (!quiet)
        fprintf(stderr, "fpb_producer: pid=%u shm=%s fps=%u e_total_us=%u frames=%llu\n",
                (unsigned)getpid(), path, fps, e_total_us,
                (unsigned long long)frames);

    for (frame = 0u; (frames == 0u || frame < frames) && !fpb_stop; frame++) {
        struct fpb_slot *s = &slots[frame & (FPB_SLOTS - 1u)];
        struct fpb_payload *p = &s->p;
        uint32_t notch_us[FPB_NOTCH_MAX], notch_kind[FPB_NOTCH_MAX];
        uint32_t act_us[FPB_NOTCH_MAX];
        uint32_t e_stamp;
        uint64_t t_open_ns, now_ns;
        uint32_t m;

        if (predict) {
            /* Actual is drawn fresh; stamped is the standing forecast. The two
             * are independent, so snap_us = actual - forecast is signed. */
            fpb_build_actual(e_total_us, jitter_pct, drift_pct, act_us);
            for (m = 0u; m < FPB_NOTCH_MAX; m++) {
                notch_us[m]   = pred_us[m];
                notch_kind[m] = (m < FPB_PROFILE_LEN) ? fpb_profile[m].kind : FPB_KIND_DELTA;
            }
            e_stamp = pred_us[FPB_PROFILE_LEN - 1u];
        } else {
            /* v1.1: the stamped schedule IS the schedule that gets slept to. */
            fpb_build_schedule(e_total_us, jitter_pct, notch_us, notch_kind);
            for (m = 0u; m < FPB_NOTCH_MAX; m++)
                act_us[m] = notch_us[m];
            e_stamp = e_total_us;
        }

        fpb_sleep_until_ns(next_open_ns);
        if (fpb_stop)
            break;
        t_open_ns = fpb_now_ns();

        /* §5.4 step 2: stamp the schedule under the seqlock. */
        fpb_write_begin(s);
        p->frame_id    = (uint32_t)frame;
        p->e_total_us  = e_stamp;
        p->notch_count = FPB_PROFILE_LEN;
        p->cursor_us   = 0u;
        p->snap_us     = 0;
        p->flags       = 0u;
        p->fired_lo    = 0u;
        p->fired_hi    = 0u;
        p->t_open_lo   = (uint32_t)(t_open_ns & 0xFFFFFFFFu);
        p->t_open_hi   = (uint32_t)(t_open_ns >> 32);
        p->t_tick_lo   = p->t_open_lo;
        p->t_tick_hi   = p->t_open_hi;
        for (m = 0u; m < FPB_NOTCH_MAX; m++) {
            p->notch_us[m]   = notch_us[m];
            p->notch_kind[m] = notch_kind[m];
        }
        fpb_write_end(s);

        /* §4.3 step 3: only now is the frame id visible. Do not reorder. */
        FPB_STORE_REL(h->cur_frame, (uint32_t)frame);

        /* §5.4 step 4: walk the milestones. */
        for (m = 0u; m < FPB_PROFILE_LEN && !fpb_stop; m++) {
            uint64_t elapsed_us;

            /* Sleep to when the work ACTUALLY completes, not to the forecast. */
            fpb_sleep_until_ns(t_open_ns + (uint64_t)act_us[m] * 1000ull);
            now_ns     = fpb_now_ns();
            elapsed_us = (now_ns - t_open_ns) / 1000ull;

            fpb_write_begin(s);
            p->cursor_us = (elapsed_us > (uint64_t)e_stamp)
                         ? e_stamp : (uint32_t)elapsed_us;
            p->t_tick_lo = (uint32_t)(now_ns & 0xFFFFFFFFu);
            p->t_tick_hi = (uint32_t)(now_ns >> 32);
            if (m < 32u)
                p->fired_lo |= (uint32_t)1u << m;
            else
                p->fired_hi |= (uint32_t)1u << (m - 32u);
            if (notch_kind[m] == FPB_KIND_ANCHOR)
                p->snap_us = (int32_t)((int64_t)elapsed_us - (int64_t)notch_us[m]);
            fpb_write_end(s);

            ticks++;

            /*
             * §4.5 wake policy, v1.1. The increment is UNCONDITIONAL and only
             * the syscall is gated on waiters; see §4.6. Both store-load pairs
             * are seq_cst — acquire on the re-check would permit the load to be
             * hoisted above the waiters store and reopen the lost wakeup.
             */
            if (notch_kind[m] == FPB_KIND_ANCHOR) {
                FPB_ADD_SEQ(h->futex_word, 1u);
                if (FPB_LOAD_SEQ(h->waiters) != 0u) {
                    syscall(SYS_futex, &h->futex_word, FPB_FUTEX_WAKE,
                            INT_MAX, NULL, NULL, 0);
                    FPB_ADD_RLX(h->writer_syscalls, 1u);
                }
            }
        }

        /* Fold this frame's actuals into the forecast for the next one. The
         * predictor therefore lags any sustained drift, which is exactly the
         * behaviour a governor has to cope with. */
        if (predict) {
            for (m = 0u; m < FPB_PROFILE_LEN; m++)
                pred_us[m] = (uint32_t)(((uint64_t)act_us[m] * ema_alpha
                                       + (uint64_t)pred_us[m] * (100u - ema_alpha)) / 100u);
            for (m = 1u; m < FPB_PROFILE_LEN; m++)
                if (pred_us[m] <= pred_us[m - 1u])
                    pred_us[m] = pred_us[m - 1u] + 1u;
        }

        /* §5.4 step 5. */
        FPB_STORE_RLX(h->heartbeat_ms, (uint32_t)((fpb_now_ns() - t_start_ns) / 1000000ull));

        if (!quiet)
            fprintf(stderr, "frame=%llu cursor_us=%u fired=0x%08x snap_us=%d\n",
                    (unsigned long long)frame, p->cursor_us, p->fired_lo, p->snap_us);

        next_open_ns += period_ns;
        now_ns = fpb_now_ns();
        if (next_open_ns < now_ns)
            next_open_ns = now_ns;    /* §5.5 resync; never build a backlog */
    }

    /*
     * §5.6. Exactly one line, NOT suppressed by --quiet: T2 reads it.
     * futex_wakes and writer_syscalls are one counter under two names, so this
     * reads the header rather than keeping a second tally.
     */
    fprintf(stderr, "producer: frames=%llu ticks=%llu futex_wakes=%u\n",
            (unsigned long long)frame, (unsigned long long)ticks,
            FPB_LOAD_RLX(h->writer_syscalls));

    munmap(map, (size_t)FPB_MAP_BYTES);
    return 0;
}
