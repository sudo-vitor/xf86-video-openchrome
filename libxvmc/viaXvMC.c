/*****************************************************************************
 * VIA Unichrome XvMC extension client lib.
 *
 * Copyright (c) 2004-2005 Thomas Hellström. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHOR(S) OR COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 *Author: Thomas Hellström, 2004.
 *Bugfixes by among others Pascal Brisset and Terry Barnaby.
 *DRI protocol support by Thomas Hellström, 2005.
 */

#undef WAITPAUSE

#include "viaXvMCPriv.h"
#include "viaLowLevel.h"
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <fourcc.h>
#include <X11/extensions/Xv.h>
#include <xf86drm.h>
#include <pthread.h>
#include "vldXvMC.h"
#include "xf86dri.h"
#include "driDrawable.h"
#include "wsbm_manager.h"
#include "wsbm_pool.h"

#define SAREAPTR(ctx) ((ViaXvMCSAreaPriv *)			\
		       (((CARD8 *)(ctx)->sAreaAddress) +	\
			(ctx)->sAreaPrivOffset))



typedef struct
{
    int major;
    int minor;
    int patchlevel;
} ViaDRMVersion;

static int error_base;
static int event_base;
static unsigned numContexts = 0;
static int globalFD;
static struct _WsbmBufferPool *ttmPool;
static struct _WsbmFenceMgr *fenceMgr;
static drmAddress sAreaAddress;
static const ViaDRMVersion drmExpected = { 4, 0, 0 };
static const ViaDRMVersion drmCompat = { 4, 0, 0 };

#define FOURCC_XVMC (('C' << 24) + ('M' << 16) + ('V' << 8) + 'X')

#define ppthread_mutex_lock(arg)		\
  {						\
    pthread_mutex_lock(arg);			\
  }						\

#define ppthread_mutex_unlock(arg)		\
  {						\
    pthread_mutex_unlock(arg);			\
  }						\

static unsigned
yOffs(ViaXvMCSurface * srf)
{
    return 0;
}

static unsigned
vOffs(ViaXvMCSurface * srf)
{
    return srf->yStride * srf->height;
}

static unsigned
uOffs(ViaXvMCSurface * srf)
{
    return (srf->yStride * srf->height) +
	(srf->yStride >> 1) * (srf->height >> 1);
}

static void
defaultQMatrices(ViaXvMCContext * ctx)
{
    int i;

    static const char intra[64] = {
	8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83
    };

    for (i = 0; i < 64; ++i) {
	ctx->intra_quantiser_matrix[i] = intra[i];
	ctx->non_intra_quantiser_matrix[i] = 16;
    }
    ctx->intraLoaded = 0;
    ctx->nonIntraLoaded = 0;
}

static void
releaseDecoder(ViaXvMCContext * ctx, int clearCtx)
{
    volatile ViaXvMCSAreaPriv *sAPriv;

    sAPriv = SAREAPTR(ctx);
    UNICHROME_UNLOCK(ctx->fd, UNICHROME_LOCK_DECODER1, sAPriv,
	ctx->drmcontext);
}

static int
grabDecoder(ViaXvMCContext * ctx, int *hadLastLock)
{
    volatile ViaXvMCSAreaPriv *sAPriv = SAREAPTR(ctx);
    int retFtx, lc;

    /*
     * Try to grab the decoder. If it is not available we will sleep until
     * it becomes available or for a maximum of 20 ms. 
     * Then try to grab it again, unless a timeout occured. If the decoder is
     * available, the lock should be reasonably fast.
     */

    if (ctx->haveDecoder) {
	flushXvMCLowLevel(ctx->xl);    /* Ignore errors here. */

	/*fprintf(stderr,"ViaXvMC: ERROR: Trying to re-lock decoder.\n"); */
	*hadLastLock = 1;
	return 0;
    }
    UNICHROME_LOCK(ctx->fd, UNICHROME_LOCK_DECODER1, sAPriv, ctx->drmcontext,
	lc, retFtx);
    *hadLastLock = (ctx->drmcontext == lc);

    return retFtx;
}

static void
setupAttribDesc(Display * display, XvPortID port,
    const ViaXvMCAttrHolder * attrib, XvAttribute attribDesc[])
{
    XvAttribute *XvAttribs, *curAD;
    int num;
    unsigned i, j;

    XLockDisplay(display);
    XvAttribs = XvQueryPortAttributes(display, port, &num);
    for (i = 0; i < attrib->numAttr; ++i) {
	curAD = attribDesc + i;
	curAD->flags = 0;
	curAD->min_value = 0;
	curAD->max_value = 0;
	curAD->name = NULL;
	for (j = 0; j < num; ++j) {
	    if (attrib->attributes[i].attribute ==
		XInternAtom(display, XvAttribs[j].name, TRUE)) {
		*curAD = XvAttribs[j];
		curAD->name = strdup(XvAttribs[j].name);
		break;
	    }
	}
    }
    if (XvAttribs)
	XFree(XvAttribs);
    XUnlockDisplay(display);

}

static void
releaseAttribDesc(int numAttr, XvAttribute attribDesc[])
{
    int i;

    for (i = 0; i < numAttr; ++i) {
	if (attribDesc[i].name)
	    free(attribDesc[i].name);
    }
}

static Status
releaseContextResources(Display * display, XvMCContext * context,
    int freePrivate, Status errType)
{
    ViaXvMCContext *pViaXvMC = (ViaXvMCContext *) context->privData;

    switch (pViaXvMC->resources) {
    case context_drawHash:
	driDestroyHashContents(pViaXvMC->drawHash);
	drmHashDestroy(pViaXvMC->drawHash);
    case context_lowLevel:
	closeXvMCLowLevel(pViaXvMC->xl);
    case context_mutex:
	pthread_mutex_destroy(&pViaXvMC->ctxMutex);
    case context_drmContext:
	XLockDisplay(display);
	uniDRIDestroyContext(display, pViaXvMC->screen, pViaXvMC->id);
	XUnlockDisplay(display);
    case context_ttmPool:
	numContexts--;
	if (numContexts == 0) {
	    ttmPool->takeDown(ttmPool);
	    ttmPool = NULL;
	}
    case context_fenceMgr:
	if (numContexts == 0) {
	    wsbmFenceMgrTTMTakedown(fenceMgr);
	    fenceMgr = NULL;
	}
    case context_sAreaMap:
	if (numContexts == 0)
	    drmUnmap(pViaXvMC->sAreaAddress, pViaXvMC->sAreaSize);
    case context_fd:
	if (numContexts == 0) {
	    if (pViaXvMC->fd >= 0)
		drmClose(pViaXvMC->fd);
	}
	pViaXvMC->fd = -1;
    case context_driConnection:
	if (numContexts == 0) {
	    XLockDisplay(display);
	    uniDRICloseConnection(display, pViaXvMC->screen);
	    XUnlockDisplay(display);
	}
    case context_context:
	XLockDisplay(display);
	_xvmc_destroy_context(display, context);
	XUnlockDisplay(display);
	if (!freePrivate)
	    break;
    default:
	free(pViaXvMC);
	context->privData = NULL;
    }
    return errType;
}

