/* RELIC part 2.14 - the ruins. TempleModule_Run 0x003678d0 mode 0.
 *
 * Green mountains and Aztec ruins under heavy fog. The landscape is a
 * ray march over a 64 x 64 grid of textured quads; the temple and the
 * structures standing on it are polygon models drawn over the top.
 *
 * IT RENDERS INTO THE DEMO'S ORDINARY 160 x 100 FOUR-BYTE FRAME, the
 * same buffer the space scenes use, and its textures arrive in that
 * exact layout - (R, G, 0, B) with a zero light byte. So nothing here
 * converts a pixel; the demo's texels are Relic_Frame pixels.
 *
 * See docs/effects/temple.txt.
 *
 * THREE DIVERGENCES, ALL DELIBERATE.
 *
 * THE EIGHT OCTANT WALKERS ARE ONE FUNCTION HERE. The demo has eight
 * near-identical routines at 0x0036fc98 .. 0x00371a88, differing in
 * three flags - which axis is major, whether the cell index is
 * mirrored, and whether the ray step adds or subtracts. OURS is the
 * merge; the flags are read out of the eight and tabulated in
 * TE_OCTANT. Splitting them back out would copy a thousand lines
 * eight times to no end.
 *
 * THE FOG IS A LERP HERE, NOT A TABLE. The demo folds the depth blend
 * into its chunky-to-planar pass, so the haze and the bitplane
 * interleave come from one lookup. A port has no planes, so the lerp
 * Temple_BuildFogRamp bakes in is applied directly at output.
 *
 * THE OBJECTS ARE NOT DRAWN YET. Temple_DrawObjects walks 41
 * Relic_SceneObject records through the space engine's own pipeline;
 * wiring that up is its own job. [open] - the terrain is the part
 * that is new.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parts.h"
#include "gfx.h"
#include "dat.h"
#include "image.h"
#include "rast.h"
#include "objset.h"
#include "orb.h"
#include "host.h"

/* ---- the demo's geometry ---------------------------------------- */

#define TE_W        160L            /* the frame, and the row stride  */
#define TE_H        100L
#define TE_PIXELS   (TE_W * TE_H)
#define TE_ROWBYTES (TE_W * PX_BYTES)   /* 0x280, the blur's stride   */

#define TE_GRID     64L             /* cells across and down          */
#define TE_CELLS    (TE_GRID * TE_GRID)
#define TE_MAP      128L            /* the source maps are twice that */

/* ORIGIN 0x0036d1a6 Temple_LoadShot1Assets - Faztecterrain splits
 * into three 0x4000 parts, which is 128 x 128 bytes each. */
#define TE_MAPBYTES (TE_MAP * TE_MAP)

/* ORIGIN 0x0036d960 Temple_LoadTextures - forty parts of 0x1400,
 * each a 0x400 palette and a 64 x 64 page of indices. 24 become the
 * ground atlas and 16 the wall atlas. */
#define TE_TILES_G  24L
#define TE_TILES_W  16L
#define TE_TILE     64L
#define TE_PARTSIZE 0x1400L
#define TE_PALBYTES 0x400L
#define TE_ATLAS_G  0x60000L
#define TE_ATLAS_W  0x40000L
/* THE TWO BLOCKS ARE ONE CONTIGUOUS ATLAS. Mem_AllocSubBlocks cuts
 * them from a single allocation, and a cell's ground selector reaches
 * past the first into the second - cell 0 selects tile 29, which
 * lands a whole band into the wall block [observed, on hardware]. 40 tiles is ten bands of 0x10000, which
 * is 0x60000 + 0x40000 exactly, so the split is the allocator's and
 * not the texture's. Allocating them apart would read off the end. */
#define TE_ATLAS    (TE_ATLAS_G + TE_ATLAS_W)

/* ORIGIN 0x0036d544 Temple_BuildRayTable - 450 directions by 64
 * sub-cell steps at four longwords, which is its 460800 bytes. 450
 * tenths of a degree is one octant. */
#define TE_DIRS     450L
#define TE_STEPS    64L
#define TE_RAYLONGS (TE_DIRS * TE_STEPS * 4L)

/* ORIGIN 0x0036d484 Temple_BuildHeadingPlan - 3600 headings of two
 * (routine, columns) halves, and the fan is 320 rays wide. */
#define TE_HEADINGS 3600L
#define TE_BAND     450L
#define TE_FAN      320L

/* ORIGIN 0x0036d75e - 32768 depth levels to a 6-bit light value. */
#define TE_LIGHTS   0x8000L
#define TE_LIGHT_AT 0x1200L
#define TE_LIGHT_SH 7L
#define TE_LIGHT_B  0x78L

/* ORIGIN 0x00367bfa - 400 negative entries then 800 row offsets. */
#define TE_ROWS     1200L
#define TE_ROWBIAS  400L

/* ORIGIN 0x0036d7be - 128 numerators by 256 denominators of
 * (n << 8) / d, the table centred at its middle. */
#define TE_UV_N     128L
#define TE_UV_D     256L

/* ORIGIN 0x0036d89e Temple_BuildCellHeights. */
#define TE_Y_BASE   0x1388L
#define TE_Y_SHIFT  5L

/* ORIGIN 0x0037095a - the antilog's bias and the lerp's shifts. */
#define TE_ANTI_BIAS 0x6c5cL

/* ORIGIN 0x0036d1a6 - Ftemplelog is 0x30000 in the archive and
 * 0x60000 allocated; the padding is the two negative halves. */
#define TE_LOG_BYTES 0x30000UL
#define TE_LOG_NEG   0x20000UL
#define TE_LOG_ALLOC 0x60000UL

#define TE_BADROW    0x40000000L

#define TE_CELLLAST  ((TE_CELLS * 20L + TE_CELLS * 8L) / 20L - 1L)

/* ORIGIN 0x0036d636 Temple_BuildFogRamp - THE BASE IS PER SHOT and
 * a LIGHT-only glow has no colour but this, so one value for the
 * whole scene turns shot 2's white exhaust grey. temple.txt 9.6. */
#define TE_FOG_SHOT1_R 0x2fL
#define TE_FOG_SHOT1_G 0x2fL
#define TE_FOG_SHOT1_B 0x2fL

static s32 g_fog[3] = { TE_FOG_SHOT1_R, TE_FOG_SHOT1_G,
                        TE_FOG_SHOT1_B };

/* ORIGIN 0x0036de04 Temple_RunShot1 - the shot's camera track, two
 * banks of 256 keyframes of three longwords. Track A is (x, z, y)
 * and track B (horizon, heading, unused); 0xc00 is 256 keyframes
 * apart. Temple_SetCameraTrack starts the clock at 0x500 - keyframe
 * five.
 *
 * It advances 2 a frame, MEASURED - temple.txt 5. */
#define TE_TRACK_A   0x003e1350UL
#define TE_TRACK_B   (TE_TRACK_A + 0xc00UL)
/* The SECOND track, which Temple_RunShot1 loads once the descent is
 * over, restarting the clock at 0x1c8 rather than carrying it on. */
#define TE_TRACK_C   0x003e2b5cUL
#define TE_TIME_C    0x1c8L
#define TE_KEYFRAME  12L
#define TE_TIME0     0x500L
#define TE_TIME_STEP 2L

/* THE CAMERA'S LENGTH, WHICH IS NOT THE SHOT'S: the demo's clock
 * steps only while g_TempleCamOn is set and that waits on the load,
 * so it covers 2357 of the shot's fields and nothing parks it when
 * they run out. TE_SETUP is the rest, which the demo spends loading
 * and ends on a black buffer [observed]. temple.txt 5. */
#define TE_SETUP     (TE_SHOT1_SPAN - TE_SHOT1_TICKS)
#define TE_SHOT1_TICKS ((5994L - TE_TIME0) / TE_TIME_STEP - 1L)
#define TE_KEYS      256L

/* ORIGIN 0x003678d0 and the image addresses the plates quote. */
#define A_RAY_SIN   0x00232b6cUL
#define A_RAY_COS   0x0023397cUL
#define A_DETAIL    0x003c5500UL    /* subtracted from the heights   */
#define TE_DISS_THRESH 0x003ce900UL /* when each texel appears       */
#define TE_DISS_PAL    0x003cd500UL /* and the palette it comes from */
#define TE_DISS_IDX    0x003cd900UL
#define TE_DISS_DST    0x50300L     /* a fixed tile of the ground    */
#define TE_DISS_AT     0xd48L       /* the clock it starts at        */
#define A_RETEX     0x003c9500UL    /* copied over the tile map      */

/* ORIGIN 0x0036a29a Temple_BuildObjects - fourteen objects with
 * geometry baked into CODE_06: the faces, then a u16 nfaces-1, then
 * a u32 nverts-1 and the vertices. Every block's size matches its
 * face count times 14 exactly [verified]. */
#define TE_OBJS 14

/* The set in force. objset.h. */
static const Relic_ObjSet *g_set;
static const u32 TE_OBJ[TE_OBJS][3] = {
	{ 0x003d5e00UL, 0x003d60aeUL, 0x003d60b0UL },
	{ 0x003d62dcUL, 0x003d658aUL, 0x003d658cUL },
	{ 0x003d67b8UL, 0x003d6a66UL, 0x003d6a68UL },
	{ 0x003d6c94UL, 0x003d6f42UL, 0x003d6f44UL },
	{ 0x003d7170UL, 0x003d7250UL, 0x003d7252UL },
	{ 0x003d72feUL, 0x003d73deUL, 0x003d73e0UL },
	{ 0x003d748cUL, 0x003d74c4UL, 0x003d74c6UL },
	{ 0x003d7512UL, 0x003d7574UL, 0x003d7576UL },
	{ 0x003d75e6UL, 0x003d762cUL, 0x003d762eUL },
	{ 0x003d7686UL, 0x003d779eUL, 0x003d77a0UL },
	{ 0x003d7834UL, 0x003d78dcUL, 0x003d78deUL },
	{ 0x003d7942UL, 0x003d7a92UL, 0x003d7a94UL },
	{ 0x003d7b7cUL, 0x003d7cccUL, 0x003d7cceUL },
	{ 0x003d7db6UL, 0x003d7eb2UL, 0x003d7eb4UL }
};

/* ORIGIN 0x00369e7a - the object transform, and it is conventional
 * where the terrain's is not: a vertex is scaled down 16, rotated,
 * offset by the camera, then divided. */
#define TE_OBJ_SHIFT 4L         /* object space is 16x the world's  */
#define TE_OBJ_NEAR  0x1f4L     /* added to depth before the divide */
#define TE_OBJ_CLIP  0xc8L      /* at or under this, the vertex is  */
                                /* stored as 0x7fffffff and killed  */
#define TE_OBJ_CX    0x50L
#define TE_OBJ_CY    0x32L
#define TE_OBJ_TEX   0x0023797aUL   /* the shared object texture */

/* ORIGIN 0x0036a29a Temple_BuildObjects - the registers it stages
 * before each Temple_GenFaceParams, in order. D0 IS A BIT MASK and
 * an object can be run through several passes; a pass that rejects
 * a face leaves whatever the one before it wrote, so the tests are
 * the layering. temple.txt 8.
 *
 * THE PASSES SEE UNHALVED VERTICES. Temple_HalveVerts runs AFTER
 * them, so every centre here is in twice the scale g_objVert holds.
 *
 * AND THE TEXTURE BANKS ARE NOT ALL 256 ROWS - two of them hold
 * 128, and reading a 256-row block from either runs off into data
 * whose bytes are far outside a 6-bit channel. That is what the
 * rows bound is for - temple.txt 8.
 *
 * Modes 4 and 0x40 are not implemented - Uv_ProjCylC1 and
 * Uv_ProjRotatedRanged - so their passes claim nothing, which is
 * what a pass that rejects every face does anyway. */
#define TE_UV_SPREAD 0x3e8L

static const Te_UvPass TE_UV[] = {
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
	{  9, TE_UV_ROTRNG, 0x0024f97aUL, 128, 0, 0, 0x3b, 0x40,
	  0x46, -0x1d4c0, 0 },
	{  9, TE_UV_ROTRNG, 0x0024f97aUL, 128, 0, 0, 0xea, 0x0d,
	  0x6e, -0x3a980, -0x1e078 },
	{ 10, TE_UV_FLAT, 0x0023a1caUL, 256, 0x4e, 0x4e,  0x040,  0x040, 0, 0, 0 },
	{ 11, TE_UV_C1C2, 0x0024fb7aUL, 128, 0x5a, 0xaa, -0x0be, -0x0b9, 0, 0, 0 },
	{ 11, TE_UV_CYLC1, 0x0023797aUL, 256, 0, 0, 0, 0,
	  0xf0,  0x400,  0x17200 },
	{ 12, TE_UV_C1C2, 0x0024fb7aUL, 128, 0x5a, 0xaa, -0x0be, -0x0b9, 0, 0, 0 },
	{ 12, TE_UV_CYLC1, 0x0023797aUL, 256, 0, 0, 0, 0,
	  0xf0, -0x17200, 0x400 },
	{ 13, TE_UV_C1C0, 0x003d7fa8UL, 0, 0x0d, 0x0d, -0x2f8, -0x1ae, 0, 0, 0 }
};
#define TE_UVPASS_N ((s32)(sizeof TE_UV / sizeof TE_UV[0]))

/* OURS - a stored component back in the scale the passes ran in. */
#define TE_UV_RAW(v, n) ((v)[n] << 1)

/* ORIGIN 0x0036b300 Temple_AtanLookup - the polar table's ANGLE byte times eight,
 * masked back to a byte and FOLDED under 0x80, so the coordinate
 * ramps up and back down the texture instead of wrapping round it. */
static s32 uv_atan(s32 row, s32 col)
{
	s32 v = ((s32)Gfx_PolarAngle(col, row) * 8L) & 0xffL;

	if (v >= 0x80L) v = 0xffL - v;
	return v;
}

/* OURS */
static s32 uv_spread(const s32 *a, const s32 *b, const s32 *c, s32 n)
{
	s32 d1 = TE_UV_RAW(b, n) - TE_UV_RAW(a, n);
	s32 d2 = TE_UV_RAW(c, n) - TE_UV_RAW(a, n);

	if (d1 < 0L) d1 = -d1;
	if (d2 < 0L) d2 = -d2;
	return d1 + d2;
}

/* ORIGIN the planar branches 0x0036ae7c, 0x0036b064 and 0x0036af70
 * - the same arithmetic on a different component pair, and only
 * 0x10 goes without a spread test. */
static int uv_claims(const Te_UvPass *p,
                     const s32 *a, const s32 *b, const s32 *c)
{
	if (p->mode == TE_UV_C1C2)
		return uv_spread(a, b, c, 1L) >= TE_UV_SPREAD;
	if (p->mode == TE_UV_C0C2)
		return uv_spread(a, b, c, 0L) >= TE_UV_SPREAD;
	if (p->mode == TE_UV_CYLC1 || p->mode == TE_UV_ROTRNG) {
		s32 n = (p->mode == TE_UV_CYLC1) ? 0L : 2L;
		s32 mid = (TE_UV_RAW(a, n) + TE_UV_RAW(b, n)
		           + TE_UV_RAW(c, n)) / 3L;

		return mid >= p->r0 && mid <= p->r1;
	}
	return p->mode == TE_UV_C1C0 || p->mode == TE_UV_FLAT;
}

/* OURS */
static void uv_corner(const Te_UvPass *p, const s32 *v, s32 *u, s32 *w)
{
	s32 ca, cb;

	if (p->mode == TE_UV_FLAT) { *u = 0L; *w = 0L; return; }
	if (p->mode == TE_UV_ROTRNG) {
		/* ORIGIN 0x0036b1ea - the rotation is by zero, temple.txt 8 */
		*u = ((TE_UV_RAW(v, 1) * p->d2) >> 16) + p->cu;
		*w = ((TE_UV_RAW(v, 0) * p->d2) >> 16) + p->cv;
		return;
	}
	if (p->mode == TE_UV_CYLC1) {
		s32 span = p->r1 - p->r0;

		*u = ((p->r1 - TE_UV_RAW(v, 1)) * p->d2) / (span ? span : 1L);
		*w = uv_atan(TE_UV_RAW(v, 0) >> 8, TE_UV_RAW(v, 2) >> 8) >> 1;
		return;
	}
	ca = (p->mode == TE_UV_C0C2) ? TE_UV_RAW(v, 0) : TE_UV_RAW(v, 1);
	cb = (p->mode == TE_UV_C1C0) ? TE_UV_RAW(v, 0) : TE_UV_RAW(v, 2);
	*u = (((ca >> 8) - p->cu) * p->su) >> 8;
	*w = (((cb >> 8) - p->cv) * p->sv) >> 8;
}

/* ORIGIN 0x0036a212 Temple_SetObjectMask - 41 longwords into each
 * record's skip at +0x40, non-zero skipping, and EACH SHOT PASSES
 * ITS OWN TABLE. Shot 2's drops seven of the thirteen meshes; THE
 * RISE'S IS ALL ZERO and draws every one. temple.txt 8 and 11.3. */
#define TE_OBJMASK_S2   0x0036a192UL
#define TE_OBJMASK_RISE 0x0036a1d2UL

/* ---- the cell, which is Relic_TempleCell ------------------------ */

/* ORIGIN the type at 0x0036d89e. Four corner heights, one ground
 * texture for the quad's top and four wall selectors - the corners in
 * (r,c) (r,c+1) (r+1,c) (r+1,c+1) order throughout. */
typedef struct {
	s16 y[4];
	u32 tex;
	s16 c[4];
} Temple_Cell;

/* OURS - the grid-and-save block is finite where the demo's heap
 * is not, so an index it cannot answer is held at the end. */
static s32 te_cell_clamp(s32 cell)
{
	if (cell < 0L) return 0L;
	if (cell > TE_CELLLAST) return TE_CELLLAST;
	return cell;
}

/* OURS - the eight octant walkers, read out of the demo's eight and
 * tabulated. `zmajor` is which axis accumulates, `mirror` is the
 * eori #0x3f on the cell index, `sub` is whether the ray step
 * subtracts, and `bias` reflects the sub-position inside the band.
 * docs/effects/temple.txt 7. */
typedef struct { int zmajor, mirror, sub, bias; } Temple_Octant;

static const Temple_Octant TE_OCTANT[8] = {
	{ 1, 0, 0, 0 },   /* 0x0036fc98 */
	{ 0, 0, 0, 1 },   /* 0x003700d8 */
	{ 0, 0, 1, 0 },   /* 0x0037051c */
	{ 1, 1, 0, 1 },   /* 0x0037095a */
	{ 1, 1, 1, 0 },   /* 0x00370da6 */
	{ 0, 1, 1, 1 },   /* 0x003711ee */
	{ 0, 1, 0, 0 },   /* 0x00371640 */
	{ 1, 0, 1, 1 }    /* 0x00371a88 */
};

/* OURS - one half of a heading's plan. */
typedef struct { s16 octant, columns, ray; } Temple_Plan;

/* ---- state ------------------------------------------------------ */

static Temple_Cell *g_cells;
static s16  *g_cellSave;            /* four corners a cell, pristine  */
static u8   *g_texG, *g_texW;       /* the two atlases                */
static u8   *g_mapTex, *g_mapShade, *g_mapHeight;   /* 128 x 128 each */
static s32  *g_ray;
static Temple_Plan *g_plan;         /* two halves a heading           */
static u16  *g_light;
static s16  *g_rowOff;
static s16  *g_uv;                  /* centred: g_uv[n * 256 + d]     */
static s32  *g_vstep;               /* 0x4000 / span height           */
static const u8 *g_log, *g_antilog;
static u8   *g_frame;               /* 160 x 100 x 4, the blur source */
static u8   *g_prev;                /* the blur's other half          */

static s32 g_camX, g_camY, g_camZ, g_heading, g_horizon, g_camTime;
static u32 g_track = TE_TRACK_A;    /* 0x00372bc6 */
static s32 g_head[TE_KEYS];         /* the headings, unwrapped */
static s32 *g_objVert;              /* every object's verts, halved */
static s32  g_objAt[TE_OBJS], g_objNV[TE_OBJS], g_objNF[TE_OBJS];
static s32  g_objCent[TE_OBJS][3];
static s32  g_objFacesDrawn;
static const u8 *g_objTex;
static s32 g_tick;
static int g_ready;

/* ORIGIN 0x00367ae2 - CATMULL-ROM, in the standard basis:
 *
 *     a = -P0 + 3P1 - 3P2 + P3      c = -P0 + P2
 *     b = 2P0 - 5P1 + 4P2 - P3      d = 2P1
 *     P(t) = (((a t + b) t + c) t + d) / 2,  t being frac / 256
 *
 * KEYFRAME -1 IS KEYFRAME 0. Temple_SetCameraTrack copies the first
 * one back a slot so the curve has a P0 at the start of the
 * track - ORIGIN 0x0036d2b8 Temple_SetCameraTrack. */
static s32 spline(u32 base, s32 comp, s32 key, s32 t)
{
	u32 at = base + (u32)(key * TE_KEYFRAME + comp * 4L);
	s32 p0 = (s32)Image_U32(key > 0 ? at - (u32)TE_KEYFRAME : at);
	s32 p1 = (s32)Image_U32(at);
	s32 p2 = (s32)Image_U32(at + (u32)TE_KEYFRAME);
	s32 p3 = (s32)Image_U32(at + (u32)(TE_KEYFRAME * 2L));
	s32 v = -p0 + 3L * p1 - 3L * p2 + p3;

	v = ((v * t) >> 8) + (2L * p0 - 5L * p1 + 4L * p2 - p3);
	v = ((v * t) >> 8) + (-p0 + p2);
	v = ((v * t) >> 8) + 2L * p1;
	return v >> 1;
}

/* ORIGIN 0x0036e97a, which Temple_SetCameraTrack runs over a track
 * as it selects it. Each heading keyframe is brought within half a
 * turn of the one before by adding or taking 0xe10, so a track that
 * crosses zero interpolates the SHORT way. Without it the spline
 * between a key of 3807 and one of 251 sweeps the whole circle the
 * wrong way and the camera spins - temple.txt 5. */
static void unwrap_headings(void)
{
	s32 k;

	for (k = 0; k < TE_KEYS; k++)
		g_head[k] = (s32)Image_U32(g_track + 0xc00UL
		                           + (u32)(k * TE_KEYFRAME + 4L));
	for (k = 1; k < TE_KEYS; k++) {
		while (g_head[k] - g_head[k - 1] >= 0x708L)
			g_head[k] -= TE_HEADINGS;
		while (g_head[k] - g_head[k - 1] <= -0x708L)
			g_head[k] += TE_HEADINGS;
	}
}

/* OURS - spline() reads the image; the headings have been unwrapped
 * into an array, so they need the same curve over that instead. */
static s32 spline_head(s32 key, s32 t)
{
	s32 p0 = g_head[key > 0 ? key - 1 : key];
	s32 p1 = g_head[key];
	s32 p2 = g_head[key + 1];
	s32 p3 = g_head[key + 2];
	s32 v = -p0 + 3L * p1 - 3L * p2 + p3;

	v = ((v * t) >> 8) + (2L * p0 - 5L * p1 + 4L * p2 - p3);
	v = ((v * t) >> 8) + (-p0 + p2);
	v = ((v * t) >> 8) + 2L * p1;
	return v >> 1;
}

/* OURS - HOW FAR A FULL DRAG LOOKS: the yaw in the heading's tenths
 * of a degree, the pitch in frame rows. reconstruction.txt 7.4. */
#define TE_LOOK_YAW   675L
#define TE_LOOK_PITCH 75L

/* OURS - THE LOOK, ON THE SCENE'S OWN TWO ANGLES. Springy, and
 * signed AGAINST Space_Look so the ground follows the hand -
 * reconstruction.txt 7.4. The shake moves both every field, so
 * neither has to be taught to take a value. */
static void te_look(void)
{
	s32 x, y;

	Demo_Look(&x, &y);
	g_heading -= (s32)((((s64)x * TE_LOOK_YAW) >> 16) % TE_HEADINGS);
	g_horizon += (y * TE_LOOK_PITCH) >> 16;
	while (g_heading >= TE_HEADINGS) g_heading -= TE_HEADINGS;
	while (g_heading < 0L) g_heading += TE_HEADINGS;
}

