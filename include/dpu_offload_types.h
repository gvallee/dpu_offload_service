//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdbool.h>
#include <pthread.h>

#ifndef DPU_OFFLOAD_TYPES_H
#define DPU_OFFLOAD_TYPES_H

#include <ucs/datastruct/list.h>

#include "dynamic_structs.h"
#include "dpu_offload_common.h"
#include "dpu_offload_utils.h"

_EXTERN_C_BEGIN

#define DEFAULT_INTER_DPU_CONNECT_PORT (11111)
#define DEFAULT_NUM_PEERS (10000)

typedef enum
{
    CONTEXT_CLIENT = 0,
    CONTEXT_SERVER
} daemon_type_t;

#define ECONTEXT_ID(_exec_ctx) ({          \
    uint64_t _my_id;                       \
    if (_exec_ctx->type == CONTEXT_CLIENT) \
    {                                      \
        _my_id = _exec_ctx->client->id;    \
    }                                      \
    else                                   \
    {                                      \
        _my_id = 0;                        \
    }                                      \
    _my_id;                                \
})

#define GET_SERVER_EP(_exec_ctx) ({         \
    ucp_ep_h _ep;                           \
    if (_exec_ctx->type == CONTEXT_CLIENT)  \
    {                                       \
        _ep = _exec_ctx->client->server_ep; \
    }                                       \
    else                                    \
    {                                       \
        _ep = NULL;                         \
    }                                       \
    _ep;                                    \
})

#define GET_CLIENT_EP(_exec_ctx, _client_id) ({                            \
    ucp_ep_h _ep;                                                          \
    if (_exec_ctx->type == CONTEXT_SERVER)                                 \
    {                                                                      \
        _ep = _exec_ctx->server->connected_clients.clients[_client_id].ep; \
    }                                                                      \
    else                                                                   \
    {                                                                      \
        _ep = NULL;                                                        \
    }                                                                      \
    _ep;                                                                   \
})

#define GET_WORKER(_exec_ctx) ({              \
    ucp_worker_h _w;                          \
    if ((_exec_ctx)->type == CONTEXT_CLIENT)  \
    {                                         \
        _w = (_exec_ctx)->client->ucp_worker; \
    }                                         \
    else                                      \
    {                                         \
        _w = (_exec_ctx)->server->ucp_worker; \
    }                                         \
    _w;                                       \
})

#define EV_SYS(_exec_ctx) ({                      \
    dpu_offload_ev_sys_t *_sys;                   \
    if (_exec_ctx->type == CONTEXT_CLIENT)        \
    {                                             \
        _sys = _exec_ctx->client->event_channels; \
    }                                             \
    else                                          \
    {                                             \
        _sys = _exec_ctx->server->event_channels; \
    }                                             \
    _sys;                                         \
})

#define ACTIVE_OPS(_exec_ctx) ({                  \
    ucs_list_link_t *_list;                       \
    if (_exec_ctx->type == CONTEXT_CLIENT)        \
    {                                             \
        _list = &(_exec_ctx->client->active_ops); \
    }                                             \
    else                                          \
    {                                             \
        _list = &(_exec_ctx->server->active_ops); \
    }                                             \
    _list;                                        \
})

#define EXECUTION_CONTEXT_DONE(_exec_ctx) ({ \
    bool _done;                              \
    if (_exec_ctx->type == CONTEXT_CLIENT)   \
        _done = _exec_ctx->client->done;     \
    else                                     \
        _done = _exec_ctx->server->done;     \
    _done;                                   \
})

typedef enum dpu_offload_state
{
    DPU_OFFLOAD_STATE_UNKNOWN = 0,
    DPU_OFFLOAD_STATE_INITIALIZED,
    DPU_OFFLOAD_STATE_FINALIZED,
} dpu_offload_state_t;

typedef struct pmix_infrastructure
{
    int dvm_argc;
    char **dvm_argv;
    int run_argc;
    char **run_argv;
    pid_t dvm_pid;
    pid_t service_pid;
    bool dvm_started;
    bool service_started;
} pmix_infrastructure_t;

typedef struct offload_config
{
    dpu_offload_state_t state;
    char *offload_config_file_path;
    char *associated_bluefield;
    bool with_pmix;

    union
    {
        pmix_infrastructure_t pmix;
    } infra;
} offload_config_t;

/* OPERATIONS */

typedef int (*op_init_fn)();
typedef int (*op_complete_fn)();
typedef int (*op_progress_fn)();
typedef int (*op_fini_fn)();

