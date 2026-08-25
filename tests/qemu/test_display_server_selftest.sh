#!/usr/bin/env bash
# Milestone 27 (ADR 0027) / Milestone 28 (ADR 0028) / Milestone 30
# (ADR 0030) smoke test: boot headless in QEMU and assert Desktop.md's
# display server actually works end to end -- not just that its own
# markers printed, but that the RIGHT pixels landed at the RIGHT place
# on the REAL framebuffer, in the RIGHT z-order, and nowhere else.
# kernel/user/display_client_a.c and display_client_b.c each
# deliberately ask for a canvas larger than display_server.c's own
# fixed 200x150 maximum -- this is "the server enforces the bound"
# (Desktop.md) made concrete. Milestone 28 added a SECOND client whose
# 200x150 canvas is cascaded (+50, +50) from client A's, genuinely
# overlapping it -- client A is guaranteed (by an explicit go-signal
# hand-off, not a race) to be presented first, so client B's window
# must appear drawn ON TOP of client A's in the overlap region.
# Milestone 30 made the server genuinely PERSISTENT and wired
# Milestone 29's real click-delivery mechanism into an actual visible
# effect: this test now ALSO injects a real synthetic click (the same
# `mouse_move`/`mouse_button` technique test_mouse_selftest.sh/
# test_framebuffer_selftest.sh/the retired test_input_focus_selftest.sh
# already established) onto client A's own exclusive region and
# confirms, via a SECOND screendump, that the overlap region actually
# FLIPS from orange (B) to teal (A) -- direct, pixel-level proof a real
# click genuinely raised a window, not just that a message was
# received.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_display_server_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_display_server_selftest.mon.sock"
SCREEN_PPM="$BUILD_DIR/test_display_server_selftest.ppm"
SCREEN_PPM_AFTER="$BUILD_DIR/test_display_server_selftest_after_click.ppm"

make -C "$ROOT_DIR" "build/os.iso"

rm -f "$SERIAL_LOG" "$MONITOR_SOCK" "$SCREEN_PPM" "$SCREEN_PPM_AFTER"

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

python3 - "$MONITOR_SOCK" "$SERIAL_LOG" "$SCREEN_PPM" "$SCREEN_PPM_AFTER" <<'PYEOF'
import socket, sys, time

mon_path, serial_log, screen_ppm, screen_ppm_after = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

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

# Cursor starts centered ((screen_w/2, screen_h/2), kernel/drivers/
# cursor.c's own cursor_init()) -- move it to (120, 520), a point
# strictly inside client A's own exclusive region ((100,500)-(299,649)
# minus the (150,550)-(299,649) overlap with client B), then click.
# Positive monitor dy = screen-down (test_framebuffer_selftest.sh's own
# established convention for this exact injection technique).
send("mouse_move -392 136", 0.3)
send("mouse_button 1", 0.2)
send("mouse_button 0", 0.2)

# Real synchronization, not a guessed delay: don't screendump until the
# server's own log line proves it actually finished recompositing.
deadline = time.time() + 10
while time.time() < deadline:
    with open(serial_log, "r", errors="replace") as f:
        if "[OK] display server: raised window 0x0" in f.read():
            break
    time.sleep(0.1)
else:
    sys.exit("display server never reported raising window 0 after the injected click")

# Move the cursor back off of client A's canvas before the second
# screendump -- its own solid 8x8 sprite is drawn ON TOP of whatever is
# underneath it (kernel/drivers/cursor.c), and would otherwise cover
# exactly 64 of client A's own pixels, an unrelated, already-proven
# (Milestone 23) behavior this test isn't trying to re-verify.
send("mouse_move 392 -136", 0.3)

send("screendump " + screen_ppm_after, 0.6)
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
check "[OK] display client A: sys_fb_acquire correctly rejected (server already owns the framebuffer)"
check "[OK] display client B: sys_fb_acquire correctly rejected (server already owns the framebuffer)"
check "[OK] display client A: canvas presented via the display server"
check "[OK] display client B: canvas presented via the display server"
check "[OK] display server: presented window 0x0"
check "[OK] display server: presented window 0x1"
check "[OK] display server: all windows presented in z-order"
check "[OK] display server self-test passed, sys_fb_present blitted 0x"
check "kernel shell -- type 'help' for commands"
check "[OK] display server: subscribed to hardware input events"
check "[OK] display server: a second sys_input_subscribe correctly failed (already subscribed)"
check "[OK] input router: routed a click to pid 0x"
check "[OK] display server: raised window 0x0"

# A [FAIL] line from either process would mean the ownership check or
# the request/grant/present handshake itself broke, not just a pixel
# mismatch -- fail fast and loud rather than let the screendump check
# below report a confusing secondary symptom.
if grep -qF "[FAIL] display server" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the display server's own self-check reported failure" >&2
    fail=1
fi
if grep -qF "[FAIL] display client" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: a display client's own self-check reported failure" >&2
    fail=1
fi

