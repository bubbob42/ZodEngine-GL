/*
 * Drawing through minigl.library -- see zod_gl.h.
 *
 * Plain C style (no STL, no exceptions), as the rest of the port.
 */
#include "zod_gl.h"

#include <SDL/SDL_opengl.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "zod_log.h"

int zod_gl_active = 0;

enum
{
	TILE_MAX    = 1024,   /* largest tile edge: every library has 1024 */
	BATCH_QUADS = 1000,   /* 4000 vertices: below MiniGL's vertex buffer (4096) */
	MAP_TILE    = 128,    /* the map texture's tile: 32 KB at 16 bit */
	MAP_MIN_PX  = 1024 * 1024, /* surfaces larger than this get a map texture */
	ATLAS_W     = 1024,
	ATLAS_PAGES = 4,
	ATLAS_SIDE  = 256,    /* larger images keep a texture of their own */
	ATLAS_SHELVES = 512
};

/* ---- the map texture -------------------------------------------------------
 * The map surface (1024x1376 to 4096x4096) does not fit the texture heap
 * (16 MB on RadeonGL, no eviction). Instead one opaque texture of the
 * screen's size plus a tile on every side holds the part around the view,
 * in MAP_TILE squares: map tile (x, y) lives in slot (x mod nx, y mod ny),
 * so a scrolling view only ever needs the tiles at its edge. A tile is sent
 * when a draw needs it; one more of the ring around the view goes up each
 * frame ahead of time. A crater or zone marker baked into the map marks the
 * tiles it touches stale. */
struct torus
{
	GLuint name;
	int W, H;                 /* texture size, powers of two */
	int nx, ny;               /* slots across and down */
	int mtx, mty;             /* map tiles across and down */
	int *slot_tile;           /* nx * ny: the map tile in the slot, -1 none */
	unsigned char *stale;     /* nx * ny: the map changed under the slot */
};

struct zod_gl_tex
{
	SDL_Surface *s;           /* the surface the texture was made from */
	int w, h;                 /* its size then */
	int tw, th;               /* tile size, powers of two */
	int cols, rows;
	GLuint *names;            /* cols * rows texture names */
	int dx0, dy0, dx1, dy1;   /* pixels to send again; dx1 <= dx0: none */
	int failed;               /* the library refused it: not tried again */
	unsigned long gen;        /* the context it belongs to */
	struct torus *tor;        /* a map texture instead of tiles (MAP_MIN_PX) */
	int atlas;                /* in an atlas page (names[0] is the page):
	                           * 1 a loaded image, 2 a run-time slot */
	int bx, by;               /* where the surface starts in its texture */
	int slot_w, slot_h;       /* a run-time slot's size class */
};

static int screen_w = 0, screen_h = 0;
static unsigned long ctx_gen = 0;

/* Texture sizes the library takes (read at zod_gl_init). RadeonGL stores a
 * non-power-of-two texture as one level, a power-of-two one with its whole
 * mip chain (+33 %) -- so a big surface (the map) goes up at its own size
 * there. Small ones stay powers of two: freed names of a size are reused,
 * and text surfaces of every width would not match. */
static int tile_max = 1024;     /* largest edge */
static int npot_ok = 0;         /* the library takes any size */
static int refused_told = 0;

/* the open batch */
static int in_batch = 0;
static int batch_quads = 0;
static GLuint cur_name = 0;
static int cur_mode = -1;      /* 0 textured, 1 hit flash, 2 fill without texel */
static Uint32 cur_rgba = 0;    /* the last glColor4ub, valid if cur_rgba_ok */
static int cur_rgba_ok = 0;

/* counters (per run; divided by frames in the report) */
static unsigned long st_frames, st_binds, st_batches, st_quads, st_fills;
static unsigned long st_made, st_made_px, st_updates, st_update_px, st_refused;
static unsigned long st_reused, st_map_tiles, st_map_quads;
static unsigned long st_atlas_in, st_atlas_px, st_atlas_resets;
static unsigned long st_bind_own;   /* binds of textures outside atlas and map */
static int bind_is_own = 0;         /* set by the caller before batch_begin */

/* ---- atlas pages -------------------------------------------------------------
 * Small loaded images (zod_gl_hint_atlas) share pages of ATLAS_W x ATLAS_W,
 * packed on their first draw onto shelves of fixed height classes with one
 * transparent pixel between neighbours. Images of different kinds drawn one
 * after another then share a texture and a batch. Page 0 starts with a
 * small white block: fills draw with it and need no switch of texturing.
 * On RadeonGL a page is one texel shorter than a power of two, so it is
 * stored as one level, without the mip chain (+33 %) it would get. When all
 * pages are full, the atlas starts again: its images are packed anew as
 * they are drawn. */
struct atlas_page
{
	GLuint name;
	int h;                    /* height (ATLAS_W, or ATLAS_W - 1) */
	int ycur;                 /* next free row for a new shelf */
};

struct atlas_shelf
{
	short page, y, h, x;      /* x: the next free column */
};

static struct atlas_page pages[ATLAS_PAGES];
static int page_n = 0;
static struct atlas_shelf shelves[ATLAS_SHELVES];
static int shelf_n = 0;
static GLuint white_name = 0;   /* page 0, for fills */
static float white_u = 0.0f, white_v = 0.0f;

/* ---- run-time slots ----------------------------------------------------------
 * Surfaces the engine makes while it runs -- text above all: chat lines,
 * the clock's digits, labels -- would each be a texture of their own and
 * break the batch wherever they are drawn. Up to DYN_W x DYN_H they get a
 * slot of a size class (powers of two from 16 x 8) in the atlas pages
 * instead, theirs while they live. A freed surface's slot waits
 * RETIRE_FRAMES frames, then serves the next surface of its class. */
enum { DYN_W = 256, DYN_H = 64, DYN_FREE = 1024 };

struct dyn_slot
{
	short page, x, y, cw, ch;
	unsigned long frame;      /* when it was retired */
};

