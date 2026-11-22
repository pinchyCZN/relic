/* RELIC part 2.17 - the valley, the birds and the rock. The one
 * scene of Stills_RunModule. See docs/effects/valley.txt.
 *
 * ITS OWN FILE BECAUSE THE DEMO'S IS. valley.txt 2.7: the Stills
 * module carries its own copy of the blob skeleton, its own runtime
 * tables and its own generator, sharing only what is baked. The
 * space scenes' primitives are not reused here because the demo
 * does not reuse them. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "relic.h"
#include "image.h"
#include "gfx.h"
#include "stills.h"
#include "dat.h"
#include "parts.h"

/* THE CUE ENDS THE OPENING, NOT THE PART - armed at 0x003143fc and
 * polled at 0x00314426, both inside this part's own 0x00314304 to
 * 0x0031460c. Valley_RunScene spans this part and the city, so a
 * poll in this range closes a loop of ours and not the scene: end
 * the PART on it and the rock, the impact and the explosion never
 * run. reconstruction.txt 4.5. */
#define VY_CUE_ORDER 0x25L
#define VY_CUE_ROW   0x1dL

static Relic_Gate g_cue_gate;

/* ORIGIN 0x00314304 Valley_RunScene. */
#define VY_W            0xa0L
#define VY_H            0x64L
#define VY_TEXELS       (VY_W * VY_H)
#define VY_LIGHT_R      0x7fL
#define VY_LIGHT_G      0x5fL
#define VY_LIGHT_B      0x4fL

/* ORIGIN 0x00316748 - the IFF DEEP FORM, and 0x44424f44 is 'DBOD'. */
#define VY_FORM         0x00316748UL
#define VY_DBOD         0x44424f44UL

/* ORIGIN 0x003143e8 and 0x00314430 - the fuse holds the rock still
 * through the opening, then the second loop drops it to 1. */
#define VY_FUSE_OPEN    0x270fL

/* ORIGIN 0x003143a0 - the rock, staged off the top right. */
#define VY_ROCK_X0      0x8c00L
#define VY_ROCK_Y0      (-0xc800L)
#define VY_ROCK_S0      0xb400L
#define VY_ROCK_CX      0x50L
#define VY_ROCK_CY      0x41L
#define VY_ROCK_END     0x1400L

/* ORIGIN 0x003163f2 - one field in 0x50 arms 1..0xa of flash. */
#define VY_FLASH_ODDS   0x50L
#define VY_FLASH_LEN    0xaL
#define VY_SIN2          0xc00L

/* ORIGIN 0x00314102 - 1024 words, and the two cursors walk the
 * first 512 of them. 0x003140c4 with D7 0x780000 and D3 0xfa makes
 * the rock's ramp, the same shape as the space module's. */
#define VY_TURB         1024L
#define VY_CURSORS      512L
#define VY_RAMP_NUM     0x780000L
#define VY_RAMP_T       0xfaL

/* ORIGIN 0x00314378 and 0x003144c8 - the impact's base pass. The
 * height map is BAKED; the colour table is built from the warm
 * palette at 0x0033aa34 through the picture's own rotate. */
#define VY_HEIGHT_MAP   0x00322314UL
#define VY_PALETTE      0x0033aa34UL
/* ORIGIN 0x003144c8 - the impact stages 0x3d0 and 0x00316476 takes
 * 0x0033b050 off it EVERY FIELD, which is one here. That fall is
 * what spreads the lens outward. */
#define VY_THRESH_P3    0x3d0L
#define VY_P3_FRAMES    0x122L

/* ORIGIN 0x00315e20 - the haze's own numbers. 0x00315f66 is staged
 * 0x15e and holds there through the impact, since 0x00315f68 is
 * zero until the aftermath. */
#define VY_HAZE_ROWS    0x32L
#define VY_HAZE_ROW0    49L
#define VY_HAZE_OFF     0x15eL
#define VY_PLASMA_BIAS  0x20200L

/* OURS - how long the opening runs before the rock is released, for
 * when the cue above cannot answer. */
#define VY_OPEN_TICKS   400L
/* OURS - the impact starts on our tick 435 and runs 0x122 fields,
 * so the scene is over at 725. The demo flows straight out of it
 * into the city's preamble; there is no fourth loop. */
#define VY_TICKS        725L

/* OURS - the opening's length, as its cue or its fallback gives it. */
static s32 vy_open(void)
{
	return Parts_GateLength(&g_cue_gate, VY_OPEN_TICKS);
}

