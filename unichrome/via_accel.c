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

/* Forward declaration of functions used in the driver */

static void VIASetupForScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int xdir,
    int ydir,
    int rop,
    unsigned planemask,
    int trans_color);

static void VIASubsequentScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int w,
    int h);

static void VIASetupForSolidFill(
    ScrnInfoPtr pScrn,
    int color,
    int rop,
    unsigned planemask);

static void VIASubsequentSolidFillRect(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h);

static void VIASetupForMono8x8PatternFill(
    ScrnInfoPtr pScrn,
    int pattern0,
    int pattern1,
    int fg,
    int bg,
    int rop,
    unsigned planemask);

static void VIASubsequentMono8x8PatternFillRect(
    ScrnInfoPtr pScrn,
    int patOffx,
    int patOffy,
    int x,
    int y,
    int w,
    int h);

static void VIASetupForColor8x8PatternFill(
    ScrnInfoPtr pScrn,
    int patternx,
    int patterny,
    int rop,
    unsigned planemask,
    int trans_color);

static void VIASubsequentColor8x8PatternFillRect(
    ScrnInfoPtr pScrn,
    int patOffx,
    int patOffy,
    int x,
    int y,
    int w,
    int h);

static void VIASetupForCPUToScreenColorExpandFill(
    ScrnInfoPtr pScrn,
    int fg,
    int bg,
    int rop,
    unsigned planemask);

static void VIASubsequentScanlineCPUToScreenColorExpandFill(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h,
    int skipleft);

static void VIASetupForImageWrite(
    ScrnInfoPtr pScrn,
    int rop,
    unsigned planemask,
    int trans_color,
    int bpp,
    int depth);

static void VIASubsequentImageWriteRect(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h,
    int skipleft);

static void
VIASetupForSolidLine(
    ScrnInfoPtr pScrn,
    int color,
    int rop,
    unsigned int planemask);

static void
VIASubsequentSolidTwoPointLine(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int flags);

static void
VIASubsequentSolidHorVertLine(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int len,
    int dir);

static void
VIASetupForDashedLine(
    ScrnInfoPtr pScrn,
    int fg,
    int bg,
    int rop,
    unsigned int planemask,
    int length,
    unsigned char *pattern);

static void
VIASubsequentDashedTwoPointLine(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int flags,
    int phase);


static void VIASetClippingRectangle(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2);

static void VIADisableClipping( ScrnInfoPtr );

