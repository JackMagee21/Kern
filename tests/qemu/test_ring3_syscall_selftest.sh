#!/usr/bin/env bash
# Milestone 7 smoke test (message text updated by Milestone 17's ELF
# loader, ADR 0017): boot headless in QEMU and assert a ring-3 process
# actually ran in user mode, its validated sys_write syscall printed its
# message, and its sys_nop loop kept round-tripping via SYSCALL/SYSRET
# repeatedly -- not just that gdt_init()/tss_init()/syscall_init() ran
# without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_ring3_syscall_selftest.log"
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

check "[OK] tss/syscall initialized"
check "[OK] scheduler initialized, 2 kernel + 2 ring-3 processes created"
check "[OK] hello from ring 3 via ELF-loaded process"
check "[OK] syscall self-test passed, "
check "syscalls serviced from 2 ring-3 processes"

# The ring-3 message must appear AFTER the scheduler creates the task,
# not before -- proves the actual sequencing (task created, then
# scheduled, then it ran in user mode and made a real syscall), not a
# coincidental substring match.
create_line=$(grep -n '\[OK\] scheduler initialized, 2 kernel + 2 ring-3 processes created' "$SERIAL_LOG" | head -1 | cut -d: -f1)
hello_line=$(grep -n '\[OK\] hello from ring 3 via ELF-loaded process' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$create_line" ] || [ -z "$hello_line" ] || [ "$create_line" -ge "$hello_line" ]; then
    echo "FAIL: ring-3 task's message did not appear after task creation as expected" >&2
    fail=1
fi

# The syscall counter must be well beyond 1 (the one-shot sys_write) --
# proves the sys_nop loop is actually round-tripping through
# SYSCALL/SYSRET repeatedly, not just succeeding once.
count_hex=$(grep -oP '(?<=self-test passed, )[0-9a-f]+(?= syscalls)' "$SERIAL_LOG" | head -1)
if [ -z "$count_hex" ]; then
    echo "FAIL: could not extract syscall count from serial output" >&2
    fail=1
elif [ "$((16#$count_hex))" -le 1 ]; then
    echo "FAIL: syscall count was <= 1 -- sys_nop loop likely never ran" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: ring-3 task ran, validated syscall wrote its message, sys_nop round-tripped repeatedly"
