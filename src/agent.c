/* RELIC - the debug agent. OURS in full; nothing here is transcribed.
 *
 * A line protocol on \.\pipe\relic_agent, served by one listener
 * thread but EXECUTED ON THE HOST'S OWN THREAD. The demo core is
 * single threaded and Demo_* is not reentrant, so the listener only
 * parks a request and waits; Agent_Pump picks it up between frames.
 * That is what makes stepping synchronous and a memory read untorn.
 *
 * Because the pump is the only thing that touches demo state, the pipe
 * can be an ordinary blocking pipe. There is no poll loop and no sleep
 * anywhere in the request path.
 *
 * NEVER COMPILE THIS FILE WITH /Gh. _penter would call itself and
 * recurse until the stack is gone.
 *
 * The protocol, its verbs and its limits are
 * docs/subsystems/agent_mcp.txt.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relic.h"
#include "host.h"
#include "parts.h"
#include "agent.h"

#if !defined(_M_IX86)

/* OURS. The agent is Win32 only - see agent.h. Failing here is what
 * lets a host say why instead of spinning. */
int Agent_Start(const char *dump_dir) { (void)dump_dir; return 1; }

/* OURS */
void Agent_Stop(void) {}

/* OURS */
int Agent_Pump(int flags) { (void)flags; return 1; }

#else

#define AGENT_PIPE_NAME   "\\\\.\\pipe\\relic_agent"
#define AGENT_LINE_MAX    2048
#define AGENT_REPLY_MAX   (64 * 1024)
#define AGENT_PATH_MAX    512
#define AGENT_MEM_MAX     8192
#define AGENT_NAME_MAX    64
#define AGENT_OBJ_MAX     48
#define AGENT_SYM_GROW    512
#define AGENT_SYMS_DEF    200

/* THE CLIENT HAS A TIMEOUT AND THE DEMO DOES NOT. An unoptimised frame
 * is slow enough that a careless RUN_TO would outlast the MCP call and
 * lose its reply, so a run is bounded by BOTH a tick count and a wall
 * clock - and a truncated run says so rather than looking complete. */
#define AGENT_STEP_MAX    20000
#define AGENT_BUDGET_MS   20000
#define AGENT_DUMP_MAX    100
#define AGENT_DUMPMEM_MAX (16 * 1024 * 1024)
#define AGENT_RES_MAX_W   1280
#define AGENT_RES_MAX_H   720
#define AGENT_RES_DEF_W   320
#define AGENT_RES_DEF_H   256

typedef struct {
	char   name[AGENT_NAME_MAX];
	char   obj[AGENT_OBJ_MAX];
	void  *addr;
	void  *bound;           /* the next symbol - A BOUND, NOT AN END */
	char   kind;            /* 'f' or 'd', from the map's flag column */
} Agent_Sym;

static Agent_Sym *s_sym;
static int        s_syms;
static void      *s_base;       /* where the image actually loaded */
static long       s_slide;      /* that, less the map's own base   */

static volatile LONG s_up;
static HANDLE   s_thread;
static HANDLE   s_pipe;
static HANDLE   s_reqEvent;
static HANDLE   s_repEvent;
static char     s_req[AGENT_LINE_MAX];
static char     s_rep[AGENT_REPLY_MAX];

static int      s_quit;
static int      s_free;         /* free-running, i.e. not paused */
static int      s_dumps;
static s32      s_resW = AGENT_RES_DEF_W;
static s32      s_resH = AGENT_RES_DEF_H;
static s32      s_shown = -1;   /* the tick the pixels show, -1 = none */
static char     s_dumpDir[AGENT_PATH_MAX];
static char     s_mapPath[AGENT_PATH_MAX];
static char     s_crashPath[AGENT_PATH_MAX];
static int      s_mapStale;
static unsigned s_penterCalls;
static unsigned s_pexitCalls;

static LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter;


/* ---- paths and files ----------------------------------------------- */

/* OURS. The module's own path with its extension replaced, which is
 * how the map is found without anybody configuring a path. */
static int agent_sibling(const char *ext, char *out, int cap)
{
	char exe[MAX_PATH];
	int n, i, cut;

	if (!GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe)) return 1;
	n = (int)strlen(exe);
	cut = n;
	for (i = n - 1; i >= 0; i--) {
		if (exe[i] == '\\' || exe[i] == '/') break;
		if (exe[i] == '.') { cut = i; break; }
	}
	if (cut + (int)strlen(ext) + 1 > cap) return 1;
	memcpy(out, exe, (size_t)cut);
	strcpy(out + cut, ext);
	return 0;
}

/* OURS */
static int agent_filetime(const char *path, FILETIME *ft)
{
	WIN32_FILE_ATTRIBUTE_DATA d;

	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 1;
	*ft = d.ftLastWriteTime;
	return 0;
}

/* OURS. A map older than the exe beside it makes every symbol address
 * silently wrong, and two builds sharing one output directory is the
 * normal way that happens. */
static void agent_check_stale(void)
{
	char exe[MAX_PATH];
	FILETIME mt, et;

	s_mapStale = 0;
	if (!GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe)) return;
	if (agent_filetime(s_mapPath, &mt) || agent_filetime(exe, &et)) return;
	if (CompareFileTime(&mt, &et) < 0) s_mapStale = 1;
}

/* OURS */
static void agent_path(const char *rel, char *out, int cap)
{
	if (rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':'))
		_snprintf(out, (size_t)cap - 1, "%s", rel);
	else
		_snprintf(out, (size_t)cap - 1, "%s/%s", s_dumpDir, rel);
	out[cap - 1] = 0;
}


/* ---- the map file -------------------------------------------------- */

/* OURS. A token ends at whitespace OF ANY KIND, the line ending
 * included: fgets keeps the newline, so a run that stopped only at a
 * space or a tab let the last field on a line - the object name -
 * absorb it, and every reply quoting one then broke in half. */
static int agent_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r'
	    || c == '\n';
}

/* OURS */
static const char *agent_token(const char *p, char *out, int cap)
{
	int n = 0;

	if (!p) { out[0] = 0; return NULL; }
	while (agent_space(*p)) p++;
	if (!*p) { out[0] = 0; return NULL; }
	while (*p && !agent_space(*p)) {
		if (n + 1 < cap) out[n++] = *p;
		p++;
	}
	out[n] = 0;
	return p;
}

/* OURS. '#' is ours - it separates a name from its object in a query -
 * and the rest are MSVC's decorated and section names, none of which
 * is a symbol anybody here would ask for. */
static int agent_name_junk(const char *s)
{
	return strpbrk(s, "?@$!#") != NULL;
}

/* OURS */
static int agent_sym_cmp(const void *a, const void *b)
{
	const Agent_Sym *A = (const Agent_Sym *)a;
	const Agent_Sym *B = (const Agent_Sym *)b;

	if (A->addr < B->addr) return -1;
	if (A->addr > B->addr) return  1;
	return strcmp(A->name, B->name);
}

/* OURS */
static int agent_sym_add(const char *name, const char *obj,
                         void *addr, char kind)
{
	Agent_Sym *grown;

	if ((s_syms % AGENT_SYM_GROW) == 0) {
		grown = (Agent_Sym *)realloc(s_sym,
		                             (size_t)(s_syms + AGENT_SYM_GROW)
		                             * sizeof(Agent_Sym));
		if (!grown) return 1;
		s_sym = grown;
	}
	memset(&s_sym[s_syms], 0, sizeof(Agent_Sym));
	_snprintf(s_sym[s_syms].name, AGENT_NAME_MAX - 1, "%s", name);
	_snprintf(s_sym[s_syms].obj, AGENT_OBJ_MAX - 1, "%s", obj);
	s_sym[s_syms].addr = addr;
	s_sym[s_syms].kind = kind;
	s_syms++;
	return 0;
}

/*
 * OURS. Both of the link map's symbol tables, in one pass.
 *
 * THE "Address Publics by Value" HEADER IS LOAD-BEARING. The section
 * table at the top of the map has the same column shape as a symbol
 * line, so a parser that only tests for ':' in column 5 ingests
 * section lengths as symbols. Entering on that header and never
 * leaving is also what picks up the "Static symbols" table below the
 * entry point, where 224 of this tree's 230 static functions live.
 *
 * Tokenised rather than read at fixed offsets, because the flag
 * column is present for a function and absent for a datum, and the
 * object name after it is what tells twelve g_ready apart.
 */
static int agent_map_load(void)
{
	FILE *f;
	char line[1024];
	int state = 0, i, j;

	s_syms = 0;
	s_slide = 0;
	s_base = (void *)GetModuleHandleA(NULL);
	if (agent_sibling(".map", s_mapPath, AGENT_PATH_MAX)) return 1;
	agent_check_stale();
	f = fopen(s_mapPath, "r");
	if (!f) return 1;

	while (fgets(line, (int)sizeof line, f)) {
		char tok[5][128];
		const char *p = line;
		unsigned long rva;
		char *end;
		int nt = 0;

		if (state == 0) {
			/* THE MAP IS WRITTEN FOR THE PREFERRED BASE AND THE IMAGE
			 * DOES NOT LOAD THERE. /DYNAMICBASE relocates it on every
			 * run, so the slide between the two has to be applied to
			 * every symbol - without it an address looks entirely
			 * plausible and reading it faults. */
			if (!strncmp(line, " Preferred load address is ", 27))
				s_slide = (long)((char *)s_base
				                 - (long)strtoul(line + 27, NULL, 16));
			if (!strncmp(line, "  Address         Publics", 25)) state = 1;
			continue;
		}
		if (line[5] != ':') continue;

		while (nt < 5 && (p = agent_token(p, tok[nt], 128)) != NULL) nt++;
		if (nt < 4) continue;
		if (agent_name_junk(tok[1])) continue;

		rva = strtoul(tok[2], &end, 16);
		if (rva == 0 || *end != 0) continue;

		/* One leading cdecl underscore, as the linker decorated it. */
		if (tok[1][0] == '_') memmove(tok[1], tok[1] + 1, strlen(tok[1]));

		if (!strcmp(tok[3], "f") && nt >= 5)
			agent_sym_add(tok[1], tok[4], (char *)rva + s_slide, 'f');
		else
			agent_sym_add(tok[1], tok[3], (char *)rva + s_slide, 'd');
	}
	fclose(f);
	if (s_syms < 1) return 1;

	qsort(s_sym, (size_t)s_syms, sizeof(Agent_Sym), agent_sym_cmp);

	/* Forward to the next DIFFERENT address: COMDAT folding puts
	 * aliases on one address and the immediate next entry would give
	 * an empty range. */
	for (i = 0; i < s_syms; i++) {
		for (j = i + 1; j < s_syms; j++)
			if (s_sym[j].addr != s_sym[i].addr) break;
		s_sym[i].bound = (j < s_syms)
		               ? s_sym[j].addr
		               : (void *)((char *)s_sym[i].addr + 0x10000);
	}
	return 0;
}

/* OURS. Case-insensitive substring, so "part_temple" finds
 * "part_temple.obj" and "MSVCRTD:_init_.obj" answers to "_init_". */
static int agent_obj_match(const char *obj, const char *want)
{
	size_t n = strlen(want), m = strlen(obj), i, k;

	if (n == 0) return 1;
	if (n > m) return 0;
	for (i = 0; i + n <= m; i++) {
		for (k = 0; k < n; k++) {
			int a = obj[i + k], b = want[k];
			if (a >= 'A' && a <= 'Z') a += 32;
			if (b >= 'A' && b <= 'Z') b += 32;
			if (a != b) break;
		}
		if (k == n) return 1;
	}
	return 0;
}

/* OURS. "name" or "name#obj" into its two halves. '#' and not '@',
 * because a name full of '@' is exactly what the map filter throws
 * away - so '#' cannot collide with a symbol that survived it. */