static u8 *g_pic;                   /* the decoded picture          */
static s32 g_rock_x, g_rock_y, g_rock_s;
static s32 g_fuse;                  /* 0x0033b04a                   */
static s32 g_flash;                 /* 0x0033b04c                   */
static s32 g_ph1, g_ph2, g_ph3, g_drift;
static s32 g_blast;                 /* 0x0033b07c                   */
static s32 g_spin;                  /* 0x0033b048                   */
static int g_impact;
static s32 g_thresh;                /* 0x0031654c                   */
static s32 g_rate;                  /* 0x0033b050                   */
static s32 g_iframe;                /* 0x00237218                   */
static int g_ready;














/* OURS - the demo blits at its own resolution; this samples the
 * picture per output pixel so the part scales. */
static void vy_blit_base(Relic_Frame *f)
{
	s32 x, y;

	for (y = 0; y < f->h; y++) {
		u8 *q = f->px + (size_t)(y * f->w) * PX_BYTES;
		s32 sy = FX_DIV(y, f->scale);

		if (sy < 0) sy = 0;
		if (sy >= VY_H) sy = VY_H - 1L;
		for (x = 0; x < f->w; x++, q += PX_BYTES) {
			s32 sx = FX_DIV(x, f->scale);
			const u8 *s;

			if (sx < 0) sx = 0;
			if (sx >= VY_W) sx = VY_W - 1L;
			s = g_pic + (size_t)(sy * VY_W + sx) * PX_BYTES;
			q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; q[3] = s[3];
		}
	}
}

/* ORIGIN 0x00314882 Valley_SkyFlash - the lightning. IT ENDS ON
 * dble, NOT dbf: sixteen rows. valley.txt 2.3. */
static void vy_sky_flash(Relic_Frame *f)
{
	s32 row, add = 0x2010L;

	if (!g_flash) return;
	for (row = 0; row < 0x1eL && add > 0L; row++, add -= 0x201L) {
		s32 dr = add >> 8;
		s32 dg = add & 0xffL;
		s32 y0 = FX_MUL(row, f->scale), y1 = FX_MUL(row + 1, f->scale);
		s32 y;

		for (y = y0; y < y1 && y < f->h; y++) {
			u8 *q = f->px + (size_t)(y * f->w) * PX_BYTES;
			s32 x;

			if (y < 0) continue;
			for (x = 0; x < f->w; x++, q += PX_BYTES) {
				s32 r = q[0] + dr, g = q[1] + dg;

				q[0] = (u8)(r < 0 ? 0 : (r > 255 ? 255 : r));
				q[1] = (u8)(g < 0 ? 0 : (g > 255 ? 255 : g));
			}
		}
	}
}

/* ORIGIN 0x003149f8 Valley_DrawBird - one bird, ONE PIXEL. The
 * first of the three sine pairs is DEAD in the original: 0x00314a0c
 * loads it and 0x00314a26 overwrites both registers before either
 * is read, so only the second and third move the bird.
 *
 * y is clipped BELOW ONLY, exactly as the demo has it. */
static void vy_draw_bird(Relic_Frame *f, s32 bx, s32 by, s32 phase)
{
	s32 a2 = (g_ph2 + phase) & 0x1fffL;
	s32 a3 = (g_ph3 + phase) & 0x1fffL;
	s32 dx, dy, x, y, px, py, px0, px1, py0, py1;
	u8 c0, c1, c2, c3;

	dx = ((Stills_Sin(0L, a2) >> 1) + Stills_Sin(0L, a3)) >> 12;
	dy = ((Stills_Sin(VY_SIN2, a2) >> 1) + Stills_Sin(VY_SIN2, a3)) >> 12;
	x = bx + g_drift + dx;
	if (x < 0 || x >= VY_W) return;
	y = by + dy;
	if (y < 0) return;
	if (y >= VY_H) return;

	/* ORIGIN 0x00314a74 - BIT 8 of the first phase is the wing. */
	if ((((g_ph1 + phase) >> 8) & 1L) != 0) {
		c0 = 0x28; c1 = 0x28; c2 = 0x00; c3 = 0x30;
	} else {
		c0 = 0x08; c1 = 0x08; c2 = 0x00; c3 = 0x10;
	}
	px0 = FX_MUL(x, f->scale); px1 = FX_MUL(x + 1, f->scale);
	py0 = FX_MUL(y, f->scale); py1 = FX_MUL(y + 1, f->scale);
	for (py = py0; py < py1 && py < f->h; py++) {
		u8 *q;

		if (py < 0) continue;
		q = f->px + (size_t)(py * f->w) * PX_BYTES;
		for (px = px0; px < px1 && px < f->w; px++) {
			u8 *p = q + (size_t)px * PX_BYTES;

			if (px < 0) continue;
			p[0] = c0; p[1] = c1; p[2] = c2; p[3] = c3;
		}
	}
}

