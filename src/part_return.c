/* RELIC - part 2.15, the return flight's departure shot.
 *
 * The ship leaving the planet, with the planet edge behind it. It is
 * the space engine's work and it runs BETWEEN the temple module's two
 * calls - SpaceModule_Run with D0 = 1.
 *
 * THE BACKDROP IS NOT GEOMETRY. The planet edge is a 640 by 100
 * panorama built once from the generated sky and the Fearth overlay,
 * and the frame is a 160-wide window into it. Nothing about the
 * horizon is projected, and the window's offset never changes during
 * the shot - the camera moves, so the SHIP moves across a fixed
 * backdrop.
 *
 * See docs/effects/return.txt.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "orb.h"
#include "parts.h"
#include "gfx.h"
#include "space.h"
#include "ship.h"
#include "orbscene.h"
#include "dat.h"
#include "image.h"
#include "objset.h"
#include "host.h"

/* ORIGIN 0x002d0146 Scene_SetupPart14 - the thirteen meshes it
 * writes into the scene slots, as (face list, count word, vertex
 * block). THE BYTES ARE THE TEMPLE'S: every one of these is
 * identical to part_temple.c's first thirteen, in the other hunk.
 * return.txt 2.4. */
static const u32 RT_OBJ[13][3] = {
	{ 0x002e26e2UL, 0x002e2990UL, 0x002e2992UL },
	{ 0x002e2bbeUL, 0x002e2e6cUL, 0x002e2e6eUL },
	{ 0x002e309aUL, 0x002e3348UL, 0x002e334aUL },
	{ 0x002e3576UL, 0x002e3824UL, 0x002e3826UL },
	{ 0x002e3a52UL, 0x002e3b32UL, 0x002e3b34UL },
	{ 0x002e3be0UL, 0x002e3cc0UL, 0x002e3cc2UL },
	{ 0x002e3d6eUL, 0x002e3da6UL, 0x002e3da8UL },
	{ 0x002e3df4UL, 0x002e3e56UL, 0x002e3e58UL },
	{ 0x002e3ec8UL, 0x002e3f0eUL, 0x002e3f10UL },
	{ 0x002e3f68UL, 0x002e4080UL, 0x002e4082UL },
	{ 0x002e4116UL, 0x002e41beUL, 0x002e41c0UL },
	{ 0x002e4224UL, 0x002e4374UL, 0x002e4376UL },
	{ 0x002e445eUL, 0x002e45aeUL, 0x002e45b0UL }
};

/* ORIGIN 0x002d0146 - the Uv_GenerateFaceParams calls, in order.
 * Objects 0 to 9 are the temple's passes unchanged; 10, 11 and 12
 * are this scene's own. */
static const Te_UvPass RT_UV[] = {
	{  0, TE_UV_C1C2, 0x0023797aUL, 256, 0x2b, 0x2b, -0x302, -0x168, 0, 0, 0 },
	{  1, TE_UV_C1C2, 0x0023797aUL, 256, 0x2b, 0x2b, -0x302, -0x168, 0, 0, 0 },
	{  2, TE_UV_C1C2, 0x0023797aUL, 256, 0x2b, 0x2b, -0x302, -0x168, 0, 0, 0 },
	{  3, TE_UV_C1C2, 0x0023797aUL, 256, 0x2b, 0x2b, -0x302, -0x168, 0, 0, 0 },
	{  4, TE_UV_C0C2, 0x0023797aUL, 256, 0x2b, 0x1e, -0x2ee, -0x190, 0, 0, 0 },
	{  4, TE_UV_C1C2, 0x0023797aUL, 256, 0x2b, 0x1e, -0x2ee, -0x190, 0, 0, 0 },
	{  5, TE_UV_C1C2, 0x0023797aUL, 256, 0x2b, 0x2b, -0x302, -0x168, 0, 0, 0 },
	{  6, TE_UV_C1C2, 0x0023797aUL, 256, 0x2b, 0x2b, -0x302, -0x168, 0, 0, 0 },
	{  7, TE_UV_C1C0, 0x0023797aUL, 256, 0x2b, 0x32, -0x302, -0x0fa, 0, 0, 0 },
	{  8, TE_UV_C1C0, 0x0023797aUL, 256, 0x2b, 0x32, -0x302, -0x0fa, 0, 0, 0 },
	{  9, TE_UV_C1C2, 0x0024f97aUL, 128, 0x6e, 0x96, -0x134, -0x1f8, 0, 0, 0 },
	{  9, TE_UV_C0C2, 0x0024f97aUL, 128, 0x6e, 0x96, -0x148, -0x1f8, 0, 0, 0 },
	{  9, TE_UV_ROTRNG, 0x0024f97aUL, 128, 0, 0, 0x3c, 0x40,
	  0x46, -0x1d4c0, 0 },
	{  9, TE_UV_ROTRNG, 0x0024f97aUL, 128, 0, 0, 0xea, 0x0d,
	  0x6e, -0x3a980, -0x1e078 },
	/* 0x002d06be - A DIFFERENT TEXTURE FROM THE TEMPLE'S, which uses
	 * 0x0023a1ca here. The 0xb2 in D2 has no counterpart in the
	 * temple's call and what it selects is not read; the stride is
	 * taken as its neighbours' 128 [inferred]. */
	{ 10, TE_UV_FLAT, 0x0024fae2UL, 128, 0x4e, 0x4e,  0x040,  0x040, 0, 0, 0 },
	{ 11, TE_UV_C1C2, 0x0024fb7aUL, 128, 0x5a, 0xaa, -0x0be, -0x0b9, 0, 0, 0 },
	{ 11, TE_UV_CYLC1, 0x0023797aUL, 256, 0, 0, 0, 0,
	  0x100,  0,  0x17200 },
	{ 12, TE_UV_C1C2, 0x0024fb7aUL, 128, 0x5a, 0xaa, -0x0be, -0x0b9, 0, 0, 0 },
	{ 12, TE_UV_CYLC1, 0x0023797aUL, 256, 0, 0, 0, 0,
	  0x100, -0x17200,  0 }
};

