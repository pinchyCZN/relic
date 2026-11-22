/* RELIC - the part registry. See parts.h. */

#include "parts.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Relic_Part *const PARTS[] = {
/* THE RUNNING ORDER, NOT THE TIMELINE NUMBERING. SpaceModule_Part0
 * runs Space_RunSunScene BEFORE the first Logo_Run and
 * maybe_Space_RunPlanetScenes after it, so the planet in shadow comes
 * first, then the logo, then the planet lit - parts.txt section 3. */
	&PART_Terminal,
	&PART_Sun,
	&PART_Logo,
	&PART_Approach,
	&PART_Portal,
	&PART_Logo2,
	&PART_Window,
	&PART_Logo3,
	&PART_Orb,
	&PART_Underside,
	&PART_Explosion,
	&PART_Release,
	&PART_Earth,
	&PART_Sand,
	&PART_Temple,
	&PART_Return,
	&PART_Debris,
	&PART_Valley,
	&PART_City,
	&PART_Destruction,
	&PART_Ark,
	/* OURS, AND PAST THE END OF THE DEMO - src/part_credits.c. It is
	 * last so every index above it is the demo's own. */
	&PART_Credits
};
#define NPARTS ((int)(sizeof PARTS / sizeof PARTS[0]))

/* OURS - src/parts.h */
void Parts_StateLine(char *out, int cap, s32 tick, int part, int mark)
{
	const Relic_Part *p = Parts_Get(part);
	int n;

	if (!out || cap < 2) return;
	n = sprintf(out, "tick=%ld part=%d mark=%d",
	            (long)tick, part, mark);
	if (p && p->stats && n > 0 && n < cap - 2) {
		out[n++] = ' ';
		out[n] = 0;
		p->stats(out + n, cap - n);
	}
}

/* OURS - parts.h. The reset on the first tick is what makes a
 * seek and a re-entry behave: Parts_Prime steps from 0. */
void Parts_GateStep(Relic_Gate *g, s32 tick, u32 order, u32 row,
                    s32 fallback)
{
	if (tick <= 1L) {
		g->at = 0;
		Music_Arm(order, row, fallback);
	}
	if (!g->at && tick > 0L && Music_Reached(tick)) g->at = tick;
}

/* OURS - parts.h. */
s32 Parts_GateLength(const Relic_Gate *g, s32 fallback)
{
	if (g->at) return g->at;
	return Music_CueLive() ? PARTS_GATE_OPEN : fallback;
}

/* OURS */
const Relic_Part *Parts_Get(int index)
{
	if (index < 0 || index >= NPARTS) return 0;
	return PARTS[index];
}

/* OURS */
int Parts_Count(void)
{
	return NPARTS;
}

/* OURS. The extra entry points. A part absent from here has only the
 * tick-0 mark Parts_Mark synthesises for it, which is why adding this
 * left the skip keys behaving as they did. A tick here is the one the
 * part's own step() counts, so it is read off that part's note. */
static const Relic_Mark EXTRA[] = {
	{ 14, 2498L, "2.14b the descent", 1, 0x05, 0x00 }, /* temple.txt */
	{ 14, 3460L, "2.14c the orb",     1, 0x09, 0x32 },
	{ 14, 4792L, "2.14d the rise",   -1, 0, 0 },
	{ 14, 5212L, "2.14e shot 2",     -1, 0, 0 },
	{ 14, 5656L, "2.14f the ship rising", -1, 0, 0 },
	{ 14, 6698L, "2.14g the underside",   -1, 0, 0 },
	/* return.txt - the three phases of part 2.15. The ticks are the
	 * part's own and come from its RT_SETUP, RT_FLIGHT and RT_BURST. */
	{ 15,  546L, "2.15b the ripple", -1, 0, 0 },
	{ 15,  780L, "2.15c the swirl",  -1, 0, 0 }
};

/* WHERE EACH PART'S FIRST TICK FALLS IN ITS TUNE. ONLY THE
 * BOUNDARIES A CUE DECIDES ARE HERE: a part that follows a
 * counter-ended one has no position of its own to state, so its mark
 * seeks nothing and says so. Each entry is the pair the PREVIOUS
 * part's poll was armed with - reconstruction.txt 4.6. */
