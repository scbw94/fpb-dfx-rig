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

/* FPB-CAPTURES-BEGIN
 * Generated by tools/mkprofile.py from
 *   fpb_offline_model.json
 * 6 streams, 13704 frames of capture. Do not hand-edit; regenerate.
 *
 * frac is the milestone's offset as a fraction of that stream's median
 * E_total. q_us[] is the E_total inverse CDF at
 *   q = 0, .10, .25, .50, .75, .90, .99, 1.0
 * The first six are captured. The p99 knot is SOLVED so the sampled mean
 * equals the captured mean -- the model's quantiles stop at p90 and give
 * the segment above it no shape. max is captured.
 */
struct fpb_capture_ms { double frac; uint32_t kind; const char *name; };
struct fpb_capture {
    const char *name;
    uint32_t    n_ms;
    const struct fpb_capture_ms *ms;
    double      q_us[8];
    double      median_us;
    uint32_t    frames;
};

static const struct fpb_capture_ms fpb_cms_game1[18] = {
    { 0.000000000, FPB_KIND_ANCHOR, "FRAME_OPEN" },
    { 0.456380725, FPB_KIND_DELTA,  "TRANSFER.1" },
    { 0.485510846, FPB_KIND_DELTA,  "RP_BEGIN.1" },
    { 0.489457407, FPB_KIND_DELTA,  "RP_END.1" },
    { 0.616051214, FPB_KIND_DELTA,  "TRANSFER.2" },
    { 0.682673714, FPB_KIND_DELTA,  "RP_BEGIN.2" },
    { 0.686772088, FPB_KIND_DELTA,  "RP_END.2" },
    { 0.697093876, FPB_KIND_DELTA,  "RP_BEGIN.3" },
    { 0.714108270, FPB_KIND_DELTA,  "CMDBUF_END.1" },
    { 0.715239848, FPB_KIND_DELTA,  "SUBMIT.1" },
    { 0.740616552, FPB_KIND_DELTA,  "CMDBUF_BEGIN.2" },
    { 0.784649776, FPB_KIND_DELTA,  "RP_END.3" },
    { 0.861262911, FPB_KIND_DELTA,  "RP_BEGIN.4" },
    { 0.901197788, FPB_KIND_ANCHOR, "ACQUIRE" },
    { 0.918833131, FPB_KIND_DELTA,  "CMDBUF_END.2" },
    { 0.919992264, FPB_KIND_DELTA,  "SUBMIT.2" },
    { 0.933915655, FPB_KIND_DELTA,  "RP_END.4" },
    { 1.000000000, FPB_KIND_ANCHOR, "FRAME_END" },
};
static const struct fpb_capture_ms fpb_cms_game2[18] = {
    { 0.000000000, FPB_KIND_ANCHOR, "FRAME_OPEN" },
    { 0.502346204, FPB_KIND_DELTA,  "TRANSFER.1" },
    { 0.516640434, FPB_KIND_DELTA,  "RP_BEGIN.1" },
    { 0.520953143, FPB_KIND_DELTA,  "RP_END.1" },
    { 0.672398849, FPB_KIND_DELTA,  "TRANSFER.2" },
    { 0.707483480, FPB_KIND_DELTA,  "RP_BEGIN.2" },
    { 0.711565589, FPB_KIND_DELTA,  "RP_END.2" },
    { 0.725317378, FPB_KIND_DELTA,  "RP_BEGIN.3" },
    { 0.744344715, FPB_KIND_DELTA,  "CMDBUF_END.1" },
    { 0.745700896, FPB_KIND_DELTA,  "SUBMIT.1" },
    { 0.779076728, FPB_KIND_DELTA,  "CMDBUF_BEGIN.2" },
    { 0.784406541, FPB_KIND_DELTA,  "RP_END.3" },
    { 0.866333954, FPB_KIND_DELTA,  "RP_BEGIN.4" },
    { 0.903737683, FPB_KIND_ANCHOR, "ACQUIRE" },
    { 0.929084881, FPB_KIND_DELTA,  "CMDBUF_END.2" },
    { 0.929247624, FPB_KIND_DELTA,  "RP_END.4" },
    { 0.930210514, FPB_KIND_DELTA,  "SUBMIT.2" },
    { 1.000000000, FPB_KIND_ANCHOR, "FRAME_END" },
};
static const struct fpb_capture_ms fpb_cms_game3[25] = {
    { 0.000000000, FPB_KIND_ANCHOR, "FRAME_OPEN" },
    { 0.413801623, FPB_KIND_DELTA,  "TRANSFER.1" },
    { 0.437666882, FPB_KIND_DELTA,  "CMDBUF_BEGIN.2" },
    { 0.449227894, FPB_KIND_DELTA,  "TRANSFER.2" },
    { 0.479300292, FPB_KIND_DELTA,  "TRANSFER.3" },
    { 0.481433570, FPB_KIND_DELTA,  "RP_BEGIN.1" },
    { 0.485892817, FPB_KIND_DELTA,  "RP_END.1" },
    { 0.491232921, FPB_KIND_DELTA,  "CMDBUF_END.1" },
    { 0.492223865, FPB_KIND_DELTA,  "SUBMIT.1" },
    { 0.628230887, FPB_KIND_DELTA,  "TRANSFER.4" },
    { 0.645544922, FPB_KIND_DELTA,  "RP_BEGIN.2" },
    { 0.656170008, FPB_KIND_DELTA,  "RP_END.2" },
    { 0.679016787, FPB_KIND_DELTA,  "RP_BEGIN.3" },
    { 0.683943971, FPB_KIND_DELTA,  "RP_END.3" },
    { 0.689091330, FPB_KIND_DELTA,  "RP_BEGIN.4" },
    { 0.765779447, FPB_KIND_DELTA,  "RP_END.4" },
    { 0.815629421, FPB_KIND_DELTA,  "CMDBUF_END.2" },
    { 0.816757987, FPB_KIND_DELTA,  "SUBMIT.2" },
    { 0.833053491, FPB_KIND_DELTA,  "RP_BEGIN.5" },
    { 0.856877420, FPB_KIND_DELTA,  "CMDBUF_BEGIN.3" },
    { 0.897134529, FPB_KIND_ANCHOR, "ACQUIRE" },
    { 0.904291363, FPB_KIND_DELTA,  "RP_END.5" },
    { 0.970767155, FPB_KIND_DELTA,  "CMDBUF_END.3" },
    { 0.971826910, FPB_KIND_DELTA,  "SUBMIT.3" },
    { 1.000000000, FPB_KIND_ANCHOR, "FRAME_END" },
};
static const struct fpb_capture_ms fpb_cms_game4[27] = {
    { 0.000000000, FPB_KIND_ANCHOR, "FRAME_OPEN" },
    { 0.414254816, FPB_KIND_DELTA,  "TRANSFER.1" },
    { 0.441581577, FPB_KIND_DELTA,  "CMDBUF_BEGIN.2" },
    { 0.451644242, FPB_KIND_DELTA,  "TRANSFER.2" },
    { 0.471427376, FPB_KIND_DELTA,  "RP_BEGIN.1" },
    { 0.475233414, FPB_KIND_DELTA,  "RP_END.1" },
    { 0.478820383, FPB_KIND_DELTA,  "TRANSFER.3" },
    { 0.495221896, FPB_KIND_DELTA,  "CMDBUF_END.1" },
    { 0.496235021, FPB_KIND_DELTA,  "SUBMIT.1" },
    { 0.651679870, FPB_KIND_DELTA,  "RP_BEGIN.2" },
    { 0.665740278, FPB_KIND_DELTA,  "RP_END.2" },
    { 0.681936417, FPB_KIND_DELTA,  "RP_BEGIN.3" },
    { 0.721762841, FPB_KIND_DELTA,  "TRANSFER.4" },
    { 0.744859120, FPB_KIND_DELTA,  "RP_END.3" },
    { 0.789436230, FPB_KIND_DELTA,  "RP_BEGIN.4" },
    { 0.790627313, FPB_KIND_DELTA,  "CMDBUF_END.2" },
    { 0.791804700, FPB_KIND_DELTA,  "SUBMIT.2" },
    { 0.796870296, FPB_KIND_DELTA,  "RP_END.4" },
    { 0.803825190, FPB_KIND_DELTA,  "RP_BEGIN.5" },
    { 0.809205643, FPB_KIND_DELTA,  "CMDBUF_BEGIN.3" },
    { 0.815339118, FPB_KIND_DELTA,  "RP_END.5" },
    { 0.842843838, FPB_KIND_DELTA,  "RP_BEGIN.6" },
    { 0.901686731, FPB_KIND_ANCHOR, "ACQUIRE" },
    { 0.902549261, FPB_KIND_DELTA,  "RP_END.6" },
    { 0.952191889, FPB_KIND_DELTA,  "CMDBUF_END.3" },
    { 0.953506202, FPB_KIND_DELTA,  "SUBMIT.3" },
    { 1.000000000, FPB_KIND_ANCHOR, "FRAME_END" },
};
static const struct fpb_capture_ms fpb_cms_game5[25] = {
    { 0.000000000, FPB_KIND_ANCHOR, "FRAME_OPEN" },
    { 0.464017257, FPB_KIND_DELTA,  "TRANSFER.1" },
    { 0.478101785, FPB_KIND_DELTA,  "CMDBUF_BEGIN.2" },
    { 0.486058724, FPB_KIND_DELTA,  "TRANSFER.2" },
    { 0.513081595, FPB_KIND_DELTA,  "RP_BEGIN.1" },
    { 0.516872758, FPB_KIND_DELTA,  "RP_END.1" },
    { 0.521523475, FPB_KIND_DELTA,  "TRANSFER.3" },
    { 0.522868012, FPB_KIND_DELTA,  "CMDBUF_END.1" },
    { 0.524058252, FPB_KIND_DELTA,  "SUBMIT.1" },
    { 0.675604477, FPB_KIND_DELTA,  "TRANSFER.4" },
    { 0.689700008, FPB_KIND_DELTA,  "RP_BEGIN.2" },
    { 0.699993373, FPB_KIND_DELTA,  "RP_END.2" },
    { 0.720227453, FPB_KIND_DELTA,  "RP_BEGIN.3" },
    { 0.761709507, FPB_KIND_DELTA,  "CMDBUF_BEGIN.3" },
    { 0.770393879, FPB_KIND_DELTA,  "RP_END.3" },
    { 0.782781178, FPB_KIND_DELTA,  "CMDBUF_END.2" },
    { 0.784588565, FPB_KIND_DELTA,  "SUBMIT.2" },
    { 0.795873818, FPB_KIND_DELTA,  "RP_BEGIN.4" },
    { 0.804976943, FPB_KIND_DELTA,  "RP_END.4" },
    { 0.823690182, FPB_KIND_DELTA,  "RP_BEGIN.5" },
    { 0.867222073, FPB_KIND_DELTA,  "RP_END.5" },
    { 0.907646178, FPB_KIND_ANCHOR, "ACQUIRE" },
    { 0.922678454, FPB_KIND_DELTA,  "CMDBUF_END.3" },
    { 0.923549096, FPB_KIND_DELTA,  "SUBMIT.3" },
    { 1.000000000, FPB_KIND_ANCHOR, "FRAME_END" },
};
static const struct fpb_capture_ms fpb_cms_combined[21] = {
    { 0.000000000, FPB_KIND_ANCHOR, "FRAME_OPEN" },
    { 0.453042299, FPB_KIND_DELTA,  "TRANSFER.1" },
    { 0.497606458, FPB_KIND_DELTA,  "RP_BEGIN.1" },
    { 0.502273543, FPB_KIND_DELTA,  "RP_END.1" },
    { 0.536383402, FPB_KIND_DELTA,  "TRANSFER.2" },
    { 0.551651464, FPB_KIND_DELTA,  "CMDBUF_BEGIN.2" },
    { 0.591828577, FPB_KIND_DELTA,  "CMDBUF_END.1" },
    { 0.593042028, FPB_KIND_DELTA,  "SUBMIT.1" },
    { 0.678756678, FPB_KIND_DELTA,  "RP_BEGIN.2" },
    { 0.687010786, FPB_KIND_DELTA,  "RP_END.2" },
    { 0.703585645, FPB_KIND_DELTA,  "RP_BEGIN.3" },
    { 0.755883877, FPB_KIND_DELTA,  "RP_END.3" },
    { 0.803715030, FPB_KIND_DELTA,  "RP_BEGIN.4" },
    { 0.839038288, FPB_KIND_DELTA,  "CMDBUF_BEGIN.3" },
    { 0.851106103, FPB_KIND_DELTA,  "RP_END.4" },
    { 0.855413193, FPB_KIND_DELTA,  "CMDBUF_END.2" },
    { 0.856506594, FPB_KIND_DELTA,  "SUBMIT.2" },
    { 0.903244294, FPB_KIND_ANCHOR, "ACQUIRE" },
    { 0.970650598, FPB_KIND_DELTA,  "CMDBUF_END.3" },
    { 0.971930718, FPB_KIND_DELTA,  "SUBMIT.3" },
    { 1.000000000, FPB_KIND_ANCHOR, "FRAME_END" },
};

