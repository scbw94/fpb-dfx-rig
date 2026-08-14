# FPB DFX Rig — Software Design Document

**Status:** Revised against spec v1.1; §3 ABI implemented and verified
**Author:** Claude (coding agent)
**Date:** 2026-08-13
**Governing specification:** `FPB_DFX_RIG_SPEC.md` **v1.1**
**Target host:** WSL2, x86-64

> **Numbering.** This document's section numbers are its own and do **not**
> correspond to the specification's. The specification ends at §10; this document
> runs to §14. §13 carries the full two-way mapping, including a check that every
> specification section has a design counterpart.

---

## 0. About this document

This document exists at the user's request. The specification's §1 lists three
deliverables (`fpb_producer.c`, `fpb_dfxd.c`, `REPORT.md`) and does not include a
design document; its presence is therefore a deviation from the spec's deliverable
list and is recorded as such in §12 below, to be repeated in `REPORT.md` §9.4.

Where this document and the specification disagree, **the specification wins**. The
purpose here is to make the design legible and to fix the open questions *before*
code exists, not to renegotiate the spec. Section 12 lists every point where the
spec is silent or self-inconsistent; per spec §0, none of those gaps are resolved by
invention — they are either resolved by a stated rule of precedence or left
unimplemented and reported.

**Reading order for the exchange material:** §6 is the centrepiece. It is what the
final report will trace against, and each exchange there carries an ID (`E0`–`E9`)
that the report will cite when presenting captured evidence.

---

## 1. Purpose and scope

### 1.1 Problem

A userspace DVFS governor wants to steer clocks *within* a frame rather than after
it. To do that it must know, at sub-frame granularity, how far through its
submission schedule the render thread currently is. The obvious mechanism — the
driver signalling the governor per event — puts a syscall on the RHI thread at
renderpass granularity, which costs more than the governor saves.

The **frame progress bar (FPB)** is the proposed alternative: the driver publishes a
progress bar into shared memory, the governor reads it whenever it likes, and the
driver only ever issues a syscall at coarse *anchor* points, and then only if a
governor is demonstrably listening.

### 1.2 What this rig is

A two-process bench that stands in for that pair and lets the design be measured:

| Process | Stands in for | Behaviour |
|---|---|---|
| `fpb_producer` | The Vulkan/D3D user-mode driver (UMD) | Replays a fixed 12-milestone frame schedule at a chosen frame rate, publishing progress into shared memory |
| `fpb_dfxd` | The userspace DVFS governor daemon | Attaches, reads, validates, logs. **Never writes anything except `waiters`.** |

### 1.3 What the deliverable actually is

The code is apparatus. **The deliverable is evidence** — a defensible account of
whether the shared-memory + gated-futex design works, backed by numbers, and an
equally explicit account of what this host *cannot* establish. `REPORT.md` is the
artifact of record.

This framing drives two design decisions that would otherwise look strange:

- Negative controls (T8, T9) are first-class, not optional. A green T4 is worthless
  unless T8 proves the wakes were real wakes and not 100 ms timeouts.
- Instrumentation must be *observational*. Spec §0 forbids adding features and §10
  excludes reporting scripts, so the rig may not grow a tracing subsystem to make
  its own case. See §10.

### 1.4 Non-goals

Restating spec §10 so it is visible at design time: no control loop, no DVFS/OPP
logic, no energy model, no multiple concurrent consumers, no external-JSON replay,
no `memfd` variant, no KMD or `poll()`-on-device-fd path, no third file for a shared
header, no build system, no plotting or reporting scripts. These are deliberate
exclusions from a later phase.

---

## 2. Requirements summary

### 2.1 Functional

| ID | Requirement | Spec ref |
|---|---|---|
| F1 | Producer publishes a 12-milestone schedule per frame into an 8-slot ring | §5.3, §5.4 |
| F2 | Each slot's payload is protected by a seqlock | §4.1, §4.2 |
| F3 | Producer issues a futex wake only on anchors, and only when `waiters != 0` | §4.5 |
| F4 | Consumer supports a fixed-period poll mode and a futex-parked anchor mode | §6.1, §6.3 |
| F5 | Consumer hard-exits on any ABI or size mismatch, printing both sides' numbers | §6.2 |
| F6 | Consumer detects and counts ring wrap (`slot_recycled`) rather than accepting stale samples | §4.4 |
| F7 | Consumer optionally validates writer-side invariants (`--verify`) | §6.4 |
| F8 | Consumer optionally emits per-sample JSONL | §6.5 |

### 2.2 Non-functional and structural

| ID | Requirement | Spec ref |
|---|---|---|
| N1 | Exactly two `.c` files, no headers, no build system | §1 |
| N2 | Clean under `-O2 -std=c11 -Wall -Wextra -Werror` | §2.2 |
| N3 | The ABI block compiles as C++17 (`g++ -fsyntax-only -x c++`) | §2.3 |
| N4 | ABI drift caught at compile time by size *and* per-field offset assertions | §3.5 |
| N5 | Clean under ThreadSanitizer and ASan/UBSan | T6, T7 |
| N6 | Backing file on ext4; never DrvFs | §2.1 |

---

## 3. System context and architecture

### 3.1 Context

```
        ┌───────────────────────────────────────────────────────────┐
        │                    ext4 (/tmp or $HOME)                   │
        │                                                           │
        │        /tmp/fpb.shm   —   7552 bytes, MAP_SHARED          │
        └───────────────────────────────────────────────────────────┘
                    ▲                                   ▲
                    │ mmap PROT_READ|PROT_WRITE          │ mmap PROT_READ|PROT_WRITE
                    │ (writes payload + header)          │ (writes ONLY header.waiters)
                    │                                   │
        ┌───────────┴────────────┐          ┌───────────┴────────────┐
        │     fpb_producer       │          │       fpb_dfxd         │
        │   (stands in for UMD)  │          │  (stands in for DVFS)  │
        │                        │          │                        │
        │  replay 12 milestones  │          │  poll mode:  timed     │
        │  per frame @ N fps     │          │  anchor mode: parked   │
        └────────────┬───────────┘          └───────────┬────────────┘
                     │                                  │
                     │   FUTEX_WAKE (op 1, shared)      │  FUTEX_WAIT (op 0, shared)
                     └──────────────►  kernel futex  ◄──┘
                                      hash bucket keyed on
                                      (inode, page offset)
```

The two processes have **no other channel**. No pipes, no sockets, no signals
between them, no fd passing. Everything in §6 happens through those 7552 bytes plus
the futex hash bucket the kernel derives from the file's inode.

### 3.2 Why a plain file rather than `memfd_create`

Spec §5.2 mandates it. The design rationale, restated because it matters to §6: a
`MAP_SHARED` mapping of an ext4 file has a **stable, inode-derived futex key**, and
a path is a rendezvous both processes can reach independently. `memfd` would require
fd passing between the two processes, which would mean inventing a control channel —
exactly the coupling this design exists to avoid.

The probe run recorded in §11.1 confirms the inode-keyed shared futex works on this
host.

### 3.3 Process lifecycle

The producer is the owner: it creates, sizes, zeroes, populates and publishes. The
consumer is a pure attacher and may start, stop and restart at any time without the
producer noticing or caring. There is no registration, no handshake acknowledgement,
and no teardown protocol. The only trace a consumer leaves is a transient `+1` on
`header.waiters`.

This asymmetry is deliberate and is the property T5 measures: **an unobserved
progress bar must cost the writer nothing.**

---

## 4. Data design — the shared ABI

