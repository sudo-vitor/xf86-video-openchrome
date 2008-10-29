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

#include <wsbm_manager.h>
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
    int ret;

    ret = wsbmBOData(pVia->scanout.bufs[VIA_SCANOUT_CURSOR], 
		    VIA_CURSOR_SIZE, NULL, NULL, 0);
    if (ret)
	return FALSE;

    pVia->cursorMap = wsbmBOMap(pVia->scanout.bufs[VIA_SCANOUT_CURSOR], 1,
			       WSBM_SYNCCPU_READ |
			       WSBM_SYNCCPU_WRITE);

    if (pVia->cursorMap == NULL)
	return FALSE;
    wsbmBOUnmap(pVia->scanout.bufs[VIA_SCANOUT_CURSOR]);
    pVia->cursorOffset = wsbmBOOffset(pVia->scanout.bufs[VIA_SCANOUT_CURSOR]);
    memset(pVia->cursorMap, 0x00, VIA_CURSOR_SIZE);

    if (pVia->Chipset == VIA_CX700) {
	if (!pVia->pBIOSInfo->PanelActive) {
	    pVia->CursorRegControl  = VIA_REG_HI_CONTROL0;
	    pVia->CursorRegBase     = VIA_REG_HI_BASE0;
	    pVia->CursorRegPos      = VIA_REG_HI_POS0;
	    pVia->CursorRegOffset   = VIA_REG_HI_OFFSET0;
	    pVia->CursorRegFifo     = VIA_REG_HI_FIFO0;
	    pVia->CursorRegTransKey = VIA_REG_HI_TRANSKEY0;
	} else {
	    pVia->CursorRegControl  = VIA_REG_HI_CONTROL1;
	    pVia->CursorRegBase     = VIA_REG_HI_BASE1;
	    pVia->CursorRegPos      = VIA_REG_HI_POS1;
	    pVia->CursorRegOffset   = VIA_REG_HI_OFFSET1;
	    pVia->CursorRegFifo     = VIA_REG_HI_FIFO1;
	    pVia->CursorRegTransKey = VIA_REG_HI_TRANSKEY1;
	}
    } else {
	pVia->CursorRegControl = VIA_REG_ALPHA_CONTROL;
	pVia->CursorRegBase = VIA_REG_ALPHA_BASE;
	pVia->CursorRegPos = VIA_REG_ALPHA_POS;
	pVia->CursorRegOffset = VIA_REG_ALPHA_OFFSET;
	pVia->CursorRegFifo = VIA_REG_ALPHA_FIFO;
	pVia->CursorRegTransKey = VIA_REG_ALPHA_TRANSKEY;
    }

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

    /* Set cursor location in frame buffer. */
    VIASETREG(VIA_REG_CURSOR_MODE, pVia->cursorOffset);

    pVia->CursorPipe = (pVia->pBIOSInfo->PanelActive) ? 1 : 0;

    /* Init HI_X0 */
    VIASETREG(pVia->CursorRegControl, 0);
    VIASETREG(pVia->CursorRegBase, pVia->cursorOffset);
    VIASETREG(pVia->CursorRegTransKey, 0);

    if (pVia->Chipset == VIA_CX700) {
	if (!pVia->pBIOSInfo->PanelActive) {
	    VIASETREG(VIA_REG_PRIM_HI_INVTCOLOR, 0x00FFFFFF);
	    VIASETREG(VIA_REG_V327_HI_INVTCOLOR, 0x00FFFFFF);
	    VIASETREG(pVia->CursorRegFifo, 0x0D000D0F);
	} else {
	    VIASETREG(VIA_REG_HI_INVTCOLOR, 0X00FFFFFF);
	    VIASETREG(VIA_REG_ALPHA_PREFIFO, 0xE0000);
	    VIASETREG(pVia->CursorRegFifo, 0xE0F0000);

	    /* Just in case */
	    VIASETREG(VIA_REG_HI_BASE0, pVia->cursorOffset);
	}
    } else {
	VIASETREG(VIA_REG_HI_INVTCOLOR, 0X00FFFFFF);
	VIASETREG(VIA_REG_ALPHA_PREFIFO, 0xE0000);
	VIASETREG(pVia->CursorRegFifo, 0xE0F0000);
    }

    return xf86InitCursor(pScreen, infoPtr);
}

