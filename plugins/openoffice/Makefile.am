AM_CPPFLAGS = \
    -DGNOMELOCALEDIR=\""$(datadir)/locale"\" 	\
    -I$(top_srcdir)/src	-I$(top_builddir)/src	\
    $(GNUMERIC_CFLAGS)

gnumeric_plugin_openofficedir = $(gnumeric_plugindir)/openoffice
xmldir = $(gnumeric_plugin_openofficedir)
gnumeric_plugin_openoffice_LTLIBRARIES = openoffice.la
openoffice_la_LDFLAGS = -module $(GNUMERIC_PLUGIN_LDFLAGS)
openoffice_la_SOURCES = 	\
	openoffice.h		\
	openoffice-read.c	\
	openoffice-write.c	\
	sfods.h			\
	sfods-read.c		\
	sfods-write.c

xml_in_files = plugin.xml.in
xml_DATA = $(xml_in_files:.xml.in=.xml)

@INTLTOOL_XML_RULE@

EXTRA_DIST = $(xml_in_files)
CLEANFILES = $(xml_DATA)
