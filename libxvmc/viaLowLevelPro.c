/*****************************************************************************
 * VIA Unichrome XvMC extension client lib.
 *
 * Copyright (c) 2004 Thomas Hellström. All rights reserved.
 * Copyright (c) 2003 Andreas Robinson. All rights reserved.
 * Copyright (c) 2008 Tungsten Graphics Inc, Cedar Park, TX. USA. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHOR(S) OR COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Low-level functions that deal directly with the hardware. In the future,
 * these functions might be implemented in a kernel module. Also, some of them
 * would benefit from DMA.
 *
 * Authors: 
 *  Andreas Robinson 2003. (Initial decoder interface functions).
 *  Thomas Hellstrom 2004, 2005 (Blitting functions, AGP and locking, Unichrome Pro Video AGP).
 *  Ivor Hewitt 2005 (Unichrome Pro modifications and merging).
 *  Thomas Hellstrom 2008, TTM port.
 */

/* IH
 * I've left the proReg or-ing in case we need/want to implement the V1/V3
 * register toggle too, which also moves the register locations.
 * The CN400 has dual mpeg decoders, not sure at the moment whether these
 * are also operated through independent registers also.
 */

#undef VIDEO_DMA
#undef HQV_USE_IRQ
#define UNICHROME_PRO

#include "viaXvMCPriv.h"
#include "viaLowLevel.h"
#include "driDrawable.h"
#include "ochr_ioctl.h"
#include <stdio.h>

typedef enum
{ ll_init, ll_agpBuf, ll_pciBuf}
LLState;

enum ViaLLEngines {
    LL_ENGINE_MPEG,
    LL_ENGINE_HQV,
    LL_ENGINE_2D,
    LL_ENGINE_3D,
    LL_ENGINE_NONE,
};


struct _XvMCLowLevel;

typedef struct _ViaCommandBuffer
{
    struct ochr_cmd_buffer c_buf;    
    unsigned bufSize;
    int mode;
    int header_start;
    int rindex;
    struct _XvMCLowLevel *xl;
    void (*flushFunc) (struct _ViaCommandBuffer * cb);
} ViaCommandBuffer;

#define HQV_STATE_BASE 0x3CC
#define HQV_STATE_SIZE 13

struct _HQVRegister
{
    CARD32 data;
    Bool set;
};

struct _HQVState {
    struct _HQVRegister regs[HQV_STATE_SIZE];
    struct _WsbmBufferObject *srcBuf;
    unsigned yOffs;
    unsigned uvOffs;
};
    

typedef struct _XvMCLowLevel
{
    ViaCommandBuffer agpBuf;
    int use_agp;
    int fd;
    drm_context_t *drmcontext;
    drmLockPtr hwLock;
    unsigned fbStride;
    unsigned fbDepth;
    unsigned width;
    unsigned height;
    int performLocking;
    unsigned errors;
    CARD32 tsOffset;
    unsigned chipId;
    uint64_t hqvFlag;
    unsigned hqvOffset;
    CARD32 subStride;
    LLState state;
    enum ViaLLEngines lastEngine;
    struct _HQVState hqvState;
} XvMCLowLevel;

#define HQV_CONTROL             0x1D0
#define HQV_SRC_OFFSET          0x1CC
#define HQV_SRC_STARTADDR_Y     0x1D4
#define HQV_SRC_STARTADDR_U     0x1D8
#define HQV_SRC_STARTADDR_V     0x1DC
#define HQV_MINIFY_DEBLOCK      0x1E8

#define REG_HQV1_INDEX      0x00001000

#define HQV_SW_FLIP         0x00000010
#define HQV_FLIP_STATUS     0x00000001
#define HQV_SUBPIC_FLIP     0x00008000
#define HQV_FLIP_ODD        0x00000020
#define HQV_DEINTERLACE     0x00010000
#define HQV_FIELD_2_FRAME   0x00020000
#define HQV_FRAME_2_FIELD   0x00040000
#define HQV_FIELD_UV        0x00100000
#define HQV_DEBLOCK_HOR     0x00008000
#define HQV_DEBLOCK_VER     0x80000000
#define HQV_YUV420          0xC0000000
#define HQV_YUV422          0x80000000
#define HQV_ENABLE          0x08000000
#define HQV_GEN_IRQ         0x00000080

#define HQV_SCALE_ENABLE    0x00000800
#define HQV_SCALE_DOWN      0x00001000

#define V_COMPOSE_MODE          0x98
#define V1_COMMAND_FIRE         0x80000000
#define V3_COMMAND_FIRE         0x40000000

/* SUBPICTURE Registers */
#define SUBP_CONTROL_STRIDE     0x1C0
#define SUBP_STARTADDR          0x1C4
#define RAM_TABLE_CONTROL       0x1C8

