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

# Ensure local headers are found before any previously installed headers
AM_CPPFLAGS = -I@top_srcdir@/include

lib_LTLIBRARIES = libdpuoffloadservice.la libdpuoffloaddaemon.la

libdpuoffloadservice_la_SOURCES = host_dpu_offload_service.c 
libdpuoffloadservice_la_LIBADD =  -lpthread 
libdpuoffloadservice_la_LDFLAGS = -version-info 0:0:0
libdpuoffloadservice_la_CPPFLAGS = -I@top_srcdir@/include
libdpuoffloadservice_la_CFLAGS = -L@top_builddir@/src/.libs

libdpuoffloaddaemon_la_SOURCES = dpu_offload_service_daemon.c \
                                dpu_offload_event_channels.c \
                                dpu_offload_ops.c \
                                dpu_off_mem_mgt.h \
                                dpu_offload_xgvmi.c \
                                inter_dpus_comm.c \
                                dpu_offload_utils.c \
                                dpu_offload_group_cache.c \
                                dpu_offload_comms.h
libdpuoffloaddaemon_la_LDFLAGS = -version-info 0:0:0 
libdpuoffloaddaemon_la_CPPFLAGS = -I@top_srcdir@/include


