#ifndef ZOD_SMOOTH_H
#define ZOD_SMOOTH_H
/*
 * Smooth scrolling under RadeonGL's panned buffers
 * (docs/concept-smooth-scrolling.md).
 *
 * ZOD_SMOOTH=scroll (default since the user's eye test, 21.11.) | aus |
 * always | messen; -X on|off from the launcher. With it on, the GL
 * screen's blank clock is measured once when the screen opens; then every
 * frame is held for N refreshes while the view moves (scroll) or always,
 * and the view's scroll clock is the blank the frame will be shown on.
 * "messen" measures the clock and reports the judder without changing
 * anything. ZOD_SMOOTH_N = 1..4 fixes N (default: chosen from the frames'
 * work).
 */

/* after the GL context is up (not on the own present): measures the blank
 * clock when ZOD_SMOOTH asks for it, and hooks the scroll clock in */
void zod_smooth_open(void);

/* the GL screen goes: the timer, the hooks */
void zod_smooth_close(void);

/* right before mglSwitchDisplay: the wait for the blank, the bookkeeping */
void zod_smooth_before_switch(void);

/* right after mglSwitchDisplay returned */
void zod_smooth_after_switch(void);

/* the report lines at the end of a run */
void zod_smooth_report(void);

#endif
