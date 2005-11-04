/*
 * Copyright 1998-2003 VIA Technologies, Inc. All Rights Reserved.
 * Copyright 2001-2003 S3 Graphics, Inc. All Rights Reserved.
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
 *
 * Heavily cleaned up and modified for EXA support by Thomas Hellström 2005.
 */

/*************************************************************************
 *
 *  File:       via_accel.c
 *  Content:    2D acceleration function for VIA/S3G UniChrome
 *
 ************************************************************************/

#include "Xarch.h"
#include "xaalocal.h"
#include "xaarop.h"
#include "miline.h"

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

static void
viaInitAgp(VIAPtr pVia)
{
    VIASETREG(VIA_REG_TRANSET, 0x00100000);
    VIASETREG(VIA_REG_TRANSPACE, 0x00000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x00333004);
    VIASETREG(VIA_REG_TRANSPACE, 0x60000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x61000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x62000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x63000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x64000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x7D000000);

    VIASETREG(VIA_REG_TRANSET, 0xfe020000);
    VIASETREG(VIA_REG_TRANSPACE, 0x00000000);
}

static void
viaEnableVQ(VIAPtr pVia)
{
    CARD32
	vqStartAddr = pVia->VQStart,
	vqEndAddr = pVia->VQEnd,
	vqStartL = 0x50000000 | (vqStartAddr & 0xFFFFFF),
	vqEndL = 0x51000000 | (vqEndAddr & 0xFFFFFF),
	vqStartEndH = 0x52000000 | ((vqStartAddr & 0xFF000000) >> 24) |
	((vqEndAddr & 0xFF000000) >> 16),
	vqLen = 0x53000000 | (VIA_VQ_SIZE >> 3);

    VIASETREG(VIA_REG_TRANSET, 0x00fe0000);
    VIASETREG(VIA_REG_TRANSPACE, 0x080003fe);
    VIASETREG(VIA_REG_TRANSPACE, 0x0a00027c);
    VIASETREG(VIA_REG_TRANSPACE, 0x0b000260);
    VIASETREG(VIA_REG_TRANSPACE, 0x0c000274);
    VIASETREG(VIA_REG_TRANSPACE, 0x0d000264);
    VIASETREG(VIA_REG_TRANSPACE, 0x0e000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x0f000020);
    VIASETREG(VIA_REG_TRANSPACE, 0x1000027e);
    VIASETREG(VIA_REG_TRANSPACE, 0x110002fe);
    VIASETREG(VIA_REG_TRANSPACE, 0x200f0060);
    
    VIASETREG(VIA_REG_TRANSPACE, 0x00000006);
    VIASETREG(VIA_REG_TRANSPACE, 0x40008c0f);
    VIASETREG(VIA_REG_TRANSPACE, 0x44000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x45080c04);
    VIASETREG(VIA_REG_TRANSPACE, 0x46800408);
    
    VIASETREG(VIA_REG_TRANSPACE, vqStartEndH);
    VIASETREG(VIA_REG_TRANSPACE, vqStartL);
    VIASETREG(VIA_REG_TRANSPACE, vqEndL);
    VIASETREG(VIA_REG_TRANSPACE, vqLen);
}
    
void
viaDisableVQ(ScrnInfoPtr pScrn)
{
    VIAPtr  pVia = VIAPTR(pScrn);

    VIASETREG(VIA_REG_TRANSET, 0x00fe0000);
    VIASETREG(VIA_REG_TRANSPACE, 0x00000004);
    VIASETREG(VIA_REG_TRANSPACE, 0x40008c0f);
    VIASETREG(VIA_REG_TRANSPACE, 0x44000000);
    VIASETREG(VIA_REG_TRANSPACE, 0x45080c04);
    VIASETREG(VIA_REG_TRANSPACE, 0x46800408);
}    

static Bool
viaAccelSetMode(int bpp, ViaTwodContext *tdc)
{
    switch (bpp) {
    case 16:
	tdc->mode = VIA_GEM_16bpp;
	return TRUE;
    case 32:
	tdc->mode = VIA_GEM_32bpp;
	return TRUE;
    case 8:
	tdc->mode = VIA_GEM_8bpp;
	return TRUE;
    default:
        return FALSE;
    }
}

  

void
viaInitialize2DEngine(ScrnInfoPtr pScrn)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;
    int i;

    /* 
     * init 2D engine regs to reset 2D engine 
     */
     
    for (i=0x04; i<0x44; i+=4) {
	VIASETREG(i, 0x0);
    }

    viaInitAgp(pVia);

    if (pVia->VQStart != 0) {
	viaEnableVQ(pVia);
    } else {
	viaDisableVQ(pScrn);
    }

    viaAccelSetMode(pScrn->bitsPerPixel, tdc);
}


