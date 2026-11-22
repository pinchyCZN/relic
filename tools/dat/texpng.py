"""Dump the temple's tile textures from Faztectextures as PNG atlases.

    py -3.10 tools/dat/texpng.py CHUNKFILE OUTDIR

CHUNKFILE is the extracted chunk:

    py -3.10 tools/dat/datlist.py --extract DIR Faztectextures

FORTY TILES OF 64 BY 64, each carrying its OWN 256-entry palette -
0x400 bytes of palette then 0x1000 one-byte indices, 0x1400 a tile.
The first 24 are the ground set and the last 16 the wall set, which
is what Temple_Init's two convert loops split them into.

Each palette entry 0xAABBCCDD becomes BB CC 00 DD masked 0xfcfcfcfc
and shifted right two, so the demo's channels are six bits; this
writes them back out eight-bit for viewing. Stdlib only.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..'))
from ppm2png import write_png

TILE = 64
PAL_BYTES = 0x400
TILE_BYTES = 0x1400
GROUND_TILES = 24
WALL_TILES = 16


def tile_rgb(blob, base):
    """One tile as a list of TILE rows, each TILE*3 bytes."""
    pal = []
    for i in range(256):
        e = base + i * 4
        pal.append((blob[e + 1] & 0xfc, blob[e + 2] & 0xfc,
                    blob[e + 3] & 0xfc))
    idx = base + PAL_BYTES
    rows = []
    for r in range(TILE):
        row = bytearray()
        for c in range(TILE):
            e = pal[blob[idx + r * TILE + c]]
            row.append(e[0])
            row.append(e[1])
            row.append(e[2])
        rows.append(row)
    return rows


def atlas(blob, first, count, across):
    down = (count + across - 1) // across
    w = across * TILE
    h = down * TILE
    rows = [bytearray(w * 3) for _ in range(h)]
    for n in range(count):
        t = tile_rgb(blob, (first + n) * TILE_BYTES)
        ox = (n % across) * TILE
        oy = (n // across) * TILE
        for r in range(TILE):
            dst = rows[oy + r]
            dst[ox * 3:(ox + TILE) * 3] = t[r]
    flat = bytearray()
    for r in rows:
        flat.extend(r)
    return w, h, bytes(flat)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src, outdir = argv[1], argv[2]
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    f = open(src, 'rb')
    blob = f.read()
    f.close()

    want = (GROUND_TILES + WALL_TILES) * TILE_BYTES
    if len(blob) < want:
        sys.stderr.write('%s is %d bytes, want at least %d\n'
                         % (src, len(blob), want))
        return 1

    w, h, rgb = atlas(blob, 0, GROUND_TILES, 6)
    p = os.path.join(outdir, 'temple_ground_tiles.png')
    write_png(p, w, h, rgb)
    print('%2d ground tiles -> %s  (%dx%d, 64x64 each)'
          % (GROUND_TILES, p, w, h))

    w, h, rgb = atlas(blob, GROUND_TILES, WALL_TILES, 4)
    p = os.path.join(outdir, 'temple_wall_tiles.png')
    write_png(p, w, h, rgb)
    print('%2d wall tiles   -> %s  (%dx%d, 64x64 each)'
          % (WALL_TILES, p, w, h))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
