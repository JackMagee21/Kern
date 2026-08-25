#!/usr/bin/env bash
# Milestone 29 (ADR 0029) smoke test: boot headless in QEMU, inject a
# REAL synthetic PS/2 left-click through QEMU's monitor (mouse_move +
# mouse_button -- actual virtual hardware, the same technique
# test_mouse_selftest.sh/test_framebuffer_selftest.sh already
# established, Milestones 16/23), and assert it reaches
# kernel/user/input_focus_demo.c -- a genuinely separate ring-3
# process -- via the new kernel/drivers/input_router.c delivery path,
# not just that the mouse driver decoded it. This is the first time
# this kernel has ever delivered a hardware event to userspace at all.
#
# input_focus_demo.c blocks forever waiting for a click that a PLAIN
# headless boot (every other test in this suite) never sends -- every
# OTHER test already confirms that steady-state is harmless (the demo
# process is created BEFORE kernel_main's own frame-leak baseline,
# deliberately excluded from the reap-count gate, see kernel/kernel.c's
# own comment). This test is the one place that actually exercises the
# full path: injected click -> mouse.c decode -> cursor.c edge
# detection -> input_router.c -> IPC -> the demo process waking up,
# verifying the opcode, and finally exiting (so it DOES get reaped
# here, on top of the baseline 10 every other test already expects).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OS_ISO="$BUILD_DIR/os.iso"
SERIAL_LOG="$BUILD_DIR/test_input_focus_selftest.log"
MONITOR_SOCK="$BUILD_DIR/test_input_focus_selftest.mon.sock"

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

def send(cmd, wait=0.3):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    s.recv(65536)

def wait_for_marker(marker, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(serial_log, "r", errors="replace") as f:
            if marker in f.read():
                return
        time.sleep(0.1)
    sys.exit(f"'{marker}' never appeared within {timeout}s")

# A known move (deterministic, no Y component -- avoids the raw-PS/2-
# vs-screen sign inversion, the same reasoning test_mouse_selftest.sh
# already gives for its own identical choice) followed by a real
# button press/release -- exactly the injection shape
# test_mouse_selftest.sh already proved decodes correctly.
send("mouse_move 40 0", 0.3)
send("mouse_button 1", 0.2)
send("mouse_button 0", 0.2)

wait_for_marker("[OK] input router: routed a click to pid 0x")
wait_for_marker("[OK] input focus demo: received a real routed click event")

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

check "[OK] input focus demo process created, pid 0x"
check "[OK] input focus demo: subscribed to hardware input events"
check "[OK] input focus demo: a second sys_input_subscribe correctly failed (already subscribed)"
check "[OK] input router: routed a click to pid 0x"
check "[OK] input focus demo: received a real routed click event"
check "kernel shell -- type 'help' for commands"

if grep -qF "[FAIL] input focus demo" "$SERIAL_LOG" 2>/dev/null; then
    echo "FAIL: the input focus demo's own self-check reported failure" >&2
    fail=1
fi

# The routed click must land at EXACTLY the expected screen position:
# cursor.c starts centered, "mouse_move 40 0" moves it +40 in X only
# (screen_width/2 + 40, screen_height/2) -- read the actual negotiated
# framebuffer dimensions straight out of the boot log (the same
# self-reported values test_framebuffer_selftest.sh's own screendump
# check trusts implicitly) rather than assuming 1024x768, so this
# stays correct even if the bootloader ever negotiates a different
# mode (framebuffer.c's own doc comment: never hardcode it).
fb_w_hex=$(grep -oP 'graphics framebuffer console initialized, \K0x[0-9a-f]+(?= x )' "$SERIAL_LOG" 2>/dev/null | head -1 || true)
if [ -z "$fb_w_hex" ]; then
    echo "FAIL: could not read the negotiated framebuffer width from the boot log" >&2
    fail=1
else
    fb_w=$((fb_w_hex))
    expected_x=$(( fb_w / 2 + 40 ))
    expected_x_hex=$(printf '0x%016x' "$expected_x")
    click_line=$(grep -F "[OK] input router: routed a click to pid 0x" "$SERIAL_LOG" | head -1)
    if ! echo "$click_line" | grep -qF "at ($expected_x_hex, "; then
        echo "FAIL: routed click x did not match the expected position: got '$click_line', expected x=$expected_x_hex" >&2
        fail=1
    fi
fi

# Eleven processes total must now be reaped (the baseline 10 every
# other test expects, PLUS this test's own input focus demo, which
# only ever exits once a real click actually arrives).
reaped_count=$(grep -cF "exited and was reaped" "$SERIAL_LOG" 2>/dev/null || true)
if [ "$reaped_count" -ne 11 ]; then
    echo "FAIL: expected exactly 11 'exited and was reaped' messages (10 baseline + this demo), got $reaped_count" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- captured serial output ---" >&2
    cat "$SERIAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "PASS: a real injected PS/2 click was routed, via IPC, to a genuinely separate ring-3 process, at the exact expected screen position"