static void agent_sym_split(const char *query, char *name, char *obj)
{
	const char *hash = strchr(query, '#');

	obj[0] = 0;
	if (hash) {
		size_t n = (size_t)(hash - query);
		if (n >= AGENT_NAME_MAX) n = AGENT_NAME_MAX - 1;
		memcpy(name, query, n);
		name[n] = 0;
		_snprintf(obj, AGENT_OBJ_MAX - 1, "%s", hash + 1);
	} else {
		_snprintf(name, AGENT_NAME_MAX - 1, "%s", query);
	}
	if (name[0] == '_') memmove(name, name + 1, strlen(name));
}

/* OURS */
static int agent_sym_is(int i, const char *name, const char *obj)
{
	return strcmp(s_sym[i].name, name) == 0
	    && agent_obj_match(s_sym[i].obj, obj);
}

/* OURS. How many symbols the query names, with the first in *out.
 * A bare name is ambiguous twelve ways for g_ready, so the COUNT is
 * the answer and the caller refuses on more than one. */
static int agent_sym_find(const char *query, int *out)
{
	char name[AGENT_NAME_MAX], obj[AGENT_OBJ_MAX];
	int i, hits = 0;

	agent_sym_split(query, name, obj);
	for (i = 0; i < s_syms; i++) {
		if (!agent_sym_is(i, name, obj)) continue;
		if (hits == 0 && out) *out = i;
		hits++;
	}
	return hits;
}

/* OURS. An address as "name+0xNN#obj", which is what a crash report
 * needs and what SYM answers for a raw address. */
static void agent_sym_at(void *p, char *out, int cap)
{
	int i, best = -1;

	out[0] = 0;
	for (i = 0; i < s_syms; i++) {
		if (s_sym[i].addr > p) break;
		best = i;
	}
	if (best < 0 || p >= s_sym[best].bound) {
		_snprintf(out, (size_t)cap - 1, "?");
		return;
	}
	if (p == s_sym[best].addr)
		_snprintf(out, (size_t)cap - 1, "%s#%s",
		          s_sym[best].name, s_sym[best].obj);
	else
		_snprintf(out, (size_t)cap - 1, "%s+0x%X#%s", s_sym[best].name,
		          (unsigned)((char *)p - (char *)s_sym[best].addr),
		          s_sym[best].obj);
}


/* ---- request parsing ----------------------------------------------- */

/* OURS. Pull "key=value" out of a request line. */
static int agent_arg(const char *line, const char *key, char *out, int cap)
{
	size_t klen = strlen(key);
	const char *p = line;

	while ((p = strstr(p, key)) != NULL) {
		if ((p == line || p[-1] == ' ') && p[klen] == '=') {
			const char *v = p + klen + 1;
			int n = 0;
			while (v[n] && v[n] != ' ' && n + 1 < cap) n++;
			memcpy(out, v, (size_t)n);
			out[n] = 0;
			return 1;
		}
		p += klen;
	}
	out[0] = 0;
	return 0;
}

/* OURS */
static int agent_u32(const char *s, unsigned *out)
{
	char *end = NULL;
	unsigned long v;

	if (!s || !s[0]) return 0;
	v = strtoul(s, &end, 0);
	if (end == s || (end && *end != 0)) return 0;
	*out = (unsigned)v;
	return 1;
}

/* OURS */
static int agent_arg_u32(const char *line, const char *key, unsigned *out)
{
	char av[64];

	if (!agent_arg(line, key, av, (int)sizeof av)) return 0;
	return agent_u32(av, out);
}

/* OURS */
static int agent_hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* OURS. SEH rather than VirtualQuery: a page can be unmapped between
 * the query and the access, and catching the fault is the only check
 * that cannot be raced. */
static int agent_safe_copy(void *dst, const void *src, size_t n)
{
	__try {
		memcpy(dst, src, n);
		return 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

/* OURS. at=0xADDR, or sym=NAME[#OBJ] with an optional off=. */
static int agent_resolve(const char *line, unsigned *addr,
                         char *out, int cap)
{
	char av[160], force[16];
	unsigned off = 0;
	int idx = 0, hits;

	if (agent_arg_u32(line, "at", addr)) {
		agent_arg_u32(line, "off", &off);
		*addr += off;
		return 1;
	}
	if (!agent_arg(line, "sym", av, (int)sizeof av)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG want=at|sym\n");
		return 0;
	}
	if (s_mapStale && !agent_arg(line, "force", force, (int)sizeof force)) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_MAP_STALE map=%s - rebuild, or pass force=1\n",
		          s_mapPath);
		return 0;
	}
	hits = agent_sym_find(av, &idx);
	if (hits == 0) {
		_snprintf(out, (size_t)cap - 1, "ERR_NO_SYMBOL sym=%s\n", av);
		return 0;
	}
	if (hits > 1) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_AMBIGUOUS sym=%s n=%d - qualify it as name#obj\n",
		          av, hits);
		return 0;
	}
	agent_arg_u32(line, "off", &off);
	*addr = (unsigned)(uintptr_t)s_sym[idx].addr + off;
	return 1;
}


/* ---- the hook table ------------------------------------------------ */

/*
 * OURS. Instrumentation that does not stop the demo.
 *
 * SINGLE THREADED, AND THAT REMOVES EVERY LOCK. _penter fires inside
 * Demo_Frame, which Agent_Pump calls, and SET_HOOK and GET_TRACE are
 * dispatched by that same pump on that same thread - so arming never
 * races dispatch and the ring has one producer and one consumer that
 * are never both inside it. A MULTI-THREADED TARGET would need a
 * critical section here, a TryEnter in dispatch and a counted drop
 * for a busy lock. None of that is needed here, and none of it is
 * present.
 *
 * AN INSTRUMENTED CALL COSTS 12 ns AND part_temple MAKES 94,000 OF
 * THEM A FRAME [agent_mcp.txt 4.3]. Anything per-call is multiplied by
 * that, so the armed count is tested in the asm stub before the call
 * into C: a session with nothing armed pays one load and one branch.
 */

#define AGENT_HOOK_MAX    16
#define AGENT_ARGS_MAX    9      /* a0..a8, and aN stays one digit */
#define AGENT_CAP_MAX     9      /* a0..a8 of fill_rows, at once */
#define AGENT_CAP_BYTES   16384
#define AGENT_SPEC_MAX    192
#define AGENT_TAG_MAX     32
#define AGENT_DEPTH_MAX   64
#define AGENT_RING_SIZE   (1024 * 1024)
#define AGENT_TRACE_DEF   64

#define CAP_ARG  0              /* aN - the 4 bytes at [esp+8+4N]    */
#define CAP_MEM  1              /* @sym#obj+OFF:LEN, resolved armed  */
#define CAP_ABS  2              /* 0xADDR:LEN, resolved when armed   */
#define CAP_PTR  3              /* *aN+OFF:LEN, resolved per call    */
#define CAP_RET  4              /* ret - the saved eax, exit only    */
#define CAP_CALL 5              /* caller - frame[1], both halves    */
#define CAP_RETP 6              /* *ret+OFF:LEN - follow it, exit    */

#define WHEN_NONE  0
#define WHEN_ENTER 1            /* decided before the call runs      */
#define WHEN_EXIT  2            /* decided after it, so enter waits  */

#define OP_EQ 0
#define OP_NE 1
#define OP_LT 2
#define OP_LE 3
#define OP_GT 4
#define OP_GE 5

#define REC_ENTER 0
#define REC_EXIT  1
#define REC_DELTA 2

typedef struct {
	int       kind;
	int       argn;             /* ARG and PTR */
	unsigned  off;              /* MEM and PTR */
	unsigned  len;
	void     *addr;             /* MEM and ABS, resolved when armed */
} Agent_Cap;

typedef struct {
	void      *start;
	void      *bound;           /* the next symbol - exit needs it   */
	char       name[AGENT_NAME_MAX];
	char       obj[AGENT_OBJ_MAX];
	char       tag[AGENT_TAG_MAX];
	int        id;              /* 0 means the slot is free          */
	int        nargs;           /* declared by the client, 0..8      */
	unsigned   maxHits;         /* 0 means unlimited                 */
	unsigned   hits;
	Agent_Cap  enter[AGENT_CAP_MAX];
	int        nEnter;
	unsigned   enterBytes;
	char       enterSpec[AGENT_SPEC_MAX];
	Agent_Cap  exitc[AGENT_CAP_MAX];
	int        nExit;
	unsigned   exitBytes;
	char       exitSpec[AGENT_SPEC_MAX];
	int        wantDelta;
	Agent_Cap  agg;             /* what the accumulator reads        */
	int        aggAt;           /* WHEN_* , or WHEN_NONE             */
	char       aggSpec[AGENT_SPEC_MAX];
	unsigned   aggN;
	long       aggLo, aggHi;
	__int64    aggSum;
	Agent_Cap  when;            /* what the predicate reads          */
	int        whenAt;          /* WHEN_*                            */
	int        whenOp;
	long       whenVal;
	char       whenSpec[AGENT_SPEC_MAX];
	unsigned   recs;            /* calls RECORDED, which max bounds  */
} Agent_Hook;

static Agent_Hook s_hook[AGENT_HOOK_MAX];
static int        s_nextId = 1;

/* The dispatch sets, dense and separate from the slots, so the scan
 * is over armed hooks and not over the whole table. */
static void *s_armedAddr[AGENT_HOOK_MAX];
static int   s_armedSlot[AGENT_HOOK_MAX];
static long  s_armedCount;

static void *s_exitLo[AGENT_HOOK_MAX];
static void *s_exitHi[AGENT_HOOK_MAX];
static int   s_exitSlot[AGENT_HOOK_MAX];
static long  s_exitCount;

/* Nothing under src/ is reachable from a capture, because agent.c is
 * never built with /Gh - this is insurance against that changing. */
static int s_inHook;

static unsigned char *s_ring;
static unsigned s_ringHead;
static unsigned s_ringTail;
static unsigned s_seq;
static unsigned s_pending;
static unsigned s_dropFull;
static unsigned s_unpaired;

/* One static scratch buffer and not a local: a capture list may be
 * 16 KB and _penter can be several frames deep when it fires. */
static unsigned char s_capBuf[AGENT_CAP_BYTES];
static unsigned char s_dltBuf[AGENT_CAP_BYTES];

typedef struct {
	int      slot;
	int      pass;              /* the filter: 1, 0, or -1 pending   */
	int      emit;              /* pass AND inside max               */
	int      aggHeld;           /* an enter value a later filter owns */
	long     aggVal;
	unsigned snapAt;
	unsigned snapLen;
} Agent_Live;

static Agent_Live    s_live[AGENT_DEPTH_MAX];
static int           s_depth;
static unsigned char s_snap[AGENT_CAP_BYTES];
static unsigned      s_snapUsed;

typedef struct {
	unsigned seq;
	unsigned tick;
	int      id;
	int      kind;
	unsigned len;
} Agent_Rec;

extern void _penter(void);


/* OURS. A FIFO over a flat buffer, compacted when it is drained.
 * A circular one would have to split a record across the wrap, and
 * the only thing that buys is the handful of bytes compaction moves. */
static int agent_ring_put(int id, int kind,
                          const unsigned char *payload, unsigned len)
{
	Agent_Rec r;

	if (!s_ring) return 0;
	if (s_ringHead + sizeof r + len > AGENT_RING_SIZE) {
		/* THE NEWEST RECORD IS DROPPED, not the oldest: discarding the
		 * oldest means parsing forward from the read cursor to find
		 * where it ends, on the render thread, to make room for a
		 * record nobody has asked for yet. */
		s_dropFull++;
		return 0;
	}
	r.seq = ++s_seq;
	r.tick = (unsigned)Demo_TickCount();
	r.id = id;
	r.kind = kind;
	r.len = len;
	memcpy(s_ring + s_ringHead, &r, sizeof r);
	s_ringHead += sizeof r;
	if (len) {
		memcpy(s_ring + s_ringHead, payload, len);
		s_ringHead += len;
	}
	s_pending++;
	return 1;
}

