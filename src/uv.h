/* RELIC - texture coordinates, which the geometry does NOT store.
 *
 * ORIGIN 0x002cf658 Uv_GenerateFaceParams. The demo computes a face's
 * UVs by projecting its object-space vertices, once at setup, and
 * Space_BuildObjects supplies the parameters per object. See
 * docs/formats/geometry.txt section 4.
 *
 * THE MODE BITS ARE INDEPENDENT TESTS, NOT A SWITCH. Every set bit
 * runs and they all write the same slot, so a later one wins over an
 * earlier one for the faces it accepts - and a projection that
 * REJECTS a face advances the write pointer without writing, leaving
 * whatever an earlier pass put there. That is how objects 3 and 4 get
 * a flat base coat with a cylindrical band over part of it.
 */

#ifndef RELIC_UV_H
#define RELIC_UV_H

#include "relic.h"
#include "geom.h"

/* The bits Uv_GenerateFaceParams tests, in the order it tests them. */
#define UV_ROTATED        0x0001L   /* Uv_ProjRotatedXY   */
#define UV_CYL_C2         0x0002L   /* Uv_ProjAngularZ    */
#define UV_CYL_C1         0x0004L   /* Uv_ProjAngularY    */
#define UV_PLANAR_C1C2    0x0008L
#define UV_PLANAR_C1C0    0x0010L
#define UV_PLANAR_C0C2    0x0020L
#define UV_ROTATED_RANGE  0x0040L
#define UV_TEXTURE_ONLY   0x4000L   /* Uv_SetTextureOnly  */
#define UV_ATAN_ALT       0x8000L   /* changes AtanLookup's scaling */

/* ONLY THE SHIP'S MODES ARE IMPLEMENTED: UV_ROTATED, UV_CYL_C2,
 * UV_ATAN_ALT and UV_TEXTURE_ONLY. The other four bits are not used by
 * any object Space_BuildObjects gives parameters to - geometry.txt
 * section 6 - and Uv_Generate reports a pass it cannot honour rather
 * than silently producing nothing. */
#define UV_IMPLEMENTED \
    (UV_ROTATED | UV_CYL_C2 | UV_TEXTURE_ONLY | UV_ATAN_ALT)

/* One call's worth of what Space_BuildObjects loads into D0..D7. */
typedef struct Relic_UvPass {
	s32 mode;
	s32 scale;
	s32 range_min, range_max;
	s32 centre_u, centre_v;
	s32 angle;
} Relic_UvPass;

/* Three corner (u, v) pairs for one face, and whether any pass wrote
 * them. u and v are NOT masked here - the sampler masks, because the
 * demo's index does. */
typedef struct Relic_FaceUv {
	s32 u[3], v[3];
	int written;
} Relic_FaceUv;

/* Runs each pass over every face in order, writing into out[nfaces].
 * out must be zeroed by the caller for the layering to mean anything.
 * Returns the number of passes that asked for an unimplemented bit. */
s32 Uv_Generate(const Relic_Obj *o, const Relic_UvPass *pass, s32 npass,
                Relic_FaceUv *out);

/* ORIGIN 0x002d1690 Uv_ProjBoxish - THE EIGHTH PROJECTION, and the
 * only one outside Uv_GenerateFaceParams' bit-selected family. Object
 * 19 alone uses it: the window's surround, 158 faces, drawn by mode 4
 * of the window scene - docs/parts.txt 2.8.
 *
 * It takes u from comp0 and v from comp1 + comp2, AND SWAPS TO
 * (comp2, comp1) WHEN ALL THREE CORNERS AGREE ON comp1 + comp2 -
 * which is the axis choice that makes it "boxish": a face lying in
 * that plane would project to a degenerate line, so it gets the other
 * pair. Then >> 12, +0x40 on u only, masked to a byte, and folded:
 * anything at or over 0x80 becomes 0xff - v negated, so the
 * coordinate ramps up and back down instead of wrapping.
 *
 * IT ALSO REWRITES EVERY FACE'S MODE WORD TO ZERO, so object 19 is
 * the one object in the chunk that IS backface-tested - geom.h. */
void Uv_Boxish(const Relic_Obj *o, Relic_FaceUv *out);

#endif /* RELIC_UV_H */
