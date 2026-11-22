/* RELIC - the Stills module's shared machinery. See stills.h for why
 * this is its own file and not part of src/gfx.c. */

#include <stdlib.h>
#include <string.h>

#include "relic.h"
#include "image.h"
#include "gfx.h"
#include "dat.h"
#include "stills.h"

#define ST_DBOD         0x44424f44UL
#define ST_TURB         1024L
#define ST_CURSORS      512L
#define ST_RAMP_NUM     0x780000L
#define ST_RAMP_T       0xfaL
#define ST_HAZE_ROWS    0x32L
#define ST_HAZE_ROW0    49L
/* ORIGIN 0x003143c4 and 0x00314636 - the seed, at the valley's
 * setup and again at the city's first shot. */
#define ST_HAZE_OFF     0x15eL
#define ST_PLASMA_BIAS  0x20200L
#define ST_PLASMA_LEN   0x40000L
/* ORIGIN 0x00315cdc - the particle cap. */
#define ST_PART_MAX     0x3a98L

/* The module's own state. The demo keeps the same things as globals
 * - 0x0033b060 for the generator, 0x0031654c for the threshold,
 * 0x00316738 onward for the assets - so this mirrors the binary. */
static s32 g_rng;                   /* 0x0033b060                   */
static s32 g_thresh;                /* 0x0031654c                   */
static s32 g_blast;                 /* 0x0033b07c                   */
static s32 g_spin;                  /* 0x0033b048                   */
static s32 g_scroll;
static s32 g_cut;
static s32 g_hazeoff = ST_HAZE_OFF;
static s32 g_hazedrift;             /* 0x00315f68                   */
static u8 *g_plasma;                /* the DARKENED Fplasma          */                   /* 0x00315da0                   */                /* 0x0033b038 >> 8, in rows     */
static const u8 *g_pic;             /* 0x00316738                   */
static const u8 *g_layer2;          /* 0x0031673c                   */
static const u8 *g_map;             /* 0x00316740                   */
static u8 *g_prev;
static s32 g_prev_n;                  /* the blur's destination       */
static u8 g_rramp[256];             /* 0x0033b79e                   */
static u8 g_tramp[256];             /* 0x0033b9c6                   */
static u8 g_splat[256 * PX_BYTES];  /* *(0x0033b0ce)                */
static u8 g_bramp[256 * PX_BYTES];  /* 0x0033b1fe                   */
static s16 g_rays[256];             /* 0x0033ae34                   */
static s16 g_turb[ST_TURB];         /* 0x00314d1a                   */
static s32 g_cur1, g_cur2;          /* 0x0031551a, 0x0031551e       */
static s16 g_trail[31][4];          /* 0x0033b10e                   */

/* OURS - the demo's accessors are plain global writes. g_cut is its
 * 0x00315da0 and g_hazeoff its 0x00315f66. */
void Stills_SetCut(s32 v) { g_cut = v; }

void Stills_SetHazeOffset(s32 v) { g_hazeoff = v; }

/* ORIGIN 0x00314392 and 0x003145fa - 0x00315f68. */
void Stills_SetHazeDrift(s32 v) { g_hazedrift = v; }

/* ORIGIN 0x00315660 - Valley_RunScene's setup calls this ONCE, at
 * 0x00314334, and it DARKENS FPLASMA IN PLACE. The chunk is 0x10000
 * longwords read as a 256 x 256 grid; the row index runs -0x80 to
 * 0x7f and every byte of the row has max(0, |row| - 0x28) taken off
 * it, saturating at zero.
 *
 * So the plasma keeps its middle 81 rows and fades to nothing at the
 * edges. Valley_DrawHaze's lower pass walks the index out past row
 * 0x28 and reads the faded part, which is what lets its contribution
 * die away instead of stopping at a row - WITHOUT THIS THE HAZE ENDS
 * ON A HARD HORIZONTAL EDGE. The debris scene reads the same chunk
 * but runs first, so it sees it undarkened. */
int Stills_BuildPlasma(void)
{
	const u8 *src;
	u32 len;
	s32 l;

	src = Dat_Chunk("Fplasma", &len);
	if (!src || len < (u32)ST_PLASMA_LEN) return 1;
	free(g_plasma);
	g_plasma = (u8 *)malloc((size_t)ST_PLASMA_LEN);
	if (!g_plasma) return 1;
	memcpy(g_plasma, src, (size_t)ST_PLASMA_LEN);
	for (l = 0; l < 0x10000L; l++) {
		s32 row = (l >> 8) - 0x80L;
		s32 d = (row < 0 ? -row : row) - 0x28L;
		u8 *q = g_plasma + (size_t)l * 4L;
		s32 k;

		if (d <= 0) continue;
		for (k = 0; k < 4L; k++)
			q[k] = (u8)(q[k] >= d ? q[k] - d : 0L);
	}
	return 0;
}

void Stills_SetThreshold(s32 t)   { g_thresh = t; }
s32  Stills_Threshold(void)       { return g_thresh; }
void Stills_SetBlast(s32 v)       { g_blast = v; }
void Stills_StepSpin(s32 d)       { g_spin += d; }
void Stills_SetScroll(s32 rows)   { g_scroll = rows; }
void Stills_SeedRand(s32 seed)    { g_rng = seed; }
const u8 *Stills_BlobRamp(void)   { return g_rramp; }
void Stills_ResetTrail(void)      { memset(g_trail, 0, sizeof g_trail); }

