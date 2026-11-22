/* RELIC part 2.18 - the city from above, its three shots. See
 * docs/effects/city.txt.
 *
 * THE DEMO DOES NOT SEPARATE THESE FROM THE VALLEY. They are
 * Valley_RunScene's fourth, fifth and sixth loops, reached with new
 * assets swapped into the same four slots, so every routine they
 * call lives in src/stills.c alongside the valley's. This file holds
 * only the three shots' script.
 *
 * THAT MAKES THE PART BOUNDARY OURS. The demo flows out of the
 * valley's impact without re-entering, so the counters this file
 * stages are ones the valley's loops would have left behind. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "relic.h"
#include "image.h"
#include "gfx.h"
#include "dat.h"
#include "stills.h"
#include "parts.h"

/* The music cue the scene ends on - armed at 0x00314828, polled at
 * 0x00314858. parts.h Relic_Gate. */
#define CY_CUE_ORDER 0x26L
#define CY_CUE_ROW   0x2eL
#define CY_TICKS (CY_S1_FRAMES + CY_S2_FRAMES + CY_S3_FRAMES)
static Relic_Gate g_cue_gate;

/* AND THE SHOTS ARE NOT THE WHOLE PART. THE EXIT BLANKS THE SCREEN -
 * 0x00314866 installs a copper list that fetches no bitplane - and
 * the next scene holds it there for 60 fields while it waits on
 * (0x26,0x34), armed at 0x002c8448 and polled at 0x002c844e.
 * city.txt 5.2. */
#define CY_HOLD_ORDER 0x26L
#define CY_HOLD_ROW   0x34L
#define CY_HOLD       60L
static Relic_Gate g_hold_gate;

/* OURS - the blank above, while it is still running. */
static int cy_holding(void)
{
	return g_cue_gate.at != 0L && g_hold_gate.at == 0L;
}

/* ORIGIN 0x00316714 - Farmdata, split FOUR ways by Dat_LoadSplit
 * and the parts are NOT the same size. Their live addresses give
 * the four lengths, and each matches its role exactly:
 *
 *   +0x00000  0x63a0   0x400 palette + 24480 indices, the city
 *   +0x063a0  0x61a0   0x200 palette + 24480 indices, scorched
 *   +0x0c540  0x0bf4   3060 = 20 bytes x 153 rows, the 1BPP MASK
 *   +0x0d134  0x5fa0   24480 = 160 x 153, the height map
 *
 * city.txt 3. */
#define CY_P1           0x00000L
#define CY_P2           0x063a0L
#define CY_P3           0x0c540L
#define CY_P4           0x0d134L
#define CY_FARM_LEN     0x130d4L
#define CY_PIC_TEXELS   0x5fa0L         /* 24480 = 160 x 153        */
#define CY_PIC_ROWS     153L
#define CY_PAL_MAIN     0x100L
#define CY_PAL_LAYER    0x80L

/* ORIGIN 0x003146a8 - shot two's stills are BAKED, not in the
 * archive, and frame sized rather than 153 tall. */
#define CY_S2_DEEP      0x00326194UL
#define CY_S2_PAL       0x00331d60UL
#define CY_S2_MAP       0x00335fe0UL
/* ORIGIN 0x003146cc - the mask follows the map, 0x3e80 on. */
#define CY_S2_MASK      0x00339e60UL
#define CY_S2_TEXELS    0x3e80L         /* 16000 = 160 x 100        */

/* ORIGIN 0x0031460c, 0x003146e0, 0x003147fa - each shot's threshold
 * and the rate 0x00316476 takes off it a field. */
#define CY_S1_THRESH    0x320L
#define CY_S1_RATE      3L
#define CY_S2_THRESH    0x2a0L
#define CY_S2_RATE      4L
#define CY_S3_THRESH    0x300L
#define CY_S3_RATE      3L

/* ORIGIN 0x00314668 and 0x00314724 - the loops' own field counts.
 * THE SHOTS ARE LONGER THAN THEIR LOOPS: between them the demo
 * decodes the next picture and draws nothing - one field after shot
 * one and nine after shot two, three of which carry the white. Shot
 * three ends on the cue and measures 212 - city.txt 5.1. */
