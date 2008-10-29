/*
 * Copyright 1998-2003 VIA Technologies, Inc. All Rights Reserved.
 * Copyright 2001-2003 S3 Graphics, Inc. All Rights Reserved.
 * Copyright 2006 Thomas Hellström. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * 2D acceleration functions for the VIA/S3G UniChrome IGPs.
 *
 * Mostly rewritten, and modified for EXA support, by Thomas Hellström.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ws_dri_bufmgr.h>
#include <X11/Xarch.h>
#include "xaalocal.h"
#include "xaarop.h"
#include "miline.h"

#include "via.h"
#include "via_driver.h"
#include "via_regs.h"
#include "via_id.h"
#include "via_dmabuffer.h"

#ifdef X_HAVE_XAAGETROP
#define VIAACCELPATTERNROP(vRop) (XAAGetPatternROP(vRop) << 24)
#define VIAACCELCOPYROP(vRop) (XAAGetCopyROP(vRop) << 24)
#else
#define VIAACCELPATTERNROP(vRop) (XAAPatternROP[vRop] << 24)
#define VIAACCELCOPYROP(vRop) (XAACopyROP[vRop] << 24)
#endif

#undef VIA_DEBUG_COMPOSITE

/*
 * Use PCI MMIO to flush the command buffer when AGP DMA is not available.
 */
static void
viaDumpDMA(ViaCommandBuffer * buf)
{
    register CARD32 *bp = buf->buf;
    CARD32 *endp = bp + buf->pos;

    while (bp != endp) {
        if (((bp - buf->buf) & 3) == 0) {
            ErrorF("\n %04lx: ", (unsigned long)(bp - buf->buf));
        }
        ErrorF("0x%08x ", (unsigned)*bp++);
    }
    ErrorF("\n");
}

void
viaFlushPCI(ViaCommandBuffer * buf)
{
    register CARD32 *bp = buf->buf;
    CARD32 transSetting;
    CARD32 *endp = bp + buf->pos;
    unsigned loop = 0;
    register CARD32 offset = 0;
    register CARD32 value;
    VIAPtr pVia = VIAPTR(buf->pScrn);

    while (bp < endp) {
        if (*bp == HALCYON_HEADER2) {
            if (++bp == endp)
                return;
            VIASETREG(VIA_REG_TRANSET, transSetting = *bp++);
            while (bp < endp) {
                if ((transSetting != HC_ParaType_CmdVdata)
                    && ((*bp == HALCYON_HEADER2)
                        || (*bp & HALCYON_HEADER1MASK) == HALCYON_HEADER1))
                    break;
                VIASETREG(VIA_REG_TRANSPACE, *bp++);
            }
        } else if ((*bp & HALCYON_HEADER1MASK) == HALCYON_HEADER1) {

            while (bp < endp) {
                if (*bp == HALCYON_HEADER2)
                    break;
                if (offset == 0) {
                    /*
                     * Not doing this wait will probably stall the processor
                     * for an unacceptable amount of time in VIASETREG while
                     * other high priority interrupts may be pending.
                     */
                    if (pVia->Chipset != VIA_P4M890 &&
                        pVia->Chipset != VIA_K8M890 &&
                        pVia->Chipset != VIA_P4M900) {
                        while (!(VIAGETREG(VIA_REG_STATUS) & VIA_VR_QUEUE_BUSY)
                               && (loop++ < MAXLOOP)) ;
                    }
                    while ((VIAGETREG(VIA_REG_STATUS) &
                            (VIA_CMD_RGTR_BUSY | VIA_2D_ENG_BUSY))
                           && (loop++ < MAXLOOP)) ;
                }
                offset = (*bp++ & 0x0FFFFFFF) << 2;
                value = *bp++;
                VIASETREG(offset, value);
            }
        } else {
            ErrorF("Command stream parser error.\n");
        }
    }
    buf->pos = 0;
    buf->mode = 0;
    buf->has3dState = FALSE;
}

#ifdef XF86DRI
/*
 * Flush the command buffer using DRM. If in PCI mode, we can bypass DRM,
 * but not for command buffers that contain 3D engine state, since then
 * the DRM command verifier will lose track of the 3D engine state.
 */
static void
viaFlushDRIEnabled(ViaCommandBuffer * cb)
{
    ScrnInfoPtr pScrn = cb->pScrn;
    VIAPtr pVia = VIAPTR(pScrn);
    char *tmp = (char *)cb->buf;
    int tmpSize;
    struct drm_via_cmdbuffer b;

    /* Align end of command buffer for AGP DMA. */
    if (pVia->agpDMA && cb->mode == 2 && cb->rindex != HC_ParaType_CmdVdata
        && (cb->pos & 1)) {
        OUT_RING(HC_DUMMY);
    }

    tmpSize = cb->pos * sizeof(CARD32);
    if (pVia->agpDMA || (pVia->directRenderingEnabled && cb->has3dState)) {
        cb->mode = 0;
        cb->has3dState = FALSE;
        while (tmpSize > 0) {
            b.size = (tmpSize > VIA_DMASIZE) ? VIA_DMASIZE : tmpSize;
            tmpSize -= b.size;
            b.buf = tmp;
            tmp += b.size;
            if (drmCommandWrite(pVia->drmFD, ((pVia->agpDMA)
                                              ? DRM_VIA_CMDBUFFER :
                                              DRM_VIA_PCICMD), &b, sizeof(b))) {
                ErrorF("DRM command buffer submission failed.\n");
                viaDumpDMA(cb);
                return;
            }
        }
        cb->pos = 0;
    } else {
        viaFlushPCI(cb);
    }
}
#endif

/*
 * Initialize a command buffer. Some fields are currently not used since they
 * are intended for Unichrome Pro group A video commands.
 */
int
viaSetupCBuffer(ScrnInfoPtr pScrn, ViaCommandBuffer * buf, unsigned size)
{
#ifdef XF86DRI
    VIAPtr pVia = VIAPTR(pScrn);
#endif

    buf->pScrn = pScrn;
    buf->bufSize = ((size == 0) ? VIA_DMASIZE : size) >> 2;
    buf->buf = (CARD32 *) xcalloc(buf->bufSize, sizeof(CARD32));
    if (!buf->buf)
        return BadAlloc;
    buf->waitFlags = 0;
    buf->pos = 0;
    buf->mode = 0;
    buf->header_start = 0;
    buf->rindex = 0;
    buf->has3dState = FALSE;
    buf->flushFunc = viaFlushPCI;
#ifdef XF86DRI
    if (pVia->directRenderingEnabled) {
        buf->flushFunc = viaFlushDRIEnabled;
    }
#endif
    return Success;
}

