; copyprobe_k.s -- the copy loops copyprobe.c times (docs/concept-asm-speed.md)
;
; All three copy n bytes (a multiple of 16) from src (any alignment) to dst
; (16-byte aligned). 68040/68060 only (move16).
;
;   void cp_longs(UBYTE *dst, const UBYTE *src, LONG n)
;       four move.l a step
;   void cp_m16tmp(UBYTE *dst, const UBYTE *src, LONG n, UBYTE *tmp16)
;       four move.l into a 16-byte aligned line in tmp16, then
;       move16 (tmp)+,(dst)+: the destination line is written whole,
;       never read
;   void cp_m16(UBYTE *dst, const UBYTE *src, LONG n)
;       move16 (src)+,(dst)+ directly (both 16-byte aligned: the best case)

	machine	68040
	xdef	_cp_longs
	xdef	_cp_m16tmp
	xdef	_cp_m16

	section	text,code

_cp_longs:
	move.l	4(sp),a1		; dst
	move.l	8(sp),a0		; src
	move.l	12(sp),d0
	lsr.l	#4,d0
	beq.s	.l9
.l1:	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	subq.l	#1,d0
	bne.s	.l1
.l9:	rts

_cp_m16tmp:
	movem.l	d2-d3/a2,-(sp)
	move.l	16(sp),a1		; dst
	move.l	20(sp),a0		; src
	move.l	24(sp),d0
	move.l	28(sp),a2		; tmp line
	lsr.l	#4,d0
	beq.s	.t9
.t1:	move.l	(a0)+,(a2)+
	move.l	(a0)+,(a2)+
	move.l	(a0)+,(a2)+
	move.l	(a0)+,(a2)+
	lea	-16(a2),a2
	move16	(a2)+,(a1)+
	lea	-16(a2),a2
	subq.l	#1,d0
	bne.s	.t1
.t9:	movem.l	(sp)+,d2-d3/a2
	rts

_cp_m16:
	move.l	4(sp),a1		; dst
	move.l	8(sp),a0		; src
	move.l	12(sp),d0
	lsr.l	#4,d0
	beq.s	.m9
.m1:	move16	(a0)+,(a1)+
	subq.l	#1,d0
	bne.s	.m1
.m9:	rts
