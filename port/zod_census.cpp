/*
 * Draw census -- see zod_census.h for what is counted and why.
 *
 * Plain C style on purpose (no STL, no exceptions): the module is part of
 * every build, Amiga included, and must compile with gcc 6.5 as well.
 * Memory is taken only when the census is switched on.
 */
#include "zod_census.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zod_log.h"
#include "amiga_startup.h"   /* zod_env: GetVar on AmigaOS, getenv elsewhere */

int zod_census_on = 0;

/* ---- registry: surface pointer -> group --------------------------------- */

enum { REG_SIZE = 65536 };   /* power of two; 6951 images + derived copies */
enum { MAX_GROUPS = 48, GROUP_NAME = 16 };
enum { MAX_DYN = 64 };        /* distinct dynamic surfaces tracked per frame */

#define REG_TOMB ((SDL_Surface *)1)

struct reg_entry
{
	SDL_Surface *s;
	unsigned long stamp;      /* frame in which this source was last counted */
	short group;              /* 1..groups; 0 = none */
	unsigned char kind;       /* 0 base, ZOD_CENSUS_ROTO, ZOD_CENSUS_HIT */
	unsigned char drawn;      /* drawn to the screen at least once */
};

static struct reg_entry *reg = 0;

struct group_stat
{
	char name[GROUP_NAME];
	unsigned long images, pixels;
	int max_w, max_h;
	unsigned long bucket[6];      /* largest side <=16, 32, 64, 128, 256, more */
	unsigned long drawn, drawn_px;  /* working set: images drawn at least once */
	unsigned long derived[3];     /* roto, hit: copies made */
	unsigned long long derived_px[3];
};

static struct group_stat groups[MAX_GROUPS];
static int group_count = 0;

static unsigned long reg_hash(const SDL_Surface *s)
{
	uintptr_t p = (uintptr_t)s;

	return (unsigned long)(((p >> 4) * 2654435761UL) & (REG_SIZE - 1));
}

static struct reg_entry *reg_find(const SDL_Surface *s)
{
	unsigned long i, n;

	if(!reg || !s) return 0;

	for(i = reg_hash(s), n = 0; n < REG_SIZE; i = (i + 1) & (REG_SIZE - 1), n++)
	{
		if(reg[i].s == s) return &reg[i];
		if(!reg[i].s) return 0;
	}

	return 0;
}

static struct reg_entry *reg_insert(SDL_Surface *s)
{
	unsigned long i, n;
	struct reg_entry *tomb = 0;

	if(!reg || !s) return 0;

	for(i = reg_hash(s), n = 0; n < REG_SIZE; i = (i + 1) & (REG_SIZE - 1), n++)
	{
		if(reg[i].s == s) return &reg[i];
		if(reg[i].s == REG_TOMB && !tomb) tomb = &reg[i];
		if(!reg[i].s)
		{
			struct reg_entry *e = tomb ? tomb : &reg[i];

			memset(e, 0, sizeof(*e));
			e->s = s;
			return e;
		}
	}

	if(tomb)
	{
		memset(tomb, 0, sizeof(*tomb));
		tomb->s = s;
	}

	return tomb;
}

/* "assets/units/robots/grunt.png" -> "units"; no "assets/" -> first part. */
static int group_of(const char *filename)
{
	char name[GROUP_NAME];
	const char *p = filename ? filename : "";
	int n = 0, i;

	if(!strncmp(p, "assets/", 7)) p += 7;

	while(p[n] && p[n] != '/' && n < GROUP_NAME - 1)
	{
		name[n] = p[n];
		n++;
	}

	name[n] = 0;

	/* a bare file name (no folder) */
	if(!p[n] || p[n] != '/') strcpy(name, "(top)");
	if(!filename || !*filename) strcpy(name, "(unnamed)");

	for(i = 0; i < group_count; i++)
		if(!strcmp(groups[i].name, name)) return i + 1;

	if(group_count >= MAX_GROUPS) return MAX_GROUPS;

	strcpy(groups[group_count].name, name);
	group_count++;

	return group_count;
}

/* ---- per-frame counters -------------------------------------------------- */

enum
{
	F_BLITS, F_BLIT_PX, F_FILLS, F_FILL_PX,
	F_BASE, F_ROTO, F_HIT, F_DYN,
	F_SRC_DISTINCT, F_SRC_SWITCH, F_PAGE_SWITCH, F_PAGES,
	F_OFF_BLITS, F_OFF_FILLS, F_OFF_PX, F_OFF_DST,
	F_COUNT
};

