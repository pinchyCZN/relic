/* RELIC - the scene objects the two orb shots share: the six beams,
 * the six attach-point flares and the orb. See orbscene.h and
 * docs/effects/orb.txt.
 *
 * BOTH SHOTS DRAW THE SAME OBJECTS. Anim modes 0x13 and 0x12 advance
 * the bolt and orb clocks with identical increments and dispatch the
 * same slot types, so what differs between the two parts is the camera
 * and which objects are live - not how any of them is drawn.
 */

#include "orbscene.h"
#include "gfx.h"
#include "image.h"
#include "ship.h"
#include "space.h"
#include "beam.h"

/* ORIGIN 0x002d5c66 Anim_SceneTick, at +0x29a for mode 0x13 and
 * +0x3ca for mode 0x12 - BOTH advance g_OrbSpinA and g_OrbSpinB by
 * these, which is why neither shot needs the satellites retuned. */
#define ORB_SPIN_A      0x64L
#define ORB_SPIN_B      0xaL

static const s32 *g_cam;
static s32 g_obj_angle;
static s32 g_beam_a, g_beam_b;
static s32 g_live_beams;
static s32 g_attach_lit;
static s32 g_orb_lit;
/* ORIGIN 0x002e0b08 g_FlareSizeStep - THE STROBE, AND IT IS TICK
 * STATE. The draw only reads it; step() moves the base on. A draw
 * that carried it would strobe at the display's rate, not the
 * demo's. docs/effects/orb.txt 11.5. */
static s32 g_pulse_step;        /* the tick's base               */
static s32 g_pulse_cur;         /* the running index in a frame  */
static s32 g_pulse_used;        /* slots the counting frame read */
/* EXACTLY ONE FRAME A TICK COUNTS, the first after a step, because a
 * tick may draw twice or not at all. Without the arm the slot count
 * depends on whether a frame or a step came first, and that decides
 * the phase for the whole scene. reconstruction.txt 3.4. */
static int g_pulse_arm;
static int g_pulse_count;
static s32 g_orb_a, g_orb_b;    /* ORIGIN 0x002d2bf8, 0x002d2bfc */
static s32 g_orb_r;             /* OURS - the centre flare's radius */
static Relic_Extra g_extra[SHIP_EXTRAS];

/* ORIGIN 0x002ee84c g_BeamAttachPoints - the seven places a beam
 * ends. Six get a pulsing flare and phase 3 reveals them all the tick
 * after the first group strikes; the claw point waits for the second.
 * effects/orb.txt 4. */
#define ORB_ATTACH      0x002ee84cUL
#define ORB_ATTACH_N    6

/* ORIGIN 0x002ee9a8, 0x002ee9c8 - THE PULSE IS A TABLE, not a
 * waveform, and the rim and the spikes are given different ones. */
#define ORB_PULSE_RIM   0x002ee9a8UL
#define ORB_PULSE_SPIKE 0x002ee9c8UL

/* ORIGIN 0x002d28ca Scene_SortAndDrawAll, +0x272, the TYPE 3
 * branch - THE ORB,
 * and its slot's centroid is the claw attach point. Before it forms
 * it is TEN GLOW BLOBS ON A SPHERE of radius 0x2800 about that point,
 * each at its own pair of angles, plus one big pulsing flare at the
 * centre. Phase 6 reveals it. effects/orb.txt 6. */
#define ORB_SATS        10
#define ORB_SAT_R       0x2800L
#define ORB_SAT_SIZE    0x1eL
#define ORB_SAT_STEP_A  0x333L
#define ORB_SAT_STEP_B  0x514L
/* ORIGIN 0x002ee9e8 - the centre flare's own eight sizes, the biggest
 * of the three tables: 400 480 320 440 360 400 480 360. */
#define ORB_PULSE_MID   0x002ee9e8UL


/* OURS - the three attach-point tables read the one counter. */
static s32 pulse_radius_at(u32 tab, s32 depth, s32 step)
{
	s32 r = (s32)Image_U32(tab + (u32)((step & 7) * 4));

	r = (r << 11) / depth;
	if (r > 0x7fL) r = 0x7fL;
	return r;
}

