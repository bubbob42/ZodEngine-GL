#ifndef ZOD_C2P_H
#define ZOD_C2P_H
/*
 * zod_c2p.h -- chunky to planar for the AGA screen (zod_aga.cpp), as inline
 * functions without Amiga headers so tests/host/c2p_test.c can check them
 * bit for bit on the Mac against a plain reference.
 *
 * c2p_u32 is a 32-bit unsigned type on both (unsigned int: 32 bits on the
 * m68k and on the host -- `unsigned long` is 64 bits on the Mac).
 *
 * Two widths:
 *  - c2p_16: 16 pixels -> one 16-bit word for each of the 8 planes (the
 *    original routine).
 *  - c2p_32: 32 pixels -> one 32-bit longword for each plane. AGA's chip bus
 *    gives the CPU about the same number of accesses a second whatever their
 *    width (measured, docs/research-aga-speed.md), so a longword moves twice
 *    the bytes of a word. Built from four of the same 8x8 bit transposes and
 *    a 4x4 byte transpose of their results (the idea of writing longwords
 *    to chip RAM as BlazeWCP does; the code is our own).
 */

typedef unsigned int c2p_u32;

/* the 8x8 bit transpose (Hacker's Delight): x = pixels 0..3, y = 4..7 (a
 * byte each, the first in the top byte) in; x = planes 7..4, y = planes
 * 3..0 out (a byte each, the leftmost pixel in the top bit) -- and back */
static inline void c2p_transpose8(c2p_u32 *px, c2p_u32 *py)
{
	c2p_u32 x = *px, y = *py, t;

	t = (x ^ (x >> 7)) & 0x00AA00AAU;  x = x ^ t ^ (t << 7);
	t = (y ^ (y >> 7)) & 0x00AA00AAU;  y = y ^ t ^ (t << 7);
	t = (x ^ (x >> 14)) & 0x0000CCCCU; x = x ^ t ^ (t << 14);
	t = (y ^ (y >> 14)) & 0x0000CCCCU; y = y ^ t ^ (t << 14);
	t = (x & 0xF0F0F0F0U) | ((y >> 4) & 0x0F0F0F0FU);
	y = ((x << 4) & 0xF0F0F0F0U) | (y & 0x0F0F0F0FU);
	*px = t;
	*py = y;
}

/* 16 pixels (s: four longwords, any alignment the CPU allows) -> w[7..0]:
 * one word per plane, w[p] for plane p */
static inline void c2p_16(const c2p_u32 *s, unsigned short *w)
{
	c2p_u32 a = s[0], b = s[1], c = s[2], d = s[3];

	c2p_transpose8(&a, &b);   /* left 8 pixels: a planes 7..4, b planes 3..0 */
	c2p_transpose8(&c, &d);   /* right 8 pixels */

	w[7] = (unsigned short)(((a >> 16) & 0xFF00) | (c >> 24));
	w[6] = (unsigned short)(((a >> 8) & 0xFF00) | ((c >> 16) & 0xFF));
	w[5] = (unsigned short)((a & 0xFF00) | ((c >> 8) & 0xFF));
	w[4] = (unsigned short)(((a << 8) & 0xFF00) | (c & 0xFF));
	w[3] = (unsigned short)(((b >> 16) & 0xFF00) | (d >> 24));
	w[2] = (unsigned short)(((b >> 8) & 0xFF00) | ((d >> 16) & 0xFF));
	w[1] = (unsigned short)((b & 0xFF00) | ((d >> 8) & 0xFF));
	w[0] = (unsigned short)(((b << 8) & 0xFF00) | (d & 0xFF));
}

/* swap the bits of a selected by m << s with those of b selected by m */
#define C2P_SWAP(a, b, s, m) do { c2p_u32 t_ = (((a) >> (s)) ^ (b)) & (m); \
                                  (b) ^= t_; (a) ^= t_ << (s); } while(0)

/* 32 pixels (s: eight longwords) -> l[7..0]: one longword per plane */
static inline void c2p_32(const c2p_u32 *s, c2p_u32 *l)
{
	c2p_u32 x0 = s[0], y0 = s[1], x1 = s[2], y1 = s[3];
	c2p_u32 x2 = s[4], y2 = s[5], x3 = s[6], y3 = s[7];

	/* each 8 pixels: x = the bytes of planes 7..4, y = of planes 3..0 */
	c2p_transpose8(&x0, &y0);
	c2p_transpose8(&x1, &y1);
	c2p_transpose8(&x2, &y2);
	c2p_transpose8(&x3, &y3);

	/* a 4x4 byte transpose: rows x0..x3 (the four 8-pixel groups), columns
	 * the planes -> row p = plane 7 - p, the leftmost group in the top byte.
	 * First the 2x2 blocks off the diagonal (x0/x1 low halves with x2/x3
	 * high halves), then the bytes inside each block. */
	C2P_SWAP(x2, x0, 16, 0x0000FFFFU);
	C2P_SWAP(x3, x1, 16, 0x0000FFFFU);
	C2P_SWAP(x1, x0, 8, 0x00FF00FFU);
	C2P_SWAP(x3, x2, 8, 0x00FF00FFU);

	C2P_SWAP(y2, y0, 16, 0x0000FFFFU);
	C2P_SWAP(y3, y1, 16, 0x0000FFFFU);
	C2P_SWAP(y1, y0, 8, 0x00FF00FFU);
	C2P_SWAP(y3, y2, 8, 0x00FF00FFU);

	l[7] = x0; l[6] = x1; l[5] = x2; l[4] = x3;
	l[3] = y0; l[2] = y1; l[1] = y2; l[0] = y3;
}

#endif
