/* RELIC part 2.16 - debris falling onto the planet, the third and
 * last phase of Space_RunReturn. See docs/effects/debris.txt.
 *
 * Almost every primitive it needs was already built for the earlier
 * space scenes; what is new here is the bolide script, the three
 * entity tables and the scorch blend. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "relic.h"
#include "image.h"
#include "gfx.h"
#include "orb.h"
#include "space.h"
#include "dat.h"
#include "parts.h"

/* ORIGIN 0x002cc3a2 Debris_RunScene - mode 5, fuse 0x1c2. */
#define DB_TICKS        450L
#define DB_LIGHT_R      0x7fL
#define DB_LIGHT_G      0x5fL
#define DB_LIGHT_B      0x3fL

/* The scripted tables, read out of the image - debris.txt 3. */
#define DB_SCRIPT       0x002ce4e4UL
#define DB_SCORCH_POS   0x002ce578UL
#define DB_IMPACT_TRIO  0x002ce5a8UL

/* ORIGIN 0x002d3136 Debris_BuildPlanet - the same three passes as
 * Earth_BuildTexture 0x002d3086 and the same constants, EXCEPT THE
 * LIMB'S X ORIGIN: 0x2a0 here against the earth's 0x280, so the
 * glow sits 32 units further along. The two are separate routines
 * in the demo and are kept separate here. */
#define DB_TEXELS       0x10000L
#define DB_REMAP_BIAS   0x118L
#define DB_REMAP_MASK   0x3ffffL
#define DB_STAMP_AT     (0x8c00L / 4L)
#define DB_STAMP_PITCH  (0x400L / 4L)
#define DB_STAMP_ROWS   0x5cL
#define DB_STAMP_COLS   0xf0L
#define DB_LIMB_AT      (0x1380cL / 4L)
#define DB_LIMB_ROWS    100L
#define DB_LIMB_COLS    250L
#define DB_LIMB_X0      0x2a0L
#define DB_LIMB_Y0      (-0x378L)
#define DB_LIMB_STEP    8L
#define DB_LIMB_R       0x41aL
/* ORIGIN 0x002d31d6 - NOT the earth's 0x002e13c8. */
#define DB_HALO_RAMP    0x002e17c8UL

/* ORIGIN 0x002d221a Texture_GlowLight - debris.txt 2.1.1. */
#define DB_CHORD_ANGLE  0x0029797aUL    /* g_SphereChordAngle     */
#define DB_SPHERE_NORM  0x0026f97aUL    /* g_SphereNormalTable    */
#define DB_NORM_BIAS    0x10100L
#define DB_RING_BIAS    0x20200L
#define DB_GLOW_R       0x7fL
#define DB_GLOW_CX      0x73L
#define DB_GLOW_CY      (-2L)
#define DB_TEX_W        0x100L
/* ORIGIN 0x002ce226 - the streak's ramp. */
#define DB_STREAK_RAMP  0x002e22e2UL
/* ORIGIN 0x002ce08e - a 0x40 byte gate, indexed by the arm count.
 * Only 25 of the 64 counts run the blend. */
#define DB_SCORCH_GATE  0x002ce15eUL
#define DB_PATCH        0x40L
#define DB_PATCH_CELLS  (DB_PATCH * DB_PATCH)
/* ORIGIN 0x002d3184 and 0x002d31b2 - BOTH passes add 0x10000000 to
 * the longword, which is +0x10 on R alone. Earth_BuildTexture's
 * same two passes do not: this is the bombarded planet's red. */
#define DB_RED          0x10L

#define DB_MAX_BOLIDE   16L
#define DB_SPARKS       0x32L
#define DB_IMPACTS      6L
#define DB_REC          6L              /* longwords in a record    */

/* ORIGIN 0x002ce312 - an impact arms the scorch for this many. */
#define DB_SCORCH_ARM   0x40L

/* ORIGIN 0x002d59b6 - mode 5's camera steps. */
#define DB_CAM_DX       0x28L
#define DB_CAM_DY       0x0fL
#define DB_WARP_DA      1L