Status
XvMCCreateContext(Display * display, XvPortID port,
    int surface_type_id, int width, int height, int flags,
    XvMCContext * context)
{
    ViaXvMCContext *pViaXvMC;
    int priv_count;
    uint *priv_data;
    uint magic;
    unsigned i;
    Status ret;
    int major, minor;
    ViaXvMCCreateContextRec *tmpComm;
    drmVersionPtr drmVer;
    char *curBusID;
    int isCapable;

    /* 
     * Verify Obvious things first 
     */

    if (context == NULL) {
	return XvMCBadContext;
    }

    if (!(flags & XVMC_DIRECT)) {
	fprintf(stderr, "Indirect Rendering not supported! Using Direct.\n");
    }

    /* 
     *FIXME: Check $DISPLAY for legal values here 
     */

    context->surface_type_id = surface_type_id;
    context->width = (unsigned short)((width + 15) & ~15);
    context->height = (unsigned short)((height + 15) & ~15);
    context->flags = flags;
    context->port = port;

    /* 
     *  Width, Height, and flags are checked against surface_type_id
     *  and port for validity inside the X server, no need to check
     *  here.
     */

    /* Allocate private Context data */
    context->privData = (void *)malloc(sizeof(ViaXvMCContext));
    if (!context->privData) {
	fprintf(stderr, "Unable to allocate resources for XvMC context.\n");
	return BadAlloc;
    }

    pViaXvMC = (ViaXvMCContext *) context->privData;
    pViaXvMC->resources = context_none;

    /* Verify the XvMC extension exists */

    XLockDisplay(display);
    if (!XvMCQueryExtension(display, &event_base, &error_base)) {
	fprintf(stderr, "XvMC Extension is not available!\n");
	free(pViaXvMC);
	XUnlockDisplay(display);
	return BadAlloc;
    }

    /* Verify XvMC version */
    ret = XvMCQueryVersion(display, &major, &minor);
    if (ret) {
	fprintf(stderr, "XvMCQuery Version Failed, unable to determine "
	    "protocol version!\n");
    }
    XUnlockDisplay(display);

    /* FIXME: Check Major and Minor here */

    XLockDisplay(display);
    if ((ret = _xvmc_create_context(display, context, &priv_count,
		&priv_data))) {
	XUnlockDisplay(display);
	fprintf(stderr, "Unable to create XvMC Context!\n");
	return releaseContextResources(display, context, 1, BadAlloc);
    }
    XUnlockDisplay(display);

    /*
     * Check size and version of returned data.
     */

    tmpComm = (ViaXvMCCreateContextRec *) priv_data;
    if (priv_count != (sizeof(ViaXvMCCreateContextRec) >> 2)) {
	fprintf(stderr, "_xvmc_create_context() returned incorrect "
	    "data size!\n");
	fprintf(stderr, "\tExpected %d, got %d\n",
	    ((int)(sizeof(ViaXvMCCreateContextRec) >> 2)), priv_count);
	XFree(priv_data);
	return releaseContextResources(display, context, 1, BadAlloc);
    }
    pViaXvMC->resources = context_context;

    if ((tmpComm->major != VIAXVMC_MAJOR) ||
	(tmpComm->minor != VIAXVMC_MINOR)) {
	fprintf(stderr, "Version mismatch between the X via driver\n"
	    "and the XvMC library. Cannot continue!\n");
	XFree(priv_data);
	return releaseContextResources(display, context, 1, BadAlloc);
    }

    pViaXvMC->ctxNo = tmpComm->ctxNo;
    pViaXvMC->sAreaSize = tmpComm->sAreaSize;
    pViaXvMC->sAreaPrivOffset = tmpComm->sAreaPrivOffset;
    pViaXvMC->decoderOn = 0;
    pViaXvMC->xvMCPort = tmpComm->xvmc_port;
    pViaXvMC->useAGP = tmpComm->useAGP;
    pViaXvMC->attrib = tmpComm->initAttrs;
    pViaXvMC->screen = tmpComm->screen;
    pViaXvMC->depth = tmpComm->depth;
    pViaXvMC->stride = tmpComm->stride;
    pViaXvMC->chipId = tmpComm->chipId;

    /* 
     * Must free the private data we were passed from X 
     */

    XFree(priv_data);
    priv_data = NULL;

    /*
     * Check for direct rendering capable, establish DRI and DRM connections,
     * map framebuffer, DRI shared area and read-only register areas. 
     * Initial checking for drm has already been done by the server. 
     * Only do this for the first context we create.
     */

    if (numContexts == 0) {
	union drm_via_extension_arg t_ext_arg;
	union drm_via_extension_arg f_ext_arg;
	const char ttm_ext[] = "via_ttm_placement_drop_080912";
	const char fence_ext[] = "via_ttm_fence_drop_080912";


	XLockDisplay(display);
	ret =
	    uniDRIQueryDirectRenderingCapable(display, pViaXvMC->screen,
	    &isCapable);
	if (!ret || !isCapable) {
	    XUnlockDisplay(display);
	    fprintf(stderr,
		"Direct Rendering is not available on this system!\n");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}

	if (!uniDRIOpenConnection(display, pViaXvMC->screen,
		&pViaXvMC->sAreaOffset, &curBusID)) {
	    XUnlockDisplay(display);
	    fprintf(stderr, "Could not open DRI connection to X server!\n");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}
	XUnlockDisplay(display);

	strncpy(pViaXvMC->busIdString, curBusID, 20);
	pViaXvMC->busIdString[20] = '\0';
	XFree(curBusID);

	pViaXvMC->resources = context_driConnection;

	if ((pViaXvMC->fd = drmOpen("via", pViaXvMC->busIdString)) < 0) {
	    fprintf(stderr, "DRM Device for via could not be opened.\n");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}
	globalFD = pViaXvMC->fd;
	pViaXvMC->resources = context_fd;

	if (NULL == (drmVer = drmGetVersion(pViaXvMC->fd))) {
	    fprintf(stderr, "viaXvMC: Could not get drm version.");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}
	if ((drmVer->version_major < drmExpected.major) ||
	    (drmVer->version_major > drmCompat.major) ||
	    ((drmVer->version_major == drmExpected.major) &&
		(drmVer->version_minor < drmExpected.minor))) {
	    fprintf(stderr,
		"viaXvMC: Kernel drm is not compatible with XvMC.\n");
	    fprintf(stderr,
		"viaXvMC: Kernel drm version: %d.%d.%d "
		"and I can work with versions %d.%d.x - %d.x.x\n"
		"Please update either this XvMC driver or your kernel DRM.\n",
		drmVer->version_major, drmVer->version_minor,
		drmVer->version_patchlevel, drmExpected.major,
		drmExpected.minor, drmCompat.major);
	    drmFreeVersion(drmVer);
	    return releaseContextResources(display, context, 1, BadAlloc);
	}
	drmFreeVersion(drmVer);
	
	strncpy(t_ext_arg.extension, ttm_ext, sizeof(t_ext_arg.extension));
	ret = drmCommandWriteRead(pViaXvMC->fd, DRM_VIA_EXTENSION, &t_ext_arg,
				  sizeof(t_ext_arg));
	if (ret != 0 || !t_ext_arg.rep.exists) {
	    fprintf(stderr, "Could not detect DRM extension \"%s\".\n",
		    ttm_ext);
	    releaseContextResources(display, context, 1, BadAlloc);
	}


	strncpy(f_ext_arg.extension, fence_ext, sizeof(f_ext_arg.extension));
	ret = drmCommandWriteRead(pViaXvMC->fd, DRM_VIA_EXTENSION, &f_ext_arg,
				  sizeof(f_ext_arg));
	if (ret != 0 || !f_ext_arg.rep.exists) {
	    fprintf(stderr, "Could not detect DRM extension \"%s\".\n",
		    ttm_ext);
	    releaseContextResources(display, context, 1, BadAlloc);
	}
	drmGetMagic(pViaXvMC->fd, &magic);

	XLockDisplay(display);
	if (!uniDRIAuthConnection(display, pViaXvMC->screen, magic)) {
	    XUnlockDisplay(display);
	    fprintf(stderr,
		"viaXvMC: X server did not allow DRI. Check permissions.\n");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}
	XUnlockDisplay(display);

	/*
	 * Map DRI Sarea.
	 */

	if (drmMap(pViaXvMC->fd, pViaXvMC->sAreaOffset,
		pViaXvMC->sAreaSize, &sAreaAddress) < 0) {
	    fprintf(stderr, "Unable to map DRI SAREA.\n");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}

	pViaXvMC->resources = context_sAreaMap;
	fenceMgr = wsbmFenceMgrTTMInit(pViaXvMC->fd, 5, 
				       f_ext_arg.rep.driver_ioctl_offset);
	if (fenceMgr == NULL) {
	    fprintf(stderr, "Could not create fence manager.\n");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}
	pViaXvMC->resources = context_fenceMgr;
	
	ttmPool = wsbmTTMPoolInit(pViaXvMC->fd,
				 t_ext_arg.rep.driver_ioctl_offset);
	if (ttmPool == NULL) {
	    fprintf(stderr, "Could not create TTM buffer pool.\n");
	    return releaseContextResources(display, context, 1, BadAlloc);
	}
	pViaXvMC->resources = context_ttmPool;

    } else {
	pViaXvMC->fd = globalFD;
    }

    pViaXvMC->sAreaAddress = sAreaAddress;
    pViaXvMC->ttmPool = ttmPool;
    pViaXvMC->fenceMgr = fenceMgr;
    pViaXvMC->resources = context_ttmPool;
    numContexts++;

    /*
     * Find a matching visual. Important only for direct drawing to the visible
     * frame-buffer.
     */

    XLockDisplay(display);
    ret = XMatchVisualInfo(display, pViaXvMC->screen,
	(pViaXvMC->depth == 32) ? 24 : pViaXvMC->depth, TrueColor,
	&pViaXvMC->visualInfo);
    XUnlockDisplay(display);
    if (!ret) {
	fprintf(stderr,
	    "viaXvMC: Could not find a matching TrueColor visual.\n");
	return releaseContextResources(display, context, 1, BadAlloc);
    }

    if (!uniDRICreateContext(display, pViaXvMC->screen,
	    pViaXvMC->visualInfo.visual, &pViaXvMC->id,
	    &pViaXvMC->drmcontext)) {

	fprintf(stderr, "viaXvMC: Could not create DRI context.\n");
	return releaseContextResources(display, context, 1, BadAlloc);
    }

    pViaXvMC->resources = context_drmContext;

    for (i = 0; i < VIA_MAX_RENDSURF; ++i) {
	pViaXvMC->rendSurf[i] = 0;
    }
    pViaXvMC->lastSrfDisplaying = ~0;
    setupAttribDesc(display, port, &pViaXvMC->attrib, pViaXvMC->attribDesc);

    pViaXvMC->hwLock = (drmLockPtr) pViaXvMC->sAreaAddress;
    defaultQMatrices(pViaXvMC);
    pViaXvMC->chromaIntraLoaded = 1;
    pViaXvMC->chromaNonIntraLoaded = 1;
    pViaXvMC->yStride = (width + 31) & ~31;
    pViaXvMC->haveDecoder = 0;
    pViaXvMC->attribChanged = 1;
    pViaXvMC->haveXv = 0;
    pViaXvMC->port = context->port;
    pthread_mutex_init(&pViaXvMC->ctxMutex, NULL);
    pViaXvMC->resources = context_mutex;
    setRegion(0, 0, -1, -1, pViaXvMC->sRegion);
    setRegion(0, 0, -1, -1, pViaXvMC->dRegion);

    if (NULL == (pViaXvMC->xl =
	    initXvMCLowLevel(pViaXvMC->fd, &pViaXvMC->drmcontext,
		pViaXvMC->hwLock, pViaXvMC->stride, pViaXvMC->depth,
		context->width, context->height,
		pViaXvMC->useAGP, pViaXvMC->chipId))) {

	fprintf(stderr, "ViaXvMC: Failed to initialize hardware.\n");
	return releaseContextResources(display, context, 1, BadAlloc);
    }
    pViaXvMC->resources = context_lowLevel;

    if (NULL == (pViaXvMC->drawHash = drmHashCreate())) {
	fprintf(stderr, "ViaXvMC: Could not allocate drawable hash table.\n");
	return releaseContextResources(display, context, 1, BadAlloc);
    }
    pViaXvMC->resources = context_drawHash;

    if (numContexts == 1) {
	hwlLock(pViaXvMC->xl, 1);
	setLowLevelLocking(pViaXvMC->xl, 0);
	viaVideoSubPictureOffLocked(pViaXvMC->xl);
	flushXvMCLowLevel(pViaXvMC->xl);
	setLowLevelLocking(pViaXvMC->xl, 1);
	hwlUnlock(pViaXvMC->xl, 1);
    }

    return Success;
}

