/* RELIC - the Sprout octree: the object the scene's light is made of.
 *
 * ORIGIN 0x002d6a74 maybe_Sprout_LoadAndBuild. See
 * effects/approach.txt section 4.
 */

#ifndef RELIC_SPROUT_H
#define RELIC_SPROUT_H

#include "relic.h"

/* A node is a branch or a leaf, never both. THE DEMO'S BUILT LAYOUT
 * IS DELIBERATELY NOT COPIED - approach.txt section 4.3. */
typedef struct Relic_SproutNode {
	struct Relic_SproutNode *child[8];  /* NULL where absent        */
	u8  colour[PX_BYTES];               /* leaf: 0RGB, >> 4 each    */
	s8  normal[3];                      /* leaf: the shading normal */
	u8  is_leaf;
} Relic_SproutNode;

/* Parses Fsprout and builds the two shading tables. Needs relic.dat
 * open: unlike every other asset the port reads, this one is not in
 * the executable. Non-zero when the chunk is absent, which the scene
 * survives. */
int  Sprout_Init(void);
int  Sprout_Ready(void);
void Sprout_Free(void);

/* Three product-table lookups summed, then the ramp. Both tables are
 * procedural - approach.txt section 4.4. */
s32  Sprout_Shade(const s8 *normal, const s32 *light_fx);
const u8 *Sprout_ShadePixel(s32 shade);

/* The four columns of a node, as child slots. The occupancy index's
 * bit c selects slot COL_SLOT[c], and the far y-slab is two slots on
 * - approach.txt section 4.6. */
extern const int Sprout_ColSlot[4];

/* One sector's span geometry, all of it read off the filler. Returns
 * zero when the sector does not draw: its quadrant faces away, its
 * slope is out of range, or the span is empty. */
typedef struct Relic_SproutSpan {
	s32 slope;          /* tan in 13-bit fixed                      */
	s32 x0;             /* the strip's first column, in pixels      */
	s32 pixels;         /* how many across                          */
	s32 rows;           /* how many down                            */
	s32 step_pixel;     /* added to the interval base per pixel     */
	s32 step_row;       /* added to the cross-axis per row          */
} Relic_SproutSpan;

int  Sprout_Span(Relic_SproutSpan *out, s32 sector, s32 angle,
                 s32 scale, const s32 *corner);

/* One pixel of the light: cast a ray through the octree and shade the
 * leaf it meets, or NULL where it meets none. u sweeps the sector's
 * axis and v the shared row axis, both over [-0x1000, +0x1000]. */
const u8 *Sprout_Pixel(s32 sector, s32 u, s32 v, s32 slope,
                       const s32 *light_fx);

/* The whole object at a screen cell. cx, cy is the cell
 * maybe_Light_DrawIfOnScreen computes, scale its perspective radius,
 * angle the view angle. Returns how many pixels it wrote. */
int Sprout_Draw(Relic_Frame *f, s32 cx, s32 cy, s32 scale, s32 angle,
                const s32 *light_fx);

/* The longest unbroken run of hits along a row. NOT A DEFECT METRIC -
 * a span is a bounding extent and the demo does not fill it either;
 * compare only against a measured run. approach.txt 4.4.10. */
s32  Sprout_ProbeRun(s32 sector, s32 slope, s32 v, s32 pixels);

/* What the parse found, for a host that wants to say so. */
void Sprout_Stats(char *out, int cap);

#endif /* RELIC_SPROUT_H */
