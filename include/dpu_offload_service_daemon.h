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
#include "dpu_offload_group_cache.h"

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

/**
 * @brief client_init creates and returns a new execution context object that can later on be used as a client. Trying to use it as a server is invalid.
 *
 * @param[in] engine Pointer to the engine in the context of which we need to create the client execution context.
 * @param[in] init_params Initialization parameters to use during the creation of the client execution context; can be NULL
 *
 * @return execution_context_t* Pointer to the new execution context or NULL in case of error.
 */
execution_context_t *client_init(offloading_engine_t *engine, init_params_t *init_params);

/**
 * @brief client_fini finalizes an execution context that is being used as a client.
 * Practically, a termination message is sent to the associated server (if any) vi a notification and the function will block until
 * the notification completes. Then the object is entirely freed. In other words, upon returning from the function, the execution context
 * is guaranteed to have been terminated, completely freed and the potential associated server notified of the termination.
 *
 * @param[in,out] ctx Pointer of pointer to the execution to finalize. Upon success the execution context is set to NULL so it cannot be used any longer.
 *
 */
void client_fini(execution_context_t **ctx);

void offload_config_free(offloading_config_t *cfg);

/**
 * @brief inter_dpus_connect_mgr initiates the connections between all the service processes after the configuration
 * has been setup.
 * The function is non-blocking.
 *
 * @param[in] engine Offloading engine in the context of which connections need to happen
 * @param[in] config Configuration of the service.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *engine, offloading_config_t *config);

/**
 * @brief send_add_group_rank_request initiates the creation of a new group from the host to a service process, after the creation of the first group during bootstrapping.
 * The function is non-blocking. The event payload must contain a rank_info_t structure that describe the group to be added.
 * The function also adds the group information to the local group cache.
 * This function checks the status of the group cache to ensure it won't be corrupted
 *
 * @param[in] econtext Execution context to use to send the message to the service process
 * @param[in] ep Endpoint of the target service process
 * @param[in] dest_id Identifier of the target service process (i.e., the server identifier)
 * @param[in] e Event to use to track completion.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t send_add_group_rank_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, dpu_offload_event_t *e);

/**
 * @brief do_send_add_group_rank_request sends a add group message, WITHOUT ensuring the cache won't be corrupted.
 * Mainly for internal use. The event payload must contain a rank_info_t structure that describe the group to be added.
 *
 * @param econtext Execution context to use to send the message to the service process
 * @param ep Endpoint of the target service process
 * @param dest_id Identifier of the target service process (i.e., the server identifier)
 * @param ev Event to use to track completion.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t do_send_add_group_rank_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, dpu_offload_event_t *ev);

/**
 * @brief send_revoke_group_rank_request_through_rank_info initiates the destruction/revokation of an existing group,
 * using a rank_info structure. This is for instance used by the ranks on the host to notify the associated service process
 * that a group is now being revoked.
 * The rank info object must contain data about a single rank that is revoking the group.
 * The function is non-blocking.
 * The function implicitly manages an event to send the notification to ensure the caller does not need to handle
 * completion and ensure that once the function exists, it does not rely on the data from the caller.
 *
 * @param[in] econtext Execution context to use to send the message to the service process
 * @param[in] ep Endpoint of the target service process
 * @param[in] dest_id Identifier of the target service process (i.e., the server identifier)
 * @param[in] rank_info Information about the rank/group to revoke
 * @param[in] meta_ev Optional meta-event to use to track completion of multiple sends (can be NULL)
 * @return dpu_offload_status_t
 */
dpu_offload_status_t send_revoke_group_rank_request_through_rank_info(execution_context_t *econtext,
                                                                      ucp_ep_h ep,
                                                                      uint64_t dest_id,
                                                                      rank_info_t *rank_info,
                                                                      dpu_offload_event_t *meta_ev);

