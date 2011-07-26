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

#define IMXXV_VSYNC_ENABLE		0	/* flag: wait for vsync before showing a frame */
#define IMXXV_VSYNC_NUM_RETRIES	0	/* max num of vsync retries due to syscall interrupts */
#define IMXXV_VSYNC_DEBUG		0	/* flag: report wait-for-vsync ioctl errors */

#include <xf86.h>
#include <xf86xv.h>
#include <X11/extensions/Xv.h>
#include <fourcc.h>
#include <dlfcn.h>
#include <fbdevhw.h>

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

#ifndef FOURCC_YVYU
#define FOURCC_YVYU 0x55595659 /* 'YVYU' in little-endian */
#endif

#define IMXXV_MAX_IMG_WIDTH		2048 /* Must be even. */
#define IMXXV_MAX_IMG_HEIGHT	1024 /* Must be even. */

#define IMXXV_MAX_OUT_WIDTH		2048 /* Z160 cannot scale beyond this width. */
#define IMXXV_MAX_OUT_HEIGHT	2048 /* Z160 cannot scale beyond this height. */

#define IMXXV_MAX_BLIT_COORD	1024 /* Z160 cannot address YUV surfaces beyond this src coord. */

/* Hooks to c2d_z160 API. */
typedef C2D_STATUS (*Z2DCreateContext)(C2D_CONTEXT *a_c2dContext);
typedef C2D_STATUS (*Z2DDestroyContext)(C2D_CONTEXT a_c2dContext);
typedef C2D_STATUS (*Z2DSurfAlloc)(C2D_CONTEXT a_c2dContext, C2D_SURFACE *a_c2dSurface, C2D_SURFACE_DEF *a_surfaceDef);
typedef C2D_STATUS (*Z2DSurfFree)(C2D_CONTEXT a_c2dContext, C2D_SURFACE a_c2dSurface);
typedef C2D_STATUS (*Z2DSurfLock)(C2D_CONTEXT a_c2dContext, C2D_SURFACE a_c2dSurface, void** a_ptr);
typedef C2D_STATUS (*Z2DSurfUnlock)(C2D_CONTEXT a_c2dContext, C2D_SURFACE a_c2dSurface);
typedef C2D_STATUS (*Z2DSetDstSurface)(C2D_CONTEXT a_c2dContext, C2D_SURFACE a_c2dSurface);
typedef C2D_STATUS (*Z2DSetSrcSurface)(C2D_CONTEXT a_c2dContext, C2D_SURFACE a_c2dSurface);
typedef C2D_STATUS (*Z2DSetBrushSurface)(C2D_CONTEXT a_c2dContext, C2D_SURFACE a_c2dSurface, C2D_POINT *a_tilingOffset);
typedef C2D_STATUS (*Z2DSetMaskSurface)(C2D_CONTEXT a_c2dContext, C2D_SURFACE a_c2dSurface, C2D_POINT *a_offset);
typedef C2D_STATUS (*Z2DSetSrcRectangle)(C2D_CONTEXT a_c2dContext, C2D_RECT *a_rect);
typedef C2D_STATUS (*Z2DSetDstRectangle)(C2D_CONTEXT a_c2dContext, C2D_RECT *a_rect);
typedef C2D_STATUS (*Z2DSetDstClipRect)(C2D_CONTEXT a_c2dContext, C2D_RECT *a_clipRect);
typedef C2D_STATUS (*Z2DDrawBlit)(C2D_CONTEXT a_c2dContext);
typedef C2D_STATUS (*Z2DDrawRect)(C2D_CONTEXT a_c2dContext, C2D_PARAMETERS a_drawConfig);
typedef C2D_STATUS (*Z2DFlush)(C2D_CONTEXT a_c2dContext);
typedef C2D_STATUS (*Z2DFinish)(C2D_CONTEXT a_c2dContext);
typedef C2D_STATUS (*Z2DSetStretchMode)(C2D_CONTEXT a_c2dContext, C2D_STRETCH_MODE a_mode);
typedef C2D_STATUS (*Z2DSetBlendMode)(C2D_CONTEXT a_c2dContext, C2D_ALPHA_BLEND_MODE a_mode);
typedef C2D_STATUS (*Z2DSetDither)(C2D_CONTEXT a_c2dContext, int a_bEnable);
typedef C2D_STATUS (*Z2DSetFgColor)(C2D_CONTEXT a_c2dContext, unsigned int a_fgColor);