/* OURS. The payload points INTO the ring and stays valid until the
 * next compaction, which is why cmd_get_trace drains before it
 * compacts. */
static int agent_ring_get(Agent_Rec *r, const unsigned char **payload)
{
	if (!s_ring || s_ringTail + sizeof *r > s_ringHead) return 0;
	memcpy(r, s_ring + s_ringTail, sizeof *r);
	*payload = s_ring + s_ringTail + sizeof *r;
	s_ringTail += sizeof *r + r->len;
	s_pending--;
	return 1;
}

/* OURS */
static void agent_ring_compact(void)
{
	unsigned left;

	if (!s_ring || s_ringTail == 0) return;
	left = s_ringHead - s_ringTail;
	if (left) memmove(s_ring, s_ring + s_ringTail, left);
	s_ringHead = left;
	s_ringTail = 0;
}


/* OURS. One capture list item into a slot. Returns 0 and fills err. */
static int agent_cap_item(const char *tok, Agent_Cap *c, int nargs,
                          int isExit, char *err, int ecap)
{
	const char *colon, *plus;
	char work[AGENT_SPEC_MAX];
	unsigned long v;
	char *end;

	memset(c, 0, sizeof *c);
	if (!strcmp(tok, "caller")) {
		c->kind = CAP_CALL;
		c->len = 4;
		return 1;
	}
	if (!strcmp(tok, "ret")) {
		/* EXIT ONLY, because on entry that slot holds whatever the
		 * caller happened to leave in eax. */
		if (!isExit) {
			_snprintf(err, (size_t)ecap - 1,
			          "ERR_BAD_ARG ret is exit only\n");
			return 0;
		}
		c->kind = CAP_RET;
		c->len = 4;
		return 1;
	}
	if (tok[0] == 'a' && tok[1] >= '0' && tok[1] <= '9' && !tok[2]) {
		c->kind = CAP_ARG;
		c->argn = tok[1] - '0';
		c->len = 4;
		if (c->argn >= nargs) {
			_snprintf(err, (size_t)ecap - 1,
			          "ERR_BAD_ARG %s needs args=%d or more\n",
			          tok, c->argn + 1);
			return 0;
		}
		return 1;
	}

	colon = strrchr(tok, ':');
	if (!colon) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG %s has no :LEN\n", tok);
		return 0;
	}
	v = strtoul(colon + 1, &end, 0);
	if (end == colon + 1 || *end || v == 0 || v > AGENT_CAP_BYTES) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG %s has a bad LEN (max %d)\n",
		          tok, AGENT_CAP_BYTES);
		return 0;
	}
	c->len = (unsigned)v;

	if ((size_t)(colon - tok) >= sizeof work) {
		_snprintf(err, (size_t)ecap - 1, "ERR_BAD_ARG item too long\n");
		return 0;
	}
	memcpy(work, tok, (size_t)(colon - tok));
	work[colon - tok] = 0;

	plus = strrchr(work, '+');
	if (plus) {
		v = strtoul(plus + 1, &end, 0);
		if (end == plus + 1 || *end) {
			_snprintf(err, (size_t)ecap - 1,
			          "ERR_BAD_ARG %s has a bad +OFF\n", tok);
			return 0;
		}
		c->off = (unsigned)v;
		*(char *)plus = 0;
	}

	if (work[0] == '*') {
		/* 74 of the instrumented functions return a pointer, and for
		 * the node and cell lookups the thing pointed at IS the
		 * answer. Same resolution rule as *aN: per call, because the
		 * address is not knowable until there is one. */
		if (!strcmp(work + 1, "ret")) {
			if (!isExit) {
				_snprintf(err, (size_t)ecap - 1,
				          "ERR_BAD_ARG *ret is exit only\n");
				return 0;
			}
			c->kind = CAP_RETP;
			return 1;
		}
		if (work[1] != 'a' || work[2] < '0' || work[2] > '9' || work[3]) {
			_snprintf(err, (size_t)ecap - 1,
			          "ERR_BAD_ARG %s is not *aN+OFF:LEN or"
			          " *ret+OFF:LEN\n", tok);
			return 0;
		}
		c->kind = CAP_PTR;
		c->argn = work[2] - '0';
		if (c->argn >= nargs) {
			_snprintf(err, (size_t)ecap - 1,
			          "ERR_BAD_ARG %s needs args=%d or more\n",
			          tok, c->argn + 1);
			return 0;
		}
		return 1;
	}

	if (work[0] == '@') {
		int idx = 0, hits = agent_sym_find(work + 1, &idx);
		if (hits == 0) {
			_snprintf(err, (size_t)ecap - 1,
			          "ERR_NO_SYMBOL sym=%s\n", work + 1);
			return 0;
		}
		if (hits > 1) {
			_snprintf(err, (size_t)ecap - 1,
			          "ERR_AMBIGUOUS sym=%s n=%d - qualify it as"
			          " name#obj\n", work + 1, hits);
			return 0;
		}
		c->kind = CAP_MEM;
		c->addr = (char *)s_sym[idx].addr + c->off;
		return 1;
	}

	v = strtoul(work, &end, 0);
	if (end == work || *end || v == 0) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG %s is not aN, @sym, *aN or 0xADDR\n", tok);
		return 0;
	}
	c->kind = CAP_ABS;
	c->addr = (char *)v + c->off;
	return 1;
}

/* OURS. A comma separated list, and on the exit side the word delta. */
static int agent_cap_list(const char *spec, Agent_Cap *out, int *n,
                          unsigned *bytes, int *wantDelta,
                          int nargs, char *err, int ecap)
{
	char work[AGENT_SPEC_MAX];
	char *p;

	*n = 0;
	*bytes = 0;
	if (!spec || !spec[0]) return 1;
	if (strlen(spec) >= sizeof work) {
		_snprintf(err, (size_t)ecap - 1, "ERR_BAD_ARG list too long\n");
		return 0;
	}
	strcpy(work, spec);
	p = work;
	while (*p) {
		char *comma = strchr(p, ',');
		if (comma) *comma = 0;
		if (!strcmp(p, "delta")) {
			if (!wantDelta) {
				_snprintf(err, (size_t)ecap - 1,
				          "ERR_BAD_ARG delta is exit only\n");
				return 0;
			}
			*wantDelta = 1;
		} else {
			if (*n >= AGENT_CAP_MAX) {
				_snprintf(err, (size_t)ecap - 1,
				          "ERR_RANGE items=%d max=%d\n",
				          *n + 1, AGENT_CAP_MAX);
				return 0;
			}
			if (!agent_cap_item(p, &out[*n], nargs, wantDelta != NULL,
			                    err, ecap)) return 0;
			*bytes += out[*n].len;
			(*n)++;
			if (*bytes > AGENT_CAP_BYTES) {
				_snprintf(err, (size_t)ecap - 1,
				          "ERR_RANGE bytes=%u max=%d\n",
				          *bytes, AGENT_CAP_BYTES);
				return 0;
			}
		}
		if (!comma) break;
		p = comma + 1;
	}
	return 1;
}


/*
 * OURS. agg=ITEM - one scalar, totalled over every call that passes
 * the filter, reported by LIST_HOOK and never written to the ring.
 *
 * Which half it is read on follows the item, exactly as when= does.
 */
static int agent_agg_parse(const char *spec, Agent_Hook *h,
                           char *err, int ecap)
{
	if (!agent_cap_item(spec, &h->agg, h->nargs, 1, err, ecap)) return 0;
	if (h->agg.len != 4) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG agg=%s reads %u bytes - a total is on"
		          " four\n", spec, h->agg.len);
		return 0;
	}
	h->aggAt = (h->agg.kind == CAP_RET || h->agg.kind == CAP_RETP)
	         ? WHEN_EXIT : WHEN_ENTER;
	_snprintf(h->aggSpec, AGENT_SPEC_MAX - 1, "%s", spec);
	return 1;
}

/*
 * OURS. when=ITEM OP VALUE, as one token - ret!=-1, a0>1000.
 *
 * WHICH HALF IT IS DECIDED ON COMES FROM THE ITEM. A predicate on ret
 * cannot be answered until the function has run, so it is an exit
 * test; everything else is knowable on the way in and is tested
 * there, where rejecting costs nothing but the capture already taken.
 *
 * A symbol name can hold none of < > ! = - the map filter drops any
 * that could - so the first of those characters is the operator and
 * never part of the item.
 */
static int agent_when_parse(const char *spec, Agent_Hook *h,
                            char *err, int ecap)
{
	char item[AGENT_SPEC_MAX];
	const char *op = strpbrk(spec, "!<>=");
	char *end;
	size_t n;
	int skip = 1;

	if (!op) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG when=%s wants ITEM OP VALUE, as one"
		          " token - ret!=-1\n", spec);
		return 0;
	}
	if (op[1] == '=') {
		skip = 2;
		switch (op[0]) {
		case '=': h->whenOp = OP_EQ; break;
		case '!': h->whenOp = OP_NE; break;
		case '<': h->whenOp = OP_LE; break;
		case '>': h->whenOp = OP_GE; break;
		default:  skip = 0; break;
		}
	} else {
		switch (op[0]) {
		case '<': h->whenOp = OP_LT; break;
		case '>': h->whenOp = OP_GT; break;
		default:  skip = 0; break;
		}
	}
	if (!skip) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG when=%s - the operator is one of"
		          " == != < <= > >=\n", spec);
		return 0;
	}

	n = (size_t)(op - spec);
	if (n == 0 || n >= sizeof item) {
		_snprintf(err, (size_t)ecap - 1, "ERR_BAD_ARG when=%s\n", spec);
		return 0;
	}
	memcpy(item, spec, n);
	item[n] = 0;
	/* isExit, so ret parses here and settles whenAt below. */
	if (!agent_cap_item(item, &h->when, h->nargs, 1, err, ecap)) return 0;
	if (h->when.len != 4) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG when=%s reads %u bytes - a test is on"
		          " four\n", spec, h->when.len);
		return 0;
	}

	h->whenVal = strtol(op + skip, &end, 0);
	if (end == op + skip || *end) {
		_snprintf(err, (size_t)ecap - 1,
		          "ERR_BAD_ARG when=%s has a bad value\n", spec);
		return 0;
	}
	h->whenAt = (h->when.kind == CAP_RET) ? WHEN_EXIT : WHEN_ENTER;
	_snprintf(h->whenSpec, AGENT_SPEC_MAX - 1, "%s", spec);
	return 1;
}

/*
 * OURS. Read one capture list out of a live call.
 *
 * A READ THAT FAULTS IS ZERO FILLED, NOT SKIPPED, so every record
 * keeps the length its list implies and the client can split it
 * knowing only what it asked for. A NULL pointer argument therefore
 * reads as a run of zeroes, which is why capturing aN alongside *aN
 * is how you tell that from genuinely zeroed memory.
 */
static const void *agent_cap_src(const Agent_Cap *c, void **frame)
{
	switch (c->kind) {
	case CAP_ARG:
		return &frame[2 + c->argn];
	case CAP_MEM:
	case CAP_ABS:
		return c->addr;
	case CAP_PTR: {
		void *base = frame[2 + c->argn];
		return base ? (const void *)((const char *)base + c->off) : NULL; }
	case CAP_RET:
		/* pushad pushes eax first of the eight, so it is deepest:
		 * esp+32 once pushfd has gone on, against the esp+36 the stub
		 * recovers as the frame. */
		return &frame[-1];
	case CAP_CALL:
		/* frame[0] is the stub's own return, five bytes into the
		 * hooked function. frame[1] is that function's return
		 * address, the instruction after the call - so it names the
		 * caller. Identical at exit, where the epilogue has put the
		 * stack back. */
		return &frame[1];
	case CAP_RETP: {
		/* ret is the ADDRESS of the saved eax; this is its VALUE. */
		void *base = frame[-1];
		return base ? (const void *)((const char *)base + c->off) : NULL; }
	default:
		return NULL;
	}
}

