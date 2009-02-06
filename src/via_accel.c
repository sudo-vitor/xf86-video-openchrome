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

#include <wsbm_manager.h>
#include <X11/Xarch.h>
#include "xaalocal.h"
#include "xaarop.h"
#include "miline.h"

#include "via.h"
#include "via_driver.h"
#include "via_regs.h"
#include "via_id.h"
#include "via_dmabuffer.h"
#include "ochr_ioctl.h"

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
    int tmpSize;
    int ret = 0;
    Via3DState *v3d = &pVia->v3d;

    /* Align end of command buffer for AGP DMA. */
    if (pVia->agpDMA && cb->mode == 2 && cb->rindex != HC_ParaType_CmdVdata
        && (cb->pos & 1)) {
        OUT_RING(HC_DUMMY);
    }

    tmpSize = cb->pos * sizeof(CARD32);
    cb->mode = 0;
    cb->has3dState = FALSE;
    
    if (cb->pos > 0)
	ret = ochr_execbuf(pVia->drmFD, cb);

    cb->needsPCI = FALSE;
    cb->execFlags = 0x0;

    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "Command buffer submission failed: \"%s\".\n",
		   strerror(-ret));	    
	viaDumpDMA(cb);
    }    

    cb->pos = 0;
    ret = ochr_reset_cmdlists(cb);

    if (ret) {
	FatalError("Failed trying to reset command buffer: \"%s\".\n",
		   strerror(-ret));
    }
    
    /*
     * Force re-emission of volatile 2D- and 3D state:
     */

    if (cb->inComposite) {
	v3d->emitState(v3d, &pVia->cb, GL_TRUE);
	v3d->emitClipRect(v3d, &pVia->cb, 0, 0, cb->compWidth,
			  cb->compHeight);
    }

    cb->srcPixmap = NULL;
    cb->dstPixmap = NULL;
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
    buf->pos = 0;
    buf->mode = 0;
    buf->header_start = 0;
    buf->rindex = 0;
    buf->has3dState = FALSE;
    buf->flushFunc = NULL;
    buf->inComposite = FALSE;
    buf->needsPCI = FALSE;
    buf->execFlags = 0x0;
    buf->execIoctlOffset = pVia->execIoctlOffset;
#ifdef XF86DRI
    if (pVia->directRenderingEnabled) {
        buf->flushFunc = viaFlushDRIEnabled;
    }
#endif
    buf->reloc_info = ochr_create_reloc_buffer();
    if (!buf->reloc_info)
	goto out_err0;
    
    buf->validate_list = wsbmBOCreateList(30, 1);
    if (!buf->validate_list)
	goto out_err1;

    return Success;
 out_err1:
    ochr_free_reloc_buffer(buf->reloc_info);
 out_err0:
    xfree(buf->buf);
    return BadAlloc;
}

/*
 * Free resources associated with a command buffer.
 */
void
viaTearDownCBuffer(ViaCommandBuffer * buf)
{
    if (buf->validate_list) {
	wsbmBOUnrefUserList(buf->validate_list);
	wsbmBOFreeList(buf->validate_list);
	buf->validate_list = NULL;
    }
    if (buf->reloc_info) {
	ochr_free_reloc_buffer(buf->reloc_info);
	buf->reloc_info = NULL;
    }    
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
    tdc->bpp = bpp;
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

static unsigned long
viaExaSuperPixmapOffset(PixmapPtr p, 
			struct _WsbmBufferObject **driBuf)
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

    *driBuf = buf->buf;
    return (unsigned long)ptr - (unsigned long) buf->virtual + 
	wsbmBOPoolOffset(buf->buf);
}