/* ORIGIN 0x003141da - Gfx_BuildColourRamp's generator, same output. */
void Stills_BuildBlastRamp(void)
{
	Gfx_BuildColourRamp(g_bramp, 0x7fL, 0x3fL, 0x3fL, ST_RAMP_NUM);
}

/* ORIGIN 0x00316506 - the two turbulence cursors. */
void Stills_StepCursors(void)
{
	/* ORIGIN 0x0031648e - the haze's sample offset drifts by
	 * 0x00315f68 a field, and THAT STEP IS ZERO FOR THE WHOLE
	 * VALLEY: 0x00314392 clears it at the scene's setup and
	 * 0x003145fa puts it to 1 on the way into the city. The
	 * valley's plasma is still and only the city's moves. */
	g_hazeoff = (g_hazeoff + g_hazedrift) & 0xffffL;
	g_cur2 -= 2L;
	if (g_cur2 < 0) g_cur2 = ST_CURSORS;
	g_cur1 += 1L;
	if (g_cur1 >= ST_CURSORS) g_cur1 = 0;
}

/* ORIGIN 0x00316476 - the threshold falls by the loop's rate every
 * field and floors at zero. That fall is the expanding ring. */
void Stills_StepThreshold(s32 rate)
{
	if (rate <= 0) return;
	g_thresh -= rate;
	if (g_thresh < 0) g_thresh = 0;
}

/* OURS - the demo writes 0x00316738 onward directly. */
void Stills_SetSources(const u8 *picture, const u8 *layer2,
                       const u8 *heightmap)
{
	g_pic = picture;
	g_layer2 = layer2;
	g_map = heightmap;
}

/* OURS - the blur needs somewhere to keep the previous frame. */
int Stills_BlurInit(s32 texels)
{
	(void)texels;
	free(g_prev);
	g_prev = 0;
	g_prev_n = 0;
	return 0;
}

/* OURS - SIZED FROM THE FRAME. The host's window is resizable, so a
 * buffer cut to any fixed multiple of ST_TEXELS is walked off the end
 * once the frame grows past it. */
static int stills_blur_fit(s32 n)
{
	if (g_prev && g_prev_n >= n) return 0;
	free(g_prev);
	g_prev = (u8 *)calloc((size_t)n, PX_BYTES);
	g_prev_n = g_prev ? n : 0;
	return g_prev ? 0 : 1;
}

/* ORIGIN 0x0031429a - a palette of `entries` longwords, then one
 * INDEX BYTE a texel. Both go through the same b0.b1.00.b2 rotate
 * the DEEP path uses, so the result is 6-bit with LIGHT zero. */
u8 *Stills_LoadPalettised(const u8 *src, s32 entries, s32 texels)
{
	const u8 *idx;
	u8 *out;
	s32 i;

	if (!src || texels <= 0) return 0;
	idx = src + (size_t)entries * 4L;
	out = (u8 *)malloc((size_t)texels * PX_BYTES);
	if (!out) return 0;
	for (i = 0; i < texels; i++) {
		u32 v = RD_U32(src + (size_t)idx[i] * 4L);

		v = ((v << 8) & 0xffff0000UL) | (v & 0xffUL);
		v = (v & 0xfcfcfcfcUL) >> 2;
		out[i * PX_BYTES + 0] = (u8)(v >> 24);
		out[i * PX_BYTES + 1] = (u8)(v >> 16);
		out[i * PX_BYTES + 2] = (u8)(v >> 8);
		out[i * PX_BYTES + 3] = (u8)v;
	}
	return out;
}

/* ORIGIN 0x003148d4 - 0x4f + 1 swaps a row, pixel i against pixel
 * ST_W - 1 - i, over `rows`. THE ROW COUNT IS THE STILL'S, not the
 * frame's: the city's picture is 153 tall. */
void Stills_MirrorPixels(u8 *pic, s32 rows)
{
	s32 y, x;

	if (!pic) return;
	for (y = 0; y < rows; y++) {
		u8 *r = pic + (size_t)(y * ST_W) * PX_BYTES;

		for (x = 0; x < ST_W / 2L; x++) {
			u8 *a = r + (size_t)x * PX_BYTES;
			u8 *b = r + (size_t)(ST_W - 1L - x) * PX_BYTES;
			int k;

			for (k = 0; k < PX_BYTES; k++) {
				u8 t = a[k]; a[k] = b[k]; b[k] = t;
			}
		}
	}
}

/* ORIGIN 0x003148b0 - the same walk for a byte map. */
void Stills_MirrorBytes(u8 *map, s32 rows)
{
	s32 y, x;

	if (!map) return;
	for (y = 0; y < rows; y++) {
		u8 *r = map + (size_t)(y * ST_W);

		for (x = 0; x < ST_W / 2L; x++) {
			u8 t = r[x]; r[x] = r[ST_W - 1L - x]; r[ST_W - 1L - x] = t;
		}
	}
}

/* ORIGIN 0x003148fe - the second table is the sine 0xc00 BYTES on,
 * which is 0x300 entries, not a cosine. */
s32 Stills_Sin(s32 base, s32 angle)
{
	return (s32)Image_U32(A_SIN_TABLE + (u32)base
	                      + (u32)((angle & 0x1fffL) * 4L));
}

/* ORIGIN 0x0031586c Valley_Rand - the Stills module's own LCG, no
 * relation to the space module's Math_Rand. */
s32 Stills_Rand(s32 range)
{
	s32 h;

	g_rng = (s32)((u32)g_rng * 0x5d43UL + 0x5c2dUL);
	h = (s32)(((u32)g_rng >> 16) ^ ((u32)g_rng & 0xffffUL));
	return (range > 0) ? (h & 0x7fffL) % range : 0;
}

