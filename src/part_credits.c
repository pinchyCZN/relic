/* RELIC - OURS: the demo's own things, on a turntable, past the end.
 *
 * THERE IS NO SUCH PART IN THE DEMO - docs/reconstruction.txt 7. What
 * is shown, its pose and every rate are the demo's; the framing is
 * ours. TWO THINGS TO LOOK AT so far, the vessel and the orb, and
 * host.h's cycle picks between them. Each is reset on arrival, so a
 * turn and a zoom belong to the thing they were made on.
 *
 * A DRAG ORBITS AND STAYS WHERE IT IS LEFT - this part's reading of
 * host.h's gesture, where a space scene springs the same drag into a
 * look. The wheel dollies.
 */

#include <stdio.h>
#include <stdlib.h>

#include "parts.h"
#include "gfx.h"
#include "host.h"
#include "dat.h"
#include "image.h"
#include "objset.h"
#include "orb.h"
#include "ship.h"
#include "stills.h"
#include "space.h"

#define CR_SHIP 0L
#define CR_ORB  1L
#define CR_ARK  2L
#define CR_CITY 3L
#define CR_CITY2 4L
#define CR_TEMPLE 5L

/* ORIGIN 0x002d5d62 - mode 1's step for g_ObjectAngle, so the
 * turntable turns at the rate the demo turns the ship: a revolution
 * in 410 ticks. */
#define CR_SPIN 0x14L

/* OURS - THE ONE FRAMING NUMBER, and both things rest on it.
 * Scene_TransformProject multiplies by 0x100 and divides by the
 * depth, so this is the vessel's radius in demo pixels at rest - 40
 * of the frame's 50 half-rows - and the orb's radius outright. */
#define CR_FILL 0x28L

/* OURS - A DRAG ACROSS THE WHOLE WINDOW IS ONE TURN. Full deflection
 * is half a window, so it is worth half of the 0x2000 revolution. */
#define CR_ORBIT 0x1000L

/* OURS - a quarter of the revolution, which is straight down over
 * the model. Past it the picture mirrors and the yaw stops meaning
 * anything. */
#define CR_PITCH_MAX 0x800L

/* OURS - ONE NOTCH, AS A RATIO rather than a step, so the travel is
 * even in apparent size instead of crowding at the near end. */
#define CR_STEP_NUM 4L
#define CR_STEP_DEN 5L

/* ORIGIN 0x002d5c66 Anim_SceneTick - the release's rate table, which
 * is what makes the cracks crawl and the shell quads scroll.
 * effects/orb_release.txt 5. */
#define CR_CRACK_A    0x25L
#define CR_CRACK_B    0x53L
#define CR_SCROLL     0x200L

/* ORIGIN 0x002cdaf2 - phase 3's own two steps, which is the one
 * place the demo spins the orb on angles of its own rather than
 * deriving the second from the camera's pitch. */
#define CR_ORB_SPIN_A 1L
#define CR_ORB_SPIN_B 2L

/* ORIGIN 0x002cdc3c, 0x002d1fdc - a revolution of the orb is 0x100,
 * not the transform's 0x2000. docs/reconstruction.txt 7.1. */
#define CR_ORB_SHIFT 5

/* ORIGIN 0x002d5c66 - THE COOL-DOWN. g_FlareGrow is seeded here and
 * walks down by the step; Orb_DrawSphere's reveal is (it >> 8) >> 1,
 * a floor on each texel's blue, so the sphere is sparse while the
 * number is high and solid once it reaches zero. Held here rather
 * than counted down once, so it can go back up. */
#define CR_COOL_SEED  0x7f00L
#define CR_COOL_STEP  0x64L

/* ORIGIN 0x002d2bc0 - THE RELEASE DROPS Crack_DrawShells ABOVE THIS
 * projected radius, and the rays go with it. Staying under costs
 * nothing: 0x50 is a radius, so the orb may be the frame's full
 * width before they would cut out. */
#define CR_ORB_MAX 0x4fL

/* ORIGIN 0x002cbef8 - the ORB SEQUENCE'S light base, not the
 * explosion's, so it reads blue rather than blue-white. */
#define CR_ORB_LIGHT_R 0x3fL
#define CR_ORB_LIGHT_G 0x3fL
#define CR_ORB_LIGHT_B 0x4fL

/* ORIGIN 0x003682f8 Temple_StepMode1 - the DEPARTURE's base, which is
 * the shot this body is being shown from. */
#define CR_ARK_LIGHT_R 0x3fL
#define CR_ARK_LIGHT_G 0x4fL
#define CR_ARK_LIGHT_B 0x6fL

/* ORIGIN 0x0036843a - the object set's own projection: the depth has
 * TE_OBJ_NEAR added before the divide and clips at TE_OBJ_CLIP, so
 * neither of the vessel's two stops carries over. Its screen centre
 * IS the frame's, so SPACE_W/2 and SPACE_H/2 stand in for the set's
 * own pair rather than a second one. objset.h. */
#define CR_ARK_NEAR 0x1f4L
#define CR_ARK_CLIP 0xc8L

