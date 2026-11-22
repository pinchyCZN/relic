/* RELIC - part 2.10, the orb forming.
 *
 * ORIGIN 0x002cc590 Space_RunPart. THAT ONE ROUTINE IS THE WHOLE ORB
 * SEQUENCE, not this part alone: it is a linear script that walks
 * g_SceneMode through 6, 0x13, 0x12, 0xa, 0xb and 0xc, raising
 * g_SceneObjectCount as it goes so geometry appears without anything
 * being loaded. docs/parts.txt 3.2 has the phase table.
 *
 * THIS FILE IS MODE 6 ONLY - the first phase, 200 ticks, where the
 * camera closes on the ship and the claws begin to open. The phases
 * after it need the lightning beams, the orb and the claws' opening
 * pose, none of which are transcribed.
 *
 * Its frame is Scene_RenderFrame (0x002cd892) and it is SHORTER than
 * the window scene's: the nine latch copies, Scene_DrawTexturedSky,
 * Scene_SortAndDrawAll, a bare rts, and the horizon band at
 * 0x002d2530 - which this phase has switched OFF through
 * 0x002e0bf8 = 1. So no sun, no flare, no save-under: sky and ship.
 */


#include <stdio.h>
#include "parts.h"
#include "gfx.h"
#include "dat.h"
#include "geom.h"
#include "ship.h"
#include "space.h"
#include "image.h"
#include "beam.h"
#include "orbscene.h"

/* The music cue the scene ends on - armed at 0x002cc818, polled at
 * 0x002cc82e. parts.h Relic_Gate. */
#define OB_CUE_ORDER 0x19L
#define OB_CUE_ROW   0L

static Relic_Gate g_cue_gate;

/* ORIGIN 0x002cc590 - the scene's own setup, every value read off it.
 *
 * THE LIGHT BASE IS BRIGHTER THAN SIX BITS. Ham8_BuildTables is called
 * with (0x3f, 0x4f, 0x6f) and clamps each channel at 0x3f00 after
 * shifting, so the green and blue arrive already saturated - the base
 * is white, reached sooner on those two channels than on red. */
#define ORB_LIGHT_R  0x3fL
#define ORB_LIGHT_G  0x4fL
#define ORB_LIGHT_B  0x6fL

/* ORIGIN 0x002cc590 - g_SceneFuseTicks, which Anim_SceneTick counts
 * down to raise g_QuitRequested, the flag each phase loop polls.
 *
 * THIS IS THE FRAME DELTA AND NOT THE FUSE'S 200: the object count
 * goes to 25 on frame 208 on hardware against mode 6 starting on
 * 7, so the loop's 200 plus a frame of transition. parts.txt 3.2. */
#define ORB_PHASE1   201L

/* ORIGIN 0x002cc590 - PHASES 2 TO 6 ARE ALL MODE 0x13 and all draw
 * through Scene_RenderFrame, so one part covers them. What changes is
 * g_SceneObjectCount and which counter ends each - and PHASES 2 AND 4
 * ARE THE TWO BEAM GROUPS GROWING, their exit counters being the
 * beams' own strengths. These lengths are MEASURED off a watch on the
 * count and are not the fuses; parts.txt 3.2 has the table. */
#define ORB_PHASE2   82L
#define ORB_PHASE3   22L
#define ORB_PHASE45  40L
#define ORB_MODE13   280L

/* ORIGIN 0x002e0afc g_FlatScroll - MEASURED, AND MODE 6 DOES NOT MOVE
 * IT. Hardware reads 0x5900 and four frames later still 0x5900, so
 * the value is static for all of phase 1 and where in phase 1 the
 * reading sits does not matter. Mode 0x13 then loses 0x100 a tick
 * [observed, on hardware], which over 280 ticks lands on 0x4100 - within two
 * ticks of the seed measured independently for 2.11, so the scroll
 * runs continuously across the cut. effects/orb.txt 9. */
#define ORB_SCROLL0  0x5900L
#define ORB_SCROLL_STEP (-0x100L)
#define ORB_TICKS    (ORB_PHASE1 + ORB_MODE13)

/* ORIGIN 0x002cc590 - the two beam strengths, seeded as each group's
 * phase begins. A is seeded NEGATIVE, which is the delay
 * before the first three even start to show: Beam_Draw draws nothing
 * under 0x10. */
