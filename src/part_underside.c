/* RELIC part 2.11 - the orb from underneath, the claws clamping on it,
 * and the white flash out. ANIM MODES 0x12, 0xa AND 0xb.
 *
 * ONE PART BECAUSE IT IS ONE CAMERA MOVE. All three phases load
 * g_pCamPath12 and step the SAME cursor two a tick [verified,
 * Anim_SceneTick +0x38c and +0x62c], so the camera never cuts; the
 * claws and the flash are events inside the pan, each on its own
 * clock. Mode 0x13's shot before it is a different camera, on
 * g_pCamPath, which is why 2.10 stays separate.
 *
 * Nothing about the effects changes across the three, so this is the
 * camera and two timed events; orbscene.c draws the rest.
 *
 * See effects/orb_underside.txt and effects/orb_clamp.txt.
 */

#include <stdio.h>
#include "parts.h"
#include "gfx.h"
#include "dat.h"
#include "geom.h"
#include "ship.h"
#include "beam.h"
#include "space.h"
#include "orbscene.h"

/* The music cue the scene ends on - armed at 0x002cc8ac, polled at
 * 0x002cc8bc. parts.h Relic_Gate. */
#define UN_P7_ORDER  0x1eL
#define UN_P7_ROW    0x34L
#define UN_P8_ORDER  0x20L
#define UN_P8_ROW    0L

/* BOTH CUES END A PHASE, NOT THE PART. Space_RunPart arms and polls
 * them at 0x002cc8ac/0x002cc8bc and 0x002cc8fa/0x002cc90a, which are
 * +0x32c and +0x370 into it - inside this part's own 0x002cc840 to
 * 0x002cc9b0 - so they close the pan and the clamp in turn. Ending
 * the PART on the first of them drops the clamp and the flash
 * altogether. reconstruction.txt 4.5. */
static Relic_Gate g_p7_gate;
static Relic_Gate g_p8_gate;

/* ORIGIN 0x002cc590 - the part's light base, unchanged across the cut
 * because Ham8_BuildTables is not called again. */
#define UND_LIGHT_R  0x3fL
#define UND_LIGHT_G  0x4fL
#define UND_LIGHT_B  0x6fL

/* ORIGIN 0x002cc590 Space_RunPart, +0x2b0 - the phase 7 setup.
 * THE PITCH IS THE SHOT:
 * -0x800 of 8192 is -90 degrees, so the camera opens looking straight
 * up. The position it writes is replaced by the path on the first
 * tick; the angles are not. */
#define UND_YAW0     0L
#define UND_PITCH0   (-0x800L)

/* ORIGIN 0x002d28ca - the ship's own angle, as in 2.10. */
#define UND_OBJ_ANGLE 0x100L

/* ORIGIN 0x002cc958 - Ham8_BuildTables for THIS phase, which is a
 * different base from the orb's at 0x002cc590. It decides what a
 * pixel resolves to once its light byte passes 0x3f, so inheriting
 * the part before's turns the orb's core whatever that one was. */
#define UND_LIGHT_R  0x5fL
#define UND_LIGHT_G  0x5fL
#define UND_LIGHT_B  0x7fL

/* ORIGIN 0x002cc590 +0x2f8 - g_CamCursor12 is seeded 0x100, NOT zero,
 * so the path starts at key 1 and key 0 is only ever P[-1]. */
#define UND_CURSOR0  0x100L

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x3a2 - g_FlatScroll loses
 * 0x100 a tick, sliding both of Rast_AddTri's taps. Mode 0x13 does
 * the same; mode 6 does not move it. effects/orb.txt 9. */
#define UND_SCROLL_STEP (-0x100L)

#define UND_SCROLL0  0x3f00L  /* 0x002e0afc, measured - see 3. */

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x398 - two a tick, so a full
 * pass of the forty keys
 * would want 5120 ticks; the cue ends the shot long before. */
#define UND_CURSOR_STEP 2L

/* ORIGIN 0x002d661e CamPath_AdvanceMode12, +0x84 - the path gives x
 * and y straight and z plus 5000. */
#define UND_CAM_Z_BIAS 0x1388L

/* ORIGIN 0x002cff88 CamPath_Setup, +0x2c - g_CamKeyCount12. */
#define UND_KEYS     40L

/* ORIGIN 0x002cc590 Space_RunPart, +0x32c - ENDS ON A CUE. MEASURED
 * as a musical span, 1394 against the demo's 1392. LANDMINE: the
 * cursor's range gives 1510 and is NOT this phase's length - it runs
 * on into phase 8. orb_clamp.txt 1. */
#define UND_PHASE7   1394L

