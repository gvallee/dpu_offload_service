//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_debug.h"

#ifndef _DPU_OFFLOAD_COMM_CHANNELS_H
#define _DPU_OFFLOAD_COMM_CHANNELS_H

static ucs_status_t am_term_msg_cb(void *arg, const void *header, size_t header_length,
                                   void *data, size_t length,
                                   const ucp_am_recv_param_t *param)
{
    execution_context_t *d = (execution_context_t *)arg;
    DBG("TERM msg received (handle=%p)", d);
    CHECK_ERR_RETURN((d == NULL), DO_ERROR, "handle is NULL");

    switch (d->type)
    {
    case CONTEXT_CLIENT:
        d->client->done = true;
        break;
    case CONTEXT_SERVER:
#if 0
        // the client id provided by the client is actually the slot where the client info is
        am_header_t *hdr = (am_header_t*)header;
        int idx = (int)hdr->id;  // todo: track which client is connected/disconnected
        d->server->connected_clients.clients[idx].status = DISCONNECTED;
#endif
        d->server->connected_clients.num_connected_clients--;
        DBG("Remaining number of connected clients: %ld", d->server->connected_clients.num_connected_clients);
        if (d->server->connected_clients.num_connected_clients == 0)
        {
            DBG("server is now done");
            d->server->done = true;
        }
        break;
    default:
        ERR_MSG("invalid type");
        return UCS_ERR_NO_MESSAGE;
    }
    return UCS_OK;
}

static dpu_offload_status_t dpu_offload_set_am_recv_handlers(execution_context_t *ctx)
{
    CHECK_ERR_RETURN((ctx == NULL), DO_ERROR, "undefined context");
    ucp_worker_h worker = GET_WORKER(ctx);
    CHECK_ERR_RETURN((worker == NULL), DO_ERROR, "undefined worker");

    ucp_am_handler_param_t term_param;
    term_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                            UCP_AM_HANDLER_PARAM_FIELD_CB |
                            UCP_AM_HANDLER_PARAM_FIELD_ARG;
    term_param.id = AM_TERM_MSG_ID;
    term_param.cb = am_term_msg_cb;
    term_param.arg = ctx;
    ucs_status_t status = ucp_worker_set_am_recv_handler(worker, &term_param);
    if (status != UCS_OK)
    {
        return -1;
    }

    DBG("AM recv handlers successfully registered");

    return 0;
}

#endif // _DPU_OFFLOAD_COMM_CHANNELS_H
