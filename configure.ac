
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

dnl # pthread

AC_CHECK_HEADERS(pthread.h,, [AC_MSG_ERROR([pthread.h required])])
AC_SUBST([CPPFLAGS],["$CPPFLAGS -D_GNU_SOURCE"])
AC_SUBST([LIBS],["$LIBS -lpthread"])
AC_CHECK_LIB(pthread, pthread_create,,AC_MSG_ERROR([Cannot use pthread lib]))

dnl # debug mode

AC_MSG_CHECKING([Check for debug mode request])
AC_ARG_ENABLE(debug,
              AS_HELP_STRING([--enable-debug],
                             [enable debugging, default: no]),
                             [case "${enableval}" in
                                yes) debug=true ;;
                                no)  debug=false ;;
                                *)   AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
                              esac],
                             [debug=false])
if test "x$enable_debug" = xyes; then
    AC_MSG_RESULT([yes])
    CFLAGS="-g -Wall -Wno-enum-compare -Wno-unused-but-set-variable -Wno-unused-function"
else
    AC_MSG_RESULT([no])
    AC_DEFINE([NDEBUG], [1], [NDEBUG set to 1 (debug disabled)])    
fi

dnl# ucx

AC_MSG_CHECKING([UCX installation])
AC_ARG_WITH(ucx,
            [AS_HELP_STRING([--with-ucx=PATH],
             [Absolute path to the install directory for UCX])],
            [UCX_INSTALL_DIR="${withval}"],
            [UCX_INSTALL_DIR=""])
UCX_INC=""
UCX_LIB=""
if test "x$UCX_INSTALL_DIR" = "x" ; then
    AC_MSG_RESULT([no])
else
    AC_MSG_RESULT([yes])
    UCX_INC="-I$UCX_INSTALL_DIR/include"
    UCX_LIB="-L$UCX_INSTALL_DIR/lib"
fi

CPPFLAGS_save="$CPPFLAGS"
LIBS_save="$LIBS"
AC_SUBST([CPPFLAGS],["$UCX_INC $CPPFLAGS_save"])
AC_SUBST([LIBS],["$UCX_LIB $LIBS_save -lucp -lucs -luct -lucm"])
AC_CHECK_HEADERS([ucp/api/ucp.h],[AC_DEFINE(HAVE_UCX)],AC_MSG_ERROR([Cannot find UCX]))
AC_CHECK_LIB([ucp],[ucp_init_version],,AC_MSG_ERROR([Cannot use UCX lib]))

dnl# pmix

pmix_happy="no"
AC_MSG_CHECKING([PMIx installation])
AC_ARG_WITH(pmix,
            [AS_HELP_STRING([--with-pmix=PATH],
             [Absolute path to the install directory for PMIx])],
            [PMIX_INSTALL_DIR="${withval}"],
            [
                PMIX_INSTALL_DIR=""
            ])

PMIX_INC=""
PMIX_LIB=""
if test "x$PMIX_INSTALL_DIR" = "x" ; then
    AC_MSG_RESULT([no])
else
    AC_MSG_RESULT([yes])
    PMIX_INC="-I$PMIX_INSTALL_DIR/include"
    PMIX_LIB="-L$PMIX_INSTALL_DIR/lib"
    pmix_happy="yes"
fi

AS_IF([test "x$pmix_happy" != xno],
    [
        CPPFLAGS_save="$CPPFLAGS"
        LIBS_save="$LIBS"
        AC_SUBST([CPPFLAGS],["$PMIX_INC $CPPFLAGS_save -DHAVE_PMIX -std=c99"])
        AC_SUBST([LIBS],["$PMIX_LIB $LIBS_save -lpmix"])
        AC_CHECK_HEADERS([pmix.h],[AC_DEFINE(HAVE_PMIX)],[AC_MSG_ERROR([Cannot find PMIX])])
        AC_CHECK_LIB([pmix],[PMIx_Init],,AC_MSG_ERROR([Cannot use PMIx lib]))
    ],
    [echo PMIx: disabled])

dnl# prrte

prrte_happy="no"
AC_MSG_CHECKING([PRRTE installation])
AC_ARG_WITH(prrte,
            [AS_HELP_STRING([--with-prrte=PATH],
             [Absolute path to the install directory for PRRTE])],
            [PRRTE_INSTALL_DIR="${withval}"],
            [
                PRRTE_INSTALL_DIR=""
            ])

PRRTE_INC=""
PRRTE_LIB=""
if test "x$PRRTE_INSTALL_DIR" = "x" ; then
    AC_MSG_RESULT([no])
else
    AC_MSG_RESULT([yes])
    PRRTE_INC="-I$PRRTE_INSTALL_DIR/include"
    PRRTE_LIB="-L$PRRTE_INSTALL_DIR/lib"
    prrte_happy="yes"
fi

AS_IF([test "x$prrte_happy" != xno],
    [
        CPPFLAGS_save="$CPPFLAGS"
        LIBS_save="$LIBS"
        AC_SUBST([CPPFLAGS],["$PRRTE_INC $CPPFLAGS_save  -DHAVE_PRRTE -std=c99"])
        AC_SUBST([LIBS],["$PRRTE_LIB $LIBS_save -lprrte"])
        AC_CHECK_HEADERS([prte.h],[AC_DEFINE(HAVE_PRRTE)],[AC_MSG_ERROR([Cannot find PRRTE])])
        AC_CHECK_LIB([prrte],[prte],,AC_MSG_ERROR([Cannot use PRRTE lib]))
    ],
    [echo PRRTE: disabled])

# Generate Makefiles
AC_CONFIG_FILES([Makefile 
                 src/Makefile
                 include/Makefile
                 daemons/Makefile
                 daemons/default/Makefile
                 daemons/job_persistent/Makefile
                 daemons/single/Makefile
                 tests/Makefile
                 tests/cache/Makefile
                 tests/process_spawn/Makefile
                 tests/offload_service/Makefile
                 tests/offload_service/server/Makefile
                 tests/offload_service/client/Makefile
                 tests/dyn_structs/Makefile
                 tests/config/Makefile
                 tests/comms/Makefile])
AC_OUTPUT
