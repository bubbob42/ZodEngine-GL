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
