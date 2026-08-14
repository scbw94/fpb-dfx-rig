# FPB DFX rig — build system
#
# DEVIATION FROM SPEC. FPB_DFX_RIG_SPEC.md v1.1 §1 says "no build system" and
# §10 lists "a build system" as explicitly out of scope. This file exists at the
# user's direct instruction (2026-08-13), which supersedes those clauses. It is
# recorded in REPORT.md §9.4.
#
# The spec's two cc invocations remain the normative build and are reproduced
# verbatim by `make host`. Nothing here changes how the deliverables compile on
# the host; the reason this file exists is the aarch64 cross-compile, which the
# spec's §8 identifies as the only environment that can actually exercise the
# memory ordering this design depends on.
#
#   make host           the two §2.2 commands, verbatim
#   make dialect        the two §2.3 g++ checks
#   make host-tsan      ThreadSanitizer build (see WARNING below)
#   make host-asan      Address + UndefinedBehaviour build
#   make android        cross-compile for arm64-v8a via the NDK
#   make android-tsan   cross-compile with ThreadSanitizer (clang; see below)
#   make push           adb push both binaries to the device
#   make run-android    run a short anchor-mode pair on the device
#   make trace-android  same, under strace if the device has it
#   make clean

CC      ?= cc
CXX     ?= g++
CFLAGS  := -O2 -std=c11 -Wall -Wextra -Werror
SRCS    := fpb_producer.c fpb_dfxd.c
BINS    := fpb_producer fpb_dfxd

.PHONY: all host dialect host-tsan host-asan android android-tsan \
        push run-android trace-android ndk-check device-check clean

all: host

# --------------------------------------------------------------------------
# Host — these two lines are the spec's §2.2 build commands, unchanged.
# --------------------------------------------------------------------------
host:
	$(CC) -O2 -std=c11 -Wall -Wextra -Werror -o fpb_producer fpb_producer.c
	$(CC) -O2 -std=c11 -Wall -Wextra -Werror -o fpb_dfxd    fpb_dfxd.c

# §2.3 dialect check. A _GNU_SOURCE redefinition warning is expected and
# acceptable; anything else is a failure.
dialect:
	$(CXX) -std=c++17 -fsyntax-only -x c++ fpb_producer.c
	$(CXX) -std=c++17 -fsyntax-only -x c++ fpb_dfxd.c

# --------------------------------------------------------------------------
# Host sanitizers
# --------------------------------------------------------------------------
# WARNING: -Werror is deliberately absent from host-tsan. GCC's -Wtsan warns
# that __atomic_thread_fence "is not supported with -fsanitize=thread", and the
# fences are mandated by §3.4/§4.1/§4.2 and load-bearing. With -Werror the build
# cannot complete at all. Dropping -Werror is the minimal change that lets T6
# run; it does not affect what T6 measures, which is runtime TSan reports.
#
# Consequence, and it matters: GCC does not instrument those fences, so TSan's
# happens-before graph is missing the fence edges. Combined with TSan's
# inability to see across the process boundary (§8), a clean T6 establishes
# considerably less than it appears to. See REPORT.md §9.5.
host-tsan:
	$(CC) -fsanitize=thread -O2 -std=c11 -Wall -Wextra -o fpb_producer_tsan fpb_producer.c
	$(CC) -fsanitize=thread -O2 -std=c11 -Wall -Wextra -o fpb_dfxd_tsan    fpb_dfxd.c
	@echo
	@echo "NOTE: on this kernel TSan needs ASLR disabled. Run as:"
	@echo "  setarch -R ./fpb_producer_tsan --fps 120 --frames 0 --quiet &"
	@echo "  setarch -R ./fpb_dfxd_tsan --mode anchor --secs 5 --verify"

host-asan:
	$(CC) -fsanitize=address,undefined -O2 -std=c11 -Wall -Wextra -Werror -o fpb_producer_asan fpb_producer.c
	$(CC) -fsanitize=address,undefined -O2 -std=c11 -Wall -Wextra -Werror -o fpb_dfxd_asan    fpb_dfxd.c

# --------------------------------------------------------------------------
# Android / aarch64 cross-compile
#
# UNVERIFIED. No NDK, no adb and no aarch64 toolchain were present on the
# machine where this rig was developed, so these rules have never been executed.
# They are written against the documented NDK r23+ layout. Treat a first run as
# something to debug, not as a regression.
# --------------------------------------------------------------------------
NDK          ?= $(ANDROID_NDK_HOME)
ANDROID_API  ?= 30
NDK_TRIPLE   := aarch64-linux-android
NDK_HOST_TAG ?= linux-x86_64
NDK_BIN      := $(NDK)/toolchains/llvm/prebuilt/$(NDK_HOST_TAG)/bin
ANDROID_CC   := $(NDK_BIN)/$(NDK_TRIPLE)$(ANDROID_API)-clang

