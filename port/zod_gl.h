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
 * Small loaded images are packed into shared atlas pages; the map surface
 * goes through one wrap-around texture. Positions are computed on the CPU
 * (no matrix stack per sprite), and quads that follow each other on the same
 * texture share one glBegin/glEnd, whatever their colour or alpha. Rotated and scaled sprites are placed exactly where the
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

/* quads and fills drawn so far (a census can tell whether a call drew) */
unsigned long zod_gl_draw_count(void);

/* E-clock ticks spent so far in the library's glEnd and in texture uploads:
 * where it draws and waits for the card (never cleared) */
unsigned long zod_gl_lib_ticks(void);

/* a kept HUD (ZOD_HUDKEEP): count draws into x > gx or y > gy while on */
void zod_gl_hud_guard(int on, int gx, int gy);
unsigned long zod_gl_guard_hits(void);

/* frames presented so far (zod_gl_flush): which of the buffers this is */
unsigned long zod_gl_frame_no(void);

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

/* s is a loaded image that stays as it is: if it is small, it goes into a
 * shared atlas page on its first draw, so the quads of different images
 * share one texture and one batch (ZSDL_Surface::LoadBaseImage calls it) */
void zod_gl_hint_atlas(SDL_Surface *s);

/* draw the rectangle src (0 = all) of surface s with its top-left at x, y,
 * unscaled; alpha 255 = opaque. hit = the white hit-flash silhouette. */
void zod_gl_blit(SDL_Surface *s, const SDL_Rect *src, int x, int y,
                 Uint8 alpha, int hit);

/* draw surface s rotated by angle degrees (counter-clockwise on screen) and
 * scaled by size, centred at cx, cy */
void zod_gl_blit_rot(SDL_Surface *s, float cx, float cy, float size,
                     float angle, Uint8 alpha, int hit);

/* the same with cos(angle) * size and sin(angle) * size already worked out
 * (by the caller, who needed them for the size anyway) */
void zod_gl_blit_rot_cs(SDL_Surface *s, float cx, float cy, float ca, float sa,
                        Uint8 alpha, int hit);

/* an untextured rectangle */
void zod_gl_fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b);

/* show a whole software-drawn surface (loading and statistics screens):
 * sent again every call */
void zod_gl_show_surface(SDL_Surface *s);

/* the same, with only x, y, w, h of it changed since (the rest of its
 * texture stays): the load progress bar */
void zod_gl_show_surface_teil(SDL_Surface *s, int x, int y, int w, int h);

/* counters for the end of a -B run */
void zod_gl_report(void);

#endif
