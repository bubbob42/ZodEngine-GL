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

/* The bottom panel on a screen of its own (soft scrolling; ZOD_AGA_SPLIT):
 * its window shares the main window's message port (open after the main
 * window, close before it); the pointer, the palette and the mouse go to
 * both. zod_aga_split_row: the picture's row where the panel begins, 0
 * without the split; zod_aga_mouse: the pointer on the picture from the
 * map screen; zod_aga_raw_y: a picture row as the map screen's row. */
#include <intuition/intuition.h>
void zod_aga_panel_window_open(struct Window *main, UWORD *pointer);
void zod_aga_panel_window_close(void);
struct Window *zod_aga_panel_window(void);
void zod_aga_panel_pointer(UWORD *pointer, int on);
void zod_aga_panel_palette(ULONG *table);
int  zod_aga_split_row(void);
void zod_aga_mouse(struct Screen *map, int *x, int *y);
int  zod_aga_raw_y(int y);

/* after CloseScreen: frees the large bitmap of hardware scrolling, which
 * the screen used and does not free itself */
void zod_aga_closed(void);

/* the report line at the end */
void zod_aga_report(void);

#endif