typedef struct offload_op
{
    // alg_id identifies the algorithm being implemented, e.g. alltoallv
    uint64_t alg_id;

    op_init_fn op_init;
    op_complete_fn op_complete;
    op_progress_fn op_progress;
    op_fini_fn op_fini;

    // alg_data is a pointer that can be used by developers to store data
    // that can be used for the execution of all operations that is specific
    // to the implementation of the algorithm.
    void *alg_data;
} offload_op_t;

typedef struct op_desc
{
    ucs_list_link_t item;
    uint64_t id;
    offload_op_t *op_definition;

    // op_data can be used by developers to associate any run-time data to the execution of the operation.
    void *op_data;
    bool completed;
} op_desc_t;

#if 0
typedef struct active_ops
{
    size_t num_active_ops;
    op_desc_t *ops;
} active_ops_t;
#endif

/* OFFLOADING ENGINE, CLIENTS/SERVERS */

#define INVALID_GROUP (-1)
#define INVALID_RANK (-1)

#define IS_A_VALID_PEER_DATA(_peer_data) ({                                                                      \
    bool _valid = false;                                                                                         \
    if ((_peer_data)->proc_info.group_id != INVALID_GROUP && (_peer_data)->proc_info.group_rank != INVALID_RANK) \
        _valid = true;                                                                                           \
    _valid;                                                                                                      \
})

typedef struct ucx_server_ctx
{
    volatile ucp_conn_request_h conn_request;
    ucp_listener_h listener;
} ucx_server_ctx_t;

typedef struct rank_info
{
    int64_t group_id;
    int64_t group_rank;
} rank_info_t;

// fixme: long term, we do not want to have a limit on the length of the address
// but this will require a new smart way to manage the memory used by cache entries
// and avoid expensive copies when exchanging cache entries between DPUs and
// application processes
#define MAX_ADDR_LEN (2048)

#define MAX_SHADOW_DPUS (8)

// peer_data_t stores all the information related to a rank in a group,
// it is designed in a way it can be directly sent without requiring
// memory copies.
typedef struct peer_data
{
    ucs_list_link_t item;
    rank_info_t proc_info;
    size_t addr_len;
    char addr[MAX_ADDR_LEN]; // ultimately ucp_address_t * when using UCX
} peer_data_t;

typedef struct shadow_dpu_info
{
    peer_data_t shadow_data; // Can be NULL if applied to DPU specific data
    ucp_ep_h shadow_ep;      // endpoint to reach the attached DPU
} shadow_dpu_info_t;

typedef struct peer_cache_entry
{
    ucs_list_link_t item;
    bool set;
    peer_data_t peer;
    ucp_ep_h ep;
    size_t num_shadow_dpus;
    shadow_dpu_info_t shadow_dpus[MAX_SHADOW_DPUS]; // Array of DPUs (when applicable)
} peer_cache_entry_t;

typedef struct peer_info
{
    ucp_ep_h ep;
    ucs_status_t ep_status;
    // rank_info_t rank;
    //  Array of group/proc entries, one per group. A rank can belong to multiple groups but have a single endpoint.
    peer_cache_entry_t **cache_entries;
} peer_info_t;

/**********************************************/
/* PUBLIC STRUCTURES RELATED TO NOTIFICATIONS */
/**********************************************/

/**
 * @brief am_header_t is the structure used to represent the header sent with UCX active messages
 */
typedef struct am_header
{
    // Unique identifier assigned at bootstrapping.
    // For clients, id assigned by server during connection,
    // used for triage when server has multiple clients
    uint64_t id;

    // Type associated to the payload, e.g., notification type.
    // Used to identify the callback to invoke upon reception of a notification.
    uint64_t type;
} am_header_t;

/**
 * @brief am_req_t is the structure used to track completion of a notification.
 */
typedef struct am_req
{
    // Header associated to the notification.
    am_header_t hdr;

    // Is the operation completed or not.
    // An example of a notification that does not complete right away
    // is a notification requiring the exchange of a RDV message under
    // the cover.
    int complete;
} am_req_t;

/**
 * @brief dpu_offload_ev_sys_t is the structure representing the event system used to implement notifications.
 */