#define CY_S1_LOOP      0xe6L
#define CY_S2_LOOP      0x96L
#define CY_S1_FRAMES    (CY_S1_LOOP + 1L)
#define CY_S2_FRAMES    (CY_S2_LOOP + 9L)
#define CY_S3_FRAMES    212L
#define CY_FLASH        3L

/* ORIGIN 0x003147b0 and 0x00314680 - the shots burn in different
 * colours from two palettes. */
#define CY_PAL_S1       0x0033aa34UL
#define CY_PAL_S3       0x0033a634UL

/* ORIGIN 0x00316444 - the pan's cap is the picture's slack: 0x3500
 * is 53 rows and the still is 53 taller than the frame. */
#define CY_SCROLL_MAX   0x3500L
#define CY_VEL_MAX      0x190L
#define CY_VEL_ENTRY    (-0x64L)
#define CY_VEL_SHOT3    0x32L

static u8 *g_pic[3];                /* 0x00316738 per shot          */
static u8 *g_layer[3];              /* 0x0031673c                   */
static const u8 *g_map[3];          /* 0x00316740                   */
static const u8 *g_mask[3];         /* Farmdata part 3, 1bpp        */
static s32 g_shot;
static s32 g_frame;
static s32 g_scroll;                /* 0x0033b03c                   */
static s32 g_vel;                   /* 0x0033b040                   */
static int g_ready;

static const s32 CY_THRESH[3] = { CY_S1_THRESH, CY_S2_THRESH, CY_S3_THRESH };
static const s32 CY_RATE[3]   = { CY_S1_RATE,   CY_S2_RATE,   CY_S3_RATE   };
static const s32 CY_FRAMES[3] = { CY_S1_FRAMES, CY_S2_FRAMES, CY_S3_FRAMES };
static const s32 CY_LOOP[3]   = { CY_S1_LOOP,   CY_S2_LOOP,   CY_S3_FRAMES };

/* ORIGIN 0x0031468e - EVERY SHOT DECODES INTO THE SAME TWO BUFFERS
 * (0x0033b0ca and 0x0033b0c6), and shot two's stills are frame
 * sized where the others are 153 rows, so rows 100 to 152 still
 * hold shot one's when shot two runs. The pan reads them, so they
 * are part of the picture rather than slack. city.txt 1. */
static u8 *cy_shot2_buffer(u8 *frame_sized, const u8 *shot1)
{
	size_t head = (size_t)CY_S2_TEXELS * PX_BYTES;
	u8 *out;

	if (!frame_sized || !shot1) { free(frame_sized); return 0; }
	out = (u8 *)malloc((size_t)CY_PIC_TEXELS * PX_BYTES);
	if (out) {
		memcpy(out, frame_sized, head);
		memcpy(out + head, shot1 + head,
		       (size_t)CY_PIC_TEXELS * PX_BYTES - head);
	}
	free(frame_sized);
	return out;
}

/* ORIGIN 0x00314588 and 0x00314756 - shots one and three decode the
 * SAME two Farmdata parts; shot three then mirrors all three of its
 * assets in place, which is why one photograph serves both. */
