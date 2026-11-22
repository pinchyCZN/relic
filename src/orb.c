/* RELIC - the cracked sphere and the fullscreen affine fill, shared
 * by the release and the Earth shot. See orb.h.
 */

#include <stddef.h>          /* size_t - relic.h declares no types of C's */
#include "orb.h"
#include "gfx.h"
#include "rast.h"
#include "dat.h"
#include "geom.h"
#include "image.h"

/* ORIGIN 0x002d1fdc and 0x002cdc3c - the tables the sphere and the
 * crack mask walk. Each bias centres a SIGNED index in its table. */
#define OB_NORMAL      0x0026f97aUL
#define OB_NORMAL_BIAS 0x10100L
#define OB_CHORDANG    0x0029797aUL
#define OB_SINTAB      0x00228b6cUL
#define OB_TEX_BIAS    0x20200L
#define OB_TEX_BYTES   0x40000UL

/* ORIGIN g_CrackPntsChunk and g_CrackEdgeChunk - the PNTS and EDGE
 * payloads of the IFF TDDD object linked into the executable.
 * formats/tddd.txt. */
#define OB_PNTS      0x002e1c66UL
#define OB_EDGE      0x002e2138UL
#define OB_MAX_PTS   256
#define OB_MAX_EDGES 256
#define OB_CULLED    0x7fffffffL

/* ORIGIN 0x002cdc3c Crack_DrawShells, +0x12c - the additive quad's
 * per-vertex U and V, passed in A0 to A5 as immediates. */
static const s32 OB_SHELL_U[3] = { 0L, 0x7fL, 0x40L };
static const s32 OB_SHELL_V[3] = { 0x7fL, 0x7fL, 0L };

static const u8 *g_tex;
static s32 g_cv[OB_MAX_PTS][3];
static s32 g_cp[OB_MAX_PTS][2];
static s32 g_cd[OB_MAX_PTS];
static s32 g_ce[OB_MAX_EDGES][2];
static s32 g_cn, g_cen;
static s32 g_shells, g_live;

/* OURS - g_SinTable read out of the image. */
s32 Orb_Sin(s32 i)
{
	return (s32)Image_U32((u32)(OB_SINTAB + (u32)((i & 0x1fffL) << 2)));
}

s32 Orb_Cos(s32 i) { return Orb_Sin(i + 2048L); }

/* ORIGIN 0x002d4258 Vec_Rotate2Axis - two 2D rotations, and the >>14
 * is because the sine table's amplitude is 0x4000. */
void Orb_Rotate2Axis(s32 *x, s32 *y, s32 *z, s32 ay, s32 ax)
{
	s32 c, s, a, b;

	c = Orb_Cos(ay); s = Orb_Sin(ay);
	a = *x; b = *y;
	*x = ((a * c + b * s) >> 8) >> 6;
	*y = ((b * c - a * s) >> 8) >> 6;
	c = Orb_Cos(ax); s = Orb_Sin(ax);
	a = *y; b = *z;
	*y = ((a * c + b * s) >> 8) >> 6;
	*z = ((b * c - a * s) >> 8) >> 6;
}

/* ORIGIN 0x002cc244 Crack_LoadGeometry - the TDDD object is a FLAT
 * sketch whose x and y are longitude and latitude, and the sphere is
 * made here. The third component stored is the latitude ANGLE, not
 * its sine. formats/tddd.txt 3. */
int Orb_Init(void)
{
	u32 len;
	s32 i, n;

	/* Geom_FlatPattern conditions ONCE: with no chunk loaded it
	 * latches its done-flag over an all-zero table and every shell
	 * quad then adds nothing, silently. orb_release.txt 4. */
	if (Geom_Init()) return 1;
	g_tex = Dat_Chunk("Fcrust", &len);
	if (!g_tex || len < OB_TEX_BYTES) return 1;
	n = (s32)Image_U16((u32)OB_PNTS);
	if (n < 1 || n > OB_MAX_PTS) return 1;
	g_cn = n;
	for (i = 0; i < n; i++) {
		u32 a = (u32)(OB_PNTS + 2L + i * 12L);
		s32 d0 = (s32)Image_U32(a);
		s32 d1 = (s32)Image_U32(a + 4L);
		s32 lon = ((d0 >> 16) << 6) & 0xffffL;
		s32 lat = -((d1 >> 8) - 0x4000L);
		s32 c   = Orb_Cos(((lat >> 8) << 5) & 0x1fffL) >> 7;

		g_cv[i][0] = (Orb_Sin(lon) * c) >> 8;
		g_cv[i][1] = (Orb_Cos(lon) * c) >> 8;
		g_cv[i][2] = lat >> 1;
	}
	n = (s32)Image_U16((u32)OB_EDGE);
	if (n < 1 || n > OB_MAX_EDGES) return 1;
	g_cen = n;
	for (i = 0; i < n; i++) {
		g_ce[i][0] = (s32)Image_U16((u32)(OB_EDGE + 2L + i * 4L));
		g_ce[i][1] = (s32)Image_U16((u32)(OB_EDGE + 4L + i * 4L));
	}
	return 0;
}

