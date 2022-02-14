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

dpu_offload_status_t event_channels_init(execution_context_t *);
dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb);
dpu_offload_status_t event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type);
dpu_offload_status_t event_channel_emit(dpu_offload_event_t *ev, uint64_t client_id, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size);
dpu_offload_status_t event_channels_fini(dpu_offload_ev_sys_t **);

dpu_offload_status_t event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev);
dpu_offload_status_t event_return(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev);

#endif // DPU_OFFLOAD_EVENT_CHANNELS_H_
