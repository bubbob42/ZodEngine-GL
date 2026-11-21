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
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <graphics/videocontrol.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "zod_aga.h"
#include "zod_c2p.h"
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
static int c2p32_on = -1;           /* ZOD_AGA_C2P32: longword writes (default on) */

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

/* The buffer and the window's place in one step (ZOD_AGA_TOGETHER, default
 * on; the user's video of 21.11.: "side panel flickers on AGA while
 * scrolling"). ChangeScreenBuffer puts the new buffer on display at once,
 * at the place the display had; the ScrollVPort after it then moves it. A
 * frame that begins between the two calls shows the new buffer at the old
 * place: the map, anchored in the bitmap, looks right, but the side panel,
 * drawn at the new place, jumps by the scroll step. Intuition does the
 * change in its input task, so with the mouse moving the gap can be
 * milliseconds long. Now the RasInfo gets the new buffer and the new
 * offsets, and one ScrollVPort shows both (the autodoc's "double buffering
 * effect"); ChangeScreenBuffer then finds that buffer on display already
 * and only sends its messages and tells Intuition. Off: the old order. */
static int together_on = -1;
/* ZOD_AGA_CHECK: the copper list read at each buffer change that moves
 * the window -- between the two calls the display must hold the old buffer
 * at the old place or the new one at the new, nothing in between */
static unsigned long st_cop_checked = 0, st_cop_mixed = 0;

/* Hardware scrolling (milestone 4, docs/concept-aga.md; ON by default since
 * the user's word of 21.11., two buffers; -H off or ZOD_AGA_HWSCROLL=off
 * switch it off). The screen's bitmap is larger than the display and
 * anchored to the map: a map pixel keeps its place in the bitmap while the
 * view moves over it, and the display is moved with the ViewPort's RasInfo
 * offsets and ScrollVPort (system calls only, no copper list of our own).
 * Each buffer has a chunky shadow in fast RAM that holds exactly what its
 * planes hold; each frame the compose surface is compared into the shadow
 * at the current offset, and only the 16-pixel groups that differ go
 * through c2p -- the newly shown strip, units and effects, and the HUD
 * (which moves across the bitmap with the view: its new place and the map
 * put back at its old place differ, nothing else needs to know). When the
 * view nears the bitmap's edge the bitmap is anchored again around it: the
 * offset jumps and the compare rewrites what differs (about the window). */
static int hw_on = -1;              /* ZOD_AGA_HWSCROLL asked for */
static int hw = 0;                  /* the screen is the large one */
static int hw_mx = 192, hw_my = 64; /* the margins around the display */
static int hw_margins_given = 0;    /* ZOD_AGA_HWSCROLL=<x>x<y> */
static int hw_bw = 0, hw_bh = 0;    /* the bitmap */
static int hw_dw = 0, hw_dh = 0;    /* the display window */
static UBYTE *hw_shadow[NBUF];      /* each buffer's planes, chunky */
static UBYTE *hw_zero = 0;          /* a row of colour 0 (rows beside the picture) */
static int hw_anchored = 0, hw_ox = 0, hw_oy = 0;   /* map pixel at bitmap (0,0) */
static int hw_rx = 0, hw_ry = 0;    /* the display window's place in the bitmap */
static int hw_shown_rx = -1, hw_shown_ry = -1;      /* what the ViewPort shows */
/* the large bitmaps are our own (SA_BitMap, AllocScreenBuffer with a
 * bitmap): the screen keeps the size of the picture, so its display window
 * is exactly the picture's -- an oversized screen showed its whole bitmap
 * height (voltago, pal400: 528 rows instead of 400). [0] the screen's,
 * freed after CloseScreen (zod_aga_closed); [1] the second buffer's. */
static struct BitMap *hw_bm[NBUF];

/* The bottom panel as a screen of its own (the user, 21.11.: "make only
 * the existing 36-row bottom panel a separate, unmoving screen"; ZOD_AGA_
 * SPLIT, default on with soft scrolling). It stands still on the display
 * anyway; in the scrolled bitmap it moved every frame and was ~16..20 % of
 * the chip writes (docs/research-aga-speed.md 3.10). Now the map screen
 * shows the picture's rows above it, and a 640 x 36 screen below -- a
 * child of the map screen (SA_Parent), both exclusive, its colours
 * inherited (VC_NoColorPaletteLoad) and loaded the same anyway -- holds the
 * panel, written only where its pixels change. Between the two the
 * display needs a few blank rows (the copper switching viewports). */
enum { PANEL_H = 36 };
static int split_on = -1;
static int split_b = 0;             /* the picture's row where the panel begins (0: no split) */
static int split_off = 0;           /* the panel screen's top, in rows below the map screen's */
static struct Screen *pnl_scr = 0;
static struct Window *pnl_win = 0;
static UBYTE *pnl_shadow = 0;       /* the panel's planes, chunky, 640 x PANEL_H */
static unsigned long st_pnl_groups = 0, st_pnl_frames = 0;
static int view_set = 0, view_sx = 0, view_sy = 0;
static unsigned long st_hw_anchors = 0, st_hw_scrolls = 0, st_hw_moved = 0;

/* The gap's edges (the user, 21.11.: look B2 "but the sidebar should get a
 * bevel at its lower edge, too"; docs/concept-aga.md). The gap between the
 * two screens cut the side panel's art: the CNU3 tiles, the knob plate's
 * top edge. Shown instead, in the side column (the picture's last 100
 * columns) only:
 *   above the gap -- the right plate's bracket and grid 2 rows higher, the
 *     grid one row and one cell row shorter, the tiles whole and 6 rows
 *     higher with a plain row below them; the bottom 4 rows a ledge, the
 *     side panel's own shelf (the rows 42..45 above the gap);
 *   below the gap -- the bottom panel's top rail (8 rows) across the side
 *     column, the side art 8 rows lower under it (its last 8 rows drop
 *     off), the tiles' remains plain plate.
 * Nothing the engine draws moves there (its minimap ends 60 rows above the
 * gap, the pointer is a sprite). Never written into the picture: the
 * engine redraws the panels only when they change. A copy of those rows
 * per frame (76 x 116 bytes), from the 16 columns left of the side column
 * on, so a group of 16 that reaches into it reads from one place. Only in
 * frames that drew the map (a loading picture stays as it is);
 * ZOD_AGA_SEAM off: the picture's own rows. */
enum {
	SIDE_W = 100,                       /* the side column */
	PLATE_X = 54,                       /* the right plate, in the side column */
	SHELF_ROW = 46,                     /* the shelf's first row, above the gap */
	LEDGE_H = 4,
	RAIL_X = 240,                       /* the rail's source, left of the side column */
	RAIL_H = 8,
	DRESS_ABOVE = 40,                   /* rows dressed above the gap */
	DRESS_ROWS = DRESS_ABOVE + PANEL_H,
	DRESS_BPR = 16 + SIDE_W
};
static int seam_on = -1;
static UBYTE *dress_buf = 0;            /* DRESS_ROWS rows of DRESS_BPR bytes */
static int dress_x = 0;                 /* the side column's first column */
static int dress_now = 0;               /* this frame: the dressed rows are current */
static unsigned long st_dress_frames = 0;

/* the right plate's rows above the ledge (DRESS_ABOVE - LEDGE_H of them):
 * the picture's row each comes from, relative to the gap (at 640x480 the
 * gap is above row 444) */
static const signed char plate_src[DRESS_ABOVE - LEDGE_H] = {
	-38, -37, -36, -35, -34, -33, -32, -31, -30, -29, -28,     /* the bracket */
	-27, -25, -24, -23, -22, -21, -20, -19, -18, -17, -16, -15,  /* the grid */
	-11, -10, -9,                                   /* its last row, bezel, plain */
	-7, -6, -5, -4, -3, -2, -1, 0, 1,               /* the tiles */
	-9                                              /* a plain row */
};

/* -H off|on|fast (main.cpp): 0, 1 two buffers, 2 one buffer; -1 = the
 * environment decides (ZOD_AGA_HWSCROLL, ZOD_AGA_DBUF) */
