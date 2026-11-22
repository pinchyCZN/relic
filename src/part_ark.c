/* RELIC part 2.20 - the ark's departure. TempleModule_Run 0x003678d0
 * mode 1, the last thing act 2 runs.
 *
 * THE SCENE IS THREE COUNTERS AND NOTHING WAITS ON THE MUSIC, unlike
 * the city's third shot or the destruction's aftermath. It opens
 * fading in from white, flies the ark away over the warped ground
 * into a light, and whites out again; one counter does both fades.
 *
 * See docs/effects/ark.txt. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relic.h"
#include "image.h"
#include "gfx.h"
#include "dat.h"
#include "objset.h"
#include "parts.h"

/* ORIGIN 0x003678d0 TempleModule_Run - what mode 1 stages. */
#define AK_TICKS        0x47eL          /* 0x00372b36, the length    */
#define AK_FADE0        0x200L          /* 0x003726d6                */
#define AK_FADE_DOWN    4L              /* a field, to zero          */
#define AK_FADE_UP      1L              /* a field, once Z is past   */
#define AK_CAM_Z0       (-0xfa0L)       /* 0x00372b88, 0xfffff060    */
#define AK_CAM_DZ       (-0x8cL)        /* a field                   */
#define AK_WHITE_Z      (-64999L)       /* where the white out starts */
#define AK_OBJ_YAW      0xf40L          /* 0x00372b74, held          */
#define AK_CAM_X        (-0xfa0L)       /* 0x00372b84                */
#define AK_CAM_Y        (-0xbb8L)       /* 0x00372b8c                */

/* ORIGIN 0x0037265a - the per-field steps. */
#define AK_SPIN_D       8L              /* g_TempleBlobSpin          */
#define AK_WARP_D       10L             /* g_TempleWarpAngle         */
#define AK_HAZE_A_D     0x201L          /* 0x00369f76                */
#define AK_HAZE_B_D     (-0x100L)       /* 0x00369f7a                */

/* ORIGIN 0x0036a106 and 0x0036a134 - the two ramps, both the same
 * inverse-square curve Ramp_BuildColour uses. */
#define AK_LIT_R        0x3fL
#define AK_LIT_G        0x2fL
#define AK_LIT_B        0x20L
#define AK_LIT_NUM      0xc80000L
#define AK_PUFF_NUM     0x1000000L
#define AK_PUFF_SCALE   0x12cL

/* ORIGIN 0x0036a0a2 - the scene loads Fplasma and greyscales it, and
 * builds one polar pair a texel off g_AtanTable. ark.txt 5. */
#define AK_W            160L
#define AK_H            100L
#define AK_PLASMA_LEN   0x40000L
#define AK_HAZE_WAVE    0x0037c900UL

/* ORIGIN 0x0036822e Temple_RunMode1 - the ground's height. */
#define AK_GROUND_ALT   0x12c0L

/* THE SPIN AND THE GROUND ANGLE ARE HANDED OVER, NOT RESET. Mode 1's
 * entry stages a dozen globals and these two are not among them:
 * 0x00372bbc and 0x00372b16 are stepped by the ruins' VBL hook
 * 0x00372524 as well as by this scene's, so both arrive carrying the
 * whole module's history. The two sky-haze angles beside them ARE at
 * zero, because only this scene's hook touches those.
 *
 * The figures are hardware's, wound back the 151 fields of the
 * +8 and +0xa steps [observed]. ark.txt 6. */
#define AK_SPIN_ENTRY   680L
#define AK_WARP_ENTRY   (-3655L)

/* ORIGIN 0x00368378 and 0x003683b4 - the two blobs. The puff is
 * nailed to the frame's centre; the light's centre comes off an
 * object. */
#define AK_PUFF_CX      0x50L
#define AK_PUFF_CY      0x32L
#define AK_PUFF_R       0x46L
#define AK_LIT_R_PX     0x7fL
#define AK_LIT_OBJ      9L
#define AK_LIT_DX       0x12L
#define AK_LIT_DY       8L

/* ORIGIN 0x0036838e - the ark is objects 0 to 0xc of the fourteen
 * Temple_BuildObjects makes, which is the temple's own set, less the
 * three its own skip table at 0x0036a152 hides: 4, 5 and 10. */
#define AK_OBJ_FIRST    0L
#define AK_OBJ_LAST     0xcL
#define AK_OBJ_MASK     0x0036a152UL