/* ORIGIN 0x002cc590 Space_RunPart, +0x370 - PHASE 8 ENDS ON A MUSIC
 * CUE, NOT ON THE FUSE: its loop tests 0x00237232 and the fuse's flag
 * is 0x00237230. Reading the fuse gives 9621 ticks, three minutes.
 * MEASURED 285 instead. orb_clamp.txt 2. */
#define UND_PHASE8   285L
#define UND_PHASE9   7L
#define UND_TICKS    (UND_PHASE7 + UND_PHASE8 + UND_PHASE9)

/* OURS - each phase's length, as its cue or its fallback gives it. */
static s32 und_p7(void)
{
	return Parts_GateLength(&g_p7_gate, UND_PHASE7);
}

/* OURS */
static s32 und_p8(void)
{
	return Parts_GateLength(&g_p8_gate, UND_PHASE8);
}


/* ORIGIN 0x002d643c Strut_AnimateClaw - open at 2/4/2 a tick from
 * (0,0,0), latch at -0x1ee, then snap under constant acceleration.
 * THE FIRST STEP IS ON TICK 1, NOT 0, and the error shows only in the
 * snap's slope. THE SHUT TEST IS ON JOINT 0 ALONE. orb_clamp.txt 2. */
#define UND_OPEN_LIMIT  (-0x1eeL)
#define UND_SHUT_LIMIT  0x200L
#define UND_CLAW_ACCEL  2L
static const s32 UND_OPEN_RATE[SHIP_CLAW_JOINTS] = { -2L, -4L, -2L };

/* ORIGIN 0x002d1f26 Flash_DrawWhiteout - ONE GLOW BLOB, the level as
 * its RADIUS, clamped 0x64 while the ramp runs to 0x8c. It writes only
 * byte +2, so the white comes from the resolve. orb_clamp.txt 3. */
#define UND_WHITE_STEP  0x14L
#define UND_WHITE_FULL  0x8cL
#define UND_WHITE_CLAMP 0x64L
#define UND_FLASH_X     0x46L
#define UND_FLASH_Y     0x37L

/* ORIGIN 0x002cf550 - the claws keep the texture 2.10 swapped them to;
 * nothing swaps them back. */
#define UND_CLAW_TEX 3L

static Relic_Cam g_cv;
static s32 g_cam[SHIP_CAM];
static s32 g_cursor;
static s32 g_aim_x, g_aim_z;
static s32 g_scroll;
static s32 g_bend[SHIP_CLAW_JOINTS];
static s32 g_vel[SHIP_CLAW_JOINTS];
static s32 g_latched;
static s32 g_white;
static const u8 *g_path;
static s32 g_keys;
static int g_ready;

/* OURS - a path entry's component, clamped to the block. */
static s32 path_at(s32 key, int comp)
{
	if (key < 0) key = 0;
	if (key > g_keys + 1) key = g_keys + 1;
	return RD_S32(g_path + (size_t)key * 12 + (size_t)comp * 4);
}

/* ORIGIN 0x002d5804 CamPath_EvalTriple, +0x20 - the same evaluator and
 * the same Horner form the other paths use, t in 0.8.
 *
 * THE +1 ON THE KEY is the path's duplicated first entry: the reader
 * takes the block from +0xc, so entry 0 is P[-1] and key n sits at
 * entry n+1 [verified, 0x002d661e +0x32]. */
static s32 path_eval(s32 key, s32 t, int comp)
{
	s32 pm = path_at(key, comp), p0 = path_at(key + 1, comp);
	s32 p1 = path_at(key + 2, comp), p2 = path_at(key + 3, comp);
	s32 a = -pm + 3 * p0 - 3 * p1 + p2;
	s32 b = 2 * pm - 5 * p0 + 4 * p1 - p2;
	s32 c = -pm + p1, d = 2 * p0;
	s32 v = ((a * t) >> 8) + b;

	v = ((v * t) >> 8) + c;
	v = ((v * t) >> 8) + d;
	return v >> 1;
}

/* ORIGIN 0x002d661e CamPath_AdvanceMode12 - the key, the spline, and
 * the two sine aim angles. The wrap is transcribed but unreachable:
 * forty keys at two a tick outlast the shot. */
