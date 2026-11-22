/* RELIC - the textured triangle. See rast.h and
 * docs/subsystems/renderer.txt.
 *
 * THE DEMO IS AFFINE PER SPAN, so linear interpolation of u and v
 * over the projected triangle IS its mapping and not an approximation
 * of it: the perspective divide happens once per VERTEX in
 * Scene_TransformProject and the span filler then steps u and v with
 * a plain add. What is NOT transcribed is the fixed-point packing of
 * that step - renderer.txt section 3 - so the stepping here is
 * straightforward 16.16 and will differ from the demo's in the low
 * bits. The texel chosen can therefore be one off along a span.
 *
 * INDICES ARE u32, NOT size_t. relic.h declares the fixed-width types
 * and includes nothing, so size_t reaches a file only through a
 * standard header - and this one needs none. A 160 x 100 frame and a
 * 256 x 256 texture both fit a u32 with room to spare.
 */

#include "rast.h"

/* ORIGIN 0x002cabce Rast_FillSpansTextured - its
 * (v & 0xff00) | (u & 0x00ff) merge, which is the whole bounds check
 * the demo has. */
static const u8 *texel(const u8 *tex, s32 rows, s32 u, s32 v)
{
	s32 uu = u & 0xffL;
	s32 vv = v & 0xffL;
	/* OURS, AND UNREACHABLE IN THE DEMO. Fshiptextures' last
	 * sub-block has 128 rows behind it, not 256, and textures.txt 3
	 * records that nothing drives v past 128 there - this keeps the
	 * read inside the chunk if anything ever does. Both counts are
	 * powers of two. */
	if (rows > 0 && vv >= rows) vv &= (rows - 1);
	return tex + (u32)((vv << 8) | uu) * PX_BYTES;
}

/* ORIGIN 0x00369dd8 - the shade the type-5 walk subtracts, or -1 for
 * the mesh walk's whole-pixel copy. THE SHADE IS ONE BYTE A TEXEL
 * where a mesh face reads four, indexed (v << 8) | u.
 * temple.txt 11.6. */
static s32 g_shadeBias;
static int g_shadeOn;

/* OURS. One scanline of the triangle, u and v interpolated across it.
 * The demo's filler takes its spans from an edge walker that emits a
 * record per line; this walks the edges directly, which produces the
 * same spans without the intermediate list. */
static void span(Relic_Frame *f, s32 y, s32 xa, s32 xb,
                 s32 ua, s32 va, s32 ub, s32 vb,
                 const u8 *tex, s32 rows)
{
	s32 n, i, u, v, du, dv;
	u8 *q;

	if (y < 0 || y >= f->h) return;
	if (xb < xa) {
		s32 t;
		t = xa; xa = xb; xb = t;
		t = ua; ua = ub; ub = t;
		t = va; va = vb; vb = t;
	}
	n = xb - xa;
	if (n <= 0) return;
	du = (ub - ua) / n;
	dv = (vb - va) / n;
	u = ua;
	v = va;
	if (xa < 0) {
		u += du * (-xa);
		v += dv * (-xa);
		n -= -xa;
		xa = 0;
	}
	if (xa + n > f->w) n = f->w - xa;
	if (n <= 0) return;
	q = f->px + (u32)(y * f->w + xa) * PX_BYTES;
	for (i = 0; i < n; i++, q += PX_BYTES) {
		const u8 *t = texel(tex, rows, u >> 16, v >> 16);

		if (g_shadeOn) {
			s32 d = (s32)tex[(((v >> 16) & 0xffL) << 8)
			                 | ((u >> 16) & 0xffL)] - g_shadeBias;

			if (d >= 0L) {
				s32 lo = (s32)((s8)(u8)d >> 2) & 0xffL;
				s32 l = (s32)q[PX_LIGHT]
				      - (s32)(s16)((d & 0xff00L) | lo);

				q[PX_LIGHT] = (u8)(l < 0L ? 0L : l);
			}
		} else {
		/* ONE move.l IN THE ORIGINAL, so the light byte comes from the
		 * texel too - and every texel has it zero, which is what lets
		 * the flash add over the ship instead of under it. */
		q[PX_R] = t[PX_R];
		q[PX_G] = t[PX_G];
		q[PX_LIGHT] = t[PX_LIGHT];
		q[PX_B] = t[PX_B];
		}
		u += du;
		v += dv;
	}
}