/*
 * Generic functions.
 */

void 
viaAccelSync(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    int loop = 0;
    
    mem_barrier();
    
    while (!(VIAGETREG(VIA_REG_STATUS) & VIA_VR_QUEUE_BUSY) && (loop++ < MAXLOOP))
	;
    
    while ((VIAGETREG(VIA_REG_STATUS) &
	    (VIA_CMD_RGTR_BUSY | VIA_2D_ENG_BUSY | VIA_3D_ENG_BUSY)) &&
	   (loop++ < MAXLOOP))
	;
}


static void
viaSetClippingRectangle(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    tdc->clipping = TRUE;
    tdc->clipX1 = x1;
    tdc->clipY1 = y1;
    tdc->clipX2 = x2;
    tdc->clipY2 = y2;
}


static void 
viaDisableClipping(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    tdc->clipping = FALSE;
}



static int
viaAccelClippingHelper(ViaCommandBuffer *cb, int refY, ViaTwodContext *tdc)
{   
    if (tdc->clipping) {
	refY = (refY < tdc->clipY1) ? refY : tdc->clipY1;
	tdc->cmd |= VIA_GEC_CLIP_ENABLE;
	BEGIN_RING_AGP(cb, 4);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_CLIPTL), ((tdc->clipY1 - refY) << 16) | tdc->clipX1);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_CLIPBR), ((tdc->clipY2 - refY) << 16) | tdc->clipX2);
    } else {
	tdc->cmd &= ~VIA_GEC_CLIP_ENABLE;
    }
    return refY;

}

static void 
viaAccelSolidHelper(ViaCommandBuffer *cb, int x, int y, int w, int h,
		    unsigned fbBase, CARD32 mode, unsigned pitch, CARD32 fg,
		    CARD32 cmd)
{
    BEGIN_RING_AGP(cb, 14); 
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), fbBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), (pitch >> 3) << 16);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), (y << 16) | x);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), ((h - 1) << 16) | (w - 1));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_FGCOLOR), fg);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), cmd);
}

static void
viaAccelTransparentHelper(ViaCommandBuffer *cb, CARD32 keyControl, 
			  CARD32 transColor)
{
    BEGIN_RING_AGP(cb, 4);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_KEYCONTROL), keyControl);
    if (keyControl) {
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCCOLORKEY), transColor);
    }
}

static void 
viaAccelCopyHelper(ViaCommandBuffer *cb, int xs, int ys, int xd, int yd,
		   int w, int h, unsigned srcFbBase, unsigned dstFbBase, CARD32 mode, 
		   unsigned srcPitch, unsigned dstPitch, CARD32 cmd)
{
    if (cmd & VIA_GEC_DECY) {
	ys += h-1;
	yd += h-1;
    }

    if (cmd & VIA_GEC_DECX) {
	xs += w-1;
	xd += w-1;
    }
    
    BEGIN_RING_AGP(cb, 16);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCBASE), srcFbBase >> 3);    
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstFbBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), ((dstPitch >> 3) << 16) | (srcPitch >> 3));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCPOS), (ys << 16) | xs);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), (yd << 16) | xd);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), ((h - 1) << 16) | (w - 1));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), cmd);
}

/*
 * XAA functions
 */
    
static void
viaSetupForScreenToScreenCopy(ScrnInfoPtr pScrn, int xdir, int ydir, int rop,
			      unsigned planemask, int trans_color)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    CARD32 cmd;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    cmd = VIA_GEC_BLT | VIAACCELCOPYROP(rop);

    if (xdir < 0)
	cmd |= VIA_GEC_DECX;

    if (ydir < 0)
	cmd |= VIA_GEC_DECY;

    tdc->cmd = cmd;
    viaAccelTransparentHelper(cb, (trans_color != -1) ? 0x4000 : 0x0000, 
			      trans_color);
}

static void
viaSubsequentScreenToScreenCopy(ScrnInfoPtr pScrn, int x1, int y1,
				int x2, int y2, int w, int h)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    int sub;

    if (!w || !h)
	return;

    sub = viaAccelClippingHelper(cb, y2, tdc);
    viaAccelCopyHelper(cb, x1, 0 , x2, y2-sub, w, h, 
		       pScrn->fbOffset+pVia->Bpl*y1, pScrn->fbOffset+pVia->Bpl*sub, 
		       tdc->mode, pVia->Bpl, pVia->Bpl, tdc->cmd);
    cb->flushFunc(cb);
}


/*
 * SetupForSolidFill is also called to set up for lines.
 */

