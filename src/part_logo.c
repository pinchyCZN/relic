/* RELIC scenes 2.2, 2.6 and 2.9 - the glowing text screens.
 *
 * ORIGIN 0x0030ffd4 Logo_Run.
 *
 * Four steps a frame: a height field relaxes, the line art is drawn
 * into a source buffer scaled by 1/frame, a shading pass refracts a
 * colour ramp through the field's gradient, and a c2p pushes it to
 * six bitplanes. The c2p is a chipset detail with no counterpart
 * here - the chunky buffer IS the picture - so the other three are
 * what this file carries.
 *
 * See docs/parts.txt section 3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parts.h"
#include "gfx.h"
#include "image.h"

#define A_VARIANTS  0x00310c0cUL    /* g_LogoVariants, 3 x 0x10     */
#define A_PALETTES  0x00310c3cUL    /* g_LogoPalettes, 0x400 each   */
#define A_LINE_ART  0x0031183cUL    /* g_LogoLineArt                */

#define ART_ROWB    66              /* a line is 528 x 16           */
#define ART_H       16
#define ART_COLS    16              /* dbf 0xf in Logo_StampLine    */

/* ORIGIN 0x0031033e. The field's row stride is 0x500 bytes - 320
 * longwords - and 0x3554 iterations of three cells is 40,959. */
#define FIELD_W     320
#define FIELD_H     128
#define FIELD_N     (FIELD_W * FIELD_H)
/* OURS - the flat stencil reads 321 cells past both ends, which in
 * the demo is pool memory nobody has read. Zero here. */
#define FIELD_PAD   (FIELD_W + 1)

/* ORIGIN 0x0031025e. Logo_StampLine walks a 0x100-byte stride and
 * 16 columns of 528, so the stamped buffer is 256 wide and at least
 * 528 tall. */
#define STAMP_STRIDE 0x100
#define STAMP_ROWS   560
#define STAMP_BYTES  (STAMP_STRIDE * STAMP_ROWS)

static s32 g_variant;
static u32 g_lines[3];
static int g_nlines;

static s32 *g_cur;          /* the height field, double buffered   */
static s32 *g_prev;
static s32 *g_field_base[2];
static u8  *g_stamp;        /* what Logo_StampLine writes          */
static u8  *g_art;          /* what the zoom writes, shader reads  */
static u8  *g_chunky[2];    /* the shader's own double buffer      */
static int  g_front;
/* ORIGIN 0x0031029e  Logo_BuildShadeRamp - 1024 of min(i, 0x3f). */
#define SHADE_RAMP_N 1024
static u8 g_shade_ramp[SHADE_RAMP_N];
static u8 g_pal[64][3];
static u8 g_pal_now[64][3];

/* ORIGIN 0x00310a58 - THE SCENE FADES THROUGH ITS PALETTE, not
 * through the picture, and the two counters Logo_Advance runs pull
 * opposite ways:
 *
 *     c = (c * g_PalFadeScale) >> 8         0x00310be2, from 0xff
 *     c = min(0xff, c + g_PalFadeOffset)    0x00310be4, from 0xfe
 *
 * parts.txt section 3. */
#define LOGO_FIELDS      0x1d6L    /* 0x003100be */
#define FADE_WHITE_0     0xfeL     /* 0x0031009c */
#define FADE_WHITE_RATE  2L        /* 0x00310b28 */
#define FADE_SCALE_0     0xffL     /* 0x00310094 */
#define FADE_SCALE_RATE  5L        /* 0x00310b68 */
#define FADE_OUT_AT      0x40L     /* 0x00310b50 */

/* ORIGIN 0x003100be  Logo_SetupVariant - the per-variant pace.
 *
 * TWO COUNTERS, AND THEY ARE EASY TO SWAP. 0x00310bcc STARTS at
 * 0x32 or 0x96 and GROWS; 0x00310b90 is a FIXED limit of 0x348,
 * 0x7d0 or 0xaf0 that never moves. Logo_Advance steps the first by
 * a per-variant rate and both the zoom and the injection test
 * against the second. Reading them the other way round leaves the
 * text permanently at its opening magnification. */
static s32 counter_start(void) { return (g_nlines == 1) ? 0x32L : 0x96L; }
/* ORIGIN 0x003100be */
static s32 counter_rate(void)
{
	return (g_nlines == 1) ? 6L : (g_nlines == 2) ? 0xeL : 0x18L;
}
/* ORIGIN 0x003100be */
static s32 counter_limit(void)
{
	return (g_nlines == 1) ? 0x348L : (g_nlines == 2) ? 0x7d0L : 0xaf0L;
}

