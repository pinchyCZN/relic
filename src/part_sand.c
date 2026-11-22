/* RELIC part 2.13 - the sand. Sand_RunScene 0x003132a4, the first
 * thing act 2 shows.
 *
 * An Aztec stone relief under a sheet of sand that blows away across
 * the shot, with lightning flickering somewhere off the horizon. It is
 * not voxels and not a texture copy: one byte per pixel over
 * 320 x 128, a height map deciding where material still stands, and
 * an accumulator that advects brightness leftward a frame at a time.
 *
 * THE OUTPUT BYTE IS A PALETTE INDEX, one to one - the display is
 * eight bitplanes and the palette is 256 entries off disk, so
 * nothing here invents a colour. See docs/effects/sand.txt.
 *
 * TWO DIVERGENCES, BOTH DELIBERATE.
 *
 * THE FILTER RUNS AT THE DEMO'S 320 x 128 AND THE RESULT IS
 * RESAMPLED ON THE WAY OUT. Its feedback is sequential and its
 * constants are spatial frequencies in demo pixels - the eight-column
 * advection, the 0xa decay per pixel, the left-neighbour tap. Scaling
 * that domain would change the effect rather than render it larger,
 * which is what relic.h's rule forbids. Scale 1.0 is therefore exact
 * and every other scale is an upscale of the demo's own buffer.
 *
 * READS BEFORE A BUFFER'S START RETURN ZERO. The demo's shader and
 * renderer index h[i-1], h[i-320] and out[i-1] without guarding i,
 * so at the first pixel and the first row they read whatever the
 * allocator left in front of the block. That is unreachable in C and
 * would be undefined anyway; it costs row 0 and column 0.
 */

#include <stdio.h>
#include <stdlib.h>
#include "parts.h"
#include "gfx.h"
#include "dat.h"
#include "image.h"

/* The music cue the scene ends on - armed at 0x0031388a, polled at
 * 0x003138a6. parts.h Relic_Gate. */
#define SA_CUE_ORDER 0x01L
#define SA_CUE_ROW   0x10L
#define SA_TICKS (SA_FADE_AT + SA_FADE_TICKS + 1L)
static Relic_Gate g_cue_gate;

/* ORIGIN the sizes at 0x00313f50 and the geometry they encode. */
#define SA_W        320L
#define SA_H        128L
#define SA_PIXELS   (SA_W * SA_H)

/* ORIGIN 0x003138de Sand_RenderFrame - A5 walks 320 words a row
 * then skips 0x20, so the stride is 336 and 128 * 336 * 2 is 86016, the
 * allocated size exactly. The pad is what lets the acc[i + 8] read
 * run past a row's end without reaching the next. */
#define SA_ACC_STRIDE  336L

/* ORIGIN 0x00313f50 slot 5 - 0x14280 bytes, every word of which
 * Sand_BuildWave fills and Sand_RenderFrame reads. */
#define SA_WAVE_WORDS  (0x14280L / 2L)

/* ORIGIN 0x00313394 - 8192 longwords, one sine period's worth. */
#define SA_WAVETAB     0x2000L

/* ORIGIN 0x0021fa94 Dat_LoadSplit with the sizes at 0x00313f14 -
 * Frelicdata is three concatenated parts and the sum is its size on
 * disk exactly. The first 0x400 of part 1 is the palette. */
#define SA_PART1       0xa400L
#define SA_PART2       0x2c0L
#define SA_PART3       0xa000L
#define SA_PAL_BYTES   0x400L
#define SA_PAL_ENTRIES 256L

/* ORIGIN 0x003134c4 Sand_CarveStamp - 32 columns over 22 rows is
 * 704 bytes, which is part 2's size exactly. */
#define SA_STAMP_W     32L
#define SA_STAMP_H     22L

/* ORIGIN 0x00313582 - g_SinTable, 8192 longwords of 14-bit sine. */
#define SA_SIN         A_SIN_TABLE