/* ORIGIN 0x002d1fdc Orb_DrawTexturedSphere and 0x0036cbb2
 * Temple_DrawTexturedSphere - ONE ROUTINE IN TWO HUNKS, so this is
 * the body both call. They index the same g_SphereChordAngle and the
 * same g_SphereNormalTable at the same 0x10100 bias; the temple's
 * copy differs only in storing every texel where the orb scene gates
 * on the reveal, and in whose texture it samples.
 *
 * Draw_GlowBlob's skeleton with the glow's add replaced by a texture
 * fetch that writes a WHOLE four-byte pixel. orb_release.txt 4.
 * THE EQUATOR SEAM IS THE DEMO'S - do not smooth it. */
void Orb_DrawSphereTo(u8 *dst, s32 w, s32 h, s32 scale,
                      const u8 *tex, s32 cx, s32 cy, s32 radius,
                      s32 reveal, int gated, s32 ang_a, s32 ang_b)
{
	s32 pcx, pcy, pr, x, y, y0, y1, rf;

	if (radius <= 0 || !tex) return;
	pcx = FX_MUL(cx, scale);
	pcy = FX_MUL(cy, scale);
	pr  = FX_MUL(radius, scale);
	if (pr <= 0) pr = 1;
	rf = radius << DEMO_FRAC;
	y0 = pcy - pr; if (y0 < 0) y0 = 0;
	y1 = pcy + pr; if (y1 > h - 1) y1 = h - 1;

	for (y = y0; y <= y1; y++) {
		s32 dyf = PX_TO_DEMO_F(y - pcy, scale);
		s32 dy = dyf >> DEMO_FRAC;
		s32 half, x0, x1, hi;
		u8 *row;

		if (dyf < 0) dyf = -dyf;
		if (dyf > rf) continue;
		/* OURS: the demo reads the chord from g_pSpanHalfWidth, a
		 * table built at run time; gfx.c computes it instead and this
		 * follows that choice. */
		half = Gfx_Isqrt(rf * rf - dyf * dyf) >> DEMO_FRAC;
		x0 = pcx - FX_MUL(half, scale); if (x0 < 0) x0 = 0;
		x1 = pcx + FX_MUL(half, scale); if (x1 > w - 1) x1 = w - 1;
		row = dst + (size_t)(y * w) * PX_BYTES;
		/* ORIGIN 0x002d207e - the row's own chord-angle term, indexed
		 * by (dy, radius). It becomes the HIGH byte of the normal
		 * table's index and is the whole latitude half of the
		 * mapping; dropping it leaves longitude streaks. */
		hi = (s32)(signed char)*Image_At((u32)(OB_CHORDANG
		         + (u32)(s32)(s16)(((dy & 0xffL) << 8) | (radius & 0xffL))));
		for (x = x0; x <= x1; x++) {
			s32 dx = PX_TO_DEMO_F(x - pcx, scale) >> DEMO_FRAC;
			s32 idx = (s32)(s16)(((dx & 0xffL) << 8) | (half & 0xffL));
			const u8 *texel;
			s32 a, t;

			a = (s32)(signed char)*Image_At((u32)(OB_CHORDANG + (u32)idx));
			a >>= 1;
			/* ORIGIN 0x002d20c0 - a WORD index: the row's term above,
			 * the column's here. */
			a = (s32)(s16)(((hi & 0xffL) << 8) | (a & 0xffL));
			t = (s32)Image_S16((u32)(OB_NORMAL + OB_NORMAL_BIAS
			                         + (u32)(a * 2L)));
			t = (t & ~0xffL) | ((t - ang_b) & 0xffL);
			t = (s32)Image_S16((u32)(OB_NORMAL + OB_NORMAL_BIAS
			                         + (u32)(((s32)(s16)t) * 2L)));
			t = (t & ~0xffL) | ((t - ang_a) & 0xffL);
			texel = tex + OB_TEX_BIAS + (s32)(s16)t * 4L;
			if (gated == 1 && (s32)texel[3] - reveal < 0) continue;
			{
				u8 *p = row + (size_t)x * PX_BYTES;

				if (gated == 2) {
					s32 lo = ((s32)p[2] << 8) | p[3];

					if (reveal > lo) continue;
					lo = (((s32)texel[2] << 8) | texel[3]) | reveal;
					p[0] = texel[0]; p[1] = texel[1];
					p[2] = (u8)(lo >> 8); p[3] = (u8)lo;
					continue;
				}
				p[0] = texel[0]; p[1] = texel[1];
				p[2] = texel[2]; p[3] = texel[3];
			}
		}
	}
}

