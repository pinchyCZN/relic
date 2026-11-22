/* RELIC - the executable as a byte image. See image.h. */

#include <stdio.h>
#include <stdlib.h>

#include "image.h"

typedef struct {
	const char *name;
	u32 lo;
	u32 hi;
	long file_lo;       /* -1 for BSS, which has no file bytes */
} Segment;

/* [verified, docs/formats/hunk.txt 2.3] - the same table as
 * tools/hunk/imgaddr.py SEGMENTS. The names match what Ghidra's
 * amiga loader creates, so an address pasted out of the listing
 * works here unchanged. */
static const Segment SEGMENTS[] = {
	{ "CODE_00", 0x0021f000UL, 0x002c797bUL, 0x000038L },
	{ "BSS_01",  0x002c797cUL, 0x002c8083UL, -1L       },
	{ "CODE_02", 0x002c8084UL, 0x002c8233UL, 0x0a8db4L },
	{ "CODE_03", 0x002c8234UL, 0x0033be1fUL, 0x0a8f70L },
	{ "BSS_04",  0x0033be20UL, 0x003431a7UL, -1L       },
	{ "CODE_05", 0x003431a8UL, 0x003678cfUL, 0x120ed0L },
	{ "CODE_06", 0x003678d0UL, 0x003e43cfUL, 0x145604L }
};
#define NSEG (sizeof SEGMENTS / sizeof SEGMENTS[0])

static u8  *g_img;
static long g_len;

/* OURS */
int Image_Load(const char *path)
{
	FILE *f;
	long n;

	Image_Free();
	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "relic: cannot open %s\n", path);
		return 1;
	}
	fseek(f, 0L, SEEK_END);
	n = ftell(f);
	fseek(f, 0L, SEEK_SET);
	if (n <= 0) {
		fclose(f);
		fprintf(stderr, "relic: %s is empty\n", path);
		return 1;
	}
	g_img = (u8 *)malloc((size_t)n);
	if (!g_img) {
		fclose(f);
		fprintf(stderr, "relic: out of memory for %ld bytes\n", n);
		return 1;
	}
	if (fread(g_img, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(g_img);
		g_img = NULL;
		fprintf(stderr, "relic: short read on %s\n", path);
		return 1;
	}
	fclose(f);
	g_len = n;
	return 0;
}

/* OURS */
void Image_Free(void)
{
	if (g_img) {
		free(g_img);
		g_img = NULL;
	}
	g_len = 0;
}

/* OURS */
const u8 *Image_At(u32 addr)
{
	size_t i;

	if (!g_img) return NULL;
	for (i = 0; i < NSEG; i++) {
		const Segment *s = &SEGMENTS[i];
		if (addr >= s->lo && addr <= s->hi) {
			long off;
			if (s->file_lo < 0) return NULL;        /* BSS */
			off = s->file_lo + (long)(addr - s->lo);
			if (off < 0 || off >= g_len) return NULL;
			return g_img + off;
		}
	}
	return NULL;
}

/* OURS */
u32 Image_U32(u32 addr)
{
	const u8 *p = Image_At(addr);
	return p ? RD_U32(p) : 0UL;
}

/* OURS */
u16 Image_U16(u32 addr)
{
	const u8 *p = Image_At(addr);
	return p ? RD_U16(p) : (u16)0;
}

/* OURS */
s16 Image_S16(u32 addr)
{
	const u8 *p = Image_At(addr);
	return p ? RD_S16(p) : (s16)0;
}
