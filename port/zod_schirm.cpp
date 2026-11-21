/* Vollbilder des Originals anzeigen. Begruendung: zod_schirm.h */

#include "zod_schirm.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL/SDL.h>

#include "zod_pack.h"
#include "zod_log.h"
#include "zod_palette.h"
#include "fineclock.h"
#include "amiga_startup.h"   /* zod_env */

#ifdef __amigaos__
/* aus port/amiga/sdl_screen.cpp -- die Schirmpalette wirklich setzen
 * (LoadRGB32 auf den ViewPort) und wieder zuruecknehmen. */
extern "C" void zod_sdl_set_shared_palette(const SDL_Color *colors);
extern "C" void zod_sdl_palette_changed(void);
extern "C" void zod_sdl_reload_palette(void);

/* aus port/amiga/sdl_video.cpp -- DAS GANZE Bild als schmutzig vermerken.
 *
 * SDL_Flip kopiert auf dem Amiga nur die vermerkten Bloecke. Dieses Modul
 * schreibt aber DIREKT in screen->pixels und geht damit an SDL_FillRect
 * und am Blitter vorbei -- ohne diesen Vermerk landet nichts davon im
 * Grafikspeicher.
 *
 * Beim Ladebild fiel das nicht auf: der Schirm ist da frisch geoeffnet und
 * ohnehin ganz schmutzig. Am Rundenende schon -- der Nutzer meldete, das
 * Spiel schalte "direkt zum Lose Video, waehrend noch die Karte aktiv
 * ist", und genau das war es: der Statistikbildschirm wurde gezeichnet
 * und nie ausgegeben.
 *
 * Dieselbe Falle gibt es bei ZSDL_FillDots. */
extern "C" void zod_dirty_force_full(void);

/* aus port/amiga/sdl_screen.cpp -- Systemzeiger zeigen/verstecken. */
extern "C" void zod_schirm_zeiger_amiga(int an);

/* the shared palette's colours (sdl_video.cpp): a SHARED8 glyph's surface
 * points at them -- and while a picture stands they are the picture's */
extern "C" const SDL_Color *zod_sdl_shared_palette(void);

/* one rectangle of the screen changed (sdl_video.cpp: the software path's
 * block copy; sdl_screen.cpp: GL re-uploads only that part) */
extern "C" void zod_dirty_mark(void *dstv, int x, int y, int w, int h);
extern "C" void zod_sdl_flip_teil(int x, int y, int w, int h);
#else
static void zod_dirty_force_full(void) {}
#endif

