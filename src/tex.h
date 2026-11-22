/* RELIC - Fshiptextures.
 *
 * ORIGIN 0x002ced9c Space_LoadShipTextures, which splits the chunk
 * through a four-entry table. See docs/formats/textures.txt.
 *
 * A TEXEL IS FOUR BYTES, 0xRRGG00BB, six bits a channel. The zero is
 * at +2 BETWEEN green and blue, not at +0 - the same layout as the
 * framebuffer, which is why the sampler copies a texel with one
 * move.l and converts nothing. Reading it as 0RGB shifts every
 * channel one position and still looks like an image.
 */

#ifndef RELIC_TEX_H
#define RELIC_TEX_H

#include "relic.h"

#define TEX_SUBS   4
#define TEX_W      256     /* the index is (v & 0xff00) | (u & 0xff) */

/* Loads and splits Fshiptextures. Needs relic.dat open. Non-zero when
 * the chunk is absent or a sub-block does not fit, which the caller
 * may survive by drawing nothing. */
int  Tex_Init(void);
int  Tex_Ready(void);
void Tex_Free(void);

/* One sub-block's base, and how many rows of 256 are actually behind
 * it. Subs 0 and 1 are 256 x 256. SUBS 2 AND 3 ARE 256 x 128 AND
 * CONTIGUOUS, so sub 2 addressed as a full 256 x 256 runs into sub 3
 * and lands exactly on the chunk's end - it reports 256 rows. Sub 3
 * is the tail and reports 128. */
const u8 *Tex_Base(s32 sub);
s32       Tex_Rows(s32 sub);

void Tex_Stats(char *out, int cap);

#endif /* RELIC_TEX_H */
