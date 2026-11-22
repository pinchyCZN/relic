/* RELIC - the shared drawing primitives. See gfx.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "image.h"

u8 g_RampBolt[256];
u8 g_RampSpark[256];
u8 g_RampBeamHead[256];
u8 g_RampBeamHead[256];
u8 g_RampBoltHalf[256];
u8 g_LightSat[256];

/* ORIGIN 0x002c9cec - ONE BLOCK, AND ITS PADDING IS PART OF THE
 * EFFECT: Draw_Flare's ramp index is a signed word that leaves
 * 0..255 at both ends [0x002d4532], and the six ramps sit 0x4a0
 * apart with 0x400 of each filled, so forty longwords of zero
 * follow every one and forty of white precede the first. Six
 * arrays cannot hold that. planet_sun.txt 6. */
#define RAMP_STRIDE  296               /* 0x4a0 bytes of longwords */
#define RAMP_SLOTS     6
#define RAMP_BYTES   (RAMP_STRIDE * PX_BYTES)
#define RAMP_SLOT(n) (g_ramp_block + RAMP_PAD * PX_BYTES + (n) * RAMP_BYTES)

static u8 g_ramp_block[RAMP_PAD * PX_BYTES + RAMP_SLOTS * RAMP_BYTES];

u8 *g_ColRampWarmB   = RAMP_SLOT(0);
u8 *g_ColRampWarm    = RAMP_SLOT(1);
u8 *g_ColRampAmber   = RAMP_SLOT(2);
u8 *g_ColRampGlow    = RAMP_SLOT(3);
u8 *g_ColRampMagenta = RAMP_SLOT(4);
u8 *g_ColRampPale    = RAMP_SLOT(5);

static const u8 *g_polar;       /* 65536 words at A_POLAR_TABLE */
static const u8 *g_falloff;     /* [radius << 8 | dist]         */
static s32 g_base_r = 0x3f;
static s32 g_base_g = 0x4f;
static s32 g_base_b = 0x6f;
static u32 g_rand_state;
/* ORIGIN 0x002e0c1d - g_ApproachSpin's low byte, read twice by
 * Draw_Flare, so the rays turn as the view does. */
static s32 g_flare_ray_phase;

/* ORIGIN 0x002d4452 - the byte Draw_Flare adds to the polar angle
 * twice. It is the low byte of the word at 0x002e0c1c, which
 * Scene_ProjectAndDrawSun writes as the camera angle >> 12, so the
 * rays turn as the camera does. */
void Gfx_SetFlareRayPhase(s32 phase)
{
	g_flare_ray_phase = phase & 0xffL;
}

/* ORIGIN 0x002c9fba  Ramp_BuildLinear:
 *     v = min(0x3f, (0xff - i) >> 1)
 * Flat at full brightness out to i = 129, then straight to zero. */
static void ramp_linear(u8 *out)
{
	int i;
	for (i = 0; i < 256; i++) {
		int v = (0xff - i) >> 1;
		out[i] = (u8)(v > LIGHT_MAX ? LIGHT_MAX : v);
	}
}

/* ORIGIN 0x002ca00e  Ramp_BuildInverseSquare:
 *     t = (i * d3) >> 7
 *     v = min(0x3f, ((d7 - t*t) / (t*t)) >> 4)        t != 0
 * The divide truncates toward zero, as divsl.l does. */
static void ramp_inverse_square(u8 *out, s32 d3)
{
	int i;
	const s32 d7 = 0x50ffffL;
	for (i = 0; i < 256; i++) {
		s32 t = ((s32)i * d3) >> 7;
		s32 t2 = t * t;
		s32 v = (t2 == 0) ? d7 : ((d7 - t2) / t2);
		v >>= 4;
		if (v < 0) v = 0;
		out[i] = (u8)(v > LIGHT_MAX ? LIGHT_MAX : v);
	}
}

/* OURS - wiring. The tables it builds carry their own ORIGIN. */
int Gfx_Init(void)
{
	const u8 *pad;
	int i;

	g_polar = Image_At(A_POLAR_TABLE);
	g_falloff = Image_At(A_BLOB_FALLOFF);
	pad = Image_At(A_RAMP_PAD);
	if (!g_polar || !g_falloff || !pad) {
		fprintf(stderr, "gfx: the polar or falloff table is missing\n");
		return 1;
	}
	memcpy(g_ramp_block, pad, RAMP_PAD * PX_BYTES);
	ramp_linear(g_RampBolt);
	ramp_inverse_square(g_RampSpark, 0x12cL);
	/* ORIGIN 0x002c9e74 - Ramp_BuildAll gives the beam's head the
	 * same inverse square with a tighter divisor, so its glow falls
	 * off faster than a spark's over the same radius. */
	ramp_inverse_square(g_RampBeamHead, 0x96L);
	ramp_inverse_square(g_RampBeamHead, 0x96L);
	for (i = 0; i < 256; i++) g_RampBoltHalf[i] = (u8)(g_RampBolt[i] >> 1);
	/* ORIGIN 0x002cff1e  Ramp_BuildSaturate */
	for (i = 0; i < 256; i++)
		g_LightSat[i] = (u8)(i > LIGHT_MAX ? LIGHT_MAX : i);
	g_rand_state = 0;
	g_flare_ray_phase = 0;
	Gfx_BuildRays();
	/* ORIGIN 0x002c9cec Ramp_BuildAll - the colours and falloffs it
	 * hands to Draw_Flare. */
	Gfx_BuildColourRamp(g_ColRampAmber,   0x3f, 0x2f, 0x20, 0x780000L);
	Gfx_BuildColourRamp(g_ColRampMagenta, 0x3f, 0x2f, 0x3f, 0x500000L);
	Gfx_BuildColourRamp(g_ColRampPale,    0x7f, 0x7f, 0xaf, 0x960000L);
	Gfx_BuildColourRamp(g_ColRampWarm,    0x3f, 0x34, 0x2d, 0x500000L);
	/* 0x002eeaf0: the same ramp, then one blue hump. The second flare
	 * uses this one and the first uses the plain warm ramp. */
	Gfx_BuildColourRamp(g_ColRampWarmB,   0x3f, 0x34, 0x2d, 0x500000L);
	Gfx_RampBlueBump(g_ColRampWarmB, 0xaa);
	/* 0x002ef8d0: blue at 0x3f against green at 0x2f is what makes the
	 * portal's field purple, and the hump is applied TWICE. */
	Gfx_BuildColourRamp(g_ColRampGlow,    0x3f, 0x2f, 0x3f, 0x6e0000L);
	Gfx_RampBlueBump(g_ColRampGlow, 0x96);
	Gfx_RampBlueBump(g_ColRampGlow, 0x96);
	return 0;
}