/* ORIGIN 0x0036884c Temple_DrawShadeFaceList - the same face loop
 * and the same walk, so this is Rast_TexTri with its write swapped.
 * THE BIAS GOES NEGATIVE and the shift is asr.b, on the low byte
 * alone, so a texel far enough over the bias subtracts a number far
 * larger than the cloud it is eating. temple.txt 11.6. */
void Rast_ShadeTri(Relic_Frame *f, const s32 *x, const s32 *y,
                   const s32 *u, const s32 *v,
                   const u8 *tex, s32 rows, s32 bias)
{
	g_shadeBias = bias;
	g_shadeOn = 1;
	Rast_TexTri(f, x, y, u, v, tex, rows);
	g_shadeOn = 0;
}

/* ORIGIN 0x002caf1a and 0x002caf52 - A FACE WHOLLY OFF THE FRAME IS
 * DROPPED, and it is not an optimisation: the bounds are the demo's
 * own, 0xa0 the width and 0x63 one short of the height. TAKEN BEFORE
 * THE SCALE, which is where the demo takes them and is also what
 * keeps the scale's multiply inside a s32 - a demo pixel past 32767
 * overflows it. The window scene's camera sits close enough to hand
 * this 145 faces a frame that are nowhere near the frame. */
static int offframe(const Relic_Frame *f, const s32 *x, const s32 *y)
{
	s32 lo, hi, i, v;

	lo = hi = x[0] >> RAST_FRAC;
	for (i = 1; i < 3; i++) {
		v = x[i] >> RAST_FRAC;
		if (v < lo) lo = v;
		if (v > hi) hi = v;
	}
	if (hi < 0 || lo >= PX_TO_DEMO(f->w, f->scale)) return 1;
	lo = hi = y[0] >> RAST_FRAC;
	for (i = 1; i < 3; i++) {
		v = y[i] >> RAST_FRAC;
		if (v < lo) lo = v;
		if (v > hi) hi = v;
	}
	return hi < 0 || lo >= PX_TO_DEMO(f->h, f->scale) - 1;
}

/* OURS. Sorted by y, then two edge walks. */
void Rast_TexTri(Relic_Frame *f, const s32 *x, const s32 *y,
                 const s32 *u, const s32 *v,
                 const u8 *tex, s32 rows)
{
	s32 px[3], py[3], pu[3], pv[3];
	s32 i, j;

	if (!tex || !f->px) return;
	if (offframe(f, x, y)) return;
	/* 24.8 TIMES 16.16 OVERFLOWS A s32, so the halves scale apart.
	 * Dropping the fraction staircases every vertex. */
	for (i = 0; i < 3; i++) {
		px[i] = FX_MUL(x[i] >> RAST_FRAC, f->scale)
		      + (((x[i] & 0xffL) * f->scale) >> (16 + RAST_FRAC));
		py[i] = FX_MUL(y[i] >> RAST_FRAC, f->scale)
		      + (((y[i] & 0xffL) * f->scale) >> (16 + RAST_FRAC));
		pu[i] = u[i] << 16;
		pv[i] = v[i] << 16;
	}
	/* a three-element sort, written out */
	for (i = 0; i < 2; i++) {
		for (j = i + 1; j < 3; j++) {
			if (py[j] < py[i]) {
				s32 t;
				t = px[i]; px[i] = px[j]; px[j] = t;
				t = py[i]; py[i] = py[j]; py[j] = t;
				t = pu[i]; pu[i] = pu[j]; pu[j] = t;
				t = pv[i]; pv[i] = pv[j]; pv[j] = t;
			}
		}
	}
	if (py[2] == py[0]) return;

	/* TWO EDGE WALKS, STEPPED IN 16.16 - no floats anywhere, because
	 * the demo has none and rule 16 keeps it that way. The long edge
	 * runs the whole height; the short one changes at the knee. */
	{
		s32 yy, dxl, dul, dvl, xl, ul, vl;
		s32 dxs, dus, dvs, xs, us, vs;
		s32 den = py[2] - py[0];
		s32 knee = py[1];

		dxl = ((px[2] - px[0]) << 8) / den;
		dul = (pu[2] - pu[0]) / den;
		dvl = (pv[2] - pv[0]) / den;
		xl = px[0] << 8;
		ul = pu[0];
		vl = pv[0];

		dxs = dus = dvs = 0;
		xs = px[0] << 8;
		us = pu[0];
		vs = pv[0];
		if (knee > py[0]) {
			den = knee - py[0];
			dxs = ((px[1] - px[0]) << 8) / den;
			dus = (pu[1] - pu[0]) / den;
			dvs = (pv[1] - pv[0]) / den;
		}

		for (yy = py[0]; yy < py[2]; yy++) {
			if (yy == knee) {
				den = py[2] - knee;
				xs = px[1] << 8;
				us = pu[1];
				vs = pv[1];
				if (den > 0) {
					dxs = ((px[2] - px[1]) << 8) / den;
					dus = (pu[2] - pu[1]) / den;
					dvs = (pv[2] - pv[1]) / den;
				} else {
					dxs = dus = dvs = 0;
				}
			}
			if (yy >= f->h) break;
			if (yy >= 0)
				span(f, yy, xl >> 8, xs >> 8, ul, vl, us, vs, tex, rows);
			xl += dxl; ul += dul; vl += dvl;
			xs += dxs; us += dus; vs += dvs;
		}
	}
}

