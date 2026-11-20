# Concept: smooth scrolling (GL first, then software RTG; AGA with the AGA item)

Status: concept for review, written 20.11. night; implementation on the
local branch `smooth-gl`, behind a switch that is off by default. Nothing
pushed. Eye test in the morning (see "Eye tests" at the end).

## The ask

The user, 19.11. 23:30: "soft scrolling for gl -- should be possible with
AGA and software RTG, too". 20.11. night (relayed): start on the backlog
items, smooth scrolling under GL first.

## What the game does now

- `ZPlayer::ProcessScroll` (zplayer.cpp), once per frame before drawing:
  for a held arrow key or the pointer at a screen edge, the view moves by
  `(now - last) * 400 px/s`, whole pixels, the fraction carried to the next
  frame. ZOD_TOUR uses the same rule. `now` is the E-clock time at the
  start of the frame's player tick.
- Under GL the view's pixel position is exact already: the map is one
  texture (orig04: 1376x1760), a different position only moves its texture
  coordinates; units, effects and the HUD are quads at whole pixels.
- RadeonGL shows the frame through three panned buffers; with sync on (the
  players' default; ZOD_GL_SYNC=aus only in benchmarks) a pan waits for the
  card's blank. Hamiller's 640x480 mode runs at 60 Hz (the library's line
  model: ~32.8 us a line, a refresh ~16.7 ms).
- Frame times while scrolling on Hamiller: ~30..34 ms (29.6 fps on orig04's
  tour, sync off), with longer frames in fights and path searches.

## Why it judders

A frame is drawn at the position of its START time and shown at the first
blank after its END. Two things vary from frame to frame:

1. How many refreshes a frame stays on screen. At ~30 ms of work on 60 Hz
   the frames land on a 16.7 ms grid: shown for 2, 2, 1, 2, 2, 3 ...
   refreshes, depending on where each one's end falls.
2. The step between frames, because it is taken from the clock at the
   frame's start, not from the time it is shown.

On screen the map then moves by 13, 13, 7, 13, 20 ... pixels per shown
frame while each stays up for a different time: the eye sees speed
changes, the classic judder. Doubling the frame rate would not remove it;
an EVEN rhythm does.

```
refresh   |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  8  |  9  | 10  |
now       [ f1      ][ f2      ][f3 ][ f4           ][ f5      ][ f6  ...
step px      13        13        7     20              13
smooth    [ f1      ][ f2      ][ f3      ][ f4      ][ f5      ][ ...
step px      13        13        13        13          13
```

## What smooth needs (any renderer)

1. **A fixed rhythm**: every frame stays on screen for exactly N refreshes
   (N = 2 at 60 Hz -> 30 fps; N = 3 -> 20 fps; at 50 Hz N = 2 -> 25 fps).
2. **A fixed step**: the view moves `speed * N / refresh rate` per frame
   (400 px/s at 30 fps: 13.33 px), not by the clock.
3. **A frame that does not fit** (a fight, a path search) shows for more
   than N refreshes; the next step then carries the missed refreshes'
   distance, so the speed over a second stays 400 px/s. Without the catch-up
   the view would slow down for that moment instead of jumping: a choice
   (open question 3).

Game logic is untouched: units, bullets and the server keep running on
wall-clock time; only the view's position is taken from the shown frames.

## GL (phase 1, branch `smooth-gl`)

### How RadeonGL shows a frame (MiniGL, 20.11. night)

- On the pan path (RadeonGLFlip=pan, three buffers) `mglEnableSync` is
  ignored: every pan goes through ScrollVPort and lands on a blank, at most
  one switch a refresh, tear-free since 0.15.
- The frame kicked in `mglSwitchDisplay` k is settled and panned to at the
  START of `mglSwitchDisplay` k+1, and is visible from the first blank
  after that: one present later than it was finished.
- `mglSwitchDisplay` returns after the kick. It blocks only in PanPace
  (1 ms timer sleeps) while the buffer it must draw into next is still on
  display -- when the game outruns the refresh. With N >= 2 refreshes a
  frame PanPace never holds.
- The installed 0.16 still has PanPace's early-hold defect (79..733 holds
  in a run); the fix is in RadeonGL main since 20.11. (d6b5236..e2a26e6,
  the same bytes as minigl-pansample c055ee5c), not released -- a release
  is the user's call. Hamiller tests of smooth scrolling run with that
  library staged (ZLIB), as the night's A/Bs did.
