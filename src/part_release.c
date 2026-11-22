/* RELIC part 2.13 - the orb release. ANIM MODES 0xd, 0x10 and 0x11,
 * Space_RunRelease's three phases.
 *
 * A long pass around the underside of the ship, then the claws open
 * and let the sphere go. The release is not a cut: each phase applies
 * a narrower skip table, so the hull sheds a group of object slots at
 * a time.
 *
 * The orb is a texture-mapped ball whose texels are gated on a
 * falling threshold, so it arrives brightest-first out of nothing -
 * and the shells then add quads and lines along a crack network that
 * the texture itself paints black, which is what reads as light
 * escaping from inside.
 *
 * See docs/effects/orb_release.txt and docs/formats/tddd.txt.
 */

#include <stdio.h>
#include "parts.h"
#include "gfx.h"
#include "rast.h"
#include "dat.h"
#include "ship.h"
#include "space.h"
#include "image.h"
#include "orb.h"

/* ORIGIN 0x002cbef8 - the ORB SEQUENCE'S light base, not the
 * explosion's, so the scene reads blue rather than blue-white. */
#define RL_LIGHT_R   0x3fL
#define RL_LIGHT_G   0x3fL
#define RL_LIGHT_B   0x4fL

/* ORIGIN 0x002cbef8 Space_RunRelease, +0xc8 and +0x104 - the two
 * seeded fuses. Phase 3 has none and ends on RL_ORB_DEPTH_END. */
#define RL_PH1_TICKS  0x320L
#define RL_PH2_TICKS  0x096L
#define RL_PH3_TICKS  200L
#define RL_PH2_AT     RL_PH1_TICKS
#define RL_PH3_AT     (RL_PH1_TICKS + RL_PH2_TICKS)
#define RL_TICKS      (RL_PH3_AT + RL_PH3_TICKS)

/* ORIGIN 0x002cbef8 Space_RunRelease, +0xa2, +0xf4 and +0x16e - one
 * a phase, each narrower than the last. orb_release.txt 1. */
#define RL_SKIP_PH1  0x002ccc24UL
#define RL_SKIP_PH2  0x002ccb80UL
#define RL_SKIP_PH3  0x002cccc8UL

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x3f2, +0x466 and +0x4fc - the
 * three mode handlers. orb_release.txt 5 is the rate table. */
#define RL_FLARE_SEED     0x7f00L
#define RL_FLARE_STEP     0x64L
#define RL_SPIN_STEP      0x14L
#define RL_SCROLL_STEP    0x200L
#define RL_CRACK_A_STEP   0x25L
#define RL_CRACK_B_STEP   0x53L
#define RL_DRIFT_SEED     0xc800L
#define RL_DRIFT_ACC      5L
#define RL_ORB_DEPTH0     0x258L
#define RL_ORB_DEPTH_END  0xc8L
#define RL_ORB_DEPTH_STEP 2L

/* ORIGIN 0x002d5704 Camera_OrbitStep - the camera CIRCLES the origin
 * 9000 out and 7000 up; the orb needs no position of its own because
 * it IS the origin. Space_RunRelease +0x72 and +0x7c seed the two
 * angles. orb_release.txt 6. */
#define RL_CAM_DIST   (-0x2328L)
#define RL_CAM_LIFT   0x1b58L
#define RL_ORBIT_A0   0x446L
#define RL_ORBIT_B0   0x872L
#define RL_ORBIT_A_ST 8L
#define RL_ORBIT_B_ST (-4L)

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x406 - the aim OFFSET handed to
 * Camera_TrackTarget, not an angle. effects/orb_underside.txt 3.
 *
 * WHICH ADDEND GOES ON WHICH AXIS MATTERS: 0x1f4 lands on the
 * target's X and -(g_OrbDrift >> 4) on its Z [verified, 0x002d57bc
 * Camera_TrackTarget +0xa and +0x1c]. Swapped, the camera's position
 * stays right and its YAW accumulates at the wrong rate.
 * orb_release.txt 6. */
#define RL_AIM_X      0x1f4L

/* ORIGIN 0x002cbef8 Space_RunRelease, +0x5e and +0x68 - g_CameraYaw
 * and g_CameraPitch, seeded before the first phase. Leaving them at
 * zero points the camera away from the origin and EVERY SHIP FACE IS
 * THEN REJECTED as unprojectable, which reads as missing geometry
 * rather than as a camera fault. */
#define RL_CAM_YAW0   0x37a0L
#define RL_CAM_PITCH0 (-0x286L)

