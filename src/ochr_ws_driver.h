#ifndef _OCHR_WS_DRIVER_H_
#define _OCHR_WS_DRIVER_H_

#include "ws_dri_util.h"
#include "ws_dri_driver.h"
#include "via_drm.h"

struct _ViaDrmValidateNode
{
    struct _ValidateNode base;
    struct via_validate_arg val_arg;
};

extern struct _WSDriVNodeFuncs *ochrVNodeFuncs(void);

static inline struct via_validate_req *
ochrValReq(struct _ValidateNode *node)
{
    return &(containerOf(node, struct _ViaDrmValidateNode, base)->
	     val_arg.d.req);
}

#endif
