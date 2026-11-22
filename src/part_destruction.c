/* RELIC part 2.19 - the planet destroyed. Space_RunDestruction
 * 0x002c8438, which is SpaceModule_Run's THIRD mode, run from
 * Demo_RunAct2 0x0021f8a6 between the stills module and the temple
 * module's second call.
 *
 * ALMOST NOTHING HERE IS NEW. The planet, its halo, the flare and the
 * flat vessel are the same routines the opening space scenes and the
 * return flight already use; what the scene adds is a layered glow
 * and, after the white flash, three thousand pieces of debris.
 *
 * THE PLANET IS AT THE WORLD ORIGIN with the camera's yaw and pitch
 * both zero, which is why it sits centred where every earlier space
 * scene has it at an edge.
 *
 * See docs/effects/destruction.txt. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "relic.h"
#include "image.h"
#include "gfx.h"
#include "space.h"
#include "dat.h"
#include "parts.h"

/* The music cue the scene ends on - armed at 0x002c8588, polled at
 * 0x002c85d0. parts.h Relic_Gate. */
#define DE_CUE_ORDER 0x27L
#define DE_CUE_ROW   0x3cL
#define DE_TICKS (((DE_TIMER0 - DE_TIMER_END) / DE_TIMER_STEP) + 2L \
                  + DE_AFTER_TICKS + DE_SETUP)

/* ORIGIN 0x002c844e - the black fields between the wait returning
 * and the scene's own copper list. city.txt 5.2. */
#define DE_SETUP 7L
static Relic_Gate g_music_gate;

/* ORIGIN 0x002c8438 - the scene's staged counters. */
#define DE_TIMER0       200L            /* 0x002c8c98               */
#define DE_TIMER_END    (-0x385L)       /* phase one's floor        */
#define DE_TIMER_STEP   10L             /* taken off a FIELD        */
#define DE_FUSE         400L            /* 0x002e0bf2               */

/* ORIGIN 0x002c8558 - the amber ramp, on NEARLY DOUBLE its usual
 * numerator. destruction.txt 6. */
#define DE_RAMP_R       0x6fL
#define DE_RAMP_G       0x4fL
#define DE_RAMP_B       0x2fL
#define DE_RAMP_NUM     0xfa0000L

/* ORIGIN 0x002c8502 - Ham8_BuildTables gets 0x6f/0x4f/0x3f here,
 * REVERSED from the opening space scenes' 0x3f/0x4f/0x6f, so light
 * washes toward a warm white rather than a blue one. */
#define DE_LIGHT_R      0x6fL
#define DE_LIGHT_G      0x4fL
#define DE_LIGHT_B      0x3fL

/* ORIGIN 0x002c8890 - the flare, dead centre at a fixed radius. */
#define DE_FLARE_CX     0x50L
#define DE_FLARE_CY     0x32L
#define DE_FLARE_R      0x7fL

/* ORIGIN 0x002c88b8 - the flat vessel's byte offset into its sheet,
 * which differs from the return flight's. */
#define DE_SPRITE_AT    0x64b8L

/* ORIGIN 0x002c8ca2 - the haze's geometry, identical in every
 * constant to the valley's. destruction.txt 3. */
#define DE_HAZE_ROWS    0x32L
#define DE_HAZE_ROW0    49L
#define DE_PLASMA_BIAS  0x20200L
#define DE_PLASMA_LEN   0x40000L
/* ORIGIN 0x002ccb08 - the same vignette Valley_DarkenPlasma applies,
 * on a keep-band of 0x1e rows where the valley's is 0x28. */
#define DE_PLASMA_BAND  0x1eL
/* ORIGIN 0x002c8d3e - the gate map is BAKED, a radial field with a
 * hole where the planet sits. The pass pointers are +0x1ea0 and
 * +0x1f40 into it, as the valley's are into its height map. */
#define DE_GATE_MAP     0x002e9290UL

