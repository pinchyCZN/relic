/* ProWizard
 * Copyright (C) 1998 Asle / ReDoX
 * Modified by Claudio Matsuoka
 * Modified in 2021 by Alice Rowan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * The_Player_6.1a.c
 *
 * The Player 6.1a to Protracker.
 *
 * Taken from libxmp's src/loaders/prowizard/p61a.c and made
 * self-contained and memory-to-memory for RELIC; the two functions
 * are otherwise unchanged. libxmp's own note follows.
 *
 * note: As for version 5.0A and 6.0A, it's a REAL mess !.
 *      It's VERY badly coded, I know. Just don't forget it was mainly done
 *      to test the description I made of P61a format.
 */

/* ---------------------------------------------------------------- *
 * RELIC: the same depacker over MEMORY rather than files.
 *
 * libxmp hands prowizard a HIO_HANDLE to read and a FILE to write,
 * and pulls ptk_table, PTK_IS_VALID_NOTE and a dozen other things in
 * through prowiz.h.  Neither suits a demo that must not touch the
 * disk, so the two functions below are carried over unchanged and
 * everything they lean on is in this file.
 *
 * The entry points are at the bottom: P61A_Test and P61A_Depack.
 * ---------------------------------------------------------------- */

#include <stdio.h>                /* SEEK_SET and its two friends */
#include <stdlib.h>
#include <string.h>

typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned int   uint32;

#define MAGIC4(a,b,c,d) \
    (((uint32)(a)<<24)|((uint32)(b)<<16)|((uint32)(c)<<8)|(d))
#define PW_MOD_MAGIC    MAGIC4('M','.','K','.')

/* prowiz.h: a test returns how many MORE bytes it needs, so a caller
 * feeding it a stream can come back with them. We always have the
 * whole module, so a short buffer is simply a reject. */
#define PW_REQUEST_DATA(s,n) \
    do { if ((s)<(n)) return ((n)-(s)); } while (0)

/* ptktable.c - Protracker's period table, indexed by note. */
static const uint8 ptk_table[37][2] = {
	{ 0x00, 0x00 },
	{ 0x03, 0x58 }, { 0x03, 0x28 }, { 0x02, 0xfa }, { 0x02, 0xd0 },
	{ 0x02, 0xa6 }, { 0x02, 0x80 }, { 0x02, 0x5c }, { 0x02, 0x3a },
	{ 0x02, 0x1a }, { 0x01, 0xfc }, { 0x01, 0xe0 }, { 0x01, 0xc5 },
	{ 0x01, 0xac }, { 0x01, 0x94 }, { 0x01, 0x7d }, { 0x01, 0x68 },
	{ 0x01, 0x53 }, { 0x01, 0x40 }, { 0x01, 0x2e }, { 0x01, 0x1d },
	{ 0x01, 0x0d }, { 0x00, 0xfe }, { 0x00, 0xf0 }, { 0x00, 0xe2 },
	{ 0x00, 0xd6 }, { 0x00, 0xca }, { 0x00, 0xbe }, { 0x00, 0xb4 },
	{ 0x00, 0xaa }, { 0x00, 0xa0 }, { 0x00, 0x97 }, { 0x00, 0x8f },
	{ 0x00, 0x87 }, { 0x00, 0x7f }, { 0x00, 0x78 }, { 0x00, 0x71 }
};

#define PTK_IS_VALID_NOTE(n) ((n) < 37)

/* ---- the reader, standing in for HIO_HANDLE --------------------- */

typedef struct {
	const uint8 *data;
	long size;
	long pos;
	int err;                    /* sticky, like hio_error */
} P61_IN;

static int hio_read8(P61_IN *in)
{
	if (in->pos < 0 || in->pos >= in->size) { in->err = 1; return 0; }
	return in->data[in->pos++];
}

static int hio_read16b(P61_IN *in)
{
	int a = hio_read8(in);
	int b = hio_read8(in);
	return (a << 8) | b;
}

static uint32 hio_read32b(P61_IN *in)
{
	uint32 a = (uint32)hio_read16b(in);
	uint32 b = (uint32)hio_read16b(in);
	return (a << 16) | b;
}