/* ORIGIN 0x002d02a2, 0x002d0316, 0x002d0494, 0x002d050e - the four
 * slots that get a cull mode written to +0x4c. The rest keep zero,
 * and the set matches the temple's first thirteen exactly. */
static const s32 RT_CULL[13] = { 0, 0, 1, 1, 0, 0, 2, 1, 0, 0, 0, 0, 0 };

/* ORIGIN 0x002d0772, 0x002d0808 - return.txt 2.4.1 */
static const s32 RT_BIAS[13] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xb71b0L, -0xb71b0L
};

/* THE GEOMETRY IS HALVED, by CODE_03's own copy of Temple_HalveVerts
 * at 0x002cfefe - the same loop without the one-shot guard, called
 * once per object. Checked against the running demo: the live vertex
 * block holds exactly half what the file does [observed].
 *
 * THE TWO GLOWS ARE THE THRUSTERS, at x of -/+0xb400 with the same
 * 0x493e0 the temple carries off component 0 for the sort only.
 * Their radius table is the eight longwords at 0x002ee948, which the
 * slot's +0x30 points at, and they draw as teardrops rather than the
 * temple's colour blobs. return.txt 2.5. */
static const Relic_ObjSet RT_SET = {
	RT_OBJ, 13L, RT_UV, (s32)(sizeof RT_UV / sizeof RT_UV[0]),
	RT_CULL, RT_BIAS, 1L,
	0x002ee804UL, 0x002ee810UL, 0x002ee948UL, 1L
};

/* ORIGIN 0x002c8258 SpaceModule_Run, the D0 == 1 staging. */
#define RT_OBJ_ANGLE   (-0x350L)
#define RT_CAM_X         0x7724L
#define RT_CAM_Y       (-0x11170L)
#define RT_CAM_Z       (-0x1770L)
#define RT_YAW           0x230L
#define RT_PITCH         0x50L

/* ORIGIN 0x002c8e6a - Ham8_BuildTables' three, and the departure
 * shares the temple's base. The portal run rebuilds them at 0, 0, 0,
 * which is what turns a LIGHT-only pixel black there. */
#define RT_LIGHT_R       0x3fL
#define RT_LIGHT_G       0x3fL
#define RT_LIGHT_B       0x5fL

/* ORIGIN 0x002d5b86 - the VBL hook's mode 1 block, which is the whole
 * of the shot's motion: the camera crosses and closes, the sun's
 * texture turns, and the portal arms once the camera is deep enough.
 * The shot ends when the depth passes RT_CAM_END, so its length is
 * arithmetic rather than a cue. */
#define RT_ANG_STEP      1L
#define RT_CAM_DY        0xc8L
#define RT_CAM_DX        0x82L
#define RT_AIM_Z         0x3e8L
#define RT_PORTAL_AT     0x2af8L
#define RT_PORTAL_STEP   2L
#define RT_PORTAL_MAX    0xfeL
#define RT_CAM_END       0x88b8L
#define RT_FLIGHT        ((RT_CAM_END - RT_CAM_Y) / RT_CAM_DY)

/* ORIGIN 0x002c8ec4 - the second half. One white frame, the tables
 * rebuilt darker, the panorama baked for the ripple, then mode 2
 * until the range reaches RT_EXP_END. THE SHIP DOES NOT EXPLODE, IT
 * STOPS BEING DRAWN - nothing projects it again. return.txt 4.2. */
#define RT_EXP_LIGHT_R   0x1fL
#define RT_EXP_LIGHT_G   0L
#define RT_EXP_LIGHT_B   0x2fL
#define RT_RANGE0        0xdc0L
#define RT_RANGE_STEP    0xcL
#define RT_EXP_END       0x320L
#define RT_RIPPLE_STEP   0x1f4L
#define RT_SPIN_STEP     4L
/* The loop tests AFTER a step, so the last tick is the one that puts
 * the range at or under RT_EXP_END - 227, not 226. */
#define RT_EXP_TICKS     ((RT_RANGE0 - RT_EXP_END + RT_RANGE_STEP - 1L) \
                          / RT_RANGE_STEP)
/* THE PHASE'S COUNTERS DO NOT START WITH THE PHASE - seven fields
 * of setup come first, and the white holds for three. return.txt
 * 4.2. */
#define RT_RIPPLE_SETUP  7L
#define RT_FLASH_FIELDS  3L
#define RT_RIPPLE_START  (RT_FLIGHT + RT_RIPPLE_SETUP)

#define RT_BURST         (RT_RIPPLE_START + RT_EXP_TICKS)

/* ORIGIN 0x002c8f76 Space_RunPortalRun - the swirl, which is the
 * shot after the ripple. Its tables resolve LIGHT to BLACK where the
 * flight's resolve pale, so the portal's own glow comes out dark.
 * return.txt 2.3. */
#define RT_SW_LIGHT_R    0L
#define RT_SW_LIGHT_G    0L
#define RT_SW_LIGHT_B    0L
#define RT_SW_FUSE       0x258L
#define RT_SW_PLANET_X (-0x1f4L)
#define RT_SW_PLANET_Z   0x96L
#define RT_SW_OPEN_MAX   0x7fL
#define RT_SW_SPIN_STEP  2L
#define RT_SW_CX         0x64L
#define RT_SW_CY         0x28L
#define RT_SW_FLARE_CAP  0x46L
#define RT_SW_SPRITE_AT  0x7d50L
/* 0x002d5ae4 and 0x002d5b10 - the two warp offsets, one backwards
 * every four ticks and one forwards every eight. */
#define RT_SW_WOB_SPAN   512L
/* ORIGIN 0x002c905a - swirl and burst end on MUSIC CUES; the 0x258
 * and 0x1f4 fuses are backstops the cues beat. LANDMINE: 0x002e0a92
 * is SHARED, so a later reading measures the burst. return.txt 2.3. */
#define RT_SW_TICKS      362L
#define RT_SWIRL_END     (RT_BURST + RT_SW_TICKS)
#define RT_SW_CUE_ORDER  0x24L
#define RT_SW_CUE_ROW    0L

/* ORIGIN 0x002c90a6 - one Space_FillWhite frame, then the tables are
 * rebuilt WARM where the swirl had them at zero. */
