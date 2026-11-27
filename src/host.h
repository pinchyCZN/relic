/* RELIC - the boundary between the demo and whatever is showing it.
 *
 * THE DEMO IS ONE FUNCTION THAT RENDERS ONE FRAME AND RETURNS.
 * Nothing under src/ except main_*.c may loop waiting for anything,
 * open a window, read a key, or sleep.
 *
 *     native   while (running) { Demo_Advance(dt); present(); }
 *     web      emscripten_set_main_loop(tick, 0, 1);
 *
 * A browser owns its own event loop, so a `while (1)` inside the
 * demo never returns control and the page hangs. Writing it this
 * way costs nothing now and cannot be retrofitted cheaply later -
 * see docs/reconstruction.txt section 3.
 */

#ifndef RELIC_HOST_H
#define RELIC_HOST_H

#include "relic.h"

/* ---- what the host calls -------------------------------------- */

/* Load the demo's files and build the shared tables. 0 = ok. */
int  Demo_Init(const char *relic_path, const char *dat_path);
void Demo_Shutdown(void);

/* Pick the render target. The host owns the size; the demo scales
 * itself into it. Safe to call again on a resize. */
int  Demo_SetResolution(s32 w, s32 h);

/* ONE FRAME. Advances the clock, renders, returns. Never blocks. */
void Demo_Frame(void);
/* One PAL frame per 20 ms of host time, however often a host calls
 * it. Prefer this to Demo_Frame in anything that presents. */
void Demo_Advance(s32 ms);

/* N ticks of state for ONE render, for a host's fast forward. The
 * tail of the run is drawn so the temporal filters refill. */
void Demo_Skip(s32 n);

/* The frame just rendered, as 8-bit RGB, w*h*3. Valid until the
 * next Demo_Frame. */
const u8 *Demo_Pixels(s32 *w, s32 *h);

/* Stepping the part list - the viewer's controls, not the demo's. */
void        Demo_GotoPart(int index);
void        Demo_StepPart(int delta);
/* OURS. The skip keys move by MARK, not by part - see parts.h. A
 * part with no extra marks is one mark, so this is the old
 * behaviour wherever none are declared. */
void        Demo_GotoMark(int index);
void        Demo_StepMark(int delta);
int         Demo_MarkIndex(void);
const char *Demo_MarkName(void);
void        Demo_SeekTick(s32 tick);
/* Hand over to the next part when this one reaches the length it
 * declares, which is how the demo's own scene changes happen. On by
 * default, and a part declaring no length never hands over. The PPM
 * dumper drives parts directly and is not affected either way. */
void        Demo_SetAutoAdvance(int on);
int         Demo_AutoAdvance(void);
int         Demo_PartIndex(void);
s32         Demo_TickCount(void);

/* RELIC_MODE_* bits. Stored on the frame and reported back. SUBPIXEL
 * is forced off at scale 1.0 so that stays the exact reference; part
 * 2.3 honours it, and Demo_Modes reports what survived the gate. */
void        Demo_SetModes(s32 modes);
s32         Demo_Modes(void);

/* OURS - THE POINTER AS A GESTURE, NOT AS A CAMERA. down is the
 * button, and x_fx, y_fx are 16.16 of how far it has moved since it
 * went down as a fraction of the window's half width and half
 * height: x right and y down positive, +-1.0 full deflection.
 *
 * A HOST NAMES THE GESTURE AND NOTHING MORE. What a deflection is
 * worth, and whether it springs back or stays where it was left, is
 * each part's own reading of it - Space_Look in src/space.c springs
 * it into a look for the space scenes, src/part_credits.c keeps it
 * as an orbit. A RELEASED DRAG EASES HOME HERE rather than in a
 * host, so every host feels the same. Up and zero unless a host
 * sets one, so main_ppm's renders are untouched. */
void        Demo_SetDrag(int down, s32 x_fx, s32 y_fx);
void        Demo_Drag(int *down, s32 *x_fx, s32 *y_fx);

/* OURS - THE LOOK HOLD. With it on, a released drag is kept rather
 * than eased home, and the next drag adds to it, so the yaw winds
 * round without limit and the pitch stops at full deflection.
 * Demo_Look is the drag plus what is kept - the reading for a part
 * that LOOKS; a part that keeps its own state, as c.1 does, reads
 * Demo_Drag and never sees the hold. A part change or Demo_SetHome
 * drops what is kept; the mode itself stays. */
void        Demo_Look(s32 *x_fx, s32 *y_fx);
void        Demo_SetLookHold(int on);
int         Demo_LookHold(void);

/* OURS - THE WHEEL AS A RUNNING COUNT, LIMITED NOWHERE. How far a
 * notch travels and where travel has to stop depend on what is being
 * looked at, and only a part knows that. Positive is nearer.
 * src/part_credits.c reads it. */
