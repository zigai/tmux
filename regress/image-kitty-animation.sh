#!/bin/sh

# Kitty graphics animation playback on Kitty, SIXEL and text clients.

PATH=/bin:/usr/bin
TERM=screen
export PATH TERM
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v python3 >/dev/null || exit 101

python3 -B - "$TEST_TMUX" <<'PY'
import sys
import time
from image_test import image_test, kitty
from image_kitty import KittyTerminal, DIACRITICS, PLACEHOLDER
from image_terminal import Terminal

red, green, blue, white = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)


def pixel(colour):
    return bytes((*colour, 255))


def animation(id, colours, gap, state=3, loops=0):
    """Upload an image with one frame per colour and start playback."""
    out = kitty('a=t,i=%d,f=32,s=1,v=1' % id, pixel(colours[0]))
    for colour in colours[1:]:
        out += kitty('a=f,i=%d,f=32,s=1,v=1,z=%d' % (id, gap), pixel(colour))
    out += kitty('a=a,i=%d,r=1,z=%d' % (id, gap))
    if loops:
        out += kitty('a=a,i=%d,v=%d' % (id, loops))
    if state:
        out += kitty('a=a,i=%d,s=%d' % (id, state))
    return out


def put(id):
    """Place two columns and one row at the top left."""
    return b'\033[H' + kitty('a=p,i=%d,C=1,c=2,r=1' % id)


def placeholder(id):
    """Show a one-cell virtual placement through a placeholder at the top
    left."""
    return kitty('a=p,i=%d,U=1,c=1,r=1' % id) + \
        b'\033[H\033[38;5;%dm' % id + \
        (PLACEHOLDER + chr(DIACRITICS[0]) * 2).encode() + b'\033[0m'


def emit(test, data, terminal):
    """Send pane output without dropping the animation output around it."""
    client = test.clients[0]
    test.marker += 1
    title = 'image-%d' % test.marker
    test.send(data + b'\033]2;' + title.encode() + b'\007')
    test.wait(lambda: test.run('display-message', '-p', '#{pane_title}') ==
        title)
    client.read(0.2)
    terminal.feed(client.data)
    client.data.clear()


def watch(test, client, terminal, duration, colour):
    """Collect the distinct colours shown over time."""
    seen = []
    end = time.monotonic() + duration
    while time.monotonic() < end:
        client.read(0.01)
        terminal.feed(client.data)
        client.data.clear()
        now = colour(terminal)
        if not seen or seen[-1] != now:
            seen.append(now)
    return seen


def kitty_colour(terminal):
    return terminal.render().get((4, 4))


def cycle(seen, colours):
    """Check colours are shown in order, starting anywhere in the cycle."""
    start = colours.index(seen[0])
    for i, colour in enumerate(seen):
        assert colour == colours[(start + i) % len(colours)], (seen, colours)


with image_test(sys.argv[1]) as test:
    client = test.attach(kitty_graphics=True)
    terminal = KittyTerminal()

    # Each frame is uploaded once, then frames are switched by number.
    emit(test, animation(1, [red, green, blue], 100) + put(1), terminal)
    seen = watch(test, client, terminal, 1.2, kitty_colour)
    cycle(seen, [red, green, blue])
    assert 7 <= len(seen) <= 15, ('unexpected frame rate', seen)
    assert terminal.uploads == 1, terminal.uploads
    assert terminal.frame_uploads == 2, terminal.frame_uploads
    assert len(terminal.switches) >= 6, terminal.switches

    # Playback stops while the image is hidden and resumes when shown.
    test.run('new-window', 'stty raw -echo; exec cat')
    hidden = watch(test, client, terminal, 0.2, kitty_colour)
    assert hidden[-1] is None and not terminal.images, hidden
    switches = len(terminal.switches)
    watch(test, client, terminal, 0.4, kitty_colour)
    assert len(terminal.switches) == switches, 'hidden image was animated'
    test.run('select-window', '-t', ':0')
    seen = watch(test, client, terminal, 0.8, kitty_colour)
    cycle([c for c in seen if c is not None], [red, green, blue])
    assert len(seen) >= 5, seen
    assert terminal.uploads == 2 and terminal.frame_uploads <= 4, (
        terminal.uploads, terminal.frame_uploads)

    # Stopping keeps the current frame; frames can then be chosen directly.
    emit(test, kitty('a=a,i=1,s=1'), terminal)
    stopped = watch(test, client, terminal, 0.3, kitty_colour)
    assert len(stopped) == 1, stopped
    emit(test, kitty('a=a,i=1,c=2'), terminal)
    assert kitty_colour(terminal) == green
    emit(test, kitty('a=a,i=1,c=3'), terminal)
    assert kitty_colour(terminal) == blue

    # Changing the shown frame updates the terminal's copy of that frame.
    uploads = terminal.frame_uploads
    emit(test, kitty('a=f,i=1,r=3,X=1,f=32,s=1,v=1', pixel(white)), terminal)
    assert kitty_colour(terminal) == white
    assert terminal.frame_uploads == uploads + 1
    edits = [c for c in terminal.commands if c.get(b'a') == b'f' and b'r' in c]
    assert len(edits) == 1, terminal.commands[-3:]

    # Loading mode stops on the last frame until another frame arrives.
    emit(test, kitty('a=d,d=I,i=1') + b'\033[2J', terminal)
    emit(test, animation(2, [red, green], 50, state=2) + put(2), terminal)
    seen = watch(test, client, terminal, 0.4, kitty_colour)
    assert seen[-1] == green and seen.count(green) == 1, seen
    emit(test, kitty('a=f,i=2,f=32,s=1,v=1,z=50', pixel(blue)), terminal)
    seen = watch(test, client, terminal, 0.4, kitty_colour)
    assert seen[-1] == blue and seen.count(blue) == 1, seen

    # A loop count of three plays the frames twice and stays on the last.
    emit(test, kitty('a=d,d=I,i=2') + b'\033[2J', terminal)
    switches = len(terminal.switches)
    emit(test, animation(3, [red, green, blue], 60, loops=3) + put(3),
        terminal)
    watch(test, client, terminal, 0.6, kitty_colour)
    seen = watch(test, client, terminal, 0.3, kitty_colour)
    assert seen == [blue], seen
    assert 4 <= len(terminal.switches) - switches <= 5, \
        terminal.switches[switches:]

    # Deleting a frame keeps the remaining frames in order.
    emit(test, kitty('a=a,i=3,s=1,c=1') + kitty('a=d,d=f,i=3,r=2') +
        kitty('a=a,i=3,s=3,v=1'), terminal)
    seen = watch(test, client, terminal, 0.5, kitty_colour)
    cycle(seen, [red, blue])
    assert len(seen) >= 4, seen

    # A deleted image stops animating.
    emit(test, kitty('a=d,d=I,i=3'), terminal)
    switches = len(terminal.switches)
    seen = watch(test, client, terminal, 0.3, kitty_colour)
    assert seen == [None] and len(terminal.switches) == switches, seen

    # A new client plays the animation too, and a cell size change uploads
    # the frames again at the new size.
    emit(test, animation(4, [red, green, blue], 100) + put(4), terminal)
    other = test.attach(kitty_graphics=True)
    second = KittyTerminal(cw=10, ch=20)
    test.resize(other, 40, 16, cw=10, ch=20)
    seen = watch(test, other, second, 0.8, kitty_colour)
    cycle(seen, [red, green, blue])
    assert len(seen) >= 5 and second.uploads == 1, (seen, second.uploads)

