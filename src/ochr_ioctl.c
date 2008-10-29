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

static int
ochr_apply_texture_reloc(uint32_t ** cmdbuf,
			uint32_t num_buffers,
			struct via_validate_buffer *buffers,
			const struct via_texture_reloc *reloc)
{
    const struct via_reloc_bufaddr *baddr = reloc->addr;
    uint32_t baseh[4];
    uint32_t *buf = *cmdbuf + reloc->offset;
    uint32_t val;
    int i;
    int basereg;
    int shift;
    uint64_t flags = 0;
    uint32_t reg_tex_fm;

    memset(baseh, 0, sizeof(baseh));

    for (i = 0; i <= (reloc->hi_mip - reloc->low_mip); ++i) {
	if (baddr->index > num_buffers) {
	    *cmdbuf = buf;
	    return -EINVAL;
	}

	val = buffers[baddr->index].offset + baddr->delta;
	if (i == 0) 
	    flags = buffers[baddr->index].flags;

	*buf++ = ((HC_SubA_HTXnL0BasL + i) << 24) | (val & 0x00FFFFFF);
	basereg = i / 3;
	shift = (3 - (i % 3)) << 3;

	baseh[basereg] |= (val & 0xFF000000) >> shift;
	baddr++;
    }

    if (reloc->low_mip < 3)
	*buf++ = baseh[0] | (HC_SubA_HTXnL012BasH << 24);
    if (reloc->low_mip < 6 && reloc->hi_mip > 2)
	*buf++ = baseh[1] | (HC_SubA_HTXnL345BasH << 24);
    if (reloc->low_mip < 9 && reloc->hi_mip > 5)
	*buf++ = baseh[2] | (HC_SubA_HTXnL678BasH << 24);
    if (reloc->hi_mip > 8)
	*buf++ = baseh[3] | (HC_SubA_HTXnL9abBasH << 24);

    reg_tex_fm = reloc->reg_tex_fm & ~HC_HTXnLoc_MASK;

    if (flags & DRM_BO_FLAG_MEM_VRAM) {
	reg_tex_fm |=  HC_HTXnLoc_Local;
    } else if (flags & (DRM_BO_FLAG_MEM_TT | VIA_BO_FLAG_MEM_AGP)) {
	reg_tex_fm |=  HC_HTXnLoc_AGP;
    } else
	abort();

    *buf++ = reg_tex_fm;
    *cmdbuf = buf;

    return 0;
}

int
ochr_tex_relocation(struct _ViaCommandBuffer *cBuf,
		    const struct via_reloc_texlist *addr,
		    uint32_t low_mip,
		    uint32_t hi_mip,
		    uint32_t reg_tex_fm, uint64_t flags, uint64_t mask)
{
    struct via_texture_reloc reloc;
    struct via_validate_buffer fake[12];
    struct via_reloc_bufaddr fake_addr[12];
    struct via_reloc_bufaddr real_addr[12];
    int itemLoc;
    struct _ValidateNode *node;
    struct via_validate_req *val_req;
    int ret;
    uint32_t tmp;
    int i;
    int count = 0;
    size_t size;
    uint32_t *cmdBuf = (uint32_t *) cBuf->buf + cBuf->pos;


    driReadLockKernelBO();
    for (i = 0; i <= (hi_mip - low_mip); ++i) {
	ret = driBOAddListItem(cBuf->validate_list, addr[i].buf,
			       flags, mask, &itemLoc, &node);
	if (ret)
		return ret;

	val_req = ochrValReq(node);

	if (!(val_req->presumed_flags & VIA_USE_PRESUMED)) {
	    val_req->presumed_gpu_offset = (uint64_t) driBOOffset(addr[i].buf);
	    val_req->presumed_flags |= VIA_USE_PRESUMED;
	}

	fake[count].po_correct = 0;
	fake[count].offset = val_req->presumed_gpu_offset;
	fake[count].flags = driBOFlags(addr[i].buf);
	real_addr[count].index = itemLoc;
	real_addr[count].delta = addr[i].delta;
	fake_addr[count].index = count;
	fake_addr[count].delta = addr[i].delta;
	count++;
    }
    driReadUnlockKernelBO();

    reloc.type = VIA_RELOC_TEX;
    reloc.offset = 0;
    reloc.low_mip = low_mip;
    reloc.hi_mip = hi_mip;
    reloc.reg_tex_fm = reg_tex_fm;
    memcpy(reloc.addr, fake_addr, count * sizeof(struct via_reloc_bufaddr));
    tmp = cBuf->pos;

    ret = ochr_apply_texture_reloc(&cmdBuf, count, fake, &reloc);
    cBuf->pos = cmdBuf - (uint32_t *)cBuf->buf;

    memcpy(reloc.addr, real_addr, count * sizeof(struct via_reloc_bufaddr));
    reloc.offset = tmp;

    size = offsetof(struct via_texture_reloc, addr) -
	offsetof(struct via_texture_reloc, type) +
	count * sizeof(struct via_reloc_bufaddr);

    assert(ret == 0);

    return ochr_add_reloc(cBuf->reloc_info, &reloc, size);
}


