#!/usr/bin/env bash
# Milestone 13 (ADR 0013) smoke test: boot headless in QEMU and assert
# the PCI config-space scan actually found real hardware -- QEMU's
# default i440fx machine's Intel host bridge at bus 0/device 0/function
# 0 specifically, not just that pci_scan() returned without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_pci_selftest.log"
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

# The Intel host bridge every i440fx-based QEMU machine exposes at
# bus 0/device 0/function 0 -- vendor 0x8086, device 0x1237.
# console_write_hex() zero-pads to 16 hex digits, matching every other
# self-test marker in this codebase.
check "[PCI] bus 0x0000000000000000 dev 0x0000000000000000 fn 0x0000000000000000: vendor 0x0000000000008086 device 0x0000000000001237"
check "[OK] pci self-test passed ("
check "device(s) found, host bridge present"

device_count_hex=$(grep -oP '(?<=pci self-test passed \(0x)[0-9a-f]+(?= device)' "$SERIAL_LOG" | head -1)
if [ -z "$device_count_hex" ]; then
    echo "FAIL: could not extract PCI device count from serial output" >&2
    fail=1
elif [ "$((16#$device_count_hex))" -lt 1 ]; then
    echo "FAIL: PCI scan reported 0 devices" >&2
    fail=1
fi

# The rest of boot must still complete normally.
check "kernel shell -- type 'help' for commands"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: PCI config-space scan found real hardware, including the expected host bridge"
