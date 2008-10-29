#ifndef _OCHR_IOCTL_H_
#define _OCHR_IOCTL_H_

#include <stdint.h>
#include <ws_dri_bufmgr.h>
#include "via_dmabuffer.h"

struct via_reloc_bufinfo;

extern void
ochr_reloc_state_save(struct via_reloc_bufinfo *info);

extern void
ochr_reloc_state_restore(struct via_reloc_bufinfo *info);

extern struct via_reloc_bufinfo *
ochr_create_reloc_buffer(void);

extern void
ochr_free_reloc_buffer(struct via_reloc_bufinfo *info);

extern int
ochr_reset_cmdlists(struct _ViaCommandBuffer *cBuf);

extern int
ochr_2d_relocation(struct _ViaCommandBuffer *cBuf,
		   struct _DriBufferObject *buffer,
		   uint32_t delta, uint32_t bpp, uint32_t pos, 
		   uint64_t flags, uint64_t mask);
#endif
