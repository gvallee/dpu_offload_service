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

bin_PROGRAMS = default_dpu_daemon_client default_dpu_daemon_server

# Flags for all the tests
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -I@top_srcdir@/include -L@top_builddir@/src/.libs

default_dpu_daemon_client_SOURCES = default_dpu_daemon_client.c

default_dpu_daemon_server_SOURCES = default_dpu_daemon_server.c
