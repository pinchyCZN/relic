/* RELIC - Fshiptextures. See tex.h and docs/formats/textures.txt. */

#include <stdio.h>

#include "tex.h"
#include "dat.h"
#include "image.h"

/* ORIGIN 0x002e495c - the four sub-block SIZES. Space_LoadShipTextures
 * passes A1 = 0x002e4948 and A2 = 0x002e495c, and Dat_LoadSplit
 * (0x0021fa94) reads a count off A1 then fills that many slots behind
 * it, advancing by each size from A2 - the same descriptor-and-sizes
 * pair Mem_AllocSubBlocks takes. SO 0x002e4948 IS [count, slot...],
 * NOT THE SIZES: it reads 4, 0, 0, 0, 0 out of the image because the
 * slots are filled at load time. The sizes are 0x40000, 0x40000,
 * 0x20000, 0x20000 and sum to the chunk [verified]. */
#define A_TEX_SPLIT   0x002e495cUL

static const u8 *g_base[TEX_SUBS];
static s32 g_rows[TEX_SUBS];
static u32 g_size;
static int g_ready;

/* OURS */
int Tex_Init(void)
{
	const u8 *blob;
	u32 off = 0;
	s32 i;

	if (g_ready) return 0;
	blob = Dat_Chunk("Fshiptextures", &g_size);
	if (!blob) return 1;
	for (i = 0; i < TEX_SUBS; i++) {
		u32 sz = Image_U32((u32)(A_TEX_SPLIT + (u32)i * 4));
		u32 avail;
		if (off + sz > g_size) return 1;
		g_base[i] = blob + off;
		/* HOW MANY ROWS ARE REALLY THERE, which is not the same as the
		 * sub-block's own height. The sampler masks v to eight bits
		 * whatever the real height, so every sub is addressed as
		 * 256 x 256; what bounds it is how much chunk follows the
		 * base. Subs 2 and 3 being contiguous is exactly why sub 2
		 * gets a full 256. */
		avail = (g_size - off) / (TEX_W * PX_BYTES);
		g_rows[i] = (s32)(avail > TEX_W ? TEX_W : avail);
		off += sz;
	}
	g_ready = 1;
	return 0;
}

/* OURS */
int Tex_Ready(void) { return g_ready; }

/* OURS */
void Tex_Free(void)
{
	s32 i;
	for (i = 0; i < TEX_SUBS; i++) { g_base[i] = 0; g_rows[i] = 0; }
	g_ready = 0;
}

/* OURS */
const u8 *Tex_Base(s32 sub)
{
	if (!g_ready || sub < 0 || sub >= TEX_SUBS) return 0;
	return g_base[sub];
}

/* OURS */
s32 Tex_Rows(s32 sub)
{
	if (!g_ready || sub < 0 || sub >= TEX_SUBS) return 0;
	return g_rows[sub];
}

/* OURS */
void Tex_Stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "%lu bytes  rows %ld/%ld/%ld/%ld",
	        (unsigned long)g_size, (long)g_rows[0], (long)g_rows[1],
	        (long)g_rows[2], (long)g_rows[3]);
}
