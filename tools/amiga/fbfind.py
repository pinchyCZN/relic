"""Which tick of ours is this savestate? Match the CAMERA, not a clock.

    py -3.10 tools/amiga/fbfind.py --cam 1460,3538 --heading 2733
    py -3.10 tools/amiga/fbfind.py --cam 1460,3538 --part 14 --ticks 1200
    py -3.10 tools/amiga/fbfind.py --match pull=16255 --part 6
    py -3.10 tools/amiga/fbfind.py --cam 1460,3538 --dump shot1.raw

Reads the camera out of a WinUAE savestate FIRST - with the MCP, since
the debug pipe takes one client - then asks this for the tick that
produces it:

    amiga_read addr=g_TempleCamX len=8     camX, camZ
    amiga_read addr=g_TempleHeading len=4  heading

THE SPACE SCENES PAIR ON THE CAMERA TOO, not on g_CamPathCursor.
The cursor LEADS the camera by one step: at a halt reading cursor 90
the demo's camera is the one our tick at cursor 88 produces, and the
cursor walks two a tick, so a cursor pairing lands a tick late. Taken
as the key it has been three ticks out - off savestate 4 the cursor
says our tick 162 while the camera says 159, and the camera is right.
The cursor is still what seeks to the neighbourhood: it is at image
0x002e0c8e, CODE_03, and walks two a tick from zero at the handover
into mode 5, so a demo cursor of 36 is 18 ticks past it.

AND THE DEMO'S RENDER TARGET CAN BE HALF DRAWN. It is TRIPLE
buffered in the space scenes - g_pFrameBuffer at image 0x002f101c and
the two longwords after it, three bases 0xfa00 apart - and which one
holds a finished frame depends on where the pause landed. The target
read at savestate 3 was visibly scrambled while another held the
picture. Take them by index, amiga_fb buffer=0, 1 or 2, and look
before scoring one; other= reaches buffer 1 alone and leaves one
frame in three unreadable.

PAIR ON THE CAMERA, NEVER ON g_TempleCamTime. The clock and the camera
do not advance together: the demo writes the clock first and evaluates
with the previous value, savestate 7 pairs five ticks away from where
its clock puts it, and a breakpoint halt is a whole frame past the
state that was saved. Four separate wrong conclusions have come out of
pairing on the clock, and none out of pairing on the camera - a tick
whose x, z and heading all match is the frame, every time.

AND READ THE CAMERA AT THE HALT when a breakpoint stopped the demo.
The state in the slot is one frame behind what the walkers are using,
which once made a correct ray direction of 258 look like a wrong 248.

--match TAKES ANY FIELD THE STATS LINE PRINTS, which is how a part
with no camera is paired at all. A field is a name followed by a
number - "pull 16255/32768" is pull, "cam 1,2,3" is cam - and a
predicate may name as many components as the line carries:

    --match pull=16255          the first component
    --match cam=1460,-200,3538  all three
    --match key=4.0a            matched as TEXT, so the hex low byte
                                in "key %ld.%02lx" needs no decoding

Several --match are ANDed, and --cam and --heading are sugar for the
two that were here first. A predicate is scored, not filtered, so a
near miss still prints with its distance - an exact pairing says
EXACT and nothing else does.

THE STATS LINE IS THE PART'S OWN AND NO TWO AGREE ON IT, so a field
exists only if that part prints it. temple, window, orb, release and
underside print a camera; approach, earth, explosion, logo, portal,
sand and sun print only derived values; part_term prints nothing.
A predicate naming a field the part does not print can never match
and says so rather than scoring every tick equally.

This renders the part from its first tick, because a part with
per-tick state cannot be seeked into.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

EXE = 'build/Win32/Debug/relic_ppm.exe'
W, H = 160, 100
RAW_BYTES = W * H * 4

# THE HANDED-OVER FRAME IS NOT THE STAGE TO SCORE FOR EVERY PART. A
# part that post-processes on its way out gives -raw a frame two
# stages past the one the demo's own buffer holds, so --dump takes
# those from the agent's view of the part's internal buffer instead.
# Everything not named here hands over what it drew and -raw is right.
# See tools/amiga/fbcmp.py and docs/subsystems/agent_mcp.txt 5.3.
RAW_WRONG = {
    14: ('g_frame#part_temple',
         'the ruins blur and fog into the handed-over frame'),
}

R_TICK = re.compile(r'tick +(\d+)')
# A FIELD IS A NAME THEN A NUMBER. The optional letter before the
# digits is for the "r%ld" and "x%ld" the parts write for a radius or
# an axis, so "glow r5" is glow=5 rather than nothing at all.
R_FIELD = re.compile(
    r'([A-Za-z_][A-Za-z_0-9]*)[ \t]+'
    r'([A-Za-z]?-?\d[0-9a-fA-F]*(?:[,/.]-?[0-9a-fA-F]+)*)')
R_SPLIT = re.compile(r'[,/.]')
# --heading predates --match and the parts spell it two ways.
ALIAS = {'heading': ('heading', 'hd')}
MISSING = 1 << 30


def numbers(raw):
    """-> the components of a value, base 10 where it parses, else 16.

    "4.0a" IS DECIMAL THEN HEX in the same value - that is how the
    parts print a key - so a component is tried both ways rather than
    the whole value being given one base.
    """
    out = []
    for part in R_SPLIT.split(raw.lstrip('ABCDEFGHIJKLMNOPQRSTUVWXYZ'
                                        'abcdefghijklmnopqrstuvwxyz')):
        if not part:
            continue
        for base in (10, 16):
            try:
                out.append(int(part, base))
                break
            except ValueError:
                pass
    return out


def parse_fields(line):
    """-> {name: (raw text, [components])} for one stats line."""
    out = {}
    for m in R_FIELD.finditer(line):
        name, raw = m.group(1), m.group(2)
        if name not in out:
            out[name] = (raw, numbers(raw))
    return out


def lookup(fields, name):
    """A field by name, or by the names a part might spell it."""
    for key in ALIAS.get(name, (name,)):
        if key in fields:
            return fields[key]
    return None


def predicate_cost(fields, name, raw, want, pick):
    """How far one row is from one predicate. MISSING if it cannot be.

    pick is the component indices to compare, which is what lets
    --cam X,Z mean x and z of a three-component camera.
    """
    got = lookup(fields, name)
    if got is None:
        return MISSING
    if pick is None and raw is not None and got[0] == raw:
        return 0
    have = got[1]
    if pick is None:
        if len(have) < len(want):
            return MISSING
        return sum(abs(have[i] - want[i]) for i in range(len(want)))
    if len(have) <= max(pick):
        return MISSING
    return sum(abs(have[pick[k]] - want[k]) for k in range(len(want)))


def sweep(exe, part, ticks, out, raw):
    """Render the part from tick 0 and parse what each tick prints."""
    # CreateProcess wants a native path; a forward-slash relative one
    # comes back as "cannot find the file specified".
    exe = os.path.normpath(os.path.abspath(exe))
    cmd = [exe, 'reverse/ghidra/relic', out,
           '-dat', 'reverse/ghidra/relic.dat',
           '-p', str(part), '-t', '0', '-n', str(ticks),
           '-e', '1', '-s', '1']
    if raw:
        cmd.append('-raw')
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    text = p.communicate()[0].decode('latin-1')
    rows = []
    for line in text.splitlines():
        t = R_TICK.search(line)
        if not t:
            continue
        fields = parse_fields(line)
        # ONLY THE STATS LINE IS A TICK ROW. The host used to print the
        # file it wrote as "tick 5 -> dir/p14_t00005.ppm", which carries
        # a tick too and so paired every tick twice, the duplicate
        # scoring zero against a tick= predicate; it now says "wrote
        # <path>". Keep the guard: a row with nothing to match on is of
        # no use here whatever the host's output grows into.
        if len(fields) < 2:
            continue
        rows.append((int(t.group(1)), fields, line.strip()))
    return rows


def agent_dump(part, tick, sym, path):
    """The part's own buffer, through the debug pipe.

    ONE CLIENT AT A TIME. The MCP server keeps its connection from
    its first call, so this cannot open the pipe while an MCP session
    is live - and that is the normal state. It fails with the verbs
    to run instead rather than pretending the capture happened.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, os.pardir, 'agent'))
    import agent as pipe

    def verb(line):
        reply = pipe.send(line)
        if 'ERR_' in reply:
            raise SystemExit('agent refused %r:\n%s' % (line, reply))
        return reply

    verb('GOTO part=%d' % part)
    # RUN_TO IS BOUNDED TWICE, by ticks and by wall clock, and reports
    # truncated=1 when either cut it short. Re-asking continues from
    # where it stopped; it cannot go backwards, so this converges.
    for _ in range(64):
        reply = verb('RUN_TO tick=%d' % tick)
        if 'truncated=1' not in reply:
            break
    else:
        raise SystemExit('RUN_TO kept truncating short of tick %d' % tick)
    verb('DUMP_MEM sym=%s deref=1 len=%d path=%s'
         % (sym, RAW_BYTES, os.path.abspath(path)))


