#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

cd "$(dirname "$0")" || exit 1

command -v python3 >/dev/null 2>&1 || exit 0

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest-kitty-draw-$$"
$TMUX kill-server 2>/dev/null

SUPPORT=$($TMUX -f/dev/null start-server \; display -p '#{image_support}')
$TMUX kill-server 2>/dev/null
case "$SUPPORT" in
*kitty*) ;;
*) exit 0 ;;
esac

export TEST_TMUX

python3 <<'PY'
import fcntl
import errno
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time


tmux = os.environ["TEST_TMUX"]
label = "test-kitty-draw-%d" % os.getpid()
root = os.path.abspath(os.path.join(os.getcwd(), ".."))
env = os.environ.copy()
env.pop("TMUX", None)


def run_tmux(*args, **kwargs):
    return subprocess.run(
        [tmux, "-L", label, "-f/dev/null", *args],
        cwd=root,
        env=env,
        **kwargs,
    )


def fail(message, output, details=None):
    print("[FAIL] kitty draw: %s" % message)
    print("captured %d bytes" % len(output))
    if details is not None:
        print(details)
    sys.exit(1)


run_tmux("kill-server", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
image_cmd = (
    "python3 -c 'import sys; sys.stdout.write(\"\\033_Ga=T,i=99,p=30,"
    "f=24,s=40,v=30,c=1,r=1,q=1;\" + \"QUJD\" * 1200 + "
    "\"\\033\\\\\")'; "
    "printf '\\033_Ga=T,i=77,p=9,f=24,o=z,s=2,v=2,x=1,y=2,w=3,h=4,"
    "q=1,"
    "X=5,Y=6,c=7,r=8,z=-3;QUJDREVGR0hJSktM\\033\\\\"
    "\\033_Ga=p,i=77,p=10,c=1,r=1,q=1\\033\\\\"
    "\\033[1;101H\\033_Ga=T,i=88,p=20,f=24,s=1,v=1,c=1,r=1,q=1;QUJD\\033\\\\"
    "\\033[1;1H\\033_Ga=p,i=88,p=21,c=1,r=1,q=1\\033\\\\"
    "\\033_Ga=T,f=24,s=1,v=1,c=1,r=1;QUJD\\033\\\\'; sleep 30"
)
subprocess.check_call(
    [
        tmux,
        "-L",
        label,
        "-f/dev/null",
        "start-server",
        ";",
        "set-option",
        "-g",
        "terminal-features",
        "xterm*:kitty",
        ";",
        "set-option",
        "-g",
        "window-size",
        "manual",
        ";",
        "new-session",
        "-x",
        "120",
        "-y",
        "24",
        "-d",
        "-s",
        label,
        image_cmd,
    ],
    cwd=root,
    env=env,
)

master, slave = pty.openpty()
output = b""
redraw = b""
proc = None
try:
    fcntl.ioctl(
        slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 800, 480)
    )
    client_env = env.copy()
    client_env["TERM"] = "xterm"
    proc = subprocess.Popen(
        [tmux, "-L", label, "-f/dev/null", "attach-session", "-t", label],
        cwd=root,
        env=client_env,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        preexec_fn=os.setsid,
    )
    os.close(slave)
    slave = -1

    deadline = time.time() + 6
    while time.time() < deadline:
        ready, _, _ = select.select([master], [], [], 0.25)
        if not ready:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError as e:
            if e.errno == errno.EIO:
                break
            raise
        if not chunk:
            break
        output += chunk
        if (
            b",p=10," in output
            and re.search(br"\033_Ga=p,i=[0-9]+,p=21", output)
            and b"z=-3" in output
            and b"\033_Gm=0,q=1;" in output
        ):
            break

    run_tmux(
        "refresh-client", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    deadline = time.time() + 3
    while time.time() < deadline:
        ready, _, _ = select.select([master], [], [], 0.25)
        if not ready:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError as e:
            if e.errno == errno.EIO:
                break
            raise
        if not chunk:
            break
        redraw += chunk
        if b"\033_Ga=d,d=i,i=" in redraw and b",p=30," in redraw:
            break
finally:
    if proc is not None:
        try:
            os.killpg(proc.pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
    if slave != -1:
        os.close(slave)
    os.close(master)
    run_tmux(
        "kill-server", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

places = [
    (int(image_id), int(placement_id))
    for image_id, placement_id in re.findall(
        br"\033_Ga=p,i=([0-9]+),p=([0-9]+)", output
    )
]
uploads = [
    int(image_id)
    for image_id in re.findall(br"\033_Ga=t,i=([0-9]+),", output)
]
events = []
for match in re.finditer(
    br"\033_Ga=([tp]),i=([0-9]+)(?:,p=([0-9]+))?", output
):
    events.append(
        (
            match.group(1),
            int(match.group(2)),
            int(match.group(3) or 0),
        )
    )
named_places = [
    (image_id, placement_id)
    for image_id, placement_id in places
    if placement_id in (9, 10)
]
if {placement_id for _, placement_id in named_places} != {9, 10}:
    fail(
        "missing both placements for inner image 77",
        output,
        "places=%r" % (places,),
    )
outer_ids = {image_id for image_id, _ in named_places}
if len(outer_ids) != 1:
    fail(
        "placements for inner image 77 used different outer ids",
        output,
        "places=%r uploads=%r" % (places, uploads),
    )
outer_id = outer_ids.pop()
if outer_id == 77:
    fail("outer terminal reused application image id 77", output)

if uploads.count(outer_id) < 1:
    fail(
        "named image was not uploaded for the redraw",
        output,
        "outer_id=%r places=%r uploads=%r" % (outer_id, places, uploads),
    )
paired_without_reupload = False
for i, event in enumerate(events):
    if event != (b"p", outer_id, 9):
        continue
    for later in events[i + 1 :]:
        if later == (b"t", outer_id, 0):
            break
        if later == (b"p", outer_id, 10):
            paired_without_reupload = True
            break
    if paired_without_reupload:
        break
if not paired_without_reupload:
    fail(
        "placements for inner image 77 were separated by an upload",
        output,
        "outer_id=%r events=%r" % (outer_id, events),
    )

offscreen_places = [
    image_id for image_id, placement_id in places if placement_id == 21
]
if len(offscreen_places) != 1:
    fail(
        "visible placement after offscreen first placement missing",
        output,
        "places=%r uploads=%r" % (places, uploads),
    )
offscreen_id = offscreen_places[0]
if uploads.count(offscreen_id) < 1:
    fail("offscreen first placement did not upload its image", output)
if any(placement_id == 20 for _, placement_id in places):
    fail("offscreen placement was unexpectedly emitted", output)
if not re.search(br"\033_Ga=t,i=[0-9]+,f=24,s=40,v=30,m=1,q=1;", output):
    fail("large named image upload was not chunked", output)
if b"\033_Gm=0,q=1;" not in output:
    fail("large named image upload did not emit final chunk", output)
large_places = [
    image_id for image_id, placement_id in places if placement_id == 30
]
large_ids = set(large_places)
if len(large_ids) != 1:
    fail("large image placement missing", output, "places=%r" % (places,))
large_id = large_ids.pop()
if (
    ("\033_Ga=d,d=i,i=%u,q=1\033\\" % large_id).encode("ascii")
    not in redraw
):
    fail("cached redraw did not clear stale placements", redraw)
if ("\033_Ga=t,i=%u," % large_id).encode("ascii") in redraw:
    fail("cached redraw uploaded image data again", redraw)
if (
    ("\033_Ga=p,i=%u,p=30" % large_id).encode("ascii")
    not in redraw
):
    fail("cached redraw did not replace large image placement", redraw)

for marker in (
    b"o=z",
    b"QUJDREVGR0hJSktM",
    b"x=1,y=2,w=3,h=4",
    b"X=5,Y=6,c=7,r=8,z=-3",
    b"\033_Ga=T,f=24,s=1,v=1",
):
    if marker not in output:
        fail("missing %r" % (marker,), output)
if b"\033_Ga=p,i=0" in output:
    fail("unexpected anonymous placement by id zero", output)
PY
