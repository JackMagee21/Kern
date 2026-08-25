#!/usr/bin/env bash
# Milestone 23 (ADR 0023) smoke test: boot headless in QEMU, take a real
# QEMU monitor `screendump` of the negotiated graphics framebuffer
# BEFORE and AFTER injecting a real synthetic PS/2 mouse move (monitor
# `mouse_move`, actual virtual hardware -- same technique
# test_mouse_selftest.sh already established, not a shortcut), and
# assert the mouse cursor sprite (kernel/drivers/cursor.c) is found at
# the EXACT expected pixel position each time -- not just that the
# framebuffer console initialized without crashing. Also confirms no
# "ghost" trail is left at the cursor's old position (proves
# erase-before-redraw actually restored the pixels underneath, not just
# that a new cursor got drawn somewhere).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_framebuffer_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_framebuffer_selftest.mon.sock"
BEFORE_PPM="$BUILD_DIR/test_framebuffer_selftest_before.ppm"
AFTER_PPM="$BUILD_DIR/test_framebuffer_selftest_after.ppm"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK" "$BEFORE_PPM" "$AFTER_PPM"

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

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" "$BEFORE_PPM" "$AFTER_PPM" <<'PYEOF'
import socket, sys, time

mon_path, serial_log, before_ppm, after_ppm = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

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

def send(cmd, wait=0.4):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    s.recv(65536)

send("screendump " + before_ppm, 0.6)
send("mouse_move 100 50", 0.5)
send("screendump " + after_ppm, 0.6)
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

check "[OK] graphics framebuffer console initialized"
check "kernel shell -- type 'help' for commands"

if [ ! -s "$BEFORE_PPM" ] || [ ! -s "$AFTER_PPM" ]; then
    echo "FAIL: one or both screendumps were not captured" >&2
    fail=1
fi

# Locate the cursor's solid 8x8 red block in each screendump and check
# it's at the EXACT expected pixel position -- not just "somewhere" --
# and that the old position is fully clean (no leftover ghost pixels)
# in the AFTER dump, proving erase-before-redraw actually restored what
# was underneath.
if [ "$fail" -eq 0 ]; then
    python3 - "$BEFORE_PPM" "$AFTER_PPM" <<'PYEOF'
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

def red_bbox(width, height, px):
    coords = []
    for y in range(height):
        row_off = y * width * 3
        for x in range(width):
            o = row_off + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            if r > 200 and g < 60 and b < 60:
                coords.append((x, y))
    if not coords:
        return None
    xs = [c[0] for c in coords]
    ys = [c[1] for c in coords]
    return (min(xs), min(ys), max(xs), max(ys), len(coords))

before_ppm, after_ppm = sys.argv[1], sys.argv[2]
bw, bh, bpx = read_ppm(before_ppm)
aw, ah, apx = read_ppm(after_ppm)

before_box = red_bbox(bw, bh, bpx)
after_box = red_bbox(aw, ah, apx)

fail = False

expected_before = (bw // 2, bh // 2, bw // 2 + 7, bh // 2 + 7, 64)
if before_box != expected_before:
    print(f"FAIL: cursor not at expected initial position: got {before_box}, expected {expected_before}", file=sys.stderr)
    fail = True

# mouse_move 100 50 -> +100 x (no inversion), +50 y (screen-down for a
# positive monitor-injected dy -- cursor.c negates the raw PS/2 dy, and
# QEMU's own mouse_move argument already accounts for the PS/2 wire
# convention, so the net effect is the intuitive "positive = down").
expected_after = (bw // 2 + 100, bh // 2 + 50, bw // 2 + 107, bh // 2 + 57, 64)
if after_box != expected_after:
    print(f"FAIL: cursor did not move to the exact expected position: got {after_box}, expected {expected_after}", file=sys.stderr)
    fail = True

sys.exit(1 if fail else 0)
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

echo "PASS: graphics framebuffer console initialized and the mouse cursor moved to the exact expected pixel position from real injected input"
