# FPB DFX Rig — Report

Against `FPB_DFX_RIG_SPEC.md` **v1.1**.

---

## 9.1 Environment

| Item | Value |
|---|---|
| Kernel | `6.18.33.2-microsoft-standard-WSL2` |
| WSL version | WSL2 (confirmed: `/proc/version` contains `microsoft-standard-WSL2`) |
| Distribution | Ubuntu 24.04.4 LTS |
| Host name | Monet |
| C compiler | `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| C++ compiler | `g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| strace | 6.8 |
| CPU | AMD Ryzen AI 9 HX 370 w/ Radeon 890M, 24 logical cores |
| Architecture | x86-64 |
| shm path | `/tmp/fpb.shm` |
| shm filesystem | **ext4** on `/dev/sdd`, confirmed by `df -T /tmp/fpb.shm` |

The §2.1 requirement is satisfied: this is WSL2, not WSL1, and the shared file is
on the ext4 root filesystem, not on a DrvFs (`/mnt/*`) path.

### Second environment — aarch64

After the x86-64 work was complete, the user requested an Android/aarch64 build
path. That produced a second execution environment, and T1–T9 were re-run on it.
All results from it are labelled as such and kept separate; they do not
retroactively license any claim about the x86-64 runs.

| Item | Value |
|---|---|
| Host | GitHub Actions public ARM runner, `ubuntu-24.04-arm` |
| Kernel | `6.17.0-1020-azure` / `6.17.0-1022-azure`, `aarch64` |
| CPU | ARM Ltd (implementer `0x41`), part **`0xd49` — Neoverse N2** |
| Relevant features | `atomics`, **`lrcpc`**, **`ilrcpc`** (FEAT_LRCPC and FEAT_LRCPC2 both present) |
| Compilers | `cc`/`g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| Runs | [31778655841](https://github.com/scbw94/fpb-dfx-rig/actions/runs/31778655841) (commit `b485b83`), [31808328407](https://github.com/scbw94/fpb-dfx-rig/actions/runs/31808328407) (commit `d6d6121`, post-D10) |

This is real silicon, not emulation, and it is weakly ordered. The presence of
`lrcpc` matters specifically: without FEAT_LRCPC there is no `ldapr` instruction
and the acquire/seq_cst distinction of §4.6 is not observable at all.

---

## 9.2 Build

All four commands exit 0.

| # | Command | Exit | Output |
|---|---|---|---|
| 1 | `cc -O2 -std=c11 -Wall -Wextra -Werror -o fpb_producer fpb_producer.c` | 0 | none (0 bytes) |
| 2 | `cc -O2 -std=c11 -Wall -Wextra -Werror -o fpb_dfxd fpb_dfxd.c` | 0 | none (0 bytes) |
| 3 | `g++ -std=c++17 -fsyntax-only -x c++ fpb_producer.c` | 0 | warning below |
| 4 | `g++ -std=c++17 -fsyntax-only -x c++ fpb_dfxd.c` | 0 | warning below |

Commands 3 and 4 emit only the `_GNU_SOURCE` redefinition warning that §2.3
explicitly permits. Quoted verbatim:

```
fpb_producer.c:9: warning: "_GNU_SOURCE" redefined
    9 | #define _GNU_SOURCE
      | 
<command-line>: note: this is the location of the previous definition
```

```
fpb_dfxd.c:10: warning: "_GNU_SOURCE" redefined
   10 | #define _GNU_SOURCE
      | 
<command-line>: note: this is the location of the previous definition
```

Neither file contains any `#ifdef __cplusplus` (verified: 0 occurrences in both).
The ABI blocks in the two files are byte-identical, verified by sha256 over the
region between the `FPB-ABI-BEGIN`/`FPB-ABI-END` sentinels:
`347365f2c2ccc4696b3944cbdb092ce83ea45ff0db52141a3605a5098d7bc091`.

---

## 9.3 Test results

> **Re-validated after D10.** The `--predict ema` mode added 116 lines to
> `fpb_producer.c` after these results were first taken, so the whole suite was
> re-run against the modified binary. Every count reproduced exactly —
> T3 `samples 1250`; T4 `futex_waits 901` = `writer_syscalls 901`;
> T5 `writer_syscalls 0`; T6 `1801` = `1801` with zero TSan warnings;
> T7 zero sanitizer reports; T9 two offset assertions and zero size assertions.
> The only figure that moved was `wake_latency_p50_us`, 87 → 47, which is the
> run-to-run latency variance §8 warns about and not a behavioural change.
> **The aarch64 suite was also re-run on ARM after D10** (commit `d6d6121`) and
> is reported below as run 2, alongside the original run 1.

| ID | Result | Actual numbers |
|---|---|---|
| **T1** | **PASS** | 4/4 commands exit 0. Only the `_GNU_SOURCE` warning (158 and 155 bytes of output on commands 3 and 4; 0 bytes on 1 and 2). |
| **T2** | **PASS** | File exactly **7552** bytes. Producer exit **0**. `producer: frames=100 ticks=1200 futex_wakes=0` — **futex_wakes = 0**. |
| **T3** | **PASS** | `verify_violations 0`, `eagain 0`, `slot_recycled 0`, `samples 1250` (target 1250 ±15% = 1062–1437; hit exactly). |
| **T4** | **PASS** | `verify_violations 0`. `futex_waits 901` vs `writer_syscalls 901` — **delta 0, 0.00%** against a ±2% band. `wake_latency_p50_us 87` < 500. |
| **T5** | **PASS** | `writer_syscalls` **0 at all 9 samples** across the 5 s window, and 0 in the final summary. Confirmed independently by strace: **zero futex syscalls** in the producer's entire trace. |
| **T6** | **PASS on criterion, with two build caveats** | Zero `WARNING: ThreadSanitizer` in both processes. Required dropping `-Werror` and disabling ASLR — see below. `eagain 1`, `odd_hits 8`, `torn 1`. |
| **T7** | **PASS** | Zero ASan/UBSan reports in both processes. `samples 1250`, `verify_violations 0`. Built cleanly *with* `-Werror`. |
| **T8** | **Criterion FAILS as written; the property it exists to prove is CONFIRMED** | See the full diagnosis below. |
| **T9** | **PASS** | Compilation failed with exit 1 on exactly two offset assertions; **zero size assertions fired**. |

