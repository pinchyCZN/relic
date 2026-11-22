/* RELIC scene 2.1 - the typing terminal.
 *
 * ORIGIN 0x0033bc0c Terminal_Tick, 0x0033bc52 Terminal_ShowFrame.
 *
 * THERE IS NO FONT AND NO GLYPH BLITTER. Every state of the line is
 * a prerendered 8-plane frame and a script names which to show; the
 * demo flips them by patching eight copper pointers. So this draws
 * a picture, it does not draw text.
 *
 * See docs/parts.txt section 3.
 */

#include <stdio.h>

#include "parts.h"
#include "gfx.h"
#include "image.h"

#define A_SCRIPT   0x0033bd18UL     /* words, 0xffff terminates     */
#define A_PALETTE  0x00349a60UL     /* 256 longwords of 0x00RRGGBB  */
#define A_FRAMES   0x00349e60UL     /* g_TerminalFrames             */

#define FRAME_STEP 0x320UL          /* muls.w #0x320 on the frame   */
#define PLANE_STEP 0x3b40UL         /* addi.l #0x3b40 per plane     */
#define PLANES     8
#define ROW_BYTES  32               /* the STRIDE: 0x320 / 25 rows  */
#define DISP_BYTES 24               /* what DDF actually fetches    */
#define TERM_W     (DISP_BYTES * 8)
#define TERM_H     24               /* DIW, NOT FRAME_STEP/ROW_BYTES */
#define REWIND     0x10             /* subi.w #0x10 on the end      */

/* THE GEOMETRY IS IN g_CopperTerminal AT 0x00367860, not in
 * Terminal_ShowFrame, which writes pointers and nothing else: DIW
 * and DDF give a 192 x 24 display, bottom-centre of a 640 x 256 PAL
 * screen. The derivation is docs/parts.txt section 3.
 *
 * TAKING FRAME_STEP/ROW_BYTES FOR THE HEIGHT CORRUPTS FRAME 19 AND
 * NOTHING ELSE. 25 looks like the height and is not: frame 19's row
 * 24 begins at exactly PLANE_STEP, so it reads plane p+1's row 0.
 * Frames 1 to 18 have blank spare rows, and frame 19 is the one that
 * loops for ever with the cursor on it, so the garbage appears under
 * the finished line and under nothing else. */
#define SCREEN_W   640              /* hstop 449 - hstart 129, hires */
#define SCREEN_H   256              /* v=44 display top, PAL         */
#define STRIP_X    ((SCREEN_W - TERM_W) / 2)   /* 224: see above    */
#define STRIP_Y    192              /* vstart 236 - display top 44   */

static s16 g_script[512];
static int g_script_len;
static u8  g_pal[256][3];

/* OURS */
static int term_init(void)
{
	u32 a;
	int i;

	g_script_len = 0;
	for (a = A_SCRIPT; g_script_len < 512; a += 2) {
		s16 v = Image_S16(a);
		if (v < 0) break;
		g_script[g_script_len++] = v;
	}
	if (g_script_len < 8) {
		fprintf(stderr, "terminal: no script at 0x%08lx\n",
		        (unsigned long)A_SCRIPT);
		return 1;
	}
	for (i = 0; i < 256; i++) {
		u32 c = Image_U32(A_PALETTE + (u32)i * 4);
		g_pal[i][0] = (u8)((c >> 16) & 0xff);
		g_pal[i][1] = (u8)((c >>  8) & 0xff);
		g_pal[i][2] = (u8)( c        & 0xff);
	}
	return 0;
}

/* ORIGIN 0x0033bc0c. The script index is the step HALVED, so every
 * entry lasts two frames. THE TERMINATOR DOES NOT STOP ANYTHING: it
 * rewinds eight entries and carries on, so the last eight loop for
 * ever - that is the cursor blinking on the finished line. The same
 * moment sets g_IntroDone, which is what releases the loader. */
static int frame_for_step(s32 step, int *looped)
{
	int i, tail, over;

	if (step < 0) step = 0;
	i = (int)(step >> 1);
	*looped = 0;
	if (i < g_script_len) return g_script[i];
	*looped = 1;
	tail = REWIND >> 1;
	over = i - g_script_len;
	return g_script[g_script_len - tail + (over % tail)];
}

/* ORIGIN 0x0033bc52. Frame 0 is the blank; frame n reads n-1 at
 * A_FRAMES + (n-1)*0x320, plane p a further p*0x3b40 on. The frames
 * are FRAME-MAJOR INSIDE EACH PLANE - plane 0 holds every frame
 * back to back before plane 1 begins. */
static void draw_frame(Relic_Frame *f, int index)
{
	const u8 *plane[PLANES];
	s32 y, x;
	int p;

	Frame_Clear(f);
	if (index <= 0) return;

	for (p = 0; p < PLANES; p++) {
		plane[p] = Image_At(A_FRAMES + (u32)(index - 1) * FRAME_STEP
		                    + (u32)p * PLANE_STEP);
		if (!plane[p]) return;
	}

	for (y = 0; y < (s32)TERM_H; y++) {
		for (x = 0; x < (s32)TERM_W; x++) {
			s32 px, py, sy = y + STRIP_Y, sx = x + STRIP_X;
			int v = 0;
			for (p = 0; p < PLANES; p++)
				if (plane[p][y * ROW_BYTES + (x >> 3)] & (0x80 >> (x & 7)))
					v |= (1 << p);
			if (!v) continue;
			/* paint the demo pixel across its scaled footprint */
			for (py = FX_MUL(sy, f->scale);
			     py < FX_MUL(sy + 1, f->scale) && py < f->h; py++) {
				if (py < 0) continue;
				for (px = FX_MUL(sx, f->scale);
				     px < FX_MUL(sx + 1, f->scale) && px < f->w; px++) {
					u8 *q;
					if (px < 0) continue;
					q = f->px + ((size_t)(py * f->w) + (size_t)px) * PX_BYTES;
					/* the palette is 8-bit per channel; the frame is
					 * 6-bit, which is what the chipset gets */
					q[PX_R] = (u8)(g_pal[v][0] >> 2);
					q[PX_G] = (u8)(g_pal[v][1] >> 2);
					q[PX_B] = (u8)(g_pal[v][2] >> 2);
				}
			}
		}
	}
}

/* OURS */
static void term_frame(Relic_Frame *f, s32 tick)
{
	int looped;
	draw_frame(f, frame_for_step(tick, &looped));
}

const Relic_Part PART_Terminal = {
	"2.1", "Terminal text", "Terminal_Tick 0x0033bc0c",
	PART_LIT, SCREEN_W, SCREEN_H, term_init, 0, term_frame, 0,
	/* ORIGIN 0x0033bd18 g_TerminalScript - 121 entries before the
	 * 0xffff terminator and the script index is the step HALVED, so
	 * the typing reaches the end on tick 242 and sets g_IntroDone.
	 * THE PART IS AS LONG AS THE SLOWER OF TWO THINGS: the typing or
	 * the relic.dat load SpaceModule_Part0 is waiting on, so 242 is
	 * the floor and not the length on a slow drive. parts.txt 3. */
	242L
};
