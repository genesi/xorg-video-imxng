/*
 * Copyright (C) 2009-2011 Freescale Semiconductor, Inc.  All Rights Reserved.
 * Copyright (C) 2011 Genesi USA, Inc. All Rights Reserved.
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
#include <fbdevhw.h>
#include <exa.h>

#include <sys/ioctl.h>
#include <linux/fb.h>
#include <errno.h>
#include <unistd.h>

/* Preparation for the inclusion of c2d_api.h */
#ifndef _LINUX
#define _LINUX
#endif

#ifndef OS_DLLIMPORT
#define OS_DLLIMPORT
#endif

#include <C2D/c2d_api.h>

#include "imx_type.h"

#if IMX_EXA_VERSION_COMPILED < IMX_EXA_VERSION(2, 5, 0)
#error This driver can be built only against EXA version 2.5.0 or higher.
#endif

/* Minimal area of pixel surfaces for accelerating operations. */
#define IMX_EXA_MIN_SURF_AREA				2048 /* 4KB at 16bpp */
/* WARNING: Z160 backend may have stability issues with surface heights less than 32 (corrupted tooltips etc.). */
#define	IMX_EXA_MIN_SURF_HEIGHT				32
/* Maximal dimension of pixel surfaces for accelerating operations. */
#define IMX_EXA_MAX_SURF_DIM 				2048
/* NOTE: When scale-blitting Z160 cannot address a source beyond the 1024th row/column */
/* (it runs out of src coord bits and wraps around), but otherwise it can address 2048 units */
/* in each direction. Large pixmaps are usually identity-blitted, so we take the risk. */

/* This flag must be enabled to perform any debug logging */
#define IMX_EXA_DEBUG_MASTER				(0 && IMX_DEBUG_MASTER)

#define	IMX_EXA_DEBUG_INSTRUMENT_SYNCS		(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PREPARE_SOLID			(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_SOLID					(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PREPARE_COPY			(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_COPY					(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PIXMAPS				(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_CHECK_COMPOSITE		(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PREPARE_COMPOSITE		(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_COMPOSITE				(0 && IMX_EXA_DEBUG_MASTER)
#define IMX_EXA_DEBUG_EVICTION				(0 && IMX_EXA_DEBUG_MASTER)
#define IMX_EXA_DEBUG_DEMOTION				(0 && IMX_EXA_DEBUG_MASTER)
#define IMX_EXA_DEBUG_TRACE					(0 && IMX_EXA_DEBUG_MASTER)

#if IMX_EXA_DEBUG_TRACE

#define IMX_EXA_TRACE_MAGIC_WIDTH 			125 /* 123 + 2 for decoration */
#define IMX_EXA_TRACE_MAGIC_HEIGHT			486 /* 456 + 30 for decoration */

static unsigned trace;

static inline Bool is_tracing()
{
	return 2 == trace & 3;
}

#endif

static inline Bool
imxexa_surf_format_from_bpp(
	imxexa_backend_t backend,
	int bitsPerPixel,
	C2D_COLORFORMAT *fmt)
{
	switch (bitsPerPixel) {

	case 8:
		*fmt = C2D_COLOR_8;
		break;

	case 16:
		*fmt = C2D_COLOR_0565;
		break;

	case 24:
		*fmt = C2D_COLOR_888;
		break;

	case 32:
		*fmt = C2D_COLOR_8888;
		break;

	default:
		return FALSE;
	}

	return TRUE;
}

static inline Bool
imxexa_surf_format_from_pict(
	imxexa_backend_t backend,
	PictFormatShort pictFormat,
	C2D_COLORFORMAT *fmt)
{
	switch (pictFormat) {

	/* 8bpp formats */
	case PICT_a8:
		*fmt = C2D_COLOR_A8;
		break;

	case PICT_g8:
		*fmt = C2D_COLOR_8;
		break;

	/* 16bpp formats */
	case PICT_r5g6b5:
		*fmt = C2D_COLOR_0565;
		break;

	case PICT_a4r4g4b4:
	case PICT_x4r4g4b4:
		*fmt = C2D_COLOR_4444;
		break;

	case PICT_a1r5g5b5:
	case PICT_x1r5g5b5:
		*fmt = C2D_COLOR_1555;
		break;

	/* 24bpp formats */
	case PICT_r8g8b8:
		*fmt = C2D_COLOR_888;
		break;

	/* 32bpp formats */
	case PICT_a8r8g8b8:
	case PICT_x8r8g8b8:
		*fmt = C2D_COLOR_8888;
		break;

	case PICT_a8b8g8r8:
	case PICT_x8b8g8r8:
		*fmt = C2D_COLOR_8888_ABGR;
		break;

	default:
		return FALSE;
	}

	return TRUE;
}

static inline const char*
imxexa_string_from_pict_format(
	PictFormatShort pictFormat)
{
	switch (pictFormat) {
	/* 32bpp formats */
	case PICT_a2r10g10b10:
		return "PICT_a2r10g10b10";
	case PICT_x2r10g10b10:
		return "PICT_x2r10g10b10";
	case PICT_a2b10g10r10:
		return "PICT_a2b10g10r10";
	case PICT_x2b10g10r10:
		return "PICT_x2b10g10r10";

	case PICT_a8r8g8b8:
		return "PICT_a8r8g8b8";
	case PICT_x8r8g8b8:
		return "PICT_x8r8g8b8";
	case PICT_a8b8g8r8:
		return "PICT_a8b8g8r8";
	case PICT_x8b8g8r8:
		return "PICT_x8b8g8r8";
	case PICT_b8g8r8a8:
		return "PICT_b8g8r8a8";
	case PICT_b8g8r8x8:
		return "PICT_b8g8r8x8";

	/* 24bpp formats */
	case PICT_r8g8b8:
		return "PICT_r8g8b8";
	case PICT_b8g8r8:
		return "PICT_b8g8r8";

	/* 16bpp formats */
	case PICT_r5g6b5:
		return "PICT_r5g6b5";
	case PICT_b5g6r5:
		return "PICT_b5g6r5";

	case PICT_a1r5g5b5:
		return "PICT_a1r5g5b5";
	case PICT_x1r5g5b5:
		return "PICT_x1r5g5b5";
	case PICT_a1b5g5r5:
		return "PICT_a1b5g5r5";
	case PICT_x1b5g5r5:
		return "PICT_x1b5g5r5";
	case PICT_a4r4g4b4:
		return "PICT_a4r4g4b4";
	case PICT_x4r4g4b4:
		return "PICT_x4r4g4b4";
	case PICT_a4b4g4r4:
		return "PICT_a4b4g4r4";
	case PICT_x4b4g4r4:
		return "PICT_x4b4g4r4";

	/* 8bpp formats */
	case PICT_a8:
		return "PICT_a8";
	case PICT_r3g3b2:
		return "PICT_r3g3b2";
	case PICT_b2g3r3:
		return "PICT_b2g3r3";
	case PICT_a2r2g2b2:
		return "PICT_a2r2g2b2";
	case PICT_a2b2g2r2:
		return "PICT_a2b2g2r2";

	case PICT_c8:
		return "PICT_c8";
	case PICT_g8:
		return "PICT_g8";

	case PICT_x4a4:
		return "PICT_x4a4";

/*	case PICT_x4c4:
		return "PICT_x4c4";
	case PICT_x4g4:
		return "PICT_x4g4";
*/
	/* 4bpp formats */
	case PICT_a4:
		return "PICT_a4";
	case PICT_r1g2b1:
		return "PICT_r1g2b1";
	case PICT_b1g2r1:
		return "PICT_b1g2r1";
	case PICT_a1r1g1b1:
		return "PICT_a1r1g1b1";
	case PICT_a1b1g1r1:
		return "PICT_a1b1g1r1";

	case PICT_c4:
		return "PICT_c4";
	case PICT_g4:
		return "PICT_g4";

	/* 1bpp formats */
	case PICT_a1:
		return "PICT_a1";
	case PICT_g1:
		return "PICT_g1";
	}

	return "unknown";
}

static inline const char*
imxexa_string_from_pict_op(
	int op)
{
	switch (op) {
	case PictOpClear:
		return "PictOpClear";
	case PictOpSrc:
		return "PictOpSrc";
	case PictOpDst:
		return "PictOpDst";
	case PictOpOver:
		return "PictOpOver";
	case PictOpOverReverse:
		return "PictOpOverReverse";
	case PictOpIn:
		return "PictOpIn";
	case PictOpInReverse:
		return "PictOpInReverse";
	case PictOpOut:
		return "PictOpOut";
	case PictOpOutReverse:
		return "PictOpOutReverse";
	case PictOpAtop:
		return "PictOpAtop";
	case PictOpAtopReverse:
		return "PictOpAtopReverse";
	case PictOpXor:
		return "PictOpXor";
	case PictOpAdd:
		return "PictOpAdd";
	case PictOpSaturate:
		return "PictOpSaturate";
	}

	return "unknown";
}

const char*
imxexa_string_from_c2d_format(
	C2D_COLORFORMAT format)
{
	switch (format) {
	/* 1bpp formats */
	case C2D_COLOR_A1:
		return "C2D_COLOR_A1";

	/* 4bpp formats */
	case C2D_COLOR_A4:
		return "C2D_COLOR_A4";

	/* 8bpp formats */
	case C2D_COLOR_A8:
		return "C2D_COLOR_A8";
	case C2D_COLOR_8:
		return "C2D_COLOR_8";

	/* 16bpp formats */
	case C2D_COLOR_4444:
		return "C2D_COLOR_4444";
	case C2D_COLOR_4444_RGBA:
		return "C2D_COLOR_4444_RGBA";
	case C2D_COLOR_1555:
		return "C2D_COLOR_1555";
	case C2D_COLOR_5551_RGBA:
		return "C2D_COLOR_5551_RGBA";
	case C2D_COLOR_0565:
		return "C2D_COLOR_0565";

	/* 32bpp formats */
	case C2D_COLOR_8888:
		return "C2D_COLOR_8888";
	case C2D_COLOR_8888_RGBA:
		return "C2D_COLOR_8888_RGBA";
	case C2D_COLOR_8888_ABGR:
		return "C2D_COLOR_8888_ABGR";

	/* 24bpp formats */
	case C2D_COLOR_888:
		return "C2D_COLOR_888";

	/* 16bpp formats */
	case C2D_COLOR_YVYU:
		return "C2D_COLOR_YVYU";
	case C2D_COLOR_UYVY:
		return "C2D_COLOR_UYVY";
	case C2D_COLOR_YUY2:
		return "C2D_COLOR_YUY2";

	/* Pacify the compiler. */
	default:
		break;
	}

	return "unknown";
}

static inline const char*
imxexa_string_from_priv_pixmap(
	IMXEXAPixmapPtr fPixmapPtr)
{
	static char buf[1024];

	snprintf(buf, sizeof(buf) / sizeof(buf[0]), "%dx%dx%d (%dbpp) %s (priv rec %p)",
		fPixmapPtr->width,
		fPixmapPtr->height,
		fPixmapPtr->depth,
		fPixmapPtr->bitsPerPixel,
		imxexa_string_from_c2d_format(fPixmapPtr->surfDef.format),
		fPixmapPtr);

	return buf;
}

