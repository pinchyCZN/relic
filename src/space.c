/* RELIC - the space backdrop, shared by every scene that has one.
 *
 * The sky, the planet and its halo, the sun's projection, the lens
 * flare and the glare band are ONE SET OF ROUTINES IN THE DEMO and
 * several scenes call them with different parameters - see
 * effects/approach.txt section 7. What differs per scene is the base
 * colour, the sun and planet world positions and the camera; what
 * does not is anything below.
 *
 * The scene order of these steps is NOT here. Each of the demo's
 * frame routines calls them in its own order, with its own
 * substitutions, so each lives in its own part file - rule 17.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relic.h"
#include "gfx.h"
#include "host.h"
#include "image.h"
#include "space.h"

#define A_GHOST32A  0x002e4990UL    /* 32x32, one byte a pixel       */
#define A_GHOST32B  0x002e4d90UL
#define A_GHOST16   0x002e5190UL
#define A_PLANET    0x002e5290UL    /* g_PlanetSprite, 64x64 x 4     */
#define A_HALORAMP  0x002e13c8UL    /* g_HaloRamp, 256 x 4           */
#define PLANET_SIDE 64
#define HALO_SIDE   80

#define SKY_GRID    256             /* the direction table is 256x256 */
#define SKY_WRAP    0x3ffffL        /* andi.l #0x3ffff - Fplasma's size */
/* ORIGIN 0x002d2db2 - corner C and the per-column and per-row steps
 * that fall out of the three projected corners at zero rotation,
 * which is what this scene uses. */
#define SKY_U0      0x5000L
#define SKY_V0      (-0x3200L)
#define SKY_STEP    0x100L

#define FOCAL       0x0fa0L         /* the z both projections use    */

#define A_STAMPMASK 0x002d1d78UL    /* 12 x 12, one byte a cell      */
#define STAMP_SIDE  12

/* ORIGIN 0x002d1e08 - g_SunRange, and it is STATE, not a constant: the
 * save-under pair measures how much of the sun the planet leaves
 * visible and eases this toward it every frame. It sets the first
 * flare's radius, gates the ghosts, and sizes the second flare.
 *
 * SO THIS PART IS NO LONGER A PURE FUNCTION OF ITS TICK. Rendering one
 * tick on its own gives the value this has reached, not the value that
 * tick would have had - run the ticks in order to compare against the
 * demo. See effects/planet_sun.txt sections 2 and 7. */
static s32 g_sun_range;
/* 0x002e0b6a and 0x002e0b6e - the band's own eased pair, and 0x002e0bea,
 * the mean row of the visible cells that it is laid across. */
static s32 g_band_now, g_band_target, g_vis_mean_y;
static s32 g_orb_band_x;      /* 0x002e0b3e, the orb flare's x */
static s32 g_band_d5;
static int g_band_on;
static int g_stamp_ok;

static u8 *g_ghost32a;              /* 32*32*4 */
static u8 *g_ghost32b;
static u8 *g_ghost16;               /* 16*16*4 */
static u8 *g_planet;                /* 64*64*4, darkened           */
static u8 *g_planet_mask;           /* 64*64,   derived            */
static s32 g_planet_wx, g_planet_wz;   /* g_PlanetWorldX/Z */

/* OURS - space.h */
void Space_SetPlanetWorld(s32 wx, s32 wz)
{
	g_planet_wx = wx;
	g_planet_wz = wz;
}

/* OURS - space.h */
const u8 *Space_PlanetMask(void) { return g_planet_mask; }
static u8 *g_halo;                  /* 80*80*4, procedural         */
static s32 *g_sky_dir;              /* 256*256 texel byte offsets   */
static const u8 *g_sky_tex;         /* 256x256x4, generated         */
static u8 *g_sky_own;               /* the same buffer, writable    */

/* ORIGIN 0x002cf52e - one byte a pixel out to the four-byte pixel,
 * with a per-channel right shift. Flare_BuildGhostSprites passes
 * (1, 1, 0), so R and G come out halved and B full: the ghosts are
 * blue-white, not the sun's amber. The LIGHT byte is set to zero
 * because Sprite_Add32x32 adds all four channels in one add.l. */
static u8 *expand_sprite(u32 addr, s32 n, s32 sr, s32 sg, s32 sb)
{
	const u8 *src = Image_At(addr);
	u8 *out;
	s32 i;

	if (!src) return 0;
	out = (u8 *)malloc((size_t)n * PX_BYTES);
	if (!out) return 0;
	for (i = 0; i < n; i++) {
		u8 s = src[i];
		out[i * PX_BYTES + PX_R]     = (u8)(s >> sr);
		out[i * PX_BYTES + PX_G]     = (u8)(s >> sg);
		out[i * PX_BYTES + PX_LIGHT] = 0;
		out[i * PX_BYTES + PX_B]     = (u8)(s >> sb);
	}
	return out;
}

/* ORIGIN 0x002cee84  Planet_BuildSpriteAndMask.
 *
 * THE MASK IS COMPUTED, NOT STORED: a texel whose three channels sum
 * to 1 or less is background, so the art needs no alpha and its
 * near-black border becomes the cutout. R, G and B also lose 4,
 * clamped at zero. */
static int planet_build(void)
{
	const u8 *src = Image_At(A_PLANET);
	s32 i;

	if (!src) return 1;
	g_planet = (u8 *)malloc((size_t)PLANET_SIDE * PLANET_SIDE * PX_BYTES);
	g_planet_mask = (u8 *)malloc((size_t)PLANET_SIDE * PLANET_SIDE);
	if (!g_planet || !g_planet_mask) return 1;
	for (i = 0; i < PLANET_SIDE * PLANET_SIDE; i++) {
		s32 c, k, sum = 0;
		/* THREE CHANNELS, NOT FOUR: the original's rol.l #8 steps over
		 * the LIGHT byte and copies it through untouched. */
		static const int CH[3] = { PX_B, PX_R, PX_G };
		g_planet[i * PX_BYTES + PX_LIGHT] = src[i * PX_BYTES + PX_LIGHT];
		for (k = 0; k < 3; k++) {
			c = src[i * PX_BYTES + CH[k]] - 4;
			if (c < 0) c = 0;
			g_planet[i * PX_BYTES + CH[k]] = (u8)c;
		}
		sum = src[i * PX_BYTES + PX_R] + src[i * PX_BYTES + PX_G]
		    + src[i * PX_BYTES + PX_B];
		g_planet_mask[i] = (u8)(sum < 1 ? 0 : 1);
	}
	return 0;
}

/* ORIGIN 0x002ceeec  Planet_BuildHaloRing - the atmosphere.
 *
 * ABS OF THE DISTANCE FROM A RADIUS MAKES A RING, not a disc: the
 * brightest entry falls where the radius is exactly 0xf4, which at 8
 * units a cell is 30.5 of the sprite's 40. g_HaloRamp keeps R at
 * zero, so the halo is a cool rim against the sun's amber. */
static int halo_build(void)
{
	const u8 *ramp = Image_At(A_HALORAMP);
	s32 cy, cx;

	if (!ramp) return 1;
	g_halo = (u8 *)malloc((size_t)HALO_SIDE * HALO_SIDE * PX_BYTES);
	if (!g_halo) return 1;
	for (cy = 0; cy < HALO_SIDE; cy++) {
		s32 y = -0x140L + cy * 8;
		for (cx = 0; cx < HALO_SIDE; cx++) {
			s32 x = -0x140L + cx * 8;
			s32 d = Gfx_Isqrt(x * x + y * y) - 0xf4L;
			s32 grad = x + 0x140L;
			s32 k;
			d *= 4;
			if (d < 0) d = -d;
			if (d > 0xff) d = 0xff;
			/* THE RAMP ENTRY IS NOT THE COLOUR. Each of its bytes 1,
			 * 2 and 3 becomes R, G and B - byte 0 is unused and the
			 * light byte is forced to zero - and each is then
			 * (c >> 2) * (x + 0x140) >> 8 >> 2. That is a factor of
			 * about seven down from the raw entry and a horizontal
			 * gradient on top, so the halo is a DIM rim. Taking the
			 * entry as the pixel makes it glare and gives it a large
			 * light value it should not have. */
			for (k = 0; k < 3; k++) {
				s32 c = ramp[d * 4 + 1 + k] >> 2;
				c = ((c * grad) >> 8) >> 2;
				if (c > 0x3f) c = 0x3f;
				g_halo[((size_t)cy * HALO_SIDE + cx) * PX_BYTES
				       + (k == 0 ? PX_R : k == 1 ? PX_G : PX_B)] = (u8)c;
			}
			g_halo[((size_t)cy * HALO_SIDE + cx) * PX_BYTES + PX_LIGHT] = 0;
		}
	}
	return 0;
}

/* ORIGIN 0x002d3566  Sky_BuildSquareTable - 256 squares, each with a
 * Z TERM ADDED IN. Both axes of Sky_BuildDirTable draw from this, so
 * the root below is sqrt(x*x + y*y + z*z) with z fixed near 22434 -
 * which is what makes the sky a sphere map and not a flat radius. */
static void sky_square_table(s32 *t)
{
	s32 i, v = -0x8000L;

	for (i = 0; i < SKY_GRID; i++) {
		t[i] = v * v + 0xf000000L;
		v += SKY_STEP;
	}
}

