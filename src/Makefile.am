SUBDIRS = lib

AM_CPPFLAGS = -I$(top_srcdir)/include

shairplay_CFLAGS =

bin_PROGRAMS = shairplay
shairplay_SOURCES = shairplay.c
shairplay_LDADD = lib/libshairplay.la
shairplay_LDFLAGS = -static-libtool-libs

if HAVE_LIBAO

  shairplay_CFLAGS += $(libao_CFLAGS)
  shairplay_LDADD += $(libao_LIBS)
endif

if HAVE_JACK
  shairplay_CFLAGS += $(JACK_CFLAGS)
  shairplay_LDADD += $(JACK_LIBS)
endif
