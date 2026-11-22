/* RELIC part 2.12b - the orb falls toward Earth. ANIM MODE 0xe,
 * Space_RunEarth's two loops.
 *
 * The short shot that closes the space sequence. The camera holds on
 * the planet, the sphere drops away toward it and shrinks, and the
 * seams in it wink out one by one as the crack threshold climbs.
 *
 * TWO THINGS HERE ARE NEW AND THE REST IS REUSE. The planet is a
 * texture BUILT at run time out of the skybox, and its blue limb is
 * baked into that texture rather than drawn over the frame.
 *
 * See docs/effects/earth.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include "parts.h"
#include "gfx.h"
#include "dat.h"
#include "space.h"
#include "image.h"
#include "orb.h"
#include "host.h"

/* ORIGIN 0x002cbef8 - INHERITED; Space_RunEarth builds no tables. */
#define EA_LIGHT_R    0x3fL
#define EA_LIGHT_G    0x3fL
#define EA_LIGHT_B    0x4fL

/* g_FlatScroll IS INHERITED; measured at the first tick [observed,
 * on hardware]. earth.txt 6. */
#define EA_SCROLL0    0x9200L

/* ORIGIN 0x002cc0ba Space_RunEarth, +0x22 onward - the seeds. */
#define EA_DEPTH0     0x96L
#define EA_THRESH0    0x7d0L
#define EA_SCROLL_Y0  2L

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x578 - mode 0xe's rates.
 * earth.txt 4 has the table. */
#define EA_WARP_A_STEP (-3L)
#define EA_YAW_STEP    0x28L
#define EA_PITCH_STEP  0xfL
#define EA_SCROLL_STEP 0x200L
#define EA_CRACK_A_ST  0x25L
#define EA_CRACK_B_ST  0x53L
#define EA_THRESH_STEP 0x1eL
#define EA_DEPTH_STEP  0xaL
#define EA_SCROLL_Y_ST 2L
#define EA_SCROLL_Y_END 0x64L

/* MEASURED - phase 1 ends on the (0x29,0) music cue, which a part
 * with no replayer cannot poll. 323 fields in a clean run from the
 * demo's first frame; it is run-dependent, so this is the fallback
 * and not the mechanism. earth.txt 4. */
#define EA_PH2_AT     323L
#define EA_CUE_ORDER  0x29L
#define EA_CUE_ROW    0L

/* ORIGIN 0x002cc18a - FIFTY steps, not 49: the loop ends when
 * g_Ham8ScrollY REACHES 0x64, so the tick writing 100 is drawn too.
 * EA_HOLD is OURS - earth.txt 5.1. */
#define EA_CLOSE      50L
/* THE HOLD AFTER THE SHUTTER IS A CUE TOO. Space_RunEarth arms
 * (0x29,0x3e) at 0x002cc1dc and WAITS on it at 0x002cc1e2 through
 * Music_WaitCue - reconstruction.txt 4.5 - so the 200 is only the
 * fallback. */
#define EA_HOLD_ORDER 0x29L
#define EA_HOLD_ROW   0x3eL
#define EA_HOLD       200L
#define EA_TICKS      (EA_PH2_AT + EA_CLOSE + EA_HOLD)

/* ORIGIN 0x002cdb70 Flash_FadeInFromOrbDepth - the opening white,
 * spent by the time the depth reaches this. */
#define EA_FADE_UNTIL 0x140L

/* ORIGIN 0x002cdef0 - A LIGHT SPHERE OVER THE WHOLE PLANET, which is
 * what keeps the texture's dark texels from reading as holes: the
 * warp copies LIGHT straight out of the texture and the texture's is
 * zero everywhere. Radius 0x7f from a rotated offset, out of
 * Fplasma. 0x002d2218 is a DATA byte after Space_DrawSunBlob's rts,
 * not self-modified code, and this is what stages it. earth.txt 2.1. */
#define EA_SUN_DX      (-0xcL)
#define EA_SUN_DY      (-0x7eL)
#define EA_SUN_R       0x7fL
#define EA_SUN_BIAS    0x5aL

/* ORIGIN 0x002d1f52 Orb_ProjectAndFlare and Scene_RenderEarth's two
 * gates - the same pair the release uses. */
