#!/usr/bin/env bash
# Milestone 10 (ADR 0010) smoke test: boot headless in QEMU and assert
# that a ring-3 process which calls sys_exit actually terminates and
# gets its resources (address space, both stacks, the task_t itself)
# fully freed -- not just that sys_exit doesn't crash the kernel.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_process_lifecycle_selftest.log"
TIMEOUT_SECS=15

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG"

timeout "$TIMEOUT_SECS" qemu-system-x86_64 \
    -cdrom "$OS_ISO" \
    -serial "file:$SERIAL_LOG" \
    -no-reboot -no-shutdown \
    -display none \
    -monitor none \
    || true

fail=0
check() {
    local marker="$1"
    if ! grep -qF "$marker" "$SERIAL_LOG" 2>/dev/null; then
        echo "FAIL: '$marker' not found in serial output" >&2
        fail=1
    fi
}

check "[OK] process A pml4: 0x"
check "[OK] process lifecycle self-test passed, "
check "matches pre-creation baseline"

# Every ring-3 process kernel_main creates must actually have been
# reaped -- exactly seven distinct "exited and was reaped" lines (the
# two hello processes, Milestone 18's fork/wait demo process, the child
# it forks at runtime, Milestone 22's exec demo process, and Milestone
# 26's ipc demo sender/receiver pair), not fewer (sys_exit/reaping not
# landing for all of them) and not more (nothing else in this
# milestone's self-tests exits, and sys_exec reuses its caller's own
# task_t/pid rather than creating a new one -- one reap for the exec
# demo process, not two, even though it runs a second image before
# finally exiting). Milestone 18 (ADR 0018) raised this from 2 to 4;
# Milestone 22 (ADR 0022) raised it again, 4 to 5; Milestone 26 (ADR
# 0026) raised it again, 5 to 7; Milestone 27 (ADR 0027) raised it
# again, 7 to 9; Milestone 28 (ADR 0028) raised it again, 9 to 10;
# Milestone 30 (ADR 0030) LOWERED it back down, 10 to 9 -- the display
# server itself became persistent and moved outside this count.
reaped_count=$(grep -cF "exited and was reaped" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$reaped_count" -ne 9 ]; then
    echo "FAIL: expected exactly 9 'exited and was reaped' messages, got $reaped_count" >&2
    fail=1
fi

# The lifecycle self-test's own pass/fail line is the real proof (it
# compares pmm_frames_free() before process creation against after full
# reaping and panics on any mismatch) -- but also independently confirm
# both processes' PML4s were still genuinely distinct beforehand, so
# this test isn't accidentally passing against a build that silently
# regressed back to Milestone 7's shared-address-space design.
pml4_a=$(grep -oP '(?<=process A pml4: 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
pml4_b=$(grep -oP '(?<=process B pml4: 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
if [ -z "$pml4_a" ] || [ -z "$pml4_b" ] || [ "$pml4_a" = "$pml4_b" ]; then
    echo "FAIL: could not confirm the two processes had distinct PML4s before exiting" >&2
    fail=1
fi

# The shell must still start after both processes exit and get reaped
# -- proves the reaper and the rest of the scheduler's ready queue keep
# working normally afterward, not just up to the point of reaping.
check "kernel shell -- type 'help' for commands"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: ring-3 process exit (sys_exit) tore down its address space/stacks with no frame leak"
