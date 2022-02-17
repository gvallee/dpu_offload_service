//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef DPU_OFFLOAD_SERVICE_DAEMON_H_
#define DPU_OFFLOAD_SERVICE_DAEMON_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdbool.h>

#include "dpu_offload_types.h"
#include "dpu_offload_event_channels.h"

#define DAEMON_GET_PEER_EP(_d, _ep) ({ \
    if (_d->type == DAEMON_CLIENT)     \
    {                                  \
        _ep = _d->client->server_ep;   \
    }                                  \
    else                               \
    {                                  \
        _ep = _d->server->client_ep;   \
    }                                  \
})

struct ucx_context
{
    int completed;
};

typedef enum
{
    FAILURE_MODE_NONE,
    FAILURE_MODE_SEND,     /* fail send operation on server */
    FAILURE_MODE_RECV,     /* fail receive operation on client */
    FAILURE_MODE_KEEPALIVE /* fail without communication on client */
} failure_mode_t;

static struct err_handling
{
    ucp_err_handling_mode_t ucp_err_mode;
    failure_mode_t failure_mode;
} err_handling_opt;

enum
{
    OOB,
    UCX_LISTENER
} conn_mode_t;

dpu_offload_status_t offload_engine_init(offloading_engine_t **engine);
void offload_engine_fini(offloading_engine_t **engine);

execution_context_t* server_init(offloading_engine_t *, init_params_t *);
void server_fini(execution_context_t **);

execution_context_t* client_init(offloading_engine_t *, init_params_t *);
void client_fini(execution_context_t **);

dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *, char *, char *);

#endif // DPU_OFFLOAD_SERVICE_DAEMON_H_