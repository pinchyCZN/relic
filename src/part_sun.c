/* RELIC - part 2.3, the planet and the sun.
 *
 * ORIGIN 0x002ce81e Space_RunSunScene, frame 0x002ce938
 * Space_SunSceneFrame. The backdrop is space.c; what is only this
 * scene's is the shadow sweep, the black base colour and the sun's
 * own drift. See effects/planet_sun.txt.
 */

#include <stdio.h>
#include <stdlib.h>

#include "parts.h"
#include "gfx.h"
#include "image.h"
#include "space.h"

/* The music cue the scene ends on - armed at 0x002ce8f8, polled at
 * 0x002ce90c. parts.h Relic_Gate. */
#define SUN_CUE_ORDER 0x02L
#define SUN_CUE_ROW   0x18L
/* MEASURED - the setup plus the 430 fields of motion that bring our
 * sun to where the demo's stands when the music reaches this scene's
 * cue. NOT the demo's own 863 fields, which are load-inflated -
 * reconstruction.txt 4.5. */
#define SUN_TICKS (SUN_SETUP + 430L)
static Relic_Gate g_cue_gate;

/* ORIGIN 0x002ce81e - what Space_RunSunScene writes before looping.
 * BOTH ROTATION ANGLES ARE ZERO HERE, so maybe_Math_Rotate2D_b is
 * the identity and the projection is a bare perspective divide. */
#define SUN_WORLD_X  (-400L)        /* g_SunWorldX  0xfffffe70       */
#define SUN_WORLD_Y  (-100L)        /* g_SunWorldY  0xffffff9c       */
#define BODY_WORLD_X (-300L)        /* 0x002e0c54                    */
#define BODY_WORLD_Y   (50L)        /* 0x002e0c58                    */

/* ORIGIN 0x002d5c66 mode 3 - this scene's entire motion, and it runs
 * in the VERTB handler, not in Space_SunSceneFrame. */
#define SUN_DRIFT_RATE    2L          /* g_SunWorldX   += 2         */
#define CAM_ANGLE_RATE    0x0fa0L     /* g_CameraAngle += 0xfa0     */
#define CAM_ANGLE_START   0x400000L

/* MEASURED - THE SCENE RUNS 115 FIELDS BEFORE ITS SUN MOVES, with
 * the music already playing. It DRAWS NOTHING for them, which is
 * what the demo shows - its render target reads all zeroes right
 * through the span, where this scene's own tick-0 picture would
 * leave the glow and a band of sky standing. reconstruction.txt
 * 4.5. */
#define SUN_SETUP       115L

/* What sun_step leaves for sun_frame. g_range_prev is Space_Range() as it
 * stood BEFORE this tick's ease: the first flare is drawn before the
 * save-under pair runs, so it must see the old value while everything
 * after it sees the new one. */
static s32 g_range_prev;
static s32 g_second_cx, g_second_cy;
static int g_second_on;

/* ORIGIN 0x002cea48 Scene_DrawShadowSweep - the planet's shadow,
 * calculated, and it fills the screen at the scene's start.
 *
 * The arc is isqrt(10000 - y*y), a radius-100 curve on a 100-row
 * screen, offset by planet + 3*(planet - flare). The sun drifts
 * right, so the offset shrinks, so the shadow's edge retreats
 * leftward and uncovers the frame from the right. At tick 0 the
 * offset puts the arc past 0xa0 with the ramp already clamped, which
 * is why the screen starts entirely black. */
static void shadow_sweep(Relic_Frame *f, s32 body_x, s32 flare_x_fx)
{
	s32 y;

	for (y = -(SPACE_H / 2); y < SPACE_H / 2; y++) {
		/* The terminator moves at THREE TIMES the sun's rate, so the
		 * truncation project() does costs three pixels here where it
		 * costs one at the sun - the whole 27-cell gradient slid
		 * bodily every eighth tick. Taking the arc fractionally slides
		 * it by the remainder instead; a zero remainder leaves every
		 * span exactly where FX_MUL put it. */
		s32 arc_fx = ((Gfx_Isqrt(10000 - y * y) + 4 * body_x) << 16)
		           - 3 * flare_x_fx;
		s32 arc = arc_fx >> 16;
		s32 frac = arc_fx & 0xffffL;
		s32 row = y + SPACE_H / 2;
		s32 light = 0xa;
		s32 i;

		for (i = arc - 1; i >= 0; i--) {
			if (i < SPACE_W) {
				s32 py0 = FX_MUL(row, f->scale);
				s32 py1 = FX_MUL(row + 1, f->scale);
				s32 px0 = Space_ScaleFx((i << 16) + frac, f->scale) >> 16;
				s32 px1 = Space_ScaleFx(((i + 1) << 16) + frac, f->scale) >> 16;
				s32 py, px;
				/* Sliding the spans right leaves [0, frac * scale)
				 * written by nobody - a gap down the left of the
				 * screen that breathes with the fraction. */
				if (i == 0) px0 = 0;
				for (py = py0; py < py1 && py < f->h; py++) {
					if (py < 0) continue;
					for (px = px0; px < px1 && px < f->w; px++) {
						if (px < 0) continue;
						f->px[((size_t)(py * f->w) + (size_t)px) * PX_BYTES
						      + PX_LIGHT] = (u8)light;
					}
				}
			}
			/* The step happens whether or not the pixel was on
			 * screen, so an arc off the right edge arrives already
			 * clamped. */
			light += 2;
			if (light > 0x40) light = 0x40;
		}
	}
}

