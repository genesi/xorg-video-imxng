/*
 * Copyright (C) 2009-2010 Freescale Semiconductor, Inc.  All Rights Reserved.
 * Copytight (C) 2011 Genesi USA, Inc. All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <xf86.h>
#include <xf86xv.h>
#include <X11/extensions/Xv.h>
#include <fourcc.h>
#include <dlfcn.h>
#include <fbdevhw.h>
#include <damage.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <errno.h>
#include <string.h>

#include "imx_type.h"
#include "imx_colorspace.h"

#define IMXXV_SURF_ALLOC_DEBUG	(1 && IMX_DEBUG_MASTER)

#ifndef FOURCC_YVYU
#define FOURCC_YVYU 0x55595659 /* 'YVYU' in little-endian */
#endif

#define IMXXV_MAX_IMG_WIDTH		2048 /* Must be even. */
#define IMXXV_MAX_IMG_HEIGHT	1024 /* Must be even. */

#define IMXXV_MAX_OUT_WIDTH		2048 /* Port max horizontal resolution. */
#define IMXXV_MAX_OUT_HEIGHT	2048 /* Port max vertical resolution. */

#define IMXXV_MAX_BLIT_COORD	1024
/* NOTE: When scale-blitting Z160 cannot address a source beyond the 1024th row/column */
/* (it runs out of src coord bits and wraps around). */

/* Adaptor encodings. */
static XF86VideoEncodingRec imxVideoEncoding[] =
{
	/* A single encoding of class image. */
	{
		.id = 0,
		.name = "XV_IMAGE",
		.width = IMXXV_MAX_IMG_WIDTH,
		.height = IMXXV_MAX_IMG_HEIGHT,
		.rate = { 1, 1 }
	}
};

/* Adaptor output depths. */
static XF86VideoFormatRec imxVideoFormat[] =
{
	/* All viable IMX fb depths. */
	{
		.depth = 15,
		.class = TrueColor
	},
	{
		.depth = 16,
		.class = TrueColor
	},
	{
		.depth = 24,
		.class = TrueColor
	}
};

static XF86AttributeRec imxPortAttribute[] =
{
    {
		.flags = XvSettable | XvGettable,
		.min_value = 0,
		.max_value = (1 << 24) - 1,
		.name = "XV_COLORKEY"
    },
    {
		.flags = XvSettable,
		.min_value = 0,
		.max_value = 0,
		.name = "XV_SET_DEFAULTS"
    }
};

#define IMXXV_NUM_ATTR (sizeof(imxPortAttribute) / sizeof(imxPortAttribute[0]))

static XF86ImageRec imxImage[] =
{
	XVIMAGE_YV12, /* transformed to C2D_COLOR_YUY2 */
	XVIMAGE_I420, /* transformed to C2D_COLOR_YUY2 */
	XVIMAGE_YUY2, /* corresponds to C2D_COLOR_YUY2 */
	XVIMAGE_UYVY, /* corresponds to C2D_COLOR_UYVY */
	/* YVYU, corresponds to C2D_COLOR_YVYU */
	{
		.id = FOURCC_YVYU,
		.type = XvYUV,
		.byte_order = LSBFirst,
		.guid = { 'Y', 'V', 'Y', 'U',
			0x00, 0x00, 0x00, 0x10, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 },
		.bits_per_pixel = 16,
		.format = XvPacked,
		.num_planes = 1,
		.y_sample_bits = 8,
		.u_sample_bits = 8,
		.v_sample_bits = 8,
		.horz_y_period = 1,
		.horz_u_period = 2,
		.horz_v_period = 2,
		.vert_y_period = 1,
		.vert_u_period = 1,
		.vert_v_period = 1,
		.component_order = { 'Y', 'V', 'Y', 'U' },
		.scanline_order = XvTopToBottom
	}
};

static void
IMXSetColorKeyAttribute(
	IMXPtr imxPtr,
	INT32  Value)
{
}

static INT32
IMXGetColorKeyAttribute(
	IMXPtr imxPtr)
{
	return (INT32) -1;
}

static void
IMXSetDefaultAttributes(
	IMXPtr imxPtr,
	INT32  Value)
{
	IMXSetColorKeyAttribute(imxPtr, (INT32) -1);
}

typedef struct _IMXAttribute
{
	Atom  attribute;
	void  (*setAttribute)(IMXPtr, INT32);
	INT32 (*getAttribute)(IMXPtr);
} IMXAttributeRec, *IMXAttributePtr;

static IMXAttributeRec imxAttributeInfo[IMXXV_NUM_ATTR] =
{
	{   /* COLORKEY */
		.setAttribute = IMXSetColorKeyAttribute,
		.getAttribute = IMXGetColorKeyAttribute
	},
	{   /* SET_DEFAULTS */
		.setAttribute = IMXSetDefaultAttributes,
	}
};

