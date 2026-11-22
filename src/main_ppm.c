/* RELIC - the no-dependency host: render frames to PPM files.
 *
 * This exists so the demo core can be built and checked with
 * nothing but a C compiler, and so its output can be diffed against
 * the Python transcriptions in sim/ that are already verified.
 *
 *     relic_ppm <relic-binary> <out-dir> [-p part] [-s scale]
 *               [-t tick] [-n count] [-e every]
 *     relic_ppm <relic-binary> <out-dir> -agent
 *     relic_ppm <relic-binary> <out-dir> -dat relic.dat -cues
 *     relic_ppm <relic-binary> <out-dir> -dat relic.dat -timeline
 *
 * -t PLAYS UP TO ITS TICK - docs/reconstruction.txt 6.1.
 *
 * SDL2 and emscripten are other hosts over the same core; nothing
 * below this line is known to the parts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "relic.h"
#include "image.h"
#include "gfx.h"
#include "parts.h"
#include "host.h"
#include "dat.h"
#include "sprout.h"
#include "agent.h"

static int write_ppm(const char *path, const u8 *rgb, s32 w, s32 h)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "relic: cannot write %s\n", path);
		return 1;
	}
	fprintf(f, "P6\n%ld %ld\n255\n", (long)w, (long)h);
	fwrite(rgb, 1, (size_t)(w * h) * 3, f);
	fclose(f);
	return 0;
}

/* A sheet of gfx.c's primitives, so one can be looked at on its own
 * before any scene uses it. A primitive that cannot be eyeballed is
 * a primitive nobody trusts. */
static void probe_sheet(Relic_Frame *f)
{
	Frame_Clear(f);
	Gfx_Flare(f, 26, 25, 20, g_RaySpikes,  g_ColRampAmber);
	Gfx_Flare(f, 80, 25, 20, g_RayArcs,    g_ColRampMagenta);
	Gfx_Flare(f, 134, 25, 20, g_RayStreak, g_ColRampPale);

	Gfx_Flare(f, 26, 72, 14, g_RayStreak,  g_ColRampWarm);
	Gfx_GlowBlob(f, 80, 72, 18, g_RampBolt);
	Gfx_GlowBlob(f, 134, 72, 18, g_RampSpark);

	Gfx_LineAA(f, 8, 50, 152, 50);
}

/* OURS - THE REPLAYER'S OWN ROW TIMING, as "tune order row field"
 * on stdout: the field each row of each tune begins on. A demo
 * field count mixes the tune's tempo with the fields it drops under
 * load, and subtracting this table is what separates the two. Runs
 * each tune to its end or the guard, LOOP AND ALL - which row
 * repeats is the reader's to spot, since a pattern break moving
 * the row backwards is not a loop and only the sequence says so.
 * docs/reconstruction.txt 4.6. */
static void music_rowdump(s32 rate)
{
	s32 spf = rate / 50;
	int t;

	for (t = 0; t < 3; t++) {
		u32 order = 0, row = 0, po = 0xffffffffUL, pr = 0;
		s32 samples = 0, n, guard;

		Music_Start(t);
		for (guard = 0; guard < 0x20000L; guard++) {
			Music_Position(&order, &row);
			if (order != po || row != pr) {
				printf("%d %lu %lu %ld\n", t,
				       (unsigned long)order,
				       (unsigned long)row,
				       (long)(samples / spf));
				po = order;
				pr = row;
			}
			n = Music_DrainTick();
			if (n <= 0) break;
			samples += n;
		}
	}
	Music_Stop();
}

/* ONE PAL FIELD OF AUDIO A TICK, as main_sdl's audio_step_one
 * pulls it. Pulling it is NOT optional: ibxm only advances where
 * the audio is drained, so a walk that skips it reads position 0
 * for ever and every gate answers its fallback - the landmine on
 * Music_Reached in music_ibxm.c. */
#define TL_RATE  44100L
#define TL_PAIRS (TL_RATE / 50)

