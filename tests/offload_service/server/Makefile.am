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
AM_CPPFLAGS = -I@top_srcdir@/include

bin_PROGRAMS = server

server_SOURCES = server.c ../common_test_params.h
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -L@top_builddir@/src/.libs