static inline int
imxxv_find_port_attribute(
	Atom attribute)
{
	int idx;

	for (idx = 0; idx < IMXXV_NUM_ATTR; ++idx)
		if (attribute == imxAttributeInfo[idx].attribute)
			return idx;

	return -1;
}

extern const char*
imxexa_string_from_c2d_format(
	C2D_COLORFORMAT format);

static inline unsigned
imxxv_bpp_from_c2d_format(
	C2D_COLORFORMAT format)
{
	switch (format) {

	/* 1bpp formats */
	case C2D_COLOR_A1:
		return 1;

	/* 4bpp formats */
	case C2D_COLOR_A4:
		return 4;

	/* 8bpp formats */
	case C2D_COLOR_A8:
	case C2D_COLOR_8:
		return 8;

	/* 16bpp formats */
	case C2D_COLOR_4444:
	case C2D_COLOR_4444_RGBA:
	case C2D_COLOR_1555:
	case C2D_COLOR_5551_RGBA:
	case C2D_COLOR_0565:
		return 16;

	/* 32bpp formats */
	case C2D_COLOR_8888:
	case C2D_COLOR_8888_RGBA:
	case C2D_COLOR_8888_ABGR:
		return 32;

	/* 24bpp formats */
	case C2D_COLOR_888:
		return 24;

	/* 16bpp formats */
	case C2D_COLOR_YVYU:
	case C2D_COLOR_UYVY:
	case C2D_COLOR_YUY2:
		return 16;

	/* Pacify the compiler. */
	default:
		break;
	}

	return 0;
}

static inline const char*
imxxv_string_from_c2d_surface(
	const C2D_SURFACE_DEF* surfDef)
{
	static char buf[1024];

	snprintf(buf, sizeof(buf) / sizeof(buf[0]), "%s %ux%u stride: %u pa: %p va: %p stride_based_size: %u",
		imxexa_string_from_c2d_format(surfDef->format),
		surfDef->width,
		surfDef->height,
		surfDef->stride,
		surfDef->buffer,
		surfDef->host,
		surfDef->height * surfDef->stride);

	return buf;
}

static inline const char*
imxxv_string_from_unalloc_c2d_surface(
	const C2D_SURFACE_DEF* surfDef)
{
	static char buf[1024];

	snprintf(buf, sizeof(buf) / sizeof(buf[0]), "%s %ux%u estimated_size: %u",
		imxexa_string_from_c2d_format(surfDef->format),
		surfDef->width,
		surfDef->height,
		surfDef->width * surfDef->height * imxxv_bpp_from_c2d_format(surfDef->format) / 8);

	return buf;
}

static inline int
imxxv_port_idx_from_cookie(
	IMXPtr imxPtr,
	pointer data)
{
	return (uint8_t *) data - (uint8_t *) imxPtr;
}

static inline pointer
imxxv_port_cookie_from_idx(
	IMXPtr imxPtr,
	int idx)
{
	return (pointer) ((uint8_t *) imxPtr + idx);
}

static void
imxxv_delete_port_surface(
	IMXPtr imxPtr,
	int port_idx)
{
	c2dSurfFree(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surf);

	imxPtr->xvPort[port_idx].surf = NULL;
	memset(&imxPtr->xvPort[port_idx].surfDef, 0, sizeof(imxPtr->xvPort[port_idx].surfDef));

	if (NULL != imxPtr->xvPort[port_idx].surfAux) {

		c2dSurfFree(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surfAux);
		imxPtr->xvPort[port_idx].surfAux = NULL;
	}

	imxPtr->xvPort[port_idx].report_split = FALSE;
}

static int
IMXXVSetPortAttribute(
	ScrnInfoPtr pScrn,
	Atom attribute,
	INT32 value,
	pointer data)
{
	int attr_idx;

	if (0 > (attr_idx = imxxv_find_port_attribute(attribute)) ||
		NULL == imxAttributeInfo[attr_idx].setAttribute) {

		return BadMatch;
	}

	if (imxPortAttribute[attr_idx].max_value < value)
		value = imxPortAttribute[attr_idx].max_value;
	else
	if (imxPortAttribute[attr_idx].min_value > value)
		value = imxPortAttribute[attr_idx].min_value;

	IMXPtr imxPtr = IMXPTR(pScrn);
	int port_idx = imxxv_port_idx_from_cookie(imxPtr, data);

	(*imxAttributeInfo[attr_idx].setAttribute)(imxPtr, value);

	return Success;
}

static int
IMXXVGetPortAttribute(
	ScrnInfoPtr pScrn,
	Atom attribute,
	INT32 *value,
	pointer data)
{
	int attr_idx;

	if (NULL == value ||
		0 > (attr_idx = imxxv_find_port_attribute(attribute)) ||
		NULL == imxAttributeInfo[attr_idx].getAttribute) {

		return BadMatch;
	}

	IMXPtr imxPtr = IMXPTR(pScrn);
	int port_idx = imxxv_port_idx_from_cookie(imxPtr, data);

	*value = (*imxAttributeInfo[attr_idx].getAttribute)(imxPtr);

	return Success;
}

