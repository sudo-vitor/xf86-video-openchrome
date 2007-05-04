/* -*- c-basic-offset: 4 -*- */
/**************************************************************************

Copyright © 2006 Dave Airlie

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Resources.h"
#include "compiler.h"
#include "miscstruct.h"
#include "xf86i2c.h"

#include "../i2c_vid.h"
#include "vt1632.h"
#include "vt1632_reg.h"

static void
vt1632PrintRegs(I2CDevPtr d);
static void
vt1632Power(I2CDevPtr d, Bool On);

static Bool
vt1632ReadByte(VT1632Ptr vt, int addr, CARD8 *ch)
{
    if (!xf86I2CReadByte(&vt->d, addr, ch)) {
	xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_ERROR,
		   "Unable to read from %s Slave 0x%02x (subaddress 0x%02x).\n",
		   vt->d.pI2CBus->BusName, vt->d.SlaveAddr, addr);
	return FALSE;
    }
    return TRUE;
}

static Bool
vt1632ReadWord(VT1632Ptr vt, int addr, CARD16 *wd)
{
    I2CByte rb[2];

    if (!xf86I2CReadBytes(&vt->d, addr, rb, sizeof(rb))) {
	xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_ERROR,
		   "Unable to read from %s Slave 0x%02x (subaddress 0x%02x).\n",
		   vt->d.pI2CBus->BusName, vt->d.SlaveAddr, addr);
	return FALSE;
    }
    *wd = (rb[1] << 8) | rb[0];
    return TRUE;
}

static Bool
vt1632WriteByte(VT1632Ptr vt, int addr, CARD8 ch)
{
    if (!xf86I2CWriteByte(&(vt->d), addr, ch)) {
	xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_ERROR,
		   "Unable to write to %s Slave 0x%02x (subaddress 0x%02x).\n",
		   vt->d.pI2CBus->BusName, vt->d.SlaveAddr, addr);
	return FALSE;
    }
    return TRUE;
}

/* Silicon Image 164 driver for chip on i2c bus */
static I2CDevPtr
vt1632Detect(I2CBusPtr b, I2CSlaveAddr addr)
{
    /* this will detect the VT1632 chip on the specified i2c bus */
    VT1632Ptr vt;
    CARD16 word;

#if 0
    xf86DrvMsg(b->scrnIndex, X_INFO, "Probing vt1632 on %s Slave 0x%02x\n",
	       b->BusName, addr);
#endif

    if (!xf86I2CProbeAddress(b, addr)) {
	xf86DrvMsg(b->scrnIndex, X_ERROR, "Probe at %s Slave 0x%02x failed to respond.\n",
		   b->BusName, addr);
	return NULL;
    }

    vt = xcalloc(1, sizeof(VT1632Rec));
    if (vt == NULL)
	return NULL;

    vt->d.DevName = "VT1632 TMDS Controller";
    vt->d.SlaveAddr = addr;
    vt->d.pI2CBus = b;
    vt->d.DriverPrivate.ptr = vt;

    /* let the rest be inherited.
     * we might fail if this address has already been claimed.
     */
    if (!xf86I2CDevInit(&vt->d)) {
	xfree(vt);
	return NULL;
    }

    if (!vt1632ReadWord(vt, VT1632_VID_LO, &word))
	goto out;

    if (word!=VT1632_VID) {
	xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_ERROR,
		   "vt1632 not detected got VId 0x%04x: from %s Slave 0x%02x.\n",
		   word, vt->d.pI2CBus->BusName, vt->d.SlaveAddr, word);
	goto out;
    }

    if (!vt1632ReadWord(vt, VT1632_DID_LO, &word))
	goto out;

    if (word!=VT1632_DID) {
	xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_ERROR,
		   "vt1632 not detected got DId 0x%04x: from %s Slave 0x%02x.\n",
		   word, vt->d.pI2CBus->BusName, vt->d.SlaveAddr);
	goto out;
    }

    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_PROBED,
	       "Detected VT1632 on %s Slave 0x%02x.\n", vt->d.pI2CBus->BusName,
	       vt->d.SlaveAddr);

    return &vt->d;

out:
    xf86DestroyI2CDevRec(&vt->d, FALSE);
    xfree(vt);
    return NULL;
}


static Bool
vt1632Init(I2CDevPtr d)
{
    VT1632Ptr vt = VTPTR(d);

vt1632WriteByte(vt, VT1632_REG8, 0x37);
    /* not much to do */
    return TRUE;
}