/* SUBP_CONTROL_STRIDE              0x3c0 */
#define SUBP_HQV_ENABLE             0x00010000
#define SUBP_IA44                   0x00020000
#define SUBP_AI44                   0x00000000
#define SUBP_STRIDE_MASK            0x00001fff
#define SUBP_CONTROL_MASK           0x00070000

/* RAM_TABLE_CONTROL                0x3c8 */
#define RAM_TABLE_RGB_ENABLE        0x00000007

#define VIA_REG_STATUS          0x400
#define VIA_REG_GEMODE          0x004
#define VIA_REG_SRCBASE         0x030
#define VIA_REG_DSTBASE         0x034
#define VIA_REG_PITCH           0x038
#define VIA_REG_SRCCOLORKEY     0x01C
#define VIA_REG_KEYCONTROL      0x02C
#define VIA_REG_SRCPOS          0x008
#define VIA_REG_DSTPOS          0x00C
#define VIA_REG_GECMD           0x000
#define VIA_REG_DIMENSION       0x010  /* width and height */
#define VIA_REG_FGCOLOR         0x018

#define VIA_VR_QUEUE_BUSY       0x00020000	/* Virtual Queue is busy */
#define VIA_CMD_RGTR_BUSY       0x00000080	/* Command Regulator is busy */
#define VIA_2D_ENG_BUSY         0x00000002	/* 2D Engine is busy */
#define VIA_3D_ENG_BUSY         0x00000001	/* 3D Engine is busy */
#define VIA_GEM_8bpp            0x00000000
#define VIA_GEM_16bpp           0x00000100
#define VIA_GEM_32bpp           0x00000300
#define VIA_GEC_BLT             0x00000001
#define VIA_PITCH_ENABLE        0x80000000
#define VIA_GEC_INCX            0x00000000
#define VIA_GEC_DECY            0x00004000
#define VIA_GEC_INCY            0x00000000
#define VIA_GEC_DECX            0x00008000
#define VIA_GEC_FIXCOLOR_PAT    0x00002000

#define VIA_BLIT_CLEAR 0x00
#define VIA_BLIT_COPY 0xCC
#define VIA_BLIT_FILL 0xF0
#define VIA_BLIT_SET 0xFF

#define VIA_SYNCWAITTIMEOUT 50000      /* Might be a bit conservative */
#define VIA_DMAWAITTIMEOUT 150000
#define VIA_VIDWAITTIMEOUT 50000
#define VIA_XVMC_DECODERTIMEOUT 50000  /*(microseconds) */

#define VIA_AGP_HEADER5 0xFE040000
#define VIA_AGP_HEADER6 0xFE050000


#define H1_ADDR(val) (((val) >> 2) | 0xF0000000)
#define BEGIN_RING_AGP(cb, xl, size)					\
    do {								\
	if ((cb)->c_buf.pos > ((cb)->bufSize-(size))) {			\
	    cb->flushFunc(cb);						\
	}								\
    } while(0)
#define OUT_RING_AGP(cb, val) do{			\
	(cb)->c_buf.buf[(cb)->c_buf.pos++] = (val);	\
  } while(0);

#define OUT_RING_QW_AGP(cb, val1, val2)			\
    do {						\
	(cb)->c_buf.buf[(cb)->c_buf.pos++] = (val1);	\
	(cb)->c_buf.buf[(cb)->c_buf.pos++] = (val2);	\
    } while (0)

#define BEGIN_HEADER5_AGP(cb, xl, index)	\
    do {					\
	BEGIN_RING_AGP(cb, xl, 8);		\
	(cb)->mode = VIA_AGP_HEADER5;		\
        (cb)->rindex = (index);			\
	(cb)->header_start = (cb)->c_buf.pos;		\
	(cb)->c_buf.pos += 4;				\
    } while (0)

#define BEGIN_HEADER6_AGP(cb, xl)		\
    do {					\
	BEGIN_RING_AGP(cb, xl, 8);		\
	(cb)->mode = VIA_AGP_HEADER6;	\
	(cb)->header_start = (cb)->c_buf.pos; \
	(cb)->c_buf.pos += 4;			\
    } while (0)

#define BEGIN_HEADER5_DATA(cb, xl, size, index)				\
    do {								\
	if ((cb)->c_buf.pos > ((cb)->bufSize - ((size) + 16))) {	\
	    cb->flushFunc(cb);						\
	    BEGIN_HEADER5_AGP(cb, xl, index);				\
	} else if ((cb)->mode && (((cb)->mode != VIA_AGP_HEADER5) ||	\
				  ((cb)->rindex != index))) {		\
	    finish_header_agp(cb);					\
	    BEGIN_HEADER5_AGP((cb), xl, (index));			\
	} else if (cb->mode != VIA_AGP_HEADER5) {			\
	    BEGIN_HEADER5_AGP((cb), xl, (index)); 			\
	}								\
    }while(0)