# /data/local/tmp is on the device's own filesystem (ext4 or f2fs), which is
# what §2.1 actually requires: a real local filesystem with a stable inode, so
# the shared futex key derives correctly. Do not put the mapping on /sdcard —
# that is a FUSE mount and has the same class of problem as DrvFs.
DEVICE_DIR ?= /data/local/tmp

ndk-check:
	@if [ -z "$(NDK)" ]; then \
	  echo "ERROR: NDK path not set."; \
	  echo "  export ANDROID_NDK_HOME=/path/to/android-ndk-r26d"; \
	  echo "  or: make android NDK=/path/to/ndk"; exit 1; fi
	@if [ ! -x "$(ANDROID_CC)" ]; then \
	  echo "ERROR: NDK clang not found or not executable:"; \
	  echo "  $(ANDROID_CC)"; \
	  echo "  Check NDK ($(NDK)), ANDROID_API ($(ANDROID_API)) and NDK_HOST_TAG ($(NDK_HOST_TAG))."; \
	  echo "  Available API levels:"; ls $(NDK_BIN) 2>/dev/null | grep -o '$(NDK_TRIPLE)[0-9]*-clang' | sort -u || true; \
	  exit 1; fi
	@echo "NDK clang: $(ANDROID_CC)"

# Same flags as the host build. -Werror is kept here: clang's TSan does support
# __atomic_thread_fence, so the GCC problem above does not apply.
android: ndk-check
	$(ANDROID_CC) -O2 -std=c11 -Wall -Wextra -Werror -o fpb_producer.arm64 fpb_producer.c
	$(ANDROID_CC) -O2 -std=c11 -Wall -Wextra -Werror -o fpb_dfxd.arm64    fpb_dfxd.c
	@echo
	@echo "Built for aarch64. NOTE: fpb_cpu_relax() compiles to 'yield' on this"
	@echo "target, and the §4.1/§4.2 fences now emit real barriers (dmb ish)"
	@echo "rather than the near-nothing they emit on x86-64."

android-tsan: ndk-check
	$(ANDROID_CC) -fsanitize=thread -O2 -std=c11 -Wall -Wextra -Werror -o fpb_producer.arm64.tsan fpb_producer.c
	$(ANDROID_CC) -fsanitize=thread -O2 -std=c11 -Wall -Wextra -Werror -o fpb_dfxd.arm64.tsan    fpb_dfxd.c

device-check:
	@command -v adb >/dev/null || { echo "ERROR: adb not found on PATH."; exit 1; }
	@adb get-state >/dev/null 2>&1 || { echo "ERROR: no device. Try 'adb devices'."; exit 1; }
	@echo "device: $$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r') " \
	      "abi=$$(adb shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"

push: device-check
	adb push fpb_producer.arm64 $(DEVICE_DIR)/fpb_producer
	adb push fpb_dfxd.arm64     $(DEVICE_DIR)/fpb_dfxd
	adb shell chmod 755 $(DEVICE_DIR)/fpb_producer $(DEVICE_DIR)/fpb_dfxd

# A 5 s anchor-mode pair, mirroring T4. The producer is backgrounded on-device
# and killed afterwards. Read the two summaries the same way as on the host.
run-android: device-check
	adb shell 'rm -f $(DEVICE_DIR)/fpb.shm; \
	  $(DEVICE_DIR)/fpb_producer --shm $(DEVICE_DIR)/fpb.shm --fps 60 --frames 0 --quiet 2>/dev/null & \
	  PP=$$!; sleep 1; \
	  $(DEVICE_DIR)/fpb_dfxd --shm $(DEVICE_DIR)/fpb.shm --mode anchor --secs 5 --verify; \
	  kill $$PP 2>/dev/null'

# Device-side tracing. Most production Android builds ship no strace; the rule
# reports that rather than pretending. simpleperf ships with the NDK and can be
# pushed if syscall-level evidence is needed.
trace-android: device-check
	@if adb shell 'command -v strace' >/dev/null 2>&1; then \
	  echo "strace present on device; tracing the consumer:"; \
	  adb shell '$(DEVICE_DIR)/fpb_producer --shm $(DEVICE_DIR)/fpb.shm --fps 10 --frames 60 --quiet 2>/dev/null & \
	    sleep 1; strace -f -ttt -e trace=futex $(DEVICE_DIR)/fpb_dfxd --shm $(DEVICE_DIR)/fpb.shm --mode anchor --secs 3'; \
	else \
	  echo "No strace on device. Options:"; \
	  echo "  - push a static strace binary to $(DEVICE_DIR)"; \
	  echo "  - use simpleperf from the NDK (\$$NDK/simpleperf/)"; \
	  echo "  - fall back to the rig's own counters: futex_waits vs writer_syscalls"; \
	fi

clean:
	rm -f $(BINS) fpb_producer_tsan fpb_dfxd_tsan fpb_producer_asan fpb_dfxd_asan \
	      fpb_producer.arm64 fpb_dfxd.arm64 fpb_producer.arm64.tsan fpb_dfxd.arm64.tsan