#define RT_BU_LIGHT_R    0x5fL
#define RT_BU_LIGHT_G    0x4fL
#define RT_BU_LIGHT_B    0x3fL
/* ORIGIN 0x002c90d6, 0x002c901a, 0x002c9022 and mode 4 at 0x002d5b44 */
#define RT_BU_GROW0      0xa0L
#define RT_BU_FRONT0     0x35cL
#define RT_BU_DECAY0     0x20L
#define RT_BU_SPIN_STEP  0x2dL
#define RT_BU_ANG_STEP   2L
#define RT_BU_TICKS      442L
#define RT_BU_CUE_ORDER  0x24L
#define RT_BU_CUE_ROW    0x24L
#define RT_BU_CX         0x64L
#define RT_BU_CY         0x28L
#define RT_BAND_ROW      0x28L
/* The three baked maps - return.txt 2.3.3. */
#define RT_MASKBITS      0x002f9074UL
#define RT_FRONTMAP      0x0030b954UL
#define RT_RAMPSRC       0x0030f7d4UL
#define RT_MAP_W         160L
#define RT_MAP_H         98L
#define RT_MAP_CELLS     (RT_MAP_W * RT_MAP_H)
/* ORIGIN 0x002c9594 - the map is BUILT 160x100 and SWEPT 160x98. */
#define RT_MAP_BUILD     (RT_MAP_W * 100L)
/* ORIGIN 0x002c91b0 - 0xbb7 + 1 */
#define RT_PARTS         3000L
#define RT_PART_CX       0x640000L
#define RT_PART_CY       0x280000L

/* ORIGIN 0x002c826a - the module's entry waits on (0x1f,0x30) and
 * then builds, and the temple's blank is on screen through both.
 * return.txt 2. */
#define RT_SETUP         21L
#define RT_TICKS         (RT_SWIRL_END + RT_BU_TICKS + RT_SETUP)

static s32 g_swirl_end;         /* the tick each cue answered on */
static s32 g_depart_end;

/* OURS - the swirl hands over on its cue, so the phase after it
 * cannot be a fixed tick. */
static int rt_in_burst(s32 tick)
{
	return g_swirl_end != 0L && tick > g_swirl_end;
}

/* ORIGIN 0x002c9272 Space_StepDeparture. */
#define RT_SCROLL_BIAS   0xc8L
#define RT_SUN_X         0x70L
#define RT_SUN_Y       (-0x24L)
#define RT_SUN_R         0x7fL
#define RT_SUN_ANG_B     0x5aL
#define RT_FLARE_X       0x12cL
#define RT_FLARE_Y       0xaL
#define RT_FLARE_R       0x64L
#define RT_GLOW_X      (-0x3b6L)

/* ORIGIN 0x002d2f02 Space_BuildPanorama - 640 x 100 four-byte
 * pixels, the sky tiled across it at 256 and Fearth stamped over. */
#define RT_PAN_W         640L
#define RT_PAN_H         100L
#define RT_SKY_WRAP      0xffL
#define RT_STAMP_ROWS    0x5cL
#define RT_STAMP_COLS    0xf0L
#define RT_LIMB_AT       3L
#define RT_LIMB_ROWS     100L
#define RT_LIMB_COLS     250L
#define RT_LIMB_X0       0x140L
#define RT_LIMB_Y0     (-0x358L)
#define RT_LIMB_STEP     8L
#define RT_LIMB_R        0x41aL
#define RT_HALO_RAMP     0x002e13c8UL

/* ORIGIN 0x002c8eac - the scroll the first loop runs under, and the
 * point Space_ResetPanoramaToSky is called. return.txt 2.2. */
#define RT_RESKY_AT      0x12cL

/* ORIGIN 0x002d20fa Space_DrawSunBlob - the sphere tables orb.c
 * names, at the same biases. */
#define RT_NORMAL        0x0026f97aUL
#define RT_NORMAL_BIAS   0x10100L
#define RT_CHORDANG      0x0029797aUL
#define RT_TEX_BIAS      0x20200L

/* ORIGIN 0x002cdfe4 Space_DrawSunFlare. */
#define RT_SF_ONE_IN     8L
#define RT_SF_X          0x8cL
#define RT_SF_Y          0x3cL
#define RT_SF_JX         0x14L
#define RT_SF_JY         0xaL
#define RT_SF_R0         0x14L
#define RT_SF_RJ         5L

static u8 *g_pan;                   /* g_pPanorama                  */
static const u8 *g_ring;            /* g_pRingTexture, which is     */
static u32 g_ring_len;              /* Fplasma - explosion.txt 3    */
static s32 g_cam[SHIP_CAM];
static s32 g_cam_x, g_cam_y;
static Relic_Cam g_look;            /* yaw, pitch and the heading   */
static s32 g_scene_ang;             /* g_SceneAngleA                */
static s32 g_flare_step;            /* g_FlareSizeStep              */
static s32 g_portal_phase;          /* g_PortalPhase                */
static s32 g_scroll;                /* g_PanoramaScrollX            */
static s32 g_range;                 /* g_ApproachRange              */
static s32 g_ripple;                /* g_RipplePhase                */
static s32 g_spin;                  /* g_ApproachSpin               */
static int g_baked;                 /* the warp buffer is filled    */
static int g_resky;                 /* the Earth has been wiped     */
static s32 g_open;                  /* g_PortalOpen                 */
static s32 g_wob_a, g_wob_b;        /* 0x002d509c, 0x002d50a0       */
static s32 g_grow;                  /* g_BurstGrow                  */
static s32 g_front;                 /* g_BurstFront                 */
static s32 g_decay;                 /* g_BurstDecay                 */
static u8 *g_lightmap;              /* g_BurstLightMap, built       */
static u8 *g_frontmap;              /* g_BurstFrontMap, a copy      */
static u8 g_frontramp[256 * PX_BYTES];
static s32 g_part[RT_PARTS][5];     /* g_BurstParticles             */
static int g_seeded;

static void burst_seed(void);
static int burst_maps(void);
static int g_ready;

/* ORIGIN 0x002d2ed2 Space_ResetPanoramaToSky - and it is also
 * Space_BuildPanorama's first pass, which is why they share a body.
 * THE COLUMN IS A BYTE, so the 256-wide sky repeats two and a half
 * times across the 640. */