Status
XvMCDestroyContext(Display * display, XvMCContext * context)
{
    ViaXvMCContext *pViaXvMC;

    if (context == NULL) {
	return (error_base + XvMCBadContext);
    }
    if (NULL == (pViaXvMC = context->privData)) {
	return (error_base + XvMCBadContext);
    }

    /*
     * Release decoder if we have it. In case of crash or termination
     * before XvMCDestroyContext, the X server will take care of this.
     */

    releaseAttribDesc(pViaXvMC->attrib.numAttr, pViaXvMC->attribDesc);
    releaseDecoder(pViaXvMC, 1);
    return releaseContextResources(display, context, 1, Success);
}

Status
XvMCCreateSurface(Display * display, XvMCContext * context,
    XvMCSurface * surface)
{
    ViaXvMCContext *pViaXvMC;
    ViaXvMCSurface *pViaSurface;
    int priv_count;
    unsigned *priv_data;
    Status ret;
    int wret;

    if ((surface == NULL) || (context == NULL) || (display == NULL)) {
	return BadValue;
    }

    pViaXvMC = (ViaXvMCContext *) context->privData;

    if (pViaXvMC == NULL) 
	return (error_base + XvMCBadContext);

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    pViaSurface = surface->privData =
	(ViaXvMCSurface *) malloc(sizeof(ViaXvMCSurface));
    if (!surface->privData) {
	ret = BadAlloc;
	goto out_err0;
    }

    XLockDisplay(display);
    ret = _xvmc_create_surface(display, context, surface,
			       &priv_count, &priv_data);
    XUnlockDisplay(display);

    if (ret != Success) {
	fprintf(stderr, "Unable to create XvMC Surface.\n");
	goto out_err1;
    }

    wret = wsbmGenBuffers(pViaXvMC->ttmPool, 1, &pViaSurface->buf,
			  0, WSBM_PL_FLAG_VRAM);
    if (wret != 0) {
	fprintf(stderr, "Unable to create surface buffer.\n");
	ret = BadAlloc;
	goto out_err2;
    }
    
    fprintf(stderr, "Referencing surface 0x%08x\n", priv_data[1]);
    wret = wsbmBOSetReferenced(pViaSurface->buf, priv_data[1]);
    if (wret != 0) {
	fprintf(stderr, "Unable to reference surface buffer.\n");
	ret = BadAlloc;
	goto out_err3;
    }

    pViaSurface->srfNo = priv_data[0];

    /* Free data returned from xvmc_create_surface */

    XFree(priv_data);

    pViaSurface->width = context->width;
    pViaSurface->height = context->height;
    pViaSurface->yStride = pViaXvMC->yStride;
    pViaSurface->privContext = pViaXvMC;
    pViaSurface->privSubPic = NULL;
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;

  out_err3:
    wsbmDeleteBuffers(1, &pViaSurface->buf);
  out_err2:
    XLockDisplay(display);
    _xvmc_destroy_surface(display, surface);
    XUnlockDisplay(display);
  out_err1:
    surface->privData = NULL;
    free(pViaSurface);
  out_err0:
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return ret;
}

Status
XvMCDestroySurface(Display * display, XvMCSurface * surface)
{
    ViaXvMCSurface *pViaSurface;

    if ((display == NULL) || (surface == NULL)) {
	return BadValue;
    }
    if (surface->privData == NULL) {
	return (error_base + XvMCBadSurface);
    }

    pViaSurface = (ViaXvMCSurface *) surface->privData;

    wsbmDeleteBuffers(1, &pViaSurface->buf);
    XLockDisplay(display);
    _xvmc_destroy_surface(display, surface);
    XUnlockDisplay(display);
    surface->privData = NULL;
    free(pViaSurface);

    return Success;
}

Status
XvMCPutSlice2(Display * display, XvMCContext * context, char *slice,
    int nBytes, int sliceCode)
{
    ViaXvMCContext *pViaXvMC;
    CARD32 sCode = 0x00010000 | (sliceCode & 0xFF) << 24;

    if ((display == NULL) || (context == NULL)) {
	return BadValue;
    }
    if (NULL == (pViaXvMC = context->privData)) {
	return (error_base + XvMCBadContext);
    }
    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    if (!pViaXvMC->haveDecoder) {
	fprintf(stderr, "XvMCPutSlice: This context does not own decoder!\n");
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return BadAlloc;
    }

    viaMpegWriteSlice(pViaXvMC->xl, (CARD8 *) slice, nBytes, sCode);

    viaFlushNotify(pViaXvMC->xl);
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}

