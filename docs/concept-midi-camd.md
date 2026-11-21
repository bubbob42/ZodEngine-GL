# Concept: music through camd.library -- General MIDI and MT-32

Status: concept for review, 20.11. night. Implementation on the local
branch `midi-camd` after the concept, off unless chosen. Nothing pushed.
Ear tests in the morning (list at the end).

## The ask

- The user, 19.11.: "Add MIDI to the backlog -- it could save even more
  cycles."
- The user, 19.11. night: "add general and mt-32 midi support for our new
  prismamidi (prisma session) and mt-32-pi to the backlog."
- cego's v1.1 README: "Music output: AHI or Paula supported. CAMD/MHI
  support is planned."

## What the game has now

- The songs are ZMU1 streams (tools/cd/xmi2zmu.py; ZExtract makes them on
  the Amiga from the user's Z CD): the original XMI (Miles/AIL) events at
  a fixed 1/120 s step -- note on, note off, program change, CC7 volume;
  channel 10 (index 9) is percussion as in General MIDI. Nothing else
  (no pitch bend, no other controllers, no SysEx: "the pieces only use
  program changes and CC7", measured on the CD).
- The instrument bank `music/MIDIBANK` has 128 melodic programs plus the
  percussion keys: a General MIDI layout. The XMIs were written for a GM
  device; a GM synth plays them as they are.
- The mixer (port/amiga/musik_mischer.c) renders the stream with that
  bank into one stereo stream; port/amiga/musik_ausgabe.c sends it to AHI
  (16 bit) or Paula (8 bit) from a process of its own. Cost: 38 per mille
  on the V1200, plus AHI's bus traffic (only with music on).
- The engine jumps inside a song for its danger levels (musik_springen),
  pauses it (musik_pause) and sets its volume (musik_lautstaerke, 0..128).

## The camd output

A third way out for the songs: `-M gm` / `-M mt32`. The mixer
(musik_mischer.c) mixes the sound effects AND the music into one AHI or
Paula stream today; with camd it keeps mixing the effects (AHI) but no
music voices (up to 18 of its 32) and loads no MIDIBANK (~100 KB). The
songs' calls (Mix_PlayMusic, Mix_SetMusicPosition, Mix_VolumeMusic,
Mix_HaltMusic -- port/amiga/sdl_addons.cpp) go to port/amiga/musik_camd.c.

- **Module** `port/amiga/musik_camd.c`: opens camd.library (V37+),
  `CreateMidi`, `AddMidiLink(MLTYPE_Sender)` to a cluster (default
  `out.0`; PrismaMIDI's is `prisma.out.0`).
- **Timing**: a process of its own (as the mixer's, same priority -- no
  raised priority, standing rule) wakes every 1/120 s from timer.device,
  corrected against the E-clock so the song does not drift, and sends the
  events that are due with `PutMidi`. A long game frame does not touch it
  (the same reason the mixer has its own process).
- **Jump** (danger levels): all notes off, then the stream is read from
  its start to the target step WITHOUT notes, keeping the last program and
  volume of each channel, which are sent; play goes on from there.
- **Pause**: all notes off (CC123 + CC120 on 16 channels), the clock
  stops; resume sends nothing extra.
- **Volume**: each CC7 of the song scaled by the game's music volume
  (0..128), and re-sent on every channel when the volume changes.
- **Stop / quit / a failed start**: all notes off, the link removed,
  camd closed -- on every path.

### The two flavours

- **General MIDI** (PrismaMIDI on the Prisma card, the mt32-pi in its
  SoundFont mode, any GM module): GM System On (`F0 7E 7F 09 01 F7`) at
  start, then the song as it is.
- **MT-32** (the mt32-pi in MT-32 mode, a real MT-32 / CM-32L):
  - The MT-32 is not GM: its programs differ and its eight melodic parts
    listen on channels 2..9 by default, rhythm on 10.
  - A GM -> MT-32 program table (128 entries: the closest MT-32 preset
    for each GM program) and the song's melodic channels mapped onto
    parts 2..9 in order of first use (Z's songs use few channels -- to be
    measured on the CD).
  - The rhythm keys of GM and the MT-32 agree on 35..81 for the common
    drums; keys outside are dropped (counted).
  - At start: a reset of the MT-32 (system area SysEx), at quit the same,
    so the next program finds it as expected.
- **PrismaMIDI's own rule** (from the Prisma session): the driver takes the
  card when a client links to `prisma.out.0` and shows a public port
  `PrismaSynth` ~0.7 s later; before the first note wait for it
  (Forbid; FindPort; Permit; poll every tick, give up after ~3 s -- the
  card may be busy), then GM System On and play. camd 37 needs the unit in
  ENV:sys/midi.prefs (PrismaMIDI PREFS writes it); camd 40 loads DEVS:Midi
  itself.
- **camd versions** (Doom64's lesson): Commodore camd 37.x has a built-in
  serial driver (cluster out.0 on the serial port at 31250 baud, nothing
  in DEVS:midi needed); the Aminet camd 40.x has none (DEVS:midi/mmp). The
  game logs which camd it opened and which cluster it linked to.

## ZodLaunch

```
+- Audio ---------------------------------------------------------+
|  [x] Sound effects   [x] Music                                  |
|    Music output  [ General MIDI (camd)                     |v]  |
|   MIDI cluster   [ out.0_______________________ ]  (camd only)  |
|     Audio mode   [ ... ]                       (AHI only)       |
+-----------------------------------------------------------------+
```

Music output: AHI / Paula / General MIDI (camd) / MT-32 (camd). The
cluster field is ghosted unless a camd entry is chosen. The game gets
`-M gm` / `-M mt32` and `-K <cluster>` (a free option letter).

## Tests

- **FS-UAE**: camd.library 37.14 (as Doom64 stages it, dist/Libs/camd,
  local) sends cluster out.0 to the emulated serial port; the autotest
  captures the raw bytes (as Doom64's MusicCamd tests do) and a host
  checker compares them with what the song file says: every note on has
  its note off, the programs and volumes arrive, the timing at 120 steps a
  second within a tick, all notes off at stop. The test install has no
  songs (they come from the user's CD), so the test brings a small ZMU1
  of its own with known events (made by a host script; test material must
  discriminate: notes on several channels, a program change, a volume
  change, a jump).
- **Real hardware** (ask Prisma first, take turns): PrismaMIDI on voltago
  (and arkon, the A4000T, which has a Prisma card too); the mt32-pi on the
  PiStorm's serial port (camd 37's internal driver, as Doom64 proved).

## Milestones

1. M1: the camd output, GM flavour, FS-UAE byte check -- DONE 20.11.
   night (branch midi-camd): tests/auto/MidiCamd, its own test songs
   (tools/zmu_test_song.py: pseudo-random keys, a drum key outside the
   MT-32's range, a CC7 change, a 300-step wait), a jump
   (ZOD_MIDI_TESTJUMP=1500@6), tools/zmu_midi_check.py: every message in
   order against a reference player, notes paired; the player's own
   lateness at most 0.55 ms.
2. M2: MT-32 flavour (program table, part mapping, reset) -- DONE 20.11.
   night: tests/auto/MidiCamdMT32 (reset first and last, channels on parts
   2..9 in the order of first use, programs through mt32_prog, drum keys
   outside 35..76 dropped) passes. The program table is a first guess from
   the two preset lists: ear test.
3. M3: ZodLaunch choice and cluster field -- DONE 20.11. night: Music
   output AHI / Paula / General MIDI (camd) / MT-32 (camd), a "MIDI
   cluster" string ghosted unless camd (default out.0), saved as
   musikweg=0..3 and midicluster=; the start line gets -M gm|mt32 and
   -K <cluster> (a new game option; ZOD_MIDI_CLUSTER stays for tests).
   tests/launcher (gmake launchtest): the four outputs, -K only with camd,
   the cluster round trip -- passes. ZodLaunch builds in cego's Docker
   toolchain (the gcc 6.5 one has no MUI headers); its window not yet seen
   in an emulator.
4. M4: PrismaMIDI's port wait; real-hardware runs (Prisma's machines,
   with Prisma's word), the mt32-pi. -- The port wait DONE 20.11. night: a
   cluster named prisma* makes the player wait for the public port
   "PrismaSynth" (FindPort under a short Forbid every 20 ms, at most 3 s),
   then play; the log says how long. FS-UAE (MidiCamdMT32's last run, -K
   prisma.out.0, no Prisma card): "not there -- the music goes out anyway
   after 3000 ms", rc 0. The real-hardware runs wait for the user and the
   Prisma session (voltago and arkon are Prisma's; Zod's 24 MB of game data
   would have to go up first).

## The user's decisions (21.11. ~00:40, relayed)

- Default cluster: **out.0**.
- MT-32: use the ORIGINAL game's MT-32 data if the CD has it (Miles keeps
  MT-32 timbres in driver/timbre files next to the XMIs), the table above
  only as the fallback. Not checked yet: there is no Z CD or image on the
  Mac (z.pac searched for); with the CD (or ZExtract on the Amiga reading
  it) the MT-32 files can be looked for and carried into ZMU1 as SysEx.
- 21.11. ~07:35, the user: "cd install archive is on icloud drive
  Amiga/setup_z...". That is GOG's setup_z_2.3.0.8.exe (Inno Setup,
  322 MB), looked into with innoextract: it is the 2014 Windows remaster
  (Z.exe with SDL2/SDL2_mixer), its music recorded audio -- Ogg tracks
  (AA16, aC16, aJ16 planet themes, ipBATTLE16, ipWIN, ipLOSE,
  ipOPTIONS16) plus speech in de/en/... folders. No XMI, no MIDI, no Miles
  driver or timbre files, and Z.exe has no MT-32/Roland/MIDI strings. So
  there is no original MT-32 data to take; the GM -> MT-32 program table
  stays (the user's decision 6 fallback), to be set by ear. The original
  DOS CD (with the XMIs ZMU1 came from) would still be worth a look.

## Open questions (recommendation first)

1. Default cluster **out.0** (the serial port: mt32-pi, MT-32, any MIDI
   interface) -- or prisma.out.0 when PrismaMIDI is installed.
2. MT-32 program table: **a standard GM -> MT-32 table** (the closest
   presets), refined by ear -- or the original game's MT-32 driver data if
   the CD carries any (to be looked for: Miles .MT timbre files).
3. The mixer keeps mixing the effects only while camd plays the music
   (**yes**: the music voices are the saving).

## Ear tests (morning)

- PrismaMIDI (GM) and the mt32-pi in MT-32 mode, one battle song each,
  against the mixer's own rendering.
