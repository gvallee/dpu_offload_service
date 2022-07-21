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
#include <limits.h>

#include "dynamic_structs.h"
#include "dpu_offload_common.h"
#include "dpu_offload_utils.h"

_EXTERN_C_BEGIN

#define DEFAULT_INTER_DPU_CONNECT_PORT (11111)
#define DEFAULT_NUM_PEERS (10000)

typedef enum
{
    CONTEXT_UNKOWN = 0,
    CONTEXT_CLIENT,
    CONTEXT_SERVER,
    CONTEXT_SELF,
    CONTEXT_LIMIT_MAX,
} daemon_type_t;

#define INIT_UCX() ({                                                      \
    ucp_params_t ucp_params;                                               \
    ucs_status_t status;                                                   \
    ucp_config_t *config;                                                  \
    ucp_context_h ucp_context = NULL;                                      \
    memset(&ucp_params, 0, sizeof(ucp_params));                            \
    status = ucp_config_read(NULL, NULL, &config);                         \
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;                      \
    ucp_params.features = UCP_FEATURE_TAG |                                \
                          UCP_FEATURE_AM |                                 \
                          UCP_FEATURE_RMA;                                 \
    status = ucp_init(&ucp_params, config, &(ucp_context));                \
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR,                         \
                     "ucp_init() failed: %s",                              \
                     ucs_status_string(status));                           \
    /* ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG); */ \
    ucp_config_release(config);                                            \
    ucp_context;                                                           \
})

#define INIT_WORKER(_ucp_context, _ucp_worker) ({                                                                  \
    ucp_worker_params_t _worker_params;                                                                            \
    ucs_status_t _status;                                                                                          \
    int _ret = DO_SUCCESS;                                                                                         \
    memset(&_worker_params, 0, sizeof(_worker_params));                                                            \
    _worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;                                                \
    _worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;                                                           \
    _status = ucp_worker_create(_ucp_context, &_worker_params, _ucp_worker);                                       \
    CHECK_ERR_RETURN((_status != UCS_OK), DO_ERROR, "ucp_worker_create() failed: %s", ucs_status_string(_status)); \
    _ret;                                                                                                          \
})

#define ECONTEXT_ID(_exec_ctx) ({         \
    uint64_t _my_id;                      \
    switch ((_exec_ctx)->type)            \
    {                                     \
    case CONTEXT_CLIENT:                  \
        _my_id = (_exec_ctx)->client->id; \
        break;                            \
    case CONTEXT_SERVER:                  \
        _my_id = (_exec_ctx)->server->id; \
        break;                            \
    default:                              \
        /* including self */              \
        _my_id = 0;                       \
    }                                     \
    _my_id;                               \
})

#define GET_SERVER_EP(_exec_ctx) ({           \
    ucp_ep_h _ep;                             \
    if ((_exec_ctx)->type == CONTEXT_CLIENT)  \
    {                                         \
        _ep = (_exec_ctx)->client->server_ep; \
    }                                         \
    else                                      \
    {                                         \
        _ep = NULL;                           \
    }                                         \
    _ep;                                      \
})

/**
 * @brief GET_CLIENT_EP returns the UCX endpoint based on a client's ID (warning, the ID is not equal to rank)
 */
#define GET_CLIENT_EP(_exec_ctx, _client_id) ({                                                 \
    ucp_ep_h _ep = NULL;                                                                        \
    if ((_exec_ctx)->type == CONTEXT_SERVER)                                                    \
    {                                                                                           \
        peer_info_t *_pi = DYN_ARRAY_GET_ELT(&((_exec_ctx)->server->connected_clients.clients), \
                                             _client_id,                                        \
                                             peer_info_t);                                      \
        assert(_pi);                                                                            \
        _ep = _pi->ep;                                                                          \
    }                                                                                           \
    _ep;                                                                                        \
})

// Todo: rename the structure, it is not limited to clients
typedef struct dest_client
{
    ucp_ep_h ep;
    uint64_t id;
} dest_client_t;

#define GET_CLIENT_BY_RANK(_exec_ctx, _gp_id, _rank) ({                                                 \
    dest_client_t _c;                                                                                   \
    _c.ep = NULL;                                                                                       \
    _c.id = UINT64_MAX;                                                                                 \
    size_t _idx = 0;                                                                                    \
    size_t _n = 0;                                                                                      \
    if ((_exec_ctx)->type == CONTEXT_SERVER)                                                            \
    {                                                                                                   \
        while (_n < (_exec_ctx)->server->connected_clients.num_connected_clients)                       \
        {                                                                                               \
            peer_info_t *_p_info = DYN_ARRAY_GET_ELT(&((_exec_ctx)->server->connected_clients.clients), \
                                                     _idx,                                              \
                                                     peer_info_t);                                      \
            if (_p_info == NULL)                                                                        \
            {                                                                                           \
                _idx++;                                                                                 \
                continue;                                                                               \
            }                                                                                           \
            /* FIXME: support more than one group */                                                    \
            if (_p_info->rank_data.group_id == _gp_id && _p_info->rank_data.group_rank == _rank)        \
            {                                                                                           \
                _c.ep = _p_info->ep;                                                                    \
                _c.id = _p_info->id;                                                                    \
                break;                                                                                  \
            }                                                                                           \
            _n++;                                                                                       \
            _idx++;                                                                                     \
        }                                                                                               \
    }                                                                                                   \
    _c;                                                                                                 \
})

#define GET_WORKER(_exec_ctx) ({                       \
    ucp_worker_h _w = (_exec_ctx)->engine->ucp_worker; \
    _w;                                                \
})

#define SET_WORKER(_exec_ctx, _worker)             \
    do                                             \
    {                                              \
        (_exec_ctx)->engine->ucp_worker = _worker; \
    } while (0)

#define EV_SYS(_exec_ctx) ({                        \
    dpu_offload_ev_sys_t *_sys = NULL;              \
    switch ((_exec_ctx)->type)                      \
    {                                               \
    case CONTEXT_CLIENT:                            \
        _sys = (_exec_ctx)->client->event_channels; \
        break;                                      \
    case CONTEXT_SERVER:                            \
        _sys = (_exec_ctx)->server->event_channels; \
        break;                                      \
    case CONTEXT_SELF:                              \
        _sys = (_exec_ctx)->event_channels;         \
        break;                                      \
    default:                                        \
        break;                                      \
    }                                               \
    _sys;                                           \
})

#define EXECUTION_CONTEXT_DONE(_exec_ctx) ({ \
    bool _done;                              \
    ECONTEXT_LOCK((_exec_ctx));              \
    if ((_exec_ctx)->type == CONTEXT_CLIENT) \
        _done = (_exec_ctx)->client->done;   \
    else                                     \
        _done = (_exec_ctx)->server->done;   \
    ECONTEXT_UNLOCK((_exec_ctx));            \
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

/**************/
/* OPERATIONS */
/**************/

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

#define RESET_OP_DESC(_op_desc)           \
    do                                    \
    {                                     \
        (_op_desc)->id = 0;               \
        (_op_desc)->op_definition = NULL; \
        (_op_desc)->op_data = NULL;       \
        (_op_desc)->completed = false;    \
    } while (0)

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
    // ID of the group associated to the rank
    int64_t group_id;

    // Rank in the group
    int64_t group_rank;

    // Size of the group associated to the rank
    int64_t group_size;

    // Number of ranks on the host. Used for optimization. Can be set to any negative value when unknown
    int64_t n_local_ranks;

    // Rank on the host, can be used to figure out which service process on the local DPU to connect to.
    int64_t local_rank;
} rank_info_t;

#define RESET_RANK_INFO(_r)              \
    do                                   \
    {                                    \
        (_r)->group_id = INVALID_GROUP;  \
        (_r)->group_rank = INVALID_RANK; \
        (_r)->group_size = 0;            \
        (_r)->n_local_ranks = 0;         \
        (_r)->local_rank = INVALID_RANK; \
    } while (0)

#define COPY_RANK_INFO(__s, __d)                     \
    do                                               \
    {                                                \
        (__d)->group_id = (__s)->group_id;           \
        (__d)->group_rank = (__s)->group_rank;       \
        (__d)->group_size = (__s)->group_size;       \
        (__d)->n_local_ranks = (__s)->n_local_ranks; \
        (__d)->local_rank = (__s)->local_rank;       \
    } while (0)

// fixme: long term, we do not want to have a limit on the length of the address
// but this will require a new smart way to manage the memory used by cache entries
// and avoid expensive copies when exchanging cache entries between DPUs and
// application processes
#define MAX_ADDR_LEN (2048)

#define MAX_SHADOW_SERVICE_PROCS (8)