/* ORIGIN 0x002d3990  Sky_BuildDirTable - 65536 longwords turning a
 * screen offset into a texel byte offset.
 *
 *     r     = isqrt(x*x + y*y + 2*0xf000000)
 *     nx    = (x << 15) / r        the unit direction times 32768
 *     ny    = (y << 15) / r
 *     entry = (ny & ~0xff) * 4 + (ny & ~0xff) / 64
 *
 * The packing is a row and column offset into a 256 x 256 four-byte
 * texture: 1024 a row, 4 a column. Both operands are multiples of
 * 0x100, so the multiply and divide are exact and need no asr. */
static int sky_build_dir(void)
{
	s32 *sq;
	s32 yi, xi;

	sq = (s32 *)malloc(SKY_GRID * sizeof(s32));
	g_sky_dir = (s32 *)malloc((size_t)SKY_GRID * SKY_GRID * sizeof(s32));
	if (!sq || !g_sky_dir) { free(sq); return 1; }
	sky_square_table(sq);
	for (yi = 0; yi < SKY_GRID; yi++) {
		s32 y = -0x8000L + yi * SKY_STEP;
		for (xi = 0; xi < SKY_GRID; xi++) {
			s32 x = -0x8000L + xi * SKY_STEP;
			s32 r = Gfx_Isqrt(sq[xi] + sq[yi]);
			s32 nx, ny;
			if (r < 1) r = 1;
			nx = (x << 15) / r;
			ny = (y << 15) / r;
			g_sky_dir[(size_t)yi * SKY_GRID + xi] =
			    (ny & ~0xffL) * 4L + (nx & ~0xffL) / 64L;
		}
	}
	free(sq);
	return 0;
}

/* ORIGIN 0x002d3c1c and 0x002d3c6e - TEN SOFT GREY BLOBS, baked into
 * the texture between the plasma and the stars.
 *
 * Each is a size x size square at a random texel, with a 1/r^2 profile:
 *
 *     v = ((size * 32 - r2) / r2) >> 6, floored at 0, capped at 0x3f
 *
 * and 0x002d3d02 adds v to R, G and B alike, clamped at 0x3f, leaving
 * LIGHT alone - so the blob is GREY, not tinted like the ghosts.
 *
 * IT IS NOT ROUND. size reaches 0x143, wider than the 256-texel
 * texture, and the original wraps only on the ROW step: within a row it
 * just walks on past the right edge into the next one. That smear is
 * what makes these read as ovals rather than discs.
 *
 * AND THE TEN OF THEM MOVE THE STARS. They draw 30 numbers from the LCG
 * before Sky_AddStars draws its own, so leaving them out does not merely
 * lose the blobs - it puts all 500 stars somewhere else.
 *
 * Ours stops at the end of the texture. The original can run up to
 * size * 4 bytes past it, into the direction table that the pool puts
 * immediately after, and we are not going to reproduce that. */
static void sky_add_nebulae(void)
{
	const s32 END = SKY_GRID * SKY_GRID * PX_BYTES;
	s32 n;

	for (n = 0; n < 10; n++) {
		s32 row = Gfx_Rand(0x100);
		s32 col = Gfx_Rand(0x100);
		s32 size = Gfx_Rand(0x140) + 4;
		s32 num = size << 5;
		s32 half = size >> 1;
		s32 off = row * (SKY_GRID * PX_BYTES) + col * PX_BYTES;
		s32 iy, ix;

		for (iy = 0; iy < size; iy++) {
			s32 y = iy - half;
			s32 o = off;
			for (ix = 0; ix < size; ix++) {
				s32 x = ix - half;
				s32 r2 = x * x + y * y;
				s32 v = num - r2;
				s32 k;
				if (r2) v /= r2;
				v >>= 6;
				if (v < 0) v = 0;
				if (v > CHAN_MAX) v = CHAN_MAX;
				if (o >= 0 && o + PX_BYTES <= END) {
					for (k = 0; k < 3; k++) {
						s32 c = (k == 0) ? PX_R : (k == 1) ? PX_G : PX_B;
						s32 t = g_sky_own[o + c] + v;
						g_sky_own[o + c] = (u8)(t > CHAN_MAX ? CHAN_MAX : t);
					}
				}
				o += PX_BYTES;
			}
			off += SKY_GRID * PX_BYTES;
			if (off >= END) off -= END;
		}
	}
}

/* ORIGIN 0x002d3a12 - 500 STARS POKED INTO THE TEXTURE, each a plus
 * of five pixels: a bright centre and four arms at +3,+3,+4. The
 * centre brightness ALTERNATES between 0x0f0f000f and 0x04040004
 * every star, so there are two magnitudes. Positions come from the
 * demo's own LCG, so the field is deterministic and reproducing the
 * generator reproduces the sky exactly.
 *
 * They are ADDED, not written, and the plasma under them only
 * reaches 14 - so the stars are the only bright thing in the
 * background, which is why the field looks washed out without
 * them. */
static void sky_add_stars(void)
{
	s32 n;
	u32 bright = 0x0f0f000fUL, dim = 0x04040004UL;

	for (n = 0; n < 500; n++) {
		s32 row = Gfx_Rand(0xfa) + 1;
		s32 col = Gfx_Rand(0xfa) + 1;
		s32 o = row * (SKY_GRID * PX_BYTES) + col * PX_BYTES;
		u32 ctr = (n & 1) ? dim : bright;
		s32 k;
		static const s32 arm[4] = { PX_BYTES, -PX_BYTES,
		                            -(SKY_GRID * PX_BYTES),
		                            (SKY_GRID * PX_BYTES) };
		for (k = 0; k < PX_BYTES; k++) {
			s32 shift = 24 - k * 8;
			s32 add = (s32)((ctr >> shift) & 0xffUL);
			s32 v = g_sky_own[o + k] + add;
			g_sky_own[o + k] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
		}
		for (k = 0; k < 4; k++) {
			s32 a = o + arm[k], j;
			if (a < 0 || a + PX_BYTES > SKY_GRID * SKY_GRID * PX_BYTES)
				continue;
			for (j = 0; j < PX_BYTES; j++) {
				s32 add = (j == PX_B) ? 4 : (j == PX_LIGHT ? 0 : 3);
				s32 v = g_sky_own[a + j] + add;
				g_sky_own[a + j] = (u8)(v > CHAN_MAX ? CHAN_MAX : v);
			}
		}
	}
}

/* ORIGIN 0x002d3a94 - THE SKY IS GENERATED, NOT LOADED. Six sine
 * tables at fixed offsets, read in pairs, one pair a channel:
 *
 *     t = (((s1 >> 1) + s2) >> 8) + 0x40 , floored at 0
 *     c = ((t >> a) + 0x10) >> b
 *
 * with (a,b) of (2,2) for blue, (3,3) for green and (2,3) for red.
 * Each pointer advances per pixel and again per row, by its own
 * stride, wrapping at 8192 entries - which is what makes the field
 * drift rather than repeat.
 *
 * THE RESULT IS VERY DIM: blue reaches 14 of 0x3f, red 7 and green
 * 4. Substituting Fplasma, whose size and format match, gives a
 * field several times brighter and the wrong way round in hue, and
 * the sun's additive flare then clips against it. */
static int sky_build_texture(void)
{
	static const s32 base[6] = { 0, 0x63d0/4, 0x3f8/4,
	                             0x1ff0/4, 0x1520/4, 0x32e0/4 };
	static const s32 perpx[6] = { 128, 64, 64, 128, 64, 32 };
	static const s32 perrow[6] = { 32, 64, 32, 32, 64, 32 };
	s32 p[6], rowp[6], y, x, i;
	u8 *out;

	out = (u8 *)malloc((size_t)SKY_GRID * SKY_GRID * PX_BYTES);
	if (!out) return 1;
	g_sky_own = out;
	g_sky_tex = out;
	for (i = 0; i < 6; i++) rowp[i] = base[i];
	for (y = 0; y < SKY_GRID; y++) {
		for (i = 0; i < 6; i++) p[i] = rowp[i];
		for (x = 0; x < SKY_GRID; x++) {
			s32 c[3];
			static const s32 sh[3][2] = { { 2, 2 }, { 3, 3 }, { 2, 3 } };
			for (i = 0; i < 3; i++) {
				s32 s1 = Gfx_Sin(p[i * 2]);
				s32 s2 = Gfx_Sin(p[i * 2 + 1]);
				s32 t = (((s1 >> 1) + s2) >> 8) + 0x40L;
				if (t < 0) t = 0;
				t = ((t >> sh[i][0]) + 0x10L) >> sh[i][1];
				if (t > CHAN_MAX) t = CHAN_MAX;
				c[i] = t;
			}
			out[PX_B]     = (u8)c[0];
			out[PX_G]     = (u8)c[1];
			out[PX_R]     = (u8)c[2];
			out[PX_LIGHT] = 0;
			out += PX_BYTES;
			for (i = 0; i < 6; i++) p[i] = (p[i] + perpx[i]) & 0x1fffL;
		}
		for (i = 0; i < 6; i++) rowp[i] = (rowp[i] + perrow[i]) & 0x1fffL;
	}
	sky_add_nebulae();          /* before the stars: it moves their LCG */
	sky_add_stars();
	return 0;
}

/* ORIGIN 0x002d4132 - a 14-bit 2D rotation, and the D6 the caller
 * loads before it is dead: the routine's first act is to overwrite D6
 * with the cosine. */