/* ORIGIN 0x00367a4e Temple_EvalCameraSpline - one clock drives both
 * tracks, the second's heading wrapped to a turn. */
static void eval_camera(void)
{
	s32 key = g_camTime >> 8, t = g_camTime & 0xffL;

	if (key < 0) key = 0;
	if (key > TE_KEYS - 3L) key = TE_KEYS - 3L;
	g_camX    = spline(g_track, 0L, key, t);
	g_camZ    = spline(g_track, 1L, key, t);
	g_camY    = (s32)(s16)spline(g_track, 2L, key, t);
	g_horizon = (s32)(s16)spline(g_track + 0xc00UL, 0L, key, t);
	g_heading = spline_head(key, t);
	while ((s32)(s16)g_heading >= TE_HEADINGS) g_heading -= TE_HEADINGS;
	te_look();
}

/* ORIGIN 0x0036d87e Temple_TileOffset - a tile index to a byte
 * offset. The atlas is four tiles across, a row 0x100 bytes and a
 * band of four 0x4000; a walker scales the result by four. */
static s32 tile_offset(s32 t)
{
	if (t >= 0x40L) t = 0L;
	return (t / 4L) * 0x4000L + (t % 4L) * 0x40L;
}

/* ORIGIN 0x0036d9ce Temple_ConvertGroundTile and 0x0036da2c, which
 * are one routine with two destinations. A part is a 0x400 palette
 * then 64 x 64 indices; the mask clears each byte's low two bits so
 * the shift cannot carry between channels, taking 8-bit to 6-bit, and
 * the rotate leaves (R, G, 0, B) - relic.h's layout, zero light. */
static void convert_tile(const u8 *part, s32 n, u8 *dest)
{
	const u8 *idx = part + TE_PALBYTES;
	u8 *out = dest + (n / 4L) * 0x10000L + (n % 4L) * 0x100L;
	s32 r, c;

	for (r = 0; r < TE_TILE; r++) {
		u8 *o = out + (size_t)(r * 0x400L);

		for (c = 0; c < TE_TILE; c++) {
			/* LANDMINE: RD_U32 EVALUATES ITS ARGUMENT FOUR TIMES, so
			 * an *idx++ inside it advances the index four places a
			 * texel and walks off the chunk. The step is its own
			 * statement for that reason. */
			const u8 *e = part + (size_t)(*idx) * 4L;
			u32 p = RD_U32(e);

			idx++;
			p = (p & 0xfcfcfcfcUL) >> 2;
			/* lsl.l #8 then ror.w #8 - (a,r,g,b) becomes (r,g,0,b) */
			o[0] = (u8)((p >> 16) & 0xff);
			o[1] = (u8)((p >> 8) & 0xff);
			o[2] = 0;
			o[3] = (u8)(p & 0xff);
			o += PX_BYTES;
		}
	}
}

/* ORIGIN 0x0036d89e Temple_BuildCellHeights - one cell is one 2 x 2
 * quad of the map, which is where the 128 against 64 comes from. A
 * larger source byte is LOWER: the transform negates before scaling. */
static void build_cell_heights(void)
{
	s32 r, c;

	for (r = 0; r < TE_GRID; r++) {
		for (c = 0; c < TE_GRID; c++) {
			const u8 *m = g_mapHeight + (size_t)(r * 2L * TE_MAP + c * 2L);
			Temple_Cell *cell = g_cells + (r * TE_GRID + c);

			cell->y[0] = (s16)(TE_Y_BASE - ((s32)m[0] << TE_Y_SHIFT));
			cell->y[1] = (s16)(TE_Y_BASE - ((s32)m[1] << TE_Y_SHIFT));
			cell->y[2] = (s16)(TE_Y_BASE - ((s32)m[TE_MAP] << TE_Y_SHIFT));
			cell->y[3] = (s16)(TE_Y_BASE
			                   - ((s32)m[TE_MAP + 1] << TE_Y_SHIFT));
		}
	}
}

/* ORIGIN 0x0036d802 Temple_BuildCellTextures - the tile map gives one
 * ground texture for the whole quad, the shade map one wall texture
 * per corner. */
static void build_cell_textures(void)
{
	s32 r, c;

	for (r = 0; r < TE_GRID; r++) {
		for (c = 0; c < TE_GRID; c++) {
			size_t at = (size_t)(r * 2L * TE_MAP + c * 2L);
			const u8 *t = g_mapTex + at;
			const u8 *v = g_mapShade + at;
			Temple_Cell *cell = g_cells + (r * TE_GRID + c);

			cell->tex  = (u32)tile_offset((s32)t[0]);
			cell->c[0] = (s16)tile_offset((s32)v[0]);
			cell->c[1] = (s16)tile_offset((s32)v[1]);
			cell->c[2] = (s16)tile_offset((s32)v[TE_MAP]);
			cell->c[3] = (s16)tile_offset((s32)v[TE_MAP + 1]);
		}
	}
}

/* ORIGIN 0x0036d918 Temple_SaveCellHeights - the pristine corners the
 * detail pass needs, because that pass SUBTRACTS and would sink the
 * terrain if it ever ran against its own output. */
static void save_cell_heights(void)
{
	s32 i, k;

	for (i = 0; i < TE_CELLS; i++)
		for (k = 0; k < 4; k++)
			g_cellSave[i * 4L + k] = g_cells[i].y[k];
}

/* OURS - the demo runs the scene once and never puts the heights
 * back; the reconstruction can be skipped into repeatedly, so mode
 * 6 starts from the pristine copy save_cell_heights took. */
static void restore_cell_heights(void)
{
	s32 i, k;

	for (i = 0; i < TE_CELLS; i++)
		for (k = 0; k < 4; k++)
			g_cells[i].y[k] = g_cellSave[i * 4L + k];
}

/* ORIGIN 0x003728a0 Temple_ApplyDetailHeights - a second 128 x 128
 * map, baked into the executable, taken off every corner. MODE 6
 * RUNS THIS EVERY FRAME, so the subtraction accumulates and the
 * ground lifts a little further each time - this is the temple
 * rising, not a one-off adjustment. temple.txt 7. */
static void apply_detail_heights(void)
{
	const u8 *d = Image_At(A_DETAIL);
	s32 r, c;

	if (!d) return;
	for (r = 0; r < TE_GRID; r++) {
		for (c = 0; c < TE_GRID; c++) {
			const u8 *m = d + (size_t)(r * 2L * TE_MAP + c * 2L);
			Temple_Cell *cell = g_cells + (r * TE_GRID + c);

			cell->y[0] = (s16)(cell->y[0] - (m[0] >> 3));
			cell->y[1] = (s16)(cell->y[1] - (m[1] >> 3));
			cell->y[2] = (s16)(cell->y[2] - (m[TE_MAP] >> 3));
			cell->y[3] = (s16)(cell->y[3] - (m[TE_MAP + 1] >> 3));
		}
	}
}

/* ORIGIN 0x0036d544 Temple_BuildRayTable and 0x0036d5b4 - between
 * them a precomputed DDA. An entry is two (offset, distance) pairs,
 * one for crossing a vertical grid line and one for a horizontal, so
 * the march is two table reads and a compare. */
static void build_ray_table(void)
{
	s32 d, s;

	for (d = 0; d < TE_DIRS; d++) {
		s32 sn = (s32)Image_U32(A_RAY_SIN + (u32)(d * 4L));
		s32 cs = (s32)Image_U32(A_RAY_COS + (u32)(d * 4L));

		for (s = 0; s < TE_STEPS; s++) {
			s32 *e = g_ray + (d * TE_STEPS + s) * 4L;
			s32 step = s * 0x100L;
			s32 t, off, a, b;

			t = 0x4000L - step;
			off = (s32)((u32)t * (u32)sn);
			if (cs) off /= cs;
			e[0] = off;
			a = t >> 4; b = off >> 4;
			e[1] = Gfx_Isqrt(a * a + b * b);

			t = step;
			off = (s32)((u32)t * (u32)cs);
			if (sn) off /= sn;
			if (off >= 0x4000L) off = 0x3fffL;
			e[2] = 0x3fffL - off;
			a = off >> 4; b = t >> 4;
			e[3] = Gfx_Isqrt(a * a + b * b);
		}
	}
}

/* ORIGIN 0x0036d484 Temple_BuildHeadingPlan - the fan is 320 ray
 * directions and the screen is 160 columns, hence the >> 1; the minus
 * one makes it a dbf count. Nine bands of 450 are eight octants plus
 * a wrap, so a fan crossing the end still finds a walker. */
static void build_heading_plan(void)
{
	s32 h;

	for (h = 0; h < TE_HEADINGS; h++) {
		Temple_Plan *p = g_plan + h * 2L;
		s32 hi = (h + TE_FAN) & ~1L;
		s32 base = 0, band, n = 0;

		p[0].octant = -1; p[1].octant = -1;
		for (band = 0; band < 9; band++) {
			s32 width = TE_BAND, lo = (h & ~1L) - base, start = 0;

			if (lo > 0) { start = lo; width -= lo; }
			if (width > 0) {
				s32 over = hi - base - TE_BAND;

				if (over < 0) width += over;
				if (width > 0 && n < 2) {
					p[n].octant  = (s16)(band & 7L);
					p[n].columns = (s16)((width >> 1) - 1L);
					p[n].ray     = (s16)start;
					n++;
				}
			}
			base += TE_BAND;
		}
	}
}

/* ORIGIN 0x0036d75e - distance to a 6-bit light value, flat until
 * the fog starts and clamped once it is total. */
static void build_light_table(void)
{
	s32 i;

	for (i = 0; i < TE_LIGHTS; i++) {
		s32 v = i - TE_LIGHT_AT;

		if (v < 0) v = 0;
		v = (v >> TE_LIGHT_SH) - TE_LIGHT_B;
		if (v < 0) v = 0;
		if (v > CHAN_MAX) v = CHAN_MAX;
		g_light[i] = (u16)v;
	}
}

/* ORIGIN 0x00367bfa - 400 negative entries, then the row offsets
 * stepping one frame row at a time and clamping at the last. A
 * NEGATIVE ENTRY MEANS ABOVE THE SCREEN and is what the walkers'
 * skip loop tests. */
static void build_row_offsets(void)
{
	s32 i, off = 0;

	for (i = 0; i < TE_ROWBIAS; i++)
		g_rowOff[i] = (s16)(-(TE_ROWBIAS - i));
	for (i = TE_ROWBIAS; i < TE_ROWS; i++) {
		g_rowOff[i] = (s16)off;
		if (off < TE_PIXELS) off += TE_W;
	}
}

/* ORIGIN 0x0036d798 - 0x4000 / h, the v increment that walks a
 * tile's 64 rows over a span of h pixels. 513 entries; h = 0 keeps
 * 0x4000. */
static void build_vsteps(void)
{
	s32 h;

	for (h = 0; h <= 512L; h++)
		g_vstep[h] = h ? (0x4000L / h) : 0x4000L;
}

/* ORIGIN 0x0036d7be - (n << 8) / d for 128 numerators and 256
 * denominators, so a texture step is one read rather than a divide. */
static void build_uv_steps(void)
{
	s32 n, d;

	for (n = 0; n < TE_UV_N; n++) {
		s32 num = n - (TE_UV_N / 2L);

		if (num < 0) num += 1L;
		num <<= 8;
		for (d = 0; d < TE_UV_D; d++)
			g_uv[n * TE_UV_D + d] = (s16)(d ? num / d : num);
	}
}

/* OURS - the centred lookup the walkers do. */
static s32 uv_step(s32 num, s32 den)
{
	s32 n = num + (TE_UV_N / 2L);

	if (n < 0) n = 0;
	if (n >= TE_UV_N) n = TE_UV_N - 1L;
	if (den < 0) den = 0;
	if (den >= TE_UV_D) den = TE_UV_D - 1L;
	return (s32)g_uv[n * TE_UV_D + den];
}

/* ORIGIN 0x0036d1a6 - RELIC.DAT STORES HALF OF EACH TABLE. The
 * archive asks for 0x20000 bytes before the chunk and 0x10000 after
 * it, and those are the log's NEGATIVE half and the antilog's
 * NEGATIVE half; temple.txt 7. */
static int build_log_tables(const u8 *chunk)
{
	u8 *p = (u8 *)malloc((size_t)TE_LOG_ALLOC);
	s32 i;

	if (!p) return 0;
	memcpy(p + TE_LOG_NEG, chunk, (size_t)TE_LOG_BYTES);
	g_log = p + TE_LOG_NEG;
	g_antilog = p + TE_LOG_NEG + 0x20000UL;
	for (i = 1; i < 0x8000L; i++) {
		const u8 *src = g_log + i * 4L;
		u8 *dst = p + TE_LOG_NEG - i * 4L;

		/* BIT 15, NOT BIT 31 - the flag has to survive into the
		 * antilog's index, which is a word. */
		dst[0] = src[0]; dst[1] = src[1];
		dst[2] = (u8)(src[2] | 0x80); dst[3] = src[3];
	}
	p[0] = 0; p[1] = 0; p[2] = 0x80; p[3] = 0;
	for (i = 0; i < 0x8000L; i++) {
		s32 v = -(s32)RD_S16(g_antilog + i * 2L);
		u8 *dst = (u8 *)g_antilog + (i + 0x8000L) * 2L;

		dst[0] = (u8)((v >> 8) & 0xff); dst[1] = (u8)(v & 0xff);
	}
	return 1;
}

/* ORIGIN 0x0037095a - the perspective divide, done in logarithms.
 * Ftemplelog is two math tables, not geometry: subtracting two logs
 * and taking the antilog is h / dist. */
static s32 project_row(s32 h, s32 dist)
{
	u32 v;
	s32 off;

	/* TERRAIN ABOVE THE CAMERA IS ORDINARY and its height index is
	 * NEGATIVE; the log's negative half sets bit 15 and the
	 * antilog's upper half negates. temple.txt 7. */
	h -= g_camY;
	if (h <= -0x8000L || h >= 0x8000L) return TE_BADROW;
	if (dist < 0 || dist >= 0x8000L) return TE_BADROW;
	/* ORIGIN 0x0037095a - THE SUBTRACTION IS ALLOWED TO WRAP, and
	 * at eye level it must. temple.txt 7. */
	v = (u32)RD_S32(g_log + h * 4L)
	  - (u32)RD_S32(g_log + dist * 4L);
	off = (s32)(v * 2u + (u32)TE_ANTI_BIAS);
	if (off < -(s32)TE_LOG_NEG * 2L || off > 0x1fffeL) return TE_BADROW;
	return (s32)RD_S16(g_antilog + off) + g_horizon;
}

/* ORIGIN 0x0036d636 Temple_BuildFogRamp - the blend the demo bakes
 * into its C2P tables, applied here directly because a port has no
 * bitplanes. w falls 4 a level over 64 levels. */
static void fog_pixel(u8 *px, s32 light)
{
	const s32 *fog = g_fog;
	s32 w = 0xffL - light * 4L;
	int k;

	if (w < 0) w = 0;
	for (k = 0; k < 3; k++) {
		s32 i = (k == 2) ? PX_B : k;
		s32 v = (((s32)px[i] - fog[k]) * w + (fog[k] << 8)) >> 8;

		if (v < 0) v = 0;
		if (v > CHAN_MAX) v = CHAN_MAX;
		px[i] = (u8)v;
	}
	px[PX_LIGHT] = 0;
}

/* OURS - a tile selector times four is a byte offset into the atlas,
 * and Temple_TileOffset's clamp lets an index through that the forty
 * tiles do not cover. The demo reads whatever is past its blocks; in
 * C that is a fault, so an out-of-range selector takes tile 0. */
static const u8 *atlas_at(const u8 *base, s32 sel)
{
	/* A TILE IS NOT CONTIGUOUS - convert_tile lays its rows 0x400
	 * apart - and the wall atlas is the far half of one block, so
	 * the room left is measured from the GROUND base. */
	s32 head = (s32)(base - g_texG);
	s32 off = sel * 4L;

	if (off < 0 || head + off + 63L * 0x400L + 63L * PX_BYTES
	               >= (s32)TE_ATLAS) off = 0L;
	return base + (size_t)off;
}

/* OURS - the fill helper both span kinds share. `du` zero is a WALL,
 * whose u stays put down the face; a TOP walks both axes.
 *
 * A TOP's steps walk two axes and a WALL's only one - temple.txt 9
 * has both and where b comes from. */
static void fill_rows(s32 col, s32 from, s32 to, const u8 *tex,
                      s32 u0, s32 du, s32 v0, s32 dv, s32 light)
{
	s32 uacc = u0 << 8, vacc = v0 << 8, y;

	/* ORIGIN 0x0036fe8a - a span above the screen is SKIPPED row by
	 * row with u and v still stepping, never clipped. */
	for (; from < 0; from++) {
		vacc = (vacc + dv) & 0xffffL;
		uacc = (uacc + du) & 0xffffL;
	}
	for (y = from; y <= to; y++) {
		s32 vt = (vacc >> 8) & 0xffL;
		s32 ut = (uacc >> 8) & 0xffL;
		const u8 *t = tex + (size_t)(vt * 0x400L + ut * PX_BYTES);
		u8 *px = g_frame + (size_t)(y * TE_W + col) * PX_BYTES;

		/* ORIGIN 0x0036fe9c - the WHOLE texel; temple.txt 7. */
		px[PX_R] = t[0]; px[PX_G] = t[1];
		px[PX_B] = t[3]; px[PX_LIGHT] = (u8)(t[2] | (u8)light);
		vacc = (vacc + dv) & 0xffffL;
		uacc = (uacc + du) & 0xffffL;
	}
}

/* OURS - the demo's A5, A6 and D4 high word, which carry a span's
 * texture seeds over from the one before it; temple.txt 9. */
static s32 g_regA5, g_regA6, g_prevTop;

/* OURS - what a crossing leaves behind. The u axis is A5 and the v
 * axis A6 always; which of them carried the fraction follows the
 * CROSSING, and the other parks at the far end of the tile. */
static void te_park(s32 frac, s32 row, s32 back)
{
	s32 park = back ? 0x3fffL : 0L;

	if (row) { g_regA5 = frac; g_regA6 = park; }
	else     { g_regA5 = park; g_regA6 = frac; }
}

/* draw_span's flags: which pass, which crossing, which direction */
#define TE_SP_PASS1 1L
#define TE_SP_ROW   2L
#define TE_SP_BACK  4L
#define TE_SP_LONG  8L

/* ORIGIN 0x0036fc98 - one step can draw two spans and they do not
 * have the same extent; temple.txt 9 has both. */
static s32 draw_span(s32 col, s32 near_y, s32 far_y, s32 d,
                     s32 frac, const Temple_Cell *q, s32 water,
                     s32 flags, s32 wall)
{
	s32 pass = flags & TE_SP_PASS1;
	s32 row  = flags & TE_SP_ROW;
	s32 back = flags & TE_SP_BACK;
	s32 near_row = project_row(near_y, d);
	s32 far_row = project_row(far_y, d);
	s32 light, topmost, h, out = water;

	/* ORIGIN 0x0036fdbc - D4 holds this span's top row in its high
	 * word and prev_top minus this near row in its low byte. */
	h = (g_prevTop - near_row) & 0xffL;
	g_prevTop = (near_y == far_y) ? near_row : far_row;
	if (near_row == TE_BADROW || far_row == TE_BADROW) goto leave;
	if (near_row > water && far_row > water) goto leave;
	light = (d >= 0 && d < TE_LIGHTS) ? (s32)g_light[d] : CHAN_MAX;

	/* ORIGIN 0x0036fdc4 - A WALL IS THE FACE BETWEEN TWO STEPS. The
	 * cell being entered projects higher than the one being left, so
	 * the face runs from the far row down to the near one and fills
	 * its OWN height [0x0036fdec]. */
	if (far_row < near_row) {
		s32 last = near_row - 1L;

		if (last > water) last = water;
		if (far_row <= last) {
			s32 wh = near_row - far_row;

			fill_rows(col, far_row, last,
			          atlas_at(g_texW, wall),
			          (frac >> 8) & 0xffL, 0L, 0L,
			          g_vstep[(wh > 512L) ? 512L : wh], light);
		}
		topmost = far_row;
	} else {
		topmost = near_row;
	}

	/* ORIGIN 0x0036fe46 - THE TOP THEN FILLS TO THE WATERMARK, not
	 * its own height: it is the quad's surface and covers everything
	 * between itself and whatever was drawn nearer. Its texture walks
	 * two axes over the fill - temple.txt 9. */
	if (near_row < water) {
		/* ORIGIN 0x0036fe5a against 0x00370044 - THE TWO CROSSINGS
		 * ENTER THE QUAD THROUGH PERPENDICULAR EDGES, so the cell
		 * crossing seeds u with the tile edge and v with the
		 * fraction and the row crossing does the reverse. The edge
		 * is 0x3fff forward and 0 backward. temple.txt 9. */
		s32 edge = back ? 0L : 0x3fffL;
		s32 su = row ? frac : edge;
		s32 sv = row ? edge : frac;

		/* ORIGIN 0x00370d1a - ONE of the sixteen crossings fetches
		 * a LONGWORD from a table of words, so it lands on the next
		 * denominator; temple.txt 9. */
		s32 den = h + ((flags & TE_SP_LONG) ? 1L : 0L);

		fill_rows(col, near_row, water - 1L,
		          atlas_at(g_texG, (s32)q->tex),
		          (su >> 8) & 0xffL, uv_step((g_regA5 - su) >> 8, den),
		          (sv >> 8) & 0xffL, uv_step((g_regA6 - sv) >> 8, den),
		          light);
	}
	/* ORIGIN 0x0036fe44 - THE WATERMARK IS THE SPAN'S OWN TOP ROW
	 * and a fill stops one short of it, so the two meet exactly. */
	if (topmost < 0) topmost = 0;
	out = (topmost < water) ? topmost : water;
leave:
	/* ORIGIN 0x0036feb6 against 0x00370d68 - the fraction axis
	 * hands its fraction on and the edge axis is parked at the
	 * OPPOSITE end of the tile from its own seed. */
	if (!pass) te_park(frac, row, back);
	return out;
}

/* ORIGIN 0x0036fc98 maybe_Temple_CastColumns0 and its seven siblings
 * - OURS is the merge; see the file head.
 *
 * ALL EIGHT FALL OUT OF THREE FLAGS, and the relations were read off
 * the eight listings rather than guessed [verified]:
 *
 *   zmajor  the CONDITIONAL crossing steps a row, not a cell
 *   mirror  the eori #0x3f on the entry index, AND the eori #0x3fff
 *           that inverts the ray fraction
 *   sub     the ray step subtracts, the march runs backward, the
 *           eori #0x3f on the second lookup's index, AND the corner
 *           pairs flip to their low halves
 *
 * THE CORNER PAIRS ARE THE PART THAT BITES. A row crossing lerps
 * along a row edge and a cell crossing along a column edge, and
 * running backward swaps which half of the quad faces the camera:
 *
 *     row  forward   y2,y3 then y0,y1     backward   y0,y1 then y2,y3
 *     cell forward   y1,y3 then y0,y2     backward   y0,y2 then y1,y3
 *
 * ONE CALL DRAWS ONE COLUMN, near to far, against a WATERMARK; each
 * span fills from its top row down to it and then lifts it, and
 * what the terrain never reaches is sky.
 */
