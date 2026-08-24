#!/usr/bin/env bash
# Milestone 15 (ADR 0015) smoke test: boot headless in QEMU, inject the
# real "reboot" keystroke sequence through the shell, and assert QEMU
# actually terminates promptly afterward -- with -no-reboot (the same
# flag every other test in this suite uses), a genuine CPU reset makes
# QEMU exit on its own instead of restarting the VM, so "the process
# ended well before the timeout, right after 'reboot' was typed" is
# real evidence the 8042 controller reset (or its triple-fault
# fallback) actually fired, not just that reboot() was called.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_reboot_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_reboot_selftest.mon.sock"
TIMEOUT_SECS=20

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK"

start_time=$(date +%s)

# Deliberately -no-reboot WITHOUT -no-shutdown, unlike every other test
# in this suite: -no-shutdown tells QEMU to stay up (paused) after a
# reset/shutdown for post-mortem debugging, which overrides -no-reboot's
# own "exit instead" behavior -- confirmed by hand: with both flags
# together, QEMU just hangs after the reset instead of exiting. This
# test's whole signal IS "did QEMU exit," so -no-shutdown must be left
# off here specifically.
timeout "$TIMEOUT_SECS" qemu-system-x86_64 \
    -cdrom "$OS_ISO" \
    -serial "file:$SERIAL_LOG" \
    -display none \
    -no-reboot \
    -monitor "unix:$MONITOR_SOCK,server,nowait" &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" <<'PYEOF'
import socket, sys, time

mon_path, serial_log = sys.argv[1], sys.argv[2]

deadline = time.time() + 15
while time.time() < deadline:
    try:
        with open(serial_log, "r", errors="replace") as f:
            if "kernel shell" in f.read():
                break
    except FileNotFoundError:
        pass
    time.sleep(0.2)
else:
    sys.exit("shell prompt never appeared within the timeout")

for _ in range(20):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(mon_path)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        time.sleep(0.2)
else:
    sys.exit("could not connect to the QEMU monitor socket")

time.sleep(0.3)
s.recv(4096)

def send(cmd, wait=0.1):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    s.recv(65536)

def type_line(text):
    for ch in text:
        send("sendkey " + ch)
    send("sendkey ret", 0.3)

type_line("reboot")
s.close()
PYEOF

# Wait for QEMU to exit ON ITS OWN (a real reset, with -no-reboot,
# makes QEMU terminate) -- don't kill it ourselves yet, so the elapsed
# time actually measures whether the reset fired promptly.
wait "$QEMU_PID" 2>/dev/null || true
trap - EXIT
end_time=$(date +%s)
elapsed=$((end_time - start_time))

fail=0
check() {
    local marker="$1"
    if ! grep -qF "$marker" "$SERIAL_LOG" 2>/dev/null; then
        echo "FAIL: '$marker' not found in serial output" >&2
        fail=1
    fi
}

check "kernel shell -- type 'help' for commands"
check "> reboot"
check "rebooting..."

# QEMU should have exited within a few seconds of the reboot command,
# not run until the full timeout -- a generous margin (10s) above the
# ~1s the shell prompt/reboot exchange itself takes, but well short of
# TIMEOUT_SECS, so a kernel that ignored the reboot command (hung
# instead of resetting) is distinguishable from one that reset promptly.
if [ "$elapsed" -ge $((TIMEOUT_SECS - 5)) ]; then
    echo "FAIL: QEMU ran for ${elapsed}s (close to the ${TIMEOUT_SECS}s timeout) -- the CPU reset likely never fired" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: reboot command triggered a real CPU reset (QEMU exited after ${elapsed}s, well under the ${TIMEOUT_SECS}s timeout)"
