#!/usr/bin/env bash
# Milestone 14 (ADR 0014) smoke test: boot headless in QEMU and assert
# the CMOS RTC read at boot decoded into a sane wall-clock time -- not
# just that rtc_read() ran without crashing (there's no known-expected
# value to compare against, so the self-test itself checks ranges;
# this test just confirms that self-test actually ran and passed).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_rtc_selftest.log"
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

check "[OK] rtc self-test passed, boot time"

# Sanity-check the decoded year field independently of the kernel's own
# self-test: extract it and confirm it's in a plausible range, the same
# property the in-kernel self-test checks, verified again here from the
# real captured output rather than just trusting the marker's presence.
year_hex=$(grep -oP '(?<=year 0x)[0-9a-f]+' "$SERIAL_LOG" | head -1)
if [ -z "$year_hex" ]; then
    echo "FAIL: could not extract the decoded RTC year from serial output" >&2
    fail=1
else
    year=$((16#$year_hex))
    if [ "$year" -lt 2020 ] || [ "$year" -gt 2100 ]; then
        echo "FAIL: decoded RTC year ($year) is out of a plausible range" >&2
        fail=1
    fi
fi

# The rest of boot must still complete normally.
check "kernel shell -- type 'help' for commands"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: CMOS RTC read at boot decoded into a sane wall-clock time"
