/* RELIC - parts 2.7 and 2.8, the window scene.
 *
 * ORIGIN 0x002cceb4 maybe_Space_RunWindowScene. The demo's own word
 * for it is the WINDOW scene, which is why this file is named for
 * that and not for the captain or the pull-back - it is one routine
 * running two phases. See docs/parts.txt sections 2.7 and 2.8.
 *
 * THERE IS A REAL RESOLUTION DROP AND IT IS THE SCENE'S ONE CUT. The
 * hold shows Fwadepic at its own 640 x 400 through a copper list of
 * its own; Space_DisplayOn (0x002d5410) swaps that for the HAM8
 * chunky display at the instant the pull-back starts, and from there
 * the picture is a 160 x 100 texture on a polygon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parts.h"
#include "gfx.h"
#include "dat.h"
#include "geom.h"
#include "ship.h"
#include "space.h"
#include "uv.h"
#include "rast.h"
#include "tex.h"

/* The music cue the scene ends on - armed at 0x002cd07e, polled at
 * 0x002cd08e. parts.h Relic_Gate. */
#define WI_CUE_ORDER 0x15L
#define WI_CUE_ROW   0L
#define WI_TICKS (153L + 2002L)
static Relic_Gate g_cue_gate;

/* THE PHOTO IS HELD ON A CUE OF ITS OWN. 0x002ccfea arms (0x0c,0)
 * and 0x002ccff0 WAITS on it at 0x0021ff50 - a blocking wait, where
 * Music_PollCue only tests - before 0x002cd07e arms the (0x15,0)
 * that ends the scene. So the scene is a hold and then a run, and
 * the two cues use the one armed slot in turn.
 *
 * The fallback is the demo's own span, entry to release: it enters on
 * field 3363 at music 0a.01 and g_PullBackVel 0x002e0be2 leaves zero
 * on 3843 at 0c.02 [observed]. reconstruction.txt 4.5. */
#define WI_HOLD_ORDER 0x0cL
#define WI_HOLD_ROW   0L
#define WI_HOLD       480L
static Relic_Gate g_hold_gate;

/* ORIGIN docs/formats/relic_dat.txt - Fwadepic is an IFF ILBM, and
 * its BMHD reads 640 x 400, EIGHT planes, compression 1. So it is a
 * 256-colour hires image packed with ByteRun1, not a texel block like
 * every other chunk the port reads.
 *
 * IT IS A SCREEN FIRST AND A TEXTURE AFTER, AND THE PART STAYS
 * 160 x 100. Declaring it at 640 x 400 would put every 160 x 100
 * primitive mode 5 draws - the sky, the sun, the ship's screen
 * centres - in a quarter of the frame. The still loses nothing by
 * that: the part is resolution-parametric, so window_still samples
 * the image's own 640 x 400 across whatever the frame is. */
#define WIN_W   640
#define WIN_H   400

/* ORIGIN 0x002d5c66 Anim_SceneTick, its mode 4 - THE PULL-BACK, AND
 * IT ACCELERATES: the position gains the velocity every tick and the
 * velocity gains 9. Seeded at Window_SeedPullBack, and the first loop
 * ends when the position taken >> 2 reaches 0x5000 - so the camera
 * leaves slowly and is still gaining speed when the loop hands over,
 * about 153 ticks in. */
#define PULL_POS0   (-0x5d00L)
#define PULL_ACCEL  9L
#define PULL_END    0x5000L

/* ORIGIN Window_TakeSnapshot - ITS STRIDE IS NOT ITS WIDTH.
 * The copy takes 256 longwords from each row of a 160-longword
 * framebuffer, so 96 pixels of the next row trail every one. That is
 * slop no UV reaches, and the power-of-two stride makes the mapper's
 * row address a shift rather than a multiply. */
#define SNAP_STRIDE 256
#define SNAP_ROWS   100

/* ORIGIN 0x002cff88 CamPath_Setup - the path table is Fspacedata's
 * sub-block past the geometry, and g_CamPathKeys is written there as
 * 0x28. CamPath_Walk indexes it from +0xc, TWELVE BYTES AN ENTRY, and
 * the
 * twelve before that are a DUPLICATE of entry 0 - the interpolator
 * reads P[-1] at -0xc, so segment 0 needs a copy in front of it.
 * Entries 0 to 26 carry a path; the rest of the declared 40 are
 * zero. */
#define PATH_KEYS   40
#define PATH_STRIDE 12
#define PATH_FIRST  0xcL

/* ORIGIN g_CamPathCursor - it is 24.8: the high bits index the
 * keyframe and its low byte is the interpolant, and mode 5 adds TWO
 * a tick. So one segment lasts 128 ticks. */
#define PATH_STEP   2L

