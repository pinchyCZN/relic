/* RELIC part 2.12 - the ship blows up, seen side on from far away.
 * ANIM MODE 0xc, Space_RunPart's phase 10.
 *
 * The scene opens almost white - the flash of the previous part is
 * still filling the frame - and a glow CONTRACTS to reveal the blast
 * behind it. The blast is a train of four rings seen edge on, which is
 * why it reads like Star Wars or Star Trek rather than a fireball: the
 * explosion is a disc and the camera is in its plane.
 *
 * EVERYTHING HERE IS DRIVEN BY ONE COUNTER. Phase 10 zeroes
 * g_WhiteoutLevel and mode 0xc ramps it one a tick; the glow's radius,
 * each ring's size and the closing wash are all functions of it. There
 * is no per-effect clock at all.
 *
 * See docs/effects/explosion.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parts.h"
#include "gfx.h"
#include "dat.h"
#include "ship.h"
#include "space.h"



/* ORIGIN 0x002cc590 Space_RunPart, +0x3c8 - Ham8_BuildTables is handed
 * a pale blue-white here, and since every effect in this scene writes
 * only byte +2, THAT IS THE COLOUR OF THE WHOLE SCENE. */
#define EX_LIGHT_R   0x5fL
#define EX_LIGHT_G   0x5fL
#define EX_LIGHT_B   0x7fL

/* MEASURED - the fuse Space_RunPart +0x452 seeds does NOT predict it,
 * and 260 ends the scene on a haze. explosion.txt 1 and 4. */
#define EX_TICKS     420L

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x6ba - one a tick, from zero. */
#define EX_LEVEL_STEP 1L

/* ORIGIN 0x002ccc24 g_ExplosionSceneSkipTable. */
#define EX_SKIP      0x002ccc24UL

/* ORIGIN 0x002cc590 Space_RunPart, +0x416 - THE CAMERA IS 70000 UNITS
 * OUT AND DOES NOT MOVE. The setup writes the target (0, -0x11170,
 * 0xdac) and zeroes both angles, the latch copies target to camera,
 * and mode 0xc never touches any of them [verified; read back live as
 * 0, -70000, 3500 with yaw and pitch 0]. That distance is what makes
 * the ship a speck and the scene a side view from far away. */
#define EX_CAM_X     0L
#define EX_CAM_D (-70000L)
#define EX_CAM_Z     3500L

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x6c2 - THE SHIP SPINS. The object
 * angle gains 0x14 a tick, measured 396 to 596 over ten [observed], and
 * phase 10 does not reseed it - it carries 0x100 out of the pan, the
 * same angle that shot held, and starts turning from there. */
#define EX_OBJ_ANGLE0 0x100L
#define EX_OBJ_STEP   0x14L

/* ORIGIN 0x002d2350 Explosion_DrawFrame - the blast's centre. */
#define EX_CX        0x4eL
#define EX_CY        0x32L

/* ORIGIN 0x002d2350, +0xbc - THE CONTRACTING GLOW: radius 0xbb minus
 * the level, so it SHRINKS and reveals the scene. explosion.txt 2. */
#define EX_GLOW_FROM 0xbbL
#define EX_GLOW_CAP  0x7fL

/* ORIGIN 0x002d2350 - FOUR RINGS, each handed (K - level) * 2 as an
 * INVERSE radius: a staggered train. explosion.txt 3. */
static const s32 EX_RING_K[4] = { 0x12cL, 0x118L, 0x104L, 0xfaL };

/* ORIGIN 0x002d2350 Explosion_DrawFrame, +0x48 - the two texture
 * phases, turned opposite ways, which churns the ring rather than
 * sliding it. */
#define EX_PHASE_A_STEP   5L
#define EX_PHASE_B_STEP (-3L)

/* ORIGIN 0x002d2444 Explosion_DrawRing - the band's geometry, all of it
 * derived from D4 = 0xa0000 / D1. */
#define EX_RING_NUM    0xa0000L
#define EX_ROW_START (-0x190000L)
#define EX_ROW_STEP    0x640000L
#define EX_COL_STEP    0x500000L
#define EX_HALF_CAP    0x31L
#define EX_MID_ROW     49L

/* ORIGIN 0x002d2444, +0x40 - the texture is Fplasma ITSELF, slot 0 of
 * the second Dat_LoadSplit (its chunk name at 0x002e0cf3 is
 * "Fplasma"), read as big-endian longwords. The +0x20200 puts the
 * window on the chunk's midpoint, because the index is a SIGNED word
 * times four and so reaches 128K either side. The chunk name sits at
 * g_DatNameFplasma. */