static struct dyn_slot dyn_free[DYN_FREE];      /* ready for a new surface */
static int dyn_free_n = 0;
static struct dyn_slot dyn_retired[DYN_FREE];   /* waiting RETIRE_FRAMES */
static int dyn_retired_n = 0;
static unsigned long st_dyn_in, st_dyn_reused;

/* ---- texture names of freed surfaces -------------------------------------
 * A surface freed right after it was drawn -- the HUD renders each clock
 * digit into a surface, blits it and frees it -- must not take its texture
 * along while the quad still waits in the batch or the card still reads it.
 * Its names wait RETIRE_FRAMES frames, then go into a pool by tile size: the
 * next surface of that size refills one with glTexSubImage2D instead of
 * asking for texture memory again (PiStorm3D never gives deleted texture
 * memory back). */
enum { RETIRE_MAX = 1024, RETIRE_FRAMES = 3, POOL_MAX = 256 };

struct old_name
{
	GLuint name;
	short tw, th;
	unsigned long frame;
};

static struct old_name retired[RETIRE_MAX];
static int retired_n = 0;
static struct old_name pool[POOL_MAX];
static int pool_n = 0;
static unsigned long frame_no = 0;

static void batch_end(void);

static void delete_name(GLuint name)
{
	batch_end();
	glDeleteTextures(1, &name);
}

/* ---- surface -> texture ---------------------------------------------------
 * Textures are found by the surface's address: ZSDL_Surface copies share one
 * SDL_Surface, and the SDL layer reports changes and frees by surface. */

enum { REG_SIZE = 8192 };     /* power of two; a frame's working set is ~200 */

#define REG_TOMB ((SDL_Surface *)1)

struct reg_entry
{
	SDL_Surface *s;
	struct zod_gl_tex *t;
	int atlas_ok;             /* a loaded image (zod_gl_hint_atlas) */
};

static struct reg_entry reg[REG_SIZE];
static int reg_full_told = 0;

static unsigned long reg_hash(const SDL_Surface *s)
{
	const unsigned long p = (unsigned long)(size_t)s;

	return ((p >> 4) * 2654435761UL) & (REG_SIZE - 1);
}

static struct reg_entry *reg_find(const SDL_Surface *s)
{
	unsigned long i, n;

	for(i = reg_hash(s), n = 0; n < REG_SIZE; i = (i + 1) & (REG_SIZE - 1), n++)
	{
		if(reg[i].s == s) return &reg[i];
		if(!reg[i].s) return 0;
	}

	return 0;
}

static struct reg_entry *reg_add(SDL_Surface *s)
{
	unsigned long i, n;

	for(i = reg_hash(s), n = 0; n < REG_SIZE; i = (i + 1) & (REG_SIZE - 1), n++)
		if(!reg[i].s || reg[i].s == REG_TOMB)
		{
			reg[i].s = s;
			reg[i].t = 0;
			reg[i].atlas_ok = 0;
			return &reg[i];
		}

	if(!reg_full_told)
	{
		ZLOG("GL: more than %d textured surfaces -- the rest stay undrawn\n", REG_SIZE);
		reg_full_told = 1;
	}

	return 0;
}

static int pot(int n)
{
	int p = 1;

	while(p < n && p < tile_max) p <<= 1;

	return p;
}

static void batch_end(void)
{
	if(in_batch)
	{
		glEnd();
		in_batch = 0;
	}
}

static void batch_begin(GLuint name, int mode)
{
	if(in_batch && mode == cur_mode && batch_quads < BATCH_QUADS &&
	   (mode == 2 || name == cur_name))
		return;

	batch_end();

	if(mode != cur_mode)
	{
		if(mode == 2) glDisable(GL_TEXTURE_2D);
		else if(cur_mode == 2 || cur_mode < 0) glEnable(GL_TEXTURE_2D);

		/* hit flash: colour + texel saturates to white, alpha stays the
		 * texel's -- the white silhouette without a second image */
		if(mode == 1) glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
		else if(cur_mode == 1 || cur_mode < 0)
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

		cur_mode = mode;
	}

	if(mode != 2 && name != cur_name)
	{
		glBindTexture(GL_TEXTURE_2D, name);
		cur_name = name;
		st_binds++;
		if(bind_is_own) st_bind_own++;
	}

	glBegin(GL_QUADS);
	in_batch = 1;
	batch_quads = 0;
	st_batches++;
}

/* the next quad's colour -- inside the batch, sent only when it changes */
static void quad_colour(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	const Uint32 c = ((Uint32)r << 24) | ((Uint32)g << 16) | ((Uint32)b << 8) | a;

	if(cur_rgba_ok && c == cur_rgba) return;

	glColor4ub(r, g, b, a);
	cur_rgba = c;
	cur_rgba_ok = 1;
}

/* ---- textures ------------------------------------------------------------ */

/* The rectangle x0, y0, w, h of s as RGBA bytes into out (rows of
 * out_pitch bytes). The colour key becomes alpha 0 -- only where the
 * surface has SDL_SRCCOLORKEY, as in the software blitter: an opaque 8-bit
 * surface (the map) keeps its index 0. */