static void
viaSetupForSolidFill(ScrnInfoPtr pScrn, int color, int rop, unsigned planemask)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    tdc->cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT | 
	VIAACCELPATTERNROP(rop);
    tdc->fgColor = color;
}


static void
viaSubsequentSolidFillRect(ScrnInfoPtr pScrn, int x, int y, int w, int h)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    int sub;

    if (!w || !h)
	return;

    sub = viaAccelClippingHelper(cb, y, tdc);
    viaAccelSolidHelper(cb, x, y-sub, w, h, pScrn->fbOffset+pVia->Bpl*sub, tdc->mode,
			pVia->Bpl, tdc->fgColor, tdc->cmd);
    cb->flushFunc(cb);
}


/*
 * The meaning of the two pattern paremeters to Setup & Subsequent for
 * Mono8x8Patterns varies depending on the flag bits.  We specify
 * HW_PROGRAMMED_BITS, which means our hardware can handle 8x8 patterns
 * without caching in the frame buffer.  Thus, Setup gets the pattern bits.
 * There is no way with BCI to rotate an 8x8 pattern, so we do NOT specify
 * HW_PROGRAMMED_ORIGIN.  XAA wil rotate it for us and pass the rotated
 * pattern to both Setup and Subsequent.  If we DID specify PROGRAMMED_ORIGIN,
 * then Setup would get the unrotated pattern, and Subsequent gets the
 * origin values.
 */

static void
viaSetupForMono8x8PatternFill(ScrnInfoPtr pScrn, int pattern0, int pattern1,
			      int fg, int bg, int rop, unsigned planemask)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    int     cmd;
    ViaTwodContext *tdc = &pVia->td;

    cmd = VIA_GEC_BLT | VIA_GEC_PAT_REG | VIA_GEC_PAT_MONO |
	VIAACCELPATTERNROP(rop);

    if (bg == -1) {
	cmd |= VIA_GEC_MPAT_TRANS;
    }

    tdc->cmd = cmd;
    tdc->fgColor = fg;
    tdc->bgColor = bg;
    tdc->pattern0 = pattern0;
    tdc->pattern1 = pattern1;
}


static void
viaSubsequentMono8x8PatternFillRect(ScrnInfoPtr pScrn, int patOffx, int patOffy,
				    int x, int y, int w, int h)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    CARD32  patOffset;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase;
    int sub;

    if (!w || !h)
	return;

    patOffset = ((patOffy & 0x7)  << 29) | ((patOffx & 0x7) << 26);
    sub = viaAccelClippingHelper(cb, y, tdc);
    dstBase = pScrn->fbOffset + sub*pVia->Bpl;

    BEGIN_RING_AGP(cb,22);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), tdc->mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
		    ((pVia->Bpl >> 3) << 16));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), ((y - sub) << 16) |  x);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), (((h - 1) << 16) | (w - 1)));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PATADDR), patOffset);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_FGCOLOR), tdc->fgColor);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_BGCOLOR), tdc->bgColor);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_MONOPAT0), tdc->pattern0);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_MONOPAT1), tdc->pattern1);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), tdc->cmd);
    cb->flushFunc(cb);
}

static void
viaSetupForColor8x8PatternFill(ScrnInfoPtr pScrn, int patternx, int patterny,
			       int rop, unsigned planemask, int trans_color)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    tdc->cmd = VIA_GEC_BLT | VIAACCELPATTERNROP(rop);
    tdc->patternAddr = (patternx * pVia->Bpp + patterny * pVia->Bpl);
}


static void
viaSubsequentColor8x8PatternFillRect(ScrnInfoPtr pScrn, int patOffx, int patOffy,
				     int x, int y, int w, int h)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    CARD32  patAddr;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase;
    int sub;

    if (!w || !h)
	return;

    patAddr = (tdc->patternAddr >> 3) |
	((patOffy & 0x7)  << 29) | ((patOffx & 0x7) << 26);
    sub = viaAccelClippingHelper(cb, y, tdc);
    dstBase = pScrn->fbOffset + sub*pVia->Bpl;

    BEGIN_RING_AGP(cb, 14);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), tdc->mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
		    ((pVia->Bpl >> 3) << 16));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), ((y - sub) << 16) | x);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), (((h - 1) << 16) | (w - 1)));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PATADDR), patAddr);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), tdc->cmd);
    cb->flushFunc(cb);
}

static void
viaSetupForCPUToScreenColorExpandFill(ScrnInfoPtr pScrn, int fg, int bg, int rop,
				      unsigned planemask)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    int     cmd;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    cmd = VIA_GEC_BLT | VIA_GEC_SRC_SYS | VIA_GEC_SRC_MONO |
	VIAACCELCOPYROP(rop);

    if (bg == -1) {
	cmd |= VIA_GEC_MSRC_TRANS;
    }

    tdc->cmd = cmd;
    tdc->fgColor = fg;
    tdc->bgColor = bg;

    cb->flushFunc(cb);
    /* Disable Transparent Bitblt */
    viaAccelTransparentHelper(cb, 0x0, 0x0);
}


