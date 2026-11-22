/* RELIC - the music clock, with ibxm behind it.
 *
 * THE DEMO'S TIMELINE RUNS ON THE REPLAYER'S POSITION, NOT ON A
 * FRAME COUNT. Music_ArmCue 0x0021ff3c stores a song position and a
 * row and Music_PollCue 0x0021ff64 waits on the replayer passing
 * both; five scene boundaries in the demo end that way. ibxm
 * exports replay_get_sequence_pos and replay_get_row, which are
 * exactly the two words 0x0021ffa2 and 0x0021ffa6 the demo's own
 * replayer compares against - so the cue is a direct translation
 * rather than a stand-in.
 *
 * NOTHING PLAYS YET. The demo's two tunes are P61 (Player 6.1)
 * packed modules and ibxm reads ProTracker, S3M and XM; until a
 * module in one of those reaches Music_Load, this keeps the silent
 * tempo clock and the timeline runs exactly as it did.
 * docs/reconstruction.txt 4.
 */

#include <stdlib.h>
#include <string.h>

#include "dat.h"
#include "image.h"
#include "host.h"
#include "third_party/ibxm/ibxm.h"

/* OURS - src/third_party/prowizard/p61a.c, libxmp's depacker made
 * self-contained and memory to memory. reconstruction.txt 4.1. */
int P61A_Test(const unsigned char *data, long size);
int P61A_Depack(const unsigned char *data, long size,
                unsigned char **out_data, long *out_size);

/* OURS - the silent clock, for when no module is loaded.
 * Protracker defaults: six ticks a row at 125 BPM is 50 ticks a
 * second, which is also PAL's frame rate - which is why a demo of
 * this era can treat the two as the same clock. */
#define TICKS_PER_SECOND 50
#define TICKS_PER_ROW     6
#define ROWS_PER_ORDER   64
#define MS_PER_TICK      (1000 / TICKS_PER_SECOND)

static u32 g_tick;
static s32 g_frac_ms;
static u32 g_armed_pos;
static u32 g_armed_row;
static s32 g_armed_fallback;
static int g_have_armed;
static int g_fired;
static int g_timeline = 1;
static u32 g_pulls;            /* Demo_Audio calls, ever         */
static u32 g_armed_pulls;      /* what it was when we armed      */

static s32 g_want_rate = 44100;     /* survives an unload           */
static int g_loaded = -1;           /* which chunk, or -1           */
static int g_pinned;                /* -mod: never swap it out      */

/* OURS - THE TUNES ARE PARSED ONCE AND KEPT. An act switch used to
 * depack 822 KB inside a frame: the main loop stopped topping the
 * device up, the queue ran dry and the handover arrived with a thud.
 * g_tune owns the three; g_module only points at the live one.
 * docs/reconstruction.txt 4.4. */
#define MUSIC_TUNES 3
static struct module *g_tune[MUSIC_TUNES];
static struct module *g_own;        /* -mod's, which we do own */

static struct module *g_module;
static struct replay *g_replay;
static int  *g_mixbuf;
static s32   g_mixlen;
static s32   g_rate;

/* OURS - the host's mixing rate, kept across an unload so a module
 * swapped in mid-run does not need telling again. */
void Music_SetRate(s32 rate)
{
	if (rate > 0) g_want_rate = rate;
}

/* OURS - -mod holds whatever it loaded against the act switch. */
void Music_Pin(void)
{
	g_pinned = 1;
}

/* OURS - what the host asks for when it wants to know whether there
 * is anything to hear. */
int Music_Playing(void)
{
	return g_replay != NULL;
}

/* OURS - the live replay dropped, the parsed tunes kept. */
static void stop_active(void)
{
	if (g_replay) { dispose_replay(g_replay); g_replay = NULL; }
	g_module = NULL;
}

/* OURS - a module ibxm reads, or a P61 one depacked on the way in.
 * THE DEMO'S OWN TUNES ARE P61, so that path is the normal one. The
 * caller keeps ownership of the bytes. */
