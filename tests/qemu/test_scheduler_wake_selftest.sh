#!/usr/bin/env bash
# Milestone 25 (ADR 0025) smoke test: boot headless in QEMU and assert
# scheduler_block_current()/scheduler_wake() genuinely block and resume
# a task -- not just that neither function crashes. kernel/kernel.c's
# own self-test proves this in two parts: (1) a dedicated kernel thread
# reaches its own block point and kernel_main observes it is STILL
# blocked (not yet woken) before anything wakes it -- proves a real
# block happened, not an instant no-op -- and (2) that same thread later
# resumes and sets its own "woke up" flag only after a SECOND thread
# explicitly calls scheduler_wake() on it. Ordering is deterministic by
# construction (an explicit go/no-go handoff flag between the two test
# threads and kernel_main), not a tuned timing margin.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_scheduler_wake_selftest.log"
TIMEOUT_SECS=10

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

check "[OK] blocking/wake self-test: task genuinely blocked, confirmed not yet woken"
check "[OK] blocking/wake self-test passed, task correctly resumed after scheduler_wake()"

# kernel_main itself panics if the blocker task is ever observed already
# woken at the point it should still be blocked -- a PANIC line here
# (instead of the first [OK] marker above) would already have failed
# the check() call, but assert explicitly so a future refactor that
# silently swallows the panic still gets caught.
if grep -qF "blocking/wake self-test failed" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the blocker task was observed already woken before scheduler_wake() was ever called" >&2
    fail=1
fi

# The "still blocked" observation must appear strictly BEFORE the
# "resumed" one -- proves real sequencing (block happened, was
# confirmed, THEN wake happened), not a coincidental substring match or
# a race where both just happen to appear in either order.
blocked_line=$(grep -n '\[OK\] blocking/wake self-test: task genuinely blocked' "$SERIAL_LOG" | head -1 | cut -d: -f1)
resumed_line=$(grep -n '\[OK\] blocking/wake self-test passed' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$blocked_line" ] || [ -z "$resumed_line" ] || [ "$blocked_line" -ge "$resumed_line" ]; then
    echo "FAIL: the 'resumed' message did not appear after the 'still blocked' message as expected" >&2
    fail=1
fi

# The blocked/woken kernel thread never exits (task_create() threads
# never do) and holds no pmm-tracked resources beyond its own kernel
# stack (same as the Milestone 6 demo tasks), so the existing process-
# lifecycle frame-leak self-test is an independent, already-passing
# check that this milestone didn't regress it -- re-checked here too.
check "[OK] process lifecycle self-test passed, "

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: scheduler_block_current()/scheduler_wake() genuinely block and resume a task, in the correct order"