/* ORIGIN 0x003148f8 Valley_DrawBirds - eight of them, from a table
 * written into the code. The phases step HERE, per draw. */
static const s32 VY_BIRD[8][3] = {
	{ 90, 70, 0x000L }, { 92, 72, 0x3e8L },
	{ 87, 75, 0x5dcL }, { 95, 77, 0x6a4L },
	{ 85, 72, 0x320L }, { 80, 72, 0x384L },
	{ 55, 72, 0x320L }, { 57, 72, 0x384L }
};

/* ORIGIN 0x003148f8 Valley_DrawBirds. */
static void vy_draw_birds(Relic_Frame *f)
{
	s32 i;

	for (i = 0; i < 8L; i++)
		vy_draw_bird(f, VY_BIRD[i][0], VY_BIRD[i][1], VY_BIRD[i][2]);
}



/* ORIGIN 0x003163d8 Valley_StepScene - the field stepper. */
static void vy_step(s32 tick)
{
	Parts_GateStep(&g_cue_gate, tick, VY_CUE_ORDER, VY_CUE_ROW,
	               VY_OPEN_TICKS);
	if (tick <= 1) {
		g_rock_x = VY_ROCK_X0;
		g_rock_y = VY_ROCK_Y0;
		g_rock_s = VY_ROCK_S0;
		g_fuse = VY_FUSE_OPEN;
		g_flash = 0;
		Stills_SeedRand(0);
		g_ph1 = g_ph2 = g_ph3 = 0;
		g_drift = -0x64L;   /* ORIGIN 0x00313ff0 */
		g_blast = 0x3fcL;   /* ORIGIN 0x003143cc */
		g_spin = 0;
		g_impact = 0;
		g_thresh = VY_THRESH_P3;
		g_rate = 0;
		g_iframe = 0;
	}
	/* ORIGIN 0x003163e0 - lightning, one field in 0x50. */
	if (g_flash > 0) g_flash--;
	else if (Stills_Rand(VY_FLASH_ODDS) == 0)
		g_flash = Stills_Rand(VY_FLASH_LEN) + 1L;

	/* GATED AS THE DEMO'S BIRD LOOP IS - phase three draws no birds,
	 * so the phases stop with them. reconstruction.txt 3.4. */
	if (!g_impact) {
		g_ph1 += 0x1c3L;
		g_ph2 += 0x53L;
		g_ph3 += 0x1fL;
		g_drift += 1L;
		if (g_drift >= 0x64L) g_drift = -0x64L;
	}
	if (tick == vy_open()) g_fuse = 1L;
	if (g_fuse > 0) { g_fuse--; return; }

	g_rock_x -= g_rock_x >> 3;
	g_rock_y -= g_rock_y >> 3;
	g_rock_s -= g_rock_s >> 4;
	/* ORIGIN 0x0031641e and 0x00316434 - both run once the fuse is
	 * clear, through the impact as well as the fall. */
	if (g_blast > 0) g_blast--;
	g_spin += 0xaL;
	Stills_StepSpin(0xaL);
	/* ORIGIN 0x00316476 - the threshold falls by the phase rate. */
	if (g_rate > 0) {
		g_thresh -= g_rate;
		if (g_thresh < 0) g_thresh = 0;
	}
	/* ORIGIN 0x00316506 - one cursor back two words, one on one. */
	Stills_StepCursors();
	/* ORIGIN 0x00237218 - a FIELD count. valley.txt 2.11. */
	if (g_impact && g_rate == 1L && ++g_iframe >= VY_P3_FRAMES)
		g_rate = 0L;
}



/* ORIGIN 0x00314470 - clamped at 0x7f, which is what stops the
 * trail being laid while the rock is still off screen. */
static s32 vy_rock_radius(void)
{
	s32 r = (g_rock_s >> 8) + 0xaL;

	return (r > 0x7fL) ? 0x7fL : r;
}