#define BEGIN_HEADER6_DATA(cb, xl, size)				\
    do{									\
	if ((cb)->c_buf.pos > (cb->bufSize-(((size) << 1) + 16))) {	\
	    cb->flushFunc(cb);						\
	    BEGIN_HEADER6_AGP(cb, xl);					\
	} else	if ((cb)->mode && ((cb)->mode != VIA_AGP_HEADER6)) {	\
	    finish_header_agp(cb);					\
	    BEGIN_HEADER6_AGP(cb, xl);					\
	}								\
	else if ((cb->mode != VIA_AGP_HEADER6)) {			\
	    BEGIN_HEADER6_AGP(cb, (xl));				\
	}								\
    }while(0)

#define SETHQVSTATE(state, offset, value)				\
    do {								\
	struct _HQVRegister *r =					\
	    &(state)->regs[((offset) - HQV_STATE_BASE) >> 2];		\
	r->data = (value);						\
	r->set = TRUE;							\
    } while(0)

#define GETHQVSTATE(state, offset)				\
    ((state)->regs[(offset - HQV_STATE_BASE) >> 2].data)	\

#define LL_HW_LOCK(xl)							\
    do {								\
	DRM_LOCK((xl)->fd,(xl)->hwLock,*(xl)->drmcontext,0);		\
    } while(0);
#define LL_HW_UNLOCK(xl)					\
    do {							\
	DRM_UNLOCK((xl)->fd,(xl)->hwLock,*(xl)->drmcontext);	\
    } while(0);


static void
initHQVState(struct _HQVState *state)
{
    int i;
    struct _HQVRegister *r = state->regs;
    
    if (state->srcBuf) {
	wsbmBOUnReference(state->srcBuf);
	state->srcBuf= NULL;
    }
    for (i = 0; i < HQV_STATE_SIZE; ++i) {
	r->data = 0;
	r++->set = FALSE;
    }
}

static void
setHQVDeblocking(struct _HQVState *state, Bool on, Bool lowPass)
{
    CARD32 tmp = GETHQVSTATE(state, 0x3DC);

    if (!on) {
	tmp &= ~(1 << 27);
	SETHQVSTATE(state, 0x3DC, tmp);
	return;
    }

    tmp |= (8 << 16) | (1 << 27);
    if (lowPass)
	tmp |= (1 << 26);
    SETHQVSTATE(state, 0x3DC, tmp);

    tmp = GETHQVSTATE(state, 0x3D4);
    tmp |= (6 << 27);
    SETHQVSTATE(state, 0x3D4, tmp);

    tmp = GETHQVSTATE(state, 0x3D8);
    tmp |= (19 << 27);
    SETHQVSTATE(state, 0x3D8, tmp);
}

static void
setHQVStartAddress(struct _HQVState *state, 
		   struct _WsbmBufferObject *buf, 
		   unsigned yOffs, unsigned uvOffs,
		   unsigned stride, unsigned format)
{
    CARD32 tmp;

    state->srcBuf = wsbmBOReference(buf);
    state->yOffs = yOffs;
    state->uvOffs = uvOffs;
 
    tmp = GETHQVSTATE(state, 0x3F8);
    tmp |= (stride & 0x1FF8);
    SETHQVSTATE(state, 0x3F8, tmp);
    tmp = GETHQVSTATE(state, 0x3D0);

    if (format == 0) {
	/*
	 * NV12
	 */
	tmp |= (0x0C << 28);
    } else if (format == 1) {
	/*
	 * RGB16
	 */
	tmp |= (0x02 << 28);
    } else if (format == 2) {
	/*
	 * RGB32
	 */
	;
    }
    SETHQVSTATE(state, 0x3D0, tmp);
}


static void
setHQVDeinterlacing(struct _HQVState * state, CARD32 frameType)
{
    CARD32 tmp = GETHQVSTATE(state, 0x3D0);

    if ((frameType & XVMC_FRAME_PICTURE) == XVMC_TOP_FIELD) {
	tmp |= HQV_FIELD_UV |
	    HQV_DEINTERLACE | HQV_FIELD_2_FRAME | HQV_FRAME_2_FIELD;
    } else if ((frameType & XVMC_FRAME_PICTURE) == XVMC_BOTTOM_FIELD) {
	tmp |= HQV_FIELD_UV |
	    HQV_DEINTERLACE |
	    HQV_FIELD_2_FRAME | HQV_FRAME_2_FIELD | HQV_FLIP_ODD;
    }
    SETHQVSTATE(state, 0x3D0, tmp);
}