Status
XvMCPutSlice(Display * display, XvMCContext * context, char *slice,
    int nBytes)
{
    ViaXvMCContext *pViaXvMC;

    if ((display == NULL) || (context == NULL)) {
	return BadValue;
    }
    if (NULL == (pViaXvMC = context->privData)) {
	return (error_base + XvMCBadContext);
    }
    ppthread_mutex_lock(&pViaXvMC->ctxMutex);

    if (!pViaXvMC->haveDecoder) {
	fprintf(stderr, "XvMCPutSlice: This context does not own decoder!\n");
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return BadAlloc;
    }

    viaMpegWriteSlice(pViaXvMC->xl, (CARD8 *) slice, nBytes, 0);
    viaFlushNotify(pViaXvMC->xl);
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}

static Status
updateXVOverlay(Display * display, ViaXvMCContext * pViaXvMC,
    ViaXvMCSurface * pViaSurface, Drawable draw,
    short srcx, short srcy, unsigned short srcw,
    unsigned short srch, short destx, short desty,
    unsigned short destw, unsigned short desth)
{
    ViaXvMCCommandBuffer buf;
    ViaXvMCSubPicture *pViaSubPic;
    Status ret;

    if (!pViaXvMC->haveXv) {
	pViaXvMC->xvImage =
	    XvCreateImage(display, pViaXvMC->port, FOURCC_XVMC,
	    (char *)&buf, pViaSurface->width, pViaSurface->height);
	pViaXvMC->gc = XCreateGC(display, draw, 0, 0);
	pViaXvMC->haveXv = 1;
    }
    pViaXvMC->draw = draw;
    pViaXvMC->xvImage->data = (char *)&buf;

    buf.command = (pViaXvMC->attribChanged) ?
	VIA_XVMC_COMMAND_FDISPLAY : VIA_XVMC_COMMAND_DISPLAY;
    buf.ctxNo = pViaXvMC->ctxNo | VIA_XVMC_VALID;
    buf.srfNo = pViaSurface->srfNo | VIA_XVMC_VALID;
    pViaSubPic = pViaSurface->privSubPic;
    buf.subPicNo = ((NULL == pViaSubPic) ? 0 : pViaSubPic->srfNo)
	| VIA_XVMC_VALID;
    buf.attrib = pViaXvMC->attrib;

    XLockDisplay(display);

    if ((ret = XvPutImage(display, pViaXvMC->port, draw, pViaXvMC->gc,
		pViaXvMC->xvImage, srcx, srcy, srcw, srch,
		destx, desty, destw, desth))) {
	XUnlockDisplay(display);
	return ret;
    }
    XSync(display, 0);
    XUnlockDisplay(display);
    pViaXvMC->attribChanged = 0;
    return Success;
}

Status
XvMCPutSurface(Display * display, XvMCSurface * surface, Drawable draw,
    short srcx, short srcy, unsigned short srcw,
    unsigned short srch, short destx, short desty,
    unsigned short destw, unsigned short desth, int flags)
{
    /*
     * This function contains some hairy locking logic. What we really want to
     * do is to flip the picture ASAP, to get a low latency and smooth playback.
     * However, if somebody else used the overlay since we used it last or if it is
     * our first time, we'll have to call X to update the overlay first. Otherwise 
     * we'll do the overlay update once we've flipped. Since we release the hardware
     * lock when we call X, X needs to verify using the SAREA that nobody else flipped
     * in a picture between the lock release and the X server control. Similarly
     * when the overlay update returns, we have to make sure that we still own the
     * overlay.
     */

    ViaXvMCSurface *pViaSurface;
    ViaXvMCContext *pViaXvMC;
    ViaXvMCSubPicture *pViaSubPic;
    volatile ViaXvMCSAreaPriv *sAPriv;
    Status ret;
    unsigned dispSurface, lastSurface;
    int overlayUpdated;
    drawableInfo *drawInfo;
    XvMCRegion sReg, dReg;
    Bool forceUpdate = FALSE;

    if ((display == NULL) || (surface == NULL)) {
	return BadValue;
    }
    if (NULL == (pViaSurface = surface->privData)) {
	return (error_base + XvMCBadSurface);
    }
    if (NULL == (pViaXvMC = pViaSurface->privContext)) {
	return (error_base + XvMCBadContext);
    }

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    pViaSubPic = pViaSurface->privSubPic;
    sAPriv = SAREAPTR(pViaXvMC);

    setRegion(srcx, srcy, srcw, srch, sReg);
    setRegion(destx, desty, destw, desth, dReg);

    if ((!regionEqual(sReg, pViaXvMC->sRegion)) ||
	(!regionEqual(dReg, pViaXvMC->dRegion))) {

	/*
	 * Force update of the video overlay to match the new format.
	 */

	pViaXvMC->sRegion = sReg;
	pViaXvMC->dRegion = dReg;
	forceUpdate = TRUE;
    }

    hwlLock(pViaXvMC->xl, 1);

    if (getDRIDrawableInfoLocked(pViaXvMC->drawHash, display,
	    pViaXvMC->screen, draw, 0, pViaXvMC->fd, pViaXvMC->drmcontext,
	    pViaXvMC->sAreaAddress, FALSE, &drawInfo, sizeof(*drawInfo))) {

	hwlUnlock(pViaXvMC->xl, 1);
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return BadAccess;
    }

    setLowLevelLocking(pViaXvMC->xl, 0);

    /*
     * Put a surface ID in the SAREA to "authenticate" to the 
     * X server.
     */

    dispSurface = sAPriv->XvMCDisplaying[pViaXvMC->xvMCPort];
    lastSurface = pViaXvMC->lastSrfDisplaying;
    sAPriv->XvMCDisplaying[pViaXvMC->xvMCPort] =
	pViaXvMC->lastSrfDisplaying = pViaSurface->srfNo | VIA_XVMC_VALID;
    overlayUpdated = 0;

    viaVideoSetSWFLipLocked(pViaXvMC->xl, 
			    pViaSurface->buf,
			    yOffs(pViaSurface),
			    uOffs(pViaSurface), 
			    vOffs(pViaSurface), pViaSurface->yStride,
			    pViaSurface->yStride >> 1);

    while ((lastSurface != dispSurface) || forceUpdate) {

	forceUpdate = FALSE;
	viaFlushNotify(pViaXvMC->xl);
	setLowLevelLocking(pViaXvMC->xl, 1);
	hwlUnlock(pViaXvMC->xl, 1);

	/*
	 * We weren't the last to display. Update the overlay before flipping.
	 */

	ret =
	    updateXVOverlay(display, pViaXvMC, pViaSurface, draw, srcx, srcy,
	    srcw, srch, destx, desty, destw, desth);
	if (ret) {
	    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	    return ret;
	}

	hwlLock(pViaXvMC->xl, 1);

	if (getDRIDrawableInfoLocked(pViaXvMC->drawHash, display,
		pViaXvMC->screen, draw, 0, pViaXvMC->fd, pViaXvMC->drmcontext,
		pViaXvMC->sAreaAddress, FALSE, &drawInfo,
		sizeof(*drawInfo))) {

	    hwlUnlock(pViaXvMC->xl, 1);
	    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	    return BadAccess;
	}

	setLowLevelLocking(pViaXvMC->xl, 0);
	lastSurface = pViaSurface->srfNo | VIA_XVMC_VALID;
	dispSurface = sAPriv->XvMCDisplaying[pViaXvMC->xvMCPort];
	overlayUpdated = 1;
    }

    /*
     * Subpictures
     */

    if (NULL != pViaSubPic) {
	if (sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort]
	    != (pViaSubPic->srfNo | VIA_XVMC_VALID)) {
	    sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] =
		pViaSubPic->srfNo | VIA_XVMC_VALID;
	    viaVideoSubPictureLocked(pViaXvMC->xl, pViaSubPic);
	}
    } else {
	if (sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] & VIA_XVMC_VALID) {
	    viaVideoSubPictureOffLocked(pViaXvMC->xl);
	    sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] &= ~VIA_XVMC_VALID;
	}
    }

    /*
     * Flip
     */

    viaVideoSWFlipLocked(pViaXvMC->xl, flags,
	pViaSurface->progressiveSequence);
    flushXvMCLowLevel(pViaXvMC->xl);

    setLowLevelLocking(pViaXvMC->xl, 1);
    hwlUnlock(pViaXvMC->xl, 1);

    if (overlayUpdated || !drawInfo->touched) {
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return Success;
    }

    /*
     * Update overlay
     */

    ret =
	updateXVOverlay(display, pViaXvMC, pViaSurface, draw, srcx, srcy,
	srcw, srch, destx, desty, destw, desth);
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return ret;

}

