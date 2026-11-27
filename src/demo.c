/* RELIC - the demo as one frame-at-a-time object. See host.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "image.h"
#include "gfx.h"
#include "parts.h"
#include "dat.h"

static Relic_Frame g_frame;
static u8  *g_rgb;
static s32  g_rgb_bytes;
static int  g_part;
static int  g_mark;
static int  g_auto = 1;
static s32  g_tick;
static s32  g_tick_fx;          /* 16.16 ticks, the real clock   */
static int  g_music = -2;       /* the act's tune, -1 for none   */
static int  g_synced;          /* did the last jump seek music? */

static void music_follow(void);
static void goto_part(int index);
static s32  g_tick_rem;         /* the 0.8 of 3276.8 carried     */
static s32  g_modes;
static int  g_drag_down;        /* the host's pointer gesture    */
static s32  g_drag_x, g_drag_y; /* 16.16 of a half window        */
static int  g_look_hold;        /* OURS: a release keeps the look */
static s32  g_held_x, g_held_y; /* the kept look, same units     */
static s32  g_wheel;            /* the host's running notch      */
static int  g_home;             /* this frame's request for rest */
static s32  g_cycle;            /* the host's running "next"   */
static u32  g_hold;             /* keys held, DEMO_HOLD_*        */

#define PAL_FRAME_MS  20        /* 50 Hz: one PAL vertical blank */
/* Ticks one Demo_Advance may contribute, which is also the longest
 * stall it will absorb rather than discard. A SLOW HOST AND A STALLED
 * ONE LOOK THE SAME FROM HERE, so this is where the two are traded:
 * at 4 the clock started losing time below 12.5 presented frames a
 * second, which is a resolution the port really does reach. */
#define CATCHUP_MAX    8

/* OURS - ticks at a skip's tail that are DRAWN, not stepped past.
 * Eight is where the temporal filters converge - reconstruction 3.1. */
#define SKIP_DRAW      8

/* OURS - THE SWING HOME OF A RELEASED DRAG, AS A TIME CONSTANT and
 * not a fraction a frame, so it takes the same time whatever the
 * host's frame rate. It lives here and not in a host because how the
 * gesture feels is the demo's, and a second host would otherwise
 * have to reproduce it - host.h. */
#define DRAG_EASE_MS 180

/* OURS. The snap is because the divide reaches zero before the value
 * does. */
static void drag_ease(s32 ms)
{
	if (g_drag_down || (!g_drag_x && !g_drag_y)) return;
	if (ms > DRAG_EASE_MS) ms = DRAG_EASE_MS;
	if (ms < 0) ms = 0;
	g_drag_x -= (g_drag_x / DRAG_EASE_MS) * ms;
	g_drag_y -= (g_drag_y / DRAG_EASE_MS) * ms;
	if (g_drag_x < DRAG_EASE_MS && g_drag_x > -DRAG_EASE_MS) g_drag_x = 0;
	if (g_drag_y < DRAG_EASE_MS && g_drag_y > -DRAG_EASE_MS) g_drag_y = 0;
}

/* OURS - one full deflection, host.h's +-1.0 */
static s32 clamp_unit(s32 v)
{
	if (v >  0x10000L) return  0x10000L;
	if (v < -0x10000L) return -0x10000L;
	return v;
}
static char g_label[160];
static int  g_inited;
static s32  g_host_w = 1280;
static s32  g_host_h = 720;

/* OURS */
static void relabel(void)
{
	const Relic_Part *p = Parts_Get(g_part);
	if (!p) {
		sprintf(g_label, "no part");
		return;
	}
	sprintf(g_label, "[%d/%d] %-5s %-18s  %s  %s",
	        g_part + 1, Parts_Count(), p->id, p->name,
	        p->status == PART_LIT ? "LIT   " : "SHAPED",
	        p->origin);
}

