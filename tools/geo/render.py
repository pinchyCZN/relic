#!/usr/bin/env python
"""Flat-shade RELIC's 3D objects to a PNG contact sheet.

    py -3.10 tools/geo/render.py Fspacedata OUT.png
    py -3.10 tools/geo/render.py Fspacedata OUT.png --obj 19 --size 512

OURS, NOT THE DEMO'S. The projection, the light, the shading and the
colours here are this tool's, invented to make the geometry legible.
The demo's own renderer has now been read - see
docs/subsystems/renderer.txt - and this is NOT it: the demo affine
texture maps into a 160x100 chunky buffer, where this flat shades at
whatever size it is asked for. Nothing drawn here is evidence of how a
part looked - CLAUDE.md rule 12. What IS evidence is the shape: the
vertices and the topology come straight out of the file.

PAINTER'S ALGORITHM, NO DEPTH BUFFER. Triangles are sorted back to
front by their farthest vertex and drawn in that order, which is right
for the convex-ish hull pieces and WRONG where two faces interpenetrate.
A per-pixel depth test cost about ten times as much in Python and this
is a shape viewer, so the trade is deliberate.

Each object is auto-fitted to its cell, so the sheet shows shape, not
relative scale. The per-object extents that geom.py prints are the
scale.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geom  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'dat'))
import chunkimg  # noqa: E402

LIGHT = (0.35, 0.55, -0.75)


def to_view(vert):
    """Object components to viewer axes - THE convention, in one place.

    A vertex is three longwords and the demo's axes are not x, y, z in
    the usual order. Scene_TransformProject (0x002d2cd4) divides by the
    SECOND component and adds the screen centres to the first and
    third, so comp1 is FORWARD:

        comp0 -> screen X      comp1 -> DEPTH      comp2 -> screen Y

    The viewer works in a conventional (x, y up, z into the screen)
    frame, so the mapping is (c0, c2, c1).

    GETTING THIS WRONG DOES NOT LOOK LIKE A BUG. With comp2 taken as
    depth, every face of a disc projects with the SAME winding - 0
    positive and 48 negative for Fspacedata object 0 - so backface
    culling either keeps all of them or none, and the object vanishes
    when rotated away. Under the correct axes the same object splits
    24/24, which is what a closed solid of revolution should do.
    """
    return (geom.fx(vert[0]), geom.fx(vert[2]), geom.fx(vert[1]))


def rotate(v, ax, ay):
    x, y, z = v
    ca, sa = math.cos(ay), math.sin(ay)
    x, z = x * ca + z * sa, -x * sa + z * ca
    ca, sa = math.cos(ax), math.sin(ax)
    y, z = y * ca - z * sa, y * sa + z * ca
    return (x, y, z)


def project(obj, w, h, ax, ay, zoom=1.0):
    """Rotate, fit to the window, and return screen points plus the
    rotated points the shading needs."""
    pts = [rotate(to_view(v), ax, ay) for v in obj.verts]
    if not pts:
        return [], []
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    cx = (min(xs) + max(xs)) / 2.0
    cy = (min(ys) + max(ys)) / 2.0
    span = max(max(xs) - min(xs), max(ys) - min(ys), 1e-6)
    s = (min(w, h) * 0.42) / (span * 0.5) * zoom
    scr = [(w / 2.0 + (p[0] - cx) * s, h / 2.0 - (p[1] - cy) * s)
           for p in pts]
    return scr, pts


def fill_tri(fb, w, h, a, b, c, col3):
    """Flat-fill one triangle by scanline, one slice assignment per row.

    This is where the speed is. A per-pixel inner loop in Python costs
    about a microsecond a pixel; a row slice costs that once per ROW.
    """
    if a[1] > b[1]:
        a, b = b, a
    if b[1] > c[1]:
        b, c = c, b
    if a[1] > b[1]:
        a, b = b, a
    ay, by, cy = a[1], b[1], c[1]
    if cy - ay < 1e-9:
        return

    y0 = max(0, int(math.ceil(ay)))
    y1 = min(h - 1, int(math.floor(cy)))
    if y1 < y0:
        return

    dx_ac = (c[0] - a[0]) / (cy - ay)
    dx_ab = (b[0] - a[0]) / (by - ay) if by - ay > 1e-9 else 0.0
    dx_bc = (c[0] - b[0]) / (cy - by) if cy - by > 1e-9 else 0.0

    for y in range(y0, y1 + 1):
        t = y - ay
        xa = a[0] + dx_ac * t
        if y < by:
            xb = a[0] + dx_ab * t
        else:
            xb = b[0] + dx_bc * (y - by)
        if xa > xb:
            xa, xb = xb, xa
        xl = int(math.ceil(xa))
        xr = int(math.floor(xb))
        if xl < 0:
            xl = 0
        if xr > w - 1:
            xr = w - 1
        n = xr - xl + 1
        if n <= 0:
            continue
        o = (y * w + xl) * 3
        fb[o:o + n * 3] = col3 * n


def shade_object(obj, w, h, ax, ay, bg=(16, 16, 22), zoom=1.0):
    """Return a w*h RGB framebuffer as a bytearray.

    zoom is OURS - a viewer convenience. The demo has no zoom.
    """
    fb = bytearray(bytes(bg) * (w * h))
    scr, pts = project(obj, w, h, ax, ay, zoom)
    if not scr:
        return fb

    ln = math.sqrt(sum(c * c for c in LIGHT))
    lx, ly, lz = (c / ln for c in LIGHT)

    order = []
    for f in obj.faces:
        ia, ib, ic = f.corners
        if ia >= len(pts) or ib >= len(pts) or ic >= len(pts):
            continue
        va, vb, vc = pts[ia], pts[ib], pts[ic]
        ux, uy, uz = vb[0] - va[0], vb[1] - va[1], vb[2] - va[2]
        vx, vy, vz = vc[0] - va[0], vc[1] - va[1], vc[2] - va[2]
        nx = uy * vz - uz * vy
        ny = uz * vx - ux * vz
        nz = ux * vy - uy * vx
        nl = math.sqrt(nx * nx + ny * ny + nz * nz)
        if nl < 1e-12:
            continue
        nx, ny, nz = nx / nl, ny / nl, nz / nl
        lum = 0.18 + 0.82 * abs(nx * lx + ny * ly + nz * lz)
        if nz < 0:
            col = (int(120 * lum), int(170 * lum), int(235 * lum))
        else:
            col = (int(150 * lum), int(110 * lum), int(185 * lum))
        depth = max(va[2], vb[2], vc[2])
        order.append((depth, bytes(col), scr[ia], scr[ib], scr[ic]))

    # far first: +z is away from the viewer after project()
    order.sort(key=lambda e: -e[0])
    for _d, col3, pa, pb, pc in order:
        fill_tri(fb, w, h, pa, pb, pc, col3)
    return fb


def fb_rows(fb, w, h):
    return [bytes(fb[y * w * 3:(y + 1) * w * 3]) for y in range(h)]


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    name, out = argv[1], argv[2]
    size = int(argv[argv.index('--size') + 1]) if '--size' in argv else 180
    one = int(argv[argv.index('--obj') + 1]) if '--obj' in argv else None
    ax = math.radians(-24.0)
    ay = math.radians(32.0)

    addr, count = geom.TABLES[name]
    sizes = geom.load_sizes(addr, count)
    blob = open(os.path.join(geom.root(), 'reverse', 'extracted',
                             name + '.bin'), 'rb').read()
    objs, _end, _used = geom.parse_chunk(blob, sizes)

    if one is not None:
        fb = shade_object(objs[one], size, size, ax, ay)
        chunkimg.write_png(out, size, size, fb_rows(fb, size, size))
        print('object %d -> %s (%dx%d)' % (one, out, size, size))
        return 0

    cols = 7
    rows = (len(objs) + cols - 1) // cols
    W, H = cols * size, rows * size
    sheet = bytearray(bytes((16, 16, 22)) * (W * H))
    for k, o in enumerate(objs):
        fb = shade_object(o, size, size, ax, ay)
        ox, oy = (k % cols) * size, (k // cols) * size
        for y in range(size):
            src = y * size * 3
            dst = ((oy + y) * W + ox) * 3
            sheet[dst:dst + size * 3] = fb[src:src + size * 3]
    chunkimg.write_png(out, W, H, fb_rows(sheet, W, H))
    print('%d objects -> %s (%dx%d, %d per row)' % (len(objs), out, W, H, cols))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