/*
 * Free resources associated with a command buffer.
 */
void
viaTearDownCBuffer(ViaCommandBuffer * buf)
{
    if (buf && buf->buf)
        xfree(buf->buf);
    buf->buf = NULL;
}

/*
 * Update our 2D state (TwoDContext) with a new mode.
 */
static Bool
viaAccelSetMode(int bpp, ViaTwodContext * tdc)
{
    switch (bpp) {
        case 16:
            tdc->mode = VIA_GEM_16bpp;
            tdc->bytesPPShift = 1;
            return TRUE;
        case 32:
            tdc->mode = VIA_GEM_32bpp;
            tdc->bytesPPShift = 2;
            return TRUE;
        case 8:
            tdc->mode = VIA_GEM_8bpp;
            tdc->bytesPPShift = 0;
            return TRUE;
        default:
            tdc->bytesPPShift = 0;
            return FALSE;
    }
}

/*
 * Initialize the 2D engine and set the 2D context mode to the
 * current screen depth. Also enable the virtual queue. 
 */
void
viaInitialize2DEngine(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    viaAccelSetMode(pScrn->bitsPerPixel, tdc);
}

/*
 * Wait for acceleration engines idle. An expensive way to sync.
 */
void
viaAccelSync(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    int loop = 0;

    mem_barrier();

    switch (pVia->Chipset) {
        case VIA_P4M890:
        case VIA_K8M890:
        case VIA_P4M900:
            while ((VIAGETREG(VIA_REG_STATUS) &
                    (VIA_CMD_RGTR_BUSY | VIA_2D_ENG_BUSY))
                   && (loop++ < MAXLOOP)) ;
            break;
        default:
            while (!(VIAGETREG(VIA_REG_STATUS) & VIA_VR_QUEUE_BUSY)
                   && (loop++ < MAXLOOP)) ;

            while ((VIAGETREG(VIA_REG_STATUS) &
                    (VIA_CMD_RGTR_BUSY | VIA_2D_ENG_BUSY | VIA_3D_ENG_BUSY))
                   && (loop++ < MAXLOOP)) ;
            break;
    }
}

/*
 * Emit a solid blit operation to the command buffer. 
 */
static void
viaAccelSolidHelper(ViaCommandBuffer * cb, int x, int y, int w, int h,
                    unsigned fbBase, CARD32 mode, unsigned pitch,
                    CARD32 fg, CARD32 cmd)
{
    BEGIN_RING(14);
    OUT_RING_H1(VIA_REG_GEMODE, mode);
    OUT_RING_H1(VIA_REG_DSTBASE, fbBase >> 3);
    OUT_RING_H1(VIA_REG_PITCH, VIA_PITCH_ENABLE | (pitch >> 3) << 16);
    OUT_RING_H1(VIA_REG_DSTPOS, (y << 16) | (x & 0xFFFF));
    OUT_RING_H1(VIA_REG_DIMENSION, ((h - 1) << 16) | (w - 1));
    OUT_RING_H1(VIA_REG_FGCOLOR, fg);
    OUT_RING_H1(VIA_REG_GECMD, cmd);
    ADVANCE_RING;
}

/*
 * Check if we can use a planeMask and update the 2D context accordingly.
 */
static Bool
viaAccelPlaneMaskHelper(ViaTwodContext * tdc, CARD32 planeMask)
{
    CARD32 modeMask = (1 << ((1 << tdc->bytesPPShift) << 3)) - 1;
    CARD32 curMask = 0x00000000;
    CARD32 curByteMask;
    int i;

    if ((planeMask & modeMask) != modeMask) {

        /* Masking doesn't work in 8bpp. */
        if (modeMask == 0xFF) {
            tdc->keyControl &= 0x0FFFFFFF;
            return FALSE;
        }

        /* Translate the bit planemask to a byte planemask. */
        for (i = 0; i < (1 << tdc->bytesPPShift); ++i) {
            curByteMask = (0xFF << (i << 3));

            if ((planeMask & curByteMask) == 0) {
                curMask |= (1 << i);
            } else if ((planeMask & curByteMask) != curByteMask) {
                tdc->keyControl &= 0x0FFFFFFF;
                return FALSE;
            }
        }
        ErrorF("DEBUG: planeMask 0x%08x, curMask 0%02x\n",
               (unsigned)planeMask, (unsigned)curMask);

        tdc->keyControl = (tdc->keyControl & 0x0FFFFFFF) | (curMask << 28);
    }

    return TRUE;
}

/*
 * Emit transparency state and color to the command buffer.
 */
static void
viaAccelTransparentHelper(ViaTwodContext * tdc, ViaCommandBuffer * cb,
                          CARD32 keyControl, CARD32 transColor,
                          Bool usePlaneMask)
{
    tdc->keyControl &= ((usePlaneMask) ? 0xF0000000 : 0x00000000);
    tdc->keyControl |= (keyControl & 0x0FFFFFFF);
    BEGIN_RING(4);
    OUT_RING_H1(VIA_REG_KEYCONTROL, tdc->keyControl);
    if (keyControl) {
        OUT_RING_H1(VIA_REG_SRCCOLORKEY, transColor);
    }
    ADVANCE_RING_VARIABLE;
}

/*
 * Emit a copy blit operation to the command buffer.
 */
static void
viaAccelCopyHelper(ViaCommandBuffer * cb, int xs, int ys, int xd, int yd,
                   int w, int h, unsigned srcFbBase, unsigned dstFbBase,
                   CARD32 mode, unsigned srcPitch, unsigned dstPitch,
                   CARD32 cmd)
{
    if (cmd & VIA_GEC_DECY) {
        ys += h - 1;
        yd += h - 1;
    }

    if (cmd & VIA_GEC_DECX) {
        xs += w - 1;
        xd += w - 1;
    }

    BEGIN_RING(16);
    OUT_RING_H1(VIA_REG_GEMODE, mode);
    OUT_RING_H1(VIA_REG_SRCBASE, srcFbBase >> 3);
    OUT_RING_H1(VIA_REG_DSTBASE, dstFbBase >> 3);
    OUT_RING_H1(VIA_REG_PITCH, VIA_PITCH_ENABLE |
                ((dstPitch >> 3) << 16) | (srcPitch >> 3));
    OUT_RING_H1(VIA_REG_SRCPOS, (ys << 16) | (xs & 0xFFFF));
    OUT_RING_H1(VIA_REG_DSTPOS, (yd << 16) | (xd & 0xFFFF));
    OUT_RING_H1(VIA_REG_DIMENSION, ((h - 1) << 16) | (w - 1));
    OUT_RING_H1(VIA_REG_GECMD, cmd);
    ADVANCE_RING;
}