/* hio_read's argument order is fread's: size then count. */
static long hio_read(void *buf, long size, long count, P61_IN *in)
{
	long want = size * count;

	if (want < 0 || in->pos < 0 || in->pos + want > in->size) {
		in->err = 1;
		return 0;
	}
	memcpy(buf, in->data + in->pos, (size_t)want);
	in->pos += want;
	return count;
}

static int hio_seek(P61_IN *in, long off, int whence)
{
	long at = (whence == SEEK_CUR) ? in->pos + off
	        : (whence == SEEK_END) ? in->size + off : off;

	if (at < 0 || at > in->size) { in->err = 1; return -1; }
	in->pos = at;
	return 0;
}

/* common.h - a big-endian word out of a plain buffer. */
static uint16 readmem16b(const uint8 *m)
{
	return (uint16)(((uint16)m[0] << 8) | m[1]);
}

static long hio_tell(P61_IN *in) { return in->pos; }
static int  hio_error(P61_IN *in) { int e = in->err; in->err = 0; return e; }

/* ---- the writer, standing in for FILE --------------------------- */

typedef struct {
	uint8 *data;
	long len;
	long cap;
	int err;
} P61_OUT;

static int p61_room(P61_OUT *out, long n)
{
	long want = out->len + n;
	uint8 *p;

	if (out->err) return 0;
	if (want <= out->cap) return 1;
	while (out->cap < want) out->cap = out->cap ? out->cap * 2 : 0x10000L;
	p = (uint8 *)realloc(out->data, (size_t)out->cap);
	if (!p) { out->err = 1; return 0; }
	out->data = p;
	return 1;
}

/* fwrite's argument order again, and its return. */
static long fwrite_mem(const void *buf, long size, long count, P61_OUT *out)
{
	long n = size * count;

	if (n <= 0 || !p61_room(out, n)) return 0;
	memcpy(out->data + out->len, buf, (size_t)n);
	out->len += n;
	return count;
}

static void write8(P61_OUT *out, uint8 b)
{
	if (!p61_room(out, 1)) return;
	out->data[out->len++] = b;
}

static void write16b(P61_OUT *out, uint16 v)
{
	if (!p61_room(out, 2)) return;
	out->data[out->len++] = (uint8)(v >> 8);
	out->data[out->len++] = (uint8)v;
}

static void write32b(P61_OUT *out, uint32 v)
{
	write16b(out, (uint16)(v >> 16));
	write16b(out, (uint16)v);
}

static int pw_write_zero(P61_OUT *out, long len)
{
	if (len <= 0 || !p61_room(out, len)) return 0;
	memset(out->data + out->len, 0, (size_t)len);
	out->len += len;
	return 0;
}

#define fwrite(b,s,c,o) fwrite_mem((b),(s),(c),(o))

