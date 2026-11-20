/*
 * musik_camd.c -- the music as MIDI through camd.library
 * (musik_camd.h, docs/concept-midi-camd.md).
 *
 * The songs are ZMU1 streams (tools/cd/xmi2zmu.py): the original XMI's
 * events at a fixed 1/120 s step -- note on, note off, program change,
 * CC7 -- written for a General MIDI device (the mixer's MIDIBANK has the
 * GM layout: 128 programs, percussion on channel 10). So for GM they go out
 * as they are; only CC7 is scaled by the music volume.
 *
 * A process of its own ("ZodMIDI", priority 5 like the mixer's ZodMusik)
 * owns camd.library, the MidiNode, the sender link to the cluster and a
 * timer request. It sleeps until the next event, a command from the game
 * (Ctrl-F) or the end (Ctrl-C) -- it never spins -- and times the events
 * from the E-clock, so a long game frame does not touch the music.
 *
 * The game task hands a song over as a copy of its own (AllocVec); the
 * process frees it. Commands go through a semaphore-guarded block.
 * Model: Doom 64 for AmigaOS's amiga/music_camd.c (same author).
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "musik_camd.h"
#include "camd_zod.h"
#include "zod_log.h"

#define STEP_US       (1000000.0 / 120.0)   /* one ZMU1 step */
#define MAX_SLEEP_US  500000.0              /* keeps the E-clock deltas short */
#define EVENTS_MAX    2000                  /* events in one go */

struct Library *CamdBase = 0;

/* ---- shared between the game and the process ------------------------------ */

static struct SignalSemaphore cmd_sem;
static UBYTE *cmd_song = 0;        /* a new song (the process takes it) */
static ULONG cmd_len = 0;
static int cmd_loop = 0;
static int cmd_new = 0;
static LONG cmd_jump = -1;         /* a step, -1 none */
static int cmd_pause = -1;         /* -1 no change, 0, 1 */
static int cmd_vol = -1;           /* -1 no change, 0..128 */

static struct Process *m_proc = 0;
static struct Task *m_parent = 0;
static BYTE m_done_sig = -1;
static volatile int m_state = 0;   /* 0 starting, 1 running, -1 failed */
static volatile int m_fail = 0;    /* where it failed: 1 library, 2 node, 3 link, 4 timer */
static volatile int m_playing = 0;
static int m_art = MIDI_GM;
static char m_cluster[64];
static UWORD m_camd_ver = 0, m_camd_rev = 0;
static int m_heard = 0;            /* MidiLinkConnected at the start */

/* Test aid (tests/auto/MidiCamd): ZOD_MIDI_TESTJUMP=<step>@<seconds> -- once
 * a song has played that long, a jump to that step, as the game's danger
 * levels do (a 40 s test match rarely reaches one) */
static LONG t_jump_step = -1;
static double t_jump_after_us = 0.0;
static double p_song_start = 0.0;

/* statistics (the process writes, the report reads at the end) */
static ULONG s_events = 0, s_notes = 0, s_songs = 0, s_jumps = 0, s_loops = 0;
static double s_late_sum = 0.0, s_late_max = 0.0;
static ULONG s_late_n = 0;

/* ---- the process's own state ------------------------------------------------ */

static struct MidiLink *p_link = 0;
static UBYTE *p_song = 0;
static ULONG p_len = 0, p_pos = 0;
static int p_loop = 0;
static ULONG p_step = 0;           /* the step of the last event done */
static ULONG p_next = 0;           /* the step of the next event */
static int p_have_next = 0;
static double p_t0 = 0.0;          /* clock (us) at step 0 of this pass */
static int p_paused = 0;
static double p_paused_at = 0.0;
static int p_vol = 128;
static UBYTE p_cc7[16];
static int p_prog[16];
static UBYTE p_on[16][16];         /* notes sounding (output channel): a bit per key */
static signed char p_out[16];      /* the song's channel -> the output channel, -1 dropped */
static ULONG s_dropped = 0;        /* events of channels or drum keys the MT-32 lacks */