void
VIAInitialize2DEngine(ScrnInfoPtr pScrn)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    CARD32  dwVQStartAddr, dwVQEndAddr;
    CARD32  dwVQLen, dwVQStartL, dwVQEndL, dwVQStartEndH;
    CARD32  dwGEMode;
    ViaTwodContext *tdc = &pVia->td;


    /* init 2D engine regs to reset 2D engine */
    VIASETREG(0x04, 0x0);
    VIASETREG(0x08, 0x0);
    VIASETREG(0x0c, 0x0);
    VIASETREG(0x10, 0x0);
    VIASETREG(0x14, 0x0);
    VIASETREG(0x18, 0x0);
    VIASETREG(0x1c, 0x0);
    VIASETREG(0x20, 0x0);
    VIASETREG(0x24, 0x0);
    VIASETREG(0x28, 0x0);
    VIASETREG(0x2c, 0x0);
    VIASETREG(0x30, 0x0);
    VIASETREG(0x34, 0x0);
    VIASETREG(0x38, 0x0);
    VIASETREG(0x3c, 0x0);
    VIASETREG(0x40, 0x0);

    /* Init AGP and VQ regs */
    VIASETREG(0x43c, 0x00100000);
    VIASETREG(0x440, 0x00000000);
    VIASETREG(0x440, 0x00333004);
    VIASETREG(0x440, 0x60000000);
    VIASETREG(0x440, 0x61000000);
    VIASETREG(0x440, 0x62000000);
    VIASETREG(0x440, 0x63000000);
    VIASETREG(0x440, 0x64000000);
    VIASETREG(0x440, 0x7D000000);

    VIASETREG(0x43c, 0xfe020000);
    VIASETREG(0x440, 0x00000000);

    if (pVia->VQStart != 0) {
        /* Enable VQ */
        dwVQStartAddr = pVia->VQStart;
        dwVQEndAddr = pVia->VQEnd;
        dwVQStartL = 0x50000000 | (dwVQStartAddr & 0xFFFFFF);
        dwVQEndL = 0x51000000 | (dwVQEndAddr & 0xFFFFFF);
        dwVQStartEndH = 0x52000000 | ((dwVQStartAddr & 0xFF000000) >> 24) |
                        ((dwVQEndAddr & 0xFF000000) >> 16);
        dwVQLen = 0x53000000 | (VIA_VQ_SIZE >> 3);

        VIASETREG(0x43c, 0x00fe0000);
        VIASETREG(0x440, 0x080003fe);
        VIASETREG(0x440, 0x0a00027c);
        VIASETREG(0x440, 0x0b000260);
        VIASETREG(0x440, 0x0c000274);
        VIASETREG(0x440, 0x0d000264);
        VIASETREG(0x440, 0x0e000000);
        VIASETREG(0x440, 0x0f000020);
        VIASETREG(0x440, 0x1000027e);
        VIASETREG(0x440, 0x110002fe);
        VIASETREG(0x440, 0x200f0060);

        VIASETREG(0x440, 0x00000006);
        VIASETREG(0x440, 0x40008c0f);
        VIASETREG(0x440, 0x44000000);
        VIASETREG(0x440, 0x45080c04);
        VIASETREG(0x440, 0x46800408);

        VIASETREG(0x440, dwVQStartEndH);
        VIASETREG(0x440, dwVQStartL);
        VIASETREG(0x440, dwVQEndL);
        VIASETREG(0x440, dwVQLen);
    }
    else {
        /* Diable VQ */
        VIASETREG(0x43c, 0x00fe0000);
        VIASETREG(0x440, 0x00000004);
        VIASETREG(0x440, 0x40008c0f);
        VIASETREG(0x440, 0x44000000);
        VIASETREG(0x440, 0x45080c04);
        VIASETREG(0x440, 0x46800408);
    }

    dwGEMode = 0;

    switch (pScrn->bitsPerPixel) {
    case 16:
        dwGEMode |= VIA_GEM_16bpp;
        break;
    case 32:
        dwGEMode |= VIA_GEM_32bpp;
	break;
    default:
        dwGEMode |= VIA_GEM_8bpp;
        break;
    }

    /* Set BPP and Pitch */
    tdc->mode = dwGEMode;
}