static void
IMXXVStopVideo(
	ScrnInfoPtr pScrn,
	pointer data,
	Bool cleanup)
{
	IMXPtr imxPtr = IMXPTR(pScrn);

	const int port_idx = imxxv_port_idx_from_cookie(imxPtr, data);

	if (cleanup && NULL != imxPtr->xvGpuContext) {

		if (NULL != imxPtr->xvPort[port_idx].surf) {

			imxxv_delete_port_surface(imxPtr, port_idx);

#if IMXXV_DBLFB_ENABLE

			/* Bring xorg's framebuffer to front. */
			const int fd = fbdevHWGetFD(pScrn);

			struct fb_var_screeninfo varinfo;

			if (-1 == ioctl(fd, FBIOGET_VSCREENINFO, &varinfo)) {
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"IMXXVStopVideo failed at get_vscreeninfo ioctl (errno: %s)\n",
					strerror(errno));
			}

			varinfo.yoffset = 0;

			if (-1 == ioctl(fd, FBIOPAN_DISPLAY, &varinfo)) {
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"IMXXVStopVideo failed at pan_display ioctl (errno: %s)\n",
					strerror(errno));
			}

			imxPtr->xvBufferTracker = 0;

#endif /* IMXXV_DBLFB_ENABLE */

			unsigned i;

			for (i = 0; i < imxPtr->xvPort[port_idx].num_phys; ++i) {

				if (NULL != imxPtr->xvPort[port_idx].phys[i].mapping)
					munmap(imxPtr->xvPort[port_idx].phys[i].mapping, imxPtr->xvPort[port_idx].phys[i].mapping_len);

				imxPtr->xvPort[port_idx].phys[i].phys_ptr = 0;
				imxPtr->xvPort[port_idx].phys[i].mapping = NULL;
				imxPtr->xvPort[port_idx].phys[i].mapping_len = 0;
				imxPtr->xvPort[port_idx].phys[i].mapping_offset = 0;
			}

			imxPtr->xvPort[port_idx].num_phys = 0;
		}
	}
}

static inline unsigned
imxxv_seek_mapping(
	const IMXPtr imxPtr,
	const unsigned port_idx,
	const intptr_t phys_ptr)
{
	unsigned i;

	for (i = 0; i < imxPtr->xvPort[port_idx].num_phys; ++i)
		if (phys_ptr == imxPtr->xvPort[port_idx].phys[i].phys_ptr)
			return i;

	return -1U;
}

static inline void
imxxv_fill_surface(
	const C2D_CONTEXT context,
	const C2D_SURFACE surf,
	const uint32_t color)
{
	C2D_RECT rect = {
		0, 0, 2048, 2048
	};

	c2dSetDstSurface(context, surf);
	c2dSetSrcSurface(context, NULL);
	c2dSetBrushSurface(context, NULL, NULL);
	c2dSetMaskSurface(context, NULL, NULL);
	c2dSetBlendMode(context, C2D_ALPHA_BLEND_NONE);
	c2dSetDstRectangle(context, &rect);
	c2dSetFgColor(context, color);

	const C2D_STATUS r = c2dDrawRect(context, C2D_PARAM_FILL_BIT);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(0, X_ERROR,
			"imxxv_fill_surface failed to clear GPU surface (code: 0x%08x)\n", r);
	}
}

static void
IMXXVQueryBestSize(
	ScrnInfoPtr pScrn,
	Bool motion,
	short vid_w,
	short vid_h,
	short drw_w,
	short drw_h,
	unsigned int *p_w,
	unsigned int *p_h,
	pointer data)
{
	*p_w = drw_w > IMXXV_MAX_OUT_WIDTH ? IMXXV_MAX_OUT_WIDTH : drw_w;
	*p_h = drw_h > IMXXV_MAX_OUT_HEIGHT ? IMXXV_MAX_OUT_HEIGHT : drw_h;
}

extern void
yuv420_to_yuv422(
	uint8_t *yuv,
	const uint8_t *y,
	const uint8_t *u,
	const uint8_t *v,
	int w,
	int h,
	int yw,
	int cw,
	int dw);