/* ORIGIN 0x002cceb4 maybe_Space_RunWindowScene - mode 5's camera
 * starts at yaw
 * -0x120 with no pitch, and Camera_TrackTarget aims it with these
 * offsets. CamPath_Walk resets the yaw to the same -0x120 when the
 * sweep loops.
 *
 * THE OFFSETS ARE NOT CONSTANT: the scene writes -0x2c3 and 0 once,
 * and then CamPath_Walk (0x002d6550) overwrites both every tick
 * from the sine table, so the aim point drifts
 * around the vessel instead of sitting on it. -0x2c3 survives only as
 * the z term's offset. The two indices differ by a factor of two, so
 * the two axes wander at different rates and the camera never quite
 * settles: that slow unease is the shot. */
#define WIN_YAW0    (-0x120L)
#define WIN_TRACK_OZ (-0x2c3L)

/* THE HEAD ANGLE IS INHERITED, NOT RESET. maybe_Space_RunWindowScene
 * seeds its own two parameters and leaves g_HeadAngle alone, so the
 * scene opens with whatever the sun, logo and planet scenes before it
 * accumulated - 0x5838d at mode 5's first frame [observed, on
 * hardware], which is 1411 of 8192 once shifted, about 62 degrees of
 * sky rotation. Starting it at zero leaves the starfield level while
 * the ship is not, and the error only shows once the camera pitches
 * under the hull. The drift from there is -83 a tick in both. */
#define WIN_HEAD0   0x5838dL

static Relic_Cam g_cv;
static s32 g_shipcam[SHIP_CAM];

static const u8 *g_path;
static s32 g_path_keys;
static s32 g_cursor;
static s32 g_cam[3];

static u8 *g_snap;
static s32 g_pull_pos, g_pull_vel;
static s32 g_range_prev;
static s32 g_second_on, g_second_cx, g_second_cy;
static int g_snapped;

static u8 *g_img;                   /* WIN_W * WIN_H, palette indices */
static u8 g_pal[256][3];            /* six-bit, as the frame wants    */

/* ORIGIN 0x002c9a20 - the window pane's texture, 160 x 100, which the
 * demo's own allocation size settles: 0x002e471c declares 0x19000
 * bytes, 256 x 100 four-byte pixels, the same layout as the mode 5
 * snapshot. The ILBM is box-filtered 4 x 4 into it once the hold's
 * full-resolution display is finished with, so what the handover to
 * mode 5 changes is only which surface the pane samples. */
#define PAIR_STRIDE 256
#define PAIR_ROWS   100
static u8 *g_pane;

/* ORIGIN 0x002e0bc2 g_pWindowPairSlot and the slot after it - objects
 * 19 and 20, the surround and the pane. Space_BuildObjects hands them
 * g_pSpaceDataBlocks (0x002e4730) + 19 * 12 and + 20 * 12. */
#define PAIR_SURROUND 19
#define PAIR_PANE     20
static Relic_FaceUv *g_pair_uv[2];
static s32 *g_pair_order[2];

/* ORIGIN 0x002d1608 g_Billboard20Uv - the pane's literal UVs, read off
 * the table: the full 160 x 100 with no inset, unlike object 11's. */
static const s32 PANE_UV[2][6] = {
	{ 0, 99,   0,  0, 160, 99 },
	{ 0,  0, 160,  0, 160, 99 }
};

/* ORIGIN 0x002d128c - THE PANE IS STRETCHED IN X AT BUILD TIME, every
 * vertex c0 = (c0 * 0x10e) >> 8, and Space_BuildObjects' tail does it
 * TO THE LOADED CHUNK IN PLACE. The surround is not touched.
 *
 * SO IT ACCUMULATES, and the window's build is the second the chunk
 * has seen - one space scene builds at field 1291 against the
 * window's 3176 [observed]. On disk the pane is 0x2ce7 to 0x21089 and
 * at the scene it is 0x31f1 to 0x24bec. Projected at the opening
 * depth that is -3 to 161 of 160, centred and a shade over the edges;
 * unstretched it is -4 to 143, the picture a tenth of a frame left of
 * centre and sampling 160 texels across 147 pixels. */
#define PANE_XSCALE 0x10eL
#define PANE_BUILDS 2
static Relic_Vert g_pane_vert[4];

static void build_pane(void);
static int  pair_init(void);
static void pane_stretch(const Relic_Obj *o);
static s32 g_planes, g_comp;
static int g_ready;

/* OURS */
static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* OURS */
static u32 be16(const u8 *p)
{
	return ((u32)p[0] << 8) | p[1];
}

/* ORIGIN the IFF standard's ByteRun1, which is what BMHD's
 * compression 1 means: a signed count byte, then either n+1 literals
 * or one byte repeated 257-n times, and 0x80 is a no-op. Returns the
 * bytes written, so a short row can be told from a long one. */