/*
 * Mark Sync using the 2D blitter for AGP. NoOp for PCI.
 * In the future one could even launch a NULL PCI DMA command
 * to have an interrupt generated, provided it is possible to
 * write to the PCI DMA engines from the AGP command stream.
 */
int
viaAccelMarkSync(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);

    RING_VARS;

    ++pVia->curMarker;

    /* Wrap around without affecting the sign bit. */
    pVia->curMarker &= 0x7FFFFFFF;

    if (pVia->agpDMA) {
        BEGIN_RING(2);
        OUT_RING_H1(VIA_REG_KEYCONTROL, 0x00);
        ADVANCE_RING;
        viaAccelSolidHelper(cb, 0, 0, 1, 1, pVia->markerOffset,
                            VIA_GEM_32bpp, 4, pVia->curMarker,
                            (0xF0 << 24) | VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT);
	FLUSH_RING;
    }
    return pVia->curMarker;
}

/*
 * Wait for the value to get blitted, or in the PCI case for engine idle.
 */
void
viaAccelWaitMarker(ScreenPtr pScreen, int marker)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 uMarker = marker;

    if (pVia->agpDMA) {
        while ((pVia->lastMarkerRead - uMarker) > (1 << 24))
            pVia->lastMarkerRead = *pVia->markerBuf;
    } else {
        viaAccelSync(pScrn);
    }
}

/*
 * Check if we need to force upload of the whole 3D state (when other
 * clients or subsystems have touched the 3D engine). Also tell DRI
 * clients and subsystems that we have touched the 3D engine.
 */
static Bool
viaCheckUpload(ScrnInfoPtr pScrn, Via3DState * v3d)
{
    VIAPtr pVia = VIAPTR(pScrn);
    Bool forceUpload;

    forceUpload = (pVia->lastToUpload != v3d);
    pVia->lastToUpload = v3d;

#ifdef XF86DRI
    if (pVia->directRenderingEnabled) {
        volatile struct drm_via_sarea *saPriv = (struct drm_via_sarea *)
                DRIGetSAREAPrivate(pScrn->pScreen);
        int myContext = DRIGetContext(pScrn->pScreen);

        forceUpload = forceUpload || (saPriv->ctxOwner != myContext);
        saPriv->ctxOwner = myContext;
    }
#endif
    return forceUpload;
}

static Bool
viaOrder(CARD32 val, CARD32 * shift)
{
    *shift = 0;

    while (val > (1 << *shift))
        (*shift)++;
    return (val == (1 << *shift));
}


/*
 * Exa functions. It is assumed that EXA does not exceed the blitter limits.
 */

static struct _ViaOffscreenBuffer *
viaInBuffer(struct _WSDriListHead *head, void *ptr)
{
    struct _ViaOffscreenBuffer *entry;
    struct _WSDriListHead *list;
    unsigned long offset;

    WSDRILISTFOREACH(list, head) {
	entry = WSDRILISTENTRY(list, struct _ViaOffscreenBuffer, head);
	offset = (unsigned long)ptr - (unsigned long)entry->virtual;
	if (offset < entry->size)
	    return entry;
    }
    return NULL;
}

static Bool
viaExaPixmapIsOffscreen(PixmapPtr p)
{
    ScreenPtr pScreen = p->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);

    return (viaInBuffer(&pVia->offscreen, p->devPrivate.ptr) != NULL);
}

static unsigned long
viaExaPixmapOffset(PixmapPtr p)
{
    ScreenPtr pScreen = p->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    void *ptr;
    struct _ViaOffscreenBuffer *buf;

    ptr = (void *) exaGetPixmapOffset(p) + 
	(unsigned long) pVia->exaMem.virtual;
    
    buf = viaInBuffer(&pVia->offscreen, ptr);

    if (!buf) 
	FatalError("Offscreen pixmap is not offscreen.\n");

    return (unsigned long)ptr - (unsigned long) buf->virtual +
	driBOOffset(buf->buf);
}


static Bool
viaExaPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planeMask, Pixel fg)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    RING_VARS;

    if (exaGetPixmapPitch(pPixmap) & 7)
        return FALSE;

    if (!viaAccelSetMode(pPixmap->drawable.depth, tdc))
        return FALSE;

    if (!viaAccelPlaneMaskHelper(tdc, planeMask))
        return FALSE;
    viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, TRUE);

    tdc->cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT | VIAACCELPATTERNROP(alu);

    tdc->fgColor = fg;

    return TRUE;
}

static void
viaExaSolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstPitch, dstOffset;
    int w = x2 - x1, h = y2 - y1;
    RING_VARS;

    dstPitch = exaGetPixmapPitch(pPixmap);
    dstOffset = viaExaPixmapOffset(pPixmap);

    viaAccelSolidHelper(cb, x1, y1, w, h, dstOffset,
                        tdc->mode, dstPitch, tdc->fgColor, tdc->cmd);
}

static void
viaExaDoneSolidCopy(PixmapPtr pPixmap)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    RING_VARS;

    FLUSH_RING;
}

static Bool
viaExaPrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int xdir,
                  int ydir, int alu, Pixel planeMask)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    RING_VARS;

    if (pSrcPixmap->drawable.bitsPerPixel != pDstPixmap->drawable.bitsPerPixel)
        return FALSE;

    if ((tdc->srcPitch = exaGetPixmapPitch(pSrcPixmap)) & 3)
        return FALSE;

    if (exaGetPixmapPitch(pDstPixmap) & 7)
        return FALSE;

    tdc->srcOffset = viaExaPixmapOffset(pSrcPixmap);

    tdc->cmd = VIA_GEC_BLT | VIAACCELCOPYROP(alu);
    if (xdir < 0)
        tdc->cmd |= VIA_GEC_DECX;
    if (ydir < 0)
        tdc->cmd |= VIA_GEC_DECY;

    if (!viaAccelSetMode(pDstPixmap->drawable.bitsPerPixel, tdc))
        return FALSE;

    if (!viaAccelPlaneMaskHelper(tdc, planeMask))
        return FALSE;
    viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, TRUE);

    return TRUE;
}