#define FPB_CAPTURE_COUNT 6u
static const struct fpb_capture fpb_captures[FPB_CAPTURE_COUNT] = {
    { "game1", 18, fpb_cms_game1,
      { 871.9, 13323.3, 16276.0, 18871.9, 21663.5, 24418.5, 36801.2, 55735.9 },
      18872, 1427 },
    { "game2", 18, fpb_cms_game2,
      { 6210.9, 14209.2, 16705.5, 19202.1, 21380.3, 23438.3, 31102.0, 37418.8 },
      19202, 1438 },
    { "game3", 25, fpb_cms_game3,
      { 6502.1, 13380.0, 16052.6, 18921.4, 21542.2, 24355.5, 27541.5, 96978.1 },
      18921, 1409 },
    { "game4", 27, fpb_cms_game4,
      { 480.7, 13640.0, 16443.2, 19021.4, 21646.9, 24138.3, 31146.3, 121163.0 },
      19021, 1435 },
    { "game5", 25, fpb_cms_game5,
      { 7591.1, 17040.7, 20259.1, 23629.7, 27016.4, 29991.6, 39024.1, 43794.3 },
      23630, 1143 },
    { "combined", 21, fpb_cms_combined,
      { 480.7, 13970.7, 16774.6, 19529.4, 22492.8, 25838.8, 32553.9, 121163.0 },
      19529, 6852 },
};
/* FPB-CAPTURES-END */

