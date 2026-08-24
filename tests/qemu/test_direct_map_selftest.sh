#!/usr/bin/env bash
# Milestone 19 (ADR 0019) smoke test: boot headless in QEMU and assert
# the physical-memory direct-map was initialized AND its own self-test
# (write a pattern through vmm_phys_to_virt(), read it back through the
# completely independent low identity mapping) actually passed -- not
# just that vmm_direct_map_init() ran without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_direct_map_selftest.log"
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

check "[OK] physical memory direct-map initialized"
check "[OK] direct-map self-test passed (write via vmm_phys_to_virt visible via the low identity mapping)"

# Must be initialized BEFORE the kernel heap and both ring-3 processes
# -- proves it's actually available (its PDPT entries under the shared
# PML4[511] already exist) by the time anything that depends on it
# (kernel/mm/elf_loader.c, kernel/sched/task.c's task_fork()) could run.
direct_map_line=$(grep -n '\[OK\] physical memory direct-map initialized' "$SERIAL_LOG" | head -1 | cut -d: -f1)
heap_line=$(grep -n '\[OK\] kernel heap initialized' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$direct_map_line" ] || [ -z "$heap_line" ] || [ "$direct_map_line" -ge "$heap_line" ]; then
    echo "FAIL: direct-map init did not appear before kernel heap init as expected" >&2
    fail=1
fi

# Both ELF-loaded processes and the fork/wait demo's forked child all
# now write their memory content through the direct-map (Milestone
# 17/18's code paths were switched over in this same change) -- if the
# direct-map's arithmetic were wrong, these would already have failed
# elsewhere, but check them here too as direct confirmation the
# refactor didn't regress anything downstream.
check "[OK] elf .data/.bss segment verification passed"
check "[OK] fork/wait self-test: child exit code verified"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: physical memory direct-map initialized correctly and independently verified"