/* OURS - how far out the extent is measured from, and over how many
 * yaws. Far enough that nothing clips, so the first vertex that
 * fails to project is the end of an object's list and not a
 * casualty of the near plane. */
#define CR_ARK_PROBE   0x4000L
#define CR_ARK_YAWS    8L
#define CR_ARK_MAXVERT 4096L

/* ORIGIN 0x00316714 - Farmdata, split four ways. The map and the
 * 1BPP mask are used where they lie; only the two pictures decode.
 * docs/effects/city.txt 3. */
#define CR_CT_P1       0x00000L
#define CR_CT_P2       0x063a0L
#define CR_CT_P3       0x0c540L
#define CR_CT_P4       0x0d134L
#define CR_CT_FARM_LEN 0x130d4L
#define CR_CT_TEXELS   0x5fa0L           /* 24480 = 160 x 153       */
#define CR_CT_ROWS     153L
#define CR_CT_PAL_MAIN 0x100L
#define CR_CT_PAL_LAYR 0x80L

/* ORIGIN 0x0031460c, 0x00316476, 0x003147b0 - shot one's threshold,
 * the rate a field takes off it, and the palette it burns in. */
#define CR_CT_THRESH   0x320L
#define CR_CT_RATE     3L
#define CR_CT_LOOP     0xe6L
#define CR_CT_END      (CR_CT_THRESH - CR_CT_LOOP * CR_CT_RATE)
#define CR_CT_PAL      0x0033aa34UL
#define CR_CT_CUT      0xffL
#define CR_CT_HAZE_OFF 0x15eL
#define CR_CT_HAZE_DRF 1L
#define CR_CT_SPIN     0xaL

/* ORIGIN 0x0031463e - the city's light base. */
#define CR_CT_LIGHT_R  0x7fL
#define CR_CT_LIGHT_G  0x5fL
#define CR_CT_LIGHT_B  0x4fL

/* OURS - 153 rows through a 100-row frame, so this is the whole of
 * the travel there is and the drag spans exactly it. The picture is
 * the frame's own width, which is why none of it goes sideways. */
#define CR_CT_TRAVEL   (CR_CT_ROWS - SPACE_H)

/* ORIGIN 0x003146a8, 0x003146cc - shot two's stills are BAKED and
 * FRAME SIZED rather than 153 tall, which is why it never scrolls:
 * there is nothing above or below the frame to move into. */
#define CR_C2_DEEP     0x00326194UL
#define CR_C2_PAL      0x00331d60UL
#define CR_C2_MAP      0x00335fe0UL
#define CR_C2_MASK     0x00339e60UL
#define CR_C2_TEXELS   0x3e80L           /* 16000 = 160 x 100       */

/* ORIGIN 0x003146e0, 0x00316476, 0x00314724 - its threshold, its
 * rate and the loop it runs, which together give the far end of the
 * burn and so the span a scrub has to cover. */
#define CR_C2_THRESH   0x2a0L
#define CR_C2_RATE     4L
#define CR_C2_LOOP     0x96L
#define CR_C2_END      (CR_C2_THRESH - CR_C2_LOOP * CR_C2_RATE)

/* OURS - THE WALKING CAMERA'S NUMBERS, none of them the demo's: it
 * has no free camera to read one from. A full drag is worth this
 * much turn and this much horizon; Z walks this far a tick along
 * the heading ALONE, so the ground stays a plane; the wheel lifts.
 * docs/reconstruction.txt 7.5. */
#define CR_TE_YAW    900                /* 90 degrees of 3600      */
#define CR_TE_PITCH  160                /* horizon rows a drag     */
#define CR_TE_SPEED  0x10
#define CR_TE_RISE   0x140               /* height a notch          */

/* OURS - ASYMMETRIC, because the two ends are not worth the same:
 * down is terrain and up is the fog colour and nothing else. Rows
 * either side of the pose the camera opens at. */
#define CR_TE_LOOK_UP   30
#define CR_TE_LOOK_DOWN 520

static s32 g_te_cam[5];
static s32 g_te_home[5];
static s32 g_te_ph, g_te_pv;
static int g_te_down;
static int g_temple_ok;
static u8 *g_c2_pic;
static u8 *g_c2_layer;
static const u8 *g_c2_map;
static const u8 *g_c2_mask;
static int g_city2_ok;
static s32 g_c2_press;                  /* the burn, latched on a press */
static int g_c2_down;
static u8 *g_ct_pic;
static u8 *g_ct_layer;
static const u8 *g_ct_map;
static const u8 *g_ct_mask;
static int g_city_ok;
static int g_z_prev;
static Relic_ObjSet g_ark_set;          /* the default, glows off    */
static int g_ark_ok;
static s32 g_ark_r;                     /* its measured half-extent  */
static s32 g_ark_depth;                 /* its own dolly             */
static s32 g_avail[6];                  /* what loaded, in order     */
static s32 g_navail;
static s32 g_slot;                      /* where in that list we are */
static s32 g_char;                      /* which thing is shown      */
static s32 g_cycle;                     /* the host's count, latched */
static int g_orb_ok;
static s32 g_ang;
static s32 g_centre[3];
static s32 g_radius;
static s32 g_depth;                     /* the vessel's dolly        */
static s32 g_orb_r;                     /* the orb's, a radius       */
static s32 g_cool;                      /* the reveal accumulator    */
static s32 g_ca, g_cb, g_scroll;        /* the crack and shell crawl */
static s32 g_oa, g_ob;                  /* the orb's own two angles  */
static s32 g_wheel;                     /* the notch count last seen */
static s32 g_yaw, g_pitch;              /* the orbit, and it sticks  */
static s32 g_press_yaw, g_press_pitch;
static int g_down;
static int g_held;                      /* has a hand taken the spin */

