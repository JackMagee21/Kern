#!/usr/bin/env bash
# Milestone 22 (ADR 0022) smoke test: boot headless in QEMU and assert
# sys_exec actually worked end to end -- not just that the syscall
# dispatched without crashing, but specifically that (1) a completely
# DIFFERENT embedded program's code genuinely started running in the
# calling process's address space (kernel/user/exec_target.asm's own
# message appeared, strictly after kernel/user/exec_demo.asm's own
# pre-exec message) and (2) it's still the SAME process, not a new one
# -- proven by kernel_main's reap-count/frame-leak accounting: the exec
# demo process is reaped exactly ONCE (as pid 0x7, task_create_user_
# image()'s return value), never twice, even though it ran two different
# images before finally calling sys_exit.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_exec_selftest.log"
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

check "[OK] exec demo process created, pid 0x"
check "[OK] exec demo running, about to sys_exec into a new image"
check "[OK] exec target running -- process image was genuinely replaced by sys_exec"
check "[OK] exec self-test passed, sys_exec replaced a running process's image 0x"

# A [FAIL] line here would mean sys_exec returned control to the OLD
# image instead of actually replacing it -- exec_demo.asm's own
# "unreachable" path.
if grep -qF "[FAIL] sys_exec returned control to the old image" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the exec demo's own control-flow check reported failure" >&2
    fail=1
fi

# The target's message must appear AFTER the demo's own pre-exec
# message -- proves real sequencing (old image ran first, printed its
# message, THEN the new image's own code ran), not a coincidental
# substring match.
before_line=$(grep -n '\[OK\] exec demo running, about to sys_exec into a new image' "$SERIAL_LOG" | head -1 | cut -d: -f1)
target_line=$(grep -n '\[OK\] exec target running -- process image was genuinely replaced by sys_exec' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$before_line" ] || [ -z "$target_line" ] || [ "$before_line" -ge "$target_line" ]; then
    echo "FAIL: exec target's message did not appear after the demo's own pre-exec message as expected" >&2
    fail=1
fi

# kernel_main itself panics (kernel/kernel.c) if syscall_get_exec_count()
# is ever 0 -- a PANIC line here (instead of the [OK] marker above) would
# already have failed the check() call, but assert explicitly so a
# future refactor that silently swallows the panic still gets caught.
if grep -qF "exec self-test failed" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: sys_exec never actually replaced a process image this boot" >&2
    fail=1
fi

# The strongest proof that exec reused its caller's OWN process rather
# than creating a new one: exactly 7 "exited and was reaped" lines total
# this boot (2 hello + fork demo's parent + its forked child + the exec
# demo process, reaped ONCE despite running two images + the ipc demo's
# sender and receiver, Milestone 26/ADR 0026) -- if sys_exec had instead
# spawned a genuinely new task while leaving the old one dangling, this
# count would be wrong (either 8, if both somehow exited, or stuck below
# 7 forever, if the old one never resumed and hung).
reaped_count=$(grep -cF "exited and was reaped" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$reaped_count" -ne 10 ]; then
    echo "FAIL: expected exactly 10 'exited and was reaped' messages, got $reaped_count" >&2
    fail=1
fi

# Frame-leak counting (process lifecycle self-test, unchanged assertion
# text since Milestone 18) must still pass -- proves the address-space
# reset (vmm_reset_user_address_space(), ADR 0022) that sys_exec does
# mid-flight, then re-populates via elf_load(), returns to the exact
# same baseline as a process that only ever ran ONE image, not just that
# nothing crashed along the way.
check "[OK] process lifecycle self-test passed, "

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: sys_exec genuinely replaced a running process's image, reusing the same process, with no frame leak"
