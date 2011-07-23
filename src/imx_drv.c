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

/*
 * Based on xorg fbdev.c
 *
 * Authors:  Alan Hourihane, <alanh@fairlite.demon.co.uk>
 *	     Michel Dänzer, <michel@tungstengraphics.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <string.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>

/* all driver need this */
#include "xf86.h"
#include "xf86_OSproc.h"

#include "mipointer.h"
#include "mibstore.h"
#include "micmap.h"
#include "colormapst.h"
#include "xf86cmap.h"
#include "exa.h"

/* for visuals */
#include "fb.h"

#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) < 6
#include "xf86Resources.h"
#include "xf86RAC.h"
#endif

#include "fbdevhw.h"

#include "imx_type.h"

#include "xf86xv.h"

static Bool debug = 0;

#define TRACE_ENTER(str) \
    do { if (debug) ErrorF("imx: " str " %d\n",pScrn->scrnIndex); } while (0)
#define TRACE_EXIT(str) \
    do { if (debug) ErrorF("imx: " str " done\n"); } while (0)
#define TRACE(str) \
    do { if (debug) ErrorF("inx trace: " str "\n"); } while (0)

/* -------------------------------------------------------------------- */
/* prototypes                                                           */

static const OptionInfoRec * IMXAvailableOptions(int chipid, int busid);
static void	IMXIdentify(int flags);
static Bool	IMXProbe(DriverPtr drv, int flags);
static Bool	IMXPreInit(ScrnInfoPtr pScrn, int flags);
static Bool	IMXScreenInit(int Index, ScreenPtr pScreen, int argc, char **argv);
static Bool	IMXCloseScreen(int scrnIndex, ScreenPtr pScreen);
static Bool	IMXDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr);

/* for XV acceleration */
extern int IMXXVInitAdaptorC2D(ScrnInfoPtr, XF86VideoAdaptorPtr **);

/* for EXA (X acceleration) */
extern void IMX_EXA_GetRec(ScrnInfoPtr pScrn);
extern void IMX_EXA_FreeRec(ScrnInfoPtr pScrn);
extern Bool IMX_EXA_PreInit(ScrnInfoPtr pScrn);
extern Bool IMX_EXA_ScreenInit(int scrnIndex, ScreenPtr pScreen);
extern Bool IMX_EXA_CloseScreen(int scrnIndex, ScreenPtr pScreen);
extern Bool IMX_EXA_GetPixmapProperties(PixmapPtr pPixmap, void** pPhysAddr, int* pPitch);

/* for X extension */
extern void IMX_EXT_Init();

/* -------------------------------------------------------------------- */

/*
 * This is intentionally screen-independent.  It indicates the binding
 * choice made in the first PreInit.
 */
static int pix24bpp = 0;

#define IMX_VERSION		1000
#define IMX_NAME		"IMX"
#define IMX_DRIVER_NAME	"imx"

_X_EXPORT DriverRec IMX = {
	IMX_VERSION,
	IMX_DRIVER_NAME,
	IMXIdentify,
	IMXProbe,
	IMXAvailableOptions,
	NULL,
	0,
	IMXDriverFunc,
};

/* Supported "chipsets" */
static SymTabRec IMXChipsets[] = {
    { 0, "imx" },
    {-1, NULL }
};

#define OPTION_STR_FBDEV		"fbdev"
#define OPTION_STR_NOACCEL		"NoAccel"
#define OPTION_STR_ACCELMETHOD	"AccelMethod"
#define OPTION_STR_BACKEND		"Backend"
#define OPTION_STR_COMPOSITING	"Compositing"
#define OPTION_STR_XV_BILINEAR	"XvBilinear"
#define OPTION_STR_DEBUG		"Debug"

static const OptionInfoRec IMXOptions[] = {
	{ OPTION_FBDEV,			OPTION_STR_FBDEV,		OPTV_STRING,	{0},	FALSE },
	{ OPTION_NOACCEL,		OPTION_STR_NOACCEL,		OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_ACCELMETHOD,	OPTION_STR_ACCELMETHOD,	OPTV_STRING,	{0},	FALSE },
	{ OPTION_BACKEND,		OPTION_STR_BACKEND,		OPTV_STRING,	{0},	FALSE },
	{ OPTION_COMPOSITING,	OPTION_STR_COMPOSITING,	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_XV_BILINEAR,	OPTION_STR_XV_BILINEAR,	OPTV_BOOLEAN,	{0},	FALSE },
	{ OPTION_DEBUG,			OPTION_STR_DEBUG,		OPTV_BOOLEAN,	{0},	FALSE },
	{ -1,					NULL,					OPTV_NONE,		{0},	FALSE }
};