static u32 unpack_byterun1(const u8 *src, u32 srcn, u8 *dst, u32 dstn)
{
	u32 si = 0, di = 0;
	while (si < srcn && di < dstn) {
		s32 n = (s32)(s8)src[si++];
		if (n >= 0) {
			s32 k = n + 1;
			while (k-- > 0 && si < srcn && di < dstn) dst[di++] = src[si++];
		} else if (n != -128) {
			s32 k = 1 - n;
			u8 v;
			if (si >= srcn) break;
			v = src[si++];
			while (k-- > 0 && di < dstn) dst[di++] = v;
		}
	}
	return di;
}

/* ORIGIN the ILBM BODY layout - PLANES ARE INTERLEAVED PER ROW, not
 * per image: each row carries plane 0's bytes, then plane 1's, and so
 * on, every plane being (width + 15) / 16 * 2 bytes. A bit set in
 * plane p contributes 1 << p to the index, and the HIGH bit of a byte
 * is the LEFTMOST pixel. Reading the planes as whole images instead
 * produces a picture that still looks like a picture, in the wrong
 * colours - the same trap textures.txt records for the texel. */
static void deplane_row(const u8 *row, s32 bpr, s32 planes, u8 *out)
{
	s32 p, x;
	memset(out, 0, (size_t)WIN_W);
	for (p = 0; p < planes; p++) {
		const u8 *src = row + (size_t)p * bpr;
		for (x = 0; x < WIN_W; x++)
			if (src[x >> 3] & (0x80 >> (x & 7)))
				out[x] = (u8)(out[x] | (1 << p));
	}
}

/* OURS */
static s32 path_at(s32 key, int comp)
{
	const u8 *p;
	if (key < 0) key = 0;
	if (key > g_path_keys) key = g_path_keys;
	p = g_path + (u32)(key * PATH_STRIDE + comp * 4);
	return (s32)be32(p);
}

/* ORIGIN 0x002d5824 CamPath_EvalCatmullRom, by Horner with t in 0.8
 * and a halving at the end. The four control points are read at
 * -0xc, 0, +0xc and +0x18 of the walker's pointer, so P[-1] through
 * P[2]:
 *
 *     a = -P[-1] + 3P[0] - 3P[1] + P[2]
 *     b = 2P[-1] - 5P[0] + 4P[1] - P[2]
 *     c = -P[-1] + P[1]
 *     d = 2P[0]
 *     (((a*t >> 8) + b) * t >> 8 + c) * t >> 8 + d, then >> 1
 *
 * A LINEAR BLEND IS NOT A SUBSTITUTE. The spline overshoots a
 * keyframe on purpose, which is what makes the sweep read as one
 * continuous move rather than a series of aimed-at points. */
static s32 path_eval(s32 key, s32 t, int comp)
{
	s32 pm = path_at(key, comp);        /* the entry BEFORE +0xc */
	s32 p0 = path_at(key + 1, comp);
	s32 p1 = path_at(key + 2, comp);
	s32 p2 = path_at(key + 3, comp);
	s32 a = -pm + 3 * p0 - 3 * p1 + p2;
	s32 b = 2 * pm - 5 * p0 + 4 * p1 - p2;
	s32 c = -pm + p1;
	s32 d = 2 * p0;
	s32 v = ((a * t) >> 8) + b;
	v = ((v * t) >> 8) + c;
	v = ((v * t) >> 8) + d;
	return v >> 1;
}

/* OURS. Mode 5's camera, from the cursor. */
static void path_camera(void)
{
	s32 key = g_cursor >> 8;
	s32 t = g_cursor & 0xffL;
	int k;
	if (!g_path) return;
	/* ORIGIN CamPath_Walk - past the last keyframe it resets
	 * the yaw to -0x120 and the cursor to zero, so the sweep loops. */
	if (key >= g_path_keys) {
		g_cursor = 0;
		key = 0;
		t = 0;
	}
	for (k = 0; k < 3; k++) g_cam[k] = path_eval(key, t, k);
}