/* ORIGIN 0x00310b18  Logo_Advance - the counter at tick `t`. */
static s32 counter_at(s32 t)
{
	s32 c;
	if (t < 0) t = 0;
	c = counter_start() + t * counter_rate();
	if (c > counter_limit()) c = counter_limit();
	return c;
}

/* ORIGIN 0x003100be */
static s32 zoom_numer(void)
{
	return (g_nlines == 1) ? 0x3e800L
	     : (g_nlines == 2) ? 0xbb800L : 0x177000L;
}

/* ORIGIN 0x003100be */
static s32 zoom_xterm(void)
{
	return (g_nlines == 3) ? 0x20e0000L : 0x20a0000L;
}

/* ORIGIN 0x003100be */
static s32 zoom_yterm(void)
{
	return (g_nlines == 1) ? 0x14400L
	     : (g_nlines == 2) ? 0x16800L : 0x21c00L;
}

/* ORIGIN 0x003100be - which column each line is stamped at. The
 * LAST offset goes to the highest column, so a three-line screen
 * reads the third entry downward. */
static const s32 START_COL[4][3] = {
	{ 0, 0, 0 }, { 0x10, 0, 0 }, { 0x28, 0x10, 0 }, { 0x40, 0x28, 0x10 }
};

/* OURS - reads the 3 x 16-byte table into something easier to hold.
 * A ZERO PAST THE FIRST MEANS UNUSED, and variant 0's entry is all
 * zeros because its one line really does sit at offset 0. */
static void read_variant(s32 v)
{
	u32 base = A_VARIANTS + (u32)v * 0x10UL;
	u32 a = Image_U32(base), b = Image_U32(base + 4), c = Image_U32(base + 8);
	int i;

	g_nlines = 1;
	g_lines[0] = a;
	if (b) g_lines[g_nlines++] = b;
	if (c) g_lines[g_nlines++] = c;

	/* ORIGIN 0x00310a36 Logo_SetPalette - 64 records of 0x10 bytes,
	 * first longword of each. EVERY SCREEN HAS ITS OWN 64 COLOURS. */
	for (i = 0; i < 64; i++) {
		u32 col = Image_U32(A_PALETTES + (u32)v * 0x400UL + (u32)i * 0x10UL);
		g_pal[i][0] = (u8)((col >> 16) & 0xff);
		g_pal[i][1] = (u8)((col >> 8) & 0xff);
		g_pal[i][2] = (u8)(col & 0xff);
	}
}

/* ORIGIN 0x00310b18  Logo_Advance - the palette at field `tick`.
 * The white term is stepped from the first field, so the scene
 * OPENS WHITE and the text fades up out of it; the scale is held
 * until the scene's countdown is inside its last 0x40 fields, so it
 * only ever fades to black at the end. */
static void logo_fade(s32 tick)
{
	s32 white = FADE_WHITE_0 - FADE_WHITE_RATE * tick;
	s32 steps = tick - (LOGO_FIELDS - FADE_OUT_AT - 1L);
	s32 scale = FADE_SCALE_0;
	s32 i, k;

	if (white < 0) white = 0;
	if (steps > 0) scale -= FADE_SCALE_RATE * steps;
	if (scale < 0) scale = 0;
	for (i = 0; i < 64; i++)
		for (k = 0; k < 3; k++) {
			s32 c = (((s32)g_pal[i][k] * scale) >> 8) + white;
			g_pal_now[i][k] = (u8)(c > 0xffL ? 0xffL : c);
		}
}

/* ORIGIN 0x0031025e  Logo_StampLine. IT TRANSPOSES - the art is
 * row-major 528 x 16 and this lays it out as 16 columns of 528. A
 * set bit is the letter; the running average with the cell above is
 * the seed glow. docs/parts.txt section 3. */
