/*
 * Copyright C 2013 The Openchrome Project  [openchrome.org]
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    James Simmons <jsimmons@infradead.org>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xf86.h>
#define GLAMOR_FOR_XORG  1
#include <glamor.h>
#include "via_driver.h"

#if HAS_DEVPRIVATEKEYREC
DevPrivateKeyRec glamor_pixmap_index;
#else
int glamor_pixmap_index;
#endif

Bool
via_glamor_pre_init(ScrnInfoPtr pScrn)
{
    VIAPtr pVia = VIAPTR(pScrn);
	pointer glamor_module;
	CARD32 version;

	if (!xf86LoaderCheckSymbol("glamor_egl_init")) {
		xf86DrvMsg(pScrn->scrnIndex,  X_ERROR,
			   "glamor requires Load \"glamoregl\" in "
			   "Section \"Module\", disabling.\n");
		return FALSE;
	}

	/* Load glamor module */
	if ((glamor_module = xf86LoadSubModule(pScrn, GLAMOR_EGL_MODULE_NAME))) {
		version = xf86GetModuleVersion(glamor_module);
		if (version < MODULE_VERSION_NUMERIC(0,3,1)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Incompatible glamor version, required >= 0.3.0.\n");
		} else {
			if (glamor_egl_init(pScrn, pVia->drmmode.fd)) {
				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
					   "glamor detected, initialising egl layer.\n");
                return TRUE;
			} else
				xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
					   "glamor detected, failed to initialize egl.\n");
		}
	} else
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "glamor not available\n");

	return FALSE;
}

Bool
via_glamor_init(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

    if (!glamor_init(pScreen, GLAMOR_INVERTED_Y_AXIS | GLAMOR_USE_EGL_SCREEN |
					 GLAMOR_USE_SCREEN | GLAMOR_USE_PICTURE_SCREEN)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to initialize glamor.\n");
        return FALSE;
    }

    if (!glamor_egl_init_textured_pixmap(pScreen)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to initialize textured pixmap of screen for glamor.\n");
        return FALSE;
    }

#if HAS_DIXREGISTERPRIVATEKEY
	if (!dixRegisterPrivateKey(&glamor_pixmap_index, PRIVATE_PIXMAP, 0))
#else
	if (!dixRequestPrivate(&glamor_pixmap_index, 0))
#endif
		return FALSE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "Use GLAMOR acceleration.\n");
    return TRUE;
}
