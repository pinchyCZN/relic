/* RELIC - scene one of the approach: the planet lit, and the light.
 *
 * ORIGIN 0x002cd102 maybe_Space_RunPlanetScenes, frame 0x002cd838
 * Space_DrawSunFrame, motion 0x002d5c66 Anim_SceneTick mode 2.
 *
 * This is timeline 2.3's second half - the same backdrop as part 2.3
 * with the shadow sweep gone, a white base colour, and a staged light
 * growing in front of it. The backdrop is space.c; what is here is
 * the scene's own parameters, its motion and the portal glow.
 *
 * [open] THE LIGHT ITSELF IS NOT DRAWN. maybe_Light_DrawStages
 * (0x002d6eb0) rotates four corners and fills between them in four
 * sectors, and each pixel is an indirect call through a node of the
 * octree built from Fsprout - so the per-pixel work is a property of
 * that data and is not identified yet. effects/approach.txt section
 * 4.3. The light's position IS tracked below, so the part reports
 * where it would be.
 */

#include <stdio.h>
#include <stdlib.h>

#include "parts.h"
#include "gfx.h"
#include "image.h"
#include "space.h"
#include "sprout.h"

/* The music cue the scene ends on - armed at 0x002cd270, polled at
 * 0x002cd280. parts.h Relic_Gate. */
#define AP_CUE_ORDER 0x04L
#define AP_CUE_ROW   0x10L

/* AND THE SCENE IS TWO LOOPS, NOT ONE. The first ends on (4,0) -
 * armed at 0x002cd220, polled at 0x002cd250 - and what the demo does
 * between them is stage the portal phase, so that cue is what opens
 * the glow. approach.txt 4.1. */
#define AP_STAGE_ORDER 0x04L
#define AP_STAGE_ROW   0L
#define AP_STAGE_A     148L

static Relic_Gate g_stage_gate;
static Relic_Gate g_cue_gate;
static int g_replaying;             /* OURS: approach_replay runs */

/* The sequencer's setup, every value read out of 0x002cd102. The sun
 * is on the other side of the frame from 2.3's and the planet is
 * nearer the middle. */
#define SUN_WORLD_X    720L         /* g_SunWorldX  0x2d0            */
#define SUN_WORLD_Y     66L         /* g_SunWorldY  0x42             */
#define BODY_WORLD_X  (-200L)       /* 0x002e0c54   0xffffff38       */
#define BODY_WORLD_Y   (-50L)       /* 0x002e0c58   0xffffffce       */

/* ORIGIN 0x002d5c66 mode 2 - the scene's whole motion, in the VERTB
 * handler. The sun drifts 3 a tick here where 2.3 drifts 2, and the
 * camera angle steps 6000 where 2.3 steps 0xfa0. */
#define SUN_DRIFT_RATE    3L
#define CAM_ANGLE_RATE 6000L
#define CAM_ANGLE_START   0L        /* 0x002e0ad8 = 0, not 0x400000  */

/* The light's position and its per-tick step, from the sequencer and
 * mode 2. THE SECOND CELL IS THE DEPTH, not the third:
 * maybe_Light_Project divides by it and returns without drawing when
 * it is not positive. It falls 6 a tick from 1205, so about 200 ticks
 * of approach; the third is the vertical screen axis and crossing
 * zero at tick 32 only means the light passes the frame's middle. */
#define LIGHT_X0       (-45L)       /* g_LightWorldX  0x002e0ae0     */
#define LIGHT_Z0       1205L        /* g_LightWorldZ  0x002e0ae4     */
#define LIGHT_Y0        160L        /* g_LightWorldY  0x002e0ae8     */
#define LIGHT_DX          3L
#define LIGHT_DZ        (-6L)
#define LIGHT_DY        (-5L)

/* ORIGIN 0x002cd63e - a data longword living in the code hunk, four
 * bytes before maybe_Portal_DrawGlow and read by its add.l. The
 * sequencer sets it to -180 and mode 2 adds 2 a tick. */
#define GLOW_OFF0    (-180L)
/* measured; the last tick is 231 - approach.txt 4.1 */
#define APPROACH_TICKS 242L     /* measured - parts.txt 3.1 */