/* OURS */
int Demo_Init(const char *relic_path, const char *dat_path)
{
	/* The archive is OPTIONAL: every part reads what it needs out
	 * of the executable, and only a texture from relic.dat is lost
	 * without it. */
	if (dat_path) Dat_Open(dat_path);
	(void)dat_path;             /* parts that need it open it themselves */
	if (Image_Load(relic_path)) return 1;
	if (Gfx_Init()) { Image_Free(); return 1; }
	g_part = 0;
	g_tick = 0;
	g_inited = 1;
	relabel();
	return 0;
}

/* OURS */
void Demo_Shutdown(void)
{
	Frame_Free(&g_frame);
	if (g_rgb) { free(g_rgb); g_rgb = NULL; }
	g_rgb_bytes = 0;
	Image_Free();
	g_inited = 0;
}

/* OURS
 * The host owns the window size; the demo picks a scale that fits
 * its own part into it. SCALE IS NOT ROUNDED TO AN INTEGER - the
 * effects are resolution-parametric, so 3.5 is as valid as 4. THE
 * WIDTH IS, to a multiple of four, so a host uploading three bytes
 * a pixel never hands over an odd pitch - reconstruction.txt 3.2. */
int Demo_SetResolution(s32 w, s32 h)
{
	const Relic_Part *p = Parts_Get(g_part);
	s32 sx, sy, scale, fw;

	if (!p || w < 1 || h < 1) return 1;
	g_host_w = w;
	g_host_h = h;
	sx = FX_DIV(w, p->demo_w);
	sy = FX_DIV(h, p->demo_h);
	scale = sx < sy ? sx : sy;
	if (scale < 1) scale = 1;

	fw = (FX_MUL(p->demo_w, scale) + 3L) & ~3L;

	Frame_Free(&g_frame);
	if (Frame_Init(&g_frame, fw, FX_MUL(p->demo_h, scale), scale))
		return 1;

	if (g_rgb_bytes < g_frame.w * g_frame.h * 3) {
		if (g_rgb) free(g_rgb);
		g_rgb_bytes = g_frame.w * g_frame.h * 3;
		g_rgb = (u8 *)malloc((size_t)g_rgb_bytes);
		if (!g_rgb) return 1;
	}
	Demo_SetModes(g_modes);
	if (p->init && p->init()) return 1;
	return 0;
}

/* OURS. THE HANDOVER, AND BOTH TICK PATHS GO THROUGH IT. A part that
 * knows its length ends on it and the next starts from its own tick 0,
 * which is what maybe_Space_RunPlanetScenes does between its two
 * scenes. Demo_GotoPart resets the clock, so the new part is seeded
 * here and its first frame is drawn by the caller. */
static const Relic_Part *handover(const Relic_Part *p)
{
	s32 len = p->length ? p->length() : p->ticks;

	if (!g_auto || len <= 0 || g_tick < len) return p;
	goto_part(g_part + 1);
	p = Parts_Get(g_part);
	if (p->step) p->step(g_tick);
	return p;
}

/* OURS */
void Demo_Frame(void)
{
	const Relic_Part *p = Parts_Get(g_part);

	if (!g_inited || !p || !g_frame.px || !g_rgb) return;
	Music_Advance(PAL_FRAME_MS);
	drag_ease(PAL_FRAME_MS);
	music_follow();
	if (p->step) p->step(g_tick);
	g_frame.subtick = 0;
	p->frame(&g_frame, g_tick);
	Gfx_ResolveToRGB(&g_frame, g_rgb);
	g_tick++;
	p = handover(p);
	g_tick_fx = g_tick << 16;
	g_tick_rem = 0;
	g_home = 0;
}

/* OURS - N ticks of state for ONE render, for a host's fast forward.
 * The tail is drawn so the temporal filters refill, a part with no
 * step() cannot skip at all, and the handover ends the pass.
 * docs/reconstruction.txt 3.1. */
void Demo_Skip(s32 n)
{
	const Relic_Part *p = Parts_Get(g_part);

	if (!g_inited || !p || !g_frame.px || !g_rgb) return;
	while (n > SKIP_DRAW && p->step) {
		Music_Advance(PAL_FRAME_MS);
		Music_DrainTicks(1L);
		music_follow();
		p->step(g_tick);
		g_tick++;
		n--;
		if (handover(p) != p) break;
	}
	while (n-- > 0) Demo_Frame();
}

