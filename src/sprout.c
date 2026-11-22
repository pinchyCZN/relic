/* RELIC - the Sprout octree and its shading. See sprout.h.
 *
 * ORIGIN 0x002d6a74 maybe_Sprout_LoadAndBuild, builder 0x002d6b8e,
 * tables 0x002d6a34 and 0x002d69a6. The renderer that walks this is
 * effects/approach.txt section 4.6 and is not here yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sprout.h"
#include "gfx.h"
#include "dat.h"

#define SPROUT_HDR   0x0c           /* magic, then two counts       */
#define PROD_SIZE    (256 * 256)    /* the A4 table, signed words   */
#define RAMP_SIZE    256            /* the A5 table, one pixel each */

static Relic_SproutNode *g_pool;    /* every node, one allocation   */
static s32   g_pool_used;
static s32   g_pool_size;
static Relic_SproutNode *g_root;
static s16  *g_prod;                /* A4 */
static u8   *g_ramp;                /* A5, RAMP_SIZE * PX_BYTES     */
static int   g_ready;

static s32   g_n_leaf, g_n_branch, g_depth;

/* ORIGIN 0x002d6a34 maybe_Sprout_BuildProductTable - a signed
 * 256 x 256 product table in 8.7 fixed point. The demo biases its
 * base by +0x10000 so a signed word indexes it; here the index is
 * built unsigned instead, which is the same entry. */
static int build_product(void)
{
	s32 a, b;

	g_prod = (s16 *)malloc((size_t)PROD_SIZE * sizeof(s16));
	if (!g_prod) return 1;
	for (a = -0x80; a < 0x80; a++)
		for (b = -0x80; b < 0x80; b++)
			g_prod[((a & 0xff) << 8) | (b & 0xff)] = (s16)((a * b) >> 7);
	return 0;
}

/* ORIGIN 0x002d69a6 maybe_Sprout_BuildShadeRamp - Lambert plus a
 * specular of exponent 32, clamped to the six-bit channel maximum.
 * Four squarings give c^16 and the fifth in the sum makes it c^32.
 *
 * THE LIGHT BYTE IS ZERO AND THAT IS THE DEMO'S, not a simplification
 * here: maybe_Sprout_BuildShadeRamp computes a third channel in six
 * instructions and then zeroes it before the store, so that
 * arithmetic has no effect. Every live entry reads (v, v, 00, v) and
 * entries 112 to 119 match this formula exactly. */
static int build_ramp(void)
{
	s32 d;

	g_ramp = (u8 *)malloc((size_t)RAMP_SIZE * PX_BYTES);
	if (!g_ramp) return 1;
	for (d = -0x80; d < 0x80; d++) {
		s32 c = d < 0 ? 0 : d;
		s32 t = c, v;
		u8 *p = g_ramp + ((d & 0xff) * PX_BYTES);
		t = (t * t) >> 7;
		t = (t * t) >> 7;
		t = (t * t) >> 7;
		t = (t * t) >> 7;
		v = (c >> 4) + ((t * t) >> 5);
		if (v > 0x3f) v = 0x3f;
		p[PX_R] = (u8)v;
		p[PX_G] = (u8)v;
		p[PX_LIGHT] = 0;
		p[PX_B] = (u8)v;
	}
	return 0;
}

/* OURS */
static Relic_SproutNode *node_alloc(void)
{
	Relic_SproutNode *n;

	if (g_pool_used >= g_pool_size) return NULL;
	n = &g_pool[g_pool_used++];
	memset(n, 0, sizeof *n);
	return n;
}

/* ORIGIN 0x002d6b8e maybe_Sprout_BuildNode - the encoding, which is
 * proven: a zero first byte is a LEAF of seven bytes, three colour
 * components taken >> 4 and three normals; anything else is a BRANCH
 * of one byte whose bits say which of eight children follow.
 *
 * The walk consumes Fsprout exactly - 10652 * 7 + 3862 * 1 + 12 is
 * the chunk's 78,438 bytes - which is what proves it. */