int zod_aga_hw_option = -1;

/* the map view's rectangle on the picture (for ZOD_AGA_COLORS) */
static int view_rx = -1, view_ry = -1, view_w = 0, view_h = 0;

/* ZMap::DoRender's view offset and rectangle this frame (zod_rtgscroll_view) */
extern "C" void zod_aga_view(int sx, int sy, int rx, int ry, int vw, int vh)
{
	view_set = 1;
	view_sx = sx;
	view_sy = sy;
	view_rx = rx;
	view_ry = ry;
	view_w = vw;
	view_h = vh;
}

/* ZOD_AGA_COLORS (research, docs/research-aga-speed.md): which colour
 * indices each part of the picture uses over a run -- the map view (terrain,
 * units, effects), the side panel right of it, the bottom panel below it --
 * every 4th frame. Answers whether fewer planes or two playfields of 16
 * colours could show the game: how many indices carry 99 / 99.9 % of the
 * pixels of each part. */
static int env_on(const char *name);

enum { COL_VIEW = 0, COL_SIDE, COL_BOTTOM, COL_N };
static int colors_on = -1;
static unsigned long col_hist[COL_N][256];
static unsigned long col_frames = 0, col_seen = 0;

/* ZOD_AGA_REGIONS (research, docs/research-aga-speed.md): the groups soft
 * scrolling writes, by the part of the picture they show -- how much of
 * the chip RAM writing the HUD causes */
static int regions_on = -1;
static unsigned long reg_groups[COL_N], reg_frames = 0;

static void regions_count(int brow, int ga, int gb)
{
	const int y = brow - hw_ry;
	int g;

	if(regions_on < 0) regions_on = zod_env_switch("ZOD_AGA_REGIONS", 0);
	if(!regions_on || view_w <= 0) return;

	for(g = ga; g <= gb; g++)
	{
		const int x = g * 16 + 8 - hw_rx;   /* the group's middle on the picture */

		if(x >= view_rx + view_w) reg_groups[COL_SIDE]++;
		else if(y >= view_ry + view_h) reg_groups[COL_BOTTOM]++;
		else reg_groups[COL_VIEW]++;
	}
}

static void colors_count(const SDL_Surface *cs)
{
	int x, y;

	if(colors_on < 0) colors_on = env_on("ZOD_AGA_COLORS");
	if(!colors_on || view_w <= 0 || (col_seen++ & 3)) return;

	col_frames++;

	for(y = 0; y < cs->h; y++)
	{
		const UBYTE *p = (const UBYTE *)cs->pixels + (long)y * cs->pitch;

		for(x = 0; x < cs->w; x++)
		{
			int r;

			if(x >= view_rx + view_w) r = COL_SIDE;
			else if(y >= view_ry + view_h) r = COL_BOTTOM;
			else r = COL_VIEW;

			col_hist[r][p[x]]++;
		}
	}
}

/* indices used, and how many of the most used carry 99 / 99.9 % */
static void colors_line(const char *name, const unsigned long *h)
{
	unsigned long sorted[256], total = 0, acc = 0;
	int i, j, used = 0, n99 = 0, n999 = 0;

	for(i = 0; i < 256; i++) { sorted[i] = h[i]; total += h[i]; if(h[i]) used++; }

	/* largest first (256 values: a plain insertion sort) */
	for(i = 1; i < 256; i++)
	{
		const unsigned long v = sorted[i];

		for(j = i; j > 0 && sorted[j - 1] < v; j--) sorted[j] = sorted[j - 1];
		sorted[j] = v;
	}

	for(i = 0; i < 256 && total; i++)
	{
		acc += sorted[i];
		if(!n99 && acc * 100.0 >= total * 99.0) n99 = i + 1;
		if(!n999 && acc * 1000.0 >= total * 999.0) n999 = i + 1;
	}

	ZLOG("  %s: %d indices used, %d carry 99 %%, %d carry 99.9 %% (%lu pixels)\n",
	     name, used, n99, n999, total);
}

