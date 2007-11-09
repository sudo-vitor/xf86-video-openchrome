/* 
 * Copyright 2007 The Openchrome Project [openchrome.org]
 * Copyright 1998-2007 VIA Technologies, Inc. All Rights Reserved.
 * Copyright 2001-2007 S3 Graphics, Inc. All Rights Reserved.
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
 * Panel core functions
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "via.h"
#include "via_driver.h"
#include "via_vgahw.h"
#include "via_id.h"

static ViaPanelModeRec ViaPanelNativeModes[] = {
        { 640, 480 },
        { 800, 600 },
        { 1024, 768 },
        { 1280, 768 },
        { 1280, 1024 },
        { 1600, 1200 },
        { 1280, 800 },
        { 800, 480 },
        { 1400, 1050 },
        { 1366, 768 },
        { 1920, 1080 },
        { 1920, 1200 },
        { 1024, 600 },
        { 1440, 900 },
        { 1280, 720 }
};

/**
 * 
 */
void
ViaPanelGetNativeModeFromOption(ScrnInfoPtr pScrn, char* name)
{
    VIAPtr pVia= VIAPTR(pScrn);
    VIABIOSInfoPtr pBIOSInfo = pVia->pBIOSInfo;
    CARD8 index;
    CARD8 length;

    pBIOSInfo->Panel->NativeModeIndex = VIA_PANEL_INVALID;
    length = sizeof(ViaPanelNativeModes) / sizeof(ViaPanelModeRec);
    char aux[10];
    for (index = 0; index < length; index++) {
        sprintf(aux, "%dx%d", ViaPanelNativeModes[ index ].Width,
                ViaPanelNativeModes[ index ].Height) ;
        if (!xf86NameCmp(name, aux)) {
            pBIOSInfo->Panel->NativeModeIndex = index;
            break;
        }
    }
}

void 
ViaPanelGetNativeModeFromScratchPad(ScrnInfoPtr pScrn) {
    VIAPtr pVia = VIAPTR(pScrn);
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    CARD8 index ;
    index = hwp->readCrtc(hwp, 0x3F) & 0x0F ;
    
    ViaPanelInfoPtr panel = pVia->pBIOSInfo->Panel ;
    panel->NativeModeIndex = index ;
    panel->NativeMode->Width = ViaPanelNativeModes[ index ].Width ;
    panel->NativeMode->Height = ViaPanelNativeModes[ index ].Height ;
}

/**
 * 
 */
void ViaPanelScale(ScrnInfoPtr pScrn, int resWidth, int resHeight, int panelWidth, int panelHeight ) {
    VIAPtr pVia = VIAPTR(pScrn);
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    int     horScalingFactor;
    int     verScalingFactor;
    CARD8   cra2 = 0;
    CARD8   cr77 = 0;
    CARD8   cr78 = 0;
    CARD8   cr79 = 0;
    CARD8   cr9f = 0;
    Bool scaling = FALSE ;

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO,
        "ViaPanelScale: %d,%d -> %d,%d\n",
       resWidth, resHeight, panelWidth, panelHeight ));

    if (resWidth < panelWidth) {

        /* FIXME: It is different for chipset < K8M800 */
        horScalingFactor = ((resWidth - 1) * 4096) / (panelWidth - 1) ;

        /* Horizontal scaling enabled */
        cra2 = 0xC0;
        cr9f = horScalingFactor & 0x0003;                  /* HSCaleFactor[1:0] at CR9F[1:0] */
        cr77 = (horScalingFactor & 0x03FC)>>2;             /* HSCaleFactor[9:2] at CR77[7:0] */
        cr79 = (horScalingFactor & 0x0C00)>>10;            /* HSCaleFactor[11:10] at CR79[5:4] */
        cr79 <<= 4;
        scaling = TRUE ;

    }

    if (resHeight < panelHeight) {

        verScalingFactor = ((resHeight - 1) * 2048) / (panelHeight - 1) ;

        /* Vertical scaling enabled */
        cra2 |= 0x08;
        cr79 |= ((verScalingFactor & 0x0001)<<3);          /* VSCaleFactor[0] at CR79[3] */
        cr78 |= (verScalingFactor & 0x01FE) >> 1;          /* VSCaleFactor[8:1] at CR78[7:0] */
        cr79 |= ((verScalingFactor & 0x0600) >> 9) << 6;   /* VSCaleFactor[10:9] at CR79[7:6] */
        scaling = TRUE ;
    }

    DEBUG(xf86DrvMsg(pScrn->scrnIndex, X_INFO,
        "Scaling factor: horizontal %d (0x%x), vertical %d (0x%x)\n",
       horScalingFactor, horScalingFactor, verScalingFactor, verScalingFactor ));

    if (scaling) {
        ViaCrtcMask(hwp, 0x77, cr77, 0xFF);
        ViaCrtcMask(hwp, 0x78, cr78, 0xFF);
        ViaCrtcMask(hwp, 0x79, cr79, 0xF8);
        ViaCrtcMask(hwp, 0x9F, cr9f, 0x03);
    }

    ViaCrtcMask(hwp, 0xA2, cra2, 0xC8);

    /* Horizontal scaling selection: interpolation */
    // ViaCrtcMask(hwp, 0x79, 0x02, 0x02);
    // else
    // ViaCrtcMask(hwp, 0x79, 0x00, 0x02);
    /* Horizontal scaling factor selection original / linear */
    //ViaCrtcMask(hwp, 0xA2, 0x40, 0x40);

    ViaCrtcMask(hwp, 0x79, 0x07, 0x07);
}
