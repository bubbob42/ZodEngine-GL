/*
 * env_switch_test.cpp -- the on/off words of the ZOD_ switches
 * (port/amiga/amiga_startup.cpp: zod_env_switch, zod_switch_value), with
 * the real file built for the host (zod_env reads getenv there).
 *
 * English on/off (1/0, yes/no, true/false, any case); the old German
 * ein/an/ja and aus/nein still read; missing or empty -> the default; any
 * other value (a mode name, margins) -> on.
 */
#include <stdio.h>
#include <stdlib.h>

#include "amiga_startup.h"

static int fails = 0;

static void want(const char *v, int dflt, int expect)
{
	const int got = zod_switch_value(v, dflt);

	if(got != expect)
	{
		printf("FAIL: \"%s\" (default %d) gave %d, want %d\n", v ? v : "(null)", dflt, got, expect);
		fails++;
	}
}

int main(void)
{
	want("on", 0, 1);   want("ON", 0, 1);   want("On", 0, 1);
	want("off", 1, 0);  want("OFF", 1, 0);  want("Off", 1, 0);
	want("1", 0, 1);    want("0", 1, 0);
	want("yes", 0, 1);  want("no", 1, 0);
	want("true", 0, 1); want("false", 1, 0);
	want("ein", 0, 1);  want("an", 0, 1);   want("ja", 0, 1);
	want("aus", 1, 0);  want("AUS", 1, 0);  want("nein", 1, 0);
	want(0, 1, 1);      want(0, 0, 0);      want(0, -1, -1);
	want("", 1, 1);     want("", 0, 0);
	want("pal", 0, 1);  want("64x16", 0, 1);  want("check", 0, 1);
	want("offx", 1, 1); /* a word that only starts like "off" is not off */

	setenv("ZOD_TEST_SWITCH", "off", 1);
	if(zod_env_switch("ZOD_TEST_SWITCH", 1) != 0) { printf("FAIL: env off\n"); fails++; }
	setenv("ZOD_TEST_SWITCH", "on", 1);
	if(zod_env_switch("ZOD_TEST_SWITCH", 0) != 1) { printf("FAIL: env on\n"); fails++; }
	unsetenv("ZOD_TEST_SWITCH");
	if(zod_env_switch("ZOD_TEST_SWITCH", 1) != 1) { printf("FAIL: env unset\n"); fails++; }

	if(fails) { printf("env switches: FAIL, %d\n", fails); return 1; }

	printf("env switches: PASS (on/off in English, the old German words, defaults, other values)\n");
	return 0;
}
