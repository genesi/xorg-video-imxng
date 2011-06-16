/*
 * software RGB to RGB converter
 * pluralize by software PAL8 to RGB converter
 *              software YUV to YUV converter
 *              software YUV to RGB converter
 * Written by Nick Kurshev.
 * palette & YUV & runtime CPU stuff by Michael (michaelni@gmx.at)
 * lot of big-endian byte order fixes by Alex Beregszaszi
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>

void yuvPlanartoyuy2_c(
	const uint8_t *ysrc, const uint8_t *usrc,
	const uint8_t *vsrc, uint8_t *dst,
	long width, long height,
	long lumStride, long chromStride,
	long dstStride, long vertLumPerChroma)
{
	long y;
	const int chromWidth = width >> 1;
	for (y=0; y<height; y++) {
#if HAVE_FAST_64BIT
		int i;
		uint64_t *ldst = (uint64_t *) dst;
		const uint8_t *yc = ysrc, *uc = usrc, *vc = vsrc;
		for (i = 0; i < chromWidth; i += 2) {
			uint64_t k, l;
			k = yc[0] + (uc[0] << 8) +
				(yc[1] << 16) + (vc[0] << 24);
			l = yc[2] + (uc[1] << 8) +
				(yc[3] << 16) + (vc[1] << 24);
			*ldst++ = k + (l << 32);
			yc += 4;
			uc += 2;
			vc += 2;
		}

#else
		int i, *idst = (int32_t *) dst;
		const uint8_t *yc = ysrc, *uc = usrc, *vc = vsrc;
		for (i = 0; i < chromWidth; i++) {
#if HAVE_BIGENDIAN
			*idst++ = (yc[0] << 24) + (uc[0] << 16) +
				(yc[1] << 8) + (vc[0] << 0);
#else
			*idst++ = yc[0] + (uc[0] << 8) +
				(yc[1] << 16) + (vc[0] << 24);
#endif
			yc += 2;
			uc++;
			vc++;
		}
#endif
		if ((y&(vertLumPerChroma-1)) == vertLumPerChroma-1) {
			usrc += chromStride;
			vsrc += chromStride;
		}
		ysrc += lumStride;
		dst  += dstStride;
	}
}

void
yuv_planar_to_yuy2(
	uint8_t *dst,
	const uint8_t *ysrc,
	const uint8_t *usrc,
	const uint8_t *vsrc,
	int width,
	int height,
	int dst_stride,
	int lum_stride,
	int chr_stride,
	int is_chroma_every_other_line) /* 1 for 2:1 luma:chroma vertical distribution,
									   0 otherwise */
{
	const int chroma_width = width / 2;

	int i;
	for (i = 0; i < height; ++i) {

		uint32_t *pack_dst = (uint32_t *) dst;

		const uint8_t *two_ysrc = ysrc;
		const uint8_t *one_usrc = usrc;
		const uint8_t *one_vsrc = vsrc;

		int j;
		for (j = 0; j < chroma_width; ++j) {

			*pack_dst++ =
				(two_ysrc[0] <<  0) +
				(one_usrc[0] <<  8) +
				(two_ysrc[1] << 16) +
				(one_vsrc[0] << 24);

			two_ysrc += 2;
			++one_usrc;
			++one_vsrc;
		}

		dst  += dst_stride;
		ysrc += lum_stride;

		if (is_chroma_every_other_line ==
			(i & is_chroma_every_other_line)) {

			usrc += chr_stride;
			vsrc += chr_stride;
		}
	}
}