/* ORIGIN 0x002d2a48 against 0x002d2c0a - A FLARE SLOT READS THE
 * COUNTER AND ADDS ONE; the centre flare reads and leaves it. */
static s32 pulse_radius(u32 tab, s32 depth)
{
	if (g_pulse_count) g_pulse_used++;
	return pulse_radius_at(tab, depth, g_pulse_cur++);
}

/* OURS - the attach point array into a vertex. */
static void attach_vert(s32 i, Relic_Vert *v)
{
	s32 j;

	for (j = 0; j < 3; j++)
		v->comp[j] = (s32)Image_U32(ORB_ATTACH + (u32)(i * 12 + j * 4));
}

/* ORIGIN 0x002d28ca Scene_SortAndDrawAll, +0x120, type 1 - a COLOURED
 * flare with rays, added to all four channels, unlike the rest of the
 * beam. Its radius is a table size over the projection's own depth. */
static void attach_flare(Relic_Frame *f, s32 i)
{
	Relic_Vert v;
	s32 sx, sy, depth;

	attach_vert(i, &v);
	if (!Ship_Project(&v, g_obj_angle, g_cam, &sx, &sy, &depth))
		return;
	Gfx_Flare(f, sx >> 8, sy >> 8,
	          pulse_radius(i < 3 ? ORB_PULSE_RIM : ORB_PULSE_SPIKE, depth),
	          g_RaySpikes, g_ColRampAmber);
}

/* ORIGIN 0x002d28ca Scene_SortAndDrawAll, +0x272 - the type 3
 * branch taken while g_OrbNearMode is clear. Ten blobs on a sphere about the
 * claw point, each at its own pair of angles so they do not sit on one
 * ring, then 0x002d2c00's big pulsing flare at the centre. */
static void orb_draw(Relic_Frame *f, s32 unused)
{
	Relic_Vert mid, v;
	s32 i, sx, sy, depth;

	(void)unused;
	attach_vert(6, &mid);
	for (i = 0; i < ORB_SATS; i++) {
		s32 p0 = ORB_SAT_R, p1 = 0, p2 = 0;
		/* ORIGIN 0x002d42d6 - the same two-axis rotation the camera
		 * uses, applied to a point out along one axis. */
		Ship_Rot2d(&p0, &p1, g_orb_a + i * ORB_SAT_STEP_A);
		Ship_Rot2d(&p1, &p2, g_orb_b + i * ORB_SAT_STEP_B);
		v.comp[0] = mid.comp[0] + p0;
		v.comp[1] = mid.comp[1] + p1;
		v.comp[2] = mid.comp[2] + p2;
		/* ORIGIN 0x002d2b78 - the flare's near plane here too. */
		if (!Ship_ProjectFlare(&v, g_obj_angle, g_cam, &sx, &sy, &depth))
			continue;
		Gfx_GlowBlob(f, sx, sy,
		             (ORB_SAT_SIZE << 11) / depth, g_RampBolt);
	}
	/* ORIGIN 0x002d2c00 Orb_DrawCentreFlare - the centre, on the same
	 * counter as the attach points, off the largest of the three
	 * tables. NOT JUST A FLARE: it borrows the sun's ghosts and feeds
	 * the horizon band, and its near plane is 0x32 where the hull's
	 * 0xc8 would reject the orb outright. docs/effects/orb.txt 11. */
	if (!Ship_ProjectFlare(&mid, g_obj_angle, g_cam, &sx, &sy, &depth))
		return;
	g_orb_r = pulse_radius_at(ORB_PULSE_MID, depth, g_pulse_cur);
	Gfx_Flare(f, sx, sy, g_orb_r, g_RaySpikes, g_ColRampAmber);
	Space_DrawOrbGhosts(f, sx, sy);
	Space_StepOrbBand(sx, sy, g_orb_r);
}

