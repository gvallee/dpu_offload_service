//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "dpu_offload_types.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_debug.h"

dpu_offload_status_t register_new_op(offloading_engine_t *engine, offload_op_t *op, uint64_t *op_id)
{
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "engine is undefined");
    CHECK_ERR_RETURN((op == NULL), DO_ERROR, "operation is undefined");
    CHECK_ERR_RETURN((op_id == NULL), DO_ERROR, "op id is undefined");

    int slot = engine->num_registered_ops;
    engine->registered_ops[slot].alg_id = op->alg_id;
    engine->registered_ops[slot].alg_data = op->alg_data;
    engine->registered_ops[slot].op_complete = op->op_complete;
    engine->registered_ops[slot].op_fini = op->op_fini;
    engine->registered_ops[slot].op_init = op->op_init;
    engine->registered_ops[slot].op_progress = op->op_progress;
    engine->num_registered_ops++;
    *op_id = (uint64_t)slot;
    return DO_SUCCESS;
}

dpu_offload_status_t op_desc_get(offloading_engine_t *engine, const uint64_t id, uint64_t op_id, op_desc_t **desc)
{
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "engine is undefined");
    CHECK_ERR_RETURN((op_id >= engine->num_registered_ops), DO_ERROR, "invalid operation id");
    CHECK_ERR_RETURN((desc == NULL), DO_ERROR, "invalid operation descriptor handle");

    op_desc_t *d;
    DYN_LIST_GET(engine->free_op_descs, op_desc_t, item, d);
    d->id = id;
    *desc = d;
    return DO_SUCCESS;
}

dpu_offload_status_t op_desc_submit(execution_context_t *econtext, op_desc_t *desc)
{
    ucs_list_link_t *target_list;
    switch (econtext->type)
    {
    case CONTEXT_CLIENT:
        target_list = &(econtext->client->active_ops);
        break;
    case CONTEXT_SERVER:
        target_list = &(econtext->server->active_ops);
        break;
    default:
        target_list = NULL;
    }
    CHECK_ERR_RETURN((target_list == NULL), DO_ERROR, "unable to find target list");

    // Note: no need to register an notification handler for operation completion, we get one by default

    // Send the submit command to the DPU
    dpu_offload_event_t *start_ev;
    int rc = event_get(econtext->event_channels, &start_ev);
    CHECK_ERR_RETURN((rc != 0 || start_ev == NULL), DO_ERROR, "unable to get event to start the operation");

    // Add the descriptor to the local list of active operations.
    // The list is used by the notification handler so it needs to happen before
    // the event is emited.
    ucs_list_add_tail(target_list, &(desc->item));

    // Everything is now all set, emit the event associated to the notification
    void *ev_data = &(desc->id);
    size_t ev_data_len = sizeof(desc->id);
    ucp_ep_h peer_ep;
    if (econtext->type == CONTEXT_CLIENT)
    {
        peer_ep = GET_SERVER_EP(econtext);
    }
    else
    {
        // fixme: at the moment only clients can start a offload op on the DPU, so client->server
        peer_ep = NULL;
    }
    rc = event_channel_emit(start_ev, ECONTEXT_ID(econtext), AM_OP_START_MSG_ID, peer_ep, desc, ev_data, ev_data_len);
    CHECK_ERR_GOTO((rc), error_out, "event_channel_emit() failed");

    return DO_SUCCESS;

error_out:
    event_return(econtext->event_channels, &start_ev);
    return DO_ERROR;
}

dpu_offload_status_t op_desc_return(offloading_engine_t *engine, op_desc_t **desc)
{
    DYN_LIST_RETURN(engine->free_op_descs, (*desc), item);
    *desc = NULL;
    return DO_SUCCESS;
}

dpu_offload_status_t progress_active_ops(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined execution context");
    op_desc_t *cur_op, *next_op, *op = NULL;
    ucs_list_for_each_safe(cur_op, next_op, ACTIVE_OPS(econtext), item)
    {
        if (cur_op->op_definition->op_progress != NULL)
            cur_op->op_definition->op_progress();
    }
    return DO_SUCCESS;
}