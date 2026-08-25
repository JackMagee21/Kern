#!/usr/bin/env bash
# Milestone 26 (ADR 0026) smoke test: boot headless in QEMU and assert
# IPC message-passing and shared memory work together, end to end,
# across two genuinely isolated processes (kernel/user/ipc_sender.c,
# kernel/user/ipc_receiver.c) -- not just that the new syscalls
# dispatch without crashing. The sender writes a known pattern into a
# freshly created shared-memory object, hands its id to the receiver
# via a real sys_ipc_send()/sys_ipc_recv() round trip, and the receiver
# maps the SAME object into its OWN, completely independent address
# space and reads the pattern back. Also asserts sys_ipc_recv() (and
# the scheduler_block_current()/scheduler_wake() primitive underneath
# it, Milestone 25) genuinely blocked at least once -- its first real
# consumer outside Milestone 25's own self-test.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_ipc_shm_selftest.log"
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

check "[OK] ipc/shm demo processes created, sender pid 0x"
check "[OK] ipc demo sender: wrote shared pattern and handed off the shm id"
check "[OK] ipc/shm self-test passed: shared memory pattern verified via IPC handoff"
check "[OK] ipc self-test passed, sys_ipc_recv genuinely blocked (0x"

# A [FAIL] line here would mean the receiver mapped the shared object
# but read back something OTHER than the sender's exact pattern --
# aliasing/corruption, not real cross-process shared memory.
if grep -qF "[FAIL] ipc/shm self-test: shared memory pattern mismatch" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the receiver's own pattern check reported a mismatch" >&2
    fail=1
fi
if grep -qF "[FAIL] ipc demo sender" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the sender's own shm_create/shm_map/ipc_send check reported failure" >&2
    fail=1
fi

# kernel_main itself panics if sys_ipc_recv never actually blocked --
# a PANIC line here (instead of the marker checked above) would already
# have failed the check() call, but assert explicitly so a future
# refactor that silently swallows the panic still gets caught.
if grep -qF "ipc self-test failed" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: sys_ipc_recv never actually blocked this boot" >&2
    fail=1
fi

# The demo's own creation line must appear BEFORE the sender's message
# -- proves real sequencing (processes created, scheduled, sender ran),
# not a coincidental substring match.
create_line=$(grep -n '\[OK\] ipc/shm demo processes created' "$SERIAL_LOG" | head -1 | cut -d: -f1)
sender_line=$(grep -n '\[OK\] ipc demo sender: wrote shared pattern' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$create_line" ] || [ -z "$sender_line" ] || [ "$create_line" -ge "$sender_line" ]; then
    echo "FAIL: the sender's own message did not appear after process creation as expected" >&2
    fail=1
fi

# Seven processes total must be reaped this boot (Milestone 26/ADR 0026
# added the sender and receiver, raising this from 5 to 7 (Milestone 27/
# ADR 0027 raised it again, 7 to 9, for the display server demo) -- see
# test_process_lifecycle_selftest.sh's own identical assertion) -- and
# the leak self-test must have passed, proving the shared object's
# REFCOUNTED frame (kernel/mm/pmm.h's pmm_frame_addref()/
# pmm_free_frame(), the same mechanism COW fork already established,
# ADR 0021) was only actually freed once BOTH mappers had exited, not
# leaked by either one alone.
reaped_count=$(grep -cF "exited and was reaped" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$reaped_count" -ne 9 ]; then
    echo "FAIL: expected exactly 9 'exited and was reaped' messages, got $reaped_count" >&2
    fail=1
fi
check "[OK] process lifecycle self-test passed, "

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: IPC message-passing and shared memory work together end to end across two isolated processes, with no frame leak"
