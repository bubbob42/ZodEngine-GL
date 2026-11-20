#ifndef ZOD_MUSIK_CAMD_H
#define ZOD_MUSIK_CAMD_H
/*
 * Music through camd.library (docs/concept-midi-camd.md): the songs' ZMU1
 * events as MIDI to a camd cluster, timed by a process of its own. The
 * sound effects keep their AHI/Paula path through the mixer.
 *
 * The calls mirror musik_ausgabe.h's for the music part; sdl_addons.cpp
 * sends a song here instead of to the mixer when the game runs with
 * -M gm (or -M mt32).
 */
#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_AUS   0
#define MIDI_GM    1
#define MIDI_MT32  2

/* camd.library, the link to cluster, the player process. 1 = running. */
int  midi_start(int art, const char *cluster);

/* A song (ZMU1). It is copied; the caller's buffer may go at once.
 * schleife: start again at its end. 1 = taken. */
int  midi_spiele(const UBYTE *zmu, ULONG len, int schleife);

/* To a step (1/120 s) of the song: notes off, programs and volumes as
 * they stand there, play on from it. */
void midi_springen(ULONG schritte);

/* 1: notes off, the song holds; 0: on from where it held. */
void midi_pause(int an);

/* The music volume, 0..128: every channel's CC7 scaled by it. */
void midi_lautstaerke(int v);

/* A song plays (not held, not at its end). */
int  midi_laeuft(void);

/* Notes off, the link and camd.library closed, the process gone. */
void midi_stop(void);

/* One log line: what went out. */
void midi_bericht(void);

#ifdef __cplusplus
}
#endif

#endif