### 4.1 The duplication decision

The ABI block is written **byte-identically into both `.c` files**. Spec §3.1
requires this and forbids fixing it with a header. It is a real hazard — two copies
of a layout that must agree exactly — mitigated by three independent mechanisms:

| Mechanism | Catches | When |
|---|---|---|
| `static_assert` on every struct size | Added/removed/resized fields | Compile time |
| `static_assert` on every field offset | **Reordered fields** (sizes unchanged) | Compile time |
| Runtime attach check of all six shape words | The two binaries built from different revisions | Consumer startup |

The offset assertions are the ones that matter most and the ones a casual
implementation omits. Swapping two `uint32_t` fields leaves every size assertion
passing while the reader silently misinterprets the payload forever. T9 exists to
prove this net has holes in it no larger than one field. The report will record that
a header split is the correct end state.

### 4.2 Memory layout

Total mapping: `sizeof(header) + 8 * sizeof(slot)` = `4096 + 8 × 432` = **7552 bytes**.

```
offset 0                                                          4096
┌─────────────────────────────────────────────────────────────────┐
│ struct fpb_header — exactly one page                            │
│  0 magic  4 abi_version  8 header_bytes  12 slot_bytes          │
│ 16 payload_bytes  20 slot_count  24 notch_max  28 writer_pid    │
│ 32 cur_frame  36 futex_word  40 waiters  44 heartbeat_ms        │
│ 48 writer_syscalls   52..4095 _pad[1011]                        │
└─────────────────────────────────────────────────────────────────┘
4096            4528            4960                          7552
┌───────────────┬───────────────┬─────  ...  ────┬───────────────┐
│ slot[0]  432B │ slot[1]  432B │                │ slot[7]  432B │
└───────────────┴───────────────┴─────  ...  ────┴───────────────┘

each slot (432 B):
┌────────┬───────────────────┬──────────────────────┬────────────┐
│ gen 4B │ _pad0[15]  60B    │ payload  304B        │ _pad1  64B │
│ off 0  │                   │ off 64               │ off 368    │
└────────┴───────────────────┴──────────────────────┴────────────┘
   ▲                              ▲
   └─ own cacheline               └─ starts on a 64 B boundary

each payload (304 B):
  0 frame_id      4 e_total_us   8 notch_count   12 cursor_us
 16 snap_us      20 flags       24 t_open_lo     28 t_open_hi
 32 t_tick_lo    36 t_tick_hi   40 fired_lo      44 fired_hi
 48 notch_us[32]   (128 B, ends 176)
176 notch_kind[32] (128 B, ends 304)
```

The `_pad0[15]` is not filler: it keeps `gen` on its own cacheline so the seqlock
counter's release store does not sit in the same line the writer is streaming
payload into. Every offset above matches the spec §3.5 table; the arithmetic is
self-consistent and was verified by hand before writing this document.

### 4.3 Type discipline

All shared fields are plain `uint32_t`/`int32_t`. **No `_Atomic`, no `volatile`, no
`std::atomic`.** Ordering lives in the accessors, not the types. This is what makes
the block dialect-neutral: the layout is byte-identical in C and C++, so putting
atomicity in the type buys nothing and costs the C++ transfer that §2.3 checks.

Consequence for implementation: the accessor macros (`FPB_LOAD_ACQ` etc.) wrap
`__atomic_*` builtins, which accept plain lvalues. `static_assert` is spelled
without the leading underscore and `<assert.h>` is included, because `_Static_assert`
is C-only and would fail §2.3.

The only `#ifdef __cplusplus` in either file is **none**. If one appears, the design
has diverged.

### 4.4 Field access rules

| Field | Access |
|---|---|
| `slot.gen`, `header.cur_frame`, `header.futex_word`, `header.waiters`, `header.heartbeat_ms`, `header.writer_syscalls` | Accessor macros only |
| Everything else | Plain assignment, inside the seqlock brackets |

### 4.5 64-bit values

Timestamps and the fired bitmask are split into `_lo`/`_hi` word pairs so the
payload is a whole number of `uint32_t` and can be copied word-wise (§5.2). Rejoin
as `((uint64_t)hi << 32) | lo`. With 12 milestones, `fired_hi` is always 0; it is
carried anyway because the layout is frozen and `FPB_NOTCH_MAX` is 32.

---

## 5. Concurrency design

### 5.1 Seqlock, write side

```
fpb_write_begin(s):   g = LOAD_RLX(s->gen);  STORE_RLX(s->gen, g+1);  FENCE_REL()
   ... plain payload stores ...
fpb_write_end(s):     g = LOAD_RLX(s->gen);  STORE_REL(s->gen, g+1)
```

Odd `gen` means a write is in flight. The release fence in `write_begin` ensures the
odd counter is visible *before* any payload store; the release store in `write_end`
ensures every payload store is visible *before* the counter goes even.

Both are load-bearing on a weakly ordered machine and compile to nearly nothing on
x86-64. They will not be removed on the grounds that this host appears not to need
them — see §11.2.

### 5.2 Seqlock, read side

Retry up to `FPB_READ_RETRY_MAX` (8): read `gen`, bail to retry if odd, copy the
payload, acquire fence, re-read `gen` and accept only if unchanged. On exhaustion,
return `-EAGAIN` and count it.

**The payload copy must not be `memcpy`.** A bulk copy out of a region the writer may
be mutating is formally a data race regardless of what the retry logic concludes
afterwards. The copy is word-wise with relaxed atomic loads:

```
for (i = 0; i < sizeof(payload)/4; i++)
    d[i] = __atomic_load_n(&s[i], __ATOMIC_RELAXED);
```

76 relaxed loads per read attempt. This is a correctness requirement, not a
performance one; §11.3 records what T6 can and cannot prove about it.

### 5.3 Counter taxonomy

The read side distinguishes four outcomes, and the design keeps them separate
because they mean different things:

| Counter | Meaning | Normal? |
|---|---|---|
| `odd_hits` | Found `gen` odd — caught the writer mid-update | Yes, contention |
| `torn` | Payload changed under us — retry consumed | Yes, contention |
| `eagain` | 8 retries exhausted — sample lost | No, should be 0 |
| `slot_recycled` | Coherent read of the **wrong frame** — ring wrapped | No, consumer too slow |
| `verify_violations` | A writer-side invariant broke | No, a bug |

`torn` and `verify_violations` are counted separately on purpose. Tearing is normal
contention and proves the retry path is live. A verify violation is a defect.

### 5.4 Slot identity versus slot contents

`slot_index = frame_id & 7`. The generation counter protects a slot's *contents*,
not its *identity*. A reader slow enough for the ring to wrap takes a perfectly
coherent, perfectly self-consistent read of the wrong frame, and no amount of
seqlock retrying detects it.

The consumer therefore compares `snapshot.frame_id` against the `cur_frame` it
intended to read and counts a mismatch as `slot_recycled` instead of accepting the
sample. At 60 fps the ring wraps every **133 ms**; a 4 ms poll is 33× faster, so T3
expects zero.

---

## 6. Interaction design — the exchanges

This is the section the final report traces against. Each exchange has an ID the
report will cite.

### 6.1 What an "exchange" is here

There are no messages. The two processes interact through exactly three physical
channels, and every exchange below is built from them:

| Channel | Direction | Cost | Observable as |
|---|---|---|---|
| **Payload + `gen` stores** in a slot | P → C | ~0 (store to mapped page) | Consumer sample content |
| **Header word updates** (`cur_frame`, `futex_word`, `waiters`, …) | Both | ~0 | Counters, JSONL |
| **`futex` syscall** (`WAKE` op 1 / `WAIT` op 0) | P → C wake, C → kernel park | Syscall + possible context switch | `strace`, `wchan`, `writer_syscalls`, `futex_waits`, `voluntary_ctxt_switches` |

Only the third is a syscall. The entire design is an argument that the third should
happen ~180 times a second instead of ~720, and zero times when nobody is listening.

### 6.2 Exchange catalogue

---

#### E0 — Attach handshake (one-shot, consumer startup)

Not a handshake in the negotiated sense. The producer never learns a consumer
arrived; the consumer unilaterally validates and either proceeds or dies loudly.

```
PRODUCER (already running)                 CONSUMER (starting)
──────────────────────────                 ───────────────────
  [at init, once]
  ftruncate 7552
  zero whole mapping
  populate header shape words
  FENCE_REL()
  STORE magic = 0x30425046 ──────┐
                                 │         open(path), fstat
                                 │         size >= 7552 ?  ── no ──► exit(1)
                                 │                                   "file is N bytes,
                                 │                                    expected 7552;
                                 │                                    is the producer
                                 │                                    running?"
                                 └───────► mmap PROT_READ|PROT_WRITE
                                           LOAD magic
                                           magic == FPB_MAGIC ? ─ no ─► exit(1) both values
                                           abi_version, header_bytes,
                                           slot_bytes, payload_bytes,
                                           slot_count, notch_max
                                             all match compiled? ─ no ─► exit(1)
                                                                        print BOTH
                                                                        full sets
                                           ── yes ──► enter read loop
```

`magic` is written **last, after a release fence**, so a consumer that sees the magic
is guaranteed to see a fully populated header. This is §4.3's ordering rule applied
at whole-object granularity, and it is the same argument as the seqlock at
per-slot granularity.

All three failures are hard exits. A silently misinterpreted layout is the worst
outcome available in this design, so it fails loudly by construction.

**Expected in trace:** one-time only; invisible in steady state.

---

#### E1 — Frame open (once per frame, 60/s at default fps)

```
PRODUCER                                          SHARED MEMORY
────────                                          ─────────────
sleep to absolute frame deadline
  (clock_nanosleep TIMER_ABSTIME)

fpb_write_begin(slot[fid & 7])  ─────────────────► gen: even → ODD
  stamp frame_id, e_total_us                       payload rewritten
  stamp notch_count = 12
  stamp notch_us[0..11], notch_kind[0..11]
  zero cursor_us, snap_us, fired_lo, fired_hi
  set t_open_{lo,hi}, t_tick_{lo,hi}
fpb_write_end(slot)             ─────────────────► gen: ODD → even (release)

STORE_REL(header.cur_frame, fid) ────────────────► cur_frame = fid
```

**The ordering here is the whole point.** `cur_frame` is stored *after*
`fpb_write_end`, with release ordering. A consumer that follows `cur_frame`
therefore can never observe a half-stamped schedule: by the time the new frame id is
visible, the slot describing it is complete and its `gen` is even.

Note this is a **separate write cycle** from milestone 0. Per frame there are 13
seqlock write cycles: this stamp, plus 12 milestone ticks.

**No syscall.** E1 is silent even though `FRAME_OPEN` is an anchor — the anchor wake
belongs to the milestone tick (E3/E4), not the stamp.

---

#### E2 — Delta tick (9 per frame, 540/s)

The common case, and the case the design is optimised for.

```
PRODUCER                                          SHARED MEMORY
────────
sleep to t_open + notch_us[i]

fpb_write_begin(slot)           ─────────────────► gen even → ODD
  cursor_us = min(elapsed, e_total_us)
  t_tick_{lo,hi} = now
  fired_lo |= (1u << i)
fpb_write_end(slot)             ─────────────────► gen ODD → even (release)

  kind == DELTA  ──►  no futex logic at all.
                      waiters is not even read.
```

**Cost to the writer: zero syscalls, one cacheline-ish of stores.** This is the
line item the design exists to protect. A wake per delta tick would be 720
syscalls/s on the RHI thread.

---

#### E3 — Anchor tick, no waiter (the gated path)

3 anchors per frame × 60 fps = 180/s. When no consumer is parked, all 180 cost
nothing but a load.

```
PRODUCER                                          SHARED MEMORY
────────
  [same seqlock write cycle as E2]

  kind == ANCHOR:
    snap_us = elapsed_us - notch_us[i]   (signed reconciliation)
    ...
    LOAD_SEQ(header.waiters)  ───────────────────► reads 0
      == 0  ──►  skip.  No futex_word increment.
                 No syscall.  writer_syscalls unchanged.
```

**This is what T5 measures.** With a poll-mode consumer running for 5 s — reading
250 times a second, fully attached, actively sampling — `writer_syscalls` must
remain **0 throughout**. The reader's existence is invisible to the writer because a
poll-mode reader never touches `waiters`.

> **Design note (test ordering).** `waiters` is a shared counter that only ever
> returns to 0 because a consumer decrements it. A consumer killed between its
> `waiters++` and `waiters--` leaks a permanent `+1`, after which the producer
> issues wakes forever to nobody and T5 can never pass. **T5 must therefore run
> against a freshly created backing file**, not one left over from an anchor-mode
> run. This is a real fragility of the design and will be reported as an
> observation, not fixed — the spec provides no reaper and inventing one is out of
> scope.

---

#### E4 — Anchor tick with a parked waiter (the wake)

The exchange the whole design is built around.

```
CONSUMER (anchor mode)                     PRODUCER
──────────────────────                     ────────
seen = LOAD_ACQ(futex_word)   ── (N) ──►
attempt read; progressed?  no
ADD_SEQ(waiters, +1)          ── 0→1 ──►
re-read LOAD_SEQ(futex_word) == seen (N)? yes
  │
  └─ futex(&futex_word, WAIT(0), N, 100ms)
         │                                   [anchor milestone fires]
         │                                   seqlock write cycle, then:
      ┌──┴──┐                                ADD_SEQ(futex_word, +1)  ── N→N+1 ──►
      │ kernel                                    UNCONDITIONAL (§4.6)
      │ compares *uaddr == N ✓                LOAD_SEQ(waiters) ──► 1  ✓
      │ enqueues on bucket                    futex(&futex_word, WAKE(1), INT_MAX)
      │ keyed by (inode, off)                      │
      │                                            │
      │  ◄─────────────── wake ────────────────────┘
      └──┬──┘                                ADD_RLX(writer_syscalls, +1)
         │
   returns 0
ADD_SEQ(waiters, −1)          ── 1→0 ──►
read slot → progressed → sample accepted
record staleness into wake-latency series
```

**Order within the producer's anchor path (v1.1):** seqlock `write_end` first, then
the unconditional `futex_word` increment, then the `waiters` gate on the syscall.
A consumer that observes the new sequence value therefore always finds the payload
already published; one that observes it early merely takes an extra loop, which is
harmless.

Six properties are load-bearing, each with a named failure mode:

| Property | If violated |
|---|---|
| **Opcode 0/1 with no `FUTEX_PRIVATE_FLAG`** | Private futexes key on `(mm, address)`. Two processes → two mms → different hash buckets → **every wake silently lost**. Presents as a hang. *Confirmed empirically on this host: see §11.1.* |
| **Wait on a monotonic sequence word, not a boolean** | A wake landing between the progress check and kernel entry is lost instead of being caught by the kernel's value comparison. See interleaving C. |
| **Publish `waiters` before re-reading `futex_word`, both seq_cst** | The writer reads `waiters == 0` after the consumer has committed to sleeping. See interleaving D. |
| **Bounded 100 ms wait** | Not for correctness. It makes a dead producer present as a stall statistic rather than a hung daemon. |
| **`futex_word` increment is unconditional**, only the syscall is gated | Lost wakeup bounded by the largest anchor gap. See interleaving D. |
| **Both store-load pairs are `seq_cst`** | Reordering on a weakly-ordered target reopens the same hole the waiter count exists to close. See interleaving D′. |

**Expected volume:** ~180 wakes/s, ~900 over a 5 s T4 run, matched ~1:1 by consumer
waits. T4's ±2% band asserts that coupling.

---

#### E5 — The park race window (analysis)

Four interleavings of the E4 sequence. Three are safe by construction; the fourth is
a genuine residual hole in the protocol *as specified*, and it will be implemented
as specified and reported rather than fixed.

**Interleaving A — clean park and wake.** The nominal path shown in E4. Safe.

**Interleaving B — wake lands after `waiters++`, before the re-check.**

```
C: seen = futex_word (N)
C: read slot → no progress
C: ADD_SEQ(waiters, +1)         0→1
                                 P: anchor publish
                                 P: LOAD_SEQ(waiters) → 1  ✓
                                 P: futex_word N→N+1
                                 P: FUTEX_WAKE  (wakes 0 — nobody parked yet)
C: re-read futex_word = N+1 ≠ N  → skip the WAIT entirely
C: ADD_SEQ(waiters, −1)
```

Caught in userspace by the re-check. **Safe.** This is why the re-read exists.

**Interleaving C — wake lands after the re-check, during kernel entry.**

```
C: ADD_SEQ(waiters, +1)         0→1
C: re-read futex_word == N  → commit, call futex(WAIT, N)
                                 P: futex_word N→N+1
                                 P: FUTEX_WAKE  (wakes 0 — not yet enqueued)
C: kernel compares *uaddr (N+1) ≠ N → returns EAGAIN immediately, no park
```

Caught **in the kernel** by the value comparison. **Safe** — and only safe because
the futex word is a monotonically incremented sequence. With a boolean flag the
kernel comparison would match and the consumer would park on a stale value. This is
the concrete justification for spec §4.5's second non-negotiable property.

**Interleaving D — producer samples `waiters` in the gap before `waiters++`.**
*This was a real defect in spec v1.0 and is closed by v1.1.*

Under **v1.0**, where the increment sat inside the `if (waiters != 0)` guard:

```
C: seen = futex_word (N)
C: read slot → no progress
                                 P: anchor publish
                                 P: LOAD_SEQ(waiters) → 0   ← C hasn't incremented yet
                                 P: skip increment AND skip wake     ← futex_word stays N
C: ADD_SEQ(waiters, +1)         0→1
C: re-read futex_word == N  → parks on stale state
   ⇒ this anchor's wake is MISSED
   ⇒ recovered at the NEXT anchor (≤ 11.62 ms at 60 fps — the FRAME_OPEN→ACQUIRE gap)
   ⇒ or by the 100 ms timeout if the producer has stopped
```

The consumer's progress decision came from a read taken *before* the anchor landed,
and the sequence word — its only other means of detecting that anchor — never moved.
Not a hang, and not a data-correctness violation (the sample read was coherent); a
latency hole bounded by the largest anchor-to-anchor gap.

Under **v1.1**, step 3 advances `futex_word` to N+1 regardless of `waiters`, so the
re-check at step 4 fails and the consumer loops instead of parking. **Closed.** Cost
is one atomic add per anchor — 180/s at 60 fps — and no additional syscall. T5 still
passes, because `writer_syscalls` counts wakes *issued*, not increments.

**Interleaving D′ — the same hole via memory ordering, with the gate already removed.**

Closing the gate is only half the fix. Both sides perform a **store followed by a
load of the other side's word**:

```
consumer:  store waiters      →  load futex_word
producer:  store futex_word   →  load waiters
```

Store-load is the one pair that **neither acquire nor release ordering constrains**.
With acquire on the consumer's re-check, that load may be hoisted above the `waiters`
store, and the machine reproduces interleaving D exactly — producer sees `waiters == 0`,
consumer sees the stale `futex_word`, wake lost — even though the gate is gone.

Both operations on both sides must therefore be `seq_cst`. This is not a stylistic
tightening of the acquire/release pairs elsewhere in the protocol; it is the one
place in the design where acquire/release is *categorically* insufficient.

**Why this is invisible here.** On x86-64 the store side of both pairs is a
`fetch_add`, which compiles to a `lock`-prefixed RMW and drains the store buffer as
a side effect — so the reordering cannot occur regardless of the ordering argument
requested. On aarch64 an `__ATOMIC_ACQUIRE` load may compile to `LDAPR`, which is
RCpc and *can* be reordered ahead of a preceding release store. **This host cannot
exhibit the bug, cannot exhibit the fix, and cannot distinguish the two.** It is a
clean instance of the §11.2 limit: the ordering *is* the algorithm, and x86 is the
wrong machine to check it on.

**Resolution.** Both interleavings are fixed in spec v1.1 (§4.5, §4.6) and the rig
implements v1.1. Nothing is left for the report to excuse. The analysis is retained
here because §4.6 exists specifically to stop someone optimising the ordering back
out later, and this section is the longer form of that argument.

---

#### E6 — Torn read and retry

```
CONSUMER                                   PRODUCER
────────
g0 = LOAD_ACQ(gen) → even, say 40
                                            fpb_write_begin → gen 41 (ODD)
word-wise copy of 76 words  ◄── racing ──►  payload stores
                                            fpb_write_end   → gen 42
FENCE_ACQ()
re-read gen = 42 ≠ 40  ⇒ torn++, retry
```

Or the earlier variant: `g0` reads odd, so `odd_hits++` and the copy is never
attempted.

Both are **normal contention, not errors.** Up to 8 attempts, then `eagain++` and
the sample is dropped.

**Expected volume:** low. A poll consumer at 4 ms against a writer that holds the
seqlock for the duration of ~76 word stores has a small collision probability.
Spec §8 is explicit that **zero retries does not mean the retry path works** — it
means the path is untested. T6 at 120 fps under TSan (which slows both sides
enormously and widens the write window) is the most likely place to see it fire, and
the report will say so rather than claiming the path is cheap.

---

#### E7 — Slot recycled (consumer too slow)

```
CONSUMER                                   PRODUCER
────────
fid = LOAD_ACQ(cur_frame) → 100
                                            ... 8 frames elapse (133 ms) ...
                                            frame 108 now occupies slot 100 & 7
read slot[100 & 7] → coherent, gen stable
snapshot.frame_id == 108 ≠ 100
  ⇒ slot_recycled++, sample DISCARDED
```

The seqlock says this read is perfectly valid, and it is — of the wrong frame. Only
the identity comparison catches it. Expected 0 in T3 and T4; a non-zero count means
the consumer cannot keep up with an 8-deep ring.

---

#### E8 — Producer stops (the timeout path)

```
CONSUMER (anchor mode)                     PRODUCER
──────────────────────
ADD_SEQ(waiters, +1)                        [killed / stalled]
futex(WAIT, seen, 100ms)
   ... 100 ms elapse, no wake ...
returns −1 / ETIMEDOUT
ADD_SEQ(waiters, −1)
read slot → no progress
staleness climbs monotonically
```

