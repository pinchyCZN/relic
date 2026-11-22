/* RELIC - the 3D object format in Fspacedata.
 *
 * ORIGIN see docs/formats/geometry.txt. A chunk is split into
 * sub-blocks of three per object - faces, a count, vertices - by the
 * size table the loader is handed, and the geometry carries NO
 * texture coordinates and NO material index. Those come from
 * src/uv.c, which generates them per face at draw time.
 */

#ifndef RELIC_GEOM_H
#define RELIC_GEOM_H

#include "relic.h"

/* A face is always a triangle - nv_m1 is 2 in every face of every
 * object - and is stored as a closed ring of three edges. The corners
 * are edge[0][0], edge[1][0], edge[2][0]; the ring is redundant but it
 * is what the renderer walks, so it is read as stored and checked. */
#define GEOM_FACE_SIZE  14
#define GEOM_VERT_SIZE  12

/* ORIGIN 0x002d18f8 Poly_DrawFaceList - THE FIRST WORD OF A FACE IS A
 * MODE AND THE DRAWER BRANCHES ON IT: 1 picks the additive rasteriser,
 * 2 textures and jumps past both the near-plane reject and the
 * backface test, anything else textures and tests. Every one of
 * Fspacedata's 582 faces is stored as mode 2 [verified], and ship.c
 * culls anyway - docs/formats/geometry.txt 3.1 has the measurements
 * that say so and what the cull is standing in for. */
typedef struct Relic_Face {
	u16 mode;
	u16 corner[3];
} Relic_Face;

/* A vertex is three longwords in 16.16, and THE AXES ARE NOT x, y, z.
 * Scene_TransformProject divides by comp1 and adds the screen centre
 * to comp0 and comp2, so comp1 is the forward axis. Named for the
 * components they are, because an x/y/z name here would be a lie in
 * one convention or the other - geometry.txt section 2. */
typedef struct Relic_Vert {
	s32 comp[3];        /* 0 -> screen x, 1 -> DEPTH, 2 -> screen y */
} Relic_Vert;

typedef struct Relic_Obj {
	const Relic_Face *face;
	const Relic_Vert *vert;
	s32 nfaces;
	s32 nverts;
} Relic_Obj;

/* Parses Fspacedata against the size table in the executable. Needs
 * relic.dat open. Non-zero when the chunk is absent or a triple does
 * not parse, which the caller may survive. */
int  Geom_Init(void);
int  Geom_Ready(void);
void Geom_Free(void);

/* How many objects the walk recovered, and one of them. */
s32              Geom_Count(void);
const Relic_Obj *Geom_Get(s32 index);

/* THE CAMERA PATHS SHARE THE CHUNK. Fspacedata's size table has four
 * entries past the geometry - 480, 516, 32768 and 1024 - and the
 * first two are camera paths: slot 63 is g_pCamPath at 40 keys of
 * twelve, slot 64 g_pCamPath12 at 43 [verified, 0x002ced00]. A path
 * entry is three big-endian longwords like a vertex, but in WHOLE
 * units rather than 16.16. formats/geometry.txt 1.
 *
 * Returns the chunk bytes from where the geometry stopped, with how
 * many are left. NULL before Geom_Init. */
const u8 *Geom_Tail(s32 *bytes);

/* One of the two camera paths - 0 for g_pCamPath, 1 for g_pCamPath12 -
 * and how many 12-byte entries it holds. NULL if the chunk is short. */
const u8 *Geom_CamPath(s32 which, s32 *entries);

/* The tail's other two sub-blocks, which Rast_AddTri needs: slot 65
 * g_pFlatPattern, 32768 bytes read as 128 rows of 256, and slot 66
 * g_pFlatPalette, 256 whole four-byte pixels. NULL if short. */
const u8 *Geom_FlatPattern(void);
const u8 *Geom_FlatPalette(void);

/* What the parse found, for a host that wants to say so. */
void Geom_Stats(char *out, int cap);

#endif /* RELIC_GEOM_H */