static void stamp_line(u32 offset, s32 start_col)
{
	const u8 *src = Image_At(A_LINE_ART + offset);
	s32 col = start_col;
	int c, b, k;

	if (!src) return;
	for (c = 0; c < ART_COLS; c++) {
		s32 y = 0;
		col--;
		if (col < 0) break;
		for (b = 0; b < ART_ROWB; b++) {
			u8 d0 = *src++;
			for (k = 0; k < 8; k++) {
				s32 v = (d0 & 0x80) ? 0x4f : 0;
				s32 above = (y > 0)
				          ? g_stamp[(y - 1) * STAMP_STRIDE + col] : 0;
				d0 = (u8)(d0 << 1);
				if (y < STAMP_ROWS)
					g_stamp[y * STAMP_STRIDE + col] = (u8)((v + above) >> 1);
				y++;
			}
		}
	}
}

/* OURS - every line of the current variant. */
static void stamp_all(void)
{
	int l;
	memset(g_stamp, 0, (size_t)STAMP_BYTES);
	for (l = 0; l < g_nlines; l++)
		stamp_line(g_lines[l], START_COL[g_nlines][l]);
}

/* ORIGIN 0x0031033e  Logo_DiffuseField - one relaxation step.
 *
 * ONE FLAT LOOP OVER 40,959 CELLS, NOT A LOOP OVER ROWS: the
 * stencil's +-1, +-320 and +-321 are flat offsets, so every border
 * cell relaxes like any other. Confine it to the interior and they
 * drift undamped under Logo_ShadeRefraction's feedback into a
 * glowing fringe down both edges. docs/parts.txt section 3. */
static void diffuse(s32 frame)
{
	const s32 inject = (counter_at(frame) < counter_limit()) ? 0x28L : 0L;
	s32 *t;
	s32 n;

	for (n = 0; n < FIELD_N - 1; n++) {
		const s32 *c = g_cur + n;
		s32 corners = c[-FIELD_W - 1] + c[-FIELD_W + 1]
		            + c[FIELD_W - 1] + c[FIELD_W + 1];
		s32 edges = c[-FIELD_W] + c[FIELD_W] + c[-1] + c[1];
		s32 v = ((corners >> 6) + edges) >> 1;
		v -= g_prev[n];
		v -= v >> 4;
		v += inject;
		g_prev[n] = v;
	}
	t = g_cur; g_cur = g_prev; g_prev = t;
}

/* ORIGIN 0x00310576  Logo_DrawZoomedText - the art, scaled by time.
 *
 * THE ZOOM IS A DIVIDE BY g_LogoCounter, which Logo_Advance steps
 * by a per-variant rate, so the text starts enormous and shrinks as
 * the counter climbs - and it clamps at g_LogoZoomLimit, the same
 * test the injection uses, so zoom and ripple settle together.
 *
 * It draws outward from the centre: the demo's 0x50a0 into a
 * 320-wide buffer is row 64, column 160. The source index is
 * (x & ~0xff) | y - the stamped buffer read back TRANSPOSED, which
 * is why Logo_StampLine laid it out in columns. */
static void draw_zoomed_text(s32 frame)
{
	s32 d7, d3, d4, d6, d2, d1, a3, span;
	s32 row, col, r, c;

	memset(g_art, 0, (size_t)FIELD_N);
	d2 = counter_at(frame);
	if (d2 < 1) return;

	d7 = zoom_numer() / d2;
	if (d7 < 1) d7 = 1;
	a3 = 0;
	d3 = zoom_xterm() / d7;
	d4 = zoom_yterm() / d7;

	d6 = (0x1fL * d7) >> 8;
	if (d6 > 0x7f) d6 = 0x7f;
	d6 -= 1;
	if (d6 < 0) return;

	/* The vertical origin: D7/b98 past 0x40 is carried into the y
	 * accumulator instead of the row, which is what lets the text
	 * run off the top without the loop changing shape. */
	d1 = 0;
	d2 = d7 / ((g_nlines == 1) ? 0x28L : 0x10L);
	if (d2 >= 0x40) {
		d1 = (d2 - 0x40) * d4;
		d2 = 0x3f;
	}
	row = FIELD_H / 2 + d2;

	/* The same trick horizontally, against half the screen. */
	d2 = d7 >> 1;
	span = d7;
	if (d2 >= 0xa0) {
		a3 = (d3 * (d2 - 0xa0)) >> 8;
		d2 = 0xa0;
		span = FIELD_W;
	}
	d3 >>= 8;
	col = FIELD_W / 2 - d2;

	span >>= 2;
	if (span < 1) return;
	span *= 4;
	d6 += 4;

	/* IT WALKS UP. A0 drops by one row each pass, and the source
	 * index is (x & ~0xff) | y - the stamped buffer read back
	 * TRANSPOSED, so the screen's X is the art's long axis and the
	 * screen's Y is its 16-column one. */
	for (r = 0; r < d6; r++) {
		s32 y = row - r;
		s32 x_acc = a3;
		s32 ycol = (d1 >> 8) & 0xffL;
		if (y >= 0 && y < FIELD_H) {
			for (c = 0; c < span; c++) {
				s32 px = col + c;
				s32 idx = (x_acc & ~0xffL) | ycol;
				if (px >= 0 && px < FIELD_W
				        && idx >= 0 && idx < STAMP_BYTES)
				    g_art[y * FIELD_W + px] = g_stamp[idx];
				x_acc += d3;
			}
		}
		d1 += d4;
	}
}

