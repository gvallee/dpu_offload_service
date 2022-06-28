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

bin_PROGRAMS = config_file_dpu config_file_host

AM_LDFLAGS = -ldpuoffloaddaemon -lucp
AM_CPPFLAGS = -I@top_srcdir@/include
AM_CFLAGS = -L@top_builddir@/src/.libs

config_file_dpu_SOURCES = config_file_dpu.c

config_file_host_SOURCES = config_file_host.c
