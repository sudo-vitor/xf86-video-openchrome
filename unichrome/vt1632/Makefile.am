# this is obnoxious:
# -module lets us name the module exactly how we want
# -avoid-version prevents gratuitous .0.0.0 version numbers on the end
# _ladir passes a dummy rpath to libtool so the thing will actually link
# TODO: -nostdlib/-Bstatic/-lgcc platform magic, not installing the .a, etc.
AM_CFLAGS = @XORG_CFLAGS@ @DRI_CFLAGS@

vt1632_la_LTLIBRARIES = vt1632.la
vt1632_la_LDFLAGS = -module -avoid-version
vt1632_ladir = @moduledir@/drivers

vt1632_la_SOURCES = \
	vt1632.c \
	vt1632_module.c \
	vt1632.h \
	vt1632_reg.h