/* OURS. The rest distance, from the bounds. */
static s32 cr_rest(void)
{
	if (g_radius < 1L) return SHIP_NEAR_CLIP * 2L;
	return (g_radius << 8) / CR_FILL;
}

/* OURS. BOTH STOPS ARE DISTANCES AND BOTH ARE MEASURED: the near one
 * is where the front of the model would reach the transform's near
 * plane, the far one where its back would overflow the rotate -
 * ship.h. Neither is a notch count, because what a count is worth
 * depends on the size of whatever has been loaded. */
static s32 cr_clamp(s32 d)
{
	s32 lo = g_radius + SHIP_NEAR_CLIP;
	s32 hi = SHIP_DEPTH_MAX - g_radius;

	if (hi < lo) hi = lo;
	if (d < lo) d = lo;
	if (d > hi) d = hi;
	return d;
}

/* ORIGIN 0x00314588 - shot one's two Farmdata parts decoded and the
 * other two taken where they lie. docs/effects/city.txt 3. */
static int cr_city_load(void)
{
	const u8 *farm;
	u32 len;

	farm = Dat_Chunk("Farmdata", &len);
	if (!farm || len < (u32)CR_CT_FARM_LEN) return 1;
	g_ct_pic = Stills_LoadPalettised(farm + CR_CT_P1, CR_CT_PAL_MAIN,
	                                 CR_CT_TEXELS);
	g_ct_layer = Stills_LoadPalettised(farm + CR_CT_P2, CR_CT_PAL_LAYR,
	                                   CR_CT_TEXELS);
	if (!g_ct_pic || !g_ct_layer) return 1;
	g_ct_map = farm + CR_CT_P4;
	g_ct_mask = farm + CR_CT_P3;
	if (Stills_BlurInit(ST_TEXELS * 16L)) return 1;
	Stills_SeedRand(0L);
	Stills_BuildBlobRamp();
	Stills_BuildTrailRamp();
	Stills_BuildTurbulence();
	Stills_BuildBlastRamp();
	Stills_BuildRays();
	if (Stills_BuildPlasma()) return 1;
	return Stills_ParticlesInit();
}

/* ORIGIN 0x003146a8 - A FRAME-SIZED STILL IN A 153-ROW BUFFER. The
 * module indexes by the taller stride, so the head is shot two's own
 * image and the tail is the aerial one's - never seen, because this
 * shot does not scroll, and the reason scrolling it would be wrong
 * rather than merely empty. */
static u8 *cr_c2_pad(u8 *frame_sized, const u8 *tall)
{
	size_t head = (size_t)CR_C2_TEXELS * PX_BYTES;
	u8 *out;

	if (!frame_sized || !tall) { free(frame_sized); return 0; }
	out = (u8 *)malloc((size_t)CR_CT_TEXELS * PX_BYTES);
	if (out) {
		memcpy(out, frame_sized, head);
		memcpy(out + head, tall + head,
		       (size_t)CR_CT_TEXELS * PX_BYTES - head);
	}
	free(frame_sized);
	return out;
}

/* ORIGIN 0x003146a8 - shot two's four assets, all out of the image.
 * It rides on the aerial shot's buffers, so it loads after them. */
static int cr_city2_load(void)
{
	g_c2_pic = cr_c2_pad(Stills_LoadDeep(CR_C2_DEEP, CR_C2_TEXELS),
	                     g_ct_pic);
	g_c2_layer = cr_c2_pad(
	        Stills_LoadPalettised(Image_At(CR_C2_PAL), CR_CT_PAL_MAIN,
	                              CR_C2_TEXELS), g_ct_layer);
	if (!g_c2_pic || !g_c2_layer) return 1;
	g_c2_map = Image_At(CR_C2_MAP);
	g_c2_mask = Image_At(CR_C2_MASK);
	return (g_c2_map && g_c2_mask) ? 0 : 1;
}

/* ORIGIN 0x003146e0 - shot two's entry. It does NOT reseed the haze,
 * which only shot one does. */
static void cr_city2_enter(void)
{
	Stills_SetThreshold(CR_C2_THRESH);
	Stills_SetSources(g_c2_pic, g_c2_layer, g_c2_map);
	Stills_BuildSplatTable(CR_CT_PAL);
	Stills_BuildVelocities(1L);
	Stills_ResetParticles();
	Stills_SetCut(CR_CT_CUT);
	Stills_SetScroll(0L);
	g_c2_down = 0;
}

