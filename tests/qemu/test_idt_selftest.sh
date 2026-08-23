#!/usr/bin/env bash
# Milestone 2 smoke test: boot headless in QEMU and assert the GDT/IDT
# installed successfully AND the exception-handling path actually works
# end to end -- kernel_main deliberately triggers a #BP (int3) as its
# last action, and this checks that isr_handler's fault dump reports the
# right vector/name for it. Proves "visibility into faults" is real, not
# just that idt_init() ran without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_idt_selftest.log"
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

check "[OK] gdt/idt installed"
check "[PANIC] exception: #BP Breakpoint"
check "vector:      0x3"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: gdt/idt installed and #BP fault dump reported the right vector"