/* OURS. Walks the IFF chunks, keeps CMAP and BODY, and decodes. */
static int decode_wadepic(const u8 *b, u32 n)
{
	u32 off = 12;
	const u8 *body = 0;
	u32 bodyn = 0;
	s32 bpr, rowbytes;
	u8 *rows = 0;
	s32 y;

	if (n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "ILBM", 4))
		return 1;
	while (off + 8 <= n) {
		u32 len = be32(b + off + 4);
		const u8 *p = b + off + 8;
		if (off + 8 + len > n) break;
		if (!memcmp(b + off, "BMHD", 4) && len >= 13) {
			if (be16(p) != WIN_W || be16(p + 2) != WIN_H) return 1;
			g_planes = p[8];
			g_comp = p[10];
		} else if (!memcmp(b + off, "CMAP", 4)) {
			u32 i, cols = len / 3;
			if (cols > 256) cols = 256;
			for (i = 0; i < cols; i++) {
				/* EIGHT BITS DOWN TO SIX. The frame's channels are
				 * 0..0x3f, which is what the demo's own ramps and
				 * texels use - formats/textures.txt section 1. */
				g_pal[i][0] = (u8)(p[i * 3 + 0] >> 2);
				g_pal[i][1] = (u8)(p[i * 3 + 1] >> 2);
				g_pal[i][2] = (u8)(p[i * 3 + 2] >> 2);
			}
		} else if (!memcmp(b + off, "BODY", 4)) {
			body = p;
			bodyn = len;
			break;
		}
		off += 8 + len + (len & 1);
	}
	if (!body || g_planes < 1 || g_planes > 8) return 1;

	bpr = ((WIN_W + 15) / 16) * 2;
	rowbytes = bpr * g_planes;
	g_img = (u8 *)malloc((size_t)WIN_W * WIN_H);
	rows = (u8 *)malloc((size_t)rowbytes * WIN_H);
	if (!g_img || !rows) { free(rows); return 1; }

	if (g_comp == 1) {
		if (unpack_byterun1(body, bodyn, rows,
		                    (u32)rowbytes * WIN_H)
		        < (u32)rowbytes * WIN_H) {
			/* a short unpack is not fatal - the tail stays zero and
			 * the picture is simply cut off, which is visible */
		}
	} else if ((u32)rowbytes * WIN_H <= bodyn) {
		memcpy(rows, body, (size_t)rowbytes * WIN_H);
	} else {
		free(rows);
		return 1;
	}

	for (y = 0; y < WIN_H; y++)
		deplane_row(rows + (size_t)y * rowbytes, bpr, g_planes,
		            g_img + (size_t)y * WIN_W);
	free(rows);
	return 0;
}

