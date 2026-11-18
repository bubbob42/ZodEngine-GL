/*
 * zod_present.cpp -- frames without tearing on PiStorm3D.
 *
 * PiStorm3D 30 flips buffers in sync only on VideoCore.card 2.0 or newer (a
 * card with a vertical blank interrupt); the PiStorm runs Emu68-VC4.card 1.0
 * (and 1.5 is the newest there is, 2026-11). Below 2.0 it keeps ONE buffer:
 * the GPU draws into the screen on display and the picture tears -- all the
 * time at Zod's 177-234 fps there. What the PiStorm offers instead, MEASURED
 * by Doom 64 on the same machine (2026-11-13, its amiga/panflip_amiga.c,
 * which this module follows):
 *
 *   WaitTOF            the chipset's 50 Hz, not the HDMI output: no use
 *   ChangeScreenBuffer 34 ms a switch (two refreshes), spinning the CPU
 *   ScrollVPort        one refresh (17.9 ms mean at 60 Hz), spinning the CPU
 *   BltBitMap 640x480  1.0 ms
 *
 * So Zod presents by itself here: a screen of its own, twice as tall as the
 * display (BGRA32, the format V3D writes), a bitmap of its own that the GL
 * context draws into (mglCreateContextFromBitMap), and per frame: the
 * finished frame copied into the half not on display, then the display
 * panned onto it (RasInfo RyOffset + ScrollVPort, which waits for the
 * blank). The rate follows the refresh (60 fps on a 60 Hz output).
 *
 * The screen has no mouse sprite (P96SA_NoSprite): Zod draws its pointer
 * into the frame itself (gl_bild_zeigen), so the pointer moves with the
 * picture between the halves. Intuition is not told about the pan, so the
 * mouse stays in the top half's coordinates; the range seen is logged.
 *
 * Doom 64's measurements on the waiting: sleeping on timer.device before the
 * pan did not help (a task woken at priority 0 can wait a whole quantum),
 * and a raised priority is forbidden -- the pan spins at the game's own
 * priority.
 *
 * Costs: PiStorm3D's bitmap contexts leak ~16 KB and slow down after ~5
 * context rounds (its defect 3); Zod makes its context once per start and per
 * screen size change.
 */

#ifndef DISABLE_OPENGL

#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <libraries/Picasso96.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/Picasso96.h>

#include <SDL/SDL_opengl.h>

#include "zod_present.h"
#include "zod_log.h"
#include "amiga_startup.h"
#include "fineclock.h"

extern struct ExecBase *SysBase;
extern struct Library *CyberGfxBase;   /* sdl_screen.cpp, open while a screen is */

struct Library *P96Base = 0;           /* Picasso96API: a screen without a sprite */

static struct Screen *pr_scr = 0;
static struct Window *pr_win = 0;
static struct BitMap *pr_off = 0;      /* the context draws here */
static UWORD *pr_blank = 0;            /* an empty sprite in chip RAM */
static int pr_p96screen = 0;           /* pr_scr from p96OpenScreenTags */
static int pr_w = 0, pr_h = 0;
static int pr_shown = 0;               /* the half on display: 0 top, 1 bottom */

/* statistics, E-Clock ticks */
static unsigned long pr_frames, pr_tear_frames, pr_missed, pr_late;
static unsigned long pr_last;
static int pr_have_last;
static double pr_work_sum, pr_blit_sum, pr_wait_sum;
static unsigned long pr_wait_max;
static unsigned long pr_period;        /* the refresh, learnt (ticks) */
static int pr_mouse_min = 0x7fff, pr_mouse_max = -1;

int zod_present_wanted(void)
{
	struct Library *card;
	LONG ver = -1;
	const char *id;
	char buf[16];
	const char *e;

	if(!MiniGLBase) return 0;

	id = (const char *)MiniGLBase->lib_IdString;
	if(!id || !strstr(id, "PiStorm3D")) return 0;

	/* ZOD_GL_PRESENT=aus: the library's own screen (one buffer, tearing) */
	e = zod_env("ZOD_GL_PRESENT", buf, sizeof(buf));
	if(e && (e[0] == 'a' || e[0] == 'A' || e[0] == '0' || (e[0] == 'o' && e[1] == 'f')))
		return 0;

	Forbid();
	card = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)"VideoCore.card");
	if(card) ver = card->lib_Version;
	Permit();

	return ver < 2;
}

/* the display onto half i */
static void pan(int i)
{
	struct ViewPort *vp = &pr_scr->ViewPort;

	vp->RasInfo->RxOffset = 0;
	vp->RasInfo->RyOffset = (WORD)(i * pr_h);
	ScrollVPort(vp);
	pr_shown = i;
}

