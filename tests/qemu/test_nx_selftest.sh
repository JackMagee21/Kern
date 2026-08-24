#!/usr/bin/env bash
# Milestone 11 (ADR 0011) smoke test: boot headless in QEMU and assert
# EFER.NXE was actually enabled, and that vmm_page_is_executable_in()
# correctly reports the kernel heap as non-executable while still
# reporting ordinary kernel code as executable -- proving the
# VMM_FLAG_NX plumbing actually reaches real page-table bits, not just
# that vmm_enable_nx() ran without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_nx_selftest.log"
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

check "[OK] NX (no-execute) enabled"
check "[OK] NX self-test passed (heap is non-executable, kernel code still is)"

# NX must be enabled BEFORE the kernel heap is mapped -- otherwise a
# VMM_FLAG_NX mapping created earlier would be a reserved-bit violation
# waiting to fault the first time it's actually walked.
nx_line=$(grep -n '\[OK\] NX (no-execute) enabled' "$SERIAL_LOG" | head -1 | cut -d: -f1)
heap_line=$(grep -n '\[OK\] kernel heap initialized' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$nx_line" ] || [ -z "$heap_line" ] || [ "$nx_line" -ge "$heap_line" ]; then
    echo "FAIL: NX was not enabled before the kernel heap was mapped" >&2
    fail=1
fi

# The rest of boot must still complete normally -- NX enforcement must
# not have broken anything downstream (scheduler, syscalls, shell).
check "kernel shell -- type 'help' for commands"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: NX enabled before first use, kernel heap correctly non-executable, kernel code still executable"