/* ORIGIN 0x003104a4  Logo_ShadeRefraction.
 *
 *     d   = (chunky_other + art) >> 3
 *     i   = (field >> 5) + d            ; a BYTE add
 *     out = ramp[i]
 *     field -= (d - art)
 *
 * TWO SOURCES MAKE THE GRADIENT, and it both indexes the ramp and
 * FEEDS BACK into the field - the field is modified here, not only
 * read, which is what keeps the surface moving after the injection
 * stops. */
static void shade(u8 *out, const u8 *other)
{
	s32 i;
	for (i = 0; i < FIELD_N; i++) {
		s32 a = g_art[i];
		s32 base = g_cur[i] >> 5;
		s32 d = (((s32)other[i] + a) >> 3) & 0xff;
		/* `add.b` carries only INSIDE the low byte, and the index is
		 * then taken as a WORD - which is why the ramp is 1024 long
		 * and not 256. */
		s32 idx = (base & ~0xffL) | ((base + d) & 0xffL);
		if (idx < 0) idx = 0;
		if (idx >= SHADE_RAMP_N) idx = SHADE_RAMP_N - 1;
		out[i] = g_shade_ramp[idx];
		g_cur[i] -= (d - a);
	}
}

/* OURS - the part's own setup. */
static int logo_init(void)
{
	read_variant(g_variant);
	if (!g_cur) {
		g_field_base[0] = (s32 *)calloc((size_t)FIELD_N
		                               + 2 * FIELD_PAD, sizeof(s32));
		g_field_base[1] = (s32 *)calloc((size_t)FIELD_N
		                               + 2 * FIELD_PAD, sizeof(s32));
		g_cur = g_field_base[0] ? g_field_base[0] + FIELD_PAD : NULL;
		g_prev = g_field_base[1] ? g_field_base[1] + FIELD_PAD : NULL;
		g_art = (u8 *)calloc((size_t)FIELD_N, 1);
		g_chunky[0] = (u8 *)calloc((size_t)FIELD_N, 1);
		g_chunky[1] = (u8 *)calloc((size_t)FIELD_N, 1);
		g_stamp = (u8 *)calloc((size_t)STAMP_BYTES, 1);
		if (!g_cur || !g_prev || !g_art || !g_chunky[0] || !g_chunky[1]
		        || !g_stamp) {
			fprintf(stderr, "logo: out of memory\n");
			return 1;
		}
	}
	/* ORIGIN 0x0031029e  Logo_BuildShadeRamp */
	{
		s32 i, v = 0;
		for (i = 0; i < SHADE_RAMP_N; i++) {
			g_shade_ramp[i] = (u8)v;
			if (++v >= 0x40) v = 0x3f;
		}
	}

	memset(g_cur, 0, (size_t)FIELD_N * sizeof(s32));
	memset(g_prev, 0, (size_t)FIELD_N * sizeof(s32));
	memset(g_art, 0, (size_t)FIELD_N);
	memset(g_chunky[0], 0, (size_t)FIELD_N);
	memset(g_chunky[1], 0, (size_t)FIELD_N);
	stamp_all();
	g_front = 0;
	return 0;
}

/* OURS - the frame loop, and the palette lookup the c2p stands in
 * for. The demo pushes the chunky buffer through six bitplanes to a
 * 64-colour screen; with no chipset here the index is simply looked
 * up, which is the same picture by a shorter road. */