/* OURS - a beam's two endpoints. */
static void beam_ends(s32 i, Relic_Vert *a, Relic_Vert *b)
{
	Beam_Geom(i, a, b);
}

/* ORIGIN 0x002d28ca Scene_SortAndDrawAll, +0xec, type 2. */
static void beam_extra(Relic_Frame *f, s32 i)
{
	Relic_Vert a, b;
	s32 x0, y0, x1, y1;

	beam_ends(i, &a, &b);
	if (!Ship_Project(&a, g_obj_angle, g_cam, &x0, &y0, 0)) return;
	if (!Ship_Project(&b, g_obj_angle, g_cam, &x1, &y1, 0)) return;
	Beam_Draw(f, i, x0, y0, x1, y1, (i < 3) ? g_beam_a : g_beam_b);
}

/* ORIGIN 0x002cfeba Obj_SetCentroid - a beam's slot carries the MEAN
 * of its vertices, so for two that is the midpoint; a flare's and the
 * orb's carry the point itself, Space_BuildObjects writing those
 * straight in without calling it. */
void OrbScene_Register(const s32 *cam, s32 obj_angle)
{
	s32 n = 0, i, j;

	g_pulse_cur = g_pulse_step;
	g_pulse_count = g_pulse_arm;
	if (g_pulse_arm) {
		g_pulse_used = 0;
		g_pulse_arm = 0;
	}

	g_cam = cam;
	g_obj_angle = obj_angle;

	for (i = 0; i < g_live_beams && i < BEAM_COUNT; i++) {
		Relic_Vert a, b;
		beam_ends(i, &a, &b);
		for (j = 0; j < 3; j++)
			g_extra[n].cent[j] = (a.comp[j] + b.comp[j]) / 2;
		g_extra[n].draw = beam_extra;
		g_extra[n].which = i;
		n++;
	}
	if (g_attach_lit) {
		for (i = 0; i < ORB_ATTACH_N; i++) {
			Relic_Vert v;
			attach_vert(i, &v);
			for (j = 0; j < 3; j++) g_extra[n].cent[j] = v.comp[j];
			g_extra[n].draw = attach_flare;
			g_extra[n].which = i;
			n++;
		}
	}
	if (g_orb_lit) {
		Relic_Vert v;
		attach_vert(6, &v);
		for (j = 0; j < 3; j++) g_extra[n].cent[j] = v.comp[j];
		g_extra[n].draw = orb_draw;
		g_extra[n].which = 0;
		n++;
	}
	Ship_SetExtras(g_extra, n);
}


/* OURS */
void OrbScene_Reset(void)
{
	g_beam_a = 0;
	g_beam_b = 0;
	g_live_beams = 0;
	g_attach_lit = 0;
	g_orb_lit = 0;
	g_pulse_step = 0;
	g_pulse_cur = 0;
	g_pulse_used = 0;
	g_pulse_arm = 0;
	g_pulse_count = 0;
	g_orb_a = 0;
	g_orb_b = 0;
	g_orb_r = 0;
	Beam_Reset();
}

/* ORIGIN 0x002d5c66 Anim_SceneTick, at +0x29a and +0x3ca - the same
 * two increments in both modes. */
void OrbScene_Step(void)
{
	Beam_Step();
	/* ORIGIN 0x002d6320 Anim_SceneTick's one, plus the slots that
	 * read it while the frame drew - 0x002d2a4e advances per slot. */
	g_pulse_step += 1 + g_pulse_used;
	g_pulse_arm = 1;
	g_orb_a += ORB_SPIN_A;
	g_orb_b += ORB_SPIN_B;
}

/* OURS */
void OrbScene_SetBeams(s32 live, s32 a, s32 b)
{
	g_live_beams = live;
	g_beam_a = a;
	g_beam_b = b;
}

/* OURS */
void OrbScene_SetLit(int flares, int orb)
{
	g_attach_lit = flares;
	g_orb_lit = orb;
}

/* OURS */
s32 OrbScene_OrbRadius(void) { return g_orb_r; }