static int
IMXXVPutImage(
	ScrnInfoPtr pScrn,
	short src_x,
	short src_y,
	short drw_x,
	short drw_y,
	short src_w,
	short src_h,
	short drw_w,
	short drw_h,
	int image,
	unsigned char* buf,
	short width,
	short height,
	Bool Sync,
	RegionPtr clipBoxes,
	pointer data,
	DrawablePtr pDraw)
{
	if (NULL == clipBoxes) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVPutImage called with no clip boxes\n");

		return BadMatch;
	}

	IMXPtr imxPtr = IMXPTR(pScrn);

	if (NULL == imxPtr->xvGpuContext) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVPutImage called with no GPU context\n");

		return BadMatch;
	}

	C2D_COLORFORMAT fmt;
	const int bytespp = 2;

	switch (image) {
	case FOURCC_YVYU:
		fmt = C2D_COLOR_YVYU;
		break;
	case FOURCC_UYVY:
		fmt = C2D_COLOR_UYVY;
		break;
	case FOURCC_YV12: /* Through a transform. */
	case FOURCC_I420: /* Through a transform. */
	case FOURCC_YUY2:
		fmt = C2D_COLOR_YUY2;
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVPutImage called with wrong src image format\n");
		return BadMatch;
	}

	const int port_idx = imxxv_port_idx_from_cookie(imxPtr, data);

	if (NULL != imxPtr->xvPort[port_idx].surf &&
		(fmt != imxPtr->xvPort[port_idx].surfDef.format ||
		 width > imxPtr->xvPort[port_idx].surfDef.width ||
		 height > imxPtr->xvPort[port_idx].surfDef.height)) {

		imxxv_delete_port_surface(imxPtr, port_idx);
	}

	if (NULL == imxPtr->xvPort[port_idx].surf) {

		imxPtr->xvPort[port_idx].surfDef.format	= fmt;
		imxPtr->xvPort[port_idx].surfDef.width	= width;
		imxPtr->xvPort[port_idx].surfDef.height	= height;

#if IMXXV_SURF_ALLOC_DEBUG

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXXVPutImage about to allocate surface: %s\n",
			imxxv_string_from_unalloc_c2d_surface(&imxPtr->xvPort[port_idx].surfDef));

#endif /* IMXXV_SURF_ALLOC_DEBUG */

		C2D_STATUS r;
		r = c2dSurfAlloc(imxPtr->xvGpuContext, &imxPtr->xvPort[port_idx].surf, &imxPtr->xvPort[port_idx].surfDef);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVPutImage failed to allocate GPU surface (code: 0x%08x)\n", r);

			return BadAlloc;
		}

#if IMXXV_SURF_ALLOC_DEBUG

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXXVPutImage allocated surface: %s\n",
			imxxv_string_from_c2d_surface(&imxPtr->xvPort[port_idx].surfDef));