static void pan_sky(void)
{
	const u8 *sky = Space_SkyTexture();
	s32 r, c;

	if (!sky || !g_pan) return;
	for (r = 0; r < RT_PAN_H; r++) {
		u8 *dst = g_pan + (size_t)(r * RT_PAN_W) * PX_BYTES;

		for (c = 0; c < RT_PAN_W; c++, dst += PX_BYTES) {
			const u8 *t = sky
			            + (size_t)(((r << 8) | (c & RT_SKY_WRAP)))
			              * PX_BYTES;
			int k;

			for (k = 0; k < PX_BYTES; k++) dst[k] = t[k];
		}
	}
}

/* ORIGIN 0x002d2f02 Space_BuildPanorama. Three passes, and the third
 * is the same limb glow Earth_BuildTexture bakes into the planet:
 * |r - RT_LIMB_R| peaks ON a circle, so it is the blue rim at the
 * edge rather than a wash over the disc. */
static int pan_build(void)
{
	const u8 *overlay;
	u32 len;
	s32 r, c;

	if (!Space_SkyTexture()) return 1;
	g_pan = (u8 *)malloc((size_t)(RT_PAN_W * RT_PAN_H) * PX_BYTES);
	if (!g_pan) return 1;
	pan_sky();

	/* Fearth again, as part_earth.c reads it, and a zero longword is
	 * a hole here too. THIS IS THE PLANET EDGE. */
	overlay = Dat_Chunk("Fearth", &len);
	if (!overlay || len < (u32)(RT_STAMP_ROWS * RT_STAMP_COLS * 4L))
		return 1;
	for (r = 0; r < RT_STAMP_ROWS; r++) {
		const u8 *src = overlay
		              + (size_t)(r * RT_STAMP_COLS) * PX_BYTES;
		u8 *dst = g_pan + (size_t)(r * RT_PAN_W) * PX_BYTES;

		for (c = 0; c < RT_STAMP_COLS; c++, src += PX_BYTES,
		                                   dst += PX_BYTES) {
			if (!src[0] && !src[1] && !src[2] && !src[3]) continue;
			dst[0] = src[0]; dst[1] = src[1];
			dst[2] = src[2]; dst[3] = src[3];
		}
	}

	for (r = 0; r < RT_LIMB_ROWS; r++) {
		s32 x = RT_LIMB_X0 + r * RT_LIMB_STEP;
		u8 *dst = g_pan
		        + (size_t)(RT_LIMB_AT + r * RT_PAN_W) * PX_BYTES;

		for (c = 0; c < RT_LIMB_COLS; c++, dst += PX_BYTES) {
			s32 y = RT_LIMB_Y0 + c * RT_LIMB_STEP;
			s32 v = (Gfx_Isqrt(x * x + y * y) - RT_LIMB_R) * 2L;
			const u8 *ramp;
			int k;

			if (v < 0) v = -v;
			if (v > 0xffL) v = 0xffL;
			ramp = Image_At((u32)(RT_HALO_RAMP + (u32)(v * 4L)));
			if (!ramp) continue;
			for (k = 0; k < PX_BYTES; k++) {
				s32 t = dst[k] + (ramp[k] >> 1);
				dst[k] = (u8)(t > CHAN_MAX ? CHAN_MAX : t);
			}
		}
	}
	return 0;
}

/* ORIGIN 0x002c989e Space_BlitPanorama - a 160 by 100 window, the
 * source stepping a whole 640-pixel row past each one. */
static void pan_blit(Relic_Frame *f, s32 scroll)
{
	s32 y;

	s32 end = RT_PAN_W * RT_PAN_H;

	if (!g_pan) return;
	/* THE SOURCE INDEX IS LINEAR AND MUST NOT BE CLAMPED PER ROW. The
	 * scroll passes 0x1e0 two thirds of the way through, and from
	 * there each row's 160 pixels run off the right of a 640-wide
	 * panorama and CONTINUE INTO THE NEXT ROW - which is still inside
	 * the buffer. Clamping froze the backdrop at exactly that tick.
	 * return.txt 2.1. */
	for (y = 0; y < RT_PAN_H; y++) {
		s32 at = y * RT_PAN_W + scroll;
		s32 py0 = FX_MUL(y, f->scale), py1 = FX_MUL(y + 1, f->scale);
		s32 py;

		for (py = py0; py < py1 && py < f->h; py++) {
			u8 *q;
			s32 px;

			if (py < 0) continue;
			q = f->px + (size_t)(py * f->w) * PX_BYTES;
			for (px = 0; px < f->w; px++) {
				/* The window is the demo's 160 columns however wide
				 * the host is, so the column divides back down. */
				s32 dx = PX_TO_DEMO(px, f->scale);
				const u8 *t;
				u8 *p = q + (size_t)px * PX_BYTES;
				s32 s;
				int k;

				if (dx < 0) dx = 0;
				if (dx > SPACE_W - 1) dx = SPACE_W - 1;
				/* OURS - the last row overruns; wrap rather than
				 * leave it stale or blacken it. return.txt 2.1 */
				s = at + dx;
				while (s < 0) s += end;
				while (s >= end) s -= end;
				t = g_pan + (size_t)s * PX_BYTES;
				for (k = 0; k < PX_BYTES; k++) p[k] = t[k];
			}
		}
	}
}

/* ORIGIN 0x002d20fa Space_DrawSunBlob - orb.c holds it, because it
 * is Orb_DrawSphereTo's skeleton and the Earth shot draws one too. */
static void sun_blob(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                     s32 ang_a, s32 ang_b)
{
	Orb_DrawSunBlob(f, g_ring, g_ring_len, cx, cy, radius,
	                ang_a, ang_b);
}

/* ORIGIN 0x002cdfe4 Space_DrawSunFlare - it draws ONE FRAME IN EIGHT
 * and jitters all three of x, y and radius, so it reads as a flicker
 * rather than a halo. */