static inline const char*
imxexa_string_from_c2d_rect(
	const C2D_RECT* rect)
{
	static char buf[1024];

	snprintf(buf, sizeof(buf) / sizeof(buf[0]), "x:%d y:%d w:%d h:%d",
		rect->x,
		rect->y,
		rect->width,
		rect->height);

	return buf;
}

static inline C2D_SURFACE
imxexa_get_preferred_surface(
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL != fPixmapPtr->alias)
		return fPixmapPtr->alias;

	return fPixmapPtr->surf;
}

static inline int
imxexa_calc_system_memory_pitch(
	int width,
	int bitsPerPixel)
{
	return ((width * bitsPerPixel + FB_MASK) >> FB_SHIFT) * sizeof(FbBits);
}

static inline void
imxexa_register_pixmap_with_driver(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return;

	/* Put new pixmap at the head of pixmap list. */
	/* If new pixmap is alone - put it as primary eviction candidate. */
	fPixmapPtr->prev = NULL;
	fPixmapPtr->next = fPtr->pFirstPix;

	if (NULL != fPtr->pFirstPix)
		fPtr->pFirstPix->prev = fPixmapPtr;
	else
		fPtr->pFirstEvictionCandidate = fPixmapPtr;

	fPtr->pFirstPix = fPixmapPtr;
}

static inline void
imxexa_unregister_pixmap_from_driver(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return;

	/* Unlink pixmap from siblings. If pixmap was last in list and thus */
	/* primary eviction candidate - update eviction candidates head. */
	if (NULL != fPixmapPtr->prev)
		fPixmapPtr->prev->next = fPixmapPtr->next;
	else
		fPtr->pFirstPix = fPixmapPtr->next;

	if (NULL != fPixmapPtr->next)
		fPixmapPtr->next->prev = fPixmapPtr->prev;
	else
		fPtr->pFirstEvictionCandidate = fPixmapPtr->prev;
}

static inline unsigned
imxexa_calc_c2d_allocated_mem(
	IMXEXAPtr fPtr)
{
	unsigned res = 0;

	IMXEXAPixmapPtr p;
	for (p = fPtr->pFirstPix; p != NULL; p = p->next) {

		if (NULL != p->surf && 0 == (C2D_SURFACE_NO_BUFFER_ALLOC & p->surfDef.flags)) {
			res += p->surfDef.height * p->surfDef.stride;
		}
	}

	return res;
}

static inline const char*
imxexa_string_from_c2d_surface(
	IMXEXAPixmapPtr fPixmapPtr)
{
	static char buf[1024];

	snprintf(buf, sizeof(buf) / sizeof(buf[0]), "%s %ux%u %u %p %p",
		imxexa_string_from_c2d_format(fPixmapPtr->surfDef.format),
		fPixmapPtr->surfDef.width,
		fPixmapPtr->surfDef.height,
		fPixmapPtr->surfDef.stride,
		fPixmapPtr->surfDef.buffer,
		fPixmapPtr->surfDef.host);

	return buf;
}

static void
imxexa_list_c2d_surfaces(
	IMXEXAPtr fPtr)
{
	xf86DrvMsg(0, X_INFO,
		"current surfaces:\n");

	unsigned surf_count = 0;
	unsigned pixmap_count = 0;

	IMXEXAPixmapPtr p;
	for (p = fPtr->pFirstPix; p != NULL; p = p->next, ++pixmap_count) {

		if (NULL != p->surf && 0 == (C2D_SURFACE_NO_BUFFER_ALLOC & p->surfDef.flags)) {

			xf86DrvMsg(0, X_INFO,
				"%u %p 0x%.16llx: %s\n",
				surf_count,
				p,
				p->stamp,
				imxexa_string_from_c2d_surface(p));

			++surf_count;
		}
	}

	xf86DrvMsg(0, X_INFO,
		"counted %u surfaces among %u pixmaps\n",
		surf_count, pixmap_count);
}

static void
imxexa_setup_context_defaults(
	C2D_CONTEXT ctx)
{
	c2dSetDstSurface(ctx, 0);
	c2dSetSrcSurface(ctx, 0);
	c2dSetBrushSurface(ctx, 0, 0);
	c2dSetMaskSurface(ctx, 0, 0);

	c2dSetDstRectangle(ctx, 0);
	c2dSetSrcRectangle(ctx, 0);
	c2dSetDstClipRect(ctx, 0);

	c2dSetBlendMode(ctx, C2D_ALPHA_BLEND_NONE);
	c2dSetGlobalAlpha(ctx, 0xff);

	c2dSetSrcColorkey(ctx, 0, 0);
	c2dSetDstColorkey(ctx, 0, 0);

	c2dSetSrcRotate(ctx, 0);
	c2dSetDstRotate(ctx, 0);

	c2dSetRop(ctx, 0xcccc);

	c2dSetFgColor(ctx, 0);
	c2dSetBgColor(ctx, 0);

	c2dSetGradientDirection(ctx, C2D_GD_LEFT_RIGHT);
	c2dSetStretchMode(ctx, C2D_STRETCH_POINT_SAMPLING); /* c2d_z160: this seems to be the _general_ sampling, not just at stretching. */
}

static inline PixmapPtr
imxexa_get_pixmap_from_drawable(DrawablePtr pDrawable)
{
	if (NULL == pDrawable)
		return NULL;

	/* Check for a backing pixmap. */
	if (DRAWABLE_WINDOW == pDrawable->type) {

		WindowPtr pWindow = (WindowPtr)pDrawable;
		return pDrawable->pScreen->GetWindowPixmap(pWindow);
	}

	/* It's a regular pixmap. */
	return (PixmapPtr) pDrawable;
}

static inline PixmapPtr
imxexa_get_pixmap_from_picture(PicturePtr pPicture)
{
	if (NULL != pPicture)
		return imxexa_get_pixmap_from_drawable(pPicture->pDrawable);

	return NULL;
}

static inline Bool
imxexa_can_accelerate_pixmap(
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return FALSE;

	/* Evicted pixmaps are reinstated on first use, report them as good for acceleration. */
	if (NULL == fPixmapPtr->surf && PIXMAP_STAMP_EVICTED != fPixmapPtr->stamp)
		return FALSE;

	return TRUE;
}

static Bool
IMXEXAPixmapIsOffscreen(
	PixmapPtr pPixmap)
{
	if (NULL == pPixmap)
		return FALSE;

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmap);

	return imxexa_can_accelerate_pixmap(fPixmapPtr);
}

static void
imxexa_gpu_context_release(
	ScrnInfoPtr pScrn)
{
	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	if (NULL == fPtr->gpuContext)
		return;

	C2D_STATUS r;

	/* Rendezvous with the GPU. */
	r = c2dFinish(fPtr->gpuContext);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Unable to sync GPU (code: 0x%08x)\n", r);
	}

	/* Dispose of screen's secondary surface. */
	if (NULL != fPtr->doubleSurf) {

		r = c2dSurfFree(fPtr->gpuContext, fPtr->doubleSurf);
		fPtr->doubleSurf = NULL;

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Unable to free screen's secondary surface (code: 0x%08x)\n", r);
		}
	}

	/* Dispose of screen's primary surface. */
	if (NULL != fPtr->screenSurf) {

		r = c2dSurfFree(fPtr->gpuContext, fPtr->screenSurf);
		fPtr->screenSurf = NULL;

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Unable to free screen's primary surface (code: 0x%08x)\n", r);
		}
	}

#if IMX_EXA_DEBUG_PIXMAPS

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"gpumem watermark: %u\n", fPtr->gpumem_watermark);

#endif /* IMX_EXA_DEBUG_PIXMAPS */

	/* We are done with this context. */
	r = c2dDestroyContext(fPtr->gpuContext);
	fPtr->gpuContext = NULL;

	if (C2D_STATUS_OK == r) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"Destroyed GPU context\n");
	}
	else {

		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			"Unable to destroy GPU context (code: 0x%08x)\n", r);
	}
}

static Bool
imxexa_gpu_context_acquire(
	ScrnInfoPtr pScrn)
{
	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	if (IMXEXA_BACKEND_NONE == imxPtr->backend)
		return FALSE;

	if (NULL != fPtr->gpuContext)
		return TRUE;

	C2D_COLORFORMAT format;

	/* Is screen's bitsPerPixel supported in surfaces? */
	if (!imxexa_surf_format_from_bpp(imxPtr->backend, pScrn->bitsPerPixel, &format)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Screen bitsPerPixel not supported by GPU\n");
		return FALSE;
	}

	/* Create GPU context. */
	C2D_STATUS r = c2dCreateContext(&fPtr->gpuContext);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Unable to acquire GPU context (code: 0x%08x)\n", r);
		return FALSE;
	}

	/* Create screen surface(s). */
	const int fd = fbdevHWGetFD(pScrn);

	struct fb_fix_screeninfo fixinfo;
	struct fb_var_screeninfo varinfo;

	ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo);
	ioctl(fd, FBIOGET_VSCREENINFO, &varinfo);

	Bool adjust_virtual_fb = FALSE;

	/* For Z430 backend make sure screen stride is multiple of 32 pixels. */
	if (IMXEXA_BACKEND_Z430 == imxPtr->backend &&
		varinfo.xres_virtual & 0x1f) {

		varinfo.xres_virtual = (varinfo.xres_virtual + 0x1f) & ~0x1f;
		varinfo.xoffset = 0;

		adjust_virtual_fb = TRUE;
	}

	/* Arrange double buffering for fullscreen clients. */
	/* Currently used only by XV adaptor which works only with Z160 backend. */
	if (IMXEXA_BACKEND_Z160 == imxPtr->backend && imxPtr->use_double_buffering &&
		varinfo.yres_virtual < varinfo.yres * 2) {

		varinfo.yres_virtual = varinfo.yres * 2;
		varinfo.yoffset = 0;

		adjust_virtual_fb = TRUE;
	}

	if (adjust_virtual_fb &&
		-1 == ioctl(fd, FBIOPUT_VSCREENINFO, &varinfo)) {

		c2dDestroyContext(fPtr->gpuContext);
		fPtr->gpuContext = NULL;

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Attempt to adjust virtual framebuffer failed at put_vscreeninfo ioctl (errno: %s)\n",
			strerror(errno));

		return FALSE;
	}

	/* At the early stages of driver init our ScrnInfoPtr does not have a valid ScreenPtr. */
	fPtr->screenSurfDef.format = format;
	fPtr->screenSurfDef.width  = varinfo.xres;
	fPtr->screenSurfDef.height = varinfo.yres;
	fPtr->screenSurfDef.stride = varinfo.xres_virtual * varinfo.bits_per_pixel / 8;
	fPtr->screenSurfDef.buffer = (void *) fixinfo.smem_start;
	fPtr->screenSurfDef.host   = fbdevHWMapVidmem(pScrn);
	fPtr->screenSurfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

	r = c2dSurfAlloc(fPtr->gpuContext, &fPtr->screenSurf, &fPtr->screenSurfDef);

	if (C2D_STATUS_OK != r) {

		c2dDestroyContext(fPtr->gpuContext);
		fPtr->gpuContext = NULL;

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Unable to allocate screen's primary surface (code: 0x%08x)\n", r);

		return FALSE;
	}

	if (IMXEXA_BACKEND_Z160 == imxPtr->backend && imxPtr->use_double_buffering) {

		C2D_SURFACE_DEF surfDef;
		memcpy(&surfDef, &fPtr->screenSurfDef, sizeof(surfDef));

		surfDef.buffer = (uint8_t *) fixinfo.smem_start + varinfo.yres * surfDef.stride;
		surfDef.host   = NULL; /* Surface is not intend to be ever locked. */
		surfDef.flags  = C2D_SURFACE_NO_BUFFER_ALLOC;

		r = c2dSurfAlloc(fPtr->gpuContext, &fPtr->doubleSurf, &surfDef);

		if (C2D_STATUS_OK != r) {

			c2dDestroyContext(fPtr->gpuContext);
			fPtr->gpuContext = NULL;

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Unable to allocate screen's secondary surface (code: 0x%08x)\n", r);

			return FALSE;
		}
	}

	/* GPU context created, set it up to defaults. */
	imxexa_setup_context_defaults(fPtr->gpuContext);

	fPtr->gpuSynced = FALSE;

	return TRUE;
}