#endif /* IMXXV_SURF_ALLOC_DEBUG */

		/* Wipe out the new surface to YUY2 black. */
		imxxv_fill_surface(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surf, 0x800000U);

		if (width > IMXXV_MAX_BLIT_COORD) {

			C2D_SURFACE_DEF surfDef;
			memcpy(&surfDef, &imxPtr->xvPort[port_idx].surfDef, sizeof(surfDef));

			surfDef.width  = width - IMXXV_MAX_BLIT_COORD;
			surfDef.buffer = (char *) surfDef.buffer + IMXXV_MAX_BLIT_COORD * bytespp;
			surfDef.host   = (char *) surfDef.host + IMXXV_MAX_BLIT_COORD * bytespp;
			surfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

			r = c2dSurfAlloc(imxPtr->xvGpuContext, &imxPtr->xvPort[port_idx].surfAux, &surfDef);

			if (C2D_STATUS_OK != r) {

				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"IMXXVPutImage failed to allocate GPU surface (code: 0x%08x)\n", r);

				return BadAlloc;
			}
		}
	}

	/* gstreamer physical buffer support */
	if (0xbeefc0de == ((intptr_t*) buf)[0]) {

		unsigned idx = imxxv_seek_mapping(imxPtr, port_idx, ((intptr_t*) buf)[1]);

		if (-1U == idx) {

			if (IMXXV_NUM_PHYS_BUFFERS > imxPtr->xvPort[port_idx].num_phys) {

				idx = imxPtr->xvPort[port_idx].num_phys++;

				const int pagemask = getpagesize() - 1;
				const intptr_t phys_ptr = ((intptr_t*) buf)[1];
				const intptr_t phys_page_ptr = phys_ptr & ~pagemask;
				const size_t src_len = width * height + width * height / 2; /* planar YUV assumed */

				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
					"IMXXVPutImage detected physical buffer at input; mapping phys memory from 0x%08x..\n",
					phys_ptr);

				const int fd = open("/dev/mem", O_RDWR);

				imxPtr->xvPort[port_idx].phys[idx].phys_ptr = phys_ptr;
				imxPtr->xvPort[port_idx].phys[idx].mapping_offset = phys_ptr - phys_page_ptr;
				imxPtr->xvPort[port_idx].phys[idx].mapping_len = (phys_ptr - phys_page_ptr + src_len + pagemask) & ~pagemask;
				imxPtr->xvPort[port_idx].phys[idx].mapping =
					mmap(0, imxPtr->xvPort[port_idx].phys[idx].mapping_len, PROT_READ, MAP_SHARED, fd, phys_page_ptr);

				close(fd);

				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
					"IMXXVPutImage mapping done. Port %d, src length 0x%08x, phys buffer length 0x%08x, virtual mapping %p\n",
					port_idx, src_len,
					imxPtr->xvPort[port_idx].phys[idx].mapping_len,
					imxPtr->xvPort[port_idx].phys[idx].mapping);
			}
			else {
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"IMXXVPutImage is unable to perform virtual mapping of physical ptr 0x%08x, port %d\n",
					((intptr_t*) buf)[1], port_idx);

				idx = 0;
			}
		}

		buf = (unsigned char*) imxPtr->xvPort[port_idx].phys[idx].mapping +
			imxPtr->xvPort[port_idx].phys[idx].mapping_offset;
	}

	unsigned char* bits;
	C2D_STATUS r;

	/* Access-lock the Xv GPU surface. */
	r = c2dSurfLock(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surf, (void**) &bits);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVPutImage failed to lock GPU surface (code: 0x%08x)\n", r);

		return BadMatch;
	}

	long align_src_x = src_x;
	long align_src_w = src_w;

	if (src_x & 1) {
		align_src_x &= ~1;
		align_src_w += 1;
	}

	align_src_w = (align_src_w + 1) & ~1;

	long align_src_y = src_y;
	long align_src_h = src_h;

	if (src_y & 1) {
		align_src_y &= ~1;
		align_src_h += 1;
	}

	align_src_h = (align_src_h + 1) & ~1;

	if (FOURCC_YV12 == image ||
		FOURCC_I420 == image ) {

		const unsigned int dst_stride = imxPtr->xvPort[port_idx].surfDef.stride;
		uint8_t* dst = bits + align_src_y * dst_stride + align_src_x * bytespp;

		const unsigned int lum_stride = width;
		const unsigned int chr_stride = width / 2;

		const uint8_t *ysrc = buf + lum_stride * align_src_y + align_src_x;
		const uint8_t *usrc = buf + lum_stride * height + chr_stride * (0	   + align_src_y) / 2 + align_src_x / 2;
		const uint8_t *vsrc = buf + lum_stride * height + chr_stride * (height + align_src_y) / 2 + align_src_x / 2;

		if (FOURCC_YV12 == image) {
#if 0
			i420_to_yuy2_c(dst, ysrc, vsrc, usrc, align_src_w, align_src_h, dst_stride, lum_stride, chr_stride);
#else
			yuv420_to_yuv422(
				dst,
				ysrc,
				vsrc,
				usrc,
				align_src_w & ~0xf,
				align_src_h & ~0xf,
				width,
				width / 2,
				dst_stride);
#endif
		}
		else {
#if 0
			i420_to_yuy2_c(dst, ysrc, usrc, vsrc, align_src_w, align_src_h, dst_stride, lum_stride, chr_stride);
#else
			yuv420_to_yuv422(
				dst,
				ysrc,
				usrc,
				vsrc,
				align_src_w & ~0xf,
				align_src_h & ~0xf,
				width,
				width / 2,
				dst_stride);
#endif
		}
	}
	else {

		const unsigned int dst_stride = imxPtr->xvPort[port_idx].surfDef.stride;
		unsigned char* dst = bits + src_y * dst_stride + align_src_x * bytespp;

		const unsigned int src_stride = width * bytespp;
		const unsigned char* src = buf + src_y * src_stride + align_src_x * bytespp;

		const size_t span_len = align_src_w * bytespp;

		int i = src_h;
		while (i--) {
			memcpy(dst, src, span_len);
			dst += dst_stride;
			src += src_stride;
		}
	}

	/* Surface updated, unlock it. */
	c2dSurfUnlock(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surf);

	/* Set various static draw parameters. */
	c2dSetBrushSurface(imxPtr->xvGpuContext, NULL, NULL);
	c2dSetMaskSurface(imxPtr->xvGpuContext, NULL, NULL);

	c2dSetBlendMode(imxPtr->xvGpuContext, C2D_ALPHA_BLEND_NONE);

	const Bool dither_blit = 16 == pScrn->bitsPerPixel;

	if (dither_blit)
		c2dSetDither(imxPtr->xvGpuContext, 1);

	const Bool stretch_blit = imxPtr->use_bilinear_filtering ?
		(src_w != drw_w || src_h != drw_h) : FALSE;

	if (stretch_blit) {
		c2dSetStretchMode(imxPtr->xvGpuContext, C2D_STRETCH_BILINEAR_SAMPLING);
		/* c2d_z160: the above seems to set the _general_ sampling, not just at stretching. */
	}

	C2D_RECT rectDst = {
		.x = drw_x,
		.y = drw_y,
		.width = drw_w,
		.height = drw_h
	};
	C2D_RECT rectSrc = {
		.x = src_x,
		.y = src_y,
		.width = src_w,
		.height = src_h
	};

	C2D_RECT rectDstAux;
	C2D_RECT rectSrcAux;

	const Bool full_screen = 
		0 == pDraw->x &&
		0 == pDraw->y &&
		pDraw->pScreen->width == pDraw->width &&
		pDraw->pScreen->height == pDraw->height;

	PixmapPtr pxDst = NULL;
	C2D_SURFACE surfDst = imxPtr->xvScreenSurf;

	if (!full_screen) {

		if (DRAWABLE_WINDOW == pDraw->type)
			pxDst = pDraw->pScreen->GetWindowPixmap((WindowPtr) pDraw);
		else
			pxDst = (PixmapPtr) pDraw;

		IMXEXAPixmapPtr pxPriv =
			(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pxDst);

		if (NULL == pxPriv->surf) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVPutImage called with a drawable whose pixmap is not in gpumem; bailing out\n");

			return BadAlloc;
		}

		surfDst = pxPriv->surf;

		rectDst.x += pxDst->drawable.x - pxDst->screen_x;
		rectDst.y += pxDst->drawable.y - pxDst->screen_y;
	}

	Bool split_blit = FALSE;
	const int src_end_x = src_x + src_w;

	if (IMXXV_MAX_BLIT_COORD < src_end_x) {

		const int aux_src_x = 0 < src_x - IMXXV_MAX_BLIT_COORD ?
			src_x - IMXXV_MAX_BLIT_COORD : 0;

		rectSrcAux.x = aux_src_x;
		rectSrcAux.y = src_y;
		rectSrcAux.width = src_end_x - IMXXV_MAX_BLIT_COORD - aux_src_x;
		rectSrcAux.height = src_h;

		const float scale_factor = (float) drw_w / src_w;

		rectDstAux.x = drw_x + (int) ((IMXXV_MAX_BLIT_COORD + rectSrcAux.x) * scale_factor);
		rectDstAux.y = drw_y;
		rectDstAux.width = (int) ceilf(rectSrcAux.width * scale_factor);
		rectDstAux.height = drw_h;

		if (src_w != rectSrcAux.width) {
			rectDst.width = drw_w - rectDstAux.width;
			rectSrc.width = src_w - rectSrcAux.width;
		}
		else {
			rectDst.width = 0;
			rectSrc.width = 0;
		}

		if (!imxPtr->xvPort[port_idx].report_split) {

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"IMXXVPutImage split blit "
				"src0 x:%d w:%d, dst0 x:%d w:%d, "
				"src1 x:%d w:%d, dst1 x:%d w:%d\n",
				rectSrc.x,
				rectSrc.width,
				rectDst.x,
				rectDst.width,
				rectSrcAux.x,
				rectSrcAux.width,
				rectDstAux.x,
				rectDstAux.width);

			imxPtr->xvPort[port_idx].report_split = TRUE;
		}

		split_blit = TRUE;