static void sky_rot(s32 *x, s32 *y, s32 a)
{
	s32 co = Gfx_Cos(a & 0x1fffL);
	s32 sn = Gfx_Sin(a & 0x1fffL);
	s32 ix = *x, iy = *y;

	*x = ((ix * co) + (iy * sn)) >> 14;
	*y = ((iy * co) - (ix * sn)) >> 14;
}

/* ORIGIN 0x002d2db2 Scene_DrawTexturedSky - the starfield, per pixel.
 *
 * IT IS A DIRECTION LOOKUP, NOT A SCROLL. The screen's three corners
 * (+0x5000,-0x3200), (-0x5000,-0x3200) and (+0x5000,+0x3200) are
 * rotated by the LATCHED HEAD ANGLE, the differences give a per-column
 * and a per-row step, and the interpolated direction indexes a 256 x
 * 256 longword table of texture offsets. The two sky angles contribute
 * one additive offset on top - that is the pan, and it is all this
 * routine takes from them.
 *
 * SO THE HEAD ANGLE IS THE THIRD AXIS. It accumulates as
 * (yaw delta * sin(-pitch)) >> 6 in maybe_Camera_MoveRotated, so it
 * stays near zero while the camera is level and swings hard once the
 * camera is pitched - which is why a fixed set of axes looks right
 * until the camera goes under the ship and then sends the stars off
 * in a direction of their own. Reading 0x002e0b5a mid-sweep gives
 * 0x53146, which is 1329 of 8192 - about 58 degrees [observed].
 *
 * The step is carried in 8.8 so a column advances the index by a
 * fraction, as the original's addx chain does. */
void Space_DrawSky(Relic_Frame *f, s32 pan, s32 head)
{
	s32 c1x = -0x5000L, c1y = -0x3200L;
	s32 c2x =  0x5000L, c2y =  0x3200L;
	s32 c3x =  0x5000L, c3y = -0x3200L;
	s32 dcx, dcy, drx, dry;
	s32 py, px;

	if (!g_sky_tex || !g_sky_dir || f->w < 1 || f->h < 1) return;
	sky_rot(&c1x, &c1y, head >> 8);
	sky_rot(&c2x, &c2y, head >> 8);
	sky_rot(&c3x, &c3y, head >> 8);
	dcx = ((c1x - c3x) << 8) / f->w;
	dcy = ((c1y - c3y) << 8) / f->w;
	drx = ((c2x - c3x) << 8) / f->h;
	dry = ((c2y - c3y) << 8) / f->h;

	for (py = 0; py < f->h; py++) {
		s32 x = (c3x << 8) + drx * py;
		s32 y = (c3y << 8) + dry * py;
		u8 *q = f->px + (u32)(py * f->w) * PX_BYTES;
		for (px = 0; px < f->w; px++, x += dcx, y += dcy, q += PX_BYTES) {
			s32 w = (s32)(s16)((((y >> 16) & 0xffL) << 8)
			                  | ((x >> 16) & 0xffL));
			s32 e = 0x8080L + w;
			const u8 *t;
			if (e < 0 || e >= SKY_GRID * SKY_GRID) continue;
			t = g_sky_tex + ((g_sky_dir[e] + pan) & SKY_WRAP);
			q[PX_R]     = t[PX_R];
			q[PX_G]     = t[PX_G];
			q[PX_LIGHT] = t[PX_LIGHT];
			q[PX_B]     = t[PX_B];
		}
	}
}

/* OURS */
int Space_Init(void)
{
	if (g_ghost32a) return 0;
	if (sky_build_dir() || sky_build_texture()) {
		fprintf(stderr, "sun: no sky\n");
		return 1;
	}
	if (planet_build() || halo_build()) {
		fprintf(stderr, "sun: no planet art at 0x%08lx\n",
		        (unsigned long)A_PLANET);
		return 1;
	}
	g_ghost32a = expand_sprite(A_GHOST32A, 32 * 32, 1, 1, 0);
	g_ghost32b = expand_sprite(A_GHOST32B, 32 * 32, 1, 1, 0);
	g_ghost16  = expand_sprite(A_GHOST16,  16 * 16, 1, 1, 0);
	if (!g_ghost32a || !g_ghost32b || !g_ghost16) {
		fprintf(stderr, "sun: no ghost sprites at 0x%08lx\n",
		        (unsigned long)A_GHOST32A);
		return 1;
	}
	return 0;
}

/* OURS. The camera the projection turns the backdrop by. */
static s32 g_cam_yaw, g_cam_pitch;

/* OURS */
void Space_SetCamera(s32 yaw, s32 pitch)
{
	g_cam_yaw = yaw;
	g_cam_pitch = pitch;
}

/* OURS - space.h. host.h's GESTURE, SPRUNG. DOWN IS A NEGATIVE
 * PITCH: Space_Project takes the pitch term off y, so a scene only
 * follows the mouse if a drag down pitches down. */
void Space_Look(s32 *yaw, s32 *pitch)
{
	s32 x = 0, y = 0;

	Demo_Drag((int *)0, &x, &y);
	*yaw   =  (x * SPACE_LOOK_MAX) >> 16;
	*pitch = -((y * SPACE_LOOK_MAX) >> 16);
}

/* ORIGIN 0x002ceab4 Scene_ProjectPlanet and 0x002d2742
 * Scene_ProjectAndDrawSun - and they are the same projection. Each
 * loads its object's x and y, puts the FOCAL LENGTH IN AS THE DEPTH,
 * rotates the triple by the camera through 0x002d4396, and only then
 * divides.
 *
 * SO THE BACKDROP MOVES IN UNISON. 0xfa0 is the depth going in, not
 * the divisor: a camera yaw swings the sun and the planet across the
 * frame together and changes their depth as it goes, which is what a
 * viewer sees when the camera pans and the planet, sun and stars all
 * travel the other way. Projecting with 0xfa0 as a fixed divisor
 * instead drifts whichever object has its own motion and leaves the
 * rest still.
 *
 * AT YAW AND PITCH ZERO THIS REDUCES TO THAT FIXED DIVIDE exactly -
 * cos 0 is 0x4000 and the shift is 14, so the depth stays FOCAL - and
 * part 2.3 sets both to zero. */
int Space_Project(s32 wx, s32 wy, s32 *sx, s32 *sy)
{
	s32 x = wx, z = FOCAL, y = wy, c, sn, a;

	c = Gfx_Cos(g_cam_yaw & 0x1fffL);
	sn = Gfx_Sin(g_cam_yaw & 0x1fffL);
	a = x;
	x = (a * c + z * sn) >> 14;
	z = (z * c - a * sn) >> 14;
	c = Gfx_Cos(g_cam_pitch & 0x1fffL);
	sn = Gfx_Sin(g_cam_pitch & 0x1fffL);
	a = z;
	z = (a * c + y * sn) >> 14;
	y = (y * c - a * sn) >> 14;
	/* THE DEMO REJECTS AND DRAWS NOTHING when the rotated depth is
	 * not positive - Scene_ProjectPlanet returns -1 and
	 * Scene_ProjectAndDrawSun just returns. CLAMPING THE DEPTH TO 1
	 * INSTEAD IS A CRASH: (x << 8) / 1 is a coordinate in the
	 * millions, and the sprite blitters walk off the frame with it. */
	if (z <= 0) {
		*sx = *sy = 0;
		return 0;
	}
	*sx = ((x << 8) / z) + (SPACE_W / 2);
	*sy = ((y << 8) / z) + (SPACE_H / 2);
	return 1;
}

/* ORIGIN 0x002cf3d0 Sprite_Add32x32 and 0x002cf44e Sprite_Add16x16 -
 * one routine at two sizes. The inner loop masks 0x3f3f003f and does
 * ONE add.l over the whole pixel, so the light byte must contribute
 * nothing and its carry must not reach blue. Here the channels are
 * added separately and clamped, which is the same result except at
 * the overflow the original tolerates.
 *
 * Centre and size are in DEMO UNITS; the footprint is scaled on the
 * way out so a sprite pixel covers its whole scaled cell. */
static void sprite_add(Relic_Frame *f, const u8 *spr, s32 side,
                       s32 cx, s32 cy, s32 shift)
{
	s32 y, x;

	if (!spr) return;
	for (y = 0; y < side; y++) {
		s32 dy = cy - side / 2 + y;
		s32 py0, py1;
		if (dy < 0) continue;
		py0 = FX_MUL(dy, f->scale);
		py1 = FX_MUL(dy + 1, f->scale);
		if (py0 >= f->h) continue;
		for (x = 0; x < side; x++) {
			s32 dx = cx - side / 2 + x;
			const u8 *s = spr + ((size_t)y * side + x) * PX_BYTES;
			s32 px0, px1, py, px;
			if (dx < 0) continue;
			px0 = FX_MUL(dx, f->scale);
			px1 = FX_MUL(dx + 1, f->scale);
			if (px0 >= f->w) continue;
			for (py = py0; py < py1 && py < f->h; py++) {
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *q = f->px
					      + ((size_t)(py * f->w) + (size_t)px) * PX_BYTES;
					s32 c;
					/* 0xff, NOT 0x3f. Sprite_Add masks the SOURCE to
					 * 0x3f3f003f and then does a plain add.l with no
					 * ceiling, so a destination the flare has already
					 * carried past 0x3f must be left alone. Clamping to
					 * 0x3f here pulled such pixels DOWN - 0x59 became
					 * 0x3f - and painted a dark square the size of the
					 * ghost inside the brightest part of the sun, for
					 * exactly the frames the gate was open. The band at
					 * 0x002d3d34 does clamp to 0x3f; this does not. */
					c = q[PX_R] + (s[PX_R] >> shift);
					q[PX_R] = (u8)(c > 0xff ? 0xff : c);
					c = q[PX_G] + (s[PX_G] >> shift);
					q[PX_G] = (u8)(c > 0xff ? 0xff : c);
					c = q[PX_B] + (s[PX_B] >> shift);
					q[PX_B] = (u8)(c > 0xff ? 0xff : c);
				}
			}
		}
	}
}

