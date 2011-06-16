 @*****************************************************************************
 @ i420_yuyv_neon.S : ARM NEONv1 I420 to YUYV chroma conversion
 @*****************************************************************************
 @ Copyright (C) 2009 Rémi Denis-Courmont
 @
 @ This program is free software; you can redistribute it and/or modify
 @ it under the terms of the GNU General Public License as published by
 @ the Free Software Foundation; either version 2 of the License, or
 @ (at your option) any later version.
 @
 @ This program is distributed in the hope that it will be useful,
 @ but WITHOUT ANY WARRANTY; without even the implied warranty of
 @ MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 @ GNU General Public License for more details.
 @
 @ You should have received a copy of the GNU General Public License
 @ along with this program; if not, write to the Free Software Foundation,
 @ Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 @****************************************************************************/

	.fpu neon
	.text
/*
void i420_yuyv_neon(
	uint8_t *out,
	const uint8_t **in, := { y, u, v }
	unsigned int pitch_o,
	unsigned int offs_i,
	unsigned int width,
	unsigned int height);
*/

#define O1	r0
#define O2	r1
#define PITCH_O	r2
#define OFFS_I	r3
#define Y1	r4
#define Y2	r5
#define U	r6
#define V	r7
#define	WIDTH	r8
#define HEIGHT	r9
#define END_O1	r12

	.align
	.global i420_yuyv_neon
	.type	i420_yuyv_neon, %function
i420_yuyv_neon:
	push		{r4-r9, lr}
	ldr		WIDTH,	[sp, #(4*7)]
	ldr		HEIGHT, [sp, #(4*8)]
	ldmia		r1,	{Y1, U, V}
	mov		END_O1,	O1
	add		END_O1,	WIDTH,		lsl #1
1:
	add		Y2,	Y1,	WIDTH
	add		O2,	O1,	PITCH_O
	add		Y2,	OFFS_I
	pld		[Y2]
2:
	pld		[U, #64]
	vld1.u8		{d2},		[U,:64]!
	pld		[V, #64]
	vld1.u8		{d3},		[V,:64]!
	pld		[Y1, #64]
	vzip.u8		d2,	d3
	vld1.u8		{q0},		[Y1,:128]!
	pld		[Y2, #64]
	vmov		q3,	q1
	vzip.u8		q0,	q1
	vld1.u8		{q2},		[Y2,:128]!
	vzip.u8		q2,	q3
	vst1.u8		{q0-q1},	[O1,:128]!
	vst1.u8		{q2-q3},	[O2,:128]!

	cmp		O1,	END_O1
	bne 		2b

	add		O1,	O2,	PITCH_O
	sub		HEIGHT,	#2
	sub		O1,	WIDTH,		lsl #1
	add		END_O1, PITCH_O,	lsl #1

	add		Y1,	Y2,	OFFS_I
	add		U,	OFFS_I,		lsr #1
	add		V,	OFFS_I,		lsr #1

	cmp		HEIGHT,	#0
	bne		1b

	pop		{r4-r9, pc}