static int env_on(const char *name)
{
	return zod_env_switch(name, 0);
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
	{ "prod",   VGA_MONITOR_ID | VGAPRODUCT_KEY, "Multiscan Productivity" },
	/* PAL HiRes Laced with a 400-row picture, centred: 22 % fewer rows to
	 * convert and 112 fewer rows of display fetches (docs/research-aga-
	 * speed.md: +9 % chip bandwidth) -- the faster choice, less map shown */
	{ "pal400", PAL_MONITOR_ID | HIRESLACE_KEY, "PAL HiRes Laced (400 rows, centred)" }
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

/* the second buffer gone, the screen's own bitmap on display again */
static void buffers_free(struct Screen *scr)
{
	if(!dbuf) return;

	/* every message home first */
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

	/* our second large bitmap: not on display any more (the screen's own
	 * is back), FreeScreenBuffer leaves a given bitmap to its owner */
	if(hw_bm[1]) { WaitBlit(); FreeBitMap(hw_bm[1]); hw_bm[1] = 0; }
	DeleteMsgPort(safe_port);
	DeleteMsgPort(disp_port);
	safe_port = disp_port = 0;
	dbuf = 0;
}

struct Screen *zod_aga_open(int w, int h, int *dy)
{
	int mi = mode_asked();
	ULONG id = aga_modes[mi].id;
	int sw = 640, sh = 512;
	ULONG err = 0;
	struct Screen *scr;

	/* a flicker-free mode without its monitor driver: PAL HiRes Laced */
	if(mi != 0 && mi != 3 && (ModeNotAvailable(id) || !mode_size(id, &sw, &sh) || sw != 640 || sh < h))
	{
		ZLOG("AGA: %s is not available here (monitor driver in DEVS:Monitors?) -- "
		     "PAL HiRes Laced\n", aga_modes[mi].title);
		mi = 0;
		id = aga_modes[0].id;
	}

	if(mi == 0) { sw = 640; sh = 512; }
	if(mi == 3) { sw = 640; sh = 400; }   /* pal400: centred in the 512 rows */

	if(hw_on < 0)
	{
		char buf[16];
		const char *e = zod_env("ZOD_AGA_HWSCROLL", buf, sizeof(buf));
		/* -H decides; else ZOD_AGA_HWSCROLL (aus/off/0 = off); else on */
		hw_on = zod_aga_hw_option >= 0 ? (zod_aga_hw_option > 0) :
		        zod_env_switch("ZOD_AGA_HWSCROLL", 1);

		/* "<x>x<y>": the margins (x rounded up to 64 px: AGA's 64-bit fetch) */
		if(hw_on && e && e[0] >= '0' && e[0] <= '9')
		{
			char *end;
			const long a = strtol(e, &end, 10);

			if(*end == 'x')
			{
				const long b = strtol(end + 1, &end, 10);

				if(a >= 0 && a <= 1024 && b >= 0 && b <= 512)
				{
					hw_mx = (int)((a + 63) & ~63L);
					hw_my = (int)b;
					hw_margins_given = 1;
				}
			}
		}
	}

	hw = 0;

	/* the buffers first: the margins depend on how many bitmaps there are */
	if(dbuf_on < 0)
	{
		dbuf_on = zod_env_switch("ZOD_AGA_DBUF", 1);

		/* -H fast: hardware scrolling into the shown bitmap */
		if(zod_aga_hw_option == 2) dbuf_on = 0;
	}

	/* chip RAM written a longword at a time (docs/research-aga-speed.md):
	 * the CPU gets as many chip accesses a second whatever their width, so
	 * a longword moves twice the bytes; ZOD_AGA_C2P32=off keeps the words */
	if(c2p32_on < 0)
	{
		c2p32_on = zod_env_switch("ZOD_AGA_C2P32", 1);
	}

	/* the bottom panel on a screen of its own: soft scrolling, a picture
	 * that fills the mode (pal, pal400, dblpal at 640x512) */
	if(split_on < 0) split_on = zod_env_switch("ZOD_AGA_SPLIT", 1);
	split_b = (hw_on && split_on && h == sh && h > PANEL_H * 4) ? h - PANEL_H : 0;

	/* Margins as wide as the chip RAM allows (measured on voltago, the
	 * A4000D's real AGA: one buffer 6.8 fps at 192 px, 7.8 at 448; two
	 * buffers 5.3 at 192, 6.3 at 384 -- at a few frames a second the view
	 * moves far each frame, and every re-anchor rewrites the window). 448 px
	 * (a 1536 px bitmap) leaves a map up to ~1430 px wide without sideways
	 * re-anchors; 256 KB of chip RAM stay free for everyone else. */
	if(hw_on && !hw_margins_given)
	{
		static const int steps[] = { 448, 384, 320, 256, 192, 128, 64 };
		const unsigned long reserve = 256UL * 1024;
		const unsigned long avail = AvailMem(MEMF_CHIP);
		const unsigned long largest = AvailMem(MEMF_CHIP | MEMF_LARGEST);
		const unsigned long nb = dbuf_on ? 2 : 1;
		unsigned i;
		int chosen = 0;

		for(i = 0; i < sizeof(steps) / sizeof(steps[0]) && !chosen; i++)
		{
			/* 8 planes of (w / 8) x h bytes each, every plane one block */
			const unsigned long bytes = (unsigned long)(sw + 2 * steps[i]) *
			                            ((split_b ? split_b : sh) + 2 * hw_my);

			if(avail > reserve && nb * bytes <= avail - reserve && largest >= bytes / 8)
			{
				hw_mx = steps[i];
				chosen = 1;
			}
		}

		if(!chosen)
		{
			ZLOG("AGA: %lu KB of chip RAM free -- too little for hardware scrolling\n", avail / 1024);
			hw_on = 0;
		}
		else
			ZLOG("AGA: %lu KB of chip RAM free -- hardware scrolling with margins of %d px "
			     "(%lu buffer%s)\n", avail / 1024, hw_mx, nb, nb > 1 ? "s" : "");
	}

	if(hw_on)
	{
		int i;

		hw_dw = sw;
		hw_dh = split_b ? split_b : sh;     /* the map screen's rows */
		hw_bw = sw + 2 * hw_mx;
		hw_bh = hw_dh + 2 * hw_my;
		hw = 1;

		for(i = 0; i < NBUF; i++)
		{
			free(hw_shadow[i]);
			hw_shadow[i] = 0;
		}

		free(hw_zero);
		hw_zero = (UBYTE *)calloc((size_t)sw, 1);
		hw_shadow[0] = (UBYTE *)calloc((size_t)hw_bw * hw_bh, 1);

		if(!hw_zero || !hw_shadow[0])
		{
			ZLOG("AGA: no memory for the scrolling shadow (%dx%d) -- no hardware scrolling\n",
			     hw_bw, hw_bh);
			free(hw_zero); hw_zero = 0;
			free(hw_shadow[0]); hw_shadow[0] = 0;
			hw = 0;
		}

		hw_anchored = 0;
		hw_shown_rx = hw_shown_ry = -1;
		hw_rx = hw_mx;
		hw_ry = hw_my;
	}

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

	if(hw)
	{
		hw_bm[0] = AllocBitMap((ULONG)hw_bw, (ULONG)hw_bh, 8, BMF_DISPLAYABLE | BMF_CLEAR, 0);

		if(!hw_bm[0])
		{
			int i;

			ZLOG("AGA: no chip RAM for the %dx%d bitmap of hardware scrolling -- without it\n",
			     hw_bw, hw_bh);
			for(i = 0; i < NBUF; i++) { free(hw_shadow[i]); hw_shadow[i] = 0; }
			free(hw_zero);
			hw_zero = 0;
			hw = 0;
		}
	}

	{
		/* the display window: the whole mode, or for pal400 the 400 rows in
		 * the middle of PAL HiRes Laced's 512 (the screen placed there, its
		 * clip the same rows -- with hardware scrolling the bitmap is larger
		 * than the window, so the clip is what sizes it) */
		const int top = (mi == 3) ? (512 - sh) / 2 : 0;
		const int map_h = (hw && split_b) ? split_b : sh;
		struct Rectangle dclip = { 0, (WORD)top, (WORD)(sw - 1), (WORD)(top + map_h - 1) };
		struct TagItem sa[] = {
			{ SA_DisplayID,   id },
			{ SA_Width,       (ULONG)sw },
			{ SA_Height,      (ULONG)map_h },
			{ (top || map_h != sh) ? SA_DClip : TAG_IGNORE, (ULONG)&dclip },
			/* hardware scrolling: our larger bitmap behind a screen of the
			 * picture's size (the offsets move the picture over it) */
			{ hw ? SA_BitMap : TAG_IGNORE, (ULONG)hw_bm[0] },
			{ top ? SA_Top : TAG_IGNORE, (ULONG)top },
			/* nothing else on the display above or below a shorter picture */
			{ (top || map_h != sh) ? SA_Exclusive : TAG_IGNORE, TRUE },
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

		/* the screen did not open with the large bitmap: the normal screen
		 * without hardware scrolling rather than no screen */
		if(!scr && hw)
		{
			int i;

			ZLOG("AGA: the screen with the %dx%d bitmap for hardware scrolling did not open "
			     "(error %ld) -- without hardware scrolling\n", hw_bw, hw_bh, (long)err);

			for(i = 0; i < NBUF; i++) { free(hw_shadow[i]); hw_shadow[i] = 0; }
			free(hw_zero);
			hw_zero = 0;
			hw = 0;
			FreeBitMap(hw_bm[0]);
			hw_bm[0] = 0;

			sa[4].ti_Tag = TAG_IGNORE;            /* no SA_BitMap */
			sa[2].ti_Data = (ULONG)sh;            /* nor the panel screen */
			dclip.MaxY = (WORD)(top + sh - 1);
			if(!top) { sa[3].ti_Tag = TAG_IGNORE; sa[6].ti_Tag = TAG_IGNORE; }
			split_b = 0;
			err = 0;
			scr = OpenScreenTagList(0, sa);
		}
	}

	if(!scr)
	{
		ZLOG("AGA: OpenScreen failed (error %ld)%s\n", (long)err,
		     hw ? " -- the large bitmap for hardware scrolling (chip RAM?)" : "");
		return 0;
	}

	/* (the large bitmap starts as the shadow does: colour 0 everywhere --
	 * BMF_CLEAR) */

	if(!hw) split_b = 0;

	/* the bottom panel's own screen, below the map screen */
	if(split_b)
	{
		const int top = (mi == 3) ? (512 - sh) / 2 : 0;
		char buf[16];
		const char *e = zod_env("ZOD_AGA_GAP", buf, sizeof(buf));
		int gap = 6;    /* laced rows: what Intuition keeps between two viewports
		                 * here (FS-UAE: at 4 the map screen lost 2 rows) */
		ULONG perr = 0;

		if(e && e[0] >= '0' && e[0] <= '9') gap = (int)strtol(e, 0, 10);

		split_off = split_b + gap;

		{
			struct TagItem pa[] = {
				{ SA_DisplayID,   id },
				{ SA_Width,       (ULONG)sw },
				{ SA_Height,      (ULONG)PANEL_H },
				{ SA_Top,         (ULONG)(top + split_off) },
				{ SA_Depth,       8 },
				{ SA_Type,        CUSTOMSCREEN },
				{ SA_Parent,      (ULONG)scr },
				{ SA_Exclusive,   TRUE },
				{ SA_Quiet,       TRUE },
				{ SA_ShowTitle,   FALSE },
				{ SA_Draggable,   FALSE },
				{ SA_AutoScroll,  FALSE },
				{ SA_Interleaved, FALSE },
				{ SA_SharePens,   TRUE },
				{ SA_ErrorCode,   (ULONG)&perr },
				{ TAG_DONE, 0 }
			};

			pnl_scr = OpenScreenTagList(0, pa);
		}

		pnl_shadow = pnl_scr ? (UBYTE *)calloc((size_t)sw * PANEL_H, 1) : 0;

		if(!pnl_scr || !pnl_shadow)
		{
			ZLOG("AGA: no screen for the bottom panel (error %ld) -- the panel stays in the "
			     "scrolled picture\n", (long)perr);
			if(pnl_scr) { CloseScreen(pnl_scr); pnl_scr = 0; }
			free(pnl_shadow);
			pnl_shadow = 0;
			split_b = 0;
			/* the map screen is shorter than the picture now: one more try
			 * is not worth it -- without the panel's rows the game still
			 * runs, the log says why */
		}
		else
		{
			SetRast(&pnl_scr->RastPort, 0);

			/* the map screen's colours, without reloading them between the
			 * two viewports (a smaller gap); they are loaded the same too */
			VideoControlTags(pnl_scr->ViewPort.ColorMap, VC_NoColorPaletteLoad, TRUE, TAG_DONE);
			MakeScreen(pnl_scr);
			RethinkDisplay();

			/* Intuition clips the screen behind at the one in front: if the
			 * map screen lost rows, the panel goes that much lower */
			if(scr->ViewPort.DHeight < split_b)
			{
				const int lost = split_b - scr->ViewPort.DHeight;

				/* FORCEDRAG: the screens are not draggable (SA_Draggable) */
				ScreenPosition(pnl_scr, SPOS_RELATIVE | SPOS_FORCEDRAG, 0, lost, 0, 0);
				split_off = (int)(pnl_scr->TopEdge - scr->TopEdge);
			}

			{
				/* the panel screen reaches below the picture by the gap: the
				 * rows it really shows, and the mode's text overscan */
				struct Rectangle txt;

				txt.MinY = txt.MaxY = -1;
				QueryOverscan(id, &txt, OSCAN_TEXT);

				ZLOG("AGA: the bottom panel on a screen of its own: rows %d..%d of the picture at "
				     "row %d of the display (%d blank rows above it); the map screen shows %d rows, "
				     "the panel screen %d of %d (text overscan rows %d..%d)\n",
				     split_b, sh - 1, (int)pnl_scr->TopEdge, split_off - split_b,
				     (int)scr->ViewPort.DHeight, (int)pnl_scr->ViewPort.DHeight, PANEL_H,
				     (int)txt.MinY, (int)txt.MaxY);
			}

			/* the gap's edges (see dress_buf) */
			if(seam_on < 0) seam_on = zod_env_switch("ZOD_AGA_SEAM", 1);
			dress_x = w - SIDE_W;
			free(dress_buf);
			dress_buf = seam_on ? (UBYTE *)malloc((size_t)DRESS_ROWS * DRESS_BPR) : 0;
			st_dress_frames = 0;

			if(!seam_on)
				ZLOG("AGA: the gap's edges: the picture's own rows (ZOD_AGA_SEAM off)\n");
			else if(!dress_buf)
				ZLOG("AGA: the gap's edges: no memory for their rows (%d bytes) -- the picture's "
				     "own rows\n", DRESS_ROWS * DRESS_BPR);
			else
				ZLOG("AGA: the gap's edges: dressed -- a ledge under the side panel, the bottom "
				     "panel's rail across it (ZOD_AGA_SEAM)\n");
		}
	}

	*dy = (sh - h) / 2;

	if(check_on < 0) check_on = env_on("ZOD_AGA_CHECK");

	/* the block sums: a dirty block whose pixels are what the planes hold
	 * already is not converted again -- on AGA every word saved is a chip
	 * RAM write saved (the RTG path's flip sums, the same sum) */
	if(sums_on < 0)
	{
		sums_on = zod_env_switch("ZOD_AGA_SUMS", 1);
	}

	if(dbuf_on < 0)
	{
		dbuf_on = zod_env_switch("ZOD_AGA_DBUF", 1);

		/* -H fast: hardware scrolling into the shown bitmap */
		if(zod_aga_hw_option == 2) dbuf_on = 0;
	}

	if(together_on < 0) together_on = zod_env_switch("ZOD_AGA_TOGETHER", 1);

	dbuf = 0;

	if(dbuf_on)
	{
		safe_port = CreateMsgPort();
		disp_port = CreateMsgPort();

		/* hardware scrolling: the screen has our bitmap (SA_BitMap), and
		 * for such a screen the first buffer is made from that bitmap --
		 * SB_SCREEN_BITMAP is only for a bitmap Intuition allocated (the
		 * AllocScreenBuffer autodoc). With SB_SCREEN_BITMAP here every
		 * soft-scrolling run since the own bitmaps (21.11. ~08:20) fell
		 * back to one buffer. The second large bitmap is our own too (a
		 * copy of the screen's would only have the screen's size). */
		if(hw)
		{
			sbuf[0] = AllocScreenBuffer(scr, hw_bm[0], 0);
			hw_bm[1] = AllocBitMap((ULONG)hw_bw, (ULONG)hw_bh, 8, BMF_DISPLAYABLE | BMF_CLEAR, hw_bm[0]);
			sbuf[1] = hw_bm[1] ? AllocScreenBuffer(scr, hw_bm[1], 0) : 0;
		}
		else
		{
			sbuf[0] = AllocScreenBuffer(scr, 0, SB_SCREEN_BITMAP);
			sbuf[1] = AllocScreenBuffer(scr, 0, SB_COPY_BITMAP);
		}

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
			ZLOG("AGA: no second buffer -- one buffer (message ports %s, the screen's buffer %s, "
			     "the second bitmap %s, its buffer %s)\n",
			     safe_port && disp_port ? "ok" : "FAILED", sbuf[0] ? "ok" : "FAILED",
			     !hw ? "--" : hw_bm[1] ? "ok" : "FAILED (chip RAM)", sbuf[1] ? "ok" : "FAILED");
			if(sbuf[1]) FreeScreenBuffer(scr, sbuf[1]);
			if(sbuf[0]) FreeScreenBuffer(scr, sbuf[0]);
			sbuf[0] = sbuf[1] = 0;
			if(hw_bm[1]) { FreeBitMap(hw_bm[1]); hw_bm[1] = 0; }
			if(safe_port) DeleteMsgPort(safe_port);
			if(disp_port) DeleteMsgPort(disp_port);
			safe_port = disp_port = 0;
		}
	}

	if(hw && dbuf)
	{
		hw_shadow[1] = (UBYTE *)malloc((size_t)hw_bw * hw_bh);

		if(hw_shadow[1])
			memcpy(hw_shadow[1], hw_shadow[0], (size_t)hw_bw * hw_bh);
		else
		{
			ZLOG("AGA: no memory for the second scrolling shadow -- one buffer\n");
			buffers_free(scr);
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

	ZLOG("AGA: the display window %dx%d at row %d%s\n",
	     (int)scr->ViewPort.DWidth, (int)scr->ViewPort.DHeight, (int)scr->TopEdge,
	     mi == 3 ? " (exclusive)" : "");

	if(hw)
		ZLOG("AGA: hardware scrolling (ZOD_AGA_HWSCROLL): a %dx%d bitmap (%ld KB of chip RAM a buffer), "
		     "margins %d px sideways and %d rows, ScrollVPort over it; the shadow %ld KB a buffer\n",
		     hw_bw, hw_bh, (long)hw_bw * hw_bh / 1024, hw_mx, hw_my, (long)hw_bw * hw_bh / 1024);

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

/* one 16-pixel group as a word of each plane */
static inline void c2p_word_out(const UBYTE *row, UBYTE *const *pl, long off, int g)
{
	unsigned short w[8];
	int p;

	c2p_16((const c2p_u32 *)(row + g * 16), w);
	for(p = 0; p < 8; p++) *(UWORD *)(pl[p] + off + g * 2) = w[p];
}

/* rows of 16-pixel groups g0..g1 from the source rows into the planes:
 * groups in pairs that start on an even group as one longword of each
 * plane (the plane rows are 64-bit aligned), a group alone at either end
 * as words; ZOD_AGA_C2P32=off writes words only */
static void c2p_rows(const UBYTE *src, int spitch, UBYTE *const *pl, long bpr,
                     int prow, int rows, int g0, int g1)
{
	int r;

	for(r = 0; r < rows; r++)
	{
		const UBYTE *row = src + (long)r * spitch;
		const long off = (long)(prow + r) * bpr;
		int g = g0;

		if(c2p32_on)
		{
			if(g & 1) { c2p_word_out(row, pl, off, g); g++; }

			for(; g < g1; g += 2)
			{
				c2p_u32 l[8];

				c2p_32((const c2p_u32 *)(row + g * 16), l);
				*(ULONG *)(pl[7] + off + g * 2) = l[7];
				*(ULONG *)(pl[6] + off + g * 2) = l[6];
				*(ULONG *)(pl[5] + off + g * 2) = l[5];
				*(ULONG *)(pl[4] + off + g * 2) = l[4];
				*(ULONG *)(pl[3] + off + g * 2) = l[3];
				*(ULONG *)(pl[2] + off + g * 2) = l[2];
				*(ULONG *)(pl[1] + off + g * 2) = l[1];
				*(ULONG *)(pl[0] + off + g * 2) = l[0];
			}
		}

		for(; g <= g1; g++) c2p_word_out(row, pl, off, g);
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

/* block k (16x16 at gx, gy) of the compose surface: does buffer b need it
 * (its pixels are not what the buffer holds already)? The sums and the
 * other buffer's stale mark are brought up to date as if it were written;
 * the caller writes the blocks of a row together (need_out). */
static int block_need(const SDL_Surface *cs, int b, int gx, int gy, int rows)
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

	return 1;
}

/* the needed groups of block row gy into the planes, a run at a time (so
 * two neighbours that are both needed go as longwords) */
static unsigned long need_out(const SDL_Surface *cs, UBYTE *const *pl, long bpr, int dy,
                              int gy, int rows, const UBYTE *need)
{
	const UBYTE *src = (const UBYTE *)cs->pixels + (long)gy * 16 * cs->pitch;
	unsigned long bytes = 0;
	int g = 0;

	while(g < sums_gw)
	{
		int e;

		if(!need[g]) { g++; continue; }

		for(e = g; e + 1 < sums_gw && need[e + 1]; e++) ;

		c2p_rows(src, cs->pitch, pl, bpr, gy * 16 + dy, rows, g, e);
		st_groups += (unsigned long)(e - g + 1) * rows;
		bytes += 16UL * rows * (e - g + 1);
		g = e + 1;
	}

	return bytes;
}

/* 16 bytes of the picture (s, any alignment) against 16 of the shadow (d,
 * aligned); copied when they differ; 1 = they differed */
static inline int group_take(const UBYTE *s, UBYTE *d)
{
	ULONG a0, a1, a2, a3;
	ULONG *q = (ULONG *)d;

	/* memcpy of 4: one move.l on the 020+ (no alignment needed there) */
	memcpy(&a0, s, 4); memcpy(&a1, s + 4, 4); memcpy(&a2, s + 8, 4); memcpy(&a3, s + 12, 4);

	if(a0 == q[0] && a1 == q[1] && a2 == q[2] && a3 == q[3]) return 0;

	q[0] = a0; q[1] = a1; q[2] = a2; q[3] = a3;
	return 1;
}

/* the same for a part of a group (the window's edges); n bytes */
static int part_take(const UBYTE *s, UBYTE *d, int n)
{
	int i, differ = 0;

	for(i = 0; i < n; i++)
		if(d[i] != s[i]) { d[i] = s[i]; differ = 1; }

	return differ;
}

/* the gap's edges this frame (see dress_buf): the side column's rows next
 * to the gap as they are shown */
static void dress_build(const SDL_Surface *cs)
{
	const UBYTE *const pic = (const UBYTE *)cs->pixels;
	const long pitch = cs->pitch;
	const int gap = split_b, x = dress_x;
	int r;

	for(r = 0; r < DRESS_ROWS; r++)
	{
		const int y = gap - DRESS_ABOVE + r;
		const UBYTE *const row = pic + (long)y * pitch;
		UBYTE *const d = dress_buf + (long)r * DRESS_BPR;
		UBYTE *const side = d + 16;

		/* the 16 columns left of the side column, as they are */
		memcpy(d, row + x - 16, 16);

		if(y < gap - LEDGE_H)
		{
			memcpy(side, row + x, PLATE_X);
			memcpy(side + PLATE_X, pic + (long)(gap + plate_src[r]) * pitch + x + PLATE_X,
			       SIDE_W - PLATE_X);
		}
		else if(y < gap)
		{
			/* the ledge: the shelf's rows, its 6 columns of the frame left
			 * of it as its first */
			const UBYTE *const shelf = pic + (long)(gap - SHELF_ROW + y - (gap - LEDGE_H)) * pitch + x;

			memset(side, shelf[6], 6);
			memcpy(side + 6, shelf + 6, SIDE_W - 6);
		}
		else if(y < gap + RAIL_H)
			memcpy(side, row + x - RAIL_X, SIDE_W);
		else
		{
			/* 8 rows lower, the frame's 4 columns on the left stay; the
			 * tiles' remains (the 3 rows below the gap) are replaced by
			 * what is 8 rows below them, plain plate */
			const UBYTE *const up = row - RAIL_H * pitch;

			memcpy(side, row + x, 4);
			if(y < gap + RAIL_H + 3)
			{
				memcpy(side + 4, up + x + 4, PLATE_X - 4);
				memcpy(side + PLATE_X, row + x + PLATE_X, SIDE_W - PLATE_X);
			}
			else
				memcpy(side + 4, up + x + 4, SIDE_W - 4);
		}
	}
}

/* picture row y as shown from 16 columns left of the side column on, or 0
 * where the picture's own row is shown */
static const UBYTE *dress_row(int y)
{
	const int r = y - (split_b - DRESS_ABOVE);

	if(!dress_now || r < 0 || r >= DRESS_ROWS) return 0;

	return dress_buf + (long)r * DRESS_BPR;
}

/* where the display window lies in the bitmap this frame */
static void hw_place(void)
{
	if(!view_set) return;       /* no map drawn: the window stays */

	view_set = 0;

	if(hw_anchored)
	{
		const int rx = view_sx - hw_ox, ry = view_sy - hw_oy;
		const int xmax = hw_bw - hw_dw, ymax = hw_bh - hw_dh;
		int nrx = rx, nry = ry;

		if(rx >= 0 && ry >= 0 && rx <= xmax && ry <= ymax)
		{
			hw_rx = rx;
			hw_ry = ry;
			return;
		}

		/* anchored again with the view near the edge it came from: the
		 * whole width of both margins lies ahead in the direction it moves
		 * (a quarter of a margin kept behind, for a small step back) */
		if(rx > xmax) nrx = hw_mx / 4;
		else if(rx < 0) nrx = xmax - hw_mx / 4;
		if(ry > ymax) nry = hw_my / 4;
		else if(ry < 0) nry = ymax - hw_my / 4;

		hw_ox = view_sx - nrx;
		hw_oy = view_sy - nry;
		hw_rx = nrx;
		hw_ry = nry;
		st_hw_anchors++;
		return;
	}

	/* the first anchor: the view in the middle */
	hw_ox = view_sx - hw_mx;
	hw_oy = view_sy - hw_my;
	hw_rx = hw_mx;
	hw_ry = hw_my;
	hw_anchored = 1;
}

/* ZOD_AGA_CHECK with hardware scrolling: the shadow's window against the
 * picture, and every group of the planes under the window against the
 * shadow (read back through the transpose) */
static unsigned long check_hw(const SDL_Surface *cs, UBYTE *const *pl, int bpr, int dy, const UBYTE *sh)
{
	unsigned long differ = 0;
	int yd, g, i;

	for(yd = 0; yd < hw_dh; yd++)
	{
		const int y = yd - dy;
		const UBYTE *s = (y >= 0 && y < cs->h) ? (const UBYTE *)cs->pixels + (long)y * cs->pitch : hw_zero;
		const UBYTE *const ds = dress_row(y);
		const long brow = (long)(yd + hw_ry);
		const UBYTE *srow = sh + brow * hw_bw;

		for(i = 0; i < hw_dw; i++)
			if(srow[hw_rx + i] != ((ds && i >= dress_x) ? ds[i - dress_x + 16] : s[i])) differ++;

		for(g = hw_rx >> 4; g <= (hw_rx + hw_dw - 1) >> 4; g++)
		{
			UWORD wd[8];
			ULONG a, b, c, d;
			UBYTE px[16];
			int p;

			for(p = 0; p < 8; p++) wd[p] = *(const UWORD *)(pl[p] + brow * bpr + g * 2);

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
				if(px[i] != srow[g * 16 + i]) differ++;
		}
	}

	return differ;
}

/* the bottom panel's rows into its own screen: compared into its shadow,
 * only differing groups (longword pairs) converted -- it never moves */
static unsigned long panel_out(const SDL_Surface *cs)
{
	struct BitMap *bm = pnl_scr->RastPort.BitMap;
	const long bpr = bm->BytesPerRow;
	const int ng = hw_dw / 16;
	UBYTE *pl[8];
	unsigned long bytes = 0;
	int p, r;

	for(p = 0; p < 8; p++) pl[p] = (UBYTE *)bm->Planes[p];

	for(r = 0; r < PANEL_H; r++)
	{
		const int y = split_b + r;
		const UBYTE *s = (y < cs->h) ? (const UBYTE *)cs->pixels + (long)y * cs->pitch : hw_zero;
		const UBYTE *const ds = dress_row(y);
		const int gs = ds ? dress_x >> 4 : ng;      /* the first group from ds */
		UBYTE *const srow = pnl_shadow + (long)r * hw_dw;
		int g, run = -1;

		for(g = 0; g <= ng; g++)
		{
			const int differ = (g < gs) ? group_take(s + g * 16, srow + g * 16) :
			                   (g < ng) ? group_take(ds + g * 16 - (dress_x - 16), srow + g * 16) : 0;

			if(differ)
			{
				if(run < 0) run = g;
			}
			else if(run >= 0)
			{
				int ga = run, gb = g - 1;

				if(c2p32_on) { ga &= ~1; gb |= 1; }   /* the shadow holds the partner */

				c2p_rows(srow, hw_dw, pl, bpr, r, 1, ga, gb);
				st_pnl_groups += (unsigned long)(gb - ga + 1);
				bytes += 16UL * (gb - ga + 1);
				run = -1;
			}
		}
	}

	st_pnl_frames++;
	return bytes;
}

/* ZOD_AGA_CHECK for the panel screen: its shadow against the picture's
 * panel rows, its planes against its shadow */
static unsigned long check_panel(const SDL_Surface *cs)
{
	struct BitMap *bm = pnl_scr->RastPort.BitMap;
	const long bpr = bm->BytesPerRow;
	unsigned long differ = 0;
	int r, g, i, p;

	for(r = 0; r < PANEL_H; r++)
	{
		const int y = split_b + r;
		const UBYTE *s = (y < cs->h) ? (const UBYTE *)cs->pixels + (long)y * cs->pitch : hw_zero;
		const UBYTE *const ds = dress_row(y);
		const UBYTE *srow = pnl_shadow + (long)r * hw_dw;

		for(i = 0; i < hw_dw; i++)
			if(srow[i] != ((ds && i >= dress_x) ? ds[i - dress_x + 16] : s[i])) differ++;

		for(g = 0; g < hw_dw / 16; g++)
		{
			UWORD wd[8];
			ULONG a, b, c, d;
			UBYTE px[16];

			for(p = 0; p < 8; p++) wd[p] = *(const UWORD *)((UBYTE *)bm->Planes[p] + (long)r * bpr + g * 2);

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
				if(px[i] != srow[g * 16 + i]) differ++;
		}
	}

	return differ;
}

/* ZOD_SHOTOUT (zod_aga_shot_request) */
static char shot_name[128];
static unsigned long shot_frame = 0;

void zod_aga_shot_request(const char *name, unsigned long frame)
{
	strncpy(shot_name, name, sizeof(shot_name) - 1);
	shot_name[sizeof(shot_name) - 1] = 0;
	shot_frame = frame ? frame : 1;
}

/* 16 pixels of a bitmap row (the word group g) from its 8 planes */
static void planes_group(UBYTE *const *pl, long bpr, long row, int g, UBYTE *px)
{
	UWORD wd[8];
	ULONG a, b, c, d;
	int p, i;

	for(p = 0; p < 8; p++) wd[p] = *(const UWORD *)(pl[p] + row * bpr + g * 2);

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
}

/* the display of this frame (hardware scrolling: the window in the buffer
 * just written, pl) into shot_name */
static void shot_write(const SDL_Surface *cs, UBYTE *const *pl, long bpr)
{
	const int gap = pnl_scr ? split_off - split_b : 0;
	const int hh = hw_dh + (pnl_scr ? gap + PANEL_H : 0);
	UBYTE *const img = (UBYTE *)calloc((size_t)hw_dw * hh, 1);
	SDL_Surface *bild;
	UBYTE px[16];
	int y, g, k, ok = 0;

	if(img)
	{
		for(y = 0; y < hw_dh; y++)
			for(g = hw_rx >> 4; g <= (hw_rx + hw_dw - 1) >> 4; g++)
			{
				planes_group(pl, bpr, (long)(y + hw_ry), g, px);

				for(k = 0; k < 16; k++)
				{
					const int col = g * 16 + k - hw_rx;

					if(col >= 0 && col < hw_dw) img[(long)y * hw_dw + col] = px[k];
				}
			}

		if(pnl_scr)
		{
			struct BitMap *bm = pnl_scr->RastPort.BitMap;
			UBYTE *ppl[8];
			int p;

			for(p = 0; p < 8; p++) ppl[p] = (UBYTE *)bm->Planes[p];

			for(y = 0; y < PANEL_H; y++)
				for(g = 0; g < hw_dw / 16; g++)
					planes_group(ppl, bm->BytesPerRow, (long)y, g,
					             img + (long)(hw_dh + gap + y) * hw_dw + g * 16);
		}

		bild = SDL_CreateRGBSurfaceFrom(img, hw_dw, hh, 8, hw_dw, 0, 0, 0, 0);

		if(bild)
		{
			if(bild->format->palette && cs->format->palette)
				memcpy(bild->format->palette->colors, cs->format->palette->colors,
				       sizeof(SDL_Color) * 256);
			ok = SDL_SaveBMP(bild, shot_name) == 0;
			SDL_FreeSurface(bild);
		}

		free(img);
	}

	ZLOG("AGA: the display read back at frame %lu (%dx%d: %d rows of the map screen, %d blank, "
	     "%d of the panel screen) into '%s'%s\n",
	     shot_frame, hw_dw, hh, hw_dh, gap, pnl_scr ? PANEL_H : 0, shot_name,
	     ok ? "" : " -- FAILED");
}

/* the panel screen's window: on the main window's message port, so its
 * clicks and keys arrive where the game reads them */
void zod_aga_panel_window_open(struct Window *main, UWORD *pointer)
{
	if(!pnl_scr || pnl_win || !main) return;

	{
		struct TagItem wa[] = {
			{ WA_CustomScreen, (ULONG)pnl_scr },
			{ WA_Left, 0 }, { WA_Top, 0 },
			{ WA_Width, (ULONG)pnl_scr->Width }, { WA_Height, (ULONG)PANEL_H },
			{ WA_Flags, WFLG_BACKDROP | WFLG_BORDERLESS | WFLG_RMBTRAP |
			            WFLG_REPORTMOUSE | WFLG_NOCAREREFRESH },
			{ WA_IDCMP, 0 },
			{ TAG_DONE, 0 }
		};

		pnl_win = OpenWindowTagList(0, wa);
	}

	if(!pnl_win)
	{
		ZLOG("AGA: no window on the panel screen -- clicks on the panel do not reach the game\n");
		return;
	}

	pnl_win->UserPort = main->UserPort;
	ModifyIDCMP(pnl_win, main->IDCMPFlags);
	if(pointer) SetPointer(pnl_win, pointer, 1, 16, 0, 0);
}

/* before the main window closes (the port is its): the panel window's
 * messages taken off the shared port, then the window closed */
void zod_aga_panel_window_close(void)
{
	if(!pnl_win) return;

	Forbid();

	if(pnl_win->UserPort)
	{
		struct Node *n = pnl_win->UserPort->mp_MsgList.lh_Head, *next;

		while((next = n->ln_Succ))
		{
			if(((struct IntuiMessage *)n)->IDCMPWindow == pnl_win)
			{
				Remove(n);
				ReplyMsg((struct Message *)n);
			}

			n = next;
		}
	}

	pnl_win->UserPort = 0;
	ModifyIDCMP(pnl_win, 0);
	Permit();

	ClearPointer(pnl_win);
	CloseWindow(pnl_win);
	pnl_win = 0;
}

struct Window *zod_aga_panel_window(void) { return pnl_win; }

void zod_aga_panel_pointer(UWORD *pointer, int on)
{
	if(!pnl_win) return;

	if(on) ClearPointer(pnl_win);
	else if(pointer) SetPointer(pnl_win, pointer, 1, 16, 0, 0);
}

void zod_aga_panel_palette(ULONG *table)
{
	if(pnl_scr) LoadRGB32(&pnl_scr->ViewPort, table);
}

int zod_aga_split_row(void) { return pnl_scr ? split_b : 0; }

/* The pointer on the picture. The panel screen is in front of the map
 * screen (its child), and Intuition keeps the front screen's mouse position
 * current wherever the pointer is (-187 above it, measured on voltago),
 * while the map screen's own stops at its last value inside it once the
 * pointer is on the panel. So only the panel's is read: at or below its
 * top the panel's rows, above it the map screen's row (the same
 * resolution), the blank rows between counting as the map's last row. */
void zod_aga_mouse(struct Screen *map, int *x, int *y)
{
	int py;

	if(!pnl_scr)
	{
		*x = map->MouseX;
		*y = map->MouseY;
		return;
	}

	*x = pnl_scr->MouseX;
	py = pnl_scr->MouseY;

	if(py >= 0)
		*y = split_b + (py < PANEL_H ? py : PANEL_H - 1);
	else
	{
		const int my = py + split_off;   /* the row on the map screen */

		*y = (my >= split_b) ? split_b - 1 : my;
	}
}

/* a picture row as the map screen's mouse row (for warping the pointer) */
int zod_aga_raw_y(int y)
{
	if(!pnl_scr || y < split_b) return y;

	return y - split_b + split_off;
}

/* ZOD_AGA_CHECK: the map ViewPort's first plane pointer and BPLCON1 as its
 * long-frame copper list loads them -- what the display takes from the
 * next frame on (read only) */
static int copper_read(const struct ViewPort *vp, ULONG *pt, UWORD *con1)
{
	const UWORD *c;
	int i, have = 0;

	*pt = 0;
	*con1 = 0;

	if(!vp->DspIns || !vp->DspIns->CopLStart) return 0;

	c = vp->DspIns->CopLStart;

	/* the viewport's 256 colours come first (two MOVEs each and the
	 * bank switches): the plane pointers lie past 512 instructions */
	for(i = 0; i < 4096 && (have & 3) != 3; i++, c += 2)
	{
		if(c[0] == 0xFFFF && c[1] == 0xFFFE) break;     /* the list's end */
		if(c[0] & 1) continue;                          /* WAIT or SKIP */

		switch(c[0] & 0x1FE)
		{
		case 0x102: *con1 = c[1]; have |= 4; break;                  /* BPLCON1 */
		case 0x0E0: *pt = (*pt & 0xFFFFUL) | ((ULONG)c[1] << 16); have |= 1; break;
		case 0x0E2: *pt = (*pt & 0xFFFF0000UL) | c[1]; have |= 2; break;
		}
	}

	return (have & 3) == 3;
}

/* the frame with hardware scrolling: the window placed, the picture
 * compared into the buffer's shadow there, the groups that differ through
 * c2p, the buffer shown at the window's place */
static void flip_hw(SDL_Surface *cs, struct Screen *scr, int dy)
{
	struct BitMap *bm;
	struct ViewPort *vp = &scr->ViewPort;
	UBYTE *pl[8];
	const int w = cs->w, h = cs->h;
	const int b = dbuf ? cur : 0;
	UBYTE *const sh = hw_shadow[b];
	unsigned long t0, bytes = 0;
	long bpr;
	int p, yd, moved;

	if(dbuf && !safe_ok)
	{
		const unsigned long w0 = zod_fineclock_ticks();

		while(!GetMsg(safe_port)) { WaitPort(safe_port); st_safe_waits++; }
		safe_ok = 1;
		st_wait_ticks += zod_fineclock_ticks() - w0;
	}

	t0 = zod_fineclock_ticks();

	/* the gap's edges: in frames that drew the map (hw_place takes the
	 * view's mark) */
	dress_now = view_set && dress_buf && pnl_scr;
	if(dress_now)
	{
		dress_build(cs);
		st_dress_frames++;
	}

	hw_place();

	bm = dbuf ? sbuf[b]->sb_BitMap : scr->RastPort.BitMap;
	bpr = bm->BytesPerRow;
	for(p = 0; p < 8; p++) pl[p] = (UBYTE *)bm->Planes[p];

	for(yd = 0; yd < hw_dh; yd++)
	{
		const int y = yd - dy;
		/* the rows beside a smaller picture: colour 0 */
		const UBYTE *s = (y >= 0 && y < h) ? (const UBYTE *)cs->pixels + (long)y * cs->pitch : hw_zero;
		const UBYTE *const ds = dress_row(y);
		const long brow = (long)(yd + hw_ry);
		UBYTE *const srow = sh + brow * hw_bw;
		const int x0 = hw_rx, x1 = hw_rx + w;     /* the window's columns */
		const int g0 = x0 >> 4, g1 = (x1 - 1) >> 4;
		/* from this group on the row comes from ds, which begins 16
		 * columns left of the side column: a group reaching into the side
		 * column lies wholly in it */
		const int gs = ds ? (x0 + dress_x) >> 4 : g1 + 1;
		int g, run = -1;

		for(g = g0; g <= g1 + 1; g++)
		{
			int differ = 0;

			if(g <= g1)
			{
				const int bx = g * 16;
				const UBYTE *const from = (g < gs) ? s : ds;
				const int fx = (g < gs) ? x0 : x0 + dress_x - 16;   /* from's column 0 */

				if(bx >= x0 && bx + 16 <= x1)
					differ = group_take(from + (bx - fx), srow + bx);
				else
				{
					const int a = bx < x0 ? x0 : bx;
					const int e = bx + 16 > x1 ? x1 : bx + 16;

					differ = part_take(from + (a - fx), srow + a, e - a);
				}
			}

			if(differ)
			{
				if(run < 0) run = g;
			}
			else if(run >= 0)
			{
				int ga = run, gb = g - 1;

				/* longwords: the pair partners go along from the shadow,
				 * which holds exactly what the planes hold (the bitmap's
				 * row is a multiple of 64 pixels: the partner exists) */
				if(c2p32_on) { ga &= ~1; gb |= 1; }

				c2p_rows(srow, hw_bw, pl, bpr, (int)brow, 1, ga, gb);
				regions_count((int)brow, ga, gb);
				st_groups += (unsigned long)(gb - ga + 1);
				bytes += 16UL * (gb - ga + 1);
				run = -1;
			}
		}
	}

	if(pnl_scr) bytes += panel_out(cs);

	zod_dirty_bytes += bytes;
	zod_dirty_frames++;
	zod_dirty_sweep();
	zod_dirty_clear();

	st_ticks += zod_fineclock_ticks() - t0;

	if(check_on > 0)
	{
		st_check_differ += check_hw(cs, pl, bpr, dy, sh);
		if(pnl_scr) st_check_differ += check_panel(cs);
		st_check_frames++;
	}

	if(shot_name[0] && zod_dirty_frames == shot_frame)
	{
		shot_write(cs, pl, bpr);
		shot_name[0] = 0;
	}

	/* offsets someone else reset (a display rebuilt by Intuition) count
	 * as a move: ScrollVPort again */
	if(vp->RasInfo->RxOffset != hw_shown_rx || vp->RasInfo->RyOffset != hw_shown_ry)
		hw_shown_rx = hw_shown_ry = -1;

	moved = hw_rx != hw_shown_rx || hw_ry != hw_shown_ry;
	vp->RasInfo->RxOffset = (WORD)hw_rx;
	vp->RasInfo->RyOffset = (WORD)hw_ry;

	if(dbuf)
	{
		ULONG p0 = 0, p1 = 0, p2 = 0;
		UWORD c0 = 0, c1 = 0, c2 = 0;
		int cop;

		if(!disp_ok)
		{
			while(!GetMsg(disp_port)) WaitPort(disp_port);
			disp_ok = 1;
		}

		cop = check_on > 0 && moved && copper_read(vp, &p0, &c0);

		if(moved && together_on)
		{
			struct BitMap *const was = vp->RasInfo->BitMap;

			/* the new buffer at the new place, one ScrollVPort */
			vp->RasInfo->BitMap = sbuf[b]->sb_BitMap;
			ScrollVPort(vp);
			if(cop) copper_read(vp, &p1, &c1);

			if(ChangeScreenBuffer(scr, sbuf[b]))
			{
				safe_ok = 0;
				disp_ok = 0;
				cur = b ^ 1;
			}
			else
			{
				/* Intuition busy (menus, gadgets): its buffer back, at
				 * the new place; this frame goes into b again */
				vp->RasInfo->BitMap = was;
				ScrollVPort(vp);
				st_change_failed++;
			}
		}
		else
		{
			if(ChangeScreenBuffer(scr, sbuf[b]))
			{
				safe_ok = 0;
				disp_ok = 0;
				cur = b ^ 1;
			}
			else
				st_change_failed++;

			if(cop) copper_read(vp, &p1, &c1);
			if(moved) ScrollVPort(vp);
		}

		if(cop && copper_read(vp, &p2, &c2))
		{
			st_cop_checked++;
			if(!(p1 == p0 && c1 == c0) && !(p1 == p2 && c1 == c2)) st_cop_mixed++;
		}
	}
	else if(moved)
		ScrollVPort(vp);

	if(moved)
	{
		hw_shown_rx = hw_rx;
		hw_shown_ry = hw_ry;
		st_hw_moved++;
	}

	st_hw_scrolls++;
	if(regions_on > 0) reg_frames++;
	st_frames++;
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

	colors_count(cs);

	if(hw) { flip_hw(cs, scr, dy); return; }

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
		UBYTE rowneed[64];

		memset(rowneed, 0, sizeof(rowneed));

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
					if(block_need(cs, b, g, y0 >> 4, rows)) rowneed[g] = 1;
				}

				continue;
			}

			c2p_rows((const UBYTE *)cs->pixels + (long)y0 * cs->pitch, cs->pitch, pl,
			         bm->BytesPerRow, y0 + dy, rows, x0 >> 4, x1 >> 4);

			st_groups += (unsigned long)((x1 >> 4) - (x0 >> 4) + 1) * rows;
			bytes += (unsigned long)((x1 >> 4) - (x0 >> 4) + 1) * 16 * rows;
		}

		if(block == 16 && (sums[b] || dbuf))
			bytes += need_out(cs, pl, bm->BytesPerRow, dy, y0 >> 4, rows, rowneed);
	}

	/* the blocks the other buffer got last time and this one has not */
	if(dbuf && stale[b])
	{
		int gy, gx;

		for(gy = 0; gy < sums_gh; gy++)
		{
			const int rows = (h - gy * 16 < 16) ? h - gy * 16 : 16;
			UBYTE rowneed[64];
			int any = 0;

			memset(rowneed, 0, sizeof(rowneed));

			for(gx = 0; gx < sums_gw; gx++)
			{
				const long k = (long)gy * sums_gw + gx;

				if(!stale[b][k]) continue;

				stale[b][k] = 0;
				st_stale++;

				if(block_need(cs, b, gx, gy, rows)) { rowneed[gx] = 1; any = 1; }
			}

			if(any) bytes += need_out(cs, pl, bm->BytesPerRow, dy, gy, rows, rowneed);
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

/* after CloseScreen: the large bitmap the screen used (SA_BitMap) */
void zod_aga_closed(void)
{
	if(hw_bm[0]) { WaitBlit(); FreeBitMap(hw_bm[0]); hw_bm[0] = 0; }
}

void zod_aga_close(struct Screen *scr)
{
	int i;

	zod_aga_panel_window_close();     /* normally closed already, before the main window */

	if(pnl_scr) { CloseScreen(pnl_scr); pnl_scr = 0; }
	free(pnl_shadow);
	pnl_shadow = 0;
	free(dress_buf);
	dress_buf = 0;
	dress_now = 0;
	split_b = 0;

	buffers_free(scr);

	for(i = 0; i < NBUF; i++)
	{
		free(sums[i]); free(sums_valid[i]); free(stale[i]);
		sums[i] = 0; sums_valid[i] = 0; stale[i] = 0;
		free(hw_shadow[i]);
		hw_shadow[i] = 0;
	}

	free(hw_zero);
	hw_zero = 0;
	hw = 0;
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

	if(check_on > 0 && st_cop_checked)
		ZLOG("AGA buffer changes that moved the window (%s, ZOD_AGA_TOGETHER): copper list read "
		     "in %lu, between the calls a buffer at the other's place in %lu\n",
		     together_on ? "one ScrollVPort" : "ChangeScreenBuffer, then ScrollVPort",
		     st_cop_checked, st_cop_mixed);

	if(regions_on > 0 && reg_frames)
	{
		const unsigned long t = reg_groups[COL_VIEW] + reg_groups[COL_SIDE] + reg_groups[COL_BOTTOM];

		ZLOG("AGA soft scrolling, groups written by part (ZOD_AGA_REGIONS), %lu frames: "
		     "map view %lu a frame (%lu %%), side panel %lu (%lu %%), bottom panel %lu (%lu %%)\n",
		     reg_frames, reg_groups[COL_VIEW] / reg_frames, t ? reg_groups[COL_VIEW] * 100 / t : 0UL,
		     reg_groups[COL_SIDE] / reg_frames, t ? reg_groups[COL_SIDE] * 100 / t : 0UL,
		     reg_groups[COL_BOTTOM] / reg_frames, t ? reg_groups[COL_BOTTOM] * 100 / t : 0UL);
	}

	if(col_frames)
	{
		ZLOG("AGA colours (ZOD_AGA_COLORS), %lu frames counted, map view %dx%d at %d,%d:\n",
		     col_frames, view_w, view_h, view_rx, view_ry);
		colors_line("map view (terrain, units, effects)", col_hist[COL_VIEW]);
		colors_line("side panel", col_hist[COL_SIDE]);
		colors_line("bottom panel", col_hist[COL_BOTTOM]);
	}

	if(st_pnl_frames)
		ZLOG("AGA bottom panel screen: %lu frames, %lu groups a frame written; the gap's "
		     "edges dressed in %lu frames\n",
		     st_pnl_frames, st_pnl_groups / st_pnl_frames, st_dress_frames);

	if(st_hw_scrolls)
		ZLOG("AGA hardware scrolling: %lu frames, the window moved in %lu, anchored again %lu times "
		     "(%dx%d bitmap)\n", st_hw_scrolls, st_hw_moved, st_hw_anchors, hw_bw, hw_bh);
}