static void
viaExaCopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX, int dstY,
           int width, int height)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;
    CARD32 srcOffset = tdc->srcOffset;
    CARD32 dstOffset = viaExaPixmapOffset(pDstPixmap);
    RING_VARS;

    if (!width || !height)
        return;

    viaAccelCopyHelper(cb, srcX, srcY, dstX, dstY, width, height,
                       srcOffset, dstOffset, tdc->mode, tdc->srcPitch,
                       exaGetPixmapPitch(pDstPixmap), tdc->cmd);
}

#ifdef VIA_DEBUG_COMPOSITE
static void
viaExaCompositePictDesc(PicturePtr pict, char *string, int n)
{
    char format[20];
    char size[20];

    if (!pict) {
        snprintf(string, n, "None");
        return;
    }

    switch (pict->format) {
        case PICT_x8r8g8b8:
            snprintf(format, 20, "RGB8888");
            break;
        case PICT_a8r8g8b8:
            snprintf(format, 20, "ARGB8888");
            break;
        case PICT_r5g6b5:
            snprintf(format, 20, "RGB565  ");
            break;
        case PICT_x1r5g5b5:
            snprintf(format, 20, "RGB555  ");
            break;
        case PICT_a8:
            snprintf(format, 20, "A8      ");
            break;
        case PICT_a1:
            snprintf(format, 20, "A1      ");
            break;
        default:
            snprintf(format, 20, "0x%x", (int)pict->format);
            break;
    }

    snprintf(size, 20, "%dx%d%s", pict->pDrawable->width,
             pict->pDrawable->height, pict->repeat ? " R" : "");

    snprintf(string, n, "0x%lx: fmt %s (%s)", (long)pict->pDrawable, format,
             size);
}

static void
viaExaPrintComposite(CARD8 op,
                     PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst)
{
    char sop[20];
    char srcdesc[40], maskdesc[40], dstdesc[40];

    switch (op) {
        case PictOpSrc:
            sprintf(sop, "Src");
            break;
        case PictOpOver:
            sprintf(sop, "Over");
            break;
        default:
            sprintf(sop, "0x%x", (int)op);
            break;
    }

    viaExaCompositePictDesc(pSrc, srcdesc, 40);
    viaExaCompositePictDesc(pMask, maskdesc, 40);
    viaExaCompositePictDesc(pDst, dstdesc, 40);

    ErrorF("Composite fallback: op %s, \n"
           "                    src  %s, \n"
           "                    mask %s, \n"
           "                    dst  %s, \n", sop, srcdesc, maskdesc, dstdesc);
}
#endif /* VIA_DEBUG_COMPOSITE */

/*
 * Helper for bitdepth expansion.
 */
static CARD32
viaBitExpandHelper(CARD32 pixel, CARD32 bits)
{
    CARD32 component, mask, tmp;

    component = pixel & ((1 << bits) - 1);
    mask = (1 << (8 - bits)) - 1;
    tmp = component << (8 - bits);
    return ((component & 1) ? (tmp | mask) : tmp);
}

/*
 * Extract the components from a pixel of the given format to an argb8888 pixel.
 * This is used to extract data from one-pixel repeat pixmaps.
 * Assumes little endian.
 */
static void
viaPixelARGB8888(unsigned format, void *pixelP, CARD32 * argb8888)
{
    CARD32 bits, shift, pixel, bpp;

    bpp = PICT_FORMAT_BPP(format);

    if (bpp <= 8) {
        pixel = *((CARD8 *) pixelP);
    } else if (bpp <= 16) {
        pixel = *((CARD16 *) pixelP);
    } else {
        pixel = *((CARD32 *) pixelP);
    }

    switch (PICT_FORMAT_TYPE(format)) {
        case PICT_TYPE_A:
            bits = PICT_FORMAT_A(format);
            *argb8888 = viaBitExpandHelper(pixel, bits) << 24;
            return;
        case PICT_TYPE_ARGB:
            shift = 0;
            bits = PICT_FORMAT_B(format);
            *argb8888 = viaBitExpandHelper(pixel, bits);
            shift += bits;
            bits = PICT_FORMAT_G(format);
            *argb8888 |= viaBitExpandHelper(pixel >> shift, bits) << 8;
            shift += bits;
            bits = PICT_FORMAT_R(format);
            *argb8888 |= viaBitExpandHelper(pixel >> shift, bits) << 16;
            shift += bits;
            bits = PICT_FORMAT_A(format);
            *argb8888 |= ((bits) ? viaBitExpandHelper(pixel >> shift,
                                                      bits) : 0xFF) << 24;
            return;
        case PICT_TYPE_ABGR:
            shift = 0;
            bits = PICT_FORMAT_B(format);
            *argb8888 = viaBitExpandHelper(pixel, bits) << 16;
            shift += bits;
            bits = PICT_FORMAT_G(format);
            *argb8888 |= viaBitExpandHelper(pixel >> shift, bits) << 8;
            shift += bits;
            bits = PICT_FORMAT_R(format);
            *argb8888 |= viaBitExpandHelper(pixel >> shift, bits);
            shift += bits;
            bits = PICT_FORMAT_A(format);
            *argb8888 |= ((bits) ? viaBitExpandHelper(pixel >> shift,
                                                      bits) : 0xFF) << 24;
            return;
        default:
            break;
    }
    return;
}

/*
 * Check if the above function will work.
 */
static Bool
viaExpandablePixel(int format)
{
    int formatType = PICT_FORMAT_TYPE(format);

    return (formatType == PICT_TYPE_A ||
            formatType == PICT_TYPE_ABGR || formatType == PICT_TYPE_ARGB);
}

#ifdef XF86DRI

