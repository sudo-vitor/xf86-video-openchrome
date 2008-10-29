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
#include "via_id.h"
#include "cursorstr.h"

void VIAShowCursor(ScrnInfoPtr pScrn);
void VIAHideCursor(ScrnInfoPtr pScrn);
static void VIASetCursorPosition(ScrnInfoPtr pScrn, int x, int y);
static Bool VIAUseHWCursor(ScreenPtr pScreen, CursorPtr pCurs);
static Bool VIAUseHWCursorARGB(ScreenPtr pScreen, CursorPtr pCurs);
static void VIALoadCursorImage(ScrnInfoPtr pScrn, unsigned char *src);
static void VIASetCursorColors(ScrnInfoPtr pScrn, int bg, int fg);
static void VIALoadCursorARGB(ScrnInfoPtr pScrn, CursorPtr pCurs);

#define MAX_CURS 64

static CARD32 mono_cursor_color[] = {
	0x00000000,
	0x00000000,
	0xffffffff,
	0xff000000,
};

Bool
VIAHWCursorInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VIAPtr pVia = VIAPTR(pScrn);
    xf86CursorInfoPtr infoPtr;
    CARD32 temp;

    if (pVia->Chipset != VIA_CX700)
	return FALSE;

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "VIAHWCursorInit\n"));
    infoPtr = xf86CreateCursorInfoRec();
    if (!infoPtr)
        return FALSE;

    pVia->CursorInfoRec = infoPtr;

    infoPtr->MaxWidth = MAX_CURS;
    infoPtr->MaxHeight = MAX_CURS;
    infoPtr->Flags = (HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_1 |
                      HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
                      HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
                      0);

    infoPtr->SetCursorColors = VIASetCursorColors;
    infoPtr->SetCursorPosition = VIASetCursorPosition;
    infoPtr->LoadCursorImage = VIALoadCursorImage;
    infoPtr->HideCursor = VIAHideCursor;
    infoPtr->ShowCursor = VIAShowCursor;
    infoPtr->UseHWCursor = VIAUseHWCursor;

    infoPtr->UseHWCursorARGB = VIAUseHWCursorARGB;
    infoPtr->LoadCursorARGB = VIALoadCursorARGB;

    if (!pVia->CursorStart) {
        pVia->CursorStart = pVia->FBFreeEnd - VIA_CURSOR_SIZE;
        pVia->FBFreeEnd -= VIA_CURSOR_SIZE;
    }

    /* Set cursor location in frame buffer. */
    VIASETREG(VIA_REG_CURSOR_MODE, pVia->CursorStart);

    pVia->CursorPipe = 0;
    /* Init HI_X0 */
    VIASETREG(VIA_REG_HI_CONTROL0, 0);
    VIASETREG(VIA_REG_HI_BASE0, pVia->CursorStart);
    VIASETREG(0x2E8, 0x0D000D0F); // VIA_FIFO
    VIASETREG(0x2EC, 0); // TRANSPARENT_KEY
    VIASETREG(0x120C, 0x00FFFFFF); // VIA_REG_PRIM_HI_INVTCOLOR
    VIASETREG(0x2E4, 0x00FFFFFF); // VIA_REG_V327_HI_INVTCOLOR

    return xf86InitCursor(pScreen, infoPtr);
}



/* TODO deprecated */
void
ViaCursorStore(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "ViaCursorStore\n"));

    pVia->CursorControl0 = VIAGETREG(VIA_REG_HI_CONTROL0);
    pVia->CursorHiBase0 = VIAGETREG(VIA_REG_HI_BASE0);
    pVia->CursorFifo = VIAGETREG(0x2E8); // VIA_FIFO
    pVia->CursorTransparentKey = VIAGETREG(0x2EC); // TRANSPARENT_KEY
    pVia->CursorPrimHiInvtColor = VIAGETREG(0x120C); // VIA_REG_PRIM_HI_INVTCOLOR
    pVia->CursorV327HiInvtColor = VIAGETREG(0x2E4); // VIA_REG_V327_HI_INVTCOLOR

/*    if (pVia->CursorImage) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "ViaCursorStore: stale image left.\n");
        xfree(pVia->CursorImage);
    }

    pVia->CursorImage = xcalloc(1, 0x1000);
    memcpy(pVia->CursorImage, pVia->FBBase + pVia->CursorStart, 0x1000);
    pVia->CursorFG = (CARD32) VIAGETREG(VIA_REG_CURSOR_FG);
    pVia->CursorBG = (CARD32) VIAGETREG(VIA_REG_CURSOR_BG);
    pVia->CursorMC = (CARD32) VIAGETREG(VIA_REG_CURSOR_MODE);*/
}

/* TODO deprecated */
void
ViaCursorRestore(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "ViaCursorRestore\n"));

    VIASETREG(VIA_REG_HI_CONTROL0, pVia->CursorControl0);
    VIASETREG(VIA_REG_HI_BASE0, pVia->CursorHiBase0);
    VIASETREG(0x2E8, pVia->CursorFifo); // VIA_FIFO
    VIASETREG(0x2EC, pVia->CursorTransparentKey); // TRANSPARENT_KEY
    VIASETREG(0x120C, pVia->CursorPrimHiInvtColor); // VIA_REG_PRIM_HI_INVTCOLOR
    VIASETREG(0x2E4, pVia->CursorV327HiInvtColor); // VIA_REG_V327_HI_INVTCOLOR
}

