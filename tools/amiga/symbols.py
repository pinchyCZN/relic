#!/usr/bin/env python
"""Labels, hunk slides, and the per-scene fingerprint table.

    py -3.10 tools/amiga/symbols.py --selftest
    py -3.10 tools/amiga/symbols.py g_CamX          resolve one label
    py -3.10 tools/amiga/symbols.py --scenes        what the table knows

WHY THIS EXISTS. Every question about the running demo has been asked in
image addresses - the ones Ghidra shows and docs/ quotes - and answered
in live ones, with the difference worked out by hand each time. That
difference is not a constant and not even one number: AmigaDOS allocates
every hunk separately, so there is ONE SLIDE PER HUNK, and a savestate
restores the layout of the run it was taken in. Two states taken in
different sessions have differed by 0x20 in CODE_03 alone.

So nothing here hardcodes a slide. Each one is derived against the live
machine and verified, and a label resolves through its own hunk's.

THREE SOURCES, AND THEY ARE DELIBERATELY NOT MERGED INTO ONE FILE:

  reverse/exports/relic/symbol_map.txt   name -> image address + hunk.
      Generated from Ghidra - every named data global. Not edited by
      hand: a name that belongs here belongs in Ghidra first.

  tools/hunk/imgaddr.py                  the hunk image/file map.
      Already the one place that knows where a hunk starts.

  docs/fingerprints.txt                  which globals identify a frame.
      The only hand-written part, because it is the only part that is a
      judgement: of several hundred globals, which three say "this is
      the frame you are looking at". See that file's own header.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(_HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'hunk'))

import imgaddr                                              # noqa: E402

SYMBOL_MAP = os.path.join(ROOT, 'reverse', 'exports', 'relic',
                          'symbol_map.txt')
FINGERPRINTS = os.path.join(ROOT, 'docs', 'fingerprints.txt')
BINARY = os.path.join(ROOT, 'reverse', 'ghidra', 'relic')

# The live image has always landed in fast RAM in this range. Used only to
# reject a derived slide that is obviously wrong, never to guess one.
LIVE_LO, LIVE_HI = 0x200000, 0x1000000

SIZES = {'b': 1, 'w': 2, 'l': 4}


class SymbolError(Exception):
    pass


# ------------------------------------------------------------- symbol map

def load_symbols(path=SYMBOL_MAP):
    """name -> dict(addr, size, type, hunk), from the Ghidra export.

    DUPLICATE NAMES AT ONE ADDRESS ARE REAL - g_SceneDrawOrder and
    g_SceneSortList are both 0x0033caf0 - so this is keyed by name and
    two names can share an address. The reverse lookup below keeps every
    name for an address rather than picking one.
    """
    out = {}
    with open(path, 'r') as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            f = line.split(',')
            if len(f) < 7:
                continue
            addr, name, typ, _count, size, _src, hunk = f[:7]
            try:
                out[name] = {
                    'addr': int(addr, 16),
                    'size': int(size, 16) if size.startswith('0x') else 0,
                    'type': typ,
                    'hunk': hunk,
                }
            except ValueError:
                continue
    if not out:
        raise SymbolError('no symbols parsed from %s' % path)
    return out


def hunk_of(addr):
    """Which hunk an image address is in, from the one table that knows."""
    seg = imgaddr.seg_of_addr(addr)
    if seg is None:
        raise SymbolError('0x%08x is outside every hunk' % addr)
    return seg[0]


# ------------------------------------------------------- the live slides

ANCHOR_LEN = 24
ANCHOR_TRIES = 40
ANCHOR_STEP = 32        # host-side scan stride; see _anchor_candidates 


def _file_bytes():
    with open(BINARY, 'rb') as fh:
        return fh.read()


def _anchor_candidates(blob, hunk):
    """4-aligned ANCHOR_LEN windows in <hunk> worth searching for.

    THE FILE IS NOT RELOCATED, so a window holding one of the demo's
    6,605 relocations has different bytes live and will simply not be
    found. That is fine and is why this yields many candidates rather
    than choosing cleverly: a window that misses is skipped and the next
    one tried, which needs no relocation table and cannot be fooled by
    one. At roughly one relocation per 290 bytes most windows are clean.

    Windows are ordered by byte variety, because the other failure is a
    window of padding or of a repeating table, which matches in a dozen
    places and identifies nothing.
    """
    seg = None
    for name, lo, hi, flo in imgaddr.SEGMENTS:
        if name == hunk:
            seg = (lo, hi, flo)
            break
    if seg is None:
        raise SymbolError('no hunk named %s' % hunk)
    lo, hi, flo = seg
    if flo is None:
        raise SymbolError(
            '%s is BSS: it has no file bytes, so there is no anchor to '
            'search for and its slide cannot be derived this way' % hunk)

    span = hi - lo + 1
    out = []
    # SCAN THE WHOLE HUNK, not a handful of probes. CODE_05 is largely
    # tables and yielded three usable windows out of 160 widely spaced
    # probes - one short of the two-anchor agreement this needs, which is
    # a derivation that fails on a hunk that is perfectly findable. The
    # scan is a megabyte or two of host memory and costs nothing.
    off = 0
    while off + ANCHOR_LEN <= span:
        w = blob[flo + off:flo + off + ANCHOR_LEN]
        variety = len(set(w))
        if variety >= 6 and w.count(0) <= ANCHOR_LEN // 2:
            out.append((variety, lo + off, w))
        off += ANCHOR_STEP
    # Most varied first: the other way to fail is a window of padding or
    # of a repeating table, which matches in a dozen places and so fixes
    # nothing. Then spread the chosen few across the hunk, so the two
    # that have to agree are not neighbours in the same table.
    out.sort(key=lambda t: -t[0])
    out = out[:ANCHOR_TRIES * 8]
    out.sort(key=lambda t: t[1])
    stride = max(1, len(out) // ANCHOR_TRIES)
    return [(a, w) for _v, a, w in out[::stride]][:ANCHOR_TRIES]


class Live(object):
    """Image addresses to live ones, against one open Dbg.

    One instance per connection. Slides are cached but ALWAYS VERIFIED
    before reuse - a savestate restore brings its own run's layout with
    it, so a cached slide is a guess about a machine that may have been
    replaced since. Verifying costs one read; deriving costs a search.
    """

    def __init__(self, d):
        self.d = d
        self._slide = {}        # hunk -> (slide, anchor_image_addr, bytes)
        self._bss = {}          # hunk -> (slide, live site, bytes there)
        self._blob = None
        self._syms = None

    @property
    def symbols(self):
        if self._syms is None:
            self._syms = load_symbols()
        return self._syms

    @property
    def blob(self):
        if self._blob is None:
            self._blob = _file_bytes()
        return self._blob

    def _seg_lo(self, hunk):
        for name, lo, _hi, _flo in imgaddr.SEGMENTS:
            if name == hunk:
                return lo
        raise SymbolError('no hunk named %s' % hunk)

    def _slide_bss(self, hunk):
        """A BSS hunk's slide, taken from the relocations into it.

        A BSS HUNK HAS NO FILE BYTES AND SO NO ANCHOR TO SEARCH FOR.
        What it does have is every pointer the loader patched into it:
        the file holds the offset within this hunk, the machine holds
        that offset plus this hunk's base, and the patch site sits in a
        CODE hunk whose own slide is derivable the ordinary way. The
        difference between the two longwords IS the base.

        The same two-agreement rule applies. A single site could be
        read through a wrong CODE slide, or be a longword the demo has
        since overwritten with something of its own, and either would
        give a confident wrong answer.
        """
        got = self._bss.get(hunk)
        if got is not None:
            slide, live_site, want = got
            try:
                if self.d.read(live_site, 4) == want:
                    return slide
            except (ValueError, OSError):
                pass
            del self._bss[hunk]

        lo = self._seg_lo(hunk)
        found = []
        for image_addr, file_off, dest in imgaddr.reloc_sites(self.blob):
            if dest != hunk:
                continue
            try:
                live_site = image_addr + self.slide(hunk_of(image_addr))
                raw = self.d.read(live_site, 4)
            except (SymbolError, ValueError, OSError):
                continue
            live_val = int.from_bytes(raw, 'big')
            file_val = int.from_bytes(
                self.blob[file_off:file_off + 4], 'big')
            base = (live_val - file_val) & 0xFFFFFFFF
            if not (LIVE_LO <= base < LIVE_HI):
                continue
            found.append((base - lo, live_site, raw))
            if len(found) >= 2:
                break
        if not found:
            raise SymbolError(
                'cannot locate %s live - no relocation into it read back '
                'a plausible address. Is the demo running?' % hunk)
        if len(found) < 2:
            raise SymbolError(
                'only one relocation into %s read back a plausible address '
                '(slide %+#x) and one is not proof. Refusing to report it.'
                % (hunk, found[0][0]))
        if found[0][0] != found[1][0]:
            raise SymbolError(
                '%s relocations disagree: %+#x and %+#x. Something is '
                'wrong - do not trust either.'
                % (hunk, found[0][0], found[1][0]))
        self._bss[hunk] = found[0]
        return found[0][0]

    def _is_bss(self, hunk):
        for name, _lo, _hi, flo in imgaddr.SEGMENTS:
            if name == hunk:
                return flo is None
        return False

    def slide(self, hunk):
        """<hunk>'s live base minus its image base, derived and checked."""
        if self._is_bss(hunk):
            return self._slide_bss(hunk)
        got = self._slide.get(hunk)
        if got is not None:
            slide, aaddr, abytes = got
            try:
                if self.d.read(aaddr + slide, len(abytes)) == abytes:
                    return slide
            except (ValueError, OSError):
                pass
            del self._slide[hunk]       # the machine moved; derive again

        found = []          # (slide, anchor addr, anchor bytes)
        for aaddr, abytes in _anchor_candidates(self.blob, hunk):
            pattern = abytes.hex()
            try:
                hits, _complete = self.d.search(LIVE_LO, LIVE_HI, pattern,
                                                align=4, limit=4)
            except (ValueError, OSError):
                continue
            if len(hits) != 1:
                # 0 hits: the window held a relocation. More than 1: it
                # was not unique. Either way it cannot fix a slide.
                continue
            found.append((hits[0] - aaddr, aaddr, abytes))
            # TWO INDEPENDENT ANCHORS MUST AGREE. One unique match is
            # not proof: it could be a window that happens to appear
            # once somewhere unrelated. Two windows from different parts
            # of the hunk giving the same slide is.
            if len(found) >= 2:
                break
        if not found:
            raise SymbolError(
                'cannot locate %s live - no anchor matched. Is the demo '
                'running, and is this the build reverse/ghidra/relic came '
                'from?' % hunk)
        if len(found) < 2:
            raise SymbolError(
                'only one anchor matched for %s (slide %+#x) and one is '
                'not proof - it could be an unrelated coincidence. Refusing '
                'to report a slide on it.' % (hunk, found[0][0]))
        if found[0][0] != found[1][0]:
            raise SymbolError(
                '%s anchors disagree: %+#x from %08x, %+#x from %08x. '
                'Something is wrong - do not trust either.'
                % (hunk, found[0][0], found[0][1],
                   found[1][0], found[1][1]))

        slide = found[0][0]
        lo = imgaddr.seg_of_addr(found[0][1])[1]
        if not (LIVE_LO <= lo + slide < LIVE_HI):
            raise SymbolError(
                '%s would land at %08x, outside %06x..%06x - rejecting'
                % (hunk, lo + slide, LIVE_LO, LIVE_HI))
        self._slide[hunk] = found[0]
        return slide

    def live(self, image_addr):
        """An image address as it is in the running machine."""
        return image_addr + self.slide(hunk_of(image_addr))

    def resolve(self, what):
        """A label or a hex address -> dict(name, image, live, hunk, size).

        A NAME IS LOOKED UP, A NUMBER IS TAKEN AS AN IMAGE ADDRESS. That
        is the useful default: every address in docs/, in Ghidra and in
        the fingerprint table is an image address, and a live one is
        something you only ever have because you just computed it.
        """
        name = None
        if isinstance(what, str) and not what.strip().lower().startswith('0x'):
            try:
                image = int(what, 16)
            except ValueError:
                sym = self.symbols.get(what)
                if sym is None:
                    raise SymbolError(
                        'no symbol %r in symbol_map.txt. It is generated '
                        'from Ghidra, so a missing name means the global '
                        'is unnamed there - give the image address '
                        'instead.' % what)
                name = what
                image = sym['addr']
        else:
            image = int(str(what), 16)
        hunk = hunk_of(image)
        size = 0
        if name:
            size = (self.symbols.get(name) or {}).get('size', 0)
        return {
            'name': name,
            'image': image,
            'live': image + self.slide(hunk),
            'hunk': hunk,
            'size': size,
        }

    def read(self, image_addr, size=4, signed=False):
        """A value out of the live machine, named by its image address."""
        raw = self.d.read(self.live(image_addr), size)
        v = int.from_bytes(raw, 'big')
        if signed and v >= (1 << (size * 8 - 1)):
            v -= 1 << (size * 8)
        return v