static Z2DCreateContext		z2dCreateContext;
static Z2DDestroyContext	z2dDestroyContext;
static Z2DSurfAlloc			z2dSurfAlloc;
static Z2DSurfFree			z2dSurfFree;
static Z2DSurfLock			z2dSurfLock;
static Z2DSurfUnlock		z2dSurfUnlock;
static Z2DSetDstSurface		z2dSetDstSurface;
static Z2DSetSrcSurface		z2dSetSrcSurface;
static Z2DSetBrushSurface	z2dSetBrushSurface;
static Z2DSetMaskSurface	z2dSetMaskSurface;
static Z2DSetSrcRectangle	z2dSetSrcRectangle;
static Z2DSetDstRectangle	z2dSetDstRectangle;
static Z2DSetDstClipRect	z2dSetDstClipRect;
static Z2DDrawBlit			z2dDrawBlit;
static Z2DDrawRect			z2dDrawRect;
static Z2DFlush				z2dFlush;
static Z2DFinish			z2dFinish;
static Z2DSetStretchMode	z2dSetStretchMode;
static Z2DSetBlendMode		z2dSetBlendMode;
static Z2DSetDither			z2dSetDither;
static Z2DSetFgColor		z2dSetFgColor;

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

#if IMXXV_VSYNC_ENABLE

static inline void
imxxv_wait_for_vsync(
	ScrnInfoPtr pScrn)
{
	const int fd = fbdevHWGetFD(pScrn);

	int res, count = 0;

	/* Waiting for vsync is an interruptable syscall, we need to persist. */
	do {
		res = ioctl(fd, MXCFB_WAIT_FOR_VSYNC, 0);
		++count;
	} while (-1 == res && EINTR == errno && count < IMXXV_VSYNC_NUM_RETRIES + 1);

#if IMXXV_VSYNC_DEBUG

	if (-1 == res)
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVPutImage failed at wait_for_vsync ioctl (errno: %s, num tries: %d)\n",
			strerror(errno), count);
	}

#endif /* IMXXV_VSYNC_DEBUG */

}

#endif /* IMXXV_VSYNC_ENABLE */

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
	z2dSurfFree(imxPtr->xvGpuContext, imxPtr->xvSurf[port_idx]);

	imxPtr->xvSurf[port_idx] = NULL;
	memset(&imxPtr->xvSurfDef[port_idx], 0, sizeof(imxPtr->xvSurfDef[0]));

	if (NULL != imxPtr->xvSurfAux[port_idx]) {

		z2dSurfFree(imxPtr->xvGpuContext, imxPtr->xvSurfAux[port_idx]);
		imxPtr->xvSurfAux[port_idx] = NULL;
	}

	imxPtr->report_split[port_idx] = FALSE;
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

		if (NULL != imxPtr->xvSurf[port_idx]) {

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

		}

		/*	TODO: physical buffer records need to be per port, to avoid the mmap stress
			from closing a port while another one is still playing. */
		unsigned i;

		for (i = 0; i < imxPtr->num_phys; ++i) {

			if (NULL != imxPtr->mapping[i])
				munmap(imxPtr->mapping[i], imxPtr->mapping_len[i]);

			imxPtr->phys_ptr[i] = 0;
			imxPtr->mapping[i] = NULL;
			imxPtr->mapping_len[i] = 0;
			imxPtr->mapping_offset[i] = 0;
		}

		imxPtr->num_phys = 0;
	}
}

static inline unsigned
imxxv_seek_mapping(
	const IMXPtr imxPtr,
	const intptr_t phys_ptr)
{
	unsigned i;

	for (i = 0; i < imxPtr->num_phys; ++i)
		if (phys_ptr == imxPtr->phys_ptr[i])
			return i;

	return -1U;
}