static void
viaSubsequentScanlineCPUToScreenColorExpandFill(ScrnInfoPtr pScrn, int x, int y,
						int w, int h, int skipleft)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    int sub;

    if (skipleft) {
	viaSetClippingRectangle(pScrn, (x + skipleft), y, (x + w - 1), (y + h -1));
    }

    sub = viaAccelClippingHelper(cb, y, tdc);
    BEGIN_RING_AGP(cb, 4); 
    OUT_RING_QW_AGP(cb,H1_ADDR(VIA_REG_BGCOLOR), tdc->bgColor);
    OUT_RING_QW_AGP(cb,H1_ADDR(VIA_REG_FGCOLOR), tdc->fgColor);
    viaAccelCopyHelper(cb, 0, 0, x, y - sub, w, h, 0, pScrn->fbOffset + sub*pVia->Bpl, 
		       tdc->mode, pVia->Bpl, pVia->Bpl, tdc->cmd);

    /*
     * Can't use AGP for CPU to screen actions.
     */

    viaFlushPCI(cb);
}

static void
viaSetupForImageWrite(ScrnInfoPtr pScrn, int rop, unsigned planemask, int trans_color,
		      int bpp, int depth)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    tdc->cmd = VIA_GEC_BLT | VIA_GEC_SRC_SYS | VIAACCELCOPYROP(rop);
    cb->flushFunc(cb);
    viaAccelTransparentHelper(cb, (trans_color != -1) ? 0x4000 : 0x0000, 
			      trans_color);
}


static void
viaSubsequentImageWriteRect(ScrnInfoPtr pScrn, int x, int y, int w, int h,
			    int skipleft)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    int sub;

    if (skipleft) {
	viaSetClippingRectangle(pScrn, (x + skipleft), y, (x + w - 1), (y + h -1));
    }

    sub = viaAccelClippingHelper(cb, y, tdc);
    viaAccelCopyHelper(cb, 0, 0, x, y - sub, w, h, 0, pScrn->fbOffset + pVia->Bpl * sub, 
		       tdc->mode, pVia->Bpl, pVia->Bpl, tdc->cmd);
    /*
     * Can't use AGP for CPU to screen actions.
     */

    viaFlushPCI(cb);
}


/* Setup for XAA solid lines. */
static void
viaSetupForSolidLine(ScrnInfoPtr pScrn, int color, int rop, unsigned int planemask)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    tdc->cmd = VIA_GEC_FIXCOLOR_PAT | VIAACCELPATTERNROP(rop);
    tdc->fgColor = color;

    BEGIN_RING_AGP(cb,6);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), tdc->mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_MONOPAT0), 0xFF);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_FGCOLOR), tdc->fgColor);
}


static void
viaSubsequentSolidTwoPointLine(ScrnInfoPtr pScrn, int x1, int y1, 
			       int x2, int y2, int flags)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    int     dx, dy, cmd, tmp, error = 1;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase;
    int sub;

    cmd = tdc->cmd | VIA_GEC_LINE;

    dx = x2 - x1;
    if (dx < 0) {
	dx = -dx;
	cmd |= VIA_GEC_DECX;            /* line will be drawn from right */
	error = 0;
    }

    dy = y2 - y1;
    if (dy < 0) {
	dy = -dy;
	cmd |= VIA_GEC_DECY;            /* line will be drawn from bottom */
    }

    if (dy > dx) {
	tmp  = dy;
	dy = dx;
	dx = tmp;                       /* Swap 'dx' and 'dy' */
	cmd |= VIA_GEC_Y_MAJOR;         /* Y major line */
    }

    if (flags & OMIT_LAST) {
	cmd |= VIA_GEC_LASTPIXEL_OFF;
    }

    /* Set Src and Dst base address and pitch, pitch is qword */

    sub = viaAccelClippingHelper(cb, (y1 < y2) ? y1 : y2, tdc);

    dstBase = pScrn->fbOffset + sub*pVia->Bpl;
    y1 -= sub;
    y2 -= sub;

    BEGIN_RING_AGP(cb, 14);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
		    ((pVia->Bpl >> 3) << 16));
    /* major = 2*dmaj, minor = 2*dmin, err = -dmaj - ((bias >> octant) & 1) */
    /* K1 = 2*dmin K2 = 2*(dmin - dmax) */
    /* Error Term = (StartX<EndX) ? (2*dmin - dmax - 1) : (2*(dmin - dmax)) */
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_LINE_K1K2), ((((dy << 1) & 0x3fff) << 16)|
						     (((dy - dx) << 1) & 0x3fff)));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_LINE_XY), ((y1 << 16) | x1));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), dx);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_LINE_ERROR), (((dy << 1) - dx - error) & 0x3fff));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), cmd);
    cb->flushFunc(cb);

}