/* ORIGIN 0x002cbef8 Space_RunRelease, +0x13c to +0x164 - PHASE 3
 * RESETS THE CAMERA. It is a new shot, not a continuation of the
 * orbit: the target goes to (0, 0, 0x251c) and the orientation to
 * yaw 0, pitch -0x800, looking up at the ship with the orb falling
 * toward the lens. */
#define RL_PH3_CAM_Z  0x251cL
#define RL_PH3_YAW    0L
#define RL_PH3_PITCH  (-0x800L)
#define RL_PH3_CAM_ZS 0x1eL
#define RL_PH3_YAW_ST (-0xaL)

/* ORIGIN 0x002d3f5c Warp_FullscreenAffine - phase 3's background is
 * the SHIP'S HULL filling the frame as a texture, which is why that
 * shot needs no starfield. Three corners are scaled by
 * g_WarpParamB >> 8 and rotated by g_WarpParamA, and the texture is
 * Fshiptextures' first 256x256 block. orb_release.txt 6. */
#define RL_WARP_X      0x5000L
#define RL_WARP_Y      0x3200L
#define RL_WARP_A_STEP 0xaL
#define RL_WARP_B_SEED 0xaaL
#define RL_WARP_B_STEP 1L

/* ORIGIN 0x002d643c Strut_AnimateClaw with D0 = 0 - the opening
 * stage only, called TWICE a tick. IT STARTS FROM ZERO, not from the
 * clamp's shut pose: Space_RunRelease +0xb4 resets the joints and
 * all three read zero live [observed, on hardware]. */
#define RL_CLAW_LIMIT  (-0x1eeL)
static const s32 RL_CLAW_RATE[3] = { -2L, -4L, -2L };

/* ORIGIN 0x002cdbe0 Flash_WashFromFuse and 0x002cdb46
 * Flash_SetLightFromOrbDepth - the opening fade and the closing ramp. */
#define RL_WASH_FROM     0x2eeL
#define RL_SETLIGHT_FROM 0xdcL

/* ORIGIN 0x002d1f52 Orb_ProjectAndFlare, and the two gates on the
 * orb's scene slot at Scene_SortAndDrawAll +0x2f6. */
#define RL_PROJ_NUM    0x3200L
/* ORIGIN 0x002d2c7a - Scene_TransformProject's chain exactly, plus a
 * radius of (size << 11) / depth, 0x96 for the orb. Ship_Project
 * already carries its >>4, so the position goes in UNSCALED. */
#define RL_ORB_SIZE    0x96L
#define RL_CX          0x50L
#define RL_CY          0x32L
#define RL_RADIUS_GATE 0x50L
#define RL_DEPTH_GATE  0x1f4L

static s32 g_tick;
static s32 g_flare;             /* g_FlareGrow      */
static s32 g_spin;              /* g_ApproachSpin   */
static s32 g_scroll;            /* g_FlatScroll     */
static s32 g_crack_a, g_crack_b;
static s32 g_ang_a, g_ang_b;    /* g_SceneAngleA/B  */
static s32 g_depth;             /* g_OrbDepth       */
static s32 g_drift, g_drift_v;
static s32 g_sx, g_sy, g_sr;    /* g_OrbScreenX/Y/Radius */
/* OURS - THE ORB AS DRAWN, which is the step's answer put through
 * the look. Kept apart from g_sx/g_sy/g_sr because those are the
 * step's and feed its own gating. */
static s32 g_dsx, g_dsy, g_dsr;
static s32 g_bend[3];
static s32 g_cam[5];
static s32 g_orbit_a, g_orbit_b;
static Relic_Cam g_camstate;
static int g_ready;

static s32 g_warp_a, g_warp_b;
static const u8 *g_hull;

/* ORIGIN 0x002cd9de Scene_RenderRelease, +0x64 - THE SECOND OBJECT
 * ANGLE IS NOT g_SceneAngleB. The render derives it from the LATCHED
 * g_CameraPitch by shifting five and inverting the LOW BYTE only, and
 * hands that to the object transform and to the cracks. Measured
 * live: a pitch of -305 gives -247. orb_release.txt 6. */
static s32 angle_b(void)
{
	s32 d;

	/* ORIGIN 0x002cdaf2 - phase 3's render latches g_SceneAngleB
	 * STRAIGHT into 0x002e0bee, where Scene_RenderRelease derived it
	 * from the pitch. Different render, different rule. */
	if (g_tick >= RL_PH3_AT) return g_ang_b;
	d = g_camstate.pitch >> 5;
	return (s32)(s16)((d & ~0xffL) | ((~d) & 0xffL));
}