static ModeStatus
vt1632ModeValid(I2CDevPtr d, DisplayModePtr mode)
{
    VT1632Ptr vt = VTPTR(d);

    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "vt1632 freq %d\n",
	       mode->Clock);

    if (mode->Clock > 165000)
	return MODE_CLOCK_HIGH;

    return MODE_OK;
}

static void
vt1632Mode(I2CDevPtr d, DisplayModePtr mode)
{
    VT1632Ptr vt = VTPTR(d);
    unsigned char reg08, reg0c;
    //    vt1632Power(d, TRUE);
    vt1632PrintRegs(d);

    vt1632ReadByte(vt, VT1632_REG8, &reg08);
       
    reg08 &= ~( VT1632_8_BSEL | VT1632_8_EDGE /*| VT1632_8_DSEL*/ | VT1632_8_PD);
    //reg08 |= VT1632_8_DSEL;
    vt1632WriteByte(vt, VT1632_REG8, reg08);
    /* recommended programming sequence from doc */
    //vt1632WriteByte(vt, 0x08, 0xf8);
    vt1632WriteByte(vt, VT1632_REG9, 0x00);
    vt1632WriteByte(vt, VT1632_REGA, 0x90);
    
    vt1632WriteByte(vt, VT1632_REGC, 0x09);
    reg08 |= VT1632_8_PD;
    vt1632WriteByte(vt, VT1632_REG8, reg08);
    /* don't do much */
    vt1632PrintRegs(d);
    return;
}

/* set the VT1632 power state */
static void
vt1632Power(I2CDevPtr d, Bool On)
{
    VT1632Ptr vt = VTPTR(d);
    int ret;
    unsigned char ch;

    ret = vt1632ReadByte(vt, VT1632_REG8, &ch);
    if (ret == FALSE)
	return;

    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "VT1632 Power Set %d %02X\n", On, ch);
    if (On)
	ch |= VT1632_8_PD;
    else
	ch &= ~VT1632_8_PD;

    vt1632WriteByte(vt, VT1632_REG8, ch);

    return;
}

static void
vt1632PrintRegs(I2CDevPtr d)
{
    VT1632Ptr vt = VTPTR(d);
    CARD8 val;

    vt1632ReadByte(vt, VT1632_FREQ_LO, &val);
    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "VT1632_FREQ_LO: 0x%02x\n",
	       val);
    vt1632ReadByte(vt, VT1632_FREQ_HI, &val);
    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "VT1632_FREQ_HI: 0x%02x\n",
	       val);
    vt1632ReadByte(vt, VT1632_REG8, &val);
    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "VT1632_REG8: 0x%02x\n", val);
    vt1632ReadByte(vt, VT1632_REG9, &val);
    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "VT1632_REG9: 0x%02x\n", val);
    vt1632ReadByte(vt, VT1632_REGA, &val);
    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "VT1632_REGA: 0x%02x\n", val);
    vt1632ReadByte(vt, VT1632_REGC, &val);
    xf86DrvMsg(vt->d.pI2CBus->scrnIndex, X_INFO, "VT1632_REGC: 0x%02x\n", val);
}

static void
vt1632SaveRegs(I2CDevPtr d)
{
    VT1632Ptr vt = VTPTR(d);

    if (!vt1632ReadByte(vt, VT1632_REG8, &vt->SavedReg.reg8))
	return;

    if (!vt1632ReadByte(vt, VT1632_REG9, &vt->SavedReg.reg9))
	return;

    if (!vt1632ReadByte(vt, VT1632_REGC, &vt->SavedReg.regc))
	return;

    return;
}

static void
vt1632RestoreRegs(I2CDevPtr d)
{
    VT1632Ptr vt = VTPTR(d);

    /* Restore it powered down initially */
    vt1632WriteByte(vt, VT1632_REG8, vt->SavedReg.reg8 & ~0x1);

    vt1632WriteByte(vt, VT1632_REG9, vt->SavedReg.reg9);
    vt1632WriteByte(vt, VT1632_REGC, vt->SavedReg.regc);
    vt1632WriteByte(vt, VT1632_REG8, vt->SavedReg.reg8);
}


I830I2CVidOutputRec VT1632VidOutput = {
    vt1632Detect,
    vt1632Init,
    vt1632ModeValid,
    vt1632Mode,
    vt1632Power,
    vt1632PrintRegs,
    vt1632SaveRegs,
    vt1632RestoreRegs,
};