/* Subsequent XAA solid horizontal and vertical lines */
static void
viaSubsequentSolidHorVertLine(ScrnInfoPtr pScrn, int x, int y, int len, int dir)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase;
    int sub;

    sub = viaAccelClippingHelper(cb, y, tdc);
    dstBase = pScrn->fbOffset + sub*pVia->Bpl;

    BEGIN_RING_AGP(cb, 10);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
		    ((pVia->Bpl >> 3) << 16));
    
    if (dir == DEGREES_0) {
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), ((y - sub) << 16) | x);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), (len - 1));
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), tdc->cmd | VIA_GEC_BLT);
    } else {
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), ((y - sub) << 16) | x);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), ((len - 1) << 16));
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), tdc->cmd | VIA_GEC_BLT);
    }
    cb->flushFunc(cb);
}

static void
viaSetupForDashedLine(ScrnInfoPtr pScrn, int fg, int bg, int rop, unsigned int planemask,
		      int length, unsigned char *pattern)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    int     cmd;
    CARD32  pat = *(CARD32 *)pattern;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    cmd = VIA_GEC_LINE | VIA_GEC_FIXCOLOR_PAT | VIAACCELPATTERNROP(rop);

    if (bg == -1) {
	cmd |= VIA_GEC_MPAT_TRANS;
    }

    tdc->cmd = cmd;
    tdc->fgColor = fg;
    tdc->bgColor = bg;

    switch (length) {
    case  2: pat |= pat <<  2; /* fall through */
    case  4: pat |= pat <<  4; /* fall through */
    case  8: pat |= pat <<  8; /* fall through */
    case 16: pat |= pat << 16;
    }

    tdc->pattern0 = pat;
    
    BEGIN_RING_AGP(cb, 8);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), tdc->mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_FGCOLOR), tdc->fgColor);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_BGCOLOR), tdc->bgColor);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_MONOPAT0), tdc->pattern0);
}

static void
viaSubsequentDashedTwoPointLine(ScrnInfoPtr pScrn, int x1, int y1, int x2,
				int y2, int flags, int phase)
{
    viaSubsequentSolidTwoPointLine(pScrn, x1, y1, x2, y2, flags);
}


