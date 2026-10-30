"""Decode native Kitty uploads and placements independently of tmux's image code."""

import base64
from image_terminal import Terminal

# Diacritics giving placeholder rows, columns and image ID high bytes.
DIACRITICS = [int(cp, 16) for cp in """
    0305 030d 030e 0310 0312 033d 033e 033f 0346 034a 034b 034c 0350 0351
    0352 0357 035b 0363 0364 0365 0366 0367 0368 0369 036a 036b 036c 036d
    036e 036f 0483 0484 0485 0486 0487 0592 0593 0594 0595 0597 0598 0599
    059c 059d 059e 059f 05a0 05a1 05a8 05a9 05ab 05ac 05af 05c4 0610 0611
    0612 0613 0614 0615 0616 0617 0657 0658 0659 065a 065b 065d 065e 06d6
    06d7 06d8 06d9 06da 06db 06dc 06df 06e0 06e1 06e2 06e4 06e7 06e8 06eb
    06ec 0730 0732 0733 0735 0736 073a 073d 073f 0740 0741 0743 0745 0747
    0749 074a 07eb 07ec 07ed 07ee 07ef 07f0 07f1 07f3 0816 0817 0818 0819
    081b 081c 081d 081e 081f 0820 0821 0822 0823 0825 0826 0827 0829 082a
    082b 082c 082d 0951 0953 0954 0f82 0f83 0f86 0f87 135d 135e 135f 17dd
    193a 1a17 1a75 1a76 1a77 1a78 1a79 1a7a 1a7b 1a7c 1b6b 1b6d 1b6e 1b6f
    1b70 1b71 1b72 1b73 1cd0 1cd1 1cd2 1cda 1cdb 1ce0 1dc0 1dc1 1dc3 1dc4
    1dc5 1dc6 1dc7 1dc8 1dc9 1dcb 1dcc 1dd1 1dd2 1dd3 1dd4 1dd5 1dd6 1dd7
    1dd8 1dd9 1dda 1ddb 1ddc 1ddd 1dde 1ddf 1de0 1de1 1de2 1de3 1de4 1de5
    1de6 1dfe 20d0 20d1 20d4 20d5 20d6 20d7 20db 20dc 20e1 20e7 20e9 20f0
    2cef 2cf0 2cf1 2de0 2de1 2de2 2de3 2de4 2de5 2de6 2de7 2de8 2de9 2dea
    2deb 2dec 2ded 2dee 2def 2df0 2df1 2df2 2df3 2df4 2df5 2df6 2df7 2df8
    2df9 2dfa 2dfb 2dfc 2dfd 2dfe 2dff a66f a67c a67d a6f0 a6f1 a8e0 a8e1
    a8e2 a8e3 a8e4 a8e5 a8e6 a8e7 a8e8 a8e9 a8ea a8eb a8ec a8ed a8ee a8ef
    a8f0 a8f1 aab0 aab2 aab3 aab7 aab8 aabe aabf aac1 fe20 fe21 fe22 fe23
    fe24 fe25 fe26 10a0f 10a38 1d185 1d186 1d187 1d188 1d189 1d1aa 1d1ab
    1d1ac 1d1ad 1d242 1d243 1d244
""".split()]
PLACEHOLDER = '\U0010eeee'


