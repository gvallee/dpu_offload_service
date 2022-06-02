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

bin_PROGRAMS = config_file_dpu

config_file_dpu_SOURCES = config_file_dpu.c
AM_LDFLAGS = -ldpuoffloaddaemon -lucp
AM_CFLAGS = -I@top_srcdir@/include -L@top_builddir@/src/.libs