/* OURS. A TICK IS A PAL VERTICAL BLANK AND NOTHING ELSE SETS THE
 * RATE. Every part's tick is the count of calls to Vbl_Server
 * (0x0021f9b0), the VERTB handler the demo hangs its per-frame work
 * on, so a tick is 1/50 s whatever the host's display does. Driving
 * Demo_Frame once per presented frame instead runs the whole demo at
 * the host's refresh - and at the host's JITTER, so a part's timing
 * stops being reproducible between two machines.
 *
 * Hosts call this with elapsed milliseconds and it decides how many
 * demo frames that was: zero on a 60 Hz display one frame in six.
 * Catching up is capped, because a breakpoint or a dragged window
 * must not fast-forward the demo to make up lost time. */
void Demo_Advance(s32 ms)
{
	const Relic_Part *p = Parts_Get(g_part);
	s32 want;
	int n;

	if (!g_inited || !p || !g_frame.px || !g_rgb) return;
	if (ms < 0) ms = 0;

	/* CLAMP THE INPUT, NEVER THE LEFTOVER. A stall - a breakpoint, a
	 * dragged window - must not fast-forward the demo, so cap what one
	 * call may contribute. Zeroing the accumulator instead threw time
	 * away on every slow frame. */
	if (ms > PAL_FRAME_MS * CATCHUP_MAX) ms = PAL_FRAME_MS * CATCHUP_MAX;
	drag_ease(ms);

	/* 16.16 ticks per millisecond at 50 Hz is 3276.8, and the 0.8 is
	 * carried rather than dropped: at 60 presented frames a second a
	 * truncation here would lose a tick every few seconds. */
	g_tick_rem += ms * 32768;
	g_tick_fx += g_tick_rem / 10;
	g_tick_rem %= 10;

	want = g_tick_fx >> 16;
	n = (int)(want - g_tick);
	if (n < 0) n = 0;
	if (n > CATCHUP_MAX) {
		n = CATCHUP_MAX;                /* a long stall does not sprint */
		g_tick_fx = (g_tick + n) << 16;
	}

	if (p->step) {
		/* THE PART SEPARATES ITS STATE FROM ITS PICTURE, so the clock no
		 * longer depends on what a frame costs to draw. State steps at
		 * 50 Hz whatever the host manages; the picture is drawn once,
		 * here, with the fraction of a tick we are actually at. Before
		 * this, a frame costing over PAL_FRAME_MS made the catch-up loop
		 * draw it two or three times per presented frame, which made the
		 * next delta bigger still - the demo settled at the render rate
		 * instead of 50 Hz and ran a quarter of its proper speed. */
		while (n-- > 0) {
			Music_Advance(PAL_FRAME_MS);
	music_follow();
			p->step(g_tick);
			g_tick++;
			{
				const Relic_Part *q = handover(p);
				if (q != p) { p = q; break; }
			}
		}
		g_frame.subtick = g_tick_fx & 0xffffL;
		p->frame(&g_frame, g_tick);
		Gfx_ResolveToRGB(&g_frame, g_rgb);
	} else {
		int drew = 0;
		g_frame.subtick = 0;
		while (n-- > 0) {
			Music_Advance(PAL_FRAME_MS);
	music_follow();
			p->frame(&g_frame, g_tick);
			g_tick++;
			drew = 1;
			/* THE HANDOVER BELONGS IN BOTH BRANCHES. A part with no
			 * step() comes through here, so leaving the check in the
			 * other branch alone means every tick-pure part ignores
			 * its declared length and sits there. */
			{
				const Relic_Part *q = handover(p);
				if (q != p) {
					p = q;
					p->frame(&g_frame, g_tick);
					break;
				}
			}
		}
		if (drew) Gfx_ResolveToRGB(&g_frame, g_rgb);
	}
	g_home = 0;
}

/* OURS */
const u8 *Demo_Pixels(s32 *w, s32 *h)
{
	if (w) *w = g_frame.w;
	if (h) *h = g_frame.h;
	return g_rgb;
}

