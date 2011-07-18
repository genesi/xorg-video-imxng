/*
 * Copyright (C) 2009-2010 Freescale Semiconductor, Inc.  All Rights Reserved.
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

#ifndef __IMX_TYPE_H__
#define __IMX_TYPE_H__

#include <xf86.h>
#include <exa.h>

#define IMX_DEBUG_MASTER	1

#include <xf86xv.h>

/* Preparation for the inclusion of c2d_api.h */
#ifndef _LINUX
#define _LINUX
#endif

#ifndef OS_DLLIMPORT
#define OS_DLLIMPORT
#endif

#include <C2D/c2d_api.h>

#define	IMX_EXA_VERSION(maj, min, rel) ((maj) * 0x10000U | (min) * 0x100U | (rel))

#define	IMX_EXA_VERSION_COMPILED IMX_EXA_VERSION( \
	EXA_VERSION_MAJOR, \
	EXA_VERSION_MINOR, \
	EXA_VERSION_RELEASE)

/* Supported config options */
typedef enum {
	OPTION_FBDEV,
	OPTION_NOACCEL,
	OPTION_ACCELMETHOD,
	OPTION_BACKEND,
	OPTION_COMPOSITING,
	OPTION_XV_BILINEAR,
	OPTION_DEBUG,
} IMXOpts;

/* Private data for the driver. */
typedef enum {

	IMXEXA_BACKEND_NONE = 0,
	IMXEXA_BACKEND_Z160,
	IMXEXA_BACKEND_Z430,

	IMXEXA_BACKEND_FORCE_TYPE = -1U

} imxexa_backend_t;

#define IMXXV_NUM_PORTS				4U			/* Number of ports supported by this adaptor. */
#define IMXXV_NUM_PHYS_BUFFERS		(1U << 5)	/* Number of supported physical gstreamer buffers, across all ports. */

#define IMXXV_DBLFB_ENABLE			1			/* Enable double-buffered full-screen XV. */

typedef struct {
	unsigned char*					fbstart;
	unsigned char*					fbmem;
	int								fboff;
	int								lineLength;
	CloseScreenProcPtr				CloseScreen;
	EntityInfoPtr					pEnt;
	OptionInfoPtr					options;

	DevUnion						xvPortPrivate[IMXXV_NUM_PORTS];
	C2D_CONTEXT						xvGpuContext;
	C2D_SURFACE						xvScreenSurf;
#if IMXXV_DBLFB_ENABLE
	C2D_SURFACE						xvScreenSurf2;
	unsigned						xvBufferTracker;
#endif
	C2D_SURFACE_DEF					xvSurfDef[IMXXV_NUM_PORTS];
	C2D_SURFACE						xvSurf[IMXXV_NUM_PORTS];
	C2D_SURFACE						xvSurfAux[IMXXV_NUM_PORTS];
	Bool							report_split[IMXXV_NUM_PORTS];
	Bool							use_bilinear_filtering;

	intptr_t						phys_ptr[IMXXV_NUM_PHYS_BUFFERS];
	void*							mapping[IMXXV_NUM_PHYS_BUFFERS];
	size_t							mapping_len[IMXXV_NUM_PHYS_BUFFERS];
	size_t							mapping_offset[IMXXV_NUM_PHYS_BUFFERS];
	unsigned						num_phys;

	/* EXA acceleration */
	imxexa_backend_t				backend;
	ExaDriverPtr					exaDriverPtr;
	void*							exaDriverPrivate;

} IMXRec, *IMXPtr;

#define IMXPTR(p) ((IMXPtr)((p)->driverPrivate))

/* Private data for the EXA driver. */
typedef struct _IMXEXAPixmapRec *IMXEXAPixmapPtr;

typedef struct _IMXEXARec {

	C2D_CONTEXT		gpuContext;
	Bool			gpuSynced;

	/* GPU surface for the screen */
	C2D_SURFACE_DEF	screenSurfDef;
	C2D_SURFACE		screenSurf;

	/* Parameters originating from PrepareComposite and going into Composite */
	Bool			composRepeat;
	Bool			composConvert;

	/* Pixmap parameters passed into Prepare{Solid,Copy,Composite} */
	IMXEXAPixmapPtr	pPixDst;
	IMXEXAPixmapPtr	pPixSrc;
	IMXEXAPixmapPtr	pPixMsk;

	IMXEXAPixmapPtr	pFirstPix;					/* header of the list of driver-allocated pixmaps, sorted by MRU */
	IMXEXAPixmapPtr pFirstEvictionCandidate;	/* header of the list of LRU-sorted pixmaps, AKA tail of the above */
	uint64_t		heartbeat;					/* counter incremented with each eax op */

#if IMX_DEBUG_MASTER
	unsigned long	numSolidBeforeSync;
	unsigned long	numCopyBeforeSync;
	unsigned long	numConvBeforeSync;
	unsigned long	numCompositeBeforeSync;
	unsigned long	numAccessBeforeSync;
	unsigned long	numUploadBeforeSync;
	unsigned long	numDnloadBeforeSync;
#endif

} IMXEXARec, *IMXEXAPtr;

#define IMXEXAPTR(p) ((IMXEXAPtr)((p)->exaDriverPrivate))
#define PIXMAP_STAMP_EVICTED	-1ULL
#define PIXMAP_STAMP_PINNED		-2ULL

typedef struct _IMXEXAPixmapRec {

	/* Properties for pixmap header passed in CreatePixmap2. */
	int				width;
	int				height;
	int				depth;
	int				bitsPerPixel;

	/* Properties for pixmap allocated from offscreen memory. */
	C2D_SURFACE_DEF	surfDef;		/* genuine surface definition */
	C2D_SURFACE		surf;			/* genuine surface */
	C2D_SURFACE		alias;			/* own-pixel-format alias/proxy of the above */
	void*			surfPtr;		/* ptr to surface buffer (VA) used by lazy unlock */
	uint64_t		stamp;			/* updated at use to the heartbeat of exa ops; see PIXMAP_STAMP_* */

	/* Offscreen pixmap quality metrics */
	unsigned 		n_uses;			/* number of successful acceleration ops pixmap participated in */
	unsigned 		n_failures;		/* number of failed acceleration ops pixmap participated in */

	/* Properties for pixmap allocated from system memory. */
	void*			sysPtr;			/* ptr to sys memory alloc */
	int				sysPitchBytes;	/* bytes per row */

	IMXEXAPixmapPtr	prev;
	IMXEXAPixmapPtr	next;

} IMXEXAPixmapRec;

#endif /* __IMX_TYPE_H__ */