/* ORIGIN 0x003132fa Sand_Init, its last two stores - the seeds. */
#define SA_DELAY0      0xfaL
#define SA_FUSE0       0x270fL

/* ORIGIN 0x00313dde Sand_Tick - the rates. */
#define SA_DELAY_GATE  0xa0L
#define SA_LEVEL_CAP   0x250L
#define SA_PHASE_STEP  8L
#define SA_FADE_STEP   2L
#define SA_FADE_FULL   0xffL
#define SA_THUNDER_AT  0xc8L
#define SA_FLASH_AMT   0x28L
#define SA_CURTAIN_MAX 0x3fL

/* ORIGIN 0x00313830 Sand_RunShots - the exit. The cue drops
 * g_SandFade to 0xfe and Sand_Tick takes 2 a tick, so the fade is 127
 * ticks and the scene ends when it reaches zero. The cue tick and the
 * warm-up are MEASURED - sand.txt 11. */
#define SA_WARMUP      4L
#define SA_FADE_AT     (831L - SA_WARMUP)
#define SA_FADE_RUN    0xfeL
#define SA_FADE_TICKS  (SA_FADE_RUN / SA_FADE_STEP)

/* ORIGIN 0x0031398a Sand_UpdateCopper - the window opens about line
 * 0x9e and the plane base starts 0x41 rows in, so the reveal is
 * centred on buffer row 0x41 and grows two rows a tick. */
#define SA_CURTAIN_MID 0x41L

/* ORIGIN 0x003138de Sand_RenderFrame. */
#define SA_LEVEL_BIAS  0x1400L
#define SA_LEVEL_SLOPE 0x10L
#define SA_ACC_LOOK    8L
#define SA_ACC_DECAY   0xaL
#define SA_ACC_GAIN    8L

/* ORIGIN 0x00313552 Sand_RandBelow. Sand_Init clears its state to
 * ZERO before the terrain is built, so the dunes are reproducible. */
#define SA_LCG_MUL     0x11dL
#define SA_LCG_ADD     0x129L
#define SA_NOISE_RANGE 0x12cL
#define SA_WAVE_RANGE  0x14L

/* ORIGIN 0x00313582 - the per-pixel and per-row increments. */
static const s32 SA_COL_STEP[8] = { 0xf, 0x11, 0x13, 4, 0x10, 0x20, 0x19, 7 };
static const s32 SA_ROW_STEP[8] = { 0x19, 0xf, 0xc, 0x11, 8, 0xb, 0xf, 9 };

/* ORIGIN 0x00313442 Sand_StampFeatures - byte offsets into a word
 * map, so the row and column are offset / 2 split by 320. */
static const s32 SA_STAMP_AT[8] = {
	0xe56L, 0x3182L, 0x6368L, 0x7f1aL,
	0xa6fcL, 0xca2eL, 0xfc12L, 0x12e3eL
};

/* OURS - the allocated buffers, in the demo's order. */
static s16 *g_height;
static u8  *g_shade;
static s16 *g_acc;
static u8  *g_out;
static u8  *g_prev;
static u16 *g_wave;
static s32 *g_wavetab;

/* OURS - Frelicdata, split as Dat_LoadSplit splits it. */
static const u8 *g_pal_src;     /* part 1 + 0,     256 longwords   */
static const u8 *g_source;      /* part 1 + 0x400, 320 x 128 bytes */
static const u8 *g_stamp;       /* part 2,         32 x 22 bytes   */
static const u8 *g_wind;        /* part 3,         320 x 128 bytes */

/* OURS - the state Sand_Tick walks. */
static s32 g_rng;
static s32 g_level, g_delay, g_fade, g_flash, g_flash_ticks;
static s32 g_phase, g_curtain, g_fuse, g_running, g_tick;
static u8  g_pal[256 * 3];      /* 6-bit, rebuilt every tick       */
static int g_ready;

/* ORIGIN 0x00313552 Sand_RandBelow - the sand's own LCG, not the one
 * in gfx.c. Sand_Init clears the state before any terrain is built,
 * SO THE DUNES ARE THE SAME EVERY RUN: the whole build consumes this
 * sequence from zero in a fixed order, and the height map it makes
 * matches the running demo word for word [observed, sand.txt 11]. */