def capture(part, tick, out, path):
    """The matched tick's four-byte pixels, at the right stage."""
    wrong = RAW_WRONG.get(part)
    if wrong is None:
        src = os.path.join(out, 'p%d_t%05d.raw' % (part, tick))
        if not os.path.exists(src):
            raise SystemExit('the sweep wrote no %s' % src)
        shutil.copyfile(src, path)
        print('wrote %s from -raw, %d bytes' % (path, os.path.getsize(path)))
        return
    sym, why = wrong
    print('part %d: -raw is the wrong stage - %s' % (part, why))
    try:
        agent_dump(part, tick, sym, path)
    except SystemExit:
        raise
    except Exception as e:
        # BUSY AND ABSENT NEED DIFFERENT ANSWERS. Absent is fixed
        # by starting a host; busy means an MCP session owns the
        # pipe and no amount of re-running will take it, so the
        # verbs to run THERE are the answer instead.
        hint = ('start a host and re-run:\n'
                '    %s reverse/ghidra/relic <out-dir>'
                ' -dat reverse/ghidra/relic.dat -agent' % EXE)
        if 'busy' in str(e).lower():
            hint = ('an MCP session holds the pipe from its first'
                    ' call, so run these through the MCP instead:')
        raise SystemExit(
            'no capture: %s\n%s\n'
            '    relic_goto part=%d\n'
            '    relic_run_to tick=%d\n'
            '    relic_dump_mem sym=%s deref=1 len=%d path=%s'
            % (e, hint, part, tick, sym, RAW_BYTES, path))
    n = os.path.getsize(path) if os.path.exists(path) else 0
    print('wrote %s from %s, %d bytes' % (path, sym, n))