/* MT-32 (-M mt32): the closest MT-32 preset for each General MIDI program.
 * A FIRST TABLE from the two preset lists, to be set by ear (concept: ear
 * tests). tools/zmu_midi_check.py --mt32 reads it from here. */
static const UBYTE mt32_prog[128] =
{
	  0,   1,   2,   7,   3,   4,  16,  19,   /*   0 pianos, harpsichord, clavinet */
	 22, 101, 101,  97, 104, 103, 102, 105,   /*   8 celesta .. dulcimer */
	  8,   9,  10,  12,  14,  15,  87,  15,   /*  16 organs, accordion, harmonica */
	 59,  60,  61,  61,  61,  62,  62,  61,   /*  24 guitars */
	 64,  66,  67,  70,  68,  69,  28,  29,   /*  32 basses */
	 52,  53,  54,  56,  50,  51,  57, 112,   /*  40 strings, harp, timpani */
	 48,  49,  50,  50,  34,  34,  39, 122,   /*  48 ensembles, choir, voice, hit */
	 88,  90,  94,  89,  92,  95,  24,  25,   /*  56 brass */
	 78,  79,  80,  81,  84,  85,  86,  82,   /*  64 saxes, double reeds, clarinet */
	 74,  72,  76,  77, 110, 107, 108,  73,   /*  72 pipes */
	 47,  44,  42,  47,  62,  39,  44,  28,   /*  80 synth leads */
	 32,  33,  32,  34,  35,  38,  41,  43,   /*  88 synth pads */
	 41,  36,  35,  37,  40,  37,  43,  36,   /*  96 synth effects */
	 63,  60, 105, 105,  99,  84,  52,  84,   /* 104 ethnic */
	100, 115,  99, 120, 117, 113, 115, 119,   /* 112 percussive */
	 59, 111, 119, 124, 123, 116, 119, 114    /* 120 sound effects */
};

static double p_clock_us = 0.0;
static ULONG p_clock_last = 0;

/* microseconds since the process started (E-clock deltas; the sleeps are
 * short enough that the low longword never wraps between two calls) */
static double clock_us(void)
{
	struct EClockVal ev;
	ULONG f = ReadEClock(&ev);

	p_clock_us += (double)(ULONG)(ev.ev_lo - p_clock_last) * 1e6 / (double)f;
	p_clock_last = ev.ev_lo;
	return p_clock_us;
}

static void send(UBYTE status, UBYTE d1, UBYTE d2);
static int event_read(ULONG *pos, ULONG *wait, UBYTE *ev);

/* the output channels of a song: General MIDI as they are; MT-32: the
 * melodic channels in the order the song first uses them onto parts 2..9
 * (channels 1..8 counted from 0), the rhythm stays on 10 (9). Once per
 * song, so a jump keeps the same parts. */
static void channels_map(void)
{
	ULONG pos = 16, w;
	UBYTE ev[4];
	int ch, next = 1;

	for(ch = 0; ch < 16; ch++) p_out[ch] = (m_art == MIDI_MT32) ? -1 : (signed char)ch;

	if(m_art != MIDI_MT32) return;

	p_out[9] = 9;

	while(event_read(&pos, &w, ev) && ev[0] != 4)
	{
		if(ev[0] > 3) continue;

		ch = ev[1] & 15;

		if(ch != 9 && p_out[ch] < 0 && next <= 8)
			p_out[ch] = (signed char)next++;
	}
}

static void send(UBYTE status, UBYTE d1, UBYTE d2)
{
	PutMidi(p_link, CAMD_MSG(status, d1, d2));
	s_events++;
}

static void send_sysex(const UBYTE *bytes, int n)
{
	UBYTE buf[16];

	if(n > (int)sizeof(buf)) return;

	memcpy(buf, bytes, (size_t)n);
	PutSysEx(p_link, buf);
}

