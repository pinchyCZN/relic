#!/usr/bin/env python
"""Talk to WinUAE's remote debugger over its named pipe.

    py -3.10 tools/amiga/dbg.py ping
    py -3.10 tools/amiga/dbg.py read 2e0c28 4
    py -3.10 tools/amiga/dbg.py watch add 2e0c28 4 2 log
    py -3.10 tools/amiga/dbg.py log read

The pipe is \\\\.\\pipe\\winuae-dbg and WinUAE opens it lazily on its
first emulated frame, so the emulator has to be running - a config has
to be started, not just the GUI sitting there.

ONE COMMAND PER EMULATED FRAME. The emulator services the pipe from
devices_vsync_pre(), so a reply takes up to 20 ms on a PAL config and
a burst of commands is paced at 50 a second. That is a deliberate
consequence of never touching guest state from another thread; see
WinUAE_SRC/docs/REMOTE_DEBUG.txt.

This module is also the transport for the MCP server in mcp_server.py.

ONE CLIENT AT A TIME. WinUAE creates the pipe with nMaxInstances=1, so
while Claude's MCP server holds the connection this CLI cannot open it
and fails with "Invalid argument" (ERROR_PIPE_BUSY) - which reads like a
bad path but is not. Use the MCP tools, or stop the MCP server first.
"""

import ctypes
import msvcrt
import sys
import time

PIPE = r'\\.\pipe\winuae-dbg'
TIMEOUT = 5.0

PIPE_NOWAIT = 0x00000001

# THE SLEEP IN THE POLL LOOP IS NOT A MILLISECOND. Windows rounds a
# sleep up to the scheduler tick - 15.6 ms - unless some process has
# asked for better, and nothing in this build does. A reply is about
# one emulated frame away, so every round trip paid one or two whole
# ticks: 0.19 s per read command [measured], which is where a 64 KB
# framebuffer's time went. timeBeginPeriod(1) asks for a 1 ms tick,
# and SPIN_S covers the common case without sleeping at all.
#
# The period is raised once and never released: this module lives in a
# long-running MCP server, and handing the resolution back between
# commands would only mean asking for it again.
SPIN_S = 0.004
_timer_raised = False


def _raise_timer():
    global _timer_raised
    if _timer_raised:
        return
    try:
        ctypes.windll.winmm.timeBeginPeriod(1)
    except (AttributeError, OSError):
        pass                # no winmm: the spin alone still helps
    _timer_raised = True


class NotRunning(Exception):
    pass