static s32 g_ticks;                 /* 0x00372b36                   */
static s32 g_fade;                  /* 0x003726d6                   */
static s32 g_cam_z;                 /* 0x00372b88                   */
static s32 g_spin;                  /* g_TempleBlobSpin             */
static s32 g_warp;                  /* g_TempleWarpAngle            */
static s32 g_haze_a, g_haze_b;      /* 0x00369f76, 0x00369f7a       */
static int g_ready;

static s32 *g_pair;                 /* (0x00374704), two a texel    */
static u8  *g_haze_tex;             /* (0x003748f8), Fplasma greyed */
/* 0x00373910, padded: the forty longwords either side of the
 * demo's ramp are zero [verified]. gfx.h RAMP_PAD. */
static u8  g_lit_buf[(2 * RAMP_PAD + 256) * PX_BYTES];
static u8 *g_lit_ramp = g_lit_buf + RAMP_PAD * PX_BYTES;
static s16 g_rays[256];             /* (0x00374720)                 */
static u32 g_rng;                   /* 0x00372b0e                   */
static u8  g_puff_ramp[256];        /* 0x00374250                   */

/* ORIGIN 0x0036cffe Temple_BuildPuffRamp - the colour ramp's curve
 * with a scale on the index and one byte out instead of three. */
static void ak_build_puff_ramp(u8 *out, s32 numer, s32 scale)
{
	s32 i;

	for (i = 0; i < 256L; i++) {
		s32 u = (scale * i) >> 7;
		s32 t = u * u;
		s32 v = t ? (numer - t) / t : numer;

		v >>= 4;
		out[i] = (u8)(v > LIGHT_MAX ? LIGHT_MAX : (v < 0 ? 0 : v));
	}
}

/* ORIGIN 0x0036b880 Temple_GreyHazeTexture - the scene's one texture
 * is FPLASMA, greyscaled in place: each longword's B byte doubled
 * plus eight, splatted over R, G and B with LIGHT left at zero. The
 * ground writes the whole longword and the sky haze reads back just
 * the one byte. ark.txt 5. */
static int ak_build_haze_tex(void)
{
	const u8 *src;
	u32 len;
	s32 i, n = AK_PLASMA_LEN / 4L;

	src = Dat_Chunk("Fplasma", &len);
	if (!src || len < (u32)AK_PLASMA_LEN) return 1;
	free(g_haze_tex);
	g_haze_tex = (u8 *)malloc((size_t)n * PX_BYTES);
	if (!g_haze_tex) return 1;
	for (i = 0; i < n; i++) {
		u8 v = (u8)(src[i * 4L + 3L] * 2L + 8L);
		u8 *p = g_haze_tex + (size_t)i * PX_BYTES;

		p[PX_R] = v;
		p[PX_G] = v;
		p[PX_B] = v;
		p[PX_LIGHT] = 0;
	}
	return 0;
}

/* ORIGIN 0x00369fd0 and 0x0036a038 - one pair of longwords a texel,
 * both off the SAME centred polar field: the angle, and a reciprocal
 * of the distance in the high byte. The second doubles the angle and
 * divides a numerator 0x32 times larger, which is what makes the two
 * layers turn and flow at different rates. ark.txt 5. */
static int ak_build_haze_pairs(void)
{
	s32 row, col;

	free(g_pair);
	g_pair = (s32 *)malloc((size_t)(AK_W * AK_H) * 2L * sizeof(s32));
	if (!g_pair) return 1;
	for (row = 0; row < AK_H; row++)
		for (col = 0; col < AK_W; col++) {
			s32 dx = col - 0x50L, dy = row - 0x32L;
			s32 ang = Gfx_PolarAngle(dx, dy);
			s32 d = Gfx_PolarDist(dx, dy);
			s32 r1 = d ? 0x10000L / d : 0x10000L;
			s32 r2 = d ? 0x320000L / d : 0x320000L;
			s32 *p = g_pair + (size_t)(row * AK_W + col) * 2L;

			p[0] = ang + (((r1 >> 8) & 0xffL) << 8);
			p[1] = ang * 2L + (((r2 >> 8) & 0xffL) << 8);
		}
	return 0;
}

/* ORIGIN 0x00369f7e Temple_DrawSkyHaze - the light byte of every
 * texel from two scrolling layers, REPLACED rather than added, so it
 * is the floor the blobs and the fade then build on. ark.txt 5. */
