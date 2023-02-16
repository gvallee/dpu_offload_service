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

bin_PROGRAMS = dpu_ping_pong client_ping_pong

AM_LDFLAGS = -ldpuoffloaddaemon
AM_CPPFLAGS = -I@top_srcdir@/include
AM_CFLAGS = -L@top_builddir@/src/.libs

dpu_ping_pong_SOURCES = dpu_ping_pong.c

client_ping_pong_SOURCES = client_ping_pong.c
