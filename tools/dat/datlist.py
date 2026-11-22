#!/usr/bin/env python
"""List or extract the chunks of relic.dat.

    py -3.10 tools/dat/datlist.py                       list the directory
    py -3.10 tools/dat/datlist.py --verify               check it tiles
    py -3.10 tools/dat/datlist.py --extract DIR          every chunk
    py -3.10 tools/dat/datlist.py --extract DIR Fplasma  just one

The archive is a flat directory of fixed-width records followed by the
payloads back to back - see docs/formats/relic_dat.txt.

    u32              entry count, big-endian
    entry[count]     36 bytes each:
                       char name[16]   space padded, NOT NUL terminated
                       u32  offset     byte offset into relic.dat
                       u32  size0      bytes read from disk
                       u32  size1      headroom allocated BEFORE the data
                       u32  size2      headroom allocated after it
                       u32  ptr        runtime slot, zero on disk

Names carry a pool prefix: C for chip RAM, F for fast RAM. Dat_LoadChunk
tests it with cmpi.b #0x43 - literally 'C' - so this is read out of the
code, not guessed [verified].

The allocation is size0+size1+size2 and the data is read in at
base+size1, so a chunk can ask for scratch space either side of itself.
Only Ftemplelog uses it: 0x30000 on disk, 0x60000 allocated.

Writes only where told to, never over its input. Chunk files are named
after the chunk, so Fplasma lands at DIR/Fplasma.bin.
"""

import os
import struct
import sys

REC = 36
NAME = 16
DEFAULT_DAT = os.path.join('reverse', 'ghidra', 'relic.dat')


def root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def load(path=None):
    path = path or os.path.join(root(), DEFAULT_DAT)
    if not os.path.isfile(path):
        sys.exit('not found: %s' % path)
    data = open(path, 'rb').read()
    if len(data) < 4:
        sys.exit('%s is too short to be an archive' % path)

    count = struct.unpack_from('>I', data, 0)[0]
    dirsize = 4 + count * REC
    if count == 0 or count > 4096 or dirsize > len(data):
        sys.exit('implausible entry count %d in %s' % (count, path))

    entries = []
    for i in range(count):
        o = 4 + i * REC
        name = data[o:o + NAME].rstrip(b' ').decode('latin-1')
        off, size = struct.unpack_from('>2I', data, o + NAME)
        extra = struct.unpack_from('>3I', data, o + NAME + 8)
        entries.append({'index': i, 'name': name, 'offset': off,
                        'size': size, 'extra': extra})
    return path, data, dirsize, entries


def do_list(path, data, dirsize, entries):
    print('%s' % path)
    print('%d bytes (0x%x), %d entries, directory 4 + %d*%d = %d (0x%x)'
          % (len(data), len(data), len(entries), len(entries), REC,
             dirsize, dirsize))
    print('')
    print('%-3s %-18s %-10s %-10s %-10s %-11s %s'
          % ('#', 'name', 'offset', 'size0', 'end', 'pool', 'allocation'))
    for e in entries:
        pool = {'C': 'CHIP', 'F': 'FAST'}.get(e['name'][:1], '?')
        alloc = e['size'] + e['extra'][0] + e['extra'][1]
        ex = ('-' if alloc == e['size']
              else 'alloc %d (+%d before, +%d after)'
                   % (alloc, e['extra'][0], e['extra'][1]))
        print('%-3d %-18s 0x%08x 0x%08x 0x%08x %-11s %s'
              % (e['index'], e['name'], e['offset'], e['size'],
                 e['offset'] + e['size'], pool, ex))
    return 0


def do_verify(path, data, dirsize, entries):
    bad = 0
    for e in entries:
        if e['offset'] < dirsize:
            print('FAIL %-18s offset 0x%08x is inside the directory'
                  % (e['name'], e['offset']))
            bad += 1
        if e['offset'] + e['size'] > len(data):
            print('FAIL %-18s runs past end of file' % e['name'])
            bad += 1

    cur = dirsize
    for e in sorted(entries, key=lambda x: x['offset']):
        gap = e['offset'] - cur
        if gap:
            print('FAIL %-18s %d byte %s before it'
                  % (e['name'], abs(gap), 'gap' if gap > 0 else 'overlap'))
            bad += 1
        cur = e['offset'] + e['size']

    tail = len(data) - cur
    if tail:
        print('FAIL %d bytes after the last chunk' % tail)
        bad += 1

    if bad:
        print('')
        print('%d problems' % bad)
        return 1
    print('OK  %d chunks tile 0x%x..0x%x with no gaps, no overlaps and '
          'no tail' % (len(entries), dirsize, len(data)))
    return 0


def do_extract(path, data, dirsize, entries, outdir, only):
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    if os.path.abspath(outdir) == os.path.dirname(os.path.abspath(path)):
        sys.exit('refusing to extract into the directory holding the '
                 'original')

    wanted = [e for e in entries if not only or e['name'] in only]
    missing = set(only) - set(e['name'] for e in wanted)
    if missing:
        sys.exit('no such chunk: %s' % ', '.join(sorted(missing)))

    for e in wanted:
        blob = data[e['offset']:e['offset'] + e['size']]
        if len(blob) != e['size']:
            sys.exit('%s is truncated in the archive' % e['name'])
        dst = os.path.join(outdir, '%s.bin' % e['name'])
        open(dst, 'wb').write(blob)
        print('%-18s 0x%08x %9d bytes -> %s'
              % (e['name'], e['offset'], e['size'], dst))
    print('')
    print('%d of %d chunks written to %s' % (len(wanted), len(entries),
                                             outdir))
    return 0


def main(argv):
    args = argv[1:]
    path, data, dirsize, entries = load()

    if not args:
        return do_list(path, data, dirsize, entries)
    if args[0] == '--verify':
        return do_verify(path, data, dirsize, entries)
    if args[0] == '--extract':
        if len(args) < 2:
            sys.exit('--extract needs an output directory')
        return do_extract(path, data, dirsize, entries, args[1], args[2:])
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
