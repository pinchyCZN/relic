/* RELIC - the shared drawing primitives.
 *
 * These are the demo's own toolbelt, not ours. A handful of
 * routines and tables carry its entire visual language; the
 * distinct-caller counts out of the database are:
 *
 *     g_SinTable       40      the sprite blitter   17
 *     g_PolarTable     24      g_BlobFalloff        15
 *     the RNG          19      Draw_GlowBlob        10
 *     Vbl_SetHook      18      Math_Isqrt           10
 *     g_LightSatLut    10      Ham8_BuildTables      9
 *
 * So anything a part needs twice belongs here, not in the part.
 */

#ifndef RELIC_GFX_H
#define RELIC_GFX_H

#include "relic.h"

/* Image addresses, so a plate comment and this header agree. */
#define A_SIN_TABLE     0x00228b6cUL   /* 8192 longwords, 14-bit    */
#define A_COS_TABLE     0x0022ab6cUL   /* the same table, +2048     */
#define A_POLAR_TABLE   0x0029f97aUL   /* 65536 words; +0x10000 is  */
                                       /* the signed centre         */
#define A_BLOB_FALLOFF  0x002bf97aUL   /* [radius << 8 | dist]      */
#define A_RAMP_PAD      0x002eea50UL   /* in front of the ramps     */

/* ORIGIN 0x002d4532 - Draw_Flare's ramp index is a signed word and
 * runs to -14 and 279, so every ramp carries forty longwords of the
 * demo's own padding at each end. gfx.c lays the six of Ramp_BuildAll
 * out as one block; a ramp of its own pads with this. */
#define RAMP_PAD        40

#define ANGLE_MASK      0x1fff         /* 8192 steps to the turn    */

int  Gfx_Init(void);                   /* builds the ramps, 0 = ok  */

/* ---- tables ---------------------------------------------------- */

/* OURS - A LONG CUT DOWN TO ITS LOW WORD, sign-extended: what a
 * 68020 muls.w does to an operand before it multiplies. A cross
 * product the demo takes in words has to be taken in words here too,
 * or a coordinate that outgrew a word overflows the s32 product
 * instead of wrapping into it and the sign comes out wrong.
 * docs/formats/geometry.txt 3.1. */
s32  Gfx_Word(s32 v);

s32  Gfx_Sin(s32 angle);               /* 14-bit fixed              */
s32  Gfx_Cos(s32 angle);

/* ORIGIN 0x0029f97a - ONE TABLE SERVES TWO UNRELATED CALLERS. The
 * high byte of a word is an angle, which Uv_AtanLookup wants; the
 * low byte is a distance, which every glow blob wants. Indexed by a
 * SIGNED (dy << 8) | dx. */
u8   Gfx_PolarDist(s32 dx, s32 dy);
u8   Gfx_BlobFalloff(s32 radius, s32 dist);
u8   Gfx_PolarAngle(s32 dx, s32 dy);

/* ORIGIN 0x002ca766  Math_Isqrt - restoring integer square root.
 * Kept rather than reduced to a library call because the demo's own
 * rounding is what the bolt amplitude and the flare radius use.
 * Gfx_IsqrtU is the routine at its own width, unsigned 32-bit;
 * Gfx_Isqrt is ours on top, returning 0 for n <= 0. */
u32  Gfx_IsqrtU(u32 n);
s32  Gfx_Isqrt(s32 n);

/* ORIGIN 0x002cfe38 - the demo's LCG, and the only randomness in
 * it. Returns 0 .. range-1, as the demo does: the state advances by
 * state*0x5d43 + 0x5c2d and the result is a high-word multiply. */
s32  Gfx_Rand(s32 range);
void Gfx_SeedRand(u32 seed);

/* The intensity ramps Ramp_BuildAll makes. A caller picks one and
 * hands it to Draw_GlowBlob, which is how one blob routine serves
 * sparks, bolt nodes and beam heads at different brightnesses. */
extern u8 g_RampBolt[256];      /* ORIGIN 0x002c9fba, linear        */
extern u8 g_RampSpark[256];     /* ORIGIN 0x002ca00e, D3 = 0x12c    */
extern u8 g_RampBeamHead[256];  /* ORIGIN 0x002ca00e, D3 = 0x96     */
extern u8 g_RampBoltHalf[256];  /* g_RampBolt, each entry >> 1      */
extern u8 g_LightSat[256];      /* ORIGIN 0x002cff1e, min(i, 0x3f)  */

/* ---- the frame -------------------------------------------------- */

int  Frame_Init(Relic_Frame *f, s32 w, s32 h, s32 scale);
void Frame_Free(Relic_Frame *f);
void Frame_Clear(Relic_Frame *f);

/* ---- the primitives --------------------------------------------- */

/* ORIGIN 0x002cc4bc Turbulence_BuildTable, and the cursors
 * Anim_SceneTick walks across it. ONE table, read twice at two
 * drifting offsets - that is the whole of the churn. */
void Gfx_BuildTurbulence(void);
void Gfx_StepTurbulence(void);

/* ORIGIN 0x002d466c  Draw_FlareTurbulent */
void Gfx_FlareTurbulent(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                        const u8 *ramp);

/* ORIGIN 0x002cefa4  Draw_GlowBlob. Centre and radius are in DEMO
 * UNITS; the frame's scale turns them into pixels. The table lookup
 * is done in demo units too, which is what keeps the radius inside
 * g_BlobFalloff's 0..0x7f domain at any scale. */
