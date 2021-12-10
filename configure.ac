
# -*- shell-script -*-
#
# Copyright 2021 NVIDIA CORPORATIONS. All rights reserved.
#
# See COPYING in top-level directory.
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_INIT([dpu_offload_service], [0.1], [geoffroy@nvidia.com])
AC_CONFIG_SRCDIR([include/host_dpu_offload_service.h])

AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects foreign tar-ustar])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_CONFIG_MACRO_DIR([m4])

# Checks for programs
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MKDIR_P

# Checks for header files.
AC_CHECK_HEADERS([stdint.h stdlib.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_REALLOC

AM_PROG_CC_C_O
m4_pattern_allow([AM_PROG_AR], [AM_PROG_AR])
AC_ENABLE_SHARED
AC_DISABLE_STATIC
LT_INIT

# pthread

AC_CHECK_HEADERS(pthread.h,, [AC_MSG_ERROR([pthread.h required])])
AC_SUBST([LIBS],["$LIBS -lpthread"])
AC_CHECK_LIB(pthread, pthread_create,,AC_MSG_ERROR([Cannot use pthread lib]))

dnl# pmix

AC_MSG_CHECKING([PMIx installation])
AC_ARG_WITH(pmix,
            [AS_HELP_STRING([--with-pmix=PATH],
             [Absolute path to the install directory for PMIx])],
            [PMIX_INSTALL_DIR="${withval}"],
            [PMIX_INSTALL_DIR=""])

PMIX_INC=""
PMIX_LIB=""
if test "x$PMIX_INSTALL_DIR" = "x" ; then
    AC_MSG_RESULT([no])
else
    AC_MSG_RESULT([yes])
    PMIX_INC="-I$PMIX_INSTALL_DIR/include"
    PMIX_LIB="-L$PMIX_INSTALL_DIR/lib"
fi

CPPFLAGS_save="$CPPFLAGS"
LIBS_save="$LIBS"
AC_SUBST([CPPFLAGS],["$PMIX_INC $CPPFLAGS_save -std=c99"])
AC_SUBST([LIBS],["$PMIX_LIB $LIBS_save -lpmix"])
AC_CHECK_HEADERS([pmix.h],[AC_DEFINE(HAVE_PMIX)],[AC_MSG_ERROR([Cannot find PMIX])])
AC_CHECK_LIB([pmix], [PMIx_Init],,AC_MSG_ERROR([Cannot use PMIx lib]))

dnl# prrte

AC_MSG_CHECKING([PRRTE installation])
AC_ARG_WITH(prrte,
            [AS_HELP_STRING([--with-prrte=PATH],
             [Absolute path to the install directory for PRRTE])],
            [PRRTE_INSTALL_DIR="${withval}"],
            [PRRTE_INSTALL_DIR=""])

PRRTE_INC=""
PRRTE_LIB=""
if test "x$PRRTE_INSTALL_DIR" = "x" ; then
    AC_MSG_RESULT([no])
else
    AC_MSG_RESULT([yes])
    PRRTE_INC="-I$PRRTE_INSTALL_DIR/include"
    PRRTE_LIB="-L$PRRTE_INSTALL_DIR/lib"
fi

CPPFLAGS_save="$CPPFLAGS"
LIBS_save="$LIBS"
AC_SUBST([CPPFLAGS],["$PRRTE_INC $CPPFLAGS_save -std=c99"])
AC_SUBST([LIBS],["$PRRTE_LIB $LIBS_save -lprrte"])
AC_CHECK_HEADERS([prte.h],[AC_DEFINE(HAVE_PRRTE)],[AC_MSG_ERROR([Cannot find PRRTE])])
AC_CHECK_LIB([prrte], [prte],,AC_MSG_ERROR([Cannot use PRRTE lib]))

# Generate Makefiles
AC_CONFIG_FILES([Makefile src/Makefile tests/Makefile])
AC_OUTPUT