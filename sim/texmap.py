#!/usr/bin/env python
"""RELIC's texture-coordinate generation, transcribed.

RELIC stores NO texture coordinates. It computes them at draw time by
projecting each face's object-space vertex positions - see
docs/formats/geometry.txt section 4. This module is that computation,
transcribed routine for routine, so a reconstruction uses the demo's
own mapping rather than a guess.

ALL SEVEN PROJECTIONS ARE HERE. Uv_GenerateFaceParams tests seven
independent bits and runs every one that is set, so more than one can
write into the same face's slot; generate_face_params below does the
same, in the same order.

AXIS CONVENTION - GET THIS RIGHT OR EVERY PROJECTION IS WRONG.
A vertex is three longwords and the demo's axes are NOT x, y, z in the
usual arrangement. Scene_TransformProject (0x002d2cd4) divides by the
SECOND component and adds the screen centre to the first and third:

    comp0  ->  screen X        (+0x50 = 80, half of 160)
    comp1  ->  DEPTH           (the divisor, +0x1f4 camera distance)
    comp2  ->  screen Y        (+0x32 = 50, half of 100)

So comp1 is the forward axis. The routines below are named for the
components they read, not for x/y/z, because any x/y/z name here would
be a lie in one convention or the other.

THE SIN TABLE IS READ OUT OF THE BINARY, not generated from math.sin.
It is the demo's own data and rule 12 says use it. It happens to be a
clean sine, but that is a measurement, not a licence. The same goes for
the 256x256 atan table.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'hunk'))

SIN_BASE = 0x00228b6c      # [verified] maybe_Math_Rotate2D
COS_BASE = 0x0022ab6c      # the same table, 2048 entries on - a quarter turn
ATAN_BASE = 0x002af97a     # [verified] Uv_AtanLookup: 0x29f97a + 0x10000
ANGLE_MASK = 0x1fff        # 8192 steps to the turn
TABLE_ONE = 16384          # the sin/cos tables are 14-bit fixed point

# mode bits, as Uv_GenerateFaceParams tests them [verified]
MODE_ROTATED       = 0x0001   # Uv_ProjRotatedXY
MODE_CYL_C2        = 0x0002   # Uv_ProjAngularZ
MODE_CYL_C1        = 0x0004   # Uv_ProjAngularY
MODE_PLANAR_C1C2   = 0x0008   # Uv_ProjPlanarYZ
MODE_PLANAR_C1C0   = 0x0010   # Uv_ProjPlanarYX
MODE_PLANAR_C0C2   = 0x0020   # Uv_ProjPlanarXZ
MODE_ROTATED_RANGE = 0x0040   # Uv_ProjRotatedRanged
MODE_TEXTURE_ONLY  = 0x4000   # Uv_SetTextureOnly
MODE_ATAN_ALT      = 0x8000   # changes how Uv_AtanLookup scales

_SINCOS = None
_ATAN = None


def _sincos():
    global _SINCOS
    if _SINCOS is None:
        import imgaddr
        d = imgaddr.data()
        _n, off = imgaddr.a2f(SIN_BASE)
        need = (COS_BASE - SIN_BASE) // 4 + ANGLE_MASK + 1
        _SINCOS = struct.unpack_from('>%di' % need, d, off)
    return _SINCOS


def _atan():
    global _ATAN
    if _ATAN is None:
        import imgaddr
        d = imgaddr.data()
        _n, off = imgaddr.a2f(ATAN_BASE)
        _ATAN = struct.unpack_from('>65536H', d, off)
    return _ATAN


def s32(x):
    """68k registers are 32 bits and muls.l keeps only the low half."""
    x &= 0xffffffff
    return x - 0x100000000 if x & 0x80000000 else x


def sdiv(a, b):
    """divs truncates toward zero; Python's // floors. Not the same."""
    if b == 0:
        return 0
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


class Params(object):
    """The globals Uv_GenerateFaceParams loads from its arguments.

    name here          global          set from
    -----------------  --------------  ------------------------------
    mode               g_UvModeBits    D0
    scale              g_UvScale       D2
    range_min          g_UvRangeMin    D3
    range_max          g_UvRangeMax    D4
    centre_u           g_UvCentreU     D5
    centre_v           g_UvCentreV     D6
    angle              g_UvAngle       D7
    plane_scale_u/v    0x2e0c3c/0c40   NOT set by the generator - the
                                       caller writes them separately
    """

    def __init__(self, mode=0, scale=0, range_min=0, range_max=0,
                 centre_u=0, centre_v=0, angle=0,
                 plane_scale_u=0, plane_scale_v=0):
        self.mode = mode
        self.scale = scale
        self.range_min = range_min
        self.range_max = range_max
        self.centre_u = centre_u
        self.centre_v = centre_v
        self.angle = angle
        self.plane_scale_u = plane_scale_u
        self.plane_scale_v = plane_scale_v


