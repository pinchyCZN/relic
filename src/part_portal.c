/* RELIC - part 2.4, the white flash the portal opens with.
 *
 * ORIGIN 0x002cd4d0 Space_DrawApproachFrame, the second of
 * maybe_Space_RunPlanetScenes' two scenes. Scene two hands over to it
 * by filling both buffers white, so this one opens on a white screen
 * and the flash blooms out of it - see effects/portal.txt.
 *
 * WHAT IS HERE: the handover fill, g_ApproachRange's countdown, the
 * flash, the spark shower and the vessel - src/ship.c. WHAT IS NOT:
 * the ripple's per-pixel displacement, marked [open] below.
 *
 * THE LOOK OF approach.txt 8 REACHES BOTH FRAME ROUTINES, so the ship
 * swings from the moment it arrives. The ripple cannot follow it: its
 * backdrop is the sky baked once at the handover.
 */

#include <stdio.h>
#include "parts.h"
#include "gfx.h"
#include "space.h"
#include "image.h"
#include "ship.h"

/* The music cue the scene ends on - armed at 0x002cd39a, polled at
 * 0x002cd3d6. parts.h Relic_Gate. */
#define PO_CUE_ORDER 0x08L
#define PO_CUE_ROW   0L
#define PO_TICKS 1028L
static Relic_Gate g_cue_gate;

/* ORIGIN 0x002cd102 maybe_Space_RunPlanetScenes - the sequencer seeds
 * g_ApproachRange at Portal_SeedRange and Anim_SceneTick's mode 1
 * counts it down, clamping at zero. It is the scene's only clock: the
 * flash radius, the ripple amplitude and the gate on
 * Scene_SortAndDrawAll all read it. portal.txt 2. */
#define RANGE0        0xe60L
#define RANGE_STEP    0xcL
#define SPARK_GATE    0x960L      /* 0x002cd53a, sparks at or below  */
#define PHASE_STEP    0x1f4L      /* 0x002d5d32, the ripple's phase  */
#define WHITE_TICKS   4L          /* measured - see portal_frame     */
#define ANG_GATE      0xa00L      /* 0x002d5d56, the spin's gate     */
#define ANG_STEP      0x14L       /* 0x002d5d62                      */
#define CAM_X0        (-0x137b8L) /* 0x002cd304                      */
#define CAM_Y0        0xcc88L     /* 0x002cd2fa - the depth          */
#define CAM_Z0        0xaf0L      /* 0x002cd30e                      */
#define CAM_X_STEP    0x68L       /* 0x002d5d76, subi of a negative  */
#define CAM_Y_STEP    (-0x50L)    /* 0x002d5d6c                      */
#define TRACK_OZ      0x640L      /* Portal_SeedTrackTarget          */
/* ORIGIN Portal_SeedBodies - the sun and the second body this scene
 * seeds. Mode 1 does not drift them; only mode 2 does. */
#define SUN_WX        (-0x960L)
#define SUN_WY        (-0x1f4L)
#define BODY_WX       (-0xfa0L)
#define BODY_WY       (-0x1f4L)
#define TRACK_OX      0L          /* the x offset, cleared there     */

/* ORIGIN 0x002cd102 maybe_Space_RunPlanetScenes - at
 * Portal_SnapCamera, mode 1 opens with the camera snapped to the
 * limit scene two's yaw gate stops at, and a pitch of its own. */
#define YAW0        (-0xacaL)
#define PITCH0      (-0x4fL)

static s32 g_range;
static s32 g_phase;
/* ORIGIN 0x002e0b26 g_ObjectAngle, cleared by the sequencer at
 * Portal_ClearObjectAngle - the angle the ship spins about. */
static s32 g_obj_ang;
/* ORIGIN g_CamSrcX, g_CamSrcY, g_CamSrcZ - latched per frame into
 * g_CamX/Y/Z. g_CamSrcY IS THE DEPTH SUBTRAHEND. portal.txt 5.4. */
