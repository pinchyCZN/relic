"""RELIC scene 2.1 - the typing terminal.

ORIGIN 0x0033bc0c Terminal_Tick, 0x0033bc52 Terminal_ShowFrame.

THERE IS NO FONT AND NO GLYPH BLITTER. Every state of the line is a
prerendered 8-plane frame and a script says which to show; the demo
draws it by writing eight bitplane pointers into a copper list. So
this module decodes frames, it does not render text.

Everything here - the script, the palette and the frames - is read
out of the executable. Nothing is invented.

    py -3.10 sim/terminal.py        self-test
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), 'tools', 'hunk'))

import imgaddr                                            # noqa: E402

SCRIPT = 0x0033bd18       # g_TerminalScript, words, 0xffff terminates
BLANK = 0x0034973c        # g_TerminalBlank, one all-zero frame
PALETTE = 0x00349a60      # 256 longwords of 0x00RRGGBB
FRAMES = 0x00349e60       # g_TerminalFrames

FRAME_STEP = 0x320        # muls.w #0x320 on the frame index
PLANE_STEP = 0x3b40       # addi.l #0x3b40 per bitplane
PLANES = 8
ROW_BYTES = 32            # the STRIDE: 0x320 / 25 rows
DISP_BYTES = 24           # what DDF actually fetches
W = DISP_BYTES * 8
H = 24                    # DIW, NOT FRAME_STEP // ROW_BYTES
NFRAMES = 19

SCREEN_W = 640            # hstop 449 - hstart 129, hires
SCREEN_H = 256            # v=44 display top, PAL
STRIP_X = (SCREEN_W - W) // 2    # 224: see below
STRIP_Y = 192             # vstart 236 - display top 44

# THE GEOMETRY IS IN g_CopperTerminal AT 0x00367860, not in
# Terminal_ShowFrame, which writes pointers and nothing else: DIW
# and DDF give a 192 x 24 display, bottom-centre of a 640 x 256 PAL
# screen. The derivation is docs/parts.txt section 3.
#
# TAKING FRAME_STEP // ROW_BYTES FOR THE HEIGHT CORRUPTS FRAME 19
# AND NOTHING ELSE. 25 looks like the height and is not: frame 19's
# row 24 begins at exactly PLANE_STEP, so it reads plane p+1's row
# 0. Frames 1 to 18 have blank spare rows, and frame 19 is the one
# that loops for ever with the cursor on it, so the garbage appears
# under the finished line and under nothing else.

REWIND = 0x10             # subi.w #0x10 on the terminator

_cache = {}


def _image():
    if 'img' not in _cache:
        _cache['img'] = open(os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            'reverse', 'ghidra', 'relic'), 'rb').read()
    return _cache['img']


def script():
    """The frame script, read until the 0xffff terminator."""
    if 'script' not in _cache:
        _n, off = imgaddr.a2f(SCRIPT)
        img = _image()
        out = []
        while True:
            v = struct.unpack('>h', img[off:off + 2])[0]
            if v < 0:
                break
            out.append(v)
            off += 2
            if len(out) > 4096:
                raise ValueError('no terminator in g_TerminalScript')
        _cache['script'] = out
    return _cache['script']


def palette():
    """256 colours. Every one of them is a green."""
    if 'pal' not in _cache:
        _n, off = imgaddr.a2f(PALETTE)
        img = _image()
        _cache['pal'] = [
            tuple(img[off + i * 4 + 1: off + i * 4 + 4]) for i in range(256)]
    return _cache['pal']


def frame(index):
    """Deplanarise one frame to a list of H rows of W palette indices.

    ORIGIN 0x0033bc52. Index 0 is the blank; index n>0 reads frame
    n-1 at FRAMES + (n-1)*FRAME_STEP, plane p a further p*PLANE_STEP
    on. The frames are FRAME-MAJOR INSIDE EACH PLANE - plane 0 holds
    every frame back to back before plane 1 begins - so stepping a
    frame moves by 0x320 and stepping a plane by 0x3b40.
    """
    key = ('f', index)
    if key in _cache:
        return _cache[key]
    if index <= 0:
        out = [[0] * W for _ in range(H)]
        _cache[key] = out
        return out
    img = _image()
    px = [[0] * W for _ in range(H)]
    base = FRAMES + (index - 1) * FRAME_STEP
    for p in range(PLANES):
        _n, off = imgaddr.a2f(base + p * PLANE_STEP)
        bit = 1 << p
        for y in range(H):
            row = img[off + y * ROW_BYTES: off + (y + 1) * ROW_BYTES]
            line = px[y]
            for bx, b in enumerate(row):
                if not b:
                    continue
                o = bx * 8
                for k in range(8):
                    if b & (0x80 >> k):
                        line[o + k] |= bit
    _cache[key] = px
    return px


def frame_for_step(step):
    """ORIGIN 0x0033bc0c - which frame is showing at tick `step`.

        i = step >> 1
        f = script[i]
        if f < 0:  step -= 0x10 ; f = script[step >> 1]

    THE INDEX IS THE STEP HALVED, so every entry lasts two frames.
    THE TERMINATOR DOES NOT STOP ANYTHING - it rewinds eight entries
    and carries on, so the last eight loop for ever. That same moment
    sets g_IntroDone, which is what releases SpaceModule_Part0's wait
    on the loader.
    """
    sc = script()
    n = len(sc)
    s = max(0, int(step))
    i = s >> 1
    if i < n:
        return sc[i], False
    # past the terminator: the tail of eight entries repeats
    tail = REWIND >> 1
    over = i - n
    return sc[n - tail + (over % tail)], True


def rgb_rows(index, scale=1):
    """One SCREEN as packed RGB rows, ready to blit.

    The strip is 256x24 of a 640x256 display and the rest is
    background, so this returns the whole screen - which is what the
    part in src/ renders and what tools/ctest.py diffs it against.
    """
    pal = palette()
    px = frame(index)
    blank = bytes(3 * SCREEN_W * scale)
    pad = bytes(pal[0]) * STRIP_X * scale
    out = []
    for y in range(SCREEN_H):
        if y < STRIP_Y or y >= STRIP_Y + H:
            for _ in range(scale):
                out.append(blank)
            continue
        line = bytearray(pad)
        for v in px[y - STRIP_Y]:
            line += bytes(pal[v]) * scale
        line += bytes(3 * (SCREEN_W - STRIP_X - W) * scale)
        b = bytes(line)
        for _ in range(scale):
            out.append(b)
    return out


def _selftest():
    assert (NFRAMES - 1) * FRAME_STEP + H * ROW_BYTES == PLANE_STEP,         'the frames must end exactly on the plane boundary'
    print('%d frames of %d rows end exactly on PLANE_STEP' % (NFRAMES, H))

    sc = script()
    print('script: %d entries, max frame %d' % (len(sc), max(sc)))
    print('  head %s' % sc[:16])
    print('  tail %s' % sc[-8:])
    assert len(sc) * 2 >= 100, 'script looks too short'

    pal = palette()
    greens = sum(1 for r, g, b in pal if g > r and g > b)
    print('palette: %d of 256 are green-dominant' % greens)
    assert pal[0] == (0, 0, 0), 'index 0 should be the background'

    blank = frame(0)
    assert not any(v for r in blank for v in r), 'frame 0 must be blank'
    print('frame 0 is blank')

    prev = 0
    for i in range(1, NFRAMES + 1):
        ink = sum(1 for r in frame(i) for v in r if v)
        if i > 1 and i % 2 == 1:
            assert ink > prev, 'frame %d has less ink than %d' % (i, i - 2)
        if i % 2 == 1:
            prev = ink
    print('ink grows monotonically across the odd frames - it is typing')

    for step in (0, 1, 2, 40, 90, 200, 400, 10000):
        f, looped = frame_for_step(step)
        print('  step %5d -> frame %2d%s'
              % (step, f, '  (looping)' if looped else ''))


if __name__ == '__main__':
    _selftest()
