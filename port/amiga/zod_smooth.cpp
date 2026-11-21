/*
 * zod_smooth.cpp -- smooth scrolling under RadeonGL's panned buffers.
 * See zod_smooth.h and docs/concept-smooth-scrolling.md.
 *
 * How RadeonGL shows a frame (the MiniGL session, 20.11.): on the pan path
 * the frame kicked in mglSwitchDisplay k is panned to at the START of
 * mglSwitchDisplay k+1 and is visible from the first blank after that.
 * mglSwitchDisplay blocks only in PanPace, while the buffer it must draw
 * into next is still on display. Nothing public tells a client the refresh
 * period or when a blank comes, so it is measured here once: empty frames
 * switched as fast as PanPace takes them return within ~1 ms after a
 * blank. A hold may also end by the library's safety bound (~17.4 ms on
 * 640x480@60) or skip a refresh -- hence the median of the gaps and a fit
 * to the lower envelope of the return times. The phase is good to ~1 ms,
 * so the wait ends 2 ms before the blank a frame is due on.
 *
 * A wrong period by a few us makes the call drift against the true blank;
 * every frame then lands on the same later blank, the rhythm stays even,
 * and only the frame where the call crosses a blank is shown once more
 * (MiniGL: "a repeated frame now and then").
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <devices/timer.h>

#include <proto/exec.h>

#include <SDL/SDL_opengl.h>

#include "zod_smooth.h"
#include "zod_log.h"
#include "amiga_startup.h"
#include "fineclock.h"
#include "../../upstream/ZodEgine_Libs/QZod_DnSeparate/common.h"

enum
{
	CAL_FRAMES = 128,       /* the measuring run: ~2 s at 60 Hz (on by default:
	                           a black screen at every GL start) */
	CAL_SKIP = 8,           /* the first switches fill the buffers */
	WORK_WIN = 120,         /* frames looked at to choose N */
	HIST = 8                /* refreshes a frame stayed up: 1..7, more */
};

static int mode = -1;               /* 0 aus, 1 scroll, 2 always, 3 messen */
static int fix_n = 2;               /* ZOD_SMOOTH_N: 1..4 fixed (default 2), 0 = chosen (auto) */
static int clock_ok = 0;            /* the blank clock is measured */
static double per = 0.0;            /* E-clock ticks a refresh */
static unsigned long base_i = 0;    /* a blank: E-clock ticks ... */
static double base_f = 0.0;         /* ... and the fraction of one */
static unsigned long freq = 0;
static unsigned long margin = 0;    /* ticks the call comes before the blank */
static unsigned long guard = 0;     /* a call this close before a blank misses it */

static struct MsgPort *t_port = 0;
static struct timerequest *t_req = 0;
static int t_open = 0;

/* the frames */
static int have_prev = 0;
static unsigned long shown_i = 0;   /* the blank of the frame panned last */
static double shown_f = 0.0;
static unsigned long after_t = 0;   /* when the last switch returned */
static unsigned long pace_until = 0;
static int pacing_now = 0;          /* the rhythm holds this frame */
static int n_cur = 2;
static int cur_step = 0, kicked_step = 0, kicked_paced = 0;
static unsigned long work_ring[WORK_WIN];
static int work_n = 0, work_pos = 0;

/* statistics */
static unsigned long st_frames = 0, st_paced = 0, st_slept = 0, st_sleep_ticks = 0;
static unsigned long st_late = 0, st_n_up = 0, st_n_down = 0;
static unsigned long st_hist_all[HIST], st_hist_scroll[HIST];
static unsigned long st_scroll_frames = 0;
static double st_ppr_sum = 0.0, st_ppr_sq = 0.0;   /* px per refresh, scrolling frames */
static unsigned long st_ppr_bins[12];              /* 0..10 px per refresh, more */
static unsigned long st_n_frames[5];               /* frames by N (1..4) */

/* -X on|off (the launcher's "Smooth scrolling"): 1, 0; -1 not given */
int zod_smooth_option = -1;