static void
setHQVTripleBuffer(struct _HQVState * state, Bool on)
{
    CARD32 tmp = GETHQVSTATE(state, 0x3D0);

    if (on)
	tmp |= (1 << 26);
    else
	tmp &= ~(1 << 26);
    SETHQVSTATE(state, 0x3D0, tmp);
}

static void
finish_header_agp(ViaCommandBuffer * cb)
{
    int numDWords, i;

    uint32_t *hb;

    if (!cb->mode)
	return;
    numDWords = cb->c_buf.pos - cb->header_start - 4;
    hb = cb->c_buf.buf + cb->header_start;
    switch (cb->mode) {
    case VIA_AGP_HEADER5:
	hb[0] = VIA_AGP_HEADER5 | cb->rindex;
	hb[1] = numDWords;
	hb[2] = 0x00F50000;	       /* SW debug flag. (?) */
	break;
    default:
	hb[0] = VIA_AGP_HEADER6;
	hb[1] = numDWords >> 1;
	hb[2] = 0x00F60000;	       /* SW debug flag. (?) */
	break;
    }
    hb[3] = 0;
    if (numDWords & 3) {
	for (i = 0; i < (4 - (numDWords & 3)); ++i)
	    OUT_RING_AGP(cb, 0x00000000);
    }
    cb->mode = 0;
}

void
hwlLock(void *xlp, int videoLock)
{
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;

    LL_HW_LOCK(xl);
}

void
hwlUnlock(void *xlp, int videoLock)
{
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;

    LL_HW_UNLOCK(xl);
}

static Status
viaCmdBufferReset(struct _ViaCommandBuffer *buf)
{
    int ret;
    ret = ochr_reset_cmdlists(&buf->c_buf);
    if (ret)
	return BadAlloc;
    buf->c_buf.pos = 0;
    buf->mode = 0;
    return Success;
}

static void
viaCmdBufferTakedown(struct _ViaCommandBuffer *buf)
{
    wsbmBOUnrefUserList(buf->c_buf.validate_list);
    wsbmBOFreeList(buf->c_buf.validate_list);
    ochr_free_reloc_buffer(buf->c_buf.reloc_info);
    free(buf->c_buf.buf);
}

static void
agpFlush(ViaCommandBuffer * cb)
{
    int ret;
    struct _XvMCLowLevel *xl = cb->xl;

    finish_header_agp(cb);
    ret = ochr_execbuf(xl->fd, &cb->c_buf);
    if (ret)
	xl->errors |= LL_AGP_COMMAND_ERR;

    viaCmdBufferReset(cb);    
}

static void 
viaEngineSwitchNotify(XvMCLowLevel * xl,
		      enum ViaLLEngines engine)
{
    if (xl->lastEngine != engine || engine == LL_ENGINE_HQV) {
	flushXvMCLowLevel(xl);
	xl->lastEngine = engine;
    }
}

static void
uploadHQVState(XvMCLowLevel * xl, unsigned offset, Bool flip)
{
    int i;
    CARD32 tmp;
    ViaCommandBuffer *cb = &xl->agpBuf;
    struct _HQVState *state = &xl->hqvState;
    struct _HQVRegister *regs = &state->regs[0];

    viaEngineSwitchNotify(xl, LL_ENGINE_HQV);
    BEGIN_HEADER6_DATA(cb, xl, HQV_STATE_SIZE);
    cb->c_buf.exec_flags |= DRM_VIA_WAIT_BARRIER;

    if (regs[0].set)
	OUT_RING_QW_AGP(cb, 0x3CC + offset, 0);

    for (i = 4; i < HQV_STATE_SIZE; ++i) {
	if (regs[i].set) {
	    OUT_RING_QW_AGP(cb, offset + HQV_STATE_BASE + (i << 2),
			    regs[i].data);
	}
    }

    /*
     * Finally the control register for flip.
     */

    if (flip) {
	int ret;

	OUT_RING_QW_AGP(cb, offset + 0x3D4, 0);
	OUT_RING_QW_AGP(cb, offset + 0x3D8, 0);
	ret = ochr_yuv_relocation(&cb->c_buf, state->srcBuf, 0, 2,
				  state->yOffs,
				  state->uvOffs,
				  0, 1,
				  WSBM_PL_FLAG_VRAM | xl->hqvFlag,
				  WSBM_PL_MASK_MEM | xl->hqvFlag);

	tmp = GETHQVSTATE(state, 0x3D0);
	OUT_RING_QW_AGP(cb, offset + HQV_CONTROL + 0x200,
	    HQV_ENABLE | HQV_GEN_IRQ | HQV_SUBPIC_FLIP | HQV_SW_FLIP | tmp);
    }
}