static int cy_load(void)
{
	const u8 *farm;
	u32 len;

	farm = Dat_Chunk("Farmdata", &len);
	if (!farm || len < (u32)CY_FARM_LEN) return 1;

	g_pic[0] = Stills_LoadPalettised(farm + CY_P1, CY_PAL_MAIN,
	                                 CY_PIC_TEXELS);
	g_layer[0] = Stills_LoadPalettised(farm + CY_P2, CY_PAL_LAYER,
	                                   CY_PIC_TEXELS);
	g_map[0] = farm + CY_P4;
	g_mask[0] = farm + CY_P3;

	g_pic[1] = cy_shot2_buffer(Stills_LoadDeep(CY_S2_DEEP, CY_S2_TEXELS),
	                           g_pic[0]);
	g_layer[1] = cy_shot2_buffer(
	        Stills_LoadPalettised(Image_At(CY_S2_PAL), CY_PAL_MAIN,
	                              CY_S2_TEXELS), g_layer[0]);
	g_map[1] = Image_At(CY_S2_MAP);
	g_mask[1] = Image_At(CY_S2_MASK);

	g_pic[2] = Stills_LoadPalettised(farm + CY_P1, CY_PAL_MAIN,
	                                 CY_PIC_TEXELS);
	g_layer[2] = Stills_LoadPalettised(farm + CY_P2, CY_PAL_LAYER,
	                                   CY_PIC_TEXELS);
	if (!g_pic[0] || !g_layer[0] || !g_pic[1] || !g_layer[1]
	    || !g_pic[2] || !g_layer[2]) return 1;
	{
		u8 *m = (u8 *)malloc((size_t)CY_PIC_TEXELS);

		if (!m) return 1;
		memcpy(m, g_map[0], (size_t)CY_PIC_TEXELS);
		Stills_MirrorPixels(g_pic[2], CY_PIC_ROWS);
		Stills_MirrorPixels(g_layer[2], CY_PIC_ROWS);
		Stills_MirrorBytes(m, CY_PIC_ROWS);
		g_map[2] = m;
		g_mask[2] = farm + CY_P3;
	}
	return 0;
}

/* ORIGIN 0x0031460c, 0x003146e0, 0x003147fa - entering a shot. */
static void cy_enter(s32 n)
{
	g_shot = n;
	g_frame = 0;
	Stills_SetThreshold(CY_THRESH[n]);
	Stills_SetSources(g_pic[n], g_layer[n], g_map[n]);
	Stills_BuildSplatTable(n == 2L ? CY_PAL_S3 : CY_PAL_S1);
	Stills_BuildVelocities(n);
	Stills_ResetParticles();
	/* ORIGIN 0x00314616 - the cut tracker enters at 0xff, so the
	 * first frame's spawn band is the whole 55 the threshold is
	 * already under it. */
	Stills_SetCut(0xffL);
	/* ORIGIN 0x00314636 - shot one reseeds the haze's drift; shots
	 * two and three let it run on. */
	if (n == 0L) {
		/* ORIGIN 0x00314636 and 0x003145fa - the city reseeds the
		 * haze's drift and, unlike the valley, lets it run. */
		Stills_SetHazeOffset(0x15eL);
		Stills_SetHazeDrift(1L);
	}
	/* ORIGIN 0x00314602, 0x003147e8 - shot 3 opens MOVING. 4.2. */
	g_scroll = 0;
	g_vel = (n == 0L) ? CY_VEL_ENTRY
	      : (n == 2L) ? CY_VEL_SHOT3 : 0L;
	Stills_SetScroll(0L);
}

/* OURS */
static int cy_init(void)
{
	if (cy_load()) return 1;
	if (Stills_BlurInit(ST_TEXELS * 16L)) return 1;
	Stills_SeedRand(0L);
	Stills_BuildBlobRamp();
	Stills_BuildTrailRamp();
	Stills_BuildTurbulence();
	Stills_BuildBlastRamp();
	Stills_BuildRays();
	if (Stills_BuildPlasma()) return 1;
	if (Stills_ParticlesInit()) return 1;
	g_ready = 1;
	cy_enter(0L);
	return 0;
}

/* ORIGIN 0x00316444 and 0x00316476 - the pan accelerates and the
 * threshold falls, both once a field. */
