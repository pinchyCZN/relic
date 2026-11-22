/* RELIC - the ship, as a drawing module rather than a part.
 *
 * ORIGIN 0x002d28ca Scene_SortAndDrawAll. The vessel is NOT a scene of
 * its own: parts.txt numbers the asteroid exploding 2.4 and the ship
 * arriving 2.5, and both are the SAME demo scene - mode 1 of
 * maybe_Space_RunPlanetScenes - so the ship appears while that scene's
 * ripple and flash are still running. Space_DrawApproachFrame calls
 * Scene_SortAndDrawAll once g_ApproachRange drops under 0x961 and
 * keeps drawing everything else around it.
 *
 * So this is a routine the owning part calls, and the state it needs
 * comes in as arguments. See docs/formats/geometry.txt.
 */

#ifndef RELIC_SHIP_H
#define RELIC_SHIP_H

#include "relic.h"
#include "geom.h"

/* Loads Fspacedata and Fshiptextures. Needs relic.dat open. Non-zero
 * when either is missing, which the caller survives by drawing no
 * ship. */
int  Ship_Init(void);
int  Ship_Ready(void);

/* The vessel at g_ObjectAngle. cam is g_CamX/Y/Z then
 * g_CameraYawLatched and g_CameraPitchLatched; THE YAW IS NOT
 * OPTIONAL in the demo's own scenes - their g_CamY leaves every
 * vertex behind the near plane until the yaw brings it round. A part
 * choosing its own camera is free of that. */
#define SHIP_CAM 5
void Ship_Draw(Relic_Frame *f, s32 angle, const s32 *cam);

/* ORIGIN 0x002d2c7a - Scene_TransformProject's twin for a flare, and
 * THE NEAR PLANE IS 0x32 WHERE THE HULL'S IS 0xc8. The orb sits
 * close enough to the camera in 2.10 and 2.11 that the hull's plane
 * would reject it. Whole demo pixels, because Gfx_Flare takes demo
 * units; depth is written either way, for the size tables.
 * docs/effects/orb.txt 11.1. */
#define SHIP_FLARE_CLIP 0x32L
int Ship_ProjectFlare(const Relic_Vert *v, s32 angle, const s32 *cam,
                      s32 *sx, s32 *sy, s32 *depth);

/* OURS - A SCENE'S CAMERA WITH A LOOK ADDED, because the vessel
 * projects through cam[3] and cam[4] of its own rather than through
 * Space_SetCamera. THE SCENE'S OWN COPY IS LEFT ALONE: a tracker's
 * camera feeds the sky's accumulators and whatever the scene bakes,
 * so a drag reaching it would bend the path for good.
 * docs/effects/approach.txt 8. */
void Ship_LookCam(s32 *out, const s32 *in, s32 yaw, s32 pitch);

/* ORIGIN 0x002d2cd4 Scene_TransformProject - the near plane it
 * rejects at, in the units the transform works in. A part picking its
 * own camera distance needs it to know how close it may come before
 * the front of the model starts to vanish. */
#define SHIP_NEAR_CLIP 0xc8L

/* OURS - THE DEPTH THE TRANSFORM CANNOT EXCEED. Scene_TransformProject
 * rotates against a 0x4000-amplitude table and shifts by 14, so a
 * coordinate at or past (1 << 31) / 0x4000 overflows the product,
 * reads back negative and drops the whole model behind the near
 * plane - which looks like the zoom wrapping round. A part choosing
 * its own camera distance has to stop short of it, counting the
 * model's own radius in. docs/reconstruction.txt 7. */
#define SHIP_DEPTH_MAX 0x20000L

/* OURS - THE DRAWN MODEL'S BOUNDS, so a viewer can frame it from the
 * demo's own geometry instead of from a distance somebody liked.
 * centre is the box's middle and radius its largest half-extent
 * across ALL THREE components, because a viewer free to orbit swings
 * every one of them onto the view axis in turn. Honours the skip
 * table, so set that first. Both come back zero when nothing has
 * loaded. */