/* ORIGIN 0x002c88c8 - the aftermath blits a finished picture out of
 * the plasma buffer and forces one LIGHT level over the whole of it. */
#define DE_STILL_AT     0x116e8L
/* ORIGIN 0x002c88bc - the row advance is +0x61 after 160 columns, so
 * the still is a 160 wide WINDOW INTO A 256 WIDE GRID. Reading it at
 * 160 bands the picture. */
#define DE_STILL_ROW    0x100L

/* ORIGIN 0x002c8630 - the debris sprite sheet: 0x300 longwords from a
 * byte index table through a palette, every byte divided by eight,
 * and the first 0xc1 entries lifted again by 0x08040002. */
#define DE_SHEET_IDX    0x002ed510UL
#define DE_SHEET_PAL    0x002ed110UL
#define DE_SHEET_N      0x300L
#define DE_SHEET_ROW    0x20L
#define DE_SHEET_BOOST  0xc1L

/* ORIGIN 0x002c873c and 0x002c8c12 - the kind weighting and the four
 * banks of four sprite frames. destruction.txt 4. */
#define DE_KIND_TABLE   0x002c873cUL
#define DE_FRAME_TABLE  0x002c8c12UL
#define DE_PART_N       0xbb8L
#define DE_PART_Z0      0xfa0L
/* OURS - the aftermath ends on a music cue we do not read. It is
 * over once the advance has passed every node's z. */
#define DE_AFTER_TICKS  0x1f4L

/* ORIGIN 0x002c8c52 - the glow is Destruct_DrawHaze three times over,
 * at three depths. The first two track the timer. */
#define DE_GLOW_A       0x352L
#define DE_GLOW_B       0x4e2L
#define DE_GLOW_C       0xc8L


static s32 g_timer;                 /* 0x002c8c98                   */
static s32 g_gate;                  /* 0x002c8c9e, the glow's cut   */
static s32 g_spin;                  /* g_ApproachSpin               */
static s32 g_zadv;                  /* 0x002c8624                   */
static s32 g_anim;                  /* 0x002e0c7c                   */
static s32 g_ringphase;             /* g_RingPhaseA                 */
static s32 g_afterlight;            /* 0x002c8622                   */
static int g_ready;
static u8 *g_plasma;                /* Fplasma, vignetted           */
static const u8 *g_gatemap;
static s32 g_phase;                 /* 0 the planet, 1 the aftermath */
static s32 g_white;                 /* ORIGIN 0x002c8868, two frames */

/* ORIGIN 0x002c867e - 0x20 bytes a node, +0x0c unused. */
typedef struct De_Node {
	s32 vx, vy, z, kind, radius, index;
	u32 colour;
} De_Node;

static De_Node *g_part;
static u32 *g_sheet;

static int de_build_plasma(void);
static int de_build_sheet(void);
static int de_seed(void);

