#!/usr/bin/env python
"""Clear the memory flags in a hunk file's HUNK_HEADER allocation table.

    py -3.10 tools/hunk/hunk_memf_any.py reverse/ghidra/relic \
                                         reverse/patched/relic_anymem

Writes a copy with bits 30-31 of every HUNK_HEADER size longword zeroed,
so every hunk reads as MEMF_ANY.  Nothing else in the file changes - not
a byte of code, not a relocation, not a block size.

Why: Ghidra's amiga loader reads each size longword into a signed Java
int and rejects it when negative, so any hunk flagged MEMF_FAST
(0x80000000) or EXTENDED (0xC0000000) aborts the import with
"HUNK_HEADER contains invalid hunk_size".  See docs/formats/hunk.txt
section 3.

The flags only tell AmigaOS which RAM pool to allocate a hunk from.  The
loader ignores them - Relocate.getSeqAddresses lays the segments out
sequentially from the image base regardless - so the imported program is
identical either way, and every address in it is the same address it
would have had from an unpatched file.

Refuses to write over its input, and refuses a file whose header does
not parse.  Run hunkdump.py on the output to confirm.
"""

import os
import struct
import sys

HUNK_HEADER = 0x3F3
MEM_FLAG = {0: 'ANY', 1: 'CHIP', 2: 'FAST', 3: 'EXTENDED'}


def header_table_offsets(data):
    """Return (offset, raw) for each size longword in the HUNK_HEADER.

    Raises ValueError if the file does not open with a parseable
    HUNK_HEADER.
    """
    if len(data) < 24:
        raise ValueError('file is too short to hold a HUNK_HEADER')

    raw = struct.unpack_from('>I', data, 0)[0]
    if raw != HUNK_HEADER:
        raise ValueError('not a hunk load file: first longword is %08x, '
                         'expected %08x' % (raw, HUNK_HEADER))

    p = 4
    while True:
        n = struct.unpack_from('>I', data, p)[0]
        p += 4
        if n == 0:
            break
        p += n * 4

    table = struct.unpack_from('>I', data, p)[0]
    first = struct.unpack_from('>I', data, p + 4)[0]
    last = struct.unpack_from('>I', data, p + 8)[0]
    p += 12

    if not 0 <= first <= last or last - first + 1 > table or table > 0xFFFF:
        raise ValueError('implausible header: table_size=%d first=%d last=%d'
                         % (table, first, last))

    out = []
    for i in range(first, last + 1):
        raw = struct.unpack_from('>I', data, p)[0]
        out.append((i, p, raw))
        p += 4
        if raw >> 30 == 3:
            p += 4
    return out


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src, dst = argv[1], argv[2]

    if os.path.exists(dst) and os.path.samefile(src, dst):
        sys.exit('refusing to write over the input')

    data = bytearray(open(src, 'rb').read())
    try:
        entries = header_table_offsets(data)
    except ValueError as e:
        sys.exit('%s: %s' % (src, e))

    changed = 0
    for i, off, raw in entries:
        flags = raw >> 30
        if flags == 3:
            sys.exit('hunk %d uses EXTENDED memory flags; the extra longword '
                     'would have to be removed and every following offset '
                     'shifted. Not handled - fix the loader instead.' % i)
        if flags == 0:
            print('hunk %d  @%08x  %08x  already MEMF_ANY' % (i, off, raw))
            continue
        struct.pack_into('>I', data, off, raw & 0x3FFFFFFF)
        print('hunk %d  @%08x  %08x -> %08x  MEMF_%s -> MEMF_ANY'
              % (i, off, raw, raw & 0x3FFFFFFF, MEM_FLAG[flags]))
        changed += 1

    d = os.path.dirname(dst)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    open(dst, 'wb').write(bytes(data))
    print('')
    print('%s -> %s  (%d bytes, %d of %d size longwords cleared)'
          % (src, dst, len(data), changed, len(entries)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