/* Acceleration init function, sets up pointers to our accelerated functions */
Bool
VIAInitAccel(ScreenPtr pScreen)
{
    ScrnInfoPtr     pScrn = xf86Screens[pScreen->myNum];
    VIAPtr          pVia = VIAPTR(pScrn);
    XAAInfoRecPtr   xaaptr;
    BoxRec 	    AvailFBArea;
    unsigned long   cacheEnd;
    unsigned long   cacheEndTmp;
    

    pVia->VQStart = 0;
    if (((pVia->FBFreeEnd - pVia->FBFreeStart) >= VIA_VQ_SIZE) &&
        pVia->VQEnable) {
        /* Reserved space for VQ */
        pVia->VQStart = pVia->FBFreeEnd - VIA_VQ_SIZE;
        pVia->VQEnd = pVia->VQStart + VIA_VQ_SIZE - 1;
        pVia->FBFreeEnd -= VIA_VQ_SIZE;
    } /* FIXME: Else disable VQ */
    
    if (pVia->hwcursor) {
	pVia->FBFreeEnd -= VIA_CURSOR_SIZE;
	pVia->CursorStart = pVia->FBFreeEnd;
    } /* FIXME: Else disable hwcursor */
    
    /*
     * Sync marker code.
     */

    pVia->FBFreeEnd -= 64;
    pVia->markerOffset = (pVia->FBFreeEnd + 31) & 31;
    pVia->markerBuf = (CARD32 *) pVia->FBBase + (pVia->markerOffset >> 2);
    
    VIAInitialize2DEngine(pScrn);

    if (pScrn->depth == 8) {
        pVia->PlaneMask = 0xFF;
    }
    else if (pScrn->depth == 15) {
        pVia->PlaneMask = 0x7FFF;
    }
    else if (pScrn->depth == 16) {
        pVia->PlaneMask = 0xFFFF;
    }
    else if (pScrn->depth == 24) {
        pVia->PlaneMask = 0xFFFFFF;
    }

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
    xaaptr->SetClippingRectangle = VIASetClippingRectangle;
    xaaptr->DisableClipping = VIADisableClipping;
    xaaptr->ClippingFlags = HARDWARE_CLIP_SOLID_FILL |
	HARDWARE_CLIP_SOLID_LINE |
	HARDWARE_CLIP_DASHED_LINE |
	HARDWARE_CLIP_SCREEN_TO_SCREEN_COPY |
	HARDWARE_CLIP_MONO_8x8_FILL |
	HARDWARE_CLIP_COLOR_8x8_FILL |
	HARDWARE_CLIP_SCREEN_TO_SCREEN_COLOR_EXPAND |
	0;

    xaaptr->Sync = VIAAccelSync;

    /* ScreenToScreen copies */
    xaaptr->SetupForScreenToScreenCopy = VIASetupForScreenToScreenCopy;
    xaaptr->SubsequentScreenToScreenCopy = VIASubsequentScreenToScreenCopy;
    xaaptr->ScreenToScreenCopyFlags = NO_PLANEMASK | ROP_NEEDS_SOURCE;

    /* Solid filled rectangles */
    xaaptr->SetupForSolidFill = VIASetupForSolidFill;
    xaaptr->SubsequentSolidFillRect = VIASubsequentSolidFillRect;
    xaaptr->SolidFillFlags = NO_PLANEMASK | ROP_NEEDS_SOURCE;

    /* Mono 8x8 pattern fills */
    xaaptr->SetupForMono8x8PatternFill = VIASetupForMono8x8PatternFill;
    xaaptr->SubsequentMono8x8PatternFillRect =
	VIASubsequentMono8x8PatternFillRect;
    xaaptr->Mono8x8PatternFillFlags = NO_PLANEMASK |
	HARDWARE_PATTERN_PROGRAMMED_BITS |
	HARDWARE_PATTERN_PROGRAMMED_ORIGIN |
	BIT_ORDER_IN_BYTE_MSBFIRST |
	0;

    /* Color 8x8 pattern fills */
    xaaptr->SetupForColor8x8PatternFill = VIASetupForColor8x8PatternFill;
    xaaptr->SubsequentColor8x8PatternFillRect =
	VIASubsequentColor8x8PatternFillRect;
    xaaptr->Color8x8PatternFillFlags = NO_PLANEMASK |
	NO_TRANSPARENCY |
	HARDWARE_PATTERN_PROGRAMMED_BITS |
	HARDWARE_PATTERN_PROGRAMMED_ORIGIN |
	0;

    /* Solid lines */
    xaaptr->SetupForSolidLine = VIASetupForSolidLine;
    xaaptr->SubsequentSolidTwoPointLine = VIASubsequentSolidTwoPointLine;
    xaaptr->SubsequentSolidHorVertLine = VIASubsequentSolidHorVertLine;
    xaaptr->SolidBresenhamLineErrorTermBits = 14;
    xaaptr->SolidLineFlags = NO_PLANEMASK | ROP_NEEDS_SOURCE;

    /* dashed line */
    xaaptr->SetupForDashedLine = VIASetupForDashedLine;
    xaaptr->SubsequentDashedTwoPointLine = VIASubsequentDashedTwoPointLine;
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
	VIASetupForCPUToScreenColorExpandFill;
	xaaptr->SubsequentScanlineCPUToScreenColorExpandFill = 
	VIASubsequentScanlineCPUToScreenColorExpandFill;
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

    xaaptr->SetupForImageWrite = VIASetupForImageWrite;
    xaaptr->SubsequentImageWriteRect = VIASubsequentImageWriteRect;
    xaaptr->ImageWriteBase = pVia->BltBase;
    xaaptr->ImageWriteRange = VIA_MMIO_BLTSIZE;

    /* We reserve space for pixel cache */
    
    cacheEnd = pVia->FBFreeEnd / pVia->Bpl;
    cacheEndTmp = (pVia->FBFreeStart + VIA_PIXMAP_CACHE_SIZE + pVia->Bpl-1) 
	/ pVia->Bpl;
    
    /*
     *	Use only requested pixmap size if it is less than available
     *  offscreen memory.
     */

    if(cacheEnd > cacheEndTmp)
        cacheEnd = cacheEndTmp;
    /*
     *	Clip to the blitter limit
     */

    if (cacheEnd > VIA_MAX_ACCEL_Y)
	cacheEnd = VIA_MAX_ACCEL_Y;

    pVia->FBFreeStart = (cacheEnd + 1) *pVia->Bpl;

    /*
     * Finally, we set up the video memory space available to the pixmap
     * cache
     */

    AvailFBArea.x1 = 0;
    AvailFBArea.y1 = 0;
    AvailFBArea.x2 = pScrn->virtualX;
    AvailFBArea.y2 = cacheEnd;

    xf86InitFBManager(pScreen, &AvailFBArea);
    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		     "Using %d lines for offscreen memory.\n",
		     AvailFBArea.y2 - pScrn->virtualY ));

    viaSetupCBuffer(pScrn, &pVia->cb, 0);

    return XAAInit(pScreen, xaaptr);
}