/* OURS */
static int window_init(void)
{
	const u8 *b;
	u32 n = 0;

	g_pull_pos = PULL_POS0;
	g_pull_vel = 0;
	g_snapped = 0;
	g_cursor = 0;
	g_cv.yaw = WIN_YAW0;
	g_cv.pitch = 0;
	g_cv.head = WIN_HEAD0;
	g_cv.angle = 0;
	g_cv.angle2 = 0;
	Space_Init();
	Ship_Init();
	Ship_SetSkipTable(SHIP_SKIP_WINDOW);
	if (!g_path) {
		s32 left = 0;
		const u8 *tail = (Geom_Init() == 0) ? Geom_Tail(&left) : 0;
		if (tail && left >= (s32)PATH_FIRST + PATH_KEYS * PATH_STRIDE) {
			g_path = tail;
			g_path_keys = PATH_KEYS;
		}
	}
	path_camera();
	if (!g_snap)
		g_snap = (u8 *)calloc((u32)SNAP_STRIDE * SNAP_ROWS, PX_BYTES);
	if (!g_pane)
		g_pane = (u8 *)calloc((u32)PAIR_STRIDE * PAIR_ROWS, PX_BYTES);
	pair_init();
	pane_stretch(Geom_Get(PAIR_PANE));
	if (g_ready) return 0;
	b = Dat_Chunk("Fwadepic", &n);
	if (!b) return 0;            /* survivable: the scene draws black */
	if (decode_wadepic(b, n)) return 0;
	build_pane();
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x002c9a20 - the 4 x 4 box filter. The demo reads eight
 * bitplane words 0x50 apart, rebuilds four pixels' palette indices per
 * source row with roxl/roxr, sums the sixteen colours a channel at a
 * time, takes the mean and packs to the frame's own byte order with a
 * zero light byte.
 *
 * Transcribed through the DECODED indices rather than the planes:
 * decode_wadepic has already done the bit gathering, and the mean is
 * the same mean. */
static void build_pane(void)
{
	s32 oy, ox, c;

	if (!g_pane || !g_img) return;
	for (oy = 0; oy < PAIR_ROWS; oy++) {
		u8 *d = g_pane + (u32)(oy * PAIR_STRIDE) * PX_BYTES;
		for (ox = 0; ox < 160; ox++, d += PX_BYTES) {
			s32 sum[3];
			s32 sy, sx;
			sum[0] = sum[1] = sum[2] = 0;
			for (sy = 0; sy < 4; sy++) {
				const u8 *r = g_img + (u32)(oy * 4 + sy) * WIN_W + ox * 4;
				for (sx = 0; sx < 4; sx++)
					for (c = 0; c < 3; c++)
						sum[c] += g_pal[r[sx]][c];
			}
			/* The demo packs from an eight-bit palette and shifts the
			 * sum down two for the frame's six; g_pal is already six,
			 * so only the mean's >> 4 remains. */
			d[PX_R] = (u8)(sum[0] >> 4);
			d[PX_G] = (u8)(sum[1] >> 4);
			d[PX_B] = (u8)(sum[2] >> 4);
			d[PX_LIGHT] = 0;
		}
	}
}

/* ORIGIN 0x002d2d4a maybe_Scene_TransformPair - THE PAIR IS NEVER
 * ROTATED. Only its depth changes, and the depth is the pull-back
 * itself: nothing moves the camera in mode 4, the two objects slide
 * away from it. Vertices come down SIX bits here and not four, so the
 * pair is drawn at a quarter of the ship's scale. */
#define PAIR_NEAR   0x1f4L
#define PAIR_OFF_X  (-0x4b4L - 0x50L)
#define PAIR_OFF_Z  0x3a2L
/* OURS. The demo's quotient plus the remainder its divsl throws away,
 * so a vertex can sit between two columns - the same recovery ship.c
 * makes. Rast_TexTri takes 24.8; returning whole pixels puts the whole
 * pair in the top-left corner of the frame. */
static s32 pair_subpix(s32 num, s32 den, s32 centre)
{
	s32 n = num << 8;
	s32 q = n / den;
	s32 r = n % den;
	s32 frac = ((r < 0 ? -r : r) << 8) / den;
	if (r < 0) frac = -frac;
	return ((q + centre) << 8) + frac;
}

/* ORIGIN 0x002d2d4a maybe_Scene_TransformPair - one vertex of it. */
static int pair_project(const Relic_Vert *v, s32 depth, s32 *sx, s32 *sy)
{
	s32 c0 = v->comp[0] >> 6;
	s32 c1 = (v->comp[1] >> 6) + depth;
	s32 c2 = v->comp[2] >> 6;

	if (c1 <= PAIR_NEAR) return 0;
	*sx = pair_subpix(c0 + PAIR_OFF_X, c1, 0x50L);
	*sy = pair_subpix(c2 + PAIR_OFF_Z, c1, 0x32L);
	return 1;
}

/* ORIGIN 0x002d1878 - Mesh_SortFacesByC2's twin, keyed on comp1
 * instead. The pair is unrotated and comp1 is its depth axis, so
 * far-to-near by comp1 of the first corner is the painter's order. */
static void sort_pair_faces(const Relic_Obj *o, s32 *order)
{
	s32 i, j;
	for (i = 0; i < o->nfaces; i++) order[i] = i;
	for (i = 0; i < o->nfaces - 1; i++)
		for (j = 0; j < o->nfaces - 1; j++) {
			s32 a = o->vert[o->face[order[j]].corner[0]].comp[1];
			s32 b = o->vert[o->face[order[j + 1]].corner[0]].comp[1];
			if (a < b) {
				s32 t = order[j];
				order[j] = order[j + 1];
				order[j + 1] = t;
			}
		}
}

/* ORIGIN 0x002d128c - the pane's stretched vertices. Kept beside the
 * part rather than written back into the shared geometry, which the
 * demo can do because it reloads the chunk and we parse it once. */
static void pane_stretch(const Relic_Obj *o)
{
	s32 i, k;

	if (!o || !o->vert || o->nverts > 4) return;
	for (i = 0; i < o->nverts; i++) {
		g_pane_vert[i] = o->vert[i];
		for (k = 0; k < PANE_BUILDS; k++)
			g_pane_vert[i].comp[0] =
			    (g_pane_vert[i].comp[0] * PANE_XSCALE) >> 8;
	}
}

/* OURS. One of the pair, textured, in its setup-time face order. */
static void pair_draw(Relic_Frame *f, int which, s32 depth,
                      const u8 *tex, s32 rows)
{
	const Relic_Obj *o = Geom_Get(which ? PAIR_PANE : PAIR_SURROUND);
	const Relic_FaceUv *uv = g_pair_uv[which];
	const s32 *order = g_pair_order[which];
	const Relic_Vert *vs;
	s32 n, k;

	if (!o || !o->vert || !uv || !order || !tex) return;
	vs = (which && o->nverts <= 4) ? g_pane_vert : o->vert;
	for (n = 0; n < o->nfaces; n++) {
		s32 i = order[n];
		s32 px[3], py[3];
		int ok = 1;
		for (k = 0; k < 3; k++)
			if (!pair_project(&vs[o->face[i].corner[k]], depth,
			                  &px[k], &py[k])) { ok = 0; break; }
		if (!ok) continue;
		/* Uv_ProjBoxish zeroes the surround's mode words, so that one
		 * IS backface-tested; the pane keeps mode 2 - geom.h. */
		if (o->face[i].mode != 2) {
			s32 e0x = px[0] - px[2], e0y = py[0] - py[2];
			s32 e1x = px[1] - px[2], e1y = py[1] - py[2];
			if ((e1x * e0y - e0x * e1y) >= 0) continue;
		}
		Rast_TexTri(f, px, py, uv[i].u, uv[i].v, tex, rows);
	}
}

/* OURS. Both slots, once the geometry is in hand. */
static int pair_init(void)
{
	s32 w;

	for (w = 0; w < 2; w++) {
		const Relic_Obj *o = Geom_Get(w ? PAIR_PANE : PAIR_SURROUND);
		if (!o || !o->vert) return 1;
		if (g_pair_uv[w]) continue;
		g_pair_uv[w] = (Relic_FaceUv *)calloc((u32)o->nfaces,
		                                      sizeof **g_pair_uv);
		g_pair_order[w] = (s32 *)malloc((u32)o->nfaces
		                                * sizeof **g_pair_order);
		if (!g_pair_uv[w] || !g_pair_order[w]) return 1;
		sort_pair_faces(o, g_pair_order[w]);
		if (w == 0) {
			Uv_Boxish(o, g_pair_uv[w]);
		} else {
			s32 fi, k;
			/* BY SORTED POSITION, not by face - Uv_WriteBillboard20
			 * runs after the pair's face sort the same way
			 * Uv_WriteBillboard11 does. src/ship.h. */
			for (fi = 0; fi < o->nfaces && fi < 2; fi++) {
				s32 fc = g_pair_order[w][fi];
				for (k = 0; k < 3; k++) {
					g_pair_uv[w][fc].u[k] = PANE_UV[fi][k * 2];
					g_pair_uv[w][fc].v[k] = PANE_UV[fi][k * 2 + 1];
				}
				g_pair_uv[w][fc].written = 1;
			}
		}
	}
	return 0;
}

/* OURS. Mode 4's tick. */
static void window_step(s32 tick)
{
	s32 mt;

	Parts_GateStep(&g_hold_gate, tick, WI_HOLD_ORDER, WI_HOLD_ROW,
	               WI_HOLD);
	if (tick <= 0) {
		g_pull_pos = PULL_POS0;
		g_pull_vel = 0;
		g_snapped = 0;
		g_cursor = 0;
		g_cv.yaw = WIN_YAW0;
		g_cv.pitch = 0;
		g_cv.head = WIN_HEAD0;
		g_cv.angle = 0;
		g_cv.angle2 = 0;
		return;
	}
	if (!g_hold_gate.at) return;
	mt = tick - g_hold_gate.at;
	Parts_GateStep(&g_cue_gate, mt, WI_CUE_ORDER, WI_CUE_ROW, WI_TICKS);
	if (mt <= 0) return;
	if ((g_pull_pos >> 2) < PULL_END) {
		g_pull_pos += g_pull_vel;
		g_pull_vel += PULL_ACCEL;
		return;
	}
	/* ORIGIN CamPath_Walk - mode 5 walks it once mode 4 is done, and
	 * its tick then aims the camera at the vessel. */
	g_cursor += PATH_STEP;
	path_camera();
	g_shipcam[0] = g_cam[0];
	g_shipcam[1] = g_cam[1];
	g_shipcam[2] = g_cam[2];
	Space_CamTrack(&g_cv, g_shipcam,
	               Gfx_Sin(g_cursor * 8) >> 4,
	               (Gfx_Sin(g_cursor * 4) >> 3) + WIN_TRACK_OZ);
	g_shipcam[3] = g_cv.yaw;
	g_shipcam[4] = g_cv.pitch;
}

/* ORIGIN 0x002ce5d8 Window_DrawMode5Frame - the same nine latch
 * copies the planet scenes use, then Scene_DrawTexturedSky,
 * Scene_ProjectAndDrawSun, Sprite_StampSaveUnder,
 * Scene_SortAndDrawAll, Sprite_RestoreSaveUnder, Flare_Draw and
 * 0x002ce81c where they have their horizon band.
 *
 * SO IT IS Space_DrawFullFrame'S LIST MINUS THE PLANET, and the ship
 * sits between the save-under stamp and the count exactly as it does
 * there - which is what makes the vessel occlude the sun's flare as
 * the camera swings past it.
 *
 * [open] 0x002ce81c, the last call. The planet scenes put their
 * horizon band here and this scene puts something else; it is not
 * read, so nothing stands in for it. */
static void window_mode5(Relic_Frame *f)
{
	s32 fx, fy, radius, dyaw, dpitch;
	s32 cam[SHIP_CAM];
	int sun_ok;

	/* THE LOOK OF approach.txt 8, AND THIS SCENE TAKES IT WHOLE -
	 * nothing here is baked, so the sky, the sun, both flares and the
	 * vessel all swing together. g_cv is read and never written: it
	 * is the path's own camera and window_step owns it. */
	Space_Look(&dyaw, &dpitch);
	Space_SetCamera(g_cv.yaw + dyaw, g_cv.pitch + dpitch);
	Gfx_SetFlareRayPhase(g_cv.angle >> 12);
	Gfx_SetLightBase(0x00, 0x00, 0x00);
	Frame_Clear(f);
	Space_DrawSky(f, Space_SkyPan(g_cv.angle + dyaw * SPACE_LOOK_SKY,
	                              g_cv.angle2 + dpitch * SPACE_LOOK_SKY),
	              g_cv.head);

	/* ORIGIN 0x002ccf5a - this scene seeds its own sun. */
	sun_ok = Space_Project(-0x1388L, 0xfa0L, &fx, &fy);
	if (!sun_ok) fx = fy = -0x4000;

	radius = 0x50L - (g_range_prev >> 8);
	if (sun_ok && radius > 0)
		Gfx_Flare(f, fx, fy, radius, g_RayArcs, g_ColRampWarm);

	g_range_prev = Space_Range();
	if (sun_ok) Space_StampSaveUnder(f, fx, fy);

	/* THE SNAPSHOT IS THE HULL'S TEXTURE. Bound every frame because
	 * mode 4 only finishes writing it at the handover. */
	Ship_SetBillboard(g_snapped ? g_snap : 0, SNAP_ROWS);
	Ship_LookCam(cam, g_shipcam, dyaw, dpitch);
	Ship_Draw(f, 0, cam);

	if (sun_ok) {
		s32 cx = fx, cy = fy;
		g_second_on = Space_EaseRange(Space_CountSaveUnder(f, &cx, &cy));
		if (g_second_on) { g_second_cx = cx; g_second_cy = cy; }
		Space_StepBand(g_second_on ? cy : -1);
	}
	if (g_second_on && sun_ok) {
		s32 r2 = Space_Range() >> 8;
		if (r2 > 0)
			Gfx_Flare(f, g_second_cx, g_second_cy, r2,
			          g_RaySpikes, g_ColRampWarmB);
	}
	if (sun_ok) Space_DrawFlareGhosts(f, fx, fy);
}

/* OURS - the hold's picture, which the chunky pipeline never carries.
 * 0x002c9918 points the copper at eight 640-wide bitplanes of
 * Fwadepic and fades it up over 127 fields, and that display stands
 * until Space_DisplayOn, so what is held is the ILBM at its own
 * resolution.
 *
 * The box is whatever source rectangle the pixel covers, so scale 4
 * is one texel a pixel and scale 1 is Wadepic_DownsampleToTexture's
 * own 4 x 4 mean, which is the pane's texture. Point-sampling instead
 * sparkles at scale 1 and throws the picture away at scale 4. */
static void window_still(Relic_Frame *f)
{
	s32 y, x;

	Frame_Clear(f);
	if (!g_img || f->w < 1 || f->h < 1) return;
	for (y = 0; y < f->h; y++) {
		s32 y0 = (s32)((long)y * WIN_H / f->h);
		s32 y1 = (s32)(((long)y + 1) * WIN_H / f->h);
		u8 *d = f->px + (u32)(y * f->w) * PX_BYTES;
		if (y1 <= y0) y1 = y0 + 1;
		for (x = 0; x < f->w; x++, d += PX_BYTES) {
			s32 x0 = (s32)((long)x * WIN_W / f->w);
			s32 x1 = (s32)(((long)x + 1) * WIN_W / f->w);
			s32 sx, sy, n = 0, r = 0, g = 0, b = 0;
			if (x1 <= x0) x1 = x0 + 1;
			for (sy = y0; sy < y1; sy++) {
				const u8 *q = g_img + (u32)sy * WIN_W;
				for (sx = x0; sx < x1; sx++, n++) {
					const u8 *c = g_pal[q[sx]];
					r += c[0];
					g += c[1];
					b += c[2];
				}
			}
			d[PX_R] = (u8)(r / n);
			d[PX_G] = (u8)(g / n);
			d[PX_B] = (u8)(b / n);
			d[PX_LIGHT] = 0;
		}
	}
}

/* ORIGIN Window_TakeSnapshot - the framebuffer to it at the demo's
 * own stride, taken once where the sequencer takes it: between the
 * two loops, when the position has run out. */
static void take_snapshot(const Relic_Frame *f)
{
	s32 y, x;

	if (!g_snap || !f->px) return;
	for (y = 0; y < SNAP_ROWS; y++) {
		s32 sy = FX_MUL(y, f->scale);
		u8 *d = g_snap + (u32)(y * SNAP_STRIDE) * PX_BYTES;
		if (sy < 0 || sy >= f->h) continue;
		for (x = 0; x < SNAP_STRIDE; x++, d += PX_BYTES) {
			s32 sx = FX_MUL(x, f->scale);
			const u8 *q;
			if (sx < 0 || sx >= f->w) continue;
			q = f->px + (u32)(sy * f->w + sx) * PX_BYTES;
			d[PX_R] = q[PX_R];
			d[PX_G] = q[PX_G];
			d[PX_LIGHT] = q[PX_LIGHT];
			d[PX_B] = q[PX_B];
		}
	}
	g_snapped = 1;
}

/* ORIGIN 0x002ce656 Window_DrawMode4Frame - ONE call,
 * maybe_Scene_DrawPair, which draws TWO scene slots from
 * g_pWindowPairSlot through maybe_Scene_TransformPair and
 * Poly_DrawFaceList. SO THE WINDOW IS A TEXTURED POLYGON PAIR AND NOT
 * A BLIT, and the pull-back shrinks it because that transform reads
 * the position.
 *
 * [open] maybe_Scene_TransformPair AND THE PAIR'S TEXTURE. Neither is
 * read, so the scaling here is OURS - the image centred and scaled by
 * the demo's own accelerating position, which is its motion applied to
 * a screen-filling rectangle instead of to its two triangles. The
 * picture and the pacing are the demo's; the geometry is a stand-in.
 * parts.txt 2.8. */
static void window_frame(Relic_Frame *f, s32 tick)
{
	(void)tick;
	/* ORIGIN 0x002cd06a Window_EnterMode5 - THE SEQUENCER SWAPS FRAME
	 * ROUTINES. Loop one runs until the pull-back is spent and loop
	 * two takes over, so this is a branch and not a sequence. */
	if (g_snapped) {
		window_mode5(f);
		return;
	}

	/* ORIGIN 0x002cceb4 - the window scene opens with
	 * Ham8_BuildTables(0, 0, 0), so its light channel resolves toward
	 * BLACK and not toward white like the first planet scene's. */
	Gfx_SetLightBase(0x00, 0x00, 0x00);
	if (!g_ready) { Frame_Clear(f); return; }

	/* ORIGIN 0x002cd004 - the scene arms (0x0c,0) and BLOCKS on it
	 * before Space_DisplayOn, so the pair is drawn into a framebuffer
	 * nothing is showing yet and the hold is the still. */
	if (!g_hold_gate.at) { window_still(f); return; }
	Frame_Clear(f);

	/* ORIGIN 0x002d2822 maybe_Scene_DrawPair - TWO CONSECUTIVE SLOTS
	 * FROM g_pWindowPairSlot, transformed and drawn one after the
	 * other, and that is the whole of mode 4. The surround goes down
	 * first: it is 158 faces of hull around the opening, which is
	 * close-range detail the later shots do without, and the pane's
	 * two triangles sit in it. docs/parts.txt 2.8.
	 *
	 * The pull-back, taken >> 2, IS the depth both of them get. */
	pair_draw(f, 0, g_pull_pos >> 2, Tex_Base(2), Tex_Rows(2));
	pair_draw(f, 1, g_pull_pos >> 2, g_pane, PAIR_ROWS);

	/* ORIGIN Window_EnterMode5 - taken when loop one ends. Mode 5 then
	 * textures object 11 from it through Uv_WriteBillboard11, and its
	 * own frame is Space_DrawFullFrame's list minus the planet, so
	 * most of it already exists in src/part_portal.c. Wiring that is
	 * [open]. */
	if (!g_snapped && (g_pull_pos >> 2) >= PULL_END)
		take_snapshot(f);
}

/* OURS */
static void window_stats(char *out, int cap)
{
	(void)cap;
	if (!g_ready) {
		sprintf(out, "no Fwadepic - the scene draws black");
		return;
	}
	sprintf(out, "pull %ld/%ld snap %ld | key %ld.%02ld cam %ld,%ld,%ld"
	             " yaw %ld pit %ld hd %ld | ship %ld",
	        (long)(g_pull_pos >> 2), (long)PULL_END, (long)g_snapped,
	        (long)(g_cursor >> 8), (long)(g_cursor & 0xffL),
	        (long)g_cam[0], (long)g_cam[1], (long)g_cam[2],
	        (long)g_cv.yaw, (long)g_cv.pitch, (long)g_cv.head,
	        (long)Ship_FacesDrawn());
}

/* OURS - parts.h. */
static s32 window_length(void)
{
	s32 hold = g_hold_gate.at ? g_hold_gate.at : WI_HOLD;

	return hold + Parts_GateLength(&g_cue_gate, WI_TICKS);
}

const Relic_Part PART_Window = {
	"2.7", "The captain at the window",
	"maybe_Space_RunWindowScene 0x002cceb4",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	window_init, window_step, window_frame, window_stats,
	/* MEASURED: mode 5 runs frames 139 to 2141 on hardware, so
	 * 2002 ticks, and the pull-back before it runs the 153 the
	 * acceleration needs to reach PULL_END. The sequencer cuts mode 5
	 * on a music cue - 0x002cd08e - so the 2002 is the cue's, read
	 * off the run and not off the code. */
	WI_HOLD + WI_TICKS, 0, window_length
};
