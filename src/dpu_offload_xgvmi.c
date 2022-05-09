#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <inttypes.h>

#include "dpu_offload_types.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_debug.h"

/**************************/
/* SENDING/RECEIVING KEYS */
/**************************/

#define cgmk_mr_crossing_t void // For now

dpu_offload_status_t send_key_to_dpu(execution_context_t *econtext, cgmk_mr_crossing_t *mr, size_t mr_size)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "execution context is undefined");
    CHECK_ERR_RETURN((econtext->type != CONTEXT_CLIENT), DO_ERROR, "Only a client running on the host can send a key to a server on a DPU");

    dpu_offload_event_t *key_send_ev;
    dpu_offload_status_t rc = event_get(econtext->event_channels, NULL, &key_send_ev);
    CHECK_ERR_RETURN((rc || !key_send_ev), DO_ERROR, "event_get() failed");

    rc = event_channel_emit_with_payload(&key_send_ev, AM_XGVMI_ADD_MSG_ID, GET_SERVER_EP(econtext), econtext->client->server_id, NULL, mr, mr_size);
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit_with_payload() failed");

    // Put the event on the ongoing events list used while progressing the execution context.
    // When event complete, we can safely return them.
    if (rc == EVENT_INPROGRESS)
        ucs_list_add_tail(&(econtext->ongoing_events), &(key_send_ev->item));

    return DO_SUCCESS;
}

dpu_offload_status_t revoke_key_on_dpu(execution_context_t *econtext, cgmk_mr_crossing_t *mr, size_t mr_size)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "execution context is undefined");
    CHECK_ERR_RETURN((econtext->type != CONTEXT_CLIENT), DO_ERROR, "Only a client running on the host can send a key to a server on a DPU");

    dpu_offload_event_t *key_revoke_ev;
    int rc = event_get(econtext->event_channels, NULL, &key_revoke_ev);
    CHECK_ERR_RETURN((rc || !key_revoke_ev), DO_ERROR, "event_get() failed");

    rc = event_channel_emit_with_payload(&key_revoke_ev, AM_XGVMI_DEL_MSG_ID, GET_SERVER_EP(econtext), econtext->client->server_id, NULL, mr, mr_size);
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit_with_payload() failed");

    // Put the event on the ongoing events list used while progressing the execution context.
    // When event complete, we can safely return them.
    if (rc == EVENT_INPROGRESS)
        ucs_list_add_tail(&(econtext->ongoing_events), &(key_revoke_ev->item));

    return DO_SUCCESS;
}