/* OURS. THE DRAG SCRUBS THE BURN, because this picture has no travel
 * to offer it. The value is the demo's own threshold and the span is
 * the one its loop covers, so this samples the shot rather than
 * inventing a control; the step pauses while a hand is on it, the
 * way every turntable here does. */
static void cr_city2_scrub(void)
{
	int down;
	s32 x = 0, y = 0, t;

	Demo_Drag(&down, &x, &y);
	if (down && !g_c2_down) g_c2_press = Stills_Threshold();
	g_c2_down = down;
	if (!down) return;
	t = g_c2_press - ((y * (CR_C2_THRESH - CR_C2_END)) >> 16);
	if (t < CR_C2_END) t = CR_C2_END;
	if (t > CR_C2_THRESH) t = CR_C2_THRESH;
	Stills_SetThreshold(t);
}

/* ORIGIN 0x0031460c - entering the shot, which is the whole of the
 * restart: the front goes back to its threshold, the pool empties
 * and the haze is reseeded. The demo enters it once; here a key
 * does, and switching to it does. */
static void cr_city_enter(void)
{
	Stills_SetThreshold(CR_CT_THRESH);
	Stills_SetSources(g_ct_pic, g_ct_layer, g_ct_map);
	Stills_BuildSplatTable(CR_CT_PAL);
	Stills_BuildVelocities(0L);
	Stills_ResetParticles();
	Stills_SetCut(CR_CT_CUT);
	Stills_SetHazeOffset(CR_CT_HAZE_OFF);
	Stills_SetHazeDrift(CR_CT_HAZE_DRF);
	Stills_SetScroll(0L);
}

/* OURS. THE DRAG IS THE SCROLL, and the demo's own row offset is
 * where it lands - no second idea of position. It rests halfway up
 * the travel and stays where it is let go. */
static void cr_city_scroll(void)
{
	s32 half = CR_CT_TRAVEL / 2L;
	s32 r = half - ((g_pitch * half) / CR_PITCH_MAX);

	if (r < 0L) r = 0L;
	if (r > CR_CT_TRAVEL) r = CR_CT_TRAVEL;
	Stills_SetScroll(r);
}

/* OURS - z forward, x back, and holding both is a stand. */
static s32 cr_te_step(void)
{
	s32 step = 0;

	if (Demo_Hold() & DEMO_HOLD_A) step += CR_TE_SPEED;
	if (Demo_Hold() & DEMO_HOLD_B) step -= CR_TE_SPEED;
	return step;
}

/* OURS - A DEVELOPER'S CAMERA: it turns while the pointer is down
 * and walks while the key is, and stops dead on either. No physics,
 * because there is nothing here to be faithful to - the demo flies
 * this ground on a spline and never freely.
 *
 * LOOKING NEVER STEERS THE WALK. The heading is a bearing and the
 * horizon only a screen row, so forward is defined at any pitch,
 * straight down included - the degeneracy a look-vector camera has
 * here does not exist. Height is the wheel's alone, and the two
 * keys are ONE SIGNED STEP, so holding both stands still. */
static void cr_temple_fly(void)
{
	int down;
	s32 x = 0, y = 0;

	Demo_Drag(&down, &x, &y);
	if (down) {
		if (!g_te_down) { g_te_ph = g_te_cam[3]; g_te_pv = g_te_cam[4]; }
		g_te_cam[3] = g_te_ph - ((x * CR_TE_YAW) >> 16);
		g_te_cam[4] = g_te_pv + ((y * CR_TE_PITCH) >> 16);
		while (g_te_cam[3] < 0L) g_te_cam[3] += 3600L;
		while (g_te_cam[3] >= 3600L) g_te_cam[3] -= 3600L;
	}
	g_te_down = down;
	if (g_te_cam[4] > g_te_home[4] + CR_TE_LOOK_UP)
		g_te_cam[4] = g_te_home[4] + CR_TE_LOOK_UP;
	if (g_te_cam[4] < g_te_home[4] - CR_TE_LOOK_DOWN)
		g_te_cam[4] = g_te_home[4] - CR_TE_LOOK_DOWN;
	{
		s32 dx = 0, dz = 0, step = cr_te_step();

		if (step) {
			Temple_HeadingStep(g_te_cam[3], step, &dx, &dz);
			g_te_cam[0] += dx;
			g_te_cam[2] += dz;
		}
	}
}

/* OURS. THE EXTENT, MEASURED THROUGH THE ONE PUBLIC PROJECTION.
 * There is no Ship_Bounds for an object set, but the screen offset
 * is (x << 8) / (depth + TE_OBJ_NEAR), so projecting from a known
 * distance and inverting that gives the half-extent in object units
 * - a measurement, not a number somebody liked. Several yaws,
 * because the view orbits and the widest one is the one that has to
 * fit. docs/reconstruction.txt 7.2. */