#define ORB_BEAM_A0  (-0x32L)
#define ORB_BEAM_B0  0L
#define ORB_BEAM_FULL 0x20L

/* ORIGIN 0x002cf550 - g_ShipTexSlots[3], the claws' second texture. */
#define ORB_CLAW_TEX    3L

/* ORIGIN 0x002cc590 - the opening orientation. */
#define ORB_YAW0     0L
#define ORB_PITCH0   (-0x12cL)

/* ORIGIN 0x002cc590 - the object angle. THE LIVE AND LATCHED COPIES
 * DISAGREE at setup, 0x100 against 0x400, and nothing in mode 6's tick
 * moves the live one, so Scene_RenderFrame's latch copy replaces the
 * 0x400 with 0x100 on the first frame. */
#define ORB_OBJ_ANGLE 0x100L

/* ORIGIN 0x002cc590 - g_CamSrc, which this scene uses as the camera
 * position the way the window scene did.
 *
 * IT ONLY FEEDS THE TWO FRAMES BEFORE Space_DisplayOn. Mode 6's first
 * tick overwrites all three from the spline, so holding the camera
 * here makes the aim start from somewhere the demo is never at and
 * swing in - the snap. Kept because those two frames are real. */
#define ORB_CAM_X    0x1c54L
#define ORB_CAM_Y    (-0x3458L)
#define ORB_CAM_Z    0x25abL

/* ORIGIN 0x002d5c66 Anim_SceneTick's mode 6 arm - THE PATH IS IN
 * g_OrbCamPath, IN THE EXECUTABLE, NOT IN relic.dat. The arm loads
 * it directly and indexes it as
 * base + (cursor >> 8) * 12 with t = cursor & 0xff, so the layout is
 * the window path's - three longwords a key, and a pre-entry at
 * base - 12 that duplicates key 0 because CamPath_EvalCatmullRom
 * wants a P[-1].
 *
 * THE CURSOR ADVANCES ONE A TICK AND THE PHASE IS 200, SO THE KEY
 * NEVER CHANGES. The whole move is keyframe 0's segment walked to
 * t = 200 of 256: from (0, -6998, 4000) toward (12000, -12000, 9000).
 * Nothing here clamps the key, so a longer phase would walk on into
 * the table. */
#define ORB_PATH     0x002eea14UL
#define ORB_PATH_STRIDE 12L

/* ORIGIN 0x002cc590 - the track offsets. THE SETUP WRITES THEM TWICE
 * and these are the second pair, so the 0xa93 earlier in the routine
 * never applies. Mode 6 adds 0x14 to the z offset a tick, so the aim
 * point climbs. */
#define ORB_TRACK_Z  (-0xa00L)
#define ORB_TRACK_X  0x190L
#define ORB_TRACK_STEP 0x14L

/* ORIGIN 0x002d5c66 Anim_SceneTick's mode 0x13 arm - IT AIMS
 * DIFFERENTLY FROM MODE 6. The z offset goes DOWN ten a tick where
 * mode 6 took it up twenty, and the x offset is rewritten to 300
 * every tick rather than left at the setup's 400. Carrying mode 6's
 * rule through leaves the pitch about 190 out by the end of mode
 * 0x13 - the camera watches the ship from the wrong angle while the
 * lightning runs. Phase 1 ends on z 1460 and 138 ticks of -10 give
 * the 80 the demo reads [observed, on hardware at frame 348]. */
#define ORB_TRACK13_X    0x12cL
#define ORB_TRACK13_STEP (-10L)

/* ORIGIN 0x002cc590 - a countdown from 0x32 that Anim_SceneTick turns
 * into one Strut_SetClawPose call, so the claws start opening 50 ticks
 * in. */
#define ORB_CLAW_TICK 0x32L