static void cy_step(s32 tick)
{
	Parts_GateStep(&g_cue_gate, tick, CY_CUE_ORDER, CY_CUE_ROW,
	               CY_TICKS);
	if (g_cue_gate.at) {
		Parts_GateStep(&g_hold_gate, tick - g_cue_gate.at,
		               CY_HOLD_ORDER, CY_HOLD_ROW, CY_HOLD);
		return;
	}
	if (tick <= 1) return;
	if (!g_ready) return;
	if (g_scroll < CY_SCROLL_MAX) {
		/* ORIGIN 0x00316458 - a NEGATIVE velocity does not move the
		 * pan, it only climbs, so the shot holds still for its
		 * first 49 fields and then eases away. */
		if ((g_vel >> 2) >= 0) g_scroll += g_vel >> 2;
		if (g_vel < CY_VEL_MAX) g_vel += 2L;
	}
	Stills_SetScroll((g_shot == 1L) ? 0L : (g_scroll >> 8));
	Stills_StepThreshold(CY_RATE[g_shot]);
	Stills_StepCursors();
	Stills_StepSpin(0xaL);
	/* ORIGIN 0x00237218 - a FIELD count. valley.txt 2.11. */
	if (++g_frame >= CY_FRAMES[g_shot] && g_shot < 2L)
		cy_enter(g_shot + 1L);
}

/* OURS - the loop has exited and the next shot is still decoding, so
 * the frame stands as the loop left it. */
static int cy_transition(void)
{
	return g_shot < 2L && g_frame >= CY_LOOP[g_shot];
}

/* ORIGIN 0x00314736 - the white that cuts shots two and three apart,
 * all of the frame but its last row. */
static void cy_flash(Relic_Frame *f)
{
	s32 n = f->w * (f->h - 1L);
	u8 *p = f->px;

	while (n-- > 0L) {
		p[PX_R] = 0x3f;
		p[PX_G] = 0x3f;
		p[PX_LIGHT] = 0;
		p[PX_B] = 0x3f;
		p += PX_BYTES;
	}
}

/* ORIGIN 0x0031463e, 0x003146f2 and 0x0031482e - the three loops. */
static void cy_frame(Relic_Frame *f, s32 tick)
{
	(void)tick;
	Gfx_SetLightBase(0x7fL, 0x5fL, 0x4fL);
	if (!g_ready) { Frame_Clear(f); return; }
	if (cy_holding()) { Frame_Clear(f); return; }
	if (cy_transition()) {
		if (g_shot == 1L && g_frame < CY_S2_LOOP + CY_FLASH) {
			cy_flash(f);
			Stills_Blur(f);
		}
		return;
	}
	Stills_CityBase(f, g_mask[g_shot]);
	/* ORIGIN 0x0031470e - shot two hands the haze the threshold
	 * DOUBLED, then draws the particles, then hazes again straight,
	 * so one pass lies under the debris and one over it. The other
	 * two shots haze once, after the particles. */
	if (g_shot == 1L) {
		Stills_DrawHaze(f, Stills_Threshold() * 2L);
		Stills_DrawParticles(f);
		Stills_DrawHaze(f, Stills_Threshold());
	} else {
		Stills_DrawParticles(f);
		Stills_DrawHaze(f, Stills_Threshold());
	}
	Stills_Blur(f);
}

/* OURS - key=value on the demo's own names. parts.h. */
static void cy_stats(char *out, int cap)
{
	(void)cap;
	sprintf(out,
	        "shot=%ld frame=%ld threshold=%ld mapcut=%ld"
	        " scroll=%ld scrollrow=%ld vel=%ld parts=%ld ready=%d",
	        (long)(g_shot + 1L), (long)g_frame,
	        (long)Stills_Threshold(), (long)(Stills_Threshold() >> 2),
	        (long)g_scroll, (long)(g_scroll >> 8), (long)g_vel,
	        (long)Stills_ParticleCount(), g_ready);
}

/* OURS - parts.h. */
static s32 cy_length(void)
{
	s32 shots = Parts_GateLength(&g_cue_gate, CY_TICKS);

	if (shots >= PARTS_GATE_OPEN) return shots;
	return shots + Parts_GateLength(&g_hold_gate, CY_HOLD);
}

const Relic_Part PART_City = {
	"2.18", "The city from above, three shots",
	"Valley_RunScene 0x0031460c",
	PART_LIT, ST_W, ST_H,
	cy_init, cy_step, cy_frame, cy_stats,
	CY_TICKS + CY_HOLD, 0, cy_length
};