unsigned
flushXvMCLowLevel(void *xlp)
{
    unsigned errors;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;

    if (xl->agpBuf.c_buf.pos)
	agpFlush(&xl->agpBuf);
    errors = xl->errors;
    if (errors)
	printf("Error 0x%x\n", errors);
    xl->errors = 0;
    xl->agpBuf.c_buf.needs_pci = 0;
    xl->agpBuf.c_buf.exec_flags = 0;
    return errors;
}

void
viaMpegSetSurfaceStride(void *xlp, ViaXvMCContext * ctx)
{
    CARD32 y_stride = ctx->yStride;
    CARD32 uv_stride = y_stride >> 1;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;

    viaEngineSwitchNotify(xl, LL_ENGINE_MPEG);
    BEGIN_HEADER6_DATA(cb, xl, 1);
    cb->c_buf.exec_flags |= DRM_VIA_WAIT_BARRIER;
    OUT_RING_QW_AGP(cb, 0xc50, (y_stride >> 3) | ((uv_stride >> 3) << 16));
}

void
viaVideoSetSWFLipLocked(void *xlp,
			struct _WsbmBufferObject *buf,
			unsigned yOffs, unsigned uOffs,
			unsigned vOffs, 
			unsigned yStride, unsigned uvStride)
{
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;

    initHQVState(&xl->hqvState);
    setHQVStartAddress(&xl->hqvState, buf, yOffs, vOffs, yStride, 0);
}

void
viaVideoSWFlipLocked(void *xlp, unsigned flags, Bool progressiveSequence)
{
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;

    setHQVDeinterlacing(&xl->hqvState, flags);
    setHQVDeblocking(&xl->hqvState,
	((flags & XVMC_FRAME_PICTURE) == XVMC_FRAME_PICTURE), TRUE);
    setHQVTripleBuffer(&xl->hqvState, TRUE);
    uploadHQVState(xl, xl->hqvOffset, TRUE);
}

void
viaMpegSetFB(void *xlp, unsigned i, 
	     struct _WsbmBufferObject *buf,
	     unsigned yOffs, unsigned uOffs, unsigned vOffs)
{
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;
    int ret;

    viaEngineSwitchNotify(xl, LL_ENGINE_MPEG);
    i *= (4 * 2);
    BEGIN_HEADER6_DATA(cb, xl, 2);
    cb->c_buf.exec_flags |= DRM_VIA_WAIT_BARRIER;
    OUT_RING_QW_AGP(cb, 0xc28 + i, 0);
    OUT_RING_QW_AGP(cb, 0xc2c + i, 0);
    ret = ochr_yuv_relocation(&cb->c_buf, buf, 0, 2,
			      yOffs, vOffs, 0, 3, 
			      WSBM_PL_FLAG_VRAM | VIA_VAL_FLAG_MPEG0,
			      WSBM_PL_MASK_MEM | VIA_VAL_FLAG_MPEG0);
}

