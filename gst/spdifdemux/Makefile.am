plugin_LTLIBRARIES = libgstspdifdemux.la

libgstspdifdemux_la_SOURCES = gstspdifdemux.c
libgstspdifdemux_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_BASE_CFLAGS) \
	$(GST_CFLAGS)
libgstspdifdemux_la_LIBADD = \
	$(GST_PLUGINS_BASE_LIBS) \
	-lgstaudio-@GST_API_VERSION@ \
	$(GST_BASE_LIBS) \
	$(GST_LIBS) \
	$(LIBM)
libgstspdifdemux_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

noinst_HEADERS = gstspdifdemux.h
