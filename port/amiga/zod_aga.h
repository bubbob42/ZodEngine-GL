#ifndef ZOD_AGA_H
#define ZOD_AGA_H
/*
 * The AGA screen (docs/concept-aga.md): PAL HiRes Laced 640x512, 8
 * bitplanes; the engine's 640x480 compose surface centred on it; the dirty
 * blocks of each frame through chunky-to-planar into the shown bitmap.
 */
#include <SDL/SDL.h>
#include <intuition/screens.h>

/* ZOD_AGA set (not "aus"): the AGA screen instead of RTG */
int zod_aga_wanted(void);

/* the screen for a w x h compose surface; 0 when the chipset or the mode
 * is not there (the log says why). *dy: the rows above the picture. */
struct Screen *zod_aga_open(int w, int h, int *dy);

/* the frame's dirty blocks into the screen's planes (the dirty marks are
 * swept and cleared as SDL_Flip does on RTG) */
void zod_aga_flip(SDL_Surface *compose, struct Screen *scr, int dy);

/* before CloseScreen: the screen's own bitmap back, the buffers freed */
void zod_aga_close(struct Screen *scr);

/* the report line at the end */
void zod_aga_report(void);

#endif