static Relic_SproutNode *parse(const u8 *b, u32 size, u32 *p, s32 depth)
{
	Relic_SproutNode *n;
	u32 mask;
	int i;

	if (*p >= size) return NULL;
	if (depth > g_depth) g_depth = depth;
	mask = b[*p];
	n = node_alloc();
	if (!n) return NULL;

	if (mask == 0) {
		if (*p + 7 > size) return NULL;
		n->is_leaf = 1;
		n->colour[PX_R] = (u8)(b[*p + 1] >> 4);
		n->colour[PX_G] = (u8)(b[*p + 2] >> 4);
		n->colour[PX_LIGHT] = 0;
		n->colour[PX_B] = (u8)(b[*p + 3] >> 4);
		n->normal[0] = (s8)b[*p + 4];
		n->normal[1] = (s8)b[*p + 5];
		n->normal[2] = (s8)b[*p + 6];
		*p += 7;
		g_n_leaf++;
		return n;
	}

	*p += 1;
	g_n_branch++;
	for (i = 0; i < 8; i++) {
		if (mask & (1UL << i)) {
			n->child[i] = parse(b, size, p, depth + 1);
			if (!n->child[i]) return NULL;
		}
	}
	return n;
}

/* OURS */
int Sprout_Init(void)
{
	const u8 *b;
	u32 size = 0, p = SPROUT_HDR;
	u32 n_branch, n_leaf;

	if (g_ready) return 0;
	b = Dat_Chunk("Fsprout", &size);
	if (!b || size < SPROUT_HDR) {
		fprintf(stderr, "sprout: no Fsprout (relic.dat not open?)\n");
		return 1;
	}
	if (b[0] != 'S' || b[1] != 'P' || b[2] != 'R' || b[3] != 'T') {
		fprintf(stderr, "sprout: Fsprout is not SPRT\n");
		return 1;
	}
	n_branch = ((u32)b[4] << 24) | ((u32)b[5] << 16)
	         | ((u32)b[6] << 8) | b[7];
	n_leaf   = ((u32)b[8] << 24) | ((u32)b[9] << 16)
	         | ((u32)b[10] << 8) | b[11];

	g_pool_size = (s32)(n_branch + n_leaf);
	g_pool = (Relic_SproutNode *)malloc((size_t)g_pool_size
	                                    * sizeof(Relic_SproutNode));
	if (!g_pool) return 1;
	g_pool_used = g_n_leaf = g_n_branch = g_depth = 0;

	g_root = parse(b, size, &p, 0);
	if (!g_root) {
		fprintf(stderr, "sprout: the tree does not parse\n");
		return 1;
	}
	/* THE HEADER IS THE CHECK. Its two counts and the exact end of
	 * the payload all have to fall out of the walk; if any of them
	 * does not, the encoding is not what approach.txt says. */
	if ((u32)g_n_branch != n_branch || (u32)g_n_leaf != n_leaf
	    || p != size) {
		fprintf(stderr, "sprout: walk gave %ld/%ld nodes to %lu of %lu\n",
		        (long)g_n_branch, (long)g_n_leaf,
		        (unsigned long)p, (unsigned long)size);
		return 1;
	}
	if (build_product() || build_ramp()) return 1;
	g_ready = 1;
	return 0;
}

/* OURS */
int Sprout_Ready(void) { return g_ready; }

/* OURS */
void Sprout_Free(void)
{
	if (g_pool) { free(g_pool); g_pool = NULL; }
	if (g_prod) { free(g_prod); g_prod = NULL; }
	if (g_ramp) { free(g_ramp); g_ramp = NULL; }
	g_root = NULL;
	g_ready = 0;
}

/* ORIGIN 0x002d7172 maybe_Sprout_ShadeLeaf0 - a normal component is
 * pasted into the low byte of its own light term, the product table
 * turns the pair into one term of a dot product, and the three are
 * summed. light_fx carries the three terms the demo latches as
 * 0x002e09f6, fa and fe. */
