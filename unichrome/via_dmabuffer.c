/*
 * Copyright (C) Thomas Hellstrom (2005) 
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

#include "via_driver.h"

void viaFlushPCI(ViaCommandBuffer *buf)
{
    unsigned size = buf->pos >> 1;
    int i;
    register CARD32 *bp = buf->buf;
    unsigned loop = 0;
    register unsigned offset;
    register unsigned value;
    VIAPtr pVia = VIAPTR(buf->pScrn);
    
    /*
     * Not doing this wait will probably stall the processor
     * for an unacceptable amount of time in VIASETREG while other high
     * priority interrupts may be pending.
     */
    
    while (!(VIAGETREG(VIA_REG_STATUS) & VIA_VR_QUEUE_BUSY) && (loop++ < MAXLOOP));
    while ((VIAGETREG(VIA_REG_STATUS) &
	    (VIA_CMD_RGTR_BUSY | VIA_2D_ENG_BUSY )) &&
	   (loop++ < MAXLOOP));
    
    for(i=0; i<size; ++i) {
	offset = (*bp++ & 0x0FFFFFFF) << 2;
	value = *bp++;
	VIASETREG( offset , value);
    }
    buf->pos = 0;
}


static void 
viaFlushDma(ViaCommandBuffer *cb) 
{
    ScrnInfoPtr pScrn = cb->pScrn;
    VIAPtr pVia = VIAPTR(pScrn);
    char *tmp = (char *) cb->buf;
    int tmpSize = cb->pos * sizeof(CARD32);
    drm_via_cmdbuffer_t b;
    
    do {
	b.size = (tmpSize > VIA_DMASIZE) ? VIA_DMASIZE : tmpSize;
	tmpSize -= b.size;
	b.buf = tmp;
	tmp += b.size;

	if (pVia->directRenderingEnabled && pVia->agpEnable && pVia->dma2d) {    
	    VIADRIPtr pVIADRI =  pVia->pDRIInfo->devPrivate;
	    if (pVIADRI->ringBufActive) {
		if (drmCommandWrite(pVia->drmFD,DRM_VIA_CMDBUFFER,&b,sizeof(b)))
		    return;
	    }
	}
    } while (tmpSize > 0);
    cb->pos = 0;
}

void 
viaSetupCBuffer(ScrnInfoPtr pScrn, ViaCommandBuffer *buf, unsigned size)
{
    VIAPtr pVia = VIAPTR(pScrn);

    buf->pScrn = pScrn;
    buf->buf = (CARD32 *)xcalloc((size == 0 ? VIA_DMASIZE : size),1);
    buf->waitFlags = 0;
    buf->pos = 0;
    buf->bufSize = VIA_DMASIZE >> 2;
    buf->mode = 0;
    buf->header_start = 0;
    buf->rindex = 0;
    if (pVia->directRenderingEnabled && pVia->agpEnable && pVia->dma2d) {    
	buf->flushFunc = viaFlushDma;
    } else {
	buf->flushFunc = viaFlushPCI;
    }
}

void
viaTearDownCBuffer(ViaCommandBuffer *buf)
{
    xfree(buf->buf);
    buf->buf = NULL;
}
