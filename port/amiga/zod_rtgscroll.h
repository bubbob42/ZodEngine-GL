#ifndef ZOD_RTGSCROLL_H
#define ZOD_RTGSCROLL_H
/*
 * Scrolling on the card for the software RTG path
 * (docs/concept-smooth-scrolling.md, "Software RTG"): when the map view
 * moved, SDL_Flip first moves the map area's screen content with one
 * BltBitMap inside the screen bitmap (the board's blitter, no bus traffic),
 * then sends only the blocks that differ from the moved previous frame.
 * ZOD_RTGSCROLL=ein switches it on (off by default until measured).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ZMap::DoRender, every frame: the view's map offset and its screen rect */
void zod_rtgscroll_view(int sx, int sy, int rx, int ry, int vw, int vh);

#ifdef __cplusplus
}

/* SDL_Flip: this frame's scroll (1 = use it), and the last frame's view
 * forgotten when not (every flip calls this once) */
int zod_rtgscroll_plan(int w, int h, int *dx, int *dy, int *rx, int *ry, int *vw, int *vh);

/* ZOD_RTGSCROLL is on (the previous frame is kept) */
int zod_rtgscroll_on(void);

/* the previous frame's pixels (what the screen shows), w x h, pitch w;
 * 0 when there is none yet */
unsigned char *zod_rtgscroll_prev(int w, int h, int *valid);
void zod_rtgscroll_prev_set_valid(int v);

/* statistics */
void zod_rtgscroll_count(unsigned long sent_blocks, unsigned long kept_blocks);
void zod_rtgscroll_report(void);
#endif

#endif
