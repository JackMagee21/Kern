#!/usr/bin/env bash
# Milestone 34 (ADR 0034) smoke test: boot headless in QEMU and prove a
# real client exit/close protocol actually reclaims a still-running
# client's process -- not just that the server stops drawing its
# window. The pulse app (kernel/user/pulse_app.c, Milestone 33) is the
# first client that could still be alive when its window is closed;
# before this milestone, closing it left the process spinning forever
# with nothing displaying it. This test injects a real click on its
# close button and confirms THREE separate, independent facts: the
# server's own close marker, the client's own "I got the exit request"
# marker, and the scheduler's own reaper actually collecting the
# process -- plus a real screendump proving no trace of its palette is
# left on screen.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_window_close_exit_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_window_close_exit_selftest.mon.sock"
SCREEN_PPM="$BUILD_DIR/test_window_close_exit_selftest.ppm"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK" "$SCREEN_PPM"

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

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" "$SCREEN_PPM" <<'PYEOF'
import socket, sys, time

mon_path, serial_log, screen_ppm = sys.argv[1], sys.argv[2], sys.argv[3]

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

# Real synchronization point: everything from here on is measured
# relative to the log's length at the shell prompt, not a guessed
# delay -- the reap marker this test looks for below must appear
# STRICTLY AFTER this point, or it could just be one of the baseline
# boot's own unrelated reaps (nine of them happen before the shell
# prompt every boot, see kernel.c's own reap-count self-test).
with open(serial_log, "r", errors="replace") as f:
    baseline_len = len(f.read())

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

def send(cmd, wait=0.3):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    s.recv(65536)

def wait_for_marker(marker, timeout=15, after=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(serial_log, "r", errors="replace") as f:
            content = f.read()
            if marker in content[after:]:
                return
        time.sleep(0.1)
    sys.exit(f"'{marker}' never appeared after byte {after} within {timeout}s")

# Cursor starts centered (512, 384). The pulse app's window is at
# (650, 520) with a 200-wide title bar (MAX_CANVAS_W, chrome.c's own
# fixed chrome width, independent of the pulse app's smaller 150x100
# granted canvas). Its close button sits at
# (650+200-2-16 .. 650+200-2, 520-20+2 .. 520-20+18) = (832-847,
# 502-517) -- center (839, 509).
send("mouse_move 327 125", 0.3)
send("mouse_button 1", 0.2)
wait_for_marker("[OK] display server: closed window 0x2")
send("mouse_button 0", 0.2)

# The client's own acknowledgment that it received DISPLAY_OP_EXIT --
# independent proof from the OTHER side of the protocol, not just the
# server's own claim that it sent something.
wait_for_marker("[OK] pulse app: received exit request, exiting", after=baseline_len)

# The scheduler's reaper actually collecting the now-exited process --
# proof the exit was REAL (task_t freed, address space torn down,
# kernel stack/shm reference reclaimed), not just that the client
# printed a message and kept spinning.
wait_for_marker("exited and was reaped", after=baseline_len)

send("screendump " + screen_ppm, 0.6)
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

check "[OK] display server: closed window 0x2"
check "[OK] pulse app: received exit request, exiting"
check "kernel shell -- type 'help' for commands"

if grep -qF "[FAIL] display server" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the display server's own self-check reported failure" >&2
    fail=1
fi
if grep -qF "[FAIL] pulse app" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the pulse app's own self-check reported failure" >&2
    fail=1
fi

if [ ! -s "$SCREEN_PPM" ]; then
    echo "FAIL: screendump was not captured" >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    python3 - "$SCREEN_PPM" <<'PYEOF'
import sys

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'P6'
    idx = 2
    vals = []
    while len(vals) < 3:
        while data[idx] in b' \t\r\n':
            idx += 1
        start = idx
        while data[idx] not in b' \t\r\n':
            idx += 1
        vals.append(int(data[start:idx]))
    width, height, _maxval = vals
    idx += 1
    pixels = data[idx:idx + width * height * 3]
    return width, height, pixels

# Any of the pulse app's own palette colors (pulse_app.c) -- none
# should be visible anywhere on screen once its window is closed and
# cleared. Matched exactly (these are solid fills, not anti-aliased),
# not with a loose tolerance -- a real leftover pixel should match
# exactly, not "closely".
palette = [
    (0xCC, 0x33, 0xFF),
    (0x33, 0xFF, 0x33),
    (0x33, 0x33, 0xFF),
    (0xFF, 0xFF, 0x33),
]

w, h, px = read_ppm(sys.argv[1])
found = None
for y in range(h):
    row_off = y * w * 3
    for x in range(w):
        o = row_off + x * 3
        r, g, b = px[o], px[o + 1], px[o + 2]
        if (r, g, b) in palette:
            found = (x, y, r, g, b)
            break
    if found:
        break

if found:
    print(f"FAIL: a pulse-app palette pixel is still visible after close: {found}", file=sys.stderr)
    sys.exit(1)
sys.exit(0)
PYEOF
    if [ $? -ne 0 ]; then
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: closing the pulse app's window sent it a real DISPLAY_OP_EXIT, it exited, the reaper actually collected it, and no trace of its palette remains on screen"