static struct module *module_from(const u8 *bytes, s32 len)
{
	struct data d;
	char msg[64];
	unsigned char *mod = 0;
	long modlen = 0;
	struct module *m;

	if (!bytes || len <= 0) return NULL;
	if (P61A_Test(bytes, (long)len) == 0
	    && P61A_Depack(bytes, (long)len, &mod, &modlen) == 0) {
		bytes = (const u8 *)mod;
		len = (s32)modlen;
	}
	msg[0] = 0;
	d.buffer = (char *)bytes;
	d.length = (int)len;
	m = module_load(&d, msg);
	free(mod);
	return m;
}

/* OURS - the mix buffer, which outlives any one tune. */
static int want_mixbuf(void)
{
	if (g_mixbuf) return 1;
	g_mixlen = (s32)calculate_mix_buf_len((int)g_rate);
	g_mixbuf = (int *)malloc((size_t)g_mixlen * sizeof(int));
	return g_mixbuf != NULL;
}

/* OURS - -mod's module, which replaces whatever the cache holds. */
int Music_Load(const u8 *bytes, s32 len, s32 rate)
{
	struct module *m;

	if (rate > 0) { g_want_rate = rate; g_rate = rate; }
	m = module_from(bytes, len);
	if (!m) return 1;
	stop_active();
	if (g_own) dispose_module(g_own);
	g_own = m;
	if (!want_mixbuf()) return 1;
	g_module = g_own;
	g_replay = new_replay(g_module, (int)g_rate, 0);
	return g_replay ? 0 : 1;
}

/* OURS - the demo's own tunes, by the names relic.dat gives them.
 * THE C PREFIX IS THE POINT: a module's samples have to reach chip
 * RAM for Paula, so the archive marks them, and these are the only
 * two chunks that carry it. formats/relic_dat.txt. */
static const char *const MUSIC_CHUNK[] = { "Cmodule1", "Cmodule2" };
#define MUSIC_CHUNKS ((int)(sizeof MUSIC_CHUNK / sizeof MUSIC_CHUNK[0]))

/* ORIGIN 0x002c8084 g_IntroModule - A THIRD P61 MODULE, AND IT IS IN
 * THE EXECUTABLE, not the archive: one pattern, one 328-byte sample,
 * 420 bytes all told. Demo_Run starts it at 0x0021f796 before
 * anything is loaded and its rows are the terminal's typewriter.
 * The length is the header's own - sample-data offset 92 plus 164
 * words of sample. docs/reconstruction.txt 4.2. */
#define MUSIC_INTRO_ADDR 0x002c8084UL
#define MUSIC_INTRO_LEN  420L

/* OURS - where each tune's bytes live. Dat_Open has to have run for
 * the two chunks; the intro's are in the executable. */
static const u8 *tune_bytes(int index, u32 *len)
{
	*len = 0;
	if (index == MUSIC_INTRO) {
		*len = (u32)MUSIC_INTRO_LEN;
		return Image_At(MUSIC_INTRO_ADDR);
	}
	if (index < 0 || index >= MUSIC_CHUNKS) return NULL;
	return Dat_Chunk(MUSIC_CHUNK[index], len);
}

/* OURS - host.h. A HOST WITH NO AUDIO NEVER CALLS THIS, which is why
 * it is not in Demo_Init: relic_ppm would pay the depack on every
 * render for a tune it never plays. */
int Music_Preload(s32 rate)
{
	int i, n = 0;

	if (rate > 0) g_want_rate = rate;
	g_rate = g_want_rate;
	if (!want_mixbuf()) return 0;
	for (i = 0; i < MUSIC_TUNES; i++) {
		const u8 *bytes;
		u32 len;

		if (!g_tune[i]) {
			bytes = tune_bytes(i, &len);
			g_tune[i] = module_from(bytes, (s32)len);
		}
		if (g_tune[i]) n++;
	}
	return n;
}

