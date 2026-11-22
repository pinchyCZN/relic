/* RELIC - the ship. See ship.h and docs/formats/geometry.txt.
 *
 * WHAT IS HERE: Fspacedata's objects, Scene_TransformProject's own
 * transform chain, and Draw_LineAA, which is the demo's line routine.
 * So the vessel's shape and the form of its projection are the
 * demo's.
 *
 * WHAT IS NOT: the textured fill. Texture coordinates are generated
 * per face rather than stored, and Rast_FillSpansTextured's
 * fixed-point layout is unsettled - subsystems/renderer.txt section 3.
 * A flat or guessed fill would be a rule 12 substitution, so the
 * edges are drawn and the fill is left [open].
 */


#include <stdio.h>
#include "ship.h"
#include "gfx.h"
#include "geom.h"
#include "tex.h"
#include "uv.h"
#include "rast.h"
#include "image.h"
#include <stdlib.h>

/* ORIGIN 0x002d2cd4 Scene_TransformProject - the camera distance added
 * to the depth. The near plane it rejects at is ship.h. */
#define CAM_DIST    0x1f4L

/* ORIGIN geometry.txt section 6 - the objects Space_BuildObjects
 * binds through Uv_GenerateFaceParams, every field read off its setup
 * code. The translucent 5..7, the boxish 19 and the billboards 11 and
 * 20 each use a different generator and are [open].
 *
 * OBJECTS 3 AND 4 TAKE TWO PASSES and the second does NOT replace the
 * first: a flat base coat over every face, then a cylindrical band
 * that only rewrites the faces inside its range. Keeping only the
 * second leaves most of each object untextured.
 *
 * HOW A PART'S VERTICES ARE MADE. Three objects are drawn straight
 * out of the chunk; the spike ring is one object instanced eight times
 * and the six struts are one chain bent three ways and placed around
 * a hub. UV_FROM tells Uv_Generate which pose to read, and the two
 * cases DISAGREE: Spike_BuildRing generates from the ROTATED
 * vertices, while Mesh_DeformAndMap points the object back at its
 * REST pose first and restores it afterwards. */
#define XF_NONE   0
#define XF_SPIKE  1
#define XF_STRUT  2

#define XF_BILLBOARD 3

typedef struct {
	s32 obj;
	s32 tex;
	s32 npass;
	Relic_UvPass pass[2];
	s32 xform;
	s32 xa;             /* spike: angle.  strut: the hub angle */
	s32 xr;             /* spike: radius */
	s32 bias;           /* taken off the centroid's c2 */
	/* ORIGIN 0x002d155c Mesh_MakeAdditiveEnvelope - the demo stores 1
	 * into every face of these objects, which sends them to the
	 * additive rasteriser. A per-object flag is the same thing: it
	 * sets the mode for all of an object's faces and nothing else
	 * does. APPENDED, so every row above keeps its initialiser. */
	s32 additive;
} Ship_Part;

/* ORIGIN 0x002d15d8 g_Billboard11Uv - two triangles over 1,1 to
 * 159,98, which is the whole 160 x 100 frame inset by a pixel. Read
 * off the table rather than derived, because the inset is the point:
 * it keeps the sampler off the snapshot's edge, where the 256-stride
 * copy leaves 96 pixels of the next row trailing. */
static const s32 BILLBOARD_UV[2][6] = {
	{   1,  1, 159,  1, 159, 98 },
	{   1, 98,   1,  1, 159, 98 }
};

/* ORIGIN 0x002d155c Mesh_MakeAdditiveEnvelope, +0x20 - g_FlatEnvelopeUv
 * at 0x002d1590 is SIX LONGWORDS PER FACE, the copy's cursor ADVANCES,
 * and each block is the same three pairs permuted - so reusing block 0
 * looks harmless and shears two faces of three. effects/orb.txt 9. */
#define FLAT_UV_ADDR   0x002d1590UL
#define FLAT_UV_FACES  3L
#define FLAT_UV_STRIDE 24L

/* OURS - face n's block, read from the image. */
static int flat_uv(s32 face, s32 *u, s32 *v)
{
	const u8 *p;
	int k;

	if (face < 0 || face >= FLAT_UV_FACES) return 0;
	p = Image_At((u32)(FLAT_UV_ADDR + face * FLAT_UV_STRIDE));
	if (!p) return 0;
	for (k = 0; k < 3; k++) {
		u[k] = RD_S32(p + k * 8);
		v[k] = RD_S32(p + k * 8 + 4);
	}
	return 1;
}

/* ORIGIN 0x002cf550 Strut_RetextureToTex3 - the claws change texture
 * and nothing else about them does, so one override is the whole of
 * it. -1 means the table's own choice. */
static s32 g_claw_tex = -1;

/* ORIGIN 0x002e0afc g_FlatScroll, and whether the shells are
 * revealed - phase 3 does both in the demo. */
static s32 g_flat_scroll;
static int g_additive_lit;
static const u8 *g_flat_pattern;
static const u8 *g_flat_palette;

static const u8 *g_bill_tex;
static s32 g_bill_rows;
static u32 g_skip;

