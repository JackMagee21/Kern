#!/usr/bin/env bash
# Milestone 8 smoke test: boot headless in QEMU, wait for the shell
# prompt, then inject REAL keystrokes through QEMU's virtual PS/2
# controller (monitor `sendkey`, not a shortcut around the keyboard
# driver) and assert the shell actually read, echoed, and executed
# them -- proving the keyboard IRQ1 driver and the shell's line
# reader/dispatcher work end to end, not just that keyboard_init() ran
# without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_shell_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_shell_selftest.mon.sock"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK"

timeout 20 qemu-system-x86_64 \
    -cdrom "$OS_ISO" \
    -serial "file:$SERIAL_LOG" \
    -display none \
    -no-reboot -no-shutdown \
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

# Wait for the monitor socket to exist and the shell prompt to appear
# before sending keys -- the PS/2 controller's output buffer holds only
# one byte, so keys sent before the guest is polling for them can be
# lost. Real synchronization, not a fixed guessed delay.
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

key_map = {' ': 'spc'}

def type_line(text):
    for ch in text:
        send("sendkey " + key_map.get(ch, ch))
    send("sendkey ret", 0.3)

type_line("help")
type_line("echo shelltest123")
type_line("date")
time.sleep(0.5)
send("quit", 0.3)
s.close()
PYEOF

wait "$QEMU_PID" 2>/dev/null || true
trap - EXIT

fail=0
check() {
    local marker="$1"
    if ! grep -qF "$marker" "$SERIAL_LOG" 2>/dev/null; then
        echo "FAIL: '$marker' not found in serial output" >&2
        fail=1
    fi
}

check "kernel shell -- type 'help' for commands"
check "> help"
check "commands: help, echo <text>, uptime, date, clear"
check "> echo shelltest123"
check "shelltest123"
check "> date"
check "UTC (from CMOS RTC)"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: real PS/2 keystrokes were read, echoed, and executed by the shell"