/* OURS */
static unsigned agent_capture(const Agent_Cap *c, int n, void **frame,
                              unsigned char *out, unsigned cap)
{
	unsigned used = 0;
	int i;

	for (i = 0; i < n; i++) {
		const void *src = agent_cap_src(&c[i], frame);
		unsigned len = c[i].len;

		if (used + len > cap) break;
		if (!src || !agent_safe_copy(out + used, src, len))
			memset(out + used, 0, len);
		used += len;
	}
	return used;
}

/* OURS */
static int agent_read4(const Agent_Cap *c, void **frame, long *out)
{
	const void *src = agent_cap_src(c, frame);

	*out = 0;
	return src && agent_safe_copy(out, src, 4);
}

/* OURS. THE FILTER DECIDES WHAT IS COUNTED, max ONLY WHAT IS KEPT.
 * Otherwise "record the first ten and count them all" cannot be
 * asked, which is the whole point of an accumulator. */
static void agent_agg_add(Agent_Hook *h, long v)
{
	if (h->aggN == 0) {
		h->aggLo = v;
		h->aggHi = v;
	} else {
		if (v < h->aggLo) h->aggLo = v;
		if (v > h->aggHi) h->aggHi = v;
	}
	h->aggSum += v;
	h->aggN++;
}

/* OURS. The predicate, as a signed 32-bit compare. A read that faults
 * is false rather than zero: a hook asking about memory it cannot
 * reach should record nothing, not record everything. */
static int agent_when(const Agent_Hook *h, void **frame)
{
	long v = 0;

	if (!agent_read4(&h->when, frame, &v)) return 0;
	switch (h->whenOp) {
	case OP_EQ: return v == h->whenVal;
	case OP_NE: return v != h->whenVal;
	case OP_LT: return v <  h->whenVal;
	case OP_LE: return v <= h->whenVal;
	case OP_GT: return v >  h->whenVal;
	case OP_GE: return v >= h->whenVal;
	default:    return 1;
	}
}

/*
 * OURS. The changed ranges, as [u16 off][u16 len][bytes] runs.
 *
 * WHY THIS EXISTS: the question a hook is asked is almost always what
 * the function did to a struct, and two full dumps answer it only
 * after the client diffs both. For a routine touching a hundred bytes
 * of a four-kilobyte state this is a fortyfold reduction, and full
 * dumps stay because a comparison against the original needs absolute
 * values at a known point.
 */
static unsigned agent_delta(const unsigned char *now,
                            const unsigned char *was, unsigned len,
                            unsigned char *out, unsigned cap)
{
	unsigned i = 0, used = 0;

	while (i < len) {
		unsigned s, run;
		while (i < len && now[i] == was[i]) i++;
		if (i >= len) break;
		s = i;
		while (i < len && now[i] != was[i]) i++;
		run = i - s;
		if (used + 4 + run > cap) break;
		out[used++] = (unsigned char)(s & 0xff);
		out[used++] = (unsigned char)((s >> 8) & 0xff);
		out[used++] = (unsigned char)(run & 0xff);
		out[used++] = (unsigned char)((run >> 8) & 0xff);
		memcpy(out + used, now + s, run);
		used += run;
	}
	return used;
}


/* OURS. Rebuild both dispatch sets from the slot table. Safe to call
 * at any point, because it runs on the thread that dispatches. */
static void agent_rebuild(void)
{
	long na = 0, nx = 0;
	int i;

	for (i = 0; i < AGENT_HOOK_MAX; i++) {
		Agent_Hook *h = &s_hook[i];
		if (!h->id) continue;
		s_armedAddr[na] = h->start;
		s_armedSlot[na] = i;
		na++;
		/* A hook with no exit list costs nothing at every return in
		 * an instrumented file, which is most of them. */
		if (h->nExit || h->wantDelta || h->whenAt == WHEN_EXIT
		    || h->aggAt == WHEN_EXIT) {
			s_exitLo[nx] = h->start;
			s_exitHi[nx] = h->bound;
			s_exitSlot[nx] = i;
			nx++;
		}
	}
	s_armedCount = na;
	s_exitCount = nx;
}

/* OURS. The entry half. frame[0] is the return address, so the hooked
 * function begins five bytes below it; arguments start at frame[2]. */
static void agent_penter_hit(void **frame)
{
	void *pc = (void *)((char *)frame[0] - 5);
	long i;

	if (s_inHook) return;
	s_inHook = 1;
	for (i = 0; i < s_armedCount; i++) {
		Agent_Hook *h;
		unsigned n;
		long aggv;
		int emit, hold, pass, held;

		if (s_armedAddr[i] != pc) continue;
		h = &s_hook[s_armedSlot[i]];
		h->hits++;

		n = agent_capture(h->enter, h->nEnter, frame,
		                  s_capBuf, AGENT_CAP_BYTES);

		/* hold THE ENTER PAYLOAD when something later needs it: as
		 * the delta baseline, or because an exit predicate has not
		 * decided whether this call is worth recording at all. */
		hold = h->wantDelta || h->whenAt == WHEN_EXIT;
		pass = 1;
		if (h->whenAt == WHEN_ENTER)
			pass = agent_when(h, frame) ? 1 : 0;
		else if (h->whenAt == WHEN_EXIT)
			pass = -1;

		/* An enter-side total under an exit-side filter cannot be
		 * added yet, so it waits in the live entry with the rest. */
		held = 0;
		aggv = 0;
		if (h->aggAt == WHEN_ENTER && agent_read4(&h->agg, frame, &aggv)) {
			if (pass == 1) agent_agg_add(h, aggv);
			else if (pass < 0) held = 1;
		}

		/* max BOUNDS WHAT IS RECORDED, NOT WHAT IS CALLED, so a
		 * filter does not spend the budget on calls it rejects. */
		emit = pass;
		if (emit == 1) {
			if (h->maxHits && h->recs >= h->maxHits) emit = 0;
			else {
				h->recs++;
				if (h->nEnter)
					agent_ring_put(h->id, REC_ENTER, s_capBuf, n);
			}
		}

		/* PAIRING IS BY THIS THREAD'S OWN STACK and not by address,
		 * because a recursive function is on it more than once. */
		if (s_depth < AGENT_DEPTH_MAX
		    && s_snapUsed + n <= AGENT_CAP_BYTES) {
			s_live[s_depth].slot = s_armedSlot[i];
			s_live[s_depth].pass = pass;
			s_live[s_depth].emit = emit;
			s_live[s_depth].aggHeld = held;
			s_live[s_depth].aggVal = aggv;
			s_live[s_depth].snapAt = s_snapUsed;
			s_live[s_depth].snapLen = hold ? n : 0;
			if (hold) {
				memcpy(s_snap + s_snapUsed, s_capBuf, n);
				s_snapUsed += n;
			}
			s_depth++;
		}
		break;
	}
	s_inHook = 0;
}

/* OURS. The exit half. /GH emits its call after the epilogue, so this
 * return address is INSIDE the function body - the match is a range
 * test against [start, bound) and never a compare. */
static void agent_pexit_hit(void **frame)
{
	void *pc = frame[0];
	long i;

	if (s_inHook) return;
	s_inHook = 1;
	for (i = 0; i < s_exitCount; i++) {
		Agent_Hook *h;
		int slot, k, found = -1;
		unsigned n;

		if (pc < s_exitLo[i] || pc >= s_exitHi[i]) continue;
		slot = s_exitSlot[i];
		h = &s_hook[slot];

		for (k = s_depth - 1; k >= 0; k--)
			if (s_live[k].slot == slot) { found = k; break; }
		if (found < 0) {
			/* NOT ZERO BY DEFINITION. bound is the next map symbol, so
			 * an unhooked static living in the gap after a hooked
			 * function has its exit misattributed here. Counting them
			 * is what makes that visible. */
			s_unpaired++;
			break;
		}

		/* THE DECISION ARRIVES HERE for an exit predicate, and the
		 * enter record it was waiting on goes in first so the pair
		 * stays in order and a filtered call leaves nothing behind. */
		if (s_live[found].pass < 0) {
			int keep = agent_when(h, frame);
			s_live[found].pass = keep;
			if (keep && s_live[found].aggHeld)
				agent_agg_add(h, s_live[found].aggVal);
			if (keep && h->maxHits && h->recs >= h->maxHits) keep = 0;
			s_live[found].emit = keep;
			if (keep) {
				h->recs++;
				if (h->nEnter)
					agent_ring_put(h->id, REC_ENTER,
					               s_snap + s_live[found].snapAt,
					               s_live[found].snapLen);
			}
		}
		if (h->aggAt == WHEN_EXIT && s_live[found].pass == 1) {
			long v;
			if (agent_read4(&h->agg, frame, &v)) agent_agg_add(h, v);
		}
		if (!s_live[found].emit) {
			s_snapUsed = s_live[found].snapAt;
			s_depth = found;
			break;
		}

		if (h->nExit) {
			n = agent_capture(h->exitc, h->nExit, frame,
			                  s_capBuf, AGENT_CAP_BYTES);
			agent_ring_put(h->id, REC_EXIT, s_capBuf, n);
		}
		if (h->wantDelta && s_live[found].snapLen) {
			unsigned was = s_live[found].snapAt;
			unsigned len = s_live[found].snapLen;
			n = agent_capture(h->enter, h->nEnter, frame,
			                  s_capBuf, AGENT_CAP_BYTES);
			if (n > len) n = len;
			n = agent_delta(s_capBuf, s_snap + was, n,
			                s_dltBuf, AGENT_CAP_BYTES);
			agent_ring_put(h->id, REC_DELTA, s_dltBuf, n);
		}
		s_snapUsed = s_live[found].snapAt;
		s_depth = found;
		break;
	}
	s_inHook = 0;
}

/*
 * OURS. Does this function actually reach _penter?
 *
 * /Gh IS PER FILE AND THAT IS THE TRAP. A hook on a function in a
 * file built without the flag would arm, never fire, and be
 * indistinguishable from a function that is never called - so the
 * call site is read rather than assumed. The first five bytes of an
 * instrumented function are a relative call, and with incremental
 * linking it lands on a jmp thunk, which is one more hop to follow.
 */
static void *agent_follow(void *t)
{
	int i;

	for (i = 0; i < 3; i++) {
		unsigned char j[5];
		long rel;
		if (!agent_safe_copy(j, t, 5) || j[0] != 0xE9) break;
		memcpy(&rel, j + 1, 4);
		t = (char *)t + 5 + rel;
	}
	return t;
}

/* OURS. See agent_follow: the thunk is on both sides. */
static int agent_verify_penter(void *start, void **landed)
{
	unsigned char op[5];
	long rel;
	void *t;

	if (landed) *landed = NULL;
	if (!agent_safe_copy(op, start, 5)) return 0;
	if (op[0] != 0xE8) return 0;
	memcpy(&rel, op + 1, 4);
	t = agent_follow((char *)start + 5 + rel);
	if (landed) *landed = t;
	return t == agent_follow((void *)_penter);
}


/* ---- commands ------------------------------------------------------ */

/*
 * OURS. A reply whose header carries a count only the body can
 * determine: reserve a fixed-width field, fill the body after it, then
 * write the header into the reservation.
 *
 * _snprintf RETURNS -1 ON TRUNCATION, NOT THE LENGTH IT WANTED, and it
 * leaves the buffer unterminated as well. Taking that return as a
 * length is what memcpy'd (size_t)-1 over the reply buffer, so the
 * header is measured with strlen after an explicit terminate and
 * clamped to the field it has to fit.
 */
