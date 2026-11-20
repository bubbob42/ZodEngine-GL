/*
 * zod_rtgscroll.cpp -- scrolling on the card for the software RTG path
 * (zod_rtgscroll.h, docs/concept-smooth-scrolling.md "Software RTG").
 *
 * While the view scrolls, every block of the map area changes, so today's
 * copy sends the whole 540x444 map area over the bus each frame (~240 KB;
 * at the A4000D's ~120 ns a byte into card memory ~29 ms). But most of the
 * new frame is the old one moved by the scroll: the board can move it with
 * its blitter. So SDL_Flip (sdl_screen.cpp) moves the map area's screen
 * content by the view's step first, then compares each 16x16 block of the
 * new frame with the previous frame's pixels at the moved place (in fast
 * RAM) and sends only the blocks that differ: the newly shown strip, units,
 * effects.
 *
 * The previous frame is a copy of what the screen shows (the compose
 * surface at the flip, pointer included), kept up to date with the same
 * dirty runs the flip sends.
 */
#include <stdlib.h>
#include <string.h>

#include "zod_rtgscroll.h"
#include "zod_log.h"
#include "amiga_startup.h"

static int on = -1;
static int cur_set = 0, cur_sx, cur_sy, cur_rx, cur_ry, cur_vw, cur_vh;
static int last_set = 0, last_sx, last_sy, last_rx, last_ry, last_vw, last_vh;
static unsigned char *prev = 0;
static int prev_w = 0, prev_h = 0, prev_valid = 0;
static unsigned long st_frames = 0, st_sent = 0, st_kept = 0, st_flips = 0;

int zod_rtgscroll_on(void)
{
	if(on < 0)
	{
		char buf[16];
		const char *e = zod_env("ZOD_RTGSCROLL", buf, sizeof(buf));

		on = (e && (e[0] == 'e' || e[0] == 'E' || e[0] == '1' || (e[0] == 'o' && e[1] == 'n'))) ? 1 : 0;

		if(on) ZLOG("RTG scroll: the map area moves on the card while the view scrolls (ZOD_RTGSCROLL)\n");
	}

	return on;
}

extern "C" void zod_rtgscroll_view(int sx, int sy, int rx, int ry, int vw, int vh)
{
	cur_set = 1;
	cur_sx = sx; cur_sy = sy;
	cur_rx = rx; cur_ry = ry;
	cur_vw = vw; cur_vh = vh;
}

int zod_rtgscroll_plan(int w, int h, int *dx, int *dy, int *rx, int *ry, int *vw, int *vh)
{
	int use = 0;

	st_flips++;

	if(!zod_rtgscroll_on()) { cur_set = 0; return 0; }

	if(cur_set && last_set && prev_valid && prev && prev_w == w && prev_h == h &&
	   cur_rx == last_rx && cur_ry == last_ry && cur_vw == last_vw && cur_vh == last_vh &&
	   cur_sx >= 0 && cur_sy >= 0 && last_sx >= 0 && last_sy >= 0 &&
	   (cur_sx != last_sx || cur_sy != last_sy) &&
	   cur_rx >= 0 && cur_ry >= 0 && cur_rx + cur_vw <= w && cur_ry + cur_vh <= h)
	{
		const int ddx = cur_sx - last_sx, ddy = cur_sy - last_sy;

		if((ddx < 0 ? -ddx : ddx) < cur_vw - 16 && (ddy < 0 ? -ddy : ddy) < cur_vh - 16)
		{
			*dx = ddx; *dy = ddy;
			*rx = cur_rx; *ry = cur_ry;
			*vw = cur_vw; *vh = cur_vh;
			use = 1;
			st_frames++;
		}
	}

	/* this frame's view is the next one's "last" -- only a frame that drew
	 * the map has one */
	last_set = cur_set;
	if(cur_set)
	{
		last_sx = cur_sx; last_sy = cur_sy;
		last_rx = cur_rx; last_ry = cur_ry;
		last_vw = cur_vw; last_vh = cur_vh;
	}
	cur_set = 0;

	return use;
}

unsigned char *zod_rtgscroll_prev(int w, int h, int *valid)
{
	if(!prev || prev_w != w || prev_h != h)
	{
		free(prev);
		prev = (unsigned char *)malloc((size_t)w * h);
		prev_w = prev ? w : 0;
		prev_h = prev ? h : 0;
		prev_valid = 0;
	}

	*valid = prev_valid;
	return prev;
}

void zod_rtgscroll_prev_set_valid(int v)
{
	prev_valid = v;
}

void zod_rtgscroll_count(unsigned long sent_blocks, unsigned long kept_blocks)
{
	st_sent += sent_blocks;
	st_kept += kept_blocks;
}

void zod_rtgscroll_report(void)
{
	if(!on || !st_flips) return;

	ZLOG("RTG scroll: %lu of %lu flips moved the map area on the card; in them %lu blocks "
	     "sent and %lu kept from the move (%lu %% kept)\n",
	     st_frames, st_flips, st_sent, st_kept,
	     (st_sent + st_kept) ? st_kept * 100UL / (st_sent + st_kept) : 0UL);
}
