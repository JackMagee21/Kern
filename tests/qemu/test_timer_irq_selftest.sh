#!/usr/bin/env bash
# Milestone 5 smoke test: boot headless in QEMU and assert the PIC was
# remapped, the timer IRQ0 self-test received real ticks, and the #BP
# self-test still resumes normally afterward instead of halting --
# proving the PIC remap, IDT IRQ gates, and PIT are actually wired
# together and firing, not just individually plausible.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_timer_irq_selftest.log"
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

check "[OK] pic/pit/keyboard/mouse initialized, IRQ0+IRQ1+IRQ2+IRQ12 unmasked"
check "[OK] timer self-test passed ("
check "ticks received via IRQ0)"

# The #BP self-test's fault dump must appear BEFORE the pic/pit line --
# proves isr_handler actually resumed execution after vector 3 instead
# of halting (the old behavior through Milestone 4).
bp_line=$(grep -n '\[PANIC\] exception: #BP Breakpoint' "$SERIAL_LOG" | head -1 | cut -d: -f1)
pic_line=$(grep -n '\[OK\] pic/pit/keyboard/mouse initialized' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$bp_line" ] || [ -z "$pic_line" ] || [ "$bp_line" -ge "$pic_line" ]; then
    echo "FAIL: #BP self-test did not resume into pic/pit init as expected" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: PIC/PIT/IRQ wired together, timer ticked, #BP resumed normally"
