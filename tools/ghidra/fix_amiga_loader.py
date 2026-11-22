#!/usr/bin/env python
"""Repair the signed-int hunk_size bug in the ghidra-amiga extension.

    py -3.10 tools/ghidra/fix_amiga_loader.py --check    is it patched?
    py -3.10 tools/ghidra/fix_amiga_loader.py            patch it
    py -3.10 tools/ghidra/fix_amiga_loader.py --restore  put .orig back

CLOSE GHIDRA FIRST. Windows holds the jar open while Ghidra runs and the
replace will fail, or worse, half-succeed.

THE BUG

hunk/HunkHeaderBlock.java reads each HUNK_HEADER allocation size with
reader.readNextInt(), which is SIGNED, then rejects the value when it is
negative:

    int hunkSize = reader.readNextInt();
    if (hunkSize < 0) {
        throw new HunkParseError("HUNK_HEADER contains invalid hunk_size");
    }
    hunkTable.add(hunkSize & 0x3FFFFFFF);

Bits 30-31 of that longword are the MEMF_ memory flags, not size bits:
0 ANY, 1 CHIP, 2 FAST, 3 EXTENDED.  MEMF_FAST is 0x80000000, so every
FAST hunk reads back negative and aborts the import.  The very next line
masks the flags off with 0x3FFFFFFF, which is the proof the check is a
mistake rather than a constraint - the author already knew the flags
were there.

It is a straight port of amitools' hunk.py, where _read_long returns -1
at end of file and `if hunk_size < 0` is the EOF test.  Python has no
32-bit signed int, so the test means something different there.

The downstream "Index 0 out of bounds for length 0" at
AmigaHunkLoader.java:176 is fallout, not a second bug: load() catches
the parse error, logs it, and calls loadExecutable anyway with an empty
segment list.

THE FIX

Drop the check, and consume the extra longword that flags value 3
(EXTENDED) places after the size - without that, every later field in
the header reads one longword short.  Memory flags do not affect layout:
Relocate.getSeqAddresses places segments sequentially from the image
base whatever the flags say, so a file that imports after this patch
gets exactly the addresses it would have had.

Only hunk/HunkHeaderBlock.class is replaced.  Nothing else in the
extension is touched, and the pristine jar is kept beside it as
ghidra-amiga.jar.orig.

ALTERNATIVE, NEEDING NO TOOLCHAIN

tools/hunk/hunk_memf_any.py clears the flags in a copy of the binary
instead.  reverse/patched/relic_anymem is that copy and imports into a
stock extension.  See docs/formats/hunk.txt section 3.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

GHIDRA_VER = 'ghidra_12.0.4_PUBLIC'
EXT_NAME = 'ghidra-amiga'
CLASS_ENTRY = 'hunk/HunkHeaderBlock.class'
SRC_ENTRY = 'src/main/java/hunk/HunkHeaderBlock.java'

# The blank lines inside the block carry trailing tabs in the shipped
# source, so the anchor is a regex with \s* between statements rather
# than a literal. It still has to match exactly once; a second match or
# none means the extension changed and this script must be re-read
# against the new HunkHeaderBlock.java.
ANCHOR = re.compile(
    r'int\s+hunkSize\s*=\s*reader\.readNextInt\(\)\s*;\s*'
    r'if\s*\(\s*hunkSize\s*<\s*0\s*\)\s*\{\s*'
    r'throw\s+new\s+HunkParseError\s*\(\s*'
    r'"HUNK_HEADER contains invalid hunk_size"\s*\)\s*;\s*'
    r'\}\s*'
    r'hunkTable\.add\s*\(\s*hunkSize\s*&\s*0x3FFFFFFF\s*\)\s*;')

REPLACEMENT = """				int hunkSize = reader.readNextInt();

				// Bits 30-31 are the MEMF_ memory flags, not part of the
				// size: 1 CHIP, 2 FAST, 3 EXTENDED. readNextInt is SIGNED,
				// so FAST and EXTENDED make hunkSize negative. The old
				// "hunkSize < 0" reject was a port of amitools' EOF
				// sentinel and refused every MEMF_FAST hunk.
				int memFlags = (hunkSize >>> 30) & 3;

				hunkTable.add(hunkSize & 0x3FFFFFFF);

				// Flags value 3 means an extended-flags longword follows
				// the size and must be consumed, or every later field in
				// the header reads one longword short.
				if (memFlags == 3) {
					reader.readNextInt();
				}"""

# Byte sequence of the error message in the compiled constant pool. Its
# absence from the class is what --check tests, because it is the one
# artifact of the bug that survives compilation.
MARKER = b'HUNK_HEADER contains invalid hunk_size'


def die(msg):
    sys.exit('fix_amiga_loader: %s' % msg)


def paths():
    home = os.environ.get('USERPROFILE') or os.path.expanduser('~')
    ext = os.path.join(home, 'AppData', 'Roaming', 'ghidra', GHIDRA_VER,
                       'Extensions', EXT_NAME)
    if not os.path.isdir(ext):
        die('extension not found: %s' % ext)
    jar = os.path.join(ext, 'lib', '%s.jar' % EXT_NAME)
    src = os.path.join(ext, 'lib', '%s-src.zip' % EXT_NAME)
    for p in (jar, src):
        if not os.path.isfile(p):
            die('missing %s' % p)
    return ext, jar, src


def jdk():
    home = os.environ.get('USERPROFILE') or os.path.expanduser('~')
    save = os.path.join(home, 'AppData', 'Roaming', 'ghidra', GHIDRA_VER,
                        'java_home.save')
    if os.path.isfile(save):
        jh = open(save).read().strip()
        javac = os.path.join(jh, 'bin', 'javac.exe')
        if os.path.isfile(javac):
            return javac
    javac = shutil.which('javac')
    if javac:
        return javac
    die('no javac. Ghidra records its JDK in %s' % save)


def ghidra_install():
    """Ghidra's own jars, needed on the compile classpath."""
    for env in ('GHIDRA_INSTALL_DIR',):
        d = os.environ.get(env)
        if d and os.path.isdir(os.path.join(d, 'Ghidra')):
            return d
    for d in (r'C:\ghidra', r'C:\Program Files\ghidra',
              '/opt/ghidra', '/usr/local/ghidra'):
        if os.path.isdir(os.path.join(d, 'Ghidra')):
            return d
    die('Ghidra install not found. Set GHIDRA_INSTALL_DIR.')