/* ORIGIN 0x002f111c g_pRecip - 0x7fff / n, SIGNED BOTH WAYS, so a
 * negative width yields a negative reciprocal and that sign IS the
 * gradient's sign. Taking abs(n) flips every leftward span's shading.
 * effects/orb.txt 9. */
static s32 recip(s32 n)
{
	if (n == 0) return 0;
	return 0x7fffL / n;
}

/* ORIGIN 0x002cb3aa Rast_AdditivePoly, +0x278 - the per-pixel core:
 *
 *     i  = pattern[(uv + scroll) & 0x7fff]
 *     i += pattern[((uv + 2*scroll) & 0x7fff) + 0x7f]
 *     *(u32 *)pixel += palette[i]
 *
 * with uv = (v_int << 8) | u_int masked to 0x7fff, which makes the
 * pattern 128 rows of 256. THE SUM IS A BYTE and the wrap is what
 * bands the shell; clamping instead loses it.
 *
 * [open] STILL NOT RIGHT - the shell comes out bright and noisy where
 * the demo is a faint smooth wash, which is why no part lights it.
 * effects/orb.txt 9. */
static void add_span(Relic_Frame *f, s32 yy, s32 xa, s32 xb,
                     s32 u, s32 v, s32 dudx, s32 dvdx,
                     const u8 *pattern, const u8 *palette, s32 scroll)
{
	s32 step = (xb >= xa) ? 1L : -1L;
	s32 x = xa;
	s32 n = (xb >= xa) ? (xb - xa) : (xa - xb);

	while (n-- > 0) {
		if (x >= 0 && x < f->w) {
			s32 uv = (((v >> 16) & 0x7fL) << 8) | ((u >> 16) & 0xffL);
			s32 t = (uv + scroll) & 0x7fffL;
			s32 i = pattern[t];
			t = (t + scroll) & 0x7fffL;
			i = (i + pattern[t + 0x7fL]) & 0xffL;
			{
				const u8 *c = palette + (u32)i * PX_BYTES;
				u8 *p = f->px
				      + ((u32)(yy * f->w) + (u32)x) * PX_BYTES;
				s32 k;

				/* ORIGIN 0x002cb63e - add.l, so a channel carries into
				 * the next; the clamp is ours.
				 * UNSCALED, as the demo's add is - orb.txt 9. */
				for (k = 0; k < PX_BYTES; k++) {
					s32 v = p[k] + c[k];
					p[k] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
				}
			}
		}
		x += step;
		/* THE WALK CARRIES THE GRADIENT, and it SUBTRACTS going left -
		 * the original keeps two code paths for the two directions
		 * rather than negating [0x002cb622 and 0x002cb88c]. */
		if (step > 0) { u += dudx; v += dvdx; }
		else          { u -= dudx; v -= dvdx; }
	}
}

/* ORIGIN 0x002cb3aa Rast_AdditivePoly.
 *
 * ONE CLIPPED PATH WHERE THE DEMO HAS TWO. The original carries the
 * same algorithm twice - a clamping copy at +0x9c and a copy with
 * the bounds tests omitted at +0x32c, chosen on whether the
 * triangle fits vertically. That is an optimisation and not a
 * difference in output, so this is the clamping one only.
 *
 * THE ORIGINAL SORTS BY X FIRST AND SORTS AGAIN BY Y. Only the
 * second order reaches the spans, so the sort is transcribed once -
 * but the extent rejects that sit between the two are NOT optional,
 * and are applied below. */