/**
 * @brief send_revoke_group_rank_request_through_num_ranks initiates the destruction/revokation of an existing group,
 * using a group id structure and the number of ranks that have revoked the group.
 * This is for instance used between service processes to notify that a group is now being revoked and how many rank revoked it.
 * The function is non-blocking.
 * The function implicitly manages an event to send the notification to ensure the caller does not need to handle
 * completion and ensure that once the function exists, it does not rely on the data from the caller.
 *
 * @param econtext Execution context to use to send the message to the service process
 * @param ep Endpoint of the target service process
 * @param dest_id Identifier of the target service process (i.e., the server identifier)
 * @param gp_uid UID of the group to revoke
 * @param num_ranks Number of ranks that have revoked the group
 * @param meta_ev Optional meta-event to use to track completion of multiple sends (can be NULL)
 * @return dpu_offload_status_t
 */
dpu_offload_status_t send_revoke_group_rank_request_through_num_ranks(execution_context_t *econtext,
                                                                      ucp_ep_h ep,
                                                                      uint64_t dest_id,
                                                                      group_uid_t gp_uid,
                                                                      uint64_t num_ranks,
                                                                      dpu_offload_event_t *meta_ev);

/**
 * @brief callback that servers (service processes acting as servers) on DPUs can
 * set (server->connected_cb) to have implicit management of caches, especially
 * when all the ranks of the group are on the local host. In such a situation,
 * it will be detected when the last ranks connects, the group cache therefore
 * completes and the cache is then sent back to local ranks.
 */
void local_rank_connect_default_callback(void *data);

/**
 * @brief Get the local service process identifier from its global identifier. The local
 * identifier is the one setup at initialization time, not a group-level identifier.
 * 
 * @param[in] engine Associated offload engine to use for the query
 * @param[in] global_sp_id Service process's global identifier
 * @param[in,out] local_sp_id Associated local identifier; UINT64_MAX when lookup fails
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_local_sp_id(offloading_engine_t *engine, uint64_t global_sp_id, uint64_t *local_sp_id);

/**
 * @brief Send group cache to a specific destination, mainly used to send the cache back to the local ranks.
 *
 * @param econtext Execution context to use to send the message to the service process
 * @param dest Destination's endpoint
 * @param gp_uid UID of the group for which the cache needs to be sent out
 * @param metaev Meta-event to use to track completion of multiple sends
 * @return dpu_offload_status_t
 */
dpu_offload_status_t send_group_cache(execution_context_t *econtext, ucp_ep_h dest, uint64_t dest_id, group_uid_t gp_uid, dpu_offload_event_t *metaev);

dpu_offload_status_t send_gp_cache_to_host(execution_context_t *econtext, group_uid_t group_uid);

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
dpu_offload_status_t send_cache(execution_context_t *econtext, cache_t *cache, ucp_ep_h dest_ep, uint64_t dest_id, dpu_offload_event_t *meta_event);

/**
 * @brief The function sends the cache entries for the local ranks of a group to all other DPUs
 * but only if all DPUs are connected, otherwise the broadcast would be incomplete.
 * Remember that all DPU daemons and ranks are initiating connections independently
 * and in parallel, there is not way to predict whether the local ranks will be fully
 * connected before all the DPUs, or the opposite.
 * In other words, when calling the function, it is possible that it would not be
 * possible to perform the broadcast right away. The function checks whether the
 * broadcast can be performed before trying to send the cache. In other words, the
 * broadcast is not initiated if all the DPUs are not locally connected.
 *
 * @param engine Current offloading engine
 * @param group_uid UID of the group to broadcast.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t broadcast_group_cache(offloading_engine_t *engine, group_uid_t group_uid);

/**
 * @brief broadcast_group_cache_revoke broadcasts the notification that a group has been locally revoked, meaning that
 * all the ranks attached to the SP revoked the said group.
 *
 * @param engine Current offloading engine
 * @param group_uid UID of the group that has been revoked
 * @param n_ranks Number of ranks involved in the revoke
 * @return dpu_offload_status_t
 */
