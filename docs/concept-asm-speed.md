# Concept: more speed, partly in assembler (22.11.2026, for review)

The ask (the user, 22.11. evening): "Dig into the source once more and
identify optimization options, possibly using assembler code."

Nothing is built yet. Each option below states what it rests on (a measured
number with its machine, or a counter still to be read), what it would
change, and an estimate. The recommendation and the open questions are at
the end.

## Morning summary (night 22./23.11.)

All work on local branches, nothing pushed or merged. Each branch builds on
the one above it; `asm-a5` holds everything that was kept.

| branch | what | result on voltago (A4000D 68060, AGA 640x512) | state |
|---|---|---|---|
| `asm-m0` | ZOD_AGA_MEASURE (compare only / into fast RAM), marked-block counts | the split below; the compare was the big part | measuring aid, off unless set |
| `asm-a1` | still view: compare only the blocks marked since the buffer was drawn (ZOD_AGA_MARKS) | still 23 -> 30 fps (AGA work 26.6 -> 15.6 ms) | **keep** |
| `asm-a2` | chunky to planar in 68k assembler (c2p_kern.s, ZOD_AGA_ASM, self-test vs C) | tour AGA work -2 ms (+3.5 %) | **keep** |
| `asm-a4` | the row as its own function, direct longword compares | tour AGA work -7 ms (+10 % fps), still -2.7 ms | **keep** |
| `asm-a5` | the row walks ranges (marked runs) instead of every group | still -1.5 ms, tour unchanged | **keep** |
| `asm-b1` | keyed blit four pixels at a time | objects section +0.2..0.3 ms | rejected, not to merge |
| `asm-a3` | scroll-aware compare (on top of map, map changes, off-map) | a third fewer compares, no gain | rejected, not to merge |
| `asm-d2` | GL: the batches as glDrawArrays, no colour array (ZOD_GL_ARRAYS; Hamiller, 23.11. morning) | 34.3 -> 30.7 fps, drawing +3.2 ms | rejected, not to merge |

**Since the start of the night, AGA on voltago:** still view 23 -> ~31 fps
(AGA work ~26 -> 11.5 ms), tour 9.7 -> ~11 fps (AGA work ~62 -> 55 ms).
Every kept step: FS-UAE AgaCheck and AgaHwScroll read every frame back from
the planes, 0 pixels differ; the full FS-UAE set ran on `asm-a5` at the end
(see the last section).

