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
AM_CPPFLAGS = -I@top_srcdir@/include

bin_PROGRAMS = client client_external_worker simple

# Flags used by all the tests
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -I@top_srcdir@/include -L@top_builddir@/src/.libs

client_SOURCES = client.c ../common_test_params.h client_common.h

client_external_worker_SOURCES = client_external_worker.c ../common_test_params.h client_common.h

simple_SOURCES = simple.c