static void cast_column(const Temple_Octant *o, s32 dir, s32 col)
{
	const s32 *ray = g_ray + dir * TE_STEPS * 4L;
	s32 minor = o->zmajor ? (g_camX & 0x3fL) : (g_camZ & 0x3fL);
	s32 major = o->zmajor ? g_camZ : g_camX;
	/* ORIGIN 0x0036fbe6 - asr #6 then asl #6, and NO mask: the
	 * camera leaves the 4096-unit map and the index goes with it,
	 * on into the height save; temple.txt 7. */
	s32 cell = te_cell_clamp(((g_camZ >> 6) * TE_GRID)
	                         + (g_camX >> 6));
	s32 acc, dist, water = TE_H - 1L, pass, wall = 0L;

	/* the two corner pairs, by crossing and direction */
	static const s32 ROW_F[4] = { 2, 3, 0, 1 }, ROW_B[4] = { 0, 1, 2, 3 };
	static const s32 CEL_F[4] = { 1, 3, 0, 2 }, CEL_B[4] = { 0, 2, 1, 3 };

	g_prevTop = 0L;             /* ORIGIN 0x0036fcc8, moveq #0,D4 */

	if (o->mirror) minor ^= 0x3fL;
	acc = (major << 8) & 0x3f00L;
	acc += o->sub ? -ray[minor * 4L + 0L] : ray[minor * 4L + 0L];
	dist = ray[minor * 4L + 1L];

	while (water > 0 && dist < 0x8000L) {
		for (pass = 0; pass < 2 && water > 0; pass++) {
			/* pass 0 is the conditional crossing - a row when the
			 * octant is z-major, a cell otherwise */
			/* ORIGIN the sixteen step instructions - EACH AXIS
			 * HAS ITS OWN DIRECTION; temple.txt 7. */
			int row = (pass == 0) ? o->zmajor : !o->zmajor;
			int back = (pass == 0) ? o->sub : o->mirror;
			const s32 *k = row ? (back ? ROW_B : ROW_F)
			                   : (back ? CEL_B : CEL_F);
			s32 delta = (row ? TE_GRID : 1L) * (back ? -1L : 1L);
			const Temple_Cell *q;
			s32 frac, d, near, far;

			if (pass == 0) {
				if (o->sub) { if (acc >= 0) continue; }
				else        { if (acc < 0x4000L) continue; }

				{
					s32 j;

					/* ORIGIN 0x0036fcda - the crossing CONSUMES
					 * the carry; temple.txt 7. */
					acc &= 0x3fffL;
					j = (acc >> 8) & 0x3fL;
					if (o->sub) j ^= 0x3fL;
					d = dist - ray[j * 4L + 3L];
					/* ORIGIN 0x003709c4 - the eori is on the RAY
					 * TABLE's fraction alone; temple.txt 7. */
					frac = ray[j * 4L + 2L];
					if (o->mirror) frac ^= 0x3fffL;
				}
			} else {
				d = dist;
				frac = acc & 0x3fffL;
			}

			q = g_cells + cell;
			near = q->y[k[0]]
			     + (((q->y[k[1]] - q->y[k[0]]) * frac) >> 8 >> 6);
			cell = te_cell_clamp(cell + delta);
			/* ORIGIN 0x0036fdd0 and the fifteen like it - the wall
			 * selector is the EDGE the ray crosses, off the cell
			 * being entered. temple.txt 7. */
			wall = (s32)(u16)g_cells[cell].c[row ? (back ? 2 : 0)
			                                     : (back ? 1 : 3)];
			far = g_cells[cell].y[k[2]]
			    + (((g_cells[cell].y[k[3]] - g_cells[cell].y[k[2]])
			        * frac) >> 8 >> 6);
			/* ORIGIN 0x0036fe4e - q is the cell being LEFT. */
			water = draw_span(col, near, far, d, frac, q, water,
			                  (pass ? TE_SP_PASS1 : 0L)
			                  | (row ? TE_SP_ROW : 0L)
			                  | (back ? TE_SP_BACK : 0L)
			                  | ((pass && o == &TE_OCTANT[3])
			                     ? TE_SP_LONG : 0L), wall);
			if (pass) te_park(frac, row, back);
		}
		acc  += o->sub ? -ray[0] : ray[0];
		dist += ray[1];
	}

	/* ORIGIN 0x003700b4 - whatever the terrain never reached is
	 * 0x2f2f3f2f, the shot's fog colour at full light. */
	{
		s32 y;

		for (y = 0; y < water; y++) {
			u8 *px = g_frame + (size_t)(y * TE_W + col) * PX_BYTES;

			px[PX_R] = (u8)g_fog[0]; px[PX_G] = (u8)g_fog[1];
			px[PX_LIGHT] = (u8)CHAN_MAX; px[PX_B] = (u8)g_fog[2];
		}
	}
}

/* ORIGIN 0x0036a272 Temple_HalveVerts and 0x0036ab58
 * Temple_SetCentroid - the geometry is halved in place ONCE and an
 * object's position is the average of its own vertices, so there is
 * no placement table: the vertices are already world coordinates. */
static int load_objects(void)
{
	s32 i, k, total = 0;

	for (i = 0; i < g_set->nobj; i++) {
		g_objNF[i] = (s32)Image_U16(g_set->geom[i][1]) + 1L;
		g_objNV[i] = (s32)Image_U32(g_set->geom[i][2]) + 1L;
		g_objAt[i] = total;
		total += g_objNV[i];
	}
	if (!g_objVert) g_objVert = (s32 *)malloc((size_t)total * 3L * sizeof(s32));
	if (!g_objVert) return 1;

	for (i = 0; i < g_set->nobj; i++) {
		s32 sum[3];

		sum[0] = sum[1] = sum[2] = 0;
		for (k = 0; k < g_objNV[i]; k++) {
			s32 *v = g_objVert + (g_objAt[i] + k) * 3L;
			u32 at = g_set->geom[i][2] + 4UL + (u32)(k * 12L);
			s32 c;

			for (c = 0; c < 3; c++) {
				v[c] = (s32)Image_U32(at + (u32)(c * 4L))
				       >> g_set->halve;
				sum[c] += v[c];
			}
		}
		for (k = 0; k < 3; k++) g_objCent[i][k] = sum[k] / g_objNV[i];
	}
	return 0;
}

/* ORIGIN 0x00372b78, 0x00372b7c, 0x00372b80 and the two angles
 * latched beside them - THE OBJECT CAMERA, which is not the terrain
 * one. Shot 1 leaves it at zero and draws no objects; shot 2 drives
 * it itself. temple.txt 9.1. */
static s32 g_objCamX, g_objCamZ, g_objCamY;
static s32 g_objYaw;                /* 0x00372b70, the object's  */
static s32 g_objPan, g_objTilt;     /* 0x00372b90, 0x00372b94    */

/* ORIGIN 0x0036b456 and the two halves of Temple_RotateCam
 * 0x00369ef0 - the same rotation all three times, cos out of
 * g_CosTable and sin out of g_SinTable on a 0x2000-step circle. */
static void obj_rot(s32 *a, s32 *b, s32 ang)
{
	s32 c = Gfx_Cos(ang & 0x1fffL), si = Gfx_Sin(ang & 0x1fffL);
	s32 na = ((*a * c) + (*b * si)) >> 14;
	s32 nb = ((*b * c) - (*a * si)) >> 14;

	*a = na;
	*b = nb;
}

/* ORIGIN 0x00369e7a Temple_ProjectVerts, and 0x00369e04
 * Temple_ProjectVertsLate for the rise - one vertex to the screen in
 * THREE ROTATIONS, not one: by g_TempleObjYaw, then
 * g_TempleCamPan and g_TempleCamTilt.
 *
 * The order is what matters. Folding them into a single angle swings
 * the camera's own offset with the scene and throws every object off
 * the side of the screen - temple.txt 9.1.
 *
 * THE TWO ROUTINES DIFFER ONLY IN WHERE THE CAMERA COMES OFF: the
 * first subtracts BETWEEN the object yaw and the camera rotation,
 * the second rotates fully and subtracts AFTER, which makes its
 * offset a pan in view space rather than a position in the world.
 * Everything past the clip is identical. temple.txt 11.2. */
static int obj_project_m(const s32 *v, s32 *sx, s32 *sy, s32 *depth,
                         int late)
{
	s32 x = v[0] >> TE_OBJ_SHIFT;
	s32 y = v[1] >> TE_OBJ_SHIFT;
	s32 z = v[2] >> TE_OBJ_SHIFT;
	s32 d;

	obj_rot(&x, &y, g_objYaw);
	if (!late) { x -= g_objCamX; y -= g_objCamZ; z -= g_objCamY; }
	obj_rot(&x, &y, g_objPan);
	obj_rot(&y, &z, g_objTilt);
	if (late) { x -= g_objCamX; y -= g_objCamZ; z -= g_objCamY; }
	d = y + TE_OBJ_NEAR;
	if (d <= TE_OBJ_CLIP) return 0;
	*sx = ((x << 8) / d) + TE_OBJ_CX;
	*sy = ((z << 8) / d) + TE_OBJ_CY;
	*depth = d;
	return 1;
}

/* OURS - the two call shapes the rest of the file uses. */
static int obj_project(const s32 *v, s32 *sx, s32 *sy, s32 *depth)
{
	return obj_project_m(v, sx, sy, depth, 0);
}

/* ORIGIN 0x0036a29a - the cullMode Temple_BuildObjects writes to
 * Relic_SceneObject +0x4c, one per object. An object it does not
 * write keeps the array's zero. */
static const s32 TE_CULL[TE_OBJS] = {
	0, 0, 1, 1, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0
};


/* ORIGIN 0x0036879c - the cross product of the two edges leaving
 * corner 2, and THE MULTIPLIES ARE muls.w: the differences are
 * taken as longs and then used as words, so a span wider than a
 * word wraps rather than saturating. Mode 2 keeps every face, mode
 * 0 takes the cross as it stands and any other mode NEGATES it
 * first; a face survives only while the result is positive. */
static int face_faces_us(const s32 *x, const s32 *y, s32 mode)
{
	s32 ax, ay, bx, by, cross;

	if (mode == 2L) return 1;
	ax = (s32)(s16)(x[0] - x[2]);
	ay = (s32)(s16)(y[0] - y[2]);
	bx = (s32)(s16)(x[1] - x[2]);
	by = (s32)(s16)(y[1] - y[2]);
	cross = ax * by - ay * bx;
	if (mode != 0L) cross = -cross;
	return cross > 0L;
}

/* ORIGIN 0x0036a96a and 0x0036a9aa - records 13 and 14, the two
 * type-4 glows, and the 0x493e0 their SORT position carries off
 * component 0 either way. temple.txt 11.8. */
#define TE_GLOW4_P13 0x003e12c0UL
#define TE_GLOW4_P14 0x003e12ccUL
#define TE_GLOW4_RAD 0x003e12d8UL
#define TE_GLOW4_CLIP 0x1388L

/* ORIGIN 0x0036a29a Temple_BuildObjects - this scene's own set, and
 * the default. objset.h says why there are two. */
static const Relic_ObjSet TE_SET = {
	TE_OBJ, TE_OBJS, TE_UV, TE_UVPASS_N, TE_CULL, 0, 1L,
	TE_GLOW4_P13, TE_GLOW4_P14, TE_GLOW4_RAD, 0L
};
static s32 g_setGlowTick;

/* OURS - objset.h. */
void Temple_SetGlowCounter(s32 c) { g_setGlowTick = c; }

static void draw_terrain(void);
static void blur_frame(void);
static void rot2d_deg(s32 *x, s32 *y, s32 ang);
static int  te_terrain_init(void);

/* OURS - objset.h. The cast reads five globals and writes the
 * module's own 160x100 buffer; this stages the five, runs the shot's
 * plain-terrain pass and lifts the result into the frame the same
 * way te_frame does. */
void Temple_TerrainDrawTo(Relic_Frame *f, const s32 *cam)
{
	s32 py;

	if (!g_cells) { Frame_Clear(f); return; }
	g_camX = cam[0]; g_camY = cam[1]; g_camZ = cam[2];
	g_heading = cam[3]; g_horizon = cam[4];
	for (py = 0; py < TE_PIXELS * PX_BYTES; py++) g_frame[py] = 0;
	draw_terrain();
	blur_frame();
	for (py = 0; py < f->h; py++) {
		s32 sy = PX_TO_DEMO(py, f->scale);
		u8 *dst;
		s32 px;

		if (sy < 0 || sy >= TE_H) continue;
		dst = f->px + (size_t)(py * f->w) * PX_BYTES;
		for (px = 0; px < f->w; px++, dst += PX_BYTES) {
			s32 sx = PX_TO_DEMO(px, f->scale);
			const u8 *src;

			if (sx < 0 || sx >= TE_W) continue;
			src = g_prev + (size_t)(sy * TE_W + sx) * PX_BYTES;
			dst[PX_R] = src[PX_R]; dst[PX_G] = src[PX_G];
			dst[PX_B] = src[PX_B];
			dst[PX_LIGHT] = 0;
			fog_pixel(dst, (s32)src[PX_LIGHT]);
		}
	}
}

/* OURS - objset.h. */
void Temple_HeadingStep(s32 heading, s32 dist, s32 *dx, s32 *dz)
{
	s32 x = 0L, y = dist;

	rot2d_deg(&x, &y, heading);
	*dx = x; *dz = y;
}

/* OURS - a point on the demo's own camera track, NOT its start:
 * TE_TIME0 opens facing unlit ground a few units off, which is
 * right for the shot and useless to look at. */
#define TE_TERRAIN_HOME 0x1000L

/* OURS - objset.h. The pose comes back in `cam`, read off that
 * track rather than invented. */
int Temple_TerrainInit(s32 *cam)
{
	if (te_terrain_init()) return 1;
	g_camTime = TE_TERRAIN_HOME;
	eval_camera();
	cam[0] = g_camX; cam[1] = g_camY; cam[2] = g_camZ;
	cam[3] = g_heading; cam[4] = g_horizon;
	return 0;
}
const Relic_ObjSet *Temple_DefaultObjectSet(void) { return &TE_SET; }
#define TE_GLOW_OFF  0x493e0L

static int g_glowsInSort;

static void draw_glow_object(Relic_Frame *f, u32 posAddr, int late);

/* ORIGIN 0x0036844e - THE SORT KEY IS NOT THE DRAWN DEPTH. It is the
 * object's own position turned by the yaw and the camera, with the
 * camera's offset NEVER subtracted and no near bias added, so a
 * record sorts by where it sits in the world rather than by how far
 * the lens is from it. temple.txt 11.8. */
static s32 obj_key(const s32 *v)
{
	s32 x = v[0] >> TE_OBJ_SHIFT;
	s32 y = v[1] >> TE_OBJ_SHIFT;
	s32 z = v[2] >> TE_OBJ_SHIFT;

	obj_rot(&x, &y, g_objYaw);
	obj_rot(&x, &y, g_objPan);
	obj_rot(&y, &z, g_objTilt);
	return y;
}

/* ORIGIN 0x0036a99c and 0x0036a9dc - a glow's SORT position is its
 * point with 0x493e0 off component 0, away from the hull either
 * way. The point itself is where it draws. */
static void glow_centre(u32 posAddr, s32 off, s32 *out)
{
	const u8 *p = Image_At(posAddr);

	out[0] = out[1] = out[2] = 0L;
	if (!p) return;
	out[0] = (s32)RD_U32(p) + off;
	out[1] = (s32)RD_U32(p + 4L);
	out[2] = (s32)RD_U32(p + 8L);
}

/* ORIGIN 0x003685e2 Temple_DrawObjects and 0x0036843a
 * Temple_DrawObjectRange - painter-sorted on obj_key, far first.
 * ITS TWO GLOBALS ARE FIRST AND LAST, NOT FIRST AND A COUNT, and
 * reading them the other way loses object 12. temple.txt 9.2.
 *
 * THE TYPE-4 GLOWS SORT IN WITH THE MESHES, which is the only way
 * they land on the right side of the hull: their two keys are equal
 * and opposite, so one is always in front and one behind, and which
 * is which turns with the ship. temple.txt 11.8.
 *
 * The cull runs unconditionally where the demo lets a face whose +0
 * word is 2 bypass it - the baked geometry reads 2 everywhere and
 * the demo's own rebuilt arrays read 0, so honouring the word here
 * would disable the cull. temple.txt 8.1. */
static void draw_objects_range(Relic_Frame *f, s32 first,
                               s32 last, int late, u32 mask,
                               int shaded, s32 shade)
{
	s32 order[TE_OBJS + 2], key[TE_OBJS + 2], i, j, n = 0;

	if (last >= g_set->nobj) last = g_set->nobj - 1L;
	for (i = first; i <= last; i++) {
		const u8 *m = Image_At(mask + (u32)(i * 4L));
		s32 sx, sy, d, cen[3];

		if (m && RD_U32(m) != 0UL) continue;
		if (!obj_project_m(g_objCent[i], &sx, &sy, &d, late))
			continue;
		cen[0] = g_objCent[i][0];
		cen[1] = g_objCent[i][1];
		cen[2] = g_objCent[i][2];
		if (g_set->sort_bias) cen[0] += g_set->sort_bias[i];
		order[n] = i; key[n] = obj_key(cen); n++;
	}
	if (g_glowsInSort) {
		s32 c[3];

		glow_centre(g_set->glow_a, -TE_GLOW_OFF, c);
		order[n] = -1L; key[n] = obj_key(c); n++;
		glow_centre(g_set->glow_b, TE_GLOW_OFF, c);
		order[n] = -2L; key[n] = obj_key(c); n++;
	}
	for (i = 0; i < n - 1; i++)
		for (j = 0; j < n - 1 - i; j++)
			if (key[j] < key[j + 1]) {
				s32 t = key[j]; key[j] = key[j + 1]; key[j + 1] = t;
				t = order[j]; order[j] = order[j + 1]; order[j + 1] = t;
			}

	for (i = 0; i < n; i++) {
		s32 o = order[i], k;
		const u8 *face;

		if (o < 0L) {
			draw_glow_object(f, o == -1L ? g_set->glow_a
			                            : g_set->glow_b, late);
			continue;
		}
		face = Image_At(g_set->geom[o][0]);
		if (!face) continue;
		for (k = 0; k < g_objNF[o]; k++) {
			const u8 *fc = face + (size_t)(k * 14L);
			const s32 *vp[3];
			const Te_UvPass *pass = 0;
			s32 x[3], y[3], px[3], py[3], u[3], vv[3], d, c;
			const u8 *tex;
			int ok = 1;

			for (c = 0; c < 3; c++) {
				s32 idx = (s32)RD_U16(fc + 2L + c * 4L);

				if (idx < 0 || idx >= g_objNV[o]) { ok = 0; break; }
				vp[c] = g_objVert + (g_objAt[o] + idx) * 3L;
				if (!obj_project_m(vp[c], &x[c], &y[c],
				                   &d, late)) {
					ok = 0;
					break;
				}
				px[c] = x[c];
				py[c] = y[c];
				x[c] <<= RAST_FRAC;
				y[c] <<= RAST_FRAC;
			}
			if (!ok) continue;
			if (!face_faces_us(px, py, g_set->cull[o])) continue;
			for (c = 0; c < g_set->nuv; c++) {
				if (g_set->uv[c].obj != o) continue;
				if (!pass) pass = &g_set->uv[c];
				if (uv_claims(&g_set->uv[c], vp[0], vp[1], vp[2]))
					pass = &g_set->uv[c];
			}
			if (!pass) continue;
			tex = Image_At(pass->tex);
			if (!tex) continue;
			for (c = 0; c < 3; c++)
				uv_corner(pass, vp[c], &u[c], &vv[c]);
			if (shaded)
				Rast_ShadeTri(f, x, y, u, vv, tex,
				              pass->rows, shade);
			else
				Rast_TexTri(f, x, y, u, vv, tex, pass->rows);
			g_objFacesDrawn++;
		}
	}
}

/* OURS - select a set and load its geometry. objset.h. */
int Temple_UseObjectSet(const Relic_ObjSet *set)
{
	if (!set) return 1;
	if (g_set != set && g_objVert) { free(g_objVert); g_objVert = 0; }
	g_set = set;
	return load_objects();
}

/* ORIGIN 0x0036843a Temple_DrawObjectRange - a slice of the set,
 * with the caller's own skip table. objset.h. */
void Temple_DrawObjectRange(Relic_Frame *f, s32 angle, const s32 *cam,
                            s32 first, s32 last, u32 mask)
{
	s32 glows = g_glowsInSort;

	g_objYaw  = angle;
	g_objCamX = cam[0];
	g_objCamZ = cam[1];
	g_objCamY = cam[2];
	g_objPan  = cam[3];
	g_objTilt = cam[4];
	g_glowsInSort = g_set->glow_a != 0UL;
	draw_objects_range(f, first, last, 0, mask, 0, 0L);
	g_glowsInSort = glows;
}

/* ORIGIN 0x003683a4 - a single vertex of a single object, projected
 * through the object camera. Temple_StepMode1 centres the ark's
 * light on one out of the object's own point buffer. objset.h. */
int Temple_ProjectObjectVertex(s32 angle, const s32 *cam, s32 obj,
                               s32 vertex, s32 *sx, s32 *sy)
{
	s32 d;

	if (obj < 0 || obj >= g_set->nobj) return 0;
	if (!g_objVert || vertex < 0 || vertex >= g_objNV[obj]) return 0;
	g_objYaw  = angle;
	g_objCamX = cam[0];
	g_objCamZ = cam[1];
	g_objCamY = cam[2];
	g_objPan  = cam[3];
	g_objTilt = cam[4];
	return obj_project_m(g_objVert + (g_objAt[obj] + vertex) * 3L,
	                     sx, sy, &d, 0);
}

/* ORIGIN 0x002d18f8 Poly_DrawFaceList through Scene_SortAndDrawAll -
 * the departure shot's whole ship, from the space camera rather than
 * the temple's. The two projections agree in every constant, so this
 * stages the temple's object camera from the five the space engine
 * passes and lets the one renderer do the work. */
void Temple_DrawObjectSet(Relic_Frame *f, s32 angle, const s32 *cam)
{
	Temple_DrawObjectRange(f, angle, cam, 0L, g_set->nobj - 1L, 0UL);
}

/* ORIGIN 0x0036fbce maybe_Temple_DrawTerrain - a ground test, then a
 * dispatch by heading. */
static void draw_terrain(void)
{
	s32 h = g_heading % TE_HEADINGS;
	const Temple_Cell *under;
	const Temple_Plan *p;
	s32 n, col = 0;

	if (h < 0) h += TE_HEADINGS;
	under = g_cells + te_cell_clamp(((g_camZ >> 6) * TE_GRID)
	                                + (g_camX >> 6));
	for (n = 0; n < 4; n++) {
		if (g_camY >= under->y[n]) {
			/* the camera at or under the terrain: the demo fills the
			 * whole frame with one pixel and returns */
			s32 i;

			for (i = 0; i < TE_PIXELS; i++) {
				u8 *d = g_frame + (size_t)i * PX_BYTES;

				d[PX_R] = 8; d[PX_G] = 0; d[PX_LIGHT] = 0; d[PX_B] = 0;
			}
			return;
		}
	}

	p = g_plan + h * 2L;
	for (n = 0; n < 2; n++) {
		s32 c;

		if (p[n].octant < 0) break;
		for (c = 0; c <= p[n].columns && col < TE_W; c++, col++) {
			/* ORIGIN 0x003700ca against 0x0037098e - A BIASED
			 * OCTANT SWEEPS BACKWARDS; temple.txt 7. */
			const Temple_Octant *o = &TE_OCTANT[p[n].octant & 7];
			s32 base = (s32)p[n].ray;
			s32 dir;

			if (o->bias) dir = TE_DIRS - base - 2L - c * 2L;
			else         dir = base + c * 2L;
			if (dir < 0) dir = 0;
			if (dir >= TE_DIRS) dir = TE_DIRS - 1L;
			cast_column(o, dir, col);
		}
	}
}

/* ORIGIN 0x0036e238 Temple_BlurFrame - a five-tap blur with temporal
 * feedback. The demo does it four bytes at a time with an 0xfefefefe
 * mask so no carry crosses a channel; one longword is one whole
 * pixel, so this averages per channel. */
static void blur_frame(void)
{
	s32 x, y, k;

	for (y = 0; y < TE_H; y++) {
		for (x = 0; x < TE_W; x++) {
			size_t i = (size_t)(y * TE_W + x) * PX_BYTES;
			const u8 *c = g_frame + i;
			const u8 *up = (y > 0) ? c - TE_ROWBYTES : c;
			const u8 *dn = (y < TE_H - 1) ? c + TE_ROWBYTES : c;
			const u8 *lf = (x > 0) ? c - PX_BYTES : c;
			const u8 *rt = (x < TE_W - 1) ? c + PX_BYTES : c;
			u8 *d = g_prev + i;

			for (k = 0; k < PX_BYTES; k++) {
				s32 v = ((s32)up[k] + dn[k]) >> 1;

				v = (v + (((s32)lf[k] + rt[k]) >> 1)) >> 1;
				v = (v + c[k]) >> 1;
				v = (v + d[k]) >> 1;
				d[k] = (u8)v;
			}
		}
	}
}

static u8 *g_blurOut;
static s32 g_blurOutN;

/* OURS - a scene can be entered more than once, and the filter's
 * other half must not survive that. */
