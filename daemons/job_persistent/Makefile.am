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

bin_PROGRAMS = job_persistent_dpu_daemon

job_persistent_dpu_daemon_SOURCES = job_persistent_dpu_daemon.c
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -L@top_builddir@/src/.libs
AM_CPPFLAGS = -I@top_srcdir@/include
