#!/usr/bin/env python
"""Project-local MCP server over the debug agent in relic_ppm.

Wired from .mcp.json, so it is scoped to this project and not
installed globally. Speaks MCP's JSON-RPC 2.0 over stdio using nothing
but the standard library - there is no mcp package to install.

Every tool goes through tools/agent/agent.py to the named pipe, and
the agent executes it on the host's own thread between frames. The
protocol is docs/subsystems/agent_mcp.txt.

    build\\Win32\\Release\\relic_ppm.exe reverse/ghidra/relic <out-dir> \\
        -dat reverse/ghidra/relic.dat -agent

    py -3.10 tools/agent/mcp_server.py --selftest
"""

import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import agent as _agent                                        # noqa: E402
from agent import NotRunning                                  # noqa: E402

PROTOCOL = '2024-11-05'

# The agent names its own crash file on every STATUS. Cached here so
# that a pipe that drops mid-session can be explained rather than just
# reported - see t_crash_note.
_crash = {'path': None}


def send(line):
    reply = _agent.send(line)
    for field in reply.split():
        if field.startswith('crashlog='):
            _crash['path'] = field[9:]
    return reply


def t_status(a):
    return send('STATUS')


def t_parts(a):
    return send('PARTS')


def t_goto(a):
    if a.get('part') is None:
        return 'goto needs part'
    return send('GOTO part=%d' % int(a['part']))


def t_next(a):
    return send('PREV' if a.get('back') else 'NEXT')


def t_step(a):
    return send('STEP n=%d' % int(a.get('n') or 1))


def t_run_to(a):
    if a.get('tick') is None:
        return 'run_to needs tick'
    return send('RUN_TO tick=%d' % int(a['tick']))


def t_pause(a):
    return send('RESUME' if a.get('resume') else 'PAUSE')


def t_res(a):
    if a.get('w') is None or a.get('h') is None:
        return 'res needs w and h'
    return send('SET_RES w=%d h=%d' % (int(a['w']), int(a['h'])))


def t_dump(a):
    """Write the frame as it stands, and say what it holds.

    THE PIXEL COUNT IS NOT DECORATION. A part whose init() failed
    renders an all-black frame that is a perfectly well formed PPM, so
    a dump that reports nothing about its content cannot be told from
    one of a scene that is genuinely dark.
    """
    path = a.get('path')
    if not path:
        return 'dump needs path'
    out = send('DUMP path=%s' % path)
    if not out.startswith('OK'):
        return out
    real = None
    for field in out.split():
        if field.startswith('path='):
            real = field[5:]
    if not real or not os.path.exists(real):
        return out
    data = io.open(real, 'rb').read()
    head = data.split(b'\n', 3)
    body = data[len(b'\n'.join(head[:3])) + 1:]
    lit = sum(1 for b in bytearray(body) if b)
    return '%s\n%d of %d bytes non-zero (%.1f%%)' % (
        out, lit, len(body), 100.0 * lit / max(1, len(body)))


def t_sym(a):
    if a.get('addr') is not None:
        return send('SYM addr=%s' % a['addr'])
    if not a.get('name'):
        return 'sym needs name or addr'
    line = 'SYM name=%s' % a['name']
    if a.get('force'):
        line += ' force=1'
    return send(line)


def t_syms(a):
    line = 'SYMS'
    for key in ('obj', 'pfx', 'kind'):
        if a.get(key):
            line += ' %s=%s' % (key, a[key])
    if a.get('max'):
        line += ' max=%d' % int(a['max'])
    return send(line)


def _where(a):
    """sym=NAME[#OBJ] or at=0xADDR, plus an optional off."""
    if a.get('sym'):
        part = 'sym=%s' % a['sym']
    elif a.get('at'):
        part = 'at=%s' % a['at']
    else:
        return None
    if a.get('off'):
        part += ' off=%s' % a['off']
    return part


def t_read(a):
    where = _where(a)
    if not where:
        return 'read needs sym or at'
    return send('GET_MEM %s len=%d' % (where, int(a.get('len') or 4)))


def t_write(a):
    where = _where(a)
    if not where:
        return 'write needs sym or at'
    if not a.get('hex'):
        return 'write needs hex'
    return send('SET_MEM %s data=%s' % (where, a['hex']))


def t_hook(a):
    if not a.get('name'):
        return 'hook needs name'
    line = 'SET_HOOK name=%s' % a['name']
    for key in ('args', 'max'):
        if a.get(key) is not None:
            line += ' %s=%d' % (key, int(a[key]))
    for key in ('tag', 'enter', 'exit', 'when', 'agg'):
        if a.get(key):
            line += ' %s=%s' % (key, a[key])
    if a.get('force'):
        line += ' force=1'
    return send(line)


def t_unhook(a):
    which = a.get('id')
    return send('CLEAR_HOOK id=%s'
                % ('all' if which in (None, 'all') else int(which)))


def t_hooks(a):
    return send('LIST_HOOK')


def t_clear_trace(a):
    return send('CLEAR_TRACE')


def t_trace(a):
    return send('GET_TRACE max=%d' % int(a.get('max') or 64))