static const struct { int part, tune; u32 order, row; } MARK_MUSIC[] = {
	{  0, MUSIC_INTRO, 0x00, 0x00 },
	{  1, 0, 0x00, 0x00 },
	{  2, 0, 0x02, 0x18 },
	{  4, 0, 0x04, 0x10 },
	{  5, 0, 0x08, 0x00 },
	{  7, 0, 0x15, 0x00 },
	{  9, 0, 0x19, 0x00 },
	{ 10, 0, 0x20, 0x00 },
	{ 14, 1, 0x01, 0x10 },
	{ 15, 1, 0x1f, 0x28 },
	{ 16, 1, 0x24, 0x24 },
	{ 19, 1, 0x26, 0x34 },
	{ 20, 1, 0x27, 0x3c }
};
#define NMARKMUSIC ((int)(sizeof MARK_MUSIC / sizeof MARK_MUSIC[0]))
#define NEXTRA ((int)(sizeof EXTRA / sizeof EXTRA[0]))

static Relic_Mark g_marks[NPARTS + NEXTRA];
static int g_nmarks;

/* OURS */
static void build_marks(void)
{
	int p, e;

	if (g_nmarks) return;
	for (p = 0; p < NPARTS; p++) {
		int m;

		g_marks[g_nmarks].part = p;
		g_marks[g_nmarks].tick = 0L;
		g_marks[g_nmarks].name = PARTS[p]->name;
		g_marks[g_nmarks].tune = -1;
		g_marks[g_nmarks].order = 0;
		g_marks[g_nmarks].row = 0;
		for (m = 0; m < NMARKMUSIC; m++)
			if (MARK_MUSIC[m].part == p) {
				g_marks[g_nmarks].tune = MARK_MUSIC[m].tune;
				g_marks[g_nmarks].order = MARK_MUSIC[m].order;
				g_marks[g_nmarks].row = MARK_MUSIC[m].row;
			}
		g_nmarks++;
		for (e = 0; e < NEXTRA; e++)
			if (EXTRA[e].part == p)
				g_marks[g_nmarks++] = EXTRA[e];
	}
}

/* OURS */
const Relic_Mark *Parts_Mark(int index)
{
	build_marks();
	if (index < 0 || index >= g_nmarks) return 0;
	return &g_marks[index];
}

/* OURS */
int Parts_MarkCount(void)
{
	build_marks();
	return g_nmarks;
}

/* OURS. The mark a part and tick sit in, so stepping on from a part
 * reached any other way carries on from the right place. */
int Parts_MarkOf(int part, s32 tick)
{
	int i, best = 0;

	build_marks();
	for (i = 0; i < g_nmarks; i++)
		if (g_marks[i].part == part && g_marks[i].tick <= tick)
			best = i;
	return best;
}

/* OURS - one "name=value", trimmed. -> 1 if the part took it. */
static int prime_one(const Relic_Part *p, const char *line)
{
	char name[64];
	const char *eq = strchr(line, '=');
	int n;

	if (!eq) return 0;
	n = (int)(eq - line);
	while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
	if (n <= 0 || n >= (int)sizeof name) return 0;
	memcpy(name, line, (size_t)n);
	name[n] = '\0';
	return p->prime(name, (s32)strtol(eq + 1, (char **)0, 0));
}

/* OURS - parts.h. Takes "a=1,b=2" or "@file" with one per line. */
int Parts_Prime(const Relic_Part *p, const char *spec)
{
	char buf[256];
	const char *s;
	int done = 0;

	if (!p || !p->prime || !spec) return -1;
	if (spec[0] == '@') {
		FILE *f = fopen(spec + 1, "r");

		if (!f) return -1;
		while (fgets(buf, (int)sizeof buf, f)) {
			char *h = strchr(buf, '#');

			if (h) *h = '\0';
			done += prime_one(p, buf);
		}
		fclose(f);
		return done;
	}
	s = spec;
	while (*s) {
		const char *c = strchr(s, ',');
		size_t len = c ? (size_t)(c - s) : strlen(s);

		if (len >= sizeof buf) len = sizeof buf - 1;
		memcpy(buf, s, len);
		buf[len] = '\0';
		done += prime_one(p, buf);
		if (!c) break;
		s = c + 1;
	}
	return done;
}
