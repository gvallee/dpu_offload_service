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

/**
 * @brief offload_engine_progress progresses the entire offloading engine, i.e., all the
 * associated execution context. It is for instance used to progress all communications
 * between DPUs and ensure completions of ongoing communications.
 * 
 * @param[in] engine The offloading engine to progress
 * @return dpu_offload_status_t
 */
dpu_offload_status_t offload_engine_progress(offloading_engine_t *engine);

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

/**
 * @brief Get the dpu ID by host rank object. That ID can then be used to look up the corresponding endpoint.
 * 
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_id Target group identifier
 * @param[in] rank Target rank in the group
 * @param[in] dpu_idx In case of multiple DPUs per host, index of the target shadow DPU for the group/rank
 * @param[out] dpu_id Resulting DPU identifier
 * @param[out] ev Associated event. If NULL, the DPU identifier is available right away. If not, it is required to call the function again once the event has completed. The caller is in charge of returning the event after completion.
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_dpu_id_by_group_rank(offloading_engine_t *engine, int64_t gp_id, int64_t rank, int64_t dpu_idx, int64_t *dpu_id, dpu_offload_event_t **ev);

/**
 * @brief Get the DPU endpoint by ID object, i.e., the identifier returned by get_dpu_id_by_host_rank
 * 
 * @param engine Offloading engine for the query
 * @param id DPU identifier
 * @return ucp_ep_h DPU's endpoint
 */
ucp_ep_h get_dpu_ep_by_id(offloading_engine_t *engine, uint64_t id);

#endif // DPU_OFFLOAD_SERVICE_DAEMON_H_