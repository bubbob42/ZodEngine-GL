/*
 * camd_zod.h -- the few camd.library calls the music needs
 * (port/amiga/musik_camd.c), as GCC inline library calls.
 *
 * Neither the toolchain nor the NDK 3.2 has camd.library headers. The
 * offsets are those of camd_lib.fd (bias 30, the CAMD 37.1 developer
 * archive) and the values those of its midi/camd.h; Commodore's 37.x and
 * the Aminet reimplementation 40.x both keep them. The same facts as Doom 64
 * for AmigaOS's amiga/include/camd_amiga.h (same author).
 */
#ifndef ZOD_CAMD_ZOD_H
#define ZOD_CAMD_ZOD_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <inline/macros.h>

struct MidiNode;
struct MidiLink;

/* a link's direction (AddMidiLinkA's type) */
#define MLTYPE_Receiver 0
#define MLTYPE_Sender   1

/* CreateMidiA tags */
#define MIDI_Base       (TAG_USER + 65)
#define MIDI_Name       (MIDI_Base + 0)
#define MIDI_MsgQueue   (MIDI_Base + 6)
#define MIDI_SysExSize  (MIDI_Base + 7)
#define MIDI_ErrorCode  (MIDI_Base + 12)

/* AddMidiLinkA tags */
#define MLINK_Base      (TAG_USER + 65)
#define MLINK_Location  (MLINK_Base + 0)
#define MLINK_Comment   (MLINK_Base + 4)
#define MLINK_ErrorCode (MLINK_Base + 12)
#define MLINK_Name      (MLINK_Base + 13)

/* PutMidi's message: the MidiMsg's first longword (status, data 1, data 2,
 * port), big-endian */
#define CAMD_MSG(status, d1, d2) \
	(((ULONG)(status) << 24) | ((ULONG)(d1) << 16) | ((ULONG)(d2) << 8))

extern struct Library *CamdBase;

#define CreateMidiA(___tags) \
	LP1(0x2a, struct MidiNode *, CreateMidiA, struct TagItem *, ___tags, a0, \
	    , CamdBase)
#define DeleteMidi(___mi) \
	LP1NR(0x30, DeleteMidi, struct MidiNode *, ___mi, a0, \
	      , CamdBase)
#define AddMidiLinkA(___mi, ___type, ___tags) \
	LP3(0x54, struct MidiLink *, AddMidiLinkA, struct MidiNode *, ___mi, a0, \
	    LONG, ___type, d0, struct TagItem *, ___tags, a1, \
	    , CamdBase)
#define RemoveMidiLink(___ml) \
	LP1NR(0x5a, RemoveMidiLink, struct MidiLink *, ___ml, a0, \
	      , CamdBase)
#define MidiLinkConnected(___ml) \
	LP1(0x78, BOOL, MidiLinkConnected, struct MidiLink *, ___ml, a0, \
	    , CamdBase)
#define PutMidi(___ml, ___msg) \
	LP2NR(0x8a, PutMidi, struct MidiLink *, ___ml, a0, ULONG, ___msg, d0, \
	      , CamdBase)
#define PutSysEx(___ml, ___buf) \
	LP2NR(0x9c, PutSysEx, struct MidiLink *, ___ml, a0, UBYTE *, ___buf, a1, \
	      , CamdBase)

#endif