static inline void
imxxv_fill_surface(
	const C2D_CONTEXT context,
	const C2D_SURFACE surf,
	const uint32_t color)
{
	z2dSetDstSurface(context, surf);
	z2dSetSrcSurface(context, NULL);
	z2dSetBrushSurface(context, NULL, NULL);
	z2dSetMaskSurface(context, NULL, NULL);
	z2dSetBlendMode(context, C2D_ALPHA_BLEND_NONE);
	z2dSetFgColor(context, color);

	z2dDrawRect(context, C2D_PARAM_FILL_BIT);
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

	if (NULL != imxPtr->xvSurf[port_idx] &&
		(fmt != imxPtr->xvSurfDef[port_idx].format ||
		 width > imxPtr->xvSurfDef[port_idx].width ||
		 height > imxPtr->xvSurfDef[port_idx].height)) {

		imxxv_delete_port_surface(imxPtr, port_idx);
	}

	if (NULL == imxPtr->xvSurf[port_idx]) {

		imxPtr->xvSurfDef[port_idx].format	= fmt;
		imxPtr->xvSurfDef[port_idx].width	= width;
		imxPtr->xvSurfDef[port_idx].height	= height;

		C2D_STATUS r;
		r = z2dSurfAlloc(imxPtr->xvGpuContext, &imxPtr->xvSurf[port_idx], &imxPtr->xvSurfDef[port_idx]);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVPutImage failed to allocate GPU surface (code: 0x%08x)\n", r);

			return BadAlloc;
		}

		/* Wipe out the new surface to YUY2 black. */
		imxxv_fill_surface(imxPtr->xvGpuContext, imxPtr->xvSurf[port_idx], 0x800000U);

		if (width > IMXXV_MAX_BLIT_COORD) {

			C2D_SURFACE_DEF surfDef;
			memcpy(&surfDef, &imxPtr->xvSurfDef[port_idx], sizeof(surfDef));

			surfDef.width  = width - IMXXV_MAX_BLIT_COORD;
			surfDef.buffer = (char *) surfDef.buffer + IMXXV_MAX_BLIT_COORD * bytespp;
			surfDef.host   = (char *) surfDef.host + IMXXV_MAX_BLIT_COORD * bytespp;
			surfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

			r = z2dSurfAlloc(imxPtr->xvGpuContext, &imxPtr->xvSurfAux[port_idx], &surfDef);

			if (C2D_STATUS_OK != r) {

				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"IMXXVPutImage failed to allocate GPU surface (code: 0x%08x)\n", r);

				return BadAlloc;
			}
		}
	}

	/* gstreamer physical buffer support */
	if (0xbeefc0de == ((intptr_t*) buf)[0]) {

		unsigned idx = imxxv_seek_mapping(imxPtr, ((intptr_t*) buf)[1]);

		if (-1U == idx) {

			if (IMXXV_NUM_PHYS_BUFFERS > imxPtr->num_phys) {

				idx = imxPtr->num_phys++;

				const int pagemask = getpagesize() - 1;
				const intptr_t phys_ptr = ((intptr_t*) buf)[1];
				const intptr_t phys_page_ptr = phys_ptr & ~pagemask;
				const size_t src_len = width * height + width * height / 2; /* planar YUV assumed */

				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
					"IMXXVPutImage detected physical buffer at input; mapping phys memory from 0x%08x..\n",
					phys_ptr);

				const int fd = open("/dev/mem", O_RDWR);

				imxPtr->phys_ptr[idx] = phys_ptr;
				imxPtr->mapping_offset[idx] = phys_ptr - phys_page_ptr;
				imxPtr->mapping_len[idx] = (imxPtr->mapping_offset[idx] + src_len + pagemask) & ~pagemask;
				imxPtr->mapping[idx] = mmap(0, imxPtr->mapping_len[idx], PROT_READ, MAP_SHARED, fd, phys_page_ptr);

				close(fd);

				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
					"IMXXVPutImage mapping done. Input src len 0x%08x, phys buffer len 0x%08x, virtual mapping %p\n",
					src_len, imxPtr->mapping_len[idx], imxPtr->mapping[idx]);
			}
			else {
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"IMXXVPutImage is unable to perform virtual mapping of physical ptr 0x%08x\n",
					((intptr_t*) buf)[1]);

				idx = 0;
			}
		}

		buf = (unsigned char*) imxPtr->mapping[idx] + imxPtr->mapping_offset[idx];
	}

	unsigned char* bits;
	C2D_STATUS r;

	/* Access-lock the Xv GPU surface. */
	r = z2dSurfLock(imxPtr->xvGpuContext, imxPtr->xvSurf[port_idx], (void**) &bits);

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

		const unsigned int dst_stride = imxPtr->xvSurfDef[port_idx].stride;
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

		const unsigned int dst_stride = imxPtr->xvSurfDef[port_idx].stride;
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
	z2dSurfUnlock(imxPtr->xvGpuContext, imxPtr->xvSurf[port_idx]);

	/* Set various static draw parameters. */
	z2dSetBrushSurface(imxPtr->xvGpuContext, NULL, NULL);
	z2dSetMaskSurface(imxPtr->xvGpuContext, NULL, NULL);

	z2dSetBlendMode(imxPtr->xvGpuContext, C2D_ALPHA_BLEND_NONE);

	const Bool dither_blit = 16 == pScrn->bitsPerPixel;

	if (dither_blit)
		z2dSetDither(imxPtr->xvGpuContext, 1);

	const Bool stretch_blit = imxPtr->use_bilinear_filtering ?
		(src_w != drw_w || src_h != drw_h) : FALSE;

	if (stretch_blit) {
		z2dSetStretchMode(imxPtr->xvGpuContext, C2D_STRETCH_BILINEAR_SAMPLING);
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
	Bool split_blit = FALSE;
	Bool full_screen = 
		0 == pDraw->x &&
		0 == pDraw->y &&
		pDraw->pScreen->width == pDraw->width &&
		pDraw->pScreen->height == pDraw->height;

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

		if (!imxPtr->report_split[port_idx]) {

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

			imxPtr->report_split[port_idx] = TRUE;
		}

		split_blit = TRUE;

#if IMXXV_DBLFB_ENABLE

		imxPtr->xvBufferTracker ^= 1;

		if (full_screen && imxPtr->xvBufferTracker)
			z2dSetDstSurface(imxPtr->xvGpuContext, imxPtr->xvScreenSurf2);
		else
			z2dSetDstSurface(imxPtr->xvGpuContext, imxPtr->xvScreenSurf);
#else

		z2dSetDstSurface(imxPtr->xvGpuContext, imxPtr->xvScreenSurf);

#endif /* IMXXV_DBLFB_ENABLE */

	}
	else {
		z2dSetDstSurface(imxPtr->xvGpuContext, imxPtr->xvScreenSurf);
		z2dSetSrcSurface(imxPtr->xvGpuContext, imxPtr->xvSurf[port_idx]);

		z2dSetSrcRectangle(imxPtr->xvGpuContext, &rectSrc);
		z2dSetDstRectangle(imxPtr->xvGpuContext, &rectDst);
	}