/* ORIGIN 0x0021f7f4 and 0x0021f82a - ONE TUNE AN ACT. Demo_RunAct1
 * loads Cmodule1 and runs everything up to the orb's fall;
 * Demo_RunAct2 loads Cmodule2 and runs the rest.
 * docs/reconstruction.txt 4.2. */
#define ACT2_FIRST_PART 13

/* MEASURED - 0x002c836c starts Cmodule1 on field 275 of a clean run
 * and the terminal's script reaches that point on ITS tick 242,
 * which also puts the terminal's tick 0 at field 33. THE TYPING IS
 * NOT SILENT: MUSIC_INTRO plays under it, and from tick 0 - the
 * demo's 32-field head start is its own start-up.
 * reconstruction.txt 4.2. */
#define TERM_MUSIC_AT 242L

/* OURS - which of the two the running order puts us in. */
static int music_for_part(int index, s32 tick)
{
	if (index == 0) {
		return (tick >= TERM_MUSIC_AT) ? 0 : MUSIC_INTRO;
	}
	return (index >= ACT2_FIRST_PART) ? 1 : 0;
}

/* OURS - the act's tune, followed per tick because the terminal's
 * starts partway through its own part. */
static void music_follow(void)
{
	int tune = music_for_part(g_part, g_tick);

	if (tune != g_music) {
		g_music = tune;
		Music_Start(tune);
	}
}

/* OURS - the handover's path, which leaves the timeline intact. */
static void goto_part(int index)
{
	int n = Parts_Count();
	if (n < 1) return;
	g_part = ((index % n) + n) % n;
	g_tick = 0;
	g_held_x = g_held_y = 0;
	music_follow();
	g_tick_fx = 0;
	g_tick_rem = 0;
	relabel();
	if (g_frame.px)
		Demo_SetResolution(g_host_w, g_host_h);
}

/* OURS - a host jumping, which does not. */
void Demo_GotoPart(int index)
{
	Music_BreakTimeline();
	goto_part(index);
}

void Demo_StepPart(int delta) { Demo_GotoPart(g_part + delta); }

/* OURS. RUNS THE PART'S step() WITHOUT DRAWING, which is what makes a
 * mark deep inside a part reachable at all: the state a shot needs -
 * an eased altitude, a warp scale, an LCG that has been rolled a
 * thousand times - exists only as the accumulation of every tick
 * before it. A part with no step() is a pure function of its tick and
 * needs none of this. */
static void seek_tick(s32 tick)
{
	const Relic_Part *p = Parts_Get(g_part);
	s32 t;

	if (!p || tick <= 0L) return;
	if (p->step)
		for (t = 0L; t < tick; t++) p->step(t);
	g_tick = tick;
	g_tick_fx = g_tick << 16;
	g_tick_rem = 0;
	relabel();
}

/* OURS - a host seeking within the part it is already on. */
void Demo_SeekTick(s32 tick)
{
	Music_BreakTimeline();
	g_synced = 0;
	seek_tick(tick);
}

/* OURS - A MARK THAT KNOWS ITS MUSIC IS A REFERENCE ENTRY POINT.
 * The replayer is drained to the pair the mark carries, so the run
 * from here is the run a full play-through would have had, and the
 * timeline is left INTACT - break it and every gate after the jump
 * would answer its fallback instead of its cue, which is exactly
 * what the jump was for. reconstruction.txt 4.6. */
void Demo_GotoMark(int index)
{
	const Relic_Mark *m;
	int n = Parts_MarkCount();

	if (n < 1) return;
	g_mark = ((index % n) + n) % n;
	m = Parts_Mark(g_mark);
	goto_part(m->part);
	g_synced = (Music_SeekTo(m->tune, m->order, m->row) == 0);
	if (!g_synced) Music_BreakTimeline();
	seek_tick(m->tick);
}

/* OURS - what a host prints after a jump, so the claim is checkable
 * rather than taken on trust. */