void
ViaCursorStore(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "ViaCursorStore\n"));

    if (pVia->CursorPipe) {
	pVia->CursorControl1 = VIAGETREG(pVia->CursorRegControl);
    } else {
	pVia->CursorControl0 = VIAGETREG(pVia->CursorRegControl);
    }

    pVia->CursorTransparentKey = VIAGETREG(pVia->CursorRegTransKey);

    if (pVia->Chipset == VIA_CX700) {
	if (!pVia->pBIOSInfo->PanelActive) {
	    pVia->CursorPrimHiInvtColor = VIAGETREG(VIA_REG_PRIM_HI_INVTCOLOR);
	    pVia->CursorV327HiInvtColor = VIAGETREG(VIA_REG_V327_HI_INVTCOLOR);
	} else {
	    /* TODO add saves here */
	}
	pVia->CursorFifo = VIAGETREG(pVia->CursorRegFifo);
    } else {
	/* TODO add saves here */
    }
}

void
ViaCursorRestore(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO, "ViaCursorRestore\n"));

    if (pVia->CursorPipe) {
	VIASETREG(pVia->CursorRegControl, pVia->CursorControl1);
    } else {
	VIASETREG(pVia->CursorRegControl, pVia->CursorControl0);
    }

    VIASETREG(pVia->CursorRegBase, pVia->cursorOffset);

    VIASETREG(pVia->CursorRegTransKey, pVia->CursorTransparentKey);

    if (pVia->Chipset == VIA_CX700) {
	if (!pVia->pBIOSInfo->PanelActive) {
	    VIASETREG(VIA_REG_PRIM_HI_INVTCOLOR, pVia->CursorPrimHiInvtColor);
	    VIASETREG(VIA_REG_V327_HI_INVTCOLOR, pVia->CursorV327HiInvtColor);
	} else {
	    /* TODO add real restores here */
	    VIASETREG(VIA_REG_HI_INVTCOLOR, 0X00FFFFFF);
	    VIASETREG(VIA_REG_ALPHA_PREFIFO, 0xE0000);
	}
	VIASETREG(pVia->CursorRegFifo, pVia->CursorFifo);
    } else {
	/* TODO add real restores here */
	VIASETREG(VIA_REG_ALPHA_PREFIFO, 0xE0000);
	VIASETREG(pVia->CursorRegFifo, 0xE0F0000);
    }
}

/*
 * ARGB Cursor
 */

void
VIAShowCursor(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;
    CARD32 control = pVia->CursorRegControl;

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
    CARD32 control = pVia->CursorRegControl;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);
}

static void
VIASetCursorPosition(ScrnInfoPtr pScrn, int x, int y)
{
    VIAPtr pVia = VIAPTR(pScrn);
    CARD32 temp;
    CARD32 control = pVia->CursorRegControl;
    CARD32 offset = pVia->CursorRegOffset;
    CARD32 pos = pVia->CursorRegPos;
    unsigned xoff, yoff;

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
    }

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);

    VIASETREG(pos,    ((x    << 16) | (y    & 0x07ff)));
    VIASETREG(offset, ((xoff << 16) | (yoff & 0x07ff)));

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
    CARD32 control = pVia->CursorRegControl;
    CARD32 temp;
    CARD32 *dst;
    CARD8 *src;
    CARD8 chunk;
    int i, j;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);

    pVia->CursorARGB = FALSE;

    dst = (CARD32*)(pVia->cursorMap);
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
    CARD32 control = pVia->CursorRegControl;
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

    dst = (CARD32*)pVia->cursorMap;
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
    CARD32 control = pVia->CursorRegControl;
    int x, y, w, h;
    CARD32 *image;
    CARD32 *dst;
    CARD32 *src;
    CARD32 temp;

    temp = VIAGETREG(control);
    VIASETREG(control, temp & 0xFFFFFFFE);

    pVia->CursorARGB = TRUE;

    dst = (CARD32*)pVia->cursorMap;
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

    VIASETREG(control, temp);
}
