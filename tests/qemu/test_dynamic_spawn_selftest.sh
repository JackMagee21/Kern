#!/usr/bin/env bash
# Milestone 36 (ADR 0036) smoke test: boot headless in QEMU, type real
# `spawn pulse`/`spawn clock` commands through the actual PS/2 keyboard
# path (monitor `sendkey`, the same technique test_shell_selftest.sh
# already established), and prove a genuinely NEW window can be
# launched AFTER boot -- not just that the shell command runs without
# crashing, but that the spawned window actually renders real pixels on
# screen, and that closing it drives the exact same real exit protocol
# (Milestone 34) a boot-time window already gets.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_dynamic_spawn_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_dynamic_spawn_selftest.mon.sock"
SCREEN_PPM_SPAWNED="$BUILD_DIR/test_dynamic_spawn_selftest_spawned.ppm"
SCREEN_PPM_CLOSED="$BUILD_DIR/test_dynamic_spawn_selftest_closed.ppm"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK" "$SCREEN_PPM_SPAWNED" "$SCREEN_PPM_CLOSED"

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

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" "$SCREEN_PPM_SPAWNED" "$SCREEN_PPM_CLOSED" <<'PYEOF'
import socket, sys, time

mon_path, serial_log, ppm_spawned, ppm_closed = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

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

def send(cmd, wait=0.1):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    try:
        s.recv(65536)
    except Exception:
        pass

key_map = {' ': 'spc'}

def type_line(text):
    for ch in text:
        send("sendkey " + key_map.get(ch, ch))
    send("sendkey ret", 0.3)

def wait_for_marker(marker, timeout=15, after=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(serial_log, "r", errors="replace") as f:
            content = f.read()
            if marker in content[after:]:
                return
        time.sleep(0.1)
    sys.exit(f"'{marker}' never appeared after byte {after} within {timeout}s")

# Real PS/2 keystrokes, same technique test_shell_selftest.sh already
# established -- launches TWO genuinely new windows after boot, one of
# each spawnable program.
type_line("spawn pulse")
wait_for_marker("[OK] display server: dynamically presented window 0x4", after=baseline_len)
type_line("spawn clock")
wait_for_marker("[OK] display server: dynamically presented window 0x5", after=baseline_len)

send("screendump " + ppm_spawned, 0.6)

# Close the dynamically spawned pulse app's window: it's granted
# (380, 560) with the server's own fixed MAX_CANVAS_W=200 chrome, so
# its close button sits at (380+200-2-16 .. 380+200-2, 560-20+2 ..
# 560-20+18) = (562-577, 542-557) -- center (569, 549). Cursor starts
# at (512, 384).
send("mouse_move 57 165", 0.3)
send("mouse_button 1", 0.2)
wait_for_marker("[OK] display server: closed window 0x4", after=baseline_len)
send("mouse_button 0", 0.2)

wait_for_marker("[OK] pulse app: received exit request, exiting", after=baseline_len)
wait_for_marker("exited and was reaped", after=baseline_len)

send("screendump " + ppm_closed, 0.6)
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

check "spawn pulse"
check "[OK] spawned pulse app, pid 0x"
check "[OK] display server: dynamically presented window 0x4"
check "spawn clock"
check "[OK] spawned clock app, pid 0x"
check "[OK] display server: dynamically presented window 0x5"
check "[OK] display server: closed window 0x4"
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
if grep -qF "[FAIL] clock app" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the clock app's own self-check reported failure" >&2
    fail=1
fi

if [ ! -s "$SCREEN_PPM_SPAWNED" ] || [ ! -s "$SCREEN_PPM_CLOSED" ]; then
    echo "FAIL: one or both screendumps were not captured" >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    python3 - "$SCREEN_PPM_SPAWNED" "$SCREEN_PPM_CLOSED" <<'PYEOF'
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

def region_has(w, h, px, x0, y0, x1, y1, color):
    for y in range(y0, y1):
        row_off = y * w * 3
        for x in range(x0, x1):
            o = row_off + x * 3
            if (px[o], px[o + 1], px[o + 2]) == color:
                return True
    return False

def region_has_any(w, h, px, x0, y0, x1, y1, colors):
    for y in range(y0, y1):
        row_off = y * w * 3
        for x in range(x0, x1):
            o = row_off + x * 3
            if (px[o], px[o + 1], px[o + 2]) in colors:
                return True
    return False

# The pulse app's own full 4-entry palette (pulse_app.c) -- its
# animation loop keeps cycling through these on its own real-time
# cadence, so by the time a screendump is actually taken it may already
# be showing any one of the four, not necessarily the first
# (kernel/user/pulse_app.c never guarantees WHICH one is showing at an
# arbitrary later moment, only that ONE of them always is).
PULSE_PALETTE = {
    (0xCC, 0x33, 0xFF),
    (0x33, 0xFF, 0x33),
    (0x33, 0x33, 0xFF),
    (0xFF, 0xFF, 0x33),
}
CLOCK_DIGIT = (0x00, 0xFF, 0xFF)

fail = False

w1, h1, px1 = read_ppm(sys.argv[1])
# Dynamic slot 0 (pulse, x=380,y=560, canvas 150x100 -> x:380-529,
# y:560-659): a sub-rect below y=619, clear of the second dynamic
# window's own canvas (clock, y:570-619) entirely, so this check never
# depends on which of the two is topmost.
if not region_has_any(w1, h1, px1, 400, 630, 440, 650, PULSE_PALETTE):
    print("FAIL: no pulse-app palette pixels found in its expected region", file=sys.stderr)
    fail = True

# Dynamic slot 1 (clock, x=390,y=570, canvas 190x50 -> x:390-579,
# y:570-619): a sub-rect at x>=529, clear of the first dynamic
# window's own canvas (pulse, x:380-529) entirely.
if not region_has(w1, h1, px1, 540, 580, 560, 600, CLOCK_DIGIT):
    print("FAIL: no digit-color (dynamically spawned clock app) pixels found in its expected region", file=sys.stderr)
    fail = True

w2, h2, px2 = read_ppm(sys.argv[2])
# After closing the pulse app's dynamic window: none of its palette
# colors should remain anywhere in its old full footprint (chrome +
# canvas) -- it stopped animating (and stopped redrawing) the moment
# it received DISPLAY_OP_EXIT, but whichever palette color it was
# showing at that instant must be gone too, since the server clears
# the whole footprint on close before this screendump was taken.
if region_has_any(w2, h2, px2, 380, 540, 580, 660, PULSE_PALETTE):
    print("FAIL: a pulse-app palette pixel is still visible after close", file=sys.stderr)
    fail = True

# The clock app's own window (a DIFFERENT dynamic window) must be
# unaffected by closing the pulse app's.
if not region_has(w2, h2, px2, 540, 580, 560, 600, CLOCK_DIGIT):
    print("FAIL: the clock app's own window was affected by closing an unrelated window", file=sys.stderr)
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

echo "PASS: real shell commands dynamically spawned two new windows that genuinely rendered on screen, and closing one drove the real exit protocol without affecting the other"