#if IMXXV_DBLFB_ENABLE

		imxPtr->xvBufferTracker ^= 1;

		if (full_screen && imxPtr->xvBufferTracker)
			c2dSetDstSurface(imxPtr->xvGpuContext, imxPtr->xvScreenSurf2);
		else
			c2dSetDstSurface(imxPtr->xvGpuContext, surfDst);

#else /* IMXXV_DBLFB_ENABLE */

		c2dSetDstSurface(imxPtr->xvGpuContext, surfDst);

#endif /* IMXXV_DBLFB_ENABLE */

		if (!full_screen) {

			rectDstAux.x += pxDst->drawable.x - pxDst->screen_x;
			rectDstAux.y += pxDst->drawable.y - pxDst->screen_y;
		}
	}
	else {

		c2dSetDstSurface(imxPtr->xvGpuContext, surfDst);
		c2dSetSrcSurface(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surf);

		c2dSetSrcRectangle(imxPtr->xvGpuContext, &rectSrc);
		c2dSetDstRectangle(imxPtr->xvGpuContext, &rectDst);
	}

	int num_box = RegionNumRects(clipBoxes);
	BoxPtr box = RegionRects(clipBoxes);

	for (; num_box--; ++box) {

		C2D_RECT rectClip = {
			.x = box->x1,
			.y = box->y1,
			.width = box->x2 - box->x1,
			.height = box->y2 - box->y1
		};

		if (!full_screen) {

			rectClip.x += pxDst->drawable.x - pxDst->screen_x;
			rectClip.y += pxDst->drawable.y - pxDst->screen_y;
		}

		c2dSetDstClipRect(imxPtr->xvGpuContext, &rectClip);

		if (split_blit) {
			
			if (rectSrc.width &&
				rectDst.x < rectClip.x + rectClip.width &&
				rectDst.y < rectClip.y + rectClip.height &&
				rectClip.x < rectDst.x + rectDst.width &&
				rectClip.y < rectDst.y + rectDst.height) {

				c2dSetSrcSurface(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surf);

				c2dSetSrcRectangle(imxPtr->xvGpuContext, &rectSrc);
				c2dSetDstRectangle(imxPtr->xvGpuContext, &rectDst);

				r = c2dDrawBlit(imxPtr->xvGpuContext);

				if (C2D_STATUS_OK != r)
					break;
			}

			if (rectDstAux.x >= rectClip.x + rectClip.width ||
				rectDstAux.y >= rectClip.y + rectClip.height ||
				rectClip.x >= rectDstAux.x + rectDstAux.width ||
				rectClip.y >= rectDstAux.y + rectDstAux.height) {

				continue;
			}

			c2dSetSrcSurface(imxPtr->xvGpuContext, imxPtr->xvPort[port_idx].surfAux);

			c2dSetSrcRectangle(imxPtr->xvGpuContext, &rectSrcAux);
			c2dSetDstRectangle(imxPtr->xvGpuContext, &rectDstAux);
		}

		r = c2dDrawBlit(imxPtr->xvGpuContext);

		if (C2D_STATUS_OK != r)
			break;
	}

	/* Reset clipping and various static draw parameters. */
	c2dSetDstClipRect(imxPtr->xvGpuContext, NULL);

	if (dither_blit)
		c2dSetDither(imxPtr->xvGpuContext, 0);

	if (stretch_blit) {
		c2dSetStretchMode(imxPtr->xvGpuContext, C2D_STRETCH_POINT_SAMPLING);
		/* c2d_z160: the above seems to set the _general_ sampling, not just at stretching. */
	}

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVPutImage failed to perform GPU draw (code: 0x%08x)\n", r);

		return BadMatch;
	}

	/* This is a synchronous movie sequence, show the individual frames on screen ASAP. */
	/* Note: Using GPU flush effectively doubles the CPU load at presenting a frame, but the */
	/* frame reaches the screen sooner, as long as the required CPU resource is available. */