static void sun_flare(Relic_Frame *f, s32 scroll)
{
	s32 x, y, r;

	if (Gfx_Rand(RT_SF_ONE_IN)) return;
	x = RT_SF_X - scroll + Gfx_Rand(RT_SF_JX) - RT_SF_JX / 2;
	y = RT_SF_Y + Gfx_Rand(RT_SF_JY) - RT_SF_JY / 2;
	r = Gfx_Rand(RT_SF_RJ) + RT_SF_R0;
	Gfx_GlowBlob(f, x, y, r, g_RampBoltHalf);
}

/* OURS */
static void return_reset(void)
{
	g_swirl_end = 0;
	g_depart_end = 0;
	g_cam_x = RT_CAM_X;
	g_cam_y = RT_CAM_Y;
	g_look.yaw = RT_YAW;
	g_look.pitch = RT_PITCH;
	g_look.head = 0;
	g_look.angle = 0;
	g_look.angle2 = 0;
	g_scene_ang = 0;
	g_flare_step = 0;
	g_portal_phase = 0;
	g_range = RT_RANGE0;
	g_ripple = 0;
	g_spin = 0;
	g_baked = 0;
	g_resky = 0;
	g_open = 0;
	g_wob_a = 0;
	g_wob_b = 0;
	g_grow = RT_BU_GROW0;
	g_front = RT_BU_FRONT0;
	g_decay = RT_BU_DECAY0;
	g_seeded = 0;
	g_scroll = ((-RT_YAW) >> 2) + RT_SCROLL_BIAS;
}

/* OURS */
static int return_init(void)
{
	if (Space_Init()) return 1;
	g_ring = Dat_Chunk("Fplasma", &g_ring_len);
	if (Temple_UseObjectSet(&RT_SET)) return 1;
	Gfx_BuildTurbulence();
	if (burst_maps()) return 1;
	Gfx_SetLightBase(RT_LIGHT_R, RT_LIGHT_G, RT_LIGHT_B);
	g_ready = (pan_build() == 0);
	return_reset();
	return 0;
}

/* ORIGIN 0x002d5b86 - the VBL hook's mode 1 block. */
static void return_step(s32 tick)
{
	if (tick <= 1) return_reset();
	if (tick <= RT_SETUP) return;
	tick -= RT_SETUP;
	/* ORIGIN 0x002d5c38 - mode 2, which is the whole of the ending's
	 * motion. The range is clamped at zero where the flight's
	 * counters are not. */
	if (tick == RT_BURST + 1L)
		Music_Arm(RT_SW_CUE_ORDER, RT_SW_CUE_ROW, RT_SW_TICKS);
	if (tick > RT_BURST && !g_swirl_end
	    && Music_Reached(tick - RT_BURST)) {
		g_swirl_end = tick;
		Music_Arm(RT_BU_CUE_ORDER, RT_BU_CUE_ROW, RT_BU_TICKS);
	}
	if (g_swirl_end && !g_depart_end
	    && Music_Reached(tick - g_swirl_end))
	    g_depart_end = tick;
	/* ORIGIN 0x002d5b44 - mode 4, the burst. */
	if (rt_in_burst(tick)) {
		if (!g_seeded) burst_seed();
		g_spin += RT_BU_SPIN_STEP;
		g_scene_ang += RT_BU_ANG_STEP;
		g_grow++;
		g_front--;
		/* ORIGIN 0x002c96aa - OURS: the demo decays the light map
		 * inside the draw, which under-runs on any host that
		 * steps more often than it draws. return.txt 2.3.3. */
		if (g_decay > 0) {
			s32 i;

			g_decay--;
			if (g_lightmap)
				for (i = 0; i < RT_MAP_CELLS; i++) {
					s32 lv = g_lightmap[i];

					if (lv > 0)
						g_lightmap[i] = g_LightSat[lv - 1];
				}
		}
		{
			s32 i;

			/* ORIGIN 0x002c9500 - two adds a particle, and the
			 * whole of their motion. */
			for (i = 0; i < RT_PARTS; i++) {
				g_part[i][0] += g_part[i][2];
				g_part[i][1] += g_part[i][3];
			}
		}
		return;
	}
	/* ORIGIN 0x002d5ab2 - mode 3, the swirl's whole motion. */
	if (tick > RT_BURST) {
		s32 t = tick - RT_BURST;

		g_spin += RT_SW_SPIN_STEP;
		if ((t & 3L) == 0) {
			if (g_open < RT_SW_OPEN_MAX) g_open++;
			g_wob_b--;
			if (g_wob_b < 0) g_wob_b = RT_SW_WOB_SPAN - 1L;
		}
		if ((t & 7L) == 0) {
			g_wob_a++;
			if (g_wob_a >= RT_SW_WOB_SPAN) g_wob_a = 0;
		}
		return;
	}
	if (tick > RT_FLIGHT) {
		/* THE BAKE IS STATE, NOT DRAWING, so it happens here. Done
		 * in frame() it is skipped the moment a host seeks past the
		 * phase boundary, and the ripple then has no source. */
		if (!g_baked) {
			Space_BakePanorama(g_pan, RT_PAN_W, g_scroll);
			g_baked = 1;
		}
		if (tick > RT_RIPPLE_START) {
			g_ripple -= RT_RIPPLE_STEP;
			if (g_range) {
				g_range -= RT_RANGE_STEP;
				if (g_range < 0L) g_range = 0L;
			}
		}
		/* THE DEMO TURNS THIS IN ITS FRAME ROUTINE [0x002c936e] and
		 * that routine is its per-tick step. Here it has to be the
		 * step, or a host drawing once per several ticks - or a
		 * single -t render - would leave the rays still. */
		g_spin += RT_SPIN_STEP;
		return;
	}
	g_scene_ang += RT_ANG_STEP;
	/* ORIGIN 0x002d5b86 - g_FlareSizeStep, which sizes the thrusters
	 * through the set's radius table. */
	g_flare_step++;
	Temple_SetGlowCounter(g_flare_step);
	g_cam_y += RT_CAM_DY;
	g_cam_x -= RT_CAM_DX;
	{
		s32 at[3];

		at[0] = g_cam_x; at[1] = g_cam_y; at[2] = RT_CAM_Z;
		Space_CamAim(&g_look, at, RT_AIM_Z);
	}
	g_scroll = ((-g_look.yaw) >> 2) + RT_SCROLL_BIAS;
	/* ORIGIN 0x002c8eac - ONCE, on the way out of the first loop.
	 * Without it the Earth scrolls into view a second time when the
	 * window starts wrapping rows. return.txt 2.2. */
	if (!g_resky && g_scroll >= RT_RESKY_AT) {
		pan_sky();
		g_resky = 1;
	}
	if (g_cam_y >= RT_PORTAL_AT && g_portal_phase < RT_PORTAL_MAX)
		g_portal_phase += RT_PORTAL_STEP;
}

