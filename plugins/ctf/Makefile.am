SUBDIRS = common \
	fs-src \
	fs-sink \
	lttng-live

noinst_HEADERS = print.h

plugindir = "$(PLUGINSDIR)"
plugin_LTLIBRARIES = babeltrace-plugin-ctf.la

# ctf plugin
babeltrace_plugin_ctf_la_SOURCES = plugin.c

babeltrace_plugin_ctf_la_LDFLAGS = \
	$(LT_NO_UNDEFINED) \
	-avoid-version -module

babeltrace_plugin_ctf_la_LIBADD = \
	common/libbabeltrace-plugin-ctf-common.la \
	fs-sink/libbabeltrace-plugin-ctf-fs-sink.la \
	fs-src/libbabeltrace-plugin-ctf-fs-src.la \
	lttng-live/libbabeltrace-plugin-ctf-lttng-live.la

if !ENABLE_BUILT_IN_PLUGINS
babeltrace_plugin_ctf_la_LIBADD += \
	$(top_builddir)/lib/libbabeltrace.la \
	$(top_builddir)/logging/libbabeltrace-logging.la \
	$(top_builddir)/common/libbabeltrace-common.la \
	$(top_builddir)/ctfser/libbabeltrace-ctfser.la
endif