#define EA_PROJ_NUM    0x3200L
#define EA_CX          0x50L
#define EA_CY          0x32L
#define EA_RADIUS_GATE 0x50L
#define EA_DEPTH_GATE  0x96L

/* ORIGIN 0x002d3026 Earth_BuildTexture - the three passes.
 * g_HaloRamp is the limb's colour. */
#define EA_TEX_TEXELS  0x10000L
#define EA_REMAP_BIAS  0x118L
#define EA_REMAP_MASK  0x3ffffL
#define EA_STAMP_AT    (0x8c00L / 4L)
#define EA_STAMP_ROWS  0x5cL
#define EA_STAMP_COLS  0xf0L
#define EA_STAMP_PITCH (0x400L / 4L)
#define EA_LIMB_AT     (0x1380cL / 4L)
#define EA_LIMB_ROWS   100L
#define EA_LIMB_COLS   250L
#define EA_LIMB_X0     0x280L
#define EA_LIMB_Y0     (-0x378L)
#define EA_LIMB_STEP   8L
#define EA_LIMB_R      0x41aL
#define EA_HALO_RAMP   0x002e13c8UL

/* OURS */
static s32 g_tick;
static s32 g_depth, g_thresh, g_crack_a, g_crack_b;
static s32 g_warp_a, g_yaw, g_pitch, g_scroll, g_scroll_y;
static s32 g_ph2_at;            /* the tick the cue answered on */
static s32 g_hold_at;           /* and the shutter's hold        */
static s32 g_sine_i, g_drift_x;
static s32 g_sx, g_sy, g_sr;
static u8 *g_planet;
static const u8 *g_sun;             /* Fplasma, the sun's skin */
static u32 g_sun_len;
static int g_ready;

/* ORIGIN 0x002cdeaa Scene_RenderEarth, +0x14 and +0x22 - both object
 * angles come off the LATCHED camera pair, the second inverted in its
 * low byte only. */
static s32 angle_a(void) { return g_yaw >> 5; }

/* ORIGIN 0x002cdeaa Scene_RenderEarth, +0x22 - the low byte only. */
static s32 angle_b(void)
{
	s32 d = g_pitch >> 5;

	return (s32)(s16)((d & ~0xffL) | ((~d) & 0xffL));
}

/* ORIGIN 0x002d3026 Earth_BuildTexture - THE PLANET IS GENERATED.
 * Three passes: a spherical remap of the skybox, a transparent stamp
 * of Fearth, then a limb glow ADDED in. earth.txt 2. */
static int earth_build(void)
{
	const u8 *sky = Space_SkyTexture();
	const s32 *dir = Space_SkyDir();
	const u8 *overlay;
	u32 len;
	s32 i, r, c;

	if (!sky || !dir) return 1;
	g_planet = (u8 *)malloc((size_t)EA_TEX_TEXELS * PX_BYTES);
	if (!g_planet) return 1;

	for (i = 0; i < EA_TEX_TEXELS; i++) {
		s32 d = (dir[i] + EA_REMAP_BIAS) & EA_REMAP_MASK;
		const u8 *t = sky + d;
		u8 *o = g_planet + (size_t)i * PX_BYTES;

		o[0] = t[0]; o[1] = t[1]; o[2] = t[2]; o[3] = t[3];
	}

	/* Pass two: Fearth is 0x5c rows of 0xf0 four-byte pixels, which
	 * is its 0x15900 bytes exactly, and a zero longword is a hole. */
	overlay = Dat_Chunk("Fearth", &len);
	if (!overlay || len < (u32)(EA_STAMP_ROWS * EA_STAMP_COLS * 4L))
		return 1;
	for (r = 0; r < EA_STAMP_ROWS; r++) {
		const u8 *src = overlay + (size_t)(r * EA_STAMP_COLS) * PX_BYTES;
		u8 *dst = g_planet
		        + (size_t)(EA_STAMP_AT + r * EA_STAMP_PITCH) * PX_BYTES;

		for (c = 0; c < EA_STAMP_COLS; c++, src += PX_BYTES, dst += PX_BYTES) {
			if (!src[0] && !src[1] && !src[2] && !src[3]) continue;
			dst[0] = src[0]; dst[1] = src[1];
			dst[2] = src[2]; dst[3] = src[3];
		}
	}

	/* Pass three: |r - EA_LIMB_R| peaks ON a circle, so this is a
	 * limb glow and not a disc, and it ADDS into what is there. */
	for (r = 0; r < EA_LIMB_ROWS; r++) {
		s32 x = EA_LIMB_X0 + r * EA_LIMB_STEP;
		u8 *dst = g_planet
		        + (size_t)(EA_LIMB_AT + r * EA_STAMP_PITCH) * PX_BYTES;

		for (c = 0; c < EA_LIMB_COLS; c++, dst += PX_BYTES) {
			s32 y = EA_LIMB_Y0 + c * EA_LIMB_STEP;
			s32 v = (Gfx_Isqrt(x * x + y * y) - EA_LIMB_R) * 2L;
			const u8 *ramp;
			int k;

			if (v < 0) v = -v;
			if (v > 0xffL) v = 0xffL;
			ramp = Image_At((u32)(EA_HALO_RAMP + (u32)(v * 4L)));
			if (!ramp) continue;
			for (k = 0; k < PX_BYTES; k++) {
				s32 t = dst[k] + (ramp[k] >> 1);
				dst[k] = (u8)(t > CHAN_MAX ? CHAN_MAX : t);
			}
		}
	}
	return 0;
}

