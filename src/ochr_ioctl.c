
#include <errno.h>

#include "ochr_ioctl.h"
#include "via_dmabuffer.h"
#include "via_drm.h"
#include "ochr_ws_driver.h"

struct via_reloc_bufinfo
{
    struct via_reloc_header *first_header;
    struct via_reloc_header *cur_header;
    struct via_reloc_header *save_first_header;
    struct via_reloc_header *save_cur_header;
    struct via_reloc_header save_old_header;
};


/*
 * Save reloc state for subsequent restoring.
 */

void
ochr_reloc_state_save(struct via_reloc_bufinfo *info)
{
    info->save_first_header = info->first_header;
    info->save_cur_header = info->cur_header;
    info->save_old_header = *info->cur_header;
}

/*
 * Free a chain of reloc buffers.
 */

static void
ochr_free_reloc_header(struct via_reloc_header *header)
{
    struct via_reloc_header *new_header;

    while (header) {
	new_header = (struct via_reloc_header *)(unsigned long)
	    header->next_header;
	free(header);
	header = new_header;
    }
}

/*
 * Restore the reloc chain to a previous state.
 */
void
ochr_reloc_state_restore(struct via_reloc_bufinfo *info)
{
    struct via_reloc_header *added_chain;

    assert(info->save_old_header.next_header == 0ULL);

    added_chain = (struct via_reloc_header *) (unsigned long) 
	info->cur_header->next_header;

    if (added_chain)
	ochr_free_reloc_header(added_chain);

    *info->save_cur_header = info->save_old_header;
}

/*
 * Add a reloc buffer to a chain.
 */

static int
ochr_add_reloc_buffer(struct via_reloc_bufinfo *info)
{
    struct via_reloc_header *header;

    header = malloc(VIA_RELOC_BUF_SIZE);

    if (!header)
	return -ENOMEM;

    info->cur_header->next_header = (uint64_t) (unsigned long)header;
    info->cur_header = header;
    header->used = sizeof(*header);
    header->num_relocs = 0;
    header->next_header = 0ULL;

    return 0;
}

/*
 * Reset a reloc chain, freeing up all but the first buffer.
 */

static int
ochr_reset_reloc_buffer(struct via_reloc_bufinfo *info)
{
    struct via_reloc_header *header;

    if (info->first_header == NULL) {
	header = info->first_header = malloc(VIA_RELOC_BUF_SIZE);
	if (!info->first_header)
	    return -ENOMEM;
    } else {
	header = info->first_header;
	ochr_free_reloc_header((struct via_reloc_header *)(unsigned long)
			      header->next_header);
    }
    header->next_header = 0ULL;
    header->used = sizeof(*header);
    info->cur_header = header;

    header->num_relocs = 0;
    return 0;
}

struct via_reloc_bufinfo *
ochr_create_reloc_buffer(void)
{
    struct via_reloc_bufinfo *tmp;
    int ret;

    tmp = calloc(1, sizeof(struct via_reloc_bufinfo));
    if (!tmp)
	return NULL;

    ret = ochr_reset_reloc_buffer(tmp);
    if (ret) {
	free(tmp);
	return NULL;
    }

    return tmp;
}

void
ochr_free_reloc_buffer(struct via_reloc_bufinfo *info)
{
    (void)ochr_reset_reloc_buffer(info);
    if (info->first_header)
	free(info->first_header);
    free(info);
}

/*
 * Add a relocation to a chain.
 */

static int
ochr_add_reloc(struct via_reloc_bufinfo *info, void *reloc, size_t size)
{
    struct via_reloc_header *header = info->cur_header;

    if (header->used + size > VIA_RELOC_BUF_SIZE) {
	int ret;

	ret = ochr_add_reloc_buffer(info);
	if (ret)
	    return ret;
	header = info->cur_header;
    }
    memcpy((unsigned char *)header + header->used, reloc, size);
    header->used += size;
    header->num_relocs++;
    return 0;
}

int
ochr_reset_cmdlists(struct _ViaCommandBuffer *cBuf)
{
    int ret;

    ret = ochr_reset_reloc_buffer(cBuf->reloc_info);
    if (ret)
	return ret;

    ret = driBOUnrefUserList(cBuf->validate_list);
    if (ret)
	return ret;

    return 0;
}

struct via_validate_buffer
{
    uint64_t flags;
    uint32_t offset;
    int po_correct;
};

static int
ochr_apply_2d_reloc(uint32_t *cmdbuf,
		    uint32_t num_buffers,
		    const struct via_validate_buffer *buffers,
		    const struct via_2d_reloc *reloc)
{
    uint32_t *buf = cmdbuf + reloc->offset;
    const struct via_reloc_bufaddr *baddr = &reloc->addr;
    const struct via_validate_buffer *val_buf;
    uint32_t val;
    uint32_t x;

    if (baddr->index > num_buffers)
	return -EINVAL;

    val_buf = &buffers[baddr->index];
    if (val_buf->po_correct)
	return 0;

    val = val_buf->offset + baddr->delta;
    x = val & 0x1f;

    if (reloc->bpp == 32)
	x >>= 2;
    else if (reloc->bpp == 16)
	x >>= 1;

    *buf = (val & ~0x1f) >> 3;
    buf += 2;
    *buf++ = reloc->pos + x;

    return 0;
}

int
ochr_2d_relocation(struct _ViaCommandBuffer *cBuf,
		   struct _DriBufferObject *buffer,
		   uint32_t delta, uint32_t bpp, uint32_t pos, 
		   uint64_t flags, uint64_t mask)
{
    struct via_2d_reloc reloc;
    struct via_validate_buffer fake;
    int itemLoc;
    struct _ValidateNode *node;
    struct via_validate_req *val_req;
    int ret;
    uint32_t tmp;
    uint32_t *cmdbuf = (uint32_t *) cBuf->buf + (cBuf->pos - 4);

    ret = driBOAddListItem(cBuf->validate_list, buffer,
			   flags, mask, &itemLoc, &node);
    if (ret)
	    return ret;

    val_req = ochrValReq(node);

    if (!(val_req->presumed_flags & VIA_USE_PRESUMED)) {
	val_req->presumed_gpu_offset = (uint64_t) driBOOffset(buffer);
	val_req->presumed_flags |= VIA_USE_PRESUMED;
    }

    fake.po_correct = 0;
    fake.offset = val_req->presumed_gpu_offset;
    if (fake.offset == 0) {
	ErrorF("Warning! Offset was 0\n");
    }
    reloc.type = VIA_RELOC_2D;
    reloc.offset = 1;
    reloc.addr.index = 0;
    reloc.addr.delta = delta;
    reloc.bpp = bpp;
    reloc.pos = pos;

    tmp = cmdbuf - (uint32_t *) cBuf->buf;

    ret = ochr_apply_2d_reloc(cmdbuf, 1, &fake, &reloc);

    reloc.addr.index = itemLoc;
    reloc.offset = tmp + 1;

    assert(ret == 0);

    return ochr_add_reloc(cBuf->reloc_info, &reloc, sizeof(reloc));
}
