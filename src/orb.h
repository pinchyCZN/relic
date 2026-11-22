/* RELIC - the cracked sphere, and the fullscreen affine fill.
 *
 * Both the release (part 2.12) and the Earth shot (2.12b) draw the
 * same orb with the same crack network, and both fill the frame from
 * a texture through the same affine walk. The two scenes differ only
 * in the parameters they hand in, so the machinery lives here rather
 * than in either part.
 *
 * See docs/effects/orb_release.txt and docs/effects/earth.txt.
 */

#ifndef RELIC_ORB_H
#define RELIC_ORB_H

#include "relic.h"

/* Loads Fcrust and the TDDD crack network. Non-zero on failure. */
int Orb_Init(void);

/* ORIGIN 0x00228b6c g_SinTable - 8192 longwords, amplitude 0x4000. */
s32 Orb_Sin(s32 i);
s32 Orb_Cos(s32 i);

/* ORIGIN 0x002d4258 Vec_Rotate2Axis - (x,y) by ay, then (y,z) by ax. */
void Orb_Rotate2Axis(s32 *x, s32 *y, s32 *z, s32 ay, s32 ax);

/* ORIGIN 0x002d1fdc Orb_DrawTexturedSphere. `reveal` is the demo's
 * (g_FlareGrow >> 8) >> 1: a texel is drawn only when its B channel
 * clears it, so the sphere arrives brightest-first. Pass 0 once the
 * reveal is spent. */
void Orb_DrawSphereTo(u8 *dst, s32 w, s32 h, s32 scale,
                      const u8 *tex, s32 cx, s32 cy, s32 radius,
                      s32 reveal, int gated, s32 ang_a, s32 ang_b);
void Orb_DrawSphere(Relic_Frame *f, s32 cx, s32 cy, s32 radius,
                    s32 reveal, s32 ang_a, s32 ang_b);

/* ORIGIN 0x002cdc3c Crack_DrawShells. A point survives only when both
 * its sine lookups clear `threshold` - zero through the release, and
 * climbing through the Earth shot, which is what closes the seams. */
void Orb_DrawShells(Relic_Frame *f, s32 sx, s32 sy, s32 radius,
                    s32 phase_a, s32 phase_b, s32 threshold,
                    s32 ang_a, s32 ang_b, s32 scroll);

/* OURS - how many shell quads the last Orb_DrawShells submitted, and
 * how many points survived its mask. For the parts' stats lines. */
s32 Orb_ShellsDrawn(void);
s32 Orb_PointsLive(void);

/* ORIGIN 0x002d3f5c Warp_FullscreenAffine and 0x002d3e94
 * Warp_FullscreenRotate - one routine, two corner transforms. With
 * `scaled` the corners are scaled by pb >> 8 before the rotation
 * (0x002d417c); without it they are rotated only and pb is ignored,
 * because 0x002d4132 overwrites it with the cosine it loads. */
void Warp_Fill(Relic_Frame *f, const u8 *tex, s32 pa, s32 pb,
               int scaled);

/* ORIGIN 0x002d20fa Space_DrawSunBlob - a textured sphere written
 * into LIGHT alone, two longitudes of `tex` summed with a byte add
 * that wraps. ang_b biases the longitude once, ang_a twice.
 * effects/earth.txt 2.1. */
void Orb_DrawSunBlob(Relic_Frame *f, const u8 *tex, u32 tex_len,
                     s32 cx, s32 cy, s32 radius, s32 ang_a,
                     s32 ang_b);

void Orb_Rotate2D(s32 x, s32 y, s32 pa, s32 *ox, s32 *oy);


#endif /* RELIC_ORB_H */