static s32 sand_rand_below(s32 range)
{
	s32 h;

	g_rng = (s32)((u32)g_rng * (u32)SA_LCG_MUL + (u32)SA_LCG_ADD);
	h = (s32)((((u32)g_rng >> 16) ^ (u32)g_rng) & 0xffffUL);
	return (s32)(((u32)range * (u32)h) >> 16);
}

/* OURS - g_SinTable out of the image, by the address the plate
 * comments quote. */
static s32 sand_sin(s32 i)
{
	return (s32)Image_U32((u32)(SA_SIN + (u32)(i * 4L)));
}

/* ORIGIN 0x00313394 Sand_BuildWaveTable - sine under a QUARTIC
 * envelope. The ramp runs TWICE over the table, so this holds two
 * windowed bursts rather than one period of anything, and a waveform
 * flat over most of its span and spiked at the end is what gives the
 * dunes long smooth stretches broken by sharp ridges. */
static void sand_build_wave_table(void)
{
	s32 i;

	for (i = 0; i < SA_WAVETAB; i++) {
		s32 ramp = ((SA_WAVETAB - 1L - i) & 0xfffL) ^ 0xfffL;
		s32 v = sand_sin(i);

		v = (s32)((u32)v * (u32)ramp); v >>= 8; v >>= 4;
		v = (s32)((u32)v * (u32)ramp); v >>= 8; v >>= 4;
		v = (s32)((u32)v * (u32)ramp); v >>= 8; v >>= 4;
		v = (s32)((u32)v * (u32)ramp); v >>= 8; v >>= 2;
		g_wavetab[i] = v;
	}
}

/* ORIGIN 0x00313582 Sand_BuildHeightSine and 0x003136b6
 * Sand_BuildHeightTable - ONE routine, and the only difference is
 * which table the COLUMN bank reads. `col_tab` NULL means g_SinTable,
 * as the first pass uses; the second hands in g_pSandWaveTable.
 *
 * THE COLUMN BANK IS RESEEDED FROM THE ROW BANK EVERY ROW, which is
 * the whole of the 2D coupling - the row bank advances once a row and
 * the column bank then walks away from wherever it left off.
 *
 * AND THE >>1 AFTER EVERY ADD IS WHAT SHAPES IT: each oscillator
 * halves what came before, so the last in each bank dominates and the
 * earlier ones survive only as finer ripple. A cascade, not a sum,
 * and the two sets of increments are the spatial frequencies.
 */
static void sand_build_height(const s32 *col_tab)
{
	s32 row[8], col[8];
	s32 x, y, k;
	s16 *h = g_height;

	for (k = 0; k < 8; k++) row[k] = 0L;
	for (y = 0; y < SA_H; y++) {
		for (k = 0; k < 8; k++) col[k] = row[k];
		for (x = 0; x < SA_W; x++) {
			s32 a = 0L, b = 0L, v;

			for (k = 0; k < 8; k++) {
				s32 i = (col[k] << 3) & 0x1fffL;

				a += col_tab ? col_tab[i] : sand_sin(i);
				a >>= 1;
			}
			for (k = 0; k < 8; k++) {
				s32 i = (row[k] << 3) & 0x1fffL;

				b += sand_sin(i);
				b >>= 1;
			}
			v = b + a;
			v += sand_rand_below(SA_NOISE_RANGE);
			v >>= 4;
			v += 0x800L;
			*h++ = (s16)v;
			for (k = 0; k < 8; k++) col[k] += SA_COL_STEP[k];
		}
		for (k = 0; k < 8; k++) row[k] += SA_ROW_STEP[k];
	}
}

/* ORIGIN 0x003134c4 Sand_CarveStamp - SUBTRACTS a 32 x 22 shape out
 * of the height map, clamped at zero. It is Sand_ErodeHeight's
 * arithmetic at twice the scale with no threshold, so every nonzero
 * stamp byte takes height away and none adds any. */
