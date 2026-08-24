#!/usr/bin/env bash
# Milestone 18 (ADR 0018) smoke test: boot headless in QEMU and assert
# sys_fork/sys_wait actually worked end to end -- a genuinely
# independent child process ran (proving the address space really is
# independent, not aliasing the parent), the parent's sys_wait
# eventually observed it exit, and the exit code sys_wait reported back
# matches exactly what the child passed to sys_exit -- not just that
# neither syscall crashed the kernel. sys_wait became genuinely
# blocking in Milestone 20 (ADR 0020, test_blocking_wait_selftest.sh)
# and fork's address-space sharing became copy-on-write in Milestone 21
# (ADR 0021, test_cow_fork_selftest.sh); this test's own assertions are
# implementation-agnostic and didn't need to change either time.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_fork_wait_selftest.log"
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

check "[OK] fork/wait demo process created, pid 0x"
check "[OK] child process running after fork"
check "[OK] fork/wait self-test: child exit code verified"

# A [FAIL] line here would mean sys_wait reported a different exit code
# than the child actually passed to sys_exit -- exit-code propagation
# broken even if fork/wait's control flow otherwise "worked".
if grep -qF "[FAIL] fork/wait self-test: unexpected child exit code" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the fork/wait demo's own exit-code check reported failure" >&2
    fail=1
fi

# The child's message must appear AFTER the demo process is created --
# proves real sequencing (process created, scheduled, forked, child ran)
# rather than a coincidental substring match.
create_line=$(grep -n '\[OK\] fork/wait demo process created, pid 0x' "$SERIAL_LOG" | head -1 | cut -d: -f1)
child_line=$(grep -n '\[OK\] child process running after fork' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$create_line" ] || [ -z "$child_line" ] || [ "$create_line" -ge "$child_line" ]; then
    echo "FAIL: child process message did not appear after demo process creation as expected" >&2
    fail=1
fi

# Four processes total must be reaped: the two independent "hello"
# processes (Milestone 17) plus the fork demo's parent and its forked
# child -- and the leak self-test must have passed, proving the child's
# address space (copy-on-write shared with the parent at fork time,
# ADR 0021 -- refcounted, not a fresh unconditional pmm allocation per
# page) was fully reclaimed too, down to the exact same baseline.
reaped_count=$(grep -cF "exited and was reaped" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$reaped_count" -ne 4 ]; then
    echo "FAIL: expected exactly 4 'exited and was reaped' messages, got $reaped_count" >&2
    fail=1
fi
check "[OK] process lifecycle self-test passed, "

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: sys_fork produced a genuinely independent child, sys_wait correctly reported its real exit code"