static Bool
viaEmitPixmap(ViaCommandBuffer *cb, 
	      PixmapPtr pDstPix,
	      PixmapPtr pSrcPix)
{
    struct _WsbmBufferObject *buf;
    unsigned long delta;
    int ret;

    if (pDstPix != NULL && cb->dstPixmap != pDstPix) {
	delta = viaExaSuperPixmapOffset(pDstPix, &buf);
	OUT_RING_H1(VIA_REG_DSTBASE, 0);
	OUT_RING_H1(VIA_REG_DSTPOS, 0);
	ret = ochr_2d_relocation(cb, buf, delta, 32, 0,
				 WSBM_PL_FLAG_VRAM, WSBM_PL_MASK_MEM);
	if (ret)
	    goto out_err;
	cb->dstPixmap = pDstPix;
    }

    if (pSrcPix != NULL && cb->srcPixmap != pSrcPix) {
	delta = viaExaSuperPixmapOffset(pSrcPix, &buf); 
	OUT_RING_H1(VIA_REG_SRCBASE, 0);
	OUT_RING_H1(VIA_REG_SRCPOS, 0);
	ret = ochr_2d_relocation(cb, buf, delta, 32, 0,
				 WSBM_PL_FLAG_VRAM, WSBM_PL_MASK_MEM);
	if (ret)
	    goto out_err;
	cb->srcPixmap = pSrcPix;
    } 
    return TRUE;
 out_err:
    return FALSE;
}

/*
 * Emit a solid blit operation to the command buffer. 
 */
static void
viaAccelSolidHelper(ViaCommandBuffer * cb, int x, int y, int w, int h,
                    struct _WsbmBufferObject *buf, 
		    unsigned delta, unsigned bpp, 
		    CARD32 mode, unsigned pitch,
                    CARD32 fg, CARD32 cmd)
{
    CARD32 pos = (y << 16) | (x & 0xFFFF);
    int ret;

    BEGIN_RING(14);
    OUT_RING_H1(VIA_REG_GEMODE, mode);
    OUT_RING_H1(VIA_REG_PITCH, VIA_PITCH_ENABLE | (pitch >> 3) << 16);
    OUT_RING_H1(VIA_REG_DSTBASE, 0);
    OUT_RING_H1(VIA_REG_DSTPOS, 0);
    ret = ochr_2d_relocation(cb, buf, delta, bpp, pos,
			     WSBM_PL_FLAG_VRAM, WSBM_PL_MASK_MEM);
    OUT_RING_H1(VIA_REG_DIMENSION, ((h - 1) << 16) | (w - 1));
    OUT_RING_H1(VIA_REG_FGCOLOR, fg);
    OUT_RING_H1(VIA_REG_GECMD, cmd);
    ADVANCE_RING;
    cb->dstPixmap = NULL;
}