void
debugControl(const XvMCMpegControl * control)
{
    printf("BVMV_range: %u\n", control->BVMV_range);
    printf("BHMV_range: %u\n", control->BHMV_range);
    printf("FVMV_range: %u\n", control->FVMV_range);
    printf("FHMV_range: %u\n", control->FHMV_range);
    printf("picture_structure: %u\n", control->picture_structure);
    printf("intra_dc_precision: %u\n", control->intra_dc_precision);
    printf("picture_coding_type: %u\n", control->picture_coding_type);
    printf("mpeg_coding: %u\n", control->mpeg_coding);
    printf("flags: 0x%x\n", control->flags);
}

Status
XvMCBeginSurface(Display * display,
    XvMCContext * context,
    XvMCSurface * target_surface,
    XvMCSurface * past_surface,
    XvMCSurface * future_surface, const XvMCMpegControl * control)
{
    ViaXvMCSurface *targS, *futS, *pastS;
    ViaXvMCContext *pViaXvMC;
    int hadDecoderLast;

    if ((display == NULL) || (context == NULL) || (target_surface == NULL)) {
	return BadValue;
    }

    pViaXvMC = context->privData;

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    if (grabDecoder(pViaXvMC, &hadDecoderLast)) {
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return BadAlloc;
    }
    pViaXvMC->haveDecoder = 1;

    if (pViaXvMC->useAGP) {
	if (!hadDecoderLast) {
	    if (flushXvMCLowLevel(pViaXvMC->xl)) {
		releaseDecoder(pViaXvMC, 0);
		ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
		return BadAlloc;
	    }
	} 
    }

    if (!hadDecoderLast || !pViaXvMC->decoderOn) {
	pViaXvMC->intraLoaded = 0;
	pViaXvMC->nonIntraLoaded = 0;
    }
    viaMpegReset(pViaXvMC->xl);

    targS = (ViaXvMCSurface *) target_surface->privData;
    futS = NULL;
    pastS = NULL;

    pViaXvMC->rendSurf[0] = targS->srfNo | VIA_XVMC_VALID;
    if (future_surface) {
	futS = (ViaXvMCSurface *) future_surface->privData;
    }
    if (past_surface) {
	pastS = (ViaXvMCSurface *) past_surface->privData;
    }

    targS->progressiveSequence = (control->flags & XVMC_PROGRESSIVE_SEQUENCE);
    targS->topFieldFirst = (control->flags & XVMC_TOP_FIELD_FIRST);
    targS->privSubPic = NULL;

    viaMpegSetSurfaceStride(pViaXvMC->xl, pViaXvMC);

    viaMpegSetFB(pViaXvMC->xl, 0, targS->buf, yOffs(targS), uOffs(targS), vOffs(targS));
    if (past_surface) {
	viaMpegSetFB(pViaXvMC->xl, 1, pastS->buf, yOffs(pastS), uOffs(pastS),
	    vOffs(pastS));
    } else {
	viaMpegSetFB(pViaXvMC->xl, 1, targS->buf, 0, 0, 0);
    }

    if (future_surface) {
	viaMpegSetFB(pViaXvMC->xl, 2, futS->buf, yOffs(futS), uOffs(futS), vOffs(futS));
    } else {
	viaMpegSetFB(pViaXvMC->xl, 2, targS->buf, 0, 0, 0);
    }

    viaMpegBeginPicture(pViaXvMC->xl, pViaXvMC, context->width,
	context->height, control);
    viaFlushNotify(pViaXvMC->xl);
    pViaXvMC->decoderOn = 1;
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}

Status
XvMCSyncSurface(Display * display, XvMCSurface * surface)
{
    ViaXvMCSurface *pViaSurface;

    if ((display == NULL) || (surface == NULL)) {
	return BadValue;
    }
    if (surface->privData == NULL) {
	return (error_base + XvMCBadSurface);
    }

    pViaSurface = (ViaXvMCSurface *) surface->privData;
    wsbmBOWaitIdle(pViaSurface->buf, 1);

    return Success;
}

Status
XvMCLoadQMatrix(Display * display, XvMCContext * context,
    const XvMCQMatrix * qmx)
{
    ViaXvMCContext * pViaXvMC;

    if ((display == NULL) || (context == NULL)) {
	return BadValue;
    }
    if (NULL == (pViaXvMC = context->privData)) {
	return (error_base + XvMCBadContext);
    }

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    if (qmx->load_intra_quantiser_matrix) {
	memcpy(pViaXvMC->intra_quantiser_matrix,
	    qmx->intra_quantiser_matrix, sizeof(qmx->intra_quantiser_matrix));
	pViaXvMC->intraLoaded = 0;
    }

    if (qmx->load_non_intra_quantiser_matrix) {
	memcpy(pViaXvMC->non_intra_quantiser_matrix,
	    qmx->non_intra_quantiser_matrix,
	    sizeof(qmx->non_intra_quantiser_matrix));
	pViaXvMC->nonIntraLoaded = 0;
    }

    if (qmx->load_chroma_intra_quantiser_matrix) {
	memcpy(pViaXvMC->chroma_intra_quantiser_matrix,
	    qmx->chroma_intra_quantiser_matrix,
	    sizeof(qmx->chroma_intra_quantiser_matrix));
	pViaXvMC->chromaIntraLoaded = 0;
    }

    if (qmx->load_chroma_non_intra_quantiser_matrix) {
	memcpy(pViaXvMC->chroma_non_intra_quantiser_matrix,
	    qmx->chroma_non_intra_quantiser_matrix,
	    sizeof(qmx->chroma_non_intra_quantiser_matrix));
	pViaXvMC->chromaNonIntraLoaded = 0;
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);

    return Success;
}

/*
 * Below, we provide functions unusable for this implementation, but for
 * standard completeness.
 */

Status XvMCRenderSurface
    (Display * display,
    XvMCContext * context,
    unsigned int picture_structure,
    XvMCSurface * target_surface,
    XvMCSurface * past_surface,
    XvMCSurface * future_surface,
    unsigned int flags,
    unsigned int num_macroblocks,
    unsigned int first_macroblock,
    XvMCMacroBlockArray * macroblock_array, XvMCBlockArray * blocks)
{
    return (error_base + XvMCBadContext);
}

Status XvMCCreateBlocks
    (Display * display,
    XvMCContext * context, unsigned int num_blocks, XvMCBlockArray * block)
{
    return (error_base + XvMCBadContext);
}

Status
XvMCDestroyBlocks(Display * display, XvMCBlockArray * block)
{
    return Success;
}

Status XvMCCreateMacroBlocks
    (Display * display,
    XvMCContext * context,
    unsigned int num_blocks, XvMCMacroBlockArray * blocks)
{
    return (error_base + XvMCBadContext);
}

Status
XvMCDestroyMacroBlocks(Display * display, XvMCMacroBlockArray * block)
{
    return (error_base + XvMCBadContext);
}