static int
viaAccelDMADownload(ScrnInfoPtr pScrn, unsigned long fbOffset,
                    unsigned srcPitch, unsigned char *dst,
                    unsigned dstPitch, unsigned w, unsigned h)
{
    VIAPtr pVia = VIAPTR(pScrn);
    struct drm_via_dmablit blit[2], *curBlit;
    unsigned char *sysAligned = NULL;
    Bool doSync[2], useBounceBuffer;
    unsigned pitch, numLines[2];
    int curBuf, err, i, ret, blitHeight;

    ret = 0;

    useBounceBuffer = (((unsigned long)dst & 15) || (dstPitch & 15));
    doSync[0] = FALSE;
    doSync[1] = FALSE;
    curBuf = 1;
    blitHeight = h;
    pitch = dstPitch;
    if (useBounceBuffer) {
        pitch = ALIGN_TO(dstPitch, 16);
        blitHeight = VIA_DMA_DL_SIZE / pitch;
    }

    while (doSync[0] || doSync[1] || h != 0) {
        curBuf = 1 - curBuf;
        curBlit = &blit[curBuf];
        if (doSync[curBuf]) {

            do {
                err = drmCommandWrite(pVia->drmFD, DRM_VIA_BLIT_SYNC,
                                      &curBlit->sync, sizeof(curBlit->sync));
            } while (err == -EAGAIN);

            if (err)
                return err;

            doSync[curBuf] = FALSE;
            if (useBounceBuffer) {
                for (i = 0; i < numLines[curBuf]; ++i) {
                    memcpy(dst, curBlit->mem_addr, w);
                    dst += dstPitch;
                    curBlit->mem_addr += pitch;
                }
            }
        }

        if (h == 0)
            continue;

        curBlit->num_lines = (h > blitHeight) ? blitHeight : h;
        h -= curBlit->num_lines;
        numLines[curBuf] = curBlit->num_lines;

        sysAligned =
                (unsigned char *)pVia->dBounce + (curBuf * VIA_DMA_DL_SIZE);
        sysAligned = (unsigned char *)
                ALIGN_TO((unsigned long)sysAligned, 16);

        curBlit->mem_addr = (useBounceBuffer) ? sysAligned : dst;
        curBlit->line_length = w;
        curBlit->mem_stride = pitch;
        curBlit->fb_addr = fbOffset;
        curBlit->fb_stride = srcPitch;
        curBlit->to_fb = 0;
        fbOffset += curBlit->num_lines * srcPitch;

        do {
            err = drmCommandWriteRead(pVia->drmFD, DRM_VIA_DMA_BLIT, curBlit,
                                      sizeof(*curBlit));
        } while (err == -EAGAIN);

        if (err) {
            ret = err;
            h = 0;
            continue;
        }

        doSync[curBuf] = TRUE;
    }

    return ret;
}

/*
 * Use PCI DMA if we can. If the system alignments don't match, we're using
 * an aligned bounce buffer for pipelined PCI DMA and memcpy.
 * Throughput for large transfers is around 65 MB/s.
 */