void
viaMpegBeginPicture(void *xlp, ViaXvMCContext * ctx,
    unsigned width, unsigned height, const XvMCMpegControl * control)
{

    unsigned j, mb_width, mb_height;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;

    viaEngineSwitchNotify(xl, LL_ENGINE_MPEG);
    mb_width = (width + 15) >> 4;

    mb_height =
	((control->mpeg_coding == XVMC_MPEG_2) &&
	(control->flags & XVMC_PROGRESSIVE_SEQUENCE)) ?
	2 * ((height + 31) >> 5) : (((height + 15) >> 4));

    BEGIN_HEADER6_DATA(cb, xl, 72);
    cb->c_buf.exec_flags |= DRM_VIA_WAIT_BARRIER;


    OUT_RING_QW_AGP(cb, 0xc00,
	((control->picture_structure & XVMC_FRAME_PICTURE) << 2) |
	((control->picture_coding_type & 3) << 4) |
	((control->flags & XVMC_ALTERNATE_SCAN) ? (1 << 6) : 0));

    if (!(ctx->intraLoaded)) {
	OUT_RING_QW_AGP(cb, 0xc5c, 0);
	for (j = 0; j < 64; j += 4) {
	    OUT_RING_QW_AGP(cb, 0xc60,
		ctx->intra_quantiser_matrix[j] |
		(ctx->intra_quantiser_matrix[j + 1] << 8) |
		(ctx->intra_quantiser_matrix[j + 2] << 16) |
		(ctx->intra_quantiser_matrix[j + 3] << 24));
	}
	ctx->intraLoaded = 1;
    }

    if (!(ctx->nonIntraLoaded)) {
	OUT_RING_QW_AGP(cb, 0xc5c, 1);
	for (j = 0; j < 64; j += 4) {
	    OUT_RING_QW_AGP(cb, 0xc60,
		ctx->non_intra_quantiser_matrix[j] |
		(ctx->non_intra_quantiser_matrix[j + 1] << 8) |
		(ctx->non_intra_quantiser_matrix[j + 2] << 16) |
		(ctx->non_intra_quantiser_matrix[j + 3] << 24));
	}
	ctx->nonIntraLoaded = 1;
    }

    if (!(ctx->chromaIntraLoaded)) {
	OUT_RING_QW_AGP(cb, 0xc5c, 2);
	for (j = 0; j < 64; j += 4) {
	    OUT_RING_QW_AGP(cb, 0xc60,
		ctx->chroma_intra_quantiser_matrix[j] |
		(ctx->chroma_intra_quantiser_matrix[j + 1] << 8) |
		(ctx->chroma_intra_quantiser_matrix[j + 2] << 16) |
		(ctx->chroma_intra_quantiser_matrix[j + 3] << 24));
	}
	ctx->chromaIntraLoaded = 1;
    }

    if (!(ctx->chromaNonIntraLoaded)) {
	OUT_RING_QW_AGP(cb, 0xc5c, 3);
	for (j = 0; j < 64; j += 4) {
	    OUT_RING_QW_AGP(cb, 0xc60,
		ctx->chroma_non_intra_quantiser_matrix[j] |
		(ctx->chroma_non_intra_quantiser_matrix[j + 1] << 8) |
		(ctx->chroma_non_intra_quantiser_matrix[j + 2] << 16) |
		(ctx->chroma_non_intra_quantiser_matrix[j + 3] << 24));
	}
	ctx->chromaNonIntraLoaded = 1;
    }

    OUT_RING_QW_AGP(cb, 0xc90,
	((mb_width * mb_height) & 0x3fff) |
	((control->flags & XVMC_PRED_DCT_FRAME) ? (1 << 14) : 0) |
	((control->flags & XVMC_TOP_FIELD_FIRST) ? (1 << 15) : 0) |
	((control->mpeg_coding == XVMC_MPEG_2) ? (1 << 16) : 0) |
	((mb_width & 0xff) << 18));

    OUT_RING_QW_AGP(cb, 0xc94,
	((control->flags & XVMC_CONCEALMENT_MOTION_VECTORS) ? 1 : 0) |
	((control->flags & XVMC_Q_SCALE_TYPE) ? 2 : 0) |
	((control->intra_dc_precision & 3) << 2) |
	(((1 + 0x100000 / mb_width) & 0xfffff) << 4) |
	((control->flags & XVMC_INTRA_VLC_FORMAT) ? (1 << 24) : 0));

    OUT_RING_QW_AGP(cb, 0xc98,
	(((control->FHMV_range) & 0xf) << 0) |
	(((control->FVMV_range) & 0xf) << 4) |
	(((control->BHMV_range) & 0xf) << 8) |
	(((control->BVMV_range) & 0xf) << 12) |
	((control->flags & XVMC_SECOND_FIELD) ? (1 << 20) : 0) |
	(0x0a6 << 16));

}

void
viaMpegReset(void *xlp)
{
    int i, j;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;

    viaEngineSwitchNotify(xl, LL_ENGINE_MPEG);
    BEGIN_HEADER6_DATA(cb, xl, 99);
    cb->c_buf.exec_flags |= DRM_VIA_WAIT_BARRIER;
    
    OUT_RING_QW_AGP(cb, 0xcf0, 0);

    for (i = 0; i < 6; i++) {
	OUT_RING_QW_AGP(cb, 0xcc0, 0);
	OUT_RING_QW_AGP(cb, 0xc0c, 0x43 | 0x20);
	for (j = 0xc10; j < 0xc20; j += 4)
	    OUT_RING_QW_AGP(cb, j, 0);
    }

    OUT_RING_QW_AGP(cb, 0xc0c, 0x1c3);
    for (j = 0xc10; j < 0xc20; j += 4)
	OUT_RING_QW_AGP(cb, j, 0);

    for (i = 0; i < 19; i++)
	OUT_RING_QW_AGP(cb, 0xc08, 0);

    OUT_RING_QW_AGP(cb, 0xc98, 0x400000);

    for (i = 0; i < 6; i++) {
	OUT_RING_QW_AGP(cb, 0xcc0, 0);
	OUT_RING_QW_AGP(cb, 0xc0c, 0x1c3 | 0x20);
	for (j = 0xc10; j < 0xc20; j += 4)
	    OUT_RING_QW_AGP(cb, j, 0);
    }
    OUT_RING_QW_AGP(cb, 0xcf0, 0);

}

