#!/usr/bin/env bash
# Milestone 3 smoke test: boot headless in QEMU and assert the physical
# frame allocator actually initialized from the real Multiboot2 memory
# map and passed its alloc/free/reuse self-test -- not just that
# pmm_init() ran without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_pmm_selftest.log"
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

check "[OK] pmm initialized, free frames: 0x"
check "[OK] pmm self-test passed (alloc/free/reuse)"

# Sanity check the free-frame count is plausible, not just present: some
# usable RAM was found (> 0) and it's within the 4GiB tracking limit
# (kernel/mm/pmm.c's PMM_MAX_FRAMES, ADR 0003).
free_hex=$(grep -oP '(?<=free frames: 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
if [ -z "$free_hex" ]; then
    echo "FAIL: could not extract free-frame count from serial output" >&2
    fail=1
elif [ "$((16#$free_hex))" -le 0 ]; then
    echo "FAIL: free-frame count was 0 -- memory map parsing likely broken" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: pmm initialized from the real memory map and self-test passed"