static void path_camera(void)
{
	s32 key = g_cursor >> 8;
	s32 t = g_cursor & 0xffL;
	int i;

	if (key >= g_keys) {
		g_cursor = 0;
		key = 0;
		t = 0;
		g_cv.yaw = -0x120L;
		g_cv.pitch = 0;
	}
	for (i = 0; i < 3; i++) g_cam[i] = path_eval(key, t, i);
	g_cam[2] += UND_CAM_Z_BIAS;

	/* ORIGIN 0x002d661e CamPath_AdvanceMode12, +0x8a - TWO SINES OFF THE
	 * CURSOR AT DIFFERENT RATES
	 * are NOT the angles, they are an AIM OFFSET. Camera_TrackTarget
	 * adds them to the camera's x and z to make a point to look at and
	 * turns the direction into the yaw and pitch, so the look drifts
	 * around the orb rather than being pointed at it.
	 *
	 * Reading them as angles is the trap: the position then matches
	 * the demo exactly and the orientation is wrong, which looks like
	 * a path problem and is not. */
	g_aim_x = Gfx_Sin((g_cursor * 8L) & 0x1fffL) >> 4;
	g_aim_z = (Gfx_Sin((g_cursor * 4L) & 0x1fffL) >> 3) - 0x2c3L;
	Space_CamTrack(&g_cv, g_cam, g_aim_x, g_aim_z);
	g_cam[3] = g_cv.yaw;
	g_cam[4] = g_cv.pitch;
}

/* ORIGIN 0x002d643c Strut_AnimateClaw - D0 = 1 licenses the latch. */
static void claw_drive(void)
{
	int j;

	if (!g_latched) {
		if (g_bend[0] > UND_OPEN_LIMIT) {
			for (j = 0; j < SHIP_CLAW_JOINTS; j++)
				g_bend[j] += UND_OPEN_RATE[j];
			Ship_SetClawBend(g_bend);
			return;
		}
		g_latched = 1;
		for (j = 0; j < SHIP_CLAW_JOINTS; j++) g_vel[j] = 0;
	}
	/* Past the limit it returns untouched, so the pose holds shut. */
	if (g_bend[0] >= UND_SHUT_LIMIT) return;
	for (j = 0; j < SHIP_CLAW_JOINTS; j++) {
		g_bend[j] += g_vel[j];
		g_vel[j] += UND_CLAW_ACCEL;
	}
	Ship_SetClawBend(g_bend);
}

