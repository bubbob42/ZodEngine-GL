#ifndef COMMON_H
#define COMMON_H

#include "qzod_dnseparate_global.h"

namespace COMMON
{
	//structs
    class QZOD_DNSEPARATESHARED_EXPORT xy_struct
	{
	public:
		xy_struct() { x=y=0; }
		xy_struct(int x_, int y_) { x=x_; y=y_; }

		int x, y;
	};

	/* Aus einem Eintrag der Kartenliste den anzeigbaren Namen machen:
	 * Verzeichnis und Endung weg.
	 *
	 * "Data/Campaing/Z_original/p02_bb_orig01.map" -> "p02_bb_orig01"
	 *
	 * WARUM: Das Auswahlfenster ist 168 Punkte breit, ein Listeneintrag
	 * also rund 20 Zeichen. Vom vollen Pfad war damit nur
	 * "Data/Campaing/Z_or" zu sehen -- bei ALLEN Eintraegen dasselbe.
	 * Vom Nutzer gemeldet: "Daher kann man nicht erkennen welche Karte
	 * man eigentlich auswaehlt."
	 *
	 * Hier und nicht an den beiden Anzeigestellen, weil es zwei sind
	 * (Liste und Abstimmungstext) und sie sonst auseinanderlaufen.
	 * Die AUSWAHL laeuft ueber den Listenplatz, nicht ueber den Text --
	 * das Kuerzen kann also nichts verstellen. */
	extern string map_short_name(const string &pfad);

	extern void split(char *dest, char *message, char split, int *initial, int d_size, int m_size);
	extern void clean_newline(char *message, int size);
	extern void lcase(char *message, int m_size);
	extern void lcase(string &message);
	extern double current_time();
	/* The clock the view's scrolling runs on (docs/concept-smooth-scrolling.md):
	 * current_time() unless a port hooks in a display clock -- the blank the
	 * frame being drawn will be shown on (port/amiga/zod_smooth.cpp).
	 * scroll_note(px): the view moves by px this frame (0: a scroll starts). */
	extern double (*scroll_clock_hook)();
	extern void (*scroll_note_hook)(int px);
	extern double scroll_clock();
	extern void scroll_note(int px);
	extern void create_folder(char *foldername);
	extern void uni_pause(int m_sec);
	extern char *wtoc_s(const wchar_t *input);
	extern char *wtoc(const wchar_t *input, char *dest, int size);
	extern wchar_t *ctow_s(const char *input);
	extern wchar_t *ctow(const char *input, wchar_t *dest, int size);
	extern void print_dump(char *message, int size, char *name);
	extern bool points_within_distance(int x1, int y1, int x2, int y2, int distance);
	extern bool points_within_area(int px, int py, int ax, int ay, int aw, int ah);
	extern bool good_user_char(int c);
	extern bool good_user_string(const char *message);
	extern void printd_reg(char *message);
	extern string data_to_hex_string(unsigned char *data, int size);
	extern bool file_can_be_written(char *filename);
	extern vector<string> directory_filelist(string foldername);
	extern void parse_filelist(vector<string> &filelist, string extension);
	extern bool sort_string_func (const string &a, const string &b);

	//inline functions...
    inline bool isz(float num) { return (num < 0.00001f && num > -0.00001f); };
	inline bool isz(double num) { return (num < 0.00001 && num > -0.00001); };

	/* floor() as an int, for values in int range: (int) cuts toward zero
	 * (one fintrz on the 060), one step down for negative non-integers --
	 * the same result as (int)floor(v). libm's floor is a call into a
	 * bit-by-bit ___fpu_round_to_integer on the 060: _floor and it were
	 * 1.4 % of the A4000D's tour samples (SmoothMove, ProcessMove, the hut
	 * animals). ZOD_FASTFLOOR aus: libm's floor again; ZOD_FLOORCHECK on:
	 * both, the differences counted (ifloor_report). */
	extern int ifloor_mode;            /* -1 not read yet, 1 fast, 0 libm, 2 check */
	extern int ifloor_other(double v);
	extern void ifloor_report();
	inline int ifloor(double v)
	{
		if(ifloor_mode == 1)
		{
			int i = (int)v;

			if((double)i > v) i--;
			return i;
		}

		return ifloor_other(v);
	}

    inline bool is1(float num) { return (num < 1.00001f && num > 0.99999f); };
	inline bool is1(double num) { return (num < 1.00001 && num > 0.99999); };

	inline void swap(int &a, int &b)
	{
		int c;

		c = a;
		a = b;
		b = c;
	}

	inline double frand() { return (rand()%10001) / 10000.0; }
};

#endif