def t_dump_mem(a):
    """Write a region of memory to a file and say what it holds.

    THE NON-ZERO COUNT IS THE POINT. A buffer that dumps as 64,000
    zeroes is the usual way of learning that the data went somewhere
    else - which is exactly what a frame dump of the ruins does with
    the light byte.
    """
    where = _where(a)
    if not where:
        return 'dump_mem needs sym or at'
    if not a.get('path'):
        return 'dump_mem needs path'
    if a.get('len') is None:
        return 'dump_mem needs len'
    line = 'DUMP_MEM %s len=%d path=%s' % (where, int(a['len']), a['path'])
    if a.get('deref'):
        line += ' deref=1'
    if a.get('skip'):
        line += ' skip=%s' % a['skip']
    return send(line)


def t_raw(a):
    if not a.get('line'):
        return 'raw needs line'
    return send(a['line'])


def t_quit(a):
    return send('QUIT')


ADDR = {'type': 'string', 'description': 'hex, e.g. 0x001de8e4'}
SYM = {'type': 'string',
       'description': 'a map symbol, qualified as name#obj when it is '
                      'ambiguous - g_ready exists in twelve objects'}

TOOLS = [
    ('relic_status',
     'Where the demo is: part, tick, the tick the pixels show, the '
     'symbol count, the image slide, and whether the map is stale.',
     {}, [], t_status),
    ('relic_parts', 'The part list, with each one id, size and length.',
     {}, [], t_parts),
    ('relic_goto',
     'Jump to a part and stop on its first frame. Reports init=FAILED '
     'when that part needs an asset the host was not given.',
     {'part': {'type': 'integer', 'description': 'index, 0 based'}},
     ['part'], t_goto),
    ('relic_next', 'Step to the next part, or the previous one.',
     {'back': {'type': 'boolean', 'description': 'go backwards'}},
     [], t_next),
    ('relic_step', 'Render n ticks and stop. One tick is one PAL frame.',
     {'n': {'type': 'integer', 'description': 'ticks, default 1'}},
     [], t_step),
    ('relic_run_to',
     'Render forward to a tick. THERE IS NO SEEK BACKWARDS: a part with '
     'per-tick state has to be replayed from its own start, so go to '
     'the part first.',
     {'tick': {'type': 'integer', 'description': 'the tick to stop on'}},
     ['tick'], t_run_to),
    ('relic_pause',
     'Pause, which also turns auto-advance off so a step cannot drift '
     'into the next part. Pass resume to let it run freely again.',
     {'resume': {'type': 'boolean', 'description': 'run freely instead'}},
     [], t_pause),
    ('relic_res',
     'Set the host size the demo fits itself into. Low by default, '
     'because a big frame makes every step slower for nothing.',
     {'w': {'type': 'integer'}, 'h': {'type': 'integer'}},
     ['w', 'h'], t_res),
    ('relic_dump',
     'Write the current frame as a PPM and report how much of it is '
     'not black. Relative paths land in the out-dir the host was '
     'started with.',
     {'path': {'type': 'string', 'description': 'file to write'}},
     ['path'], t_dump),
    ('relic_sym',
     'Look a symbol up by name, or ask what is at an address.',
     {'name': SYM, 'addr': ADDR,
      'force': {'type': 'boolean',
                'description': 'answer even when the map is stale'}},
     [], t_sym),
    ('relic_syms', 'List symbols, filtered by object, prefix or kind.',
     {'obj': {'type': 'string', 'description': 'e.g. part_temple'},
      'pfx': {'type': 'string', 'description': 'name prefix, e.g. g_'},
      'kind': {'type': 'string', 'enum': ['f', 'd'],
               'description': 'f for a function, d for a datum'},
      'max': {'type': 'integer', 'description': 'rows, default 200'}},
     [], t_syms),
    ('relic_read', 'Read memory at a symbol or an address, as hex.',
     {'sym': SYM, 'at': ADDR,
      'off': {'type': 'string',
              'description': 'added to sym or to at, either way'},
      'len': {'type': 'integer', 'description': 'bytes, max 8192'}},
     [], t_read),
    ('relic_write', 'Write memory at a symbol or an address.',
     {'sym': SYM, 'at': ADDR,
      'off': {'type': 'string',
              'description': 'added to sym or to at, either way'},
      'hex': {'type': 'string', 'description': 'bytes as hex, e.g. 00ff10'}},
     ['hex'], t_write),
    ('relic_hook',
     'Instrument a function without stopping the demo. Captures run on '
     'entry and on exit; a list is comma separated and takes aN for an '
     'argument, @sym#obj+OFF:LEN for a global, *aN+OFF:LEN for a '
     'pointer argument dereferenced, 0xADDR:LEN for a literal address, '
     'ret and *ret+OFF:LEN for what it returns, caller for where it was '
     'called from, and exit=delta for only what changed. Refused with '
     'ERR_NO_PENTER if that object was not built with /Gh.',
     {'name': SYM,
      'args': {'type': 'integer',
               'description': 'how many arguments the function takes. '
                              'DECLARED, NEVER DISCOVERED - aN needs '
                              'it, and too high a count reads the '
                              'caller locals without faulting'},
      'enter': {'type': 'string', 'description': 'capture list on entry'},
      'exit': {'type': 'string',
               'description': 'capture list on exit, or delta'},
      'max': {'type': 'integer',
              'description': 'stop capturing after this many calls. '
                             'Bounds the records, not the hit count'},
      'when': {'type': 'string',
               'description': 'record a call only if this holds, as one '
                              'token: ret==-1, a0>1000. A test on ret is '
                              'decided at exit and the enter record waits '
                              'with it, so a filtered call leaves nothing'},
      'agg': {'type': 'string',
              'description': 'total one scalar over every call that passes '
                             'when, reported by relic_hooks as n/min/max/'
                             'sum and written to no record at all. The way '
                             'to measure a per-pixel function'},
      'tag': {'type': 'string', 'description': 'a label for the listing'},
      'force': {'type': 'boolean',
                'description': 'arm even when the map is stale'}},
     ['name'], t_hook),
    ('relic_unhook', 'Remove one hook, or all of them.',
     {'id': {'type': 'integer', 'description': 'omit to clear all'}},
     [], t_unhook),
    ('relic_hooks',
     'The armed hooks with their hit counts, plus how much the ring '
     'holds and what it had to drop.',
     {}, [], t_hooks),
    ('relic_clear_trace',
     'Throw away captured records without reading them. A hook on a '
     'per-pixel function fills the ring in one frame, and this is the '
     'way back without pulling it all through the pipe.',
     {}, [], t_clear_trace),
    ('relic_trace',
     'Drain captured records. data= is little-endian hex in the order '
     'the capture list asked for, and caller= is resolved to a name when '
     'the list asked for one.',
     {'max': {'type': 'integer', 'description': 'records, default 64'}},
     [], t_trace),
    ('relic_dump_mem',
     'Write a region of memory to a file, past the 8 KB and the hex '
     'that bound relic_read. deref follows a pointer VARIABLE to the '
     'buffer it names - g_prev#part_temple is the ruins per-pixel '
     'light at 160x100x4, which the frame does not carry because '
     'te_frame zeroes PX_LIGHT. Reports how much was written and how '
     'much of it is not zero.',
     {'sym': SYM, 'at': ADDR,
      'off': {'type': 'string',
              'description': 'address arithmetic on the symbol, applied '
                             'BEFORE any deref'},
      'deref': {'type': 'boolean',
                'description': 'read the 4 bytes there as a pointer and '
                               'dump what it points at'},
      'skip': {'type': 'string',
               'description': 'offset INTO the buffer, applied after the '
                              'deref'},
      'len': {'type': 'integer', 'description': 'bytes, max 16 MB'},
      'path': {'type': 'string', 'description': 'file to write'}},
     ['len', 'path'], t_dump_mem),
    ('relic_raw', 'Send one protocol line verbatim. For a verb with no tool.',
     {'line': {'type': 'string', 'description': 'e.g. CAPS'}},
     ['line'], t_raw),
    ('relic_quit', 'Ask the host to shut down cleanly.', {}, [], t_quit),
]

