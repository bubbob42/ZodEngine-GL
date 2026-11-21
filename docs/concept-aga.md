# Concept: AGA -- PAL HiRes Laced (and the flicker-free AGA modes)

Status: concept for review, 20.11. night. Implementation on the local
branch `aga` (development in FS-UAE's AGA emulation; the A1200 at .31 did
not answer this evening). Nothing pushed. Picture checks wait for the
user's eye (list at the end).

## The ask

The user, 19.11. night: AGA, PAL HiRes Laced. cego's README: "AGA is NOT
supported yet ... Everything the engine draws already goes through one
shared 256 colour palette, which is the hard part. What is missing is an
AGA screen and the copy path to it. The video player already supports
AGA."

## What the software renderer does now (port/amiga/sdl_screen.cpp, sdl_video.cpp)

- The engine draws 8-bit chunky into a compose surface in fast RAM
  (640x480, one byte a pixel, indices into the shared palette).
- Drawing marks dirty blocks on a 40x30 grid of 16x16 pixels; SDL_Flip
  copies the dirty runs of each block row into the RTG screen's bitmap,
  skipping blocks whose sums did not change (the flip sums).
- The screen is an RTG screen of the size (BestCModeID with CyberGraphX
  tags, depth 8, 16 as a fallback); the palette goes on with LoadRGB32.
  Without an RTG mode the game logs "kein RTG-Modus" and stops.

## The AGA screen

- **Mode**: PAL HiRes Laced 640x512 (`PAL_MONITOR_ID | HIRESLACE_KEY`),
  8 bitplanes: AGA only (OCS/ECS have at most 4 planes in hires). The
  engine keeps drawing 640x480; the picture sits centred (16 rows above
  and below) -- the layout stays exactly as on RTG. 640x512 with 32 more
  map rows is an option later (the engine lays out from the screen size).
- **Other AGA modes**, same code, chosen by the player: Multiscan
  Productivity 640x480 and DblPAL HiRes 640x512 -- no interlace flicker,
  but a 31 kHz monitor; PAL HiRes (non-laced, 640x256) is not offered:
  the engine's 480 rows would need halving.
- **Palette**: the shared 256 colours with LoadRGB32 (AGA has 24 bits a
  colour) -- the same call as on RTG.
- **Opening**: OpenScreenTags with SA_DisplayID, SA_Depth 8,
  SA_Interleaved FALSE (c2p writes plane by plane), SA_Quiet, a backdrop
  borderless window as on RTG. The mode is checked with ModeNotAvailable
  first; without AGA the game says so and stops (as today without RTG).

## The copy path: chunky to planar by dirty blocks

- A dirty block is 16 pixels wide -- exactly one word of each of the 8
  bitplanes per row. So c2p works per block: 16x16 bytes in, 8 planes x
  16 rows x 1 word out. A run of dirty blocks in a row becomes a run of
  words per plane.