/* -------------------------------------------------------------------- */

#ifdef XFree86LOADER

MODULESETUPPROTO(IMXSetup);

static XF86ModuleVersionInfo IMXVersRec =
{
	"imx",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	NULL,
	{0,0,0,0}
};

_X_EXPORT XF86ModuleData imxModuleData = { &IMXVersRec, IMXSetup, NULL };

pointer
IMXSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&IMX, module, HaveDriverFuncs);
		return (pointer)1;
	} else {
		if (errmaj) *errmaj = LDR_ONCEONLY;
		return NULL;
	}
}

#endif /* XFree86LOADER */

static Bool
IMXGetRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate != NULL)
		return TRUE;
	
	pScrn->driverPrivate = xnfcalloc(1, sizeof(IMXRec));

	IMXPtr fPtr = IMXPTR(pScrn);

	fPtr->backend = IMXEXA_BACKEND_NONE;

	IMX_EXA_GetRec(pScrn);

	return TRUE;
}

static void
IMXFreeRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate == NULL)
		return;
	IMX_EXA_FreeRec(pScrn);
	free(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}

/* -------------------------------------------------------------------- */

static const OptionInfoRec *
IMXAvailableOptions(int chipid, int busid)
{
	return IMXOptions;
}

static void
IMXIdentify(int flags)
{
	xf86PrintChipsets(IMX_NAME, "driver for framebuffer", IMXChipsets);
}

static Bool
IMXProbe(DriverPtr drv, int flags)
{
	int i;
	ScrnInfoPtr pScrn;
       	GDevPtr *devSections;
	int numDevSections;
	char *dev;
	Bool foundScreen = FALSE;

	TRACE("probe start");

	/* For now, just bail out for PROBE_DETECT. */
	if (flags & PROBE_DETECT)
		return FALSE;

	if ((numDevSections = xf86MatchDevice(IMX_DRIVER_NAME, &devSections)) <= 0) 
	    return FALSE;
	
	if (!xf86LoadDrvSubModule(drv, "fbdevhw"))
	    return FALSE;
	    
	for (i = 0; i < numDevSections; i++) {

	    dev = xf86FindOptionValue(devSections[i]->options, OPTION_STR_FBDEV);
	    if (fbdevHWProbe(NULL, dev, NULL)) {
			int entity;
			pScrn = NULL;

			entity = xf86ClaimFbSlot(drv, 0,
				devSections[i], TRUE);

			pScrn = xf86ConfigFbEntity(pScrn, 0, entity,
				NULL, NULL, NULL, NULL);
			   
			if (pScrn) {
				foundScreen = TRUE;
				
				pScrn->driverVersion = IMX_VERSION;
				pScrn->driverName    = IMX_DRIVER_NAME;
				pScrn->name          = IMX_NAME;
				pScrn->Probe         = IMXProbe;
				pScrn->PreInit       = IMXPreInit;
				pScrn->ScreenInit    = IMXScreenInit;
				pScrn->SwitchMode    = fbdevHWSwitchModeWeak();
				pScrn->AdjustFrame   = fbdevHWAdjustFrameWeak();
				pScrn->EnterVT       = fbdevHWEnterVTWeak();
				pScrn->LeaveVT       = fbdevHWLeaveVTWeak();
				pScrn->ValidMode     = fbdevHWValidModeWeak();
				
				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
					   "using %s\n", dev ? dev : "default device");
			}
		}
	}
	free(devSections);

	TRACE("probe done");
	return foundScreen;
}

