"""Compare the reconstruction's state against the demo's, at a tick.

OURS IN FULL. Two pictures that differ may only be at different
moments, so a comparison has to be on STATE AT A TICK - and the tick
comes from tools/anchor.py, not from counting frames.

BOTH SIDES EMIT THE SAME KEYS and nothing here knows what a field
means: ours from relic_ppm -states, the demo's read out of WinUAE by
the name the key already is. docs/subsystems/state.txt.

    py -3.10 tools/statecmp.py --syms -p 15 -t 247
    py -3.10 tools/statecmp.py --demo "scenemode=1 camsrcx=-1610 ..."
    py -3.10 tools/statecmp.py --demo "..." --ours "tick=247 ..."
    py -3.10 tools/statecmp.py --demo "..." --drift 8
    py -3.10 tools/statecmp.py --selftest

THE DEMO'S HALF IS PASTED, NOT FETCHED. The debug pipe takes one
client and an MCP server holds it, so --syms prints the amiga_read
calls to make and their answers come back on the command line.

A KEY RESOLVES TO A SYMBOL BY NAME ALONE. The key is the demo's
global lowercased without g_, so g_CamSrcX is found by matching
symbol_map.txt case-insensitively - there is no table to keep in
step. A key that matches nothing is ours and is reported as having
nothing to compare against, which is not a failure.

--drift SWEEPS OUR TICK EITHER SIDE and reports the offset that fits
best. A BEST FIT IS ONLY A MEASUREMENT WHILE THE STATES STILL MATCH:
once they truly diverge every offset scores about the same and the
winner is noise, so a flat sweep is reported as unusable rather than
called an offset.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'amiga'))
sys.path.insert(0, HERE)

import anchor                                        # noqa: E402

PPM = os.path.join(ROOT, 'build', 'Win32', 'Release', 'relic_ppm.exe')
BIN = os.path.join(ROOT, 'reverse', 'ghidra', 'relic')
DAT = os.path.join(ROOT, 'reverse', 'ghidra', 'relic.dat')

# The port's own fields, which name no demo global by design.
OURS = ('tick', 'part', 'mark')


def symbols():
    try:
        import symbols as S
        return S.load_symbols()
    except Exception:
        return {}


def resolve(key, syms):
    """key -> the Ghidra symbol it names, or None if it is ours."""
    want = ('g_' + key).lower()
    for name in syms:
        if name.lower() == want:
            return name
    return None


def width_of(sym, syms):
    """THE MAP CARRIES THE WIDTH AND GUESSING IT IS WRONG. g_CameraYaw
    is a longword and g_SceneAngleA beside it is a word, so a flat
    length=4 reads the next global's top half into the answer."""
    s = syms.get(sym) or {}
    n = s.get('size') or 4
    return n if n in (1, 2, 4) else 4


def run_ours(part, tick, exe=PPM, binary=BIN, dat=DAT):
    """One state line out of relic_ppm, at that tick."""
    if not os.path.exists(exe):
        raise SystemExit('no %s - build Release|Win32 first' % exe)
    out = subprocess.run(
        [exe, binary, os.path.join(ROOT, 'build'), '-dat', dat,
         '-p', str(part), '-t', str(tick), '-n', '1', '-states'],
        capture_output=True, text=True)
    for line in out.stdout.splitlines():
        if line.startswith('tick='):
            return line.strip()
    raise SystemExit('relic_ppm printed no state line:\n' + out.stdout
                     + out.stderr)


def compare(demo, ours, syms):
    """(matched, differing, demo_only, ours_only, ours_unread)."""
    same, diff, unread, mine = [], [], [], []
    for key in sorted(ours):
        if key in OURS:
            continue
        if key in demo:
            (same if ours[key] == demo[key] else diff).append(key)
        elif resolve(key, syms):
            unread.append(key)
        else:
            mine.append(key)
    extra = sorted(k for k in demo if k not in ours and k != 'scenemode')
    return same, diff, unread, mine, extra


def report(demo, ours, syms, out=sys.stdout):
    same, diff, unread, mine, extra = compare(demo, ours, syms)
    w = out.write
    for key in diff:
        w('  %-18s demo %12d   ours %12d   delta %+d\n'
          % (key, demo[key], ours[key], ours[key] - demo[key]))
    for key in same:
        w('  %-18s %12d   MATCH\n' % (key, demo[key]))
    for key in unread:
        w('  %-18s %12s   not read from the demo - add it to --syms\n'
          % (key, '-'))
    for key in mine:
        w('  %-18s %12d   ours, nothing to compare\n' % (key, ours[key]))
    for key in extra:
        w('  %-18s %12d   demo only - our side does not report it\n'
          % (key, demo[key]))
    w('\n%d match, %d differ, %d uncompared\n'
      % (len(same), len(diff), len(unread) + len(mine) + len(extra)))
    return len(diff)