/* OURS */
static int de_init(void)
{
	if (Space_Init()) return 1;
	Gfx_BuildColourRamp(g_ColRampAmber, DE_RAMP_R, DE_RAMP_G,
	                    DE_RAMP_B, DE_RAMP_NUM);
	g_timer = DE_TIMER0;
	g_gate = 0;
	g_spin = 0;
	g_zadv = 0;
	g_anim = 0;
	g_ringphase = 0;
	g_afterlight = 0x28L;
	g_gatemap = Image_At(DE_GATE_MAP);
	if (!g_gatemap || de_build_plasma()) return 1;
	if (de_build_sheet() || de_seed()) return 1;
	if (Space_BlurInit()) return 1;
	g_phase = 0;
	g_white = 0;
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x002d58f4 Anim_ModeStep's mode 6 - the whole of the scene's
 * motion, once a FIELD. The gate it derives is what opens the glow. */
static void de_step(s32 tick)
{
	s32 v;

	Parts_GateStep(&g_music_gate, tick, DE_CUE_ORDER, DE_CUE_ROW,
	               DE_TICKS);
	if (tick <= DE_SETUP || !g_ready) return;
	/* ORIGIN 0x002d58f4 - the aftermath's light only starts climbing
	 * once the timer is past -0xaf0, deep into phase one. */
	if (g_timer < -0xaf0L && g_afterlight < 0xffL) g_afterlight += 2L;
	g_zadv += 10L;
	g_anim += 1L;
	g_spin += 1L;
	g_timer -= DE_TIMER_STEP;
	/* ORIGIN 0x002d5946 - THE TERM IS ZEROED WHILE IT IS POSITIVE,
	 * not while it is negative, so the gate sits at 0xff and the
	 * glow stays invisible until the timer passes -0x280. */
	v = (s32)(s16)(g_timer + 0x280L);
	if (v > 0) v = 0;
	v = -v;
	if (v > 0xffL) v = 0xffL;
	g_gate = v ^ 0xffL;
	/* ORIGIN 0x002c8862 - the first loop ends when the timer passes
	 * -0x385. Space_FillWhite is then presented TWICE and the timer
	 * restarts at zero for the aftermath. */
	if (g_phase == 0L && g_timer <= DE_TIMER_END) {
		if (++g_white >= 2L) { g_phase = 1L; g_timer = 0L; }
	}
}

/* ORIGIN 0x002ccb08 - darkens Fplasma's edges in place, exactly as
 * Valley_DarkenPlasma does for the stills module but on a tighter
 * band. The glow reads the result. destruction.txt 3. */
static int de_build_plasma(void)
{
	const u8 *src;
	u32 len;
	s32 l;

	src = Dat_Chunk("Fplasma", &len);
	if (!src || len < (u32)DE_PLASMA_LEN) return 1;
	free(g_plasma);
	g_plasma = (u8 *)malloc((size_t)DE_PLASMA_LEN);
	if (!g_plasma) return 1;
	memcpy(g_plasma, src, (size_t)DE_PLASMA_LEN);
	for (l = 0; l < 0x10000L; l++) {
		s32 row = (l >> 8) - 0x80L;
		s32 d = (row < 0 ? -row : row) - DE_PLASMA_BAND;
		u8 *q = g_plasma + (size_t)l * 4L;
		s32 k;

		if (d <= 0) continue;
		for (k = 0; k < 4L; k++)
			q[k] = (u8)(q[k] >= d ? q[k] - d : 0L);
	}
	return 0;
}

/* ORIGIN 0x002c8ca2 Destruct_DrawHaze - the space module's own copy
 * of Valley_DrawHaze, same geometry throughout. It gates on a BAKED
 * radial field rather than a height map, and its write SATURATES
 * through g_LightSat where the valley's add.b wraps.
 * destruction.txt 3. */
static void de_haze(Relic_Frame *f, s32 term)
{
	s32 d4, rows, row, pass;
	s32 step_col, row_acc, row_step, col0;

	if (!g_plasma || !g_gatemap) return;
	if (term < 0x14L) term = 0x14L;
	d4 = 0xaa0000L / term;
	if (d4 <= 0) return;
	col0 = -0xc0000L / d4;
	row_step = 0x640000L / d4;
	step_col = (s32)(((u32)(0x320000L / d4) >> 8)
	                 | ((u32)(0x320000L / d4) << 24));
	rows = d4 >> 8;
	if (rows >= DE_HAZE_ROWS) rows = DE_HAZE_ROWS - 1L;

	for (pass = 0; pass < 2L; pass++)
	for (row = 0, row_acc = 0; row < rows;
	     row++, row_acc += pass ? row_step : -row_step) {
		s32 dy = pass ? DE_HAZE_ROW0 + 1L + row : DE_HAZE_ROW0 - row;
		u32 acc = (u32)col0;
		s32 col, py0, py1;

		if (dy < 0 || dy >= SPACE_H) break;
		py0 = FX_MUL(dy, f->scale);
		py1 = FX_MUL(dy + 1L, f->scale);
		for (col = 0; col < SPACE_W; col++) {
			s32 h = g_gatemap[dy * SPACE_W + col];
			u32 prev;
			s32 i0, i1, add, px, px0, px1, py;

			if (g_gate <= h) {
				i0 = (s32)(s16)(u16)(((u32)row_acc & 0xff00UL)
				                     | (acc & 0xffUL));
				i1 = (s32)(s16)(u16)((u32)(i0 + g_ringphase));
				add = (s32)((((RD_U32(g_plasma + DE_PLASMA_BIAS
				                      + i0 * 4L) >> 16)
				             + RD_U32(g_plasma + DE_PLASMA_BIAS
				                      + i1 * 4L))) & 0xffUL);
				if (add) {
					px0 = FX_MUL(col, f->scale);
					px1 = FX_MUL(col + 1L, f->scale);
					for (py = py0; py < py1 && py < f->h; py++) {
						u8 *q;

						if (py < 0) continue;
						q = f->px + (size_t)(py * f->w) * PX_BYTES;
						for (px = px0; px < px1 && px < f->w; px++) {
							u8 *pp = q + (size_t)px * PX_BYTES;

							if (px < 0) continue;
							pp[PX_LIGHT] = g_LightSat[
							    (pp[PX_LIGHT] + add) & 0xffL];
						}
					}
				}
			}
			prev = acc;
			acc += (u32)step_col;
			if (acc < prev)
				acc = (acc & 0xffffff00UL) | ((acc + 1UL) & 0xffUL);
		}
	}
}

/* ORIGIN 0x002c8630 Destruct_BuildSpriteSheet. */
static int de_build_sheet(void)
{
	const u8 *idx = Image_At(DE_SHEET_IDX);
	const u8 *pal = Image_At(DE_SHEET_PAL);
	s32 i;

	if (!idx || !pal) return 1;
	free(g_sheet);
	g_sheet = (u32 *)malloc((size_t)DE_SHEET_N * sizeof(u32));
	if (!g_sheet) return 1;
	for (i = 0; i < DE_SHEET_N; i++) {
		u32 v = 0;

		if (idx[i]) {
			u32 q = RD_U32(pal + (size_t)idx[i] * 4L);

			v = (((q & 0xff0000UL) << 8) | ((q & 0xff00UL) << 8)
			     | (q & 0xffUL)) & 0xf8f8f8f8UL;
			v >>= 3;
		}
		g_sheet[i] = v;
	}
	for (i = 0; i < DE_SHEET_BOOST; i++)
		if (g_sheet[i]) g_sheet[i] += 0x08040002UL;
	return 0;
}

/* ORIGIN 0x002c867e Destruct_SeedParticles - a radial velocity off a
 * random angle, and a kind drawn from a weighted table.
 *
 * THE KIND TABLE IS READ AT RUNTIME, all hundred entries, because
 * only EIGHTY of them exist: Math_Rand(0x64) indexes past the table
 * into Destruct_DrawFrame's own code, and those nodes match no kind
 * and never draw. Transcribing eighty entries would lose that. */
static int de_seed(void)
{
	const u8 *kinds = Image_At(DE_KIND_TABLE);
	s32 i;

	if (!kinds) return 1;
	free(g_part);
	g_part = (De_Node *)malloc((size_t)DE_PART_N * sizeof(De_Node));
	if (!g_part) return 1;
	for (i = 0; i < DE_PART_N; i++) {
		De_Node *n = &g_part[i];
		s32 a = Gfx_Rand(0x2000L);
		s32 s = Gfx_Rand(0xd2L) + 4L;

		n->vx = (Gfx_Sin(a) * s) >> 13;
		n->vy = (Gfx_Cos(a) * s) >> 13;
		n->z = Gfx_Rand(0x7d0L) + DE_PART_Z0;
		n->radius = Gfx_Rand(0x1eL) + 0x14L;
		n->kind = (s32)RD_U32(kinds + (size_t)Gfx_Rand(0x64L) * 4L);
		n->index = i;
		n->colour = Gfx_Rand(5L) ? 0UL : 0x30200010UL;
	}
	return 0;
}

/* OURS - one 6 by 5 sprite, zero texels skipped so the sheet carries
 * its own mask. */
static void de_blit_sprite(Relic_Frame *f, s32 bank, const De_Node *n,
                           s32 sx, s32 sy, u32 light)
{
	const u8 *tbl = Image_At(DE_FRAME_TABLE);
	s32 at, r, col;

	if (!tbl || !g_sheet) return;
	at = (s32)RD_U32(tbl + (size_t)(((n->index + (g_anim >> 1)) & 3L)
	                                + bank) * 4L);
	for (r = 0; r < 5L; r++)
		for (col = 0; col < 6L; col++) {
			s32 o = at + r * DE_SHEET_ROW + col;
			u32 texel;
			s32 px0, px1, py0, py1, px, py;

			if (o < 0 || o >= DE_SHEET_N) continue;
			texel = g_sheet[o];
			if (!texel) continue;
			px0 = FX_MUL(sx + col, f->scale);
			px1 = FX_MUL(sx + col + 1L, f->scale);
			py0 = FX_MUL(sy + r, f->scale);
			py1 = FX_MUL(sy + r + 1L, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *pp = q + (size_t)px * PX_BYTES;
					u32 v = texel | light;

					if (px < 0) continue;
					pp[0] = (u8)(v >> 24); pp[1] = (u8)(v >> 16);
					pp[2] = (u8)(v >> 8);  pp[3] = (u8)v;
				}
			}
		}
}