**Measured and dropped:** ZOD_RTGSCROLL on the ZZ9000 (Zorro III) loses
(15.2 -> 13.3 fps; stays off); a move16 copy of the map background
(copyprobe: slower, or only with an aligned source); the block-sum
multiply (the 040 build already uses shifts); GL option E (the map's
conversion is ~10 ms a strip, the upload is the library's). GL D2 on
Hamiller (23.11. morning): 11 % slower, rejected; D1 not built -- its close
cousin ZOD_UVCACHE was measured worse on 19.11. (gl-plan.md), and its
~0.3 ms lies below what Hamiller resolves.

**For you:** whether to merge `asm-a5` into `gl` (it contains m0, A1, A2,
A4, A5); the AGA look is unchanged by construction, but an eye check on
voltago's or the A1200's native output is the usual last step. A 68040
machine would show A2's real worth (no 040 was available).

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

## Step 0: measured (voltago, 23.11. ~00:15-00:45)

The user's answers (23.11. ~00:05): measure first, then build what the
numbers support, a branch per milestone, no push; voltago and the PiStorm's
A1200; vasm 68020+ with a C fallback; ask MiniGL about D2.

Branch `asm-m0` (975bfb9): ZOD_AGA_MEASURE=compare (the compare pass alone,
nothing converted or written) and =fastram (converted into fast RAM instead
of the planes), and the dirty grid's marked blocks counted per frame.
voltago (A4000D 68060), software drawing, AGA -C pal (640x512, the bottom
panel on its own screen), two buffers, orig01, 30 s each, build/m0:

| run | fps | AGA work a frame | groups a frame |
|---|---|---|---|
| tour, normal (twice) | 10.1 / 10.2 | 61.3 / 60.7 ms | 8,528 |
| tour, compare only | 18.5 | 25.0 ms | -- |
| tour, into fast RAM | 12.7 | 42.3 ms | -- |
| still, normal (twice) | 23.8 / 23.3 | 23.2 / 23.6 ms | -- |
| still, compare only | 27.2 | 20.9 ms | -- |
| still, into fast RAM | 25.7 | 22.2 ms | -- |

- **Tour:** the compare pass ~25 ms, the c2p arithmetic and its fast RAM
  stores ~17 ms, the chip bus ~19 ms more. The compare reads ~655 KB a
  frame (picture and shadow) and looks memory-bound (~30 MB/s); the c2p
  arithmetic (~13 ms after the ~4 ms of fast RAM stores, ~1.5 us a group)
  is the part assembler shortens and can hide behind the chip writes (A2).
- **Still view:** the compare pass is ~21 of the ~23 ms. The dirty grid
  marks 24..28 % of the blocks in a still view (FS-UAE 26 %, voltago's tour
  24 % in its still frames) and 92..97 % while scrolling: A1 hits the still
  view, A2 the tour.

Software on voltago's ZZ9000 (Zorro III), the same binary without -C:

| run | fps | drawing | flip to the card |
|---|---|---|---|
| still | 63.0 | 8.3 ms (map 3.1, objects 1.5) | 3.7 ms |
| tour | 16.2 | 20.4 ms (map 9.9, objects 6.2) | 35.0 ms (484 of 518 frames copied whole) |

The keyed blits are ~25,000 pixels a frame in both (48 / 13 million in the
runs): option B can save ~1..2 ms a frame, worth doing after A. The tour's
35 ms flip is the whole frame over Zorro III every frame: the pending
ZOD_RTGSCROLL A/B (scrolling on the card, "waits for a Zorro RTG card")
can be done on voltago's ZZ9000.

## A1 built and measured (branch `asm-a1`, 13e6449)

ZOD_AGA_MARKS (default on): when the window stands where it stood when this
buffer was drawn last (same place, anchor and picture, at most 4 frames
ago), only the groups under the blocks marked since then are compared; else
the whole window as before. The dressed side column is always compared.
FS-UAE AgaHwScroll: every frame read back, 0 pixels differ in all eight
runs; the still run compared 6,085 instead of 20,480 groups a frame.

voltago, the same binary (build/a1), ZOD_AGA_MARKS off / on / on / off,
orig01 still view, then the tour, 30 s each (23.11. ~00:50-01:05):

| run | off | on | on | off |
|---|---|---|---|---|
| still: fps | 23.2 | **29.6** | **30.4** | 22.1 |
| still: AGA work a frame | 26.2 ms | **15.6 ms** | **15.7 ms** | 27.0 ms |
| still: groups compared a frame | 19,040 | 5,370 | 5,295 | 19,040 |
| tour: fps | 9.7 | 9.7 | | |

**+31 % in the still view** (most of real play), the tour unchanged (the
marks are used in 32 of 326 tour frames: the view stands only between
moves). The picture is the same by construction and by the read-back check.

## B built and measured: REJECTED (branch `asm-b1`, e0fd899, not to be merged)

The keyed row four pixels at a time (port/amiga/zod_keyblit.h, ZOD_KEYBLIT4;
host test keyblittest: 112,128 rows byte for byte equal to the byte loop;
FS-UAE SwSums, NoGL, AgaCheck pass). voltago, software on the ZZ9000, same
binary, off / on / on / off, then the tour off / on (23.11. ~01:20-01:35):

| run | off | on | on | off |
|---|---|---|---|---|
| still: fps | 65.2 | 63.4 | 60.8 | 58.8 |
| still: objects section | 1.49 ms | 1.76 ms | 1.79 ms | 1.62 ms |
| tour: objects section | 6.23 ms | 6.49 ms | | |

The frame rate drifts between blocks (65 -> 59 off/off), but the objects
section is slower with the four-pixel loop every time (+0.2..0.3 ms): Zod's
sprites are small and ragged, most groups of four are mixed, and the
row's alignment prologue and the extra tests cost more than the saved
loads. By the rule (a change measured worse is reverted) B stays on its
branch; A3 was rebased onto A1 without it.

## A3 built (branch `asm-a3`)

In frames whose window MOVED since the buffer was drawn (same anchor), only
what can differ from the shadow is compared: what was drawn on top of the
map in this frame and in the buffer's frame (a second mark grid in
sdl_video.cpp that leaves out the background's blits and the restores --
zod_dirty_mapbg_begin/end around ZMap::DoRender's full blit -- and that the
pointer enters through zod_dirty_mark_ontop), the picture off the map in
both frames (the side panel, the borders, the rows beside the picture: they
stand still on the screen, so they move in the bitmap), the map's changes
since (every blit or fill into ZMap's map surface is reported:
zod_aga_map_changed; ~40 a frame on the tours, the effect tiles), and the
places the buffer's window did not cover. ZOD_AGA_SCROLLMARKS (default on).