static const char *const f_name[F_COUNT] =
{
	"screen blits", "screen blit pixels", "screen fills", "screen fill pixels",
	"  from loaded images", "  from rotated/scaled copies", "  from hit silhouettes",
	"  from dynamic surfaces",
	"distinct sources", "source changes in draw order",
	"page changes in draw order (one page set per group)",
	"distinct pages (groups + dynamic surfaces)",
	"off-screen blits", "off-screen fills", "off-screen pixels",
	"distinct off-screen targets"
};

static unsigned long frame_id = 1;
static unsigned long frames = 0;
static unsigned long long f_now[F_COUNT];
static unsigned long long f_sum[F_COUNT];
static unsigned long long f_max[F_COUNT];

static const SDL_Surface *last_src = 0;
static uintptr_t last_page = 0;
static unsigned long long group_bits = 0;  /* groups drawn this frame */
static const SDL_Surface *dyn_seen[MAX_DYN];
static int dyn_count = 0;
static const SDL_Surface *off_seen[MAX_DYN];
static int off_count = 0;
static int big_dyn_w = 0, big_dyn_h = 0;   /* largest dynamic source drawn */

static int seen_add(const SDL_Surface **list, int *count, const SDL_Surface *s)
{
	int i;

	for(i = 0; i < *count; i++)
		if(list[i] == s) return 0;

	if(*count < MAX_DYN) list[(*count)++] = s;

	return 1;
}

void zod_census_init(void)
{
	if(!zod_env_switch("ZOD_CENSUS", 0)) return;

	reg = (struct reg_entry *)calloc(REG_SIZE, sizeof(struct reg_entry));

	if(!reg)
	{
		ZLOG("Census: no memory for the registry -- off\n");
		return;
	}

	zod_census_on = 1;
	ZLOG("Census: on (ZOD_CENSUS) -- report at the end of a -B run\n");
}

void zod_census_image(SDL_Surface *s, const char *filename)
{
	struct reg_entry *e;
	struct group_stat *g;
	int side;

	if(!zod_census_on || !s) return;

	e = reg_insert(s);
	if(!e) return;

	e->group = (short)group_of(filename);
	e->kind = 0;

	g = &groups[e->group - 1];
	g->images++;
	g->pixels += (unsigned long)s->w * s->h;
	if(s->w > g->max_w) g->max_w = s->w;
	if(s->h > g->max_h) g->max_h = s->h;

	side = s->w > s->h ? s->w : s->h;
	g->bucket[side <= 16 ? 0 : side <= 32 ? 1 : side <= 64 ? 2 :
	          side <= 128 ? 3 : side <= 256 ? 4 : 5]++;
}

void zod_census_derived(SDL_Surface *s, SDL_Surface *base, int kind)
{
	struct reg_entry *b, *e;

	if(!zod_census_on || !s) return;

	b = reg_find(base);
	e = reg_insert(s);
	if(!e) return;

	e->group = b ? b->group : 0;
	e->kind = (unsigned char)kind;

	if(e->group > 0 && kind > 0 && kind < 3)
	{
		groups[e->group - 1].derived[kind]++;
		groups[e->group - 1].derived_px[kind] += (unsigned long long)s->w * s->h;
	}
}

void zod_census_free(SDL_Surface *s)
{
	struct reg_entry *e;

	if(!zod_census_on) return;

	e = reg_find(s);
	if(e) e->s = REG_TOMB;
}