# ------------------------------------------------- the fingerprint table

class Field(object):
    __slots__ = ('label', 'ref', 'size', 'signed', 'stat')

    def __init__(self, label, ref, size, signed, stat):
        self.label = label
        self.ref = ref
        self.size = size
        self.signed = signed
        self.stat = stat

    def __repr__(self):
        return '<Field %s %s %s%s>' % (self.label, self.ref,
                                       self.size, 's' if self.signed else 'u')


class Scene(object):
    def __init__(self, name):
        self.name = name
        self.part = None
        self.when = None
        self.note = None
        # None means the block did not say, which is NOT the same as "no"
        # and is reported as "unstated" rather than being rounded down to
        # either - a block whose provenance nobody recorded is its own
        # kind of gap and worth seeing.
        self.verified = None
        self.fields = []


def parse_fingerprints(path=FINGERPRINTS):
    """docs/fingerprints.txt -> {scene name: Scene}.

    THE DOC IS THE DATA. A separate machine-readable copy would drift
    from the prose explaining it, and the prose is most of the value -
    so the parser reads the document and ignores everything that is not
    inside a block. Blocks are opened by ".fingerprint <name>" at column
    zero and closed by ".end"; a directive can never be confused with a
    paragraph because a paragraph does not start with a dot.
    """
    scenes = {}
    selectors = {}
    cur = None
    lineno = 0

    def bad(msg):
        raise SymbolError('%s:%d: %s' % (os.path.basename(path), lineno, msg))

    if not os.path.exists(path):
        return scenes, selectors
    with open(path, 'r') as fh:
        for raw in fh:
            lineno += 1
            line = raw.rstrip()
            if line.startswith('.fingerprint'):
                if cur is not None:
                    bad('.fingerprint inside %s - missing .end?' % cur.name)
                parts = line.split()
                if len(parts) != 2:
                    bad('.fingerprint takes one scene name')
                cur = Scene(parts[1])
                if cur.name in scenes:
                    bad('scene %s declared twice' % cur.name)
                scenes[cur.name] = cur
                continue
            if line.startswith('.selector'):
                body = line[len('.selector'):].strip()
                nm, _, rhs = body.partition('=')
                if not rhs:
                    bad('.selector <name> = <ref> <b|w|l>')
                f = rhs.split()
                selectors[nm.strip()] = (f[0], SIZES.get(f[1] if len(f) > 1
                                                         else 'l', 4))
                continue
            if line.startswith('.end'):
                if cur is None:
                    bad('.end outside a block')
                cur = None
                continue
            stripped = line.strip()
            if cur is None or not stripped or stripped.startswith('#'):
                continue                        # prose, or a comment
            key, _, rhs = line.partition('=')
            if not rhs:
                continue                        # prose inside a block
            key = key.strip()
            rhs = rhs.strip()
            if key == 'part':
                cur.part = int(rhs, 0)
                continue
            if key == 'when':
                cur.when = rhs
                continue
            if key == 'note':
                cur.note = rhs
                continue
            if key == 'verified':
                low = rhs.strip().lower()
                if low not in ('yes', 'no'):
                    bad('verified must be yes or no, not %r' % rhs)
                cur.verified = (low == 'yes')
                continue
            # <label> = <symbol|0xaddr> <b|w|l> [signed] [-> <stat>]
            stat = None
            if '->' in rhs:
                rhs, _, stat = rhs.partition('->')
                rhs, stat = rhs.strip(), stat.strip()
            f = rhs.split()
            if not f:
                bad('%s has no value' % key)
            size = SIZES.get(f[1]) if len(f) > 1 else None
            if size is None:
                bad('%s: want "<symbol|0xaddr> <b|w|l> [signed]"' % key)
            cur.fields.append(Field(key, f[0], size,
                                    'signed' in f[2:], stat))
    if cur is not None:
        raise SymbolError('%s: unterminated .fingerprint %s'
                          % (os.path.basename(path), cur.name))
    return scenes, selectors