Status
XvMCCreateSubpicture(Display * display,
    XvMCContext * context,
    XvMCSubpicture * subpicture,
    unsigned short width, unsigned short height, int xvimage_id)
{
    ViaXvMCContext *pViaXvMC;
    ViaXvMCSubPicture *pViaSubPic;
    int priv_count;
    unsigned *priv_data;
    Status ret;
    int wret;

    if ((subpicture == NULL) || (context == NULL) || (display == NULL)) {
	return BadValue;
    }

    pViaXvMC = (ViaXvMCContext *) context->privData;
    if (pViaXvMC == NULL) {
	return (error_base + XvMCBadContext);
    }

    subpicture->privData = (ViaXvMCSubPicture *)
	malloc(sizeof(ViaXvMCSubPicture));
    if (!subpicture->privData) 
	return BadAlloc;

    pViaSubPic = (ViaXvMCSubPicture *) subpicture->privData;
    ppthread_mutex_lock(&pViaXvMC->ctxMutex);

    subpicture->width = context->width;
    subpicture->height = context->height;
    subpicture->xvimage_id = xvimage_id;

    XLockDisplay(display);
    ret = _xvmc_create_subpicture(display, context, subpicture,
				  &priv_count, &priv_data);
    XUnlockDisplay(display);
    if (ret != Success) {
	fprintf(stderr, "Unable to create XvMC Subpicture.\n");
	goto out_err0;
    }

    wret = wsbmGenBuffers(pViaXvMC->ttmPool, 1, &pViaSubPic->buf,
			  0, WSBM_PL_FLAG_VRAM);
    if (wret != 0) {
	fprintf(stderr, "Unable to create surbpicture buffer.\n");
	ret = BadAlloc;
	goto out_err1;
    }
    
    fprintf(stderr, "Referencing subpic 0x%08x\n", priv_data[1]);
    wret = wsbmBOSetReferenced(pViaSubPic->buf, priv_data[1]);
    if (wret != 0) {
	fprintf(stderr, "Unable to reference subpicture buffer.\n");
	ret = BadAlloc;
	goto out_err2;
    }

    subpicture->num_palette_entries = VIA_SUBPIC_PALETTE_SIZE;
    subpicture->entry_bytes = 3;
    strncpy(subpicture->component_order, "YUV", 4);
    pViaSubPic->srfNo = priv_data[0];
    pViaSubPic->stride = (subpicture->width + 31) & ~31;
    pViaSubPic->privContext = pViaXvMC;
    pViaSubPic->ia44 = (xvimage_id == FOURCC_IA44);

    /* Free data returned from _xvmc_create_subpicture */

    XFree(priv_data);
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;

  out_err2:
    wsbmDeleteBuffers(1, &pViaSubPic->buf);
  out_err1:
    XLockDisplay(display);
    _xvmc_destroy_subpicture(display, subpicture);
    XUnlockDisplay(display);    
  out_err0:
    subpicture->privData = NULL;
    free(pViaSubPic);
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return ret;
}

Status
XvMCSetSubpicturePalette(Display * display, XvMCSubpicture * subpicture,
    unsigned char *palette)
{
    ViaXvMCSubPicture *pViaSubPic;
    ViaXvMCContext *pViaXvMC;
    volatile ViaXvMCSAreaPriv *sAPriv;
    unsigned i;
    CARD32 tmp;

    if ((subpicture == NULL) || (display == NULL)) {
	return BadValue;
    }
    if (subpicture->privData == NULL) {
	return (error_base + XvMCBadSubpicture);
    }
    pViaSubPic = (ViaXvMCSubPicture *) subpicture->privData;
    for (i = 0; i < VIA_SUBPIC_PALETTE_SIZE; ++i) {
	tmp = *palette++ << 8;
	tmp |= *palette++ << 16;
	tmp |= *palette++ << 24;
	tmp |= ((i & 0x0f) << 4) | 0x07;
	pViaSubPic->palette[i] = tmp;
    }

    pViaXvMC = pViaSubPic->privContext;
    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    sAPriv = SAREAPTR(pViaXvMC);
    hwlLock(pViaXvMC->xl, 1);
    setLowLevelLocking(pViaXvMC->xl, 0);

    /*
     * If the subpicture is displaying, Immeadiately update it with the
     * new palette.
     */

    if (sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] ==
	(pViaSubPic->srfNo | VIA_XVMC_VALID)) {
	viaVideoSubPictureLocked(pViaXvMC->xl, pViaSubPic);
    }
    viaFlushNotify(pViaXvMC->xl);
    setLowLevelLocking(pViaXvMC->xl, 1);
    hwlUnlock(pViaXvMC->xl, 1);
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}

static int
findOverlap(unsigned width, unsigned height,
    short *dstX, short *dstY,
    short *srcX, short *srcY, unsigned short *areaW, unsigned short *areaH)
{
    int w, h;
    unsigned mWidth, mHeight;

    w = *areaW;
    h = *areaH;

    if ((*dstX >= width) || (*dstY >= height))
	return 1;
    if (*dstX < 0) {
	w += *dstX;
	*srcX -= *dstX;
	*dstX = 0;
    }
    if (*dstY < 0) {
	h += *dstY;
	*srcY -= *dstY;
	*dstY = 0;
    }
    if ((w <= 0) || ((h <= 0)))
	return 1;
    mWidth = width - *dstX;
    mHeight = height - *dstY;
    *areaW = (w <= mWidth) ? w : mWidth;
    *areaH = (h <= mHeight) ? h : mHeight;
    return 0;
}

Status
XvMCClearSubpicture(Display * display,
    XvMCSubpicture * subpicture,
    short x,
    short y, unsigned short width, unsigned short height, unsigned int color)
{

    ViaXvMCContext *pViaXvMC;
    ViaXvMCSubPicture *pViaSubPic;
    short dummyX, dummyY;
    unsigned bOffs;

    if ((subpicture == NULL) || (display == NULL)) {
	return BadValue;
    }
    if (subpicture->privData == NULL) {
	return (error_base + XvMCBadSubpicture);
    }
    pViaSubPic = (ViaXvMCSubPicture *) subpicture->privData;
    pViaXvMC = pViaSubPic->privContext;
    ppthread_mutex_lock(&pViaXvMC->ctxMutex);

    /* Clip clearing area so that it fits inside subpicture. */

    if (findOverlap(subpicture->width, subpicture->height, &x, &y,
	    &dummyX, &dummyY, &width, &height)) {
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return Success;
    }

    bOffs = y * pViaSubPic->stride + x;
    viaBlit(pViaXvMC->xl, 8, pViaSubPic->buf, bOffs, pViaSubPic->stride, 
	    pViaSubPic->buf,  bOffs, pViaSubPic->stride,
	    width, height, 1, 1, VIABLIT_FILL, color);

    if (flushXvMCLowLevel(pViaXvMC->xl)) {
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return BadValue;
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}

Status
XvMCCompositeSubpicture(Display * display,
    XvMCSubpicture * subpicture,
    XvImage * image,
    short srcx,
    short srcy,
    unsigned short width, unsigned short height, short dstx, short dsty)
{

    unsigned i;
    ViaXvMCContext *pViaXvMC;
    ViaXvMCSubPicture *pViaSubPic;
    CARD8 *dAddr, *sAddr;
    CARD8 *dMap;
    Status ret = Success;
    int wret;

    if ((subpicture == NULL) || (display == NULL) || (image == NULL)) {
	return BadValue;
    }
    if (NULL == (pViaSubPic = (ViaXvMCSubPicture *) subpicture->privData)) {
	return (error_base + XvMCBadSubpicture);
    }

    pViaXvMC = pViaSubPic->privContext;

    if (image->id != subpicture->xvimage_id)
	return BadMatch;

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);

    /*
     * Clip copy area so that it fits inside subpicture and image.
     */

    if (findOverlap(subpicture->width, subpicture->height,
	    &dstx, &dsty, &srcx, &srcy, &width, &height)) {
	goto out_unlock;
    }
    if (findOverlap(image->width, image->height,
	    &srcx, &srcy, &dstx, &dsty, &width, &height)) {
	goto out_unlock;
    }

    dMap = wsbmBOMap(pViaSubPic->buf, WSBM_ACCESS_WRITE);

    /*
     * FIXME: Move to create.
     */

    if (dMap == NULL) {
	ret = BadAlloc;
	goto out_unlock;
    }

    wret = wsbmBOSyncForCpu(pViaSubPic->buf, WSBM_SYNCCPU_WRITE);

    if (wret) {
	ret = BadAlloc;
	goto out_unlock;
    }

    for (i = 0; i < height; ++i) {
	dAddr = dMap + ((dsty + i) * pViaSubPic->stride + dstx);
	sAddr = (((CARD8 *) image->data) +
	    (image->offsets[0] + (srcy + i) * image->pitches[0] + srcx));
	memcpy(dAddr, sAddr, width);
    }

    (void) wsbmBOReleaseFromCpu(pViaSubPic->buf, WSBM_SYNCCPU_WRITE);
    (void) wsbmBOUnmap(pViaSubPic->buf);

    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
  out_unlock:
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return ret;
    
}

