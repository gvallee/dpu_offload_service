//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <inttypes.h>

#include "dpu_offload_types.h"
#include "dpu_offload_event_channels.h"

/**************************/
/* SENDING/RECEIVING KEYS */
/**************************/

#define cgmk_mr_crossing_t void // For now

int send_key_to_dpu(execution_context_t *econtext, cgmk_mr_crossing_t *mr, size_t mr_size)
{
    assert(econtext);
    // Only a client running on the host can send a key to a server on a DPU
    assert(econtext->type == CONTEXT_CLIENT);

    dpu_offload_event_t *key_send_ev;
    int rc = event_get(econtext->event_channels, &key_send_ev);
    if (rc || !key_send_ev)
    {
        fprintf(stderr, "event_get() failed\n");
        return -1;
    }

    rc = event_channel_emit(key_send_ev, econtext->client->id, AM_XGVMI_ADD_MSG_ID, GET_SERVER_EP(econtext), NULL, mr, mr_size);
    if (rc)
    {
        fprintf(stderr, "event_channel_emit() failed\n");
        return -1;
    }

    // Put the event on the ongoing events list used while progressing the execution context.
	// When event complete, we can safely return them.
	ucs_list_add_tail(&(econtext->ongoing_events), &(key_send_ev->item));

	return 0;
}

int revoke_key_on_dpu(execution_context_t *econtext, cgmk_mr_crossing_t *mr, size_t mr_size)
{
    assert(econtext);
    // Only a client running on the host can send a key to a server on a DPU
    assert(econtext->type == CONTEXT_CLIENT);

    dpu_offload_event_t *key_revoke_ev;
    int rc = event_get(econtext->event_channels, &key_revoke_ev);
    if (rc || !key_revoke_ev)
    {
        fprintf(stderr, "event_get() failed\n");
        return -1;
    }

    rc = event_channel_emit(key_revoke_ev, econtext->client->id, AM_XGVMI_DEL_MSG_ID, GET_SERVER_EP(econtext), NULL, mr, mr_size);
    if (rc)
    {
        fprintf(stderr, "event_channel_emit() failed\n");
        return -1;
    }

    // Put the event on the ongoing events list used while progressing the execution context.
	// When event complete, we can safely return them.
	ucs_list_add_tail(&(econtext->ongoing_events), &(key_revoke_ev->item));

    return 0;
}