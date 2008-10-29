#ifndef _OCHR_IOCTL_H_
#define _OCHR_IOCTL_H_

#include <stdint.h>
#include <wsbm_manager.h>
#include "via_dmabuffer.h"

struct via_reloc_bufinfo;
struct via_reloc_texlist
{
    struct _WsbmBufferObject *buf;
    uint32_t delta;
};

extern void ochr_reloc_state_save(struct via_reloc_bufinfo *info);

extern void ochr_reloc_state_restore(struct via_reloc_bufinfo *info);

extern struct via_reloc_bufinfo *ochr_create_reloc_buffer(void);

extern void ochr_free_reloc_buffer(struct via_reloc_bufinfo *info);

extern int ochr_reset_cmdlists(struct _ViaCommandBuffer *cBuf);

extern int
ochr_2d_relocation(struct _ViaCommandBuffer *cBuf,
		   struct _WsbmBufferObject *buffer,
		   uint32_t delta, uint32_t bpp, uint32_t pos,
		   uint64_t flags, uint64_t mask);
extern int
ochr_tex_relocation(struct _ViaCommandBuffer *cBuf,
		    const struct via_reloc_texlist *addr,
		    uint32_t low_mip,
		    uint32_t hi_mip,
		    uint32_t reg_tex_fm, uint64_t flags, uint64_t mask);
extern int
ochr_dest_relocation(struct _ViaCommandBuffer *cBuf,
		     struct _WsbmBufferObject *dstBuffer,
		     uint32_t delta, uint64_t flags, uint64_t mask);
extern int
ochr_yuv_relocation(struct _ViaCommandBuffer *cBuf,
		    struct _WsbmBufferObject *buffer,
		    uint32_t delta, int planes, 
		    uint32_t plane_0, uint32_t plane_1,
		    uint32_t plane_2,
		    uint64_t flags, uint64_t mask);


extern int ochr_execbuf(int fd, struct _ViaCommandBuffer *cBuf);
#endif