static s32 g_cam[SHIP_CAM];
/* ORIGIN Portal_SnapCamera and Portal_SeedTrackTarget - the camera
 * the tracker steers, and the offsets it aims at. */
static Relic_Cam g_cv;
static s32 g_flash_r;
static s32 g_filled;
static s32 g_range_prev;
static s32 g_second_on, g_second_cx, g_second_cy;

/* OURS */
static int portal_init(void)
{
	if (Space_Init()) return 1;
	g_range = RANGE0;
	g_phase = 0;
	g_obj_ang = 0;
	g_cam[0] = CAM_X0; g_cam[1] = CAM_Y0; g_cam[2] = CAM_Z0;
	g_cam[3] = YAW0;   g_cam[4] = PITCH0;
	g_cv.yaw = YAW0; g_cv.pitch = PITCH0;
	g_cv.head = 0;
	Approach_FinalCamAngles(&g_cv.angle, &g_cv.angle2);
	g_flash_r = 0;
	g_filled = 0;
	g_range_prev = 0;
	g_second_on = 0;
	/* OPTIONAL. Without relic.dat the scene still draws; it just has
	 * no vessel in it. */
	Ship_Init();
	Ship_SetSkipTable(SHIP_SKIP_PLANET);
	return 0;
}

/* OURS. Mode 1's tick, Anim_SceneTick 0x002d5c66. The four other
 * longs it steps belong to the ripple and the object pass and are
 * [open] with them. */
static void portal_step(s32 tick)
{
	Parts_GateStep(&g_cue_gate, tick, PO_CUE_ORDER, PO_CUE_ROW,
	               PO_TICKS);
	if (tick <= 0) {
		g_range = RANGE0;
		g_phase = 0;
		g_obj_ang = 0;
		g_cam[0] = CAM_X0; g_cam[1] = CAM_Y0; g_cam[2] = CAM_Z0;
		g_cam[3] = YAW0;   g_cam[4] = PITCH0;
		g_cv.yaw = YAW0; g_cv.pitch = PITCH0;
		g_cv.head = 0;
		/* THE ACCUMULATORS CARRY ACROSS THE HANDOVER - portal.txt 5.6 */
		Approach_FinalCamAngles(&g_cv.angle, &g_cv.angle2);
		g_filled = 0;
		/* ORIGIN 0x002cd44a - seeded once at the handover from the
		 * object's last projected position. Standalone, the viewer has
		 * no scene two behind it, so this uses the position scene two
		 * ends on, which Approach_FinalLightProj recomputes rather
		 * than stores. */
		{
			s32 px, py;
			Approach_FinalLightProj(&px, &py);
			Space_SeedSparks(px, py);
			/* ORIGIN 0x002cd376 - the sky is baked into the ripple's
			 * source once, between the two scenes, and never again. */
			Space_BakeSky(Space_SkyPan(g_cv.angle, g_cv.angle2));
		}
		return;
	}
	/* ORIGIN 0x002d5d32 - the ripple's phase first, unconditionally,
	 * and it only ever goes down. */
	g_phase -= PHASE_STEP;
	/* ORIGIN 0x002d5d44 - minus 12 a tick, floored at zero. */
	if (g_range) {
		g_range -= RANGE_STEP;
		if (g_range < 0) g_range = 0;
	}
	/* ORIGIN 0x002d5c66 Anim_SceneTick - mode 1 spins the object only
	 * once g_ApproachRange is under 0xa00, so the vessel is still as
	 * it arrives and turns as it closes. */
	/* THE GATE NEVER CLOSES - zero is still under 0xa00 - so the
	 * camera travels for the rest of the scene, closing on the ship
	 * rather than revealing it. portal.txt 5.4. */
	if (g_range < ANG_GATE) {
		g_obj_ang += ANG_STEP;
		g_cam[0] += CAM_X_STEP;
		g_cam[1] += CAM_Y_STEP;
	}
	/* ORIGIN 0x002d5c66 Anim_SceneTick - THE TRACKER RUNS ONLY ONCE
	 * THE COUNTDOWN IS SPENT, so the camera holds still for 312 ticks
	 * and then swings after the vessel. portal.txt 5.5. */
	if (g_range <= 0) {
		Space_CamTrack(&g_cv, g_cam, TRACK_OX, TRACK_OZ);
		g_cam[3] = g_cv.yaw;
		g_cam[4] = g_cv.pitch;
	}
}