def drift(demo, part, base, span, syms, out=sys.stdout):
    """How many fields agree at each offset either side of the tick."""
    rows = []
    for d in range(-span, span + 1):
        t = base + d
        if t < 0:
            continue
        ours = anchor.parse(run_ours(part, t))
        same, bad, _, _, _ = compare(demo, ours, syms)
        rows.append((d, len(same), len(bad)))
    best = max(r[1] for r in rows)
    win = [r for r in rows if r[1] == best]
    for d, nsame, nbad in rows:
        out.write('  offset %+3d   %d match  %d differ%s\n'
                  % (d, nsame, nbad, '   <-' if nsame == best else ''))
    if len(win) == len(rows):
        out.write('\nUNUSABLE: every offset scores the same, so the'
                  ' minimum is noise and not a phase difference.\n')
        return None
    if len(win) > 1:
        out.write('\nNOT SHARP: %d offsets tie at %d. Treat it as a range,'
                  ' not an answer.\n' % (len(win), best))
        return None
    out.write('\nbest offset %+d, and it is the only one at %d match\n'
              % (win[0][0], best))
    return win[0][0]


def selftest():
    ok = True

    def check(name, got, want):
        nonlocal ok
        if got != want:
            print('FAIL %s: got %r want %r' % (name, got, want))
            ok = False

    import io
    syms = {'g_CamSrcX': 1, 'g_CamSrcY': 1, 'g_PortalPhase': 1}

    demo = anchor.parse('camsrcx=-1610 camsrcy=-20600')
    ours = anchor.parse('tick=247 part=15 mark=-1 camsrcx=-1610'
                        ' camsrcy=-20600 glowpx=0')
    check('all match', report(demo, ours, syms, io.StringIO()), 0)

    ours = anchor.parse('tick=247 camsrcx=-1480 camsrcy=-20600')
    check('one differs', report(demo, ours, syms, io.StringIO()), 1)

    # a key of ours that names a real global but was not read
    ours = anchor.parse('tick=247 camsrcx=-1610 camsrcy=-20600'
                        ' portalphase=0')
    buf = io.StringIO()
    check('unread not a failure', report(demo, ours, syms, buf), 0)
    check('unread is said', 'add it to --syms' in buf.getvalue(), True)

    # a key of ours with no global is not a failure and is labelled
    ours = anchor.parse('tick=247 camsrcx=-1610 camsrcy=-20600 glowpx=7')
    buf = io.StringIO()
    check('ours-only not a failure', report(demo, ours, syms, buf), 0)
    check('ours-only is said',
          'nothing to compare' in buf.getvalue(), True)

    # resolve is case insensitive and refuses what is not there
    check('resolve', resolve('camsrcx', syms), 'g_CamSrcX')
    check('resolve miss', resolve('glowpx', syms), None)

    print('statecmp selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--demo', help="the demo's key=value values")
    ap.add_argument('--ours', help='our state line, if already in hand')
    ap.add_argument('-p', '--part', type=int, default=15)
    ap.add_argument('-t', '--tick', type=int)
    ap.add_argument('--scene', help='name it rather than read scenemode=')
    ap.add_argument('--syms', action='store_true',
                    help='print the amiga_read calls for our keys')
    ap.add_argument('--drift', type=int, metavar='N',
                    help='sweep our tick N either side')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    syms = symbols()

    if a.syms:
        tick = a.tick if a.tick is not None else 1
        ours = anchor.parse(run_ours(a.part, tick))
        rows = []
        for key in sorted(ours):
            if key in OURS:
                continue
            sym = resolve(key, syms)
            if sym:
                rows.append((sym, key, width_of(sym, syms)))
        if not rows:
            print('part %d reports no key naming a demo global. Its'
                  ' stats() is still prose - state.txt 2.2.' % a.part)
            return 1
        rows.append(('g_SceneMode', 'scenemode',
                     width_of('g_SceneMode', syms)))
        width = max(len(r[0]) for r in rows)
        for sym, key, size in sorted(rows, key=lambda r: r[1]):
            print('amiga_read addr=%-*s length=%d   -> %s=<value>'
                  % (width, sym, size, key))
        return 0

    if not a.demo:
        ap.print_help()
        return 1
    demo = anchor.parse(a.demo)

    if a.ours:
        ours = anchor.parse(a.ours)
        tick = ours.get('tick')
    else:
        scene, err = anchor.pick(demo, a.scene)
        if err:
            print(err)
            return 1
        tick = a.tick
        if tick is None:
            tick = anchor.report(scene, demo)
            if tick is None:
                return 1
            print()
        ours = anchor.parse(run_ours(a.part, tick))

    if a.drift is not None:
        print('our part %d, sweeping %+d..%+d around tick %d'
              % (a.part, -a.drift, a.drift, tick))
        return 0 if drift(demo, a.part, tick, a.drift,
                          syms) is not None else 1

    print('demo vs ours at tick %d, part %d' % (tick, a.part))
    return 0 if report(demo, ours, syms) == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