/* ORIGIN 0x002d1fdc Orb_DrawTexturedSphere - the orb scene's own
 * call: its Fcrust texture, and the reveal gate on.
 *
 * gated: 0 none, 1 the reveal cutoff, 2 ORIGIN 0x0036ca9a
 * Temple_DrawTexturedSphereDepth - reveal then carries a DEPTH KEY
 * rather than a cutoff, the texel is kept only where that key is no
 * greater than the word already at the pixel's +2, and the key is
 * OR'd into the stored low word. The frame's LIGHT and blue bytes
 * are the demo's z-buffer in that mode - src/orb.c. */
void Orb_DrawSphere(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                    s32 reveal, s32 ang_a, s32 ang_b)
{
	Orb_DrawSphereTo(f->px, f->w, f->h, f->scale, g_tex,
	                 cx, cy, radius, reveal, 1, ang_a, ang_b);
}

/* ORIGIN 0x002cdc3c Crack_DrawShells - a sine mask that culls points,
 * then a quad and a line per surviving edge. */
void Orb_DrawShells(Relic_Frame *f, s32 sx, s32 sy, s32 radius,
                    s32 phase_a, s32 phase_b, s32 threshold,
                    s32 ang_a, s32 ang_b, s32 scroll)
{
	s32 i;

	g_shells = 0;
	g_live = 0;
	for (i = 0; i < g_cn; i++) {
		s32 x = g_cv[i][0], y = g_cv[i][1], z = g_cv[i][2];

		/* A point survives only when BOTH sines clear the threshold.
		 * Zero through the release; climbing through the Earth shot,
		 * which is what closes the seams. */
		if (Orb_Sin((x >> 2) + phase_a) - threshold < 0
		 || Orb_Sin((z >> 2) + phase_b) - threshold < 0) {
			g_cp[i][0] = OB_CULLED;
			continue;
		}
		Orb_Rotate2Axis(&x, &y, &z, (ang_a << 5) & 0x1fffL,
		                            ((~ang_b) << 5) & 0x1fffL);
		g_live++;
		g_cp[i][0] = (x * radius) >> 5;
		g_cp[i][1] = (z * radius) >> 5;
		g_cd[i] = y;
	}
	for (i = 0; i < g_cen; i++) {
		s32 a = g_ce[i][0], b = g_ce[i][1];
		s32 x0, y0, x1, y1, d0, d1, qx0, qy0, qx1, qy1, t;
		s32 px[3], py[3];

		if (a < 0 || a >= g_cn || b < 0 || b >= g_cn) continue;
		if (g_cp[a][0] == OB_CULLED || g_cp[b][0] == OB_CULLED) continue;
		x0 = g_cp[a][0]; y0 = g_cp[a][1]; d0 = g_cd[a];
		x1 = g_cp[b][0]; y1 = g_cp[b][1]; d1 = g_cd[b];
		if (d1 < d0) {
			t = d0; d0 = d1; d1 = t;
			t = x0; x0 = x1; x1 = t;
			t = y0; y0 = y1; y1 = t;
		}
		/* ORIGIN 0x002cdd2e - the near end must be in front and the
		 * far one is then clipped onto z = 0. */
		if (d0 >= 0) continue;
		if (d1 > 0) {
			s32 dd = d1 - d0;
			if (dd == 0) continue;
			y1 = y0 + (((-d0) * (y1 - y0)) / dd);
			x1 = x0 + (((-d0) * (x1 - x0)) / dd);
		}
		x0 >>= 6; y0 >>= 6; x1 >>= 6; y1 >>= 6;
		qx0 = x0 >> 2; qy0 = y0 >> 2;
		qx1 = x1 >> 2; qy1 = y1 >> 2;

		/* Two triangles sharing edge PQ with the pair at a quarter
		 * radius: a quad running from the crack INWARD, added over an
		 * already-drawn sphere. */
		px[0] = (x0 + sx) << 8;  py[0] = (y0 + sy) << 8;
		px[1] = (x1 + sx) << 8;  py[1] = (y1 + sy) << 8;
		px[2] = (qx0 + sx) << 8; py[2] = (qy0 + sy) << 8;
		Rast_AddTri(f, px, py, OB_SHELL_U, OB_SHELL_V,
		            Geom_FlatPattern(), Geom_FlatPalette(), scroll);
		g_shells++;
		px[0] = (qx1 + sx) << 8; py[0] = (qy1 + sy) << 8;
		Rast_AddTri(f, px, py, OB_SHELL_U, OB_SHELL_V,
		            Geom_FlatPattern(), Geom_FlatPalette(), scroll);
		/* [open] Crack_DrawMidpointGlow sits between these and fires
		 * only once the threshold passes 0x64, so it is inert through
		 * the release and live through the Earth shot. Not drawn. */
		/* ORIGIN 0x002cde14 - the movem pops the QUARTER points back
		 * into D0-D3, so the line lies on the sphere's surface, at the
		 * quad's inner edge, not out at its far end. */
		Gfx_LineAA(f, qx0 + sx, qy0 + sy, qx1 + sx, qy1 + sy);
	}
}