class KittyTerminal(Terminal):
    """Keep images separate from text, as required by the Kitty protocol."""

    def __init__(self, *args, scroll_images=False, **kwargs):
        super().__init__(*args, **kwargs)
        self.images = {}
        self.frames = {}
        self.current = {}
        self.placements = {}
        self.virtuals = {}
        self.upload = None
        self.loading = bytearray()
        self.uploads = 0
        self.frame_uploads = 0
        self.switches = []
        self.commands = []
        self.scroll_images = scroll_images

    def apc(self, data):
        """Apply a graphics command, including independently encoded chunks."""
        assert data.startswith(b'G'), data
        control, sep, payload = data[1:].partition(b';')
        fields = dict(item.split(b'=', 1) for item in control.split(b','))
        action = fields.get(b'a', b't')
        if action == b'q':
            return
        self.commands.append(fields)
        image = int(fields.get(b'i', 0))
        placement = int(fields.get(b'p', 0))
        if action in (b't', b'f') and self.upload is not None:
            assert set(fields) == {b'm'}, fields
            self.loading.extend(base64.b64decode(payload, validate=True))
            if fields[b'm'] == b'0':
                self.loaded()
        elif action in (b't', b'f'):
            assert sep and len(payload) <= 4096, (fields, len(payload))
            assert image and fields[b'f'] == b'32', fields
            assert fields[b'q'] == b'2', fields
            if action == b't':
                assert image not in self.images, image
            else:
                # Frames replace the whole canvas and never play by themselves.
                assert image in self.images and fields[b'X'] == b'1', fields
                assert b'r' in fields or fields[b'z'] == b'-1', fields
            self.upload = fields
            self.loading = bytearray(base64.b64decode(payload, validate=True))
            if fields.get(b'm', b'0') == b'0':
                self.loaded()
        elif action == b'p' and fields.get(b'U') == b'1':
            # tmux sends one virtual placement per uploaded canvas.
            assert fields[b'q'] == b'2' and image in self.images, fields
            assert image not in self.virtuals, fields
            self.virtuals[image] = int(fields[b'c']), int(fields[b'r'])
        elif action == b'p':
            assert fields[b'q'] == b'2' and fields[b'C'] == b'1', fields
            assert placement and image in self.images, fields
            width, height, _ = self.images[image]
            x, y, w, h = [int(fields[key]) for key in (b'x', b'y', b'w', b'h')]
            assert 0 <= x < x + w <= width and 0 <= y < y + h <= height
            c, r = int(fields[b'c']), int(fields[b'r'])
            # Equal aspect ratios avoid Kitty's letterboxing rule.
            assert w * r * self.ch == h * c * self.cw, fields
            self.placements[image, placement] = dict(fields,
                dst_x=self.x, dst_y=self.y)
        elif action == b'a':
            assert fields[b'q'] == b'2' and set(fields) == {b'a', b'i',
                b'c', b'q'}, fields
            frame = int(fields[b'c'])
            assert 1 <= frame <= len(self.frames[image]), fields
            width, height, _ = self.images[image]
            self.images[image] = width, height, self.frames[image][frame - 1]
            self.current[image] = frame
            self.switches.append((image, frame))
        elif action == b'd':
            assert fields[b'q'] == b'2' and fields[b'd'] in (b'i', b'I'), fields
            self.placements = {key: value for key, value in self.placements.items()
                if key[0] != image or placement and key[1] != placement}
            if fields[b'd'] == b'I':
                self.images.pop(image, None)
                self.virtuals.pop(image, None)
                self.frames.pop(image, None)
        else:
            raise AssertionError(fields)

    def loaded(self):
        """Store a completed image or frame upload."""
        fields, self.upload = self.upload, None
        width, height = int(fields[b's']), int(fields[b'v'])
        assert len(self.loading) == width * height * 4
        image, data = int(fields[b'i']), bytes(self.loading)
        if fields.get(b'a', b't') == b't':
            self.images[image] = width, height, data
            self.frames[image] = [data]
            self.current[image] = 1
            self.uploads += 1
            return
        assert self.images[image][:2] == (width, height), fields
        frames = self.frames[image]
        if b'r' in fields:
            frame = int(fields[b'r'])
            assert 1 <= frame <= len(frames), fields
            frames[frame - 1] = data
            if frame == self.current[image]:
                self.images[image] = width, height, data
        else:
            frames.append(data)
        self.frame_uploads += 1

    def scroll(self, top, bottom, n):
        """Move graphics with the scroll region when imagescroll is enabled."""
        super().scroll(top, bottom, n)
        if self.scroll_images:
            for fields in self.placements.values():
                if top <= fields['dst_y'] <= bottom:
                    fields['dst_y'] -= n

    def render(self):
        """Blend visible placements in layer, then terminal image ID order."""
        pixels = {}
        for key in sorted(self.placements, key=lambda key:
                (int(self.placements[key].get(b'z', 0)), key)):
            fields = self.placements[key]
            width, height, data = self.images[key[0]]
            sx, sy, w, h = [int(fields[k]) for k in (b'x', b'y', b'w', b'h')]
            dw, dh = int(fields[b'c']) * self.cw, int(fields[b'r']) * self.ch
            dx, dy = fields['dst_x'] * self.cw, fields['dst_y'] * self.ch
            for y in range(dh):
                for x in range(dw):
                    point = dx + x, dy + y
                    if not (0 <= point[0] < self.sx * self.cw and
                            0 <= point[1] < self.sy * self.ch):
                        continue
                    pos = ((sy + y * h // dh) * width + sx + x * w // dw) * 4
                    colour = data[pos:pos + 4]
                    alpha = colour[3]
                    if alpha:
                        under = pixels.get(point, (0, 0, 0))
                        pixels[point] = tuple((v * alpha + u * (255 - alpha))
                            // 255 for v, u in zip(colour[:3], under))
        for (x, y), cell in self.cells.items():
            if cell[0].startswith(PLACEHOLDER):
                self.placeholder(pixels, x, y, cell)
        return pixels

    def placeholder(self, pixels, x, y, cell):
        """Draw one placeholder cell, which tmux always sends complete."""
        char, fg = cell[:2]
        values = [DIACRITICS.index(ord(c)) for c in char[1:]]
        assert len(values) >= 2, ('placeholder without column', cell)
        image = fg if isinstance(fg, int) else (fg[0] << 16) | (fg[1] << 8) | fg[2]
        if len(values) > 2:
            image |= values[2] << 24
        assert image in self.virtuals, ('placeholder for unknown image', cell)
        width, height, data = self.images[image]
        columns, rows = self.virtuals[image]
        # The canvas is fitted by tmux, so no letterboxing is needed here.
        assert (width, height) == (columns * self.cw, rows * self.ch)
        row, column = values[:2]
        for yy in range(self.ch):
            for xx in range(self.cw):
                sx, sy = column * self.cw + xx, row * self.ch + yy
                colour = data[(sy * width + sx) * 4:(sy * width + sx + 1) * 4]
                if colour[3]:
                    pixels[x * self.cw + xx, y * self.ch + yy] = \
                        tuple(colour[:3])

    def check(self, expected):
        """Compare the composed graphics with expected pixel positions and colours."""
        self.pixels = self.render()
        super().check(expected)