- Nothing public tells a client the refresh period, a blank count or when
  a pan went live. Internally the line model has an upper bound of the
  line time and the E-clock time of each pan. A small addition to the
  library's public port "RadeonGL" (lib/rglport.h) could carry the measured
  period, a blank count and the E-clock time of the last pan's first
  blank: a library change, concept and the user's word first; MiniGL has
  put it in its backlog.
- WaitTOF is no help on Hamiller: it ticks with the chipset's 50 Hz, not
  the Radeon's 60.

### The rhythm: who holds a frame for N refreshes

- **Best, later: the library.** A swap interval (OpenGL's own idea:
  `glXSwapIntervalSGI`, `wglSwapIntervalEXT`): "show this frame no earlier
  than N refreshes after the previous one", or at least the blank clock in
  the public port (above). Then Zod's own measuring below goes away.
- **Now, on Zod's side: a blank clock of its own.**
  1. *Measure it once*, when the GL screen opens (only with smooth on):
     ~1 s of empty frames switched as fast as the library takes them. The
     game then outruns the refresh, so every `mglSwitchDisplay` returns
     from PanPace within ~1 ms after a blank: a straight-line fit of the
     return times gives the period, the earliest returns give the blank's
     phase. A period outside 8..40 ms (FS-UAE's null card, a library that
     never holds) switches smooth off with a log line.
  2. *Pace with it*: frame m-1 goes on screen with the switch call after
     frame m is drawn. Before that call Zod sleeps until just before the
     blank that frame m-1 is due on (`previous shown + N * period - 2 ms`),
     then calls `mglSwitchDisplay`; the frame shows on that blank. A frame
     that took too long gets the first blank after its call, and the grid
     moves on from there.
  3. The sleep is a timer.device request in E-clock ticks (UNIT_ECLOCK),
     never a busy loop and never a raised priority (standing rule); the
     game's `uni_pause` (usleep: a 20 ms grid) is too coarse for it. The
     existing 60-fps limiter in main.cpp stays; with the rhythm on it is not
     reached (the frame budget is N refreshes).
  4. *Drift*: the measured period is good to a few us; the 2 ms margin
     lasts minutes against that, then a frame lands one blank late (a
     repeated frame) and the grid restarts from the blank it got. MiniGL
     expects exactly this "now and then" until the library tells the
     period.
- **N**: fixed by the switch (2 or 3), or chosen: start at the smallest N
  whose budget the last second's frames fit at the 90th percentile; go up
  at once when frames keep overrunning, come down only after 3 s that fit
  the smaller N. Changing N changes the step, so it happens rarely by
  design.

### The step

`ProcessScroll` keeps its rule (`(now - last) * 400 px/s`, the fraction
carried), but with smooth on, `now` is the blank the frame being drawn
will be shown on -- a display clock instead of the wall clock. At the
start of frame m the last switch call has fixed the blank of frame m-2;
frame m is due 2 * N refreshes after it. On the rhythm, the display clock
advances by exactly N refreshes a frame, so the step is the same every
frame (13.33 px at 30 fps: 13, 13, 14 with the carry). After a late frame
the clock jumps by the missed refreshes: one bigger step, the speed over
the second stays 400 px/s. The same clock goes to `StartMouseScrolling`
and the tour. Option: round the speed so the step is a whole number of
pixels (13 px at 30 fps = 390 px/s): perfectly even, 2.5 % slower (open
question 2).

### Only while scrolling?

The rhythm costs frames in a still view (Hamiller's orig01: ~50 fps would be
capped to 30). Recommendation: the rhythm only while the view moves (key,
edge, tour, the camera flight to a unit), plus 0.5 s after it stops, so a
still view keeps its full rate. Switch: ZOD_SMOOTH=scroll (this) or
ZOD_SMOOTH=always.

### What is measured (no eye needed)

