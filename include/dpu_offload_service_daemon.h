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

typedef enum
{
    OOB,
    UCX_LISTENER
} conn_mode_t;

dpu_offload_status_t offload_engine_init(offloading_engine_t **engine);
void offload_engine_fini(offloading_engine_t **engine);

execution_context_t *server_init(offloading_engine_t *, init_params_t *);
void server_fini(execution_context_t **);

execution_context_t *client_init(offloading_engine_t *, init_params_t *);
void client_fini(execution_context_t **);

dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *, dpu_config_t *);

/**
 * @brief send_cache sends the content of the local endpoint cache to a specific remote endpoint.
 * This is a non-blocking operation.
 *
 * @param econtext Current execution context
 * @param cache Endpoint cache to be sent
 * @param dest_ep Endpoint destination
 * @param meta_event Event used to track completion.
 * @return dpu_offload_status_t DO_SUCCESS for success; DO_ERROR if any error occurs
 */
dpu_offload_status_t send_cache(execution_context_t *econtext, cache_t *cache, ucp_ep_h dest_ep, dpu_offload_event_t *meta_event);

/**
 * @brief exchange_cache exchanges the context of the local endpoint cache.
 * It is meant to be called on DPUs to synchronize their endpoint cache. This is
 * a non-blocking collective operation.
 *
 * @param[in] econtext Current execution context
 * @param[in] cache Endpoint cache to be sent
 * @param[out] meta_event Event used to track completion.
 * @return dpu_offload_status_t DO_SUCCESS for success; DO_ERROR if any error occurs
 */
dpu_offload_status_t exchange_cache(execution_context_t *econtext, cache_t *cache, dpu_offload_event_t *meta_event);

#endif // DPU_OFFLOAD_SERVICE_DAEMON_H_