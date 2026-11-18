/*
 * Silent music output for the shim build (the whole engine on the host).
 *
 * port/amiga/musik_ausgabe.c drives AHI or Paula and exists only on the
 * Amiga. The shim draws but plays nothing, so this stand-in refuses to start
 * (musik_start returns 0): the engine then runs without music, exactly as on
 * an Amiga without AHI and audio.device. The mixer (musik_mischer.c) is the
 * real one -- it is plain C and already runs on the host in mischer_test.
 */
#include "musik_ausgabe.h"

int musik_start(int weg, const UBYTE *bank, ULONG banklen, ULONG rate, int stimmen)
{
	(void)weg; (void)bank; (void)banklen; (void)rate; (void)stimmen;
	return 0;
}

int  musik_spiele(const UBYTE *zmu, ULONG len, int schleife)
{
	(void)zmu; (void)len; (void)schleife;
	return 0;
}

void musik_springen(ULONG schritte)  { (void)schritte; }
void musik_pause(int an)             { (void)an; }
void musik_lautstaerke(int v)        { (void)v; }
int  musik_laeuft(void)              { return 0; }
int  musik_weg(void)                 { return MUSIK_AHI; }
void musik_stop(void)                { }

void musik_zahlen(ULONG *puffer, ULONG *zu_spaet)
{
	if(puffer)   *puffer = 0;
	if(zu_spaet) *zu_spaet = 0;
}