#if IMXXV_DBLFB_ENABLE

	if (full_screen && split_blit) {

		c2dFinish(imxPtr->xvGpuContext);

		const int fd = fbdevHWGetFD(pScrn);

		struct fb_var_screeninfo varinfo;

		if (-1 == ioctl(fd, FBIOGET_VSCREENINFO, &varinfo)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVPutImage failed at get_vscreeninfo ioctl (errno: %s)\n",
				strerror(errno));
		}

		varinfo.yoffset = varinfo.yres * imxPtr->xvBufferTracker;

		if (-1 == ioctl(fd, FBIOPAN_DISPLAY, &varinfo)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVPutImage failed at pan_display ioctl (errno: %s)\n",
				strerror(errno));
		}
	}
	else
		c2dFlush(imxPtr->xvGpuContext);

	if (!full_screen)
		DamageDamageRegion(pDraw, clipBoxes);

#else /* IMXXV_DBLFB_ENABLE */

	c2dFlush(imxPtr->xvGpuContext);

	if (!full_screen)
		DamageDamageRegion(pDraw, clipBoxes);

#endif /* IMXXV_DBLFB_ENABLE */

	return Success;
}

static int
IMXXVQueryImageAttributes(
	ScrnInfoPtr pScrn,
	int image,
	unsigned short *width,
	unsigned short *height,
	int *pitches,
	int *offsets)
{
	if (NULL == width || NULL == height)
		return 0;

	int w = *width;
	int h = *height;

	if (w > IMXXV_MAX_IMG_WIDTH)
		w = IMXXV_MAX_IMG_WIDTH;

	if (h > IMXXV_MAX_IMG_HEIGHT)
		h = IMXXV_MAX_IMG_HEIGHT;

	if (offsets)
		offsets[0] = 0;

	int size = 0;

	switch (image) {
	case FOURCC_YVYU:
	case FOURCC_UYVY:
	case FOURCC_YUY2:
		w = (w + 1) & ~1;
		size = 2 * w * h;
		if (pitches)
			pitches[0] = 2 * w;
		break;
	case FOURCC_YV12:
	case FOURCC_I420:
		w = (w + 1) & ~1;
		h = (h + 1) & ~1;
		size = w * h + w * h / 2;
		if (pitches) {
			pitches[0] = w;
			pitches[1] = w / 2;
			pitches[2] = w / 2;
		}
		if (offsets) {
			offsets[1] = w * h;
			offsets[2] = w * h + w * h / 4;
		}
		break;
	}

	*width = w;
	*height = h;

	return size;
}

