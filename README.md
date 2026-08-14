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

## On a phone (Termux)

This is the interesting target: aarch64 is weakly ordered, and x86-64 cannot
exercise the memory ordering this design depends on.

Install Termux **from F-Droid** — the Play Store build is abandoned and broken on
current Android.

```
pkg install clang make binutils strace
make host
./fpb_producer --shm $HOME/fpb.shm --fps 60 --frames 0 --quiet &
./fpb_dfxd --shm $HOME/fpb.shm --mode anchor --secs 5 --verify
kill %1
```

Three things that will bite:

- **Keep the mapping on `$HOME`.** Termux's home is on the device's own
  filesystem, which is what the shared futex needs — the key derives from
  `(inode, page offset)`. `/sdcard` is FUSE and will break the wake path in a way
  that looks exactly like a lost-wakeup bug.
- **`termux-wake-lock`**, and exempt Termux from battery optimisation, or Android
  will suspend the backgrounded producer mid-run.
- **`--fifo` will warn and continue.** `SCHED_FIFO` and `mlockall` are not
  granted; this is expected and handled.

**The code has only ever been compiled by GCC.** Clang against bionic is
untested, and clang surfaces warnings GCC does not — with `-Werror` that is a
hard failure. If `make host` fails, drop `-Werror` to see what it is actually
complaining about; it will be something small.

Worth capturing first, since it decides whether the ordering question is even
observable on that chip:

```
uname -m
grep -o 'lrcpc[0-9]*' /proc/cpuinfo | sort -u
```

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