/* ORIGIN 0x002ceaf8 pass one - a masked OPAQUE copy. A zero mask
 * byte skips the pixel; anything else replaces it outright. */
static void blit_masked(Relic_Frame *f, const u8 *spr, const u8 *mask,
                        s32 side, s32 ox, s32 oy)
{
	s32 y, x, k;

	if (!spr || !mask) return;
	for (y = 0; y < side; y++) {
		for (x = 0; x < side; x++) {
			size_t si = (size_t)y * side + x;
			s32 py0, py1, px0, px1, py, px;
			if (!mask[si]) continue;
			py0 = FX_MUL(oy + y, f->scale);
			py1 = FX_MUL(oy + y + 1, f->scale);
			px0 = FX_MUL(ox + x, f->scale);
			px1 = FX_MUL(ox + x + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				if (py < 0) continue;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *q;
					if (px < 0) continue;
					q = f->px + ((size_t)(py * f->w) + (size_t)px) * PX_BYTES;
					for (k = 0; k < PX_BYTES; k++)
						q[k] = spr[si * PX_BYTES + k];
				}
			}
		}
	}
}

/* ORIGIN 0x002ceaf8 pass two - ADDITIVE over all four channels, the
 * light byte included, which is how the halo both tints blue and
 * dims: g_HaloRamp carries light as well as colour. */
static void blit_additive(Relic_Frame *f, const u8 *spr, s32 side,
                          s32 ox, s32 oy)
{
	s32 y, x, k;

	if (!spr) return;
	for (y = 0; y < side; y++) {
		for (x = 0; x < side; x++) {
			size_t si = (size_t)y * side + x;
			s32 py0, py1, px0, px1, py, px;
			py0 = FX_MUL(oy + y, f->scale);
			py1 = FX_MUL(oy + y + 1, f->scale);
			px0 = FX_MUL(ox + x, f->scale);
			px1 = FX_MUL(ox + x + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				if (py < 0) continue;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *q;
					if (px < 0) continue;
					q = f->px + ((size_t)(py * f->w) + (size_t)px) * PX_BYTES;
					/* add.l with no ceiling, as at 0x002cebfc - see
					 * sprite_add for what clamping to 0x3f costs. */
					for (k = 0; k < PX_BYTES; k++) {
						s32 c = q[k] + spr[si * PX_BYTES + k];
						q[k] = (u8)(c > 0xff ? 0xff : c);
					}
				}
			}
		}
	}
}

/* ORIGIN 0x002cf2aa Flare_Draw, with its four ghosts 0x002cf2fe,
 * 0x002cf39e, 0x002cf336 and 0x002cf36a.
 *
 * ONE NUMBER IS BOTH THE GATE AND THE BRIGHTNESS: the sun's distance
 * from (0x50,0x40) plus a range term gates at 3 and is also the right
 * shift each sprite is drawn with, so the flare fades out as the sun
 * leaves the middle of the frame instead of being switched off.
 *
 * The ghosts are the sun reflected through (0x3c,0x3c) at -2, -1/2,
 * -1 and +2. That pivot is NOT the screen centre, which is
 * (0x50,0x32), and neither is the gate's (0x50,0x40). All three
 * reference points are the demo's. */
void Space_DrawFlareGhosts(Relic_Frame *f, s32 fx, s32 fy)
{
	s32 dx = fx - 0x50, dy = fy - 0x40;
	s32 shift;

	/* THE DISTANCE TERM IS ALWAYS ZERO ON A 160 x 100 SCREEN and that
	 * is the original's arithmetic, not a slip here: the furthest two
	 * points are about 190 apart, Math_Isqrt returns a plain root, and
	 * the >>8 crushes anything under 256 to nothing. g_SunRange alone
	 * decides the gate. The sum is u32, as muls.l leaves it: signed, a
	 * wrapped sum roots to 0 and opens it - effects/planet_sun.txt 6. */
	shift = (s32)(Gfx_IsqrtU((u32)dx * (u32)dx + (u32)dy * (u32)dy) >> 8)
	      + (s32)((((u32)(g_sun_range >> 8)) ^ 0x7fUL) >> 5);

	if (shift >= 3) return;

	sprite_add(f, g_ghost32a, 32,
	           0x3c - 2 * (fx - 0x3c), 0x3c - 2 * (fy - 0x3c), shift);
	sprite_add(f, g_ghost32b, 32,
	           0x3c - ((fx - 0x3c) >> 1), 0x3c - ((fy - 0x3c) >> 1), shift);
	sprite_add(f, g_ghost16, 16,
	           0x3c - (fx - 0x3c), 0x3c - (fy - 0x3c), shift);
	sprite_add(f, g_ghost16, 16,
	           0x3c + 2 * (fx - 0x3c), 0x3c + 2 * (fy - 0x3c), shift);
}

/* OURS. A 16.16 demo coordinate in 16.16 frame pixels. In halves
 * because the straight multiply overflows a s32. */
s32 Space_ScaleFx(s32 a_fx, s32 scale)
{
	return (a_fx >> 16) * scale + (((a_fx & 0xffffL) * (scale >> 8)) >> 8);
}

/* ORIGIN 0x002d1a94 Sprite_StampSaveUnder and 0x002d1e08
 * Sprite_RestoreSaveUnder - HOW MUCH OF THE SUN IS STILL VISIBLE, and
 * the demo measures it THROUGH THE FRAMEBUFFER rather than
 * geometrically. The stamp writes a sentinel into the sun's cells,
 * everything drawn after it overwrites some of them, and the restore
 * counts the survivors and puts the saved pixels back.
 *
 * THAT IS WHY THE OBJECT CONTROLS THE FLARE. It is drawn between the
 * two, so its voxels destroy sentinels and the count falls, which
 * shrinks g_SunRange and with it the flare - the sun is barely dimmed
 * when the object clips its edge and nearly shut out when it sits
 * squarely in front. Testing the planet's mask instead cannot see any
 * other occluder at all.
 *
 * ONE PROBE PIXEL PER CELL. The demo's cell is one pixel; above scale
 * 1 ours is a block, and the sentinel goes in the block's top-left
 * only. Anything that covers the cell covers that pixel too, so the
 * test is the same and the stamp stays invisible. */
#define STAMP_CELLS (STAMP_SIDE * STAMP_SIDE)
static u8  g_stamp_save[STAMP_CELLS][PX_BYTES];
static s32 g_stamp_at[STAMP_CELLS];
static s32 g_stamp_cx[STAMP_CELLS], g_stamp_cy[STAMP_CELLS];
static s32 g_stamp_n;
static int g_stamped;
static s32 g_last_count, g_last_stamped;

/* ORIGIN 0x002d1a94 Sprite_StampSaveUnder */
void Space_StampSaveUnder(Relic_Frame *f, s32 fx, s32 fy)
{
	const u8 *mask = Image_At(A_STAMPMASK);
	s32 ox = fx - 6, oy = fy - 6, i, j;

	g_stamp_n = 0;
	g_stamped = 0;
	if (!mask || !f || !f->px) return;
	if (ox < 0 || ox >= SPACE_W - STAMP_SIDE) return;
	if (oy < 0 || oy >= SPACE_H - STAMP_SIDE) return;
	g_stamped = 1;
	for (i = 0; i < STAMP_SIDE; i++)
		for (j = 0; j < STAMP_SIDE; j++) {
			s32 cx = ox + j, cy = oy + i;
			s32 xx = FX_MUL(cx, f->scale), yy = FX_MUL(cy, f->scale);
			u8 *p;
			if (!mask[i * STAMP_SIDE + j]) continue;
			if (xx < 0 || xx >= f->w || yy < 0 || yy >= f->h) continue;
			p = f->px + ((size_t)(yy * f->w) + (size_t)xx) * PX_BYTES;
			memcpy(g_stamp_save[g_stamp_n], p, PX_BYTES);
			g_stamp_at[g_stamp_n] = yy * f->w + xx;
			g_stamp_cx[g_stamp_n] = cx;
			g_stamp_cy[g_stamp_n] = cy;
			p[PX_R] = 0;
			p[PX_G] = 0;
			p[PX_LIGHT] = 0x80;         /* outside the 0..0x40 range */
			p[PX_B] = 0;
			g_stamp_n++;
		}
}