- The first build missed the pointer (set into the picture before the copy
  and marked for the output only): 8,000..30,000 pixels differed in the
  moving runs. With the pointer: FS-UAE AgaHwScroll and AgaCheck, every
  frame read back, 0 pixels differ (2,289 and 2,302 frames).
- FS-UAE tours: the compare in 98..292 of ~200..380 frames per run takes
  this way, 9,000..15,000 groups a frame instead of ~19,000..20,000.

**Measured on voltago: no gain -- REJECTED (branch `asm-a3`, 7e716c5, not
to be merged).** Same binary, ZOD_AGA_SCROLLMARKS off / on / on / off,
orig01 tour, 30 s each (23.11. ~01:55-02:05):

| run | off | on | on | off |
|---|---|---|---|---|
| fps | 9.3 | 9.9 | 9.4 | 9.9 |
| AGA work a frame | 64.4 ms | 64.1 ms | 65.4 ms | 62.3 ms |
| groups compared a frame | 17,985 | 11,680 | 11,752 | 18,014 |

A third fewer compares changed nothing: a compare of two equal groups is
cheap, and the ~25 ms of the tour's "compare only" run lies elsewhere --
in the per-row work over all 476 rows, the partial groups at the window's
edges (byte loops whenever the window is not on a 16-pixel boundary), and
the shadow updates and runs of the groups that DO differ, which A3 does not
touch. A1 stays (the still view); A2 was rebased onto A1 without A3.

## A2 built and measured (branch `asm-a2`, on A1 without A3)

port/amiga/c2p_kern.s: 32 pixels at a time by five passes of bit-field swaps
on eight registers (16, 8, 4, 2, 1 bits; the first pass without a temporary
by swap and three eor.w, the others with one register parked in a5), the
eight plane stores inside the last pass, each right after the swap that
makes its register final. 160 instructions a pair against the 306 gcc makes
of c2p_32 (18 of them stack spills). Pure 68020 (assembled with -m68020 as
the check), all CPU trees. The sequence and the register-to-plane mapping
were checked bit for bit against the C reference in Python (2,000 random
groups) before the assembler was written; on the Amiga a self-test at the
first frame compares it with c2p_32 and falls back to C if they differ
(ZOD_AGA_ASM=off takes the C). FS-UAE AgaHwScroll and AgaCheck: self-test
passed, every frame read back, 0 pixels differ (4,814 frames, and 2,502
again after the rebase onto A1).

voltago, same binary (build/a2), ZOD_AGA_ASM off / on / on / off, orig01
tour, then the still view off / on, 30 s each (23.11. ~02:20-02:35):

| run | C | asm | asm | C |
|---|---|---|---|---|
| tour: fps | 9.7 | **10.3** | **9.9** | 9.6 |
| tour: AGA work a frame | 63.3 ms | **60.1 ms** | **62.0 ms** | 63.3 ms |
| still: fps | 30.0 | 30.3 | | |
| still: AGA work a frame | 15.6 ms | 15.5 ms | | |

~2 ms (3.5 %) less AGA work while scrolling, both assembler runs ahead of
both C runs; the still view converts only ~400 groups a frame, so it hardly
shows there. Less than the halved instruction count suggests: the chip
bus (~30 ms of plane writes a scrolling frame at the measured 4.49 MB/s) is
the floor, and the C version already overlapped part of its arithmetic with
the stores. On a 68040, where the arithmetic weighs more against the bus,
the gain should be larger (no 040 machine was available to measure).