static unsigned long ticks_us(double ticks)
{
	const unsigned long f = zod_fineclock_freq() ? zod_fineclock_freq() : 709379UL;

	return (unsigned long)(ticks * 1e6 / f);
}

void zod_present_close(void)
{
	if(pr_frames > 0)
	{
		const unsigned long n = pr_frames - pr_tear_frames;

		ZLOG("Present: %lu frames, %lu of them without sync\n", pr_frames, pr_tear_frames);

		if(n > 1)
			ZLOG("Present: refreshes missed %lu (late frames %lu, missed blanks %lu); work mean %lu us,"
			     " copy mean %lu us, pan wait mean %lu us max %lu; refresh %lu us\n",
			     pr_late + pr_missed, pr_late, pr_missed,
			     ticks_us(pr_work_sum / (n - 1)), ticks_us(pr_blit_sum / pr_frames),
			     ticks_us(pr_wait_sum / n), ticks_us((double)pr_wait_max), ticks_us((double)pr_period));

		if(pr_mouse_max >= 0)
			ZLOG("Present: mouse y seen %d..%d (the display is %d high)\n",
			     pr_mouse_min, pr_mouse_max, pr_h);
	}

	pr_frames = pr_tear_frames = pr_missed = pr_late = 0;
	pr_work_sum = pr_blit_sum = pr_wait_sum = 0.0;
	pr_wait_max = 0;
	pr_have_last = 0;
	pr_mouse_min = 0x7fff;
	pr_mouse_max = -1;

	if(pr_win)
	{
		ClearPointer(pr_win);
		CloseWindow(pr_win);
		pr_win = 0;
	}

	if(pr_off)
	{
		FreeBitMap(pr_off);
		pr_off = 0;
	}

	if(pr_scr)
	{
		if(pr_p96screen) p96CloseScreen(pr_scr);
		else CloseScreen(pr_scr);
		pr_scr = 0;
	}

	pr_p96screen = 0;

	if(P96Base)
	{
		CloseLibrary(P96Base);
		P96Base = 0;
	}

	if(pr_blank)
	{
		FreeVec(pr_blank);
		pr_blank = 0;
	}
}

/* the display database's BGRA32 mode of exactly w x h; INVALID_ID none
 * (BestModeID gave the same id for different sizes on VideoCore.card 1.5) */
static ULONG find_mode(int w, int h)
{
	struct DimensionInfo dims;
	ULONG id = INVALID_ID;

	while((id = NextDisplayInfo(id)) != INVALID_ID)
	{
		if(!IsCyberModeID(id) || GetCyberIDAttr(CYBRIDATTR_PIXFMT, id) != PIXFMT_BGRA32)
			continue;

		if(GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, id) == 0)
			continue;

		if(dims.Nominal.MaxX - dims.Nominal.MinX + 1 == w &&
		   dims.Nominal.MaxY - dims.Nominal.MinY + 1 == h)
			return id;
	}

	return INVALID_ID;
}