# ---------------------------------------------------------------- selftest

class _StubDbg(object):
    """A reader that answers as the machine would for one known layout.

    It serves the file's own bytes at every CODE address, except at a
    relocation into BSS_04, where it serves what the loader would have
    patched there: the file's offset plus that hunk's base. That is
    exactly the difference _slide_bss has to find, so the stub proves
    the arithmetic without an emulator and without the pipe, which an
    MCP session holds anyway.
    """

    def __init__(self, blob, code_slide, bss_base):
        self.blob = blob
        self.code_slide = code_slide
        self.bss_base = bss_base
        self.sites = dict((a, f) for a, f, d in imgaddr.reloc_sites(blob)
                          if d == 'BSS_04')

    def read(self, addr, size):
        image = addr - self.code_slide
        seg = imgaddr.seg_of_addr(image)
        if seg is None or seg[3] is None:
            raise ValueError('unmapped %08x' % addr)
        if size == 4 and image in self.sites:
            off = self.sites[image]
            fv = int.from_bytes(self.blob[off:off + 4], 'big')
            return ((fv + self.bss_base) & 0xFFFFFFFF).to_bytes(4, 'big')
        _name, off = imgaddr.a2f(image)
        return self.blob[off:off + size]


def _selftest_bss_slide(blob):
    """_slide_bss must recover a base that was only ever written live."""
    code_slide, bss_base = 0x1C3564, 0x004FF390
    want = bss_base - 0x0033BE20
    live = Live(_StubDbg(blob, code_slide, bss_base))
    live._blob = blob
    for name, lo, _hi, flo in imgaddr.SEGMENTS:
        if flo is not None:
            live._slide[name] = (code_slide, lo, blob[flo:flo + 8])
    got = live._slide_bss('BSS_04')
    assert got == want, 'BSS_04 slide came back %+#x, wanted %+#x' % (got,
                                                                      want)
    print('  BSS_04   slide %+#x recovered from relocations' % got)
    cached = live._slide_bss('BSS_04')
    assert cached == want, 'the cached answer disagreed'