/* ORIGIN 0x002cdbe0 Flash_WashFromFuse - keyed on the FUSE, so the
 * scene opens saturated and fades over its first 50 ticks. That is
 * the other half of the explosion's ending. */
static void wash_open(Relic_Frame *f)
{
	s32 amt = (RL_PH1_TICKS - g_tick) - RL_WASH_FROM;
	s32 i, n = f->w * f->h;

	if (g_tick >= RL_PH1_TICKS || amt < 0) return;
	amt <<= 1;
	if (amt > CHAN_MAX) amt = CHAN_MAX;
	for (i = 0; i < n; i++) {
		u8 *p = f->px + (size_t)i * PX_BYTES;
		s32 v = p[PX_LIGHT] + amt;
		p[PX_LIGHT] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
	}
}

/* ORIGIN 0x002cdb46 Flash_SetLightFromOrbDepth - it SETS rather than
 * adds, and only over the phase's last ten ticks. */
static void wash_close(Relic_Frame *f)
{
	s32 amt = g_depth - RL_SETLIGHT_FROM;
	s32 i, n = f->w * f->h;

	if (amt >= 0) return;
	amt = ((-amt) << 2) & 0xffL;
	if (amt > CHAN_MAX) amt = CHAN_MAX;
	for (i = 0; i < n; i++)
		f->px[(size_t)i * PX_BYTES + PX_LIGHT] = (u8)amt;
}


/* ORIGIN 0x002d5704 Camera_OrbitStep. The render latches the target
 * into the camera, so this IS the camera's POSITION;
 * Camera_TrackTarget then makes the yaw and pitch from the aim
 * offset. */
static void cam_orbit(void)
{
	s32 x = 0L, y = RL_CAM_DIST, z = 0L;

	Orb_Rotate2Axis(&x, &y, &z, g_orbit_b, g_orbit_a);
	g_cam[0] = x;
	g_cam[1] = y;
	g_cam[2] = z + RL_CAM_LIFT;
	g_orbit_a = (g_orbit_a + RL_ORBIT_A_ST) & 0x1fffL;
	g_orbit_b = (g_orbit_b + RL_ORBIT_B_ST) & 0x1fffL;
	Space_CamTrack(&g_camstate, g_cam, RL_AIM_X, -(g_drift >> 4));
	g_cam[3] = g_camstate.yaw;
	g_cam[4] = g_camstate.pitch;
}

/* OURS. Phases 1 and 2 against whichever camera is asked for, so the
 * frame can repeat the projection through the look of approach.txt 8
 * without the step's own answer moving. -> 0 if the orb is rejected.
 *
 * ORIGIN 0x002d2bac - the slot's world position is three longwords
 * at 0x002ee894 and the THIRD is g_OrbDrift, so it is NOT at the
 * origin. */
static int orb_project_at(const s32 *cam, s32 *sx, s32 *sy, s32 *sr)
{
	Relic_Vert o;
	s32 px, py, d;

	o.comp[0] = 0L; o.comp[1] = 0L; o.comp[2] = g_drift;
	if (!Ship_Project(&o, g_ang_a, cam, &px, &py, &d) || d <= 0) return 0;
	*sx = px >> 8;
	*sy = py >> 8;
	*sr = (RL_ORB_SIZE << 11) / d;
	return 1;
}

/* ORIGIN 0x002d1f52 Orb_ProjectAndFlare for phase 3, where the slot
 * is gone and the frame centre IS the answer. In phases 1 and 2 the
 * orb is a scene slot and its place is the camera's doing; pinning it
 * to the centre there reads as the SHIP being offset.
 * orb_release.txt 6. */
static void orb_project(void)
{
	if (g_tick >= RL_PH3_AT) {
		g_sx = RL_CX;
		g_sy = RL_CY;
		g_sr = (g_depth > 0) ? RL_PROJ_NUM / g_depth : 0;
		return;
	}
	if (!orb_project_at(g_cam, &g_sx, &g_sy, &g_sr)) g_sr = 0;
}

/* ORIGIN 0x002cd9de Scene_RenderRelease +0x7e and 0x002d1f52
 * Orb_ProjectAndFlare +0x1c - the same radius in both. */