/* ORIGIN 0x002c8974 Destruct_DrawParticles - a PERSPECTIVE DIVIDE,
 * not a 2D step: vx and vy never change and the whole motion is in z.
 * destruction.txt 4. */
static void de_particles(Relic_Frame *f)
{
	s32 i;

	if (!g_part) return;
	for (i = 0; i < DE_PART_N; i++) {
		const De_Node *n = &g_part[i];
		s32 z = n->z - g_zadv;
		s32 sx, sy, r, d, light;

		if (z <= 0x64L) continue;
		sx = ((n->vx << 8) / z) + 0x50L;
		sy = ((n->vy << 8) / z) + 0x32L;
		r = (n->radius << 8) / z;
		if (n->kind == 1L) {
			if (r < 0x7fL)
				Gfx_FlareTurbulent(f, sx, sy, r, g_RampBeamHead);
			continue;
		}
		/* ORIGIN 0x002c89cc - zeroed while POSITIVE, the same idiom
		 * the glow's gate uses. */
		d = z - 0xbb8L;
		if (d > 0) d = 0;
		light = 0x3fL + (d >> 4);
		if (light < 0) light = 0;
		light <<= 8;
		if (n->kind == 0L) {
			s32 px0, px1, py0, py1, px, py;

			if (sx < 0 || sx >= SPACE_W || sy < 0 || sy >= 99L)
				continue;
			px0 = FX_MUL(sx, f->scale);
			px1 = FX_MUL(sx + 1L, f->scale);
			py0 = FX_MUL(sy, f->scale);
			py1 = FX_MUL(sy + 1L, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *pp = q + (size_t)px * PX_BYTES;
					u32 v = n->colour | (u32)light;

					if (px < 0) continue;
					pp[0] = (u8)(v >> 24); pp[1] = (u8)(v >> 16);
					pp[2] = (u8)(v >> 8);  pp[3] = (u8)v;
				}
			}
			continue;
		}
		/* ORIGIN 0x002c8a0a - the fourth kind is UNREACHABLE: that
		 * test repeats #3, which the branch above already took, so
		 * the nodes the table gives kind 4 never draw. */
		if (n->kind == 2L)
			de_blit_sprite(f, 0L, n, sx, sy, (u32)light);
		else if (n->kind == 3L)
			de_blit_sprite(f, 4L, n, sx, sy, (u32)light);
	}
}