/* What the step leaves for the frame. */
static s32 g_range_prev;
static s32 g_second_cx, g_second_cy;
static int g_second_on;
static s32 g_portal_phase;          /* g_PortalPhase, 0 then 2..0xfe  */
static s32 g_glow_off;              /* 0x002cd63e                     */
static s32 g_light_x, g_light_y, g_light_z;
/* ORIGIN 0x002e0a02, 06 and 0a - the direction the object is lit
 * from, which maybe_Sprout_Animate RECOMPUTES every tick rather than
 * storing. maybe_Light_DrawStages then shifts each component up a
 * byte so it lands in the product table's high half. The middle one
 * is never written and stays zero. */
static s32 g_light_dir[3];
/* g_SproutSpin 0x002e0a20, and the camera pair 0x002e0b32 / 36. */
static s32 g_spin, g_cam_yaw, g_cam_pitch;
/* g_CameraAngle and g_CameraAngle2 - THE ANGLE THE SKY PANS BY, fed
 * by both mode 2's own step and the camera move, plus the heading
 * g_CameraHeading the move accumulates. */
static s32 g_cam_angle, g_cam_angle2, g_cam_head;
static s32 g_light_px, g_light_sx, g_light_sy, g_light_r;
static s32 g_glow_px;
/* g_LightProjX / Y / g_LightDepth, the projected form the gate reads. */
static s32 g_light_proj_x, g_light_proj_y, g_light_depth;
static s32 g_st_fx, g_st_fy, g_st_bx, g_st_by;
static s32 g_look_yaw, g_look_pitch;    /* OURS: the drag, space.h */

/* ORIGIN 0x002d671c maybe_Camera_MoveRotated - it adds the deltas to
 * the camera pair, accumulates a heading, and then feeds the same
 * deltas through that heading into the angle the sky pans by. The
 * shifts are the demo's: >> 6 into the heading, >> 14 for the pitch
 * term, and only >> 1 after the heading rotation - so the angle moves
 * in thousands a tick where mode 2's own step is 6000. */
static void cam_move(s32 d0, s32 d1)
{
	s32 p, c, sn, h, a, b;

	g_cam_yaw += d0;
	g_cam_pitch += d1;
	d1 = -d1;

	p = (-g_cam_pitch) & 0x1fffL;
	g_cam_head += (d0 * Gfx_Sin(p)) >> 6;
	d0 = (d0 * Gfx_Cos(p)) >> 14;

	h = (g_cam_head >> 8) & 0x1fffL;
	c = Gfx_Cos(h);
	sn = Gfx_Sin(h);
	a = ((d0 * c) - (d1 * sn)) >> 1;
	b = ((d0 * sn) + (d1 * c)) >> 1;
	g_cam_angle += a;
	g_cam_angle2 -= b;
}

/* ORIGIN 0x002d4396 maybe_Camera_Rotate2Axis - a yaw in the
 * (x, depth) plane then a pitch in the (depth, y) one. The demo takes
 * the 14-bit product down by 8 then 6. The angles are parameters
 * because the frame turns the same point through the look offset. */
static void cam_rotate(s32 yaw, s32 pitch, s32 *x, s32 *z, s32 *y)
{
	s32 c = Gfx_Cos(yaw & 0x1fffL);
	s32 sn = Gfx_Sin(yaw & 0x1fffL);
	s32 a = *x, b = *z;

	*x = (a * c + b * sn) >> 14;
	*z = (b * c - a * sn) >> 14;
	c = Gfx_Cos(pitch & 0x1fffL);
	sn = Gfx_Sin(pitch & 0x1fffL);
	a = *z; b = *y;
	*z = (a * c + b * sn) >> 14;
	*y = (b * c - a * sn) >> 14;
}

/* ORIGIN 0x002cec64 maybe_Light_Project - THE DEPTH IT TESTS IS THE
 * TRANSFORMED ONE. The raw z goes negative at tick 201, so gating on
 * that freezes the object. A rejection leaves all three outputs
 * alone, which is how the demo keeps the last good triple.
 * approach.txt 4.2. */