typedef struct dpu_offload_ev_sys
{
    // Pool of available events from which objects are taken when invoking event_get().
    // Once the object obtained, one can populate the event-specific data and emit the event.
    // From a communication point-of-view, these objects are therefore used on the send side.
    dyn_list_t *free_evs;

    // Current number of event objects from the pool that are being used.
    // Note that it means these objects are not in the pool and must be returned at some points.
    size_t num_used_evs;

    /* pending notifications are notifications that cannot be delivered upon reception because the callback is not registered yet */
    ucs_list_link_t pending_notifications;

    // free_pending_notifications is a pool oof pending notification objects that can be used when a notification is received and
    // no callback is registered yet. It avoids allocating memory.
    dyn_list_t *free_pending_notifications;

    // Array of callback functions, i.e., array of pointers, organized based on the notification type, a.k.a. notification ID
    dyn_array_t notification_callbacks;
} dpu_offload_ev_sys_t;

typedef struct connected_clients
{
    size_t num_max_connected_clients;
    size_t num_connected_clients;
    // Array of structures to track connected clients
    peer_info_t *clients;
} connected_clients_t;

typedef struct conn_params
{
    ucs_list_link_t item;
    char *addr_str;
    char *port_str;
    int port;
    struct sockaddr_storage saddr;
} conn_params_t;

typedef struct init_params
{
    conn_params_t *conn_params;
    rank_info_t *proc_info;
    ucp_worker_h worker;
} init_params_t;

#define ECONTEXT_LOCK(_econtext)                             \
    do                                                       \
    {                                                        \
        if (_econtext->type == CONTEXT_CLIENT)               \
            pthread_mutex_lock(&(_econtext->client->mutex)); \
        else                                                 \
            pthread_mutex_lock(&(_econtext->server->mutex)); \
    } while (0)

#define ECONTEXT_UNLOCK(_econtext)                             \
    do                                                         \
    {                                                          \
        if (_econtext->type == CONTEXT_CLIENT)                 \
            pthread_mutex_unlock(&(_econtext->client->mutex)); \
        else                                                   \
            pthread_mutex_unlock(&(_econtext->server->mutex)); \
    } while (0)

typedef struct dpu_offload_server_t
{
    int mode;
    bool done;
    conn_params_t conn_params;
    ucp_worker_h ucp_worker;
    ucp_context_h ucp_context;
    pthread_t connect_tid;
    pthread_mutex_t mutex;
    pthread_mutexattr_t mattr;
    connected_clients_t connected_clients;

    dpu_offload_ev_sys_t *event_channels;

    /* Active operations: a server can execute operations on behalf of all the clients that are connected */
    ucs_list_link_t active_ops;

    union
    {
        struct
        {
            ucx_server_ctx_t context;
        } ucx_listener;
        struct
        {
            ucp_address_t *local_addr;
            size_t local_addr_len;
            void *peer_addr;
            size_t peer_addr_len;
            int sock;
            int listenfd;
            int tag;
            char *addr_msg_str;
            ucp_tag_t tag_mask;
        } oob;
    } conn_data;
} dpu_offload_server_t;

typedef struct dpu_offload_client_t
{
    uint64_t id; // Identifier assigned by server
    int mode;
    conn_params_t conn_params;
    bool done;

    ucp_worker_h ucp_worker;
    ucp_context_h ucp_context;
    ucp_ep_h server_ep;
    ucs_status_t server_ep_status;
    pthread_mutex_t mutex;

    dpu_offload_ev_sys_t *event_channels;

    /* Active operations: a client can execute operations on behalf of the server it is connected to */
    ucs_list_link_t active_ops;

    union
    {
        struct
        {
            struct sockaddr_storage connect_addr;
        } ucx_listener;
        struct
        {
            ucp_address_t *local_addr;
            size_t local_addr_len;
            void *peer_addr;
            size_t peer_addr_len;
            int sock;
            char *addr_msg_str;
            int tag;
        } oob;
    } conn_data;
} dpu_offload_client_t;

struct execution_context;
typedef int (*execution_context_progress_fn)(struct execution_context *);

struct offloading_engine; // forward declaration
typedef struct execution_context
{
    int type;
    struct offloading_engine *engine;
    dpu_offload_ev_sys_t *event_channels;
    ucs_list_link_t ongoing_events;
    execution_context_progress_fn progress;
    rank_info_t rank;
    dyn_list_t *free_pending_rdv_recv;
    ucs_list_link_t pending_rdv_recvs;
    union
    {
        dpu_offload_client_t *client;
        dpu_offload_server_t *server;
    };
} execution_context_t;

typedef struct pending_am_rdv_recv
{
    ucs_list_link_t item;
    execution_context_t *econtext;
    size_t hdr_len;
    am_header_t *hdr;
    ucs_status_ptr_t req;
    size_t payload_size;
    size_t buff_size;
    void *desc;
    void *user_data;
} pending_am_rdv_recv_t;

