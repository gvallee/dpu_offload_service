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

SUBDIRS = 
AM_LDFLAGS = -ldpuoffloaddaemon
AM_CPPFLAGS = -I@top_srcdir@/include 
AM_CFLAGS = -L@top_builddir@/src/.libs

bin_PROGRAMS = test_cache cache_server cache_client cache_job_client cache_dpu_daemon group_hash_test hostname_hash_test

test_cache_SOURCES = test_cache.c test_cache_common.h

cache_server_SOURCES = cache_server.c test_cache_common.h

cache_client_SOURCES = cache_client.c test_cache_common.h

cache_job_client_SOURCES = cache_job_client.c

cache_dpu_daemon_SOURCES = cache_dpu_daemon.c

group_hash_test_SOURCES = group_hash_test.c

hostname_hash_test_SOURCES = hostname_hash_test.c