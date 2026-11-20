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
static int dbuf_on = -1;            /* ZOD_AGA_DBUF (default on) */

/* Two buffers (phase 2): the frame goes into the hidden bitmap, then
 * ChangeScreenBuffer shows it at the next blank -- no block is ever seen
 * half old, half new. Each buffer has its own block sums (what its planes
 * hold); a block converted into one is stale in the other, which gets it
 * the next time it is drawn into even when that frame does not mark it. */
enum { NBUF = 2 };
static struct ScreenBuffer *sbuf[NBUF];
static struct MsgPort *safe_port = 0, *disp_port = 0;
static int dbuf = 0;                /* double buffered now */
static int cur = 0;                 /* the buffer the next frame goes into */
static int safe_ok = 1, disp_ok = 1;
static ULONG *sums[NBUF];           /* two per 16x16 block */
static UBYTE *sums_valid[NBUF];
static UBYTE *stale[NBUF];
static int sums_gw = 0, sums_gh = 0;
static unsigned long st_frames = 0, st_groups = 0, st_ticks = 0, st_skipped = 0;
static unsigned long st_stale = 0, st_safe_waits = 0, st_change_failed = 0;
static unsigned long st_wait_ticks = 0;    /* waiting for a safe buffer */
static unsigned long st_check_frames = 0, st_check_differ = 0;

static int env_on(const char *name)
{
	char buf[16];
	const char *e = zod_env(name, buf, sizeof(buf));

	if(!e || !*e) return 0;
	if(e[0] == 'a' || e[0] == 'A' || e[0] == '0' || (e[0] == 'o' && e[1] == 'f')) return 0;

	return 1;
}

/* -C <mode> (the launcher's AGA choice); ZOD_AGA for tests */
char zod_aga_mode[16] = "";

int zod_aga_wanted(void)
{
	return zod_aga_mode[0] || env_on("ZOD_AGA");
}

/* the AGA modes: PAL HiRes Laced (any TV or 1084), and the two flicker-free
 * ones a 31 kHz monitor shows (their monitor drivers in DEVS:Monitors) */
static const struct { const char *name; ULONG id; const char *title; } aga_modes[] =
{
	{ "pal",    PAL_MONITOR_ID | HIRESLACE_KEY, "PAL HiRes Laced" },
	{ "dblpal", DBLPAL_MONITOR_ID | DBLPALHIRESFF_KEY, "DblPAL HiRes flicker-free" },
	{ "prod",   VGA_MONITOR_ID | VGAPRODUCT_KEY, "Multiscan Productivity" }
};

/* the mode asked for: its table index (0 = PAL HiRes Laced) */
static int mode_asked(void)
{
	char buf[16];
	const char *e = zod_aga_mode[0] ? zod_aga_mode : zod_env("ZOD_AGA", buf, sizeof(buf));
	unsigned i;

	for(i = 0; e && i < sizeof(aga_modes) / sizeof(aga_modes[0]); i++)
		if(!strcmp(e, aga_modes[i].name)) return (int)i;

	return 0;
}

/* the mode's nominal size (its DimensionInfo); 0 when it has none */
static int mode_size(ULONG id, int *mw, int *mh)
{
	struct DimensionInfo dims;
	DisplayInfoHandle hd = FindDisplayInfo(id);

	if(!hd || GetDisplayInfoData(hd, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, id) < sizeof(dims) - 32)
		return 0;

	*mw = dims.Nominal.MaxX - dims.Nominal.MinX + 1;
	*mh = dims.Nominal.MaxY - dims.Nominal.MinY + 1;
	return 1;
}

struct Screen *zod_aga_open(int w, int h, int *dy)
{
	int mi = mode_asked();
	ULONG id = aga_modes[mi].id;
	int sw = 640, sh = 512;
	ULONG err = 0;
	struct Screen *scr;