void Ship_Bounds(s32 *centre, s32 *radius);

/* OURS - EVERY PIECE OF PER-PART SHIP STATE BACK TO ITS DEFAULT. The
 * setters below are module state and each part sets only what it
 * needs, so a part that wants the plain vessel has to undo whatever
 * the part before it left behind. */
void Ship_ResetPose(void);

/* ORIGIN 0x002d6388 Strut_SetClawPose - THE CLAWS' THREE BEND ANGLES,
 * one per joint. The closed pose is the demo's own three biases, so
 * passing 0 straightens the claw completely; src/ship.c holds that
 * pose until a part supplies its own. Re-deforms all six claws from
 * the rest pose, which is what 0x002cffea does.
 * docs/formats/geometry.txt 6.1. */
#define SHIP_CLAW_JOINTS 3
void Ship_SetClawBend(const s32 *bend);

/* ORIGIN 0x002ccb66 Scene_ApplySkipTable - WHICH SLOTS THIS SCENE
 * HIDES. The table is 41 longwords in the image, one per slot, and
 * non-zero means skip; the caller names the one its scene passes.
 * THE PLANET TABLE SETS INDEX 8, WHICH IS OBJECT 11 - that scene has
 * no framebuffer snapshot to texture the billboard with, so leaving
 * the slot drawn paints an untextured square over the hull. It is the
 * only geometry difference between the two ship scenes.
 * docs/formats/geometry.txt 6.1. */
#define SHIP_SKIP_PLANET  0x002ccd6cUL
#define SHIP_SKIP_WINDOW  0x002cce10UL
void Ship_SetSkipTable(u32 image_addr);

/* ORIGIN 0x002d1638 Uv_WriteBillboard11 - OBJECT 11 IS TEXTURED FROM
 * A FRAMEBUFFER SNAPSHOT, not from Fshiptextures, and its UVs are
 * literal rather than projected. Bind the surface here and object 11
 * draws; leave it unbound and object 11 is skipped. tex must be
 * addressable as 256 wide, which is why the snapshot carries that
 * stride - parts.txt 2.8. */
void Ship_SetBillboard(const u8 *tex, s32 rows);

/* ORIGIN 0x002d2cd4 Scene_TransformProject - one vertex through the
 * same transform the hull uses, so an effect drawn in the ship's
 * scene lands in the ship's frame. Returns zero for a vertex behind
 * the near plane. sx and sy come back in DEMO pixels, 24.8. */
int Ship_Project(const Relic_Vert *v, s32 angle, const s32 *cam,
                 s32 *sx, s32 *sy, s32 *depth);

/* ORIGIN 0x002d28ca Scene_SortAndDrawAll - the beams, the flares and
 * the orb are scene objects like the hull and are SORTED AMONG IT, so
 * a nearer claw paints over a glow behind it. Drawing them after the
 * ship instead puts every glow on top of everything, which is what
 * makes the orb look painted on rather than held inside the claws.
 *
 * A part hands them over as centroid plus callback; Ship_Draw keys
 * them the same way it keys a hull part. */
#define SHIP_EXTRAS 16

typedef void (*Relic_ExtraFn)(Relic_Frame *f, s32 which);

typedef struct {
	s32 cent[3];            /* the slot's +0x18, +0x20, +0x1c */
	Relic_ExtraFn draw;
	s32 which;
} Relic_Extra;

void Ship_SetExtras(const Relic_Extra *ex, s32 n);
void Ship_Rot2d(s32 *a, s32 *b, s32 angle);
void Ship_RecentroidStruts(void);
void Ship_SetClawTexture(s32 tex);
void Ship_SetAdditiveLit(int lit);
void Ship_SetFlatScroll(s32 scroll);

/* How many faces the last Ship_Draw projected, and what the loaders
 * found, for a host that wants to say so. */
s32  Ship_FacesDrawn(void);
void Ship_Stats(char *out, int cap);

#endif /* RELIC_SHIP_H */
