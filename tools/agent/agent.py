#!/usr/bin/env python
"""Talk to the debug agent embedded in relic_ppm.

    py -3.10 tools/agent/agent.py status
    py -3.10 tools/agent/agent.py parts
    py -3.10 tools/agent/agent.py raw "GOTO part=11"
    py -3.10 tools/agent/agent.py raw "STEP n=40"
    py -3.10 tools/agent/agent.py raw "DUMP path=t.ppm"

The pipe is \\\\.\\pipe\\relic_agent and only exists while the Debug
Win32 build is running with -agent:

    build\\Win32\\Debug\\relic_ppm.exe reverse/ghidra/relic <out-dir> \\
        -dat reverse/ghidra/relic.dat -agent

ONE CLIENT AT A TIME. The agent creates the pipe with nMaxInstances=1,
so while the MCP server holds the connection this CLI cannot open it
and fails with ERROR_PIPE_BUSY - which reads like a bad path and is
not. Use the MCP tools, or stop the MCP server first.

THE AGENT BLOCKS ON THE PIPE WHILE PAUSED, so a reply is immediate and
there is no poll loop on either side. A long STEP or RUN_TO is bounded
in the agent, not here; its reply carries truncated=1 when the bound
cut it short.

This module is also the transport for mcp_server.py.
"""

import errno
import io
import sys

PIPE = '\\\\.\\pipe\\relic_agent'

# ONE READ PER CALL, NOT A LOOP. buffering=0 gives a FileIO whose read()
# issues a single ReadFile, so it returns whatever the agent has
# written. A buffered wrapper would sit waiting for 8 KB of a reply
# that is sixty bytes long.
CHUNK = 8192


class NotRunning(Exception):
    """relic_ppm -agent is not up, or somebody else holds the pipe."""


class Stale(NotRunning):
    """The handle is dead and NOTHING WAS SENT THROUGH IT.

    Kept apart from every other failure because it is the only one
    that is safe to retry: the host was restarted under a connection
    this process still held open, so reopening and sending again asks
    the question once. A read that fails has already had its verb
    accepted - see send().
    """


class Agent(object):
    """One connection, held open for the life of the object.

    OPENED WITH open() RATHER THAN CreateFileW. ctypes gives a bare
    CreateFileW the default int return type, so the -1 of
    INVALID_HANDLE_VALUE compares unequal to a 64-bit (void *)-1 and
    the failure is missed - the first symptom is a WriteFile that
    reports ERROR_INVALID_HANDLE instead of the pipe being absent.
    Python's own open() gets the error right and names it.
    """

    def __init__(self, pipe=PIPE):
        self.pipe = pipe
        self.buf = ''
        self.f = None
        try:
            self.f = io.open(pipe, 'r+b', buffering=0)
        except OSError as e:
            # BUSY AND ABSENT NEED DIFFERENT ANSWERS. The pipe takes one
            # client, so a second session reaching for it gets EINVAL
            # from ERROR_PIPE_BUSY - and told 'no agent' it would start
            # a second host, which cannot create the pipe either.
            if e.errno == errno.EINVAL:
                raise NotRunning('%s is busy - the agent is running and'
                                 ' another client holds it. One client at'
                                 ' a time; stop the other session or its'
                                 ' MCP server.' % pipe)
            raise NotRunning('no agent at %s (%s) - start relic_ppm with'
                             ' -agent' % (pipe, e))

    def close(self):
        if self.f is not None:
            try:
                self.f.close()
            finally:
                self.f = None

    def _readline(self):
        while '\n' not in self.buf:
            try:
                got = self.f.read(CHUNK)
            except OSError as e:
                raise NotRunning('the agent stopped answering (%s)' % e)
            if not got:
                raise NotRunning('the agent closed the pipe')
            self.buf += got.decode('ascii', 'replace')
        line, self.buf = self.buf.split('\n', 1)
        return line.rstrip('\r')

    def send(self, line):
        """One verb, and everything its reply says follows it.

        A HEADER CARRYING lines=N IS A PROMISE OF N MORE LINES. Reading
        only the first line of a PARTS or SYMS reply would leave the
        rest in the pipe and desynchronise every later command, so the
        count is honoured here rather than by each caller.
        """
        try:
            self.f.write((line.strip() + '\n').encode('ascii'))
        except OSError as e:
            raise Stale('the handle is dead, nothing sent (%s)' % e)
        head = self._readline()
        out = [head]
        n = 0
        for field in head.split():
            if field.startswith('lines='):
                try:
                    n = int(field[6:])
                except ValueError:
                    n = 0
        for _ in range(n):
            out.append(self._readline())
        # The SYMS header is padded to a fixed width so that writing a
        # count it could not know up front cannot shift the rows under
        # it. That padding is the agent's business, not the reader's.
        return '\n'.join(l.rstrip() for l in out)


_conn = {'a': None}


def agent():
    """The shared connection, opened on first use."""
    if _conn['a'] is None:
        _conn['a'] = Agent()
    return _conn['a']


def reset():
    """Drop the connection so the next call opens a new one."""
    if _conn['a'] is not None:
        _conn['a'].close()
    _conn['a'] = None


def send(line):
    """One verb, reconnecting once if the handle went stale.

    RETRY ONLY WHEN THE WRITE ITSELF FAILED. The host is restarted far
    more often than a long-lived client is, and the first call after
    that would otherwise fail against a handle to a process that no
    longer exists - which reads as "the agent is not running" while it
    sits there waiting. A write that landed and a read that did not is
    the opposite case: the host died while executing the verb, and
    re-sending would run it a second time. For STEP or SET_MEM that is
    a different answer, not the same one, so it is reported instead.
    """
    try:
        return agent().send(line)
    except Stale:
        reset()
        return agent().send(line)


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    verb = argv[1].lower()
    if verb == 'raw':
        if len(argv) < 3:
            print('raw needs a command line')
            return 2
        line = ' '.join(argv[2:])
    else:
        line = ' '.join([verb.upper()] + argv[2:])
    try:
        print(send(line))
    except NotRunning as e:
        print('agent: %s' % e)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
