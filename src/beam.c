/* RELIC - the lightning beams. See beam.h and
 * docs/subsystems/effects.txt sections 2 and 3. */

#include <string.h>
#include "beam.h"
#include "gfx.h"
#include "image.h"

/* ORIGIN 0x002ee8a0 - six two-vertex blocks back to back, each a
 * longword nverts-1 then two twelve-byte vertices. */
#define BEAM_GEOM    0x002ee8a0UL
#define BEAM_STRIDE  28L

/* ORIGIN 0x002ca9e4 Beam_BuildBoltPath - eight nodes, and the walk
 * steps 0x2000 of a 16.16 fraction per node. */
#define BOLT_NODES   8
#define BOLT_STEP    0x2000L

/* ORIGIN 0x002ca800 - the two thresholds the strength is tested
 * against. Below the first nothing is drawn; between them the bolt is
 * still extending. */
#define BEAM_GROW_MIN 0x10L

static s32 g_bolt_seed;
static s32 g_bolt_phase;

/* ORIGIN each beam slot's +0x48 - every scene object carries one of
 * these arrays and the beams are the only objects that fill it.
 * Particle_Spawn walks 51 entries; Particle_UpdateAndDraw walks 50,
 * so the last is spawned into and never drawn. */
#define SPARKS      51
#define SPARK_LIFE  0xa

typedef struct {
	s16 life, x, y, size;
} Relic_Spark;

static Relic_Spark g_spark[BEAM_COUNT][SPARKS];

/* OURS */
void Beam_Reset(void)
{
	g_bolt_seed = 0;
	g_bolt_phase = 0;
	memset(g_spark, 0, sizeof g_spark);
}

/* ORIGIN 0x002ce264 Particle_Spawn - the first free slot wins, and a
 * full array drops the spawn silently. */
static void spark_spawn(s32 which, s32 x, s32 y, s32 size)
{
	s32 i;

	for (i = 0; i < SPARKS; i++) {
		if (g_spark[which][i].life != 0) continue;
		g_spark[which][i].life = SPARK_LIFE;
		g_spark[which][i].x = (s16)x;
		g_spark[which][i].y = (s16)y;
		g_spark[which][i].size = (s16)size;
		return;
	}
}

/* ORIGIN 0x002ca962 Particle_UpdateAndDraw - THE TRAIL, and Beam_Draw
 * runs it every frame whatever the strength. So the sparks the head
 * shed on its way out are still being drawn after the bolt has
 * arrived, and they fade over five ticks rather than stopping with
 * it: the head leads, the trail follows, the bolt lands. Life counts
 * down by two from ten. */
static void spark_draw(Relic_Frame *f, s32 which)
{
	s32 i;

	for (i = 0; i < SPARKS - 1; i++) {
		Relic_Spark *sp = &g_spark[which][i];
		if (sp->life <= 0) continue;
		sp->life = (s16)(sp->life - 2);
		Gfx_GlowBlob(f, sp->x, sp->y, sp->size, g_RampSpark);
	}
}

/* ORIGIN Anim_SceneTick mode 0x13 - 0x002caaea and 0x002caae6. */
void Beam_Step(void)
{
	g_bolt_seed += 2;
	g_bolt_phase += 200;
}

/* OURS */
void Beam_Geom(s32 which, Relic_Vert *a, Relic_Vert *b)
{
	u32 base = (u32)(BEAM_GEOM + which * BEAM_STRIDE + 4L);
	s32 i;

	for (i = 0; i < 3; i++) {
		a->comp[i] = (s32)Image_U32(base + (u32)(i * 4));
		b->comp[i] = (s32)Image_U32(base + (u32)(12 + i * 4));
	}
}

/* OURS. The demo's own routine is at 0x002ca766 and is not
 * transcribed, so this may round differently by a unit; the bolt's
 * shape is scaled by the result, not positioned by it. */
