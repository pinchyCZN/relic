#!/usr/bin/env python3
"""The demo's 160x100 chunky framebuffer, out of a paused WinUAE.

    py -3.10 tools/amiga/fbdump.py             ASCII, THE RENDER TARGET
    py -3.10 tools/amiga/fbdump.py -o f.ppm    also a P6 for src/ diffs
    py -3.10 tools/amiga/fbdump.py --other     the OTHER buffer, rarely
    py -3.10 tools/amiga/fbdump.py --selftest  prove the offset search

THE DEFAULT IS THE ONE YOU WANT. g_pFrameBuffer is what every draw
routine writes through, so no argument gives the frame the demo is
building. The pointer four bytes along is the other buffer of the pair,
and reading it gets a stale or half-finished frame.

THE LABELS USED TO BE INVERTED HERE and it cost a whole session's
measurements: the flag was called --back and documented as "the buffer
being drawn into" while actually selecting the other one, and the
default printed itself as "front". Captures taken that way showed the
explosion's rings as a narrow band over rows 30 to 66 when the real
frame is a near-uniform wash over the whole height, and the port was
rewritten twice chasing the difference.

THE MCP SERVER HOLDS THE PIPE AND IT TAKES ONE CLIENT. So the CLI here
only works with the server stopped; while it is up, call its amiga_fb,
which reaches the same grab() below.

g_pFrameBuffer is 0x002f101c in the database; where CODE_03 sits live
is derived per run by code03_offset, NOT hardcoded - see its docstring.
A pixel is four bytes (R, G, LIGHT, B), each 0..0x3f - see
docs/subsystems/renderer.txt.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

G_PFRAMEBUFFER = 0x002F101C
W, H = 160, 100
RAMP = ' .:-=+*#%@'

# ORIGIN 0x002d15d8 g_Billboard11Uv - six longwords that are not
# relocated and appear nowhere else in CODE_03, so finding them gives
# that hunk's live base.
UV11 = '00000001000000010000009f000000010000009f00000062'
UV11_IMAGE = 0x002D15D8


def code03_offset(d):
    """Where CODE_03 sits in this run, which is NOT a constant.

    AmigaDOS allocates every hunk separately, so the offset from a
    Ghidra image address to a live one changes from run to run - and a
    savestate restores its own run's layout. Two states taken in
    different sessions have differed by 0x20 here. Hardcoding it reads
    plausible garbage from the wrong place, so derive it every time.
    """
    # SEARCH RETURNS (hits, complete), NOT A BARE LIST - iterating the
    # tuple yields the list and then the bool, which fails on the
    # subtraction below rather than on the unpacking.
    hits, _ = d.search(0x200000, 0x1000000, UV11, align=4, limit=8)
    for h in hits:
        off = h - UV11_IMAGE
        # the scratch copy Uv_WriteBillboard11 leaves in the params
        # array matches too; the real one puts g_pFrameBuffer in range
        p = G_PFRAMEBUFFER + off
        if 0x200000 <= p < 0x1000000:
            base = int.from_bytes(d.read(p, 4), 'big')
            if base and base < 0x2000000:
                return off
    raise ValueError('cannot locate CODE_03 - is the demo running?')


def buffer_bases(d, n=3):
    """The first n entries of the g_pFrameBuffer table, live.

    THE DEMO IS TRIPLE BUFFERED IN THE SPACE SCENES. Breaking at
    Scene_DrawTexturedSky and reading A0, the buffer about to be drawn,
    gives 00592c20 on one frame and 005a2620 on the next, and the table
    holds 00583220, 00592c20, 005a2620 - three bases 0xfa00 apart, one
    frame each [observed]. The entry after those is 0x10000 on and is
    not a fourth frame buffer.

    Reading only the first two, which is all grab() ever reached, means
    one frame in three cannot be captured at all and a walk that
    alternates between them silently drifts out of phase. Two passes
    were scored against torn frames before that was noticed.
    """
    off = code03_offset(d)
    raw = d.read(G_PFRAMEBUFFER + off, 4 * n)
    return [int.from_bytes(raw[i * 4:i * 4 + 4], 'big') for i in range(n)]


def grab_at(d, base):
    """The raw 4-byte-per-pixel buffer at an explicit live base."""
    if not base or base > 0x2000000:
        raise ValueError('framebuffer base %08x is not in RAM' % base)
    buf = bytearray()
    want = W * H * 4
    chunk = 4096
    while len(buf) < want:
        try:
            buf += d.read(base + len(buf), min(chunk, want - len(buf)))
        except Exception:
            chunk //= 2
            if chunk < 16:
                raise
    return base, bytes(buf[:want])


def grab(d, other=False):
    """The raw 4-byte-per-pixel buffer, read through an open Dbg.

    DEFAULT IS g_pFrameBuffer, THE RENDER TARGET. Pass other=True only
    to inspect the other buffer of the pair deliberately; it holds a
    stale frame and is not what a comparison against src/ wants.
    """
    p = G_PFRAMEBUFFER + code03_offset(d) + (4 if other else 0)
    base = int.from_bytes(d.read(p, 4), 'big')
    if not base or base > 0x2000000:
        raise ValueError('framebuffer pointer reads %08x' % base)
    buf = bytearray()
    want = W * H * 4
    # READ_MAX in remote_debug.cpp, which refuses anything larger with
    # "err len > 4096". Asking for more just spends round trips finding
    # that out, and the round trips ARE the cost - the bytes are not.
    # Halving on refusal anyway, so a rebuilt emulator with a bigger
    # cap needs no change here and an older one still works.
    chunk = 4096
    while len(buf) < want:
        try:
            buf += d.read(base + len(buf), min(chunk, want - len(buf)))
        except (ValueError, OSError):
            if chunk <= 512:
                raise
            chunk //= 2
    return base, bytes(buf)


def to_rgb(buf):
    """0..0x3f channels to 0..255 RGB, dropping the LIGHT byte.

    CLAMPED, because a channel is not always inside 0..0x3f: a frame
    caught mid-write, or one an effect has not finished resolving, can
    hold more, and *255//0x3f then overflows a byte.
    """
    out = bytearray()
    for i in range(0, len(buf), 4):
        out += bytes((min(255, (buf[i] * 255) // 0x3f),
                      min(255, (buf[i + 1] * 255) // 0x3f),
                      min(255, (buf[i + 3] * 255) // 0x3f)))
    return bytes(out)


def ascii_art(rgb, cols=120):
    lum = [(rgb[o] * 30 + rgb[o + 1] * 59 + rgb[o + 2] * 11) // 100
           for o in range(0, W * H * 3, 3)]
    mx = max(lum) or 1
    rows = cols * H // W // 2
    out = ['%dx%d  max=%d  lit=%d%%' % (
        W, H, mx, 100 * sum(1 for v in lum if v) // (W * H))]
    for ry in range(rows):
        out.append(''.join(
            RAMP[min(9, lum[(ry * H // rows) * W + rx * W // cols] * 10
                     // (mx + 1))] for rx in range(cols)))
    return '\n'.join(out)


def write_ppm(path, rgb):
    with open(path, 'wb') as fh:
        fh.write(b'P6\n%d %d\n255\n' % (W, H))
        fh.write(rgb)


class _FakeDbg(object):
    """Just enough of Dbg to exercise code03_offset."""

    def __init__(self, off):
        self.off = off

    def search(self, start, end, pattern, align=1, limit=256):
        return ([UV11_IMAGE + self.off, 0x64b6e8], True)

    def read(self, addr, n):
        if addr == G_PFRAMEBUFFER + self.off:
            return (0x583220).to_bytes(4, 'big')
        return (0).to_bytes(n, 'big')


def selftest():
    for off in (0x1C3544, 0x1C3564):
        got = code03_offset(_FakeDbg(off))
        if got != off:
            raise AssertionError('offset %06x came back as %06x'
                                 % (off, got))
    print('code03_offset ok for both observed layouts')
    return 0


def main(argv):
    if '--selftest' in argv:
        return selftest()
    import dbg
    d = dbg.Dbg()
    d.open()
    try:
        base, buf = grab(d, '--other' in argv)
    finally:
        d.close()
    rgb = to_rgb(buf)
    if '-o' in argv:
        write_ppm(argv[argv.index('-o') + 1], rgb)
    print('%s at %08x'
          % ('OTHER buffer (stale)' if '--other' in argv
             else 'render target', base))
    print(ascii_art(rgb))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