void Temple_BlurFrameReset(void)
{
	if (g_blurOut)
		memset(g_blurOut, 0, (size_t)g_blurOutN * PX_BYTES);
}

/* ORIGIN 0x0036e238 - the same filter over the host's frame. THE
 * HISTORY IS THE PICTURE: the demo blurs its render buffer into the
 * one it presents and feeds that back in, so what is handed on is
 * the history and not the render. objset.h. */
void Temple_BlurFrameTo(Relic_Frame *f)
{
	s32 x, y, k, n = f->w * f->h, rowb = f->w * PX_BYTES;

	if (!g_blurOut || g_blurOutN < n) {
		free(g_blurOut);
		g_blurOut = (u8 *)calloc((size_t)n, PX_BYTES);
		g_blurOutN = g_blurOut ? n : 0;
	}
	if (!g_blurOut) return;
	for (y = 0; y < f->h; y++)
		for (x = 0; x < f->w; x++) {
			size_t i = (size_t)(y * f->w + x) * PX_BYTES;
			const u8 *c  = f->px + i;
			const u8 *up = (y > 0) ? c - rowb : c;
			const u8 *dn = (y < f->h - 1) ? c + rowb : c;
			const u8 *lf = (x > 0) ? c - PX_BYTES : c;
			const u8 *rt = (x < f->w - 1) ? c + PX_BYTES : c;
			u8 *d = g_blurOut + i;

			for (k = 0; k < PX_BYTES; k++) {
				s32 v = ((s32)up[k] + dn[k]) >> 1;

				v = (v + (((s32)lf[k] + rt[k]) >> 1)) >> 1;
				v = (v + c[k]) >> 1;
				v = (v + d[k]) >> 1;
				d[k] = (u8)v;
			}
		}
	memcpy(f->px, g_blurOut, (size_t)n * PX_BYTES);
}

/* ---- the part --------------------------------------------------- */

/* OURS */
static int te_alloc(void)
{
	if (g_cells) return 0;
	/* ORIGIN 0x003679b8 - the grid and the save are one allocation
	 * in that order; a ray off the map reads on into the save. */
	g_cells    = (Temple_Cell *)malloc((size_t)TE_CELLS
	                                   * sizeof(Temple_Cell)
	                                   + (size_t)TE_CELLS * 8L);
	g_cellSave = (s16 *)((u8 *)g_cells
	                     + (size_t)TE_CELLS * sizeof(Temple_Cell));
	g_texG     = (u8 *)malloc((size_t)TE_ATLAS);
	g_texW     = g_texG ? g_texG + TE_ATLAS_G : 0;
	g_mapTex   = (u8 *)malloc((size_t)TE_MAPBYTES);
	g_ray      = (s32 *)malloc((size_t)TE_RAYLONGS * sizeof(s32));
	g_plan     = (Temple_Plan *)malloc((size_t)TE_HEADINGS * 2L
	                                   * sizeof(Temple_Plan));
	g_light    = (u16 *)malloc((size_t)TE_LIGHTS * sizeof(u16));
	g_rowOff   = (s16 *)malloc((size_t)TE_ROWS * sizeof(s16));
	g_uv       = (s16 *)malloc((size_t)(TE_UV_N * TE_UV_D) * sizeof(s16));
	g_vstep    = (s32 *)malloc(513L * sizeof(s32));
	g_frame    = (u8 *)malloc((size_t)TE_PIXELS * PX_BYTES);
	g_prev     = (u8 *)malloc((size_t)TE_PIXELS * PX_BYTES);
	return (g_cells && g_cellSave && g_texG && g_texW && g_mapTex
	        && g_ray && g_plan && g_light && g_rowOff && g_uv && g_vstep
	        && g_frame && g_prev) ? 0 : 1;
}

/* ORIGIN 0x0036db2a Temple_LoadSphereTex - the module loads Fcrust
 * into a buffer of its OWN, which is why the orb scene's pointer
 * cannot stand in for it: that one is filled by the orb part's init
 * and is null in every other part.
 *
 * THE SPHERE SAMPLES IT ROTATED, ABCD to DACB, and the buffer is
 * already in that order when the descent begins - the rotation is
 * Temple_ShuffleSphereTex 0x0036d0b4, run from Temple_RunShot1's
 * teardown [verified, on hardware]. A copy of ours is rotated
 * because the chunk pointer is shared with the orb scene, which
 * samples it unrotated - temple.txt. */
static const u8 *g_s3_sphere;

/* ORIGIN 0x0036db2a Temple_LoadSphereTex - see above. */
static int load_sphere_tex(void)
{
	const u8 *fc;
	u8 *cp;
	u32 len;
	s32 i;

	fc = Dat_Chunk("Fcrust", &len);
	if (!fc || len < 0x40000UL) return 1;
	cp = (u8 *)malloc(0x40000U);
	if (!cp) return 1;
	for (i = 0; i < 0x40000L; i += 4L) {
		cp[i]     = fc[i + 3]; cp[i + 1] = fc[i];
		cp[i + 2] = fc[i + 2]; cp[i + 3] = fc[i + 1];
	}
	g_s3_sphere = cp;
	return 0;
}

/* OURS. Both are defined with the shot-3 state they touch, below. */
static void shot3_reset(void);
static void build_blob_colour(void);
static void seed_sparks(void);
static void build_glow_colour(void);
static void build_noise(void);
static void build_light_curve(void);

/* OURS - THE TERRAIN'S OWN HALF OF te_init, split out so the cast
 * can be had without the orb, the objects or the sparks - and
 * without touching the object set, which a caller may own. The
 * order inside is the demo's. objset.h. */
static int te_terrain_init(void)
{
	const u8 *terr, *tex, *lg;
	u32 len;
	s32 i;

	if (Gfx_Init()) return 1;
	if (te_alloc()) return 1;

	terr = Dat_Chunk("Faztecterrain", &len);
	if (!terr || len < (u32)(3L * TE_MAPBYTES)) return 1;
	for (i = 0; i < TE_MAPBYTES; i++) g_mapTex[i] = terr[i];
	g_mapShade  = (u8 *)terr + TE_MAPBYTES;
	g_mapHeight = (u8 *)terr + TE_MAPBYTES * 2L;

	tex = Dat_Chunk("Faztectextures", &len);
	if (!tex || len < (u32)((TE_TILES_G + TE_TILES_W) * TE_PARTSIZE))
		return 1;
	for (i = 0; i < TE_TILES_G; i++)
		convert_tile(tex + (size_t)(i * TE_PARTSIZE), i, g_texG);
	for (i = 0; i < TE_TILES_W; i++)
		convert_tile(tex + (size_t)((TE_TILES_G + i) * TE_PARTSIZE),
		             i, g_texW);

	lg = Dat_Chunk("Ftemplelog", &len);
	if (!lg || len < TE_LOG_BYTES) return 1;
	if (!build_log_tables(lg)) return 1;

	build_light_table();
	build_row_offsets();
	build_uv_steps();
	build_vsteps();
	build_ray_table();
	build_heading_plan();
	build_cell_heights();
	build_cell_textures();
	save_cell_heights();
	unwrap_headings();
	return 0;
}

/* OURS */
static int te_init(void)
{
	s32 i;

	g_ready = 0;
	if (te_terrain_init()) return 1;
	seed_sparks();
	build_glow_colour();
	build_noise();
	build_light_curve();

	/* ORIGIN 0x0036d1a6 - the build chain's order, and the two
	 * routines it runs twice run twice. */
	g_objTex = Image_At(TE_OBJ_TEX);
	if (Temple_UseObjectSet(&TE_SET)) return 1;

	/* LANDMINE: Temple_ApplyDetailHeights IS NOT APPLIED HERE. It is
	 * reachable from Temple_RunShot1's setup, but the cells read off
	 * hardware one frame into the scene equal the UNMODIFIED
	 * transform of the height map, corner for corner [observed, on
	 * hardware] - so whatever calls it does not do so for this shot.
	 * Applying it costs three of every four corners a few units.
	 * apply_detail_heights() is kept and unused until that is read. */

	for (i = 0; i < TE_PIXELS * PX_BYTES; i++) { g_frame[i] = 0; g_prev[i] = 0; }

	if (load_sphere_tex()) return 1;
	shot3_reset();
	build_blob_colour();
	g_camTime = TE_TIME0;
	eval_camera();
	g_tick = 0L;
	g_ready = 1;
	return 0;
}

static u32 g_s3_seed;           /* g_TempleRandSeed 0x00372b0e */

/* ORIGIN 0x003680e8 Temple_Rand - an LCG whose product's top half is
 * the result, so it returns 0 .. n-1 without a modulo. */
static s32 te_rand(s32 n)
{
	u32 r;

	g_s3_seed = g_s3_seed * 0x5d43UL + 0x5c2dUL;
	r = (g_s3_seed ^ (g_s3_seed >> 16)) & 0xffffUL;
	return (s32)((((u32)n & 0xffffUL) * r) >> 16);
}

/* ORIGIN 0x0036ccbc Temple_FlashFrame - THE WHITEOUT. A flat add over
 * all 16000 pixels of the LIGHT byte, at the phase count plus the
 * flicker, clamped to 0x3f.
 *
 * THE COUNT IS THE FADE. It is loaded with 0x3e at each phase and
 * comes down a frame at a time, so a phase opens white and clears;
 * the arming that ends a phase drives it back up, so the phase closes
 * white again. The cut is hidden inside that. Only the LEVEL is
 * clamped - the add itself wraps, as the demo's byte add does. */
static void flash_frame(s32 level)
{
	s32 i;
	u8 *p;

	if (level <= 0L) return;
	if (level > 0x3fL) level = 0x3fL;
	p = g_frame + PX_LIGHT;
	for (i = 0; i < TE_PIXELS; i++, p += PX_BYTES)
		*p = (u8)(*p + (u8)level);
}

/* ORIGIN 0x00367ce4 Temple_WarpCorner - scale, then rotate. The
 * temple's own copy of CODE_03's Warp_ScaleRotateCorner, which
 * orb.c carries as warp_corner; the shifts differ and the table is
 * the same one. */
static void warp_corner(s32 x, s32 y, s32 scale, s32 angle,
                        s32 *ou, s32 *ov)
{
	s32 c  = Gfx_Cos(angle & ANGLE_MASK) >> 4;
	s32 sn = Gfx_Sin(angle & ANGLE_MASK) >> 4;
	s32 px = (scale * x) >> 12;
	s32 py = (scale * y) >> 12;

	*ou = (sn * py + c * px) >> 10;
	*ov = (c * py - sn * px) >> 10;
}

/* ORIGIN 0x00367d3a Temple_WarpTerrain - THE GROUND. The same three
 * corners and the same merged index as the cloud layer above it, but
 * it writes WHOLE PIXELS from a longword texture rather than adding
 * to one byte, and the texture is biased 0x20000 - 0x8000 longwords,
 * the middle of a 256 by 256 image.
 *
 * THE TINT ONLY GOES ON WHILE THE FLICKER IS LIT - the original
 * branches on g_TempleShot3Flash and has a second, untinted copy of
 * the whole loop for the frames it is dark.
 *
 * THE TINT IS A PACKED ADD. 0x40008 goes on as a 32-bit sum, so it
 * lifts green by 4 and blue by 8 and a carry out of blue reaches the
 * LIGHT byte. Adding the channels separately would lose that.
 *
 * [open] A1, the texture, is set by the caller. */
static void warp_terrain_px(u8 *dst, s32 dw, s32 dh, s32 scale,
                            const u8 *tex, s32 angle, s32 alt, s32 flash)
{
	s32 au, av, bu, bv, ou, ov, du, dv, su, sv, x, y;

	if (!tex || !dst) return;
	warp_corner( 0x5000L, -0x3200L, alt, angle, &au, &av);
	warp_corner(-0x5000L,  0x3200L, alt, angle, &bu, &bv);
	warp_corner(-0x5000L, -0x3200L, alt, angle, &ou, &ov);
	du = FX_DIV((au - ou) / TE_W, scale);
	dv = FX_DIV((av - ov) / TE_W, scale);
	su = FX_DIV((bu - ou) / TE_H, scale);
	sv = FX_DIV((bv - ov) / TE_H, scale);

	for (y = 0; y < dh; y++) {
		s32 u = ou + su * y + (0x80L << 8);
		s32 v = ov + sv * y;
		u8 *p = dst + (size_t)(y * dw) * PX_BYTES;

		for (x = 0; x < dw; x++, p += PX_BYTES) {
			s32 idx = ((v >> 8) & 0xffL) << 8 | ((u >> 8) & 0xffL);
			const u8 *t = tex + (size_t)((idx + 0x8000L) & 0xffffL) * 4L;
			u32 w = ((u32)t[0] << 24) | ((u32)t[1] << 16)
			      | ((u32)t[2] << 8)  | (u32)t[3];

			if (flash) w += 0x40008UL;
			p[0] = (u8)(w >> 24);
			p[1] = (u8)(w >> 16);
			p[2] = (u8)(w >> 8);
			p[3] = (u8)w;
			u += du;
			v += dv;
		}
	}
}

/* ORIGIN 0x00367d3a - the module's own 160 by 100 buffer. */
static void warp_terrain(const u8 *tex, s32 angle, s32 alt, s32 flash)
{
	warp_terrain_px(g_frame, TE_W, TE_H, FX_ONE, tex, angle, alt,
	                flash);
}

/* ORIGIN 0x00367d3a - the same, over the host's frame rather than
 * the module's own 160 by 100 one. The demo's ground is the whole
 * backdrop, not a strip: it writes every pixel of the frame and is
 * what the rest of the scene is then drawn over. objset.h. */
void Temple_WarpTerrainTo(Relic_Frame *f, const u8 *tex, s32 angle,
                          s32 alt, s32 flash)
{
	warp_terrain_px(f->px, f->w, f->h, f->scale, tex, angle, alt,
	                flash);
}

/* ORIGIN 0x00367e42 Temple_WarpClouds - a full-screen affine layer
 * ADDED INTO THE LIGHT BYTE, which is what makes shot 3's cloud
 * whiteouts. It is the layer OVER the ground, not the ground. Three corners at the same +/-0x5000, +/-0x3200 that
 * orb.c's Warp_Fill uses, differenced and divided by the frame's own
 * 160 and 100 for the per-pixel and per-row steps.
 *
 * THE DEMO KEEPS u ROTATED RIGHT BY EIGHT so its low byte is the
 * integer and the carry-around does the fraction; both accumulators
 * are 24.8 and both take >>8 here, which is the same arithmetic
 * without the packing. The texture is one byte a texel with its
 * origin at the middle of 64K, so the merged index is signed.
 *
 * THE TEXTURE IS g_TempleWarpTex 0x003b5500, 64K, put in A1 by a lea
 * at each call site. Temple_InitShot3 0x0036ce7c shifts it lsr.b #2
 * in place and Temple_HalveWarpTex another lsr.b #1 partway through.
 * Both are destructive on the image's own bytes, so a port reading
 * it read-only must shift a copy. */
static void warp_clouds(const u8 *tex, s32 angle, s32 scale, s32 bias)
{
	s32 au, av, bu, bv, ou, ov, du, dv, su, sv, x, y;

	if (!tex) return;
	warp_corner( 0x5000L, -0x3200L, scale, angle, &au, &av);
	warp_corner(-0x5000L,  0x3200L, scale, angle, &bu, &bv);
	warp_corner(-0x5000L, -0x3200L, scale, angle, &ou, &ov);
	du = (au - ou) / TE_W;
	dv = (av - ov) / TE_W;
	su = (bu - ou) / TE_H;
	sv = (bv - ov) / TE_H;

	for (y = 0; y < TE_H; y++) {
		s32 u = ou + su * y + (((bias >> 1) + 0x80L) << 8);
		s32 v = ov + sv * y;
		u8 *p = g_frame + (size_t)(y * TE_W) * PX_BYTES;

		for (x = 0; x < TE_W; x++, p += PX_BYTES) {
			s32 idx = ((v >> 8) & 0xffL) << 8 | ((u >> 8) & 0xffL);

			p[PX_LIGHT] = (u8)(p[PX_LIGHT]
			                   + tex[(idx + 0x8000L) & 0xffffL]);
			u += du;
			v += dv;
		}
	}
}

/* ORIGIN 0x0036bf36 Temple_RunShot3, the D0 = 0 arm. Four phases,
 * each the demo's own block of assignments followed by a blocking
 * loop over a per-frame step.
 *
 * dist and scale are STARTING values, not constants: the vbl hook at
 * 0x0037233e animates both as the orb falls, which is why dist reads
 * 245 in phase 1 where the block set 0x32 [observed, on hardware].
 * A zero here means the phase leaves that global alone.
 *
 * step 1 and 2 are maybe_Temple_StepShot3Mid and ...End, which are
 * not transcribed, so every phase currently runs the one step there
 * is. */
typedef struct {
	s32 dist;           /* g_TempleShot3Dist   0x00372b98 */
	s32 offx;           /* g_TempleShot3OffX   0x00372b9c */
	s32 phase;          /* g_TempleShot3Phase  0x00372b02 */
	s32 scale;          /* g_TempleWarpScale   0x00372b1e */
	s32 angle;          /* g_TempleWarpAngle   0x00372b16 */
	s32 bias;           /* g_TempleWarpBias    0x00372b00 */
	s32 alt;            /* the altitude        0x00372b1a */
	s32 settle;         /* mode 5's position   0x00372b22 */
	s32 sstep;          /* mode 5's step       0x0036c170 */
	int hook;           /* g_TempleHookMode, 1 or 5 */
	u32 ground;         /* a ground image to rebuild from, or 0 */
	int clockon;        /* 0x0036c866, which lets the clock run */
} Te_Shot3Phase;

/* The three ground images, 0x10400 apart. The descent swaps the
 * ground ONCE, at its third segment and from image C - temple.txt. */
#define TE_GROUND_A   0x00384900UL
#define TE_GROUND_B   0x00394d00UL
#define TE_GROUND_C   0x003a5100UL
#define TE_GROUND_PIX 65536L

/* The VBLs the ground rebuild burns, which only the angle keeps. */
#define TE_GROUND_VBL 3L

/* ORIGIN 0x0036d0e4 - defined with the ground state, below. */
static void build_ground_tex(u32 image_addr);

static const Te_Shot3Phase TE_SHOT3[] = {
	/* 0x0036bf64 */
	{ 0x32L,  0x50L, 0L, 0x3840L, 0x1190L,   0L,
	  0x2580L,      0L,     0L, 1, 0UL,          0 },
	/* 0x0036c016 */
	{    0L,     0L, 1L, 0x3840L,      0L, 100L,
	  0x1f40L,      0L,     0L, 1, 0UL,          0 },
	/* 0x0036c06c - the one ground swap, and the clock starts here */
	{  900L,     0L, 2L, 0x3840L,      0L,   0L,
	  0x1f40L,      0L,     0L, 1, TE_GROUND_C,  1 },
	/* 0x0036c0de */
	{ 0x640L,    0L, 0L,      0L,      0L,   0L,
	  0x15e00L, 0x15e00L, 0x100L, 5, 0UL,        0 }
};
#define TE_SHOT3_PHASES ((s32)(sizeof TE_SHOT3 / sizeof TE_SHOT3[0]))

/* THE COUNTER NEVER WRAPS. Each phase loads 0x3e and the hook takes
 * one off a frame until it reaches zero, where it stops. What ends
 * the phase is the hook ARMING it: once the warp scale falls below
 * TE_S3_ARM it adds three a frame against that one, so the count
 * climbs at two a frame until it passes 0x3e and the loop breaks.
 * docs/effects/temple.txt. */
#define TE_S3_LOAD   0x3eL
#define TE_S3_ARM    0x700L
#define TE_S3_DELAY  200L           /* g_TempleDistDelay, phase 0 */
#define TE_WARP_TEX   0x003b5500UL  /* g_TempleWarpTex */
#define TE_WARP_BYTES 0x10000L
#define TE_BLOBCOL_NUM 0xc80000L    /* D7 at 0x00367c4e */

static s32 g_s3_phase, g_s3_count, g_s3_dist, g_s3_offx;
static s32 g_s3_scale, g_s3_angle, g_s3_bias, g_s3_delay;
static s32 g_s3_alt, g_s3_settle, g_s3_sstep;
static s32 g_s3_flash, g_s3_flashticks, g_s3_anglelive;
/* THE DRAW READS THE RAMPS AS THEY STOOD BEFORE THE TICK'S MOVE,
 * because the original draws and then lets the hook move - these
 * are that frozen set. temple.txt. */
static s32 g_s3_altlive, g_s3_scalelive, g_s3_biaslive;
static s32 g_s3_distlive;
static s32 g_s3_spin_a, g_s3_spin_b;    /* 0x00372bc2, 0x00372bc4 */
static s32 g_s3_sang_a, g_s3_sang_b;    /* 0x00372bbe, 0x00372bc0 */
static int g_s3_live, g_s3_hook, g_s3_order;
static s32 g_s3_clock;              /* 0x0036c862 */
static s32 g_s3_blobjit;            /* the blob's radius jitter */
static s32 g_orb_h, g_orb_step;     /* 0x0036e784, 0x0036e788 */
static int g_orb;                   /* OURS: the sequence runs */
static int g_shot2;                 /* OURS: shot 2 runs      */
static int g_rise;                  /* OURS: the rise runs    */
static s32 g_s1_end;            /* the tick each cue answered on */
static s32 g_s3_end;
static s32 g_te_end;
static s32 g_te_blank;
static s32 g_orb_wait;              /* OURS: the blank before it */
#define TE_RISE_NEAR (-0xbb8L)      /* the camera stops here  */
#define TE_RISE_SHIP (-0xfa0L)      /* the gate at 0x0036c698 */
static s32 g_rise_x, g_rise_z, g_rise_y;   /* 0x00372b84/88/8c */
static s32 g_rise_roll;                    /* 0x00372b74       */
static s32 g_risePan, g_riseTilt;          /* 0x00372ba0/a4    */
static s32 g_rise_cnt, g_rise_fade;        /* 0x00372b36, 0x0036ccf8 */
static s32 g_rise_flick;                   /* 0x00372af8       */
static s32 g_rise_cut;                     /* 0x00372b04       */
static s32 g_rise_stage;                   /* OURS - see below */
static int g_rise_armed = 1;               /* OURS - see below */

static s32 g_rise_phase;                   /* 0x003728f0       */
static s32 g_tick_pulse;                   /* 0x00372b38       */

/* ORIGIN 0x003728f0 - the phase number Temple_RunShot1 steps after
 * the descent, and the clock each phase runs to. temple.txt 6. */
#define TE_PH_ORB   2
#define TE_PH_GLOW  3
#define TE_PH_BEAM  4
#define TE_PH_SHAKE 5
#define TE_PH_END   6
#define TE_PH_LAST  6

static const s32 TE_PHASE_END[TE_PH_LAST + 1] = {
	0L, 0L, 0x3caL, 0x514L, 0x578L, 0xb86L, 0xed8L
};
static int g_phase;
static s32 g_shakeAmp;              /* 0x00372aea */
static s32 g_dissolve;              /* 0x0037289e */
static int g_s3_clockon;            /* 0x0036c866 */

/* OURS. The phase's own assignments, which the original writes inline
 * before each loop. Phase 0 loads a counter of zero where the other
 * three load 0x3e, so the descent opens with no whiteout at all
 * [verified, on hardware]. */
static void shot3_enter(s32 ph)
{
	const Te_Shot3Phase *p;

	if (ph < 0 || ph >= TE_SHOT3_PHASES) { g_s3_live = 0; return; }
	/* ORIGIN 0x0036cee8 - temple.txt 9.6 */
	if (ph == 0L) Gfx_SetLightBase(0x3fL, 0x3fL, 0x3fL);
	p = &TE_SHOT3[ph];
	g_s3_delay = (ph == 0L) ? TE_S3_DELAY : 0L;
	if (p->dist)  g_s3_dist  = p->dist;
	if (p->offx)  g_s3_offx  = p->offx;
	if (p->scale) g_s3_scale = p->scale;
	if (p->angle) g_s3_angle = p->angle;
	g_s3_bias  = p->bias;
	if (p->alt)    g_s3_alt    = p->alt;
	if (p->settle) g_s3_settle = p->settle;
	if (p->sstep)  g_s3_sstep  = p->sstep;
	g_s3_hook  = p->hook;
	if (p->clockon) g_s3_clockon = 1;
	if (p->ground) {
		build_ground_tex(p->ground);
		g_s3_angle  -= 5L * TE_GROUND_VBL;
		g_s3_spin_a += 2L * TE_GROUND_VBL;
		g_s3_spin_b += 1L * TE_GROUND_VBL;
	}
	g_s3_phase = ph;
	g_s3_count = (ph == 0L) ? 0L : TE_S3_LOAD;
	g_s3_live  = 1;
}

