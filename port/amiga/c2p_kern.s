; c2p_kern.s -- chunky to planar for the AGA screen, 32 pixels at a time,
; in 68k assembler (docs/concept-asm-speed.md, option A2)
;
; Replaces the pair loop of c2p_rows in zod_aga.cpp. The C fallback STAYS
; (zod_c2p.h, c2p_32) and decides at run time: ZOD_AGA_ASM=off takes it, and
; a self-test at the first frame compares this routine's planes with the C
; result for test pixels and falls back if they differ -- the same split as
; the music mixer's kernel (musik_kern.s).
;
; PURE 68020: no 040/060/080-only instruction (assembled with -m68020 as the
; check), so the module goes into all three CPU trees.
;
; The method: five passes of bit-field swaps on eight registers holding the
; 32 chunky bytes (one register = four pixels, pixel 0 in the top byte):
;   pass A, 16 bits: register pairs (4,0) (5,1) (6,2) (7,3)
;   pass B,  8 bits: (2,0) (3,1) (6,4) (7,5)
;   pass C,  4 bits: (1,0) (3,2) (5,4) (7,6)
;   pass D,  2 bits: (4,0) (5,1) (6,2) (7,3)
;   pass E,  1 bit:  (2,0) (3,1) (6,4) (7,5)
; each swap(a,b,s,m) being C2P_SWAP of zod_c2p.h: t = ((a >> s) ^ b) & m;
; b ^= t; a ^= t << s. Afterwards register r holds one plane's longword
; (the leftmost pixel in bit 31): plane 0 = d7, 1 = d5, 2 = d3, 3 = d1,
; 4 = d6, 5 = d4, 6 = d2, 7 = d0. Checked bit for bit against the C
; routine's plain reference in Python before this file was written (2,000
; random 32-pixel groups), and on the Amiga by the self-test and by FS-UAE's
; AgaCheck / AgaHwScroll, which read every frame back from the planes.
;
; 160 instructions a pair against the 306 gcc makes of c2p_32 (register
; spills to the stack included). The stores of a pair go out inside its
; last pass, each right after the swap that makes its register final, so the
; chip bus works while the next swaps are computed.
;
; CONTRACT with zod_aga.cpp:
;
;   void zod_c2p32_run(const unsigned char *src, struct zod_c2p_job *job)
;
;   src       the first pair's 32 chunky bytes (longword aligned: the
;             shadow row is a multiple of 64 bytes, a pair starts at an
;             even group)
;   job+0..28 ULONG *plane[8]: plane p's address of the first pair
;   job+32    LONG pairs, at least 1
;
;   pair i of plane p goes to plane[p] + 4 * i. Saves d2-d7/a2-a6 (gcc's
;   callee-saved registers); job is not changed.

	xdef	_zod_c2p32_run

	section	text,code

; swap(a, b, s, m) with t as the temporary
MERGE	macro
	move.l	\1,\5
	lsr.l	#\3,\5
	eor.l	\2,\5
	and.l	#\4,\5
	eor.l	\5,\2
	lsl.l	#\3,\5
	eor.l	\5,\1
	endm

; swap(a, b, 16, $0000FFFF) without a temporary: a's high word with b's low
; word (swap a, exchange the low words by three eor.w, swap a back)
SWAP16	macro
	swap	\1
	eor.w	\1,\2
	eor.w	\2,\1
	eor.w	\1,\2
	swap	\1
	endm

; register \1 is plane \2's longword of this pair
STORE	macro
	move.l	\2*4(a2),a4
	move.l	\1,(a4,a3.l)
	endm

_zod_c2p32_run:
	movem.l	d2-d7/a2-a6,-(sp)		; 44 bytes
	move.l	48(sp),a0			; src
	move.l	52(sp),a2			; job
	move.l	32(a2),a1			; pairs left
	suba.l	a3,a3				; offset into the planes: 4 a pair

.pair:
	movem.l	(a0)+,d0-d7			; 32 pixels, d0 = pixels 0..3

	; pass A: 16 bits
	SWAP16	d4,d0
	SWAP16	d5,d1
	SWAP16	d6,d2
	SWAP16	d7,d3

	; pass B: 8 bits (d7 parked in a5 as the temporary, then d0)
	move.l	d7,a5
	MERGE	d2,d0,8,$00FF00FF,d7
	MERGE	d3,d1,8,$00FF00FF,d7
	MERGE	d6,d4,8,$00FF00FF,d7
	move.l	a5,d7
	move.l	d0,a5
	MERGE	d7,d5,8,$00FF00FF,d0
	move.l	a5,d0

	; pass C: 4 bits
	move.l	d7,a5
	MERGE	d1,d0,4,$0F0F0F0F,d7
	MERGE	d3,d2,4,$0F0F0F0F,d7
	MERGE	d5,d4,4,$0F0F0F0F,d7
	move.l	a5,d7
	move.l	d0,a5
	MERGE	d7,d6,4,$0F0F0F0F,d0
	move.l	a5,d0

	; pass D: 2 bits
	move.l	d7,a5
	MERGE	d4,d0,2,$33333333,d7
	MERGE	d5,d1,2,$33333333,d7
	MERGE	d6,d2,2,$33333333,d7
	move.l	a5,d7
	move.l	d0,a5
	MERGE	d7,d3,2,$33333333,d0
	move.l	a5,d0

	; pass E: 1 bit, each pair's two registers stored as soon as they are
	; final (d0 is free after its store: the last swap's temporary)
	move.l	d7,a5
	MERGE	d2,d0,1,$55555555,d7
	STORE	d0,7
	STORE	d2,6
	MERGE	d3,d1,1,$55555555,d7
	STORE	d1,3
	STORE	d3,2
	MERGE	d6,d4,1,$55555555,d7
	STORE	d4,5
	STORE	d6,4
	move.l	a5,d7
	MERGE	d7,d5,1,$55555555,d0
	STORE	d5,1
	STORE	d7,0

	addq.l	#4,a3
	subq.l	#1,a1
	cmpa.w	#0,a1
	bne	.pair

	movem.l	(sp)+,d2-d7/a2-a6
	rts