static void convert(SDL_Surface *s, int x0, int y0, int w, int h,
                    unsigned char *out, int out_pitch)
{
	const SDL_PixelFormat *f = s->format;
	const int bpp = f->BytesPerPixel;
	const int keyed = (s->flags & SDL_SRCCOLORKEY) != 0;
	const Uint32 key = f->colorkey;
	int x, y;

	if(bpp == 1 && f->palette)
	{
		unsigned char tab[256][4];
		const SDL_Palette *p = f->palette;
		int i;

		for(i = 0; i < 256; i++)
		{
			if(i < p->ncolors)
			{
				tab[i][0] = p->colors[i].r;
				tab[i][1] = p->colors[i].g;
				tab[i][2] = p->colors[i].b;
			}
			else
				tab[i][0] = tab[i][1] = tab[i][2] = 0;

			tab[i][3] = (keyed && (Uint32)i == key) ? 0 : 255;
		}

		for(y = 0; y < h; y++)
		{
			const Uint8 *src = (const Uint8 *)s->pixels + (size_t)(y0 + y) * s->pitch + x0;
			unsigned char *d = out + (size_t)y * out_pitch;

			for(x = 0; x < w; x++, d += 4)
			{
				const unsigned char *c = tab[src[x]];

				d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3];
			}
		}

		return;
	}

	for(y = 0; y < h; y++)
	{
		const Uint8 *row = (const Uint8 *)s->pixels + (size_t)(y0 + y) * s->pitch + (size_t)x0 * bpp;
		unsigned char *d = out + (size_t)y * out_pitch;

		for(x = 0; x < w; x++, row += bpp, d += 4)
		{
			Uint32 px;
			Uint8 r, g, b, a;

			switch(bpp)
			{
			case 1:  px = row[0]; break;
			case 2:  px = *(const Uint16 *)row; break;
			case 3:  px = ((Uint32)row[0] << 16) | ((Uint32)row[1] << 8) | row[2]; break;
			default: px = *(const Uint32 *)row; break;
			}

			SDL_GetRGBA(px, f, &r, &g, &b, &a);

			if(keyed && px == key) a = 0;

			d[0] = r; d[1] = g; d[2] = b; d[3] = a;
		}
	}
}

static int gl_refused(void)
{
	GLenum e = glGetError();

	return e == GL_OUT_OF_MEMORY;
}

static void tex_drop_names(struct zod_gl_tex *t)
{
	if(t->tor)
	{
		struct torus *tr = t->tor;

		if(tr->name && zod_gl_active && t->gen == ctx_gen)
		{
			if(retired_n < RETIRE_MAX)
			{
				retired[retired_n].name = tr->name;
				retired[retired_n].tw = (short)tr->W;
				retired[retired_n].th = (short)tr->H;
				retired[retired_n].frame = frame_no;
				retired_n++;
			}
			else
				delete_name(tr->name);
		}

		free(tr->slot_tile);
		free(tr->stale);
		free(tr);
		t->tor = 0;
	}

	if(t->atlas)
	{
		/* the page belongs to the atlas. A loaded image's place stays used
		 * until the atlas starts again; a run-time slot is retired. */
		if(t->atlas == 2 && t->names && zod_gl_active && t->gen == ctx_gen &&
		   dyn_retired_n < DYN_FREE)
		{
			int pg;

			for(pg = 0; pg < page_n; pg++)
				if(pages[pg].name == t->names[0]) break;

			if(pg < page_n)
			{
				struct dyn_slot *d = &dyn_retired[dyn_retired_n++];

				d->page = (short)pg;
				d->x = (short)t->bx;
				d->y = (short)t->by;
				d->cw = (short)t->slot_w;
				d->ch = (short)t->slot_h;
				d->frame = frame_no;
			}
		}

		free(t->names);
		t->names = 0;
		return;
	}

	if(t->names)
	{
		/* only into the context that made them -- and never without one:
		 * surfaces are also freed after the context closed (at exit) */
		if(zod_gl_active && t->gen == ctx_gen)
		{
			int i;

			for(i = 0; i < t->cols * t->rows; i++)
			{
				if(retired_n < RETIRE_MAX)
				{
					retired[retired_n].name = t->names[i];
					retired[retired_n].tw = (short)t->tw;
					retired[retired_n].th = (short)t->th;
					retired[retired_n].frame = frame_no;
					retired_n++;
				}
				else
					delete_name(t->names[i]);
			}
		}

		free(t->names);
		t->names = 0;
	}
}

static void vertex(float u, float v, float x, float y);

/* the map texture for t (see struct torus) */
static void torus_make(struct zod_gl_tex *t, SDL_Surface *s)
{
	struct torus *tr = (struct torus *)calloc(1, sizeof(*tr));
	int W = MAP_TILE, H = MAP_TILE, i;

	if(!tr)
	{
		t->failed = 1;
		return;
	}

	while(W < screen_w + 2 * MAP_TILE && W < tile_max) W <<= 1;
	while(H < screen_h + 2 * MAP_TILE && H < tile_max) H <<= 1;

	tr->W = W;
	tr->H = H;
	tr->nx = W / MAP_TILE;
	tr->ny = H / MAP_TILE;
	tr->mtx = (s->w + MAP_TILE - 1) / MAP_TILE;
	tr->mty = (s->h + MAP_TILE - 1) / MAP_TILE;
	tr->slot_tile = (int *)malloc(sizeof(int) * tr->nx * tr->ny);
	tr->stale = (unsigned char *)calloc((size_t)tr->nx * tr->ny, 1);
	t->tor = tr;

	if(!tr->slot_tile || !tr->stale)
	{
		t->failed = 1;
		return;
	}

	for(i = 0; i < tr->nx * tr->ny; i++) tr->slot_tile[i] = -1;

	batch_end();

	/* a freed map's texture of the same size (the last map) */
	for(i = pool_n - 1; i >= 0; i--)
		if(pool[i].tw == W && pool[i].th == H) break;

	if(i >= 0)
	{
		tr->name = pool[i].name;
		pool[i] = pool[--pool_n];
		st_reused++;
	}
	else
	{
		/* RadeonGL takes no pixels (storage only); other libraries get zeros */
		unsigned char *zero = npot_ok ? 0 : (unsigned char *)calloc((size_t)W * H, 3);

		glGenTextures(1, &tr->name);
		glBindTexture(GL_TEXTURE_2D, tr->name);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
		/* opaque: RGB, which RadeonGL keeps as RGB565 */
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, W, H, 0, GL_RGB, GL_UNSIGNED_BYTE, zero);
		free(zero);

		if(gl_refused())
		{
			delete_name(tr->name);
			tr->name = 0;
			t->failed = 1;
			st_refused++;
			ZLOG("GL: no room for the %dx%d map texture -- the map stays undrawn\n", W, H);
			return;
		}

		st_made_px += (unsigned long)W * H;
	}

	cur_name = 0;
	st_made++;
	ZLOG("GL: map %dx%d through a %dx%d texture of %dx%d tiles\n",
	     s->w, s->h, W, H, MAP_TILE, MAP_TILE);
}