static s32 flare_radius(void)
{
	s32 v = g_flare >> 8;
	s32 r = (((v * v) >> 1) << 2) + 0x1eL;

	if (r <= 0x23L) return 0;
	return (r > 0x7fL) ? 0x7fL : r;
}

/* ORIGIN 0x002d643c Strut_AnimateClaw, D0 = 0 - the opening stage
 * only, so the latch never arms and the claws never snap. */
static void claw_open(void)
{
	int j;

	if (g_bend[0] <= RL_CLAW_LIMIT) return;
	for (j = 0; j < 3; j++) g_bend[j] += RL_CLAW_RATE[j];
	Ship_SetClawBend(g_bend);
}

/* OURS */
static int rl_init(void)
{
	u32 len;
	int j;

	g_tick = 0;
	g_flare = RL_FLARE_SEED;
	g_spin = 0;
	g_scroll = 0;
	g_crack_a = 0;
	g_crack_b = 0;
	g_ang_a = 0;
	g_ang_b = 0;
	g_depth = RL_ORB_DEPTH0;
	g_drift = RL_DRIFT_SEED;
	g_drift_v = 0;
	for (j = 0; j < 5; j++) g_cam[j] = 0;
	g_orbit_a = RL_ORBIT_A0;
	g_orbit_b = RL_ORBIT_B0;
	g_camstate.yaw = RL_CAM_YAW0;
	g_camstate.pitch = RL_CAM_PITCH0;
	g_camstate.head = 0;
	g_camstate.angle = 0;
	g_camstate.angle2 = 0;
	for (j = 0; j < 3; j++) g_bend[j] = 0L;
	g_ready = 0;
	if (Space_Init()) return 1;
	Ship_Init();
	Ship_SetSkipTable(RL_SKIP_PH1);
	Ship_SetExtras(0, 0);
	/* ORIGIN 0x002cbef8 Space_RunRelease +0x22 - Space_BuildObjects
	 * puts the claws back on their original texture. */
	Ship_SetClawTexture(-1L);
	Ship_SetClawBend(g_bend);
	/* ORIGIN 0x002e494c g_ShipTexSlots[1] - Fshiptextures' FIRST
	 * 0x40000 block; the demo loads Fcrust over the second. */
	g_hull = Dat_Chunk("Fshiptextures", &len);
	if (!g_hull || len < 0x40000UL) return 1;
	g_warp_a = 0L;
	g_warp_b = RL_WARP_B_SEED;
	if (Orb_Init()) return 1;
	Gfx_SetLightBase(RL_LIGHT_R, RL_LIGHT_G, RL_LIGHT_B);
	orb_project();
	cam_orbit();
	g_ready = 1;
	return 0;
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, +0x3f2, +0x466 and +0x4fc - one
 * handler a mode. orb_release.txt 5 is the rate table. */
static void rl_step(s32 tick)
{
	g_tick = tick;
	g_spin += RL_SPIN_STEP;
	g_scroll -= RL_SCROLL_STEP;
	g_crack_a += RL_CRACK_A_STEP;
	g_crack_b += RL_CRACK_B_STEP;
	Ship_SetFlatScroll(g_scroll);
	if (tick < RL_PH3_AT) {
		cam_orbit();
		orb_project();
		g_flare -= RL_FLARE_STEP;
		if (g_flare < 0) g_flare = 0;
		g_ang_a += 2L;
		if (tick >= RL_PH2_AT) {
			claw_open();
			claw_open();
			g_drift += g_drift_v;
			g_drift_v += RL_DRIFT_ACC;
		}
	} else {
		if (tick == RL_PH3_AT) {
			g_cam[0] = 0L;
			g_cam[1] = 0L;
			g_cam[2] = RL_PH3_CAM_Z;
			g_camstate.yaw = RL_PH3_YAW;
			g_camstate.pitch = RL_PH3_PITCH;
		}
		g_ang_a += 1L;
		g_ang_b += 2L;
		g_depth -= RL_ORB_DEPTH_STEP;
		g_warp_a += RL_WARP_A_STEP;
		g_warp_b += RL_WARP_B_STEP;
		g_cam[2] += RL_PH3_CAM_ZS;
		g_camstate.yaw += RL_PH3_YAW_ST;
		g_cam[3] = g_camstate.yaw;
		g_cam[4] = g_camstate.pitch;
		orb_project();
	}
}

/* ORIGIN 0x002d28ca Scene_SortAndDrawAll, +0x2e2 - the orb slot's
 * body, reached from INSIDE
 * its loop. Being in the sort is the point: the
 * hull can paint over the orb and the orb over the hull, by depth.
 * Drawing it after Ship_Draw puts it on top of everything. */
static void orb_slot(Relic_Frame *f, s32 which)
{
	(void)which;
	Orb_DrawSphere(f, g_dsx, g_dsy, g_dsr,
	               (g_flare >> 8) >> 1, g_ang_a, angle_b());
	if (g_dsr < RL_RADIUS_GATE)
		Orb_DrawShells(f, g_dsx, g_dsy, g_dsr, g_crack_a, g_crack_b,
		               0L, g_ang_a, angle_b(), g_scroll);
}

/* OURS. PHASE 3 DOES NOT TAKE THE LOOK OF approach.txt 8: it has no
 * sky - the hull fills the frame - and the demo pins the orb to the
 * frame's centre there, so a drag would swing the vessel inside a
 * backdrop and around an orb that both stayed put. Phases 1 and 2
 * are a scene and take it whole, the orb with them. */
static void rl_frame(Relic_Frame *f, s32 tick)
{
	int phase3 = (tick >= RL_PH3_AT);
	s32 cam[SHIP_CAM], dyaw = 0, dpitch = 0;

	Frame_Clear(f);
	if (!g_ready) return;
	if (!phase3) Space_Look(&dyaw, &dpitch);
	Ship_LookCam(cam, g_cam, dyaw, dpitch);
	g_dsx = g_sx;
	g_dsy = g_sy;
	g_dsr = g_sr;
	if ((dyaw || dpitch) && !orb_project_at(cam, &g_dsx, &g_dsy, &g_dsr))
		g_dsr = 0;
	Ship_SetSkipTable(phase3 ? RL_SKIP_PH3
	                 : (tick >= RL_PH2_AT ? RL_SKIP_PH2 : RL_SKIP_PH1));

	/* ORIGIN 0x002cdaac against 0x002cd9de - PHASE 3 HAS NO SKY. Its
	 * render opens with the hull filling the frame instead, so the
	 * stars are simply not there. */
	if (phase3)
		Warp_Fill(f, g_hull, g_warp_a, g_warp_b, 1);
	else
		Space_DrawSky(f,
		              Space_SkyPan(g_camstate.angle
		                           + dyaw * SPACE_LOOK_SKY,
		                           g_camstate.angle2
		                           + dpitch * SPACE_LOOK_SKY),
		              g_camstate.head);
	/* ORIGIN 0x002d2bc0 - in phases 1 and 2 the orb goes THROUGH the
	 * sort, gated on its projected radius; in phase 3 the slot is
	 * gone and the render draws it directly, over everything. */
	if (!phase3 && g_dsr > 0 && g_dsr < RL_RADIUS_GATE) {
		Relic_Extra ex;

		ex.cent[0] = 0L; ex.cent[1] = 0L; ex.cent[2] = g_drift;
		ex.draw = orb_slot;
		ex.which = 0L;
		Ship_SetExtras(&ex, 1);
	} else {
		Ship_SetExtras(0, 0);
	}
	Ship_Draw(f, g_ang_a, cam);
	if (phase3) orb_slot(f, 0L);
	if (phase3) {
		wash_close(f);
	} else {
		s32 r = flare_radius();
		if (r) {
			Gfx_SetFlareRayPhase(g_spin);
			Gfx_Flare(f, g_dsx, g_dsy, r, g_RayArcs, g_ColRampGlow);
		}
		wash_open(f);
	}
}

/* OURS */
static void rl_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out, "tick %ld/%ld | phase %d | flare %ld r%ld"
	             " | depth %ld orb %ld,%ld r%ld | claw %ld | faces %ld"
	             " | cam %ld,%ld,%ld yaw %ld",
	        (long)g_tick, (long)RL_TICKS,
	        g_tick >= RL_PH3_AT ? 3 : (g_tick >= RL_PH2_AT ? 2 : 1),
	        (long)g_flare, (long)flare_radius(),
	        (long)g_depth, (long)g_sx, (long)g_sy, (long)g_sr,
	        (long)g_bend[0],
	        (long)Ship_FacesDrawn(),
	        (long)g_cam[0], (long)g_cam[1], (long)g_cam[2],
	        (long)g_camstate.yaw);
}

const Relic_Part PART_Release = {
	"2.13", "The orb release",
	"Space_RunRelease 0x002cbef8, anim modes 0xd, 0x10, 0x11",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	rl_init, rl_step, rl_frame, rl_stats,
	RL_TICKS
};