### T3 — full summary block

Producer: `--fps 60 --frames 0 --quiet`. Consumer: `--mode poll --period-us 4000 --secs 5 --verify`.

```
mode poll
samples 1250
reads_ok 1250
retries 0
odd_hits 0
torn 0
eagain 0
slot_recycled 0
verify_violations 0
futex_waits 0
writer_syscalls 0
staleness_mean_us 847
staleness_max_us 3447
period_us 4000
```

Producer line: `producer: frames=331 ticks=3965 futex_wakes=0`

### T4 — full summary block

Producer: `--fps 60 --frames 0 --quiet`. Consumer: `--mode anchor --secs 5 --verify`.

```
mode anchor
samples 1804
reads_ok 1804
retries 0
odd_hits 0
torn 0
eagain 0
slot_recycled 0
verify_violations 0
futex_waits 901
writer_syscalls 901
staleness_mean_us 111
staleness_max_us 707
wake_latency_p50_us 87
wake_latency_p90_us 140
wake_latency_p99_us 252
wake_latency_samples 903
```

Producer line: `producer: frames=331 ticks=3972 futex_wakes=901`

901 wakes over 5 s is 180.2/s, matching the 3 anchors × 60 fps the profile
implies.

### T4 — syscall trace evidence (exchange E4)

Dedicated tracing run, `--fps 10`, 30 frames, consumer `--secs 2`. **Separate
from the measured run above; no latency here is reported as a measurement.**
Producer `= P`, consumer `= C`, time-sorted:

```
C  1786662147.108905  futex(0x7c3f9c54e024, FUTEX_WAIT, 19, {tv_sec=0, tv_nsec=100000000}) = 0
P  1786662147.173648  futex(0x7801e0c73024, FUTEX_WAKE, 2147483647) = 1
C  1786662147.174096  futex(0x7c3f9c54e024, FUTEX_WAIT, 20, {tv_sec=0, tv_nsec=100000000}) = 0
P  1786662147.188275  futex(0x7801e0c73024, FUTEX_WAKE, 2147483647) = 1
C  1786662147.188804  futex(0x7c3f9c54e024, FUTEX_WAIT, 21, {tv_sec=0, tv_nsec=100000000}) = 0
P  1786662147.203199  futex(0x7801e0c73024, FUTEX_WAKE, 2147483647) = 1
C  1786662147.203624  futex(0x7c3f9c54e024, FUTEX_WAIT, 22, {tv_sec=0, tv_nsec=100000000}) = 0
P  1786662147.275582  futex(0x7801e0c73024, FUTEX_WAKE, 2147483647) = 1
C  1786662147.276285  futex(0x7c3f9c54e024, FUTEX_WAIT, 23, {tv_sec=0, tv_nsec=100000000}) = 0
```

Five separate facts are visible here, and each substantiates a distinct clause
of §4.5:

1. **The addresses differ.** The producer wakes `0x7801e0c73024`; the consumer
   waits on `0x7c3f9c54e024`. These are two independently `mmap`ed processes, so
   the mapping landed at a different virtual address in each. The wake lands
   anyway. That is direct evidence the rendezvous is keyed on
   `(inode, page offset)` and not on `(mm, virtual address)`. Both addresses end
   in `024` — byte offset 36 within the page, which is `header.futex_word`.
2. **`FUTEX_WAKE … = 1`** on all 61 calls. The return value is the number of
   waiters actually woken, so every wake found and released exactly one waiter.
   Tally over the run: `61 × "= 1"`, zero `= 0`.
3. **The compared value increments monotonically** — 19, 20, 21, 22, 23 — which
   is §4.5's requirement to wait on a sequence word rather than a boolean.
4. **`FUTEX_WAIT … = 0`** on all 61 calls, and **zero `ETIMEDOUT`**. Every wait
   was ended by a real wake, not by the 100 ms bound.
5. **Zero `PRIVATE` opcodes** in either trace: `FUTEX_WAKE`/`FUTEX_WAIT`, never
   the `_PRIVATE` variants.

Counts for the traced run: 61 `FUTEX_WAKE`, 61 `FUTEX_WAIT`, 0 `ETIMEDOUT`.
Producer line `producer: frames=30 ticks=360 futex_wakes=61`, consumer
`futex_waits 61`.

### T5 — syscall trace evidence (exchange E3, the gated path)

The counter reading 0 is weaker evidence than the syscall never being made. Both
were checked. Producer traced with `strace -f -ttt -e trace=futex` while a poll
consumer ran attached for 2 s:

```
producer futex syscalls in trace : 0
producer summary                 : producer: frames=30 ticks=360 futex_wakes=0
consumer samples                 : samples 500
consumer writer_syscalls         : writer_syscalls 0
```

The trace file contains no `futex` line at all. A reader that is fully attached
and sampling 250 times a second costs the writer not one syscall.

Live sampling of the header during the 5 s measured T5 run — columns
`futex_word`, `waiters`, `heartbeat_ms`, `writer_syscalls`:

```
  0.5s   181 0  997 0
  1.0s   274 0 1514 0
  1.5s   364 0 2014 0
  2.0s   457 0 2531 0
  2.5s   548 0 3031 0
  3.0s   640 0 3547 0
  3.5s   732 0 4064 0
  4.0s   823 0 4564 0
  4.5s   916 0 5081 0
```

`futex_word` climbs 181 → 916, so the producer is performing the v1.1
unconditional increment on every anchor. `waiters` is 0 throughout, so the gate
is closed. `writer_syscalls` is 0 at every sample.

### T6 — ThreadSanitizer, two build caveats

Both are environmental, both are reported rather than worked around silently.

**(a) `-Werror` and `-fsanitize=thread` are mutually exclusive for this code
under GCC.** The build fails with:

```
fpb_producer.c:111:29: warning: ‘atomic_thread_fence’ is not supported with ‘-fsanitize=thread’ [-Wtsan]
```

three times in the producer and once in the consumer. The fences are mandated by
§3.4 and are load-bearing per §4.1/§4.2, so they cannot be removed. T6 says only
"rebuild both with `-fsanitize=thread`" and does not specify the flag set, so the
sanitizer build drops `-Werror` and keeps `-Wall -Wextra`. This does not affect
what T6 measures, which is runtime TSan reports. Recorded in §9.4.