/* map tile (mx, my) into its slot, if it is not there yet */
static int torus_tile(struct zod_gl_tex *t, int mx, int my)
{
	struct torus *tr = t->tor;
	const int slot = (my % tr->ny) * tr->nx + (mx % tr->nx);
	const int tile = my * tr->mtx + mx;
	const int x0 = mx * MAP_TILE, y0 = my * MAP_TILE;
	const int w = (t->w - x0 < MAP_TILE) ? t->w - x0 : MAP_TILE;
	const int h = (t->h - y0 < MAP_TILE) ? t->h - y0 : MAP_TILE;
	static unsigned char buf[MAP_TILE * MAP_TILE * 4];

	if(tr->slot_tile[slot] == tile && !tr->stale[slot]) return 0;

	batch_end();
	convert(t->s, x0, y0, w, h, buf, w * 4);
	glBindTexture(GL_TEXTURE_2D, tr->name);
	glTexSubImage2D(GL_TEXTURE_2D, 0, (mx % tr->nx) * MAP_TILE, (my % tr->ny) * MAP_TILE,
	                w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	cur_name = 0;

	tr->slot_tile[slot] = tile;
	tr->stale[slot] = 0;
	st_map_tiles++;

	return 1;
}

/* the map surface's rectangle src at x, y */
static void torus_blit(struct zod_gl_tex *t, int sx0, int sy0, int sx1, int sy1,
                       int x, int y, Uint8 alpha)
{
	struct torus *tr = t->tor;
	const int mx0 = sx0 / MAP_TILE, my0 = sy0 / MAP_TILE;
	const int mx1 = (sx1 - 1) / MAP_TILE, my1 = (sy1 - 1) / MAP_TILE;
	int mx, my, ahead = 1;

	/* the view's tiles first, all of them: they are drawn now */
	for(my = my0; my <= my1; my++)
		for(mx = mx0; mx <= mx1; mx++)
			torus_tile(t, mx, my);

	/* one tile of the ring around the view ahead of time -- only those
	 * whose slot holds nothing of the view itself */
	for(my = my0 - 1; my <= my1 + 1 && ahead; my++)
		for(mx = mx0 - 1; mx <= mx1 + 1 && ahead; mx++)
		{
			if(mx < 0 || my < 0 || mx >= tr->mtx || my >= tr->mty) continue;
			if(mx >= mx0 && mx <= mx1 && my >= my0 && my <= my1) continue;
			if(mx1 - mx0 + 1 >= tr->nx || my1 - my0 + 1 >= tr->ny) continue;
			{
				/* its slot is free of the view: another column or row */
				const int sxl = mx % tr->nx, syl = my % tr->ny;
				int vx, vy, used = 0;

				for(vy = my0; vy <= my1 && !used; vy++)
					for(vx = mx0; vx <= mx1 && !used; vx++)
						if(vx % tr->nx == sxl && vy % tr->ny == syl) used = 1;

				if(!used && torus_tile(t, mx, my)) ahead = 0;
			}
		}

	for(my = my0; my <= my1; my++)
		for(mx = mx0; mx <= mx1; mx++)
		{
			const int tx0 = mx * MAP_TILE, ty0 = my * MAP_TILE;
			const int ox0 = sx0 > tx0 ? sx0 : tx0;
			const int oy0 = sy0 > ty0 ? sy0 : ty0;
			const int ox1 = sx1 < tx0 + MAP_TILE ? sx1 : tx0 + MAP_TILE;
			const int oy1 = sy1 < ty0 + MAP_TILE ? sy1 : ty0 + MAP_TILE;
			const int px = (mx % tr->nx) * MAP_TILE, py = (my % tr->ny) * MAP_TILE;
			float u0, v0, u1, v1, X0, Y0, X1, Y1;

			if(ox1 <= ox0 || oy1 <= oy0) continue;

			u0 = (float)(px + ox0 - tx0) / tr->W;
			v0 = (float)(py + oy0 - ty0) / tr->H;
			u1 = (float)(px + ox1 - tx0) / tr->W;
			v1 = (float)(py + oy1 - ty0) / tr->H;
			X0 = (float)(x + ox0 - sx0);
			Y0 = (float)(y + oy0 - sy0);
			X1 = X0 + (ox1 - ox0);
			Y1 = Y0 + (oy1 - oy0);

			batch_begin(tr->name, 0);
			quad_colour(255, 255, 255, alpha);
			vertex(u0, v0, X0, Y0);
			vertex(u1, v0, X1, Y0);
			vertex(u1, v1, X1, Y1);
			vertex(u0, v1, X0, Y1);
			batch_quads++;
			st_quads++;
			st_map_quads++;
		}
}

/* a cleared page; page 0 gets the white block for fills */
static int atlas_page_new(void)
{
	struct atlas_page *p;
	unsigned char *zero;
	int y;

	if(page_n >= ATLAS_PAGES) return -1;

	p = &pages[page_n];
	p->h = npot_ok ? ATLAS_W - 1 : ATLAS_W;
	p->ycur = 0;

	batch_end();
	glGenTextures(1, &p->name);
	glBindTexture(GL_TEXTURE_2D, p->name);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	/* NULL pixels (storage only) on RadeonGL alone: PiStorm3D reads them
	 * from address 0 over the Amiga bus -- with RGBA that froze the machine
	 * (memory: pistorm3d-defects 1). Elsewhere the page comes zeroed. */
	if(npot_ok)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, p->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	else
	{
		zero = (unsigned char *)calloc((size_t)ATLAS_W * p->h, 4);

		if(!zero)
		{
			delete_name(p->name);
			p->name = 0;
			return -1;
		}

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, p->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
		free(zero);
	}

	cur_name = 0;

	if(gl_refused())
	{
		delete_name(p->name);
		p->name = 0;
		st_refused++;
		return -1;
	}

	/* RadeonGL: transparent between the images, in strips (the buffer
	 * stays small); the contents are undefined until written */
	if(npot_ok)
	{
		zero = (unsigned char *)calloc((size_t)ATLAS_W * 64, 4);

		if(zero)
		{
			for(y = 0; y < p->h; y += 64)
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, ATLAS_W, (p->h - y < 64) ? p->h - y : 64,
				                GL_RGBA, GL_UNSIGNED_BYTE, zero);
			free(zero);
		}
	}

	st_made_px += (unsigned long)ATLAS_W * p->h;

	if(page_n == 0)
	{
		/* a 4x4 white block at the top left, its own 4-row shelf */
		static unsigned char white[4 * 4 * 4];

		memset(white, 255, sizeof(white));
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, white);
		white_name = p->name;
		white_u = 2.0f / ATLAS_W;
		white_v = 2.0f / p->h;
		p->ycur = 5;
	}

	return page_n++;
}