/* ORIGIN 0x002c935a Space_StepExplosion - the ripple and one flare,
 * and the vessel is simply absent. The first frame of the phase is
 * Space_FillWhite's instead, which is the flash. return.txt 4.2. */
static void explosion_frame(Relic_Frame *f, s32 tick)
{
	Gfx_SetLightBase(RT_EXP_LIGHT_R, RT_EXP_LIGHT_G, RT_EXP_LIGHT_B);
	if (tick <= RT_FLIGHT + RT_FLASH_FIELDS) {
		/* ORIGIN 0x002c8ed0 - 0x3f3f003f, and it is the whole of
		 * the explosion. */
		Space_FillWhite(f);
		return;
	}
	Gfx_SetFlareRayPhase(g_spin);
	Space_Ripple(f, g_ripple, g_range);
	Space_DrawExplosionFlash(f, g_range);
}

/* ORIGIN 0x002c960c Burst_BlurMap - nine taps, the centre a half,
 * each edge an eighth and each corner a thirty-second. */
static void burst_blur(const u8 *src, u8 *dst)
{
	s32 i;

	for (i = RT_MAP_W; i < RT_MAP_W + RT_MAP_CELLS; i++) {
		s32 c = src[i - RT_MAP_W - 1] + src[i - RT_MAP_W + 1]
		      + src[i + RT_MAP_W - 1] + src[i + RT_MAP_W + 1];
		s32 e = src[i - RT_MAP_W] + src[i + RT_MAP_W]
		      + src[i + 1] + src[i - 1];
		s32 v = ((((c >> 2) + (e >> 2)) & 0xffL) >> 1);

		dst[i] = (u8)((((v + src[i]) & 0xffL) >> 1));
	}
}

/* ORIGIN 0x002c9588 Burst_BuildMaps - the explosion's per-pixel
 * maps, all three read out of BAKED data. The mask is 1bpp: a set
 * bit writes 0x30 and a clear one 0x3f. The planet is punched out
 * of the front map at row 28 column 16 so the blast does not cross
 * it.
 * Each map carries a LEADING GUARD ROW: the blur's first cell reads
 * index -1. return.txt 2.3.3. */
static int burst_maps(void)
{
	const u8 *bits = Image_At(RT_MASKBITS);
	const u8 *front = Image_At(RT_FRONTMAP);
	const u8 *rsrc = Image_At(RT_RAMPSRC);
	const u8 *pmask = Space_PlanetMask();
	u8 *scratch, *lm, *fm, *sc;
	size_t span = (size_t)RT_MAP_BUILD + RT_MAP_W * 2L;
	s32 i, r, c;

	if (!bits || !front || !rsrc) return 1;
	lm = (u8 *)malloc(span);
	fm = (u8 *)malloc(span);
	sc = (u8 *)malloc(span);
	if (!lm || !fm || !sc) { free(lm); free(fm); free(sc); return 1; }
	memset(lm, 0, span);
	memset(fm, 0, span);
	memset(sc, 0, span);
	g_lightmap = lm + RT_MAP_W;
	g_frontmap = fm + RT_MAP_W;
	scratch = sc + RT_MAP_W;

	for (i = 0; i < RT_MAP_BUILD; i++) {
		s32 bit = bits[i >> 3] & (0x80L >> (i & 7L));

		g_lightmap[i] = (u8)(bit ? 0x30L : 0x3fL);
	}
	burst_blur(g_lightmap, scratch);
	burst_blur(scratch, g_lightmap);
	free(sc);

	for (i = 0; i < RT_MAP_CELLS; i++) g_frontmap[i] = front[i];
	if (pmask) {
		for (r = 0; r < 64; r++)
			for (c = 0; c < 64; c++)
				if (pmask[r * 64L + c]) {
					s32 at = (28L + r) * RT_MAP_W + 16L + c;

					if (at >= 0 && at < RT_MAP_CELLS)
						g_frontmap[at] = 0;
				}
	}

	/* ORIGIN 0x002c9166 Burst_MakeFrontRamp. */
	for (i = 0; i < 256; i++) {
		u32 v = RD_U32(rsrc + (size_t)i * 4L);

		v = ((v << 8) & 0xffff0000UL) | (v & 0xffUL);
		v = (v & 0xfcfcfcfcUL) >> 2;
		g_frontramp[i * PX_BYTES + 0] = (u8)(v >> 24);
		g_frontramp[i * PX_BYTES + 1] = (u8)(v >> 16);
		g_frontramp[i * PX_BYTES + 2] = (u8)(v >> 8);
		g_frontramp[i * PX_BYTES + 3] = (u8)v;
	}
	return 0;
}

/* ORIGIN 0x002c91aa Burst_SeedParticles - a disc about the flare's
 * own centre, the angle from g_SinTable and A PARTICLE'S SPEED ITS
 * DISTANCE, so the disc expands rather than scattering. */
static void burst_seed(void)
{
	s32 i;

	for (i = 0; i < RT_PARTS; i++) {
		s32 ang = Gfx_Rand(0x2000L);
		s32 rad = Gfx_Rand(0x100L) + 8L;
		s32 px = ((Orb_Sin(ang) * rad) >> 3) + RT_PART_CX;
		s32 py = ((Orb_Cos(ang) * rad) >> 3) + RT_PART_CY;
		s32 vx = (px - RT_PART_CX) >> 7;
		s32 vy = (py - RT_PART_CY) >> 7;

		vx += Gfx_Rand(0xc350L) - 0x61a8L;
		vy += Gfx_Rand(0xc350L) - 0x61a8L;
		g_part[i][0] = px;
		g_part[i][1] = py;
		g_part[i][2] = (vx * 0x4aL) >> 8;
		g_part[i][3] = (vy * 0x4aL) >> 8;
		g_part[i][4] = Gfx_Rand(0x3fL);
	}
	g_seeded = 1;
}