/* OURS */
static int vy_init(void)
{
	g_pic = Stills_LoadDeep(VY_FORM, VY_TEXELS);
	if (!g_pic) return 1;
	if (Stills_BlurInit(VY_TEXELS * 16L)) return 1;
	/* ORIGIN 0x00314378 - the valley's map and picture. */
	Stills_SetSources(g_pic, 0, Image_At(VY_HEIGHT_MAP));
	Stills_BuildBlobRamp();
	Stills_BuildTrailRamp();
	Stills_BuildTurbulence();
	/* ORIGIN 0x003141da - valley.txt 2.8. */
	Stills_BuildBlastRamp();
	Stills_BuildRays();
	if (Stills_BuildPlasma()) return 1;
	/* ORIGIN 0x00314392 - the valley's haze does not drift. */
	Stills_SetHazeOffset(0x15eL);
	Stills_SetHazeDrift(0L);
	Stills_BuildSplatTable(VY_PALETTE);
	Stills_ResetTrail();
	g_ready = 1;
	vy_step(1L);
	return 0;
}

/* ORIGIN 0x00314402 and 0x00314438 - the first two of the scene's
 * three loops. [open] Valley_DrawRock 0x00314c0c, its trail and the
 * impact loop at 0x003144c2 are not transcribed. valley.txt 4. */
static void vy_frame(Relic_Frame *f, s32 tick)
{
	(void)tick;
	Gfx_SetLightBase(VY_LIGHT_R, VY_LIGHT_G, VY_LIGHT_B);
	if (!g_ready) { Frame_Clear(f); return; }
	if (g_impact) {
		/* ORIGIN 0x003144f2 - the third loop, and it draws NEITHER
		 * the birds NOR the lightning. */
		s32 br = g_blast >> 3;

		/* OURS - the scene owns its counters and hands them to
		 * stills.c before it draws, where the demo has one set of
		 * module globals both halves read. */
		Stills_SetThreshold(g_thresh);
		Stills_SetBlast(g_blast);
		Stills_ImpactBase(f);
		/* ORIGIN 0x00314572 - phase three ends at 0x122 frames and
		 * phase four follows with a DIFFERENT call list: no blast,
		 * no base pass, no trail. [open] its 0x00315c34 and
		 * 0x0031589e are unread, so the part holds here. */
		/* ORIGIN 0x00314528 - the impact doubles the term. */
		Stills_DrawHaze(f, g_thresh * 2L);
		Stills_DrawBlast(f, VY_ROCK_CX, VY_ROCK_CY,
		                 (br > 0x7fL) ? 0x7fL : br);
		Stills_DrawTrail(f);
	} else {
		vy_blit_base(f);
		vy_sky_flash(f);
		vy_draw_birds(f);
		/* ORIGIN 0x00314438 - the second loop adds the rock. */
		if (!g_fuse && g_rock_s >= VY_ROCK_END) {
			s32 rx = (g_rock_x >> 8) + VY_ROCK_CX;
			s32 ry = (g_rock_y >> 8) + VY_ROCK_CY;
			s32 rr = vy_rock_radius();

			Stills_SetThreshold(g_thresh);
		Stills_Blob(f, rx, ry, rr, Stills_BlobRamp(), 1);
			Stills_SpawnTrail(rx, ry, rr);
			Stills_DrawTrail(f);
		} else if (!g_fuse) {
			/* ORIGIN 0x003144c2 - phase three begins. */
			g_impact = 1;
			g_thresh = VY_THRESH_P3;
			g_rate = 1L;
			g_iframe = 0;
		}
	}
	Stills_Blur(f);
}

/* OURS - key=value on the demo's own names. parts.h. */
static void vy_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out,
	        "rockx=%ld rocky=%ld rocksize=%ld"
	        " screenx=%ld screeny=%ld radius=%ld"
	        " fuse=%ld flash=%ld drift=%ld blast=%ld impact=%d"
	        " ready=%d",
	        (long)g_rock_x, (long)g_rock_y, (long)g_rock_s,
	        (long)((g_rock_x >> 8) + VY_ROCK_CX),
	        (long)((g_rock_y >> 8) + VY_ROCK_CY),
	        (long)((g_rock_s >> 8) + 0xaL),
	        (long)g_fuse, (long)g_flash, (long)g_drift,
	        (long)(g_blast >> 3), g_impact, g_ready);
}

/* OURS - parts.h. */
static s32 vy_length(void)
{
	return vy_open() + (VY_TICKS - VY_OPEN_TICKS);
}

const Relic_Part PART_Valley = {
	"2.17", "The valley, the birds and the rock",
	"Valley_RunScene 0x00314304",
	PART_LIT, VY_W, VY_H,
	vy_init, vy_step, vy_frame, vy_stats,
	VY_TICKS, 0, vy_length
};