void Gfx_GlowBlobColourTo(u8 *dst, s32 w, s32 h, s32 scale,
                          s32 cx, s32 cy, s32 radius,
                          const u32 *colour);
void Gfx_GlowBlobTo(u8 *dst, s32 w, s32 h, s32 scale,
                    s32 cx, s32 cy, s32 radius, const u8 *ramp);
/* ORIGIN 0x002cf19e - the thruster glow: LIGHT only, through
 * g_RampSpark and g_LightSat, and a TEARDROP rather than a disc
 * because its row index steps 0x50 below the centre and 0x100
 * above. */
void Gfx_GlowOval(Relic_Frame *f, s32 cx, s32 cy, s32 radius);

/* ORIGIN 0x002d4552 Draw_MeteorStreak - LIGHT only, the polar walk
 * linear and the centre leaning by `slant` in 8.8 a row. */
void Gfx_MeteorStreak(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                      s32 step, s32 slant, const u8 *ramp);

/* ORIGIN 0x002d4780 Space_DrawLightBlob - the swirl: an ellipse
 * twice as wide as tall, its falloff warped by two g_turb samples
 * at the cursors passed in. A SCENE OWNS ITS CURSORS because the
 * rate differs - mode 3 steps them every fourth and eighth tick
 * where Gfx_StepTurbulence steps both every tick. return.txt 2.3. */
void Gfx_GlowEllipse(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                     s32 wob_a, s32 wob_b, const u8 *ramp);

/* ORIGIN 0x002d25f8 Draw_FlareBand - ten rows about `row`, a wide
 * flat ellipse added to R, G and B only. return.txt 2.3.1. */
void Gfx_FlareBand(Relic_Frame *f, s32 row, s32 cx);

/* ORIGIN 0x002cf0a0 - the same blob SUBTRACTING from LIGHT. */
void Gfx_GlowBlobSub(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                     const u8 *ramp);

void Gfx_GlowBlob(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                  const u8 *ramp);

/* ORIGIN 0x002ca848  Draw_LineAA - a three-pixel-wide line: the
 * core SET to 0x3f, each neighbour += 0x0f with a raw byte add and
 * no saturation. Endpoints are in demo units; the width follows the
 * scale, because three pixels is three DEMO pixels. */
void Gfx_LineAA(Relic_Frame *f, s32 x0, s32 y0, s32 x1, s32 y1);

/* ORIGIN 0x002ca04c  Ray_BuildTables. 256 words each, indexed by
 * the polar angle byte, generated with the demo's own LCG so they
 * differ every run. Draw_Flare SUBTRACTS the value, and g_RayArcs
 * is negated when built - so spikes darken and arcs brighten.
 * docs/subsystems/effects.txt section 7.1.
 */
extern s16 g_RaySpikes[256];    /* 20 sharp random spikes           */
extern s16 g_RayArcs[256];      /* 20 broad random arcs, negated    */
extern s16 g_RayStreak[256];    /* two bumps 128 apart - a streak   */
extern s16 g_RayStreak[256];    /* two rays, 180 degrees apart      */

void Gfx_BuildRays(void);

/* ORIGIN 0x002c9eac. 256 colours, inverse-square falloff, built in
 * the FRAMEBUFFER'S OWN PIXEL LAYOUT so a single 32-bit add puts
 * one in without carrying between channels.
 */
void Gfx_BuildColourRamp(u8 *out /* 256 * PX_BYTES */,
                         s32 r, s32 g, s32 b, s32 numer);

/* The six Ramp_BuildAll makes, in block order from 0x002eeaf0 and
 * 0x4a0 apart - pointers into one block, gfx.c. */
extern u8 *g_ColRampWarmB;
extern u8 *g_ColRampWarm;
extern u8 *g_ColRampAmber;
extern u8 *g_ColRampGlow;
extern u8 *g_ColRampMagenta;
extern u8 *g_ColRampPale;
void Gfx_RampBlueBump(u8 *ramp, s32 at);

/* ORIGIN 0x002d4452  Draw_Flare - a radial glow WITH ANGULAR RAYS,
 * in colour, and the demo's most used blitter at 17 callers.
 * Draw_GlowBlob uses the polar table's distance byte and writes
 * only light; this also uses its ANGLE byte and adds a colour.
 * Centre and radius in DEMO UNITS. effects.txt section 7.
 */
void Gfx_FlareFx(Relic_Frame *f, s32 cx_fx, s32 cy_fx, s32 radius,
                 const s16 *rays, const u8 *ramp);
void Gfx_Flare(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
               const s16 *rays, const u8 *colour_ramp);

/* ORIGIN 0x002ca456  Ham8_BuildTables - resolve the light byte.
 *
 *     d5  = max(0, 0xff - 4*light)
 *     out = min(0x3f00, base*256 + (c - base)*d5) >> 8
 *
 * SO LIGHT FADES THE PIXEL TOWARD A BASE COLOUR, it is not added.
 * THE BASE IS PER SCENE: Space_RunPart passes (0x3f,0x4f,0x6f) and
 * light brightens toward blue-white; the sun scene passes (0,0,0)
 * and the same byte darkens.
 */
void Gfx_SetLightBase(s32 r, s32 g, s32 b);
/* ORIGIN 0x002e0c1c - turns Draw_Flare's rays. */
void Gfx_SetFlareRayPhase(s32 phase);
void Gfx_ResolveToRGB(const Relic_Frame *f, u8 *rgb_out);

#endif /* RELIC_GFX_H */