/* ORIGIN 0x002c9526 Burst_DrawParticles - the LOW NIBBLE of a
 * counter added to LIGHT with a byte add that wraps, the counter
 * then advanced 0xd. The nibble cycles, so each one twinkles. */
static void burst_particles(Relic_Frame *f)
{
	s32 i;

	for (i = 0; i < RT_PARTS; i++) {
		s32 px = g_part[i][0], py = g_part[i][1];
		s32 dx, dy, add, qx, qy, x0, x1, y0, y1;

		if (px < 0 || py < 0) goto advance;
		dx = px >> 16;
		dy = py >> 16;
		if (dx >= SPACE_W || dy >= 0x63L) goto advance;
		add = g_part[i][4] & 0xfL;
		x0 = FX_MUL(dx, f->scale); x1 = FX_MUL(dx + 1, f->scale);
		y0 = FX_MUL(dy, f->scale); y1 = FX_MUL(dy + 1, f->scale);
		for (qy = y0; qy < y1 && qy < f->h; qy++) {
			u8 *q;

			if (qy < 0) continue;
			q = f->px + (size_t)(qy * f->w) * PX_BYTES;
			for (qx = x0; qx < x1 && qx < f->w; qx++) {
				u8 *p = q + (size_t)qx * PX_BYTES;

				if (qx < 0) continue;
				p[PX_LIGHT] = (u8)((p[PX_LIGHT] + add) & 0xffL);
			}
		}
advance:
		g_part[i][4] += 0xdL;
	}
}

/* ORIGIN 0x002c9650 Burst_DrawWavefront - two per-pixel passes. The
 * light map adds into LIGHT through g_LightSat, and where the front
 * map clears g_BurstFront >> 2 a colour longword is added to the
 * whole pixel. THE THRESHOLD SWEEPS DOWN A BAKED HEIGHT MAP, which
 * is what makes the blast spread. return.txt 2.3.3. */
static void burst_front(Relic_Frame *f)
{
	s32 i, thr = g_front >> 2;

	if (!g_lightmap || !g_frontmap) return;
	for (i = 0; i < RT_MAP_CELLS; i++) {
		s32 dx = i % RT_MAP_W, dy = i / RT_MAP_W;
		s32 lv = g_lightmap[i];
		s32 fv = (s32)g_frontmap[i] - thr;
		s32 x0, x1, y0, y1, qx, qy;

		if (!lv && fv <= 0) continue;
		x0 = FX_MUL(dx, f->scale); x1 = FX_MUL(dx + 1, f->scale);
		y0 = FX_MUL(dy, f->scale); y1 = FX_MUL(dy + 1, f->scale);
		for (qy = y0; qy < y1 && qy < f->h; qy++) {
			u8 *q;

			if (qy < 0) continue;
			q = f->px + (size_t)(qy * f->w) * PX_BYTES;
			for (qx = x0; qx < x1 && qx < f->w; qx++) {
				u8 *p = q + (size_t)qx * PX_BYTES;
				int k;

				if (qx < 0) continue;
				p[PX_LIGHT] = g_LightSat[(p[PX_LIGHT] + lv) & 0xffL];
				if (fv <= 0) continue;
				for (k = 0; k < PX_BYTES; k++) {
					s32 t = p[k] + g_frontramp[(fv & 0xffL)
					                           * PX_BYTES + k];

					p[k] = (u8)(t > 255 ? 255 : t);
				}
			}
		}
	}
}

/* ORIGIN 0x002c970c Space_DrawPortalSparks - one flare every frame
 * and a SUBTRACTING blob ONE FRAME IN THREE, both sized by the
 * portal counter so they grow with it. return.txt 2.3. */
static void swirl_sparks(Relic_Frame *f)
{
	s32 x = Gfx_Rand(0x1eL) + 0x55L;
	s32 y = Gfx_Rand(0xaL) + 0x23L;
	s32 r = ((Gfx_Rand(0x46L) + 0x1eL) * g_open) >> 7;

	Gfx_Flare(f, x, y, r, g_RayArcs, g_ColRampAmber);
	if (Gfx_Rand(3L)) return;
	x = Gfx_Rand(0x28L) + 0x50L;
	y = Gfx_Rand(0x1eL) + 0x19L;
	r = ((Gfx_Rand(0x28L) + 0x28L) * g_open) >> 7;
	Gfx_GlowBlobSub(f, x, y, r, g_RampSpark);
}

/* ORIGIN 0x002c943c Space_StepPortalBurst - mode 4's frame. Only
 * three of its eight calls are new; the sun blob and the flare are
 * the departure's, and EVERYTHING GROWS FROM g_BurstGrow.
 * return.txt 2.3.1. */
static void burst_frame(Relic_Frame *f, s32 tick)
{
	s32 bx, by, r;

	Gfx_SetLightBase(RT_BU_LIGHT_R, RT_BU_LIGHT_G, RT_BU_LIGHT_B);
	if (tick == g_swirl_end + 1L) {
		/* ORIGIN 0x002c90a6 - ONE white frame between the phases. */
		Space_FillWhite(f);
		return;
	}
	Space_SetCamera(0L, 0L);
	Space_SetPlanetWorld(RT_SW_PLANET_X, RT_SW_PLANET_Z);
	Space_DrawSky(f, 0L, 0L);

	/* ORIGIN 0x002c948a - and g_SunAngleB is forced to ZERO here
	 * where the departure passes 0x5a. */
	sun_blob(f, RT_BU_CX, RT_BU_CY, (g_grow >> 5) + 0xfL,
	         g_scene_ang >> 2, 0L);
	burst_particles(f);

	r = (g_grow >> 4) + 0x28L + Gfx_Rand(0x14L);
	Gfx_SetFlareRayPhase(g_spin);
	Gfx_Flare(f, RT_BU_CX, RT_BU_CY, r, g_RaySpikes, g_ColRampAmber);

	if (Space_Project(RT_SW_PLANET_X, RT_SW_PLANET_Z, &bx, &by))
		Space_DrawPlanetAndHalo(f, bx, by);
	Space_BlitPortalSprite(f, RT_SW_SPRITE_AT);
	burst_front(f);
	Gfx_FlareBand(f, RT_BAND_ROW, RT_SW_CX);
}

