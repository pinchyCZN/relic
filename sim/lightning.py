"""RELIC - the lightning bolt, the glow blob and the light channel.

The demo's framebuffer pixel is four bytes and only three of them are
colour. BYTE +2 IS A LIGHT ACCUMULATOR: Draw_LineAA and Draw_GlowBlob
write it and nothing else does, textures leave it zero, and the HAM8
encode tables fold it into all three channels on the way out.

Everything here is transcribed from the binary. The two big tables are
read out of the image; the three ramps and the saturating lookup are
built by the same arithmetic the demo builds them with.

    py -3.10 sim/lightning.py        self-test
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), 'tools', 'hunk'))

import imgaddr                                            # noqa: E402
import texmap                                             # noqa: E402

FB_W = 160
FB_H = 100

POLAR_BASE = 0x0029f97a   # 65536 words; +0x10000 is the signed centre
FALLOFF_BASE = 0x002bf97a  # [radius][distance] -> ramp index

LIGHT_MAX = 0x3f          # Ramp_BuildSaturate clamps here
BASE_COLOUR = (0x3f, 0x4f, 0x6f)
# ORIGIN 0x002cc596 - the triple Space_RunPart hands Ham8_BuildTables.
# WHICH OF THE THREE IS RED IS NOT SETTLED: the packing before
# Ham8_TransposeLongword has not been unpicked. It only affects
# intermediate light levels - every channel clamps to 0x3f, so a fully
# lit pixel is white whichever way round they go.

_cache = {}


def _image():
    if 'img' not in _cache:
        _cache['img'] = open(os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            'reverse', 'ghidra', 'relic'), 'rb').read()
    return _cache['img']


def polar_table():
    """The 65536-word table at 0x0029f97a, as (angle, radius) byte pairs.

    ONE TABLE SERVES TWO UNRELATED CALLERS. Uv_AtanLookup takes the
    HIGH byte of a word and uses it as an angle; Draw_GlowBlob takes
    the LOW byte and uses it as a distance. Indexed by a signed
    (dy << 8) | dx, which is why the readers all add 0x10000 first.
    """
    if 'polar' not in _cache:
        _n, off = imgaddr.a2f(POLAR_BASE)
        raw = _image()[off:off + 0x20000]
        _cache['polar'] = raw
    return _cache['polar']


def falloff_table():
    """The table at 0x002bf97a: [radius << 8 | distance] -> ramp index."""
    if 'fall' not in _cache:
        _n, off = imgaddr.a2f(FALLOFF_BASE)
        _cache['fall'] = _image()[off:off + 0x8000]
    return _cache['fall']


def ramp_linear():
    """ORIGIN 0x002c9fba - the bolt-node ramp at 0x002f06b0.

        v = min(0x3f, (0xff - i) >> 1)

    Flat at full brightness out to i = 129, then straight down to 0.
    """
    return bytes(min(LIGHT_MAX, (0xff - i) >> 1) for i in range(256))


def ramp_inverse_square(d3, d7=0x50ffff):
    """ORIGIN 0x002ca00e - the spark ramps at 0x002f07d8 and 0x002f0c50.

        t = (i * d3) >> 7
        v = min(0x3f, ((d7 - t*t) / (t*t)) >> 4)        t != 0

    An inverse-square falloff. d3 is the only thing that differs
    between the two: 0x12c for sparks, 0x96 for the growing beam's
    head. The divide truncates toward zero, as divsl.l does.
    """
    out = bytearray(256)
    for i in range(256):
        t = texmap.s32(i * d3) >> 7
        t2 = texmap.s32(t * t)
        v = d7 if t2 == 0 else texmap.sdiv(texmap.s32(d7 - t2), t2)
        v >>= 4
        out[i] = min(LIGHT_MAX, v) if v > 0 else 0
    return bytes(out)


def saturate_lut():
    """ORIGIN 0x002cff1e - the table at 0x002f0e50: min(i, 0x3f)."""
    return bytes(min(i, LIGHT_MAX) for i in range(256))


RAMP_BOLT = None
RAMP_SPARK = None
RAMP_HEAD = None
SAT = None


def _ramps():
    global RAMP_BOLT, RAMP_SPARK, RAMP_HEAD, SAT
    if SAT is None:
        RAMP_BOLT = ramp_linear()
        RAMP_SPARK = ramp_inverse_square(0x12c)
        RAMP_HEAD = ramp_inverse_square(0x96)
        SAT = saturate_lut()
    return SAT


def isqrt68k(n):
    """ORIGIN 0x002ca766 - the restoring integer square root.

    Sixteen rounds of shift-two-bits-and-trial-subtract, so it returns
    floor(sqrt(n)) exactly. Kept rather than reduced to math.isqrt
    because the demo's own rounding is what the bolt amplitude uses.
    """
    if n <= 0:
        return 0
    rem = 0
    root = 0
    for i in range(16):
        rem = (rem << 2) | ((n >> (30 - 2 * i)) & 3)
        root <<= 1
        if rem > 2 * root:
            rem -= 2 * root + 1
            root += 1
    return root


def draw_glow_blob(light, cx, cy, radius, ramp, w=FB_W, h=FB_H):
    """ORIGIN 0x002cefa4  Draw_GlowBlob - an additive round blob.

    Rejects whole against the framebuffer, then per row takes a span
    half-width and per pixel adds ramp[falloff] into the light byte
    through the saturating lookup.

    THE SPAN SHAPE COMES OUT OF THE TABLES, not out of a circle
    equation - the low byte of the polar table is the distance and
    the falloff table turns (radius, distance) into a ramp index.
    """
    sat = _ramps()
    if radius <= 0:
        return
    if cy + radius <= 0 or cy - radius >= h:
        return
    if cx + radius <= 0 or cx - radius >= w:
        return
    pol = polar_table()
    fall = falloff_table()
    fbase = (radius & 0x7f) << 8
    y0 = max(0, cy - radius)
    y1 = min(h - 1, cy + radius)
    for y in range(y0, y1 + 1):
        dy = (y - cy) & 0xff
        row = dy << 8
        x0 = max(0, cx - radius)
        x1 = min(w - 1, cx + radius)
        base = y * w
        for x in range(x0, x1 + 1):
            dx = (x - cx) & 0xff
            # THE INDEX IS SIGNED AND MEASURED FROM THE TABLE'S
            # MIDDLE: the readers all do `lea 0x29f97a,An ; adda.l
            # #0x10000,An` and then index by a signed word times two.
            off = (((row | dx) << 1) + 0x10000) & 0x1ffff
            dist = pol[off + 1]                    # the LOW byte
            add = ramp[fall[fbase | dist]]
            if add:
                light[base + x] = sat[light[base + x] + add]


def draw_line_aa(light, x0, y0, x1, y1, w=FB_W, h=FB_H):
    """ORIGIN 0x002ca848  Draw_LineAA - a three-pixel-wide line.

        centre pixel   light = 0x3f        SET, not added
        either side    light += 0x0f       a raw byte add

    THE NEIGHBOURS ARE NOT SATURATED. The demo adds 0x0f straight into
    the byte with no lookup, so a crossing of lines can push the light
    value past 0x3f - up to 0x4e, which the HAM8 table still has room
    for. Clamping here would lose that.
    """
    steep = abs(x1 - x0) <= abs(y1 - y0)
    pts = []
    if steep:
        if y1 <= y0:
            x0, y0, x1, y1 = x1, y1, x0, y0
        dy = y1 - y0
        if dy <= 0:
            return
        step = texmap.sdiv((x1 - x0) << 8, dy)
        x = x0 << 8
        for y in range(y0, y1):
            pts.append((x >> 8, y))
            x += step
    else:
        if x1 <= x0:
            x0, y0, x1, y1 = x1, y1, x0, y0
        dx = x1 - x0
        if dx <= 0:
            return
        step = texmap.sdiv((y1 - y0) << 8, dx)
        y = y0 << 8
        for x in range(x0, x1):
            pts.append((x, y >> 8))
            y += step
    for x, y in pts:
        if not (0 <= y < h):
            continue
        if 0 <= x < w:
            light[y * w + x] = LIGHT_MAX
        for nx in (x - 1, x + 1):
            if 0 <= nx < w:
                light[y * w + nx] = (light[y * w + nx] + 0x0f) & 0xff


BOLT_POINTS = 8


def bolt_path(x0, y0, x1, y1, phase, seed):
    """ORIGIN 0x002ca9e4  Beam_BuildBoltPath - eight points and radii.

        length = isqrt(dx*dx + dy*dy)
        d6 = (seed & 0x3f) << 7
        eight times:
            p  = lerp(start, end, d6 >> 8)
            p.x += (sin[(d6>>1 + phase) & 0x1fff] >> 4) * length >> 8
            p.y += (sin[(d6>>3 + phase) & 0x1fff] >> 3) * length >> 8
            r  = max(4, abs(((0x10000 - d6/2) >> 11) * length >> 9))
            d6 += 0x2000

    TWO SINES AT DIFFERENT RATES, so they never come back into step
    and the bolt reads as irregular. NO RANDOM NUMBERS ARE INVOLVED -
    `phase` advancing per frame is the whole of the crackle.

    THE RADIUS TAPERS ALONG THE BOLT. d6 climbs from 0 to 0x10000
    across the eight points, so the term falls and the glow narrows
    from root to tip, with a floor of 4. For a 120-pixel bolt that is
    about 7 pixels down to 4 - small, and the reason the bolt reads
    as a line with a halo rather than as a row of lamps.

    Returns a list of (x, y, radius).
    """
    t = texmap._sincos()
    dxs = texmap.s32(x1 - x0)
    dys = texmap.s32(y1 - y0)
    length = isqrt68k(texmap.s32(dxs * dxs) + texmap.s32(dys * dys))
    fx0, fy0 = x0 << 8, y0 << 8
    dx = texmap.s32((x1 << 8) - fx0)
    dy = texmap.s32((y1 << 8) - fy0)
    d6 = (seed & 0x3f) << 7
    out = []
    for _ in range(BOLT_POINTS):
        walk = d6
        f = d6 >> 8
        px = texmap.s32(fx0 + (texmap.s32(dx * f) >> 8))
        py = texmap.s32(fy0 + (texmap.s32(dy * f) >> 8))
        a = ((d6 >> 1) + phase) & texmap.ANGLE_MASK
        px = texmap.s32(px + (texmap.s32((t[a] >> 4) * length) >> 8))
        b = ((d6 >> 3) + phase) & texmap.ANGLE_MASK
        py = texmap.s32(py + (texmap.s32((t[b] >> 3) * length) >> 8))
        r = texmap.s32((walk >> 1) - 0x10000)
        r = texmap.s32(-r) >> 8 >> 3
        r = texmap.s32(texmap.s32(r * length) >> 8) >> 1
        if r < 0:
            r = -r
        if r < 4:
            r = 4
        out.append((px >> 8, py >> 8, r))
        d6 += 0x2000
    return out


def draw_bolt(light, x0, y0, x1, y1, phase, seed, w=FB_W, h=FB_H):
    """ORIGIN 0x002ca800  Beam_Draw, the full-strength branch.

    NINE STROKES FOR EIGHT POINTS: the loop walks point to point and
    one more stroke closes the path back to the endpoint the dbf loop
    leaves behind.
    """
    _ramps()
    pts = bolt_path(x0, y0, x1, y1, phase, seed)
    for px, py, r in pts:
        draw_glow_blob(light, px, py, r, RAMP_BOLT, w, h)
    cx, cy = x0, y0
    for px, py, _r in pts:
        draw_line_aa(light, cx, cy, px, py, w, h)
        cx, cy = px, py
    draw_line_aa(light, cx, cy, x1, y1, w, h)


def resolve(rgb, light, n, base=BASE_COLOUR):
    """ORIGIN 0x002ca456  Ham8_BuildTables - what the light byte does.

        d5  = max(0, 0xff - 4*light)
        out = min(0x3f00, base*256 + (c - base)*d5) >> 8

    SO LIGHT FADES THE PIXEL TOWARD A BRIGHT BASE COLOUR. It is not
    added as a fourth channel and it is not a separate plane - the
    encode table does the blend, one lookup per channel pair, with no
    per-pixel arithmetic at all. Light 0 leaves the texture's own
    colour; light 64 and over is the base colour outright.

    THE SIM'S FRAMEBUFFER IS 8 BITS A CHANNEL AND THE DEMO'S IS 6, so
    this drops to 6, blends, and scales back. That rounding is ours.
    """
    out = bytearray(n * 3)
    tab = {}
    for i in range(n):
        lv = light[i]
        if lv == 0:
            out[i * 3:i * 3 + 3] = rgb[i * 3:i * 3 + 3]
            continue
        d5 = 0xff - 4 * lv
        if d5 < 0:
            d5 = 0
        for k in range(3):
            c6 = rgb[i * 3 + k] >> 2
            key = (d5, k, c6)
            v = tab.get(key)
            if v is None:
                b = base[k]
                r = b * 256 + (c6 - b) * d5
                if r > 0x3f00:
                    r = 0x3f00
                if r < 0:
                    r = 0
                v = ((r >> 8) * 255) // 0x3f
                tab[key] = v
            out[i * 3 + k] = v
    return out


def resolve_into(fb, light, w, h, box=None, base=BASE_COLOUR):
    """Blend a light buffer into an existing RGB framebuffer, in place.

    Same arithmetic as resolve(). It takes a bounding box because a
    bolt covers a few per cent of a large window and walking every
    pixel costs more than drawing the bolt did.
    """
    x0, y0, x1, y1 = box if box else (0, 0, w - 1, h - 1)
    x0 = max(0, x0)
    y0 = max(0, y0)
    x1 = min(w - 1, x1)
    y1 = min(h - 1, y1)
    tab = {}
    for y in range(y0, y1 + 1):
        row = y * w
        for x in range(x0, x1 + 1):
            lv = light[row + x]
            if not lv:
                continue
            d5 = 0xff - 4 * lv
            if d5 < 0:
                d5 = 0
            o = (row + x) * 3
            for k in range(3):
                c6 = fb[o + k] >> 2
                key = (d5, k, c6)
                v = tab.get(key)
                if v is None:
                    b = base[k]
                    r = b * 256 + (c6 - b) * d5
                    if r > 0x3f00:
                        r = 0x3f00
                    if r < 0:
                        r = 0
                    v = ((r >> 8) * 255) // 0x3f
                    tab[key] = v
                fb[o + k] = v


# ORIGIN 0x002ee8a0 and 0x002ee8f4 - the six beam objects' geometry,
# read straight out of the image as 16.16 object-space pairs.
#
# THE TWO GROUPS ARE A CHAIN, NOT A CHOICE. Group A runs from the
# disc perimeter up to three points at radius 0.664; group B starts
# at those SAME three points and converges on (0, 0, 0.781), which is
# the orb's own position at 0x002ee894. So the lightning walks from
# the hull to the spikes and then into the claw's grip.
BEAMS_A = [((0x00000, 0x10400, -0xaa00), (0x0000, 0xaa00, 0x7800)),
           ((0x0e100, -0x8200, -0xaa00), (0x9300, -0x5500, 0x7800)),
           ((-0xe100, -0x8200, -0xaa00), (-0x9300, -0x5500, 0x7800))]
BEAMS_B = [((0x0000, 0xaa00, 0x7800), (0, 0, 0xc800)),
           ((0x9300, -0x5500, 0x7800), (0, 0, 0xc800)),
           ((-0x9300, -0x5500, 0x7800), (0, 0, 0xc800))]
ORB_POS = (0, 0, 0xc800)


def _selftest():
    import math
    for v in (0, 1, 2, 3, 99, 1000, 65535, 1 << 30, (1 << 31) - 1):
        assert isqrt68k(v) == math.isqrt(v), v
    print('isqrt68k matches math.isqrt over the range')

    sat = saturate_lut()
    assert sat[0] == 0 and sat[0x3f] == 0x3f and sat[0xff] == 0x3f
    rb = ramp_linear()
    print('ramp_linear   [0]=%d [129]=%d [192]=%d [255]=%d'
          % (rb[0], rb[129], rb[192], rb[255]))
    rs = ramp_inverse_square(0x12c)
    print('ramp_spark    [0]=%d [8]=%d [32]=%d [128]=%d [255]=%d'
          % (rs[0], rs[8], rs[32], rs[128], rs[255]))

    pol = polar_table()
    print('polar table   %d bytes, centre word = 0x%04x'
          % (len(pol), (pol[0x10000] << 8) | pol[0x10001]))
    fall = falloff_table()
    print('falloff table %d bytes, [r=30][d=0]=%d [r=30][d=200]=%d'
          % (len(fall), fall[(30 << 8)], fall[(30 << 8) | 200]))

    for ph in (0, 1000, 4000):
        pts = bolt_path(20, 50, 140, 50, ph, 0)
        print('bolt phase %-5d %s' % (ph, ' '.join(
            '%d,%d r%d' % q for q in pts)))

    light = bytearray(FB_W * FB_H)
    draw_bolt(light, 20, 50, 140, 50, 0, 0)
    lit = sum(1 for v in light if v)
    print('drew a bolt: %d lit pixels, peak %d' % (lit, max(light)))

    rgb = bytearray([0x20, 0x30, 0x40] * (FB_W * FB_H))
    out = resolve(rgb, light, FB_W * FB_H)
    print('resolve: unlit %s  lit %s'
          % (list(out[0:3]), list(out[(50 * FB_W + 80) * 3:
                                      (50 * FB_W + 80) * 3 + 3])))

    fb2 = bytearray([0x20, 0x30, 0x40] * (FB_W * FB_H))
    resolve_into(fb2, light, FB_W, FB_H)
    assert bytes(fb2) == bytes(out), 'resolve_into disagrees with resolve'
    print('resolve_into matches resolve')

    import math as _m
    for nm, grp in (('A', BEAMS_A), ('B', BEAMS_B)):
        for i, (a, b) in enumerate(grp):
            fa = [v / 65536.0 for v in a]
            fb3 = [v / 65536.0 for v in b]
            print('  beam %s%d  r=%.3f z=%+.3f  ->  r=%.3f z=%+.3f'
                  % (nm, i, _m.hypot(fa[0], fa[1]), fa[2],
                     _m.hypot(fb3[0], fb3[1]), fb3[2]))
    assert BEAMS_A[0][1] == BEAMS_B[0][0], 'the chain must join'
    assert BEAMS_B[0][1] == ORB_POS, 'group B must end at the orb'
    print('the two groups chain, and group B ends on the orb')


if __name__ == '__main__':
    _selftest()