static void ak_sky(Relic_Frame *f)
{
	const u8 *wave = Image_At(AK_HAZE_WAVE);
	s32 row, col;

	if (!wave || !g_pair || !g_haze_tex) return;
	for (row = 0; row < AK_H; row++) {
		s32 py0 = FX_MUL(row, f->scale);
		s32 py1 = FX_MUL(row + 1L, f->scale);

		for (col = 0; col < AK_W; col++) {
			const s32 *p = g_pair + (size_t)(row * AK_W + col) * 2L;
			s32 ia = (s32)(s16)(u16)(u32)(g_haze_a + p[0]);
			s32 ib = (s32)(s16)(u16)(u32)(g_haze_b + p[1]);
			s32 px0 = FX_MUL(col, f->scale);
			s32 px1 = FX_MUL(col + 1L, f->scale);
			s32 px, py;
			u8 v = (u8)(wave[ib] + g_haze_tex[(0x8000L + ia) * PX_BYTES + PX_B]);

			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					if (px < 0) continue;
					q[(size_t)px * PX_BYTES + PX_LIGHT] = v;
				}
			}
		}
	}
}

/* ORIGIN 0x003680e8 Temple_Rand - the module's own LCG. 0x003679be
 * CLEARS ITS SEED and nothing else in the demo draws from it, so the
 * sequence below is the demo's own and the ray table is exact. */
static s32 ak_rand(s32 range)
{
	u32 h;

	g_rng = g_rng * 0x5d43UL + 0x5c2dUL;
	h = ((g_rng >> 16) ^ g_rng) & 0xffffUL;
	return (s32)((((u32)range & 0xffffUL) * h) >> 16);
}

/* ORIGIN 0x00368118 Temple_SeedNoiseTables - two 256-word tables off
 * that LCG. THE FIRST IS BUILT ONLY FOR ITS DRAWS HERE: it feeds the
 * glow of a type-4 object, and the ark's range draws none, but its
 * forty calls set where the second one starts. ark.txt 4.1.
 *
 * The second is twenty arcs of a constant random height over a
 * random run of angles, smoothed against the next entry and then
 * NEGATED - the same generator as Stills_BuildRays, and negated for
 * the same reason: Gfx_Flare subtracts it, so an arc pushes its
 * sample DOWN the ramp. Built positive the light comes out a flat
 * bright disc. */
static void ak_seed_noise(void)
{
	s16 spike[256];
	s32 i, k;

	for (i = 0; i < 256L; i++) { spike[i] = 0; g_rays[i] = 0; }
	for (k = 0; k < 20L; k++) {
		s32 at = ak_rand(0x100L);
		s32 v  = ak_rand(8L);

		spike[at] = (s16)v;
		at = (at + 1L) & 0xffL;
		if ((v -= 3L) < 0) continue;
		spike[at] = (s16)v;
		at = (at - 2L) & 0xffL;
		spike[at] = (s16)v;
		if ((v -= 2L) < 0) continue;
		at = (at - 1L) & 0xffL;
		spike[at] = (s16)v;
		at = (at + 4L) & 0xffL;
		spike[at] = (s16)v;
	}
	for (k = 0; k < 20L; k++) {
		s32 at = ak_rand(0x100L);
		s32 h  = ak_rand(0xdL);
		s32 n  = ak_rand(0x10L) + 0x14L;

		for (i = 0; i <= n; i++, at = (at + 1L) & 0xffL)
			g_rays[at] = (s16)h;
	}
	for (i = 0; i < 256L; i++) {
		s32 a = g_rays[i] + g_rays[(i + 1L) & 0xffL];

		a = (a & ~0xffL) | ((a & 0xffL) >> 1);
		g_rays[i] = (s16)(-a);
	}
	(void)spike;
}