static s32 isqrt(s32 n)
{
	s32 r = 0, bit = 1L << 30;

	if (n <= 0) return 0;
	while (bit > n) bit >>= 2;
	while (bit) {
		if (n >= r + bit) {
			n -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* ORIGIN 0x002ca9e4 Beam_BuildBoltPath - the whole of the lightning,
 * and there is no random number in it.
 *
 * TWO SINES AT DIFFERENT RATES. The x offset walks the table at half
 * the rate of the march along the line and the y offset at an eighth,
 * so they never come back into step and the result reads as irregular
 * rather than as a wave. Both are scaled by the line's own length, so
 * a bolt keeps its shape at any distance instead of flattening.
 *
 * THE NODE RADIUS TAPERS. d climbs 0x2000 a node, and the blob's
 * radius falls out of (0x10000 - d/2), so the bolt reads as a line
 * with a halo rather than a row of lamps. Four is the floor.
 *
 * Takes and returns WHOLE demo pixels: the original is handed the
 * projected vertices, which the transform has already truncated. */
static void bolt_path(Relic_Frame *f, s32 x0, s32 y0, s32 x1, s32 y1,
                      s32 *px, s32 *py)
{
	s32 dx = x1 - x0, dy = y1 - y0;
	s32 len = isqrt(dx * dx + dy * dy);
	s32 fx0 = x0 << 8, fy0 = y0 << 8;
	s32 fdx = (x1 << 8) - fx0, fdy = (y1 << 8) - fy0;
	s32 d = (g_bolt_seed & 0x3fL) << 7;
	s32 n;

	for (n = 0; n < BOLT_NODES; n++, d += BOLT_STEP) {
		s32 frac = d >> 8;
		s32 ax = fx0 + (((fdx * frac) >> 8));
		s32 ay = fy0 + (((fdy * frac) >> 8));
		s32 r;

		ax += ((Gfx_Sin(((d >> 1) + g_bolt_phase) & 0x1fffL) >> 4)
		       * len) >> 8;
		ay += ((Gfx_Sin(((d >> 3) + g_bolt_phase) & 0x1fffL) >> 3)
		       * len) >> 8;
		px[n] = ax >> 8;
		py[n] = ay >> 8;

		r = ((((0x10000L - (d >> 1)) >> 11) * len) >> 8) >> 1;
		if (r < 0) r = -r;
		if (r < 4) r = 4;
		/* ORIGIN 0x002ca9e4 - the original hands Draw_GlowBlob
		 * g_RampBolt, the same ramp the bolt's lines use. */
		Gfx_GlowBlob(f, px[n], py[n], r, g_RampBolt);
	}
}

/* ORIGIN 0x002ca9a0 Beam_DrawGrowing - THE HEAD, AND THERE IS NO LINE
 * BEHIND IT. The strength walks a point from the origin out to
 * strength/16 of the way along and draws ONE BIG GLOW there, radius
 * 0x1e plus a random ten - thirty to forty pixels on a hundred-row
 * frame. Nothing else is drawn at all until the strength reaches
 * 0x20.
 *
 * SO A BEAM DOES NOT SWITCH ON. A glow head runs out along where the
 * bolt will be, and only when it arrives does the lightning appear -
 * which is why the three rim beams and then the three claw beams read
 * as a flow rather than as lamps.
 *
 * [open] THE SPARKS. Particle_UpdateAndDraw, the per-object array at
 * each slot's +0x48 that Beam_Draw runs every frame whatever the
 * strength, is not transcribed - those are the trail the head leaves
 * behind it. Particle_Spawn at 0x002ce264 is what seeds it, writing
 * {life 0xa, x, y, size} into the first free of 51 slots, and the
 * head seeds one every tick. */
static void beam_growing(Relic_Frame *f, s32 which, s32 x0, s32 y0,
                         s32 x1, s32 y1, s32 strength)
{
	s32 t = strength - BEAM_GROW_MIN;
	s32 ex, ey, r;

	if (t < 0) return;
	ex = x0 + (((x1 - x0) * t) >> 4);
	ey = y0 + (((y1 - y0) * t) >> 4);
	r = 0x1eL + Gfx_Rand(0xaL);
	spark_spawn(which, ex, ey, r);
	Gfx_FlareTurbulent(f, ex, ey, r, g_RampBeamHead);
}

/* ORIGIN 0x002ca800 Beam_Draw - the trail is drawn FIRST and
 * unconditionally; only then does the strength decide between a
 * travelling head and a struck bolt. */
void Beam_Draw(Relic_Frame *f, s32 which, s32 x0, s32 y0, s32 x1,
               s32 y1, s32 strength)
{
	s32 px[BOLT_NODES], py[BOLT_NODES];
	s32 cx, cy, n;

	spark_draw(f, which);
	if (strength < BEAM_FULL) {
		beam_growing(f, which, x0 >> 8, y0 >> 8, x1 >> 8, y1 >> 8,
		             strength);
		return;
	}
	bolt_path(f, x0 >> 8, y0 >> 8, x1 >> 8, y1 >> 8, px, py);
	cx = x0 >> 8;
	cy = y0 >> 8;
	for (n = 0; n < BOLT_NODES; n++) {
		Gfx_LineAA(f, cx, cy, px[n], py[n]);
		cx = px[n];
		cy = py[n];
	}
	/* ORIGIN 0x002ca800 - one more back to the endpoint, because the
	 * eighth node stops short of it. */
	Gfx_LineAA(f, cx, cy, x1 >> 8, y1 >> 8);
}