/* OURS */
void Music_Unload(void)
{
	int i;

	stop_active();
	if (g_own) { dispose_module(g_own); g_own = NULL; }
	for (i = 0; i < MUSIC_TUNES; i++)
		if (g_tune[i]) { dispose_module(g_tune[i]); g_tune[i] = NULL; }
	free(g_mixbuf);
	g_mixbuf = NULL;
	g_mixlen = 0;
	g_rate = 0;
	g_loaded = -1;
}

/* ORIGIN 0x0021fed4 - the demo starts the replayer on the module the
 * act loaded, so switching act switches tune. A NEGATIVE INDEX IS
 * SILENCE, which is what the terminal runs under: SpaceModule_Part0
 * spins on 0x0023721a at 0x002c834c until Terminal_Tick raises it
 * and only then starts the replayer, so the typing has no music
 * behind it. docs/reconstruction.txt 4.2. */
void Music_Start(int module_index)
{
	g_tick = 0;
	g_frac_ms = 0;
	g_armed_pos = 0;
	g_armed_row = 0;
	g_have_armed = 0;
	g_fired = 0;
	if (g_pinned) {
		if (g_replay) replay_set_sequence_pos(g_replay, 0);
		return;
	}
	if (module_index == g_loaded && g_replay) {
		replay_set_sequence_pos(g_replay, 0);
		return;
	}
	stop_active();
	g_loaded = -1;
	if (module_index < 0 || module_index >= MUSIC_TUNES) return;
	if (!g_tune[module_index] || !g_rate) return;
	g_module = g_tune[module_index];
	g_replay = new_replay(g_module, (int)g_rate, 0);
	if (!g_replay) { g_module = NULL; return; }
	replay_set_sequence_pos(g_replay, 0);
	g_loaded = module_index;
}

/* OURS */
void Music_Stop(void)
{
	g_have_armed = 0;
}

/* ORIGIN 0x0021ff3c Music_ArmCue - the pair, and the flag cleared. */
void Music_Arm(u32 order, u32 row, s32 fallback_ticks)
{
	g_armed_pos = order;
	g_armed_row = row;
	g_armed_fallback = fallback_ticks;
	g_armed_pulls = g_pulls;
	g_have_armed = 1;
	g_fired = 0;
}

/* OURS - see host.h. */
void Music_BreakTimeline(void)
{
	g_timeline = 0;
}

/* ORIGIN 0x0021ff64 Music_PollCue - the flag the replayer raises
 * once its position and row have BOTH passed what was armed. Two
 * floors, not two equalities, so a cue armed behind the playhead
 * answers at once. docs/reconstruction.txt 4.
 *
 * LANDMINE: IBXM'S POSITION ADVANCES ONLY WHEN AUDIO IS PULLED,
 * from whichever thread the host mixes on. A host that loads a
 * module and never opens a device leaves the position at 0 for
 * ever, and a cue gate would hang the part rather than fail - so
 * the cue is only believed while Demo_Audio is actually draining
 * the replayer. */
int Music_Reached(s32 elapsed)
{
	if (!g_have_armed) return 1;
	if (!g_timeline || !g_replay || g_pulls == g_armed_pulls)
		return elapsed >= g_armed_fallback;
	if (!g_fired
	    && (u32)replay_get_sequence_pos(g_replay) >= g_armed_pos
	    && (u32)replay_get_row(g_replay) >= g_armed_row)
	    g_fired = 1;
	return g_fired;
}

/* OURS - ONE TICK, AND HOW LONG IT WAS. ibxm's tick is a TEMPO tick
 * and an Fxx above 0x1f changes its length, so a count of ticks fixes
 * no duration at all - only the samples one covers does. -> that
 * sample count. docs/reconstruction.txt 4.6. */
s32 Music_DrainTick(void)
{
	if (!g_replay || !g_mixbuf) return 0;
	return replay_get_audio(g_replay, g_mixbuf, 0);
}

/* OURS - ADVANCE THE REPLAYER WITHOUT LISTENING, which is the one
 * primitive a seek and a fast forward both need. ibxm hands back one
 * tick of audio a call, so draining is counting calls and dropping
 * what comes back. -> the ticks actually drained.
 * docs/reconstruction.txt 4.6. */