/* OURS. What the LAST frame actually drew, for sun_stats. SNAPSHOT,
 * NEVER RECOMPUTE: deriving it from the scene constants reported
 * tick-0 geometry forever, and an echo of the shadow's arc drifted
 * from shadow_sweep the moment that changed. */
static s32 g_st_tick, g_st_fy, g_st_bx, g_st_by, g_st_fx_fx;

/* OURS - the tick the MOTION is at, which the setup span holds at 0. */
static s32 sun_motion(s32 tick)
{
	return (tick > SUN_SETUP) ? tick - SUN_SETUP : 0L;
}

/* OURS. Everything in the scene that depends on the PREVIOUS frame,
 * pulled out of the drawing so it can run at 50 Hz while the picture is
 * drawn once per presented frame - see demo.c. The order inside is the
 * demo's: the save-under pair sits between the two flares, so the ease
 * happens after g_range_prev is taken and before the second flare uses
 * the result. */
static void sun_step(s32 tick)
{
	s32 fx, fy, bx, by;

	Parts_GateStep(&g_cue_gate, tick, SUN_CUE_ORDER, SUN_CUE_ROW,
	               SUN_TICKS);
	Space_Project(SUN_WORLD_X + sun_motion(tick) * SUN_DRIFT_RATE,
	              SUN_WORLD_Y, &fx, &fy);
	Space_Project(BODY_WORLD_X, BODY_WORLD_Y, &bx, &by);
	(void)fx; (void)fy; (void)bx; (void)by;

}