/* ORIGIN 0x002c9378 Space_StepPortalRun - the swirl's frame: seven
 * calls, and the vessel is a flat sprite rather than geometry. */
static void swirl_frame(Relic_Frame *f)
{
	s32 r;

	Gfx_SetLightBase(RT_SW_LIGHT_R, RT_SW_LIGHT_G, RT_SW_LIGHT_B);
	Space_SetCamera(0L, 0L);
	Space_SetPlanetWorld(RT_SW_PLANET_X, RT_SW_PLANET_Z);
	Space_DrawSky(f, 0L, 0L);

	/* ORIGIN 0x002c93da - THE FLARE OPENS ON A COMPLEMENT, so it
	 * SHRINKS as the counter climbs, and the jitter keeps it alive. */
	r = g_open ^ RT_SW_OPEN_MAX;
	if (r > RT_SW_FLARE_CAP) r = RT_SW_FLARE_CAP;
	r += Gfx_Rand(0xaL);
	Gfx_SetFlareRayPhase(g_spin);
	Gfx_Flare(f, RT_SW_CX, RT_SW_CY, r, g_RaySpikes, g_ColRampWarmB);

	Gfx_GlowEllipse(f, RT_SW_CX, RT_SW_CY, g_open,
	                g_wob_a, g_wob_b, g_RampBeamHead);
	swirl_sparks(f);
	{
		s32 bx, by;

		/* IT TAKES SCREEN COORDINATES, not the world point. */
		if (Space_Project(RT_SW_PLANET_X, RT_SW_PLANET_Z, &bx, &by))
			Space_DrawPlanetAndHalo(f, bx, by);
	}
	Space_BlitPortalSprite(f, RT_SW_SPRITE_AT);
	Space_DrawPlanetMasked(f, g_open);
}

/* ORIGIN 0x002c9272 Space_StepDeparture. The scroll comes from the
 * yaw, and Camera_AimAhead turns that - so the sun, the flare and the
 * portal all slide with the panorama while the ship holds the frame.
 * return.txt 3.1. */
static void return_frame(Relic_Frame *f, s32 tick)
{
	s32 fx, fy;

	if (tick <= RT_SETUP) { Frame_Clear(f); return; }
	tick -= RT_SETUP;
	if (rt_in_burst(tick)) { burst_frame(f, tick); return; }
	if (tick > RT_BURST) { swirl_frame(f); return; }
	if (tick > RT_FLIGHT) { explosion_frame(f, tick); return; }
	g_cam[0] = g_cam_x;
	g_cam[1] = g_cam_y;
	g_cam[2] = RT_CAM_Z;
	g_cam[3] = g_look.yaw;
	g_cam[4] = g_look.pitch;
	Space_SetCamera(g_look.yaw, g_look.pitch);
	Gfx_SetLightBase(RT_LIGHT_R, RT_LIGHT_G, RT_LIGHT_B);

	pan_blit(f, g_scroll);
	sun_blob(f, RT_SUN_X - g_scroll, RT_SUN_Y, RT_SUN_R,
	         g_scene_ang >> 2, RT_SUN_ANG_B);
	sun_flare(f, g_scroll);

	fx = RT_FLARE_X - g_scroll;
	fy = RT_FLARE_Y;
	/* ORIGIN 0x002c931a - the same value goes to g_ApproachSpin, whose
	 * low byte is the phase Draw_Flare samples the rays at twice. The
	 * scroll climbs as the camera turns, so THE RAYS TURN WITH IT. */
	Gfx_SetFlareRayPhase(fx);
	Gfx_Flare(f, fx, fy, RT_FLARE_R, g_RaySpikes, g_ColRampWarmB);
	Space_PortalGlow(f, g_portal_phase, g_scroll + RT_GLOW_X);

	/* ORIGIN 0x002d28ca Scene_SortAndDrawAll - THE SHIP THAT LEFT THE
	 * SURFACE, not the opening scenes' one. Scene_SetupPart14 rebuilt
	 * the slots from the temple's own meshes. return.txt 2.4. */
	Temple_DrawObjectSet(f, RT_OBJ_ANGLE, g_cam);

	Space_DrawFlareGhosts(f, fx, fy);
}

/* OURS - key=value on the demo's own names. parts.h. */
static void return_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out,
	        "camsrcx=%ld camsrcy=%ld camsrcz=%ld"
	        " camerayaw=%ld camerapitch=%ld"
	        " panoramascrollx=%ld sceneanglea=%ld flaresizestep=%ld"
	        " portalphase=%ld objectangle=%ld"
	        " approachrange=%ld ripplephase=%ld approachspin=%ld"
	        " portalopen=%ld burstgrow=%ld burstfront=%ld"
	        " burstdecay=%ld glowpx=%ld ready=%d",
	        (long)g_cam_x, (long)g_cam_y, (long)RT_CAM_Z,
	        (long)g_look.yaw, (long)g_look.pitch,
	        (long)g_scroll, (long)g_scene_ang, (long)g_flare_step,
	        (long)g_portal_phase, (long)RT_OBJ_ANGLE,
	        (long)g_range, (long)g_ripple, (long)g_spin,
	        (long)g_open, (long)g_grow, (long)g_front,
	        (long)g_decay, (long)Space_PortalGlowPx(), g_ready);
}

/* OURS - the burst runs from wherever the swirl's cue fell. */
static s32 return_length(void)
{
	if (g_depart_end) return g_depart_end + RT_SETUP;
	return (g_swirl_end ? g_swirl_end : RT_SWIRL_END) + RT_BU_TICKS
	       + RT_SETUP;
}

const Relic_Part PART_Return = {
	"2.15", "The ship departing",
	"Space_RunDeparture 0x002c8e58",
	PART_LIT, SPACE_W, SPACE_H,
	return_init, return_step, return_frame, return_stats,
	RT_TICKS, 0, return_length
};
