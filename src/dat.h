/* RELIC - the relic.dat archive.
 *
 * ORIGIN see docs/formats/relic_dat.txt. Big-endian throughout: a
 * u32 count, then 36-byte directory entries, then the payloads back
 * to back in no particular order.
 *
 * A chunk's NAME PREFIX IS ITS MEMORY POOL on the Amiga - C for chip
 * RAM, F for fast - which says what the data is FOR. Nothing here
 * needs that distinction, but the names keep it.
 */

#ifndef RELIC_DAT_H
#define RELIC_DAT_H

#include "relic.h"

int  Dat_Open(const char *path);
void Dat_Close(void);

/* The chunk's bytes, loaded on first ask and kept. NULL if the
 * archive is not open or the name is not in the directory; the
 * caller decides whether that is fatal. Names are the directory's,
 * trailing spaces stripped - "Fplasma", "Fcrust". */
const u8 *Dat_Chunk(const char *name, u32 *size_out);

/* For a host that wants to say what it found. */
int         Dat_Count(void);
const char *Dat_Name(int index);
u32         Dat_Size(int index);

#endif /* RELIC_DAT_H */
