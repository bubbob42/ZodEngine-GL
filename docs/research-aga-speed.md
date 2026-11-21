# Research: faster AGA scrolling

The user, 21.11. ~06:50: "research if we could speed up AGA scrolling by
reducing viewport size, gain DMA cycles by switching off hardware sprites
except for mouse, reducing side panel color depth, reducing color depth of
the game and running dual playfield mode, etc."

Where AGA scrolling stands (soft scrolling on by default since the same
morning, docs/concept-aga.md milestone 4): orig04 tour, 640x512 PAL HiRes
Laced, 8 planes -- voltago (A4000D 060) 5.5 fps without, 5.6 with two
buffers, 7.7 with one; the PiStorm's A1200 7.8 / 12.3 / 17.4. Both are
chip-RAM-bound: the frame's changed 16-pixel groups go through c2p into
chip RAM, and that write rate is what everything below is about.

## 1. What the CPU gets from chip RAM (measured)

tools/amiga/chipprobe: a screen opened in front, then 1 MB written into a
chip buffer and timed with the E-clock -- as 16-bit and as 32-bit writes.

| display in front | PiStorm A1200: 16 bit | 32 bit | voltago A4000D: 16 bit | 32 bit |
|---|---|---|---|---|
| no AGA screen (the Workbench is on RTG) | 3.36 MB/s | 6.79 | 3.38 | 6.76 |
| PAL HiRes Laced 640x512, **8 planes (Zod)** | **2.29** | **4.60** | **2.29** | **4.49** |
| the same, 6 planes | 2.85 | 5.69 | 2.84 | 5.60 |
| the same, 4 planes | 3.40 | 6.72 | 3.39 | 6.79 |
| the same, 2 planes | 3.39 | 6.80 | 3.37 | 6.80 |
| 640x400, 8 planes | 2.53 | 4.94 | 2.48 | 5.00 |
| 640x256 window (laced), 8 planes | 2.83 | 5.67 | 2.82 | 5.48 |

The two machines agree to within a few per cent (both go through Alice's
chip bus; the CPU's own speed does not matter here). What it says:

- **The CPU gets at most ~1.7 million chip accesses a second** (every second
  bus slot), whatever their width. A 32-bit write costs the same as a 16-bit
  one, so **32-bit writes move twice the bytes** -- in every display.
- **Zod's 8-plane display takes a third** of those accesses (3.38 -> 2.29
  MB/s); 6 planes take a sixth; 4 planes and fewer take nothing measurable
  (their fetches fit into slots the CPU cannot use anyway).
- A smaller display window gives back a little: 400 rows +9 %, 256 rows +23 %.

**The c2p writes 16-bit words today** (one word of each plane per 16-pixel
group). That is the cheapest big lever of all -- see 3.1.

## 2. What the colours allow (measured)

FS-UAE ColorCensus: the colour indices of the terrain alone (the map
surface as drawn, ZOD_MAPCOLORS) and of the AGA picture's parts over a
20 s tour (ZOD_AGA_COLORS, every 4th frame). Each planet has its own
256-colour palette.

| | used | carry 99 % of the pixels | carry 99.9 % |
|---|---|---|---|
| terrain, desert (orig01 / orig22) | 84 / 93 | 33 / 35 | 53 / 65 |
| terrain, volcanic (orig08) | 117 | 55 | 88 |
| terrain, jungle (orig15) | 102 | 46 | 71 |
| terrain, arctic (orig29) | 94 | 39 | 59 |
| map view with units and effects | 196..211 | 56..84 | 124..145 |
| side panel (100 x 476) | 119 | 65 | 101 |
| bottom panel (540 x 36) | 80..82 | 50 | 70 |

## 3. The ideas, one by one

### 3.1 Write chip RAM 32 bits at a time -- RECOMMENDED, no change to the look

Two neighbouring 16-pixel groups make one longword per plane (the plane
rows are 64-bit aligned, so a pair starting at an even group is a
longword). Convert by pairs: when either group of a pair differs, write
both as 8 longwords instead of up to 16 words. The strip, the HUD and the
units are mostly contiguous, so few pairs carry an unchanged half.

- Chip write time halves (measured: 2.29 -> 4.49..4.60 MB/s on Zod's screen).
- Estimate on voltago (one buffer, tour): the frame's ~114 KB of writes
  take ~50 of the 77 ms of AGA work; with longwords ~25 ms -> ~105 instead
  of ~130 ms a frame, **~9.5 instead of 7.7 fps**; two buffers ~5.6 -> ~7.
  On the PiStorm the c2p's own CPU part is small, so closer to 2x on the
  write share. Same for the still view and the whole-view path (scrolling
  off).
- System-friendly, both compilers, no look change; FS-UAE's read-back
  check proves it the same way as today.

### 3.2 Fewer planes for the whole game (6 = 64 colours) -- possible, changes the look

- Gains twice: 25 % fewer bytes to write, and the display leaves more slots
  (2.29 -> 2.84 MB/s). Together ~1.65x on the chip-bound part; with 3.1
  on top ~3.3x against today.
- But the picture needs more than 64 colours: the terrain alone needs 33..55
  for 99 % of its pixels (fits), but the map view with units and effects
  needs 56..84 for 99 % and 124..145 for 99.9 %, the side panel 65 / 101.
  64 colours mean a per-planet palette reduction (quantising, maybe
  dithering) of every image at load time -- the units, effects and the
  panel would visibly lose shades. Extra Half-Brite (32 + 32 darker) fits
  this art even less.
- 7 planes (128 colours) would cover 99 % everywhere and 99.9 % nearly;
  the probe did not measure 7 planes (expected between 6 and 8).
- Needs the user's eye; an option at most, not a default.

### 3.3 Dual playfield (two layers of 15 + 15 colours) -- NOT recommended

The idea: the map in one playfield, scrolled by the hardware in a large
bitmap that is never rewritten (only new strips); units, effects and the
HUD in the other playfield, which does not scroll -- so the HUD would cost
nothing (today it is ~4400 of the ~6600 groups a frame while scrolling).

- System-friendly it is possible: Intuition does not open dual playfield
  screens ("DUALPF ... not supported yet"), but graphics.library takes a
  second RasInfo on the screen's ViewPort (the RKRM's dual playfield
  example: Modes |= DUALPF, MakeScreen, RethinkDisplay); since V39
  VC_DUALPF_Disable even allows two scroll offsets with one 256-entry
  palette.
