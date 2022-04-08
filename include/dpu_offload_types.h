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

#define INIT_UCX() ({                                                      \
    ucp_params_t ucp_params;                                               \
    ucs_status_t status;                                                   \
    ucp_config_t *config;                                                  \
    ucp_context_h ucp_context = NULL;                                      \
    memset(&ucp_params, 0, sizeof(ucp_params));                            \
    status = ucp_config_read(NULL, NULL, &config);                         \
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;                      \
    ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_AM;                \
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

#define ECONTEXT_ID(_exec_ctx) ({            \
    uint64_t _my_id;                         \
    if ((_exec_ctx)->type == CONTEXT_CLIENT) \
    {                                        \
        _my_id = (_exec_ctx)->client->id;    \
    }                                        \
    else                                     \
    {                                        \
        _my_id = (_exec_ctx)->server->id;    \
    }                                        \
    _my_id;                                  \
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

#define GET_CLIENT_EP(_exec_ctx, _client_id) ({                              \
    ucp_ep_h _ep;                                                            \
    if ((_exec_ctx)->type == CONTEXT_SERVER)                                 \
    {                                                                        \
        peer_info_t *_pi;                                                    \
        DYN_ARRAY_GET_ELT(&((_exec_ctx)->server->connected_clients.clients), \
                          _client_id,                                        \
                          peer_info_t,                                       \
                          _pi);                                              \
        assert(_pi);                                                         \
        _ep = _pi->ep;                                                       \
    }                                                                        \
    else                                                                     \
    {                                                                        \
        _ep = NULL;                                                          \
    }                                                                        \
    _ep;                                                                     \
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
    dpu_offload_ev_sys_t *_sys;                     \
    if ((_exec_ctx)->type == CONTEXT_CLIENT)        \
    {                                               \
        _sys = (_exec_ctx)->client->event_channels; \
    }                                               \
    else                                            \
    {                                               \
        _sys = (_exec_ctx)->server->event_channels; \
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
    int64_t group_id;
    int64_t group_rank;
} rank_info_t;

// fixme: long term, we do not want to have a limit on the length of the address
// but this will require a new smart way to manage the memory used by cache entries
// and avoid expensive copies when exchanging cache entries between DPUs and
// application processes
#define MAX_ADDR_LEN (2048)

#define MAX_SHADOW_DPUS (8)

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

    // Is the entry set?
    bool set;

    // Peer data (group/rank)
    peer_data_t peer;

    // endpoint to reach the peer
    ucp_ep_h ep;

    // Number of  the peer's shadow DPU(s)
    size_t num_shadow_dpus;

    // List of DPUs' unique ID that are the shadow DPU(s) of the peer
    uint64_t shadow_dpus[MAX_SHADOW_DPUS]; // Array of DPUs (when applicable)

    // Is the list of events already initialized or not (lazy initialization)
    bool events_initialized;

    // List of events to complete when any update is made to the entry
    ucs_list_link_t events;
} peer_cache_entry_t;

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
} am_header_t; // todo: rename, nothing to do with AM

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
} am_req_t; // todo: rename, nothing to do with AM

#if !USE_AM_IMPLEM
// Forward declaration
struct execution_context;

typedef struct payload_notif_req
{
    bool complete;
    ucp_request_param_t recv_params;
    void *buffer;
    struct ucx_context *req;
} payload_notif_req_t;

typedef struct hdr_notif_req
{
    bool complete;
    am_header_t hdr;
    struct ucx_context *req;
    uint64_t recv_peer_id;
    struct execution_context *econtext;
    payload_notif_req_t payload_ctx;
} hdr_notif_req_t;

typedef struct notif_reception
{
    bool initialized;
    hdr_notif_req_t ctx;
    ucp_request_param_t hdr_recv_params;
    ucp_tag_t hdr_ucp_tag;
    ucp_tag_t hdr_ucp_tag_mask;
    struct ucx_context *req;
} notif_reception_t;
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

