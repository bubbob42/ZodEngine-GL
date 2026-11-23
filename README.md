# Zod Engine 2.1 — AmigaOS port

A native AmigaOS 3.2 port of the **Zod Engine**, an open source
re-implementation of *Z* (Bitmap Brothers, 1996) written by Nighsoft.

This is version 2.1. It is based on cego's port, version 1.1
([AMIGAbench/ZodEngine](https://github.com/AMIGAbench/ZodEngine)).
Version 2.0 added a 3D card renderer, a native AGA screen, music through
camd.library, smooth scrolling and many speed-ups; 2.1 makes the AGA screen
faster. The work is on the `gl` branch; `main` is still cego's v1.1.

**No game data is in this repository.** The graphics, sounds and music belong
to the Bitmap Brothers. They are read from the user's own CD of *Z*, on the
Amiga, during installation.

## What 2.1 adds over 2.0

**A faster AGA screen.** A still view compares only the blocks that changed
since that buffer was last shown. Chunky to planar conversion is done in 68k
assembler, with the C version as a fallback and a self-test against it at
the first frame. The rows of the hardware-scrolled screen compare ranges of
pixel groups at a time. On an A4000D with a 68060 (AGA, PAL, 23.11.2026)
the still view runs at 33 fps instead of 23, and scrolling at 10.2 to
11.1 fps instead of 9.7. The picture is unchanged, and so are the MiniGL
and software renderers. The measurements and the rejected attempts are in
[docs/concept-asm-speed.md](docs/concept-asm-speed.md).

## What 2.0 adds over v1.1

**MiniGL renderer.** When `minigl.library` opens, the game draws through it.
That is RadeonGL on a Radeon 9200 (R200) under Picasso96, or PiStorm3D on a
PiStorm. The whole map is one texture, images and text are packed into atlas
pages, and the HUD is kept between frames. Without the library the game
falls back to cego's software path.
- On a Radeon A4000D (still view, 19.11.2026) v1.1 ran at 19.4 fps and 2.0
  at 34.1 fps.
- Smooth scrolling is on by default: each frame is held for two refreshes,
  so the view moves in even steps.
- Display sync can be chosen (pan, blank or now).
- At a map change the old map's texture is freed, so the next map loads
  fully at 1280×720 on a 16 MB card.

**Software drawing on RTG.** Blocks whose pixels did not change are not
sent to the card again, and frames that scroll are copied plainly. On an
A4000T (68060, still view) v1.1 ran at 33.6 fps and 2.0 at 61.2 fps.

**AGA screen.** PAL HiRes Laced 640×512 (or 640×400) in 256 colours, no RTG
card needed.
- Two buffers, with chip RAM written a longword at a time and only changed
  blocks converted.
- Soft scrolling through the display hardware is on by default. The bottom
  panel gets a screen of its own while scrolling.

**Music through camd.library.** General MIDI or MT-32, sent to any camd
cluster. The player is a process of its own and sleeps on timer.device
between events.

**ZodLaunch 2.0.** Renderer (Software, MiniGL or AGA), AGA mode and AGA
scrolling, a Display group, General MIDI and MT-32 music outputs with a
cluster field. It starts with MUI 3.8.

**Game.** A load progress bar in Z's own look. Many server and drawing
passes are left out while nothing changes.

The release [v2.1](../../releases/tag/v2.1) holds `zod_040`, `zod_060`,
ZodLaunch and [a README](docs/release/README_GL.txt). It is an update to copy
over an installed v1.1 or 2.0.

## Hardware

AmigaOS 3.2 with a 68040 or 68060 and FPU (`zod_040`, `zod_060`). The 68080
build (`zod_080`, for the Vampire V1200) still builds from the same sources
but was not tested in 2.0 or 2.1. Fast RAM: 32 MB is comfortable. ZodLaunch needs
MUI 3.8 or newer.

| Renderer | Needs |
|---|---|
| MiniGL | `minigl.library`: RadeonGL 0.16 or newer (0.18 recommended) with a Radeon 9200, or PiStorm3D |
| Software (RTG) | Picasso96 |
| AGA | an AGA Amiga |

## Building

The game uses bebbo's gcc 6.5 for m68k-amigaos (its `bin` directory in
`ZOD_GCC6`, default `~/amiga-gcc6/bin`) and the MiniGL SDK from RadeonGL,
found at `../RadeonGL/sdk/minigl-29.1` by default (`MGL_SDK`):

    gmake gl-builds          # zod_040 and zod_060 in build/amiga-g6

ZodLaunch needs the MUI headers of the Docker cross toolchain:

    tools/build.sh amiga BUILD=build/amiga-g6 launcher

Tests run in FS-UAE, with `dist/` as DEV: and the game data from your own CD
in `../ZodEngine-data/run`:

    gmake autotest T=GLSmoke

The test scripts are in `tests/auto`. `gmake launchtest` checks the
launcher's logic on the host.

## Documents

* [docs/gl-plan.md](docs/gl-plan.md): the MiniGL port, its measurements and
  the release notes (section 5l)
* [docs/concept-aga.md](docs/concept-aga.md) and
  [docs/research-aga-speed.md](docs/research-aga-speed.md): the AGA screen
  and its speed; [docs/concept-asm-speed.md](docs/concept-asm-speed.md):
  the 2.1 speed work
* [docs/concept-midi-camd.md](docs/concept-midi-camd.md): music through
  camd.library
* [docs/concept-smooth-scrolling.md](docs/concept-smooth-scrolling.md),
  [docs/concept-display-sync.md](docs/concept-display-sync.md),
  [docs/concept-load-progress.md](docs/concept-load-progress.md)

## Licence

GPLv3, see [LICENSE](LICENSE). The Zod Engine by Nighsoft is GPLv3 and this
port inherits it.

Third party code included here under its own terms:

* `upstream/ZodEgine_Libs/QZod_DnSeparate/SDL_rotozoom.*` — zlib licence
  (Andreas Schiffler)
* `port/third_party/stb_image.h` — public domain / MIT (Sean Barrett)

The game data is **not** covered by that licence and is not distributed.

Made with AI: the 2.0 and 2.1 changes were written with Claude Code.