static s32 g_bol[DB_MAX_BOLIDE][DB_REC];
static s32 g_nbol;
static s16 g_imp[DB_IMPACTS][7];
static s16 g_spark[DB_SPARKS][4];
static s32 g_scorch_arm;                /* 0x002cc4b8               */
static s32 g_scorch_idx;                /* 0x002cc4ba               */
static s32 g_cam_x, g_cam_y;            /* 0x002e0b32, 0x002e0b36   */
static s32 g_warp_a;                    /* 0x002e0c5c               */
static s32 g_turb_phase;                /* 0x002d50a4               */
static u8  g_RampEmber[256];
static u8 *g_planet;                /* 0x002d32d4, the warp source  */
static u8 *g_planet_b;              /* 0x002d32d8, the other one    */
static u8 *g_mask;                  /* *g_ScorchMask                */
static s32 g_sc_off[DB_PATCH_CELLS];
static s16 g_sc_w0[DB_PATCH_CELLS];
static s16 g_sc_w1[DB_PATCH_CELLS];
static int g_ready;

/* OURS - the image is big endian and these records are longwords. */
static s32 rd_s32(const u8 *p)
{
	return (s32)(((u32)p[0] << 24) | ((u32)p[1] << 16)
	           | ((u32)p[2] << 8) | (u32)p[3]);
}

static void db_texture_glow(s32 r, s32 cx, s32 cy);
static void db_snapshot_mask(void);
static void db_build_scorch(void);
static void db_blend_scorch(void);

/* ORIGIN 0x002c9fe4 - the ember ramp, (0xff - i) >> 5 clamped at
 * CHAN_MAX. It never exceeds 7, so a spark is a dim thing. */
static void db_build_ember(void)
{
	s32 i;

	for (i = 0; i < 256L; i++) {
		s32 v = (0xffL - i) >> 5;

		g_RampEmber[i] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
	}
}

/* ORIGIN 0x002d3136 Debris_BuildPlanet - debris.txt 2.1. */
static int db_build_planet(void)
{
	const u8 *sky = Space_SkyTexture();
	const s32 *dir = Space_SkyDir();
	const u8 *overlay;
	u32 len;
	s32 i, r, c;

	if (!sky || !dir) return 1;
	g_planet = (u8 *)malloc((size_t)DB_TEXELS * PX_BYTES);
	if (!g_planet) return 1;

	for (i = 0; i < DB_TEXELS; i++) {
		const u8 *t = sky + ((dir[i] + DB_REMAP_BIAS) & DB_REMAP_MASK);
		u8 *o = g_planet + (size_t)i * PX_BYTES;

		o[0] = (u8)(t[0] + DB_RED); o[1] = t[1];
		o[2] = t[2]; o[3] = t[3];
	}

	overlay = Dat_Chunk("Fearth", &len);
	if (!overlay || len < (u32)(DB_STAMP_ROWS * DB_STAMP_COLS * 4L))
		return 1;
	for (r = 0; r < DB_STAMP_ROWS; r++) {
		const u8 *src = overlay + (size_t)(r * DB_STAMP_COLS) * PX_BYTES;
		u8 *dst = g_planet
		        + (size_t)(DB_STAMP_AT + r * DB_STAMP_PITCH) * PX_BYTES;

		for (c = 0; c < DB_STAMP_COLS; c++, src += PX_BYTES, dst += PX_BYTES) {
			if (!src[0] && !src[1] && !src[2] && !src[3]) continue;
			dst[0] = (u8)(src[0] + DB_RED); dst[1] = src[1];
			dst[2] = src[2]; dst[3] = src[3];
		}
	}

	for (r = 0; r < DB_LIMB_ROWS; r++) {
		s32 x = DB_LIMB_X0 + r * DB_LIMB_STEP;
		u8 *dst = g_planet
		        + (size_t)(DB_LIMB_AT + r * DB_STAMP_PITCH) * PX_BYTES;

		for (c = 0; c < DB_LIMB_COLS; c++, dst += PX_BYTES) {
			s32 y = DB_LIMB_Y0 + c * DB_LIMB_STEP;
			s32 v = (Gfx_Isqrt(x * x + y * y) - DB_LIMB_R) * 2L;
			const u8 *ramp;
			int k;

			if (v < 0) v = -v;
			if (v > 0xffL) v = 0xffL;
			ramp = Image_At((u32)(DB_HALO_RAMP + (u32)(v * 4L)));
			if (!ramp) continue;
			/* ORIGIN 0x002d3228 - the SAME rotate the burst's front
			 * ramp uses: B3.B2.B1.B0 becomes B2.B1.00.B0, so the
			 * limb glow cannot touch LIGHT. */
			for (k = 0; k < PX_BYTES; k++) {
				static const int SRC[4] = {1, 2, -1, 3};
				s32 t;

				if (SRC[k] < 0) continue;
				t = dst[k] + ((ramp[SRC[k]] & 0xfcL) >> 2);
				dst[k] = (u8)(t > 0xffL ? 0xffL : t);
			}
		}
	}

	db_texture_glow(DB_GLOW_R, DB_GLOW_CX, DB_GLOW_CY);
	/* ORIGIN 0x002d326c - pass 5. D0 IS CLEARED OUTSIDE THE LOOP,
	 * so this is a running smear along the texture's memory order,
	 * not a halve: acc = ((acc + texel) & 0xfefefefe) >> 1. */
	{
		u32 acc = 0;
		for (i = 0; i < DB_TEXELS; i++) {
			u8 *q = g_planet + (size_t)i * PX_BYTES;
			u32 v = ((u32)q[0] << 24) | ((u32)q[1] << 16)
			      | ((u32)q[2] << 8) | (u32)q[3];

			acc = ((acc + v) & 0xfefefefeUL) >> 1;
			q[0] = (u8)(acc >> 24); q[1] = (u8)(acc >> 16);
			q[2] = (u8)(acc >> 8);  q[3] = (u8)acc;
		}
	}
	return 0;
}

