#!/usr/bin/env python
"""Parse RELIC's 3D object format out of a split asset chunk.

    py -3.10 tools/geo/geom.py Fspacedata            list the objects
    py -3.10 tools/geo/geom.py Fspacedata --obj 19   one object in detail

THE FORMAT, read out of the sub-block size table in the executable and
checked against every object in Fspacedata [verified]:

A chunk that holds geometry is split by Dat_LoadSplit (0x0021fa94) into
sub-blocks of three per object, in this order:

    faces      Face[nfaces]          14 bytes each
    nfaces_m1  u16                   its own 2-byte sub-block
    verts      u32 nverts_m1; Vertex[nverts]

    Face   = u16 nv_m1               always 2, so every face is a triangle
             u16 edge[3][2]          (a,b) (b,c) (c,a) - a closed ring
    Vertex = s32 x, y, z             16.16 fixed point, big-endian

Both counts are stored MINUS ONE because the code loops with dbf, which
runs count+1 times. nfaces = nfaces_m1 + 1 and nverts = nverts_m1 + 1.

The edge ring is redundant - the three corners are edge[0][0],
edge[1][0], edge[2][0] - but it is what the renderer walks, so the
parser reads it as stored and checks the ring closes.

NO TEXTURE COORDINATES AND NO MATERIAL INDEX ARE PRESENT. A 14-byte
face carries nothing but topology, so whatever colours these objects
comes from somewhere else - see docs/formats/geometry.txt.
"""

import os
import struct
import sys

FACE_SIZE = 14
VERT_SIZE = 12


class Face(object):
    __slots__ = ('nv_m1', 'edges', 'ring_ok')

    def __init__(self, nv_m1, edges):
        self.nv_m1 = nv_m1
        self.edges = edges
        self.ring_ok = (edges[0][1] == edges[1][0] and
                        edges[1][1] == edges[2][0] and
                        edges[2][1] == edges[0][0])

    @property
    def corners(self):
        return (self.edges[0][0], self.edges[1][0], self.edges[2][0])


class Obj(object):
    __slots__ = ('index', 'offset', 'faces', 'verts')

    def __init__(self, index, offset, faces, verts):
        self.index = index
        self.offset = offset
        self.faces = faces
        self.verts = verts

    def bounds(self):
        xs = [v[0] for v in self.verts]
        ys = [v[1] for v in self.verts]
        zs = [v[2] for v in self.verts]
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def fx(v):
    """16.16 fixed point to float."""
    return v / 65536.0


def parse_object(blob, offset, a_size, b_size, c_size, index):
    """One (faces, count, verts) triple at offset. Raises on a mismatch."""
    a = blob[offset:offset + a_size]
    b = blob[offset + a_size:offset + a_size + b_size]
    c = blob[offset + a_size + b_size:offset + a_size + b_size + c_size]

    if b_size != 2:
        raise ValueError('object %d: count sub-block is %d bytes, expected 2'
                         % (index, b_size))
    nfaces = struct.unpack('>H', b)[0] + 1
    nverts = struct.unpack_from('>I', c, 0)[0] + 1

    if a_size != nfaces * FACE_SIZE:
        raise ValueError('object %d: face block is %d bytes, expected %d '
                         'for %d faces' % (index, a_size,
                                           nfaces * FACE_SIZE, nfaces))
    if c_size != 4 + nverts * VERT_SIZE:
        raise ValueError('object %d: vertex block is %d bytes, expected %d '
                         'for %d verts' % (index, c_size,
                                           4 + nverts * VERT_SIZE, nverts))

    faces = []
    for i in range(nfaces):
        w = struct.unpack_from('>7H', a, i * FACE_SIZE)
        faces.append(Face(w[0], ((w[1], w[2]), (w[3], w[4]), (w[5], w[6]))))

    verts = []
    for i in range(nverts):
        verts.append(struct.unpack_from('>3i', c, 4 + i * VERT_SIZE))

    return Obj(index, offset, faces, verts)


def parse_chunk(blob, sizes):
    """Walk a size table in (faces, count, verts) triples.

    Stops at the first triple that does not parse, so a table whose tail
    is something other than geometry yields the objects up to it and the
    offset where it stopped.
    """
    objs = []
    off = 0
    i = 0
    while i + 2 < len(sizes):
        a, b, c = sizes[i], sizes[i + 1], sizes[i + 2]
        if off + a + b + c > len(blob):
            break
        try:
            objs.append(parse_object(blob, off, a, b, c, len(objs)))
        except (ValueError, struct.error):
            break
        off += a + b + c
        i += 3
    return objs, off, i


def root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def load_sizes(addr, count):
    """Read `count` big-endian longwords from an image address."""
    sys.path.insert(0, os.path.join(root(), 'tools', 'hunk'))
    import imgaddr
    d = imgaddr.data()
    _name, o = imgaddr.a2f(addr)
    return [struct.unpack_from('>I', d, o + i * 4)[0] for i in range(count)]


# chunk -> (size-table address, entry count) read from the loader call
# sites in the executable [verified]
TABLES = {
    'Fspacedata': (0x002e483c, 67),
}


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    name = argv[1]
    if name not in TABLES:
        sys.exit('no size table recorded for %s; known: %s'
                 % (name, ', '.join(sorted(TABLES))))
    addr, count = TABLES[name]
    sizes = load_sizes(addr, count)
    blob = open(os.path.join(root(), 'reverse', 'extracted',
                             name + '.bin'), 'rb').read()
    objs, end, used = parse_chunk(blob, sizes)

    want = argv.index('--obj') if '--obj' in argv else -1
    if want >= 0:
        k = int(argv[want + 1])
        o = objs[k]
        lo, hi = o.bounds()
        print('object %d  @0x%05x  %d faces  %d verts' % (k, o.offset,
                                                          len(o.faces),
                                                          len(o.verts)))
        print('bounds  x %+.4f .. %+.4f' % (fx(lo[0]), fx(hi[0])))
        print('        y %+.4f .. %+.4f' % (fx(lo[1]), fx(hi[1])))
        print('        z %+.4f .. %+.4f' % (fx(lo[2]), fx(hi[2])))
        print('')
        for i, v in enumerate(o.verts[:12]):
            print('  v%-3d %+10.5f %+10.5f %+10.5f' % (i, fx(v[0]), fx(v[1]),
                                                       fx(v[2])))
        if len(o.verts) > 12:
            print('  ... %d more' % (len(o.verts) - 12))
        print('')
        for i, f in enumerate(o.faces[:12]):
            print('  f%-3d %-12s ring=%s' % (i, f.corners, f.ring_ok))
        if len(o.faces) > 12:
            print('  ... %d more' % (len(o.faces) - 12))
        return 0

    print('%s: %d objects from %d of %d size entries, %d bytes consumed '
          'of %d' % (name, len(objs), used, len(sizes), end, len(blob)))
    print('')
    print('%-4s %-8s %6s %6s  %-26s %s' % ('obj', 'off', 'faces', 'verts',
                                           'extent (x, y, z)', 'tris'))
    for o in objs:
        lo, hi = o.bounds()
        ext = (fx(hi[0] - lo[0]), fx(hi[1] - lo[1]), fx(hi[2] - lo[2]))
        tris = sum(1 for f in o.faces if f.nv_m1 == 2)
        print('%-4d 0x%05x %6d %6d  %7.3f %7.3f %7.3f   %d/%d'
              % (o.index, o.offset, len(o.faces), len(o.verts),
                 ext[0], ext[1], ext[2], tris, len(o.faces)))
    if used < len(sizes):
        print('')
        print('size entries %d.. are not geometry: %s'
              % (used, sizes[used:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