/*
 * ARGB Cursor
 */

void
VIAShowCursor(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;
    CARD32 control = pVia->CursorPipe ? VIA_REG_HI_CONTROL1 : VIA_REG_HI_CONTROL0;

    temp =
	(1 << 30) |
	(1 << 29) |
	(1 << 28) |
	(1 << 26) |
	(1 << 25) |
	(1 <<  2) |
	(1 <<  0);

    if (pVia->CursorPipe)
	temp |= (1 << 31);

    VIASETREG(control, temp);
}

void
VIAHideCursor(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;
    CARD32 control = pVia->CursorPipe ? VIA_REG_HI_CONTROL1 : VIA_REG_HI_CONTROL0;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);
}

static void
VIASetCursorPosition(ScrnInfoPtr pScrn, int x, int y)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;
    CARD32 control = pVia->CursorPipe ? VIA_REG_HI_CONTROL1 : VIA_REG_HI_CONTROL0;
    CARD32 offset = pVia->CursorPipe ? VIA_REG_HI_OFFSET1 : VIA_REG_HI_OFFSET0;
    CARD32 pos = pVia->CursorPipe ? VIA_REG_HI_POS1 : VIA_REG_HI_POS0;

    if (x < 0)
	x = 0;

    if (y < 0)
	y = 0;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);

    VIASETREG(pos, ((x << 16) | (y & 0x07ff)));
    VIASETREG(offset, ((0 << 16) | (0 & 0x07ff)));

    VIASETREG(control, temp);
}

static Bool
VIAUseHWCursorARGB(ScreenPtr pScreen, CursorPtr pCurs)
{
    return TRUE;
}

static Bool
VIAUseHWCursor(ScreenPtr pScreen, CursorPtr pCurs)
{
    return TRUE;
}

static void
VIALoadCursorImage(ScrnInfoPtr pScrn, unsigned char *s)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 control = pVia->CursorPipe ? VIA_REG_HI_CONTROL1 : VIA_REG_HI_CONTROL0;
    CARD32 temp;
    CARD32 *dst;
    CARD8 *src;
    CARD8 chunk;
    int i, j;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);

    pVia->CursorARGB = FALSE;

    dst = (CARD32*)(pVia->FBBase + pVia->CursorStart);
    src = (CARD8*)s;

#define ARGB_PER_CHUNK	(8 * sizeof (chunk) / 2)
    for (i = 0; i < (MAX_CURS * MAX_CURS / ARGB_PER_CHUNK); i++) {
	chunk = *s++;
	for (j = 0; j < ARGB_PER_CHUNK; j++, chunk >>= 2)
	    *dst++ = mono_cursor_color[chunk & 3];
    }

    pVia->CursorFG = mono_cursor_color[3];
    pVia->CursorBG = mono_cursor_color[2];

    VIASETREG(control, temp);
}

static void
VIASetCursorColors(ScrnInfoPtr pScrn, int bg, int fg)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 control = pVia->CursorPipe ? VIA_REG_HI_CONTROL1 : VIA_REG_HI_CONTROL0;
    CARD32 pixel;
    CARD32 temp;
    CARD32 *dst;
    int i;

    if (pVia->CursorFG)
	return;

    fg |= 0xff000000;
    bg |= 0xff000000;

    if (fg == pVia->CursorFG && bg == pVia->CursorBG)
	return;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);

    dst = (CARD32*)(pVia->FBBase + pVia->CursorStart);
    for (i = 0; i < MAX_CURS * MAX_CURS; i++, dst++)
	if ((pixel = *dst))
	    *dst = (pixel == pVia->CursorFG) ? fg : bg;

    pVia->CursorFG = fg;
    pVia->CursorBG = bg;

    VIASETREG(control, temp);
}

static void
VIALoadCursorARGB(ScrnInfoPtr pScrn, CursorPtr pCurs)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 control = pVia->CursorPipe ? VIA_REG_HI_CONTROL1 : VIA_REG_HI_CONTROL0;
    int x, y, w, h;
    CARD32 *image;
    CARD32 *dst;
    CARD32 *src;
    CARD32 temp;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);

    pVia->CursorARGB = TRUE;

    dst = (CARD32*)(pVia->FBBase + pVia->CursorStart);
    image = pCurs->bits->argb;

    w = pCurs->bits->width;
    if (w > MAX_CURS)
	w = MAX_CURS;

    h = pCurs->bits->height;
    if (h > MAX_CURS)
	h = MAX_CURS;

    for (y = 0; y < h; y++) {

	src = image;
	image += pCurs->bits->width;

	for (x = 0; x < w; x++)
	    *dst++ = *src++;
	for (; x < MAX_CURS; x++)
	    *dst++ = 0;
    }

    for (; y < MAX_CURS; y++)
	for (x = 0; x < MAX_CURS; x++)
	    *dst++ = 0;
}
