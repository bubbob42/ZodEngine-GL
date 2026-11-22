# Concept: more speed, partly in assembler (22.11.2026, for review)

The ask (the user, 22.11. evening): "Dig into the source once more and
identify optimization options, possibly using assembler code."

Nothing is built yet. Each option below states what it rests on (a measured
number with its machine, or a counter still to be read), what it would
change, and an estimate. The recommendation and the open questions are at
the end.

## Where the time goes, per renderer

**GL (RadeonGL, Hamiller, the gl tip, orig04 tour, 4000 PC samples,
prof-gl21-tour).** Program 73.9 %, library 19.6 %, outside 6.5 %, no
waiting. The top program function holds 5.8 % (zod_gl_blit); the rest is
flat: our GL glue together ~14 % (~33 us a quad), ZMap::DoEffects 5.0 %,
A* path finding ~3.3 %, the server's object passes, the bots. No single
loop dominates, and none is a pixel loop: assembler has no target here
beyond the per-quad glue (option D).

**AGA (voltago, A4000D 68060, orig04 tour, 21.11. 10:21-10:29, ABBA,
docs/concept-aga.md).** The AGA work minus the chip writes (the groups'
plane bytes at the measured 32-bit write rate of tools/amiga/chipprobe:
4.49 MB/s at 640x512, ~5.0 at 640x400):

| run | groups a frame | AGA work | chip writes | the rest (CPU) |
|---|---|---|---|---|
| 640x512, two buffers | 9,340 | 65 ms | ~33 ms | ~32 ms |
| 640x512, one buffer | 6,400 | 54 ms | ~23 ms | ~31 ms |
| 640x400, two buffers | 6,459 | 45 ms | ~21 ms | ~24 ms |
| 640x400, one buffer | 4,898 | 40 ms | ~16 ms | ~24 ms |

**The CPU part does not follow the changed groups; it follows the picture's
area** (31..32 ms at 512 rows, 24 ms at 400: the ratio of the rows). The
code agrees: with hardware scrolling (the default) every frame compares the
WHOLE window with the buffer's shadow, all 20,480 groups at 640x512, group
by group with a branch each, whether or not anything moved there
(zod_aga.cpp, the hardware-scrolling frame); only then are the changed runs
converted and written. ~1.5 us a group for that pass is the largest single
CPU cost on AGA, and the c2p arithmetic itself is the smaller part. Both
are the place where hand-written 68060 code, and a smarter choice of what
to compare, have a clear, large target.

**Software (RTG).** No PC profile on real hardware exists. The A4000T
(arkon, 68060, orig01 still) runs 61.2 fps against v1.1's 33.6, the flip
3.5..4 ms of a 16.3 ms frame (19.11.). FS-UAE's section times
(SwSums on the 2.0 build: drawing 1.7 ms, flip 8.4 ms) are not real
hardware's proportions. The drawing's keyed blits are a plain byte loop
(option B); how much of a real frame they take must be measured first.

**Checked and ruled out: instructions the 040/060 emulate.** The 2.0
binaries were disassembled and searched for FPU and integer instructions
that trap to 68040.library/68060.library (fsin, fcos, fetox, fatan, fmovecr,
movep, 64-bit mul/div, and fint/fintrz on the 040):
- fsin/fcos/fetox/fmovecr sit only in libm's sin/cos/exp and in libstdc++
  locale code, none of them called per frame (the trig cache, ZOD_TRIGCACHE,
  took sin/cos out of the drawing).
- fintrz appears only in the 060 build, where the 68060 has it in hardware;
  the 040 build has none (ifloor, ZOD_FASTFLOOR).
- No 64-bit multiply or divide in either build. The movep hits are data in
  the code section, not instructions.

## The options

### A. AGA: compare less, and compare, convert and write in one assembler pass -- RECOMMENDED

Two parts, the first in C++, the second in assembler:

**A1. Compare only where something can have changed.** In a frame where the
view did not move, only the 16x16 blocks the engine drew into can differ
from the shadow, and the engine marks exactly those (the dirty grid the RTG
flip already relies on, with the previous frame's marks for the second
buffer). Compare those blocks and skip the rest. While the view scrolls,
everything is marked and nothing changes; in a still view (most of real
play) the ~31 ms pass shrinks to the marked blocks.

**A2. One pass in assembler.** A 68060/68040 routine (vasm, pure 68020+
instructions, like port/amiga/musik_kern.s) that walks a row of groups:
compares 16 bytes with the shadow without a branch per longword, converts a
changed pair to 8 longwords, and **interleaves the chip stores of one pair
with the compare and arithmetic of the next**. While the chip bus completes
a store, the CPU is already reading the next group. This is the structure
of the fast 040/060 c2p routines (Kalms' c2p family); our code would be our
own.

- **Premise to check first (step 0):** a measuring switch (ZOD_AGA_TIMES)
  times the compare pass, the c2p arithmetic and the stores separately with
  the E-clock; a second switch writes the planes to a fast RAM buffer
  instead of chip RAM, so the chip bus cost stands alone; and the dirty
  grid's marked share in a still view and on the tour. The table above
  says the compare pass is the big part; step 0 confirms it with its own
  numbers before any assembler is written.
- **Estimate (if the premise holds):** still view: the CPU part from ~31
  ms to a few ms (A1). Scrolling: the compare and the arithmetic largely
  hidden behind the ~33 ms of writes (A2): voltago's AGA work ~65 ->
  ~40..45 ms, ~8.7 -> ~10.5..11.5 fps with two buffers. The PiStorm's
  A1200 gains less: Emu68 runs the CPU part fast already (34 ms of AGA
  work there were mostly chip bus).
- **Exactness:** bit for bit. FS-UAE's AgaCheck and AgaHwScroll read every
  frame back from the planes (0 pixels must differ), and the host test
  (tests/host/c2p_test.c) checks the C reference.
- **Safety:** the C path stays as the fallback (ZOD_AGA_ASM=off), the same
  pattern as the music mixer's kernel: a self-test at start compares the
  assembler result with the C result for a test block and falls back if
  they differ.

### B. Software and AGA: the keyed blit four pixels at a time

All sprite drawing into the 8-bit compose surface (software and AGA
renderers; GL does not use it) goes through one loop in
port/amiga/sdl_video.cpp: load a byte, compare with the key, store it if
it is not transparent. Sprites are mostly whole runs of opaque or
transparent pixels, so a longword test (no zero byte: store all four;
all zero: skip; else byte by byte) handles most of a sprite at a quarter of
the loads and branches. The Vampire build already does this with AMMX
(ammx_blit.s); the 040/060 builds have only the byte loop.

- **Premise to check first:** one software run on real hardware (arkon,
  A4000T) with the section times and the blit counters
  (zod_blit_key_pixels; the log's "Zeit je Bild" line). If the keyed
  blits' sections (objects, effects, objects before/after) are only a
  small share of the frame there, this option drops.
- **Form:** C first (the zero-byte test is plain C and compiles well with
  gcc 6.5); assembler only if C leaves a clear gap.
- **Estimate:** the keyed pixels' time 2..3x shorter; the frame gain
  depends on the share measured above.
- **Exactness:** bit for bit; SwSums and AgaCheck compare every frame.

### C. The block sums: a cheaper hash, or the shadow compare

The RTG flip and the AGA path without hardware scrolling hash each marked
16x16 block (block_sum, block_sum16: two chains, one with a multiply by 33
per longword). Replacing the multiply by a shift and an add, and running
both chains in a short assembler loop, cuts the hash's time; the AGA
hardware-scrolling path already compares against a shadow instead.

- **Premise:** how many blocks are hashed a frame (the flip counters in
  the log) and the hash's share of the 3.5..4 ms flip on arkon.
- **Note:** the hash value changes with the new formula. That is fine (the
  sums are only compared with each other within one run), but the old and
  new formulas must never be mixed in one run.
- **Estimate:** small; likely under 1 ms a frame on arkon. Worth doing
  only together with A or B, not alone.

### D. GL: fewer calls per quad

Our glue costs ~33 us a quad on Hamiller, and the library's side of a quad
(its dispatch 6.4 %, the vertex hooks ~5.5 %) comes from immediate mode:
glBegin, four glTexCoord/glVertex pairs, glEnd for every sprite.
- D1: a whole-image blit of a one-tile texture always gets the same four
  texture coordinates; computing them once per texture removes four
  divisions per quad (bit-exact; ~0.3 ms a frame, ~1 %, below the A/B noise
  on the A4000D -- recorded in 5h, not done).
- D2: the quads of one batch as one vertex array (glDrawArrays) instead of
  four calls per vertex. Whether that is cheaper depends on RadeonGL's
  array path, so this is a question for the MiniGL session, not something
  to build blind. Their library-side rectangle lists (quadrect2/3) made Zod
  slower, which is a reason for care.
- No assembler: the time is spread over calls and cache misses, not one
  loop.

### E. Load time: the map texture's conversion

The whole-map texture goes up in 14 strips at the first frame after the
load bar: ~95 ms each on the A4000D, ~43 ms of that the 8-bit to RGB565
conversion (n17, 20.11.), together ~1.3 s of stand-still before the game
moves. A table of pixel pairs (65,536 longwords = 256 KB, built once) or a
short assembler loop halves the conversion's loads.
- **Estimate:** ~0.3..0.5 s less stand-still after each map load; no
  frame-rate change.
- Moving the send behind the load bar (5h) would hide it completely and
  may be the better fix; both can be done.

### Not proposed

- **The game logic** (the server's passes, the bots, A* ~3.3 %,
  DoEffects 5 %): flat, double-precision C++ that the 060's FPU runs well.
  The remaining DoEffects cost is the original's random draws per tile
  (5h); changing it changes the random stream.
- **move16 for the flip to the card:** whether a Zorro III card takes
  bursts depends on the card; the copy is not the flip's main cost after
  the block sums.
- **The music mixer:** already assembler (musik_kern.s) for AHI/Paula.

## Plan

0. **Measure** (no behaviour change, switches only):
   - ZOD_AGA_TIMES and the fast-RAM target on voltago (Prisma's machine,
     with their OK) and on the PiStorm's A1200: the 65 ms split.
   - one software run on arkon (A4000T): the section times and the blit
     counters.
   The results go into this document; then the options are re-ranked.
1. **A1** (C++: compare only the marked blocks when the view stands
   still), then **A2** (the assembler pass, the C fallback, the self test),
   each on its own branch; FS-UAE AgaCheck/AgaHwScroll 0 differ; voltago
   and the A1200, ABBA, before and after, still view and tour.
2. **B** (if step 0 shows a share worth it): C first, SwSums/AgaCheck,
   arkon A/B.
3. **E** (load time), then **C** if it still shows.
4. **D2** only after the MiniGL session's answer.

One change at a time; a change measured worse is reverted.

## Open questions (recommendation first)

1. **Start with the measurements (step 0)?** Recommended: yes. They take
   one evening of runs and decide whether A and B are worth building.
2. **Machines for step 0:** voltago is Prisma's. Recommended: ask the
   Prisma session for a slot, as before; the PiStorm's A1200 as the second
   AGA machine; arkon for the software run.
3. **Assembler style:** recommended vasm with Motorola syntax, pure 68020+
   (runs on 040, 060 and 080), a C fallback and a start-up self-test, as
   musik_kern.s does.
4. **D2 (vertex arrays):** recommended to ask the MiniGL session first
   whether RadeonGL's glDrawArrays path is cheaper per quad than immediate
   mode for ~140 small textured quads a frame.