/* OURS */
static void sun_frame(Relic_Frame *f, s32 tick)
{
	s32 fx, fy, bx, by, radius, sub, ang, fx_fx, fy_fx;

	if (tick <= SUN_SETUP) {
		g_st_tick = tick;
		Frame_Clear(f);
		return;
	}

	/* DIVERGENCE: under RELIC_MODE_SUBPIXEL the scene is drawn at the
	 * fraction of a tick the host is actually at, so the sun follows the
	 * clock rather than stepping on tick boundaries. It has to: the sun
	 * drifts 2 world units a tick, which the projection's divide by
	 * FOCAL turns into 0.128 of a pixel, so the demo's own centre only
	 * changes every eighth tick - at scale 12 that is a 12-pixel jump
	 * six times a second. The mode is forced off at scale 1.0, which
	 * keeps it the exact reference. See reconstruction.txt section 5. */
	sub = (f->modes & RELIC_MODE_SUBPIXEL) ? f->subtick : 0;
	if (tick <= SUN_SETUP) sub = 0;
	ang = CAM_ANGLE_START + sun_motion(tick) * CAM_ANGLE_RATE
	    + ((sub * CAM_ANGLE_RATE) >> 16);

	/* ORIGIN 0x002cd102 - this scene sets the camera to zero, which
	 * is why its sun drifts alone while the approach scene's whole
	 * backdrop swings together. */
	Space_SetCamera(0, 0);
	Gfx_SetFlareRayPhase(ang >> 12);
	/* ORIGIN 0x002ce81e - Ham8_BuildTables(0,0,0). THE BLACK BASE IS
	 * WHAT MAKES THE LIGHT BYTE DARKEN, and the planet depends on it.
	 * Scene one of maybe_Space_RunPlanetScenes passes white instead and
	 * the same byte brightens - effects/approach.txt section 2. */
	Gfx_SetLightBase(0, 0, 0);
	Frame_Clear(f);

	/* ORIGIN 0x002d2db2 - the pan is the camera angle's bits 15..22,
	 * four bytes a step, which is one texel: the sky slides about one
	 * texel every eight frames. Nothing else in the scene moves the
	 * background, and that slide is what reads as a camera pan.
	 *
	 * ORIGIN 0x002d2e64 - ADDED, NOT SUBTRACTED; negated it samples 272
	 * texels away. The real pan is 2D but its vertical angle is zero all
	 * scene. See effects/planet_sun.txt section 9. */
	Space_DrawSky(f, (((ang * 2) >> 16) & 0xffL) * 4L, 0L);

	Space_Project(SUN_WORLD_X + sun_motion(tick) * SUN_DRIFT_RATE,
	              SUN_WORLD_Y,
	              &fx, &fy);
	Space_Project(BODY_WORLD_X, BODY_WORLD_Y, &bx, &by);
	/* THE INTEGER CENTRE WHEN THE MODE IS OFF, and not merely
	 * Space_ProjectFx at sub 0: that carries the fraction the
	 * projection truncates, so using it unconditionally moves the sun
	 * in the reference render too. */
	if (f->modes & RELIC_MODE_SUBPIXEL) {
		s32 wx_fx = ((SUN_WORLD_X
		              + sun_motion(tick) * SUN_DRIFT_RATE) << 16)
		          + sub * SUN_DRIFT_RATE;
		Space_ProjectFx(wx_fx, SUN_WORLD_Y << 16, &fx_fx, &fy_fx);
	} else {
		fx_fx = fx << 16;
		fy_fx = fy << 16;
	}

	radius = 0x50 - (g_range_prev >> 8);
	if (radius > 0)
		Gfx_FlareFx(f, fx_fx, fy_fx, radius, g_RayArcs, g_ColRampWarm);

	/* ORIGIN steps 4 to 7 - THE SAVE-UNDER PAIR BRACKETS THE
	 * OCCLUDERS, so what the count sees is whatever has been drawn
	 * between the two. It has to live here rather than in the step:
	 * the demo measures it through the framebuffer, so there is
	 * nothing to measure until the frame has been drawn.
	 *
	 * DIVERGENCE: that ties the ease to the drawn frame rather than
	 * to the tick. The demo's own frame is a fixed 25 Hz so its ease
	 * is deterministic; ours follows whatever rate the host manages.
	 * See effects/planet_sun.txt section 7. */
	{
		s32 cx = fx, cy = fy;
		g_range_prev = Space_Range();
		Space_StampSaveUnder(f, fx, fy);
		Space_DrawPlanetAndHalo(f, bx, by);
		g_second_on = Space_EaseRange(Space_CountSaveUnder(f, &cx, &cy));
		if (g_second_on) {
			g_second_cx = cx;
			g_second_cy = cy;
		}
		Space_StepBand(g_second_on ? cy : -1);
	}

	/* ORIGIN 0x002d1e08 - the save-under pair belongs HERE, between the
	 * planet and the ghosts; sun_step has already run it. THE SECOND
	 * FLARE IS CENTRED ON THE CENTROID OF WHAT IS STILL VISIBLE, not on
	 * the sun [0x002d1e7e], so the glow leads the sun out from behind
	 * the planet. */
	if (g_second_on) {
		s32 r2 = Space_Range() >> 8;
		if (r2 > 0)
			Gfx_FlareFx(f, (g_second_cx << 16) + (fx_fx - (fx << 16)),
			            (g_second_cy << 16) + (fy_fx - (fy << 16)),
			            r2, g_RaySpikes, g_ColRampWarmB);
	}

	Space_DrawFlareGhosts(f, fx, fy);
	shadow_sweep(f, bx, fx_fx);
	Space_DrawHorizonBand(f, fx);

	g_st_tick = tick; g_st_fy = fy; g_st_fx_fx = fx_fx;
	g_st_bx = bx; g_st_by = by;
}

/* OURS */
static void sun_stats(char *out, int cap)
{
	s32 vis = 0, st = 0;

	(void)cap;
	Space_CountStats(&vis, &st);
	sprintf(out, "t%ld sun (%ld.%03ld,%ld) planet (%ld,%ld) "
	             "cells %ld/%ld range %04lx",
	        (long)g_st_tick, (long)(g_st_fx_fx >> 16),
	        (long)(((g_st_fx_fx & 0xffffL) * 1000) >> 16), (long)g_st_fy,
	        (long)g_st_bx, (long)g_st_by,
	        (long)vis, (long)st, (long)Space_Range());
}

/* OURS - parts.h. */
static s32 sun_length(void)
{
	return Parts_GateLength(&g_cue_gate, SUN_TICKS);
}

const Relic_Part PART_Sun = {
	"2.3", "Planet and sun", "Space_RunSunScene 0x002ce81e",
	PART_LIT, SPACE_W, SPACE_H, Space_Init, sun_step, sun_frame,
	sun_stats,
	SUN_TICKS, 0, sun_length
};