static int read_mode(void)
{
	char buf[16];

	if(zod_smooth_option >= 0) return zod_smooth_option ? 1 : 0;
	const char *e = zod_env("ZOD_SMOOTH", buf, sizeof(buf));

	/* on by default while the view moves (the user's eye test, 21.11.:
	 * N = 2 fixed felt better than off and than the automatic N);
	 * ZOD_SMOOTH=off (or -X off) switches it off; the other words: on,
	 * scroll, always, measure (the old aus / immer / messen still read) */
	if(!e || !*e) return 1;
	if(e[0] == 'a' && e[1] == 'u' && e[2] == 's') return 0;   /* aus */
	if(e[0] == '0' || (e[0] == 'o' && e[1] == 'f')) return 0;
	if(e[0] == 's' || e[0] == 'S') return 1;
	if(e[0] == 'i' || e[0] == 'I' || (e[0] == 'a' && e[1] == 'l')) return 2;   /* immer, always */
	if(e[0] == 'm' || e[0] == 'M') return 3;
	if(e[0] == 'e' || e[0] == 'E' || e[0] == '1' || (e[0] == 'o' && e[1] == 'n')) return 1;

	return 0;
}

static void timer_close(void)
{
	if(t_open) { CloseDevice((struct IORequest *)t_req); t_open = 0; }
	if(t_req) { DeleteIORequest((struct IORequest *)t_req); t_req = 0; }
	if(t_port) { DeleteMsgPort(t_port); t_port = 0; }
}

static int timer_ready(void)
{
	if(t_open) return 1;

	t_port = CreateMsgPort();
	if(!t_port) return 0;

	t_req = (struct timerequest *)CreateIORequest(t_port, sizeof(struct timerequest));
	if(!t_req) { timer_close(); return 0; }

	if(OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)t_req, 0) != 0)
	{
		DeleteIORequest((struct IORequest *)t_req);
		t_req = 0;
		timer_close();
		return 0;
	}

	t_open = 1;
	return 1;
}

/* sleep for d E-clock ticks (the task waits; nothing spins) */
static void sleep_ticks(unsigned long d)
{
	unsigned long us;

	if(!t_open || freq < 1000) return;

	us = (unsigned long)((double)d * 1000000.0 / (double)freq);
	if(us < 200) return;

	t_req->tr_node.io_Command = TR_ADDREQUEST;
	t_req->tr_time.tv_secs = us / 1000000UL;
	t_req->tr_time.tv_micro = us % 1000000UL;
	DoIO((struct IORequest *)t_req);
}

/* the first blank after t + guard, as ticks and a fraction */
static void blank_after(unsigned long t, unsigned long *bi, double *bf)
{
	const double d = (double)(long)(t + guard - base_i) - base_f;
	const double k = floor(d / per) + 1.0;
	const double x = k * per + base_f;
	const double xi = floor(x);

	*bi = base_i + (unsigned long)(long)xi;
	*bf = x - xi;
}

/* a blank n refreshes after (bi, bf) */
static void blank_plus(unsigned long bi, double bf, double n, unsigned long *oi, double *of)
{
	const double x = bf + n * per;
	const double xi = floor(x);

	*oi = bi + (unsigned long)(long)xi;
	*of = x - xi;
}