static inline const char*
imxexa_string_from_pixmap_usage(
	int usage)
{
	switch (usage) {
	case 0:
		return "nil_usage";
	case CREATE_PIXMAP_USAGE_SCRATCH:
		return "CREATE_PIXMAP_USAGE_SCRATCH";
	case CREATE_PIXMAP_USAGE_BACKING_PIXMAP:
		return "CREATE_PIXMAP_USAGE_BACKING_PIXMAP";
	case CREATE_PIXMAP_USAGE_GLYPH_PICTURE:
		return "CREATE_PIXMAP_USAGE_GLYPH_PICTURE";
	}

	return "unknown";
}

static inline void
imxexa_update_surface_from_backup(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return;

	if (NULL == fPixmapPtr->surf)
		return;

	char* ptr_dst = fPixmapPtr->surfPtr;
	char* ptr_src = fPixmapPtr->sysPtr;

	if (NULL == fPixmapPtr->surfPtr) {

		/* Access-lock the surface. */
		const C2D_STATUS r = c2dSurfLock(fPtr->gpuContext,
			imxexa_get_preferred_surface(fPixmapPtr), (void**) &ptr_dst);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(0, X_ERROR,
				"imxexa_update_surface_from_backup failed to lock GPU surface (code: 0x%08x)\n", r);
			return;
		}

		fPixmapPtr->surfPtr = ptr_dst;
	}

	int pitch_dst = fPixmapPtr->surfDef.stride;
	int pitch_src = fPixmapPtr->sysPitchBytes;

	int line_bytes = fPixmapPtr->width * fPixmapPtr->bitsPerPixel / 8;
	int line_count = fPixmapPtr->height;

	while (0 != line_count--) {

		memcpy(ptr_dst, ptr_src, line_bytes);
		ptr_dst += pitch_dst;
		ptr_src += pitch_src;
	}
}

static Bool
imxexa_update_backup_from_surface(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return FALSE;

	if (NULL == fPixmapPtr->surf)
		return FALSE;

	/* Has surface ever been evicted? */
	if (NULL == fPixmapPtr->sysPtr) {

		/* Compute number of pitch bytes for sysmem pixmaps of the specified geometry. */
		const int sysPitchBytes =
			imxexa_calc_system_memory_pitch(fPixmapPtr->width, fPixmapPtr->bitsPerPixel);

		/* Allocate contiguous height * sysPitchBytes from heap. */
		void* const sysPtr = malloc(fPixmapPtr->height * sysPitchBytes);

		if (NULL != sysPtr) {

			fPixmapPtr->sysPtr = sysPtr;
			fPixmapPtr->sysPitchBytes = sysPitchBytes;
		}
		else {

			xf86DrvMsg(0, X_ERROR,
				"imxexa_update_backup_from_surface failed to allocate backing storage for GPU surface\n");
			return FALSE;
		}
	}

	char* ptr_dst = fPixmapPtr->sysPtr;
	char* ptr_src = fPixmapPtr->surfPtr;

	if (NULL == fPixmapPtr->surfPtr) {

		/* Access-lock the surface. */
		const C2D_STATUS r = c2dSurfLock(fPtr->gpuContext,
			imxexa_get_preferred_surface(fPixmapPtr), (void**) &ptr_src);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(0, X_ERROR,
				"imxexa_update_backup_from_surface failed to lock GPU surface (code: 0x%08x)\n", r);
			return FALSE;
		}

		fPixmapPtr->surfPtr = ptr_src;
	}

	int pitch_dst = fPixmapPtr->sysPitchBytes;
	int pitch_src = fPixmapPtr->surfDef.stride;

	int line_bytes = fPixmapPtr->width * fPixmapPtr->bitsPerPixel / 8;
	int line_count = fPixmapPtr->height;

	while (0 != line_count--) {

		memcpy(ptr_dst, ptr_src, line_bytes);
		ptr_dst += pitch_dst;
		ptr_src += pitch_src;
	}

	return TRUE;
}

static Bool
imxexa_evict_pixmap(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr);

C2D_STATUS
imxexa_alloc_c2d_surface(
	IMXEXAPtr fPtr,
	C2D_SURFACE_DEF* surfDef,
	C2D_SURFACE* surf)
{
	C2D_STATUS r = c2dSurfAlloc(fPtr->gpuContext, surf, surfDef);

	/* In case of failure due to running out of memory, start evicting from the top eviction candidate. */
	IMXEXAPixmapPtr p = fPtr->pFirstEvictionCandidate;

	for (; NULL != p && C2D_STATUS_OUT_OF_MEMORY == r; p = p->prev) {

		if (!imxexa_evict_pixmap(fPtr, p))
			continue;

		r = c2dSurfAlloc(fPtr->gpuContext, surf, surfDef);
	}

	return r;
}

static inline Bool
imxexa_unlock_surface(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return TRUE;

	/* Is surface currently evicted? Reinstate it first. */
	if (PIXMAP_STAMP_EVICTED == fPixmapPtr->stamp) {

		const C2D_COLORFORMAT format = fPixmapPtr->surfDef.format;
		memset(&fPixmapPtr->surfDef, 0, sizeof(fPixmapPtr->surfDef));

		fPixmapPtr->surfDef.format = format;
        fPixmapPtr->surfDef.width = fPixmapPtr->width;
        fPixmapPtr->surfDef.height = fPixmapPtr->height;

		const C2D_STATUS r = imxexa_alloc_c2d_surface(fPtr, &fPixmapPtr->surfDef, &fPixmapPtr->surf);

		if (C2D_STATUS_OK == r) {

			imxexa_update_surface_from_backup(fPtr, fPixmapPtr);

			fPixmapPtr->stamp = 0;
		}
		else {

			xf86DrvMsg(0, X_ERROR,
				"imxexa_unlock_surface failed to reinstate surface (code: 0x%08x), c2d mem utilization %u\n",
				r, imxexa_calc_c2d_allocated_mem(fPtr));

			return FALSE;
		}

#if IMX_EXA_DEBUG_EVICTION

		xf86DrvMsg(0, X_INFO,
			"imxexa_reinstate_pixmap %s\n",
			imxexa_string_from_priv_pixmap(fPixmapPtr));
#endif

	}

	/* Is surface not locked? */
	if (NULL == fPixmapPtr->surfPtr)
		return TRUE;

	c2dSurfUnlock(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapPtr));

	fPixmapPtr->surfPtr = NULL;

	return TRUE;
}

static inline Bool
imxexa_evict_pixmap(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return FALSE;

	if (PIXMAP_STAMP_PINNED == fPixmapPtr->stamp)
		return FALSE;

	/* Is pixmap not in gpumem, or was it not allocated via the standard allocator? */
	if (NULL == fPixmapPtr->surf || 0 != (C2D_SURFACE_NO_BUFFER_ALLOC & fPixmapPtr->surfDef.flags))
		return FALSE;

	/* Is pixmap participating in an ongoing op? */
	if (fPtr->pPixDst == fPixmapPtr ||
		fPtr->pPixSrc == fPixmapPtr ||
		fPtr->pPixMsk == fPixmapPtr) {

		return FALSE;
	}

	if (!imxexa_update_backup_from_surface(fPtr, fPixmapPtr))
		return FALSE;

	imxexa_unlock_surface(fPtr, fPixmapPtr);

	/* Is pixmap using an alias? */
	if (NULL != fPixmapPtr->alias && fPixmapPtr->surf != fPixmapPtr->alias) {

		const C2D_STATUS r = c2dSurfFree(fPtr->gpuContext, fPixmapPtr->alias);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(0, X_ERROR,
				"imxexa_evict_pixmap failed to free pixmap alias (priv rec %p) (code: 0x%08x)\n",
				fPixmapPtr, r);

			return FALSE;
		}
	}

	const C2D_STATUS r = c2dSurfFree(fPtr->gpuContext, fPixmapPtr->surf);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(0, X_ERROR,
			"imxexa_evict_pixmap failed to free surface (priv rec %p) (code: 0x%08x)\n",
			fPixmapPtr, r);

		return FALSE;
	}

	fPixmapPtr->alias = NULL;
	fPixmapPtr->surf = NULL;
	fPixmapPtr->stamp = PIXMAP_STAMP_EVICTED;

#if IMX_EXA_DEBUG_EVICTION

	xf86DrvMsg(0, X_INFO,
		"imxexa_evict_pixmap %s\n",
		imxexa_string_from_priv_pixmap(fPixmapPtr));
#endif

	return TRUE;
}

static inline void
imxexa_update_pixmap_on_failure(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return;

	/* Update pixmap's failure count. */
	++fPixmapPtr->n_failures;

	/* Is pixmap excepted from demotion? */
	if (PIXMAP_STAMP_EVICTED == fPixmapPtr->stamp ||
		PIXMAP_STAMP_PINNED == fPixmapPtr->stamp) {

		return;
	}

#if 0
	/* Has pixmap passed a threshold number of failures, and is its */
	/* ratio of successful uses to failures below another threshold? */
	if (4 < fPixmapPtr->n_failures &&
		2 * fPixmapPtr->n_failures >= 5 * fPixmapPtr->n_uses) {

#if IMX_EXA_DEBUG_DEMOTION

		xf86DrvMsg(0, X_INFO,
			"imxexa_update_pixmap_on_failure demoted offscreen pixmap after %u uses, %u failures\n",
			fPixmapPtr->n_uses, fPixmapPtr->n_failures);
#endif

		imxexa_evict_pixmap(fPtr, fPixmapPtr);
	}
#endif
}

