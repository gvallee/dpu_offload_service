//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_types.h"

#ifndef DPU_OFFLOAD_EVENT_CHANNELS_H_
#define DPU_OFFLOAD_EVENT_CHANNELS_H_

int event_channels_init(dpu_offload_daemon_t *d);
int event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb);
int event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type);
int event_channel_emit(dpu_offload_event_t *ev, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size);
int event_channels_fini(dpu_offload_ev_sys_t **);

int event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev);
int event_return(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev);

#endif // DPU_OFFLOAD_EVENT_CHANNELS_H_