/*
 * Capture mode (--profile NAME). NOT part of spec v1.1; §10 defers replay from
 * external data to a later phase. See REPORT.md §9.4 D12.
 *
 * With no --profile the rig is exactly v1.1: the hardcoded §5.3 profile above,
 * a fixed e_total_us from --fps/--load-pct, and synthetic jitter. Every result
 * in §9.3 was produced on that path and is unaffected by anything here.
 *
 * With --profile the milestone fractions and the frame-time distribution both
 * come from captured systrace instead. The two things that changes:
 *
 *   shape     a real per-setting work-item schedule (18-27 milestones, with
 *             ACQUIRE where the capture puts it) replaces the 12 invented ones.
 *
 *   duration  E_total is redrawn every frame from the captured marginal, which
 *             is right-skewed with a long tail -- the property §9.5 obs. 18
 *             records the symmetric-uniform default as lacking.
 */
static const struct fpb_capture *fpb_cap = NULL;

static uint32_t fpb_cap_len(void)
{
    return fpb_cap ? fpb_cap->n_ms : FPB_PROFILE_LEN;
}

static double fpb_cap_frac(uint32_t i)
{
    return fpb_cap ? fpb_cap->ms[i].frac : fpb_profile[i].frac;
}

static uint32_t fpb_cap_kind(uint32_t i)
{
    if (i >= fpb_cap_len())
        return FPB_KIND_DELTA;
    return fpb_cap ? fpb_cap->ms[i].kind : fpb_profile[i].kind;
}

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
/*
 * Draw one frame time from the captured E_total marginal.
 *
 * Piecewise-linear inverse CDF through eight knots. Six are captured quantiles;
 * the p99 knot is solved offline (tools/mkprofile.py) so the sampled mean equals
 * the captured mean, which is what fixes the shape of the p90-to-max segment the
 * model's quantiles leave undescribed. All linear, so no libm and no -lm.
 *
 * LIMIT, and it is the important one: the model is summary statistics, so
 * draws here are INDEPENDENT. Real frame times are autocorrelated -- a heavy
 * scene stays heavy for many frames -- and that structure is not in the file
 * to reproduce. An EMA forecast exploits autocorrelation, so it does better on
 * a real trace than on this. Treat snap_us magnitudes from --profile as a
 * PESSIMISTIC bound on forecast error, not an estimate of it.
 */
