/* RELIC - Fspacedata's objects. See geom.h and
 * docs/formats/geometry.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geom.h"
#include "dat.h"
#include "image.h"

/* ORIGIN 0x002e483c g_SpaceDataSplit - the size table the loader is
 * handed for Fspacedata, 67 longwords. Read off the Dat_LoadSplit
 * call site, not guessed from the chunk - geometry.txt section 1. */
#define A_SPLIT_TABLE   0x002e483cUL
#define SPLIT_ENTRIES   67

#define MAX_OBJS        32

static Relic_Obj  g_obj[MAX_OBJS];
static Relic_Face *g_faces;
static Relic_Vert *g_verts;
static s32 g_nobj;
static s32 g_stop_off, g_stop_entry;
static int g_ready;

/* OURS */
static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* OURS */
static u16 be16(const u8 *p)
{
	return (u16)(((u32)p[0] << 8) | p[1]);
}

/* ORIGIN geometry.txt section 1 - one (faces, count, verts) triple.
 * BOTH COUNTS ARE STORED MINUS ONE because the loops are dbf, which
 * runs count + 1 times. Returns zero when the triple does not parse,
 * which is how the walk finds the end of the geometry in a table whose
 * tail is something else. */
static int parse_one(const u8 *blob, u32 off, u32 a, u32 b, u32 c,
                     Relic_Obj *o, Relic_Face *fout, Relic_Vert *vout)
{
	const u8 *fa = blob + off;
	const u8 *ct = fa + a;
	const u8 *ve = ct + b;
	s32 nf, nv, i;

	if (b != 2) return 0;
	nf = (s32)be16(ct) + 1;
	nv = (s32)be32(ve) + 1;
	if (a != (u32)nf * GEOM_FACE_SIZE) return 0;
	if (c != 4 + (u32)nv * GEOM_VERT_SIZE) return 0;

	for (i = 0; i < nf; i++) {
		const u8 *f = fa + i * GEOM_FACE_SIZE;
		/* THE RING MUST CLOSE - (a,b) (b,c) (c,a). A triple that
		 * parses by size but whose ring does not close is not
		 * geometry, so this is part of the test and not a nicety. */
		if (be16(f) != 2) return 0;
		if (be16(f + 4) != be16(f + 6)) return 0;
		if (be16(f + 8) != be16(f + 10)) return 0;
		if (be16(f + 12) != be16(f + 2)) return 0;
		fout[i].mode = be16(f + 0);
		fout[i].corner[0] = be16(f + 2);
		fout[i].corner[1] = be16(f + 6);
		fout[i].corner[2] = be16(f + 10);
		if (fout[i].corner[0] >= nv || fout[i].corner[1] >= nv ||
		    fout[i].corner[2] >= nv)
		    return 0;
	}
	for (i = 0; i < nv; i++) {
		const u8 *v = ve + 4 + i * GEOM_VERT_SIZE;
		vout[i].comp[0] = (s32)be32(v);
		vout[i].comp[1] = (s32)be32(v + 4);
		vout[i].comp[2] = (s32)be32(v + 8);
	}
	o->face = fout;
	o->vert = vout;
	o->nfaces = nf;
	o->nverts = nv;
	return 1;
}

/* OURS. Two passes over the same table: one to total the faces and
 * vertices so the two arrays can be one allocation each, one to fill
 * them. The sizes come from the table, so the first pass is the same
 * test as the second and cannot disagree with it. */
int Geom_Init(void)
{
	u32 sizes[SPLIT_ENTRIES];
	const u8 *blob;
	u32 size = 0, off;
	s32 i, nf_tot = 0, nv_tot = 0, fi = 0, vi = 0;

	if (g_ready) return 0;
	blob = Dat_Chunk("Fspacedata", &size);
	if (!blob) return 1;
	for (i = 0; i < SPLIT_ENTRIES; i++)
		sizes[i] = Image_U32((u32)(A_SPLIT_TABLE + (u32)i * 4));

	/* first pass: how much there is */
	off = 0;
	for (i = 0; i + 2 < SPLIT_ENTRIES; i += 3) {
		u32 a = sizes[i], b = sizes[i + 1], c = sizes[i + 2];
		s32 nf, nv;
		if (off + a + b + c > size) break;
		if (b != 2) break;
		nf = (s32)be16(blob + off + a) + 1;
		nv = (s32)be32(blob + off + a + b) + 1;
		if (a != (u32)nf * GEOM_FACE_SIZE) break;
		if (c != 4 + (u32)nv * GEOM_VERT_SIZE) break;
		if (nf_tot + nf < 0 || nv_tot + nv < 0) break;
		nf_tot += nf;
		nv_tot += nv;
		off += a + b + c;
	}
	if (!nf_tot || !nv_tot) return 1;

	g_faces = (Relic_Face *)malloc((size_t)nf_tot * sizeof *g_faces);
	g_verts = (Relic_Vert *)malloc((size_t)nv_tot * sizeof *g_verts);
	if (!g_faces || !g_verts) { Geom_Free(); return 1; }

	off = 0;
	g_nobj = 0;
	for (i = 0; i + 2 < SPLIT_ENTRIES && g_nobj < MAX_OBJS; i += 3) {
		u32 a = sizes[i], b = sizes[i + 1], c = sizes[i + 2];
		if (off + a + b + c > size) break;
		if (!parse_one(blob, off, a, b, c, &g_obj[g_nobj],
		               g_faces + fi, g_verts + vi))
		    break;
		fi += g_obj[g_nobj].nfaces;
		vi += g_obj[g_nobj].nverts;
		off += a + b + c;
		g_nobj++;
	}
	g_stop_off = (s32)off;
	g_stop_entry = i;
	g_ready = (g_nobj > 0);
	return g_ready ? 0 : 1;
}

