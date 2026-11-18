/*
 * Draw census -- what the software renderer draws, counted as input for the
 * MiniGL renderer's design (texture atlas pages, batches, texture uploads).
 *
 * Off unless the environment variable ZOD_CENSUS is set (any value except
 * "0" or "off"). Off, every hook costs one test of zod_census_on. On, the
 * counters are reported at the end of a benchmark run (-B) next to the
 * frame-time sections.
 *
 * Terms used in the report:
 *   group    the asset folder an image was loaded from (assets/<group>/...):
 *            the natural unit for one atlas page set.
 *   derived  a rotated/scaled copy (rotozoom) or a hit-flash silhouette of a
 *            loaded image. Under GL these are the same texture drawn
 *            differently, so they count under their base image's group.
 *   dynamic  a surface the engine built at run time (the map, text, the
 *            minimap). Under GL each is its own texture.
 *   screen   draws whose destination is the screen surface; all other draws
 *            change an off-screen surface and become texture updates.
 */
#ifndef ZOD_CENSUS_H
#define ZOD_CENSUS_H

#include <SDL/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int zod_census_on;

void zod_census_init(void);
void zod_census_image(SDL_Surface *s, const char *filename);
void zod_census_derived(SDL_Surface *s, SDL_Surface *base, int kind);
void zod_census_blit(SDL_Surface *src, SDL_Surface *dst, int w, int h);
void zod_census_fill(SDL_Surface *dst, int w, int h);
void zod_census_free(SDL_Surface *s);
void zod_census_frame(void);
void zod_census_report(void);

#define ZOD_CENSUS_ROTO 1
#define ZOD_CENSUS_HIT  2

#ifdef __cplusplus
}
#endif

#endif