Status
XvMCBlendSubpicture(Display * display,
    XvMCSurface * target_surface,
    XvMCSubpicture * subpicture,
    short subx,
    short suby,
    unsigned short subw,
    unsigned short subh,
    short surfx, short surfy, unsigned short surfw, unsigned short surfh)
{
    ViaXvMCSurface *pViaSurface;
    ViaXvMCSubPicture *pViaSubPic;

    if ((display == NULL) || target_surface == NULL) {
	return BadValue;
    }

    if (subx || suby || surfx || surfy || (subw != surfw) || (subh != surfh)) {
	fprintf(stderr, "ViaXvMC: Only completely overlapping subpicture "
	    "supported.\n");
	return BadValue;
    }

    if (NULL == (pViaSurface = target_surface->privData)) {
	return (error_base + XvMCBadSurface);
    }

    if (subpicture) {

	if (NULL == (pViaSubPic = subpicture->privData)) {
	    return (error_base + XvMCBadSubpicture);
	}

	pViaSurface->privSubPic = pViaSubPic;
    } else {
	pViaSurface->privSubPic = NULL;
    }
    return Success;
}

Status
XvMCBlendSubpicture2(Display * display,
		     XvMCSurface * source_surface,
		     XvMCSurface * target_surface,
		     XvMCSubpicture * subpicture,
		     short subx,
		     short suby,
		     unsigned short subw,
		     unsigned short subh,
    short surfx, short surfy, unsigned short surfw, unsigned short surfh)
{
    ViaXvMCSurface *pViaSurface, *pViaSSurface;
    ViaXvMCSubPicture *pViaSubPic;
    ViaXvMCContext *pViaXvMC;

    unsigned width, height;

    if ((display == NULL) || target_surface == NULL || source_surface == NULL) {
	return BadValue;
    }

    if (subx || suby || surfx || surfy || (subw != surfw) || (subh != surfh)) {
	fprintf(stderr, "ViaXvMC: Only completely overlapping subpicture "
	    "supported.\n");
	return BadMatch;
    }

    if (NULL == (pViaSurface = target_surface->privData)) {
	return (error_base + XvMCBadSurface);
    }

    if (NULL == (pViaSSurface = source_surface->privData)) {
	return (error_base + XvMCBadSurface);
    }
    pViaXvMC = pViaSurface->privContext;
    width = pViaSSurface->width;
    height = pViaSSurface->height;
    if (width != pViaSurface->width || height != pViaSSurface->height) {
	return BadMatch;
    }

    if (XvMCSyncSurface(display, source_surface)) {
	return BadValue;
    }

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    viaBlit(pViaXvMC->xl, 8, pViaSSurface->buf, yOffs(pViaSSurface), 
	    pViaSSurface->yStride,
	    pViaSurface->buf, 
	    yOffs(pViaSurface), pViaSurface->yStride,
	    width, height, 1, 1, VIABLIT_COPY, 0);
    viaFlushNotify(pViaXvMC->xl);
    if (pViaXvMC->chipId != PCI_CHIP_VT3259) {

	/*
	 * YV12 Chroma blit.
	 */

	viaBlit(pViaXvMC->xl, 8, pViaSSurface->buf,
		uOffs(pViaSSurface),
		pViaSSurface->yStride >> 1, 
		pViaSurface->buf,
		uOffs(pViaSurface),
		pViaSurface->yStride >> 1, width >> 1, height >> 1, 1, 1,
		VIABLIT_COPY, 0);
	viaFlushNotify(pViaXvMC->xl);
	viaBlit(pViaXvMC->xl, 8, pViaSSurface->buf,
		vOffs(pViaSSurface),
		pViaSSurface->yStride >> 1, 
		pViaSurface->buf, 
		vOffs(pViaSurface),
		pViaSurface->yStride >> 1, width >> 1, height >> 1, 1, 1,
		VIABLIT_COPY, 0);
    } else {

	/*
	 * NV12 Chroma blit.
	 */

	viaBlit(pViaXvMC->xl, 8, pViaSSurface->buf, 
		vOffs(pViaSSurface), pViaSSurface->yStride,
		pViaSurface->buf,
		vOffs(pViaSurface), pViaSurface->yStride,
		width, height >> 1, 1, 1, VIABLIT_COPY, 0);
    }
    if (flushXvMCLowLevel(pViaXvMC->xl)) {
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return BadValue;
    }
    if (subpicture) {

	if (NULL == (pViaSubPic = subpicture->privData)) {
	    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	    return (error_base + XvMCBadSubpicture);
	}

	pViaSurface->privSubPic = pViaSubPic;
    } else {
	pViaSurface->privSubPic = NULL;
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}

Status
XvMCSyncSubpicture(Display * display, XvMCSubpicture * subpicture)
{
    ViaXvMCSubPicture *pViaSubPic;

    if ((display == NULL) || subpicture == NULL) {
	return BadValue;
    }
    if (NULL == (pViaSubPic = subpicture->privData)) {
	return (error_base + XvMCBadSubpicture);
    }

    wsbmBOWaitIdle(pViaSubPic->buf, 1);
    return Success;
}

Status
XvMCFlushSubpicture(Display * display, XvMCSubpicture * subpicture)
{
    ViaXvMCSubPicture *pViaSubPic;

    if ((display == NULL) || subpicture == NULL) {
	return BadValue;
    }
    if (NULL == (pViaSubPic = subpicture->privData)) {
	return (error_base + XvMCBadSubpicture);
    }

    return Success;
}

Status
XvMCDestroySubpicture(Display * display, XvMCSubpicture * subpicture)
{
    ViaXvMCSubPicture *pViaSubPic;
    ViaXvMCContext *pViaXvMC;
    volatile ViaXvMCSAreaPriv *sAPriv;

    if ((display == NULL) || subpicture == NULL) {
	return BadValue;
    }
    if (NULL == (pViaSubPic = subpicture->privData)) {
	return (error_base + XvMCBadSubpicture);
    }
    pViaXvMC = pViaSubPic->privContext;
    ppthread_mutex_lock(&pViaXvMC->ctxMutex);

    sAPriv = SAREAPTR(pViaXvMC);
    hwlLock(pViaXvMC->xl, 1);
    setLowLevelLocking(pViaXvMC->xl, 0);
    if (sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] ==
	(pViaSubPic->srfNo | VIA_XVMC_VALID)) {
	viaVideoSubPictureOffLocked(pViaXvMC->xl);
	sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] = 0;
    }
    viaFlushNotify(pViaXvMC->xl);
    setLowLevelLocking(pViaXvMC->xl, 1);
    hwlUnlock(pViaXvMC->xl, 1);

    wsbmDeleteBuffers(1, &pViaSubPic->buf);

    XLockDisplay(display);
    _xvmc_destroy_subpicture(display, subpicture);
    XUnlockDisplay(display);

    subpicture->privData = NULL;
    free(pViaSubPic);
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);

    return Success;
}

Status
XvMCGetSubpictureStatus(Display * display, XvMCSubpicture * subpic, int *stat)
{
    ViaXvMCSubPicture *pViaSubPic;
    ViaXvMCContext *pViaXvMC;
    volatile ViaXvMCSAreaPriv *sAPriv;

    if ((display == NULL) || subpic == NULL) {
	return BadValue;
    }
    if (NULL == (pViaSubPic = subpic->privData)) {
	return (error_base + XvMCBadSubpicture);
    }
    if (stat) {
	*stat = 0;
	pViaXvMC = pViaSubPic->privContext;
	sAPriv = SAREAPTR(pViaXvMC);
	if (sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] ==
	    (pViaSubPic->srfNo | VIA_XVMC_VALID))
	    *stat |= XVMC_DISPLAYING;
    }
    return Success;
}