/* the shelf height for an image h rows tall */
static int shelf_class(int h)
{
	if(h <= 8)  return 8;
	if(h <= 16) return 16;
	if(h <= 24) return 24;
	if(h <= 32) return 32;
	if(h <= 48) return 48;
	if(h <= 64) return 64;
	if(h <= 96) return 96;
	if(h <= 128) return 128;
	if(h <= 192) return 192;
	return 256;
}

/* forget every image in the atlas: they are packed again as drawn */
static void atlas_reset(void)
{
	int i;

	for(i = 0; i < REG_SIZE; i++)
		if(reg[i].s && reg[i].s != REG_TOMB && reg[i].t && reg[i].t->atlas)
		{
			free(reg[i].t->names);
			free(reg[i].t);
			reg[i].t = 0;
		}

	shelf_n = 0;
	dyn_free_n = 0;
	dyn_retired_n = 0;

	for(i = 0; i < page_n; i++) pages[i].ycur = (i == 0) ? 5 : 0;

	st_atlas_resets++;
	ZLOG("GL: atlas full -- starting it again\n");
}

/* a place for w x h in a page; 0 = no room */
static int atlas_place(int w, int h, int *page, int *x, int *y)
{
	const int cls = shelf_class(h);
	int i;

	for(i = 0; i < shelf_n; i++)
	{
		struct atlas_shelf *sh = &shelves[i];

		if(sh->h == cls && sh->x + w <= ATLAS_W)
		{
			*page = sh->page; *x = sh->x; *y = sh->y;
			sh->x = (short)(sh->x + w + 1);
			return 1;
		}
	}

	if(shelf_n >= ATLAS_SHELVES) return 0;

	/* a new shelf: in the last page with room, else a new page */
	for(i = 0; i <= page_n; i++)
	{
		const int pg = (i < page_n) ? i : atlas_page_new();

		if(pg < 0) return 0;

		if(pages[pg].ycur + cls <= pages[pg].h)
		{
			struct atlas_shelf *sh = &shelves[shelf_n++];

			sh->page = (short)pg;
			sh->y = (short)pages[pg].ycur;
			sh->h = (short)cls;
			sh->x = (short)(w + 1);
			pages[pg].ycur += cls + 1;
			*page = pg; *x = 0; *y = sh->y;
			return 1;
		}
	}

	return 0;
}

/* s into page pg at x, y */
static int atlas_fill(struct zod_gl_tex *t, SDL_Surface *s, int pg, int x, int y);

/* s into the atlas; 0 = no room (it then gets a texture of its own) */
static int atlas_put(struct zod_gl_tex *t, SDL_Surface *s)
{
	int pg, x, y;

	if(!atlas_place(s->w, s->h, &pg, &x, &y))
	{
		atlas_reset();
		if(!atlas_place(s->w, s->h, &pg, &x, &y)) return 0;
	}

	if(!atlas_fill(t, s, pg, x, y)) return 0;

	t->atlas = 1;
	st_atlas_in++;
	st_atlas_px += (unsigned long)s->w * s->h;

	return 1;
}

/* a run-time surface into a slot of its size class */
static int dyn_put(struct zod_gl_tex *t, SDL_Surface *s)
{
	int cw = 16, ch = 8, pg, x, y, i;

	while(cw < s->w) cw <<= 1;
	while(ch < s->h) ch <<= 1;

	for(i = dyn_free_n - 1; i >= 0; i--)
		if(dyn_free[i].cw == cw && dyn_free[i].ch == ch) break;

	if(i >= 0)
	{
		pg = dyn_free[i].page;
		x = dyn_free[i].x;
		y = dyn_free[i].y;
		dyn_free[i] = dyn_free[--dyn_free_n];
		st_dyn_reused++;
	}
	else if(!atlas_place(cw, ch, &pg, &x, &y))
	{
		atlas_reset();
		if(!atlas_place(cw, ch, &pg, &x, &y)) return 0;
	}

	if(!atlas_fill(t, s, pg, x, y)) return 0;

	t->atlas = 2;
	t->slot_w = cw;
	t->slot_h = ch;
	st_dyn_in++;

	return 1;
}

static int atlas_fill(struct zod_gl_tex *t, SDL_Surface *s, int pg, int x, int y)
{
	unsigned char *buf;

	t->names = (GLuint *)malloc(sizeof(GLuint));
	buf = (unsigned char *)malloc((size_t)s->w * s->h * 4);

	if(!t->names || !buf)
	{
		free(t->names);
		t->names = 0;
		free(buf);
		return 0;
	}

	convert(s, 0, 0, s->w, s->h, buf, s->w * 4);

	batch_end();
	glBindTexture(GL_TEXTURE_2D, pages[pg].name);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	cur_name = 0;
	free(buf);

	t->names[0] = pages[pg].name;
	t->tw = ATLAS_W;
	t->th = pages[pg].h;
	t->cols = t->rows = 1;
	t->bx = x;
	t->by = y;

	return 1;
}

