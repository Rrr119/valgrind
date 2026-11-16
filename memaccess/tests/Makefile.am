include $(top_srcdir)/Makefile.tool-tests.am

dist_noinst_SCRIPTS = \
	filter_flame \
	filter_stderr

EXTRA_DIST = \
	patterns.c \
	patterns.vgtest \
	patterns.stderr.exp \
	patterns.post.exp

check_PROGRAMS = patterns

AM_CFLAGS += $(AM_FLAG_M3264_PRI)
patterns_CFLAGS = $(AM_CFLAGS) -O2
