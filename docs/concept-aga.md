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
  idea as ZOD_FLIP_SUMS=check: "0 differ"); the software RTG tests stay
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

voltago (A4000D, 68060, its real AGA chipset; 21.11. 03:11-03:24, orig04
tour, software, -C pal 640x512, 30 s):

| run | fps | groups a frame | work a frame | re-anchors |
|---|---|---|---|---|
| without | 5.5 / 5.6 | ~14 000 | 111 ms | -- |
| two buffers, 192x64 | 5.3 / 5.2 | ~12 300 | 118 ms | 43 |
| two buffers, 384x64 (2 x 880 KB) | 6.3 | 9 856 | 97 ms | 24 |
| one buffer, 192x64 | 6.8 | 8 331 | 87 ms | 44 |
| one buffer, 448x64 (960 KB) | 7.8 | 7 108 | 77 ms | 27 |

The A4000D writes AGA chip RAM no faster than the PiStorm (~2 MB/s with
this 8-plane laced display fetching), so at ~5 fps the view moves ~75 px a
frame and the bitmap is anchored again every 4..8 frames -- with two
buffers each re-anchor rewrites both, which ate the whole gain at 192 px.
Since then the margins are chosen from the free chip RAM (the widest of
448..64 px whose bitmaps fit with 256 KB to spare, a plane in the largest
free block); ZOD_AGA_HWSCROLL=<x>x<y> still sets them. If OpenScreen
fails anyway, the normal screen opens without hardware scrolling. FS-UAE
(1990 KB free): 320 px with two buffers, 448 with one; 0 differ.
voltago with them (03:37-03:45, 1975 KB free, same tour): without 5.5 fps
(111 ms work); -H on, 320 px: 5.6 (108 ms, 34 re-anchors); -H fast,
448 px: 7.7 (78 ms, 28 re-anchors). On the A4000D two buffers barely pay
(every re-anchor rewrites both); one buffer is +40 %. So the eye test
decides between "fast" (+40 % here, +110 % on the PiStorm) and its look.

**Default since the user's word (21.11. ~06:50, "softscrolling becomes
default"): ON with two buffers** -- -H off (or ZOD_AGA_HWSCROLL=off)
switches it off, -H fast takes one buffer; ZodLaunch's "AGA scrolling"
starts at "On (two buffers)" and always passes its -H. Two buffers as the
default because they never show a half-drawn frame; "fast" (the bigger
gain, +40 % on voltago) waits for the user's eye.

How it is chosen (21.11. ~02:35): the game's
-H off|on|fast (on = two buffers, fast = one buffer; without -H the
environment decides: ZOD_AGA_HWSCROLL, ZOD_AGA_DBUF, and
ZOD_AGA_HWSCROLL=<x>x<y> for other margins); ZodLaunch's "AGA scrolling"
cycle (Off / On (two buffers) / Fast (one buffer)) under the AGA mode,
ghosted unless AGA is the renderer, saved as agascroll=. Launcher test
and FS-UAE AgaHwScroll (through -H) pass; ZodLaunch builds.

## The gap above the bottom panel (21.11. midday, for review)

The user: "cover up the black gap on AGA between lower hud and sidebar a
little ... giving the lower hud a smooth edge and shifting elements
between huds so they don't get split by the blackness? Setting another
background colour for the gap could also be an option".

Facts. Between the map screen and the panel screen the display keeps 6
blank laced rows; the panel screen sits that much lower than in the
picture (FS-UAE, SeamProbe: it shows all 36 rows, 482..517 at 640x512,
past the text overscan's 511; 426..461 at 640x400). The seam is always
row 448 of the side panel's art (the engine anchors the side panel at
the bottom: its top is at h - 484), so one rearrangement fits every
height. At the seam the side panel's lower plate (art rows ~406..484)
holds: the CNU3 tiles (art 440..450: 8 rows above the seam, 3 below),
the knob plate's top edge (445..447, above), the hazard stripes
(438..444, period 6 rows), the grid display and its bracket above,
the dial below. Over the map the gap reads as a frame line; the bottom
panel's own top frame starts right under it.

Options (mockups from a real frame on the review page, "Midday"):

- **B (recommended): move what the seam cuts.** Above the seam the right
  plate's contents move up (bracket and grid 2 rows, the CNU3 tiles 3
  rows: whole above the gap), the hazard stripes run 3 rows longer down
  to the gap; below it the tiles' leftovers become plain plate and the
  knob plate starts at the gap. AGA layer only: the map path and the
  panel path take the side column's rows next to the seam from a
  rearranged copy (~40 rows of 46/28 px a frame); the checks compare
  against the same rows. The engine's picture is untouched.
- **B2:** B plus the bottom panel's top frame rail across the side
  column; the art below it 8 rows lower (most of the dial drops off).
- **D:** B with the gap in dark grey. The gap shows colour 0, which is
  also the border's colour: the border around the picture turns grey
  as well, and the two boxes the HUD fills with black (unit picture,
  chat line: index 0 through SDL_MapRGB) need the palette's second
  black (80). Colouring only the gap would take a copper list or
  sprites -- not done here. To be checked in FS-UAE before promising.

