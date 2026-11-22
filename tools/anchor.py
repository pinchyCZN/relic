"""Which tick of the demo a savestate is sitting at.

Not when the snapshot was taken - WHERE IN THE DEMO it is. An
emulator savestate carries no tick counter, and without one any
difference between its picture and ours could just as well be the two
being at different moments.

OURS IN FULL. The 64k project pins a tick from three music globals;
we do not reverse the replayer and do not need to. RELIC's scenes
advance plain counters by a fixed amount a tick, staged to known
values, so the tick is arithmetic:

    tick = (live - start) / step

A scene with two or more such counters cross-checks itself, and that
agreement is the confidence measure - a wrong scene or a bad read has
to satisfy every one of them at once.

    py -3.10 tools/anchor.py --list
    py -3.10 tools/anchor.py --syms departure
    py -3.10 tools/anchor.py "scenemode=1 camsrcx=-1610 camsrcy=-20600"
    py -3.10 tools/anchor.py --selftest

INPUT IS THE STATE LINE'S OWN FORMAT, key=value on the demo's global
names - docs/subsystems/state.txt. So the same command reads a line
scraped out of the emulator and a line printed by relic_ppm -states,
and feeding it ours is the self-test.

THE PIPE TAKES ONE CLIENT and an MCP server holds it, so this reads
nothing itself. --syms prints the amiga_read calls to make; paste
their values back in.
"""

import argparse
import sys

# A counter anchors a tick only when it is STAGED to a known value on
# the way in. g_SceneAngleA and g_FlareSizeStep both advance one a
# tick in mode 1 and neither is staged, so they carry whatever the
# previous scene left - 0x3709 and 0x21a7 at a tick of 247 [observed,
# savestate 8] - and anchor nothing. They are listed as carried so the
# mistake is not made twice.
SCENES = {
    'departure': {
        'mode': 1,
        'origin': 'SpaceModule_Run D0=1, effects/return.txt 3.1',
        'anchors': (
            # key        Ghidra symbol   start      step
            ('camsrcx',  'g_CamSrcX',    0x7724,   -0x82),
            ('camsrcy',  'g_CamSrcY',   -0x11170,   0xc8),
        ),
        'carried': ('sceneanglea', 'flaresizestep'),
        'ticks': 525,
    },
    # The departure's second half: one white frame, then the whole
    # screen ripples while the ship is simply no longer drawn. Both
    # counters ARE staged here - Space_RunDeparture sets 0xdc0 and
    # clears the phase at 0x002c8f12 - so both anchor.
    # Space_RunPortalRun's first phase. The fuse is staged and counts
    # down one a tick; g_PortalOpen climbs one per FOUR ticks, so it
    # cross-checks but cannot anchor on its own. THE FUSE IS A
    # BACKSTOP, NOT THE LENGTH - the loop ends on a music cue and 600
    # is only where it would give up.
    'portal': {
        'mode': 3,
        'origin': 'Space_RunPortalRun 0x002c905a, effects/return.txt 2.3',
        'anchors': (
            # key      Ghidra symbol    start   step
            ('fuse',   'g_SceneFuse',   0x258,   -1),
        ),
        'carried': ('portalopen',),
        'ticks': 600,
    },
    'explosion': {
        'mode': 2,
        'origin': 'Space_RunDeparture 0x002c8f12, effects/return.txt 6',
        'anchors': (
            # key             Ghidra symbol      start    step
            ('approachrange', 'g_ApproachRange', 0xdc0,   -0xc),
            ('ripplephase',   'g_RipplePhase',   0,      -0x1f4),
        ),
        'carried': (),
        'ticks': 227,
    },
}


def parse(text):
    """key=value pairs to a dict of ints. Hex with 0x, else decimal."""
    out = {}
    for tok in text.replace(',', ' ').split():
        if '=' not in tok:
            continue
        k, _, v = tok.partition('=')
        try:
            out[k.strip().lower()] = int(v, 0)
        except ValueError:
            pass
    return out


def pick(vals, want):
    """The scene to solve in: what was asked for, or scenemode says."""
    if want:
        if want not in SCENES:
            return None, 'no scene %r - try --list' % want
        return want, None
    mode = vals.get('scenemode')
    if mode is None:
        return None, ('no scenemode= in the input and no --scene given;'
                      ' read g_SceneMode or name the scene')
    hit = [n for n, s in SCENES.items() if s['mode'] == mode]
    if not hit:
        return None, ('scenemode=%d has no anchor table. Only a scene'
                      ' whose per-tick deltas have been read can be'
                      ' anchored - see --list.' % mode)
    return hit[0], None


def solve(scene, vals):
    """Every anchor's answer, and whether they agree."""
    rows, miss = [], []
    for key, _sym, start, step in SCENES[scene]['anchors']:
        if key not in vals:
            miss.append(key)
            continue
        delta = vals[key] - start
        tick, rem = divmod(delta, step)
        rows.append((key, vals[key], tick, rem))
    return rows, miss