/**
 * @brief dpu_offload_event_t represents an event, i.e., the implementation of a notification
 */
typedef struct dpu_offload_event
{
    // item is used to be able to add/remove the event to lists, e.g., the list for ongoing events and the pool of free event objects.
    ucs_list_link_t item;
    // sub_events is the list of sub-events composing this event. 
    // The event that has sub-events is not considered completed unless all sub-events are completed. 
    // event_completed() can be used to easily check for completion.
    ucs_list_link_t sub_events;
    // sub_events_initialized tracks whether the sub-event list has been initialized.
    bool sub_events_initialized;
    // ctx is the communication context associated to the event, used to track the status of the potential underlying UCX AM communication
    am_req_t ctx;
    // req is the opaque request object used to track any potential underlying communication associated to the event.
    // If more than one communication operation is required, please use sub-events.
    void *req;
    // context is the user defined context of the event. Can be NULL.
    void *context;
    // data is the payload associated to the event. Can be NULL.
    void *data;
    // user_context is the user-defined context for the event. Can be NULL.
    void *user_context;
} dpu_offload_event_t;

typedef enum
{
    EVENT_DONE = UCS_OK,
    EVENT_INPROGRESS = UCS_INPROGRESS
} event_state_t;

/* OFFLOADING ENGINE */

typedef struct cache
{
    size_t size; // not used at the moment

    /* data is a dynamic array for all the groups */
    dyn_array_t data;
} cache_t;

typedef struct group_cache
{
    bool initialized;
    dyn_array_t ranks;
} group_cache_t;

#define SET_PEER_CACHE_ENTRY_FROM_PEER_DATA(_peer_cache, _peer_data)                              \
    do                                                                                            \
    {                                                                                             \
        if (_peer_cache[_peer_data->group_id] == NULL)                                            \
        {                                                                                         \
            /* Cache for the group is empty */                                                    \
            dyn_array_t *_dyn_array;                                                              \
            DYN_ARRAY_ALLOC(_dyn_array, DEFAULT_NUM_RANKS_IN_GROUP, peer_cache_entry_t);          \
            DYN_ARRAY_SET_ELT(_peer_cache[_peer_data->group_id], peer_cache_entry_t, _dyn_array); \
        }                                                                                         \
        dyn_array_t *_ptr = (dyn_array_t *)_peer_cache[_peer_data->group_id];                     \
        cache_entry_t *_cache = (cache_entry_t *)_ptr->base;                                      \
        _cache[_peer_data->group_rank].peer.proc_info.group_id = _peer_data->group_id;            \
        _cache[_peer_data->group_rank].peer.proc_info.group_rank = _peer_data->group_rank;        \
        _cache[_peer_data->group_rank].set = true;                                                \
    } while (0)

#define SET_PEER_CACHE_ENTRY(_peer_cache, _entry)                                                 \
    do                                                                                            \
    {                                                                                             \
        int32_t _gp_id = (_entry)->peer.proc_info.group_id;                                       \
        if (_gp_id >= _peer_cache->data.num_elts)                                                 \
            DYN_ARRAY_GROW(&(_peer_cache->data), group_cache_t, _gp_id);                          \
        group_cache_t *_gp_cache = (group_cache_t *)_peer_cache->data.base;                       \
        dyn_array_t *_rank_cache = &(_gp_cache[_gp_id].ranks);                                    \
        if (_gp_cache[_gp_id].initialized == false)                                               \
        {                                                                                         \
            /* Cache for the group is empty */                                                    \
            DYN_ARRAY_ALLOC(_rank_cache, DEFAULT_NUM_PEERS, peer_cache_entry_t);                  \
            _gp_cache[_gp_id].initialized = true;                                                 \
            _peer_cache->size++;                                                                  \
        }                                                                                         \
        if ((_entry)->peer.proc_info.group_rank >= _rank_cache->num_elts)                         \
            DYN_ARRAY_GROW(_rank_cache, peer_cache_entry_t, (_entry)->peer.proc_info.group_rank); \
        assert((_entry)->peer.proc_info.group_rank < _rank_cache->num_elts);                      \
        peer_cache_entry_t *_ptr = (peer_cache_entry_t *)_rank_cache->base;                       \
        memcpy(&(_ptr[(_entry)->peer.proc_info.group_rank]), _entry, sizeof(peer_cache_entry_t)); \
    } while (0)