/* ORIGIN 0x002d20fa Space_DrawSunBlob - Orb_DrawSphereTo's skeleton
 * and a DIFFERENT fetch: it samples the ring texture at TWO
 * longitudes one ang_a apart and sums the two into LIGHT alone, with
 * an add.b that wraps rather than saturating.
 *
 * The chord comes out of Gfx_Isqrt where the demo reads it from
 * g_pSpanHalfWidth, as everywhere else in this file.
 *
 * So the sun is a textured sphere turning in the LIGHT byte, not a
 * falloff blob, and Space_HalveBuffer's quartering of this same
 * texture is what fades it between the two shots. */
void Orb_DrawSunBlob(Relic_Frame *f, const u8 *tex, u32 tex_len,
                     s32 cx, s32 cy, s32 radius, s32 ang_a,
                     s32 ang_b)
{
	s32 pcx, pcy, pr, x, y, y0, y1, rf;

	if (radius <= 0 || !tex) return;
	pcx = FX_MUL(cx, f->scale);
	pcy = FX_MUL(cy, f->scale);
	pr  = FX_MUL(radius, f->scale);
	if (pr <= 0) pr = 1;
	rf = radius << DEMO_FRAC;
	y0 = pcy - pr; if (y0 < 0) y0 = 0;
	y1 = pcy + pr; if (y1 > f->h - 1) y1 = f->h - 1;

	for (y = y0; y <= y1; y++) {
		s32 dyf = PX_TO_DEMO_F(y - pcy, f->scale);
		s32 dy = dyf >> DEMO_FRAC;
		s32 half, x0, x1, hi;
		u8 *row;

		if (dyf < 0) dyf = -dyf;
		if (dyf > rf) continue;
		half = Gfx_Isqrt(rf * rf - dyf * dyf) >> DEMO_FRAC;
		x0 = pcx - FX_MUL(half, f->scale); if (x0 < 0) x0 = 0;
		x1 = pcx + FX_MUL(half, f->scale);
		if (x1 > f->w - 1) x1 = f->w - 1;
		row = f->px + (size_t)(y * f->w) * PX_BYTES;
		hi = *Image_At((u32)(OB_CHORDANG
		     + (u32)(s32)(s16)(((dy & 0xffL) << 8) | (radius & 0xffL))));
		for (x = x0; x <= x1; x++) {
			s32 dx = PX_TO_DEMO_F(x - pcx, f->scale) >> DEMO_FRAC;
			s32 idx = (s32)(s16)(((dx & 0xffL) << 8) | (half & 0xffL));
			s32 a, t, o, v;

			a = *Image_At((u32)(OB_CHORDANG + (u32)idx));
			a = (s32)(s16)(((hi & 0xffL) << 8) | (a & 0xffL));
			t = (s32)Image_S16((u32)(OB_NORMAL + OB_NORMAL_BIAS
			                         + (u32)(a * 2L)));
			t = (t & ~0xffL) | ((t - ang_b) & 0xffL);
			t = (s32)Image_S16((u32)(OB_NORMAL + OB_NORMAL_BIAS
			                         + (u32)(((s32)(s16)t) * 2L)));
			t = (t & ~0xffL) | ((t - ang_a) & 0xffL);
			o = OB_TEX_BIAS + (s32)(s16)t * 4L;
			if (o < 0 || (u32)o >= tex_len) continue;
			v = tex[o];
			t = (t & ~0xffL) | ((t - ang_a) & 0xffL);
			o = OB_TEX_BIAS + (s32)(s16)t * 4L;
			if (o >= 0 && (u32)o < tex_len) v += tex[o];
			row[(size_t)x * PX_BYTES + PX_LIGHT] = (u8)v;
		}
	}
}

