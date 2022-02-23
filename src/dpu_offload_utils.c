//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "dpu_offload_types.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_event_channels.h"

int send_cache_entry(execution_context_t *econtext, up_ep_h ep, cache_entry_t *cache_entry)
{
    dpu_offload_event_t *send_cache_entry_ev;
    dpu_offload_status_t rc = event_get(econtext->event_channels, &send_cache_entry_ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");

    rc = event_channel_emit(send_cache_entry_ev, ECONTEXT_ID(econtext), AM_CACHE_MSG_ID, ep, NULL, cache_entry->peer_data, sizeof(cache_entry->peer_data));
    CHECCK_ERR_RETURN((rc), DO_ERROR, "event_channel_emit() failed");

    // Put the event on the ongoing events list used while progressing the execution context.
    // When event complete, we can safely return them.
    ucs_list_add_tail(&(econtext->ongoing_events), &(send_cache_ev->item));

    return DO_SUCCESS;
}