#define EX_TEX_MID   0x20200L

/* ORIGIN 0x002ccaa4 - THE SCENE FADES THE PLASMA'S ROWS BEFORE IT
 * DRAWS ANYTHING, and that fade is the whole reason the bands end in
 * wisps rather than at an edge. Every longword of the 256 x 256
 * texture loses max(0, |row - 0x80| - 0x28) from R, G and B, floored
 * at zero, and the LIGHT byte is left alone. Fplasma's B peaks at
 * 0x24, so every row past 0x4c is black - and a band's own last row
 * is 89 to 95 rows out, well past that, so it adds nothing at all.
 * explosion.txt 3.
 *
 * The demo does it IN PLACE, but every Space_Run* that wants Fplasma
 * loads its own copy from the archive first, so the fade never
 * reaches another part. Ours fades a copy, which is the same thing.
 * The one other part that gets a fade is the destruction, with its
 * own 0x1e - explosion.txt 3. */
#define EX_FADE_FLAT  0x28L
#define EX_TEX_SIDE   256L

/* ORIGIN 0x002ceab4 Scene_ProjectPlanet - g_PlanetWorldX and
 * g_PlanetWorldZ against a FIXED depth, projected with the usual
 * shift-8-and-divide. Phase 10 zeroes both camera angles, so the
 * rotate ahead of it is the identity. explosion.txt 1. */
#define EX_PLANET_WX   0xc8L
#define EX_PLANET_WD   0xfa0L
#define EX_PLANET_WZ (-0x64L)

/* ORIGIN 0x002d2350 Explosion_DrawFrame, +0x58 - THE RAY BURST, whose
 * radius is CONSTANT despite looking driven: every g_FlarePulseOrb
 * entry exceeds the 0x3c cap. Draw_Flare adds all four channel bytes,
 * so it is the only thing here that is not pure LIGHT.
 * explosion.txt 1. */
#define EX_BURST_R   0x3cL

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x6cc - the ray phase turns 0xa a
 * tick here, and the sprite counter one. */
#define EX_RAY_STEP   0xaL

/* ORIGIN 0x002cdb9a Flash_WashFullScreen - uniform, and nothing below
 * 0xe1, so it is the fade OUT. explosion.txt 4. */
#define EX_WASH_FROM 0xe1L

static s32 g_level;
static s32 g_phase_a, g_phase_b;
static s32 g_ray_phase;
static s32 g_obj_angle;
static s32 g_cam[SHIP_CAM];
static const u8 *g_tex;
static u32 g_tex_len;
static u8 *g_tex_faded;
static int g_ready;

/* ORIGIN 0x002d2444, +0x84 - a SIGNED word index of longwords about
 * the chunk's midpoint. The demo lets it run off Fplasma's end; this
 * clamps, a deliberate divergence. explosion.txt 3. */
static u32 ring_tex(s32 idx)
{
	s32 off = EX_TEX_MID + (s32)(s16)(u16)(idx & 0xffffL) * 4L;

	if (off < 0 || (u32)off + 4 > g_tex_len) return 0;
	return RD_U32(g_tex_faded + (u32)off);
}

/* ORIGIN 0x002ccaa4 - Space_RunPart runs this over Fplasma on its way
 * into the phase, right after Ham8_BuildTables. */
static int ex_fade_texture(void)
{
	s32 row, col;

	if (g_tex_len < (u32)(EX_TEX_SIDE * EX_TEX_SIDE * 4L)) return 1;
	if (!g_tex_faded) {
		g_tex_faded = (u8 *)malloc((size_t)g_tex_len);
		if (!g_tex_faded) return 1;
	}
	memcpy(g_tex_faded, g_tex, (size_t)g_tex_len);
	for (row = 0; row < EX_TEX_SIDE; row++) {
		s32 d = row - EX_TEX_SIDE / 2L;
		u8 *p = g_tex_faded + (u32)(row * EX_TEX_SIDE) * PX_BYTES;

		if (d < 0) d = -d;
		d -= EX_FADE_FLAT;
		if (d <= 0) continue;
		for (col = 0; col < EX_TEX_SIDE; col++, p += PX_BYTES) {
			p[PX_R] = (u8)(p[PX_R] > d ? p[PX_R] - d : 0);
			p[PX_G] = (u8)(p[PX_G] > d ? p[PX_G] - d : 0);
			p[PX_B] = (u8)(p[PX_B] > d ? p[PX_B] - d : 0);
		}
	}
	return 0;
}

