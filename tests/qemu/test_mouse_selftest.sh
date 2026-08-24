#!/usr/bin/env bash
# Milestone 16 (ADR 0016) smoke test: boot headless in QEMU, inject REAL
# synthetic PS/2 mouse input through QEMU's monitor (mouse_move/
# mouse_button -- actual virtual hardware, not a shortcut around the
# driver), and assert the shell's `mouse` command decoded it correctly
# -- proves IRQ12/the 8042 aux port/packet framing all work together,
# not just that mouse_init() ran without crashing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_mouse_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_mouse_selftest.mon.sock"

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

def send(cmd, wait=0.2):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    s.recv(65536)

def type_line(text):
    for ch in text:
        send("sendkey " + ch, 0.05)
    send("sendkey ret", 0.3)

def wait_for_line_count(marker, count, timeout=10):
    # Real synchronization, not a fixed guessed delay: don't send the
    # NEXT monitor command until the guest has actually finished
    # processing and printed the previous `mouse` command's result --
    # otherwise a mouse_button sent too early can race ahead of a
    # still-pending mouse_move and get picked up by the wrong `mouse`
    # read (this raced and failed intermittently before this fix).
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(serial_log, "r", errors="replace") as f:
            if f.read().count(marker) >= count:
                return
        time.sleep(0.1)
    sys.exit(f"'{marker}' did not reach count {count} within {timeout}s")

# Pure X movement, no Y, no button -- deterministic and avoids the
# raw-PS/2-vs-screen Y-axis sign inversion (confirmed correct by hand,
# but not this assertion's concern).
send("mouse_move 15 0", 0.3)
type_line("mouse")
wait_for_line_count("dx=", 1)

send("mouse_button 1", 0.2)
send("mouse_button 0", 0.2)
type_line("mouse")
wait_for_line_count("dx=", 2)

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
check "> mouse"
check "waiting for a mouse event (move it or click)..."
check "dx=15 dy=0 buttons: L=0 R=0 M=0"
check "buttons: L=1"

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: real injected PS/2 mouse movement and button events were correctly decoded"
