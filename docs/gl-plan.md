# Zod Engine on MiniGL (RadeonGL / PiStorm3D) -- plan

Status: draft for review, 2026-11-18. Nothing built yet. The plan is based on
cego's v1.1 source (main = tag v1.1, 0c7c767), the RadeonGL session's answers
(RadeonGL 0.16) and the GLTron/Doom64 ports.

## 1. Goal

One Zod Engine binary per CPU. It draws through minigl.library 29.1 when one
opens and a context comes up (RadeonGL on the A4000D, PiStorm3D on the
PiStorm). Otherwise it runs cego's 8-bit software path unchanged, so V1200
users lose nothing. The work lives on a `gl` branch and goes back to
AMIGAbench/ZodEngine when cego agrees.

## 2. What we start from (facts)

**The port's renderer.** cego wrote his own SDL 1.2 layer (port/amiga/sdl_screen.cpp,
sdl_video.cpp):
- It composes every frame in an 8-bit fast-RAM surface with one shared
  palette, on its own LUT8 CUSTOMSCREEN.
- SDL_Flip copies the dirty 16x16-block runs to the RTG screen.
- There is no window mode, by decision: the 8-bit palette path needs its own screen.
- The map is one big opaque surface: width*16 x height*16 pixels, up to 4.8 MB at 8 bit.
  ZMap::DoRender restores only last frame's dirty blocks and blits the full
  view only after a scroll.
- Rotation and scaling go through SDL_rotozoom with an 8-entry cache per surface.
  cego turns the per-frame debris spin OFF on the Amiga (ZOD_SPIN). He
  measured 167 ms in one frame for 22 rotations on the V1200.

**cego's measurements (V1200, 68080):**

| Measure | Value |
|---|---|
| Frame time | 12-19 ms, up to 40 ms in explosions |
| Drawing ("zeichnen") | 79% of the frame |
| Flip | ~2.4 ms for 148 KB in 673 copies |

The port has its own section profiler (zod_rsec_*, fineclock), a host
"shim" build that runs the whole engine on the Mac with ASan and
ZOD_SHOT=<file>:<frame> screenshots, and a protocol log (ZOD_PACKET_LOG) with
goldens from a 120 s bot game.

**Upstream's GL path** (upstream/.../QZod_DnSeparate/zsdl_opengl.cpp) is
compiled out by -DDISABLE_OPENGL. The switch `use_opengl` is a runtime flag,
and upstream's `-o` turns GL off.
- It is immediate-mode GL 1.1, 28 distinct calls, all in minigl 29.1.
- Every sprite is one glPushMatrix / glTranslatef / glScalef / glRotatef /
  glBindTexture / glBegin(GL_QUADS) / 4 vertices / glEnd / glPopMatrix.
- Every surface is its own 24/32-bit texture, GL_LINEAR.
- In GL mode the HUD is redrawn in full every frame.
- BlitOnToMe / FillRectOnToMe DELETE the texture, so it is uploaded again on next use.
- Some paths have no GL branch, for example the text images (zplayer.cpp:1025, zplayer_events.cpp:753).

**RadeonGL 0.16 (from the MiniGL session):**
- Textures:
  - Formats: RGBA / RGB / L / A / LA / GL_BGRA.
  - Stored as RGBA = ARGB4444, a sized GL_RGBA8 = ARGB8888, and RGB = RGB565.
  - GL_COLOR_INDEX through ONE glColorTable palette, expanded at upload.
  - At most 2048 x 2048, NPOT allowed (one level).
- **Heap 16 MB on a screen context (8 MB in a window), NO eviction:** a full
  heap gives GL_OUT_OF_MEMORY. The library's upload cache (locked vertex
  arrays) and its vertex stream come out of the same heap: 4 MB + 0.5 MB at
  the start, and the cache never gives way below that. Zod holds the cache
  at 1 MB (5k).
- Uploads cross the Mediator at ~8.4 MB/s, so upload once and never per frame.
- **A small draw costs ~120 us on the 68060** (Doom 64 MAP33 profile). One
  draw per sprite would cost more than the software blitter saves.
- Static geometry stays on the card under GL_RGL_static_arrays. Moving data
  goes under GL_RGL_dynamic_arrays, but GLTron measured that locked DYNAMIC
  arrays lose to immediate mode on RadeonGL.
- Screen contexts in every mode, vsync, tear-free triple buffering (0.15/0.16);
  window and bitmap contexts since 0.9.

**PiStorm3D traps** (memory pistorm3d-defects):
- A single context only.
- Deleted textures' memory is NOT returned.
- Large immediate blocks give GL_OUT_OF_MEMORY unless mglChooseVertexBufferSize is called.

**Bus arithmetic (a prediction, M1 measures it).** The A4000D moves 8.9 MB/s
to the Radeon. The V1200's typical flip of 148 KB therefore costs ~17 ms
there (V1200: 2.4 ms). A scroll that sends the full 640x480 8-bit view costs
~35 ms. With GL, a sprite is ~100 bytes of commands, so 300 sprites cost
~3.4 ms of bus time, and scrolling costs nothing extra. That gap, plus free
rotation and scaling, is the reason to do this.

## 3. Architecture

### 3.1 Glue (port/amiga)

- **minigl.library is opened at run time, without autoopen.** If it is
  missing, too old, or mglCreateContext fails, the program logs why and takes
  the software path. use_opengl becomes "GL wanted AND GL came up".
- **Context:** a screen context at the chosen size, 16 bit, Z16, triple buffered.
  - Copy the glue from GLTron amiga/video_amiga.c + panflip_amiga.c. Doom64
    amiga/i_video_amiga.c is newer: ask the Doom64 session which one to
    take (siblings-before-implementing).
  - SDL_GL_SwapBuffers maps to mglSwitchDisplay / the panflip.
- **Input:** cego's IDCMP code (sdl_screen.cpp) is fed from the context's
  window (mglGetWindowHandle) instead of his own window. The pointer is
  either drawn as the last sprite (same look) or uses the system pointer
  (decision D6).
- **Build:**
  - DISABLE_OPENGL goes; a ZOD_NO_GL build switch keeps the old binary
    reproducible.
  - The minigl 29.1 SDK comes from ../RadeonGL/sdk/minigl-29.1. Check
    whether its headers compile under g++ (extern "C", inline macros).
  - CPUs: 040 and 060. The 080 build carries the GL code as well and simply
    falls back at run time.

### 3.2 The batching renderer (new module port/zod_gl.cpp behind ZSDL_Surface)

The upstream use_opengl branches call into one module instead of issuing GL
themselves.

- **Atlases:**
  - At load, sprites go into atlas pages of 1024x1024 (or 2048x2048) as
    GL_COLOR_INDEX through the shared palette. The colour-key index becomes
    alpha 0.
  - Every page carries one white texel for FillRect / FillDots, so fills do
    not break a batch.
  - Each ZSDL_Surface keeps (page, u0, v0, u1, v1).
  - The texture objects are fixed and are refilled with glTexSubImage2D, never
    deleted and re-created. PiStorm3D does not return deleted texture memory.
- **Corners on the CPU:** translation, scale and rotation (sin/cos table)
  give 4 screen-space vertices. There is no matrix stack per sprite.
- **Batches:** sprites queue per page and are flushed as one glBegin(GL_QUADS)
  block, or one vertex array if measuring says so.
- **Painter's order without breaking batches (to be measured in M3):** each
  sprite gets z = its sequence number in the frame.
  - Opaque and colour-keyed sprites are drawn grouped by page, with depth
    test LESS + alpha test, so their order no longer matters.
  - Sprites with SetAlpha < 255 are drawn afterwards, in sequence, with depth
    test on and depth write off.
  - The fallback is plain in-order batching (a flush on every page change).
- **Map:** decision D4, after the M1 census.
  - (a) Chunked textures: 256x256 RGB565 chunks of full_render, uploaded at
    map load. A crater or zone marker becomes a glTexSubImage2D of its rectangle.
  - (b) A tile map: the planet template as one small texture, the map's tile
    quads as a static array on the card, craters and zone markers as decal quads.
  - (a) is close to today's code. (b) cannot run out of heap.
- **HUD, minimap, text:**
  - The static HUD lives in an atlas.
  - The minimap and text images are small textures updated with
    glTexSubImage2D when they change.
  - Glyphs come from the zfont surfaces, into an atlas.
- **Hit flash** (white silhouette): GL_ADD with colour white. RadeonGL main
  supports it (GL_EXT_texture_env_add, used by Doom 64). If PiStorm3D lacks
  it, use white silhouette copies in the atlas there.
- **Out of texture memory:** every upload is checked with glGetError. A
  GL_OUT_OF_MEMORY at load closes the context and takes the software path,
  with a log line; it is never a half-drawn game. A window or bitmap context
  can be cut to a 512 KB heap under memory pressure (RadeonGL branch
  heap-floor), so this case is real.
- **Filtering:** GL_NEAREST by default, which matches the software path for
  1:1 sprites. Pages get a 1-texel border so scaled or rotated sprites do not
  bleed.

## 4. Milestones

Each milestone ends with a measurement or a picture. One unverified change at a time.