static int
imxxv_init_adaptor(
	ScreenPtr pScreen,
	ScrnInfoPtr pScrn,
	XF86VideoAdaptorPtr **pppAdaptor)
{
	/* Allocate one Xv adaptor. */
	XF86VideoAdaptorPtr pAdaptor;

	if (!(pAdaptor = xf86XVAllocateVideoAdaptorRec(pScrn)))
		return 0;

	/* Allocate and return an array for one adaptor pointer. */
	*pppAdaptor = xnfalloc(sizeof(pAdaptor));
	**pppAdaptor = pAdaptor;

	IMXPtr imxPtr = IMXPTR(pScrn);

	/* Define ports in this adaptor and make them use our private DevUnion Xv record. */
	pAdaptor->nPorts = IMXXV_NUM_PORTS;
	pAdaptor->pPortPrivates = imxPtr->xvPortPrivate;
	unsigned i;
	for (i = 0; i < IMXXV_NUM_PORTS; ++i)
		pAdaptor->pPortPrivates[i].ptr = imxxv_port_cookie_from_idx(imxPtr, i);

	pAdaptor->type = XvInputMask | XvImageMask | XvWindowMask;
	pAdaptor->flags = VIDEO_OVERLAID_IMAGES | VIDEO_CLIP_TO_VIEWPORT;
	pAdaptor->name = "Freescale i.MX5x GPU (z160) Overlay Scaler";
	pAdaptor->nEncodings = sizeof(imxVideoEncoding) / sizeof(imxVideoEncoding[0]);
	pAdaptor->pEncodings = imxVideoEncoding;
	pAdaptor->nFormats = sizeof(imxVideoFormat) / sizeof(imxVideoFormat[0]);
	pAdaptor->pFormats = imxVideoFormat;
	pAdaptor->nAttributes = IMXXV_NUM_ATTR;
	pAdaptor->pAttributes = imxPortAttribute;
	pAdaptor->nImages = sizeof(imxImage) / sizeof(imxImage[0]);
	pAdaptor->pImages = imxImage;

	pAdaptor->StopVideo            = IMXXVStopVideo;
	pAdaptor->SetPortAttribute     = IMXXVSetPortAttribute;
	pAdaptor->GetPortAttribute     = IMXXVGetPortAttribute;
	pAdaptor->QueryBestSize        = IMXXVQueryBestSize;
	pAdaptor->PutImage             = IMXXVPutImage;
	pAdaptor->QueryImageAttributes = IMXXVQueryImageAttributes;

	/* Produce atoms for all port attributes. */
	int idx;

	for (idx = 0; idx < IMXXV_NUM_ATTR; ++idx)
		imxAttributeInfo[idx].attribute = MakeAtom(imxPortAttribute[idx].name,
			strlen(imxPortAttribute[idx].name), TRUE);

	return 1;
}

int
IMXXVInitAdaptorC2D(
	ScrnInfoPtr pScrn,
	XF86VideoAdaptorPtr **pppAdaptor)
{
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	if (IMXEXA_BACKEND_Z160 == imxPtr->backend) {

		imxPtr->xvGpuContext = fPtr->gpuContext;
		imxPtr->xvScreenSurf = fPtr->screenSurf;
	}
	else {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"XV adaptor does not currently support the active EXA backend.\n");
		return 0;
	}

#if IMXXV_DBLFB_ENABLE

	const int fd = fbdevHWGetFD(pScrn);

	struct fb_fix_screeninfo fixinfo;
	struct fb_var_screeninfo varinfo;

	ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo);
	ioctl(fd, FBIOGET_VSCREENINFO, &varinfo);

	if (varinfo.yres_virtual < varinfo.yres * 2) {

		varinfo.yres_virtual = varinfo.yres * 2;
		varinfo.yoffset = 0;

		if (-1 == ioctl(fd, FBIOPUT_VSCREENINFO, &varinfo)) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVInitAdaptor failed at put_vscreeninfo ioctl (errno: %s)\n",
				strerror(errno));

			return 0;
		}
	}

	C2D_SURFACE_DEF surfDef;
	memset(&surfDef, 0, sizeof(surfDef));

	surfDef.format = fPtr->screenSurfDef.format;
	surfDef.width  = varinfo.xres;
	surfDef.height = varinfo.yres;
	surfDef.stride = varinfo.xres_virtual * varinfo.bits_per_pixel / 8;
	surfDef.buffer = (uint8_t *) fixinfo.smem_start + varinfo.yres * surfDef.stride;
	surfDef.host   = NULL; /* We don't intend to ever lock this surface. */
	surfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

	C2D_STATUS r = c2dSurfAlloc(imxPtr->xvGpuContext, &imxPtr->xvScreenSurf2, &surfDef);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVInitAdaptor failed to allocate surface for screen (code: 0x%08x)\n", r);

		return 0;
	}

	/* Wipe out the new surface to RGB black. */
	imxxv_fill_surface(imxPtr->xvGpuContext, imxPtr->xvScreenSurf2, 0U);

#endif /* IMXXV_DBLFB_ENABLE */

	/* This early during driver init ScrnInfoPtr does not have a valid ScreenPtr yet. */
	ScreenPtr pScreen = screenInfo.screens[pScrn->scrnIndex];

	XF86VideoAdaptorPtr *ppAdaptor = NULL;
	const int nAdaptor = imxxv_init_adaptor(pScreen, pScrn, &ppAdaptor);

	if (pppAdaptor)
		*pppAdaptor = ppAdaptor;

	return nAdaptor;
}
