#!/usr/bin/env bash
# Milestone 6 smoke test: boot headless in QEMU and assert the
# preemptive scheduler actually forced two never-yielding demo tasks to
# share the CPU -- not just that scheduler_init() ran without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_scheduler_selftest.log"
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

check "[OK] scheduler initialized, 2 kernel + 2 ring-3 processes created"
check "[OK] scheduler self-test passed, task A: 0x"
check "(both made progress under preemption)"

# Both demo task counters must be nonzero: if preemption weren't
# actually forcing the CPU away from a busy-looping task, one of them
# would still be exactly 0 -- extracted from the real serial output,
# not assumed from marker presence alone.
task_a_hex=$(grep -oP '(?<=task A: 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
task_b_hex=$(grep -oP '(?<=task B: 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
if [ -z "$task_a_hex" ] || [ -z "$task_b_hex" ]; then
    echo "FAIL: could not extract task A/B counters from serial output" >&2
    fail=1
elif [ "$((16#$task_a_hex))" -le 0 ] || [ "$((16#$task_b_hex))" -le 0 ]; then
    echo "FAIL: a demo task counter was 0 -- preemption likely not working" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: preemptive scheduler forced both never-yielding tasks to share the CPU"