void
viaMpegWriteSlice(void *xlp, CARD8 * slice, int nBytes, CARD32 sCode)
{
    int i, n, r;
    CARD32 *buf;
    int count;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;

    if (xl->errors & (LL_DECODER_TIMEDOUT |
	    LL_IDCT_FIFO_ERROR | LL_SLICE_FIFO_ERROR | LL_SLICE_FAULT))
	return;

    n = nBytes >> 2;
    if (sCode)
	nBytes += 4;
    r = nBytes & 3;
    buf = (CARD32 *) slice;

    if (r)
	nBytes += 4 - r;

    nBytes += 8;

    viaEngineSwitchNotify(xl, LL_ENGINE_MPEG);
    BEGIN_HEADER6_DATA(cb, xl, 2);
    OUT_RING_QW_AGP(cb, 0xc9c, nBytes);

    if (sCode)
	OUT_RING_QW_AGP(cb, 0xca0, sCode);

    i = 0;
    count = 0;

    do {
	count += (LL_AGP_CMDBUF_SIZE - 20);
	count = (count > n) ? n : count;
	BEGIN_HEADER5_DATA(cb, xl, (count - i), 0xca0);

	for (; i < count; i++) {
	    OUT_RING_AGP(cb, *buf++);
	}
	finish_header_agp(cb);
    } while (i < n);

    BEGIN_HEADER5_DATA(cb, xl, 3, 0xca0);

    if (r) {
	OUT_RING_AGP(cb, *buf & ((1 << (r << 3)) - 1));
    }
    OUT_RING_AGP(cb, 0);
    OUT_RING_AGP(cb, 0);
    finish_header_agp(cb);
}

void
viaVideoSubPictureOffLocked(void *xlp)
{

    CARD32 stride;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;

    stride = xl->subStride;
    viaEngineSwitchNotify(xl, LL_ENGINE_HQV);
    cb->c_buf.exec_flags |= DRM_VIA_WAIT_BARRIER;
    BEGIN_HEADER6_DATA(cb, xl, 1);
    OUT_RING_QW_AGP(cb, xl->hqvOffset | SUBP_CONTROL_STRIDE | 0x200,
	stride & ~SUBP_HQV_ENABLE);
}

void
viaVideoSubPictureLocked(void *xlp, ViaXvMCSubPicture * pViaSubPic)
{

    unsigned i;
    CARD32 cWord;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;
    int ret;

    viaEngineSwitchNotify(xl, LL_ENGINE_HQV);
    BEGIN_HEADER6_DATA(cb, xl, VIA_SUBPIC_PALETTE_SIZE + 2);
    cb->c_buf.exec_flags |= DRM_VIA_WAIT_BARRIER;

    for (i = 0; i < VIA_SUBPIC_PALETTE_SIZE; ++i) {
	OUT_RING_QW_AGP(cb, xl->hqvOffset | RAM_TABLE_CONTROL | 0x200,
	    pViaSubPic->palette[i]);
    }

    cWord = (pViaSubPic->stride & SUBP_STRIDE_MASK) | SUBP_HQV_ENABLE;
    cWord |= (pViaSubPic->ia44) ? SUBP_IA44 : SUBP_AI44;
    OUT_RING_QW_AGP(cb, xl->hqvOffset | SUBP_STARTADDR | 0x200, 0);
    ret = ochr_yuv_relocation(&cb->c_buf, pViaSubPic->buf, 0, 1, 0, 0, 0, 0,
			      WSBM_PL_FLAG_VRAM | xl->hqvFlag,
			      WSBM_PL_MASK_MEM | xl->hqvFlag);
    OUT_RING_QW_AGP(cb, xl->hqvOffset | SUBP_CONTROL_STRIDE | 0x200, cWord);
    xl->subStride = cWord;	
}

void
viaBlit(void *xlp, unsigned bpp, 
	struct _WsbmBufferObject *srcBuf,
	unsigned srcDelta, unsigned srcPitch, 
	struct _WsbmBufferObject *dstBuf,
	unsigned dstDelta, unsigned dstPitch,
	unsigned w, unsigned h, int xdir, int ydir, unsigned blitMode,
	unsigned color)
{

    CARD32 dwGEMode = 0, srcY = 0, srcX = 0, dstY = 0, dstX = 0;
    CARD32 cmd;
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;
    ViaCommandBuffer *cb = &xl->agpBuf;
    int ret;


    if (!w || !h)
	return;

    finish_header_agp(cb);

    switch (bpp) {
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

    viaEngineSwitchNotify(xl, LL_ENGINE_2D);
    BEGIN_RING_AGP(cb, xl, 20);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GEMODE), dwGEMode);
    cmd = 0;

    if (xdir < 0) {
	cmd |= VIA_GEC_DECX;
	srcX = (w - 1);
	dstX = (w - 1);
    }
    if (ydir < 0) {
	cmd |= VIA_GEC_DECY;
	srcY = (h - 1);
	dstY = (h - 1);
    }

    switch (blitMode) {
    case VIABLIT_TRANSCOPY:
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCCOLORKEY), color);
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_KEYCONTROL), 0x4000);
	cmd |= VIA_GEC_BLT | (VIA_BLIT_COPY << 24);
	break;
    case VIABLIT_FILL:
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_FGCOLOR), color);
	cmd |= VIA_GEC_BLT | VIA_GEC_FIXCOLOR_PAT | (VIA_BLIT_FILL << 24);
	break;
    default:
	OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_KEYCONTROL), 0x0);
	cmd |= VIA_GEC_BLT | (VIA_BLIT_COPY << 24);
    }

    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCBASE), 0);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_SRCPOS), 0);
    ret = ochr_2d_relocation(&cb->c_buf, srcBuf, srcDelta, bpp,
			     ((srcY << 16) | srcX), 
			     WSBM_PL_FLAG_VRAM,
			     WSBM_PL_MASK_MEM);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTBASE), 0);
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DSTPOS), 0);
    ret = ochr_2d_relocation(&cb->c_buf, dstBuf, dstDelta, bpp,
			     ((dstY << 16) | dstX), 
			     WSBM_PL_FLAG_VRAM,
			     WSBM_PL_MASK_MEM);    
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_PITCH), VIA_PITCH_ENABLE |
	(srcPitch >> 3) | (((dstPitch) >> 3) << 16));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_DIMENSION),
	(((h - 1) << 16) | (w - 1)));
    OUT_RING_QW_AGP(cb, H1_ADDR(VIA_REG_GECMD), cmd);
}

