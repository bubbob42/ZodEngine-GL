/*
 * zod_aga.cpp -- the AGA screen (zod_aga.h, docs/concept-aga.md).
 *
 * PAL HiRes Laced 640x512 with 8 bitplanes (AGA only: OCS/ECS have at most
 * 4 planes in hires). The engine keeps drawing its 640x480 chunky compose
 * surface; it sits centred, 16 rows from the top. The palette is the shared
 * one, loaded with LoadRGB32 as on RTG (sdl_screen.cpp).
 *
 * The copy: a dirty block is 16 pixels wide, exactly one word of each
 * plane per row. Chunky to planar per 16 pixels: two 8x8 bit transposes
 * (Hacker's Delight's transpose8, on two longwords of four pixels each);
 * the 8 bytes that come out are planes 7..0 for 8 pixels, the leftmost
 * pixel in the top bit -- the Amiga's planar order. The transpose is its
 * own inverse: ZOD_AGA_CHECK reads the planes back through it and
 * compares every pixel with the compose surface ("0 differ").
 *
 * Phase 1 writes into the shown bitmap (one buffer): a block may show half
 * old, half new for one frame. Double buffering and an assembler c2p are
 * phase 2 (the concept's milestones).
 */
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "zod_aga.h"
#include "zod_log.h"
#include "amiga_startup.h"
#include "fineclock.h"

extern struct GfxBase *GfxBase;

extern "C" int  zod_dirty_out_runs(int blockzeile, short *x0, short *x1, int max);
extern "C" int  zod_dirty_block_shift(void);
extern "C" void zod_dirty_sweep(void);
extern "C" void zod_dirty_clear(void);
extern unsigned long long zod_dirty_bytes;
extern unsigned long zod_dirty_frames;

static int check_on = -1;
static int sums_on = -1;            /* ZOD_AGA_SUMS (default on) */
static ULONG *sums = 0;             /* two per 16x16 block: what the planes hold */
static UBYTE *sums_valid = 0;
static int sums_gw = 0, sums_gh = 0;
static unsigned long st_frames = 0, st_groups = 0, st_ticks = 0, st_skipped = 0;
static unsigned long st_check_frames = 0, st_check_differ = 0;

static int env_on(const char *name)
{
	char buf[16];
	const char *e = zod_env(name, buf, sizeof(buf));

	if(!e || !*e) return 0;
	if(e[0] == 'a' || e[0] == 'A' || e[0] == '0' || (e[0] == 'o' && e[1] == 'f')) return 0;

	return 1;
}

int zod_aga_wanted(void)
{
	return env_on("ZOD_AGA");
}

struct Screen *zod_aga_open(int w, int h, int *dy)
{
	const ULONG id = PAL_MONITOR_ID | HIRESLACE_KEY;
	ULONG err = 0;
	struct Screen *scr;

	if(w != 640 || h > 512)
	{
		ZLOG("AGA: the screen is 640x512, the game asks for %dx%d -- no AGA screen\n", w, h);
		return 0;
	}

	if(!(GfxBase->ChipRevBits0 & GFXF_AA_ALICE))
	{
		ZLOG("AGA: no AGA chipset (8 planes in hires need it) -- no AGA screen\n");
		return 0;
	}

	if(ModeNotAvailable(id))
	{
		ZLOG("AGA: PAL HiRes Laced is not available (%08lx) -- no AGA screen\n",
		     (unsigned long)ModeNotAvailable(id));
		return 0;
	}

	{
		struct TagItem sa[] = {
			{ SA_DisplayID,   id },
			{ SA_Width,       640 },
			{ SA_Height,      512 },
			{ SA_Depth,       8 },
			{ SA_Type,        CUSTOMSCREEN },
			{ SA_Quiet,       TRUE },
			{ SA_ShowTitle,   FALSE },
			{ SA_Draggable,   FALSE },
			{ SA_AutoScroll,  FALSE },
			{ SA_Interleaved, FALSE },
			{ SA_SharePens,   TRUE },
			{ SA_ErrorCode,   (ULONG)&err },
			{ TAG_DONE, 0 }
		};

		scr = OpenScreenTagList(0, sa);
	}