/* ORIGIN 0x002d1e08 Sprite_RestoreSaveUnder */
s32 Space_CountSaveUnder(Relic_Frame *f, s32 *mx, s32 *my)
{
	s32 k, n = 0, sx = 0, sy = 0;

	if (!g_stamped || !f || !f->px) return -1;
	for (k = 0; k < g_stamp_n; k++) {
		u8 *p = f->px + (size_t)g_stamp_at[k] * PX_BYTES;
		/* RESTORE ONLY WHERE THE SENTINEL SURVIVED. Where it did not,
		 * something drew over the cell and that drawing must stand -
		 * putting the saved pixel back would erase the planet or the
		 * object. The demo's own restore confirms it: on a row where
		 * the object covered pixels 72 to 101 it wrote back only
		 * outside that run.
		 *
		 * THE TEST IS THE HIGH BIT, NOT THE WHOLE PIXEL: the halo
		 * drawn between the stamp and the count is ADDITIVE and
		 * shifts the other channels. Equality instead makes the
		 * bloom late and leaves a grid of black dots -
		 * approach.txt 4.4.9. */
		if (p[PX_LIGHT] & 0x80) {
			n++;
			sx += g_stamp_cx[k];
			sy += g_stamp_cy[k];
			memcpy(p, g_stamp_save[k], PX_BYTES);
		}
	}
	if (n > 0) { *mx = sx / n; *my = sy / n; }
	g_last_count = n;
	g_last_stamped = g_stamp_n;
	return n;
}

/* OURS. The raw cell counts, for a host that wants to see why the
 * flare is the size it is. */
void Space_CountStats(s32 *visible, s32 *stamped)
{
	if (visible) *visible = g_last_count;
	if (stamped) *stamped = g_last_stamped;
}

/* ORIGIN 0x002d1e08 - the count becomes a target and g_SunRange eases a
 * quarter of the way to it, or HALF the way to zero when nothing of the
 * sun is left. The steps are arithmetic shifts in the original, so a
 * negative difference rounds down, not toward zero as a divide would. */
int Space_EaseRange(s32 cells)
{
	s32 n, target;

	g_stamp_ok = (cells >= 0);
	if (cells < 0) return 1;            /* rejected: hold, but still draw */
	if (cells == 0) {
		g_band_target = 0;
		g_band_now = 0;
		g_sun_range += (0 - g_sun_range) >> 1;
		return 0;                       /* and no second flare */
	}
	n = cells * 2;
	if (n >= 0xb4) n = 0xb3;
	/* The band reads the count BEFORE the fold below [0x002d1ea0]. */
	g_band_target = n << 8;
	if (n >= 0x82) n = 0x82 - (n - 0x82);
	target = n << 8;
	g_sun_range += (target - g_sun_range) >> 2;
	return 1;
}

/* ORIGIN 0x002d3d34 - ONE CHANNEL, AND THE TEST IS SIGNED. add.b
 * then cmp.b #0x3f: the additive passes leave a channel past 0x7f,
 * which reads back NEGATIVE as a byte, so ble keeps it. An unsigned
 * clamp pulls exactly those pixels - the brightest in the frame -
 * down to 0x3f, which the resolve then halves. bvs catches the add
 * crossing 127 from below and clamps that. effects/orb.txt 11.2. */
static u8 band_add(s32 c, s32 amt)
{
	s32 sum = (c + amt) & 0xffL;
	s32 sc = (c & 0x80L) ? c - 0x100L : c;
	s32 ss = (sum & 0x80L) ? sum - 0x100L : sum;

	if (sc >= 0 && ss < 0) return 0x3f;      /* bvs */
	return (u8)(ss > 0x3fL ? 0x3fL : sum);
}

/* ORIGIN 0x002d2530 - a glare band ten rows tall, laid across the row
 * the VISIBLE sun sits on and brightest where the sun is. It is a
 * transient: the gate below only lets it through while the visible count
 * sits near 100, which is the sun about half out from behind the planet.
 *
 * Its blend is 0x002d3d34 - R, G and B each take the value CLAMPED AT
 * 0x3f, and the LIGHT byte loses twice as much, floored at zero, so the
 * band tints and un-darkens at once.
 *
 * The polar lookup's row runs -100 to +80 in twenties over the ten
 * screen rows, so the band samples a tall slice of the table through a
 * short one of the screen. */
void Space_DrawHorizonBand(Relic_Frame *f, s32 fx)
{
	s32 d5 = g_band_d5, top, k, x;

	if (!g_band_on) return;
	top = g_vis_mean_y - 7;

	for (k = 0; k < 10; k++) {
		s32 dy = -100 + 20 * k;
		s32 row = top + k;
		if (row < 0 || row >= SPACE_H) continue;
		for (x = 0; x < SPACE_W; x++) {
			s32 dx = (s32)(s8)(x - fx) >> 1;
			s32 v = ((s32)(Gfx_PolarDist(dx, dy) >> 1) ^ 0x3fL) + d5;
			s32 py0, py1, px0, px1, py, px;
			if (v < 0) continue;
			py0 = FX_MUL(row, f->scale);
			py1 = FX_MUL(row + 1, f->scale);
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;
				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;
					s32 l;
					if (px < 0) continue;
					p[PX_R] = band_add(p[PX_R], v);
					p[PX_G] = band_add(p[PX_G], v);
					p[PX_B] = band_add(p[PX_B], v);
					l = (v * 2L) & 0xffL;
					p[PX_LIGHT] = (u8)(p[PX_LIGHT] < l
					                   ? 0L : p[PX_LIGHT] - l);
				}
			}
		}
	}
}

/* ORIGIN 0x002ceaf8 Scene_DrawPlanetAndHalo - two passes, and they
 * blend differently: the planet is a MASKED OPAQUE copy (move.l over
 * the pixel) and the halo is ADDITIVE over all four channels, light
 * byte included. Both centre on the projected planet, the halo on 80
 * and the planet on 64 across but on 40 down either way - the
 * sprite's content is not vertically centred in its cell. The planet
 * alone sits NINE ROWS below its cell, from the 0x1680 added to the
 * framebuffer pointer partway through - 0x1680 is 9 rows of 160
 * longwords. */
void Space_DrawPlanetAndHalo(Relic_Frame *f, s32 bx, s32 by)
{
	blit_masked(f, g_planet, g_planet_mask, PLANET_SIDE,
	            bx - PLANET_SIDE / 2, by - 40 + 9);
	blit_additive(f, g_halo, HALO_SIDE, bx - HALO_SIDE / 2, by - 40);
}

/* ORIGIN 0x002d2530 maybe_Scene_DrawHorizonBand - its eased pair and
 * the gate that
 * decides whether there is a band at all. Separated from the drawing
 * so it can run once a tick while the picture is drawn once a frame -
 * see demo.c. mean_y is the mean row of the visible cells, or -1 when
 * nothing of the sun is left. */
static void band_step(s32 mean_y, int gate)
{
	g_vis_mean_y = mean_y;
	g_band_on = 0;
	if (gate && mean_y >= 0) {
		s32 d5;
		g_band_now += (g_band_target - g_band_now) >> 1;
		d5 = (g_band_now >> 8) - 0x64;
		if (d5 > 0) d5 = -d5;
		d5 = (d5 >> 2) - 0x1e;
		if (d5 > -0x32) {
			g_band_d5 = d5;
			g_band_on = 1;
		}
	}
}

/* ORIGIN 0x002d2530 - the sun's feed: the save-under decides it. */
void Space_StepBand(s32 mean_y)
{
	band_step(mean_y, g_stamp_ok);
}

/* ORIGIN 0x002d2c3a - THE ORB DRIVES THE SAME BAND IN 2.10 AND 2.11.
 * Orb_DrawCentreFlare writes the row and the target itself, where a
 * space scene's sun derives them from its visible count, so this is
 * the chain above with the orb's two numbers in front of it and no
 * save-under to gate on. docs/effects/orb.txt 11.2. */
void Space_StepOrbBand(s32 x, s32 row, s32 radius)
{
	g_orb_band_x = x;
	g_band_target = ((radius - 0x38L) * 2L) << 8;
	band_step(row, 1);
}

/* OURS - space.h. The band reads the x its flare left behind, which
 * is what 0x002e0b3e is for in the demo. */
void Space_DrawOrbBand(Relic_Frame *f)
{
	Space_DrawHorizonBand(f, g_orb_band_x);
}

/* OURS. The projection without its truncation - see
 * effects/planet_sun.txt section 10. Carrying only the remainder
 * within one world unit and keeping the divide looks safer and is not:
 * the step being removed happens BETWEEN world units, so that
 * reproduces the stall. In two parts because wx_fx * 256 overflows a
 * s32, and it needs / and % to truncate the same way on a negative
 * operand, which C89 leaves implementation-defined. */
void Space_ProjectFx(s32 wx_fx, s32 wy_fx, s32 *sx_fx, s32 *sy_fx)
{
	*sx_fx = ((wx_fx / FOCAL) * 256)
	       + (((wx_fx % FOCAL) * 256) / FOCAL) + ((SPACE_W / 2) << 16);
	*sy_fx = ((wy_fx / FOCAL) * 256)
	       + (((wy_fx % FOCAL) * 256) / FOCAL) + ((SPACE_H / 2) << 16);
}

/* OURS. g_SunRange is read by three of the drawing steps and written
 * by the save-under pair, so it is module state here as it is a
 * global there. */
s32 Space_Range(void)
{
	return g_sun_range;
}

