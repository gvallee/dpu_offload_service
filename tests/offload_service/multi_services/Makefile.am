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

AM_CPPFLAGS = -I@top_srcdir@/include
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -L@top_builddir@/src/.libs

bin_PROGRAMS = two_engines_dpu two_engines_host

two_engines_dpu_SOURCES = two_engines_dpu.c
two_engines_host_SOURCES = two_engines_host.c