typedef struct peer_info
{
    bootstrapping_t bootstrapping;

#if !USE_AM_IMPLEM
    notif_reception_t notif_recv;
#endif

    // Length of the peer's address
    size_t peer_addr_len;

    // Peer's address. Used to create endpoint when using OOB
    void *peer_addr;

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

    /* pending notifications are notifications that cannot be delivered upon reception because the callback is not registered yet */
    ucs_list_link_t pending_notifications;

    // free_pending_notifications is a pool oof pending notification objects that can be used when a notification is received and
    // no callback is registered yet. It avoids allocating memory.
    dyn_list_t *free_pending_notifications;

    // Array of callback functions, i.e., array of pointers, organized based on the notification type, a.k.a. notification ID
    dyn_array_t notification_callbacks;

    // Number of remote events sent but not completed yet
    size_t num_pending_sends;

    // Maximum number of pending emit operations (in progress emits)
    size_t max_pending_emits;

    // List of pending emits
    ucs_list_link_t pending_emits;

    // Execution context the event system is associated with.
    struct execution_context *econtext;

#if !USE_AM_IMPLEM
    notif_reception_t notif_recv;
#endif
} dpu_offload_ev_sys_t;

typedef struct connected_clients
{
    size_t num_max_connected_clients;
    size_t num_connected_clients;
    size_t num_ongoing_connections;
    // Dynamic array of structures to track connected clients (type: peer_info_t)
    dyn_array_t clients;
} connected_clients_t;

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

    // Peer's ID
    uint64_t peer_id;
} connected_peer_data_t;

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
    SCOPE_INTER_DPU,
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
        if ((_econtext)->type == CONTEXT_CLIENT)   \
            CLIENT_LOCK((_econtext)->client);      \
        else                                       \
            SERVER_LOCK((_econtext)->server);      \
    } while (0)

#define ECONTEXT_UNLOCK(_econtext)                   \
    do                                               \
    {                                                \
        pthread_mutex_unlock(&((_econtext)->mutex)); \
        if ((_econtext)->type == CONTEXT_CLIENT)     \
            CLIENT_UNLOCK((_econtext)->client);      \
        else                                         \
            SERVER_UNLOCK((_econtext)->server);      \
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
    bootstrapping_t bootstrapping;

    uint64_t id; // Identifier assigned by server

    uint64_t server_id; // Unique identifier of the server

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

typedef void (*execution_context_progress_fn)(struct execution_context *);

struct offloading_engine; // forward declaration

#define ECONTEXT_ON_DPU(_ctx) ((_ctx)->engine->on_dpu)

typedef enum
{
    BOOTSTRAP_NOT_INITIATED = 0,
    OOB_CONNECT_DONE,
    UCX_CONNECT_DONE,
    BOOTSTRAP_DONE,
} bootstrap_phase_t;

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
    int type;

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

    /* Active operations that are running in the execution context */
    ucs_list_link_t active_ops;

    // During bootstrapping, the execution context acts either as a client or server.
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

#if !USE_AM_IMPLEM
    struct ucx_context *hdr_request;
    struct ucx_context *payload_request;
#endif

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

    // Specifies whether the payload buffer needs to be managed by the library.
    // If so, it uses the payload_size from the infro structure used when getting
    // the event to allocate/get the buffer and associate it to the event.
    bool manage_payload_buf;

    // payload buffer when the library manages it. Its size is stored in the header object.
    void *payload;

    // Destination endpoint for remote events
    ucp_ep_h dest_ep;

    // Specifies whether the event was pending or not
    bool was_pending;

    // event_system is the event system the event was initially from
    dpu_offload_ev_sys_t *event_system;
} dpu_offload_event_t;

/**
 * @brief RESET_EVENT does not reinitialize sub_events_initialized because if is done
 * only once and reuse as events are reused. However, it is initialized when the
 * dynamic list is initialized
 */
#define RESET_EVENT(__ev)                   \
    do                                      \
    {                                       \
        (__ev)->context = NULL;             \
        (__ev)->payload = NULL;             \
        (__ev)->event_system = NULL;        \
        (__ev)->req = NULL;                 \
        (__ev)->ctx.complete = 0;           \
        (__ev)->ctx.hdr.type = 0;           \
        (__ev)->ctx.hdr.id = 0;             \
        (__ev)->ctx.hdr.payload_size = 0;   \
        (__ev)->manage_payload_buf = false; \
        (__ev)->dest_ep = NULL;             \
        (__ev)->was_pending = false;        \
    } while (0)