/* OURS - WALK THE WHOLE DEMO AND SAY WHERE ITS MUSIC IS, printing
 * "tick part order row" whenever one of the three changes. This is
 * the PORT'S timeline, with part lengths and gates in play, so it is
 * what the demo's own cue frames get compared against - where -cues
 * reports the replayer on its own and nothing else. The tick is
 * counted here because Demo_TickCount is PART-LOCAL: handover
 * resets it, so it steps backwards at every part boundary.
 * docs/reconstruction.txt 4.5. */
static void demo_timeline(const char *bin, const char *dat, s32 cap)
{
	static s16 mix[TL_PAIRS * 2];
	u32 order = 0, row = 0, po = 0xffffffffUL, pr = 0;
	s32 t = 0;
	int part, pp = -1, n;

	if (Demo_Init(bin, dat)) return;
	n = Demo_PartCount();
	if (Music_Preload(TL_RATE) != 3)
		fprintf(stderr, "relic: -timeline wants -dat for the tunes\n");
	if (Demo_SetResolution(320, 256)) { Demo_Shutdown(); return; }
	while (t < cap) {
		Demo_Frame();
		t++;
		Demo_Audio(mix, TL_PAIRS);
		part = Demo_PartIndex();
		if (part < 0 || part >= n || part < pp) break;
		Music_Position(&order, &row);
		if (part != pp || order != po || row != pr) {
			printf("%ld %d %lu %lu\n",
			       (long)t, part,
			       (unsigned long)order, (unsigned long)row);
			pp = part;
			po = order;
			pr = row;
		}
	}
	Demo_Shutdown();
}