/* ORIGIN 0x002d32b8 - the mask is a SNAPSHOT of the texture's LIGHT
 * plane, taken when the texture is built and again on every impact. */
static void db_snapshot_mask(void)
{
	s32 i;

	if (!g_mask || !g_planet) return;
	for (i = 0; i < DB_TEXELS; i++)
		g_mask[i] = g_planet[i * PX_BYTES + PX_LIGHT];
}

/* ORIGIN 0x002cc2d0 - the scorch's 64 x 64 blend table. Each cell
 * gets a source offset and a pair of weights summing to 0x1f.
 *
 * THE OFFSET IS A RADIAL DISPLACEMENT: the angle comes from the
 * polar table and sin and cos of it, scaled by the distance, put
 * the sample further out along the same ray. The weight ramps with
 * distance, so the centre keeps the live buffer - which the seed
 * has zeroed - and the rim restores the snapshot. debris.txt 2.5. */
static void db_build_scorch(void)
{
	s32 row, col;

	for (row = 0; row < DB_PATCH; row++) {
		s32 dy = row - 0x20L;

		for (col = 0; col < DB_PATCH; col++) {
			s32 dx = col - 0x20L;
			s32 a = (s32)Gfx_PolarAngle(dx, dy) << 5;
			s32 dist = Gfx_Isqrt(dx * dx + dy * dy) - 1L;
			s32 cx, cy, w;

			if (dist < 0) dist = 0;
			cx = Gfx_Cos(a) * dist;
			cy = Gfx_Sin(a) * dist;
			cx >>= 8;
			cy >>= 8;
			cx = (cx >> 6) + ((cx >> 5) & 1L);
			cy = (cy >> 6) + ((cy >> 5) & 1L);
			cx += 0x20L;
			cy += 0x20L;
			g_sc_off[row * DB_PATCH + col] = cy * 0x100L + cx;

			w = (dist + Gfx_Rand(0xaL) - 0xfL) * 2L;
			if (w < 0) w = 0;
			if (w >= 0x1fL) w = 0x1fL;
			g_sc_w0[row * DB_PATCH + col] = (s16)w;
			g_sc_w1[row * DB_PATCH + col] = (s16)(w ^ 0x1fL);
		}
	}
}

