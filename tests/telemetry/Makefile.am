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

bin_PROGRAMS = dpu_telemetry client_telemetry

AM_LDFLAGS = -ldpuoffloaddaemon
AM_CPPFLAGS = -I@top_srcdir@/include
AM_CFLAGS = -L@top_builddir@/src/.libs

dpu_telemetry_SOURCES = dpu_telemetry.c

client_telemetry_SOURCES = client_telemetry.c