/* ORIGIN 0x002c88c8 Destruct_DrawAftermath - a still out of the
 * plasma buffer under ONE light level, then the flare and the debris. */
static void de_aftermath(Relic_Frame *f)
{
	s32 x, y;

	if (!g_plasma) { Frame_Clear(f); return; }
	for (y = 0; y < SPACE_H; y++) {
		s32 py0 = FX_MUL(y, f->scale);
		s32 py1 = FX_MUL(y + 1L, f->scale);

		for (x = 0; x < SPACE_W; x++) {
			u32 v = RD_U32(g_plasma + DE_STILL_AT
			               + (size_t)(y * DE_STILL_ROW + x) * 4L);
			s32 px0 = FX_MUL(x, f->scale);
			s32 px1 = FX_MUL(x + 1L, f->scale);
			s32 px, py;

			v = (v & 0xffff00ffUL) | ((u32)(g_afterlight >> 2) << 8);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *pp = q + (size_t)px * PX_BYTES;

					if (px < 0) continue;
					pp[0] = (u8)(v >> 24); pp[1] = (u8)(v >> 16);
					pp[2] = (u8)(v >> 8);  pp[3] = (u8)v;
				}
			}
		}
	}
	Gfx_SetFlareRayPhase(g_spin);
	Gfx_Flare(f, DE_FLARE_CX, DE_FLARE_CY, DE_FLARE_R,
	          g_RayArcs, g_ColRampAmber);
	de_particles(f);
}