| # | Milestone | Done when |
|---|---|---|
| M0 | Repository and build | `gl` branch builds zod_040/zod_060 with our bebbo 6.5.0b (cego uses the same version in Docker; his Makefile's bbb/btst guards must pass) |
| M1 | Baseline + census | A4000D software fps and cego's section split (v1.1, 060); shim census: draws per frame, distinct surfaces, rotations, fills, texel bytes per kind, every map's size; then D4/D5 decided |
| M2 | GL comes up, naive | upstream's per-sprite path compiled in, context + input + fallback; correct picture in FS-UAE on the RadeonGL null card (frame dumps via r200view) and on the A4000D; slow is fine -- it is the reference picture |
| M3 | Batching renderer | 3.2 built; null-card frames identical to M2's at the same texture format; packet golden of the bot game identical to the software build (logic untouched) |
| M4 | Speed on the A4000D | software vs GL, ABBA, same map/scene; profile; then optimise one step at a time; free spin/scaling as an option once it is cheap |
| M5 | PiStorm3D + polish | PiStorm run (vertex buffer size, textures never deleted); ZodLaunch gets "Renderer: Software / MiniGL"; README section |
| M6 | Back to cego | the user contacts cego; `gl` pushed to AMIGAbench/ZodEngine or offered as a PR, on the user's word |

**Test material:**
- The Z (Europe) CD image is in ~/Downloads and is installed with cego's ZExtract.
- The FS-UAE harness is a copy of GLTron's: 68040 + uaegfx + RadeonGL
  null-card library; run `pgrep -x fs-uae` first.
- A4000D runs are announced to all sessions, with a screen check first.
- Game data never goes into a repository.

## 5. Risks

- **Heap without eviction (16 MB).** All sprites at 2 bytes a texel plus a
  large map may not fit. M1's census answers this before any renderer code.
- **Per-draw cost.** If the batches stay small (many page changes),
  RadeonGL's ~120 us a draw eats the gain. The z-sequence trick exists for that.
- **Colour depth.** ARGB4444 has 4 bits per channel, so the palette's colours band.
  - ARGB8888 doubles VRAM.
  - ARGB1555 would fit Zod exactly (5-bit colour, 1-bit key). RadeonGL's
    branch gl13-phase1 already stores GL_RGB5_A1 as ARGB1555. It is merged
    only after MiniGL's GLQuake A/B (in progress 2026-11-18); main 0.16
    still gives 4444.
- **The upstream GL path is incomplete** (text, hit flash, partial updates).
  M2 lists what is missing. Nothing is assumed working until it is seen.
- **C++ and the MiniGL SDK headers** have never been tried with g++ in this family.
- **The PiStorm3D single context** gives no restart after a context failure
  inside one run.

## 5a. M1 results so far (host census, 2026-11-18)

Measured with the shim build (the whole engine on Linux, cego's own
graphics layer) and the new draw census (`ZOD_CENSUS=on`, port/zod_census.*).
The setup: bot red against bot blue, the player watching (`-t null`), no
start pause (`-e psettings_nopause.txt`), 640x480, 40-60 s, still view.

| Map | Map surface | Screen draws/frame (mean, max) | Changes in draw order | Distinct pages |
|---|---|---|---|---|
| p02_bb_orig01 | 1024x1376 | 50, 79 | 19, 31 | 15, 23 |
| p02_bb_orig04 | 1376x1760 | 113, 143 | 65, 79 | 28, 36 |
| p04_bb_p04m01 | 2048x2048 | 66, 74 | 30, 38 | 24, 32 |
| p08_bb_p08m01 | 4096x4096 | 49, 58 | 13, 20 | 11, 17 |

- **Images:** 3,282 loaded (3.44 M px: 6.9 MB at 16 bit), but only about
  190-210 are drawn in a 40 s match (0.25-0.28 M px: about 0.5 MB at 16 bit).
- **The map surface does not fit the heap.** 4096x4096 is 33.5 MB at 16
  bit, against a 16 MB heap.
- **Most draws on orig04 are map restores** (70 of 113 are copies from the
  map surface back over last frame's sprites). Under GL the map is drawn
  whole every frame, so these disappear.
- **Off-screen drawing is rare:** 0.4-1.4 draws a frame, so texture updates
  cost nothing in play. The burst at map load is the map being built.
- **Sprite draws per frame are far below the plan's guess** of 300: 35-42
  from loaded images, about 1 rotated copy.

**Decisions taken from this data:**

- **D4 (map): a wrap-around map texture.**
  - One 1024x1024 RGB565 texture (2 MB) holds the part of the map around
    the view in 128x128 tiles, addressed modulo the texture size.
  - As the view moves, new tiles are uploaded ahead of it, one or two per
    frame (32 KB each, about 4 ms at 8.4 MB/s).
  - A crater or zone marker baked into the map surface becomes a
    glTexSubImage2D of its rectangle if that tile is resident.
  - The whole map layer is one draw. The map surface itself stays in fast
    RAM as today.
- **D5 (format): 16 bit.** RGBA8 doubles memory for no visible gain at
  this palette. Use ARGB1555 once gl13-phase1 is merged, 4444 until then.
- **Atlas: one shared page set, filled on first draw, instead of one page
  per asset group.**
  - The working set is small (about 0.5 MB), so most frames need one
    sprite page.
  - A sprite is uploaded when it is first drawn (a 32x32 sprite is 2 KB,
    0.24 ms). Groups can be pre-warmed at map load if first-draw hiccups show.

Still open from M1: the A4000D software baseline (cego's `-B` report with
`ZOD_CENSUS`), and a scrolling benchmark: a still view hides the flip cost
of scrolling, which is where GL gains most.

## 5b. Progress, 2026-11-18

All in FS-UAE (68040 + RTG) on RadeonGL 0.16's null card. The packet
checker sees every command; r200view renders the dumped frames.

- **M0 done.**
  - The private repo bubbob42/ZodEngine-GL, with branch `gl`.
  - A separate bebbo gcc 6.5 (amiga6 of 31 Oct) with libstdc++ in
    ~/amiga-gcc6. The shared toolchain has no libstdc++.
  - cego's shim build links again; it runs in a Linux container.
- **M1, host part done (5a).** The A4000D baseline is still open.
- **M2 done:** port/zod_gl.cpp, plus the context in sdl_screen.cpp.
  - Without the library, without a screen from it, or with -o, the game
    draws in software as before.
  - GLSmoke frame equals the software frame; 0 checker errors.
- **M3, most of it done:**
  - The map goes through one 1024x1024 RGB565 wrap-around texture of
    128x128 tiles, so p08's 4096x4096 map now draws.
  - Images up to 256x256 sit in atlas pages, run-time surfaces up to
    256x64 in slots, and fills use the white block.
  - Colour goes per quad, so it never ends a batch.
  - **Batches per frame: 13-15** (orig01 still 13.0, orig04 scrolling
    14.6, p08 14.3), down from 70-119 in M2's first state.
  - Heap about 10 of 16 MB; one atlas page holds every image of a run.
  - **Not built, and maybe not needed:** the depth-by-draw-order trick.
    In-order batching already gives 13-15 a frame; the A4000D decides.
- **Found on the way:**
  - The HUD frees each text surface right after drawing it, so texture
    names are retired for three frames and then reused.
  - RadeonGL gives power-of-two textures a full mip chain. Large surfaces
    therefore go up at their own size, and atlas pages are 1023 high.

## 5c. First A4000D runs (M1 baseline, M4 start), 2026-11-18

A4000D, 68060, Radeon 9200 through the Mediator, RadeonGL 0.16 as installed.
60 s bot matches, sound off, 640x480, logs in RAM:; the game's -B report.
R = cego's v1.1 zod_060, S = ours with -o, G/H/D = ours through MiniGL
(G = 4746f4a, H = + content sums fe71755, D = + HUD text 8e069ed + 565
tiles adc3502). Runners: tools/hw/zod_run.py, zod_ab.py (git-ignored).

**orig01, still view (R S G G S R, then G H H G):**

| variant | fps | draw ms | flip ms | HUD ms |
|---|---|---|---|---|
| R cego v1.1 | 18.0 / 19.0 | 22.7 | 14.2 | 2.7 |
| S ours, software | 20.4 / 19.8 | 21.2 | 13.5 | 2.6 |
| G GL | 17.0 / 16.6 / 17.7 / 16.9 | 42.5 | 0 | 20.2 |
| H GL + content sums | 20.9 / 22.5 | 31.0 | 0 | 9.2 |

**orig04, view scrolling (ZOD_TOUR; S H H S):**

| variant | fps | draw ms | flip ms | map ms | HUD ms | gui ms |
|---|---|---|---|---|---|---|
| S | 7.2 / 7.0 | 56.1 | 44.6 | 25.7 | 5.4 | 0.5 |
| H | 6.3 / 6.7 | 115 | 0 | 65.0 | 15.1 | 5.2 |

What the numbers said, and what was done:
- **Texture hazards.** RadeonGL waits for the card to go idle before an
  upload into a texture the frame has drawn from (docs/core.md "Texture
  hazards"). The HUD's per-frame text went into the atlas page every sprite
  uses: 4.4 idle waits a frame, ~18 ms. Fixed by content sums (fe71755).
- **The HUD's full redraw.** GL redraws the whole HUD every frame; the
  clock and the unit count rendered four new text surfaces a frame. Fixed
  by keeping them (8e069ed). FS-UAE: GL HUD 674 -> 327 us. A software full
  redraw costs 2983 us there, so GL itself draws the HUD cheaply.
- **Scrolling.** Each 128x128 map tile went up as RGBA bytes, converted
  twice: ~20 ms a tile, 1.7 tiles a frame. Now RGB565 words the library
  only copies (adc3502, ZOD_GL_565=off to compare).
- **Map section in a still view** costs the same in S and GL (~7 ms): the
  game's own work, not drawing.

**After a reboot (18:59), D measured (still H D D H, tour E D D E; E = D with
ZOD_GL_565=off):**

| run | variant | fps | draw ms | map ms | HUD ms | gui ms |
|---|---|---|---|---|---|---|
| orig01 still | H | 23.4 / 22.4 | 28.2 | 4.0 | 8.9 | 1.6 |
| orig01 still | D | 24.9 / 26.2 | 24.5 | 2.4 | 5.6 | 1.4 |
| orig04 tour | E | 7.1 / 7.4 | 99.2 | 54.6 | 11.5 | 4.8 |
| orig04 tour | D | 12.3 / 12.5 | 50.1 | 11.7 | 8.6 | 2.8 |

- The HUD text cache: +11 % in the still view (HUD 8.9 -> 5.6 ms).
- RGB565 map tiles: +70 % while scrolling (map 54.6 -> 11.7 ms; software
  25.7 ms). GL now scrolls at 12.4 fps where the software build does 7.1.
- Against the software build (S, earlier series): still +27 %, scrolling
  +75 %; against cego's v1.1 release, still +38 %.
- Left where GL costs more than software: HUD (+3 ms, the full redraw),
  gui (+1 to +2.4 ms), effects (+1 ms), pre-drawn objects (+0.8 ms). The
  minimap is rebuilt ~4 times a second in both paths (2.3-3.8 ms each);
  GL re-sends it whole as RGBA bytes (252 updates a minute).

**Images up to 512 tall in the atlas (3ce97b9; F = D + d2d111d + 3ce97b9,
still D F F D, 19:30):** batches 10.3 -> 4.2 a frame, HUD 6.0 -> 5.3 ms,
but fps D 23.7 / 25.7 against F 24.8 / 25.2: within the runs' own spread.
Batch breaks are cheap on RadeonGL 0.16; their number no longer limits.
The largest GL extra left is the effects section (2.4-3.4 ms against 0.8).

**No sin/cos for unturned sprites (119d63b; T = F + 119d63b, still F T T F,
20:10):** every scaled sprite paid two sin/cos pairs, in software on the
040/060 (~47 us an effect). F 22.3 / 24.9 fps, T 27.4 / 27.5; effects
3.4 / 1.1 -> 0.7 / 1.0 ms, draw 28.6 / 25.4 -> 22.5 / 21.9 ms.

**Where the A4000D stands (orig01 still, 60 s):** GL 27.5 fps, our software
build ~20, cego's v1.1 18.5. Scrolling (orig04): GL 12.4, software 7.1.
GL still costs more than software in: HUD +2.4 ms (the full redraw), gui
+0.9, pre-drawn objects +0.9, birds +0.35; it saves 4.7 ms on the map,
1 ms on objects and the whole 13.5 ms flip.

## 5d. PiStorm3D (M5), 2026-11-18

A1200 PiStorm CM4 (Emu68, reported as a 68040), PiStorm3D 30.0 (MiniGLV3D
b519) as installed, through the same 29.1-SDK build (040 binaries: cego's
v1.1 zod_040, ours d2d111d = build/amiga-g6e). 60 s bot matches, 640x480.

| run | R cego v1.1 | S ours, software | G ours, GL |
|---|---|---|---|
| orig01 still (R S G G S R) | 220 / 219 fps | 221 / 230 | 229 / 240 |
| orig04 scrolling (S G G S) | -- | 174 / 174 | 177 / 177 |

- Works at the first try: powers of two, RGBA map tiles (copy565_ok is
  RadeonGL only), no NULL pixels, 0 textures refused.
- PiStorm3D lists no GL_EXT_texture_env_add: the hit flash is drawn plain.
- At ~230 fps the game logic and the server dominate; GL gains 2-4 %.
  Drawing: map 0.13 vs 0.34 ms, objects 0.27 vs 0.58 ms; the HUD's full
  redraw 0.40 vs 0.16 ms (this build predates 3ce97b9).
- Not checked yet: the picture on the PiStorm's own screen (AmiAgent
  screenshots during GL coincided with hangs elsewhere), and tearing:
  PiStorm3D has no vsync and no multi-buffering (defect 4). An own present
  like Doom64's panflip would cure it, at the cost of a bitmap context
  (defect 3: leaks per context round).

## 5e. Next steps -- concepts for review (2026-11-18 evening)

Three candidates; none is started. Each is a new piece of behaviour, so it
waits for the user's choice.

**A. Tear-free on the PiStorm: a present of our own.**
- Problem: PiStorm3D keeps one buffer and no vsync below VideoCore.card 2.0
  (defect 4; the PiStorm runs Emu68-VC4.card 1.0). The GPU draws into the
  screen on display, so the picture tears. Zod runs at 177-234 fps there,
  so the picture tears all the time.
- Recipe, measured by Doom64 (amiga/panflip_amiga.c, GPL-2-or-later, same
  author, so usable under GPLv3):
  - an own 640x960 BGRA32 screen, plus a backdrop window for IDCMP;
  - mglCreateContextFromBitMap into an own 32-bit friend bitmap;
  - per frame: mglSwitchDisplay, BltBitMap into the hidden half (1 ms),
    then RyOffset + ScrollVPort, which waits for the blank.
  - Result: 58.8 fps locked to 60 Hz. The wait spins the CPU (a timer
    sleep first; raised priority is forbidden).
- Zod: a branch in gl_oeffnen (port/amiga/sdl_screen.cpp), only when the
  library's id string says PiStorm3D and no VideoCore.card 2+ is loaded.
  window_ptr becomes our backdrop window. The pointer is already drawn by
  the engine (gl_bild_zeigen), so nothing changes there.
- Cost: defect 3 (bitmap contexts leak ~16 KB a round and slow down after
  ~5 rounds). Zod makes its context once per start and per resolution
  change. A ZodLaunch switch "Sync" (default on with PiStorm3D).
- Effort: medium (~400 lines from Doom64, adapted to Zod's SDL layer).
- **Recommended: yes.** It is what a PiStorm player sees first.

**B. The HUD as a layer: the last ~2.4 ms on the A4000D.**
- Problem: GL redraws the whole HUD every frame (zplayer ReRenderAll).
  Software draws only what changed into the compose surface. A4000D:
  HUD 5.0 ms in GL against 2.6 ms in software.
- Idea: in GL, the HUD draws in software, incrementally as the software
  path does, into a persistent 8-bit layer. Around zhud.DoRender:
  SetUseOpenGL(false) and SetMainSoftwareSurface(layer); no ReRenderAll.
  GL then draws the layer's two HUD rectangles (the right column and the
  bottom bar) as two quads at the end of the frame.
- Needed in zod_gl: a few dirty rectangles per texture instead of one
  bounding box. Otherwise the clock (top right), the minimap and the chat
  bar union into nearly the whole layer each second.
- Hazard: the layer is uploaded in frame N and last drawn at the end of
  frame N-1, so usually no wait.
- Gain: ~2 ms of ~36 ms, about 6 % on the A4000D; nothing on the PiStorm
  (game logic bound).
- Risks: the portrait animation (frequent small uploads); team palettes.
- **Recommended: later, or not at all.** Small gain for a change in how
  cego's HUD is driven.

**C. M6 preparation (no code).**
- A README section in English: "Made with AI" (the RadeonGL convention),
  what the GL renderer does, the measured tables, and how to choose it
  (ZodLaunch "Renderer", or -o for software).
- The licence question for cego: the MiniGL SDK's AmigaOS-only clause next
  to GPLv3 (GLTron and Doom64 link the same SDK). cego decides.
- The shape of the hand-over: branch gl pushed to cego's repo (we have
  write access) or a PR from bubbob42/ZodEngine-GL. Toolchain note:
  bebbo gcc 6.5 with libstdc++; his Docker image now ships GCC 16.
- **Recommended: yes, after A.** The user writes to cego; I never send mail.

## 5f. Overnight 2026-11-18/19 (the user: "continue optimizing through the night without asking back -- max FPS on the 4000D, tear free on the PiStorm, cego's software path still working on non-GL Amigas and as fast as possible")

Branches, each on the one before, none pushed:

| branch | commits | what | measured |
|---|---|---|---|
| `pistorm-present` | 3af6c02 | PiStorm3D: a present of our own (port/amiga/zod_present.cpp, Doom64's panflip recipe) | PiStorm orig01: 56-57 fps on the 60 Hz output, every frame synced; refreshes missed 50-79 of ~1700; mouse y stays 0..479 |
| `gl-fps-2` | 43e6a8c | PC profile of the -B run (ZOD_PROFILE, tools/prof_report.py) | first profiles below |
| | e7dc207 | RGBA uploads as ARGB4444 words on RadeonGL, even atlas places | A4000D B Q Q B: 26.2 / 25.3 -> 28.3 / 27.9 fps (+9 %) |
| | 83ae128 | tests/auto/NoGL: no minigl.library -> software by itself | FS-UAE: passes |
| | a2b00fb | profile starts after ZOD_PROFILE_SKIP s | -- |
| | fe89c8b | ZodLaunch: MiniGL is the default renderer | launchtest passes; ZodLaunch builds in cego's image |
| `sw-speed` | 1e9cb89 | software path: the copy gap from the machine's own prices | A4000D M P P M: 19.8 / 19.9 -> 22.1 / 21.5 fps (+10 %) |
| | 685dc7d | software path: dirty blocks summed, only changed ones sent (where a byte into the screen is dear) | A4000D P O O P still: 22.0 / 21.4 -> 24.3 / 23.1 (+9 %), 5 KB a frame sent instead of 54; but scrolling -9 % |
| | 73b4100 | frames that scroll are copied plainly (after three summed frames that marked and changed more than half the screen) | still O I I O 23.3 / 23.3 vs 22.6 / 23.5 (same), scrolling P I I P 7.2 / 7.2 vs 7.1 / 7.2 (same) |

Tried and dropped (measured, not committed; patches in build/hw/):
- **Map items' server shortcut** (868 of ~1000 objects skip six no-op calls):
  server.process 5.1 -> 4.7 ms, but the frame got slower through code layout
  elsewhere (objects +0.45 ms in both runs) -- the 060 layout lottery
  swallows a 1 % gain.
- **Inline sort comparator**: K L L K 27.6 / 28.2 against 29.1 / 25.7, noise.
- **The flip's shadow** (skip unchanged bytes): exact (FS-UAE check: 0 of
  666 M screen bytes differ from the shadow, and 0 from the compose
  surface), and 96 % of the copied bytes were unchanged -- but no gain on
  the A4000D: there the CPU reads its own card RAM at ~40 ns a byte and
  writes the Radeon at ~120 ns, so reading compose AND shadow costs about
  what the saved writes did. build/hw/flip-shadow.patch. Its successor,
  per-block sums read from the compose surface alone, is 685dc7d.
- **The HUD as a GL layer** (concept B): worse, 23.6 / 24.2 -> 23.3 / 21.4
  fps. The software HUD redraws small parts nearly every frame; as texels
  they cost ~240 ns each over the bus (16 M px a minute against 1.3 M).
  build/hw/hud-layer.patch.
- **Client arrays** (one glDrawArrays a batch instead of the per-vertex
  calls, RadeonGL only): picture right (FS-UAE checker 0), but N v v N
  24.2 / 27.5 against 26.1 / 23.3 fps, the HUD 2 ms slower in both array
  runs -- RadeonGL's array path converts each vertex's colour, immediate
  mode keeps one current colour. build/hw/gl-arrays.patch.
- **The limit of measuring**: the same binary varied by 13 % inside one
  series (bot matches differ: fights, effects). Gains under ~5 % no longer
  show reliably; a seeded, repeatable benchmark would be the next tool.

What the profiles say (A4000D, GL, orig01, 40 s after a 15 s warm-up):
program 80 % of the running samples, minigl.library 15 %, the OS 5 %. The
game's own logic is about half the frame (the server's object loop alone
~17 %: ~130 units at ~36 us each per tick; the 868 map items cost only
~0.4 ms). Texture uploads are down to 1.7 %; the vertex stream to the card
(ToVram) 5 %, the per-vertex calls ~5 %. A profile from frame one is mostly
start-up (map download, first uploads) -- hence the skip.