/* OURS - the demo's add-and-halve idiom. The sum's carry out is the
 * X bit roxr.l #1 rotates back in, so the halving keeps all 33 bits
 * while the 0xfefefefe mask stops bytes bleeding into each other. */
static u32 stills_mean(u32 a, u32 b)
{
	u32 s = a + b;
	u32 x = (s < a) ? 0x80000000UL : 0UL;

	return ((s & 0xfefefefeUL) >> 1) | x;
}

/* ORIGIN 0x003140c4 with D7 = 0x780000 and D3 = 0xfa, which is the
 * numerator the space module's amber ramp uses too. Only the UPPER
 * clamp is in the original; the quotient never goes negative here. */
void Stills_BuildBlobRamp(void)
{
	s32 i;

	for (i = 0; i < 256L; i++) {
		s32 t = (i * ST_RAMP_T) >> 7;
		s32 t2 = t * t;
		s32 v = (t2 == 0) ? ST_RAMP_NUM : ((ST_RAMP_NUM - t2) / t2);

		v >>= 4;
		g_rramp[i] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
	}
}

/* ORIGIN 0x0031409a - the trail's ramp, (0xff - i) >> 4 clamped,
 * so it tops out at 15 and the trail stays dim under the head. */
void Stills_BuildTrailRamp(void)
{
	s32 i;

	for (i = 0; i < 256L; i++) {
		s32 v = (0xffL - i) >> 4;

		g_tramp[i] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
	}
}

/* ORIGIN 0x00315b22 - 256 longwords from the palette through the
 * SAME b0.b1.00.b2 rotate the picture uses, so the table carries a
 * zero LIGHT byte like everything else here. */
void Stills_BuildSplatTable(u32 palette)
{
	const u8 *pal = Image_At(palette);
	s32 i;

	if (!pal) return;
	for (i = 0; i < 256L; i++) {
		u32 v = RD_U32(pal + (size_t)i * 4L);

		/* The palette entry is 00.RR.GG.BB and the rotate turns it
		 * into RR.GG.00.BB - the picture's own path reads three
		 * bytes instead, which is not the same thing. */
		v = ((v << 8) & 0xffff0000UL) | (v & 0xffUL);
		v = (v & 0xfcfcfcfcUL) >> 2;
		g_splat[i * PX_BYTES + 0] = (u8)(v >> 24);
		g_splat[i * PX_BYTES + 1] = (u8)(v >> 16);
		g_splat[i * PX_BYTES + 2] = (u8)(v >> 8);
		g_splat[i * PX_BYTES + 3] = (u8)v;
	}
}

/* ORIGIN 0x00314162 - the explosion's spokes. TWENTY RUNS of a
 * random height written over a random span of angles, then smoothed.
 * Entirely Valley_Rand driven, so ours differ in detail. */
void Stills_BuildRays(void)
{
	s32 i, k;

	for (i = 0; i < 256L; i++) g_rays[i] = 0;
	for (k = 0; k < 20L; k++) {
		s32 at = Stills_Rand(0x100L);
		s32 h = Stills_Rand(0xdL);
		s32 n = Stills_Rand(0x10L) + 0x14L;

		for (i = 0; i <= n; i++, at = (at + 1L) & 0xffL)
			g_rays[at] = (s16)h;
	}
	/* ORIGIN 0x003141b2 - each entry plus its neighbour, halved by
	 * a lsr.b that touches the LOW BYTE only, and then NEGATED.
	 * The table is therefore <= 0, and Valley_DrawBlast subtracts
	 * it, so a ray pushes its sample FURTHER DOWN the colour ramp.
	 * Built positive the blast comes out a bright disc instead of
	 * a glow with a falling edge. */
	for (i = 0; i < 256L; i++) {
		s32 a = g_rays[i] + g_rays[(i + 1L) & 0xffL];

		a = (a & ~0xffL) | ((a & 0xffL) >> 1);
		g_rays[i] = (s16)(-a);
	}
}

/* ORIGIN 0x00314102 - 1024 words of noise, a Valley_Rand term and
 * two sine walks whose phases advance 0x80 and 0x100 an entry. */
void Stills_BuildTurbulence(void)
{
	s32 i, p2 = 0, p3 = 0;

	for (i = 0; i < ST_TURB; i++, p2 += 0x80L, p3 += 0x100L) {
		s32 a = (Stills_Rand(0x1388L) - 0x9c4L) * 4L;

		a += Stills_Sin(0L, p2);
		g_turb[i] = (s16)((a >> 10) + (Stills_Sin(0L, p3) >> 10));
	}
	g_cur1 = 0;
	g_cur2 = 0;
}

/* ORIGIN 0x0031426a Valley_LoadDeepImage - the FORM is scanned a
 * BYTE at a time for 'DBOD', not walked as chunks, and the body is
 * three plain bytes a pixel. Each becomes b0.b1.00.b2 masked
 * 0xfcfcfcfc and halved twice, so LIGHT is zero by construction. */
u8 *Stills_LoadDeep(u32 form, s32 texels)
{
	const u8 *p = Image_At(form);
	s32 i;

	u8 *pic;

	if (!p || texels <= 0) return 0;
	for (i = 0; i < 0x10000L; i++, p++)
		if (RD_U32(p) == ST_DBOD) break;
	if (i == 0x10000L) return 0;
	p += 8;

	pic = (u8 *)malloc((size_t)texels * PX_BYTES);
	if (!pic) return 0;
	for (i = 0; i < texels; i++, p += 3) {
		u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) | (u32)p[2];

		v = (v & 0xfcfcfcfcUL) >> 2;
		pic[i * PX_BYTES + 0] = (u8)(v >> 24);
		pic[i * PX_BYTES + 1] = (u8)(v >> 16);
		pic[i * PX_BYTES + 2] = (u8)(v >> 8);
		pic[i * PX_BYTES + 3] = (u8)v;
	}
	return pic;
}