const char *Demo_MusicLine(void)
{
	static char line[96];
	u32 order = 0, row = 0;

	Music_Position(&order, &row);
	sprintf(line, "music: %02lx.%02lx - %s", (unsigned long)order,
	        (unsigned long)row,
	        g_synced ? "in sync"
	                 : "NOT seeked, gates on their fallbacks");
	return line;
}

/* OURS */
void Demo_StepMark(int delta)
{
	Demo_GotoMark(Parts_MarkOf(g_part, g_tick) + delta);
}

/* OURS */
int Demo_MarkIndex(void) { return g_mark; }

/* OURS */
const char *Demo_MarkName(void)
{
	const Relic_Mark *m = Parts_Mark(Parts_MarkOf(g_part, g_tick));
	return m ? m->name : "";
}
void Demo_SetAutoAdvance(int on) { g_auto = on; }
int  Demo_AutoAdvance(void)      { return g_auto; }
int  Demo_PartIndex(void)     { return g_part; }
s32  Demo_TickCount(void)     { return g_tick; }

/* OURS. SUBPIXEL IS FORCED OFF AT SCALE 1.0, because rule 20 makes
 * that the exact reference and there is nothing to interpolate into
 * below one destination pixel per demo unit. */
void Demo_SetModes(s32 modes)
{
	g_modes = modes;
	g_frame.modes = (g_frame.scale <= FX_ONE)
	              ? (modes & ~RELIC_MODE_SUBPIXEL) : modes;
}
s32  Demo_Modes(void)         { return g_frame.modes; }

/* OURS - src/host.h. A RELEASE KEEPS THE DEFLECTION, because
 * drag_ease has to have something left to bring home; only a held
 * pointer may set one. Full deflection is clamped here rather than
 * in a host, since +-1.0 is this boundary's own definition. UNDER
 * THE LOOK HOLD A RELEASE BANKS IT instead, so nothing eases and the
 * next press starts from where this one left off. */
void Demo_SetDrag(int down, s32 x_fx, s32 y_fx)
{
	if (!down && g_drag_down && g_look_hold) {
		g_held_x += g_drag_x;
		g_held_y = clamp_unit(g_held_y + g_drag_y);
		g_drag_x = g_drag_y = 0;
	}
	g_drag_down = down;
	if (!down) return;
	g_drag_x = clamp_unit(x_fx);
	g_drag_y = clamp_unit(y_fx);
}

/* OURS */
void Demo_Drag(int *down, s32 *x_fx, s32 *y_fx)
{
	if (down) *down = g_drag_down;
	if (x_fx) *x_fx = g_drag_x;
	if (y_fx) *y_fx = g_drag_y;
}

/* OURS - src/host.h. The yaw is not clamped, so repeated drags can
 * wind it round; the pitch is held to one full deflection. */
void Demo_Look(s32 *x_fx, s32 *y_fx)
{
	*x_fx = g_held_x + g_drag_x;
	*y_fx = clamp_unit(g_held_y + g_drag_y);
}

/* OURS - src/host.h */
void Demo_SetLookHold(int on)
{
	g_look_hold = on;
	if (!on) g_held_x = g_held_y = 0;
}
int  Demo_LookHold(void)      { return g_look_hold; }

/* OURS - src/host.h */
void Demo_SetWheel(s32 notch) { g_wheel = notch; }
s32  Demo_Wheel(void)         { return g_wheel; }

/* OURS - src/host.h. Rest drops the kept look as well. */
void Demo_SetHome(void)
{
	g_home = 1;
	g_held_x = g_held_y = 0;
}
/* OURS - src/host.h */
void Demo_SetCycle(s32 n)     { g_cycle = n; }
s32  Demo_Cycle(void)         { return g_cycle; }
void Demo_SetHold(u32 mask)   { g_hold = mask; }
u32  Demo_Hold(void)          { return g_hold; }
int  Demo_Home(void)          { return g_home; }

int  Demo_PartCount(void)     { return Parts_Count(); }
const char *Demo_PartLabel(void) { return g_label; }

/* OURS - src/host.h */
const char *Demo_StateLine(void)
{
	static char line[512];

	Parts_StateLine(line, (int)sizeof line, g_tick, g_part, g_mark);
	return line;
}