HANDLERS = dict((name, fn) for name, _d, _p, _r, fn in TOOLS)
SCHEMA = dict((name, set(props)) for name, _d, props, _r, _fn in TOOLS)


def check_args(name, args):
    """Refuse an argument the tool does not have.

    A MISSPELLED KEY THAT IS IGNORED STILL ANSWERS OK, and the answer
    is to a different question than the one asked. Every silent one of
    those is a reading that has to be thrown away later.
    """
    bad = sorted(set(args) - SCHEMA.get(name, set()))
    if not bad:
        return None
    ok = sorted(SCHEMA.get(name, set()))
    return ('%s has no argument %s - it takes %s'
            % (name, ', '.join(repr(b) for b in bad),
               ', '.join(ok) if ok else 'no arguments'))


def crash_note():
    """What the agent wrote on its way out, if it died."""
    path = _crash['path']
    if not path or not os.path.exists(path):
        return ''
    try:
        return '\nit left a crash report: %s' % io.open(
            path, encoding='ascii', errors='replace').read().strip()
    except IOError:
        return ''


def tool_list():
    return [{
        'name': name,
        'description': desc,
        'inputSchema': {'type': 'object', 'properties': props,
                        'required': req},
    } for name, desc, props, req, _fn in TOOLS]


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
            'serverInfo': {'name': 'relic-agent', 'version': '0.1'},
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
            is_err = text.startswith('ERR_')
        except NotRunning as e:
            text = ('the agent is not answering: %s%s\n'
                    'Start it with:  build\\Win32\\Release\\relic_ppm.exe '
                    'reverse/ghidra/relic <out-dir> '
                    '-dat reverse/ghidra/relic.dat -agent'
                    % (e, crash_note()))
            _agent.reset()
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
    try:
        print('agent: %s' % send('STATUS'))
    except NotRunning as e:
        print('agent: not running (%s)' % e)
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
