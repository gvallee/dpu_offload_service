# -*- shell-script -*-
#
# Copyright 2021 NVIDIA CORPORATIONS. All rights reserved.
#
# See COPYING in top-level directory.
#
# Additional copyrights may follow
#
# $HEADER$
#

lib_LTLIBRARIES = libdpuoffloadservice.la

libdpuoffloadservice_la_SOURCES = host_dpu_offload_service.c
libdpuoffloadservice_la_LIBADD =  /global/scratch/users/geoffroy/projects/pmix/x86/prrte/src/tools/prun/libprun.la -lpthread
libdpuoffloadservice_la_LDFLAGS = -version-info 0:0:0 
libdpuoffloadservice_la_CFLAGS = -I@top_srcdir@/include -I/global/scratch/users/geoffroy/projects/pmix/x86/prrte/src/include