static s32 cr_ark_measure(void)
{
	const Relic_ObjSet *set = &g_ark_set;
	s32 cam[SHIP_CAM], o, v, k, e = 0;

	cam[0] = 0; cam[1] = -CR_ARK_PROBE; cam[2] = 0;
	cam[3] = 0; cam[4] = 0;
	for (k = 0; k < CR_ARK_YAWS; k++) {
		s32 ang = (k * 0x2000L) / CR_ARK_YAWS;

		for (o = 0; o < set->nobj; o++)
			for (v = 0; v < CR_ARK_MAXVERT; v++) {
				s32 sx, sy, dx, dy;

				if (!Temple_ProjectObjectVertex(ang, cam, o, v,
				                                &sx, &sy))
				    break;
				dx = sx - SPACE_W / 2L;
				dy = sy - SPACE_H / 2L;
				if (dx < 0) dx = -dx;
				if (dy < 0) dy = -dy;
				if (dx > e) e = dx;
				if (dy > e) e = dy;
			}
	}
	return (e * (CR_ARK_PROBE + CR_ARK_NEAR)) >> 8;
}

/* OURS. Both stops are the set's own: in, where the near face would
 * reach TE_OBJ_CLIP; out, the same 0x4000-table overflow the vessel
 * has, since obj_rot shifts by 14 against the same amplitude. */
static s32 cr_ark_clamp(s32 d)
{
	s32 lo = g_ark_r + CR_ARK_CLIP - CR_ARK_NEAR;
	s32 hi = SHIP_DEPTH_MAX - g_ark_r;

	if (lo < CR_ARK_CLIP) lo = CR_ARK_CLIP;
	if (hi < lo) hi = lo;
	if (d < lo) d = lo;
	if (d > hi) d = hi;
	return d;
}

/* OURS */
static s32 cr_orb_clamp(s32 r)
{
	if (r < 2L) r = 2L;
	if (r > CR_ORB_MAX) r = CR_ORB_MAX;
	return r;
}

/* OURS - A RATIO STEP STALLS ON A SMALL INTEGER, and this is the one
 * value here small enough to reach that: 2 * 5 / 4 truncates back to
 * 2, so the radius has fixed points at 2 and 3 and cannot be wound
 * back in from either. One unit is the floor on a notch. The dollies
 * are distances in the thousands and never come near it. */
static s32 cr_orb_zoom(s32 r, int in)
{
	s32 n = in ? (r * CR_STEP_DEN) / CR_STEP_NUM
	           : (r * CR_STEP_NUM) / CR_STEP_DEN;

	if (n == r) n = in ? r + 1L : r - 1L;
	return cr_orb_clamp(n);
}

/* OURS. THE SIZE IS THE STATE, NOT THE NOTCH COUNT. Clamping a count
 * against the stops instead lets it keep winding past one and then
 * hands back that much dead travel on the way out. A notch in means
 * NEARER for the vessel, which is a smaller distance, and BIGGER for
 * the orb, which is a larger radius.
 *
 * THE TEMPLE'S IS LINEAR WHERE THE REST ARE RATIOS: a ratio keeps a
 * dolly even in apparent size, and a height has no such property.
 * EVERY CHARACTER IS NAMED - the pictures take no notch at all, and
 * a fall-through would quietly wind the vessel's dolly for them. */
static void cr_wheel(void)
{
	s32 n = Demo_Wheel();
	s32 d = n - g_wheel;

	g_wheel = n;
	while (d > 0L) {
		if (g_char == CR_ORB)
			g_orb_r = cr_orb_zoom(g_orb_r, 1);
		else if (g_char == CR_ARK)
			g_ark_depth = cr_ark_clamp((g_ark_depth * CR_STEP_NUM)
			                           / CR_STEP_DEN);
		else if (g_char == CR_TEMPLE)
			g_te_cam[1] -= CR_TE_RISE;
		else if (g_char == CR_SHIP)
			g_depth = cr_clamp((g_depth * CR_STEP_NUM) / CR_STEP_DEN);
		d--;
	}
	while (d < 0L) {
		if (g_char == CR_ORB)
			g_orb_r = cr_orb_zoom(g_orb_r, 0);
		else if (g_char == CR_ARK)
			g_ark_depth = cr_ark_clamp((g_ark_depth * CR_STEP_DEN)
			                           / CR_STEP_NUM);
		else if (g_char == CR_TEMPLE)
			g_te_cam[1] += CR_TE_RISE;
		else if (g_char == CR_SHIP)
			g_depth = cr_clamp((g_depth * CR_STEP_DEN) / CR_STEP_NUM);
		d++;
	}
}

/* OURS - THE STICKY READING OF host.h's GESTURE, against Space_Look's
 * springy one. The deflection is measured from the press, so the turn
 * is latched on the press edge and added: integrating a delta a frame
 * instead would make the speed depend on the host's frame rate.
 *
 * A DRAG TURNS THE NEAR FACE WITH THE POINTER, and that is a NEGATIVE
 * yaw - the transform's yaw sends a vertex in front of the pivot the
 * other way. docs/reconstruction.txt 7. */
