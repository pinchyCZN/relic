"""Score a src/ render against the demo's own framebuffer.

    py -3.10 tools/amiga/fbcmp.py DEMO OURS [options]

      --chan            per-channel means and the byte-identical count
      --rows A:B        restrict --chan to rows A up to but not B
      --bands HEADING   split the score by the ruins' heading plan
      --col N           one column, row by row
      --png PREFIX      both sides as PNGs, the haze resolved

DEMO is a .raw from amiga_fb or a .ppm from it; OURS is a .raw from
relic_ppm -raw, or a .ppm.

AND TAKE THE DEMO'S SIDE FROM THE BACK BUFFER. The HAM8 resolve
consumes byte +2 in place, so the front buffer has LIGHT zeroed and
its RGB already blended toward the light base, while relic_ppm's raw
output is pre-resolve. Scored against the front, every glow reads as
undrawn and the colours are compared across the resolve - on one
underside frame the front held zero lit pixels where the back held
2507 [measured]. Pass amiga_fb back=true.

COMPARE RAW AGAINST RAW WHERE YOU CAN. A .ppm from either side has
had the LIGHT byte dropped, and every glow in the demo lives in that
byte alone - a flare, a bolt, a spark and a lens streak all leave R,
G and B exactly as they were. Scored through a .ppm they are
invisible, so a frame can agree to within a point while one side
draws a glow the other has never heard of. That is not a theory: it
hid a missing beam-head flare for a whole session, and the ppm score
moved the wrong way when the flare was added.

AND SCORE THE CHANNELS APART, NOT THEIR LUMINANCE. A luminance score
folds red, green and blue into one number, so a frame drawn in the
wrong HUE can sit at a plausible mean while every pixel of it is
wrong. --chan prints the four channels separately and counts the
pixels that agree in all four, which is the figure that moves when
something is actually fixed.

THE RUINS DO NOT HAND THE HOST THE BUFFER THIS COMPARES. Their part
blurs and fogs on the way out, so relic_ppm -raw gives a frame two
stages past the walkers, while amiga_fb reads g_pTempleBlurSrc, which
is what the walkers wrote. Take that side from the agent instead:

    relic_dump_mem sym=g_frame#part_temple deref=1 len=64000

Other parts hand over the pre-resolve frame and -raw is right for
them.
"""

import io
import sys

W, H = 160, 100

BAND = 450
FAN = 320
# The eight walkers by band, as docs/effects/temple.txt 7 tabulates
# them: z-major, mirrored, subtracting, biased.
OCTANT = (
    (1, 0, 0, 0), (0, 0, 0, 1), (0, 0, 1, 0), (1, 1, 0, 1),
    (1, 1, 1, 0), (0, 1, 1, 1), (0, 1, 0, 0), (1, 0, 1, 1),
)