static void logo_frame(Relic_Frame *f, s32 tick)
{
	const u8 *src;
	s32 y, x;

	diffuse(tick);
	draw_zoomed_text(tick);
	shade(g_chunky[g_front], g_chunky[g_front ^ 1]);
	src = g_chunky[g_front];
	g_front ^= 1;
	logo_fade(tick);

	Frame_Clear(f);
	for (y = 0; y < FIELD_H; y++) {
		for (x = 0; x < FIELD_W; x++) {
			const u8 *col = g_pal_now[src[y * FIELD_W + x] & 0x3f];
			s32 px, py;
			for (py = FX_MUL(y, f->scale);
			     py < FX_MUL(y + 1, f->scale) && py < f->h; py++) {
				if (py < 0) continue;
				for (px = FX_MUL(x, f->scale);
				     px < FX_MUL(x + 1, f->scale) && px < f->w; px++) {
					u8 *q;
					if (px < 0) continue;
					q = f->px + ((size_t)(py * f->w) + (size_t)px) * PX_BYTES;
					q[PX_R] = (u8)(col[0] >> 2);
					q[PX_G] = (u8)(col[1] >> 2);
					q[PX_B] = (u8)(col[2] >> 2);
				}
			}
		}
	}
}

/* OURS - so the field can be checked whatever the picture shows. */
static void logo_stats(char *out, int cap)
{
	s32 i, lo = 0x7fffffffL, hi = -0x7fffffffL;
	double sum = 0.0;
	(void)cap;
	for (i = 0; i < FIELD_N; i++) {
		s32 v = g_cur[i];
		if (v < lo) lo = v;
		if (v > hi) hi = v;
		sum += (double)v;
	}
	sprintf(out, "field %ld..%ld mean %ld   variant %ld, %d line(s)",
	        (long)lo, (long)hi, (long)(sum / (double)FIELD_N),
	        (long)g_variant, g_nlines);
}

/* OURS - the viewer steps the three variants. */
void Logo_SetVariant(s32 v)
{
	g_variant = ((v % 3) + 3) % 3;
	logo_init();
}

/* OURS */
s32 Logo_Variant(void) { return g_variant; }

/* OURS */
int Logo_LineCount(void) { return g_nlines; }

/* OURS. EVERY LOGO PART MUST NAME ITS VARIANT. g_variant is module
 * state and logo_init only reads it, so a part that leaves it alone
 * inherits whichever screen ran last - and 2.2 then shows 2.6's
 * letters, with "nerve axis" never appearing at all. */
static int logo1_init(void)
{
	g_variant = 0;
	return logo_init();
}

const Relic_Part PART_Logo = {
	"2.2", "Logo screens", "Logo_Run 0x0030ffd4 D0=0",
	PART_SHAPED_ONLY, FIELD_W, FIELD_H, logo1_init, 0, logo_frame,
	logo_stats,
	/* ORIGIN 0x003100be - Logo_SetupVariant seeds 0x1d6 and the logo's
	 * own VBL hook takes one off it a blank; Logo_MainLoop runs while
	 * it is positive. SO ALL THREE LOGO PARTS ARE 470 TICKS, nine and
	 * a half seconds, whatever variant they draw. */
	LOGO_FIELDS
};

/* ORIGIN parts.txt section 3 - THE SAME EFFECT, A DIFFERENT VARIANT.
 * SpaceModule's table calls Logo_Run with D0 = 0, 1 and 2 for parts
 * 2.2, 2.6 and 2.9, and the variant index is all that differs: it
 * selects up to three line-art offsets out of g_LogoVariants AND its
 * own 64-colour palette, which is why these screens have different
 * letters and different colour but no different code. The second one
 * follows the planet scenes.
 *
 * ONE PART'S WORTH OF STATE SERVES BOTH, because logo_init re-reads
 * the variant and Demo_SetResolution calls init on every part switch.
 * So this is a second entry, not a second implementation. */
static int logo2_init(void)
{
	g_variant = 1;
	return logo_init();
}

const Relic_Part PART_Logo2 = {
	"2.6", "Logo screens, the second", "Logo_Run 0x0030ffd4 D0=1",
	PART_SHAPED_ONLY, FIELD_W, FIELD_H, logo2_init, 0, logo_frame,
	logo_stats, LOGO_FIELDS
};

/* OURS */
static int logo3_init(void)
{
	g_variant = 2;
	return logo_init();
}

const Relic_Part PART_Logo3 = {
	"2.9", "Logo screens, the third", "Logo_Run 0x0030ffd4 D0=2",
	PART_SHAPED_ONLY, FIELD_W, FIELD_H, logo3_init, 0, logo_frame,
	logo_stats, LOGO_FIELDS
};