static int depack_p61a(P61_IN *in, P61_OUT *out)
{
	uint8 c1, c2, c3, c4, c5, c6;
	long max_row;
	static uint8 tmp[1024];
	signed char *smp_buffer;
	int len;
	int npat;
	int nins;
	static uint8 tdata[512][256];
	static uint8 ptable[128];
	int isize[31];
	/* uint8 PACK[31]; */
	uint8 use_delta = 0;
	/* uint8 use_packed = 0; */
	static int taddr[128][4];
	int tdata_addr = 0;
	int sdata_addr = 0;
	/* int ssize = 0; */
	int i = 0, j, k, l, a, b, z;
	int smp_size[31];
	int saddr[31];
	/* int Unpacked_Sample_Data_Size; */
	int val;

	memset(taddr, 0, sizeof(taddr));
	memset(tdata, 0, sizeof(tdata));
	memset(ptable, 0, sizeof(ptable));
	memset(smp_size, 0, sizeof(smp_size));
	memset(isize, 0, sizeof(isize));
	memset(saddr, 0, sizeof(saddr));
	for (i = 0; i < 31; i++) {
	/* PACK[i] = 0; */
	/* DELTA[i] = 0;*/
	}

	sdata_addr = hio_read16b(in);   /* read sample data address */
	npat = hio_read8(in);       /* read real number of pattern */

	/* Sanity check */
	if (npat >= 128) {
		return -1;
	}

	nins = hio_read8(in);       /* read number of samples */

	if (nins & 0x80) {
	/* Samples are saved as delta values */
	use_delta = 1;
	}
	if (nins & 0x40) {
	/* Some samples are packed -- depacking not implemented */
	/* use_packed = 1; */
	return -1;
	}
	nins &= 0x3f;

	/* read unpacked sample data size */
	/*
	if (use_packed == 1)
	Unpacked_Sample_Data_Size = hio_read32b(in);
	*/

	pw_write_zero(out, 20);     /* write title */

	/* sample headers stuff */
	for (i = 0; i < nins; i++) {
	pw_write_zero(out, 22);     /* write sample name */

	j = isize[i] = hio_read16b(in); /* sample size */

	if (j > 0xff00) {
		smp_size[i] = smp_size[0xffff - j];
		isize[i] = isize[0xffff - j];
		saddr[i] = saddr[0xffff - j];
	} else {
			if (i > 0) {
			saddr[i] = saddr[i - 1] + smp_size[i - 1];
			}
		smp_size[i] = j * 2;
		/* ssize += smp_size[i]; */
	}
	j = smp_size[i] / 2;
	write16b(out, isize[i]);

	c1 = hio_read8(in);         /* finetune */
	/*
	if (c1 & 0x40)
	    PACK[i] = 1;
	*/
	c1 &= 0x3f;
	write8(out, c1);

	write8(out, hio_read8(in));     /* volume */

	/* loop start */
	val = hio_read16b(in);
	if (val == 0xffff) {
		write16b(out, 0x0000);
		write16b(out, 0x0001);
		continue;
	}
	write16b(out, val);
	write16b(out, j - val);
	}

	/* go up to 31 samples */
	memset(tmp, 0, 30);
	tmp[29] = 0x01;
	for (; i < 31; i++)
	fwrite(tmp, 30, 1, out);

	/* read tracks addresses per pattern */
	for (i = 0; i < npat; i++) {
	for (j = 0; j < 4; j++)
		taddr[i][j] = hio_read16b(in);
	}

	/* pattern table */
	for (len = 0; len < 128; len++) {
	c1 = hio_read8(in);
	if (c1 == 0xff)
		break;
	ptable[len] = c1;       /* <--- /2 in p50a */
	}

	write8(out, len);           /* write size of pattern list */
	write8(out, 0x7f);          /* write noisetracker byte */
	fwrite(ptable, 128, 1, out);    /* write pattern table */
	write32b(out, PW_MOD_MAGIC);    /* write ptk ID */

	if ((tdata_addr = hio_tell(in)) < 0) {
		return -1;
	}

	/* rewrite the track data */
	for (i = 0; i < npat; i++) {
	max_row = 63;
	for (j = 0; j < 4; j++) {
		hio_seek(in, taddr[i][j] + tdata_addr, SEEK_SET);
		for (k = 0; k <= max_row; k++) {
		uint8 *x = &tdata[i * 4 + j][k * 4];
		c1 = hio_read8(in);

			/* case no fxt nor fxtArg  (3 bytes) */
			if ((c1 & 0x70) == 0x70 && c1 != 0xff && c1 != 0x7F) {
			c2 = hio_read8(in);
				c6 = ((c1 << 4) & 0xf0) | ((c2 >> 4) & 0x0e);

			/* Sanity check */
			if (hio_error(in) || !PTK_IS_VALID_NOTE(c6 / 2)) {
			return -1;
			}

				*x++ = (c2 & 0x10) | (ptk_table[c6 / 2][0]);
				*x++ = ptk_table[c6 / 2][1];
				*x++ = (c2 << 4) & 0xf0;

				if (c1 & 0x80) {
			c3 = hio_read8(in);
					if (c3 < 0x80) {
						k += c3;
						continue;
					}
					c4 = c3 - 0x80;

					for (l = 0; l < c4 && k < max_row; l++) {
						k++;
				x = &tdata[i * 4 + j][k * 4];
						*x++ = (c2 & 0x10) | ptk_table[c6 / 2][0];
						*x++ = ptk_table[c6 / 2][1];
						*x++ = (c2 << 4) & 0xf0;
					}
				}
				continue;
			}
			/* end of case no fxt nor fxtArg */

			/* case no sample number nor relative note number */
			if ((c1 & 0x70) == 0x60 && c1 != 0xff) {
			c2 = hio_read8(in);
				c6 = c1 & 0x0f;
				if (c6 == 0x08)
					c1 -= 0x08;
			x += 2;
				*x++ = c1 & 0x0f;
				if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
					c2 = c2 > 0x7f ? (0x100 - c2) << 4 : c2;
				*x++ = c2;

				if (c6 == 0x0d) {   /* PATTERN BREAK, track ends */
					max_row = k;
			break;
				}
				if (c6 == 0x0B) {   /* PATTERN JUMP, track ends */
					max_row = k;
			break;
				}
				if (c1 & 0x80) {
			c3 = hio_read8(in);
					if (c3 < 0x80) {    /* bypass c3 rows */
						k += c3;
						continue;
					}
					c4 = c3 - 0x80;     /* repeat current row */
					for (l = 0; l < c4 && k < max_row; l++) {
						k++;
				x = &tdata[i * 4 + j][k * 4] + 2;
						*x++ = c1 & 0x0f;
						*x++ = c2;
					}
				}
				continue;
			}
			/* end of case no Sample number nor Relative not number */

			if ((c1 & 0x80) == 0x80 && c1 != 0xff) {
			c2 = hio_read8(in);
			c3 = hio_read8(in);
			c4 = hio_read8(in);
				c1 &= 0x7f;

			/* Sanity check */
			if (hio_error(in) || !PTK_IS_VALID_NOTE(c1 / 2)) {
			return -1;
			}

			*x++ = ((c1 << 4) & 0x10) | ptk_table[c1 / 2][0];
				*x++ = ptk_table[c1 / 2][1];
				c6 = c2 & 0x0f;
				if (c6 == 0x08)
					c2 -= 0x08;
				*x++ = c2;

				if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
					c3 = c3 > 0x7f ? (0x100 - c3) << 4 : c3;

				*x++ = c3;

				if (c6 == 0x0d) {   /* PATTERN BREAK, track ends */
					max_row = k;
			break;
				}
				if (c6 == 0x0B) {   /* PATTERN JUMP, track ends */
					max_row = k;
			break;
				}

				if (c4 < 0x80) {    /* bypass c4 rows */
					k += c4;
					continue;
				}
				c4 = c4 - 0x80;

				for (l = 0; l < c4 && k < max_row; l++) {   /* repeat row c4-0x80 times */
					k++;
			x = &tdata[i * 4 + j][k * 4];

					*x++ = ((c1 << 4) & 0x10) | ptk_table[c1 / 2][0];
					*x++ = ptk_table[c1 / 2][1];
					c6 = c2 & 0x0f;
					if (c6 == 0x08)
						c2 -= 0x08;
					*x++ = c2;

					if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
						c3 = c3 > 0x7f ? (0x100 - c3) << 4 : c3;
					*x++ = c3;
				}
				continue;
			}

			if ((c1 & 0x7f) == 0x7f) {
				if (~c1 & 0x80) {       /* bypass 1 row */
					/*k += 1; */
					continue;
				}
			c2 = hio_read8(in);
				if (c2 < 0x40) {    /* bypass c2 rows */
					k += c2;
					continue;
				}
				c2 -= 0x40;
			c3 = hio_read8(in);
				z = c3;
				if (c2 >= 0x80) {
					c2 -= 0x80;
			c4 = hio_read8(in);
					z = (c3 << 8) + c4;
				}
				if ((a = hio_tell(in)) < 0) {
						return -1;
					}
				c5 = c2;

				hio_seek(in, -z, SEEK_CUR);

				for (l = 0; l <= c5 && k <= max_row; l++, k++) {
			c1 = hio_read8(in);
			x = &tdata[i * 4 + j][k * 4];

					/* case no fxt nor fxtArg  (3 bytes) */
					if ((c1 & 0x70) == 0x70 && c1 != 0xff && c1 != 0x7f) {
				c2 = hio_read8(in);
						c6 = ((c1 << 4) & 0xf0) | ((c2 >> 4) & 0x0e);

				/* Sanity check */
				if (hio_error(in) || !PTK_IS_VALID_NOTE(c6 / 2)) {
				return -1;
				}

						*x++ = (c2 & 0x10) | ptk_table[c6 / 2][0];
						*x++ = ptk_table[c6 / 2][1];
						*x++ = (c2 << 4) & 0xf0;

						if (c1 & 0x80) {
				c3 = hio_read8(in);
							if (c3 < 0x80) {    /* bypass c3 rows */
								k += c3;
								continue;
							}
							c4 = c3 - 0x80; /* repeat row c3-0x80 times */
							for (b = 0; b < c4 && k < max_row; b++) {
								k++;
						x = &tdata[i * 4 + j][k * 4];
								*x++ = (c2 & 0x10) | ptk_table[c6 / 2][0];
								*x++ = ptk_table[c6 / 2][1];
								*x++ = (c2 << 4) & 0xf0;
							}
						}
						continue;
					}
					/* end of case no fxt nor fxtArg */

					/* case no sample number nor relative note number */
					if ((c1 & 0x60) == 0x60 && c1 != 0xff && c1 != 0x7f) {
				c2 = hio_read8(in);
						c6 = c1 & 0x0f;
						if (c6 == 0x08)
							c1 -= 0x08;

				x += 2;
						*x++ = c1 & 0x0f;

						if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
								c2 = c2 > 0x7f ? (0x100 - c2) << 4 : c2;
						*x++ = c2;

						if (c6 == 0x0d) {   /* PATTERN BREAK, track ends */
							max_row = k;
				goto brk_k;
						}
						if (c6 == 0x0b) {   /* PATTERN JUMP, track ends */
							max_row = k;
				goto brk_k;
						}

						if (c1 & 0x80) {
				c3 = hio_read8(in);
							if (c3 < 0x80) {    /* bypass c3 rows */
								k += c3;
								continue;
							}
							c4 = c3 - 0x80; /* repeat row c3-0x80 times */
							for (b = 0; b < c4 && k < max_row; b++) {
								k++;
						x = &tdata[i * 4 + j][k * 4] + 2;
								*x++ = c1 & 0x0f;
								*x++ = c2;
							}
						}
						continue;
					}
					/* end of case no sample nor relative note number */

					if ((c1 & 0x80) && c1 != 0xff && c1 != 0x7f) {
				c2 = hio_read8(in);
				c3 = hio_read8(in);
				c4 = hio_read8(in);
						c1 &= 0x7f;

				/* Sanity check */
				if (hio_error(in) || !PTK_IS_VALID_NOTE(c1 / 2)) {
				return -1;
				}

						*x++ = ((c1 << 4) & 0x10) | ptk_table[c1 / 2][0];
						*x++ = ptk_table[c1 / 2][1];

						c6 = c2 & 0x0f;
						if (c6 == 0x08)
							c2 -= 0x08;
						*x++ = c2;

						if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
								c3 = c3 > 0x7f ? (0x100 - c3) << 4 : c3;
						*x++ = c3;

						if (c6 == 0x0d) {   /* PATTERN BREAK, track ends */
							max_row = k;
				goto brk_k;
						}
						if (c6 == 0x0b) {   /* PATTERN JUMP, track ends */
							max_row = k;
				goto brk_k;
						}
						if (c4 < 0x80) {    /* bypass c4 rows */
							k += c4;
							continue;
						}
						c4 = c4 - 0x80; /* repeat row c4-0x80 times */
						for (b = 0; b < c4 && k < max_row; b++) {
							k++;
					x = &tdata[i * 4 + j][k * 4];

							*x++ = ((c1 << 4) & 0x10) |ptk_table[c1 / 2][0];
							*x++ = ptk_table[c1 / 2][1];

							c6 = c2 & 0x0f;
							if (c6 == 0x08)
								c2 -= 0x08;
							*x++ = c2;

							if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
								c3 = c3 > 0x7f ? (0x100 - c3) << 4 : c3;
							*x++ = c3;
						}
						continue;
					}
					if ((c1 & 0x7f) == 0x7f) {
						if ((c1 & 0x80) == 0x00) {  /* bypass 1 row */
							/*k += 1; */
							continue;
						}
				c2 = hio_read8(in);
						if (c2 < 0x40) {    /* bypass c2 rows */
							k += c2;
							continue;
						}
						continue;
					}

			c2 = hio_read8(in);
			c3 = hio_read8(in);

			if (hio_error(in) || !PTK_IS_VALID_NOTE(c1 / 2)) {
				return -1;
			}

					*x++ = ((c1 << 4) & 0x10) | ptk_table[c1 / 2][0];
					*x++ = ptk_table[c1 / 2][1];

					c6 = c2 & 0x0f;
					if (c6 == 0x08)
						c2 -= 0x08;
					*x++ = c2;

					if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
						c3 = c3 > 0x7f ? (0x100 - c3) << 4 : c3;
					*x++ = c3;
				}
				hio_seek(in, a, SEEK_SET);
				k -= 1;
				continue;
			}

		c2 = hio_read8(in);
		c3 = hio_read8(in);

		/* Sanity check */
		if (hio_error(in) || !PTK_IS_VALID_NOTE(c1 / 2)) {
			return -1;
		}

			*x++ = ((c1 << 4) & 0x10) | ptk_table[c1 / 2][0];
			*x++ = ptk_table[c1 / 2][1];

			c6 = c2 & 0x0f;
			if (c6 == 0x08)
				c2 -= 0x08;
			*x++ = c2;

			if (c6 == 0x05 || c6 == 0x06 || c6 == 0x0a)
			c3 = c3 > 0x7f ? (0x100 - c3) << 4 : c3;
			*x++ = c3;

			if (c6 == 0x0d) {   /* PATTERN BREAK, track ends */
				max_row = k;
			break;
			}
			if (c6 == 0x0b) {   /* PATTERN JUMP, track ends */
				max_row = k;
			break;
			}
		}
		brk_k: ;
	}
	}

	/* write pattern data */
	for (i = 0; i < npat; i++) {
	memset(tmp, 0, sizeof(tmp));
	for (j = 0; j < 64; j++) {
		for (k = 0; k < 4; k++)
		memcpy(&tmp[j * 16 + k * 4], &tdata[k + i * 4][j * 4], 4);
	}
	fwrite (tmp, 1024, 1, out);
	}

	/* go to sample data address */
	hio_seek(in, sdata_addr, SEEK_SET);

	/* read and write sample data */

	/*printf ( "writing sample data ... " ); */
	for (i = 0; i < nins; i++) {
	hio_seek(in, sdata_addr + saddr[i], 0);
	smp_buffer = (signed char *) calloc(1, smp_size[i]);
	hio_read(smp_buffer, smp_size[i], 1, in);
	if (use_delta == 1) {
		c1 = 0;
		for (j = 1; j < smp_size[i]; j++) {
			c2 = smp_buffer[j];
			c2 = 0x100 - c2;
			c3 = c2 + c1;
			smp_buffer[j] = c3;
			c1 = c3;
		}
	}
	fwrite(smp_buffer, smp_size[i], 1, out);
	free(smp_buffer);
	}

	/* if (use_delta == 1)
	pw_p60a.flags |= PW_DELTA; */

	return 0;
}