typedef struct offloading_engine
{
    int done;

    /* client here is used to track the bootstrapping as a client. */
    /* it can only be at most one (the offload engine bootstraps only once */
    /* for both host process and the DPU daemon) */
    execution_context_t *client;

    /* we can have as many servers as we want, each server having multiple clients */
    size_t num_max_servers;
    size_t num_servers;
    execution_context_t **servers;

    /* we track the clients used for inter-DPU connection separately. Servers are at the */
    /* moment in the servers list. */
    size_t num_inter_dpus_clients;
    size_t num_max_inter_dpus_clients;
    execution_context_t **inter_dpus_clients;

    /* Vector of registered operation, ready for execution */
    size_t num_registered_ops;
    offload_op_t *registered_ops;
    dyn_list_t *free_op_descs;

    /* Cache for groups/rank so we can propagate rank and DPU related data */
    cache_t procs_cache;
    dyn_list_t *free_peer_cache_entries; // pool of peer descriptors that can also be used directly into a cache
    dyn_list_t *free_peer_descs;         // pool of peer data descriptirs

    /* Objects used during wire-up */
    dyn_list_t *pool_conn_params;
} offloading_engine_t;

/***************************/
/* NOTIFICATIONS INTERNALS */
/***************************/

struct dpu_offload_ev_sys;
// notification_cb is the signature of all notification callbacks that are invoked when receiving a notification via the event system
typedef int (*notification_cb)(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_size);

/**
 * @brief notification_callback_entry_t is the structure representing a callback. 
 * The event system has a vector of such structures. The type associated to the callback is 
 * its index in the vector used to track all callbacks (one and only one callback per type)
 */
typedef struct notification_callback_entry
{
    // Specify whether the callback has been set or not.
    bool set;
    // Pointer to the associated event system.
    struct dpu_offload_ev_sys *ev_sys;
    // Actually callback function
    notification_cb cb;
} notification_callback_entry_t;

/**
 * @brief pending_notification_t is the structure used to capture the data related to a event that
 * has been received but cannot yet be delivered because a callback has not been registered yet.
 */
typedef struct pending_notification
{
    // Element used to be able to add/remove a pending notification to a list
    ucs_list_link_t item;
    // Event type
    uint64_t type;
    // src_id is the unique identifier of the sender of the notification
    uint64_t src_id;
    // header is the AM header associated to the AM message when the notification arrived
    void *header;
    // header_size is the length of the header
    size_t header_size;
    // AM msg and event payload
    void *data;
    // Size of the payload
    size_t data_size;
    // Associated execution context
    execution_context_t *econtext;
} pending_notification_t;

/*********************/
/* DPU CONFIGURATION */
/*********************/

typedef enum
{
    CONNECT_STATUS_UNKNOWN = 0,
    CONNECT_STATUS_CONNECTED,
    CONNECT_STATUS_DISCONNECTED
} connect_status_t;

typedef struct remote_dpu_info
{
    ucs_list_link_t item;
    char *hostname;
    init_params_t init_params;
    connect_status_t conn_status;
    pthread_t connection_tid;
    offloading_engine_t *offload_engine;
} remote_dpu_info_t;

typedef struct dpu_config_data
{
    union
    {
        struct
        {
            char *hostname;
            char *addr;
            int rank_port;
            int interdpu_port;
        } version_1;
    };
} dpu_config_data_t;

typedef struct dpu_inter_connect_info
{
    dyn_list_t *pool_remote_dpu_info;
    ucs_list_link_t connect_to;
    size_t num_connect_to;
} dpu_inter_connect_info_t;

typedef struct dpu_config
{
    char *list_dpus;
    char *config_file;
    int format_version;
    offloading_engine_t *offloading_engine;
    bool dpu_found;
    size_t num_connecting_dpus;
    dpu_inter_connect_info_t info_connecting_to;
    size_t num_dpus;
    dyn_array_t dpus_config;
    struct
    {
        dpu_config_data_t *config;
        char hostname[1024];
        conn_params_t interdpu_conn_params;
        conn_params_t host_conn_params;
        init_params_t interdpu_init_params; // Parameters used to connect to other DPUs, or other DPUS connect to the DPU
        init_params_t host_init_params;     // Parameters used to let the host connect to the DPU
    } local_dpu;
} dpu_config_t;