static void
viaAccelSolidPixmapHelper(ViaCommandBuffer * cb, int x, int y, int w, int h,
			  PixmapPtr pPix, 
			  CARD32 mode, unsigned pitch,
			  CARD32 fg, CARD32 cmd)
{
    CARD32 pos = (y << 16) | (x & 0xFFFF);
    Bool ret;
    
    BEGIN_RING(16);
    OUT_RING_H1(VIA_REG_GEMODE, mode);
    OUT_RING_H1(VIA_REG_PITCH, VIA_PITCH_ENABLE | (pitch >> 3) << 16);
    ret = viaEmitPixmap(cb, pPix, NULL);
    OUT_RING_H1(VIA_REG_DSTPOS, pos);
    OUT_RING_H1(VIA_REG_DIMENSION, ((h - 1) << 16) | (w - 1));
    OUT_RING_H1(VIA_REG_FGCOLOR, fg);
    OUT_RING_H1(VIA_REG_GECMD, cmd);
    ADVANCE_RING_VARIABLE;
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
viaAccelCopyPixmapHelper(ViaCommandBuffer * cb, int xs, int ys, int xd, int yd,
			 int w, int h, 
			 PixmapPtr pSrcPixmap,
			 PixmapPtr pDstPixmap,
			 CARD32 mode, unsigned srcPitch, unsigned dstPitch,
			 CARD32 cmd)
{
    int ret;

    if (cmd & VIA_GEC_DECY) {
        ys += h - 1;
        yd += h - 1;
    }

    if (cmd & VIA_GEC_DECX) {
        xs += w - 1;
        xd += w - 1;
    }

    BEGIN_RING(20);
    OUT_RING_H1(VIA_REG_GEMODE, mode);
    ret = viaEmitPixmap(cb, pDstPixmap, pSrcPixmap);
    OUT_RING_H1(VIA_REG_SRCPOS, (ys << 16) | (xs & 0xFFFF));
    OUT_RING_H1(VIA_REG_DSTPOS, (yd << 16) | (xd & 0xFFFF));
    OUT_RING_H1(VIA_REG_PITCH, VIA_PITCH_ENABLE |
                ((dstPitch >> 3) << 16) | (srcPitch >> 3));
    OUT_RING_H1(VIA_REG_DIMENSION, ((h - 1) << 16) | (w - 1));
    OUT_RING_H1(VIA_REG_GECMD, cmd);
    ADVANCE_RING_VARIABLE;
}

/*
 * Emit a copy blit operation to the command buffer.
 */
static void
viaAccelCopyHelper(ViaCommandBuffer * cb, int xs, int ys, int xd, int yd,
		   int w, int h, 
		   struct _WsbmBufferObject *buf,
		   unsigned long delta,
		   unsigned int bpp,
		   CARD32 mode, unsigned srcPitch, unsigned dstPitch,
		   CARD32 cmd)
{
    int ret;
    CARD32 srcPos;
    CARD32 dstPos;

    if (cmd & VIA_GEC_DECY) {
        ys += h - 1;
        yd += h - 1;
    }

    if (cmd & VIA_GEC_DECX) {
        xs += w - 1;
        xd += w - 1;
    }
    srcPos = (ys << 16) | (xs & 0xFFFF);
    dstPos = (yd << 16) | (xd & 0xFFFF);

    BEGIN_RING(20);
    OUT_RING_H1(VIA_REG_GEMODE, mode);
    OUT_RING_H1(VIA_REG_SRCBASE, 0);
    OUT_RING_H1(VIA_REG_SRCPOS, 0);
    ret = ochr_2d_relocation(cb, buf, delta, bpp, srcPos,
			     WSBM_PL_FLAG_VRAM, WSBM_PL_MASK_MEM);
    OUT_RING_H1(VIA_REG_DSTBASE, 0);
    OUT_RING_H1(VIA_REG_DSTPOS, 0);
    ret = ochr_2d_relocation(cb, buf, delta, bpp, dstPos,
			     WSBM_PL_FLAG_VRAM, WSBM_PL_MASK_MEM);
    OUT_RING_H1(VIA_REG_PITCH, VIA_PITCH_ENABLE |
                ((dstPitch >> 3) << 16) | (srcPitch >> 3));
    OUT_RING_H1(VIA_REG_DIMENSION, ((h - 1) << 16) | (w - 1));
    OUT_RING_H1(VIA_REG_GECMD, cmd);
    ADVANCE_RING_VARIABLE;
    cb->srcPixmap = NULL;
    cb->dstPixmap = NULL;
}


static void
viaExaWaitMarker(ScreenPtr pScreen, int marker)
{
    ;
}

static int
viaExaMarkSync(ScreenPtr pScreen)
{
    return 1;
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

#ifdef XF86DRI
    return 1;
#endif
    forceUpload = (pVia->lastToUpload != v3d);
    pVia->lastToUpload = v3d;

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

struct _ViaOffscreenBuffer *
viaInBuffer(struct _WsbmListHead *head, void *ptr)
{
    struct _ViaOffscreenBuffer *entry;
    struct _WsbmListHead *list;
    unsigned long offset;

    WSBMLISTFOREACH(list, head) {
	entry = WSBMLISTENTRY(list, struct _ViaOffscreenBuffer, head);
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

    if (pVia->vtNotified == TRUE)
	return FALSE;

    return (viaInBuffer(&pVia->offscreen, p->devPrivate.ptr) != NULL);
}

static Bool
viaExaPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planeMask, Pixel fg)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    if (exaGetPixmapPitch(pPixmap) & 7)
        return FALSE;

    if (!viaAccelSetMode(pPixmap->drawable.depth, tdc))
        return FALSE;

    if (!viaAccelPlaneMaskHelper(tdc, planeMask))
        return FALSE;

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
    struct _WsbmBufferObject *buf;
    RING_VARS;

    dstPitch = exaGetPixmapPitch(pPixmap);
    dstOffset = viaExaSuperPixmapOffset(pPixmap, &buf);

    viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, TRUE);
    viaAccelSolidPixmapHelper(cb, x1, y1, w, h, pPixmap,
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

static void
viaFreeScratchBuffers(VIAPtr pVia)
{
    struct _ViaOffscreenBuffer *entry;
    struct _WsbmListHead *list;
    struct _WsbmListHead *prev;

    WSBMLISTFOREACHPREVSAFE(list, prev, &pVia->offscreen) {
	entry = WSBMLISTENTRY(list, struct _ViaOffscreenBuffer, head);
	if (entry->scratch) {
	    WSBMLISTDEL(list);
	    wsbmBOUnreference(&entry->buf);
	    free(entry);
	}
    }
}
    
static void
viaExaDoneComposite(PixmapPtr pPixmap)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    RING_VARS;

    cb->inComposite = FALSE;
    FLUSH_RING;

    viaFreeScratchBuffers(pVia);
}


static Bool
viaExaPrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int xdir,
                  int ydir, int alu, Pixel planeMask)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    if (pSrcPixmap->drawable.bitsPerPixel != pDstPixmap->drawable.bitsPerPixel)
        return FALSE;

    if ((tdc->srcPitch = exaGetPixmapPitch(pSrcPixmap)) & 3)
        return FALSE;

    if (exaGetPixmapPitch(pDstPixmap) & 7)
        return FALSE;

    tdc->cmd = VIA_GEC_BLT | VIAACCELCOPYROP(alu);
    if (xdir < 0)
        tdc->cmd |= VIA_GEC_DECX;
    if (ydir < 0)
        tdc->cmd |= VIA_GEC_DECY;

    if (!viaAccelSetMode(pDstPixmap->drawable.bitsPerPixel, tdc))
        return FALSE;

    if (!viaAccelPlaneMaskHelper(tdc, planeMask))
        return FALSE;

    tdc->pSrcPixmap = pSrcPixmap;

    return TRUE;
}