/* ORIGIN 0x002ce084 Debris_BlendScorch - LIGHT only, and it writes
 * the TEXTURE rather than the frame. The two buffers trade places
 * afterwards, so the mark spreads further each time it runs. */
static void db_blend_scorch(void)
{
	const u8 *gate = Image_At(DB_SCORCH_GATE);
	const u8 *pos = Image_At(DB_SCORCH_POS);
	s32 base, row, col, seed;
	u8 *tmp;

	if (!g_scorch_arm || !gate || !pos || !g_mask) return;
	if (!gate[g_scorch_arm]) { g_scorch_arm--; return; }

	base = rd_s32(pos + (size_t)(g_scorch_idx - 1L) * 8L)
	     + (rd_s32(pos + (size_t)(g_scorch_idx - 1L) * 8L + 4L) << 8);

	for (row = 0; row < DB_PATCH; row++) {
		for (col = 0; col < DB_PATCH; col++) {
			s32 k = row * DB_PATCH + col;
			s32 at = (base + row * DB_TEX_W + col) & (DB_TEXELS - 1L);
			s32 src = (base + g_sc_off[k]) & (DB_TEXELS - 1L);
			s32 v = g_planet_b[src * PX_BYTES + PX_LIGHT] * g_sc_w1[k]
			      + g_mask[at] * g_sc_w0[k];

			g_planet[at * PX_BYTES + PX_LIGHT] = (u8)((v >> 5) & 0xffL);
		}
	}

	tmp = g_planet; g_planet = g_planet_b; g_planet_b = tmp;

	/* ORIGIN 0x002ce144 - the 2 x 2 at the patch centre, zeroed as
	 * the seed the spread grows from. */
	seed = (base + 0x20L * DB_TEX_W + 0x20L) & (DB_TEXELS - 1L);
	g_planet_b[seed * PX_BYTES + PX_LIGHT] = 0;
	g_planet_b[((seed - 1L) & (DB_TEXELS - 1L)) * PX_BYTES + PX_LIGHT] = 0;
	g_planet_b[((seed - DB_TEX_W) & (DB_TEXELS - 1L)) * PX_BYTES + PX_LIGHT] = 0;
	g_planet_b[((seed - DB_TEX_W - 1L) & (DB_TEXELS - 1L)) * PX_BYTES
	           + PX_LIGHT] = 0;
	g_scorch_arm--;
}

/* OURS - a 68k index register taken as a SIGNED word. */
static s32 db_sw(s32 v)
{
	return (s32)(s16)(u16)(v & 0xffffL);
}

/* ORIGIN 0x002d221a Texture_GlowLight - the planet's whole LIGHT
 * plane: a clipped circle whose every texel is shaded through two
 * chord angles, two sphere-normal lookups and two Fplasma samples.
 * IT WRITES BYTE +2 AND NOTHING ELSE. debris.txt 2.1.1.
 *
 * The half-width is Gfx_Isqrt rather than g_pSpanHalfWidth, which
 * is the divergence gfx.c's span_clip already documents. */
