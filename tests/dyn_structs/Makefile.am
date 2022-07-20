# -*- shell-script -*-
#
# Copyright 2021-2022 NVIDIA CORPORATIONS. All rights reserved.
#
# See COPYING in top-level directory.
#
# Additional copyrights may follow
#
# $HEADER$
#

bin_PROGRAMS = dyn_structs_test smart_buffers_test

AM_LDFLAGS = -lucp
AM_CPPFLAGS = -I@top_srcdir@/include
AM_CFLAGS = -L@top_builddir@/src/.libs

dyn_structs_test_SOURCES = dyn_structs_test.c

smart_buffers_test_SOURCES = smart_buffers_test.c