/* OURS - gfx.h */
s32 Gfx_Word(s32 v)
{
	v &= 0xffffL;
	return (v & 0x8000L) ? v - 0x10000L : v;
}

/* ORIGIN 0x00228b6c - the sin table itself */
s32 Gfx_Sin(s32 angle)
{
	return (s32)Image_U32(A_SIN_TABLE + (u32)((angle & ANGLE_MASK) * 4));
}

/* ORIGIN 0x0022ab6c - the same table, a quarter on */
s32 Gfx_Cos(s32 angle)
{
	return (s32)Image_U32(A_COS_TABLE + (u32)((angle & ANGLE_MASK) * 4));
}

/* ORIGIN 0x0029f97a  g_PolarTable
 * The readers all do `lea 0x29f97a,An ; adda.l #0x10000,An` and then
 * index by a SIGNED word times two, so the centre of the table is
 * 0x10000 bytes in. */
static const u8 *polar_word(s32 dx, s32 dy)
{
	u32 u = (((u32)dy & 0xffUL) << 8) | ((u32)dx & 0xffUL);
	u32 off = ((u << 1) + 0x10000UL) & 0x1ffffUL;
	return g_polar + off;
}

u8 Gfx_PolarDist(s32 dx, s32 dy)  { return polar_word(dx, dy)[1]; }

/* ORIGIN 0x002bf97a g_BlobFalloff, for the callers that index it
 * with a radius and a distance of their own. */
u8 Gfx_BlobFalloff(s32 radius, s32 dist)
{
	if (dist > 0xffL) dist = 0xffL;
	return g_falloff[((radius & 0x7fL) << 8) | dist];
}
u8 Gfx_PolarAngle(s32 dx, s32 dy) { return polar_word(dx, dy)[0]; }

/* ORIGIN 0x002ca766  Math_Isqrt */
s32 Gfx_Isqrt(s32 n)
{
	s32 rem = 0, root = 0;
	int i;
	if (n <= 0) return 0;
	for (i = 0; i < 16; i++) {
		rem = (rem << 2) | ((n >> (30 - 2 * i)) & 3L);
		root <<= 1;
		if (rem > 2 * root) {
			rem -= 2 * root + 1;
			root += 1;
		}
	}
	return root;
}

void Gfx_SeedRand(u32 seed) { g_rand_state = seed; }

/* ORIGIN 0x002cfe38 - the demo's LCG */
s32 Gfx_Rand(s32 range)
{
	u32 d2, d3;
	g_rand_state = g_rand_state * 0x5d43UL + 0x5c2dUL;
	d2 = g_rand_state;
	d3 = (d2 >> 16) ^ (d2 & 0xffffUL);
	return (s32)(((u32)range * (d3 & 0xffffUL)) >> 16);
}

/* OURS - the render target.
 * ---------------------------------------------------------------- */

int Frame_Init(Relic_Frame *f, s32 w, s32 h, s32 scale)
{
	f->w = w;
	f->h = h;
	f->scale = scale;
	f->modes = RELIC_MODE_NONE;
	f->px = (u8 *)calloc((size_t)(w * h), PX_BYTES);
	return f->px ? 0 : 1;
}

/* OURS */
void Frame_Free(Relic_Frame *f)
{
	if (f->px) { free(f->px); f->px = NULL; }
}

/* OURS */
void Frame_Clear(Relic_Frame *f)
{
	memset(f->px, 0, (size_t)(f->w * f->h) * PX_BYTES);
}

/* ---------------------------------------------------------------- */

/* OURS
 * THE ROW SPAN MUST BE THE CIRCLE, NOT THE BOUNDING BOX. The demo
 * reads a half-width out of g_pSpanHalfWidth so a pixel outside the
 * circle is never touched at all. That is not an optimisation:
 * g_BlobFalloff returns 255 out there, and a COLOUR ramp's entry
 * 255 is dim but not zero, so walking the box lays a faint square
 * haze around every flare.
 *
 * Computing it as isqrt(r^2 - dy^2) is the same circle the table
 * holds, and it stays exact at any scale where a tabled half-width
 * would quantise. Returns non-zero when the row is empty.
 */
static int span_clip(s32 scale, s32 pcx, s32 radius, s32 dyf,
                     s32 *out0, s32 *out1, s32 lo, s32 hi)
{
	s32 rf = radius << DEMO_FRAC;
	s32 half, p0, p1;
	if (dyf < 0) dyf = -dyf;
	if (dyf > rf) return 1;
	half = Gfx_Isqrt(rf * rf - dyf * dyf);
	p0 = pcx - DEMO_F_TO_PX(half, scale);
	p1 = pcx + DEMO_F_TO_PX(half, scale);
	if (p0 < lo) p0 = lo;
	if (p1 > hi) p1 = hi;
	*out0 = p0;
	*out1 = p1;
	return p0 > p1;
}

/* ORIGIN 0x0029f97a - g_PolarTable's distance byte is
 * isqrt(dx*dx + dy*dy) + 3 for every one of its 65536 entries
 * [verified], so this IS the table, computed where it has no entry.
 * Demo units in and out, DEMO_FRAC fraction bits; exact at scale 1.
 * Why that is allowed: docs/effects/orb.txt 5. */
static s32 polar_dist_f(s32 dxf, s32 dyf)
{
	return Gfx_Isqrt(dxf * dxf + dyf * dyf) + (3 << DEMO_FRAC);
}

/* OURS - g_BlobFalloff read at a fractional distance, INTERPOLATED
 * ONLY ABOVE SCALE 1 so the reference render keeps the demo's own
 * single-byte lookup. */