#define SELF_DPU(_engine, _dpu_index) ({                                             \
    bool _is_self = (_engine)->on_dpu;                                               \
    if (_is_self == true)                                                            \
    {                                                                                \
        _is_self = false;                                                            \
        if ((_engine)->config != NULL)                                               \
        {                                                                            \
            offloading_config_t *_config = (offloading_config_t *)(_engine)->config; \
            if (_dpu_index == _config->local_dpu.id)                                 \
                _is_self = true;                                                     \
        }                                                                            \
    }                                                                                \
    _is_self;                                                                        \
})

// peer_data_t stores all the information related to a rank in a group,
// it is designed in a way it can be directly sent without requiring
// memory copies.
typedef struct peer_data
{
    rank_info_t proc_info;
    size_t addr_len;
    char addr[MAX_ADDR_LEN]; // ultimately ucp_address_t * when using UCX
} peer_data_t;

#define RESET_PEER_DATA(_d)               \
    do                                    \
    {                                     \
        RESET_RANK_INFO((_d)->proc_info); \
        (_d)->addr_len = 0;               \
        (_d)->addr = NULL;                \
    } while (0)

#define COPY_PEER_DATA(_src, _dst)                                  \
    do                                                              \
    {                                                               \
        COPY_RANK_INFO(&((_src)->proc_info), &((_dst)->proc_info)); \
        assert((_src)->addr_len < MAX_ADDR_LEN);                    \
        (_dst)->addr_len = (_src)->addr_len;                        \
        if ((_src)->addr_len > 0)                                   \
            strncpy((_dst)->addr, (_src)->addr, (_src)->addr_len);  \
    } while (0)

typedef struct shadow_service_proc_info
{
    peer_data_t shadow_data; // Can be NULL if applied to DPU specific data
    ucp_ep_h shadow_ep;      // endpoint to reach the attached DPU
} shadow_service_pro_info_t;

typedef struct peer_cache_entry
{
    // Is the entry set?
    bool set;

    // Peer data (group/rank)
    peer_data_t peer;

    // endpoint to reach the peer
    ucp_ep_h ep;

    // Number of the peer's shadow service process(es)
    size_t num_shadow_service_procs;

    // List of DPUs' unique global IDs that are the shadow DPU(s) of the peer.
    // The global DPU identifier is based on the index in the common list of DPU
    // to use for the job.
    uint64_t shadow_service_procs[MAX_SHADOW_SERVICE_PROCS]; // Array of DPUs (when applicable)

    // Is the list of events already initialized or not (lazy initialization)
    bool events_initialized;

    // List of events to complete when any update is made to the entry
    ucs_list_link_t events;
} peer_cache_entry_t;

typedef struct cache_entry_request
{
    ucs_list_link_t item;

    struct offloading_engine *offload_engine;

    // ID of the target service process ID in case multiple service processes are attached to the target group/rank
    uint64_t target_sp_idx;

    int64_t gp_id;
    int64_t rank;
} cache_entry_request_t;

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

    // Size of the payload. Not always used, e.g., when using UCX AM, this is a piece
    // of data that is directly provided; however, when using tag send/recv, this is
    // used to know the size to expect when we post the receive for the payload.
    uint64_t payload_size;

#if !NDEBUG
    uint64_t event_id;
    uint64_t client_id;
    uint64_t server_id;
#endif
} am_header_t; // todo: rename, nothing to do with AM

#define RESET_AM_HDR(_h)        \
    do                          \
    {                           \
        (_h)->id = 0;           \
        (_h)->type = 0;         \
        (_h)->payload_size = 0; \
    } while (0)

typedef void (*request_compl_cb_t)(void *);

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
    bool complete;

    // Callback to invoke upon completion
    request_compl_cb_t completion_cb;

    // Context to be passed in the completion callback
    void *completion_cb_ctx;
} am_req_t; // todo: rename, nothing to do with AM

#define RESET_AM_REQ(_r)                \
    do                                  \
    {                                   \
        RESET_AM_HDR(&((_r)->hdr));     \
        (_r)->complete = false;         \
        (_r)->completion_cb = NULL;     \
        (_r)->completion_cb_ctx = NULL; \
    } while (0)

typedef struct event_req
{
    // Header associated to the notification.
    am_header_t hdr;

    bool hdr_completed;
    bool payload_completed;

    // Callback to invoke upon completion
    request_compl_cb_t completion_cb;

    // Context to be passed in the completion callback
    void *completion_cb_ctx;
} event_req_t;

#define RESET_EVENT_REQ(_r)              \
    do                                   \
    {                                    \
        RESET_AM_HDR(&((_r)->hdr));      \
        (_r)->hdr_completed = false;     \
        (_r)->payload_completed = false; \
        (_r)->completion_cb = NULL;      \
        (_r)->completion_cb_ctx = NULL;  \
    } while (0)

#if !USE_AM_IMPLEM
// Forward declaration
struct execution_context;

typedef void *(*get_buf_fn)(void *pool, void *args);
typedef void (*return_buf_fn)(void *pool, void *buf);

typedef struct notification_info
{
    // Optional function to get a buffer from a pool
    get_buf_fn get_buf;
    // Optional function to return a buffer to a pool
    return_buf_fn return_buf;
    // Memory pool to get notification payload buffer
    void *mem_pool;
    // Optional arguments to pass to the get function
    void *get_buf_args;
    // Size of the elements in the list
    size_t element_size;
} notification_info_t;

#define RESET_NOTIF_INFO(__info)       \
    do                                 \
    {                                  \
        (__info)->get_buf = NULL;      \
        (__info)->return_buf = NULL;   \
        (__info)->mem_pool = NULL;     \
        (__info)->get_buf_args = NULL; \
        (__info)->element_size = 0;    \
    } while (0)

#define COPY_NOTIF_INFO(_src, _dst)                  \
    do                                               \
    {                                                \
        (_dst)->get_buf = (_src)->get_buf;           \
        (_dst)->return_buf = (_src)->return_buf;     \
        (_dst)->mem_pool = (_src)->mem_pool;         \
        (_dst)->get_buf_args = (_src)->get_buf_args; \
        (_dst)->element_size = (_src)->element_size; \
    } while (0)

#define CHECK_NOTIF_INFO(__info)                \
    do                                          \
    {                                           \
        assert((__info)->get_buf == NULL);      \
        assert((__info)->return_buf == NULL);   \
        assert((__info)->mem_pool == NULL);     \
        assert((__info)->get_buf_args == NULL); \
        assert((__info)->element_size == 0);    \
    } while (0)

typedef struct payload_notif_req
{
    bool complete;
    ucp_request_param_t recv_params;
    notification_info_t pool;
    smart_chunk_t *smart_buf;
    void *buffer;
    struct ucx_context *req;
} payload_notif_req_t;

#define RESET_PAYLOAD_NOTIF_REQ(_r)      \
    do                                   \
    {                                    \
        (_r)->complete = false;          \
        (_r)->smart_buf = NULL;          \
        (_r)->buffer = NULL;             \
        (_r)->req = NULL;                \
        RESET_NOTIF_INFO(&((_r)->pool)); \
    } while (0)

typedef struct hdr_notif_req
{
    bool complete;
    am_header_t hdr;
    struct ucx_context *req;
    uint64_t client_id;
    uint64_t server_id;
    struct execution_context *econtext;
    payload_notif_req_t payload_ctx;
} hdr_notif_req_t;

#define RESET_HDR_NOTIF_REQ(_r)                        \
    do                                                 \
    {                                                  \
        (_r)->complete = false;                        \
        RESET_AM_HDR(&((_r)->hdr));                    \
        (_r)->req = NULL;                              \
        (_r)->client_id = UINT64_MAX;                  \
        (_r)->server_id = UINT64_MAX;                  \
        (_r)->econtext = NULL;                         \
        RESET_PAYLOAD_NOTIF_REQ(&((_r)->payload_ctx)); \
    } while (0)

typedef struct notif_reception
{
    bool initialized;
    hdr_notif_req_t ctx;
    ucp_request_param_t hdr_recv_params;
    ucp_tag_t hdr_ucp_tag;
    ucp_tag_t hdr_ucp_tag_mask;
    struct ucx_context *req;
} notif_reception_t;

#define RESET_NOTIF_RECEPTION(_n)          \
    do                                     \
    {                                      \
        (_n)->initialized = false;         \
        RESET_HDR_NOTIF_REQ(&((_n)->ctx)); \
        (_n)->req = NULL;                  \
    } while (0)
#endif

typedef struct boostrapping
{
    int phase;
    struct ucx_context *addr_size_request;
    am_req_t addr_size_ctx;
    struct ucx_context *addr_request;
    am_req_t addr_ctx;
    struct ucx_context *rank_request;
    am_req_t rank_ctx;
} bootstrapping_t;

