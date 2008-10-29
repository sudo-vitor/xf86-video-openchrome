#ifndef _OCHR_IOCTL_H_
#define _OCHR_IOCTL_H_

#include <stdint.h>
#include <wsbm_manager.h>

struct via_reloc_bufinfo;

struct ochr_cmd_buffer {
    struct via_reloc_bufinfo *reloc_info;
    struct _WsbmBufferList *validate_list;
    uint32_t *buf;
    uint32_t pos;
    int needs_pci;
    uint32_t exec_flags;
};

extern void ochr_reloc_state_save(struct via_reloc_bufinfo *info);

extern void ochr_reloc_state_restore(struct via_reloc_bufinfo *info);

extern struct via_reloc_bufinfo *ochr_create_reloc_buffer(void);

extern void ochr_free_reloc_buffer(struct via_reloc_bufinfo *info);

extern int ochr_reset_cmdlists(struct ochr_cmd_buffer *cBuf);

extern int
ochr_2d_relocation(struct ochr_cmd_buffer *cBuf,
		   struct _WsbmBufferObject *buffer,
		   uint32_t delta, uint32_t bpp, uint32_t pos,
		   uint64_t flags, uint64_t mask);
extern int
ochr_yuv_relocation(struct ochr_cmd_buffer *cBuf,
		    struct _WsbmBufferObject *buffer,
		    uint32_t delta, int planes, 
		    uint32_t plane_0, uint32_t plane_1,
		    uint32_t plane_2,
		    uint32_t shift,
		    uint64_t flags, uint64_t mask);


extern int ochr_execbuf(int fd, struct ochr_cmd_buffer *cBuf);
#endif