The bounded wait converts a hung daemon into a stall statistic. This is the design's
only liveness guarantee, and it is deliberately weak: the consumer does not detect
producer death, it merely fails to observe progress. `heartbeat_ms` exists for this
but the spec never defines its value semantics — see §12.

---

#### E9 — Negative control: the broken exchange (T8)

A copy of the consumer with `FUTEX_PRIVATE_FLAG` (128) OR'd into its **wait** opcode
only. The producer still wakes with the shared opcode.

```
CONSUMER (private, op 128)                 PRODUCER (shared, op 1)
──────────────────────────                 ───────────────────────
futex(&fw, WAIT|128, N, 100ms)
  kernel key = (mm_consumer, vaddr)         futex(&fw, WAKE, INT_MAX)
        ▼                                     kernel key = (inode, offset)
  bucket X                                          ▼
                                              bucket Y  ≠ X
  ... no wake ever arrives ...                wakes 0 waiters
  100 ms timeout                              writer_syscalls still increments
```

**Expected:** `futex_waits` greatly exceeds `writer_syscalls` (the consumer spins at
10 waits/s on timeout while the producer wakes at 180/s into an empty bucket), and
wake latency collapses to the 100 ms timeout floor.

This is the test that makes T4 mean something. Without it, a T4 that "passed" could
be a consumer that never received a single real wake and simply timed out 50 times
in 5 seconds. §11.1 already confirms the mechanism at the syscall level; T8
confirms it end-to-end in the actual rig. The variant is discarded afterwards.

### 6.3 Steady-state exchange budget

Default configuration: `--fps 60 --load-pct 85` ⇒ frame period 16666 µs,
`e_total_us` = 14166 µs.

Milestone offsets before jitter, and the anchor spacing that bounds E5-D:

| # | Fraction | Offset µs | Kind | Name | Gap to next anchor |
|---|---|---|---|---|---|
| 0 | 0.000 | 0 | **ANCHOR** | FRAME_OPEN | 11616 µs |
| 1 | 0.060 | 849 | DELTA | RP_BEGIN.0 | |
| 2 | 0.130 | 1841 | DELTA | DRAW.0 | |
| 3 | 0.210 | 2974 | DELTA | DRAW.1 | |
| 4 | 0.290 | 4108 | DELTA | RP_END.0 | |
| 5 | 0.370 | 5241 | DELTA | RP_BEGIN.1 | |
| 6 | 0.470 | 6658 | DELTA | DRAW.2 | |
| 7 | 0.660 | 9349 | DELTA | RP_END.1 | |
| 8 | 0.740 | 10482 | DELTA | SUBMIT | |
| 9 | 0.820 | 11616 | **ANCHOR** | ACQUIRE | 2550 µs |
| 10 | 0.920 | 13032 | DELTA | PRESENT | |
| 11 | 1.000 | 14166 | **ANCHOR** | FRAME_END | 2500 µs (to next FRAME_OPEN) |

**Max anchor-to-anchor gap: 11.62 ms.** That is the recovery bound for E5-D.

Per-second budget at 60 fps:

| Quantity | Rate | Over 5 s |
|---|---|---|
| Seqlock write cycles (E1 + E2 + E3/E4) | 780/s | 3900 |
| Milestone ticks (spec's "ticks") | 720/s | 3600 |
| Anchors | 180/s | 900 |
| **Producer syscalls, no waiter (E3)** | **0/s** | **0** |
| **Producer syscalls, waiter parked (E4)** | **180/s** | **900** |
| Poll-mode samples @ 4000 µs | 250/s | **1250** ← T3 target |
| Anchor-mode wakes | ~180/s | ~900 |
| Ring wrap period | 133 ms | 37 wraps |

**Syscall reduction claimed by the design: 720/s → 180/s when observed (4×), and
720/s → 0/s when unobserved (∞).** These two numbers are what T4 and T5 exist to
substantiate.

### 6.4 Predicted staleness

*Predictions recorded before implementation, so the report can compare.*

- **Poll mode, 4 ms period.** Mean inter-tick gap is 14166/12 ≈ 1180 µs, but the
  frame tail (FRAME_END → next FRAME_OPEN) is a 2500 µs silence. Sampling at 4 ms
  lands uniformly, so predicted mean staleness ≈ **700–1000 µs**, max ≈ **2.5 ms**
  (bounded by the tail gap).
- **Anchor mode.** Staleness of a progressed sample *is* the wake latency: producer
  timestamp → consumer observation. On an idle WSL2 VM, predicted p50 ≈ **30–100 µs**,
  well inside T4's 500 µs bar. p99 will be dominated by scheduler noise and possibly
  by E5-D outliers at the next-anchor distance.

Per spec §8, **magnitudes are not portable** — WSL2 is a VM with no competing render
workload and `SCHED_FIFO` unlikely to be granted. Only the ordering between modes is
indicative.

---

## 7. Component design — producer

### 7.1 Interface

```
fpb_producer [--shm PATH] [--fps N] [--frames N] [--load-pct N]
             [--jitter-pct N] [--fifo] [--quiet]
```

Defaults: `/tmp/fpb.shm`, 60, 600, 85, 8, off, off. `--frames 0` runs until killed.
Unknown arguments print usage and **exit 2**.

### 7.2 Initialisation

`open(O_CREAT|O_RDWR|O_CLOEXEC, 0666)` → `ftruncate(7552)` → `mmap(PROT_READ|PROT_WRITE, MAP_SHARED)` → **close the fd** (the mapping keeps the file alive; holding the fd buys nothing). Then zero the mapping, populate the header shape words and `writer_pid`, release fence, publish `magic` last.

Path validation per spec §2.1 happens before anything else: a `/mnt/` path is a
configuration error, not a bug to debug later, and it presents identically to a
lost-wakeup failure.

### 7.3 Schedule generation

`e_total_us = (1000000 / fps) * load_pct / 100`. Base offsets are `fraction × e_total_us`.

Jitter applies to milestones **1–10 only**; index 0 is pinned to 0 and index 11 to
`e_total_us`. After jittering, strict monotonicity is enforced by bumping any offset
that failed to exceed its predecessor. The RNG is a deterministic xorshift32 seeded
with a fixed constant, so runs reproduce exactly — which matters because the report
must be checkable.

### 7.4 Frame loop

Per spec §5.4: sleep to the frame deadline → stamp under seqlock (E1) → release-store
`cur_frame` → for each milestone, sleep to `t_open + offset`, update under seqlock
(E2/E3/E4), and on anchors apply the wake policy → update `heartbeat_ms`.

### 7.5 Pacing

**Absolute deadlines only**, via `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`,
retrying on `EINTR`. Relative sleeps accumulate scheduling jitter into drift across a
long run. On overrun, the next deadline resynchronises to *now* rather than letting a
backlog build — a producer that tries to "catch up" would emit a burst of ticks with
compressed spacing and corrupt every latency number in the report.

`CLOCK_MONOTONIC`, not `CLOCK_MONOTONIC_RAW`: `clock_nanosleep` does not accept RAW.

### 7.6 Summary output

Exactly one line to stderr on exit, per spec §5.6:

```
producer: frames=<N> ticks=<N> futex_wakes=<N>
```

Two constraints that are easy to get wrong:

- **`--quiet` does not suppress this line.** It suppresses per-frame progress only.
  T2 reads `futex_wakes` from this line, so suppressing it would make T2
  untestable — which is exactly the gap v1.1 closed.
- **`futex_wakes` and `writer_syscalls` are one counter under two names.** The
  producer prints it as `futex_wakes`; the header exposes it as
  `writer_syscalls` and the consumer prints it under that name. There is a single
  increment site, in the anchor wake path, and no second counter is introduced.

`ticks` counts milestone publications (12 per frame), not seqlock write cycles
(13 per frame — the twelve milestones plus the E1 open stamp). At `--frames 100`
that is 1200 ticks.

---

## 8. Component design — consumer

### 8.1 Interface

```
fpb_dfxd [--shm PATH] [--mode poll|anchor] [--period-us N]
         [--secs N] [--out FILE] [--verify] [--fifo]
```

Defaults: `/tmp/fpb.shm`, `poll`, 4000, 10, none, off, off. `--secs 0` runs until
SIGINT/SIGTERM.

Signals set a `volatile sig_atomic_t` flag and the loop exits cleanly, **so the
summary is still printed**. A daemon that dies without its summary has destroyed the
measurement it existed to take.

### 8.2 Attach validation

Three hard exits, in order, per E0: size, magic, then all six shape words. Any
mismatch prints **both sides' complete set of numbers** — the compiled-in
expectations and what was found in the file. Guessing which field disagreed is
exactly the debugging tax this check exists to eliminate.

The mapping is `PROT_READ|PROT_WRITE` despite the consumer being logically a reader,
because it must write `header.waiters`.

### 8.3 Read loop ordering

Per iteration, in this exact order:

1. `seen = LOAD_ACQ(futex_word)` — **first**, before anything else
2. `fid = LOAD_ACQ(cur_frame)`
3. `fpb_read_slot()`
4. If `snapshot.frame_id != fid` ⇒ `slot_recycled++`, no sample (E7)
5. Otherwise accept; `progressed` = (`frame_id` or `cursor_us` differs from previous accepted sample)
6. Staleness = `now_ns() − join64(t_tick_hi, t_tick_lo)`, floored at 0
7. Poll mode: pace to the next absolute deadline. Anchor mode: if `!progressed`, park per E4.

Step 1 must precede step 3. Reading `futex_word` *after* the progress check would
reopen the classic lost-wakeup window that the whole sequence-word scheme closes.

### 8.4 Verify checks

With `--verify`, against the previous accepted sample:

- `cursor_us <= e_total_us`
- `notch_count ∈ [1, 32]`
- within a frame: `cursor_us` non-decreasing
- within a frame: `fired_lo` is a superset of the previous `fired_lo`
- within a frame: `e_total_us` unchanged

These detect writer-side bugs and ordering violations. Counted separately from
`torn`, which is normal contention (§5.3).

### 8.5 Output

**JSONL** (`--out`), one line per accepted sample, line-buffered, never to a terminal:

```json
{"t_ns":…,"frame":…,"cursor_us":…,"e_total_us":…,"snap_us":…,"fired":…,"stale_ns":…,"retries":…}
```

The schema is fixed by spec §6.5 and **cannot be extended**, which constrains the
trace design in §10.

**Summary to stderr** on exit, one metric per line, in spec order: mode, samples,
reads_ok, retries, odd_hits, torn, eagain, slot_recycled, verify_violations,
futex_waits, writer_syscalls, staleness mean and max (µs). Anchor mode adds wake
latency p50/p90/p99 with sample count; poll mode prints the period. Percentiles come
from a sorted bounded reservoir (cap 65536).

---

## 9. Error handling

| Condition | Response | Rationale |
|---|---|---|
| Path under `/mnt/` | Refuse at startup | DrvFs is 9p; neither `MAP_SHARED` coherency nor futex keys are dependable, and it presents as a lost-wakeup bug |
| File < 7552 bytes | Consumer exits, names both sizes, suggests producer not running | Most likely cause is a race with producer startup |
| Bad magic / ABI mismatch | Consumer exits, prints both full sets | Silent misinterpretation is the worst available failure |
| `-EAGAIN` after 8 retries | Count, drop sample, continue | Transient contention; a dropped sample is recoverable, a wrong one is not |
| `SCHED_FIFO`/`mlockall` denied (`--fifo`) | Warn, continue | Spec §5.1: never exit. WSL2 is unlikely to grant it |
| `EINTR` from `clock_nanosleep` | Retry to the same absolute deadline | Preserves the no-drift property |
| Deadline overrun | Resync to now | Prevents catch-up bursts corrupting latency data |
| `futex` `ETIMEDOUT` | Count, loop | Converts dead producer into a statistic (E8) |
| SIGINT/SIGTERM | Flag, exit loop, **print summary** | The summary is the measurement |

---

## 10. Test and trace strategy

### 10.1 Acceptance tests

T1–T9 per spec §7, run in order, with actual numbers recorded pass or fail. No test
is skipped to continue, no test is adjusted to pass, and a failure is reported with
its numbers and a diagnosis rather than a spec-changing fix.

T8 and T9 are the two that make this rig evidence rather than a demo, and the spec
correctly predicts they are the two most likely to be skipped. They will not be.

### 10.2 How the exchanges get traced

The report must show the **actual** exchanges of §6, not merely counters consistent
with them. Spec §0 forbids adding features and §10 excludes reporting scripts, so
the rig may not grow instrumentation to make its own case. **All trace evidence is
therefore observational and external**, from four independent channels:

| # | Channel | Evidences | Availability |
|---|---|---|---|
| 1 | `strace -f -ttt -e trace=futex` | The wire exchange: opcode, `uaddr`, compared value, return, timestamp. **Discriminates op 0 from op 128 — the only channel that does.** | ✅ verified working, strace 6.8 (§11.1) |
| 2 | `--out` JSONL | Sample-level exchange: frame, cursor, fired bitmask, staleness per accepted sample | ✅ built in (spec §6.5) |
| 3 | Summary counters both sides | Aggregate: `writer_syscalls` vs `futex_waits`, torn/odd/eagain/recycled | ✅ built in (spec §6.6) |
| 4 | `/proc/<pid>/wchan`, `/proc/<pid>/status` | Independent witness that the consumer is genuinely parked (`futex_do_wait`) and how many voluntary context switches it took | ✅ verified working (§11.1) |

`/proc/<pid>/syscall` — which would have shown opcode and compared value without
ptrace — is **blocked by `yama/ptrace_scope=1`** on this host and is not available.

**Correlation method.** JSONL carries `CLOCK_MONOTONIC` nanoseconds; `strace -ttt`
carries epoch seconds. Rather than depend on clock alignment, the report correlates
by **sequence and inter-arrival delta**: the *n*-th `FUTEX_WAKE` in the producer
trace is matched to the *n*-th progressed anchor sample in the consumer JSONL, and
the delta series is compared. Absolute alignment is a cross-check, not the basis.

**Distortion caveat, stated up front.** `strace` stops the traced process twice per
syscall via ptrace. Under trace, the producer's pacing and the consumer's wake
latency are both badly distorted. Traced runs are therefore **separate, dedicated,
short runs at reduced fps**, and no latency figure from a traced run appears as a
measurement. The report will state this explicitly wherever a trace excerpt is
shown.

### 10.3 What the report will show per exchange

| Exchange | Evidence |
|---|---|
| E0 attach | Consumer stderr on success; deliberate size/magic/ABI mismatches showing all three hard exits |
| E1 frame open | JSONL excerpt at a frame boundary: `frame` increments, `cursor_us` resets to 0, `fired` resets |
| E2 delta tick | JSONL excerpt across one frame: `fired` bitmask filling 1 bit at a time, no futex line in the trace |
| E3 gated anchor | T5: `writer_syscalls == 0` with a poll consumer attached and sampling |
| E4 wake | Interleaved trace: producer `FUTEX_WAKE` ↔ consumer `FUTEX_WAIT` return, with deltas |
| E5 race | `writer_syscalls` vs anchors-elapsed deficit; wake-latency outliers at the next-anchor distance |
| E6 torn/retry | `torn`/`odd_hits`/`retries` counters, T3 vs T6@120fps |
| E7 recycle | `slot_recycled` counter (expected 0; reported as untested if 0) |
| E8 timeout | `ETIMEDOUT` returns after killing the producer mid-run |
| E9 broken | T8 side-by-side with T4: wait/wake ratio and the 100 ms latency floor |

---

## 11. Risks and limits

### 11.1 Pre-implementation validation already performed

A throwaway probe (scratchpad, not a deliverable) forked two processes sharing an
ext4 `MAP_SHARED` page and exercised both futex opcodes on this host:

| Opcode | `FUTEX_WAKE` return | Waiter outcome |
|---|---|---|
| 0 (shared) | **1 waiter woken** | returned 0 after 300 ms (when woken) |
| 128 (private) | **0 waiters woken** | returned −1 after the full 3000 ms timeout |

Two core assumptions are therefore confirmed *before* implementation: the
inode-keyed shared futex works across processes on ext4 under WSL2, and the private
flag produces exactly the silent-loss failure mode E9/T8 predicts. `wchan` read
`futex_do_wait` in **both** cases, confirming it witnesses the park but **cannot**
discriminate the opcode.

The same probe under `strace -f -ttt -e trace=futex` (strace 6.8, installed by the
user mid-design) confirms channel 1 of §10.2 renders the exchange exactly as the
report needs it.

Shared, opcode 0 — the wake lands:

```
[pid 110163] 1786660591.304740 futex(0x77f67c82c000, FUTEX_WAIT, 0, {tv_sec=3, tv_nsec=0}
[pid 110162] 1786660591.607203 futex(0x77f67c82c000, FUTEX_WAKE, 2147483647) = 1
[pid 110163] 1786660591.607482 <... futex resumed>) = 0
```

Private, opcode 128 — the wake is silently lost:

```
[pid 110228] 1786660605.171469 futex(0x7d5ed9ee1000, FUTEX_WAIT_PRIVATE, 0, {tv_sec=3, tv_nsec=0}
[pid 110227] 1786660605.474649 futex(0x7d5ed9ee1000, FUTEX_WAKE_PRIVATE, 2147483647) = 0
[pid 110228] 1786660608.171996 <... futex resumed>) = -1 ETIMEDOUT (Connection timed out)
```

Three things this establishes about the trace method:

1. **strace decodes the private flag as a distinct opcode name**, so E9/T8's failure
   is visible on the wire rather than inferred from counters.
2. **The `FUTEX_WAKE` return value is the number of waiters actually woken** — `1`
   versus `0`. This is a direct measurement of whether a wake landed, independent of
   anything either process reports about itself.
3. `strace` launching a child works under `yama/ptrace_scope=1`; only `-p` attach to
   a non-descendant would be blocked. Both rig processes will be launched under
   strace rather than attached.

**One asymmetry to exploit in the real rig.** This probe used `fork()`, so both
sides happened to share a virtual address. The producer and consumer are separate
`exec`'d processes and will map the file at **different virtual addresses**. The
trace will therefore show a `FUTEX_WAKE` on address *X* waking a `FUTEX_WAIT` on
address *Y*, with *X ≠ Y* — which is direct visual evidence that the rendezvous is
keyed on `(inode, page offset)` and not on `(mm, vaddr)`. That is a stronger
demonstration than this probe could produce, and the report will show it.

**Distortion is visible even here:** the wake→resume delta in the shared trace is
279 µs, against a probe that does nothing but sleep. That is ptrace overhead, and it
is why §10.2 mandates traced runs be separate from measured runs.

### 11.2 What this environment cannot establish

Restated from spec §8 so it is designed-for, not discovered late. The report must
state these as limits and must not present any result as contradicting them.

- **Memory-ordering correctness is not tested here.** x86-64 is TSO. The release and
  acquire fences in §5.1–5.2 compile to almost nothing, and a missing or misplaced
  barrier passes silently. aarch64 is weakly ordered and reorders both loads and
  stores. For a seqlock, **the ordering is the algorithm** — so the property this rig
  exists to demonstrate is precisely the one this host cannot check. `qemu-user` does
  not help; it does not faithfully model the weak memory model. Only real aarch64
  hardware settles it.
- **No latency figure transfers.** WSL2 is a VM, `SCHED_FIFO` is unlikely to be
  granted, and there is no competing render workload. Magnitudes are unusable; only
  the ordering between modes is indicative.
- **Futex viability on the eventual driver mapping is untested.** This rig uses a
  file-backed mapping with a stable inode-derived key. Driver memory mapped via
  `remap_pfn_range` or a dma-buf over carveout is `VM_PFNMAP`, has no page or inode
  key, and a shared futex against it is expected to fail. Settled on device, not here.
- **Zero retries does not mean the retry path works.** If `retries` and `torn` come
  back 0, the path is **untested, not proven cheap**, and will be reported that way.

**No claim about aarch64, on-device behaviour, or the driver mapping will appear in
the report.**

### 11.3 Design-specific risks identified here

| Risk | Impact | Handling |
|---|---|---|
| **TSan is per-process.** It has no shadow-memory visibility into the *other* process's stores through the shared mapping. | T6 likely proves "no intra-process race, and the word-wise copy is formally clean" — **not** that TSan detected the cross-process race the spec cites as its motivation. | Verify empirically during T6; report what TSan actually did and did not observe. Do not overstate. |
| **Leaked `waiters`** if a consumer dies while parked | Producer wakes forever into an empty bucket; T5 can never pass on a reused file | Run T5 against a fresh backing file; report the fragility (§6.2 E3) |
| **E5-D missed wake** | One anchor's wake lost, bounded by 11.62 ms | Implement as specified, report as observation, do not fix |
| **T4's ±2% band** may be tight | `futex_waits` vs `writer_syscalls` need not couple 1:1 under timeouts or multi-anchor catch-up | Report actual ratio with numbers; diagnose rather than tune |
| **`strace` distorts what it measures** (two ptrace stops per syscall; 279 µs wake latency observed on a do-nothing probe) | A traced run's latencies are not the design's latencies | Traced runs are separate, dedicated, short and at reduced fps; no latency figure from a traced run is reported as a measurement (§10.2) |
| `/proc/<pid>/syscall` blocked by `yama/ptrace_scope=1` | Cannot read a blocked task's syscall args without ptrace | Not needed — strace covers it; `wchan` remains as an independent park witness |

---

## 12. Open questions and underspecified items

Per spec §0, none of these are resolved by invention. Each is either resolved by a
stated rule of precedence or left unimplemented and reported in `REPORT.md` §9.4.

### 12.1 Closed by spec v1.1

Four items raised against v1.0 were resolved in the specification rather than by
this document, and are no longer gaps. They are listed so the trail is legible.

| # | Item | Resolved by |
|---|---|---|
| Q1 | §4.5 prose said "both seq_cst" while its code used `FPB_LOAD_ACQ` for the consumer's re-check | **v1.1 §4.5 + new §4.6.** Both store-load pairs are now explicitly `seq_cst`. Filed separately from Q-E5 in error — they were one defect; see E5-D′. |
| Q2 | T2 required the producer to report `futex_wakes`, but no producer output was specified anywhere | **v1.1 §5.6.** Exit line fixed as `producer: frames=<N> ticks=<N> futex_wakes=<N>`, not suppressed by `--quiet`, and `futex_wakes`/`writer_syscalls` declared one counter under two names. |
| Q8 | Where traced exchanges belong, given §9's "these sections and nothing else" | **v1.1 §9.3.** Traces are admissible as evidence for the test they substantiate, kept with that test, and no latency measured under tracing may be reported. |
| — | §11.3's claim that TSan cannot see across the process boundary | **v1.1 §8 + T6 criterion.** Conceded in-spec; T6 now instructs reporting the weaker claim, and directs inspecting the copy function by eye. |

### 12.2 Still open

| # | Item | Status |
|---|---|---|
| Q3 | **`header.heartbeat_ms` has no defined value.** §5.4 step 5 says "update `heartbeat_ms`" but never states whether it is ms since start, monotonic ms, wall-clock ms, or a delta. No consumer check reads it. | **Left semantically undefined**; a monotonic ms-since-start value is written via an accessor (§3.6 requires accessor access but not an ordering), so the field is not garbage. Ambiguity reported. Nothing depends on it. |
| Q4 | **`payload.flags` is declared and never given meaning.** No producer rule sets it, no consumer check reads it. | **Left as zero.** Reported. |
| Q5 | **`snap_us = elapsed_us − offset_us`** — "elapsed_us" is not formally defined. | Read as `now − t_open` for the current frame, the only reading consistent with the field being an anchor reconciliation. Reported. |
| Q6 | **`--jitter-pct` distribution unspecified** (uniform? symmetric? applied to offset or to gap?). | Read as symmetric uniform `±jitter_pct` of the base offset, with monotonicity repair per §5.3. Reported. |
| Q7 | **`fired_hi` is unreachable** with 12 milestones. `--verify` only checks `fired_lo`. | Carried as specified, always 0. Noted, not a gap. |
| Q9 | **This document itself** is not in the spec's §1 deliverable list. | Deviation, at user request. Recorded in §9.4. |
| Q10 | **`header.magic` is not in §3.6's accessor list**, so it is a plain field, yet §4.3 requires it be published last after a release fence and the consumer reads it to gate the layout. A plain store racing a plain load is formally a data race even with the fence. | Implemented exactly as specified — `FPB_FENCE_REL()` then plain store; plain load on the consumer. The fence gives the required ordering in practice. Recorded as an observation, not a deviation. |

---

## 13. Traceability

The specification ends at §10; this document runs to §14. The numbering domains
are independent. This table is the full two-way mapping, checked so that no
specification section is left without a design counterpart.

### 13.1 Specification → design

| Spec § | Title | Design § | Implemented in |
|---|---|---|---|
| §0 | How to use / override rules | §1.4, §14 | — |
| §1 | Deliverables | §1.2, §0 | — |
| §2.1 | Environment | §9, §11.1 | both `main()` path checks |
| §2.2 | Build commands | §2.2 (N2) | `Makefile: host` |
| §2.3 | Dialect check | §2.2 (N3), §4.3 | `Makefile: dialect` |
| §3.1 | Duplication deliberate | §4.1 | ABI sentinel block, both files |
| §3.2 | Constants | §4.2 | ABI block |
| §3.3 | Structures | §4.2 | ABI block |
| §3.4 | Accessors | §4.3, §4.4 | ABI block |
| §3.5 | Required assertions | §4.1, §4.2 | ABI block |
| §3.6 | Accessor-only fields | §4.4 | both files |
| §4.1 | Seqlock write side | §5.1 | `fpb_write_begin/end` (producer) |
| §4.2 | Seqlock read side | §5.2, E6 | `fpb_read_slot`, `fpb_copy_payload` (consumer) |
| §4.3 | Publication order | E0, E1 | producer init + `cur_frame` release store |
| §4.4 | Slot addressing / recycling | §5.4, E7 | consumer `frame_id != fid` check |
| §4.5 | Wake policy | E3, E4 | producer anchor path, consumer park path |
| **§4.6** | **Why unconditional + seq_cst** *(new in v1.1)* | **E5, E5-D, E5-D′** | comments at both call sites |
| §5.1 | Producer CLI | §7.1 | `fpb_usage`, arg loop |
| §5.2 | Backing store | §7.2, §3.2 | producer init |
| §5.3 | Replay profile | §7.3, §6.3 | `fpb_profile`, `fpb_build_schedule` |
| §5.4 | Frame lifecycle | §7.4, E1–E4 | producer frame loop |
| §5.5 | Pacing | §7.5 | `fpb_sleep_until_ns` |
| **§5.6** | **Producer summary** *(new in v1.1)* | **§7.6** | final `fprintf` |
| §6.1 | Consumer CLI | §8.1 | `fpb_usage`, arg loop |
| §6.2 | Attach validation | §8.2, E0 | three hard exits |
| §6.3 | Read loop | §8.3 | consumer main loop |
| §6.4 | Verify checks | §8.4 | `if (verify)` block |
| §6.5 | JSONL output | §8.5 | `fprintf(out, ...)` |
| §6.6 | Summary to stderr | §8.5 | final summary block |
| §7 | Acceptance tests T1–T9 | §10.1 | REPORT.md §9.3 |
| §8 | What cannot be established | §11.2 | REPORT.md §9.5, TUTORIAL §12 |
| §9.1–9.5 | Report | §10.3, §12 | REPORT.md |
| §10 | Out of scope | §1.4 | — (see D1/D3 deviations) |

**No specification section is unmapped.** The v1.1 additions §4.6 and §5.6 were
the two that had no counterpart when this document was first written; §7.6 was
added for §5.6, and E5 was rewritten for §4.6.

### 13.2 Design → specification

Sections of this document with no specification counterpart, i.e. things this
document adds rather than restates:

| Design § | Content | Status |
|---|---|---|
| §6.1 | Exchange channel taxonomy | Design aid; no spec counterpart needed |
| §6.2 E0–E9 | The exchange catalogue | Requested by the user; cited by REPORT.md §9.3 |
| §6.3 | Steady-state budget arithmetic | Derived from §5.3; predictions checked in REPORT.md §9.5 |
| §6.4 | Staleness predictions | Pre-registered predictions; outcome in REPORT.md §9.5 |
| §10.2 | Trace capture method | Enabled by v1.1 §9.3 |
| §11.1 | Pre-implementation probe | Design validation; not a deliverable |
| §11.3 | Design-specific risks | Feeds REPORT.md §9.4/§9.5 |
| §12 | Open questions | Feeds REPORT.md §9.4 |
| §14 | Implementation plan | Follows spec §0's ordered list; own numbering |

---

## 14. Implementation plan

Spec §0 fixes the order. Each step gates the next.

1. **ABI block in both files, compiling.** Do not proceed until every §3.5 assertion
   passes in both files and under both `cc` and `g++`. This is the step where the
   offset table is transcribed and T9's premise is established.
2. **Protocol helpers** — seqlock read/write, word-wise copy, `cpu_relax`, clock helpers.
3. **Producer** — backing store, schedule generation, frame loop, pacing, wake policy.
4. **Consumer** — attach validation, read loop, verify, JSONL, summary, both modes.
5. **T1 → T9 in order**, no skipping past a failure, no adjusting a test to pass.
6. **Trace capture** per §10.2, as dedicated runs separate from the measurement runs.
7. **`REPORT.md`** per spec §9, including the §9.4 gaps from §12 above and the §11.2
   limits verbatim.