#define RESET_BOOTSTRAPPING(_b)                \
    do                                         \
    {                                          \
        (_b)->phase = BOOTSTRAP_NOT_INITIATED; \
        (_b)->addr_size_request = NULL;        \
        RESET_AM_REQ(&((_b)->addr_size_ctx));  \
        (_b)->addr_request = NULL;             \
        RESET_AM_REQ(&((_b)->addr_ctx));       \
        (_b)->rank_request = NULL;             \
        RESET_AM_REQ(&((_b)->rank_ctx));       \
    } while (0)

typedef struct peer_info
{
    bootstrapping_t bootstrapping;

    uint64_t id;

#if !USE_AM_IMPLEM
    notif_reception_t notif_recv;
#endif

    // Length of the peer's address
    size_t peer_addr_len;

    // Peer's address. Used to create endpoint when using OOB
    void *peer_addr;

    // Human readable version of the address used for bootstrapping
    // todo: this is current used to lookup which DPU is in the process of connecting, we should not have to do lookup based on a string
    char *peer_addr_str;

    // UCX endpoint to communicate with the peer
    ucp_ep_h ep;

    // Peer's endpoint status
    ucs_status_t ep_status;

    rank_info_t rank_data;

    // Dynamic array of group/proc entries, one per group. A rank can belong to multiple groups but have a single endpoint.
    // Type: peer_cache_entry_t *
    dyn_array_t cache_entries;
} peer_info_t;

/**********************************************/
/* PUBLIC STRUCTURES RELATED TO NOTIFICATIONS */
/**********************************************/

/**
 * @brief dpu_offload_ev_sys_t is the structure representing the event system used to implement notifications.
 */
typedef struct dpu_offload_ev_sys
{
    pthread_mutex_t mutex;

    // Pool of available events from which objects are taken when invoking event_get().
    // Once the object obtained, one can populate the event-specific data and emit the event.
    // From a communication point-of-view, these objects are therefore used on the send side.
    dyn_list_t *free_evs;

    // Current number of event objects from the pool that are being used.
    // Note that it means these objects are not in the pool and must be returned at some points.
    size_t num_used_evs;

    // Current number of event sends that are posted. Used to manage how many events can be
    // being sent at any given time
    size_t posted_sends;

    /* pending notifications are notifications that cannot be delivered upon reception because the callback is not registered yet */
    ucs_list_link_t pending_notifications;

    // free_pending_notifications is a pool oof pending notification objects that can be used when a notification is received and
    // no callback is registered yet. It avoids allocating memory.
    dyn_list_t *free_pending_notifications;

    // Array of callback functions, i.e., array of pointers, organized based on the notification type, a.k.a. notification ID
    dyn_array_t notification_callbacks;

    // Execution context the event system is associated with.
    struct execution_context *econtext;

#if !USE_AM_IMPLEM
    notif_reception_t notif_recv;
#endif
} dpu_offload_ev_sys_t;

#if !USE_AM_IMPLEM
#define RESET_EV_SYS(_s)                            \
    do                                              \
    {                                               \
        (_s)->free_evs = NULL;                      \
        (_s)->num_used_evs = 0;                     \
        (_s)->posted_sends = 0;                     \
        (_s)->free_pending_notifications = NULL;    \
        (_s)->econtext = 0;                         \
        RESET_NOTIF_RECEPTION(&((_s)->notif_recv)); \
    } while (0)
#else
#define RESET_EV_SYS(_s)                         \
    do                                           \
    {                                            \
        (_s)->free_evs = NULL;                   \
        (_s)->num_used_evs = 0;                  \
        (_s)->posted_sends = 0;                  \
        (_s)->free_pending_notifications = NULL; \
        (_s)->econtext = 0;                      \
    } while (0)
#endif

typedef struct connected_clients
{
    // Number of clients that connected over time
    size_t num_total_connected_clients;

    // Number of clients currently fully connected (bootstrapping completed)
    size_t num_connected_clients;

    // Number of clients in the process of connecting
    size_t num_ongoing_connections;

    // Dynamic array of structures to track connected clients (type: peer_info_t)
    dyn_array_t clients;
} connected_clients_t;

#define RESET_CONNECTED_CLIENTS(_c)            \
    do                                         \
    {                                          \
        (_c)->num_total_connected_clients = 0; \
        (_c)->num_connected_clients = 0;       \
        (_c)->num_ongoing_connections = 0;     \
    } while (0)

/**
 * @brief connected_peer_data is the data that can be passed to a connection completion
 * callback. In other words, it gathers all the data to identify a peer that just
 * successfully completed its connection.
 */
typedef struct connected_peer_data
{
    // IP of the peer that just completed its connection
    char *peer_addr;

    // Associated execution context
    struct execution_context *econtext;

    // Peer's ID used for the connection
    uint64_t peer_id;

    // Peer's unique global ID, valid mainly in the context of inter-service-processes connections.
    uint64_t global_peer_id;

    rank_info_t rank_info;
} connected_peer_data_t;

#define RESET_CONNECTED_PEER_DATA(_d)      \
    do                                     \
    {                                      \
        (_d)->peer_addr = NULL;            \
        (_d)->econtext = NULL;             \
        (_d)->peer_id = UINT64_MAX;        \
        (_d)->global_peer_id = UINT64_MAX; \
        REST_RANK_DATA((_d)->rank_info);   \
    } while (0)

#define COPY_CONNECTED_PEER_DATA(_src, _dest)                  \
    do                                                         \
    {                                                          \
        (_dest)->peer_addr = strdup((_src)->peer_addr);        \
        (_dest)->econtext = (_src)->econtext;                  \
        (_dest)->peer_id = (_src)->peer_id;                    \
        COPY_RANK_INFO((_dest)->rank_info, (_src)->rank_info); \
    } while (0)

typedef struct conn_params
{
    ucs_list_link_t item;
    char *addr_str;
    char *port_str;
    int port;
    struct sockaddr_storage saddr;
} conn_params_t;

#define RESET_CONN_PARAMS(_params)  \
    do                              \
    {                               \
        (_params)->addr_str = NULL; \
        (_params)->port_str = NULL; \
        (_params)->port = -1;       \
    } while (0)

/**
 * @brief connect_completed_cb is the type of the callback used when a connection completes.
 * It can be used both on a client and server.
 */
typedef void (*connect_completed_cb)(void *);

typedef enum
{
    SCOPE_HOST_DPU = 0,
    SCOPE_INTER_SERVICE_PROCS,
    SCOPE_SELF,
} execution_scope_t;

typedef struct init_params
{
    // Identifier to know the context, i.e., between DPUs or host-DPU.
    // This is for instance used to differentiate communications between DPUs
    // and DPU-host when using the tag send/recv implementation of notifications.
    execution_scope_t scope_id;

    // Parameters specific to the initial connection
    conn_params_t *conn_params;

    // Proc identifier passed in by the calling layer.
    // Mainly used to create the mapping between group/rank from layer such as MPI
    rank_info_t *proc_info;

    // worker to use to perform the initial connection.
    // If NULL, a new worker will be created
    ucp_worker_h worker;

    // UCP context and reusing a worker from another library
    ucp_context_h ucp_context;

    // Specifies whether a unique ID is passed in and should be used when creating the execution context
    bool id_set;

    // Optional unique ID to use when creating the execution context
    uint64_t id;

    // Callback to invoke when a connection completes
    connect_completed_cb connected_cb;
} init_params_t;

#define RESET_INIT_PARAMS(_params)            \
    do                                        \
    {                                         \
        (_params)->conn_params = NULL;        \
        (_params)->proc_info = NULL;          \
        (_params)->worker = NULL;             \
        (_params)->ucp_context = NULL;        \
        (_params)->id_set = false;            \
        (_params)->connected_cb = NULL;       \
        (_params)->scope_id = SCOPE_HOST_DPU; \
    } while (0)

#define SYS_EVENT_LOCK(_sys_evt)                  \
    do                                            \
    {                                             \
        pthread_mutex_lock(&((_sys_evt)->mutex)); \
    } while (0)

#define SYS_EVENT_UNLOCK(_sys_evt)                  \
    do                                              \
    {                                               \
        pthread_mutex_unlock(&((_sys_evt)->mutex)); \
    } while (0)

#define ENGINE_LOCK(_engine)                     \
    do                                           \
    {                                            \
        pthread_mutex_lock(&((_engine)->mutex)); \
    } while (0)

#define ENGINE_UNLOCK(_engine)                     \
    do                                             \
    {                                              \
        pthread_mutex_unlock(&((_engine)->mutex)); \
    } while (0)

#define CLIENT_LOCK(_client)                     \
    do                                           \
    {                                            \
        pthread_mutex_lock(&((_client)->mutex)); \
    } while (0)

#define CLIENT_UNLOCK(_client)                     \
    do                                             \
    {                                              \
        pthread_mutex_unlock(&((_client)->mutex)); \
    } while (0)

#define SERVER_LOCK(_server)                     \
    do                                           \
    {                                            \
        pthread_mutex_lock(&((_server)->mutex)); \
    } while (0)