Status
XvMCFlushSurface(Display * display, XvMCSurface * surface)
{
    ViaXvMCSurface *pViaSurface;
    ViaXvMCContext *pViaXvMC;
    Status ret;

    if ((display == NULL) || surface == NULL) {
	return BadValue;
    }
    if (NULL == (pViaSurface = surface->privData)) {
	return (error_base + XvMCBadSurface);
    }

    pViaXvMC = pViaSurface->privContext;
    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    ret = (flushXvMCLowLevel(pViaXvMC->xl)) ? BadValue : Success;
    if (pViaXvMC->rendSurf[0] == (pViaSurface->srfNo | VIA_XVMC_VALID)) {
	hwlLock(pViaXvMC->xl, 0);
	pViaXvMC->haveDecoder = 0;
	releaseDecoder(pViaXvMC, 0);
	hwlUnlock(pViaXvMC->xl, 0);
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return ret;
}

Status
XvMCGetSurfaceStatus(Display * display, XvMCSurface * surface, int *stat)
{
    ViaXvMCSurface *pViaSurface;
    ViaXvMCContext *pViaXvMC;
    volatile ViaXvMCSAreaPriv *sAPriv;
    unsigned i;
    int ret = 0;

    if ((display == NULL) || surface == NULL) {
	return BadValue;
    }
    if (NULL == (pViaSurface = surface->privData)) {
	return (error_base + XvMCBadSurface);
    }
    if (stat) {
	*stat = 0;
	pViaXvMC = pViaSurface->privContext;
	ppthread_mutex_lock(&pViaXvMC->ctxMutex);
	sAPriv = SAREAPTR(pViaXvMC);
	if (sAPriv->XvMCDisplaying[pViaXvMC->xvMCPort]
	    == (pViaSurface->srfNo | VIA_XVMC_VALID))
	    *stat |= XVMC_DISPLAYING;
	for (i = 0; i < VIA_MAX_RENDSURF; ++i) {
	    if (pViaXvMC->rendSurf[i] ==
		(pViaSurface->srfNo | VIA_XVMC_VALID)) {
		*stat |= XVMC_RENDERING;
		break;
	    }
	}
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    }
    return ret;
}

XvAttribute *
XvMCQueryAttributes(Display * display, XvMCContext * context, int *number)
{
    ViaXvMCContext *pViaXvMC;
    XvAttribute *ret;
    unsigned long siz;

    *number = 0;
    if ((display == NULL) || (context == NULL)) {
	return NULL;
    }

    if (NULL == (pViaXvMC = context->privData)) {
	return NULL;
    }

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    if (NULL != (ret = (XvAttribute *)
	    malloc(siz = VIA_NUM_XVMC_ATTRIBUTES * sizeof(XvAttribute)))) {
	memcpy(ret, pViaXvMC->attribDesc, siz);
	*number = VIA_NUM_XVMC_ATTRIBUTES;
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);

    return ret;
}

Status
XvMCSetAttribute(Display * display,
    XvMCContext * context, Atom attribute, int value)
{
    int found;
    unsigned i;
    ViaXvMCContext *pViaXvMC;
    ViaXvMCCommandBuffer buf;

    if ((display == NULL) || (context == NULL)) {
	return (error_base + XvMCBadContext);
    }

    if (NULL == (pViaXvMC = context->privData)) {
	return (error_base + XvMCBadContext);
    }

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);

    found = 0;
    for (i = 0; i < pViaXvMC->attrib.numAttr; ++i) {
	if (attribute == pViaXvMC->attrib.attributes[i].attribute) {
	    if ((!(pViaXvMC->attribDesc[i].flags & XvSettable)) ||
		value < pViaXvMC->attribDesc[i].min_value ||
		value > pViaXvMC->attribDesc[i].max_value) {
		ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
		return BadValue;
	    }
	    pViaXvMC->attrib.attributes[i].value = value;
	    found = 1;
	    pViaXvMC->attribChanged = 1;
	    break;
	}
    }
    if (!found) {
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return BadMatch;
    }
    if (pViaXvMC->haveXv) {
	buf.command = VIA_XVMC_COMMAND_ATTRIBUTES;
	pViaXvMC->xvImage->data = (char *)&buf;
	buf.ctxNo = pViaXvMC->ctxNo | VIA_XVMC_VALID;
	buf.attrib = pViaXvMC->attrib;
	XLockDisplay(display);
	pViaXvMC->attribChanged =
	    XvPutImage(display, pViaXvMC->port, pViaXvMC->draw,
	    pViaXvMC->gc, pViaXvMC->xvImage, 0, 0, 1, 1, 0, 0, 1, 1);
	XUnlockDisplay(display);
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}

Status
XvMCGetAttribute(Display * display,
    XvMCContext * context, Atom attribute, int *value)
{
    int found;
    unsigned i;
    ViaXvMCContext *pViaXvMC;

    if ((display == NULL) || (context == NULL)) {
	return (error_base + XvMCBadContext);
    }

    if (NULL == (pViaXvMC = context->privData)) {
	return (error_base + XvMCBadContext);
    }

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    found = 0;
    for (i = 0; i < pViaXvMC->attrib.numAttr; ++i) {
	if (attribute == pViaXvMC->attrib.attributes[i].attribute) {
	    if (pViaXvMC->attribDesc[i].flags & XvGettable) {
		*value = pViaXvMC->attrib.attributes[i].value;
		found = 1;
		break;
	    }
	}
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);

    if (!found)
	return BadMatch;
    return Success;
}

Status
XvMCHideSurface(Display * display, XvMCSurface * surface)
{

    ViaXvMCSurface *pViaSurface;
    ViaXvMCContext *pViaXvMC;
    ViaXvMCSubPicture *pViaSubPic;
    volatile ViaXvMCSAreaPriv *sAPriv;
    ViaXvMCCommandBuffer buf;
    Status ret;

    if ((display == NULL) || (surface == NULL)) {
	return BadValue;
    }
    if (NULL == (pViaSurface = surface->privData)) {
	return (error_base + XvMCBadSurface);
    }
    if (NULL == (pViaXvMC = pViaSurface->privContext)) {
	return (error_base + XvMCBadContext);
    }

    ppthread_mutex_lock(&pViaXvMC->ctxMutex);
    if (!pViaXvMC->haveXv) {
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return Success;
    }

    sAPriv = SAREAPTR(pViaXvMC);
    hwlLock(pViaXvMC->xl, 1);

    if (sAPriv->XvMCDisplaying[pViaXvMC->xvMCPort] !=
	(pViaSurface->srfNo | VIA_XVMC_VALID)) {
	hwlUnlock(pViaXvMC->xl, 1);
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return Success;
    }
    setLowLevelLocking(pViaXvMC->xl, 0);
    if (NULL != (pViaSubPic = pViaSurface->privSubPic)) {
	if (sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] ==
	    (pViaSubPic->srfNo | VIA_XVMC_VALID)) {
	    sAPriv->XvMCSubPicOn[pViaXvMC->xvMCPort] &= ~VIA_XVMC_VALID;
	    viaVideoSubPictureOffLocked(pViaXvMC->xl);
	}
    }
    viaFlushNotify(pViaXvMC->xl);
    setLowLevelLocking(pViaXvMC->xl, 1);
    hwlUnlock(pViaXvMC->xl, 1);

    buf.command = VIA_XVMC_COMMAND_UNDISPLAY;
    buf.ctxNo = pViaXvMC->ctxNo | VIA_XVMC_VALID;
    buf.srfNo = pViaSurface->srfNo | VIA_XVMC_VALID;
    pViaXvMC->xvImage->data = (char *)&buf;
    if ((ret = XvPutImage(display, pViaXvMC->port, pViaXvMC->draw,
		pViaXvMC->gc, pViaXvMC->xvImage, 0, 0, 1, 1, 0, 0, 1, 1))) {
	fprintf(stderr, "XvMCPutSurface: Hiding overlay failed.\n");
	ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
	return ret;
    }
    ppthread_mutex_unlock(&pViaXvMC->ctxMutex);
    return Success;
}
