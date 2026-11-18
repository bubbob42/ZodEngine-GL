/*
 * zod_prof.cpp -- a PC profile of the benchmark (measuring aid).
 *
 * With ZOD_PROFILE=<file> set, the game task is sampled by RadeonGL's PC
 * sampler (port/amiga/pcprof.c, the one GLQuake, GLTron and Doom 64 use)
 * from the first frame of the -B run until its time is up. The file holds
 * two anchors -- minigl.library's glTexCoord2f hook (0 when the library is
 * not open: the software path) and zod_prof_start's own address -- from
 * which tools/prof_report.py places the library and the program:
 *
 *   python3 tools/prof_report.py <file> <the unstripped zod_0x0 that ran>
 *
 * ZOD_PROFILE_US=n sets the interval (default 10000 us), ZOD_PROFILE_LONGS=n
 * (64..512) how much of the stack each sample keeps -- more for deep library
 * call chains, at fewer samples: the buffer stays ~1 MB (AmiAgent fetches it
 * in parts), 4000 samples of 64 longwords by default (40 s). The sampler
 * costs a few percent: time profiled runs separately. Host builds: nothing.
 *
 * Sampling starts after ZOD_PROFILE_SKIP seconds of the run (default 15):
 * the first seconds are the map download, the path finder's set-up and the
 * first upload of every image, and a profile that starts at frame one is
 * mostly those (MEASURED: 13 s at 192 longwords were all start-up).
 */

#include "zod_prof.h"

#if defined(__amigaos__) && !defined(ZOD_SHIM)

#include <stdlib.h>

#include <exec/types.h>

extern "C" {
#include "pcprof.h"
}

#include "amiga_startup.h"
#include "zod_log.h"

#ifndef DISABLE_OPENGL
#include <SDL/SDL_opengl.h>
#endif

static char prof_path[256];
static int prof_on = 0;
static int prof_wanted = -1;      /* -1 not read, 0 no, 1 yes */
static double prof_skip = 15.0;
static unsigned long prof_lib = 0;

extern "C" void zod_prof_tick(double run_seconds)
{
	if(prof_wanted < 0)
	{
		char buf[16];
		const char *e = zod_env("ZOD_PROFILE", prof_path, sizeof(prof_path));

		prof_wanted = (e && *e) ? 1 : 0;

		e = zod_env("ZOD_PROFILE_SKIP", buf, sizeof(buf));
		if(e && *e) prof_skip = strtod(e, 0);
	}

	if(prof_wanted == 1 && !prof_on && run_seconds >= prof_skip)
	{
		prof_wanted = 2;   /* once */
		zod_prof_start();
	}
}

extern "C" void zod_prof_start(void)
{
	char buf[16];
	const char *e;
	unsigned long us = 10000, longs = 64;

	if(prof_on) return;

	e = zod_env("ZOD_PROFILE", prof_path, sizeof(prof_path));
	if(!e || !*e) return;

	e = zod_env("ZOD_PROFILE_US", buf, sizeof(buf));
	if(e && *e) us = strtoul(e, 0, 10);
	if(us < 1000) us = 1000;

	e = zod_env("ZOD_PROFILE_LONGS", buf, sizeof(buf));
	if(e && *e) longs = strtoul(e, 0, 10);
	if(longs < 64) longs = 64;
	if(longs > 512) longs = 512;

	prof_lib = 0;

#ifndef DISABLE_OPENGL
	if(MiniGLBase && MiniGLDispatch) prof_lib = (unsigned long)MiniGLDispatch->GLTexCoord2f;
#endif

	if(!PcProf_Start(us, 4000UL * 64 / longs, longs))
	{
		ZLOG("Profile: the sampler did not start (memory?)\n");
		return;
	}

	prof_on = 1;
	ZLOG("Profile: sampling every %lu us, %lu longwords a sample, into %s\n", us, longs, prof_path);
}

extern "C" void zod_prof_stop(void)
{
	if(!prof_on) return;

	PcProf_Stop(prof_path, prof_lib, (ULONG)zod_prof_start);
	prof_on = 0;
}

#else

extern "C" void zod_prof_start(void) {}
extern "C" void zod_prof_stop(void) {}
extern "C" void zod_prof_tick(double) {}

#endif