/*
 * The sync function for the GE
 */
void 
VIAAccelSync(ScrnInfoPtr pScrn)
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

#ifdef X_HAVE_XAAGETROP			
#define VIAACCELPATTERNROP(vRop) (XAAGetPatternROP(vRop) << 24)
#define VIAACCELCOPYROP(vRop) (XAAGetCopyROP(vRop) << 24)
#else
#define VIAACCELPATTERNROP(vRop) (XAAPatternROP[vRop] << 24)
#define VIAACCELCOPYROP(vRop) (XAACopyROP[vRop] << 24)
#endif


static void
viaAccelClippingHelper(ViaCommandBuffer *cb, int refY, ViaTwodContext *tdc)
{   
    if (tdc->clipping) {
	tdc->cmd |= VIA_GEC_CLIP_ENABLE;
	BEGIN_RING_AGP(cb, 4);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_CLIPTL), ((tdc->clipY1 - refY) << 16) | tdc->clipX1);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_CLIPBR), ((tdc->clipY2 - refY) << 16) | tdc->clipX2);
    } else {
	tdc->cmd &= ~VIA_GEC_CLIP_ENABLE;
    }
}

static void 
viaAccelSolidHelper(ViaCommandBuffer *cb, int x, int y, int w, int h,
		    unsigned fbBase, CARD32 mode, unsigned pitch, CARD32 fg,
		    CARD32 cmd)
{
    unsigned
	dstBase  = fbBase + pitch * y;

    BEGIN_RING_AGP(cb, 14); 
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), (pitch >> 3) << 16);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), x);
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
    unsigned
	srcBase = srcFbBase + srcPitch*ys,
	dstBase = dstFbBase + dstPitch*yd;

    ys = (cmd & VIA_GEC_DECY) ? (h-1) : 0;
    yd = ys;

    if (cmd & VIA_GEC_DECX) {
	xs += (w - 1);
	xd += (w - 1);
    }
    
    BEGIN_RING_AGP(cb, 16);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCBASE), srcBase >> 3);    
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), ((dstPitch >> 3) << 16) | (srcPitch >> 3));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCPOS), (ys << 16) | xs);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), (yd << 16) | xd);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), ((h - 1) << 16) | (w - 1));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), cmd);
}
    