dpu_offload_status_t get_dpu_config(dpu_config_t *);
dpu_offload_status_t get_host_config(dpu_config_t *);
dpu_offload_status_t find_dpu_config_from_platform_configfile(char *, dpu_config_t *);
dpu_offload_status_t find_config_from_platform_configfile(char *, char *, dpu_config_t *);

#define INIT_DPU_CONFIG_DATA(_data)                                                                       \
    do                                                                                                    \
    {                                                                                                     \
        (_data)->offloading_engine = NULL;                                                                \
        (_data)->num_dpus = 0;                                                                            \
        (_data)->dpu_found = false;                                                                       \
        (_data)->info_connecting_to.num_connect_to = 0;                                                   \
        (_data)->num_connecting_dpus = 0;                                                                 \
        (_data)->local_dpu.config = NULL;                                                                 \
        (_data)->local_dpu.hostname[0] = '\0';                                                            \
        (_data)->local_dpu.interdpu_conn_params.port = -1;                                                \
        (_data)->local_dpu.interdpu_conn_params.port_str = NULL;                                          \
        (_data)->local_dpu.interdpu_conn_params.addr_str = NULL;                                          \
        (_data)->local_dpu.host_conn_params.port = -1;                                                    \
        (_data)->local_dpu.host_conn_params.port_str = NULL;                                              \
        (_data)->local_dpu.host_conn_params.addr_str = NULL;                                              \
        dyn_array_t *array = &((_data)->dpus_config);                                                     \
        DYN_ARRAY_ALLOC(array, 32, dpu_config_data_t);                                                    \
        (_data)->local_dpu.interdpu_init_params.conn_params = &((_data)->local_dpu.interdpu_conn_params); \
        (_data)->local_dpu.host_init_params.conn_params = &((_data)->local_dpu.host_conn_params);         \
        ucs_list_link_t *_list = &((_data)->info_connecting_to.connect_to);                               \
        ucs_list_head_init(_list);                                                                        \
        DYN_LIST_ALLOC((_data)->info_connecting_to.pool_remote_dpu_info, 32, remote_dpu_info_t, item);    \
    } while (0)

#define SET_DPU_TO_CONNECT_TO(_cfg, _dpu_hostname)                                                                  \
    do                                                                                                              \
    {                                                                                                               \
        remote_dpu_info_t *new_conn_to;                                                                             \
        DYN_LIST_GET(_cfg->info_connecting_to.pool_remote_dpu_info, remote_dpu_info_t, item, new_conn_to);          \
        assert(new_conn_to);                                                                                        \
        conn_params_t *new_conn_params;                                                                             \
        DYN_LIST_GET(_cfg->offloading_engine->pool_conn_params, conn_params_t, item, new_conn_params);              \
        assert(new_conn_params);                                                                                    \
        new_conn_to->hostname = _dpu_hostname;                                                                      \
        new_conn_to->init_params.conn_params = new_conn_params;                                                     \
        new_conn_to->init_params.conn_params->addr_str = token;                                                     \
        if (_cfg->local_dpu.interdpu_conn_params.port > 0)                                                          \
        {                                                                                                           \
            /* fixme: this is not working right now but not really needed for our current use cases */              \
            /* if (init_params->conn_params->port_str != NULL) */                                                   \
            /*      new_conn_to->init_params.conn_params->port_str = strdup(init_params->conn_params->port_str); */ \
            new_conn_to->init_params.conn_params->port_str = NULL;                                                  \
            new_conn_to->init_params.conn_params->port = _cfg->local_dpu.interdpu_conn_params.port;                 \
        }                                                                                                           \
        new_conn_to->offload_engine = _cfg->offloading_engine;                                                      \
        ucs_list_add_tail(&(_cfg->info_connecting_to.connect_to), &(new_conn_to->item));                            \
        _cfg->info_connecting_to.num_connect_to++;                                                                  \
    } while (0)

/**********************/
/* ACTIVE MESSAGE IDS */
/**********************/

typedef enum
{
    AM_TERM_MSG_ID = 33, // 33 to make it easier to see corruptions (dbg)
    AM_EVENT_MSG_ID,
    AM_OP_START_MSG_ID, // 35
    AM_OP_COMPLETION_MSG_ID,
    AM_XGVMI_ADD_MSG_ID,
    AM_XGVMI_DEL_MSG_ID,
    AM_PEER_CACHE_REQ_MSG_ID,
    AM_PEER_CACHE_ENTRIES_MSG_ID, // 40
    AM_TEST_MSG_ID
} am_id_t;

_EXTERN_C_END

#endif // DPU_OFFLOAD_TYPES_H