static int light_project(s32 yaw, s32 pitch, s32 *px, s32 *py, s32 *depth)
{
	s32 x = g_light_x, z = g_light_z, y = g_light_y >> 5;

	cam_rotate(yaw, pitch, &x, &z, &y);
	if (z <= 0) return 0;
	*depth = z;
	*px = (x << 12) / z;
	*py = (y << 12) / z;
	return 1;
}

/* OURS. The scene's whole starting state, in one place because two
 * callers need it: approach_init, and the replay Approach_* runs. */
static void approach_reset(void)
{
	g_portal_phase = 0;
	g_glow_off = GLOW_OFF0;
	g_light_x = LIGHT_X0;
	g_light_y = LIGHT_Y0;
	g_light_z = LIGHT_Z0;
	g_light_proj_x = g_light_proj_y = g_light_depth = 0;
	g_spin = 0;
	g_cam_yaw = 0;
	g_cam_pitch = 0;
	g_cam_angle = 0;
	g_cam_angle2 = 0;
	g_cam_head = 0;
}

/* The statics the replay disturbs, so it can put them back. */
typedef struct { s32 v[13]; } Approach_Snap;

/* OURS */
static void approach_save(Approach_Snap *s)
{
	s->v[0] = g_portal_phase; s->v[1] = g_glow_off;
	s->v[2] = g_light_x;      s->v[3] = g_light_y;
	s->v[4] = g_light_z;      s->v[5] = g_spin;
	s->v[6] = g_cam_yaw;      s->v[7] = g_cam_pitch;
	s->v[8] = g_cam_angle;    s->v[9] = g_cam_angle2;
	s->v[10] = g_cam_head;
	s->v[11] = g_light_proj_x; s->v[12] = g_light_proj_y;
}

/* OURS */
static void approach_load(const Approach_Snap *s)
{
	g_portal_phase = s->v[0]; g_glow_off = s->v[1];
	g_light_x = s->v[2];      g_light_y = s->v[3];
	g_light_z = s->v[4];      g_spin = s->v[5];
	g_cam_yaw = s->v[6];      g_cam_pitch = s->v[7];
	g_cam_angle = s->v[8];    g_cam_angle2 = s->v[9];
	g_cam_head = s->v[10];
	g_light_proj_x = s->v[11]; g_light_proj_y = s->v[12];
}

/* OURS */
static int approach_init(void)
{
	if (Space_Init()) return 1;
	/* OPTIONAL, and the only asset in the port that is not in the
	 * executable: the light's octree is the Fsprout chunk. Without
	 * relic.dat the rest of the scene still draws. */
	Sprout_Init();
	approach_reset();
	return 0;
}

/* ORIGIN 0x002cd642 maybe_Portal_DrawGlow - the body is shared
 * with the departure shot and lives in space.c. */
static void portal_glow(Relic_Frame *f)
{
	Space_PortalGlow(f, g_portal_phase, g_glow_off);
	g_glow_px = Space_PortalGlowPx();
}

/* OURS. Mode 2, once a tick. See demo.c for why this is not in the
 * frame.
 *
 * THE PORTAL RAMP ARMS ON (4,0), which is where the demo stages the
 * phase - 0x002cd262, between its two loops - and the cue's own tick
 * is the one the phase is 2 on. At tick 0 it would bloom beside the
 * sun, before the camera has turned to it. approach.txt 4.1.
 *
 * AND A REPLAY MUST ARM NOTHING: it runs from inside the NEXT part's
 * tick, where Parts_GateStep would re-arm on its first. */