def math_rotate2d(d0, d1, d7):
    """ORIGIN 0x002d4354  maybe_Math_Rotate2D

        andi.l  #0x1fff,D7
        D6 = cos[D7] ; D7 = sin[D7]
        D0 = (D0*D6 + D1*D7) >> 14
        D1 = (D1*D6 - D0_in*D7) >> 14

    8192 steps to a full turn. The shift is written asr #8 then asr #6
    in the original, which is why the tables are 14-bit not 16.
    """
    t = _sincos()
    i = d7 & ANGLE_MASK
    cos = t[(COS_BASE - SIN_BASE) // 4 + i]
    sin = t[i]
    return (s32(s32(d0 * cos) + s32(d1 * sin)) >> 14,
            s32(s32(d1 * cos) - s32(d0 * sin)) >> 14)


def uv_atan_lookup(d1, d2, mode):
    """ORIGIN 0x002cfdb0  Uv_AtanLookup

        lea     g_AtanTable,A6
        lsl.l   #8,D1
        move.b  D2b,D1b            ; index = (D1 & 0xff) << 8 | (D2 & 0xff)
        move.w  (0,A6,D1w*2),D0w
        lsr.w   #8,D0w             ; take the high byte

    then, with mode bit 0x8000 CLEAR:
        D0 = (D0 * 8) & 0xff
    and either way:
        if D0 >= 0x80:  D0 = 0xff - D0      ; fold the top half back

    Only the low WORD of the shifted D1 reaches the index, so the
    table is 256 x 256 even though the shift looks wider.

    The original also has a `tst.l D0 / moveq #0,D3` tail that would
    zero the caller's depth value when D0 is negative. After the fold
    D0 is 0..127, so on the default path that never fires. It is
    transcribed anyway, as a flag, because the alternative path tests
    D3 instead and there it does fire.
    """
    tab = _atan()
    idx = ((d1 & 0xff) << 8) | (d2 & 0xff)
    d0 = (tab[idx] >> 8) & 0xff
    if not (mode & MODE_ATAN_ALT):
        d0 = (d0 * 8) & 0xff
    if d0 >= 0x80:
        d0 = 0xff - d0
    return d0


def _avg3(verts, face, comp):
    """The three-vertex average the ranged projections cull on.

    divsl.l #3 truncates toward zero, so this is sdiv not //.
    """
    t = 0
    for idx in face.corners:
        t = s32(t + verts[idx][comp])
    return sdiv(t, 3)


def _in_range(v, p):
    return not (v < p.range_min or v > p.range_max)


# ---------------------------------------------------------------------
# the seven projections
# ---------------------------------------------------------------------

def uv_proj_rotated(verts, face, p):
    """ORIGIN 0x002cfbcc  Uv_ProjRotatedXY - mode bit 0x01.

    Planar onto the comp0/comp1 plane, rotated by g_UvAngle. No range
    test, so every face is written.

        D0 = comp1 ; D1 = comp0          ; comp1 FIRST
        maybe_Math_Rotate2D(D0, D1, angle)
        u = ((D0 * scale) >> 16) + centre_u
        v = ((D1 * scale) >> 16) + centre_v

    THE CENTRE IS ADDED AFTER SCALING here. The three planar modes
    below SUBTRACT their centre BEFORE scaling. That asymmetry is in
    the original and it is the easiest thing in this file to get
    backwards.
    """
    out = []
    for idx in face.corners:
        c0, c1, _c2 = verts[idx]
        d0, d1 = math_rotate2d(c1, c0, p.angle)
        out.append((s32(d0 * p.scale) >> 16) + p.centre_u)
        out.append((s32(d1 * p.scale) >> 16) + p.centre_v)
    return [(out[0], out[1]), (out[2], out[3]), (out[4], out[5])]


def uv_proj_rotated_ranged(verts, face, p):
    """ORIGIN 0x002cfc9a  Uv_ProjRotatedRanged - mode bit 0x40.

    Identical arithmetic to uv_proj_rotated, with a cull: the face is
    dropped unless the AVERAGE of comp2 over its three vertices lies
    in [range_min, range_max].
    """
    if not _in_range(_avg3(verts, face, 2), p):
        return None
    return uv_proj_rotated(verts, face, p)


def uv_proj_cyl_c2(verts, face, p):
    """ORIGIN 0x002cf73e  Uv_ProjAngularZ - mode bit 0x02.

    A cylinder about the comp2 axis. Culled on the average of comp2.

        span = range_max - range_min
        per vertex:
            D1 = comp1 ; D2 = comp0 ; D3 = comp2
            D3 = -(D3 - range_max)          ; distance below the top
            D1 >>= 8 ; D2 >>= 8
            D3 = (D3 * scale) / span
            u = Uv_AtanLookup(D1, D2)
            v = D3
    """
    if not _in_range(_avg3(verts, face, 2), p):
        return None
    span = s32(p.range_max - p.range_min)
    out = []
    for idx in face.corners:
        c0, c1, c2 = verts[idx]
        d3 = s32(-s32(c2 - p.range_max))
        d1 = c1 >> 8
        d2 = c0 >> 8
        d3 = sdiv(s32(d3 * p.scale), span)
        out.append((uv_atan_lookup(d1, d2, p.mode), d3))
    return out


def uv_proj_cyl_c1(verts, face, p):
    """ORIGIN 0x002cf832  Uv_ProjAngularY - mode bit 0x04.

    A cylinder about the comp1 axis, and the only projection whose u
    and v come out in the opposite order:

        per vertex:
            D1 = comp0 ; D2 = comp2 ; D3 = comp1
            D3 = -(D3 - range_max)
            D1 >>= 8 ; D2 >>= 8
            D3 = (D3 * scale) / span
            D0 = Uv_AtanLookup(D1, D2) >> 1     ; HALVED
            u = D3                               ; depth first
            v = D0

    TWO ODDITIES, BOTH TRANSCRIBED AS WRITTEN. The angle is halved,
    which wraps the texture twice around instead of once. And the
    cull averages comp0 while the depth comes from comp1 - every
    other ranged projection culls on the axis it measures. Whether
    that is a cheap extent test or a slip in the original cannot be
    told from the code.
    """
    if not _in_range(_avg3(verts, face, 0), p):
        return None
    span = s32(p.range_max - p.range_min)
    out = []
    for idx in face.corners:
        c0, c1, c2 = verts[idx]
        d3 = s32(-s32(c1 - p.range_max))
        d1 = c0 >> 8
        d2 = c2 >> 8
        d3 = sdiv(s32(d3 * p.scale), span)
        d0 = uv_atan_lookup(d1, d2, p.mode) >> 1
        out.append((d3, d0))
    return out


def _planar(verts, face, p, ca, cb):
    """The shared body of the three planar projections.

        D0 = comp[ca] >> 8 ; D1 = comp[cb] >> 8
        D0 -= centre_u     ; D1 -= centre_v          ; SUBTRACT first
        D0 = (D0 * plane_scale_u) >> 8
        D1 = (D1 * plane_scale_v) >> 8
    """
    out = []
    for idx in face.corners:
        v = verts[idx]
        d0 = (v[ca] >> 8) - p.centre_u
        d1 = (v[cb] >> 8) - p.centre_v
        d0 = s32(d0 * p.plane_scale_u) >> 8
        d1 = s32(d1 * p.plane_scale_v) >> 8
        out.append((d0, d1))
    return out


def _spread_ok(verts, face, comp):
    """ORIGIN 0x002cf950 and 0x002cfa44 - the extent test.

        D3 = |comp[v1] - comp[v0]| + |comp[v2] - comp[v0]|
        if D3 < 0x3e8: reject

    A cheap stand-in for face area along one axis: a face too flat in
    that component is dropped rather than smeared across the texture.
    """
    ia, ib, ic = face.corners
    base = verts[ia][comp]
    d3 = abs(s32(verts[ib][comp] - base)) + abs(s32(verts[ic][comp] - base))
    return d3 >= 0x3e8


def uv_proj_planar_c1c2(verts, face, p):
    """ORIGIN 0x002cf92c  Uv_ProjPlanarYZ - mode bit 0x08.

    Planar from comp1 and comp2, culled by the comp1 spread test.
    """
    if not _spread_ok(verts, face, 1):
        return None
    return _planar(verts, face, p, 1, 2)


def uv_proj_planar_c0c2(verts, face, p):
    """ORIGIN 0x002cfa20  Uv_ProjPlanarXZ - mode bit 0x20.

    Planar from comp0 and comp2, culled by the comp0 spread test.
    """
    if not _spread_ok(verts, face, 0):
        return None
    return _planar(verts, face, p, 0, 2)


def uv_proj_planar_c1c0(verts, face, p):
    """ORIGIN 0x002cfb14  Uv_ProjPlanarYX - mode bit 0x10.

    Planar from comp1 and comp0. THE ONLY PLANAR MODE WITH NO CULL -
    it has neither a range test nor a spread test, so every face is
    written.
    """
    return _planar(verts, face, p, 1, 0)


# The articulated strut, from Mesh_DeformAndMap (0x002cf59c) and the
# two routines it drives, maybe_Mesh_Deform (0x002d007e) and the
# per-vertex rotate at 0x002d00bc. Each pair is (anchor vertex, raw
# angle); everything downstream of the anchor turns about it.
# ORIGIN 0x002cf5a6 - EVERY STRUT BENDS OBJECT 13's VERTICES.
# Space_BuildObjects points all six slots' rest pointer (+0x8) at the
# one block at 0x002e47d4, which is object 13's. Fspacedata does hold
# six distinct strut meshes, already placed around the hub, but the
# demo reads only this one and rotates it into place six times.
#
# The other five are not dead: Mesh_DeformAndMap points the working
# pointer at the object's OWN block (+0x38) across Obj_SetCentroid,
# so each instance takes its draw-order centroid from its own
# pre-placed mesh and its bent geometry from object 13's.
#
# Bending each object's own vertices AND THEN applying the placement
# angle places them twice - they come out at unrelated angles, which
# reads as six struts in six different poses.
STRUT_REST_OBJ = 13

STRUT_ANCHORS = (0, 4, 8)
STRUT_SETUP_ANGLES = (0x0700, 0x1c80, 0x1a80)
STRUT_VERTS = 16          # Mesh_BendChain stops at vertex 15

# ORIGIN 0x002d643c  Strut_AnimateClaw - the two-phase claw script.
STRUT_CLOSE_RATE = (2, 4, 2)    # subi.l per joint per frame
STRUT_CLOSE_LIMIT = -0x1ee      # cmpi.l #-0x1ee on joint 0
STRUT_OPEN_LIMIT = 0x200        # cmpi.l #0x200 on joint 0
STRUT_OPEN_ACCEL = 2            # each joint's velocity gains this per frame


def rotate_about(v, anchor, raw_angle):
    """ORIGIN 0x002d00bc - turn one vertex about an anchor.

        D0 -= anchor0 ; D1 -= anchor1 ; D2 -= anchor2
        D7 = (g_DeformAngle & 0x1fff) ^ 0x1fff
        D0' = (D0*cos + D2*sin) >> 14
        D2' = (D2*cos - D0*sin) >> 14
        add the anchor back

    THE ROTATION IS IN THE comp0/comp2 PLANE and comp1 comes out
    unchanged - it is subtracted and added back untouched.

    THE ANGLE IS MIRRORED, not used directly: `eori.l #0x1fff` turns
    g into 8191-g, so a raw 0x700 is a turn of 6399 steps, about
    -78.8 degrees, not +31.5. Reading the immediate as the angle gives
    a strut bent the wrong way.
    """
    t = _sincos()
    i = (raw_angle & ANGLE_MASK) ^ ANGLE_MASK
    cos = t[(COS_BASE - SIN_BASE) // 4 + i]
    sin = t[i]
    x = s32(v[0] - anchor[0])
    z = s32(v[2] - anchor[2])
    nx = s32(s32(x * cos) + s32(z * sin)) >> 14
    nz = s32(s32(z * cos) - s32(x * sin)) >> 14
    return (s32(nx + anchor[0]), v[1], s32(nz + anchor[2]))


def strut_anim_angles(frame, triggered=True):
    """ORIGIN 0x002d643c  Strut_AnimateClaw - run the script `frame` times.

    The demo does not hold one strut pose. This runs per frame and
    rewrites the three joint angles Mesh_AnimateStruts then applies,
    in two phases latched by the flag at 0x002d64e6:

        CLOSE   while joint 0 > -0x1ee:
                    joints -= (2, 4, 2)         ; linear, 247 frames
        (hold)  at the limit, wait for the caller to pass D0 != 0
        OPEN    while joint 0 < 0x200:
                    joint[i] += v[i] ; v[i] += 2

    THE OPEN PHASE ACCELERATES. Each joint carries its own velocity
    at 0x002d64e8/ec/f0, zeroed on the latch and gaining 2 every
    frame, so the angle moves as a quadratic - it eases in and snaps.
    All three velocities are identical, so the open phase adds the
    SAME delta to all three joints and takes about 33 frames.

    ANGLES OF ZERO ARE THE STRAIGHT POSE, and Strut_ResetClaw
    (0x002cca5a) writes exactly that before the sequence runs. So the
    struts start straight, draw in to a spike, and then open.
    """
    a = [0, 0, 0]
    v = [0, 0, 0]
    opening = False
    for _ in range(max(0, int(frame))):
        if not opening:
            if a[0] > STRUT_CLOSE_LIMIT:
                for i in range(3):
                    a[i] -= STRUT_CLOSE_RATE[i]
                continue
            if not triggered:
                continue
            opening = True             # the latch, then fall through
            v = [0, 0, 0]
        if a[0] >= STRUT_OPEN_LIMIT:
            continue
        for i in range(3):
            a[i] += v[i]
            v[i] += STRUT_OPEN_ACCEL
    return tuple(a)


def strut_anim_frames():
    """How many frames the script takes to reach its final hold."""
    n = 0
    last = None
    while n < 4000:
        cur = strut_anim_angles(n)
        if cur == last and cur[0] >= STRUT_OPEN_LIMIT:
            return n
        last = cur
        n += 1
    return n


def mesh_deform_strut(verts, place_angle, angles=None):
    """ORIGIN 0x002cf59c Mesh_DeformAndMap / 0x002cffea Mesh_AnimateStruts.

    Three passes of Mesh_BendChain bend a 16-vertex chain at vertices
    0, 4 and 8, each pass turning EVERYTHING DOWNSTREAM of its anchor,
    so the bends compound into an articulated arm. Then the whole
    strut is turned about the hub by the caller's angle, in the
    comp0/comp1 plane.

    TWO ROUTINES DRIVE THIS AND THEY DISAGREE ON THE ANGLES.
    Mesh_DeformAndMap, at setup, uses the hardcoded STRUT_SETUP_ANGLES.
    Mesh_AnimateStruts re-reads them from 0x002e0ba6/aa/ae every frame
    and ALWAYS RE-COPIES THE REST POSE FIRST, so it overwrites the
    setup pose rather than compounding with it - pass `angles` to get
    what is actually on screen once the animation runs.

    THE UVs DO NOT COME FROM THIS. Mesh_DeformAndMap points the
    object's vertex pointer at the REST pose before it calls
    Uv_GenerateFaceParams and restores it afterwards, so the texture
    coordinates are computed from the undeformed shape. Only the
    geometry bends - and Mesh_AnimateStruts never touches UVs at all.
    """
    if angles is None:
        angles = STRUT_SETUP_ANGLES
    v = [tuple(x) for x in verts]
    top = min(STRUT_VERTS, len(v))
    for start, raw in zip(STRUT_ANCHORS, angles):
        if start >= top:
            continue
        a = v[start]
        for i in range(start + 1, top):
            v[i] = rotate_about(v[i], a, raw)
    out = []
    for x in v:
        d0, d1 = math_rotate2d(x[0], x[1], place_angle)
        out.append((d0, d1, x[2]))
    return out


# ORIGIN 0x002d12ac  Spike_BuildRing - object 18, seven more times.
# Each entry is the (angle, radius) pair handed to
# Mesh_InstanceAtAngle. With the master at angle 0 that makes EIGHT
# spikes at exactly 45 degrees apart, and the four diagonals carry a
# 1.25 radius factor the axis-aligned ones do not.
SPIKE_RING = [(0x0800, 0x100), (0x1000, 0x100), (0x1800, 0x100),
              (0x0400, 0x140), (0x0c00, 0x140), (0x1400, 0x140),
              (0x1c00, 0x140)]
SPIKE_MASTER = 18


def mesh_instance_at_angle(verts, angle, radius):
    """ORIGIN 0x002d146e  Mesh_InstanceAtAngle - a rotated copy.

        per vertex:
            d0, d1 = rotate2d(comp0, comp1, angle)
            if d6w == 1:                    ; ONE vertex only
                d0 = d0*radius >> 8
                d1 = d1*radius >> 8

    THE RADIUS SCALES EXACTLY ONE VERTEX. The test is on the dbf
    counter, not on the vertex index, so it fires on the pass where
    the counter happens to be 1 - the second to last vertex. Scaling
    every vertex makes the whole spike bigger instead of longer, and
    with 0x140 on a four-vertex mesh that is a visibly different
    shape.

    comp2 is copied through untouched, so the ring is flat.
    """
    out = []
    n = len(verts)
    for i, v in enumerate(verts):
        d0, d1 = math_rotate2d(v[0], v[1], angle)
        if (n - 1 - i) == 1:
            d0 = s32(s32(d0 * radius) >> 8)
            d1 = s32(s32(d1 * radius) >> 8)
        out.append((d0, d1, v[2]))
    return out


def uv_proj_boxish(verts, face, p):
    """ORIGIN 0x002d1690 - object 19's own projection.

    NOT part of the Uv_GenerateFaceParams bit family. Space_BuildObjects calls
    it directly for one object, and it writes the face mode word to 0
    and the texture pointer itself.

        per corner:  a = comp0 ;  b = comp2 + comp1
        if b is the SAME for all three corners:
            fall back to  a = comp2 ; b = comp1
        every value >>= 12            ; written asr #8 then asr #4
        the three a values += 0x40
        all values &= 0xff
        if a value >= 0x80:  value = 0xff - value     ; fold

    The fallback is a degeneracy guard: a face lying in the plane
    comp1+comp2 = k projects to a line under the first mapping, so it
    is remapped onto comp2/comp1 instead.
    """
    a = []
    b = []
    for idx in face.corners:
        c0, c1, c2 = verts[idx]
        a.append(c0)
        b.append(s32(c2 + c1))
    if b[1] == b[0] and b[2] == b[0]:
        a = []
        b = []
        for idx in face.corners:
            _c0, c1, c2 = verts[idx]
            a.append(c2)
            b.append(c1)
    out = []
    for k in range(3):
        u = ((a[k] >> 8) >> 4) + 0x40
        v = (b[k] >> 8) >> 4
        u &= 0xff
        v &= 0xff
        if u >= 0x80:
            u = 0xff - u
        if v >= 0x80:
            v = 0xff - v
        out.append((u, v))
    return out


# dispatch order is the order Uv_GenerateFaceParams tests the bits,
# which is NOT numeric order [verified]
_ORDER = [
    (MODE_ROTATED,       uv_proj_rotated),
    (MODE_CYL_C2,        uv_proj_cyl_c2),
    (MODE_PLANAR_C1C2,   uv_proj_planar_c1c2),
    (MODE_CYL_C1,        uv_proj_cyl_c1),
    (MODE_PLANAR_C1C0,   uv_proj_planar_c1c0),
    (MODE_ROTATED_RANGE, uv_proj_rotated_ranged),
    (MODE_PLANAR_C0C2,   uv_proj_planar_c0c2),
]


def generate_part_uvs(verts, faces, part):
    """UVs for one SHIP_PARTS row, whichever generator it uses.

    A PART CAN TAKE MORE THAN ONE PASS, and the passes are not
    alternatives - they LAYER. Space_BuildObjects calls
    Uv_GenerateFaceParams twice for objects 3 and 4: a mode-1 pass
    over every face, then a cylinder pass that only writes the faces
    whose centroid falls inside its range. A face the second pass
    rejects keeps what the first pass gave it, because the reject
    path advances the write pointer by 28 bytes without writing.

    KEEPING ONLY THE LAST PASS LEAVES 48 OF OBJECT 3's 80 FACES AND 8
    OF OBJECT 4's 24 WITH NO COORDINATES - the flat untextured disc
    inside the textured rim.
    """
    mode = part['mode']
    if mode == 'boxish':
        return [uv_proj_boxish(verts, f, None) for f in faces]
    if mode == 'flat':
        return [None] * len(faces)
    out = [None] * len(faces)
    for ps in part.get('passes') or [part]:
        got = generate_face_params(verts, faces, part_params(ps))
        for i, uv in enumerate(got):
            if uv is not None:
                out[i] = uv
    return out


def generate_face_params(verts, faces, p):
    """ORIGIN 0x002cf658  Uv_GenerateFaceParams

    Returns one entry per face: a list of three (u, v) pairs, or None
    where every selected projection rejected the face.

    THE BITS ARE INDEPENDENT TESTS, NOT A SWITCH. If two are set, both
    run and both write into the same 28-byte slot, so the last one to
    write wins. This mirrors that rather than tidying it up.

    A rejected projection advances the write pointer by 28 without
    writing, which in the original leaves the previous frame's values
    in place. Here that shows up as None so a caller can decide.
    """
    if p.mode == MODE_TEXTURE_ONLY:
        return [[(0, 0), (0, 0), (0, 0)] for _ in faces]
    out = []
    for f in faces:
        uv = None
        for bit, fn in _ORDER:
            if p.mode & bit:
                got = fn(verts, f, p)
                if got is not None:
                    uv = got
        out.append(uv)
    return out


# THE SHIP IS NINE TEXTURED OBJECTS, not two [verified].
#
# Space_BuildObjects (0x002d0990) fills 21 scene slots with Fspacedata
# objects and calls Uv_GenerateFaceParams eleven times. Objects 3 and 4
# are called TWICE and the second call overwrites the first, so nine
# distinct objects end up with usable parameters. Every field below was
# read off the setup code.
#
#   slot  obj  tex    mode  scale   cu   cv   angle    rmin    rmax
#      0    0    1    0001     94  128  128       0       -       -
#      1    2    0    0001     94  128  128    -700       -       -
#      2    1    3    8002    126  128  128    -700  -29062    8192
#      3    3    2    0002    128  192   64       0    8960   13568
#      4    4    2    8002    128  192   64       0   17230   29453
#      5    8    0    4000    128  192   64       0       -       -
#      6    9    0    4000    128  192   64       0       -       -
#      7   10    0    4000    128  192   64       0       -       -
#     15   18    2    0001    100  192   64       0       -       -
#
# TEN MORE OBJECTS GET A FACE ARRAY AND NO UV CALL - 5, 6, 7, 11..17,
# 19, 20 in slots 8-14, 22-24 and 32-33. Where their parameters come
# from has not been found, so they are left out rather than guessed.
#
# ALL PARTS USE CULL MODE 0. Space_BuildObjects never writes the draw
# context's +0x4c and BSS_04 starts zeroed.
#
# NO POSITIONS ARE APPLIED. The only placement in the setup is
# subi.l #0x186a0 and #0xc350 on slot 0 and slot 1's world z, which
# moves those scene objects through the world rather than offsetting
# the parts against each other; the vertices already carry the
# assembly.
SHIP_PARTS = [
    {'obj': 0,  'tex': 1, 'mode': 0x0001, 'scale': 94,  'cu': 128, 'cv': 128,
     'angle': 0,    'rmin': 0,       'rmax': 0},
    {'obj': 2,  'tex': 0, 'mode': 0x0001, 'scale': 94,  'cu': 128, 'cv': 128,
     'angle': -700, 'rmin': 0,       'rmax': 0},
    {'obj': 1,  'tex': 3, 'mode': 0x8002, 'scale': 126, 'cu': 128, 'cv': 128,
     'angle': -700, 'rmin': -29062,  'rmax': 8192},
    # TWO PASSES EACH, and the second does not replace the first -
    # it only rewrites the faces inside its range. Call 4 then 5 of
    # Space_BuildObjects for object 3, call 6 then 7 for object 4.
    {'obj': 3,  'tex': 2, 'mode': 0x0002, 'scale': 128, 'cu': 192, 'cv': 64,
     'angle': 0,    'rmin': 8960,    'rmax': 13568,
     'passes': [
         {'mode': 0x0001, 'scale': 145, 'cu': 192, 'cv': 64,
          'angle': 0, 'rmin': 0, 'rmax': 0},
         {'mode': 0x0002, 'scale': 128, 'cu': 192, 'cv': 64,
          'angle': 0, 'rmin': 8960, 'rmax': 13568}]},
    {'obj': 4,  'tex': 2, 'mode': 0x8002, 'scale': 128, 'cu': 192, 'cv': 64,
     'angle': 0,    'rmin': 17230,   'rmax': 29453,
     'passes': [
         {'mode': 0x0001, 'scale': 150, 'cu': 192, 'cv': 64,
          'angle': 0, 'rmin': 0, 'rmax': 0},
         {'mode': 0x8002, 'scale': 128, 'cu': 192, 'cv': 64,
          'angle': 0, 'rmin': 17230, 'rmax': 29453}]},
    {'obj': 8,  'tex': 0, 'mode': 0x4000, 'scale': 128, 'cu': 192, 'cv': 64,
     'angle': 0,    'rmin': 0,       'rmax': 0},
    {'obj': 9,  'tex': 0, 'mode': 0x4000, 'scale': 128, 'cu': 192, 'cv': 64,
     'angle': 0,    'rmin': 0,       'rmax': 0},
    {'obj': 10, 'tex': 0, 'mode': 0x4000, 'scale': 128, 'cu': 192, 'cv': 64,
     'angle': 0,    'rmin': 0,       'rmax': 0},
    {'obj': 18, 'tex': 2, 'mode': 0x0001, 'scale': 100, 'cu': 192, 'cv': 64,
     'angle': 0,    'rmin': 0,       'rmax': 0},
] + [
    # Spike_BuildRing gives every instance the SAME parameters the
    # master got - mode 1, scale 100, centre (192, 64), texture 2 -
    # and generates them from the ROTATED vertices, not from the
    # master's. That is the opposite of what Mesh_DeformAndMap does
    # for the struts, where the UVs come off the rest pose.
    {'obj': SPIKE_MASTER, 'tex': 2, 'mode': 0x0001, 'scale': 100,
     'cu': 192, 'cv': 64, 'angle': 0, 'rmin': 0, 'rmax': 0,
     'spike': (_a, _r)}
    for _a, _r in SPIKE_RING
] + [

    # Objects 12..17 go through FUN_002cf59c (slots 9-14), which
    # DEFORMS a rest pose into the working vertex block and then calls
    # Uv_GenerateFaceParams with these [verified]. The deformation is
    # not transcribed, so the rest pose out of Fspacedata is what gets
    # drawn here - the mapping is the demo's, the pose is the file's.
    # placed at exactly 60 degrees apart - 0, 1365, 2730, 4095, 5460
    # and 6825 of 8192 - which is the six-armed hub.
    {'obj': 13, 'tex': 2, 'mode': 0x0001, 'scale': 150, 'cu': 192, 'cv': 64,
     'angle': 0, 'rmin': 0, 'rmax': 0, 'strut': 0},
    {'obj': 12, 'tex': 2, 'mode': 0x0001, 'scale': 150, 'cu': 192, 'cv': 64,
     'angle': 0, 'rmin': 0, 'rmax': 0, 'strut': 6825},
    {'obj': 14, 'tex': 2, 'mode': 0x0001, 'scale': 150, 'cu': 192, 'cv': 64,
     'angle': 0, 'rmin': 0, 'rmax': 0, 'strut': 1365},
    {'obj': 15, 'tex': 2, 'mode': 0x0001, 'scale': 150, 'cu': 192, 'cv': 64,
     'angle': 0, 'rmin': 0, 'rmax': 0, 'strut': 2730},
    {'obj': 16, 'tex': 2, 'mode': 0x0001, 'scale': 150, 'cu': 192, 'cv': 64,
     'angle': 0, 'rmin': 0, 'rmax': 0, 'strut': 4095},
    {'obj': 17, 'tex': 2, 'mode': 0x0001, 'scale': 150, 'cu': 192, 'cv': 64,
     'angle': 0, 'rmin': 0, 'rmax': 0, 'strut': 5460},

    # Objects 5, 6, 7 go through FUN_002d155c, which sets their FACE
    # MODE to 1 - the flat, untextured rasteriser - and writes six
    # longwords per face from a table at 0x2d1590. They carry no
    # texture, so they are drawn flat here too.
    {'obj': 5, 'tex': None, 'mode': 'flat', 'scale': 0, 'cu': 0, 'cv': 0,
     'angle': 0, 'rmin': 0, 'rmax': 0},
    {'obj': 6, 'tex': None, 'mode': 'flat', 'scale': 0, 'cu': 0, 'cv': 0,
     'angle': 0, 'rmin': 0, 'rmax': 0},
    {'obj': 7, 'tex': None, 'mode': 'flat', 'scale': 0, 'cu': 0, 'cv': 0,
     'angle': 0, 'rmin': 0, 'rmax': 0},
]

# OBJECT 19 IS NOT SHIP BODYWORK. It is 17.359 x 10.897 x 9.398 units
# where no ship part exceeds 2.680, so it belongs to the scene around
# the ship rather than to the ship. Space_BuildObjects sets up the
# whole space scene, not just the vessel, and this is the clearest
# evidence of that. It gets its own projection - uv_proj_boxish,
# ORIGIN 0x002d1690 - and ship texture 2.
SCENE_PARTS = [
    {'obj': 19, 'tex': 2, 'mode': 'boxish', 'scale': 0, 'cu': 0, 'cv': 0,
     'angle': 0, 'rmin': 0, 'rmax': 0},
]

# Objects 11 and 20 are BILLBOARDS SHOWING A RUNTIME IMAGE and cannot
# be drawn from the file. FUN_002d1638 and FUN_002d1664 write their
# parameter streams from literal tables - (1,1)(159,1)(159,98) and
# (0,99)(0,0)(160,99) and their partners, which are two triangles
# covering a 160 x 98 and a 160 x 99 rectangle - and take their
# texture from pointers at 0x002f1074 and 0x002e4718 that are zero in
# the image. 160 x 98 is the framebuffer, so these are panels
# displaying something the demo renders. See
# docs/formats/geometry.txt section 6.


def part_params(part):
    """A Params from one SHIP_PARTS row."""
    return Params(mode=part['mode'], scale=part['scale'],
                  centre_u=part['cu'], centre_v=part['cv'],
                  angle=part['angle'],
                  range_min=part['rmin'], range_max=part['rmax'])


def self_test():
    t = _sincos()
    ok = True
    print('sin/cos  %d entries, table[2048] = %d (expect %d)'
          % (len(t), t[2048], TABLE_ONE))
    print('atan     %d entries' % len(_atan()))
    print('rotate2d(16384, 0, 0)    =', math_rotate2d(16384, 0, 0))
    print('rotate2d(16384, 0, 2048) =', math_rotate2d(16384, 0, 2048))
    print('sdiv(-7,3) = %d  (divs truncates; Python // would give %d)'
          % (sdiv(-7, 3), -7 // 3))
    print('')

    sys.path.insert(0, os.path.join(ROOT, 'tools', 'geo'))
    import geom
    addr, count = geom.TABLES['Fspacedata']
    sizes = geom.load_sizes(addr, count)
    blob = open(os.path.join(ROOT, 'reverse', 'extracted',
                             'Fspacedata.bin'), 'rb').read()
    objs, _e, _u = geom.parse_chunk(blob, sizes)

    print('the ship, as Space_BuildObjects configures it:')
    print('  %-4s %-5s %-8s %6s %6s  %-12s %s'
          % ('obj', 'tex', 'mode', 'faces', 'kept', 'u range', 'v range'))
    for part in SHIP_PARTS + SCENE_PARTS:
        o = objs[part['obj']]
        uvs = generate_part_uvs(o.verts, o.faces, part)
        if part['mode'] == 'flat':
            print('  %-4d %-5s %-8s %6d %6s  untextured, face mode 1'
                  % (part['obj'], '-', 'flat', len(o.faces), '-'))
            continue
        kept = [f for f in uvs if f]
        flat = [c for f in kept for c in f]
        if not flat:
            print('  obj %-2d every face rejected' % part['obj'])
            ok = False
            continue
        us = [c[0] for c in flat]
        vs = [c[1] for c in flat]
        mode = part['mode']
        mname = mode if isinstance(mode, str) else '0x%04x' % mode
        print('  %-4d %-5s %-8s %6d %6d  %4d..%-6d %4d..%d'
              % (part['obj'], part['tex'], mname, len(o.faces),
                 len(kept), min(us), max(us), min(vs), max(vs)))
        # the sampler masks to 8 bits, so anything is "legal" - but a
        # part whose u or v leaves 0..255 is almost certainly a wrong
        # scale or centre, so flag it
        if min(us) < 0 or max(us) > 255 or min(vs) < 0 or max(vs) > 255:
            print('       ^ outside 0..255; the sampler would wrap')
            ok = False

    print('')
    print('every projection over object 0, exercised:')
    o = objs[0]
    probe = Params(scale=0x5e, centre_u=0x80, centre_v=0x80, angle=0,
                   range_min=-0x20000, range_max=0x20000,
                   plane_scale_u=0x2b, plane_scale_v=0x2b)
    for bit, fn in _ORDER:
        probe.mode = bit
        uvs = generate_face_params(o.verts, o.faces, probe)
        kept = [f for f in uvs if f]
        if kept:
            flat = [c for f in kept for c in f]
            us = [c[0] for c in flat]
            vs = [c[1] for c in flat]
            rng = 'u %5d..%-5d  v %5d..%-5d' % (min(us), max(us),
                                                min(vs), max(vs))
        else:
            rng = 'every face rejected'
        print('  %-18s bit 0x%04x  %3d/%3d kept   %s'
              % (fn.__name__, bit, len(kept), len(o.faces), rng))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(self_test())