#define CHECK_EVENT(__ev)                                     \
    do                                                        \
    {                                                         \
        assert((__ev)->ctx.complete == 0);                    \
        assert((__ev)->ctx.hdr.payload_size == 0);            \
        assert((__ev)->ctx.hdr.type == 0);                    \
        assert((__ev)->ctx.hdr.id == 0);                      \
        assert((__ev)->manage_payload_buf == false);          \
        assert((__ev)->dest_ep == NULL);                      \
        assert((__ev)->was_pending == false);                 \
        if ((__ev)->sub_events_initialized)                   \
        {                                                     \
            assert(ucs_list_is_empty(&((__ev)->sub_events))); \
        }                                                     \
    } while (0)

typedef struct dpu_offload_event_info
{
    // Size of the payload that the library needs to be managing. If 0 not payload needs to be managed
    size_t payload_size;
} dpu_offload_event_info_t;

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
    size_t size; // not used at the moment

    /* data is a dynamic array for all the groups */
    dyn_array_t data;
} cache_t;

typedef struct group_cache
{
    bool initialized;
    dyn_array_t ranks;
} group_cache_t;

#define GET_GROUP_RANK_CACHE_ENTRY(_cache, _gp_id, _rank)                        \
    ({                                                                           \
        peer_cache_entry_t *_entry = NULL;                                       \
        group_cache_t *_gp_cache = (group_cache_t *)(_cache)->data.base;         \
        dyn_array_t *_rank_cache = &(_gp_cache[_gp_id].ranks);                   \
        if (_gp_cache[_gp_id].initialized == false)                              \
        {                                                                        \
            /* Cache for the group is empty */                                   \
            DYN_ARRAY_ALLOC(_rank_cache, DEFAULT_NUM_PEERS, peer_cache_entry_t); \
            _gp_cache[_gp_id].initialized = true;                                \
            (_cache)->size++;                                                    \
        }                                                                        \
        if (_rank >= _rank_cache->num_elts)                                      \
            DYN_ARRAY_GROW(_rank_cache, peer_cache_entry_t, _rank);              \
        assert(_rank < _rank_cache->num_elts);                                   \
        peer_cache_entry_t *_ptr = (peer_cache_entry_t *)_rank_cache->base;      \
        _entry = &(_ptr[_rank]);                                                 \
        _entry;                                                                  \
    })

#define SET_GROUP_RANK_CACHE_ENTRY(__econtext, __gp_id, __rank)                                        \
    ({                                                                                                 \
        peer_cache_entry_t *__entry = GET_GROUP_RANK_CACHE_ENTRY(&((__econtext)->engine->procs_cache), \
                                                                 __gp_id, __rank);                     \
        if (__entry != NULL)                                                                           \
        {                                                                                              \
            __entry->peer.proc_info.group_id = __gp_id;                                                \
            __entry->peer.proc_info.group_rank = __rank;                                               \
            __entry->num_shadow_dpus = 1; /* fixme: find a way to get the DPUs config from here */     \
            __entry->shadow_dpus[0] = ECONTEXT_ID(__econtext);                                         \
            __entry->set = true;                                                                       \
        }                                                                                              \
        __entry;                                                                                       \
    })

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

#define SET_PEER_CACHE_ENTRY(_peer_cache, _entry)                                          \
    ({                                                                                     \
        int64_t _gp_id = (_entry)->peer.proc_info.group_id;                                \
        int64_t _rank = (_entry)->peer.proc_info.group_rank;                               \
        peer_cache_entry_t *_ptr = GET_GROUP_RANK_CACHE_ENTRY(_peer_cache, _gp_id, _rank); \
        if (_ptr != NULL)                                                                  \
        {                                                                                  \
            _ptr->set = true;                                                              \
            memcpy(&(_ptr->peer), &((_entry)->peer), sizeof(peer_data_t));                 \
            _ptr->ep = NULL;                                                               \
            _ptr->num_shadow_dpus = (_entry)->num_shadow_dpus;                             \
            size_t _i;                                                                     \
            for (_i = 0; _i < (_entry)->num_shadow_dpus; _i++)                             \
                _ptr->shadow_dpus[_i] = (_entry)->shadow_dpus[_i];                         \
        }                                                                                  \
        _ptr;                                                                              \
    })