void zod_census_blit(SDL_Surface *src, SDL_Surface *dst, int w, int h)
{
	const unsigned long px = (unsigned long)w * (unsigned long)h;
	struct reg_entry *e;
	uintptr_t page;

	if(!zod_census_on) return;

	if(dst != SDL_GetVideoSurface())
	{
		f_now[F_OFF_BLITS]++;
		f_now[F_OFF_PX] += px;
		if(seen_add(off_seen, &off_count, dst)) f_now[F_OFF_DST]++;
		return;
	}

	f_now[F_BLITS]++;
	f_now[F_BLIT_PX] += px;

	e = reg_find(src);

	if(e && e->group > 0)
	{
		f_now[e->kind == ZOD_CENSUS_ROTO ? F_ROTO :
		      e->kind == ZOD_CENSUS_HIT  ? F_HIT  : F_BASE]++;

		if(!e->drawn && e->kind == 0)
		{
			e->drawn = 1;
			groups[e->group - 1].drawn++;
			groups[e->group - 1].drawn_px += (unsigned long)src->w * src->h;
		}

		if(e->stamp != frame_id)
		{
			e->stamp = frame_id;
			f_now[F_SRC_DISTINCT]++;
		}

		page = (uintptr_t)e->group;
		group_bits |= 1ULL << (e->group & 63);
	}
	else
	{
		f_now[F_DYN]++;

		if(seen_add(dyn_seen, &dyn_count, src))
		{
			f_now[F_SRC_DISTINCT]++;

			if(src->w * src->h > big_dyn_w * big_dyn_h)
			{
				big_dyn_w = src->w;
				big_dyn_h = src->h;
			}
		}

		/* every dynamic surface is its own texture */
		page = (uintptr_t)src;
	}

	if(src != last_src) f_now[F_SRC_SWITCH]++;
	if(page != last_page) f_now[F_PAGE_SWITCH]++;

	last_src = src;
	last_page = page;
}

void zod_census_fill(SDL_Surface *dst, int w, int h)
{
	const unsigned long px = (unsigned long)w * (unsigned long)h;

	if(!zod_census_on) return;

	if(dst != SDL_GetVideoSurface())
	{
		f_now[F_OFF_FILLS]++;
		f_now[F_OFF_PX] += px;
		if(seen_add(off_seen, &off_count, dst)) f_now[F_OFF_DST]++;
		return;
	}

	/* fills use the white texel every page carries: no page change */
	f_now[F_FILLS]++;
	f_now[F_FILL_PX] += px;
}

void zod_census_frame(void)
{
	int i, bits = 0;
	unsigned long long g;

	if(!zod_census_on) return;

	for(g = group_bits; g; g &= g - 1) bits++;
	f_now[F_PAGES] = (unsigned long long)bits + (unsigned long long)dyn_count;

	for(i = 0; i < F_COUNT; i++)
	{
		f_sum[i] += f_now[i];
		if(f_now[i] > f_max[i]) f_max[i] = f_now[i];
		f_now[i] = 0;
	}

	frames++;
	frame_id++;
	last_src = 0;
	last_page = 0;
	group_bits = 0;
	dyn_count = 0;
	off_count = 0;
}

void zod_census_report(void)
{
	int i, b;
	unsigned long images = 0, pixels = 0, drawn = 0, drawn_px = 0;

	if(!zod_census_on) return;

	ZLOG("Census: loaded images by group (pixels at 1 byte; GL textures 2 bytes a texel)\n");

	for(i = 0; i < group_count; i++)
	{
		const struct group_stat *g = &groups[i];

		ZLOG("  %-12s %5lu images %8lu px  drawn %5lu (%lu px)  largest %ldx%ld  sides <=16:%lu <=32:%lu <=64:%lu <=128:%lu <=256:%lu more:%lu  rotated copies %lu (%lu px)  hit copies %lu\n",
		     g->name, g->images, g->pixels, g->drawn, g->drawn_px, (long)g->max_w, (long)g->max_h,
		     g->bucket[0], g->bucket[1], g->bucket[2], g->bucket[3], g->bucket[4], g->bucket[5],
		     g->derived[ZOD_CENSUS_ROTO], (unsigned long)g->derived_px[ZOD_CENSUS_ROTO],
		     g->derived[ZOD_CENSUS_HIT]);
		images += g->images;
		pixels += g->pixels;
		drawn += g->drawn;
		drawn_px += g->drawn_px;
	}

	ZLOG("  total        %5lu images %8lu px  drawn %5lu (%lu px)\n", images, pixels, drawn, drawn_px);
	ZLOG("Census: largest dynamic source drawn to the screen: %ldx%ld\n",
	     (long)big_dyn_w, (long)big_dyn_h);
	ZLOG("Census: per frame over %lu frames (mean x10, max)\n", frames);

	for(b = 0; b < F_COUNT; b++)
	{
		const unsigned long long mean10 = frames ? (f_sum[b] * 10ULL) / frames : 0;

		ZLOG("  %-52s %8lu.%lu  max %lu\n", f_name[b],
		     (unsigned long)(mean10 / 10), (unsigned long)(mean10 % 10),
		     (unsigned long)f_max[b]);
	}
}