static uint32_t fpb_cap_e_total(void)
{
    static const double q[8] = { 0.0, 0.10, 0.25, 0.50, 0.75, 0.90, 0.99, 1.0 };
    double u, v;
    uint32_t i;

    if (!fpb_cap)
        return 0u;

    u = (double)(fpb_xorshift32() % 1000001u) / 1000000.0;

    for (i = 0u; i < 6u && u > q[i + 1u]; i++)
        ;
    v = fpb_cap->q_us[i]
      + (fpb_cap->q_us[i + 1u] - fpb_cap->q_us[i])
        * (u - q[i]) / (q[i + 1u] - q[i]);

    /* One microsecond of headroom per milestone, so the schedule can be made
     * strictly monotonic at 1 us resolution even for the shortest frames. */
    if (v < (double)(fpb_cap->n_ms + 1u))
        v = (double)(fpb_cap->n_ms + 1u);
    return (uint32_t)v;
}

static void fpb_build_schedule(uint32_t e_total_us, uint32_t jitter_pct,
                               uint32_t *notch_us, uint32_t *notch_kind)
{
    const uint32_t n = fpb_cap_len();
    uint32_t i;

    for (i = 0; i < n; i++) {
        double   base_d = fpb_cap_frac(i) * (double)e_total_us;
        uint32_t base   = (uint32_t)base_d;
        uint32_t v      = base;

        if (i >= 1u && i + 1u < n && jitter_pct > 0u) {
            uint32_t span = (uint32_t)((base_d * (double)jitter_pct) / 100.0);
            if (span > 0u) {
                uint32_t r = fpb_xorshift32() % (2u * span + 1u);
                v = (base > span) ? (base - span + r) : (base + r);
            }
        }
        /* Leave room for every later milestone plus the pinned final one. */
        if (v + (n - i) > e_total_us)
            v = e_total_us - (n - i);

        notch_us[i]   = v;
        notch_kind[i] = fpb_cap_kind(i);
    }

    notch_us[0] = 0u;
    for (i = 1u; i + 1u < n; i++)
        if (notch_us[i] <= notch_us[i - 1u])
            notch_us[i] = notch_us[i - 1u] + 1u;
    notch_us[n - 1u] = e_total_us;

    for (i = n; i < FPB_NOTCH_MAX; i++) {
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
    const uint32_t n = fpb_cap_len();
    uint32_t i;

    if (fpb_cap) {
        /* The frame time already came from the captured marginal, so the
         * synthetic AR(1) difficulty is not applied -- it exists only to
         * manufacture frame-to-frame variation the default profile has no
         * other source for. */
        fpb_difficulty = 1.0;
    } else {
        fpb_difficulty = 0.75 * fpb_difficulty
                       + 0.25 * (1.0 + fpb_unit() * (double)drift_pct / 100.0);
        if (fpb_difficulty < 0.5) fpb_difficulty = 0.5;
        if (fpb_difficulty > 1.8) fpb_difficulty = 1.8;
    }

    for (i = 0; i < n; i++) {
        double base = fpb_cap_frac(i) * (double)e_nom_us * fpb_difficulty;
        double v = base * (1.0 + fpb_unit() * (double)jitter_pct / 100.0);
        act_us[i] = (v < 0.0) ? 0u : (uint32_t)v;
    }
    act_us[0] = 0u;
    for (i = 1u; i < n; i++)
        if (act_us[i] <= act_us[i - 1u])
            act_us[i] = act_us[i - 1u] + 1u;
    for (i = n; i < FPB_NOTCH_MAX; i++)
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
        "  --drift-pct N     per-frame common-mode difficulty percent, default 12\n"
        "\n"
        "  --profile NAME    replay a captured schedule and frame-time distribution\n"
        "                    instead of the synthetic §5.3 profile (non-spec).\n"
        "                    Sets --jitter-pct 0; pass it after to override.\n"
        "                    Draws are IID -- see REPORT.md observation 19.\n");
    if (FPB_CAPTURE_COUNT) {
        uint32_t c;
        fprintf(stderr, "  profiles:        ");
        for (c = 0u; c < FPB_CAPTURE_COUNT; c++)
            fprintf(stderr, " %s", fpb_captures[c].name);
        fprintf(stderr, "\n");
    }
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
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            uint32_t c;
            const char *want = argv[++i];
            for (c = 0u; c < FPB_CAPTURE_COUNT; c++)
                if (!strcmp(fpb_captures[c].name, want)) { fpb_cap = &fpb_captures[c]; break; }
            if (!fpb_cap) {
                fprintf(stderr, "fpb_producer: unknown profile '%s'; available:", want);
                for (c = 0u; c < FPB_CAPTURE_COUNT; c++)
                    fprintf(stderr, " %s", fpb_captures[c].name);
                fprintf(stderr, "%s\n", FPB_CAPTURE_COUNT ? "" : " (none embedded;"
                        " run tools/mkprofile.py)");
                return 2;
            }
            /* Shape and duration now both come from the capture; synthetic
             * within-frame jitter is off unless asked for explicitly. */
            jitter_pct = 0u;
        }
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

    /* Seed the forecast: frame 0 has no history. Under --profile the seed is
     * the capture's own median frame, otherwise the nominal §5.3 profile. */
    if (fpb_cap)
        e_total_us = (uint32_t)fpb_cap->median_us;
    for (i = 0; i < (int)FPB_NOTCH_MAX; i++)
        pred_us[i] = (i < (int)fpb_cap_len())
                   ? (uint32_t)(fpb_cap_frac((uint32_t)i) * (double)e_total_us) : 0u;
    pred_us[fpb_cap_len() - 1u] = e_total_us;

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

        /* In capture mode every frame redraws its own duration from the
         * captured marginal; otherwise e_total_us is the fixed fps/load value. */
        if (fpb_cap)
            e_total_us = fpb_cap_e_total();

        if (predict) {
            /* Actual is drawn fresh; stamped is the standing forecast. The two
             * are independent, so snap_us = actual - forecast is signed. */
            fpb_build_actual(e_total_us, jitter_pct, drift_pct, act_us);
            for (m = 0u; m < FPB_NOTCH_MAX; m++) {
                notch_us[m]   = pred_us[m];
                notch_kind[m] = fpb_cap_kind(m);
            }
            e_stamp = pred_us[fpb_cap_len() - 1u];
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
        p->notch_count = fpb_cap_len();
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
        for (m = 0u; m < fpb_cap_len() && !fpb_stop; m++) {
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
            for (m = 0u; m < fpb_cap_len(); m++)
                pred_us[m] = (uint32_t)(((uint64_t)act_us[m] * ema_alpha
                                       + (uint64_t)pred_us[m] * (100u - ema_alpha)) / 100u);
            for (m = 1u; m < fpb_cap_len(); m++)
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
