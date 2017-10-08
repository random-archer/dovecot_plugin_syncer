
AM_CPPFLAGS = \
    $(LIBDOVECOT_INCLUDE) \
    -DMODULEDIR=\""$(dovecot_moduledir)"\"

module_LTLIBRARIES = \
    lib20_syncer_plugin.la

lib20_syncer_plugin_la_LDFLAGS = -module -avoid-version

lib20_syncer_plugin_la_SOURCES = \
    syncer-plugin.c

noinst_HEADERS = \
    syncer-plugin.h