static UBYTE scaled(int v)
{
	return (UBYTE)((v * p_vol + 64) / 128);
}

static void notes_off(void)
{
	int ch, k;

	/* every note still sounding gets its own note off first (a synth that
	 * ignores CC123 is silent too), then All Notes Off on all 16 channels
	 * in one run */
	for(ch = 0; ch < 16; ch++)
	{
		for(k = 0; k < 128; k++)
			if(p_on[ch][k >> 3] & (1 << (k & 7)))
				send((UBYTE)(0x80 | ch), (UBYTE)k, 0);

		memset(p_on[ch], 0, sizeof(p_on[ch]));
	}

	for(ch = 0; ch < 16; ch++)
		send((UBYTE)(0xB0 | ch), 123, 0);      /* All Notes Off */
}

static void volumes_out(void)
{
	int oc, ch;

	/* all 16 output channels: the volume of the song's channel on it, 100
	 * where none is */
	for(oc = 0; oc < 16; oc++)
	{
		int v = 100;

		for(ch = 0; ch < 16; ch++)
			if(p_out[ch] == oc) { v = p_cc7[ch]; break; }

		send((UBYTE)(0xB0 | oc), 7, scaled(v));
	}
}

/* the event at *pos: its wait (steps) and its kind and data (ev[0..3]);
 * 0 at the end of the data or on a broken event */
static int event_read(ULONG *pos, ULONG *wait, UBYTE *ev)
{
	ULONG p = *pos;
	ULONG w;

	if(!p_song || p + 2 > p_len) return 0;

	w = p_song[p++];

	if(w == 255)
	{
		if(p + 2 > p_len) return 0;
		w = ((ULONG)p_song[p] << 8) | p_song[p + 1];
		p += 2;
	}

	if(p >= p_len) return 0;

	ev[0] = p_song[p++];

	switch(ev[0])
	{
	case 0:                 /* note on: channel, key, velocity */
		if(p + 3 > p_len) return 0;
		ev[1] = p_song[p]; ev[2] = p_song[p + 1]; ev[3] = p_song[p + 2];
		p += 3;
		break;
	case 1:                 /* note off: channel, key */
	case 2:                 /* program: channel, program */
	case 3:                 /* CC7: channel, value */
		if(p + 2 > p_len) return 0;
		ev[1] = p_song[p]; ev[2] = p_song[p + 1];
		p += 2;
		break;
	case 4:                 /* end */
	case 5:                 /* a wait only */
		break;
	default:
		return 0;
	}

	*pos = p;
	*wait = w;
	return 1;
}

/* the next event's step (p_next), without consuming it */
static void next_peek(void)
{
	ULONG pos = p_pos, w;
	UBYTE ev[4];

	p_have_next = event_read(&pos, &w, ev);
	if(p_have_next) p_next = p_step + w;
}

static UBYTE prog_out(int prog)
{
	return (UBYTE)(m_art == MIDI_MT32 ? mt32_prog[prog & 127] : (prog & 127));
}

static void event_do(const UBYTE *ev)
{
	const int ch = ev[1] & 15;
	const int oc = p_out[ch];
	const UBYTE key = (UBYTE)(ev[2] & 127);

	/* the state goes on even for a channel the MT-32 has no part for */
	if(ev[0] == 2) p_prog[ch] = ev[2] & 127;
	else if(ev[0] == 3) p_cc7[ch] = (UBYTE)(ev[2] & 127);

	if(oc < 0) { s_dropped++; return; }

	/* the MT-32's rhythm part knows keys 35..76 */
	if(m_art == MIDI_MT32 && oc == 9 && ev[0] <= 1 && (key < 35 || key > 76))
	{
		s_dropped++;
		return;
	}

	switch(ev[0])
	{
	case 0:
		if(ev[3])
		{
			send((UBYTE)(0x90 | oc), key, (UBYTE)(ev[3] & 127));
			p_on[oc][key >> 3] |= (UBYTE)(1 << (key & 7));
			s_notes++;
		}
		else
		{
			send((UBYTE)(0x80 | oc), key, 0);
			p_on[oc][key >> 3] &= (UBYTE)~(1 << (key & 7));
		}
		break;
	case 1:
		send((UBYTE)(0x80 | oc), key, 0);
		p_on[oc][key >> 3] &= (UBYTE)~(1 << (key & 7));
		break;
	case 2:
		send((UBYTE)(0xC0 | oc), prog_out(p_prog[ch]), 0);
		break;
	case 3:
		send((UBYTE)(0xB0 | oc), 7, scaled(p_cc7[ch]));
		break;
	default:
		break;
	}
}