A4000D facts measured tonight: memory -- CPU card 128 MB (pri 40, from
$08000000), motherboard 16 MB (pri 30); card RAM reads ~40 ns/byte with a
plain longword loop; Radeon VRAM writes through the Mediator ~120 ns/byte;
a copy call ~0.5-0.65 us. Runs of the same binary drift by up to 15 %
between blocks on a long-running machine: compare only inside one A B B A
series, reboot before a series.

Software path on the A4000D, orig01 still, from tonight's start to the
branch tip: ~19.8 fps (gap 8, everything marked sent) -> ~23 fps (gap 0 +
block sums), about +17 %; scrolling unchanged at ~7.2 fps (bus-bound: a
scrolling frame is 300 KB of new bytes at ~120 ns each). Idea for later:
let the Radeon shift the screen itself while the view scrolls (a blit in
its own memory) and send only the new edge and the sprites.

Waiting for the user's eye: tearing on the PiStorm (the present), the
4444 picture on the A4000D (FS-UAE frames match software), and the software
path with gap 0 on the A4000D (by construction it copies a subset of the
same dirty blocks). Not done: a tear-free software path on the PiStorm (the
same panned screen, copying the dirty blocks of the last two frames) -- GL
is the launcher's default now and tear-free there.

## 5g. Morning 2026-11-19 (the user: "merge everything, then optimize the hell out of the whole thing -- we need >30 fps, that includes gl scrolling"; "it does not have to stop at 30")

The three overnight branches are merged into `gl` (local). Where the frame
goes on the A4000D at the tip (60 s, the game's report):

| | frame | server | client game | net | drawing sections | rest of "zeichnen" |
|---|---|---|---|---|---|---|
| orig01 still | 41 ms | 8.2 | 5.4 | 0.7 | 19.4 | 7.3 (the present) |
| orig04 tour | 81 ms | 18.5 | 9.1 | 2.7 | 47.0 | 3.5 |

**Uploads into a texture drawn this frame.** Counted on our side by the
library's own rule (a texture stamped by a glBegin since the last
mglSwitchDisplay): 402 of them in a 30 s FS-UAE run against the library's
402 idle waits, 644 against 642 in the tour. ALL came from atlas uploads
(new images and run-time slots into a page already drawn from); map tiles,
the minimap and other refreshes never. Now such an upload is HELD: its
pixels go into the page at the next frame's first draw, until then the image
draws from a texture of its own (ZOD_GL_HOLD=off: at once). FS-UAE and the
A4000D: 0 idle waits.