# Window 0 (client A) must be fully presented -- and therefore already
# ACKed by the server, which is what client A's own go-signal to client
# B depends on -- before client B's window even shows up in the log.
# This isn't a coincidental ordering: it's what display_client_a.c's
# go-signal hand-off guarantees by construction.
win0_line=$(grep -n '\[OK\] display server: presented window 0x0' "$SERIAL_LOG" | head -1 | cut -d: -f1)
win1_line=$(grep -n '\[OK\] display server: presented window 0x1' "$SERIAL_LOG" | head -1 | cut -d: -f1)
if [ -z "$win0_line" ] || [ -z "$win1_line" ] || [ "$win0_line" -ge "$win1_line" ]; then
    echo "FAIL: window 0 (client A) was not presented before window 1 (client B) as expected" >&2
    fail=1
fi

# Nine processes total must be reaped this boot (Milestone 28/ADR 0028
# replaced the single Milestone 27 client with two, raising this from 9
# to 10; Milestone 30/ADR 0030 LOWERED it back down, 10 to 9, once the
# display server itself became persistent -- created before the
# frame-leak baseline, see kernel/kernel.c -- and moved outside this
# count, leaving only its two clients -- see
# test_ipc_shm_selftest.sh's own identical assertion) -- and the leak
# self-test must still pass (now against a baseline that itself
# accounts for the server's own two permanently-held canvas buffers,
# see kernel_main's own updated comment), proving BOTH clients' shared
# canvas buffers were otherwise fully freed, not leaked by either one.
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

# The real proof: find every pixel matching each client's distinctive
# fill color anywhere on screen. Client A (teal, kernel/user/
# display_client_a.c's FILL_COLOR, 0x0000B4C8 == r=0 g=180 b=200) is
# granted (100,100)-(299,249); client B (orange,
# display_client_b.c's FILL_COLOR, 0x00FF8C00 == r=255 g=140 b=0) is
# granted (150,150)-(349,299), 50 pixels down and right of A -- so
# their canvases overlap in a real 150x100 region, (150,150)-(299,249).
# Client B is guaranteed presented SECOND (see the log-ordering check
# above), so as the topmost, fully opaque window, it must win the ENTIRE
# overlap region: orange's own bounding box should be its full,
# UNBROKEN 200x150 rectangle (30000 pixels, nothing missing), while
# teal should be reduced to an L-shape (client A's rectangle minus the
# overlap, 30000 - 15000 = 15000 pixels) with the SAME outer bounding
# box as before (a plain bounding-box check alone can't see the missing
# corner, so this also spot-checks specific points: A's own untouched
# corners must still be teal, while the overlap corner must now be
# orange, not teal -- direct, pixel-level proof of z-order occlusion,
# not just "both colors exist somewhere").
if [ "$fail" -eq 0 ]; then
    python3 - "$SCREEN_PPM" "$SCREEN_PPM_AFTER" <<'PYEOF'
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

def pixel_at(width, px, x, y):
    o = (y * width + x) * 3
    return px[o], px[o + 1], px[o + 2]

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

screen_ppm = sys.argv[1]
w, h, px = read_ppm(screen_ppm)

fail = False

# X is checked against the ABSOLUTE constants both windows were
# actually granted (100/299 for A, 150/349 for B) -- horizontal
# position is unaffected by anything else on screen. Y is checked
# RELATIVE to wherever client A's own top row actually landed, not
# against an absolute constant: kernel/drivers/fbconsole.c's own
# fb_scroll_up() (Milestone 8/23) shifts the ENTIRE framebuffer -- both
# already-drawn windows included -- up by however many text rows the
# REST of this boot's own console output (everything printed AFTER the
# windows were composited: reap messages, every later self-test line,
# the shell prompt itself) ends up overflowing by, once the shell
# prompt appears. That shift is real, uniform, and deterministic for a
# given boot sequence, but hardcoding its exact size here would make
# this test needlessly fragile against any UNRELATED change to how much
# text prints before the prompt -- checking the RELATIVE 50px cascade
# offset between the two windows (which fb_scroll_up's uniform vertical
# shift can never disturb) proves the actual claim this milestone makes
# (z-order/bound enforcement), without depending on that incidental
# number. See ADR 0028's Known limitations for why this coupling
# exists and isn't fixed at this milestone's scope.
teal_box = color_bbox(w, h, px, is_teal)
if teal_box is None:
    print("FAIL: no teal (client A) pixels found anywhere on screen", file=sys.stderr)
    sys.exit(1)
teal_x0, teal_y0, teal_x1, teal_y1, teal_count = teal_box

expected_teal_x = (100, 299)
expected_teal_count = 200 * 150 - 150 * 100  # client A's full rect minus the overlap client B now owns
if (teal_x0, teal_x1) != expected_teal_x or (teal_y1 - teal_y0 + 1) != 150 or teal_count != expected_teal_count:
    print(f"FAIL: client A's (teal) visible remainder is wrong: got {teal_box} (height {teal_y1 - teal_y0 + 1}), expected x {expected_teal_x}, height 150, {expected_teal_count} pixels", file=sys.stderr)
    fail = True

orange_box = color_bbox(w, h, px, is_orange)
if orange_box is None:
    print("FAIL: no orange (client B) pixels found anywhere on screen", file=sys.stderr)
    sys.exit(1)