namespace
{

int steht = 0;          /* eine fremde Palette ist gesetzt */

/* Das zuletzt gezeigte Bild bleibt liegen -- zod_schirm_putzen holt
 * daraus einen Ausschnitt zurueck. Ohne das bliebe jede ueberschriebene
 * Schrift als Schmutz stehen, und die Knoepfe koennten ihre Helligkeit
 * nicht wechseln. Freigegeben in zod_schirm_ende. */
SDL_Surface *letzt_bild = 0;

/* Lage und Groesse des zuletzt gezeigten Bildes -- damit zod_schirm_text
 * in BILDkoordinaten rechnen kann und die Anordnung nicht an der
 * Aufloesung haengt. */
int letzt_f = 1, letzt_x = 0, letzt_y = 0;

/* Die Palette des zuletzt gezeigten Bildes. Der Text wird dorthin
 * umgesetzt. */
SDL_Color bild_farben[256];
SDL_Palette bild_palette = { 256, bild_farben };

/* Planetennamen in der Reihenfolge von planet_type (constants.h).
 * Bewusst hier und nicht aus der Engine geholt: dieses Modul soll ohne
 * Engine-Koepfe uebersetzbar bleiben. Die Reihenfolge ist durch das
 * Archiv festgelegt, das ZExtract schreibt -- beide Tabellen stehen in
 * tools/cd/amiga/schirme.c und hier, und sie muessen zusammenpassen. */
const char *planet_name[5] =
	{ "desert", "volcanic", "arctic", "jungle", "city" };

/* Teamfarben. Die Reihenfolge ist die von team_type (RED, BLUE, GREEN,
 * YELLOW, ...). Alles dahinter gibt es im Original nicht. */
const char *team_name[4] = { "red", "blue", "green", "yellow" };

/* --- Schrift --------------------------------------------------------
 *
 * Die Glyphen liegen als assets/fonts/<satz>/char_NNN.png im Archiv
 * (NNN = ASCII-Code) und sind in die GEMEINSAME Palette indiziert. Auf
 * dem Schirm steht waehrend eines Vollbildes aber dessen eigene Palette
 * -- die Indizes muessen also umgesetzt werden, sonst kaeme die Schrift
 * in beliebigen Farben.
 *
 * Die Umsetztabelle erhaelt die SCHATTIERUNG: eine flache Maske (alles,
 * was nicht 0 ist, weiss) saehe nach Schreibmaschine aus statt nach dem
 * Original -- und die Originalschrift IST schattiert (siehe das
 * DOSBox-Bildschirmfoto des Nutzers). */
/* --- Die Originalschrift, und warum die Farbe vorher falsch war ----
 *
 * Gemessen am DOSBox-Bild des Nutzers (23.09.), nicht geraten:
 *
 * 1. ES IST EINE EINZIGE SCHRIFT, font19, in zwei Groessen.
 *    Sprite 6448+0..35 sind A..Z und 0..9, 19 Punkt hoch (Titel,
 *    Knoepfe, "LEVEL 01" und der Levelname); Sprite 6512+0..35 ist
 *    dieselbe Schrift 12 Punkt hoch (die Statistikzeilen, "LOADING").
 *    Dahinter je sechs Satzzeichen . , ? ! * : -- die kleine Fassung
 *    hat zusaetzlich einen Bindestrich.
 *    Nachgerechnet: "QUIT" 70 gegen 70 gemessen, "UNITS KILLED" 147
 *    gegen 147, "YOU LOST" 141 gegen 140,4, "VIRGIN SOLDIERS" 247
 *    gegen 248,2.
 *
 * 2. DIE FARBE STECKT IN DER PALETTE DES BILDES, NICHT IN DER SCHRIFT.
 *    Die Glyphen benutzen im Original NUR die VGA-Plaetze 1..4, und
 *    jedes dieser Vollbilder belegt genau diese vier gleich:
 *
 *        1 = (120,120,143)   2 = (156,166,176)
 *        3 = (255,255,255)   4 = (10,10,12)
 *
 *    -- eine Grauverlauf-Rampe auf Weiss plus schwarzer Schatten.
 *    (Plaetze 5..16 sind vier Teamfarben-Rampen; sie gehoeren zu den
 *    farbigen Fassungen derselben Schrift, font21..font25.)
 *
 *    Im Bestand tragen die Glyphen aber die Farben der SPRITES.RSC-
 *    Palette, also BRAUN. Die bisherige Fassung suchte dazu die
 *    naechste Farbe im Bild -- und schrieb damit braune Schrift auf
 *    das Ladebild. Genau das hat der Nutzer gemeldet.
 *
 *    Richtig ist: den ORIGINALINDEX schreiben, nicht die Farbe. Die
 *    Rueckrechnung geht ueber die vier bekannten Braunwerte; sie liegen
 *    im RGB-Wuerfel weit auseinander, die Zuordnung ist eindeutig.
 *
 * 3. Die Knoepfe sind im Original GRAU, nicht weiss (gemessen
 *    (154,154,174) = Platz 2). Das Original dimmt also die Rampe um
 *    eine Stufe. Daran haengt hier die Rueckmeldung: der Knopf unter
 *    dem Zeiger ist weiss, die uebrigen sind grau -- ohne das ist
 *    nicht zu sehen, dass sie anklickbar sind.
 */

/* Zeichenreihenfolge der Sprites, ab Satzanfang. */
const char SATZ_ZEICHEN[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?!*:-";

struct SatzInfo
{
	const char *name;    /* Name fuer zod_schirm_schrift */
	const char *ordner;  /* Ordner im Archiv */
	int basis;           /* Spritenummer von 'A' */
	int abstand;         /* Punkte zwischen zwei Zeichen */
	int leer;            /* Vorschub des Leerzeichens */
};

const SatzInfo saetze[] =
{
	/* Gemessen: gross und klein laufen mit Abstand 2, die Zeile
	 * "LOADING" auf dem Ladebild dagegen eng (79 gegen 91 Punkte
	 * gerechnet, 77,5 gemessen). Deshalb ein eigener Eintrag statt
	 * einer geratenen Mitte. */
	{ "gross",  "font19", 6448, 2, 8 },
	{ "klein",  "font19", 6512, 2, 8 },
	{ "laden",  "font19", 6512, 0, 8 },
	{ 0, 0, 0, 0, 0 }
};

/* Die vier Braunwerte der SPRITES.RSC-Palette, wie sie im Bestand
 * stehen, in der Reihenfolge der VGA-Plaetze 1..4. */
const struct { int r, g, b; } satz_farbe[4] =
{
	{  63,  31,   0 },   /* 1 */
	{  95,  63,  31 },   /* 2 */
	{ 127,  95,  63 },   /* 3 -- der Koerper der Schrift */
	{  63,  63,  95 }    /* 4 -- der Schatten */
};

char schrift_satz[16] = "gross";
int schrift_skala = 1;
int schrift_hell = 1;
const SatzInfo *schrift_info = &saetze[0];

SDL_Surface *glyph[96];          /* ASCII 32..127 */
int glyph_vor[96];               /* Vorschub, aus den deckenden Punkten */
int glyph_geholt[96];            /* schon einmal versucht */

/* Rohwert der Glyphenflaeche -> VGA-Platz 1..4. Alle Glyphen eines
 * Satzes teilen dieselbe Palette, eine Tabelle genuegt. */
unsigned char vga[256];
int vga_gut = 0;

/* Fuer fremde Saetze (nicht font19): der alte Weg ueber die naechste
 * Farbe. */
unsigned char umsetz[256];
int umsetz_gut = 0;

/* Den Platz suchen, der einer Farbe am naechsten kommt. Quadratischer
 * Abstand im RGB-Wuerfel -- fuer eine Schriftumsetzung genuegt das; eine
 * wahrnehmungsrichtige Metrik waere hier Scheingenauigkeit. */
int naechster_platz(const SDL_Palette *p, int r, int g, int b)
{
	int best = 0;
	long best_d = -1;
	int i;

	if(!p) return 0;

	for(i = 0; i < p->ncolors; i++)
	{
		long dr = (long)p->colors[i].r - r;
		long dg = (long)p->colors[i].g - g;
		long db = (long)p->colors[i].b - b;
		long d = dr * dr + dg * dg + db * db;

		if(best_d < 0 || d < best_d) { best_d = d; best = i; }
	}

	return best;
}

/* A glyph's colour i. SHARED8 glyphs (font19, green_building: format 4
 * of the archive) index the ENGINE's base palette. On the Amiga their
 * surface points at the shared palette -- and while a full screen stands,
 * that holds the PICTURE's colours (zod_schirm_zeigen sets them), so the
 * surface's own colours say nothing about the glyph. The base palette does. */
const SDL_Color &glyph_farbe(const SDL_Surface *g, int i)
{
#ifdef __amigaos__
	const SDL_Color *basis = zod_palette_colors();

	if(basis && g->format->palette && g->format->palette->colors == zod_sdl_shared_palette())
		return basis[i];
#endif
	return g->format->palette->colors[i];
}

void glyphen_weg(void)
{
	int i;

	for(i = 0; i < 96; i++)
	{
		if(glyph[i]) { SDL_FreeSurface(glyph[i]); glyph[i] = 0; }
		glyph_vor[i] = 0;
		glyph_geholt[i] = 0;
	}

	vga_gut = 0;
	umsetz_gut = 0;
}

/* Platz eines Zeichens im Satz, oder -1. */
int satz_platz(int c)
{
	const char *p;

	if(c >= 'a' && c <= 'z') c = c - 'a' + 'A';

	p = strchr(SATZ_ZEICHEN, c);

	return (p && c) ? (int)(p - SATZ_ZEICHEN) : -1;
}

SDL_Surface *glyphe(int c)
{
	char name[64];
	SDL_Surface *g;

	if(c < 32 || c > 127) c = 32;

	if(glyph_geholt[c - 32]) return glyph[c - 32];

	glyph_geholt[c - 32] = 1;

	if(schrift_info)
	{
		int k = satz_platz(c);

		if(k < 0) return 0;   /* Leerzeichen, Unbekanntes */

		snprintf(name, sizeof(name), "assets/fonts/%s/SPRITES_%d.png",
		         schrift_info->ordner, schrift_info->basis + k);
	}
	else
	{
		snprintf(name, sizeof(name), "assets/fonts/%s/char_%03d.png",
		         schrift_satz, c);
	}

	g = zod_pack_load_quiet(name);

	glyph[c - 32] = g;

	/* Der Vorschub kommt aus den DECKENDEN Punkten, nicht aus der
	 * Feldbreite: die Felder sind 24 bzw. 16 Punkte breit, die
	 * Buchstaben darin 7 bis 23. Mit der Feldbreite stuende alles
	 * viel zu weit auseinander. */
	if(g)
	{
		int x, y, rechts = -1;

		if(SDL_MUSTLOCK(g)) SDL_LockSurface(g);

		for(y = 0; y < g->h; y++)
		{
			const Uint8 *q = (const Uint8 *)g->pixels + y * g->pitch;

			for(x = g->w - 1; x > rechts; x--)
				if(q[x]) { rechts = x; break; }
		}

		if(SDL_MUSTLOCK(g)) SDL_UnlockSurface(g);

		glyph_vor[c - 32] = rechts + 1
		                  + (schrift_info ? schrift_info->abstand : 1);
	}

	return g;
}

/* Vorschub eines Zeichens in BILDpunkten (ohne Skalierung). */
int glyph_breite(int c)
{
	SDL_Surface *g = glyphe(c);

	if(g) return glyph_vor[c - 32];

	return schrift_info ? schrift_info->leer : 8;
}

/* Einen Punktblock der Groesse letzt_f setzen -- ein "Bildpunkt" des
 * 320er Bildes. */
void punkt(SDL_Surface *s, int px, int py, Uint32 farbe)
{
	int k, l;

	for(l = 0; l < letzt_f; l++)
	{
		int yy = py + l;
		Uint8 *z8;
		Uint16 *z16;
		Uint32 *z32;

		if(yy < 0 || yy >= s->h) continue;

		switch(s->format->BytesPerPixel)
		{
		case 1:
			z8 = (Uint8 *)s->pixels + yy * s->pitch + px;
			for(k = 0; k < letzt_f; k++)
				if(px + k >= 0 && px + k < s->w) z8[k] = (Uint8)farbe;
			break;
		case 2:
			z16 = (Uint16 *)((Uint8 *)s->pixels + yy * s->pitch) + px;
			for(k = 0; k < letzt_f; k++)
				if(px + k >= 0 && px + k < s->w) z16[k] = (Uint16)farbe;
			break;
		case 4:
			z32 = (Uint32 *)((Uint8 *)s->pixels + yy * s->pitch) + px;
			for(k = 0; k < letzt_f; k++)
				if(px + k >= 0 && px + k < s->w) z32[k] = farbe;
			break;
		default:
			break;
		}
	}
}

}

extern "C" {

void zod_schirm_name(char *aus, int platz, const char *anlass,
                     int planet, int team)
{
	if(!aus || platz <= 0) return;

	if(planet < 0 || planet > 4) planet = 0;
	if(team   < 0 || team   > 3) team   = 0;   /* Rueckfall auf rot */

	snprintf(aus, platz, "assets/screens/%s_%s_%s",
	         anlass ? anlass : "load", planet_name[planet], team_name[team]);
}

int zod_schirm_da(const char *name)
{
	SDL_Surface *s;

	if(!name || !*name) return 0;

	s = zod_pack_load_quiet(name);

	if(!s) return 0;

	SDL_FreeSurface(s);

	return 1;
}

void zod_schirm_schrift(const char *satz, int skala)
{
	const SatzInfo *si = 0;
	int i;

	schrift_skala = (skala > 0) ? skala : 1;

	if(!satz || !*satz) return;
	if(!strcmp(satz, schrift_satz)) return;

	for(i = 0; saetze[i].name; i++)
		if(!strcmp(saetze[i].name, satz)) { si = &saetze[i]; break; }

	glyphen_weg();

	schrift_info = si;
	strncpy(schrift_satz, satz, sizeof(schrift_satz) - 1);
	schrift_satz[sizeof(schrift_satz) - 1] = 0;
}

void zod_schirm_hell(int an)
{
	schrift_hell = an ? 1 : 0;
}

static int zeigen_bild(SDL_Surface *bild, const char *name);

int zod_schirm_zeigen(const char *name)
{
	SDL_Surface *bild;
	SDL_Surface *schirm = SDL_GetVideoSurface();

	if(!name || !*name || !schirm) return 0;

	if(letzt_bild) { SDL_FreeSurface(letzt_bild); letzt_bild = 0; }

	bild = zod_pack_load_quiet(name);

	if(!bild)
	{
		/* NICHT still zurueckkehren. Fehlt ein Bildschirm, soll das im
		 * Protokoll stehen -- sonst ist "kein Statistikbildschirm" von
		 * "Statistikbildschirm kaputt" nicht zu unterscheiden. */
		ZLOG("Schirm: %s ist nicht im Archiv\n", name);

		return 0;
	}

	if(bild->format->BitsPerPixel != 8 || !bild->format->palette)
	{
		ZLOG("Schirm: %s ist nicht 8 Bit (%d)\n", name,
		     (int)bild->format->BitsPerPixel);
		SDL_FreeSurface(bild);

		return 0;
	}

	return zeigen_bild(bild, name);
}

/* bild (8 bit, its own palette) shown full screen: it becomes letzt_bild */
static int zeigen_bild(SDL_Surface *bild, const char *name)
{
	SDL_Surface *schirm = SDL_GetVideoSurface();
	int fx, fy, f, zx, zy, x, y;

	if(!schirm) { SDL_FreeSurface(bild); return 0; }

	/* Ganzzahliger Faktor, sonst entstehen beim Vergroessern ungleich
	 * breite Spalten -- bei einem 320er Bild auf 640 faellt das sofort
	 * auf. Mindestens 1, auch wenn der Schirm kleiner ist. */
	fx = schirm->w / bild->w;
	fy = schirm->h / bild->h;
	f  = (fx < fy) ? fx : fy;

	if(f < 1) f = 1;

	zx = (schirm->w - bild->w * f) / 2;
	zy = (schirm->h - bild->h * f) / 2;

	if(zx < 0) zx = 0;
	if(zy < 0) zy = 0;

	/* Palette merken, Umsetztabelle verwerfen -- sie gilt je Bild. */
	{
		int n = bild->format->palette->ncolors;

		if(n > 256) n = 256;

		memcpy(bild_farben, bild->format->palette->colors,
		       (size_t)n * sizeof(SDL_Color));

		umsetz_gut = 0;
	}

	if(schirm->format->BitsPerPixel == 8)
	{
		/* 8 Bit: die Indizes wandern UNVERAENDERT in den Schirm, also
		 * muss die Schirmpalette die des Bildes werden. ERST die
		 * Palette, DANN zeichnen -- andersherum blitzt fuer ein Bild
		 * die alte Palette auf dem neuen Bild auf. */
#ifdef __amigaos__
		zod_sdl_set_shared_palette(bild->format->palette->colors);
		zod_sdl_palette_changed();
#else
		SDL_SetColors(schirm, bild->format->palette->colors, 0,
		              bild->format->palette->ncolors);
#endif
		steht = 1;

		SDL_FillRect(schirm, 0, 0);       /* Platz 0 ist ueberall schwarz */

		if(SDL_MUSTLOCK(schirm)) SDL_LockSurface(schirm);

		for(y = 0; y < bild->h * f; y++)
		{
			const Uint8 *q = (const Uint8 *)bild->pixels
			               + (y / f) * bild->pitch;
			Uint8 *z = (Uint8 *)schirm->pixels
			         + (zy + y) * schirm->pitch + zx;

			if(f == 1) { memcpy(z, q, bild->w); continue; }

			for(x = 0; x < bild->w; x++)
			{
				Uint8 c = q[x];
				int k;

				for(k = 0; k < f; k++) z[x * f + k] = c;
			}
		}

		if(SDL_MUSTLOCK(schirm)) SDL_UnlockSurface(schirm);
	}
	else
	{
		/* Echtfarbschirm (Host): KEIN Palettenwechsel noetig. Eine
		 * vergroesserte 8-Bit-Zwischenflaeche mit der Palette des
		 * Bildes, und SDL setzt sie beim Blitten selbst um.
		 *
		 * Dieser Zweig ist nicht nur Beiwerk: ohne ihn liesse sich der
		 * ganze Weg auf dem Host gar nicht pruefen, und geprueft wuerde
		 * erst auf dem Amiga -- also dort, wo ein Fehler teuer ist. */
		SDL_Surface *gross = SDL_CreateRGBSurface(SDL_SWSURFACE,
		                                          bild->w * f,
		                                          bild->h * f,
		                                          8, 0, 0, 0, 0);
		SDL_Rect nach;

		if(!gross) { SDL_FreeSurface(bild); return 0; }

		SDL_SetColors(gross, bild->format->palette->colors, 0,
		              bild->format->palette->ncolors);

		for(y = 0; y < gross->h; y++)
		{
			const Uint8 *q = (const Uint8 *)bild->pixels
			               + (y / f) * bild->pitch;
			Uint8 *z = (Uint8 *)gross->pixels + y * gross->pitch;

			if(f == 1) { memcpy(z, q, bild->w); continue; }

			for(x = 0; x < bild->w; x++)
			{
				Uint8 c = q[x];
				int k;

				for(k = 0; k < f; k++) z[x * f + k] = c;
			}
		}

		SDL_FillRect(schirm, 0, 0);

		nach.x = (Sint16)zx; nach.y = (Sint16)zy;
		nach.w = (Uint16)gross->w; nach.h = (Uint16)gross->h;

		SDL_BlitSurface(gross, 0, schirm, &nach);
		SDL_FreeSurface(gross);
	}

	letzt_f = f;
	letzt_x = zx;
	letzt_y = zy;

	zod_dirty_force_full();
	SDL_Flip(schirm);

	ZLOG("Schirm: %s (%dx%d, Faktor %d)\n", name, bild->w, bild->h, f);

	/* NICHT freigeben -- siehe letzt_bild. */
	letzt_bild = bild;

	return 1;
}

int zod_schirm_breite(const char *text)
{
	const char *p;
	int b = 0;

	if(!text) return 0;

	for(p = text; *p; p++)
		b += glyph_breite((unsigned char)*p) * schrift_skala;

	/* Der Abstand HINTER dem letzten Zeichen gehoert nicht zur Breite
	 * -- sonst sitzt jeder zentrierte Text um ein Halbes daneben. */
	if(b && schrift_info && glyphe((unsigned char)p[-1]))
		b -= schrift_info->abstand * schrift_skala;

	return b;
}

void zod_schirm_text(int x, int y, const char *text)
{
	SDL_Surface *schirm = SDL_GetVideoSurface();
	const char *p;
	int sx = x;

	if(!schirm || !text) return;

	if(SDL_MUSTLOCK(schirm)) SDL_LockSurface(schirm);

	for(p = text; *p; p++)
	{
		SDL_Surface *g = glyphe((unsigned char)*p);
		int gx, gy;

		if(!g) { sx += glyph_breite((unsigned char)*p) * schrift_skala; continue; }

		/* Umsetztabellen beim ersten Glyphen bauen. */
		if(g->format->palette)
		{
			int i;

			if(schrift_info && !vga_gut)
			{
				for(i = 0; i < 256; i++)
				{
					const SDL_Color &c = glyph_farbe(g, i);
					int k, best = 0;
					long best_d = -1;

					for(k = 0; k < 4; k++)
					{
						long dr = satz_farbe[k].r - (long)c.r;
						long dg = satz_farbe[k].g - (long)c.g;
						long db = satz_farbe[k].b - (long)c.b;
						long d = dr * dr + dg * dg + db * db;

						if(best_d < 0 || d < best_d)
						{ best_d = d; best = k; }
					}

					vga[i] = (unsigned char)(best + 1);
				}

				vga_gut = 1;
			}

			if(!schrift_info && !umsetz_gut)
			{
				for(i = 0; i < 256; i++)
				{
					const SDL_Color &c = glyph_farbe(g, i);

					umsetz[i] = (unsigned char)
						naechster_platz(&bild_palette, c.r, c.g, c.b);
				}

				umsetz_gut = 1;
			}
		}

		for(gy = 0; gy < g->h; gy++)
		{
			const Uint8 *q = (const Uint8 *)g->pixels + gy * g->pitch;

			for(gx = 0; gx < g->w; gx++)
			{
				Uint32 farbe;
				int platz;
				int mx, my;

				/* Platz 0 ist durchgehend der Farbschluessel. */
				if(!q[gx]) continue;

				if(schrift_info)
				{
					platz = vga_gut ? vga[q[gx]] : 3;

					/* Gedimmt: die Rampe eine Stufe herunter, der
					 * Schatten bleibt schwarz. */
					if(!schrift_hell && platz >= 2 && platz <= 3)
						platz--;
				}
				else
					platz = umsetz_gut ? umsetz[q[gx]] : q[gx];

				if(schirm->format->BitsPerPixel == 8)
					farbe = (Uint32)platz;
				else
				{
					const SDL_Color &c = bild_farben[platz & 255];

					farbe = SDL_MapRGB(schirm->format, c.r, c.g, c.b);
				}

				/* Ein Glyphenpunkt wird zu skala x skala
				 * Bildpunkten, und jeder davon zu letzt_f x
				 * letzt_f Schirmpunkten. */
				for(my = 0; my < schrift_skala; my++)
					for(mx = 0; mx < schrift_skala; mx++)
						punkt(schirm,
						      letzt_x + (sx + gx * schrift_skala + mx) * letzt_f,
						      letzt_y + (y + gy * schrift_skala + my) * letzt_f,
						      farbe);
			}
		}

		sx += glyph_vor[(unsigned char)*p - 32] * schrift_skala;
	}

	if(SDL_MUSTLOCK(schirm)) SDL_UnlockSurface(schirm);
}

void zod_schirm_putzen(int x, int y, int b, int h)
{
	SDL_Surface *schirm = SDL_GetVideoSurface();
	int gx, gy;

	if(!schirm || !letzt_bild) return;

	if(x < 0) { b += x; x = 0; }
	if(y < 0) { h += y; y = 0; }
	if(x + b > letzt_bild->w) b = letzt_bild->w - x;
	if(y + h > letzt_bild->h) h = letzt_bild->h - y;
	if(b <= 0 || h <= 0) return;

	if(SDL_MUSTLOCK(schirm)) SDL_LockSurface(schirm);

	for(gy = 0; gy < h; gy++)
	{
		const Uint8 *q = (const Uint8 *)letzt_bild->pixels
		               + (y + gy) * letzt_bild->pitch + x;

		for(gx = 0; gx < b; gx++)
		{
			Uint32 farbe;

			if(schirm->format->BitsPerPixel == 8)
				farbe = q[gx];
			else
			{
				const SDL_Color &c = bild_farben[q[gx]];

				farbe = SDL_MapRGB(schirm->format, c.r, c.g, c.b);
			}

			punkt(schirm, letzt_x + (x + gx) * letzt_f,
			      letzt_y + (y + gy) * letzt_f, farbe);
		}
	}

	if(SDL_MUSTLOCK(schirm)) SDL_UnlockSurface(schirm);
}

int zod_schirm_maus(int mx, int my, int *bx, int *by)
{
	int x = (mx - letzt_x) / (letzt_f > 0 ? letzt_f : 1);
	int y = (my - letzt_y) / (letzt_f > 0 ? letzt_f : 1);

	if(bx) *bx = x;
	if(by) *by = y;

	return (x >= 0 && x < 320 && y >= 0 && y < 200) ? 1 : 0;
}

void zod_schirm_zeiger(int an)
{
#ifdef __amigaos__
	zod_schirm_zeiger_amiga(an);
#else
	SDL_ShowCursor(an ? SDL_ENABLE : SDL_DISABLE);
#endif
}

void zod_schirm_ausgeben(void)
{
	SDL_Surface *schirm = SDL_GetVideoSurface();

	if(!schirm) return;

	zod_dirty_force_full();
	SDL_Flip(schirm);
}

void zod_schirm_ende(void)
{
	if(letzt_bild) { SDL_FreeSurface(letzt_bild); letzt_bild = 0; }

	SDL_Surface *schirm = SDL_GetVideoSurface();

	/* Die Glyphen werden nur hier gebraucht -- zwischen zwei
	 * Bildschirmen belegen sie nur Speicher, und auf dem Amiga ist der
	 * knapp. */
	glyphen_weg();
	umsetz_gut = 0;

	if(!steht) return;

	steht = 0;

	/* Schwarz, BEVOR die Palette zurueckgeht: sonst steht das Bild einen
	 * Augenblick in den falschen Farben da. Platz 0 ist in beiden
	 * Paletten schwarz, das Loeschen ist also in jedem Fall richtig. */
	if(schirm) SDL_FillRect(schirm, 0, 0);

#ifdef __amigaos__
	zod_sdl_reload_palette();
#else
	if(schirm && zod_palette_ready())
		SDL_SetColors(schirm, (SDL_Color *)zod_palette_colors(), 0, 256);
#endif

	if(schirm)
	{
		zod_dirty_force_full();
		SDL_Flip(schirm);
	}
}


/* ---- A black screen in the same frame as an original picture ------------
 *
 * Without zod_screens.zpk there is no loading picture, and the screen stood
 * black with nothing on it. A 320x200 black "picture" with a palette of its
 * own puts "LOADING" and the progress bar there in the same frame: slots
 * 1..4 are the grey ramp every original full screen holds (the font's
 * colours, see above), the rest the bar's. */
enum
{
	SW_RECESS = 5, SW_LED_AN, SW_LED_HELL, SW_LED_MATT, SW_LED_AUS, SW_KASTEN, SW_FARBEN
};

int zod_schirm_zeigen_schwarz(void)
{
	static const SDL_Color farben[SW_FARBEN] =
	{
		{   0,   0,   0, 0 },
		{ 120, 120, 143, 0 }, { 156, 166, 176, 0 }, { 255, 255, 255, 0 }, { 10, 10, 12, 0 },
		{  24,  26,  30, 0 },                                    /* the bar's recess */
		{  90, 230,  70, 0 }, { 170, 255, 150, 0 },              /* a lit LED, its highlight */
		{  40, 150,  40, 0 }, {  20,  52,  22, 0 },              /* its foot, an unlit LED */
		{  12,  20,  12, 0 }                                     /* the digits' box */
	};
	SDL_Color pal[256];
	SDL_Surface *bild;

	if(!SDL_GetVideoSurface()) return 0;

	if(letzt_bild) { SDL_FreeSurface(letzt_bild); letzt_bild = 0; }

	bild = SDL_CreateRGBSurface(SDL_SWSURFACE, 320, 200, 8, 0, 0, 0, 0);
	if(!bild) return 0;

	memset(pal, 0, sizeof(pal));
	memcpy(pal, farben, sizeof(farben));
	SDL_SetColors(bild, pal, 0, 256);
	SDL_FillRect(bild, 0, 0);

	return zeigen_bild(bild, "(black, the bar's own palette)");
}

/* ---- The load progress bar (docs/concept-load-progress.md) --------------
 *
 * Under the original "LOADING" (picture y 131): a bevelled plate in the
 * picture's grey ramp with LED segments lit in green, the share in the
 * forts' countdown digits (green_building) in a recessed box, the step in
 * font19's small set, dimmed. Picture coordinates, the picture's palette.
 * At most four updates a second; each redraws and outputs only the bar's
 * region. Every step's time goes to the log. ZOD_LADEBALKEN off: off. */
enum
{
	LB_X = 70, LB_Y = 148, LB_B = 180, LB_H = 10,   /* the plate */
	LB_SEG = 44,                                    /* LED segments */
	LB_TEXT_Y = 162,                                /* the step */
	LB_BEREICH_X = 40, LB_BEREICH_Y = 145,          /* what is redrawn */
	LB_BEREICH_B = 260, LB_BEREICH_H = 32,
	LB_LOG = 24
};

static int lade_an = -1;
static int lade_aktiv = 0;
static unsigned long lade_t0 = 0, lade_schritt_t0 = 0, lade_gezeichnet = 0;
static char lade_name[40];
static int lade_hat_gezeichnet = 0;
static struct { char name[40]; unsigned long ms; } lade_log[LB_LOG];
static int lade_nlog = 0;

/* the palette slots the bar draws with, for this picture */
static int lb_grau, lb_schatten, lb_recess, lb_an, lb_hl, lb_matt, lb_aus, lb_kasten;
static unsigned char lb_ziffer_lut[256];
static int lb_ziffer_lut_gut = 0;
static SDL_Surface *lb_ziffer[10];

/* ZOD_LADESHOT=<file>: the screen as BMP when the bar first passes 40 %
 * (<file>.40.bmp) and at the end (<file>.100.bmp) -- for the FS-UAE test
 * (tests/auto/LoadBar), the loading screen comes before any game frame */
static char lade_shot[96];
static int lade_shot_40 = 0;

static void lade_shot_speichern(const char *teil)
{
	char name[128];
	SDL_Surface *schirm = SDL_GetVideoSurface();

	if(!lade_shot[0] || !schirm) return;

	snprintf(name, sizeof(name), "%s.%s.bmp", lade_shot, teil);
	SDL_SaveBMP(schirm, name);
	ZLOG("[load] screen saved: %s\n", name);
}

static unsigned long lade_ms(void)
{
	const unsigned long f = zod_fineclock_freq();

	return f >= 1000 ? zod_fineclock_ticks() / (f / 1000UL) : 0;
}

static int lade_switch(void)
{
	if(lade_an < 0)
	{
		const char *e = getenv("ZOD_LADEBALKEN");
#ifdef __amigaos__
		char buf[16];

		e = zod_env("ZOD_LADEBALKEN", buf, sizeof(buf));
#endif
		lade_an = zod_switch_value(e, 1);
	}

	return lade_an;
}

int zod_lade_balken_an(void)
{
	return lade_switch();
}

/* one picture pixel in slot `platz` */
static void lb_punkt(SDL_Surface *schirm, int x, int y, int platz)
{
	Uint32 farbe;

	if(schirm->format->BitsPerPixel == 8)
		farbe = (Uint32)platz;
	else
	{
		const SDL_Color &c = bild_farben[platz & 255];

		farbe = SDL_MapRGB(schirm->format, c.r, c.g, c.b);
	}

	punkt(schirm, letzt_x + x * letzt_f, letzt_y + y * letzt_f, farbe);
}

static void lb_flaeche(SDL_Surface *schirm, int x, int y, int b, int h, int platz)
{
	int i, j;

	for(j = 0; j < h; j++)
		for(i = 0; i < b; i++)
			lb_punkt(schirm, x + i, y + j, platz);
}

/* a green_building digit at x, y (picture coordinates); its width */
static int lb_ziffer_zeichnen(SDL_Surface *schirm, int x, int y, int d)
{
	SDL_Surface *g = (d >= 0 && d <= 9) ? lb_ziffer[d] : 0;
	int gx, gy;

	if(!g) return 4;

	if(!lb_ziffer_lut_gut && g->format->palette)
	{
		int i;

		for(i = 0; i < 256; i++)
		{
			const SDL_Color &c = glyph_farbe(g, i);

			lb_ziffer_lut[i] = (unsigned char)naechster_platz(&bild_palette, c.r, c.g, c.b);
		}

		lb_ziffer_lut_gut = 1;
	}

	for(gy = 0; gy < g->h; gy++)
	{
		const Uint8 *q = (const Uint8 *)g->pixels + gy * g->pitch;

		for(gx = 0; gx < g->w; gx++)
			if(q[gx]) lb_punkt(schirm, x + gx, y + gy, lb_ziffer_lut_gut ? lb_ziffer_lut[q[gx]] : lb_an);
	}

	return g->w;
}

static void lade_zeichnen(int prozent)
{
	SDL_Surface *schirm = SDL_GetVideoSurface();
	int i, lit, w, x;
	char ziffern[8];

	if(!schirm || !letzt_bild) return;

	if(prozent < 0) prozent = 0;
	if(prozent > 100) prozent = 100;

	/* the picture back under the bar, the step and the digits */
	zod_schirm_putzen(LB_BEREICH_X, LB_BEREICH_Y, LB_BEREICH_B, LB_BEREICH_H);

	if(SDL_MUSTLOCK(schirm)) SDL_LockSurface(schirm);

	/* the plate: a light edge top-left, the shadow bottom-right, the recess */
	lb_flaeche(schirm, LB_X - 1, LB_Y - 1, LB_B + 2, LB_H + 2, lb_grau);
	lb_flaeche(schirm, LB_X, LB_Y, LB_B + 1, LB_H + 1, lb_schatten);
	lb_flaeche(schirm, LB_X, LB_Y, LB_B, LB_H, lb_recess);

	/* the LEDs: 44 of 4 picture pixels (3 lit, 1 gap) from x+2 */
	lit = LB_SEG * prozent / 100;

	for(i = 0; i < LB_SEG; i++)
	{
		const int sx = LB_X + 2 + i * 4;

		if(i < lit)
		{
			lb_flaeche(schirm, sx, LB_Y + 2, 3, 1, lb_hl);
			lb_flaeche(schirm, sx, LB_Y + 3, 3, LB_H - 5, lb_an);
			lb_flaeche(schirm, sx, LB_Y + LB_H - 2, 3, 1, lb_matt);
		}
		else
			lb_flaeche(schirm, sx, LB_Y + 2, 3, LB_H - 4, lb_aus);
	}

	/* the share in the countdown's digits, in a recessed box right of it */
	snprintf(ziffern, sizeof(ziffern), "%d", prozent);
	w = 0;
	for(i = 0; ziffern[i]; i++) w += (lb_ziffer[ziffern[i] - '0'] ? lb_ziffer[ziffern[i] - '0']->w : 4) + 1;

	lb_flaeche(schirm, LB_X + LB_B + 5, LB_Y - 1, w + 4, LB_H + 2, lb_grau);
	lb_flaeche(schirm, LB_X + LB_B + 6, LB_Y, w + 2, LB_H, lb_kasten);

	x = LB_X + LB_B + 7;
	for(i = 0; ziffern[i]; i++)
		x += lb_ziffer_zeichnen(schirm, x, LB_Y + 2, ziffern[i] - '0') + 1;

	if(SDL_MUSTLOCK(schirm)) SDL_UnlockSurface(schirm);

	/* the step, dimmed, centred under the bar */
	zod_schirm_schrift("klein", 1);
	zod_schirm_hell(0);
	zod_schirm_text((320 - zod_schirm_breite(lade_name)) / 2, LB_TEXT_Y, lade_name);
	zod_schirm_hell(1);

	zod_schirm_ausgeben_teil(LB_BEREICH_X, LB_BEREICH_Y, LB_BEREICH_B, LB_BEREICH_H);
	lade_hat_gezeichnet = 1;

	if(prozent >= 40 && !lade_shot_40)
	{
		lade_shot_40 = 1;
		lade_shot_speichern("40");
	}
}

void zod_lade_beginn(void)
{
	int d;

	lade_aktiv = 0;

	if(!lade_switch() || !letzt_bild) return;

	/* the picture's slots for the bar: the grey ramp is slots 1..4 in every
	 * original full screen (and in the black one); the rest the nearest */
	lb_grau = 1; lb_schatten = 4;
	lb_recess = naechster_platz(&bild_palette, 24, 26, 30);
	lb_an = naechster_platz(&bild_palette, 90, 230, 70);
	lb_hl = naechster_platz(&bild_palette, 170, 255, 150);
	lb_matt = naechster_platz(&bild_palette, 40, 150, 40);
	lb_aus = naechster_platz(&bild_palette, 20, 52, 22);
	lb_kasten = naechster_platz(&bild_palette, 12, 20, 12);

	lb_ziffer_lut_gut = 0;

	for(d = 0; d < 10; d++)
	{
		char name[64];

		snprintf(name, sizeof(name), "assets/fonts/green_building/char_%03d.png", '0' + d);
		lb_ziffer[d] = zod_pack_load_quiet(name);
	}

	{
		const char *e = getenv("ZOD_LADESHOT");
#ifdef __amigaos__
		char buf[96];

		e = zod_env("ZOD_LADESHOT", buf, sizeof(buf));
#endif
		lade_shot[0] = 0;
		if(e) { strncpy(lade_shot, e, sizeof(lade_shot) - 1); lade_shot[sizeof(lade_shot) - 1] = 0; }
		lade_shot_40 = 0;
	}

	lade_t0 = lade_schritt_t0 = lade_ms();
	lade_gezeichnet = 0;
	lade_name[0] = 0;
	lade_nlog = 0;
	lade_hat_gezeichnet = 0;
	lade_aktiv = 1;

	strcpy(lade_name, "STARTING");
	lade_zeichnen(0);
}

/* `done` of `total` steps finished; `name` is the one that just finished */
void zod_lade_schritt(const char *name, int done, int total)
{
	unsigned long jetzt;
	int neu;

	if(!lade_aktiv || !name) return;

	jetzt = lade_ms();

	/* the step's time to the log, per name */
	if(lade_nlog == 0 || strcmp(lade_log[lade_nlog - 1].name, name))
	{
		if(lade_nlog < LB_LOG)
		{
			strncpy(lade_log[lade_nlog].name, name, sizeof(lade_log[0].name) - 1);
			lade_log[lade_nlog].name[sizeof(lade_log[0].name) - 1] = 0;
			lade_log[lade_nlog].ms = 0;
			lade_nlog++;
		}
	}

	if(lade_nlog > 0) lade_log[lade_nlog - 1].ms += jetzt - lade_schritt_t0;
	lade_schritt_t0 = jetzt;

	neu = strcmp(lade_name, name) != 0;

	/* at most four times a second, and always at the last step */
	if(!neu && jetzt - lade_gezeichnet < 250 && done < total) return;
	if(neu && jetzt - lade_gezeichnet < 100 && done < total) { strncpy(lade_name, name, sizeof(lade_name) - 1); return; }

	strncpy(lade_name, name, sizeof(lade_name) - 1);
	lade_name[sizeof(lade_name) - 1] = 0;
	lade_gezeichnet = jetzt;

	lade_zeichnen(total > 0 ? (int)(100L * done / total) : 0);
}

void zod_lade_ende(void)
{
	int i, d;

	if(!lade_aktiv) return;

	lade_aktiv = 0;

	for(i = 0; i < lade_nlog; i++)
		ZLOG("[load] %-22s %6lu ms\n", lade_log[i].name, lade_log[i].ms);

	ZLOG("[load] all steps %lu ms\n", lade_ms() - lade_t0);

	lade_shot_speichern("100");

	for(d = 0; d < 10; d++)
		if(lb_ziffer[d]) { SDL_FreeSurface(lb_ziffer[d]); lb_ziffer[d] = 0; }
}

/* the rectangle x, y, b, h of the picture to the screen, nothing else */
void zod_schirm_ausgeben_teil(int x, int y, int b, int h)
{
	SDL_Surface *schirm = SDL_GetVideoSurface();
	const int sx = letzt_x + x * letzt_f, sy = letzt_y + y * letzt_f;
	const int sb = b * letzt_f, sh = h * letzt_f;

	if(!schirm) return;

#ifdef __amigaos__
	zod_dirty_mark(schirm, sx, sy, sb, sh);
	zod_sdl_flip_teil(sx, sy, sb, sh);
	SDL_Flip(schirm);
#else
	SDL_UpdateRect(schirm, sx, sy, sb, sh);
#endif
}

}