**(b) TSan will not start on this kernel without disabling ASLR.** Both
processes died immediately with:

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x5df0fb1c4000-0x5df0fb1c5000
```

This is the known incompatibility between TSan's fixed shadow layout and the
high-entropy `mmap_rnd_bits` used by current kernels. Running both processes
under `setarch -R` resolves it. Recorded in §9.4.

Full T6 consumer summary (`--fps 120`, anchor mode, 5 s, `--verify`):

```
mode anchor
samples 3601
reads_ok 3601
retries 1
odd_hits 8
torn 1
eagain 1
slot_recycled 0
verify_violations 0
futex_waits 1801
writer_syscalls 1801
staleness_mean_us 74
staleness_max_us 507
wake_latency_p50_us 66
wake_latency_p90_us 106
wake_latency_p99_us 177
wake_latency_samples 1801
```

Producer line: `producer: frames=722 ticks=8656 futex_wakes=1801`. Zero
`WARNING: ThreadSanitizer` in either process, which is T6's stated criterion.

Per the T6 criterion text and §8, the claim made here is the weaker one: **this
run shows the read path is internally race-free and that the payload copy uses
atomic loads rather than `memcpy`. It does not show that a cross-process data
race would have been detected.** See §9.5 for why the caveat is stronger than
§8 already states.

### T7 — full summary

```
mode poll
samples 1250
reads_ok 1250
retries 0
odd_hits 0
torn 0
eagain 0
slot_recycled 0
verify_violations 0
futex_waits 0
writer_syscalls 0
staleness_mean_us 874
staleness_max_us 3586
period_us 4000
```

Zero sanitizer reports in either process. Unlike TSan, the ASan/UBSan build
succeeds with `-Werror` unchanged.

### T8 — negative control: criterion fails as written, property confirmed

The variant differs from `fpb_dfxd.c` by exactly one line:

```
< #define FPB_FUTEX_WAIT 0
> #define FPB_FUTEX_WAIT (0 | 128)   /* T8 NEGATIVE CONTROL */
```

Measured run, T4 procedure:

| Metric | T4 (shared, correct) | T8 (private, broken) |
|---|---|---|
| `samples` | 1804 | **100** |
| `futex_waits` | 901 | **50** |
| `writer_syscalls` | 901 | **904** |
| `wake_latency_p50_us` | 87 | 701 |
| `wake_latency_p90_us` | 140 | 1813 |
| `wake_latency_p99_us` | 252 | 2682 |

**The stated criterion is that `futex_waits` must greatly exceed
`writer_syscalls`. It does the opposite: 50 versus 904.** Reported as a failure
against the criterion as written, with no adjustment to the test.

**Diagnosis.** A private `FUTEX_WAIT` on a valid address is a valid, *blocking*
operation — it simply hashes into a bucket no shared `FUTEX_WAKE` will ever
touch. The consumer therefore sleeps the **full** 100 ms every time rather than
spinning through many short waits. In a 5 s run that permits at most
5000 / 100 = **50** waits, which is exactly the number observed. The criterion
appears to anticipate a consumer that fails fast and retries; the actual failure
mode is a consumer that blocks to the ceiling and so issues *fewer* syscalls,
not more.

The second clause — "wake latency must collapse to the 100 ms timeout" — also
does not hold as stated, for a different reason. §6.3 defines the wake-latency
series as the *staleness of progressed samples*, which is producer-tick-to-
observation, not time spent parked. When the consumer finally wakes from a
100 ms timeout it reads a sample the producer stamped ~1 ms ago, so staleness
stays in the millisecond range (p50 701 µs) even though every park ran the full
100 ms. The park duration does collapse to the timeout; the metric named
"wake latency" does not measure park duration.

**What the run does establish, decisively.** The purpose of T8 — "confirms the
flag matters and that T4's wakes were real rather than timeouts" — is confirmed
by the trace, which is unambiguous:

```
P  futex(0x738fa6903024, FUTEX_WAKE, 2147483647) = 0
C  futex(0x70e118e61024, FUTEX_WAIT_PRIVATE, 19, {tv_sec=0, tv_nsec=100000000}) = -1 ETIMEDOUT
C  futex(0x70e118e61024, FUTEX_WAIT_PRIVATE, 22, {tv_sec=0, tv_nsec=100000000}) = -1 ETIMEDOUT
C  futex(0x70e118e61024, FUTEX_WAIT_PRIVATE, 25, {tv_sec=0, tv_nsec=100000000}) = -1 ETIMEDOUT
```

| Trace fact | T4 (shared) | T8 (private) |
|---|---|---|
| `FUTEX_WAKE` return tally | **61 × `= 1`** | **60 × `= 0`** |
| `ETIMEDOUT` count | **0** | **20 of 20** |
| Opcode | `FUTEX_WAIT` | `FUTEX_WAIT_PRIVATE` |

Every wake in the broken configuration woke **zero** waiters; every wake in the
correct configuration woke **one**. Every private wait timed out; no shared wait
did. T4's 901 waits in 5000 ms average 5.5 ms each, far below the 100 ms
ceiling, which is only possible if they were ended by real wakes.

A further detail visible in the trace: the compared value advances 19 → 22 → 25,
stepping by 3 each time. The consumer misses exactly three anchors per 100 ms
timeout, which is the expected count at `--fps 10`.

### T9 — offset assertions catch reordering

Swapping the declaration order of `cursor_us` and `snap_us` in `fpb_dfxd.c`
only. Compilation failed, exit 1:

```
fpb_dfxd.c:128:1: error: static assertion failed: "payload.cursor_us"
fpb_dfxd.c:129:1: error: static assertion failed: "payload.snap_us"
```

**Zero size assertions fired**, which is the entire point: the swap leaves
`sizeof(struct fpb_payload)` at 304 and every size check passing, while the
reader would have misinterpreted two fields indefinitely. Only the offset
assertions caught it. Reverted afterwards; the ABI blocks were re-verified
byte-identical after the revert.

### T1–T9 re-run on aarch64 (Neoverse N2)

Same source, same commit, unmodified. Every step of the CI job completed.

Two runs, both on Neoverse N2 (`0xd49`, `lrcpc` + `ilrcpc` present). **Run 1**
is commit `b485b83`; **run 2** is `d6d6121`, after the `--predict ema` work of
D10, and exists to confirm the shipped code still passes.

| ID | x86-64 (WSL2) | aarch64 run 1 | aarch64 run 2 | Verdict |
|---|---|---|---|---|
| T1 | PASS | 4/4 exit 0 | 4/4 exit 0 | **PASS** |
| T2 | PASS | 7552, `futex_wakes=0` | 7552, `futex_wakes=0` | **PASS** |
| T3 | PASS | `samples 1249`, **`eagain 1`** | `samples 1250`, **`eagain 0`** | **FAIL then PASS — see below** |
| T4 | PASS | `901` = `901`, p50 **9 µs** | `901` = `901`, p50 **9 µs** | **PASS** |
| T5 | PASS | `writer_syscalls 0` | `writer_syscalls 0` | **PASS** |
| T6 | PASS | `1801` = `1801`, 0 TSan | `1802` = `1802`, 0 TSan | **PASS** |
| T7 | PASS | 0 reports | 0 reports | **PASS** |
| T8 | criterion fails | `50` vs `900` | `50` vs `900` | **criterion fails identically** |
| T9 | PASS | 2 offset, 0 size | 2 offset, 0 size | **PASS** |

Runs: [31778655841](https://github.com/scbw94/fpb-dfx-rig/actions/runs/31778655841),
[31808328407](https://github.com/scbw94/fpb-dfx-rig/actions/runs/31808328407).

**T3 failed its criterion on the first aarch64 run and passed on the second.**
Run 1 produced `eagain 1` against a criterion of `eagain == 0`, alongside
`odd_hits 8`; run 2 produced `eagain 0` and `samples 1250`. Both are recorded;
the first is not withdrawn.

*Diagnosis.* `eagain` means one sample exhausted all 8 retries — the writer held
the seqlock across eight consecutive read attempts — and was dropped. Two
candidates were offered after run 1: a noisy neighbour on shared CI descheduling
the producer mid-write, or a genuinely more active retry path on aarch64. **Run 2
discriminates between them.** The same code on the same runner class produced
zero, so the event is not reproducible and not a property of the architecture;
the shared-runner explanation is the surviving one. The retry path *is* more
active on aarch64 in general — T6 shows `odd_hits 11, torn 8` here against
`odd_hits 8, torn 1` on x86-64 — but being more active is not the same as
exhausting, and one non-reproducing exhaustion in two runs does not support an
ordering claim. No fix was attempted.

**T8's inversion reproduces exactly**, 50 waits against 900 wakes, matching
x86-64's 50 against 904. That the same inversion appears on both architectures
supports the §9.3 diagnosis that the criterion mis-describes the failure mode of
a private futex, rather than the behaviour being platform-specific.

### aarch64 barrier codegen at `-mcpu=native`

Compiled on the N2 runner itself, so the target is exactly the silicon executing
it:

```
== prod_native.o ==            == cons_native.o ==
      3 stlr                         3 ldapr
      3 dmb   ish                    2 ldaddal
      1 ldar                         1 yield
      1 ldaddal                      1 ldar
      1 ldadd                        1 dmb   ishld
```

Against the x86-64 build of the same source: `mfence` 0, `lfence` 0, `sfence` 0.

### The v1.0 / v1.1 ordering difference, on real hardware

The comparison from §9.5 observation 10, re-run natively on Neoverse N2 rather
than cross-compiled:

| Build | `ldar` | `ldapr` |
|---|---|---|
| v1.1, seq_cst re-check (shipped) | **1** | 3 |
| v1.0, acquire re-check | **0** | 4 |

Identical to the cross-compiled prediction at `-march=armv8.3-a`. On a machine
that reports `lrcpc` and `ilrcpc`, and compiling for that machine, the v1.0
spelling of the re-check emits `ldapr` — the RCpc load — and the v1.1 spelling
emits `ldar`.

### E0 — attach validation (evidence for the §6.2 hard exits)

All three hard exits were exercised:

```
$ ./fpb_dfxd --shm /tmp/fpb_bad.shm            # 100-byte file
fpb_dfxd: '/tmp/fpb_bad.shm' is 100 bytes, expected at least 7552.
  The producer is probably not running, or is still initialising.        exit=1

