#ifndef _OCHR_WS_DRIVER_H_
#define _OCHR_WS_DRIVER_H_

#include "wsbm_util.h"
#include "wsbm_driver.h"
#include "ochr_drm.h"

struct _ViaDrmValidateNode
{
    struct _ValidateNode base;
    struct drm_via_validate_arg val_arg;
};

extern struct _WsbmVNodeFuncs *ochrVNodeFuncs(void);

static inline struct drm_via_validate_req *
ochrValReq(struct _ValidateNode *node)
{
    return &(containerOf(node, struct _ViaDrmValidateNode, base)->
	     val_arg.d.req);
}

#endif
