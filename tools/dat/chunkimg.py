#!/usr/bin/env python
"""Render a relic.dat chunk as a PNG under a stated interpretation.

    py -3.10 tools/dat/chunkimg.py rgb6 Fcrust 256 OUT.png
    py -3.10 tools/dat/chunkimg.py gray Faztecterrain 256 OUT.png
    py -3.10 tools/dat/chunkimg.py idx6 Fshiptextures 256 OUT.png

Modes:
    rgb6   4 bytes/pixel as (r, g, pad, b), each 0..0x3f, scaled to 8 bits
    gray   1 byte/pixel, used as-is
    idx6   1 byte/pixel, value 0..0x3f scaled to 8 bits

THIS IS A VIEWER, NOT A FORMAT CLAIM. The interpretation is named on the
command line precisely so that nothing here is mistaken for a decoded
format - CLAUDE.md rule 12. A render that looks like an image is
evidence worth following; it is not proof of the layout.

PNG is written with stdlib zlib only - no PIL in this project.
"""

import os
import struct
import sys
import zlib

EXTRACTED = os.path.join('reverse', 'extracted')


def root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def write_png(path, width, height, rows):
    """rows is an iterable of bytes objects, each width*3 long (RGB8)."""
    raw = bytearray()
    for r in rows:
        raw.append(0)          # filter type 0, no prediction
        raw += r

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', ihdr)
           + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)
    return len(png)


def s6(v):
    """6-bit 0..63 to 8-bit, replicating the top bits so 0x3f -> 0xff."""
    v &= 0x3f
    return (v << 2) | (v >> 4)


def render(mode, name, width, out):
    src = os.path.join(root(), EXTRACTED, name + '.bin')
    if not os.path.isfile(src):
        sys.exit('not found: %s  (run datlist.py --extract first)' % src)
    d = open(src, 'rb').read()

    if mode == 'rgb6':
        bpp = 4
        npix = len(d) // bpp
    elif mode in ('gray', 'idx6'):
        bpp = 1
        npix = len(d)
    else:
        sys.exit('unknown mode %r' % mode)

    height = npix // width
    if height == 0:
        sys.exit('width %d is wider than the data' % width)
    rows = []
    for y in range(height):
        row = bytearray()
        base = y * width * bpp
        for x in range(width):
            o = base + x * bpp
            if mode == 'rgb6':
                row += bytes((s6(d[o]), s6(d[o + 1]), s6(d[o + 3])))
            elif mode == 'gray':
                v = d[o]
                row += bytes((v, v, v))
            else:
                v = s6(d[o])
                row += bytes((v, v, v))
        rows.append(bytes(row))

    n = write_png(out, width, height, rows)
    left = npix - width * height
    print('%-16s %-5s %4d x %-4d  %d bytes png  %d pixels unused'
          % (name, mode, width, height, n, left))
    return 0


def main(argv):
    if len(argv) != 5:
        sys.exit(__doc__)
    return render(argv[1], argv[2], int(argv[3]), argv[4])


if __name__ == '__main__':
    sys.exit(main(sys.argv))