s32 Sprout_Shade(const s8 *normal, const s32 *light_fx)
{
	s32 i, sum = 0;

	for (i = 0; i < 3; i++) {
		u32 idx = ((u32)light_fx[i] & 0xff00UL)
		        | ((u32)normal[i] & 0xffUL);
		sum += g_prod[idx];
	}
	return sum;
}

/* OURS. The ramp is 256 entries and the demo indexes it with a word
 * biased by +0x200, so a sum outside -128..127 reaches entries the
 * builder never wrote. What the demo does there is not established -
 * approach.txt section 4.4 - so this clamps rather than invent a
 * wrap, and says so. */
const u8 *Sprout_ShadePixel(s32 shade)
{
	if (shade < -0x80) shade = -0x80;
	if (shade > 0x7f) shade = 0x7f;
	return g_ramp + ((shade & 0xff) * PX_BYTES);
}

/* OURS. The hit rate over a coarse grid of rays is the check that the
 * traversal reaches geometry at all: nothing says the descent is
 * broken, everything says its guards never reject. */
static s32 probe_hits(void)
{
	s32 gx, gy, n = 0;

	for (gy = 0; gy < 32; gy++)
		for (gx = 0; gx < 32; gx++) {
			s32 u = -0x1000L + gx * (0x2000L / 32);
			s32 v = -0x1000L + gy * (0x2000L / 32);
			if (Sprout_Pixel(0, u, v, 0, NULL))
				n++;
		}
	return n;
}

/* OURS. THE WATCH LOG SAYS THE DEMO'S ROWS ARE SOLID RUNS - fifteen
 * consecutive pixels from one visitor, approach.txt section 4.4.6 -
 * so any ray in a span that returns nothing is a defect. This reports
 * the longest unbroken run each sector manages along one row, which
 * is the number to drive to the span's full width. */
s32 Sprout_ProbeRun(s32 sector, s32 slope, s32 v, s32 pixels)
{
	s32 i, run = 0, best = 0;

	if (!g_ready || pixels < 1) return 0;
	for (i = 0; i <= pixels; i++) {
		s32 u = -0x1000L + i * (0x2000L / pixels);
		if (Sprout_Pixel(sector, u, v, slope, NULL)) {
			run++;
			if (run > best) best = run;
		} else {
			run = 0;
		}
	}
	return best;
}

/* OURS */
void Sprout_Stats(char *out, int cap)
{
	(void)cap;
	if (!g_ready) {
		sprintf(out, "sprout not loaded");
		return;
	}
	sprintf(out, "sprout %ld branch %ld leaf depth %ld  probe %ld/1024",
	        (long)g_n_branch, (long)g_n_leaf, (long)g_depth,
	        (long)probe_hits());
}

/* ORIGIN 0x002d6cae - the occupancy index's bit c selects this child
 * slot, and the far y-slab is two slots on. The slots are read off
 * maybe_Sprout_Walk0_Cols01 and its sibling, which fetch children at
 * +0x00 and +0x10. */
const int Sprout_ColSlot[4] = { 0, 1, 4, 5 };

/* ORIGIN 0x002d6f98 and its three siblings - the four fillers are one
 * routine with the angle rotated a quarter turn, so this takes the
 * sector rather than being written out four times. The rotations are
 * 0, -0x800, -0x1000 and +0xe800; the last reduces to +0x800, so the
 * four are quarter turns 0, 3, 2, 1 - approach.txt section 4.5. */
static const s32 SECTOR_ROT[4] = { 0L, -0x800L, -0x1000L, 0x800L };

/* Which corner pair bounds each sector's span. SECTORS 0 AND 1 ARE
 * READ off their fillers, which take g_SproutCorner2 - g_SproutCorner3
 * and g_SproutCorner3 - g_SproutCorner, and both agree with the live
 * corner array: sector 0's span comes out negative there and sector
 * 1's positive, exactly as the demo's own ble decides. SECTORS 2 AND 3
 * CONTINUE THE RING and are not read - see approach.txt section
 * 4.6. */