static struct zod_gl_tex *tex_make(SDL_Surface *s, int atlas_ok)
{
	struct zod_gl_tex *t = (struct zod_gl_tex *)calloc(1, sizeof(*t));
	unsigned char *buf;
	int c, r, n;

	if(!t) return 0;

	t->s = s;
	t->w = s->w;
	t->h = s->h;
	t->gen = ctx_gen;

	if((long)s->w * s->h > MAP_MIN_PX)
	{
		torus_make(t, s);
		return t;
	}

	if(atlas_ok && s->w <= ATLAS_SIDE && s->h <= ATLAS_SIDE && atlas_put(t, s))
	{
		st_made++;
		return t;
	}

	if(!atlas_ok && s->w <= DYN_W && s->h <= DYN_H && dyn_put(t, s))
	{
		st_made++;
		return t;
	}

	t->tw = (npot_ok && s->w > 256) ? (s->w < tile_max ? s->w : tile_max) : pot(s->w);
	t->th = (npot_ok && s->h > 256) ? (s->h < tile_max ? s->h : tile_max) : pot(s->h);
	t->cols = (s->w + t->tw - 1) / t->tw;
	t->rows = (s->h + t->th - 1) / t->th;
	t->gen = ctx_gen;
	n = t->cols * t->rows;

	t->names = (GLuint *)calloc((size_t)n, sizeof(GLuint));
	buf = (unsigned char *)malloc((size_t)t->tw * t->th * 4);

	if(!t->names || !buf)
	{
		free(buf);
		t->failed = 1;
		ZLOG("GL: no memory to convert a %dx%d surface\n", s->w, s->h);
		return t;
	}

	batch_end();

	for(r = 0; r < t->rows && !t->failed; r++)
		for(c = 0; c < t->cols && !t->failed; c++)
		{
			const int x0 = c * t->tw, y0 = r * t->th;
			const int cw = (s->w - x0 < t->tw) ? s->w - x0 : t->tw;
			const int ch = (s->h - y0 < t->th) ? s->h - y0 : t->th;

			if(cw < t->tw || ch < t->th) memset(buf, 0, (size_t)t->tw * t->th * 4);

			convert(s, x0, y0, cw, ch, buf, t->tw * 4);

			{
				GLuint *name = &t->names[r * t->cols + c];
				int i;

				for(i = pool_n - 1; i >= 0; i--)
					if(pool[i].tw == t->tw && pool[i].th == t->th) break;

				if(i >= 0)
				{
					/* a freed surface's texture of the same size: refilled */
					*name = pool[i].name;
					pool[i] = pool[--pool_n];

					glBindTexture(GL_TEXTURE_2D, *name);
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->tw, t->th,
					                GL_RGBA, GL_UNSIGNED_BYTE, buf);
					st_reused++;
				}
				else
				{
					glGenTextures(1, name);
					glBindTexture(GL_TEXTURE_2D, *name);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->tw, t->th, 0,
					             GL_RGBA, GL_UNSIGNED_BYTE, buf);
				}
			}

			if(gl_refused())
			{
				t->failed = 1;
				st_refused++;

				if(refused_told < 3)
					ZLOG("GL: texture memory full -- a %dx%d surface stays undrawn%s\n",
					     s->w, s->h, ++refused_told == 3 ? " (no more of these)" : "");
			}

			st_made_px += (unsigned long)t->tw * t->th;
		}

	cur_name = 0;
	free(buf);
	st_made++;

	/* refused: the tiles made so far give their memory back at once
	 * (nothing has drawn with them), not through the retired list */
	if(t->failed)
	{
		int i;

		for(i = 0; i < n; i++)
			if(t->names[i]) delete_name(t->names[i]);

		free(t->names);
		t->names = 0;
	}

	return t;
}

