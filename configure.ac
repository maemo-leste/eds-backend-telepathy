AC_PREREQ(2.53)
AC_INIT(eds-backend-telepathy, 0.4.62)
AM_INIT_AUTOMAKE()
AC_CONFIG_SRCDIR(src/e-book-backend-tp.c)
AM_CONFIG_HEADER(config.h)

AC_CONFIG_FILES([test/Makefile])

AM_DISABLE_STATIC
AC_ISC_POSIX
AC_PROG_CC
AC_STDC_HEADERS
AC_PROG_LIBTOOL
AM_PROG_CC_C_O

PKG_CHECK_EXISTS([evolution-data-server-1.2],
                 [extensiondir=$($PKG_CONFIG --variable=extensiondir evolution-data-server-1.2 2>/dev/null)],
                 [AC_MSG_ERROR([Package requirements were not met: evolution-data-server-1.2.pc not found])])
AC_SUBST(EXTENSIONDIR, $extensiondir)

PKG_CHECK_MODULES(GIO, gio-2.0)
PKG_CHECK_MODULES(DBUS, dbus-glib-1)
PKG_CHECK_MODULES(EDATABOOK, libedata-book-1.2 libebook-1.2)
PKG_CHECK_MODULES(TP, telepathy-glib)
#PKG_CHECK_MODULES(TPR, rtcom-telepathy-glib)
PKG_CHECK_MODULES(SQLITE, sqlite3)
PKG_CHECK_MODULES([MCE], mce >= 1.5)

AC_SUBST(GIO_CFLAGS)
AC_SUBST(GIO_LIBS)

AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

AC_SUBST(EDATABOOK_CFLAGS)
AC_SUBST(EDATABOOK_LIBS)

AC_SUBST(TP_CFLAGS)
AC_SUBST(TP_LIBS)

AC_SUBST(TPR_CFLAGS)
AC_SUBST(TPR_LIBS)

AC_SUBST([MCE_LIBS])
AC_SUBST([MCE_CFLAGS])

AC_OUTPUT([
Makefile
src/Makefile
data/Makefile
])