**The map as one texture** where it fits (RadeonGL, at most 6 MB at 16 bit:
orig01, orig04; p08's 4096x4096 keeps the ring of tiles), all of it sent
when the map is made (ZOD_GL_MAPRES=off: the ring). A changed part of a
tile (crater, zone marker, shadow) now goes up alone instead of the whole
128x128 tile: a quarter of a tile on average (FS-UAE: 249 parts in 30 s
still, 387 in 60 s tour).

A4000D, t2 (one texture + held uploads) as a against the same binary with
both switched off as b:

| series | a | b | |
|---|---|---|---|
| orig04 tour, a b b a | 14.9 / 14.7 | 12.9 / 12.7 | +15.5 %; map 11.7 -> 5.9 ms |
| orig01 still, a b b a | 24.7 / 22.9 | 27.4 / 25.5 | -10 % |

**Why the still view got slower: RadeonGL's pan pacing.** RadeonGL pans once
a refresh at most: a pan less than a refresh after the last one (17.35 ms at
640x480) sleeps until the blank ("pans held for the last pan's blank" in its
log): holds in 641 / 1232 of ~1480 frames with a, 427 / 635 with b, 6.2 /
11.7 s of 1 ms sleeps a run (b 3.6 / 5.7 s). That is also the 13 % spread of
one binary inside a series (old logs: 34 to 822 holds a run). The present's
cadence log (sdl_screen.cpp, in every GL report from t4 on) showed the work
between Zod's presents at 17..28 ms in all but the first frames -- they do
not bunch. MiniGL then found the cause in the library: its line model learns
only from flushes long enough for the beam to move 48 lines, Zod's flushes
are short, and until three samples are in the refresh rule is off and every
pan waits for the beam. Early in a run, for a variable number of frames.
The fix belongs in RadeonGL (MiniGL proposes it to the user); nothing to do
on Zod's side.

**Rocks out of the server's per-tick passes** (ZOLists::server_olist;
ZOD_ROCKS_IDLE=off: all objects). A rock's ProcessServer changes nothing,
but each pass touched several cache lines of every rock: the PC histogram
inside ZObject::ProcessServer (tools/prof_report.py --addr) put the samples
on its first stores into the object (sflags.clear), and those inside
ZServer::ProcessObjects on the update loop's flag tests. Three passes skip
the rocks: ProcessServer, the update loop, and DeleteObject's "forget this
object" pass. FS-UAE, the same maps: server.process -47 % (still) to -57 %
(tour), server.update -58 to -75 %, p08 46 -> 64 fps.

A4000D, t5 (+ changed map parts alone, rocks out of the server passes) as e:

| series | e | other | |
|---|---|---|---|
| orig01 still, e g g e (g: uploads at once) | 32.4 / 34.0 | 32.8 / 31.2 | the held uploads no longer matter |
| orig01 still, e f f e (f: rocks in every pass) | 33.7 / 22.9 | 27.7 / 23.1 | pan holds rule it (one e run slept 13 s); ~+10 % with the sleeps taken out |
| orig04 tour, e f f e | 17.5 / 17.7 | 15.1 / 15.3 | +15 % |

**Uploads, timed** (t7..t12: by origin, first of a frame against later ones,
every upload of 10 ms or more). The tour spent ~6 ms a frame in upload
calls. Most of a seemingly moving 2 s chunk was the ATLAS PAGE CLEAR: a new
1024x1023 page cleared with zero RGBA bytes, converted by the library texel
by texel, 17 strips at ~120 ms = 2 s a page -- the first at the start, the
second mid-run as a 2 s frame; the clear had been booked to whichever upload
kind came last. As ARGB4444 words: ~20 ms a strip, ~0.3 s a page. The map's
first send is ~0.7 s (14 strips of 128 rows). Own textures at their own size
(no power of two, no mip chain) saved heap but not time.

**Rocks as one image** (ORock::DoRender): the three 16x16 parts of a rock
never overlap, so one 16x48 image per combination of parts gives the same
pixels with one test and one blit (FS-UAE: no difference on any rock edge,
in GL or software). **Draw order on cached keys**: the same insertion sort
on keys read once per object, without sort_objects_func's out-of-line call
per comparison (4.2 % of the A4000D tour). **Bots** run SmoothMove without
the map items, as the player's client already does.

A4000D t11/t12 tour: 18.9 / 19.0 fps (objects 8.6 -> 7.4 ms). The series h
(t12, all on) i i h was cut short at ~08:06: the Mac left the home LAN (all
Amigas unreachable, gateway changed) -- not an A4000D fault. The sort and
rock changes have no clean A/B yet.

**Objects drawn: only the band around the view** (RenderObjects). The draw
list is sorted by key each frame; the sort keeps its keys, two binary
searches give the objects with keys in [view top - 32, view bottom + 128].
Margins from a census (ZOD_BANDCENSUS on: every object that drew in
DoRender, how far its key lay outside the view): over orig01 still, orig04
and p08 scrolling, 1.83 M draws, never above the view's top, at most 47 px
below its bottom (a rock), and 0 draws outside the band. FS-UAE: 18 / 34 /
12 % of the objects visited; GLResHold and SwSums pass. ZOD_BAND off: all.

**The bots' 0.6..1 s tick** is their map download at the start of a match
(it sits between the "map_data_size" lines: each bot parses the map and
builds its path tiles), not play. It is in the -B window (~1..2 s of a
60 s A4000D run) but no hitch in a game.

Where the A4000D stands (orig01 still / orig04 tour, 60 s): GL from ~24 /
12.4 fps at this morning's tip to 32..34 / 19.0 fps (t12; the band, t15, not
yet measured there). The tour's frame is ~52 ms: server ~9.5, client game
~7.5, net ~1.8, drawing ~33 (objects 7.5, map ~3.7 with the changed parts,
HUD 3.8, map effects 3.6, GUI 2.0, obj_vor 2.0).

## 5h. Evening 2026-11-19 and the backlog

Measured on the Radeon A4000D (.132), 60 s bot matches:
- **RadeonGL's PanPace fix** (MiniGL, branch pan-sample): Zod t17, still,
  p q q p: main13 37.6 / 31.1 fps with 68 / 702 held pans, pansample 38.7 /
  38.0 with 2 / 2. The holds were the run-to-run noise of every A/B before:
  in GL the present is booked into "draw", so a run with many holds looks
  4..8 ms slower. A/B series now stage minigl-pansample.library (ZLIB).
- **The band** (t18, installed 0.16, j k k j): objects 3.0..3.2 -> 1.55 ms
  still, 7.4..7.9 -> 4.5 ms scrolling; still 37.8 / 36.2 against 32.1 / 29.5
  fps; scrolling hidden by the holds (18.7 / 20.5 against 19.5 / 19.1).
- **Library time per drawing section** (93495f6): the HUD's 3..4 ms are its
  own code, not the library (0.34..0.85 ms). Its parts (t19): minimap 1.6 /
  2.2 ms (still / scrolling; builds of 2.4 / 3.7 ms four times a second,
  the 62x85 refresh ~2.2 ms each), the GL-only full redraw 0.9 ms, the
  clock 0.4..0.5 ms.
- A4000T (arkon, 060, labelmaster-ff), software, orig01 still, R S S R x2:
  cego's v1.1 33.6 fps, ours (t18) 61.2 fps = 1.82x; the flip 14..15 ->
  3.5..4 ms (dirty blocks with sums, 6 % of the marked bytes sent).
- Radeon A4000D, orig01 still, R G G R x2, installed RadeonGL 0.16: cego's
  v1.1 (software) 19.4 fps, ours (t23, GL) 34.1 fps = 1.76x (the G spread
  31..37 is 0.16's pan holds; with pansample the same build ran 36..39).
- **The card RAM is the cost** (E-clock timers in the draw order, t23): on
  the A4000D every pass over a list of ~1900 pointers costs ~0.3 ms (reads
  at ~40 ns a byte), so keeping the rocks' keys (e688ebc) saved nothing in
  the sorts -- the passes over the arrays cost what the object reads did.
  What helped is not passing at all:
  - cc20dd2 two views per list, rocks sorted only when the lists change:
    the sorts 3.0 -> 0.6 ms scrolling, 1.4 -> 0.44 ms still;
  - 98031b2 minimap from the split object list: a build 3.7 -> 2.8 ms;
  - 72c8c74 hut animals off the view not asked: the pre-render 1.4 -> 0.6 ms;
  - 43c0335 map effect tiles with kept positions: 3.8 -> 3.4 ms.
  Each is exact by a check mode run in FS-UAE (ZOD_SORTCHECK, ZOD_SPLITCHECK,
  ZOD_MINICHECK, ZOD_HUTCHECK, ZOD_EFFCHECK: 0 differences); the only
  difference is the draw order of a rock and another object on EQUAL keys
  (now the rock first) -- for the eye test.
- Where the library's time goes (MiniGL named Zod's hot spots in
  pansample): _ToVram 5.4 % (the vertex stream copied into the card's ring
  over the bus), the vertex hook 3.4 %, texture uploads 2.6 % -- per vertex,
  no cheaper entry; on Zod's side the lever is fewer quads.
- **sin/cos of turned sprites kept** (fps-night, ZOD_TRIGCACHE): birds
  and turned effects called sin and cos (software on the 060, ~23 us a
  pair) for every turned sprite every frame; angles are quantized, so the
  pairs are kept (99 % reused). A4000D (pansample, 1 2 2 1): birds 0.43
  against 1.1 ms scrolling, 0.27 against 0.76 ms still; still 42.6 / 42.3
  against 40.2 / 39.4 fps; scrolling 24.3 / 22.1 against 24.0 / 24.1 (the
  22.1 run had a big fight: effects 1.8 ms, server 9.5 ms).
- **Held atlas uploads re-measured** after the white-box fix (now two real
  uploads per new image: the temporary texture and the page copy): A4000D
  (pansample, t30, 3 4 4 3) scrolling 23.8 / 23.6 held against 24.0 / 24.8
  not held, still 41.9 / 43.7 against 40.8 / 41.1 -- a wash (not held:
  fewer uploads, but ~550 idle waits a run). The default stays (held).
- **The server's deletion pass left out while nothing is set to die**
  (fps-night, ZOD_KILLFAST): ZServer::ProcessObjects asked every object
  KillMe() each tick (~1900 on orig04, one pass over card RAM); a counter
  of objects set to die (DoKillMe up, the destructor down) lets it skip the
  pass while that is 0 -- on the tour it ran 14..17 of ~1460 ticks.
  A4000D (pansample, t32, 5 6 6 5): server.loeschen 80 / 83 against 533 /
  551 us; scrolling 24.3 / 24.3 against 24.3 / 23.2 fps, frame 41.0 / 41.0
  against 41.1 / 42.9 ms.
- **The after-effects drawn from a band around the view** (fps-night,
  ZOD_AFTERBAND): the after-effects list walked whole for a handful in
  view; now from the sort keys' band (32 above, 256 below the view) -- a
  census of the after-effects first (ZOD_BANDCENSUS) gave the band's edges.
  FS-UAE GLCheck: 0 failed binds; 21..34 thousand of 53..65 thousand
  visited. A4000D (pansample, t34): obj_nach tour 1049 / 1069 against
  1414 / 1517 us, still 716 / 785 / 696 / 717 against 860 / 830 / 700 /
  822 us. The frame rates swap sides between the two still series (7887:
  40.7 / 36.9 against 42.2 / 43.0; 8778: 44.4 / 40.6 against 40.3 / 39.9;
  tour 24.5 / 24.8 against 24.9 / 24.5) -- the slow runs lost their time in
  the map's library time, effects and zones, which the band does not
  touch. Kept: the section it changes is lower in every pair.