/* ORIGIN 0x0037233e maybe_Temple_Shot3VblHook, its mode-1 arm - the
 * ramps behind phases 0 to 2. Mode 5, which phase 3 selects, moves a
 * different set and is not transcribed.
 *
 * THE BIAS MOVES BY THE PHASE INDEX, so the cloud layer is still in
 * phase 0, scrolls one a frame in phase 1 and two in phase 2. That is
 * the whole of its acceleration.
 *
 * MODE 5 IS PHASE 3's, and it eases rather than ramps: the altitude
 * comes from a position moved by a step that decays to zero, and
 * nothing arms the count because that phase ends on a music cue.
 *
 * [open] the hook also steps 372bbc, 372652, 372656, 372bc2 and
 * 372bc4. */
static void shot3_step(void)
{
	if (!g_s3_live) return;

	/* DELIBERATE: rolled here, not in the draw - temple.txt. */
	g_s3_blobjit = te_rand(5L);

	/* The live word trails the source by one step - temple.txt. */
	g_s3_anglelive = g_s3_angle;
	g_s3_altlive   = g_s3_alt;
	g_s3_scalelive = g_s3_scale;
	g_s3_biaslive  = g_s3_bias;
	g_s3_distlive  = g_s3_dist;

	if (g_s3_hook == 5) {
		/* A SETTLE, NOT A RAMP: the step decays a frame at a time, so
		 * the altitude eases to a stop and the loop waits on the cue
		 * rather than on the count. */
		g_s3_settle -= g_s3_sstep;
		if (g_s3_sstep > 0L) g_s3_sstep -= 1L;
		g_s3_alt = g_s3_settle >> 4;
		g_s3_angle -= 5L;
		if (g_s3_clock != 0L) g_s3_clock -= 1L;
		if (g_s3_count != 0L) g_s3_count -= 1L;
		return;
	}

	if (g_s3_clockon && g_s3_clock != 0L) g_s3_clock -= 1L;
	g_s3_alt   -= 0x10L;
	g_s3_angle -= 5L;
	g_s3_spin_a += 2L;
	g_s3_spin_b += 1L;

	/* one frame in a hundred opens a flicker of ten to nineteen */
	if (te_rand(100L) == 0L)
		g_s3_flashticks = te_rand(10L) + 10L;
	g_s3_flash = 0L;
	if (g_s3_flashticks != 0L) {
		g_s3_flash = 0xfL;
		g_s3_flashticks -= 1L;
	}

	g_s3_scale -= 0x40L;
	g_s3_bias  += g_s3_phase;

	if (g_s3_scale < TE_S3_ARM && g_s3_count < TE_S3_LOAD + 1L)
		g_s3_count += 3L;
	if (g_s3_count != 0L)
		g_s3_count -= 1L;

	/* dist waits out a delay, then climbs; crossing 699 is what
	 * flips g_TempleDrawOrder. */
	if (g_s3_delay < 1L) {
		g_s3_dist += 5L;
		if (g_s3_dist > 699L) g_s3_order = 1;
	} else {
		g_s3_delay -= 1L;
	}

	if (g_s3_count > TE_S3_LOAD)
		shot3_enter(g_s3_phase + 1L);
}

static u8 *g_s3_tex;
static u8 *g_s3_ground;             /* g_pTempleGroundTex 0x0037467c */

/* ORIGIN 0x0036d0e4 Temple_BuildGroundTex - a palette of 256
 * longwords then 65536 one-byte indices, expanded into the 256K
 * buffer the terrain warp samples.
 *
 * THE PALETTE ENTRY IS SHUFFLED, NOT COPIED. 0xAABBCCDD becomes
 * BB CC 00 DD, which drops the top byte and leaves the LIGHT slot
 * clear, and the whole longword is then masked 0xfcfcfcfc and
 * shifted right two - a per-channel divide by four into the demo's
 * six-bit range, safe as a 32-bit shift because the mask has already
 * cleared the bits that would cross a byte. */
static u32 g_groundSrc;             /* OURS - the atlas in use */

static void build_ground_tex(u32 image_addr)
{
	const u8 *src = Image_At(image_addr);
	const u8 *idx;
	s32 i;

	g_groundSrc = image_addr;
	if (!src || !g_s3_ground) return;
	idx = src + 0x400L;
	for (i = 0; i < TE_GROUND_PIX; i++) {
		const u8 *e = src + (size_t)idx[i] * 4L;
		u8 *d = g_s3_ground + (size_t)i * 4L;

		d[0] = (u8)((e[1] & 0xfc) >> 2);
		d[1] = (u8)((e[2] & 0xfc) >> 2);
		d[2] = 0;
		d[3] = (u8)((e[3] & 0xfc) >> 2);
	}
}

/* OURS. The warp table, shifted into a copy. Temple_InitShot3 does
 * its lsr.b #2 on the image's own bytes and image.c is read-only, so
 * the shift has to land somewhere of ours. */
static int shot3_load_tex(void)
{
	const u8 *src = Image_At(TE_WARP_TEX);
	s32 i;

	if (!src) return 1;
	if (!g_s3_ground)
		g_s3_ground = (u8 *)malloc((size_t)TE_GROUND_PIX * 4L);
	if (!g_s3_tex)
		g_s3_tex = (u8 *)malloc(TE_WARP_BYTES);
	if (!g_s3_tex || !g_s3_ground) return 1;
	for (i = 0; i < TE_WARP_BYTES; i++)
		g_s3_tex[i] = (u8)(src[i] >> 2);
	return 0;
}

/* OURS. The two terms every shot-3 step builds before it draws, and
 * the only use either makes of the distance. */
static void shot3_project(s32 *scale, s32 *xc)
{
	*xc    = ((g_s3_offx << 8) / g_s3_distlive) + 0x50L;
	*scale = 10000L / g_s3_distlive;
}

/* The 200 entries live at 0x00342528 in BSS, 16 bytes each, and
 * whatever seeds them has not been found. */
typedef struct {
	s32 x, y;           /* +0, +4 the seed direction */
	s32 spawn;          /* +8  compared against the clock */
	s32 dark;           /* +12 what it takes off each channel */
} Te_Particle;

#define TE_PARTICLES 200L

static Te_Particle g_s3_part[TE_PARTICLES];
static u8  g_s3_ramp[256];          /* 0x00374578 */

static u8 g_s3_satramp[256];        /* g_TempleSatRamp 0x00374478 */
static u32 g_s3_blobcol[256];       /* g_TempleBlobColour 0x00372fd0 */

/* ORIGIN 0x00367fae Temple_BuildBlobColour, with the amber its only
 * caller stages - Temple_BuildColourRamps 0x00367c2a, from
 * Temple_Init, builds three more tables of two colours against two
 * profiles and this is the one the blob reads.
 *
 * THE FOLD IS WHAT SHAPES IT. The quotient falls as i rises, and
 * while it is over 0x40 the fold turns it into 0x80 minus itself, so
 * the table is zero to entry 0x33, climbs to the full colour where
 * the two meet, then tails off. Entry 0x34 is 03 02 00 02 and 0x35
 * is 06 05 00 03 [verified, on hardware].
 *
 * BYTE +2 IS NEVER WRITTEN, which is why the blob adds no LIGHT. */
static void build_blob_colour(void)
{
	s32 i;

	for (i = 0; i < 256L; i++) {
		s32 u = (i * 200L) / 128L;
		s32 t = u * u;
		s32 v = TE_BLOBCOL_NUM;
		s32 r, g, b;

		if (t != 0L) v = (TE_BLOBCOL_NUM - t) / t;
		v >>= 4;
		if (v >= 0x40L) {
			v = -(v - 0x80L);
			if (v < 0L) v = 0L;
		}
		r = (0x3fL * v) >> 6; if (r > 0x3fL) r = 0x3fL;
		g = (0x2fL * v) >> 6; if (g > 0x3fL) g = 0x3fL;
		b = (0x20L * v) >> 6; if (b > 0x3fL) b = 0x3fL;
		g_s3_blobcol[i] = ((u32)r << 24) | ((u32)g << 16) | (u32)b;
	}
}

/* OURS. RE-ENTERING THE PART HAS TO PUT SHOT 3 BACK, because the
 * host calls init() every time the scene is skipped to and every one
 * of these carries over otherwise - g_s3_live left set sends the
 * first frame of the second visit into shot3_step with a scale that
 * ran thousands of units negative, which draws nothing.
 *
 * Temple_Init clears the seed too [verified, its clr.l on
 * g_TempleRandSeed], so a revisit runs the same flicker as the
 * first. THE THREE CUE TICKS CARRY OVER THE SAME WAY, and left
 * standing they skip shot 1 and the descent entirely. */
static void shot3_reset(void)
{
	g_s3_phase = 0L; g_s3_count = 0L; g_s3_dist = 0L; g_s3_offx = 0L;
	g_s3_scale = 0L; g_s3_angle = 0L; g_s3_bias = 0L; g_s3_delay = 0L;
	g_s3_alt = 0L;   g_s3_settle = 0L; g_s3_sstep = 0L;
	g_s3_flash = 0L; g_s3_flashticks = 0L; g_s3_anglelive = 0L;
	g_s3_altlive = 0L; g_s3_scalelive = 0L; g_s3_biaslive = 0L;
	g_s3_distlive = 0L;
	g_s3_spin_a = 0L; g_s3_spin_b = 0L;
	g_s3_sang_a = 0L; g_s3_sang_b = 0L;
	g_s3_live = 0;   g_s3_hook = 0;  g_s3_order = 0;
	g_s3_clock = 0L; g_s3_seed = 0UL; g_s3_clockon = 0;
	g_orb = 0; g_orb_h = 0L; g_orb_step = 0L; g_phase = 0; g_shakeAmp = 0L; g_shot2 = 0;
	g_rise = 0;
	g_track = TE_TRACK_A;
	g_s3_blobjit = 0L;
	g_s1_end = 0L; g_s3_end = 0L; g_te_end = 0L; g_te_blank = 0L;
	g_orb_wait = 0L;
}

/* ORIGIN 0x0036d07a Temple_BuildDarkenRamp - TWO tables.
 * g_s3_satramp gets all 256 entries, min(i, 0x3f); g_s3_ramp, the
 * darken ramp, gets only the FIRST 64 copied on.
 *
 * THE 192 ENTRIES ABOVE g_s3_ramp ARE NEVER WRITTEN, AND THAT IS
 * THE CLAMP. A speck darker than the pixel under it indexes with a
 * byte that wrapped past zero, lands in that zeroed tail and comes
 * out black. Filling them would be a different effect, not a tidier
 * one. The saturation ramp has no such tail - it holds 0x3f - which
 * is what the exhaust puffs read. */
static void shot3_build_ramp(void)
{
	s32 i, v = 0L;

	for (i = 0; i < 256L; i++) {
		g_s3_satramp[i] = (u8)v;
		v += 1L;
		if (v > 0x3fL) v = 0x3fL;
	}
	for (i = 0; i < 0x40L; i++)
		g_s3_ramp[i] = g_s3_satramp[i];
}

/* ORIGIN 0x0036c868 g_TempleParticleDark - what a speck takes off a
 * channel, one of ten picked at random. */
static const s32 TE_PART_DARK[10] = {
	24L, 16L, 8L, 2L, 4L, 6L, 32L, 28L, 8L, 12L
};

/* ORIGIN 0x0036c7cc Temple_SeedParticles - TWENTY FANS OF TEN, which
 * is why the specks read as streaks rather than as noise: each fan
 * shares one random direction and the ten differ only in how far out
 * they start.
 *
 * The clock starts at 0x200 against spawns of 0 to 0xff, so every
 * speck is live from the first frame and they die off as the hook
 * counts the clock down past each spawn in turn. */
static void shot3_seed_particles(void)
{
	Te_Particle *q = g_s3_part;
	s32 f, k;

	for (f = 0; f < 20L; f++) {
		s32 a  = te_rand(0x2000L);
		s32 sn = Gfx_Sin(a);
		s32 cs = Gfx_Cos(a);

		for (k = 0; k < 10L; k++, q++) {
			s32 r = te_rand(0x100L) + 8L;

			q->x     = (sn * r) >> 10;
			q->y     = (cs * r) >> 10;
			q->dark  = TE_PART_DARK[te_rand(10L)];
			q->spawn = te_rand(0x100L);
		}
	}
	g_s3_clock = 0x200L;
}

/* ORIGIN 0x0036c6be maybe_Temple_Shot3Particles - 200 specks that
 * DARKEN the frame rather than light it.
 *
 * Each walks outward along its own direction at a rate set by its
 * age, with a circular wobble from the sin and cos of that same age,
 * about the screen centre - 0x5000 and 0x3200 are 80 and 50 in 24.8,
 * the same constants the cloud warp uses for its corners.
 *
 * BOTH TRANSFORM INPUTS ARE MIRRORED: the angle is the cloud's
 * XOR 0x1fff and the scale is the altitude XOR 0x3fff, so the specks
 * counter-rotate against the cloud they are seen through.
 *
 * shot3_seed_particles fills the array; the hook counts the clock
 * down, so a speck dies when the clock reaches its spawn. The
 * darkness pulses rather than being the stored byte - temple.txt. */
static void shot3_particles(void)
{
	s32 n;

	for (n = 0; n < TE_PARTICLES; n++) {
		const Te_Particle *q = &g_s3_part[n];
		s32 u, v, age, idx, x, y, px, py, dark;
		u8 *p;

		age = g_s3_clock - q->spawn;
		if (age <= 0L) continue;
		dark = ((q->dark * (Gfx_Sin((age << 9) & 0x1fffL) >> 8))
		        >> 8) + 0xdL;
		warp_corner(q->x, q->y, g_s3_alt ^ 0x3fffL,
		            g_s3_anglelive ^ 0x1fffL, &u, &v);
		idx = (age & 0x3fL) << 7;
		x = (Gfx_Sin(idx) >> 7) + (((age & 0xffL) * u) >> 8) + 0x5000L;
		y = (Gfx_Cos(idx) >> 7) + (((age & 0xffL) * v) >> 8) + 0x3200L;
		px = x >> 8;
		py = y >> 8;
		if (px <= 0L || px >= TE_W || py <= 0L || py >= TE_H - 1L)
			continue;
		p = g_frame + (size_t)(py * TE_W + px) * PX_BYTES;
		p[PX_R] = g_s3_ramp[(u8)(p[PX_R] - (u8)dark)];
		p[PX_G] = g_s3_ramp[(u8)(p[PX_G] - (u8)dark)];
		p[PX_B] = g_s3_ramp[(u8)(p[PX_B] - (u8)dark)];
	}
}

/* ORIGIN 0x0036c40e Temple_StepShot3, its glow call - the
 * glow over the orb. ITS RADIUS IS NOT THE
 * SPHERE'S: the sphere's is doubled twice and given a jitter of up to
 * four, then clamped to 0x7f, and that clamp is what keeps the
 * falloff table's row index inside it. The roll happens every frame
 * whether the blob is visible or not, so it is part of the random
 * stream the flicker draws from. */
static void shot3_glow(s32 scale, s32 xc)
{
	s32 r = scale * 4L + g_s3_blobjit;

	if (r > 0x7fL) r = 0x7fL;
	Gfx_GlowBlobColourTo(g_frame, TE_W, TE_H, FX_ONE, xc, 0x32L, r,
	                     g_s3_blobcol);
}

/* ORIGIN 0x0036c40e Temple_StepShot3 - phases 0 and 1. The cloud
 * layer and the flash go before the objects or after them, depending
 * on g_TempleDrawOrder, which dist crossing 699 sets.
 *
 * The orb goes through Orb_DrawSphereTo, which is the body CODE_03's
 * Orb_DrawTexturedSphere and this hunk's copy share - ungated here,
 * because the temple's copy stores every texel where the orb scene
 * gates on its reveal.
 *
 * ITS TEXTURE IS RAW Fcrust, which is why orb.c's copy serves. The
 * temple loads the same chunk into its own buffer and that buffer
 * still holds the file's bytes unshuffled where shot 3 reads it
 * [verified, on hardware], so Temple_ShuffleSphereTex does not run
 * on the path this shot is reached by. The shading pass 0x0036b498
 * is not transcribed - docs/effects/temple.txt. */

static void shot3_step_main(int specks)
{
	s32 scale, xc;

	g_s3_sang_a = g_s3_spin_a >> 2;
	g_s3_sang_b = g_s3_spin_b >> 2;
	warp_terrain(g_s3_ground, g_s3_anglelive, g_s3_altlive,
	             g_s3_flash);
	if (specks) shot3_particles();
	if (!g_s3_order) {
		warp_clouds(g_s3_tex, g_s3_anglelive, g_s3_scalelive,
		            g_s3_biaslive);
		flash_frame(g_s3_flash + g_s3_count);
	}
	shot3_project(&scale, &xc);
	Orb_DrawSphereTo(g_frame, TE_W, TE_H, FX_ONE, g_s3_sphere,
	                 xc, 0x32L, scale, 0L, 0, g_s3_sang_a, g_s3_sang_b);
	shot3_glow(scale, xc);
	if (g_s3_order) {
		warp_clouds(g_s3_tex, g_s3_anglelive, g_s3_scalelive,
		            g_s3_biaslive);
		flash_frame(g_s3_flash + g_s3_count);
	}
}

/* ORIGIN 0x0036c56c maybe_Temple_StepShot3Mid - phase 2. The same
 * body with the particle pass at 0x0036c6be added, which is where
 * that effect starts. */
static void shot3_step_mid(void)
{
	shot3_step_main(1);
}

/* ORIGIN 0x0036c4e2 maybe_Temple_StepShot3End - phase 3, and it does
 * NOT draw the cloud layer: by the settle the orb is under it. The
 * flash runs unconditionally at the end rather than inside the
 * draw-order test, and the three cloud parameters are not copied. */
static void shot3_step_end(void)
{
	s32 scale, xc;

	warp_terrain(g_s3_ground, g_s3_anglelive, g_s3_altlive,
	             g_s3_flash);
	shot3_particles();
	shot3_project(&scale, &xc);
	Orb_DrawSphereTo(g_frame, TE_W, TE_H, FX_ONE, g_s3_sphere,
	                 xc, 0x32L, scale, 0L, 0, g_s3_sang_a, g_s3_sang_b);
	shot3_glow(scale, xc);
	flash_frame(g_s3_flash + g_s3_count);
}

/* ---- the orb over the pyramid -----------------------------------
 *
 * Temple_RunShot1's phases after the descent put an orb above the
 * temple and ease it down. The projection they place it with is
 * Temple_ProjectPoint, which is NOT the warp's: it turns on a second
 * sine table of 3600 entries and amplitude 255 where everything else
 * in this module uses 0x2000 of 0x4000. docs/effects/temple.txt.
 */

#define TE_DEG_SIN  0x00221aecUL
#define TE_DEG_COS  0x002228fcUL
#define TE_DEG_FULL 3600L

/* OURS. Image_S32 is not in image.h and one call wants it. */
static s32 te_deg(u32 base, s32 i)
{
	return (s32)Image_U32(base + (u32)(i * 4L));
}

/* ORIGIN 0x0036fb94 Temple_Rot2dDeg - tenths of a degree, amplitude
 * 255, so the shift is 8 and not the 14 every other rotation here
 * uses. THE INDEX IS NOT MASKED by the original; its one caller adds
 * 0xa0 to a heading and passes it on, so this keeps the range. */
static void rot2d_deg(s32 *x, s32 *y, s32 ang)
{
	s32 i = ang % TE_DEG_FULL;
	s32 c, sn, ox = *x, oy = *y;

	if (i < 0) i += TE_DEG_FULL;
	c  = te_deg(TE_DEG_COS, i);
	sn = te_deg(TE_DEG_SIN, i);
	*x = (oy * c - ox * sn) >> 8;
	*y = (ox * c + oy * sn) >> 8;
}

/* ORIGIN 0x0036fad2 Temple_ProjectPoint - a world point to the
 * screen, with the near and far rejection the orb and the beam both
 * gate on. Returns 0 and fills the three outputs, or -1.
 *
 * THE RADIUS GOES THROUGH THE SAME DIVIDE as the position, so the
 * caller gets a screen-space size without a second perspective.
 *
 * THE DEPTH KEY IS A MULTIPLY TABLE READ, not a reciprocal. The
 * original indexes the 64 by 256 word table at (0x003746a8) with
 * (depth << 4); that table holds ((row * col) >> 8) << 8, built by
 * 0x0036d456, so the key is computed here rather than stored. It
 * lands in the high byte, which is why OR-ing it into a texel sets
 * LIGHT and leaves blue alone. It is NOT monotonic in depth - it
 * falls to zero at every multiple of 16 - but that is what the demo
 * computes.
 *
 * [open] the original also writes the blob's angular phase on its
 * way out - screen x doubled into g_TempleBlobPhaseWord - which
 * couples a glow drawn after it to where the point landed. */
static int project_point(s32 x, s32 y, s32 z, s32 radius,
                         s32 *sx, s32 *sy, s32 *sr)
{
	x -= g_camX;
	y -= g_camZ;
	rot2d_deg(&x, &y, g_heading + 0xa0L);
	if (y <= 0L || y >= 0x7d0L) return -1;
	z -= g_camY >> 4;
	*sx = ((x << 8) / y) + 0x50L;
	*sy = ((z << 8) / y) + g_horizon;
	*sr = (radius << 8) / y;
	return 0;
}

/* THE THREE SPANS ARE MUSICAL - the fields our own Cmodule2 takes
 * between the cues bounding them, 2498/962/3638 against the demo's
 * 2490/962/3677 [observed]. FALLBACKS only: with a module playing
 * each transition waits on its poll. LANDMINE: g_TempleHookMode is
 * zero for part of ONE field - temple.txt 2 and 11.9. */
#define TE_SHOT1_SPAN  2498L
#define TE_DESCENT_LEN  962L
#define TE_RISE_LEN    3638L
#define TE_SHOT3_END   (TE_SHOT1_SPAN + TE_DESCENT_LEN)
#define TE_TICKS       (TE_SHOT3_END + TE_RISE_LEN)

/* The three cues this module polls, each with the gap the demo ran
 * between it and the one before as its fallback - 0x0036deae,
 * 0x0036c144 and 0x0036c3e6. THE RISE'S IS ARMED AT THE DESCENT'S
 * END, not at 0x0036c3c6 where the demo arms it, because that stage
 * has no hook of its own; a cue is a position floor either way.
 * temple.txt 11.9. */
#define TE_CUE1_ORDER  0x05L
#define TE_CUE1_ROW    0L
#define TE_CUE3_ORDER  0x09L
#define TE_CUE3_ROW    0x32L
#define TE_CUE4_ORDER  0x1fL
#define TE_CUE4_ROW    0x28L

/* ORIGIN 0x0036793c - THE MODULE'S EXIT BLANKS THE SCREEN, and the
 * space module holds that blank while it waits on (0x1f,0x30) at
 * 0x002c8264 and builds. temple.txt 11.9. */
#define TE_BLANK_ORDER 0x1fL
#define TE_BLANK_ROW   0x30L
#define TE_BLANK       21L

/* ORIGIN 0x0036c158 - the descent hands over on the same blank list,
 * and these are the fields the next shot spends building behind it.
 * THE MODULE STILL ENDS WHERE IT ENDED, so they come off the last
 * shot. temple.txt 11.9. */
#define TE_ORB_BLANK   36L


/* ORIGIN 0x0036de04 Temple_RunShot1 - the two values it stages
 * before its mode-2 phase. */