static int cmp_ul(const void *a, const void *b)
{
	const unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

static int cmp_d(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

/* ---- the scroll clock ----------------------------------------------------- */

/* the blank the frame being drawn will be shown on, in current_time()'s
 * seconds: the last call fixed the blank of the frame two before it */
static double clock_hook(void)
{
	unsigned long ti, now;
	double tf;

	if(!pacing_now || !have_prev) return COMMON::current_time();

	blank_plus(shown_i, shown_f, 2.0 * n_cur, &ti, &tf);
	now = zod_fineclock_ticks();

	return COMMON::current_time() + ((double)(long)(ti - now) + tf) / (double)freq;
}

/* the view moves this frame by px (0: a scroll starts) */
static void note_hook(int px)
{
	if(px < 0) px = -px;

	cur_step += px;

	if(mode == 1 && clock_ok)
	{
		pace_until = zod_fineclock_ticks() + freq / 2;   /* and 0.5 s after it stops */

		if(!pacing_now)
		{
			pacing_now = 1;
			/* the rhythm starts from the blank last known */
		}
	}
}

/* ---- measuring the blank clock -------------------------------------------- */

static int measure_clock(void)
{
	static unsigned long t[CAL_FRAMES];
	static unsigned long gap[CAL_FRAMES];
	static double r[CAL_FRAMES], res[CAL_FRAMES], tmp[CAL_FRAMES];
	const int n = CAL_FRAMES - CAL_SKIP;
	unsigned long t0, med;
	double sr, st, srr, srt, b, a, lo, cut, minres;
	int i, m, kept, near;

	for(i = 0; i < CAL_FRAMES; i++)
	{
		glClear(GL_COLOR_BUFFER_BIT);
		mglSwitchDisplay();
		t[i] = zod_fineclock_ticks();
	}

	for(i = 1; i < n; i++) gap[i - 1] = t[CAL_SKIP + i] - t[CAL_SKIP + i - 1];
	qsort(gap, n - 1, sizeof(gap[0]), cmp_ul);
	med = gap[(n - 1) / 2];

	ZLOG("Smooth: blank clock, %d switches of an empty frame: gaps us min %lu median %lu max %lu\n",
	     n, (unsigned long)(gap[0] * 1000000.0 / freq), (unsigned long)(med * 1000000.0 / freq),
	     (unsigned long)(gap[n - 2] * 1000000.0 / freq));

	/* 8..40 ms a refresh, or the library does not hold (FS-UAE's null card) */
	if((double)med < 0.008 * freq || (double)med > 0.040 * freq)
	{
		ZLOG("Smooth: no blank clock (median gap %lu us) -- smooth off\n",
		     (unsigned long)(med * 1000000.0 / freq));
		return 0;
	}

	/* each return's refresh number: a gap of two refreshes is two */
	t0 = t[CAL_SKIP];
	r[0] = 0.0;
	for(i = 1; i < n; i++)
	{
		double k = floor((double)(t[CAL_SKIP + i] - t[CAL_SKIP + i - 1]) / (double)med + 0.5);

		if(k < 1.0) k = 1.0;
		r[i] = r[i - 1] + k;
	}

	/* a straight line through all, then through the lower half again: the
	 * returns come at a blank or after it, never before */
	b = (double)med;
	a = 0.0;
	for(m = 0; m < 2; m++)
	{
		sr = st = srr = srt = 0.0;
		kept = 0;

		if(m == 1)
		{
			for(i = 0; i < n; i++) tmp[i] = res[i];
			qsort(tmp, n, sizeof(tmp[0]), cmp_d);
			cut = tmp[n / 2];
		}
		else
			cut = 1e30;

		for(i = 0; i < n; i++)
		{
			const double y = (double)(long)(t[CAL_SKIP + i] - t0);

			if(m == 1 && res[i] > cut) continue;

			sr += r[i]; st += y; srr += r[i] * r[i]; srt += r[i] * y;
			kept++;
		}

		if(kept < n / 4) return 0;

		b = (kept * srt - sr * st) / (kept * srr - sr * sr);
		a = (st - b * sr) / kept;

		for(i = 0; i < n; i++)
			res[i] = (double)(long)(t[CAL_SKIP + i] - t0) - (a + b * r[i]);
	}

	/* the blank: the early returns -- a low percentile, not the earliest
	 * one: a single return out of line (a hold that ended early after a
	 * system hiccup, Hamiller 20.11.) must not move the phase */
	for(i = 0; i < n; i++) tmp[i] = res[i];
	qsort(tmp, n, sizeof(tmp[0]), cmp_d);
	minres = tmp[n / 5];

	lo = 0.002 * freq;
	near = 0;
	for(i = 0; i < n; i++) if(res[i] - minres < lo && res[i] - minres > -lo) near++;

	if(b < 0.008 * freq || b > 0.040 * freq || near < (n * 3) / 5)
	{
		ZLOG("Smooth: the blank clock does not fit (period %ld us, %d of %d returns within 2 ms "
		     "of the blank) -- smooth off\n", (long)(b * 1000000.0 / freq), near, n);
		return 0;
	}

	per = b;
	{
		const double x = a + minres;
		const double xi = floor(x);

		base_i = t0 + (unsigned long)(long)xi;
		base_f = x - xi;
	}

	ZLOG("Smooth: blank clock measured: a refresh %ld.%02ld ms (%ld.%02ld Hz), %d of %d returns "
	     "within 2 ms of the blank\n",
	     (long)(per * 1000.0 / freq), (long)(per * 100000.0 / freq) % 100,
	     (long)(freq / per), (long)(freq * 100.0 / per) % 100, near, n);

	return 1;
}

/* ---- the frames ----------------------------------------------------------- */

static void choose_n(unsigned long work)
{
	int i, over_now, fits_less;

	work_ring[work_pos] = work;
	work_pos = (work_pos + 1) % WORK_WIN;
	if(work_n < WORK_WIN) work_n++;

	if(fix_n) { n_cur = fix_n; return; }

	/* up: a fifth of the last 30 frames did not fit N. A single spike far
	 * over it (a path search, a bot's long tick: Hamiller has 0.5..1.5 s
	 * ones) is late at any N and says nothing about N + 1: not counted.
	 * (Hamiller 20.11., the first rule -- two of 30 -- climbed to N = 4,
	 * 12.6 fps, and never came back.) */
	if(work_n >= 30 && n_cur < 4)
	{
		over_now = 0;
		for(i = 0; i < 30; i++)
		{
			const unsigned long w = work_ring[(work_pos - 1 - i + WORK_WIN) % WORK_WIN];

			if((double)w > n_cur * per - margin && (double)w < (n_cur + 2) * per) over_now++;
		}

		if(over_now >= 6)
		{
			n_cur++;
			st_n_up++;
			work_n = 0;
			return;
		}
	}

	/* down: 95 % of the last WORK_WIN frames would have fit N - 1 */
	if(n_cur > 1 && work_n == WORK_WIN)
	{
		fits_less = 0;
		for(i = 0; i < WORK_WIN; i++)
			if((double)work_ring[i] < (n_cur - 1) * per - margin - 0.0015 * freq) fits_less++;

		if(fits_less >= (WORK_WIN * 19) / 20)
		{
			n_cur--;
			st_n_down++;
			work_n = 0;
		}
	}
}

void zod_smooth_before_switch(void)
{
	unsigned long now, call, si;
	double sf;
	int paced;

	if(!clock_ok)
		return;

	now = zod_fineclock_ticks();
	st_frames++;

	if(after_t) choose_n(now - after_t);

	if(mode == 2) pacing_now = 1;
	else if(mode == 1 && pacing_now && (long)(now - pace_until) > 0) pacing_now = 0;

	paced = pacing_now;

	if(paced && have_prev)
	{
		unsigned long ti;
		double tf;
		long left;

		/* the frame panned now is due N refreshes after the last one */
		blank_plus(shown_i, shown_f, (double)n_cur, &ti, &tf);
		left = (long)(ti - margin - now);

		if(left > (long)(freq / 2000))
		{
			const unsigned long s0 = zod_fineclock_ticks();

			sleep_ticks((unsigned long)left);
			st_slept++;
			st_sleep_ticks += zod_fineclock_ticks() - s0;
		}

		st_paced++;
		if(n_cur >= 1 && n_cur <= 4) st_n_frames[n_cur]++;
	}

	call = zod_fineclock_ticks();
	blank_after(call, &si, &sf);

	if(have_prev)
	{
		const double gap = ((double)(long)(si - shown_i) + (sf - shown_f)) / per;
		int k = (int)floor(gap + 0.5);

		if(k < 1) k = 1;

		st_hist_all[k < HIST ? k - 1 : HIST - 1]++;

		if(kicked_paced && k > n_cur) st_late++;

		/* the frame panned now moved the view by kicked_step: per refresh */
		if(kicked_step > 0)
		{
			const double ppr = (double)kicked_step / k;
			int bin = (int)ppr;

			st_hist_scroll[k < HIST ? k - 1 : HIST - 1]++;
			st_scroll_frames++;
			st_ppr_sum += ppr;
			st_ppr_sq += ppr * ppr;
			st_ppr_bins[bin < 11 ? bin : 11]++;
		}
	}

	shown_i = si;
	shown_f = sf;
	have_prev = 1;

	/* the grid goes on from this blank: the E-clock's low longword stays a
	 * short signed difference from it however long the game runs */
	base_i = si;
	base_f = sf;

	kicked_step = cur_step;
	kicked_paced = paced;
	cur_step = 0;
}

void zod_smooth_after_switch(void)
{
	if(clock_ok) after_t = zod_fineclock_ticks();
}

/* ---- open, close, report -------------------------------------------------- */

void zod_smooth_open(void)
{
	char buf[16];
	const char *e;

	if(mode < 0) mode = read_mode();
	if(!mode) return;

	e = zod_env("ZOD_SMOOTH_N", buf, sizeof(buf));
	/* N = 2 fixed by default (the user's eye test); "auto" picks it from the
	 * frames' work -- it flipped 17 times in 150 s on Hamiller and felt
	 * "far worse" */
	fix_n = 2;
	if(e && e[0] >= '1' && e[0] <= '4') fix_n = e[0] - '0';
	else if(e && (e[0] == 'a' || e[0] == 'A')) fix_n = 0;
	if(fix_n) n_cur = fix_n;

	freq = zod_fineclock_freq();
	if(freq < 1000)
	{
		ZLOG("Smooth: no E-clock -- smooth off\n");
		mode = 0;
		return;
	}

	margin = freq / 500;     /* 2 ms */
	guard = freq / 2000;     /* 0.5 ms */

	if(mode != 3 && !timer_ready())
	{
		ZLOG("Smooth: no timer.device request -- smooth off\n");
		mode = 0;
		return;
	}

	/* ZOD_SMOOTH_HZ=<rate>: no measuring, a nominal clock from now -- for
	 * FS-UAE, whose null card holds no pan, so the paced path runs there */
	e = zod_env("ZOD_SMOOTH_HZ", buf, sizeof(buf));
	if(e && atoi(e) >= 25 && atoi(e) <= 125)
	{
		per = (double)freq / atoi(e);
		base_i = zod_fineclock_ticks();
		base_f = 0.0;
		clock_ok = 1;
		ZLOG("Smooth: a nominal blank clock of %d Hz (ZOD_SMOOTH_HZ), not measured\n", atoi(e));
	}
	else
	{
		int tries;

		/* a measurement disturbed by the system (a disk, the network) is
		 * taken again, at most three times */
		for(tries = 0; tries < 3 && !clock_ok; tries++)
			clock_ok = measure_clock();
	}

	if(!clock_ok)
	{
		timer_close();
		mode = 0;
		return;
	}

	memset(st_hist_all, 0, sizeof(st_hist_all));
	memset(st_hist_scroll, 0, sizeof(st_hist_scroll));
	have_prev = 0;
	after_t = 0;
	pacing_now = (mode == 2);

	/* "messen" counts the steps but keeps the wall clock */
	COMMON::scroll_note_hook = note_hook;
	if(mode != 3) COMMON::scroll_clock_hook = clock_hook;

	ZLOG("Smooth: %s, N %s%d\n",
	     mode == 1 ? "while the view moves (ZOD_SMOOTH scroll)" :
	     mode == 2 ? "always (ZOD_SMOOTH always)" : "measuring only (ZOD_SMOOTH measure)",
	     fix_n ? "fixed " : "chosen, now ", n_cur);
}

void zod_smooth_close(void)
{
	COMMON::scroll_clock_hook = 0;
	COMMON::scroll_note_hook = 0;
	clock_ok = 0;
	pacing_now = 0;
	timer_close();
}

void zod_smooth_report(void)
{
	int i;

	if(!st_frames) return;

	ZLOG("Smooth (%s): %lu frames, %lu in the rhythm (N=1 %lu, N=2 %lu, N=3 %lu, N=4 %lu), "
	     "%lu late for their blank, N up %lu down %lu; slept %lu times, %lu us a sleep\n",
	     mode == 1 ? "scroll" : mode == 2 ? "always" : "messen",
	     st_frames, st_paced, st_n_frames[1], st_n_frames[2], st_n_frames[3], st_n_frames[4],
	     st_late, st_n_up, st_n_down, st_slept,
	     st_slept ? (unsigned long)(st_sleep_ticks * 1000000.0 / freq / st_slept) : 0UL);

	ZLOG("  refreshes a frame stayed up, all frames: 1..7, more:");
	for(i = 0; i < HIST; i++) ZLOG(" %lu", st_hist_all[i]);
	ZLOG("\n  the same, frames that moved the view:");
	for(i = 0; i < HIST; i++) ZLOG(" %lu", st_hist_scroll[i]);
	ZLOG("\n");

	if(st_scroll_frames)
	{
		const double mean = st_ppr_sum / st_scroll_frames;
		const double var = st_ppr_sq / st_scroll_frames - mean * mean;

		ZLOG("  view pixels per refresh while it moved: mean %ld.%02ld, spread (sd) %ld.%02ld; "
		     "0..10 px, more:",
		     (long)mean, (long)(mean * 100.0) % 100,
		     (long)sqrt(var > 0 ? var : 0), (long)(sqrt(var > 0 ? var : 0) * 100.0) % 100);
		for(i = 0; i < 12; i++) ZLOG(" %lu", st_ppr_bins[i]);
		ZLOG("\n");
	}
}