orange_x0, orange_y0, orange_x1, orange_y1, orange_count = orange_box

expected_orange_x = (150, 349)
expected_cascade_offset = 50  # display_server.c's own window_y[1] - window_y[0]
if (orange_x0, orange_x1) != expected_orange_x or (orange_y1 - orange_y0 + 1) != 150 or orange_count != 200 * 150:
    print(f"FAIL: client B's (orange, topmost) canvas is the wrong shape/size: got {orange_box} (height {orange_y1 - orange_y0 + 1}), expected x {expected_orange_x}, height 150, {200 * 150} pixels", file=sys.stderr)
    fail = True
if (orange_y0 - teal_y0) != expected_cascade_offset:
    print(f"FAIL: client B's top is not offset exactly {expected_cascade_offset}px below client A's top: got {orange_y0 - teal_y0}", file=sys.stderr)
    fail = True

# Spot-checks, all relative to the DISCOVERED teal_y0/orange_y0 rather
# than an absolute constant, for the same reason as above. A's own
# untouched corners are still teal...
for (x, y) in [(teal_x0, teal_y0), (teal_x0, teal_y1), (teal_x1, teal_y0)]:
    if not is_teal(*pixel_at(w, px, x, y)):
        print(f"FAIL: expected teal (client A, untouched by B) at ({x},{y}), got {pixel_at(w, px, x, y)}", file=sys.stderr)
        fail = True

# ...but the corner client A and B both cover is orange, not teal --
# the direct proof client B was drawn on top, not underneath.
for (x, y) in [(teal_x1, teal_y1), (orange_x0 + 50, orange_y0 + 50), (orange_x0, orange_y0)]:
    if not is_orange(*pixel_at(w, px, x, y)):
        print(f"FAIL: expected orange (client B, on top of A in the overlap) at ({x},{y}), got {pixel_at(w, px, x, y)}", file=sys.stderr)
        fail = True

# Milestone 30 (ADR 0030): the real, pixel-level proof a genuine click
# actually raised a window. A second screendump, taken AFTER injecting
# a real click on client A's own exclusive region (120, 520) and
# waiting for the server's own "raised window 0x0" log line, must show
# the EXACT MIRROR IMAGE of the first: client A (teal) now the full,
# unbroken 200x150 rectangle, client B (orange) now reduced to the
# L-shape -- the overlap region has genuinely changed owner, not just
# "some pixels somewhere changed". Reuses the SAME (x0,x1) reference
# geometry discovered from the FIRST screendump, since x is unaffected
# by anything console-scroll-related and the windows themselves never
# moved -- only which one is drawn on top changed.
screen_ppm_after = sys.argv[2]
aw, ah, apx = read_ppm(screen_ppm_after)

after_teal_box = color_bbox(aw, ah, apx, is_teal)
after_orange_box = color_bbox(aw, ah, apx, is_orange)
if after_teal_box is None or after_orange_box is None:
    print(f"FAIL: after the click, one of the two colors vanished entirely: teal={after_teal_box}, orange={after_orange_box}", file=sys.stderr)
    fail = True
else:
    at_x0, at_y0, at_x1, at_y1, at_count = after_teal_box
    ao_x0, ao_y0, ao_x1, ao_y1, ao_count = after_orange_box

    if (at_x0, at_x1) != (teal_x0, teal_x1) or (at_y1 - at_y0 + 1) != 150 or at_count != 200 * 150:
        print(f"FAIL: after raising client A, its (teal) canvas is not the full unbroken rectangle: got {after_teal_box}", file=sys.stderr)
        fail = True
    if (ao_x0, ao_x1) != (orange_x0, orange_x1) or (ao_y1 - ao_y0 + 1) != 150 or ao_count != expected_teal_count:
        print(f"FAIL: after raising client A, client B's (orange) canvas is not reduced to the expected L-shape: got {after_orange_box}, expected {expected_teal_count} pixels", file=sys.stderr)
        fail = True

    # The direct proof: the overlap point that was ORANGE in the first
    # screendump (client B on top) must now be TEAL (client A raised on
    # top) -- and client B's own exclusive corner (never covered by A
    # at all, regardless of z-order) must still be orange, proving this
    # is a real reordering, not client B simply vanishing.
    overlap_point = (orange_x0 + 50, orange_y0 + 50)
    if not is_teal(*pixel_at(aw, apx, *overlap_point)):
        print(f"FAIL: expected teal (client A, now raised on top of B) at {overlap_point} after the click, got {pixel_at(aw, apx, *overlap_point)}", file=sys.stderr)
        fail = True
    b_exclusive_point = (ao_x1, ao_y1)
    if not is_orange(*pixel_at(aw, apx, *b_exclusive_point)):
        print(f"FAIL: expected orange (client B's own exclusive corner, untouched by the raise) at {b_exclusive_point}, got {pixel_at(aw, apx, *b_exclusive_point)}", file=sys.stderr)
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

echo "PASS: two windows composited in the exact expected bound-enforced sizes/positions with the correct initial z-order, and a real injected click genuinely raised client A above client B"