- **The buildings' sounds looked for among the buildings** (fps-night,
  ZOD_SNDLIST): PlayBuildingSounds walked every object (~1900 with the
  rocks) each frame for two flags; ols.building_olist (~50) gives the same
  (ols.AddObject fills both; the order does not matter). FS-UAE SndCheck
  (ZOD_SNDCHECK, both ways every frame): 0 of 11 108 frames differ, radar
  heard in 3467, a factory in 259. A4000D (pansample, t37, z 0 0 z):
  zeichnen.klaenge 105 / 126 against 572 / 592 us; 24.8 / 24.6 against
  24.0 / 25.4 fps.
- **Where the bots' 1.8 ms go** (census, t37, A4000D tour): socket 0.94
  ms a frame (the loopback packets and their handlers), SmoothMove 0.60 ms
  (43 objects a tick, two bots, ~2 ticks a frame: ~7 us an object -- the
  two floor() calls go through ___fpu_round_to_integer on the 060), AI
  0.03 ms (once a second).
- **floor() as fintrz and a compare** (fps-night, ifloor in common.h,
  ZOD_FASTFLOOR): libm's floor on the 060 is a call into a bit-by-bit
  ___fpu_round_to_integer (_floor and it: 1.4 % of the tour's samples);
  (int) is one fintrz there, one step down for negative non-integers gives
  the same int. SmoothMove (server, bots, player), ProcessMove and the hut
  animals. FS-UAE FloorCheck (ZOD_FLOORCHECK: both at every call): 0 of
  1 995 584 calls differ, 476 823 of them took the step down. A4000D
  (pansample, t38, f1 f0 f0 f1, tour): 26.3 / 25.1 against 24.4 / 24.8
  fps; game 4448 / 4598 against 5177 / 5069 us (the Process pass 2002 /
  2033 against 2239 / 2306: the hut animals), bots' SmoothMove 503 / 534
  against 693 / 699 us.
- **The atlas pages made while loading** (fps-night2, ZOD_GL_PREPAGES,
  default 2): a page is cleared in 16 strips of ~19 ms; on an orig04 tour
  the second page came mid-game (a ~0.3 s hitch). Now both are made in a
  load step of their own ("TEXTURES", behind the load bar, RadeonGL
  only); pages are taken lowest first, so every image lands where it did.
  FS-UAE LoadBar, GLSmoke, GLTour pass (pages 0..1 made in 86..99 ms
  there). A4000D (pansample, n1, p1 p0 p0 p1, tour): 26.7 / 27.6 against
  25.0 / 25.5 fps, frames over 100 ms 9 / 10 against 14 / 14, over 50 ms
  104 / 62 against 119 / 114; the load step takes ~745 ms instead of 84 ms
  (page clears 640 ms). Part of the fps gap is likely the fights (server
  time differs too: 7.8 / 7.5 against 8.9 / 9.1 ms) -- the hitch is what
  it removes for sure.
- **The water tiles' start timers in a compact array** (fps-night2,
  ZOD_WATERDUE): DoEffects walked all water tiles' 20-byte records (734 on
  orig04) every frame for their start timers -- a third of DoEffects in the
  tour profile, the load of each record's time. Now the times sit in one
  array in the list's order (rebuilt when the list changes, once a run),
  the record is touched only when due. FS-UAE EffCheck (the copy against
  the records every frame): 0 differ; SwSums, NoGL, GLSmoke pass. A4000D
  (pansample, n2, w1 w0 w0 w1, tour): map effects 3267 / 3255 against
  3612 / 3483 us; 26.9 / 25.9 against 26.6 / 26.1 fps (the 25.9 run: server
  9.0 ms).
- **REJECTED: zod_gl_blit's texture coordinates kept per texture**
  (ZOD_UVCACHE, n3, not committed): the last four UVs and their source
  rectangle, place and page size kept in the texture's struct, to skip the
  four single divisions a quad. Bit-identical (FS-UAE UVCheck: 0 of ~3.9
  million kept uses differ; kept 91 % on the still view, 66 % scrolling).
  But A4000D (pansample, u1 u0 u0 u1, tour): 26.3 / 25.9 against 27.3 /
  26.5 fps, HUD 2740 / 2643 against 2466 / 2530 us -- writing twelve fields
  to the struct on every miss (card memory) costs more than the divisions.
  Reverted. Divisions are not the cost; memory traffic is.
- **Objects beside the view left out of DoRender** (fps-night2,
  ZOD_XCULL): the objects' band (the rows near the view) spans the whole
  map's width, the view only part of it. The band census got a sideways
  part (ZOD_BANDCENSUS): objects that drew lay at most 15 px left of the
  view (map items) and 1 px right of it (orig01 still, orig04 and p08
  scrolling, ~1.8 M draws) -- 64 px margins each way. The render view keeps
  each entry's x and x + w beside its key (read in the same pass,
  moved by the same sort); the merge leaves out what lies wholly beside
  the columns. FS-UAE XCullCheck (the left-out ones drawn anyway, every
  quad counted): 0 of 3.1 M drew; 23 % of the band left out on orig01,
  43 % on orig04, 82 % on p08; SwSums, NoGL, GLSmoke pass. A4000D
  (pansample, n4, tour, two series x1 x0 x0 x1 and x0 x1 x1 x0): objects
  4104 / 4098 / 4136 / 4118 against 4895 / 5100 / 4908 / 4950 us; the sort
  +0.1..0.2 ms (the x arrays); 27.4 / 26.4 / 27.3 / 27.8 against 26.9 /
  27.2 / 27.0 / 26.1 fps (means 27.2 against 26.8).
- **The rocks' pre-render band left out while their shadows are baked**
  (fps-night2, ZOD_ROCKPRESKIP): baked, every banded entry (only ORock is
  banded) returns at DoPreRender's first line; the band was still searched
  (two binary searches reading rocks) and called (~110 virtual calls a
  frame on orig04). Now one comparison of the map's two generations skips
  it. FS-UAE RockPreCheck (ZOD_ROCKPRECHECK: the band run anyway in every
  baked frame, any quad counted): 0 frames drew; SwSums, NoGL, GLSmoke
  pass. A4000D (pansample, n5, r1 r0 r0 r1, tour): obj_vor 802 / 703
  against 1118 / 1077 us; 27.1 / 28.4 against 26.7 / 28.3 fps.
- **8-bit own textures send only what differs** (fps-night2, ZOD_REFDIFF):
  the minimap marks its whole surface at every rebuild (~4 a second) and
  was sent whole each time (ARGB4444, ~4.6 ms on the A4000D); the
  portrait likewise. Now zod_gl keeps the pixels as last sent and sends
  only the box around what really differs (or nothing); the first refresh
  sends the whole surface, so the copy and the texture start out the same.
  FS-UAE RefDiffCheck (ZOD_REFDIFFCHECK: an image of the texture by what
  was sent, against the surface after every refresh): 0 of 295 unlike;
  23..35 % of the marked pixels sent. A4000D (pansample, n6, d1 d0 d0 d1,
  tour): refresh uploads 513 / 494 against 1289 / 1270 ms a run, HUD 2163
  / 2492 against 2668 / 2675 us; 28.9 / 25.4 against 27.0 / 26.2 fps (the
  25.4 run: server 8.9 ms).
- **The map items left out of the server's per-tick passes** (fps-night2,
  ZOD_SRVMAPSKIP): the process and update passes ran over every object but
  the rocks, ~300 of them map items (trees, crates, huts, flags, grenade
  boxes) that all run ZObject's own ProcessServer -- ~10 cache misses each
  a pass, for nothing. Now both passes run over non_mapitem_olist (the same
  relative order). FS-UAE SrvMapCheck (ZOD_SRVMAPCHECK: both passes as
  before, every map item's bytes compared before and after its
  ProcessServer with its two time stamps put back): 0 of 1 184 284
  changed; SwSums, NoGL, GLSmoke pass. A4000D (pansample, n7, m1 m0 m0 m1,
  tour): server.process 2720 / 2552 against 4145 / 4086 us, update 204 /
  194 against 482 / 465 us; 27.8 / 28.8 against 26.4 / 27.1 fps.
- **REJECTED: static map objects as fixed sort keys** (ZOD_MAPOBJFIX, n8,
  not committed): plain map objects, grenade and rocket boxes never move
  and have an empty Process(), so they could join the rocks in the views'
  fixed part (sorted once, left out of the Process pass). A4000D
  (pansample, k1 k0 k0 k1, tour): sorts 653 / 576 against 719 / 794 us,
  Process pass about the same, 27.9 / 29.2 against 29.6 / 27.9 fps -- and
  FS-UAE SplitCheck counted ~9 places a frame where a map object and
  another object tie on the key (idle units stand on the tile grid like
  the trees) and now draw in the other order. A visible-order change for
  ~0.15 ms: reverted.
- **REJECTED: the GL batches as client arrays** (ZOD_GL_ARRAYS, n10, not
  committed): each batch's quads into vertex / texcoord / colour arrays and
  one glDrawArrays(GL_QUADS), instead of glTexCoord2f + glVertex3f per
  vertex -- to save two library calls a vertex. r200view pictures of
  frame 300 (FS-UAE GLSmoke dumps) the same apart from moving units. But
  A4000D (pansample, g1 g0 g0 g1, tour): 24.9 / 23.4 against 28.4 / 26.1
  fps, drawing 27.0 / 27.9 against 21.4 / 22.1 ms, the flush at the present
  576 against 138 us; FS-UAE showed it too (510 against 663 fps).
  RadeonGL's unlocked array path (core/array.c: Fetch per attribute, the
  byte colours converted per vertex) costs more than its immediate calls.
  Reverted.
- **The bots' "socket" time is their start**: a packet census in ZBot
  (FS-UAE, 20 s): ~1 packet a frame for both bots together, most of them
  ADD_NEW_OBJECT -- the whole map's objects sent to each bot at the start,
  the one-second peak of server.bots. Averaged over a 60 s run that peak
  alone is ~0.6 ms "a frame"; the bots' steady cost is well below the
  1.7 ms average. Not a target.
- **The waypoint pass left out while no unit shows its waypoints**
  (fps-night2, ZOD_WPSKIP): DoRenderWaypoints ran for every unit each frame
  and returned at its first line unless show_waypoints was on (3 s after
  an order). A count of the objects showing them (ShowWaypoints up,
  ProcessObject and the destructor down) skips the pass while it is 0.
  FS-UAE WPCheck (ZOD_WPCHECK: the pass run anyway in those frames, any
  quad counted): 0 of 31 151 frames drew; SwSums, GLSmoke pass. A4000D
  (pansample, n11, tour): wegpunkte 28 / 28 against 187 / 177 us; 29.1 /
  30.7 against 29.4 / 28.7 fps -- the first scrolling run over 30 fps.