#define AGENT_HEAD 128

/* OURS */
static size_t agent_head_open(char *out)
{
	memset(out, ' ', AGENT_HEAD - 1);
	out[AGENT_HEAD - 1] = (char)10;
	out[AGENT_HEAD] = 0;
	return AGENT_HEAD;
}

/* OURS */
static void agent_head_close(char *out, char *text, size_t cap)
{
	size_t n;

	text[cap - 1] = 0;
	n = strlen(text);
	if (n > AGENT_HEAD - 1) n = AGENT_HEAD - 1;
	memcpy(out, text, n);
}

/* OURS */
static void cmd_status(char *out, int cap)
{
	s32 fw = 0, fh = 0;

	Demo_Pixels(&fw, &fh);
	_snprintf(out, (size_t)cap - 1,
	          "OK state=%s part=%d count=%d tick=%ld shown=%ld auto=%d "
	          "host=%ldx%ld frame=%ldx%ld syms=%d base=0x%08X "
	          "slide=0x%08X mapstale=%d "
	          "penter=%u pexit=%u armed=%ld dumps=%d/%d map=%s "
	          "crashlog=%s "
	          "label=%s\n",
	          s_free ? "running" : "paused",
	          Demo_PartIndex(), Demo_PartCount(),
	          (long)Demo_TickCount(), (long)s_shown, Demo_AutoAdvance(),
	          (long)s_resW, (long)s_resH, (long)fw, (long)fh,
	          s_syms, (unsigned)(uintptr_t)s_base, (unsigned)s_slide,
	          s_mapStale, s_penterCalls, s_pexitCalls, s_armedCount,
	          s_dumps, AGENT_DUMP_MAX, s_mapPath, s_crashPath,
	          Demo_PartLabel());
}

/* OURS */
static void cmd_caps(char *out, int cap)
{
	_snprintf(out, (size_t)cap - 1,
	          "OK target=relic_ppm verbs=HELLO,STATUS,CAPS,PARTS,GOTO,"
	          "NEXT,PREV,STEP,RUN_TO,PAUSE,RESUME,SET_RES,DUMP,"
	          "DUMP_RESET,DUMP_MEM,SYM,SYMS,GET_MEM,SET_MEM,"
	          "SET_HOOK,CLEAR_HOOK,CLEAR_TRACE,LIST_HOOK,GET_TRACE,"
	          "QUIT "
	          "stepmax=%d budgetms=%d dumpmax=%d memmax=%d "
	          "resmax=%dx%d dumpmemmax=%d hooks=yes hookmax=%d "
	          "argsmax=%d "
	          "capmax=%d capbytes=%d ring=%d depth=%d "
	          "capture=arg,mem,abs,ptr,ret,retp,caller,delta exit=yes"
	          " when=yes agg=yes\n",
	          AGENT_STEP_MAX, AGENT_BUDGET_MS, AGENT_DUMP_MAX,
	          AGENT_MEM_MAX, AGENT_RES_MAX_W, AGENT_RES_MAX_H,
	          AGENT_DUMPMEM_MAX,
	          AGENT_HOOK_MAX, AGENT_ARGS_MAX, AGENT_CAP_MAX,
	          AGENT_CAP_BYTES, AGENT_RING_SIZE, AGENT_DEPTH_MAX);
}

/* OURS */
static void cmd_parts(char *out, int cap)
{
	int n = Parts_Count(), i;
	size_t used;

	used = (size_t)_snprintf(out, (size_t)cap - 1, "OK lines=%d\n", n);
	for (i = 0; i < n; i++) {
		const Relic_Part *p = Parts_Get(i);
		if (!p || used + 256 >= (size_t)cap) break;
		used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
		                          "part=%d id=%s name=%s status=%s "
		                          "demo=%ldx%ld ticks=%ld origin=%s\n",
		                          i, p->id, p->name,
		                          p->status == PART_LIT ? "LIT" : "SHAPED",
		                          (long)p->demo_w, (long)p->demo_h,
		                          (long)p->ticks, p->origin);
	}
}

/* OURS. One tick: step, draw, resolve. The pixels then show the tick
 * BELOW the clock, because Demo_Frame renders and then advances - so
 * both numbers are reported and neither has to be inferred. */
static void agent_one_frame(void)
{
	s_shown = Demo_TickCount();
	Demo_Frame();
}

/* OURS */
static void cmd_goto(const char *line, char *out, int cap, int delta)
{
	unsigned want = 0;
	int bad;

	if (delta == 0) {
		if (!agent_arg_u32(line, "part", &want)) {
			_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=part\n");
			return;
		}
		if ((int)want >= Demo_PartCount()) {
			_snprintf(out, (size_t)cap - 1, "ERR_RANGE part=%u count=%d\n",
			          want, Demo_PartCount());
			return;
		}
		Demo_GotoPart((int)want);
	} else {
		Demo_StepPart(delta);
	}
	/* Demo_GotoPart re-fits the host size itself, and DISCARDS WHAT
	 * THAT RETURNS - so a part whose init() failed would draw a black
	 * frame and report nothing wrong. Asking again is the only way to
	 * get the answer: part 2.13 needs relic.dat, and without it every
	 * dump of it is empty and perfectly well formed. */
	bad = Demo_SetResolution(s_resW, s_resH);
	agent_one_frame();
	_snprintf(out, (size_t)cap - 1,
	          "OK part=%d tick=%ld shown=%ld init=%s label=%s\n",
	          Demo_PartIndex(), (long)Demo_TickCount(), (long)s_shown,
	          bad ? "FAILED" : "ok", Demo_PartLabel());
}

/* OURS. Bounded by ticks AND by wall clock - see AGENT_BUDGET_MS. */
static void agent_run(unsigned n, char *out, int cap)
{
	DWORD t0 = GetTickCount();
	unsigned done = 0;
	int cut = 0;

	while (done < n) {
		agent_one_frame();
		done++;
		if (GetTickCount() - t0 >= AGENT_BUDGET_MS && done < n) {
			cut = 1;
			break;
		}
	}
	_snprintf(out, (size_t)cap - 1,
	          "OK part=%d tick=%ld shown=%ld ran=%u asked=%u"
	          " truncated=%d ms=%lu\n",
	          Demo_PartIndex(), (long)Demo_TickCount(), (long)s_shown,
	          done, n, cut, (unsigned long)(GetTickCount() - t0));
}

/* OURS */
static void cmd_step(const char *line, char *out, int cap)
{
	unsigned n = 1;

	if (!agent_arg_u32(line, "n", &n)) n = 1;
	if (n == 0 || n > AGENT_STEP_MAX) {
		_snprintf(out, (size_t)cap - 1, "ERR_RANGE n=%u max=%d\n",
		          n, AGENT_STEP_MAX);
		return;
	}
	agent_run(n, out, cap);
}

/* OURS. THERE IS NO SEEK, ONLY REPLAY. A part with a step() callback
 * is stateful, so reaching a tick means running to it from the part's
 * own start - which is what GOTO gives you. */
static void cmd_run_to(const char *line, char *out, int cap)
{
	unsigned want = 0;
	s32 now = Demo_TickCount();

	if (!agent_arg_u32(line, "tick", &want)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=tick\n");
		return;
	}
	if ((s32)want <= now) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_NO_SEEK tick=%u now=%ld - GOTO the part first\n",
		          want, (long)now);
		return;
	}
	if (want - (unsigned)now > AGENT_STEP_MAX) {
		_snprintf(out, (size_t)cap - 1, "ERR_RANGE n=%u max=%d\n",
		          want - (unsigned)now, AGENT_STEP_MAX);
		return;
	}
	agent_run(want - (unsigned)now, out, cap);
}

/* OURS. Pausing takes auto-advance with it, so a step cannot drift
 * into the next part behind the client back. */
static void cmd_pause(char *out, int cap)
{
	s_free = 0;
	Demo_SetAutoAdvance(0);
	_snprintf(out, (size_t)cap - 1, "OK state=paused part=%d tick=%ld\n",
	          Demo_PartIndex(), (long)Demo_TickCount());
}

/* OURS */
static void cmd_resume(char *out, int cap)
{
	s_free = 1;
	Demo_SetAutoAdvance(1);
	_snprintf(out, (size_t)cap - 1, "OK state=running part=%d tick=%ld\n",
	          Demo_PartIndex(), (long)Demo_TickCount());
}

/* OURS */
static void cmd_set_res(const char *line, char *out, int cap)
{
	unsigned w = 0, h = 0;
	s32 fw = 0, fh = 0;

	if (!agent_arg_u32(line, "w", &w) || !agent_arg_u32(line, "h", &h)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG want=w,h\n");
		return;
	}
	if (w < 1 || h < 1 || w > AGENT_RES_MAX_W || h > AGENT_RES_MAX_H) {
		_snprintf(out, (size_t)cap - 1, "ERR_RANGE max=%dx%d\n",
		          AGENT_RES_MAX_W, AGENT_RES_MAX_H);
		return;
	}
	if (Demo_SetResolution((s32)w, (s32)h)) {
		_snprintf(out, (size_t)cap - 1, "ERR_RESOLUTION w=%u h=%u\n", w, h);
		return;
	}
	s_resW = (s32)w;
	s_resH = (s32)h;
	s_shown = -1;
	Demo_Pixels(&fw, &fh);
	_snprintf(out, (size_t)cap - 1, "OK host=%ux%u frame=%ldx%ld\n",
	          w, h, (long)fw, (long)fh);
}

/* OURS. The frame as it stands, never a render - a DUMP that drew
 * would move the clock as a side effect of looking at it. */
static void cmd_dump(const char *line, char *out, int cap)
{
	char rel[AGENT_PATH_MAX], path[AGENT_PATH_MAX];
	const u8 *rgb;
	s32 w = 0, h = 0;
	FILE *f;

	if (!agent_arg(line, "path", rel, (int)sizeof rel)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=path\n");
		return;
	}
	if (s_dumps >= AGENT_DUMP_MAX) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_DUMP_LIMIT n=%d max=%d - DUMP_RESET to clear\n",
		          s_dumps, AGENT_DUMP_MAX);
		return;
	}
	if (s_shown < 0) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_NO_FRAME - nothing rendered yet, STEP n=1 first\n");
		return;
	}
	rgb = Demo_Pixels(&w, &h);
	if (!rgb || w < 1 || h < 1) {
		_snprintf(out, (size_t)cap - 1, "ERR_NO_FRAME\n");
		return;
	}
	agent_path(rel, path, AGENT_PATH_MAX);
	f = fopen(path, "wb");
	if (!f) {
		_snprintf(out, (size_t)cap - 1, "ERR_IO path=%s\n", path);
		return;
	}
	fprintf(f, "P6\n%ld %ld\n255\n", (long)w, (long)h);
	fwrite(rgb, 1, (size_t)(w * h) * 3, f);
	fclose(f);
	s_dumps++;
	_snprintf(out, (size_t)cap - 1,
	          "OK path=%s w=%ld h=%ld part=%d shown=%ld dumps=%d/%d\n",
	          path, (long)w, (long)h, Demo_PartIndex(), (long)s_shown,
	          s_dumps, AGENT_DUMP_MAX);
}