## Profiles of the AGA frame, and A4 (branch `asm-a4`, on A2)

PC profiles on voltago (A2 build, 40 s each, build/hw/v-prof-s and
v-prof-t, symbols build/hw/a2-060.syms):
- **Still view:** the AGA frame's own code 45 % of the running samples, the
  conversion (C and assembler) ~5.5 %. Inside the frame: ~28 % at the group
  compare -- gcc 6 had built group_take's memcpy of 4 bytes as copies
  through the stack and back -- and ~22 % at the group loop's control, the
  loop's values reloaded from the stack (flip_hw is inlined into the frame
  function with dozens of live values).
- **Tour:** the frame's own code 33.7 % (60 % of it the compare), the map
  background's copy (copy_lang) 13.4 %, the assembler conversion and the OS
  35 % ("outside": c2p_kern.s is its own code hunk).

A4: the row as a function of its own (hw_row, not inlined), the compare by
direct longword loads (the 68020+ takes any alignment). The result is the
same by construction; FS-UAE AgaHwScroll and AgaCheck: 0 pixels differ
(4,905 frames). voltago, A2's binary against A4's, ABBA, 30 s each
(23.11. ~02:50-03:05):

| run | A2 | A4 | A4 | A2 |
|---|---|---|---|---|
| tour: fps | 9.3 | **10.4** | **11.1** | 10.2 |
| tour: AGA work a frame | 63.5 ms | **56.5 ms** | **53.8 ms** | 61.0 ms |
| still: fps | 30.7 | 29.3 | 30.4 | 28.1 |
| still: AGA work a frame | 15.0 ms | **13.8 ms** | **12.8 ms** | 17.1 ms |

~7 ms less AGA work while scrolling (+10 % fps), ~2.7 ms in the still
view. Together with A1 and A2: voltago's tour 9.7 -> ~10.8 fps, its still
view 23 -> ~30 fps since the start of the night.

## The map background's copy: move16 does not help (measured)