/* ORIGIN 0x002d417c Warp_ScaleRotateCorner and 0x002d4132
 * Gfx_Rotate2D - the second is the first without its scale, which is
 * why the rotate-only caller's pb never reaches the maths. */
static void warp_corner(s32 x, s32 y, s32 pa, s32 pb, int scaled,
                        s32 *ox, s32 *oy)
{
	s32 c, sn;

	if (scaled) {
		x = (x * pb) >> 8;
		y = (y * pb) >> 8;
	}
	c  = Orb_Cos(pa & 0x1fffL);
	sn = Orb_Sin(pa & 0x1fffL);
	*ox = ((x * c + y * sn) << 2) >> 16;
	*oy = ((y * c - x * sn) << 2) >> 16;
}

/* ORIGIN 0x002d4132 Gfx_Rotate2D - warp_corner without the scale. */
void Orb_Rotate2D(s32 x, s32 y, s32 pa, s32 *ox, s32 *oy)
{
	warp_corner(x, y, pa, 0L, 0, ox, oy);
}

/* ORIGIN 0x002d3f5c Warp_FullscreenAffine - whole longwords, one per
 * pixel, from three transformed corners. */
void Warp_Fill(Relic_Frame *f, const u8 *tex, s32 pa, s32 pb,
               int scaled)
{
	s32 tlx, tly, trx, try_, blx, bly, ax, ay, dx, dy, px, py;

	if (!tex) return;
	warp_corner(0x5000L, -0x3200L, pa, pb, scaled, &trx, &try_);
	warp_corner(-0x5000L, 0x3200L, pa, pb, scaled, &blx, &bly);
	warp_corner(-0x5000L, -0x3200L, pa, pb, scaled, &tlx, &tly);
	ax = (trx - tlx) / DEMO_W;
	ay = (try_ - tly) / DEMO_W;
	dx = (blx - tlx) / DEMO_H;
	dy = (bly - tly) / DEMO_H;

	for (py = 0; py < f->h; py++) {
		s32 r = PX_TO_DEMO_F(py, f->scale) >> DEMO_FRAC;
		s32 u = tlx + dx * r;
		s32 v = tly + dy * r;
		u8 *p = f->px + (size_t)(py * f->w) * PX_BYTES;

		for (px = 0; px < f->w; px++, p += PX_BYTES) {
			s32 c = PX_TO_DEMO_F(px, f->scale) >> DEMO_FRAC;
			s32 uu = u + ax * c;
			s32 vv = v + ay * c;
			/* ORIGIN 0x002d4008 - v's low byte replaced by u's
			 * INTEGER part, both 24.8. orb_release.txt 6. */
			s32 texel = ((OB_TEX_BIAS / 4L)
			          + ((vv & ~0xffL) | ((uu >> 8) & 0xffL))) & 0xffffL;
			const u8 *t = tex + texel * 4L;

			p[0] = t[0]; p[1] = t[1]; p[2] = t[2]; p[3] = t[3];
		}
	}
}

/* OURS */
s32 Orb_ShellsDrawn(void) { return g_shells; }
s32 Orb_PointsLive(void)  { return g_live; }
