#!/usr/bin/env python
"""Check src/ against the verified Python transcriptions in sim/.

    py -3.10 tools/ctest.py [build-dir]

CLAUDE.md rule 20 says scale 1.0 stays exact and is the reference
every other scale is checked against. This is what enforces it: the
C host renders a part at scale 1, sim/ renders the same part, and
the two framebuffers must be byte for byte identical.

It needs relic_ppm built. With no argument it looks in build/ and
then in the session scratch directory.

    cmake -S src -B build -G "Visual Studio 15 2017" -A Win32
    cmake --build build --config Release
"""

import os
import subprocess
import time
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(ROOT, 'sim'))

BIN = os.path.join(ROOT, 'reverse', 'ghidra', 'relic')


def find_exe(hint):
    """The NEWEST relic_ppm, not the first one found.

    A tree can hold several - Win32 and x64, Debug and Release - and
    taking whichever os.walk reaches first means silently testing a
    build from an hour ago and believing the result. Age is printed
    so a stale pick is visible rather than merely wrong.
    """
    names = ('relic_ppm.exe', 'relic_ppm')
    roots = []
    if hint:
        roots.append(hint)
    roots.append(os.path.join(ROOT, 'build'))
    roots.append(os.path.join(tempfile.gettempdir(), 'claude'))
    found = []
    for r in roots:
        if not os.path.isdir(r):
            continue
        for dirpath, _d, files in os.walk(r):
            for n in names:
                if n in files:
                    p = os.path.join(dirpath, n)
                    found.append((os.path.getmtime(p), p))
    if not found:
        return None
    found.sort()
    newest = found[-1][1]
    if len(found) > 1:
        print('%d binaries found; taking the newest' % len(found))
    return newest


def read_ppm(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    if parts[0] != b'P6':
        raise ValueError('%s is not P6' % path)
    w, h = (int(x) for x in parts[1].split())
    return w, h, parts[3]


def to6(rgb):
    """Round-trip 8-bit RGB through the chipset's 6 bits, as C does."""
    out = bytearray(len(rgb))
    for i, v in enumerate(rgb):
        out[i] = ((v >> 2) * 255) // 0x3f
    return bytes(out)


def check_terminal(exe, outdir):
    import terminal
    ticks = (0, 6, 60, 120, 180, 234, 348, 999)
    bad = 0
    for t in ticks:
        subprocess.check_output([exe, BIN, outdir, '-p', '0', '-s', '1',
                                 '-t', str(t), '-n', '1'])
        path = os.path.join(outdir, 'p0_t%05d.ppm' % t)
        w, h, cdata = read_ppm(path)
        f, _looped = terminal.frame_for_step(t)
        pdata = bytearray()
        for row in terminal.rgb_rows(f):
            pdata += row
        pdata = to6(bytes(pdata))
        if len(pdata) != len(cdata):
            print('  tick %-5d SIZE %d vs %d' % (t, len(pdata), len(cdata)))
            bad += 1
            continue
        diff = sum(1 for a, b in zip(pdata, cdata) if a != b)
        print('  tick %-5d frame %-3d %3dx%-3d  %s'
              % (t, f, w, h, 'ok' if diff == 0 else 'DIFFERS %d' % diff))
        if diff:
            bad += 1
    return bad


def check_cycle(exe):
    """Stepping parts must not change the scale.

    Re-fitting a part into the already-fitted frame instead of the
    host's size shrinks it on every switch, and the error compounds -
    invisible in one render, unmistakable after four.
    """
    out = subprocess.check_output([exe, BIN, tempfile.gettempdir(),
                                   '-cycle', '8']).decode('ascii')
    seen = {}
    bad = 0
    for line in out.split('\n'):
        if 'frame' not in line:
            continue
        bits = line.split()
        part, size = bits[3], bits[5]
        if part in seen and seen[part] != size:
            print('  part %s was %s, now %s - the scale is compounding'
                  % (part, seen[part], size))
            bad += 1
        seen[part] = size
    for part in sorted(seen):
        print('  part %s stable at %s' % (part, seen[part]))
    return bad


CLOCK_S = 4


def check_clock(exe):
    """A tick is a PAL vertical blank, so four seconds is 200 of them.

    Driving the part off the host's loop instead ran everything at
    whatever rate the loop managed, which made the terminal cursor
    blink at twice its real rate - visible, but only if you know
    what the real one looks like.

    THE RUN MUST NOT OUTLAST THE FIRST PART. Demo_TickCount is the
    CURRENT part's tick and the handover resets it, so a ten second
    run reported 257 - the terminal's 242 plus what the part after
    it had got to - and read as a 25.7 Hz clock.
    """
    out = subprocess.check_output([exe, BIN, tempfile.gettempdir(),
                                   '-clock', str(CLOCK_S)]).decode('ascii')
    got = int(out.split('->')[1].split()[0])
    print('  %s' % out.strip())
    if got != CLOCK_S * 50:
        print('  the demo clock is %.1f Hz, not 50'
              % (got / float(CLOCK_S)))
        return 1
    return 0


def main(argv):
    exe = find_exe(argv[1] if len(argv) > 1 else None)
    if not exe:
        print('relic_ppm not built - see the header of this file')
        return 2
    print('exe %s  (%.0f s old)'
          % (exe, time.time() - os.path.getmtime(exe)))
    outdir = os.path.join(tempfile.gettempdir(), 'relic_ctest')
    if not os.path.isdir(outdir):
        os.makedirs(outdir)

    total = 0
    print('part 0  Terminal text  (src/part_term.c vs sim/terminal.py)')
    total += check_terminal(exe, outdir)
    print()
    print('part scale is stable across switches')
    total += check_cycle(exe)
    print()
    print('the demo clock is 50 Hz, whatever the host does')
    total += check_clock(exe)

    print()
    if total:
        print('%d check(s) failed - scale 1.0 must be exact, rule 20' % total)
        return 1
    print('src/ matches sim/ byte for byte at scale 1.0')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
