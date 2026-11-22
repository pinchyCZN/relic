#ifndef RELIC_STILLS_H
#define RELIC_STILLS_H

#include "relic.h"

/* The Stills module's shared machinery - Stills_RunModule 0x00313f6c
 * and everything its six loops call. The valley and the city's three
 * shots run the SAME routines with different assets swapped in, so
 * this file holds them once and the parts hold only their own script.
 *
 * IT IS NOT src/gfx.c AND MUST NOT BECOME IT. effects/valley.txt 2.7:
 * the Stills module carries its own copy of the blob skeleton, its
 * own runtime tables and its own generator, sharing with the space
 * scenes only what is BAKED. Pure helpers from gfx.h - Gfx_Isqrt,
 * Gfx_BlobFalloff, Gfx_BuildColourRamp - are used here because their
 * output is identical; no DRAW routine is shared in either direction.
 */

#define ST_W            0xa0L
#define ST_H            0x64L
#define ST_TEXELS       (ST_W * ST_H)

/* ORIGIN 0x0031586c Valley_Rand - the module's own LCG, unrelated to
 * the space module's Math_Rand 0x002cfe38. */
s32  Stills_Rand(s32 range);

/* ORIGIN 0x003148fe - the sine, and the same table `base` bytes on. */
s32  Stills_Sin(s32 base, s32 angle);

/* Build the module's tables once. */
void Stills_BuildBlobRamp(void);
void Stills_BuildTrailRamp(void);
void Stills_BuildRays(void);
void Stills_BuildTurbulence(void);
void Stills_BuildSplatTable(u32 palette);
/* ORIGIN 0x003141da - the blast's colour ramp, 0x7f/0x3f/0x3f over
 * 0x780000. Gfx_BuildColourRamp's generator with the same output. */
void Stills_BuildBlastRamp(void);
/* ORIGIN 0x00316506 - one cursor back two words, the other on one. */
void Stills_StepCursors(void);
void Stills_ResetTrail(void);
void Stills_SeedRand(s32 seed);

/* The scene's inputs. The demo keeps these as module globals too -
 * 0x0031654c, 0x0033b07c, 0x0033b048 and the four asset slots at
 * 0x00316738 onward - so holding them here mirrors the binary. */
void Stills_SetThreshold(s32 t);
void Stills_SetCut(s32 v);
/* ORIGIN 0x003143c4 - 0x00315f66, seeded 0x15e. */
void Stills_SetHazeOffset(s32 v);
void Stills_SetHazeDrift(s32 v);

/* ORIGIN 0x00315660 - darken Fplasma's edges. Valley_DrawHaze reads
 * the result, and without it its lower pass ends on a hard edge. */
int  Stills_BuildPlasma(void);
s32  Stills_Threshold(void);
void Stills_StepThreshold(s32 rate);
void Stills_SetBlast(s32 v);
void Stills_StepSpin(s32 d);
void Stills_SetSources(const u8 *picture, const u8 *layer2,
                       const u8 *heightmap);
void Stills_SetScroll(s32 rows);

/* ORIGIN 0x0031426a Valley_LoadDeepImage - scans the FORM a BYTE at
 * a time for 'DBOD', then three plain bytes a pixel. Returns a
 * malloc'd ST_TEXELS buffer or NULL. */
u8  *Stills_LoadDeep(u32 form, s32 texels);

/* ORIGIN 0x0031429a - a palette of `entries` longwords followed by
 * one INDEX BYTE a texel, both through the b0.b1.00.b2 rotate. */
u8  *Stills_LoadPalettised(const u8 *src, s32 entries, s32 texels);

/* ORIGIN 0x003148d4 and 0x003148b0 - flip a still on the VERTICAL
 * axis, in place: 153 rows of 80 swaps. The city's third shot is its
 * first mirrored. effects/city.txt 2. */
void Stills_MirrorPixels(u8 *pic, s32 rows);
void Stills_MirrorBytes(u8 *map, s32 rows);

/* ORIGIN 0x00314c0c Valley_DrawRock - Space_DrawLightBlob's
 * algorithm on a circular span, LIGHT only. `turb` warps the falloff
 * by two turbulence samples the way the swirl does. */
void Stills_Blob(Relic_Frame *f, s32 cx, s32 cy, s32 r,
                 const u8 *ramp, int turb);
/* The blob's own ramp, 0x0033b79e - the rock reads it directly. */
const u8 *Stills_BlobRamp(void);

/* ORIGIN 0x00315524 - the fireball. The ONLY blob here that adds a
 * whole longword, so it writes colour rather than LIGHT. */
void Stills_DrawBlast(Relic_Frame *f, s32 cx, s32 cy, s32 r);

/* ORIGIN 0x00315e20 - a perspective plasma on the ground, LIGHT
 * only, gated on the height map clearing the threshold. */
void Stills_DrawHaze(Relic_Frame *f, s32 term);

/* ORIGIN 0x00315b5a - the picture under the burn, plus the light
 * floor that whitens the frame. effects/valley.txt 2.9. */
void Stills_ImpactBase(Relic_Frame *f);

/* ORIGIN 0x00314a9e and 0x00314acc - the rock's trail. */
void Stills_SpawnTrail(s32 x, s32 y, s32 r);
void Stills_DrawTrail(Relic_Frame *f);

/* ORIGIN 0x0031636c - five spatial taps AND the destination's
 * previous contents, so the scene carries a temporal smear. */
void Stills_Blur(Relic_Frame *f);
int  Stills_BlurInit(s32 texels);

/* ORIGIN 0x00315c34 - the city's base pass: the picture where the
 * front has not reached, splat plus layer two where it has.
 */
void Stills_CityBase(Relic_Frame *f, const u8 *mask);

/* ORIGIN 0x0031589e - plot, step, age and recycle the particles.
 * The step is inside the draw in the demo too. */
int  Stills_ParticlesInit(void);
/* ORIGIN 0x00315ace - every shot entry empties the pool. */
void Stills_ResetParticles(void);
int  Stills_BuildVelocities(s32 shot);
void Stills_DrawParticles(Relic_Frame *f);
s32  Stills_ParticleCount(void);

#endif /* RELIC_STILLS_H */