#define SERVER_UNLOCK(_server)                     \
    do                                             \
    {                                              \
        pthread_mutex_unlock(&((_server)->mutex)); \
    } while (0)

#define ECONTEXT_LOCK(_econtext)                   \
    do                                             \
    {                                              \
        pthread_mutex_lock(&((_econtext)->mutex)); \
        switch ((_econtext)->type)                 \
        {                                          \
        case CONTEXT_CLIENT:                       \
            CLIENT_LOCK((_econtext)->client);      \
            break;                                 \
        case CONTEXT_SERVER:                       \
            SERVER_LOCK((_econtext)->server);      \
            break;                                 \
        default:                                   \
            break;                                 \
        }                                          \
    } while (0)

#define ECONTEXT_UNLOCK(_econtext)                   \
    do                                               \
    {                                                \
        pthread_mutex_unlock(&((_econtext)->mutex)); \
        switch ((_econtext)->type)                   \
        {                                            \
        case CONTEXT_CLIENT:                         \
            CLIENT_UNLOCK((_econtext)->client);      \
            break;                                   \
        case CONTEXT_SERVER:                         \
            SERVER_UNLOCK((_econtext)->server);      \
            break;                                   \
        default:                                     \
            break;                                   \
        }                                            \
    } while (0)

#define ADD_CLIENT_TO_ENGINE(_client, _engine) \
    do                                         \
    {                                          \
        (_engine)->client = _client;           \
    } while (0)

#define ADD_SERVER_TO_ENGINE(_server, _engine)                \
    do                                                        \
    {                                                         \
        (_engine)->servers[(_engine)->num_servers] = _server; \
        (_engine)->num_servers++;                             \
    } while (0)

