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
 *  File:       via_cursor.c
 *  Content:    Hardware cursor support for VIA/S3G UniChrome
 *
 ************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "via.h"
#include "via_driver.h"
#include "cursorstr.h"

static void VIALoadCursorImage(ScrnInfoPtr pScrn, unsigned char *src);
static void VIASetCursorPosition(ScrnInfoPtr pScrn, int x, int y);
static void VIASetCursorColors(ScrnInfoPtr pScrn, int bg, int fg);
static Bool VIAUseHWCursor(ScreenPtr pScreen, CursorPtr pCurs);

static void VIAShowCursorARGB(ScrnInfoPtr pScrn);
static void VIAHideCursorARGB(ScrnInfoPtr pScrn);
static void VIASetCursorPositionARGB(ScrnInfoPtr pScrn, int x, int y);
static void VIALoadCursorARGB(ScrnInfoPtr pScrn, CursorPtr pCurs);
static Bool VIAUseHWCursorARGB(ScreenPtr pScreen, CursorPtr pCurs);

#define MAX_CURS 32

Bool
VIAHWCursorInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    xf86CursorInfoPtr infoPtr;
    CARD32 temp;

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "VIAHWCursorInit\n"));
    infoPtr = xf86CreateCursorInfoRec();
    if (!infoPtr)
        return FALSE;

    pVia->CursorInfoRec = infoPtr;

    infoPtr->MaxWidth = MAX_CURS;
    infoPtr->MaxHeight = MAX_CURS;
    infoPtr->Flags = (HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_64 |
                      HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
                      /*HARDWARE_CURSOR_SWAP_SOURCE_AND_MASK | */
                      HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
                      HARDWARE_CURSOR_INVERT_MASK |
                      HARDWARE_CURSOR_BIT_ORDER_MSBFIRST |
                      0);

    infoPtr->SetCursorColors = VIASetCursorColors;
    infoPtr->SetCursorPosition = VIASetCursorPositionARGB;
    infoPtr->LoadCursorImage = VIALoadCursorImage;
    infoPtr->HideCursor = VIAHideCursorARGB;
    infoPtr->ShowCursor = VIAShowCursorARGB;
    infoPtr->UseHWCursor = VIAUseHWCursor;

    infoPtr->UseHWCursorARGB = VIAUseHWCursorARGB;
    infoPtr->LoadCursorARGB = VIALoadCursorARGB;

    if (!pVia->CursorStart) {
        pVia->CursorStart = pVia->FBFreeEnd - VIA_CURSOR_SIZE;
        pVia->FBFreeEnd -= VIA_CURSOR_SIZE;
    }

    /* Set cursor location in frame buffer. */
    VIASETREG(VIA_REG_CURSOR_MODE, pVia->CursorStart);

    /* HI hack */
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "HAXOR\n");
    VIASETREG(0x2F4, pVia->CursorStart); // VIA_REG_HI_BASE0
    VIASETREG(VIA_REG_HI_CONTROL0, 0);
    VIASETREG(0x2E8, 0x0D000D0F); // VIA_FIFO
    //VIASETREG(0x2E8, 0);
    VIASETREG(0x2EC, 0); // TRANSPARENT_KEY
    VIASETREG(0x120C, 0x00FFFFFF); // VIA_REG_PRIM_HI_INVTCOLOR
    VIASETREG(0x2E4, 0x00FFFFFF); // VIA_REG_V327_HI_INVTCOLOR
    VIASETREG(VIA_REG_HI_POS0, 0);

    return xf86InitCursor(pScreen, infoPtr);
}

/*
 * Lets not use old monochrome cursor
 */
static Bool
VIAUseHWCursor(ScreenPtr pScreen, CursorPtr pCurs)
{
    return FALSE;
}

void
VIAShowCursor(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 dwCursorMode;

    dwCursorMode = VIAGETREG(VIA_REG_CURSOR_MODE);

    /* Turn on hardware cursor. */
    VIASETREG(VIA_REG_CURSOR_MODE, dwCursorMode | 0x3);
}


void
VIAHideCursor(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 dwCursorMode;

    dwCursorMode = VIAGETREG(VIA_REG_CURSOR_MODE);

    /* Turn cursor off. */
    VIASETREG(VIA_REG_CURSOR_MODE, dwCursorMode & 0xFFFFFFFE);
}


static void
VIALoadCursorImage(ScrnInfoPtr pScrn, unsigned char *src)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 dwCursorMode;

    viaAccelSync(pScrn);

    dwCursorMode = VIAGETREG(VIA_REG_CURSOR_MODE);

    /* Turn cursor off. */
    VIASETREG(VIA_REG_CURSOR_MODE, dwCursorMode & 0xFFFFFFFE);

    /* Upload the cursor image to the frame buffer. */
    memcpy(pVia->FBBase + pVia->CursorStart, src, MAX_CURS * MAX_CURS / 8 * 2);

    /* Restore cursor status */
    VIASETREG(VIA_REG_CURSOR_MODE, dwCursorMode);
}

