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
3. The flicker-free modes; ZodLaunch choice.
4. Hardware scrolling (with the smooth-scrolling concept).

## Open questions (recommendation first)

1. 640x480 **centred in 640x512** -- or the full 640x512 with 32 more map
   rows.
2. AGA as the **fallback without RTG**, plus an explicit choice -- or
   only the explicit choice.
3. The pointer: **drawn as today** in phase 1, sprite later.

## Eye checks (morning or when the A1200 is up)

- Colours against the RTG picture; interlace flicker on the HUD's thin
  lines; the flicker-free modes on a 31 kHz monitor.
