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
	BATCH_QUADS = 1000    /* 4000 vertices: below MiniGL's vertex buffer (4096) */
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
};

static int screen_w = 0, screen_h = 0;
static unsigned long ctx_gen = 0;

/* the open batch */
static int in_batch = 0;
static int batch_quads = 0;
static GLuint cur_name = 0;
static Uint8 cur_alpha = 255;
static int cur_mode = -1;      /* 0 textured, 1 hit flash, 2 fill */

/* counters (per run; divided by frames in the report) */
static unsigned long st_frames, st_binds, st_batches, st_quads, st_fills;
static unsigned long st_made, st_made_px, st_updates, st_update_px, st_refused;
static unsigned long st_reused;

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

	while(p < n && p < TILE_MAX) p <<= 1;

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

static void batch_begin(GLuint name, Uint8 alpha, int mode)
{
	if(in_batch && mode == cur_mode && batch_quads < BATCH_QUADS &&
	   (mode == 2 || (name == cur_name && alpha == cur_alpha)))
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

	if(mode != 2)
	{
		if(name != cur_name)
		{
			glBindTexture(GL_TEXTURE_2D, name);
			cur_name = name;
			st_binds++;
		}

		glColor4ub(255, 255, 255, alpha);
		cur_alpha = alpha;
	}

	glBegin(GL_QUADS);
	in_batch = 1;
	batch_quads = 0;
	st_batches++;
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

static struct zod_gl_tex *tex_make(SDL_Surface *s)
{
	struct zod_gl_tex *t = (struct zod_gl_tex *)calloc(1, sizeof(*t));
	unsigned char *buf;
	int c, r, n;

	if(!t) return 0;

	t->s = s;
	t->w = s->w;
	t->h = s->h;
	t->tw = pot(s->w);
	t->th = pot(s->h);
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
				ZLOG("GL: texture memory full -- a %dx%d surface stays undrawn\n", s->w, s->h);
			}

			st_made_px += (unsigned long)t->tw * t->th;
		}

	cur_name = 0;
	free(buf);
	st_made++;

	if(t->failed) tex_drop_names(t);

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
			glTexSubImage2D(GL_TEXTURE_2D, 0, ox0 - x0, oy0 - y0, ox1 - ox0, oy1 - oy0,
			                GL_RGBA, GL_UNSIGNED_BYTE, buf);
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

	if(!t) t = e->t = tex_make(s);
	if(!t || t->failed) return 0;

	tex_refresh(t);

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
	cur_alpha = 255;
	retired_n = 0;
	pool_n = 0;

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

			u0 = (float)(ox0 - tx0) / t->tw;
			v0 = (float)(oy0 - ty0) / t->th;
			u1 = (float)(ox1 - tx0) / t->tw;
			v1 = (float)(oy1 - ty0) / t->th;
			X0 = (float)(x + ox0 - sx0);
			Y0 = (float)(y + oy0 - sy0);
			X1 = X0 + (ox1 - ox0);
			Y1 = Y0 + (oy1 - oy0);

			batch_begin(t->names[r * t->cols + c], alpha, hit ? 1 : 0);
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
			const float u1 = (float)cw / t->tw, v1 = (float)ch / t->th;

			batch_begin(t->names[r * t->cols + c], alpha, hit ? 1 : 0);

			/* counter-clockwise on screen (y grows downwards):
			 * x' = x cos + y sin, y' = -x sin + y cos */
			vertex(0.0f, 0.0f, cx + x0 * ca + y0 * sa, cy - x0 * sa + y0 * ca);
			vertex(u1,   0.0f, cx + x1 * ca + y0 * sa, cy - x1 * sa + y0 * ca);
			vertex(u1,   v1,   cx + x1 * ca + y1 * sa, cy - x1 * sa + y1 * ca);
			vertex(0.0f, v1,   cx + x0 * ca + y1 * sa, cy - x0 * sa + y1 * ca);
			batch_quads++;
			st_quads++;
		}
}

void zod_gl_fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b)
{
	if(!zod_gl_active || w <= 0 || h <= 0) return;

	batch_begin(0, 255, 2);
	glColor4ub(r, g, b, 255);
	glVertex3f((float)x, (float)y, 0.0f);
	glVertex3f((float)(x + w), (float)y, 0.0f);
	glVertex3f((float)(x + w), (float)(y + h), 0.0f);
	glVertex3f((float)x, (float)(y + h), 0.0f);
	batch_quads++;
	st_fills++;
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
	ZLOG("GL: textures made %lu (%lu texels, %lu tiles refilled from freed ones), updates %lu (%lu px), refused %lu\n",
	     st_made, st_made_px, st_reused, st_updates, st_update_px, st_refused);
}