def main(argv):
    preds = []
    part, ticks, exe, dump = 14, 1000, EXE, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--cam' and i + 1 < len(argv):
            want = [int(v, 0) for v in argv[i + 1].split(',')]
            if len(want) not in (2, 3):
                raise SystemExit('--cam takes X,Z or X,Y,Z')
            pick = (0, 1, 2) if len(want) == 3 else (0, 2)
            preds.append(('cam', None, want, pick))
            i += 2
        elif a == '--heading' and i + 1 < len(argv):
            preds.append(('heading', None, [int(argv[i + 1], 0)], None))
            i += 2
        elif a == '--match' and i + 1 < len(argv):
            if '=' not in argv[i + 1]:
                raise SystemExit('--match wants NAME=VALUE')
            name, raw = argv[i + 1].split('=', 1)
            want = numbers(raw)
            if not want:
                raise SystemExit('--match %s has no number in it' % name)
            preds.append((name.strip(), raw.strip(), want, None))
            i += 2
        elif a == '--part' and i + 1 < len(argv):
            part = int(argv[i + 1], 0)
            i += 2
        elif a == '--ticks' and i + 1 < len(argv):
            ticks = int(argv[i + 1], 0)
            i += 2
        elif a == '--exe' and i + 1 < len(argv):
            exe = argv[i + 1]
            i += 2
        elif a == '--dump' and i + 1 < len(argv):
            dump = argv[i + 1]
            i += 2
        else:
            raise SystemExit(__doc__)
    if not preds:
        raise SystemExit(__doc__)

    keep = dump is not None and part not in RAW_WRONG
    out = tempfile.mkdtemp(prefix='fbfind')
    try:
        rows = sweep(exe, part, ticks, out, keep)
        if not rows:
            raise SystemExit('no labelled ticks - does part %d print a'
                             ' stats line?' % part)

        def cost(r):
            return sum(predicate_cost(r[1], *p) for p in preds)

        # A FIELD THE PART NEVER PRINTS IS A STOP, not a tie. Scored,
        # every tick would be equally far from it and the first would
        # be reported as the answer.
        for name, _, _, _ in preds:
            if all(lookup(r[1], name) is None for r in rows):
                raise SystemExit(
                    'part %d prints no %r - its fields are: %s'
                    % (part, name,
                       ' '.join(sorted(rows[0][1])) or '(none)'))

        rows.sort(key=cost)
        best = cost(rows[0])
        hits = [r for r in rows if cost(r) == best]
        print('%d tick(s) at distance %d%s'
              % (len(hits), best, '  EXACT' if best == 0 else ''))
        for r in hits[:8]:
            print('  ' + r[2])
        if best:
            print('  nearest misses:')
            for r in rows[len(hits):len(hits) + 3]:
                print('    d=%-4d %s' % (cost(r), r[2]))
        if dump is not None:
            if len(hits) > 1:
                print('  %d ticks tie - dumping the first, %d'
                      % (len(hits), hits[0][0]))
            capture(part, hits[0][0], out, dump)
        return 0 if best == 0 else 1
    finally:
        for f in os.listdir(out):
            os.remove(os.path.join(out, f))
        os.rmdir(out)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