static void song_reset_state(void)
{
	int ch;

	for(ch = 0; ch < 16; ch++)
	{
		p_cc7[ch] = 100;     /* the MIDI default until the song sets its own */
		p_prog[ch] = -1;
	}

	p_pos = 16;
	p_step = 0;
}

/* to step `target`: the programs and volumes as they stand there, no notes */
static void song_chase(ULONG target)
{
	ULONG pos, w;
	UBYTE ev[4];
	int ch;

	song_reset_state();

	for(;;)
	{
		pos = p_pos;
		if(!event_read(&pos, &w, ev)) break;
		/* the events AT the target play (its notes too): chase only those
		 * before it */
		if(p_step + w >= target || ev[0] == 4) break;

		p_step += w;
		p_pos = pos;

		if(ev[0] == 2) p_prog[ev[1] & 15] = ev[2] & 127;
		else if(ev[0] == 3) p_cc7[ev[1] & 15] = (UBYTE)(ev[2] & 127);
	}

	for(ch = 0; ch < 16; ch++)
		if(p_prog[ch] >= 0 && p_out[ch] >= 0)
			send((UBYTE)(0xC0 | p_out[ch]), prog_out(p_prog[ch]), 0);

	/* p_step stays the step of the last event read: the next one's wait
	 * counts from it; the caller sets the clock so that `target` is now */
	volumes_out();
}

static void song_jump(ULONG target)
{
	notes_off();
	song_chase(target);
	p_t0 = clock_us() - (double)target * STEP_US;
	s_jumps++;
}

/* the commands of the game, under the semaphore */
static void commands_take(void)
{
	UBYTE *song = 0;
	ULONG len = 0;
	int is_new, loop, pause, vol;
	LONG jump;

	ObtainSemaphore(&cmd_sem);
	is_new = cmd_new;
	if(is_new) { song = cmd_song; len = cmd_len; loop = cmd_loop; cmd_song = 0; cmd_new = 0; }
	else loop = 0;
	jump = cmd_jump; cmd_jump = -1;
	pause = cmd_pause; cmd_pause = -1;
	vol = cmd_vol; cmd_vol = -1;
	ReleaseSemaphore(&cmd_sem);

	if(vol >= 0)
	{
		p_vol = vol > 128 ? 128 : vol;
		volumes_out();
	}

	if(is_new)
	{
		notes_off();
		if(p_song) FreeVec(p_song);
		p_song = song;
		p_len = len;
		p_loop = loop;
		song_reset_state();
		channels_map();
		volumes_out();
		p_t0 = clock_us();
		p_song_start = p_t0;
		p_paused = 0;
		m_playing = p_song != 0;
		s_songs++;
	}

	if(jump >= 0 && p_song) song_jump((ULONG)jump);

	if(pause == 1 && !p_paused)
	{
		notes_off();
		p_paused = 1;
		p_paused_at = clock_us();
	}
	else if(pause == 0 && p_paused)
	{
		p_t0 += clock_us() - p_paused_at;
		p_paused = 0;
	}

	if(p_song) next_peek();
}