static const Ship_Part SHIP_PART[] = {
 /* ORIGIN 0x002d0990 Space_BuildObjects - THE SLOT ORDER IS THE
  * DEMO'S, not the chunk's object order, because the two skip tables
  * index it. The whole map is docs/formats/geometry.txt 6.1.
  *
  * ORIGIN 0x002d09f4, 0x002d0a74 - THE TWO SUBI'S ARE A SORT BIAS AND
  * NOT A MOVE. Space_BuildObjects takes 0x186a0 off slot 0's c2 and
  * 0xc350 off slot 1's AFTER Obj_SetCentroid has written the vertex
  * mean, and nothing but the sort key reads c2. So these two - the
  * hull's biggest pieces, whose real centroids sit within a few
  * hundred units of each other - are forced to the far end of the
  * order and painted first. Dropping the bias puts them wherever
  * their true depth lands, which at the window scene's range is on
  * top of the rest of the ship. */
 { 0, 1, 1, {{0x0001L,  94,      0,     0, 128, 128,     0}, {0,0,0,0,0,0,0}},
  XF_NONE, 0, 0, -0x186a0L},
 { 2, 0, 1, {{0x0001L,  94,      0,     0, 128, 128, -700L}, {0,0,0,0,0,0,0}},
  XF_NONE, 0, 0, -0xc350L},
 { 1, 3, 1, {{0x8002L, 126, -29062L, 8192L, 128, 128, -700L}, {0,0,0,0,0,0,0}}, XF_NONE, 0, 0},
 { 3, 2, 2, {{0x0001L, 145,      0,     0, 192,  64,     0},
             {0x0002L, 128,  8960L, 13568L, 192,  64,    0}}, XF_NONE, 0, 0},
 { 4, 2, 2, {{0x0001L, 150,      0,     0, 192,  64,     0},
             {0x8002L, 128, 17230L, 29453L, 192,  64,    0}}, XF_NONE, 0, 0},
 { 8, 0, 1, {{0x4000L, 128,      0,     0, 192,  64,     0}, {0,0,0,0,0,0,0}}},
 { 9, 0, 1, {{0x4000L, 128,      0,     0, 192,  64,     0}, {0,0,0,0,0,0,0}}},
 {10, 0, 1, {{0x4000L, 128,      0,     0, 192,  64,     0}, {0,0,0,0,0,0,0}}, XF_NONE, 0, 0},

 /* ORIGIN 0x002d1638 Uv_WriteBillboard11 - object 11, the window
  * billboard, AND SLOT 8 IS WHY IT IS HERE AND NOT AT THE END: the
  * planet scenes' skip table sets exactly this index, so the slot
  * number is load-bearing. Its texture is bound at run time and its
  * UVs are literal, so it takes no Uv_GenerateFaceParams pass. */
 {11, 0, 0, {{0,0,0,0,0,0,0}, {0,0,0,0,0,0,0}}, XF_BILLBOARD, 0, 0},

 /* ORIGIN 0x002cf59c Mesh_DeformAndMap - the six claws, placed at
  * exactly 60 degrees apart: 0, 1365, 2730, 4095, 5460 and 6825 of
  * 8192. Each is its own object, all bent by the same script. THEY
  * ARE SLOTS 9 TO 14 and the order is load-bearing - a skip table
  * splits this group from the spikes. formats/geometry.txt 6.1. */
 {13, 2, 1, {{0x0001L, 150, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_STRUT, 0L, 0},
 {12, 2, 1, {{0x0001L, 150, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_STRUT, 6825L, 0},
 {14, 2, 1, {{0x0001L, 150, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_STRUT, 1365L, 0},
 {15, 2, 1, {{0x0001L, 150, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_STRUT, 2730L, 0},
 {16, 2, 1, {{0x0001L, 150, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_STRUT, 4095L, 0},
 {17, 2, 1, {{0x0001L, 150, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_STRUT, 5460L, 0},

 /* ORIGIN 0x002d12ac Spike_BuildRing - object 18 as the master at
  * angle 0 and seven Mesh_InstanceAtAngle copies 0x400 apart, which
  * is 45 degrees, the four diagonals carrying a 0x140 radius the
  * axis-aligned ones do not. The last one is slot 22 and both ship
  * scenes set g_SceneObjectCount to 22, so it only draws because
  * Scene_SortAndDrawAll's dbf runs the count plus one times. */
 {18, 2, 1, {{0x0001L, 100,      0,     0, 192,  64,     0}, {0,0,0,0,0,0,0}}, XF_NONE, 0, 0},
 {18, 2, 1, {{0x0001L, 100, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_SPIKE, 0x0800L, 0x100L},
 {18, 2, 1, {{0x0001L, 100, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_SPIKE, 0x1000L, 0x100L},
 {18, 2, 1, {{0x0001L, 100, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_SPIKE, 0x1800L, 0x100L},
 {18, 2, 1, {{0x0001L, 100, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_SPIKE, 0x0400L, 0x140L},
 {18, 2, 1, {{0x0001L, 100, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_SPIKE, 0x0c00L, 0x140L},
 {18, 2, 1, {{0x0001L, 100, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_SPIKE, 0x1400L, 0x140L},
 {18, 2, 1, {{0x0001L, 100, 0, 0, 192, 64, 0}, {0,0,0,0,0,0,0}},
  XF_SPIKE, 0x1c00L, 0x140L},

 /* ORIGIN 0x002d1010 onward - THE THREE SHELLS AROUND THE ARCS,
  * Fspacedata objects 5, 6 and 7, each BUILT from three of that
  * chunk's slots, and sitting AFTER the spike ring in scene order.
  * They carry no texture and no UV pass: every face is rewritten to
  * mode 1 and Rast_AddTri reads its coordinates from the 24-byte
  * template instead. effects/orb.txt 9, formats/geometry.txt 6.1. */
 { 5, 0, 0, {{0,0,0,0,0,0,0}, {0,0,0,0,0,0,0}}, XF_NONE, 0, 0, 0, 1},
 { 6, 0, 0, {{0,0,0,0,0,0,0}, {0,0,0,0,0,0,0}}, XF_NONE, 0, 0, 0, 1},
 { 7, 0, 0, {{0,0,0,0,0,0,0}, {0,0,0,0,0,0,0}}, XF_NONE, 0, 0, 0, 1},
};
#define SHIP_OBJS ((s32)(sizeof SHIP_PART / sizeof SHIP_PART[0]))

/* The generated UVs, the face order, and - for a transformed part -
 * its own vertices. g_obj is the Relic_Obj the part actually draws:
 * the chunk's for XF_NONE, a copy pointing at g_vert otherwise. */
static Relic_FaceUv *g_uv[SHIP_OBJS];
static s32 *g_order[SHIP_OBJS];
static Relic_Vert *g_vert[SHIP_OBJS];
static Relic_Obj g_obj[SHIP_OBJS];
/* ORIGIN 0x002cfeba Obj_SetCentroid - the vertex MEAN, which is the
 * sort key's input. Note the component order the demo stores it in:
 * comp1's mean goes to +0x20 and comp2's to +0x1c. */
static s32 g_cent[SHIP_OBJS][3];
static Relic_Extra g_extra[SHIP_EXTRAS];
static s32 g_extras;
static s32 g_unsupported;

/* ORIGIN 0x002d4354 - the same rotation the projection uses, kept
 * separate because this one turns VERTICES at setup. */
static void vrot(s32 *a, s32 *b, s32 angle)
{
	s32 c = Gfx_Cos(angle & 0x1fffL);
	s32 sn = Gfx_Sin(angle & 0x1fffL);
	s32 x = *a, y = *b;
	*a = ((x * c) + (y * sn)) >> 14;
	*b = ((y * c) - (x * sn)) >> 14;
}

/* ORIGIN 0x002d146e Mesh_InstanceAtAngle - a rotated copy in the
 * comp0/comp1 plane, comp2 passed through so the ring stays flat.
 *
 * THE RADIUS SCALES EXACTLY ONE VERTEX. The test is on the dbf
 * counter rather than the vertex index, so it fires on the pass where
 * the counter is 1 - the second to last vertex. Scaling every vertex
 * makes the whole spike bigger instead of longer, which on a
 * four-vertex mesh with 0x140 is a visibly different shape. */
static void instance_at_angle(const Relic_Obj *src, Relic_Vert *out,
                              s32 angle, s32 radius)
{
	s32 i, n = src->nverts;
	for (i = 0; i < n; i++) {
		s32 d0 = src->vert[i].comp[0], d1 = src->vert[i].comp[1];
		vrot(&d0, &d1, angle);
		if ((n - 1 - i) == 1) {
			d0 = (d0 * radius) >> 8;
			d1 = (d1 * radius) >> 8;
		}
		out[i].comp[0] = d0;
		out[i].comp[1] = d1;
		out[i].comp[2] = src->vert[i].comp[2];
	}
}

/* ORIGIN 0x002d00bc Mesh_RotateAboutAnchor - one vertex turned about
 * another IN THE comp0/comp2 PLANE, comp1 untouched.
 *
 * THE ANGLE IS MIRRORED, NOT USED DIRECTLY: eori.l #0x1fff turns g
 * into 8191 - g, so a raw 0x700 is about -78.8 degrees and not
 * +31.5. Reading the immediate as the angle bends the claw the wrong
 * way. */
static void rotate_about(Relic_Vert *v, const Relic_Vert *anchor,
                         s32 raw)
{
	s32 i = (raw & 0x1fffL) ^ 0x1fffL;
	s32 c = Gfx_Cos(i), sn = Gfx_Sin(i);
	s32 x = v->comp[0] - anchor->comp[0];
	s32 z = v->comp[2] - anchor->comp[2];
	v->comp[0] = (((x * c) + (z * sn)) >> 14) + anchor->comp[0];
	v->comp[2] = (((z * c) - (x * sn)) >> 14) + anchor->comp[2];
}

/* ORIGIN 0x002d6388 Strut_SetClawPose - THE CLOSED POSE IS THAT
 * ROUTINE'S THREE BIASES, so it animates from exactly this. The
 * three are spelled as defines because Ship_ResetPose has to be able
 * to come back to them after a part has animated away.
 * geometry.txt 6.1. */
#define CLAW_CLOSED_A 0x0700L
#define CLAW_CLOSED_B 0x1c80L
#define CLAW_CLOSED_C 0x1a80L
static s32 g_claw_bend[3] = { CLAW_CLOSED_A, CLAW_CLOSED_B,
                              CLAW_CLOSED_C };

/* ORIGIN 0x002cf59c Mesh_DeformAndMap with 0x002d007e Mesh_BendChain
 * - three passes bend a 16-vertex chain at vertices 0, 4 and 8, each
 * turning EVERYTHING DOWNSTREAM of its anchor, so the bends compound
 * into an articulated arm, and the claw is then placed about the hub.
 *
 * THE SOURCE IS ONE CANONICAL CLAW FOR ALL SIX AND IT IS OBJECT 13.
 * The chunk also holds 12 and 14..17 pre-rotated, and using those is
 * the trap - bend one in the global comp0/comp2 plane and it goes
 * sideways; rotate it as well and it turns twice. geometry.txt 6.1. */
static void deform_strut(const Relic_Obj *src, Relic_Vert *out,
                         s32 nverts, s32 place)
{
	static const s32 ANCHOR[3] = { 0, 4, 8 };
	const s32 *BEND = g_claw_bend;
	s32 i, j, top = nverts < 16 ? nverts : 16;

	for (i = 0; i < nverts; i++) out[i] = src->vert[i];
	for (j = 0; j < 3; j++) {
		Relic_Vert a;
		if (ANCHOR[j] >= top) continue;
		a = out[ANCHOR[j]];
		for (i = ANCHOR[j] + 1; i < top; i++)
			rotate_about(&out[i], &a, BEND[j]);
	}
	for (i = 0; i < nverts; i++)
		vrot(&out[i].comp[0], &out[i].comp[1], place);
}

static s32 g_faces_drawn;
static int g_ready;

/* ORIGIN 0x002d17fc Mesh_SortFacesByC2 - each object's faces are
 * ordered ONCE AT SETUP by comp2 of their first vertex, and nothing
 * re-sorts them per frame. That single ordering plus a backface test
 * is the whole of the demo's hidden-surface handling: there is no
 * depth buffer, which is why a part is only correct over the range of
 * angles it is actually seen from. */
static void sort_faces(const Relic_Obj *o, s32 *order)
{
	s32 i, j;
	for (i = 0; i < o->nfaces; i++) order[i] = i;
	for (i = 0; i < o->nfaces - 1; i++) {
		for (j = 0; j < o->nfaces - 1 - i; j++) {
			s32 a = o->vert[o->face[order[j]].corner[0]].comp[2];
			s32 b = o->vert[o->face[order[j + 1]].corner[0]].comp[2];
			if (b < a) {
				s32 t = order[j]; order[j] = order[j + 1]; order[j + 1] = t;
			}
		}
	}
}

/* ORIGIN 0x002cfeba Obj_SetCentroid - the vertex mean, which the
 * depth sort keys on. */
static void centroid(s32 i)
{
	s32 k, c;
	for (c = 0; c < 3; c++) {
		s32 tot = 0;
		for (k = 0; k < g_obj[i].nverts; k++)
			tot += g_obj[i].vert[k].comp[c];
		g_cent[i][c] = tot / g_obj[i].nverts;
	}
	g_cent[i][2] += SHIP_PART[i].bias;
}

/* OURS */
int Ship_Init(void)
{
	s32 i;

	if (g_ready) return 0;
	if (Geom_Init()) return 1;
	Tex_Init();
	/* THE UVs ARE GENERATED ONCE, not per frame - Space_BuildObjects
	 * calls Uv_GenerateFaceParams at setup and the projections are
	 * object-space, so nothing about them depends on the camera. */
	g_unsupported = 0;
	for (i = 0; i < SHIP_OBJS; i++) {
		const Relic_Obj *rest = Geom_Get(SHIP_PART[i].obj);
		const Relic_Obj *uvsrc;
		if (!rest) continue;
		g_obj[i] = *rest;
		/* XF_BILLBOARD BELONGS WITH XF_NONE - reaching the else hands
		 * object 11 the canonical claw and the decal lands 110 rows
		 * off. parts.txt 2.8. */
		if (SHIP_PART[i].xform != XF_NONE
		    && SHIP_PART[i].xform != XF_BILLBOARD) {
			g_vert[i] = (Relic_Vert *)malloc((u32)rest->nverts
			                                 * sizeof **g_vert);
			if (!g_vert[i]) continue;
			if (SHIP_PART[i].xform == XF_SPIKE)
				instance_at_angle(rest, g_vert[i], SHIP_PART[i].xa,
				                  SHIP_PART[i].xr);
			else {
				/* object 13 is the canonical claw - see deform_strut */
				const Relic_Obj *canon = Geom_Get(13);
				if (!canon || canon->nverts < rest->nverts) continue;
				deform_strut(canon, g_vert[i], rest->nverts,
				             SHIP_PART[i].xa);
			}
			g_obj[i].vert = g_vert[i];
		}
		g_uv[i] = (Relic_FaceUv *)calloc((u32)rest->nfaces, sizeof **g_uv);
		g_order[i] = (s32 *)malloc((u32)rest->nfaces * sizeof **g_order);
		if (!g_uv[i] || !g_order[i]) continue;
		/* WHICH POSE THE UVs COME OFF DIFFERS BY TRANSFORM - a spike's
		 * from its rotated vertices, a strut's from the rest pose. */
		if (SHIP_PART[i].xform == XF_BILLBOARD) {
			s32 fi, k;
			/* BY SORTED POSITION, NOT BY FACE - Mesh_SortFacesByC2
			 * moves the records and runs first, so the chunk's face
			 * order gives each triangle the other one's coordinates
			 * and the quad tears along its diagonal.
			 * docs/formats/geometry.txt 3.3. */
			sort_faces(&g_obj[i], g_order[i]);
			for (fi = 0; fi < rest->nfaces && fi < 2; fi++) {
				s32 fc = g_order[i][fi];
				for (k = 0; k < 3; k++) {
					g_uv[i][fc].u[k] = BILLBOARD_UV[fi][k * 2];
					g_uv[i][fc].v[k] = BILLBOARD_UV[fi][k * 2 + 1];
				}
				g_uv[i][fc].written = 1;
			}
			centroid(i);
			continue;
		}
		uvsrc = (SHIP_PART[i].xform == XF_STRUT) ? rest : &g_obj[i];
		g_unsupported += Uv_Generate(uvsrc, SHIP_PART[i].pass,
		                             SHIP_PART[i].npass, g_uv[i]);
		sort_faces(&g_obj[i], g_order[i]);
		centroid(i);
	}
	g_flat_pattern = Geom_FlatPattern();
	g_flat_palette = Geom_FlatPalette();
	g_ready = 1;
	Ship_RecentroidStruts();
	return 0;
}

/* OURS */
int Ship_Ready(void) { return g_ready && Geom_Ready(); }

/* OURS - ship.h */
void Ship_LookCam(s32 *out, const s32 *in, s32 yaw, s32 pitch)
{
	int i;

	for (i = 0; i < SHIP_CAM; i++) out[i] = in[i];
	out[3] += yaw;
	out[4] += pitch;
}

/* OURS - ship.h */
void Ship_Bounds(s32 *centre, s32 *radius)
{
	s32 lo[3], hi[3];
	s32 i, k, c, got = 0;

	for (c = 0; c < 3; c++) {
		lo[c] = hi[c] = 0;
		if (centre) centre[c] = 0;
	}
	if (radius) *radius = 0;
	if (!Ship_Ready()) return;
	for (i = 0; i < SHIP_OBJS; i++) {
		if (!g_obj[i].vert) continue;
		if (g_skip && Image_U32(g_skip + (u32)i * 4)) continue;
		for (k = 0; k < g_obj[i].nverts; k++) {
			for (c = 0; c < 3; c++) {
				s32 v = g_obj[i].vert[k].comp[c] >> 4;
				if (!got || v < lo[c]) lo[c] = v;
				if (!got || v > hi[c]) hi[c] = v;
			}
			got = 1;
		}
	}
	if (!got) return;
	if (centre)
		for (c = 0; c < 3; c++) centre[c] = (lo[c] + hi[c]) / 2L;
	if (radius) {
		s32 r = 0;

		for (c = 0; c < 3; c++) {
			s32 h = (hi[c] - lo[c]) / 2L;
			if (h > r) r = h;
		}
		*radius = r;
	}
}

/* OURS - ship.h */
void Ship_ResetPose(void)
{
	s32 closed[3];

	closed[0] = CLAW_CLOSED_A;
	closed[1] = CLAW_CLOSED_B;
	closed[2] = CLAW_CLOSED_C;
	Ship_SetClawBend(closed);
	Ship_SetClawTexture(-1L);
	Ship_SetAdditiveLit(0);
	Ship_SetFlatScroll(0L);
	Ship_SetBillboard(0, 0L);
	Ship_SetExtras(0, 0L);
}

/* ORIGIN 0x002cffea - the re-driver. It walks the six claw slots,
 * copies each back from the ONE canonical rest pose, bends at vertices
 * 0, 4 and 8 by the three angles and rotates by a hub angle stepping
 * 0x555 a claw. Nothing re-sorts the faces or re-centroids: the UVs
 * come off the rest pose and do not move either, so only the vertices
 * are rebuilt. */
void Ship_SetClawBend(const s32 *bend)
{
	const Relic_Obj *canon;
	s32 i;

	if (!g_ready || !bend) return;
	for (i = 0; i < 3; i++) g_claw_bend[i] = bend[i];
	canon = Geom_Get(13);
	if (!canon) return;
	for (i = 0; i < SHIP_OBJS; i++) {
		const Relic_Obj *rest;
		if (SHIP_PART[i].xform != XF_STRUT || !g_vert[i]) continue;
		rest = Geom_Get(SHIP_PART[i].obj);
		if (!rest || canon->nverts < rest->nverts) continue;
		deform_strut(canon, g_vert[i], rest->nverts, SHIP_PART[i].xa);
	}
}


/* OURS */
void Ship_SetSkipTable(u32 image_addr)
{
	g_skip = image_addr;
}

/* OURS */
void Ship_SetBillboard(const u8 *tex, s32 rows)
{
	g_bill_tex = tex;
	g_bill_rows = rows;
}

/* ORIGIN 0x002d4354 maybe_Math_Rotate2D - one rotation of a pair, the
 * products taken down by 8 then 6. */
static void rot2d(s32 *a, s32 *b, s32 angle)
{
	s32 c = Gfx_Cos(angle & 0x1fffL);
	s32 s = Gfx_Sin(angle & 0x1fffL);
	s32 x = *a, y = *b;
	*a = ((x * c) + (y * s)) >> 14;
	*b = ((y * c) - (x * s)) >> 14;
}

/* ORIGIN 0x002d42d6 Math_Rotate2Axis is this twice, for the orb's
 * satellites: the same tables and the same >> 14. */
void Ship_Rot2d(s32 *a, s32 *b, s32 angle) { rot2d(a, b, angle); }

/* ORIGIN 0x002cf550 Strut_RetextureToTex3 - A STRAIGHT SWAP. The
 * routine re-runs the claws' UVs with the pass they already carry off
 * the pose they already use, and the only thing that differs is the
 * texture. effects/orb.txt 8. */
void Ship_SetClawTexture(s32 tex)
{
	g_claw_tex = tex;
}

/* OURS - phase 3 reveals the three shells; nothing hides them again. */
void Ship_SetAdditiveLit(int lit)
{
	g_additive_lit = lit;
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x3a2 - anim mode 0x12 takes
 * 0x100 off g_FlatScroll a tick and mode 0x13 leaves it alone, so
 * the shells shimmer in the second shot and are still in the
 * first. */
void Ship_SetFlatScroll(s32 scroll)
{
	g_flat_scroll = scroll;
}

/* ORIGIN 0x002d146e Mesh_InstanceAtAngle, +0x90 - A CLAW IS
 * CENTROIDED ONCE, from the UNBENT arm at its hub angle, and
 * Strut_SetClawPose never does it again. Keying the POSED arm instead
 * collapses all six centroids to the hub axis, their keys land within
 * 219 of each other, and every claw then paints over every beam
 * behind it. effects/orb.txt 7.1. */
void Ship_RecentroidStruts(void)
{
	const Relic_Obj *canon = Geom_Get(13);
	s32 i;

	if (!g_ready || !canon) return;
	for (i = 0; i < SHIP_OBJS; i++) {
		const Relic_Obj *rest;
		s32 k, c, tot[3];

		if (SHIP_PART[i].xform != XF_STRUT || !g_vert[i]) continue;
		rest = Geom_Get(SHIP_PART[i].obj);
		if (!rest || canon->nverts < rest->nverts) continue;
		/* THE UNBENT ARM AT ITS HUB ANGLE, which is what
		 * Mesh_InstanceAtAngle centroided - NOT the posed vertices.
		 * Closed, a claw's mean sits 2230 from the axis; extended it
		 * sits 16912 out, and 16912 is what the demo's slot holds. */
		tot[0] = tot[1] = tot[2] = 0;
		for (k = 0; k < rest->nverts; k++) {
			s32 c0 = canon->vert[k].comp[0];
			s32 c1 = canon->vert[k].comp[1];
			rot2d(&c0, &c1, SHIP_PART[i].xa);
			tot[0] += c0;
			tot[1] += c1;
			tot[2] += canon->vert[k].comp[2];
		}
		for (c = 0; c < 3; c++) g_cent[i][c] = tot[c] / rest->nverts;
		g_cent[i][2] += SHIP_PART[i].bias;
	}
}

/* OURS. The demo's quotient, plus the remainder it throws away, as a
 * 24.8 screen coordinate. At scale 1.0 the fraction cannot move a
 * pixel, so the reference render is unchanged. */
static s32 subpix(s32 num, s32 den, s32 centre)
{
	s32 n = num << 8;
	s32 q = n / den;
	s32 r = n % den;
	s32 frac = ((r < 0 ? -r : r) << 8) / den;

	if (r < 0) frac = -frac;
	/* SIGNED, SO THE VALUE STAYS MONOTONIC - non-negative shears every
	 * vertex left of or above centre. orb_underside.txt 3. */
	return ((q + centre) << 8) + frac;
}

/* ORIGIN 0x002d2cd4 Scene_TransformProject - THE SECOND COMPONENT IS
 * THE DEPTH: it is the divisor, and the other two get the screen
 * centres 0x50 and 0x32. Returns zero for a rejected vertex, which the
 * demo does by writing nothing at all rather than by clamping.
 *
 * RETURNS DEMO COORDINATES. Gfx_LineAA scales by the frame itself, so
 * scaling here as well throws every line off a frame larger than
 * 160x100 and the ship vanishes - at scale 1.0 the double scale is the
 * identity, which is exactly why it looks fine in a reference render. */
static void to_camera(const Relic_Vert *v, s32 angle, const s32 *cam,
                     s32 *o0, s32 *o1, s32 *o2)
{
	s32 c0 = v->comp[0] >> 4;
	s32 c1 = v->comp[1] >> 4;
	s32 c2 = v->comp[2] >> 4;

	rot2d(&c0, &c1, angle);
	/* THE VERTICES CARRY THEIR OWN PLACEMENT, so nothing is
	 * translated here. The scene slot's x, z and y at +0x18, +0x1c and
	 * +0x20 are the vertex MEAN, written by 0x002cfeba, and
	 * Scene_SortAndDrawAll reads them only to compute +0x24, the sort
	 * key - never to move an object. Fspacedata already holds the
	 * hull discs at one height and the struts at another. */
	c0 -= cam[0];
	c1 -= cam[1];
	c2 -= cam[2];
	/* ORIGIN 0x002d41d2 maybe_Camera_Rotate2AxisLatched. THE YAW IS
	 * WHY ANYTHING IS VISIBLE: zero here and every vertex falls
	 * behind the near plane. portal.txt 5.4. */
	rot2d(&c0, &c1, cam[3]);
	rot2d(&c1, &c2, cam[4]);
	c1 += CAM_DIST;
	*o0 = c0;
	*o1 = c1;
	*o2 = c2;
}

/* ORIGIN 0x002d2cd4 Scene_TransformProject - its divide, its screen
 * centres and its near-plane test. Returns zero for a rejected
 * vertex, which the demo does by writing 0x7fffffff. CLIPPING TO THE
 * PLANE HERE LOOKS RIGHT AND SMEARS THE FACE ACROSS THE FRAME -
 * docs/formats/geometry.txt 3.1. */
static int to_screen(s32 c0, s32 c1, s32 c2, s32 *sx, s32 *sy)
{
	if (c1 <= SHIP_NEAR_CLIP) return 0;
	/* THE DEMO'S DIVIDE TRUNCATES TO A WHOLE PIXEL and that is the
	 * jitter: divsl.l throws the remainder away, so a vertex can only
	 * sit on one of 160 columns and flicks between two as the object
	 * turns. The quotient here is the demo's exactly; the remainder it
	 * discards is recovered into eight fraction bits.
	 *
	 * IN TWO PARTS BECAUSE (c0 << 16) OVERFLOWS A s32 - c0 reaches
	 * about 78000 after the camera subtraction. Taking the quotient
	 * and then the remainder is the same trick Space_ProjectFx uses. */
	*sx = subpix(c0, c1, 0x50L);
	*sy = subpix(c2, c1, 0x32L);
	return 1;
}

/* ORIGIN 0x002d2c7a - ship.h */
int Ship_ProjectFlare(const Relic_Vert *v, s32 angle, const s32 *cam,
                      s32 *sx, s32 *sy, s32 *depth)
{
	s32 c0, c1, c2;

	to_camera(v, angle, cam, &c0, &c1, &c2);
	if (depth) *depth = c1;
	if (c1 <= SHIP_FLARE_CLIP) return 0;
	*sx = ((c0 << 8) / c1) + 0x50L;
	*sy = ((c2 << 8) / c1) + 0x32L;
	return 1;
}

/* OURS. The hull's own transform for one vertex, so an effect sharing
 * the ship's scene lands in the ship's frame - the beams' endpoints
 * come through here. */
int Ship_Project(const Relic_Vert *v, s32 angle, const s32 *cam,
                 s32 *sx, s32 *sy, s32 *depth)
{
	s32 c0, c1, c2;

	to_camera(v, angle, cam, &c0, &c1, &c2);
	/* THE DEPTH IS THE DIVISOR, not the distance. */
	if (depth) *depth = c1;
	return to_screen(c0, c1, c2, sx, sy);
}

/* ORIGIN 0x002d18f8 Poly_DrawFaceList - the cross product of the
 * two projected edges from corner 2, in WORDS, and cull mode 0 keeps
 * the negative side. Mode 2 culls nothing, and Space_BuildObjects
 * leaves the draw context's +0x4c zero for every ship part, so mode 0
 * is what the vessel uses. */
static int facing(const s32 *px, const s32 *py)
{
	/* WHOLE PIXELS, NOT OUR FRACTION - divsl has already truncated
	 * before the demo takes this cross product. geometry.txt 3.1. */
	s32 x0 = px[0] >> 8, x1 = px[1] >> 8, x2 = px[2] >> 8;
	s32 y0 = py[0] >> 8, y1 = py[1] >> 8, y2 = py[2] >> 8;
	s32 e0x = x0 - x2, e0y = y0 - y2;
	s32 e1x = x1 - x2, e1y = y1 - y2;

	/* ORIGIN 0x002d1970 - SUBTRACTED LONG, MULTIPLIED WORD: muls.w
	 * takes the low 16 bits. geometry.txt 3.1. */
	return (Gfx_Word(e1x) * Gfx_Word(e0y)
	      - Gfx_Word(e0x) * Gfx_Word(e1y)) < 0;
}

/* OURS. One part's faces, textured, in the setup-time order. */
static void draw_part(Relic_Frame *f, s32 pi, s32 angle, const s32 *cam)
{
	const Relic_Obj *o = &g_obj[pi];
	int bill = (SHIP_PART[pi].xform == XF_BILLBOARD);
	s32 ti = SHIP_PART[pi].tex;
	const u8 *tex;
	s32 rows;
	s32 n, k;

	if (g_claw_tex >= 0 && SHIP_PART[pi].xform == XF_STRUT)
		ti = g_claw_tex;
	tex = bill ? g_bill_tex : Tex_Base(ti);
	rows = bill ? g_bill_rows : Tex_Rows(ti);

	if (SHIP_PART[pi].additive) {
		if (!g_additive_lit || !g_flat_pattern || !g_flat_palette) return;
	} else if (!g_uv[pi]) {
		return;
	}
	if (!o->vert || !g_order[pi]) return;
	for (n = 0; n < o->nfaces; n++) {
		s32 i = g_order[pi][n];
		const Relic_Face *fc = &o->face[i];
		s32 px[3], py[3];
		int ok = 1;

		for (k = 0; k < 3; k++) {
			s32 c0, c1, c2;
			to_camera(&o->vert[fc->corner[k]], angle, cam, &c0, &c1, &c2);
			if (!to_screen(c0, c1, c2, &px[k], &py[k])) {
				ok = 0;
				break;
			}
		}
		/* ORIGIN 0x002d19e6 - the mode-1 branch rejects a face with
		 * any unprojectable corner, which the mode-2 one does not,
		 * and it does NOT cross-product test. */
		if (!ok) continue;
		if (SHIP_PART[pi].additive) {
			s32 fu[3], fv[3];

			if (!flat_uv(i, fu, fv)) continue;
			g_faces_drawn++;
			Rast_AddTri(f, px, py, fu, fv,
			            g_flat_pattern, g_flat_palette, g_flat_scroll);
			continue;
		}
		if (!bill && !facing(px, py)) continue;
		g_faces_drawn++;
		if (tex && g_uv[pi][i].written)
			Rast_TexTri(f, px, py, g_uv[pi][i].u, g_uv[pi][i].v,
			            tex, rows);
		else
			for (k = 0; k < 3; k++) {
				s32 j = (k + 1) % 3;
				/* Gfx_LineAA takes whole demo pixels and scales them
				 * itself, so the fraction comes off here. */
				Gfx_LineAA(f, px[k] >> 8, py[k] >> 8,
				           px[j] >> 8, py[j] >> 8);
			}
	}
}

/* OURS - a part's extra scene objects, cleared by passing n = 0. */
void Ship_SetExtras(const Relic_Extra *ex, s32 n)
{
	s32 i;

	if (n > SHIP_EXTRAS) n = SHIP_EXTRAS;
	if (n < 0) n = 0;
	for (i = 0; i < n; i++) g_extra[i] = ex[i];
	g_extras = n;
}

/* ORIGIN 0x002d28ca Scene_SortAndDrawAll, +0x10 - the key is the
 * centroid taken through the
 * object angle and both camera angles, and NOT translated by the
 * camera: Scene_SortAndDrawAll reads the slot's own x, z and y and
 * keeps only the depth. */
static s32 depth_key(const s32 *cent, s32 angle, const s32 *cam)
{
	s32 c0 = cent[0] >> 4;
	s32 c1 = cent[1] >> 4;
	s32 c2 = cent[2] >> 4;
	s32 t;

	rot2d(&c0, &c1, angle);
	t = c0; c0 = ((t * Gfx_Cos(cam[3] & 0x1fffL))
	            + (c1 * Gfx_Sin(cam[3] & 0x1fffL))) >> 14;
	c1 = ((c1 * Gfx_Cos(cam[3] & 0x1fffL))
	    - (t * Gfx_Sin(cam[3] & 0x1fffL))) >> 14;
	t = c1; c1 = ((t * Gfx_Cos(cam[4] & 0x1fffL))
	            + (c2 * Gfx_Sin(cam[4] & 0x1fffL))) >> 14;
	(void)c0;
	return c1;
}

/* OURS. The vessel, over whatever the scene has already drawn.
 *
 * [open] ORIGIN 0x002d28ca Scene_SortAndDrawAll walks
 * g_SceneObjectCount slots of 0x50 bytes, writes {pointer, depth}
 * pairs, bubble-sorts them by depth past g_SceneSortSkip and
 * dispatches on the type word at +0x2c; Space_BuildObjects' 21-slot
 * setup is what fills those slots. Neither is transcribed, so this
 * draws a fixed list in chunk order - renderer.txt section 5. */
void Ship_Draw(Relic_Frame *f, s32 angle, const s32 *cam)
{
	s32 key[SHIP_OBJS + SHIP_EXTRAS], ord[SHIP_OBJS + SHIP_EXTRAS];
	s32 i, j, n = 0;

	g_faces_drawn = 0;
	if (!Ship_Ready()) return;

	/* ORIGIN 0x002d28ca Scene_SortAndDrawAll - A DEPTH KEY PER OBJECT,
	 * THEN A SORT, and that is the whole of the demo's inter-object
	 * ordering. Obj_SetCentroid puts each object's vertex mean in the
	 * slot, the first loop rotates it through the object and camera
	 * angles and keeps the DEPTH as the key at +0x24, and a bubble
	 * sort then orders the list far-to-near.
	 *
	 * WITHOUT IT A FIXED ORDER SHOWS THROUGH: there is no depth
	 * buffer, so a spike behind the shaft drawn after the shaft paints
	 * straight over it. Mesh_SortFacesByC2's per-object face order
	 * cannot help - it only sorts within one object. */
	for (i = 0; i < SHIP_OBJS; i++) {
		if (!g_obj[i].vert) continue;
		/* ORIGIN 0x002d2970 - the skip test is on the slot the sorted
		 * list points at, so it holds whatever the order came out as. */
		if (g_skip && Image_U32(g_skip + (u32)i * 4)) continue;
		key[n] = depth_key(g_cent[i], angle, cam);
		ord[n] = i;
		n++;
	}
	for (i = 0; i < g_extras; i++) {
		key[n] = depth_key(g_extra[i].cent, angle, cam);
		ord[n] = SHIP_OBJS + i;
		n++;
	}
	/* far first, so the near ones paint last */
	for (i = 0; i < n - 1; i++) {
		for (j = 0; j < n - 1 - i; j++) {
			if (key[j] < key[j + 1]) {
				s32 t = key[j]; key[j] = key[j + 1]; key[j + 1] = t;
				t = ord[j]; ord[j] = ord[j + 1]; ord[j + 1] = t;
			}
		}
	}
	for (i = 0; i < n; i++) {
		if (ord[i] >= SHIP_OBJS) {
			const Relic_Extra *e = &g_extra[ord[i] - SHIP_OBJS];
			if (e->draw) e->draw(f, e->which);
		} else {
			draw_part(f, ord[i], angle, cam);
		}
	}
}

/* OURS */
s32 Ship_FacesDrawn(void) { return g_faces_drawn; }

/* OURS */
void Ship_Stats(char *out, int cap)
{
	char g[96], t[64];
	(void)cap;
	g[0] = t[0] = 0;
	if (Geom_Ready()) Geom_Stats(g, (int)sizeof g);
	if (Tex_Ready()) Tex_Stats(t, (int)sizeof t);
	sprintf(out, "%s | tex %s | uv-unsupported %ld",
	        g[0] ? g : "no Fspacedata", t[0] ? t : "none",
	        (long)g_unsupported);
}
