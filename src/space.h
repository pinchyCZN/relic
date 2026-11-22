/* RELIC - the space backdrop. See space.c. */

#ifndef RELIC_SPACE_H
#define RELIC_SPACE_H

#include "relic.h"

/* The demo's own screen, measured three independent ways - the
 * projection's halves, Warp_FullscreenRipple's two divisors and the
 * dbf counts. See effects/approach.txt section 5. */
#define SPACE_W     160
#define SPACE_H     100

/* Builds the sky, the planet, the halo and the ghost sprites out of
 * the executable. Idempotent - every scene that needs them calls it. */
int  Space_Init(void);

/* head is the LATCHED head angle - it rotates the whole direction
 * field, and zero is only right for a scene that never pitches.
 * space.c has the rest. */
void Space_DrawSky(Relic_Frame *f, s32 pan, s32 head);

/* The sky's pan offset, from BOTH accumulators - the low byte is
 * horizontal, the rest vertical. portal.txt 5.6. */
s32  Space_SkyPan(s32 angle, s32 angle2);
void Space_DrawPlanetAndHalo(Relic_Frame *f, s32 bx, s32 by);
void Space_DrawFlareGhosts(Relic_Frame *f, s32 fx, s32 fy);

/* ORIGIN 0x002d2c52 - THE ORB BORROWS THE SUN'S GHOSTS by planting
 * the sun's own number: Orb_DrawCentreFlare forces g_SunRange to
 * 0x7f00 and calls Flare_Draw, whose gate and shift are both that.
 * docs/effects/orb.txt 11.1. */
#define SPACE_ORB_RANGE 0x7f00L
void Space_DrawOrbGhosts(Relic_Frame *f, s32 fx, s32 fy);

/* ORIGIN 0x002d2c3a - THE ORB FEEDS Space_DrawHorizonBand IN 2.10
 * AND 2.11, where a space scene's sun feeds it. Call it from the
 * centre flare with the flare's row and radius, then draw the band
 * after the sort - the order Scene_RenderFrame uses.
 * docs/effects/orb.txt 11.2. */
void Space_StepOrbBand(s32 x, s32 row, s32 radius);
void Space_DrawOrbBand(Relic_Frame *f);
void Space_DrawHorizonBand(Relic_Frame *f, s32 fx);

/* ORIGIN 0x002cd642 maybe_Portal_DrawGlow. return.txt 4. */
void Space_PortalGlow(Relic_Frame *f, s32 phase, s32 off);
s32  Space_PortalGlowPx(void);

/* ORIGIN 0x002c8944 - the 35 by 29 flat vessel, at a BYTE offset
 * into the frame. return.txt 2.3. */
void Space_BlitPortalSprite(Relic_Frame *f, s32 byte_off);

/* ORIGIN 0x002c97b8 - g_pPlanetMask stamped at the projected planet,
 * writing LIGHT to open >> 1 wherever it is set. */
void Space_DrawPlanetMasked(Relic_Frame *f, s32 open);

/* OURS - the planet's world point, which the two routines above and
 * Space_DrawPlanetAndHalo all project. */
void Space_SetPlanetWorld(s32 wx, s32 wz);
const u8 *Space_PlanetMask(void);

/* World to screen, through the camera. ZERO MEANS REJECTED - the
 * camera has turned the object behind the eye and nothing should be
 * drawn for it. The Fx form below keeps the fraction the demo
 * truncates and takes 16.16 world coordinates, and has no camera -
 * effects/planet_sun.txt section 10. */
int  Space_Project(s32 wx, s32 wy, s32 *sx, s32 *sy);

/* The camera the projection turns the backdrop by. Zero for part 2.3,
 * which is why that scene's sun drifts alone. */
void Space_SetCamera(s32 yaw, s32 pitch);

/* OURS - A SPACE SCENE'S LOOK AS A CAMERA PAIR, host.h's drag mapped
 * and springing back on release. SPACE_LOOK_MAX is
 * what full deflection is worth where a revolution is 0x2000: 45
 * degrees, just past a space scene's own half fields so the framing
 * leaves the frame, and short of the yaw where a projected depth
 * reaches zero. SPACE_LOOK_SKY is maybe_Camera_MoveRotated's gain
 * with the camera level, so a scene adding it to the sky's
 * accumulators pans by the demo's own gearing. approach.txt 8. */
