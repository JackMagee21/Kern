#!/usr/bin/env bash
# Milestone 27 (ADR 0027) smoke test: boot headless in QEMU and assert
# Desktop.md's minimal single-client display server actually works end
# to end -- not just that its own markers printed, but that the RIGHT
# pixels landed at the RIGHT place on the REAL framebuffer, and nowhere
# else. kernel/user/display_client.c deliberately asks for a 400x300
# canvas; kernel/user/display_server.c's own fixed policy never grants
# more than 200x150 -- this is "the server enforces the bound"
# (Desktop.md) made concrete: the client can never paint more than it
# was actually granted, since it never even allocates a buffer bigger
# than that. A real QEMU `screendump` (the same technique
# test_framebuffer_selftest.sh established, Milestone 23) is what
# proves this pixel-for-pixel, not a trusted self-report.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_display_server_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_display_server_selftest.mon.sock"
SCREEN_PPM="$BUILD_DIR/test_display_server_selftest.ppm"

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

# Wait for the shell prompt -- proves every boot-time self-test
# (including the display server/client demo, which runs well before
# this) has already finished, so the screendump below captures its
# final, settled output, not a mid-composite race.
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

check "[OK] display server/client processes created, server pid 0x"
check "[OK] display server: framebuffer acquired"
check "[OK] display server: a second sys_fb_acquire correctly failed (framebuffer already owned)"
check "[OK] display client: sys_fb_acquire correctly rejected (server already owns the framebuffer)"
check "[OK] display client: canvas presented via the display server"
check "[OK] display server: presented the client's granted canvas"
check "[OK] display server self-test passed, sys_fb_present blitted 0x"
check "kernel shell -- type 'help' for commands"

# A [FAIL] line from either process would mean the ownership check or
# the request/grant/present handshake itself broke, not just a pixel
# mismatch -- fail fast and loud rather than let the screendump check
# below report a confusing secondary symptom.
if grep -qF "[FAIL] display server" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the display server's own self-check reported failure" >&2
    fail=1
fi
if grep -qF "[FAIL] display client" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the display client's own self-check reported failure" >&2
    fail=1
fi

# Nine processes total must be reaped this boot (Milestone 27/ADR 0027
# added the display server and its one client, raising this from 7 to
# 9 -- see test_ipc_shm_selftest.sh's own identical assertion) -- and
# the leak self-test must still pass, proving the client's shared
# canvas buffer (refcounted the same way Milestone 26's IPC demo
# object already was) was fully freed once BOTH the server and client
# had exited, not leaked by either one alone.
reaped_count=$(grep -cF "exited and was reaped" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$reaped_count" -ne 9 ]; then
    echo "FAIL: expected exactly 9 'exited and was reaped' messages, got $reaped_count" >&2
    fail=1
fi
check "[OK] process lifecycle self-test passed, "

if [ ! -s "$SCREEN_PPM" ]; then
    echo "FAIL: screendump was not captured" >&2
    fail=1
fi

# The real proof: find every pixel matching the client's distinctive
# fill color (kernel/user/display_client.c's own FILL_COLOR, teal,
# 0x0000B4C8 == r=0 g=180 b=200) anywhere on screen, and assert the
# resulting bounding box is EXACTLY the server's granted canvas --
# (100, 100) to (299, 249), 200x150 pixels, 30000 total -- not the
# 400x300 the client originally asked for. If the server's own bound
# enforcement (display_server.c's MAX_CANVAS_W/H clamp) were broken, or
# if sys_fb_present() somehow blitted past what it was told to, this
# bounding box would be the wrong size or in the wrong place; if the
# canvas never got composited at all, no teal pixels would exist
# anywhere and this would come back empty.
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

def teal_bbox(width, height, px):
    coords = []
    for y in range(height):
        row_off = y * width * 3
        for x in range(width):
            o = row_off + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            if r < 20 and 165 <= g <= 195 and 185 <= b <= 215:
                coords.append((x, y))
    if not coords:
        return None
    xs = [c[0] for c in coords]
    ys = [c[1] for c in coords]
    return (min(xs), min(ys), max(xs), max(ys), len(coords))

screen_ppm = sys.argv[1]
w, h, px = read_ppm(screen_ppm)

box = teal_bbox(w, h, px)
expected = (100, 100, 299, 249, 200 * 150)

fail = False
if box is None:
    print("FAIL: no teal (client canvas) pixels found anywhere on screen", file=sys.stderr)
    fail = True
elif box != expected:
    print(f"FAIL: granted canvas not at the exact expected bound-enforced position/size: got {box}, expected {expected}", file=sys.stderr)
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

echo "PASS: the display server/client handshake presented the client's canvas at exactly the server's bound-enforced size and position, nowhere else"