	if(!scr)
	{
		ZLOG("AGA: OpenScreen failed (error %ld)\n", (long)err);
		return 0;
	}

	*dy = (512 - h) / 2;

	if(check_on < 0) check_on = env_on("ZOD_AGA_CHECK");

	/* the block sums: a dirty block whose pixels are what the planes hold
	 * already is not converted again -- on AGA every word saved is a chip
	 * RAM write saved (the RTG path's flip sums, the same sum) */
	if(sums_on < 0)
	{
		char buf[16];
		const char *e = zod_env("ZOD_AGA_SUMS", buf, sizeof(buf));

		sums_on = !(e && (e[0] == 'a' || e[0] == 'A' || e[0] == '0' || (e[0] == 'o' && e[1] == 'f')));
	}

	free(sums);
	free(sums_valid);
	sums = 0;
	sums_valid = 0;
	sums_gw = w / 16;
	sums_gh = (h + 15) / 16;

	if(sums_on)
	{
		sums = (ULONG *)malloc((size_t)sums_gw * sums_gh * 2 * sizeof(ULONG));
		sums_valid = (UBYTE *)calloc((size_t)sums_gw * sums_gh, 1);
		if(!sums || !sums_valid) { free(sums); free(sums_valid); sums = 0; sums_valid = 0; }
	}

	ZLOG("AGA: PAL HiRes Laced 640x512, 8 planes, the picture %dx%d from row %d; "
	     "chunky to planar by dirty blocks into the shown bitmap%s\n",
	     w, h, *dy, check_on ? "; every frame read back (ZOD_AGA_CHECK)" : "");

	return scr;
}

/* the 8x8 bit transpose: x = pixels 0..3, y = 4..7 (a byte each, the first
 * in the top byte) in, x = planes 7..4, y = planes 3..0 out (a byte each,
 * the leftmost pixel in the top bit) -- and back the same way */
static inline void transpose8(ULONG &x, ULONG &y)
{
	ULONG t;

	t = (x ^ (x >> 7)) & 0x00AA00AAUL;  x = x ^ t ^ (t << 7);
	t = (y ^ (y >> 7)) & 0x00AA00AAUL;  y = y ^ t ^ (t << 7);
	t = (x ^ (x >> 14)) & 0x0000CCCCUL; x = x ^ t ^ (t << 14);
	t = (y ^ (y >> 14)) & 0x0000CCCCUL; y = y ^ t ^ (t << 14);
	t = (x & 0xF0F0F0F0UL) | ((y >> 4) & 0x0F0F0F0FUL);
	y = ((x << 4) & 0xF0F0F0F0UL) | (y & 0x0F0F0F0FUL);
	x = t;
}

/* rows of 16-pixel groups g0..g1 from the compose rows into the planes */
static void c2p_rows(const UBYTE *src, int spitch, UBYTE *const *pl, int bpr,
                     int prow, int rows, int g0, int g1)
{
	int r, g;

	for(r = 0; r < rows; r++)
	{
		const ULONG *s = (const ULONG *)(src + (long)r * spitch + g0 * 16);
		long off = (long)(prow + r) * bpr + g0 * 2;

		for(g = g0; g <= g1; g++, off += 2)
		{
			ULONG a = s[0], b = s[1], c = s[2], d = s[3];

			s += 4;
			transpose8(a, b);   /* left 8 pixels: a planes 7..4, b planes 3..0 */
			transpose8(c, d);   /* right 8 pixels */

			*(UWORD *)(pl[7] + off) = (UWORD)(((a >> 16) & 0xFF00) | (c >> 24));
			*(UWORD *)(pl[6] + off) = (UWORD)(((a >> 8) & 0xFF00) | ((c >> 16) & 0xFF));
			*(UWORD *)(pl[5] + off) = (UWORD)((a & 0xFF00) | ((c >> 8) & 0xFF));
			*(UWORD *)(pl[4] + off) = (UWORD)(((a << 8) & 0xFF00) | (c & 0xFF));
			*(UWORD *)(pl[3] + off) = (UWORD)(((b >> 16) & 0xFF00) | (d >> 24));
			*(UWORD *)(pl[2] + off) = (UWORD)(((b >> 8) & 0xFF00) | ((d >> 16) & 0xFF));
			*(UWORD *)(pl[1] + off) = (UWORD)((b & 0xFF00) | ((d >> 8) & 0xFF));
			*(UWORD *)(pl[0] + off) = (UWORD)(((b << 8) & 0xFF00) | (d & 0xFF));
		}
	}
}

