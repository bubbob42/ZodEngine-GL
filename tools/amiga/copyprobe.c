/*
 * copyprobe.c -- how fast the 040/060 copies the map's background into the
 * picture, four ways (docs/concept-asm-speed.md).
 *
 * The question: the tour's frame spends ~13 ms (AGA) and more (RTG) in
 * copy_lang, the longword copy of the map's background into the picture
 * (fast RAM to fast RAM, the source at any byte offset, the destination row
 * aligned). If the CPU's cache reads every destination line before the
 * copy overwrites it (write-allocate), a copy that writes whole lines with
 * move16 from a small aligned buffer moves a third less data. So the same
 * copy is timed:
 *   A  the C loop of fastcopy.cpp (copy_lang's longword part)
 *   B  four move.l a step in assembler
 *   C  four move.l into an aligned 16-byte line, then move16 to the row
 *   D  move16 straight from the source (16-byte aligned source only: the
 *      best case, for reference)
 * on 476 rows of 512 bytes (the AGA tour's map view), the source rows 1408
 * bytes apart (a map surface), at each source offset 0..15. Every variant's
 * result is checked against A's. Fast RAM only, nothing on the screen.
 *
 * Usage: copyprobe [PASSES=<n>]   (n frames a measurement, default 20)
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <string.h>

struct Device *TimerBase = 0;

void cp_longs(UBYTE *dst, const UBYTE *src, LONG n);
void cp_m16tmp(UBYTE *dst, const UBYTE *src, LONG n, UBYTE *tmp16);
void cp_m16(UBYTE *dst, const UBYTE *src, LONG n);

enum { ROWS = 476, ROWW = 512, DPITCH = 640, SPITCH = 1408 };

static ULONG eclock_rate = 0;

static ULONG ticks_now(void)
{
	struct EClockVal ev;

	eclock_rate = ReadEClock(&ev);
	return ev.ev_lo;
}

/* fastcopy.cpp copy_lang, its longword part (n a multiple of 16) */
static void cp_c(UBYTE *d, const UBYTE *s, int n)
{
	unsigned long *dl = (unsigned long *)d;
	const unsigned long *sl = (const unsigned long *)s;
	int vier = n >> 4;

	while(vier--)
	{
		dl[0] = sl[0]; dl[1] = sl[1]; dl[2] = sl[2]; dl[3] = sl[3];
		dl += 4;
		sl += 4;
	}
}

static ULONG run(int variant, UBYTE *dst, const UBYTE *src, UBYTE *tmp, int passes)
{
	ULONG t0, t1;
	int k, r;

	t0 = ticks_now();

	for(k = 0; k < passes; k++)
		for(r = 0; r < ROWS; r++)
		{
			UBYTE *const d = dst + (long)r * DPITCH;
			const UBYTE *const s = src + (long)r * SPITCH;

			switch(variant)
			{
			case 0: cp_c(d, s, ROWW); break;
			case 1: cp_longs(d, s, ROWW); break;
			case 2: cp_m16tmp(d, s, ROWW, tmp); break;
			default: cp_m16(d, s, ROWW); break;
			}
		}

	t1 = ticks_now();
	return t1 - t0;
}

int main(void)
{
	static const char *const names[4] = {
		"A  C loop (copy_lang)", "B  move.l x4 (asm)", "C  move.l to a line, move16 out",
		"D  move16 straight (aligned source)" };
	struct MsgPort *port = 0;
	struct timerequest *tr = 0;
	UBYTE *srcmem = 0, *dstmem = 0, *refmem = 0, *tmpmem = 0;
	UBYTE *src, *dst, *ref, *tmp;
	LONG args[1] = { 0 };
	struct RDArgs *rda;
	int passes = 20, v, off, rc = 20;
	ULONG t[4] = { 0, 0, 0, 0 };
	unsigned long bad = 0;
	long i;

	rda = ReadArgs((CONST_STRPTR)"PASSES/N", args, 0);
	if(rda && args[0]) passes = (int)*(LONG *)args[0];
	if(rda) FreeArgs(rda);
	if(passes < 1) passes = 1;

	port = CreateMsgPort();
	tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : 0;
	if(!tr || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)tr, 0))
	{
		Printf("copyprobe: no timer.device\n");
		if(tr) DeleteIORequest((struct IORequest *)tr);
		if(port) DeleteMsgPort(port);
		return 20;
	}
	TimerBase = tr->tr_node.io_Device;

	srcmem = (UBYTE *)AllocVec((ULONG)SPITCH * ROWS + 64, MEMF_FAST);
	dstmem = (UBYTE *)AllocVec((ULONG)DPITCH * ROWS + 64, MEMF_FAST);
	refmem = (UBYTE *)AllocVec((ULONG)DPITCH * ROWS + 64, MEMF_FAST);
	tmpmem = (UBYTE *)AllocVec(64, MEMF_FAST);
	if(!srcmem || !dstmem || !refmem || !tmpmem)
	{
		Printf("copyprobe: no fast RAM\n");
		goto done;
	}

	src = (UBYTE *)(((ULONG)srcmem + 15) & ~15UL);
	dst = (UBYTE *)(((ULONG)dstmem + 15) & ~15UL);
	ref = (UBYTE *)(((ULONG)refmem + 15) & ~15UL);
	tmp = (UBYTE *)(((ULONG)tmpmem + 15) & ~15UL);

	for(i = 0; i < (long)SPITCH * ROWS; i++) src[i] = (UBYTE)(i * 7 + (i >> 9));

	Printf("copyprobe: %ld rows of %ld bytes, %ld frames a measurement, source offsets 0..15\n",
	       (long)ROWS, (long)ROWW, (long)passes);

	for(off = 0; off < 16; off++)
	{
		/* the reference at this offset, then each variant checked against it */
		run(0, ref, src + off, tmp, 1);

		for(v = 0; v < 4; v++)
		{
			int r;

			if(v == 3 && off != 0) continue;   /* move16 needs an aligned source */

			memset(dst, 0, (size_t)DPITCH * ROWS);
			t[v] += run(v, dst, src + off, tmp, passes);

			for(r = 0; r < ROWS; r++)
				if(memcmp(dst + (long)r * DPITCH, ref + (long)r * DPITCH, ROWW)) bad++;
		}
	}

	for(v = 0; v < 4; v++)
	{
		const ULONG frames = (ULONG)passes * (v == 3 ? 1UL : 16UL);
		const ULONG us = eclock_rate ? (ULONG)(((t[v] / frames) * 1000UL) / (eclock_rate / 1000UL)) : 0;

		Printf("  %-38s %6ld us a frame (%ld KB)\n", names[v], (long)us, (long)(ROWS * ROWW / 1024));
	}
	Printf("  rows that differ from the C loop's: %ld\n", (long)bad);
	rc = bad ? 10 : 0;

done:
	if(tmpmem) FreeVec(tmpmem);
	if(refmem) FreeVec(refmem);
	if(dstmem) FreeVec(dstmem);
	if(srcmem) FreeVec(srcmem);
	CloseDevice((struct IORequest *)tr);
	DeleteIORequest((struct IORequest *)tr);
	DeleteMsgPort(port);
	return rc;
}