/* OURS */
static int ak_init(void)
{
	if (Gfx_Init()) return 1;
	if (Temple_UseObjectSet(Temple_DefaultObjectSet())) return 1;
	/* ORIGIN 0x0036a10c - 0x3f / 0x2f / 0x20 over 0xc80000. */
	Gfx_BuildColourRamp(g_lit_ramp, AK_LIT_R, AK_LIT_G, AK_LIT_B,
	                    AK_LIT_NUM);
	ak_build_puff_ramp(g_puff_ramp, AK_PUFF_NUM, AK_PUFF_SCALE);
	ak_seed_noise();
	if (ak_build_haze_tex() || ak_build_haze_pairs()) return 1;

	g_ticks = AK_TICKS;
	g_fade = AK_FADE0;
	g_cam_z = AK_CAM_Z0;
	g_spin = AK_SPIN_ENTRY;
	g_warp = AK_WARP_ENTRY;
	g_haze_a = 0;
	g_haze_b = 0;
	Temple_BlurFrameReset();
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x0037265a Temple_StepMode1Camera - the whole of the scene's
 * motion, once a FIELD. ark.txt 3. */
static void ak_step(s32 tick)
{
	if (tick <= 1 || !g_ready) return;
	g_spin += AK_SPIN_D;
	g_warp += AK_WARP_D;
	g_haze_a += AK_HAZE_A_D;
	g_haze_b += AK_HAZE_B_D;
	g_cam_z += AK_CAM_DZ;
	/* ORIGIN 0x0037268a - ONE COUNTER, BOTH FADES: it falls to zero
	 * while the camera is still near, and climbs once the camera
	 * passes AK_WHITE_Z. */
	if (g_cam_z < AK_WHITE_Z) {
		g_fade += AK_FADE_UP;
	} else if (g_fade > 0) {
		g_fade -= AK_FADE_DOWN;
		if (g_fade < 0) g_fade = 0;
	}
	if (g_ticks > 0) g_ticks--;
}

/* ORIGIN 0x003683d8 Temple_Mode1Fade - min(0x3f, level >> 3) onto
 * every pixel's LIGHT, saturating through g_LightSat. */
static void ak_fade(Relic_Frame *f)
{
	s32 n = f->w * f->h, i, add;

	if (g_fade == 0) return;
	add = g_fade >> 3;
	if (add > LIGHT_MAX) add = LIGHT_MAX;
	for (i = 0; i < n; i++) {
		u8 *p = f->px + (size_t)i * PX_BYTES;

		p[PX_LIGHT] = g_LightSat[(p[PX_LIGHT] + add) & 0xffL];
	}
}

/* ORIGIN 0x003682f8 Temple_StepMode1 - the scene's whole frame, in
 * the demo's order. THE SKY REPLACES THE LIGHT BYTE, so it is the
 * floor the two blobs and the fade then build on and anything
 * written to LIGHT before it is lost. ark.txt 2. */
static void ak_frame(Relic_Frame *f, s32 tick)
{
	(void)tick;
	Gfx_SetLightBase(0x3fL, 0x4fL, 0x6fL);
	if (!g_ready) { Frame_Clear(f); return; }

	/* ORIGIN 0x0036834e - the ground IS the backdrop: it writes
	 * every pixel, so nothing clears the frame first. */
	Temple_WarpTerrainTo(f, g_haze_tex, g_warp, AK_GROUND_ALT, 0L);
	ak_sky(f);

	/* ORIGIN 0x00368378 - the puff, fixed at the frame's centre on a
	 * CONSTANT radius: 0x00368356 divides a perspective one out of
	 * 0x186a0 and throws it away two instructions later. */
	Gfx_GlowBlobTo(f->px, f->w, f->h, f->scale,
	               AK_PUFF_CX, AK_PUFF_CY, AK_PUFF_R, g_puff_ramp);

	/* ORIGIN 0x0036839e - the camera the hook staged, as the five
	 * the object calls take: x, z, y, pan, tilt. */
	{
		s32 cam[5], lx, ly;

		cam[0] = AK_CAM_X;
		cam[1] = g_cam_z;
		cam[2] = AK_CAM_Y;
		cam[3] = 0L;
		cam[4] = 0L;

		/* ORIGIN 0x003683b4 - the light the ark flies into. It is
		 * NAILED TO THE SHIP: its centre is object 9's first point,
		 * projected, less 0x12 across and plus 8 down. The demo
		 * reads it out of that object's own point buffer, which the
		 * previous frame's draw filled. */
		if (Temple_ProjectObjectVertex(AK_OBJ_YAW, cam, AK_LIT_OBJ,
		                               0L, &lx, &ly)) {
			Gfx_SetFlareRayPhase(g_spin & 0xffL);
			Gfx_Flare(f, lx - AK_LIT_DX, ly + AK_LIT_DY,
			          AK_LIT_R_PX, g_rays, g_lit_ramp);
		}
		Temple_DrawObjectRange(f, AK_OBJ_YAW, cam,
		                       AK_OBJ_FIRST, AK_OBJ_LAST, AK_OBJ_MASK);
	}
	ak_fade(f);
	Temple_BlurFrameTo(f);
}

/* OURS - key=value on the demo's own names. parts.h. */
static void ak_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "ticks=%ld fade=%ld camz=%ld spin=%ld warp=%ld ready=%d",
	        (long)g_ticks, (long)g_fade, (long)g_cam_z, (long)g_spin,
	        (long)g_warp, g_ready);
}

const Relic_Part PART_Ark = {
	"2.20", "The ark's departure",
	"TempleModule_Run 0x003678d0 mode 1",
	PART_LIT, 160, 100,
	ak_init, ak_step, ak_frame, ak_stats,
	AK_TICKS
};