/* the RTG path's block sum (sdl_screen.cpp block_sum), for a 16-wide block */
static void block_sum16(const UBYTE *p, long pitch, int rows, ULONG *a, ULONG *b)
{
	ULONG x = 0x9e3779b9UL, y = 2166136261UL;
	int r;

	for(r = 0; r < rows; r++, p += pitch)
	{
		const ULONG *q = (const ULONG *)p;

		x = ((x << 1) | (x >> 31)) + q[0]; y = y * 33 + q[0];
		x = ((x << 1) | (x >> 31)) + q[1]; y = y * 33 + q[1];
		x = ((x << 1) | (x >> 31)) + q[2]; y = y * 33 + q[2];
		x = ((x << 1) | (x >> 31)) + q[3]; y = y * 33 + q[3];
	}

	*a = x;
	*b = y;
}

/* ZOD_AGA_CHECK: the whole picture read back from the planes, compared */
static unsigned long check_all(const SDL_Surface *cs, UBYTE *const *pl, int bpr, int dy)
{
	unsigned long differ = 0;
	int y, g, i;

	for(y = 0; y < cs->h; y++)
	{
		const UBYTE *s = (const UBYTE *)cs->pixels + (long)y * cs->pitch;
		const long row = (long)(y + dy) * bpr;

		for(g = 0; g < cs->w / 16; g++)
		{
			UWORD wd[8];
			ULONG a, b, c, d;
			UBYTE px[16];
			int p;

			for(p = 0; p < 8; p++) wd[p] = *(const UWORD *)(pl[p] + row + g * 2);

			a = ((ULONG)(wd[7] >> 8) << 24) | ((ULONG)(wd[6] >> 8) << 16) |
			    ((ULONG)(wd[5] >> 8) << 8) | (wd[4] >> 8);
			b = ((ULONG)(wd[3] >> 8) << 24) | ((ULONG)(wd[2] >> 8) << 16) |
			    ((ULONG)(wd[1] >> 8) << 8) | (wd[0] >> 8);
			c = ((ULONG)(wd[7] & 0xFF) << 24) | ((ULONG)(wd[6] & 0xFF) << 16) |
			    ((ULONG)(wd[5] & 0xFF) << 8) | (wd[4] & 0xFF);
			d = ((ULONG)(wd[3] & 0xFF) << 24) | ((ULONG)(wd[2] & 0xFF) << 16) |
			    ((ULONG)(wd[1] & 0xFF) << 8) | (wd[0] & 0xFF);
			transpose8(a, b);
			transpose8(c, d);

			for(i = 0; i < 4; i++)
			{
				px[i]      = (UBYTE)(a >> (24 - 8 * i));
				px[4 + i]  = (UBYTE)(b >> (24 - 8 * i));
				px[8 + i]  = (UBYTE)(c >> (24 - 8 * i));
				px[12 + i] = (UBYTE)(d >> (24 - 8 * i));
			}

			for(i = 0; i < 16; i++)
				if(px[i] != s[g * 16 + i]) differ++;
		}
	}

	return differ;
}