def report(scene, vals, out=sys.stdout):
    s = SCENES[scene]
    rows, miss = solve(scene, vals)
    w = out.write
    w('scene %s   mode %d   %s\n' % (scene, s['mode'], s['origin']))
    if not rows:
        w('NO ANCHOR VALUE GIVEN. Needs one of: %s\n'
          % ', '.join(a[0] for a in s['anchors']))
        return None
    for key, live, tick, rem in rows:
        w('  %-14s %10d  -> tick %-6d %s\n'
          % (key, live, tick, '' if rem == 0 else '(REMAINDER %d)' % rem))
    for key in miss:
        w('  %-14s %10s     not given\n' % (key, '-'))

    bad = [r for r in rows if r[3] != 0]
    ticks = sorted(set(r[2] for r in rows))
    if bad:
        w('\nREFUSED: %s does not land on its own step, so this is not'
          ' that scene or the read is wrong.\n'
          % ', '.join(r[0] for r in bad))
        return None
    if len(ticks) != 1:
        w('\nREFUSED: the anchors disagree (%s). One of them is not the'
          ' counter this table names.\n'
          % ', '.join(str(t) for t in ticks))
        return None

    tick = ticks[0]
    w('\ntick %d of %d, from %d anchor(s) in agreement\n'
      % (tick, s['ticks'], len(rows)))
    if tick < 0 or tick > s['ticks']:
        w('*** OUTSIDE THE SCENE (0..%d). The counters are linear past'
          ' their own end, so a tick out of range means the savestate is'
          ' somewhere else.\n' % s['ticks'])
        return None
    w('  relic_ppm ... -t %d        the matching frame on our side\n' % tick)
    w('  amiga_frames n=<k>         one tick is one VBL here [measured]\n')
    if s['carried']:
        w('  carried, NOT anchors: %s - see the note in this file\n'
          % ', '.join(s['carried']))
    return tick


def syms(scene, out=sys.stdout):
    """THE SYMBOL IS STORED, NOT DERIVED. A key is the demo's name
    lowercased, and lowercasing does not invert - g_CamSrcX and
    g_CamSrcx are not the same lookup."""
    rows = [(a[1], a[0], 4) for a in SCENES[scene]['anchors']]
    rows.append(('g_SceneMode', 'scenemode', 2))
    w = max(len(r[0]) for r in rows)
    for sym, key, size in rows:
        out.write('amiga_read addr=%-*s length=%d   -> %s=<value>\n'
                  % (w, sym, size, key))


def selftest():
    """The departure's own numbers, and the refusals."""
    ok = True

    def check(name, got, want):
        nonlocal ok
        if got != want:
            print('FAIL %s: got %r want %r' % (name, got, want))
            ok = False

    import io
    # savestate 8, read live out of WinUAE
    v = parse('scenemode=1 camsrcx=-1610 camsrcy=-20600')
    sc, err = pick(v, None)
    check('scene from mode', (sc, err), ('departure', None))
    check('savestate 8', report(sc, v, io.StringIO()), 247)

    # tick 0 is the staging itself
    v = parse('camsrcx=0x7724 camsrcy=-0x11170')
    check('tick 0', report('departure', v, io.StringIO()), 0)

    # one anchor off its lattice is a refusal, not a best fit
    v = parse('camsrcx=-1611 camsrcy=-20600')
    check('off-lattice refused', report('departure', v, io.StringIO()), None)

    # two anchors that each divide cleanly but disagree
    v = parse('camsrcx=-1610 camsrcy=-20400')
    check('disagreement refused', report('departure', v, io.StringIO()), None)

    # past the end of the scene
    v = parse('camsrcx=-100000 camsrcy=100000')
    check('out of range refused', report('departure', v, io.StringIO()), None)

    v = parse('scenemode=2 approachrange=3352 ripplephase=-7000')
    sc, err = pick(v, None)
    check('scene from mode 2', (sc, err), ('explosion', None))
    check('savestate 6', report(sc, v, io.StringIO()), 14)

    v = parse('scenemode=3 fuse=582')
    check('portal slot 5', report('portal', v, io.StringIO()), 18)
    v = parse('scenemode=3 fuse=339')
    check('portal slot 6', report('portal', v, io.StringIO()), 261)

    # an unknown mode is a refusal with a reason
    sc, err = pick(parse('scenemode=7'), None)
    check('unknown mode', sc, None)
    check('unknown mode says why', 'no anchor table' in (err or ''), True)

    print('anchor selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('values', nargs='?', help='key=value pairs')
    ap.add_argument('--scene', help='name it rather than read scenemode=')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--syms', metavar='SCENE')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if a.list:
        for n, s in sorted(SCENES.items()):
            print('%-12s mode %d  %d ticks  anchors: %s'
                  % (n, s['mode'], s['ticks'],
                     ', '.join(a[0] for a in s['anchors'])))
        print('\nA scene absent here has no anchor: its per-tick deltas'
              ' have not been read.')
        return 0
    if a.syms:
        if a.syms not in SCENES:
            print('no scene %r - try --list' % a.syms)
            return 1
        syms(a.syms)
        return 0
    if not a.values:
        ap.print_help()
        return 1

    vals = parse(a.values)
    scene, err = pick(vals, a.scene)
    if err:
        print(err)
        return 1
    return 0 if report(scene, vals) is not None else 1


if __name__ == '__main__':
    sys.exit(main())
