AC_PREREQ([2.68])
AC_INIT([daemonproxy], [0], [mike@nrdvana.net])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])

AC_ARG_ENABLE(static, AS_HELP_STRING([static], [enable static compilation]),
 [ if test "$enableval" != "no"; then LDFLAGS="$LDFLAGS -static"; fi; ],
 [ ])

AC_ARG_ENABLE(debug,  AS_HELP_STRING([debug], [enable assertions and debug symbols]),
 [ if test "$enableval" != "no"; then CFLAGS="$CFLAGS -O0 -g3"; else CFLAGS="$CFLAGS -O2 -DNDEBUG"; fi; ],
 [ CFLAGS="$CFLAGS -O0 -g3"; ])

AC_ARG_ENABLE(dev,    AS_HELP_STRING([dev], [enable source generators (requires perl)]),
 [ if test "$enableval" != "no"; then dev_include_makefile="\$(scriptdir)/dev_rules.mak"; fi; ],
 [ dev_include_makefile="\$(scriptdir)/dev_rules.mak"; ])
AC_SUBST(dev_include_makefile)

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB([rt], [clock_gettime])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h stdint.h stdlib.h string.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_INT64_T
AC_TYPE_PID_T
AC_TYPE_UINT64_T

# Checks for library functions.
AC_FUNC_ALLOCA
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_CHECK_FUNCS([clock_gettime dup2 memchr memmove memset mkdir select setenv strchr strdup strstr])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
