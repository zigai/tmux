"""Decode native Kitty uploads and placements independently of tmux's image code."""

import base64
from image_terminal import Terminal


class KittyTerminal(Terminal):
    """Keep images separate from text, as required by the Kitty protocol."""

    def __init__(self, *args, scroll_images=False, **kwargs):
        super().__init__(*args, **kwargs)
        self.images = {}
        self.frames = {}
        self.current = {}
        self.placements = {}
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
        return pixels

    def check(self, expected):
        """Compare the composed graphics with expected pixel positions and colours."""
        self.pixels = self.render()
        super().check(expected)