static inline void
imxexa_update_pixmap_on_use(
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	if (NULL == fPixmapPtr)
		return;

	/* No need to do anything if pixmap is pinned. */
	if (PIXMAP_STAMP_PINNED	== fPixmapPtr->stamp)
		return;

	/* Make pixmap last candidate for eviction. Re-registering the */
	/* pixmap sends it to the tail of the eviction-candidates list. */
	imxexa_unregister_pixmap_from_driver(fPtr, fPixmapPtr);
	imxexa_register_pixmap_with_driver(fPtr, fPixmapPtr);

	/* Restamp pixmap. */
	fPixmapPtr->stamp = fPtr->heartbeat;

	/* Update pixmap's use count. */
	++fPixmapPtr->n_uses;
}

void
IMX_EXA_GetRec(ScrnInfoPtr pScrn)
{
	IMXPtr imxPtr = IMXPTR(pScrn);

	if (NULL != imxPtr->exaDriverPrivate)
		return;

	imxPtr->exaDriverPrivate = xnfcalloc(1, sizeof(IMXEXARec));
}

void
IMX_EXA_FreeRec(ScrnInfoPtr pScrn)
{
	IMXPtr imxPtr = IMXPTR(pScrn);

	if (NULL == imxPtr->exaDriverPrivate)
		return;

	free(imxPtr->exaDriverPrivate);
	imxPtr->exaDriverPrivate = NULL;
}

Bool
IMX_EXA_GetPixmapProperties(
	PixmapPtr pPixmap,
	void** pPhysAddr,
	int* pPitch)
{
	/* Initialize values to be returned. */
	*pPhysAddr = NULL;
	*pPitch = 0;

	if (NULL == pPixmap)
		return FALSE;

	/* Access screen info associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmap);

	/* Reinstate and/or unlock pixmap as needed. */
	if (!imxexa_unlock_surface(fPtr, fPixmapPtr))
		return FALSE;

	/* Get the physical address of pixmap and its pitch. */
	*pPhysAddr = fPixmapPtr->surfDef.buffer;
	*pPitch = fPixmapPtr->surfDef.stride;

	/* Pin pixmap for the rest of its life, so that data we just submitted stays valid. */
	fPixmapPtr->stamp = PIXMAP_STAMP_PINNED;

	return TRUE;
}

static void*
IMXEXACreatePixmap2(
	ScreenPtr pScreen,
	int width, int height,
	int depth, int usage_hint, int bitsPerPixel,
	int *pPitch)
{
	/* Access screen info associated with this screen. */
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Allocate the private data structure to be stored with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) xnfcalloc(1, sizeof(IMXEXAPixmapRec));

	/* Initialize pixmap properties passed in. */
	fPixmapPtr->width = width;
	fPixmapPtr->height = height;
	fPixmapPtr->depth = depth;
	fPixmapPtr->bitsPerPixel = bitsPerPixel;

	imxexa_register_pixmap_with_driver(fPtr, fPixmapPtr);
	*pPitch = 0;

#if IMX_EXA_DEBUG_TRACE

	if (IMX_EXA_TRACE_MAGIC_WIDTH == width &&
		IMX_EXA_TRACE_MAGIC_HEIGHT == height) {

		++trace;

		if (is_tracing())
			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "tracing on\n");
		else
			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "tracing off\n");
	}

#endif

#if IMX_EXA_DEBUG_PIXMAPS

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXACreatePixmap2 called with: %dx%dx%d %dbpp %s (priv rec %p)\n",
		width, height, depth, bitsPerPixel, imxexa_string_from_pixmap_usage(usage_hint), fPixmapPtr);

#endif

	/* Nothing more to do if width, height or bitsPerPixel is nil. */
	if (0 >= width || 0 >= height || 0 >= bitsPerPixel)
		return fPixmapPtr;

	/* Attempt to allocate from gpumem if surface geometry and bitsPerPixel are eligible. */
	if (NULL != fPtr->gpuContext &&
		IMX_EXA_MAX_SURF_DIM >= width && IMX_EXA_MAX_SURF_DIM >= height &&
		IMX_EXA_MIN_SURF_AREA <= (width * height) && IMX_EXA_MIN_SURF_HEIGHT <= height &&
		imxexa_surf_format_from_bpp(imxPtr->backend, bitsPerPixel, &fPixmapPtr->surfDef.format)) {

		fPixmapPtr->surfDef.width = width;
		fPixmapPtr->surfDef.height = height;

		const C2D_STATUS r = imxexa_alloc_c2d_surface(fPtr, &fPixmapPtr->surfDef, &fPixmapPtr->surf);

		if (C2D_STATUS_OK == r) {

			*pPitch = fPixmapPtr->surfDef.stride;

#if IMX_EXA_DEBUG_PIXMAPS

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"IMXEXACreatePixmap2 allocated offscreen pixmap of stride %u - %s\n",
				fPixmapPtr->surfDef.stride, (fPixmapPtr->surfDef.stride % (32 / 8 * bitsPerPixel) ? "wrong" : "ok"));

			const uint32_t gpumem = imxexa_calc_c2d_allocated_mem(fPtr);

			if (gpumem > fPtr->gpumem_watermark)
				fPtr->gpumem_watermark = gpumem;
#endif
		}
		else {

			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				"IMXEXACreatePixmap2 failed to allocate GPU surface (code: 0x%08x), c2d mem utilization %u\n",
				r, imxexa_calc_c2d_allocated_mem(fPtr));
		}
	}

	if (NULL == fPixmapPtr->surf) {

		/* Allocate heap storage for sysmem pixmap. */

		/* Compute number of pitch bytes for sysmem pixmaps of the specified geometry. */
		const int sysPitchBytes =
			imxexa_calc_system_memory_pitch(width, bitsPerPixel);

		/* Allocate contiguous height * sysPitchBytes from heap. */
		void* const sysPtr = malloc(height * sysPitchBytes);

		if (NULL != sysPtr) {

			fPixmapPtr->sysPtr = sysPtr;
			fPixmapPtr->sysPitchBytes = sysPitchBytes;

			*pPitch = sysPitchBytes;

#if IMX_EXA_DEBUG_PIXMAPS

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"IMXEXACreatePixmap2 allocated system-memory pixmap of stride %d\n",
				sysPitchBytes);
#endif
		}
		else {

			/* Unregister and free the driver private data associated with pixmap. */
			imxexa_unregister_pixmap_from_driver(fPtr, fPixmapPtr);
			free(fPixmapPtr);

			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				"IMXEXACreatePixmap2 failed to allocate system memory; no pixmap allocated\n");

			return NULL;
		}
	}

	return fPixmapPtr;
}

static void
IMXEXADestroyPixmap(
	ScreenPtr pScreen,
	void *driverPriv)
{
	if (NULL == pScreen)
		return;

	if (NULL == driverPriv)
		return;

	IMXEXAPixmapPtr fPixmapPtr = (IMXEXAPixmapPtr) driverPriv;

	/* Access screen info associated with this screen. */
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Conclude any pending lazy unlocks on this pixmap, but don't reinstate it if evicted. */
	if (NULL != fPixmapPtr->surfPtr)
		imxexa_unlock_surface(fPtr, fPixmapPtr);

	/* Is pixmap allocated from offscreen memory? */
	if (NULL != fPixmapPtr->surf && fPtr->screenSurf != fPixmapPtr->surf) {

		/* Is pixmap using an alias? */
		if (NULL != fPixmapPtr->alias && fPixmapPtr->surf != fPixmapPtr->alias) {

			const C2D_STATUS r = c2dSurfFree(fPtr->gpuContext, fPixmapPtr->alias);

			if (C2D_STATUS_OK == r) {

#if IMX_EXA_DEBUG_PIXMAPS

				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
					"IMXEXADestroyPixmap freed offscreen pixmap alias (priv rec %p)\n",
					fPixmapPtr);
#endif
			}
			else {

				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"IMXEXADestroyPixmap failed to free offscreen pixmap alias (priv rec %p) (code: 0x%08x)\n",
					fPixmapPtr, r);
			}
		}

		const C2D_STATUS r = c2dSurfFree(fPtr->gpuContext, fPixmapPtr->surf);

		if (C2D_STATUS_OK == r) {

#if IMX_EXA_DEBUG_PIXMAPS

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"IMXEXADestroyPixmap freed offscreen pixmap %dx%dx%d %dbpp (priv rec %p)\n",
				fPixmapPtr->width,
				fPixmapPtr->height,
				fPixmapPtr->depth,
				fPixmapPtr->bitsPerPixel,
				fPixmapPtr);
#endif
		}
		else {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXADestroyPixmap failed to free offscreen pixmap (priv rec %p) (code: 0x%08x)\n",
				fPixmapPtr, r);
		}
	}

	/* Is pixmap allocated in system memory or does pixmap have backing storage? */
	if (NULL != fPixmapPtr->sysPtr) {

		if (imxPtr->fbstart != fPixmapPtr->sysPtr)
			free(fPixmapPtr->sysPtr);

#if IMX_EXA_DEBUG_PIXMAPS

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXEXADestroyPixmap freed system-memory pixmap %dx%dx%d %dbpp (priv rec %p)\n",
			fPixmapPtr->width,
			fPixmapPtr->height,
			fPixmapPtr->depth,
			fPixmapPtr->bitsPerPixel,
			fPixmapPtr);
#endif

	}

	/* Unregister pixmap from driver and free the driver private data associated with pixmap. */
	imxexa_unregister_pixmap_from_driver(fPtr, fPixmapPtr);
	free(fPixmapPtr);
}

static Bool
IMXEXAModifyPixmapHeader(
	PixmapPtr pPixmap,
	int width,
	int height,
	int depth,
	int bitsPerPixel,
	int devKind,
	pointer pPixData)
{
	if (NULL == pPixmap)
		return FALSE;

	/* Access screen info associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmap);

#if IMX_EXA_DEBUG_PIXMAPS

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXAModifyPixmapHeader called with %dx%dx%d %dbpp stride %d pixdata %p (priv rec %p)\n",
		width, height, depth, bitsPerPixel, devKind, pPixData, fPixmapPtr);

#endif

	/* Refuse to patch pixmaps receiving a custom data ptr of unknown origin. */
	if (NULL != pPixData && pPixData != imxPtr->fbstart && pPixData != fPixmapPtr->sysPtr) {

#if IMX_EXA_DEBUG_PIXMAPS

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXEXAModifyPixmapHeader detected attempt to patch pixmap with foreign pixdata; bailing out\n");
#endif
		return FALSE;
	}

	/* Is this an attempt to patch the screen pixmap? */
	if (pPixData == imxPtr->fbstart) {

		if (NULL == fPtr->gpuContext) {

			fPixmapPtr->width = width;
			fPixmapPtr->height = height;

			fPixmapPtr->sysPitchBytes = devKind;
			fPixmapPtr->sysPtr = pPixData;
		}
		else /* Is pixmap not using the screen surface? */
		if (fPtr->screenSurf != fPixmapPtr->surf) {

			/* Does pixmap already have a genuine surface? */
			if (NULL != fPixmapPtr->surf) {

				/* Is pixmap using an alias? */
				if (NULL != fPixmapPtr->alias && fPixmapPtr->surf != fPixmapPtr->alias) {

					xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
						"IMXEXAModifyPixmapHeader encountered invalid screen pixmap with an alias\n");

					const C2D_STATUS r = c2dSurfFree(fPtr->gpuContext, fPixmapPtr->alias);

					if (C2D_STATUS_OK != r) {

						xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
							"IMXEXAModifyPixmapHeader failed to free invalid screen alias (code: 0x%08x)\n", r);
					}
				}
				else {

					xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
						"IMXEXAModifyPixmapHeader encountered invalid screen pixmap\n");
				}

				const C2D_STATUS r = c2dSurfFree(fPtr->gpuContext, fPixmapPtr->surf);

				if (C2D_STATUS_OK != r) {

					xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
						"IMXEXAModifyPixmapHeader failed to free invalid screen surface (code: 0x%08x)\n", r);
				}
			}

			/* Update the surface params of this pixmap from the screen surface. */
			fPixmapPtr->alias = NULL;
			fPixmapPtr->surf = fPtr->screenSurf;
			memcpy(&fPixmapPtr->surfDef, &fPtr->screenSurfDef, sizeof(fPixmapPtr->surfDef));

			fPixmapPtr->width = fPtr->screenSurfDef.width;
			fPixmapPtr->height = fPtr->screenSurfDef.height;

