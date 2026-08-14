# FPB DFX Rig

A two-process bench for a **frame progress bar**: a producer standing in for a
graphics driver publishes intra-frame progress into shared memory, and a reader
standing in for a userspace DVFS governor consumes it. The point is to find out
whether the driver can publish that progress cheaply enough to be worth doing —
and to be explicit about what the measurements do and do not establish.

Built against `FPB_DFX_RIG_SPEC.md` v1.1. Not included in this tree; the design
it describes is reconstructible from `DESIGN.md`.

## Build

Two files, two commands, no dependencies:

```
cc -O2 -std=c11 -Wall -Wextra -Werror -o fpb_producer fpb_producer.c
cc -O2 -std=c11 -Wall -Wextra -Werror -o fpb_dfxd    fpb_dfxd.c
```

`make host` runs exactly those. `make dialect` additionally checks both files
compile as C++17, because the shared ABI block is meant to transfer to a C++
driver unchanged.

## Run it

```
./fpb_producer --fps 60 --frames 0 --quiet &
./fpb_dfxd --mode anchor --secs 5 --verify
kill %1
```

Then read the consumer's summary. The numbers that matter:

| Metric | Healthy | Meaning |
|---|---|---|
| `verify_violations` | 0 | writer-side invariants held |
| `futex_waits` vs `writer_syscalls` | equal | every wake landed on exactly one waiter |
| `eagain` | 0 | no sample lost to seqlock retry exhaustion |
| `slot_recycled` | 0 | reader kept up with the 8-slot ring |
| `wake_latency_p50_us` | small | how fast the reader learns |

Swap `--mode anchor` for `--mode poll --period-us 4000` and `writer_syscalls`
should stay at **0** — a reader that never parks costs the writer nothing. That
asymmetry is the main claim the rig exists to test.

## On a device (NDK + adb)

```
export ANDROID_NDK_HOME=/path/to/android-ndk-r26d
make android          # cross-compiles for arm64-v8a
make push             # adb push to /data/local/tmp
make run-android      # 5 s anchor-mode pair, mirrors T4
```

**These rules have never been executed** — no NDK, adb or device was available
where this was built. They are written against the documented NDK r23+ layout
with a preflight that fails loudly. Treat the first run as something to debug.

Capture this first, because it decides whether the ordering question is even
observable on that silicon:

```
adb shell uname -m
adb shell grep -o 'lrcpc[0-9]*' /proc/cpuinfo | sort -u
```

No `lrcpc` means no `ldapr`, acquire compiles to `ldar`, and the §4.6
acquire-vs-seq_cst distinction is invisible — the same blindness as `-march=armv8-a`.

Three things that will bite:

- **Keep the mapping on `/data/local/tmp`.** That is the device's own filesystem,
  which is what the shared futex needs — the key derives from
  `(inode, page offset)`. `/sdcard` is FUSE and will break the wake path in a way
  that looks exactly like a lost-wakeup bug, so you will go hunting for a memory
  ordering problem that isn't there.
- **`--fifo` will warn and continue.** `SCHED_FIFO` and `mlockall` are not
  granted. Expected and handled.
- **The code has only ever been compiled by GCC.** The NDK is clang against
  bionic — untested. Clang surfaces warnings GCC does not, and with `-Werror`
  that is a hard build failure. If `make android` fails, drop `-Werror` to see
  the real message; it will be something small.

### What running on a device does and does not show

It shows the code builds and runs against bionic on a mobile SoC, and that the
shared futex works over a file mapping on an Android kernel. That last one is a
genuine question a server-class aarch64 runner does not answer.

It does **not** put the rig anywhere near the environment the real design targets.
This is a shell binary in `/data/local/tmp`, not a UMD process publishing to a
system daemon: different SELinux context, different privileges, different
scheduling, and a file-backed mapping rather than driver memory. In particular it
says nothing about a shared futex over `VM_PFNMAP` — see `REPORT.md` §8, which
expects that to fail outright. "It ran on a phone" is a weaker claim than it
sounds; do not let it stand in for an on-device driver integration.

Termux is a workable fallback if there is no NDK or no USB access, but it is
worse for this: an app sandbox with cgroup throttling and background suspension,
a different clang packaging, and timing noisier than CI already provides. Use it
to answer "does it build and run at all", not to measure anything.

## What's here

| File | |
|---|---|
| `fpb_producer.c` | Writer. Publishes the progress bar. |
| `fpb_dfxd.c` | Reader. Reads and logs only; the sole shared field it writes is `waiters`. |
| `REPORT.md` | Results, deviations and — importantly — the limits. Read §9.4 and §9.5. |
| `TUTORIAL.md` | The concepts: memory ordering, seqlocks, futexes, ABI stability. |
| `DESIGN.md` | Architecture and the exchange catalogue E0–E9. Predates the `--predict` mode. |
| `trace_viewer.html` | Open in a browser. Drag a `--out` JSONL onto it to see your own run. |
| `Makefile` | Host, sanitizer and Android/NDK targets. The NDK ones are **unverified**. |
| `.github/workflows/` | Runs the acceptance suite on a real aarch64 runner. |

## Read this before trusting a number

`REPORT.md` §9.5 carries seventeen observations, and several are load-bearing:

- **Memory-ordering correctness is not established**, on either machine tested.
  A seqlock ordering bug is probabilistic; a passing five-second run is
  consistent with correct barriers and with broken ones that did not fire.
- **No latency figure transfers.** p50 wake latency measured 9 µs, 47 µs and
  87 µs on three different hosts with no change to the code.
- **`snap_us` is non-negative by default**, so half the signal's range is
  unreachable unless you pass `--predict ema`. See observation 17.
- **T8's stated pass criterion fails** — reported unadjusted, with the failure
  mode diagnosed.