static s32 falloff_f(s32 fbase, s32 distf, s32 scale)
{
	s32 i = distf >> DEMO_FRAC;
	s32 a;
	if (i > 0xff) return 0xff;
	a = (s32)g_falloff[fbase | i];
	if (scale > FX_ONE && i < 0xff) {
		s32 b = (s32)g_falloff[fbase | (i + 1)];
		a += ((b - a) * (distf & ((1 << DEMO_FRAC) - 1))) >> DEMO_FRAC;
	}
	return a;
}

/* ORIGIN 0x002cc4bc Turbulence_BuildTable - 1024 words of noise that
 * is NOT white: a random term four times as wide as either sine is
 * added to two sine walks taken at different rates, and the sum is
 * shifted down ten. So it has structure at two scales with grain on
 * top, which is why the flare churns instead of fizzing.
 *
 * ONLY 512 WORDS ARE EVER A CURSOR and the table is twice that long.
 * That is deliberate: a read reaches dist + angle/4 words past the
 * cursor, up to 318, so the second half is the overrun the wrap would
 * otherwise have to fold. Nothing masks the read index. */
#define TURB_WORDS   1024
#define TURB_WINDOW  512

static s16 g_turb[TURB_WORDS];
static s32 g_turb_a;            /* ORIGIN 0x002d509c, in WORDS */
static s32 g_turb_b;            /* ORIGIN 0x002d50a0, in WORDS */