	/* a flicker-free mode without its monitor driver: PAL HiRes Laced */
	if(mi != 0 && (ModeNotAvailable(id) || !mode_size(id, &sw, &sh) || sw != 640 || sh < h))
	{
		ZLOG("AGA: %s is not available here (monitor driver in DEVS:Monitors?) -- "
		     "PAL HiRes Laced\n", aga_modes[mi].title);
		mi = 0;
		id = aga_modes[0].id;
	}

	if(mi == 0) { sw = 640; sh = 512; }

	if(w != 640 || h > sh)
	{
		ZLOG("AGA: the screen is %dx%d, the game asks for %dx%d -- no AGA screen\n", sw, sh, w, h);
		return 0;
	}

	if(!(GfxBase->ChipRevBits0 & GFXF_AA_ALICE))
	{
		ZLOG("AGA: no AGA chipset (8 planes in hires need it) -- no AGA screen\n");
		return 0;
	}

	if(ModeNotAvailable(id))
	{
		ZLOG("AGA: %s is not available (%08lx) -- no AGA screen\n",
		     aga_modes[mi].title, (unsigned long)ModeNotAvailable(id));
		return 0;
	}

	{
		struct TagItem sa[] = {
			{ SA_DisplayID,   id },
			{ SA_Width,       (ULONG)sw },
			{ SA_Height,      (ULONG)sh },
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

	*dy = (sh - h) / 2;

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

	if(dbuf_on < 0)
	{
		char buf[16];
		const char *e = zod_env("ZOD_AGA_DBUF", buf, sizeof(buf));

		dbuf_on = !(e && (e[0] == 'a' || e[0] == 'A' || e[0] == '0' || (e[0] == 'o' && e[1] == 'f')));
	}

	dbuf = 0;

	if(dbuf_on)
	{
		safe_port = CreateMsgPort();
		disp_port = CreateMsgPort();
		sbuf[0] = AllocScreenBuffer(scr, 0, SB_SCREEN_BITMAP);
		sbuf[1] = AllocScreenBuffer(scr, 0, SB_COPY_BITMAP);

		if(safe_port && disp_port && sbuf[0] && sbuf[1])
		{
			int i;

			for(i = 0; i < NBUF; i++)
			{
				sbuf[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe_port;
				sbuf[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = disp_port;
			}

			dbuf = 1;
			cur = 1;            /* buffer 0 is on display */
			safe_ok = disp_ok = 1;
		}
		else
		{
			ZLOG("AGA: no second buffer (chip RAM?) -- one buffer\n");
			if(sbuf[1]) FreeScreenBuffer(scr, sbuf[1]);
			if(sbuf[0]) FreeScreenBuffer(scr, sbuf[0]);
			sbuf[0] = sbuf[1] = 0;
			if(safe_port) DeleteMsgPort(safe_port);
			if(disp_port) DeleteMsgPort(disp_port);
			safe_port = disp_port = 0;
		}
	}

	{
		int i;

		sums_gw = w / 16;
		sums_gh = (h + 15) / 16;

		for(i = 0; i < NBUF; i++)
		{
			free(sums[i]); free(sums_valid[i]); free(stale[i]);
			sums[i] = 0; sums_valid[i] = 0; stale[i] = 0;
		}

		for(i = 0; sums_on && i < (dbuf ? NBUF : 1); i++)
		{
			sums[i] = (ULONG *)malloc((size_t)sums_gw * sums_gh * 2 * sizeof(ULONG));
			sums_valid[i] = (UBYTE *)calloc((size_t)sums_gw * sums_gh, 1);

			if(!sums[i] || !sums_valid[i])
			{
				ZLOG("AGA: no memory for the block sums -- without them\n");
				sums_on = 0;
			}
		}

		if(!sums_on)
			for(i = 0; i < NBUF; i++)
			{
				free(sums[i]); free(sums_valid[i]);
				sums[i] = 0; sums_valid[i] = 0;
			}

		/* two buffers need the stale marks, with or without the sums */
		for(i = 0; dbuf && i < NBUF; i++)
		{
			stale[i] = (UBYTE *)calloc((size_t)sums_gw * sums_gh, 1);

			if(!stale[i])
			{
				ZLOG("AGA: no memory for the stale marks -- one buffer\n");
				zod_aga_close(scr);   /* frees the second buffer */
				break;
			}
		}
	}

	ZLOG("AGA: %s %dx%d, 8 planes, the picture %dx%d from row %d; "
	     "chunky to planar by dirty blocks, %s%s\n",
	     aga_modes[mi].title, sw, sh,
	     w, h, *dy, dbuf ? "two buffers (ChangeScreenBuffer)" : "into the shown bitmap",
	     check_on ? "; every frame read back (ZOD_AGA_CHECK)" : "");

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

/* block k (16x16 at gx, gy) of the compose surface into buffer b's planes,
 * when its pixels are not what they hold already; 1 = converted */
static int block_out(const SDL_Surface *cs, UBYTE *const *pl, int bpr, int dy, int b,
                     int gx, int gy, int rows)
{
	const UBYTE *src = (const UBYTE *)cs->pixels + (long)gy * 16 * cs->pitch;
	const long k = (long)gy * sums_gw + gx;

	if(sums[b])
	{
		ULONG x, y;

		block_sum16(src + gx * 16, cs->pitch, rows, &x, &y);

		/* the other buffer: stale wherever it does not hold these pixels --
		 * whether or not this buffer needs them (the pointer gone from a
		 * place this buffer already shows bare must still go from the
		 * other one) */
		if(dbuf && stale[b ^ 1])
		{
			const int o = b ^ 1;

			if(!sums_valid[o][k] || sums[o][2 * k] != x || sums[o][2 * k + 1] != y)
				stale[o][k] = 1;
		}

		if(sums_valid[b][k] && sums[b][2 * k] == x && sums[b][2 * k + 1] == y)
		{
			st_skipped += (unsigned long)rows;
			return 0;
		}

		sums[b][2 * k] = x;
		sums[b][2 * k + 1] = y;
		sums_valid[b][k] = 1;
	}
	else if(dbuf && stale[b ^ 1])
		stale[b ^ 1][k] = 1;    /* without sums: the other one gets it too */

	c2p_rows(src, cs->pitch, pl, bpr, gy * 16 + dy, rows, gx, gx);
	st_groups += (unsigned long)rows;

	return 1;
}

void zod_aga_flip(SDL_Surface *cs, struct Screen *scr, int dy)
{
	enum { MAX_RUNS = 24 };
	struct BitMap *bm;
	UBYTE *pl[8];
	short rx0[MAX_RUNS], rx1[MAX_RUNS];
	const int schub = zod_dirty_block_shift();
	const int block = 1 << schub;
	const int w = cs->w, h = cs->h;
	unsigned long t0;
	const int b = dbuf ? cur : 0;
	unsigned long bytes = 0;
	int p, y0, i;

	/* the buffer about to be drawn into may still be on display */
	if(dbuf && !safe_ok)
	{
		const unsigned long w0 = zod_fineclock_ticks();

		while(!GetMsg(safe_port)) { WaitPort(safe_port); st_safe_waits++; }
		safe_ok = 1;
		st_wait_ticks += zod_fineclock_ticks() - w0;
	}

	t0 = zod_fineclock_ticks();
	bm = dbuf ? sbuf[b]->sb_BitMap : scr->RastPort.BitMap;
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

			if(block == 16 && (sums[b] || dbuf))
			{
				int g;

				for(g = x0 >> 4; g <= (x1 >> 4); g++)
				{
					if(stale[b]) stale[b][(long)(y0 >> 4) * sums_gw + g] = 0;
					if(block_out(cs, pl, bm->BytesPerRow, dy, b, g, y0 >> 4, rows))
						bytes += 16UL * rows;
				}

				continue;
			}

			c2p_rows((const UBYTE *)cs->pixels + (long)y0 * cs->pitch, cs->pitch, pl,
			         bm->BytesPerRow, y0 + dy, rows, x0 >> 4, x1 >> 4);

			st_groups += (unsigned long)((x1 >> 4) - (x0 >> 4) + 1) * rows;
			bytes += (unsigned long)((x1 >> 4) - (x0 >> 4) + 1) * 16 * rows;
		}
	}

	/* the blocks the other buffer got last time and this one has not */
	if(dbuf && stale[b])
	{
		const long nk = (long)sums_gw * sums_gh;
		long k;

		for(k = 0; k < nk; k++)
			if(stale[b][k])
			{
				const int gy = (int)(k / sums_gw);
				const int rows = (h - gy * 16 < 16) ? h - gy * 16 : 16;

				stale[b][k] = 0;
				st_stale++;

				if(block_out(cs, pl, bm->BytesPerRow, dy, b, (int)(k % sums_gw), gy, rows))
					bytes += 16UL * rows;
			}
	}

	zod_dirty_bytes += bytes;
	zod_dirty_frames++;
	zod_dirty_sweep();
	zod_dirty_clear();

	/* the work: sums, c2p, the chip RAM writes (not the wait, not the check) */
	st_ticks += zod_fineclock_ticks() - t0;

	if(check_on > 0)
	{
		st_check_differ += check_all(cs, pl, bm->BytesPerRow, dy);
		st_check_frames++;
	}

	/* show it at the next blank; the old one is written again only after
	 * its safe message */
	if(dbuf)
	{
		if(!disp_ok)
		{
			while(!GetMsg(disp_port)) WaitPort(disp_port);
			disp_ok = 1;
		}

		if(ChangeScreenBuffer(scr, sbuf[b]))
		{
			safe_ok = 0;
			disp_ok = 0;
			cur = b ^ 1;
		}
		else
			st_change_failed++;
	}

	st_frames++;
}

void zod_aga_close(struct Screen *scr)
{
	int i;

	if(dbuf)
	{
		/* the screen's own bitmap back on display, every message home */
		if(!disp_ok) { while(!GetMsg(disp_port)) WaitPort(disp_port); disp_ok = 1; }
		if(!safe_ok) { while(!GetMsg(safe_port)) WaitPort(safe_port); safe_ok = 1; }

		if(cur == 0 && ChangeScreenBuffer(scr, sbuf[0]))
		{
			while(!GetMsg(disp_port)) WaitPort(disp_port);
			while(!GetMsg(safe_port)) WaitPort(safe_port);
		}

		FreeScreenBuffer(scr, sbuf[1]);
		FreeScreenBuffer(scr, sbuf[0]);
		sbuf[0] = sbuf[1] = 0;
		DeleteMsgPort(safe_port);
		DeleteMsgPort(disp_port);
		safe_port = disp_port = 0;
		dbuf = 0;
	}

	for(i = 0; i < NBUF; i++)
	{
		free(sums[i]); free(sums_valid[i]); free(stale[i]);
		sums[i] = 0; sums_valid[i] = 0; stale[i] = 0;
	}
}

void zod_aga_report(void)
{
	const unsigned long f = zod_fineclock_freq();

	if(!st_frames) return;

	ZLOG("AGA: %lu frames, %lu 16-pixel groups a frame through c2p, %lu left out "
	     "(their block unchanged, ZOD_AGA_SUMS %s), %lu stale blocks a frame from the other "
	     "buffer; waits for a safe buffer %lu (%lu us a frame), failed changes %lu; %lu us a "
	     "frame (sums, c2p and the chip RAM writes)%s",
	     st_frames, st_groups / st_frames, st_skipped / st_frames, sums_on ? "on" : "off",
	     st_stale / st_frames, st_safe_waits,
	     f >= 1000 ? (st_wait_ticks / st_frames) * 1000UL / (f / 1000UL) : 0UL,
	     st_change_failed,
	     f >= 1000 ? (st_ticks / st_frames) * 1000UL / (f / 1000UL) : 0UL,
	     check_on > 0 ? "" : "\n");

	if(check_on > 0)
		ZLOG("; read back in %lu frames: %lu pixels differ\n", st_check_frames, st_check_differ);
}