#if IMX_EXA_DEBUG_PIXMAPS

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"IMXEXAModifyPixmapHeader patched screen pixmap\n");
#endif
		}
	}

#if IMX_EXA_DEBUG_PIXMAPS

	/* Try to catch any violations of offscreen pixmap params. For diagnostics. */
	if (NULL != fPixmapPtr->surf) {

		if (0 < width && width != fPixmapPtr->width) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify width of offscreen pixmap (old: %d, new: %d)\n",
				fPixmapPtr->width, width);
		}

		if (0 < height && height != fPixmapPtr->height) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify height of offscreen pixmap (old: %d, new: %d)\n",
				fPixmapPtr->height, height);
		}

		if (0 < depth && depth != fPixmapPtr->depth) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify depth of offscreen pixmap (old: %d, new: %d)\n",
				fPixmapPtr->depth, depth);
		}

		if (0 < bitsPerPixel && bitsPerPixel != fPixmapPtr->bitsPerPixel) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify bitsPerPixel of offscreen pixmap (old: %d, new: %d)\n",
				fPixmapPtr->bitsPerPixel, bitsPerPixel);
		}

		if (0 < devKind && devKind != fPixmapPtr->surfDef.stride) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify stride of offscreen pixmap (old: %d, new: %d)\n",
				fPixmapPtr->surfDef.stride, devKind);
		}

		if (NULL != pPixData && pPixData != fPixmapPtr->surfDef.host) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify virtual address of offscreen pixmap (old: %p, new: %p)\n",
				fPixmapPtr->surfDef.host, pPixData);
		}
	}
	/* Pixmap is not offscreen. */
	else {

		if (0 < width && width != fPixmapPtr->width) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify width of driver-allocated system-memory pixmap\n");
		}

		if (0 < height && height != fPixmapPtr->height) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify height of driver-allocated system-memory pixmap\n");
		}

		if (0 < depth && depth != fPixmapPtr->depth) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify depth of driver-allocated system-memory pixmap\n");
		}

		if (0 < bitsPerPixel && bitsPerPixel != fPixmapPtr->bitsPerPixel) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify bitsPerPixel of driver-allocated system-memory pixmap\n");
		}

		if (0 < devKind && devKind != fPixmapPtr->sysPitchBytes) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify stride of driver-allocated system-memory pixmap\n");
		}

		if (NULL != pPixData && pPixData != fPixmapPtr->sysPtr) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAModifyPixmapHeader attempted to modify address of driver-allocated system-memory pixmap\n");
		}
	}

#endif /* IMX_EXA_DEBUG_PIXMAPS */

	/* Update the pixmap header with our info. */
	pPixmap->drawable.width			= fPixmapPtr->width;
	pPixmap->drawable.height		= fPixmapPtr->height;
	pPixmap->drawable.depth			= fPixmapPtr->depth;
	pPixmap->drawable.bitsPerPixel	= fPixmapPtr->bitsPerPixel;

	if (NULL != fPixmapPtr->surf) {

		pPixmap->devKind = fPixmapPtr->surfDef.stride;
		pPixmap->devPrivate.ptr = NULL;
	}
	else {

		pPixmap->devKind = fPixmapPtr->sysPitchBytes;
		pPixmap->devPrivate.ptr = fPixmapPtr->sysPtr;
	}

#if IMX_EXA_DEBUG_PIXMAPS

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXAModifyPixmapHeader returned pixmap: %dx%dx%d %dbpp\n",
		pPixmap->drawable.width,
		pPixmap->drawable.height,
		pPixmap->drawable.depth,
		pPixmap->drawable.bitsPerPixel);

#endif

	return TRUE;
}

static Bool
IMXEXAPrepareAccess(
	PixmapPtr pPixmap,
	int index)
{
	if (NULL == pPixmap)
		return FALSE;

	/* Access screen info associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmap);

	if (!imxexa_can_accelerate_pixmap(fPixmapPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareAccess called with sysmem pixmap\n");
		return FALSE;
	}

	/* Is surface currently evicted? */
	if (PIXMAP_STAMP_EVICTED == fPixmapPtr->stamp) {

		pPixmap->devKind = fPixmapPtr->sysPitchBytes;
		pPixmap->devPrivate.ptr = fPixmapPtr->sysPtr;

		return TRUE;
	}

	/* Is surface already locked? */
	if (NULL != fPixmapPtr->surfPtr) {

		pPixmap->devKind = fPixmapPtr->surfDef.stride;
		pPixmap->devPrivate.ptr = fPixmapPtr->surfPtr;

		return TRUE;
	}

	void* bits;

	/* Access-lock the surface. */
	const C2D_STATUS r = c2dSurfLock(fPtr->gpuContext,
		imxexa_get_preferred_surface(fPixmapPtr), &bits);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareAccess failed to lock GPU surface (code: 0x%08x)\n", r);
		return FALSE;
	}

	pPixmap->devKind = fPixmapPtr->surfDef.stride;
	pPixmap->devPrivate.ptr = bits;

	fPixmapPtr->surfPtr = bits;

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++fPtr->numAccessBeforeSync;

#endif

	return TRUE;
}

static void
IMXEXAFinishAccess(
	PixmapPtr pPixmap,
	int index)
{
	if (NULL == pPixmap)
		return;

	/* Access screen info associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

#if 0
	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);
#endif

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmap);

	if (!imxexa_can_accelerate_pixmap(fPixmapPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAFinishAccess called with sysmem pixmap\n");
		return;
	}

	/* Is the surface neither locked nor evicted? */
	if (NULL == fPixmapPtr->surfPtr && PIXMAP_STAMP_EVICTED != fPixmapPtr->stamp) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAFinishAccess called with pixmap not being accessed\n");
		return;
	}

	/* To relieve the GPU pipeline of EXA's enormous access pressure, surface will be unlocked upon */
	/* first use (lazy unlock). Just notify clients that access to the pixmap content is no more. */
	pPixmap->devPrivate.ptr = NULL;
}

static Bool
IMXEXAPrepareSolid(
	PixmapPtr pPixmap,
	int alu,
	Pixel planemask,
	Pixel fg)
{
	if (NULL == pPixmap)
		return FALSE;

	/* Access screen info associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmap);

	/* Make sure pixmap can be accelerated in principle. */
	if (!imxexa_can_accelerate_pixmap(fPixmapPtr))
		return FALSE;

	/* Do not accelerate 8bpp-or-narrower targets unless backend is Z160. */
	if (8 >= fPixmapPtr->bitsPerPixel && IMXEXA_BACKEND_Z160 != imxPtr->backend) {

		imxexa_update_pixmap_on_failure(fPtr, fPixmapPtr);
		return FALSE;
	}

	/* Make sure that the input planemask specifies a solid */
	if (!EXA_PM_IS_SOLID(&pPixmap->drawable, planemask)) {

#if IMX_EXA_DEBUG_PREPARE_SOLID

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareSolid called with planemask 0x%08x which is not solid\n",
			(unsigned)planemask);
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapPtr);
		return FALSE;
	}

	/* Make sure that the raster op is supported. */
	Bool rop_success = FALSE;

	switch (alu) {
	case GXclear:
		fg = 0;
		rop_success = TRUE;
		break;
	case GXcopy:
		rop_success = TRUE;
		break;
	case GXcopyInverted:
		fg = ~fg;
		rop_success = TRUE;
		break;
	case GXset:
		fg = -1U;
		rop_success = TRUE;
		break;
	}

	if (!rop_success) {

#if IMX_EXA_DEBUG_PREPARE_SOLID

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareSolid called with unsupported rop 0x%08x\n",
			(unsigned) alu);
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapPtr);
		return FALSE;
	}

	/* Remember the pixmap passed in. */
	fPtr->pPixDst = fPixmapPtr;
	fPtr->pPixSrc = NULL;
	fPtr->pPixMsk = NULL;

	/* Set up draw state. */
	if (!imxexa_unlock_surface(fPtr, fPixmapPtr))
		return FALSE;

	c2dSetDstSurface(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapPtr));
	c2dSetSrcSurface(fPtr->gpuContext, NULL);
	c2dSetBrushSurface(fPtr->gpuContext, NULL, NULL);
	c2dSetMaskSurface(fPtr->gpuContext, NULL, NULL);

	c2dSetFgColor(fPtr->gpuContext, fg);
	c2dSetBlendMode(fPtr->gpuContext, C2D_ALPHA_BLEND_NONE);

	/* Mark pixmap as used and update driver's heartbeat. */
	imxexa_update_pixmap_on_use(fPtr, fPixmapPtr);
	++fPtr->heartbeat;

	return TRUE;
}

static void
IMXEXASolid(
	PixmapPtr pPixmap,
	int x1, int y1,
	int x2, int y2)
{
	/* Access screen info associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	C2D_RECT rect = {
		.x = x1,
		.y = y1,
		.width = x2 - x1,
		.height = y2 - y1
	};

	c2dSetDstRectangle(fPtr->gpuContext, &rect);

	const C2D_STATUS r = c2dDrawRect(fPtr->gpuContext, C2D_PARAM_FILL_BIT);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXASolid failed to perform GPU draw (code: 0x%08x)\n", r);
	}

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++fPtr->numSolidBeforeSync;

#endif

#if IMX_EXA_DEBUG_SOLID

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXASolid called with rect (%d-%d, %d-%d)\n",
		x1, x2, y1, y2);

#elif IMX_EXA_DEBUG_TRACE

	if (is_tracing()) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXEXASolid called with rect (%d-%d, %d-%d)\n",
			x1, x2, y1, y2);
	}

#endif

}

static void
IMXEXADoneSolid(
	PixmapPtr pPixmap)
{
	/* Access screen info associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	if (NULL != fPtr->gpuContext) {

		/* Flush pending operations to the GPU. */
		c2dFlush(fPtr->gpuContext);

		fPtr->gpuSynced = FALSE;

		fPtr->pPixDst = NULL;
		fPtr->pPixSrc = NULL;
		fPtr->pPixMsk = NULL;
	}
}

