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
  heap gives GL_OUT_OF_MEMORY.
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
  only copies (adc3502, ZOD_GL_565=aus to compare).
- **Map section in a still view** costs the same in S and GL (~7 ms): the
  game's own work, not drawing.

**After a reboot (18:59), D measured (still H D D H, tour E D D E; E = D with
ZOD_GL_565=aus):**

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
draws from a texture of its own (ZOD_GL_HOLD=aus: at once). FS-UAE and the
A4000D: 0 idle waits.

**The map as one texture** where it fits (RadeonGL, at most 6 MB at 16 bit:
orig01, orig04; p08's 4096x4096 keeps the ring of tiles), all of it sent
when the map is made (ZOD_GL_MAPRES=aus: the ring). A changed part of a
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
12 % of the objects visited; GLResHold and SwSums pass. ZOD_BAND aus: all.

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
- **The user's white boxes** (videos 21:03..21:07, the fort's countdown,
  the snakes, any new animation frame: a white box of the image's size
  for 1-2 frames): held_take made a new image's temporary texture inside
  an open batch (glBegin), where RadeonGL rejects the bind, the
  parameters and the upload -- the texture had no image, the quad drew in
  its vertex colour. Since 3e5c455 (held uploads, 19.11 07:30) every run's
  RadeonGL log said "first GL error raised by glBindTexture"; runs with
  ZOD_GL_HOLD=aus never. Fixed in 17261a8 (batch_end first); ZOD_GLCHECK
  on checks every bind: 378 of 71 492 failed before, 0 after (FS-UAE), 0
  of 37 297 on the A4000D. My first answer (the hit flash, from mapping
  the time to a software run) was wrong: the log line was the clue.
- **A kept HUD under GL** (ZOD_HUDKEEP, default off until the user's eye):
  the HUD drawn only where it changed or something drew over it, three
  frames for RadeonGL's three buffers. A4000D: scrolling 24.5 / 25.1
  against 23.0 / 23.8 fps, still 45.1 / 45.7 against 37.3 / 41.9; the eye
  test ran 21:17 (t29, 120 s scrolling, 26.3 fps).
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
- **Retained HUD under GL**: draw the HUD only when it changed or something
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
  (sdl_screen.cpp, mglEnableSync on, ZOD_GL_SYNC aus for benchmarks).
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
- **Atlas page clears**: 32 a minute while scrolling, ~20 ms each (the
  atlas holds fewer images than the scrolling view keeps needing).
- **Map effect tiles** (ZMap::DoEffects): every effect tile of the whole
  map gets a division and a view-test call per frame, every water tile a
  due check -- ~1.9 ms of a scrolling frame on orig04; precomputed tile
  positions and an inline view test keep the behaviour exact.

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