static void *
releaseXvMCLowLevel(XvMCLowLevel * xl)
{
    switch (xl->state) {
    case ll_agpBuf:
	viaCmdBufferTakedown(&xl->agpBuf);
    case ll_init:
	free(xl);
    default:
	;
    }
    return NULL;
}

static Status
viaCmdBufferInit(struct _ViaCommandBuffer *buf,
		 struct _XvMCLowLevel *xl)
{
    buf->xl = xl;
    buf->c_buf.buf = (uint32_t *) malloc(LL_AGP_CMDBUF_SIZE * sizeof(uint32_t));

    if (!buf->c_buf.buf)
	return BadAlloc;
    buf->c_buf.reloc_info = ochr_create_reloc_buffer();
    if (!buf->c_buf.reloc_info)
	goto out_err0;
    
    buf->c_buf.validate_list = wsbmBOCreateList(10, 1);
    if (!buf->c_buf.validate_list)
	goto out_err1;

    buf->bufSize = LL_AGP_CMDBUF_SIZE;
    buf->flushFunc = &agpFlush;
    buf->mode = 0;

    buf->c_buf.needs_pci = 0;
    buf->c_buf.exec_flags = 0;

    if (viaCmdBufferReset(buf))
	goto out_err2;

    return Success;
 out_err2:
    wsbmBOUnrefUserList(buf->c_buf.validate_list);
    wsbmBOFreeList(buf->c_buf.validate_list);    
 out_err1:
    ochr_free_reloc_buffer(buf->c_buf.reloc_info);
 out_err0:
    free(buf->c_buf.buf);
    return BadAlloc;
}

void *
initXvMCLowLevel(int fd, drm_context_t * ctx,
    drmLockPtr hwLock, unsigned fbStride, unsigned fbDepth,
    unsigned width, unsigned height, int useAgp, unsigned chipId)
{
    XvMCLowLevel *xl;
    
    if (chipId != PCI_CHIP_VT3259 && chipId != PCI_CHIP_VT3364) {
	fprintf(stderr, "You are using an XvMC driver for the wrong chip.\n");
	fprintf(stderr, "Chipid is 0x%04x.\n", chipId);
	return NULL;
    }

    xl = (XvMCLowLevel *) malloc(sizeof(XvMCLowLevel));
    if (!xl)
	return NULL;
    xl->state = ll_init;

    if (viaCmdBufferInit(&xl->agpBuf, xl)) {
	releaseXvMCLowLevel(xl);
	return NULL;
    }
    xl->state = ll_agpBuf;

    xl->use_agp = 1;
    xl->fd = fd;
    xl->drmcontext = ctx;
    xl->hwLock = hwLock;
    xl->fbDepth = fbDepth;
    xl->fbStride = fbStride;
    xl->width = width;
    xl->height = height;
    xl->performLocking = 1;
    xl->errors = 0;
    xl->chipId = chipId;
    xl->hqvOffset = REG_HQV1_INDEX;
    xl->hqvFlag = VIA_VAL_FLAG_HQV1;
    xl->hqvState.srcBuf = NULL;
    return xl;
}

void
setLowLevelLocking(void *xlp, int performLocking)
{
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;

    xl->performLocking = performLocking;
}

void
closeXvMCLowLevel(void *xlp)
{
    XvMCLowLevel *xl = (XvMCLowLevel *) xlp;

    releaseXvMCLowLevel(xl);
}