static void
viaExaCopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX, int dstY,
           int width, int height)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;
    RING_VARS;

    if (!width || !height)
        return;

    viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, TRUE);
    viaAccelCopyPixmapHelper(cb, srcX, srcY, dstX, dstY, width, height,
			     tdc->pSrcPixmap, pDstPixmap, tdc->mode, tdc->srcPitch,
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

static Bool
viaExaUploadToScratch(PixmapPtr pSrc, PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86Screens[pSrc->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    char *src, *dst;
    unsigned w, wBytes, srcPitch, h;
    CARD32 dstPitch;
    struct _ViaOffscreenBuffer *entry;
    int ret;
    unsigned size;

    entry = malloc(sizeof(*entry));
    if (!entry)
	return FALSE;

    ret = wsbmGenBuffers(pVia->mainPool, 1, 
			&entry->buf, 0, 
			VIA_PL_FLAG_AGP |
			WSBM_PL_FLAG_VRAM);
    if (ret)
	goto out_err0;

    w = pSrc->drawable.width;
    h = pSrc->drawable.height;
    wBytes = (w * pSrc->drawable.bitsPerPixel + 7) >> 3;
    dstPitch = (wBytes + 31) & ~31;
    size = dstPitch * h;

    ret = wsbmBOData(entry->buf, size, NULL, NULL, 0);
    if (ret)
	goto out_err1;

    dst = wsbmBOMap(entry->buf, WSBM_ACCESS_WRITE);
    if (dst == NULL)
	goto out_err1;

    *pDst = *pSrc;
    
    pDst->devKind = dstPitch;
    pDst->devPrivate.ptr = dst;
    src = (void *) exaGetPixmapOffset(pSrc) + 
      (unsigned long) pVia->exaMem.virtual;

    srcPitch = exaGetPixmapPitch(pSrc);

    while (h--) {
        memcpy(dst, src, wBytes);
        dst += dstPitch;
        src += srcPitch;
    }

    (void) wsbmBOUnmap(entry->buf);
    entry->size = size;
    entry->virtual = pDst->devPrivate.ptr;
    entry->scratch = TRUE;

    WSBMLISTADDTAIL(&entry->head, &pVia->offscreen);

    return TRUE;

  out_err1:
    wsbmBOUnreference(&entry->buf);
  out_err0:
    free(entry);
    return FALSE;
}

static Bool
viaIsPot(unsigned int val)
{
    return ((val & (val - 1)) == 0);
}


static Bool
viaExaCheckComposite(int op, PicturePtr pSrcPicture,
                     PicturePtr pMaskPicture, PicturePtr pDstPicture)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPicture->pDrawable->pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    Via3DState *v3d = &pVia->v3d;

    if (pSrcPicture->repeat && 
	(!viaIsPot(pSrcPicture->pDrawable->width) ||
	 !viaIsPot(pSrcPicture->pDrawable->height)))
	return FALSE;


    if (pMaskPicture && pMaskPicture->repeat && 
	(!viaIsPot(pMaskPicture->pDrawable->width) ||
	 !viaIsPot(pMaskPicture->pDrawable->height)))
	return FALSE;


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
viaPrepareAccess(PixmapPtr pPix, int index, Bool noWait)
{
    ScrnInfoPtr pScrn = xf86Screens[pPix->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    struct _ViaOffscreenBuffer *buf;
    uint32_t flags;
    void *ptr;
    int ret = 0;

    ptr = (void *) (exaGetPixmapOffset(pPix) + 
		    (unsigned long) pVia->exaMem.virtual);

    buf = viaInBuffer(&pVia->offscreen, ptr);
    if (buf) {
	flags = (index == EXA_PREPARE_DEST) ?
	    WSBM_SYNCCPU_WRITE : WSBM_SYNCCPU_READ;
	if (noWait)
	    flags |= WSBM_SYNCCPU_DONT_BLOCK;
	ret = wsbmBOSyncForCpu(buf->buf, flags);
    }

    pPix->devPrivate.ptr = ptr;

    return (ret == 0);
}

static Bool 
viaExaPrepareAccess(PixmapPtr pPix, int index)
{
    return viaPrepareAccess(pPix, index, 0);
}

static void
viaExaFinishAccess(PixmapPtr pPix, int index)
{
    ScrnInfoPtr pScrn = xf86Screens[pPix->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    struct _ViaOffscreenBuffer *buf;
    void *ptr;
    uint32_t flags;
    
    ptr = (void *) (exaGetPixmapOffset(pPix) + 
		    (unsigned long) pVia->exaMem.virtual);

    buf = viaInBuffer(&pVia->offscreen, ptr);

    if (buf) {
	flags = (index == EXA_PREPARE_DEST) ?
	    WSBM_SYNCCPU_WRITE : WSBM_SYNCCPU_READ;
	(void) wsbmBOReleaseFromCpu(buf->buf, flags);      
    }
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
    CARD32 col;
    struct _WsbmBufferObject *buf;
    unsigned long delta;

    delta = viaExaSuperPixmapOffset(pDst, &buf);
    v3d->setDestination(v3d, buf, delta,
                        exaGetPixmapPitch(pDst), pDstPicture->format);
    v3d->setCompositeOperator(v3d, op);
    v3d->setDrawing(v3d, 0x0c, 0xFFFFFFFF, 0x000000FF, 0xFF);    
    
    viaOrder(pSrcPicture->pDrawable->width, &width);
    viaOrder(pSrcPicture->pDrawable->height, &height);

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

	/*
	 * Only optimize if mask is currently idle.
	 */

	if (viaPrepareAccess(pMask, EXA_PREPARE_MASK, 1)) {
	    pVia->maskP = pMask->devPrivate.ptr;
	    pVia->maskFormat = pMaskPicture->format;
	    pVia->componentAlpha = pMaskPicture->componentAlpha;
	    srcMode = ((pMaskPicture->componentAlpha)
		       ? via_src_onepix_comp_mask : via_src_onepix_mask);

	    viaPixelARGB8888(pVia->maskFormat, pVia->maskP, &col);
	    v3d->setTexBlendCol(v3d, 0, pVia->componentAlpha, col);
	    viaExaFinishAccess(pMask, EXA_PREPARE_MASK);
	}
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

	if (viaPrepareAccess(pSrc, EXA_PREPARE_SRC, 1)) {	
	    pVia->srcP = pSrc->devPrivate.ptr;
	    pVia->srcFormat = pSrcPicture->format;
	    viaPixelARGB8888(pVia->srcFormat, pVia->srcP, &col);
	    v3d->setDrawing(v3d, 0x0c, 0xFFFFFFFF, col & 0x00FFFFFF, col >> 24);
	    viaExaFinishAccess(pSrc, EXA_PREPARE_SRC);
	}
    }

    /* Exa should be smart enough to eliminate this IN operation. */
    if (pVia->srcP && pVia->maskP) {
        ErrorF("Bad one-pixel IN composite operation. "
               "EXA needs to be smarter.\n");
        return FALSE;
    }

    if (!pVia->srcP) {

	delta = viaExaSuperPixmapOffset(pSrc, &buf);
	
        if (!v3d->setTexture(v3d, buf, delta, curTex,
                             exaGetPixmapPitch(pSrc), pVia->nPOT[curTex],
                             1 << width, 1 << height, pSrcPicture->format,
                             via_repeat, via_repeat, srcMode, TRUE)) {
	  
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Src setTexture\n");
#endif
            return FALSE;
        }
        curTex++;
    }

    if (pMaskPicture && !pVia->maskP) {
	delta = viaExaSuperPixmapOffset(pMask, &buf);

        viaOrder(pMask->drawable.width, &width);
        viaOrder(pMask->drawable.height, &height);
        if (!v3d->setTexture(v3d, buf, delta, curTex,
                             exaGetPixmapPitch(pMask), pVia->nPOT[curTex],
                             1 << width, 1 << height, pMaskPicture->format,
                             via_repeat, via_repeat,
                             ((pMaskPicture->componentAlpha)
                              ? via_comp_mask : via_mask), TRUE)) {
#ifdef VIA_DEBUG_COMPOSITE
        ErrorF("Dst setTexture\n");
#endif
            return FALSE;
        }
        curTex++;
    }

    v3d->setFlags(v3d, curTex, PICT_FORMAT_A(pDstPicture->format) != 0, 
		  TRUE, TRUE);

    v3d->emitState(v3d, &pVia->cb, viaCheckUpload(pScrn, v3d));
    v3d->emitClipRect(v3d, &pVia->cb, 0, 0, pDst->drawable.width,
                      pDst->drawable.height);
    
    pVia->cb.inComposite = TRUE;
    pVia->cb.compWidth =  pDst->drawable.width;
    pVia->cb.compHeight = pDst->drawable.height;

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
    pExa->WaitMarker = viaExaWaitMarker;
    pExa->MarkSync = viaExaMarkSync;
    pExa->PrepareSolid = viaExaPrepareSolid;
    pExa->Solid = viaExaSolid;
    pExa->DoneSolid = viaExaDoneSolidCopy;
    pExa->PrepareCopy = viaExaPrepareCopy;
    pExa->Copy = viaExaCopy;
    pExa->DoneCopy = viaExaDoneSolidCopy;
    pExa->PrepareAccess = viaExaPrepareAccess;
    pExa->FinishAccess = viaExaFinishAccess;
    pExa->DownloadFromScreen = NULL;
    pExa->PixmapIsOffscreen = viaExaPixmapIsOffscreen;

#ifdef XF86DRI
    if (pVia->directRenderingEnabled) 
	pExa->DownloadFromScreen = NULL; /*viaExaDownloadFromScreen;*/
   
#endif /* XF86DRI */

    pExa->UploadToScratch = viaExaUploadToScratch;

    if (!pVia->noComposite) {
        pExa->CheckComposite = viaExaCheckComposite;
        pExa->PrepareComposite = viaExaPrepareComposite;
        pExa->Composite = viaExaComposite;
        pExa->DoneComposite = viaExaDoneComposite;
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
    unsigned long exaMemSize = pScrn->videoRam * 1024 / 2;

    viaInitialize2DEngine(pScrn);

#ifdef XF86DRI
    if (pVia->directRenderingEnabled) {
	struct drm_via_getparam_arg arg;

	arg.param = DRM_VIA_PARAM_VRAM_SIZE;
	ret = drmCommandWriteRead(pVia->drmFD, DRM_VIA_GET_PARAM, &arg, 
			     sizeof(arg));
	if (ret == 0) {
	    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		       "[Accel] Largest free vram region is %lu bytes.\n",
		       (unsigned long) arg.value);
	    exaMemSize = (unsigned long) arg.value / 2;
	}
    }
#endif
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	       "[Accel] Setting evictable pixmap cache "
	       "to %lu bytes.\n",
	       exaMemSize);
	    
    /*
     * Pixmap cache.
     */

    ret = wsbmGenBuffers(pVia->mainPool, 1,
			&pVia->exaMem.buf, 0, 
			WSBM_PL_FLAG_VRAM);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
	       "[Accel] Failed allocating offscreen pixmap space.\n");
	return FALSE;
    }

    ret = wsbmBOData(pVia->exaMem.buf, exaMemSize, NULL, NULL, 0);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
	       "[Accel] Failed allocating offscreen pixmap space.\n");	
	goto out_err0;
    }

    pVia->exaMem.virtual = wsbmBOMap(pVia->exaMem.buf, 
				     WSBM_SYNCCPU_READ | WSBM_SYNCCPU_WRITE);
    if (pVia->exaMem.virtual == NULL) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
	       "[Accel] Failed mapping offscreen pixmap space.\n");	
	goto out_err0;
    }	
    pVia->exaMem.size = wsbmBOSize(pVia->exaMem.buf);
    pVia->exaMem.scratch = FALSE;
    pVia->front.scratch = FALSE;

    WSBMINITLISTHEAD(&pVia->offscreen);
    WSBMLISTADDTAIL(&pVia->front.head, &pVia->offscreen);
    WSBMLISTADDTAIL(&pVia->exaMem.head, &pVia->offscreen);

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
	
	goto out_err0;
    }
    
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	       "[EXA] Trying to enable EXA acceleration.\n");
    
    return TRUE;
 out_err0:

    pVia->NoAccel = TRUE;
    wsbmDeleteBuffers(1, &pVia->exaMem.buf);
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