- The c2p is the classic merge (transpose 8x8 bits in registers with
  shifts and masks, then 16 bits wide), first in C, then in 68k assembler
  for the 040/060 (the loop is the cost). Writes go to chip RAM: at
  ~7..14 MB/s for an 040/060 (16/32-bit writes into the chipset's slots),
  a full frame (640x480 = 300 KB of planes) costs ~25..45 ms; a still
  view changes ~6 % of its blocks a frame (measured on the A4000T) --
  ~2..3 ms. The c2p's own CPU cost at ~1 cycle a pixel on the 060 is
  ~0.3 ms for such a frame.
- **Buffers**: phase 1 writes into the shown bitmap (one buffer, the
  simplest; a block may be shown half old, half new for one frame).
  Phase 2: two bitmaps (ChangeScreenBuffer, 2 x 320 KB chip RAM), every
  block that changed in frame k is written into both buffers over frames
  k and k+1 (a "dirty for buffer B" flag next to the sums).
- **Flip sums**: they already skip unchanged blocks -- on AGA that skip is
  worth more than on RTG (chip RAM is slower than an RTG card's bus).
- **The pointer**: the engine draws its own pointer into the compose
  surface (as on RTG). Phase 1 keeps that (its block goes through c2p each
  frame it moves). Later: the system sprite pointer (free on AGA) with the
  game's cursor image reduced to the sprite's colours.

## Scrolling on AGA

A scrolling view changes every block: full c2p, ~25..45 ms of chip writes
-- slow. The answer is hardware scrolling: a planar buffer larger than the
view, ScrollVPort over it (RasInfo RxOffset/RyOffset -- graphics.library,
no copper of our own, no register writes), only the newly exposed strip
and the changed blocks through c2p; the HUD (right edge and bottom) is
drawn again at each offset. That is the smooth-scrolling concept's AGA
phase: after the static AGA screen works.

### Milestone 4 in detail: what hardware scrolling costs here (for review)

Measured on the PiStorm's A1200: scrolling rewrites the whole view, 230 KB
of chip writes a frame, 8 fps. Hardware scrolling with system calls only
(ScrollVPort on RasInfo offsets; no copper list of our own) means:

- **A bitmap larger than the view**, holding the map around the view
  ("world-anchored": a map pixel keeps its place in the bitmap while the
  view moves over it). A chunky shadow of that bitmap in fast RAM keeps
  c2p aligned to bitmap words and keeps the pixels outside the view; block
  sums on the shadow send only what changed: the newly shown strip, units,
  effects -- and the HUD.
- **The HUD** (100 px right, 36 rows below) sits at a fixed place on
  screen, so it moves across the bitmap with the view: drawn again at
  every offset, and the map put back where it was (~67 KB a frame).
- **No wrap-around**: AGA cannot wrap a bitmap sideways, and a vertical
  wrap needs a copper split. When the view nears the bitmap's edge, the
  bitmap re-centres: one full redraw (~230 KB, ~100 ms on the PiStorm).
  With margins of 256 px sideways and 128 rows up/down (a 1152x768 bitmap,
  884 KB of chip RAM) that is every ~0.6 s of continuous scrolling at
  400 px/s.
- **One buffer**: two 884 KB bitmaps do not fit next to Workbench in 2 MB
  of chip RAM, so blocks are written into the shown bitmap (a unit may be
  seen half drawn for a frame); the still view keeps its two buffers.
- Estimate on the PiStorm's A1200: ~80 KB a frame (strip + HUD + units)
  instead of 230 KB, plus a re-centring hitch twice a second: ~20 fps
  scrolling instead of 8. On a real 040/060 card (faster chip writes) more.

Open points for the user: the chip RAM it takes (884 KB, or smaller
margins with more re-centring), one buffer while scrolling, and whether the
re-centring hitch is acceptable -- or a smaller target: a "scroll with
fewer updates" mode (the HUD and units refreshed every other frame).

## How it is chosen

- ZodLaunch, Display group, Renderer: "Software (RTG)", "MiniGL if
  installed", **"AGA (native screen)"**, plus an "AGA mode" cycle (PAL
  HiRes Laced / Multiscan Productivity / DblPAL HiRes), ghosted unless AGA
  is the renderer.
- The game: `-o` stays "software"; a new `-a <mode>` (free letter) asks for
  the AGA screen. Without the option the RTG path is tried first and AGA
  is the fallback when there is no RTG mode at all (an AGA machine without
  a card then starts instead of stopping).

## Development and tests

- FS-UAE with an AGA chipset and no RTG card (a config of its own,
  `fsuae/local/ZodEngine-autotest-aga.fs-uae`: A4000/040, AGA, 2 MB chip,
  fast RAM, the same system and DEV: as the RTG autotest).
- Checks: the screen opens (mode, depth, palette), a still bot match runs
  and a dumped frame of the planar bitmap equals the compose surface
  through the palette (a planar-to-chunky read-back in the test, the same
  idea as ZOD_FLIP_SUMS=pruefen: "0 differ"); the software RTG tests stay
  green.
- Real hardware: the A1200 (.31) when it answers; the user's eye for
  flicker and colours.

## Milestones

1. AGA screen + c2p of dirty blocks into the shown bitmap; FS-UAE
   read-back check -- DONE 20.11. night (branch aga): port/amiga/zod_aga.cpp,
   ZOD_AGA, and the fallback when there is no RTG mode;
   tests/auto/AgaCheck on the Makefile's new AGA machine (the 040 machine
   without its RTG card): orig01 still, 774 frames, 4850 of 19200 groups a
   frame through c2p, 0 pixels differ in every frame read back; orig04
   tour (the fallback), 595 frames, 18838 groups a frame (the whole view),
   0 differ. SwSums (software RTG) still 0 differ.