def load(path):
    """-> (rgb, light or None). Both W*H long."""
    d = io.open(path, 'rb').read()
    if d[:2] == b'P6':
        n = 0
        pos = 2
        while n < 3:
            while pos < len(d) and d[pos] in b' \t\r\n':
                pos += 1
            if pos < len(d) and d[pos:pos + 1] == b'#':
                while d[pos] not in b'\r\n':
                    pos += 1
                continue
            while pos < len(d) and d[pos] not in b' \t\r\n':
                pos += 1
            n += 1
        pos += 1
        px = d[pos:]
        return [(px[i * 3], px[i * 3 + 1], px[i * 3 + 2])
                for i in range(W * H)], None
    if len(d) != W * H * 4:
        raise SystemExit('%s is %d bytes, not a %dx%d raw'
                         % (path, len(d), W, H))
    rgb = [(d[i * 4] * 255 // 0x3f, d[i * 4 + 1] * 255 // 0x3f,
            d[i * 4 + 3] * 255 // 0x3f) for i in range(W * H)]
    rgb = [(min(255, r), min(255, g), min(255, b)) for r, g, b in rgb]
    return rgb, [d[i * 4 + 2] for i in range(W * H)]


def load_bytes(path):
    """The four-byte pixels, or None when the file is a .ppm."""
    d = io.open(path, 'rb').read()
    if d[:2] == b'P6':
        return None
    return d


def lum(p):
    return (p[0] * 30 + p[1] * 59 + p[2] * 11) // 100


def score(name, a, b):
    n = len(a)
    diff = [abs(a[i] - b[i]) for i in range(n)]
    print('%-14s mean %5.1f   >32 %4.1f%%   >64 %4.1f%%   peak %d/%d' % (
        name, sum(diff) / float(n),
        100.0 * sum(1 for v in diff if v > 32) / n,
        100.0 * sum(1 for v in diff if v > 64) / n,
        max(a), max(b)))


def chan_stats(a, b, cols, rows=None):
    """Per-channel mean |diff| and the all-four-agree count."""
    tot = [0.0] * 4
    same = 0
    n = 0
    if rows is None:
        rows = range(H)
    for x in cols:
        for y in rows:
            p = (y * W + x) * 4
            hit = True
            for k in range(4):
                v = abs(a[p + k] - b[p + k])
                tot[k] += v
                if v:
                    hit = False
            if hit:
                same += 1
            n += 1
    return [t / n for t in tot], same, n


def plan(heading):
    """The ruins' two bands for a heading, as build_heading_plan does.

    -> [(octant, first column, column count)], left to right.
    """
    h = heading % 3600
    hi = (h + FAN) & ~1
    out = []
    base = 0
    col = 0
    for band in range(9):
        width, lo, start = BAND, (h & ~1) - base, 0
        if lo > 0:
            start, width = lo, width - lo
        if width > 0:
            over = hi - base - BAND
            if over < 0:
                width += over
            if width > 0 and len(out) < 2:
                n = (width >> 1)
                out.append((band & 7, col, n))
                col += n
        base += BAND
    return out


def png_gray(px, path):
    """Three bytes a pixel, no library."""
    import struct
    import zlib

    raw = b''.join(b'\x00' + bytes(px[y * W * 3:(y + 1) * W * 3])
                   for y in range(H))

    def chunk(tag, body):
        c = tag + body
        return (struct.pack('>I', len(body)) + c
                + struct.pack('>I', zlib.crc32(c)))

    io.open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def resolved(d, fog=0x2f):
    """The haze applied the way the C2P's tables apply it."""
    out = bytearray()
    for i in range(W * H):
        r, g, l, b = d[i * 4:i * 4 + 4]
        w = max(0, 0xff - l * 4)
        for c in (r, g, b):
            v = ((c - fog) * w + (fog << 8)) >> 8
            out.append(min(63, max(0, v)) * 255 // 63)
    return out


def main(argv):
    opts = {}
    files = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--bands', '--col', '--png', '--rows'):
            if i + 1 >= len(argv):
                raise SystemExit(__doc__)
            opts[a] = argv[i + 1]
            i += 2
            continue
        if a == '--chan':
            opts[a] = True
            i += 1
            continue
        files.append(a)
        i += 1
    if len(files) != 2:
        raise SystemExit(__doc__)

    drgb, dlight = load(files[0])
    orgb, olight = load(files[1])
    # REFUSE A MIXED PAIR. One side carrying LIGHT and the other not
    # means one is pre-resolve and the other post-resolve, and an RGB
    # score then compares the demo's raw colour against the port's
    # already-blended colour. That is not a small error: it read as the
    # port being fifteen times too bright where the honest figure was
    # two, and a deferred item was opened on the strength of it.
    # relic_ppm takes -raw; use it.
    if (dlight is None) != (olight is None):
        bad = files[0] if dlight is None else files[1]
        raise SystemExit(
            'refusing to score across the resolve: ' + bad +
            ' has no LIGHT plane, so one side is pre-resolve and the'
            ' other post-resolve. Render the port with -raw and compare'
            ' raw against raw.')
    score('rgb lum', [lum(p) for p in drgb], [lum(p) for p in orgb])
    if dlight is None:
        print('NEITHER side has a LIGHT plane - both post-resolve, so'
              ' this scores resolved colour only.')
    else:
        score('light', dlight, olight)

    db, ob = load_bytes(files[0]), load_bytes(files[1])
    if (opts.get('--chan') or '--bands' in opts or '--col' in opts
            or '--png' in opts) and (db is None or ob is None):
        raise SystemExit('that option needs .raw on both sides')

    if opts.get('--chan'):
        rows = None
        if '--rows' in opts:
            lo, hi = opts['--rows'].split(':')
            rows = range(max(0, int(lo, 0)), min(H, int(hi, 0)))
            print('rows %d:%d of %d' % (rows[0], rows[-1] + 1, H))
        m, same, n = chan_stats(db, ob, range(W), rows)
        print('per channel    R %.2f  G %.2f  LIGHT %.2f  B %.2f'
              % (m[0], m[1], m[2], m[3]))
        print('identical      %d/%d (%.1f%%)'
              % (same, n, 100.0 * same / n))

    if '--bands' in opts:
        heading = int(opts['--bands'], 0)
        print('heading %d' % heading)
        for oct_, first, n in plan(heading):
            z, mir, sub, bias = OCTANT[oct_]
            m, same, cells = chan_stats(db, ob, range(first, first + n))
            print('  cols %3d..%-3d octant %d  %s%s%s%s  '
                  'R %.2f G %.2f L %.2f B %.2f  identical %.1f%%'
                  % (first, first + n - 1, oct_,
                     'z' if z else 'x', 'm' if mir else '-',
                     's' if sub else '-', 'b' if bias else '-',
                     m[0], m[1], m[2], m[3], 100.0 * same / cells))

    if '--col' in opts:
        c = int(opts['--col'], 0)
        print('column %d   demo R G L B          ours R G L B' % c)
        for y in range(H):
            p = (y * W + c) * 4
            d4 = tuple(db[p:p + 4])
            o4 = tuple(ob[p:p + 4])
            print('  y%-3d  %3d %3d %3d %3d    %3d %3d %3d %3d %s'
                  % ((y,) + d4 + o4 + ('' if d4 == o4 else '<<',)))

    if '--png' in opts:
        pre = opts['--png']
        png_gray(list(resolved(db)), pre + '_demo.png')
        png_gray(list(resolved(ob)), pre + '_ours.png')
        print('wrote %s_demo.png and %s_ours.png' % (pre, pre))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
