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

/**
 * @brief Progress the entrie library, i.e., all the engines and execution contexts, based on an execution context.
 *
 * @param[in] econtext Execution context from which to start progressing the entire library.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t lib_progress(execution_context_t *econtext);

execution_context_t *server_init(offloading_engine_t *, init_params_t *);
void server_fini(execution_context_t **);

execution_context_t *client_init(offloading_engine_t *, init_params_t *);
void client_fini(execution_context_t **);

void offload_config_free(offloading_config_t *cfg);

dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *, offloading_config_t *);

dpu_offload_status_t send_add_group_rank_request(execution_context_t *econtext, ucp_ep_h ep, int64_t group_id, int64_t rank, int64_t group_size, dpu_offload_event_t **e);

void local_rank_connect_default_callback(void *data);

/**
 * @brief Send group cache to a specific destination, mainly used to send the cache back to the local ranks.
 *
 * @param econtext
 * @param dest
 * @param gp_id
 * @param metaev
 * @return dpu_offload_status_t
 */
dpu_offload_status_t send_group_cache(execution_context_t *econtext, ucp_ep_h dest, uint64_t dest_id, int64_t gp_id, dpu_offload_event_t *metaev);

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
 * @brief The function exchange a group cache between DPUs but only if all DPUs are
 * connected, otherwise the broadcast would be incomplete.
 *
 * @param engine Current offloading engine
 * @param group_id ID of the group to broadcast.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t broadcast_group_cache(offloading_engine_t *engine, int64_t group_id);

/**
 * @brief Get the dpu ID by host rank object. That ID can then be used to look up the corresponding endpoint.
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_id Target group identifier
 * @param[in] rank Target rank in the group
 * @param[in] dpu_idx In case of multiple DPUs per host, index of the target shadow DPU for the group/rank
 * @param[out] dpu_id Resulting DPU identifier
 * @param[out] ev Associated event. If NULL, the DPU identifier is available right away. If not, it is required to call the function again once the event has completed. The caller is in charge of returning the event after completion. The event cannot be added to any list since it is already put on a list.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_dpu_id_by_group_rank(offloading_engine_t *engine, int64_t gp_id, int64_t rank, int64_t dpu_idx, int64_t *dpu_id, dpu_offload_event_t **ev);

/**
 * @brief Get the dpu ID by host rank object. That ID can then be used to look up the corresponding endpoint.
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_id Target group identifier
 * @param[in] rank Target rank in the group
 * @param[in] dpu_idx In case of multiple DPUs per host, index of the target shadow DPU for the group/rank
 * @param[out] cb Associated callback. If the event completes right away, the callback is still being invoked. The user is in charge of returning the object related to the request.
 * @return dpu_offload_status_t
 *
 * Example:
 *      - To issue the request for the first DPU attached to the group/rank `gp_id` and `rank_id`
 *          get_cache_entry_by_group_rank(offload_engine, gp_id, rank_id, 0, my_completion_cb);
 *      - Completion callback example:
 *          void my_completion_cb(void *data)
 *          {
 *              assert(data);
 *              cache_entry_request_t *cache_entry_req = (cache_entry_request_t*)data;
 *              assert(cache_entry_req->offload_engine);
 *              offloading_engine_t *engine = (offloading_engine_t*)cache_entry_req->offload_engine;
 *              ucp_ep_h target_dpu_ep = get_dpu_ep_by_id(engine, cache_entry_req->target_dpu_idx);
 *              assert(target_dpu_ep == NULL);
 *              DYN_LIST_RETURN(engine->free_cache_entry_requests, cache_entry_req, item);
 *          }
 */
dpu_offload_status_t get_cache_entry_by_group_rank(offloading_engine_t *engine, int64_t gp_id, int64_t rank, int64_t dpu_idx, request_compl_cb_t cb);

/**
 * @brief Get the DPU endpoint by ID object, i.e., the identifier returned by get_dpu_id_by_host_rank
 *
 * @param[in] engine engine Offloading engine for the query
 * @param[in] id DPU identifier
 * @param[out] ucp_ep_h DPU's endpoint to use to communicate with the target DPU
 * @param[out] econtext_comm The execution context to use for notification, must be used to get an event
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_dpu_ep_by_id(offloading_engine_t *engine, uint64_t id, ucp_ep_h *ep, execution_context_t **econtext_comm);

bool group_cache_populated(offloading_engine_t *engine, int64_t gp_id);

bool is_in_cache(cache_t *cache, int64_t gp_id, int64_t rank_id, int64_t group_size);

execution_context_t *get_server_servicing_host(offloading_engine_t *engine);

#define SET_DEFAULT_DPU_HOST_SERVER_CALLBACKS(_init_params)                      \
    do                                                                      \
    {                                                                       \
        (_init_params)->connected_cb = local_rank_connect_default_callback; \
    } while (0)

#endif // DPU_OFFLOAD_SERVICE_DAEMON_H_