# Concept: display sync while scrolling (RadeonGL), and its launcher choice

Status: concept for review, 20.11. night. Builds on
`concept-smooth-scrolling.md` (branch `smooth-gl`); the launcher part
would go on its own branch `display-sync` after review. Nothing pushed.

## The ask

The user, 19.11. night: vsync while scrolling under RadeonGL (gl-plan 5h,
"Vsync while scrolling"); 20.11. night: work through the backlog.

## What is true today (MiniGL, 20.11. night, and the code)

- Zod always asks RadeonGL for three panned buffers (sdl_screen.cpp sets
  `RadeonGLFlip=pan` for the context, restores the user's value at the
  end) and calls `mglEnableSync(TRUE)` for players.
- On the pan path RadeonGL ignores `mglEnableSync`: every pan goes through
  ScrollVPort and lands on a blank, at most one switch a refresh. Since
  RadeonGL 0.15 (cure B: the first frame into a left third waits for the
  beam) this is tear-free -- the user's blind eye test confirmed it for
  GLQuake (0.15, 14.11.).
- So **Zod under RadeonGL 0.15+ does not tear while scrolling** already.
  What the eye sees instead is uneven motion -- the subject of the smooth
  concept: frames shown for 2, 1, 3 refreshes, the view stepping by the
  clock.
- The installed 0.16 still holds some pans too early (PanPace's
  early-hold defect: 252 of the first 600 presents of a tour held 8 ms or
  more, 20.11. 21:30). Fixed in RadeonGL main (unreleased); releasing it is
  the user's call (RadeonGL session).
- The copy path (`RadeonGLFlip=copy`) honours `mglEnableSync`: with sync
  it copies behind the beam (GLQuake: 29.9 against 32.2 fps at 720p),
  without it it tears. Zod never uses it today.
- PiStorm3D (no pan below VideoCore.card 2): Zod's own present, two halves
  of one screen panned with its own sync (zod_present.cpp) -- a separate
  path, unchanged here.

## What is left to do

1. **A choice in ZodLaunch** (Display group, active with Renderer =
   MiniGL, ghosted otherwise -- Style Guide: ghost what cannot apply):

   ```
   +- Display ------------------------------------------------------+
   |       Resolution  [ 640x480                               |v]  |
   |         Renderer  [ MiniGL if installed (RadeonGL, PiStorm3D)|v]|
   |     Display sync  [ Panned, tear-free (RadeonGL 0.15+)    |v]  |
   |                   [x] Smooth scrolling: even steps, frames    |
   |                       held for whole refreshes                |
   |          Scaling  [====|=================]  3.0               |
   |       Frame rate  [x] Show frames per second, top left        |
   |        Cutscenes  [x] Play the films between missions         |
   +----------------------------------------------------------------+
   ```

   Display sync entries:
   - "Panned, tear-free (RadeonGL 0.15+)" -- default; `RadeonGLFlip=pan`.
   - "Copied at the blank (older RadeonGL, slower)" -- `RadeonGLFlip=copy`,
     sync on: tear-free on libraries before 0.15.
   - "Copied at once (fastest, tears)" -- `RadeonGLFlip=copy`, sync off:
     for measuring and for players who prefer speed.
   Smooth scrolling: on = `ZOD_SMOOTH=scroll`, off = as today. Default:
   off until the user's eye test says otherwise, then on.
2. **How the launcher hands them over**: two new game options, so they
   stand in the start line, the preview and the game's log like the other
   settings (the launcher's rule: "dann steht sie in der Vorschau und im
   Spielprotokoll"): `-Y pan|blank|now` (display sync) and `-X on|off`
   (smooth). Free letters; the game maps them onto the existing switches
   (RadeonGLFlip for its context, ZOD_GL_SYNC, ZOD_SMOOTH), the environment
   variables keep working for tests and override nothing set on the line.
3. **The library release** with the PanPace fix -- the user's call, in
   the RadeonGL session; Zod works with 0.16 meanwhile (the early holds
   cost ~5 ms a frame in the first ~600 frames).
4. **A tear check on Hamiller while scrolling** (eye test, morning): pan
   vs. copy-at-once, orig04 tour, blind.

## Not done here

- No change to the pan path itself (MiniGL's library).
- No new sync for the software renderer: cego's 8-bit RTG path copies
  dirty blocks to the screen without a beam wait; vsync there belongs to
  the software-RTG smooth phase (on-card blit + the RTG board's vblank).

## Open questions (recommendation first)

1. Option letters **-Y / -X** -- or longer names through the settings file.
2. Smooth default **off until the eye test**, then on.
3. Display sync default **Panned** -- or "Copied at the blank" when the
   library is older than 0.15 (the game could read the library's version
   and say so in its log).

## Eye tests (morning)

- Tearing: panned vs. copied at once, orig04 tour (blind).