$ ./fpb_dfxd --shm /tmp/fpb_bad.shm            # 7552 bytes, zeroed
fpb_dfxd: bad magic in '/tmp/fpb_bad.shm': found 0x00000000, expected 0x30425046.
  The file is not an FPB mapping, or the producer has not published yet.  exit=1

$ ./fpb_producer --shm /mnt/c/tmp/fpb.shm
fpb_producer: refusing '/mnt/c/tmp/fpb.shm': DrvFs path (§2.1).
  Neither MAP_SHARED coherency nor futex key derivation is dependable
  over 9p. Use a path on the ext4 root filesystem (/tmp or $HOME).        exit=2
```

---

## 9.4 Deviations and gaps

This section is not empty.

### Deviations

| # | Deviation | Reason |
|---|---|---|
| D1 | **A build system exists** (`Makefile`). §1 says "no build system" and §10 lists it as out of scope. | Direct user instruction on 2026-08-13, requesting an Android cross-compile target. The user authored the spec; the instruction supersedes those clauses. `make host` reproduces the two §2.2 commands verbatim, so the normative build is unchanged. |
| D2 | **`DESIGN.md` exists**, and is not in the §1 deliverable list. | Requested by the user before implementation began. |
| D3 | **Android/aarch64 cross-compile targets exist and are PARTLY VERIFIED.** | Requested by the user. The `Makefile`'s NDK rules target Android/bionic and **have never been executed** — no NDK, no `adb` and no device were available. Separately, `gcc-aarch64-linux-gnu 13.3.0` was installed mid-session at the user's request, and both files **cross-compile and link cleanly for aarch64 with `-Werror`** under it (§9.5 observations 9 and 10). That verifies the source is valid aarch64 C and lets the emitted barriers be inspected; it does **not** verify the Android toolchain, the bionic libc, the device paths, or any runtime behaviour. |
| D4 | **T6 drops `-Werror`.** | GCC's `-Wtsan` rejects `__atomic_thread_fence` under `-fsanitize=thread`, and the fences are mandated by §3.4 and load-bearing per §4.1/§4.2. With `-Werror` the T6 build cannot complete at all. T6 specifies only "rebuild both with `-fsanitize=thread`" and not the flag set. Runtime TSan reporting, which is what T6 measures, is unaffected. |
| D5 | **T6 runs under `setarch -R`.** | TSan aborts at startup with `FATAL: ThreadSanitizer: unexpected memory mapping` on this kernel's ASLR entropy. Disabling randomisation per-process is the rootless workaround. Does not change program semantics. |
| D6 | **The producer handles SIGINT/SIGTERM.** §6.1 mandates this for the consumer; §5 does not mention it for the producer. | §5.6 requires a summary line "on exit", and `--frames 0` (required by T3, T4 and T5) has no other exit path. Without a handler the mandated summary would be unreachable in the mode the tests use. |
| D7 | **Sanitizer builds use suffixed binary names** (`fpb_producer_tsan` etc.) rather than overwriting `fpb_producer`. | Keeps the §2.2 artifacts intact while T6/T7 run. Same source, same flags otherwise. |
| D8 | **`FPB_MAP_BYTES` macro added** to the ABI block. | §3.5 requires asserting the total mapping size and the value is needed at three sites (`ftruncate`, the `fstat` check, the assertion). It names a spec-mandated quantity rather than introducing a new one. |
| D9 | **A CI workflow exists** (`.github/workflows/aarch64.yml`), and the deliverables were published to a public GitHub repository. | Direct user instruction. Same standing as D1: §1/§10 exclude build tooling, and the user overrode that. The workflow runs the §7 tests unmodified on real aarch64 and adds no capability to the deliverables themselves. It is what produced the second environment in §9.1. |
| D10 | **The producer has an opt-in `--predict ema` mode**, with `--ema-alpha` and `--drift-pct`. This changes §5.3 and §5.4's contract and adds three flags to §5.1. | Direct user instruction, following §9.5 observation 17. In v1.1 the producer stamps its own jittered schedule and then sleeps to it, so `snap_us` is non-negative by construction and half the signal is unreachable. In this mode the stamped `notch_us[]` is an EMA over previous frames' *actual* offsets — a forecast — while the actual firing times are drawn from the nominal profile scaled by an AR(1) per-frame difficulty and per-milestone jitter. `snap_us` is then a signed forecast error. **The flag is off by default and the v1.1 path is byte-for-byte the same code path**, so every T1–T9 result in §9.3 stands unchanged; this was verified by re-running the default and confirming `snap_us` still has zero negatives and `verify_violations 0`. |

### Gaps — underspecified, therefore not invented

| D11 | **`README.md` and `trace_viewer.html` exist**, neither in §1's deliverable list. | Written so the rig can be handed to someone else and run without this conversation. The README is a quickstart plus the caveats from §9.5; the viewer renders captured `--out` JSONL and strace output. Neither changes the deliverables or the results. |
| # | Gap | What was done |
|---|---|---|
| G1 | **`header.heartbeat_ms` has no defined value.** §5.4 step 5 says "update `heartbeat_ms`" without stating whether it is milliseconds since start, monotonic ms, wall-clock ms, or a delta. No consumer check reads it. | Monotonic milliseconds since producer start, written with a relaxed accessor. Nothing depends on it, and no meaning is asserted. |
| G2 | **`payload.flags` is declared and never given meaning.** No producer rule sets it; no consumer check reads it. | Left as zero. |
| G3 | **`snap_us = elapsed_us − offset_us`** — `elapsed_us` is not formally defined. | Read as `now − t_open` within the current frame, the only reading consistent with the field being an anchor reconciliation. |
| G4 | **`--jitter-pct` distribution is unspecified** — uniform or otherwise, symmetric or not, applied to the offset or to the gap. | Symmetric uniform `±jitter_pct` of the base offset, with the §5.3 monotonicity repair applied afterwards. |
| G5 | **§5.3's two invariants can conflict at extreme jitter.** "Index 11 is always `e_total_us`" and "enforce strict monotonicity" cannot both hold if jitter pushes milestone 10 to or past `e_total_us`. | Jittered values are capped so the repair can never consume the remaining headroom. At the default `--jitter-pct 8` the cap never binds (milestone 10's maximum is 14074 against `e_total_us` 14166), so it affects no reported result. |
| G6 | **T8's pass criterion does not match the failure mode**, and its second clause does not match the metric §6.3 defines. | Reported as a criterion failure with numbers and diagnosis in §9.3. The test was not adjusted. |
| G7 | **`header.magic` is not in §3.6's accessor list**, so it is a plain field, yet §4.3 requires it be published last after a release fence and the consumer reads it to gate the layout. A plain store racing a plain load is formally a data race even with the fence. | Implemented exactly as specified: `FPB_FENCE_REL()` then a plain store; plain load on the consumer. |
| G8 | **`fired_hi` is unreachable.** With 12 milestones only `fired_lo` is ever written, and §6.4 only checks `fired_lo`. | Carried as specified; always 0. |

---

## 9.5 Observations

Factual, and confined to things the specification did not anticipate.

**1. TSan's blindness here is worse than §8 states.** §8 correctly says
ThreadSanitizer maintains shadow state only for its own process, so the other
process's stores through the shared mapping are invisible. There is a second,
independent limitation: GCC does not instrument `__atomic_thread_fence` under
`-fsanitize=thread` at all, and says so — four `-Wtsan` warnings across the two
files. TSan's happens-before graph is therefore missing the fence edges *within*
each process as well. A clean T6 establishes less than even the corrected §8
text implies. Per the T6 criterion, the copy function was inspected directly:
`fpb_copy_payload` performs 76 `__atomic_load_n(..., __ATOMIC_RELAXED)` word
loads and contains no `memcpy`.

**2. The retry path fired, but only under specific conditions.** T3 and T7 both
returned `retries 0`, `torn 0`, `odd_hits 0` — per §8 that means the path was
untested in those runs, not proven cheap. It did fire elsewhere: the measured T5
run produced `retries 2, torn 2`, and T6 at 120 fps under TSan produced
`odd_hits 8, torn 1, retries 1` and the only `eagain 1` observed anywhere. §8
predicted that T6 at 120 fps was the most likely place to see it, and that is
what happened. The single `eagain` means one sample exhausted all 8 retries and
was dropped, which is the intended behaviour under heavy writer slowdown.

**3. `futex_waits` and `writer_syscalls` matched exactly, not merely within
±2%.** T4: 901 and 901. T6 at 120 fps: 1801 and 1801. The traced run at 10 fps:
61 and 61. The ±2% band anticipated some decoupling; none was observed at three
different frame rates. Under the v1.1 protocol every anchor that finds a waiter
produces exactly one wake that ends exactly one wait.

An independent repeat of T3 and T4 after a clean rebuild reproduced the counts
exactly — `samples 1250` again, `futex_waits 901` and `writer_syscalls 901`
again — while `wake_latency_p50_us` moved from 87 to 79. The counts are a
property of the protocol; the latency is a property of the evening. That
difference is a small illustration of §8's point that magnitudes here are
unusable and only the ordering between modes is indicative.

**4. The address divergence in the trace is the strongest single piece of
evidence in this report.** The producer wakes on `0x7801e0c73024` and the
consumer waits on `0x7c3f9c54e024`. Two different virtual addresses in two
different address spaces, and the wake lands. That settles the §3.2 hypothesis —
the rendezvous is keyed on `(inode, page offset)` — more directly than any
latency figure could. Both addresses share the low bits `024`, which is byte
offset 36, the location of `header.futex_word` within the page.

**5. Predicted staleness matched measurement closely.** Before implementation the
design predicted poll-mode mean staleness of 700–1000 µs and anchor-mode p50 of
30–100 µs. Measured: 847 µs and 874 µs mean in the two poll runs, and p50 of 87 µs
(T4) and 66 µs (T6). Poll-mode maximum staleness was 3447 µs against a predicted
~2500 µs bound; the excess is scheduler noise on top of the 2500 µs
FRAME_END → next FRAME_OPEN silence.

**6. A consumer killed while parked leaks a `waiters` count permanently.** The
increment and decrement bracket a blocking syscall, and nothing reaps a dead
consumer's contribution. The producer would then issue wakes to an empty bucket
forever, and T5 could never pass against that backing file. T5 was therefore run
against a freshly created file. The specification provides no reaper and none
was added.

**7. `strace` distortion is large enough to matter.** A wake-to-resume delta of
279 µs was observed on a probe that does nothing but sleep. All traced runs in
this report were dedicated short runs at reduced frame rate, and no latency from
a traced run is presented as a measurement, per §9.3.

**8. The ABI duplication mechanism worked as designed during T9.** Swapping two
fields left `sizeof(struct fpb_payload)` at 304 and every size assertion passing.
Only the per-field offset assertions failed. As §3.1 requires recording: a header
split is the correct end state.

**9. Codegen inspection: the fences emit nothing on x86-64 and real instructions
on aarch64.** Both files were cross-compiled with `aarch64-linux-gnu-gcc 13.3.0`
(`-O2 -std=c11 -Wall -Wextra -Werror`, both linked, exit 0) and disassembled.

| Construct | aarch64 (`armv8-a`) | x86-64 |
|---|---|---|
| `FPB_FENCE_REL()` | `dmb ish` × 3 | **nothing** |
| `FPB_FENCE_ACQ()` | `dmb ishld` × 1 | **nothing** |
| `FPB_STORE_REL` | `stlr` × 4 | plain `mov` |
| `FPB_LOAD_ACQ` / `FPB_LOAD_SEQ` | `ldar` × 5 | plain `mov` |
| `FPB_ADD_SEQ` / `FPB_ADD_RLX` | `ldaddal` / `ldadd` | `lock addl` × 2 each binary |
| `fpb_cpu_relax()` | `yield` | `pause` |
| **Fence instructions total** | **8** | **`mfence`=0, `lfence`=0, `sfence`=0** |

The x86-64 binaries contain **zero** fence instructions of any kind. (An apparent
20 `xchg` in the producer are all `xchg %ax,%ax`, the two-byte NOP padding
encoding, not operations.) This is §8's claim rendered concrete: removing every
`FPB_FENCE_REL`/`FPB_FENCE_ACQ` from the source would produce **identical x86-64
machine code**, so no test on this host could possibly distinguish correct
barriers from absent ones.

**10. The v1.0 → v1.1 ordering change alters the emitted instruction, but only
above `armv8-a`.** A two-function probe compiled at two targets:

| Target | `__ATOMIC_ACQUIRE` load | `__ATOMIC_SEQ_CST` load |
|---|---|---|
| `-march=armv8-a` | `ldar` | `ldar` — **indistinguishable** |
| `-march=armv8.3-a` | **`ldapr`** | **`ldar`** — **different instructions** |

Applied to the real consumer at `-march=armv8.3-a`, comparing the shipped code
against a variant differing only in the §4.5 re-check (`FPB_LOAD_SEQ` →
`FPB_LOAD_ACQ`, i.e. the v1.0 spelling):

| Build | `ldar` | `ldapr` |
|---|---|---|
| v1.1, seq_cst re-check (shipped) | **1** | 3 |
| v1.0, acquire re-check | **0** | 4 |

Under v1.1 the re-check compiles to `ldar`. Under v1.0 it degrades to `ldapr`.
The other three acquire loads (`seen`, `cur_frame`, `slot.gen`) are `ldapr` in
both builds, which is unremarkable — those only need to order against *later*
loads.

**This is a codegen observation and licenses no runtime claim.** It establishes
that the v1.1 change is not cosmetic: it selects a different instruction on
targets implementing `FEAT_LRCPC`. It does **not** establish that any reordering
occurs on any device, that the v1.0 form would fail in practice, or anything else
about aarch64 execution.

It is also a second instance of the §8 pattern: at `armv8-a` the two spellings
are *also* indistinguishable, so even a cross-compile to the baseline target
would have hidden the difference. It is visible only at `armv8.3-a` and above.

This result was subsequently reproduced natively on Neoverse N2 hardware — see
§9.3 — with identical counts.

**11. The aarch64 run does not lift §8's central limit.** T1–T9 were executed on
a Neoverse N2 that reports `lrcpc` and `ilrcpc`, and eight of nine passed. It is
tempting to read that as the memory ordering having been validated. It has not
been. A seqlock ordering bug is *probabilistic*: it requires a specific
interleaving to coincide with a specific reordering, and a five-second run at
60 fps samples a vanishingly small part of that space. Passing here is consistent
with correct barriers and also consistent with incorrect barriers that did not
happen to fire. The v1.0 variant was compiled on that hardware but never run
under stress, so nothing is known about whether it would fail in practice.

What the aarch64 run *does* establish is narrower and still worth having: the
code builds and runs correctly on a weakly-ordered machine, the protocol's
counting invariants hold there, and the §4.6 instruction-selection difference is
real on shipping silicon rather than only in a cross-compiler. Establishing that
the ordering is *correct* would need a stress harness and a deliberate attempt to
provoke the reordering, which is out of scope here.

**12. Latency on the N2 runner was an order of magnitude better, which is the
point of §8's second limit.** Wake latency p50 was **9 µs** on Neoverse N2 against
**87 µs** on WSL2, with p99 22 µs against 252 µs. Staleness mean 10 µs against
111 µs. Nothing about the protocol changed; the difference is WSL2 being a VM
versus a runner closer to bare metal. Both numbers are equally unusable as
predictions for a phone under render load, which is what §8 says.

**13. The retry path is measurably more active on aarch64.** At 120 fps under
TSan, x86-64 produced `odd_hits 8, torn 1, eagain 1`; the N2 runs at the same
settings produced `odd_hits 2, torn 15` and `odd_hits 11, torn 8`. In poll mode
at 60 fps, x86-64 produced all zeros in T3 while N2 produced `odd_hits 8,
eagain 1` on the first run and `eagain 0` on the second. The path that §8 warned might be untested is being
exercised considerably harder on the second platform. Whether the difference is
architectural or an artefact of a shared CI runner is not separable from this
data.

**14. `futex_waits == writer_syscalls` held exactly on aarch64 too.** 901 and 901
at 60 fps, 1801 and 1801 at 120 fps under TSan — the same exact equality observed
on x86-64 at three frame rates, now on a fourth and fifth data point on different
silicon. The ±2% band has yet to be approached on any platform tested.

**15. `snap_us` carries a systematic positive offset that is not schedule
deviation.** Across 37 fully-observed frames at 20 fps, the median `snap_us` was
**+111 µs at FRAME_OPEN, +122 µs at ACQUIRE and +131 µs at FRAME_END**, with a
p10–p90 spread of roughly ±40 µs about those medians. The sign is consistently
positive at every anchor including FRAME_OPEN, where the scheduled offset is 0
by definition and no schedule deviation is possible. The offset is therefore the
cost of the publish path itself — `clock_nanosleep` wake latency plus the seqlock
write — and not the quantity §5.4 describes the field as measuring. Under strace
the same figure rose to +303 µs, consistent with ptrace overhead landing in the
same path. A consumer reading this field would be reading a baseline plus a
deviation, not a deviation.

**16. Early `snap_us` does not forecast late `snap_us` in this replay, by
construction.** Correlation between the FRAME_OPEN value and the FRAME_END value
of the same frame was **−0.055**; between ACQUIRE and FRAME_END, **+0.035**. Both
are indistinguishable from zero. This follows directly from §5.3: the xorshift
jitter is drawn independently per milestone, so there is no mechanism by which an
early deviation could carry information about a later one. It is a property of
the synthetic profile, not a measurement of the design. The rig can therefore
demonstrate the *timeliness* of the signal — at ACQUIRE it is published with
15 150 µs of the 50 000 µs frame period still remaining at 20 fps — but it cannot
demonstrate that the signal's *value* is predictive of anything. That question
belongs to a real workload, where a slow draw plausibly does make the rest of the
frame slow.


**17. `snap_us` can never be negative in this replay, so half the signal's range
is unreachable.** Across 10 289 samples in four runs the minimum observed value
was **0** and no sample in any run was negative. This is structural, not
incidental. §5.5 paces with `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`,
which by contract sleeps until *at least* the deadline and cannot return early;
§5.4 then computes `snap_us = elapsed_us - offset_us` from a timestamp taken
after that sleep. Therefore `elapsed_us >= offset_us` always, and `snap_us >= 0`
always.

The deeper cause is that the replay's schedule is **self-fulfilling**. §5.4 has
the producer generate the jittered `notch_us[]` at frame open and then sleep to
its own offsets, so "scheduled" and "actual" are the same object and their
difference can only be sleep overshoot. A writer whose milestones fire when work
*completes* is measuring actual against a *prediction*, and real work can beat a
prediction — that is the case the signed field exists for, and the case this rig
structurally cannot produce.

**The plumbing for the negative case is correct but untested by the test suite.**
Verified directly: with the producer stopped, a negative value was written into
`payload.snap_us` of the live slot under the seqlock, and the consumer logged
`"snap_us":-4321` on all 500 subsequent samples. The sign survives
`fpb_copy_payload`'s word-wise `uint32_t` loads and reads back correctly as
`int32_t`. So the ABI, the copy and the JSONL path all handle the slack case; only
the producer cannot generate it.

Two consequences worth stating. A consumer exercised solely against this rig is
tested on half its input domain. And because §5.4 also clamps `cursor_us` to
`e_total_us`, an overrunning frame pins the progress bar at maximum, so the
*magnitude* of lateness is available only from `snap_us` at anchors, not from the
bar itself. Reported rather than altered; changing either would mean changing
§5.4 or §5.5.

**18. Every distribution in this rig is synthetic, and the prediction-mode
parameters were chosen rather than fitted.** No capture data was supplied and
neither binary reads any external file; §10 excludes replay from external JSON.
The complete model is:

| Quantity | Value | Provenance |
|---|---|---|
| Milestone fractions | the 12 values of §5.3 | **specified** |
| `e_total_us` | `(1e6/fps) x load_pct/100` | **specified** |
| Jitter distribution | symmetric uniform on ±`jitter_pct` | mechanism specified; **shape chosen** |
| `jitter_pct` | 8 | **specified** (§5.1 default) |
| RNG | xorshift32, seed `0x9E3779B9` | mechanism specified; **seed chosen** |
| Difficulty AR(1) coefficients | 0.75 / 0.25 | **chosen** |
| Difficulty clamp | [0.5, 1.8] | **chosen** |
| `drift_pct` | 12 | **chosen** |
| `ema_alpha` | 20 | **chosen** |

The measured trace variance reported earlier (sd 975 µs against the nominal
profile) was used to *diagnose* the sign problem of observation 17. It was **not**
fed back in as a parameter; nothing here is fitted to any measurement.

One consequence is worth stating plainly. A symmetric uniform is a poor model of
frame time: real distributions are right-skewed with a heavy tail — shader
compiles, memory stalls, thermal throttling — and this one has bounded support,
no skew and no tail. So the rig cannot produce the frames a governor most needs
to catch, and any figure derived from the shape of the distribution rather than
from the transport should be treated as illustrative. The lead-time and syscall
counts do not depend on the shape; the correlation of +0.276 in D10 does.

---

---

*On the §8 boundary, stated precisely because it moved during this work.*

*T1–T9 were executed on real aarch64 hardware (Neoverse N2, §9.1), so this report
does contain aarch64 **measurements**. It contains no claim that memory-ordering
correctness has been **established** — see §9.5 observation 11 for why eight
passing tests on weakly-ordered silicon do not amount to that. §8's first and
second limits stand: ordering correctness is not demonstrated, and no latency
figure from either environment transfers to a device under render load.*

*The remaining §8 limits are untouched. Futex viability over a `VM_PFNMAP` driver
mapping was not tested and is not testable here. ThreadSanitizer's cross-process
blindness is unchanged. No claim is made about on-device behaviour: the
`Makefile`'s Android/NDK targets were never executed, no phone was involved, and
the aarch64 results come from a Linux CI runner, not from Android or bionic.*
