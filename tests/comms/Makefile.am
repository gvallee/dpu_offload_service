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

bin_PROGRAMS = self_comm

self_comm_SOURCES = self_comm.c
AM_LDFLAGS = -ldpuoffloaddaemon -lucp
AM_CFLAGS = -I@top_srcdir@/include -L@top_builddir@/src/.libs