with image_test(sys.argv[1]) as test:
    # Placeholder cells play the animation through the virtual placement,
    # and relative placements of an animated parent play with it.
    client = test.attach(kitty_graphics=True)
    terminal = KittyTerminal()
    emit(test, animation(5, [red, green, blue], 100) + placeholder(5),
        terminal)
    seen = watch(test, client, terminal, 1.0, kitty_colour)
    cycle(seen, [red, green, blue])
    assert len(seen) >= 6 and terminal.uploads == 1, (seen, terminal.uploads)
    assert len(terminal.switches) >= 5, terminal.switches

    emit(test, kitty('a=d,d=I,i=5') + b'\033[2J', terminal)
    emit(test, animation(1, [red, green, blue], 100) + put(1) +
        animation(2, [blue, white], 100) +
        kitty('a=p,i=2,p=1,P=1,H=3,C=1,c=2,r=1'), terminal)
    seen = watch(test, client, terminal, 1.0, lambda terminal:
        (kitty_colour(terminal), terminal.render().get((28, 4))))
    cycle([parent for parent, _ in seen], [red, green, blue])
    children = [child for _, child in seen]
    assert set(children) == {blue, white}, seen
    assert len(set(seen)) >= 4, seen

with image_test(sys.argv[1]) as test:
    # SIXEL clients redraw placeholder cells for each frame.
    client = test.attach()
    terminal = Terminal()
    emit(test, animation(1, [red, green, blue], 100) + placeholder(1),
        terminal)
    seen = watch(test, client, terminal, 1.0,
        lambda terminal: terminal.pixels.get((4, 4)))
    cycle(seen, [red, green, blue])
    assert len(seen) >= 6, seen

with image_test(sys.argv[1]) as test:
    # Text clients redraw placeholder cells for each frame.
    test.run('set-option', '-as', 'terminal-features', 'screen-256color:RGB')
    client = test.attach(graphical=False)
    terminal = Terminal()
    emit(test, animation(1, [red, green, blue], 100) + placeholder(1),
        terminal)
    seen = watch(test, client, terminal, 1.0,
        lambda terminal: terminal.cells.get((0, 0), (None,) * 3)[2])
    seen = [colour for colour in seen if colour is not None]
    assert len(set(seen)) == 3 and len(seen) >= 6, seen

with image_test(sys.argv[1]) as test:
    # SIXEL clients are sent the changed image for each frame.
    client = test.attach()
    terminal = Terminal()
    emit(test, animation(1, [red, green, blue], 100) + put(1), terminal)
    seen = watch(test, client, terminal, 1.0,
        lambda terminal: terminal.pixels.get((4, 4)))
    cycle(seen, [red, green, blue])
    assert 6 <= len(seen) <= 13, seen

with image_test(sys.argv[1]) as test:
    # Text clients redraw the cells for each frame.
    client = test.attach(graphical=False)
    terminal = Terminal()
    emit(test, animation(1, [red, green, blue], 100) + put(1), terminal)
    seen = watch(test, client, terminal, 1.0,
        lambda terminal: terminal.cells.get((0, 0), (None,) * 3)[2])
    seen = [colour for colour in seen if colour is not None]
    assert len(set(seen)) == 3 and len(seen) >= 6, seen
PY
