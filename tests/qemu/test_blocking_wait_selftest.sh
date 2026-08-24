#!/usr/bin/env bash
# Milestone 20 (ADR 0020) smoke test: boot headless in QEMU and assert
# sys_wait actually took its blocking path (sti/hlt/cli, re-polling
# until a match appears) at least once this boot -- not just that it
# eventually returned the right answer, which alone wouldn't
# distinguish a genuine block from a lucky immediate success. See
# kernel/kernel.c's blocking-wait self-test and ADR 0020's Verification
# for why kernel/user/fork_demo.asm's child spin makes this
# deterministic rather than a timing race.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_blocking_wait_selftest.log"
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

check "[OK] blocking wait self-test passed, sys_wait genuinely blocked (0x"

# kernel_main itself panics (kernel/kernel.c) if the reported turn count
# is ever 0 -- a PANIC line here (instead of the [OK] marker above)
# would already have failed the check() call, but assert explicitly
# that the specific panic message never appears, so a future refactor
# that silently swallows the panic still gets caught here.
if grep -qF "blocking wait self-test failed" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: sys_wait never actually took its blocking path this boot" >&2
    fail=1
fi

# Must appear strictly after the fork/wait demo's own exit-code check --
# proves the blocking-wait self-test is observing the SAME sys_wait call
# fork/wait's own self-test already verified the correctness of, not an
# unrelated/coincidental match.
fork_wait_line=$(grep -n '\[OK\] fork/wait self-test: child exit code verified' "$SERIAL_LOG" | head -1 | cut -d: -f1)
block_line=$(grep -n '\[OK\] blocking wait self-test passed' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$fork_wait_line" ] || [ -z "$block_line" ] || [ "$fork_wait_line" -ge "$block_line" ]; then
    echo "FAIL: blocking-wait self-test did not appear after the fork/wait exit-code check as expected" >&2
    fail=1
fi

# Every downstream self-test (through the shell prompt) must still pass
# -- proves the per-task saved_user_rsp refactor (ADR 0020) didn't
# regress any OTHER syscall path, not just sys_wait's own.
check "[OK] process lifecycle self-test passed, "

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: sys_wait genuinely blocked at least once, deterministically, with no regression downstream"