static void orb_enter(void)
{
	g_orb_h    = -0x34800L;
	g_orb_step = 0x3e8L;
	g_orb      = 1;
	g_phase    = TE_PH_ORB;
	/* ORIGIN 0x0036df4e - shot 1 re-asserts its own grey here,
	 * temple.txt 9.6 */
	g_fog[0] = 0x2fL; g_fog[1] = 0x2fL; g_fog[2] = 0x2fL;
	/* ORIGIN 0x0036df14 - Temple_SetCameraTrack on the second track,
	 * which restarts the clock rather than carrying shot 1's on. */
	g_track    = TE_TRACK_C;
	g_camTime  = TE_TIME_C;
	unwrap_headings();
	/* ORIGIN 0x0036e126 Temple_RetextureTerrain, called at 0x0036df6c
	 * - a second tile map copied over the first, and NOT at load: the
	 * ruins run on the original one. */
	{
		const u8 *retex = Image_At(A_RETEX);
		s32 i;

		if (retex) {
			for (i = 0; i < TE_MAPBYTES; i++) g_mapTex[i] = retex[i];
			build_cell_textures();
		}
	}
}

/* ORIGIN 0x00372a92 Temple_StepOrb - it eases rather than ramps: the
 * step decays five a frame to a floor of zero, so it coasts to a
 * stop.
 * ONE SPIN ACCUMULATES AND THE OTHER IS ASSIGNED: b is the camera's
 * horizon, not a rotation. temple.txt 6. */
static void orb_step(void)
{
	g_orb_h += g_orb_step;
	if (g_orb_step != 0L) {
		g_orb_step -= 5L;
		if (g_orb_step < 0L) g_orb_step = 0L;
	}
	g_s3_spin_a += 0xaL;
	g_s3_spin_b = ((s32)(s16)g_horizon) >> 1;
}

/* ORIGIN 0x0036e752 Temple_DrawBeamOrb - one sphere at a fixed world
 * position, its height eased by orb_step.
 *
 * [open] the original draws it through Temple_DrawTexturedSphereDepth
 * 0x0036ca9a, which ORs a depth into the texel's low word and keeps
 * the pixel only where that beats what the frame already holds - so
 * the pyramid occludes it. This takes the ungated body instead and
 * the orb is drawn over everything. */
static void draw_beam_orb(void)
{
	s32 sx, sy, sr;

	if (project_point(0x7a0L, 0x820L, g_orb_h >> 8, 0x64L,
	                  &sx, &sy, &sr)) return;
	Orb_DrawSphereTo(g_frame, TE_W, TE_H, FX_ONE, g_s3_sphere,
	                 sx, sy, sr, 0L, 0,
	                 g_s3_spin_a >> 2, g_s3_spin_b >> 2);
}

/* ---- the glow ----------------------------------------------------
 *
 * g_TempleGlowTable 0x0036eb9a is a count and two {x, y, z, radius}
 * entries, both on the orb's own column. It holds the FINISHED
 * state: mode 3 resets entry 0's radius to 0 and entry 1's z to
 * entry 0's, and the per-frame ramps grow them back to the table's
 * values. docs/effects/temple.txt 6.
 */

#define TE_GLOW_N      2L
#define TE_GLOW_X      0x7a0L
#define TE_GLOW_Y      0x820L
#define TE_GLOW_Z0     (-0x15eL)
#define TE_GLOW_R0     0x96L
#define TE_GLOW_Z1     (-0xbeL)
#define TE_GLOW_R1     0xc8L
#define TE_BEAM_Z      (-0x116L)
#define TE_BEAM_R      0x4eL
#define TE_GLOWCOL_NUM 0x5a0000L
/* ORIGIN 0x00367cd8 - the SECOND ramp Temple_BuildColourRamps makes
 * from the same three peaks, and the type-4 glows take this one. Over
 * twice the numerator, so it holds white to entry 0x48 where the
 * other gives up at 0x30 - these are the scene's bright glows. */
#define TE_GLOWCOL4_NUM 0xc80000L

static u32 g_glowcol[256];          /* g_TempleGlowColour 0x00373db0 */
static u32 g_glowcol4[256];         /* 0x00373470                   */
static s32 g_glowR;                 /* 0x0036ebaa, entry 0's radius  */
static s32 g_beamZ;                 /* 0x0036ebb6, entry 1's z       */
static s32 g_glowSX[TE_GLOW_N], g_glowSY[TE_GLOW_N], g_glowSR[TE_GLOW_N];

/* ORIGIN 0x00367f1e, which Temple_BuildColourRamps runs for this
 * ramp with peaks 0x3f, 0x4f, 0x7f. The same
 * curve as build_blob_colour WITHOUT its fold, and two of its three
 * peaks are past 0x3f so the core clamps flat and blows out. */
static void build_glow_ramp(u32 *out, s32 numer)
{
	s32 i;

	for (i = 0; i < 256L; i++) {
		s32 u = (i * 200L) / 128L;
		s32 t = u * u;
		s32 v = numer;
		s32 r, g, b;

		if (t != 0L) v = (numer - t) / t;
		v >>= 4;
		r = (0x3fL * v) >> 6; if (r > 0x3fL) r = 0x3fL;
		g = (0x4fL * v) >> 6; if (g > 0x3fL) g = 0x3fL;
		b = (0x7fL * v) >> 6; if (b > 0x3fL) b = 0x3fL;
		out[i] = ((u32)r << 24) | ((u32)g << 16) | (u32)b;
	}
}

/* OURS - both of them, in Temple_BuildColourRamps' order. */
static void build_glow_colour(void)
{
	build_glow_ramp(g_glowcol, TE_GLOWCOL_NUM);
	build_glow_ramp(g_glowcol4, TE_GLOWCOL4_NUM);
}

/* ORIGIN 0x0036b69e Temple_DrawGlowDisc and 0x0036b79a
 * Temple_DrawBeamGlow - ONE ROUTINE, because the two differ by a
 * single lookup. The disc carries (row << 8) in its polar index and
 * reads a half-width per row; the column keeps row 0 and the full
 * radius for every row, so its brightness falls off horizontally
 * only. `column' picks between them.
 *
 * Both ADD the colour longword and then MODULATE LIGHT by the
 * falloff level. The original's modulate is a read of the 64 by 256
 * table at (0x003746a8), which holds ((row * col) >> 8) << 8 and so
 * IS this multiply - temple.txt.
 *
 * THE POLAR INDEX IS WALKED, NOT RECOMPUTED. It is formed once a
 * row and stepped with (A5)+, so a span crossing column 255 carries
 * into the NEXT ROW of the table and the right half of every blob
 * reads its distances one row further down. Every span crosses. */
static void temple_glow_blob(s32 cx, s32 cy, s32 radius,
                             const u32 *colour, int column)
{
	s32 y, y0, y1;

	if (radius <= 0L) return;
	y0 = cy - radius; if (y0 < 0L) y0 = 0L;
	y1 = cy + radius; if (y1 > TE_H - 1L) y1 = TE_H - 1L;

	for (y = y0; y <= y1; y++) {
		s32 dy = y - cy;
		s32 half = column ? radius : Gfx_Isqrt(radius * radius
		                                       - dy * dy);
		s32 x0 = cx - half, x1 = cx + half, x, idx;
		u8 *row = g_prev + (size_t)(y * TE_W) * PX_BYTES;

		if (x0 < 0L) x0 = 0L;
		if (x1 > TE_W - 1L) x1 = TE_W - 1L;
		idx = (((column ? 0L : dy) & 0xffL) << 8)
		    | ((x0 - cx) & 0xffL);
		for (x = x0; x <= x1; x++) {
			s32 tr = (idx >> 8) & 0xffL, tc = idx & 0xffL;
			s32 lvl = (s32)Gfx_BlobFalloff(radius,
			              (s32)Gfx_PolarDist(
			                  tc < 0x80L ? tc : tc - 0x100L,
			                  tr < 0x80L ? tr : tr - 0x100L));
			u32 add = colour[lvl];
			u8 *p = row + (size_t)x * PX_BYTES;
			s32 v;

			v = (s32)p[PX_R] + (s32)(add >> 24);
			p[PX_R] = (u8)(v > 0xffL ? 0xffL : v);
			v = (s32)p[PX_G] + (s32)((add >> 16) & 0xffUL);
			p[PX_G] = (u8)(v > 0xffL ? 0xffL : v);
			v = (s32)p[PX_B] + (s32)(add & 0xffUL);
			p[PX_B] = (u8)(v > 0xffL ? 0xffL : v);
			p[PX_LIGHT] = (u8)(((s32)p[PX_LIGHT] * lvl) >> 8);
			idx = (idx + 1L) & 0xffffL;
		}
	}
}

/* ORIGIN 0x0036eae8 Temple_ProjectGlowList - projects BOTH entries
 * every frame whatever the draw count is, so the two jitter rolls
 * are in the random stream either way. */
static void project_glow_list(void)
{
	s32 i;

	for (i = 0; i < TE_GLOW_N; i++) {
		s32 z = (i == 0L) ? TE_GLOW_Z0 : g_beamZ;
		s32 r = (i == 0L) ? g_glowR : TE_GLOW_R1;
		s32 sx, sy, sr;

		r += te_rand(0x32L);
		if (project_point(TE_GLOW_X, TE_GLOW_Y, z, r, &sx, &sy, &sr)) {
			g_glowSR[i] = -1L;
			continue;
		}
		if (sr > 0x7fL) sr = 0x7fL;
		g_glowSX[i] = sx; g_glowSY[i] = sy; g_glowSR[i] = sr;
	}
}

/* ORIGIN 0x0036e9cc, and 0x0036eaba inside it. The PRESENT buffer
 * stands in as the draw target, which is why the glow is the one
 * thing the blur does not smear.
 * [open] the demo also modulates LIGHT by the falloff level through
 * the table at (0x003746a8); this only adds the colour. */
static void draw_glow_list(s32 count)
{
	s32 i;

	project_glow_list();
	for (i = 0; i < count && i < TE_GLOW_N; i++) {
		if (g_glowSR[i] < 0L) continue;
		temple_glow_blob(g_glowSX[i], g_glowSY[i], g_glowSR[i],
		                 g_glowcol, 0);
	}
}

/* ---- the temple's lights ------------------------------------------
 *
 * 0x0036ebfe is a count and 28 entries of {x, y, z, phase}: four
 * rows of 4, 6, 8 and 10 down ONE face of the pyramid, each row 20
 * behind the one above, so the pulse travels down the face. They
 * are drawn with the disc blob and the SAME ramp as the orb's glow.
 */

#define TE_LIGHT_TBL   0x0036ebfeUL
#define TE_LIGHT_STEPS 128L
#define TE_LIGHT_MAX   0x226L

static s32 g_lightClock;            /* 0x0036eb3a */
static s32 g_lightCurve[TE_LIGHT_STEPS];

/* OURS - the image is big-endian and this reads a longword out
 * of it. */
static u32 te_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16)
	     | ((u32)p[2] << 8)  | (u32)p[3];
}

/* ORIGIN 0x0036d312 - 64 steps rising by 4 from 0, then 64 falling
 * by 8 from 0x100 and HELD at 0 once they get there, so a light
 * brightens slowly and goes out in half the time. */
static void build_light_curve(void)
{
	s32 i, v;

	for (i = 0; i < 64L; i++) g_lightCurve[i] = i * 4L;
	v = 0x100L;
	for (i = 64L; i < TE_LIGHT_STEPS; i++) {
		g_lightCurve[i] = v;
		v -= 8L;
		if (v < 0L) v = 0L;
	}
}

/* ORIGIN 0x0036eb3e Temple_ProjectLights and 0x0036ea84, its draw.
 * The clock drives every light through the one curve, each offset
 * by its own phase, and the draw STOPS once the clock tops out -
 * the lights do not stay lit, they finish. */
static void draw_lights(void)
{
	const u8 *t = Image_At(TE_LIGHT_TBL);
	s32 n, i;

	if (!t || g_lightClock >= TE_LIGHT_MAX) return;
	n = (s32)te_be32(t);
	for (i = 0; i < n; i++) {
		const u8 *e = t + 4 + i * 16;
		s32 ph = (s32)te_be32(e + 12);
		s32 r = g_lightCurve[(ph + (g_lightClock >> 2)) & 0x7fL] >> 1;
		s32 sx, sy, sr;

		if (project_point((s32)te_be32(e), (s32)te_be32(e + 4),
		                  (s32)te_be32(e + 8), r, &sx, &sy, &sr)) {
			continue;
		}
		if (sr > 0x7fL) sr = 0x7fL;
		temple_glow_blob(sx, sy, sr, g_glowcol, 0);
	}
}

/* ORIGIN 0x0036b8ae Temple_TexAddSat - adds an intensity to a
 * packed texel, saturating each channel at 0x3f. ROL.L #8 ROTATES
 * THE TOP BYTE DOWN, so after the first add the low byte is byte 0,
 * not byte 2: B takes twice the intensity, R and G take it once and
 * LIGHT is the one left alone. */
static void tex_add_sat(u8 *p, s32 v)
{
	s32 t;

	t = (s32)p[PX_B] + v * 2L; p[PX_B] = (u8)(t > 0x3fL ? 0x3fL : t);
	t = (s32)p[PX_R] + v;      p[PX_R] = (u8)(t > 0x3fL ? 0x3fL : t);
	t = (s32)p[PX_G] + v;      p[PX_G] = (u8)(t > 0x3fL ? 0x3fL : t);
}

/* ORIGIN 0x0036d3f0 - a 64 by 64 glow painted into one tile of a
 * 256-wide band, 0x1f4 over the distance from the patch's centre.
 * The band's row stride is 0x400, which is why a 64-texel run is
 * followed by a 0x300 skip. */
static void bake_tex_glow(u8 *p)
{
	s32 r, c;

	for (r = -0x1eL; r < 0x22L; r++) {
		for (c = -0x20L; c < 0x20L; c++) {
			s32 d = Gfx_Isqrt(c * c + r * r);
			s32 v = 0x1f4L;

			if (d != 0L) v = 0x1f4L / d;
			if (v > 0xffL) v = 0xffL;
			tex_add_sat(p, v >> 2);
			p += PX_BYTES;
		}
		p += 0x300L;
	}
}

/* ORIGIN 0x0036d3ce - a flat 8 added to the whole texel longword
 * over the same patch, with NO saturation: the demo lets it run
 * past 0x3f. */
static void brighten_tex(u8 *p)
{
	s32 r, c;

	for (r = 0L; r < 64L; r++) {
		for (c = 0L; c < 64L; c++) {
			u32 v = (((u32)p[0] << 24) | ((u32)p[1] << 16)
			      |  ((u32)p[2] << 8)  | (u32)p[3]) + 8UL;

			p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
			p[2] = (u8)(v >> 8);  p[3] = (u8)v;
			p += PX_BYTES;
		}
		p += 0x300L;
	}
}

/* ORIGIN 0x0036d340 Temple_LightTextures - THE TEMPLE'S OTHER
 * LIGHTS, and the ones that actually cover it. Temple_RunShot1
 * calls it ONCE as mode 4 begins, and they are never drawn: four
 * glows go into the wall atlas's first band, one per tile, so they
 * repeat wherever those tiles are laid and stay lit. The 28 of
 * g_TempleLightTable are a display laid over this base set.
 * temple.txt 6. */
static void light_textures(void)
{
	s32 i;

	for (i = 0L; i < 4L; i++) bake_tex_glow(g_texW + i * 0x100L);
	for (i = 0L; i < 4L; i++) brighten_tex(g_texG + i * 0x100L);
	for (i = 0L; i < 4L; i++)
		brighten_tex(g_texG + 0x30000L + i * 0x100L);
	brighten_tex(g_texG + 0x40000L);
}

/* ORIGIN 0x0036e9ec Temple_DrawGlowToPresent - the glow list and
 * then ONE MORE blob at a point that never moves, drawn as a column
 * rather than a disc. This is the beam. */
static void draw_beam_column(void)
{
	s32 sx, sy, sr;

	if (project_point(TE_GLOW_X, TE_GLOW_Y, TE_BEAM_Z, TE_BEAM_R,
	                  &sx, &sy, &sr)) return;
	if (sr > 0x7fL) sr = 0x7fL;
	temple_glow_blob(sx, sy, sr, g_glowcol, 1);
}

/* ORIGIN 0x00372824 Temple_DissolveGroundTex - the ship's planking
 * arriving. Once the clock passes 0xd48 a counter ramps to 0xff and
 * each texel of a 64 by 64 image is written the frame the counter
 * reaches ITS OWN threshold, so the picture appears a speckle at a
 * time rather than all at once. The destination is a fixed tile of
 * the ground atlas, so it lands wherever that tile is laid.
 * [open] a replay does not put the tile back. temple.txt 6. */
static void dissolve_ground_tex(void)
{
	const u8 *thr = Image_At(TE_DISS_THRESH);
	const u8 *pal = Image_At(TE_DISS_PAL);
	const u8 *idx = Image_At(TE_DISS_IDX);
	u8 *d;
	s32 r, c;

	if (!thr || !pal || !idx) return;
	if (g_camTime <= TE_DISS_AT || g_dissolve >= 0xffL) return;
	g_dissolve++;
	d = g_texG + TE_DISS_DST;
	for (r = 0; r < TE_TILE; r++) {
		for (c = 0; c < TE_TILE; c++) {
			if (g_dissolve >= (s32)*thr) {
				u32 v = RD_U32(pal + (size_t)(*idx) * 4L);

				v = (v & 0xfcfcfcfcUL) >> 2;
				d[0] = (u8)((v >> 16) & 0xff);
				d[1] = (u8)((v >> 8) & 0xff);
				d[2] = 0;
				d[3] = (u8)(v & 0xff);
			}
			thr++;
			idx++;
			d += PX_BYTES;
		}
		d += 0x300L;
	}
}

/* ORIGIN 0x0036e58a and 0x0036e638 - the wobble modes 5 and 6 put
 * on the camera. It lands on g_TempleHeading and g_TempleHorizon,
 * the LATCHED pair the draw reads, so it shakes the view without
 * disturbing the track the next frame interpolates from. */
static void shake_camera(void)
{
	g_heading += ((te_rand(0xaL) - 5L) * g_shakeAmp) >> 8;
	g_horizon += ((te_rand(6L) - 3L) * g_shakeAmp) >> 8;
}

/* ORIGIN 0x00372a7a Temple_StepGlowRadius and 0x00372a64
 * Temple_StepBeamHeight - one ramp a phase, each kept by the phases
 * after it. */
static void step_glow(s32 phase)
{
	if (phase >= 3L && g_glowR < TE_GLOW_R0) g_glowR += 0xaL;
	if (phase >= 4L && g_beamZ < TE_GLOW_Z1) g_beamZ += 0x14L;
	if (phase == 5L && g_lightClock < TE_LIGHT_MAX) g_lightClock++;
	if (phase == 5L && g_camTime >= 0x9c4L && g_shakeAmp < 0xffL)
		g_shakeAmp++;
	if (phase == 6L) {
		apply_detail_heights();
		dissolve_ground_tex();
		g_shakeAmp = 0xffL;
	}
}

/* ---- the beam phase ---------------------------------------------
 *
 * Temple_RunShot1's mode-4 and mode-5 phases: the orb over the
 * pyramid firing a beam down it with sparks flying off. The draws
 * are 0x0036e518 and 0x0036e58a, which share every pass but their
 * last; mode 5 adds a camera shake. [open] the two glow passes,
 * 0x0036e9ec and 0x0036ea34. docs/effects/temple.txt.
 */

#define TE_SPARKS 100L

/* The demo's entry is 0x14 bytes in the order these fields are in. */
typedef struct {
	s32 x, y;           /* +0x00, +0x04   24.8 screen position */
	s32 vx, vy;         /* +0x08, +0x0c                        */
	s32 life;           /* +0x10                               */
} Te_Spark;

static Te_Spark g_spark[TE_SPARKS];

/* ORIGIN 0x0036d256 Temple_SeedSparks - CALLED ONCE FROM THE ASSET
 * LOAD. Life counts down only inside the draw and only modes 4 and
 * 5 draw, so the burst sits frozen until the beam phase. Seeding it
 * on entry would give every speck a different velocity. */
static void seed_sparks(void)
{
	s32 n;

	for (n = 0; n < TE_SPARKS; n++) {
		Te_Spark *q = &g_spark[n];

		q->x    = 0x5000L;
		q->y    = 0x3200L;
		q->life = te_rand(0x28L) + 0x14L;
		q->vx   = te_rand(0x3e8L) - 0x1f4L;
		q->vy   = te_rand(0x1f4L) - 0x190L;
	}
}

/* ORIGIN 0x0036e6da Temple_DrawSparks - one white pixel each, moved
 * under a flat gravity. THE LIFE TEST GUARDS THE MOVE AS WELL AS THE
 * PLOT, so a dead speck freezes where it died rather than drifting
 * on, and a negative coordinate is rejected before the shift - a
 * speck off the left or top is gone, not wrapped. */
static void draw_sparks(void)
{
	s32 n;

	for (n = 0; n < TE_SPARKS; n++) {
		Te_Spark *q = &g_spark[n];
		s32 px, py;
		u8 *p;

		if (q->life == 0L) continue;
		q->life -= 1L;
		if (q->x < 0L || q->y < 0L) continue;
		px = q->x >> 8;
		py = q->y >> 8;
		if (px >= TE_W || py >= TE_H - 1L) continue;
		p = g_frame + (size_t)(py * TE_W + px) * PX_BYTES;
		p[PX_R] = 0x3fU; p[PX_G] = 0x3fU;
		p[PX_LIGHT] = 0U; p[PX_B] = 0x3fU;
		q->x  += q->vx;
		q->y  += q->vy;
		q->vy += 0x10L;
	}
}

/* ORIGIN 0x0036e28c Temple_BlurFeedback - blur_frame's five taps and
 * then ONE MORE AVERAGE against what the destination already held,
 * which is what leaves a trail behind a speck instead of clearing it.
 * The original halves four channels at once by masking 0xfefefefe
 * first; per byte here, which costs the same bottom bit. */
static void blur_feedback(void)
{
	s32 x, y, k;

	for (y = 0; y < TE_H; y++) {
		for (x = 0; x < TE_W; x++) {
			size_t i = (size_t)(y * TE_W + x) * PX_BYTES;
			const u8 *c = g_frame + i;
			const u8 *up = (y > 0) ? c - TE_ROWBYTES : c;
			const u8 *dn = (y < TE_H - 1) ? c + TE_ROWBYTES : c;
			const u8 *lf = (x > 0) ? c - PX_BYTES : c;
			const u8 *rt = (x < TE_W - 1) ? c + PX_BYTES : c;
			u8 *d = g_prev + i;

			for (k = 0; k < PX_BYTES; k++) {
				s32 v = ((s32)up[k] + dn[k]) >> 1;

				v = (v + (((s32)lf[k] + rt[k]) >> 1)) >> 1;
				v = (v + c[k]) >> 1;
				v = (v + (d[k] >> 1)) >> 1;
				d[k] = (u8)v;
			}
		}
	}
}

/* OURS. The phase has no transcribed cue to start it, so the viewer
 * selects it - as Logo_SetVariant does for the logo. */
const u8 *Temple_PresentBuffer(void)
{
	return g_prev;
}

/* ---- shot 2, the cut the temple scene ends on --------------------
 *
 * ORIGIN 0x0036b942 Temple_RunShot2. It shares nothing with the
 * shots before it: no ray-marched terrain, no spline track. The work
 * buffer is filled flat and POLYGON OBJECTS are drawn over it in two
 * ranges, and the camera is explicit and ACCELERATES.
 * [open] the rocks between the two ranges, and the second loop at
 * 0x0036ba86. temple.txt 9.1.
 */

#define TE_S2_FILL   0x2f00UL   /* the flat background            */
#define TE_S2_FILL_B 0x1f00UL   /* the tail phase darkens it      */
#define TE_S2_FRAMES 0x1c2L     /* before the cue ends the shot   */
#define TE_S2_DONE   0x6b1L     /* the dolly's first-loop limit   */

static s32 g_s2_x, g_s2_z;          /* 0x00372b84, 0x00372b88     */
static s32 g_s2_dolly, g_s2_dollyV; /* 0x00372b8c, 0x00372b3c     */
static s32 g_s2_ang, g_s2_angV;     /* 0x00372ba4, 0x00372b40     */
static s32 g_s2_pitch;              /* 0x00372ba0                 */
static s32 g_s2_roll;               /* 0x00372b74                 */
static s32 g_s2_count;              /* 0x00372b36                 */

#define TE_OVL_PAL   0x003cf900UL  /* 256 longs                  */
#define TE_OVL_IDX   0x003cfd00UL  /* 160 x 144 bytes             */
#define TE_OVL_H     144L
#define TE_OVL_TOP   20L           /* 0x3200 / 0x280, rows above  */
#define TE_OVL_ROWS  0x62L         /* the dbf counts this plus one */

