#!/usr/bin/env bash
# Milestone 38 (ADR 0038) smoke test: boot headless in QEMU with a real
# raw disk image explicitly attached (kernel/drivers/ata.c's own
# primary-slave slot, ide.0/unit=1 -- never the same slot -cdrom's own
# boot media uses), and prove the polled PIO driver genuinely reads
# real hardware, not just that it compiles: IDENTIFY reports the exact
# sector count this test's own disk image was created with, and a
# real write/read-back round trip at a specific LBA matches
# byte-for-byte.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
DISK_IMG="$BUILD_DIR/disk.img"
SERIAL_LOG="$BUILD_DIR/test_ata_selftest.log"

make -C "$ROOT_DIR" "build/os.iso" "$DISK_IMG"

rm -f "$SERIAL_LOG"

timeout 20 qemu-system-x86_64 \
    -cdrom "$OS_ISO" \
    -drive "file=$DISK_IMG,format=raw,if=none,id=ata_disk0" -device ide-hd,drive=ata_disk0,bus=ide.0,unit=1 \
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

# 16MiB = 0x1000000 bytes = 0x8000 512-byte sectors -- the EXACT size
# this test's own disk.img was created with (Makefile's own DISK_IMG
# rule, `dd ... count=16` at bs=1M), proving IDENTIFY genuinely read
# real device geometry, not a hardcoded/assumed constant.
check "[OK] ata self-test: drive present, 0x0000000000008000 sectors (0x0000000001000000 bytes)"
check "[OK] ata self-test passed, 0x0000000000000200 byte read/write round trip verified byte-for-byte at LBA 0x7D0"
check "kernel shell -- type 'help' for commands"

if grep -qF "ata self-test failed" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the ata self-test's own panic path fired" >&2
    fail=1
fi
if grep -qF "no drive detected" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: no drive was detected despite one being explicitly attached" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: the ATA PIO driver identified a real attached disk's exact geometry and verified a real write/read-back round trip byte-for-byte"