static int
viaInitXAA(ScreenPtr pScreen)
{

    ScrnInfoPtr     pScrn = xf86Screens[pScreen->myNum];
    VIAPtr          pVia = VIAPTR(pScrn);
    XAAInfoRecPtr   xaaptr;

    /* General acceleration flags */
    if (!(xaaptr = pVia->AccelInfoRec = XAACreateInfoRec()))
	return FALSE;

    xaaptr->Flags = PIXMAP_CACHE |
	OFFSCREEN_PIXMAPS |
	LINEAR_FRAMEBUFFER |
	MICROSOFT_ZERO_LINE_BIAS |
	0;

    if (pScrn->bitsPerPixel == 8)
	xaaptr->CachePixelGranularity = 128;

    /* Clipping */
    xaaptr->SetClippingRectangle = viaSetClippingRectangle;
    xaaptr->DisableClipping = viaDisableClipping;
    xaaptr->ClippingFlags = HARDWARE_CLIP_SOLID_FILL |
	HARDWARE_CLIP_SOLID_LINE |
	HARDWARE_CLIP_DASHED_LINE |
	HARDWARE_CLIP_SCREEN_TO_SCREEN_COPY |
	HARDWARE_CLIP_MONO_8x8_FILL |
	HARDWARE_CLIP_COLOR_8x8_FILL |
	HARDWARE_CLIP_SCREEN_TO_SCREEN_COLOR_EXPAND |
	0;

    xaaptr->Sync = viaAccelSync;

    /* ScreenToScreen copies */
    xaaptr->SetupForScreenToScreenCopy = viaSetupForScreenToScreenCopy;
    xaaptr->SubsequentScreenToScreenCopy = viaSubsequentScreenToScreenCopy;
    xaaptr->ScreenToScreenCopyFlags = NO_PLANEMASK | ROP_NEEDS_SOURCE;

    /* Solid filled rectangles */
    xaaptr->SetupForSolidFill = viaSetupForSolidFill;
    xaaptr->SubsequentSolidFillRect = viaSubsequentSolidFillRect;
    xaaptr->SolidFillFlags = NO_PLANEMASK | ROP_NEEDS_SOURCE;

    /* Mono 8x8 pattern fills */
    xaaptr->SetupForMono8x8PatternFill = viaSetupForMono8x8PatternFill;
    xaaptr->SubsequentMono8x8PatternFillRect =
	viaSubsequentMono8x8PatternFillRect;
    xaaptr->Mono8x8PatternFillFlags = NO_PLANEMASK |
	HARDWARE_PATTERN_PROGRAMMED_BITS |
	HARDWARE_PATTERN_PROGRAMMED_ORIGIN |
	BIT_ORDER_IN_BYTE_MSBFIRST |
	0;

    /* Color 8x8 pattern fills */
    xaaptr->SetupForColor8x8PatternFill = viaSetupForColor8x8PatternFill;
    xaaptr->SubsequentColor8x8PatternFillRect =
	viaSubsequentColor8x8PatternFillRect;
    xaaptr->Color8x8PatternFillFlags = NO_PLANEMASK |
	NO_TRANSPARENCY |
	HARDWARE_PATTERN_PROGRAMMED_BITS |
	HARDWARE_PATTERN_PROGRAMMED_ORIGIN |
	0;

    /* Solid lines */
    xaaptr->SetupForSolidLine = viaSetupForSolidLine;
    xaaptr->SubsequentSolidTwoPointLine = viaSubsequentSolidTwoPointLine;
    xaaptr->SubsequentSolidHorVertLine = viaSubsequentSolidHorVertLine;
    xaaptr->SolidBresenhamLineErrorTermBits = 14;
    xaaptr->SolidLineFlags = NO_PLANEMASK | ROP_NEEDS_SOURCE;

    /* dashed line */
    xaaptr->SetupForDashedLine = viaSetupForDashedLine;
    xaaptr->SubsequentDashedTwoPointLine = viaSubsequentDashedTwoPointLine;
    xaaptr->DashPatternMaxLength = 8;
    xaaptr->DashedLineFlags = NO_PLANEMASK |
	ROP_NEEDS_SOURCE |
	LINE_PATTERN_POWER_OF_2_ONLY |
	LINE_PATTERN_MSBFIRST_LSBJUSTIFIED |
	0;

    /* CPU to Screen color expansion */
    xaaptr->ScanlineCPUToScreenColorExpandFillFlags = NO_PLANEMASK |
	CPU_TRANSFER_PAD_DWORD |
	SCANLINE_PAD_DWORD |
	BIT_ORDER_IN_BYTE_MSBFIRST |
	LEFT_EDGE_CLIPPING |
	ROP_NEEDS_SOURCE |
	0;

    xaaptr->SetupForScanlineCPUToScreenColorExpandFill = 
	viaSetupForCPUToScreenColorExpandFill;
    xaaptr->SubsequentScanlineCPUToScreenColorExpandFill = 
	viaSubsequentScanlineCPUToScreenColorExpandFill;
    xaaptr->ColorExpandBase = pVia->BltBase;
    xaaptr->ColorExpandRange = VIA_MMIO_BLTSIZE;

    /* ImageWrite */
    xaaptr->ImageWriteFlags = NO_PLANEMASK |
	CPU_TRANSFER_PAD_DWORD |
	SCANLINE_PAD_DWORD |
	BIT_ORDER_IN_BYTE_MSBFIRST |
	LEFT_EDGE_CLIPPING |
	ROP_NEEDS_SOURCE |
	SYNC_AFTER_IMAGE_WRITE |
	0;
    
    /*
     * CLE266 has fast direct processor access to the framebuffer.
     * Therefore, disable the PCI GXcopy.
     */
    
    if (pVia->Chipset == VIA_CLE266)
	xaaptr->ImageWriteFlags |= NO_GXCOPY;

    xaaptr->SetupForImageWrite = viaSetupForImageWrite;
    xaaptr->SubsequentImageWriteRect = viaSubsequentImageWriteRect;
    xaaptr->ImageWriteBase = pVia->BltBase;
    xaaptr->ImageWriteRange = VIA_MMIO_BLTSIZE;

    return XAAInit(pScreen, xaaptr);

}

#ifdef VIA_HAVE_EXA

static Bool
viaExaPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask,
		   Pixel fg)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    
    if (exaGetPixmapPitch(pPixmap) & 7)
	return FALSE;

    if (! viaAccelSetMode(pPixmap->drawable.depth, tdc)) 
        return FALSE;

    viaAccelTransparentHelper(cb, 0x0, 0x0);

    tdc->cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT | 
	VIAACCELPATTERNROP(alu);

    tdc->fgColor = fg;

    return TRUE;
}