typedef struct dpu_offload_server_t
{
    uint64_t id;

    // Execution context the server is associated to
    struct execution_context *econtext;

    int mode;
    bool done;
    conn_params_t conn_params;
    pthread_t connect_tid;
    pthread_mutex_t mutex;
    pthread_mutexattr_t mattr;
    connected_clients_t connected_clients;

    // Callback to invoke when a connection completes
    connect_completed_cb connected_cb;

    dpu_offload_ev_sys_t *event_channels;

    // Vector to track which groups have been sent to the host once all the local ranks
    // showed up. Only used on DPUs.
    dyn_array_t local_groups_sent_to_host;

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

#define RESET_SERVER(_server)                                     \
    do                                                            \
    {                                                             \
        (_server)->id = UINT64_MAX;                               \
        (_server)->econtext = NULL;                               \
        (_server)->done = false;                                  \
        (_server)->mode = -1;                                     \
        RESET_CONN_PARAMS(&((_server)->conn_params));             \
        RESET_CONNECTED_CLIENTS(&((_server)->connected_clients)); \
        (_server)->connected_cb = NULL;                           \
        (_server)->event_channels = NULL;                         \
        DYN_ARRAY_ALLOC(&((_server)->local_groups_sent_to_host),  \
                        8, bool);                                 \
    } while (0)

typedef struct dpu_offload_client_t
{
    bootstrapping_t bootstrapping;

    // Index in the list of clietns
    int64_t idx;

    // Identifier assigned by server
    uint64_t id;

    // Unique identifier of the server
    uint64_t server_id;

    uint64_t server_global_id;

    // Execution context the server is associated to
    struct execution_context *econtext;

    int mode;
    conn_params_t conn_params;
    bool done;

    // Callback to invoke when a connection completes
    connect_completed_cb connected_cb;

    ucp_ep_h server_ep;
    ucs_status_t server_ep_status;
    pthread_mutex_t mutex;

    dpu_offload_ev_sys_t *event_channels;

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

#define RESET_CLIENT(_c)                             \
    do                                               \
    {                                                \
        RESET_BOOTSTRAPPING(&((_c)->bootstrapping)); \
        (_c)->id = UINT64_MAX;                       \
        (_c)->server_id = UINT64_MAX;                \
        (_c)->econtext = NULL;                       \
        (_c)->mode = INT_MAX;                        \
        RESET_CONN_PARAMS(&((_c)->conn_params));     \
        (_c)->done = false;                          \
        (_c)->connected_cb = NULL;                   \
        (_c)->server_ep = NULL;                      \
        (_c)->event_channels = NULL;                 \
    } while (0)

typedef void (*execution_context_progress_fn)(struct execution_context *);

struct offloading_engine; // forward declaration

#define ECONTEXT_ON_DPU(_ctx) ((_ctx)->engine->on_dpu)

typedef enum
{
    // Bootstrapping is not yet initiated but bootstrapping can be initiated any time.
    BOOTSTRAP_NOT_INITIATED = 0,

    // Bootstrapping OOB connection has been completed.
    OOB_CONNECT_DONE,

    // UCX connection has been completed.
    UCX_CONNECT_DONE,

    // The entire bootstrapping phase has been completed.
    BOOTSTRAP_DONE,

    // All connections are now disconnected, a new bootstrapping is necessary to communicate again with the remote entity.
    // The state may not be suitable to initiate a new bootstrapping.
    DISCONNECTED,
} bootstrap_phase_t;

// Forward declaration
struct dpu_offload_event;

/**
 * @brief execution_context_t is the structure holding all the information related to DPU offloading, both on the hosts and DPUs.
 * The primary goal of the structure is too abstract whether the process is a client or server during bootstrapping
 * and how many clients and servers are used for the deployment of the infrastructure.
 * It recommanded to use the associated macros in order to make the code more abstract and mainly rely on execution contexts.
 */
typedef struct execution_context
{
    pthread_mutex_t mutex;

    // type specifies if the execution context is a server or a client during the bootstrapping process
    daemon_type_t type;

    // Identifier to know the context, i.e., between DPUs or host-DPU.
    // This is for instance used to differentiate communications between DPUs
    // and DPU-host when using the tag send/recv implementation of notifications.
    execution_scope_t scope_id;

    // engine is the associated offloading engine
    struct offloading_engine *engine;

    // event_channels is the notification/event system of the execution context
    dpu_offload_ev_sys_t *event_channels;

    // ongoing_events is a list of ongoing events.
    // During progress of the execution context, the status of the event on the list is checked and
    // if completed, resources are freed and events are returned to the list of free event in the notification system.
    // In other words, once added to the list, there is no need to track the event and return them, it is all done implicitly.
    ucs_list_link_t ongoing_events;

    // progress function to invoke to progress the execution context
    execution_context_progress_fn progress;

    // rank is the process's group information optinally specified during bootstrapping.
    // In the context of a execution context running on the host and in the context of
    // an application, it can be the group/rank data from the runtime (e.g., MPI). This
    // is used to map the unique identifier of the proc to the DPU offload library during
    // bootstrapping. If not in such a context, it must be set to INVALID_GROUP and INVALID_RANK.
    rank_info_t rank;

    // free_pending_rdv_recv is a list of allocated descriptors used to track pending UCX AM RDV messages.
    // This list prevents us from allocating memory while handling AM RDV messages.
    dyn_list_t *free_pending_rdv_recv;

    // pending_rdv_recvs is the current list of pending AM RDV receives.
    // Once completed, the element is returned to free_pending_rdv_recv.
    ucs_list_link_t pending_rdv_recvs;

    // Active operations that are running in the execution context
    ucs_list_link_t active_ops;

    // Termination is asynchronous so we need to be able to track the associated event, so we can
    // check for completion and invoke to final step of the execution context termination (which
    // ends the notification system).
    struct
    {
        struct dpu_offload_event *ev;
    } term;

    // During bootstrapping, the execution context acts either as a client or server.
    union
    {
        dpu_offload_client_t *client;
        dpu_offload_server_t *server;
    };

} execution_context_t;

#define RESET_ECONTEXT(_e)                  \
    do                                      \
    {                                       \
        (_e)->type = CONTEXT_UNKOWN;        \
        (_e)->scope_id = SCOPE_HOST_DPU;    \
        (_e)->engine = NULL;                \
        (_e)->event_channels = NULL;        \
        (_e)->progress = NULL;              \
        RESET_RANK_INFO(&((_e)->rank));     \
        (_e)->free_pending_rdv_recv = NULL; \
        (_e)->term.ev = NULL;               \
    } while (0)

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

#define RESET_PENDING_RDV_RECV(_rdv_recv)                                                                  \
    do                                                                                                     \
    {                                                                                                      \
        (_rdv_recv)->econtext = NULL;                                                                      \
        (_rdv_recv)->hdr_len = 0;                                                                          \
        (_rdv_recv)->hdr = NULL;                                                                           \
        (_rdv_recv)->req = NULL;                                                                           \
        (_rdv_recv)->payload_size = 0;                                                                     \
        (_rdv_recv)->desc = NULL;                                                                          \
        /* Do not reset user_data and buff_size as it is used over time as a buffer to minimize mallocs */ \
    } while (0)

/**
 * @brief dpu_offload_event_t represents an event, i.e., the implementation of a notification
 */
typedef struct dpu_offload_event
{
    // item is used to be able to add/remove the event to lists, e.g., the list for ongoing events and the pool of free event objects.
    ucs_list_link_t item;

    uint64_t seq_num;

#if USE_AM_IMPLEM
    // ctx is the communication context associated to the event, used to track the status of the potential underlying UCX AM communication
    am_req_t ctx;
#else
    event_req_t ctx;
    struct ucx_context *hdr_request;
    struct ucx_context *payload_request;
    int scope_id;
#endif

#if !NDEBUG
    uint64_t client_id;
    uint64_t server_id;
#endif

    // sub_events is the list of sub-events composing this event.
    // The event that has sub-events is not considered completed unless all sub-events are completed.
    // event_completed() can be used to easily check for completion.
    ucs_list_link_t sub_events;

    // sub_events_initialized tracks whether the sub-event list has been initialized.
    bool sub_events_initialized;

    bool is_subevent;

    bool is_ongoing_event;

    bool was_posted;

    // req is the opaque request object used to track any potential underlying communication associated to the event.
    // If more than one communication operation is required, please use sub-events.
    void *req;

    // context is the user defined context of the event. Can be NULL.
    void *context;

    // data is the payload associated to the event. Can be NULL.
    void *data;

    // user_context is the user-defined context for the event. Can be NULL.
    void *user_context;

    // Specifies whether the user is in charge of explicitly returning the event or not (false by default)
    bool explicit_return;

    // Specifies whether the payload buffer needs to be managed by the library.
    // If so, it uses the payload_size from the infro structure used when getting
    // the event to allocate/get the buffer and associate it to the event.
    bool manage_payload_buf;

    // payload buffer when the library manages it. Its size is stored in the header object.
    void *payload;

    // Destination endpoint for remote events
    struct
    {
        ucp_ep_h ep;
        uint64_t id;
    } dest;

    // event_system is the event system the event was initially from
    dpu_offload_ev_sys_t *event_system;

    notification_info_t info;
} dpu_offload_event_t;

#define EVENT_HDR_ID(_ev) (_ev)->ctx.hdr.id
#define EVENT_HDR_TYPE(_ev) (_ev)->ctx.hdr.type
#define EVENT_HDR_PAYLOAD_SIZE(_ev) (_ev)->ctx.hdr.payload_size
#define EVENT_HDR(_ev) &((_ev)->ctx.hdr)
#if !NDEBUG
#define EVENT_HDR_CLIENT_ID(_ev) (_ev)->ctx.hdr.client_id
#define EVENT_HDR_SERVER_ID(_ev) (_ev)->ctx.hdr.server_id
#define EVENT_HDR_SEQ_NUM(_ev) (_ev)->ctx.hdr.event_id
#endif

/**
 * @brief RESET_EVENT does not reinitialize sub_events_initialized because if is done
 * only once and reuse as events are reused. However, it is initialized when the
 * dynamic list is initialized
 */
#if USE_AM_IMPLEM
#define RESET_EVENT(__ev)                     \
    do                                        \
    {                                         \
        (__ev)->context = NULL;               \
        (__ev)->payload = NULL;               \
        (__ev)->event_system = NULL;          \
        (__ev)->req = NULL;                   \
        (__ev)->ctx.complete = false;         \
        (__ev)->ctx.completion_cb = NULL;     \
        (__ev)->ctx.completion_cb_ctx = NULL; \
        EVENT_HDR_TYPE(__ev) = UINT64_MAX;    \
        EVENT_HDR_ID(__ev) = UINT64_MAX;      \
        EVENT_HDR_PAYLOAD_SIZE(__ev) = 0;     \
        (__ev)->manage_payload_buf = false;   \
        (__ev)->explicit_return = false;      \
        (__ev)->dest.ep = NULL;               \
        (__ev)->dest.id = UINT64_MAX;         \
        (__ev)->scope_id = SCOPE_HOST_DPU;    \
        (__ev)->is_subevent = false;          \
        (__ev)->is_ongoing_event = false;     \
        (__ev)->was_posted = false;           \
        RESET_NOTIF_INFO(&((__ev)->info));    \
    } while (0)
#else
#define RESET_EVENT(__ev)                      \
    do                                         \
    {                                          \
        (__ev)->context = NULL;                \
        (__ev)->payload = NULL;                \
        (__ev)->event_system = NULL;           \
        (__ev)->req = NULL;                    \
        (__ev)->ctx.hdr_completed = false;     \
        (__ev)->ctx.payload_completed = false; \
        (__ev)->ctx.completion_cb = NULL;      \
        (__ev)->ctx.completion_cb_ctx = NULL;  \
        EVENT_HDR_TYPE(__ev) = UINT64_MAX;     \
        EVENT_HDR_ID(__ev) = UINT64_MAX;       \
        EVENT_HDR_PAYLOAD_SIZE(__ev) = 0;      \
        (__ev)->manage_payload_buf = false;    \
        (__ev)->explicit_return = false;       \
        (__ev)->dest.ep = NULL;                \
        (__ev)->dest.id = UINT64_MAX;          \
        (__ev)->scope_id = SCOPE_HOST_DPU;     \
        (__ev)->hdr_request = NULL;            \
        (__ev)->payload_request = NULL;        \
        (__ev)->is_subevent = false;           \
        (__ev)->is_ongoing_event = false;      \
        (__ev)->was_posted = false;            \
        RESET_NOTIF_INFO(&((__ev)->info));     \
    } while (0)
#endif

#if USE_AM_IMPLEM
#define CHECK_EVENT(__ev)                                     \
    do                                                        \
    {                                                         \
        assert((__ev)->ctx.complete == 0);                    \
        assert(EVENT_HDR_PAYLOAD_SIZE(__ev) == 0);            \
        assert(EVENT_HDR_TYPE(__ev) == UINT64_MAX);           \
        assert(EVENT_HDR_ID(__ev) == UINT64_MAX);             \
        assert((__ev)->manage_payload_buf == false);          \
        assert((__ev)->explicit_return == false);             \
        assert((__ev)->dest.ep == NULL);                      \
        assert((__ev)->dest.id == UINT64_MAX);                \
        if ((__ev)->sub_events_initialized)                   \
        {                                                     \
            assert(ucs_list_is_empty(&((__ev)->sub_events))); \
        }                                                     \
        assert((__ev)->is_subevent == false);                 \
        assert((__ev)->is_ongoing_event == false);            \
        assert((__ev)->was_posted == false);                  \
        CHECK_NOTIF_INFO(&((__ev)->info));                    \
    } while (0)
#else
#define CHECK_EVENT(__ev)                                     \
    do                                                        \
    {                                                         \
        assert((__ev)->ctx.hdr_completed == 0);               \
        assert((__ev)->ctx.payload_completed == 0);           \
        assert(EVENT_HDR_PAYLOAD_SIZE(__ev) == 0);            \
        assert(EVENT_HDR_TYPE(__ev) == UINT64_MAX);           \
        assert(EVENT_HDR_ID(__ev) == UINT64_MAX);             \
        assert((__ev)->manage_payload_buf == false);          \
        assert((__ev)->explicit_return == false);             \
        assert((__ev)->dest.ep == NULL);                      \
        assert((__ev)->dest.id == UINT64_MAX);                \
        if ((__ev)->sub_events_initialized)                   \
        {                                                     \
            assert(ucs_list_is_empty(&((__ev)->sub_events))); \
        }                                                     \
        assert((__ev)->was_posted == false);                  \
        CHECK_NOTIF_INFO(&((__ev)->info));                    \
    } while (0)
#endif

typedef struct dpu_offload_event_info
{
    // Size of the payload that the library needs to be managing. If 0 not payload needs to be managed
    size_t payload_size;

    // Specify whether the user will explicitely return the event once done or not
    bool explicit_return;

    notification_info_t pool;
} dpu_offload_event_info_t;

#define RESET_EVENT_INFO(__info)             \
    do                                       \
    {                                        \
        (__info)->payload_size = 0;          \
        (__info)->explicit_return = false;   \
        RESET_NOTIF_INFO(&((__info)->pool)); \
    } while (0)

typedef enum
{
    EVENT_DONE = UCS_OK,
    EVENT_INPROGRESS = UCS_INPROGRESS
} event_state_t;

/*********************/
/* OFFLOADING ENGINE */
/*********************/

typedef struct cache
{
    // How many group caches that compose the cache are currently in use.
    size_t size;

    // data is a dynamic array for all the group caches (type: group_cache_t)
    dyn_array_t data;
} cache_t;

typedef struct group_cache
{
    bool initialized;

    size_t group_size;

    // How many entries are already locally populated
    size_t num_local_entries;

    // Number of ranks on the local host
    size_t n_local_ranks;

    // Number of ranks on the local host that we already know about
    size_t n_local_ranks_populated;

    // Array with all the ranks in the group (type: peer_cache_entry_t)
    dyn_array_t ranks;
} group_cache_t;

#define GET_GROUP_CACHE(_cache, _gp_id) ({                                                  \
    group_cache_t *_gp_cache = DYN_ARRAY_GET_ELT(&((_cache)->data), _gp_id, group_cache_t); \
    _gp_cache;                                                                              \
})