static Bool
IMXPreInit(ScrnInfoPtr pScrn, int flags)
{
	IMXPtr fPtr;
	int default_depth, fbbpp;
	const char *s;
	int type;

	if (flags & PROBE_DETECT)
		return FALSE;

	TRACE_ENTER("PreInit");

	/* Check the number of entities, and fail if it isn't one. */
	if (pScrn->numEntities != 1)
		return FALSE;

	pScrn->monitor = pScrn->confScreen->monitor;

	IMXGetRec(pScrn);
	fPtr = IMXPTR(pScrn);

	fPtr->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

#ifndef XSERVER_LIBPCIACCESS
	pScrn->racMemFlags = RAC_FB | RAC_COLORMAP | RAC_CURSOR | RAC_VIEWPORT;
	/* XXX Is this right?  Can probably remove RAC_FB */
	pScrn->racIoFlags = RAC_FB | RAC_COLORMAP | RAC_CURSOR | RAC_VIEWPORT;

	if (fPtr->pEnt->location.type == BUS_PCI &&
	    xf86RegisterResources(fPtr->pEnt->index,NULL,ResExclusive)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "xf86RegisterResources() found resource conflicts\n");
		return FALSE;
	}
#endif

	/* open device */
	if (!fbdevHWInit(pScrn,NULL,xf86FindOptionValue(fPtr->pEnt->device->options, OPTION_STR_FBDEV)))
		return FALSE;
	default_depth = fbdevHWGetDepth(pScrn,&fbbpp);
	if (!xf86SetDepthBpp(pScrn, default_depth, default_depth, fbbpp,
			     Support24bppFb | Support32bppFb | SupportConvert32to24 | SupportConvert24to32))
		return FALSE;
	xf86PrintDepthBpp(pScrn);

	/* Get the depth24 pixmap format */
	if (pScrn->depth == 24 && pix24bpp == 0)
		pix24bpp = xf86GetBppFromDepth(pScrn, 24);

	/* color weight */
	if (pScrn->depth > 8) {
		rgb zeros = { 0, 0, 0 };
		if (!xf86SetWeight(pScrn, zeros, zeros))
			return FALSE;
	}

	/* visual init */
	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;

	/* We don't currently support DirectColor at > 8bpp */
	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"requested default visual (%s) is not supported at depth %d\n",
			xf86GetVisualName(pScrn->defaultVisual), pScrn->depth);
		return FALSE;
	}

	{
		Gamma zeros = {0.0, 0.0, 0.0};

		if (!xf86SetGamma(pScrn,zeros)) {
			return FALSE;
		}
	}

	pScrn->progClock = TRUE;
	pScrn->rgbBits   = 8;
	pScrn->chipset   = "imx";
	pScrn->videoRam  = fbdevHWGetVidmem(pScrn);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"hardware: %s (video memory: %dkB)\n",
		fbdevHWGetName(pScrn), pScrn->videoRam / 1024);

	/* handle options */
	xf86CollectOptions(pScrn, NULL);
	if (!(fPtr->options = malloc(sizeof(IMXOptions))))
		return FALSE;
	memcpy(fPtr->options, IMXOptions, sizeof(IMXOptions));
	xf86ProcessOptions(pScrn->scrnIndex, fPtr->pEnt->device->options, fPtr->options);

	/* NoAccel option */
	Bool useAccel = TRUE;

	if (xf86ReturnOptValBool(fPtr->options, OPTION_NOACCEL, FALSE)) {
		useAccel = FALSE;
	}

	/* AccelMethod option */
	if (useAccel) {
		s = xf86GetOptValString(fPtr->options, OPTION_ACCELMETHOD);
		if ((NULL != s) && (0 != xf86NameCmp(s, "EXA"))) {
			useAccel = FALSE;
		}
	} 

	/* Backend option */
	if (useAccel) {
		const char* lib_name = NULL;

		s = xf86GetOptValString(fPtr->options, OPTION_BACKEND);
		if ((NULL != s) && (0 == xf86NameCmp(s, "Z430"))) {
			fPtr->backend = IMXEXA_BACKEND_Z430;
			lib_name = "libc2d_z430.so";
		}
		else {
			fPtr->backend = IMXEXA_BACKEND_Z160;
			lib_name = "libc2d_z160.so";
		}

		if (!dlopen(lib_name, RTLD_NOW | RTLD_GLOBAL))
		{
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"failed to load GPU acceleration library (dlerror: %s)\n",
				dlerror());

			fPtr->backend = IMXEXA_BACKEND_NONE;
		}
	}

	/* Debug option */
	debug = xf86ReturnOptValBool(fPtr->options, OPTION_DEBUG, FALSE);

	/* Select video modes */
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "checking modes against framebuffer device...\n");
	fbdevHWSetVideoModes(pScrn);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "checking modes against monitor...\n");
	{
		DisplayModePtr mode, first = mode = pScrn->modes;

		if (mode != NULL) do {
			mode->status = xf86CheckModeForMonitor(mode, pScrn->monitor);
			mode = mode->next;
		} while (mode != NULL && mode != first);

		xf86PruneDriverModes(pScrn);
	}

	if (NULL == pScrn->modes)
		fbdevHWUseBuildinMode(pScrn);
	pScrn->currentMode = pScrn->modes;

	/* First approximation, may be refined in ScreenInit */
	pScrn->displayWidth = pScrn->virtualX;

	xf86PrintModes(pScrn);

	/* Set display resolution */
	xf86SetDpi(pScrn, 0, 0);

	/* Load bpp-specific modules */
	switch ((type = fbdevHWGetType(pScrn)))
	{
	case FBDEVHW_PLANES:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"plane mode is not supported by the imx driver\n");
		return FALSE;
	case FBDEVHW_PACKED_PIXELS:
		switch (pScrn->bitsPerPixel)
		{
		case 8:
		case 16:
		case 24:
		case 32:
			break;
		default:
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"unsupported number of bits per pixel: %d",
				pScrn->bitsPerPixel);
			return FALSE;
		}
		break;
	case FBDEVHW_INTERLEAVED_PLANES:
		/* Not supported yet, don't know what to do with this */
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"interleaved planes are not yet supported by the imx driver\n");
		return FALSE;
	case FBDEVHW_TEXT:
		/* This should never happen ...
		 * we should check for this much much earlier ... */
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"text mode is not supported by the fbdev driver\n");
		return FALSE;
	case FBDEVHW_VGA_PLANES:
		/* Not supported yet */
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"EGA/VGA planes are not yet supported by the imx driver\n");
		return FALSE;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"unrecognised imx hardware type (%d)\n", type);
		return FALSE;
	}
	if (xf86LoadSubModule(pScrn, "fb") == NULL) {
		IMXFreeRec(pScrn);
		return FALSE;
	}

	/* Perform EXA pre-init */
	if (!IMX_EXA_PreInit(pScrn)) {
		IMXFreeRec(pScrn);
		return FALSE;
	}

	fPtr->use_bilinear_filtering = xf86ReturnOptValBool(fPtr->options, OPTION_XV_BILINEAR, TRUE);

	/* Register adaptors in the reverse order we want them enumerated. */
	/* xserver/hw/xfree86/common/xf86xv.c:xf86XVListGenericAdaptors() lists them in reverse. */
	if (IMXEXA_BACKEND_NONE != fPtr->backend) {
		xf86XVRegisterGenericAdaptorDriver(IMXXVInitAdaptorC2D);
	}

	TRACE_EXIT("PreInit");
	return TRUE;
}

