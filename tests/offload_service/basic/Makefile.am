# -*- shell-script -*-
#
# Copyright 2022 NVIDIA CORPORATIONS. All rights reserved.
#
# See COPYING in top-level directory.
#
# Additional copyrights may follow
#
# $HEADER$
#
bin_PROGRAMS = daemon_basic_init

# Flags used by all the tests
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CPPFLAGS = -I@top_srcdir@/include
AM_CFLAGS = -L@top_builddir@/src/.libs

daemon_basic_init_SOURCES = daemon_basic_init.c
