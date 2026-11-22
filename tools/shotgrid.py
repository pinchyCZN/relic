"""A frame as a PNG with a framebuffer coordinate grid, RGB and LIGHT.

OURS IN FULL. A difference can be pointed at - "it is wrong here" -
only if both people can name the same pixel, and a position read off
a zoomed screenshot is not a framebuffer position. This labels the
grid in DEMO coordinates, the 160 by 100 a watch address is computed
from, so nobody does the division.

    py -3.10 tools/shotgrid.py build/ref/demo9.raw -o grid.png
    py -3.10 tools/shotgrid.py build/ref/demo9.raw ours.raw -o both.png

A BARE -o NAME LANDS IN build/, which is where a picture made to be
looked at belongs - not in a session scratch directory nobody can
find again. A path with a separator is used as given.
    py -3.10 tools/shotgrid.py demo7.raw --at 117,69 --base 0x583220
    py -3.10 tools/shotgrid.py --selftest

THE LIGHT PLANE GETS ITS OWN PANEL, and that is the point of this
over any screenshot. The thrusters, the sun and the portal glow write
byte +2 alone, so they are INVISIBLE in resolved colour - scoring
them through a PPM once sent a whole session chasing a regression
that was the measurement.

A PIXEL OVER CHAN_MAX IS DRAWN RED. That is a marker, not a verdict -
the demo runs past 0x3f itself along the planet's limb [observed].
What it is for is seeing WHERE each side saturates, since two frames
can agree everywhere else and clip differently.

A .raw is four bytes a pixel, R G LIGHT B, from amiga_fb raw= or from
relic_ppm -raw. A .ppm is three and carries no LIGHT; its panel says
so rather than showing black.

--at NAMES ONE PIXEL: its channels on each side, and the live address
to arm a watch on. The base is the render target amiga_fb reports and
IT MOVES BETWEEN RUNS, so it is asked for rather than remembered.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'amiga'))
sys.path.insert(0, HERE)

import fbcmp                                         # noqa: E402
from ppm2png import write_png                        # noqa: E402

W, H = fbcmp.W, fbcmp.H
CHAN_MAX = 0x3f

GRID = 10           # a line every N demo pixels
LABEL = 20          # a number every N
MARGIN_L, MARGIN_T = 26, 8
GAP = 10

# A 3x5 glyph is five rows of three bits, so a label needs no font
# file. Only the characters the panel titles and the numbers use.
GLYPH = {
    '0': (7, 5, 5, 5, 7), '1': (2, 6, 2, 2, 7), '2': (7, 1, 7, 4, 7),
    '3': (7, 1, 7, 1, 7), '4': (5, 5, 7, 1, 1), '5': (7, 4, 7, 1, 7),
    '6': (7, 4, 7, 5, 7), '7': (7, 1, 1, 1, 1), '8': (7, 5, 7, 5, 7),
    '9': (7, 5, 7, 1, 7),
    'B': (6, 5, 6, 5, 6), 'D': (6, 5, 5, 5, 6), 'E': (7, 4, 7, 4, 7),
    'G': (7, 4, 5, 5, 7), 'H': (5, 5, 7, 5, 5), 'I': (7, 2, 2, 2, 7),
    'L': (4, 4, 4, 4, 7), 'M': (5, 7, 7, 5, 5), 'O': (7, 5, 5, 5, 7),
    'R': (7, 5, 6, 5, 5), 'S': (7, 4, 7, 1, 7), 'T': (7, 2, 2, 2, 2),
    'U': (5, 5, 5, 5, 7),
    ' ': (0, 0, 0, 0, 0), '-': (0, 0, 7, 0, 0),
}


class Canvas(object):
    def __init__(self, w, h, fill=(16, 16, 20)):
        self.w, self.h = w, h
        self.px = bytearray(bytes(fill) * (w * h))

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            o = (y * self.w + x) * 3
            self.px[o:o + 3] = bytes(c)

    def hline(self, x0, x1, y, c):
        for x in range(x0, x1):
            self.put(x, y, c)

    def vline(self, x, y0, y1, c):
        for y in range(y0, y1):
            self.put(x, y, c)

    def text(self, x, y, s, c=(200, 200, 210)):
        for ch in s.upper():
            g = GLYPH.get(ch, GLYPH[' '])
            for r in range(5):
                for b in range(3):
                    if g[r] & (4 >> b):
                        self.put(x + b, y + r, c)
            x += 4
        return x


def panel(cv, ox, oy, pix, zoom, title):
    """One image with its grid, at (ox, oy) in canvas pixels."""
    cv.text(ox, oy - 7, title)
    for y in range(H):
        for x in range(W):
            v = pix[y * W + x]
            for dy in range(zoom):
                for dx in range(zoom):
                    cv.put(ox + x * zoom + dx, oy + y * zoom + dy, v)

    faint, bright = (70, 70, 80), (130, 130, 150)
    for gx in range(0, W + 1, GRID):
        c = bright if gx % LABEL == 0 else faint
        cv.vline(ox + gx * zoom, oy, oy + H * zoom, c)
    for gy in range(0, H + 1, GRID):
        c = bright if gy % LABEL == 0 else faint
        cv.hline(ox, ox + W * zoom, oy + gy * zoom, c)
    for gx in range(0, W, LABEL):
        cv.text(ox + gx * zoom + 2, oy - 7, str(gx), (150, 150, 165))
    for gy in range(0, H, LABEL):
        cv.text(ox - MARGIN_L + 2, oy + gy * zoom + 1, str(gy),
                (150, 150, 165))


def as_rgb(path):
    """(rgb, light, label). The haze is applied for the colour panel."""
    raw = fbcmp.load_bytes(path)
    if raw is None:
        rgb, light = fbcmp.load(path)
        return rgb, None, 'PPM - NO LIGHT'
    res = fbcmp.resolved(raw)
    rgb = [(res[i * 3], res[i * 3 + 1], res[i * 3 + 2])
           for i in range(W * H)]
    return rgb, [raw[i * 4 + 2] for i in range(W * H)], 'RAW'


def light_panel(light):
    """0..CHAN_MAX as grey, over it in red. Both sides do go over."""
    if light is None:
        return [(40, 20, 20)] * (W * H)
    out = []
    for v in light:
        if v > CHAN_MAX:
            out.append((255, 60, 60))
        else:
            g = v * 255 // CHAN_MAX
            out.append((g, g, g))
    return out


def build(sides, zoom, out):
    cols, rows = len(sides), 2
    pw, ph = W * zoom, H * zoom
    cv = Canvas(MARGIN_L + cols * (pw + GAP + MARGIN_L),
                MARGIN_T + rows * (ph + GAP + MARGIN_T) + 4)
    for i, (name, rgb, light, kind) in enumerate(sides):
        ox = MARGIN_L + i * (pw + GAP + MARGIN_L)
        panel(cv, ox, MARGIN_T + 7, rgb, zoom, '%s RGB' % name)
        panel(cv, ox, MARGIN_T * 2 + ph + GAP + 7, light_panel(light),
              zoom, '%s LIGHT %s' % (name, '' if light else kind))
    write_png(out, cv.w, cv.h, bytes(cv.px))
    return cv.w, cv.h


def at(sides, x, y, base):
    if not (0 <= x < W and 0 <= y < H):
        print('(%d,%d) is outside the %dx%d framebuffer' % (x, y, W, H))
        return 1
    i = y * W + x
    print('pixel (%d,%d), index %d' % (x, y, i))
    for name, rgb, light, _k in sides:
        print('  %-5s rgb %3d,%3d,%3d   light %s'
              % (name, rgb[i][0], rgb[i][1], rgb[i][2],
                 light[i] if light else '-'))
    if base is None:
        print('  no --base, so no address. amiga_fb prints the render'
              ' target it read, and IT MOVES BETWEEN RUNS.')
        return 0
    a = base + i * 4
    print('  live %08x   LIGHT byte %08x' % (a, a + 2))
    print('  every writer of this pixel, in order:')
    print('    amiga_watch addr=%x length=4' % a)
    print('    amiga_frames n=1')
    print('    amiga_hits                  the PC, the frame, the value')
    print('    amiga_watch_clear')
    return 0


def selftest():
    ok = True

    def check(name, got, want):
        nonlocal ok
        if got != want:
            print('FAIL %s: got %r want %r' % (name, got, want))
            ok = False

    cv = Canvas(20, 10, (0, 0, 0))
    cv.put(1, 1, (255, 0, 0))
    check('put', tuple(cv.px[(1 * 20 + 1) * 3:(1 * 20 + 1) * 3 + 3]),
          (255, 0, 0))
    check('put clips', cv.put(99, 99, (1, 2, 3)), None)
    check('text advances', cv.text(0, 0, '12'), 8)

    # the LIGHT ceiling is what makes an over-range pixel visible
    p = light_panel([0, CHAN_MAX, CHAN_MAX + 1] + [0] * (W * H - 3))
    check('light zero', p[0], (0, 0, 0))
    check('light full', p[1], (255, 255, 255))
    check('light over is red', p[2], (255, 60, 60))
    check('no light plane', light_panel(None)[0], (40, 20, 20))

    # every glyph is five rows of three bits
    for ch, g in GLYPH.items():
        if len(g) != 5 or max(g) > 7 or min(g) < 0:
            print('FAIL glyph %r is %r' % (ch, g))
            ok = False

    print('shotgrid selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('files', nargs='*', help='one or two .raw or .ppm')
    ap.add_argument('-o', '--out',
                    help='the PNG; a bare name lands in build/')
    ap.add_argument('--zoom', type=int, default=4)
    ap.add_argument('--at', metavar='X,Y')
    ap.add_argument('--base', type=lambda v: int(v, 0),
                    help='the render target amiga_fb reported')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if not a.files:
        ap.print_help()
        return 1
    if len(a.files) > 2:
        print('one or two files - demo first, ours second')
        return 1

    names = ('DEMO', 'OURS')
    sides = []
    for i, f in enumerate(a.files):
        rgb, light, kind = as_rgb(f)
        sides.append((names[i], rgb, light, kind))

    rc = 0
    if a.at:
        try:
            x, y = (int(v) for v in a.at.split(','))
        except ValueError:
            print('--at wants X,Y')
            return 1
        rc = at(sides, x, y, a.base)
    if a.out:
        out = a.out
        if not os.path.isabs(out) and os.sep not in out and '/' not in out:
            d = os.path.join(ROOT, 'build')
            if not os.path.isdir(d):
                os.makedirs(d)
            out = os.path.join(d, out)
        w, h = build(sides, a.zoom, out)
        print('wrote %s  %dx%d  %d panel(s), grid every %d demo px'
              % (out, w, h, len(sides) * 2, GRID))
    elif not a.at:
        print('nothing to do - give -o to write a PNG or --at to name'
              ' a pixel')
        return 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