static const int SECTOR_FROM[4] = { 3, 0, 1, 2 };
static const int SECTOR_TO[4]   = { 2, 3, 0, 1 };

/* ORIGIN 0x002d7060 maybe_Sprout_FillSector0 - the span geometry, one
 * sector at a time. */
int Sprout_Span(Relic_SproutSpan *out, s32 sector, s32 angle,
                s32 scale, const s32 *corner)
{
	s32 a, c, sn, from, to, span;

	if (!out || sector < 0 || sector > 3 || !corner) return 0;
	a = (angle + SECTOR_ROT[sector]) & 0x1fffL;
	c = Gfx_Cos(a);
	/* THE QUADRANT TEST. A sector only draws while its own rotated
	 * frame faces forward, which is what lets one span-walk serve
	 * every orientation. */
	if (c <= 0) return 0;
	sn = Gfx_Sin(a);
	out->slope = (sn << 13) / c;
	if (out->slope > 0xffffL || out->slope < -0xffffL) return 0;

	from = corner[SECTOR_FROM[sector]];
	to   = corner[SECTOR_TO[sector]];
	span = to - from;
	if (span <= 0) return 0;

	/* THE CORNER IS AN X OFFSET, NOT A ROW. The filler's
	 * lea ([0x2f101c,-,D0w*0x4]),A1 is memory indirect POST-indexed -
	 * extension word 0x05b5, BS set and I/IS 101 - so it loads
	 * g_pFrameBuffer and then adds the index, four bytes a pixel.
	 * Read as pre-indexed it would be a row-pointer lookup, and the
	 * four sectors would stack vertically instead of tiling side by
	 * side: the object comes out with strips of it see-through. */
	out->x0 = from >> 8;
	out->pixels = span >> 8;
	/* THE LOW WORD, not the high one: the filler reads the word at
	 * g_SproutScale + 2 and this is big-endian. Taking the high
	 * word gives -1 rows and draws nothing. */
	out->rows = (scale & 0xffffL) - 1;
	if (out->pixels < 1 || out->rows < 1) return 0;
	/* 0x200000 / span across, 0x2000 / scale down - both divides are
	 * the filler's own. */
	out->step_pixel = 0x200000L / span;
	out->step_row = 0x2000L / scale;
	return 1;
}

/* ORIGIN 0x002d6b8e - which coordinate a column advances depends on
 * the SECTOR, since each reads its own split triple. Derived from the
 * builder's bit tests; see effects/approach.txt section 4.3.
 * IGNORING THE SECTOR makes every strip draw the same traversal, so
 * the object appears once per strip at a different compression with a
 * hard seam between them. */
static const int COL_ADVANCE[4][4] = {
	{ 0, 1, 0, 1 },
	{ 1, 1, 0, 0 },
	{ 1, 0, 1, 0 },
	{ 0, 0, 1, 1 }
};

/* THE VISIT ORDER PER SECTOR AND SLOPE SIGN, decoded by
 * tools/sprout/walkdecode.py `order` as { first, the far-clipping
 * middle, the near-clipping middle, last }. The filler dispatches
 * through node+0x20 for a positive slope and +0x24 for a negative
 * one, and THOSE ARE DIFFERENT ROUTINES with different orders and
 * mirrored guards - see effects/approach.txt section 4.4.7.
 *
 * THE ORDER IS WHAT MAKES THE WALK FRONT-TO-BACK: a wrong one loses
 * no pixels, it lets a FAR voxel win the ray. And one set used for
 * both signs fails outright on half the rotations - a longest run of
 * 0 of 15 where the right set gives 14 - which reads as quadrants of
 * the object vanishing as it turns. */
static const int ORDER_NEG[4][4] = {
	{ 1, 0, 3, 2 },
	{ 0, 2, 1, 3 },
	{ 2, 3, 0, 1 },
	{ 3, 1, 2, 0 }
};
static const int ORDER_POS[4][4] = {
	{ 0, 1, 2, 3 },
	{ 2, 0, 3, 1 },
	{ 3, 2, 1, 0 },
	{ 1, 3, 0, 2 }
};