static Bool
viaExaDownloadFromScreen(PixmapPtr pSrc, int x, int y, int w, int h,
                         char *dst, int dst_pitch)
{
    ScrnInfoPtr pScrn = xf86Screens[pSrc->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    unsigned srcPitch = exaGetPixmapPitch(pSrc);
    unsigned wBytes = (pSrc->drawable.bitsPerPixel * w + 7) >> 3;
    unsigned srcOffset;
    char *bounceAligned = NULL;
    unsigned totSize;

    if (!w || !h)
        return TRUE;

    srcOffset = x * pSrc->drawable.bitsPerPixel;
    if (srcOffset & 3)
        return FALSE;
    srcOffset = viaExaPixmapOffset(pSrc) + y * srcPitch + (srcOffset >> 3);

    totSize = wBytes * h;

    exaWaitSync(pScrn->pScreen);
    if (totSize < VIA_MIN_DOWNLOAD) {
	return FALSE;
#if 0
        bounceAligned = (char *)pVia->FBBase + srcOffset;
        while (h--) {
            memcpy(dst, bounceAligned, wBytes);
            dst += dst_pitch;
            bounceAligned += srcPitch;
        }
        return TRUE;
#endif
    }

    if (!pVia->directRenderingEnabled)
        return FALSE;

    if ((srcPitch & 3) || (srcOffset & 3)) {
        ErrorF("VIA EXA download src_pitch misaligned\n");
        return FALSE;
    }

    if (viaAccelDMADownload(pScrn, srcOffset, srcPitch, (unsigned char *)dst,
                            dst_pitch, wBytes, h))
        return FALSE;

    return TRUE;
}

#endif
#if 0

static Bool
viaExaUploadToScratch(PixmapPtr pSrc, PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86Screens[pSrc->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    char *src, *dst;
    unsigned w, wBytes, srcPitch, h;
    CARD32 dstPitch;

    if (!pVia->scratchAddr)
        return FALSE;

    *pDst = *pSrc;
    w = pSrc->drawable.width;
    h = pSrc->drawable.height;
    wBytes = (w * pSrc->drawable.bitsPerPixel + 7) >> 3;

    viaOrder(wBytes, &dstPitch);
    dstPitch = 1 << dstPitch;
    if (dstPitch < 8)
        dstPitch = 8;
    if (dstPitch * h > pVia->exaScratchSize * 1024) {
        ErrorF("EXA UploadToScratch Failed %u %u %u %u\n",
               dstPitch, h, dstPitch * h, pVia->exaScratchSize * 1024);
        return FALSE;
    }

    pDst->devKind = dstPitch;
    pDst->devPrivate.ptr = dst = pVia->scratchAddr;
    src = pSrc->devPrivate.ptr;
    srcPitch = exaGetPixmapPitch(pSrc);

    /*
     * Copying to AGP needs not be HW accelerated.
     * If scratch is in FB, we are without DRI and HW accel.
     */

    viaAccelSync(pScrn);

    while (h--) {
        memcpy(dst, src, wBytes);
        dst += dstPitch;
        src += srcPitch;
    }

    return TRUE;
}

#endif

static Bool
viaExaCheckComposite(int op, PicturePtr pSrcPicture,
                     PicturePtr pMaskPicture, PicturePtr pDstPicture)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPicture->pDrawable->pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    Via3DState *v3d = &pVia->v3d;

    /* Reject small composites early. They are done much faster in software. */
    if (!pSrcPicture->repeat &&
        pSrcPicture->pDrawable->width *
        pSrcPicture->pDrawable->height < VIA_MIN_COMPOSITE) {
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Size.\n");
#endif
        return FALSE;
    }
    if (pMaskPicture &&
        !pMaskPicture->repeat &&
        pMaskPicture->pDrawable->width *
        pMaskPicture->pDrawable->height < VIA_MIN_COMPOSITE) {
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Size.\n");
#endif
        return FALSE;
    }
    if (pSrcPicture->transform ||
	(pMaskPicture && pMaskPicture->transform)) {
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Tranform\n");
#endif
	return FALSE;
    }

    if (pMaskPicture && pMaskPicture->componentAlpha) {
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Component Alpha operation\n");
#endif
        return FALSE;
    }

    if (!v3d->opSupported(op)) {
#ifdef VIA_DEBUG_COMPOSITE
#warning Composite verbose debug turned on.
        ErrorF("Operator not supported\n");
        viaExaPrintComposite(op, pSrcPicture, pMaskPicture, pDstPicture);
#endif
        return FALSE;
    }

    /*
     * FIXME: A8 destination formats are currently not supported and do not
     * seem supported by the hardware, although there are some leftover
     * register settings apparent in the via_3d_reg.h file. We need to fix this
     * (if important), by using component ARGB8888 operations with bitmask.
     */

    if (!v3d->dstSupported(pDstPicture->format)) {
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Destination format not supported:\n");
        viaExaPrintComposite(op, pSrcPicture, pMaskPicture, pDstPicture);
#endif
        return FALSE;
    }

    if (v3d->texSupported(pSrcPicture->format)) {
        if (pMaskPicture && (PICT_FORMAT_A(pMaskPicture->format) == 0 ||
                             !v3d->texSupported(pMaskPicture->format))) {
#ifdef VIA_DEBUG_COMPOSITE
            ErrorF("Mask format not supported:\n");
            viaExaPrintComposite(op, pSrcPicture, pMaskPicture, pDstPicture);
#endif
            return FALSE;
        }
        return TRUE;
    }
#ifdef VIA_DEBUG_COMPOSITE
    ErrorF("Src format not supported:\n");
    viaExaPrintComposite(op, pSrcPicture, pMaskPicture, pDstPicture);
#endif
    return FALSE;
}

static Bool
viaIsAGP(VIAPtr pVia, PixmapPtr pPix, unsigned long *offset)
{
#if 0
#ifdef XF86DRI
    unsigned long offs;

    if (pVia->directRenderingEnabled && !pVia->IsPCI) {
        offs = ((unsigned long)pPix->devPrivate.ptr
                - (unsigned long)pVia->agpMappedAddr);

        if ((offs - pVia->scratchOffset) < pVia->agpSize) {
            *offset = offs + pVia->agpAddr;
            return TRUE;
        }
    }
#endif
#endif
    return FALSE;
}

static Bool
viaExaPrepareComposite(int op, PicturePtr pSrcPicture,
                       PicturePtr pMaskPicture, PicturePtr pDstPicture,
                       PixmapPtr pSrc, PixmapPtr pMask, PixmapPtr pDst)
{
    CARD32 height, width;
    ScrnInfoPtr pScrn = xf86Screens[pDst->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    Via3DState *v3d = &pVia->v3d;
    int curTex = 0;
    ViaTexBlendingModes srcMode;
    Bool isAGP;
    unsigned long offset;
    CARD32 col;

    v3d->setDestination(v3d, viaExaPixmapOffset(pDst),
                        exaGetPixmapPitch(pDst), pDstPicture->format);
    v3d->setCompositeOperator(v3d, op);
    v3d->setDrawing(v3d, 0x0c, 0xFFFFFFFF, 0x000000FF, 0xFF);

    viaOrder(pSrc->drawable.width, &width);
    viaOrder(pSrc->drawable.height, &height);

    /*
     * For one-pixel repeat mask pictures we avoid using multitexturing by
     * modifying the src's texture blending equation and feed the pixel
     * value as a constant alpha for the src's texture. Multitexturing on the
     * Unichromes seems somewhat slow, so this speeds up translucent windows.
     */

    srcMode = via_src;
    pVia->maskP = NULL;
    if (pMaskPicture &&
        (pMaskPicture->pDrawable->height == 1) &&
        (pMaskPicture->pDrawable->width == 1) &&
        pMaskPicture->repeat && viaExpandablePixel(pMaskPicture->format)) {
        pVia->maskP = pMask->devPrivate.ptr;
        pVia->maskFormat = pMaskPicture->format;
        pVia->componentAlpha = pMaskPicture->componentAlpha;
        srcMode = ((pMaskPicture->componentAlpha)
                   ? via_src_onepix_comp_mask : via_src_onepix_mask);
    }

    /*
     * One-Pixel repeat src pictures go as solid color instead of textures.
     * Speeds up window shadows.
     */

    pVia->srcP = NULL;
    if (pSrcPicture && pSrcPicture->repeat
        && (pSrcPicture->pDrawable->height == 1)
        && (pSrcPicture->pDrawable->width == 1)
        && viaExpandablePixel(pSrcPicture->format)) {
        pVia->srcP = pSrc->devPrivate.ptr;
        pVia->srcFormat = pSrcPicture->format;
    }

    /* Exa should be smart enough to eliminate this IN operation. */
    if (pVia->srcP && pVia->maskP) {
        ErrorF("Bad one-pixel IN composite operation. "
               "EXA needs to be smarter.\n");
        return FALSE;
    }

    if (!pVia->srcP) {
        offset = viaExaPixmapOffset(pSrc);
        isAGP = viaIsAGP(pVia, pSrc, &offset);
        if (!v3d->setTexture(v3d, curTex, offset,
                             exaGetPixmapPitch(pSrc), pVia->nPOT[curTex],
                             1 << width, 1 << height, pSrcPicture->format,
                             via_repeat, via_repeat, srcMode, isAGP)) {
	  
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Src setTexture\n");
#endif
            return FALSE;
        }
        curTex++;
    }

    if (pMaskPicture && !pVia->maskP) {
        offset = viaExaPixmapOffset(pMask);
        isAGP = viaIsAGP(pVia, pMask, &offset);
        viaOrder(pMask->drawable.width, &width);
        viaOrder(pMask->drawable.height, &height);
        if (!v3d->setTexture(v3d, curTex, offset,
                             exaGetPixmapPitch(pMask), pVia->nPOT[curTex],
                             1 << width, 1 << height, pMaskPicture->format,
                             via_repeat, via_repeat,
                             ((pMaskPicture->componentAlpha)
                              ? via_comp_mask : via_mask), isAGP)) {
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Dst setTexture\n");
#endif
            return FALSE;
        }
        curTex++;
    }

    v3d->setFlags(v3d, curTex, FALSE, TRUE, TRUE);
    if (pVia->maskP) {
        viaPixelARGB8888(pVia->maskFormat, pVia->maskP, &col);
        v3d->setTexBlendCol(v3d, 0, pVia->componentAlpha, col);
    }
    if (pVia->srcP) {
        viaPixelARGB8888(pVia->srcFormat, pVia->srcP, &col);
        v3d->setDrawing(v3d, 0x0c, 0xFFFFFFFF, col & 0x00FFFFFF, col >> 24);
    }
    v3d->emitState(v3d, &pVia->cb, viaCheckUpload(pScrn, v3d));
    v3d->emitClipRect(v3d, &pVia->cb, 0, 0, pDst->drawable.width,
                      pDst->drawable.height);

    return TRUE;
}

static void
viaExaComposite(PixmapPtr pDst, int srcX, int srcY, int maskX, int maskY,
                int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86Screens[pDst->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    Via3DState *v3d = &pVia->v3d;
#if 0
    if (pVia->maskP || pVia->srcP)
        v3d->emitState(v3d, &pVia->cb, viaCheckUpload(pScrn, v3d));
#endif
    if (pVia->srcP) {
      srcX = maskX;
      srcY = maskY;
    }
    v3d->emitQuad(v3d, &pVia->cb, dstX, dstY, srcX, srcY, maskX, maskY,
                  width, height);
}

static Bool 
viaExaPrepareAccess(PixmapPtr pPix, int index)
{
    ScrnInfoPtr pScrn = xf86Screens[pPix->drawable.pScreen->myNum];
    
    viaAccelSync(pScrn);
    return GL_TRUE;
}


static ExaDriverPtr
viaInitExa(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ExaDriverPtr pExa = exaDriverAlloc();

    memset(pExa, 0, sizeof(*pExa));

    if (!pExa)
        return NULL;

    pExa->exa_major = EXA_VERSION_MAJOR;
    pExa->exa_minor = EXA_VERSION_MINOR;
    pExa->memoryBase = (unsigned char *) pVia->exaMem.virtual;
    pExa->memorySize = pVia->exaMem.size;
    pExa->offScreenBase = 0;
    pExa->pixmapOffsetAlign = 32;
    pExa->pixmapPitchAlign = 16;
    pExa->flags = EXA_OFFSCREEN_PIXMAPS |
            (pVia->nPOT[1] ? 0 : EXA_OFFSCREEN_ALIGN_POT);
    pExa->maxX = 2047;
    pExa->maxY = 2047;
    pExa->WaitMarker = viaAccelWaitMarker;
    pExa->MarkSync = viaAccelMarkSync;
    pExa->PrepareSolid = viaExaPrepareSolid;
    pExa->Solid = viaExaSolid;
    pExa->DoneSolid = viaExaDoneSolidCopy;
    pExa->PrepareCopy = viaExaPrepareCopy;
    pExa->Copy = viaExaCopy;
    pExa->DoneCopy = viaExaDoneSolidCopy;
    pExa->PrepareAccess = viaExaPrepareAccess;
    pExa->DownloadFromScreen = NULL;
    pExa->PixmapIsOffscreen = viaExaPixmapIsOffscreen;

#ifdef XF86DRI
    if (pVia->directRenderingEnabled) 
	pExa->DownloadFromScreen = viaExaDownloadFromScreen;
   
#endif /* XF86DRI */

    pExa->UploadToScratch = NULL; /* viaExaUploadToScratch; */

    if (!pVia->noComposite) {
        pExa->CheckComposite = viaExaCheckComposite;
        pExa->PrepareComposite = viaExaPrepareComposite;
        pExa->Composite = viaExaComposite;
        pExa->DoneComposite = viaExaDoneSolidCopy;
    } else {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "[EXA] Disabling EXA accelerated composite.\n");
    }

    if (!exaDriverInit(pScreen, pExa)) {
        xfree(pExa);
        return NULL;
    }

    viaInit3DState(&pVia->v3d);
    return pExa;
}

/*
 * Acceleration initialization function. Sets up offscreen memory disposition,
 * and initializes engines and acceleration method.
 */

Bool
viaInitAccel(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    Bool nPOTSupported;
    int ret;

    viaInitialize2DEngine(pScrn);

    /*
     * Pixmap cache.
     */

    ret = driGenBuffers(pVia->mainPool, "Pixmap cache", 1,
			&pVia->exaMem.buf, 0, 
			DRM_BO_FLAG_MEM_VRAM |
			DRM_BO_FLAG_READ |
			DRM_BO_FLAG_WRITE, 0);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
	       "[Accel] Failed allocating offscreen pixmap space.\n");
	return FALSE;
    }

    ret = driBOData(pVia->exaMem.buf, 1*1024*1024, NULL, NULL, 0);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
	       "[Accel] Failed allocating offscreen pixmap space.\n");	
	goto out_err0;
    }

    pVia->exaMem.virtual = driBOMap(pVia->exaMem.buf, WS_DRI_MAP_READ | WS_DRI_MAP_WRITE);
    if (pVia->exaMem.virtual == NULL) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
	       "[Accel] Failed mapping offscreen pixmap space.\n");	
	goto out_err0;
    }	
    driBOUnmap(pVia->exaMem.buf);
    pVia->exaMem.size = driBOSize(pVia->exaMem.buf);

    WSDRIINITLISTHEAD(&pVia->offscreen);
    WSDRILISTADDTAIL(&pVia->front.head, &pVia->offscreen);
    WSDRILISTADDTAIL(&pVia->exaMem.head, &pVia->offscreen);

    /* Sync marker space. */
    ret = driBOData(pVia->scanout.bufs[VIA_SCANOUT_SYNC],
		    64, NULL, NULL, 0);
    if (ret) {
	goto out_err0;
    }

    pVia->markerBuf = (CARD32 *) driBOMap(pVia->scanout.bufs[VIA_SCANOUT_SYNC],
					  WS_DRI_MAP_READ | WS_DRI_MAP_WRITE);
    if (!pVia->markerBuf)
	goto out_err1;

    driBOUnmap(pVia->scanout.bufs[VIA_SCANOUT_SYNC]);
    pVia->markerOffset = driBOOffset(pVia->scanout.bufs[VIA_SCANOUT_SYNC]);

    *pVia->markerBuf = 0;
    pVia->curMarker = 0;
    pVia->lastMarkerRead = 0;

    /*
     * nPOT textures. DRM versions below 2.11.0 don't allow them.
     * Also some CLE266 hardware may not allow nPOT textures for 
     * texture engine 1. We need to figure that out.
     */

    nPOTSupported = TRUE;
