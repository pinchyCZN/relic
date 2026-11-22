/* RELIC - the relic.dat archive. See dat.h and
 * docs/formats/relic_dat.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dat.h"

#define DAT_MAX_ENTRIES 64
#define ENTRY_BYTES     36
#define NAME_BYTES      16

typedef struct {
	char name[NAME_BYTES + 1];
	u32  offset;
	u32  size;
	u8  *data;                  /* NULL until asked for */
} Dat_Entry;

static FILE      *g_fp;
static Dat_Entry  g_dir[DAT_MAX_ENTRIES];
static int        g_count;

/* OURS */
static u32 rd_u32(const u8 *p)
{
	return RD_U32(p);
}

/* OURS - the name is SPACE padded and not NUL terminated, so the
 * trailing spaces have to come off or no lookup ever matches. */
static void copy_name(char *dst, const u8 *src)
{
	int i, n = NAME_BYTES;

	while (n > 0 && src[n - 1] == ' ') n--;
	for (i = 0; i < n; i++) dst[i] = (char)src[i];
	dst[n] = 0;
}

/* OURS */
int Dat_Open(const char *path)
{
	u8 hdr[4], rec[ENTRY_BYTES];
	int i;

	Dat_Close();
	if (!path) return 1;
	g_fp = fopen(path, "rb");
	if (!g_fp) return 1;
	if (fread(hdr, 1, 4, g_fp) != 4) { Dat_Close(); return 1; }
	g_count = (int)rd_u32(hdr);
	if (g_count < 1 || g_count > DAT_MAX_ENTRIES) {
		fprintf(stderr, "dat: %d entries is not credible\n", g_count);
		Dat_Close();
		return 1;
	}
	for (i = 0; i < g_count; i++) {
		if (fread(rec, 1, ENTRY_BYTES, g_fp) != ENTRY_BYTES) {
			Dat_Close();
			return 1;
		}
		copy_name(g_dir[i].name, rec);
		g_dir[i].offset = rd_u32(rec + 0x10);
		g_dir[i].size   = rd_u32(rec + 0x14);
		g_dir[i].data   = NULL;
	}
	return 0;
}

/* OURS */
void Dat_Close(void)
{
	int i;
	for (i = 0; i < g_count; i++) {
		if (g_dir[i].data) { free(g_dir[i].data); g_dir[i].data = NULL; }
	}
	g_count = 0;
	if (g_fp) { fclose(g_fp); g_fp = NULL; }
}

/* OURS */
const u8 *Dat_Chunk(const char *name, u32 *size_out)
{
	int i;

	if (size_out) *size_out = 0;
	if (!g_fp || !name) return NULL;
	for (i = 0; i < g_count; i++) {
		if (strcmp(g_dir[i].name, name) != 0) continue;
		if (!g_dir[i].data) {
			u8 *buf = (u8 *)malloc((size_t)g_dir[i].size);
			if (!buf) return NULL;
			if (fseek(g_fp, (long)g_dir[i].offset, SEEK_SET) != 0
			        || fread(buf, 1, g_dir[i].size, g_fp) != g_dir[i].size) {
				fprintf(stderr, "dat: %s is short at 0x%08lx\n",
				        name, (unsigned long)g_dir[i].offset);
				free(buf);
				return NULL;
			}
			g_dir[i].data = buf;
		}
		if (size_out) *size_out = g_dir[i].size;
		return g_dir[i].data;
	}
	return NULL;
}

/* OURS */
int Dat_Count(void) { return g_count; }

/* OURS */
const char *Dat_Name(int index)
{
	return (index >= 0 && index < g_count) ? g_dir[index].name : NULL;
}

/* OURS */
u32 Dat_Size(int index)
{
	return (index >= 0 && index < g_count) ? g_dir[index].size : 0;
}