/* ORIGIN 0x002cd422 - one longword store per pixel over both buffers,
 * 0xf9f + 1 passes of four. 0x3f3f003f is R, G, light, B with the
 * light byte zero, so it is the ramp builders' white. */
void Space_FillWhite(Relic_Frame *f)
{
	u8 *p = f->px;
	size_t n = (size_t)f->w * f->h;
	while (n--) {
		p[PX_R] = 0x3f; p[PX_G] = 0x3f; p[PX_LIGHT] = 0x00; p[PX_B] = 0x3f;
		p += PX_BYTES;
	}
}

/* ORIGIN 0x002f0f50 g_SparkList - ten entries of five longs,
 * [x, y, vx, vy, life], x and y in 12.4 fixed. */
static s32 g_sparks[SPARK_SEEDED][5];
static s32 g_spark_live;

/* ORIGIN 0x002cd44a - scattered about the object's last projected
 * position, each with an up-and-right velocity and its own life. The
 * ranges are the demo's: +-0xc8 of position from a 0x190 draw, vx
 * 4..0x1b, vy -(4..0x13), life (8..0x17) << 8. */
void Space_SeedSparks(s32 proj_x, s32 proj_y)
{
	int i;
	for (i = 0; i < SPARK_SEEDED; i++) {
		g_sparks[i][0] = proj_x + (Gfx_Rand(0x190) - 0xc8);
		g_sparks[i][1] = proj_y + (Gfx_Rand(0x190) - 0xc8);
		g_sparks[i][2] = Gfx_Rand(0x18) + 4;
		g_sparks[i][3] = -(Gfx_Rand(0x10) + 4);
		g_sparks[i][4] = (Gfx_Rand(0x10) + 8) << 8;
	}
	g_spark_live = SPARK_SEEDED;
}

/* ORIGIN 0x002cd5da - THE FLARE IS DRAWN AT THE PRE-STEP POSITION:
 * the position written back is x + vx, but the one passed to
 * Draw_Flare is the x the entry was loaded with. Radius is the life's
 * high byte plus a 0 to 3 jitter, so each spark shrinks as its life
 * runs down 0x80 a frame.
 *
 * ONLY SEVEN OF THE TEN ARE STEPPED - dbf runs 0x6 + 1 times over a
 * list the seeder fills ten of. The last three are seeded and never
 * touched. */
void Space_StepSparks(Relic_Frame *f)
{
	int i;
	g_spark_live = 0;
	for (i = 0; i < SPARK_STEP; i++) {
		s32 x = g_sparks[i][0], y = g_sparks[i][1];
		s32 vx = g_sparks[i][2], vy = g_sparks[i][3];
		s32 life = g_sparks[i][4], r;
		if (life <= 0) continue;
		g_spark_live++;
		g_sparks[i][4] = life - 0x80;
		g_sparks[i][0] = x + vx;
		g_sparks[i][1] = y + vy;
		r = (life >> 8) + Gfx_Rand(4);
		Gfx_Flare(f, (x >> 4) + 0x50, (y >> 4) + 0x32,
		          r, g_RayArcs, g_ColRampAmber);
	}
}

/* OURS */
s32 Space_SparkCount(void)
{
	return g_spark_live;
}

/* OURS. Where the live sparks are, in screen pixels, so a host can
 * check which way the shower actually flies. */
void Space_SparkMid(s32 *mx, s32 *my)
{
	s32 i, n = 0, sx = 0, sy = 0;
	for (i = 0; i < SPARK_STEP; i++) {
		if (g_sparks[i][4] <= 0) continue;
		sx += (g_sparks[i][0] >> 4) + 0x50;
		sy += (g_sparks[i][1] >> 4) + 0x32;
		n++;
	}
	*mx = n ? sx / n : -1;
	*my = n ? sy / n : -1;
}

/* ORIGIN 0x0021fdca Mem_AllocSubBlocks - g_WarpAllocCount and
 * g_WarpAllocSize read 1 and 0x11880 out of the image, and 0x11880 is
 * 71808 bytes = 17952 longwords = 176 x 102 exactly. It zeroes what it
 * allocates, so Sky_RenderToWarpBuffer is what puts a picture in it.
 * portal.txt 5.1. */
static u8 *g_ripple;

/* OURS - the Earth shot reads both to build its planet. */
const u8 *Space_SkyTexture(void) { return g_sky_tex; }
const s32 *Space_SkyDir(void)    { return g_sky_dir; }

/* ORIGIN 0x002d32dc Sky_RenderToWarpBuffer - THE SAME SAMPLER AS
 * Scene_DrawTexturedSky with
 * two values changed: 176 columns instead of 160, and the destination
 * is the ripple buffer instead of the framebuffer. Its inner loop is
 * instruction-for-instruction identical.
 *
 * The u span is +-0x5800 against the screen pass's +-0x5000 and both
 * divide by their own width, so BOTH STEPS COME OUT AT 0x100 and the
 * difference is exactly eight columns of sky each side. That is what
 * makes the ripple's +-8 pixel displacement always land on real sky.
 *
 * Only rows 0 to 99 are written - the row count is 0x64 - and the
 * buffer holds 102, so the last two rows stay as the allocator left
 * them. */
void Space_BakeSky(s32 pan)
{
	s32 py, px;

	if (!g_sky_tex || !g_sky_dir) return;
	if (!g_ripple) {
		g_ripple = (u8 *)malloc((size_t)RIPPLE_W * RIPPLE_H * PX_BYTES);
		if (!g_ripple) return;
	}
	memset(g_ripple, 0, (size_t)RIPPLE_W * RIPPLE_H * PX_BYTES);
	for (py = 0; py < 100; py++) {
		s32 v = SKY_V0 + py * SKY_STEP;
		s32 u = 0x5800L;
		u8 *q = g_ripple + (size_t)(py * RIPPLE_W) * PX_BYTES;
		for (px = 0; px < RIPPLE_W; px++, u -= SKY_STEP, q += PX_BYTES) {
			s32 w = (s32)(s16)((((v >> 8) & 0xffL) << 8)
			                  | ((u >> 8) & 0xffL));
			s32 e = 0x8080L + w;
			const u8 *t;
			if (e < 0 || e >= SKY_GRID * SKY_GRID) continue;
			t = g_sky_tex + ((g_sky_dir[e] + pan) & SKY_WRAP);
			q[PX_R]     = t[PX_R];
			q[PX_G]     = t[PX_G];
			q[PX_LIGHT] = t[PX_LIGHT];
			q[PX_B]     = t[PX_B];
		}
	}
}

/* ORIGIN 0x002c9868 Space_BakePanorama - the same buffer Space_BakeSky
 * fills, from the departure's panorama instead of the sky: 100 rows of
 * RIPPLE_W from (scroll - 8), the source stepping a whole 640-pixel
 * row. The eight columns of slack each side are what the ripple's
 * displacement reads at the edges.
 *
 * THE DEMO WRITES FROM OFFSET ZERO where Space_BakeSky writes from
 * RIPPLE_LIVE, and hands the ripple g_pWarpBuffer + 0x20 rather than
 * + 0x2e0 - eight longs with no row. Writing at RIPPLE_LIVE here
 * keeps one accessor for both.
 *
 * THE WALK IS LINEAR AND THAT IS LOAD-BEARING. The source index is
 * not clamped per row: by the end of the flight the scroll is past
 * 900 on a 640-wide panorama, so a row's 176 pixels run off the right
 * and CONTINUE INTO THE NEXT ROW. That is where the ripple's stars
 * and its horizontal seams come from, and clamping each row instead
 * bakes a smear of one column. effects/return.txt 4.2.1. */
void Space_BakePanorama(const u8 *pan, s32 pan_w, s32 scroll)
{
	s32 py, px, at = (scroll - 8) * PX_BYTES;
	s32 end = pan_w * SPACE_H * PX_BYTES;

	if (!pan) return;
	if (!g_ripple) {
		g_ripple = (u8 *)malloc((size_t)RIPPLE_W * RIPPLE_H * PX_BYTES);
		if (!g_ripple) return;
	}
	memset(g_ripple, 0, (size_t)RIPPLE_W * RIPPLE_H * PX_BYTES);
	for (py = 0; py < SPACE_H; py++) {
		u8 *q = g_ripple + RIPPLE_LIVE
		      + (size_t)(py * RIPPLE_W) * PX_BYTES;

		for (px = 0; px < RIPPLE_W; px++, q += PX_BYTES,
		                                 at += PX_BYTES) {
			int k;

			/* A DELIBERATE DIVERGENCE at the very end, where the
			 * demo reads past its own allocation into the pool. */
			if (at < 0 || at + PX_BYTES > end) continue;
			for (k = 0; k < PX_BYTES; k++) q[k] = pan[at + k];
		}
		at += (pan_w - RIPPLE_W) * PX_BYTES;
	}
}

/* ORIGIN 0x002cd71c Explosion_DrawFlash - one flare at the screen
 * centre whose radius is a V in g_ApproachRange:
 *
 *     t = (range >> 4) - 0xc8
 *     t = t < 0 ? -t : t * 2
 *     r = 0x80 - t - 0xa,  and nothing is drawn at r <= 0
 *
 * The two sides have different slopes - doubled above the peak,
 * single below - so it rushes in and ebbs away slowly. Both the
 * portal's approach and the departure's ending draw it.
 * effects/portal.txt, effects/return.txt 4.2. */