int zod_present_open(int w, int h)
{
	struct Rectangle clip;
	const ULONG tall = (ULONG)(2 * h);   /* both halves */
	ULONG id;
	void *ctx;

	if(!CyberGfxBase)
	{
		ZLOG("Present: no cybergraphics.library\n");
		return 0;
	}

	id = find_mode(w, h);

	if(id == INVALID_ID)
		id = BestModeID(BIDTAG_NominalWidth, w, BIDTAG_NominalHeight, h, BIDTAG_Depth, 24, TAG_DONE);

	if(id == INVALID_ID || !IsCyberModeID(id) ||
	   GetCyberIDAttr(CYBRIDATTR_PIXFMT, id) != PIXFMT_BGRA32 ||
	   GetCyberIDAttr(CYBRIDATTR_WIDTH, id) != (ULONG)w ||
	   GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) != (ULONG)h)
	{
		ZLOG("Present: no %dx%d BGRA32 mode ($%08lx)\n", w, h, (unsigned long)id);
		zod_present_close();
		return 0;
	}

	clip.MinX = 0;
	clip.MinY = 0;
	clip.MaxX = (WORD)(w - 1);
	clip.MaxY = (WORD)(h - 1);

	/* without the mouse sprite where Picasso96API is there: the hardware
	 * pointer would stay put while the view jumps between the halves */
	P96Base = OpenLibrary((CONST_STRPTR)P96NAME, 2);

	if(P96Base)
	{
		pr_scr = p96OpenScreenTags(P96SA_DisplayID, id, P96SA_Width, w, P96SA_Height, tall,
		                           P96SA_Depth, p96GetModeIDAttr(id, P96IDA_DEPTH),
		                           P96SA_RGBFormat, p96GetModeIDAttr(id, P96IDA_RGBFORMAT),
		                           P96SA_DClip, (ULONG)&clip, P96SA_AutoScroll, FALSE,
		                           P96SA_Quiet, TRUE, P96SA_ShowTitle, FALSE,
		                           P96SA_Type, CUSTOMSCREEN, P96SA_Title, (ULONG)"Zod Engine",
		                           P96SA_NoSprite, TRUE, TAG_DONE);

		if(pr_scr) pr_p96screen = 1;
		else ZLOG("Present: Picasso96's screen did not open, intuition's instead\n");
	}

	if(!pr_scr)
		pr_scr = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, w, SA_Height, tall,
		                        SA_DClip, (ULONG)&clip, SA_AutoScroll, FALSE, SA_Depth, 24,
		                        SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN,
		                        SA_Title, (ULONG)"Zod Engine", TAG_DONE);

	if(!pr_scr || pr_scr->RastPort.BitMap->Rows < (UWORD)tall ||
	   GetCyberMapAttr(pr_scr->RastPort.BitMap, CYBRMATTR_BPPIX) != 4)
	{
		ZLOG("Present: the %dx%d screen did not open as it should\n", w, 2 * h);
		zod_present_close();
		return 0;
	}

	/* what the library allocates for itself: 32 bits, displayable, the
	 * screen's friend */
	pr_off = AllocBitMap(w, h, 32, BMF_MINPLANES | BMF_DISPLAYABLE, pr_scr->RastPort.BitMap);
	pr_blank = (UWORD *)AllocVec(16, MEMF_CHIP | MEMF_CLEAR);

	/* the input window covers both halves: a click never lands outside it */
	pr_win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)pr_scr, WA_Left, 0, WA_Top, 0,
	                        WA_Width, w, WA_Height, tall, WA_Backdrop, TRUE, WA_Borderless, TRUE,
	                        WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_SimpleRefresh, TRUE,
	                        WA_NoCareRefresh, TRUE, WA_BackFill, (ULONG)LAYERS_NOBACKFILL,
	                        WA_IDCMP, 0, TAG_DONE);

	if(!pr_off || !pr_blank || !pr_win)
	{
		ZLOG("Present: bitmap, pointer or window missing\n");
		zod_present_close();
		return 0;
	}

	/* the engine draws the pointer itself */
	SetPointer(pr_win, pr_blank, 1, 16, 0, 0);

	ctx = mglCreateContextFromBitMap(pr_off);

	if(!ctx)
	{
		ZLOG("Present: the library gave no context for the bitmap\n");
		zod_present_close();
		return 0;
	}

	pr_w = w;
	pr_h = h;
	pr_period = zod_fineclock_freq() / 60;   /* learnt from the frames */
	pan(0);

	ZLOG("Present: %dx%d screen of its own (%s), the display panned between its halves\n",
	     w, 2 * h, pr_p96screen ? "Picasso96, no sprite" : "intuition");

	return 1;
}

int zod_present_active(void)
{
	return pr_scr != 0;
}

struct Window *zod_present_window(void)
{
	return pr_win;
}

void zod_present_show(int sync)
{
	const int target = sync ? !pr_shown : pr_shown;
	unsigned long t0, t1, t2, work = 0, blit, wait;

	if(!pr_scr) return;

	/* the pointer's coordinates while the view is panned (a check that
	 * Intuition keeps them in the top half) */
	if(pr_scr->MouseY < pr_mouse_min) pr_mouse_min = pr_scr->MouseY;
	if(pr_scr->MouseY > pr_mouse_max) pr_mouse_max = pr_scr->MouseY;

	t0 = zod_fineclock_ticks();

	if(pr_have_last)
	{
		work = t0 - pr_last;
		pr_work_sum += (double)work;
	}

	BltBitMap(pr_off, 0, 0, pr_scr->RastPort.BitMap, 0, target * pr_h, pr_w, pr_h, 0xC0, 0xFF, NULL);
	WaitBlit();

	t1 = zod_fineclock_ticks();
	blit = t1 - t0;
	pr_blit_sum += (double)blit;
	pr_frames++;

	if(!sync)
	{
		pr_tear_frames++;
		pr_last = t1;
		pr_have_last = 1;
		return;
	}

	pan(target);

	t2 = zod_fineclock_ticks();
	wait = t2 - t1;
	pr_wait_sum += (double)wait;
	if(wait > pr_wait_max) pr_wait_max = wait;

	if(pr_have_last)
	{
		const unsigned long frame = t2 - pr_last;

		if(frame > pr_period * 3 / 2)
		{
			if(work + blit > pr_period) pr_late++;
			else pr_missed++;
		}
		else if(frame > pr_period / 2)
			pr_period = (pr_period * 15 + frame) / 16;   /* 50 and 60 Hz alike */
	}

	pr_last = t2;
	pr_have_last = 1;
}

#endif /* DISABLE_OPENGL */
