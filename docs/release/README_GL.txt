ZOD ENGINE 2.1 -- AMIGA PORT
============================

Version: 2.1 (23.11.2026)
Based on: Cem "Cego" Goektas' ZodEngine v1.1 for AmigaOS 3.2


WHAT IS THIS?
-------------

An update for an installed ZodEngine v1.1. It adds a 3D card renderer
through minigl.library (RadeonGL), a native AGA screen, music through
camd.library (General MIDI and MT-32), smooth scrolling, and many speed-ups
in the game itself. Version 2.1 makes the AGA screen faster.

This archive holds programs only: no game data, no installer. Install
ZodEngine v1.1 first (it reads the game data from YOUR OWN "Z" CD), then copy
these files over it.


CONTENTS
--------

   zod_040        the game for the 68040
   zod_060        the game for the 68060
   ZodLaunch      the launcher, with the new choices (2.0, unchanged in 2.1)
   README_GL.txt  this file

zod_080 (Vampire) is not part of this update; keep the one from v1.1.


INSTALLATION
------------

   1. Install ZodEngine v1.1 as usual, if it is not installed yet.
   2. Copy zod_040, zod_060 and ZodLaunch into the ZodEngine drawer,
      replacing the old ones. Keep your ZodLaunch.info.
   3. Start ZodLaunch and check the Renderer, Display and Music choices.

Your ZodLaunch.cfg stays valid; the new settings get their defaults.
Coming from 2.0, only zod_040 and zod_060 have changed.


REQUIREMENTS
------------

   AmigaOS     3.2
   CPU         68040 or 68060 with FPU
   Memory      Fast RAM, 32 MB is comfortable
   MUI         3.8 or newer, for ZodLaunch

   For the GL renderer:
               minigl.library from RadeonGL 0.16 or newer (0.18
               recommended) and a Radeon 9200 (R200) under Picasso96.
               Without minigl.library the game falls back to software
               drawing on RTG, as in v1.1.

   For the AGA screen:
               an AGA Amiga; no RTG card is needed.

   For MIDI music:
               camd.library and a MIDI driver; a General MIDI or MT-32
               synthesizer on the chosen cluster.


WHAT IS NEW
-----------

In 2.1: the AGA screen is faster
   *  In a still view only the blocks that changed since that buffer was
      last shown are compared.
   *  Chunky to planar conversion in 68k assembler. The C version stays as
      a fallback, and the game checks the two against each other at the
      first frame.
   *  The rows of the hardware-scrolled screen compare whole ranges of
      pixel groups instead of testing every group.
   *  Measured on an A4000D with a 68060 (AGA, PAL, 23.11.2026): still
      view 23 fps with 2.0, 33 fps with 2.1; while scrolling 9.7 fps with
      2.0, 10.2 to 11.1 fps with 2.1.
   *  The picture is the same as in 2.0. The MiniGL and software (RTG)
      renderers are unchanged.

In 2.0, over v1.1:

Renderer (MiniGL / RadeonGL)
   *  Drawing through minigl.library when it is installed (ZodLaunch
      "Renderer: MiniGL", the default). The map is one texture, images and
      text go into atlas pages, the HUD is kept between frames.
   *  Measured on a Radeon A4000D (19.11.2026, still view): v1.1 19.4 fps,
      this version 34.1 fps.
   *  Smooth scrolling on by default: frames held for two refreshes, the
      view stepping evenly (ZodLaunch "Even steps"; the game's -X on|off).
   *  Display sync as a choice: -Y pan|blank|now (ZodLaunch "Display").
   *  A map change frees the earlier map's texture, so the next map loads
      fully, also at 1280x720 on a 16 MB card.

Software drawing on RTG
   *  Unchanged blocks stay off the bus, scrolling frames are copied
      plainly: on an A4000T (68060, still view) 33.6 fps with v1.1, 61.2
      fps with this version.

AGA screen
   *  PAL HiRes Laced 640x512 in 256 colours (or 640x400), two buffers,
      without any RTG card (ZodLaunch "Renderer: AGA").
   *  Soft scrolling through the display hardware, on by default with two
      buffers (ZodLaunch "AGA scrolling": Off, On, Fast with one buffer;
      the game's -H off|on|fast). The bottom panel sits on a screen of its
      own while scrolling.
   *  Chip RAM written a longword at a time, changed blocks only.

Music through camd.library
   *  General MIDI or MT-32 as music output (ZodLaunch "Music", the game's
      -M gm|mt32), sent to a camd cluster of your choice (-K <cluster>,
      e.g. out.0 for the serial port, prisma.out.0 for PrismaSynth's GM
      synth, prisma.out.1 for its MT-32).
   *  The music player is a process of its own above the game's priority,
      sleeping on timer.device between events.

Game
   *  A load progress bar in Z's own look.
   *  Many passes of the server and the drawing left out where nothing
      changes: less CPU time per frame on every renderer.

ZodLaunch
   *  Renderer: Software (RTG), MiniGL or AGA, with the AGA mode and AGA
      scrolling.
   *  Display group: sync and smooth scrolling.
   *  Music: AHI, Paula, General MIDI (camd) or MT-32 (camd), with a MIDI
      cluster field.
   *  Starts with MUI 3.8 (muimaster 19).


ENVIRONMENT SWITCHES
--------------------

Advanced settings and test aids are ENV: variables named ZOD_..., most of
them on or off (the docs in the source list them). ZOD_LOG=<file> writes
the game's log to a file. RadeonGL's own variables still work; Zod sets
RadeonGLUploadCache=1 for its own game unless you set it yourself.
New in 2.1: ZOD_AGA_ASM=off uses the C conversion instead of the
assembler one, ZOD_AGA_MARKS=off compares every block in a still view
again.


LICENCE AND SOURCE
------------------

The Zod Engine is free software under the GNU General Public License
version 3 (see LICENSE in the source). The source code of this version is
in the repository this release comes from, at the tag v2.1.

The game data (graphics, sounds, music) belongs to the Bitmap Brothers and
is not included.

Made with AI: the changes in this edition were written with Claude Code.