s32 Space_FlashRadius(s32 range)
{
	s32 t = (range >> 4) - 0xc8L;

	t = (t < 0) ? -t : t * 2;
	return 0x80L - t - 0xaL;
}

/* ORIGIN 0x002cd748 - the flare the radius above feeds. */
void Space_DrawExplosionFlash(Relic_Frame *f, s32 range)
{
	s32 r = Space_FlashRadius(range);

	if (r > 0L)
		Gfx_Flare(f, 0x56L, 0x34L, r, g_RayStreak, g_ColRampAmber);
}

/* ORIGIN 0x002d2c52 and 0x002d2c74 - space.h. */
void Space_DrawOrbGhosts(Relic_Frame *f, s32 fx, s32 fy)
{
	g_sun_range = SPACE_ORB_RANGE;
	Space_DrawFlareGhosts(f, fx, fy);
}

/* OURS. The live region's top-left, which is what +0x2e0 names. */
const u8 *Space_RippleSource(s32 *stride)
{
	if (stride) *stride = RIPPLE_W;
	if (!g_ripple) return 0;
	return g_ripple + RIPPLE_LIVE;
}

/* ORIGIN 0x002cd760 Warp_FullscreenRipple - the baked sky resampled
 * into the framebuffer, one longword a pixel.
 *
 * WHAT RIPPLES IS THE LIGHT CHANNEL. Byte 2 is replaced outright and
 * R, G, B come from a HORIZONTALLY displaced sample, so the wavefront
 * is a light index resolving toward the scene's purple base. The copy
 * branch is NOT the zero-displacement case - it is eight pixels left
 * of it. The two -0x20 biases are the demo's. portal.txt 5.2. */
void Space_Ripple(Relic_Frame *f, s32 phase, s32 range)
{
	s32 stride, py, px;
	const u8 *src = Space_RippleSource(&stride);

	if (!src) { Frame_Clear(f); return; }
	for (py = 0; py < f->h; py++) {
		s32 drow = FX_DIV(py, f->scale);
		u8 *q = f->px + (size_t)(py * f->w) * PX_BYTES;
		if (drow < 0 || drow >= SPACE_H) continue;
		for (px = 0; px < f->w; px++, q += PX_BYTES) {
			s32 dcol = FX_DIV(px, f->scale);
			s32 dist, d4, t, c, v, d5, w, sc;
			const u8 *p;
			if (dcol < 0 || dcol >= SPACE_W) continue;
			dist = Gfx_PolarDist((s32)(s8)(dcol - 0x56), drow - 0x34);
			d4 = dist - (range >> 3);
			t = (dist << 7) + phase;
			if (t >= 0) {
				sc = dcol - 8;
				w = -1;
			} else {
				c = (s32)Image_U32(A_COS_TABLE - 0x20
				                   + (u32)((t & 0x1fffL) * 4));
				v = ((c * range) >> 8) >> 2;
				v = (v + 0x10000L) >> 10;
				d5 = v >> 3;
				w = v + 0x1f4L + d4;
				if (w >= 0xffL) w = 0xffL;
				w = ((w ^ 0xffL) & 0xffL) >> 2;
				sc = dcol - 8 + d5;
			}
			/* sc IS MEANT TO GO NEGATIVE - the source is biased to
			 * column 8, so -8 is the buffer's first column and 167 its
			 * last. Clamping at zero throws away the left slack the
			 * allocation exists to provide. */
			if (sc < -8) sc = -8;
			if (sc > RIPPLE_W - 9) sc = RIPPLE_W - 9;
			p = src + (size_t)(drow * stride + sc) * PX_BYTES;
			q[PX_R]     = p[PX_R];
			q[PX_G]     = p[PX_G];
			q[PX_B]     = p[PX_B];
			q[PX_LIGHT] = (u8)(w < 0 ? p[PX_LIGHT] : w);
		}
	}
}

/* ORIGIN 0x002d671c, the ext.l pair - a word, sign-extended. */
static s32 ext16(s32 v)
{
	return (s32)(s16)(u16)(v & 0xffffL);
}

/* ORIGIN 0x002d671c maybe_Camera_MoveRotated. */
void Space_CamMove(Relic_Cam *c, s32 d0, s32 d1)
{
	s32 p, co, sn, h, a, b;

	c->yaw += d0;
	c->pitch += d1;
	d1 = -d1;

	p = (-c->pitch) & 0x1fffL;
	c->head += (d0 * Gfx_Sin(p)) >> 6;
	d0 = (d0 * Gfx_Cos(p)) >> 14;

	h = (c->head >> 8) & 0x1fffL;
	co = Gfx_Cos(h);
	sn = Gfx_Sin(h);
	a = ((d0 * co) - (d1 * sn)) >> 1;
	b = ((d0 * sn) + (d1 * co)) >> 1;
	c->angle += a;
	c->angle2 -= b;
}

/* ORIGIN 0x002d57bc Camera_TrackTarget.
 *
 *     D0 = camX + ox ; D1 = camY ; D2 = camZ + oz
 *     maybe_Camera_Rotate2Axis       the LIVE angles, not the latched
 *     D0 <<= 8 ; D2 <<= 8 ; divide both by D1 when it is non-zero
 *     yaw delta = -(D0 >> 1),  pitch delta = D2 >> 1
 *
 * IT STEERS BY HALF THE ERROR, which is why the swing eases in rather
 * than snapping: each tick removes half of what is left. The divide is
 * SKIPPED when the depth is zero rather than guarded, so the
 * unprojected offsets are used as-is on that tick. */
void Space_CamTrack(Relic_Cam *c, const s32 *cam, s32 ox, s32 oz)
{
	s32 d0 = cam[0] + ox;
	s32 d1 = cam[1];
	s32 d2 = cam[2] + oz;
	s32 co, sn, t;

	co = Gfx_Cos(c->yaw & 0x1fffL);
	sn = Gfx_Sin(c->yaw & 0x1fffL);
	t = d0;
	d0 = ((t * co) + (d1 * sn)) >> 14;
	d1 = ((d1 * co) - (t * sn)) >> 14;
	co = Gfx_Cos(c->pitch & 0x1fffL);
	sn = Gfx_Sin(c->pitch & 0x1fffL);
	t = d1;
	d1 = ((t * co) + (d2 * sn)) >> 14;
	d2 = ((d2 * co) - (t * sn)) >> 14;

	d0 <<= 8;
	d2 <<= 8;
	if (d1) { d0 /= d1; d2 /= d1; }
	/* ORIGIN 0x002d57f2 - NEGATE, THEN HALVE. asr floors, so (-d0) >> 1
	 * and -(d0 >> 1) differ by one for every odd d0, and Space_CamMove
	 * ACCUMULATES, so it drifts rather than cancelling.
	 * effects/orb_underside.txt 3. */
	d0 = -d0;
	Space_CamMove(c, ext16(d0 >> 1), ext16(d2 >> 1));
}

/* ORIGIN 0x002d5772 Camera_AimAhead - Camera_TrackTarget again, with
 * the target pushed `oz` ahead on Z and ONE ASYMMETRY: a positive yaw
 * step is clamped to 3 while a negative one passes through whole. So
 * the camera whips one way and creeps back the other, which is what
 * swings the departure shot round behind the ship. */
void Space_CamAim(Relic_Cam *c, const s32 *cam, s32 oz)
{
	s32 d0 = cam[0];
	s32 d1 = cam[1];
	s32 d2 = cam[2] + oz;
	s32 co, sn, t;

	co = Gfx_Cos(c->yaw & 0x1fffL);
	sn = Gfx_Sin(c->yaw & 0x1fffL);
	t = d0;
	d0 = ((t * co) + (d1 * sn)) >> 14;
	d1 = ((d1 * co) - (t * sn)) >> 14;
	co = Gfx_Cos(c->pitch & 0x1fffL);
	sn = Gfx_Sin(c->pitch & 0x1fffL);
	t = d1;
	d1 = ((t * co) + (d2 * sn)) >> 14;
	d2 = ((d2 * co) - (t * sn)) >> 14;

	d0 <<= 8;
	d2 <<= 8;
	if (d1) { d0 /= d1; d2 /= d1; }
	d0 = (-d0) >> 1;
	if (d0 > 0) d0 = 3;
	Space_CamMove(c, ext16(d0), ext16(d2 >> 1));
}

/* ORIGIN 0x002d2db2 Scene_DrawTexturedSky - angle slides the field
 * inside a row, angle2 moves it in whole rows. portal.txt 5.6. */
s32 Space_SkyPan(s32 angle, s32 angle2)
{
	s32 lo = ((angle * 2) >> 16) & 0xffL;
	s32 hi = (angle2 * 2) >> 8;
	return (((hi & ~0xffL) | lo) * 4);
}

static s32 g_glow_px;

#define PSPR_SRC   0x002ed810UL
#define PSPR_W     0x23L
#define PSPR_H     0x1dL
#define PMASK_W    0x40L
#define PMASK_ROWS 9L

/* ORIGIN 0x002c8944 Space_BlitPortalSprite - a 35 by 29 baked
 * sprite from g_PortalSprite, A ZERO LONGWORD MEANING LEAVE WHAT IS
 * THERE. The caller passes a BYTE offset into the frame, 0x7d50 for
 * both of this shot's phases, which is column 20 of row 50.
 *
 * This is the vessel, drawn flat - the shot does not project it.
 * effects/return.txt 2.3. */