#if IMXXV_VSYNC_ENABLE

	/* Wait for vsync to avoid frame tearing. */
	imxxv_wait_for_vsync(pScrn);

#endif /* IMXXV_VSYNC_ENABLE */

	int num_box = RegionNumRects(clipBoxes);
	BoxPtr box = RegionRects(clipBoxes);

	for (; num_box--; ++box) {

		C2D_RECT rectClip = {
			.x = box->x1,
			.y = box->y1,
			.width = box->x2 - box->x1,
			.height = box->y2 - box->y1
		};

		z2dSetDstClipRect(imxPtr->xvGpuContext, &rectClip);

		if (split_blit) {
			
			if (rectSrc.width &&
				rectDst.x < box->x2 &&
				rectDst.y < box->y2 &&
				rectDst.x + rectDst.width > box->x1 &&
				rectDst.y + rectDst.height > box->y1) {

				z2dSetSrcSurface(imxPtr->xvGpuContext, imxPtr->xvSurf[port_idx]);

				z2dSetSrcRectangle(imxPtr->xvGpuContext, &rectSrc);
				z2dSetDstRectangle(imxPtr->xvGpuContext, &rectDst);

				r = z2dDrawBlit(imxPtr->xvGpuContext);

				if (C2D_STATUS_OK != r)
					break;
			}

			if (rectDstAux.x >= box->x2 ||
				rectDstAux.y >= box->y2 ||
				rectDstAux.x + rectDstAux.width <= box->x1 ||
				rectDstAux.y + rectDstAux.height <= box->y1) {

				continue;
			}

			z2dSetSrcSurface(imxPtr->xvGpuContext, imxPtr->xvSurfAux[port_idx]);

			z2dSetSrcRectangle(imxPtr->xvGpuContext, &rectSrcAux);
			z2dSetDstRectangle(imxPtr->xvGpuContext, &rectDstAux);
		}

		r = z2dDrawBlit(imxPtr->xvGpuContext);

		if (C2D_STATUS_OK != r)
			break;
	}

	/* Reset clipping and various static draw parameters. */
	z2dSetDstClipRect(imxPtr->xvGpuContext, NULL);

	if (dither_blit)
		z2dSetDither(imxPtr->xvGpuContext, 0);

	if (stretch_blit) {
		z2dSetStretchMode(imxPtr->xvGpuContext, C2D_STRETCH_POINT_SAMPLING);
		/* c2d_z160: the above seems to set the _general_ sampling, not just at stretching. */
	}

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVPutImage failed to perform GPU draw (code: 0x%08x)\n", r);

		return BadMatch;
	}
	else {
		/* This is a synchronous movie sequence, show the individual frames on screen ASAP. */
		/* Note: Next GPU flush effectively doubles the CPU load at presenting a frame, but the */
		/* frame reaches the screen sooner, as long as the required CPU resource is available. */

#if IMXXV_DBLFB_ENABLE

		if (full_screen && split_blit) {

			z2dFinish(imxPtr->xvGpuContext);

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
			z2dFlush(imxPtr->xvGpuContext);
#else

		z2dFlush(imxPtr->xvGpuContext);

#endif /* IMXXV_DBLFB_ENABLE */

	}

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
	static const char lib_name[] = "libc2d_z160.so";

	void* library = dlopen(lib_name, RTLD_NOW | RTLD_GLOBAL);

	if (!library) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVInitAdaptor failed to open C2D Z160 library (dlerror: %s)\n",
			dlerror());

		return 0;
	}

	/* Reset dlerror state. */
	dlerror();

	z2dCreateContext	= (Z2DCreateContext)	dlsym(library, "c2dCreateContext");
	z2dDestroyContext	= (Z2DDestroyContext)	dlsym(library, "c2dDestroyContext");
	z2dSurfAlloc		= (Z2DSurfAlloc)		dlsym(library, "c2dSurfAlloc");
	z2dSurfFree			= (Z2DSurfFree)			dlsym(library, "c2dSurfFree");
	z2dSurfLock			= (Z2DSurfLock)			dlsym(library, "c2dSurfLock");
	z2dSurfUnlock		= (Z2DSurfUnlock)		dlsym(library, "c2dSurfUnlock");
	z2dSetDstSurface	= (Z2DSetDstSurface)	dlsym(library, "c2dSetDstSurface");
	z2dSetSrcSurface	= (Z2DSetSrcSurface)	dlsym(library, "c2dSetSrcSurface");
	z2dSetBrushSurface	= (Z2DSetBrushSurface)	dlsym(library, "c2dSetBrushSurface");
	z2dSetMaskSurface	= (Z2DSetMaskSurface)	dlsym(library, "c2dSetMaskSurface");
	z2dSetDstRectangle	= (Z2DSetDstRectangle)	dlsym(library, "c2dSetDstRectangle");
	z2dSetSrcRectangle	= (Z2DSetSrcRectangle)	dlsym(library, "c2dSetSrcRectangle");
	z2dSetDstClipRect	= (Z2DSetDstClipRect)	dlsym(library, "c2dSetDstClipRect");
	z2dDrawBlit			= (Z2DDrawBlit)			dlsym(library, "c2dDrawBlit");
	z2dDrawRect			= (Z2DDrawRect)			dlsym(library, "c2dDrawRect");
	z2dFlush			= (Z2DFlush)			dlsym(library, "c2dFlush");
	z2dFinish			= (Z2DFinish)			dlsym(library, "c2dFinish");
	z2dSetStretchMode	= (Z2DSetStretchMode)	dlsym(library, "c2dSetStretchMode");
	z2dSetBlendMode		= (Z2DSetBlendMode)		dlsym(library, "c2dSetBlendMode");
	z2dSetDither		= (Z2DSetDither)		dlsym(library, "c2dSetDither");
	z2dSetFgColor		= (Z2DSetFgColor)		dlsym(library, "c2dSetFgColor");

	const char* err = dlerror();

	if (NULL != err) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVInitAdaptor failed to load symbol from C2D Z160 library (dlerror: %s)\n",
			err);

		dlclose(library);
		return 0;
	}

	IMXPtr imxPtr = IMXPTR(pScrn);

	const int fd = fbdevHWGetFD(pScrn);

	struct fb_fix_screeninfo fixinfo;
	struct fb_var_screeninfo varinfo;

	ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo);
	ioctl(fd, FBIOGET_VSCREENINFO, &varinfo);

	C2D_SURFACE_DEF surfDef;
	memset(&surfDef, 0, sizeof(surfDef));

	C2D_STATUS r;

	if (IMXEXA_BACKEND_Z160 == imxPtr->backend) {

		IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

		imxPtr->xvGpuContext = fPtr->gpuContext;
		imxPtr->xvScreenSurf = fPtr->screenSurf;

#if IMXXV_DBLFB_ENABLE

		surfDef.format = fPtr->screenSurfDef.format;
		surfDef.width  = varinfo.xres;
		surfDef.height = varinfo.yres;
		surfDef.stride = fixinfo.line_length;
		surfDef.buffer = (uint8_t *) fixinfo.smem_start + varinfo.yres * fixinfo.line_length;
		surfDef.host   = NULL; /* We don't intend to ever lock this surface. */
		surfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

#endif /* IMXXV_DBLFB_ENABLE */

	}
	else {

		r = z2dCreateContext(&imxPtr->xvGpuContext);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVInitAdaptor failed to acquire GPU context (code: 0x%08x)\n",
				r);

			dlclose(library);
			return 0;
		}

		C2D_COLORFORMAT fmt;

		switch (pScrn->bitsPerPixel) {

		case 8: 
			fmt = C2D_COLOR_8;
			break;

		case 16:
			fmt = C2D_COLOR_0565;
			break;

		case 24:
			fmt = C2D_COLOR_888;
			break;

		case 32:
			fmt = C2D_COLOR_8888;
			break;

		default:
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVInitAdaptor failed to allocate surface for screen due to unsupported bpp (%d)\n",
				pScrn->bitsPerPixel);

			z2dDestroyContext(imxPtr->xvGpuContext);
			imxPtr->xvGpuContext = NULL;

			dlclose(library);
			return 0;
		}

		surfDef.format = fmt;
		surfDef.width  = varinfo.xres;
		surfDef.height = varinfo.yres;
		surfDef.stride = fixinfo.line_length;
		surfDef.buffer = (void *) fixinfo.smem_start;
		surfDef.host   = NULL; /* We don't intend to ever lock this surface. */
		surfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

		r = z2dSurfAlloc(imxPtr->xvGpuContext, &imxPtr->xvScreenSurf, &surfDef);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVInitAdaptor failed to allocate surface for screen (code: 0x%08x)\n", r);

			z2dDestroyContext(imxPtr->xvGpuContext);
			imxPtr->xvGpuContext = NULL;

			dlclose(library);
			return 0;
		}