#ifdef XF86DRI
    if (pVia->dBounce)
	xfree(pVia->dBounce);
#endif /* XF86DRI */
    if (pVia->exaDriverPtr) {
	exaDriverFini(pScreen);
    }
    xfree(pVia->exaDriverPtr);
    pVia->exaDriverPtr = NULL;
    viaTearDownCBuffer(&pVia->cb);
    viaFreeScratchBuffers(pVia);
    WSBMLISTDELINIT(&pVia->front.head);
    WSBMLISTDELINIT(&pVia->exaMem.head);
    (void) wsbmBOUnmap(pVia->exaMem.buf);
    wsbmDeleteBuffers(1, &pVia->exaMem.buf);
    return;
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
    struct _WsbmBufferObject *buf;
    unsigned long delta;

    RING_VARS;

    if (!w || !h)
        return;

    buf = pVia->scanout.bufs[VIA_SCANOUT_DISPLAY];
    delta = wsbmBOPoolOffset(buf);

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
        viaAccelCopyHelper(cb, srcx, srcy, dstx, dsty, w, h,
			   buf, delta, tdc->bpp,
			   tdc->mode, pVia->Bpl, pVia->Bpl, cmd);
	FLUSH_RING;
    }
}

void
viaAccelFillRect(ScrnInfoPtr pScrn, int x, int y, int w, int h,
                 unsigned long color)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;
    CARD32 cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT |
            VIAACCELPATTERNROP(GXcopy);
    struct _WsbmBufferObject *buf;
    unsigned long delta;
    
    RING_VARS;

    if (!w || !h)
        return;

    buf = pVia->scanout.bufs[VIA_SCANOUT_DISPLAY];
    delta = wsbmBOPoolOffset(buf);
    
    if (!pVia->NoAccel) {
        viaAccelSetMode(pScrn->bitsPerPixel, tdc);
        viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, FALSE);
        viaAccelSolidHelper(cb, x, y, w, h,
			    buf, delta, tdc->bpp,
			    tdc->mode,
			    pVia->Bpl, 
			    color, cmd);
	FLUSH_RING;
    }
}

void
viaAccelFillPixmap(ScrnInfoPtr pScrn,
		   PixmapPtr pPix, 
                   unsigned long pitch,
                   int depth, int x, int y, int w, int h, unsigned long color)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;
	
    CARD32 cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT |
            VIAACCELPATTERNROP(GXcopy);
    RING_VARS;

    if (!w || !h)
        return;

    if (!pVia->NoAccel) {
        viaAccelSetMode(depth, tdc);
        viaAccelTransparentHelper(tdc, cb, 0x0, 0x0, FALSE);
	viaAccelSolidPixmapHelper(cb, x, y, w, h, pPix, tdc->mode,
				  pitch, color, cmd);
        ADVANCE_RING;
	FLUSH_RING;
    }
}

void
viaAccelSyncMarker(ScrnInfoPtr pScrn)
{
    viaAccelSync(pScrn);
}