static Bool
IMXScreenInit(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	IMXPtr fPtr = IMXPTR(pScrn);
	VisualPtr visual;
	int init_picture = 0;
	int ret, flags;
	int type;

	TRACE_ENTER("IMXScreenInit");

#if DEBUG
	ErrorF("\tbitsPerPixel=%d, depth=%d, defaultVisual=%s\n"
	       "\tmask: %x,%x,%x, offset: %d,%d,%d\n",
	       pScrn->bitsPerPixel,
	       pScrn->depth,
	       xf86GetVisualName(pScrn->defaultVisual),
	       pScrn->mask.red,pScrn->mask.green,pScrn->mask.blue,
	       pScrn->offset.red,pScrn->offset.green,pScrn->offset.blue);
#endif

	if (NULL == (fPtr->fbmem = fbdevHWMapVidmem(pScrn))) {
	        xf86DrvMsg(scrnIndex,X_ERROR,"mapping of video memory"
			   " failed\n");
		return FALSE;
	}
	fPtr->fboff = fbdevHWLinearOffset(pScrn);

	fbdevHWSave(pScrn);

	if (!fbdevHWModeInit(pScrn, pScrn->currentMode)) {
		xf86DrvMsg(scrnIndex,X_ERROR,"mode initialization failed\n");
		return FALSE;
	}
	fbdevHWSaveScreen(pScreen, SCREEN_SAVER_ON);
	fbdevHWAdjustFrame(scrnIndex,0,0,0);

	/* mi layer */
	miClearVisualTypes();
	if (pScrn->bitsPerPixel > 8) {
		if (!miSetVisualTypes(pScrn->depth, TrueColorMask, pScrn->rgbBits, TrueColor)) {
			xf86DrvMsg(scrnIndex,X_ERROR,"visual type setup failed"
				   " for %d bits per pixel [1]\n",
				   pScrn->bitsPerPixel);
			return FALSE;
		}
	} else {
		if (!miSetVisualTypes(pScrn->depth,
				      miGetDefaultVisualMask(pScrn->depth),
				      pScrn->rgbBits, pScrn->defaultVisual)) {
			xf86DrvMsg(scrnIndex,X_ERROR,"visual type setup failed"
				   " for %d bits per pixel [2]\n",
				   pScrn->bitsPerPixel);
			return FALSE;
		}
	}
	if (!miSetPixmapDepths()) {
	  xf86DrvMsg(scrnIndex,X_ERROR,"pixmap depth setup failed\n");
	  return FALSE;
	}

	/* FIXME: this doesn't work for all cases, e.g. when each scanline
		has a padding which is independent from the depth (controlfb) */
	pScrn->displayWidth = fbdevHWGetLineLength(pScrn) /
		(pScrn->bitsPerPixel / 8);

	if (pScrn->displayWidth != pScrn->virtualX) {
		xf86DrvMsg(scrnIndex, X_INFO,
			"Pitch updated to %d after ModeInit\n",
			pScrn->displayWidth);
	}

	fPtr->fbstart = fPtr->fbmem + fPtr->fboff;

	switch ((type = fbdevHWGetType(pScrn)))
	{
	case FBDEVHW_PACKED_PIXELS:
		switch (pScrn->bitsPerPixel) {
		case 8:
		case 16:
		case 24:
		case 32:
			ret = fbScreenInit(pScreen, fPtr->fbstart,
					   pScrn->virtualX, pScrn->virtualY,
					   pScrn->xDpi, pScrn->yDpi,
					   pScrn->displayWidth,
					   pScrn->bitsPerPixel);
			init_picture = 1;
			break;
	 	default:
			xf86DrvMsg(scrnIndex, X_ERROR,
				   "internal error: invalid number of bits per"
				   " pixel (%d) encountered in"
				   " IMXScreenInit()\n", pScrn->bitsPerPixel);
			ret = FALSE;
			break;
		}
		break;
	case FBDEVHW_INTERLEAVED_PLANES:
		/* This should never happen ...
		* we should check for this much much earlier ... */
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: interleaved planes are not yet "
			   "supported by the imx driver\n");
		ret = FALSE;
		break;
	case FBDEVHW_TEXT:
		/* This should never happen ...
		* we should check for this much much earlier ... */
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: text mode is not supported by the "
			   "imx driver\n");
		ret = FALSE;
		break;
	case FBDEVHW_VGA_PLANES:
		/* Not supported yet */
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: EGA/VGA Planes are not yet "
			   "supported by the imx driver\n");
		ret = FALSE;
		break;
	default:
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: unrecognised hardware type (%d) "
			   "encountered in IMXScreenInit()\n", type);
		ret = FALSE;
		break;
	}
	if (!ret)
		return FALSE;

	if (pScrn->bitsPerPixel > 8) {
		/* Fixup RGB ordering */
		visual = pScreen->visuals + pScreen->numVisuals;
		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed   = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue  = pScrn->offset.blue;
				visual->redMask     = pScrn->mask.red;
				visual->greenMask   = pScrn->mask.green;
				visual->blueMask    = pScrn->mask.blue;
			}
		}
	}

	/* must be after RGB ordering fixed */
	if (init_picture && !fbPictureInit(pScreen, NULL, 0))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			"Render extension initialisation failed\n");

	xf86SetBlackWhitePixels(pScreen);

	/* INITIALIZE ACCELERATION BEFORE INIT FOR BACKING STORE AND SOFTWARE CURSOR */ 
	if (!IMX_EXA_ScreenInit(scrnIndex, pScreen)) {

	    xf86DrvMsg(scrnIndex, X_ERROR,
		       "IMX EXA failed to initialize screen.\n");
	    return FALSE;
	}

	/* If acceleration was enabled, then initialize the extension. */
	if (IMXEXA_BACKEND_NONE != fPtr->backend) {

		IMX_EXT_Init();
	}

	/* Note if acceleration is in use. */
	if (IMXEXA_BACKEND_NONE != fPtr->backend) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "IMX EXA acceleration setup successful\n");

	} else {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "IMX EXA acceleration setup failed - no acceleration in use\n");
	}
	miInitializeBackingStore(pScreen);
	xf86SetBackingStore(pScreen);

	/* software cursor */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	/* colormap */
	switch ((type = fbdevHWGetType(pScrn)))
	{
	/* XXX It would be simpler to use miCreateDefColormap() in all cases. */
	case FBDEVHW_PACKED_PIXELS:
		if (!miCreateDefColormap(pScreen)) {
			xf86DrvMsg(scrnIndex, X_ERROR,
                                   "internal error: miCreateDefColormap failed "
				   "in IMXScreenInit()\n");
			return FALSE;
		}
		break;
	case FBDEVHW_INTERLEAVED_PLANES:
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: interleaved planes are not yet "
			   "supported by the imx driver\n");
		return FALSE;
	case FBDEVHW_TEXT:
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: text mode is not supported by "
			   "the imx driver\n");
		return FALSE;
	case FBDEVHW_VGA_PLANES:
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: EGA/VGA planes are not yet "
			   "supported by the imx driver\n");
		return FALSE;
	default:
		xf86DrvMsg(scrnIndex, X_ERROR,
		           "internal error: unrecognised imx hardware type "
			   "(%d) encountered in IMXScreenInit()\n", type);
		return FALSE;
	}
	flags = CMAP_PALETTED_TRUECOLOR;
	if(!xf86HandleColormaps(pScreen, 256, 8, fbdevHWLoadPaletteWeak(), 
				NULL, flags))
		return FALSE;

	xf86DPMSInit(pScreen, fbdevHWDPMSSetWeak(), 0);

	pScreen->SaveScreen = fbdevHWSaveScreenWeak();

	/* Wrap the current CloseScreen function. */
	fPtr->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = IMXCloseScreen;

	/* Init the Xv Adaptor. */
	XF86VideoAdaptorPtr *ptr;
	int n = xf86XVListGenericAdaptors(pScrn, &ptr);

	if (0 < n) {
		xf86XVScreenInit(pScreen, ptr, n);
	}

	TRACE_EXIT("IMXScreenInit");

	return TRUE;
}