/* the events that are due; returns the clock time of the next one (or 0) */
static double events_due(void)
{
	int n = 0;

	while(p_song && !p_paused && m_playing && n < EVENTS_MAX)
	{
		ULONG pos = p_pos, w;
		UBYTE ev[4];
		double due, now;

		if(!event_read(&pos, &w, ev))
		{
			m_playing = 0;
			notes_off();
			break;
		}

		due = p_t0 + (double)(p_step + w) * STEP_US;
		now = clock_us();

		if(due > now) return due;

		if(now - due > 0.0)
		{
			s_late_sum += now - due;
			s_late_n++;
			if(now - due > s_late_max) s_late_max = now - due;
		}

		p_pos = pos;
		p_step += w;

		if(ev[0] == 4)
		{
			if(p_loop)
			{
				/* again from the start; the time goes on */
				p_t0 += (double)p_step * STEP_US;
				p_pos = 16;
				p_step = 0;
				s_loops++;
				continue;
			}

			m_playing = 0;
			notes_off();
			break;
		}

		event_do(ev);
		n++;
	}

	return 0.0;
}

static void midi_proc(void)
{
	struct MsgPort *tport = 0;
	struct timerequest *treq = 0;
	struct MidiNode *node = 0;
	struct TagItem tags[4];
	int topen = 0;
	int ch_i;
	ULONG tsig = 0;

	memset(p_on, 0, sizeof(p_on));
	p_vol = 128;
	song_reset_state();

	CamdBase = OpenLibrary((CONST_STRPTR)"camd.library", 37);
	if(!CamdBase) { m_fail = 1; goto done; }
	m_camd_ver = CamdBase->lib_Version;
	m_camd_rev = CamdBase->lib_Revision;

	tags[0].ti_Tag = MIDI_Name;
	tags[0].ti_Data = (ULONG)"Zod Engine";
	tags[1].ti_Tag = TAG_DONE;
	node = CreateMidiA(tags);
	if(!node) { m_fail = 2; goto done; }

	tags[0].ti_Tag = MLINK_Location;
	tags[0].ti_Data = (ULONG)m_cluster;
	tags[1].ti_Tag = MLINK_Name;
	tags[1].ti_Data = (ULONG)"Zod Engine music";
	tags[2].ti_Tag = MLINK_Comment;
	tags[2].ti_Data = (ULONG)"Zod Engine music";
	tags[3].ti_Tag = TAG_DONE;
	p_link = AddMidiLinkA(node, MLTYPE_Sender, tags);
	if(!p_link) { m_fail = 3; goto done; }
	m_heard = MidiLinkConnected(p_link) ? 1 : 0;

	tport = CreateMsgPort();
	if(tport) treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
	if(!treq || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)treq, 0) != 0)
	{
		m_fail = 4;
		goto done;
	}
	topen = 1;
	tsig = 1UL << tport->mp_SigBit;

	{
		struct EClockVal ev;

		ReadEClock(&ev);
		p_clock_last = ev.ev_lo;
	}

	/* General MIDI on, or the MT-32's "all parameters reset" (system area
	 * 7F 00 00, Roland checksum 01) and the time it takes */
	for(ch_i = 0; ch_i < 16; ch_i++) p_out[ch_i] = (signed char)ch_i;

	if(m_art == MIDI_MT32)
	{
		static const UBYTE mt_reset[] = { 0xF0, 0x41, 0x10, 0x16, 0x12, 0x7F, 0x00, 0x00, 0x00, 0x01, 0xF7 };

		send_sysex(mt_reset, sizeof(mt_reset));
		treq->tr_node.io_Command = TR_ADDREQUEST;
		treq->tr_time.tv_secs = 0;
		treq->tr_time.tv_micro = 300000;
		DoIO((struct IORequest *)treq);
	}
	else
	{
		static const UBYTE gm_on[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };

		send_sysex(gm_on, sizeof(gm_on));
	}

	m_state = 1;
	Signal(m_parent, 1UL << m_done_sig);

	for(;;)
	{
		double next, now, sleep_us;
		ULONG sigs;

		if(t_jump_step >= 0 && p_song && m_playing && !p_paused &&
		   clock_us() - p_song_start >= t_jump_after_us)
		{
			song_jump((ULONG)t_jump_step);
			t_jump_step = -1;          /* once */
			next_peek();
		}

		next = events_due();
		now = clock_us();

		sleep_us = MAX_SLEEP_US;
		if(next > 0.0)
		{
			sleep_us = next - now;
			if(sleep_us > MAX_SLEEP_US) sleep_us = MAX_SLEEP_US;
			if(sleep_us < 1.0) sleep_us = 1.0;
		}

		treq->tr_node.io_Command = TR_ADDREQUEST;
		treq->tr_time.tv_secs = 0;
		treq->tr_time.tv_micro = (ULONG)sleep_us;
		SendIO((struct IORequest *)treq);

		sigs = Wait(tsig | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F);

		if(!CheckIO((struct IORequest *)treq)) AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
		SetSignal(0, tsig);

		if(sigs & SIGBREAKF_CTRL_C) break;
		if(sigs & SIGBREAKF_CTRL_F) commands_take();
	}

	notes_off();

	if(m_art == MIDI_MT32)
	{
		static const UBYTE mt_reset[] = { 0xF0, 0x41, 0x10, 0x16, 0x12, 0x7F, 0x00, 0x00, 0x00, 0x01, 0xF7 };

		send_sysex(mt_reset, sizeof(mt_reset));
	}