static int
ochr_apply_dest_reloc(uint32_t ** cmdbuf,
		      uint32_t num_buffers,
		      struct via_validate_buffer *buffers,
		      const struct via_zbuf_reloc *reloc)
{
    uint32_t *buf = *cmdbuf + reloc->offset;
    const struct via_reloc_bufaddr *baddr = &reloc->addr;
    const struct via_validate_buffer *val_buf;
    uint32_t val;

    if (baddr->index > num_buffers)
	return -EINVAL;

    val_buf = &buffers[baddr->index];
    if (val_buf->po_correct)
	return 0;

    val = val_buf->offset + baddr->delta;
    *buf++ = (HC_SubA_HDBBasL << 24) | (val & 0xFFFFFF);
    *buf++ = (HC_SubA_HDBBasH << 24) | ((val & 0xFF000000) >> 24);

    *cmdbuf = buf;
    return 0;
}


int
ochr_dest_relocation(struct _ViaCommandBuffer *cBuf,
		     struct _DriBufferObject *dstBuffer,
		     uint32_t delta,
		     uint64_t flags, uint64_t mask)
{
    struct via_zbuf_reloc reloc;
    struct via_validate_buffer fake;
    int itemLoc;
    struct _ValidateNode *node;
    struct via_validate_req *val_req;
    int ret;
    uint32_t tmp;
    uint32_t *cmdbuf = (uint32_t *)cBuf->buf + cBuf->pos;

    ret = driBOAddListItem(cBuf->validate_list, dstBuffer,
			   flags, mask, &itemLoc, &node);
    if (ret)
	return ret;

    val_req = ochrValReq(node);

    if (!(val_req->presumed_flags & VIA_USE_PRESUMED)) {
	driReadLockKernelBO();
	val_req->presumed_gpu_offset = (uint64_t) driBOOffset(dstBuffer);
	driReadUnlockKernelBO();
	val_req->presumed_flags |= VIA_USE_PRESUMED;
    }

    fake.po_correct = 0;
    fake.offset = val_req->presumed_gpu_offset;

    reloc.type = VIA_RELOC_DSTBUF;
    reloc.offset = 0;
    reloc.addr.index = 0;
    reloc.addr.delta = delta;

    tmp = cBuf->pos;
    (void) ochr_apply_dest_reloc(&cmdbuf, 1, &fake, &reloc);

    reloc.addr.index = itemLoc;
    reloc.offset = tmp;
    cBuf->pos = cmdbuf - (uint32_t *)cBuf->buf;

    return ochr_add_reloc(cBuf->reloc_info, &reloc, sizeof(reloc));
}


int ochr_execbuf(int fd, struct _ViaCommandBuffer *cBuf)
{
    union via_ttm_execbuf_arg arg;
    struct via_ttm_execbuf_req *exec_req = &arg.req;
    struct _ValidateList *valList;
    struct _ValidateNode *node;
    struct _ViaDrmValidateNode *viaNode;
    struct via_validate_arg *val_arg;
    struct via_validate_req *req;
    struct via_validate_rep *rep;

    uint64_t first = 0ULL;
    uint64_t *prevNext = NULL;
    void *iterator;
    uint32_t count = 0;
    int ret;
    
    /*
     * Prepare arguments for all buffers that need validation
     * prior to the command submission.
     */

    valList = driGetDRMValidateList(cBuf->validate_list);
    iterator = validateListIterator(valList);
    while (iterator) {
	node = validateListNode(iterator);
	viaNode = containerOf(node, struct _ViaDrmValidateNode, base);
	val_arg = &viaNode->val_arg;
	val_arg->handled = 0;
	val_arg->ret = 0;
	req = &val_arg->d.req;

	if (!first) 
	    first = (uint64_t) (unsigned long) val_arg;
	if (prevNext)
	    *prevNext = (uint64_t) (unsigned long) val_arg;
	prevNext = &req->next;
	
	req->buffer_handle = wsDriKbufHandle((struct _DriKernelBuf *)
					     node->buf);
	req->group = 0;

	iterator = validateListNext(valList, iterator);
	++count;
    }

    /*
     * Fill in the execbuf arg itself.
     */

    exec_req->buffer_list = first;
    exec_req->num_buffers = count;
    exec_req->reloc_list = (uint64_t) (unsigned long) 
	cBuf->reloc_info->first_header;
    exec_req->cmd_buffer = (uint64_t) (unsigned long) 
	cBuf->buf;
    exec_req->cmd_buffer_size = cBuf->pos << 2;
    exec_req->engine = 0;
    exec_req->exec_flags = 0x00000000;
    exec_req->cliprect_offset = 0;
    exec_req->num_cliprects = 0;

    do {
	ret = drmCommandWriteRead(fd, DRM_VIA_TTM_EXECBUF, 
				  &arg, sizeof(arg));
    }while(ret == EAGAIN || ret == EINTR);
    
    iterator = validateListIterator(valList);

    /*
     * Update all user-space cached offsets and flags for kernel
     * buffers involved in this commands.
     */

    while (iterator) {
	node = validateListNode(iterator);
	viaNode = containerOf(node, struct _ViaDrmValidateNode, base);
	val_arg = &viaNode->val_arg;

	if (!val_arg->handled)
	    break;
	
	if (val_arg->ret != 0) {
	    xf86DrvMsg(cBuf->pScrn->scrnIndex, X_ERROR,
		       "Failed a buffer validation: \"%s\".\n",
		       strerror(-val_arg->ret));
	    iterator = validateListNext(valList, iterator);
	    continue;
	}

	rep = &val_arg->d.rep;
	wsDriUpdateKbuf((struct _DriKernelBuf *) node->buf,
			rep->gpu_offset, rep->flags);

	iterator = validateListNext(valList, iterator);
    }

    return ret;
}