def selftest():
    syms = load_symbols()
    print('%d symbols from symbol_map.txt' % len(syms))
    for n in ('g_CamX', 'g_PullBackPos', 'g_CamPathCursor', 'g_pFrameBuffer'):
        s = syms[n]
        assert hunk_of(s['addr']) == s['hunk'], n
        print('  %-18s %08x %-8s' % (n, s['addr'], s['hunk']))

    # The hunk of an address must agree between the two tables that know.
    bad = [(n, s) for n, s in syms.items()
           if imgaddr.seg_of_addr(s['addr'])
           and imgaddr.seg_of_addr(s['addr'])[0] != s['hunk']]
    assert not bad, 'hunk disagreement: %r' % bad[:3]
    print('  all %d symbol hunks agree with imgaddr.SEGMENTS' % len(syms))

    blob = _file_bytes()
    for hunk in ('CODE_00', 'CODE_03', 'CODE_05', 'CODE_06'):
        cands = _anchor_candidates(blob, hunk)
        assert len(cands) >= 4, '%s gave only %d anchors' % (hunk, len(cands))
        print('  %-8s %2d anchor candidates, first at %08x'
              % (hunk, len(cands), cands[0][0]))
    for hunk in ('BSS_01', 'BSS_04'):
        try:
            _anchor_candidates(blob, hunk)
        except SymbolError as e:
            assert 'BSS' in str(e)
            print('  %-8s correctly refused: BSS has no file bytes' % hunk)

    sites = imgaddr.reloc_sites(blob)
    assert len(sites) == 6605, 'expected 6,605 relocations, got %d' % len(sites)
    for hunk in ('BSS_01', 'BSS_04'):
        n = sum(1 for _a, _f, d in sites if d == hunk)
        assert n >= 2, '%s has %d relocation(s), need two to agree' % (hunk, n)
        print('  %-8s reachable by %d relocation(s)' % (hunk, n))

    _selftest_bss_slide(blob)

    # The known CODE_03 anchor must still be where fbdump says it is.
    import fbdump
    name, off = imgaddr.a2f(fbdump.UV11_IMAGE)
    assert blob[off:off + 24].hex() == fbdump.UV11, \
        'UV11 is not at %08x in the file any more' % fbdump.UV11_IMAGE
    print('  fbdump UV11 pattern confirmed at %08x (%s)'
          % (fbdump.UV11_IMAGE, name))

    scenes, selectors = parse_fingerprints()
    print('%d scene(s) and %d selector(s) in fingerprints.txt'
          % (len(scenes), len(selectors)))
    for nm in sorted(scenes):
        sc = scenes[nm]
        for f in sc.fields:
            if not f.ref.lower().startswith('0x'):
                assert f.ref in syms, \
                    '%s names %s, which symbol_map.txt does not have' \
                    % (nm, f.ref)
            else:
                hunk_of(int(f.ref, 16))         # must be inside a hunk
        prov = {True: 'measured', False: 'INFERRED',
                None: 'unstated'}[sc.verified]
        print('  %-16s part=%-5s %-9s %d field(s): %s'
              % (nm, sc.part, prov, len(sc.fields),
                 ', '.join(f.label for f in sc.fields)))
    print('OK')
    return 0


def main(argv):
    if '--selftest' in argv:
        return selftest()
    if '--scenes' in argv:
        scenes, selectors = parse_fingerprints()
        for nm in sorted(scenes):
            sc = scenes[nm]
            print('%s  part=%s  when=%s' % (nm, sc.part, sc.when))
            for f in sc.fields:
                print('    %-12s %-20s %d%s%s'
                      % (f.label, f.ref, f.size, ' signed' if f.signed else '',
                         ('  -> ' + f.stat) if f.stat else ''))
        for nm, (ref, size) in sorted(selectors.items()):
            print('selector %s = %s (%d)' % (nm, ref, size))
        return 0
    if not argv:
        print(__doc__)
        return 2
    syms = load_symbols()
    for a in argv:
        s = syms.get(a)
        if s:
            print('%-24s image %08x  %-8s size %d'
                  % (a, s['addr'], s['hunk'], s['size']))
        else:
            print('%-24s not in symbol_map.txt' % a)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
