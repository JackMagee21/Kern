#!/usr/bin/env bash
# Milestone 35 (ADR 0035) smoke test: boot headless in QEMU and prove
# the clock app (kernel/user/clock_app.c) is a REAL clock -- not just
# that it presents a window, but that what it draws is genuinely driven
# by real wall-clock time (two screendumps, seconds apart, must differ
# inside its own canvas), and that it supports the same real close/exit
# protocol (Milestone 34, ADR 0034) as the pulse app.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_clock_app_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_clock_app_selftest.mon.sock"
SCREEN_PPM_1="$BUILD_DIR/test_clock_app_selftest_1.ppm"
SCREEN_PPM_2="$BUILD_DIR/test_clock_app_selftest_2.ppm"
SCREEN_PPM_CLOSED="$BUILD_DIR/test_clock_app_selftest_closed.ppm"
RESULT_FILE="$BUILD_DIR/test_clock_app_selftest.result"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK" "$SCREEN_PPM_1" "$SCREEN_PPM_2" "$SCREEN_PPM_CLOSED" "$RESULT_FILE"

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

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" "$SCREEN_PPM_1" "$SCREEN_PPM_2" "$SCREEN_PPM_CLOSED" "$RESULT_FILE" <<'PYEOF'
import socket, sys, time

mon_path, serial_log, ppm1, ppm2, ppm_closed, result_file = sys.argv[1:7]

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
    try:
        s.recv(65536)
    except Exception:
        pass

def wait_for_marker(marker, timeout=15, after=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(serial_log, "r", errors="replace") as f:
            content = f.read()
            if marker in content[after:]:
                return
        time.sleep(0.1)
    sys.exit(f"'{marker}' never appeared after byte {after} within {timeout}s")

# Real liveness proof: two screendumps a few real seconds apart must
# show DIFFERENT content inside the clock's own canvas region -- the
# same "don't infer from a guessed delay, observe the real state
# change" discipline test_pulse_app_selftest.sh already established,
# generalized from "did the color change" to "did the rendered digits
# change" (this app only redraws when the real second actually ticks
# over, kernel/user/clock_app.c's own render-on-change design).
send("screendump " + ppm1, 0.6)
time.sleep(3.0)
send("screendump " + ppm2, 0.6)

# Now close it: window 3's close button is at
# (650+200-2-16 .. 650+200-2, 700-20+2 .. 700-20+18) = (832-847,
# 682-697) -- center (839, 689). Cursor starts at (512, 384).
send("mouse_move 327 305", 0.3)
send("mouse_button 1", 0.2)
wait_for_marker("[OK] display server: closed window 0x3")
send("mouse_button 0", 0.2)

wait_for_marker("[OK] clock app: received exit request, exiting", after=baseline_len)
wait_for_marker("exited and was reaped", after=baseline_len)

send("screendump " + ppm_closed, 0.6)
send("quit", 0.3)
s.close()

with open(result_file, "w") as f:
    f.write("ok\n")
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

check "[OK] clock app: canvas presented via the display server, now ticking"
check "[OK] display server: closed window 0x3"
check "[OK] clock app: received exit request, exiting"
check "kernel shell -- type 'help' for commands"

if grep -qF "[FAIL] display server" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the display server's own self-check reported failure" >&2
    fail=1
fi
if grep -qF "[FAIL] clock app" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the clock app's own self-check reported failure" >&2
    fail=1
fi

if [ ! -s "$SCREEN_PPM_1" ] || [ ! -s "$SCREEN_PPM_2" ] || [ ! -s "$SCREEN_PPM_CLOSED" ]; then
    echo "FAIL: one or more screendumps were not captured" >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    python3 - "$SCREEN_PPM_1" "$SCREEN_PPM_2" "$SCREEN_PPM_CLOSED" <<'PYEOF'
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

def region(w, h, px, x0, y0, x1, y1):
    out = []
    for y in range(y0, y1):
        row_off = y * w * 3
        for x in range(x0, x1):
            o = row_off + x * 3
            out.append((px[o], px[o + 1], px[o + 2]))
    return out

DIGIT_COLOR = (0x00, 0xFF, 0xFF)
BG_COLOR = (0x10, 0x10, 0x30)
# Window 3's full chrome+canvas footprint: x 650-849 (MAX_CANVAS_W=200
# wide chrome), y 680-749 (CHROME_H=20 chrome + 50-tall canvas).
REGION = (650, 680, 850, 750)

fail = False

w1, h1, px1 = read_ppm(sys.argv[1])
r1 = region(w1, h1, px1, *REGION)
if DIGIT_COLOR not in r1:
    print("FAIL: no clock digit pixels found in the clock window's region on the first screendump", file=sys.stderr)
    fail = True
if BG_COLOR not in r1:
    print("FAIL: no clock background pixels found in the clock window's region on the first screendump", file=sys.stderr)
    fail = True

w2, h2, px2 = read_ppm(sys.argv[2])
r2 = region(w2, h2, px2, *REGION)

if r1 == r2:
    print("FAIL: the clock window's region is byte-for-byte identical 3 real seconds apart -- not actually ticking", file=sys.stderr)
    fail = True

wc, hc, pxc = read_ppm(sys.argv[3])
rc = region(wc, hc, pxc, *REGION)
if DIGIT_COLOR in rc:
    print("FAIL: clock digit pixels are still visible after the window was closed", file=sys.stderr)
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

echo "PASS: the clock app rendered real digits that genuinely changed 3 real seconds apart, and closing its window sent a real DISPLAY_OP_EXIT that it received, exited on, and was reaped for"