void Rast_AddTri(Relic_Frame *f, const s32 *x, const s32 *y,
                 const s32 *u, const s32 *v,
                 const u8 *pattern, const u8 *palette, s32 scroll)
{
	s32 px[3], py[3], pu[3], pv[3];
	s32 i, j;

	if (!pattern || !palette || !f->px) return;
	if (offframe(f, x, y)) return;
	for (i = 0; i < 3; i++) {
		px[i] = FX_MUL(x[i] >> RAST_FRAC, f->scale)
		      + (((x[i] & 0xffL) * f->scale) >> (16 + RAST_FRAC));
		py[i] = FX_MUL(y[i] >> RAST_FRAC, f->scale)
		      + (((y[i] & 0xffL) * f->scale) >> (16 + RAST_FRAC));
		pu[i] = u[i];
		pv[i] = v[i];
	}
	/* ORIGIN 0x002cb3ce and 0x002cb40a - THE EXTENT REJECTS, and they
	 * are not an optimisation: a face wholly off the frame is DROPPED,
	 * never clipped later, and a per-span clamp draws one as a smear
	 * down the edge instead of nothing. The bounds are the demo's own
	 * in frame pixels - 0xa0 the width, 0x63 one short of the height.
	 * effects/orb.txt 9. */
	{
		s32 lo = px[0], hi = px[0];

		for (i = 1; i < 3; i++) {
			if (px[i] < lo) lo = px[i];
			if (px[i] > hi) hi = px[i];
		}
		if (hi < 0 || lo >= f->w) return;
		lo = hi = py[0];
		for (i = 1; i < 3; i++) {
			if (py[i] < lo) lo = py[i];
			if (py[i] > hi) hi = py[i];
		}
		if (hi < 0 || lo >= f->h - 1) return;
	}
	for (i = 0; i < 2; i++) {
		for (j = i + 1; j < 3; j++) {
			if (py[j] < py[i]) {
				s32 t;
				t = px[i]; px[i] = px[j]; px[j] = t;
				t = py[i]; py[i] = py[j]; py[j] = t;
				t = pu[i]; pu[i] = pu[j]; pu[j] = t;
				t = pv[i]; pv[i] = pv[j]; pv[j] = t;
			}
		}
	}
	if (py[2] == py[0]) return;
	{
		s32 dyt = py[2] - py[0];
		s32 dym = py[1] - py[0];
		s32 t = (dym * recip(dyt)) >> 7;
		s32 width = (((px[2] - px[0]) * t) >> 8) - (px[1] - px[0]);
		s32 dun, dvn, dudx, dvdx;
		s32 yy, dxl, dxs, xl, xs, ul, vl, dul, dvl;

		if (width == 0) return;
		/* ORIGIN 0x002cb498 - ONE AFFINE PAIR FOR THE WHOLE TRIANGLE,
		 * taken at the knee, and the step is WHOLE UNITS for u and 8.8
		 * for v. Dividing each span by its own width instead gives a v
		 * that advances whole rows a pixel and the shell speckles.
		 * Checked against the hardware: dyt 116, dym 11, width 41 and
		 * uv (0,0) (127,0) (64,127) give -3 and 68, which is what the
		 * registers hold at 0x002cb4c6 [verified]. */
		dun = (((pu[2] - pu[0]) * t) >> 8) - (pu[1] - pu[0]);
		dvn = (((pv[2] - pv[0]) * t) >> 8) - (pv[1] - pv[0]);
		dudx = (dun * recip(width) * 2) >> 16;
		dvdx = (dvn * recip(width)) >> 7;

		dxl = ((px[2] - px[0]) << 8) / dyt;
		dul = (pu[2] - pu[0]) * recip(dyt) * 2;
		dvl = (pv[2] - pv[0]) * recip(dyt) * 2;
		xl = px[0] << 8;
		ul = pu[0] << 16;
		vl = pv[0] << 16;

		xs = px[0] << 8;
		dxs = (dym > 0) ? (((px[1] - px[0]) << 8) / dym) : 0L;

		for (yy = py[0]; yy < py[2]; yy++) {
			if (yy == py[1]) {
				s32 den = py[2] - py[1];
				xs = px[1] << 8;
				dxs = (den > 0) ? (((px[2] - px[1]) << 8) / den) : 0L;
			}
			if (yy >= f->h) break;
			if (yy >= 0)
				add_span(f, yy, xl >> 8, xs >> 8, ul, vl,
				         dudx << 16, dvdx << 8,
				         pattern, palette, scroll);
			xl += dxl; ul += dul; vl += dvl;
			xs += dxs;
		}
	}
}
