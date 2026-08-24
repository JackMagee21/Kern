#!/usr/bin/env bash
# Milestone 12 (ADR 0012) smoke test: boot headless in QEMU and assert
# that a kernel-mode stack's guard page is genuinely unmapped -- proves
# kernel stacks moved off kmalloc() onto their own dedicated,
# guard-paged VA region, not just that the kernel still boots.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_guard_page_selftest.log"
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

check "[OK] guard page self-test passed (kernel stack guard page is unmapped)"

# Must run before the scheduler starts preempting tasks (proves the
# check happened against a real, already-allocated kernel stack, not
# some earlier, unrelated point in boot).
guard_line=$(grep -n '\[OK\] guard page self-test passed' "$SERIAL_LOG" | head -1 | cut -d: -f1)
sched_line=$(grep -n "scheduler initialized" "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$guard_line" ] || [ -z "$sched_line" ] || [ "$guard_line" -ge "$sched_line" ]; then
    echo "FAIL: guard page self-test did not run before the scheduler started" >&2
    fail=1
fi

# The rest of boot must still complete normally -- moving kernel stacks
# off the heap must not have broken anything downstream.
check "kernel shell -- type 'help' for commands"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: kernel-mode stack's guard page is genuinely unmapped"