static void approach_step(s32 tick)
{
	s32 fx, fy, bx, by;

	if (!g_replaying) {
		Parts_GateStep(&g_stage_gate, tick, AP_STAGE_ORDER,
		               AP_STAGE_ROW, AP_STAGE_A);
		if (g_stage_gate.at)
			Parts_GateStep(&g_cue_gate, tick - g_stage_gate.at,
			               AP_CUE_ORDER, AP_CUE_ROW,
			               APPROACH_TICKS - AP_STAGE_A);
	}

	/* TICK 0 IS THE SEEDED STATE AND STEPS NOTHING, so g_LightWorldX
	 * is -0x2d + 3t exactly. Stepping it here too runs the whole
	 * scene a tick ahead of the demo. */
	if (tick <= 0) {
		g_light_x = LIGHT_X0;
		g_light_y = LIGHT_Y0;
		g_light_z = LIGHT_Z0;
		return;
	}

	/* THE FRAME ROUTINES LATCH THE CAMERA at their top - 0x002e0b2a
	 * and 0x002e0b2e are the copies Scene_ProjectAndDrawSun's
	 * transform reads - so the whole backdrop is projected through one
	 * camera and moves in unison. */
	Space_SetCamera(g_cam_yaw, g_cam_pitch);
	if (!Space_Project(SUN_WORLD_X + tick * SUN_DRIFT_RATE,
	                   SUN_WORLD_Y, &fx, &fy))
	    fx = fy = -0x4000;      /* off any screen, so no cell counts */
	Space_Project(BODY_WORLD_X, BODY_WORLD_Y, &bx, &by);


	/* ORIGIN 0x002d68da maybe_Sprout_Animate - the object spins and
	 * its lighting turns with it, which is why the light direction is
	 * not a constant. */
	g_spin = (g_spin + 0x3cL) & 0x1fffL;
	{
		s32 t = (0x3c00L - g_spin + g_cam_yaw) & 0x1fffL;
		g_light_dir[0] = ((0x7fL * Gfx_Cos(t)) >> 14) << 8;
		g_light_dir[1] = 0;
		g_light_dir[2] = ((0x7fL * Gfx_Sin(t)) >> 14) << 8;
	}

	/* ORIGIN 0x002d5d9e - THE CAMERA TRACKS THE OBJECT'S PROJECTED X.
	 *
	 *     D2 = g_LightProjX + 0x64
	 *     skip everything if that is negative
	 *     skip it too if g_CameraYaw < -0xaca
	 *     yaw += -(D2 >> 5),  pitch += 0
	 *
	 * So the further right the object has got, the faster the camera
	 * yaws the other way - it pans one direction and accelerates,
	 * and it never comes back. g_LightProjX is 12.4 fixed.
	 *
	 * The gate reads the PREVIOUS tick's projection, because mode 2
	 * tests it before moving the object, so this runs before the step
	 * below rather than after. */
	{
		s32 d2 = g_light_proj_x + 0x64L;
		/* a negative d2 still steps the angle - approach.txt 4 */
		if (d2 < 0) {
			g_cam_angle += 0x1770L;
		} else if (g_cam_yaw >= -0xacaL) {
			cam_move(-(d2 >> 5), 0);
			g_cam_angle += 0x1770L;
		}
	}

	g_light_x += LIGHT_DX;
	g_light_y += LIGHT_DY;
	g_light_z += LIGHT_DZ;

	light_project(g_cam_yaw, g_cam_pitch, &g_light_proj_x,
	              &g_light_proj_y, &g_light_depth);
	if (g_stage_gate.at && !g_portal_phase) {
		g_portal_phase = 2;
		g_glow_off = GLOW_OFF0;
	} else if (g_portal_phase && g_portal_phase < 0xfe) {
		g_portal_phase += 2;
		g_glow_off += 2;
	}
}

/* OURS. Space_DrawSunFrame's eight steps; step 6 is the light.
 *
 * THE LOOK OF approach.txt 8 ENTERS HERE AND NOWHERE ELSE - the
 * camera, the sky's pan, and a SECOND projection of the object. That
 * last one is why the step keeps its own: an offset reaching the
 * camera gate there would bend the scene's path for good. */