**Decided (the user, 21.11. ~13:00): "I like B2, but the sidebar should
get a bevel at its lower edge, too".** Built that way (ZOD_AGA_SEAM,
default on; port/amiga/zod_aga.cpp, dress_build), rows relative to the
gap:

- above it, the right plate: bracket and grid 2 rows higher, the grid
  without one inner row and one cell row (5 rows of cells -> 4), the
  CNU3 tiles without their outer rows, whole and 6 rows higher, a plain
  row below them; the hazard stripes end one row earlier (6 rows, one
  full period); the bottom 4 rows a ledge across the whole side column,
  the side panel's own shelf (the rows 46..43 above the gap, its 6 frame
  columns on the left as its first);
- below it, B2: the bottom panel's 8-row top rail across the side
  column (its columns 240 to the left), the side art 8 rows lower (its
  4 frame columns on the left stay), the tiles' remains plain plate.

Only the side column's rows from 40 above the gap to the panel's end;
nothing the engine draws moves there (the minimap ends 60 rows above
the gap, the pointer is a sprite). Never written into the picture -- the
engine redraws the panels only when they change, so dressing in place
would pile up. A 76 x 116-byte copy per frame (the side column and the
16 columns left of it, so a 16-pixel group reaching into the side column
reads from one place); the map path, the panel path and both checks read
those rows from it. Only in frames that drew the map: a loading picture
is shown as it is.

FS-UAE (tests/auto/AgaSeam, 21.11. 13:48): pal, pal400 and the orig04
tour dressed in every frame that drew the map, read back 0 pixels differ
in all four runs; the control (ZOD_AGA_SEAM off) dressed 0 frames, 0
differ. ZOD_SHOTOUT now works on AGA too: it reads the display back from
the planes (the map screen's rows, the blank rows, the panel screen) --
the review page shows those pictures beside the mockup. They match it.

## The side panel flicker (21.11. ~10:00, the user's A4000T)

The user's video: "side panel flickers on AGA while scrolling" (ZodLaunch
"On (two buffers)", 640x512). Two causes, both fixed (aaa50a0):

1. **"Two buffers" had been one buffer since the own bitmaps (93cb055).**
   The first ScreenBuffer was asked for with SB_SCREEN_BITMAP, which the
   AllocScreenBuffer autodoc keeps for a bitmap Intuition allocated; on a
   screen with our bitmap (SA_BitMap) it fails, and the game fell back to
   one buffer -- the log said "no second buffer", every soft-scrolling
   run from 21.11. ~08:20 on, FS-UAE and voltago alike, unnoticed. One
   buffer writes into the shown bitmap: the side panel, which moves across
   the bitmap every frame, is seen half at its old place and half at its
   new one. Now the first buffer is made from our bitmap (as the autodoc
   says for such screens); the fallback line names the allocation that
   failed, and AgaHwScroll notes that every two-buffer run must say "two
   buffers".
2. **The buffer and its place shown in two steps.** ChangeScreenBuffer
   puts the new buffer on display at the place the display had; the
   ScrollVPort after it moves it to the new place. A frame that starts
   between the two calls shows the new buffer at the old place: the map,
   anchored in the bitmap, looks right, but the side panel, drawn at the
   new place, is off by the scroll step for that frame. Intuition does
   the change in its input task, so while the mouse moves the gap can be
   milliseconds. Now the RasInfo gets the new buffer and the new offsets
   and one ScrollVPort shows both (the ScrollVPort autodoc's "double
   buffering effect"); ChangeScreenBuffer then finds that buffer on
   display and only sends its messages and tells Intuition (if it fails,
   Intuition being busy, the old buffer goes back at the new place).
   ZOD_AGA_TOGETHER=off keeps the old order for a comparison by eye.

The check: with ZOD_AGA_CHECK the game reads the map ViewPort's copper
list (its first plane pointer and BPLCON1) before, between and after the
two calls; between them the display must hold the old buffer at the old
place or the new one at the new. FS-UAE AgaHwScroll: the old order 231 of
231 moving buffer changes "a buffer at the other's place", one ScrollVPort
0 of 473 / 292 / 388 / 232; every two-buffer run two buffers, 0 pixels
differ.

What two buffers really cost (voltago, orig04 tour, 30 s, the bottom panel
on its own screen, same binary, ABBA; 21.11. 10:21-10:29):

| | two buffers | one buffer ("fast") |
|---|---|---|
| 640x512 | 8.9 / 8.7 fps, 9 340 groups, 65 ms AGA work | 10.4 / 10.4 fps, 6 400 groups, 54 ms |
| 640x400 | 12.3 fps, 6 459 groups, 45 ms | 13.0 fps, 4 898 groups, 40 ms |

Each buffer is two frames behind, so the newly shown strip and the moving
units are larger per write (FS-UAE AgaRegions: map view 2 524 groups a
frame with two buffers against 1 773 with one; the side panel, rewritten
almost whole every frame either way, 3 076 against 2 857): two buffers
cost ~15 % at 640x512 and ~5 % at 640x400 --
not "almost nothing", as the one-buffer runs of the morning had made it
look (docs/research-aga-speed.md 3.9 corrected). The A4000T now has the
fixed zod_060 (the morning's build kept as zod_060.onebuf) for the
user's eye: two buffers should show no panel flicker at all; "fast"
still tears by design.

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