/* ORIGIN 0x002d1f52 Orb_ProjectAndFlare. */
static void orb_project(void)
{
	s32 wx;

	if (g_depth <= 0) { g_sr = 0; return; }
	/* ORIGIN 0x002cdf32 - the render hands g_OrbDriftX >> 8 in as a
	 * WORLD x and Orb_ProjectAndFlare divides it by the depth, so the
	 * sideways travel shrinks as the orb recedes. Using it as a screen
	 * offset walks the orb straight off the frame. */
	wx = g_drift_x >> 8;
	g_sx = ((wx << 8) / g_depth) + EA_CX;
	g_sy = EA_CY;
	g_sr = EA_PROJ_NUM / g_depth;
}

/* ORIGIN 0x002cdb70 Flash_FadeInFromOrbDepth - it SETS rather than
 * adds, and is spent seventeen ticks in. */
static void fade_in(Relic_Frame *f)
{
	s32 amt = g_depth - EA_FADE_UNTIL;
	s32 i, n = f->w * f->h;

	if (amt >= 0) return;
	amt = (-amt) >> 1;
	if (amt > CHAN_MAX) amt = CHAN_MAX;
	for (i = 0; i < n; i++)
		f->px[(size_t)i * PX_BYTES + PX_LIGHT] = (u8)amt;
}