static void cr_orbit(void)
{
	int down;
	s32 x = 0, y = 0;

	Demo_Drag(&down, &x, &y);
	if (down) {
		if (!g_down) {
			g_press_yaw = g_yaw;
			g_press_pitch = g_pitch;
		}
		g_held = 1;
		g_yaw = (g_press_yaw - ((x * CR_ORBIT) >> 16)) & 0x1fffL;
		g_pitch = g_press_pitch + ((y * CR_ORBIT) >> 16);
		if (g_pitch >  CR_PITCH_MAX) g_pitch =  CR_PITCH_MAX;
		if (g_pitch < -CR_PITCH_MAX) g_pitch = -CR_PITCH_MAX;
	}
	g_down = down;
}

/* OURS. THE VIEW BACK TO REST, AND THE HOST'S COUNTS RE-LATCHED WITH
 * IT. Both of them run whether this part is looking or not, so a
 * notch or a press still outstanding would be applied on top of the
 * reset and undo it. Latching is also what stops a part entered after
 * somebody span the wheel elsewhere starting on the backlog. */
static void cr_home(void)
{
	g_depth = cr_clamp(cr_rest());
	g_orb_r = CR_FILL;
	g_ark_depth = cr_ark_clamp(((g_ark_r << 8) / CR_FILL) - CR_ARK_NEAR);
	g_cool = 0;
	g_wheel = Demo_Wheel();
	g_yaw = 0;
	g_pitch = 0;
	g_down = 0;
	g_held = 0;
	if (g_temple_ok) {
		s32 n;

		for (n = 0; n < 5L; n++) g_te_cam[n] = g_te_home[n];
		g_te_down = 0;
	}
	if (g_char == CR_CITY2) { if (g_city2_ok) cr_city2_enter(); }
	else if (g_city_ok) cr_city_enter();
}

/* OURS. The cycle as a thing to look at. THE DELTA IS WHAT COUNTS,
 * not the count itself: it arrives already running, and shift sends
 * it backwards. The list holds only what loaded, so a missing orb
 * closes its gap rather than leaving a slot that draws nothing. A
 * reset follows, because a turn made on one thing means nothing on
 * the next. */
static void cr_select(void)
{
	s32 n = Demo_Cycle();
	s32 d = n - g_cycle;

	if (!d || g_navail < 2L) { g_cycle = n; return; }
	g_cycle = n;
	g_slot = (((g_slot + d) % g_navail) + g_navail) % g_navail;
	g_char = g_avail[g_slot];
	cr_home();
}

/* OURS. THE ORB IS OPTIONAL AND THE VESSEL IS NOT: Orb_Init wants
 * Fcrust and Fspacedata out of relic.dat, so a run without the
 * archive has to lose the orb rather than the whole part, and the
 * cycle then has one thing in it instead of two. */
static int credits_init(void)
{
	s32 n;

	if (Ship_Init()) return 1;
	g_orb_ok = (Orb_Init() == 0);
	g_ark_set = *Temple_DefaultObjectSet();
	g_ark_set.glow_a = 0UL;
	g_ark_set.glow_b = 0UL;
	g_ark_ok = (Temple_UseObjectSet(&g_ark_set) == 0);
	if (g_ark_ok) g_ark_r = cr_ark_measure();
	g_city_ok = (cr_city_load() == 0);
	g_city2_ok = g_city_ok && (cr_city2_load() == 0);
	g_temple_ok = (Temple_TerrainInit(g_te_home) == 0);
	for (n = 0; n < 5L; n++) g_te_cam[n] = g_te_home[n];
	/* THE PLANET TABLE, because object 11 is textured from a
	 * framebuffer snapshot and there is none here - ship.h. */
	Ship_SetSkipTable(SHIP_SKIP_PLANET);
	Ship_ResetPose();
	Ship_Bounds(g_centre, &g_radius);
	g_navail = 0;
	g_avail[g_navail++] = CR_SHIP;
	if (g_orb_ok) g_avail[g_navail++] = CR_ORB;
	if (g_ark_ok) g_avail[g_navail++] = CR_ARK;
	if (g_city_ok) g_avail[g_navail++] = CR_CITY;
	if (g_city2_ok) g_avail[g_navail++] = CR_CITY2;
	if (g_temple_ok) g_avail[g_navail++] = CR_TEMPLE;
	g_slot = 0;
	g_char = CR_SHIP;
	g_cycle = Demo_Cycle();
	cr_home();
	g_ang = 0;
	g_oa = 0;
	g_ob = 0;
	g_ca = 0;
	g_cb = 0;
	g_scroll = 0;
	return 0;
}

/* OURS. THE TURNTABLE STOPS WHEN A HAND IS PUT ON IT, because an
 * orbit that sticks is pointless against a thing that keeps turning
 * out from under it. Demo_Home gives the spin back.
 *
 * A BURN STOPS WHERE ITS SHOT ENDS. Stills_StepThreshold floors at
 * zero, but each shot's loop runs a fixed count and leaves the front
 * well above that, so running on would burn further than the demo
 * ever shows. The end is the loop's, not a number of ours.
 *
 * EVERY RATE HERE IS A TICK'S, never a frame's - reconstruction.txt
 * 3.4. The cool-down walks both ways on the demo's one step, so
 * holding takes the orb apart at the speed the release puts it
 * together. THE ONE HELD BIT IS READ TWO WAYS: as a state by the
 * orb, and as an EDGE by the city, where a restart is an event. */
