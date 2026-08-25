#!/usr/bin/env bash
# Milestone 31 (ADR 0031) smoke test: boot headless in QEMU and prove
# window chrome (a server-drawn title bar) is genuinely interactive --
# not just that it's drawn, but that a real injected drag actually
# moves a window, and a real injected click on its close button
# actually removes it, cleanly, with nothing left behind. Reuses the
# same `mouse_move`/`mouse_button` QEMU-monitor injection technique
# every earlier input test in this suite already established
# (Milestones 16/23/29/30).
#
# Client B (orange, kernel/user/display_client_b.c) starts granted a
# canvas at (150, 550), with its own server-drawn title bar at
# (150, 530)-(349, 549). This test presses on that title bar (clear of
# the close button, which sits at its own right end), drags it 450px
# right (clear of client A's own canvas entirely, so the "did it
# actually move" check is an unambiguous, disjoint rectangle), and
# releases -- then separately clicks client A's OWN close button and
# confirms client A's canvas vanishes completely, with client B's
# canvas (now dragged away, potentially overlapping where A used to
# be) fully intact underneath.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_window_chrome_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_window_chrome_selftest.mon.sock"
SCREEN_PPM_DRAGGED="$BUILD_DIR/test_window_chrome_selftest_dragged.ppm"
SCREEN_PPM_CLOSED="$BUILD_DIR/test_window_chrome_selftest_closed.ppm"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK" "$SCREEN_PPM_DRAGGED" "$SCREEN_PPM_CLOSED"

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

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" "$SCREEN_PPM_DRAGGED" "$SCREEN_PPM_CLOSED" <<'PYEOF'
import socket, sys, time

mon_path, serial_log, screen_ppm_dragged, screen_ppm_closed = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

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

def send(cmd, wait=0.3):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    s.recv(65536)

def wait_for_marker(marker, timeout=10):
    # Real synchronization, not a guessed delay -- the same
    # "wait_for_line_count" discipline test_mouse_selftest.sh's own
    # comment already explains the reasoning for (a command sent too
    # early can race ahead of a still-pending prior one).
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(serial_log, "r", errors="replace") as f:
            if marker in f.read():
                return
        time.sleep(0.1)
    sys.exit(f"'{marker}' never appeared within {timeout}s")

# Cursor starts centered (512, 384). Move to (200, 535) -- inside
# client B's title bar (150-349, 530-549), clear of its close button
# (332-347, 532-547) -- and press.
send("mouse_move -312 151", 0.3)
send("mouse_button 1", 0.2)
wait_for_marker("[OK] display server: started dragging window 0x1")

# Drag 450px right, 0 vertically -- clear of client A's canvas
# (100-299, 500-649) entirely, so the resulting rectangle is
# unambiguous. New cursor position: (650, 535).
send("mouse_move 450 0", 0.3)
wait_for_marker("[OK] display server: dragged window 0x1")

send("mouse_button 0", 0.2)
wait_for_marker("[OK] input router: routed release to pid 0x")

send("screendump " + screen_ppm_dragged, 0.6)

# Now click client A's own close button: (100 + 200 - 2 - 16, 500 - 20
# + 2) .. -- (282, 482), well inside its (282-297, 482-497) rect. Move
# from the current cursor position (650, 535) to (290, 485).
send("mouse_move -360 -50", 0.3)
send("mouse_button 1", 0.2)
wait_for_marker("[OK] display server: closed window 0x0")
send("mouse_button 0", 0.2)

send("screendump " + screen_ppm_closed, 0.6)
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

check "[OK] display server: started dragging window 0x1"
check "[OK] display server: dragged window 0x1"
check "[OK] display server: closed window 0x0"
check "kernel shell -- type 'help' for commands"

if grep -qF "[FAIL] display server" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the display server's own self-check reported failure" >&2
    fail=1
fi
if grep -qF "[FAIL] display client" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: a display client's own self-check reported failure" >&2
    fail=1
fi

if [ ! -s "$SCREEN_PPM_DRAGGED" ] || [ ! -s "$SCREEN_PPM_CLOSED" ]; then
    echo "FAIL: one or both screendumps were not captured" >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    python3 - "$SCREEN_PPM_DRAGGED" "$SCREEN_PPM_CLOSED" <<'PYEOF'
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

def is_teal(r, g, b):
    return r < 20 and 165 <= g <= 195 and 185 <= b <= 215

def is_orange(r, g, b):
    return r > 235 and 125 <= g <= 155 and b < 20

def color_bbox(width, height, px, matcher):
    coords = []
    for y in range(height):
        row_off = y * width * 3
        for x in range(width):
            o = row_off + x * 3
            if matcher(px[o], px[o + 1], px[o + 2]):
                coords.append((x, y))
    if not coords:
        return None
    xs = [c[0] for c in coords]
    ys = [c[1] for c in coords]
    return (min(xs), min(ys), max(xs), max(ys), len(coords))

fail = False

# After the drag: client B (orange) must be a full, unbroken 200x150
# rectangle at its NEW position, x relative to wherever it actually
# started (window_x[1]=150 plus the 450px drag) -- but since client A
# was never touched, its own teal geometry is unaffected and still
# provides the SAME relative-to-scroll-drift reference technique
# test_display_server_selftest.sh's own comment already explains (x is
# stable, absolute; y is coupled to console scroll only pre-Milestone
# 30 relocation, which no longer applies now that both windows live
# below the reserved console region -- still checked relatively here
# for consistency, not because it's still drifting).
dw, dh, dpx = read_ppm(sys.argv[1])
dragged_box = color_bbox(dw, dh, dpx, is_orange)
if dragged_box is None:
    print("FAIL: no orange (client B) pixels found anywhere after the drag", file=sys.stderr)
    fail = True
else:
    dx0, dy0, dx1, dy1, dcount = dragged_box
    # window_x[1] (150) + 450px drag = 600; y unchanged at window_y[1] (550).
    expected = (600, 550, 799, 699, 200 * 150)
    if (dx0, dy0, dx1, dy1, dcount) != expected:
        print(f"FAIL: client B is not at the exact expected post-drag position: got {dragged_box}, expected {expected}", file=sys.stderr)
        fail = True

# After the close: client A (teal) must be GONE entirely -- not
# shrunk, not moved, just not present anywhere on screen -- while
# client B (orange, at its dragged position) remains the full,
# unbroken rectangle, proving the close only affected the window that
# was actually closed.
cw, ch, cpx = read_ppm(sys.argv[2])
closed_teal_box = color_bbox(cw, ch, cpx, is_teal)
if closed_teal_box is not None:
    print(f"FAIL: client A's (teal) canvas is still visible after being closed: {closed_teal_box}", file=sys.stderr)
    fail = True

closed_orange_box = color_bbox(cw, ch, cpx, is_orange)
if closed_orange_box is None:
    print("FAIL: no orange (client B) pixels found anywhere after closing client A", file=sys.stderr)
    fail = True
else:
    ex0, ey0, ex1, ey1, ecount = closed_orange_box
    expected_after_close = (600, 550, 799, 699, 200 * 150)
    if (ex0, ey0, ex1, ey1, ecount) != expected_after_close:
        print(f"FAIL: client B is not the exact expected full rectangle after client A closed: got {closed_orange_box}, expected {expected_after_close}", file=sys.stderr)
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

echo "PASS: a real injected drag moved client B's window to the exact expected position, and a real injected click on client A's close button removed it cleanly"