def read_entry(jar, entry):
    with zipfile.ZipFile(jar) as z:
        return z.read(entry)


def is_patched(jar):
    return MARKER not in read_entry(jar, CLASS_ENTRY)


def class_version(blob):
    minor, major = struct.unpack_from('>HH', blob, 4)
    return major, minor


def replace_in_jar(jar, entry, blob):
    """Rebuild the jar with one entry's content swapped, order preserved."""
    fd, tmp = tempfile.mkstemp(suffix='.jar', dir=os.path.dirname(jar))
    os.close(fd)
    try:
        with zipfile.ZipFile(jar) as zin:
            names = zin.namelist()
            if entry not in names:
                die('%s is not in the jar' % entry)
            with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as zout:
                for info in zin.infolist():
                    data = blob if info.filename == entry else zin.read(info)
                    out = zipfile.ZipInfo(info.filename, info.date_time)
                    out.compress_type = info.compress_type
                    out.external_attr = info.external_attr
                    zout.writestr(out, data)
        shutil.move(tmp, jar)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def do_check(jar):
    patched = is_patched(jar)
    major, _ = class_version(read_entry(jar, CLASS_ENTRY))
    print('jar     %s' % jar)
    print('class   %s  (class file major %d)' % (CLASS_ENTRY, major))
    print('backup  %s' % ('present' if os.path.isfile(jar + '.orig')
                          else 'none'))
    print('state   %s' % ('PATCHED - MEMF_FAST hunks load'
                          if patched else
                          'STOCK - MEMF_FAST hunks are rejected'))
    return 0 if patched else 1


def do_restore(jar):
    orig = jar + '.orig'
    if not os.path.isfile(orig):
        die('no backup at %s' % orig)
    shutil.copy2(orig, jar)
    print('restored %s from %s' % (jar, orig))
    return do_check(jar)


def do_patch(jar, srczip):
    if is_patched(jar):
        print('already patched; nothing to do')
        return do_check(jar)

    javac = jdk()
    install = ghidra_install()

    work = tempfile.mkdtemp(prefix='amigaldr_')
    try:
        with zipfile.ZipFile(srczip) as z:
            names = [n for n in z.namelist()
                     if n.endswith('/hunk/') or '/hunk/' in n]
            if SRC_ENTRY not in z.namelist():
                die('%s not in %s' % (SRC_ENTRY, srczip))
            srcdir = os.path.join(work, 'hunk')
            os.makedirs(srcdir)
            for n in names:
                if n.endswith('.java'):
                    open(os.path.join(srcdir, os.path.basename(n)),
                         'wb').write(z.read(n))

        target = os.path.join(srcdir, 'HunkHeaderBlock.java')
        text = open(target, encoding='utf-8').read()
        hits = ANCHOR.findall(text)
        if len(hits) != 1:
            die('the buggy block was found %d times, expected once - the '
                'extension version has changed. Re-read '
                'HunkHeaderBlock.java and update ANCHOR.' % len(hits))
        open(target, 'w', encoding='utf-8', newline='').write(
            ANCHOR.sub(lambda _m: REPLACEMENT.strip(), text))

        cp = [jar, os.path.join(os.path.dirname(jar),
                                'commons-lang3-3.12.0.jar')]
        for root, _dirs, files in os.walk(os.path.join(install, 'Ghidra')):
            cp += [os.path.join(root, f) for f in files if f.endswith('.jar')]

        out = os.path.join(work, 'classes')
        os.makedirs(out)
        cmd = [javac, '-nowarn', '-source', '21', '-target', '21',
               '-cp', os.pathsep.join(cp), '-d', out, target]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            die('javac failed:\n%s\n%s' % (r.stdout, r.stderr))

        blob = open(os.path.join(out, 'hunk', 'HunkHeaderBlock.class'),
                    'rb').read()
        if MARKER in blob:
            die('the compiled class still carries the error string; the '
                'patch did not take')

        want = class_version(read_entry(jar, CLASS_ENTRY))
        got = class_version(blob)
        if got != want:
            die('class file version %s does not match the jar\'s %s'
                % (got, want))

        orig = jar + '.orig'
        if not os.path.isfile(orig):
            shutil.copy2(jar, orig)
            print('backed up  %s' % orig)
        else:
            print('backup already exists, left alone: %s' % orig)

        replace_in_jar(jar, CLASS_ENTRY, blob)
        print('replaced   %s  (%d bytes)' % (CLASS_ENTRY, len(blob)))
        print('')
        print('RESTART GHIDRA for this to take effect.')
        print('')
        return do_check(jar)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv):
    _ext, jar, srczip = paths()
    arg = argv[1] if len(argv) > 1 else ''
    if arg == '--check':
        return do_check(jar)
    if arg == '--restore':
        return do_restore(jar)
    if arg:
        sys.exit(__doc__)
    return do_patch(jar, srczip)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
