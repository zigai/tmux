#!/bin/sh

# Kitty Unicode placeholder cells on Kitty, SIXEL and text fallback clients.

PATH=/bin:/usr/bin
TERM=screen
export PATH TERM
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v python3 >/dev/null || exit 101

python3 -B - "$TEST_TMUX" <<'PY'
import sys
from image_test import image_test, kitty
from image_terminal import Terminal
from image_kitty import KittyTerminal, DIACRITICS, PLACEHOLDER

red, green, blue, white = (255, 0, 0), (0, 255, 0), (0, 0, 255), \
    (255, 255, 255)
colours = [red, green, blue, white]
raw = bytes(value for y in range(16) for x in range(16)
    for value in (*colours[2 * (y >= 8) + (x >= 8)], 255))

# Image 7 is shown four columns wide and two rows high.
image = kitty('a=t,i=7,f=32,s=16,v=16', raw) + \
    kitty('a=p,i=7,U=1,c=4,r=2')


def cell(row=None, col=None):
    """Build one placeholder cell with optional row and column."""
    text = PLACEHOLDER
    if row is not None:
        text += chr(DIACRITICS[row])
    if col is not None:
        text += chr(DIACRITICS[col])
    return text


def text(x, y, sgr=b'38;5;7', infer=False):
    """Print the whole placeholder grid with its top-left at a cell."""
    out = b''
    for row in range(2):
        out += b'\033[%d;%dH\033[%sm' % (y + row + 1, x + 1, sgr)
        if infer:
            line = cell(row) + cell() * 3
        else:
            line = ''.join(cell(row, col) for col in range(4))
        out += line.encode() + b'\033[0m'
    return out


def expected(x, y, cw=8, ch=16):
    """Pixels of the image fitted and centred in its cells at a position."""
    size = min(4 * cw, 2 * ch)
    left = x * cw + (4 * cw - size) // 2
    top = y * ch + (2 * ch - size) // 2
    return {(left + xx, top + yy): colours[2 * (yy >= size // 2) +
        (xx >= size // 2)] for yy in range(size) for xx in range(size)}


def check(client, terminal, wanted):
    """Feed new client output and check the composed pixels."""
    terminal.feed(client.data)
    terminal.check(wanted)


with image_test(sys.argv[1]) as test:
    client = test.attach(kitty_graphics=True)
    terminal = KittyTerminal()

    # Cells use the terminal's own image ID, never the application's.
    test.emit(image + text(2, 1))
    check(client, terminal, expected(2, 1))
    assert terminal.uploads == 1 and 7 not in terminal.images
    [outer] = terminal.virtuals
    test.redraw()
    check(client, terminal, expected(2, 1))
    assert terminal.uploads == 1, 'redraw uploaded the image again'

    # Missing columns are filled in from the cell to the left.
    test.emit(b'\033[2J' + text(10, 4, infer=True))
    check(client, terminal, expected(10, 4))

    # True colour IDs work too, and unknown IDs never reach the terminal.
    test.emit(b'\033[2J' + kitty('a=t,i=1193046,f=32,s=16,v=16', raw) +
        kitty('a=p,i=1193046,U=1,c=4,r=2') + text(1, 1, b'38;2;18;52;86'))
    check(client, terminal, expected(1, 1))
    test.emit(b'\033[2J' + text(1, 1, b'38;5;99'))
    assert PLACEHOLDER.encode() not in client.data
    check(client, terminal, {})

    # Scrolled-off placeholders show the image again in copy mode.
    test.emit(b'\033[2J' + text(2, 1) + b'\033[16;1H' + b'\n' * 20)
    check(client, terminal, {})
    client.clear()
    test.run('copy-mode')
    test.run('send-keys', '-X', '-N', '20', 'scroll-up')
    client.read(0.3)
    check(client, terminal, expected(2, 1))
    test.run('send-keys', '-X', 'cancel')
    client.read(0.3)
    check(client, terminal, {})

    # Placeholders move with their pane.
    test.emit(b'\033[2J' + text(2, 1))
    client.clear()
    test.run('split-window', '-hb', '-l', '10', 'exec cat')
    client.read(0.3)
    left = int(test.run('display-message', '-p', '-t', ':.1',
        '#{pane_left}'))
    assert left > 0, left
    check(client, terminal, expected(left + 2, 1))
    client.clear()
    test.run('kill-pane', '-t', ':.0')
    client.read(0.3)
    check(client, terminal, expected(2, 1))

    # Each client gets its own upload at its own cell size.
    test.emit(b'\033[2J')
    check(client, terminal, {})
    large = test.attach(kitty_graphics=True, cw=16, ch=32)
    large_terminal = KittyTerminal(cw=16, ch=32)
    square = test.attach(kitty_graphics=True, cw=16, ch=16)
    square_terminal = KittyTerminal(cw=16, ch=16)
    test.emit(b'\033[2J' + text(3, 2))
    check(client, terminal, expected(3, 2))
    check(large, large_terminal, expected(3, 2, 16, 32))
    check(square, square_terminal, expected(3, 2, 16, 16))
    test.redraw()
    check(client, terminal, expected(3, 2))
    check(large, large_terminal, expected(3, 2, 16, 32))
    check(square, square_terminal, expected(3, 2, 16, 16))
    assert large_terminal.uploads == 1 and square_terminal.uploads == 1

    # Deleting the virtual placement removes the image from the terminal.
    test.emit(kitty('a=d,d=i,i=7'))
    check(client, terminal, {})
    assert outer not in terminal.images, terminal.images.keys()

# SIXEL clients draw the placeholder images themselves.
with image_test(sys.argv[1]) as test:
    client = test.attach()
    test.emit(image + text(2, 1))
    test.redraw()
    terminal = Terminal()
    terminal.feed(client.data)
    terminal.check(expected(2, 1))
    assert PLACEHOLDER.encode() not in client.data

# Text clients show the placeholder images as coloured blocks.
with image_test(sys.argv[1]) as test:
    test.run('set-option', '-as', 'terminal-features', 'screen-256color:RGB')
    client = test.attach(graphical=False)
    test.emit(image + text(2, 1))
    test.redraw()
    terminal = Terminal()
    terminal.feed(client.data)
    for (x, y), colour in [((2, 1), red), ((5, 1), green), ((2, 2), blue),
            ((5, 2), white)]:
        char, fg, bg, bold = terminal.cells[x, y]
        assert char == '▄' and fg == bg == colour, (x, y, char, fg, bg)
PY