void Gfx_BuildTurbulence(void)
{
	s32 i, d2 = 0, d3 = 0;

	for (i = 0; i < TURB_WORDS; i++) {
		s32 v = ((Gfx_Rand(0x1388L) - 0x9c4L) << 2) + Gfx_Sin(d2 & 0x1fffL);
		v >>= 10;
		v += Gfx_Sin(d3 & 0x1fffL) >> 10;
		g_turb[i] = (s16)v;
		d2 = (d2 & 0x1fffL) + 0x80L;
		d3 = (d3 & 0x1fffL) + 0x100L;
	}
	g_turb_a = 0;
	g_turb_b = 0;
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, at +0x2c2, so it runs once a
 * tick for the whole of the scene. THE TWO CURSORS MOVE OPPOSITE
 * WAYS AND AT DIFFERENT SPEEDS - one word forward, two words back -
 * so the pair never repeats its pairing until both wrap. */
void Gfx_StepTurbulence(void)
{
	g_turb_b -= 2;
	if (g_turb_b < 0) g_turb_b = TURB_WINDOW;
	g_turb_a += 1;
	if (g_turb_a >= TURB_WINDOW) g_turb_a = 0;
}

/* ORIGIN 0x002d466c - the ramp is indexed by a SIGNED word, so the
 * turbulence walks off both of its ends and what lies there is part
 * of the effect. Read live at 0x002f0c50: the 128 bytes below the
 * ramp hold 0x3f, which is its own saturated plateau, and the 256
 * above hold zero. So turbulence toward the centre cannot overbrighten
 * and turbulence at the rim cuts the glow off hard - the sharp edge
 * the demo's own frame shows. */
static u8 flare_ramp(const u8 *ramp, s32 i)
{
	if (i < 0) return LIGHT_MAX;
	if (i > 255) return 0;
	return ramp[i];
}

/* ORIGIN 0x002d466c  Draw_FlareTurbulent - the beam head's glow, and
 * NOT Draw_GlowBlob. The geometry is the same circle, but the ramp
 * index carries the two turbulence reads, and the result is added to
 * LIGHT as a PLAIN BYTE. Draw_GlowBlob saturates; this does not, so
 * two heads crossing push LIGHT past 0x3f and Ham8_BuildTables has
 * the levels to show it. The demo's frame reads 0x4b where ours
 * saturated flat at 0x3f.
 *
 * [open] Draw_Flare at 0x002d4452 is this routine's sibling and the
 * two differ in more than their tables; the ray tables it subtracts
 * are built but unused here. */
void Gfx_FlareTurbulent(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                        const u8 *ramp)
{
	s32 pcx, pcy, pr, x, y, x0, x1, y0, y1, sx0, sx1;
	s32 fbase;

	if (radius <= 0) return;
	pcx = FX_MUL(cx, f->scale);
	pcy = FX_MUL(cy, f->scale);
	pr  = FX_MUL(radius, f->scale);
	if (pr <= 0) pr = 1;
	fbase = (radius & 0x7fL) << 8;

	y0 = pcy - pr; if (y0 < 0) y0 = 0;
	y1 = pcy + pr; if (y1 > f->h - 1) y1 = f->h - 1;
	x0 = pcx - pr; if (x0 < 0) x0 = 0;
	x1 = pcx + pr; if (x1 > f->w - 1) x1 = f->w - 1;
	if (y0 > y1 || x0 > x1) return;

	for (y = y0; y <= y1; y++) {
		s32 dyf = PX_TO_DEMO_F(y - pcy, f->scale);
		s32 dy = dyf >> DEMO_FRAC;
		u8 *row = f->px + (size_t)(y * f->w) * PX_BYTES;
		if (span_clip(f->scale, pcx, radius, dyf, &sx0, &sx1, x0, x1))
			continue;
		for (x = sx0; x <= sx1; x++) {
			s32 dxf = PX_TO_DEMO_F(x - pcx, f->scale);
			s32 distf = polar_dist_f(dxf, dyf);
			/* THE TURBULENCE STAYS ON THE DEMO'S OWN GRID. Its two
			 * tables are word arrays indexed by a whole distance, and
			 * the churn is noise - resampling it would smooth out the
			 * grain that is the point of it. Only the falloff, which
			 * is the smooth part, is taken between entries. */
			s32 dist = distf >> DEMO_FRAC;
			s32 ang = (s32)Gfx_PolarAngle(dxf >> DEMO_FRAC, dy) >> 2;
			s32 i = falloff_f(fbase, distf, f->scale);
			u8 add;
			i += (s32)g_turb[g_turb_a + dist];
			i += (s32)g_turb[g_turb_b + dist + ang];
			add = flare_ramp(ramp, i);
			if (add) {
				u8 *p = row + (size_t)x * PX_BYTES;
				p[PX_LIGHT] = (u8)(p[PX_LIGHT] + add);
			}
		}
	}
}

/* ORIGIN 0x002cefa4 Draw_GlowBlob, as a buffer rather than a frame.
 * CODE_06's Temple_DrawGlowBlob shares the skeleton but adds a
 * colour longword to the pixel where this adds to LIGHT. Both are
 * radially symmetric - temple.txt. */
void Gfx_GlowBlobTo(u8 *dst, s32 w, s32 h, s32 scale,
                    s32 cx, s32 cy, s32 radius, const u8 *ramp)
{
	s32 pcx, pcy, pr, x, y, x0, x1, y0, y1, sx0, sx1;
	s32 fbase;

	if (radius <= 0) return;
	pcx = FX_MUL(cx, scale);
	pcy = FX_MUL(cy, scale);
	pr  = FX_MUL(radius, scale);
	if (pr <= 0) pr = 1;

	/* THE TABLE INDEX STAYS IN DEMO UNITS - scaling it would run off
	 * the end of g_BlobFalloff; the pixel offset is divided back down
	 * instead. The row is masked to a BYTE, not to 0x7f: the demo
	 * builds the index with a word shift, so rows run 0 to 255 and
	 * wrap there [verified, 0x002cf024]. Every caller clamps the
	 * radius below 128 anyway, so the two spellings differ only for a
	 * radius no part asks for. */
	fbase = (radius & 0xffL) << 8;

	y0 = pcy - pr; if (y0 < 0) y0 = 0;
	y1 = pcy + pr; if (y1 > h - 1) y1 = h - 1;
	x0 = pcx - pr; if (x0 < 0) x0 = 0;
	x1 = pcx + pr; if (x1 > w - 1) x1 = w - 1;
	if (y0 > y1 || x0 > x1) return;

	for (y = y0; y <= y1; y++) {
		s32 dyf = PX_TO_DEMO_F(y - pcy, scale);
		u8 *row = dst + (size_t)(y * w) * PX_BYTES;
		if (span_clip(scale, pcx, radius, dyf, &sx0, &sx1, x0, x1))
			continue;
		for (x = sx0; x <= sx1; x++) {
			s32 dxf = PX_TO_DEMO_F(x - pcx, scale);
			s32 distf = polar_dist_f(dxf, dyf);
			u8 add = ramp[falloff_f(fbase, distf, scale)];
			if (add) {
				u8 *p = row + (size_t)x * PX_BYTES;
				p[PX_LIGHT] = g_LightSat[p[PX_LIGHT] + add];
			}
		}
	}
}

/* ORIGIN 0x002cefa4 Draw_GlowBlob - the frame's own call. */
void Gfx_GlowBlob(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                  const u8 *ramp)
{
	Gfx_GlowBlobTo(f->px, f->w, f->h, f->scale, cx, cy, radius, ramp);
}

/* ORIGIN 0x002d4552 Draw_MeteorStreak - the streak an impact wears
 * early in its life. It ADDS TO LIGHT AND NOTHING ELSE.
 *
 * THE POLAR TABLE IS WALKED LINEARLY, not sampled per (dx, dy): the
 * pointer starts at (dy << 8) | dx and advances 2 bytes plus `step`
 * a column, so a non-zero step runs through the table faster than
 * the span does and stretches the shape sideways. It also HALVES
 * the row's half width. Clamping the walk per row would square the
 * streak off - the overrun into the next row is the effect.
 *
 * `slant` is the caller's own D5 in 8.8, added to the centre every
 * row, which is what leans the streak.
 *
 * The walk starts at the UNCLIPPED left edge, because clipping it
 * would shift the whole row's samples. debris.txt 2.3.1. */
void Gfx_MeteorStreak(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                      s32 step, s32 slant, const u8 *ramp)
{
	s32 y, y0, y1, cx_fx, fade;

	if (radius <= 0 || !ramp || !g_polar) return;
	if (cy + radius <= 0 || cy - radius >= 0x64L) return;
	if (cx + radius <= 0 || cx - radius >= 0xa0L) return;

	fade = (radius * 2L) & 0xffL;
	cx_fx = cx << 8;
	y0 = cy - radius;
	y1 = cy + radius; if (y1 > 0x63L) y1 = 0x63L;

	for (y = y0; y <= y1; y++, cx_fx += slant) {
		s32 dy = y - cy;
		s32 d = (dy < 0) ? dy : dy + 1L;
		s32 half = Gfx_Isqrt(radius * radius - d * d);
		s32 cxr = cx_fx >> 8;
		s32 x, x0, x1, off, py0, py1, py;

		if (y < 0) continue;
		if (step) half >>= 1;
		x0 = cxr - half;
		x1 = cxr + half; if (x1 > 0x9fL) x1 = 0x9fL;
		off = (((dy & 0xffL) << 8) | ((-half) & 0xffL)) * 2L;
		if (x0 < 0) { off += (0L - x0) * (2L + step); x0 = 0; }
		py0 = FX_MUL(y, f->scale);
		py1 = FX_MUL(y + 1, f->scale);

		for (x = x0; x <= x1; x++, off += 2L + step) {
			s32 dist = g_polar[(off + 0x10001L) & 0x1ffffL];
			s32 v = (s32)Gfx_BlobFalloff(radius, dist);
			s32 add, px, px0, px1;

			add = (ramp[v * PX_BYTES + 3] >> 2) - fade;
			if (add <= 0) continue;
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1, f->scale);
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

/* ORIGIN 0x002d4780 Space_DrawLightBlob - THE SWIRL, and it is a
 * glow blob with two things done to it.
 *
 * IT IS AN ELLIPSE. The vertical half-extent is the radius SHIFTED
 * DOWN ONE and the row index steps 0x200 rather than 0x100, so the
 * shape is twice as wide as it is tall.
 *
 * AND THE FALLOFF IS WARPED BY TWO SAMPLES OF g_Wobble, one taken at
 * the distance and one at the distance plus a quarter of the ANGLE -
 * which is what curves it into a swirl rather than a ring. The two
 * offsets scroll at different rates, so it turns. return.txt 2.3.
 *
 * The write is a byte add into LIGHT, no saturation of its own. */
void Gfx_GlowEllipse(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                     s32 wob_a, s32 wob_b, const u8 *ramp)
{
	s32 x, y, y0, y1, half_h;

	if (radius <= 0 || !ramp) return;
	if (cy + radius <= 0 || cy - radius >= 0x64L) return;
	if (cx + radius <= 0 || cx - radius >= 0xa0L) return;

	half_h = radius >> 1;
	y0 = cy - half_h; if (y0 < 0) y0 = 0;
	y1 = cy + half_h; if (y1 > 0x63L) y1 = 0x63L;

	for (y = y0; y <= y1; y++) {
		s32 dy = (y - cy) * 2L;
		s32 half = Gfx_Isqrt(radius * radius - dy * dy);
		s32 x0, x1, py0, py1, py;

		if (dy > radius || dy < -radius) continue;
		x0 = cx - half; if (x0 < 0) x0 = 0;
		x1 = cx + half; if (x1 > 0x9fL) x1 = 0x9fL;
		py0 = FX_MUL(y, f->scale);
		py1 = FX_MUL(y + 1, f->scale);

		for (x = x0; x <= x1; x++) {
			s32 dx = x - cx;
			s32 d = (s32)Gfx_PolarDist(dx, dy);
			s32 a = (s32)Gfx_PolarAngle(dx, dy) >> 2;
			s32 v = (s32)Gfx_BlobFalloff(radius, d);
			s32 add, px, px0, px1;

			v += g_turb[wob_a + d];
			v += g_turb[wob_b + d + a];
			if (v < 0) v = 0;
			if (v > 0xffL) v = 0xffL;
			add = ramp[v];
			if (!add) continue;
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1, f->scale);
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

/* ORIGIN 0x002d25f8 Draw_FlareBand - ten rows about `row`, each
 * pixel keyed on a polar distance taken on a SQUASHED grid: the
 * column is halved and the row steps 0x14 of the table per screen
 * row, so the shape is a wide flat ellipse rather than a disc.
 *
 * The blend at 0x002d3d72 adds one scalar to R, G and B, each
 * saturating at CHAN_MAX, and LEAVES LIGHT ALONE. return.txt 2.3.1. */
void Gfx_FlareBand(Relic_Frame *f, s32 row, s32 cx)
{
	s32 r, c, ty = -0x64L;

	for (r = 0; r < 10L; r++, ty += 0x14L) {
		s32 dy = row - 7L + r;
		s32 py0, py1, py;

		if (dy < 0 || dy >= 0x63L) continue;
		py0 = FX_MUL(dy, f->scale);
		py1 = FX_MUL(dy + 1, f->scale);
		for (c = 0; c < 0xa0L; c++) {
			s32 dxb = (s32)(s8)((c - cx) >> 1);
			s32 v = (s32)Gfx_PolarDist(dxb, (s32)(s8)ty);
			s32 px, px0, px1;

			v = ((v >> 1) ^ 0x3fL) - 0x23L;
			if (v < 0) continue;
			px0 = FX_MUL(c, f->scale);
			px1 = FX_MUL(c + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;
					int k;

					if (px < 0) continue;
					for (k = 0; k < PX_BYTES; k++) {
						s32 t;

						if (k == PX_LIGHT) continue;
						t = p[k] + v;
						p[k] = (u8)(t > CHAN_MAX ? CHAN_MAX : t);
					}
				}
			}
		}
	}
}

/* ORIGIN 0x002cf0a0 - Gfx_GlowBlob's skeleton with the add turned
 * into a SUBTRACT: LIGHT = g_LightSat[LIGHT - ramp[falloff]], the
 * index signed. Where the scene's tables resolve LIGHT to black this
 * cuts a hole in the dark rather than adding light. */
void Gfx_GlowBlobSub(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                     const u8 *ramp)
{
	s32 x, y, y0, y1;

	if (radius <= 0 || !ramp) return;
	if (cy + radius <= 0 || cy - radius >= 0x64L) return;
	if (cx + radius <= 0 || cx - radius >= 0xa0L) return;

	y0 = cy - radius; if (y0 < 0) y0 = 0;
	y1 = cy + radius; if (y1 > 0x63L) y1 = 0x63L;

	for (y = y0; y <= y1; y++) {
		s32 dy = y - cy;
		s32 half = Gfx_Isqrt(radius * radius - dy * dy);
		s32 x0, x1, py0, py1, py;

		x0 = cx - half; if (x0 < 0) x0 = 0;
		x1 = cx + half; if (x1 > 0x9fL) x1 = 0x9fL;
		py0 = FX_MUL(y, f->scale);
		py1 = FX_MUL(y + 1, f->scale);

		for (x = x0; x <= x1; x++) {
			s32 d = (s32)Gfx_PolarDist(x - cx, dy);
			s32 sub = ramp[Gfx_BlobFalloff(radius, d)];
			s32 px, px0, px1;

			if (!sub) continue;
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;
					s32 t;

					if (px < 0) continue;
					t = (s32)p[PX_LIGHT] - sub;
					p[PX_LIGHT] = (t < 0) ? 0 : g_LightSat[t];
				}
			}
		}
	}
}

/* ORIGIN 0x002cf19e - the ship's thrusters, and THE ROW INDEX DOES
 * NOT ADVANCE EVENLY. Above the centre it steps one table row per
 * screen row; at and below it steps 0x50 of 0x100 [0x002cf29a], so
 * the lower half is stretched to 3.2 times its height and the blob
 * comes out a teardrop rather than a disc.
 *
 * It writes LIGHT alone, through g_RampSpark and then g_LightSat, and
 * its row bound is 99 where every other blob here uses 100.
 *
 * RESOLUTION-PARAMETRIC: the walk is over the demo's own rows and
 * columns so the table indices stay in range, and each one fills the
 * screen block it covers - rule 20. */
void Gfx_GlowOval(Relic_Frame *f, s32 cx, s32 cy, s32 radius)
{
	s32 y, y0, y1, ty;

	if (radius <= 0) return;
	if (cy + radius <= 0 || cy - radius >= 0x63L) return;
	if (cx + radius <= 0 || cx - radius >= 0xa0L) return;

	y0 = cy - radius; if (y0 < 0) y0 = 0;
	y1 = cy + radius; if (y1 > 0x62L) y1 = 0x62L;
	y1--;             /* the demo's own subq - return.txt 2.5 */
	ty = (y0 - cy) << 8;

	for (y = y0; y <= y1; y++, ty += ty < 0 ? 0x100L : 0x50L) {
		s32 tyi = ty >> 8;
		s32 half, x, x0, x1, py, py0, py1;

		if (tyi > radius || tyi < -radius) continue;
		half = Gfx_Isqrt(radius * radius - tyi * tyi);
		x0 = cx - half; if (x0 < 0) x0 = 0;
		x1 = cx + half; if (x1 > 0x9fL) x1 = 0x9fL;
		x1--;
		py0 = FX_MUL(y, f->scale);
		py1 = FX_MUL(y + 1, f->scale);

		for (x = x0; x <= x1; x++) {
			s32 d = (s32)Gfx_PolarDist(x - cx, tyi);
			s32 add = g_RampSpark[Gfx_BlobFalloff(radius, d)];
			s32 px, px0, px1;

			if (!add) continue;
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;

					if (px < 0) continue;
					p[PX_LIGHT] =
					    g_LightSat[(p[PX_LIGHT] + add) & 0xffL];
				}
			}
		}
	}
}

