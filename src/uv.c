/* RELIC - texture coordinate generation. See uv.h and
 * docs/formats/geometry.txt section 4.
 */

#include "uv.h"
#include "gfx.h"

/* ORIGIN 0x002cfdb0 Uv_AtanLookup - the angle byte out of the polar
 * table, then folded.
 *
 *     index = ((d1 & 0xff) << 8) | (d2 & 0xff)   a SIGNED word
 *     D0 = table[index] >> 8
 *     if the 0x8000 mode bit is CLEAR:  D0 = (D0 * 8) & 0xff
 *     if D0 >= 0x80:                    D0 = 0xff - D0
 *
 * THE FOLD HALVES THE RANGE, so u only ever spans 0..0x7f and the
 * texture's right half is never addressed by this projection. The
 * times-eight on the default path is what makes the pattern repeat
 * eight times around the cylinder; the 0x8000 path drops it and goes
 * round once. Gfx_PolarAngle reads the same centred table with the
 * same signed index, so it is reused rather than re-derived. */
static s32 atan_lookup(s32 d1, s32 d2, s32 mode)
{
	s32 d0 = Gfx_PolarAngle(d2, d1);
	if (!(mode & UV_ATAN_ALT)) d0 = (d0 * 8) & 0xffL;
	if (d0 >= 0x80L) d0 = 0xffL - d0;
	return d0;
}

/* ORIGIN 0x002d4354 maybe_Math_Rotate2D. */
static void rot2d(s32 *a, s32 *b, s32 angle)
{
	s32 c = Gfx_Cos(angle & 0x1fffL);
	s32 s = Gfx_Sin(angle & 0x1fffL);
	s32 x = *a, y = *b;
	*a = ((x * c) + (y * s)) >> 14;
	*b = ((y * c) - (x * s)) >> 14;
}

/* ORIGIN the divsl.l #3 in the ranged projections - it truncates
 * toward zero, which C89 leaves implementation-defined for a negative
 * operand, so the sign is taken out and put back. */
static s32 avg3(const Relic_Obj *o, const Relic_Face *f, int comp)
{
	s32 t = 0;
	int k;
	for (k = 0; k < 3; k++) t += o->vert[f->corner[k]].comp[comp];
	return (t < 0) ? -((-t) / 3) : (t / 3);
}

/* OURS. divsl.l truncates toward zero; C89 leaves that
 * implementation-defined for a negative operand. */
static s32 sdiv(s32 a, s32 b)
{
	s32 q;
	if (!b) return 0;
	q = (a < 0) == (b < 0) ? 0 : 1;
	if (!q) return a / b;
	a = (a < 0) ? -a : a;
	b = (b < 0) ? -b : b;
	return -(a / b);
}

/* ORIGIN 0x002cfbcc Uv_ProjRotatedXY - mode bit 0x01. Planar onto the
 * comp0/comp1 plane, rotated by the pass angle, and no range test so
 * every face is written.
 *
 * THE CENTRE IS ADDED AFTER SCALING HERE, where the planar modes
 * subtract theirs before. COMP1 GOES IN FIRST, not comp0. */
static int proj_rotated(const Relic_Obj *o, const Relic_Face *f,
                        const Relic_UvPass *p, Relic_FaceUv *uv)
{
	int k;
	for (k = 0; k < 3; k++) {
		const Relic_Vert *v = &o->vert[f->corner[k]];
		s32 d0 = v->comp[1], d1 = v->comp[0];
		rot2d(&d0, &d1, p->angle);
		uv->u[k] = ((d0 * p->scale) >> 16) + p->centre_u;
		uv->v[k] = ((d1 * p->scale) >> 16) + p->centre_v;
	}
	return 1;
}

