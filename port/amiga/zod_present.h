#ifndef ZOD_PRESENT_H
#define ZOD_PRESENT_H
/*
 * zod_present -- frames without tearing on PiStorm3D, which cannot flip in
 * sync by itself (see zod_present.cpp). Only used with the GL renderer.
 */

#include <intuition/intuition.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1: the library is PiStorm3D without a synced flip of its own (no
 * VideoCore.card 2+ loaded) and ZOD_GL_PRESENT is not "aus" */
int zod_present_wanted(void);

/* An own w x 2h BGRA32 screen, a bitmap the GL context draws into, and a
 * window over both halves for input. 1: open, the context is current;
 * 0: nothing is left open (the caller takes the library's own screen). */
int zod_present_open(int w, int h);

/* after mglDeleteContext: screen, window, bitmap and libraries */
void zod_present_close(void);

int zod_present_active(void);
struct Window *zod_present_window(void);

/* After mglSwitchDisplay: the frame into the half not on display, then the
 * display panned onto it (waits for the blank). sync 0: into the half on
 * display, no wait (benchmarks; tears). */
void zod_present_show(int sync);

#ifdef __cplusplus
}
#endif

#endif