static void credits_step(s32 tick)
{
	if (tick <= 0L) {
		g_ang = 0;
		return;
	}
	if (!g_held) {
		g_ang = (g_ang + CR_SPIN) & 0x1fffL;
		g_oa = (g_oa + CR_ORB_SPIN_A) & 0xffL;
		g_ob = (g_ob + CR_ORB_SPIN_B) & 0xffL;
	}
	g_ca += CR_CRACK_A;
	g_cb += CR_CRACK_B;
	g_scroll -= CR_SCROLL;
	if (Demo_Hold() & DEMO_HOLD_A) {
		g_cool += CR_COOL_STEP;
		if (g_cool > CR_COOL_SEED) g_cool = CR_COOL_SEED;
	} else {
		g_cool -= CR_COOL_STEP;
		if (g_cool < 0L) g_cool = 0L;
	}
	{
		int z = (Demo_Hold() & DEMO_HOLD_A) != 0;

		if (z && !g_z_prev) {
			if (g_char == CR_CITY) cr_city_enter();
			if (g_char == CR_CITY2) cr_city2_enter();
		}
		g_z_prev = z;
	}
	if (g_char == CR_CITY || g_char == CR_CITY2) {
		s32 end = (g_char == CR_CITY) ? CR_CT_END : CR_C2_END;

		if (g_char == CR_CITY)
			Stills_StepThreshold(CR_CT_RATE);
		else if (!g_held)
			Stills_StepThreshold(CR_C2_RATE);
		if (Stills_Threshold() < end) Stills_SetThreshold(end);
		Stills_StepCursors();
		Stills_StepSpin(CR_CT_SPIN);
	}
}

/* OURS. THE ORB NEEDS NO CAMERA AT ALL - Orb_DrawSphere takes a
 * screen place and a radius, so the drag goes straight onto its two
 * angles and the wheel straight onto its size. ORIGIN 0x002cdaa6 for
 * the centre, which is the release's own 0x50, 0x32. */
static void cr_draw_orb(Relic_Frame *f)
{
	s32 a = (g_oa + (g_yaw >> CR_ORB_SHIFT)) & 0xffL;
	s32 b = (g_ob + (g_pitch >> CR_ORB_SHIFT)) & 0xffL;

	Gfx_SetLightBase(CR_ORB_LIGHT_R, CR_ORB_LIGHT_G, CR_ORB_LIGHT_B);
	Frame_Clear(f);
	Orb_DrawSphere(f, SPACE_W / 2L, SPACE_H / 2L, g_orb_r,
	               (g_cool >> 8) >> 1, a, b);
	Orb_DrawShells(f, SPACE_W / 2L, SPACE_H / 2L, g_orb_r,
	               g_ca, g_cb, 0L, a, b, g_scroll);
}

/* OURS - objset.h does the work; this part owns only the camera. */
static void cr_draw_temple(Relic_Frame *f)
{
	cr_temple_fly();
	Temple_TerrainDrawTo(f, g_te_cam);
}

/* ORIGIN 0x0031470e - shot two's loop, and its haze is drawn TWICE:
 * once at double the threshold under the debris and once straight
 * over it. The other shots haze only after the particles. */
static void cr_draw_city2(Relic_Frame *f)
{
	Gfx_SetLightBase(CR_CT_LIGHT_R, CR_CT_LIGHT_G, CR_CT_LIGHT_B);
	cr_city2_scrub();
	Stills_CityBase(f, g_c2_mask);
	Stills_DrawHaze(f, Stills_Threshold() * 2L);
	Stills_DrawParticles(f);
	Stills_DrawHaze(f, Stills_Threshold());
	Stills_Blur(f);
}

/* ORIGIN 0x0031463e - shot one's loop: the picture under the burn,
 * the debris, one haze pass over them, and the module's own smear.
 * Stills_CityBase writes every pixel, so nothing clears first. */
static void cr_draw_city(Relic_Frame *f)
{
	Gfx_SetLightBase(CR_CT_LIGHT_R, CR_CT_LIGHT_G, CR_CT_LIGHT_B);
	cr_city_scroll();
	Stills_CityBase(f, g_ct_mask);
	Stills_DrawParticles(f);
	Stills_DrawHaze(f, Stills_Threshold());
	Stills_Blur(f);
}

/* OURS. THE SAME FIVE THE VESSEL TAKES, because the object set's
 * projection subtracts the camera before it rotates exactly as
 * Scene_TransformProject does - objset.h. So the eye is placed the
 * one way section 7 describes, about the set's own origin: the
 * extent is measured from the frame's centre, which is where that
 * origin lands, so a body sitting off it is already counted. */