/* ORIGIN 0x002cf73e Uv_ProjAngularZ - mode bit 0x02. A cylinder about
 * the comp2 axis, culled on the average of comp2 over the face:
 *
 *     span = range_max - range_min
 *     d3 = -(comp2 - range_max)      how far below the top
 *     u  = AtanLookup(comp1 >> 8, comp0 >> 8)
 *     v  = (d3 * scale) / span
 */
static int proj_cyl_c2(const Relic_Obj *o, const Relic_Face *f,
                       const Relic_UvPass *p, Relic_FaceUv *uv)
{
	s32 a = avg3(o, f, 2), span;
	int k;
	if (a < p->range_min || a > p->range_max) return 0;
	span = p->range_max - p->range_min;
	for (k = 0; k < 3; k++) {
		const Relic_Vert *v = &o->vert[f->corner[k]];
		s32 d3 = -(v->comp[2] - p->range_max);
		uv->u[k] = atan_lookup(v->comp[1] >> 8, v->comp[0] >> 8, p->mode);
		uv->v[k] = sdiv(d3 * p->scale, span);
	}
	return 1;
}

/* OURS */
s32 Uv_Generate(const Relic_Obj *o, const Relic_UvPass *pass, s32 npass,
                Relic_FaceUv *out)
{
	s32 i, n, unsupported = 0;

	for (n = 0; n < npass; n++) {
		const Relic_UvPass *p = &pass[n];
		if (p->mode & ~UV_IMPLEMENTED) { unsupported++; continue; }
		if (p->mode & UV_TEXTURE_ONLY) {
			/* ORIGIN Uv_SetTextureOnly - every corner at the origin,
			 * so the face takes one texel and reads as flat. */
			for (i = 0; i < o->nfaces; i++) {
				int k;
				for (k = 0; k < 3; k++) { out[i].u[k] = 0; out[i].v[k] = 0; }
				out[i].written = 1;
			}
			continue;
		}
		for (i = 0; i < o->nfaces; i++) {
			Relic_FaceUv got;
			int ok = 0;
			if (p->mode & UV_ROTATED)
				ok = proj_rotated(o, &o->face[i], p, &got);
			if (p->mode & UV_CYL_C2)
				ok = proj_cyl_c2(o, &o->face[i], p, &got);
			/* A REJECTED FACE KEEPS WHAT AN EARLIER PASS GAVE IT. */
			if (ok) {
				got.written = 1;
				out[i] = got;
			}
		}
	}
	return unsupported;
}

/* ORIGIN 0x002d1690 Uv_ProjBoxish - see uv.h. */
static s32 boxish_fold(s32 v, int is_u)
{
	v >>= 12;
	if (is_u) v += 0x40L;
	v &= 0xffL;
	if (v >= 0x80L) v = -(v - 0xffL);
	return v;
}

/* ORIGIN 0x002d1690 Uv_ProjBoxish */
void Uv_Boxish(const Relic_Obj *o, Relic_FaceUv *out)
{
	s32 n, k;

	if (!o || !o->vert || !o->face || !out) return;
	for (n = 0; n < o->nfaces; n++) {
		const Relic_Vert *v[3];
		s32 uu[3], vv[3];
		int alt;

		for (k = 0; k < 3; k++) v[k] = &o->vert[o->face[n].corner[k]];
		alt = (v[1]->comp[2] + v[1]->comp[1]
		         == v[0]->comp[2] + v[0]->comp[1])
		   && (v[2]->comp[2] + v[2]->comp[1]
		         == v[0]->comp[2] + v[0]->comp[1]);
		for (k = 0; k < 3; k++) {
			if (alt) {
				uu[k] = v[k]->comp[2];
				vv[k] = v[k]->comp[1];
			} else {
				uu[k] = v[k]->comp[0];
				vv[k] = v[k]->comp[2] + v[k]->comp[1];
			}
			out[n].u[k] = boxish_fold(uu[k], 1);
			out[n].v[k] = boxish_fold(vv[k], 0);
		}
		out[n].written = 1;
	}
}