static int test_p61a(const uint8 *data, char *t, int s)
{
	int i, n;
	int len;
	int lstart;
	int npat;
	int nins;
	int pattern_data_offset;
	int sample_data_offset;
	/* int ssize; */

#if 0
	if (i < 7) {
	return -1;
	return;
	}
	start = i - 7;
#endif

	PW_REQUEST_DATA(s, 4);

	/* number of pattern (real) */
	npat = data[2];
	if (npat > 0x7f || npat == 0)
	return -1;

	/* number of sample */
	nins = (data[3] & 0x3f);
	if (nins > 0x1f || nins == 0)
	return -1;

	PW_REQUEST_DATA(s, 4 + nins * 6);

	for (i = 0; i < nins; i++) {
	/* test volumes */
	if (data[7 + i * 6] > 0x40)
		return -1;

	/* test finetunes */
	if (data[6 + i * 6] > 0x0f)
		return -1;
	}

	/* test sample sizes and loop start */
	/* ssize = 0; */
	for (i = 0; i < nins; i++) {
	len = readmem16b(data + i * 6 + 4);
	if ((len <= 0xffdf && len > 0x8000) || len == 0)
		return -1;

	/*
	if (len < 0xff00)
	    ssize += len * 2;
	*/

	lstart = readmem16b(data + i * 6 + 8);
	if (lstart != 0xffff && lstart >= len)
		return -1;

	if (len > 0xffdf) {
		if (0xffff - len > nins)
			return -1;
	}
	}

	pattern_data_offset = 4 + nins * 6 + npat * 8;

	/* test sample data address */
	sample_data_offset = readmem16b(data);
	if (sample_data_offset < pattern_data_offset)
	return -1;

	PW_REQUEST_DATA(s, pattern_data_offset);

	/* test track table */
	for (i = 0; i < npat * 4; i++) {
	int track_start = readmem16b(data + 4 + nins * 6 + i * 2);
	if (track_start + pattern_data_offset > sample_data_offset)
		return -1;
	}

	PW_REQUEST_DATA(s, sample_data_offset);

	/* test pattern table */
	for (i = 0; i < 128; i++) {
	if (pattern_data_offset + i >= sample_data_offset)
		return -1;

	if (data[pattern_data_offset + i] == 0xff)
		break;

	if (data[pattern_data_offset + i] > npat - 1)
		return -1;
	}

	if (i == 0 || i == 128)
	return -1;

	/* test notes ... pfiew */

	for (n = pattern_data_offset + i + 1; n < sample_data_offset - 1; n++) {
		uint8 d, e;

	d = data[n];
	e = data[n + 1];

	if ((d & 0xff) == 0xff) {
		if ((e & 0xc0) == 0x00) {
			n += 1;
			continue;
		}
		if ((e & 0xc0) == 0x40) {
			n += 2;
			continue;
		}
		if ((e & 0xc0) == 0xc0) {
			n += 3;
			continue;
		}
	}

	if ((d & 0xff) == 0x7f)
		continue;

	/* no fxt nor fxtArg */
	if ((d & 0xf0) == 0xf0) {
		if ((e & 0x1f) > nins)
			return -1;
		n += 2;
		continue;
	}

	if ((d & 0xf0) == 0x70) {
		if ((e & 0x1f) > nins)
			return -1;
		n += 1;
		continue;
	}

	/* no note nor Sample number */
	if ((d & 0xf0) == 0xe0) {
		n += 2;
		continue;
	}

	if ((d & 0xf0) == 0x60) {
		n += 1;
		continue;
	}

	if ((d & 0x80) == 0x80) {
		if ((((d << 4) & 0x10) | ((e >> 4) & 0x0f)) > nins)
			return -1;
		n += 3;
		continue;
	}

	if ((((d << 4) & 0x10) | ((e >> 4) & 0x0F)) > nins)
		return -1;
	n += 2;
	}

	return 0;
}