static void
viaExaSolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    unsigned dstPitch, dstBase;

    int
	w = x2 - x1,
	h = y2 - y1;

    dstPitch = exaGetPixmapPitch(pPixmap);
    dstBase = exaGetPixmapOffset(pPixmap);

    viaAccelSolidHelper(cb, x1, y1, w, h, dstBase,
			tdc->mode, dstPitch, tdc->fgColor, tdc->cmd);
    cb->flushFunc(cb);
}

static void
viaExaDoneSolidCopy(PixmapPtr pPixmap)
{
}

static Bool 
viaExaPrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int xdir,
		  int ydir, int alu, Pixel planeMask) 
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    
    if((planeMask & ((1 << pSrcPixmap->drawable.depth) - 1)) !=
       (1 << pSrcPixmap->drawable.depth) - 1) {
	return FALSE;
    }

    if(pSrcPixmap->drawable.bitsPerPixel != 
       pDstPixmap->drawable.bitsPerPixel)
	return FALSE;

    if ((tdc->srcPitch = exaGetPixmapPitch(pSrcPixmap)) & 3)
	return FALSE;

    if (exaGetPixmapPitch(pDstPixmap) & 7)
	return FALSE;

    tdc->srcOffset = exaGetPixmapOffset(pSrcPixmap);

    tdc->cmd = VIA_GEC_BLT | VIAACCELCOPYROP(alu);
    if (xdir < 0)
	tdc->cmd |= VIA_GEC_DECX;
    if (ydir < 0)
	tdc->cmd |= VIA_GEC_DECY;

    if (! viaAccelSetMode(pDstPixmap->drawable.bitsPerPixel, tdc)) 
        return FALSE;

    viaAccelTransparentHelper(cb, 0x0, 0x0);

    return TRUE;
}    

static void
viaExaCopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX, int dstY,
	   int width, int height)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    if (!width || !height)
	return;

    viaAccelCopyHelper(cb, srcX, srcY, dstX, dstY, width, height,
		       tdc->srcOffset, exaGetPixmapOffset(pDstPixmap),
		       tdc->mode, tdc->srcPitch, exaGetPixmapPitch(pDstPixmap), 
		       tdc->cmd);
    cb->flushFunc(cb);
}

static void
viaExaWaitMarker(ScreenPtr pScreen, int marker)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 uMarker = marker;

    while((pVia->lastMarkerRead - uMarker) > (1 << 24))
        pVia->lastMarkerRead = *pVia->markerBuf;
}