static Bool
IMXEXAPrepareCopy(
	PixmapPtr pPixmapSrc,
	PixmapPtr pPixmapDst,
	int xdir,
	int ydir,
	int alu,
	Pixel planemask)
{
	if (NULL == pPixmapDst || NULL == pPixmapSrc)
		return FALSE;

	/* Access the screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmaps. */
	IMXEXAPixmapPtr fPixmapDstPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapDst);
	IMXEXAPixmapPtr fPixmapSrcPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapSrc);

	/* Make sure pixmaps can be accelerated in principle. */
	if (!imxexa_can_accelerate_pixmap(fPixmapDstPtr) ||
		!imxexa_can_accelerate_pixmap(fPixmapSrcPtr)) {

		return FALSE;
	}

	/* Do not accelerate 8bpp-or-narrower targets unless backend is Z160. */
	if (8 >= fPixmapDstPtr->bitsPerPixel && IMXEXA_BACKEND_Z160 != imxPtr->backend) {

		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		return FALSE;
	}

	/* Make sure that the input planemask specifies a solid */
	if (!EXA_PM_IS_SOLID(&pPixmapDst->drawable, planemask)) {

#if IMX_EXA_DEBUG_PREPARE_COPY

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareCopy called with planemask 0x%08x which is not solid\n",
			(unsigned)planemask);
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		return FALSE;
	}

	/* Make sure that the raster op is supported. */
	Bool rop_success = FALSE;

	switch (alu) {
	case GXcopy:
		rop_success = TRUE;
		break;
	}

	if (!rop_success) {

#if IMX_EXA_DEBUG_PREPARE_COPY

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareCopy called with unsupported rop 0x%08x\n",
			(unsigned)alu);
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		return FALSE;
	}

	/* Remember the pixmaps passed in. */
	fPtr->pPixDst = fPixmapDstPtr;
	fPtr->pPixSrc = fPixmapSrcPtr;
	fPtr->pPixMsk = NULL;

	/* Set up draw state. */
	if (!imxexa_unlock_surface(fPtr, fPixmapDstPtr) ||
		!imxexa_unlock_surface(fPtr, fPixmapSrcPtr)) {

		return FALSE;
	}

	c2dSetDstSurface(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapDstPtr));
	c2dSetSrcSurface(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapSrcPtr));
	c2dSetBrushSurface(fPtr->gpuContext, NULL, NULL);
	c2dSetMaskSurface(fPtr->gpuContext, NULL, NULL);

	c2dSetBlendMode(fPtr->gpuContext, C2D_ALPHA_BLEND_NONE);

	/* Mark pixmaps as used and update driver's heartbeat. */
	imxexa_update_pixmap_on_use(fPtr, fPixmapDstPtr);
	imxexa_update_pixmap_on_use(fPtr, fPixmapSrcPtr);
	++fPtr->heartbeat;

	return TRUE;
}

static void
IMXEXACopy(
	PixmapPtr pPixmapDst,
	int srcX, int srcY,
	int dstX, int dstY,
	int width, int height)
{
	/* Access screen info associated with dst pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	C2D_RECT rectDst = {
		.x = dstX,
		.y = dstY,
		.width = width,
		.height = height
	};

	C2D_RECT rectSrc = {
		.x = srcX,
		.y = srcY,
		.width = width,
		.height = height
	};

	c2dSetDstRectangle(fPtr->gpuContext, &rectDst);
	c2dSetSrcRectangle(fPtr->gpuContext, &rectSrc);

	const C2D_STATUS r = c2dDrawBlit(fPtr->gpuContext);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXACopy failed to perform GPU draw (code: 0x%08x)\n", r);
	}

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++fPtr->numCopyBeforeSync;

#endif

#if IMX_EXA_DEBUG_COPY

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXACopy called with src (%d-%d, %d-%d), dst (%d-%d, %d-%d)\n",
		srcX, srcX + width, srcY, srcY + height, dstX, dstX + width, dstY, dstY + height);

#elif IMX_EXA_DEBUG_TRACE

	if (is_tracing()) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXEXACopy called with src (%d-%d, %d-%d), dst (%d-%d, %d-%d)\n",
			srcX, srcX + width, srcY, srcY + height, dstX, dstX + width, dstY, dstY + height);
	}

#endif
}

static void
IMXEXADoneCopy(
	PixmapPtr pPixmapDst)
{
	/* Access screen info associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	if (NULL != fPtr->gpuContext) {

		/* Flush pending operations to the GPU. */
		c2dFlush(fPtr->gpuContext);

		fPtr->gpuSynced = FALSE;

		fPtr->pPixDst = NULL;
		fPtr->pPixSrc = NULL;
		fPtr->pPixMsk = NULL;
	}
}

static Bool
IMXEXAUploadToScreen(
	PixmapPtr pPixmapDst,
	int dstX,
	int dstY,
	int width,
	int height,
	char* pBufferSrc,
	int pitchSrc)
{
	if (NULL == pPixmapDst)
		return FALSE;

	/* Access screen info associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	if (0 >= width || 0 >= height || 0 > dstX || 0 > dstY) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAUploadToScreen called with malformed rectangle\n");
		return FALSE;
	}

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapDst);

	if (!imxexa_can_accelerate_pixmap(fPixmapPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAUploadToScreen called with sysmem pixmap\n");
		return FALSE;
	}

	/* By default set up copy parameters for a locked target surface. */
	int pitchDst = fPixmapPtr->surfDef.stride;
	char* pBufferDst = fPixmapPtr->surfPtr;

	/* Is surface currently evicted? */
	if (PIXMAP_STAMP_EVICTED == fPixmapPtr->stamp) {

		pitchDst = fPixmapPtr->sysPitchBytes;
		pBufferDst = fPixmapPtr->sysPtr;
	}
	else /* Is surface currently not locked? */
	if (NULL == fPixmapPtr->surfPtr) {

		/* Access-lock the surface. */
		const C2D_STATUS r = c2dSurfLock(fPtr->gpuContext,
			imxexa_get_preferred_surface(fPixmapPtr), (void**) &pBufferDst);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXAUploadToScreen failed to lock GPU surface (code: 0x%08x)\n", r);
			return FALSE;
		}

		fPixmapPtr->surfPtr = pBufferDst;
	}

	/* Compute number of bytes per pixel to transfer. */
	int bytesPerPixel = pPixmapDst->drawable.bitsPerPixel / 8;

	/* Advance to the starting pixel. */
	pBufferDst += dstY * pitchDst + dstX * bytesPerPixel;

	/* Compute how many bytes to copy per line of rectangle. */
	int lineCopyBytes = width * bytesPerPixel;

	while (0 != height--) {

		memcpy(pBufferDst, pBufferSrc, lineCopyBytes);
		pBufferDst += pitchDst;
		pBufferSrc += pitchSrc;
	}

	/* Don't unlock the surface here - leave it to the lazy unlock. */

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++fPtr->numUploadBeforeSync;

#endif

	return TRUE;
}

static Bool
IMXEXADownloadFromScreen(
	PixmapPtr pPixmapSrc,
	int srcX,
	int srcY,
	int width,
	int height,
	char* pBufferDst,
	int pitchDst)
{
	if (NULL == pPixmapSrc)
		return FALSE;

	/* Access screen info associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapSrc->drawable.pScreen->myNum];

	if (0 >= width || 0 >= height || 0 > srcX || 0 > srcY) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXADownloadFromScreen called with malformed rectangle\n");
		return FALSE;
	}

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapSrc);

	if (!imxexa_can_accelerate_pixmap(fPixmapPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXADownloadFromScreen called with sysmem pixmap\n");
		return FALSE;
	}

	/* By default set up copy parameters for a locked source surface. */
	int pitchSrc = fPixmapPtr->surfDef.stride;
	char* pBufferSrc = fPixmapPtr->surfPtr;

	/* Is surface currently evicted? */
	if (PIXMAP_STAMP_EVICTED == fPixmapPtr->stamp) {

		pitchSrc = fPixmapPtr->sysPitchBytes;
		pBufferSrc = fPixmapPtr->sysPtr;
	}
	else /* Is surface currently not locked? */
	if (NULL == fPixmapPtr->surfPtr) {

		/* Access-lock the surface. */
		const C2D_STATUS r = c2dSurfLock(fPtr->gpuContext,
			imxexa_get_preferred_surface(fPixmapPtr), (void**) &pBufferSrc);

		if (C2D_STATUS_OK != r) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"IMXEXADownloadFromScreen failed to lock GPU surface (code: 0x%08x)\n", r);
			return FALSE;
		}

		fPixmapPtr->surfPtr = pBufferSrc;
	}

	/* Compute number of bytes per pixel to transfer. */
	int bytesPerPixel = pPixmapSrc->drawable.bitsPerPixel / 8;

	/* Advance to the starting pixel. */
	pBufferSrc += srcY * pitchSrc + srcX * bytesPerPixel;

	/* Compute how many bytes to copy per line of rectangle. */
	int lineCopyBytes = width * bytesPerPixel;

	while (0 != height--) {

		memcpy(pBufferDst, pBufferSrc, lineCopyBytes);
		pBufferDst += pitchDst;
		pBufferSrc += pitchSrc;
	}

	/* Don't unlock the surface here - leave it to the lazy unlock. */

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++fPtr->numDnloadBeforeSync;

#endif

	return TRUE;
}

static void
IMXEXAWaitMarker(
	ScreenPtr pScreen,
	int marker)
{
	/* Access screen info associated with this screen. */
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Nothing to do if there has not been a GPU operation since last sync. */
	if (fPtr->gpuSynced)
		return;

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	/* Log how many calls were made to solid, copy, and composite before sync called. */
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXAWaitMarker called after solid=%lu copy=%lu conv=%lu composite=%lu access=%lu up=%lu down=%lu\n",
			fPtr->numSolidBeforeSync,
			fPtr->numCopyBeforeSync,
			fPtr->numConvBeforeSync,
			fPtr->numCompositeBeforeSync,
			fPtr->numAccessBeforeSync,
			fPtr->numUploadBeforeSync,
			fPtr->numDnloadBeforeSync);

	/* Reset counters */
	fPtr->numSolidBeforeSync = 0;
	fPtr->numCopyBeforeSync = 0;
	fPtr->numConvBeforeSync = 0;
	fPtr->numCompositeBeforeSync = 0;
	fPtr->numAccessBeforeSync = 0;
	fPtr->numUploadBeforeSync = 0;
	fPtr->numDnloadBeforeSync = 0;

#endif

	/* We use a surface-lock mechanism when doing CPU access to GPU surfaces, */
	/* so no need to sync here. Just update the sync status. By doing so we achieve */
	/* higher cpu-gpu concurrency. */
	fPtr->gpuSynced = TRUE;
}

