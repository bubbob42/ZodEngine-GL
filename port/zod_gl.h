/*
 * Drawing through minigl.library (RadeonGL, PiStorm3D) behind ZSDL_Surface.
 *
 * The engine keeps its own surfaces (8 bit with the shared palette, or the
 * port's other formats); a surface gets a GL texture the first time it is
 * drawn, found again by the surface's address. Large surfaces are split
 * into tiles of at most 1024x1024, every tile a power of two in each
 * direction, so the path also works on libraries without non-power-of-two
 * textures.
 *
 * Positions are computed on the CPU (no matrix stack per sprite), and quads
 * that follow each other with the same texture and alpha share one
 * glBegin/glEnd. Rotated and scaled sprites are placed exactly where the
 * software path puts its rotozoom copy, so a frame drawn either way can be
 * compared.
 *
 * Only built when DISABLE_OPENGL is not set (the Amiga build).
 */
#ifndef ZOD_GL_H
#define ZOD_GL_H

#include <SDL/SDL.h>

/* 1 while a GL context draws the frames (set by the screen layer) */
extern int zod_gl_active;

/* the drawing state for a w x h context; call after every context open */
void zod_gl_init(int w, int h);

/* draw everything pending; the screen layer calls it before the flip */
void zod_gl_flush(void);

/* clear the frame to a colour */
void zod_gl_clear(Uint8 r, Uint8 g, Uint8 b);

/* s was drawn into (x, y, w, h): its texture follows on the next draw,
 * for that rectangle only. The SDL layer calls it for every off-screen
 * blit and fill. */
void zod_gl_changed(SDL_Surface *s, int x, int y, int w, int h);

/* s is freed: its texture goes too (SDL_FreeSurface calls it) */
void zod_gl_forget(SDL_Surface *s);

/* the shared palette changed: every texture is made again on its next
 * draw (8-bit surfaces carry no colours of their own) */
void zod_gl_drop_all(void);

/* draw the rectangle src (0 = all) of surface s with its top-left at x, y,
 * unscaled; alpha 255 = opaque. hit = the white hit-flash silhouette. */
void zod_gl_blit(SDL_Surface *s, const SDL_Rect *src, int x, int y,
                 Uint8 alpha, int hit);

/* draw surface s rotated by angle degrees (counter-clockwise on screen) and
 * scaled by size, centred at cx, cy */
void zod_gl_blit_rot(SDL_Surface *s, float cx, float cy, float size,
                     float angle, Uint8 alpha, int hit);

/* an untextured rectangle */
void zod_gl_fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b);

/* show a whole software-drawn surface (loading and statistics screens):
 * sent again every call */
void zod_gl_show_surface(SDL_Surface *s);

/* counters for the end of a -B run */
void zod_gl_report(void);

#endif