/* ORIGIN 0x0036b498 Temple_DrawGlowBlob - the same falloff as
 * Gfx_GlowBlobTo above and a DIFFERENT WRITE: it adds a packed colour
 * longword from a 256-entry table, where the other saturates a byte
 * into LIGHT. The add is a true 32-bit sum, so a carry out of blue
 * reaches the LIGHT byte; the table's own LIGHT byte is always zero.
 *
 * The original modulates the falloff by two angular terms a phase
 * apart out of the sub-block at 0x0037471c. That block reads zero
 * for every frame of this scene [verified], which leaves the falloff
 * alone, so it is not carried here - see docs/effects/temple.txt. */
void Gfx_GlowBlobColourTo(u8 *dst, s32 w, s32 h, s32 scale,
                          s32 cx, s32 cy, s32 radius, const u32 *colour)
{
	s32 pcx, pcy, pr, x, y, x0, x1, y0, y1, sx0, sx1;
	s32 fbase;

	if (radius <= 0 || !colour) return;
	pcx = FX_MUL(cx, scale);
	pcy = FX_MUL(cy, scale);
	pr  = FX_MUL(radius, scale);
	if (pr <= 0) pr = 1;
	fbase = (radius & 0xffL) << 8;

	y0 = pcy - pr; if (y0 < 0) y0 = 0;
	y1 = pcy + pr; if (y1 > h - 1) y1 = h - 1;
	x0 = pcx - pr; if (x0 < 0) x0 = 0;
	x1 = pcx + pr; if (x1 > w - 1) x1 = w - 1;
	if (y0 > y1 || x0 > x1) return;

	for (y = y0; y <= y1; y++) {
		s32 dyf = PX_TO_DEMO_F(y - pcy, scale);
		u8 *row = dst + (size_t)(y * w) * PX_BYTES;
		if (span_clip(scale, pcx, radius, dyf, &sx0, &sx1, x0, x1))
			continue;
		for (x = sx0; x <= sx1; x++) {
			s32 dxf = PX_TO_DEMO_F(x - pcx, scale);
			s32 distf = polar_dist_f(dxf, dyf);
			u32 add = colour[falloff_f(fbase, distf, scale)];

			if (add) {
				u8 *p = row + (size_t)x * PX_BYTES;
				u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16)
				      | ((u32)p[2] << 8)  | (u32)p[3];

				v += add;
				p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
				p[2] = (u8)(v >> 8);  p[3] = (u8)v;
			}
		}
	}
}

