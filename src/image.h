/* RELIC - the executable as a byte image, addressed the demo's way.
 *
 * The reconstruction reads the ORIGINAL FILES. There is no
 * intermediate asset format to keep in sync, and a table quoted by
 * its image address in a plate comment can be fetched here by that
 * same address.
 *
 * See docs/formats/hunk.txt section 2.3 for the segment map, and
 * tools/hunk/imgaddr.py, which is the same mapping in Python.
 */

#ifndef RELIC_IMAGE_H
#define RELIC_IMAGE_H

#include "relic.h"

/* Load reverse/ghidra/relic. Returns 0 on success. */
int  Image_Load(const char *path);
void Image_Free(void);

/* A pointer to `addr` in the loaded image, or NULL.
 *
 * THERE IS NO SINGLE DELTA between an image address and a file
 * offset: each hunk's data begins after its own block header, and
 * the two BSS hunks take address space but no file bytes, so the
 * drift reaches 0x3304 across the file. An address inside BSS has
 * no bytes at all and returns NULL rather than a guess.
 */
const u8 *Image_At(u32 addr);

/* Big-endian reads at an image address. */
u32 Image_U32(u32 addr);
u16 Image_U16(u32 addr);
s16 Image_S16(u32 addr);

#endif /* RELIC_IMAGE_H */
