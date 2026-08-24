#!/usr/bin/env bash
# Milestone 21 (ADR 0021) smoke test: boot headless in QEMU and assert
# fork's copy-on-write sharing actually worked end to end -- not just
# that fork/wait still passes (test_fork_wait_selftest.sh already
# proves that), but specifically that pages are shared lazily (proven
# by vmm_get_cow_fault_count() being nonzero -- a real #PF was actually
# resolved, not just "correct by coincidence because nothing wrote
# anything") AND that the sharing is genuinely isolated, not aliased
# (proven by kernel/user/fork_demo.asm's parent observing its OWN
# write survive, strictly after the child's own DIFFERENT write to the
# same originally-shared page and full exit).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_cow_fork_selftest.log"
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

check "[OK] COW isolation verified: parent's write survived the child's own write"
check "[OK] copy-on-write self-test passed, "

# A [FAIL] line here would mean the parent's post-wait readback saw the
# CHILD's write instead of its own -- accidental aliasing, not real
# per-process isolation, even if every other marker above looks fine.
if grep -qF "[FAIL] COW isolation broken: parent observed the child's write" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the fork demo's own COW isolation check reported failure" >&2
    fail=1
fi

# kernel_main itself panics (kernel/kernel.c) if vmm_get_cow_fault_count()
# is ever 0 -- a PANIC line here (instead of the [OK] marker above)
# would already have failed the check() call, but assert explicitly so
# a future refactor that silently swallows the panic still gets caught.
if grep -qF "copy-on-write self-test failed" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: no COW fault was ever resolved this boot" >&2
    fail=1
fi

# At least 3 COW faults expected this boot: the parent's own write to
# its child_pid .bss slot, the parent's write to shared_var, and the
# child's write to shared_var (kernel/user/fork_demo.asm) -- fewer
# would mean the demo's own write sequence didn't execute as designed.
fault_line=$(grep -oF "copy-on-write self-test passed, " "$SERIAL_LOG" | head -1 || true)
if [ -n "$fault_line" ]; then
    fault_count_hex=$(grep -oP '(?<=copy-on-write self-test passed, )[0-9a-fA-F]+' "$SERIAL_LOG" | head -1 || true)
    if [ -n "$fault_count_hex" ]; then
        fault_count=$((16#$fault_count_hex))
        if [ "$fault_count" -lt 3 ]; then
            echo "FAIL: expected at least 3 COW faults, got $fault_count" >&2
            fail=1
        fi
    fi
fi

# Frame-leak counting (process lifecycle self-test, unchanged assertion
# text since Milestone 18) must still pass -- proves refcounted frames
# (pmm_frame_addref()/pmm_free_frame(), ADR 0021) return to the exact
# same baseline as an eager copy would have, once every reference is
# actually dropped, not just that nothing crashed along the way.
check "[OK] process lifecycle self-test passed, "

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: fork's copy-on-write sharing is genuinely lazy and correctly isolated, with no frame leak"