/* A three-pixel line in DEMO units becomes 3*scale in pixels: the
 * core is one demo pixel and each neighbour is one more. THE
 * NEIGHBOURS ARE NOT SATURATED - the demo adds 0x0f straight into
 * the byte, so crossing lines reach 0x5d, and Ham8_BuildTables has
 * 128 light levels to hold that. Clamping here would lose it. */
static void line_pixel(Relic_Frame *f, s32 x, s32 y, int core)
{
	u8 *p;
	if (x < 0 || x >= f->w || y < 0 || y >= f->h) return;
	p = f->px + ((size_t)(y * f->w) + (size_t)x) * PX_BYTES;
	if (core) p[PX_LIGHT] = LIGHT_MAX;
	else      p[PX_LIGHT] = (u8)(p[PX_LIGHT] + 0x0f);
}

/* ORIGIN 0x002ca848  Draw_LineAA */
void Gfx_LineAA(Relic_Frame *f, s32 x0, s32 y0, s32 x1, s32 y1)
{
	s32 px0 = FX_MUL(x0, f->scale), py0 = FX_MUL(y0, f->scale);
	s32 px1 = FX_MUL(x1, f->scale), py1 = FX_MUL(y1, f->scale);
	s32 wide = (f->scale + FX_ONE / 2) >> 16;
	s32 dx, dy, step, v, i, n;
	int steep;

	if (wide < 1) wide = 1;
	dx = px1 - px0;
	dy = py1 - py0;
	steep = (dx < 0 ? -dx : dx) <= (dy < 0 ? -dy : dy);

	if (steep) {
		if (py1 <= py0) {
			s32 t;
			t = px0; px0 = px1; px1 = t;
			t = py0; py0 = py1; py1 = t;
			dx = -dx; dy = -dy;
		}
		n = py1 - py0;
		if (n <= 0) return;
		step = (dx << 8) / n;
		v = px0 << 8;
		for (i = 0; i < n; i++) {
			s32 x = v >> 8, y = py0 + i, k;
			line_pixel(f, x, y, 1);
			for (k = 1; k <= wide; k++) {
				line_pixel(f, x - k, y, 0);
				line_pixel(f, x + k, y, 0);
			}
			v += step;
		}
	} else {
		if (px1 <= px0) {
			s32 t;
			t = px0; px0 = px1; px1 = t;
			t = py0; py0 = py1; py1 = t;
			dx = -dx; dy = -dy;
		}
		n = px1 - px0;
		if (n <= 0) return;
		step = (dy << 8) / n;
		v = py0 << 8;
		for (i = 0; i < n; i++) {
			s32 x = px0 + i, y = v >> 8, k;
			line_pixel(f, x, y, 1);
			for (k = 1; k <= wide; k++) {
				line_pixel(f, x, y - k, 0);
				line_pixel(f, x, y + k, 0);
			}
			v += step;
		}
	}
}