done:
	if(topen) CloseDevice((struct IORequest *)treq);
	if(treq) DeleteIORequest((struct IORequest *)treq);
	if(tport) DeleteMsgPort(tport);
	if(p_link) { RemoveMidiLink(p_link); p_link = 0; }
	if(node) DeleteMidi(node);
	if(CamdBase) { CloseLibrary(CamdBase); CamdBase = 0; }
	if(p_song) { FreeVec(p_song); p_song = 0; }
	m_playing = 0;

	if(m_state == 0) m_state = -1;

	/* the parent may unload the code right after this signal: the rest of
	 * the exit runs under Forbid (RemTask ends it) */
	Forbid();
	Signal(m_parent, 1UL << m_done_sig);
}

/* ---- the game's side --------------------------------------------------------- */

static void poke(void)
{
	if(m_proc) Signal((struct Task *)m_proc, SIGBREAKF_CTRL_F);
}

int midi_start(int art, const char *cluster)
{
	if(m_proc) return 1;

	m_art = art;
	strncpy(m_cluster, (cluster && *cluster) ? cluster : "out.0", sizeof(m_cluster) - 1);
	m_cluster[sizeof(m_cluster) - 1] = 0;

	{
		char buf[32];
		LONG st, sec;

		if(GetVar((STRPTR)"ZOD_MIDI_TESTJUMP", (STRPTR)buf, sizeof(buf), 0) > 0 &&
		   StrToLong((STRPTR)buf, &st) > 0 && strchr(buf, '@') &&
		   StrToLong((STRPTR)strchr(buf, '@') + 1, &sec) > 0)
		{
			t_jump_step = st;
			t_jump_after_us = (double)sec * 1e6;
			ZLOG("MIDI: test jump to step %ld after %ld s of a song (ZOD_MIDI_TESTJUMP)\n",
			     (long)st, (long)sec);
		}
	}

	InitSemaphore(&cmd_sem);
	m_parent = FindTask(0);
	m_done_sig = AllocSignal(-1);

	if(m_done_sig < 0)
	{
		ZLOG("MIDI: no signal free -- music off\n");
		return 0;
	}

	m_state = 0;
	m_fail = 0;
	SetSignal(0, 1UL << m_done_sig);

	m_proc = CreateNewProcTags(NP_Entry,     (ULONG)midi_proc,
	                           NP_Name,      (ULONG)"ZodMIDI",
	                           NP_Priority,  5,
	                           NP_StackSize, 8192,
	                           TAG_DONE);

	if(!m_proc)
	{
		ZLOG("MIDI: the player process cannot be made -- music off\n");
		FreeSignal(m_done_sig);
		m_done_sig = -1;
		return 0;
	}

	Wait(1UL << m_done_sig);

	if(m_state != 1)
	{
		static const char *const why[] = { "", "no camd.library (V37+)", "CreateMidi failed",
		                                   "no link to the cluster", "no timer.device" };

		ZLOG("MIDI: %s (cluster %s) -- music off\n",
		     why[m_fail >= 1 && m_fail <= 4 ? m_fail : 0], m_cluster);
		m_proc = 0;                       /* it has ended */
		FreeSignal(m_done_sig);
		m_done_sig = -1;
		return 0;
	}

	ZLOG("MIDI: camd.library %u.%u, cluster %s%s, %s\n", m_camd_ver, m_camd_rev, m_cluster,
	     m_heard ? "" : " (no receiver linked yet)",
	     m_art == MIDI_MT32 ? "MT-32 (programs mapped, parts 2..9, rhythm on 10)" : "General MIDI");

	return 1;
}

