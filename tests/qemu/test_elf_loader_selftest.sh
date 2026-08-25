#!/usr/bin/env bash
# Milestone 17 smoke test: boot headless in QEMU and assert the embedded
# userspace ELF64 executable (kernel/user/hello.c, rewritten from
# hand-written NASM to C atop the new userspace runtime in Milestone 24
# -- see ADR 0024) was actually parsed and mapped correctly by
# kernel/mm/elf_loader.c -- not just that a ring-3 process ran
# (test_ring3_syscall_selftest.sh already covers that). Specifically
# checks the program's own .data/.bss verification passed for BOTH
# processes, and that neither ever printed the "[FAIL]" message its own
# self-check would emit if the loader zeroed .bss incorrectly or failed
# to copy .data's real initializer in. This test's own assertions are
# implementation-agnostic (they check hello's OUTPUT, not its source
# language) and needed zero changes for Milestone 24 -- the strongest
# available proof the new C runtime produces byte-for-byte the same
# correct behavior the hand-written NASM version did.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_elf_loader_selftest.log"
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

check "[OK] hello from ring 3 via ELF-loaded process"
check "[OK] elf .data/.bss segment verification passed"

# Two processes, each running their own private mapping of the ELF
# image (Milestone 17 deliberately dropped Milestone 7-16's shared
# read-only code page -- every PT_LOAD segment is now a fresh,
# VMM_FLAG_OWNED per-process allocation) -- both must independently
# read .bss as zero and .data as its real 0x1234 initializer.
verified_count=$(grep -cF "[OK] elf .data/.bss segment verification passed" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$verified_count" -ne 2 ]; then
    echo "FAIL: expected exactly 2 'elf .data/.bss segment verification passed' messages (one per process), got $verified_count" >&2
    fail=1
fi

# A [FAIL] line here would mean the loader zeroed .bss incorrectly or
# didn't actually copy .data's file-backed initializer in.
if grep -qF "[FAIL] elf .data/.bss segment verification failed" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the embedded ELF program's own .data/.bss self-check reported failure" >&2
    fail=1
fi

# Both processes must still exit and be reaped with no frame leak --
# Milestone 17's per-process (not shared) segment frames need
# VMM_FLAG_OWNED to be reclaimed correctly on exit (ADR 0010); a
# missing/wrong flag here would regress this into a real leak, silently
# passed over by test_process_lifecycle_selftest.sh only if that test
# happened to run before this bug was introduced.
check "[OK] process lifecycle self-test passed, "

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: embedded ELF64 image parsed/mapped correctly by both processes (.data/.bss verified, no leak)"