/* ORIGIN 0x002cd55c Space_DrawFullFrame - the scene's SECOND loop,
 * which the sequencer runs once g_ApproachRange reaches zero and
 * Space_DrawApproachFrame stops. Eight calls behind the same nine
 * latch copies, and every one of them already exists here from the
 * planet scenes:
 *
 *     Scene_DrawTexturedSky        a LIVE sky, not the ripple's still
 *     Scene_ProjectAndDrawSun
 *     Sprite_StampSaveUnder
 *     Scene_DrawPlanetAndHalo
 *     Scene_SortAndDrawAll         the ship, between stamp and count
 *     Sprite_RestoreSaveUnder
 *     Flare_Draw
 *     maybe_Scene_DrawHorizonBand
 *
 * THIS IS WHY THE SKY MOVES IN THE SECOND HALF. The first loop's
 * background is the warp buffer baked once, so it can only ripple;
 * this one samples the sky through the live camera every frame, and
 * the tracker is turning that camera.
 *
 * AND WHY THE SHIP GETS ITS OWN FLARE. Drawn between the save-under
 * stamp and the count, the vessel destroys sentinels exactly as the
 * asteroid does in 2.3, so the surviving count - and with it the
 * second flare's radius - falls as it crosses the sun. */
static void portal_full_frame(Relic_Frame *f)
{
	s32 fx, fy, bx, by, radius;
	s32 dyaw, dpitch, cam[SHIP_CAM];
	int sun_ok, body_ok;

	Space_Look(&dyaw, &dpitch);
	Ship_LookCam(cam, g_cam, dyaw, dpitch);
	Space_SetCamera(g_cv.yaw + dyaw, g_cv.pitch + dpitch);
	Gfx_SetFlareRayPhase(g_cv.angle >> 12);
	Gfx_SetLightBase(0x1f, 0x00, 0x2f);
	Frame_Clear(f);
	Space_DrawSky(f, Space_SkyPan(g_cv.angle + dyaw * SPACE_LOOK_SKY,
	                              g_cv.angle2 + dpitch * SPACE_LOOK_SKY),
	              g_cv.head);

	sun_ok = Space_Project(SUN_WX, SUN_WY, &fx, &fy);
	if (!sun_ok) fx = fy = -0x4000;
	body_ok = Space_Project(BODY_WX, BODY_WY, &bx, &by);

	radius = 0x50L - (g_range_prev >> 8);
	if (sun_ok && radius > 0)
		Gfx_Flare(f, fx, fy, radius, g_RayArcs, g_ColRampWarm);

	g_range_prev = Space_Range();
	if (sun_ok) Space_StampSaveUnder(f, fx, fy);
	if (body_ok) Space_DrawPlanetAndHalo(f, bx, by);

	Ship_Draw(f, g_obj_ang, cam);

	if (sun_ok) {
		s32 cx = fx, cy = fy;
		g_second_on = Space_EaseRange(Space_CountSaveUnder(f, &cx, &cy));
		if (g_second_on) { g_second_cx = cx; g_second_cy = cy; }
		Space_StepBand(g_second_on ? cy : -1);
	}
	if (g_second_on && sun_ok) {
		s32 r2 = Space_Range() >> 8;
		if (r2 > 0)
			Gfx_Flare(f, g_second_cx, g_second_cy, r2,
			          g_RaySpikes, g_ColRampWarmB);
	}
	if (sun_ok) Space_DrawFlareGhosts(f, fx, fy);
	Space_DrawHorizonBand(f, fx);
}