tools/amiga/copyprobe (with copyprobe_k.s), voltago 68060, 476 rows of 512
bytes from a 1408-byte source pitch (the AGA tour's map view), every source
offset 0..15, 20 frames each, results checked against the C loop
(23.11. ~03:20):

| copy | a frame (238 KB) |
|---|---|
| A  the C loop (fastcopy.cpp copy_lang) | 9.3 ms |
| B  four move.l a step, assembler | 9.1 ms |
| C  four move.l into an aligned line, then move16 to the row | 10.0 ms |
| D  move16 straight (16-byte aligned source only) | 6.1 ms |

The 68060 does not pay for reading the destination lines (C, which writes
them whole, is slower), so a move16 copy helps only with an aligned
source -- one scroll position in 16. copy_lang stays; the background copy
is bound by the fast RAM (~51 MB/s read and write together).

## A5 built and measured (branch `asm-a5`, on A4)

The A4 profile (v-prof-s4): hw_row 33.6 % of the still view's samples, ~35 %
of them the compare's loads (memory), ~30 % the loop's control -- every
group of a marked row walked with a test of the marks flag and need[g].
A5 hands hw_row a list of group ranges: the whole window, or the marked
runs (built once per block row a frame) and the dressed groups. FS-UAE
AgaHwScroll and AgaCheck: 0 pixels differ (4,900 frames). voltago, A4
against A5, ABBA (23.11. ~03:40-03:50):

| run | A4 | A5 | A5 | A4 |
|---|---|---|---|---|
| still: fps | 30.3 | 30.7 | 31.5 | 30.6 |
| still: AGA work a frame | 13.5 ms | **11.8 ms** | **11.5 ms** | 12.9 ms |
| tour: AGA work a frame | 55.0 ms | 55.2 ms | | |

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
- **MiniGL's answer (23.11. ~00:20):** unlocked glDrawArrays runs the same
  per-vertex emit inside the library; it saves only the external calls
  (bounded by the 6.4 % dispatch and part of the 5.5 % hook share). Only
  with a size-3 GL_FLOAT vertex array (size 2 takes a slower FPU path), no
  colour array (glColor4ub between draws, a draw per colour run), no
  glLockArraysEXT (ignored by DrawArrays; the locked path hashes for
  nothing here), one texture unit. A few per cent of the drawing time at
  best; to be measured behind a switch on Hamiller.
- **D1, not built:** the same divisions were already tried as ZOD_UVCACHE
  (19.11., gl-plan.md: kept UVs per texture, bit-identical) and measured
  worse on the A4000D; the estimate (~0.3 ms) is below the A/B noise.
- **D2 measured, REJECTED (branch `asm-d2`, 23.11. ~07:00-07:25).** Exactly
  as MiniGL advised: positions as size-3 floats, texture coordinates as
  size-2 floats, no colour array (a quad of another colour first sends the
  quads gathered so far, then glColor4ub), no lock, GL_QUADS; ZOD_GL_ARRAYS=on
  in one binary (build/d2). FS-UAE `tests/auto/GLArrays` (null card):
  checker and GL errors 0, 61,614 binds checked without an error, frame 3000
  through r200view the same as immediate mode apart from what moves between
  runs (units, effects, the clock, the fps counter). Hamiller (RadeonGL 0.18,
  minigl.library 29.1 (22.11.26); orig04 tour, 640x480, 60 s, ABBA x2):

  | run | arrays (va1) | immediate (va0) |
  |---|---|---|
  | fps | 30.6 / 31.0 / 31.1 / 29.9 (30.7) | 35.6 / 34.8 / 33.9 / 33.0 (34.3) |
  | drawing a frame | 21.9 ms | 18.7 ms |

  No overlap in any pair. By section (mean us a frame, arrays against
  immediate): HUD 3274 / 1793 (~11 quads a frame in many colour runs), zones
  1920 / 1130 (~16 quads), objects after 1479 / 900, GUI 352 / 187, fps
  counter 418 / 241; objects (59 quads) even at 3767 / 3740; only the map
  effects (36 quads in long runs) gain, 2571 / 2817. ~11.8 glDrawArrays a
  frame against ~8.9 batches. FS-UAE pointed the same way (drawing 1097 /
  859 us). A mixed mode (arrays only for long runs) would win at most the
  map effects' 0.25 ms: not worth it.
- **Why (FS-UAE profiles, `tests/auto/ProfArrays`, mapped by the MiniGL
  session with a byte-identical 0.16 build's function list):** the cost is
  in RadeonGL and it is per VERTEX, not per call (the section split above
  had suggested per call). Immediate mode: Hook_GLVertex4f 10.0 % (the
  inlined fast vertex write) + Hook_GLTexCoord2f 1.2 % = 11.2 % of the
  running samples. Arrays: Emit 5.0 + Elem 4.5 + FetchAttribs 4.3 +
  ArrayVertex 3.5 + Core_VertexBits 1.1 + Core_DrawArrays 0.7 + Core_Begin
  0.5 = 19.6 %, ~1.75x: the unlocked array loop fetches each vertex through
  four or five internal calls (Elem per attribute recomputes a stride 0
  through a type switch) and sends it down the general Emit, not the
  hook's inline fast path. Zod's own side is cheaper with arrays (vertex()
  6.5 % against 8.0 %). The immediate hooks are the library's leanest path;
  immediate mode is right for Zod with RadeonGL 0.16..0.18. MiniGL noted a
  possible fast path for this array layout; if it lands, D2 is worth one
  re-measure.

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

## The full FS-UAE set on `asm-a5` (23.11. ~04:00-04:40)

SwSums, NoGL, GLSmoke, GLTour, LoadBar, HudKeep, LoadBarPresents,
RoundEnd720, AgaCheck, AgaHwScroll, MidiCamd on build/a5 (040): all pass --
the flip sums check 0 bytes differ, the AGA planes read back 0 pixels differ
(4,852 frames, the conversion in assembler after its self-test), NoGL falls
back to software, RoundEnd720 0 refused and 0 black, MidiCamd 0 events
dropped, no GL error in any log.
