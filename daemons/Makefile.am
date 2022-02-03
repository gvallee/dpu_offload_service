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

bin_PROGRAMS = default_dpu_daemon

default_dpu_daemon_SOURCES = default_dpu_daemon.c
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -I@top_srcdir@/include -L@top_srcdir@/src/.libs