/* OURS */
static int und_init(void)
{
	g_cv.yaw = UND_YAW0;
	g_cv.pitch = UND_PITCH0;
	/* g_HeadAngle is inherited across every cut; UNMEASURED here, so
	 * the sky does not rotate yet. subsystems/effects.txt 11. */
	g_cv.head = 0;
	g_cv.angle = 0;
	g_cv.angle2 = 0;
	g_cursor = UND_CURSOR0;

	if (Space_Init()) return 1;
	Ship_Init();
	g_path = Geom_CamPath(1, &g_keys);
	if (!g_path) return 1;
	if (g_keys > UND_KEYS + 2) g_keys = UND_KEYS;

	Gfx_SetLightBase(UND_LIGHT_R, UND_LIGHT_G, UND_LIGHT_B);
	Ship_SetClawTexture(UND_CLAW_TEX);
	/* Revealed in 2.10's phase 3 and never hidden, so already on.
	 * [open] SHADING WRONG - palette block mismatched. orb.txt 9. */
	Ship_SetAdditiveLit(1);
	g_scroll = UND_SCROLL0;
	Ship_SetFlatScroll(g_scroll);
	g_latched = 0;
	g_white = 0;
	{ int j; for (j = 0; j < SHIP_CLAW_JOINTS; j++) {
		g_bend[j] = 0; g_vel[j] = 0; } }
	/* THE CLAW POSE IS INHERITED, NOT SET. Mode 0x12 never calls
	 * Strut_SetClawPose, so the claws hold whatever 2.10's animation
	 * left them at and nothing here may touch them.
	 *
	 * [open] RUN ON ITS OWN, THIS PART SHOWS Ship_Init's POSE, because
	 * nothing has animated the claws first. Picking a pose here would
	 * be inventing one - the demo's comes out of the previous shot's
	 * last tick, which is itself a measured stand-in for a music cue -
	 * so it is left alone until the parts are chained. */
	OrbScene_Reset();
	/* EVERYTHING IS ALREADY ON. Mode 0x12 never writes a beam
	 * strength, so both groups hold the full 0x20 the last shot left
	 * them at, all six are live, and the flares and the orb were
	 * revealed in phases 3 and 6. */
	OrbScene_SetBeams(BEAM_COUNT, BEAM_FULL, BEAM_FULL);
	OrbScene_SetLit(1, 1);
	path_camera();
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x386 - anim mode 0x12. The
 * cursor, and the clocks that
 * are shared with mode 0x13.
 *
 * [open] 0x002e0afc goes down 0x100 a tick here and 0x002e0c1c up
 * 0x14; neither is read by anything transcribed. */
static void und_step(s32 tick)
{
	s32 p7, p8;

	Parts_GateStep(&g_p7_gate, tick, UN_P7_ORDER, UN_P7_ROW, UND_PHASE7);
	p7 = und_p7();
	if (g_p7_gate.at)
		Parts_GateStep(&g_p8_gate, tick - p7, UN_P8_ORDER, UN_P8_ROW,
		               UND_PHASE8);
	p8 = und_p8();
	if (!g_ready) return;
	/* THE PATH IS READ BEFORE THE CURSOR MOVES [verified,
	 * Anim_SceneTick +0x38c then +0x398], so the camera a frame shows
	 * belongs to the cursor's PREVIOUS value. One tick the other way
	 * and every position is one step ahead of the demo's. */
	path_camera();
	g_cursor += UND_CURSOR_STEP;
	g_scroll = (g_scroll + UND_SCROLL_STEP) & 0xffffL;
	Ship_SetFlatScroll(g_scroll);
	OrbScene_Step();
	/* The first claw step is the tick AFTER phase 8 opens, hence >. */
	if (tick > p7) claw_drive();
	if (tick >= p7 + p8) {
		g_white += UND_WHITE_STEP;
		if (g_white > UND_WHITE_FULL) g_white = UND_WHITE_FULL;
	}
}

/* ORIGIN 0x002cd892 Scene_RenderFrame - the sky, then every object
 * through one sort. NO TURBULENCE STEP: mode 0x12 does not walk the
 * two cursors, and nothing in this shot samples them because no beam
 * is still growing. */
static void und_frame(Relic_Frame *f, s32 tick)
{
	s32 cam[SHIP_CAM], dyaw, dpitch;

	Gfx_SetLightBase(UND_LIGHT_R, UND_LIGHT_G, UND_LIGHT_B);
	Frame_Clear(f);
	if (!g_ready) return;
	/* THE LOOK OF approach.txt 8. There is no Space_SetCamera in this
	 * shot because nothing projects through one - no sun, no planet -
	 * so the offset reaches the sky's pan and the scene's camera and
	 * nothing else. The flash below is at a fixed screen place and
	 * stays where it is. */
	Space_Look(&dyaw, &dpitch);
	Space_DrawSky(f, Space_SkyPan(g_cv.angle + dyaw * SPACE_LOOK_SKY,
	                              g_cv.angle2 + dpitch * SPACE_LOOK_SKY),
	              g_cv.head);
	/* [open] WE DRAW LESS LIGHT THAN THE DEMO LATER IN THIS SHOT:
	 * byte +2 matches at tick 500 and is two thirds of the demo's by
	 * 700 - effects/orb_underside.txt 4. */
	OrbScene_SetBeams(BEAM_COUNT, BEAM_FULL, BEAM_FULL);
	OrbScene_SetLit(1, 1);
	Ship_LookCam(cam, g_cam, dyaw, dpitch);
	OrbScene_Register(cam, UND_OBJ_ANGLE);
	Ship_Draw(f, UND_OBJ_ANGLE, cam);
	/* ORIGIN 0x002cd892 - last, after the sort that sets it. */
	Space_DrawOrbBand(f);
	/* ORIGIN 0x002cd8fe Scene_RenderFlash - one call appended to
	 * Scene_RenderFrame, so the flash lands OVER the whole scene. */
	if (tick >= UND_PHASE7 + UND_PHASE8)
		Gfx_GlowBlob(f, UND_FLASH_X, UND_FLASH_Y,
		             g_white > UND_WHITE_CLAMP ? UND_WHITE_CLAMP : g_white,
		             g_RampBolt);
}

/* OURS */
static void und_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "cur %ld.%02ld key %ld/%ld cam %ld,%ld,%ld"
	             " | bend %ld,%ld,%ld %s | white %ld | ship %ld",
	        (long)(g_cursor >> 8), (long)(g_cursor & 0xffL),
	        (long)(g_cursor >> 8), (long)g_keys,
	        (long)g_cam[0], (long)g_cam[1], (long)g_cam[2],
	        (long)g_bend[0], (long)g_bend[1], (long)g_bend[2],
	        g_latched ? "SNAP" : "open", (long)g_white,
	        (long)Ship_FacesDrawn());
}

/* OURS - parts.h. */
static s32 und_length(void)
{
	return und_p7() + und_p8() + UND_PHASE9;
}

const Relic_Part PART_Underside = {
	"2.11", "The orb from underneath, the clamp and the flash",
	"Space_RunPart 0x002cc840 phases 7-9, anim modes 0x12 0xa 0xb",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	und_init, und_step, und_frame, und_stats,
	UND_TICKS, 0, und_length
};
