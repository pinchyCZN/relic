#!/usr/bin/env python
"""Walk an AmigaDOS hunk file and print every block.

    py -3.10 tools/hunk/hunkdump.py reverse/ghidra/relic

Prints the HUNK_HEADER allocation table with its memory flags decoded,
then every block in file order with its file offset, size and data
extent, and finally the offset parsing stopped at.  That last line is
the check that matters: it must equal the file size.  Anything less and
the file is truncated or a block was misparsed.

Memory flags live in bits 30-31 of a size longword: 0 ANY, 1 CHIP,
2 FAST, 3 EXTENDED (a second longword follows).  Flags 2 and 3 set the
sign bit, which is what Ghidra's amiga loader rejects - see
docs/formats/hunk.txt section 3.
"""

import struct
import sys

HUNK_TYPE = {
    0x3E7: 'HUNK_UNIT',     0x3E8: 'HUNK_NAME',    0x3E9: 'HUNK_CODE',
    0x3EA: 'HUNK_DATA',     0x3EB: 'HUNK_BSS',     0x3EC: 'HUNK_RELOC32',
    0x3ED: 'HUNK_RELOC16',  0x3EE: 'HUNK_RELOC8',  0x3EF: 'HUNK_EXT',
    0x3F0: 'HUNK_SYMBOL',   0x3F1: 'HUNK_DEBUG',   0x3F2: 'HUNK_END',
    0x3F3: 'HUNK_HEADER',   0x3F5: 'HUNK_OVERLAY', 0x3F6: 'HUNK_BREAK',
    0x3F7: 'HUNK_DREL32',   0x3F8: 'HUNK_DREL16',  0x3F9: 'HUNK_DREL8',
    0x3FA: 'HUNK_LIB',      0x3FB: 'HUNK_INDEX',
    0x3FC: 'HUNK_RELOC32SHORT',
    0x3FD: 'HUNK_RELRELOC32',
    0x3FE: 'HUNK_ABSRELOC16',
}

MEM_FLAG = {0: 'ANY', 1: 'CHIP', 2: 'FAST', 3: 'EXTENDED'}


class Reader(object):
    def __init__(self, data):
        self.d = data
        self.p = 0

    def long(self):
        v = struct.unpack_from('>I', self.d, self.p)[0]
        self.p += 4
        return v

    def word(self):
        v = struct.unpack_from('>H', self.d, self.p)[0]
        self.p += 2
        return v

    def skip(self, n):
        self.p += n


def dump(path):
    data = open(path, 'rb').read()
    r = Reader(data)
    print('%s' % path)
    print('file size %d (0x%x)' % (len(data), len(data)))
    print('')

    blocks = 0
    while r.p < len(data):
        at = r.p
        raw = r.long()
        ht = raw & 0x3FFFFFFF
        fl = raw >> 30
        name = HUNK_TYPE.get(ht, 'UNKNOWN(0x%x)' % ht)
        line = '%08x  %-18s' % (at, name)
        if fl:
            line += ' [mem=%s]' % MEM_FLAG[fl]

        if ht == 0x3F3:
            print(line)
            while True:
                n = r.long()
                if n == 0:
                    break
                print('          resident lib: %r' % data[r.p:r.p + n * 4])
                r.skip(n * 4)
            table, first, last = r.long(), r.long(), r.long()
            print('          table_size=%d first_hunk=%d last_hunk=%d'
                  % (table, first, last))
            for i in range(first, last + 1):
                v = r.long()
                print('          hunk %d  %7d lw  %9d bytes  mem=%-8s raw=%08x'
                      % (i, v & 0x3FFFFFFF, (v & 0x3FFFFFFF) * 4,
                         MEM_FLAG[v >> 30], v))
                if v >> 30 == 3:
                    print('            extended flags raw=%08x' % r.long())
        elif ht in (0x3E9, 0x3EA):
            n = r.long() & 0x3FFFFFFF
            print(line + ' %7d lw  %9d bytes  data %08x..%08x'
                  % (n, n * 4, r.p, r.p + n * 4))
            r.skip(n * 4)
        elif ht == 0x3EB:
            n = r.long() & 0x3FFFFFFF
            print(line + ' %7d lw  %9d bytes  no file data' % (n, n * 4))
        elif ht in (0x3EC, 0x3ED, 0x3EE):
            total = 0
            while True:
                count = r.long()
                if count == 0:
                    break
                r.long()
                r.skip(count * 4)
                total += count
            print(line + ' %7d offsets' % total)
        elif ht == 0x3FC:
            start = r.p
            total = 0
            while True:
                count = r.word()
                if count == 0:
                    break
                r.word()
                r.skip(count * 2)
                total += count
            if (r.p - start) % 4:
                r.skip(2)
            print(line + ' %7d offsets (short form)' % total)
        elif ht == 0x3F0:
            count = 0
            while True:
                n = r.long()
                if n == 0:
                    break
                r.skip(n * 4 + 4)
                count += 1
            print(line + ' %7d symbols' % count)
        elif ht == 0x3F1:
            n = r.long()
            r.skip(n * 4)
            print(line + ' %9d bytes debug' % (n * 4))
        elif ht == 0x3F2:
            print(line)
        else:
            print(line + '  *** not handled, stopping')
            break

        blocks += 1
        if blocks > 10000:
            print('*** more than 10000 blocks, stopping')
            break

    print('')
    print('parsing stopped at %08x of %08x -- %s'
          % (r.p, len(data), 'OK' if r.p == len(data) else 'MISMATCH'))
    return 0 if r.p == len(data) else 1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(dump(sys.argv[1]))