void zod_aga_flip(SDL_Surface *cs, struct Screen *scr, int dy)
{
	enum { MAX_RUNS = 24 };
	struct BitMap *bm = scr->RastPort.BitMap;
	UBYTE *pl[8];
	short rx0[MAX_RUNS], rx1[MAX_RUNS];
	const int schub = zod_dirty_block_shift();
	const int block = 1 << schub;
	const int w = cs->w, h = cs->h;
	const unsigned long t0 = zod_fineclock_ticks();
	unsigned long bytes = 0;
	int p, y0, i;

	for(p = 0; p < 8; p++) pl[p] = (UBYTE *)bm->Planes[p];

	for(y0 = 0; y0 < h; y0 += block)
	{
		const int rows = (h - y0 < block) ? h - y0 : block;
		const int n = zod_dirty_out_runs(y0 >> schub, rx0, rx1, MAX_RUNS);

		for(i = 0; i < n; i++)
		{
			int x0 = rx0[i], x1 = rx1[i];

			if(x0 < 0) x0 = 0;
			if(x1 > w - 1) x1 = w - 1;
			if(x1 < x0) continue;

			if(sums && block == 16)
			{
				/* block by block: only those whose pixels changed */
				const UBYTE *row = (const UBYTE *)cs->pixels + (long)y0 * cs->pitch;
				int g, run0 = -1;

				for(g = x0 >> 4; g <= (x1 >> 4) + 1; g++)
				{
					int changed = 0;

					if(g <= (x1 >> 4))
					{
						const long k = (long)(y0 >> 4) * sums_gw + g;
						ULONG a, b;

						block_sum16(row + g * 16, cs->pitch, rows, &a, &b);

						if(!sums_valid[k] || sums[2 * k] != a || sums[2 * k + 1] != b)
						{
							changed = 1;
							sums[2 * k] = a;
							sums[2 * k + 1] = b;
							sums_valid[k] = 1;
						}
						else
							st_skipped += (unsigned long)rows;
					}

					if(changed && run0 < 0) run0 = g;

					if(!changed && run0 >= 0)
					{
						c2p_rows(row, cs->pitch, pl, bm->BytesPerRow, y0 + dy, rows, run0, g - 1);
						st_groups += (unsigned long)(g - run0) * rows;
						bytes += (unsigned long)(g - run0) * 16 * rows;
						run0 = -1;
					}
				}

				continue;
			}

			c2p_rows((const UBYTE *)cs->pixels + (long)y0 * cs->pitch, cs->pitch, pl,
			         bm->BytesPerRow, y0 + dy, rows, x0 >> 4, x1 >> 4);

			st_groups += (unsigned long)((x1 >> 4) - (x0 >> 4) + 1) * rows;
			bytes += (unsigned long)((x1 >> 4) - (x0 >> 4) + 1) * 16 * rows;
		}
	}

	zod_dirty_bytes += bytes;
	zod_dirty_frames++;
	zod_dirty_sweep();
	zod_dirty_clear();

	st_ticks += zod_fineclock_ticks() - t0;
	st_frames++;

	if(check_on > 0)
	{
		st_check_differ += check_all(cs, pl, bm->BytesPerRow, dy);
		st_check_frames++;
	}
}

void zod_aga_report(void)
{
	const unsigned long f = zod_fineclock_freq();

	if(!st_frames) return;

	ZLOG("AGA: %lu frames, %lu 16-pixel groups a frame through c2p, %lu left out "
	     "(their block unchanged, ZOD_AGA_SUMS %s), %lu us a frame (sums, c2p and the "
	     "chip RAM writes)%s",
	     st_frames, st_groups / st_frames, st_skipped / st_frames, sums ? "on" : "off",
	     f >= 1000 ? (st_ticks / st_frames) * 1000UL / (f / 1000UL) : 0UL,
	     check_on > 0 ? "" : "\n");

	if(check_on > 0)
		ZLOG("; read back in %lu frames: %lu pixels differ\n", st_check_frames, st_check_differ);
}