/* OURS. Space_DrawApproachFrame's five steps, in its order. */
static void portal_frame(Relic_Frame *f, s32 tick)
{
	s32 dyaw, dpitch, cam[SHIP_CAM];

	/* ORIGIN 0x002cd3a0 against 0x002cd3cc - THE SEQUENCER SWAPS FRAME
	 * ROUTINES, it does not add to one. The first loop runs while
	 * g_ApproachRange is above zero and the second takes over after,
	 * so this is a branch and not a sequence. */
	if (!g_range && tick >= WHITE_TICKS) {
		portal_full_frame(f);
		return;
	}

	Space_Look(&dyaw, &dpitch);
	Ship_LookCam(cam, g_cam, dyaw, dpitch);
	Space_SetCamera(g_cv.yaw + dyaw, g_cv.pitch + dpitch);
	/* ORIGIN 0x002cd102 - Ham8_BuildTables(0x1f, 0, 0x2f) at the
	 * handover. THE BASE IS PURPLE HERE, NOT SCENE TWO'S WHITE, and
	 * that is most of the scene's colour: Draw_Flare adds, so the
	 * amber flash over a purple base reads pastel magenta rather than
	 * the yellow it is on its own. portal.txt 3. */
	Gfx_SetLightBase(0x1f, 0x00, 0x2f);

	/* ORIGIN 0x002cd422 - THE CUT BELONGS TO NEITHER SCENE: the
	 * sequencer fills both buffers between the two frame loops. Drawn
	 * here because this is the part it is on screen over. Holding it
	 * for every frame would pin each channel at CHAN_MAX and swallow
	 * the flash, which ADDS. portal.txt 5.3. */
	if (tick < WHITE_TICKS) {
		Space_FillWhite(f);
		g_filled = 1;
	} else {
		/* ORIGIN 0x002cd536 Warp_FullscreenRipple. */
		Space_Ripple(f, g_phase, g_range);
		g_filled = 0;
	}

	/* [open] the ripple's DISPLACEMENT. Its source and geometry are
	 * settled - portal.txt 5.1 - but the per-pixel radial offset is
	 * not transcribed, so the copy above stands in for it. */

	/* ORIGIN 0x002cd546 Scene_SortAndDrawAll. THE SHIP ARRIVES INSIDE
	 * THIS SCENE, over the ripple and under the flash. portal.txt
	 * 5.4. */
	if (g_range <= SPARK_GATE)
		Ship_Draw(f, g_obj_ang, cam);

	/* ORIGIN 0x002cd54a - the flash. */
	g_flash_r = Space_FlashRadius(g_range);
	Space_DrawExplosionFlash(f, g_range);

	/* ORIGIN 0x002cd54e - the sparks, over the top of the flash. */
	Space_StepSparks(f);
}

/* OURS */
static void portal_stats(char *out, int cap)
{
	s32 mx, my;
	(void)cap;
	Space_SparkMid(&mx, &my);
	sprintf(out, "range %04lx ph %ld  flash r%ld  sparks %ld at %ld,%ld"
	             "  yaw %ld ship %ld",
	        (long)g_range, (long)g_phase, (long)g_flash_r,
	        (long)Space_SparkCount(), (long)mx, (long)my,
	        (long)g_cv.yaw, (long)Ship_FacesDrawn());
	(void)g_filled;
}

/* OURS - parts.h. */
static s32 portal_length(void)
{
	return Parts_GateLength(&g_cue_gate, PO_TICKS);
}

const Relic_Part PART_Portal = {
	"2.4", "The asteroid explodes",
	"Space_DrawApproachFrame 0x002cd4d0",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	portal_init, portal_step, portal_frame, portal_stats,
	PO_TICKS, 0, portal_length
};