/* OURS */
static int ea_init(void)
{
	g_tick = 0;
	g_depth = EA_DEPTH0;
	g_thresh = EA_THRESH0;
	g_crack_a = 0;
	g_crack_b = 0;
	g_warp_a = 0;
	g_yaw = 0;
	g_pitch = 0;
	g_scroll = EA_SCROLL0;
	g_scroll_y = 0;
	g_ph2_at = 0;
	g_hold_at = 0;
	g_sine_i = 0;
	g_drift_x = 0;
	g_ready = 0;
	if (g_planet) { free(g_planet); g_planet = 0; }
	if (Space_Init()) return 1;
	if (Orb_Init()) return 1;
	if (earth_build()) return 1;
	g_sun = Dat_Chunk("Fplasma", &g_sun_len);
	if (!g_sun) return 1;
	Gfx_SetLightBase(EA_LIGHT_R, EA_LIGHT_G, EA_LIGHT_B);
	orb_project();
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x578 - mode 0xe. */
static void ea_step(s32 tick)
{
	g_tick = tick;
	g_warp_a += EA_WARP_A_STEP;
	g_yaw += EA_YAW_STEP;
	g_pitch += EA_PITCH_STEP;
	g_scroll = (g_scroll - EA_SCROLL_STEP) & 0xffffL;
	g_crack_a += EA_CRACK_A_ST;
	g_crack_b += EA_CRACK_B_ST;
	g_thresh += EA_THRESH_STEP;
	g_depth += EA_DEPTH_STEP;
	g_drift_x -= Orb_Sin(g_sine_i) >> 3;
	g_sine_i += 1L;
	if (tick <= 1L) Music_Arm(EA_CUE_ORDER, EA_CUE_ROW, EA_PH2_AT);
	if (g_ph2_at) {
		s32 shut = g_ph2_at + EA_CLOSE;

		if (tick == shut)
			Music_Arm(EA_HOLD_ORDER, EA_HOLD_ROW, EA_HOLD);
		if (!g_hold_at && tick > shut && Music_Reached(tick - shut))
			g_hold_at = tick - shut;
	}
	if (!g_ph2_at && tick > 0L && Music_Reached(tick)) {
		g_ph2_at = tick;
		g_scroll_y = EA_SCROLL_Y0;
	} else if (g_scroll_y) {
		g_scroll_y += EA_SCROLL_Y_ST;
		if (g_scroll_y > EA_SCROLL_Y_END) g_scroll_y = EA_SCROLL_Y_END;
	}
	orb_project();
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x5ce - the shot closes by
 * walking the DISPLAY WINDOW shut, not by drawing over the picture:
 *
 *     DIWSTRT = ((0x3e + g_Ham8ScrollY) << 8) | 0x83
 *     DIWSTOP = ((0x02 - g_Ham8ScrollY) << 8) | 0xbf
 *
 * The window is 196 scanlines over the frame's 100 rows, so each
 * edge takes one row a tick and they meet in the middle as the
 * counter reaches 0x64. OURS is the blacking: a port has no display
 * window, so the same bars are painted into the frame. */
static void letterbox(Relic_Frame *f)
{
	s32 bar = FX_MUL(g_scroll_y / 2L, f->scale);
	s32 y;

	if (bar <= 0) return;
	if (bar > f->h / 2) bar = f->h / 2;
	for (y = 0; y < bar; y++) {
		u8 *t = f->px + (size_t)(y * f->w) * PX_BYTES;
		u8 *b = f->px + (size_t)((f->h - 1 - y) * f->w) * PX_BYTES;
		s32 i;

		for (i = 0; i < f->w * PX_BYTES; i++) { t[i] = 0; b[i] = 0; }
	}
}

/* OURS */
static void ea_frame(Relic_Frame *f, s32 tick)
{
	(void)tick;
	Frame_Clear(f);
	if (!g_ready || g_scroll_y >= EA_SCROLL_Y_END) return;

	/* ORIGIN 0x002cdeec - the planet fills the frame through the same
	 * affine walk the release's hull uses, rotate-only. */
	Warp_Fill(f, g_planet, g_warp_a, 0L, 0);

	/* ORIGIN 0x002cdef0 - the light sphere, before the orb. */
	{
		s32 bx, by;

		Orb_Rotate2D(EA_SUN_DX, EA_SUN_DY, g_warp_a ^ 0x1fffL,
		             &bx, &by);
		Orb_DrawSunBlob(f, g_sun, g_sun_len, bx + EA_CX, by + EA_CY,
		                EA_SUN_R, (s32)(s16)angle_a() >> 2,
		                EA_SUN_BIAS);
	}

	/* ORIGIN 0x002cdf46 - the orb's two gates are the release's. */
	if (g_depth >= EA_DEPTH_GATE && g_sr > 0) {
		Orb_DrawSphere(f, g_sx, g_sy, g_sr, 0L, angle_a(), angle_b());
		if (g_sr < EA_RADIUS_GATE)
			Orb_DrawShells(f, g_sx, g_sy, g_sr, g_crack_a, g_crack_b,
			               g_thresh, angle_a(), angle_b(), g_scroll);
	}
	fade_in(f);
	letterbox(f);
}

/* OURS */
static void ea_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "tick %ld | depth %ld | thresh %ld | orb %ld,%ld r%ld"
	             " | warp %ld | live %ld shells %ld | bars %ld",
	        (long)g_tick, (long)g_depth, (long)g_thresh,
	        (long)g_sx, (long)g_sy, (long)g_sr, (long)g_warp_a,
	        (long)Orb_PointsLive(), (long)Orb_ShellsDrawn(),
	        (long)(g_scroll_y / 2L));
}

/* OURS - the shutter and the hold run from wherever the cue fell. */
static s32 ea_length(void)
{
	return (g_ph2_at ? g_ph2_at : EA_PH2_AT) + EA_CLOSE
	     + (g_hold_at ? g_hold_at : EA_HOLD);
}

const Relic_Part PART_Earth = {
	"2.12b", "The orb falls to Earth",
	"Space_RunEarth 0x002cc0ba, anim mode 0xe",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	ea_init, ea_step, ea_frame, ea_stats,
	EA_TICKS, 0, ea_length
};