static void sand_carve_stamp(s32 at)
{
	const u8 *s = g_stamp;
	s32 r, c;

	for (r = 0; r < SA_STAMP_H; r++) {
		s32 base = at + r * SA_W;

		for (c = 0; c < SA_STAMP_W; c++, s++) {
			s32 i = base + c;
			s32 v, hv;

			if (i < 0L || i >= SA_PIXELS) continue;
			v = ((s32)s[0] + (s32)s[1]) << 3;
			hv = (s32)g_height[i] - v;
			g_height[i] = (s16)(hv < 0L ? 0L : hv);
		}
	}
}

/* ORIGIN 0x00313442 Sand_StampFeatures - the one shape at eight
 * places, all in the right-hand quarter and stepping rightward as
 * they descend. Sand_Init runs this twice, so each site is carved to
 * double depth. The bare slab is these. */
static void sand_stamp_features(void)
{
	s32 n;

	for (n = 0; n < 8; n++) sand_carve_stamp(SA_STAMP_AT[n] / 2L);
}

/* ORIGIN 0x0031350e Sand_ShadeFromHeight - lit from its own slope,
 * and there is no light vector anywhere: the sum of the two slopes IS
 * the lambert term, biased to mid-grey, which is why the dunes read
 * as lit from a fixed corner. The walk is linear, not per row, so a
 * row's first pixel differences against the previous row's last. */
static void sand_shade_from_height(void)
{
	s32 i;

	for (i = 0; i < SA_PIXELS; i++) {
		s32 hc = (s32)g_height[i];
		s32 hl = i >= 1L   ? (s32)g_height[i - 1L]   : 0L;
		s32 hu = i >= SA_W ? (s32)g_height[i - SA_W] : 0L;
		s32 v = ((s32)(s16)(hc - hl)) >> 1;

		v += ((s32)(s16)(hc - hu)) >> 1;
		v += 0x80L;
		if (v < 0L) v = 0L;
		if (v > 0xffL) v = 0xffL;
		g_shade[i] = (u8)v;
	}
}

/* ORIGIN 0x003133fc Sand_ErodeHeight - THE WIND FIELD IS A RATE, NOT
 * A MASK. It averages each byte with its right neighbour, scales by
 * four and subtracts 0xa0, so a byte above 20 EATS height and one
 * below 20 ADDS to it: sand is carried off where the field is high
 * and piles up where it is low. Floored at zero and never capped, so
 * ground once stripped stays stripped while a sheltered pixel keeps
 * gaining. */
static void sand_erode_height(void)
{
	s32 i;

	for (i = 0; i < SA_PIXELS; i++) {
		s32 w1 = (i + 1L < SA_PIXELS) ? (s32)g_wind[i + 1L] : 0L;
		s32 d = (((s32)g_wind[i] << 3) + (w1 << 3)) >> 1;
		s32 hv;

		d -= 0xa0L;
		hv = (s32)g_height[i] - d;
		g_height[i] = (s16)(hv < 0L ? 0L : hv);
	}
}

/* ORIGIN 0x003133de Sand_BuildWave - 41280 words of rand(0x14), 0 to
 * 19, one per pixel for the renderer.
 *
 * THE COUNT IS LOAD-BEARING, not just the values: Sand_Tick draws the
 * thunder from this same generator, so filling a different number of
 * words puts every later roll out of phase and the scene fades out to
 * grey instead of black. sand.txt 11 has the measurement. */
static void sand_build_wave(void)
{
	s32 i;

	for (i = 0; i < SA_WAVE_WORDS; i++)
		g_wave[i] = (u16)sand_rand_below(SA_WAVE_RANGE);
}