/* These are the ScreenToScreen bitblt functions. We support all ROPs, all
 * directions.
 *
 */

static void
VIASetupForScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int xdir,
    int ydir,
    int rop,
    unsigned planemask,
    int trans_color)
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
VIASubsequentScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int w,
    int h)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    if (!w || !h)
        return;

    viaAccelClippingHelper(cb, y2, tdc);
    viaAccelCopyHelper(cb, x1, y1, x2, y2, w, h, pScrn->fbOffset, pScrn->fbOffset, tdc->mode,
		       pVia->Bpl, pVia->Bpl, tdc->cmd);
    cb->flushFunc(cb);
}


/*
 * SetupForSolidFill is also called to set up for lines.
 */

static void
VIASetupForSolidFill(
    ScrnInfoPtr pScrn,
    int color,
    int rop,
    unsigned planemask)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    tdc->cmd = VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT | 
      VIAACCELPATTERNROP(rop);
    tdc->fgColor = color;
}


static void
VIASubsequentSolidFillRect(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    if (!w || !h)
        return;

    viaAccelClippingHelper(cb, y, tdc);
    viaAccelSolidHelper(cb, x, y, w, h, pScrn->fbOffset, tdc->mode,
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
VIASetupForMono8x8PatternFill(
    ScrnInfoPtr pScrn,
    int pattern0,
    int pattern1,
    int fg,
    int bg,
    int rop,
    unsigned planemask)
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
VIASubsequentMono8x8PatternFillRect(
    ScrnInfoPtr pScrn,
    int patOffx,
    int patOffy,
    int x,
    int y,
    int w,
    int h)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    CARD32  patOffset;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase;

    if (!w || !h)
        return;

    patOffset = ((patOffy & 0x7)  << 29) | ((patOffx & 0x7) << 26);
    dstBase = pScrn->fbOffset + y*pVia->Bpl;
    
    viaAccelClippingHelper(cb, y, tdc);
    BEGIN_RING_AGP(cb,11);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), tdc->mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
	    ((pVia->Bpl >> 3) << 16));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), x);
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
VIASetupForColor8x8PatternFill(
    ScrnInfoPtr pScrn,
    int patternx,
    int patterny,
    int rop,
    unsigned planemask,
    int trans_color)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    tdc->cmd = VIA_GEC_BLT | VIAACCELPATTERNROP(rop);
    tdc->patternAddr = (patternx * pVia->Bpp + patterny * pVia->Bpl);
}


static void
VIASubsequentColor8x8PatternFillRect(
    ScrnInfoPtr pScrn,
    int patOffx,
    int patOffy,
    int x,
    int y,
    int w,
    int h)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    CARD32  patAddr;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase;

    if (!w || !h)
        return;

    patAddr = (tdc->patternAddr >> 3) |
	((patOffy & 0x7)  << 29) | ((patOffx & 0x7) << 26);
    dstBase = pScrn->fbOffset + y*pVia->Bpl;

    viaAccelClippingHelper(cb, y, tdc);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), tdc->mode);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
	    ((pVia->Bpl >> 3) << 16));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), x);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), (((h - 1) << 16) | (w - 1)));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PATADDR), patAddr);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), tdc->cmd);
    cb->flushFunc(cb);
}

