/* RELIC - the textured triangle.
 *
 * ORIGIN 0x002caef6 Rast_TexturedPoly and its span filler at
 * 0x002cabce Rast_FillSpansTextured. See
 * docs/subsystems/renderer.txt.
 *
 * THE TEXTURE IS 256 x 256 AND A TEXEL IS FOUR BYTES, read off the
 * renderer rather than off the data: the index is built as
 * (v & 0xff00) | (u & 0x00ff) and the addressing mode scales it by
 * four. SO u AND v WRAP, AND A RECONSTRUCTION MUST MASK, NOT CLAMP -
 * there is no bounds test anywhere in the loop, so a polygon whose
 * coordinates run off the texture tiles.
 */

#ifndef RELIC_RAST_H
#define RELIC_RAST_H

#include "relic.h"

/* One triangle. x and y are DEMO pixels IN 24.8 - the fraction is
 * what stops a vertex snapping to whole demo pixels and boiling as
 * the object turns. The frame's scale is applied here, so the demo's
 * texels are resampled rather than run off - rule 20. rows bounds the
 * texture, which matters only for Fshiptextures' last sub-block. */
#define RAST_FRAC 8
void Rast_TexTri(Relic_Frame *f, const s32 *x, const s32 *y,
                 const s32 *u, const s32 *v,
                 const u8 *tex, s32 rows);

/* ORIGIN 0x0036884c - a TYPE-5 face. The same affine walk, but the
 * texel is a shade: bias comes off it and the rest is subtracted
 * from the pixel's LIGHT. See rast.c. */
void Rast_ShadeTri(Relic_Frame *f, const s32 *x, const s32 *y,
                   const s32 *u, const s32 *v,
                   const u8 *tex, s32 rows, s32 bias);

/* ORIGIN 0x002cb3aa Rast_AdditivePoly - a face whose MODE WORD IS 1,
 * which in this corpus is only the three shells around the lightning
 * arcs. It ADDS a whole four-byte pixel, so unlike every glow it
 * touches R, G, LIGHT and B together. x and y in 24.8 as above; u and
 * v are pattern-space whole numbers. docs/effects/orb.txt 9. */
void Rast_AddTri(Relic_Frame *f, const s32 *x, const s32 *y,
                 const s32 *u, const s32 *v,
                 const u8 *pattern, const u8 *palette, s32 scroll);

#endif /* RELIC_RAST_H */
