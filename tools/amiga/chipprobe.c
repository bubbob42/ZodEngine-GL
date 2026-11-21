/*
 * chipprobe.c -- how fast the CPU writes AGA chip RAM under different
 * displays (docs/research-aga-speed.md).
 *
 * The question behind it: on the A4000D and the PiStorm's A1200 Zod's AGA
 * screen gets only ~2 MB/s of chip RAM writes. Is that the display's DMA
 * taking the bus (then fewer planes, a smaller window or fewer sprites
 * would help), or the CPU's own way to chip RAM (then only fewer or wider
 * writes help)? So the same writes are timed with no AGA display, with
 * Zod's screen (PAL HiRes Laced 640x512, 8 planes) and with lighter ones,
 * as 16-bit and as 32-bit writes.
 *
 * Each configuration: a screen opened in front (SA_Quiet, colour 0), two
 * frames to settle, then 1 MB written into a 64 KB chip buffer of its own
 * (not the shown bitmap -- the bus is the same), timed with the E-clock.
 * Reads are timed too (c2p does not read chip RAM; for completeness).
 *
 * Usage: chipprobe [PASSES=<n>]   (n x 64 KB a measurement, default 16)
 * Writes one table to the console. Every screen is closed again.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>

struct GfxBase *GfxBase = 0;
struct IntuitionBase *IntuitionBase = 0;
struct Device *TimerBase = 0;

enum { BUFSIZE = 64 * 1024 };

struct config
{
	const char *name;
	ULONG id;              /* INVALID_ID: no AGA screen at all */
	int depth, w, h;
};

static const struct config configs[] =
{
	{ "no AGA screen (the Workbench in front)", INVALID_ID, 0, 0, 0 },
	{ "PAL HiRes Laced 640x512, 8 planes (Zod)", PAL_MONITOR_ID | HIRESLACE_KEY, 8, 640, 512 },
	{ "PAL HiRes Laced 640x512, 6 planes", PAL_MONITOR_ID | HIRESLACE_KEY, 6, 640, 512 },
	{ "PAL HiRes Laced 640x512, 4 planes", PAL_MONITOR_ID | HIRESLACE_KEY, 4, 640, 512 },
	{ "PAL HiRes Laced 640x512, 2 planes", PAL_MONITOR_ID | HIRESLACE_KEY, 2, 640, 512 },
	{ "PAL HiRes Laced 640x400, 8 planes", PAL_MONITOR_ID | HIRESLACE_KEY, 8, 640, 400 },
	{ "PAL HiRes Laced 640x256 window, 8 planes", PAL_MONITOR_ID | HIRESLACE_KEY, 8, 640, 256 },
	{ "no AGA screen again", INVALID_ID, 0, 0, 0 }
};

static ULONG eclock_rate = 0;

static ULONG ticks_now(void)
{
	struct EClockVal ev;

	eclock_rate = ReadEClock(&ev);
	return ev.ev_lo;
}

/* KB per second for n bytes in t E-clock ticks */
static ULONG kbs(ULONG bytes, ULONG t)
{
	if(!t) return 0;

	/* bytes / (t / rate) / 1024, without overflow for ~1 MB and ~0.5 M ticks */
	return (ULONG)(((bytes / 1024UL) * (eclock_rate / 16UL)) / (t / 16UL + 1UL));
}

static ULONG time_words(volatile UWORD *p, int passes)
{
	ULONG t0, t1;
	int k;
	long i;

	t0 = ticks_now();

	for(k = 0; k < passes; k++)
		for(i = 0; i < BUFSIZE / 2; i += 8)
		{
			p[i] = 0x1234; p[i + 1] = 0x1234; p[i + 2] = 0x1234; p[i + 3] = 0x1234;
			p[i + 4] = 0x1234; p[i + 5] = 0x1234; p[i + 6] = 0x1234; p[i + 7] = 0x1234;
		}

	t1 = ticks_now();
	return t1 - t0;
}

static ULONG time_longs(volatile ULONG *p, int passes)
{
	ULONG t0, t1;
	int k;
	long i;

	t0 = ticks_now();

	for(k = 0; k < passes; k++)
		for(i = 0; i < BUFSIZE / 4; i += 8)
		{
			p[i] = 0x12345678UL; p[i + 1] = 0x12345678UL; p[i + 2] = 0x12345678UL;
			p[i + 3] = 0x12345678UL; p[i + 4] = 0x12345678UL; p[i + 5] = 0x12345678UL;
			p[i + 6] = 0x12345678UL; p[i + 7] = 0x12345678UL;
		}

	t1 = ticks_now();
	return t1 - t0;
}

static ULONG time_reads(volatile ULONG *p, int passes, ULONG *sink)
{
	ULONG t0, t1, s = 0;
	int k;
	long i;

	t0 = ticks_now();

	for(k = 0; k < passes; k++)
		for(i = 0; i < BUFSIZE / 4; i += 4)
			s += p[i] + p[i + 1] + p[i + 2] + p[i + 3];

	t1 = ticks_now();
	*sink += s;
	return t1 - t0;
}