/* ORIGIN 0x002d6388 Strut_SetClawPose - THREE JOINTS, THREE RATES.
 * Each joint keeps a phase, and the angle it hands the deformer is
 *
 *     bias + ((sin(min(phase, 0x800)) * -bias) >> 14)
 *
 * so phase 0 is the closed pose exactly and the clamp - a quarter
 * turn, where the table reads 0x4000 - is zero, a straight finger.
 * The phases start at (0, 0, 0) and step at different rates, which is
 * what makes the claw unfurl rather than snap open.
 *
 * THE PHASES DO NOT ALL START AT ZERO. Strut_ResetClawPhases seeds
 * (0, -0x800, -0x800) and the lookup is skipped while a phase is
 * negative, so the claw unfurls from the base outward. The file holds
 * zeros there, so reading it instead opens all three at once and in
 * the wrong order. geometry.txt 6.1. */
#define ORB_CLAW_CLAMP 0x800L
static const s32 ORB_CLAW_BIAS[SHIP_CLAW_JOINTS] = {
	0x700L, -0x380L, -0x580L
};
static const s32 ORB_CLAW_RATE[SHIP_CLAW_JOINTS] = { 0xaL, 0x14L, 0x28L };
static const s32 ORB_CLAW_SEED[SHIP_CLAW_JOINTS] = { 0L, -0x800L, -0x800L };

static Relic_Cam g_cv;
static s32 g_cam[SHIP_CAM];
static s32 g_track_z, g_track_x;
static s32 g_cursor;
static s32 g_claw_phase[SHIP_CLAW_JOINTS];
static s32 g_claw_count;
static s32 g_beam_a, g_beam_b;
static s32 g_live_beams;
static s32 g_attach_lit, g_orb_lit, g_scroll;
static int g_ready;

/* ORIGIN 0x002d6388 - one call of the pose. The lookup is SKIPPED
 * while a phase is negative, which leaves that joint on its bias; the
 * phase still advances. */
static void claw_pose(void)
{
	s32 bend[SHIP_CLAW_JOINTS];
	s32 j;

	for (j = 0; j < SHIP_CLAW_JOINTS; j++) {
		s32 ph = g_claw_phase[j];
		s32 term = 0;
		if (ph >= 0) {
			if (ph > ORB_CLAW_CLAMP) ph = ORB_CLAW_CLAMP;
			term = (Gfx_Sin(ph) * -ORB_CLAW_BIAS[j]) >> 14;
		}
		g_claw_phase[j] += ORB_CLAW_RATE[j];
		bend[j] = ORB_CLAW_BIAS[j] + term;
	}
	Ship_SetClawBend(bend);
	g_claw_count++;
}

/* ORIGIN g_OrbCamPath - one component of one keyframe, by signed
 * longword. The key may be negative: EvalCatmullRom reads P[-1]. */
static s32 path_at(s32 key, int comp)
{
	return (s32)Image_U32((u32)(ORB_PATH + key * ORB_PATH_STRIDE
	                            + comp * 4L));
}

/* ORIGIN 0x002d5824 CamPath_EvalCatmullRom, by Horner with t in 0.8 -
 * the same evaluator the window scene's path uses. */
static s32 path_eval(s32 key, s32 t, int comp)
{
	s32 pm = path_at(key - 1, comp), p0 = path_at(key, comp);
	s32 p1 = path_at(key + 1, comp), p2 = path_at(key + 2, comp);
	s32 a = -pm + 3 * p0 - 3 * p1 + p2;
	s32 b = 2 * pm - 5 * p0 + 4 * p1 - p2;
	s32 c = -pm + p1, d = 2 * p0;
	s32 v = ((a * t) >> 8) + b;

	v = ((v * t) >> 8) + c;
	v = ((v * t) >> 8) + d;
	return v >> 1;
}

/* ORIGIN 0x002d5c66 - the cursor's two halves, and the triple the
 * spline gives for them. */
static void path_camera(void)
{
	s32 key = g_cursor >> 8;
	s32 t = g_cursor & 0xffL;
	int i;

	for (i = 0; i < 3; i++) g_cam[i] = path_eval(key, t, i);
}

