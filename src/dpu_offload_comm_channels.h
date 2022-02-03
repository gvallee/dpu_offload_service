//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "dpu_offload_service_daemon.h"

static ucs_status_t am_term_msg_cb(void *arg, const void *header, size_t header_length,
                                   void *data, size_t length,
                                   const ucp_am_recv_param_t *param)
{
    execution_context_t *d = (execution_context_t *)arg;
    fprintf(stderr, "TERM msg received (handle=%p)\n", d);
    if (d == NULL)
    {
        fprintf(stderr, "am_term_msg_cb() - handle is NULL\n");
        return -1;
    }
    switch (d->type)
    {
    case CONTEXT_CLIENT:
        d->client->done = true;
        break;
    case CONTEXT_SERVER:
        d->server->done = true;
        break;
    default:
        fprintf(stderr, "invalid type\n");
        return UCS_ERR_NO_MESSAGE;
    }
    return UCS_OK;
}

static int dpu_offload_set_am_recv_handlers(execution_context_t *ctx)
{
    ucp_worker_h worker = GET_WORKER(ctx);
    if (worker == NULL)
    {
        fprintf(stderr, "undefined worker\n");
        return -1;
    }

    if (ctx == NULL)
    {
        fprintf(stderr, "undefined context\n");
        return -1;
    }
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

    fprintf(stderr, "AM recv handlers successfully registered\n");

    return 0;
}