void        Demo_SetWheel(s32 notch);
s32         Demo_Wheel(void);

/* OURS - A RUNNING COUNT OF "NEXT", which is the keyboard's answer
 * to the wheel: a host bumps it, a part reads it modulo whatever it
 * has to show. Nothing here says what is being cycled.
 * src/part_credits.c reads it. */
void        Demo_SetCycle(s32 n);
s32         Demo_Cycle(void);

/* OURS - THE KEYS A HOST IS HOLDING, under generic names, because
 * which key it is belongs to the host and what it means belongs to
 * the part - the same split as the drag's button. Clear unless a
 * host sets one. */
#define DEMO_HOLD_A 0x1UL
#define DEMO_HOLD_B 0x2UL
void        Demo_SetHold(u32 mask);
u32         Demo_Hold(void);

/* OURS - THE HOST ASKING FOR REST, for a part holding a value no
 * amount of winding can land on exactly. ONE FRAME LONG, so a part
 * that was not running cannot collect it afterwards. */
void        Demo_SetHome(void);
int         Demo_Home(void);
int         Demo_PartCount(void);
const char *Demo_PartLabel(void);

/* OURS - where the demo is, as one key=value line. The format and
 * why the keys are the demo's own names: src/parts.h. */
const char *Demo_StateLine(void);

/* ---- what the demo calls -------------------------------------- */

/* THE TIMELINE RUNS ON THE MUSIC, NOT ON FRAMES. 0x0021ff3c arms a
 * position in the module and 0x0021ff64 polls whether the replayer
 * has reached it; scenes end on that, not on a frame count. So the
 * clock is an interface from the start even while nothing plays -
 * a silent implementation advances it at the module's tempo and the
 * timeline still runs true. See docs/reconstruction.txt section 4.
 */
void Music_Start(int module_index);
void Music_Stop(void);
/* ORIGIN 0x0021ff3c. fallback_ticks is how long the phase lasts
 * when the music cannot answer - no module, or a timeline the host
 * has broken. It is the measured length of the phase. */
void Music_Arm(u32 order, u32 row, s32 fallback_ticks);
/* ORIGIN 0x0021ff64. elapsed is the PART'S OWN ticks since the arm,
 * because the fallback cannot use the music clock: the hosts that
 * need the fallback are the ones that never advance it. */
int  Music_Reached(s32 elapsed);
/* A CUE IS AN ABSOLUTE SONG POSITION, so it only means anything in a
 * continuous run from part 0. A jump or a skip leaves the music
 * where it was and every later cue with it, so both say so here and
 * the fallback takes over for the rest of the run. */
void Music_BreakTimeline(void);
/* Whether an armed cue can still answer. A part waiting on one must
 * keep running while this holds, and fall back when it does not. */
int  Music_CueLive(void);

/* ADVANCE THE REPLAYER WITHOUT LISTENING. Music_SeekTo rewinds the
 * tune and drains to the pair, which is how a jump lands in sync; a
 * bare order jump cannot, because it resets the tempo. -> 0 when it
 * lands. Music_DrainTicks is the same thing by count, for a fast
 * forward. docs/reconstruction.txt 4.6. */
const char *Demo_MusicLine(void);
s32  Music_DrainTick(void);           /* -> the tick's sample count */
s32  Music_DrainTicks(s32 ticks);
int  Music_DrainTo(u32 order, u32 row);
int  Music_SeekTo(int tune, u32 order, u32 row);
void Music_Position(u32 *order, u32 *row);
u32  Music_Tick(void);                /* monotonic replayer ticks  */

/* OURS - a module in any format ibxm reads, handed over whole; the
 * caller keeps the bytes. Until one loads the clock runs silent and
 * the timeline is unchanged. docs/reconstruction.txt 4. */
int  Music_Load(const u8 *bytes, s32 len, s32 rate);
/* The demo's three tunes: 0 and 1 are relic.dat's chunks, MUSIC_INTRO
 * is the module embedded in the executable that plays under the
 * terminal. Music_Preload parses all three at start-up - Dat_Open
 * has to have run - and returns how many it got. A HOST THAT WANTS
 * SOUND MUST CALL IT; without it Music_Start finds nothing and the
 * timeline runs on the fallbacks. */
#define MUSIC_INTRO 2
int  Music_Preload(s32 rate);
/* The mixing rate, told once, so a tune swapped in at an act
 * boundary does not need the host to say it again. */
void Music_SetRate(s32 rate);
void Music_Pin(void);
void Music_Unload(void);
int  Music_Playing(void);

/* OURS - THE HOST'S SECOND ENTRY POINT, called from wherever that
 * host pulls audio and never from the frame call. Interleaved
 * stereo pairs; silence when nothing is loaded. */
void Demo_Audio(s16 *out, s32 pairs);
void Music_Advance(s32 ms);           /* a silent clock steps here */

#endif /* RELIC_HOST_H */