Per shown frame: the refreshes it stayed up (from the library when it can
say; else the E-clock between switches, rounded to the period) and the
view's step. The report gives the share of frames shown for exactly N, and
the spread of pixels per refresh (now vs. smooth): the judder in numbers.
Baseline before any change (Hamiller, 20.11. 21:30, gl n17, stock RadeonGL
0.16, orig04 tour, sync on, 60 s): 27.5 fps; mglSwitchDisplay 5.3 ms a
present on average, 252 presents of 8 ms and more in the first 600 frames
(PanPace's early holds, fixed in RadeonGL main, not released) and none
after. The judder in numbers needs the blank clock: ZOD_SMOOTH=messen on
the same run gives it.

### Switches

- `ZOD_SMOOTH` = aus (default for the night) | scroll | always | messen
  (measure the blank clock and report the judder, change nothing: the
  "before" numbers with the same instrument).
- `ZOD_SMOOTH_N` = 1..4 fixed, else chosen from the frames' work.
- `ZOD_SMOOTH_HZ` = a nominal refresh instead of measuring -- for FS-UAE,
  whose null card holds no pan (the measuring then switches smooth off).
- ZOD_GL_SYNC does not matter here: on the pan path RadeonGL ignores
  mglEnableSync (above).
- Later a ZodLaunch choice next to "Display sync" (the vsync item).

## Software RTG (phase 2, own branch)

From gl-plan 5h: scroll on the card instead of copying the whole view. The
part that stays visible moves with an on-card blit (BltBitMap VRAM to VRAM:
the card's blitter on most RTG boards, no bus traffic); only the newly
shown strip (13 px x 480 at 30 fps) and the changed blocks of units and
effects go over the bus. The compose surface in fast RAM shifts the same
way (a memmove) so the dirty-block sums stay valid; the HUD is outside the
blit. The same rhythm and step as GL, with WaitTOF/the RTG library's own
vblank wait for the blank where the board has one.

### Built (20.11. night, branch rtg-scroll, ZOD_RTGSCROLL, off by default)

port/amiga/zod_rtgscroll.cpp: ZMap::DoRender reports the view's offset and
screen rect each frame; when it moved within the same rect, SDL_Flip moves
the map area's screen content with one BltBitMap inside the screen bitmap
before the lock, then compares each dirty 16x16 block with the previous
frame (kept in fast RAM, following the same dirty runs) at the moved place
and sends only the blocks that differ. No compose-surface shift is needed:
the comparison is against the previous frame directly.

FS-UAE RtgScrollCheck (software, ZOD_FLIP_SUMS pruefen reads the whole
screen back after every flip): orig04 tour 0 differ; 1087 of 1103 flips
moved the map on the card, 69 % of the dirty blocks kept; 94 196 bytes a
frame to the screen instead of 304 645 (-69 %), but 791 copy calls a frame
instead of 31 (block spans). orig01 still: no scroll frames, 0 differ.
FS-UAE's card makes bytes nearly free, so its times say nothing; on the
A4000D's bus (~120 ns a byte into card memory, measured) ~36 ms of copying
would become ~11 ms plus the calls and a ~300 KB compare in fast RAM. An
A/B on a real RTG Amiga decides the default (A4000T, A4000D without GL).

## AGA

With the AGA item (PAL HiRes Laced): ScrollVPort over a larger planar
buffer, the exposed strip through c2p -- described there.

## Open questions (recommendation first)

1. N at 60 Hz with Hamiller at ~30 fps scrolling: **auto** (30 where it
   holds, 20 in heavy fights) -- or fixed 3 (20 fps, always even), or a 50 Hz
   screen mode (25 fps, N = 2) where the monitor takes one.
2. Step: **carry fractions** (exact 400 px/s, 13/13/14 px) -- or a whole
   pixel step (390 px/s, perfectly even).
3. A frame that overruns: **catch up** (speed stays 400 px/s, one bigger
   step) -- or slow down for that moment.
4. Rhythm only while scrolling: **yes** (still view keeps its rate) -- or
   always.
5. Library: **a swap interval in RadeonGL** (MiniGL), Zod's own pacing as
   the fallback for libraries without it (PiStorm3D, older RadeonGL).

## Eye tests (morning)

- Smooth on/off, blind A/B, orig04, scrolling by keys and by the edge.