static u8 *g_ovl;                  /* (0x00374688)                */

/* ORIGIN 0x0036cd66 - the overlay is decoded once from a palette
 * and an index plane baked into CODE_06. A palette long is
 * 0x00RRGGBB and the shuffle keeps bytes 1, 2 and 3, masks the low
 * two bits off each and shifts, so a channel lands 6-bit; LIGHT is
 * set flat to 0x10. INDEX 0 STAYS A ZERO LONGWORD, which is what
 * the blit tests for transparency - so palette entry 0 is never
 * reachable and a zero pixel cannot be black. */
static void overlay_decode(void)
{
	const u8 *pal = Image_At(TE_OVL_PAL);
	const u8 *idx = Image_At(TE_OVL_IDX);
	s32 i;

	if (g_ovl || !pal || !idx) return;
	g_ovl = (u8 *)malloc((size_t)(TE_W * TE_OVL_H) * PX_BYTES);
	if (!g_ovl) return;
	for (i = 0; i < TE_W * TE_OVL_H; i++) {
		u8 *q = g_ovl + (size_t)i * PX_BYTES;
		s32 n = idx[i];

		if (!n) {
			q[0] = 0; q[1] = 0; q[2] = 0; q[3] = 0;
			continue;
		}
		q[0] = (u8)(pal[n * 4L + 1L] >> 2);
		q[1] = (u8)(pal[n * 4L + 2L] >> 2);
		q[2] = 0x10;
		q[3] = (u8)(pal[n * 4L + 3L] >> 2);
	}
}

/* ORIGIN 0x0036bed2 - THE FOREGROUND TEMPLE IS A BAKED IMAGE, not
 * geometry: 160 longwords a row laid straight over the finished
 * objects, a zero longword meaning leave what is there. The camera
 * tilt scrolls it - up to TE_OVL_TOP rows higher in the source, and
 * past that the whole strip is pushed down the screen and loses a
 * row of its count for each row it drops. temple.txt 9.2. */
static void overlay_blit(Relic_Frame *f, s32 tilt)
{
	s32 rows = TE_OVL_ROWS, top = 0L, x, y;

	if (!g_ovl) return;
	tilt = -(tilt >> 2);
	if (tilt >= TE_OVL_TOP) {
		top = tilt - TE_OVL_TOP;
		rows -= top;
		if (rows < 0L) return;
		tilt = TE_OVL_TOP;
	}
	if (tilt < 0L) tilt = 0L;
	for (y = 0L; y <= rows; y++) {
		const u8 *sp = g_ovl + (size_t)((TE_OVL_TOP - tilt + y)
		                                * TE_W) * PX_BYTES;
		u8 *d = f->px + (size_t)((top + y) * TE_W) * PX_BYTES;

		for (x = 0L; x < TE_W; x++, sp += PX_BYTES, d += PX_BYTES)
			if (sp[0] | sp[1] | sp[2] | sp[3]) {
				d[0] = sp[0]; d[1] = sp[1];
				d[2] = sp[2]; d[3] = sp[3];
			}
	}
}

#define TE_ROCKS     500L
#define TE_ROCK_PAL  0x003d5700UL
#define TE_ROCK_IDX  0x003d5b00UL
#define TE_ROCK_BANKW 32L         /* the 0x80 row stride, in pixels */
#define TE_ROCK_BANKH 24L
#define TE_ROCK_W    6L           /* the dbf on 5                   */
#define TE_ROCK_H    5L           /* the dbf on 4                   */
#define TE_ROCK_XMAX 0x9cL
#define TE_ROCK_YCLIP 0x5dL
#define TE_SPECK     0x2615100cUL /* the one longword a young rock is */

/* ORIGIN 0x0036bb1c - four rock types of four frames, as offsets in
 * LONGWORDS into the bank. The groups are 192 apart, which is six
 * rows of 32. */
static const s32 TE_ROCK_FRAME[16] = {
	0,   5,   11,  17,
	192, 197, 203, 209,
	384, 389, 395, 401,
	576, 581, 587, 593
};

typedef struct {
	s32 state;   /* +0    0 dormant, 1 armed, 2 falling  */
	s32 age;     /* +2                                   */
	s32 x;       /* +4    fixed point, >> 4 for the pixel */
	s32 y;       /* +6                                   */
	s32 vx;      /* +8                                   */
	s32 vy;      /* +0xa                                 */
	s32 frame;   /* +0xc  this rock's base frame          */
} Te_Rock;

static Te_Rock g_rock[TE_ROCKS];   /* 0x003409d0, 14 bytes each */
static u8 *g_rockBank;             /* g_pTempleRockBank          */

/* ORIGIN 0x0036cd9e Temple_DecodeRockSprites - the same decode as
 * the overlay's on the block next door, but LIGHT lands 0x18. */
static void rocks_decode(void)
{
	const u8 *pal = Image_At(TE_ROCK_PAL);
	const u8 *idx = Image_At(TE_ROCK_IDX);
	s32 i;

	if (g_rockBank || !pal || !idx) return;
	g_rockBank = (u8 *)malloc((size_t)(TE_ROCK_BANKW * TE_ROCK_BANKH)
	                          * PX_BYTES);
	if (!g_rockBank) return;
	for (i = 0; i < TE_ROCK_BANKW * TE_ROCK_BANKH; i++) {
		u8 *q = g_rockBank + (size_t)i * PX_BYTES;
		s32 n = idx[i];

		if (!n) {
			q[0] = 0; q[1] = 0; q[2] = 0; q[3] = 0;
			continue;
		}
		q[0] = (u8)(pal[n * 4L + 1L] >> 2);
		q[1] = (u8)(pal[n * 4L + 2L] >> 2);
		q[2] = 0x18;
		q[3] = (u8)(pal[n * 4L + 3L] >> 2);
	}
}

/* ORIGIN 0x0036bc6a Temple_SeedRocks - all five hundred at once, and
 * ONLY THE FIRST 99 GET A SPRITE: the rest keep frame 0, which the
 * step reads as negative and draws as a single speck. */
static void rocks_seed(void)
{
	s32 i;

	for (i = 0; i < TE_ROCKS; i++) {
		Te_Rock *r = &g_rock[i];

		r->state = 1L;
		r->age   = 0L;
		r->x     = te_rand(0x8cL) << 4;
		r->y     = te_rand(5L) - 5L;
		r->vx    = 0L;
		r->vy    = 0L;
		r->frame = 0L;
		if (i < 0x63L)
			r->frame = (te_rand(4L) << 2) + 1L;
	}
}

/* ORIGIN 0x0036bbf6 - gravity, and a one-in-0x78 bounce that costs
 * three quarters of the fall speed and picks a new drift. */
static void rocks_move(void)
{
	s32 i;

	for (i = 0; i < TE_ROCKS; i++) {
		Te_Rock *r = &g_rock[i];

		if (r->state != 2L) continue;
		r->x += r->vx;
		r->y += r->vy >> 5;
		r->vy += 1L;
		r->age += 1L;
		if (r->y >= 0x64L) continue;
		if (te_rand(0x78L) != 0L) continue;
		if (r->vy <= 0L) continue;
		r->vy = -(r->vy >> 2);
		r->vx = te_rand(0xaL) - 5L;
	}
}

/* ORIGIN 0x0036bd40 - a rock too young for a sprite is one pixel of
 * one colour. */
static void rock_speck(Relic_Frame *f, s32 px, s32 y)
{
	u8 *d;

	if (px < 0L || px >= TE_ROCK_XMAX) return;
	if (y < 0L || y >= 0x63L) return;
	d = f->px + (size_t)(y * TE_W + px) * PX_BYTES;
	d[0] = (u8)((TE_SPECK >> 24) & 0xff);
	d[1] = (u8)((TE_SPECK >> 16) & 0xff);
	d[2] = (u8)((TE_SPECK >> 8) & 0xff);
	d[3] = (u8)(TE_SPECK & 0xff);
}

/* ORIGIN 0x0036bcd2 Temple_DrawRockSprite - six longwords by five
 * rows out of the bank, a zero longword meaning leave what is there.
 * The bottom clip drops a row per row past TE_ROCK_YCLIP; there is
 * no clip on the right, so a sprite at the last column the x bound
 * allows runs one pixel into the row below. */
static void rock_sprite(Relic_Frame *f, s32 frame, s32 px, s32 y)
{
	s32 rows = TE_ROCK_H - 1L, row, c, at;

	if (!g_rockBank) return;
	if (px < 0L || px >= TE_ROCK_XMAX) return;
	if (y < 0L) return;
	if (y >= TE_ROCK_YCLIP) {
		rows -= y - TE_ROCK_YCLIP;
		if (rows < 0L) return;
	}
	if (frame < 0L || frame >= 16L) return;
	at = TE_ROCK_FRAME[frame];
	for (row = 0; row <= rows; row++) {
		const u8 *sp = g_rockBank
		             + (size_t)(at + row * TE_ROCK_BANKW) * PX_BYTES;
		u8 *d = f->px + (size_t)((y + row) * TE_W + px) * PX_BYTES;

		for (c = 0; c < TE_ROCK_W; c++, sp += PX_BYTES, d += PX_BYTES)
			if (sp[0] | sp[1] | sp[2] | sp[3]) {
				d[0] = sp[0]; d[1] = sp[1];
				d[2] = sp[2]; d[3] = sp[3];
			}
	}
}

/* OURS - the screen y every rock is measured against: THE PROJECTED
 * Y OF OBJECT 0'S VERTEX 30, so the fall is read against the rising
 * temple and not against the screen. ORIGIN 0x0036bb82, the
 * (projected + 0xf0) the step indexes. */
static s32 rocks_base(void)
{
	s32 sx, sy, d;
	const s32 *vp;

	if (g_objNV[0] <= 30L) return 0L;
	vp = g_objVert + (g_objAt[0] + 30L) * 3L;
	if (!obj_project(vp, &sx, &sy, &d)) return 0L;
	return sy;
}

/* ORIGIN 0x0036bb62 Temple_StepRocks, the arming half. EVERY LIVE
 * ROCK ROLLS THE RNG ONCE, which is where most of a shot 2 frame's
 * seven hundred rolls go - temple.txt 8.2.
 *
 * Split from the draw only to keep the two readable: the demo
 * interleaves them in one loop, and since the draw rolls nothing the
 * stream is the same either way. The DRAW RUNS FIRST so a rock armed
 * this frame is not drawn until the next, which is the order the
 * demo's single loop produces. */
static void rocks_arm(void)
{
	s32 i, base = rocks_base();

	for (i = 0; i < TE_ROCKS; i++) {
		Te_Rock *r = &g_rock[i];

		if (!r->state) continue;
		if (te_rand(r->y + base - 0x64L) == 0L) r->state = 2L;
	}
}

/* ORIGIN 0x0036bb62 Temple_StepRocks, the drawing half. */
static void rocks_draw(Relic_Frame *f)
{
	s32 i, base = rocks_base();

	for (i = 0; i < TE_ROCKS; i++) {
		Te_Rock *r = &g_rock[i];
		s32 fr = r->frame - 4L;
		s32 y;

		if (r->state != 2L) continue;
		y = r->y + base - 0x64L;
		if (fr < 0L)
			rock_speck(f, r->x >> 4, y);
		else
			rock_sprite(f, ((r->age >> 1) & 3L) + fr,
			            r->x >> 4, y);
	}
}

/* ORIGIN 0x003e12b8 - the two type-4 glows' world points and the
 * eight-entry radius table they share, all baked in CODE_06. The
 * radii run 0x960 0x8c0 0x7d0 0x8c0 0x960 0xa00 0xaf0 0xa00, which
 * is the pulse. */


static s16 g_noise[256];           /* (0x0037471c) */

/* ORIGIN 0x00368118 Temple_SeedNoiseTables - twenty bumps scattered
 * into a 256-entry word table, each a position and a height rolled
 * from Temple_Rand and then smeared over as many as five
 * neighbours as the height steps down by 3 and 2. Every index is a
 * BYTE operation, so the walk wraps inside the table.
 *
 * The demo builds two of these and this is the one the glow reads.
 * It fills an allocation it does not clear first; ours starts
 * zeroed. */
static void build_noise(void)
{
	s32 i;

	for (i = 0; i < 256L; i++) g_noise[i] = 0;
	for (i = 0; i < 20L; i++) {
		s32 at = te_rand(0x100L) & 0xffL;
		s32 v  = te_rand(8L) & 0xffL;

		g_noise[at] = (s16)v;
		at = (at + 1L) & 0xffL;
		v -= 3L;
		if (v < 0L) continue;
		g_noise[at] = (s16)v;
		at = (at - 2L) & 0xffL;
		g_noise[at] = (s16)v;
		v -= 2L;
		if (v < 0L) continue;
		at = (at - 1L) & 0xffL;
		g_noise[at] = (s16)v;
		at = (at + 4L) & 0xffL;
		g_noise[at] = (s16)v;
	}
}

/* ORIGIN 0x0036b498 Temple_DrawGlowBlob - the big pulsing light,
 * ADDED into the work buffer as a packed longword.
 *
 * It is the falloff disc the beam glow uses, roughened: the polar
 * table gives a distance AND an angle per pixel, the distance picks
 * the falloff level, and then the ANGLE INDEXES THE NOISE TABLE
 * TWICE, the second time a further phase on, both subtracted. That
 * is what gives the light its ragged edge rather than a clean disc.
 *
 * THE PHASE IS THE BLOB'S OWN X. The demo stores cx to 0x00372bba
 * and then reads the LOW BYTE of that word back as the step, so the
 * noise rotates as the light crosses the screen.
 *
 * DIVERGENCE: a level driven negative by the two subtractions
 * indexes the ramp from before its base in the demo. This clamps at
 * zero instead - there is nothing defensible to read there. */
static void glow_blob_add(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                          const u32 *colour)
{
	s32 phase = cx & 0xffL;
	s32 y, y0, y1;

	if (radius <= 0L) return;
	if (radius + cy <= 0L || cy - radius >= TE_H) return;
	if (radius + cx <= 0L || cx - radius >= TE_W) return;

	y0 = cy - radius; if (y0 < 0L) y0 = 0L;
	y1 = cy + radius; if (y1 > TE_H - 1L) y1 = TE_H - 1L;
	for (y = y0; y <= y1; y++) {
		s32 dy = y - cy;
		s32 half = Gfx_Isqrt(radius * radius - dy * dy);
		s32 x0 = cx - half, x1 = cx + half, x, idx;

		if (x0 < 0L) x0 = 0L;
		if (x1 > TE_W - 1L) x1 = TE_W - 1L;
		idx = ((dy & 0xffL) << 8) | ((x0 - cx) & 0xffL);
		for (x = x0; x <= x1; x++) {
			s32 tr = (idx >> 8) & 0xffL, tc = idx & 0xffL;
			s32 sdx = tc < 0x80L ? tc : tc - 0x100L;
			s32 sdy = tr < 0x80L ? tr : tr - 0x100L;
			s32 lvl = (s32)Gfx_BlobFalloff(radius,
			              (s32)Gfx_PolarDist(sdx, sdy));
			s32 a = (s32)Gfx_PolarAngle(sdx, sdy);
			u8 *q = f->px + (size_t)(y * TE_W + x) * PX_BYTES;
			u32 v;

			a = (a + phase) & 0xffL;
			lvl -= g_noise[a];
			a = (a + phase) & 0xffL;
			lvl -= g_noise[a];
			if (lvl < 0L) lvl = 0L;
			v = ((u32)q[0] << 24) | ((u32)q[1] << 16)
			  | ((u32)q[2] << 8) | (u32)q[3];
			v += colour[lvl];
			q[0] = (u8)(v >> 24);
			q[1] = (u8)(v >> 16);
			q[2] = (u8)(v >> 8);
			q[3] = (u8)v;
			idx = (idx + 1L) & 0xffffL;
		}
	}
}

/* ORIGIN 0x003686d8 and 0x00368530 - the type-4 branch of
 * Temple_DrawObjects and of Temple_DrawObjectRange, which SIZE THE
 * LIGHT DIFFERENTLY. temple.txt 11.8. */
static void draw_glow_object(Relic_Frame *f, u32 posAddr, int late)
{
	const u8 *pp = Image_At(posAddr);
	const u8 *rt = Image_At(g_set->glow_rad);
	s32 v[3], sx, sy, d, r, c;

	if (!pp || !rt) return;
	v[0] = (s32)RD_U32(pp);
	v[1] = (s32)RD_U32(pp + 4L);
	v[2] = (s32)RD_U32(pp + 8L);
	if (!obj_project_m(v, &sx, &sy, &d, late)) return;
	if (d <= TE_GLOW4_CLIP) return;
	c = g_set->glow_style ? g_setGlowTick
	                      : (late ? g_tick_pulse : g_tick);
	r = (s32)RD_U32(rt + (size_t)(((c >> 1) & 7L) * 4L));
	if (g_set->glow_style) {
		/* ORIGIN 0x002d2b08 - the same shift and cap the rise uses,
		 * and a teardrop instead of a colour blob. */
		r = (r << 10) / d;
		if (r > 0x7fL) r = 0x7fL;
		Gfx_GlowOval(f, sx, sy, r);
		return;
	}
	if (late) {
		r = (r << 10) / d;
		if (r > 0x7fL) r = 0x7fL;
	} else {
		r = (r << 8) / d;
		if (r > 0x64L) r = 0x64L;
		r += te_rand(0x11L);
	}
	glow_blob_add(f, sx, sy, r, late ? g_glowcol : g_glowcol4);
}

#define TE_PUFFS    20L
#define TE_PUFF_NUM 0x1010000L   /* D7 at 0x0036cd42 */
#define TE_PUFF_DEN 0x12cL       /* D3 at 0x0036cd48 */

typedef struct {
	s32 r;    /* +0  radius, and the slot is free once it is spent */
	s32 x;    /* +2                                                */
	s32 y;    /* +4                                                */
	s32 vy;   /* +6                                                */
} Te_Puff;

static Te_Puff g_puff[TE_PUFFS];   /* 0x0036be20, baked all zero */
static u8 g_puffAmt[256];          /* 0x00374250                 */

/* ORIGIN 0x0036cffe, which Temple_InitShot2 runs over 0x00374250 -
 * the same curve as build_glow_colour with 300 for its 200, but one
 * byte out instead of three channels. Entry 0 takes the untouched
 * numerator and so clamps to 0x3f. */
static void build_puff_ramp(void)
{
	s32 i;

	for (i = 0; i < 256L; i++) {
		s32 u = (TE_PUFF_DEN * i) >> 7;
		s32 t = u * u;
		s32 v = TE_PUFF_NUM;

		if (t != 0L) v = (TE_PUFF_NUM - t) / t;
		v >>= 4;
		if (v > 0x3fL) v = 0x3fL;
		g_puffAmt[i] = (u8)v;
	}
}

/* ORIGIN 0x0036cf02 - THE EXHAUST PUFF IS A LIGHT-ONLY GLOW. It
 * walks the same falloff disc as the ship's light and the beam, but
 * where those add a colour this touches BYTE +2 ALONE: the falloff
 * level picks an amount out of g_puffAmt, that is added to the
 * pixel's LIGHT as a BYTE - so it wraps - and the sum indexes
 * g_s3_satramp, which holds the identity to 0x3f and 0x3f above it.
 * R, G and B are never read or written. temple.txt 9.4. */
static void puff_draw(Relic_Frame *f, s32 cx, s32 cy, s32 radius)
{
	s32 y, y0, y1;

	if (radius <= 0L) return;
	if (radius + cy <= 0L || cy - radius >= TE_H) return;
	if (radius + cx <= 0L || cx - radius >= TE_W) return;

	y0 = cy - radius; if (y0 < 0L) y0 = 0L;
	y1 = cy + radius; if (y1 > TE_H - 1L) y1 = TE_H - 1L;
	for (y = y0; y <= y1; y++) {
		s32 dy = y - cy;
		s32 half = Gfx_Isqrt(radius * radius - dy * dy);
		s32 x0 = cx - half, x1 = cx + half, x, idx;

		if (x0 < 0L) x0 = 0L;
		if (x1 > TE_W - 1L) x1 = TE_W - 1L;
		idx = ((dy & 0xffL) << 8) | ((x0 - cx) & 0xffL);
		for (x = x0; x <= x1; x++) {
			s32 tr = (idx >> 8) & 0xffL, tc = idx & 0xffL;
			s32 lvl = (s32)Gfx_BlobFalloff(radius,
			              (s32)Gfx_PolarDist(
			                  tc < 0x80L ? tc : tc - 0x100L,
			                  tr < 0x80L ? tr : tr - 0x100L));
			u8 *q = f->px + (size_t)(y * TE_W + x) * PX_BYTES;

			q[PX_LIGHT] = g_s3_satramp[((s32)q[PX_LIGHT]
			                            + g_puffAmt[lvl]) & 0xffL];
			idx = (idx + 1L) & 0xffffL;
		}
	}
}

/* ORIGIN 0x0036bdc0 Temple_SpawnDustPuff - fills the FIRST free slot
 * and rolls nothing when the field is full, so a saturated field
 * costs the random stream nothing. The y is anchored to the
 * projected screen y of OBJECT 11'S VERTEX 0. */
static void puff_spawn(void)
{
	s32 i, sx, sy, d, base = 0L;

	for (i = 0; i < TE_PUFFS; i++)
		if (g_puff[i].r <= 0L) break;
	if (i >= TE_PUFFS) return;
	if (g_objNV[11] > 0L) {
		const s32 *vp = g_objVert + g_objAt[11] * 3L;

		if (obj_project(vp, &sx, &sy, &d)) base = sy;
	}
	g_puff[i].r  = te_rand(0x28L) + 0x14L;
	g_puff[i].x  = te_rand(0x14L) + 0x5aL;
	g_puff[i].y  = base + 0x1eL;
	g_puff[i].vy = te_rand(0xaL) + 0xaL;
}

/* ORIGIN 0x0036bd72 Temple_StepDustPuffs - draw, then age: the puff
 * drifts DOWN by a speed that decays to nothing while its radius
 * loses 2 a frame, so it slows and shrinks out. Two spawns a frame
 * keep the field topped up. */
static void puffs_step(Relic_Frame *f)
{
	s32 i;

	for (i = 0; i < TE_PUFFS; i++) {
		Te_Puff *q = &g_puff[i];

		if (q->r <= 0L) continue;
		puff_draw(f, q->x, q->y, q->r);
		q->y += q->vy;
		q->vy -= 1L;
		if (q->vy < 0L) q->vy = 0L;
		q->r -= 2L;
	}
	puff_spawn();
	puff_spawn();
}

#define TE_SHADOW_SPR 0x003dafa8UL  /* g_TempleShadowSprite    */
#define TE_SHADOW_W   46L           /* the dbf on 0x2d         */
#define TE_SHADOW_H   60L           /* the dbf on 0x3b         */
#define TE_SHADOW_X0  0x5eL
#define TE_SHADOW_Y0  0x61L
#define TE_GROUND_W   256L          /* the 0x400 row stride    */

static u8  g_shadowSave[TE_SHADOW_W * TE_SHADOW_H * 4];
static s32 g_shadowX, g_shadowY;      /* 0x0036c96a, 0x0036c96e */
static s32 g_shadowPX, g_shadowPY;    /* 0x0036c972, 0x0036c976 */

/* ORIGIN 0x0036c890 Temple_StampGroundShadow - THE SHIP'S SHADOW IS
 * STAMPED INTO THE GROUND TEXTURE, NOT RENDERED. Nothing projects
 * it: it is painted into the 256-wide map the terrain warp samples,
 * so the walker draws it in perspective for free.
 *
 * Last frame's patch is restored from g_shadowSave first; the new
 * one saves the ground as it goes. The sprite byte is a 6-bit
 * darken amount per channel and BYTE +2 IS LEFT ALONE.
 *
 * THE SUBTRACT IS A BYTE SUBTRACT: where the shadow is darker than
 * the ground it wraps past zero into g_s3_ramp's unwritten tail and
 * comes out black, which is what those 192 zero entries are for.
 * temple.txt 11.1. */
