if XVMC
#lib_LTLIBRARIES=libchromeXvMC.la libchromeXvMCPro.la
lib_LTLIBRARIES=libchromeXvMCPro.la

libchromeXvMCPro_la_SOURCES = \
	driDrawable.c \
	xf86dri.c \
	ochr_ioctl.c \
	viaXvMC.c \
	viaLowLevelPro.c \
	viaLowLevel.h \
	driDrawable.h \
	viaXvMCPriv.h \
	xf86dri.h \
	xf86dristr.h \
	vldXvMC.h

libchromeXvMCPro_la_CFLAGS = @XORG_CFLAGS@ @DRI_CFLAGS@ @libwsbm_CFLAGS@ -Wall\
	-I$(top_srcdir)/src -I$(top_srcdir)/libxvmc -DTRUE=1 -DFALSE=0
libchromeXvMCPro_la_LDFLAGS = -version-number 1:0:0
libchromeXvMCPro_la_LIBADD = @DRI_LIBS@ @libwsbm_LIBS@
else
EXTRA_DIST = \
	driDrawable.c \
	driDrawable.h \
	viaLowLevelPro.c \
	viaLowLevel.c \
	viaLowLevel.h \
	viaXvMC.c \
	viaXvMCPriv.h \
	xf86dri.c \
	xf86dri.h \
	xf86dristr.h
endif
