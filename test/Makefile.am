AM_CPPFLAGS = \
    -DG_LOG_DOMAIN=\"libebookbackendtp-test\" \
    -g \
    -Wall \
    -Werror \
    -I$(top_srcdir)/src \
    $(EDATABOOK_CFLAGS) \
    $(DBUS_CFLAGS) \
    $(TP_CFLAGS) \
    $(MCE_CFLAGS) \
    $(SQLITE_CFLAGS)

AM_LDFLAGS =    \
    -L$(top_builddir)/src -L. \
    -lebookbackendtpcl \
    -ltestutils \
    $(EDATABOOK_LIBS) \
    $(DBUS_LIBS) \
    $(TP_LIBS) \
    $(MCE_LIBS) \
    $(SQLITE_LIBS) \
    -module -avoid-version

check_LIBRARIES = libtestutils.a

libtestutils_a_SOURCES = \
    test-utils.c  \
    test-utils.h


UNCOMPILED_TESTS = \
	add-contacts-pinocchio.sh \
	remove-contacts-pinocchio.sh \
	change-aliases-pinocchio.sh

COMPILED_TESTS =

# support programs to be compiled (will not run as tests themselves)
check_PROGRAMS = \
	add-contacts \
	remove-contacts \
	change-aliases

TESTS = $(UNCOMPILED_TESTS) $(COMPILED_TESTS)

test:
	@$(MAKE) $(AM_MAKEFLAGS) check
