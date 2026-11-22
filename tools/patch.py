"""Apply a plain-text patch file to source, all or nothing.

    py -3.10 tools/patch.py P            apply
    py -3.10 tools/patch.py P --check    say what would change
    py -3.10 tools/patch.py --selftest

WHY THIS EXISTS. Edits were being made by writing a throwaway Python
script and running it. That puts the replacement text through
Python's string escaping AND, when the script is written from a
shell heredoc, through the shell's - so a `\\n` meant for C source
arrives as a newline and the file silently gains a line break in the
middle of a string literal. It has cost several build cycles.

A PATCH FILE GETS NO ESCAPE PROCESSING AT ALL. What is between the
markers is the bytes, whatever they are. Backslashes, quotes and
percent signs are all literal.

Format - markers must start the line, and OLD must appear EXACTLY
ONCE in the file named above it:

    # anything before the first --- is a comment
    --- src/part_temple.c
    <<<
    the text to find
    >>>
    the text to put there
    ===

A hunk whose OLD is missing, or present more than once, is an
error and NOTHING is written. That is the point: a patch that half
applies is worse than one that does not apply.

An empty NEW deletes. An empty OLD is refused - there is no sane
place to insert it.
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class PatchError(Exception):
    pass


def parse(text):
    """-> [(path, old, new)], in file order."""
    hunks = []
    path = None
    state = 'idle'
    old = []
    new = []
    for n, line in enumerate(text.split('\n'), 1):
        if state == 'idle':
            if line.startswith('--- '):
                path = line[4:].strip()
            elif line.startswith('<<<'):
                if path is None:
                    raise PatchError('line %d: <<< before any --- file' % n)
                state, old, new = 'old', [], []
        elif state == 'old':
            if line.startswith('>>>'):
                state = 'new'
            else:
                old.append(line)
        elif state == 'new':
            if line.startswith('==='):
                if not old:
                    raise PatchError('line %d: empty OLD, nothing to find'
                                     % n)
                hunks.append((path, '\n'.join(old), '\n'.join(new)))
                state = 'idle'
            else:
                new.append(line)
    if state != 'idle':
        raise PatchError('patch ends inside a hunk - a === is missing')
    if not hunks:
        raise PatchError('no hunks found')
    return hunks


def apply(hunks, check=False):
    """Verify every hunk, then write. -> [(path, count)]."""
    want = {}
    for path, old, new in hunks:
        full = path if os.path.isabs(path) else os.path.join(ROOT, path)
        if full not in want:
            if not os.path.isfile(full):
                raise PatchError('no such file: %s' % path)
            want[full] = io.open(full, encoding='ascii',
                                 errors='strict').read()
        n = want[full].count(old)
        if n == 0:
            raise PatchError('%s: OLD not found\n    %s'
                             % (path, old.split('\n')[0][:68]))
        if n > 1:
            raise PatchError('%s: OLD appears %d times, give more context'
                             '\n    %s'
                             % (path, n, old.split('\n')[0][:68]))
        want[full] = want[full].replace(old, new, 1)

    out = []
    for full, text in sorted(want.items()):
        try:
            rel = os.path.relpath(full, ROOT).replace(os.sep, '/')
        except ValueError:
            rel = full
        out.append((rel, sum(1 for h in hunks
                             if os.path.join(ROOT, h[0]) == full
                             or h[0] == full)))
        if not check:
            io.open(full, 'w', encoding='ascii', newline='\n').write(text)
    return out


SELFTEST = [
    ('a backslash survives',
     '--- T\n<<<\nalpha\n>>>\nprintf("x\\n");\n===\n',
     'alpha\n', 'printf("x\\n");\n'),
    ('an empty NEW deletes',
     '--- T\n<<<\ngone\n>>>\n===\n',
     'keep\ngone\n', 'keep\n\n'),
]


def selftest():
    import tempfile
    bad = 0
    for name, patch, before, after in SELFTEST:
        fd, path = tempfile.mkstemp(suffix='.c')
        os.close(fd)
        io.open(path, 'w', encoding='ascii', newline='\n').write(before)
        try:
            hunks = [(path, o, n) for _p, o, n in parse(patch)]
            apply(hunks)
            got = io.open(path, encoding='ascii').read()
        finally:
            os.remove(path)
        ok = got == after
        bad += not ok
        print('  %-26s %s' % (name, 'ok' if ok else 'FAILED'))
        if not ok:
            print('     wanted %r' % after)
            print('     got    %r' % got)

    # a hunk that matches twice must refuse, and write nothing
    fd, path = tempfile.mkstemp(suffix='.c')
    os.close(fd)
    io.open(path, 'w', encoding='ascii', newline='\n').write('dup\ndup\n')
    try:
        try:
            apply([(path, 'dup', 'x')])
            print('  %-26s FAILED' % 'an ambiguous OLD refuses')
            bad += 1
        except PatchError:
            print('  %-26s ok' % 'an ambiguous OLD refuses')
        if io.open(path, encoding='ascii').read() != 'dup\ndup\n':
            print('  %-26s FAILED' % 'and writes nothing')
            bad += 1
        else:
            print('  %-26s ok' % 'and writes nothing')
    finally:
        os.remove(path)

    print()
    print('selftest: %s' % ('every rule holds' if not bad else 'FAILURES'))
    return 1 if bad else 0


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    flags = set(a for a in argv[1:] if a.startswith('--'))
    if '--selftest' in flags:
        return selftest()
    if len(args) != 1:
        sys.stdout.write(__doc__)
        return 2
    text = io.open(args[0], encoding='ascii', errors='strict').read()
    try:
        hunks = parse(text)
        done = apply(hunks, check='--check' in flags)
    except PatchError as e:
        sys.stderr.write('patch refused: %s\n' % e)
        return 1
    verb = 'would change' if '--check' in flags else 'patched'
    for rel, n in done:
        print('%s %s  %d hunk(s)' % (verb, rel, n))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