/*
 * OURS. A region of memory straight to a file.
 *
 * GET_MEM ANSWERS IN HEX DOWN THE PIPE, which caps it at 8 KB and
 * costs two characters a byte. The buffers worth looking at are
 * bigger than that and not shaped like a reply: g_prev in the ruins
 * is 160 x 100 x 4, the per-pixel light that fog consumes, and the
 * frame never carries it - te_frame zeroes PX_LIGHT on the way out.
 * Sixty-four kilobytes is eight round trips and 128 KB of hex as a
 * read, and one call as a file.
 *
 * deref FOLLOWS THE SYMBOL RATHER THAN READING IT. g_prev is a
 * pointer variable, so without it the dump is the four bytes of the
 * pointer rather than the 64,000 it points at.
 *
 * TWO OFFSETS, BECAUSE THERE ARE TWO PLACES TO BE. off is address
 * arithmetic on the symbol and means there what it means everywhere
 * else; skip is applied last, after any deref, and is the offset
 * into the buffer being written out. One key doing both would have
 * to be applied twice and would be wrong once.
 *
 * COPIED A CHUNK AT A TIME under SEH. A region running off the end
 * of what is mapped stops and says how far it got, because a short
 * file that claims to be whole is worse than no file.
 */
static void cmd_dump_mem(const char *line, char *out, int cap)
{
	static unsigned char buf[AGENT_MEM_MAX];
	char rel[AGENT_PATH_MAX], path[AGENT_PATH_MAX];
	unsigned addr = 0, len = 0, done = 0, nonzero = 0, skip = 0;
	int cut = 0;
	FILE *f;

	if (!agent_arg(line, "path", rel, (int)sizeof rel)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=path\n");
		return;
	}
	if (!agent_arg_u32(line, "len", &len) || len == 0
	    || len > AGENT_DUMPMEM_MAX) {
		_snprintf(out, (size_t)cap - 1, "ERR_RANGE len=%u max=%d\n",
		          len, AGENT_DUMPMEM_MAX);
		return;
	}
	if (s_dumps >= AGENT_DUMP_MAX) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_DUMP_LIMIT n=%d max=%d - DUMP_RESET to clear\n",
		          s_dumps, AGENT_DUMP_MAX);
		return;
	}
	if (!agent_resolve(line, &addr, out, cap)) return;

	if (agent_arg_u32(line, "deref", &skip) && skip) {
		unsigned via = addr;
		if (!agent_safe_copy(&addr, (const void *)(uintptr_t)via, 4)) {
			_snprintf(out, (size_t)cap - 1,
			          "ERR_RANGE at=0x%08X unreadable as a pointer\n", via);
			return;
		}
		if (addr == 0) {
			_snprintf(out, (size_t)cap - 1,
			          "ERR_RANGE at=0x%08X holds a null pointer\n", via);
			return;
		}
	}
	skip = 0;
	agent_arg_u32(line, "skip", &skip);
	addr += skip;

	agent_path(rel, path, AGENT_PATH_MAX);
	f = fopen(path, "wb");
	if (!f) {
		_snprintf(out, (size_t)cap - 1, "ERR_IO path=%s\n", path);
		return;
	}
	while (done < len) {
		unsigned n = len - done;
		unsigned i;

		if (n > AGENT_MEM_MAX) n = AGENT_MEM_MAX;
		if (!agent_safe_copy(buf, (const void *)(uintptr_t)(addr + done),
		                     n)) {
			cut = 1;
			break;
		}
		for (i = 0; i < n; i++) if (buf[i]) nonzero++;
		if (fwrite(buf, 1, n, f) != n) { cut = 1; break; }
		done += n;
	}
	fclose(f);
	s_dumps++;
	_snprintf(out, (size_t)cap - 1,
	          "OK path=%s addr=0x%08X len=%u written=%u nonzero=%u"
	          " truncated=%d dumps=%d/%d\n",
	          path, addr, len, done, nonzero, cut, s_dumps,
	          AGENT_DUMP_MAX);
}

/* OURS */
static void cmd_dump_reset(char *out, int cap)
{
	s_dumps = 0;
	_snprintf(out, (size_t)cap - 1, "OK dumps=0/%d\n", AGENT_DUMP_MAX);
}

/* OURS */
static void cmd_sym(const char *line, char *out, int cap)
{
	char av[160], force[16], named[256];
	char qname[AGENT_NAME_MAX], qobj[AGENT_OBJ_MAX];
	unsigned addr = 0;
	int idx = 0, hits, i;
	size_t used;

	if (agent_arg_u32(line, "addr", &addr)) {
		agent_sym_at((void *)(uintptr_t)addr, named, (int)sizeof named);
		_snprintf(out, (size_t)cap - 1, "OK addr=0x%08X at=%s\n",
		          addr, named);
		return;
	}
	if (!agent_arg(line, "name", av, (int)sizeof av)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG want=name|addr\n");
		return;
	}
	if (s_mapStale && !agent_arg(line, "force", force, (int)sizeof force)) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_MAP_STALE map=%s - rebuild, or pass force=1\n",
		          s_mapPath);
		return;
	}
	hits = agent_sym_find(av, &idx);
	if (hits == 0) {
		_snprintf(out, (size_t)cap - 1, "ERR_NO_SYMBOL name=%s\n", av);
		return;
	}
	agent_sym_split(av, qname, qobj);
	used = (size_t)_snprintf(out, (size_t)cap - 1, "OK lines=%d%s\n",
	                         hits, hits > 1 ? " ambiguous=1" : "");
	for (i = 0; i < s_syms && used + 200 < (size_t)cap; i++) {
		if (!agent_sym_is(i, qname, qobj)) continue;
		used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
		                          "name=%s#%s addr=0x%08X bound=0x%08X"
		                          " kind=%c size=%u\n",
		                          s_sym[i].name, s_sym[i].obj,
		                          (unsigned)(uintptr_t)s_sym[i].addr,
		                          (unsigned)(uintptr_t)s_sym[i].bound,
		                          s_sym[i].kind,
		                          (unsigned)((char *)s_sym[i].bound
		                                   - (char *)s_sym[i].addr));
	}
}

/* OURS. The header carries the count, so it is written LAST into a
 * fixed-width field reserved up front - a count cannot be known
 * before the rows are filtered, and rewriting a shorter header would
 * move every row under it. */
static void cmd_syms(const char *line, char *out, int cap)
{
	char obj[AGENT_OBJ_MAX], pfx[AGENT_NAME_MAX], kind[8], head[256];
	unsigned max = AGENT_SYMS_DEF;
	size_t used;
	int i, n = 0;

	agent_arg(line, "obj", obj, (int)sizeof obj);
	agent_arg(line, "pfx", pfx, (int)sizeof pfx);
	agent_arg(line, "kind", kind, (int)sizeof kind);
	agent_arg_u32(line, "max", &max);
	if (max == 0) max = AGENT_SYMS_DEF;

	used = agent_head_open(out);
	for (i = 0; i < s_syms && (unsigned)n < max; i++) {
		if (obj[0] && !agent_obj_match(s_sym[i].obj, obj)) continue;
		if (pfx[0] && strncmp(s_sym[i].name, pfx, strlen(pfx))) continue;
		if (kind[0] && s_sym[i].kind != kind[0]) continue;
		if (used + 200 >= (size_t)cap) break;
		used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
		                          "name=%s#%s addr=0x%08X kind=%c\n",
		                          s_sym[i].name, s_sym[i].obj,
		                          (unsigned)(uintptr_t)s_sym[i].addr,
		                          s_sym[i].kind);
		n++;
	}
	_snprintf(head, sizeof head - 1, "OK lines=%d max=%u total=%d",
	          n, max, s_syms);
	agent_head_close(out, head, sizeof head);
}

/* OURS */
static void cmd_get_mem(const char *line, char *out, int cap)
{
	static unsigned char buf[AGENT_MEM_MAX];
	unsigned addr = 0, len = 4, i;
	size_t used;

	if (!agent_resolve(line, &addr, out, cap)) return;
	agent_arg_u32(line, "len", &len);
	if (len == 0 || len > AGENT_MEM_MAX) {
		_snprintf(out, (size_t)cap - 1, "ERR_RANGE len=%u max=%d\n",
		          len, AGENT_MEM_MAX);
		return;
	}
	if (!agent_safe_copy(buf, (const void *)(uintptr_t)addr, len)) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_RANGE addr=0x%08X unreadable\n", addr);
		return;
	}
	used = (size_t)_snprintf(out, (size_t)cap - 1,
	                         "OK addr=0x%08X len=%u bytes=", addr, len);
	for (i = 0; i < len; i++) {
		if (used + 4 >= (size_t)cap) {
			_snprintf(out, (size_t)cap - 1, "ERR_LIMIT len=%u\n", len);
			return;
		}
		used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
		                          "%02X", buf[i]);
	}
	_snprintf(out + used, (size_t)cap - used - 1, "\n");
}

/* OURS */
static void cmd_set_mem(const char *line, char *out, int cap)
{
	static unsigned char buf[AGENT_MEM_MAX];
	char av[AGENT_LINE_MAX];
	unsigned addr = 0;
	size_t n, i;

	if (!agent_resolve(line, &addr, out, cap)) return;
	if (!agent_arg(line, "data", av, (int)sizeof av)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=data\n");
		return;
	}
	n = strlen(av);
	if (n == 0 || (n & 1) || (n / 2) > AGENT_MEM_MAX) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=data len=%u\n",
		          (unsigned)n);
		return;
	}
	for (i = 0; i < n; i += 2) {
		int hi = agent_hex_nibble(av[i]);
		int lo = agent_hex_nibble(av[i + 1]);
		if (hi < 0 || lo < 0) {
			_snprintf(out, (size_t)cap - 1,
			          "ERR_BAD_ARG key=data at=%u\n", (unsigned)i);
			return;
		}
		buf[i / 2] = (unsigned char)((hi << 4) | lo);
	}
	if (!agent_safe_copy((void *)(uintptr_t)addr, buf, n / 2)) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_RANGE addr=0x%08X unwritable\n", addr);
		return;
	}
	_snprintf(out, (size_t)cap - 1, "OK addr=0x%08X written=%u\n",
	          addr, (unsigned)(n / 2));
}

