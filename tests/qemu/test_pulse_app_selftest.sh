#!/usr/bin/env bash
# Milestone 33 (ADR 0033) smoke test: boot headless in QEMU and prove
# kernel/user/pulse_app.c is a genuinely LIVE, self-redrawing window --
# not just that its canvas landed once at boot (every earlier client,
# A and B, already proves that much), but that the SAME on-screen
# rectangle actually changes color over real wall-clock time, driven by
# its own DISPLAY_OP_REDRAW pings (display_server.c's composite_all()).
#
# Unlike every earlier input test in this suite, there is no serial
# marker to wait on for "a redraw happened" -- display_server.c's own
# DISPLAY_OP_REDRAW handler deliberately prints nothing (see its own
# comment: composite_all() needs no per-window bookkeeping, and neither
# does logging a message every single frame forever). So this test
# polls the ACTUAL OBSERVABLE FACT directly (repeated screendumps,
# classifying the sampled pixel's color against the app's own fixed
# four-color palette) instead of a serial marker -- the same
# "real synchronization, not a guessed delay" discipline every other
# test in this suite already follows, just applied to pixels instead
# of log lines, since that's the only observable signal this
# particular behavior has.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_pulse_app_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_pulse_app_selftest.mon.sock"
SCREEN_PPM="$BUILD_DIR/test_pulse_app_selftest.ppm"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK" "$SCREEN_PPM"

timeout 30 qemu-system-x86_64 \
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

RESULT_FILE="$BUILD_DIR/test_pulse_app_selftest.result"
rm -f "$RESULT_FILE"

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" "$SCREEN_PPM" "$RESULT_FILE" <<'PYEOF'
import socket, sys, time

mon_path, serial_log, screen_ppm, result_file = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

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
    with open(result_file, "w") as f:
        f.write("FAIL: shell prompt never appeared within the timeout\n")
    sys.exit(0)

for _ in range(20):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(mon_path)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        time.sleep(0.2)
else:
    with open(result_file, "w") as f:
        f.write("FAIL: could not connect to the QEMU monitor socket\n")
    sys.exit(0)

time.sleep(0.3)
s.recv(4096)

def screendump():
    s.sendall(("screendump " + screen_ppm + "\n").encode())
    time.sleep(0.4)
    s.recv(65536)
    with open(screen_ppm, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6"
    idx = 2
    vals = []
    while len(vals) < 3:
        while data[idx] in b" \t\r\n":
            idx += 1
        start = idx
        while data[idx] not in b" \t\r\n":
            idx += 1
        vals.append(int(data[start:idx]))
    width, height, _maxval = vals
    idx += 1
    pixels = data[idx:idx + width * height * 3]
    return width, height, pixels

# Center of the pulse app's own fixed canvas: window_x[2]=650,
# window_y[2]=520 (display_server.c), granted the full requested
# 150x100 (well under MAX_CANVAS_W/H, so never clamped) -- (725, 570)
# is comfortably inside (650-799, 520-619) regardless of the palette
# index currently showing.
SAMPLE_X, SAMPLE_Y = 725, 570

# Palette colors are 0x00RRGGBB (0x00CC33FF purple, 0x0033FF33 green,
# 0x003333FF blue, 0x00FFFF33 yellow -- see kernel/user/pulse_app.c's
# own doc comment for why purple, not red: this window is big enough
# that a near-red entry here would collide with test_framebuffer_
# selftest.sh's whole-screen cursor-color scan), chosen so a simple
# threshold classifier can never confuse one for another, or for
# anything else already on screen (console background, clients A/B's
# teal/orange, the chrome's magenta close button, the cursor's red).
def classify(r, g, b):
    if r > 150 and g < 100 and b > 150:
        return 0
    if r < 90 and g > 200 and b < 90:
        return 1
    if r < 90 and g < 90 and b > 200:
        return 2
    if r > 200 and g > 200 and b < 90:
        return 3
    return None

def sample():
    width, _height, px = screendump()
    off = (SAMPLE_Y * width + SAMPLE_X) * 3
    return classify(px[off], px[off + 1], px[off + 2])

first = sample()
if first is None:
    with open(result_file, "w") as f:
        f.write("FAIL: sampled pixel was not any recognized palette color on the first screendump\n")
    sys.exit(0)

changed_to = None
poll_deadline = time.time() + 20
while time.time() < poll_deadline:
    time.sleep(0.5)
    current = sample()
    if current is not None and current != first:
        changed_to = current
        break

s.sendall(b"quit\n")
time.sleep(0.2)
s.close()

if changed_to is None:
    with open(result_file, "w") as f:
        f.write(f"FAIL: sampled pixel never changed from palette index {first} within the polling window\n")
    sys.exit(0)

with open(result_file, "w") as f:
    f.write(f"PASS: sampled pixel changed from palette index {first} to {changed_to}\n")
PYEOF

wait "$QEMU_PID" 2>/dev/null || true
trap - EXIT

fail=0

if [ ! -f "$RESULT_FILE" ]; then
    echo "FAIL: test driver produced no result" >&2
    fail=1
elif grep -q "^FAIL" "$RESULT_FILE"; then
    cat "$RESULT_FILE" >&2
    fail=1
else
    cat "$RESULT_FILE"
fi

if grep -qF "[FAIL] pulse app" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the pulse app's own self-check reported failure" >&2
    fail=1
fi
if grep -qF "[FAIL] display server" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the display server's own self-check reported failure" >&2
    fail=1
fi
if ! grep -qF "[OK] display server: presented window 0x2" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the pulse app's window was never presented during initial setup" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: the pulse app's window genuinely redraws with new content over real wall-clock time"
