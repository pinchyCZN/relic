#!/usr/bin/env python
"""Project-local MCP server over WinUAE's remote debugger.

Wired from .mcp.json, so it is scoped to this project and not installed
globally. Speaks MCP's JSON-RPC 2.0 over stdio using nothing but the
standard library - there is no mcp package to install.

Every tool goes through tools/amiga/dbg.py to the named pipe, and the
emulator answers one command per emulated frame. See
WinUAE_SRC/docs/REMOTE_DEBUG.txt for the protocol and why it is paced
way.

    py -3.10 tools/amiga/mcp_server.py --selftest
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dbg import Dbg, NotRunning                              # noqa: E402
import symbols                                               # noqa: E402

PROTOCOL = '2024-11-05'

_conn = {'d': None}

# Mtimes as they were when this process imported them. A long-running
# stdio server does NOT reload its own source, so an edit on disk reaches
# nobody until the server restarts - which has now been mistaken twice for
# a fix that did not work. t_ping compares these against the files to make
# that visible in one call instead of invisible for a session.
_SRC = ('mcp_server.py', 'dbg.py', 'symbols.py')
_LOADED = {}
for _f in _SRC:
    try:
        _LOADED[_f] = os.path.getmtime(os.path.join(
            os.path.dirname(os.path.abspath(__file__)), _f))
    except OSError:
        pass


def dbg():
    if _conn['d'] is None:
        _conn['d'] = Dbg()
    return _conn['d']


def _hexdump(addr, data):
    out = []
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        out.append('%08x  %s  %s' % (
            addr + i,
            ' '.join('%02x' % b for b in row).ljust(47),
            ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)))
    return '\n'.join(out)


# ---------------------------------------------------------------- tools

def t_ping(a):
    """Liveness, plus whether THIS SERVER IS RUNNING STALE CODE.

    The second half is the point. An MCP server is a long-running stdio
    process and never reloads its source, so a fix landed in
    mcp_server.py reaches a running session only when the user restarts
    it. Twice now a fix has been reported as not working when the real
    state was that the caller was executing the code from before it. The
    files are re-stat'ed here and any that have changed since import are
    named, so "am I stale?" costs one call rather than a session.
    """
    out = [dbg().ping()]
    here = os.path.dirname(os.path.abspath(__file__))
    stale = []
    for f in _SRC:
        was = _LOADED.get(f)
        try:
            now = os.path.getmtime(os.path.join(here, f))
        except OSError:
            continue
        if was is not None and now > was:
            stale.append('%s (changed %.0fs after this server started)'
                         % (f, now - was))
    out.append('server: %d tools, %s'
               % (len(TOOLS), time.strftime(
                   '%Y-%m-%d %H:%M', time.localtime(
                       _LOADED.get('mcp_server.py', 0)))))
    if stale:
        out.append('*** THIS SERVER IS RUNNING STALE CODE: '
                   + '; '.join(stale))
        out.append('*** Restart the MCP server before trusting a fix. '
                   'A running server never reloads its own source.')
    return chr(10).join(out)


def t_regs(a):
    r = dbg().regs()
    d = ' '.join('d%d=%08x' % (i, r['d%d' % i]) for i in range(8))
    n = ' '.join('a%d=%08x' % (i, r['a%d' % i]) for i in range(8))
    return '%s\n%s\npc=%08x sr=%04x' % (d, n, r['pc'], r['sr'])


def t_read(a):
    addr = addr_of(a['addr'])
    length = int(a.get('length', 16))
    return _hexdump(addr, dbg().read(addr, length))


def t_write(a):
    return dbg().command('write %x %s' % (addr_of(a['addr']), a['hex']))


def t_watch(a):
    addr = addr_of(a['addr'])
    length = int(a.get('length', 4))
    rwi = {'read': 1, 'write': 2, 'both': 3}[a.get('access', 'write')]
    halt = bool(a.get('halt', False))
    cmd = 'watch add %x %x %d%s' % (addr, length, rwi, '' if halt else ' log')
    return dbg().command(cmd)


def t_watch_clear(a):
    return dbg().command('watch del %s' % a.get('slot', 'all'))


def t_hits(a):
    recs = dbg().hits(int(a.get('max', 1024)))
    if not recs:
        return 'no hits logged'
    lines = ['frame     pc        addr      sz rw value']
    for r in recs:
        lines.append('%-9d %08x  %08x  %-2d %s  %08x'
                     % (r['frame'], r['pc'], r['addr'], r['size'],
                        r['rw'], r['val']))
    return '\n'.join(lines)


def t_hits_clear(a):
    """Throw the watch log away so the next read is only new records.

    The log is a ring the emulator fills whenever a watch matches, and
    nothing empties it but a read. So a log left full by an earlier
    investigation is what the NEXT amiga_hits returns - the records
    look plausible, carry old frame numbers, and have cost real time to
    misread more than once. This drains it in rounds to report how much
    it discarded, then issues "log clear".

    THE DRAIN ALONE DID NOT CLEAR THE TALLIES and that was a wrong-answer
    bug. Draining reads records out of the ring; the counters amiga_hits_
    count reports are separate and only "log clear" resets them. So this
    said "log already empty" while amiga_hits_count still returned the
    previous investigation's totals - 19 hits on an address that had not
    been written once, which reads as a finding rather than as stale
    state. "log clear" also zeroes the per-watch counters in watch list.

    PAUSE FIRST. A running emulator keeps adding records, so the drain
    would race it; the round cap stops that becoming an infinite loop
    rather than making it safe.
    """
    cap = int(a.get('max_records', 1 << 18))
    rounds_cap = int(a.get('max_rounds', 512))
    total = 0
    rounds = 0
    drained = None
    while total < cap and rounds < rounds_cap:
        recs = dbg().hits(1024)
        rounds += 1
        if not recs:
            drained = ('log drained: %d record(s) discarded in %d round(s)'
                       % (total, rounds)) if total else 'log already empty'
            break
        total += len(recs)
    if drained is None:
        drained = ('stopped after %d record(s) in %d round(s) - the log did '
                   'not run dry, so the emulator is probably still running '
                   'and filling it. Pause and try again.' % (total, rounds))
    cleared = dbg().command('log clear')
    if not cleared.startswith('ok'):
        return ('%s' + chr(10) + 'but "log clear" failed: %s'
                ) % (drained, cleared)
    return ('%s' + chr(10) + 'tallies and per-watch counters cleared too, '
            'so amiga_hits_count now reads 0.') % drained


def t_search(a):
    # WHITESPACE STRIPPED, because a dump is where a pattern gets copied
    # from and a dump has spaces in it. The protocol line is
    # space-separated, so "000012930000 0cf6ffffeece" arrived at the
    # emulator as a pattern plus a second argument and came back as
    # "err unknown option 0cf6ffffeece" - which reads like a bad option
    # rather than like a pattern that needed joining up.
    pattern = ''.join(str(a['pattern']).split())
    a = dict(a, pattern=pattern)
    start = addr_of(a['start'])
    end = addr_of(a['end'])
    align = int(a.get('align', 1))
    limit = int(a.get('limit', 256))
    hits, complete = dbg().search(start, end, a['pattern'], align, limit)
    if not hits:
        return 'no match for %s in %08x..%08x' % (a['pattern'], start, end)
    out = ['%d match(es) for %s in %08x..%08x'
           % (len(hits), a['pattern'], start, end)]
    out.append(' '.join('%08x' % h for h in hits))
    if not complete:
        out.append('STOPPED at the limit of %d - there may be more past %08x'
                   % (limit, hits[-1]))
    return '\n'.join(out)


def t_break(a):
    """A breakpoint, optionally one that only stops when something holds.

    WITHOUT A CONDITION, finding the one interesting call out of the ~400
    that go through an address costs a continue and a register read each,
    at an emulated frame plus this server's own latency per round trip.
    With one it costs a single stop.

    Read the counters back with amiga_break_list: hits= is how often the
    PC was reached and passed= how often the condition held, and
    hits=412 passed=0 is the difference between code that never runs and
    a condition that is wrong.
    """
    cmd = 'bp add %x' % addr_of(a['addr'])
    if a.get('when'):
        cmd += ' when=%s' % expand(a['when'])
    if a.get('skip'):
        cmd += ' skip=%x' % int(a['skip'])
    return dbg().command(cmd)


def t_break_list(a):
    r = dbg().command('bp list')
    if r.strip() == 'ok':
        return 'no breakpoints set'
    return r


def t_break_clear(a):
    return dbg().command('bp del %s' % a.get('slot', 'all'))


def t_cont(a):
    return dbg().command('cont')


def t_pause(a):
    set_to = a.get('set')
    return dbg().command('pause' if set_to is None else 'pause %s' % set_to)


def wait_for_stop(tries=400, delay=0.05):
    """Poll until the guest is stopped, and answer with its state.

    Returns the "pause" reply, whatever it is - the caller decides
    whether 'still running' matters. Note that 'restoring' is a RUNNING
    state: a restore in flight has the guest going on purpose.
    """
    for _ in range(tries):
        st = dbg().command('pause')
        if 'paused' in st or 'halted' in st:
            return st
        time.sleep(delay)
    return 'still running: ' + dbg().command('pause')


def wait_for_restore(tries=400, delay=0.02):
    """Poll until an armed restore has actually landed.

    "state load" only ARMS the restore; savestate_check() carries it out
    later in the frame, after the point the pipe is serviced. So the
    command immediately after a load can still see the OLD machine -
    measured: a load of a slot holding frame 1, issued while running,
    answered "ok restoring 4" and the very next command read frame 219.
    The framebuffer was already restored one command after that, which is
    what makes it easy to miss: whether you catch it depends on which
    command lands in the gap.

    The state reads "restoring" for as long as savestate_state is set, so
    waiting for that word to go is waiting for the restore to land.

    THIS IS NOT wait_for_stop(). A load taken while running leaves the
    guest RUNNING, so waiting for a stop waits for something that never
    comes - it spun out its whole budget and let the demo run a thousand
    frames past where the caller wanted it.
    """
    st = ''
    for _ in range(tries):
        st = dbg().command('pause')
        if 'restoring' not in st:
            return st
        time.sleep(delay)
    return 'still restoring: ' + st


def t_frames(a):
    """Run n frames, wait for the stop, and SAY WHERE IT ACTUALLY LANDED.

    The countdown in the emulator is exact - it is decremented once per
    frame from the same hsync_handler() call that increments the frame
    counter, and measures as exactly +n at every size tried. So when a
    run does not land on n, the cause is here rather than there, and
    both causes are invisible unless this reports them:

    A HALT PART WAY THROUGH leaves the rest of the countdown standing.
    The emulator now says "owed=<k>" in that case, and the next
    amiga_continue or amiga_pause set=off will run those k frames - so
    reading "halted" as "the run finished" leaves the guest somewhere
    else entirely the moment it is resumed.

    A POLL TIMEOUT returns while the run carries on. The budget below is
    400 polls of 50ms, and a measured n=1000 took 20.0s against it, so a
    bigger n - or any n with a breakpoint armed, which slows emulation a
    long way - can outlast it.

    Either way the frame numbers from before and after are the
    measurement, not the reply to the command.
    """
    n = int(a.get('n', 1))
    # BUDGET SCALED TO n, BECAUSE A FLAT ONE IS BEATEN BY A BREAKPOINT.
    # Measured on this config: 20.0 ms/frame with nothing armed, 20.1 with
    # a log-only watch (free), and 47.6 ms/frame with a single breakpoint
    # armed that never even fires - 2.4x, the cost of checking. The old
    # flat 400 polls of 50ms ran out at about n=420 under a breakpoint.
    # Allowing 60ms a frame plus the old 20s of slack covers that; the
    # timeout is still reported rather than hidden if it is beaten anyway.
    tries = 400 + int(n * 0.06 / 0.05)
    f0 = dbg().frame()
    r = dbg().command('frames %x' % n)
    if not r.startswith('ok'):
        return r
    st = wait_for_stop(tries)
    f1 = dbg().frame()
    out = [r, 'now: ' + st,
           'frame %d -> %d: advanced %d, asked for %d' % (f0, f1, f1 - f0, n)]
    if f1 - f0 != n:
        out.append('*** DID NOT LAND ON %d. The %d frames above are what the '
                   'guest actually ran; treat THAT as the measurement.'
                   % (f0 + n, f1 - f0))
    if 'owed=' in st:
        out.append('*** THE COUNTDOWN IS STILL OWED FRAMES - see owed= above. '
                   'They will run on the next amiga_continue or amiga_pause '
                   'set=off, landing the guest further on than this. Clear it '
                   'with another amiga_frames, which replaces the count.')
    if 'still running' in st:
        out.append('*** THE POLL BUDGET RAN OUT, NOT THE RUN. It is still '
                   'going and the frame number above is already stale.')
    return chr(10).join(out)


def t_state_save(a):
    return dbg().command('state save %d' % int(a.get('slot', 0)))


def t_state_load(a):
    """Restore a slot, and do not come back until it is really restored.

    THE RESTORE IS NOT WHAT THE REPLY SAYS IT IS. "state load" only
    arms it; savestate_check() in the emulator carries it out, and that
    runs LATER IN THE FRAME than the point the pipe is serviced from. A
    load taken while the guest is stopped therefore used to arm a
    restore that was never reached, and every read afterwards handed
    back the memory from BEFORE the load - not one frame late but
    indefinitely, which reads as a successful load of a state that
    happens to look like the last one.

    The emulator now lets the guest go for the frame or two the restore
    needs and pauses itself again, answering "restoring" until then.
    Waiting for that here is the whole point: a caller that reads on
    the reply measures the state it just replaced.

    BOTH RUN STATES WAIT NOW, and the running one used not to. The
    old code waited only when the reply said "repausing" - the
    paused case - on the grounds that a load taken while running
    completes inside the same frame. It does not reliably:
    measured, a load of a slot holding frame 1 answered "ok
    restoring 4" and the very next command read frame 219. The
    guarantee must not depend on which state the guest happened
    to be in.
    """
    r = dbg().command('state load %d' % int(a.get('slot', 0)))
    if not r.startswith('ok'):
        return r
    st = wait_for_restore()
    if 'repausing' in r:
        st = wait_for_stop()
    return ('%s' + chr(10) + 'now: %s' + chr(10) + '%s') % (
        r, st, dbg().ping())


def t_snap_save(a):
    """Save to the MCP's own savestate namespace, 0-1000.

    SEPARATE FILES FROM THE NUMPAD'S. These are mcp_NNNN.uss beside
    default_N.uss, so nothing here can overwrite a slot marked by hand -
    which amiga_state_save CAN, because it shares default_0..9.uss with
    the numpad keys. Use this for sweeps and amiga_state_save only when
    you mean one of the ten human slots.
    """
    n = int(a.get('slot', 0))
    label = str(a.get('label', '')).strip()
    cmd = 'snap save %x' % n
    if label:
        cmd += ' ' + label
    return dbg().command(cmd)


def t_snap_load(a):
    """Restore an MCP snapshot, waiting until it has really landed."""
    r = dbg().command('snap load %x' % int(a.get('slot', 0)))
    if not r.startswith('ok'):
        return r
    st = wait_for_restore()
    if 'repausing' in r:
        st = wait_for_stop()
    return ('%s' + chr(10) + 'now: %s' + chr(10) + '%s') % (
        r, st, dbg().ping())


def t_snap_list(a):
    """What the MCP namespace holds: slot, frame and label."""
    cmd = 'snap list'
    if a.get('start') is not None:
        cmd += ' %x' % int(a['start'])
        if a.get('count') is not None:
            cmd += ' %x' % int(a['count'])
    r = dbg().command(cmd)
    if not r.startswith('ok'):
        return r
    body = r[2:].strip()
    if not body or body == 'none':
        return 'no MCP snapshots saved yet'
    rows = []
    for t in body.split('|'):
        p = t.strip().split(':', 2)
        if len(p) == 3:
            rows.append('  %-5s frame %-9s %s' % (p[0], p[1], p[2]))
    return ('%d snapshot(s)   slot / frame / label' % len(rows)
            + chr(10) + chr(10).join(rows))


def t_warp(a):
    """DISABLED - it will report, and turn warp off, and nothing else.

    WARP IS NOT ACCURATE ENOUGH TO MEASURE ON. With cpu_speed=max the
    per-frame cycle budget is wall-clock derived, and WinUAE gates that
    budgeting on not being in warp (custom.cpp:8709) - so warping
    starves the demo's scene stepping while its music keeps absolute
    time, and the two come apart. One savestate run 2901 frames each
    way read the swirl's fuse at 412 normal against 476 warped, with
    the music identical in both. A whole set of snapshots was recorded
    under warp and had to be thrown away.

    Turning it OFF still works, so a warp left on elsewhere can always
    be cleared from here.
    """
    set_to = a.get('set')
    if set_to is not None and set_to != 'off':
        return ('warp is DISABLED in this server: it starves the scene of '
                'CPU cycles while the music keeps absolute time, so no span '
                'or savestate taken under it is trustworthy. Sweep at normal '
                'speed - amiga_frames 500 is 10 s of demo.')
    return dbg().command('warp' if set_to is None else 'warp off')


def t_reset(a):
    return dbg().command('reset hard' if a.get('hard') else 'reset')


def t_raw(a):
    return dbg().command(a['command'])


# ------------------------------------------------- labels and addresses

_live = {'l': None}


def live():
    """One symbols.Live per connection, so slides are derived once.

    It caches them and re-verifies before every reuse, so this being
    long-lived is safe across a savestate load - which is the one event
    that silently changes every slide.
    """
    if _live['l'] is None or _live['l'].d is not dbg():
        _live['l'] = symbols.Live(dbg())
    return _live['l']


def addr_of(spec):
    """An address argument, which may be a label.

    THREE FORMS, AND THE DEFAULT IS DELIBERATELY THE OLD ONE:

        4a40de          a LIVE address, exactly as before
        img:2e0b7a      an IMAGE address, slid to live for you
        g_CamX          a label, looked up and slid to live

    Bare hex still means live because every existing call, every
    savestate note and every number fbdump hands back is a live address,
    and quietly reinterpreting those would turn working calls into
    plausible garbage. The cost is that an image address pasted from
    docs/ needs the img: prefix - which is why the prefix is three
    characters and says what it does.
    """
    s = str(spec).strip()
    low = s.lower()
    for pre in ('img:', 'image:'):
        if low.startswith(pre):
            return live().live(int(s[len(pre):], 16))
    try:
        return int(s, 16)
    except ValueError:
        pass
    return live().resolve(s)['live']


def expand(text):
    """{label} and {img:addr} inside a condition -> live hex.

    A condition goes to the emulator as addresses, because the emulator
    has no symbols and should not have any - see docs/REMOTE_DEBUG.txt.
    the substitution happens here:

        sl@{g_TempleCamX}:-100..100
        w@{img:2e0bf6}==4

    Braces rather than bare names because the condition grammar already
    uses letters for registers and size prefixes, and "sl@g_CamX" cannot
    be told from a typo without implementing the whole grammar twice.
    """
    out = []
    i = 0
    while True:
        j = text.find('{', i)
        if j < 0:
            out.append(text[i:])
            return ''.join(out)
        k = text.find('}', j)
        if k < 0:
            raise ValueError('unclosed { in %r' % text)
        out.append(text[i:j])
        out.append('%x' % addr_of(text[j + 1:k]))
        i = k + 1


def t_symbols(a):
    """What the label table knows, without touching the emulator."""
    syms = symbols.load_symbols()
    q = (a.get('match') or '').lower()
    rows = [(n, s) for n, s in syms.items() if not q or q in n.lower()]
    rows.sort(key=lambda t: t[1]['addr'])
    if not rows:
        return ('no symbol matching %r in symbol_map.txt (%d known)'
                % (a.get('match'), len(syms)))
    limit = int(a.get('limit', 40))
    out = ['%d of %d symbol(s)%s   image addresses, from Ghidra'
           % (min(len(rows), limit), len(syms),
              ' matching %r' % a['match'] if q else '')]
    for n, s in rows[:limit]:
        out.append('  %08x  %-34s %-8s %s'
                   % (s['addr'], n, s['hunk'], s['type']))
    if len(rows) > limit:
        out.append('  ... %d more; narrow with match=' % (len(rows) - limit))
    return chr(10).join(out)


def t_slide(a):
    """The live slide for a hunk, derived and double-checked."""
    hunk = a.get('hunk', 'CODE_03')
    L = live()
    s = L.slide(hunk)
    seg = None
    for nm, lo, hi, _flo in symbols.imgaddr.SEGMENTS:
        if nm == hunk:
            seg = (lo, hi)
    return ('%s slide %+#x\n  image %08x..%08x -> live %08x..%08x\n'
            'Derived from two independent anchors that agreed. It is a '
            'property of THIS run: a savestate load can change it, and '
            'the next read through symbols.Live re-verifies rather than '
            'trusting it.'
            % (hunk, s, seg[0], seg[1], seg[0] + s, seg[1] + s))


# ------------------------------------------------------------ fingerprint

def _match_term(term, selvals):
    """One <selector> == <n> or <selector> != <n>. None if not evaluable."""
    for op in ('!=', '=='):             # '!=' first: it contains '='
        if op in term:
            lhs, _, rhs = term.partition(op)
            lhs, rhs = lhs.strip(), rhs.strip()
            if lhs not in selvals or selvals[lhs] is None:
                return None
            try:
                want = int(rhs, 0)
            except ValueError:
                return None
            got = selvals[lhs]
            return got != want if op == '!=' else got == want
    return None


def _match_when(when, selvals):
    """A conjunction of <selector> == <n> and <selector> != <n> terms.

    ANYTHING ELSE IS REPORTED, NOT GUESSED. A scene picked by a
    half-understood expression sends the reader to the wrong row, which
    is worse than being told to pick one.

    "&&" IS NOT GENERALITY FOR ITS OWN SAKE. Identifying the ruins needs
    two terms that fail in opposite directions - an allocated pointer
    that says the temple module has run at all, and a mode that says
    which of its two scenes - and neither is sufficient alone. See
    docs/fingerprints.txt.

    THREE-VALUED, deliberately: a term that cannot be evaluated makes
    the answer None rather than False, because "I cannot tell" and "this
    is not the scene" lead to different and equally important replies.
    One definitely-false term still settles the whole conjunction,
    though, so an unreadable selector elsewhere cannot turn a clear
    rejection into a shrug.
    """
    if not when:
        return None
    unknown = False
    for term in when.split('&&'):
        v = _match_term(term.strip(), selvals)
        if v is False:
            return False                # false && anything is false
        if v is None:
            unknown = True
    return None if unknown else True


def t_fingerprint(a):
    """Which frame is this? Read the globals that identify the scene.

    ONE CALL FOR WHAT USED TO BE A PROCEDURE: derive the hunk slide,
    work out which globals the part even has, read each at the right
    size and signedness, and label the result. The slide is derived
    here - the caller never does the UV11 search - and every address in
    docs/fingerprints.txt stays an image address, which is what makes
    the table survive a re-analysis.
    """
    L = live()
    scenes, selectors = symbols.parse_fingerprints()
    if not scenes:
        return ('docs/fingerprints.txt has no .fingerprint blocks. '
                'Nothing to read.')

    # FRAME BEFORE AND AFTER, because a fingerprint is several reads and
    # a running demo moves between them - which produces a reading that
    # is internally inconsistent and looks fine.
    f0 = dbg().frame()
    state = dbg().command('pause')
    out = []

    selvals = {}
    for nm, (ref, size) in sorted(selectors.items()):
        try:
            selvals[nm] = L.read(L.resolve(ref)['image'], size)
        except Exception as e:
            selvals[nm] = None
            out.append('selector %s unreadable: %s' % (nm, e))

    # EVERY SELECTOR ZERO IS THE SHAPE OF A BAD SLIDE, not of a scene.
    # These live in hunks whose slide is derived; a derivation that came
    # back wrong reads zeros out of the wrong addresses, and a zero is a
    # confident answer to "is this the ruins" rather than an obviously
    # broken one. Two agreeing anchors make that unlikely, not
    # impossible, so the shape is flagged.
    readable = [v for v in selvals.values() if v is not None]
    if readable and not any(readable):
        out.append('*** EVERY SELECTOR READS ZERO. That is what a wrong '
                   'hunk slide looks like as well as what an early scene '
                   'looks like, and the two cannot be told apart from the '
                   'values. Check amiga_slide against a known global '
                   'before trusting a verdict below.')

    want = a.get('scene')
    if want and want not in scenes:
        return ('no scene %r in docs/fingerprints.txt. It has: %s'
                % (want, ', '.join(sorted(scenes))))
    if not want:
        hits = [nm for nm, sc in scenes.items()
                if _match_when(sc.when, selvals) is True]
        if len(hits) == 1:
            want = hits[0]
            out.append('scene %s, picked by its when= against the '
                       'selectors below' % want)
        else:
            out.append('CANNOT TELL WHICH SCENE THIS IS%s.'
                       % ('' if not hits else ' - %d blocks match: %s'
                          % (len(hits), ', '.join(hits))))
            out.append('Selectors read from the live machine:')
            for nm in sorted(selvals):
                v = selvals[nm]
                out.append('  %-22s %s' % (nm, 'unreadable' if v is None
                                           else '%d (0x%x)' % (v, v)))
            out.append('')
            out.append('Blocks in docs/fingerprints.txt: %s'
                       % ', '.join(sorted(scenes)))
            out.append('Pass scene= to read one anyway. No value of any '
                       'selector has been written down as meaning a '
                       'particular part, which is why this cannot be '
                       'resolved automatically yet - see that file.')
            return chr(10).join(out)

    sc = scenes[want]
    # A FORCED SCENE WHOSE when= DOES NOT HOLD READS PERFECTLY AND MEANS
    # NOTHING. Every field resolves, every value is a plausible number,
    # and they are the wrong scene's globals - which is the exact shape
    # of mistake this table exists to stop.
    held = _match_when(sc.when, selvals)
    if held is False:
        out.append('*** THIS IS PROBABLY NOT THE RUNNING SCENE. %s does '
                   'not hold - see the selector values at the bottom. The '
                   'readings below are that scene of globals read '
                   'anyway, and they will be plausible numbers. '
                   % sc.when)
    if sc.verified is False:
        out.append('*** THIS BLOCK IS MARKED verified = no. The rows are '
                   'inference, not measurement - do not quote the reading '
                   'below as fact.')
    elif sc.verified is None:
        out.append('*** This block does not say whether it was verified.')
    out.append('scene %s   part %s   state: %s'
               % (sc.name, sc.part, state[3:] if state.startswith('ok ')
                  else state))
    if sc.when:
        out.append('when: %s' % sc.when)
    if sc.note:
        out.append('note: %s' % sc.note)
    out.append('')
    if not sc.fields:
        out.append('This block has NO FIELDS: it identifies the scene and '
                   'reads nothing. The selector pair below is what names '
                   'it; nobody has read the scene itself yet.')
    else:
        out.append('%-12s %-18s %-10s %12s  %-10s %s'
                   % ('label', 'symbol', 'live', 'value', 'hex',
                      'our stats()'))
    for f in sc.fields:
        try:
            r = L.resolve(f.ref)
            v = L.read(r['image'], f.size, f.signed)
            out.append('%-12s %-18s %08x   %12d  %-10s %s'
                       % (f.label, f.ref, r['live'], v,
                          '%0*x' % (f.size * 2, v & ((1 << (f.size * 8)) - 1)),
                          f.stat or '-'))
        except Exception as e:
            out.append('%-12s %-18s  ERROR: %s' % (f.label, f.ref, e))

    out.append('')
    for nm in sorted(selvals):
        v = selvals[nm]
        out.append('selector %-22s %s'
                   % (nm, 'unreadable' if v is None else '%d (0x%x)' % (v, v)))

    f1 = dbg().frame()
    out.append('')
    if f0 != f1:
        out.append('*** THE DEMO RAN WHILE THIS WAS BEING READ, frame %d to '
                   '%d. The values above are not all from one frame. Pause '
                   'and read again.' % (f0, f1))
    else:
        out.append('all read at frame %d' % f0)
    return chr(10).join(out)


# ------------------------------------------------------------------ until

def t_advance_until(a):
    """Run frames until a condition holds, then pause.

    The frame budget is given in DECIMAL here and sent as hex, because
    the protocol takes hex everywhere and a frame count is the one place
    that reads as a decimal quantity - see docs/REMOTE_DEBUG.txt under
    "until".
    """
    cond = expand(a['condition'])
    maxf = int(a.get('max_frames', 300))
    if maxf < 1:
        return 'max_frames must be at least 1'
    f0 = dbg().frame()
    r = dbg().command('until %s max=%x' % (cond, maxf))
    if not r.startswith('ok'):
        return r
    out = [r]
    if 'already true' in r:
        out.append('frame %d, nothing run - the guest was already there.' % f0)
        return chr(10).join(out)
    # The same budget shape amiga_frames uses, scaled to the cap, plus
    # slack: a breakpoint armed at the same time costs about 2.4x.
    st = wait_for_stop(400 + int(maxf * 0.06 / 0.05))
    f1 = dbg().frame()
    out.append('now: ' + st)
    out.append('frame %d -> %d: ran %d of at most %d'
               % (f0, f1, f1 - f0, maxf))
    if 'until-expired' in st:
        out.append('*** THE CONDITION NEVER HELD in %d frame(s). Either it '
                   'is wrong, or the demo does not reach that state from '
                   'here. A conditional breakpoint reports hits= and '
                   'passed= and will tell you which.' % (f1 - f0))
    elif 'until-hit' not in st and 'still running' in st:
        out.append('*** THE POLL BUDGET RAN OUT, NOT THE RUN. It is still '
                   'going and the frame number above is already stale.')
    if 'until-owed' in st:
        out.append('*** SOMETHING HALTED PART WAY THROUGH and the rest of '
                   'the cap is still standing - see until-owed above. Your '
                   'next amiga_continue runs it.')
    return chr(10).join(out)


def t_hits_count(a):
    """Tallies rather than records - one line instead of nine hundred."""
    by = a.get('by', 'pc')
    if by not in ('pc', 'val', 'addr'):
        return "by must be pc, val or addr"
    limit = int(a.get('limit', 32))
    r = dbg().command('log count by=%s max=%x' % (by, limit))
    if not r.startswith('ok'):
        return r
    head, rows = {}, []
    for tok in r.split()[1:]:
        if '=' in tok:
            k, _, v = tok.partition('=')
            head[k] = v
        elif ':' in tok:
            k, _, v = tok.partition(':')
            rows.append((int(k, 16), int(v)))
    out = ['by=%s  total=%s  distinct=%s  dropped=%s'
           % (head.get('by'), head.get('total'), head.get('distinct'),
              head.get('dropped'))]
    if not rows:
        out.append('no hits counted. Is a log-mode watch set, and has the '
                   'demo run since the last amiga_hits_clear?')
        return chr(10).join(out)
    width = max(len('%d' % c) for _k, c in rows)
    for k, c in rows:
        out.append('  %08x  %*d' % (k, width, c))
    if head.get('more'):
        out.append('  ... %s more key(s); raise limit=' % head['more'])
    out.append('')
    out.append('COUNTED AS THE HITS ARRIVED, so total is exact even where '
               'amiga_hits would have lost records to the 4096-slot ring. '
               'This does NOT drain the log - amiga_hits still returns the '
               'records, and amiga_hits_clear resets both.')
    if head.get('dropped') not in (None, '0'):
        out.append('*** dropped=%s: that many hits had a key the 192-entry '
                   'table could not fit. The counts shown are still exact '
                   'for the keys named.' % head['dropped'])
    return chr(10).join(out)

ADDR = {'type': 'string',
        'description': 'a LIVE hex address (2e0c28), or "img:<hex>" for an '
                       'image address from docs/ or Ghidra, or a label like '
                       'g_CamX. Bare hex stays live so existing calls and '
                       'savestate notes keep working.'}
SLOT = {'type': 'integer',
        'description': 'savestate slot 0-9, the same slots the numpad '
                       'keys use'}

def t_fb(a):
    import fbdump
    # TIMED INSIDE THE SERVER. Timing an MCP tool from outside measures
    # the caller's own turn latency as well, which is seconds and swamps
    # the pipe - the reason an earlier read of this was wrong by an
    # order of magnitude. Only the server can time just the transfer.
    t0 = time.perf_counter()
    # DEFAULT IS THE RENDER TARGET. 'other' selects the stale buffer of
    # the pair and is almost never what a comparison wants; the old
    # name for it was 'back', which read as the right choice and was
    # not. fbdump.py's docstring has what that cost.
    base, buf = grab_fb(fbdump, a.get('other'), a.get('addr'),
                        a.get('buffer'))
    took = time.perf_counter() - t0
    rgb = fbdump.to_rgb(buf)
    if a.get('addr') is not None:
        what = 'explicit address'
    elif a.get('buffer') is not None:
        what = 'buffer %d of 3' % int(a['buffer'])
    elif a.get('other'):
        what = 'OTHER buffer (stale)'
    else:
        what = 'render target'
    out = ['%s at %08x  (%d bytes in %.2fs over the pipe)'
           % (what, base, len(buf), took)]
    path = a.get('ppm')
    if path:
        fbdump.write_ppm(path, rgb)
        out.append('wrote %s' % path)
    path = a.get('raw')
    if path:
        with open(path, 'wb') as fh:
            fh.write(buf)
        out.append('wrote %s (%d bytes, 4 per pixel)' % (path, len(buf)))
    out.append(fbdump.ascii_art(rgb, int(a.get('cols') or 120)))
    return chr(10).join(out)


def grab_fb(fbdump, back, addr=None, index=None):
    if addr is not None:
        return fbdump.grab_at(dbg(), addr_of(addr))
    if index is not None:
        bases = fbdump.buffer_bases(dbg(), max(3, int(index) + 1))
        return fbdump.grab_at(dbg(), bases[int(index)])
    return fbdump.grab(dbg(), bool(back))


TOOLS = [
    ('amiga_ping', 'Is the emulator running, and what frame is it on?',
     {}, [], t_ping),
    ('amiga_regs', 'The 68k integer registers and PC.',
     {}, [], t_regs),
    ('amiga_read', 'Read guest memory as a hex dump.',
     {'addr': ADDR,
      'length': {'type': 'integer', 'description': 'bytes, max 4096'}},
     ['addr'], t_read),
    ('amiga_write', 'Write guest RAM. Refuses ROM and I/O.',
     {'addr': ADDR,
      'hex': {'type': 'string', 'description': 'bytes as hex, e.g. 00ff10'}},
     ['addr', 'hex'], t_write),
    ('amiga_watch',
     'Watch an address. By default the hit is LOGGED and the demo keeps '
     'running - set halt to stop instead. Read the hits with amiga_hits.',
     {'addr': ADDR,
      'length': {'type': 'integer', 'description': 'bytes, default 4'},
      'access': {'type': 'string', 'enum': ['read', 'write', 'both'],
                 'description': 'default write'},
      'halt': {'type': 'boolean', 'description': 'stop the CPU on a hit'}},
     ['addr'], t_watch),
    ('amiga_watch_clear', 'Remove a watch, or all of them.',
     {'slot': {'type': 'string', 'description': 'slot number, or "all"'}},
     [], t_watch_clear),
    ('amiga_hits',
     'Drain the watch log: which PC touched which address, with the frame '
     'number and the value written.',
     {'max': {'type': 'integer', 'description': 'records, default 256'}},
     [], t_hits),
    ('amiga_hits_clear',
     'Discard the whole watch log so the next amiga_hits returns only '
     'new records. Nothing empties the log but a read, so records left '
     'by an earlier investigation are what the next read hands back - '
     'with their old frame numbers, which reads as a plausible result. '
     'Call this after amiga_watch_clear and before arming the watch you '
     'actually care about. PAUSE FIRST: a running emulator keeps '
     'filling the log and the drain will not run dry.',
     {'max_records': {'type': 'integer',
                      'description': 'give up after this many records'},
      'max_rounds': {'type': 'integer',
                     'description': 'give up after this many reads'}},
     [], t_hits_clear),
    ('amiga_fb',
     'The 160x100 chunky framebuffer the demo renders into, as '
     'ASCII and '
     'optionally as a P6 or as raw four-byte pixels to diff against a '
     'src/ render. PAUSE FIRST - the read takes 17 pipe round trips and '
     'a running emulator will swap buffers underneath it. AND CHECK THE '
     'FRAME IS FINISHED: the demo renders every second VBL, so stopping '
     'on the wrong parity catches the front buffer part drawn, which '
     'reads as a plausible dark frame rather than as an error. THE '
     'SPACE SCENES ARE TRIPLE BUFFERED - pass buffer=0, 1 or 2; '
     'other= reaches only buffer 1 and leaves one frame in three '
     'unreadable.',
     {'addr': {'type': 'string',
               'description': 'read 160x100x4 from HERE instead of a '
                              'pointer - live hex, img:<hex> or a '
                              'label. The way to reach a buffer the '
                              'table does not name'},
      'buffer': {'type': 'integer',
                 'description': 'which of the THREE frame buffers, 0 '
                                'to 2. The space scenes are triple '
                                'buffered, so other= reaches only two '
                                'of them and one frame in three is '
                                'uncapturable without this'},
      'other': {'type': 'boolean',
               'description': 'buffer 1. Kept for old calls; prefer '
                              'buffer=, which can also reach 2'},
      'ppm': {'type': 'string',
              'description': 'write a P6 here as well as printing. THE '
                             'LIGHT CHANNEL IS DROPPED, so a glow is '
                             'invisible in it - use raw to compare one'},
      'raw': {'type': 'string',
              'description': 'write the four-byte pixels here, LIGHT '
                             'included, to diff against relic_ppm -raw. '
                             'THE ONLY WAY TO SEE A GLOW: every glow in '
                             'the demo is an addition to byte +2 and '
                             'leaves R, G and B untouched'},
      'cols': {'type': 'integer', 'description': 'ASCII width, default 120'}},
     [], t_fb),
    ('amiga_search',
     'Find a byte pattern in guest memory. The search runs inside the '
     'emulator, so an 8MB range costs one call rather than thousands. '
     'Pattern is hex byte pairs in MEMORY ORDER (a 68k longword is written '
     'big-endian, exactly as a dump shows it), and "??" matches any byte - '
     'use that to skip relocated addresses, which differ between the file '
     'on disk and the running image. Paging is handled for you. Only banks '
     'with host backing are touched, so the chipset registers are never '
     'read.',
     {'start': {'type': 'string',
                'description': 'live hex start, inclusive - or img:<hex> or '
                               'a label, same forms as addr elsewhere'},
      'end': {'type': 'string',
              'description': 'live hex end, exclusive - same forms'},
      'pattern': {'type': 'string',
                  'description': 'e.g. 2c79???????? or 00ff00ff. Spaces are '
                                 'stripped, so a run of bytes copied '
                                 'straight out of a hex dump works as-is.'},
      'align': {'type': 'integer',
                'description': 'power of two; 4 finds only longword-aligned '
                               'matches, which is what you want when hunting '
                               'pointers. Default 1.'},
      'limit': {'type': 'integer', 'description': 'max hits, default 256'}},
     ['start', 'end', 'pattern'], t_search),
    ('amiga_break',
     'Set a PC breakpoint, optionally CONDITIONAL. A hit stops the guest '
     'exactly at that address and leaves it paused with the pipe still live '
     '- check it with amiga_pause, then resume with amiga_continue. With '
     'when=, the hit only stops the guest if the condition holds, which is '
     'how to find the one interesting call out of hundreds through the same '
     'address in a single stop instead of twenty continue/read-registers '
     'round trips. NOTE: while any breakpoint is set the emulator runs much '
     'slower, because every instruction is checked - the condition itself is '
     'free, it is only looked at once the PC has already matched.',
     {'addr': ADDR,
      'when': {'type': 'string',
               'description': 'condition, terms comma-separated and ANDed, '
                              'no spaces. Left side: d0-d7, a0-a7, pc, sr, '
                              'or [s]b|w|l@<addr> for 1/2/4 bytes of guest '
                              'memory. Operators == != < <= > >= and : / !: '
                              'for lo..hi ranges. An "s" prefix makes the '
                              'load AND the compare signed, which the demo '
                              'camera globals need. NUMBERS ARE HEX, minus '
                              'sign included. Use {g_CamX} or {img:2e0b7a} '
                              'to put a label in: it is substituted with the '
                              'live address before sending, since the '
                              'emulator has no symbols. Examples: '
                              'd0==7fffffff | a1:200000..280000 | '
                              'sl@{g_TempleCamX}<0,d1!=0'},
      'skip': {'type': 'integer',
               'description': 'ignore this many hits that pass the condition '
                              'before stopping on one - the way to reach the '
                              '50th matching call without 49 resumes'}},
     ['addr'], t_break),
    ('amiga_break_list',
     'The breakpoints, with hits= (how often the PC was reached) and '
     'passed= (how often the condition held). THE PAIR IS THE DIAGNOSTIC: '
     'hits=412 passed=0 means the code runs and your condition is wrong, '
     'while hits=0 means it never runs at all - and before these counters '
     'both looked identical, as a breakpoint that never fires.',
     {}, [], t_break_list),
    ('amiga_break_clear', 'Remove a breakpoint, or all of them.',
     {'slot': {'type': 'string', 'description': 'slot number, or "all"'}},
     [], t_break_clear),
    ('amiga_continue', 'Resume after a halt or a pause.', {}, [], t_cont),
    ('amiga_pause',
     'Get the emulator state, and optionally set it, in one call. Answers '
     'running, paused, or halted with the PC and breakpoint that stopped it. '
     'Reads and writes work while paused - it is the best time to inspect '
     'memory, since nothing moves underneath you.',
     {'set': {'type': 'string', 'enum': ['on', 'off', 'toggle'],
              'description': 'omit to just query'}}, [], t_pause),
    ('amiga_frames',
     'Run exactly n emulated frames, then pause - the way to land on a '
     'chosen frame, since timing an unpause cannot. REPORTS THE FRAME NUMBER '
     'BEFORE AND AFTER, and says so loudly if the two do not differ by n. '
     'The countdown itself is exact, so a mismatch means either a halt '
     'interrupted it - the state then reads "owed=<k>", and those k frames '
     'run on your next amiga_continue - or the wait timed out while the run '
     'carried on. Do not trust a landing you have not seen the two frame '
     'numbers for.',
     {'n': {'type': 'integer', 'description': 'frames to run, default 1'}},
     [], t_frames),
    ('amiga_advance_until',
     'Run emulated frames until a condition holds, then pause - capped, so '
     'a condition that never holds cannot run the demo off the end. THE WAY '
     'TO REACH AN INTERESTING FRAME whose number you do not know: before '
     'this, that meant a hand-run binary search of amiga_frames and '
     'amiga_read at a round trip each. The condition is CHECKED ONCE A '
     'FRAME, so a value the demo writes and overwrites within one frame is '
     'invisible to it - use a conditional amiga_break for that. Says which '
     'of the two things happened, because a hit and an expiry both just '
     'leave the guest paused.',
     {'condition': {'type': 'string',
                    'description': 'same grammar as amiga_break when=, plus '
                                   'changed@<addr>:<len>, which is true once '
                                   'the checksum of that span differs from '
                                   'when the command was issued - how to '
                                   'wait for the framebuffer to be rewritten '
                                   'without knowing what it will hold. '
                                   '{g_CamX} and {img:<hex>} are substituted '
                                   'with live addresses. Examples: '
                                   'sl@{g_TempleCamX}:-100..100 | '
                                   'w@{img:2e0bf6}==4 | changed@583220:fa00'},
      'max_frames': {'type': 'integer',
                     'description': 'frames to allow, DECIMAL here, default '
                                    '300 (six seconds of PAL)'}},
     ['condition'], t_advance_until),
    ('amiga_hits_count',
     'Tallies from the watch log instead of records: which PC touched the '
     'watched address and HOW MANY TIMES EACH, biggest first. One line for '
     'what a drain answers in nine hundred near-identical rows. COUNTED ON '
     'THE CPU PATH as the hits arrive, so the total is exact even where '
     'amiga_hits would have lost records to the 4096-slot ring - which a '
     'watch on a per-face global overruns inside one frame. Does not drain: '
     'amiga_hits still returns the records afterwards.',
     {'by': {'type': 'string', 'enum': ['pc', 'val', 'addr'],
             'description': 'what to count by, default pc. val answers '
                            '"which values does it take"; addr matters when '
                            'the watch covers a range.'},
      'limit': {'type': 'integer',
                'description': 'keys to print, default 32'}},
     [], t_hits_count),
    ('amiga_fingerprint',
     'WHICH FRAME IS THIS? Reads the globals that identify the running '
     'scene and returns them labelled, with the matching field name from '
     "our own stats() line beside each. Derives the hunk slide itself - the "
     'caller never does the UV11 search - and every address in '
     'docs/fingerprints.txt stays an image address so the table survives a '
     're-analysis. PAUSE FIRST: a fingerprint is several reads and a running '
     'demo moves between them, which produces a reading that is internally '
     'inconsistent and looks fine. It reports the frame before and after and '
     'says so loudly if they differ. Without scene=, it reads the selector '
     'globals and names the scene only if exactly one block matches - '
     'otherwise it reports the raw selectors rather than guessing, because a '
     'guessed scene sends you to the wrong row.',
     {'scene': {'type': 'string',
                'description': 'a block name from docs/fingerprints.txt, '
                               'e.g. temple, window_cam, window_pullback. '
                               'Omit to let it try to work out which.'}},
     [], t_fingerprint),
    ('amiga_symbols',
     'Search the demo label table - name, image address and hunk for every '
     'named global, generated from Ghidra into '
     'reverse/exports/relic/symbol_map.txt. Touches no emulator. These are '
     'IMAGE addresses; pass a name straight to amiga_read or use {name} in a '
     'condition and the live address is worked out for you.',
     {'match': {'type': 'string',
                'description': 'case-insensitive substring, e.g. "cam" or '
                               '"temple"'},
      'limit': {'type': 'integer', 'description': 'rows, default 40'}},
     [], t_symbols),
    ('amiga_slide',
     "A hunk's live slide, derived from two independent anchors that have to "
     'agree before it is reported. AmigaDOS allocates every hunk separately '
     'so there is ONE SLIDE PER HUNK, and a savestate restores the layout of '
     'the run it was taken in - two states from different runs have differed '
     'by 0x20 in CODE_03 alone. Nothing caches this across a state load '
     'without re-verifying it.',
     {'hunk': {'type': 'string',
               'description': 'CODE_00, CODE_02, CODE_03, CODE_05 or '
                              'CODE_06. Default CODE_03, where the demo '
                              'globals live. The two BSS hunks have no file '
                              'bytes, so there is no anchor to search for '
                              'and this refuses them rather than guessing.'}},
     [], t_slide),
    ('amiga_state_save',
     'Save a savestate slot. The same slots Shift+numpad 1-6 write, so a '
     'state saved here can be reloaded from the keyboard.',
     {'slot': SLOT}, [], t_state_save),
    ('amiga_state_load',
     'Restore a savestate slot, and wait until the state is really in '
     'memory before answering - which is not the same moment the emulator '
     'accepts the command. Reading on the reply used to hand back the '
     'memory from BEFORE the load, indefinitely rather than one frame '
     'late: it reads as a successful load of a state that looks like the '
     'last one. NOTE THAT A RESTORE ALSO RESTORES THE FRAME COUNTER - it '
     'is in the state file, so amiga_ping goes BACKWARDS across a load '
     'and two loads of one slot land on the same number. It is not a '
     'clock, and it stamps every record in the watch log. The counter is '
     'ZEROED by a restore rather than restored from the file, so it means '
     '"frames since the last load".',
     {'slot': SLOT}, [], t_state_load),
    ('amiga_snap_save',
     "Save a savestate in the MCP's OWN namespace, slot 0-1000, with a "
     "label. These are mcp_NNNN.uss files beside the numpad own "
     'default_N.uss, so a save here can never overwrite one of the ten '
     'slots a human marked by hand - which amiga_state_save can, since it '
     'shares those files with the numpad keys. Use this for sweeps.',
     {'slot': {'type': 'integer', 'description': '0-1000'},
      'label': {'type': 'string',
                'description': 'short description, listed by '
                               'amiga_snap_list; the frame number is '
                               'recorded for you'}},
     ['slot'], t_snap_save),
    ('amiga_snap_load',
     'Restore an MCP snapshot by slot, waiting until the restore has '
     'really landed before answering - same contract as amiga_state_load.',
     {'slot': {'type': 'integer', 'description': '0-1000'}},
     ['slot'], t_snap_load),
    ('amiga_snap_list',
     'What the MCP snapshot namespace holds: slot, the frame it was taken '
     'on, and the label. Read from mcp_index.txt beside the states, so it '
     'costs no restores and survives a restart.',
     {'start': {'type': 'integer', 'description': 'first slot to list'},
      'count': {'type': 'integer', 'description': 'how many to list'}},
     [], t_snap_list),
    ('amiga_warp',
     'DISABLED, and it only queries or turns warp OFF. Warping starves the '
     'demo scene of CPU cycles while its music keeps absolute time, because '
     'with cpu_speed=max the per-frame cycle budget is wall-clock derived '
     'and WinUAE gates that budgeting on not being in warp '
     '(custom.cpp:8709). Scene state diverges: the swirl fuse read 412 at '
     'normal speed against 476 warped over the same 2901 frames. Sweep at '
     'normal speed instead - amiga_frames 500 costs 10 s of wall clock.',
     {'set': {'type': 'string', 'enum': ['off'],
              'description': 'omit to query; off is the only setting left'}},
     [], t_warp),
    ('amiga_reset', 'Reboot the emulated Amiga. Soft by default.',
     {'hard': {'type': 'boolean', 'description': 'power-cycle rather than '
               'a keyboard reset'}}, [], t_reset),
    ('amiga_raw', 'Send a raw protocol line. For anything not wrapped above.',
     {'command': {'type': 'string'}}, ['command'], t_raw),
]

HANDLERS = dict((name, fn) for name, _d, _p, _r, fn in TOOLS)
SCHEMA = dict((name, set(props)) for name, _d, props, _r, _fn in TOOLS)


def check_args(name, args):
    """Refuse an argument the tool does not have.

    A MISSPELLED KEY USED TO BE IGNORED, AND THE CALL STILL SAID OK.
    amiga_fb takes other/ppm/raw; called with which='back' and out=...
    it answered 'front buffer' and wrote no file, which reads as a
    successful grab of the wrong buffer. Every silent one of those is a
    measurement that has to be thrown away later, so an unknown key is
    an error here rather than a surprise three steps downstream.
    """
    bad = sorted(set(args) - SCHEMA.get(name, set()))
    if not bad:
        return None
    ok = sorted(SCHEMA.get(name, set()))
    return ('%s has no argument %s - it takes %s'
            % (name, ', '.join(repr(b) for b in bad),
               ', '.join(ok) if ok else 'no arguments'))


def tool_list():
    return [{
        'name': name,
        'description': desc,
        'inputSchema': {'type': 'object', 'properties': props, 'required': req},
    } for name, desc, props, req, _fn in TOOLS]


# ------------------------------------------------------------ transport

def reply(msg_id, result=None, error=None):
    m = {'jsonrpc': '2.0', 'id': msg_id}
    if error is not None:
        m['error'] = error
    else:
        m['result'] = result
    sys.stdout.write(json.dumps(m) + '\n')
    sys.stdout.flush()


def handle(msg):
    method = msg.get('method')
    msg_id = msg.get('id')

    if method == 'initialize':
        reply(msg_id, {
            'protocolVersion': PROTOCOL,
            'capabilities': {'tools': {}},
            'serverInfo': {'name': 'amiga-dbg', 'version': '0.1'},
        })
        return
    if method in ('notifications/initialized', 'initialized'):
        return                                  # notification, no reply
    if method == 'tools/list':
        reply(msg_id, {'tools': tool_list()})
        return
    if method == 'tools/call':
        params = msg.get('params') or {}
        name = params.get('name')
        args = params.get('arguments') or {}
        fn = HANDLERS.get(name)
        if not fn:
            reply(msg_id, error={'code': -32601,
                                 'message': 'no tool %r' % name})
            return
        bad = check_args(name, args)
        if bad:
            reply(msg_id, {'content': [{'type': 'text', 'text': bad}],
                           'isError': True})
            return
        try:
            text = fn(args)
            is_err = False
        except NotRunning as e:
            text = ('WinUAE is not answering: %s\n'
                    'Start a config in WinUAE - the pipe opens on the first '
                    'emulated frame, not when the GUI appears.' % e)
            is_err = True
        except Exception as e:          # a bad address must not kill us
            text = '%s: %s' % (type(e).__name__, e)
            is_err = True
        reply(msg_id, {'content': [{'type': 'text', 'text': text}],
                       'isError': is_err})
        return
    if msg_id is not None:
        reply(msg_id, error={'code': -32601,
                             'message': 'no method %r' % method})


def selftest():
    names = [t[0] for t in TOOLS]
    assert len(names) == len(set(names)), 'duplicate tool name'
    for name, desc, props, req, fn in TOOLS:
        assert desc and callable(fn), name
        for r in req:
            assert r in props, (
                '%s requires %s but does not declare it' % (name, r))
    print('%d tools, schemas consistent' % len(TOOLS))
    print('  ' + ', '.join(names))
    d = Dbg(timeout=0.4)
    try:
        print('emulator: %s' % d.ping())
    except NotRunning as e:
        print('emulator: not running (%s)' % str(e).split('(')[0].strip())
    return 0


def main():
    if '--selftest' in sys.argv:
        return selftest()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        handle(msg)
    return 0


if __name__ == '__main__':
    sys.exit(main())