/* OURS */
static void cmd_set_hook(const char *line, char *out, int cap)
{
	char av[AGENT_SPEC_MAX], force[16];
	Agent_Hook h;
	void *landed = NULL;
	unsigned v;
	int idx = 0, hits, slot = -1, i;

	if (!agent_arg(line, "name", av, (int)sizeof av)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=name\n");
		return;
	}
	if (s_mapStale && !agent_arg(line, "force", force, (int)sizeof force)) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_MAP_STALE map=%s - rebuild, or pass force=1\n",
		          s_mapPath);
		return;
	}
	hits = agent_sym_find(av, &idx);
	if (hits == 0) {
		_snprintf(out, (size_t)cap - 1, "ERR_NO_SYMBOL name=%s\n", av);
		return;
	}
	if (hits > 1) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_AMBIGUOUS name=%s n=%d - qualify it as name#obj\n",
		          av, hits);
		return;
	}
	if (s_sym[idx].kind != 'f') {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_NOT_CODE name=%s kind=d - a hook needs a"
		          " function\n", av);
		return;
	}

	memset(&h, 0, sizeof h);
	h.start = s_sym[idx].addr;
	h.bound = s_sym[idx].bound;
	_snprintf(h.name, AGENT_NAME_MAX - 1, "%s", s_sym[idx].name);
	_snprintf(h.obj, AGENT_OBJ_MAX - 1, "%s", s_sym[idx].obj);
	if (agent_arg(line, "tag", av, (int)sizeof av))
		_snprintf(h.tag, AGENT_TAG_MAX - 1, "%s", av);
	if (agent_arg_u32(line, "args", &v)) {
		if (v > AGENT_ARGS_MAX) {
			_snprintf(out, (size_t)cap - 1, "ERR_RANGE args=%u max=%d\n",
			          v, AGENT_ARGS_MAX);
			return;
		}
		h.nargs = (int)v;
	}
	if (agent_arg_u32(line, "max", &v)) h.maxHits = v;

	if (!agent_verify_penter(h.start, &landed)) {
		/* The address it DID reach is reported, because that is what
		 * tells a missing flag from a bad symbol. */
		_snprintf(out, (size_t)cap - 1,
		          "ERR_NO_PENTER name=%s#%s start=0x%08X reached=0x%08X"
		          " - that object was not built with /Gh\n",
		          h.name, h.obj, (unsigned)(uintptr_t)h.start,
		          (unsigned)(uintptr_t)landed);
		return;
	}

	if (agent_arg(line, "enter", av, (int)sizeof av)) {
		_snprintf(h.enterSpec, AGENT_SPEC_MAX - 1, "%s", av);
		if (!agent_cap_list(av, h.enter, &h.nEnter, &h.enterBytes,
		                    NULL, h.nargs, out, cap))
		    return;
	}
	if (agent_arg(line, "exit", av, (int)sizeof av)) {
		_snprintf(h.exitSpec, AGENT_SPEC_MAX - 1, "%s", av);
		if (!agent_cap_list(av, h.exitc, &h.nExit, &h.exitBytes,
		                    &h.wantDelta, h.nargs, out, cap))
		    return;
	}
	if (agent_arg(line, "agg", av, (int)sizeof av)
	    && !agent_agg_parse(av, &h, out, cap))
	    return;
	if (agent_arg(line, "when", av, (int)sizeof av)
	    && !agent_when_parse(av, &h, out, cap))
	    return;
	if (h.nEnter == 0 && h.nExit == 0 && !h.wantDelta && !h.aggAt) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_BAD_ARG nothing to capture - give enter=,"
		          " exit= or agg=\n");
		return;
	}
	if (h.whenAt == WHEN_EXIT && h.nEnter == 0 && h.nExit == 0
	    && !h.wantDelta && !h.aggAt) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_BAD_ARG when=%s decides at exit but nothing is"
		          " captured to keep\n", h.whenSpec);
		return;
	}
	if (h.wantDelta && h.nEnter == 0) {
		_snprintf(out, (size_t)cap - 1,
		          "ERR_BAD_ARG exit=delta needs an enter list to diff\n");
		return;
	}

	for (i = 0; i < AGENT_HOOK_MAX; i++) {
		if (s_hook[i].id && s_hook[i].start == h.start) {
			_snprintf(out, (size_t)cap - 1,
			          "ERR_EXISTS id=%d name=%s#%s\n",
			          s_hook[i].id, s_hook[i].name, s_hook[i].obj);
			return;
		}
		if (!s_hook[i].id && slot < 0) slot = i;
	}
	if (slot < 0) {
		_snprintf(out, (size_t)cap - 1, "ERR_RANGE hooks=%d max=%d\n",
		          AGENT_HOOK_MAX, AGENT_HOOK_MAX);
		return;
	}

	h.id = s_nextId++;
	s_hook[slot] = h;
	agent_rebuild();
	_snprintf(out, (size_t)cap - 1,
	          "OK id=%d name=%s#%s start=0x%08X bound=0x%08X args=%d"
	          " max=%u enter=%d ebytes=%u exit=%d xbytes=%u delta=%d"
	          " when=%s at=%s agg=%s aggat=%s verified=penter\n",
	          h.id, h.name, h.obj, (unsigned)(uintptr_t)h.start,
	          (unsigned)(uintptr_t)h.bound, h.nargs, h.maxHits,
	          h.nEnter, h.enterBytes, h.nExit, h.exitBytes, h.wantDelta,
	          h.whenSpec[0] ? h.whenSpec : "-",
	          h.whenAt == WHEN_EXIT ? "exit"
	              : (h.whenAt == WHEN_ENTER ? "enter" : "-"),
	          h.aggSpec[0] ? h.aggSpec : "-",
	          h.aggAt == WHEN_EXIT ? "exit"
	              : (h.aggAt == WHEN_ENTER ? "enter" : "-"));
}

/* OURS */
static void cmd_clear_hook(const char *line, char *out, int cap)
{
	char av[32];
	unsigned id = 0;
	int i, gone = 0;

	if (!agent_arg(line, "id", av, (int)sizeof av)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG key=id\n");
		return;
	}
	if (strcmp(av, "all") && !agent_u32(av, &id)) {
		_snprintf(out, (size_t)cap - 1, "ERR_BAD_ARG id=%s\n", av);
		return;
	}
	for (i = 0; i < AGENT_HOOK_MAX; i++) {
		if (!s_hook[i].id) continue;
		if (id && s_hook[i].id != (int)id) continue;
		memset(&s_hook[i], 0, sizeof s_hook[i]);
		gone++;
	}
	agent_rebuild();
	/* The pairing stack names slots that may have just been freed. */
	s_depth = 0;
	s_snapUsed = 0;
	_snprintf(out, (size_t)cap - 1, "OK removed=%d armed=%ld exits=%ld\n",
	          gone, s_armedCount, s_exitCount);
}

/* OURS */
static void cmd_list_hook(char *out, int cap)
{
	char head[256];
	size_t used;
	int i, n = 0;

	used = agent_head_open(out);
	for (i = 0; i < AGENT_HOOK_MAX; i++) {
		Agent_Hook *h = &s_hook[i];
		if (!h->id || used + 300 >= (size_t)cap) continue;
		used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
		                          "id=%d name=%s#%s start=0x%08X"
		                          " bound=0x%08X args=%d hits=%u"
		                          " recs=%u max=%u when=%s agg=%s"
		                          " n=%u min=%ld max2=%ld sum=%I64d"
		                          " enter=%s exit=%s delta=%d tag=%s\n",
		                          h->id, h->name, h->obj,
		                          (unsigned)(uintptr_t)h->start,
		                          (unsigned)(uintptr_t)h->bound,
		                          h->nargs, h->hits, h->recs, h->maxHits,
		                          h->whenSpec[0] ? h->whenSpec : "-",
		                          h->aggSpec[0] ? h->aggSpec : "-",
		                          h->aggN, h->aggLo, h->aggHi, h->aggSum,
		                          h->enterSpec[0] ? h->enterSpec : "-",
		                          h->exitSpec[0] ? h->exitSpec : "-",
		                          h->wantDelta,
		                          h->tag[0] ? h->tag : "-");
		n++;
	}
	_snprintf(head, sizeof head - 1,
	          "OK lines=%d armed=%ld exits=%ld penter=%u pexit=%u"
	          " pending=%u dropped_full=%u unpaired=%u",
	          n, s_armedCount, s_exitCount, s_penterCalls,
	          s_pexitCalls, s_pending, s_dropFull, s_unpaired);
	agent_head_close(out, head, sizeof head);
}

/*
 * OURS. Where the caller address sits in a record's payload, or -1.
 *
 * THE RECORD CARRIES BYTES AND AN ID, NOT A LAYOUT. The hook owns the
 * capture list, so the offset is recomputed from it at drain time -
 * which also means a trace outliving the hook that made it loses the
 * annotation rather than inventing one.
 */
static long agent_caller_off(int id, int kind)
{
	const Agent_Cap *c;
	long off = 0;
	int slot, i, nc;

	for (slot = 0; slot < AGENT_HOOK_MAX; slot++)
		if (s_hook[slot].id == id) break;
	if (slot == AGENT_HOOK_MAX) return -1;
	if (kind == REC_ENTER) {
		c = s_hook[slot].enter;
		nc = s_hook[slot].nEnter;
	} else if (kind == REC_EXIT) {
		c = s_hook[slot].exitc;
		nc = s_hook[slot].nExit;
	} else {
		return -1;                  /* a delta is ranges, not the list */
	}
	for (i = 0; i < nc; i++) {
		if (c[i].kind == CAP_CALL) return off;
		off += (long)c[i].len;
	}
	return -1;
}

/*
 * OURS. Throw the ring away without reading it.
 *
 * A HOOK ARMED ON A PER-PIXEL FUNCTION FILLS IT IN ONE FRAME, and
 * without this the only way back to a clean trace is pulling 37,000
 * records through the pipe to look at none of them. The drop counter
 * goes with them because it describes that ring; unpaired does not,
 * because it describes the pairing and outlives any one trace.
 */
static void cmd_clear_trace(char *out, int cap)
{
	unsigned had = s_pending;

	s_ringHead = 0;
	s_ringTail = 0;
	s_pending = 0;
	s_dropFull = 0;
	_snprintf(out, (size_t)cap - 1, "OK discarded=%u dropped_full=0\n", had);
}

/* OURS. Drain the ring. The payload points into it, so every record is
 * formatted before the compaction at the end. */
static void cmd_get_trace(const char *line, char *out, int cap)
{
	static const char *KIND[3] = { "enter", "exit", "delta" };
	unsigned max = AGENT_TRACE_DEF, bytes = 0;
	char hdr[256];
	size_t used;
	Agent_Rec r;
	const unsigned char *p;
	long co;
	int n = 0;

	agent_arg_u32(line, "max", &max);
	if (max == 0) max = AGENT_TRACE_DEF;

	used = agent_head_open(out);
	while ((unsigned)n < max) {
		unsigned i;
		if (!agent_ring_get(&r, &p)) break;
		if (used + 256 + r.len * 2 >= (size_t)cap) {
			/* Put it back: the header has not been consumed by the
			 * client, so rewinding is the honest thing to do. */
			s_ringTail -= sizeof r + r.len;
			s_pending++;
			break;
		}
		used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
		                          "seq=%u tick=%u id=%d kind=%s len=%u"
		                          " data=", r.seq, r.tick, r.id,
		                          r.kind >= 0 && r.kind < 3
		                              ? KIND[r.kind] : "?", r.len);
		for (i = 0; i < r.len; i++)
			used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
			                          "%02X", p[i]);
		co = agent_caller_off(r.id, r.kind);
		if (co >= 0 && (unsigned)co + 4 <= r.len) {
			void *pc = NULL;
			char named[160];
			memcpy(&pc, p + co, 4);
			agent_sym_at(pc, named, (int)sizeof named);
			used += (size_t)_snprintf(out + used, (size_t)cap - used - 1,
			                          " caller=%s", named);
		}
		out[used++] = '\n';
		out[used] = 0;
		bytes += r.len;
		n++;
	}
	agent_ring_compact();
	_snprintf(hdr, sizeof hdr - 1,
	          "OK lines=%d bytes=%u pending=%u dropped_full=%u"
	          " unpaired=%u", n, bytes, s_pending, s_dropFull, s_unpaired);
	agent_head_close(out, hdr, sizeof hdr);
}

/* OURS. One verb per line, executed on the host thread. Longer verbs
 * are tested before their prefixes - SYMS before SYM, DUMP_RESET
 * before DUMP. */