#ifdef XF86DRI
    nPOTSupported = ((!pVia->directRenderingEnabled) ||
                     (pVia->drmVerMajor > 2) ||
                     ((pVia->drmVerMajor == 2) && (pVia->drmVerMinor >= 11)));
#endif
    pVia->nPOT[0] = nPOTSupported;
    pVia->nPOT[1] = nPOTSupported;

#ifdef XF86DRI
    pVia->dBounce = NULL;
#endif /* XF86DRI */
    pVia->exaDriverPtr = viaInitExa(pScreen);
    if (!pVia->exaDriverPtr) {

	/*
	 * Docs recommend turning off also Xv here, but we handle this
	 * case with the old linear offscreen FB manager through
	 * VIAInitLinear.
	 */
	
	pVia->NoAccel = TRUE;
	goto out_err0;
    }
    
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	       "[EXA] Trying to enable EXA acceleration.\n");
    
    pVia->driSize = (pVia->FBFreeEnd - pVia->FBFreeStart) / 2;
    
    if ((pVia->driSize > (pVia->maxDriSize * 1024))
	&& pVia->maxDriSize > 0)
	pVia->driSize = pVia->maxDriSize * 1024;
    
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	       "[EXA] Enabled EXA acceleration.\n");

    return TRUE;
 out_err1:
    (void) driBOData(pVia->scanout.bufs[VIA_SCANOUT_SYNC],
		     0, NULL, NULL, 0);
 out_err0:

    driDeleteBuffers(1, &pVia->exaMem.buf);
    return FALSE;

}

