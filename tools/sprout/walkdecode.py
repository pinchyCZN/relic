#!/usr/bin/env py -3.10
"""Decode a Sprout walk routine out of the relic executable.

The light's octree is traversed by about 96 hand-unrolled routines in
six tables per sector - docs/effects/approach.txt section 4.6. They are
highly regular, so the column order, the split planes each one tests
and the interval clipping can be read off the bytes instead of one
disassembly at a time.

    py -3.10 tools/sprout/walkdecode.py table 0x2d71b0
    py -3.10 tools/sprout/walkdecode.py entry 0x2d71b0 15
    py -3.10 tools/sprout/walkdecode.py triples
    py -3.10 tools/sprout/walkdecode.py order

A LINEAR READ MISCOUNTS THESE ROUTINES. Each is a binary decision
tree, so a column reached from both arms of a branch appears twice in
address order. `entry` prints the branch map so the arms can be told
apart and `order` follows them properly; a blt +2 is the y-slab skip
and anything longer is part of the decision.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'hunk'))
import imgaddr

# split[] offsets from the node pointer, named by what the builder
# writes there - maybe_Sprout_BuildNode.
SPLIT = {
    0x40: 'A5',
    0x42: 'A3', 0x44: 'A3+h', 0x46: 'A3+2h',
    0x48: 'A4', 0x4a: 'A4+h', 0x4c: 'A4+2h',
    0x4e: 'D1', 0x50: 'D1+h', 0x52: 'D1+2h',
    0x54: 'D2', 0x56: 'D2+h', 0x58: 'D2+2h',
}
TRIPLE = {0x42: 'A3', 0x44: 'A3', 0x46: 'A3',
          0x48: 'A4', 0x4a: 'A4', 0x4c: 'A4',
          0x4e: 'D1', 0x50: 'D1', 0x52: 'D1',
          0x54: 'D2', 0x56: 'D2', 0x58: 'D2'}
REG = {0xb0: 'near', 0xb2: 'mid', 0xb4: 'far', 0xb6: 'row'}
SLOT = {0x00: 'col0', 0x04: 'col1', 0x10: 'col2', 0x14: 'col3'}

# The four sectors' six tables each, from the four visitor setters at
# 0x002d6cae, 0x002d6d14, 0x002d6d88 and 0x002d6e0c. Each pair is
# (positive slope -> node+0x20.., negative slope -> node+0x24..), and
# the setters differ only in how many times they put the occupancy
# nibbles through g_SproutColumnRotate: none, once, twice, three
# times - one quarter turn of the column ring per sector.
TABLES = {
    0: (0x2d8480, 0x2d71b0, 0x2d8b10, 0x2d785e, 0x2d90e2, 0x2d7e48),
    1: (0x2daae0, 0x2d980a, 0x2db178, 0x2d9eb6, 0x2db752, 0x2da4a8),
    2: (0x2dd148, 0x2dbe7a, 0x2dd7d6, 0x2dc526, 0x2ddda0, 0x2dcb10),
    3: (0x2df788, 0x2de4cc, 0x2dfe20, 0x2deb6c, 0x2e03fa, 0x2df14e),
}


def u32(b, o):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def entries(img, addr, n=16):
    """the n code pointers of one jump table

    THE FILE'S POINTERS ARE UNRELOCATED. Every one of these tables is
    covered by a RELOC32 entry, so the bytes on disk hold an offset
    from the start of the hunk and the loader adds the segment base -
    docs/formats/hunk.txt. Entry 15 of sector 0's table reads 0xf540
    in the file and 0x002d7774 once loaded. Reading them raw gives
    addresses in no segment at all, which is how this shows up.
    """
    b = img
    seg, o = imgaddr.a2f(addr)
    base = [lo for name, lo, _hi, _f in imgaddr.SEGMENTS
            if name == seg][0]
    return [u32(b, o + i * 4) + base for i in range(n)]


def _is(b, o, *vals):
    for k, v in enumerate(vals):
        if b[o + k] != v:
            return False
    return True


def decode_ops(img, addr, limit=400):
    """one routine as an offset -> operation map, branches kept"""
    o = imgaddr.a2f(addr)[1]
    b = img
    ops, i = {}, 0
    while i < limit:
        op, nx = b[o + i], b[o + i + 1]
        if op == 0x4e and nx == 0x75:
            ops[i] = ('rts', None, None, None)
            i += 2
            continue
        if op in REG and nx == 0x68:
            off = (b[o + i + 2] << 8) | b[o + i + 3]
            ops[i] = ('cmp', (REG[op], off), i + 4, None)
            i += 4
            continue
        if op in (0x6c, 0x6d):
            ops[i] = ('br', op, i + 2, i + 2 + nx)
            i += 2
            continue
        if _is(b, o + i, 0x22, 0x00, 0xd2, 0x82, 0xe2, 0x81):
            ops[i] = ('mid', None, i + 6, None)
            i += 6
            continue
        if _is(b, o + i, 0x24, 0x01):
            ops[i] = ('clip', 'far', i + 2, None)
            i += 2
            continue
        if _is(b, o + i, 0x20, 0x01):
            ops[i] = ('clip', 'near', i + 2, None)
            i += 2
            continue
        if _is(b, o + i, 0x28, 0x10):
            ops[i] = ('child', 0x00, i + 2, None)
            i += 2
            continue
        if _is(b, o + i, 0x28, 0x28):
            ops[i] = ('child', b[o + i + 3], i + 4, None)
            i += 4
            continue
        ops[i] = ('other', None, i + 2, None)
        i += 2
    return ops


def paths(ops, start=0):
    """every straight-line path through the branch structure

    A blt whose target is pc + 4 is the y-slab skip - it hops the
    addq.l #8,A0 - and is not a decision, so it is followed rather
    than forked on.
    """
    out, stack = [], [(start, [], 0)]
    while stack:
        pc, seq, n = stack.pop()
        while pc in ops and n < 400:
            kind, info, nxt, tgt = ops[pc]
            if kind == 'rts':
                break
            if kind == 'child':
                seq = seq + [SLOT.get(info, '?%02x' % info)]
            if kind == 'br' and tgt != pc + 4:
                stack.append((tgt, list(seq), n + 1))
            pc = nxt
            n += 1
        if seq and seq not in out:
            out.append(seq)
    return out


def scan(img, addr, limit=400):
    """what one routine touches, in address order"""
    ops = decode_ops(img, addr, limit)
    out = {'splits': [], 'slots': [], 'clips': [], 'branches': [],
           'size': 0}
    for i in sorted(ops):
        kind, info, nxt, tgt = ops[i]
        if kind == 'cmp':
            out['splits'].append(info)
        elif kind == 'clip':
            out['clips'].append(info)
        elif kind == 'child':
            out['slots'].append(info)
        elif kind == 'br':
            out['branches'].append((i, 'bge' if info == 0x6c else 'blt',
                                    tgt))
        elif kind == 'rts' and not out['size']:
            out['size'] = i + 2
    return out


def show_table(img, addr):
    ptr = entries(img, addr)
    print('table 0x%06x' % addr)
    print('  entry  columns  split planes tested        child slots')
    for k, p in enumerate(ptr):
        if p < 0x200000:
            print('  %5d  (null)' % k)
            continue
        s = scan(img, p)
        occ = ''.join('%d' % ((k >> j) & 1) for j in range(4))
        pl = sorted(set(SPLIT.get(o, '+0x%02x' % o)
                        for r, o in s['splits'] if r != 'row'))
        print('  %5d  %s    %-24s  %s'
              % (k, occ, ','.join(pl) or '-',
                 ' '.join(SLOT.get(x, '?%02x' % x) for x in s['slots'])))


def show_entry(img, addr, k):
    p = entries(img, addr)[k]
    s = scan(img, p)
    print('table 0x%06x entry %d -> 0x%06x, %d bytes'
          % (addr, k, p, s['size']))
    print('  splits : %s' % ', '.join('%s vs %s'
                                      % (r, SPLIT.get(o, hex(o)))
                                      for r, o in s['splits']))
    print('  clips  : %s' % ', '.join(s['clips']))
    print('  branches (a blt +2 is the y-slab skip):')
    for off, kind, tgt in s['branches']:
        note = '   y-slab' if tgt == off + 4 else ''
        print('    %04x  %s -> %04x%s' % (off, kind, tgt, note))
    print('  paths:')
    for seq in paths(decode_ops(img, p)):
        print('      %s' % ' -> '.join(seq))


def show_triples(img):
    print('which split triple each sector tests')
    print('  sector  triples')
    for sec in sorted(TABLES):
        tr = set()
        for t in TABLES[sec][:2]:
            for p in entries(img, t):
                if p >= 0x200000:
                    for _, o in scan(img, p)['splits']:
                        if o in TRIPLE:
                            tr.add(TRIPLE[o])
        print('  %6d  %s' % (sec, ', '.join(sorted(tr)) or 'none'))


def show_order(img):
    """the visit orders of each sector's all-four-columns routine

    THIS IS WHAT DECIDES FRONT-TO-BACK. A ray must meet the nearest
    occupied child first; if a sector's order is wrong, a far voxel
    wins the ray and the object is seen through.
    """
    print('visit order, entry 15 (all four columns) per sector')
    for sec in sorted(TABLES):
        p = entries(img, TABLES[sec][1])[15]
        print('  sector %d at 0x%06x' % (sec, p))
        for seq in paths(decode_ops(img, p)):
            print('      %s' % ' -> '.join(seq))


def main(argv):
    img = imgaddr.data()
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'table' and len(argv) > 2:
        show_table(img, int(argv[2], 16))
    elif cmd == 'entry' and len(argv) > 3:
        show_entry(img, int(argv[2], 16), int(argv[3]))
    elif cmd == 'triples':
        show_triples(img)
    elif cmd == 'order':
        show_order(img)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