static void shadow_stamp(void)
{
	const u8 *spr = Image_At(TE_SHADOW_SPR);
	u8 *save = g_shadowSave;
	s32 row, col, k;

	if (!g_s3_ground || !spr) return;
	if (g_shadowPX >= 0L) {
		const u8 *b = g_shadowSave;

		for (row = 0; row < TE_SHADOW_H; row++) {
			u8 *d = g_s3_ground
			      + (size_t)((g_shadowPY + row) * TE_GROUND_W
			                 + g_shadowPX) * 4L;

			for (col = 0; col < TE_SHADOW_W * 4L; col++)
				*d++ = *b++;
		}
	}
	for (row = 0; row < TE_SHADOW_H; row++) {
		u8 *d = g_s3_ground
		      + (size_t)((g_shadowY + row) * TE_GROUND_W
		                 + g_shadowX) * 4L;
		const u8 *q = spr + (size_t)(row * TE_SHADOW_W) * 4L;

		for (col = 0; col < TE_SHADOW_W; col++, d += 4, q += 4) {
			for (k = 0; k < 4; k++) save[k] = d[k];
			save += 4;
			for (k = 0; k < 3; k++) {
				s32 i = (k == 2) ? PX_B : k;
				s32 amt = (q[i] & 0xfcL) >> 2;

				d[i] = g_s3_ramp[((s32)d[i] - amt) & 0xffL];
			}
		}
	}
	g_shadowPX = g_shadowX;
	g_shadowPY = g_shadowY;
	g_shadowX += 1L;
}

/* ORIGIN 0x0036c174 - Temple_RunShot3's D0 == 1 entry, the orb's
 * descent scene run in reverse. It rebuilds the ground from
 * the ruined map, stages the warp, and parks the shadow at the top
 * left of its run.
 *
 * [open] THE CAMERA DOES NOT MOVE YET. Temple_StepApproach's own
 * stepper and the object range it draws once the ship is far enough
 * out are not transcribed, so this stages the scene and walks the
 * shadow across it and no more. temple.txt 11. */
static void rise_enter(void)
{
	g_rise   = 1;
	g_shot2  = 0;
	build_ground_tex(TE_GROUND_C);
	g_s3_scale = 0x12c0L;
	g_s3_alt   = 0x12c0L;
	g_s3_angle = 0L;
	g_rise_x   = 0xdacL;
	g_rise_z   = -0x222e0L;
	g_rise_y   = -0x5dcL;
	g_rise_cnt = 0x3b6L;
	g_rise_fade = 0L;
	g_rise_flick = 0L;
	g_rise_cut = 0x258L;
	g_rise_stage = 0L;
	g_rise_armed = 1;
	g_rise_phase = 2L;
	g_risePan  = 0L;
	g_riseTilt = 0L;
	g_shadowX  = TE_SHADOW_X0;
	g_shadowY  = TE_SHADOW_Y0;
	g_shadowPX = -1L;
	g_shadowPY = -1L;
}

/* ORIGIN 0x0037233e - the VBL hook the descent and the rise SHARE,
 * switched on the phase word. Phases 2 and 4 are the rise's and are
 * the same but for the camera move - 0x0037222e against 0x0037227a,
 * which differ only in how the roll follows the ground.
 *
 * THE CAMERA COMES IN, IT DOES NOT GO OUT: z climbs 0xb4 a frame
 * and stops at -0xbb8, which is the ship rising toward the lens.
 * The roll TRACKS THE WARP ANGLE rather than being held, so the
 * horizon turns with the ground. */
static void rise_step(void)
{
	g_s3_angle -= 5L;
	g_tick_pulse += 1L;
	g_risePan  = 0L;
	if (g_rise_phase == 3L) {
		/* ORIGIN 0x003722c0 - the underside view LOOKS UP and the
		 * camera leaves, where every stage before it came in. */
		g_riseTilt  = 0x1800L;
		g_rise_roll = g_s3_angle ^ 0x1fffL;
		g_rise_z   -= 0xb4L;
		g_s3_scale -= 0x40L;
		if (g_rise_fade != 0L) g_rise_fade -= 1L;
		if (g_rise_cnt > 0L) g_rise_cnt--;
		return;
	}
	g_riseTilt = 0x800L;
	if (g_rise_phase == 4L)
		g_rise_roll = g_s3_angle ^ 0x1fffL;
	else
		g_rise_roll = (g_s3_angle - 0x200L) & 0x1fffL;
	if (g_rise_z < TE_RISE_NEAR) g_rise_z += 0xb4L;
	g_rise_cut -= 4L;
	g_s3_alt   += 8L;
	g_s3_scale += 0x20L;
	if (g_s3_scale > 0x2800L && g_rise_fade < 0x3fL)
		g_rise_fade += 3L;
	if (g_rise_fade != 0L) g_rise_fade -= 1L;
	if (g_rise_cnt > 0L) g_rise_cnt--;
	/* ORIGIN 0x0036c27e, 0x0036c2ce and 0x0036c322 - the rise
	 * restages behind each whiteout, and THE FIRST OF THEM DOES NOT
	 * SWAP THE GROUND. Folding the first two together jumps the
	 * camera 0x7530 too far and the ship arrives nearly past the
	 * lens. OURS IS THE TRIGGER - temple.txt 11.4 and 11.7 */
	/* WHICH STAGE THE UNRESET LEVEL EATS IS A FUNCTION OF THE
	 * DEMO'S FRAME RATE, and ours is one tick a field throughout,
	 * so the collapse is placed where its rate put it - at the
	 * exit from stage 0. temple.txt 11.2. */
	if (g_rise_fade < 0x3fL) {
		g_rise_armed = 1;
		return;
	}
	if (!g_rise_armed) return;
	/* leaving stage 0 only: the next field fires again */
	g_rise_armed = (g_rise_stage == 0L);
	g_s3_alt = 0x12c0L;
	g_s3_scale = 0x700L;
	g_rise_cut = 0x258L;
	if (g_rise_stage == 0L) {
		g_rise_stage = 1L;
		g_rise_phase = 4L;
		g_rise_z   = -0x1adb0L;
	} else if (g_rise_stage == 1L) {
		g_rise_stage = 2L;
		build_ground_tex(TE_GROUND_A);
		g_rise_z   = -0x13880L;
	} else if (g_rise_stage == 2L) {
		g_rise_stage = 3L;
		g_rise_z   = -0x61a8L;
		g_s3_scale = 0xd00L;
		g_rise_cut = 0x320L;
	} else if (g_rise_stage == 3L) {
		/* ORIGIN 0x0036c36c - the cut to the underside view. */
		s32 i;

		g_rise_stage = 4L;
		g_rise_phase = 3L;
		if (g_s3_tex)
			for (i = 0; i < TE_WARP_BYTES; i++)
				g_s3_tex[i] = (u8)(g_s3_tex[i] >> 1);
		build_ground_tex(TE_GROUND_B);
		g_rise_x   = 0xdacL;
		g_rise_y   = -0x5dcL;
		g_rise_z   = -0x2710L;
		g_s3_alt   = 0x15e0L;
		g_s3_scale = 0x2bc0L;
		g_s3_angle = 0L;
	}
}

/* ORIGIN 0x0036c644 Temple_StepApproach and 0x0036c97a
 * Temple_StepApproachMid - the rise's two frame routines. The latch
 * is the same and copies the dolly straight through where shot 2
 * shifts it down four; the object range covers records 13 and 14,
 * so both type-4 glows belong to it.
 *
 * THE MID ROUTINE DOES NOT STAMP THE SHADOW. It appears in the
 * opening stretch and then stops, and a port that keeps stamping
 * drags the OLD ground's pixels into the new one through the
 * stamp's restore buffer - the backup was taken before the swap.
 *
 * WHAT MID ADDS IS THE CLOUD LAYER, the same warp the descent runs
 * over the same texture, and it gates the ship on -0xbb8 rather
 * than -0xfa0. temple.txt 11.5. */
static void rise_frame(Relic_Frame *f)
{
	Relic_Frame wf;
	s32 i, gate;

	g_objCamX = g_rise_x;
	g_objCamZ = g_rise_z;
	g_objCamY = g_rise_y;
	g_objPan  = g_risePan;
	g_objTilt = g_riseTilt;
	g_objYaw  = g_rise_roll;

	if (g_rise_stage == 4L) {
		/* ORIGIN 0x0036ca1a - the underside view. The ship is drawn
		 * with no distance gate at all, and the cloud lasts only
		 * while the warp scale is over 0x64. */
		for (i = 0; i < TE_PIXELS * PX_BYTES; i++) g_frame[i] = 0;
		wf = *f;
		wf.w = TE_W; wf.h = TE_H;
		wf.scale = FX_ONE; wf.px = g_frame;
		warp_terrain(g_s3_ground, g_s3_angle, g_s3_alt, 0L);
		g_glowsInSort = 1;
		draw_objects_range(&wf, 0L, 12L, 1, TE_OBJMASK_RISE, 0, 0L);
		g_glowsInSort = 0;
		if (g_s3_scale > 0x64L)
			warp_clouds(g_s3_tex, g_s3_angle, g_s3_scale, 0L);
		flash_frame(g_rise_fade + g_rise_flick);
		for (i = 0; i < TE_PIXELS * PX_BYTES; i++)
			g_prev[i] = g_frame[i];
		return;
	}
	if (!g_rise_stage) shadow_stamp();
	for (i = 0; i < TE_PIXELS * PX_BYTES; i++) g_frame[i] = 0;
	wf = *f;
	wf.w = TE_W; wf.h = TE_H; wf.scale = FX_ONE; wf.px = g_frame;
	warp_terrain(g_s3_ground, g_s3_angle, g_s3_alt, 0L);
	gate = g_rise_stage ? TE_RISE_NEAR : TE_RISE_SHIP;
	if (g_rise_z < gate) {
		g_glowsInSort = 1;
		draw_objects_range(&wf, 0L, 12L, 1, TE_OBJMASK_RISE, 0, 0L);
		g_glowsInSort = 0;
	}
	if (g_rise_stage) {
		warp_clouds(g_s3_tex, g_s3_angle, g_s3_scale, 0L);
		/* ORIGIN 0x0036ca00 - record 15, AFTER the cloud, as its
		 * own range. Geometry slot 13 is its mesh: records 13 and
		 * 14 are glows and carry none. temple.txt 11.6. */
		if (g_rise_z < TE_RISE_NEAR)
			draw_objects_range(&wf, 13L, 13L, 1,
			                   TE_OBJMASK_RISE, 1, g_rise_cut);
	}
	flash_frame(g_rise_fade + g_rise_flick);
	for (i = 0; i < TE_PIXELS * PX_BYTES; i++) g_prev[i] = g_frame[i];
}

/* ORIGIN 0x0036b942's setup, the nine values it stages. */
static void shot2_enter(void)
{
	g_shot2    = 1;
	g_s2_x     = -0x1f4L;
	g_s2_z     = -0x1f40L;
	g_s2_dolly = -0xbb80L;
	g_s2_roll  = 0x800L;
	g_s2_dollyV = 0x64L;
	g_s2_ang   = 0L;
	g_s2_angV  = 0L;
	g_s2_pitch = 0L;
	g_s2_count = TE_S2_FRAMES;
	overlay_decode();
	rocks_decode();
	rocks_seed();
	build_puff_ramp();
	shot3_build_ramp();
	/* ORIGIN 0x0036cd26 - shot 2's base, temple.txt 9.6 */
	g_fog[0] = 0x3fL; g_fog[1] = 0x3fL; g_fog[2] = 0x42L;
}

/* ORIGIN 0x003722f2 Temple_StepShot2Camera - once a VBL. The dolly
 * gains a unit of speed every frame and the angle loses one, so both
 * accelerate rather than ramp. */
static void shot2_step(void)
{
	g_s2_dolly += g_s2_dollyV;
	g_s2_dollyV += 1L;
	g_s2_ang += g_s2_angV;
	g_s2_angV -= 1L;
	if (g_s2_count > 0L) g_s2_count--;
	rocks_move();

}

/* ORIGIN 0x0036b9f0 Temple_StepShot2 and 0x0036ba86, its TAIL - the
 * latch, the fill, then the draws. Temple_RunShot2 switches to the
 * tail once the dolly passes TE_S2_DONE, and THE TAIL IS NOT THE
 * SAME FRAME: it fills darker and it REORDERS THE DRAWS, putting
 * the overlay before the rocks and the glow so both land ON TOP of
 * the foreground temple instead of behind it. temple.txt 9.5. */
static void shot2_frame(Relic_Frame *f)
{
	Relic_Frame wf;
	s32 i, fill;
	int tail;

	g_objCamX    = g_s2_x;
	g_objCamZ    = g_s2_z;
	g_objCamY    = g_s2_dolly >> 4;
	g_objYaw  = g_s2_roll;
	g_objPan  = g_s2_pitch;
	g_objTilt = (g_s2_ang >> 8) + te_rand(0xfL) - 0xaL;

	tail = g_s2_dolly > TE_S2_DONE;
	fill = tail ? TE_S2_FILL_B : TE_S2_FILL;
	for (i = 0; i < TE_PIXELS; i++) {
		u8 *q = g_frame + (size_t)i * PX_BYTES;

		q[0] = 0; q[1] = 0;
		q[2] = (u8)((fill >> 8) & 0xff);
		q[3] = (u8)(fill & 0xff);
	}
	wf = *f;
	wf.w = TE_W; wf.h = TE_H; wf.scale = FX_ONE; wf.px = g_frame;
	draw_objects_range(&wf, 0L, 12L, 0, TE_OBJMASK_S2, 0, 0L);
	if (tail) {
		puffs_step(&wf);
		overlay_blit(&wf, g_objTilt);
		rocks_draw(&wf);
		rocks_arm();
		draw_glow_object(&wf, TE_GLOW4_P14, 0);
	} else {
		rocks_draw(&wf);
		rocks_arm();
		draw_glow_object(&wf, TE_GLOW4_P14, 0);
		puffs_step(&wf);
		overlay_blit(&wf, g_objTilt);
	}
	blur_frame();
}

/* OURS, AND FOR MEASUREMENT ONLY - parts.h. The live state a
 * capture can set so a frame renders from the demo's own numbers.
 * Every entry is a global this file already carries; nothing here
 * changes what the port computes, only what it starts from. */
static const struct { const char *name; s32 *at; } TE_PRIME[] = {
	{ "seed",      (s32 *)0 },      /* g_s3_seed, u32 - see below */
	{ "rise_z",    &g_rise_z },
	{ "rise_x",    &g_rise_x },
	{ "rise_y",    &g_rise_y },
	{ "warp_angle", &g_s3_angle },
	{ "warp_alt",  &g_s3_alt },
	{ "warp_scale", &g_s3_scale },
	{ "shadow_x",  &g_shadowX },
	{ "shadow_y",  &g_shadowY },
	{ "pulse",     &g_tick_pulse }
};

/* OURS - the prime hook, parts.h. */
static int te_prime(const char *name, s32 value)
{
	int i;

	if (!strcmp(name, "seed")) {
		g_s3_seed = (u32)value;
		return 1;
	}
	for (i = 1; i < (int)(sizeof TE_PRIME / sizeof TE_PRIME[0]); i++)
		if (!strcmp(name, TE_PRIME[i].name)) {
			*TE_PRIME[i].at = value;
			return 1;
		}
	return 0;
}

/* OURS - the tick the module's last shot draws, after which the
 * screen is blank. */
static s32 te_end_tick(void)
{
	if (g_te_end) return g_te_end;
	return (g_s3_end ? g_s3_end : TE_SHOT3_END) + TE_RISE_LEN;
}

/* OURS - 1 while the blank is up, and THE ONLY PLACE (0x1f,0x28) CAN
 * BE POLLED: every path above this one in te_step returns first.
 * 0x0021f742 puts the default VBL hook back as the screen goes, so
 * nothing steps through the blank either. */
static int te_blank_step(s32 tick)
{
	s32 end;

	if (g_s3_end && !g_te_end && Music_Reached(tick - g_s3_end))
		g_te_end = tick;
	end = te_end_tick();
	if (tick == end)
		Music_Arm(TE_BLANK_ORDER, TE_BLANK_ROW, TE_BLANK);
	if (tick <= end) return 0;
	if (!g_te_blank && Music_Reached(tick - end)) g_te_blank = tick;
	return 1;
}

/* OURS */
static void te_step(s32 tick)
{
	g_tick = tick;
	if (tick <= 1L) Music_Arm(TE_CUE1_ORDER, TE_CUE1_ROW, TE_SHOT1_SPAN);
	if (!g_ready) return;
	if (g_rise) { if (!te_blank_step(tick)) rise_step(); return; }
	if (g_shot2) {
		shot2_step();
		if (g_s2_count == 0L) rise_enter();
		return;
	}
	if (!g_s3_live && !g_orb && !g_s1_end && tick > 0L
	    && Music_Reached(tick)) {
		if (shot3_load_tex()) return;
		g_s1_end = tick;
		shot3_build_ramp();
		shot3_seed_particles();
		build_ground_tex(TE_GROUND_A);
		shot3_enter(0L);
		Music_Arm(TE_CUE3_ORDER, TE_CUE3_ROW, TE_DESCENT_LEN);
	}
	if (g_s3_live && !g_s3_end && Music_Reached(tick - g_s1_end)) {
		g_s3_end = tick;
		g_s3_live = 0;
		g_orb_wait = tick;
		Music_Arm(TE_CUE4_ORDER, TE_CUE4_ROW, TE_RISE_LEN);
	}
	if (g_orb_wait) {
		if (tick < g_orb_wait + TE_ORB_BLANK) return;
		g_orb_wait = 0L;
		orb_enter();
	}
	if (g_s3_end && !g_te_end && Music_Reached(tick - g_s3_end))
		g_te_end = tick;
	if (g_s3_live) {
		shot3_step();
		return;
	}
	if (g_orb) {
		orb_step();
		g_camTime += TE_TIME_STEP;
		while (g_phase > 0 && g_phase < TE_PH_LAST
		       && g_camTime >= TE_PHASE_END[g_phase]) {
			g_phase++;
			if (g_phase == TE_PH_GLOW) {
				g_glowR = 0L;
				g_beamZ = TE_GLOW_Z0;
			}
			if (g_phase == TE_PH_BEAM) light_textures();
			if (g_phase == TE_PH_SHAKE) {
				g_lightClock = 0L;
				g_shakeAmp = 0L;
				g_dissolve = 0L;
			}
			if (g_phase == TE_PH_END) restore_cell_heights();
		}
		/* ORIGIN 0x0036e0b8 - the driver leaves on mode 6's limit */
		if (g_phase == TE_PH_LAST
		    && g_camTime >= TE_PHASE_END[TE_PH_LAST]) {
			g_phase = 0;
			g_orb = 0;
			shot2_enter();
		}
		step_glow((s32)g_phase);
		eval_camera();
		if (g_phase == TE_PH_SHAKE || g_phase == TE_PH_END)
			shake_camera();
		return;
	}
	if (g_tick >= TE_SETUP) {
		g_camTime += TE_TIME_STEP;
		eval_camera();
	}
}

/* OURS. Shot 3 writes the display buffer itself, so its LIGHT byte is
 * the cloud and flash glow and passes straight through; the ruins'
 * LIGHT is a fog distance and goes through fog_pixel instead. */
static void te_frame(Relic_Frame *f, s32 tick)
{
	s32 py;

	Frame_Clear(f);
	if (!g_ready) return;
	if (tick < TE_SETUP && !g_s1_end) return;
	if (tick > te_end_tick() || g_orb_wait) return;
	if (g_rise) rise_frame(f);
	else if (g_shot2) shot2_frame(f);
	else

	{
	if (g_phase != TE_PH_BEAM)
		for (py = 0; py < TE_PIXELS * PX_BYTES; py++) g_frame[py] = 0;
	if (g_s3_live) {
		if (g_s3_phase < 2L)       shot3_step_main(0);
		else if (g_s3_phase == 2L) shot3_step_mid();
		else                       shot3_step_end();
		/* OURS: no Temple_BlurFrame - temple.txt */
		for (py = 0; py < TE_PIXELS * PX_BYTES; py++)
			g_prev[py] = g_frame[py];
	} else if (g_phase == TE_PH_BEAM || g_phase == TE_PH_SHAKE) {
		/* ORIGIN 0x0036e518, 0x0036e58a - temple.txt 6 */
		draw_terrain();
		draw_beam_orb();
		draw_sparks();
		blur_feedback();
		draw_glow_list(2L);
		draw_beam_column();
		if (g_phase == TE_PH_SHAKE) draw_lights();
	} else if (g_phase == TE_PH_GLOW) {
		/* ORIGIN 0x0036e4aa - temple.txt 6 */
		draw_terrain();
		draw_beam_orb();
		blur_feedback();
		draw_glow_list(1L);
	} else if (g_phase == TE_PH_END) {
		/* ORIGIN 0x0036e638 - terrain alone, temple.txt 6 */
		draw_terrain();
		blur_feedback();
	} else if (g_phase) {
		/* ORIGIN 0x0036e440 - [open] temple.txt 6 */
		draw_terrain();
		draw_beam_orb();
		blur_frame();
	} else {
		draw_terrain();
		blur_frame();
	}
	}

	for (py = 0; py < f->h; py++) {
		s32 sy = PX_TO_DEMO(py, f->scale);
		u8 *dst;
		s32 px;

		if (sy < 0 || sy >= TE_H) continue;
		dst = f->px + (size_t)(py * f->w) * PX_BYTES;
		for (px = 0; px < f->w; px++, dst += PX_BYTES) {
			s32 sx = PX_TO_DEMO(px, f->scale);
			const u8 *s;

			if (sx < 0 || sx >= TE_W) continue;
			s = g_prev + (size_t)(sy * TE_W + sx) * PX_BYTES;
			dst[PX_R] = s[PX_R]; dst[PX_G] = s[PX_G];
			dst[PX_B] = s[PX_B];
			if (g_s3_live) { dst[PX_LIGHT] = s[PX_LIGHT]; continue; }
			dst[PX_LIGHT] = 0;
			fog_pixel(dst, (s32)s[PX_LIGHT]);
		}
	}

	(void)draw_objects_range;
	(void)warp_clouds;
	(void)shot3_step;
	(void)shot3_enter;
	(void)shot3_step_main;
	(void)shot3_step_mid;
	(void)shot3_step_end;
	(void)shot3_build_ramp;
	(void)shot3_seed_particles;
	(void)build_ground_tex;
}

/* OURS */
static void te_stats(char *out, int cap)
{
	(void)cap;
	if (g_s3_live) {
		sprintf(out, "tick %ld | shot 3 phase %ld | count %ld"
		             " | dist %ld scale %ld angle %ld | flash %ld",
		        (long)g_tick, (long)g_s3_phase, (long)g_s3_count,
		        (long)g_s3_dist, (long)g_s3_scale, (long)g_s3_angle,
		        (long)(g_s3_flash + g_s3_count));
		return;
	}
	if (g_rise) {
		sprintf(out, "t%ld st%ld ph%ld | cam %ld,%ld,%ld r%ld p%ld "
		             "t%ld | sc %ld alt %ld wh %ld | gnd %06lx",
		        (long)g_tick, (long)g_rise_stage, (long)g_rise_phase,
		        (long)g_rise_x, (long)g_rise_y, (long)g_rise_z,
		        (long)g_rise_roll, (long)g_risePan, (long)g_riseTilt,
		        (long)g_s3_scale, (long)g_s3_alt, (long)g_rise_fade,
		        (unsigned long)g_groundSrc);
		return;
	}
	sprintf(out, "tick %ld | key %ld.%02lx | cam %ld,%ld,%ld"
	             " | heading %ld horizon %ld | obj faces %ld",
	        (long)g_tick, (long)(g_camTime >> 8),
	        (unsigned long)(g_camTime & 0xffL),
	        (long)g_camX, (long)g_camY, (long)g_camZ,
	        (long)g_heading, (long)g_horizon, (long)g_objFacesDrawn);
}

/* OURS - both cue-ended phases move the handover with them, and the
 * blank is the part's own tail. */
static s32 te_length(void)
{
	if (g_te_blank) return g_te_blank;
	return te_end_tick() + TE_BLANK;
}

const Relic_Part PART_Temple = {
	"2.14", "The temple",
	"TempleModule_Run 0x003678d0 mode 0",
	PART_SHAPED_ONLY, TE_W, TE_H,
	te_init, te_step, te_frame, te_stats,
	TE_TICKS,
	te_prime,
	te_length
};