/* ORIGIN 0x002d2444 Explosion_DrawRing - a full-width band centred on
 * the middle row, growing both ways. D1 IS INVERSE RADIUS, so the band
 * grows as the level rises.
 *
 * RESOLUTION-PARAMETRIC, AND SCALE 1.0 CANNOT CATCH IT: raw rows put
 * the band at row 49 of the HOST's height, not the demo's, and at 1.0
 * the two are the same number - it measures perfect and looks wrong.
 * The geometry scales; the column accumulator stays on the demo's own
 * 160 columns. explosion.txt 5. */
static void ring_draw(Relic_Frame *f, s32 d1)
{
	s32 d4, half, py, ptop, pbot;
	u32 row0, rowstep, colstep, col;
	u8 lowbyte[DEMO_W];
	s32 dx;

	if (d1 <= 0) return;
	d4 = EX_RING_NUM / d1;
	if (d4 <= 0) return;
	row0 = (u32)(EX_ROW_START / d4);
	rowstep = (u32)(EX_ROW_STEP / d4);
	/* ORIGIN 0x002d2476 - ror.l #8 on the column step. */
	colstep = (u32)(EX_COL_STEP / d4);
	colstep = ((colstep >> 8) | (colstep << 24));
	half = d4 >> 8;
	if (half > EX_HALF_CAP) half = EX_HALF_CAP;
	if (half < 1) return;

	/* The accumulator over the DEMO's 160 columns, carry and all. */
	col = row0;
	for (dx = 0; dx < DEMO_W; dx++) {
		u32 was = col;

		lowbyte[dx] = (u8)(col & 0xffUL);
		/* ORIGIN 0x002d24be - add.l then addx.b, so the carry out of
		 * the long add folds into the low byte, not away. */
		col += colstep;
		if (col < was)
			col = (col & ~0xffUL) | ((col + 1) & 0xffUL);
	}

	ptop = FX_MUL(EX_MID_ROW - half + 1, f->scale);
	pbot = FX_MUL(EX_MID_ROW + half, f->scale);
	if (ptop < 0) ptop = 0;
	if (pbot > f->h - 1) pbot = f->h - 1;

	for (py = ptop; py <= pbot; py++) {
		/* THE DEMO ROW IN 8.8, NOT ROUNDED. Rounding it makes every
		 * demo row a block of `scale` identical frame rows, so the
		 * band comes out a staircase with a hard edge instead of a
		 * gradient - relic.h's note on resampling a table domain
		 * rather than stepping it. Exact at 1.0, where the fraction
		 * is zero. */
		s32 r256 = (s32)(((u32)py << 16) / ((u32)f->scale >> 8));
		s32 drow = r256 >> 8;
		s32 i256;
		s32 i;
		u32 acc;
		u8 *p;

		if (drow <= EX_MID_ROW) {
			i256 = (EX_MID_ROW << 8) - r256;
			/* SEAM: a mid row indexes negative. explosion.txt 5. */
			if (i256 < 0) i256 = 0;
			acc = (u32)(0 - (s32)((rowstep * (u32)i256) >> 8));
		} else {
			i256 = r256 - ((EX_MID_ROW + 1) << 8);
			acc = (rowstep * (u32)i256) >> 8;
		}
		i = i256 >> 8;
		if (i >= half) continue;
		p = f->px + ((u32)(py * f->w)) * PX_BYTES;
		for (dx = 0; dx < f->w; dx++, p += PX_BYTES) {
			s32 dcol = (s32)(((u32)dx << 16) / (u32)f->scale);
			u32 idx, v;

			if (dcol >= DEMO_W) dcol = DEMO_W - 1;
			idx = (acc & 0xff00UL) | (u32)lowbyte[dcol];
			v = ring_tex((s32)((idx + (u32)g_phase_b) & 0xffffUL)) >> 8;
			v += ring_tex((s32)((idx + (u32)g_phase_b
			                         + (u32)g_phase_a) & 0xffffUL));
			p[PX_LIGHT] = (u8)(p[PX_LIGHT] + (u8)v);
		}
	}
}

/* ORIGIN 0x002cdb9a Flash_WashFullScreen - uniform, whole frame. */
static void wash_draw(Relic_Frame *f)
{
	s32 amt = (g_level - EX_WASH_FROM);
	s32 n, i;
	u8 *p;

	if (amt < 0) return;
	amt >>= 1;
	if (amt > CHAN_MAX) amt = CHAN_MAX;
	n = f->w * f->h;
	p = f->px;
	for (i = 0; i < n; i++, p += PX_BYTES) {
		s32 v = p[PX_LIGHT] + amt;
		p[PX_LIGHT] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
	}
}