/* ORIGIN 0x00314c0c Valley_DrawRock - THE SWIRL'S ALGORITHM on a
 * circular span. The falloff is warped by two turbulence samples,
 * one at the distance and one at the distance plus a QUARTER OF THE
 * ANGLE, exactly as Space_DrawLightBlob does it - but this is the
 * Stills module's own copy, reading its own tables. valley.txt 2.5.
 *
 * It adds to LIGHT and nothing else, with no saturation.
 *
 * THE RAMP INDEX IS A SIGNED WORD and runs off the ramp both ways:
 * 0x3f below it, zeros above. Masking it draws a ring. */
void Stills_Blob(Relic_Frame *f, s32 cx, s32 cy, s32 r,
                    const u8 *ramp, int turb)
{
	const u8 *polar = Image_At(A_POLAR_TABLE);
	s32 top, bottom, dy, row, height;

	if (!polar || r <= 0) return;
	if (r + cy <= 0 || cy - r >= ST_H) return;
	if (r + cx <= 0 || cx - r >= ST_W) return;

	bottom = cy + r;
	if (bottom >= ST_H) bottom = ST_H - 1L;
	top = cy - r;
	dy = -r;
	if (top < 0) { dy -= top; top = 0; }
	height = bottom - top - 1L;

	for (row = 0; row <= height; row++, dy++) {
		s32 d = (dy < 0) ? dy : dy + 1L;
		s32 half = Gfx_Isqrt(r * r - d * d);
		s32 left, right, dx, col, width, off;
		s32 py0 = FX_MUL(top + row, f->scale);
		s32 py1 = FX_MUL(top + row + 1L, f->scale);

		right = cx + half;
		if (right >= ST_W) right = ST_W - 1L;
		left = cx - half;
		dx = -half;
		if (left < 0) { dx -= left; left = 0; }
		width = right - left - 1L;
		off = (((dy & 0xffL) << 8) | ((dx) & 0xffL)) * 2L;

		for (col = 0; col <= width; col++, off += 2L) {
			s32 w = (s32)RD_U16(polar
			                    + ((off + 0x10000L) & 0x1ffffL));
			s32 dist = w & 0xffL;
			s32 ang = (w >> 8) >> 2;
			s32 v = (s32)Gfx_BlobFalloff(r, dist);
			s32 add, x, px, px0, px1, py;

			if (turb) {
				v += g_turb[(g_cur1 + dist) & (ST_TURB - 1L)];
				v += g_turb[(g_cur2 + dist + ang) & (ST_TURB - 1L)];
			} else {
				(void)ang;
			}
			add = (v < 0) ? CHAN_MAX
			              : ((v > 0xffL) ? 0L : (s32)ramp[v]);
			if (!add) continue;
			x = left + col;
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1L, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;

					if (px < 0) continue;
					p[PX_LIGHT] = (u8)((p[PX_LIGHT] + add) & 0xffL);
				}
			}
		}
	}
}

/* ORIGIN 0x00315524 - the impact's blast. UNLIKE EVERY OTHER BLOB
 * IN THIS SCENE IT ADDS A WHOLE LONGWORD, so it writes colour and
 * not just LIGHT.
 *
 * The warp is SUBTRACTIVE AND ANGULAR: the falloff loses two ray
 * samples taken at the angle plus the spin and at the angle plus
 * twice it, which is what cuts the spokes into the fireball.
 *
 * OURS: the index is clamped to the ramp. In the demo it is a
 * signed word that can run below it into the trail slots, which is
 * live data rather than a table. valley.txt 2.8. */
void Stills_DrawBlast(Relic_Frame *f, s32 cx, s32 cy, s32 r)
{
	const u8 *polar = Image_At(A_POLAR_TABLE);
	s32 step = g_spin & 0xffL;
	s32 top, bottom, dy, row, height;

	if (!polar || r <= 0) return;
	if (r + cy <= 0 || cy - r >= ST_H) return;
	if (r + cx <= 0 || cx - r >= ST_W) return;

	bottom = cy + r;
	if (bottom >= ST_H) bottom = ST_H - 1L;
	top = cy - r;
	dy = -r;
	if (top < 0) { dy -= top; top = 0; }
	height = bottom - top - 1L;

	for (row = 0; row <= height; row++, dy++) {
		s32 d = (dy < 0) ? dy : dy + 1L;
		s32 half = Gfx_Isqrt(r * r - d * d);
		s32 left, right, dx, col, width, off;
		s32 py0 = FX_MUL(top + row, f->scale);
		s32 py1 = FX_MUL(top + row + 1L, f->scale);

		right = cx + half;
		if (right >= ST_W) right = ST_W - 1L;
		left = cx - half;
		dx = -half;
		if (left < 0) { dx -= left; left = 0; }
		width = right - left - 1L;
		off = (((dy & 0xffL) << 8) | (dx & 0xffL)) * 2L;

		for (col = 0; col <= width; col++, off += 2L) {
			s32 w = (s32)RD_U16(polar
			                    + ((off + 0x10000L) & 0x1ffffL));
			s32 v = (s32)Gfx_BlobFalloff(r, w & 0xffL);
			s32 a = ((w >> 8) + step) & 0xffL;
			const u8 *c;
			s32 x, px, px0, px1, py;

			v -= g_rays[a];
			a = (a + step) & 0xffL;
			v -= g_rays[a];
			/* ORIGIN 0x00315604 - the ramp index is NOT clamped;
			 * the memory past the table's 256 entries is zero, so
			 * running off the end adds nothing [observed]. Pinning
			 * it to the last entry instead paints the rim. */
			if (v < 0 || v > 0xffL) continue;
			c = g_bramp + (size_t)v * PX_BYTES;
			if (!c[0] && !c[1] && !c[3]) continue;
			x = left + col;
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1L, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;
					int k;

					if (px < 0) continue;
					for (k = 0; k < PX_BYTES; k++) {
						s32 t = p[k] + c[k];

						p[k] = (u8)(t > 255L ? 255L : t);
					}
				}
			}
		}
	}
}

