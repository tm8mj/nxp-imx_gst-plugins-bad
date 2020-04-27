
plugin_LTLIBRARIES = libgsttinycompress.la

libgsttinycompress_la_SOURCES = tinycompresssink.c 	\
                            tinycompress.c

libgsttinycompress_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_CFLAGS) \
    -Wno-deprecated-declarations
libgsttinycompress_la_LIBADD =            \
    -ltinycompress                        \
    -lgstaudio-@GST_API_VERSION@      \
    $(GST_PLUGINS_BASE_LIBS)          \
    $(GST_BASE_LIBS)                  \
    $(GST_LIBS)
libgsttinycompress_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

noinst_HEADERS = tinycompresssink.h