static void
VIASetCursorPosition(ScrnInfoPtr pScrn, int x, int y)
{
    VIAPtr pVia = VIAPTR(pScrn);
    VIABIOSInfoPtr pBIOSInfo = pVia->pBIOSInfo;
    unsigned char xoff, yoff;
    CARD32 dwCursorMode;

    if (x < 0) {
        xoff = ((-x) & 0xFE);
        x = 0;
    } else {
        xoff = 0;
    }

    if (y < 0) {
        yoff = ((-y) & 0xFE);
        y = 0;
    } else {
        yoff = 0;
        /* LCD Expand Mode Cursor Y Position Re-Calculated */
        if (pBIOSInfo->scaleY) {
            y = (int)(((pBIOSInfo->panelY * y) + (pBIOSInfo->resY >> 1))
                      / pBIOSInfo->resY);
        }
    }

    /* Hide cursor before set cursor position in order to avoid ghost cursor
     * image when directly set cursor position. It should be a HW bug but
     * we can use patch by SW. */
    dwCursorMode = VIAGETREG(VIA_REG_CURSOR_MODE);

    /* Turn cursor off. */
    VIASETREG(VIA_REG_CURSOR_MODE, dwCursorMode & 0xFFFFFFFE);

    VIASETREG(VIA_REG_CURSOR_ORG, ((xoff << 16) | (yoff & 0x003f)));
    VIASETREG(VIA_REG_CURSOR_POS, ((x << 16) | (y & 0x07ff)));

    /* Restore cursor status */
    VIASETREG(VIA_REG_CURSOR_MODE, dwCursorMode);
}


static void
VIASetCursorColors(ScrnInfoPtr pScrn, int bg, int fg)
{
    VIAPtr pVia = VIAPTR(pScrn);

    VIASETREG(VIA_REG_CURSOR_FG, fg);
    VIASETREG(VIA_REG_CURSOR_BG, bg);
}

void
ViaCursorStore(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "ViaCursorStore\n"));

    if (pVia->CursorImage) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "ViaCursorStore: stale image left.\n");
        xfree(pVia->CursorImage);
    }

    pVia->CursorImage = xcalloc(1, 0x1000);
    memcpy(pVia->CursorImage, pVia->FBBase + pVia->CursorStart, 0x1000);
    pVia->CursorFG = (CARD32) VIAGETREG(VIA_REG_CURSOR_FG);
    pVia->CursorBG = (CARD32) VIAGETREG(VIA_REG_CURSOR_BG);
    pVia->CursorMC = (CARD32) VIAGETREG(VIA_REG_CURSOR_MODE);
}

void
ViaCursorRestore(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "ViaCursorRestore\n"));

    if (pVia->CursorImage) {
        memcpy(pVia->FBBase + pVia->CursorStart, pVia->CursorImage, 0x1000);
        VIASETREG(VIA_REG_CURSOR_FG, pVia->CursorFG);
        VIASETREG(VIA_REG_CURSOR_BG, pVia->CursorBG);
        VIASETREG(VIA_REG_CURSOR_MODE, pVia->CursorMC);
        xfree(pVia->CursorImage);
        pVia->CursorImage = NULL;
    } else
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "ViaCursorRestore: No cursor image stored.\n");
}

static void
VIAShowCursorARGB(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%s\n", __FUNCTION__);
    temp = VIAGETREG(VIA_REG_HI_CONTROL0);
    temp = 0;
    temp |=
	(1 << 29) |
	(1 << 28) |
	(1 << 26) |
	(1 << 25) |
	(0xF << 16)  |
	(0xF << 4) |
	(1 << 2) |
	(1 << 0);
    VIASETREG(VIA_REG_HI_CONTROL0, 0x76000005);
}

static void
VIAHideCursorARGB(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%s\n", __FUNCTION__);

    temp = VIAGETREG(VIA_REG_HI_CONTROL0);
    VIASETREG(VIA_REG_HI_CONTROL0, temp & 0xFFFFFFFE);
}

static void
VIASetCursorPositionARGB(ScrnInfoPtr pScrn, int x, int y)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%s, (%i, %i)\n", __FUNCTION__, x, y);

    if (x < 0)
	x = 0;

    if (y < 0)
	y = 0;

    temp = VIAGETREG(VIA_REG_HI_CONTROL0);
    VIASETREG(VIA_REG_HI_CONTROL0, temp & 0xFFFFFFFA);
    VIASETREG(0x2F8, ((x << 16) | (y & 0x07ff))); // VIA_REG_HI_POS0
    VIASETREG(0x20C, ((0<< 16) | (0 & 0x07ff)));
    VIASETREG(VIA_REG_HI_CONTROL0, temp);
}

Bool
VIAUseHWCursorARGB(ScreenPtr pScreen, CursorPtr pCurs)
{
    //xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%s\n", __FUNCTION__);
    return TRUE;
}

void
VIALoadCursorARGB(ScrnInfoPtr pScrn, CursorPtr pCurs)
{
    VIAPtr pVia = VIAPTR(pScrn);
    int x, y, w, h;
    CARD32 *image;
    CARD32 *dst;
    CARD32 temp;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%s\n", __FUNCTION__);

    /* Turn off the cursor */
    temp = VIAGETREG(VIA_REG_HI_CONTROL0);
    VIASETREG(VIA_REG_HI_CONTROL0, temp & 0xFFFFFFFA);
    VIASETREG(VIA_REG_HI_CONTROL1, temp & 0xFFFFFFFA);
    dst = (CARD32*)(pVia->FBBase + pVia->CursorStart);
    image = pCurs->bits->argb;

    memset(dst, 0xFF, VIA_CURSOR_SIZE);
    return;
    w = pCurs->bits->width;
    if (w > MAX_CURS)
	w = MAX_CURS;

    h = pCurs->bits->height;
    if (h > MAX_CURS)
	h = MAX_CURS;

    for (y = 0; y < h; y++) {
	for (x = 0; x < w; x++)
	    *dst++ = *image;
	for (; x < MAX_CURS; x++)
	    *dst++ = 0;
    }

    for (; y < MAX_CURS; y++)
	for (x = 0; x < MAX_CURS; x++)
	    *dst++ = 0;

    /* Restore cursor */
//    VIASETREG(VIA_REG_HI_BASE0, pVia->CursorStart);
//    VIASETREG(VIA_REG_HI_CONTROL0, temp);
}
