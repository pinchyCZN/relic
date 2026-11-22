/* RELIC - types, configuration and the render target.
 *
 * ANSI C89. No // comments, no declarations after statements, no
 * stdint.h - the fixed-width types are spelled here, once.
 *
 * THE DEMO CORE HAS NO HOST DEPENDENCY. Everything under src/ except
 * main_*.c renders into a Relic_Frame and nothing else; a host picks
 * it up and puts it on a screen. That is what lets the same code run
 * under a PPM dumper, SDL2 and emscripten without a rewrite.
 */

#ifndef RELIC_H
#define RELIC_H

typedef unsigned char   u8;
typedef signed char     s8;
typedef unsigned short  u16;
typedef signed short    s16;
typedef unsigned long   u32;
typedef signed long     s32;

/* THE AMIGA IS BIG-ENDIAN AND A MODERN PC IS NOT. Every multi-byte
 * read out of the demo's own data goes through these, never through
 * a cast to a wider pointer. */
#define RD_U16(p)  ((u16)(((u16)(p)[0] << 8) | (u16)(p)[1]))
#define RD_S16(p)  ((s16)RD_U16(p))
#define RD_U32(p)  (((u32)(p)[0] << 24) | ((u32)(p)[1] << 16) \
                  | ((u32)(p)[2] <<  8) |  (u32)(p)[3])
#define RD_S32(p)  ((s32)RD_U32(p))

/* ---------------------------------------------------------------
 * Resolution
 *
 * THE DEMO'S OWN SIZES. The space part renders 160 x 100 and the
 * logo screens 320 x 128; those are the units every effect's
 * constants are expressed in.
 */
#define DEMO_W      160
#define DEMO_H      100
#define LOGO_W      320
#define LOGO_H      128

/* Effects are RESOLUTION-PARAMETRIC: a frame carries its own size
 * and a scale against the demo's units.
 *
 * THE RULE THAT KEEPS THAT HONEST: scale the pixel geometry, never
 * the table domains. g_BlobFalloff is indexed [radius << 8 | dist]
 * with radius masked to 0x7f, so a radius-30 blob at 8x would run
 * off the end of the table at 240. Divide the pixel offset by the
 * scale BEFORE the lookup instead - that resamples the demo's own
 * tables and keeps every index in the range the demo uses.
 *
 * Scale 1.0 must stay exact. It is the reference every other scale
 * is checked against.
 */

typedef struct {
	s32 w;          /* pixels across                                */
	s32 h;          /* pixels down                                  */
	s32 scale;      /* 16.16 pixels per demo unit; 0x10000 is 1:1   */
	s32 modes;      /* RELIC_MODE_* the host asked for, scale-gated  */
	s32 subtick;    /* 0..0xffff into the current tick, for tweening  */
	u8 *px;         /* w*h*4 - see the channel note below           */
} Relic_Frame;

/* THE PIXEL IS FOUR BYTES AND ONLY THREE OF THEM ARE COLOUR.
 *
 *     +0  R      6-bit, 0..0x3f
 *     +1  G      6-bit
 *     +2  LIGHT  6-bit, saturating - NOT a colour
 *     +3  B      6-bit
 *
 * Byte +2 is an additive light accumulator. Draw_LineAA and
 * Draw_GlowBlob write it and nothing else does; textures leave it
 * zero. Ham8_BuildTables then blends the three real channels toward
 * a base colour by that amount, so light does not add a colour, it
 * fades the pixel toward one. See docs/subsystems/effects.txt.
 */
#define PX_R      0
#define PX_G      1
#define PX_LIGHT  2
#define PX_B      3
#define PX_BYTES  4

/* RENDER MODES - DECLARED, HONOURED BY NOTHING YET. They exist so
 * transcription is written with them in mind rather than retrofitted.
 * docs/reconstruction.txt section 5. */
#define RELIC_MODE_NONE      0x0
#define RELIC_MODE_SUBPIXEL  0x1
#define RELIC_MODE_TWEEN     0x2

#define LIGHT_MAX 0x3f
#define CHAN_MAX  0x3f

/* 16.16 helpers. The demo works in 16.16 and 24.8 in different
 * places; these are only for the viewer's own scaling. */
#define FX_ONE      ((s32)0x10000)
#define FX_MUL(a,b) ((s32)(((s32)(a) * (s32)(b)) >> 16))
#define FX_DIV(a,b) ((s32)((((s32)(a)) << 16) / (s32)(b)))

/* PIXELS BACK TO DEMO UNITS. An int over a 16.16 scale is ALREADY
 * whole demo units; shifting down again floors every offset to
 * zero and draws a square where a circle was wanted. */
#define PX_TO_DEMO(px,scale) FX_DIV((px), (scale))

/* THE SAME, KEEPING FOUR FRACTION BITS, so a table can be resampled
 * between demo pixels instead of rounded to one - without it a glow's
 * falloff steps once every `scale` pixels and the disc comes out as a
 * staircase of blocks. EXACT AT 1.0: the fraction is then zero.
 * docs/effects/orb.txt 5. */
#define DEMO_FRAC          4
#define SCALE_F(s)         ((s32)(s) >> DEMO_FRAC)
#define PX_TO_DEMO_F(px,s) ((s32)(((s32)(px) << 16) / SCALE_F(s)))
#define DEMO_F_TO_PX(d,s)  (FX_MUL((d), (s)) >> DEMO_FRAC)

#endif /* RELIC_H */