/**
 * @brief GET_GROUP_RANK_CACHE_ENTRY is a macro that looks up the cache entry for
 * a given rank in a group. Also, group caches are populated in a lazy way since
 * we only get data on the DPU when ranks are connecting. So the macro is also
 * in charge is initialization the cache if the group and the rank entry do not
 * exist. Of course, I means that the data passed in is assumed accurate, i.e.,
 * the group identifier, rank and group size are the actual value and won't change.
 */
#define GET_GROUP_RANK_CACHE_ENTRY(_cache, _gp_id, _rank, _gp_size)                             \
    ({                                                                                          \
        peer_cache_entry_t *_entry = NULL;                                                      \
        group_cache_t *_gp_cache = DYN_ARRAY_GET_ELT(&((_cache)->data), _gp_id, group_cache_t); \
        dyn_array_t *_rank_cache = &(_gp_cache->ranks);                                         \
        if (_gp_cache->initialized == false)                                                    \
        {                                                                                       \
            /* Cache for the group is empty, lazy initialization */                             \
            DYN_ARRAY_ALLOC(_rank_cache, DEFAULT_NUM_PEERS, peer_cache_entry_t);                \
            _gp_cache->initialized = true;                                                      \
            if (_gp_size >= 0)                                                                  \
                _gp_cache->group_size = _gp_size;                                               \
            (_cache)->size++;                                                                   \
        }                                                                                       \
        if (_gp_cache->initialized &&                                                           \
            _gp_cache->group_size <= 0 &&                                                       \
            _gp_size >= 0)                                                                      \
        {                                                                                       \
            /* the cache was initialized with a group size but we now know it */                \
            _gp_cache->group_size = _gp_size;                                                   \
        }                                                                                       \
        _entry = DYN_ARRAY_GET_ELT(_rank_cache, _rank, peer_cache_entry_t);                     \
        _entry;                                                                                 \
    })

struct remote_service_proc_info; // Forward declaration

typedef struct remote_dpu_connect_tracker
{
    struct remote_service_proc_info *remote_service_proc_info;
    execution_context_t *client_econtext;
} remote_service_procs_connect_tracker_t;

// Forward declaration
struct offloading_config;

typedef struct offloading_engine
{

    pthread_mutex_t mutex;
    int done;

    // All the configuration details associated to the engine.
    // In most cases, this is the data from the configuration file.
    struct offloading_config *config;

    /* client here is used to track the bootstrapping as a client. */
    /* it can only be at most one (the offload engine bootstraps only once */
    /* for both host process and the DPU daemon) */
    execution_context_t *client;

    /* we can have as many servers as we want, each server having multiple clients */
    size_t num_max_servers;
    size_t num_servers;
    execution_context_t **servers;

    // Execution context for communications and notifications to self
    execution_context_t *self_econtext;

    // Engine's worker
    ucp_worker_h ucp_worker;

    // ucp_worker_allocated tracks whether the library allocated the worker. If so, it is freed when the engine is being finalized.
    bool ucp_worker_allocated;

    // Engine's UCP context
    ucp_context_h ucp_context;

    // ucp_context_allocated tracks whether the library allocated the UCP context. If so, it is freed when the engine is being finalized.
    bool ucp_context_allocated;

    // Self endpoint
    ucp_ep_h self_ep;

    /* we track the clients used for inter-service-processes connection separately. */
    /* Servers are at the moment in the servers list. */
    size_t num_inter_service_proc_clients;
    size_t num_max_inter_service_proc_clients;
    remote_service_procs_connect_tracker_t *inter_service_proc_clients;

    /* Vector of registered operation, ready for execution */
    size_t num_registered_ops;
    offload_op_t *registered_ops;
    dyn_list_t *free_op_descs;

    /* Cache for groups/rank so we can propagate rank and DPU related data */
    cache_t procs_cache;
    dyn_list_t *free_cache_entry_requests; // pool of descriptors to issue asynchronous cache updates (type: cache_entry_request_t)

    /* Objects used during wire-up */
    dyn_list_t *pool_conn_params;

    /* Pool of remote_dpu_info_t structures, used when getting the configuration */
    dyn_list_t *pool_remote_dpu_info;

    // Flag to specify if we are on the DPU or not
    bool on_dpu;

    // dpus is a vector of remote_dpu_info_t structures used on the DPUs
    // to easily track all the DPUs used in the current configuration.
    // This is at the moment not used on the host. Type: remote_dpu_info_t*
    dyn_array_t dpus;

    // service_procs is a vector of remote_service_proc_info_t structures
    // used on the DPUs to easily track all the remote services processes
    // that we will have a connection to.
    // This is at the moment not used on the host. Type: remote_service_proc_info_t
    dyn_array_t service_procs;

    // Number of DPUs defined in dpus
    size_t num_dpus;

    // Number of services processses running on the DPUs
    size_t num_service_procs;

    // Number of service processes with which a connection is established (both as server and client).
    size_t num_connected_service_procs;

    // List of default notifications that are applied to all new execution contexts added to the engine.
    dpu_offload_ev_sys_t *default_notifications;

    // Current number of default notifications that have been registered
    size_t num_default_notifications;

    // Smart buffer system associated to the engine
    smart_buffers_t smart_buffer_sys;
} offloading_engine_t;

#define RESET_ENGINE(_engine, _ret)                                                                                 \
    do                                                                                                              \
    {                                                                                                               \
        _ret = 0;                                                                                                   \
        (_engine)->mutex = (pthread_mutex_t)PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;                                \
        (_engine)->done = false;                                                                                    \
        (_engine)->config = NULL;                                                                                   \
        (_engine)->client = NULL;                                                                                   \
        (_engine)->num_max_servers = DEFAULT_MAX_NUM_SERVERS;                                                       \
        (_engine)->num_servers = 0;                                                                                 \
        (_engine)->servers = DPU_OFFLOAD_MALLOC((_engine)->num_max_servers * sizeof(dpu_offload_server_t *));       \
        if ((_engine)->servers == NULL)                                                                             \
            break;                                                                                                  \
        memset((_engine)->servers, 0, (_engine)->num_max_servers * sizeof(dpu_offload_server_t *));                 \
        (_engine)->self_econtext = NULL;                                                                            \
        (_engine)->ucp_worker = NULL;                                                                               \
        (_engine)->ucp_worker_allocated = false;                                                                    \
        (_engine)->ucp_context = NULL;                                                                              \
        (_engine)->ucp_context_allocated = false;                                                                   \
        (_engine)->self_ep = NULL;                                                                                  \
        (_engine)->num_inter_service_proc_clients = 0;                                                              \
        (_engine)->num_max_inter_service_proc_clients = DEFAULT_MAX_NUM_SERVERS;                                    \
        (_engine)->inter_service_proc_clients = DPU_OFFLOAD_MALLOC((_engine)->num_max_inter_service_proc_clients *  \
                                                                   sizeof(remote_service_procs_connect_tracker_t)); \
        if ((_engine)->inter_service_proc_clients == NULL)                                                          \
            break;                                                                                                  \
        (_engine)->num_registered_ops = 0;                                                                          \
        DYN_LIST_ALLOC((_engine)->free_op_descs, 8, op_desc_t, item);                                               \
        if ((_engine)->registered_ops == NULL)                                                                      \
            break;                                                                                                  \
        GROUPS_CACHE_INIT(&((_engine)->procs_cache));                                                               \
        DYN_LIST_ALLOC((_engine)->free_cache_entry_requests, DEFAULT_NUM_PEERS, cache_entry_request_t, item);       \
        if ((_engine)->free_cache_entry_requests == NULL)                                                           \
            break;                                                                                                  \
        DYN_LIST_ALLOC((_engine)->pool_conn_params, 32, conn_params_t, item);                                       \
        if ((_engine)->pool_conn_params == NULL)                                                                    \
            break;                                                                                                  \
        DYN_LIST_ALLOC_WITH_INIT_CALLBACK((_engine)->pool_remote_dpu_info,                                          \
                                          32,                                                                       \
                                          remote_dpu_info_t,                                                        \
                                          item,                                                                     \
                                          init_remote_dpu_info_struct);                                             \
        if ((_engine)->pool_remote_dpu_info == NULL)                                                                \
            break;                                                                                                  \
        (_engine)->on_dpu = false;                                                                                  \
        /* Note that engine->dpus is a vector of remote_dpu_info_t pointers. */                                     \
        /* The actual object are from pool_remote_dpu_info */                                                       \
        DYN_ARRAY_ALLOC(&((_engine)->dpus), 32, remote_dpu_info_t *);                                               \
        DYN_ARRAY_ALLOC(&((_engine)->service_procs), 256, remote_service_proc_info_t);                              \
        (_engine)->num_dpus = 0;                                                                                    \
        (_engine)->num_service_procs = 0;                                                                           \
        (_engine)->num_connected_service_procs = 0;                                                                 \
        (_engine)->default_notifications = NULL;                                                                    \
        (_engine)->num_default_notifications = 0;                                                                   \
        SMART_BUFFS_INIT(&((_engine)->smart_buffer_sys), NULL);                                                     \
    } while (0)