static void db_texture_glow(s32 r, s32 cx, s32 cy)
{
	const u8 *chord = Image_At(DB_CHORD_ANGLE);
	const u8 *norm = Image_At(DB_SPHERE_NORM);
	const u8 *ring;
	u32 rlen;
	s32 top, bottom, dy, row, height;

	ring = Dat_Chunk("Fplasma", &rlen);
	if (!chord || !norm || !ring) return;
	if (r + cy <= 0 || cy - r >= DB_TEX_W) return;
	if (r + cx <= 0 || cx - r >= DB_TEX_W) return;

	bottom = cy + r;
	if (bottom >= DB_TEX_W) bottom = DB_TEX_W - 1L;
	top = cy - r;
	dy = -r;
	if (top < 0) { dy -= top; top = 0; }
	height = bottom - top - 1L;

	for (row = 0; row <= height; row++, dy++) {
		/* ORIGIN 0x002ca188 - the table's upper half is MIRRORED FROM
		 * ONE ROW BACK, so dy >= 0 is served by dy + 1's chord. */
		s32 d = (dy < 0) ? dy : dy + 1L;
		s32 half = Gfx_Isqrt(r * r - d * d);
		s32 cya = chord[db_sw((dy << 8) | r)];
		s32 left, right, dx, col, width;
		u8 *q;

		right = cx + half;
		if (right >= DB_TEX_W) right = DB_TEX_W - 1L;
		left = cx - half;
		dx = -half;
		if (left < 0) { dx -= left; left = 0; }
		width = right - left - 1L;
		q = g_planet + (size_t)((top + row) * DB_TEX_W + left) * PX_BYTES;

		for (col = 0; col <= width; col++, dx++, q += PX_BYTES) {
			s32 cxa = chord[db_sw((dx << 8) | half)];
			s32 n, i0, i1;
			s32 v;

			n = db_sw((cya << 8) | cxa);
			n = (s32)RD_U16(norm + DB_NORM_BIAS + n * 2L);
			n = (n & 0xff00L) | ((n - 0x5aL) & 0xffL);
			n = (s32)RD_U16(norm + DB_NORM_BIAS + db_sw(n) * 2L);
			n = (n & 0xff00L) | ((n - 0x16L) & 0xffL);
			n = (n * 8L) & 0xffffL;
			i0 = DB_RING_BIAS + db_sw(n) * 4L;
			n = (n & 0xff00L) | ((n - 0x16L) & 0xffL);
			n &= 0xffffL;
			i1 = DB_RING_BIAS + db_sw(n) * 4L + 1L;
			i0 &= (s32)(rlen - 1U);
			i1 &= (s32)(rlen - 1U);
			v = (ring[i0] + ring[i1]) & 0xffL;
			v = (s32)(s8)(u8)(v - 0x0fL);
			q[PX_LIGHT] = (u8)(v < 0 ? 0L : v);
		}
	}
}

/* ORIGIN 0x002cc534 - the script is copied record by record until a
 * NEGATIVE FIRST LONGWORD, which is the only terminator there is. */
static int db_load_script(void)
{
	const u8 *p = Image_At(DB_SCRIPT);
	s32 i, j;

	if (!p) return 1;
	for (i = 0; i < DB_MAX_BOLIDE; i++) {
		if (rd_s32(p + (size_t)i * DB_REC * 4L) < 0) break;
		for (j = 0; j < DB_REC; j++)
			g_bol[i][j] = rd_s32(p + (size_t)(i * DB_REC + j) * 4L);
	}
	g_nbol = i;
	return 0;
}

/* ORIGIN 0x002ce34a Debris_SpawnSpark - the first free of 50 slots,
 * scattered about the body by its own radius. */
static void db_spawn_spark(s32 x, s32 y, s32 r)
{
	s32 i;

	for (i = 0; i < DB_SPARKS; i++) if (!g_spark[i][0]) break;
	if (i == DB_SPARKS) return;
	g_spark[i][0] = 0x19;
	g_spark[i][1] = (s16)(x + (((Gfx_Rand(0x14L) - 0xaL) * r) >> 5));
	g_spark[i][2] = (s16)(y + (((Gfx_Rand(0x14L) - 0xaL) * r) >> 5));
	g_spark[i][3] = (s16)r;
}

/* ORIGIN 0x002ce3aa Debris_SpawnImpact - the first free of six. */
static void db_spawn_impact(s32 x, s32 y, s32 a, s32 b, s32 c)
{
	s32 i;

	for (i = 0; i < DB_IMPACTS; i++) if (!g_imp[i][0]) break;
	if (i == DB_IMPACTS) return;
	g_imp[i][0] = 0xfa;
	g_imp[i][1] = (s16)x;
	g_imp[i][2] = (s16)y;
	g_imp[i][3] = 0xf;
	g_imp[i][4] = (s16)a;
	g_imp[i][5] = (s16)b;
	g_imp[i][6] = (s16)c;
}

