#!/usr/bin/env python
"""Convert between RELIC image addresses and relic file offsets, and dump.

    py -3.10 tools/hunk/imgaddr.py a2f 0x002e483c      address -> file offset
    py -3.10 tools/hunk/imgaddr.py f2a 0x000c5578      file offset -> address
    py -3.10 tools/hunk/imgaddr.py dump 0x002e483c 64  hex dump at an address
    py -3.10 tools/hunk/imgaddr.py long 0x002e483c 68  N big-endian longwords

THERE IS NO SINGLE DELTA. Each hunk's data starts after its own block
header and the two BSS hunks take image space but no file space, so the
offset drifts by 0x3304 across the file. The per-segment table below is
the mapping; see docs/formats/hunk.txt section 2.3.

BSS segments have no file bytes at all, so an address inside BSS_01 or
BSS_04 converts to nothing and is reported as such rather than guessed.

The segment names match what Ghidra's amiga loader creates, so an
address pasted out of the listing works here unchanged.
"""

import os
import struct
import sys

# name, image_lo, image_hi, file_lo (None for BSS)  [verified, hunk.txt 2.3]
SEGMENTS = [
    ('CODE_00', 0x0021f000, 0x002c797b, 0x000038),
    ('BSS_01',  0x002c797c, 0x002c8083, None),
    ('CODE_02', 0x002c8084, 0x002c8233, 0x0a8db4),
    ('CODE_03', 0x002c8234, 0x0033be1f, 0x0a8f70),
    ('BSS_04',  0x0033be20, 0x003431a7, None),
    ('CODE_05', 0x003431a8, 0x003678cf, 0x120ed0),
    ('CODE_06', 0x003678d0, 0x003e43cf, 0x145604),
]

DEFAULT_BIN = os.path.join('reverse', 'ghidra', 'relic')


def root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def seg_of_addr(a):
    for name, lo, hi, flo in SEGMENTS:
        if lo <= a <= hi:
            return name, lo, hi, flo
    return None


def seg_of_file(o):
    for name, lo, hi, flo in SEGMENTS:
        if flo is None:
            continue
        if flo <= o <= flo + (hi - lo):
            return name, lo, hi, flo
    return None


def a2f(a):
    s = seg_of_addr(a)
    if s is None:
        raise ValueError('0x%08x is outside every segment' % a)
    name, lo, _hi, flo = s
    if flo is None:
        raise ValueError('0x%08x is in %s, which is BSS and has no file bytes'
                         % (a, name))
    return name, flo + (a - lo)


def f2a(o):
    s = seg_of_file(o)
    if s is None:
        raise ValueError('file 0x%06x is outside every segment\'s data' % o)
    name, lo, _hi, flo = s
    return name, lo + (o - flo)


def reloc_sites(blob=None):
    """[(image address, file offset, destination hunk)] per RELOC32 entry.

    A RELOCATION IS THE ONLY THING THAT TIES A BSS HUNK TO AN ADDRESS
    YOU CAN READ. A BSS hunk has no file bytes, so there is no window
    to search for live and its slide cannot be derived by matching
    content. But every pointer INTO it was patched by the loader, and
    the patch site lives in a CODE hunk: the file holds the offset
    within the destination hunk, the running machine holds that offset
    plus the destination's base. The difference is the base.

    The file is walked rather than any table being trusted - the hunk
    index a RELOC32 block names is an index into the same sequence
    SEGMENTS lists, so hunk n is SEGMENTS[n].

    Only HUNK_RELOC32 is read. This file carries 6,605 of them in three
    blocks and no short-form or 16-bit relocations at all [verified,
    tools/hunk/hunkdump.py].
    """
    if blob is None:
        blob = data()
    p, hunk, out = 0, -1, []

    def lw():
        v = struct.unpack_from('>I', blob, p)[0]
        return v

    while p + 4 <= len(blob):
        raw = lw()
        p += 4
        ht = raw & 0x3FFFFFFF
        if ht == 0x3F3:                      # HUNK_HEADER
            while True:
                n = lw()
                p += 4
                if n == 0:
                    break
                p += n * 4
            p += 4                           # table size
            first = lw()
            p += 4
            last = lw()
            p += 4
            for _ in range(first, last + 1):
                v = lw()
                p += 4
                if v >> 30 == 3:
                    p += 4
        elif ht in (0x3E9, 0x3EA):           # CODE, DATA
            hunk += 1
            n = lw() & 0x3FFFFFFF
            p += 4 + n * 4
        elif ht == 0x3EB:                    # BSS
            hunk += 1
            p += 4
        elif ht == 0x3EC:                    # RELOC32
            while True:
                count = lw()
                p += 4
                if count == 0:
                    break
                dest = lw()
                p += 4
                for _ in range(count):
                    off = lw()
                    p += 4
                    out.append((hunk, dest, off))
        elif ht == 0x3F2:                    # END
            pass
        else:
            break

    sites = []
    for src, dest, off in out:
        if not (0 <= src < len(SEGMENTS)) or not (0 <= dest < len(SEGMENTS)):
            continue
        _n, lo, _hi, flo = SEGMENTS[src]
        if flo is None:
            continue
        sites.append((lo + off, flo + off, SEGMENTS[dest][0]))
    return sites


def data():
    return open(os.path.join(root(), DEFAULT_BIN), 'rb').read()


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    cmd = argv[1]
    val = int(argv[2], 0)

    if cmd == 'a2f':
        name, o = a2f(val)
        print('0x%08x  %-8s -> file 0x%06x' % (val, name, o))
        return 0
    if cmd == 'f2a':
        name, a = f2a(val)
        print('file 0x%06x  %-8s -> 0x%08x' % (val, name, a))
        return 0
    if cmd in ('dump', 'long'):
        n = int(argv[3], 0) if len(argv) > 3 else 64
        name, o = a2f(val)
        d = data()
        if cmd == 'dump':
            print('%s  0x%08x (file 0x%06x), %d bytes' % (name, val, o, n))
            for i in range(0, n, 16):
                print('  %08x  %s' % (val + i, d[o + i:o + i + 16].hex(' ')))
        else:
            print('%s  0x%08x (file 0x%06x), %d longwords' % (name, val, o, n))
            for i in range(n):
                v = struct.unpack_from('>I', d, o + i * 4)[0]
                print('  [%3d] %08x  0x%08x  %12d' % (i, val + i * 4, v, v))
        return 0
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