static void approach_frame(Relic_Frame *f, s32 tick)
{
	s32 fx, fy, bx, by, radius, ang;
	s32 lpx, lpy, ldepth;
	int sun_ok, body_ok;

	/* THE SKY PANS BY THE ACCUMULATED CAMERA ANGLE, not by a formula
	 * in the tick: mode 2 steps it 0x1770 and the camera move adds a
	 * rotated term that dwarfs that, which is why the starfield here
	 * travels the opposite way from part 2.3's and far faster. */
	Space_Look(&g_look_yaw, &g_look_pitch);
	ang = g_cam_angle;
	Space_SetCamera(g_cam_yaw + g_look_yaw, g_cam_pitch + g_look_pitch);
	Gfx_SetFlareRayPhase(ang >> 12);
	/* ORIGIN 0x002cd102 - Ham8_BuildTables(0x3f,0x3f,0x3f). THE WHITE
	 * BASE IS WHY THIS SCENE IS LIT: the same light byte that darkens
	 * in 2.3 brightens toward white here. */
	Gfx_SetLightBase(0x3f, 0x3f, 0x3f);
	Frame_Clear(f);

	Space_DrawSky(f, Space_SkyPan(ang + g_look_yaw * SPACE_LOOK_SKY,
	                              g_look_pitch * SPACE_LOOK_SKY), 0L);
	portal_glow(f);

	/* NOTHING IS DRAWN FOR A REJECTED PROJECTION. This scene's camera
	 * turns far enough to put the sun and then the planet behind the
	 * eye, and the demo draws neither once that happens. */
	sun_ok = Space_Project(SUN_WORLD_X + tick * SUN_DRIFT_RATE,
	                       SUN_WORLD_Y, &fx, &fy);
	body_ok = Space_Project(BODY_WORLD_X, BODY_WORLD_Y, &bx, &by);

	radius = 0x50 - (g_range_prev >> 8);
	if (sun_ok && radius > 0)
		Gfx_Flare(f, fx, fy, radius, g_RayArcs, g_ColRampWarm);

	/* ORIGIN steps 4 to 7 - THE SAVE-UNDER BRACKETS THE PLANET AND
	 * THE OBJECT BOTH, and the object is the reason it has to: drawn
	 * between the stamp and the count, its voxels destroy sentinels
	 * and the count falls, which shrinks the flare. That is how the
	 * object controls how much of the sun bleeds past it - clipping
	 * the edge barely dims it, sitting squarely in front nearly shuts
	 * it out. See effects/approach.txt section 4.4.9. */
	g_range_prev = Space_Range();
	if (sun_ok)
		Space_StampSaveUnder(f, fx, fy);

	if (body_ok)
		Space_DrawPlanetAndHalo(f, bx, by);

	/* ORIGIN 0x002cec14 maybe_Light_DrawIfOnScreen - step 6, the
	 * light. The projection is maybe_Light_Project: the second world
	 * coordinate is the DEPTH, and the radius is a perspective
	 * divide, so the object grows as it closes. */
	lpx = g_light_proj_x; lpy = g_light_proj_y; ldepth = g_light_depth;
	if (g_look_yaw || g_look_pitch) {
		s32 yaw = g_cam_yaw + g_look_yaw;
		s32 pitch = g_cam_pitch + g_look_pitch;

		if (!light_project(yaw, pitch, &lpx, &lpy, &ldepth))
			ldepth = 0;
	}

	if (Sprout_Ready() && ldepth > 0) {
		/* THE SCREEN X IS CENTRED ON 80 and the Y PASSED IS THE TOP,
		 * not the centre. The gate halves the scale only for its
		 * bounds test - 0x002cec4a keeps the full value in D2 and
		 * 0x002cec4c halves a copy - then hands over y - half. Using
		 * the halved scale draws the object at half size and placing
		 * it on y rather than y - half sets it half a radius low. */
		s32 lx = (lpx >> 4) + 0x50;
		s32 ly = (lpy >> 4) + 0x32;
		s32 r = 0x6400L / ldepth;
		s32 half = r >> 1;

		g_light_px = 0;
		if (lx >= 0 && lx < 0xa0 && ly >= half && ly < 100 - half)
			/* THE SPIN, not the scene's camera angle:
			 * maybe_Light_DrawStages copies g_SproutSpin into
			 * g_SproutAngle and that is what turns the bounding
			 * square. */
			g_light_px = Sprout_Draw(f, lx, ly - half, r, g_spin,
			                         g_light_dir);
		g_light_sx = lx; g_light_sy = ly; g_light_r = r;
	}

	/* ORIGIN 0x002cd838 step 8 - AFTER the object, and additive, so
	 * the sun burns through it - approach.txt section 4.4.8. */
	/* and the count AFTER both of them */
	if (sun_ok) {
		s32 cx = fx, cy = fy;
		g_second_on = Space_EaseRange(Space_CountSaveUnder(f, &cx, &cy));
		if (g_second_on) {
			g_second_cx = cx;
			g_second_cy = cy;
		}
		Space_StepBand(g_second_on ? cy : -1);
	}

	if (g_second_on && sun_ok) {
		s32 r2 = Space_Range() >> 8;
		if (r2 > 0)
			Gfx_Flare(f, g_second_cx, g_second_cy, r2,
			          g_RaySpikes, g_ColRampWarmB);
	}


	if (sun_ok)
		Space_DrawFlareGhosts(f, fx, fy);

	g_st_fx = fx; g_st_fy = fy;
	g_st_bx = bx; g_st_by = by;
}