static Bool
IMXEXACheckComposite(
	int op,
	PicturePtr pPictureSrc,
	PicturePtr pPictureMask,
	PicturePtr pPictureDst)
{
	if (NULL == pPictureDst || NULL == pPictureSrc)
		return FALSE;

	/* Access the pixmap associated with each picture. */
	PixmapPtr pPixmapDst = imxexa_get_pixmap_from_picture(pPictureDst);
	PixmapPtr pPixmapSrc = imxexa_get_pixmap_from_picture(pPictureSrc);
	PixmapPtr pPixmapMsk = imxexa_get_pixmap_from_picture(pPictureMask);

	if (NULL == pPixmapDst || NULL == pPixmapSrc)
		return FALSE;

	/* Access screen associated with dst pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmaps. */
	IMXEXAPixmapPtr fPixmapDstPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapDst);
	IMXEXAPixmapPtr fPixmapSrcPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapSrc);
	IMXEXAPixmapPtr fPixmapMskPtr = NULL != pPixmapMsk ?
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapMsk) : NULL;

	/* Make sure pixmaps can be accelerated in principle. */
	if (!imxexa_can_accelerate_pixmap(fPixmapDstPtr) ||
		!imxexa_can_accelerate_pixmap(fPixmapSrcPtr) ||
		NULL != fPixmapMskPtr && !imxexa_can_accelerate_pixmap(fPixmapMskPtr)) {

		return FALSE;
	}

	/* Cannot perform blend unless screens associated with src and dst pixmaps are the same. */
	if (pPixmapSrc->drawable.pScreen->myNum !=
		pPixmapDst->drawable.pScreen->myNum) {

		return FALSE;
	}

	/* Do not accelerate transformed sources. */
	if (NULL != pPictureSrc->transform) {

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXACheckComposite called with transformed source\n");
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapMskPtr);
		return FALSE;
	}

	/* Do not accelerate masks of component alpha (used for sub-pixel glyph anti-aliasing). */
	if (NULL != pPictureMask && pPictureMask->componentAlpha) {

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXACheckComposite called with mask of component alpha\n");
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapMskPtr);
		return FALSE;
	}

	/* Do not accelerate if mask format is not supported by backend. */
	if (NULL != pPictureMask &&
		pPictureMask->format != PICT_a8 &&
		pPictureMask->format != PICT_a8r8g8b8 &&
		pPictureMask->format != PICT_a8b8g8r8) {

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXACheckComposite called with mask of unsupported format (%s)\n",
			imxexa_string_from_pict_format(pPictureMask->format));
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapMskPtr);
		return FALSE;
	}

	/* Do not accelerate transformed masks. */
	if (NULL != pPictureMask && (NULL != pPictureMask->transform)) {

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXACheckComposite called with transformed mask\n");
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapMskPtr);
		return FALSE;
	}

	/* Do not accelerate repeating masks. */
	if (NULL != pPictureMask && pPictureMask->repeat) {

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXACheckComposite called with repeating mask\n");
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapMskPtr);
		return FALSE;
	}

	/* Do not accelerate 8bpp-or-narrower targets unless backend is Z160. */
	if (8 >= fPixmapDstPtr->bitsPerPixel && IMXEXA_BACKEND_Z160 != imxPtr->backend) {

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXACheckComposite called with target too narrow for current backend\n");
#endif
		imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
		imxexa_update_pixmap_on_failure(fPtr, fPixmapMskPtr);
		return FALSE;
	}

	/* Filter out unsupported blending ops. */
	Bool success = FALSE;

	switch (op) {
	case PictOpSrc:
	case PictOpOver:
		success = TRUE;
		break;
	case PictOpAdd:
		if (IMXEXA_BACKEND_Z160 == imxPtr->backend)
			success = TRUE;
		break;
	case PictOpIn:
		if (IMXEXA_BACKEND_Z160 == imxPtr->backend)
			success = TRUE;
		break;
/*	case PictOpClear:
	case PictOpSrc:
	case PictOpDst:
	case PictOpOver:
	case PictOpOverReverse:
	case PictOpIn:
	case PictOpInReverse:
	case PictOpOut:
	case PictOpOutReverse:
	case PictOpAtop:
	case PictOpAtopReverse:
	case PictOpXor:
	case PictOpAdd:
	case PictOpSaturate:
*/	}

	if (success)
		return TRUE;

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		"IMXEXACheckComposite called with unsupported op (%s)\n",
		imxexa_string_from_pict_op(op));
#endif

	imxexa_update_pixmap_on_failure(fPtr, fPixmapDstPtr);
	imxexa_update_pixmap_on_failure(fPtr, fPixmapSrcPtr);
	imxexa_update_pixmap_on_failure(fPtr, fPixmapMskPtr);

	return FALSE;
}

static Bool
imxexa_prepare_surface_alias(
	imxexa_backend_t backend,
	PictFormatShort pictFormat,
	IMXEXAPtr fPtr,
	IMXEXAPixmapPtr fPixmapPtr)
{
	/* Is there already an alias? */
	if (NULL != fPixmapPtr->alias)
		return TRUE;

	/* Is there a genuine surface? */
	if (NULL == fPixmapPtr->surf)
		return FALSE;

	/* Is surface locked? */
	if (NULL != fPixmapPtr->surfPtr)
		return FALSE;

#if IMX_EXA_DEBUG_PREPARE_COMPOSITE

	/* Is there a bitsPerPixel mismatch? Should not happen, still check for diagnostics. */
	if (PICT_FORMAT_BPP(pictFormat) != fPixmapPtr->bitsPerPixel) {

		xf86DrvMsg(0, X_ERROR,
			"imxexa_prepare_surface_alias encountered mismatching picture format %s (%ubpp, pixmap %ubpp)\n",
			imxexa_string_from_pict_format(pictFormat), PICT_FORMAT_BPP(pictFormat), fPixmapPtr->bitsPerPixel);

		return FALSE;
	}

#endif

	/* Is this the screen surface? Screen surface has a known-in-advance format and does not need aliases. */
	if (fPtr->screenSurf == fPixmapPtr->surf) {

		fPixmapPtr->alias = fPtr->screenSurf;
		return TRUE;
	}

	/* Alias inherits the definition of the genuine surface, except for one field.. */
	C2D_SURFACE_DEF surfDef;
	memcpy(&surfDef, &fPixmapPtr->surfDef, sizeof(surfDef));

	/* Find a valid surface format for the pict format; use that as the alias format. */
	if (!imxexa_surf_format_from_pict(backend, pictFormat, &surfDef.format)) {

		xf86DrvMsg(0, X_ERROR,
			"imxexa_prepare_surface_alias failed to find surface format for pict format %s\n",
			imxexa_string_from_pict_format(pictFormat));

		return FALSE;
	}

	/* Is alias format matching that of the genuine surface? */
	if (fPixmapPtr->surfDef.format == surfDef.format) {

		fPixmapPtr->alias = fPixmapPtr->surf;
		return TRUE;
	}

#if IMX_EXA_DEBUG_PREPARE_COMPOSITE

	xf86DrvMsg(0, X_INFO,
		"imxexa_prepare_surface_alias encountered format cast (%s >> %s)\n",
		imxexa_string_from_c2d_format(fPixmapPtr->surfDef.format),
		imxexa_string_from_c2d_format(surfDef.format));

#endif

	surfDef.flags = C2D_SURFACE_NO_BUFFER_ALLOC;

	const C2D_STATUS r = c2dSurfAlloc(fPtr->gpuContext, &fPixmapPtr->alias, &surfDef);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(0, X_ERROR,
			"imxexa_prepare_surface_alias failed to allocate surface (code 0x%08x) for pict format %s\n",
			r, imxexa_string_from_pict_format(pictFormat));

		return FALSE;
	}

	/* To preserve access exclusivity, make sure the surface we just assigned an alias to */
	/* is not being accessed by the GPU. To avoid future access conflicts, all sanctioned */
	/* access to the surface of this pixmap will be via its alias. */
	c2dWaitForTimestamp(fPtr->gpuContext);

	return TRUE;
}

static Bool
IMXEXAPrepareComposite(
	int op,
	PicturePtr pPictureSrc,
	PicturePtr pPictureMask,
	PicturePtr pPictureDst,
	PixmapPtr pPixmapSrc,
	PixmapPtr pPixmapMask,
	PixmapPtr pPixmapDst)
{
	/* Access the screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Access driver private data associated with pixmaps. */
	IMXEXAPixmapPtr fPixmapDstPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapDst);
	IMXEXAPixmapPtr fPixmapSrcPtr =
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapSrc);
	IMXEXAPixmapPtr fPixmapMskPtr = NULL != pPixmapMask ?
		(IMXEXAPixmapPtr) exaGetPixmapDriverPrivate(pPixmapMask) : NULL;

	/* Remember the pixmaps passed in. */
	fPtr->pPixDst = fPixmapDstPtr;
	fPtr->pPixSrc = fPixmapSrcPtr;
	fPtr->pPixMsk = fPixmapMskPtr;

	/* Set up draw state. */
	if (!imxexa_unlock_surface(fPtr, fPixmapDstPtr) ||
		!imxexa_unlock_surface(fPtr, fPixmapSrcPtr) ||
		!imxexa_unlock_surface(fPtr, fPixmapMskPtr)) {

		return FALSE;
	}

	if (!imxexa_prepare_surface_alias(
			imxPtr->backend,
			pPictureDst->format,
			fPtr,
			fPixmapDstPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareComposite failed to prepare alias for target surface\n");
		return FALSE;
	}

	if (!imxexa_prepare_surface_alias(
			imxPtr->backend,
			pPictureSrc->format,
			fPtr,
			fPixmapSrcPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareComposite failed to prepare alias for source surface\n");
		return FALSE;
	}

	if (NULL != pPixmapMask &&
		!imxexa_prepare_surface_alias(
			imxPtr->backend,
			pPictureMask->format,
			fPtr,
			fPixmapMskPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAPrepareComposite failed to prepare alias for mask surface\n");
		return FALSE;
	}

	switch (op) {
	case PictOpSrc:
		c2dSetBlendMode(fPtr->gpuContext, C2D_ALPHA_BLEND_NONE);
		break;
	case PictOpOver:
		c2dSetBlendMode(fPtr->gpuContext, C2D_ALPHA_BLEND_SRCOVER);
		break;
	case PictOpAdd:
		c2dSetBlendMode(fPtr->gpuContext, C2D_ALPHA_BLEND_ADDITIVE);
		break;
	case PictOpIn:
		c2dSetBlendMode(fPtr->gpuContext, C2D_ALPHA_BLEND_SRCIN);
		break;
	default:
		return FALSE;
	}

	fPtr->composConvert = pPictureDst->format != pPictureSrc->format;

	c2dSetDstSurface(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapDstPtr));
	c2dSetSrcSurface(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapSrcPtr));

	/* Repeating source is a special case of pattern fill on the Z160 backend */
	/* (which is an anachronism; c2d_z160 needs to be brought up to date and on par with c2d_z430) */
	if (pPictureSrc->repeat && IMXEXA_BACKEND_Z160 == imxPtr->backend) {

		c2dSetBrushSurface(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapSrcPtr), NULL);
		fPtr->composRepeat = TRUE;
	}
	else {
		c2dSetBrushSurface(fPtr->gpuContext, NULL, NULL);
		fPtr->composRepeat = FALSE;
	}

	/* Set up mask, but watch out for Z160 doing pattern fill - combining the two can produce hard lock-ups. */
	if (NULL != pPixmapMask && !fPtr->composRepeat)
		c2dSetMaskSurface(fPtr->gpuContext, imxexa_get_preferred_surface(fPixmapMskPtr), NULL);
	else
		c2dSetMaskSurface(fPtr->gpuContext, NULL, NULL);

	/* Mark pixmaps as used and update driver's heartbeat. */
	imxexa_update_pixmap_on_use(fPtr, fPixmapDstPtr);
	imxexa_update_pixmap_on_use(fPtr, fPixmapSrcPtr);
	imxexa_update_pixmap_on_use(fPtr, fPixmapMskPtr);
	++fPtr->heartbeat;