- But each playfield has 4 planes = 15 colours + transparent in hires (AGA's
  dual playfield is at most 4 + 4 planes). The terrain alone needs 33..55
  colours for 99 % of its pixels; units, effects and the panel more. 15
  colours per layer would be a different-looking game.
- The DMA stays that of 8 planes (2.29 MB/s), so the gain is only in fewer
  bytes written -- large, but bought with the look. With VC_DUALPF_Disable
  the colours become 16 x 16 combinations of one palette -- still 16 shades
  for the terrain.

### 3.4 Smaller viewport -- small gain, takes back the user's 640x512

- The display gives back +9 % (400 rows) .. +23 % (256 rows) of bandwidth,
  and fewer rows need converting: at 640x400 ~22 % fewer groups. Together
  ~1.35x on the chip-bound part.
- But the user chose the whole 640x512 on 21.11. (32 more map rows). A
  smaller window shows less map. Possible as an explicit option for slow
  machines, not as a default.

### 3.5 Hardware sprites off except the pointer -- no gain

- AGA's sprite DMA uses fixed slots near the horizontal blank; a sprite that
  is not shown reads its two control words once a frame and then fetches
  nothing. With only the pointer in use, sprite DMA is a few accesses a
  line on the pointer's own lines.
- Switching sprite DMA off would need a write to the DMA control register --
  hardware banging the project rules forbid -- and would take the pointer
  with it. Expected gain: below 1 %. Not worth it.

### 3.6 Side panel in fewer colours -- not possible on one screen

- The side panel shares its scanlines with the map: AGA's depth is per
  line (per ViewPort), not per column, and viewports only stack top to
  bottom. A panel with fewer planes beside the map is not possible without
  a copper list of our own (changing the fetch mid-line -- not
  system-friendly, and not really possible either).
- Only the bottom panel (36 rows, 7 % of the picture) could become a
  viewport of its own with fewer planes -- it needs 50..70 colours, so 6
  planes; the gain would be ~1 % of the bandwidth plus a gap of blank lines
  between the two viewports. Not worth it.

### 3.7 Also looked at

- **DblPAL / Productivity** (the flicker-free modes) fetch twice the lines a
  second -- they cost more bandwidth than PAL HiRes Laced, not less. PAL
  laced stays the cheapest way to show 512 rows.
- **The blitter** (for c2p or for re-anchoring): it uses the same chip bus,
  and a copy costs a read and a write per word where the CPU's c2p costs
  one write. It runs beside the CPU, but on these machines the bus is the
  limit, not the CPU. No gain.
- **The pointer as the system sprite** instead of drawn into the picture:
  saves the pointer's groups whenever the mouse moves (a few dozen a
  frame). Small; a possible later polish.
- **The shadow compare** (fast RAM, ~650 KB of reads a scrolling frame): part
  of the ~27 ms of non-chip AGA work on voltago. Worth measuring after 3.1.

## 4. Recommendation

1. **32-bit chip writes (3.1)** -- next, no change to the look, ~25..30 %
   more fps on voltago while scrolling, more on the PiStorm, also for the
   still view. One branch, FS-UAE read-back, then voltago and the PiStorm.
2. Then measure what is left (the shadow compare, the c2p's own time).
3. **Options for the user's eye**, not defaults: 7 or 6 planes with a
   per-planet palette (3.2), a 640x400 window for slow machines (3.4).
4. Not pursued: dual playfield (15 colours per layer), sprites off, a side
   panel in fewer colours.

## 5. How it was measured

- tools/amiga/chipprobe.c (built with m68k-amigaos-gcc -m68020 -O2
  -noixemul): PiStorm 21.11. 07:02 (labelmaster-ff's OK), voltago 07:05
  (Prisma's OK; only the Workbench screen open before and after,
  PrismaControl running). Logs build/hw/chipprobe-{pistorm,voltago}.txt.
  In FS-UAE it only runs (its chip timing is not the real machine's).
- tests/auto/ColorCensus (FS-UAE, AGA machine): ZOD_MAPCOLORS and
  ZOD_AGA_COLORS, orig01, 08, 15, 22, 29 (desert, volcanic, jungle, desert,
  arctic), 20 s tours.