s32 Music_DrainTicks(s32 ticks)
{
	s32 n = 0;

	while (n < ticks && Music_DrainTick() > 0) n++;
	return n;
}

/* OURS - drain until the replayer has passed the pair, on the same
 * two floors Music_Reached tests. -> 0 when it lands. */
int Music_DrainTo(u32 order, u32 row)
{
	s32 guard = 0x40000L;

	if (!g_replay || !g_mixbuf) return 1;
	while (guard-- > 0) {
		if ((u32)replay_get_sequence_pos(g_replay) >= order
		    && (u32)replay_get_row(g_replay) >= row)
		    return 0;
		if (replay_get_audio(g_replay, g_mixbuf, 0) <= 0) return 1;
	}
	return 1;
}

/* OURS - THE SEEK DRAINS FROM THE TUNE'S START AND NEVER JUMPS.
 * LANDMINE: replay_set_sequence_pos resets the tempo to the module's
 * default, which throws away the F commands RELIC sets in its first
 * two orders - so a jump lands in the right place and plays every
 * row after it at the wrong rate. Draining costs only mixing, and a
 * whole module is a fraction of a second once it is parsed.
 * LANDMINE: LANDING MENDS THE TIMELINE - reconstruction.txt 4.6. */
int Music_SeekTo(int tune, u32 order, u32 row)
{
	if (tune < 0) return 1;
	Music_Start(tune);
	if (Music_DrainTo(order, row)) return 1;
	g_timeline = 1;
	return 0;
}

/* OURS - where the replayer stands, for a host to report. */
void Music_Position(u32 *order, u32 *row)
{
	if (order) *order = g_replay
			   ? (u32)replay_get_sequence_pos(g_replay) : 0;
	if (row) *row = g_replay ? (u32)replay_get_row(g_replay) : 0;
}

/* OURS - host.h. Whether an armed cue can still answer, which is
 * exactly the condition Music_Reached uses to believe one. */
int Music_CueLive(void)
{
	return g_timeline && g_replay != NULL && g_pulls != g_armed_pulls;
}

/* OURS */
u32 Music_Tick(void)
{
	return g_tick;
}

/* OURS - the silent clock's only mover; ibxm keeps its own. */
void Music_Advance(s32 ms)
{
	if (ms < 0) return;
	g_frac_ms += ms;
	g_tick += (u32)(g_frac_ms / MS_PER_TICK);
	g_frac_ms %= MS_PER_TICK;
}

/* OURS - the host's second entry point. THE MIXER IS NOT DRIVEN BY
 * THE FRAME CALL: SDL pulls audio on its own thread and emscripten
 * through Web Audio, so this is called from wherever that is and
 * must not touch the demo's state. Silence when nothing is loaded,
 * so a host can call it unconditionally.
 *
 * ibxm mixes into ints at its own block length; this drains that
 * into the caller's 16-bit pairs and keeps the remainder. */
void Demo_Audio(s16 *out, s32 pairs)
{
	static s32 g_have;
	static s32 g_at;
	s32 n;

	if (!out || pairs <= 0) return;
	g_pulls++;
	if (!g_replay || !g_mixbuf) {
		memset(out, 0, (size_t)pairs * 2 * sizeof(s16));
		return;
	}
	while (pairs > 0) {
		if (g_at >= g_have) {
			g_have = (s32)replay_get_audio(g_replay, g_mixbuf, 0);
			g_at = 0;
			if (g_have <= 0) {
				memset(out, 0, (size_t)pairs * 2 * sizeof(s16));
				return;
			}
		}
		n = g_have - g_at;
		if (n > pairs) n = pairs;
		for (; n > 0; n--, g_at++, pairs--) {
			s32 l = g_mixbuf[g_at * 2];
			s32 r = g_mixbuf[g_at * 2 + 1];

			if (l < -32768L) l = -32768L;
			if (l > 32767L) l = 32767L;
			if (r < -32768L) r = -32768L;
			if (r > 32767L) r = 32767L;
			*out++ = (s16)l;
			*out++ = (s16)r;
		}
	}
}