s16 g_RaySpikes[256];
s16 g_RayArcs[256];
s16 g_RayStreak[256];
s16 g_RayStreak[256];

/* ORIGIN 0x002ca04c  Ray_BuildTables */
void Gfx_BuildRays(void)
{
	int i, n;

	memset(g_RaySpikes, 0, sizeof g_RaySpikes);
	memset(g_RayArcs, 0, sizeof g_RayArcs);
	/* ORIGIN 0x002ca104 - NOT A LOOP. Ten explicit word stores
	 * into an otherwise zero table, giving two five-entry bumps
	 * 128 angles apart: a streak across the flare rather than a
	 * ring of rays. The d7 set before them is never used. */
	memset(g_RayStreak, 0, sizeof g_RayStreak);
	g_RayStreak[0]   = 2; g_RayStreak[1]   = 4;
	g_RayStreak[2]   = 6; g_RayStreak[3]   = 4;
	g_RayStreak[4]   = 2;
	g_RayStreak[126] = 2; g_RayStreak[127] = 4;
	g_RayStreak[128] = 6; g_RayStreak[129] = 4;
	g_RayStreak[130] = 2;
	memset(g_RayStreak, 0, sizeof g_RayStreak);

	for (n = 0; n < 20; n++) {
		s32 a = Gfx_Rand(0x100);
		s32 h = Gfx_Rand(8);
		g_RaySpikes[a & 0xff] = (s16)h;
		h -= 3;
		if (h < 0) continue;
		g_RaySpikes[(a + 1) & 0xff] = (s16)h;
		g_RaySpikes[(a - 1) & 0xff] = (s16)h;
		h -= 2;
		if (h < 0) continue;
		g_RaySpikes[(a - 2) & 0xff] = (s16)h;
		g_RaySpikes[(a + 2) & 0xff] = (s16)h;
	}

	for (n = 0; n < 20; n++) {
		s32 a = Gfx_Rand(0x100);
		s32 h = Gfx_Rand(0xd);
		s32 w = Gfx_Rand(0x10) + 0x14;      /* 20 to 35 wide */
		for (i = 0; i <= w; i++)
			g_RayArcs[(a + i) & 0xff] = (s16)h;
	}
	/* SMOOTHED AND NEGATED. Draw_Flare subtracts the ray value, so
	 * the sign flip is what makes these arcs BRIGHTEN where the
	 * spikes darken. The average is taken as a BYTE, as the demo
	 * does, which clips a sum over 255 - that is the demo's own
	 * behaviour and the heights never reach it. */
	for (i = 0; i < 256; i++) {
		s32 v = (s32)g_RayArcs[i] + (s32)g_RayArcs[(i + 1) & 0xff];
		v = (v & 0xff) >> 1;
		g_RayArcs[i] = (s16)(-v);
	}

	/* Ten entries, hand written, and the rest left zero. */
	g_RayStreak[0x80] = 6;
	g_RayStreak[0x81] = 4;
	g_RayStreak[0x7f] = 4;
	g_RayStreak[0x82] = 2;
	g_RayStreak[0x7e] = 2;
	g_RayStreak[0x02] = 6;
	g_RayStreak[0x01] = 4;
	g_RayStreak[0x03] = 4;
	g_RayStreak[0x00] = 2;
	g_RayStreak[0x04] = 2;
}

/* ORIGIN 0x002c9eac  Ramp_BuildColour */
void Gfx_BuildColourRamp(u8 *out, s32 r, s32 g, s32 b, s32 numer)
{
	int i;
	for (i = 0; i < 256; i++) {
		s32 t = ((s32)i * 0xc8L) / 0x80L;
		s32 t2 = t * t;
		s32 v = (t2 == 0) ? numer : ((numer - t2) / t2);
		s32 cr, cg, cb;
		v >>= 4;
		cr = (r * v) >> 6;
		cg = (g * v) >> 6;
		cb = (b * v) >> 6;
		if (cr < 0) cr = 0; if (cr > CHAN_MAX) cr = CHAN_MAX;
		if (cg < 0) cg = 0; if (cg > CHAN_MAX) cg = CHAN_MAX;
		if (cb < 0) cb = 0; if (cb > CHAN_MAX) cb = CHAN_MAX;
		out[i * PX_BYTES + PX_R] = (u8)cr;
		out[i * PX_BYTES + PX_G] = (u8)cg;
		out[i * PX_BYTES + PX_LIGHT] = 0;   /* never carries */
		out[i * PX_BYTES + PX_B] = (u8)cb;
	}
}

/* ORIGIN 0x002c9e80 - adds a small hump to a ramp's BLUE channel. The
 * original adds a longword whose only non-zero byte is the low one, and
 * the low byte of a pixel is B. Entry i pairs with 255 - i because the
 * dbf counts down while the pointer counts up.
 *
 * The guard is ours: the original lets t to the fourth overflow its
 * longword, which is undefined in C rather than merely wrapping. Past
 * the guard the result is zero either way, so nothing is lost. */
void Gfx_RampBlueBump(u8 *ramp, s32 at)
{
	int i;
	for (i = 0; i < 256; i++) {
		s32 t = (255 - i) - at;
		s32 q = t * t;
		s32 v = 0;
		if (q < 1024) {
			v = 8 - ((q * q) >> 6);
			if (v < 0) v = 0;
		}
		ramp[i * PX_BYTES + PX_B] = (u8)(ramp[i * PX_BYTES + PX_B] + v);
	}
}