int main(int argc, char **argv)
{
	const char *bin, *dir, *dat = 0;
	const char *prime = 0;
	s32 stepped;
	const Relic_Part *part;
	Relic_Frame frame;
	u8 *rgb;
	char path[512];
	int i, pidx = 0, count = 1, every = 1, probe = 0, cycle = 0, clock_s = 0;
	int dat_list = 0, raw = 0, rate = 0, subt = -1, agent = 0;
	int states = 0, cues = 0, tline = 0;
	int prefog = 0;
	int sprout_probe = 0;
	s32 scale = FX_ONE, tick = 0, n;

	if (argc < 3) {
		fprintf(stderr,
		        "usage: %s <relic-binary> <out-dir>"
		        " [-p part] [-s scale] [-t tick] [-n count] [-e every]\n",
		        argv[0]);
		return 2;
	}
	bin = argv[1];
	dir = argv[2];
	for (i = 3; i + 1 < argc; i += 2) {
		if      (!strcmp(argv[i], "-dat")) dat = argv[i + 1];
		else if (!strcmp(argv[i], "-p")) pidx  = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-t")) tick  = (s32)atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-n")) count = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-e")) every = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-prime")) prime = argv[i + 1];
		else if (!strcmp(argv[i], "-s"))
			scale = (s32)(atof(argv[i + 1]) * 65536.0);
	}
	for (i = 3; i < argc; i++)
	{   if (!strcmp(argv[i], "-probe")) probe = 1;
			if (!strcmp(argv[i], "-raw")) raw = 1;
			if (!strcmp(argv[i], "-prefog")) { raw = 1; prefog = 1; }
			if (!strcmp(argv[i], "-datlist")) dat_list = 1;
			if (!strcmp(argv[i], "-agent")) agent = 1;
			if (!strcmp(argv[i], "-states")) states = 1;
			if (!strcmp(argv[i], "-cues")) cues = 1;
			}
	for (i = 3; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "-cycle")) cycle = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "-clock")) clock_s = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "-rate")) rate = atoi(argv[i + 1]);
	if (!strcmp(argv[i], "-timeline")) tline = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "-sub")) subt = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "-sprout"))
			sprout_probe = atoi(argv[i + 1]);
	}
	if (every < 1) every = 1;
	if (scale < 1) scale = FX_ONE;

	/* THE AGENT OWNS THE CLOCK HERE. Nothing advances except on a
	 * verb, so the pump may block while paused and an idle session
	 * costs nothing. Demo_Init does the Image_Load below itself. */
	if (agent) {
		if (Demo_Init(bin, dat)) return 1;
		if (Agent_Start(dir)) {
			fprintf(stderr,
			        "relic: -agent needs a Win32 build\n");
			Demo_Shutdown();
			return 1;
		}
		while (!Agent_Pump(AGENT_PUMP_BLOCK | AGENT_PUMP_CLOCK))
			;
		Agent_Stop();
		Demo_Shutdown();
		return 0;
	}

	if (Image_Load(bin)) return 1;
	/* OPTIONAL: only a texture is lost without it. */
	if (dat) Dat_Open(dat);
	if (Gfx_Init()) { Image_Free(); return 1; }

	if (tline) {
		demo_timeline(bin, dat, (s32)tline);
		return 0;
	}

	if (cues) {
		s32 r = rate ? rate : 44100;

		if (Music_Preload(r) != 3)
			fprintf(stderr, "relic: -cues wants -dat"
			        " for tunes 0 and 1\n");
		music_rowdump(r);
		Image_Free();
		return 0;
	}

	if (clock_s) {
		/* A TICK IS A PAL VERTICAL BLANK, NOT A PRESENTED FRAME.
		 * Feeding Demo_Frame straight off the host's loop ran the
		 * demo at whatever rate the loop managed - the terminal's
		 * cursor blinked at twice its real rate. This drives
		 * Demo_Advance with a 60 Hz host's deltas, so the count it
		 * prints is the demo's own rate. */
		int f;
		if (Demo_Init(bin, 0)) return 1;
		Demo_SetResolution(320, 256);
		for (f = 0; f < clock_s * 60; f++)
			Demo_Advance(f % 3 == 0 ? 16 : 17);   /* 60 Hz, as ints */
		printf("clock  %d s of a 60 Hz host  ->  %ld tick(s)\n",
		       clock_s, (long)Demo_TickCount());
		Demo_Shutdown();
		return 0;
	}

	if (rate) {
		/* THE MEASUREMENT THAT MATTERS: real elapsed time, a real
		 * resolution, and the demo actually drawing. -clock feeds
		 * synthetic deltas and so only checks the arithmetic; this
		 * checks whether a frame's cost still drags the clock down
		 * with it, which is what made the scene run at a quarter
		 * speed. */
		clock_t t0, now, last;
		s32 w = FX_MUL(160, scale), h = FX_MUL(100, scale);
		long ticks, frames = 0;
		double secs;
		if (Demo_Init(bin, 0)) return 1;
		Demo_GotoPart(pidx);
		Demo_SetResolution(w, h);
		t0 = last = clock();
		for (;;) {
			s32 ms;
			now = clock();
			ms = (s32)((now - last) * 1000 / CLOCKS_PER_SEC);
			if (ms > 0) { Demo_Advance(ms); last = now; frames++; }
			if ((now - t0) >= (clock_t)rate * CLOCKS_PER_SEC) break;
		}
		/* AGAINST ACTUAL ELAPSED TIME, not the nominal window: the
		 * loop overshoots by however long the last present took, and
		 * at 13 frames a second that alone reads as 3% fast. */
		secs = (double)(now - t0) / (double)CLOCKS_PER_SEC;
		ticks = (long)Demo_TickCount();
		printf("rate  part %d at %ldx%ld for %.2f s:"
		       "  %ld tick(s), %ld present(s)\n",
		       pidx, (long)w, (long)h, secs, ticks, frames);
		printf("      %.1f ticks/s (want 50.0), %.1f presented/s\n",
		       (double)ticks / secs, (double)frames / secs);
		Demo_Shutdown();
		return 0;
	}

	if (sprout_probe) {
		/* OURS. The octree's silhouette as text, which is how the
		 * traversal gets judged before there is any placement to draw
		 * it with: a coherent shape says the descent and its guards
		 * work, noise says they do not. */
		int gx, gy, g = sprout_probe, hits = 0;
		if (Dat_Open(dat ? dat : "relic.dat")) {
			fprintf(stderr, "relic: -sprout needs relic.dat\n");
			return 1;
		}
		if (Sprout_Init()) return 1;
		for (gy = 0; gy < g; gy++) {
			for (gx = 0; gx < g; gx++) {
				s32 u = -0x1000L + gx * (0x2000L / g);
				s32 v = -0x1000L + gy * (0x2000L / g);
				const u8 *p = Sprout_Pixel(0, u, v, 0, 0);
				if (p) hits++;
				putchar(p ? (p[PX_R] > 7 ? '#' : '+') : '.');
			}
			putchar('\n');
		}
		printf("%d of %d rays hit\n", hits, g * g);
		/* The demo writes solid runs - approach.txt 4.4.6 - so the
		 * longest run each sector manages along a row is the number
		 * to drive up to the span width. */
		/* THE DEMO'S OWN NUMBERS, captured at a frame where sector 2
		 * is steep: spin 5460, scale 36, slope 14100, row 46 of the
		 * object which is v = -10, and an 18-pixel span. The demo
		 * wrote 8 consecutive pixels there - approach.txt 4.4.10. */
		{
			s32 run = Sprout_ProbeRun(2, 14100, -10, 18);
			s32 sh  = Sprout_ProbeRun(3, -4741, -10, 31);
			printf("sector 2 steep  slope 14100 v -10 span 18"
			       " -> longest run %d  (demo: 8)\n", (int)run);
			printf("sector 3 shallow slope -4741 v -10 span 31"
			       " -> longest run %d\n", (int)sh);
		}
		Sprout_Free();
		Dat_Close();
		return 0;
	}

	if (dat_list) {
		int k;
		if (Dat_Open(argv[2])) {
			fprintf(stderr, "relic: cannot read %s\n", argv[2]);
			return 1;
		}
		printf("relic.dat: %d chunk(s)\n", Dat_Count());
		for (k = 0; k < Dat_Count(); k++)
			printf("  %-16s %9lu bytes\n", Dat_Name(k),
			       (unsigned long)Dat_Size(k));
		Dat_Close();
		return 0;
	}

	if (cycle) {
		/* STEPPING PARTS MUST NOT CHANGE THE SCALE. Re-fitting a
		 * part into the already-fitted frame instead of the host's
		 * size shrinks it on every switch and the error compounds,
		 * which is invisible in a single render and obvious after
		 * four. */
		int step;
		s32 fw, fh;
		if (Demo_Init(bin, 0)) { return 1; }
		Demo_SetResolution(1280, 720);
		for (step = 0; step < cycle; step++) {
			Demo_Pixels(&fw, &fh);
			printf("  step %2d  part %d  frame %ldx%ld\n",
			       step, Demo_PartIndex(), (long)fw, (long)fh);
			Demo_StepPart(1);
			Demo_SetResolution(1280, 720);
		}
		Demo_Shutdown();
		return 0;
	}

	if (probe) {
		Relic_Frame pf;
		u8 *prgb;
		if (Frame_Init(&pf, FX_MUL(DEMO_W, scale),
		               FX_MUL(DEMO_H, scale), scale)) {
			Image_Free();
			return 1;
		}
		prgb = (u8 *)malloc((size_t)(pf.w * pf.h) * 3);
		if (!prgb) { Frame_Free(&pf); Image_Free(); return 1; }
		probe_sheet(&pf);
		Gfx_ResolveToRGB(&pf, prgb);
		sprintf(path, "%s/probe.ppm", dir);
		write_ppm(path, prgb, pf.w, pf.h);
		printf("probe sheet %ldx%ld -> %s\n",
		       (long)pf.w, (long)pf.h, path);
		free(prgb);
		Frame_Free(&pf);
		Image_Free();
		return 0;
	}

	part = Parts_Get(pidx);
	if (!part) {
		fprintf(stderr, "relic: no part %d (have %d)\n",
		        pidx, Parts_Count());
		Image_Free();
		return 1;
	}
	if (part->init && part->init()) { Image_Free(); return 1; }

	n = FX_MUL(part->demo_w, scale);
	if (Frame_Init(&frame, n, FX_MUL(part->demo_h, scale), scale)) {
		fprintf(stderr, "relic: out of memory for the frame\n");
		Image_Free();
		return 1;
	}
	rgb = (u8 *)malloc((size_t)(frame.w * frame.h) * 3);
	if (!rgb) {
		Frame_Free(&frame);
		Image_Free();
		return 1;
	}

	printf("part %d  %s  %s\n", pidx, part->id, part->name);
	printf("  origin %s\n", part->origin);
	printf("  demo %ldx%ld  scale %.3f  frame %ldx%ld  %s\n",
	       (long)part->demo_w, (long)part->demo_h,
	       scale / 65536.0, (long)frame.w, (long)frame.h,
	       part->status == PART_LIT ? "LIT" : "SHAPED ONLY");

	if (part->step)
		for (i = 0; i < (int)tick; i++) part->step((s32)i);
	stepped = tick - 1;

	if (prime) {
		int got = Parts_Prime(part, prime);

		if (got < 0)
			printf("  -prime: part %d has no prime hook\n", pidx);
		else
			printf("  -prime: %d value(s) applied, NOT A SCORED "
			       "RUN\n", got);
	}

	for (i = 0; i < count; i++) {
		s32 t = tick + (s32)i * every;
		if (part->step)
			while (stepped < t) part->step(++stepped);
		/* -sub holds the scene at a fixed fraction of a tick, which is
		 * how the subpixel path gets checked without a window. GATED AT
		 * SCALE 1.0 exactly as Demo_SetModes gates it, so no argument to
		 * this tool can disturb the reference render. */
		frame.subtick = 0;
		if (subt >= 0 && scale > FX_ONE) {
			frame.modes |= RELIC_MODE_SUBPIXEL;
			frame.subtick = (s32)subt & 0xffffL;
		}
		part->frame(&frame, t);
		if (!states) {
			Gfx_ResolveToRGB(&frame, rgb);
			sprintf(path, "%s/p%d_t%05ld.ppm", dir, pidx, (long)t);
			if (write_ppm(path, rgb, frame.w, frame.h)) break;
			printf("  wrote %s\n", path);
		}
		/* OURS. The four-byte pixels before Gfx_ResolveToRGB, so a frame
		 * can be diffed against the demo's own framebuffer read out of
		 * WinUAE. The resolved RGB cannot do that job: wherever LIGHT is
		 * 0x40 both sides are simply black, which hides every difference
		 * in the colour underneath. */
		if (raw) {
			FILE *rf;
			const u8 *src = frame.px;
			size_t n = (size_t)(frame.w * frame.h) * PX_BYTES;

			if (prefog) {
				const u8 *pre = Temple_PresentBuffer();

				if (pre) {
					src = pre;
					n = (size_t)(160 * 100) * PX_BYTES;
				} else {
					printf("  -prefog: part has no buffer\n");
				}
			}
			sprintf(path, "%s/p%d_t%05ld.raw", dir, pidx, (long)t);
			rf = fopen(path, "wb");
			if (rf) {
				fwrite(src, 1, n, rf);
				fclose(rf);
				printf("  wrote %s\n", path);
			}
		}
		if (states) {
			char line[512];

			Parts_StateLine(line, (int)sizeof line, t, pidx, -1);
			puts(line);
		}
		/* A part with no picture yet can still say something. */
		if (!states && part->stats) {
			char line[256];
			line[0] = 0;
			part->stats(line, (int)sizeof line);
			printf("  tick %-6ld  %s\n", (long)t, line);
		}
	}

	free(rgb);
	Frame_Free(&frame);
	Image_Free();
	return 0;
}
