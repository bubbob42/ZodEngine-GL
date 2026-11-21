/*
 * c2p_test.c -- the AGA chunky-to-planar routines (port/amiga/zod_c2p.h)
 * against a plain reference, bit for bit, on the host.
 *
 * The routines take the chunky pixels as big-endian longwords (pixel 0 in
 * the top byte, as the 68k reads them from memory); the Mac is
 * little-endian, so the test builds those longwords itself. Reference: bit
 * (15 - i) of plane p's word, or (31 - i) of its longword, is bit p of
 * pixel i.
 *
 * Random pixels, the 256 single colours, and each bit alone at each place.
 * Prints "c2p: PASS ..." and exits 0, or the first difference and 1.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../../port/amiga/zod_c2p.h"

static int fails = 0;

static void load(const unsigned char *px, int n, c2p_u32 *s)
{
	int k;

	for(k = 0; k < n / 4; k++)
		s[k] = ((c2p_u32)px[4 * k] << 24) | ((c2p_u32)px[4 * k + 1] << 16) |
		       ((c2p_u32)px[4 * k + 2] << 8) | px[4 * k + 3];
}

static void check32(const unsigned char *px, const char *what)
{
	c2p_u32 s[8], l[8];
	int p, i;

	load(px, 32, s);
	c2p_32(s, l);

	for(p = 0; p < 8; p++)
	{
		c2p_u32 want = 0;

		for(i = 0; i < 32; i++)
			if((px[i] >> p) & 1) want |= (c2p_u32)1 << (31 - i);

		if(l[p] != want && fails++ < 5)
			printf("c2p_32 %s: plane %d is %08x, should be %08x\n", what, p, l[p], want);
	}
}

static void check16(const unsigned char *px, const char *what)
{
	c2p_u32 s[4];
	unsigned short w[8];
	int p, i;

	load(px, 16, s);
	c2p_16(s, w);

	for(p = 0; p < 8; p++)
	{
		unsigned short want = 0;

		for(i = 0; i < 16; i++)
			if((px[i] >> p) & 1) want |= (unsigned short)(1 << (15 - i));

		if(w[p] != want && fails++ < 5)
			printf("c2p_16 %s: plane %d is %04x, should be %04x\n", what, p, w[p], want);
	}
}

int main(void)
{
	unsigned char px[32];
	int n, i, b;

	if(sizeof(c2p_u32) != 4)
	{
		printf("c2p: c2p_u32 is %d bytes, not 4\n", (int)sizeof(c2p_u32));
		return 1;
	}

	srand(1234);

	for(n = 0; n < 200000; n++)
	{
		for(i = 0; i < 32; i++) px[i] = (unsigned char)(rand() & 0xFF);
		check32(px, "random");
		check16(px, "random");
	}

	for(n = 0; n < 256; n++)
	{
		for(i = 0; i < 32; i++) px[i] = (unsigned char)n;
		check32(px, "one colour");
		check16(px, "one colour");
	}

	for(i = 0; i < 32; i++)
		for(b = 0; b < 8; b++)
		{
			int k;

			for(k = 0; k < 32; k++) px[k] = 0;
			px[i] = (unsigned char)(1 << b);
			check32(px, "single bit");
			if(i < 16) check16(px, "single bit");
		}

	if(fails)
	{
		printf("c2p: FAIL, %d differences\n", fails);
		return 1;
	}

	printf("c2p: PASS -- c2p_32 and c2p_16 equal the reference (200000 random blocks, 256 colours, every single bit)\n");
	return 0;
}
