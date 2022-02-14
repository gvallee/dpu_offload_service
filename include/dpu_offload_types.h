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

#include "dynamic_list.h"
#include "dpu_offload_common.h"
#include "dpu_offload_utils.h"

_EXTERN_C_BEGIN

typedef enum
{
    CONTEXT_CLIENT = 0,
    CONTEXT_SERVER
} daemon_type_t;

#define MY_ID(_exec_ctx) ({                \
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
    if (_exec_ctx->type == CONTEXT_CLIENT)                                 \
    {                                                                      \
        _ep = _exec_ctx->server->connected_clients.clients[_client_id].ep; \
    }                                                                      \
    else                                                                   \
    {                                                                      \
        _ep = NULL;                                                        \
    }                                                                      \
    _ep;                                                                   \
})

#define GET_WORKER(_exec_ctx) ({            \
    ucp_worker_h _w;                        \
    if (_exec_ctx->type == CONTEXT_CLIENT)  \
    {                                       \
        _w = _exec_ctx->client->ucp_worker; \
    }                                       \
    else                                    \
    {                                       \
        _w = _exec_ctx->server->ucp_worker; \
    }                                       \
    _w;                                     \
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

/* NOTIFICATIONS */

struct dpu_offload_ev_sys;
typedef int (*notification_cb)(struct dpu_offload_ev_sys *ev_sys, void *context, void *data);

typedef struct notification_callback_entry
{
    bool set;
    struct dpu_offload_ev_sys *ev_sys;
    notification_cb cb;
} notification_callback_entry_t;

typedef struct pending_notification
{
    ucs_list_link_t item;
    uint64_t type;
    uint64_t client_id;
    void *header;
    size_t header_size;
    void *data;
    size_t data_size;
    void *arg;
} pending_notification_t;

typedef struct dpu_offload_ev_sys
{
    dyn_list_t *free_evs;
    size_t num_used_evs;
    uint64_t num_notification_callbacks;
    ucs_list_link_t pending_notifications;
    dyn_list_t *free_pending_notifications;

    // Array of callback functions, i.e., array of pointers
    notification_callback_entry_t *notification_callbacks;
} dpu_offload_ev_sys_t;

/* OFFLOADING ENGINE, CLIENTS/SERVERS */

typedef struct ucx_server_ctx
{
    volatile ucp_conn_request_h conn_request;
    ucp_listener_h listener;
} ucx_server_ctx_t;

typedef struct connected_client
{
    ucp_ep_h ep;
    ucs_status_t ep_status;
} connected_client_t;

typedef struct connected_clients
{
    dpu_offload_ev_sys_t *event_channels;
    size_t num_max_connected_clients;
    size_t num_connected_clients;
    connected_client_t *clients;
} connected_clients_t;

typedef struct conn_params
{
    char *addr_str;
    char *port_str;
    int port;
    struct sockaddr_storage saddr;
} conn_params_t;

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
    union
    {
        dpu_offload_client_t *client;
        dpu_offload_server_t *server;
    };
} execution_context_t;

typedef struct am_header
{
    // For clients, id assigned by server during connection,
    // used for triage when server has multiple clients
    uint64_t id;

    // Type associated to the payload, e.g., notification type.
    uint64_t type;
} am_header_t;

typedef struct am_req
{
    am_header_t hdr;
    int complete;
} am_req_t;

typedef struct dpu_offload_event
{
    ucs_list_link_t item;
    uint64_t id;
    am_req_t ctx;
    void *req;
    void *context;
    void *data;
} dpu_offload_event_t;

typedef enum
{
    EVENT_DONE = UCS_OK,
    EVENT_INPROGRESS = UCS_INPROGRESS
} event_state_t;

/* OFFLOADING ENGINE */

typedef struct offloading_engine
{
    int done;

    /* client here is used to track the bootstrapping as a client. */
    /* it can only be at most one (the offload engine bootstraps only once */
    /* for both host process and the DPU daemon) */
    dpu_offload_client_t *client;

    /* we can have as many servers as we want, each server having multiple clients */
    size_t num_max_servers;
    size_t num_servers;
    dpu_offload_server_t **servers;

    /* Vector of registered operation, ready for execution */
    size_t num_registered_ops;
    offload_op_t *registered_ops;
    dyn_list_t *free_op_descs;
} offloading_engine_t;

typedef enum
{
    AM_TERM_MSG_ID = 33, // 33 to make it easier to see corruptions (dbg)
    AM_EVENT_MSG_ID,
    AM_OP_START_MSG_ID,
    AM_OP_COMPLETION_MSG_ID,
    AM_XGVMI_ADD_MSG_ID,
    AM_XGVMI_DEL_MSG_ID,
    AM_TEST_MSG_ID
} am_id_t;

_EXTERN_C_END

#endif // DPU_OFFLOAD_TYPES_H