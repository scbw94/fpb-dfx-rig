# The FPB Rig, Explained

A refresher on every concept this codebase depends on, built around the actual
code and the actual measured numbers. Read it start to finish, or jump to the
part you want back.

**Contents**

1. [The problem being solved](#1-the-problem-being-solved)
2. [Shared memory between two processes](#2-shared-memory-between-two-processes)
3. [Memory ordering — the hard part](#3-memory-ordering--the-hard-part)
4. [The seqlock](#4-the-seqlock)
5. [Futexes](#5-futexes)
6. [The lost wakeup](#6-the-lost-wakeup)
7. [Data races versus "it works"](#7-data-races-versus-it-works)
8. [Cache lines and false sharing](#8-cache-lines-and-false-sharing)
9. [Keeping an ABI stable across two compilations](#9-keeping-an-abi-stable-across-two-compilations)
10. [Timing without drift](#10-timing-without-drift)
11. [Walking one real exchange](#11-walking-one-real-exchange)
12. [What this rig cannot prove](#12-what-this-rig-cannot-prove)

---

## 1. The problem being solved

A phone's CPU/GPU governor wants to pick clock frequencies. To do that well it
needs to know how a frame is progressing *while the frame is still being built*,
not after it has been presented.

The obvious design is for the graphics driver to signal the governor whenever
something happens — a renderpass begins, a draw is submitted. But "something
happens" occurs roughly **720 times a second** at 60 fps with twelve milestones
per frame. Each signal is a syscall, and each syscall lands on the render
thread — the single most latency-sensitive thread in the system. You would spend
more time telling the governor about the frame than the governor could ever save
you.

The **frame progress bar** inverts it. The driver writes progress into a shared
memory region — no syscall, just a store to memory both processes can see. The
governor reads it whenever it likes. The driver only makes a syscall at three
coarse *anchor* points per frame, and **only if a governor is actually
listening**.

Measured on this rig:

| Situation | Producer syscalls per second |
|---|---|
| 12 milestones/frame at 60 fps, naive signalling | 720 |
| Anchors only, governor listening | **180** |
| Anchors only, nobody listening | **0** |

The last row is the interesting one. A reader sampling 250 times a second, fully
attached, cost the writer **zero** syscalls — confirmed by `strace` finding not a
single `futex` call in the producer's entire trace.

---

## 2. Shared memory between two processes

### The mapping

Every process has its own virtual address space. Two processes cannot normally
see each other's memory. `mmap` with `MAP_SHARED` is the exception: it maps the
same *physical* pages into both address spaces.

```c
int fd = open("/tmp/fpb.shm", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
ftruncate(fd, 7552);
void *map = mmap(NULL, 7552, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);                       /* the mapping keeps the file alive */
```

Three things worth re-absorbing:

- **`MAP_SHARED` versus `MAP_PRIVATE`.** Private gives you copy-on-write: your
  writes are yours alone. Shared means writes are visible to everyone mapping
  the same file. Getting this wrong produces a program where each side sees only
  its own changes and neither can explain why.
- **You can close the fd.** The mapping holds a reference to the underlying
  file. This is why the design needs no fd-passing between the processes — the
  path *is* the rendezvous.
- **The two processes get different virtual addresses.** This surprises people.
  In the captured trace the producer's copy lives at `0x7801e0c73024` and the
  consumer's at `0x7c3f9c54e024`. Same physical memory, different addresses.
  Section 5 explains why that matters enormously.

### Why a plain file and not `memfd_create`

A file on ext4 has a stable **inode**. As section 5 explains, the kernel derives
a futex's identity from `(inode, page offset)` for shared mappings. A path is
also something both processes can reach independently, with no coordination.
`memfd` would require passing a file descriptor over a socket — you would have to
invent a control channel to bootstrap the channel that exists to avoid control
channels.

### The filesystem matters

The spec is blunt that the file must live on ext4, never on `/mnt/c` or any
other DrvFs path under WSL. DrvFs is a 9p network-protocol mount. Neither
`MAP_SHARED` coherency nor futex key derivation is dependable over it, and — this
is the cruel part — **the failure looks exactly like a lost-wakeup bug**. You
would spend a day auditing your memory barriers when the actual problem was a
path. The producer refuses such paths outright:

```
fpb_producer: refusing '/mnt/c/tmp/fpb.shm': DrvFs path (§2.1).
```

The same reasoning applies on Android: `/data/local/tmp` is fine (real ext4 or
f2fs), `/sdcard` is not (FUSE).

---

## 3. Memory ordering — the hard part

This is the concept everything else rests on, and it is the one that rots
fastest when you haven't used it in a while.

### Your program does not execute in the order you wrote it

Two independent agents reorder your memory accesses:

1. **The compiler**, which moves loads and stores around to keep the pipeline
   fed.
2. **The CPU**, which executes out of order and buffers stores.

Single-threaded, you never notice — both are required to preserve the illusion
for *your* thread. The moment another thread or process observes your memory,
the illusion breaks and the reordering becomes visible.

### The four reorderings

Any two memory operations can potentially be reordered with respect to each
other. There are four combinations:

| Pair | Meaning |
|---|---|
| **LoadLoad** | a later load moves before an earlier load |
| **LoadStore** | a later store moves before an earlier load |
| **StoreStore** | a later store moves before an earlier store |
| **StoreLoad** | a later **load** moves before an earlier **store** |

**x86-64 is TSO (Total Store Order).** It forbids the first three and permits
**only StoreLoad**. That single permitted reordering comes from the store
buffer: your store is sitting in a queue not yet visible to anyone, while your
next load reads straight from cache and completes first.

**aarch64 is weakly ordered.** It permits essentially all four. This is the
entire reason the spec keeps insisting the fences are load-bearing even though
they compile to almost nothing on x86.

### The ordering vocabulary

```c
#define FPB_LOAD_ACQ(p)     __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define FPB_STORE_REL(p,v)  __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)
#define FPB_LOAD_SEQ(p)     __atomic_load_n(&(p), __ATOMIC_SEQ_CST)
```

- **Relaxed** — atomic (no torn value) but no ordering guarantees at all.
- **Acquire** (loads) — nothing after it in program order may move before it.
  Think of it as a one-way barrier facing downward.
- **Release** (stores) — nothing before it in program order may move after it. A
  one-way barrier facing upward.
- **Seq_cst** — acquire/release *plus* a single global total order that all
  seq_cst operations agree on.

The classic pairing: a writer fills a buffer then does a **release** store to a
flag; a reader does an **acquire** load of the flag and, if it sees the new
value, is guaranteed to see the buffer contents too. Release publishes, acquire
subscribes.

### The trap: acquire/release cannot fix StoreLoad

Look carefully at what release and acquire actually constrain:

- release store: stops **earlier** operations moving **after**
- acquire load: stops **later** operations moving **before**

Now consider a store followed by a load:

```
store X          <- release would stop earlier things crossing down
load Y           <- acquire would stop later things crossing up
```

Neither barrier faces the gap *between them*. The store can sink past the load.
**StoreLoad is the one pair release/acquire cannot constrain**, and it is
precisely the pair this protocol depends on:

```
consumer:  store waiters      →  load futex_word
producer:  store futex_word   →  load waiters
```

Both sides announce themselves, then check whether the other side announced.
This shape is the **store-buffer pattern** (you may know it as Dekker's
algorithm). If both stores can sink below both loads, both sides can read stale
zeroes and each concludes the other isn't there. Section 6 shows what that costs.

The only fix is `seq_cst` on **all four** operations. Weakening any one of them
breaks the total order that makes the argument work.

### Why this bug is invisible on x86 in this specific code

x86 *does* permit StoreLoad reordering, so you might expect the bug to be
reproducible here. It isn't, for a reason worth knowing:

The "store" on both sides is a `__atomic_fetch_add`, which x86 compiles to a
`lock`-prefixed read-modify-write instruction. The `lock` prefix drains the store
buffer as a side effect. So the reordering cannot happen — not because the
ordering argument was satisfied, but because the *instruction selection*
happened to prevent it.

On aarch64 there is no such accident. An `__ATOMIC_ACQUIRE` load may compile to
`LDAPR`, which is RCpc — deliberately weaker — and **can** be reordered ahead of a
preceding release store. The bug is real there.

> This is exactly why the spec says memory-ordering correctness is not tested by
> this rig. x86 cannot exhibit the bug, cannot exhibit the fix, and cannot tell
> them apart.

### See it for yourself

This isn't hand-waving; you can watch it happen. Both files were cross-compiled
with `aarch64-linux-gnu-gcc` and disassembled:

| Construct | aarch64 | x86-64 |
|---|---|---|
| `FPB_FENCE_REL()` | `dmb ish` | **nothing** |
| `FPB_FENCE_ACQ()` | `dmb ishld` | **nothing** |
| `FPB_STORE_REL` | `stlr` | plain `mov` |
| `FPB_LOAD_ACQ` | `ldar` | plain `mov` |
| `fpb_cpu_relax()` | `yield` | `pause` |
| **Fence instructions** | **8** | **0** |

The x86-64 binaries contain zero `mfence`, `lfence` or `sfence`. **Delete every
fence from the source and the x86 machine code is byte-identical.** That is why
no amount of testing here can validate them.

### And the acquire-versus-seq_cst distinction is real

Compile two loads at two targets and look at what comes out:

```c
uint32_t load_acq(uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
uint32_t load_seq(uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
```

| Target | acquire | seq_cst |
|---|---|---|
| `-march=armv8-a` | `ldar` | `ldar` — identical |
| `-march=armv8.3-a` | **`ldapr`** | **`ldar`** — different |

`LDAPR` is ARMv8.3's RCpc load-acquire. It is deliberately weaker than `LDAR`:
it still orders *later* accesses, but it **can** float above an earlier release
store. That is precisely the StoreLoad gap.

On the real consumer at `armv8.3-a`, the shipped seq_cst re-check emits `ldar`;
change that one macro to `FPB_LOAD_ACQ` and it emits `ldapr` instead — the weaker
load, in the one place the protocol cannot tolerate one.

Note the trap: at `armv8-a` the two spellings are *also* identical. Even
cross-compiling to the baseline ARM target would have hidden the difference. You
have to target `armv8.3-a` or later to see it at all — and then still run on real
silicon to know what the hardware does with it.

---

## 4. The seqlock

### The problem

One writer updates a 304-byte payload. Readers want a coherent snapshot. A mutex
would work but is unacceptable: the writer is a render thread, and a reader that
dies holding the lock would block it forever.

### The mechanism

A seqlock is a generation counter with one rule: **odd means a write is in
flight.**

```c
static inline void fpb_write_begin(struct fpb_slot *s)
{
    uint32_t g = FPB_LOAD_RLX(s->gen);
    FPB_STORE_RLX(s->gen, g + 1u);   /* now odd */
    FPB_FENCE_REL();                 /* odd visible BEFORE any payload store */
}

static inline void fpb_write_end(struct fpb_slot *s)
{
    uint32_t g = FPB_LOAD_RLX(s->gen);
    FPB_STORE_REL(s->gen, g + 1u);   /* payload visible BEFORE gen goes even */
}
```

The reader never blocks the writer. It takes an optimistic copy and then checks
whether it got away with it:

```c
for (unsigned i = 0; i < FPB_READ_RETRY_MAX; i++) {
    uint32_t g0 = FPB_LOAD_ACQ(s->gen);
    if (g0 & 1u) { st->odd_hits++; fpb_cpu_relax(); continue; }  /* write in flight */

    fpb_copy_payload(out, &s->p);

    FPB_FENCE_ACQ();
    if (FPB_LOAD_RLX(s->gen) == g0) { ... return 0; }   /* unchanged: snapshot is good */
    st->torn++;                                         /* changed: discard, retry */
}
return -EAGAIN;
```

Three outcomes, and the code counts them separately because they mean different
things:

| Counter | Meaning | Healthy? |
|---|---|---|
| `odd_hits` | caught the writer mid-update, didn't even try | yes — contention |
| `torn` | copied, then found `gen` had moved; discarded | yes — contention |
| `eagain` | 8 retries exhausted, sample dropped | should be rare |

**The writer is never delayed by a reader.** That property is the whole reason to
use a seqlock instead of a lock.

### Why both fences are load-bearing

- `write_begin`'s release fence guarantees the odd counter is visible *before*
  any payload store. Without it, a reader could see a still-even counter
  alongside half-written payload and accept garbage.
- `write_end`'s release store guarantees every payload store is visible *before*
  the counter goes even.
- The reader's acquire fence stops the re-read of `gen` being hoisted above the
  payload copy — which would let it validate a snapshot it hadn't finished
  taking.

On x86 these compile to nearly nothing. Delete them and every test still passes.
On aarch64 they are the algorithm.

### What a seqlock does not protect

**Identity.** The generation counter tells you the payload didn't change while
you read it. It says nothing about *which frame* you read.

There are 8 slots and `slot_index = frame_id & 7`. At 60 fps the ring wraps
every 133 ms. A reader slower than that takes a perfectly coherent, perfectly
self-consistent snapshot of **the wrong frame**, and the seqlock is entirely
happy about it. So the reader must check separately:

```c
if (snap.frame_id != fid) { slot_recycled++; }   /* discard */
```

---

## 5. Futexes

### What a futex is

**F**ast **u**serspace mu**tex**. The idea: in the common uncontended case you
should never enter the kernel at all. You only make a syscall when you actually
need to sleep or to wake someone.

Two operations matter here:

```c
syscall(SYS_futex, &word, FUTEX_WAIT, expected, &timeout, NULL, 0);
syscall(SYS_futex, &word, FUTEX_WAKE, INT_MAX,  NULL,     NULL, 0);
```

`FUTEX_WAIT` says: *atomically*, check whether `*word == expected`; if yes,
sleep; if no, return `EAGAIN` immediately. That atomicity is the entire point —
it closes the window between "I decided to sleep" and "I actually slept".

`FUTEX_WAKE` wakes up to N waiters and **returns how many it actually woke**.
That return value turns out to be the best diagnostic in the whole system.

### Private versus shared: the trap

The kernel identifies a futex by a **key**. There are two ways to compute it:

| Variant | Key | Works across processes? |
|---|---|---|
| `FUTEX_WAIT_PRIVATE` (opcode + 128) | `(mm, virtual address)` | **No** |
| `FUTEX_WAIT` (opcode, no flag) | `(inode, page offset)` | Yes |

The private variant is faster — no page lookup — and is what glibc uses
internally, so it is the one you have seen most. But two processes have two
different `mm` structures. Even at the same virtual address, they hash to
**different buckets**. Every wake goes to a bucket nobody is waiting in.

Nothing errors. The waiter simply sleeps until its timeout. It presents as a
hang, or as mysterious latency, and you will look everywhere except the flag.

This is why the code defines the opcodes by hand rather than including
`<linux/futex.h>`, where the tempting `_PRIVATE` constants live:

```c
#define FPB_FUTEX_WAIT 0
#define FPB_FUTEX_WAKE 1
```

### The measured proof

T8 built one consumer differing by a single line — `128` OR'd into the wait
opcode. The traces side by side:

```
correct (shared):
P  futex(0x7801e0c73024, FUTEX_WAKE, 2147483647) = 1
C  futex(0x7c3f9c54e024, FUTEX_WAIT, 20, {tv_sec=0, tv_nsec=100000000}) = 0

broken (private):
P  futex(0x738fa6903024, FUTEX_WAKE, 2147483647) = 0
C  futex(0x70e118e61024, FUTEX_WAIT_PRIVATE, 19, {...}) = -1 ETIMEDOUT
```

Read the **return values**. `= 1` means one waiter was woken. `= 0` means the
wake found nobody — 60 times out of 60. And every private wait ended in
`ETIMEDOUT`, 20 out of 20, while zero shared waits did.

### The address divergence

Look again at the working trace. The producer wakes address `0x7801e0c73024`.
The consumer waits on `0x7c3f9c54e024`. **Different addresses**, and the wake
lands.

That is the inode-keying made visible. Both addresses end in `024` — byte offset
36 within the page, where `header.futex_word` lives. Same inode, same page, same
offset, therefore same key, despite completely different virtual addresses.

If you want one piece of evidence that the shared futex works as designed, it is
this.

---

## 6. The lost wakeup

Now we combine sections 3 and 5, because this is where the real bug lived.

### The naive version and why it fails

The obvious way to avoid waking nobody:

```c
if (waiters != 0) {          /* somebody is listening */
    futex_word++;
    FUTEX_WAKE(...);
}
```

And the consumer:

```c
seen = futex_word;
/* ...read the data; did anything change? ... */
if (!progressed) {
    waiters++;
    if (futex_word == seen)      /* nothing happened while I was deciding */
        FUTEX_WAIT(&futex_word, seen, 100ms);
    waiters--;
}
```

Now interleave them adversarially:

```
C: seen = futex_word            (value N)
C: read the slot → nothing new
                                 P: publishes an anchor
                                 P: reads waiters → 0     ← C hasn't incremented yet!
                                 P: skips the increment AND the wake
C: waiters++                    (0 → 1)
C: re-reads futex_word → still N → goes to sleep
```

The consumer's "nothing new" decision came from a read taken *before* the anchor
landed. Its only other way to notice the anchor is the sequence word — and that
never moved, because the producer skipped the increment. It sleeps through data
that is already published.

Not a hang: the next anchor finds `waiters == 1` and wakes normally. But the
consumer is late by up to the largest anchor gap — about **11.6 ms** at 60 fps.
For a governor steering clocks inside a 16 ms frame, that is most of the frame.

### Fix, part one: increment unconditionally

```c
FPB_ADD_SEQ(h->futex_word, 1u);          /* ALWAYS */
if (FPB_LOAD_SEQ(h->waiters) != 0u) {    /* gate only the syscall */
    syscall(SYS_futex, &h->futex_word, FPB_FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    FPB_ADD_RLX(h->writer_syscalls, 1u);
}
```

Now step 3 advances the word to N+1 regardless. The consumer's re-check fails,
and it loops instead of parking.

The cost is one atomic add per anchor — 180 per second — and **no extra
syscalls**, because the syscall is still gated. The measured proof is T5: with a
poll consumer attached and sampling 250 times a second, `futex_word` climbed
181 → 916 while `writer_syscalls` stayed at **0** at every sample.

### Fix, part two: seq_cst, or the same bug comes back

Closing the gate is only half of it. Look at the shape again:

```
consumer:  store waiters      →  load futex_word
producer:  store futex_word   →  load waiters
```

That is StoreLoad on both sides — the pair section 3 explained that
acquire/release cannot constrain. With merely an acquire load on the consumer's
re-check, the hardware may hoist that load above the `waiters` store, and you
reproduce the *identical* interleaving with the gate already removed.

Both operations on both sides must be `seq_cst`. Not stylistic tightening — the
one place in this design where acquire/release is categorically insufficient.

### Why the sequence word must be a counter, not a flag

A third interleaving, which the design handles for free:

```
C: waiters++
C: re-check futex_word == N → decides to sleep, calls FUTEX_WAIT(N)
                                 P: futex_word N → N+1
                                 P: FUTEX_WAKE  (C not yet enqueued; wakes 0)
C: kernel compares *word (N+1) ≠ N → returns EAGAIN immediately, never sleeps
```

The kernel's atomic compare catches it. But **only because the word changed
value**. With a boolean flag that the producer set to 1 and the consumer had
already seen as 1, the comparison would match and the consumer would park on
stale state. A monotonically incrementing counter is never equal to itself
across an event.

### The result

| Run | `futex_waits` | `writer_syscalls` |
|---|---|---|
| T4 (60 fps) | 901 | 901 |
| T6 (120 fps, TSan) | 1801 | 1801 |
| Traced (10 fps) | 61 | 61 |

Exact equality at three different frame rates. Every anchor that found a waiter
produced exactly one wake that ended exactly one wait.

---

## 7. Data races versus "it works"

### The `memcpy` question

The reader copies 304 bytes out of a region the writer may be actively mutating.
The seqlock detects afterwards whether the copy was clean. So why not:

```c
memcpy(out, &s->p, sizeof *out);        /* NO */
```

Because "the retry logic sorts it out afterwards" is an argument about
*outcomes*, and a data race is a statement about the *program*. In C and C++, two
unsynchronised accesses to the same location where at least one is a write, and
they are not both atomic, is **undefined behaviour**. Not "returns garbage" —
undefined. The compiler is entitled to assume it never happens and optimise on
that basis.

The fix is to make every access atomic, even relaxed:

```c
const uint32_t *s = (const uint32_t *)(const void *)src;
uint32_t *d = (uint32_t *)(void *)dst;
for (size_t i = 0; i < sizeof(*dst) / sizeof(uint32_t); i++)
    d[i] = __atomic_load_n(&s[i], __ATOMIC_RELAXED);
```

76 relaxed atomic loads. Relaxed means no ordering is imposed — the seqlock still
does that job — but each individual load is now a well-defined atomic operation
rather than undefined behaviour. Same machine code on most targets. Entirely
different standing in the language.

This is also why the payload has no 64-bit fields: timestamps are split into
`_lo`/`_hi` word pairs so the whole struct is a whole number of `uint32_t` and
the loop is uniform.

### What ThreadSanitizer can and cannot tell you

TSan is excellent at finding races. It is also, in this configuration, much
weaker than it looks — for **two** independent reasons:

1. **It is per-process.** TSan maintains shadow memory for accesses *its own
   process* performs. Stores made by the other process through the shared
   mapping are completely invisible to it. It cannot see the cross-process race
   even in principle.
2. **GCC does not instrument standalone fences.** Building this code with
   `-fsanitize=thread` produces four warnings of the form
   `'atomic_thread_fence' is not supported with '-fsanitize=thread'`. So TSan's
   happens-before graph is missing the fence edges *within* each process too.

T6 passed with zero warnings. What that actually establishes is: the read path is
internally race-free and uses atomic loads rather than `memcpy`. It does **not**
establish that a cross-process race would have been caught. The honest check is
to read `fpb_copy_payload` with your eyes, which is what the spec ended up
requiring.

**Lesson worth keeping:** a clean sanitizer run is evidence about what the tool
inspected, not proof of what it did not.

---

## 8. Cache lines and false sharing

A CPU moves memory in **cache lines**, typically 64 bytes. Two variables in the
same line are, for coherency purposes, one variable: writing either invalidates
the line in every other core's cache.

That is **false sharing** — two cores fighting over a line despite touching
logically unrelated data.

Look at the slot layout:

```c
struct fpb_slot {
    uint32_t gen;             /* the seqlock counter */
    uint32_t _pad0[15];       /* 60 bytes -> pushes p to offset 64 */
    struct fpb_payload p;
    uint32_t _pad1[16];
};
```

`_pad0[15]` isn't filler. `gen` is 4 bytes and the padding takes it to exactly
64, so **`gen` gets its own cache line** and the payload starts on a fresh one.

Why it matters: readers poll `gen` constantly. The writer streams the payload
constantly. Same line, and every payload store would invalidate the line the
readers are spinning on, and vice versa. Separated, they stay independent.

Verify with the assertions the code carries:

```c
static_assert(offsetof(struct fpb_slot, gen) ==   0, "slot.gen");
static_assert(offsetof(struct fpb_slot, p)   ==  64, "slot.p");
```

---

## 9. Keeping an ABI stable across two compilations

Two programs, compiled separately, must agree byte-for-byte on a shared layout.
Nothing in the language enforces this. If they disagree, one writes `cursor_us`
where the other reads `snap_us`, and **nothing fails** — you get plausible
nonsense forever.

### Three defences

**1. Size assertions.**

```c
static_assert(sizeof(struct fpb_payload) == 304, "payload size");
static_assert(sizeof(struct fpb_slot)    == 432, "slot size");
static_assert(sizeof(struct fpb_header)  == 4096, "header must be one page");
```

**2. Offset assertions — the ones that actually matter.**

```c
static_assert(offsetof(struct fpb_payload, cursor_us) == 12, "payload.cursor_us");
static_assert(offsetof(struct fpb_payload, snap_us)   == 16, "payload.snap_us");
```

Why sizes aren't enough: swap two `uint32_t` fields and the struct is still
exactly 304 bytes. Every size check passes. The reader misinterprets two fields
indefinitely.

T9 tested precisely this. Swapping `cursor_us` and `snap_us` produced:

```
error: static assertion failed: "payload.cursor_us"
error: static assertion failed: "payload.snap_us"
```

and **zero size assertions fired**. That is the whole argument in one experiment.

**3. A runtime check at attach.** Compile-time assertions only catch a
mismatch *within* one compilation. If the two binaries were built from different
revisions, only a runtime check sees it, so the consumer validates every shape
word against its own constants and exits loudly, printing both sides' numbers.

### Why no `_Atomic` in the shared struct

```c
/* No _Atomic, no volatile, no std::atomic. */
uint32_t frame_id;
```

The ordering lives in the *accessors*, not the types:

```c
#define FPB_LOAD_ACQ(p)  __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
```

Two payoffs. First, the layout is byte-identical in C and C++, so the block can
be pasted into a C++ driver unchanged — verified by compiling both files with
`g++ -std=c++17 -fsyntax-only`. Second, `__atomic_*` builtins have the same
spelling in C11, C++ and kernel C, whereas `<stdatomic.h>` and `<atomic>` do not.

One detail that trips people: spell it `static_assert` and include `<assert.h>`.
`_Static_assert` is C-only and would fail the C++ check.

---

## 10. Timing without drift

To replay a frame schedule you must sleep to precise instants. The naive way:

```c
nanosleep(&one_frame, NULL);        /* WRONG */
```

`nanosleep` sleeps *at least* the requested time. Every scheduling delay adds to
the next sleep instead of being absorbed. Errors accumulate: over 600 frames the
replay drifts arbitrarily far from real time, and every latency number you
derive is garbage.

The right way is an **absolute deadline**:

```c
static void fpb_sleep_until_ns(uint64_t deadline_ns)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    int r;
    do {
        r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (r == EINTR && !fpb_stop);
}
```

`TIMER_ABSTIME` means "wake at this instant", not "sleep this long". Overshoot on
one deadline doesn't shift the next.

Three details:

- **`EINTR`.** A signal can cut a sleep short. Retrying to the *same absolute
  deadline* is trivially correct; with relative sleeps you'd have to compute the
  remainder.
- **Resync on overrun.** If you fall behind, don't try to catch up — that emits
  a burst of compressed ticks and corrupts every measurement. Snap the next
  deadline to now.
- **`CLOCK_MONOTONIC`, not `CLOCK_MONOTONIC_RAW`.** Monotonic never goes
  backwards (unlike `CLOCK_REALTIME`, which NTP can step). `clock_nanosleep`
  does not accept `RAW`.

---

## 11. Walking one real exchange

Everything above, in one anchor publication. This is the actual captured trace.

**Producer side**, on an anchor milestone:

```c
fpb_write_begin(s);                     /* gen even -> odd, release fence */
  p->cursor_us = ...;                   /* plain stores, protected by the seqlock */
  p->t_tick_lo = ...; p->t_tick_hi = ...;
  p->fired_lo |= 1u << m;
  p->snap_us   = elapsed_us - notch_us[m];
fpb_write_end(s);                       /* release store, gen odd -> even */

FPB_ADD_SEQ(h->futex_word, 1u);         /* unconditional; N -> N+1 */
if (FPB_LOAD_SEQ(h->waiters) != 0u) {   /* gate only the syscall */
    syscall(SYS_futex, &h->futex_word, FPB_FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    FPB_ADD_RLX(h->writer_syscalls, 1u);
}
```

**Consumer side**:

```c
seen = FPB_LOAD_ACQ(h->futex_word);     /* FIRST, before deciding anything */
fid  = FPB_LOAD_ACQ(h->cur_frame);
fpb_read_slot(&slots[fid & 7], &snap, &st);
if (snap.frame_id != fid) slot_recycled++;   /* ring wrapped, discard */
/* ... determine progressed ... */
if (!progressed) {
    FPB_ADD_SEQ(h->waiters, 1u);
    if (FPB_LOAD_SEQ(h->futex_word) == seen)
        syscall(SYS_futex, &h->futex_word, FPB_FUTEX_WAIT, seen, &to_100ms, NULL, 0);
    FPB_SUB_SEQ(h->waiters, 1u);
}
```

**And on the wire:**

```
C  1786662147.174096  futex(0x7c3f9c54e024, FUTEX_WAIT, 20, {tv_sec=0, tv_nsec=100000000}) = 0
P  1786662147.188275  futex(0x7801e0c73024, FUTEX_WAKE, 2147483647) = 1
C  1786662147.188804  futex(0x7c3f9c54e024, FUTEX_WAIT, 21, {tv_sec=0, tv_nsec=100000000}) = 0
```

Every concept is visible in those three lines:

- **different addresses** (§2, §5) — two address spaces, one inode key
- **compared value 20 then 21** (§6) — a monotonic sequence, not a flag
- **`= 1` on the wake** (§5) — exactly one waiter was really woken
- **`= 0` on the wait** (§5) — returned by a wake, not a timeout
- **the 100 ms bound** (§6) — a dead producer becomes a statistic, not a hang
- **no `_PRIVATE` suffix** (§5) — inode-keyed, so it crosses the process boundary

### Why `seen` is read first

Note the consumer reads `futex_word` **before** reading the data and deciding
whether anything progressed. Reversed, an anchor landing between the decision and
the re-check would be invisible. Reading first means any anchor after that point
necessarily changes the word, so the re-check catches it. The ordering of those
two lines is load-bearing.

---

## 12. What this rig cannot prove

The most valuable engineering habit here: being explicit about the boundary
between what you measured and what you'd like to believe.

**Memory ordering is not tested by any of this.** x86-64 is TSO. The fences
compile to almost nothing, and a missing or misplaced barrier passes every test
silently. For a seqlock the ordering *is* the algorithm, so the one property this
design most needs demonstrated is exactly the one this machine cannot check.
`qemu-user` doesn't help — it doesn't faithfully model the weak memory model.
Only real aarch64 hardware settles it. (That is why the `Makefile` has an NDK
cross-compile target — though it has never been executed and is unverified.)

**No latency figure transfers.** WSL2 is a VM, `SCHED_FIFO` was never granted,
and nothing else was competing. The 87 µs p50 is a number about this machine on
this evening. Only the *ordering* between modes is indicative.

**The futex question is not settled for the real target.** This rig maps a
file, which has a stable inode key. Driver memory mapped via `remap_pfn_range`
or a dma-buf over carveout is `VM_PFNMAP` — no page, no inode, no key — and a
shared futex against it is expected to fail outright. That question gets answered
on a device.

**Zero retries does not mean the retry path works.** T3 and T7 both reported
`retries 0, torn 0`. That means the path was *untested*, not proven cheap. It did
fire under load: T6 at 120 fps under TSan produced `odd_hits 8, torn 1,
eagain 1`. If a counter reads zero, the honest report is "not exercised".

---

## Quick reference

| Concept | One-line version |
|---|---|
| `MAP_SHARED` | same physical pages, different virtual addresses |
| Seqlock | odd counter = write in flight; readers retry, never block the writer |
| Slot recycling | seqlock protects contents, not identity — check `frame_id` too |
| Acquire | nothing after it moves before it |
| Release | nothing before it moves after it |
| **StoreLoad** | **the pair acquire/release cannot constrain — needs seq_cst** |
| x86-64 TSO | permits only StoreLoad; hides almost every ordering bug |
| aarch64 | weakly ordered; reorders loads *and* stores |
| Futex private | keyed `(mm, vaddr)` — **never** across processes |
| Futex shared | keyed `(inode, offset)` — works, and is why the file is on ext4 |
| `FUTEX_WAKE` return | number of waiters actually woken — your best diagnostic |
| Wait on a counter | a flag can equal itself across an event; a counter cannot |
| Data race | unsynchronised, one is a write, not both atomic = **undefined** |
| False sharing | pad hot counters onto their own cache line |
| Offset assertions | sizes cannot catch reordering; offsets can |
| `TIMER_ABSTIME` | absolute deadlines don't accumulate drift |
