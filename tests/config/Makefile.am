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

bin_PROGRAMS = config_file

config_file_SOURCES = config_file.c
AM_LDFLAGS = -ldpuoffloaddaemon -lucp
AM_CFLAGS = -I@top_srcdir@/include -L@top_srcdir@/src/.libs