- **REJECTED: the bots move only robots and vehicles** (ZOD_BOTMOVE, n12,
  not committed): buildings and cannons never move, SmoothMove only wrote
  their centre again (FS-UAE BotMoveCheck: 0 of 642 296 changed). The bots'
  SmoothMove 359..370 against 502..523 us in all eight A4000D runs, but the
  frames over both series (bm1 bm0 bm0 bm1, bm0 bm1 bm1 bm0): 28.9 against
  30.0 fps on average -- path finding 1.38 against 0.89 ms and the bots'
  start-up peaks decided it, not SmoothMove. Chance or an indirect effect
  on the bots' timing cannot be told apart with eight runs; the totals are
  worse, so not kept (0.15 ms at stake).
- **fps-night2 as a whole, 20.11 ~03:40** (A4000D, pansample, G0 = gl as
  pushed 17d5074, N13 = fps-night2 73b3df4; G0 N13 N13 G0 each): orig04
  scrolling 26.7 / 26.5 against 29.0 / 30.2 fps (frame 37.6 against 33.7
  ms, drawing 23.8 against 21.0 ms); orig01 still 46.1 / 46.5 against 50.5
  / 49.4 fps (server 4.8 against 3.4 ms). FS-UAE on the tip: SwSums,
  NoGL, GLSmoke, GLTour, LoadBar, HudKeep pass.
- **The draw order reads again only the movers' keys** (fps-night2,
  ZOD_MOVEKEYS): every frame each view's dynamic entries had their keys
  (and the render view's x ranges) read again -- but only robots and
  vehicles move; buildings, cannons, huts, flags and the map objects keep
  theirs. A flag per entry, moved by the sort, keeps theirs. FS-UAE
  MoveKeyCheck (ZOD_MOVEKEYCHECK: the kept ones read anyway every frame):
  0 of ~6.7 M had changed; SplitCheck, XCullCheck, SwSums, GLSmoke pass.
  A4000D (pansample, n14, two tour series): sorts 519 / 548 / 545 / 534
  against 768 / 796 / 850 / 803 us; frames 34.13 against 34.17 ms over
  eight runs (server 5.7..8.1 ms either way).
- **The software path's dirty grids set only when read, under GL**
  (fps-night2, ZOD_DIRTYLAZY): ZMap::DoRender called zod_dirty_force_full
  at every frame the view moved, which set both block grids whole -- ~60
  small memsets, the profile's memset callers (grid_alles, 0.85 % of the
  samples) -- though under GL nothing reads them while the game runs. Under
  GL it now only notes that they are due; every reader (the sweep, the
  copy's runs, zod_dirty_clear) sets them first, so it sees what it saw
  before. tools/prof_report.py --callers NAME found the callers. FS-UAE
  SwSums, GLSmoke, GLTour, LoadBar, NoGL pass. A4000D (pansample, n15, two
  tour series): map section 2377 / 2603 / 2490 / 2361 against 2952 / 2689
  / 2758 / 2771 us; 29.2 against 29.25 fps over eight runs.
- **REJECTED: zone tiles, the view test for water tiles only**
  (ZOD_ZONEWATER, 6684790, reverted in 701d668): DoZoneEffects asked
  GetBlitInfo for every tile of the zones in view with water (~130 a frame
  on orig04) though only the ~14 water tiles do anything after it. Exact by
  the code; FS-UAE SwSums, GLSmoke, GLTour passed. On the rebooted Hamiller
  (pansample, two tour series): zones 1047 / 1004 / 1055 / 1057 against
  1338 / 1263 / 1276 / 1269 us -- but the frames 34.78 against 33.87 ms
  on average over the eight runs (server +0.4, game +0.25, the other
  drawing +0.15 ms). Code this change does not touch got slower; on the
  060 a small change can move the layout of unrelated hot loops. Measured
  worse, so reverted, like the bots' SmoothMove.
- **Hamiller's drops** (~23:28 and ~04:04, both during zod_run.py's setup
  while a ~2.4 MB binary went up in one write; its network is USB on a
  ~4.8 V rail): zod_run.py now sends programs in 256 KB pieces with pauses
  and joins them on the Amiga -- the next binary went up in 10 pieces
  without a drop.
- **Where the present's 3.7 ms go** (t37, A4000D tour): flush 0.12 ms,
  mglSwitchDisplay 3.7 ms, present_show 0.02 ms. The PC profile (4000
  samples) shows no waiting (0.0 %): the library works -- _ToVram 5.9 % of
  all samples, _Upload 2.5 %, most of them with no program caller on the
  stack, i.e. inside mglSwitchDisplay. ~156 quads a frame at ~120 ns a
  byte into card memory: ~24 us a quad at the present alone, on top of the
  vertex calls. Fewer quads is the lever (per frame: objects 61, map
  effects 35, HUD 31, zones 15).
- **The user's white boxes** (videos 21:03..21:07, the fort's countdown,
  the snakes, any new animation frame: a white box of the image's size
  for 1-2 frames): held_take made a new image's temporary texture inside
  an open batch (glBegin), where RadeonGL rejects the bind, the
  parameters and the upload -- the texture had no image, the quad drew in
  its vertex colour. Since 3e5c455 (held uploads, 19.11 07:30) every run's
  RadeonGL log said "first GL error raised by glBindTexture"; runs with
  ZOD_GL_HOLD=off never. Fixed in 17261a8 (batch_end first); ZOD_GLCHECK
  on checks every bind: 378 of 71 492 failed before, 0 after (FS-UAE), 0
  of 37 297 on the A4000D. My first answer (the hit flash, from mapping
  the time to a software run) was wrong: the log line was the clue.