/* ORIGIN 0x00315e20 Valley_DrawHaze - a perspective plasma on the
 * ground, LIGHT ONLY. Both the impact and the aftermath call it.
 *
 * IT WALKS UPWARD. A0 starts at frame row 49 and each row advances
 * 0x280 then subtracts 0x500, which is one row UP; the height map
 * walks with it. The row count is 0xaa0000 over the caller's term,
 * clamped under 0x32, so a nearer horizon draws fewer rows.
 *
 * The texture is FPLASMA at +0x20200 - the same chunk and the same
 * bias the debris scene's Texture_GlowLight reads. Two samples per
 * texel, the first SWAPPED before the second is added, and only
 * the low byte reaches LIGHT. valley.txt 2.10. */
void Stills_DrawHaze(Relic_Frame *f, s32 term)
{
	const u8 *map = g_map;
	const u8 *tex;
	s32 d4, rows, row, pass, thresh = g_thresh >> 2;
	s32 step_col, row_acc, row_step, col0;

	tex = g_plasma;
	if (!map || !tex) return;
	if (term < 0x14L) term = 0x14L;
	d4 = 0xaa0000L / term;
	if (d4 <= 0) return;
	col0 = -0xc0000L / d4;
	row_step = 0x640000L / d4;
	step_col = (s32)(((u32)(0x320000L / d4) >> 8)
	                 | ((u32)(0x320000L / d4) << 24));
	rows = d4 >> 8;
	if (rows >= ST_HAZE_ROWS) rows = ST_HAZE_ROWS - 1L;

	/* ORIGIN 0x00315f04 - the SECOND pass is the first mirrored
	 * about the horizon: the same count of rows downward from row
	 * 50, the row accumulator climbing where the first one fell.
	 * Both are gated on the same map, so the plasma stops at the
	 * burn front rather than at a row. */
	for (pass = 0; pass < 2L; pass++)
	for (row = 0, row_acc = 0; row < rows;
	     row++, row_acc += pass ? row_step : -row_step) {
		s32 dy = pass ? ST_HAZE_ROW0 + 1L + row : ST_HAZE_ROW0 - row;
		u32 acc = (u32)col0;
		s32 col, py0, py1;

		if (dy < 0 || dy >= ST_H) break;
		py0 = FX_MUL(dy, f->scale);
		py1 = FX_MUL(dy + 1L, f->scale);
		for (col = 0; col < ST_W; col++) {
			/* ORIGIN 0x00315e7a - the gate's map pointer carries
			 * the scroll, so the haze tracks the pan. */
			s32 h = map[(dy + g_scroll) * ST_W + col];
			u32 prev;
			s32 i0, i1, add, px, px0, px1, py;

			if (h >= thresh) {
				i0 = (s32)(s16)(u16)(((u32)row_acc & 0xff00UL)
				                     | (acc & 0xffUL));
				i1 = (s32)(s16)(u16)((u32)(i0 + g_hazeoff));
				add = (s32)((((RD_U32(tex + ST_PLASMA_BIAS
				                      + i0 * 4L) >> 16)
				             + RD_U32(tex + ST_PLASMA_BIAS
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
							pp[PX_LIGHT] =
							    (u8)((pp[PX_LIGHT] + add) & 0xffL);
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

/* ORIGIN 0x00315b5a Valley_ImpactBase - phase three's base pass,
 * which REPLACES Valley_BlitBase rather than following it.
 *
 * TWO THINGS AT ONCE. Where the baked height map clears the
 * threshold the picture is HALVED and a colour from the splat
 * table added, which is the scorched lens on the ground. And every
 * pixel takes or.w of 0x0033b080 - whose high byte lands on LIGHT,
 * so the whole frame gets a light FLOOR that whitens it.
 *
 * The floor is clamp(0x0033b07c >> 5, >= 0x12), and 0x0033b07c
 * counts down, so the frame fades back as the blast shrinks. */
void Stills_ImpactBase(Relic_Frame *f)
{
	const u8 *map = g_map;
	s32 floor = g_blast >> 5;
	s32 x, y;

	if (floor < 0x12L) floor = 0x12L;
	for (y = 0; y < f->h; y++) {
		u8 *q = f->px + (size_t)(y * f->w) * PX_BYTES;
		s32 sy = FX_DIV(y, f->scale);

		if (sy < 0) sy = 0;
		if (sy >= ST_H) sy = ST_H - 1L;
		for (x = 0; x < f->w; x++, q += PX_BYTES) {
			s32 sx = FX_DIV(x, f->scale);
			const u8 *src;
			s32 at, h, k;

			if (sx < 0) sx = 0;
			if (sx >= ST_W) sx = ST_W - 1L;
			at = sy * ST_W + sx;
			src = g_pic + (size_t)at * PX_BYTES;
			h = map ? ((s32)map[at] - (g_thresh >> 2)) : -1L;
			if (h >= 0) {
				const u8 *c = g_splat + (size_t)(h & 0xffL) * PX_BYTES;

				for (k = 0; k < PX_BYTES; k++) {
					s32 t = (src[k] >> 1) + c[k];

					q[k] = (u8)(t > 255L ? 255L : t);
				}
			} else {
				q[0] = src[0]; q[1] = src[1];
				q[2] = src[2]; q[3] = src[3];
			}
			q[PX_LIGHT] = (u8)(q[PX_LIGHT] | floor);
		}
	}
}

/* ORIGIN 0x00314a9e Valley_SpawnTrail - REFUSES WHILE THE RADIUS IS
 * ABOVE 0x7f, which is what stops a trail being laid before the
 * rock is on screen. First free of 0x1f slots. */
void Stills_SpawnTrail(s32 x, s32 y, s32 r)
{
	s32 i;

	if (r > 0x7fL) return;
	for (i = 0; i < 31L; i++) if (!g_trail[i][0]) break;
	if (i == 31L) return;
	g_trail[i][0] = 0x4b;
	g_trail[i][1] = (s16)x;
	g_trail[i][2] = (s16)y;
	g_trail[i][3] = (s16)r;
}

/* ORIGIN 0x00314acc Valley_DrawTrail - 0x1e slots walked, the
 * lifetime AND the size counted down, and the slot dropped the
 * moment the size reaches zero rather than the lifetime.
 *
 * ORIGIN 0x00314b1e for the puff itself: the same skeleton as the
 * rock's WITHOUT the two turbulence terms, through its own ramp. */
void Stills_DrawTrail(Relic_Frame *f)
{
	s32 i;

	for (i = 0; i < 30L; i++) {
		s32 r;

		if (!g_trail[i][0]) continue;
		g_trail[i][0]--;
		g_trail[i][3]--;
		if (g_trail[i][3] <= 0) continue;
		r = (g_trail[i][3] >> 2) + Stills_Rand(2L);
		Stills_Blob(f, g_trail[i][1], g_trail[i][2], r, g_tramp, 0);
	}
}

/* ORIGIN 0x0031636c Valley_BlurFrame - five spatial taps AND the
 * destination's PREVIOUS contents, each folded in by halves:
 *
 *   (((up+down)/2 + (left+right)/2)/2 + centre)/2 + previous)/2
 *
 * so the scene carries a temporal smear, not just a blur.
 * valley.txt 2.4. */
void Stills_Blur(Relic_Frame *f)
{
	s32 n = f->w * f->h, i, w = f->w;
	u32 left, cur;

	if (stills_blur_fit(n)) return;
	left = RD_U32(f->px);
	cur = left;
	for (i = 0; i < n; i++) {
		u8 *p = f->px + (size_t)i * PX_BYTES;
		u8 *d = g_prev + (size_t)i * PX_BYTES;
		u32 up = (i >= w) ? RD_U32(p - (size_t)w * PX_BYTES) : cur;
		u32 dn = (i < n - w) ? RD_U32(p + (size_t)w * PX_BYTES) : cur;
		u32 right = (i < n - 1L) ? RD_U32(p + PX_BYTES) : cur;
		u32 v = stills_mean(stills_mean(stills_mean(up, dn), stills_mean(left, right)),
		                cur);

		v = stills_mean(v, RD_U32(d));
		d[0] = (u8)(v >> 24); d[1] = (u8)(v >> 16);
		d[2] = (u8)(v >> 8);  d[3] = (u8)v;
		left = cur;
		cur = right;
	}
	memcpy(f->px, g_prev, (size_t)n * PX_BYTES);
}

static void st_spawn(s32 at, s32 col, s32 row, const u8 *src);

/* ORIGIN 0x00315c34 City_BaseAndSpawn, its base half. Unlike the
 * valley's Stills_ImpactBase this crossfades TWO pictures: the
 * intact still where the front has not reached and the scorched one
 * under the splat colour where it has. effects/city.txt 4.
 *
 * [open] the spawn half - see stills.h. */
void Stills_CityBase(Relic_Frame *f, const u8 *mask)
{
	s32 thresh = g_thresh >> 2;
	s32 band, grp, sx, y;

	if (!g_pic || !g_map) { Frame_Clear(f); return; }
	/* ORIGIN 0x00315c84 - 0x00315da0 keeps the PREVIOUS frame's cut
	 * and the band is how far it moved, so only the newly exposed
	 * strip spawns rather than the whole burnt area. */
	band = g_cut - thresh;
	g_cut = thresh;
	for (y = 0; y < ST_H; y++) {
		s32 sy = y + g_scroll;
		s32 py0 = FX_MUL(y, f->scale);
		s32 py1 = FX_MUL(y + 1L, f->scale);

		if (sy < 0) sy = 0;
		for (grp = 0; grp < 5L; grp++) {
			/* ORIGIN 0x00315caa - one mask longword a GROUP of
			 * 0x20 columns. */
			u32 bits = mask
			         ? RD_U32(mask + (size_t)(sy * 0x14L + grp * 4L))
			         : 0;

			for (sx = grp * 0x20L; sx < (grp + 1L) * 0x20L; sx++) {
				s32 at = sy * ST_W + sx;
				s32 h = (s32)g_map[at] - thresh;
				const u8 *src = g_pic + (size_t)at * PX_BYTES;
				u8 col[PX_BYTES];
				s32 k, px, py, px0, px1;

				if (h < 0) {
					col[0] = src[0]; col[1] = src[1];
					col[2] = src[2]; col[3] = src[3];
				} else {
					const u8 *s = g_splat + (size_t)h * PX_BYTES;
					const u8 *u = g_layer2
					            ? g_layer2 + (size_t)at * PX_BYTES
					            : src;

					for (k = 0; k < PX_BYTES; k++) {
						s32 t = s[k] + u[k];

						col[k] = (u8)(t > 255L ? 255L : t);
					}
				}
				px0 = FX_MUL(sx, f->scale);
				px1 = FX_MUL(sx + 1L, f->scale);
				for (py = py0; py < py1 && py < f->h; py++) {
					u8 *q;

					if (py < 0) continue;
					q = f->px + (size_t)(py * f->w) * PX_BYTES;
					for (px = px0; px < px1 && px < f->w; px++) {
						u8 *o = q + (size_t)px * PX_BYTES;

						if (px < 0) continue;
						o[0] = col[0]; o[1] = col[1];
						o[2] = col[2]; o[3] = col[3];
					}
				}
				/* ORIGIN 0x00315cc4 - THE MASK BIT IS CONSUMED ONLY
				 * BY A CANDIDATE. roxl.l sits after both height
				 * tests, so the bits are a stream over candidates
				 * rather than a picture-shaped stencil: within a
				 * group the first candidate takes bit 31, the next
				 * bit 30, and a column that fails the height takes
				 * none. effects/city.txt 4. */
				if (h < 0x12L || h - 0x12L >= band) continue;
				{
					u32 bit = bits >> 31;

					bits <<= 1;
					if (bit) st_spawn(y * ST_W + sx, sx, y, src);
				}
			}
		}
	}
}

/* ORIGIN 0x003159ce - the shape jump table, 0x3c longwords. The
 * eight arms it can reach are at these addresses and each writes a
 * different footprint; the table maps a selector onto them and IS
 * NOT MONOTONIC, so a particle's shape jumps about as it ages
 * rather than shrinking cleanly. effects/city.txt 4. */
#define ST_JUMP_TABLE   0x003159ceUL
#define ST_SHAPES       0x3cL
static const u32 ST_ARM[8] = {
	0x00315908UL, 0x0031591cUL, 0x0031592cUL, 0x00315936UL,
	0x0031593cUL, 0x0031594cUL, 0x00315958UL, 0x00315964UL
};
/* Each arm's footprint as (dx, dy) pairs, terminated by -1. */
static const s8 ST_FOOT[8][9] = {
	{ 0,0, 1,0, 0,1, 1,1, -1 },   /* 0x315908  2 x 2            */
	{ 0,0, 1,0, 0,1, -1 },        /* 0x31591c  an L             */
	{ 0,0, 1,0, -1 },             /* 0x31592c  a pair across    */
	{ 0,0, -1 },                  /* 0x315936  one              */
	{ 0,0, 1,0, 1,1, -1 },        /* 0x31593c  the other L      */
	{ 0,0, 1,1, -1 },             /* 0x31594c  a diagonal pair  */
	{ 0,0, 1,1, -1 },             /* 0x315958  the same         */
	{ 0,0, -1 }                   /* 0x315964  one              */
};
static u8 g_shape[ST_SHAPES];

/* ORIGIN 0x003142bc - the selector table, (Stills_Rand(0x2c) + 0x10)
 * doubled, so 0x20 to 0x76 even. The draw halves it to index the
 * jump table and takes one off it a draw. */
#define ST_SEL_N        256L
static s16 g_sel[ST_SEL_N];
static s32 g_selcur;                /* 0x00315d9c                   */

static s32 *g_vel;

typedef struct St_Node {
	struct St_Node *next, *prev;
	s32 x, y, vx, vy;
	s32 sel;
	u8 col[PX_BYTES];
} St_Node;

static St_Node *g_pool;
static St_Node g_live, g_free;
static s32 g_live_n;

/* OURS - the demo splices the list inline at each site. */
static void st_push(St_Node *h, St_Node *n)
{
	n->next = h->next; n->prev = h;
	h->next->prev = n; h->next = n;
}

/* OURS */
static void st_unlink(St_Node *n)
{
	n->prev->next = n->next; n->next->prev = n->prev;
}

#define ST_VEL_ROWS     0x99L

/* ORIGIN 0x0031571a, 0x003156b6 and 0x00315796 - one builder a shot,
 * rebuilt at every transition. SHOTS ONE AND THREE RADIATE FROM A
 * CORNER: the origin sits 0x14 rows above the picture and either one
 * frame width to its right or on its left edge, the two mirroring
 * each other the way their pictures do, and THE SPEED GROWS WITH THE
 * DISTANCE - dx * dist, not dx / dist - so the far city is thrown
 * hardest. Shot two, whose camera is on the ground, radiates from
 * the frame's centre on a flat gradient instead.
 *
 * Shots one and three fill the PICTURE's 153 rows; shot two fills
 * the frame's 100. Only the first 100 are ever read, since the draw
 * indexes the table at the screen texel and does not scroll it. */
int Stills_BuildVelocities(s32 shot)
{
	s32 rows = (shot == 1L) ? ST_H : ST_VEL_ROWS;
	s32 r, c;

	free(g_vel);
	g_vel = (s32 *)malloc((size_t)(ST_VEL_ROWS * ST_W) * 2L * sizeof(s32));
	if (!g_vel) return 1;
	memset(g_vel, 0, (size_t)(ST_VEL_ROWS * ST_W) * 2L * sizeof(s32));
	for (r = 0; r < rows; r++)
		for (c = 0; c < ST_W; c++) {
			s32 *v = g_vel + (size_t)(r * ST_W + c) * 2L;

			if (shot == 1L) {
				v[0] = ((c - 0x50L) << 2) + Stills_Rand(0xc8L) - 0x64L;
				v[1] = ((r - 0x32L) << 1) + Stills_Rand(0x64L) - 0x32L;
			} else {
				s32 dx = (shot == 0L) ? c - ST_W : c;
				s32 dy = r + 0x14L;
				s32 d = Gfx_Isqrt(dx * dx + dy * dy);

				v[0] = ((dx * d) >> 5) + Stills_Rand(0x64L) - 0x32L;
				v[1] = ((dy * d) >> 6) + Stills_Rand(0x64L) - 0x32L;
			}
		}
	return 0;
}

/* ORIGIN 0x003142bc City_BuildSelectorTable - the selector half. The
 * pool, and reading the eight arms back out of the jump table rather
 * than transcribing them, are OURS. */
int Stills_ParticlesInit(void)
{
	const u8 *jt = Image_At(ST_JUMP_TABLE);
	s32 i, c;

	if (!jt) return 1;
	for (i = 0; i < ST_SHAPES; i++) {
		u32 a = RD_U32(jt + (size_t)i * 4L);

		g_shape[i] = 7;
		for (c = 0; c < 8L; c++)
			if ((a & 0xffffffUL) == ST_ARM[c]) { g_shape[i] = (u8)c; break; }
	}
	for (i = 0; i < ST_SEL_N; i++)
		g_sel[i] = (s16)((Stills_Rand(0x2cL) + 0x10L) * 2L);
	g_selcur = 0;

	if (Stills_BuildVelocities(0L)) return 1;

	free(g_pool);
	g_pool = (St_Node *)malloc((size_t)ST_PART_MAX * sizeof(St_Node));
	if (!g_pool) return 1;
	Stills_ResetParticles();
	return 0;
}

/* ORIGIN 0x00315ace City_ResetParticles - re-links all 0x3a98 nodes
 * into the free list, empties the active one and zeroes the count.
 *
 * EVERY SHOT ENTRY CALLS IT - 0x003145c8, 0x0031468e and 0x003147ac,
 * as does the valley's setup at 0x00314360. Without it the previous
 * shot's debris is still falling through the next one. */
void Stills_ResetParticles(void)
{
	s32 i;

	if (!g_pool) return;
	g_live.next = g_live.prev = &g_live;
	g_free.next = g_free.prev = &g_free;
	for (i = 0; i < ST_PART_MAX; i++) st_push(&g_free, &g_pool[i]);
	g_live_n = 0;
}

s32 Stills_ParticleCount(void) { return g_live_n; }

/* ORIGIN 0x00315cea - the spawn half of City_BaseAndSpawn. */
static void st_spawn(s32 at, s32 col, s32 row, const u8 *src)
{
	St_Node *n;

	if (g_live_n >= ST_PART_MAX || g_free.next == &g_free) return;
	n = g_free.next;
	st_unlink(n);
	st_push(&g_live, n);
	/* ORIGIN 0x00315cfc - x and y are 24.8, and y is stored with
	 * the scroll already in it so the pan carries the particle. */
	n->x = col << 8;
	n->y = (row + g_scroll) << 8;
	n->vx = g_vel[(size_t)at * 2L];
	n->vy = g_vel[(size_t)at * 2L + 1L];
	n->sel = g_sel[g_selcur & (ST_SEL_N - 1L)];
	g_selcur++;
	n->col[0] = src[0]; n->col[1] = src[1];
	n->col[2] = src[2]; n->col[3] = src[3];
	g_live_n++;
}

/* ORIGIN 0x0031589e City_DrawParticles - plot, step, age, recycle. */
void Stills_DrawParticles(Relic_Frame *f)
{
	St_Node *n = g_live.next;

	while (n != &g_live) {
		St_Node *nx = n->next;
		s32 x = n->x >> 8;
		s32 y = (n->y - (g_scroll << 8)) >> 8;
		s32 i, sh;

		if (x < 0 || x >= ST_W - 1L || y < 0 || y >= ST_H - 2L) goto kill;
		i = n->sel >> 1;
		sh = g_shape[i < ST_SHAPES ? i : ST_SHAPES - 1L];
		for (i = 0; ST_FOOT[sh][i] >= 0; i += 2) {
			s32 px0 = FX_MUL(x + ST_FOOT[sh][i], f->scale);
			s32 px1 = FX_MUL(x + ST_FOOT[sh][i] + 1L, f->scale);
			s32 py0 = FX_MUL(y + ST_FOOT[sh][i + 1], f->scale);
			s32 py1 = FX_MUL(y + ST_FOOT[sh][i + 1] + 1L, f->scale);
			s32 px, py;

			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;

					if (px < 0) continue;
					p[0] = n->col[0]; p[1] = n->col[1];
					p[2] = n->col[2]; p[3] = n->col[3];
				}
			}
		}
		/* ORIGIN 0x00315968 - x drags, y does not. */
		n->x += n->vx;
		n->vx -= n->vx >> 3;
		n->y += n->vy;
		if (--n->sel >= 0) { n = nx; continue; }
kill:
		st_unlink(n);
		st_push(&g_free, n);
		g_live_n--;
		n = nx;
	}
}
