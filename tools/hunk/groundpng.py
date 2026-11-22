"""Dump the temple descent's warp source textures as PNGs.

    py -3.10 tools/hunk/groundpng.py OUTDIR

The descent's ground is NOT a heightmap - it is three fixed 256x256
indexed images baked into CODE_06, each 0x400 bytes of palette
followed by 0x10000 one-byte indices, which Temple_BuildGroundTex
0x0036d0e4 expands into the longword texture the affine warp samples.
The cloud layer is a fourth table, one byte a texel, added into the
LIGHT channel. See docs/effects/temple.txt.

Each palette entry 0xAABBCCDD becomes BB CC 00 DD, masked 0xfcfcfcfc
and shifted right two, so the demo's channels are six bits; this
writes them back out eight-bit for viewing.

Stdlib only - zlib is enough to write a PNG.
"""

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import imgaddr

GROUNDS = (
    ('ground_a', 0x00384900),
    ('ground_b', 0x00394d00),
    ('ground_c', 0x003a5100),
)
CLOUDS = ('clouds', 0x003b5500)

SIDE = 256
PAL_BYTES = 0x400
IDX_BYTES = 0x10000


def write_png(path, width, height, rows):
    """rows is a list of bytearrays, each width*3 bytes of RGB."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw.extend(row)
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)

    def chunk(tag, data):
        out = struct.pack('>I', len(data)) + tag + data
        return out + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', ihdr)
           + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
           + chunk(b'IEND', b''))
    f = open(path, 'wb')
    f.write(png)
    f.close()


def ground_rows(blob, base):
    """The palette-and-index image at base, as RGB rows."""
    pal = []
    for i in range(256):
        off = base + i * 4
        b = blob[off + 1] & 0xfc
        g = blob[off + 2] & 0xfc
        r = blob[off + 3] & 0xfc
        # BB CC 00 DD >> 2 is the demo's six bits; << 2 shows them
        pal.append((b, g, r))
    rows = []
    idx0 = base + PAL_BYTES
    for y in range(SIDE):
        row = bytearray()
        for x in range(SIDE):
            e = pal[blob[idx0 + y * SIDE + x]]
            row.append(e[0])
            row.append(e[1])
            row.append(e[2])
        rows.append(row)
    return rows


def grey_rows(blob, base):
    rows = []
    for y in range(SIDE):
        row = bytearray()
        for x in range(SIDE):
            v = blob[base + y * SIDE + x]
            row.append(v)
            row.append(v)
            row.append(v)
        rows.append(row)
    return rows


def main(argv):
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    outdir = argv[1]
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    blob = imgaddr.data()

    for name, addr in GROUNDS:
        off = imgaddr.a2f(addr)[1]
        path = os.path.join(outdir, name + '.png')
        write_png(path, SIDE, SIDE, ground_rows(blob, off))
        print('%-9s image 0x%08x -> %s  (256x256, palette + indices)'
              % (name, addr, path))

    name, addr = CLOUDS
    off = imgaddr.a2f(addr)[1]
    path = os.path.join(outdir, name + '.png')
    write_png(path, SIDE, SIDE, grey_rows(blob, off))
    print('%-9s image 0x%08x -> %s  (256x256, one byte a texel)'
          % (name, addr, path))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
