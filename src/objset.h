/* RELIC - an object set: image-resident meshes with UV passes.
 *
 * ONE SHIP IS STORED TWICE. The vessel the temple scene flies off the
 * surface and the one the departure shot draws are byte-identical
 * meshes in two hunks, and the two renderers that walk them agree in
 * every constant - the >> 4, the 0x1f4 added before the divide, the
 * 0xc8 near clip and the 0x50, 0x32 screen centres. So this is one
 * body with two sets of data rather than two transcriptions.
 *
 * ORIGIN 0x0036a29a Temple_BuildObjects and 0x002d0146
 * Scene_SetupPart14 build the two sets; 0x00368610 and 0x002d18f8
 * Poly_DrawFaceList draw them.
 *
 * See docs/effects/return.txt 2.4.
 */

#ifndef RELIC_OBJSET_H
#define RELIC_OBJSET_H

#include "relic.h"

/* ORIGIN 0x002cf658 Uv_GenerateFaceParams - the D0 it is called
 * with, naming the pair of corners a pass spans and the two
 * whole-object modes. */
#define TE_UV_CYLC1  0x0004L
#define TE_UV_C1C2   0x0008L
#define TE_UV_C1C0   0x0010L
#define TE_UV_C0C2   0x0020L
#define TE_UV_ROTRNG 0x0040L
#define TE_UV_FLAT   0x4000L

/* One pass over an object's faces: `tex` is the A1, su/sv the pair
 * at 0x002e0c3c, cu/cv the D5 and D6 centres, `rows` the texture's
 * stride, and d2/r0/r1 the D2, D3 and D4 the rotate-and-range and
 * cylinder modes take. */
typedef struct {
	s32 obj, mode;
	u32 tex;
	s32 rows, su, sv, cu, cv;
	s32 d2, r0, r1;
} Te_UvPass;

/* The geometry triple is (face list, count word, vertex block), the
 * three addresses the builder writes into a scene slot. `halve` is
 * the shift the vertices are loaded with: the temple's copy is halved
 * in place by Temple_HalveVerts and the departure's is not. */
typedef struct {
	const u32 (*geom)[3];
	s32 nobj;
	const Te_UvPass *uv;
	s32 nuv;
	const s32 *cull;
	/* a bias, NOT a move - return.txt 2.4.1. NULL for none */
	const s32 *sort_bias;
	s32 halve;
	/* The two type-4 glows: their position triples, the eight-entry
	 * radius table at the slot's +0x30, and which blob draws them -
	 * 0 the temple's colour blob, 1 the departure's LIGHT teardrop
	 * at 0x002cf19e, whose counter a part supplies. */
	u32 glow_a, glow_b, glow_rad;
	s32 glow_style;
} Relic_ObjSet;

/* The set draw_objects_range walks. Selecting one loads its geometry,
 * so a part calls this from its init and nothing else touches it;
 * parts are initialised one at a time, which is what makes a single
 * shared state safe. Non-zero on failure. */
int Temple_UseObjectSet(const Relic_ObjSet *set);

/* OURS - the temple's own set, which is the default. */
const Relic_ObjSet *Temple_DefaultObjectSet(void);

/* The vessel at `angle`, from `cam` = x, y, z, yaw, pitch - the same
 * five Ship_Draw takes. Draws every object of the current set plus
 * its two glows, depth sorted. */
void Temple_DrawObjectSet(Relic_Frame *f, s32 angle, const s32 *cam);

/* ORIGIN 0x0036843a Temple_DrawObjectRange - the same, over a slice,
 * skipping every object whose longword in the mask table is set. The
 * ark's departure draws 0 to 0xc of a fourteen-object set and masks
 * three of those away. docs/effects/ark.txt 2. */
void Temple_DrawObjectRange(Relic_Frame *f, s32 angle, const s32 *cam,
                            s32 first, s32 last, u32 mask);

int Temple_ProjectObjectVertex(s32 angle, const s32 *cam, s32 obj,
                               s32 vertex, s32 *sx, s32 *sy);

void Temple_WarpTerrainTo(Relic_Frame *f, const u8 *tex, s32 angle,
                          s32 alt, s32 flash);

/* ORIGIN 0x0036e238 Temple_BlurFrame - the module's present pass:
 * five taps of the render buffer averaged against the last frame it
 * presented. Reset drops that history when the scene is entered. */
void Temple_BlurFrameTo(Relic_Frame *f);
void Temple_BlurFrameReset(void);

/* ORIGIN 0x002e0b08 g_FlareSizeStep - the counter a style-1 set
 * sizes its glows from, which the VBL hook turns one a tick. */
void Temple_SetGlowCounter(s32 c);

/* ---- the raycast terrain, on its own ---------------------------- */

/* THE SCENE'S GROUND WITHOUT ITS SCENE. draw_terrain casts one column
 * per screen x over a 64x64 grid and needs three chunks and ten
 * tables behind it; these three calls are that, and nothing of the
 * orb, the objects or the sparks. The object set is deliberately NOT
 * touched, so a caller holding one of its own keeps it.
 *
 * `cam` is five longwords - x, y, z, heading, horizon - the same
 * five the module keeps. Init fills them with the camera track's own
 * opening pose so a caller starts where the demo starts.
 * docs/reconstruction.txt 7.5. */
int  Temple_TerrainInit(s32 *cam);
void Temple_TerrainDrawTo(Relic_Frame *f, const s32 *cam);

/* A HEADING TO A STEP ALONG IT, in the scene's own degree tenths, so
 * a mover agrees with what the cast draws. */
void Temple_HeadingStep(s32 heading, s32 dist, s32 *dx, s32 *dz);

#endif /* RELIC_OBJSET_H */