int main(void)
{
	static const char template[] = "PASSES/N";
	LONG args[1] = { 0 };
	struct RDArgs *rd = 0;
	struct MsgPort *port = 0;
	struct timerequest *tr = 0;
	UBYTE *buf = 0;
	int passes = 16;
	int rc = RETURN_FAIL;
	ULONG sink = 0;
	unsigned c;

	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39L);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39L);

	if(!GfxBase || !IntuitionBase)
	{
		Printf("chipprobe: needs graphics and intuition V39\n");
		goto out;
	}

	rd = ReadArgs((CONST_STRPTR)template, args, 0);
	if(!rd)
	{
		PrintFault(IoErr(), (CONST_STRPTR)"chipprobe");
		goto out;
	}
	if(args[0]) passes = (int)*(LONG *)args[0];
	if(passes < 1) passes = 1;
	if(passes > 256) passes = 256;

	port = CreateMsgPort();
	tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : 0;

	if(!tr || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)tr, 0))
	{
		Printf("chipprobe: no timer.device\n");
		if(tr) { DeleteIORequest((struct IORequest *)tr); tr = 0; }
		goto out;
	}
	TimerBase = tr->tr_node.io_Device;

	buf = (UBYTE *)AllocMem(BUFSIZE, MEMF_CHIP | MEMF_CLEAR);
	if(!buf)
	{
		Printf("chipprobe: no %ld bytes of chip RAM\n", (long)BUFSIZE);
		goto out;
	}

	Printf("chipprobe: %ld KB a measurement, chipset %s, CPU writes into a 64 KB chip buffer\n",
	       (long)(passes * BUFSIZE / 1024),
	       (GfxBase->ChipRevBits0 & GFXF_AA_ALICE) ? "AGA" : "not AGA");
	Printf("%-46s %10s %10s %10s\n", "display", "word KB/s", "long KB/s", "read KB/s");

	for(c = 0; c < sizeof(configs) / sizeof(configs[0]); c++)
	{
		const struct config *cf = &configs[c];
		struct Screen *scr = 0;
		ULONG tw, tl, trd;

		if(cf->id != INVALID_ID)
		{
			struct Rectangle dclip;
			ULONG err = 0;
			struct TagItem sa[] = {
				{ SA_DisplayID, 0 },
				{ SA_Width,     0 },
				{ SA_Height,    0 },
				{ SA_DClip,     0 },
				{ SA_Depth,     0 },
				{ SA_Type,      CUSTOMSCREEN },
				{ SA_Quiet,     TRUE },
				{ SA_ShowTitle, FALSE },
				{ SA_Draggable, FALSE },
				{ SA_ErrorCode, 0 },
				{ TAG_DONE,     0 }
			};

			if(ModeNotAvailable(cf->id))
			{
				Printf("%-46s not available\n", cf->name);
				continue;
			}

			dclip.MinX = 0;
			dclip.MinY = 0;
			dclip.MaxX = (WORD)(cf->w - 1);
			dclip.MaxY = (WORD)(cf->h - 1);
			sa[0].ti_Data = cf->id;
			sa[1].ti_Data = (ULONG)cf->w;
			sa[2].ti_Data = (ULONG)cf->h;
			sa[3].ti_Data = (ULONG)&dclip;
			sa[4].ti_Data = (ULONG)cf->depth;
			sa[9].ti_Data = (ULONG)&err;

			scr = OpenScreenTagList(0, sa);
			if(!scr)
			{
				Printf("%-46s OpenScreen failed (%ld)\n", cf->name, (long)err);
				continue;
			}

			ScreenToFront(scr);
		}

		WaitTOF();
		WaitTOF();

		tw = time_words((volatile UWORD *)buf, passes);
		tl = time_longs((volatile ULONG *)buf, passes);
		trd = time_reads((volatile ULONG *)buf, passes, &sink);

		if(scr) CloseScreen(scr);

		Printf("%-46s %10lu %10lu %10lu\n", cf->name,
		       kbs((ULONG)passes * BUFSIZE, tw), kbs((ULONG)passes * BUFSIZE, tl),
		       kbs((ULONG)passes * BUFSIZE, trd));

		if(CheckSignal(SIGBREAKF_CTRL_C)) { Printf("***Break\n"); break; }
	}

	Printf("(E-clock %lu Hz; read sum %lu)\n", eclock_rate, sink & 1);
	rc = RETURN_OK;

out:
	if(buf) FreeMem(buf, BUFSIZE);
	if(TimerBase) { CloseDevice((struct IORequest *)tr); TimerBase = 0; }
	if(tr) DeleteIORequest((struct IORequest *)tr);
	if(port) DeleteMsgPort(port);
	if(rd) FreeArgs(rd);
	if(IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
	if(GfxBase) CloseLibrary((struct Library *)GfxBase);
	return rc;
}