/* OURS */
static int ex_init(void)
{
	g_level = 0;
	g_phase_a = 0;
	g_phase_b = 0;
	g_ray_phase = 0;
	g_obj_angle = EX_OBJ_ANGLE0;
	g_cam[0] = EX_CAM_X;
	g_cam[1] = EX_CAM_D;
	g_cam[2] = EX_CAM_Z;
	g_cam[3] = 0;
	g_cam[4] = 0;
	g_ready = 0;
	if (Space_Init()) return 1;
	Ship_Init();
	/* ORIGIN 0x002cc9d8 Scene_ApplySkipTable - the phase's own table,
	 * which draws slots 0-7, 9-14 and 38 only. Emptying the extras is
	 * OURS: the demo's arcs, flares and orb are slots that table
	 * switches off, and nothing gates our extras. explosion.txt 6. */
	Ship_SetSkipTable(EX_SKIP);
	Ship_SetExtras(0, 0);
	g_tex = Dat_Chunk("Fplasma", &g_tex_len);
	if (!g_tex || ex_fade_texture()) return 1;
	Gfx_SetLightBase(EX_LIGHT_R, EX_LIGHT_G, EX_LIGHT_B);
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x6ba - mode 0xc. */
static void ex_step(s32 tick)
{
	(void)tick;
	if (!g_ready) return;
	g_level += EX_LEVEL_STEP;
	g_ray_phase += EX_RAY_STEP;
	g_obj_angle += EX_OBJ_STEP;
	g_phase_a += EX_PHASE_A_STEP;
	g_phase_b += EX_PHASE_B_STEP;
}

/* ORIGIN 0x002cd96e Scene_RenderExplosion - the phase 10 render.
 * * effects/explosion.txt 1. */
static void ex_frame(Relic_Frame *f, s32 tick)
{
	int k;

	(void)tick;
	Frame_Clear(f);
	if (!g_ready) return;
	/* Scene_RenderExplosion's order: sky, planet, objects, then the
	 * explosion and the wash. There are no scene objects in this
	 * phase that this part knows of. */
	Space_DrawSky(f, 0L, 0L);
	Space_DrawPlanetAndHalo(f,
	    ((EX_PLANET_WX << 8) / EX_PLANET_WD) + 0x50L,
	    ((EX_PLANET_WZ << 8) / EX_PLANET_WD) + 0x32L);
	/* ORIGIN 0x002cd9d0 - Scene_SortAndDrawAll. The wreck is drawn
	 * 70000 units out and spinning; ex_init's skip table is what
	 * decides the phase shows only it. */
	Ship_Draw(f, g_obj_angle, g_cam);
	for (k = 0; k < 4; k++)
		ring_draw(f, (EX_RING_K[k] - g_level) * 2L);
	Gfx_SetFlareRayPhase(g_ray_phase);
	Gfx_Flare(f, EX_CX, EX_CY, EX_BURST_R, g_RayArcs, g_ColRampMagenta);
	if (g_level < EX_GLOW_FROM) {
		s32 r = EX_GLOW_FROM - g_level;
		Gfx_GlowBlob(f, EX_CX, EX_CY,
		             r > EX_GLOW_CAP ? EX_GLOW_CAP : r, g_RampBolt);
	}
	wash_draw(f);
}

/* OURS */
static void ex_stats(char *out, int cap)
{
	s32 glow = EX_GLOW_FROM - g_level;

	(void)cap;
	if (glow < 0) glow = 0;
	if (glow > EX_GLOW_CAP) glow = EX_GLOW_CAP;
	sprintf(out, "level %ld/%ld | glow r%ld | rings %ld,%ld,%ld,%ld"
	             " | phase %ld,%ld | wash %ld",
	        (long)g_level, (long)EX_TICKS, (long)glow,
	        (long)((EX_RING_K[0] - g_level) * 2L),
	        (long)((EX_RING_K[1] - g_level) * 2L),
	        (long)((EX_RING_K[2] - g_level) * 2L),
	        (long)((EX_RING_K[3] - g_level) * 2L),
	        (long)g_phase_a, (long)g_phase_b,
	        (long)(g_level > EX_WASH_FROM ? (g_level - EX_WASH_FROM) >> 1 : 0));
}

const Relic_Part PART_Explosion = {
	"2.12", "The ship blows up, side on",
	"Space_RunPart 0x002cc9b0 phase 10, anim mode 0xc",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	ex_init, ex_step, ex_frame, ex_stats,
	EX_TICKS
};
