"""RELIC scenes 2.2, 2.6 and 2.9 - the glowing text screens.

ORIGIN 0x0030ffd4 Logo_Run.

The running effect is four steps a frame: a height field relaxes,
the line art is stamped into it scaled by 1/frame, the shading pass
refracts a colour ramp through the field's gradient, and a c2p
pushes it to six bitplanes.

THIS MODULE COVERS THE FIRST TWO. The line art is decoded and
Logo_StampLine is transcribed; the diffusion and the refraction are
not, so what this draws is the SEED, not the finished screen. The
viewer labels it as such rather than approximating the rest.

    py -3.10 sim/logo.py        self-test
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), 'tools', 'hunk'))

import imgaddr                                            # noqa: E402

VARIANTS = 0x00310c0c     # g_LogoVariants, 3 entries of 0x10
LINE_ART = 0x0031183c     # g_LogoLineArt
BLOCK = 0x462             # the stride between lines
ROW_BYTES = 66            # a line is 528 x 16, row-major
W = ROW_BYTES * 8
H = 16
COLUMNS = 16              # dbf 0xf in Logo_StampLine
STAMP_STRIDE = 0x100      # the A2 step

# THE ART IS STORED SQUASHED - sixteen rows to a 528-pixel line - and
# the display stretches it about eight times vertically. Read at 1:1
# it is unrecognisable.
DISPLAY_YSCALE = 8

# ORIGIN 0x003100be - which column each line is stamped at, by how
# many lines the variant has. The LAST offset goes to the highest
# column, so a three-line screen reads (0x8,A6) downward.
START_COLUMNS = {1: (0x10,), 2: (0x28, 0x10), 3: (0x40, 0x28, 0x10)}

_cache = {}


def _image():
    if 'img' not in _cache:
        _cache['img'] = open(os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            'reverse', 'ghidra', 'relic'), 'rb').read()
    return _cache['img']


def variants():
    """The three 16-byte entries: three line offsets and the index."""
    if 'var' not in _cache:
        _n, off = imgaddr.a2f(VARIANTS)
        img = _image()
        out = []
        for v in range(3):
            o = off + v * 0x10
            a, b, c, idx = struct.unpack('>4I', img[o:o + 16])
            # A ZERO PAST THE FIRST MEANS UNUSED, and variant 0's entry
            # is all zeros because its one line really does sit at
            # offset 0 - so the first slot always counts.
            lines = [a] + [x for x in (b, c) if x]
            out.append({'lines': lines, 'index': idx})
        _cache['var'] = out
    return _cache['var']


def line_bits(offset):
    """One line of art as H rows of W bits.

    A SET BIT IS THE LETTER [observed]. Logo_StampLine turns a set
    bit into 0x4f and a clear one into 0, so the letters arrive in
    the height field as the bright part.

    THE INK COUNTS DO NOT SETTLE THIS - see docs/parts.txt section
    3. The lettering is heavily stylised and does not read as text
    at a glance, so the sense was settled against the running demo,
    not from the art.
    """
    key = ('l', offset)
    if key in _cache:
        return _cache[key]
    _n, off = imgaddr.a2f(LINE_ART + offset)
    img = _image()
    rows = []
    for y in range(H):
        r = []
        for b in img[off + y * ROW_BYTES: off + (y + 1) * ROW_BYTES]:
            for k in range(8):
                r.append(1 if b & (0x80 >> k) else 0)
        rows.append(r)
    _cache[key] = rows
    return rows


def stamp_line(offset, start_col, buf, rows):
    """ORIGIN 0x0031025e  Logo_StampLine, transcribed.

    Writes 16 columns of 528 into `buf`, whose rows are
    STAMP_STRIDE bytes. IT TRANSPOSES: the art is row-major 528 x 16
    and this lays it out as columns, which is why the display reads
    the buffer the other way round.

    The running average with the cell above is the seed glow - an
    edge arrives in the field already soft.
    """
    img = _image()
    _n, src = imgaddr.a2f(LINE_ART + offset)
    a1 = start_col
    for _c in range(COLUMNS):
        a1 -= 1
        a2 = a1
        for _b in range(ROW_BYTES):
            d0 = img[src]
            src += 1
            for _k in range(8):
                d3 = 0
                d0 <<= 1
                if d0 & 0x100:
                    d3 = 0x4f
                d0 &= 0xff
                above = buf[a2 - STAMP_STRIDE] if a2 >= STAMP_STRIDE else 0
                if 0 <= a2 < len(buf):
                    buf[a2] = ((d3 + above) >> 1) & 0xff
                a2 += STAMP_STRIDE
    return buf


def stamp_variant(v):
    """Stamp every line of variant v and return (buf, rows)."""
    lines = variants()[v]['lines']
    cols = START_COLUMNS[len(lines)]
    rows = W + 2
    buf = bytearray(STAMP_STRIDE * rows)
    for off, col in zip(lines, cols):
        stamp_line(off, col, buf, rows)
    return buf, rows


def variant_rgb(v, colour=(0, 235, 80), yscale=DISPLAY_YSCALE):
    """The variant's lines as packed RGB rows, at the display aspect.

    This is THE ART, not the running effect - no diffusion and no
    refraction. Letters are drawn where the bit is SET.
    """
    lines = variants()[v]['lines']
    out = []
    for off in lines:
        bits = line_bits(off)
        for y in range(H):
            row = bits[y]
            line = bytearray()
            for x in range(W):
                line += bytes(colour if row[x] else (0, 0, 0))
            b = bytes(line)
            for _ in range(yscale):
                out.append(b)
        for _ in range(yscale):
            out.append(bytes(W * 3))
    return out, W, len(out)


def _selftest():
    vs = variants()
    for n, v in enumerate(vs):
        print('variant %d  index %d  %d line(s) at %s'
              % (n, v['index'], len(v['lines']),
                 ' '.join('0x%04x' % x for x in v['lines'])))
    assert [len(v['lines']) for v in vs] == [1, 2, 3], 'line counts'

    allo = [o for v in vs for o in v['lines']]
    assert allo == [0x0, 0x462, 0x8c4, 0xd26, 0x1188, 0x15ea], allo
    assert all(o % BLOCK == 0 for o in allo), 'offsets must be block steps'
    print('six lines, every offset a multiple of 0x%x' % BLOCK)

    fracs = []
    for o in allo:
        bits = line_bits(o)
        set_ = sum(sum(r) for r in bits)
        tot = W * H
        frac = set_ / float(tot)
        assert 0.3 < frac < 0.9, (o, frac)
        fracs.append(frac)
        print('  line 0x%04x  %.0f%% ink' % (o, frac * 100))
    # Ink tracks how much of the 528 pixels the word fills, which is
    # what settles the bit sense: the longest line carries the most
    # and the shortest ones the least. If a set bit were the page,
    # this would run the other way.
    assert fracs[0] == max(fracs), 'nerve axis should carry the most ink'
    assert fracs[0] > 0.7 and min(fracs) < 0.4, fracs
    print('ink tracks word length - a set bit is the letter')

    buf, rows = stamp_variant(2)
    cols = sorted(set(i % STAMP_STRIDE for i, b in enumerate(buf) if b))
    print('variant 2 stamped into columns %d..%d' % (min(cols), max(cols)))
    assert max(cols) < 0x40, 'three lines should fit under column 0x40'

    rgb, w, h = variant_rgb(0)
    print('variant 0 renders %d x %d at the display aspect' % (w, h))


if __name__ == '__main__':
    _selftest()