2. Double buffering; c2p in assembler. -- Double buffering DONE 20.11.
   night (ZOD_AGA_DBUF, default on): AllocScreenBuffer + ChangeScreenBuffer
   with the safe/display messages; per-buffer block sums; a block whose
   pixels the other buffer does not hold is marked stale there (also when
   this buffer already holds them -- the first version missed the pointer
   leaving a place: 200 pixels differed; since then 0). FS-UAE AgaCheck: 0
   pixels differ in every frame of both runs. On PAL HiRes Laced a buffer
   change waits for a whole frame: the game shows at most 25 frames a
   second there (the display's own rate). Block sums (ZOD_AGA_SUMS, default
   on): the still view converts ~650 instead of 4850 groups a frame. The
   c2p in assembler waits for a real AGA machine to measure on.
3. The flicker-free modes; ZodLaunch choice. -- DONE 20.11. night: the
   game's -C pal|dblpal|prod (the screen size from the mode's nominal
   dimensions; a mode without its monitor driver falls back to PAL HiRes
   Laced, logged); ZodLaunch's Renderer gets "AGA (native screen)" and an
   "AGA mode" choice, ghosted otherwise; the start line -o -C <mode>;
   tests/launcher passes; ZodLaunch builds (cego's Docker toolchain).
   FS-UAE's test system has no DblPAL/Multiscan driver: the fallback is
   tested, the two modes themselves wait for a real AGA machine.
4. Hardware scrolling (with the smooth-scrolling concept). -- FIRST VERSION
   21.11. night (local branch aga-hwscroll, ZOD_AGA_HWSCROLL, off by
   default; see "Milestone 4: first results" below).

## Milestone 4: first results (21.11. 01:20-01:46)

Built as the section above describes, with two changes found on the way:

- **A shadow per buffer instead of block sums.** Each buffer keeps a
  chunky copy (fast RAM) of exactly what its planes hold. Every frame the
  picture is compared into it at the window's place, 16 bytes at a time;
  only the 16-pixel groups that differ go through c2p. That covers the new
  strip, units, effects and the HUD moving across the bitmap without any
  of them being special cases, and a re-anchor is just a jump of the
  offset. Colour 0 for the rows beside a smaller picture.
- **Anchored again near the edge the view came from**, not in the middle:
  the whole width of both margins lies ahead (a quarter margin kept behind
  for a small step back). FS-UAE: re-anchors 73 -> 42 in a 30 s tour,
  groups a frame 10 620 -> 8 960.

Correctness (FS-UAE AgaHwScroll, ZOD_AGA_CHECK every frame: the shadow's
window against the picture, every plane group under the window against the
shadow): 0 pixels differ in all four runs -- tour with two buffers, one
buffer, margins 64x16 (anchored again 80..129 times), still view.

Speed on real AGA (the PiStorm's A1200, chip RAM over its own bus,
orig04 tour, software, -C pal 640x512, 30 s; ABBA):

| run | fps | groups a frame through c2p | work a frame |
|---|---|---|---|
| without (today's path) | 7.8 / 7.8 | 15 480 | 109 ms |
| two buffers, margins 192x64 (2 x 640 KB chip) | 10.9 / 10.9 | 10 048 | 77 ms |
| one buffer, 192x64 (640 KB) | 16.6 | 6 608 | 51 ms |
| two buffers, 320x64 (2 x 800 KB) | 12.3 | 8 782 | 67 ms |
| one buffer, 448x64 (960 KB) | 17.4 | 6 218 | 49 ms |
| still view, two buffers, 192x64 | 48.8 | 260 | 3.3 ms |

Where the remaining groups come from (one buffer, 192x64): re-anchors
~1 700 a frame (43 in 519 frames, each the whole window), the HUD ~4 400
(the right panel, 100 px x 476 rows, and the bottom 36 rows move across the
bitmap with the view: its new place and the map put back at its old one),
the new strip ~550. With two buffers each buffer is two frames behind, so
the HUD's and the strip's share doubles, and every re-anchor costs both
buffers. The HUD is inherent to a hardware-scrolled 8-bit screen (no dual
playfield with 256 colours, no sideways viewport split); a bottom HUD in a
viewport of its own could save its 36 rows (~1 400 groups) later.

For the user (eye tests on an AGA monitor, voltago or the A1200):
- One buffer is ~1.5x faster but writes into the shown bitmap: the HUD
  can be seen at its new place a few pixels off for part of a frame
  (it moves across the bitmap each frame). Two buffers never show that.
- Chip RAM: 640 KB a buffer at 192x64; wider margins buy little (the HUD
  dominates). Recommended default when switched on: two buffers, 192x64;
  one buffer as the faster choice if the eye accepts it.

How it is chosen (21.11. ~02:35, still off by default): the game's
-H off|on|fast (on = two buffers, fast = one buffer; without -H the
environment decides: ZOD_AGA_HWSCROLL, ZOD_AGA_DBUF, and
ZOD_AGA_HWSCROLL=<x>x<y> for other margins); ZodLaunch's "AGA scrolling"
cycle (Off / On (two buffers) / Fast (one buffer)) under the AGA mode,
ghosted unless AGA is the renderer, saved as agascroll=. Launcher test
and FS-UAE AgaHwScroll (through -H) pass; ZodLaunch builds.

## First real AGA (20.11. 23:36-23:40)

The PiStorm CM4 sits in an A1200: its chipset is real AGA, the CPU is
Emu68, chip RAM goes over the A1200's own bus. aga branch, software (-o),
-C pal, two buffers, block sums, 30 s each:

| run | fps | groups through c2p a frame | work a frame (sums, c2p, chip writes) |
|---|---|---|---|
| orig01 still | 48.8 | 549 of 19 200 | 4.5 ms |
| orig04 tour | 8.2 | 14 416 (230 KB) | 101 ms |
| RTG reference, orig04 tour (same binary, no -C) | 185.5 | -- | 0.2 ms copy |

- The screen works on real AGA: 1484 frames, no failed buffer change, 8
  waits for a safe buffer in the still view.
- The still view is fine thanks to the block sums.
- Scrolling is chip-bound: ~2.3 MB/s of chip writes through the PiStorm,
  a whole view a frame -> 8 fps. A real 040/060 accelerator writes chip RAM
  at ~7..14 MB/s (the concept's estimate): ~20..35 fps at best. So the step
  that matters on AGA is hardware scrolling (milestone 4), not a faster
  c2p: the c2p's CPU part is small against the chip writes here.
- The picture itself (colours, flicker) waits for someone looking at the
  A1200's native output.

## The user's eye and decisions (21.11. ~00:15, voltago's native output)

- "AGA is looking good on voltago, no complaints, jerky scrolling was to
  be expected" -- picture, colours, flicker and the HUD fine; voltago
  (A4000D 060): still 34.5 fps; hardware scrolling stays the next AGA step.
- The picture: the whole 640x512 for the view (32 more map rows), not
  640x480 centred -- since then -C pal / -C dblpal run the game at
  640x512 (AgaCheck: "the picture 640x512 from row 0", 0 differ);
  Productivity stays 640x480; the automatic fallback without -C still
  640x480 centred.

## Open questions (recommendation first)

1. 640x480 **centred in 640x512** -- or the full 640x512 with 32 more map
   rows.
2. AGA as the **fallback without RTG**, plus an explicit choice -- or
   only the explicit choice.
3. The pointer: **drawn as today** in phase 1, sprite later.

## Eye checks (morning or when the A1200 is up)

- Colours against the RTG picture; interlace flicker on the HUD's thin
  lines; the flicker-free modes on a 31 kHz monitor.
