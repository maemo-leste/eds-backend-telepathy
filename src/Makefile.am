backenddir = $(EXTENSIONDIR)

AM_CPPFLAGS = \
	$(GIO_CFLAGS) \
	$(EDATABOOK_CFLAGS) \
	$(DBUS_CFLAGS) \
	$(TP_CFLAGS) \
	$(TPR_CFLAGS) \
	$(MCE_CFLAGS) \
	$(SQLITE_CFLAGS)

AM_CFLAGS = \
	-DG_LOG_DOMAIN=\"libebookbackendtp\" \
	-Wall

noinst_LTLIBRARIES = libebookbackendtpcl.la

libebookbackendtpcl_la_LIBADD =	\
	$(GIO_LIBS) \
	$(EDATABOOK_LIBS) \
	$(DBUS_LIBS) \
	$(TP_LIBS) \
	$(TPR_LIBS) \
	$(MCE_LIBS) \
	$(SQLITE_LIBS)

libebookbackendtpcl_la_SOURCES =	\
	e-book-backend-tp-types.c \
	e-book-backend-tp-types.h \
	e-book-backend-tp-log.c \
	e-book-backend-tp-log.h \
	e-book-backend-tp-cl.c \
	e-book-backend-tp-cl.h \
	e-book-backend-tp-contact.c \
	e-book-backend-tp-contact.h

backend_LTLIBRARIES = libebookbackendtp.la

libebookbackendtp_la_SOURCES =	\
	e-book-backend-tp-factory.c	\
	e-book-backend-tp.h		\
	e-book-backend-tp.c		\
	e-book-backend-tp-db.h		\
	e-book-backend-tp-db.c

libebookbackendtp_la_LIBADD = 	\
	libebookbackendtpcl.la \
	$(GIO_LIBS) \
	$(EDATABOOK_LIBS) \
	$(DBUS_LIBS) \
	$(TP_LIBS) \
	$(TPR_LIBS) \
	$(MCE_LIBS) \
	$(SQLITE_LIBS)

libebookbackendtp_la_LDFLAGS = -avoid-version -module

noinst_PROGRAMS = \
	test-tpcl

test_tpcl_SOURCES = test-tpcl.c
test_tpcl_LDADD = \
	libebookbackendtpcl.la

install-data-hook:
	@(cd $(DESTDIR)$(backenddir) && $(RM) $(backend_LTLIBRARIES))