struct remote_dpu_info; // Forward declaration

typedef struct remote_dpu_connect_tracker
{
    struct remote_dpu_info *remote_dpu_info;
    execution_context_t *client_econtext;
} remote_dpu_connect_tracker_t;

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

    // On DPU to simply communications with other DPUs, we set a default execution context
    // so we can always easily get events
    execution_context_t *default_econtext;

    // Engine's worker
    ucp_worker_h ucp_worker;

    // Engine's UCP context
    ucp_context_h ucp_context;

    // Self endpoint
    ucp_ep_h self_ep;

    /* we track the clients used for inter-DPU connection separately. Servers are at the */
    /* moment in the servers list. */
    size_t num_inter_dpus_clients;
    size_t num_max_inter_dpus_clients;
    remote_dpu_connect_tracker_t *inter_dpus_clients;

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

    // Flag to specify if we are on the DPU or not
    bool on_dpu;

    // dpus is a vector of remote_dpu_info_t structures used on the DPUs
    // to easily track all the DPUs the execution context is connected to.
    // This is at the moment not used on the host.
    dyn_array_t dpus;

    // Number of DPUs defined in dpus
    size_t num_dpus;

    // Number of DPUs we are connected to.
    // Note that ATM it only account for the number of DPUs that the current DPU connects to,
    // not the DPUs connecting to it.
    size_t num_connected_dpus;

    // List of default notifications that are applied to all new execution contexts added to the engine.
    dpu_offload_ev_sys_t *default_notifications;

    // Current number of default notifications that have been registered
    size_t num_default_notifications;
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

/*********************/
/* DPU CONFIGURATION */
/*********************/

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
 * representing the list of known DPU, based on a execution context. This is relevant mainly on DPUs
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

#define ECONTEXT_FOR_DPU_COMMUNICATION(_engine, _dpu_idx) ({    \
    execution_context_t *_e = NULL;                             \
    remote_dpu_info_t **_list = LIST_DPUS_FROM_ENGINE(_engine); \
    if (_list != NULL && _list[_dpu_idx] != NULL)               \
    {                                                           \
        _e = _list[_dpu_idx]->econtext;                         \
    }                                                           \
    _e;                                                         \
})

typedef enum
{
    CONNECT_STATUS_UNKNOWN = 0,
    CONNECT_STATUS_CONNECTED,
    CONNECT_STATUS_DISCONNECTED
} connect_status_t;

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
} remote_dpu_info_t;

#define RESET_REMOTE_DPU_INFO(_info)                        \
    do                                                      \
    {                                                       \
        (_info)->idx = 0;                                   \
        (_info)->hostname = NULL;                           \
        (_info)->peer_addr = NULL;                          \
        RESET_INIT_PARAMS(&((_info)->init_params));         \
        (_info)->conn_status = CONNECT_STATUS_DISCONNECTED; \
        (_info)->offload_engine = NULL;                     \
        (_info)->econtext = NULL;                           \
        (_info)->ucp_worker = NULL;                         \
        (_info)->ep = NULL;                                 \
    } while (0)

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
        // id is the unique identifier This will be used to set the context ID for the server on the DPU
        uint64_t id;
        dpu_config_data_t *config;
        char hostname[1024];
        conn_params_t interdpu_conn_params;
        conn_params_t host_conn_params;
        init_params_t interdpu_init_params; // Parameters used to connect to other DPUs, or other DPUS connect to the DPU
        init_params_t host_init_params;     // Parameters used to let the host connect to the DPU
    } local_dpu;
} offloading_config_t;

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
        RESET_INIT_PARAMS(&((_data)->local_dpu.interdpu_init_params));                                    \
        RESET_INIT_PARAMS(&((_data)->local_dpu.host_init_params));                                        \
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

/**********************/
/* ACTIVE MESSAGE IDS */
/**********************/

typedef enum
{
    AM_TERM_MSG_ID = 33, // 33 to make it easier to see corruptions (dbg)
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