void Space_BlitPortalSprite(Relic_Frame *f, s32 byte_off)
{
	const u8 *src = Image_At(PSPR_SRC);
	s32 ox = (byte_off / PX_BYTES) % SPACE_W;
	s32 oy = (byte_off / PX_BYTES) / SPACE_W;
	s32 x, y;

	if (!src) return;
	for (y = 0; y < PSPR_H; y++) {
		s32 dy = oy + y;
		s32 py0, py1, py;

		if (dy < 0 || dy >= SPACE_H) continue;
		py0 = FX_MUL(dy, f->scale);
		py1 = FX_MUL(dy + 1, f->scale);
		for (x = 0; x < PSPR_W; x++) {
			const u8 *t = src + (size_t)(y * PSPR_W + x) * PX_BYTES;
			s32 dx = ox + x;
			s32 px, px0, px1;

			if (dx < 0 || dx >= SPACE_W) continue;
			if (!t[0] && !t[1] && !t[2] && !t[3]) continue;
			px0 = FX_MUL(dx, f->scale);
			px1 = FX_MUL(dx + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;
					int k;

					if (px < 0) continue;
					for (k = 0; k < PX_BYTES; k++) p[k] = t[k];
				}
			}
		}
	}
}

/* ORIGIN 0x002c97b8 Space_DrawPlanetMasked - g_pPlanetMask stamped
 * at the projected planet, 64 by 64, NINE ROWS DOWN from it
 * [0x002c982a]. Where the mask byte is set it WRITES LIGHT outright
 * - it does not add - to half the portal counter.
 *
 * With the swirl's tables at 0, 0, 0 a high LIGHT resolves black, so
 * this is what swallows the planet as the portal opens.
 * effects/return.txt 2.3. */
void Space_DrawPlanetMasked(Relic_Frame *f, s32 open)
{
	const u8 *mask = Space_PlanetMask();
	s32 sx, sy, x, y;
	u8 v = (u8)(open >> 1);

	if (!mask) return;
	if (!Space_Project(g_planet_wx, g_planet_wz, &sx, &sy)) return;
	sx -= 0x20L;
	sy -= 0x28L;
	sy += PMASK_ROWS;

	for (y = 0; y < PMASK_W; y++) {
		s32 dy = sy + y;
		s32 py0, py1, py;

		if (dy < 0 || dy >= SPACE_H) continue;
		py0 = FX_MUL(dy, f->scale);
		py1 = FX_MUL(dy + 1, f->scale);
		for (x = 0; x < PMASK_W; x++) {
			s32 dx = sx + x;
			s32 px, px0, px1;

			if (dx < 0 || dx >= SPACE_W) continue;
			if (!mask[y * PMASK_W + x]) continue;
			px0 = FX_MUL(dx, f->scale);
			px1 = FX_MUL(dx + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;

				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					if (px < 0) continue;
					q[(size_t)px * PX_BYTES + PX_LIGHT] = v;
				}
			}
		}
	}
}

/* ORIGIN 0x002cd642 maybe_Portal_DrawGlow - a polar interference
 * field added over the picture, then one flare. Two parts draw it:
 * 2.3's planet shot and the return flight's departure.
 *
 * THE FIELD IS A PRODUCT OF TWO SINES and both terms are dropped when
 * negative, so only where both are positive inks at all - that is
 * what makes it a pattern rather than a ramp. The second index folds
 * the distance in with the angle, so the interference runs radially
 * as well as around. */
void Space_PortalGlow(Relic_Frame *f, s32 phase, s32 off)
{
	s32 x, y, a5;

	if (!phase) return;
	g_glow_px = 0;
	a5 = (phase * 2) - 0x100;
	if (a5 < 0) a5 = -a5;

	for (y = 0; y < SPACE_H; y++) {
		for (x = 0; x < SPACE_W; x++) {
			s32 bx = (s32)(s8)(x - 0x56 + off);
			s32 ang = Gfx_PolarAngle(bx, y - 52);
			s32 dist = Gfx_PolarDist(bx, y - 52);
			s32 i0 = ((ang * 2) << 8) & 0x1fffL;
			s32 i1 = ((dist + ang * 2 + phase) << 7) & 0x1fffL;
			s32 a = Gfx_Sin(i0), b = Gfx_Sin(i1);
			s32 v, py0, py1, px0, px1, py, px;
			const u8 *c;

			if (a < 0 || b < 0) continue;
			v = ((a * b) >> 20) - (dist + a5);
			if (v < 0) continue;
			g_glow_px++;
			/* THE INDEX IS A WORD AND ONLY THE LOW BYTE IS INVERTED
			 * [0x002cd6d6, 0x002cd6da], so the one case where the
			 * product saturates reads entry 511 of a 256-entry ramp.
			 * Masking it to a byte would be tidier and would not be
			 * what the demo fetches. */
			v = (v & 0xff00L) | ((v ^ 0xffL) & 0xffL);
			c = g_ColRampGlow + v * PX_BYTES;

			py0 = FX_MUL(y, f->scale);
			py1 = FX_MUL(y + 1, f->scale);
			px0 = FX_MUL(x, f->scale);
			px1 = FX_MUL(x + 1, f->scale);
			for (py = py0; py < py1 && py < f->h; py++) {
				u8 *q;
				if (py < 0) continue;
				q = f->px + (size_t)(py * f->w) * PX_BYTES;
				for (px = px0; px < px1 && px < f->w; px++) {
					u8 *p = q + (size_t)px * PX_BYTES;
					int k;
					if (px < 0) continue;
					/* one add.l in the original, no ceiling of its own */
					for (k = 0; k < PX_BYTES; k++) {
						s32 s = p[k] + c[k];
						p[k] = (u8)(s > 255 ? 255 : s);
					}
				}
			}
		}
	}

	/* ORIGIN 0x002cd6f2 - D1 is Draw_Flare's RADIUS, D3 its row; swap
	 * them and the two halves drift apart. approach.txt 4.1. */
	Gfx_Flare(f, 0x56 - off, 0x34, phase >> 1, g_RayArcs, g_ColRampMagenta);
}

/* OURS - how many cells the last call inked, for a part's stats. */
s32 Space_PortalGlowPx(void) { return g_glow_px; }

/* OURS - the halving each tap is folded in by. */
static u32 space_mean(u32 a, u32 b)
{
	return ((a & 0xfefefefeUL) >> 1) + ((b & 0xfefefefeUL) >> 1)
	     + (a & b & 0x01010101UL);
}

static u8 *g_blur_prev;
static s32 g_blur_n;

/* OURS - the history the temporal half of the blur reads. IT IS SIZED
 * FROM THE FRAME, not from SPACE_W * SPACE_H: the host's window is
 * resizable and a scaled frame is many times the demo's 160 by 100,
 * so a buffer cut to the demo's size is walked off the end on the
 * first blurred row. */
static int blur_fit(s32 n)
{
	if (g_blur_prev && g_blur_n >= n) return 0;
	free(g_blur_prev);
	g_blur_prev = (u8 *)calloc((size_t)n, PX_BYTES);
	g_blur_n = g_blur_prev ? n : 0;
	return g_blur_prev ? 0 : 1;
}

/* OURS - drop the history; blur_fit sizes it on the next frame. */
int Space_BlurInit(void)
{
	free(g_blur_prev);
	g_blur_prev = 0;
	g_blur_n = 0;
	return 0;
}

/* ORIGIN 0x002ce992 Space_BlurFrame - five spatial taps AND the
 * destination's PREVIOUS contents, each folded in by halves:
 *
 *   ((((up+down)/2 + (left+right)/2)/2 + centre)/2 + previous)/2
 *
 * so a space scene carries a temporal smear, not just a blur. It is
 * the same routine Valley_BlurFrame 0x0031636c is for the stills
 * module, duplicated rather than shared the way this corpus does.
 *
 * THE PRESENT CALLS IT, NOT THE SCENE. Space_PresentFrame 0x002d5520
 * runs it before Ham8_EncodeFrame on EVERY frame of EVERY space
 * scene, which is why no scene's own call list mentions it and why
 * it is easy to leave out of a transcription entirely.
 * destruction.txt 2. */
void Space_BlurFrame(Relic_Frame *f)
{
	s32 n = f->w * f->h, i, w = f->w;
	u32 left, cur;

	if (blur_fit(n)) return;
	left = RD_U32(f->px);
	cur = left;
	for (i = 0; i < n; i++) {
		u8 *p = f->px + (size_t)i * PX_BYTES;
		u8 *d = g_blur_prev + (size_t)i * PX_BYTES;
		u32 up = (i >= w) ? RD_U32(p - (size_t)w * PX_BYTES) : cur;
		u32 dn = (i < n - w) ? RD_U32(p + (size_t)w * PX_BYTES) : cur;
		u32 right = (i < n - 1L) ? RD_U32(p + PX_BYTES) : cur;
		u32 v = space_mean(space_mean(space_mean(up, dn),
		                              space_mean(left, right)), cur);

		v = space_mean(v, RD_U32(d));
		d[0] = (u8)(v >> 24); d[1] = (u8)(v >> 16);
		d[2] = (u8)(v >> 8);  d[3] = (u8)v;
		left = cur;
		cur = right;
	}
	memcpy(f->px, g_blur_prev, (size_t)n * PX_BYTES);
}