static Bool
viaExaDownloadFromScreen(PixmapPtr pSrc, int x, int y, int w, int h,
			 char *dst, int dst_pitch) 
{
    ScrnInfoPtr pScrn = xf86Screens[pSrc->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    unsigned bpp = (pSrc->drawable.bitsPerPixel >> 3); 
    unsigned i;

    char *src = (char *)pVia->FBBase + exaGetPixmapOffset(pSrc) + 
      y*exaGetPixmapPitch(pSrc) + x*bpp;
    
    for (i=0; i<h; ++i) {
	memcpy(dst, src, w*bpp);
	dst += dst_pitch;
	src += exaGetPixmapPitch(pSrc);
    }
    return TRUE;
}

static Bool
viaExaUploadToScreen(PixmapPtr pDst, int x, int y, int w, int h, char *src, int src_pitch) 
{
    ScrnInfoPtr pScrn = xf86Screens[pDst->drawable.pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    unsigned bpp = (pDst->drawable.bitsPerPixel >> 3); 
    unsigned i;

    char *dst = (char *)pVia->FBBase + exaGetPixmapOffset(pDst) + 
      y*exaGetPixmapPitch(pDst) + x*bpp;
    
    for (i=0; i<h; ++i) {
	memcpy(dst, src, w*bpp);
	dst += exaGetPixmapPitch(pDst);
	src += src_pitch;
    }
    return TRUE;
}

static int
viaExaMarkSync(ScreenPtr pScreen)
{
    ScrnInfoPtr     pScrn = xf86Screens[pScreen->myNum];
    VIAPtr          pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;

    ++pVia->curMarker;

    /*
     * Wrap around without possibly affecting the int sign bit. 
     */

    pVia->curMarker &= 0x7FFFFFFF; 

    viaAccelSolidHelper(&pVia->cb, 0, 0, 1, 1, pVia->markerOffset, VIA_GEM_32bpp,
			4, pVia->curMarker, 
			(0xF0 << 24) | VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT);
    cb->flushFunc(cb);
    return pVia->curMarker;
}

static Bool
viaExaUploadToScratch(PixmapPtr pSrc, PixmapPtr pDst)
{
    return FALSE;
}

static void
viaExaScratchSave(ScreenPtr pScreen, ExaOffscreenArea *area)
{
    ScrnInfoPtr     pScrn = xf86Screens[pScreen->myNum];
    VIAPtr          pVia = VIAPTR(pScrn);

    pVia->exa_scratch = NULL;
}

static ExaDriverPtr
viaInitExa(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    ExaDriverPtr pExa = (ExaDriverPtr) xnfcalloc(sizeof(ExaDriverRec),1);
    
    if (!pExa) 
	return NULL;
    
    pExa->card.memoryBase = pVia->FBBase;
    pExa->card.memorySize = pVia->FBFreeEnd;
    pExa->card.offScreenBase = pScrn->virtualY * pVia->Bpl;
    pExa->card.pixmapOffsetAlign = 16; /* PCI DMA Bitblt restriction */
    pExa->card.pixmapPitchAlign = 16; 
    pExa->card.flags = EXA_OFFSCREEN_PIXMAPS;
    pExa->card.maxX = 2047;
    pExa->card.maxY = 2047;

    pExa->accel.WaitMarker = viaExaWaitMarker;
    pExa->accel.MarkSync = viaExaMarkSync;
    pExa->accel.PrepareSolid = viaExaPrepareSolid;
    pExa->accel.Solid = viaExaSolid;
    pExa->accel.DoneSolid = viaExaDoneSolidCopy;
    pExa->accel.PrepareCopy = viaExaPrepareCopy;
    pExa->accel.Copy = viaExaCopy;
    pExa->accel.DoneCopy = viaExaDoneSolidCopy;
#if 0
    pExa->accel.UploadToScreen = viaExaUploadToScreen;
    pExa->accel.DownloadFromScreen = viaExaDownloadFromScreen;
#endif
    /*
     * Composite not supported. Could we use the 3D engine?
     */

    if (!exaDriverInit(pScreen, pExa)) {
	xfree(pExa);
	return NULL;
    }
#if 0
    pVia->exa_scratch = exaOffscreenAlloc(pScreen, 128*1024, 16, TRUE,
					  viaExaScratchSave, pVia);
    if (pVia->exa_scratch) {
	pVia->exa_scratch_next = pVia->exa_scratch->offset;
	pExa->accel.UploadToScratch = viaExaUploadToScratch;
    }
#endif
    return pExa;
}

#endif /* VIA_HAVE_EXA */

/* 
 * Acceleration init function, sets up pointers to our accelerated functions.
 */

Bool
viaInitAccel(ScreenPtr pScreen)
{
    ScrnInfoPtr     pScrn = xf86Screens[pScreen->myNum];
    VIAPtr          pVia = VIAPTR(pScrn);
    BoxRec 	    AvailFBArea;

    pVia->VQStart = 0;
    if (((pVia->FBFreeEnd - pVia->FBFreeStart) >= VIA_VQ_SIZE) &&
	pVia->VQEnable) {
	/* Reserved space for VQ */
	pVia->VQStart = pVia->FBFreeEnd - VIA_VQ_SIZE;
	pVia->VQEnd = pVia->VQStart + VIA_VQ_SIZE - 1;
	pVia->FBFreeEnd -= VIA_VQ_SIZE;
    } 

    viaInitialize2DEngine(pScrn);
    
    if (pVia->hwcursor) {
	pVia->FBFreeEnd -= VIA_CURSOR_SIZE;
	pVia->CursorStart = pVia->FBFreeEnd;
    } 
    
#ifdef VIA_HAVE_EXA

    /*
     * Sync marker space.
     */

    pVia->FBFreeEnd -= 32;
    pVia->markerOffset = (pVia->FBFreeEnd + 31) & ~31;
    pVia->markerBuf = (CARD32 *) ((char *) pVia->FBBase + pVia->markerOffset);
    
    viaSetupCBuffer(pScrn, &pVia->cb, 0);

    if (pVia->useEXA) {
	pVia->exaDriverPtr = viaInitExa(pScreen);
	if (!pVia->exaDriverPtr) {

	    /*
	     * Docs recommend turning off also Xv here, but we handle this
	     * case with the old linear offscreen FB manager through
	     * VIAInitLinear.
	     */

	    pVia->NoAccel = TRUE;
	    return FALSE;
	}
	return TRUE;
    } 
#endif

    AvailFBArea.x1 = 0;
    AvailFBArea.y1 = 0;
    AvailFBArea.x2 = pScrn->virtualX;
    AvailFBArea.y2 = pVia->FBFreeEnd / pVia->Bpl;

    xf86InitFBManager(pScreen, &AvailFBArea);
    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		     "Using %d lines for offscreen memory.\n",
		     AvailFBArea.y2 - pScrn->virtualY ));

    return viaInitXAA(pScreen);
}