/**
 * @brief Data structure used when parsing a configuration file.
 */
typedef struct dpu_config_data
{
    union
    {
        struct
        {
            char *hostname;
            char *addr;
            size_t num_host_ports;
            dyn_array_t host_ports; // Type: int
            size_t num_interdpu_ports;
            dyn_array_t interdpu_ports; // Type: int
        } version_1;
    };
} dpu_config_data_t;

typedef struct service_proc_config_data
{
    union
    {
        struct
        {
            char *hostname;
            char *addr;
            int host_port;
            int intersp_port;
        } version_1;
    };
} service_proc_config_data_t;

/**
 * @brief remote_dpu_info_t gathers all the data necessary to track and connect to other DPUs.
 */
typedef struct remote_dpu_info
{
    ucs_list_link_t item;

    // idx is the index in the engine's list of known DPUs
    size_t idx;

    // DPU's hostname
    char *hostname;

    dpu_config_data_t *config;

    // engine associated to the remote DPU
    offloading_engine_t *engine;

    // Array of service processes running on the remote DPU
    // Type: remote_service_proc_info_t
    ucs_list_link_t remote_service_procs;
} remote_dpu_info_t;

#define RESET_REMOTE_DPU_INFO(_info)                          \
    do                                                        \
    {                                                         \
        (_info)->idx = 0;                                     \
        (_info)->hostname = NULL;                             \
        (_info)->engine = NULL;                               \
        ucs_list_head_init(&((_info)->remote_service_procs)); \
    } while (0)

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
    // Optional info associated to the entry
    notification_info_t info;
} notification_callback_entry_t;

#define RESET_NOTIF_CB_ENTRY(_entry)         \
    do                                       \
    {                                        \
        (_entry)->set = false;               \
        (_entry)->ev_sys = NULL;             \
        (_entry)->cb = NULL;                 \
        RESET_NOTIF_INFO(&((_entry)->info)); \
    } while (0)

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

#define RESET_PENDING_NOTIF(_notif) \
    do                              \
    {                               \
        (_notif)->type = 0;         \
        (_notif)->src_id = 0;       \
        (_notif)->header = NULL;    \
        (_notif)->header_size = 0;  \
        (_notif)->data = NULL;      \
        (_notif)->data_size = 0;    \
        (_notif)->econtext = NULL;  \
    } while (0)

/***********************************/
/* DPU/SERVICE PROCS CONFIGURATION */
/***********************************/

/**
 * @brief LIST_DPUS_FROM_ENGINE returns the pointer to the array of remote_dpu_info_t structures
 * representing the list of known DPU, based on a engine. This is relevant mainly on DPUs
 *
 * @parma[in] engine
 */
#define LIST_DPUS_FROM_ENGINE(_engine) ({                   \
    remote_dpu_info_t **_list = NULL;                       \
    if (_engine)                                            \
    {                                                       \
        _list = (remote_dpu_info_t **)(_engine)->dpus.base; \
    }                                                       \
    _list;                                                  \
})

/**
 * @brief LIST_DPUS_FROM_ENGINE returns the pointer to the array of remote_dpu_info_t structures
 * representing the list of known DPU, based on a execution context. This is relevant mainly on DPUs.
 *
 * @parma[in] econtext
 */
#define LIST_DPUS_FROM_ECONTEXT(_econtext) ({               \
    remote_dpu_info_t **_list = NULL;                       \
    if (_econtext != NULL)                                  \
    {                                                       \
        _list = LIST_DPUS_FROM_ENGINE((_econtext)->engine); \
    }                                                       \
    _list;                                                  \
})

#define ECONTEXT_FOR_SERVICE_PROC_COMMUNICATION(_engine, _service_proc_idx) ({ \
    execution_context_t *_e = NULL;                                            \
    remote_service_proc_info_t *_sp;                                           \
    _sp = DYN_ARRAY_GET_ELT(&((_engine)->service_procs),                       \
                            _service_proc_idx,                                 \
                            remote_service_proc_info_t);                       \
    if (_sp != NULL)                                                           \
    {                                                                          \
        _e = _sp->econtext;                                                    \
    }                                                                          \
    _e;                                                                        \
})

#define GET_REMOTE_SERVICE_PROC_ECONTEXT(_engine, _service_proc_idx) ({                \
    execution_context_t *_e = NULL;                                                    \
    if ((_service_proc_idx) <= (_engine)->num_connected_service_procs)                 \
    {                                                                                  \
        remote_service_proc_info_t *_sp;                                               \
        _sp = DYN_ARRAY_GET_ELT(&((_engine)->service_procs),                           \
                                _service_proc_idx,                                     \
                                remote_service_proc_info_t);                           \
        if (_sp->econtext == NULL &&                                                   \
            _service_proc_idx == (_engine)->config->local_service_proc.info.global_id) \
        {                                                                              \
            _sp->econtext = (_engine)->self_econtext;                                  \
        }                                                                              \
        if (_sp->econtext != NULL)                                                     \
        {                                                                              \
            _e = _sp->econtext;                                                        \
        }                                                                              \
    }                                                                                  \
    _e;                                                                                \
})

#define GET_REMOTE_SERVICE_PROC_EP(_engine, _idx) ({                       \
    ucp_ep_h __ep = NULL;                                                  \
    if (_idx <= (_engine)->num_connected_service_procs)                    \
    {                                                                      \
        remote_service_proc_info_t *_sp;                                   \
        _sp = DYN_ARRAY_GET_ELT(&((_engine)->service_procs),               \
                                _idx,                                      \
                                remote_service_proc_info_t);               \
        assert(_sp);                                                       \
        if (_sp->ep != NULL)                                               \
        {                                                                  \
            __ep = _sp->ep;                                                \
        }                                                                  \
        else                                                               \
        {                                                                  \
            if (_sp->peer_addr != NULL)                                    \
            {                                                              \
                /* Generate the endpoint with the data we have */          \
                ucp_ep_params_t _ep_params;                                \
                _ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS; \
                _ep_params.address = _sp->peer_addr;                       \
                ucs_status_t status = ucp_ep_create((_engine)->ucp_worker, \
                                                    &_ep_params,           \
                                                    &(_sp->ep));           \
                CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR,             \
                                 "ucp_ep_create() failed: %s",             \
                                 ucs_status_string(status));               \
                assert(_sp->ep);                                           \
                __ep = _sp->ep;                                            \
            }                                                              \
        }                                                                  \
    }                                                                      \
    __ep;                                                                  \
})

typedef enum
{
    CONNECT_STATUS_UNKNOWN = 0,
    CONNECT_STATUS_CONNECTED,
    CONNECT_STATUS_DISCONNECTED
} connect_status_t;

typedef struct service_proc
{
    // Identifier of the DPU where the service process is running
    uint64_t dpu;

    // local unique identifier based on how many service processes are running on
    // the DPU
    uint64_t local_id;

    //
    char *local_id_str;

    // global unique identifier based on how many service processes are available
    // for offloading
    uint64_t global_id;

    // String version of global_id, used to point at environment variable
    char *global_id_str;
} service_proc_t;

#define RESET_SERVICE_PROC(_p)        \
    do                                \
    {                                 \
        (_p)->dpu = UINT64_MAX;       \
        (_p)->local_id = UINT64_MAX;  \
        (_p)->local_id_str = NULL;    \
        (_p)->global_id = UINT64_MAX; \
        (_p)->global_id_str = NULL;   \
    } while (0)

/**
 * @brief Datatstructure representing a remote service process that is running on
 * a specific DPU. A service process is identified by both its local unique identifier
 * based on how many service processes are running on the DPU and a global unique
 * identifier based on how many service processes are available for offloading.
 */