#if IMXXV_DBLFB_ENABLE

		surfDef.format = fmt;
		surfDef.width  = varinfo.xres;
		surfDef.height = varinfo.yres;
		surfDef.stride = fixinfo.line_length;
		surfDef.buffer = (uint8_t *) fixinfo.smem_start + varinfo.yres * fixinfo.line_length;
		surfDef.host   = NULL; /* We don't intend to ever lock this surface. */
		surfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

#endif /* IMXXV_DBLFB_ENABLE */

	}

#if IMXXV_DBLFB_ENABLE

	if (varinfo.yres_virtual < varinfo.yres * 2) {

		varinfo.yres_virtual = varinfo.yres * 2;
		varinfo.yoffset = 0;

		if (-1 == ioctl(fd, FBIOPUT_VSCREENINFO, &varinfo)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXXVInitAdaptor failed at put_vscreeninfo ioctl (errno: %s)\n",
				strerror(errno));
		}
	}

	r = z2dSurfAlloc(imxPtr->xvGpuContext, &imxPtr->xvScreenSurf2, &surfDef);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXXVInitAdaptor failed to allocate surface for screen (code: 0x%08x)\n", r);

		z2dDestroyContext(imxPtr->xvGpuContext);
		imxPtr->xvGpuContext = NULL;

		dlclose(library);
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