/* ORIGIN 0x002c8c52 Destruct_DrawGlow - the haze three times over at
 * three depths, the first two tracking the timer. */
static void de_glow(Relic_Frame *f)
{
	de_haze(f, g_timer + DE_GLOW_A);
	de_haze(f, g_timer + DE_GLOW_B);
	de_haze(f, DE_GLOW_C);
}

/* ORIGIN 0x002c887c Destruct_DrawFrame - five calls over a cleared
 * frame. [open] the aftermath phase and its 3000 particles are not
 * in - destruction.txt 4. */
static void de_frame(Relic_Frame *f, s32 tick)
{
	s32 bx, by;

	Gfx_SetLightBase(DE_LIGHT_R, DE_LIGHT_G, DE_LIGHT_B);
	if (!g_ready || tick <= DE_SETUP) { Frame_Clear(f); return; }
	if (g_phase != 0L) { de_aftermath(f); Space_BlurFrame(f); return; }
	if (g_white) { Space_FillWhite(f); return; }
	Frame_Clear(f);
	Space_SetCamera(0L, 0L);
	Space_SetPlanetWorld(0L, 0L);

	Gfx_SetFlareRayPhase(g_spin);
	Gfx_Flare(f, DE_FLARE_CX, DE_FLARE_CY, DE_FLARE_R,
	          g_RayArcs, g_ColRampAmber);
	if (Space_Project(0L, 0L, &bx, &by))
		Space_DrawPlanetAndHalo(f, bx, by);
	Space_BlitPortalSprite(f, DE_SPRITE_AT);
	de_glow(f);
	Space_BlurFrame(f);
}

/* OURS - key=value on the demo's own names. parts.h. */
static void de_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "phase=%ld timer=%ld gate=%ld zadv=%ld alight=%ld ready=%d",
	        (long)g_phase, (long)g_timer, (long)g_gate, (long)g_zadv,
	        (long)g_afterlight, g_ready);
}

/* OURS - parts.h. */
static s32 de_length(void)
{
	return Parts_GateLength(&g_music_gate, DE_TICKS);
}

const Relic_Part PART_Destruction = {
	"2.19", "The planet destroyed",
	"Space_RunDestruction 0x002c8438",
	PART_LIT, SPACE_W, SPACE_H,
	de_init, de_step, de_frame, de_stats,
	/* ORIGIN 0x002c8878 - phase one is 110 fields, the timer running
	 * from 200 to -0x385 at ten a field. The aftermath ends on the
	 * music, so its length is OURS. */
	DE_TICKS, 0, de_length
};
