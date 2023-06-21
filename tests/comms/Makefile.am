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

bin_PROGRAMS = self_comm self_notif_mem_pools notif_iovec

AM_LDFLAGS = -ldpuoffloaddaemon -lucp
AM_CFLAGS = -L@top_builddir@/src/.libs
AM_CPPFLAGS = -I@top_srcdir@/include

self_comm_SOURCES = self_comm.c

self_notif_mem_pools_SOURCES = self_notif_mem_pools.c

notif_iovec_SOURCES = notif_iovec.c