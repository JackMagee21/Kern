#!/usr/bin/env bash
# Milestone 1 smoke test: boot headless in QEMU and assert the kernel
# reaches kernel_main and prints its "hello kernel" marker over serial.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_boot_serial.log"
EXPECTED_MARKER="[OK] hello kernel"
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

if grep -qF "$EXPECTED_MARKER" "$SERIAL_LOG" 2>/dev/null; then
    echo "PASS: found '$EXPECTED_MARKER' in serial output"
    exit 0
fi

echo "FAIL: '$EXPECTED_MARKER' not found in serial output" >&2
echo "--- captured serial output ---" >&2
cat "$SERIAL_LOG" >&2 2>/dev/null || true
exit 1