typedef struct remote_service_proc_info
{
    ucs_list_link_t item;

    // Index in the array of known service processes
    size_t idx;

    service_proc_config_data_t *config;

    // Associated physical DPU
    remote_dpu_info_t *dpu;

    // Traget service process
    service_proc_t service_proc;

    // Associated client_id when applicable
    uint64_t client_id;

    // Pointer to the address. Used for example to create new endpoint
    void *peer_addr;

    // Initialization paramaters for bootstrapping
    init_params_t init_params;

    // Connection parameters for bootstrapping
    connect_status_t conn_status;

    // identifier of the connection thread
    pthread_t connection_tid;

    // Associated offloading engine
    offloading_engine_t *offload_engine;

    // Execution context to communication with it
    execution_context_t *econtext;

    // Worker to use to communicate with the DPU
    ucp_worker_h ucp_worker;

    // Pointer to the endpoint to communicate with the DPU
    ucp_ep_h ep;
} remote_service_proc_info_t;

#define RESET_REMOTE_SERVICE_PROC(_info)                    \
    do                                                      \
    {                                                       \
        (_info)->idx = UINT64_MAX;                          \
        (_info)->config = NULL;                             \
        RESET_REMOTE_DPU_INFO(&((_info).dpu));              \
        RESET_SERVICE_PROC(&((_info).service_proc));        \
        (_info)->client_id = UINT64_MAX;                    \
        (_info)->peer_addr = NULL;                          \
        RESET_INIT_PARAMS(&((_info)->init_params));         \
        (_info)->conn_status = CONNECT_STATUS_DISCONNECTED; \
        (_info)->offload_engine = NULL;                     \
        (_info)->econtext = NULL;                           \
        (_info)->ucp_worker = NULL;                         \
        (_info)->ep = NULL;                                 \
        (_info)->client_id = UINT64_MAX;                    \
    } while (0)

/**
 * @brief While the remote_service_proc_info_t structure is ready to be used
 * as a list item, list items can only be on one list at a time and we need
 * to track both the service processes per DPU, as well as the service processes
 * to connect to. To avoid any problem, connect_to_service_proc is a container
 * to be used with the list of service procs to connect to that holds a pointer
 * to the service process structure that belongs to a remote_dpu_info_t element.
 */
typedef struct connect_to_service_proc
{
    ucs_list_link_t item;
    remote_service_proc_info_t *sp;
} connect_to_service_proc_t;

typedef struct service_proc_inter_connect_info
{
    dyn_list_t *pool_remote_sp_connect_to; // Pool of connect_to_service_proc_t structures
    ucs_list_link_t sps_connect_to;        // List of connect_to_service_proc_t structures
    ucs_list_link_t dpus_connect_to;       // List of  structures

    // Number of physical DPUs to connect to
    size_t num_dpus;

    // Total number of service processes to connect to.
    size_t num_connect_to;
} service_procs_inter_connect_info_t;

#define RESET_INFO_CONNECTING_TO(_i)                                                           \
    do                                                                                         \
    {                                                                                          \
        ucs_list_head_init(&((_i)->dpus_connect_to));                                          \
        ucs_list_head_init(&((_i)->sps_connect_to));                                           \
        DYN_LIST_ALLOC((_i)->pool_remote_sp_connect_to, 256, connect_to_service_proc_t, item); \
        (_i)->num_dpus = 0;                                                                    \
        (_i)->num_connect_to = 0;                                                              \
    } while (0)

/**
 * @brief offloading_config_t represents the configuration that will be used by the offloading infrastructure.
 * It reflects for instance the content of the configuration when a configuration file is used.
 *
 */
typedef struct offloading_config
{
    // List of DPUs to be used during the execution of the job. Usually from a environment variable.
    char *list_dpus;

    // Path to the configuration file (NULL when no configuration file is used).
    char *config_file;

    // String pointing at environment variable that specifies the number of
    // service processes per DPU
    char *num_service_procs_per_dpu_str;

    // Number of service processes per DPU, assumed to be the same on all DPUs
    uint64_t num_service_procs_per_dpu;

    // Version of the format of the configuration file when config_file is not NULL.
    int format_version;

    // Associated offloading engine.
    offloading_engine_t *offloading_engine;

    // Specifies whether the local DPU has been found in the configuration
    bool service_proc_found;

    // Number of service processes that are connecting to the current service proc
    // (only valid on DPUs), used for inter-service-processes connections
    size_t num_connecting_service_procs;

    // Data related to the service processes that the infrastructure needs to connect to (only valid on DPUs)
    service_procs_inter_connect_info_t info_connecting_to;

    // Total number of DPUs to be used
    size_t num_dpus;

    // Total number of service processes to be used
    size_t num_service_procs;

    // Configuration of all the DPUs (type: dpu_config_data_t)
    // This is where the DPUs' data is initially stored and then used to help
    // populate the list of DPUs maintained at the engine level.
    dyn_array_t dpus_config;

    // Configuration of all the service processes (type: service_proc_config_data_t)
    dyn_array_t sps_configs;

    // Configuration of the local DPU (only valid on DPUs)
    struct
    {
        service_proc_t info;
        service_proc_config_data_t *config;
        char hostname[1024];

        // Connection parameters for the service processes to connect to each other
        conn_params_t inter_service_procs_conn_params;

        // Connection parameters for processes on the host connect to the service process
        conn_params_t host_conn_params;

        // Parameters used to connect to other service processes, or other service processes connect to the current service process
        init_params_t inter_service_procs_init_params;

        // Parameters used to let the host connect to the service process
        init_params_t host_init_params;
    } local_service_proc;
} offloading_config_t;

#define INIT_DPU_CONFIG_DATA(_data)                                                                                 \
    do                                                                                                              \
    {                                                                                                               \
        (_data)->list_dpus = NULL;                                                                                  \
        (_data)->config_file = NULL;                                                                                \
        (_data)->num_service_procs_per_dpu_str = NULL;                                                              \
        (_data)->num_service_procs_per_dpu = 0;                                                                     \
        (_data)->format_version = 0;                                                                                \
        (_data)->offloading_engine = NULL;                                                                          \
        (_data)->service_proc_found = false;                                                                        \
        (_data)->num_connecting_service_procs = 0;                                                                  \
        RESET_INFO_CONNECTING_TO(&((_data)->info_connecting_to));                                                   \
        (_data)->num_dpus = 0;                                                                                      \
        (_data)->num_service_procs = 0;                                                                             \
        DYN_ARRAY_ALLOC(&((_data)->dpus_config), 32, dpu_config_data_t);                                            \
        DYN_ARRAY_ALLOC(&((_data)->sps_configs), 256, service_proc_config_data_t);                                  \
        RESET_SERVICE_PROC(&((_data)->local_service_proc.info));                                                    \
        (_data)->local_service_proc.config = NULL;                                                                  \
        (_data)->local_service_proc.hostname[1023] = '\0';                                                          \
        RESET_INIT_PARAMS(&((_data)->local_service_proc.host_init_params));                                         \
        RESET_INIT_PARAMS(&((_data)->local_service_proc.inter_service_procs_init_params));                          \
        RESET_CONN_PARAMS(&((_data)->local_service_proc.host_conn_params));                                         \
        RESET_CONN_PARAMS(&((_data)->local_service_proc.inter_service_procs_conn_params));                          \
        (_data)->local_service_proc.inter_service_procs_init_params.scope_id = SCOPE_INTER_SERVICE_PROCS;           \
        (_data)->local_service_proc.host_init_params.conn_params = &((_data)->local_service_proc.host_conn_params); \
        (_data)->local_service_proc.inter_service_procs_init_params.conn_params =                                   \
            &((_data)->local_service_proc.inter_service_procs_conn_params);                                         \
    } while (0)

/**
 * @brief Get the DPU config object based on the content of the configuration file.
 *
 * @param[in] offloading_engine The offloading engine to configure with the configuration file.
 * @param[in/out] dpu_config Configuration details for all the DPUs from the configuration file.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_dpu_config(offloading_engine_t *, offloading_config_t *);

dpu_offload_status_t get_host_config(offloading_config_t *);
dpu_offload_status_t find_dpu_config_from_platform_configfile(char *, offloading_config_t *);
dpu_offload_status_t find_config_from_platform_configfile(char *, char *, offloading_config_t *);

/**********************/
/* ACTIVE MESSAGE IDS */
/**********************/

typedef enum
{
    META_EVENT_TYPE = 32, // 32 to make it easier to see corruptions (dbg)
    AM_TERM_MSG_ID = 33,
    AM_EVENT_MSG_ID,
    AM_EVENT_MSG_HDR_ID, // 35
    AM_OP_START_MSG_ID,
    AM_OP_COMPLETION_MSG_ID,
    AM_XGVMI_ADD_MSG_ID,
    AM_XGVMI_DEL_MSG_ID,
    AM_PEER_CACHE_REQ_MSG_ID, // 40
    AM_PEER_CACHE_ENTRIES_MSG_ID,
    AM_PEER_CACHE_ENTRIES_REQUEST_MSG_ID,
    AM_ADD_GP_RANK_MSG_ID,
    AM_TEST_MSG_ID
} am_id_t;

_EXTERN_C_END

#endif // DPU_OFFLOAD_TYPES_H
