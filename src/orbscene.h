/* RELIC - the scene objects the two orb shots share.
 *
 * The beams, the attach-point flares and the orb are the same objects
 * in anim mode 0x13 and in mode 0x12; a part says which are live and
 * hands over its camera, and this draws them through Ship_Draw's sort
 * so the claws occlude them. docs/effects/orb.txt 7.
 */

#ifndef RELIC_ORBSCENE_H
#define RELIC_ORBSCENE_H

#include "relic.h"

void OrbScene_Reset(void);

/* The bolt seed and phase and the orb's two spin angles, one tick. */
void OrbScene_Step(void);

/* How many beams are live, and the strength each group holds. */
void OrbScene_SetBeams(s32 live, s32 a, s32 b);

/* Whether the six attach flares and the orb are revealed. */
void OrbScene_SetLit(int flares, int orb);

/* Hands the live objects to Ship_SetExtras, keyed for the depth sort.
 * Call once a frame BEFORE Ship_Draw. */
void OrbScene_Register(const s32 *cam, s32 obj_angle);

/* OURS - the orb centre flare's last radius, for a stats line. */
s32 OrbScene_OrbRadius(void);

#endif /* RELIC_ORBSCENE_H */