static void tex_refresh(struct zod_gl_tex *t)
{
	unsigned char *buf;
	int c, r;

	if(t->dx1 <= t->dx0 || t->dy1 <= t->dy0) return;

	batch_end();

	for(r = 0; r < t->rows; r++)
		for(c = 0; c < t->cols; c++)
		{
			const int x0 = c * t->tw, y0 = r * t->th;
			const int ox0 = t->dx0 > x0 ? t->dx0 : x0;
			const int oy0 = t->dy0 > y0 ? t->dy0 : y0;
			const int ox1 = t->dx1 < x0 + t->tw ? t->dx1 : x0 + t->tw;
			const int oy1 = t->dy1 < y0 + t->th ? t->dy1 : y0 + t->th;

			if(ox1 <= ox0 || oy1 <= oy0) continue;

			buf = (unsigned char *)malloc((size_t)(ox1 - ox0) * (oy1 - oy0) * 4);
			if(!buf) continue;

			convert(t->s, ox0, oy0, ox1 - ox0, oy1 - oy0, buf, (ox1 - ox0) * 4);

			glBindTexture(GL_TEXTURE_2D, t->names[r * t->cols + c]);
			glTexSubImage2D(GL_TEXTURE_2D, 0, t->bx + ox0 - x0, t->by + oy0 - y0,
			                ox1 - ox0, oy1 - oy0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
			free(buf);

			st_updates++;
			st_update_px += (unsigned long)(ox1 - ox0) * (oy1 - oy0);
		}

	cur_name = 0;
	t->dx0 = t->dy0 = t->dx1 = t->dy1 = 0;
}

static void tex_free(struct zod_gl_tex *t)
{
	if(!t) return;

	tex_drop_names(t);
	free(t);
}

/* the texture for s, made or brought up to date as needed; 0 = none */
static struct zod_gl_tex *tex_get(SDL_Surface *s)
{
	struct reg_entry *e;
	struct zod_gl_tex *t;

	if(!s || !s->pixels || !zod_gl_active) return 0;

	e = reg_find(s);

	if(!e)
	{
		e = reg_add(s);
		if(!e) return 0;
	}

	t = e->t;

	/* a new size or a new context: made again */
	if(t && (t->w != s->w || t->h != s->h || t->gen != ctx_gen))
	{
		tex_free(t);
		t = e->t = 0;
	}

	if(!t) t = e->t = tex_make(s, e->atlas_ok);
	if(!t || t->failed) return 0;

	if(!t->tor) tex_refresh(t);

	return t;
}

void zod_gl_changed(SDL_Surface *s, int x, int y, int w, int h)
{
	struct reg_entry *e;
	struct zod_gl_tex *t;
	int x1 = x + w, y1 = y + h;

	if(!s) return;

	e = reg_find(s);
	if(!e || !(t = e->t)) return;   /* no texture yet: made fresh anyway */

	if(x < 0) x = 0;
	if(y < 0) y = 0;
	if(x1 > t->w) x1 = t->w;
	if(y1 > t->h) y1 = t->h;
	if(x1 <= x || y1 <= y) return;

	if(t->tor)
	{
		struct torus *tr = t->tor;
		int mx, my;

		for(my = y / MAP_TILE; my <= (y1 - 1) / MAP_TILE; my++)
			for(mx = x / MAP_TILE; mx <= (x1 - 1) / MAP_TILE; mx++)
			{
				const int slot = (my % tr->ny) * tr->nx + (mx % tr->nx);

				if(tr->slot_tile[slot] == my * tr->mtx + mx) tr->stale[slot] = 1;
			}

		return;
	}

	if(t->dx1 <= t->dx0)
	{
		t->dx0 = x; t->dy0 = y; t->dx1 = x1; t->dy1 = y1;
		return;
	}

	if(x < t->dx0) t->dx0 = x;
	if(y < t->dy0) t->dy0 = y;
	if(x1 > t->dx1) t->dx1 = x1;
	if(y1 > t->dy1) t->dy1 = y1;
}

void zod_gl_forget(SDL_Surface *s)
{
	struct reg_entry *e;

	if(!s) return;

	e = reg_find(s);
	if(!e) return;

	tex_free(e->t);
	e->t = 0;
	e->s = REG_TOMB;
}

void zod_gl_drop_all(void)
{
	int i;

	for(i = 0; i < REG_SIZE; i++)
		if(reg[i].s && reg[i].s != REG_TOMB && reg[i].t)
		{
			tex_free(reg[i].t);
			reg[i].t = 0;
		}
}

/* ---- drawing ------------------------------------------------------------- */

void zod_gl_init(int w, int h)
{
	screen_w = w;
	screen_h = h;
	ctx_gen++;

	in_batch = 0;
	cur_name = 0;
	cur_mode = -1;
	cur_rgba_ok = 0;
	retired_n = 0;
	pool_n = 0;
	page_n = 0;
	shelf_n = 0;
	white_name = 0;
	dyn_free_n = 0;
	dyn_retired_n = 0;

	{
		const char *ext = (const char *)glGetString(GL_EXTENSIONS);
		GLint max = 0;

		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
		tile_max = max >= 256 ? (max > 2048 ? 2048 : (int)max) : TILE_MAX;
		/* RadeonGL (its extensions are GL_RGL_*): any size, one level */
		npot_ok = ext && strstr(ext, "GL_RGL_") != 0;

		ZLOG("GL: textures up to %d, %s\n", tile_max,
		     npot_ok ? "any size (large surfaces at their own size)" : "powers of two");
	}

	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, (double)w, (double)h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glShadeModel(GL_FLAT);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.0f);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

void zod_gl_flush(void)
{
	int i, j;

	batch_end();
	st_frames++;
	frame_no++;

	/* names whose surface went RETIRE_FRAMES frames ago: into the pool */
	for(i = 0, j = 0; i < retired_n; i++)
	{
		if(frame_no - retired[i].frame < RETIRE_FRAMES)
		{
			retired[j++] = retired[i];
			continue;
		}

		if(pool_n < POOL_MAX)
			pool[pool_n++] = retired[i];
		else
			delete_name(retired[i].name);
	}

	retired_n = j;

	for(i = 0, j = 0; i < dyn_retired_n; i++)
	{
		if(frame_no - dyn_retired[i].frame < RETIRE_FRAMES)
			dyn_retired[j++] = dyn_retired[i];
		else if(dyn_free_n < DYN_FREE)
			dyn_free[dyn_free_n++] = dyn_retired[i];
	}

	dyn_retired_n = j;
}

void zod_gl_clear(Uint8 r, Uint8 g, Uint8 b)
{
	batch_end();
	glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

static void vertex(float u, float v, float x, float y)
{
	glTexCoord2f(u, v);
	glVertex3f(x, y, 0.0f);
}

void zod_gl_blit(SDL_Surface *s, const SDL_Rect *src, int x, int y,
                 Uint8 alpha, int hit)
{
	struct zod_gl_tex *t = tex_get(s);
	int sx0, sy0, sx1, sy1, c, r;

	if(!t) return;

	sx0 = src ? src->x : 0;
	sy0 = src ? src->y : 0;
	sx1 = src ? src->x + src->w : t->w;
	sy1 = src ? src->y + src->h : t->h;

	/* a source rectangle outside the surface moves the destination along,
	 * as SDL_BlitSurface clips it */
	if(sx0 < 0) { x -= sx0; sx0 = 0; }
	if(sy0 < 0) { y -= sy0; sy0 = 0; }
	if(sx1 > t->w) sx1 = t->w;
	if(sy1 > t->h) sy1 = t->h;
	if(sx1 <= sx0 || sy1 <= sy0) return;

	if(t->tor)
	{
		torus_blit(t, sx0, sy0, sx1, sy1, x, y, alpha);
		return;
	}

	for(r = 0; r < t->rows; r++)
		for(c = 0; c < t->cols; c++)
		{
			const int tx0 = c * t->tw, ty0 = r * t->th;
			const int ox0 = sx0 > tx0 ? sx0 : tx0;
			const int oy0 = sy0 > ty0 ? sy0 : ty0;
			const int ox1 = sx1 < tx0 + t->tw ? sx1 : tx0 + t->tw;
			const int oy1 = sy1 < ty0 + t->th ? sy1 : ty0 + t->th;
			float u0, v0, u1, v1, X0, Y0, X1, Y1;

			if(ox1 <= ox0 || oy1 <= oy0) continue;

			u0 = (float)(t->bx + ox0 - tx0) / t->tw;
			v0 = (float)(t->by + oy0 - ty0) / t->th;
			u1 = (float)(t->bx + ox1 - tx0) / t->tw;
			v1 = (float)(t->by + oy1 - ty0) / t->th;
			X0 = (float)(x + ox0 - sx0);
			Y0 = (float)(y + oy0 - sy0);
			X1 = X0 + (ox1 - ox0);
			Y1 = Y0 + (oy1 - oy0);

			bind_is_own = !t->atlas;
			batch_begin(t->names[r * t->cols + c], hit ? 1 : 0);
			bind_is_own = 0;
			quad_colour(255, 255, 255, alpha);
			vertex(u0, v0, X0, Y0);
			vertex(u1, v0, X1, Y0);
			vertex(u1, v1, X1, Y1);
			vertex(u0, v1, X0, Y1);
			batch_quads++;
			st_quads++;
		}
}

void zod_gl_blit_rot(SDL_Surface *s, float cx, float cy, float size,
                     float angle, Uint8 alpha, int hit)
{
	struct zod_gl_tex *t = tex_get(s);
	const float a = angle * 3.14159265f / 180.0f;
	const float ca = cosf(a) * size, sa = sinf(a) * size;
	int c, r;

	if(!t) return;

	for(r = 0; r < t->rows; r++)
		for(c = 0; c < t->cols; c++)
		{
			const int tx0 = c * t->tw, ty0 = r * t->th;
			const int cw = (t->w - tx0 < t->tw) ? t->w - tx0 : t->tw;
			const int ch = (t->h - ty0 < t->th) ? t->h - ty0 : t->th;
			/* the tile's corners around the surface's centre */
			const float x0 = tx0 - t->w * 0.5f, y0 = ty0 - t->h * 0.5f;
			const float x1 = x0 + cw, y1 = y0 + ch;
			const float u0 = (float)t->bx / t->tw, v0 = (float)t->by / t->th;
			const float u1 = (float)(t->bx + cw) / t->tw, v1 = (float)(t->by + ch) / t->th;

			batch_begin(t->names[r * t->cols + c], hit ? 1 : 0);
			quad_colour(255, 255, 255, alpha);

			/* counter-clockwise on screen (y grows downwards):
			 * x' = x cos + y sin, y' = -x sin + y cos */
			vertex(u0, v0, cx + x0 * ca + y0 * sa, cy - x0 * sa + y0 * ca);
			vertex(u1, v0, cx + x1 * ca + y0 * sa, cy - x1 * sa + y0 * ca);
			vertex(u1, v1, cx + x1 * ca + y1 * sa, cy - x1 * sa + y1 * ca);
			vertex(u0, v1, cx + x0 * ca + y1 * sa, cy - x0 * sa + y1 * ca);
			batch_quads++;
			st_quads++;
		}
}

void zod_gl_fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b)
{
	if(!zod_gl_active || w <= 0 || h <= 0) return;

	if(!white_name && page_n == 0) atlas_page_new();

	if(white_name)
	{
		/* the atlas's white block, coloured: no switch of texturing */
		batch_begin(white_name, 0);
		quad_colour(r, g, b, 255);
		vertex(white_u, white_v, (float)x, (float)y);
		vertex(white_u, white_v, (float)(x + w), (float)y);
		vertex(white_u, white_v, (float)(x + w), (float)(y + h));
		vertex(white_u, white_v, (float)x, (float)(y + h));
	}
	else
	{
		batch_begin(0, 2);
		quad_colour(r, g, b, 255);
		glVertex3f((float)x, (float)y, 0.0f);
		glVertex3f((float)(x + w), (float)y, 0.0f);
		glVertex3f((float)(x + w), (float)(y + h), 0.0f);
		glVertex3f((float)x, (float)(y + h), 0.0f);
	}

	batch_quads++;
	st_fills++;
}

void zod_gl_hint_atlas(SDL_Surface *s)
{
	struct reg_entry *e;

	if(!s || !zod_gl_active) return;

	e = reg_find(s);
	if(!e) e = reg_add(s);
	if(e) e->atlas_ok = 1;
}

void zod_gl_show_surface(SDL_Surface *s)
{
	if(!s) return;

	zod_gl_changed(s, 0, 0, s->w, s->h);
	zod_gl_blit(s, 0, 0, 0, 255, 0);
}

void zod_gl_report(void)
{
	const unsigned long f = st_frames ? st_frames : 1;

	ZLOG("GL: %lu frames; per frame x10: batches %lu, binds %lu, quads %lu, fills %lu\n",
	     st_frames, st_batches * 10 / f, st_binds * 10 / f, st_quads * 10 / f, st_fills * 10 / f);
	ZLOG("GL: map texture: %lu tiles sent, %lu quads drawn (per frame x10: %lu, %lu)\n",
	     st_map_tiles, st_map_quads, st_map_tiles * 10 / f, st_map_quads * 10 / f);
	ZLOG("GL: binds of own textures (not atlas, not map) per frame x10: %lu\n", st_bind_own * 10 / f);
	ZLOG("GL: atlas: %d pages, %lu images packed (%lu px), started again %lu times;"
	     " run-time slots %lu (%lu of them reused)\n",
	     page_n, st_atlas_in, st_atlas_px, st_atlas_resets, st_dyn_in, st_dyn_reused);
	ZLOG("GL: textures made %lu (%lu texels, %lu tiles refilled from freed ones), updates %lu (%lu px), refused %lu\n",
	     st_made, st_made_px, st_reused, st_updates, st_update_px, st_refused);
}
