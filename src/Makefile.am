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

lib_LTLIBRARIES = libdpuoffloadservice.la libdpuoffloaddaemon.la

libdpuoffloadservice_la_SOURCES = host_dpu_offload_service.c 
libdpuoffloadservice_la_LIBADD =  -lpthread 
libdpuoffloadservice_la_LDFLAGS = -version-info 0:0:0
libdpuoffloadservice_la_CFLAGS = -I@top_srcdir@/include -L@top_srcdir@/src/.libs

libdpuoffloaddaemon_la_SOURCES = dpu_offload_service_daemon.c \
                                dpu_offload_event_channels.c \
                                dpu_offload_ops.c \
                                dpu_offload_comm_channels.h \
                                dpu_off_mem_mgt.h \
                                dpu_offload_xgvmi.c \
                                inter_dpus_comm.c \
                                dpu_offload_utils.c
libdpuoffloaddaemon_la_LDFLAGS = -version-info 0:0:0 
libdpuoffloaddaemon_la_CFLAGS = -I@top_srcdir@/include