static void agent_dispatch(const char *line, char *out, int cap)
{
	out[0] = 0;
	if      (!strncmp(line, "HELLO", 5))
		_snprintf(out, (size_t)cap - 1, "OK relic_agent proto=1\n");
	else if (!strncmp(line, "STATUS", 6))      cmd_status(out, cap);
	else if (!strncmp(line, "CAPS", 4))        cmd_caps(out, cap);
	else if (!strncmp(line, "PARTS", 5))       cmd_parts(out, cap);
	else if (!strncmp(line, "GOTO", 4))        cmd_goto(line, out, cap, 0);
	else if (!strncmp(line, "NEXT", 4))        cmd_goto(line, out, cap, 1);
	else if (!strncmp(line, "PREV", 4))        cmd_goto(line, out, cap, -1);
	else if (!strncmp(line, "STEP", 4))        cmd_step(line, out, cap);
	else if (!strncmp(line, "RUN_TO", 6))      cmd_run_to(line, out, cap);
	else if (!strncmp(line, "PAUSE", 5))       cmd_pause(out, cap);
	else if (!strncmp(line, "RESUME", 6))      cmd_resume(out, cap);
	else if (!strncmp(line, "SET_RES", 7))     cmd_set_res(line, out, cap);
	else if (!strncmp(line, "SET_MEM", 7))     cmd_set_mem(line, out, cap);
	else if (!strncmp(line, "GET_MEM", 7))     cmd_get_mem(line, out, cap);
	else if (!strncmp(line, "DUMP_RESET", 10)) cmd_dump_reset(out, cap);
	else if (!strncmp(line, "DUMP_MEM", 8))    cmd_dump_mem(line, out,
	                                                        cap);
	else if (!strncmp(line, "DUMP", 4))        cmd_dump(line, out, cap);
	else if (!strncmp(line, "SYMS", 4))        cmd_syms(line, out, cap);
	else if (!strncmp(line, "SYM", 3))         cmd_sym(line, out, cap);
	else if (!strncmp(line, "SET_HOOK", 8))    cmd_set_hook(line, out, cap);
	else if (!strncmp(line, "CLEAR_HOOK", 10)) cmd_clear_hook(line, out, cap);
	else if (!strncmp(line, "CLEAR_TRACE", 11)) cmd_clear_trace(out, cap);
	else if (!strncmp(line, "LIST_HOOK", 9))   cmd_list_hook(out, cap);
	else if (!strncmp(line, "GET_TRACE", 9))   cmd_get_trace(line, out, cap);
	else if (!strncmp(line, "QUIT", 4)) {
		s_quit = 1;
		_snprintf(out, (size_t)cap - 1, "OK bye\n");
	}
	else
		_snprintf(out, (size_t)cap - 1, "ERR_NO_VERB line=%.64s\n", line);
}


/* ---- the crash report ---------------------------------------------- */

/*
 * OURS. Where it died, in the demo own names.
 *
 * TO A FILE AND NOT THE PIPE. An unsolicited line would arrive in the
 * middle of a reply the client is parsing, so the fault is written
 * beside the exe and the client reads it when the pipe drops. STATUS
 * reports the path.
 */
static LONG WINAPI agent_crash(PEXCEPTION_POINTERS ep)
{
	char named[256];
	void *pc = (void *)(uintptr_t)ep->ContextRecord->Eip;
	FILE *f;

	agent_sym_at(pc, named, (int)sizeof named);
	fprintf(stderr, "agent: crash code=0x%08lX at=0x%08lX %s\n",
	        (unsigned long)ep->ExceptionRecord->ExceptionCode,
	        (unsigned long)(uintptr_t)pc, named);
	f = fopen(s_crashPath, "w");
	if (f) {
		fprintf(f, "code=0x%08lX pc=0x%08lX at=%s part=%d tick=%ld\n",
		        (unsigned long)ep->ExceptionRecord->ExceptionCode,
		        (unsigned long)(uintptr_t)pc, named,
		        Demo_PartIndex(), (long)Demo_TickCount());
		fclose(f);
	}
	return s_prevFilter ? s_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}


/* ---- the instrumentation hooks ------------------------------------- */

/*
 * OURS. What /Gh and /GH call, and for now all they do is count.
 *
 * THE COUNT IS THE POINT. /Gh is per file, so a hook armed on a
 * function in a file built without it would never fire and would look
 * exactly like a function that is never called - STATUS reporting a
 * non-zero penter is the proof the flag reached the object.
 *
 * PUSHFD IS NOT OPTIONAL. pushad saves the eight GPRs and not the
 * flags; at a function entry the flags are dead, so omitting it
 * appears to work and then corrupts one comparison in a thousand once
 * a body does arithmetic. x87 and MXCSR are untouched because nothing
 * here touches them, and anything added below must stay integer-only.
 *
 * The call precedes the prologue, so the hooked function starts at
 * [esp]-5 and its arguments are two slots down, not one. [esp+36]
 * recovers the esp the stub was entered with, pushad being 32 bytes
 * and pushfd 4, and that is what the hit handlers are handed.
 */
void __declspec(naked) _penter(void)
{
	__asm {
		pushad
		pushfd
		inc  dword ptr [s_penterCalls]
		mov  eax, dword ptr [s_armedCount]
		test eax, eax
		jz   pe_out
		lea  eax, [esp + 36]
		push eax
		call agent_penter_hit
		add  esp, 4
	pe_out:
		popfd
		popad
		ret
	}
}

/* OURS. /GH emits this after the epilogue, so its return address is
 * INSIDE the function body - identifying the function here is a range
 * test against [addr, bound), never a compare. */
void __declspec(naked) _pexit(void)
{
	__asm {
		pushad
		pushfd
		inc  dword ptr [s_pexitCalls]
		mov  eax, dword ptr [s_exitCount]
		test eax, eax
		jz   px_out
		lea  eax, [esp + 36]
		push eax
		call agent_pexit_hit
		add  esp, 4
	px_out:
		popfd
		popad
		ret
	}
}


/* ---- the listener -------------------------------------------------- */

/* OURS. Park the line for the pump and wait for its reply. The pump
 * is the only thread that touches demo state. */
static void agent_serve(const char *line)
{
	DWORD written = 0;

	_snprintf(s_req, AGENT_LINE_MAX - 1, "%s", line);
	s_rep[0] = 0;
	SetEvent(s_reqEvent);
	WaitForSingleObject(s_repEvent, INFINITE);
	if (s_rep[0] == 0)
		_snprintf(s_rep, AGENT_REPLY_MAX - 1, "ERR_INTERNAL\n");
	WriteFile(s_pipe, s_rep, (DWORD)strlen(s_rep), &written, NULL);
}

/* OURS. A BLOCKING PIPE, which is what a single-threaded target can
 * afford: this thread may sleep in ReadFile for ever because nothing
 * else needs it, so the whole poll-and-sleep dance a non-blocking pipe
 * forces is absent. Agent_Stop wakes it with CancelSynchronousIo. */
static DWORD WINAPI agent_listen(void *param)
{
	char in[AGENT_LINE_MAX];
	DWORD len = 0;
	int connected = 0;

	(void)param;
	while (InterlockedCompareExchange(&s_up, 1, 1) != 0) {
		DWORD got = 0;

		if (!connected) {
			if (!ConnectNamedPipe(s_pipe, NULL)
			    && GetLastError() != ERROR_PIPE_CONNECTED) {
				DisconnectNamedPipe(s_pipe);
				continue;
			}
			connected = 1;
			len = 0;
		}
		if (!ReadFile(s_pipe, in + len, (DWORD)(sizeof in - len - 1),
		              &got, NULL) || got == 0) {
			DisconnectNamedPipe(s_pipe);
			connected = 0;
			len = 0;
			continue;
		}
		len += got;
		in[len] = 0;
		for (;;) {
			char *nl = strchr(in, '\n');
			size_t used, k;
			if (!nl) break;
			*nl = 0;
			used = (size_t)(nl - in) + 1;
			k = strlen(in);
			while (k > 0 && in[k - 1] == '\r') in[--k] = 0;
			if (in[0]) agent_serve(in);
			memmove(in, in + used, len - used);
			len -= (DWORD)used;
			in[len] = 0;
		}
		if (len + 1 >= sizeof in) { len = 0; in[0] = 0; }
	}
	if (connected) DisconnectNamedPipe(s_pipe);
	return 0;
}


/* ---- the host boundary --------------------------------------------- */

/* OURS */
int Agent_Start(const char *dump_dir)
{
	if (InterlockedCompareExchange(&s_up, 1, 0) != 0) return 0;

	_snprintf(s_dumpDir, AGENT_PATH_MAX - 1, "%s",
	          dump_dir ? dump_dir : ".");
	if (agent_sibling(".crash.txt", s_crashPath, AGENT_PATH_MAX))
		_snprintf(s_crashPath, AGENT_PATH_MAX - 1, "relic.crash.txt");
	/* A REPORT FROM A PREVIOUS RUN WOULD BE READ AS THIS ONE'S. The
	 * client reaches for this file whenever the pipe drops, so a
	 * stale one explains an ordinary QUIT as a fault that happened
	 * days ago. It exists only if THIS run wrote it. */
	DeleteFileA(s_crashPath);
	if (agent_map_load())
		printf("agent: no usable map at %s - symbol verbs will refuse\n",
		       s_mapPath);
	else
		printf("agent: %d symbols from %s%s\n", s_syms, s_mapPath,
		       s_mapStale ? "  (STALE - older than the exe)" : "");

	s_ring = (unsigned char *)VirtualAlloc(NULL, AGENT_RING_SIZE,
	                                       MEM_COMMIT | MEM_RESERVE,
	                                       PAGE_READWRITE);
	if (!s_ring)
		printf("agent: ring alloc failed, SET_HOOK will capture"
		       " nothing\n");

	s_prevFilter = SetUnhandledExceptionFilter(agent_crash);
	s_reqEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
	s_repEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
	s_pipe = CreateNamedPipeA(AGENT_PIPE_NAME, PIPE_ACCESS_DUPLEX,
	                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
	                          | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
	                          1, 64 * 1024, 64 * 1024, 0, NULL);
	if (!s_reqEvent || !s_repEvent || s_pipe == INVALID_HANDLE_VALUE) {
		printf("agent: cannot open %s (%lu)\n", AGENT_PIPE_NAME,
		       (unsigned long)GetLastError());
		InterlockedExchange(&s_up, 0);
		return 1;
	}

	Demo_SetAutoAdvance(0);
	if (Demo_SetResolution(s_resW, s_resH))
		printf("agent: part %d init FAILED - a dat chunk is probably"
		       " missing, pass -dat\n", Demo_PartIndex());
	s_thread = CreateThread(NULL, 0, agent_listen, NULL, 0, NULL);
	if (!s_thread) {
		InterlockedExchange(&s_up, 0);
		return 1;
	}
	printf("agent: listening on %s  (paused at part %d)\n",
	       AGENT_PIPE_NAME, Demo_PartIndex());
	return 0;
}

/* OURS */
void Agent_Stop(void)
{
	if (InterlockedCompareExchange(&s_up, 0, 1) != 1) return;
	SetEvent(s_repEvent);               /* release a waiting listener */
	if (s_thread) {
		CancelSynchronousIo(s_thread);
		WaitForSingleObject(s_thread, 1000);
		CloseHandle(s_thread);
		s_thread = NULL;
	}
	if (s_pipe && s_pipe != INVALID_HANDLE_VALUE) CloseHandle(s_pipe);
	s_pipe = NULL;
	if (s_reqEvent) { CloseHandle(s_reqEvent); s_reqEvent = NULL; }
	if (s_repEvent) { CloseHandle(s_repEvent); s_repEvent = NULL; }
	if (s_sym) { free(s_sym); s_sym = NULL; s_syms = 0; }
	if (s_ring) { VirtualFree(s_ring, 0, MEM_RELEASE); s_ring = NULL; }
	SetUnhandledExceptionFilter(s_prevFilter);
}

/*
 * OURS. Drain whatever is parked, then decide whether to give the host
 * its thread back.
 *
 * Paused and allowed to block, the wait is INFINITE - so an idle
 * session costs nothing at all and a command is served the moment it
 * lands. A host with a window must NOT ask for that: a blocked
 * message loop is a window Windows paints as not responding.
 */
int Agent_Pump(int flags)
{
	if (InterlockedCompareExchange(&s_up, 1, 1) == 0) return 0;
	for (;;) {
		DWORD ms = ((flags & AGENT_PUMP_BLOCK) && !s_free) ? INFINITE : 0;

		if (WaitForSingleObject(s_reqEvent, ms) == WAIT_OBJECT_0) {
			agent_dispatch(s_req, s_rep, AGENT_REPLY_MAX);
			SetEvent(s_repEvent);
			if (s_quit) return 1;
			continue;
		}
		if (s_free) {
			if (flags & AGENT_PUMP_CLOCK) agent_one_frame();
			return 0;
		}
		if (!(flags & AGENT_PUMP_BLOCK)) return 0;
	}
}

#endif /* _M_IX86 */