/* ORIGIN 0x00313d08 Sand_WritePalette and 0x00313d24 its entry - 256
 * colours rebuilt every tick out of Frelicdata's first 0x400. A
 * longword is (unused, R, G, B) at 8 bits a channel; each channel is
 * scaled by g_SandFade and then has g_SandFlash ADDED, saturating,
 * which is how the fade-out and the thunder compose.
 *
 * OURS is the >> 2: the demo hands 8-bit channels to the AGA CLUT and
 * a Relic_Frame channel is 6-bit. */
static void sand_write_palette(void)
{
	s32 i, k;

	for (i = 0; i < SA_PAL_ENTRIES; i++) {
		const u8 *src = g_pal_src + (size_t)(i * 4L) + 1L;

		for (k = 0; k < 3; k++) {
			s32 c = ((s32)src[k] * g_fade) >> 8;

			c += g_flash;
			if (c > 0xffL) c = 0xffL;
			g_pal[(size_t)(i * 3L + k)] = (u8)(c >> 2);
		}
	}
}

/* ORIGIN 0x003138de Sand_RenderFrame - THE WHOLE EFFECT.
 *
 * THE LEVEL IS A DIAGONAL, recomputed at the head of every row and
 * rising 0x10 a pixel across it, so the clearing sweeps rather than
 * dropping uniformly.
 *
 * THE ACCUMULATOR ADVECTS LEFT; IT DOES NOT TRAIL RIGHT. acc[i] is
 * loaded from acc[i + 8], a cell this pass has not reached, so it
 * still holds the PREVIOUS FRAME's value - the field slides eight
 * columns left per frame while decaying and gaining where sand
 * stands. The rightward smear comes from the second average.
 *
 * AND THE STORED VALUE IS NEVER FLOORED. Only the copy read out is
 * clamped to zero, so ground bare a long time stays dark for many
 * frames after sand returns to it.
 *
 * THE TWO AVERAGES ARE THE GRAIN - one temporal tap on the previous
 * frame, one spatial on the left neighbour. The demo spells each as
 * add.b plus roxr.b #1, which rotates the add's carry into bit 7 and
 * is therefore an exact 9-bit (a + b) >> 1.
 */
static void sand_render_frame(void)
{
	s32 x, y, i = 0L;
	s32 wave_at = g_phase & 0xffL;

	for (y = 0; y < SA_H; y++) {
		s32 level = (g_level << 4) - SA_LEVEL_BIAS;
		s16 *acc = g_acc + (size_t)(y * SA_ACC_STRIDE);

		for (x = 0; x < SA_W; x++, i++) {
			s32 v = (s32)g_source[i];
			s32 a;

			acc[x] = acc[x + SA_ACC_LOOK];
			if ((s32)g_height[i] >= level) {
				v = (s32)g_shade[i];
				acc[x] = (s16)((s32)acc[x]
				               + ((s32)g_shade[i] >> 4) + SA_ACC_GAIN);
			}
			acc[x] = (s16)((s32)acc[x] - SA_ACC_DECAY);
			a = (s32)acc[x];
			if (a < 0L) a = 0L;
			v = (v + a) >> 1;
			v += (s32)g_wave[wave_at + i];
			if (v > 0xffL) v = 0xffL;
			v = (v + (s32)g_prev[i]) >> 1;
			v = (v + (i ? (s32)g_out[i - 1L] : 0L)) >> 1;
			g_out[i] = (u8)v;
			level += SA_LEVEL_SLOPE;
		}
	}
}

/* ORIGIN 0x00313a5c C2p_Chunky_To8Plane - g_pSandOut and g_pSandPrev
 * are ONE double buffer, swapped after the interleave has read the
 * finished frame, which is what makes the renderer's first average
 * temporal. */
static void sand_present(void)
{
	u8 *t = g_out;

	g_out = g_prev;
	g_prev = t;
}

/* ORIGIN 0x00313dde Sand_Tick - once per VBL off Sand_VblHook,
 * whatever the renderer is doing, and nothing but the fuse moves
 * until Sand_RunShots raises g_SandRunning. */
