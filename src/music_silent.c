/* RELIC - the music clock, with no music.
 *
 * THE DEMO'S TIMELINE RUNS ON THE REPLAYER'S POSITION, NOT ON A
 * FRAME COUNT. Space_RunPart arms a position with 0x0021ff3c and
 * then blocks on 0x0021ff64 until the replayer reaches it; so do
 * several other scenes. Model that from the start or every scene
 * transition is a guess.
 *
 * This implementation plays nothing and advances a clock at the
 * module's tempo, which is enough for the timeline to run true. A
 * real replayer drops in behind the same four calls later - see
 * docs/reconstruction.txt section 4 for why none of it is being
 * reverse engineered.
 */

#include "host.h"

/* OURS - a silent clock at the module's tempo.
 * Protracker defaults: six ticks a row at 125 BPM is 50 ticks a
 * second, which is also PAL's frame rate - which is why a demo of
 * this era can treat the two as the same clock. */
#define TICKS_PER_SECOND 50
#define TICKS_PER_ROW     6
#define ROWS_PER_ORDER   64

static u32 g_tick;
static s32 g_frac_ms;
static u32 g_armed;
static int g_have_armed;
/* OURS - the replayer is not reverse engineered. */

void Music_Start(int module_index)
{
	(void)module_index;
	g_tick = 0;
	g_frac_ms = 0;
	g_armed = 0;
	g_have_armed = 0;
}
/* OURS */

void Music_Stop(void)
{
	g_have_armed = 0;
}
/* ORIGIN 0x0021ff3c  Music_ArmCue */

void Music_Arm(u32 order, u32 row, s32 fallback_ticks)
{
	g_armed = (order * ROWS_PER_ORDER + row) * TICKS_PER_ROW;
	g_have_armed = 1;
}
/* OURS - host.h. Nothing to drain and nowhere to seek. */
s32 Music_DrainTicks(s32 ticks)
{
	(void)ticks;
	return 0;
}

/* OURS */
int Music_DrainTo(u32 order, u32 row)
{
	(void)order; (void)row;
	return 1;
}

/* OURS */
int Music_SeekTo(int tune, u32 order, u32 row)
{
	(void)tune; (void)order; (void)row;
	return 1;
}

/* OURS */
void Music_Position(u32 *order, u32 *row)
{
	if (order) *order = 0;
	if (row) *row = 0;
}

/* OURS - host.h. Nothing can answer a cue here. */
int Music_CueLive(void)
{
	return 0;
}

/* ORIGIN 0x0021ff64  Music_PollCue */

int Music_Reached(s32 elapsed)
{
	if (!g_have_armed) return 1;
	return g_tick >= g_armed;
}
/* OURS */

u32 Music_Tick(void)
{
	return g_tick;
}

#define MS_PER_TICK (1000 / TICKS_PER_SECOND)   /* 20 */

void Music_Advance(s32 ms)
{
	if (ms < 0) return;
	g_frac_ms += ms;
	g_tick += (u32)(g_frac_ms / MS_PER_TICK);
	g_frac_ms %= MS_PER_TICK;
}
