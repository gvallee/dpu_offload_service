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

bin_PROGRAMS = server

server_SOURCES = server.c
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -I@top_srcdir@/include -L@top_srcdir@/src/.libs