int midi_spiele(const UBYTE *zmu, ULONG len, int schleife)
{
	UBYTE *copy;

	if(!m_proc || !zmu || len < 16 || memcmp(zmu, "ZMU1", 4) != 0) return 0;

	copy = (UBYTE *)AllocVec(len, MEMF_ANY);
	if(!copy) return 0;

	CopyMem((APTR)zmu, copy, len);

	ObtainSemaphore(&cmd_sem);
	if(cmd_song) FreeVec(cmd_song);     /* a song not yet taken */
	cmd_song = copy;
	cmd_len = len;
	cmd_loop = schleife;
	cmd_new = 1;
	cmd_jump = -1;
	cmd_pause = 0;
	ReleaseSemaphore(&cmd_sem);

	m_playing = 1;
	poke();
	return 1;
}

void midi_springen(ULONG schritte)
{
	if(!m_proc) return;

	ObtainSemaphore(&cmd_sem);
	cmd_jump = (LONG)schritte;
	ReleaseSemaphore(&cmd_sem);
	poke();
}

void midi_pause(int an)
{
	if(!m_proc) return;

	ObtainSemaphore(&cmd_sem);
	cmd_pause = an ? 1 : 0;
	ReleaseSemaphore(&cmd_sem);
	poke();
}

void midi_lautstaerke(int v)
{
	if(!m_proc) return;

	ObtainSemaphore(&cmd_sem);
	cmd_vol = v < 0 ? 0 : (v > 128 ? 128 : v);
	ReleaseSemaphore(&cmd_sem);
	poke();
}

int midi_laeuft(void)
{
	return m_proc && m_playing && !p_paused;
}

void midi_stop(void)
{
	if(!m_proc) return;

	SetSignal(0, 1UL << m_done_sig);
	Signal((struct Task *)m_proc, SIGBREAKF_CTRL_C);
	Wait(1UL << m_done_sig);
	m_proc = 0;

	ObtainSemaphore(&cmd_sem);
	if(cmd_song) { FreeVec(cmd_song); cmd_song = 0; }
	ReleaseSemaphore(&cmd_sem);

	FreeSignal(m_done_sig);
	m_done_sig = -1;
}

void midi_bericht(void)
{
	if(!s_songs && !s_events) return;

	ZLOG("MIDI: %lu songs, %lu events sent, %lu notes, %lu jumps, %lu loops, %lu dropped; "
	     "events late on average %lu us, at most %lu us\n",
	     (unsigned long)s_songs, (unsigned long)s_events, (unsigned long)s_notes,
	     (unsigned long)s_jumps, (unsigned long)s_loops, (unsigned long)s_dropped,
	     (unsigned long)(s_late_n ? s_late_sum / s_late_n : 0.0), (unsigned long)s_late_max);
}
