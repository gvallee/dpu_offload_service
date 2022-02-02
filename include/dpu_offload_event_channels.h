//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>

#ifndef DPU_OFFLOAD_EVENT_CHANNELS_H_
#define DPU_OFFLOAD_EVENT_CHANNELS_H_

typedef struct dpu_offload_event {
    uint64_t id;
    void *context;
    void *data;
} dpu_offload_event_t;

int event_channels_init();
int event_channel_register();
int event_channel_emit();
int event_channels_fini();

int event_get(dpu_offload_event_t **ev);
int event_fini(dpu_offload_event_t **ev);

#endif // DPU_OFFLOAD_EVENT_CHANNELS_H_