class Dbg(object):
    """One connection. WinUAE accepts a single client at a time."""

    def __init__(self, pipe=PIPE, timeout=TIMEOUT):
        self.pipe = pipe
        self.timeout = timeout
        self.f = None
        self._buf = b''

    def open(self):
        if self.f:
            return
        _raise_timer()
        deadline = time.time() + self.timeout
        last = None
        while time.time() < deadline:
            try:
                self.f = open(self.pipe, 'r+b', buffering=0)
                self._set_nowait()
                return
            except OSError as e:
                last = e
                time.sleep(0.05)
        raise NotRunning(
            'no pipe at %s - is a WinUAE config running? (%s)'
            % (self.pipe, last))

    def _set_nowait(self):
        """Make reads return immediately instead of blocking.

        WITHOUT THIS THE TIMEOUT BELOW DOES NOTHING. The server's handle
        is PIPE_NOWAIT but ours is not, so a blocking read(1) parks
        forever when the emulator stops answering - and the deadline
        loop never gets to run. One wedged command then costs minutes
        rather than self.timeout.
        """
        mode = ctypes.c_ulong(PIPE_NOWAIT)
        ctypes.windll.kernel32.SetNamedPipeHandleState(
            ctypes.c_void_p(msvcrt.get_osfhandle(self.f.fileno())),
            ctypes.byref(mode), None, None)

    def close(self):
        self._buf = b''
        if self.f:
            try:
                self.f.close()
            finally:
                self.f = None

    def command(self, text):
        """Send one line, return the reply with its ok/err kept.

        Retried once on a dead handle, so a WinUAE restart does not need
        the MCP server restarted with it: open() short-circuits on a
        cached self.f, and writing to the stale handle of a closed
        emulator raises instead of reconnecting.
        """
        try:
            return self._once(text)
        except OSError:
            self.close()
            return self._once(text)

    def _once(self, text):
        self.open()
        try:
            self.f.write((text.strip() + '\n').encode('ascii'))
        except OSError:
            self.close()
            raise
        deadline = time.time() + self.timeout
        idle = None
        # READ IN BLOCKS, NOT A BYTE AT A TIME. A `read` reply is two
        # hex characters per byte, so a full one is thousands of them
        # and a per-character loop costs a syscall and a string copy
        # each. Over-reading is safe because the protocol is strictly
        # one reply per command: anything past the newline belongs to
        # the next reply and is kept in _buf.
        while True:
            nl = self._buf.find(chr(10).encode('ascii'))
            if nl >= 0:
                line = self._buf[:nl]
                self._buf = self._buf[nl + 1:]
                return line.decode('ascii', 'replace')
            if time.time() >= deadline:
                break
            try:
                got = self.f.read(65536)
            except OSError:
                got = b''       # non-blocking handle: nothing ready yet
            if got:
                self._buf += got
                idle = None
            else:
                now = time.perf_counter()
                if idle is None:
                    idle = now
                if now - idle >= SPIN_S:
                    time.sleep(0.001)
        self.close()            # reopen next time; this one is no good
        raise NotRunning(
            'no reply to %r within %.1fs - the emulator is up but its '
            'emulation thread has stopped servicing the pipe'
            % (text, self.timeout))

    # ---- conveniences over the raw protocol ----

    def ping(self):
        return self.command('ping')

    def frame(self):
        r = self.command('frame')
        return int(r.split()[1]) if r.startswith('ok') else None

    def read(self, addr, length):
        """Returns bytes, or raises on an err reply."""
        r = self.command('read %x %x' % (addr, length))
        if not r.startswith('ok'):
            raise ValueError(r)
        return bytes.fromhex(''.join(r.split()[1:]))

    def regs(self):
        r = self.command('regs')
        if not r.startswith('ok'):
            raise ValueError(r)
        out = {}
        for tok in r.split()[1:]:
            k, _, v = tok.partition('=')
            out[k] = int(v, 16)
        return out

    def search(self, start, end, pattern, align=1, limit=256):
        """Find every occurrence of pattern in [start, end).

        pattern is hex byte pairs in MEMORY ORDER, so a 68k longword is
        written big-endian exactly as a dump shows it, and "??" matches
        any byte - which is how you skip the relocated addresses that
        make demo code differ between the file and the running image.

        The emulator answers one page of hits plus a cursor; this follows
        the cursor so the caller never has to pick a new range. Returns
        (hits, complete); complete is False only if limit was reached.
        """
        hits, cur = [], start
        while cur < end:
            r = self.command('search %x %x %s align=%d max=64'
                             % (cur, end, pattern, align))
            if not r.startswith('ok'):
                raise ValueError(r)
            nxt = None
            for tok in r.split()[2:]:
                if tok == 'done':
                    return hits, True
                if tok.startswith('next='):
                    nxt = int(tok[5:], 16)
                    break
                hits.append(int(tok, 16))
            if nxt is None:                 # neither done nor next=
                return hits, True
            if len(hits) >= limit:
                return hits, False
            cur = nxt
        return hits, True

    def hits(self, maxrec=1024):
        """Drain the watch log. Each record is a dict.

        1024 BECAUSE A FRAME CAN EXCEED 256. A watch on a global the
        renderer writes per face logs hundreds a frame, and a short
        drain reads exactly like a complete one - plausible records,
        just fewer than were logged. Needs REP_MAX above 16K in
        remote_debug.cpp to actually arrive; against an older build
        the reply stops at the buffer instead.
        """
        r = self.command('log read %x' % maxrec)
        if not r.startswith('ok'):
            raise ValueError(r)
        recs = []
        for tok in r.split()[1:]:
            if tok.startswith('lost='):
                continue
            parts = tok.split(',')
            if len(parts) != 6:
                continue
            recs.append({
                'frame': int(parts[0]),
                'pc': int(parts[1], 16),
                'addr': int(parts[2], 16),
                'size': int(parts[3]),
                'rw': parts[4],
                'val': int(parts[5], 16),
            })
        return recs


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    d = Dbg()
    try:
        print(d.command(' '.join(argv[1:])))
    except NotRunning as e:
        print('not running: %s' % e)
        return 1
    finally:
        d.close()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