/* OURS. A GENERIC FRONT-TO-BACK OCTREE RAYCAST, which is what the
 * demo's ninety-six specialised walk routines unroll between them.
 * The first leaf the ray meets wins, which is the same pixel their
 * visit order produces - so this is the maths of rule 16 rather than
 * the shape of the assembler.
 *
 * The decision below IS the demo's, decoded from sector 0's
 * all-four-columns routine: col1, then col0 or col3 by the midpoint,
 * then col2, with the interval narrowed at each step. What is ours is
 * skipping an absent column with a test instead of with a different
 * routine, and tracking the node's bounds during the descent instead
 * of precomputing them per node.
 *
 * lo is the node's lower bound along THIS SECTOR'S axis - each sector
 * reads its own triple, approach.txt section 4.3 - centre is the
 * node's middle along the shared row axis, and ext is the extent the
 * builder would have been entered with. */
static const Relic_SproutNode *cast(const Relic_SproutNode *n,
                                    s32 lo, s32 centre, s32 ext,
                                    s32 near, s32 far, s32 row,
                                    s32 sector, int pos_slope)
{
	const int *ord = pos_slope ? ORDER_POS[sector] : ORDER_NEG[sector];
	s32 h, mid, pos;
	int slab;

	if (!n) return NULL;
	if (n->is_leaf) return n;

	h = ext >> 1;
	mid = (near + far) >> 1;
	/* The row decides which y-slab, exactly as the demo's compare
	 * against split[0] does; the far slab is two child slots on. */
	slab = (row < centre) ? 0 : 2;

	/* THREE POSITIONS, NOT THREE FIXED COLUMNS: first, whichever
	 * middle the midpoint selects, then last. The guards and the
	 * clipping go by position; which column each position is comes
	 * from the sector's own order. */
	for (pos = 0; pos < 3; pos++) {
		const Relic_SproutNode *child;
		s32 cn = near, cf = far, clo;
		int c, clip_far;

		if (pos == 0) {
			c = ord[0];
			clip_far = 1;
			if (pos_slope) {
				if (!(near < lo + h && mid >= lo)) continue;
			} else {
				if (!(near >= lo + h && mid < lo + 2 * h)) continue;
			}
		} else if (pos == 1) {
			/* THE MIDDLE DECISION IS INVERTED BETWEEN THE SIGNS. The
			 * negative routine branches bge on mid against lo + h and
			 * the positive one blt, so the fall-through arm - the one
			 * that clips the FAR end - is mid < lo + h for a negative
			 * slope and mid >= lo + h for a positive one. */
			int first_arm = pos_slope ? (mid >= lo + h) : (mid < lo + h);
			if (first_arm) { c = ord[1]; clip_far = 1; }
			else           { c = ord[2]; clip_far = 0; }
		} else {
			c = ord[3];
			clip_far = 0;
			if (pos_slope) {
				if (!(far >= lo + h && mid < lo + 2 * h)) continue;
			} else {
				if (!(far < lo + h && mid >= lo)) continue;
			}
		}

		child = n->child[Sprout_ColSlot[c] + slab];
		if (!child) continue;
		/* THE GUARD SENSE IS THE NEGATION OF THE BRANCH: cmp.w
		 * (off,A0),Dn computes Dn - split and the branch target is
		 * the skip, so the first position's `blt -> skip` means it
		 * draws when near >= lo + h. Reading a branch as the visit
		 * condition inverts the test and punches holes through the
		 * object. */
		if (clip_far) cf = mid; else cn = mid;
		clo = lo + (COL_ADVANCE[sector][c] ? h : 0);
		{
			const Relic_SproutNode *r =
			    cast(child, clo,
			         centre + ((slab ? h : -h) >> 1), h, cn, cf, row,
			         sector, pos_slope);
			if (r) return r;
		}
	}
	return NULL;
}