/*
 * Free the used acceleration resources.
 */
void
viaExitAccel(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);

    viaAccelSync(pScrn);
    viaTearDownCBuffer(&pVia->cb);
    driDeleteBuffers(1, &pVia->exaMem.buf);

    if (pVia->useEXA) {
#ifdef XF86DRI
        if (pVia->dBounce)
            xfree(pVia->dBounce);
#endif /* XF86DRI */
        if (pVia->exaDriverPtr) {
            exaDriverFini(pScreen);
        }
        xfree(pVia->exaDriverPtr);
        pVia->exaDriverPtr = NULL;
        return;
    }
    if (pVia->AccelInfoRec) {
        XAADestroyInfoRec(pVia->AccelInfoRec);
        pVia->AccelInfoRec = NULL;
    }
}

/*
 * Allocate a command buffer and  buffers for accelerated upload, download,
 * and EXA scratch area. The scratch area resides primarily in AGP memory,
 * but reverts to FB if AGP is not available. 
 */
void
viaFinishInitAccel(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);

#ifdef XF86DRI

    if (pVia->directRenderingEnabled && pVia->useEXA) {

        pVia->dBounce = xcalloc(VIA_DMA_DL_SIZE * 2, 1);

    }
#endif /* XF86DRI */

    if (Success != viaSetupCBuffer(pScrn, &pVia->cb, 0)) {
        pVia->NoAccel = TRUE;
        viaExitAccel(pScreen);
        return;
    }
}

/*
 * DGA accelerated functions go here and let them be independent of
 * acceleration method.
 */
void
viaAccelBlitRect(ScrnInfoPtr pScrn, int srcx, int srcy, int w, int h,
                 int dstx, int dsty)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;
    unsigned dstOffset = pVia->displayOffset + dsty * pVia->Bpl;
    unsigned srcOffset = pVia->displayOffset + srcy * pVia->Bpl;

    RING_VARS;

    if (!w || !h)
        return;

    if (!pVia->NoAccel) {

        int xdir = ((srcx < dstx) && (srcy == dsty)) ? -1 : 1;
        int ydir = (srcy < dsty) ? -1 : 1;
        CARD32 cmd = VIA_GEC_BLT | VIAACCELCOPYROP(GXcopy);

        if (xdir < 0)
            cmd |= VIA_GEC_DECX;
        if (ydir < 0)
            cmd |= VIA_GEC_DECY;

        viaAccelSetMode(pScrn->bitsPerPixel, tdc);
        viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, FALSE);
        viaAccelCopyHelper(cb, srcx, 0, dstx, 0, w, h, srcOffset, dstOffset,
                           tdc->mode, pVia->Bpl, pVia->Bpl, cmd);
        pVia->accelMarker = viaAccelMarkSync(pScrn->pScreen);
	FLUSH_RING;
    }
}

void
viaAccelFillRect(ScrnInfoPtr pScrn, int x, int y, int w, int h,
                 unsigned long color)
{
    VIAPtr pVia = VIAPTR(pScrn);
    unsigned dstBase = pVia->displayOffset + y * pVia->Bpl;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT |
            VIAACCELPATTERNROP(GXcopy);
    RING_VARS;

    if (!w || !h)
        return;

    if (!pVia->NoAccel) {
        viaAccelSetMode(pScrn->bitsPerPixel, tdc);
        viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, FALSE);
        viaAccelSolidHelper(cb, x, 0, w, h, dstBase, tdc->mode,
                            pVia->Bpl, color, cmd);
        pVia->accelMarker = viaAccelMarkSync(pScrn->pScreen);
	FLUSH_RING;
    }
}

void
viaAccelFillPixmap(ScrnInfoPtr pScrn,
                   unsigned long offset,
                   unsigned long pitch,
                   int depth, int x, int y, int w, int h, unsigned long color)
{
    VIAPtr pVia = VIAPTR(pScrn);
    unsigned dstBase = offset + y * pitch;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT |
            VIAACCELPATTERNROP(GXcopy);
    RING_VARS;

    if (!w || !h)
        return;

    if (!pVia->NoAccel) {
        viaAccelSetMode(depth, tdc);
        viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, FALSE);
        viaAccelSolidHelper(cb, x, 0, w, h, dstBase, tdc->mode,
                            pitch, color, cmd);
        pVia->accelMarker = viaAccelMarkSync(pScrn->pScreen);
        ADVANCE_RING;
	FLUSH_RING;
    }
}

void
viaAccelSyncMarker(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);

    viaAccelWaitMarker(pScrn->pScreen, pVia->accelMarker);
}

void
viaAccelTextureBlit(ScrnInfoPtr pScrn, unsigned long srcOffset,
                    unsigned srcPitch, unsigned w, unsigned h, unsigned srcX,
                    unsigned srcY, unsigned srcFormat, unsigned long dstOffset,
                    unsigned dstPitch, unsigned dstX, unsigned dstY,
                    unsigned dstFormat, int rotate)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 wOrder, hOrder;
    Via3DState *v3d = &pVia->v3d;

    viaOrder(w, &wOrder);
    viaOrder(h, &hOrder);

    v3d->setDestination(v3d, dstOffset, dstPitch, dstFormat);
    v3d->setDrawing(v3d, 0x0c, 0xFFFFFFFF, 0x000000FF, 0x00);
    v3d->setFlags(v3d, 1, TRUE, TRUE, FALSE);
    v3d->setTexture(v3d, 0, srcOffset, srcPitch, TRUE,
                    1 << wOrder, 1 << hOrder, srcFormat,
                    via_single, via_single, via_src, FALSE);
    v3d->emitState(v3d, &pVia->cb, viaCheckUpload(pScrn, v3d));
    v3d->emitClipRect(v3d, &pVia->cb, dstX, dstY, w, h);
    v3d->emitQuad(v3d, &pVia->cb, dstX, dstY, srcX, srcY, 0, 0, w, h);
}