/* ORIGIN 0x002d59ac - the VBL hook's mode 5 block. EVERY TERM EASES
 * THE SAME WAY, losing itself >> 5 a field, and the offsets freeze
 * once the body is under one unit across. debris.txt 2.2. */
static void db_step_bolides(void)
{
	s32 i;

	for (i = 0; i < g_nbol; i++) {
		s32 *r = g_bol[i];

		if (r[1] < 0) continue;
		if (r[5]) { r[5]--; continue; }
		r[4] -= r[4] >> 5;
		if (r[4] < 0x10000L) continue;
		r[2] -= r[2] >> 5;
		r[3] -= r[3] >> 5;
	}
}

/* ORIGIN 0x002ce288 Debris_StepBolide - the record says where the
 * body LANDS; the offset at +0x08 is what shrinks toward it. */
static void db_draw_bolides(Relic_Frame *f)
{
	s32 i;

	for (i = 0; i < g_nbol; i++) {
		s32 *r = g_bol[i];
		s32 x, y, size;

		if (r[1] < 0 || r[5]) continue;
		x = r[0] + (r[2] >> 16);
		y = r[1] + (r[3] >> 16);
		size = r[4] >> 16;
		if (size <= 1L) {
			const u8 *t = Image_At(DB_IMPACT_TRIO
			                       + (u32)g_scorch_idx * 8UL);

			r[1] = -1L;
			g_scorch_arm = DB_SCORCH_ARM;
			/* ORIGIN 0x002ce308 - a fresh snapshot per impact. */
			db_snapshot_mask();
			if (t)
				db_spawn_impact(x, y,
				    (s32)(s16)((t[0] << 8) | t[1]),
				    (s32)(s16)((t[2] << 8) | t[3]),
				    (s32)(s16)((t[4] << 8) | t[5]));
			g_scorch_idx++;
			continue;
		}
		size += Gfx_Rand(0xaL);
		if (size > 0x7fL) size = 0x7fL;
		db_spawn_spark(x, y, size);
		Gfx_FlareTurbulent(f, x, y, size, g_RampBeamHead);
	}
}

/* ORIGIN 0x002ce19e Debris_DrawSparks - THE DRAW OWNS THE LIFETIME
 * where the step owns the size, the same split the burst has. */
static void db_draw_sparks(Relic_Frame *f)
{
	s32 i;

	for (i = 0; i < DB_SPARKS; i++) {
		s32 r;

		if (!g_spark[i][0]) continue;
		g_spark[i][0]--;
		r = (g_spark[i][3] >> 1) + Gfx_Rand(0xaL);
		Gfx_GlowBlob(f, g_spark[i][1], g_spark[i][2], r, g_RampEmber);
	}
}

/* ORIGIN 0x002ce1e8 Debris_DrawImpacts - one slot draws BOTH the
 * streak and the bloom; the streak is what it looks like early. */
static void db_draw_impacts(Relic_Frame *f)
{
	const u8 *streak = Image_At(DB_STREAK_RAMP);
	s32 i;

	for (i = 0; i < DB_IMPACTS; i++) {
		s32 d, r;

		if (!g_imp[i][0]) continue;
		g_imp[i][0]--;
		d = g_imp[i][3] >> 2;
		/* ORIGIN 0x002ce212 - the streak only while the slot is
		 * young, and it is the SAME slot as the bloom. */
		if (d < 0x32L && streak)
			Gfx_MeteorStreak(f, g_imp[i][1] + g_imp[i][5],
			                 g_imp[i][2], d, g_imp[i][6],
			                 g_imp[i][4], streak);
		r = (d >> 3) + 0x19L;
		if (r > 0x28L) r = 0x28L;
		Gfx_GlowBlob(f, g_imp[i][1], g_imp[i][2], r, g_RampSpark);
	}
}