/* OURS. One pixel: build the ray the filler would have, cast it, and
 * hand back the leaf or NULL. u and v sweep [-0x1000, +0x1000] across
 * the span and down the rows - the filler starts both at -0x1000 and
 * steps 0x200000/span and 0x2000/scale. */
const u8 *Sprout_Pixel(s32 sector, s32 u, s32 v, s32 slope,
                       const s32 *light_fx)
{
	const Relic_SproutNode *leaf;

	if (!g_ready || !g_root) return NULL;
	if (sector < 0 || sector > 3) return NULL;
	leaf = cast(g_root, -0x1000L, 0L, 0x2000L, u, u + slope, v,
	            sector, slope >= 0);
	if (!leaf) return NULL;
	if (!light_fx) return leaf->colour;     /* the probe wants the hit */
	return Sprout_ShadePixel(Sprout_Shade(leaf->normal, light_fx));
}

/* ORIGIN 0x002e0a22 g_SproutSquare - the bounding square in object
 * space, half-extent 128 [observed, on hardware]. */
static const s32 SQUARE[8] = { -128, -128,  128, -128,
                                128,  128, -128,  128 };

/* ORIGIN 0x002d6eb0 maybe_Light_DrawStages - the whole object. Rotate
 * the bounding square, then fill the four sectors.
 *
 * EVERYTHING HERE IS 16-BIT. The visitors compare with cmp.w, and the
 * filler advances its sweep with add.w, so the ray parameters are
 * signed words sweeping -0x1000 to +0x1000 and the splits are words
 * too. Keeping them in s32 is the same arithmetic while they stay in
 * range, which they do by construction.
 *
 * The leaf writes a whole longword over the pixel - move.l D1,(A1) -
 * so the light is OPAQUE, not additive like the flare. */
int Sprout_Draw(Relic_Frame *f, s32 cx, s32 cy, s32 scale, s32 angle,
                const s32 *light_fx)
{
	s32 corner[4], sec, i, c, sn, drawn = 0;

	if (!g_ready || !f || scale < 1) return 0;
	c = Gfx_Cos(angle & 0x1fffL);
	sn = Gfx_Sin(angle & 0x1fffL);
	/* (scale * (cos*x + sin*y)) >> 14, low byte masked off - so each
	 * corner is a whole pixel carried in 24.8. */
	for (i = 0; i < 4; i++)
		corner[i] = ((scale * ((c * SQUARE[i * 2]
		                      + sn * SQUARE[i * 2 + 1]) >> 14)))
		            & ~0xffL;

	for (sec = 0; sec < 4; sec++) {
		Relic_SproutSpan sp;
		s32 py;
		if (!Sprout_Span(&sp, sec, angle, scale, corner)) continue;
		for (py = 0; py <= sp.rows; py++) {
			s32 v = -0x1000L + py * sp.step_row;
			s32 u = -0x1000L;
			s32 px;
			for (px = 0; px <= sp.pixels; px++) {
				const u8 *src = Sprout_Pixel(sec, u, v, sp.slope,
				                             light_fx);
				u += sp.step_pixel;
				if (!src) continue;
				/* the demo's 160 x 100 cell, scaled into the frame */
				{
					s32 dx = cx + sp.x0 + px, dy = cy + py;
					s32 x0 = FX_MUL(dx, f->scale);
					s32 x1 = FX_MUL(dx + 1, f->scale);
					s32 y0 = FX_MUL(dy, f->scale);
					s32 y1 = FX_MUL(dy + 1, f->scale);
					s32 yy, xx;
					for (yy = y0; yy < y1 && yy < f->h; yy++) {
						u8 *row;
						if (yy < 0) continue;
						row = f->px + (size_t)(yy * f->w) * PX_BYTES;
						for (xx = x0; xx < x1 && xx < f->w; xx++) {
							if (xx < 0) continue;
							memcpy(row + (size_t)xx * PX_BYTES, src,
							       PX_BYTES);
						}
					}
					drawn++;
				}
			}
		}
	}
	return drawn;
}