/* OURS */
int Geom_Ready(void) { return g_ready; }

/* OURS */
void Geom_Free(void)
{
	free(g_faces); g_faces = 0;
	free(g_verts); g_verts = 0;
	g_nobj = 0;
	g_ready = 0;
}

/* OURS */
s32 Geom_Count(void) { return g_nobj; }

/* OURS */
const Relic_Obj *Geom_Get(s32 index)
{
	if (index < 0 || index >= g_nobj) return 0;
	return &g_obj[index];
}

/* OURS */
const u8 *Geom_Tail(s32 *bytes)
{
	const u8 *blob;
	u32 size = 0;

	if (!g_ready) return 0;
	blob = Dat_Chunk("Fspacedata", &size);
	if (!blob || (u32)g_stop_off >= size) return 0;
	if (bytes) *bytes = (s32)(size - (u32)g_stop_off);
	return blob + g_stop_off;
}

/* ORIGIN 0x002ced00 Space_LoadAssets, +0x2a - the two sub-blocks
 * after the geometry, by their
 * sizes out of the same table the triples came from. */
const u8 *Geom_CamPath(s32 which, s32 *entries)
{
	static const s32 OFF[2] = { 0L, 480L };
	static const s32 LEN[2] = { 480L, 516L };
	const u8 *tail;
	s32 bytes = 0;

	if (which < 0 || which > 1) return 0;
	tail = Geom_Tail(&bytes);
	if (!tail || bytes < OFF[which] + LEN[which]) return 0;
	if (entries) *entries = LEN[which] / 12L;
	return tail + OFF[which];
}

/* ORIGIN 0x002ced00 Space_LoadAssets, +0x2a - slots 65 and 66,
 * past the two camera paths. */
static const u8 *tail_at(s32 off, s32 len)
{
	const u8 *tail;
	s32 bytes = 0;

	tail = Geom_Tail(&bytes);
	if (!tail || bytes < off + len) return 0;
	return tail + off;
}

/* ORIGIN 0x002cff42 Flat_ConditionTables - BOTH BLOCKS ARE CONDITIONED
 * IN PLACE, ONCE, so the raw chunk bytes are never what the rasteriser
 * reads. Neither conditioned form is anywhere in relic.dat, which
 * reads as the blocks being built from scratch; they are not.
 * The copies are ours - the mapped image must not be written.
 * ONE ARRAY, BECAUSE 0x002cb636 READS PAST THE PATTERN: it indexes
 * with a +0x7f displacement AFTER masking to 0x7fff, and in the demo
 * those bytes are the palette - 996 + 0x8000 is where it starts.
 * effects/orb.txt 9. */
#define GEOM_PAT_LEN 32768L
#define GEOM_PAL_LEN  1024L
static u8 g_flat[GEOM_PAT_LEN + GEOM_PAL_LEN];
static int g_cond;

/* ORIGIN 0x002cff42 Flat_ConditionTables, +0x3a - lsr.b #2 over
 * 0x8001 bytes, one past the block: the demo shifts the palette's
 * first byte too, and the palette's own pass discards it. */
static void condition(void)
{
	const u8 *src;
	s32 i;

	g_cond = 1;
	src = tail_at(996L, GEOM_PAT_LEN);
	if (src)
		for (i = 0; i < GEOM_PAT_LEN; i++)
			g_flat[i] = (u8)(src[i] >> 2);
	/* ORIGIN 0x002cff42 Flat_ConditionTables, +0x1c - 193 longwords,
	 * NOT 256. lsl.l #8 then lsr.w #8 drops byte 0 and puts a ZERO in
	 * byte 2, so the conditioned palette carries NO LIGHT. */
	src = tail_at(33764L, GEOM_PAL_LEN);
	if (src)
		memcpy(g_flat + GEOM_PAT_LEN, src, (size_t)GEOM_PAL_LEN);
	if (src)
		for (i = 0; i < 193L; i++) {
			const u8 *e = src + i * 4L;
			u8 *o = g_flat + GEOM_PAT_LEN + i * 4L;
			o[0] = (u8)((e[1] & 0xf8) >> 2);
			o[1] = (u8)((e[2] & 0xf8) >> 2);
			o[2] = 0;
			o[3] = (u8)((e[3] & 0xf8) >> 2);
		}
}

/* ORIGIN 0x002e4834 g_pFlatPattern, conditioned. */
const u8 *Geom_FlatPattern(void)
{
	if (!g_cond) condition();
	return tail_at(996L, GEOM_PAT_LEN) ? g_flat : 0;
}

/* ORIGIN 0x002e4838 g_pFlatPalette, conditioned. */
const u8 *Geom_FlatPalette(void)
{
	if (!g_cond) condition();
	return tail_at(33764L, GEOM_PAL_LEN) ? g_flat + GEOM_PAT_LEN : 0;
}

/* OURS */
void Geom_Stats(char *out, int cap)
{
	s32 i, nf = 0, nv = 0;
	(void)cap;
	for (i = 0; i < g_nobj; i++) {
		nf += g_obj[i].nfaces;
		nv += g_obj[i].nverts;
	}
	sprintf(out, "%ld obj  %ld faces  %ld verts  stopped at entry %ld "
	             "offset %ld",
	        (long)g_nobj, (long)nf, (long)nv,
	        (long)g_stop_entry, (long)g_stop_off);
}