dpu_offload_status_t broadcast_group_cache_revoke(offloading_engine_t *engine, group_uid_t group_uid, uint64_t n_ranks);

/**
 * @brief Get the service process endpoint by ID object, i.e., the identifier returned by get_sp_id_by_host_rank
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] id Global service process identifier
 * @param[out] ucp_ep_h DPU's endpoint to use to communicate with the target DPU
 * @param[out] econtext_comm The execution context to use for notification, must be used to get an event
 * @param[out] notif_dest_id The local identifier to send notification to the remote DPU
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_sp_ep_by_id(offloading_engine_t *engine, uint64_t sp_id, ucp_ep_h *sp_ep, execution_context_t **econtext_comm, uint64_t *comm_id);

/**
 * @brief Get the group rank host identifier from the local cache
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank Target rank in the group
 * @param[out] host_id ID of the host where the rank is running (64-bit hash)
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_group_rank_host(offloading_engine_t *engine,
                                         group_uid_t gp_uid,
                                         int64_t rank,
                                         uint64_t *host_id);

/**
 * @brief Checks whether two ranks of a same group are on the same host.
 * 
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank1 First rank to use for the comparison
 * @param[in] rank2 Second rank to use for the comparison
 * @return false if the two ranks are not on the same host or in the context of an error
 * @return true if the two ranks are on the same host
 */
bool on_same_host(offloading_engine_t *engine,
                  group_uid_t gp_uid,
                  int64_t rank1,
                  int64_t rank2);

/**
 * @brief Get the group ranks on a host from the local cache
 * 
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] host_id Target host ID
 * @param[in,out] n_ranks Pointer to the variable that will hold the number of ranks on the target host
 * @param[in,out] ranks Pointer to the dynamic array of int64_t that will store all the ranks
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_group_ranks_on_host(offloading_engine_t *engine,
                                             group_uid_t gp_uid,
                                             uint64_t host_id,
                                             size_t *n_ranks,
                                             dyn_array_t *ranks);

/**
 * @brief Get the list of all local SPs for a given rank in a group.
 * Note that the function returns all the local SPs, including the SPs
 * the rank may not be directly connected to.
 * 
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank Target rank in the group
 * @param[in,out] n_sps Pointer to the variable that will hold the number of local SPs
 * @param[in,out] sps Pointer to the dunamic array of uint64_t that will store all the service processes global ID
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_group_rank_sps(offloading_engine_t *engine,
                                        group_uid_t gp_uid,
                                        uint64_t rank,
                                        size_t *n_sps,
                                        dyn_array_t *sps);

/**
 * @brief Get the local SPs for a given group. Can be used only is the context of SPs.
 * This can be used to know how many and which local SPs are involved in a given group.
 * 
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in,out] n_sps Pointer to the variable that will hold the number of local SPs
 * @param[in,out] sps Pointer to the dunamic array of uint64_t that will store all the service processes global ID
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_group_local_sps(offloading_engine_t *engine,
                                         group_uid_t gp_uid,
                                         size_t *n_sps,
                                         dyn_array_t *sps);

execution_context_t *get_server_servicing_host(offloading_engine_t *engine);

dpu_offload_status_t get_local_service_proc_connect_info(offloading_config_t *cfg, rank_info_t *rank_info, init_params_t *init_params);

dpu_offload_status_t get_num_connecting_ranks(uint64_t num_service_procs_per_dpu, int64_t n_local_ranks, uint64_t sp_lid, uint64_t *n_ranks);

bool all_service_procs_connected(offloading_engine_t *engine);

dpu_offload_status_t forward_cache_entry_to_local_sps(offloading_engine_t *engine, group_id_t *gp_id);

#define SET_DEFAULT_DPU_HOST_SERVER_CALLBACKS(_init_params)                 \
    do                                                                      \
    {                                                                       \
        (_init_params)->connected_cb = local_rank_connect_default_callback; \
    } while (0)

#endif // DPU_OFFLOAD_SERVICE_DAEMON_H_