#undef fwrite

/* ---- what the rest of the project calls ------------------------- *
 *
 * P61A_Test says whether a buffer looks like The Player 6.1a. It
 * returns 0 for yes; anything else is no, and a positive value is
 * how many more bytes libxmp's version would have asked for, which
 * for a module we already hold whole is just another rejection.
 *
 * P61A_Depack turns one into a ProTracker module in a buffer the
 * caller frees. Returns 0 on success.
 */
int P61A_Test(const unsigned char *data, long size)
{
	char title[32];

	if (!data || size < 8) return 1;
	if (size > 0x10000L) size = 0x10000L;
	return test_p61a((const uint8 *)data, title, (int)size);
}

int P61A_Depack(const unsigned char *data, long size,
                unsigned char **out_data, long *out_size)
{
	P61_IN in;
	P61_OUT out;
	int rc;

	if (out_data) *out_data = 0;
	if (out_size) *out_size = 0;
	if (!data || size <= 0 || !out_data || !out_size) return -1;

	in.data = (const uint8 *)data;
	in.size = size;
	in.pos = 0;
	in.err = 0;

	out.data = 0;
	out.len = 0;
	out.cap = 0;
	out.err = 0;

	rc = depack_p61a(&in, &out);
	if (rc != 0 || out.err || out.len <= 0) {
		free(out.data);
		return rc ? rc : -1;
	}
	*out_data = out.data;
	*out_size = out.len;
	return 0;
}