/* OURS */
static int orb_init(void)
{
	s32 j;

	g_cv.yaw = ORB_YAW0;
	g_cv.pitch = ORB_PITCH0;
	/* g_HeadAngle is inherited here as it is everywhere - the scenes
	 * before this one leave it where 2.4 put it. UNMEASURED for this
	 * scene, so the sky does not rotate yet: effects.txt 11. */
	g_cv.head = 0;
	g_cv.angle = 0;
	g_cv.angle2 = 0;
	g_track_z = ORB_TRACK_Z;
	g_track_x = ORB_TRACK_X;
	g_cursor = 0;
	for (j = 0; j < SHIP_CLAW_JOINTS; j++)
		g_claw_phase[j] = ORB_CLAW_SEED[j];
	g_claw_count = 0;
	path_camera();
	g_cam[3] = ORB_YAW0;
	g_cam[4] = ORB_PITCH0;
	g_beam_a = ORB_BEAM_A0;
	g_beam_b = ORB_BEAM_B0;
	g_live_beams = 0;
	g_attach_lit = 0;
	g_orb_lit = 0;
	Ship_SetClawTexture(-1L);
	Ship_SetAdditiveLit(0);
	g_scroll = ORB_SCROLL0;
	Ship_SetFlatScroll(g_scroll);
	/* The envelope arrives with phase 3's nine slots, not before. */
	Ship_SetAdditiveLit(0);
	OrbScene_Reset();
	/* ORIGIN 0x002cc4bc - draws 1024 random numbers doing it. */
	Gfx_BuildTurbulence();
	if (Space_Init()) return 1;
	Ship_Init();
	/* THE CLAWS MUST BE PUT BACK. Ship_Init returns early once the
	 * ship is loaded, so on a replay the claws keep whatever pose the
	 * last run left them in and the scene opens with them already
	 * spread. The demo never replays a scene, but Space_BuildObjects
	 * does re-deform them at every setup, so this is the same
	 * reset - Strut_SetClawPose's phase-0 pose. */
	{
		s32 closed[SHIP_CLAW_JOINTS];
		for (j = 0; j < SHIP_CLAW_JOINTS; j++)
			closed[j] = ORB_CLAW_BIAS[j];
		Ship_SetClawBend(closed);
	}
	/* ORIGIN 0x002cc6ae - the PLANET table, so object 11's decal is
	 * hidden again; this scene has no snapshot to put on it. */
	Ship_SetSkipTable(SHIP_SKIP_PLANET);
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, its mode 6 arm - the whole tick:
 *
 *     g_CamPathCursor += 1          ONE, where mode 5 adds two
 *     CamPath_EvalTriple
 *     g_CamTrackOffsetZ += 0x14
 *     g_CamSrc = the spline's triple
 *     Camera_TrackTarget
 *     the claw countdown, then Strut_SetClawPose
 *     the fuse
 *
 * THE CURSOR IS READ BEFORE IT IS BUMPED, so the first tick evaluates
 * at t = 0 and the camera starts exactly on keyframe 0. */
static void orb_step(s32 tick)
{
	Parts_GateStep(&g_cue_gate, tick, OB_CUE_ORDER, OB_CUE_ROW,
	               ORB_TICKS);
	if (tick <= 0) {
		orb_init();
		return;
	}
	path_camera();
	g_cursor += 1;
	if (tick > ORB_PHASE1) {
		g_track_z += ORB_TRACK13_STEP;
		g_track_x = ORB_TRACK13_X;
	} else {
		g_track_z += ORB_TRACK_STEP;
	}
	Space_CamTrack(&g_cv, g_cam, g_track_x, g_track_z);
	g_cam[3] = g_cv.yaw;
	g_cam[4] = g_cv.pitch;
	/* ORIGIN 0x002d5fca - the counter is decremented FIRST and the
	 * pose runs only once it has gone negative, so 0x32 gives a first
	 * call on tick 51 and one every tick after. */
	if (tick > ORB_CLAW_TICK) claw_pose();

	/* ORIGIN 0x002cc590 - mode 0x13, where the beams are live. */
	if (tick > ORB_PHASE1) {
		s32 t13 = tick - ORB_PHASE1;
		OrbScene_Step();
		Gfx_StepTurbulence();
		/* Phase 2 raises the count by 3, which is the first beam
		 * group; phase 4 raises it by 3 again for the second. */
		/* NOT CLAMPED: 0x20 is only Beam_Draw's growing
		 * threshold and the counters climb past it. */
		g_beam_a += 1;
		g_live_beams = 3;
		/* ORIGIN 0x002cc752 - phase 3's nine slots are the six attach
		 * flares AND the three envelope shells, so they arrive on the
		 * same tick. */
		if (t13 > ORB_PHASE2) {
			g_attach_lit = 1;
			Ship_SetAdditiveLit(1);
		}
		/* The scroll moves from mode 0x13 on, which is what makes the
		 * envelope flow rather than sit. */
		g_scroll = (g_scroll + ORB_SCROLL_STEP) & 0xffffL;
		Ship_SetFlatScroll(g_scroll);
		/* ORIGIN 0x002cc590 - phase 6 raises the count to 38, the orb
		 * being the last slot. MEASURED at frame 352, so tick 343 and
		 * t13 143 [observed, on hardware]. */
		if (t13 > ORB_PHASE2 + ORB_PHASE3 + ORB_PHASE45) {
			/* ORIGIN 0x002cc7d8 - the count going to 38, the claws
			 * changing texture and the orb appearing are the SAME
			 * tick, the phase 5 to 6 boundary. */
			g_orb_lit = 1;
			Ship_SetClawTexture(ORB_CLAW_TEX);
		}
		if (t13 > ORB_PHASE2 + ORB_PHASE3) {
			if (t13 == ORB_PHASE2 + ORB_PHASE3 + 1)
				/* phase 4 re-seeds it */
				g_beam_b = ORB_BEAM_B0;
			g_beam_b += 1;
			g_live_beams = 6;
		}
	}
}

/* ORIGIN 0x002cd892 Scene_RenderFrame - the latch copies, the sky and
 * every object. THE BAND AT THE END IS THE ORB'S, fed by the centre
 * flare inside the sort - docs/effects/orb.txt 11.2. No sun here. */
static void orb_frame(Relic_Frame *f, s32 tick)
{
	s32 cam[SHIP_CAM], dyaw, dpitch;

	(void)tick;
	Gfx_SetLightBase(ORB_LIGHT_R, ORB_LIGHT_G, ORB_LIGHT_B);
	Frame_Clear(f);
	if (!g_ready) return;
	/* THE LOOK OF approach.txt 8, AND THE WHOLE SHOT TAKES IT:
	 * nothing is baked here, and the beams and the satellites follow
	 * because OrbScene projects through the camera it is handed.
	 * g_cv and g_cam are the path's and orb_step owns both. */
	Space_Look(&dyaw, &dpitch);
	Space_SetCamera(g_cv.yaw + dyaw, g_cv.pitch + dpitch);
	Space_DrawSky(f, Space_SkyPan(g_cv.angle + dyaw * SPACE_LOOK_SKY,
	                              g_cv.angle2 + dpitch * SPACE_LOOK_SKY),
	              g_cv.head);
	OrbScene_SetBeams(g_live_beams, g_beam_a, g_beam_b);
	OrbScene_SetLit(g_attach_lit, g_orb_lit);
	Ship_LookCam(cam, g_cam, dyaw, dpitch);
	OrbScene_Register(cam, ORB_OBJ_ANGLE);
	Ship_Draw(f, ORB_OBJ_ANGLE, cam);
	/* ORIGIN 0x002cd892 - last, after the sort that sets it. */
	Space_DrawOrbBand(f);
}

/* OURS */
static void orb_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "cur %ld.%02ld cam %ld,%ld,%ld yaw %ld pit %ld"
	             " claw %ld beam %ld/%ld x%ld | ship %ld"
	             " | flare %ld orb %ld r%ld",
	        (long)(g_cursor >> 8), (long)(g_cursor & 0xffL),
	        (long)g_cam[0], (long)g_cam[1], (long)g_cam[2],
	        (long)g_cv.yaw, (long)g_cv.pitch,
	        (long)g_claw_phase[0],
	        (long)g_beam_a, (long)g_beam_b, (long)g_live_beams,
	        (long)Ship_FacesDrawn(),
	        (long)g_attach_lit, (long)g_orb_lit, (long)OrbScene_OrbRadius());
}

/* OURS - parts.h. */
static s32 orb_length(void)
{
	return Parts_GateLength(&g_cue_gate, ORB_TICKS);
}

const Relic_Part PART_Orb = {
	"2.10", "The orb forms, phase one", "Space_RunPart 0x002cc590 mode 6",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	orb_init, orb_step, orb_frame, orb_stats,
	ORB_TICKS, 0, orb_length
};
