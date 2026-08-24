#!/usr/bin/env bash
# Per-process address spaces smoke test: boot headless in QEMU and
# assert the two ring-3 processes actually get DIFFERENT top-level page
# tables (not just "both ran without crashing" -- test_ring3_syscall_
# selftest.sh already covers that). Extracts both printed PML4 physical
# addresses from the real serial output and checks they differ.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_process_isolation_selftest.log"
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

check "[OK] process A pml4: 0x"
check "(different address spaces)"

pml4_a=$(grep -oP '(?<=process A pml4: 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
pml4_b=$(grep -oP '(?<=process B pml4: 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
if [ -z "$pml4_a" ] || [ -z "$pml4_b" ]; then
    echo "FAIL: could not extract both processes' PML4 addresses from serial output" >&2
    fail=1
elif [ "$pml4_a" = "$pml4_b" ]; then
    echo "FAIL: both processes got the SAME PML4 ($pml4_a) -- address spaces are not actually isolated" >&2
    fail=1
fi

# Both processes' own "hello from ring 3" message must appear exactly
# twice -- proves BOTH independently loaded/ran their own private copy
# of the embedded ELF image (Milestone 17, ADR 0017) and made their own
# validated syscall, not just one of them.
hello_count=$(grep -cF "[OK] hello from ring 3 via ELF-loaded process" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$hello_count" -ne 2 ]; then
    echo "FAIL: expected exactly 2 'hello from ring 3' messages (one per process), got $hello_count" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: two ring-3 processes got genuinely distinct page tables, both ran independently"
