/* RELIC - the part list.
 *
 * One entry per thing the demo shows, in the order
 * docs/parts.txt section 2 lists them. A part renders one frame
 * into a Relic_Frame and nothing else - no host calls, no globals
 * owned by a host - which is what lets the same code run under the
 * PPM dumper, SDL2 and emscripten.
 */

#ifndef RELIC_PARTS_H
#define RELIC_PARTS_H

#include "relic.h"

typedef enum {
	PART_SHAPED_ONLY,   /* geometry or art correct, effects not yet */
	PART_LIT            /* the effect itself is transcribed         */
} Relic_PartStatus;

typedef struct Relic_Part {
	const char *id;                 /* the docs/parts.txt number    */
	const char *name;
	const char *origin;             /* the routine it comes from    */
	Relic_PartStatus status;
	s32 demo_w, demo_h;             /* the part's own resolution    */
	int  (*init)(void);
	/* OPTIONAL, AND IT IS WHAT DECOUPLES MOTION FROM FRAME RATE.
	 * A part whose state evolves frame to frame - an eased value, a
	 * diffusion field - puts that here. step() is called once per 50 Hz
	 * tick however often the host presents, and frame() then draws ONCE
	 * per presented frame. A part that leaves this NULL is taken to be a
	 * pure function of its tick and gets the old behaviour: frame() per
	 * tick, resolved once. See demo.c. */
	void (*step)(s32 tick);
	void (*frame)(Relic_Frame *f, s32 tick);
	/* OPTIONAL. Writes one line of numbers a host may print, so an
	 * effect that has no picture yet is still checkable. NULL when
	 * there is nothing to say. */
	void (*stats)(char *out, int cap);
	/* OPTIONAL. How many ticks the part lasts, or 0 for open-ended.
	 * APPENDED DELIBERATELY: the initialisers are positional, so a
	 * part that does not know its length leaves this off and gets 0.
	 * A length here is the DEMO'S, measured or read - the sequencer
	 * cuts most scenes on a music cue, so most parts cannot have one.
	 * Nothing stops a host running past it; it says where the scene
	 * ends, it does not enforce it. */
	s32 ticks;
	/* OPTIONAL, AND FOR MEASUREMENT ONLY. Sets one named piece of
	 * the part's live state from a value read out of the running
	 * demo, so a frame can be rendered from the demo's OWN numbers
	 * rather than from ours. -> 0 if the name is not one it knows.
	 *
	 * This exists because the reconstruction's clock does not line
	 * up with the demo's: one global RNG stream, phases that end on
	 * cues rather than counts. Priming makes a frame comparable
	 * without pretending the drift is fixed. A SCORED RUN MUST NOT
	 * USE IT - see docs/effects/temple.txt 8.2. */
	int (*prime)(const char *name, s32 value);
	/* OPTIONAL. The part's length when a MUSIC CUE decides it and
	 * not a constant, so the handover can move with the cue. NULL
	 * uses ticks. */
	s32 (*length)(void);
} Relic_Part;

/* OURS - THE COMMON SHAPE OF A PART THAT ENDS ON ONE MUSIC CUE:
 * armed on its first tick, polled on every tick after, and the tick
 * it answered on becomes the part's length. Eleven parts are exactly
 * this, so they share it rather than each spelling it out.
 * reconstruction.txt 4.1. */
typedef struct {
	s32 at;                 /* the tick it answered on, or 0 */
} Relic_Gate;

void Parts_GateStep(Relic_Gate *g, s32 tick, u32 order, u32 row,
                    s32 fallback);
/* The tick the gate opened on. WHILE A LIVE CUE IS STILL PENDING
 * THIS IS PARTS_GATE_OPEN, not the fallback: a gate has to be able
 * to LENGTHEN a part and not only shorten it, because the demo's own
 * poll is a loop that keeps drawing until its cue lands. Small
 * enough that two of them still sum inside an s32. */
#define PARTS_GATE_OPEN 0x00100000L
s32  Parts_GateLength(const Relic_Gate *g, s32 fallback);

const Relic_Part *Parts_Get(int index);
int               Parts_Count(void);

/* OURS. Applies "name=value", or a whole file of them one per line
 * with # for a comment, to the part given. -> the number applied,
 * or -1 if the part has no prime hook. */
int Parts_Prime(const Relic_Part *p, const char *spec);

/* OURS. AN ENTRY POINT THE SKIP KEYS STOP AT. Every part has one at
 * tick 0, and a part holding more than one scene declares the rest,
 * so a module thousands of ticks long can be reached at the shot
 * wanted rather than only at its start. */
typedef struct Relic_Mark {
	int         part;
	s32         tick;
	const char *name;
	/* WHERE THE MUSIC STANDS WHEN A FULL RUN REACHES THIS MARK, so
	 * a jump can drain the replayer to it instead of guessing. tune
	 * is -1 where no cue decides the boundary and nothing has been
	 * measured; the jump then runs on the fallbacks and says so.
	 * docs/reconstruction.txt 4.6. */
	int         tune;
	u32         order, row;
} Relic_Mark;

const Relic_Mark *Parts_Mark(int index);
int               Parts_MarkCount(void);
int               Parts_MarkOf(int part, s32 tick);

/* Each part lives in its own file and exports one of these. */
extern const Relic_Part PART_Terminal;
extern const Relic_Part PART_Logo;
extern const Relic_Part PART_Logo2;
extern const Relic_Part PART_Logo3;
extern const Relic_Part PART_Orb;
extern const Relic_Part PART_Underside;
extern const Relic_Part PART_Explosion;
extern const Relic_Part PART_Release;
extern const Relic_Part PART_Earth;
extern const Relic_Part PART_Sand;
extern const Relic_Part PART_Temple;
extern const Relic_Part PART_Return;
extern const Relic_Part PART_Debris;
extern const Relic_Part PART_Valley;
extern const Relic_Part PART_City;
extern const Relic_Part PART_Destruction;
extern const Relic_Part PART_Ark;
extern const Relic_Part PART_Window;
extern const Relic_Part PART_Sun;
extern const Relic_Part PART_Approach;
extern const Relic_Part PART_Portal;
/* OURS - no counterpart in the demo. src/part_credits.c. */
extern const Relic_Part PART_Credits;

/* The object's last projected position, which the next scene's spark
 * shower is seeded from. */
void Approach_FinalLightProj(s32 *proj_x, s32 *proj_y);
s32  Approach_FinalSkyPan(void);
void Approach_FinalCamAngles(s32 *angle, s32 *angle2);

/* OURS: the temple's presented buffer, pre-fog - what (0x00374714)
 * holds. 160 x 100 x 4, NULL before the part allocates. */
const u8 *Temple_PresentBuffer(void);

/* Logo_Run has three variants; the viewer steps them. */
void Logo_SetVariant(s32 v);
s32  Logo_Variant(void);
int  Logo_LineCount(void);

/* OURS - one key=value line saying where the demo is, keyed on the
 * demo's own global names. Read only, so a host may call it while
 * stopped. docs/subsystems/state.txt. */
void Parts_StateLine(char *out, int cap, s32 tick, int part, int mark);

#endif /* RELIC_PARTS_H */