- **A kept HUD under GL** (ZOD_HUDKEEP, on by default since the user's eye
  test):
  the HUD drawn only where it changed or something drew over it, three
  frames for RadeonGL's three buffers. A4000D: scrolling 24.5 / 25.1
  against 23.0 / 23.8 fps, still 45.1 / 45.7 against 37.3 / 41.9; the eye
  test ran 21:17 (t29, 120 s scrolling, 26.3 fps). **The user's verdict,
  19.11 ~23:40** (merged gl 8e5091a: kept HUD on, the white-box fix, the
  load bar; orig01, 150 s, the user scrolling by hand with the pointer
  over the HUD's edges): "no problems detected, looking good" -- no stale
  or garbled parts, no pointer trails, no white boxes, and the load bar's
  look accepted. The run's counters: 4712 of 5709 frames kept the HUD, 996
  drew it whole (812 of them because something drew over it first, 111
  after, 6 a part changed, 67 asked for); HUD quads 10.4 a frame against ~31 drawn
  whole. The user, right after: "yes, hud on by default" -- on by default
  since then (ZOD_HUDKEEP=off draws the HUD whole every frame, for A/Bs).
- **The load bar on the A4000D** (same run): all steps 22.7 s; the
  longest EFFECTS 3.1 s, PORTRAITS 3.0 s, VEHICLES 2.7 s, UNITS 2.3 s,
  ANIMALS 2.3 s, BUILDINGS 2.1 s -- the numbers a faster start would work
  from.
- **The A4000D stopped answering at ~23:28** (no ping, no amiagent) while
  zod_run.py set up the second run of a floor A/B (t38, ZOD_FASTFLOOR=off):
  its blanker check answered, the data check did not; no game had started,
  the first run (t38, fast floor) had ended normally a minute before. The
  user rebooted it. Cause unknown; nothing in our logs (the run's log
  stays in RAM: until the end). The floor A/B is to be run again.
- Where the A4000D stands (pansample): orig01 still 38..43 fps, orig04
  scrolling 24 fps (this morning 32..34 / 19.0; at last night's tip ~24 /
  12.4). The scrolling frame (~41 ms): server ~8.8, game ~4.8, net ~1.6,
  drawing ~26 (objects 4.7, HUD 3.7 of it the minimap 1.9, map 3.6, map
  effects 3.4, obj_nach 1.3, zones 1.2, birds 1.1).

Backlog (not started):
- **Music through camd.library** (the user: "Add MIDI to the backlog -- it
  could save even more cycles"). cego's v1.1 README: "Music output: AHI or
  Paula supported. CAMD/MHI support is planned." ZMU1 is already a MIDI-like
  event stream (converted XMI, fixed 1/120 s steps), so a camd output path
  would send its events as MIDI to an external or card synth and skip
  mischer_fuellen() and the 16-bit AHI stream (or the Paula path) entirely.
  What it saves: the mixer process (port/amiga/musik_ausgabe.h: 38 per mille
  on the V1200) plus AHI's bus traffic -- only in runs with sound on; the
  -B benchmarks run with -s. Sibling: Doom64's MIDI-via-camd concept
  (camd 37.x has the serial driver, 40.x does not).
  Two MIDI flavours (the user: "add general and mt-32 midi support for our
  new prismamidi and mt-32-pi"):
  - **General MIDI** for PrismaMIDI -- the Prisma session's GM synth on the
    Prisma card (PrismaSynth on the VS1063a, camd driver DEVS:Midi/prisma,
    branches midi-m1..m5 in prismamegamix-midi, not released yet). Needs a
    GM program for each of cego's instruments (128 melodic + percussion on
    channel 10). From the Prisma session: the cluster is "prisma.out.0", a
    normal camd GM output; on camd 37 the unit must be in
    ENV:sys/midi.prefs (PrismaMIDI PREFS writes it, camd 40 loads DEVS:Midi
    itself). Update from the Prisma session (branch midi-m6): the driver
    takes the card when a client links to "prisma.out.0" and shows a public
    port "PrismaSynth" while the synth runs (~0.7 s after the link); before
    the first notes wait for it (Forbid; FindPort("PrismaSynth"); Permit;
    poll every tick, give up after ~3 s -- the card may be busy), then send
    GM System On and play.
  - **MT-32** for the mt32-pi (the user's, in MT-32 mode; also a real MT-32
    on the PiStorm's serial port through camd 37's internal driver). Needs an
    MT-32 patch map, or the song's own MT-32 setup if the original XMI
    carries one -- open: which device the XMI music was written for.
  One output path for both (camd cluster from ZodLaunch, e.g. out.0 or the
  Prisma cluster), the flavour choosing the program map and the reset sent
  at the start (GM System On, or the MT-32 reset). A ZodLaunch "Music"
  choice: Mixer (AHI/Paula) / General MIDI / MT-32.
- **Retained HUD under GL** -- DONE: the kept HUD (ZOD_HUDKEEP), on by
  default since the user's eye test on 19.11. Was: draw the HUD only when it changed or something
  drew over it, for as many frames as the swap chain has buffers; needs the
  map and objects clipped to the view (glScissor on RadeonGL). ~1.7 ms a
  frame, both views; a picture change that needs the user's eye.
- **AGA: PAL HiRes Laced** (the user, 19.11 night). cego's README: "AGA is
  NOT supported yet ... Everything the engine draws already goes through
  one shared 256 colour palette, which is the hard part. What is missing is
  an AGA screen and the copy path to it. The video player already supports
  AGA." What it would take:
  - A screen of 640x512 interlaced, 8 bitplanes (OpenScreenTags with
    PAL_MONITOR_ID|HIRESLACE_KEY, depth 8), the shared palette loaded
    with LoadRGB32. The engine lays out from the screen size (HUD at
    w-648 / h-484), so 640x512 gives 32 more map rows; or 640x480 centred.
  - The copy path: the software renderer already draws 8-bit chunky into
    its compose surface and copies only the changed blocks (dirty blocks
    with sums, 685dc7d). For AGA those blocks go through chunky-to-planar
    (c2p) into the bitplanes instead of a byte copy to RTG VRAM. Two
    buffers in chip RAM (2 x 320 KB) and ChangeScreenBuffer or a pan.
  - The cost: AGA chip RAM takes ~7..14 MB/s from an 040/060 (16/32-bit
    writes, the chipset's slots), so a FULL 640x512 frame is 320 KB =
    ~25..45 ms -- full redraws (scrolling) stay slow; the still view
    changes ~6 % of its bytes a frame (A4000T measure) -> ~2..3 ms. A
    fast c2p (Kalms-style, 060: ~1 cycle a pixel) adds ~0.3 ms a still
    frame. Scrolling would want a copper-free hardware scroll of a larger
    planar map buffer -- a renderer of its own, out of scope at first.
  - Laced flicker: the HUD's thin lines flicker at 25 Hz; an option for
    640x256 non-laced (every second line) or a flicker fixer.
  - The pointer: a hardware sprite (the system pointer) instead of the
    drawn one, so it costs no c2p.
  - Parts that change: port/amiga/sdl_screen.cpp (screen open, the copy
    path, the palette), zod_schirm (mode choice), ZodLaunch (an "AGA"
    entry). Not the engine, not the GL path.
- **Vsync while scrolling under RadeonGL** (the user, 19.11 night).
  How our RadeonGL GLQuake does it (gl_vidamiga.c, menu.c "Display sync"):
  one chooser for the four ways a frame reaches the screen -- off (copied
  at once, tears), vsync (copied behind the beam, slower: 29.9 against
  32.2 fps at 720p), ScrollVPort (three buffers panned, fast; RadeonGL
  0.15's cure B waits for the beam before the first frame into a left
  third, tear-free by a blind eye test), ChangeScreenBuffer (slow); vsync
  is the default since 1.5.1. Zod today: three buffers panned
  (sdl_screen.cpp, mglEnableSync on, ZOD_GL_SYNC off for benchmarks).
  What Zod would need:
  - the same "Display sync" choice in ZodLaunch (off / vsync / panned),
    panned as the default on RadeonGL 0.15+, vsync on older libraries;
  - MiniGL's PanPace fix (branch pan-sample, measured here: 68..702 held
    pans -> 2) so the panned path paces at most one pan per refresh
    without the early holds;
  - even motion: the view scrolls by time (StartMouseScrolling and the
    tour use the clock), so frames that alternate between two and three
    refreshes show as judder at 22..26 fps. A frame cap at a divisor of
    the refresh (30 fps at 60 Hz, 25 at 50 Hz) when the frame rate stays
    above it, presented on the beam (the library's line model knows when),
    would give even steps; below it, presenting every frame is better;
  - a tearing eye test on the A4000D while scrolling (MiniGL waits for
    one anyway before merging pan-sample).
- **Soft scrolling, for GL and for AGA and software RTG too** (the user,
  19.11 23:30: "soft scrolling for gl -- should be possible with AGA and
  software RTG, too").
  - What the game does now: the view's position is in pixels; the arrow
    keys, the mouse at the edge and ZOD_TOUR move it 400 px/s times the
    frame's time (zplayer.cpp, shift_speed). At 22..26 fps that is a jump
    of 15..18 px a frame, and the jumps differ because the frame times
    differ (and frames alternate between two and three refreshes). Every
    frame draws the whole view again; on the software path every block
    changes, so the dirty-block copy becomes a full copy (640x480 8-bit:
    300 KB a frame over the bus).
  - What smooth needs, on any renderer: the same step on every frame the
    display shows, and frames on the display's beat. Best at the full
    refresh (50 Hz PAL: 8 px a refresh at 400 px/s); a fixed divisor
    (25 fps at 50 Hz, 16 px) still reads as smooth; anything in between
    judders. So: a frame cap at a refresh divisor while the frame rate
    holds above it, the step taken from the presented frame count (speed /
    cap) instead of the clock, and the present on the beam -- the same
    pacing as the vsync item above. On orig04 the A4000D sits at 24..25
    fps scrolling, right at the 25 fps edge: the >30 fps goal is what makes
    a steady 25 possible.
  - GL: per-pixel offsets are already free -- the map is one texture
    (1376x1760, drawn as one quad per strip), the view's position only
    moves its texture coordinates. Missing: the pacing above.
  - Software RTG: scroll on the card instead of copying the whole view.
    The part of the view that stays visible moves by an on-card blit
    (BltBitMap VRAM to VRAM, the card's blitter on most RTG boards, no bus
    traffic); only the newly exposed strip (16 px x 480 at 25 fps) and the
    changed blocks of units and effects go over the bus -- close to the
    still view's ~6 % of a frame instead of 100 %. The HUD stays where it
    is (the blit covers the map area only). Needs: the compose surface
    shifted the same way (a memmove in fast RAM) so the block sums stay
    valid, and the dirty-block code told about the shift. Alternative:
    the whole map in one VRAM bitmap (1376x1760 8-bit = 2.4 MB) panned by
    ScrollVPort -- but then the HUD scrolls with it and must be drawn
    again at every offset; the on-card blit is the simpler fit.
  - AGA: hardware scroll with ScrollVPort over a planar buffer larger than
    the display (RasInfo RxOffset/RyOffset -- graphics.library, no copper
    of our own, no register writes), the exposed strip and the changed
    blocks through c2p; a wrap-around buffer (every new row drawn twice,
    the offset taken modulo the view height) keeps chip RAM bounded. The
    open point is the HUD: it sits at the right edge and the bottom, so a
    second ViewPort (top/bottom split) cannot hold it; it would be drawn
    again through c2p at each offset (~75 KB a frame) or the AGA layout
    gets a HUD row only. Cost per frame at 7..14 MB/s chip RAM: strip +
    blocks ~2..4 ms instead of ~25..45 ms for a full frame -- the
    difference between unplayable and smooth on AGA.
  - A further option where the frame rate stays low (AGA, slower RTG):
    the view pans at the full refresh from a VBlank-driven ScrollVPort
    while the game draws units at its own rate; units then lag the ground
    by up to a frame ("swim") -- the classic trade, an eye test.
  - Order: GL pacing first (with the vsync item), then the software RTG
    on-card scroll (helps the A4000T, PiStorm without GL, every RTG
    Amiga), then AGA together with the AGA item above.
- **Atlas page clears** -- DONE by ZOD_GL_PREPAGES: the "32 a minute"
  were the 2 x 16 strips of the two pages; in the last A4000D tour (gl,
  n17) all 32 lie in the load step (22.3..23.0 s after the start, before
  the bench clock), the atlas never starts again (2 pages, 432 images,
  only ~270 000 px of them -- the second page comes from the shelf
  classes, not from the pixels).
- **Map effect tiles** (ZMap::DoEffects): every effect tile of the whole
  map gets a division and a view-test call per frame, every water tile a
  due check -- ~1.9 ms of a scrolling frame on orig04; precomputed tile
  positions and an inline view test keep the behaviour exact.
  Evening 20.11, the n14 tour profile (A4000D, after EFFPOS and
  WATERDUE): DoEffects 193 samples of 4000 (~1.6 ms a frame) -- the
  water timers' walk 57 (the compact array: memory and fcmp.d), the due
  water tiles 61 (rand() twice and two divs.l each), the effect-tile list
  74. A water tile is due again 0.2..0.5 s after its last check
  (min_interval_time + (rand() % 4) * 0.033), so ~100 of orig04's 734
  are due EVERY frame: a heap of due times would save only the walk
  (~0.45 ms) and must keep the list order of the due ones (the rand()
  stream is the server's too, in the same process). What remains is
  inherent to the original's design (a 1-in-40 draw per tile every
  ~0.25 s); a geometric draw of the time to the next start would be
  statistically the same but changes the random stream -- not exact,
  not done.
- **The bench's first frame holds the map's first send** (gl, n17 tour on
  the A4000D): the 14 strips of the whole-map texture (orig04, 52 ms in
  the call each, ~95 ms with the 8-bit -> RGB565 conversion) go up on the
  first draw, after the bench clock started (main.cpp takes start_time
  after zplayer.Setup(); the client gets the map over the loopback socket
  only then): ~1.3 s of the 60 s, ~2 % of the bench figure, and a ~1.3 s
  stand-still for the player right after the load bar. Moving it behind
  the load bar needs the client's map during Setup (in single player the
  server has it already).
- **Per-quad cost in our code** (n14 tour profile): zod_gl_blit 168
  samples self (~1.4 ms a frame, ~9 us a quad at ~150 quads): the
  sub-texture loop's four fsdiv for u0/v0/u1/v1 and the int -> float
  moves 76, the calls (batch_begin, quad_colour, four vertex()) 44, the
  prologue/epilogue saving fp2..fp7 and d2..a6 44. A whole-image blit of a
  one-tile texture always gets the same four UVs: computing them once per
  texture would be bit-exact and save the divisions (~0.3 ms a frame, ~1
  %, below the A4000D's A/B noise alone). The library's side of a quad
  (dispatch 6.4 %, the vertex hooks ~5.5 %, plus the vertex stream inside
  mglSwitchDisplay) is MiniGL's per-quad concept (census of Zod sent to
  the MiniGL session 20.11 09:29, build/census/run2).

## 5i. The backlog night, 20./21.11. (the user: "start on the backlog items, run through the night")

Concepts and results in their own documents: docs/concept-smooth-scrolling.md
(GL smooth scrolling and the software-RTG on-card scroll),
docs/concept-display-sync.md, docs/concept-midi-camd.md, docs/concept-aga.md.
Branches smooth-gl, display-sync, rtg-scroll, midi-camd, aga, loadbar-fix,
merged as night-merge and then into gl (21.11. ~00:50, the user's word).

- Smooth scrolling: ON by default, N = 2 fixed (the user's eye test on
  Hamiller: N = 2 by hand "feels better" than off; the automatic N "far
  worse"). The refresh is measured from PanPace's returns (~2 s at start).
- Display sync: -Y pan|blank|now, ZodLaunch "Display sync".
- Software RTG on-card scroll (ZOD_RTGSCROLL, off): -69 % bytes in FS-UAE,
  but on the PiStorm 55 fps against 185 (a 7 ms blit against nearly free
  bytes); a Zorro A/B decides.
- MIDI through camd: -M gm|mt32, -K cluster (default out.0), ZodLaunch;
  byte-exact FS-UAE checks; the original MT-32 data waits for the Z CD.
- AGA: -C pal|dblpal|prod, the fallback without RTG; the user on voltago:
  "looking good, no complaints"; PAL/DblPAL at 640x512; scrolling is
  chip-bound (PiStorm's A1200 8 fps, hardware scrolling is the next step).
- The load bar no longer comes back for a moment after loading: a frame
  that drew nothing is not presented (the game's first frame before the
  map arrived showed an older buffer).

## 5j. The rest of the night, 21.11. (the user, ~01:20: "continue implementing through the night without asking")

- **gl pushed** to the private origin only, 62943ba..51a6d79, after all 10
  FS-UAE tests passed on the merged build.
- **MiniGL's rect lists, measured with Zod** (Hamiller, n17 060, 60 s, p q
  q p against pansample): quadrect2 tour 28.3 / 27.7 against 29.9 / 29.5
  fps, still 45.0 / 45.9 against 45.3 / 50.5; quadrect3 (the cheaper one)
  tour 28.9 / 28.4 against 30.1 / 30.7, still 47.4 / 49.9 against 50.9 /
  49.1. mglSwitchDisplay 0.4..0.7 ms cheaper a present, but the library's
  time inside Zod's drawing calls +0.4..1.0 ms (~6..7 us a rectangle to
  detect and move it; nearly every Zod quad is a rectangle: 134..140 a
  frame on the tour). By the user's stop rule Zod does not gain.
- **Profile of the gl tip** (prof-gl21-tour, Hamiller, pansample, orig04
  tour, 4000 samples): program 73.9 %, library 19.6 %, outside 6.5 %, no
  waiting. Zod's GL glue (zod_gl_blit 5.8, vertex 3.4, tex_get 2.4,
  batch_begin/_end 1.6, quad_colour 0.5) ~14 % = ~33 us a quad, spread
  over cache misses rather than one hot spot; ZMap::DoEffects 5.0 % (the
  original's per-tile random draws, see 5h); A* path finding ~3.3 %.
- **REJECTED before building: the texture lookup's hash** (branch perf-n1,
  census only): tex_get looks a surface up by address in an 8192-slot table
  with linear probing and tombstones. FS-UAE GLTour census: 1.54 slots a
  lookup (at most 14..29), 6..7 tombstones in the table at the end -- no
  long chains, so nothing to gain there; tex_get's cost is the reads of
  the surface and texture records.
- **NOT ADOPTED: a smaller path budget under smooth N = 2**
  (ZOD_PFBUDGET=8000 against the default 32 ms, an env knob only; gl21
  060, pansample, smooth on, orig04 tour, b32 b8 b8 b32): fps 22.6 / 23.9
  against 24.6 / 23.8; frames late for their blank 17.7 / 15.0 % against
  13.7 / 19.8 %; frames over 50 ms 134 / 123 against 70 / 110, over 100 ms
  11 / 9 against 4 / 4; server.wegjobs peaks 37.5 / 39.2 against 25.8 /
  30.7 ms. Fewer very slow frames, but the share that misses its blank --
  the stutter under N = 2 -- does not move, and units would get their
  paths later (cego's 32 ms was the user's choice on the V1200). Default
  kept.
- **AGA hardware scrolling** (branch aga-hwscroll, ZOD_AGA_HWSCROLL, off):
  see docs/concept-aga.md, milestone 4 -- FS-UAE 0 pixels differ; the
  PiStorm's A1200 tour 7.8 -> 10.9 fps (two buffers) / 16.6 (one).
- **AGA soft scrolling ON by default** (the user, 21.11. ~06:50: "merge
  the changes in, softscrolling becomes default"): two buffers, margins
  from the free chip RAM; -H off|on|fast, ZodLaunch "AGA scrolling".
  FS-UAE AgaCheck (the whole-view path kept checked with
  ZOD_AGA_HWSCROLL=off, the fallback with the default), AgaHwScroll,
  GLSmoke and the launcher test pass. night2-merge merged into gl.
- **AGA without RTG at the whole 640x512** (branch aga-fb512): the
  automatic fallback now runs the game at 640x512 like -C pal (the user's
  decision 7); FS-UAE AgaCheck 0 differ, GLSmoke and SwSums unchanged.

- **The environment switches in English** (the user, 21.11. ~07:35: "make
  all env switches english on/off"): every ZOD_ on/off switch is read by
  one helper, zod_env_switch (port/amiga/amiga_startup.cpp): on / off (also
  1/0, yes/no, true/false, any case); the old German ein/an/ja and
  aus/nein are still read so an old ENV: setting keeps working. The words
  with more choices: ZOD_FLIP_SUMS=check (old pruefen), ZOD_SMOOTH=
  off/on/scroll/always/measure/auto, ZOD_ZEIGER=picture (old bild),
  ZOD_SCHLAF=always/never (old immer/nie). The tests, the runner tools,
  the log lines and these documents now write on/off. tests/host/
  env_switch_test.cpp (gmake envtest) checks the word rules on the host.
  The variable names stay (some are German: ZOD_STEINE, ZOD_ZONEN,
  ZOD_ZEIGER, ZOD_LADEBALKEN, ZOD_LUECKE, ZOD_BREIT, ZOD_SCHLAF).
- **AGA: the side panel flickered while scrolling** (the user's video,
  21.11. ~09:56, the A4000T, "On (two buffers)"): two causes, fixed in
  aaa50a0 on aga-split -- (1) since the own bitmaps the first ScreenBuffer
  failed (SB_SCREEN_BITMAP on a screen with our bitmap) and "two buffers"
  quietly ran as one, writing into the shown bitmap; (2) ChangeScreenBuffer
  then ScrollVPort showed the new buffer at the old place for a frame that
  began between them -- now one ScrollVPort shows buffer and place
  together (ZOD_AGA_TOGETHER, default on). FS-UAE copper read: old order
  231 of 231 moving changes wrong, new 0. voltago with real two buffers:
  640x512 8.9 fps (one buffer 10.4), 640x400 12.3 (13.0); the morning's
  "two buffers" figures were one buffer (research-aga-speed 3.9, 3.10
  corrected). The A4000T has the fixed zod_060 for the user's eye.

Backlog, LAST (the user, 21.11. ~01:25: "ignore the center stutter for now
- put it at the end of backlog"):
- **The stutter "when scrolling over the map centre"** (the user's eye on
  Hamiller, hand scrolling, smooth N = 2: "maybe I'm imagining"). A
  measuring aid is ready on the local branch centre-stutter (worktree
  ../ZodEngine-centre): ZOD_SLOWMAP records each GL frame's work, server
  and game time by where the view's middle is on an 8 x 8 grid of the map,
  and the frames of 31 ms work and more (an N = 2 slip); FS-UAE
  SlowMapCheck passes (14 331 frames in the cells against the bench's
  14 332).
  Run 21.11. 02:23-02:28 (the rest of the backlog done or waiting for an
  eye, an ear, a machine or the Z CD): two orig04 tours on Hamiller,
  pansample, smooth N = 2, 60 s. Drawing (work - server - game) and the
  share of frames of 31 ms work and more, both runs together, rows 1..6 x
  columns 1..6 of the 8 x 8 grid (the outer ring is never the view's
  middle):

      row 1: start (map upload, the bots' start)  16.8 16.4 16.4 15.6  start
      row 2: 17.8 [18 %] 18.6 [20] 20.6 [36] 20.9 [33] 25.2 [27] 22.9 [30]
      row 3: 19.1 [14]   17.3 [15] 18.3 [8]  18.0 [13] 16.5 [9]  16.7 [16]
      row 4: 15.7 [10]   16.3 [8]  17.6 [9]  18.6 [11] 16.7 [7]  16.6 [16]
      row 5: 17.2 [16]   18.8 [13] 16.7 [12] 15.4 [8]  14.9 [13] 12.8 [9]
      row 6: 15.9 [17]   16.1 [12] 14.5 [7]  14.7 [8]  14.5 [6]  13.7 [9]

  The centre (rows 3..4, columns 3..4: 17.6..18.6 ms, 8..13 % slips) is
  not slower than the map around it. The slow band is row 2 (drawing up to
  25 ms, server ~10 ms against ~4.5 elsewhere, 20..36 % slips) in both
  runs -- but the tour always passes it 6..12 s into the match, so place
  and the match's early load cannot be told apart yet; a tour starting
  elsewhere would. In a red-against-blue bot match the fights also tend to
  be in the middle, where the user scrolled by hand: "the centre" may be
  "where the fight is" (server time and units on screen), not the map.

## 5k. The round end on Hamiller, 21.11. evening

After the fort fell, the next map came up with trails (a44cc48: the earlier
maps' textures deleted at a map change, refused textures asked again every
50 frames, black instead of nothing), then -- after that fix, at 1280x720 --
with a BLACK floor: the new map's texture still found no room.

- **Cause:** of the card's 16 MB texture heap, RadeonGL takes 4 MB for its
  upload cache and 0.5 MB for the vertex stream at the start, and the cache
  never shrinks below its start size for a texture (ucgrow.c,
  Core_UCacheShrink). At 1280x720 Zod's other textures take ~8.5 MB; the
  next map (1024x1376 = 2.8 MB as one texture, 1152x1440 = 3.3 MB, or the
  2048x1024 ring around the view = 4 MB) then fits nowhere. At 640x480
  (all the earlier tests) it did.
- **The cache serves locked vertex arrays only** (DrawCached); Zod draws
  immediate quads, "0 hits, 0 misses" in every log.
- **The fix:** Zod sets RadeonGLUploadCache=1 as a local variable for its
  own context (port/amiga/sdl_screen.cpp, like RadeonGLFlip), unless the
  user set the variable; it goes again at SDL_Quit. 3 MB more for textures.
- **Checked:** `tests/auto/RoundEnd720` (GL null card, 1280x720, heap capped
  at 16 MB, two round ends): before 292 refusals, the map drawn black in
  14843 frames, the heap at its 16.0 MB peak; after 0 refusals, 0 black
  frames, peak 13.1 MB. GLSmoke, GLCheck, HudKeep, LoadBarPresents,
  RefDiffCheck unchanged.
- **Still without eviction:** over a long session the other textures keep
  growing; 2.9 MB of room was left after three maps at 1280x720.

## 6. Open questions (recommendation first)

1. **Repository.** A GitHub fork of a public repo cannot be private.
   Recommended: a new PRIVATE repo bubbob42/ZodEngine-GL, with `upstream` =
   AMIGAbench/ZodEngine (push disabled locally) and the `gl` branch. Later,
   push `gl` to cego's repo (we have write access) or open a PR.
2. **Language and attribution in cego's tree.** Recommended: code comments in
   German like his (his files are UTF-8), commit messages in English like his
   one commit. Attribution: the RadeonGL family convention (no trailers, a
   "Made with AI" README note) unless cego wants otherwise.
3. **Look.** Recommended: pixel-exact with the software path first (NEAREST,
   cego's defaults: no spin). Free spin/scaling becomes an option after M4.
4. **Map approach (D4).** Decide after M1. Leaning (a) chunked textures if
   the census fits under ~12 MB of heap, else (b).
5. **Texture format (D5).** Recommended: GL_RGB5_A1 (ARGB1555) once
   gl13-phase1 is merged into RadeonGL. Until then, decide after M1: 4444 if
   the colours hold, 8888 if the heap allows.
6. **Pointer (D6).** Recommended: drawn as the last sprite (the look stays
   the same). The system pointer later if latency shows.
7. **Resolution.** Recommended: the launcher's sizes on a 16-bit screen
   context. A Workbench window (context from window) after M5, not in scope now.
8. **When cego hears of it.** Recommended: after M2 shows a picture on the
   A4000D. The user writes to him; I never send mail.