/* ORIGIN 0x002d4452  Draw_Flare, with the centre taken in 16.16 demo
 * units. The demo truncates it to a whole pixel because a whole pixel
 * is all its framebuffer has; above scale 1.0 ours has more, and the
 * fraction is the demo's own projection arithmetic carried rather than
 * invented. A ZERO FRACTION REDUCES THIS TERM FOR TERM TO THE
 * ORIGINAL. THE RAMP INDEX IS NOT CLAMPED - planet_sun.txt 6. */
void Gfx_FlareFx(Relic_Frame *f, s32 cx_fx, s32 cy_fx, s32 radius,
                 const s16 *rays, const u8 *ramp)
{
	s32 pcx_fx, pcy_fx, pcx, pcy, pr, x, y, x0, x1, y0, y1;
	s32 sx0, sx1, fbase;

	if (radius <= 0 || !rays || !ramp) return;
	/* In halves: cx_fx * scale overflows a s32 outright. */
	pcx_fx = (cx_fx >> 16) * f->scale
	       + (((cx_fx & 0xffffL) * (f->scale >> 8)) >> 8);
	pcy_fx = (cy_fx >> 16) * f->scale
	       + (((cy_fx & 0xffffL) * (f->scale >> 8)) >> 8);
	pcx = pcx_fx >> 16;
	pcy = pcy_fx >> 16;
	pr  = FX_MUL(radius, f->scale);
	if (pr <= 0) pr = 1;
	fbase = (radius & 0x7fL) << 8;

	y0 = pcy - pr; if (y0 < 0) y0 = 0;
	y1 = pcy + pr; if (y1 > f->h - 1) y1 = f->h - 1;
	x0 = pcx - pr; if (x0 < 0) x0 = 0;
	x1 = pcx + pr; if (x1 > f->w - 1) x1 = f->w - 1;
	if (y0 > y1 || x0 > x1) return;

	for (y = y0; y <= y1; y++) {
		s32 dyf = (((y << 16) - pcy_fx) << DEMO_FRAC) / f->scale;
		s32 dy = dyf >> DEMO_FRAC;
		u8 *row = f->px + (size_t)(y * f->w) * PX_BYTES;
		if (span_clip(f->scale, pcx, radius, dyf, &sx0, &sx1, x0, x1))
			continue;
		/* THE RIGHT EDGE IS EXCLUSIVE HERE, unlike Gfx_GlowBlob's.
		 * Draw_Flare clamps cx + chord to 0x9f and then takes one off
		 * before the dbf, so the last column it touches is one short of
		 * the span [0x002d4502]. Drawing it leaves a bright fringe down
		 * the right of the screen whenever the disc runs off it. */
		for (x = sx0; x < sx1; x++) {
			s32 dxf = (((x << 16) - pcx_fx) << DEMO_FRAC) / f->scale;
			const u8 *w = polar_word(dxf >> DEMO_FRAC, dy);
			s32 ang = w[0];
			s32 v = falloff_f(fbase, polar_dist_f(dxf, dyf), f->scale);
			u8 *p;
			const u8 *c;
			int k;

			/* Sampled TWICE, g_FlareRayPhase apart, so a non-zero
			 * phase splits every ray in two and advancing it turns
			 * them. g_FlareRayPhase is the LOW BYTE of the word at
			 * 0x002e0c1c - see Gfx_SetFlareRayPhase. */
			ang = (ang + g_flare_ray_phase) & 0xff;
			v -= rays[ang];
			ang = (ang + g_flare_ray_phase) & 0xff;
			v -= rays[ang];

			c = ramp + v * PX_BYTES;
			p = row + (size_t)x * PX_BYTES;
			/* The demo does this as one add.l. Done per channel
			 * here so a host with a different pixel order still
			 * works; the ramp's light byte is zero either way. */
			for (k = 0; k < PX_BYTES; k++) {
				s32 s = p[k] + c[k];
				p[k] = (u8)(s > 255 ? 255 : s);
			}
		}
	}
}

/* OURS. The integer centre every transcribed call site uses. */
void Gfx_Flare(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
               const s16 *rays, const u8 *ramp)
{
	Gfx_FlareFx(f, cx << 16, cy << 16, radius, rays, ramp);
}

/* OURS - selects the base Ham8_BuildTables blends toward; the demo
 * passes it per scene. */
void Gfx_SetLightBase(s32 r, s32 g, s32 b)
{
	g_base_r = r;
	g_base_g = g;
	g_base_b = b;
}

/* OURS */
static s32 chan_clamp(s32 c)
{
	return c > CHAN_MAX ? CHAN_MAX : c;
}

/* ORIGIN 0x002ca456  Ham8_BuildTables, one channel */
static u8 resolve_chan(s32 c6, s32 base, s32 d5)
{
	s32 v = base * 256L + (c6 - base) * d5;
	if (v > 0x3f00L) v = 0x3f00L;
	if (v < 0) v = 0;
	return (u8)(((v >> 8) * 255L) / CHAN_MAX);
}

/* ORIGIN 0x002ca456  Ham8_BuildTables */
void Gfx_ResolveToRGB(const Relic_Frame *f, u8 *rgb_out)
{
	s32 i, n = f->w * f->h;
	const u8 *p = f->px;
	u8 *o = rgb_out;

	for (i = 0; i < n; i++, p += PX_BYTES, o += 3) {
		s32 light = p[PX_LIGHT];
		s32 d5;
		if (light == 0) {
			/* CLAMP FIRST. The additive passes let a channel past 0x3f -
			 * the demo's own framebuffer reaches 0x55 - and scaling that
			 * by 255/0x3f overflows a byte, so the brightest pixels wrap
			 * round to nearly black. One over the top turns into 3. The
			 * lit path below already clamps; this one did not. */
			o[0] = (u8)((chan_clamp(p[PX_R]) * 255L) / CHAN_MAX);
			o[1] = (u8)((chan_clamp(p[PX_G]) * 255L) / CHAN_MAX);
			o[2] = (u8)((chan_clamp(p[PX_B]) * 255L) / CHAN_MAX);
			continue;
		}
		d5 = 0xffL - 4L * light;
		if (d5 < 0) d5 = 0;
		o[0] = resolve_chan(p[PX_R], g_base_r, d5);
		o[1] = resolve_chan(p[PX_G], g_base_g, d5);
		o[2] = resolve_chan(p[PX_B], g_base_b, d5);
	}
}
