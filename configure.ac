AC_PREREQ([2.69])
AC_INIT([chxrt], [0.1.0], [me@chromo.dev])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CONFIG_SRCDIR([src/rt.c])

AC_LANG([C])
AC_PROG_CC
AM_PROG_AR
LT_INIT

AC_MSG_CHECKING([whether $CC supports -std=c23])
save_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS -std=c23"
AC_COMPILE_IFELSE(
	[AC_LANG_PROGRAM([], [[]])],
	[AC_MSG_RESULT([yes])],
	[AC_MSG_FAILURE([$CC does not support -std=c23])]
)
CFLAGS="$save_CFLAGS -std=c23"

TEST_LDADD="\$(top_builddir)/src/libchxrt.la"
AC_SUBST([TEST_LDADD])

AC_CONFIG_FILES([
	Makefile
	src/Makefile
	test/Makefile
])
AC_OUTPUT
