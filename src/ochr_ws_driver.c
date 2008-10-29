#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include "ochr_ws_driver.h"


/*
 * Allocate a validate list node. On the list of drm buffers, which is
 * identified by driver_private == 0, we allocate a derived item which 
 * also contains a drm validate arg, which means we can start to fill
 * this in immediately.
 */

static struct _ValidateNode *vn_alloc(struct _WSDriVNodeFuncs *func,
				      int driver_private)
{
    if (driver_private == 0) {
	struct _ViaDrmValidateNode *vNode = malloc(sizeof(*vNode));

	vNode->base.func = func;
	vNode->base.driver_private = 0;
	return &vNode->base;
    } else {
	struct _ValidateNode *node = malloc(sizeof(*node));

	node->func = func;
	node->driver_private = 1;
	return node;
    }
}

/*
 * Free an allocated validate list node. 
 */

static void vn_free(struct _ValidateNode *node) 
{
    if (node->driver_private == 0) 
	free(containerOf(node, struct _ViaDrmValidateNode, base));
    else
	free(node);
}

/*
 * Clear the private part of the validate list node. This happens when
 * the list node is newly allocated or is being reused. Since we only have
 * a private part when node->driver_private == 0 we only care to clear in that
 * case. We want to clear the drm ioctl argument.
 */

static void vn_clear(struct _ValidateNode *node) {
    if (node->driver_private == 0) {
	struct _ViaDrmValidateNode *vNode = 
	    containerOf(node, struct _ViaDrmValidateNode, base);
	
	memset(&vNode->val_arg.d.req, 0, sizeof(vNode->val_arg.d.req));
    }
}

    
static struct _WSDriVNodeFuncs viaVNode = {
    .alloc = vn_alloc,
    .free = vn_free,
    .clear = vn_clear,
};

struct _WSDriVNodeFuncs *ochrVNodeFuncs(void)
{
    return &viaVNode;
}