static void sand_tick(void)
{
	if (g_running) {
		g_delay -= 1L;
		if (g_delay < SA_DELAY_GATE && g_level < SA_LEVEL_CAP)
			g_level += 1L;

		/* The thunder: a one-in-a-hundred roll per tick, and only
		 * while the scene is bright. 0x28 of 0xff is about a sixth,
		 * which is why it reads as lightning off the horizon rather
		 * than a flash on the scene - every colour lifts together and
		 * nothing in the picture moves. */
		if (g_fade >= SA_THUNDER_AT) {
			if (sand_rand_below(0x64L) == 0L)
				g_flash_ticks = sand_rand_below(5L) + 4L;
			g_flash = 0L;
			if (g_flash_ticks) {
				g_flash = SA_FLASH_AMT;
				g_flash_ticks -= 1L;
			}
		}
		g_phase += SA_PHASE_STEP;
		if (g_fade != SA_FADE_FULL) {
			g_fade -= SA_FADE_STEP;
			if (g_fade < 0L) g_fade = 0L;
		}
		sand_write_palette();
		if (g_curtain < SA_CURTAIN_MAX) g_curtain += 1L;
	}
	g_fuse -= 1L;
}

/* OURS */
static int sa_alloc(void)
{
	if (g_height) return 0;
	g_height  = (s16 *)malloc((size_t)SA_PIXELS * sizeof(s16));
	g_shade   = (u8  *)malloc((size_t)SA_PIXELS);
	g_acc     = (s16 *)malloc((size_t)(SA_H * SA_ACC_STRIDE) * sizeof(s16));
	g_out     = (u8  *)malloc((size_t)SA_PIXELS);
	g_prev    = (u8  *)malloc((size_t)SA_PIXELS);
	g_wave    = (u16 *)malloc((size_t)SA_WAVE_WORDS * sizeof(u16));
	g_wavetab = (s32 *)malloc((size_t)SA_WAVETAB * sizeof(s32));
	return (g_height && g_shade && g_acc && g_out && g_prev
	        && g_wave && g_wavetab) ? 0 : 1;
}

/* OURS */
static int sa_init(void)
{
	const u8 *dat;
	u32 len;
	s32 i;

	g_ready = 0;
	if (Gfx_Init()) return 1;
	if (sa_alloc()) return 1;

	dat = Dat_Chunk("Frelicdata", &len);
	if (!dat || len < (u32)(SA_PART1 + SA_PART2 + SA_PART3)) return 1;
	g_pal_src = dat;
	g_source  = dat + SA_PAL_BYTES;
	g_stamp   = dat + SA_PART1;
	g_wind    = dat + SA_PART1 + SA_PART2;

	/* MEMF_CLEAR - Mem_AllocSubBlocks zeroes the whole block, and the
	 * renderer reads the accumulator before anything writes it. */
	for (i = 0; i < SA_H * SA_ACC_STRIDE; i++) g_acc[i] = 0;
	for (i = 0; i < SA_PIXELS; i++) { g_out[i] = 0; g_prev[i] = 0; }

	/* ORIGIN 0x00313354 onward - Sand_Init's build chain, in its
	 * order, with the two routines it runs TWICE run twice. */
	g_rng = 0L;
	sand_build_height(0);           /* Sand_BuildHeightSine         */
	sand_build_wave_table();
	sand_stamp_features();
	sand_erode_height();
	sand_shade_from_height();
	sand_build_height(g_wavetab);   /* Sand_BuildHeightTable        */
	sand_stamp_features();
	sand_erode_height();
	sand_build_wave();

	g_delay = SA_DELAY0;
	g_fuse = SA_FUSE0;

	/* ORIGIN 0x00313830 Sand_RunShots - the fade is held at full and
	 * the level cleared, then TWO frames are rendered and presented
	 * before g_SandRunning goes up. */
	g_fade = SA_FADE_FULL;
	g_flash = 0L;
	g_flash_ticks = 0L;
	g_level = 0L;
	g_phase = 0L;
	g_curtain = 0L;
	g_running = 0L;
	g_tick = 0L;
	sand_write_palette();
	sand_render_frame(); sand_present();
	sand_render_frame(); sand_present();
	g_running = 1L;
	g_ready = 1;
	return 0;
}