/* OURS. The second buffer is Debris_BuildPlanet's pass 6,
 * 0x002d3288, and the scorch's two tables follow it. */
static int db_init(void)
{
	if (Space_Init()) return 1;
	Gfx_BuildTurbulence();
	db_build_ember();
	g_ready = (db_build_planet() == 0) && (db_load_script() == 0);
	if (g_ready) {
		g_planet_b = (u8 *)malloc((size_t)DB_TEXELS * PX_BYTES);
		g_mask = (u8 *)malloc((size_t)DB_TEXELS);
		if (!g_planet_b || !g_mask) return 1;
		memcpy(g_planet_b, g_planet, (size_t)DB_TEXELS * PX_BYTES);
		db_snapshot_mask();
		db_build_scorch();
	}
	return 0;
}

/* ORIGIN 0x002cc530 Debris_SetupScene plus 0x002cc3ea's staging. */
static void db_reset(void)
{
	memset(g_imp, 0, sizeof g_imp);
	memset(g_spark, 0, sizeof g_spark);
	g_scorch_arm = 0;
	g_scorch_idx = 0;
	g_cam_x = 0;
	g_cam_y = 0;
	g_warp_a = 0x3e8L;
	g_turb_phase = 0;
	db_load_script();
}

/* ORIGIN 0x002d59ac - mode 5. */
static void db_step(s32 tick)
{
	if (tick <= 1) db_reset();
	g_warp_a += DB_WARP_DA;
	g_cam_x += DB_CAM_DX;
	g_cam_y += DB_CAM_DY;
	/* ORIGIN 0x002d59e6 - the cursors move every FOURTH field. */
	if (!(g_turb_phase & 3L)) Gfx_StepTurbulence();
	g_turb_phase++;
	db_step_bolides();
	{
		s32 i;

		for (i = 0; i < DB_IMPACTS; i++)
			if (g_imp[i][0]) g_imp[i][3]++;
	}
}

/* ORIGIN 0x002ce046 Debris_DrawFrame. [open] Debris_BlendScorch
 * 0x002ce084 and Draw_MeteorStreak 0x002d4552 are not in yet - the
 * scorch needs the weight pairs at 0x002f1140 and its mask at
 * 0x002f1144, the streak the shape at 0x002e22e2. debris.txt 5. */
static void db_frame(Relic_Frame *f, s32 tick)
{
	(void)tick;
	Gfx_SetLightBase(DB_LIGHT_R, DB_LIGHT_G, DB_LIGHT_B);
	Frame_Clear(f);
	if (!g_ready) return;
	db_blend_scorch();
	Warp_Fill(f, g_planet, g_warp_a, 0L, 0);
	db_draw_bolides(f);
	db_draw_sparks(f);
	db_draw_impacts(f);
}

/* OURS - key=value on the demo's own names. parts.h. */
static void db_stats(char *out, int cap)
{
	s32 i, live = 0, sp = 0, im = 0;

	(void)cap;
	for (i = 0; i < g_nbol; i++) if (g_bol[i][1] >= 0 && !g_bol[i][5]) live++;
	for (i = 0; i < DB_SPARKS; i++) if (g_spark[i][0]) sp++;
	for (i = 0; i < DB_IMPACTS; i++) if (g_imp[i][0]) im++;
	sprintf(out,
	        "camsrcx=%ld camsrcy=%ld warpangle=%ld"
	        " bolides=%ld sparks=%ld impacts=%ld"
	        " scorcharm=%ld scorchindex=%ld ready=%d",
	        (long)g_cam_x, (long)g_cam_y, (long)g_warp_a,
	        (long)live, (long)sp, (long)im,
	        (long)g_scorch_arm, (long)g_scorch_idx, g_ready);
}

const Relic_Part PART_Debris = {
	"2.16", "Debris falling onto the planet",
	"Debris_RunScene 0x002cc3a2",
	PART_LIT, SPACE_W, SPACE_H,
	db_init, db_step, db_frame, db_stats,
	DB_TICKS
};
