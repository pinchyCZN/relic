"""Convert the PPM frames relic_ppm writes into PNGs.

    py -3.10 tools/ppm2png.py FILE_OR_DIR [OUTDIR]

A directory converts every .ppm in it. Without OUTDIR each PNG is
written beside its PPM. Stdlib only.

THE PPM DROPS THE LIGHT CHANNEL, exactly as the emulator's own ppm=
does - it is three bytes a pixel. A glow is an addition to byte +2
and so cannot be seen in one; use the .raw files to compare those.
"""

import os
import struct
import sys
import zlib


def read_ppm(path):
    f = open(path, 'rb')
    data = f.read()
    f.close()
    if not data.startswith(b'P6'):
        raise ValueError('%s is not a P6 PPM' % path)
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _maxv = fields
    return w, h, data[i:i + w * h * 3]


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y * w * 3:(y + 1) * w * 3])

    def chunk(tag, payload):
        out = struct.pack('>I', len(payload)) + tag + payload
        return out + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
           + chunk(b'IEND', b''))
    f = open(path, 'wb')
    f.write(png)
    f.close()


def convert(src, outdir):
    w, h, rgb = read_ppm(src)
    base = os.path.splitext(os.path.basename(src))[0] + '.png'
    dst = os.path.join(outdir or os.path.dirname(src), base)
    write_png(dst, w, h, rgb)
    return dst


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    src = argv[1]
    outdir = argv[2] if len(argv) > 2 else None
    if outdir and not os.path.isdir(outdir):
        os.makedirs(outdir)
    if os.path.isdir(src):
        names = sorted(n for n in os.listdir(src) if n.endswith('.ppm'))
        for n in names:
            convert(os.path.join(src, n), outdir)
        print('%d frame(s) -> %s' % (len(names), outdir or src))
    else:
        print(convert(src, outdir))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