static void cr_draw_ark(Relic_Frame *f)
{
	s32 cam[SHIP_CAM], off[3];

	Gfx_SetLightBase(CR_ARK_LIGHT_R, CR_ARK_LIGHT_G, CR_ARK_LIGHT_B);
	Frame_Clear(f);

	off[0] = 0;
	off[1] = -g_ark_depth;
	off[2] = 0;
	Ship_Rot2d(&off[1], &off[2], -g_pitch);
	Ship_Rot2d(&off[0], &off[1], -g_yaw);

	cam[0] = off[0];
	cam[1] = off[1];
	cam[2] = off[2];
	cam[3] = g_yaw;
	cam[4] = g_pitch;
	Temple_DrawObjectSet(f, g_ang, cam);
}

/* OURS. THE PIVOT IS THE MODEL'S CENTRE, and placing the eye on it
 * takes two turns. The spin, because the transform turns vertices
 * about the geometry's origin before subtracting the camera, so an
 * unspun centre lets the model wander round the frame. Then the
 * INVERSE camera rotation, because that subtraction comes first too:
 * a distance put straight into cam[1] pans instead of orbiting.
 * docs/reconstruction.txt 7. */
static void cr_draw_ship(Relic_Frame *f)
{
	s32 cam[SHIP_CAM], off[3];

	/* ORIGIN 0x002cd102 - Ham8_BuildTables(0,0,0), part 2.3's base,
	 * so the light byte darkens rather than tinting. The hull's
	 * texels carry none of it either way. */
	Gfx_SetLightBase(0, 0, 0);
	Frame_Clear(f);
	if (!Ship_Ready()) return;

	off[0] = 0;
	off[1] = -g_depth;
	off[2] = 0;
	Ship_Rot2d(&off[1], &off[2], -g_pitch);
	Ship_Rot2d(&off[0], &off[1], -g_yaw);

	cam[0] = g_centre[0];
	cam[1] = g_centre[1];
	Ship_Rot2d(&cam[0], &cam[1], g_ang);
	cam[2] = g_centre[2];
	cam[0] += off[0];
	cam[1] += off[1];
	cam[2] += off[2];
	cam[3] = g_yaw;
	cam[4] = g_pitch;
	Ship_Draw(f, g_ang, cam);
}

/* OURS */
static void credits_frame(Relic_Frame *f, s32 tick)
{
	(void)tick;
	cr_select();
	if (Demo_Home()) cr_home();
	cr_wheel();
	cr_orbit();
	if (g_char == CR_ORB)       cr_draw_orb(f);
	else if (g_char == CR_ARK)  cr_draw_ark(f);
	else if (g_char == CR_CITY) cr_draw_city(f);
	else if (g_char == CR_CITY2) cr_draw_city2(f);
	else if (g_char == CR_TEMPLE) cr_draw_temple(f);
	else                        cr_draw_ship(f);
}

/* OURS */
static void credits_stats(char *out, int cap)
{
	(void)cap;
	if (g_char == CR_TEMPLE)
		sprintf(out, "temple cam %ld,%ld,%ld  head %ld  horizon %ld%s",
		        (long)g_te_cam[0], (long)g_te_cam[1], (long)g_te_cam[2],
		        (long)g_te_cam[3], (long)g_te_cam[4],
		        cr_te_step() > 0L ? "  FWD"
		                          : (cr_te_step() < 0L ? "  BACK" : ""));
	else if (g_char == CR_CITY2)
		sprintf(out, "city2 thresh %ld/%ld..%ld  parts %ld%s",
		        (long)Stills_Threshold(), (long)CR_C2_THRESH,
		        (long)CR_C2_END, (long)Stills_ParticleCount(),
		        g_c2_down ? "  SCRUB" : "");
	else if (g_char == CR_CITY)
		sprintf(out, "city thresh %ld  scroll %ld/%ld  parts %ld",
		        (long)Stills_Threshold(),
		        (long)(CR_CT_TRAVEL / 2L
		               - ((g_pitch * (CR_CT_TRAVEL / 2L))
		                  / CR_PITCH_MAX)),
		        (long)CR_CT_TRAVEL, (long)Stills_ParticleCount());
	else if (g_char == CR_ARK)
		sprintf(out, "ark  ang %ld  yaw %ld pitch %ld  depth %ld"
		             "  r %ld",
		        (long)g_ang, (long)g_yaw, (long)g_pitch,
		        (long)g_ark_depth, (long)g_ark_r);
	else if (g_char == CR_ORB)
		sprintf(out, "orb  a %ld b %ld  r %ld  cool %ld/%ld"
		             "  shells %ld pts %ld",
		        (long)g_oa, (long)g_ob, (long)g_orb_r,
		        (long)g_cool, (long)CR_COOL_SEED,
		        (long)Orb_ShellsDrawn(), (long)Orb_PointsLive());
	else
		sprintf(out, "ship ang %ld  yaw %ld pitch %ld  depth %ld"
		             "  r %ld  faces %ld",
		        (long)g_ang, (long)g_yaw, (long)g_pitch,
		        (long)g_depth, (long)g_radius, (long)Ship_FacesDrawn());
}

const Relic_Part PART_Credits = {
	"c.1", "The geometry, to look at",
	"OURS - no counterpart in the demo",
	PART_SHAPED_ONLY, SPACE_W, SPACE_H,
	credits_init, credits_step, credits_frame, credits_stats,
	0, 0, 0
};