static void
VIASetupForCPUToScreenColorExpandFill(
    ScrnInfoPtr pScrn,
    int fg,
    int bg,
    int rop,
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
VIASubsequentScanlineCPUToScreenColorExpandFill(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h,
    int skipleft)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    if (skipleft) {
        VIASetClippingRectangle(pScrn, (x + skipleft), y, (x + w - 1), (y + h -1));
    }

    viaAccelClippingHelper(cb, y, tdc);
    BEGIN_RING_AGP(cb, 4); 
    OUT_RING_QW_AGP(cb,H1_ADDR(VIA_REG_BGCOLOR), tdc->bgColor);
    OUT_RING_QW_AGP(cb,H1_ADDR(VIA_REG_FGCOLOR), tdc->bgColor);
    viaAccelCopyHelper(cb, 0, 0, x, y, w, h, 0, pScrn->fbOffset, tdc->mode, pVia->Bpl,
		       pVia->Bpl, tdc->cmd);

    /*
     * Can't use AGP for CPU to screen actions.
     */

    viaFlushPCI(cb);
}

static void
VIASetupForImageWrite(
    ScrnInfoPtr pScrn,
    int rop,
    unsigned planemask,
    int trans_color,
    int bpp,
    int depth)
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
VIASubsequentImageWriteRect(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h,
    int skipleft)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;

    if (skipleft) {
        VIASetClippingRectangle(pScrn, (x + skipleft), y, (x + w - 1), (y + h -1));
    }

    viaAccelClippingHelper(cb, y, tdc);
    viaAccelCopyHelper(cb, 0, 0, x, y, w, h, 0, pScrn->fbOffset, tdc->mode, pVia->Bpl,
		       pVia->Bpl, tdc->cmd);
    /*
     * Can't use AGP for CPU to screen actions.
     */

    viaFlushPCI(cb);
}


/* Setup for XAA solid lines. */
static void
VIASetupForSolidLine(
    ScrnInfoPtr pScrn,
    int color,
    int rop,
    unsigned int planemask)
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
VIASubsequentSolidTwoPointLine(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int flags)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    int     dx, dy, cmd, tmp, error = 1;
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase;

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

    if (y1 < y2) {
      dstBase = pScrn->fbOffset + y1*pVia->Bpl;
      y1 = 0;
      y2 -= y1;
      viaAccelClippingHelper(cb, y1, tdc);
    } else {
      dstBase = pScrn->fbOffset + y2*pVia->Bpl;
      y1 -= y2;
      y2 = 0;
      viaAccelClippingHelper(cb, y2, tdc);
    }

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
VIASubsequentSolidHorVertLine(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int len,
    int dir)
{
    VIAPtr  pVia = VIAPTR(pScrn);
    ViaCommandBuffer *cb = &pVia->cb;
    ViaTwodContext *tdc = &pVia->td;
    CARD32 dstBase = pScrn->fbOffset + y*pVia->Bpl;
    
    viaAccelClippingHelper(cb, y, tdc);
    BEGIN_RING_AGP(cb, 10);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), dstBase >> 3);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
		    ((pVia->Bpl >> 3) << 16));
    
    if (dir == DEGREES_0) {
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), x);
        OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), (len - 1));
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), tdc->cmd | VIA_GEC_BLT);
    } else {
        OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), x);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION), ((len - 1) << 16));
        OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), tdc->cmd | VIA_GEC_BLT);
    }
    cb->flushFunc(cb);
}

static void
VIASetupForDashedLine(
    ScrnInfoPtr pScrn,
    int fg,
    int bg,
    int rop,
    unsigned int planemask,
    int length,
    unsigned char *pattern)
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
VIASubsequentDashedTwoPointLine(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int flags,
    int phase)
{
    VIASubsequentSolidTwoPointLine(pScrn, x1, y1, x2, y2, flags);
}

static void
VIASetClippingRectangle(
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
VIADisableClipping(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    ViaTwodContext *tdc = &pVia->td;

    tdc->clipping = FALSE;
}

/*
 *
 */
void 
ViaVQDisable(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    VIASETREG(VIA_REG_TRANSET, 0x00FE0000);
    VIASETREG(VIA_REG_TRANSPACE, 0x00000004);
}