#if IMX_EXA_DEBUG_PREPARE_COMPOSITE

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXAPrepareComposite called with %s on dst, src, msk (priv rec %p %p %p)\n",
		imxexa_string_from_pict_op(op),
		fPixmapDstPtr,
		fPixmapSrcPtr,
		fPixmapMskPtr);

#elif IMX_EXA_DEBUG_TRACE

	if (is_tracing()) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXEXAPrepareComposite called with %s on dst, src, msk (priv rec %p %p %p)\n",
			imxexa_string_from_pict_op(op),
			fPixmapDstPtr,
			fPixmapSrcPtr,
			fPixmapMskPtr);
	}

#endif

	return TRUE;
}

static void
IMXEXAComposite(
	PixmapPtr pPixmapDst,
	int srcX,
	int srcY,
	int maskX,
	int maskY,
	int dstX,
	int dstY,
	int width,
	int height)
{
	/* Access screen info associated with dst pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	C2D_RECT rectDst = {
		.x = dstX,
		.y = dstY,
		.width = width,
		.height = height
	};

	C2D_RECT rectSrc = {
		.x = srcX,
		.y = srcY,
		.width = width,
		.height = height
	};

	c2dSetDstRectangle(fPtr->gpuContext, &rectDst);
	c2dSetSrcRectangle(fPtr->gpuContext, &rectSrc);

	C2D_STATUS r;

	if (fPtr->composRepeat)
		r = c2dDrawRect(fPtr->gpuContext, C2D_PARAM_PATTERN_BIT);
	else
		r = c2dDrawBlit(fPtr->gpuContext);

	if (C2D_STATUS_OK != r) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"IMXEXAComposite failed to perform GPU draw (code: 0x%08x) - %s\n",
			r, (fPtr->composRepeat ? "pattern fill" : "blit"));
	}

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++fPtr->numCompositeBeforeSync;

	if (fPtr->composConvert)
		++fPtr->numConvBeforeSync;

#endif

#if IMX_EXA_DEBUG_COMPOSITE

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"IMXEXAComposite called with src (%d-%d, %d-%d), mask offset (%d, %d), dst (%d-%d, %d-%d)\n",
		srcX, srcX + width,
		srcY, srcY + height,
		maskX,
		maskY,
		dstX, dstX + width,
		dstY, dstY + height);
	}

#elif IMX_EXA_DEBUG_TRACE

	if (is_tracing()) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"IMXEXAComposite called with src (%d-%d, %d-%d), mask offset (%d, %d), dst (%d-%d, %d-%d)\n",
			srcX, srcX + width,
			srcY, srcY + height,
			maskX,
			maskY,
			dstX, dstX + width,
			dstY, dstY + height);
	}

#endif

}

static void
IMXEXADoneComposite(
	PixmapPtr pPixmapDst)
{
	/* Access screen info associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	if (NULL != fPtr->gpuContext) {

		/* Flush pending operations to the GPU. */
		c2dFlush(fPtr->gpuContext);

		fPtr->gpuSynced = FALSE;

		fPtr->pPixDst = NULL;
		fPtr->pPixSrc = NULL;
		fPtr->pPixMsk = NULL;
	}
}

Bool
IMX_EXA_PreInit(ScrnInfoPtr pScrn)
{
	XF86ModReqInfo req;
	memset(&req, 0, sizeof(req));
	req.majorversion = EXA_VERSION_MAJOR;
	req.minorversion = EXA_VERSION_MINOR;

	int errmaj, errmin;

	if (!LoadSubModule(pScrn->module, "exa", NULL, NULL, NULL, &req,
			&errmaj, &errmin)) {

		LoaderErrorMsg(NULL, "exa", errmaj, errmin);
		return FALSE;
	}

	return TRUE;
}

Bool
IMX_EXA_ScreenInit(int scrnIndex, ScreenPtr pScreen)
{
	/* Access screen info associated with this screen. */
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);

	/* Compute the number of bytes per pixel */
	unsigned bytesPerPixel = ((pScrn->bitsPerPixel + 7) / 8);

	/* Compute the number of bytes used by the screen. */
	unsigned numScreenBytes = pScrn->displayWidth * pScrn->virtualY * bytesPerPixel;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "physAddr=0x%08x fbstart=0x%08x fbmem=0x%08x fboff=0x%08x\n",
		(int)(pScrn->memPhysBase),
		(int)(imxPtr->fbstart),
		(int)(imxPtr->fbmem),
		imxPtr->fboff);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "framebuffer: size=%dx%d bits=%d screenbytes=%d stride=%u\n",
		pScrn->virtualX,
		pScrn->virtualY,
		pScrn->bitsPerPixel,
		numScreenBytes,
		bytesPerPixel * pScrn->displayWidth);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "rgbOffset=%d,%d,%d rgbMask=0x%08x,0x%08x,0x%08x\n",
		(int)(pScrn->offset.red),
		(int)(pScrn->offset.green),
		(int)(pScrn->offset.blue),
		(int)(pScrn->mask.red),
		(int)(pScrn->mask.green),
		(int)(pScrn->mask.blue));

	/* Initialize EXA. */
	imxPtr->exaDriverPtr = exaDriverAlloc();
	if (NULL == imxPtr->exaDriverPtr) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "EXA driver structure initialization failed.\n");
		return FALSE;
	}

	imxPtr->exaDriverPtr->flags = EXA_OFFSCREEN_PIXMAPS;
	imxPtr->exaDriverPtr->exa_major = EXA_VERSION_MAJOR;
	imxPtr->exaDriverPtr->exa_minor = EXA_VERSION_MINOR;
	imxPtr->exaDriverPtr->memoryBase = imxPtr->fbstart;
	imxPtr->exaDriverPtr->memorySize = fbdevHWGetVidmem(pScrn);
	imxPtr->exaDriverPtr->offScreenBase = numScreenBytes;
	imxPtr->exaDriverPtr->pixmapOffsetAlign = getpagesize();
	imxPtr->exaDriverPtr->pixmapPitchAlign = 32 * 4; /* 32 pixels by 32bpp max */
	imxPtr->exaDriverPtr->maxPitchBytes = IMX_EXA_MAX_SURF_DIM * 4;
	imxPtr->exaDriverPtr->maxX = IMX_EXA_MAX_SURF_DIM - 1;
	imxPtr->exaDriverPtr->maxY = IMX_EXA_MAX_SURF_DIM - 1;

	/* Required */
	imxPtr->exaDriverPtr->WaitMarker = IMXEXAWaitMarker;

	/* Solid fill - required */
	imxPtr->exaDriverPtr->PrepareSolid = IMXEXAPrepareSolid;
	imxPtr->exaDriverPtr->Solid = IMXEXASolid;
	imxPtr->exaDriverPtr->DoneSolid = IMXEXADoneSolid;

	/* Copy - required */
	imxPtr->exaDriverPtr->PrepareCopy = IMXEXAPrepareCopy;
	imxPtr->exaDriverPtr->Copy = IMXEXACopy;
	imxPtr->exaDriverPtr->DoneCopy = IMXEXADoneCopy;

	/* Composite - optional. */
	if (xf86ReturnOptValBool(imxPtr->options, OPTION_COMPOSITING, TRUE))
	{
		imxPtr->exaDriverPtr->CheckComposite = IMXEXACheckComposite;
		imxPtr->exaDriverPtr->PrepareComposite = IMXEXAPrepareComposite;
		imxPtr->exaDriverPtr->Composite = IMXEXAComposite;
		imxPtr->exaDriverPtr->DoneComposite = IMXEXADoneComposite;
	}

	/* Screen upload/download */
	imxPtr->exaDriverPtr->UploadToScreen = IMXEXAUploadToScreen;
	imxPtr->exaDriverPtr->DownloadFromScreen = IMXEXADownloadFromScreen;

	/* Prepare/Finish access */
	imxPtr->exaDriverPtr->PrepareAccess = IMXEXAPrepareAccess;
	imxPtr->exaDriverPtr->FinishAccess = IMXEXAFinishAccess;

	/* For driver pixmap allocation. */
	imxPtr->exaDriverPtr->flags |= EXA_HANDLES_PIXMAPS;
	imxPtr->exaDriverPtr->flags |= EXA_SUPPORTS_PREPARE_AUX;
	imxPtr->exaDriverPtr->flags |= EXA_SUPPORTS_OFFSCREEN_OVERLAPS;

	imxPtr->exaDriverPtr->CreatePixmap2 = IMXEXACreatePixmap2;
	imxPtr->exaDriverPtr->DestroyPixmap = IMXEXADestroyPixmap;
	imxPtr->exaDriverPtr->ModifyPixmapHeader = IMXEXAModifyPixmapHeader;
	imxPtr->exaDriverPtr->PixmapIsOffscreen = IMXEXAPixmapIsOffscreen;

	if (!exaDriverInit(pScreen, imxPtr->exaDriverPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "EXA initialization failed.\n");
		free(imxPtr->exaDriverPtr);
		imxPtr->exaDriverPtr = NULL;
		return FALSE;
	}

	/* Connect to the GPU if accelerated backend in use. */
	if (!imxexa_gpu_context_acquire(pScrn))
		imxPtr->backend = IMXEXA_BACKEND_NONE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"Using %s backend\n",
		(imxPtr->backend == IMXEXA_BACKEND_Z160 ? "Z160" :
		(imxPtr->backend == IMXEXA_BACKEND_Z430 ? "Z430" :
		"software fallback")) );

	return TRUE;
}

Bool
IMX_EXA_CloseScreen(int scrnIndex, ScreenPtr pScreen)
{
	/* Access screen info associated with this screen. */
	ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);

	/* Disconnect from the GPU if accelerated backend in use. */
	imxexa_gpu_context_release(pScrn);

	/* EXA interface cleanup. */
	if (imxPtr->exaDriverPtr) {

		exaDriverFini(pScreen);
		free(imxPtr->exaDriverPtr);
		imxPtr->exaDriverPtr = NULL;
	}

	return TRUE;
}
