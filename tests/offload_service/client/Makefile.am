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

bin_PROGRAMS = client client_external_worker simple fake_mpi_rank

# Flags used by all the tests
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CFLAGS = -I@top_srcdir@/include -L@top_srcdir@/src/.libs

client_SOURCES = client.c ../common_test_params.h client_common.h

client_external_worker_SOURCES = client_external_worker.c ../common_test_params.h client_common.h

simple_SOURCES = simple.c

fake_mpi_rank_SOURCES = fake_mpi_rank.c