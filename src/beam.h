/* RELIC - the lightning beams of the orb sequence.
 *
 * ORIGIN 0x002ca800 Beam_Draw, type 2 in g_SceneObjects. Six of them
 * in all, each a two-vertex object: three from the saucer's rim out
 * to the small disc's spikes and three more from those spikes in to
 * the orb's place in the claw. See docs/subsystems/effects.txt
 * sections 2 and 3.
 */

#ifndef RELIC_BEAM_H
#define RELIC_BEAM_H

#include "relic.h"
#include "geom.h"

/* ORIGIN 0x002ee8a0 g_BeamGeomHullToSpike and 0x002ee8f4
 * g_BeamGeomSpikeToOrb - SIX CONSECUTIVE TWO-VERTEX BLOCKS, 28 bytes
 * each, in the executable. 0 to 2 are the rim-to-spike beams and 3 to
 * 5 the spike-to-orb ones. */
#define BEAM_COUNT 6

/* ORIGIN 0x002ca800 Beam_Draw, +8 - the strength at which a beam
 * stops growing and
 * is struck. NOT A CLAMP: the counters climb past it. orb.txt 2. */
#define BEAM_FULL  0x20L

/* The two endpoints of one beam, straight out of the image. */
void Beam_Geom(s32 which, Relic_Vert *a, Relic_Vert *b);

/* ORIGIN 0x002ca800 - one beam, from two points already projected to
 * DEMO pixels in 24.8. strength is what the object's +0x34 points at,
 * g_BeamPhaseA or g_BeamPhaseB: under 0x10 nothing is drawn at all,
 * 0x10 to 0x20 is the growing phase, and at 0x20 the bolt is full
 * length. */
void Beam_Draw(Relic_Frame *f, s32 which, s32 x0, s32 y0, s32 x1,
               s32 y1, s32 strength);

/* ORIGIN Anim_SceneTick's mode 0x13 - g_BoltSeed steps 2 a tick and
 * g_BoltPhase 200. The seed moves where a bolt's walk begins so two
 * bolts in one frame take different paths; the phase shifts both
 * sines, which is the crackle. */
void Beam_Step(void);
void Beam_Reset(void);

#endif /* RELIC_BEAM_H */
