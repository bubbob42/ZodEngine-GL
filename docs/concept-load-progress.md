# Concept: a load progress bar in Z's own look

Status: concept and mockup for review (19./20.11. night), implemented on the
branch `load-progress` the same night. Nothing pushed.

## The ask

The user, 19.11. night: "a progress bar with information during load like
doom has, but using a style that fits the game's bitmap brother visuals".

## What Zod shows today

- `ZPlayer` shows the original loading picture of the level's planet and
  team (`zod_screens.zpk`, 320x200 at 2x, centred, with its own palette)
  and writes "LOADING" on it in the original font (font19) -- or, when the
  archive is missing (the A4000D's test install), a black screen and
  nothing else (`zplayer.cpp`, "Ladebild ... Schirm bleibt schwarz").
- Then it loads synchronously: `ZTeam::Init`, `ZMap::Init`, `zhud.Init`,
  `ORock::Init`, `SetupSelectionImages`, and `Load_Graphics` with 80 steps
  (messages, sounds, cursors, objects, buildings, cannons, vehicles,
  robots, animals, effects, GUI ...). `Load_Graphics` already counts
  `loaded_percent` -- and nothing shows it. The "LOADING x%" of the Zod
  Engine's own splash was removed on 02.10. (the user wanted that splash
  gone; `zod_schirm.h` explains why a percentage on the original picture
  was dropped: the picture has its own palette).
- On the A4000D the whole start takes tens of seconds with a still picture.

## The Doom64 model (`Doom64/amiga/loadscreen_amiga.c`)

`Amiga_LoadBegin(title)`, `Amiga_LoadStep(what)`, `Amiga_LoadProgress(done,
total)`, `Amiga_LoadTick()`, `Amiga_LoadEnd()`: the step being done, a bar
(the share where it is known), the time so far; at most ten frames a
second, nothing for loads under 0.3 s; every step's time goes to the log.

## Design

![45 %](img/load-progress-45.png) ![90 %](img/load-progress-90.png)

(Mockups drawn from the game's own art by `tools/assets/load_mockup.py`
on the black screen the A4000D shows today; on an original loading picture
the same layout sits under its "LOADING".)

Everything in picture coordinates (320x200, the 2x layout of `zod_schirm`),
under the existing "LOADING" line (y 131):

- **The bar**: a bevelled plate in the picture's own grey ramp (VGA slots
  1..4, which every original full screen holds: `zod_schirm.cpp`), a dark
  recess, 44 LED segments lit in green from left to right. Green is the
  colour of the game's own displays (the forts' and factories' countdown).
- **The share** in the countdown's own digits (`green_building`, the font
  of the forts' timer), in a small recessed box right of the bar.
- **The step** under the bar in font19's small set, dimmed one step like
  the original's grey buttons: "TEAMS", "MAP", "HUD", "UNITS AND
  BUILDINGS", "SOUNDS", ... -- font19 has A..Z, 0..9 and . , ? ! * : -
  only, so no "%", no lower case.
- **Colours on a loading picture**: the four greys are the picture's own
  slots; the greens are the nearest entries of the picture's palette
  (looked up once per picture, as the old text path did). On the black
  screen (no picture) the shared palette has the exact greens.

What it does not do: no animation between steps (the load is synchronous;
the screen changes only when a step ends), no sound, nothing over the HUD
or the game.

## Where the time is reported

- `zod_lade_beginn()` when the picture (or black) goes up, `zod_lade_schritt(
  name, done, total)` at each step, `zod_lade_ende()` before the game's
  first frame. Load_Graphics' 80 steps give `done/total` (its own
  `loaded_percent` arithmetic); the steps before it get a fixed share each.
- At most four updates a second: every update redraws only the bar's region
  and marks only that region changed -- the software path copies those
  blocks, GL re-uploads that part of the screen's texture (a whole 640x480
  re-upload costs ~130 ms on the A4000D).
- Every step's time goes to the log: `[load] UNITS AND BUILDINGS 2340 ms`,
  and the total -- the numbers a later optimisation of the start needs.

## Both renderers

The loading screen is drawn in software into the 8-bit screen (`zod_schirm`
writes into the surface's pixels); `SDL_Flip` shows it -- by the dirty-block
copy on RTG without GL, by `zod_gl_show_surface` with GL. The bar uses the
same path, so it works on Amigas without GL unchanged.

## Switch

`ZOD_LADEBALKEN aus` (env) shows the old screen without the bar.

## Eye test (morning)

The bar on the A4000D (GL, black screen) and in FS-UAE (software); with the
original loading pictures if an install with `zod_screens.zpk` is at hand.