#define SPACE_LOOK_MAX  0x400L
#define SPACE_LOOK_SKY  0x2000L
void Space_Look(s32 *yaw, s32 *pitch);
void Space_ProjectFx(s32 wx_fx, s32 wy_fx, s32 *sx_fx, s32 *sy_fx);

/* The save-under pair: it measures the sun's occlusion THROUGH THE
 * FRAME, so stamp before the occluders and count after - anything
 * drawn between them dims the flare. The count is -1 when the sun's
 * cell block does not fit on screen, and it also eases the range and
 * reports the centroid of what survived. */
void Space_StampSaveUnder(Relic_Frame *f, s32 fx, s32 fy);
s32  Space_CountSaveUnder(Relic_Frame *f, s32 *mx, s32 *my);
int  Space_EaseRange(s32 cells);
s32  Space_Range(void);
void Space_CountStats(s32 *visible, s32 *stamped);
void Space_StepBand(s32 mean_y);

s32  Space_ScaleFx(s32 a_fx, s32 scale);

/* The scene-two to scene-three transition: both buffers filled with
 * 0x3f3f003f, which is white in the four-byte pixel. */
void Space_FillWhite(Relic_Frame *f);

/* The spark shower, seeded once from the object's last projected
 * position and stepped per frame. SPARK_SEEDED is 10 and SPARK_STEP
 * is 7, which is the demo's own mismatch - see effects/portal.txt. */
#define SPARK_SEEDED 10
#define SPARK_STEP    7
void Space_SeedSparks(s32 proj_x, s32 proj_y);
void Space_StepSparks(Relic_Frame *f);
s32  Space_SparkCount(void);
void Space_SparkMid(s32 *mx, s32 *my);

/* THE RIPPLE'S SOURCE, which is the sky baked once into its own
 * buffer - 176 x 102 four-byte pixels, the screen plus eight columns
 * of slack each side and a row top and bottom. Space_BakeSky fills
 * rows 0 to 99 of it; Space_RippleSource hands back the live region's
 * top-left and its stride in pixels. NULL until the bake has run. */
#define RIPPLE_W      176
#define RIPPLE_H      102
#define RIPPLE_LIVE   0x2e0L      /* bytes: one row plus eight longs */
/* The generated sky and its direction table, 256x256 each. The Earth
 * shot builds its planet out of them - effects/earth.txt 2. NULL
 * until Space_Init has run. */
const u8  *Space_SkyTexture(void);
const s32 *Space_SkyDir(void);

void      Space_BakeSky(s32 pan);

/* ORIGIN 0x002c9868 - the same buffer, filled from the departure's
 * panorama at (scroll - 8). effects/return.txt 4.2.1. */
void      Space_BakePanorama(const u8 *pan, s32 pan_w, s32 scroll);

/* ORIGIN 0x002cd71c Explosion_DrawFlash and its radius, which is a V
 * in g_ApproachRange. Two parts draw it. */
s32  Space_FlashRadius(s32 range);
void Space_DrawExplosionFlash(Relic_Frame *f, s32 range);
const u8 *Space_RippleSource(s32 *stride);

/* ORIGIN 0x002d671c - the camera's five longs, caller-owned. */
typedef struct Relic_Cam {
	s32 yaw, pitch; /* g_CameraYaw, g_CameraPitch */
	s32 head;       /* g_CameraHeading            */
	s32 angle, angle2;
} Relic_Cam;

void Space_CamMove(Relic_Cam *c, s32 d0, s32 d1);

/* ORIGIN 0x002d57bc Camera_TrackTarget. portal.txt 5.5. */
void Space_CamTrack(Relic_Cam *c, const s32 *cam, s32 ox, s32 oz);

/* ORIGIN 0x002d5772 Camera_AimAhead - the same track with a Z offset
 * and a positive yaw step capped at 3. return.txt 2.1. */
void Space_CamAim(Relic_Cam *c, const s32 *cam, s32 oz);

/* The ripple itself: reads the baked buffer, writes the frame. phase
 * winds backwards 0x1f4 a tick from zero and range is
 * g_ApproachRange. */
void      Space_Ripple(Relic_Frame *f, s32 phase, s32 range);

/* ORIGIN 0x002ce992 - the blur SPACE_PRESENT applies to every frame
 * of every space scene, so a part that leaves it out is missing a
 * pass the demo always runs. destruction.txt 2. */
int  Space_BlurInit(void);
void Space_BlurFrame(Relic_Frame *f);

#endif