static Bool
IMXCloseScreen(int scrnIndex, ScreenPtr pScreen)
{
	IMX_EXA_CloseScreen(scrnIndex, pScreen);

	ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
	IMXPtr fPtr = IMXPTR(pScrn);

	fbdevHWRestore(pScrn);
	fbdevHWUnmapVidmem(pScrn);
	pScrn->vtSema = FALSE;

	pScreen->CloseScreen = fPtr->CloseScreen;

	IMXFreeRec(pScrn);

	return (*pScreen->CloseScreen)(scrnIndex, pScreen);
}

Bool
IMXGetPixmapProperties(
	PixmapPtr pPixmap,
	void** pPhysAddr,
	int* pPitch)
{
	/* Initialize values to be returned. */
	*pPhysAddr = NULL;
	*pPitch = 0;

	/* Is there a pixmap? */
	if (NULL == pPixmap) {
		return FALSE;
	}

	/* Access screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Check if the screen associated with this pixmap has IMX driver. */
	if (0 != strcmp(IMX_DRIVER_NAME, pScrn->driverName)) {
		return FALSE;
	}

	/* Access driver specific content. */
	IMXPtr fPtr = IMXPTR(pScrn);

	/* Cannot process if not accelerating. */
	if (IMXEXA_BACKEND_NONE == fPtr->backend) {
		return FALSE;
	}

	/* If we get here, then query EXA portion of driver. */
	return IMX_EXA_GetPixmapProperties(pPixmap, pPhysAddr, pPitch);
}

static Bool
IMXDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr)
{
    xorgHWFlags *flag;
    
    switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
	    flag = (CARD32*)ptr;
	    (*flag) = 0;
	    return TRUE;
	default:
	    return FALSE;
    }
}