/* OURS - the tick, then the cue. Sand_RunShots drops g_SandFade
 * BETWEEN two presents, so the tick the cue lands on still sees 0xfe
 * and the first decrement is the tick after. */
static void sa_step(s32 tick)
{
	Parts_GateStep(&g_cue_gate, tick, SA_CUE_ORDER, SA_CUE_ROW,
	               SA_TICKS);
	g_tick = tick;
	if (!g_ready) return;
	sand_tick();
	if (tick == SA_FADE_AT) {
		g_fade = SA_FADE_RUN;
		g_fuse = SA_FUSE0;
	}
}

/* ORIGIN 0x0031398a Sand_UpdateCopper - THE OPENING BARS ARE A
 * DISPLAY-WINDOW EFFECT. Not one black pixel is drawn in the demo:
 *
 *     DIWSTRT = ((0x9e - g_SandCurtain) << 8) | 0x83
 *     DIWSTOP = ((0x9e + g_SandCurtain) << 8) | 0xc1
 *     base    = shown + (0x40 - g_SandCurtain) * 0x28 + 0x28
 *
 * 0x28 is one row of 40 bytes, so every line the window gains at the
 * top is matched by one row the plane base loses - which is what
 * keeps the picture anchored instead of sliding down as the bars
 * part. The visible span is therefore buffer rows 0x41 - c through
 * 0x41 + c. OURS is doing it by not blitting those rows, a port
 * having no display window.
 */
static int sa_row_visible(s32 row)
{
	return row >= SA_CURTAIN_MID - g_curtain
	    && row <  SA_CURTAIN_MID + g_curtain;
}

/* OURS - the resample out of the demo's buffer; see the file head for
 * why the filter is not run at the frame's resolution. */
static void sa_frame(Relic_Frame *f, s32 tick)
{
	s32 py;

	(void)tick;
	Frame_Clear(f);
	if (!g_ready) return;

	sand_render_frame();
	sand_present();

	for (py = 0; py < f->h; py++) {
		s32 sy = PX_TO_DEMO(py, f->scale);
		const u8 *row;
		u8 *dst;
		s32 px;

		if (sy < 0L || sy >= SA_H || !sa_row_visible(sy)) continue;
		row = g_prev + (size_t)(sy * SA_W);
		dst = f->px + (size_t)(py * f->w) * PX_BYTES;
		for (px = 0; px < f->w; px++, dst += PX_BYTES) {
			s32 sx = PX_TO_DEMO(px, f->scale);
			const u8 *c;

			if (sx < 0L || sx >= SA_W) continue;
			c = g_pal + (size_t)row[sx] * 3L;
			dst[PX_R] = c[0];
			dst[PX_G] = c[1];
			dst[PX_B] = c[2];
			dst[PX_LIGHT] = 0;
		}
	}
}

/* OURS */
static void sa_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "tick %ld | level %ld/%ld delay %ld | fade %ld"
	             " flash %ld (%ld) | phase %ld | bars %ld/%ld",
	        (long)g_tick, (long)g_level, (long)SA_LEVEL_CAP,
	        (long)g_delay, (long)g_fade, (long)g_flash,
	        (long)g_flash_ticks, (long)g_phase,
	        (long)g_curtain, (long)SA_CURTAIN_MAX);
}

/* OURS - parts.h. The fallback is the DEMO'S, measured off hardware
 * - see SA_FADE_AT: the cue lands at 827 and the fade takes 127
 * more, so 955 ticks, a little over 19 seconds of PAL. */
static s32 sa_length(void)
{
	return Parts_GateLength(&g_cue_gate, SA_TICKS);
}

const Relic_Part PART_Sand = {
	"2.13", "The sand",
	"Sand_RunScene 0x003132a4",
	PART_LIT, SA_W, SA_H,
	sa_init, sa_step, sa_frame, sa_stats,
	SA_TICKS, 0, sa_length
};