/* OURS. The light's position is its WORLD position. Its perspective
 * radius is 0x6400 / g_LightDepth, and g_LightDepth is what
 * FUN_002cec64 projects this triple into - unread, so there is no
 * radius to report and dividing by the world z here would be a number
 * of our own invention. */
static void approach_stats(char *out, int cap)
{
	char sp[80];

	(void)cap;
	Sprout_Stats(sp, (int)sizeof sp);
	sprintf(out, "glow %ld  sun x%ld  rock x%ld r%ld px%ld  range %04lx",
	        (long)g_glow_px,
	        (long)g_st_fx,
	        (long)g_light_sx, (long)g_light_r, (long)g_light_px,
	        (long)Space_Range());
	(void)sp;
}

/* OURS - parts.h. */
static s32 approach_length(void)
{
	s32 a = Parts_GateLength(&g_stage_gate, AP_STAGE_A);

	if (a >= PARTS_GATE_OPEN) return a;
	return a + Parts_GateLength(&g_cue_gate, APPROACH_TICKS - AP_STAGE_A);
}

const Relic_Part PART_Approach = {
	"2.3b", "Planet lit, and the light",
	"maybe_Space_RunPlanetScenes 0x002cd102",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	approach_init, approach_step, approach_frame, approach_stats,
	APPROACH_TICKS, 0, approach_length
};

/* OURS. The next scene's sparks seed from where this one's object
 * ends, which the accumulated pan leaves with no closed form, so this
 * replays the step - no drawing - to find it. portal.txt 6. */
static void approach_replay(void)
{
	s32 t;
	approach_reset();
	g_replaying = 1;
	for (t = 1; t < APPROACH_TICKS; t++) approach_step(t);
	g_replaying = 0;
}

/* OURS. IT MUST RESET BEFORE REPLAYING AND RESTORE AFTER. approach_step
 * reseeds only the light's position, so replaying on top of a scene
 * that has already run leaves g_cam_yaw past its -0xaca gate: the pan
 * never runs, the projection comes out somewhere else entirely, and
 * the next scene's sparks fly the wrong way. Which way they flew used
 * to depend on whether the scene had been watched first. */
void Approach_FinalLightProj(s32 *proj_x, s32 *proj_y)
{
	Approach_Snap snap;
	approach_save(&snap);
	approach_replay();
	*proj_x = g_light_proj_x;
	*proj_y = g_light_proj_y;
	approach_load(&snap);
}

/* OURS. The pan the sky is baked with at the handover - 0x002d32dc
 * reads the same latched camera angle Scene_DrawTexturedSky does. */
s32 Approach_FinalSkyPan(void)
{
	s32 a, a2;
	Approach_FinalCamAngles(&a, &a2);
	return Space_SkyPan(a, a2);
}

/* OURS. THE HANDOVER MUST CARRY THESE OR THE SKY JUMPS when the
 * ripple stops. portal.txt 5.6. */
void Approach_FinalCamAngles(s32 *angle, s32 *angle2)
{
	Approach_Snap snap;
	approach_save(&snap);
	approach_replay();
	*angle = g_cam_angle;
	*angle2 = g_cam_angle2;
	approach_load(&snap);
}
